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
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */


#pragma once

#include "fx/dyn/oversampler.h"
#include "fx/eroder/eroder_types.h"
#include "fx/eroder/shell.h"

#include "device/device.h"
#include "device/transport_lock.h"

#include <cmath>

namespace feathers::fx::eroder {

/// **THE DRIVE STAGE: A STEREO SIGNAL PUSHED INTO THE SHAPER `DriveSettings` NAMES.** Both engines run one, on their
/// wet: Dust on what its head reads, the Comb on its wet as it is about to be added to the dry. The curves
/// (`shapeDrive`) run four times oversampled, so what they add above the band is filtered off rather than folded back
/// into it; Crush, Decimate, Slew and Lopside are not curves and are not oversampled (their steps and edges are the
/// sound) and are not level-matched. Paunch-S and Paunch-H run oversampled, with no make-up. Tube's, Fold's, Lean's and the two Paunch laws' standing offsets are taken back out (Lean's opposite on
/// the two sides). Not run at all at the bottom of Drive, so there the signal is the bits it was.
///
/// **THE STAGE'S OWN MIX** (`DriveSettings::mix`): `dry + mix * (processed - dry)`. At 0 the stage is not run; at 1 the
/// blend is not computed, so the output is exactly what it is without one.
///
/// No allocation; a frame at a time, so a host that cuts its blocks anywhere gets the same bits.
inline constexpr double kDcGlideSeconds = 0.010;

/// **WHERE THE SONG PUTS THE ROTATION LFO — THE HOUSE RULE FOR A FREE LFO** (Phaser's Free rate, automod's `songPeriod`): on a PLAYING
/// transport that reports a position the phase FOLLOWS THE SONG, `position * rate` on the seconds timeline, by the song's own movement
/// (`device::TransportLock`) and stepping to it only at a jump (play, seek, loop), so the same passage played twice sounds the same; a
/// stopped transport is not a position (it would freeze the turn) and a host that says nothing leaves the phase running free from the stage's
/// reset. Asked once a control tick, at the tick's own offset in the block.
class RotationClock {
public:
	static constexpr int kSecondsRoad = 2;
	void setClock(const device::TransportClock& clock) noexcept { clock_ = clock; }
	/// THE PHASE (turns, 0..1) the song places the LFO at for a tick `offsetFrames` into the block, or -1 where the stage free-runs.
	[[nodiscard]] double place(double rateHz, int offsetFrames, double sampleRate) noexcept {
		if (!(rateHz > 0.0) || !std::isfinite(rateHz) || !clock_.playing || !clock_.havePositionSeconds()) {
			lock_.release();
			return -1.0;
		}
		const double rate = sampleRate > 0.0 ? sampleRate : 48000.0;
		const double turns = lock_.follow(clock_.songPositionSeconds + static_cast<double>(offsetFrames) / rate, 1.0 / rateHz,
		                                  static_cast<double>(kControlFrames) / rate, kSecondsRoad);
		const double phase = turns - std::floor(turns);
		if (!std::isfinite(phase) || phase >= 1.0) {
			lock_.release();
			return -1.0;
		}
		return phase;
	}

private:
	device::TransportClock clock_ {};
	device::TransportLock lock_ {};
};

class DriveStage {
public:
	void prepare(double sampleRate) noexcept {
		const double rate = sampleRate > 0.0 ? sampleRate : 48000.0;
		dcCoefficient_ = static_cast<float>(1.0 - std::exp(-2.0 * 3.141592653589793 * kDriveDcHz / rate));
		dcGlide_ = static_cast<float>(1.0 - std::exp(-1.0 / (kDcGlideSeconds * rate)));
		followCoefficient_ = paunchFollowCoefficient(4.0 * rate); // the follower runs on the oversampled frames
		trim_.prepare(rate);                                       // the level trim runs on the host frames
		hostRate_ = rate;
		slew_.prepare(rate);
		slewSet_ = false; // the slew was just turned off: the next settings turn it on
		reset();
	}

	void reset() noexcept {
		over_.reset();
		quiet_ = 0;
		crushPhase_ = 0.0;
		crushL_ = crushR_ = 0.0f;
		prevL_ = prevR_ = 0.0f;
		slew_.reset();
		dcL_ = dcR_ = 0.0f;
		fullL_ = fullR_ = 0.0f;
		dcNow_ = settings_.dcAmount;
		followPower_ = 0.0f;
		trim_.reset();
		rotPhase_ = 0.0;
	}

