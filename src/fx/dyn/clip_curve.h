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

/// THE ONE SOFT-CLIP CURVE: identity below 1 - knee, a tanh shoulder to ±1 above it, a clamp at knee 0.
/// Bus Utility's clip stage (`busutil::clipSample`) forwards here, and so does anything after it that
/// means "the same clip", so the two cannot drift apart.
[[nodiscard]] inline float softClip(float x, float knee) {
	const float k = std::clamp(knee, 0.0f, 1.0f);
	// A KNEE OF NOTHING IS THE CLAMP — the top of the travel. "Off" is the caller's question, not this
	// function's: a stage at its default leaves the samples alone by not calling this at all.
	if (k <= 0.0f) {
		return std::clamp(x, -1.0f, 1.0f);
	}
	const float t = 1.0f - k;
	const float a = std::fabs(x);
	// **BELOW THE KNEE IT IS EXACTLY THE INPUT** — not multiplied by anything, not rounded through a
	// curve that is nearly straight. That is the whole point of a knee: a bus can carry this stage
	// with nothing happening to it until the signal nears the ceiling.
	if (a <= t) {
		return x;
	}
	const float shaped = t + k * std::tanh((a - t) / k);
	return x < 0.0f ? -shaped : shaped;
}

} // namespace feathers::fx::dyn
