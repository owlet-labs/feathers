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

#include "plugin/ui/encoder_face.h"

#include "plugin/ui/knob.h"
#include "plugin/ui/theme.h"

#include <cmath>

namespace feathers::ui {

namespace {

constexpr float kPi = 3.14159265358979323846f;

} // namespace

EncoderFace::EncoderFace(const char* caption, Hooks hooks, MacroMenuHooks menu)
    : caption_(caption), hooks_(std::move(hooks)), menu_(std::move(menu)), faceDiameter_(Knob::kDiameter) {
	onMouseDown() = [this](const visage::MouseEvent& e) {
		if (e.shouldTriggerPopup()) {
			(void)showMacroMenu(*this, menu_);
			return;
		}
		lastY_ = e.position.y;
		turning_ = true;
		if (hooks_.begin) {
			hooks_.begin();
		}
	};
	onMouseDrag() = [this](const visage::MouseEvent& e) {
		if (!turning_) {
			return;
		}
		const float dy = lastY_ - e.position.y; // up is clockwise, as on a knob
		lastY_ = e.position.y;
		const double delta = static_cast<double>(dy / kDragRange * (e.isShiftDown() ? kFineFactor : 1.0f));
		if (delta == 0.0) {
			return;
		}
		travel_ += delta;
		if (hooks_.turn) {
			hooks_.turn(delta);
		}
		redraw();
	};
	onMouseUp() = [this](const visage::MouseEvent&) {
		if (!turning_) {
			return;
		}
		turning_ = false;
		if (hooks_.end) {
			hooks_.end();
		}
	};
	// LAST, after every `=` above — see `installTooltipHover`. It had no hover tooltip at all before this.
	installTooltipHover(*this, *this);
}

std::optional<LabelFace> EncoderFace::labelFaceAt(visage::Point) const {
	if (caption_ == nullptr || caption_[0] == '\0') {
		return std::nullopt;
	}
	// THE KNOB'S RULE, THE KNOB'S FIT: a macro's caption is a name (`Knob::columnName`), cut only past the floor size.
	const FittedName drawn = Knob::columnName(caption_, static_cast<float>(width()));
	return LabelFace { .label = caption_, .full = fullName_.empty() ? std::string(caption_) : fullName_, .cut = drawn.elided };
}

void EncoderFace::draw(visage::Canvas& canvas) {
	const float centreX = width() * 0.5f;
	const float centreY = faceDiameter_ * 0.5f;
	// THE WHOLE CIRCLE AS THE TRACK — a knob's track stops at 270°, and the gap is what says "an end is here".
	// An encoder has none, so the ring is closed.
	const float trackRadius = faceDiameter_ * (19.0f / Knob::kDiameter);
	canvas.setColor(kLine);
	canvas.ring(centreX - trackRadius, centreY - trackRadius, trackRadius * 2.0f, Knob::kTrackWidth);
	// THE POINTER FOLLOWS THE HAND AND WRAPS: one unit of travel is one whole turn.
	const float angle = -0.5f * kPi + 2.0f * kPi * static_cast<float>(travel_ - std::floor(travel_));
	const float inner = faceDiameter_ * (9.0f / Knob::kDiameter);
	const float outer = faceDiameter_ * (15.0f / Knob::kDiameter);
	canvas.setColor(turning_ ? kValue : kPointer);
	canvas.segment(centreX + std::cos(angle) * inner, centreY + std::sin(angle) * inner,
	               centreX + std::cos(angle) * outer, centreY + std::sin(angle) * outer, 2.0f, true);

	canvas.setColor(kMuted);
	const FittedName name = Knob::columnName(caption_, static_cast<float>(width()));
	Knob::drawColumnCaption(canvas, name.text, static_cast<float>(width()), faceDiameter_ + Knob::kGap, name.size);
	// NO VALUE TO PRINT — the reading line says what the control is instead.
	canvas.text("Relative", font(kKnobReadoutSize), visage::Font::kCenter, 0.0f,
	            faceDiameter_ + Knob::kGap + Knob::kLabelHeight + Knob::kLineGap, width(), Knob::kReadoutHeight);
}

} // namespace feathers::ui
