/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 *
 * --- Additional terms under GNU GPL version 3 section 7 ---
 * This file requires preservation of the above copyright notice and author attribution
 * in all copies or substantial portions of this file.
 */

#pragma once

#include "plugin/gui_bridge.h"
#include "plugin/stack_index.h"
#include "plugin/ui/tooltip.h"
#include "plugin/ui/widget_registry.h"

#include <vector>
#include <visage/ui.h>

// **THE STACK INDEX, DRAWN** (voice stacking step 5): one dot per stacked oscillator, lowest part on the left, at its
// index from −1 (bottom) to +1 (top), beside the Shape it comes from — so a hand turning Shape, Mapping or Curve sees
// which part goes where BEFORE routing it anywhere (the "no knob-list cards" rule: the controls sit on the picture).
//
// A PICTURE OF THE PATCH, NOT OF A NOTE: it reads the parameters' base values and calls the engine's own
// `stackIndexSpread`, so it shows what every note starts from. What moves per note is not drawn — a per-voice source
// into an amount, Drift's walk (the base is drawn) and Random Fresh's draw (the fixed draw stands in for it; voice-
// stacking doc §7, assumption 33). Nothing here repaints at modulation rate: the parameters' watches redraw it.

namespace feathers::ui {

class StackIndexPlot final : public visage::Frame, public TooltipSource {
public:
	StackIndexPlot(ParamBridge& bridge, WidgetRegistry& registry);

	void draw(visage::Canvas& canvas) override;
	const char* tooltipAt(visage::Point) const override;
	/// A picture of core state; it offers no device control.
	void offeredControls(std::vector<Offered>&) const override {}

	/// Greyed with the rest of the stack page (the honesty rule: until a route carries the index, it reaches nothing).
	void setEnabled(bool on);
	[[nodiscard]] bool enabled() const noexcept { return enabled_; }

	/// The indices drawn, one per part — what `draw` places, asked rather than read off pixels.
	[[nodiscard]] std::vector<float> indicesForTest() const;
	/// Where part `part`'s dot is drawn, in this frame's coordinates.
	[[nodiscard]] visage::Point dotForTest(int part) const;

private:
	[[nodiscard]] StackIndexSettings settings() const;
	[[nodiscard]] visage::Point dotAt(int part, int count, float index) const;

	ParamBridge& bridge_;
	std::vector<Watch> watches_;
	bool enabled_ = true;
};

} // namespace feathers::ui
