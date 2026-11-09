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

#include "plugin/ui/fader_pill.h"

#include "plugin/ui/mod_route_menu.h"
#include "plugin/ui/mod_state.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>

namespace feathers::ui {

namespace {
constexpr float kRadius = 4.0f;
constexpr float kRailInset = 5.0f;
} // namespace

FaderPill::FaderPill(device::Modulatable& amount, device::Modulatable& power, std::string caption)
    : FaderPill(amount, std::move(caption)) {
	power_ = &power;
}

FaderPill::FaderPill(device::Modulatable& amount, std::string caption) : amount_(amount), caption_(std::move(caption)) {
	// THE HOVER IS TRACKED, AND NO LONGER CHANGES THE CURSOR — the side-to-side arrow cursor was
	// dropped. What says "fader" before it is touched is the kept amount's dim fill and the rail.
	onMouseEnter() = [this](const visage::MouseEvent&) { setHover(true); };
	onMouseMove() = [this](const visage::MouseEvent&) { setHover(true); };
	onMouseExit() = [this](const visage::MouseEvent&) { setHover(false); };

	onMouseDown() = [this](const visage::MouseEvent& e) {
		const bool onGlyph = powerGlyphShown() && powerGlyph().contains(e.position);
		device::Modulatable& control = onGlyph ? *power_ : amount_;
		// THE INBOUND-ROUTE MENU FIRST — a right-click is a different gesture, never a quiet drag. On the
		// symbol it lists the POWER's routes, anywhere else the amount's.
		if (e.shouldTriggerPopup() && showRouteMenu(*this, control)) {
			return;
		}
		target_ = onGlyph ? Target::Power : Target::Amount;
		dragged_ = false;
		startX_ = e.position.x;
		startY_ = e.position.y;
		routing_ = mappable(control, e.isAltDown());
		if (routing_) {
			// DOUBLE-CLICK UNROUTES in mapping mode — the knob's and the box's split: the thing under the
			// hand is the amount, and a reset of the value would answer a gesture nobody made.
			if (e.repeatClickCount() >= 2) {
				control.setAmount(0.0f);
				routing_ = false;
				target_ = Target::None;
				redraw();
				return;
			}
			control.beginAmountGesture();
			const float before = control.display().amount;
			// A PRESS ON AN UNROUTED DESTINATION SEEDS AN AMOUNT, so the reach is under the hand from the
			// first frame — the knob's rule.
			anchorAmount_ = before != 0.0f ? before : kRouteSeedAmount;
			control.setAmount(anchorAmount_);
			redraw();
			return;
		}
		// **A DOUBLE-CLICK ON AN AMOUNT-ONLY PILL RESETS IT** (horizontal faders without a toggle did not
		// reset on double-click) — the knob's and the vertical fader's rule (`resetToDefault`, one undo
		// step). A pill WITH a power keeps its own convention: every click toggles the power, so a double-click is two toggles,
		// and resetting the amount underneath would answer a gesture aimed at the switch.
		if (power_ == nullptr && e.repeatClickCount() >= 2) {
			target_ = Target::None;
			if (amount_.resetToDefault()) {
				say(caption_ + " " + amount_.text());
				redraw();
			}
			return;
		}
		startValue_ = amount_.value();
	};

	onMouseDrag() = [this](const visage::MouseEvent& e) {
		if (target_ == Target::None) {
			return;
		}
		const float dx = e.position.x - startX_;
		if (routing_) {
			// **RIGHT OR UP, EITHER ONE.** The power symbol routed only on a SIDEWAYS drag — the
			// pill's own axis — where every knob and box routes on an upward one. A route drag takes both,
			// so a hand that knows either gesture finds it works.
			const float travel = dx + (startY_ - e.position.y);
			device::Modulatable& control = target_ == Target::Power ? *power_ : amount_;
			control.setAmount(std::clamp(anchorAmount_ + travel / kRouteDragRange, -device::kMaxAmount,
			                             device::kMaxAmount));
			redraw();
			return;
		}
		if (!dragged_ && std::fabs(dx) <= kSlop) {
			return; // a hand is never perfectly still — inside the slop a press is still a click
		}
		if (!dragged_) {
			dragged_ = true;
			amount_.beginGesture();
		}
		const device::ParamDesc& d = amount_.desc();
		const double span = d.max - d.min;
		amount_.setValue(std::clamp(startValue_ + static_cast<double>(dx / kSpan) * span, d.min, d.max));
		redraw();
	};

	onMouseUp() = [this](const visage::MouseEvent&) {
		if (target_ == Target::None) {
			return;
		}
		const Target was = target_;
		target_ = Target::None;
		if (routing_) {
			routing_ = false;
			(was == Target::Power ? *power_ : amount_).endAmountGesture();
			redraw();
			return;
		}
		if (dragged_) {
			amount_.endGesture();
			const auto percent = static_cast<int>(std::lround(amount_.normalized() * 100.0));
			say(caption_ + " " + std::to_string(percent) + "%");
			redraw();
			return;
		}
		if (power_ == nullptr) {
			return; // AMOUNT ONLY: a click is nothing (the section's chip is the switch)
		}
		// A CLICK IS THE POWER, and the amount stays where it was.
		const bool wasOn = power_->value() >= 0.5;
		power_->beginGesture();
		power_->setValue(wasOn ? power_->desc().min : power_->desc().max);
		power_->endGesture();
		say(caption_ + (wasOn ? " off" : " on"));
		redraw();
	};
	// LAST, after every `=` above — see `installTooltipHover`.
	installTooltipHover(*this, *this);
}

const char* FaderPill::tooltipAt(visage::Point local) const {
	// THE PRESS'S OWN SPLIT: over the power symbol the thing under the hand is the power, everywhere else
	// the amount — so the tooltip describes what a press there would reach.
	const bool onGlyph = powerGlyphShown() && powerGlyph().contains(local);
	return tooltipOf(onGlyph ? power_ : &amount_);
}

std::optional<LabelFace> FaderPill::labelFaceAt(visage::Point local) const {
	if (caption_.empty() || (powerGlyphShown() && powerGlyph().contains(local))) {
		return std::nullopt;
	}
	// `draw`'s rooms: the whole pill centred, or its left half beside a readout (less the inset).
	const auto w = static_cast<float>(width());
	const float room = readout_.empty() ? w : w * 0.5f - 7.0f;
	const char* name = amount_.desc().name;
	return LabelFace { .label = caption_,
		               .full = name != nullptr ? name : caption_,
		               .cut = textWidth(caption_, font(kKnobLabelSize)) > room };
}

void FaderPill::setActive(bool active) {
	if (active == active_) {
		return;
	}
	active_ = active;
	redraw();
}

void FaderPill::setHover(bool over) {
	if (over == hovering_) {
		return;
	}
	hovering_ = over;
}

bool FaderPill::powered() const {
	// WHERE MODULATION HAS PUT IT, not the base — a gate routed to the power turns the pill on and off
	// in front of you, and a pill that showed the base would say Off while the sound was on.
	return power_ == nullptr || power_->normalizedModulated() >= 0.5;
}

bool FaderPill::powerRingShown() const {
	// ROUTED, AND NOT ARMED — armed, the symbol says it instead.
	if (power_ == nullptr) {
		return false;
	}
	const device::ModDisplay p = power_->display();
	if (!p.isDestination || p.armed) {
		return false;
	}
	const device::Modulatable::Span span = power_->amountSpan();
	return span.hi > span.lo || p.amount != 0.0f || p.routedElsewhere; // a route whose reach clamps is a route

}

bool FaderPill::powerGlyphShown() const {
	if (power_ == nullptr) {
		return false;
	}
	const device::ModDisplay p = power_->display();
	return p.isDestination && p.armed;
}

visage::Bounds FaderPill::powerGlyph() const {
	const float d = std::max(height() - 4.0f, 6.0f);
	return { width() - d - 2.0f, (height() - d) * 0.5f, d, d };
}

bool FaderPill::mappable(const device::Modulatable& control, bool altDown) {
	// ALT EDITS THE VALUE UNDERNEATH; otherwise ONE answer, asked of the control — `ValueBox::mappable`'s
	// note is why it is one: armed AND this source may actually reach here.
	return !altDown && control.display().routable;
}

void FaderPill::refreshWatch() {
	ModState* state = routingStateOf(amount_);
	if (state == watched_) {
		return;
	}
	watched_ = state;
	watch_ = state != nullptr ? state->watch(this) : Watch {};
}

bool FaderPill::captionFits() const {
	return textWidth(caption_, font(kKnobLabelSize)) <= static_cast<float>(width()) - 2.0f;
}

void FaderPill::draw(visage::Canvas& canvas) {
	refreshWatch();
	const float w = width();
	const float h = height();
	const device::ModDisplay mod = amount_.display();
	const bool lit = mod.isDestination && mod.armed;
	const bool on = powered();
	const double base = std::clamp(amount_.normalized(), 0.0, 1.0);

	canvas.setColor(withAlpha(kSurfaceRaised, 0.9f));
	canvas.roundedRectangle(0.0f, 0.0f, w, h, kRadius);

	// THE AMOUNT — `kValue`, the knob's own value colour, never `kAccent`: the mini slider's face is
	// the same colour as a normal knob's displacement, to prevent confusion under mod glow. Full
	// while powered, dim while off — the amount is KEPT while off, and the pill says so.
	if (base > 0.0) {
		const float alpha = on ? (active_ ? 0.85f : 0.3f) : 0.22f;
		canvas.setColor(withAlpha(kValue, alpha));
		canvas.roundedRectangle(0.0f, 0.0f, std::max(static_cast<float>(base) * w, 2.0f * kRadius), h, kRadius);
	}

	// THE REACH — how far a full-strength source would carry the amount, the box's full-height bar in
	// the modulation colour: brighter while that source is armed.
	if (mod.isDestination) {
		const device::Modulatable::Span span = amount_.amountSpan();
		if (span.hi > span.lo) {
			canvas.setColor(withAlpha(kModulation, lit ? 0.34f : 0.20f));
			canvas.rectangle(span.lo * w, 0.0f, (span.hi - span.lo) * w, h);
		}
		else if (mod.routedElsewhere || mod.amount != 0.0f) { // a route whose reach clamps is a route
			canvas.setColor(withAlpha(kModulation, 0.5f));
			canvas.circle(2.0f, 2.0f, 3.0f);
		}
	}

	// THE RAIL — what makes it read as a fader before it is touched, with the field's own (50%) ticked.
	//
	// **NO HANDLE.** There was one, standing at the amount: exactly at the fill's edge, so it rode the
	// meniscus as a second mark and read as a glitch — a small tick following the meniscus.
	// The fill's edge IS the amount, and a kept amount draws dim
	// while off, so a handle said nothing the fill did not.
	const float railLeft = kRailInset;
	const float railRight = w - kRailInset;
	const float railY = h - 3.5f;
	canvas.setColor(withAlpha(base > 0.0 ? kVoid : kLine, active_ ? 0.45f : 0.2f));
	canvas.segment(railLeft, railY, railRight, railY, 1.0f, true);
	const float mid = (railLeft + railRight) * 0.5f;
	canvas.segment(mid, railY - 2.0f, mid, railY + 2.0f, 1.0f, true); // the field's own, 50%

	// THE MODULATED AMOUNT, LIVE — where modulation has put it this frame, in the modulation colour, so
	// a moving amount is seen moving (the knob's live dot, as a pill has room for it).
	if (mod.isDestination) {
		const double live = std::clamp(amount_.normalizedModulated(), 0.0, 1.0);
		if (std::fabs(live - base) > 1e-4) {
			const float x = railLeft + (railRight - railLeft) * static_cast<float>(live);
			canvas.setColor(withAlpha(kModulation, 0.95f));
			canvas.roundedRectangle(x - 1.0f, 2.0f, 2.0f, h - 4.0f, 1.0f);
		}
	}

	// THE BORDER IS THE MAPPING SIGNAL — the modulation colour while an armed source could route here.
	// A GREYED PILL'S SIGNAL IS DIMMER (QK, by design): plainly lit, plainly not the live pill beside it.
	canvas.setColor(lit ? (active_ ? kModulation : withAlpha(kModulation, 0.5f)) : withAlpha(kLine, active_ ? 0.6f : 0.3f));
	canvas.roundedRectangleBorder(0.0f, 0.0f, w, h, kRadius, 1.0f);

	canvas.setColor(!active_ ? kDisabled : (on ? kInk : kMuted));
	if (readout_.empty()) {
		canvas.text(caption_, font(kKnobLabelSize), visage::Font::kCenter, 0.0f, 0.0f, w, h);
	}
	else {
		// CAPTION LEFT, READOUT RIGHT — clear of the power ring, which sits at the right end when it shows.
		constexpr float kTextInset = 7.0f;
		const float right = powerRingShown() ? std::max(h - 10.0f, 5.0f) + 6.0f : 0.0f;
		canvas.text(caption_, font(kKnobLabelSize), visage::Font::kLeft, kTextInset, 0.0f, w * 0.5f, h);
		canvas.text(readout_, font(kKnobLabelSize), visage::Font::kRight, w * 0.35f, 0.0f,
		            std::max(0.0f, w * 0.65f - kTextInset - right), h);
	}

	// **THE POWER'S MODULATION, SHOWN** — the power ('active') had no modulated-amount display.
	// The amount has its reach bar and its live mark; the power had nothing outside the
	// armed symbol. Its REACH is how far a full-strength route carries the switch (`amountSpan`, 0..1);
	// its LIVE state is `powered()`, which already follows modulation.
	const device::ModDisplay powerMod = power_ != nullptr ? power_->display() : device::ModDisplay {};
	const device::Modulatable::Span powerSpan =
	    powerMod.isDestination && power_ != nullptr ? power_->amountSpan() : device::Modulatable::Span {};
	const float powerReach = std::clamp(powerSpan.hi - powerSpan.lo, 0.0f, 1.0f);

	if (powerRingShown()) {
		// NOT ARMED: a small ring at the right end says "this switch is modulated", and its centre fills
		// while modulation has it ON — so a gate on the power is seen opening and closing.
		const float d = std::max(h - 10.0f, 5.0f);
		const float x = w - d - 4.0f;
		const float y = (h - d) * 0.5f;
		canvas.setColor(withAlpha(kModulation, 0.9f));
		canvas.ring(x, y, d, 1.0f);
		if (on) {
			canvas.circle(x + d * 0.25f, y + d * 0.25f, d * 0.5f);
		}
	}

	// THE POWER SYMBOL, ONLY IN MOD-ASSIGN MODE AND ONLY WHERE THE POWER IS A DESTINATION.
	if (powerGlyphShown()) {
		const device::ModDisplay p = powerMod;
		const visage::Bounds g = powerGlyph();
		const float r = g.width() * 0.5f;
		const float cx = g.x() + r;
		const float cy = g.y() + r;
		canvas.setColor(withAlpha(kSurface, 0.95f));
		canvas.circle(g.x(), g.y(), g.width());
		// ITS FILL IS THE ARMED SOURCE'S REACH on the switch — deeper the further a full-strength route
		// carries it — so the symbol shows the modulated amount as the face's bar does for the amount.
		if (powerReach > 0.0f) {
			canvas.setColor(withAlpha(kModulation, 0.2f + 0.5f * powerReach));
			canvas.circle(g.x(), g.y(), g.width());
		}
		canvas.setColor(p.routable ? kModulation : kMuted);
		canvas.ring(g.x() + 2.0f, g.y() + 2.0f, g.width() - 4.0f, 1.2f);
		canvas.setColor(withAlpha(kSurface, 0.95f));
		canvas.rectangle(cx - 1.5f, g.y() + 1.0f, 3.0f, r * 0.6f); // the gap in the ring at the top
		canvas.setColor(p.routable ? kModulation : kMuted);
		canvas.segment(cx, g.y() + 2.5f, cx, cy, 1.2f, true); // the stroke through it
	}
}

} // namespace feathers::ui

namespace feathers::ui {

ControlFace::ModLook FaderPill::modLookFor(const device::Modulatable& c) const {
	if (&c == &amount_) {
		const device::ModDisplay mod = amount_.display();
		if (!mod.isDestination) {
			return {};
		}
		const device::Modulatable::Span span = amount_.amountSpan();
		// THE BORDER (`draw`'s `lit`) and, not armed, the reach bar or the routed-elsewhere dot.
		return { true, mod.isDestination && mod.armed, !mod.armed && (span.hi > span.lo || mod.amount != 0.0f || mod.routedElsewhere) };
	}
	if (power_ != nullptr && &c == power_) {
		if (!power_->display().isDestination) {
			return {};
		}
		return { true, powerGlyphShown(), powerRingShown() };
	}
	return {};
}

} // namespace feathers::ui
