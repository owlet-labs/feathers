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

// **THE EDGE PAN — ONE LAW FOR EVERY TERRAIN A PUCK IS DRAGGED ON** (Explore's gesture, backlog IJ; the ExplorePad's,
// which the Shaper's and the Phaser's pads run; and the Pattern's map, which lacked one). While a DRAGGING hand is within `kEdgeBandPixels` of an edge, or past it, the view scrolls on TIME, ramping to
// `kPanViewsPerSecond` `kRampPixels` deeper — the hit box is the edge itself and everything beyond it (which is why it
// always worked in Explore: its hit box extends to the edges). Moved here from `ExplorePad` unchanged, so the two surfaces
// cannot drift apart.

#include <algorithm>
#include <utility>

namespace feathers::ui::edge_pan {

/// Where the scroll starts, in pixels inside the surface's real edge.
inline constexpr double kEdgeBandPixels = 24.0;
/// How much deeper than the band's start the scroll reaches full speed.
inline constexpr double kRampPixels = 48.0;
/// Full speed, in views per second — Explore's `kViewsPerSecond`.
inline constexpr double kPanViewsPerSecond = 1.2;
/// How far inside the surface the puck is held while the hand is beyond it: the puck stays whole.
inline constexpr double kStarInsetPixels = 8.0;

/// The scroll, in views per second along x and y (UP IS MORE), for a pointer at (x, y) on a w × h surface.
[[nodiscard]] inline std::pair<double, double> velocity(double x, double y, double w, double h) noexcept {
	const auto rate = [](double depth) { return depth <= 0.0 ? 0.0 : kPanViewsPerSecond * std::min(1.0, depth / kRampPixels); };
	const double vx = rate(x - (w - kEdgeBandPixels)) - rate(kEdgeBandPixels - x);
	const double vy = rate(kEdgeBandPixels - y) - rate(y - (h - kEdgeBandPixels));
	return { vx, vy };
}

} // namespace feathers::ui::edge_pan
