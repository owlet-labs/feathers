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

// A 0 dBFS CLIP LED'S THREE STATES — backlog ML: on the clip LEDs, the master's especially, -2 to 0 dBFS has a
// colour of its own, so "almost clipping" is easy to tell from clipping.
//
//   - CLIP: the signal reached full scale. The engine's over detector is `>= 1.0` (`busutil::Meter::process`: "0 dBFS
//     IS the ceiling"), and a hard clamp's output sits at exactly 1.0, so 0.0 dBFS itself is a clip: were it "near",
//     a bus that is hard-clipping would never show red.
//   - NEAR: the peak is within `kNearClipDb` of full scale, `[-2, 0)` dBFS, drawn in `kMeterNear`.
//   - NORMAL: below that; the LED is the seated ring.
//
// One classifier, so every LED that shows the state draws the same boundaries.

namespace feathers::ui {

enum class ClipLedState { Normal, Near, Clip };

/// HOW FAR UNDER FULL SCALE "ALMOST CLIPPING" STARTS, dB.
inline constexpr float kNearClipDb = -2.0f;
/// `kNearClipDb` as a linear peak, 10^(-2/20).
inline constexpr float kNearClipLinear = 0.79432823472428150f;

/// THE STATE FOR A LINEAR SAMPLE PEAK. `over` is the LED's own latched "reached full scale" (a hold the engine's
/// clip age drives), so an over drawn a moment ago stays a clip while the held peak falls.
[[nodiscard]] constexpr ClipLedState clipLedStateFor(float peak, bool over) noexcept {
	if (over || peak >= 1.0f) {
		return ClipLedState::Clip;
	}
	return peak >= kNearClipLinear ? ClipLedState::Near : ClipLedState::Normal;
}

} // namespace feathers::ui
