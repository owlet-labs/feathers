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

#include "plugin/ui/orientation_row.h"

#include "dsp/slice_build.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <cstdio>

namespace feathers::ui {

namespace {

/// The slider's full throw, ± half a turn in the encoder's own unit. The Explore corner can carry a
/// turn far beyond this — the pin is an audition handle, not the verb — so the span only needs to
/// reach planes worth comparing, and a half turn each way is already a different country.
constexpr float kRotateSpan = 3.14159265f;

} // namespace

OrientationRow::OrientationRow(ParamBridge& bridge, std::function<void()> onChanged)
    : bridge_(bridge), onChanged_(std::move(onChanged)) {
	onMouseDown() = [this](const visage::MouseEvent& e) {
		// RIGHT-CLICK RELEASES THE PIN, and it is not a nicety. The judgment the user has to make is
		// whether the sounds' own turns are good ones, which is a comparison against a pinned turn —
		// and a comparison you can only make by restarting the host is not a comparison anyone makes
		// twice.
		if (e.isRightButton()) {
			bridge_.setOrientation({});
			redraw();
			if (onChanged_) {
				onChanged_();
			}
			return;
		}
		applyFrom(e);
	};
	onMouseDrag() = [this](const visage::MouseEvent& e) {
		if (e.isRightButton()) {
			return;
		}
		applyFrom(e);
	};
}

float OrientationRow::value() const {
	// UNPINNED READS ZERO, AND ZERO IS TRUE: an unturned sound plays at rotate 0 — the open plane its
	// own home gives it — so the handle at rest shows the turn everything is actually at, and a drag
	// starts from where the sounds already are instead of jumping. The old rows had to derive a
	// φ-orientation through the whole character stack to avoid lying at rest; the one-scalar decode
	// makes the honest reading a constant.
	return static_cast<float>(rotateFor(bridge_.orientation()));
}

void OrientationRow::applyFrom(const visage::MouseEvent& e) {
	const float w = width();
	if (w <= 0.0f) {
		return;
	}
	// `position`, not `relativePosition()` — the same trap ParamRow documents, and the same answer.
	const float t = std::clamp(e.position.x / w, 0.0f, 1.0f);

	// Built fresh rather than seeded: the decode reads one number, so the turn under the mouse IS the
	// whole pin.
	SliceOrientationPin pin;
	pin.generator.rotate = (t * 2.0f - 1.0f) * kRotateSpan;
	// TOUCHING THIS PINS THE TURN. Otherwise the slider would show a number and change nothing,
	// because an unpinned sound reads its own state and never looks here.
	pin.pinned = true;
	bridge_.setOrientation(pin);

	redraw();
	if (onChanged_) {
		// The waveform readout draws the character it is about to hear, and it redraws when a watched
		// PARAMETER moves. The rotate pin is not a parameter, so nothing would tell it — and a picture
		// that lags the sound during a sweep is worse than no picture, because it is confidently
		// wrong.
		onChanged_();
	}
}

void OrientationRow::draw(visage::Canvas& canvas) {
	const float w = width();
	const float h = height();
	const float trackY = h * 0.55f;
	const float trackH = std::max(2.0f, h * 0.28f);
	const float v = value();

	canvas.setColor(kSurfaceRaised);
	canvas.fill(0.0f, trackY, w, trackH);

	canvas.setColor(kValue);
	// Filled from the centre, because zero is the meaningful place for a turn rather than one end of
	// its travel: a rotation is judged by how far it has been turned from rest.
	const float t = std::clamp((v / kRotateSpan) * 0.5f + 0.5f, 0.0f, 1.0f);
	const float mid = w * 0.5f;
	canvas.fill(std::min(mid, w * t), trackY, std::abs(w * t - mid), trackH);

	// The label says which of the two worlds is live, because a slider showing 0.000 while a sound
	// plays a turn of its own would be a control lying about what it does. Right-click to get back.
	const bool pinned = bridge_.orientation().pinned;
	char label[48] = {};
	std::snprintf(label, sizeof(label), pinned ? "ROTATE (PINNED)" : "ROTATE (PER-SOUND)");
	char printed[32] = {};
	std::snprintf(printed, sizeof(printed), "%.3f", static_cast<double>(v));

	const visage::Font& face = font(std::max(8.0f, h * 0.34f));
	canvas.setColor(kMuted);
	canvas.text(label, face, visage::Font::kLeft, 0.0f, 0.0f, w, trackY);
	canvas.text(printed, face, visage::Font::kRight, 0.0f, 0.0f, w, trackY);
}

} // namespace feathers::ui
