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

#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <utility>

// **POINTER ACCELERATION FOR A VALUE-BOX DRAG — backlog QB.**
//
// THE PROBLEM: the zoned coordinate's digit drag felt too slow even on the large digits. Fast movement
// should be quite fast and slow movement quite precise — acceleration.
//
// THE RATE A PIXEL IS WORTH BECOMES A FUNCTION OF HOW FAST THE HAND IS MOVING. Below `kSlowSpeed` it is EXACTLY
// the grabbed digit's own rate — the gain is 1.0, not "about 1" — so a slow drag is as precise as it always was.
// Above `kFastSpeed` it is `kMaxGain` times that rate, so a flick crosses many zones. Between, a smoothstep, so the
// curve is monotonic and has no corner a hand can feel.
//
// **EVERY CONSTANT A HAND TUNES IS IN THIS ONE BLOCK, AND NONE OF THEM IS TUNED.** They are proposals, written down
// with their units, to be set by feel (decide the shape; let measurement set the numbers — here the
// measurement is a hand).
//
// **SPEED IS MEASURED AGAINST A CLOCK, NOT PER EVENT.** visage's mouse events carry no timestamp, and the number of
// motion events a second depends on the mouse (125 Hz to 1000 Hz) and the host. A per-event delta would make a
// 1000 Hz mouse never accelerate and a 125 Hz one accelerate eight times as readily. So travel is pooled until at
// least `kWindowMs` has passed, and the pool's px/ms is the sample; an exponential average over `kSmoothingMs`
// calms the jitter. A hand that STOPS mid-drag and starts again is slow by construction: the pause is inside the
// next window, so that window's speed is small.

namespace feathers::ui::drag_accel {

//------------------------------------------------------------------------------------------------------------------//
// THE CURVE — UNTUNED. The knobs to tune by hand.                                                                        //
//------------------------------------------------------------------------------------------------------------------//

/// Below this the gain is exactly 1: the grabbed digit's own rate. px per ms (0.15 px/ms ≈ 2.5 px per 60 Hz frame).
inline constexpr double kSlowSpeed = 0.15;
/// At and above this the gain is `kMaxGain`. px per ms (1.5 px/ms ≈ 25 px per 60 Hz frame — a flick).
inline constexpr double kFastSpeed = 1.5;
/// How many times the grabbed digit's rate a flick is worth. Ten is "one column up" at full speed.
inline constexpr double kMaxGain = 10.0;
/// The shortest span a speed sample may cover, in ms — see the header.
inline constexpr double kWindowMs = 8.0;
/// The time constant of the speed average, in ms. Smaller reacts faster and jitters more.
inline constexpr double kSmoothingMs = 30.0;

static_assert(kSlowSpeed > 0.0 && kFastSpeed > kSlowSpeed, "the curve needs a slow end below its fast end");
static_assert(kMaxGain >= 1.0, "acceleration may never make a drag slower than the digit's own rate");

/// THE GAIN AT `speed` px/ms: 1 at or below `kSlowSpeed`, `kMaxGain` at or above `kFastSpeed`, smoothstep between.
/// Monotonic non-decreasing in `speed` (a case sweeps it).
[[nodiscard]] inline double gain(double speed) noexcept {
	if (!(speed > kSlowSpeed)) { // NaN-safe: a speed that is not a number is no speed
		return 1.0;
	}
	const double t = std::min(1.0, (speed - kSlowSpeed) / (kFastSpeed - kSlowSpeed));
	return 1.0 + (kMaxGain - 1.0) * t * t * (3.0 - 2.0 * t);
}

//------------------------------------------------------------------------------------------------------------------//
// THE CLOCK — steady, in ms, replaceable by a test.                                                                 //
//------------------------------------------------------------------------------------------------------------------//

[[nodiscard]] inline std::function<double()>& clockOverride() {
	static std::function<double()> clock;
	return clock;
}

[[nodiscard]] inline double nowMs() {
	if (const std::function<double()>& clock = clockOverride()) {
		return clock();
	}
	using namespace std::chrono;
	return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

/// A TEST'S CLOCK, or empty to go back to the steady one. Main thread only, as every drag is.
inline void setClockForTest(std::function<double()> clock) {
	clockOverride() = std::move(clock);
}

//------------------------------------------------------------------------------------------------------------------//
// ONE GESTURE'S SPEED                                                                                                //
//------------------------------------------------------------------------------------------------------------------//

/// **BEGUN AT EVERY PRESS, SO NOTHING CROSSES FROM ONE GESTURE TO THE NEXT.** A tracker that kept its speed would
/// make the second of two identical drags faster than the first — the shape that was described, and the case that
/// guards it drives two identical gestures and compares them by value.
class Tracker {
public:
	void begin(double now) noexcept {
		speed_ = 0.0;
		windowStart_ = now;
		windowTravel_ = 0.0;
	}

	/// `dy` px of travel arriving at `now`; answers the gain this travel is applied at.
	[[nodiscard]] double step(double dy, double now) noexcept {
		windowTravel_ += std::fabs(dy);
		const double elapsed = now - windowStart_;
		if (elapsed >= kWindowMs) {
			const double sample = windowTravel_ / elapsed;
			const double blend = 1.0 - std::exp(-elapsed / kSmoothingMs);
			speed_ += (sample - speed_) * blend;
			windowStart_ = now;
			windowTravel_ = 0.0;
		}
		return gain(speed_);
	}

	[[nodiscard]] double speed() const noexcept { return speed_; }

private:
	double speed_ = 0.0;
	double windowStart_ = 0.0;
	double windowTravel_ = 0.0;
};

} // namespace feathers::ui::drag_accel
