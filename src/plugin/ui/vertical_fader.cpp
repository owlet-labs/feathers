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

#include "plugin/ui/vertical_fader.h"

#include "plugin/ui/mod_route_menu.h"
#include "plugin/ui/mod_state.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace feathers::ui {

namespace {
constexpr float kRadius = 4.0f;
constexpr float kTrackInset = 4.0f;
} // namespace

VerticalFader::VerticalFader(device::Modulatable& control, std::string caption)
    : control_(control), caption_(std::move(caption)) {
	onMouseDown() = [this](const visage::MouseEvent& e) {
		// THE INBOUND-ROUTE MENU FIRST — a right-click is a different gesture, never a quiet drag.
		if (e.shouldTriggerPopup() && showRouteMenu(*this, control_)) {
			return;
		}
		dragged_ = false;
		startY_ = e.position.y;
		routing_ = mappable(e.isAltDown());
		if (routing_) {
			// DOUBLE-CLICK UNROUTES in mapping mode — the knob's split: the thing under the hand is the amount.
			if (e.repeatClickCount() >= 2) {
				routing_ = false;
				if (control_.dropRoute()) {
					redraw();
				}
				return;
			}
			pressed_ = true;
			control_.beginAmountGesture();
			const float before = control_.display().amount;
			// A PRESS ON AN UNROUTED DESTINATION SEEDS AN AMOUNT, so the reach is under the hand from the first frame.
			anchorAmount_ = before != 0.0f ? before : kRouteSeedAmount;
			control_.setAmount(anchorAmount_);
			redraw();
			return;
		}
		// DOUBLE-CLICK RESETS, bracketed as one undoable step by the control's own reset.
		if (e.repeatClickCount() >= 2) {
			if (control_.resetToDefault()) {
				say(caption_ + " " + control_.text());
				redraw();
			}
			return;
		}
		pressed_ = true;
		startValue_ = control_.value();
	};

	onMouseDrag() = [this](const visage::MouseEvent& e) {
		if (!pressed_) {
			return;
		}
		const float up = startY_ - e.position.y;
		if (routing_) {
			control_.setAmount(std::clamp(anchorAmount_ + up / kRouteDragRange, -device::kMaxAmount, device::kMaxAmount));
			redraw();
			return;
		}
		if (!dragged_ && std::fabs(up) <= kSlop) {
			return; // a hand is never perfectly still — inside the slop a press is still a click
		}
		if (!dragged_) {
			dragged_ = true;
			control_.beginGesture();
		}
		const device::ParamDesc& d = control_.desc();
		const double span = d.max - d.min;
		control_.setValue(std::clamp(startValue_ + static_cast<double>(up / kSpan) * span, d.min, d.max));
		redraw();
	};

	onMouseUp() = [this](const visage::MouseEvent&) {
		if (!pressed_) {
			return;
		}
		pressed_ = false;
		if (routing_) {
			routing_ = false;
			control_.endAmountGesture();
			redraw();
			return;
		}
		if (dragged_) {
			dragged_ = false;
			control_.endGesture();
			say(caption_ + " " + control_.text());
			redraw();
		}
	};
	// LAST, after every `=` above — see `installTooltipHover`.
	installTooltipHover(*this, *this);
}

std::string VerticalFader::readout() const {
	const double v = control_.value();
	if (std::fabs(v) < 0.05) {
		return "0";
	}
	// ONE DECIMAL WHILE IT FITS, whole numbers past ten: "+9.5" and "+24" are both four characters, which is what a
	// bar this narrow holds.
	char text[16];
	std::snprintf(text, sizeof(text), std::fabs(v) < 9.95 ? "%+.1f" : "%+.0f", v);
	return text;
}

visage::Bounds VerticalFader::track() const {
	return { kTrackInset, kTextHeight, std::max(0.0f, width() - 2.0f * kTrackInset),
		     std::max(0.0f, height() - 2.0f * kTextHeight) };
}

float VerticalFader::yOf(double normalized) const {
	const visage::Bounds t = track();
	return t.bottom() - static_cast<float>(std::clamp(normalized, 0.0, 1.0)) * t.height();
}

void VerticalFader::refreshWatch() {
	ModState* state = routingStateOf(control_);
	if (state == watched_) {
		return;
	}
	watched_ = state;
	watch_ = state != nullptr ? state->watch(this) : Watch {};
}

