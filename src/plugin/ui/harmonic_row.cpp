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

#include "plugin/ui/harmonic_row.h"
#include "plugin/ui/menu_anchor.h"

#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace feathers::ui {

namespace {

namespace hrm = feathers::fx::harm;

/// HOW GREY THE ROW GOES WHILE THE DEVICE IS DOING NOTHING — the card's number, deliberately the
/// same one, because "how grey is off" should have one answer across the lane.
constexpr float kOffAlpha = 0.35f;

/// The ratio as a player reads it: "3:2", not 1.5. A decimal is the number the DSP multiplies by; a
/// ratio is the interval, and the interval is what the harmonic table was ported for.
std::string ratioLabelOf(size_t index) {
	const size_t at = std::min(index, hrm::kHarmonicCount - 1);
	return std::to_string(static_cast<int>(hrm::kHarmonics[at].num)) + ":"
	       + std::to_string(static_cast<int>(hrm::kHarmonics[at].den));
}

} // namespace

HarmonicRow::HarmonicRow(device::Modulatable& control) : control_(control) {
	onMouseDown() = [this](const visage::MouseEvent& e) {
		switch (partAt(e.position)) {
			case Part::Prev:
				step(-1);
				break;
			case Part::Next:
				step(1);
				break;
			case Part::Menu:
				showMenu();
				break;
			case Part::None:
				break;
		}
	};
	// LAST, after every `=` above — see `installTooltipHover`.
	installTooltipHover(*this, *this);
}

void HarmonicRow::setActive(bool active) {
	if (active_ == active) {
		return;
	}
	active_ = active;
	redraw();
}

size_t HarmonicRow::index() const {
	const auto at = static_cast<long>(std::lround(control_.value()));
	return static_cast<size_t>(
	    std::clamp<long>(at, 0, static_cast<long>(hrm::kHarmonicCount) - 1));
}

std::string HarmonicRow::label() const {
	return hrm::harmonicLabel(index());
}

std::string HarmonicRow::ratioText() const {
	return ratioLabelOf(index());
}

bool HarmonicRow::canStep(int direction) const {
	const size_t at = index();
	return direction < 0 ? at > 0 : at < hrm::kHarmonicCount - 1;
}

visage::Bounds HarmonicRow::boxBounds() const {
	const float left = kArrowWidth;
	const float right = std::max(left, width() - kArrowWidth);
	return { left, 0.0f, right - left, height() };
}

visage::Bounds HarmonicRow::labelBounds() const {
	const visage::Bounds box = boxBounds();
	return { box.x(), box.y(), std::max(0.0f, box.width() - kChevronStrip), box.height() };
}

visage::Bounds HarmonicRow::prevArrowBounds() const {
	return { 0.0f, 0.0f, kArrowWidth, height() };
}

visage::Bounds HarmonicRow::nextArrowBounds() const {
	return { std::max(0.0f, width() - kArrowWidth), 0.0f, kArrowWidth, height() };
}

HarmonicRow::Part HarmonicRow::partAt(visage::Point position) const {
	// THE ARROWS BEFORE THE BOX. They do not overlap it, but the order is stated rather than relied
	// on: this file's own card records what happens when a smaller target is tested after the larger
	// rectangle it sits in.
	if (prevArrowBounds().contains(position)) {
		return Part::Prev;
	}
	if (nextArrowBounds().contains(position)) {
		return Part::Next;
	}
	if (boxBounds().contains(position)) {
		return Part::Menu;
	}
	return Part::None;
}

void HarmonicRow::notify() {
	redraw();
	if (onChanged) {
		onChanged();
	}
}

void HarmonicRow::step(int direction) {
	const auto at = static_cast<long>(std::lround(control_.value()));
	const long next = std::clamp<long>(at + direction, 0, static_cast<long>(hrm::kHarmonicCount) - 1);
	if (next == at) {
		// AT THE END OF THE TABLE. The arrow is drawn dim and this does nothing.
		//
		// AND WHAT THIS GUARD PROTECTS IS NOT THE VALUE, which is worth stating because it looks like
		// it is. `FxControl::setValue` goes through the bridge, which COERCES against the control's
		// own declaration — so an unclamped step would store a perfectly legal index anyway, and a
		// known-bad that removed the clamp came back GREEN against every value assertion including
		// the stored one.
		//
		// WHAT IT PROTECTS IS `onChanged`. Without it, a press on a dead arrow fires the host's
		// callback saying something moved when nothing did — which redraws the card, and on the panel
		// redraws the scope, once per press against a stop. That is the assertion the case makes now,
		// because it is the only one that can see this line.
		return;
	}
	control_.beginGesture();
	control_.setValue(static_cast<double>(next));
	control_.endGesture();
	notify();
}

