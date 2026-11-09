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

#include "plugin/ui/rail.h"

// THE STAGE'S COLLAPSE — backlog NF: the Explore pane can be compressed, just like the fx lane, and
// collapsing it fully is fine.
//
// SO IT IS A `Rail`, and nothing about the collapse is written twice: the toggle's rectangle, the double
// chevron, the 34px strip, the upright label at reading size and the announce-don't-resize contract are all
// the base's, exactly as the fx lane has them. The one difference is what the rail HOLDS. The other rails
// parent their contents; the stage's contents are the Explore and Perform surfaces and the terrain ground
// under them, which are the editor's own children in an order that matters (the ground must be an EARLIER
// sibling than the stage it sits under — see `TerrainGround`). Re-parenting them under a rail would put the
// ground on top. So this frame holds nothing: the editor hides the surfaces while it is closed and lays this
// frame over the stage's corner while it is open.
//
// OPEN, IT IS ONLY THE TOGGLE'S CORNER — `kOpenWidth` x `kOpenHeight` at the stage's top-right, which is where
// a LEFT rail keeps its toggle (inboard, the end the panel closes away from). Everything else in that corner
// is the stage's; the caption keeps clear of it (`ExploreStage::setCaptionReserve`).
//
// CLOSED, IT IS THE STRIP, and the strip's word follows the header's switch — EXPLORE or PERFORM — because
// the stage is both and a strip that said one while holding the other would be a label that lies.
//
// EDITOR-LOCAL, like every rail: a desk preference (`rail.explore.collapsed`), never the patch.

namespace feathers::ui {

class StageRail final : public Rail {
public:
	StageRail();

	void draw(visage::Canvas& canvas) override;

	/// THE FRAME'S WIDTH WHILE OPEN — the toggle's corner and nothing else. A constant, as the base demands.
	float expandedWidth() const override { return kOpenWidth; }

	/// The toggle and its inset on both sides, the base's numbers: `toggleBounds()` puts an open left
	/// rail's toggle `2·kCollapsedPadX` in from its right edge.
	static constexpr float kOpenWidth = kToggleSize + 4.0f * kCollapsedPadX;
	/// The toggle at `kPadTop/2` and the same again under it.
	static constexpr float kOpenHeight = kToggleSize + kPadTop;

	/// **WHERE A STAGE'S CAPTION STARTS SO IT CLEARS THE TOGGLE** — the fx lane's "P⟩ST-FX" lesson
	/// (`Rail::railHeadingLeft`) on the stage, whose caption is centred and shares the toggle's row. Centred
	/// while that clears the corner, slid left of it when it does not; below `kCaptionMinLeft` the caption
	/// does not fit beside the toggle at all and the stage writes its short word instead.
	static float captionLeft(float stageWidth, float textWidth) {
		const float centred = (stageWidth - textWidth) * 0.5f;
		const float clear = stageWidth - kOpenWidth - textWidth;
		return centred < clear ? centred : clear;
	}
	static constexpr float kCaptionMinLeft = 2.0f * kCollapsedPadX;

	/// The words the strip says, one per surface.
	static constexpr const char* kExploreLabel = "EXPLORE";
	static constexpr const char* kPerformLabel = "PERFORM";
};

} // namespace feathers::ui
