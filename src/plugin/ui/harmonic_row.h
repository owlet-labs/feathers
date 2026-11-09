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

#pragma once

#include "fx/harm/harm_types.h"
#include "plugin/ui/fx_control.h"
#include "plugin/ui/tooltip.h"

#include <visage/ui.h>

#include <functional>
#include <string>

// THE HARMONIC CONTROL: two steppers, a ratio box that is also a menu, and the chevron that says so.
//
// EXTRACTED SO BOTH SURFACES CAN BUILD IT. It was `HarmCard`'s — four rectangles, a 102-entry popup,
// a stepper pair and a chevron, all reaching a control by index — and `HarmExpanded` could not have
// it without a second copy. **That gap was real and was written down rather than hidden**: the
// expanded view PLOTS the harmonic on its scope and a hand could not move it from there.
//
// A SECOND COPY WOULD HAVE BEEN THE WRONG FIX, and not for tidiness. This row has three separate
// pieces of hard-won behaviour on it — the chevron that went missing twice, the arrows dimming
// at the ends of the table, and the text centred in the box LESS the chevron strip so a long label
// does not run under it. Two copies is two places for any of those to stop being true, and the one
// that stopped would be the one nobody was looking at.
//
// IT OWNS NO STATE. The value lives in the device's row, reached through an `FxControl` the host
// supplies — so the card and the panel are two views of one number by construction rather than by
// being kept in step.

namespace feathers::ui {

class HarmonicRow : public visage::Frame, public TooltipSource {
public:
	/// The control's declared tooltip, anywhere on the row — see `ui/tooltip.h`.
	const char* tooltipAt(visage::Point) const override { return tooltipOf(&control_); }
	void offeredControls(std::vector<Offered>& out) const override { out.push_back({ &control_, Road::Face }); }

	/// `control` must outlive this row. Both hosts hold their `FxControl`s for the life of the
	/// surface, which is what makes a reference right here rather than a copy.
	/// IT TAKES THE COMMON BASE, NOT EITHER ROAD.
	///
	/// This row only ever calls `value`, `setValue`, `beginGesture` and `endGesture` — all four are
	/// `device::Modulatable`, which `FxControl` and `DeviceControl` both are. Binding it to one of them
	/// made it usable by exactly one host for no reason it could name.
	explicit HarmonicRow(device::Modulatable& control);

	void draw(visage::Canvas& canvas) override;

	static constexpr float kHeight = 24.0f;
	static constexpr float kArrowWidth = 20.0f;
	/// Reserved to the RIGHT of the label before it is centred, not after. "1 oct + perfect 5th down"
	/// is long, and text centred across the full box runs under the chevron at exactly the labels
	/// that most need reading — the two-things-in-one-rectangle shape this card has paid for twice.
	static constexpr float kChevronStrip = 14.0f;

	//----------//
	// Geometry //
	//----------//
	//
	// IN THIS FRAME'S OWN COORDINATES. The row used to compute its rectangles from the CARD's height
	// and padding; as a frame it computes them from itself, which is what lets a panel place it
	// somewhere the card would never have put it.

	visage::Bounds boxBounds() const;
	visage::Bounds prevArrowBounds() const;
	visage::Bounds nextArrowBounds() const;

	/// WHERE THE LABEL IS ACTUALLY DRAWN — the box less the chevron strip, and `draw` uses THIS
	/// rather than recomputing it.
	///
	/// IT IS AN ACCESSOR BECAUSE A CASE COULD NOT SEE THE SUBTRACTION OTHERWISE. A known-bad that
	/// deleted the strip from the draw call came back GREEN: the case was asserting a relationship
	/// between two CONSTANTS — the longest label and `boxBounds().width() - kChevronStrip` — and the
	/// mutation was in a line no assertion could reach. A geometry the drawing reads is a geometry a
	/// case can check.
	visage::Bounds labelBounds() const;

	enum class Part { None, Prev, Next, Menu };
	Part partAt(visage::Point position) const;

	//---------//
	// Reading //
	//---------//

	/// The whole label: the ratio, and the interval beside it where the ratio names one honestly.
	std::string label() const;
	/// The bare "3:2" — what a FOLDED head has room for, which is why it is separate.
	std::string ratioText() const;
	/// Which entry of the harmonic table is selected.
	size_t index() const;

	/// Whether a stepper would move. Drawn dimmed when it would not, which is the honesty rule: an
	/// arrow that cannot move should not look like one that can.
	bool canStep(int direction) const;

	//---------//
	// Writing //
	//---------//

	void step(int direction);
	void showMenu();

	/// SOMETHING CHANGED — installed by the host, because only the host knows what else has to
	/// repaint. The card's own head shows the ratio while folded, and the panel's scope moves the
	/// notch; neither is this row's business.
	std::function<void()> onChanged;

	/// Whether the device is switched into the signal, so the row can recede with the rest of a
	/// switched-out card. TOLD rather than asked: a row has no way to reach its device's enable, and
	/// a panel does not dim at all.
	void setActive(bool active);
	bool active() const noexcept { return active_; }

private:
	void notify();

	device::Modulatable& control_;
	bool active_ = true;
};

} // namespace feathers::ui
