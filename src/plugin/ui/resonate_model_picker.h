/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "device/modulatable.h"
#include "plugin/ui/tooltip.h"

#include <visage/ui.h>

#include <cstddef>

// RESONATE'S MODEL AS FIVE PICTOGRAMS — a loop, a free bar, a pinned tube, a grid, a spring (Task 8's
// layout rule). EVERY OPTION IS ON THE FACE, so the multivalue-toggle rule is met by visibility rather
// than by a menu, and each cell is a small picture of what its physics does. A press selects the cell
// under it; the selected cell is drawn raised with the accent.

namespace feathers::ui {

class ResonateModelPicker final : public visage::Frame, public TooltipSource {
public:
	explicit ResonateModelPicker(device::Modulatable& model);

	void draw(visage::Canvas& canvas) override;
	[[nodiscard]] const char* tooltipAt(visage::Point local) const override;
	/// Every model on the face, so it is Model's face.
	void offeredControls(std::vector<Offered>& out) const override { out.push_back({ &model_, Road::Face }); }

	static constexpr size_t kCells = 5;
	static constexpr float kGap = 4.0f;
	static constexpr float kHeight = 24.0f;

	/// THE CELL UNDER `local`, or `kCells` for none (the gaps belong to no cell).
	[[nodiscard]] size_t cellAt(visage::Point local) const;
	[[nodiscard]] visage::Bounds cellBounds(size_t i) const;
	/// THE CELL THE CONTROL'S VALUE SELECTS.
	[[nodiscard]] size_t selected() const;
	/// CELL `i`'S OWN LINE — its model's name, then what it does to the motion.
	[[nodiscard]] static const char* cellTooltip(size_t i);

private:
	void drawPictogram(visage::Canvas& canvas, size_t i, const visage::Bounds& box, uint32_t colour);

	device::Modulatable& model_;
};

} // namespace feathers::ui
