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

#include "fx/busutil/busutil_meter.h"

// THE MASTER METER'S READINGS, AS THEY CROSS FROM THE ENGINE TO THE EDITOR — backlog DN (2026-09-15): the master meter sits at the far right of the knob tray,
// full tray height; instantaneous but coloured, orange at -6 and red at -3 (sample peak, on transport play).
//
// The synth runs a `fx::busutil::Meter` — the bus utility's own K-weighted meter — over its OUTPUT,
// after the master gain, so a "master" meter measures what leaves the plugin and not wherever a
// device was dropped. Three of its readings cross: the momentary loudness (the "instantaneous" one —
// BS.1770's 400 ms window), the raw sample peak, and how long ago a sample clipped, which is what the
// 0 dBFS LED's instantaneous core and decaying glow are drawn from (the same two time constants the
// bus utility's card has). Plain data, so the synth and the bridge can share it without either
// including the other.

namespace feathers {

struct MasterMeterReadings {
	float momentaryLufs = fx::busutil::kSilenceLufs;
	float rawPeak = 0.0f;
	float clipAge = fx::busutil::kClipAgeMax;
};

inline MasterMeterReadings masterMeterReadingsOf(const fx::busutil::Meter& meter) noexcept {
	using fx::busutil::Reading;
	return { meter.reading(Reading::Momentary), meter.reading(Reading::RawPeak), meter.reading(Reading::ClipAge) };
}

} // namespace feathers
