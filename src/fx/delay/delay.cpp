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

#include "fx/delay/delay.h"
#include "fx/scrub.h"

#include "dsp/compander.h"

#include "fx/ring_lerp.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace feathers::fx::delay {

namespace {

const device::ParamDesc* descOf(Param param) {
	const auto index = static_cast<size_t>(param);
	if (index >= std::size(kParamDescs)) {
		return nullptr;
	}
	return &kParamDescs[index];
}

float coerce(const device::ParamDesc& desc, float value) {
	if (!std::isfinite(value)) {
		return static_cast<float>(desc.def);
	}
	const float clamped = device::coerceControl(desc, value);
	// STEPPED CONTROLS ROUND ON THE WAY IN — device.h's rule: `coerceControl` deliberately does not
	// round, because a switch stored at 0.4 must survive as the hand left it, and every READER takes
	// the nearest step. This decode is one of those readers.
	return desc.stepped ? static_cast<float>(std::lround(clamped)) : clamped;
}

/// **THE SAFETY CLIP IN THE FEEDBACK LOOP, AND IT IS TRANSPARENT BELOW FULL SCALE.**
///
/// Portal's manual warns of exactly the failure this prevents: *"If DENSITY is small, and GRAIN SIZE
/// is large, volume spikes in the signal can cause the feedback to get very loud."* Overlapping
/// grains sum, and a summed loop with feedback near unity is an exponent waiting to happen.
///
/// **IDENTITY BELOW 1.0 RATHER THAN A CURVE EVERYWHERE**, which is the difference between a safety
/// net and a tone control. A `tanh` or a rational saturator costs 2 dB at full scale and would make
/// every repeat quieter than the last for a reason no control explains. This is exactly `x` until the
/// signal is already over, then asymptotes to 2 — bounded, continuous at the join, and inaudible
/// until it is doing its job.
float softClip(float x) noexcept {
	const float a = std::abs(x);
	if (a <= 1.0f) {
		return x;
	}
	return (x < 0.0f ? -1.0f : 1.0f) * (2.0f - 1.0f / a);
}

/// **TAPE SATURATION: A `tanh` WHOSE SMALL-SIGNAL GAIN IS HELD AT UNITY.** `tanh(d·x)/d` tends to
/// `x` as `d` tends to zero, so the control's bottom is genuinely transparent rather than "a little
/// saturated" — a Saturation knob that colours the sound at 0% is a knob whose range starts in the
/// wrong place. Its ceiling is `1/d`, which shrinks as the drive rises: more drive is more
/// compression, never more level, and that is what bounds the loop in this mode.
///
/// BELOW A DRIVE OF ONE THE CEILING IS ABOVE FULL SCALE, which is why the caller still runs the
/// transparent safety clip over the result — free where this has already done the job, and the
/// bound where it has not.
float tapeSat(float x, float drive) noexcept {
	if (drive < 1.0e-4f) {
		return x;
	}
	return std::tanh(drive * x) / drive;
}

/// A ONE-POLE COEFFICIENT. `exp(-2*pi*f/sr)`, so f == 0 gives 1 (the pole never moves) and a large f
/// gives 0 (the pole follows the input exactly). Both ends are passthrough for the filter they are
/// used in, which is what lets the tone tilt run both poles unconditionally.
float onePole(double hz, double sampleRate) noexcept {
	if (!(hz > 0.0) || !(sampleRate > 0.0)) {
		return 1.0f;
	}
	const double nyquist = sampleRate * 0.5;
	const double f = std::min(hz, nyquist * 0.99);
	return static_cast<float>(std::exp(-2.0 * 3.14159265358979323846 * f / sampleRate));
}

} // namespace

//--------------------------------------------------------------------------------------------------//
// THE CODEC.                                                                                         //
//--------------------------------------------------------------------------------------------------//

Row rowOf(const State& state) {
	Row row {};
	const Settings& s = state.settings;
	row[static_cast<size_t>(Param::Enabled)] = s.enabled ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::Sync)] = static_cast<float>(s.sync);
	row[static_cast<size_t>(Param::Time)] = static_cast<float>(std::log2(s.timeMultiple));
	row[static_cast<size_t>(Param::Feedback)] = s.feedback;
	row[static_cast<size_t>(Param::Tone)] = s.tone;
	row[static_cast<size_t>(Param::PingPong)] = static_cast<float>(static_cast<int>(s.pingPong));
	row[static_cast<size_t>(Param::Spread)] = s.spread;
	row[static_cast<size_t>(Param::Mode)] = static_cast<float>(static_cast<int>(s.mode));
	// **ZERO SECONDS IS THE OFF POSITION AND IT ENCODES AS THE RANGE'S MINIMUM**, which is the whole
	// of the `Unit::LogSeconds` convention: a log domain has no zero to mean "no time" with, so the
	// bottom of the range carries it. `log2(0)` is -inf, so this cannot be the plain inverse.
	row[static_cast<size_t>(Param::GrainSize)] =
	    s.grainSeconds > 0.0 ? static_cast<float>(std::log2(s.grainSeconds))
	                         : static_cast<float>(kGrainSizeLog2Min);
	row[static_cast<size_t>(Param::Density)] = static_cast<float>(std::log2(s.densityHz));
	row[static_cast<size_t>(Param::Spray)] = s.spray;
	row[static_cast<size_t>(Param::Pitch)] = s.pitchSemitones;
	row[static_cast<size_t>(Param::Mix)] = s.mix;
	row[static_cast<size_t>(Param::Dub)] = s.dub ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::Wow)] = s.wow;
	row[static_cast<size_t>(Param::Flutter)] = s.flutter;
	row[static_cast<size_t>(Param::Saturation)] = s.saturation;
	row[static_cast<size_t>(Param::HfLoss)] = s.hfLoss;
	row[static_cast<size_t>(Param::Taps)] = static_cast<float>(s.taps);
	row[static_cast<size_t>(Param::TapPattern)] = s.tapPattern;
	row[static_cast<size_t>(Param::TapLevels)] = s.tapLevels;
	row[static_cast<size_t>(Param::TapWidth)] = s.tapWidth;
	row[static_cast<size_t>(Param::Duck)] = s.duck;
	row[static_cast<size_t>(Param::DuckRelease)] = static_cast<float>(std::log2(s.duckReleaseSeconds));
	row[static_cast<size_t>(Param::Freeze)] = s.freeze ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::Clear)] = s.clear ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::Swarm)] = s.swarm;
	row[static_cast<size_t>(Param::BbdStages)] = [&] {
		for (size_t i = 0; i < std::size(kBbdStageCounts); ++i) {
			if (kBbdStageCounts[i] == s.bbdStages) {
				return static_cast<float>(i);
			}
		}
		return static_cast<float>(kBbdDefaultStages);
	}();
	row[static_cast<size_t>(Param::BbdCompander)] = s.bbdCompander;
	row[static_cast<size_t>(Param::BbdNoise)] = s.bbdNoise;
	row[static_cast<size_t>(Param::BbdDrive)] = s.bbdDrive;
	return row;
}

State stateOf(const Row& row, size_t fieldsPresent) {
	State out {};
	const auto read = [&](Param p) {
		const device::ParamDesc* d = descOf(p);
		if (d == nullptr) {
			return 0.0f;
		}
		// A FIELD THE PAYLOAD DOES NOT REACH KEEPS ITS DECLARED DEFAULT — device_row.h's rule, and the
		// reason the count travels with the row rather than beside it.
		return static_cast<size_t>(p) < fieldsPresent ? coerce(*d, row[static_cast<size_t>(p)])
		                                              : static_cast<float>(d->def);
	};

	Settings& s = out.settings;
	s.enabled = read(Param::Enabled) >= 0.5f;
	s.sync = static_cast<size_t>(std::lround(read(Param::Sync)));
	s.timeMultiple = std::exp2(static_cast<double>(read(Param::Time)));
	s.feedback = read(Param::Feedback);
	s.tone = read(Param::Tone);
	s.pingPong = static_cast<PingPong>(std::lround(read(Param::PingPong)));
	s.spread = read(Param::Spread);
	s.mode = static_cast<Mode>(std::lround(read(Param::Mode)));
	const float grainLog2 = read(Param::GrainSize);
	s.grainSeconds = grainLog2 <= static_cast<float>(kGrainSizeLog2Min)
	                     ? 0.0
	                     : std::exp2(static_cast<double>(grainLog2));
	s.densityHz = std::exp2(static_cast<double>(read(Param::Density)));
	s.spray = read(Param::Spray);
	s.pitchSemitones = read(Param::Pitch);
	s.mix = read(Param::Mix);
	s.dub = read(Param::Dub) >= 0.5f;
	s.wow = read(Param::Wow);
	s.flutter = read(Param::Flutter);
	s.saturation = read(Param::Saturation);
	s.hfLoss = read(Param::HfLoss);
	s.taps = static_cast<int>(std::lround(read(Param::Taps)));
	s.tapPattern = read(Param::TapPattern);
	s.tapLevels = read(Param::TapLevels);
	s.tapWidth = read(Param::TapWidth);
	s.duck = read(Param::Duck);
	s.duckReleaseSeconds = std::exp2(static_cast<double>(read(Param::DuckRelease)));
	s.freeze = read(Param::Freeze) >= 0.5f;
	s.clear = read(Param::Clear) >= 0.5f;
	s.swarm = read(Param::Swarm);
	s.bbdStages = kBbdStageCounts[std::clamp<size_t>(static_cast<size_t>(std::lround(read(Param::BbdStages))), 0,
	                                                   std::size(kBbdStageCounts) - 1)];
	s.bbdCompander = read(Param::BbdCompander);
	s.bbdNoise = read(Param::BbdNoise);
	s.bbdDrive = read(Param::BbdDrive);
	return out;
}

float valueOf(const State& state, Param param) {
	const auto index = static_cast<size_t>(param);
	if (index >= static_cast<size_t>(Param::kCount)) {
		return 0.0f;
	}
	// READ OFF THE ROW rather than off a second switch over the same fields — the row IS this
	// device's indexed view of itself, and a switch beside it would be a second copy of the order
	// `rowOf` exists to hold alone.
	return rowOf(state)[index];
}

void setValueOf(State* state, Param param, float value) {
	const auto index = static_cast<size_t>(param);
	if (state == nullptr || index >= static_cast<size_t>(Param::kCount)) {
		return;
	}
	const device::ParamDesc* desc = descOf(param);
	if (desc == nullptr || !std::isfinite(value)) {
		return;
	}
	// THROUGH THE ROW, so there is one field order and one coercion rule. A write past a control's
	// end STOPS AT THE END rather than falling through to the default.
	Row row = rowOf(*state);
	row[index] = coerce(*desc, value);
	*state = stateOf(row, static_cast<size_t>(Param::kCount));
}

double tapSeconds(const Settings& s, const device::TransportClock& clock, float notePitch) {
	return std::clamp(syncBaseSeconds(s, clock, notePitch) * s.timeMultiple, kMinDelaySeconds, kMaxDelaySeconds);
}

double syncBaseSeconds(const Settings& s, const device::TransportClock& clock, float notePitch) {
	const auto choice = static_cast<int>(s.sync);
	double base = 0.0;
	// THE HELD NOTE'S PERIOD (S6b): the tempo and the free list play no part.
	if (isNoteTime(choice)) {
		return notePeriodSeconds(notePitch);
	}
	if (isFreeTime(choice)) {
		base = freeTimeSecondsFor(choice);
	}
	else {
		// **BEATS, NOT SECONDS, AND THE CLOCK IS ASKED FOR `secondsPerBeat` RATHER THAN A POSITION.**
		// device.h states the rule and `automod` follows it: a synced division's period is N beats, so
		// deriving it from the beat length keeps it on the grid and survives tempo automation, while a
		// seconds-derived period goes discontinuous the moment the tempo moves.
		//
		// ONE SECOND A BEAT WHEN THE HOST HAS SAID NOTHING. `haveTempo()` is independent of `playing`
		// — a stopped host still reports its tempo — so this is rarer than it looks, and 60 BPM is a
		// number a hand can recognise as "no tempo" rather than a silence.
		const double secondsPerBeat = clock.haveTempo() ? clock.secondsPerBeat : 1.0;
		base = syncDivisionBeats(choice) * secondsPerBeat;
	}
	return base;
}

