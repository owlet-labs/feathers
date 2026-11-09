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

#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>
#include <visage/ui.h>

// THE DYNAMICS CARDS' METER COLUMN — COMP's and CLIP's vertical bars, drawn one way.
//
// **THE MAPPING IS THE CONTRACT A TEST STATES.** A level bar is linear in dB between `kFloorDb` and
// 0 dBFS; a gain-reduction bar is linear in dB between 0 and `kGrRangeDb`, hanging from the top. The
// readings are LINEAR peaks (`comp::Reading`, `clip::Reading`) because a reading is a number; the bar is
// in dB because that is how a level reads to a hand — Bus Utility's reasoning, `busutil_card.h`.

namespace feathers::ui::dyn_meter {

/// WHERE A LEVEL BAR BOTTOMS OUT, rather than running to negative infinity.
inline constexpr float kFloorDb = -60.0f;
/// THE GAIN-REDUCTION BAR'S FULL TRAVEL, in dB.
inline constexpr float kGrRangeDb = 24.0f;
/// THE CAPTION TYPE — Bus Utility's meter caption size, so the two meters read alike.
inline constexpr float kCaptionSize = 7.5f;
inline constexpr float kCaptionHeight = 10.0f;
inline constexpr float kBarWidth = 9.0f;

/// A LINEAR PEAK AS A BAR FRACTION, 0 at `kFloorDb` and below, 1 at 0 dBFS and above.
[[nodiscard]] inline float levelFraction(float linear) {
	if (!(linear > 0.0f)) {
		return 0.0f;
	}
	const float db = 20.0f * std::log10(linear);
	return std::clamp((db - kFloorDb) / (0.0f - kFloorDb), 0.0f, 1.0f);
}

/// GAIN REDUCTION (positive dB) AS A BAR FRACTION, 0 for none and 1 at `kGrRangeDb`.
[[nodiscard]] inline float grFraction(float grDb) {
	if (!(grDb > 0.0f)) {
		return 0.0f;
	}
	return std::clamp(grDb / kGrRangeDb, 0.0f, 1.0f);
}

/// ONE VERTICAL BAR: the track, then `fraction` of it filled from the bottom — or from the top when
/// `hanging`, which is how gain reduction reads (it is taken AWAY from the level).
inline void drawBar(visage::Canvas& canvas, float x, float y, float height, float fraction, bool hanging,
                    uint32_t colour) {
	canvas.setColor(kSurfaceRaised);
	canvas.roundedRectangle(x, y, kBarWidth, height, 2.0f);
	const float filled = height * std::clamp(fraction, 0.0f, 1.0f);
	if (filled <= 0.0f) {
		return;
	}
	canvas.setColor(colour);
	canvas.roundedRectangle(x, hanging ? y : y + height - filled, kBarWidth, filled, 2.0f);
}

/// **NO DATA DRAWS NO METER** — `device_host.h`'s rule, Bus Utility's words for it: "no" over "signal",
/// centred in the column, rather than bars at zero that would look like measured silence.
inline void drawNoSignal(visage::Canvas& canvas, visage::Bounds box) {
	canvas.setColor(kDisabled);
	const float mid = box.y() + box.height() * 0.5f;
	canvas.text("no", font(kCaptionSize), visage::Font::kCenter, box.x(), mid - kCaptionHeight, box.width(),
	            kCaptionHeight);
	canvas.text("signal", font(kCaptionSize), visage::Font::kCenter, box.x(), mid, box.width(), kCaptionHeight);
}

/// A BAR'S NAME, centred under it.
inline void drawCaption(visage::Canvas& canvas, const char* what, float centreX, float y, float width) {
	canvas.setColor(kDisabled);
	canvas.text(what, font(kCaptionSize), visage::Font::kCenter, centreX - width * 0.5f, y, width,
	            kCaptionHeight);
}

} // namespace feathers::ui::dyn_meter
