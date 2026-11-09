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

#include "backplane/device_host.h"
#include "fx/delay/delay_taps.h"

#include <visage/ui.h>

// THE DELAY'S TAPS PICTURE, ONCE, FOR THE CARD'S STRIP AND THE WINDOW'S TIME PLANE (delay-love §2.2).
//
// Lifted out of `DelayCard` so both surfaces decode the same readings into the same ticks and paint them the same way
// — the card at 58 px, the window at whatever its plot is given. **One decode, so the two cannot disagree** (a window
// tick and a card tick for the same arrival are the same `StripTick`). `delay_card.h` has what the picture means.

namespace feathers::ui {

/// THE GEOMETRY OF ONE STRIP: its rectangle and the seconds its axis spans.
struct DelayStrip {
	/// The time axis's margins: the left one holds the L and R letters clear of the dry tick at zero.
	static constexpr float kInsetLeft = 14.0f;
	static constexpr float kInsetRight = 8.0f;

	visage::Bounds bounds {};
	double span = 0.0;

	/// THE X a time lands at, clamped to the axis.
	[[nodiscard]] float xAt(double seconds) const;
	/// THE TIME an x stands for — `xAt`'s inverse on the axis (the plot's handles read a drag through it).
	[[nodiscard]] double secondsAt(float x) const;
	/// THE Y a tick of `gain` on `channel` reaches: left above the centre line, right below, on the dB scale.
	[[nodiscard]] float tipY(int channel, float gain) const;
	[[nodiscard]] float midY() const { return bounds.y() + bounds.height() * 0.5f; }
};

/// THE READINGS AS A PICTURE; false while nothing is published.
[[nodiscard]] bool delayPicture(const backplane::DeviceHost& host, DeviceInstanceId id, fx::delay::TapPicture* out);

/// **THE TAP THE UNMODULATED CONTROLS ASK FOR**, seconds — the axis's unit. The FIELDS, not the modulated values: Sync's
/// base as the device resolved it (it knows the tempo), times the Time the hand left, widened by the Spread it left.
[[nodiscard]] double delayHandUnitSeconds(const backplane::DeviceHost& host, DeviceInstanceId id,
                                          const fx::delay::TapPicture& p);

/// PAINT THE STRIP: its bed, the rules, the centre line, L and R, the whiskers, the grains, the ticks and the live
/// head's time. `picture` null paints the bed and the no-signal mark.
void paintDelayStrip(visage::Canvas& canvas, const DelayStrip& strip, const fx::delay::TapPicture* picture, double unit);

} // namespace feathers::ui