//--------------------------------------------------------------------------------------------------//
// THE DSP.                                                                                           //
//--------------------------------------------------------------------------------------------------//

void Delay::prepare(double sampleRate) {
	sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
	// **THE ONE ALLOCATION, AND IT IS SIZED FROM A CONSTANT RATHER THAN FROM THE SETTINGS.** A
	// setting moves on the audio thread and an allocation may not follow it there, so the line is as
	// long as this device can ever be asked for and the tap is clamped into it.
	line_.prepare(sampleRate_, kMaxDelaySeconds);
	tapFadeFrames_ = std::max(1, static_cast<int>(std::lround(kTapFadeSeconds * sampleRate_)));
	tapFadeInc_ = 1.0 / static_cast<double>(tapFadeFrames_);
	reset();
}

void Delay::Ramp::retarget(float to, int frames) noexcept {
	if (to == target) {
		return; // THE SAME ASK: a ramp already under way carries on, so a split block splits nothing
	}
	target = to;
	left = std::max(1, frames);
	step = (to - now) / static_cast<float>(left);
}

void Delay::startSlotFade(Slot& slot, double headFrames) const noexcept {
	if (slot.fadePos >= 1.0 && std::abs(slot.target - slot.frac) * headFrames > 0.5) {
		slot.fracFrom = slot.frac;
		slot.frac = slot.target;
		slot.fadePos = 0.0;
	}
}

void Delay::reset() noexcept {
	// **AS IF JUST SWITCHED ON.** `Ring::reset` zeroes the audio and the write position while keeping
	// the memory and the length, which is exactly the half that belongs to the line; everything below
	// is the half that belongs to the device. The counter goes back to zero WITH the ring's write
	// position, which is what keeps `writeCounter_ % length == writePos` true from a fresh start.
	line_.reset();
	writeCounter_ = 0;
	lpL_ = 0.0f;
	lpR_ = 0.0f;
	hpL_ = 0.0f;
	hpR_ = 0.0f;
	activeGrains_ = 0;
	spawnAccum_ = 0.0;
	// THE RANDOM STREAM TOO, and it is not decoration: `test_fx_devices.cpp` asserts that the same
	// samples in a different partition are the same samples out, and a granulator whose stream
	// survived a reset would fail that for a reason unrelated to the bug the case hunts.
	rng_ = kRngSeed;
	quietRun_ = line_.length(); // the line was just zeroed: it holds nothing (`holdsTail`, backlog MC)
	// **BACK TO "NOT YET PLACED"**, so the first block after a reset snaps to the controls rather
	// than gliding up from wherever the head happened to be — "as if just switched on".
	tapNowL_ = -1.0;
	tapNowR_ = -1.0;
	tapFadeFromL_ = 0.0;
	tapFadeFromR_ = 0.0;
	fadePos_ = 1.0;
	fadeInc_ = 1.0;
	wowPhaseA_ = 0.0;
	wowPhaseB_ = 0.0;
	flutterPhaseA_ = 0.0;
	flutterPhaseB_ = 0.0;
	tapeLpL_ = 0.0f;
	tapeLpR_ = 0.0f;
	lastTapSeconds_ = 0.0;
	lastTapSecondsRight_ = 0.0;
	lastSpreadFrames_ = 0.0;
	// THE STRIP'S HALF OF THE DEVICE — the sink is the host's and stays.
	lastHeadL_ = 0.0;
	lastHeadR_ = 0.0;
	lastFadeNew_ = 1.0f;
	lastFadeOld_ = 0.0f;
	// THE REVERSE'S HEADS go back to "not placed", so the first Reverse block seats them afresh.
	for (int c = 0; c < 2; ++c) {
		revMirrorA_[c] = 0.0;
		revMirrorB_[c] = 0.0;
		revHalfDone_[c] = false;
		revChunk_[c] = 0.0;
		revWeight_[c] = 0.0f;
	}
	revPlaced_ = false;
	// THE MULTI-TAP'S SLOTS go back to "not placed" (backlog MD), so the first Multi-Tap block seats them afresh.
	slots_.fill(Slot {});
	tapNormRamp_.snap(1.0f);
	slotsPlaced_ = false;
	// THE LANE'S LOOP STATE (the lane itself is the holder's to reset).
	laneL_.fill(0.0f);
	laneR_.fill(0.0f);
	dcInL_ = 0.0f;
	dcInR_ = 0.0f;
	dcOutL_ = 0.0f;
	dcOutR_ = 0.0f;
	limitGain_ = 1.0f;
	lanePass_ = 1.0f;
	laneInEnergy_ = 0.0;
	laneOutEnergy_ = 0.0;
	chunkAt_ = 0;
	chunkN_ = 0;
	laneCap_ = kLoopChunkMax;
	laneQuiet_ = static_cast<size_t>(insert_.latency) + kLoopChunkWide;
	notePitch_ = kDefaultNotePitch; // THE BUS NOTE (S6b): forgotten, as the Phaser forgets its key track
	// THE BUCKET BRIGADE AND THE SWARM (2026-11-05).
	for (auto& z : bbdAa_) {
		z.fill(0.0f);
	}
	for (auto& z : bbdRe_) {
		z.fill(0.0f);
	}
	bbdPhase_.fill(0.0);
	bbdHeld_.fill(0.0f);
	bbdEnvIn_ = 0.0f;
	bbdEnvOut_ = 0.0f;
	bbdEnvCh_.fill(0.0f);
	bbdRng_ = kRngSeed;
	swarmDrawCount_ = 0;
	swarmSerial_ = 0;
	songSpawns_ = 0;
	songLastIndex_ = -1;
	grainsOnSong_ = false;
	sprayDrawCount_ = 0;
	// THE DUCK, THE FREEZE, THE CLEAR (S4).
	duckEnv_ = 0.0f;
	lastDuckGain_ = 1.0f;
	freezeNow_ = 0.0f;
	frozenL_ = 0;
	frozenR_ = 0;
	clearFade_ = 1.0f;
	clearFading_ = false;
	clearPrimed_ = false;
}

int Delay::chunkFor(double nearestFeedFrames, int cap) const noexcept {
	// THE FEED READ AT `t` READS POSITION `t − D`, WHOSE FEEDBACK LANDS AT THE END OF THE CHUNK HOLDING `t − D + L`.
	// So the chunk must satisfy D(t) ≥ L + n for every t in it; the head can close by at most
	// `kGlideMaxFramesPerSample` a sample, hence the 1.5. At least one sample, so a head still gliding up to a new
	// floor runs (and reads a slice a few samples early) rather than stalls.
	const double room = (nearestFeedFrames - static_cast<double>(insert_.latency)) / (1.0 + kGlideMaxFramesPerSample);
	const int fit = room >= 1.0 ? static_cast<int>(std::floor(room)) : 1;
	return std::max(1, std::min(cap, fit));
}

void Delay::flushLane(int n) noexcept {
	// WITHOUT A LANE (it went away with a chunk open) the returns pass as they are: the loop the device has without one.
	// THE PASS GAIN, MEASURED AROUND THE CALL — the energy in and out, smoothed (see `lanePass_`).
	double inE = 0.0;
	for (int j = 0; j < n; ++j) {
		inE += static_cast<double>(laneL_[static_cast<size_t>(j)]) * laneL_[static_cast<size_t>(j)]
		       + static_cast<double>(laneR_[static_cast<size_t>(j)]) * laneR_[static_cast<size_t>(j)];
	}
	if (insert_.process != nullptr) {
		insert_.process(insert_.context, laneL_.data(), laneR_.data(), n);
	}
	{
		double outE = 0.0;
		for (int j = 0; j < n; ++j) {
			const float l = laneL_[static_cast<size_t>(j)];
			const float r = laneR_[static_cast<size_t>(j)];
			outE += std::isfinite(l) ? static_cast<double>(l) * l : 0.0;
			outE += std::isfinite(r) ? static_cast<double>(r) * r : 0.0;
		}
		const double keep = std::exp(-static_cast<double>(n) / (0.2 * sampleRate_));
		laneInEnergy_ = keep * laneInEnergy_ + inE;
		laneOutEnergy_ = keep * laneOutEnergy_ + outE;
		if (laneInEnergy_ > 1.0e-12) {
			lanePass_ = static_cast<float>(std::min(16.0, std::sqrt(laneOutEnergy_ / laneInEnergy_)));
		}
	}
	++laneChunks_;
	laneChunkMaxSeen_ = std::max(laneChunkMaxSeen_, n);
	const float feedback = chunkFeedback_;
	const bool pingPong = chunkPingPong_;
	const bool tape = chunkTape_;
	const float tapeDrive = chunkDrive_;
	const double dcR = std::exp(-6.283185307179586 * kLoopDcHz / sampleRate_);
	const auto dcA = static_cast<float>(dcR);
	const auto atk = static_cast<float>(1.0 - std::exp(-1.0 / std::max(1.0, kLoopLimitAttackSeconds * sampleRate_)));
	const auto rel = static_cast<float>(1.0 - std::exp(-1.0 / std::max(1.0, kLoopLimitReleaseSeconds * sampleRate_)));
	const size_t length = line_.length();
	const auto latency = static_cast<size_t>(chunkLatency_);
	for (int j = 0; j < n; ++j) {
		float yL = laneL_[static_cast<size_t>(j)];
		float yR = laneR_[static_cast<size_t>(j)];
		// **ONE NaN IN A FEEDBACK LINE IS FOR EVER**, so a sample that is not finite is replaced by silence and counted.
		if (!std::isfinite(yL)) {
			yL = 0.0f;
			++loopResets_;
		}
		if (!std::isfinite(yR)) {
			yR = 0.0f;
			++loopResets_;
		}
		// THE DC BLOCKER: an asymmetric shaper in a loop ratchets DC up to the clip ceiling.
		const float bL = yL - dcInL_ + dcA * dcOutL_;
		const float bR = yR - dcInR_ + dcA * dcOutR_;
		dcInL_ = yL;
		dcInR_ = yR;
		dcOutL_ = std::abs(bL) < kSilenceFloor ? 0.0f : bL;
		dcOutR_ = std::abs(bR) < kSilenceFloor ? 0.0f : bR;
		float fL = feedback * dcOutL_;
		float fR = feedback * dcOutR_;
		// THE LIMITER, stereo-linked, ahead of the safety clip: a loop whose gain the lane has pushed past unity
		// swells to the ceiling and stays there rather than grinding in `softClip`.
		const float peak = std::max(std::abs(fL), std::abs(fR));
		const float want = peak > kLoopLimitCeiling ? kLoopLimitCeiling / peak : 1.0f;
		limitGain_ += (want - limitGain_) * (want < limitGain_ ? atk : rel);
		if (std::abs(1.0f - limitGain_) < 1.0e-7f) {
			limitGain_ = 1.0f;
		}
		// THE SMOOTHED GAIN RELEASES; THE INSTANT ONE NEVER LETS A PEAK PAST THE CEILING (the attack is the smoothing).
		const float gain = std::min(limitGain_, want);
		fL *= gain;
		fR *= gain;
		if (pingPong) {
			std::swap(fL, fR);
		}
		// WHERE THIS OUTPUT BELONGS: `latency` samples before the sample that fed it, which is `n − j + latency` back
		// from the write head now.
		const size_t back = static_cast<size_t>(n - j) + latency;
		if (back > length) {
			continue;
		}
		const size_t at = (line_.writePos() + length - back) % length;
		float rawL = 0.0f;
		float rawR = 0.0f;
		line_.readAt(at, &rawL, &rawR);
		float wL = 0.0f;
		float wR = 0.0f;
		if (tape) {
			wL = softClip(tapeSat(rawL + fL, tapeDrive));
			wR = softClip(tapeSat(rawR + fR, tapeDrive));
		}
		else if (chunkBbd_) {
			// THE PLAIN LOOP'S RECORD, input and return together (see the live road in `processStereo`).
			float xl = rawL + fL;
			float xr = rawR + fR;
			bbdRecord(xl, xr, chunkBbdRecord_);
			wL = softClip(xl);
			wR = softClip(xr);
		}
		else {
			wL = rawL + softClip(fL);
			wR = rawR + softClip(fR);
		}
		if (std::abs(wL) < kSilenceFloor) {
			wL = 0.0f;
		}
		if (std::abs(wR) < kSilenceFloor) {
			wR = 0.0f;
		}
		line_.writeAt(at, wL, wR);
		if (wL == 0.0f && wR == 0.0f) {
			if (quietRun_ < length) {
				++quietRun_;
			}
		}
		else {
			quietRun_ = 0;
		}
	}
}

