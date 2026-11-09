/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <algorithm>
#include <cmath>

namespace feathers::fx::dyn {

/// A value published once per block and read at UI frame rate must be HELD, not decayed, or sub-frame
/// peaks vanish between two reads (`busutil_meter.h`'s `RawPeak` argues it; 50 ms is its number).
inline constexpr double kHoldSeconds = 0.05;
/// Every gain the dynamics devices apply moves over this long, so nothing steps (Bus Utility's
/// `kRampSeconds`).
inline constexpr double kRampSeconds = 0.005;

/// THE LARGEST VALUE SEEN IN THE LAST ONE-TO-TWO WINDOWS. `reading()` is max(the last completed
/// window, the one filling now), so any value survives at least one whole window.
/// **IT HOLDS NON-NEGATIVE MAGNITUDES:** the floor is 0, so a negative push never registers. Pass a
/// peak as |x|, and a gain reduction as a POSITIVE number of dB.
class HeldPeak {
public:
	void prepare(double sampleRate, double holdSeconds = kHoldSeconds) noexcept {
		window_ = std::max(1, static_cast<int>(std::lround(holdSeconds * std::max(sampleRate, 1.0))));
		reset();
	}
	void reset() noexcept {
		accum_ = held_ = 0.0f;
		count_ = 0;
	}
	void push(float v) noexcept {
		accum_ = std::max(accum_, v);
		if (++count_ >= window_) {
			held_ = accum_;
			accum_ = 0.0f;
			count_ = 0;
		}
	}
	[[nodiscard]] float reading() const noexcept { return std::max(held_, accum_); }

private:
	int window_ = 2400, count_ = 0;
	float accum_ = 0.0f, held_ = 0.0f;
};

/// A GAIN THAT MOVES IN A STRAIGHT LINE AND LANDS EXACTLY. The first `set` after `reset` snaps, so a
/// device's first block starts at its settings rather than ramping up from zero. A `set` mid-ramp
/// continues from the value it has reached. **A NON-FINITE TARGET IS IGNORED:** one NaN would
/// otherwise become the value and every step after it, for good.
class LinearRamp {
public:
	/// Changes the ramp length; a ramp in flight lands on its target now, so no step computed for
	/// the old length survives into the new one.
	void prepare(double sampleRate, double seconds = kRampSeconds) noexcept {
		samples_ = std::max(1, static_cast<int>(std::lround(seconds * std::max(sampleRate, 1.0))));
		value_ = target_;
		remaining_ = 0;
		step_ = 0.0f;
	}
	void reset() noexcept {
		primed_ = false;
		remaining_ = 0;
	}
	void snap(float v) noexcept {
		value_ = target_ = v;
		remaining_ = 0;
		primed_ = true;
	}
	void set(float target) noexcept {
		if (!std::isfinite(target)) {
			return;
		}
		if (!primed_) {
			snap(target);
			return;
		}
		if (target == target_) {
			return;
		}
		target_ = target;
		remaining_ = samples_;
		step_ = (target_ - value_) / static_cast<float>(samples_);
	}
	float next() noexcept {
		if (remaining_ > 0) {
			// THE LAST STEP IS THE TARGET, not the sum of the steps: float accumulation lands near it, and a
			// gain that is meant to reach 0 or 1 has to be exactly that.
			value_ = (--remaining_ == 0) ? target_ : value_ + step_;
		}
		return value_;
	}
	[[nodiscard]] bool settled() const noexcept { return remaining_ == 0; }
	[[nodiscard]] float value() const noexcept { return value_; }

private:
	int samples_ = 240, remaining_ = 0;
	float value_ = 0.0f, target_ = 0.0f, step_ = 0.0f;
	bool primed_ = false;
};

} // namespace feathers::fx::dyn