	/// WHERE THE SONG PLACES THE ROTATION LFO (turns), from the control tick; the stage advances it by its own rate a frame at a time.
	void placeRotation(double phase) noexcept { rotPhase_ = phase; }
	[[nodiscard]] double rotationPhaseForTest() const noexcept { return rotPhase_; }
	/// THE ROTATION LFO'S RATE as the stage runs it, in hertz (0 when off).
	[[nodiscard]] double rotationRateHzForTest() const noexcept { return rotInc_ * hostRate_; }

	/// TEST SEAMS: the trim's time constant in seconds (the shipped one is `kTrimSeconds`), the trim off (the stage as it was before it), the
	/// target ratio forced to 1, and the last k it found.
	void setTrimSecondsForTest(double seconds, double rate) noexcept { trim_.prepare(rate, seconds); }
	void setTrimOnForTest(bool on) noexcept { trimOn_ = on; }
	void setTrimUnityForTest(bool on) noexcept { trimUnity_ = on; }
	[[nodiscard]] float trimKForTest() const noexcept { return trim_.k; }

	/// THE STAGE'S OUTPUT AS IT WOULD BE WITH ALL OF ITS OWN OFFSET REMOVED (the last frame): what the loops' Driven and outer taps
	/// take, whatever the Drive's DC amount. Equal to the output at an amount of 1, and to it for a Type that makes no offset.
	void fullyBlocked(float& left, float& right) const noexcept {
		left = fullL_;
		right = fullR_;
	}

	/// THE SETTINGS FOR THE FRAMES THAT FOLLOW. A shaper that was resting, or another Type, starts from nothing rather
	/// than from what it last held.
	void set(const DriveSettings& settings) noexcept {
		const bool fresh = running(settings) && (!running(settings_) || settings_.mode != settings.mode);
		if (fresh) {
			reset();
		}
		settings_ = settings;
		rotInc_ = settings.rateHz > 0.0f && hostRate_ > 0.0 ? static_cast<double>(settings.rateHz) / hostRate_ : 0.0;
		if (fresh || !running(settings)) {
			dcNow_ = settings.dcAmount; // a stage that starts from nothing starts at its amount; a running one glides to it
		}
		if (running(settings)) {
			if (settings.slewHz != slewHz_ || settings.slewCurve != slewCurve_ || settings.slewBalance != slewBalance_ ||
			    !slewSet_) {
				slewHz_ = settings.slewHz;
				slewCurve_ = settings.slewCurve;
				slewBalance_ = settings.slewBalance;
				slewSet_ = true;
				SlewSettings slew;
				slew.on = true;
				slew.hz = slewHz_;
				slew.curve = slewCurve_;
				slew.balance = slewBalance_;
				slew_.set(slew);
			}
		}
	}

	/// WHETHER THE STAGE RUNS ON THE NEXT FRAME: its Drive is up and its mix is above zero. Otherwise the signal is untouched.
	[[nodiscard]] bool active() const noexcept { return running(settings_); }
	/// THE STAGE'S MIX ALONE, between control blocks (Etch scales a slot's by Follow a frame at a time). A stage that was
	/// not running starts from nothing when its mix comes up, as `set` does.
	void setMix(float mix) noexcept {
		const float clamped = std::clamp(mix, 0.0f, 1.0f);
		if (settings_.driven && clamped > 0.0f && !running(settings_)) {
			reset();
		}
		settings_.mix = clamped;
	}

	[[nodiscard]] const DriveSettings& settings() const noexcept { return settings_; }
	/// The stage's `Slew`, as it is set (a test reads the rate it was given).
	[[nodiscard]] const Slew& slewForTest() const noexcept { return slew_; }

	/// ONE FRAME, in place.
	void process(float& left, float& right) noexcept {
		const DriveSettings& s = settings_;
		if (!running(s)) {
			return;
		}
		if (s.mix >= 1.0f) {
			run(left, right);
			return;
		}
		const float dryL = left;
		const float dryR = right;
		run(left, right);
		left = dryL + s.mix * (left - dryL);
		right = dryR + s.mix * (right - dryR);
	}

private:
	/// Not run: at the bottom of Drive, or with none of the stage in the signal.
	[[nodiscard]] static bool running(const DriveSettings& s) noexcept { return s.driven && s.mix > 0.0f; }