void Delay::setTarget(const Settings& settings) noexcept { target_ = settings; }

void Delay::setTransportClock(const device::TransportClock& clock) noexcept {
	// HELD, NOT ACTED ON — `automod`'s pattern. The tap is re-derived from this at the top of every
	// block, so a tempo that does not change the tap changes nothing.
	clock_ = clock;
}

float Delay::nextBipolar() noexcept {
	rng_ ^= rng_ << 13u;
	rng_ ^= rng_ >> 17u;
	rng_ ^= rng_ << 5u;
	return static_cast<float>(static_cast<int32_t>(rng_)) * (1.0f / 2147483648.0f);
}

/// THE CHIP'S HISS: its own stream, so BBD's noise never moves a grain's spray, and reset with the device.
float Delay::nextBbdNoise() noexcept {
	bbdRng_ ^= bbdRng_ << 13u;
	bbdRng_ ^= bbdRng_ >> 17u;
	bbdRng_ ^= bbdRng_ << 5u;
	return static_cast<float>(static_cast<int32_t>(bbdRng_)) * (1.0f / 2147483648.0f);
}

void Delay::readLerp(double absolutePos, float* left, float* right) const noexcept {
	// SHARED SINCE THE PITCH SHIFTER WANTED THE SAME READ — see `fx/ring_lerp.h`.
	fx::readLerp(line_, absolutePos, left, right);
}

void Delay::readBehind(double back, float* left, float* right) const noexcept {
	// `readLerp` AT `writeCounter_ - back`, WITH THE FRACTION TAKEN FROM `back` ALONE: floor(head - back) is head - ceil(back),
	// and the weight on the later sample is ceil(back) - back. Integer index arithmetic, so nothing here sees the
	// counter's magnitude. `back` is 1..length-2 (the caller clamps), so the earlier sample is never the write head.
	const size_t len = line_.length();
	const double up = std::ceil(back);
	const auto frac = static_cast<float>(up - back);
	const auto steps = static_cast<size_t>(up);
	const size_t w = line_.writePos();
	const size_t i0 = w >= steps ? w - steps : w + len - steps;
	const size_t i1 = i0 + 1u == len ? 0u : i0 + 1u;
	float l0 = 0.0f;
	float r0 = 0.0f;
	float l1 = 0.0f;
	float r1 = 0.0f;
	line_.readWrapped(i0, &l0, &r0);
	line_.readWrapped(i1, &l1, &r1);
	*left = l0 + (l1 - l0) * frac;
	*right = r0 + (r1 - r0) * frac;
}

void Delay::readTap(double absolutePos, float* left, float* right) const noexcept {
	// **`readLerp`'s ARITHMETIC, `readLerp`'s ANSWER, WITHOUT ITS THREE MODULOS** (backlog MD). The read is always
	// behind the write head by `back` whole frames, 1 to the line's length, so its index is the write position
	// stepped back once and wrapped by one comparison — the same index `ringIndex` finds by `fmod`, negative
	// positions included, since `writeCounter_ % length` IS the write position. The fraction is computed exactly
	// as there, so the sample read is the same to the bit; the MD case pinning Multi-Tap's output is the check.
	const size_t len = line_.length();
	const double whole = std::floor(absolutePos);
	const auto frac = static_cast<float>(absolutePos - whole);
	const auto back = static_cast<size_t>(static_cast<double>(writeCounter_) - whole);
	const size_t w = line_.writePos();
	const size_t i0 = w >= back ? w - back : w + len - back;
	const size_t i1 = i0 + 1u == len ? 0u : i0 + 1u;
	float l0 = 0.0f;
	float r0 = 0.0f;
	float l1 = 0.0f;
	float r1 = 0.0f;
	line_.readWrapped(i0, &l0, &r0);
	line_.readWrapped(i1, &l1, &r1);
	*left = l0 + (l1 - l0) * frac;
	*right = r0 + (r1 - r0) * frac;
}

float Delay::reverseRead(int channel, double chunk, double now) noexcept {
	const auto c = static_cast<size_t>(channel);
	const auto capFrames = static_cast<double>(line_.length());
	double ageA = now - revMirrorA_[c];
	if (ageA >= chunk) {
		// THE LEADING HEAD JUMPS TO THE NEXT CHUNK, by exactly one chunk, so a held Time keeps the grid on
		// multiples of it — which is what lets the loop's forward read (period `chunk`) land each repeat in the
		// same place of its chunk as the first. A chunk that SHRANK past the head's age re-seats it at now.
		revMirrorA_[c] += chunk;
		ageA -= chunk;
		if (ageA >= chunk) {
			revMirrorA_[c] = now;
			ageA = 0.0;
		}
		revHalfDone_[c] = false;
	}
	if (!revHalfDone_[c] && ageA >= 0.5 * chunk) {
		// THE TRAILING HEAD JUMPS AS THE LEADING ONE PASSES ITS MIDDLE — half a chunk behind it, exactly.
		revMirrorB_[c] = revMirrorA_[c] + 0.5 * chunk;
		revHalfDone_[c] = true;
	}
	const double ageB = now - revMirrorB_[c];
	// **EQUAL GAIN: `sin^2` AND ITS COMPLEMENT**, zero at the instant either head jumps. One transcendental a
	// channel a sample.
	const double sn = std::sin(3.14159265358979323846 * ageA / chunk);
	const double wA = sn * sn;
	const double wB = 1.0 - wA;
	// THE HEAD WALKS BACK FROM ITS MIRROR AT THE LINE'S OWN SPEED, so the delay is twice the head's age — never
	// under a frame (the write head), never past the line.
	float al = 0.0f;
	float ar = 0.0f;
	float bl = 0.0f;
	float br = 0.0f;
	readLerp(now - std::clamp(2.0 * ageA, 1.0, capFrames - 2.0), &al, &ar);
	readLerp(now - std::clamp(2.0 * ageB, 1.0, capFrames - 2.0), &bl, &br);
	revChunk_[c] = chunk;
	revWeight_[c] = static_cast<float>(wA);
	const float a = channel == 0 ? al : ar;
	const float b = channel == 0 ? bl : br;
	return static_cast<float>(wA * static_cast<double>(a) + wB * static_cast<double>(b));
}

void Delay::spawnGrain(double tapFrames, double sprayFrames, double pitchRatio, double grainFrames,
                       int64_t songIndex) noexcept {
	if (activeGrains_ >= kMaxGrains) {
		return;
	}
	const auto capFrames = static_cast<double>(line_.length());
	if (capFrames < 4.0) {
		return;
	}
	Grain& g = grains_[activeGrains_++];
	// THE SWARM'S DRAWS: a hash of the seed and a grain index — the SONG'S index while the transport plays (`songIndex`),
	// else the serial since reset. Independent of the spray's stream, so Swarm moves no grain the spray already placed.
	const auto unit = [](uint64_t x) {
		x += 0x9e3779b97f4a7c15ull;
		x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
		x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
		x ^= x >> 31;
		return static_cast<double>(x >> 11) * (1.0 / 9007199254740992.0) * 2.0 - 1.0; // -1..1
	};
	const bool songGrain = songIndex != kNoSongGrain;
	const uint64_t base = kSwarmSeed ^ ((songGrain ? static_cast<uint64_t>(songIndex) : swarmSerial_) * 4);
	// **THE SPRAY IS DRAWN PER GRAIN, which is the one piece of per-grain randomness the MVP has of
	// its own.** Everything else Portal gives a DIAMOND to is reachable by routing a Random LFO at it
	// — see `delay_types.h` for why that works at any sane density. Spray is not, because it is a
	// property of a grain's BIRTH rather than of a control's value at that instant.
	// ON THE SONG'S GRID THE SPRAY IS THE GRAIN'S FOURTH HASHED DRAW, not the stream's next value: a stream's position
	// is how many grains this instance has spawned, which is when it was switched on, not where the song is.
	const double sprayDraw = songGrain ? unit(base + 3) : static_cast<double>(nextBipolar());
	const double jitter = sprayDraw * sprayFrames;
	if (sprayDrawCount_ < kSwarmDraws) {
		sprayDraws_[sprayDrawCount_++] = sprayDraw;
	}
	const double back = std::clamp(tapFrames + jitter, 1.0, capFrames - 2.0);
	g.readPos = static_cast<double>(writeCounter_) - back;
	g.inc = pitchRatio;
	g.age = 0.0;
	g.length = std::max(1.0, grainFrames);
	g.panL = 1.0f;
	g.panR = 1.0f;
	g.back = back;
	if (swarm_ > 0.0f) {
		const double s = static_cast<double>(swarm_);
		const double offset = unit(base) * s * kSwarmSpreadSeconds * (sampleRate_ > 0.0 ? sampleRate_ : 48000.0);
		const double pan = unit(base + 1) * s;
		const double semis = unit(base + 2) * s * kSwarmPitchSemitones;
		++swarmSerial_;
		const double placed = std::clamp(back + offset, 1.0, capFrames - 2.0);
		g.readPos = static_cast<double>(writeCounter_) - placed;
		g.back = placed;
		g.inc = pitchRatio * std::exp2(semis / 12.0);
		const double angle = (pan + 1.0) * 0.78539816339744831;
		g.panL = static_cast<float>(1.41421356237309505 * std::cos(angle));
		g.panR = static_cast<float>(1.41421356237309505 * std::sin(angle));
		if (swarmDrawCount_ < kSwarmDraws) {
			swarmDraws_[swarmDrawCount_++] = { placed - back, static_cast<float>(pan), static_cast<float>(semis) };
		}
	}
}

void Delay::bbdRecord(float& l, float& r, const BbdRecord& p) noexcept {
	l = tapeSat(l, p.drive);
	r = tapeSat(r, p.drive);
	const float level = std::max(std::abs(l), std::abs(r));
	// THE CHIP'S MATHS LIVE IN `dsp/compander.h` (backlog QG), shared with COMP's Compander mode, to the bit.
	dsp::compander::follow(bbdEnvIn_, level, p.attack, p.release, kSilenceFloor);
	if (p.compander > 0.0f) {
		// `powf` STAYS (backlog PV, MEASURED): a 1.3e-5 exp2/log2 polynomial was 25 µs a block SLOWER here, because this
		// gain sits on the frame's serial chain, where latency is what costs, and glibc's table-driven `powf` is as short
		// a chain as any polynomial of that accuracy without FMA.
		const float g = dsp::compander::gain(bbdEnvIn_, kBbdCompanderRef, -0.5f * p.compander, 0.25f, 4.0f);
		l *= g;
		r *= g;
	}
	l = biquad(bbdCoef_[0], bbdAa_[0], l);
	r = biquad(bbdCoef_[1], bbdAa_[1], r);
}

float Delay::biquad(const Biquad& c, std::array<float, 2>& z, float x) noexcept {
	// TRANSPOSED DIRECT FORM II.
	const float y = c.b0 * x + z[0];
	z[0] = c.b1 * x - c.a1 * y + z[1];
	z[1] = c.b2 * x - c.a2 * y;
	if (std::abs(z[0]) < kSilenceFloor) {
		z[0] = 0.0f;
	}
	if (std::abs(z[1]) < kSilenceFloor) {
		z[1] = 0.0f;
	}
	return std::abs(y) < kSilenceFloor ? 0.0f : y;
}

