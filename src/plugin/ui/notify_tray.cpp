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

#include "plugin/ui/notify_tray.h"

#include "plugin/ui/diagnostic.h"
#include "plugin/ui/theme.h"

#include <visage/utils.h>

#include <cmath>

namespace feathers::ui {

NotifyTray::NotifyTray(Clock clock, Timing timing) : clock_(std::move(clock)), timing_(timing) {
	if (!clock_) {
		clock_ = [] { return static_cast<int64_t>(visage::time::milliseconds()); };
	}
}

void NotifyTray::say(std::string_view text) {
	// The text change has to be remembered, because the clock cannot report it. update() decides on
	// opacity, and opacity does not move when a message lands on top of a message: it is pinned at 1
	// for the whole hold. Without this flag a drag's readout froze at the first value it announced —
	// every later reading arrived, and none of them was ever painted.
	//
	// The same reading twice is not a change. A control that re-says what is already on screen asks
	// for nothing, which is what keeps a still tray still.
	const bool novel = text != text_;
	// Reported before the state moves, and reported for EVERY call rather than only the ones that
	// change something. A say() that repeats itself asks the font atlas for nothing, and that is a
	// distinction worth having in the log: it separates a gesture that churned glyphs from one that
	// merely happened. Compiles to an empty call outside a diagnostic build.
	diagnostic::said(text, novel);
	if (novel) {
		text_.assign(text);
		textChanged_ = true;
	}
	saidAt_ = clock_();
	everSaid_ = true;
}

float NotifyTray::opacity() const noexcept {
	if (!everSaid_) {
		return 0.0f;
	}
	const int64_t age = clock_() - saidAt_;
	if (age < timing_.holdMs) {
		return 1.0f;
	}
	const int64_t fading = age - timing_.holdMs;
	if (fading >= timing_.fadeMs) {
		return 0.0f;
	}
	return 1.0f - static_cast<float>(fading) / static_cast<float>(timing_.fadeMs);
}

bool NotifyTray::update() {
	const float now = opacity();
	// A whole pixel of alpha, so a fade redraws every frame it is visibly moving and a held or
	// finished message redraws not at all. Comparing exactly would repaint on every frame of the
	// fade AND on none of the rest, which sounds the same and is not: the fade is 150ms and the
	// stillness is the entire rest of the session.
	const bool opacityMoved = std::abs(now - drawnOpacity_) >= 1.0f / 255.0f;
	if (!opacityMoved && !textChanged_) {
		return false;
	}
	drawnOpacity_ = now;
	textChanged_ = false;
	redraw();
	return true;
}

void NotifyTray::draw(visage::Canvas& canvas) {
	const float alpha = opacity();
	if (alpha <= 0.0f) {
		return;
	}

	const visage::Font& face = font(kReadoutSize);
	const float textWidth = trackedWidth(face, text_, 0.0f);
	const float boxWidth = textWidth + 2.0f * kPadX;
	const float boxHeight = kHeight;
	const float top = height() - boxHeight;

	// The mock's `background: rgba(7,10,18,.9)` over a 1px line, both taken down by the fade. The
	// readout sits ON the editor rather than in a reserved strip, so it has to carry its own ground
	// or it becomes unreadable over whatever it happens to cover.
	canvas.setColor(withAlpha(kNotifyGround, alpha * 0.9f));
	canvas.roundedRectangle(0.0f, top, boxWidth, boxHeight, 6.0f);
	canvas.setColor(withAlpha(kLine, alpha));
	canvas.roundedRectangleBorder(0.0f, top, boxWidth, boxHeight, 6.0f, 1.0f);

	canvas.setColor(withAlpha(kInk, alpha));
	canvas.text(text_, face, visage::Font::kLeft, kPadX, top, textWidth, boxHeight);
}

} // namespace feathers::ui