	void run(float& left, float& right) noexcept {
		const DriveSettings& s = settings_;
		if (s.mode == DriveMode::Crush) {
			// HELD FOR A TIME, ROUNDED TO A STEP. A hold of one frame is every frame.
			crushPhase_ += 1.0;
			if (crushPhase_ >= s.crushHoldFrames) {
				crushPhase_ -= s.crushHoldFrames;
				crushL_ = std::round(left / s.crushStep) * s.crushStep;
				crushR_ = std::round(right / s.crushStep) * s.crushStep;
			}
			left = crushL_;
			right = crushR_;
			return;
		}
		if (s.mode == DriveMode::Decimate) {
			// HELD FOR A TIME, NOT ROUNDED. Shape 0 is the held value; Shape 1 a straight line from the previous held
			// value to the current one across the hold (a first-order hold: it lags by one hold period); between,
			// the two outputs blended.
			crushPhase_ += 1.0;
			if (crushPhase_ >= s.crushHoldFrames) {
				crushPhase_ -= s.crushHoldFrames;
				prevL_ = crushL_;
				prevR_ = crushR_;
				crushL_ = left;
				crushR_ = right;
			}
			if (s.shape <= 0.0f) {
				left = crushL_;
				right = crushR_;
				return;
			}
			const auto t = static_cast<float>(crushPhase_ / s.crushHoldFrames);
			const float lineL = prevL_ + (crushL_ - prevL_) * t;
			const float lineR = prevR_ + (crushR_ - prevR_) * t;
			if (s.shape >= 1.0f) {
				left = lineL;
				right = lineR;
				return;
			}
			left = crushL_ + s.shape * (lineL - crushL_);
			right = crushR_ + s.shape * (lineR - crushR_);
			return;
		}
		if (s.mode == DriveMode::Slew || s.mode == DriveMode::Lopside) {
			left = slew_.process(0, left);
			right = slew_.process(1, right);
			return;
		}
		const bool silentIn = left == 0.0f && right == 0.0f;
		const float lIn = left;
		const float rIn = right;
		const DriveMode mode = s.mode;
		const float gain = s.gain;
		const float shape = s.shape;
		const float norm = s.norm;
		// THE PAUNCH TYPES' ANGLE: Shape's, plus the rotation LFO's turns (none when its rate is off). The LFO advances a frame at a time.
		Rotation rot;
		if (driveIsPaunchOdd(mode) || driveIsPaunchLaw(mode)) {
			rot = rotationOf(shape, rotInc_ > 0.0 ? rotPhase_ : 0.0);
			if (rotInc_ > 0.0) {
				rotPhase_ += rotInc_;
				rotPhase_ -= std::floor(rotPhase_);
			}
		}
		if (driveIsPaunchOdd(mode)) {
			// THE LEVEL-TRACKED ODD LAW (`paunchOdd`): the follower's state is the stage's, advanced a sample at a time.
			over_.process(&left, &right, 1, [this, mode, gain, rot](float& l, float& r) {
				paunchOdd(mode, l, r, gain, rot, followPower_, followCoefficient_);
			});
		}
		else if (driveIsPaunchLaw(mode)) {
			// THE LAW (`paunchLaw`) inside the oversampled region, where both channels are at hand: the rectified or squared
			// mid, turned to the angle, added to the two sides. No make-up and no saturation; `gain` is the linear Amount `a`.
			over_.process(&left, &right, 1, [mode, gain, rot](float& l, float& r) { paunchLaw(mode, l, r, gain, rot); });
		}
		else if (mode == DriveMode::Chonk) {
			// THE RIGHT'S RESIDUE (the tanh less the clean signal) IS TURNED OVER BY SHAPE: r + (1 - 2 shape)(yr - r),
			// written as yr + 2 shape (r - yr) so that at the bottom it is exactly yr. Norm is applied here.
			over_.process(&left, &right, 1, [gain, shape, norm](float& l, float& r) {
				const float yl = std::tanh(gain * l) * norm;
				const float yr = std::tanh(gain * r) * norm;
				l = yl;
				r = yr + 2.0f * shape * (r - yr);
			});
		}
		else {
			over_.process(&left, &right, 1, [mode, gain, shape](float& l, float& r) {
				l = shapeDrive(mode, l * gain, shape, gain);
				r = shapeDriveRight(mode, r * gain, shape, gain);
			});
			left *= norm;
			right *= norm;
		}
		// THE WIDTH TYPES' LEVEL TRIM (`widthTrim`), on the host frames after the law and before the offset's removal: a slow gain pair that keeps
		// the balance the input had. Chonk's is its Bal (0 leaves the law as it was, to the bit); the others' is always all of it.
		if (trimOn_ && driveBalances(mode, shape)) {
			widthTrim(trim_, lIn, rIn, left, right, mode == DriveMode::Chonk ? s.balance : 1.0f, trimUnity_);
		}
		if (driveMakesOffset(mode) && !s.leaveDc) {
			// THE OFFSET (the stage's own one-pole estimate) TAKEN OUT BY THE AMOUNT: all of it at 1, as this always did.
			dcL_ += (left - dcL_) * dcCoefficient_;
			dcR_ += (right - dcR_) * dcCoefficient_;
			fullL_ = left - dcL_;
			fullR_ = right - dcR_;
			dcNow_ += (s.dcAmount - dcNow_) * dcGlide_;
			if (std::abs(s.dcAmount - dcNow_) < 1.0e-6f) {
				dcNow_ = s.dcAmount;
			}
			left -= dcL_ * dcNow_;
			right -= dcR_ * dcNow_;
		}
		else {
			fullL_ = left;
			fullR_ = right;
		}
		// THE OVERSAMPLER'S FILTERS RING DOWN FOR EVER IN A FLOAT: once nothing has gone in and next to nothing has
		// come out for a while, they are emptied and the stage puts out nothing until something goes in.
		if (silentIn && std::abs(left) < kQuietLevel && std::abs(right) < kQuietLevel) {
			if (quiet_ < kQuietFrames) {
				++quiet_;
				if (quiet_ == kQuietFrames) {
					over_.reset();
					dcL_ = dcR_ = 0.0f;
					followPower_ = 0.0f;
					trim_.reset();
				}
			}
			if (quiet_ >= kQuietFrames) {
				left = right = 0.0f;
				fullL_ = fullR_ = 0.0f;
			}
		}
		else {
			quiet_ = 0;
		}
	}

