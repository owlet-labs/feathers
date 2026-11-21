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

#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

namespace feathers::fx {

/// THE HP WET CONTROL'S RANGE, log2 hertz: the bottom of it is "no filter at all", the top 8 kHz.
inline constexpr double kWetHighpassLog2Min = 4.321928094887362;  // 20 Hz
inline constexpr double kWetHighpassLog2Max = 12.965784284662087; // 8 kHz
/// The wet highpass's Q: Butterworth.
inline constexpr double kWetHighpassQ = 0.7071067811865476;

/// **WHAT A `WetHighpass` RUNS ON**: off at the bottom of the control, else the trapezoidal coefficients of a
/// two-pole highpass at `hz`.
struct HighpassSettings {
	bool on = false;
	double hz = 0.0;
	float a1 = 1.0f;
	float a2 = 0.0f;
	float a3 = 0.0f;
	float k = 0.0f;
};

/// THE COEFFICIENTS of a two-pole highpass at `hz` (held under `nyquistFraction` of the rate), `on` as given.
[[nodiscard]] inline HighpassSettings highpassAt(bool on, double hz, double rate, double nyquistFraction) noexcept {
	HighpassSettings h;
	h.on = on;
	h.hz = std::min(hz, nyquistFraction * rate);
	const double g = std::tan(std::numbers::pi * h.hz / rate);
	const double k = 1.0 / kWetHighpassQ;
	const double a1 = 1.0 / (1.0 + g * (g + k));
	h.a1 = static_cast<float>(a1);
	h.a2 = static_cast<float>(g * a1);
	h.a3 = static_cast<float>(g * g * a1);
	h.k = static_cast<float>(k);
	return h;
}

/// **A WET HIGHPASS WITH A SUB THRU: A TWO-POLE HIGHPASS ON THE WET, AND THE SAME FILTER ON THE DRY** — what it took
/// from the dry is `dry - highpass(dry)`, which a device adds back to its wet when Sub Thru is on. Shared by every
/// device that offers HP Wet and Sub Thru (the Eroder, the Smooth Shaper). Trapezoidal, so it is stable to the top of
/// its range; a filter that has rung down to nothing is set there, so a tail ends. The settings are latched by the
/// device on its own control grid (`set`); nothing here allocates.
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

} // namespace feathers::fx
