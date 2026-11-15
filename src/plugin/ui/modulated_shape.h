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

#include "plugin/ui/theme.h"

#include <visage/ui.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace feathers::ui {

/// **THE AS-MODULATED SHAPE, DRAWN OVER THE AT-REST ONE** — a knob's value arc and its modulated dot, on a plot. A plot
/// draws its rest shape as it always has, and while `drawsModulatedShape` says modulation moved it, calls this to stroke
/// the modulated one in the modulation colour on top. `pointAt(i)` gives point `i` of `count` as a `visage::Point`;
/// non-finite points are skipped (the renderer must never see one).
template <class PointAt>
void strokeModulatedShape(visage::Canvas& canvas, size_t count, PointAt&& pointAt, float width = 1.75f) {
	if (count < 2) {
		return;
	}
	// **A CORE IN THE DOT'S COLOUR, BECAUSE THE ACCENT AND THE MODULATION COLOUR ARE NEIGHBOURS** (their OKLab distance is
	// about 0.03): over a plot whose rest shape is drawn in the accent, a line in `kModulation` alone cannot be told from
	// it. A modulated value's mark is `kModulatedDot`, which differs from both in lightness; a stereo dot wears it as its
	// core, and so does this line.
	for (int pass = 0; pass < 2; ++pass) {
		canvas.setColor(pass == 0 ? withAlpha(kModulation, 0.95f) : withAlpha(kModulatedDot, 0.9f));
		const float thickness = pass == 0 ? width : std::max(0.8f, width * 0.45f);
		for (size_t i = 1; i < count; ++i) {
			const visage::Point from = pointAt(i - 1);
			const visage::Point to = pointAt(i);
			if (!std::isfinite(from.x) || !std::isfinite(from.y) || !std::isfinite(to.x) || !std::isfinite(to.y)) {
				continue;
			}
			canvas.segment(from.x, from.y, to.x, to.y, thickness, true);
		}
	}
}

} // namespace feathers::ui
