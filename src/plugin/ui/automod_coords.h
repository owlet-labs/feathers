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

#include "fx/automod/automod_types.h"
#include "plugin/ui/zone_coord_cell.h"

#include <array>
#include <memory>
#include <span>
#include <vector>

namespace feathers::ui {

/// **AUTOMOD'S THREE COORDINATES, SEATED ONCE FOR BOTH SURFACES.**
///
/// DECISION, on the expanded window: the same coarse/fine pairs, displaying the same values as
/// the card. Not a look-alike — the SAME seating, so the two cannot
/// drift. The card and the window now build their coordinate cells through `makeCoordCell` below, and
/// the specs live here rather than in either surface.
///
/// **THIS IS THE `GammaBox` LESSON APPLIED BEFORE IT COSTS ANYTHING.** Gamma's configuration sat as
/// two calls in `automod_card.cpp`, correct there, and the window built later never looked for them
/// and drew a knob across the whole 2^24 range. A third surface turned up with the same fault. A
/// seating that lives beside neither surface is the only version that a new surface cannot miss.
namespace automod_coords {

/// ONE COORDINATE: the axis itself, its zone names, and the caption.
///
/// **ONE PARAM, NOT A PAIR — backlog BJ, piece 3.** The `phase` field is gone because the param it
/// named is gone: the engine only ever read the sum of the two, so they collapsed into one coordinate
/// and the zone names now sit over that. A spec that still carried a second param would be the last
/// place in the editor claiming this device has a coarse:fine pair.
///
/// **AND THE COLLAPSE RETIRES A WHOLE CLASS OF BUG THIS STRUCT WAS BUILT TO PREVENT.** It used to
/// hold two same-typed params, and its own comment recorded what that cost: the fields were once
/// `{ coarse, fine }` here and `{ fine, coarse }` at the original call site, passed POSITIONALLY into
/// the widget, so **every automod box had its halves reversed from `a0e6a25` until that was found** —
/// in the card, in the window, in the installed build. Naming them `zoned` and `phase` was the fix,
/// because two words that describe different things give a reader something to check. One field
/// cannot be transposed with anything at all, which is the better version of the same fix.
struct CoordSpec {
	fx::automod::Param coordinate;
	std::span<const fx::automod::ZoneName> names;
	const char* label;
};

/// THE THREE, IN ROW ORDER. Declared once so no surface can disagree about the order — the same
/// reason the card kept them in one array when it was the only surface.
inline const std::array<CoordSpec, 3>& specs() {
	static const std::array<CoordSpec, 3> kSpecs { {
		{ fx::automod::Param::Type, {}, "Type" }, // FREE ONLY (backlog IW): no names, a sigil
		{ fx::automod::Param::Flavor, {}, "Flavour" },
		{ fx::automod::Param::Mod, {}, "Mod" },
	} };
	return kSpecs;
}

/// A DEVICE'S ZONE NAMES IN THE WIDGET'S VOCABULARY.
///
/// **A TEMPLATE, BECAUSE FOUR BYTE-IDENTICAL COPIES OF THIS EXISTED.** `automod_card.cpp`,
/// `shaper_card.cpp`, `shaper_expanded.cpp` and `owltt_expanded.cpp` each held the same six lines
/// differing only in the namespace of the parameter — and two of the four were the SAME device, which
/// is the tell that it was copy-paste rather than three considered answers. A survey of the
/// zone/gamma coordinates counted them; this collapses automod's. The other three wait on a
/// decision about where the shared
/// `ZoneName` type should live.
template <typename ZoneNameT>
[[nodiscard]] std::vector<ZoneSelect::ZoneName> zonesFrom(std::span<const ZoneNameT> names) {
	std::vector<ZoneSelect::ZoneName> out;
	out.reserve(names.size());
	for (const ZoneNameT& n : names) {
		out.push_back({ n.name, n.description });
	}
	return out;
}

/// THE SEATING ITSELF. Every configured property is here — the lap size, the face diameter, the zone
/// names — so a surface supplies only its control and its size.
///
/// **IT BUILDS A `ZoneCoordCell` NOW, NOT A `ZoneCoordBox`** — backlog BJ, piece 3. The pair behind
/// this device collapsed into one param, so the widget that expressed a pair has nothing left to
/// express. The cell keeps the same footprint: one box where two and a colon were, and the region
/// name row turned into the dropdown.
template <typename ControlOf>
[[nodiscard]] std::unique_ptr<ZoneCoordCell> makeCoordCell(const CoordSpec& spec, ControlOf&& control,
                                                           float faceDiameter) {
	auto cell = std::make_unique<ZoneCoordCell>(control(spec.coordinate), spec.label,
	                                            zonesFrom(spec.names),
	                                            ZoneSelect::kCoordinatePerLap);
	cell->setFaceDiameter(faceDiameter);
	return cell;
}

} // namespace automod_coords
} // namespace feathers::ui