	static constexpr int kQuietFrames = 256;
	static constexpr float kQuietLevel = 1.0e-9f;

	DriveSettings settings_ {};
	dyn::Oversampler4x over_ {};
	Slew slew_ {};
	int quiet_ = 0;
	double crushPhase_ = 0.0;
	float crushL_ = 0.0f;
	float crushR_ = 0.0f;
	float prevL_ = 0.0f;
	float prevR_ = 0.0f;
	float dcL_ = 0.0f;
	float dcR_ = 0.0f;
	float dcCoefficient_ = 0.0f;
	/// The DC amount as it glides (`kDcGlideSeconds`) to the setting's, and the fully blocked frame.
	float dcGlide_ = 0.0f;
	float dcNow_ = 1.0f;
	float fullL_ = 0.0f;
	float fullR_ = 0.0f;
	/// THE ODD LAWS' FOLLOWER: the mean square of the mid, and its one-pole's coefficient at the oversampled rate.
	float followPower_ = 0.0f;
	float followCoefficient_ = 0.0f;
	/// THE ROTATION LFO's phase (turns) and its step per host frame (0 when the rate is off); the host rate it was prepared at.
	double rotPhase_ = 0.0;
	double rotInc_ = 0.0;
	double hostRate_ = 48000.0;
	/// THE WIDTH TYPES' LEVEL TRIM state (`WidthTrim`), at the host rate.
	WidthTrim trim_ {};
	bool trimOn_ = true;
	bool trimUnity_ = false;
	float slewHz_ = 0.0f;
	float slewCurve_ = 0.0f;
	float slewBalance_ = 0.0f;
	bool slewSet_ = false;
};

/// **THE POST-DRIVE DC STAGE** (the same one-pole as the stage's, `kDriveDcHz`), per channel, for an offset that arrives from a slot when
/// the Drive does not run a Type that blocks its own: the output is `x - amount * dc` (the amount glides) and `full` is `x - dc`, what
/// the loops' taps take. A blocker with nothing in it, and an input that is exactly zero, give exact zero (a tail ends).
class BiasBlocker {
public:
	void prepare(double sampleRate) noexcept {
		const double rate = sampleRate > 0.0 ? sampleRate : 48000.0;
		coefficient_ = static_cast<float>(1.0 - std::exp(-2.0 * 3.141592653589793 * kDriveDcHz / rate));
		glide_ = static_cast<float>(1.0 - std::exp(-1.0 / (kDcGlideSeconds * rate)));
		reset();
	}
	void reset() noexcept {
		dc_[0] = dc_[1] = 0.0f;
		full_[0] = full_[1] = 0.0f;
	}
	/// The amount the output glides to; it starts from here when the blocker has just been emptied (`snap`).
	void setAmount(float target, bool snap) noexcept {
		target_ = target;
		if (snap) {
			now_ = target;
		}
	}
	void process(float& left, float& right) noexcept {
		now_ += (target_ - now_) * glide_;
		if (std::abs(target_ - now_) < 1.0e-6f) {
			now_ = target_;
		}
		left = one(0, left);
		right = one(1, right);
	}
	[[nodiscard]] float fullL() const noexcept { return full_[0]; }
	[[nodiscard]] float fullR() const noexcept { return full_[1]; }
	[[nodiscard]] float dcForTest(int channel) const noexcept { return dc_[channel & 1]; }

private:
	[[nodiscard]] float one(int c, float x) noexcept {
		float& dc = dc_[c];
		dc += (x - dc) * coefficient_;
		if (x == 0.0f && std::abs(dc) < kFloor) {
			dc = 0.0f;
			full_[c] = 0.0f;
			return 0.0f;
		}
		full_[c] = x - dc;
		return x - dc * now_;
	}
	static constexpr float kFloor = 1.0e-9f;
	float dc_[2] = {};
	float full_[2] = {};
	float coefficient_ = 0.0f;
	float glide_ = 0.0f;
	float now_ = 1.0f;
	float target_ = 1.0f;
};

/// **HP WET: A TWO-POLE HIGHPASS ON THE WET, AFTER THE SHAPER, and the same filter on the dry for Sub Thru** — what it
/// took from the wet is `dry - highpass(dry)`. Trapezoidal, so it is stable to the top of its range; a filter that
/// has rung down to nothing is set there, so a tail ends.
class WetHighpass {
public:
	void reset() noexcept {
		for (State& state : wet_) {
			state = State {};
		}
		for (State& state : dry_) {
			state = State {};
		}
	}
	void set(const HighpassSettings& settings) noexcept { settings_ = settings; }
	[[nodiscard]] bool on() const noexcept { return settings_.on; }

