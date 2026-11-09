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

#include "fx/shaper/shaper_types.h"
#include "plugin/ui/mode_value_cell.h"
#include "plugin/ui/value_box.h"

#include <cmath>
#include <memory>
#include <vector>

namespace feathers::ui {

/// **COLOUR'S MODE CELL — ONE BUILDER FOR THE CARD AND THE PANEL (backlog IW: 2026-09-23).** The dropdown is a
/// mode selector, not a phase region selector: Colour's dropdown lists its MODES, Inflate then Free (Inflate comes
/// first because it is fenced and Free is infinite), and the box beside it is whichever value the mode reads — Colour's unbounded
/// field in Free, the Inflate amount in Inflate. Built here once, so the two surfaces cannot offer different modes.
///
/// `control(p)` answers a stable `device::Modulatable&` for a shaper param, as both surfaces' `control` does.
template <typename ControlOf>
[[nodiscard]] std::unique_ptr<ModeValueCell> makeColourModeCell(ControlOf&& control, float faceDiameter) {
	namespace shp = fx::shaper;
	device::Modulatable& mode = control(shp::Param::ColourMode);
	std::vector<ZoneSelect::ZoneName> names {
		{ shp::kColourModeNames[shp::kColourInflate], "Blends the curve toward a loudness-inflation curve: quiet expanded, loud compressed" },
		{ shp::kColourModeNames[shp::kColourFree], "The continuous Colour field — six shaping curves blended, and it keeps sweeping" },
	};
	// A FOLLOWER IS LIVE WHEN THE CURRENT MODE IS IN ITS DECLARED GATE — read from the descriptor, not restated here.
	auto relevant = [&mode](size_t param) {
		const long now = std::lround(mode.value());
		const uint32_t mask = shp::kParamDescs[param].gateMask;
		return now >= 0 && now < 32 && ((mask >> static_cast<uint32_t>(now)) & 1u) != 0;
	};
	auto cell = std::make_unique<ModeValueCell>(mode, "Colour", std::move(names), relevant);
	for (const shp::Param p : { shp::Param::Colour, shp::Param::InflateAmount }) {
		cell->addFollower(static_cast<size_t>(p), std::make_unique<ValueBox>(control(p), ""));
	}
	cell->setFaceDiameter(faceDiameter);
	cell->refresh();
	return cell;
}

} // namespace feathers::ui
