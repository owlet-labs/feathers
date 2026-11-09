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

#include "visage_graphics/canvas.h"

#include <array>
#include <utility>
#include <cstddef>

namespace feathers::ui {

/// **A CACHED FIELD-ACTIVITY MAP AND ITS DRAW, FOR ANY PAD THAT WANTS TERRAIN UNDER ITS PUCK.**
///
/// DECIDED, on Automod's pad: not a new coordinate circle but a repeat of the shaper-style square with
/// terrain — the terrain need not be render previews; it can be decoded from the field — and REUSING the
/// shaper's construct rather than re-deriving it.
///
/// **WHAT IS SHARED IS SMALLER THAN A PAD AND BIGGER THAN A LOOP.** The two pads differ in what their
/// axes mean, how they write their controls, what bands they draw and where the puck sits; none of
/// that belongs here. What they have in common is a coarse brightness map of a field that is
/// expensive to evaluate and constant for the life of the widget — plus two details that are easy to
/// get subtly wrong and were got right once:
///
///   - **SAMPLED AT CELL CENTRES**, so a cell is a fair average of what it covers rather than of its
///     top-left corner — for a field this fast that is a visible difference, and the shaper's pad
///     says so where it first got it right.
///   - **ROWS RUN BOTTOM-UP**, matching an axis whose zero is at the bottom, which is where every
///     vertical drag in this editor puts its minimum.
///
/// **NAMED `FieldActivityMap` AND NOT `TerrainMap`, WHICH WAS THE FIRST NAME AND IS TAKEN.**
/// `terrain.h:555` already has a `TerrainMap` — Explore's, an entirely different thing about the
/// constellation plane. The compiler caught the collision immediately; had this been a new name in a
/// new namespace it would have compiled and left two unrelated "terrain maps" for the next reader to
/// disentangle. This one is about a device field's ACTIVITY, and the name now says so.
///
/// So this is not a pad base class. Extracting one would have meant inventing an axis abstraction
/// that neither device asked for, to share code that is four lines in each of them.
template <size_t Wide, size_t High>
class FieldActivityMap {
public:
	static constexpr size_t kWide = Wide;
	static constexpr size_t kHigh = High;

	/// @param activityAt called with unit coordinates in [0,1] — x rightward, y UPWARD — returning
	///        0 (nothing happening here) to 1 (everything happening here). Called `Wide * High` times,
	///        once, at construction of the owning widget.
	template <typename ActivityFn>
	void build(ActivityFn&& activityAt) {
		for (size_t y = 0; y < High; ++y) {
			for (size_t x = 0; x < Wide; ++x) {
				const float uy = (static_cast<float>(y) + 0.5f) / static_cast<float>(High);
				const float ux = (static_cast<float>(x) + 0.5f) / static_cast<float>(Wide);
				cells_[y * Wide + x] = activityAt(ux, uy);
			}
		}
	}

	/// THE CELLS, at whatever size the frame is now. `colourFor` turns an activity into a colour, so
	/// the caller keeps its own palette and its own floor — the shaper wants a dim floor everywhere so
	/// the grid reads as a surface rather than as scattered marks, and another pad may not.
	template <typename ColourFn>
	void draw(visage::Canvas& canvas, float w, float h, ColourFn&& colourFor) const {
		drawAt(canvas, 0.0f, 0.0f, w, h, std::forward<ColourFn>(colourFor));
	}

	/// THE CELLS AT AN OFFSET AND A SIZE OF THEIR OWN — a map built WIDER than the view it sits in, and
	/// panned under it (the shaper pad, backlog IJ: a map built with a margin so a pan reveals ground
	/// already drawn, Explore's own arrangement). The frame clips what falls outside it.
	template <typename ColourFn>
	void drawAt(visage::Canvas& canvas, float x0, float y0, float w, float h, ColourFn&& colourFor) const {
		const float cellW = w / static_cast<float>(Wide);
		const float cellH = h / static_cast<float>(High);
		for (size_t y = 0; y < High; ++y) {
			for (size_t x = 0; x < Wide; ++x) {
				canvas.setColor(colourFor(cells_[y * Wide + x]));
				// THE HALF-PIXEL OVERLAP IS DELIBERATE: without it the cells leave hairlines between
				// them at fractional sizes, which reads as a grid drawn on the field rather than as
				// the field itself.
				canvas.rectangle(x0 + static_cast<float>(x) * cellW, y0 + h - static_cast<float>(y + 1) * cellH,
				                 cellW + 0.5f, cellH + 0.5f);
			}
		}
	}

	[[nodiscard]] const std::array<float, Wide * High>& cells() const noexcept { return cells_; }

private:
	std::array<float, Wide * High> cells_ {};
};

} // namespace feathers::ui
