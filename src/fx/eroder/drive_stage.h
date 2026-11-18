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
class DriveStage {
public:
	void prepare(double sampleRate) noexcept {
		const double rate = sampleRate > 0.0 ? sampleRate : 48000.0;
		dcCoefficient_ = static_cast<float>(1.0 - std::exp(-2.0 * 3.141592653589793 * kDriveDcHz / rate));
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
	}

	/// THE SETTINGS FOR THE FRAMES THAT FOLLOW. A shaper that was resting, or another Type, starts from nothing rather
	/// than from what it last held.
	void set(const DriveSettings& settings) noexcept {
		if (running(settings) && (!running(settings_) || settings_.mode != settings.mode)) {
			reset();
		}
		settings_ = settings;
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
		const DriveMode mode = s.mode;
		const float gain = s.gain;
		const float shape = s.shape;
		const float norm = s.norm;
		if (driveIsPaunchLaw(mode)) {
			// THE LAW (`paunchLaw`) inside the oversampled region, where both channels are at hand: the rectified or squared
			// mid added to the left and taken from the right. No make-up and no saturation; `gain` is the linear Amount `a`.
			over_.process(&left, &right, 1, [mode, gain, shape](float& l, float& r) { paunchLaw(mode, l, r, gain, shape); });
		}
		else if (mode == DriveMode::Paunch) {
			// ONLY THE DISTORTION IS TURNED OVER (`paunchRight`): the left is the tanh, the right has the same
			// fundamental and the odd harmonics in the other polarity, held under the left's ceiling. Norm is applied here.
			over_.process(&left, &right, 1, [gain, shape, norm](float& l, float& r) {
				l = std::tanh(gain * l) * norm;
				r = paunchRight(r, gain, norm, shape);
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
		if (mode == DriveMode::Tube || mode == DriveMode::Fold || mode == DriveMode::Lean || (driveIsPaunchLaw(mode) && !s.leaveDc)) {
			dcL_ += (left - dcL_) * dcCoefficient_;
			dcR_ += (right - dcR_) * dcCoefficient_;
			left -= dcL_;
			right -= dcR_;
		}
		// THE OVERSAMPLER'S FILTERS RING DOWN FOR EVER IN A FLOAT: once nothing has gone in and next to nothing has
		// come out for a while, they are emptied and the stage puts out nothing until something goes in.
		if (silentIn && std::abs(left) < kQuietLevel && std::abs(right) < kQuietLevel) {
			if (quiet_ < kQuietFrames) {
				++quiet_;
				if (quiet_ == kQuietFrames) {
					over_.reset();
					dcL_ = dcR_ = 0.0f;
				}
			}
			if (quiet_ >= kQuietFrames) {
				left = right = 0.0f;
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
	float slewHz_ = 0.0f;
	float slewCurve_ = 0.0f;
	float slewBalance_ = 0.0f;
	bool slewSet_ = false;
};

/// **THE BIAS BLOCKER: THE ONE DC BLOCKER AFTER THE LAST SHAPER** (the same one-pole as the stage's, `kDriveDcHz`), per channel.
/// Paunch-S and Paunch-H make a bias that follows the signal; their stages leave it in (`DriveSettings::leaveDc`) so it can push
/// the shapers after them off centre, and the engine takes it out here. A blocker that has nothing in it, and an input that is
/// exactly zero, give exact zero (a tail ends).
class BiasBlocker {
public:
	void prepare(double sampleRate) noexcept {
		const double rate = sampleRate > 0.0 ? sampleRate : 48000.0;
		coefficient_ = static_cast<float>(1.0 - std::exp(-2.0 * 3.141592653589793 * kDriveDcHz / rate));
		reset();
	}
	void reset() noexcept { dc_[0] = dc_[1] = 0.0f; }
	[[nodiscard]] float process(int channel, float x) noexcept {
		float& dc = dc_[channel & 1];
		dc += (x - dc) * coefficient_;
		if (x == 0.0f && std::abs(dc) < kFloor) {
			dc = 0.0f;
			return 0.0f;
		}
		return x - dc;
	}
	void process(float& left, float& right) noexcept {
		left = process(0, left);
		right = process(1, right);
	}
	[[nodiscard]] float dcForTest(int channel) const noexcept { return dc_[channel & 1]; }

private:
	static constexpr float kFloor = 1.0e-9f;
	float dc_[2] = {};
	float coefficient_ = 0.0f;
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
