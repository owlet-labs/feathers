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

#include "fx/phaser/phaser.h"

#include "device/format_value.h"
#include "dsp/fast_math_approx.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <new>
#include <numbers>
#include <string>

namespace feathers::fx::phaser {

namespace {

float coerce(const device::ParamDesc& desc, float value) {
	if (!std::isfinite(value)) {
		return static_cast<float>(desc.def);
	}
	const float clamped = device::coerceControl(desc, value);
	return desc.stepped ? static_cast<float>(std::lround(clamped)) : clamped;
}

/// A stored Choice back to its index, inside the list it indexes.
uint32_t choiceIndex(float value, size_t count) {
	return static_cast<uint32_t>(std::clamp<long>(std::lround(value), 0, static_cast<long>(count) - 1));
}

/// THE STAGE COUNT BACK TO ITS CHOICE INDEX. A count not in the list (a `Settings` built by hand) lands
/// on the nearest one below it, never past the end.
size_t stagesIndexOf(int stages) {
	size_t index = 0;
	for (size_t i = 0; i < std::size(kStageCounts); ++i) {
		if (kStageCounts[i] <= stages) {
			index = i;
		}
	}
	return index;
}

} // namespace

//--------------------------------------------------------------------------------------------------//
// THE CODEC.                                                                                         //
//--------------------------------------------------------------------------------------------------//

Row rowOf(const State& state) {
	Row row {};
	const Settings& s = state.settings;
	const auto put = [&row](Param p, float v) { row[static_cast<size_t>(p)] = v; };
	put(Param::Enabled, s.enabled ? 1.0f : 0.0f);
	put(Param::Mode, static_cast<float>(s.mode));
	put(Param::Stages, static_cast<float>(stagesIndexOf(s.stages)));
	put(Param::CenterHz, s.centerLog2Hz);
	put(Param::DelayMs, s.delayLog2s);
	put(Param::Feedback, s.feedback);
	put(Param::Damping, s.dampingLog2Hz);
	put(Param::Drive, s.drive);
	put(Param::Mix, s.mix);
	put(Param::Position, s.position);
	put(Param::LfoOn, s.lfoOn ? 1.0f : 0.0f);
	put(Param::LfoDepth, s.lfoDepth);
	put(Param::Sync, static_cast<float>(s.sync));
	put(Param::RateHz, s.rateLog2Hz);
	put(Param::Stereo, s.stereoDegrees);
	put(Param::EnvOn, s.envOn ? 1.0f : 0.0f);
	put(Param::EnvAmount, s.envAmount);
	put(Param::EnvAttack, s.envAttackLog2s);
	put(Param::EnvRelease, s.envReleaseLog2s);
	put(Param::EnvSens, s.envSensDb);
	put(Param::Sweep, static_cast<float>(s.sweep));
	put(Param::ThroughZero, s.throughZero ? 1.0f : 0.0f);
	put(Param::KeyTrack, s.keyTrack ? 1.0f : 0.0f);
	put(Param::Rotate, s.rotate);
	for (size_t g = 0; g < phase_space::kGroupCount; ++g) {
		const auto group = static_cast<phase_space::Group>(g);
		put(pillKnobParam(group), s.pillKnob[g]);
		put(pillPowerParam(group), s.pillOn[g] ? 1.0f : 0.0f);
	}
	return row;
}

State stateOf(const Row& row, size_t fieldsPresent) {
	State out {};
	const auto read = [&](Param p) {
		const auto i = static_cast<size_t>(p);
		const device::ParamDesc& d = kParamDescs[i];
		return i < fieldsPresent ? coerce(d, row[i]) : static_cast<float>(d.def);
	};
	const auto on = [&](Param p) { return read(p) >= 0.5f; };
	Settings& s = out.settings;
	s.enabled = on(Param::Enabled);
	s.mode = static_cast<Mode>(choiceIndex(read(Param::Mode), std::size(kModeNames)));
	s.stages = kStageCounts[choiceIndex(read(Param::Stages), std::size(kStageCounts))];
	s.centerLog2Hz = read(Param::CenterHz);
	s.delayLog2s = read(Param::DelayMs);
	s.feedback = read(Param::Feedback);
	s.dampingLog2Hz = read(Param::Damping);
	s.drive = read(Param::Drive);
	s.mix = read(Param::Mix);
	s.position = read(Param::Position);
	s.lfoOn = on(Param::LfoOn);
	s.lfoDepth = read(Param::LfoDepth);
	s.sync = choiceIndex(read(Param::Sync), kSyncChoiceCount);
	s.rateLog2Hz = read(Param::RateHz);
	s.stereoDegrees = read(Param::Stereo);
	s.envOn = on(Param::EnvOn);
	s.envAmount = read(Param::EnvAmount);
	s.envAttackLog2s = read(Param::EnvAttack);
	s.envReleaseLog2s = read(Param::EnvRelease);
	s.envSensDb = read(Param::EnvSens);
	s.sweep = static_cast<SweepShape>(choiceIndex(read(Param::Sweep), std::size(kSweepNames)));
	s.throughZero = on(Param::ThroughZero);
	s.keyTrack = on(Param::KeyTrack);
	s.rotate = read(Param::Rotate);
	for (size_t g = 0; g < phase_space::kGroupCount; ++g) {
		const auto group = static_cast<phase_space::Group>(g);
		s.pillKnob[g] = read(pillKnobParam(group));
		s.pillOn[g] = on(pillPowerParam(group));
	}
	return out;
}

float valueOf(const State& state, Param param) {
	const auto index = static_cast<size_t>(param);
	if (index >= kParamCount) {
		return 0.0f;
	}
	return rowOf(state)[index];
}

void setValueOf(State* state, Param param, float value) {
	const auto index = static_cast<size_t>(param);
	if (state == nullptr || index >= kParamCount || !std::isfinite(value)) {
		return;
	}
	// THE TRAVEL IS NOT IN THE ROW, so it is carried across the re-decode by hand.
	const auto travel = state->settings.travel;
	Row row = rowOf(*state);
	row[index] = coerce(kParamDescs[index], value);
	*state = stateOf(row, kParamCount);
	state->settings.travel = travel;
}

//--------------------------------------------------------------------------------------------------//
// THE READOUTS.                                                                                      //
//--------------------------------------------------------------------------------------------------//

namespace {

bool formatText(size_t control, double value, const device::TextView&, char* out, size_t size) {
	if (out == nullptr || size == 0) {
		return false;
	}
	switch (static_cast<Param>(control)) {
	case Param::DelayMs:
	case Param::EnvAttack:
	case Param::EnvRelease: {
		// A TIME ALL THE WAY DOWN: the LogSeconds convention would print the bottom "off", and none of
		// these skips its stage there (see `kDelayLog2Min`).
		// NO HEAP: the longest reading (a few characters: "0.2ms", "841ms", "2s") sits well inside the string's small buffer, so
		// TextOps' "allocates nothing" holds.
		const std::string text = device::printSeconds(std::exp2(value));
		std::snprintf(out, size, "%s", text.c_str());
		return true;
	}
	case Param::Damping:
		if (dampingOpen(value)) {
			// THE ENGINE BYPASSES THE LOWPASS HERE, so a frequency would describe a filter not running.
			std::snprintf(out, size, "open");
			return true;
		}
		return false;
	case Param::RateHz: {
		// AN LFO'S RATE IS MOSTLY UNDER ONE HERTZ, where the unit's whole-hertz "%.0f" printed the default 0.3 Hz as
		// "0" (found by the card's LFO caption, Task 9b). Two decimals under 1 Hz, one under 10, whole above.
		const double hz = std::exp2(value);
		std::snprintf(out, size, hz < 1.0 ? "%.2f Hz" : (hz < 10.0 ? "%.1f Hz" : "%.0f Hz"), hz);
		return true;
	}
	default:
		return false;
	}
}

} // namespace

const device::TextOps kTextOps { .format = &formatText };

//--------------------------------------------------------------------------------------------------//
// THE ENGINE.                                                                                        //
//--------------------------------------------------------------------------------------------------//

namespace {

/// Exact zero below `kSnapLevel` — see its comment.
inline float snap(float v) noexcept { return std::fabs(v) < kSnapLevel ? 0.0f : v; }

/// eq.cpp's input guard: a non-finite sample from upstream is read as silence.
inline float sane(float x) noexcept { return std::isfinite(x) ? x : 0.0f; }

/// One control tick's one-pole coefficient for a time constant.
double alphaFor(double seconds, double sampleRate) {
	return 1.0 - std::exp(-static_cast<double>(kControlTick) / (seconds * sampleRate));
}

/// **THE FLANGER'S OWN KERNEL**, at B = 1 (`kFlangerBandwidth`). Namespace scope for the reason
/// `filter::kSincTable` is: built during the library's initialisation, never inside a render call.
const dsp::SincTable kFlangerSincTable { kFlangerBandwidth };

} // namespace

Phaser::FlangerChannel::FlangerChannel() : wet(kFlangerSincTable) {}

void Phaser::prepare(double sampleRate) noexcept {
	sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
	smoothAlpha_ = alphaFor(kSmoothSeconds, sampleRate_);
	positionAlpha_ = alphaFor(kPositionSmoothSeconds, sampleRate_);
	keyAlpha_ = alphaFor(kKeySmoothSeconds, sampleRate_);
	dcG_ = loopDcGainOf(sampleRate_);
	sourceAlpha_ = alphaFor(kSourceSmoothSeconds, sampleRate_);
	reconvergeAlpha_ = alphaFor(kReconvergeSeconds, sampleRate_);
	tzDry_ = tzDrySamples(sampleRate_);
	reset();
}

void Phaser::reset() noexcept {
	// THE NOTE IS FORGOTTEN HERE AND ONLY HERE — "as if just switched on" means no note yet, so Key Track falls back
	// to the knobs until one arrives. Coming back from the enable switch (`resetState(true)`) keeps it: the host's
	// last note is no less the last note because the device was bypassed.
	heardNote_ = false;
	noteSemitones_ = 60.0;
	resetState(false);
}

void Phaser::setNoteContext(const device::NoteContext& note) noexcept {
	// LATCHED ONLY WHILE A NOTE IS HELD, so the centre HOLDS the last note's after the release — the host already
	// holds its pitch then (synth.cpp), and this does not lean on it. Glide and bend arrive folded into the pitch
	// while the key is down, and are followed through the centre's own glide. Only latched: the centre is computed
	// at the next tick from this and the row together, so fields and notes may arrive in either order.
	//
	// **FROZEN AT THE RELEASE, BY SPEC — AND THAT IS A DIFFERENCE FROM THE DISPERSER, DECIDED.** The disperser
	// (`disperser.cpp`'s `setNoteContext`) keeps following the pitch after the release once a note has been heard, so
	// a bend in a release tail moves its centre. This device does not: by design the centre holds the
	// last note's value after release (no snap back), so a per-voice pitch bend arriving during a release tail is
	// IGNORED here. It is a choice, not an omission — a tail that keeps re-tuning a resonant comb after the key is up
	// is a sound the spec did not ask for.
	if (!note.gate || !std::isfinite(note.pitchSemitones)) {
		return;
	}
	heardNote_ = true;
	noteSemitones_ = static_cast<double>(note.pitchSemitones);
}

void Phaser::resetState(bool keepDry) noexcept {
	untilTick_ = 0;
	placed_ = false;
	wasEnabled_ = true;
	mode_ = target_.mode;
	throughZero_ = target_.mode == Mode::Flanger && target_.throughZero; // as `targetsOf` gives it, which `tick` reads
	activeStages_ = 0;
	loopFb_ = {};
	drive_ = {};
	mix_ = {};
	dampOpen_ = true;
	dampG_ = 1.0f;
	dampHz_ = 0.0;
	cascades_ = {};
	gain_ = {};
	voiceTick_ = {};
	voicesRunning_ = 1;
	barber_ = false;
	barberPrev_ = false;
	psi_ = {};
	barberRun_ = {};
	barberStep_ = {};
	barberRot_ = {};
	barberRotStep_.fill({ 1.0, 0.0 });
	barberDir_ = 1.0;
	clearFlanger();
	// THE SOURCES START OVER: the LFO at phase 0 (a lock re-anchors on its next tick), the follower empty. The
	// clock and the readings sink are the HOST's, and stay.
	for (Clock& c : lfoClock_) {
		c.phase = 0.0;
		c.inc = 0.0;
		c.lock.release();
	}
	for (Clock& c : barberClock_) {
		c.phase = 0.0;
		c.inc = 0.0;
		c.lock.release();
	}
	rateScaleR_ = 1.0;
	offsetR_ = {};
	convergedR_ = { true, true };
	lfoPhaseUsed_ = {};
	lfoRot_ = {};
	lfoStep_.fill({ 1.0, 0.0 });
	envPeak_ = 0.0f;
	envLevel_ = 0.0;
	envUsed_ = 0.0;
	sweepBase_ = {};
	lfoAmountRamp_ = {};
	delayLog2Ramp_ = {};
	sweep_ = {};
	cornerHz_ = {};
	delayTarget_ = {};
	// THE EXPLORE LAYER'S GLIDE is placed on the first tick, as every other glide is.
	explore_ = {};
	exploreWant_ = {};
	fieldsSeen_ = phase_space::kHomeField;
	travelSeen_ = {};
	pillsSeen_ = {};
	curveExp_ = 1.0;
	exploreSettled_ = false;
	exploreAtHome_ = false;
	driveBias_ = 0.0f;
	biasTanh_ = 0.0f;
	dcOn_ = false;
	squareNorm_ = 1.0;
	tapLog2Ramp_ = {};
	tapPhaseRamp_ = {};
	lfoRun_ = {};
	flangerOffsets_ = false;
	flangerRun_ = false;
	if (!keepDry) {
		for (FlangerChannel& f : flangers_) {
			f.dry.fill(0.0f);
			f.dryPos = 0;
			f.late.fill(0.0f);
			f.latePos = 0;
		}
	}
}

void Phaser::clearPhaser() noexcept {
	for (size_t v = 0; v < cascades_.size(); ++v) {
		clearVoice(v);
	}
}

void Phaser::clearVoice(size_t voice) noexcept {
	for (Channel& c : cascades_[voice]) {
		c.s = {};
		c.prevWet = 0.0f;
		c.damp = 0.0f;
		c.dc = 0.0f;
	}
}

void Phaser::clearFlanger() noexcept {
	for (FlangerChannel& f : flangers_) {
		// NOT THE DRY RING: it is written on every road, on or off, either mode, so it is always current.
		f.wet.clear();
		f.delay = 0.0f;
		f.damp = 0.0f;
		f.dc = 0.0f;
		f.lastWet = 0.0f;
		f.tapDelay = {};
		f.tapGain = {};
		f.fadeDelay = 0.0f;
		f.fadeTapDelay = {};
		f.fadeTapGain = {};
		// THE PLAIN FLANGER'S LATE RING HOLDS ITS OWN PAST OUTPUT, which must not replay either — it starts over as the
		// DRY, late: what a latent device that has just been cleared plays (and what the off road played, so coming
		// back on is seamless).
		f.late = f.dry;
		f.latePos = f.dryPos;
	}
	// A FADE IN FLIGHT FADES FROM A READ OF THE LINE JUST CLEARED: nothing to fade from.
	fadeLeft_ = 0;
	fadeFromBarber_ = false;
}

double Phaser::lfoPhaseFor(int offset, Clock& c, double rateScale) noexcept {
	const Settings& t = target_;
	const double tickSeconds = static_cast<double>(kControlTick) / sampleRate_;
	const bool synced = isSynced(t.sync);
	const double beats = syncDivisionBeats(t.sync);
	// THE RATE IT IS ACTUALLY RUNNING AT. A division with NO TEMPO runs at one second a beat, the delay's fallback
	// (`delay.cpp`'s `tapSeconds`) — 60 BPM, a sweep a hand recognises as "no tempo" rather than a stopped one.
	// The control still reads its division, which is what it will do the moment a tempo arrives.
	// THE CHANNEL'S RATE SCALE (round 2's rate skew: 1 for L, 2^skew for R) multiplies the rate and divides a synced
	// division's period, so R's lock follows the song at R's own rate. At 1 both are exact: x·1 and x/1 are x.
	const double cyclesPerSecond =
	    (synced ? 1.0 / (beats * (clock_.haveTempo() ? clock_.secondsPerBeat : 1.0)) : std::exp2(rateLog2_)) * rateScale;
	const double periodBeats = beats / rateScale;
	// **ON A PLAYING TRANSPORT THE PHASE FOLLOWS THE SONG** — automod's `songPeriod`, road for road: a synced
	// division with a tempo and a beat position on the BEATS timeline (its period is N beats, so it stays on the
	// grid through tempo automation); anything else — Free, or a division with no tempo, at the rate it is really
	// running — on the SECONDS timeline, a window of one cycle. A stopped transport is not a position (it would
	// freeze the sweep), and a host that says nothing leaves the accumulator running. The clock's position is the
	// block's first sample's, so this tick's is `offset` samples on.
	const double offsetSeconds = static_cast<double>(offset) / sampleRate_;
	bool locked = false;
	double turns = 0.0;
	if (clock_.playing && synced && clock_.haveTempo() && clock_.havePositionBeats()) {
		const double spb = clock_.secondsPerBeat;
		turns = c.lock.follow(clock_.songPositionBeats + offsetSeconds / spb, periodBeats, tickSeconds / spb,
		                      kBeatsRoad);
		locked = true;
	}
	else if (clock_.playing && clock_.havePositionSeconds() && cyclesPerSecond > 0.0
	         && std::isfinite(cyclesPerSecond)) {
		turns = c.lock.follow(clock_.songPositionSeconds + offsetSeconds, 1.0 / cyclesPerSecond, tickSeconds,
		                     kSecondsRoad);
		locked = true;
	}
	else {
		c.lock.release();
	}
	if (locked) {
		c.phase = turns - std::floor(turns);
		// A TRANSPORT THAT HANDS OVER NONSENSE (a tempo of 1e-300 s a beat puts an infinity in the position) is not
		// followed: the phase starts over and the lock re-anchors on the next sane tick. And `turns − floor` can
		// round to exactly 1 just below an integer (automod_lfo.h's `lockPhaseTo` has the same guard).
		if (!std::isfinite(c.phase) || c.phase >= 1.0) {
			c.phase = 0.0;
			c.lock.release();
		}
	}
	const double phase = c.phase;
	const double inc = cyclesPerSecond / sampleRate_;
	c.inc = std::isfinite(inc) ? inc : 0.0;
	const double next = c.phase + c.inc * static_cast<double>(kControlTick);
	c.phase = std::isfinite(next) ? next - std::floor(next) : 0.0;
	return phase;
}

double Phaser::lfoCyclesPerSecond() const noexcept {
	const Settings& t = target_;
	const bool synced = isSynced(t.sync);
	return synced ? 1.0 / (syncDivisionBeats(t.sync) * (clock_.haveTempo() ? clock_.secondsPerBeat : 1.0))
	              : std::exp2(rateLog2_);
}

double Phaser::barberPhaseFor(int offset, double depth, Clock& c, double rateScale) noexcept {
	// `lfoPhaseFor`'s ROADS, ROAD FOR ROAD, at ψ's rate: the LFO's scaled by Depth (`barberCyclesPerSecond`). The
	// windows are the LFO's scaled the same way — a division of N beats becomes N·(lfo / ψ) beats — so a synced
	// division is one crossing per division at Depth 0.5, and a Depth that moves frequency-modulates ρ through the
	// lock (`TransportLock`: by the song's movement, at the window as it is now), never re-places it.
	const Settings& t = target_;
	const double tickSeconds = static_cast<double>(kControlTick) / sampleRate_;
	const bool synced = isSynced(t.sync);
	const double beats = syncDivisionBeats(t.sync);
	// THE CHANNEL'S LFO RATE (R's skewed by its scale — exact at 1), and ψ's from it.
	const double lfoCps = lfoCyclesPerSecond() * rateScale;
	const double cps = barberCyclesPerSecond(lfoCps, depth);
	const double offsetSeconds = static_cast<double>(offset) / sampleRate_;
	bool locked = false;
	double turns = 0.0;
	if (cps > 0.0 && clock_.playing && synced && clock_.haveTempo() && clock_.havePositionBeats()) {
		const double spb = clock_.secondsPerBeat;
		turns = c.lock.follow(clock_.songPositionBeats + offsetSeconds / spb, beats / rateScale * (lfoCps / cps),
		                      tickSeconds / spb, kBeatsRoad);
		locked = true;
	}
	else if (cps > 0.0 && clock_.playing && clock_.havePositionSeconds()) {
		turns = c.lock.follow(clock_.songPositionSeconds + offsetSeconds, 1.0 / cps, tickSeconds, kSecondsRoad);
		locked = true;
	}
	else {
		c.lock.release();
	}
	if (locked) {
		c.phase = turns - std::floor(turns);
		if (!std::isfinite(c.phase) || c.phase >= 1.0) {
			c.phase = 0.0;
			c.lock.release();
		}
	}
	const double phase = c.phase;
	const double inc = cps / sampleRate_;
	c.inc = std::isfinite(inc) ? inc : 0.0;
	const double next = c.phase + c.inc * static_cast<double>(kControlTick);
	c.phase = std::isfinite(next) ? next - std::floor(next) : 0.0;
	return phase;
}

double Phaser::phaseRFor(size_t which, double rawR, double rawL) noexcept {
	// THE SHORTEST SIGNED DISTANCE ROUND THE CYCLE, in [−0.5, 0.5).
	const auto around = [](double d) { return d - std::floor(d + 0.5); };
	const auto wrapped = [](double p) {
		const double f = p - std::floor(p);
		return f < 1.0 && f >= 0.0 ? f : 0.0;
	};
	double& off = offsetR_[which];
	if (rateScaleR_ != 1.0) {
		// THE SKEW IS LIVE: R runs on its own clock, the offset frozen where it stood, so R is continuous.
		convergedR_[which] = false;
		return off == 0.0 ? rawR : wrapped(rawR + off);
	}
	if (convergedR_[which]) {
		return rawR; // L's clock, copied: rawR is rawL to the bit
	}
	// SKEW AT 0 (zero travel with the pill at 50% or off), NOT YET ON L: glide the offset toward the one that puts R on L (one-pole, `kReconvergeSeconds`).
	const double target = around(rawL - rawR);
	off += around(target - off) * reconvergeAlpha_;
	if (std::fabs(around(target - off)) < kGlideSnap) {
		// ON L: R's clock becomes L's (phase, rate and song lock), the offset goes, and the two are one again.
		Clock& r = which == 0 ? lfoClock_[1] : barberClock_[1];
		const Clock& l = which == 0 ? lfoClock_[0] : barberClock_[0];
		r = l;
		off = 0.0;
		convergedR_[which] = true;
		return rawL;
	}
	return wrapped(rawR + off);
}

void Phaser::followerStep() noexcept {
	// A PEAK DETECTOR: the tick's loudest input sample, times Sens, steps a one-pole — Attack's rate rising,
	// Release's falling. One step a tick, so a level held for k ticks closes (1 − a)^k = e^(−t/τ) of the gap.
	const double peak = static_cast<double>(envPeak_) * envGain_;
	envPeak_ = 0.0f;
	const double a = peak > envLevel_ ? envAttackA_ : envReleaseA_;
	envLevel_ += (peak - envLevel_) * a;
	if (!(envLevel_ >= static_cast<double>(kSnapLevel))) {
		envLevel_ = 0.0; // exact zero below the snap — a release reaches it rather than parking on a denormal
	}
	if (!std::isfinite(envLevel_)) {
		envLevel_ = 0.0;
	}
}

void Phaser::tick(int offset) noexcept {
	const Settings& t = target_;
	const bool snapNow = !placed_;
	// A MODE SWITCH CLEARS THE ENGINE BEING ENTERED, so nothing it held when it was left replays. (Mode is
	// not modulatable, so this is a click at an edit, never a stream of them.)
	if (t.mode != mode_) {
		if (t.mode == Mode::Phaser) {
			clearPhaser();
		}
		else {
			clearFlanger();
		}
		mode_ = t.mode;
	}
	// THROUGH-ZERO MOVES THE READ BY 8 ms: jumped — the per-sample delay reads the new map from the next sample,
	// with no scrub across it (a 384-sample scrub is a worse noise than the step). Not modulatable either — it
	// moves the latency.
	// KEY TRACKING: with a note since the reset, the note sets the centre and the Center and Delay knobs become the
	// offset from it (`centreTargetLog2`, through `targetsOf`) — the phaser's so its first notch is on the note moved by
	// that offset, the flanger's so its comb is tuned there — and
	// the sweep moves around it as it moves around a knob's. Through the centre's glide, so a new note (or a glide the
	// host folded into the pitch) slides the centre rather than stepping it — but a 5 ms one while a note sets it
	// (`kKeySmoothSeconds`: a trill must land on its notes), the knobs' 20 ms otherwise. No note yet: the knobs, as if
	// it were off.
	noteHz_ = t.keyTrack && heardNote_ ? keyHzOf(noteSemitones_) : 0.0;
	// EVERY GLIDE'S TARGET, by the statement the response model reads too (`targetsOf`).
	const Targets want = targetsOf(t, noteHz_, sampleRate_);
	throughZero_ = want.throughZero;
	// A STAGE JOINING THE CASCADE JOINS EMPTY — it has been out of the signal since it last left.
	const int stages = want.stages;
	for (int i = activeStages_; i < stages; ++i) {
		for (auto& voice : cascades_) {
			for (Channel& c : voice) {
				c.s[static_cast<size_t>(i)] = 0.0f;
			}
		}
	}
	activeStages_ = stages;

	// THE SMOOTHERS: placed on the first tick, then one-pole toward the target, landing ON it once within
	// `kGlideSnap` (a one-pole only approaches).
	const auto glide = [&](double& now, double to, double alpha) {
		now = snapNow ? to : now + (to - now) * alpha;
		if (std::fabs(to - now) < kGlideSnap) {
			now = to;
		}
	};
	const double centreAlpha = want.tracking ? keyAlpha_ : smoothAlpha_;
	glide(centerLog2_, want.centerLog2, centreAlpha);
	glide(delayLog2_, want.delayLog2, centreAlpha);
	glide(position_, want.position, positionAlpha_);
	glide(feedbackRaw_, want.feedback, smoothAlpha_);
	glide(driveRaw_, want.drive, smoothAlpha_);
	glide(mixRaw_, want.mix, smoothAlpha_);
	glide(dampLog2_, want.dampingLog2, smoothAlpha_);
	glide(rateLog2_, want.rateLog2, smoothAlpha_);
	glide(stereo_, want.stereoDegrees, smoothAlpha_);
	// A SOURCE SWITCHED OFF GLIDES ITS AMOUNT TO ZERO, and lands there exactly (`kGlideSnap`), so off is
	// `(LfoOn ? LfoDepth·lfo : 0)` at rest and a fade, not a step, on the way.
	glide(lfoAmount_, want.lfoAmount, sourceAlpha_);
	glide(envAmount_, want.envAmount, sourceAlpha_);
	glide(envSensDb_, want.envSensDb, smoothAlpha_);
	glide(envAttackLog2_, want.envAttackLog2, smoothAlpha_);
	glide(envReleaseLog2_, want.envReleaseLog2, smoothAlpha_);
	// THE FOLLOWER'S COEFFICIENTS for the step at this tick's end: a one-pole's per-tick 1 − e^(−T/τ).
	envGain_ = std::pow(10.0, envSensDb_ / 20.0);
	envAttackA_ = alphaFor(std::exp2(envAttackLog2_), sampleRate_);
	envReleaseA_ = alphaFor(std::exp2(envReleaseLog2_), sampleRate_);

	// **THE EXPLORE LAYER** (`phaser_phase_space.h`): the thirty-four offsets the pad's travel derives, each through the
	// knobs' 20 ms glide so a drag slides the character rather than stepping it. EXACTLY ZERO AT HOME WITH THE PILLS AT
	// 0.5 — the derive's `tri(home + 0.0) − tri(home)` is 0.0, and a glide from 0 to 0 stays 0 — so every road below
	// that adds one of these adds +0.0, which is exact, and the engine is the plain one bit for bit. Rotate is NOT read
	// here: it aims the pad's next gesture, never the sound standing still (the shaper's fact, and this device's
	// dead-list entry).
	// THE DERIVE RUNS ONLY WHEN THE TRAVEL CHANGES, and the glides only until they land: sixty triangle reads and thirty
	// glides a tick MEASURED ~20 ns a sample at home (`bench_phaser`), for values that sit still between drags.
	// THE PILLS STAND IN FRONT OF THE FIELDS (`phase_space::offsetOf`, the house `fieldWithOverride`): the travel's
	// thirty-four fields are kept, so a pill's move, or its modulation, re-derives from them (thirty-four overrides, no
	// triangle read), and only a travel change runs the triangle reads again. At every knob 0.5 (or off) zero travel is
	// still exactly zero; a knob away from 0.5 moves the sound at home too — that is the law (0 and 1 force the field).
	const phase_space::Pills pills = pillsOf(t);
	const bool travelMoved = snapNow || !(t.travel == travelSeen_);
	if (travelMoved) {
		travelSeen_ = t.travel;
		fieldsSeen_ = phase_space::fieldsOf(t.travel);
	}
	if (travelMoved || !(pills == pillsSeen_)) {
		pillsSeen_ = pills;
		exploreWant_ = phase_space::exploreOfFields(fieldsSeen_, pills);
		exploreSettled_ = false;
		exploreAtHome_ = false;
	}
	if (!exploreSettled_) {
		const phase_space::Explore& want = exploreWant_;
		for (size_t i = 0; i < phase_space::kSpreadCount; ++i) {
			glide(explore_.spreadOctaves[i], want.spreadOctaves[i], smoothAlpha_);
		}
		for (size_t i = 0; i < phase_space::kLfoPhaseCount; ++i) {
			glide(explore_.lfoPhase[i], want.lfoPhase[i], smoothAlpha_);
		}
		glide(explore_.bend.skew, want.bend.skew, smoothAlpha_);
		glide(explore_.bend.square, want.bend.square, smoothAlpha_);
		glide(explore_.bend.triangle, want.bend.triangle, smoothAlpha_);
		glide(explore_.dampingOctaves, want.dampingOctaves, smoothAlpha_);
		glide(explore_.driveBias, want.driveBias, smoothAlpha_);
		glide(explore_.stereoOctaves, want.stereoOctaves, smoothAlpha_);
		glide(explore_.rateSkewOctaves, want.rateSkewOctaves, smoothAlpha_);
		glide(explore_.feedbackTilt, want.feedbackTilt, smoothAlpha_);
		glide(explore_.pairingOctaves, want.pairingOctaves, smoothAlpha_);
		glide(explore_.sweepCurve, want.sweepCurve, smoothAlpha_);
		driveBias_ = static_cast<float>(explore_.driveBias);
		squareNorm_ = squareNormOf(explore_.bend);
		// R's RATE AGAINST L's (index 30) and the sweep curve's exponent (index 33) — exactly 1 at home.
		rateScaleR_ = explore_.rateSkewOctaves == 0.0 ? 1.0 : std::exp2(explore_.rateSkewOctaves);
		curveExp_ = sweepCurveExponentOf(explore_.sweepCurve);
		exploreSettled_ = explore_ == want;
		exploreAtHome_ = exploreSettled_ && explore_ == phase_space::Explore {};
	}
	const phase_space::LfoBend& bend = explore_.bend;

	// THE RAMPS, aimed at this tick's values. Drive is aimed first: the feedback's ceiling reads it.
	drive_.aim(loopDriveOf(driveRaw_), snapNow);
	// THE FEEDBACK, A RAMP A LOOP: the stored value plus the explore layer's tilt (index 31) for that loop, THEN the
	// clamp and the ceiling (`loopFeedbackOf`). The phaser's barberpole steps it across its voices; everywhere else the
	// channels split it (`feedbackTiltOf`). At home the tilt is ±0.0 and every loop is aimed at the plain value.
	{
		const bool perVoice = mode_ == Mode::Phaser && isBarberpole(t.sweep);
		for (size_t v = 0; v < loopFb_.size(); ++v) {
			for (size_t ch = 0; ch < 2; ++ch) {
				const double tilt
				    = feedbackTiltOf(perVoice, static_cast<int>(v), static_cast<int>(ch), explore_.feedbackTilt);
				loopFb_[v][ch].aim(loopFeedbackOf(feedbackRaw_ + tilt, drive_.end), snapNow);
			}
		}
	}
	// THE SATURATOR'S BIAS, its tanh once a tick from the drive ramp's end (0 exactly with no bias), and the DC blocker
	// that runs with it. ENGAGING, the blocker's state starts empty: it has seen nothing since it last ran.
	biasTanh_ = biasTanhOf(drive_.end, driveBias_);
	const bool dcWasOn = dcOn_;
	// KEYED ON THE EFFECTIVE BIAS, tanh(b·min(1, Drive/0.1)), not the derived one (the Task 11 review): at Drive 0 there
	// is no curve to bias, so the loop is the plain one — no blocker — and the window's "Bias grey at Drive 0" is true.
	dcOn_ = biasTanh_ != 0.0f;
	if (dcOn_ && !dcWasOn) {
		for (auto& voice : cascades_) {
			for (Channel& c : voice) {
				c.dc = 0.0f;
			}
		}
		for (FlangerChannel& f : flangers_) {
			f.dc = 0.0f;
		}
	}
	mix_.aim(loopMixOf(mixRaw_), snapNow);

	// THE DAMPING: out of the loop once the GLIDE has landed on the knob's top (so the lowpass sweeps up to
	// 20 kHz and only then leaves, rather than jumping out from wherever the glide was), otherwise a TPT
	// lowpass at its corner. Re-entering, its state starts empty: it has seen nothing since it left.
	const bool wasOpen = dampOpen_;
	dampOpen_ = dampingOpen(dampLog2_);
	if (!dampOpen_) {
		if (wasOpen) {
			for (auto& voice : cascades_) {
				for (Channel& c : voice) {
					c.damp = 0.0f;
				}
			}
			for (FlangerChannel& f : flangers_) {
				f.damp = 0.0f;
			}
		}
		// THE EXPLORE LAYER'S COLOUR moves a RUNNING lowpass; an open loop stays open (`colouredDampingCornerHz`).
		dampHz_ = colouredDampingCornerHz(dampLog2_, explore_.dampingOctaves, sampleRate_);
		dampG_ = tptGainOf(dampHz_, sampleRate_);
	}

	// THE SWEEP, PER CHANNEL: the glided Position, then the two source terms ADDED AFTER its glide. The right
	// reads ITS OWN clock Stereo/360 of a cycle ahead: at L's rate times 2^(rate skew), index 30 — so at home it is L's
	// clock exactly and the right is the left plus Stereo/360, as it always was.
	const double phaseL = lfoPhaseFor(offset, lfoClock_[0], 1.0);
	const double phaseR = phaseRFor(0, lfoPhaseFor(offset, lfoClock_[1], rateScaleR_), phaseL);
	lfoPhaseUsed_ = { phaseL, channelLfoPhase(phaseR, stereo_, 1) };
	const double env = envelopeOf(envLevel_);
	envUsed_ = env;
	// THE FLANGER'S PER-SAMPLE SWEEP: its ramps land on this tick's values at the tick's LAST sample, as every
	// coefficient ramp here does, and its phasor is seeded so that the same sample reads exactly this tick's phase
	// — 16 samples back, then rotated once before each sample.
	lfoAmountRamp_.aim(static_cast<float>(lfoAmount_), snapNow);
	delayLog2Ramp_.aim(static_cast<float>(delayLog2_), snapNow);
	const double twoPi = 2.0 * std::numbers::pi;

	// THE BARBERPOLE'S VOICES. ENTERING it, voices 1 and 2 have been out of the signal since they last left, so they
	// JOIN EMPTY (a stage joining the cascade does too) and their corners are placed, not ramped from where they
	// stood. LEAVING it, all three run one more tick while their windows ramp to (1, 0, 0); then voice 0 runs alone.
	const bool barber = isBarberpole(t.sweep);
	const bool joining = barber && !barberPrev_;
	if (joining) {
		for (size_t v = 1; v < cascades_.size(); ++v) {
			clearVoice(v);
		}
	}
	voicesRunning_ = barber || barberPrev_ ? kBarberVoices : 1;
	// ψ IS THE BARBERPOLE'S OWN PHASE ρ (Up ρ, Down 1 − ρ), turning at the LFO's rate scaled by Depth so the climb
	// is Depth's to leave alone (`barberCyclesPerSecond`), and song-locked as the LFO is; the right's is Stereo/360
	// on. The window phasor turns the way ψ does. ρ advances every tick and reaches nothing in Normal.
	// THE RIGHT'S ρ ON ITS OWN CLOCK, at its skewed LFO rate (index 30) — L's exactly at home.
	const double rho = barberPhaseFor(offset, lfoAmount_, barberClock_[0], 1.0);
	const double rhoR = phaseRFor(1, barberPhaseFor(offset, lfoAmount_, barberClock_[1], rateScaleR_), rho);
	const double dir = t.sweep == SweepShape::Down ? -1.0 : 1.0;
	// THE FLANGER'S READ MOVES AT A SWEEP SWITCH (Normal ↔ barberpole, or Up ↔ Down): fade it, from the read the
	// last sample ran — so this runs BEFORE anything below re-aims the reads.
	if (placed_ && mode_ == Mode::Flanger && (barber != barberPrev_ || (barber && dir != barberDir_))) {
		beginSweepCrossfade();
	}
	barberPrev_ = barber;
	barber_ = barber;
	barberDir_ = dir;
	for (size_t ch = 0; ch < 2; ++ch) {
		lfoStep_[ch] = std::polar(1.0, twoPi * lfoClock_[ch].inc);
		barberStep_[ch] = dir * barberClock_[ch].inc;
		barberRotStep_[ch] = std::polar(1.0, twoPi * barberStep_[ch]);
	}
	if (barber) {
		const double psiL = barberPhaseOf(rho, t.sweep);
		psi_ = { psiL, channelBarberPhase(barberPhaseOf(rhoR, t.sweep), stereo_, 1) };
	}
	else {
		psi_ = {};
	}
	const bool flanger = mode_ == Mode::Flanger;
	// THE FLANGER'S PER-TAP EXPLORE OFFSETS (`tapDelayOffsetLog2`, `tapPhaseOffset`), ramped per sample like the
	// flanger's other reads, so a drag never steps the read. At home every target is 0.0, every ramp sits on 0.0, and
	// the per-sample road is the plain one.
	// AT HOME WITH EVERY RAMP ALREADY ON 0.0 (`flangerOffsets_` false says so) there is nothing to aim: skipped.
	const bool rampsAtHome = exploreAtHome_ && !flangerOffsets_;
	flangerOffsets_ = false;
	for (size_t ch = 0; ch < 2 && !rampsAtHome; ++ch) {
		const TapOffsets want = tapOffsetsOf(barber, static_cast<int>(ch), explore_);
		for (size_t k = 0; k < static_cast<size_t>(kBarberVoices); ++k) {
			Ramp& delayOff = tapLog2Ramp_[ch][k];
			Ramp& phaseOff = tapPhaseRamp_[ch][k];
			delayOff.aim(want.log2[k], snapNow);
			phaseOff.aim(want.phase[k], snapNow);
			flangerOffsets_ = flangerOffsets_ || delayOff.value != 0.0f || delayOff.end != 0.0f
			                  || phaseOff.value != 0.0f || phaseOff.end != 0.0f;
		}
	}
	flangerRun_ = flanger && (flangerOffsets_ || !bend.atHome());
	for (size_t ch = 0; ch < 2; ++ch) {
		sweepBase_[ch].aim(static_cast<float>(position_ + envAmount_ * env), snapNow);
		const double phase = lfoPhaseUsed_[ch];
		// THIS CHANNEL'S TURNS OVER ONE TICK, by its own clock (R's runs at the skewed rate).
		const double tickTurns = lfoClock_[ch].inc * static_cast<double>(kControlTick);
		const double barberTickTurns = barberClock_[ch].inc * static_cast<double>(kControlTick);
		// THE FLANGER'S PER-SAMPLE LFO PHASE, seeded so the tick's last sample reads exactly this tick's φ (the
		// phasor's arrangement). Read only while `flangerRun_`.
		const double runSeed = phase - tickTurns;
		lfoRun_[ch] = runSeed - std::floor(runSeed);
		// THE CHANNEL'S CENTRE: the glided Center, the right moved by the explore layer's stereo skew (+0.0 at home).
		const double centre = channelCentreLog2(centerLog2_, static_cast<int>(ch), explore_);
		// **THE CHANNEL'S WHOLE SWEEP, BY THE PURE STATEMENT THE RESPONSE MODEL RUNS TOO** (`channelSweepOf`): each
		// voice's ψ, sweep, window, corner, per-stage sweeps and flanger read — the read being what the per-sample delay
		// passes through on the tick's last sample, and what the readings report. The flanger's tap offsets are the
		// ramps' TARGETS, which the per-sample taps land on at the tick's last sample.
		SweepInputs in;
		in.flanger = flanger;
		in.barberpole = barber;
		in.throughZero = throughZero_;
		in.position = position_;
		in.lfoAmount = lfoAmount_;
		in.envTerm = envAmount_ * env;
		in.lfoPhase = phase;
		in.psi = psi_[ch];
		in.centreLog2 = centre;
		in.delayLog2 = delayLog2_;
		in.squareNorm = squareNorm_;
		in.sampleRate = sampleRate_;
		for (size_t k = 0; k < static_cast<size_t>(kBarberVoices); ++k) {
			in.taps.log2[k] = tapLog2Ramp_[ch][k].end;
			in.taps.phase[k] = tapPhaseRamp_[ch][k].end;
		}
		ChannelSweep made;
		channelSweepOf(in, explore_, made);
		sweep_[ch] = made.sweep;
		cornerHz_[ch] = made.cornerHz;
		delayTarget_[ch] = made.delay;
		if (!barber) {
			lfoRot_[ch] = std::polar(1.0, twoPi * (phase - tickTurns));
			// THE CORNERS, as each stage's TPT gain G = g/(1+g) — the divide once a tick, not once a sample per stage.
			// The flanger has no stages; its cascades are aimed at the channel's sweep, unheard.
			voiceTick_[0][ch] = made.voices[0];
			aimStages(cascades_[0][ch], made.voices[0].stageS, centre, snapNow);
			gain_[0][ch].aim(1.0f, snapNow);
			for (size_t v = 1; v < static_cast<size_t>(voicesRunning_); ++v) {
				// THE TICK LEAVING THE BARBERPOLE: voices 1 and 2 fade out where they stand.
				gain_[v][ch].aim(0.0f, snapNow);
				for (Ramp& G : cascades_[v][ch].G) {
					G.aim(G.end, false);
				}
				voiceTick_[v][ch] = {};
			}
			continue;
		}
		// THE BARBERPOLE. The flanger's per-sample ramp phase and its window phasor are seeded so the tick's last sample
		// reads exactly this tick's ψ, as the plain phasor is. (The per-stage offsets act on the sweep VALUE, never on ψ:
		// `stageSweepsOf` has the reason.)
		const double seed = psi_[ch] - dir * barberTickTurns;
		barberRun_[ch] = seed - std::floor(seed);
		barberRot_[ch] = std::polar(1.0, twoPi * barberRun_[ch]);
		for (size_t v = 0; v < static_cast<size_t>(kBarberVoices); ++v) {
			const VoiceSweep& vs = made.voices[v];
			voiceTick_[v][ch] = vs;
			aimStages(cascades_[v][ch], vs.stageS, centre, snapNow || (joining && v > 0));
			gain_[v][ch].aim(static_cast<float>(vs.w), snapNow);
		}
	}
	placed_ = true;
}

void Phaser::aimStages(Channel& c, const std::array<double, kMaxStages>& stageS, double centreLog2,
                       bool snap) noexcept {
	// EACH STAGE AT ITS OWN SWEEP (the explore layer's per-stage LFO offsets already in `stageS`: on φ in Normal, on
	// the sweep VALUE and never on ψ under the barberpole — `stageSweepsOf` has the reason) AND ITS OWN SPREAD
	// (`stageCornerHz`, indices 0..11). The corner's exp2, the prewarp and the divide are paid only where a stage's
	// sweep or spread differs from the one before it — so at home, once a voice, as before the layer existed; with
	// travel, up to twelve.
	double lastS = std::numeric_limits<double>::quiet_NaN();
	double lastSpread = std::numeric_limits<double>::quiet_NaN();
	float G = 0.0f;
	for (size_t i = 0; i < static_cast<size_t>(kMaxStages); ++i) {
		const double s = stageS[i];
		const double spread = stageSpreadOf(i, explore_); // the spread and the notch pairing (index 32)
		if (!(s == lastS && spread == lastSpread)) {
			G = tptGainOf(stageCornerHz(centreLog2, s, spread, sampleRate_), sampleRate_);
			lastS = s;
			lastSpread = spread;
		}
		c.G[i].aim(G, snap);
	}
}

void Phaser::beginSweepCrossfade() noexcept {
	// THE OUTGOING READ, AS THE LAST SAMPLE RAN IT: the taps and their windows leaving the barberpole, else the plain
	// read's delay. A switch landing on a fade in flight fades from the read the last sample ran as the incoming one
	// (the older frozen read is let go: a residue of at most the fade's remaining weight times their difference).
	fadeFromBarber_ = barberPrev_;
	for (FlangerChannel& f : flangers_) {
		f.fadeDelay = f.delay;
		f.fadeTapDelay = f.tapDelay;
		f.fadeTapGain = f.tapGain;
	}
	fadeLeft_ = kSweepFadeSamples;
}

float Phaser::loopOf(float wet, float& damp, float& dc, float fb) const noexcept {
	float loop = wet;
	if (!dampOpen_) {
		const float v = (loop - damp) * dampG_;
		loop = v + damp;
		damp = snap(loop + v);
	}
	// THE SATURATOR, with the explore layer's asymmetry (index 28) — `saturate(v, drive)` exactly at bias 0.
	float y = saturateWith(loop, drive_.value, biasTanh_);
	if (dcOn_) {
		// THE DC BLOCKER, only with a bias: the input less its 10 Hz TPT lowpass. Its state snaps to exact zero, as
		// the damping's does, so a tail still arrives.
		const float v = (y - dc) * dcG_;
		const float lp = v + dc;
		dc = snap(lp + v);
		y -= lp;
	}
	return fb * y;
}

float Phaser::phaserSample(Channel& c, float x, float fb) noexcept {
	// THE LOOP: last sample's wet, damped, saturated, scaled.
	float u = x + loopOf(c.prevWet, c.damp, c.dc, fb);
	// THE CASCADE: each stage a TPT lowpass, its allpass 2·lp − u.
	for (int i = 0; i < activeStages_; ++i) {
		const auto at = static_cast<size_t>(i);
		const float v = (u - c.s[at]) * c.G[at].value;
		const float lp = v + c.s[at];
		c.s[at] = snap(lp + v);
		u = 2.0f * lp - u;
	}
	c.prevWet = snap(u);
	return u;
}

float Phaser::tapsWet(const FlangerChannel& f, const std::array<float, kBarberVoices>& delays,
                      const std::array<float, kBarberVoices>& gains) noexcept {
	// THREE READS, summed by their windows BEFORE the loop — one line, one loop, one wet.
	float wet = 0.0f;
	for (size_t k = 0; k < delays.size(); ++k) {
		wet += gains[k] * f.wet.read(delays[k]);
	}
	return wet;
}

float Phaser::flangerSample(FlangerChannel& f, float x, float dryLate, float mix, float wet, float fb) noexcept {
	f.lastWet = wet;
	f.wet.write(snap(x + loopOf(wet, f.damp, f.dc, fb)));
	if (!throughZero_) {
		// THE MIX, written so wet == dry is exact.
		return x + mix * (wet - x);
	}
	// THROUGH-ZERO: the dry held back by kTzDryMs, and the wet INVERTED, so at the crossing
	// `dry + mix·(−wet − dry)` is exactly zero at Mix 0.5.
	return dryLate + mix * (-wet - dryLate);
}

double Phaser::flangerSweepSample(size_t ch) noexcept {
	// ONE COMPLEX MULTIPLY: the phasor's imaginary part is sin(2πφ), which IS `lfoShape` at home.
	lfoRot_[ch] *= lfoStep_[ch];
	if (!flangerRun_) {
		return sweepOf(static_cast<double>(sweepBase_[ch].value),
		               static_cast<double>(lfoAmountRamp_.value) * lfoRot_[ch].imag(), 0.0, curveExp_);
	}
	// **THE EXPLORE LAYER BENDS THE SHAPE OR OFFSETS THE PHASE**: the bent shape evaluated PER SAMPLE, from the
	// per-sample phase (re-seeded each tick, like the phasor) plus the channel's tap-0 offset, so the flanger sweeps
	// what the phaser's tick computes — not a straight line through it.
	return sweepOf(static_cast<double>(sweepBase_[ch].value),
	               static_cast<double>(lfoAmountRamp_.value)
	                   * lfoShapeWith(lfoRun_[ch] + static_cast<double>(tapPhaseRamp_[ch][0].value), explore_.bend,
	                                  squareNorm_),
	               0.0, curveExp_);
}

namespace {
/// THE CUBE ROOTS OF UNITY, e^(j2πk/3): voice k's window phasor is voice 0's times its root.
const std::array<std::complex<double>, kBarberVoices> kThirdTurns {
	std::complex<double> { 1.0, 0.0 },
	std::complex<double> { -0.5, 0.86602540378443864676 },
	std::complex<double> { -0.5, -0.86602540378443864676 },
};
} // namespace

void Phaser::barberTaps(size_t ch, double delayLog2) noexcept {
	// THE RAMP PHASE STEPS ONE SAMPLE, and the window phasor with it (re-seeded from ψ every tick, so neither drifts).
	// (A step is under a cycle a sample, so one conditional add or subtract wraps it — no floor.)
	double& run = barberRun_[ch];
	run += barberStep_[ch];
	if (run >= 1.0) {
		run -= 1.0;
	}
	else if (run < 0.0) {
		run += 1.0;
	}
	if (!(run < 1.0) || !(run >= 0.0)) {
		run = 0.0;
	}
	barberRot_[ch] *= barberRotStep_[ch];
	const double base = static_cast<double>(sweepBase_[ch].value);
	const double amount = static_cast<double>(lfoAmountRamp_.value);
	// THROUGH-ZERO'S DRY AND SPAN ARE THE SAME FOR EVERY TAP: paid once a sample, not once a tap.
	const double dry = static_cast<double>(tzDry_);
	const double span = throughZero_ ? tzSpanSamples(delayLog2, dry, sampleRate_) : 0.0;
	FlangerChannel& f = flangers_[ch];
	// THE LFO'S SHAPE AT THIS SAMPLE'S φ, once a channel — every tap's offset is measured from it.
	const double shapeRun = flangerOffsets_ ? lfoShapeWith(lfoRun_[ch], explore_.bend, squareNorm_) : 0.0;
	double weighted = 0.0;
	for (size_t k = 0; k < f.tapDelay.size(); ++k) {
		// EACH TAP'S SWEEP IS LINEAR IN ITS RAMP PHASE — `barberSweepOf` with the base already holding the follower's
		// term — and its window is the phasor's real part against the voice's third of a turn. ψ_k = ψ + k/3 is
		// under 5/3, so one conditional subtract wraps it (`barberVoicePhase`'s value, without its floor).
		double psiK = run + static_cast<double>(k) / static_cast<double>(kBarberVoices);
		if (psiK >= 1.0) {
			psiK -= 1.0;
		}
		double s = sweepOf(base, amount * (2.0 * psiK - 1.0), 0.0, curveExp_);
		double d = 0.0;
		if (!flangerOffsets_) {
			d = throughZero_ ? tzWetOfSpan(dry, span, s) : flangerDelaySamples(delayLog2, s, sampleRate_);
		}
		else {
			// THE EXPLORE LAYER'S TAP OFFSETS: the LFO offset as a sweep VALUE (`offsetSweepOf`; never on ψ —
			// `stageSweepsOf` has the reason),
			// the spread (and R's skew) on the delay's log2, both per sample from their ramps.
			const double off = static_cast<double>(tapPhaseRamp_[ch][k].value);
			if (off != 0.0) {
				s = offsetSweepOf(s, amount, lfoShapeWith(lfoRun_[ch] + off, explore_.bend, squareNorm_), shapeRun);
			}
			const double dl = delayLog2 + static_cast<double>(tapLog2Ramp_[ch][k].value);
			d = throughZero_ ? tzWetOfSpan(dry, tzSpanSamples(dl, dry, sampleRate_), s)
			                 : flangerDelaySamples(dl, s, sampleRate_);
		}
		const double w = barberGainOfCos((barberRot_[ch] * kThirdTurns[k]).real());
		f.tapDelay[k] = static_cast<float>(d);
		f.tapGain[k] = static_cast<float>(w);
		weighted += w * d;
	}
	f.delay = static_cast<float>(weighted);
}

void Phaser::render(float* left, float* right, int frames) noexcept {
	int done = 0;
	while (done < frames) {
		if (untilTick_ <= 0) {
			tick(done);
			untilTick_ = kControlTick;
		}
		const int run = std::min(untilTick_, frames - done);
		const bool phaser = mode_ == Mode::Phaser;
		const bool barber = barber_;
		for (int j = done; j < done + run; ++j) {
			// THE LOOPS' FEEDBACK: voice 0's two always (the plain engine and the flanger), the barberpole phaser's
			// other voices below with their cascades.
			loopFb_[0][0].advance();
			loopFb_[0][1].advance();
			drive_.advance();
			mix_.advance();
			// THE CASCADES' RAMPS STEP IN THE PHASER ONLY. The flanger does not run the cascades, so it leaves them alone:
			// `Ramp::aim` starts every tick from the last tick's `end`, never from `value`, so the phaser comes back
			// exactly where it was aimed.
			if (phaser && voicesRunning_ == 1) {
				// NORMAL: voice 0 alone, its window not read (it is 1), so not stepped — the loop as it was before
				// the barberpole, which MEASURED 8 ns a sample dearer written as the general one below.
				for (Channel& c : cascades_[0]) {
					for (int i = 0; i < activeStages_; ++i) {
						c.G[static_cast<size_t>(i)].advance();
					}
				}
			}
			else if (phaser) {
				for (size_t v = 0; v < cascades_.size(); ++v) {
					for (size_t ch = 0; ch < 2; ++ch) {
						Channel& c = cascades_[v][ch];
						for (int i = 0; i < activeStages_; ++i) {
							c.G[static_cast<size_t>(i)].advance();
						}
						gain_[v][ch].advance();
						if (v > 0) {
							loopFb_[v][ch].advance(); // voice 0's stepped above
						}
					}
				}
			}
			if (!phaser) {
				// THE FLANGER'S DELAY, PER SAMPLE (see the class comment for the zipper this removes) — or, under the
				// barberpole, its three taps' delays and windows.
				lfoAmountRamp_.advance();
				delayLog2Ramp_.advance();
				const double dl = static_cast<double>(delayLog2Ramp_.value);
				for (size_t ch = 0; ch < 2; ++ch) {
					sweepBase_[ch].advance();
					if (flangerRun_) {
						// THE EXPLORE LAYER'S PER-SAMPLE LFO PHASE (its offsets and bent shape read it) and its tap ramps.
						double& run = lfoRun_[ch];
						run += lfoClock_[ch].inc;
						if (run >= 1.0) {
							run -= 1.0;
						}
						if (!(run >= 0.0 && run < 1.0)) {
							run = 0.0;
						}
					}
					if (flangerOffsets_) {
						for (size_t k = 0; k < static_cast<size_t>(kBarberVoices); ++k) {
							tapLog2Ramp_[ch][k].advance();
							tapPhaseRamp_[ch][k].advance();
						}
					}
					if (barber) {
						barberTaps(ch, dl);
						continue;
					}
					// THE CHANNEL'S DELAY moved by its tap-0 offset — spread, and the stereo skew on R (+0.0 at home).
					const double dlCh = dl + static_cast<double>(tapLog2Ramp_[ch][0].value);
					const double s = flangerSweepSample(ch);
					flangers_[ch].delay = static_cast<float>(throughZero_ ? tzWetSamples(dlCh, s, sampleRate_)
					                                                      : flangerDelaySamples(dlCh, s, sampleRate_));
				}
			}
			const float mix = mix_.value;
			// THE SWEEP-SWITCH CROSSFADE's weight on the outgoing read this sample: (n − 1)/n … 0 over its samples.
			// The flanger's alone (the phaser's switch fades by its windows' ramps).
			bool fading = false;
			float fadeWeight = 0.0f;
			if (!phaser && fadeLeft_ > 0) {
				fading = true;
				--fadeLeft_;
				fadeWeight = static_cast<float>(fadeLeft_) / static_cast<float>(kSweepFadeSamples);
			}
			for (int ch = 0; ch < 2; ++ch) {
				float* io = ch == 0 ? left : right;
				if (io == nullptr) {
					continue;
				}
				const auto at = static_cast<size_t>(ch);
				const float dry = sane(io[j]);
				// THE FOLLOWER HEARS THE INPUT, before anything this device does to it.
				envPeak_ = std::max(envPeak_, std::fabs(dry));
				// THE THROUGH-ZERO DRY RING IS WRITTEN ON EVERY ROAD, so switching it on (or the device back on)
				// finds it current. Read before write, as the wet is, so equal delays align exactly.
				FlangerChannel& f = flangers_[at];
				const float dryLate = phaser || !throughZero_ ? 0.0f : f.dryAt(tzDry_);
				f.writeDry(dry);
				if (phaser) {
					// ONE VOICE IS THE PLAIN ENGINE, unweighted (its window is 1 by construction); three are summed by
					// their windows, each cascade with its own loop.
					float wet = 0.0f;
					if (voicesRunning_ == 1) {
						wet = phaserSample(cascades_[0][at], dry, loopFb_[0][at].value);
					}
					else {
						for (size_t v = 0; v < cascades_.size(); ++v) {
							wet += gain_[v][at].value * phaserSample(cascades_[v][at], dry, loopFb_[v][at].value);
						}
					}
					// THE MIX, written so wet == dry is exact.
					io[j] = dry + mix * (wet - dry);
					f.writeLate(dry); // the plain flanger's ring kept current (`FlangerChannel::late`)
				}
				else {
					// READ BEFORE WRITE: the read is `delay` writes ago (sinc_delay_line.h's contract) — or, under the
					// barberpole, the three taps' windowed sum.
					float wet = barber ? tapsWet(f, f.tapDelay, f.tapGain) : f.wet.read(f.delay);
					if (fading) {
						// THE OUTGOING READ, frozen at the switch, on the same line — faded out against the incoming.
						const float outgoing = fadeFromBarber_ ? tapsWet(f, f.fadeTapDelay, f.fadeTapGain)
						                                       : f.wet.read(f.fadeDelay);
						wet += fadeWeight * (outgoing - wet);
					}
					const float y = flangerSample(f, dry, dryLate, mix, wet, loopFb_[0][at].value);
					if (throughZero_ || tzDry_ <= 0 || plainUndelayedForTest_) {
						io[j] = y;
						f.writeLate(dry);
					}
					else {
						// THE PLAIN FLANGER, AS LATE AS THROUGH-ZERO: the latency the Flanger reports either way
						// (`latencySamplesFor`), so flipping Through Zero never asks the host for a restart.
						io[j] = f.lateAt(tzDry_);
						f.writeLate(y);
					}
				}
			}
		}
		untilTick_ -= run;
		done += run;
		if (untilTick_ == 0) {
			// THE TICK'S END, wherever the host cut its blocks: the follower steps on the whole tick's peak.
			followerStep();
		}
	}
	guardLoop();
}

void Phaser::publishReadings() noexcept {
	if (sink_.publish == nullptr) {
		return;
	}
	float values[kReadingCount] {};
	const auto put = [&values](Reading r, double v) { values[static_cast<size_t>(r)] = static_cast<float>(v); };
	const double msPerSample = 1000.0 / sampleRate_;
	const double dry = throughZero_ ? static_cast<double>(tzDry_) : 0.0;
	put(Reading::SweepL, sweep_[0]);
	put(Reading::SweepR, sweep_[1]);
	put(Reading::LfoPhase, lfoPhaseUsed_[0]);
	// THE READING THE SWEEP USED, so Env, SweepL and SweepR are one tick's facts together (the follower itself
	// may have stepped once since, at the last tick's end).
	put(Reading::Env, envUsed_);
	put(Reading::CornerHzL, cornerHz_[0]);
	put(Reading::CornerHzR, cornerHz_[1]);
	put(Reading::DelayMsL, (delayTarget_[0] - dry) * msPerSample);
	put(Reading::DelayMsR, (delayTarget_[1] - dry) * msPerSample);
	put(Reading::NoteHz, noteHz_);
	put(Reading::BarberPhase, psi_[0]);
	sink_.publish(sink_.context, values, kReadingCount);
}

void Phaser::guardLoop() noexcept {
	bool finite = true;
	for (const auto& voice : cascades_) {
		for (const Channel& c : voice) {
			finite = finite && std::isfinite(c.prevWet) && std::isfinite(c.damp) && std::isfinite(c.dc);
		}
	}
	for (const FlangerChannel& f : flangers_) {
		finite = finite && std::isfinite(f.lastWet) && std::isfinite(f.damp) && std::isfinite(f.dc);
	}
	if (!finite) {
		++loopResets_;
		resetLoop();
	}
}

double Phaser::voiceStateMagnitudeForTest(int voice, int channel) const noexcept {
	const Channel& c = cascades_[voiceIndex(voice)][static_cast<size_t>(channel & 1)];
	double sum = std::fabs(static_cast<double>(c.prevWet)) + std::fabs(static_cast<double>(c.damp));
	for (const float s : c.s) {
		sum += std::fabs(static_cast<double>(s));
	}
	return sum;
}

void Phaser::resetLoop() noexcept {
	clearPhaser();
	for (FlangerChannel& f : flangers_) {
		// THE LINE, ITS LOWPASS AND ITS DC BLOCKER — not the read delay (a coefficient) nor the dry ring (sanitised
		// input only).
		f.wet.clear();
		f.damp = 0.0f;
		f.dc = 0.0f;
		f.lastWet = 0.0f;
	}
}

void Phaser::processStereo(float* left, float* right, int frames) noexcept {
	if (frames <= 0 || left == nullptr) {
		return;
	}
	if (right == left) {
		right = nullptr;
	}
	if (!target_.enabled) {
		wasEnabled_ = false;
		// OFF IS THE DRY — AND IN THE FLANGER THE DRY IS LATE. The host was told this device's latency
		// (a function of the stored Mode, not of Enabled, as the EQ's is of its Phase), and
		// it delays everything that meets this output by that much, so switched off the device still delays
		// by exactly that — the input itself, through the ring it keeps current.
		const bool late = latencySamplesFor(target_.mode, sampleRate_) != 0;
		for (int ch = 0; ch < 2; ++ch) {
			float* io = ch == 0 ? left : right;
			if (io == nullptr) {
				continue;
			}
			FlangerChannel& f = flangers_[static_cast<size_t>(ch)];
			for (int j = 0; j < frames; ++j) {
				const float x = io[j];
				if (late) {
					io[j] = f.dryAt(tzDry_);
				}
				f.writeDry(sane(x));
				f.writeLate(sane(x));
			}
		}
		// THE READINGS STILL GO OUT, so a surface drawing the device switched off draws where it stands, rather
		// than seeing its meters stop as if the device had gone.
		publishReadings();
		return;
	}
	if (!wasEnabled_) {
		// COMING BACK ON IS AS IF JUST SWITCHED ON — nothing held from before replays — but for the dry ring,
		// which the off road kept current.
		resetState(true);
	}
	render(left, right, frames);
	publishReadings();
}

void Phaser::processMono(float* mono, int frames) noexcept { processStereo(mono, nullptr, frames); }


//--------------------------------------------------------------------------------------------------//
// THE OPS TABLE.                                                                                     //
//--------------------------------------------------------------------------------------------------//

namespace {
Phaser& asPhaser(void* storage) { return *static_cast<Phaser*>(storage); }
} // namespace

const device::AudioOps kAudioOps {
    .stateBytes = sizeof(Phaser),
    .stateAlign = alignof(Phaser),
    .construct = [](void* storage) { new (storage) Phaser(); },
    .destruct = [](void* storage) { asPhaser(storage).~Phaser(); },
    .prepare = [](void* storage, double sampleRate, int) { asPhaser(storage).prepare(sampleRate); },
    .reset = [](void* storage) { asPhaser(storage).reset(); },
    .setFields =
        [](void* storage, const float* fields, size_t fieldsPresent) {
	        Row row {};
	        const size_t take = std::min(fieldsPresent, row.size());
	        for (size_t i = 0; i < take; ++i) {
		        row[i] = fields[i];
	        }
	        // THE COUNT GOES THROUGH (absent fields decode to their declared defaults), and THE FOLDED
	        // TRAVEL, which lives past the params and so past `Row` — the shaper's arrangement. A row with
	        // none reads as zero travel: home.
	        Settings s = stateOf(row, take).settings;
	        for (size_t i = 0; i < phase_space::kPhaseCount; ++i) {
		        const size_t at = kTravelFirst + i;
		        s.travel[i] = at < fieldsPresent && std::isfinite(fields[at]) ? fields[at] : 0.0f;
	        }
	        asPhaser(storage).setTarget(s);
        },
    .setNoteContext =
        [](void* storage, const device::NoteContext& note) { asPhaser(storage).setNoteContext(note); },
    .setTransportClock =
        [](void* storage, const device::TransportClock& clock) { asPhaser(storage).setTransportClock(clock); },
    .processStereo =
        [](void* storage, float* left, float* right, int frames) {
	        asPhaser(storage).processStereo(left, right, frames);
        },
    .processMono = [](void* storage, float* mono, int frames) { asPhaser(storage).processMono(mono, frames); },
    .setReadingsSink =
        [](void* storage, device::ReadingsSink sink) { asPhaser(storage).setReadingsSink(sink); },
    .setOuterSources = nullptr,
    .beginPerVoiceBlock = nullptr,
    .processVoice = nullptr,
    .windowedControls = {},
    .setFieldWindows = nullptr,
    .latencySamples =
        [](const void*, const float* fields, size_t fieldsPresent, double sampleRate) -> uint32_t {
	        // A PURE FUNCTION OF THE STORED ROW AND THE RATE (registry.h, assumption L1): Mode alone, decoded as
	        // `stateOf` decodes it — an absent field at its default. Through Zero does not move it (`latencySamplesFor`).
	        const auto read = [&](Param p) {
		        const auto i = static_cast<size_t>(p);
		        return i < fieldsPresent && fields != nullptr ? coerce(kParamDescs[i], fields[i])
		                                                      : static_cast<float>(kParamDescs[i].def);
	        };
	        const auto mode = static_cast<Mode>(choiceIndex(read(Param::Mode), std::size(kModeNames)));
	        return latencySamplesFor(mode, sampleRate);
        },
    .setFieldsR = nullptr,
    .holdsTail = [](const void* storage) { return static_cast<const Phaser*>(storage)->holdsTail(); },
};

} // namespace feathers::fx::phaser
