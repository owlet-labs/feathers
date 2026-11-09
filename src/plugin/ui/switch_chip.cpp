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

#include "plugin/ui/switch_chip.h"

#include "device/device.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/mod_route_menu.h"
#include "plugin/ui/mod_state.h"

#include <algorithm>
#include <string_view>

namespace feathers::ui {

SwitchChip::SwitchChip(device::Modulatable& control, std::string label)
    : control_(control), label_(std::move(label)) {
	onMouseDown() = [this](const visage::MouseEvent& e) {
		// **THE RIGHT BUTTON NEVER PRESSES THE SWITCH.** It used to: this handler never asked which
		// button, so a right-click toggled. That is a latent misfire the day the right button means
		// something — see `setPopup`.
		// GREYED (setEnabled): it changes nothing now, so it takes no press — EXCEPT the route its dimmer glow offers (QK, by
		// design: greyed controls glow). A control that changes nothing WHERE IT STANDS (`unavailable`) offers none.
		const bool greyedRoute = showsModulation_ && !e.isAltDown() && !e.shouldTriggerPopup()
		                         && control_.unavailable() == nullptr && control_.display().routable;
		if (!usable() && !greyedRoute) {
			return;
		}
		if (e.shouldTriggerPopup()) {
			if (popup_) {
				popup_();
			}
			return;
		}
		// ARMED AND ROUTABLE, THE PRESS IS THE ROUTE'S (`setShowsModulation`) — `FaderPill`'s power rule: a double-click
		// unroutes, a press on an unrouted switch seeds an amount, and a drag up or right sets it. Alt presses the switch.
		if (showsModulation_ && !e.isAltDown() && control_.display().routable) {
			if (e.repeatClickCount() >= 2) {
				control_.setAmount(0.0f);
				redraw();
				return;
			}
			routing_ = true;
			routeStartX_ = e.position.x;
			routeStartY_ = e.position.y;
			control_.beginAmountGesture();
			const float before = control_.display().amount;
			anchorAmount_ = before != 0.0f ? before : kRouteSeedAmount;
			control_.setAmount(anchorAmount_);
			redraw();
			return;
		}
		if (pressOverride_ && pressOverride_(e)) {
			redraw();
			return; // THE OWNER TOOK THE PRESS (`setPressOverride`)
		}
		if (behaviour_ == Behaviour::Momentary) {
			press();
		}
		else {
			toggle();
		}
	};
	// **THE RELEASE IS THE ONLY THING THAT LETS A MOMENTARY GO** — no exit or drag handler, on purpose.
	// visage hands the release to the frame that took the press wherever the pointer is by then, so a
	// hand that slides off the chip still ends the gesture here. **ANY BUTTON'S RELEASE LETS GO**, not
	// only the left's: a momentary held with the left while the right is clicked and released ends on
	// that release. Deliberate — a release that could fail to end a DAW gesture is the worse bug, and a
	// release with nothing held (a right-click on its own) writes nothing.
	onMouseDrag() = [this](const visage::MouseEvent& e) {
		if (!routing_) {
			return; // a switch's own press is one instant; only a route's amount is dragged
		}
		const float travel = (e.position.x - routeStartX_) + (routeStartY_ - e.position.y);
		control_.setAmount(std::clamp(anchorAmount_ + travel / kRouteDragRange, -device::kMaxAmount, device::kMaxAmount));
		redraw();
	};
	onMouseUp() = [this](const visage::MouseEvent&) {
		if (routing_) {
			routing_ = false;
			control_.endAmountGesture();
			redraw();
			return;
		}
		release();
	};
	// LAST, after every `=` above — see `installTooltipHover`.
	installTooltipHover(*this, *this);
	// WATCHED FROM THE START, since the chip shows modulation by default: the glow repaints when the arm moves.
	refreshWatch();
	// **A HOLD WHOSE RELEASE NEVER ARRIVED** (Pattern's Go stuck on): the host or the window manager can
	// take the pointer between a press and its release, and visage then never delivers the mouse-up — the chip would hold
	// max for ever. A plain move with no button down is proof the hand let go, so it ends the hold. A drag (a button still
	// down) does not: that is the hand holding, on or off the chip.
	onMouseMove() += [this](const visage::MouseEvent& e) {
		if (held_ && !e.isLeftButtonCurrentlyDown() && !e.isRightButtonCurrentlyDown() && !e.isMiddleButtonCurrentlyDown()) {
			release();
		}
	};
}

SwitchChip::~SwitchChip() {
	// **BEFORE `~Frame`, which cannot do it for us.** visage's destructor (and `removeChild`) runs
	// `notifyRemoveFromHierarchy`, which gives up the pointer by clearing `mouse_down_frame_` WITHOUT
	// sending a mouse-up — so a chip destroyed while held would never see its release, and the host
	// would be left with an open gesture and the control stuck at max.
	cancelHold();
	if (routing_) {
		routing_ = false;
		control_.endAmountGesture(); // a route drag in flight closes too
	}
}

void SwitchChip::setShowsModulation(bool shows) {
	if (showsModulation_ != shows) {
		showsModulation_ = shows;
		refreshWatch();
		redraw();
	}
}

void SwitchChip::refreshWatch() {
	const ModState* state = showsModulation_ ? routingStateOf(control_) : nullptr;
	if (state == watched_) {
		return;
	}
	watched_ = state;
	watch_ = state != nullptr ? const_cast<ModState*>(state)->watch(this) : Watch {};
}

SwitchChip::ModMarks SwitchChip::modMarks() const {
	ModMarks out;
	if (!showsModulation_) {
		return out;
	}
	const device::ModDisplay d = control_.display();
	if (!d.isDestination) {
		return out;
	}
	if (d.armed) {
		out.glow = d.routable && control_.unavailable() == nullptr; // greyed or not (QK); `draw` dims a greyed one
		const device::Modulatable::Span span = control_.amountSpan();
		out.reach = std::clamp(span.hi - span.lo, 0.0f, 1.0f);
		return out;
	}
	const device::Modulatable::Span span = control_.amountSpan();
	// A ROUTE, NOT A REACH: an ON switch routed upward reaches nowhere further (the span clamps to nothing), and the route
	// is still live (2026-11-07, the glow-honesty sweep).
	out.ring = span.hi > span.lo || d.amount != 0.0f || d.routedElsewhere;
	out.ringFilled = out.ring && control_.normalizedModulated() >= 0.5;
	return out;
}

void SwitchChip::visibilityChanged() {
	// HIDING ALONE WOULD BE SAFE for the gesture — visage delivers the release to the frame that took
	// the press whether or not it is visible — but a macro held at max behind a chip nobody can see is
	// wrong in itself, and hiding is what a slot does when the macro's type changes under the hand.
	if (!isVisible()) {
		cancelHold();
	}
}

void SwitchChip::setPresentation(Presentation presentation) {
	if (presentation_ != presentation) {
		presentation_ = presentation;
		redraw();
	}
}

void SwitchChip::setFaceDiameter(float diameter) {
	if (diameter > 0.0f && diameter != faceDiameter_) {
		faceDiameter_ = diameter;
		redraw();
	}
}

visage::Bounds SwitchChip::padBounds() const {
	if (presentation_ != Presentation::Pad) {
		return {};
	}
	const float side = std::min(faceDiameter_, static_cast<float>(width()));
	return { (static_cast<float>(width()) - side) * 0.5f, (faceDiameter_ - side) * 0.5f, side, side };
}

std::string SwitchChip::drawnCaption() const {
	if (captionIsName_) {
		return Knob::columnName(label_.c_str(), static_cast<float>(width())).text;
	}
	return Knob::columnCaption(label_.c_str(), static_cast<float>(width()));
}

std::optional<LabelFace> SwitchChip::labelFaceAt(visage::Point) const {
	if (label_.empty()) {
		return std::nullopt;
	}
	const std::string full = !fullName_.empty() ? fullName_
	                         : control_.desc().name != nullptr ? std::string(control_.desc().name)
	                                                          : label_;
	if (presentation_ == Presentation::Pad) {
		const std::string whole = captionIsName_ ? label_ : upper(label_.c_str());
		return LabelFace { .label = label_, .full = full, .cut = drawnCaption() != whole };
	}
	// A CHIP PRINTS ITS LABEL ONLY AS AN OFF/ON SWITCH (`caption`), centred and clipped at its own width.
	if (!device::isOffOnChoice(control_.desc())) {
		return std::nullopt;
	}
	const bool cut = textWidth(label_, font(kKnobLabelSize)) > static_cast<float>(width());
	return LabelFace { .label = label_, .full = full, .cut = cut };
}

float SwitchChip::drawnCaptionSize() const {
	return captionIsName_ ? Knob::columnName(label_.c_str(), static_cast<float>(width())).size : kKnobLabelSize;
}

void SwitchChip::setCaptionIsName(bool isName) {
	if (captionIsName_ != isName) {
		captionIsName_ = isName;
		redraw();
	}
}

const std::string& SwitchChip::readout() const {
	static const std::string kOn = "ON";
	static const std::string kOff = "OFF";
	return on() ? kOn : kOff;
}

void SwitchChip::setLabel(std::string label) {
	if (label == label_) {
		return;
	}
	label_ = std::move(label);
	redraw();
}

void SwitchChip::cancelHold() {
	if (!held_) {
		return;
	}
	held_ = false;
	// THE RELEASE ROAD (`Modulatable::releaseValue`): a press quicker than a block is still read by one.
	control_.releaseValue(control_.desc().min);
	control_.endGesture();
	// NO `onEdited`: this is the chip letting go on the way out, not an edit the hand made — and from
	// the destructor the card that registered the callback may already be half torn down.
}

void SwitchChip::press() {
	if (held_) {
		return; // a second press while held (another button) opens no second gesture
	}
	held_ = true;
	heldFrom_ = control_.value();
	control_.beginGesture();
	control_.setValue(control_.desc().max);
	redraw();
}

void SwitchChip::release() {
	if (!held_) {
		return; // no press behind it — a latch release, or the tail of a right-click
	}
	held_ = false;
	// THE RELEASE ROAD (`Modulatable::releaseValue`): a press quicker than a block is still read by one.
	control_.releaseValue(control_.desc().min);
	control_.endGesture();
	// ONE GESTURE, ONE EDIT, and only if it left the value somewhere new — a press-and-release from
	// rest is a performance, not a change to the patch.
	if (edited_ && control_.value() != heldFrom_) {
		edited_(heldFrom_, control_.value());
	}
	redraw();
}

bool SwitchChip::on() const {
	// **THE MIDPOINT OF THE DECLARED RANGE, NOT A COMPARISON WITH 1.** `sklitter_types.cpp`'s `switchOf`
	// makes the same call for the same reason: a host automating a stepped control with a RAMP passes
	// through every value between the two, and a switch that only reads `== max` is off for all of them.
	const device::ParamDesc& d = control_.desc();
	return control_.value() >= (d.min + d.max) * 0.5;
}

void SwitchChip::toggle() {
	const double before = control_.value();
	const device::ParamDesc& d = control_.desc();
	const double after = on() ? d.min : d.max;
	if (after == before) {
		return;
	}
	// **BRACKETED AS A GESTURE, because a press IS one.** A host that records automation needs the two
	// edges; `ValueBox` brackets its drag for the same reason, and a switch whose whole interaction is
	// one instant is exactly the case where forgetting them looks harmless and writes nothing.
	control_.beginGesture();
	control_.setValue(after);
	control_.endGesture();
	if (control_.value() == before) {
		return; // the control refused it — say nothing rather than report an edit that did not happen
	}
	if (edited_) {
		edited_(before, control_.value());
	}
	redraw();
}

const std::string& SwitchChip::caption() const {
	// **A SWITCH PRINTS WHAT IT IS; A TWO-WAY PICKER PRINTS WHAT IT IS SET TO. Which one this is comes
	// from the CHOICE TABLE rather than from the caller.**
	//
	// The header above says a switch needs no readout because "the lit state carries the value", and
	// that is exactly true of `Off`/`On`: lit MEANS on, so a chip reading `Ping-Pong` and glowing has
	// told you everything. **It is false the moment the two states are NAMED THINGS.** The delay's
	// `Mode` is `Clean`/`Granular`, and a lit chip reading `Delay Mode` never says the word
	// `Granular` anywhere on the card — the player is left to infer which of two words a glow means.
	// In use, it was not obvious at all that Delay Mode enables grain.
	//
	// **IT IS THE FIRST CONTROL IN THE REGISTRY TO HIT THIS, WHICH IS WHY IT SURVIVED.** Every other
	// two-value choice anywhere in `src/fx` and `src/mod` is `Off`/`On` — measured, not assumed — so
	// the lit rule has been carrying the value correctly for every chip that has ever existed, and
	// the first counterexample arrived with a device built five hours ago.
	//
	// **THE TEST IS THE TABLE, NOT A FLAG ON THE CALLER.** A `bool showsValue` argument would be a
	// claim ABOUT the names that could disagree with them — the same objection `Traits::enableControl`
	// records against a `hasEnable` bool. Asking the names cannot drift from the names.
	return device::isOffOnChoice(control_.desc()) ? label_ : valueName();
}

const std::string& SwitchChip::valueName() const {
	const device::ParamDesc& d = control_.desc();
	const size_t index = on() ? 1u : 0u;
	if (index < d.names.size() && d.names[index] != nullptr) {
		// CACHED so `caption()` can return a reference and the draw path allocates nothing.
		valueCache_ = d.names[index];
	}
	else {
		valueCache_ = label_;
	}
	return valueCache_;
}

void SwitchChip::setEnabled(bool enabled) {
	if (enabled_ != enabled) {
		enabled_ = enabled;
		redraw();
	}
}

void SwitchChip::draw(visage::Canvas& canvas) {
	// **`env_card.cpp`'s CHIP, MOVED RATHER THAN REDRAWN.** Same fill, same border, same radius, same
	// lit rule — so a switch on a generic card and a switch on the DAHDSR's are one object, which is
	// the whole reason this became a widget instead of a fourth copy of the arithmetic.
	const bool lit = on() && usable(); // GREYED: never lit, its caption muted
	const float w = static_cast<float>(width());
	const float h = static_cast<float>(height());
	if (presentation_ == Presentation::Pad) {
		// THE CHIP'S FILL, BORDER AND LIT RULE, at the face's size and with a knob's two lines under it —
		// `Knob::draw`'s caption — THROUGH `Knob`'s own drawing, so the two cannot cut a long name at
		// different letters — and its reading, at the same offsets, so a pad beside a knob lines up line
		// for line.
		const visage::Bounds pad = padBounds();
		canvas.setColor(lit ? withAlpha(kAccent, 0.22f) : kVoid);
		canvas.roundedRectangle(pad.x(), pad.y(), pad.width(), pad.height(), 4.0f);
		canvas.setColor(lit ? kAccent : withAlpha(kLine, 0.8f));
		canvas.roundedRectangleBorder(pad.x(), pad.y(), pad.width(), pad.height(), 4.0f, 1.0f);

		canvas.setColor(kMuted);
		Knob::drawColumnCaption(canvas, drawnCaption(), w, faceDiameter_ + Knob::kGap, drawnCaptionSize());
		canvas.setColor(kMuted);
		canvas.text(readout(), font(kKnobReadoutSize), visage::Font::kCenter, 0.0f,
		            faceDiameter_ + Knob::kGap + Knob::kLabelHeight + Knob::kLineGap, w, Knob::kReadoutHeight);
		return;
	}
	// DORMANT (`setDormant`): lit at half strength — set, but asleep.
	const bool asleep = lit && dormant_;
	canvas.setColor(lit ? withAlpha(kAccent, asleep ? 0.08f : 0.22f) : kVoid);
	canvas.roundedRectangle(0.0f, 0.0f, w, h, 3.0f);
	canvas.setColor(lit ? withAlpha(kAccent, asleep ? 0.45f : 1.0f) : withAlpha(kLine, 0.8f));
	canvas.roundedRectangleBorder(0.0f, 0.0f, w, h, 3.0f, 1.0f);
	if (pending_ && !lit) {
		// PENDING: armed, waiting for its boundary — two accent borders and an accent caption, no motion.
		canvas.setColor(withAlpha(kAccent, 0.12f));
		canvas.roundedRectangle(0.0f, 0.0f, w, h, 3.0f);
		canvas.setColor(kAccent);
		canvas.roundedRectangleBorder(0.0f, 0.0f, w, h, 3.0f, 1.0f);
		canvas.roundedRectangleBorder(2.0f, 2.0f, w - 4.0f, h - 4.0f, 2.0f, 1.0f);
		canvas.setColor(kAccent);
		canvas.text(caption(), font(kKnobLabelSize), visage::Font::kCenter, 0.0f, 0.0f, w, h);
	}
	else {
		canvas.setColor(lit && !asleep ? kInk : kMuted);
		canvas.text(caption(), font(kKnobLabelSize), visage::Font::kCenter, 0.0f, 0.0f, w, h);
	}

	// MODULATION (`setShowsModulation`), the pill's power marks: armed, a glow as deep as the source's reach and a border
	// in the modulation colour; routed and not armed, the ring at the right end, filled while modulation has it ON.
	const ModMarks marks = modMarks();
	if (showsModulation_) {
		refreshWatch();
	}
	if (marks.glow) {
		// A GREYED CHIP'S GLOW IS DIMMER (QK): plainly lit, plainly not the live chip beside it.
		const float dim = enabled_ ? 1.0f : 0.5f;
		canvas.setColor(withAlpha(kModulation, (0.15f + 0.45f * marks.reach) * dim));
		canvas.roundedRectangle(0.0f, 0.0f, w, h, 3.0f);
		canvas.setColor(withAlpha(kModulation, dim));
		canvas.roundedRectangleBorder(0.0f, 0.0f, w, h, 3.0f, 1.0f);
	}
	if (marks.ring) {
		const float d = std::max(h - 10.0f, 4.0f);
		const float x = w - d - 2.0f;
		const float y = (h - d) * 0.5f;
		canvas.setColor(withAlpha(kModulation, 0.9f));
		canvas.ring(x, y, d, 1.0f);
		if (marks.ringFilled) {
			canvas.circle(x + d * 0.25f, y + d * 0.25f, d * 0.5f);
		}
	}
}

} // namespace feathers::ui
