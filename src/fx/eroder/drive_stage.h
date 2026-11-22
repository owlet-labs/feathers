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
#include "fx/wet_highpass.h"

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
/// **THE AMOUNT'S GLIDE**: the quantities the Drive's Amount sets (the gain into the curve and its make-up, the holds, the slew rates) move to a
/// new value by one pole, a frame at a time, with this time constant, so an Amount that is modulated or automated does not step once a control
/// block. A time, not a count of frames, so a host's block size and the stage's rate do not move it.
inline constexpr double kDriveAmountGlideSeconds = kDustSmoothingSeconds;

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
		amountGlide_ = 1.0 - std::exp(-1.0 / (kDriveAmountGlideSeconds * rate));
		reset();
	}

	void reset() noexcept {
		over_.reset();
		quiet_ = 0;
		crushPhaseL_ = crushPhaseR_ = 0.0;
		crushL_ = crushR_ = 0.0f;
		prevL_ = prevR_ = 0.0f;
		inPrevL_ = inPrevR_ = 0.0f;
		slew_.reset();
		dcL_ = dcR_ = 0.0f;
		fullL_ = fullR_ = 0.0f;
		dcNow_ = settings_.dcAmount;
		snapAmounts();
		followPower_ = 0.0f;
		trim_.reset();
		rotPhase_ = 0.0;
	}

	/// TEST SEAMS: the Amount's quantities as the stage runs them this frame (they glide to `settings()`'s).
	[[nodiscard]] double gainNowForTest() const noexcept { return gainNow_; }
	[[nodiscard]] double normNowForTest() const noexcept { return normNow_; }
	[[nodiscard]] double holdFramesNowForTest(int channel) const noexcept { return channel == 0 ? holdLNow_ : holdRNow_; }
	[[nodiscard]] double slewHzNowForTest(int channel) const noexcept { return channel == 0 ? slewHzNow_ : slewHzRightNow_; }

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
			snapAmounts();              // and so do the Amount's quantities
		}
		if (settings.mode != DriveMode::Slew && settings.mode != DriveMode::Lopside) {
			// Not a slew: nothing hears the rate, so it is set at once (the glide is for the Types that run on it).
			slewHzNow_ = static_cast<double>(settings.slewHz);
			slewHzRightNow_ = static_cast<double>(settings.slewHzRight);
		}
		if (running(settings)) {
			aimSlew();
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
	/// THE AMOUNT'S QUANTITIES START AT THEIR TARGETS (a stage from rest, another Type, a reset).
	void snapAmounts() noexcept {
		const DriveSettings& s = settings_;
		gainNow_ = static_cast<double>(s.gain);
		normNow_ = static_cast<double>(s.norm);
		holdLNow_ = s.crushHoldFrames;
		holdRNow_ = rightHold(s);
		slewHzNow_ = static_cast<double>(s.slewHz);
		slewHzRightNow_ = static_cast<double>(s.slewHzRight);
	}
	/// ONE POLE TOWARD A TARGET, snapped to it within a millionth (relative, floored at 1): a settled stage holds the target's own bits.
	static void approach(double& now, double target, double coefficient) noexcept {
		if (now == target) {
			return;
		}
		now += (target - now) * coefficient;
		if (std::abs(target - now) <= 1.0e-6 * std::max(1.0, std::abs(target))) {
			now = target;
		}
	}
	/// THE SLEW'S SETTINGS FROM THE RATES NOW (redone only when one moved, or the curve or balance did).
	void aimSlew() noexcept {
		const DriveSettings& s = settings_;
		const auto hz = static_cast<float>(slewHzNow_);
		const auto hzRight = static_cast<float>(slewHzRightNow_);
		if (hz == slewHz_ && hzRight == slewHzRight_ && s.slewCurve == slewCurve_ && s.slewBalance == slewBalance_ && slewSet_) {
			return;
		}
		slewHz_ = hz;
		slewHzRight_ = hzRight;
		slewCurve_ = s.slewCurve;
		slewBalance_ = s.slewBalance;
		slewSet_ = true;
		SlewSettings slew;
		slew.on = true;
		slew.hz = slewHz_;
		slew.hzRight = slewHzRight_ > 0.0f ? slewHzRight_ : -1.0f; // (a hand-built settings without a right rate has one rate)
		slew.curve = slewCurve_;
		slew.balance = slewBalance_;
		slew_.set(slew);
	}
	/// ONE FRAME OF THE GLIDE.
	void glideAmounts(bool slewMode) noexcept {
		const DriveSettings& s = settings_;
		approach(gainNow_, static_cast<double>(s.gain), amountGlide_);
		approach(normNow_, static_cast<double>(s.norm), amountGlide_);
		approach(holdLNow_, s.crushHoldFrames, amountGlide_);
		approach(holdRNow_, rightHold(s), amountGlide_);
		if (slewMode) {
			approach(slewHzNow_, static_cast<double>(s.slewHz), amountGlide_);
			approach(slewHzRightNow_, static_cast<double>(s.slewHzRight), amountGlide_);
			aimSlew();
		}
	}

	/// Not run: at the bottom of Drive, or with none of the stage in the signal.
	[[nodiscard]] static bool running(const DriveSettings& s) noexcept { return s.driven && s.mix > 0.0f; }

	/// THE RIGHT SIDE'S HOLD, in frames: a settings built without one (the default) holds both sides alike.
	[[nodiscard]] static double rightHold(const DriveSettings& s) noexcept {
		return s.crushHoldFramesRight > 1.0 || s.crushHoldFrames <= 1.0 ? s.crushHoldFramesRight : s.crushHoldFrames;
	}
	/// **THE TUNED HOLD'S EDGE** (Key on only). The clock wraps in the sample where its phase passes the hold, `over` samples (0..1) after the true edge, so
	/// the new value has held for the fraction `over` of that sample and the old for the rest: the sample is `old + over (new - old)`, a one-sample box
	/// average of the ideal edge. A whole-sample switch puts the edge up to a sample early or late and the error jitters with the hold's fraction.
	[[nodiscard]] static float holdEdge(float old, float fresh, float over) noexcept { return old + over * (fresh - old); }
	/// THE INPUT AT THE EDGE: keyed, the value the input had `over` samples before this one (a line between the previous sample and this), so a tuned hold takes
	/// its value at the true edge time as well as switching at it; unkeyed, this sample's own value (today's).
	[[nodiscard]] static float atEdge(bool keyedHold, float now, float before, float over) noexcept {
		return keyedHold ? now + (before - now) * over : now;
	}
	/// DECIMATE'S OUTPUT at a clock position: Shape 0 the held value, 1 the line from the previous to the current held value across the hold, between them blended.
	[[nodiscard]] static float decimateOut(const DriveSettings& s, double phase, double hold, float prev, float cur) noexcept {
		if (s.shape <= 0.0f) {
			return cur;
		}
		const auto t = static_cast<float>(std::min(phase / hold, 1.0));
		const float line = prev + (cur - prev) * t;
		if (s.shape >= 1.0f) {
			return line;
		}
		return cur + s.shape * (line - cur);
	}

	void run(float& left, float& right) noexcept {
		const DriveSettings& s = settings_;
		glideAmounts(s.mode == DriveMode::Slew || s.mode == DriveMode::Lopside);
		const double holdL = holdLNow_;
		if (s.mode == DriveMode::Crush) {
			// HELD FOR A TIME, ROUNDED TO A STEP. A hold of one frame is every frame. EACH SIDE HAS ITS OWN CLOCK (the Spread detunes them; at 0 they
			// count alike). KEYED, the edge is placed on the fraction of a sample (`holdEdge`).
			crushPhaseL_ += 1.0;
			crushPhaseR_ += 1.0;
			const double holdR = holdRNow_;
			const float oldL = crushL_;
			const float oldR = crushR_;
			float overL = -1.0f;
			float overR = -1.0f;
			const float inL = left;
			const float inR = right;
			if (crushPhaseL_ >= holdL) {
				overL = static_cast<float>(crushPhaseL_ - holdL);
				crushPhaseL_ -= holdL;
				crushL_ = std::round(atEdge(s.keyed, inL, inPrevL_, overL) / s.crushStep) * s.crushStep;
			}
			if (crushPhaseR_ >= holdR) {
				overR = static_cast<float>(crushPhaseR_ - holdR);
				crushPhaseR_ -= holdR;
				crushR_ = std::round(atEdge(s.keyed, inR, inPrevR_, overR) / s.crushStep) * s.crushStep;
			}
			inPrevL_ = inL;
			inPrevR_ = inR;
			left = s.keyed && overL >= 0.0f ? holdEdge(oldL, crushL_, overL) : crushL_;
			right = s.keyed && overR >= 0.0f ? holdEdge(oldR, crushR_, overR) : crushR_;
			return;
		}
		if (s.mode == DriveMode::Decimate) {
			// HELD FOR A TIME, NOT ROUNDED. Shape 0 is the held value; Shape 1 a straight line from the previous held
			// value to the current one across the hold (a first-order hold: it lags by one hold period); between,
			// the two outputs blended. Each side has its own clock, as in Crush.
			const double holdR = holdRNow_;
			crushPhaseL_ += 1.0;
			crushPhaseR_ += 1.0;
			float oldOutL = 0.0f;
			float oldOutR = 0.0f;
			float overL = -1.0f;
			float overR = -1.0f;
			const float inL = left;
			const float inR = right;
			if (crushPhaseL_ >= holdL) {
				if (s.keyed) {
					oldOutL = decimateOut(s, crushPhaseL_, holdL, prevL_, crushL_);
				}
				overL = static_cast<float>(crushPhaseL_ - holdL);
				crushPhaseL_ -= holdL;
				prevL_ = crushL_;
				crushL_ = atEdge(s.keyed, inL, inPrevL_, overL);
			}
			if (crushPhaseR_ >= holdR) {
				if (s.keyed) {
					oldOutR = decimateOut(s, crushPhaseR_, holdR, prevR_, crushR_);
				}
				overR = static_cast<float>(crushPhaseR_ - holdR);
				crushPhaseR_ -= holdR;
				prevR_ = crushR_;
				crushR_ = atEdge(s.keyed, inR, inPrevR_, overR);
			}
			inPrevL_ = inL;
			inPrevR_ = inR;
			const float outL = decimateOut(s, crushPhaseL_, holdL, prevL_, crushL_);
			const float outR = decimateOut(s, crushPhaseR_, holdR, prevR_, crushR_);
			left = s.keyed && overL >= 0.0f ? holdEdge(oldOutL, outL, overL) : outL;
			right = s.keyed && overR >= 0.0f ? holdEdge(oldOutR, outR, overR) : outR;
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
		const auto gain = static_cast<float>(gainNow_);
		const float shape = s.shape;
		const auto norm = static_cast<float>(normNow_);
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
		else if (mode == DriveMode::Lean && rotInc_ > 0.0) {
			// LEAN'S LEAN SWAPS SIDES. A Lean side is its odd part plus or minus its even part (`f(u) = o + e`, the left; the right is `-f(-u) = o - e`); the
			// rotation scales the even part by `c = cos(2 pi phase)`: left `o + c e`, right `o - c e`. The two sides' power is equal at every instant (`o` is
			// odd and `e` even, so they do not correlate), the mono sum is `o`, with no even content at any instant, and the lean runs from full left through
			// none to full right and back once a turn. At `c = 1` (rate off) this is not run: the branch above is today's Lean to the bit.
			const auto c = static_cast<float>(std::cos(2.0 * 3.141592653589793 * rotPhase_));
			rotPhase_ += rotInc_;
			rotPhase_ -= std::floor(rotPhase_);
			over_.process(&left, &right, 1, [gain, shape, c](float& l, float& r) {
				const std::pair<float, float> y = leanRotated(l * gain, r * gain, shape, gain, c);
				l = y.first;
				r = y.second;
			});
			left *= norm;
			right *= norm;
		}
		else {
			const CurveExtras curve = s.curve;
			over_.process(&left, &right, 1, [mode, gain, shape, curve](float& l, float& r) {
				l = shapeDrive(mode, l * gain, shape, gain, curve);
				r = shapeDriveRight(mode, r * gain, shape, gain, curve);
			});
			left *= norm;
			right *= norm;
		}
		// THE WIDTH TYPES' LEVEL TRIM (`widthTrim`), on the host frames after the law and before the offset's removal: a slow gain pair that keeps
		// the balance the input had. Chonk's is its Bal (0 leaves the law as it was, to the bit); the others' is always all of it.
		if (trimOn_ && driveBalances(mode, shape)) {
			widthTrim(trim_, lIn, rIn, left, right, mode == DriveMode::Chonk ? s.balance : 1.0f, trimUnity_);
		}
		if (driveMakesOffset(mode, s.curve.bias) && !s.leaveDc) {
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
	double crushPhaseL_ = 0.0;
	double crushPhaseR_ = 0.0;
	float crushL_ = 0.0f;
	float crushR_ = 0.0f;
	float inPrevL_ = 0.0f;
	float inPrevR_ = 0.0f;
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
	/// THE AMOUNT'S QUANTITIES as they glide (`kDriveAmountGlideSeconds`), in double so that they land on the target's bits and snap to it.
	double amountGlide_ = 0.0;
	double gainNow_ = 1.0;
	double normNow_ = 1.0;
	double holdLNow_ = 1.0;
	double holdRNow_ = 1.0;
	double slewHzNow_ = 0.0;
	double slewHzRightNow_ = 0.0;
	/// The rates the slew was last set to.
	float slewHz_ = 0.0f;
	float slewHzRight_ = 0.0f;
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

/// HP WET and Sub Thru run on the shared `fx::WetHighpass` (`fx/wet_highpass.h`).
using WetHighpass = fx::WetHighpass;

} // namespace feathers::fx::eroder
