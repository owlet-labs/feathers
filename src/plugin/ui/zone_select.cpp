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

#include "plugin/ui/zone_select.h"
#include "plugin/ui/menu_anchor.h"
#include "plugin/ui/mod_route_menu.h"
#include "plugin/ui/mod_state.h"

#include "fx/automod/automod_types.h"
#include "plugin/ui/rail.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>

namespace feathers::ui {

// **ONE QUANTUM, NOT TWO SPELLINGS OF IT.** The carry has always moved a phase by one per lap; this
// widget navigates the same line in the same unit. If somebody retunes that, both must move together
// or the selector and the carry would disagree about where a zone boundary is.
// **THE DEVICE'S OWN NUMBER, which is where a modulation reach is derived from.** `kOneZone` is
// `kLapCoordinate / kZones`, so if the widget and the device ever disagreed about a lap, a route's
// depth and the dropdown's step would be describing different distances on the same line.
//
// This used to be pinned against `ZoneCoordBox::kPhasePerLap` as well. That widget is gone (BJ piece
// 4) and the device is the better anchor anyway: a lap is a fact about the quantity, not about a
// widget that happened to carry it.
static_assert(fx::automod::kLapCoordinate == ZoneSelect::kCoordinatePerLap,
              "the selector's lap and automod's must be the same distance");

ZoneSelect::ZoneSelect(device::Modulatable& coordinate, std::vector<ZoneName> names, double perLap)
    : control_(coordinate), names_(std::move(names)), perLap_(perLap) {
	onMouseDown() = [this](const visage::MouseEvent& e) {
		// ARMED AND ROUTABLE, THE PRESS IS THE ROUTE'S (`ValueBox`'s road for a dropdown): up sets how far the armed source
		// reaches the choice, a double-click unroutes. Alt picks, whatever is armed.
		if (mappable(e.isAltDown())) {
			if (e.repeatClickCount() == 2) {
				if (control_.dropRoute()) {
					redraw();
				}
				return;
			}
			control_.beginAmountGesture();
			const float before = control_.display().amount;
			anchorAmount_ = before != 0.0f ? before : kRouteSeedAmount;
			control_.setAmount(anchorAmount_);
			routeStartY_ = e.position.y;
			routing_ = true;
			redraw();
			return;
		}
		pressAt(e.position);
	};
	onMouseDrag() = [this](const visage::MouseEvent& e) {
		if (routing_) {
			const float travel = routeStartY_ - e.position.y;
			control_.setAmount(std::clamp(anchorAmount_ + travel / kRouteDragRange, -device::kMaxAmount, device::kMaxAmount));
			redraw();
		}
	};
	onMouseUp() = [this](const visage::MouseEvent&) {
		if (routing_) {
			routing_ = false;
			control_.endAmountGesture();
			redraw();
		}
	};
	// LAST, after every `=` above — see `installTooltipHover`.
	installTooltipHover(*this, *this);
}

double ZoneSelect::perZone() const noexcept {
	return names_.empty() ? perLap_ : perLap_ / static_cast<double>(names_.size());
}

double ZoneSelect::zonesFromOrigin() const {
	const double per = perZone();
	if (per == 0.0) {
		return 0.0;
	}
	// **FLOOR, NOT TRUNCATION**, and the difference is the whole of the negative half of the line.
	// Truncation rounds toward zero, so -0.1 zones would land in zone 0 exactly as +0.1 does and the
	// region just below the origin would be a double-width one. `ZoneKnob::lapsIn` and
	// `ValueBox::dragTo`'s carry both carry this same correction, for this same reason.
	//
	// **AND THE TOP OF A BOUNDED RANGE IS THE LAST ZONE'S, NOT THE NEXT LAP'S FIRST (backlog IG).** An
	// unbounded coordinate has no top, so this never fires for one. A BOUNDED zoned control — SKLITTER's
	// Groove, 0..1 over seven feels — reaches its declared max as the last feel at full, which is what the
	// device decodes; the floor alone would name the first feel there, a name the sound is not.
	const device::ParamDesc& d = control_.desc();
	const double v = control_.value();
	// **A STEPPED CONTROL IS ON ITS NEAREST STEP** — every reader of one rounds (`device::coerceControl`
	// stores it unrounded), and its last step is a zone of its own rather than the end of the one before
	// (`ModeValueCell`'s selector over a Choice).
	if (d.stepped) {
		return std::floor(std::round(std::clamp(v, d.min, d.max)) / per);
	}
	if (!d.offsetsMayLeaveRange && v >= d.max) {
		return std::ceil(d.max / per) - 1.0;
	}
	return std::floor(v / per);
}

size_t ZoneSelect::zoneIndex() const {
	if (names_.empty()) {
		return 0;
	}
	const auto count = static_cast<long>(names_.size());
	const auto zones = static_cast<long>(zonesFromOrigin());
	// C++'s % keeps the sign of the dividend, so the +count before the second % is what makes zone -1
	// the LAST name rather than a negative index into the vector.
	return static_cast<size_t>(((zones % count) + count) % count);
}

long ZoneSelect::lap() const {
	if (names_.empty()) {
		return 0;
	}
	const auto count = static_cast<double>(names_.size());
	return static_cast<long>(std::floor(zonesFromOrigin() / count));
}

double ZoneSelect::positionInZone() const {
	const double per = perZone();
	if (per == 0.0) {
		return 0.0;
	}
	return (control_.value() - zonesFromOrigin() * per) / per;
}

std::string ZoneSelect::nameOf(size_t index) const {
	return index < names_.size() ? names_[index].name : std::string {};
}

std::string ZoneSelect::zoneName() const {
	return names_.empty() ? std::string {} : nameOf(zoneIndex());
}

std::string ZoneSelect::captionOf(size_t index) const {
	if (index >= names_.size()) {
		return {};
	}
	const std::string& name = names_[index].name;
	// THE NUMBER WHERE THERE IS NO NAME — `ZoneCoordBox::regionText`'s rule, and OWLTT is the device
	// that needs it: its character zones 1 and 2 are blank on purpose.
	return name.empty() ? "zone " + std::to_string(index) : name;
}

std::string ZoneSelect::captionText() const {
	if (names_.empty()) {
		return {};
	}
	return captionOf(zoneIndex());
}

float ZoneSelect::minimumWidth() const {
	// EVERY NAME, NOT THE ONE SHOWING. The set is small and fixed, so there is nothing to sample and
	// no endpoint argument to make: the widest string this widget can ever print is simply the widest
	// of the strings it has.
	float widest = 0.0f;
	for (size_t i = 0; i < names_.size(); ++i) {
		const std::string text = captionOf(i);
		widest = std::max(widest,
		                  font(kKnobReadoutSize).stringWidth(std::u32string(text.begin(), text.end())));
	}
	// The two arrow cells keep their full width whenever the row is wide enough to want them —
	// `prevBounds`/`nextBounds` cap at `kArrowWidth` and shrink only below 4·kArrowWidth, which is
	// narrower than any answer this returns.
	return 2.0f * kArrowWidth + widest;
}

void ZoneSelect::commit(double coordinate) {
	control_.beginGesture();
	control_.setValue(coordinate);
	control_.endGesture();
	redraw();
}

void ZoneSelect::stepZone(int delta) {
	if (delta == 0 || names_.empty()) {
		return;
	}
	// **A STEPPED CONTROL'S ARROWS STOP AT ITS ENDS** — a Choice has no laps to roll into, and an arrow that
	// wrapped Free 16 round to Straight would be the rolling knob the split removed. A press at an end
	// commits nothing, so it opens no gesture either.
	const device::ParamDesc& d = control_.desc();
	if (d.stepped) {
		const double at = std::round(std::clamp(control_.value(), d.min, d.max));
		const double to = std::clamp(at + static_cast<double>(delta) * perZone(), d.min, d.max);
		if (to != at) {
			commit(to);
		}
		return;
	}
	commit(control_.value() + static_cast<double>(delta) * perZone());
}

void ZoneSelect::selectZone(size_t index) {
	if (index >= names_.size()) {
		return;
	}
	// THE MOVE IS A WHOLE NUMBER OF ZONES FROM WHERE WE STAND, which is what preserves the position
	// within the zone without having to reconstruct it — see the header on why this stays in the
	// current lap.
	const auto delta = static_cast<long>(index) - static_cast<long>(zoneIndex());
	if (delta == 0) {
		return;
	}
	// A STEPPED CONTROL TAKES THE STEP ITSELF, not the stored value moved by the difference — a fraction
	// left on the stored value would otherwise ride along.
	if (control_.desc().stepped) {
		commit(control_.desc().min + static_cast<double>(index) * perZone());
		return;
	}
	commit(control_.value() + static_cast<double>(delta) * perZone());
}

//-- geometry ---------------------------------------------------------------------------------//

visage::Bounds ZoneSelect::prevBounds() const {
	const float w = std::min(kArrowWidth, static_cast<float>(width()) * 0.25f);
	return { 0.0f, 0.0f, w, static_cast<float>(height()) };
}

visage::Bounds ZoneSelect::nextBounds() const {
	const float w = std::min(kArrowWidth, static_cast<float>(width()) * 0.25f);
	return { static_cast<float>(width()) - w, 0.0f, w, static_cast<float>(height()) };
}

visage::Bounds ZoneSelect::captionBounds() const {
	const visage::Bounds prev = prevBounds();
	const visage::Bounds next = nextBounds();
	return { prev.right(), 0.0f, std::max(0.0f, next.x() - prev.right()),
		     static_cast<float>(height()) };
}

ZoneSelect::Verb ZoneSelect::verbAt(visage::Point position) const {
	// **A DISABLED CARD ANSWERS NOTHING**, rather than answering and then declining in `pressAt`.
	// The honesty rule is about what a control DOES, and a widget that reports a verb it will not
	// perform is a widget whose hit test and whose behaviour disagree.
	if (!enabled_ || names_.empty()) {
		return Verb::None;
	}
	// **ARROWS BEFORE THE CAPTION, AND THE ORDER IS LOAD-BEARING ONLY BECAUSE IT IS ALSO EXCLUSIVE.**
	// `captionBounds` is defined as the gap BETWEEN them, so no pixel is in two of these; the case
	// that sweeps every x is what keeps that true as the geometry moves.
	if (prevBounds().contains(position)) {
		return Verb::Prev;
	}
	if (nextBounds().contains(position)) {
		return Verb::Next;
	}
	if (captionBounds().contains(position)) {
		return Verb::Menu;
	}
	return Verb::None;
}

bool ZoneSelect::pressAt(visage::Point position) {
	switch (verbAt(position)) {
	case Verb::Prev:
		stepZone(-1);
		return true;
	case Verb::Next:
		stepZone(1);
		return true;
	case Verb::Menu: {
		// **THE SHELL, AND EVERYTHING IT DOES IS `selectZone`.** A popup needs a window and clicking
		// one is not synthesisable here, so the callback stays one line: which row was picked. The
		// behaviour a case asserts is the verb, not this.
		visage::PopupMenu menu;
		for (size_t i = 0; i < names_.size(); ++i) {
			menu.addOption(static_cast<int>(i), captionOf(i));
		}
		menu.onSelection() = [this](int id) {
			if (id >= 0) {
				selectZone(static_cast<size_t>(id));
			}
		};
		showMenuAt(std::move(menu), *this, { 0.0f, 0.0f, width(), height() });
		return true;
	}
	case Verb::None:
		break;
	}
	return false;
}

void ZoneSelect::setEnabled(bool on) {
	enabled_ = on;
	redraw();
}

bool ZoneSelect::mappable(bool altDown) const {
	const device::ModDisplay d = control_.display();
	return !altDown && enabled_ && d.isDestination && d.routable;
}

ControlFace::ModLook ZoneSelect::modLookFor(const device::Modulatable& c) const {
	const device::ModDisplay d = control_.display();
	if (&c != &control_ || !d.isDestination) {
		return {};
	}
	const device::Modulatable::Span span = control_.amountSpan();
	return { true, mappable(false), !d.armed && (span.hi > span.lo || d.amount != 0.0f || d.routedElsewhere) };
}

void ZoneSelect::refreshWatch() {
	ModState* state = routingStateOf(control_);
	if (state == watched_) {
		return;
	}
	watched_ = state;
	watch_ = state != nullptr ? state->watch(this) : Watch {};
}

//-- drawing ----------------------------------------------------------------------------------//

void ZoneSelect::draw(visage::Canvas& canvas) {
	refreshWatch();
	const uint32_t ink = enabled_ ? kInk : kDisabled;
	const uint32_t chrome = enabled_ ? kMuted : kDisabled;

	// THE GROUND, the same recess a compact `ValueBox` sits in — this widget is that box's partner
	// on the row and reading as a different KIND of surface would split one control in two.
	canvas.setColor(kSurface);
	canvas.roundedRectangle(0.0f, 0.0f, width(), height(), kCornerRadius);

	if (names_.empty()) {
		// A LINE WITH NO NAMED REGIONS SAYS NOTHING rather than "zone 0" — Explore's gamma is such a
		// line, and inventing a vocabulary for it would claim a structure the quantity does not have.
		return;
	}

	canvas.setColor(chrome);
	// **THE GLYPH IS GEOMETRY, NOT A CHARACTER** — `rail.cpp`'s rule, and the reason for it is the
	// atlas: `‹` is one missing glyph away from being an empty box in the control you need to get
	// back. A SINGLE chevron each way, deliberately: the rail spends a DOUBLE one on "move this panel
	// aside" and this editor already spends filled triangles on the modulator arrow and on solo, so a
	// third filled triangle would have made three different verbs look like one family.
	for (int side = 0; side < 2; ++side) {
		const visage::Bounds cell = side == 0 ? prevBounds() : nextBounds();
		const float mid = cell.y() + cell.height() * 0.5f;
		const float tipX = side == 0 ? cell.x() + cell.width() * 0.38f
		                             : cell.right() - cell.width() * 0.38f;
		const float tailX = side == 0 ? tipX + kChevronHalf : tipX - kChevronHalf;
		canvas.segment(tipX, mid, tailX, mid - kChevronHalf, Rail::kChevronWeight, true);
		canvas.segment(tipX, mid, tailX, mid + kChevronHalf, Rail::kChevronWeight, true);
	}

	// THE NAME, WHICH IS THE WHOLE POINT OF THE WIDGET. No number beside it: the `ValueBox` on this
	// row carries the coordinate, and printing one here would be the coarse:fine pair again.
	const visage::Bounds caption = captionBounds();
	canvas.setColor(ink);
	canvas.text(captionText(), font(kKnobReadoutSize), visage::Font::kCenter, caption.x(), caption.y(),
	            caption.width(), caption.height());

	// **MODULATION, AS A DROPDOWN SHOWS IT** (`modLookFor`): the border in the modulation colour while an armed source may
	// route here; routed and not armed, a small ring at the right end.
	const ModLook look = modLookFor(control_);
	if (look.armedGlow) {
		drawBoxOutline(canvas, 0.0f, 0.0f, width(), height(), kCornerRadius, boxOutlineOf(false, true, true));
	}
	else if (look.routedMark) {
		const float d = std::max(height() - 10.0f, 4.0f);
		canvas.setColor(withAlpha(kModulation, 0.9f));
		canvas.ring(width() - kArrowWidth - d - 1.0f, (height() - d) * 0.5f, d, 1.0f);
	}
}

} // namespace feathers::ui
