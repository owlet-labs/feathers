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

#include <cmath>

namespace feathers::fx::eroder {

/// **THE DRIVE STAGE: A STEREO SIGNAL PUSHED INTO THE SHAPER `DriveSettings` NAMES.** Both engines run one, on their
/// wet: Dust on what its head reads, the Comb on its wet as it is about to be added to the dry. The curves
/// (`shapeDrive`) run four times oversampled, so what they add above the band is filtered off rather than folded back
/// into it; Crush is not a curve and is not oversampled — its steps are the sound. Tube's standing offset is taken
/// back out. Not run at all at the bottom of Drive, so there the signal is the bits it was.
///
/// No allocation; a frame at a time, so a host that cuts its blocks anywhere gets the same bits.
class DriveStage {
public:
	void prepare(double sampleRate) noexcept {
		const double rate = sampleRate > 0.0 ? sampleRate : 48000.0;
		dcCoefficient_ = static_cast<float>(1.0 - std::exp(-2.0 * 3.141592653589793 * kDriveDcHz / rate));
		reset();
	}

	void reset() noexcept {
		over_.reset();
		quiet_ = 0;
		crushPhase_ = 0.0;
		crushL_ = crushR_ = 0.0f;
		dcL_ = dcR_ = 0.0f;
	}

	/// THE SETTINGS FOR THE FRAMES THAT FOLLOW. A shaper that was resting, or another Type, starts from nothing rather
	/// than from what it last held.
	void set(const DriveSettings& settings) noexcept {
		if (settings.driven && (!settings_.driven || settings_.mode != settings.mode)) {
			reset();
		}
		settings_ = settings;
	}

	[[nodiscard]] const DriveSettings& settings() const noexcept { return settings_; }

	/// ONE FRAME, in place.
	void process(float& left, float& right) noexcept {
		const DriveSettings& s = settings_;
		if (!s.driven) {
			return;
		}
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
		const bool silentIn = left == 0.0f && right == 0.0f;
		const DriveMode mode = s.mode;
		const float gain = s.gain;
		const float shape = s.shape;
		over_.process(&left, &right, 1, [mode, gain, shape](float& l, float& r) {
			l = shapeDrive(mode, l * gain, shape);
			r = shapeDrive(mode, r * gain, shape);
		});
		left *= s.norm;
		right *= s.norm;
		if (mode == DriveMode::Tube) {
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

private:
	static constexpr int kQuietFrames = 256;
	static constexpr float kQuietLevel = 1.0e-9f;

	DriveSettings settings_ {};
	dyn::Oversampler4x over_ {};
	int quiet_ = 0;
	double crushPhase_ = 0.0;
	float crushL_ = 0.0f;
	float crushR_ = 0.0f;
	float dcL_ = 0.0f;
	float dcR_ = 0.0f;
	float dcCoefficient_ = 0.0f;
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
