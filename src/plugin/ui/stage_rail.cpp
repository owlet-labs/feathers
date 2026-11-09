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

#include "plugin/ui/stage_rail.h"

#include "plugin/ui/theme.h"

namespace feathers::ui {

StageRail::StageRail() : Rail(kExploreLabel, Rail::Side::Left) {
	// THE TOGGLE AND NOTHING ELSE, in both states — the fx lane's own rule (`FxLane::partAt`): a press on
	// the closed strip away from the toggle does nothing, so the strip is not a second, larger button.
	onMouseDown() = [this](const visage::MouseEvent& e) {
		if (toggleHit(e.position)) {
			toggleCollapsed();
		}
	};
}

void StageRail::draw(visage::Canvas& canvas) {
	// CLOSED IS A WHOLE DRAWING, the strip the left rails draw: their ground and their hairline, on the
	// inboard edge, which here is the one facing the fx lane.
	if (collapsed()) {
		canvas.setColor(kSurface);
		canvas.fill(0.0f, 0.0f, width(), height());
		canvas.setColor(kLine);
		canvas.fill(width() - 1.0f, 0.0f, 1.0f, height());
	}
	// OPEN, ONLY THE CHEVRONS, on the stage's own ground — the frame is the toggle's corner and paints
	// nothing a hand did not come for.
	drawRailChrome(canvas);
}

} // namespace feathers::ui
