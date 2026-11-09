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

// THE LIKES' GLYPH, shared by every surface that marks a like on a plane (Explore's chart, the Pattern map), so a like
// looks the same wherever it is drawn.

#include <visage/graphics.h>

#include <cstdint>

namespace feathers::ui {

/// THE LIKES' COLOUR — a warm pink, one hue for every like whichever sound made it: a like is the patch's, not a sound's.
inline constexpr uint32_t kLikeHue = 0xffE8708E;
/// How wide a like's mark is on the chart.
inline constexpr float kLikeMarkSize = 9.0f;

/// A HEART, `size` wide, centred on (cx, cy): two lobes and the point between them.
inline void drawHeart(visage::Canvas& canvas, float cx, float cy, float size) {
	const float lobe = size * 0.56f;
	const float top = cy - size * 0.36f;
	canvas.circle(cx - size * 0.5f, top, lobe);
	canvas.circle(cx + size * 0.5f - lobe, top, lobe);
	canvas.triangle(cx - size * 0.49f, cy - size * 0.05f, cx + size * 0.49f, cy - size * 0.05f, cx, cy + size * 0.46f);
}

} // namespace feathers::ui
