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

// A CARD'S COURTESY OUTPUT METER — backlog MK: every fx card carries a tiny meter in its header, visible even when
// the card is collapsed, and a clipping signal fills it from the opposite end by the amount over 0 dBFS, in the clip
// colour.
//
// ONE LINE, ONE PIXEL THICK, ALONG THE HEAD'S INNER EDGE: the bottom of a top head, the right of a spine — inside the
// border, so a folded card (which is its head) still shows it. The level fills from the head's start (left, or the foot
// of a spine) on a dB scale; at 0 dBFS and over, a second run fills from the OPPOSITE end, its length the dB over full
// scale. The level's colour is the clip LED's state (`clip_led.h`, backlog ML): the value colour, the near colour within
// 2 dB of full scale and at it. All data colours, outside the hue offset (backlog LG).
//
// THE PEAK IS THE ENGINE'S (`RackEngine::outputPeakAt`): held and falling there, so this file only maps a number onto
// pixels and the arithmetic is one function a case can ask.

#include "plugin/ui/clip_led.h"
#include "plugin/ui/theme.h"

#include <visage/ui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace feathers::ui {

/// WHERE THE LEVEL RUN STARTS, dBFS: a peak at or under it draws nothing.
inline constexpr float kCardMeterFloorDb = -48.0f;
/// HOW MANY dB OVER FULL SCALE FILL THE WHOLE LINE FROM THE OTHER END. PROVISIONAL: to be set by eye.
inline constexpr float kCardMeterOverSpanDb = 12.0f;
/// THE OVER RUN IS NEVER SHORTER THAN THIS while the state is Clip, so a peak sitting exactly at full scale — a hard
/// clamp's output, which `clip_led.h` rules a clip — still shows red rather than a full line of the near colour.
inline constexpr float kCardMeterOverMinLength = 2.0f;
/// How far in from the card's outer edge the line sits: inside the 1 px border.
inline constexpr float kCardMeterInset = 2.0f;

/// THE PEAK AS FRACTIONS OF THE LINE — `level` from the start, `over` from the end — and the clip LED's state.
struct CardMeterReading {
	float level = 0.0f;
	float over = 0.0f;
	ClipLedState state = ClipLedState::Normal;
	friend bool operator==(const CardMeterReading&, const CardMeterReading&) = default;
};

/// A LINEAR SAMPLE PEAK, MAPPED. A negative, zero or non-finite peak is no level at all.
[[nodiscard]] inline CardMeterReading cardMeterReadingFor(float peak) noexcept {
	CardMeterReading r;
	if (!std::isfinite(peak) || peak <= 0.0f) {
		return r;
	}
	const float db = 20.0f * std::log10(peak);
	r.level = std::clamp((db - kCardMeterFloorDb) / (0.0f - kCardMeterFloorDb), 0.0f, 1.0f);
	r.over = std::clamp(db / kCardMeterOverSpanDb, 0.0f, 1.0f);
	r.state = clipLedStateFor(peak, false);
	return r;
}

/// THE LINE'S TRACK ON A CARD OF `card` BOUNDS: along the bottom of a top head, up the right of a spine, inside the
/// border and clear of the rounded corners.
[[nodiscard]] inline visage::Bounds cardMeterTrack(visage::Bounds card, CardHeadEdge edge) noexcept {
	if (edge == CardHeadEdge::Left) {
		const float length = std::max(0.0f, card.height() - 2.0f * kCardCorner);
		return { card.x() + kCardHeadHeight - kCardMeterInset, card.y() + kCardCorner, 1.0f, length };
	}
	const float length = std::max(0.0f, card.width() - 2.0f * kCardCorner);
	return { card.x() + kCardCorner, card.y() + kCardHeadHeight - kCardMeterInset, length, 1.0f };
}

/// WHAT THE LINE DRAWS, as rectangles in the track's own coordinates and the colours they are drawn in. An empty
/// rectangle draws nothing.
struct CardMeterMarks {
	visage::Bounds level;
	uint32_t levelColour = kMeterValue;
	visage::Bounds over;
	uint32_t overColour = kMeterClip;
	friend bool operator==(const CardMeterMarks& a, const CardMeterMarks& b) {
		return a.level == b.level && a.levelColour == b.levelColour && a.over == b.over
		       && a.overColour == b.overColour;
	}
};

/// THE MARKS FOR A TRACK `length` LONG — `vertical` for a spine, whose level rises from the foot and whose over falls
/// from the top. Lengths are whole pixels, so a reading that moves less than one draws the same marks and asks for no
/// frame.
[[nodiscard]] inline CardMeterMarks cardMeterMarksFor(float length, bool vertical, const CardMeterReading& r) noexcept {
	CardMeterMarks m;
	const float level = std::round(r.level * length);
	float over = r.state == ClipLedState::Clip ? std::max(std::round(r.over * length), kCardMeterOverMinLength) : 0.0f;
	over = std::min(over, length);
	m.levelColour = r.state == ClipLedState::Normal ? kMeterValue : kMeterNear;
	if (vertical) {
		m.level = { 0.0f, length - level, 1.0f, level };
		m.over = { 0.0f, 0.0f, 1.0f, over };
	}
	else {
		m.level = { 0.0f, 0.0f, level, 1.0f };
		m.over = { length - over, 0.0f, over, 1.0f };
	}
	return m;
}

/// **THE LINE AS A FRAME OF ITS OWN**, so a moving level repaints one pixel row rather than the card under it (the
/// backlog's cost note: "a visage frame per card: watch the repaint cost on a long lane"). It ignores the mouse, so
/// the head's press, drag and fold reach the card exactly as before. `FxCard::syncOutputMeter` places it and hands it
/// its marks.
class CardOutputMeter final : public visage::Frame {
public:
	CardOutputMeter() {
		setIgnoresMouseEvents(true, false);
		// OVER WHATEVER A CARD ADDS LATER — the head is the base's, and the line is one pixel along its inner edge.
		setOnTop(true);
		// HIDDEN UNTIL A HOST SAYS IT METERS THIS DEVICE (`FxCard::syncOutputMeter`).
		setVisible(false);
	}
	/// HAND IT NEW MARKS; true, and a redraw, only when they differ from what it drew.
	bool setMarks(const CardMeterMarks& marks) {
		if (marks == marks_) {
			return false;
		}
		marks_ = marks;
		redraw();
		return true;
	}
	[[nodiscard]] const CardMeterMarks& marks() const noexcept { return marks_; }
	void draw(visage::Canvas& canvas) override {
		if (marks_.level.width() > 0.0f && marks_.level.height() > 0.0f) {
			canvas.setColor(marks_.levelColour);
			canvas.fill(marks_.level.x(), marks_.level.y(), marks_.level.width(), marks_.level.height());
		}
		if (marks_.over.width() > 0.0f && marks_.over.height() > 0.0f) {
			canvas.setColor(marks_.overColour);
			canvas.fill(marks_.over.x(), marks_.over.y(), marks_.over.width(), marks_.over.height());
		}
	}

private:
	CardMeterMarks marks_ {};
};

} // namespace feathers::ui