void HarmonicRow::showMenu() {
	// A LIST AS WELL AS THE ARROWS — the ratio's arrows alone, without a dropdown, were not enough. The
	// arrows step and the list jumps; a hundred-and-two entries is more than a hand wants to walk.
	// MARKED AND OPENED ON THE CURRENT RATIO (in Harm and other places with long lists of options, the menu
	// reopens centred on the current option, with a mark beside the current choice) — a hundred-and-two rows opened at the top hid the one you are on.
	visage::PopupMenu menu;
	const auto current = static_cast<long>(std::lround(control_.value()));
	for (size_t i = 0; i < hrm::kHarmonicCount; ++i) {
		addChoiceRow(menu, static_cast<int>(i), hrm::harmonicLabel(i), static_cast<long>(i) == current);
	}
	menu.onSelection() = [this](int id) {
		if (id < 0 || static_cast<size_t>(id) >= hrm::kHarmonicCount) {
			return;
		}
		control_.beginGesture();
		control_.setValue(static_cast<double>(id));
		control_.endGesture();
		notify();
	};
	const visage::Bounds box = boxBounds();
	showMenuAt(std::move(menu), *this, box);
}

void HarmonicRow::draw(visage::Canvas& canvas) {
	const auto stated = [this](uint32_t colour) {
		if (active_) {
			return colour;
		}
		const float alpha = static_cast<float>((colour >> 24) & 0xFFu) / 255.0f;
		return withAlpha(colour, alpha * kOffAlpha);
	};

	// THE RATIO THE OSCILLATOR SITS AT, and therefore where the notch sits — one number for both,
	// which is the coupling that makes HARM one device rather than two.
	const visage::Bounds box = boxBounds();
	canvas.setColor(stated(withAlpha(kLine, 0.9f)));
	canvas.roundedRectangleBorder(box.x(), box.y(), box.width(), box.height(), 3.0f, 1.0f);

	// THE RATIO AND WHAT IT IS CALLED, TOGETHER — helpful colour text in the dropdown (e.g. 1 oct,
	// perfect 5th). A ratio is exact and a name is legible, and a synthesist
	// wants both. Where a ratio names no interval honestly — 11:8 is 51 cents from a fourth — the
	// ratio stands alone; see harmonicLabel.
	//
	// CENTRED IN THE BOX LESS THE CHEVRON STRIP, which is why that reservation is a named width and
	// not a nudge: text centred across the FULL box runs under the chevron at exactly the longest
	// labels. That is the two-things-in-one-rectangle shape this control has already paid for twice.
	canvas.setColor(stated(kInk));
	const visage::Bounds text = labelBounds();
	canvas.text(label(), font(kControlSize), visage::Font::kCenter, text.x(), text.y(),
	            text.width(), text.height());

	// THE CHEVRON THAT SAYS THIS BOX IS A MENU.
	//
	// THIS DROPDOWN WAS REPORTED MISSING TWICE AND IT WAS NEVER MISSING: the menu has worked all
	// along and the box has always been its hit target. What was missing was any mark saying so. The
	// box drew a border and centred text — pixel-for-pixel what the filter's type box draws — and THAT one
	// was found, because "lowpass" reads as a choice. "3:2 · perfect 5th" between two steppers
	// reads as THE READOUT THE STEPPERS ARE CHANGING, so here the affordance was not merely absent,
	// it was CONTRADICTED BY ITS OWN NEIGHBOURS. A hidden hit area survives only while something
	// visible names it.
	//
	// GEOMETRY RATHER THAN A GLYPH — this font has no guaranteed chevron, and a missing glyph is an
	// invisible control, which is precisely the bug being fixed rather than a hazard in the abstract.
	canvas.setColor(stated(kMuted));
	const float chevronCx = box.right() - kChevronStrip * 0.5f;
	const float chevronCy = box.y() + box.height() * 0.5f;
	const float chevronReach = 3.0f;
	canvas.segment(chevronCx - chevronReach, chevronCy - chevronReach * 0.5f, chevronCx,
	               chevronCy + chevronReach * 0.5f, 1.6f, true);
	canvas.segment(chevronCx + chevronReach, chevronCy - chevronReach * 0.5f, chevronCx,
	               chevronCy + chevronReach * 0.5f, 1.6f, true);

	for (const int direction : { -1, 1 }) {
		const visage::Bounds arrow = direction < 0 ? prevArrowBounds() : nextArrowBounds();
		// GEOMETRY RATHER THAN A GLYPH, and DIMMED AT THE ENDS OF THE TABLE — the honesty rule: an
		// arrow that cannot move should not look like one that can.
		canvas.setColor(stated(canStep(direction) ? kMuted : kDisabled));
		const float cx = arrow.x() + arrow.width() * 0.5f;
		const float cy = arrow.y() + arrow.height() * 0.5f;
		const float reach = 3.5f;
		const float tip = cx + reach * static_cast<float>(direction);
		const float back = cx - reach * static_cast<float>(direction);
		canvas.segment(tip, cy, back, cy - reach, 1.6f, true);
		canvas.segment(tip, cy, back, cy + reach, 1.6f, true);
	}
}

} // namespace feathers::ui
