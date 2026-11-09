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

#include "plugin/ui/fx_card.h"

#include <functional>
#include <visage/ui.h>

// THE `(+)` AT THE END OF THE RACK'S CHAIN — an empty card-sized slot that says "a card goes here".
//
// IT IS A BUTTON AND NOTHING ELSE (backlog KP). It used to open a list of its own, inline in the lane,
// and the editor's full-window takeover browser stood in front of that; both are gone. A press now
// opens Feathers' add menu (`RackEditor::showAddMenu`), the same popup the fx lane opens.

namespace feathers::rack {

class AddDeviceRow : public visage::Frame {
public:
	AddDeviceRow();

	/// THE SLOT WAS PRESSED — the owner opens its add menu against this frame.
	std::function<void()> onPress;

	/// MY EXTENT CHANGED (the lane's axis flipped) — the owner must place me again. A callback rather than
	/// a `parent()->resized()`, because placing things is the owner's job.
	std::function<void()> onHeightChanged;

	void draw(visage::Canvas& canvas) override;

	/// HOW TALL THIS IS IN A VERTICAL STACK.
	float laneHeight() const { return kBarHeight; }

	static constexpr float kBarHeight = 26.0f;
	/// THE FLOW EXTENT IN A HORIZONTAL LANE — a narrow spine.
	static constexpr float kShutWidth = 44.0f;

	/// WHICH WAY THE LANE THIS SITS IN RUNS. Reuses `FxCard`'s enum: this is not an `FxCard`, but it is in
	/// the same lane, and a lane with two ideas of what "horizontal" means is a lane with a bug waiting.
	void setLaneAxis(ui::FxCard::LaneAxis axis);

	/// HOW MUCH FLOW THIS NEEDS, given its cross allotment — the question `FxCard::laneExtent` answers.
	float laneExtent(float cross) const;

private:
	ui::FxCard::LaneAxis axis_ = ui::FxCard::LaneAxis::Vertical;
};

} // namespace feathers::rack
