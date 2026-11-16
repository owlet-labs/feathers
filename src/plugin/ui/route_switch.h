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

#pragma once

#include "plugin/ui/tooltip.h"

#include <visage/ui.h>

#include <string>

// A ROUTE SWITCH — how two things are chained, as two pictures of the two ways.
//
//     ┌────┐
//     │ ■  │   SERIES: the first feeds the second, one after the other on one line
//     │ │  │
//     │ ■  │
//     ├────┤
//     │ ┬  │   PARALLEL: both hear the input, side by side, and their outputs are summed
//     │■ ■ │
//     │ ┴  │
//     └────┘
//
// **BOTH CHOICES ARE ALWAYS DRAWN, the active one lit** (CLAUDE.md: a control shows its choices before one is applied),
// so it is not one glyph that flips. A press on a glyph selects its choice; the other press does nothing new.
// **BOUND TO A TWO-CHOICE CONTROL**: the top glyph is the control's minimum, the bottom its maximum. It is the narrow
// middle of a `PairCell` between two slots, so it is about a knob column tall and 22 px wide.
//
// A PRESS IS ONE GESTURE (`beginGesture`, `setValue`, `endGesture`) as `SegmentedControl::select` makes it, and the
// notifier is told what was chosen; a press on the lit glyph, or outside both, writes nothing and says nothing.

namespace feathers::ui {

class Notifier;

class RouteSwitch final : public visage::Frame, public TooltipSource {
public:
	/// The control's declared tooltip, anywhere on the switch — see `ui/tooltip.h`.
	const char* tooltipAt(visage::Point) const override { return tooltipOf(&control_); }
	void offeredControls(std::vector<Offered>& out) const override { out.push_back({ &control_, Road::Face }); }

	/// @param control   a two-choice control: its minimum is the top glyph's choice and its maximum the bottom's
	/// @param notifier  told "<name> Series" / "<name> Parallel" on a change; may be null
	explicit RouteSwitch(device::Modulatable& control, Notifier* notifier = nullptr);

	/// THE NATURAL SIZE: the width a middle gap is, and the height of a knob column of `faceDiameter`.
	static constexpr float kWidth = 22.0f;
	static constexpr float kGlyphGap = 3.0f;
	[[nodiscard]] static float heightFor(float faceDiameter);

	enum Choice { Series = 0, Parallel = 1 };

	/// **WHICH GLYPH IS LIT** — the control's value read at its midpoint, as `SwitchChip::on` reads one.
	[[nodiscard]] int litChoice() const;
	/// A glyph's hit rectangle, in this frame's own coordinates; the plates are what is drawn and what is hit.
	[[nodiscard]] visage::Bounds glyphBounds(int choice) const;
	/// **WHERE A GLYPH'S INK LIES**, in this frame's coordinates: the picture's own extent, inside its plate with `kInkMargin`
	/// to spare on every side. `draw` builds the picture from the same numbers.
	[[nodiscard]] visage::Bounds inkBounds(int choice) const;
	static constexpr float kInkMargin = 2.0f;
	/// Which glyph a point falls in, or -1 between and outside them.
	[[nodiscard]] int choiceAt(visage::Point position) const;
	/// Choose as a press would. A choice already lit, one out of range, or an unavailable control does nothing.
	void select(int choice);

	/// GREYED (the honesty rule): draws unlit and takes no press. On by default.
	void setEnabled(bool enabled);
	[[nodiscard]] bool enabled() const noexcept { return enabled_; }
	[[nodiscard]] bool usable() const { return enabled_ && control_.unavailable() == nullptr; }
	[[nodiscard]] device::Modulatable& control() const noexcept { return control_; }

	void draw(visage::Canvas& canvas) override;

private:
	void drawGlyph(visage::Canvas& canvas, const visage::Bounds& plate, int choice, bool lit) const;

	device::Modulatable& control_;
	Notifier* notifier_ = nullptr;
	bool enabled_ = true;
};

} // namespace feathers::ui
