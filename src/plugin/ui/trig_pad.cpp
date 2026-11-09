/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#include "plugin/ui/trig_pad.h"

#include "plugin/ui/menu_anchor.h"
#include "plugin/ui/mod_route_menu.h"

#include <algorithm>
#include <chrono>

namespace feathers::ui {

TrigPad::TrigPad(device::Modulatable& control, FireCount fires) : control_(control), fires_(std::move(fires)) {
	onMouseDown() = [this](const visage::MouseEvent& e) {
		// THE RIGHT-CLICK IS THE ROUTE MENU every other destination offers (`mod_route_menu.h`), and the normal's row;
		// with nothing routed in and no normal (backlog NU) there is no menu and the press does nothing, as before — a
		// right-click never fires.
		if (e.shouldTriggerPopup()) {
			(void)showRouteMenu();
			return;
		}
		press(e.isAltDown(), e.repeatClickCount());
	};
	onMouseUp() = [this](const visage::MouseEvent&) { release(); };
	installTooltipHover(*this, *this);
}

TrigPad::~TrigPad() {
	cancelHold();
}

void TrigPad::visibilityChanged() {
	if (!isVisible()) {
		cancelHold();
	}
}

bool TrigPad::showRouteMenu() {
	visage::PopupMenu menu = menuForTest();
	if (menu.size() == 0) {
		return false;
	}
	return showMenuAt(std::move(menu), *this, { 0.0f, 0.0f, width(), height() });
}

visage::PopupMenu TrigPad::menuForTest() {
	visage::PopupMenu menu;
	(void)appendRouteRows(menu, control_);
	if (!hasNormal()) {
		return menu;
	}
	// **THE NORMAL'S ROW, LAST AND CHECKABLE** (backlog NU): it shows its state before the pick, so the choice is on
	// the face (a two-state switch). Behind a break when routes are listed above it.
	if (menu.size() > 0) {
		menu.addBreak();
	}
	menu.addOption(kNormalRow, "Notes fire Trig").select(normalled());
	const visage::CallbackList<void(int)> earlier = menu.onSelection();
	const std::weak_ptr<int> token = alive_;
	menu.onSelection() = [this, token, earlier](int id) {
		if (token.expired()) {
			return;
		}
		if (id == kNormalRow) {
			setNormalled(!normalled());
			return;
		}
		earlier.callback(id);
	};
	return menu;
}

bool TrigPad::hasNormal() const {
	if (normal_.control == nullptr) {
		return false;
	}
	const double v = normal_.control->value();
	return v == normal_.made || v == normal_.broken;
}

bool TrigPad::normalled() const {
	return normal_.control != nullptr && normal_.control->value() == normal_.made;
}

void TrigPad::setNormalled(bool made) {
	if (!hasNormal() || normalled() == made) {
		return;
	}
	normal_.control->beginGesture();
	normal_.control->setValue(made ? normal_.made : normal_.broken);
	normal_.control->endGesture();
	redraw();
}

bool TrigPad::mappable() const {
	return control_.display().routable;
}

void TrigPad::press(bool altDown, int clicks) {
	if (held_ || routing_) {
		return;
	}
	// WITH A SOURCE ARMED: THE ROUTE, AT THE DEPTH A TRIGGER WANTS — or, on the second click of a pair, gone.
	if (!altDown && mappable()) {
		routing_ = true;
		if (clicks >= 2 && control_.display().amount != 0.0f) {
			(void)control_.dropRoute();
		}
		else {
			control_.beginAmountGesture();
			control_.setAmount(1.0f);
			control_.endAmountGesture();
		}
		redraw();
		return;
	}
	// OTHERWISE A FIRE BY HAND: the base held at the top for as long as the press.
	held_ = true;
	control_.beginGesture();
	control_.setValue(control_.desc().max);
	redraw();
}

void TrigPad::release() {
	routing_ = false;
	if (!held_) {
		return;
	}
	held_ = false;
	control_.setValue(control_.desc().min);
	control_.endGesture();
	redraw();
}

void TrigPad::cancelHold() {
	routing_ = false;
	if (!held_) {
		return;
	}
	held_ = false;
	control_.setValue(control_.desc().min);
	control_.endGesture();
}

bool TrigPad::poll(double nowSeconds) {
	const bool wasLit = lit();
	// THE NORMAL'S MARK FOLLOWS ITS CONTROL, however it moved (an undo, the host): redrawn when it changes.
	const bool normalNow = normalled();
	const bool normalMoved = normalNow != normalWas_;
	normalWas_ = normalNow;
	lastNow_ = nowSeconds;
	uint32_t fires = 0;
	if (fires_ && fires_(fires)) {
		// THE FIRST READING IS WHERE THE COUNT STANDS, not a fire: a card opened on a device that has fired before
		// must not flash for it.
		if (haveSeen_ && fires != seen_) {
			flashing_ = true;
			flashUntil_ = nowSeconds + kFlashSeconds;
		}
		haveSeen_ = true;
		seen_ = fires;
	}
	if (flashing_ && nowSeconds >= flashUntil_) {
		flashing_ = false;
	}
	if (lit() != wasLit || normalMoved) {
		redraw();
		return true;
	}
	return false;
}

bool TrigPad::poll() {
	using Clock = std::chrono::steady_clock;
	return poll(std::chrono::duration<double>(Clock::now().time_since_epoch()).count());
}

void TrigPad::draw(visage::Canvas& canvas) {
	const float w = static_cast<float>(width());
	const float h = static_cast<float>(height());
	const float d = std::min({ kDiameter, w, h });
	const float x = (w - d) * 0.5f;
	const float y = (h - d) * 0.5f;
	const bool on = lit();
	const device::ModDisplay state = control_.display();
	const bool armed = state.routable;
	// A ROUTE FROM ANY SOURCE marks it (backlog QK): one from an unfocused source drew nothing.
	const bool routed = routedNow();

	// THE JACK: a round socket, ringed. Amber filled when lit; the mapping colour around it while a source is armed
	// (the same signal every destination gives); a routed pad keeps a thin amber ring so a wired trigger reads as one.
	canvas.setColor(on ? kLitColour : kVoid);
	canvas.circle(x, y, d);
	canvas.setColor(armed ? kModulation : on || routed ? kLitColour : withAlpha(kLine, 0.95f));
	canvas.ring(x, y, d, armed ? 1.4f : 1.0f);

	// THE GLYPH, the same on every card: a rising edge, the thing that fires it.
	const float cx = x + d * 0.5f;
	const float lo = y + d * 0.68f;
	const float hi = y + d * 0.32f;
	canvas.setColor(on ? kInk : routed ? kLitColour : kMuted);
	canvas.segment(x + d * 0.24f, lo, cx, lo, 1.2f, true);
	canvas.segment(cx, lo, cx, hi, 1.2f, true);
	canvas.segment(cx, hi, x + d * 0.76f, hi, 1.2f, true);

	// THE NORMAL'S MARK (backlog NU): a dot over the rising edge while notes fire this Trig too — the note patched in.
	// Absent when the normal is broken, and on a pad that has none.
	if (normalled()) {
		const float dot = d * 0.18f;
		canvas.setColor(on ? kInk : kMuted);
		canvas.circle(cx - dot * 0.5f, y + d * 0.1f, dot);
	}
}

} // namespace feathers::ui