void VerticalFader::draw(visage::Canvas& canvas) {
	refreshWatch();
	const float w = width();
	const float h = height();
	const device::ModDisplay mod = control_.display();
	const bool lit = mod.isDestination && mod.armed;
	const visage::Bounds t = track();
	const device::ParamDesc& d = control_.desc();
	const double span = d.max - d.min;
	const double rest = span > 0.0 ? std::clamp((d.def - d.min) / span, 0.0, 1.0) : 0.0;
	const double base = std::clamp(control_.normalized(), 0.0, 1.0);

	canvas.setColor(withAlpha(kSurfaceRaised, 0.9f));
	canvas.roundedRectangle(0.0f, 0.0f, w, h, kRadius);

	// THE TRACK, and the rest ticked across it.
	canvas.setColor(withAlpha(kVoid, 0.45f));
	canvas.roundedRectangle(t.x(), t.y(), t.width(), t.height(), 2.0f);

	// THE VALUE — `kValue`, the knob's own value colour, from the rest to where the hand put it.
	const float yRest = yOf(rest);
	const float yBase = yOf(base);
	if (std::fabs(yBase - yRest) > 0.5f) {
		canvas.setColor(withAlpha(kValue, 0.85f));
		canvas.rectangle(t.x(), std::min(yRest, yBase), t.width(), std::fabs(yBase - yRest));
	}
	canvas.setColor(withAlpha(kInk, 0.55f));
	canvas.rectangle(t.x() - 2.0f, yRest - 0.5f, t.width() + 4.0f, 1.0f);

	// THE REACH — how far a full-strength source would carry the value, in the modulation colour across the track:
	// brighter while that source is armed. `FaderPill`'s bar, stood up.
	if (mod.isDestination) {
		const device::Modulatable::Span reach = control_.amountSpan();
		if (reach.hi > reach.lo) {
			canvas.setColor(withAlpha(kModulation, lit ? 0.34f : 0.20f));
			const float top = yOf(reach.hi);
			canvas.rectangle(0.0f, top, w, yOf(reach.lo) - top);
		}
		// NO BAND TO DRAW, AND STILL ROUTED (backlog QK): a route whose reach clamps, or another source's — the dot says so.
		else if (mod.amount != 0.0f || mod.routedElsewhere) {
			canvas.setColor(withAlpha(kModulation, 0.5f));
			canvas.circle(2.0f, kTextHeight - 1.0f, 3.0f);
		}
		// THE MAPPING SIGNAL (backlog QK): the border in the modulation colour while the armed source may reach it — the
		// knob's and the box's glow, which this fader only ever drew as a brighter band once a route existed.
		// THE MODULATED VALUE, LIVE — where modulation has put it this frame, so a moving value is seen moving.
		const double live = std::clamp(control_.normalizedModulated(), 0.0, 1.0);
		if (std::fabs(live - base) > 1e-4) {
			canvas.setColor(withAlpha(kVoid, 0.8f)); // the keyline, as the knob's dot wears
			canvas.roundedRectangle(1.0f, yOf(live) - 2.0f, w - 2.0f, 4.0f, 1.5f);
			canvas.setColor(kModulatedDot);
			canvas.roundedRectangle(1.0f, yOf(live) - 1.0f, w - 2.0f, 2.0f, 1.0f);
		}
	}

	// THE BORDER IS THE MAPPING SIGNAL — the modulation colour while an armed source could route here.
	if (lit) {
		drawBoxOutline(canvas, 0.0f, 0.0f, w, h, kRadius, boxOutlineOf(false, true, true));
	}
	else {
		canvas.setColor(withAlpha(kLine, 0.6f));
		canvas.roundedRectangleBorder(0.0f, 0.0f, w, h, kRadius, 1.0f);
	}

	canvas.setColor(kInk);
	canvas.text(readout(), font(kKnobLabelSize), visage::Font::kCenter, 0.0f, 0.0f, w, kTextHeight);
	canvas.setColor(kMuted);
	canvas.text(caption_, font(kKnobLabelSize), visage::Font::kCenter, 0.0f, h - kTextHeight, w, kTextHeight);
}

} // namespace feathers::ui
