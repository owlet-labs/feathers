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

#include "plugin/ui/pill_stack.h"

#include "plugin/ui/theme.h"

#include <algorithm>

namespace feathers::ui {

PillStack::PillStack(std::array<Spec, kCount> specs, std::string title) : title_(std::move(title)) {
	for (size_t i = 0; i < kCount; ++i) {
		add(i, std::move(specs[i]));
	}
}

PillStack::PillStack(std::array<Spec, 2> specs, std::string title) : count_(2), title_(std::move(title)) {
	for (size_t i = 0; i < 2; ++i) {
		add(i, std::move(specs[i]));
	}
}

void PillStack::add(size_t i, Spec spec) {
	pills_[i] = spec.power != nullptr ? std::make_unique<FaderPill>(*spec.amount, *spec.power, std::move(spec.caption))
	                                   : std::make_unique<FaderPill>(*spec.amount, std::move(spec.caption));
	amounts_[i] = spec.amount;
	addChild(pills_[i].get());
	watchPill(i);
}

void PillStack::setActive(bool active) {
	for (size_t i = 0; i < count_; ++i) {
		pills_[i]->setActive(active);
	}
}

void PillStack::watchPill(size_t i) {
	// **`+=`, BESIDE THE PILL'S OWN HANDLERS** — they were installed with `=` in its constructor, so these run
	// after them: by the time the readout is asked for its text, the drag has already moved the value.
	FaderPill& p = *pills_[i];
	const int index = static_cast<int>(i);
	const auto hover = [this, index](const visage::MouseEvent&) {
		if (hovered_ != index) {
			hovered_ = index;
			(void)refresh();
		}
	};
	p.onMouseEnter() += hover;
	p.onMouseMove() += hover;
	p.onMouseExit() += [this, index](const visage::MouseEvent&) {
		if (hovered_ == index) {
			hovered_ = -1;
			(void)refresh();
		}
	};
	const auto touch = [this, index](const visage::MouseEvent&) {
		touched_ = index;
		(void)refresh();
	};
	p.onMouseDown() += touch;
	p.onMouseDrag() += touch;
	p.onMouseUp() += touch;
}

visage::Bounds PillStack::pillBounds(size_t i) const {
	// THE PILLS SHARE WHAT THE READOUT LEAVES — 16 px each in a `kHeight` cell.
	const float pills = std::max(0.0f, height() - kReadoutHeight - kReadoutGap);
	const float each = std::max(0.0f, (pills - static_cast<float>(kCount - 1) * kPillGap) / static_cast<float>(kCount));
	return { 0.0f, static_cast<float>(i) * (each + kPillGap), width(), each };
}

visage::Bounds PillStack::readoutBounds() const {
	return { 0.0f, height() - kReadoutHeight, width(), kReadoutHeight };
}

std::string PillStack::readoutText() const {
	const int i = shown();
	if (i < 0) {
		return title_;
	}
	const auto k = static_cast<size_t>(i);
	return pills_[k]->caption() + " " + amounts_[k]->text();
}

bool PillStack::refresh() {
	// **THE TEXT LAST ASKED FOR, NOT THE TEXT LAST DRAWN** — a stack that is not being drawn (folded, offscreen)
	// never runs `draw`, and a comparison against the drawn text would ask for a repaint on every tick for ever.
	std::string now = readoutText();
	if (now == asked_) {
		return false;
	}
	asked_ = std::move(now);
	redraw();
	return true;
}

void PillStack::resized() {
	// UNCONDITIONALLY — CLAUDE.md's scale rule: a scale change moves nothing logical.
	for (size_t i = 0; i < count_; ++i) {
		pills_[i]->setBounds(pillBounds(i));
	}
}

void PillStack::draw(visage::Canvas& canvas) {
	const visage::Bounds r = readoutBounds();
	// THE TITLE IS DIM, a reading is ink — so the line says whether it is naming the group or reading a pill.
	canvas.setColor(shown() < 0 ? kMuted : kInk);
	canvas.text(readoutText(), font(kKnobReadoutSize), visage::Font::kCenter, r.x(), r.y(), r.width(), r.height());
}

} // namespace feathers::ui