void Delay::renderGrains(double* outLeft, double* outRight) noexcept {
	double accL = 0.0;
	double accR = 0.0;
	const auto head = static_cast<double>(writeCounter_);
	const auto capFrames = static_cast<double>(line_.length());
	const double minPos = head - (capFrames - 2.0);
	const double maxPos = head - 1.0;
	const double spreadFrames = lastSpreadFrames_;

	if (swarm_ > 0.0f || grainsOnSong_) {
		// **THE SWARM'S WALK, WHICH IS TWO DEPARTURES FROM THE ONE BELOW, BOTH FOR THE SONG LOCK** (2026-11-05). Each grain
		// reads by its DISTANCE behind the head, never by an absolute position, so the bits it reads do not depend on how
		// long this instance has been writing; and expired grains are removed in ORDER, not swapped, so the sum runs in
		// birth order whatever died before — a swap-remove leaves a permutation that is the instance's history. Two
		// instances on the same song position then hold the same grains in the same order. Taken by Swarm, and by any grain
		// on the song's grid; stopped at Swarm 0 the walk below is today's, bit for bit.
		// MEASURED (known-bads, test_delay_lane): the relative read is load-bearing — an absolute one breaks bit-identity
		// across pre-roll. The ordered removal is NOT visible to any test at a practical length: the sum is a double cast
		// to float, so a reordering moves the output only on a float rounding boundary (~2^-29 a sample). It is kept
		// because it costs what the swap costs and makes the guarantee one of construction rather than of odds.
		const double lo = 1.0;
		const double hi = capFrames - 2.0;
		size_t keep = 0;
		for (size_t k = 0; k < activeGrains_; ++k) {
			Grain& g = grains_[k];
			const double t = g.age / g.length;
			const double w = 4.0 * t * (1.0 - t);
			const double d = std::clamp(g.back, lo, hi);
			float l = 0.0f;
			float r = 0.0f;
			readBehind(d, &l, &r);
			accL += static_cast<double>(l) * (w * static_cast<double>(g.panL));
			if (spreadFrames != 0.0) {
				float dl = 0.0f;
				float dr = 0.0f;
				readBehind(std::clamp(d + spreadFrames, lo, hi), &dl, &dr);
				accR += static_cast<double>(dr) * (w * static_cast<double>(g.panR));
			}
			else {
				accR += static_cast<double>(r) * (w * static_cast<double>(g.panR));
			}
			g.readPos += g.inc;
			g.back += 1.0 - g.inc;
			g.age += 1.0;
			if (g.age < g.length) {
				if (keep != k) {
					grains_[keep] = g;
				}
				++keep;
			}
		}
		activeGrains_ = keep;
		*outLeft = accL;
		*outRight = accR;
		return;
	}

	size_t i = 0;
	while (i < activeGrains_) {
		Grain& g = grains_[i];
		// **A PARABOLIC BELL RATHER THAN A HANN WINDOW.** `4t(1-t)` is zero at both ends, smooth
		// between them, and costs a multiply where `0.5 - 0.5*cos(2*pi*t)` costs a transcendental —
		// at 128 grains and 48 kHz that is six million cosines a second for a shape whose difference
		// from this one is inaudible on a grain envelope.
		const double t = g.age / g.length;
		const double w = 4.0 * t * (1.0 - t);

		const double pos = std::clamp(g.readPos, minPos, maxPos);
		float l = 0.0f;
		float r = 0.0f;
		readLerp(pos, &l, &r);
		const double wl = swarm_ > 0.0f ? w * static_cast<double>(g.panL) : w;
		const double wr = swarm_ > 0.0f ? w * static_cast<double>(g.panR) : w;
		accL += static_cast<double>(l) * wl;
		if (spreadFrames != 0.0) {
			// THE RIGHT CHANNEL READS ITS OWN POSITION so Spread survives into Granular mode. Without
			// this the granulator reads one place and hands the same material to both channels — a
			// control that works in one mode and silently does nothing in the other, which is the
			// honesty rule failing quietly.
			float dl = 0.0f;
			float dr = 0.0f;
			readLerp(std::clamp(pos - spreadFrames, minPos, maxPos), &dl, &dr);
			accR += static_cast<double>(dr) * wr;
		}
		else {
			accR += static_cast<double>(r) * wr;
		}

		g.readPos += g.inc;
		g.back += 1.0 - g.inc; // kept in step, so a Swarm raised mid-grain reads where this one was
		g.age += 1.0;
		if (g.age >= g.length) {
			// SWAP-REMOVE, so the per-sample walk is over the grains that exist rather than the pool.
			grains_[i] = grains_[activeGrains_ - 1];
			--activeGrains_;
			continue;
		}
		++i;
	}
	*outLeft = accL;
	*outRight = accR;
}

