/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */


#include "plugin/ui/route_switch.h"

#include "plugin/ui/knob.h"
#include "plugin/ui/notify_tray.h"
#include "plugin/ui/theme.h"

#include <algorithm>

namespace feathers::ui {

RouteSwitch::RouteSwitch(device::Modulatable& control, Notifier* notifier) : control_(control), notifier_(notifier) {
	onMouseDown() = [this](const visage::MouseEvent& e) {
		// THE LEFT BUTTON ONLY: a right-click is a menu's (a popup), never a press of the switch.
		if (!usable() || e.shouldTriggerPopup()) {
			return;
		}
		// `position`, not `relativePosition()` — the latter is mouse movement.
		select(choiceAt(e.position));
	};
	// LAST, after every `=` above — see `installTooltipHover`.
	installTooltipHover(*this, *this);
}

float RouteSwitch::heightFor(float faceDiameter) { return Knob::columnHeight(faceDiameter); }

int RouteSwitch::litChoice() const {
	const device::ParamDesc& d = control_.desc();
	return control_.value() >= (d.min + d.max) * 0.5 ? Parallel : Series;
}

visage::Bounds RouteSwitch::glyphBounds(int choice) const {
	const float w = static_cast<float>(width());
	const float h = static_cast<float>(height());
	const float each = std::max(0.0f, (h - kGlyphGap) * 0.5f);
	return { 0.0f, choice == Series ? 0.0f : each + kGlyphGap, w, each };
}

int RouteSwitch::choiceAt(visage::Point position) const {
	for (const int choice : { static_cast<int>(Series), static_cast<int>(Parallel) }) {
		const visage::Bounds b = glyphBounds(choice);
		if (position.x >= b.x() && position.x < b.right() && position.y >= b.y() && position.y < b.bottom()) {
			return choice;
		}
	}
	return -1;
}

void RouteSwitch::select(int choice) {
	if (control_.unavailable() != nullptr || (choice != Series && choice != Parallel) || choice == litChoice()) {
		return;
	}
	const device::ParamDesc& d = control_.desc();
	const double before = control_.value();
	// ONE COMPLETE EDIT, bracketed, so the host records a single undoable step.
	control_.beginGesture();
	control_.setValue(choice == Series ? d.min : d.max);
	control_.endGesture();
	if (control_.value() == before) {
		return; // the control refused it — say nothing rather than report an edit that did not happen
	}
	if (notifier_ != nullptr) {
		notifier_->say(std::string(d.name) + (choice == Series ? " Series" : " Parallel"));
	}
	redraw();
}

void RouteSwitch::setEnabled(bool enabled) {
	if (enabled_ != enabled) {
		enabled_ = enabled;
		redraw();
	}
}

void RouteSwitch::drawGlyph(visage::Canvas& canvas, const visage::Bounds& plate, int choice, bool lit) const {
	// THE CHIP'S OWN FILL, BORDER AND LIT RULE (`SwitchChip::draw`): a lit plate is one accent wash and a selected outline.
	canvas.setColor(lit ? withAlpha(kAccent, 0.22f) : kVoid);
	canvas.roundedRectangle(plate.x(), plate.y(), plate.width(), plate.height(), 3.0f);
	canvas.setColor(lit ? kSelectedOutline : withAlpha(kLine, 0.8f));
	canvas.roundedRectangleBorder(plate.x(), plate.y(), plate.width(), plate.height(), 3.0f, 1.0f);

	canvas.setColor(lit ? kInk : kMuted);
	const float cx = std::floor(plate.x() + plate.width() * 0.5f);
	const float cy = std::floor(plate.y() + plate.height() * 0.5f);
	const float s = 6.0f;      // a square's side
	const float stub = 5.0f;   // the line's reach from the plate's edge
	const float top = plate.y() + stub;
	const float bottom = plate.bottom() - stub;
	if (choice == Series) {
		// ONE VERTICAL LINE THROUGH TWO SQUARES, one above the other.
		canvas.fill(cx, top, 1.0f, bottom - top);
		canvas.fill(cx - s * 0.5f + 0.5f, cy - s - 2.0f, s, s);
		canvas.fill(cx - s * 0.5f + 0.5f, cy + 2.0f, s, s);
		return;
	}
	// A LINE FORKING ABOVE, TWO SQUARES SIDE BY SIDE, AND A LINE JOINING BELOW.
	const float arm = 4.0f + s * 0.5f; // the squares' centres, either side of the middle
	const float leftX = std::floor(cx - arm);
	const float rightX = std::floor(cx + arm);
	const float forkY = top + 3.0f;
	const float joinY = bottom - 3.0f;
	const float squareY = cy - s * 0.5f;
	canvas.fill(cx, top, 1.0f, forkY - top);
	canvas.fill(leftX, forkY, rightX - leftX + 1.0f, 1.0f);
	canvas.fill(leftX, forkY, 1.0f, squareY - forkY);
	canvas.fill(rightX, forkY, 1.0f, squareY - forkY);
	canvas.fill(leftX - s * 0.5f + 0.5f, squareY, s, s);
	canvas.fill(rightX - s * 0.5f + 0.5f, squareY, s, s);
	canvas.fill(leftX, squareY + s, 1.0f, joinY - squareY - s);
	canvas.fill(rightX, squareY + s, 1.0f, joinY - squareY - s);
	canvas.fill(leftX, joinY, rightX - leftX + 1.0f, 1.0f);
	canvas.fill(cx, joinY, 1.0f, bottom - joinY);
}

void RouteSwitch::draw(visage::Canvas& canvas) {
	// GREYED: never lit, the glyphs muted — a greyed control that still highlights its choice reads as available.
	const int lit = usable() ? litChoice() : -1;
	for (const int choice : { static_cast<int>(Series), static_cast<int>(Parallel) }) {
		drawGlyph(canvas, glyphBounds(choice), choice, choice == lit);
	}
}

} // namespace feathers::ui
