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

#include "rack/add_device.h"

#include "plugin/ui/theme.h"

namespace feathers::rack {

AddDeviceRow::AddDeviceRow() {
	onMouseDown() = [this](const visage::MouseEvent&) {
		if (onPress) {
			onPress();
		}
	};
}

void AddDeviceRow::setLaneAxis(ui::FxCard::LaneAxis axis) {
	if (axis_ == axis) {
		return;
	}
	axis_ = axis;
	if (onHeightChanged) {
		onHeightChanged();
	}
	redraw();
}

float AddDeviceRow::laneExtent(float cross) const {
	// THE CROSS IS NOT USED: this row has no contents to pack, so neither its height nor its width is
	// derived from the other.
	(void)cross;
	return axis_ == ui::FxCard::LaneAxis::Horizontal ? kShutWidth : laneHeight();
}

void AddDeviceRow::draw(visage::Canvas& canvas) {
	// **AN EMPTY SLOT, DRAWN AS ONE.** A muted outline the
	// size of the whole unit, with the verb centred in it, says "a card goes here" in the same vocabulary
	// the cards beside it use. The label shrinks to "+" on a narrow spine rather than being clipped.
	const float w = static_cast<float>(width());
	const float h = static_cast<float>(height());
	canvas.setColor(ui::withAlpha(ui::kLine, 0.55f));
	canvas.roundedRectangleBorder(0.0f, 0.0f, w, h, ui::kCardCorner, 1.0f);
	canvas.setColor(ui::kMuted);
	canvas.text(w >= 96.0f ? "+ ADD DEVICE" : "+", ui::font(11.0f), visage::Font::kCenter, 0, 0,
	            static_cast<int>(w), static_cast<int>(h));
}

} // namespace feathers::rack