void Delay::processStereo(float* left, float* right, int frames) noexcept {
	if (left == nullptr || right == nullptr || frames <= 0) {
		return;
	}
	const Settings s = target_;
	if (!s.enabled) {
		return;
	}
	const auto capFrames = static_cast<double>(line_.length());
	if (capFrames < 4.0) {
		return;
	}
	// A NON-FINITE INPUT SAMPLE IS TREATED AS 0 before anything sees it (as EQ and Comp do; `fx::scrubNonFinite`).
	fx::scrubNonFinite(left, right, frames);

	// THE CLEAR (delay-love S4): a fade that reached silence empties the line HERE, between blocks, where no head is
	// mid-sample; then a rising edge starts the next fade. What the clear keeps: the freeze and the duck.
	if (clearFading_ && clearFade_ <= 0.0f) {
		const float freeze = freezeNow_;
		const size_t fl = frozenL_;
		const size_t fr = frozenR_;
		const float duck = duckEnv_;
		reset();
		freezeNow_ = freeze;
		frozenL_ = fl;
		frozenR_ = fr;
		duckEnv_ = duck;
		clearFade_ = 1.0f;
		clearFading_ = false;
		++clears_;
	}
	const bool firstBlock = !clearPrimed_;
	if (firstBlock) {
		// THE FIRST BLOCK SINCE A RESET OR A LOAD TAKES THE LEVEL AS IT FINDS IT: a patch saved with Clear high is no edge.
		clearPrev_ = s.clear;
		clearPrimed_ = true;
	}
	if (s.clear && !clearPrev_) {
		clearFading_ = true;
	}
	clearPrev_ = s.clear;
	const auto duckAttack = static_cast<float>(1.0 - std::exp(-1.0 / std::max(1.0, kDuckAttackSeconds * sampleRate_)));
	const auto duckRelease = static_cast<float>(1.0 - std::exp(-1.0 / std::max(1.0, s.duckReleaseSeconds * sampleRate_)));
	const auto clearStep = static_cast<float>(1.0 / std::max(1.0, kClearFadeSeconds * sampleRate_));
	const auto freezeStep = static_cast<float>(1.0 / std::max(1.0, kFreezeRampSeconds * sampleRate_));
	const float freezeTarget = s.freeze ? 1.0f : 0.0f;
	const auto limitAttack = static_cast<float>(1.0 - std::exp(-1.0 / std::max(1.0, kLoopLimitAttackSeconds * sampleRate_)));
	const auto limitRelease = static_cast<float>(1.0 - std::exp(-1.0 / std::max(1.0, kLoopLimitReleaseSeconds * sampleRate_)));
	if (firstBlock) {
		// AND A PATCH THAT LOADS FROZEN IS FROZEN FROM ITS FIRST SAMPLE — over an empty loop, with no ramp letting the first
		// 10 ms of input in (the audio is not persisted; the switch is).
		freezeNow_ = freezeTarget;
	}

	// **THE TAP, RESOLVED ONCE PER BLOCK.** Not per sample: the controls themselves only move once
	// per `device::kTickWindow`, so re-deriving under them would be arithmetic for a value that
	// cannot have changed.
	const double tapSec = tapSeconds(s, clock_, notePitch_);
	const double tapSecRight = std::clamp(tapSec * (1.0 + kSpreadFraction * static_cast<double>(s.spread)),
	                                      kMinDelaySeconds, kMaxDelaySeconds);
	lastTapSeconds_ = tapSec;
	lastTapSecondsRight_ = tapSecRight;

	// **REVERSE READS BACK TO TWICE ITS CHUNK**, so its tap stops at half the line (`kReverseMaxFraction`);
	// every other mode keeps the whole line, the same ceiling as before.
	const bool reverse = s.mode == Mode::Reverse;
	const double tapCeil = reverse ? (capFrames - 2.0) * kReverseMaxFraction : capFrames - 2.0;
	// **WHILE THE FEEDBACK LANE IS LIVE THE TAP CANNOT BE SHORTER THAN THE LANE IS LATE, PLUS A CHUNK** (decision 5):
	// the compensation writes each return `latency` back, so a tap inside that would be a loop with no valid chunk.
	// FROZEN, OR STILL OPENING FROM A FREEZE, THE LANE IS OUT OF THE LOOP (decision 6: a true hold).
	const bool live = insert_.process != nullptr && !s.freeze && freezeNow_ == 0.0f;
	const double tapFloor = live ? static_cast<double>(insert_.latency + static_cast<uint32_t>(kLoopChunkMin)) : 1.0;
	const double tapFramesL = std::clamp(tapSec * sampleRate_, std::min(tapFloor, tapCeil), tapCeil);
	const double tapFramesR = std::clamp(tapSecRight * sampleRate_, std::min(tapFloor, tapCeil), tapCeil);
	lastSpreadFrames_ = tapFramesR - tapFramesL;
	if (reverse) {
		// WHAT THE CHUNK IS AFTER ITS OWN CLAMP — the tap the device runs, which is what this seam reports.
		lastTapSeconds_ = tapFramesL / sampleRate_;
		lastTapSecondsRight_ = tapFramesR / sampleRate_;
		if (!revPlaced_) {
			// **SEATED WHERE THE LINE IS NOW**: the leading head's chunk starts here, and the trailing head is
			// half a chunk into the previous one — at its full weight, reading the half-chunk just recorded.
			const auto now = static_cast<double>(writeCounter_);
			const double chunk[2] = { tapNowL_ < 0.0 ? tapFramesL : std::min(tapNowL_, tapCeil),
				                      tapNowR_ < 0.0 ? tapFramesR : std::min(tapNowR_, tapCeil) };
			for (int c = 0; c < 2; ++c) {
				revMirrorA_[c] = now;
				revMirrorB_[c] = now - 0.5 * chunk[c];
				revHalfDone_[c] = false;
			}
			revPlaced_ = true;
		}
	}
	else {
		revPlaced_ = false;
	}

	// **HOW THE HEAD GETS FROM WHERE IT IS TO WHERE IT HAS BEEN ASKED TO BE**, which is a property
	// of the Mode. Clean and Granular GLIDE, so the material in flight bends in pitch — the tape
	// behaviour, and the default. Xfade jumps the head and fades between the two, so the time
	// changes without bending.
	const bool crossfading = s.mode == Mode::Xfade;
	if (tapNowL_ < 0.0) {
		// FIRST BLOCK SINCE A RESET: arrive rather than travel.
		tapNowL_ = tapFramesL;
		tapNowR_ = tapFramesR;
	}
	else if (crossfading) {
		// **A FADE STARTS ONLY ON A REAL MOVE**, or a knob resting a hair off its last value would
		// restart the fade every block and the delay would never stop crossfading with itself. Half
		// a frame is below what the interpolator can express.
		if (std::abs(tapFramesL - tapNowL_) > 0.5 || std::abs(tapFramesR - tapNowR_) > 0.5) {
			tapFadeFromL_ = tapNowL_;
			tapFadeFromR_ = tapNowR_;
			tapNowL_ = tapFramesL;
			tapNowR_ = tapFramesR;
			fadePos_ = 0.0;
			fadeInc_ = 1.0 / std::max(1.0, kXfadeSeconds * sampleRate_);
		}
	}
	const double glideCoeff =
	    crossfading ? 0.0 : 1.0 - std::exp(-1.0 / std::max(1.0, kGlideSeconds * sampleRate_));

	// GRANULAR IFF THE MODE SAYS SO **AND** GRAIN SIZE IS OFF ITS BOTTOM STOP — the `Unit::LogSeconds`
	// convention, honoured rather than merely declared. See `kGrainSizeLog2Min`.
	const bool granular = s.mode == Mode::Granular && s.grainSeconds > 0.0;
	// SWARM (2026-11-05): smaller grains, more of them, and the overlap normalised to what then sounds. At 0 every line
	// here is the expression it was.
	swarm_ = granular ? s.swarm : 0.0f;
	const double swarmShrink = swarm_ > 0.0f ? 1.0 - kSwarmShrink * static_cast<double>(swarm_) : 1.0;
	const double swarmDensity = swarm_ > 0.0f ? 1.0 + kSwarmDensityGain * static_cast<double>(swarm_) : 1.0;
	const double grainFrames = swarm_ > 0.0f ? s.grainSeconds * swarmShrink * sampleRate_ : s.grainSeconds * sampleRate_;
	const double spawnPeriod = swarm_ > 0.0f ? sampleRate_ / std::max(0.01, s.densityHz * swarmDensity)
	                                         : sampleRate_ / std::max(0.01, s.densityHz);
	const double pitchRatio = std::exp2(static_cast<double>(s.pitchSemitones) / 12.0);
	const double sprayFrames = static_cast<double>(s.spray) * kSprayFraction * tapFramesL;
	// **OVERLAP NORMALISATION.** `density * grainSeconds` grains sound at once, and summing them
	// raises the level with a control that is not a level. The square root is the incoherent-sum
	// answer: grains at different read positions are uncorrelated, so their powers add, not their
	// amplitudes. Never boosts — an overlap below one is a gap, not a deficit to make up.
	const auto grainGain =
	    swarm_ > 0.0f
	        ? static_cast<float>(1.0 / std::sqrt(std::max(1.0, s.densityHz * swarmDensity * s.grainSeconds * swarmShrink)))
	        : static_cast<float>(1.0 / std::sqrt(std::max(1.0, s.densityHz * s.grainSeconds)));
	// **THE GRANULATOR ON A PLAYING TRANSPORT IS THE SONG'S** (2026-11-05; CLAUDE.md, "determinism comes first"). Grain k
	// is born at the song frame where floor(n / spawnPeriod) steps to k, and every draw it takes (the spray, and Swarm's
	// three) is a hash of k — so the same song position is the same grains to the bit, whatever the host's block size and
	// whenever playback started. AT EVERY SWARM, 0 included: Granular's own spray was the stream since reset until a
	// decision the same day. Stopped (or with no seconds timeline) it free-runs on `spawnAccum_` as before.
	// The frame is the host's position rounded to a whole sample, then counted — not re-rounded per sample.
	const bool songLocked = granular && clock_.playing && clock_.havePositionSeconds();
	grainsOnSong_ = songLocked;
	const auto songFrame0 = songLocked ? static_cast<int64_t>(std::llround(clock_.songPositionSeconds * sampleRate_)) : 0;
	const auto songGrainOf = [spawnPeriod](int64_t n) {
		return static_cast<int64_t>(std::floor(static_cast<double>(n) / spawnPeriod));
	};

	// THE TONE TILT. One pole each way, both always run; the inactive one sits at a passthrough
	// coefficient so a sweep through zero has no discontinuity at the crossing.
	const double tone = static_cast<double>(s.tone);
	const float lpA = onePole(tone < 0.0 ? 20000.0 * std::pow(0.01, -tone) : 0.0, sampleRate_);
	const float hpA = onePole(tone > 0.0 ? 20.0 * std::pow(100.0, tone) : 0.0, sampleRate_);
	// `onePole(0)` is 1, which for the LOWPASS means "never move" — the wrong end. A tone at or above
	// zero wants the lowpass wide open, which is a coefficient of 0.
	const float lpCoeff = tone < 0.0 ? lpA : 0.0f;
	const float hpCoeff = hpA;

	// **THE TAPE MACHINE, RESOLVED ONCE PER BLOCK.** Everything here is zero or identity outside
	// Tape mode, so the other three modes pay a branch and nothing else.
	const bool tape = s.mode == Mode::Tape;
	constexpr double kTwoPi = 6.28318530717958647692;
	// **THE CONTROL IS A PITCH DEVIATION; THE MODULATION IS A POSITION.** A head offset of
	// `A·sin(2πft)` bends the pitch by `A·2πf/sr`, so each component's depth in FRAMES is solved from
	// the deviation it is asked for — `A = deviation·sr / 2πf`. A fixed frame depth would wobble the
	// pitch harder the faster it ran, and Wow and Flutter would stop being comparable. Each control's
	// deviation is split evenly across its two incommensurate components.
	const double wowDev = tape ? 0.5 * static_cast<double>(s.wow) * kWowMaxDeviation : 0.0;
	const double flutterDev = tape ? 0.5 * static_cast<double>(s.flutter) * kFlutterMaxDeviation : 0.0;
	const double wowFramesA = wowDev * sampleRate_ / (kTwoPi * kWowHzA);
	const double wowFramesB = wowDev * sampleRate_ / (kTwoPi * kWowHzB);
	const double flutterFramesA = flutterDev * sampleRate_ / (kTwoPi * kFlutterHzA);
	const double flutterFramesB = flutterDev * sampleRate_ / (kTwoPi * kFlutterHzB);
	const double wowIncA = kWowHzA / sampleRate_;
	const double wowIncB = kWowHzB / sampleRate_;
	const double flutterIncA = kFlutterHzA / sampleRate_;
	const double flutterIncB = kFlutterHzB / sampleRate_;
	// THE PER-PASS HF LOSS: a geometric sweep from wide open down to `kHfLossMinHz`, so equal knob
	// travel is equal octaves. Zero outside Tape, which for a lowpass coefficient means wide open.
	const float tapeLpCoeff =
	    tape ? onePole(kHfLossMaxHz * std::pow(kHfLossMinHz / kHfLossMaxHz,
	                                           static_cast<double>(s.hfLoss)),
	                   sampleRate_)
	         : 0.0f;
	const float tapeDrive = tape ? s.saturation * static_cast<float>(kTapeMaxDrive) : 0.0f;

	// **THE MULTI-TAP'S GEOMETRY, RESOLVED ONCE PER BLOCK.** Where each tap sits as a fraction of
	// the base, and what it weighs in each channel. The sample loop multiplies and adds.
	const bool multiTap = s.mode == Mode::MultiTap;
	const int tapCount = std::clamp(s.taps, kMinTaps, kMaxTaps);
	// MEMBERS, NOT LOCALS, so the block's end can publish what it ran (backlog LF). Outside Multi-Tap they
	// describe the one tap every other mode reads: at the head, at unity.
	auto& tapFrac = tapFrac_;
	auto& tapGainL = tapGainL_;
	auto& tapGainR = tapGainR_;
	tapFrac.fill(0.0);
	tapGainL.fill(0.0f);
	tapGainR.fill(0.0f);
	float tapNorm = 1.0f;
	tapCount_ = multiTap ? tapCount : 1;
	if (!multiTap) {
		tapFrac[0] = 1.0;
		tapGainL[0] = 1.0f;
		tapGainR[0] = 1.0f;
	}
	if (multiTap) {
		// **WHERE EACH TAP FALLS AND HOW LOUD IT IS: a family and a position each** (backlog MH, `delay_types.h`). The
		// LAST tap is at the full time in every family, which keeps `Sync` and `Time` meaning the pattern's length;
		// zone 0 of each is the old Shape's `(k/N)^(2^shape)` and the old Slope's linear run, by the same expressions.
		const TapZone pattern = decodeTapCoord(s.tapPattern);
		const TapZone levels = decodeTapCoord(s.tapLevels);
		float energyL = 0.0f;
		float energyR = 0.0f;
		for (int k = 0; k < tapCount; ++k) {
			tapFrac[static_cast<size_t>(k)] = tapFractionOf(pattern, k, tapCount);
			const float gain = tapLevelOf(levels, k, tapCount);
			// SUCCESSIVE TAPS ALTERNATE SIDES, by `Width`. A linear balance rather than equal-power:
			// the taps do not overlap in time, so there is no centre build-up to compensate.
			const float pan = s.tapWidth * ((k % 2) == 0 ? -1.0f : 1.0f);
			const float gl = gain * (pan > 0.0f ? 1.0f - pan : 1.0f);
			const float gr = gain * (pan < 0.0f ? 1.0f + pan : 1.0f);
			tapGainL[static_cast<size_t>(k)] = gl;
			tapGainR[static_cast<size_t>(k)] = gr;
			energyL += gl * gl;
			energyR += gr * gr;
		}
		// **`Taps` IS NOT A LEVEL CONTROL, SO IT MUST NOT ACT AS ONE** — the grain overlap's argument.
		// On sustained material N taps of different delays are uncorrelated and their POWERS add, so
		// the sum is scaled by the root of the louder channel's energy. The cost, stated: on sparse
		// material where taps never overlap, each tap is heard below a single tap's level. A feel
		// decision, to be revisited by ear.
		tapNorm = 1.0f / std::sqrt(std::max(1.0e-6f, std::max(energyL, energyR)));
	}
	tapNorm_ = tapNorm;

	// **WHAT THE CONTROLS ASK FOR IS NOT WHERE THE TAPS GO THIS BLOCK** (backlog MD). Each slot is told its new
	// place and gains; a slot that moves crossfades there, and every gain ramps. Slot `N-1-k` is tap k (`delay.h`).
	const double slotHead = std::max(tapNowL_, tapNowR_);
	if (multiTap && !slotsPlaced_) {
		// ENTERING THE MODE (or the first block): arrive rather than travel.
		for (size_t j = 0; j < slots_.size(); ++j) {
			Slot& slot = slots_[j];
			const bool sounding = j < static_cast<size_t>(tapCount);
			const size_t k = sounding ? static_cast<size_t>(tapCount) - 1 - j : 0;
			slot.frac = sounding ? tapFrac[k] : 1.0;
			slot.fracFrom = slot.frac;
			slot.target = slot.frac;
			slot.fadePos = 1.0;
			slot.gain[0].snap(sounding ? tapGainL[k] : 0.0f);
			slot.gain[1].snap(sounding ? tapGainR[k] : 0.0f);
		}
		tapNormRamp_.snap(tapNorm);
		slotsPlaced_ = true;
	}
	else if (multiTap) {
		for (size_t j = 0; j < slots_.size(); ++j) {
			Slot& slot = slots_[j];
			if (j >= static_cast<size_t>(tapCount)) {
				// GOING (or gone): it fades out where it stands.
				slot.gain[0].retarget(0.0f, tapFadeFrames_);
				slot.gain[1].retarget(0.0f, tapFadeFrames_);
				continue;
			}
			const size_t k = static_cast<size_t>(tapCount) - 1 - j;
			slot.target = tapFrac[k];
			if (slot.silent()) {
				// A TAP NOBODY HEARS MOVES FOR FREE: seated where it is asked, then faded in.
				slot.frac = slot.target;
				slot.fracFrom = slot.target;
			}
			else {
				startSlotFade(slot, slotHead);
			}
			slot.gain[0].retarget(tapGainL[k], tapFadeFrames_);
			slot.gain[1].retarget(tapGainR[k], tapFadeFrames_);
		}
		tapNormRamp_.retarget(tapNorm, tapFadeFrames_);
	}
	else {
		slotsPlaced_ = false;
	}

	const float mix = s.mix;
	const float feedback = s.feedback;
	// **DUB SWAPS WHICH END OF THE WET PATH `Mix` GOVERNS.** Off, it is a crossfade at the OUTPUT:
	// dry and wet trade against each other and the two always sum to one signal's worth. On, it is a
	// SEND at the INPUT — it decides how much reaches the line at all — and the dry passes at full
	// level beside it.
	//
	// **THAT IS WHY THE DUB MOVE WORKS AND A CROSSFADE CANNOT IMITATE IT AT ANY SETTING.** Gating
	// the input leaves everything ALREADY IN THE LINE untouched, so pulling the send to zero lets
	// the phrase wash out through its own feedback while the dry carries on dry. A crossfade taken
	// to zero mutes the tail along with the send, because it is acting after the line rather than
	// before it.
	const float send = s.dub ? mix : 1.0f;
	const float dryGain = s.dub ? 1.0f : 1.0f - mix;
	const float wetGain = s.dub ? 1.0f : mix;

	// THE LANE'S CHUNK IS CUT ON THE SAMPLE STREAM, NOT ON THE HOST'S BLOCKS: one left open at a block's end carries into the next,
	// so where a chunk ends — and so which near reads see a slice finished — cannot depend on how the host cut the audio.
	// A chunk still open when the lane goes away is finished without it.
	if (!live) {
		lanePass_ = 1.0f;
		laneInEnergy_ = 0.0;
		laneOutEnergy_ = 0.0;
	}
	if (!live && chunkN_ > 0) {
		flushLane(chunkAt_);
		chunkAt_ = 0;
		chunkN_ = 0;
	}
	// **THE BUCKET BRIGADE, RESOLVED ONCE PER BLOCK** (2026-11-05): each channel's clock from the head it is reading at, and
	// the one corner both its filters sit at — `bbdCutoffHz`. A Time that moves moves the head (it glides), so the clock
	// and the corner follow it block by block, and the pitch bends.
	const bool bbd = s.mode == Mode::Bbd;
	float bbdExpand = 0.0f;
	float bbdEnvAttack = 0.0f;
	float bbdEnvRelease = 0.0f;
	float bbdSatDrive = 0.0f;
	if (bbd) {
		for (int c = 0; c < 2; ++c) {
			const double seconds = std::max(1.0, c == 0 ? tapNowL_ : tapNowR_) / sampleRate_;
			bbdClockHz_[c] = static_cast<double>(s.bbdStages) / (2.0 * seconds);
			bbdCutoffHz_[c] = bbdCutoffHz(s.bbdStages, seconds, sampleRate_);
			// RBJ LOW-PASS, Q = 1/sqrt(2): Butterworth, 12 dB an octave each — two of them around the chip.
			const double w0 = 2.0 * 3.14159265358979323846 * bbdCutoffHz_[c] / sampleRate_;
			const double alpha = std::sin(w0) / (2.0 * 0.70710678118654752);
			const double cw = std::cos(w0);
			const double a0 = 1.0 + alpha;
			Biquad& q = bbdCoef_[static_cast<size_t>(c)];
			q.b0 = static_cast<float>((1.0 - cw) * 0.5 / a0);
			q.b1 = static_cast<float>((1.0 - cw) / a0);
			q.b2 = q.b0;
			q.a1 = static_cast<float>(-2.0 * cw / a0);
			q.a2 = static_cast<float>((1.0 - alpha) / a0);
			// THE WHINE'S PHASOR, ANCHORED AT THE EXACT PHASE ONCE A BLOCK (backlog PV): the clock is constant through
			// the block, so the frame's sine is a rotation by a constant, and the anchor keeps the drift to one block's.
			bbdWhine_[static_cast<size_t>(c)].anchor(bbdPhase_[static_cast<size_t>(c)], bbdClockHz_[c] / sampleRate_);
		}
		// THE EXPANDER UNDOES THE COMPRESSOR AT STEADY STATE: a squeeze of c/2 in, an exponent c/(2 − c) out.
		const float c = s.bbdCompander;
		bbdExpand = c / (2.0f - c);
		bbdEnvAttack = dsp::compander::coefFor(kBbdCompanderAttackSeconds, sampleRate_);
		bbdEnvRelease = dsp::compander::coefFor(kBbdCompanderReleaseSeconds, sampleRate_);
		bbdSatDrive = static_cast<float>(static_cast<double>(s.bbdDrive) * kBbdMaxDrive);
	}
	// THE CHIP'S INPUT: saturate, compress (the compressor's follower is linked across the pair), anti-alias.
	const BbdRecord bbdNow { bbdSatDrive, bbdEnvAttack, bbdEnvRelease, s.bbdCompander };
	const auto bbdIn = [&](float& l, float& r) { bbdRecord(l, r, bbdNow); };

	// A CHUNK IS CUT AT ITS FIRST SAMPLE (`chunkFor`), and what it will be flushed with is taken there too.
	for (int i = 0; i < frames; ++i) {
		const float inL = left[i];
		const float inR = right[i];
		// THE DUCK (delay-love S4): the follower always runs; only a Duck off centre reads it, so 0 is the old device.
		{
			const float level = std::max(std::abs(inL), std::abs(inR));
			duckEnv_ += (level - duckEnv_) * (level > duckEnv_ ? duckAttack : duckRelease);
			if (duckEnv_ < kSilenceFloor) {
				duckEnv_ = 0.0f;
			}
		}
		float wetScale = 1.0f;
		if (s.duck != 0.0f) {
			// ENGAGED ON A dB SCALE: none below `kDuckFloorDb`, all from `kDuckFullDb` — so a release ends EXACTLY, at the
			// floor, rather than creeping back over the follower's exponential tail.
			const float db = duckEnv_ > 0.0f ? 20.0f * std::log10(duckEnv_) : kDuckFloorDb;
			const float m = std::clamp((db - kDuckFloorDb) / (kDuckFullDb - kDuckFloorDb), 0.0f, 1.0f);
			const float a = std::abs(s.duck);
			wetScale = s.duck > 0.0f ? 1.0f - a * m : (1.0f - a) + a * m;
		}
		lastDuckGain_ = wetScale;
		if (clearFading_) {
			clearFade_ = std::max(0.0f, clearFade_ - clearStep);
			wetScale *= clearFade_;
		}
		// THE FREEZE'S RAMP, and its tap latched to the frame the moment it starts closing.
		if (freezeNow_ != freezeTarget) {
			freezeNow_ = freezeTarget > freezeNow_ ? std::min(1.0f, freezeNow_ + freezeStep)
			                                       : std::max(0.0f, freezeNow_ - freezeStep);
		}
		if (freezeNow_ > 0.0f && frozenL_ == 0) {
			const auto most = static_cast<double>(line_.length() - 1);
			frozenL_ = static_cast<size_t>(std::clamp(std::round(tapNowL_), 1.0, most));
			frozenR_ = static_cast<size_t>(std::clamp(std::round(tapNowR_), 1.0, most));
		}
		else if (freezeNow_ == 0.0f) {
			frozenL_ = 0;
			frozenR_ = 0;
		}
		if (live && chunkN_ == 0) {
			// THE NEAREST HEAD THAT FEEDS THE LOOP: the placed heads, the old one too while an Xfade runs, and the
			// tape's wobble at its widest. Granular's grains and Multi-Tap's early taps are heard reads (`LoopInsert`).
			double nearest = std::min(tapNowL_, tapNowR_);
			if (crossfading && fadePos_ < 1.0) {
				nearest = std::min({ nearest, tapFadeFromL_, tapFadeFromR_ });
			}
			if (tape) {
				nearest *= 1.0 - (kWowMaxDeviation + kFlutterMaxDeviation);
			}
			laneCap_ = bbd ? kLoopChunkWide : kLoopChunkMax;
			chunkN_ = chunkFor(nearest, laneCap_);
			chunkLatency_ = insert_.latency;
			chunkFeedback_ = feedback;
			chunkPingPong_ = s.pingPong != PingPong::Off;
			chunkTape_ = tape;
			chunkDrive_ = tapeDrive;
			chunkBbd_ = bbd;
			chunkBbdRecord_ = bbdNow;
		}

		float wetL = 0.0f;
		float wetR = 0.0f;
		// **WHAT ENTERS THE LOOP, WHICH IS NO LONGER ALWAYS WHAT IS HEARD.** In every mode but one
		// the two are the same signal; the multi-tap hears a sum of taps and recirculates only the
		// last of them, so the loop needs a source of its own.
		float loopL = 0.0f;
		float loopR = 0.0f;
		if (granular) {
			if (songLocked) {
				const int64_t n = songFrame0 + i;
				const int64_t k = songGrainOf(n);
				if (k != songGrainOf(n - 1) && activeGrains_ < kMaxGrains) {
					spawnGrain(tapNowL_, sprayFrames, pitchRatio, grainFrames, k);
					++songSpawns_;
					songLastIndex_ = k;
				}
			}
			else {
				spawnAccum_ += 1.0;
				while (spawnAccum_ >= spawnPeriod && activeGrains_ < kMaxGrains) {
					spawnAccum_ -= spawnPeriod;
					// FROM THE HEAD THAT IS ACTUALLY PLACED, not the one asked for — or a glide
					// would move the clean tap while the grains kept spawning at the destination.
					spawnGrain(tapNowL_, sprayFrames, pitchRatio, grainFrames);
				}
				if (spawnAccum_ >= spawnPeriod) {
					// THE POOL IS FULL: DROP THE BACKLOG RATHER THAN CARRY IT. Carried, the accumulator
					// grows without bound and every freed slot is instantly refilled from a queue that is
					// minutes old — a device that never recovers from one dense moment.
					spawnAccum_ = 0.0;
				}
			}
			double gL = 0.0;
			double gR = 0.0;
			renderGrains(&gL, &gR);
			wetL = static_cast<float>(gL) * grainGain;
			wetR = static_cast<float>(gR) * grainGain;
			loopL = wetL;
			loopR = wetR;
			// WHERE THE GRAINS SPAWN, which is the granular tap: the placed left head, and the right
			// channel's reads `lastSpreadFrames_` behind it (`renderGrains`).
			lastHeadL_ = tapNowL_;
			lastHeadR_ = tapNowL_ + lastSpreadFrames_;
			lastFadeNew_ = 1.0f;
			lastFadeOld_ = 0.0f;
		}
		else {
			// **WOW AND FLUTTER LIVE ON THE READ POSITION, NOT ON THE OUTPUT — and that placement is
			// the whole character.** Because the wobbled read is what feeds the loop, every pass
			// acquires its own: repeat five is markedly less steady than repeat one, which is what
			// a real transport does and what an output-side vibrato cannot. It falls out of the
			// routing, the same way the granular tail disintegrates.
			//
			// ONE TRANSPORT, SO ONE WOBBLE FOR BOTH CHANNELS — a stereo machine's two tracks ride
			// the same capstan. Decorrelating them would be wider and would not be tape.
			double wobble = 0.0;
			if (tape) {
				wobble = wowFramesA * std::sin(kTwoPi * wowPhaseA_)
				         + wowFramesB * std::sin(kTwoPi * wowPhaseB_)
				         + flutterFramesA * std::sin(kTwoPi * flutterPhaseA_)
				         + flutterFramesB * std::sin(kTwoPi * flutterPhaseB_);
				wowPhaseA_ += wowIncA;
				wowPhaseB_ += wowIncB;
				flutterPhaseA_ += flutterIncA;
				flutterPhaseB_ += flutterIncB;
				wowPhaseA_ -= std::floor(wowPhaseA_);
				wowPhaseB_ -= std::floor(wowPhaseB_);
				flutterPhaseA_ -= std::floor(flutterPhaseA_);
				flutterPhaseB_ -= std::floor(flutterPhaseB_);
			}
			const double headL = std::clamp(tapNowL_ + wobble, 1.0, capFrames - 2.0);
			const double headR = std::clamp(tapNowR_ + wobble, 1.0, capFrames - 2.0);
			lastHeadL_ = headL;
			lastHeadR_ = headR;
			lastFadeNew_ = 1.0f;
			lastFadeOld_ = 0.0f;
			if (reverse) {
				// **THE CHUNK IS THE PLACED HEAD**, so a glide stretches the chunks as it moves the loop and the two
				// stay one length — the condition for every repeat landing where the first did in its chunk.
				const auto now = static_cast<double>(writeCounter_);
				const double chunkL = std::min(headL, tapCeil);
				const double chunkR = std::min(headR, tapCeil);
				lastHeadL_ = chunkL;
				lastHeadR_ = chunkR;
				wetL = reverseRead(0, chunkL, now);
				wetR = reverseRead(1, chunkR, now);
				// THE LOOP IS THE FORWARD READ AT THE CHUNK, NOT WHAT IS HEARD (`delay_types.h`): heard, the next
				// pass would reverse the reversed repeat back to forwards.
				float fl = 0.0f;
				float fr = 0.0f;
				float gl = 0.0f;
				float gr = 0.0f;
				readLerp(now - chunkL, &fl, &fr);
				readLerp(now - chunkR, &gl, &gr);
				loopL = fl;
				loopR = gr;
			}
			else if (multiTap) {
				// **EVERY TAP IS A FRACTION OF THE HEAD THAT IS ACTUALLY PLACED**, so a glide moves
				// the whole pattern together and it stays a pattern while it bends.
				const auto now = static_cast<double>(writeCounter_);
				// **NO SPREAD, ONE READ**: the two heads are one place, and one read carries both channels.
				const bool oneRead = headL == headR;
				float accL = 0.0f;
				float accR = 0.0f;
				// TAP k OF N IS SLOT N-1-k, summed in tap order from the earliest; then any slot still fading out.
				for (int k = 0; k < kMaxTaps; ++k) {
					const auto j = static_cast<size_t>(k < tapCount ? tapCount - 1 - k : k);
					Slot& slot = slots_[j];
					if (k >= tapCount && slot.silent()) {
						continue;
					}
					float a = 0.0f;
					float b = 0.0f;
					float c = 0.0f;
					float e = 0.0f;
					readTap(now - std::max(1.0, headL * slot.frac), &a, &b);
					if (oneRead) {
						e = b;
					}
					else {
						readTap(now - std::max(1.0, headR * slot.frac), &c, &e);
					}
					if (slot.fadePos < 1.0) {
						// **THE OLD PLACE, WHILE THE TAP CROSSFADES TO THE NEW ONE — LINEARLY, NOT `Xfade`'s
						// EQUAL POWER** (backlog MD, measured). A modulated Shape moves a tap a little every fade,
						// so the two reads are nearly the SAME signal, and equal-power weights swell it up to 3 dB
						// a fade: back to back, a 100 Hz ripple. A slow LFO left -19 dB of the output off the tone
						// with equal power against -36 dB before MD and -39 dB with this. A linear crossfade of two
						// near-identical reads is interpolation. The cost lands on a big jump, where the reads
						// are unrelated: a 3 dB dip in that one tap for 10 ms.
						float oa = 0.0f;
						float ob = 0.0f;
						float oc = 0.0f;
						float oe = 0.0f;
						readTap(now - std::max(1.0, headL * slot.fracFrom), &oa, &ob);
						if (oneRead) {
							oe = ob;
						}
						else {
							readTap(now - std::max(1.0, headR * slot.fracFrom), &oc, &oe);
						}
						const auto w = static_cast<float>(slot.fadePos);
						const float gNew = w;
						const float gOld = 1.0f - w;
						a = a * gNew + oa * gOld;
						e = e * gNew + oe * gOld;
						slot.fadePos += tapFadeInc_;
						if (slot.fadePos >= 1.0 - 0.5 * tapFadeInc_) {
							// LANDED: from here it reads exactly the new place — and it takes the next move at
							// once, if the controls asked for one while this fade ran.
							slot.fadePos = 1.0;
							slot.fracFrom = slot.frac;
							startSlotFade(slot, slotHead);
						}
					}
					accL += a * slot.gain[0].now;
					accR += e * slot.gain[1].now;
					slot.gain[0].advance();
					slot.gain[1].advance();
					if (j == 0) {
						// **THE LOOP IS FED FROM THE BASE TAP'S RAW READ — not from the sum, and not
						// through the tap's own gain.** From the sum, every tap re-taps and cycle
						// two is N*N echoes; through the gain, `Slope` at full would starve the
						// repeats. This way the pattern REPEATS and `Feedback` alone owns the loop.
						// The base tap is slot 0, which sits at the head for every Shape and Taps.
						loopL = a;
						loopR = e;
					}
				}
				wetL = accL * tapNormRamp_.now;
				wetR = accR * tapNormRamp_.now;
				tapNormRamp_.advance();
			}
			else {
			readLerp(static_cast<double>(writeCounter_) - headL, &wetL, &wetR);
			float dl = 0.0f;
			float dr = 0.0f;
			readLerp(static_cast<double>(writeCounter_) - headR, &dl, &dr);
			wetR = dr;

			// **THE OLD HEAD, WHILE A CROSSFADE IS RUNNING.** Equal-power rather than linear: the
			// two heads are reading different material, so a linear pair would dip about 3 dB in
			// the middle of every time change. Two trig calls, and only during a fade.
			if (fadePos_ < 1.0) {
				float ol = 0.0f;
				float orr = 0.0f;
				readLerp(static_cast<double>(writeCounter_) - tapFadeFromL_, &ol, &orr);
				float or2 = 0.0f;
				float od = 0.0f;
				readLerp(static_cast<double>(writeCounter_) - tapFadeFromR_, &od, &or2);
				const auto angle = static_cast<float>(fadePos_) * 1.57079632679f;
				const float gNew = std::sin(angle);
				const float gOld = std::cos(angle);
				wetL = wetL * gNew + ol * gOld;
				wetR = wetR * gNew + or2 * gOld;
				lastFadeNew_ = gNew;
				lastFadeOld_ = gOld;
				fadePos_ = std::min(1.0, fadePos_ + fadeInc_);
			}
			if (bbd) {
				// **THE CHIP'S OUTPUT** (2026-11-05): the line sampled and held at the clock, so a slow clock steps and
				// aliases; the clock's whine riding on it; the reconstruction low-pass at the same corner; the expander.
				float* const wet[2] = { &wetL, &wetR };
				for (size_t c = 0; c < 2; ++c) {
					const double ratio = bbdClockHz_[c] / sampleRate_;
					bbdPhase_[c] += ratio;
					if (ratio >= 1.0 || bbdPhase_[c] >= 1.0) {
						bbdPhase_[c] -= std::floor(bbdPhase_[c]);
						bbdHeld_[c] = *wet[c];
					}
					float y = ratio >= 1.0 ? *wet[c] : bbdHeld_[c];
					if (s.bbdNoise > 0.0f && bbdClockHz_[c] < 0.5 * sampleRate_) {
						// sin(2π·bbdPhase_[c]) — the phasor, stepped only on this road, so a block without the whine
						// leaves it to the next block's anchor. Wrapping the phase by a whole cycle moves no sine.
						y += s.bbdNoise * kBbdWhineLevel * std::min(1.0f, bbdEnvCh_[c] / kBbdCompanderRef)
						     * static_cast<float>(bbdWhine_[c].step());
					}
					*wet[c] = biquad(bbdCoef_[c], bbdRe_[c], y);
					// EACH CHANNEL'S OWN LEVEL, which its noise rides on: a side the loop has not reached stays silent.
					const float mine = std::abs(*wet[c]);
					dsp::compander::follow(bbdEnvCh_[c], mine, bbdEnvAttack, bbdEnvRelease, kSilenceFloor);
					if (std::abs(bbdHeld_[c]) < kSilenceFloor) {
						bbdHeld_[c] = 0.0f;
					}
				}
				const float level = std::max(std::abs(wetL), std::abs(wetR));
				dsp::compander::follow(bbdEnvOut_, level, bbdEnvAttack, bbdEnvRelease, kSilenceFloor);
				if (s.bbdCompander > 0.0f) {
					const float g = dsp::compander::gain(bbdEnvOut_, kBbdCompanderRef, bbdExpand, 0.125f, 4.0f);
					wetL *= g;
					wetR *= g;
				}
			}
			loopL = wetL;
			loopR = wetR;
			if (bbd && s.bbdNoise > 0.0f) {
				// THE HISS, AFTER THE LOOP IS FED: heard, scaled by the expander's own envelope so it breathes with the
				// signal and stops with it — in the line it would recirculate for ever (`delay_types.h`).
				const float rideL = s.bbdNoise * kBbdHissLevel * std::min(1.0f, bbdEnvCh_[0] / kBbdCompanderRef);
				const float rideR = s.bbdNoise * kBbdHissLevel * std::min(1.0f, bbdEnvCh_[1] / kBbdCompanderRef);
				const float nl = nextBbdNoise();
				const float nr = nextBbdNoise();
				wetL += rideL * nl;
				wetR += rideR * nr;
			}
			} // single-tap
		}

		// **THE HEAD MOVES AFTER THE READ, one sample's worth, and the SPEED of this is the pitch.**
		// A one-pole approach, so the bend is strongest where the hand moved and eases out — a
		// linear ramp would bend by a constant amount and stop abruptly. Clamped per sample so a
		// violent jump bends hard and briefly instead of tearing: at 0.5 the read rate stays inside
		// [0.5x, 1.5x].
		//
		// **IN `Xfade` THE COEFFICIENT IS ZERO**, so the head does not travel at all — it was
		// already placed at the target above, and the fade is what covers the discontinuity.
		if (glideCoeff > 0.0) {
			const double stepL =
			    std::clamp((tapFramesL - tapNowL_) * glideCoeff, -kGlideMaxFramesPerSample,
			               kGlideMaxFramesPerSample);
			const double stepR =
			    std::clamp((tapFramesR - tapNowR_) * glideCoeff, -kGlideMaxFramesPerSample,
			               kGlideMaxFramesPerSample);
			tapNowL_ = std::clamp(tapNowL_ + stepL, 1.0, capFrames - 2.0);
			tapNowR_ = std::clamp(tapNowR_ + stepR, 1.0, capFrames - 2.0);
		}

		// THE TONE SITS ON THE FEEDBACK, NOT ON THE OUTPUT — Portal's HP and LP are described as
		// applying "to the feedback signal", and it is what makes successive repeats darken rather
		// than the first tap arriving already filtered.
		lpL_ += (1.0f - lpCoeff) * (loopL - lpL_);
		lpR_ += (1.0f - lpCoeff) * (loopR - lpR_);
		float fbL = lpL_;
		float fbR = lpR_;
		hpL_ += (1.0f - hpCoeff) * (fbL - hpL_);
		hpR_ += (1.0f - hpCoeff) * (fbR - hpR_);
		fbL -= hpL_;
		fbR -= hpR_;

		// **THE TAPE'S OWN TOP-END LOSS, PER PASS AND ON TOP OF `Tone`.** Tone is the player's
		// deliberate tilt; this is the machine's. Because it sits in the loop it is CUMULATIVE — the
		// fifth repeat has been through it five times — so the tail dissolves instead of fading as
		// a bright copy. Coefficient zero outside Tape, which is wide open.
		tapeLpL_ += (1.0f - tapeLpCoeff) * (fbL - tapeLpL_);
		tapeLpR_ += (1.0f - tapeLpCoeff) * (fbR - tapeLpR_);
		fbL = tapeLpL_;
		fbR = tapeLpR_;

		if (live) {
			// **THE LANE'S HALF OF THE SAMPLE: the return, before the swap, waits for the chunk's end; only the input
			// is written now** (`LoopInsert`). The flush below adds the feedback where it belongs.
			const bool classicNow = s.pingPong == PingPong::Classic;
			float rawL = (classicNow ? 0.5f * (inL + inR) : inL) * send;
			float rawR = (classicNow ? 0.0f : inR) * send;
			// IN BUCKETS THE INPUT IS WRITTEN RAW, AND THE CHIP RECORDS IT WITH ITS RETURN AT THE FLUSH (`flushLane`) — input
			// and feedback through the drive, the compressor and the anti-alias filter TOGETHER, as the plain loop records
			// them. (It used to chip the input alone here and add the returns unchipped; the read side's expander then
			// lifted every pass, and any lane, even one of a disabled device, sent the loop to the limiter..)
			laneL_[static_cast<size_t>(chunkAt_)] = fbL;
			laneR_[static_cast<size_t>(chunkAt_)] = fbR;
			if (fbL == 0.0f && fbR == 0.0f && rawL == 0.0f && rawR == 0.0f) {
				++laneQuiet_;
			}
			else {
				laneQuiet_ = 0;
			}
			// THE POLES' FLUSH, as below, or a residue keeps re-seeding the loop for ever.
			for (float* pole : { &lpL_, &lpR_, &hpL_, &hpR_, &tapeLpL_, &tapeLpR_ }) {
				if (std::abs(*pole) < kSilenceFloor) {
					*pole = 0.0f;
				}
			}
			line_.write(rawL, rawR);
			++writeCounter_;
			right[i] = inR * dryGain + wetR * wetScale * wetGain;
			left[i] = inL * dryGain + wetL * wetScale * wetGain;
			++chunkAt_;
			if (chunkAt_ == chunkN_) {
				flushLane(chunkN_);
				chunkAt_ = 0;
				chunkN_ = 0;
			}
			continue;
		}

		if (s.pingPong != PingPong::Off) {
			// ONLY THE FEEDBACK CROSSES. The first tap stays where it was recorded and every repeat
			// after it alternates, which is what "ping-pong" names.
			std::swap(fbL, fbR);
		}

		// THE SEND IS ON THE INPUT, NOT ON THE FEEDBACK — cutting it must not also cut what is
		// already circulating, which is the whole point of the mode.
		float wL = 0.0f;
		float wR = 0.0f;
		// **CLASSIC PING-PONG: THE INPUT ENTERS THE LEFT LINE ONLY, AS MONO** (`delay_types.h`), so the first repeat is
		// on the left and the crossed feedback carries every later one across — a centred sound bounces too. The
		// dry is untouched. Off and Stereo take the inputs as they are, which keeps them the samples they were.
		const bool classic = s.pingPong == PingPong::Classic;
		const float lineInL = classic ? 0.5f * (inL + inR) : inL;
		const float lineInR = classic ? 0.0f : inR;
		// **FEEDBACK PAST 110% GOES THROUGH THE LOOP LIMITER** (delay-love S6b, from Dedalus: to 200%, limited in the loop).
		// At or below 110% — every value the Delay had before — the product is the same float it always was, untouched.
		float fbScaledL = feedback * fbL;
		float fbScaledR = feedback * fbR;
		if (feedback > static_cast<float>(kFeedbackLimitAbove)) {
			const float peak = std::max(std::abs(fbScaledL), std::abs(fbScaledR));
			const float want = peak > kLoopLimitCeiling ? kLoopLimitCeiling / peak : 1.0f;
			limitGain_ += (want - limitGain_) * (want < limitGain_ ? limitAttack : limitRelease);
			if (std::abs(1.0f - limitGain_) < 1.0e-7f) {
				limitGain_ = 1.0f;
			}
			const float gain = std::min(limitGain_, want);
			fbScaledL *= gain;
			fbScaledR *= gain;
		}
		if (tape) {
			// **TAPE SATURATES WHAT IS RECORDED — THE INPUT AND THE FEEDBACK TOGETHER — not the
			// feedback alone.** That is where the record head is, and it is why a tape echo is a
			// saturated COPY rather than a clean one: the first repeat is already coloured, and
			// every pass goes through the same head again. The transparent safety clip still runs
			// over the result — identity wherever the saturation has done the job (its ceiling is
			// `1/drive`), and the bound where a low drive leaves that ceiling above full scale.
			wL = softClip(tapeSat(lineInL * send + fbScaledL, tapeDrive));
			wR = softClip(tapeSat(lineInR * send + fbScaledR, tapeDrive));
		}
		else if (bbd) {
			// THE CHIP RECORDS THE INPUT AND THE FEEDBACK TOGETHER (2026-11-05), as a pedal's loop does: through the
			// saturation, the compressor and the anti-alias filter, then the safety clip.
			float xl = lineInL * send + fbScaledL;
			float xr = lineInR * send + fbScaledR;
			bbdIn(xl, xr);
			wL = softClip(xl);
			wR = softClip(xr);
		}
		else {
			wL = lineInL * send + softClip(fbScaledL);
			wR = lineInR * send + softClip(fbScaledR);
		}

		// **THE FREEZE: THE LOOP HANDED TO A RAW READ AT THE LATCHED TAP** (delay-love S4). At full hold the write is the
		// held read itself — no input, no Tone, no clip, nothing lost — so the loop repeats to the bit for ever.
		if (freezeNow_ > 0.0f) {
			const size_t len = line_.length();
			float hL = 0.0f;
			float hR = 0.0f;
			float unused = 0.0f;
			line_.readAt((line_.writePos() + len - frozenL_) % len, &hL, &unused);
			line_.readAt((line_.writePos() + len - frozenR_) % len, &unused, &hR);
			wL = wL * (1.0f - freezeNow_) + hL * freezeNow_;
			wR = wR * (1.0f - freezeNow_) + hR * freezeNow_;
		}

		// **EXACT SILENCE, RATHER THAN THE EXEMPTION.** `test_fx_devices.cpp` requires every audio
		// device's tail to reach exact zero or be named as one that does not, and a geometric decay
		// reaches denormal-small forever. Writing a true zero once the sample is below audibility
		// means the line fills with exact zeros within one lap and everything downstream of it is
		// exactly zero after that — no O(n) flush, no special case, and nothing audible discarded at
		// -140 dBFS. It handles denormals on the way past.
		if (std::abs(wL) < kSilenceFloor) {
			wL = 0.0f;
		}
		if (std::abs(wR) < kSilenceFloor) {
			wR = 0.0f;
		}
		// THE FILTER POLES TOO, or a residue below the floor keeps re-seeding the line forever and the
		// tail never reaches the zero the paragraph above promises.
		if (std::abs(lpL_) < kSilenceFloor) {
			lpL_ = 0.0f;
		}
		if (std::abs(lpR_) < kSilenceFloor) {
			lpR_ = 0.0f;
		}
		if (std::abs(hpL_) < kSilenceFloor) {
			hpL_ = 0.0f;
		}
		if (std::abs(hpR_) < kSilenceFloor) {
			hpR_ = 0.0f;
		}
		if (std::abs(tapeLpL_) < kSilenceFloor) {
			tapeLpL_ = 0.0f;
		}
		if (std::abs(tapeLpR_) < kSilenceFloor) {
			tapeLpR_ = 0.0f;
		}

		if (wL == 0.0f && wR == 0.0f) {
			if (quietRun_ < line_.length()) {
				++quietRun_;
			}
		}
		else {
			quietRun_ = 0;
		}

		line_.write(wL, wR);
		++writeCounter_;

		// **THE RIGHT CHANNEL IS WRITTEN FIRST, AND THE ORDER IS LOAD-BEARING — see `processMono`.**
		// That road passes ONE buffer as both channels, so these two stores land in the same place
		// and the LAST one wins. Written the other way round the mono road returned the RIGHT
		// channel while claiming in its own comment to return the left, and every channel-separating
		// control — Spread, Ping-Pong — silently came out of the wrong side.
		//
		// The reads above are safe under that aliasing because both happen before either store, in
		// the same iteration.
		right[i] = inR * dryGain + wetR * wetScale * wetGain;
		left[i] = inL * dryGain + wetL * wetScale * wetGain;
	}

	// **THE TAPS STRIP'S READINGS (backlog LF): WHAT THIS BLOCK RAN, NOT WHAT THE CONTROLS ASKED FOR.**
	// The heads are the ones the last sample read at — glide, wobble and crossfade included — so a
	// modulated Time, the tape's wow and the grains' spray all move the strip. `delay_taps.h` has the
	// layout and how the card expands it.
	if (multiTap) {
		// **WHAT THE BLOCK RAN, NOT WHAT THE CONTROLS ASKED** (backlog MD): each tap where its slot is — the place it
		// is fading TO while a fade runs, which is where an impulse arriving now will be heard — at its ramped gain.
		for (size_t k = 0; k < static_cast<size_t>(tapCount); ++k) {
			const Slot& slot = slots_[static_cast<size_t>(tapCount) - 1 - k];
			tapFrac_[k] = slot.frac;
			tapGainL_[k] = slot.gain[0].now;
			tapGainR_[k] = slot.gain[1].now;
		}
		tapNorm_ = tapNormRamp_.now;
	}

	if (sink_.publish != nullptr) {
		namespace r = reading;
		std::array<float, kReadingCount> out {};
		const TapLayout layout = granular  ? TapLayout::Granular
		                         : multiTap ? TapLayout::MultiTap
		                         : reverse  ? TapLayout::Reverse
		                                    : TapLayout::Single;
		if (reverse) {
			// WHERE THE LEADING HEAD IS IN ITS CHUNK FOR THE NEXT SAMPLE — an impulse arriving now. One past the
			// end is the start of the next chunk, which is where `reverseRead` will put it.
			for (size_t c = 0; c < 2; ++c) {
				const double chunk = revChunk_[c];
				double phase = chunk > 0.0 ? (static_cast<double>(writeCounter_) - revMirrorA_[c]) / chunk : 0.0;
				if (phase >= 1.0) {
					phase -= 1.0;
				}
				out[c == 0 ? reading::kRevPhaseL : reading::kRevPhaseR] = static_cast<float>(phase);
			}
		}
		out[r::kLayout] = static_cast<float>(static_cast<int>(layout));
		out[r::kHeadL] = static_cast<float>(lastHeadL_ / sampleRate_);
		out[r::kHeadR] = static_cast<float>(lastHeadR_ / sampleRate_);
		out[r::kFadeFromL] = static_cast<float>(tapFadeFromL_ / sampleRate_);
		out[r::kFadeFromR] = static_cast<float>(tapFadeFromR_ / sampleRate_);
		out[r::kFadeNew] = lastFadeNew_;
		out[r::kFadeOld] = lastFadeOld_;
		// THE LOOP GAIN PER PASS AS HEARD: the Feedback, times what the lane does to a pass (1 without one).
		out[r::kFeedback] = feedback * lanePass_;
		out[r::kPingPong] = static_cast<float>(static_cast<int>(s.pingPong));
		// SEND TIMES WET — Mix either way: a crossfade's wet gain without Dub, the send into the line with it.
		out[r::kWetGain] = send * wetGain;
		out[r::kDryGain] = dryGain;
		out[r::kAxisUnit] = static_cast<float>(syncBaseSeconds(s, clock_, notePitch_));
		out[r::kTapCount] = static_cast<float>(tapCount_);
		for (size_t k = 0; k < static_cast<size_t>(kMaxTaps); ++k) {
			out[r::kTapFrac + k] = static_cast<float>(tapFrac_[k]);
			out[r::kTapGainL + k] = tapGainL_[k] * tapNorm_;
			out[r::kTapGainR + k] = tapGainR_[k] * tapNorm_;
		}
		if (granular) {
			// ONE PASS'S WINDOW: the spray either way, and the drift a pitched grain makes over its life —
			// a grain reading faster than the line is written walks toward the write head, so it lands
			// EARLIER the further up it is pitched, and later the further down.
			const double spraySec = sprayFrames / sampleRate_;
			// AND SWARM'S THROW (2026-11-05), so the whisker covers where the cloud can land.
			const double swarmSec = static_cast<double>(swarm_) * kSwarmSpreadSeconds;
			out[r::kPassEarly] = static_cast<float>(spraySec + swarmSec + std::max(0.0, pitchRatio - 1.0) * s.grainSeconds);
			out[r::kPassLate] = static_cast<float>(spraySec + swarmSec + std::max(0.0, 1.0 - pitchRatio) * s.grainSeconds);
			const size_t shown = std::min(activeGrains_, kGrainReadings);
			out[r::kGrainCount] = static_cast<float>(shown);
			out[r::kGrainSpread] = static_cast<float>(lastSpreadFrames_ / sampleRate_);
			const auto head = static_cast<double>(writeCounter_);
			for (size_t i = 0; i < shown; ++i) {
				const Grain& g = grains_[i];
				const double t = g.age / g.length;
				out[r::kGrainDelay + i] = static_cast<float>((head - g.readPos) / sampleRate_);
				out[r::kGrainWeight + i] = static_cast<float>(4.0 * t * (1.0 - t)) * grainGain;
			}
		}
		// THE LANE (delay-love S3): what it does to a pass, the tap it allows, the limiter.
		out[r::kLanePass] = lanePass_;
		out[r::kTapFloor] = live ? static_cast<float>(tapFloor / sampleRate_) : 0.0f;
		out[r::kLoopLimit] = limitGain_;
		sink_.publish(sink_.context, out.data(), out.size());
	}
}

void Delay::processMono(float* mono, int frames) noexcept {
	if (mono == nullptr || frames <= 0) {
		return;
	}
	// **THE STEREO ROAD OVER ONE BUFFER, which gives the LEFT channel only because `processStereo`
	// stores right-then-left.** One buffer as both channels means the two stores collide and the
	// last one survives; the loop is ordered so that the survivor is the left.
	//
	// **THIS COMMENT USED TO CLAIM THE RESULT WITHOUT THE MECHANISM, AND THE CLAIM WAS FALSE.** The
	// stores were left-then-right, so this returned the RIGHT channel — meaning Spread and
	// Ping-Pong, the two controls whose whole job is to separate the channels, came out of the
	// wrong one. It survived `test_fx_devices.cpp`'s mono sweep because that probe is far shorter
	// than a half-second tap: both channels were pure scaled dry and agreed trivially, and the
	// sweep's non-vacuity guard was satisfied by `Mix` scaling the dry signal rather than by the
	// delay being audible at all.
	//
	// Running a second, mono-shaped loop instead is the other option and is worse: two loops for
	// one sound is two places for it to drift, which is the objection `registry.h` raises against
	// exactly this.
	processStereo(mono, mono, frames);
}


} // namespace feathers::fx::delay
