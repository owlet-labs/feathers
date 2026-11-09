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

#include "plugin/ui/widget_registry.h"
#include "plugin/ui/param_row.h"

#include "plugin/ui/theme.h"

#include <algorithm>

namespace feathers::ui {

ParamRow::ParamRow(ParamBridge& bridge, WidgetRegistry& registry, const ParamDesc& desc)
    : bridge_(bridge), desc_(desc) {
	watch_ = registry.watchToken(static_cast<clap_id>(desc.id), this);

	onMouseDown() = [this](const visage::MouseEvent& e) {
		bridge_.beginGesture(id());
		applyFrom(e);
	};
	onMouseDrag() = [this](const visage::MouseEvent& e) { applyFrom(e); };
	onMouseUp() = [this](const visage::MouseEvent&) { bridge_.endGesture(id()); };
}

float ParamRow::normalized(double value) const noexcept {
	const double span = desc_.max - desc_.min;
	if (span <= 0.0) {
		return 0.0f;
	}
	return static_cast<float>(std::clamp((value - desc_.min) / span, 0.0, 1.0));
}

void ParamRow::applyFrom(const visage::MouseEvent& e) {
	const float w = width();
	if (w <= 0.0f) {
		return;
	}
	// `position`, NOT `relativePosition()`. visage's names invite the opposite reading: `position` is
	// the point within this frame, while `relative_position` is how far the mouse MOVED since the
	// previous event, for relative-mouse mode. Reading the movement here set the parameter from a
	// delta — zero on a press, a few pixels on each drag event — so every press snapped the value to
	// the minimum and no drag could climb away from it. Every visage widget of their own reads
	// `position`; so do we.
	const double t = std::clamp(e.position.x / w, 0.0f, 1.0f);
	// Unrounded even for a stepped parameter: the bridge sanitises on the way in, so the rounding
	// lives in one place rather than being re-derived by every control that writes a value.
	bridge_.setValue(id(), desc_.min + t * (desc_.max - desc_.min));
	redraw();
}

void ParamRow::draw(visage::Canvas& canvas) {
	const float w = width();
	const float h = height();
	const float trackY = h * 0.55f;
	const float trackH = std::max(2.0f, h * 0.28f);
	// One read of the published value, used three times below. It is a cheap lookup rather than an
	// expensive one, but reading it repeatedly inside a draw invites the three uses to disagree the
	// day it stops being cheap.
	const double value = bridge_.value(id());

	canvas.setColor(kSurfaceRaised);
	canvas.fill(0.0f, trackY, w, trackH);

	canvas.setColor(kValue);
	canvas.fill(0.0f, trackY, w * normalized(value), trackH);

	// The modulated reading only earns a mark when it differs, so an unmodulated editor is not covered
	// in ticks that never move.
	const double modulated = bridge_.modulated(id());
	if (modulated != value) {
		canvas.setColor(kModulation);
		canvas.fill(w * normalized(modulated) - 1.0f, trackY - trackH * 0.4f, 2.0f, trackH * 1.8f);
	}

	// Cached rather than built here: constructing a Font rasterises, and a fresh one per widget per
	// frame was a lookup 24 times a frame for no reason.
	const visage::Font& face = font(std::max(8.0f, h * 0.34f));
	canvas.setColor(kMuted);
	canvas.text(desc_.name, face, visage::Font::kLeft, 0.0f, 0.0f, w, trackY);

	char printed[32] = {};
	if (valueToText(id(), value, printed, sizeof(printed))) {
		canvas.text(printed, face, visage::Font::kRight, 0.0f, 0.0f, w, trackY);
	}
}

} // namespace feathers::ui