	[[nodiscard]] float wet(int channel, float x) noexcept { return run(wet_[channel & 1], x); }
	/// WHAT THE FILTER TOOK from the dry: its low end.
	[[nodiscard]] float taken(int channel, float dry) noexcept { return dry - run(dry_[channel & 1], dry); }

private:
	struct State {
		float ic1 = 0.0f;
		float ic2 = 0.0f;
	};
	[[nodiscard]] float run(State& state, float x) const noexcept {
		const HighpassSettings& s = settings_;
		const float v3 = x - state.ic2;
		const float v1 = s.a1 * state.ic1 + s.a2 * v3;
		const float v2 = state.ic2 + s.a2 * state.ic1 + s.a3 * v3;
		state.ic1 = 2.0f * v1 - state.ic1;
		state.ic2 = 2.0f * v2 - state.ic2;
		if (x == 0.0f && std::abs(state.ic1) < kFloor && std::abs(state.ic2) < kFloor) {
			state.ic1 = state.ic2 = 0.0f;
			return 0.0f;
		}
		return x - s.k * v1 - v2;
	}
	static constexpr float kFloor = 1.0e-9f;

	HighpassSettings settings_ {};
	State wet_[2] = {};
	State dry_[2] = {};
};

} // namespace feathers::fx::eroder
