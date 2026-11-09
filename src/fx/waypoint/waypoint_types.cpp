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

#include "fx/waypoint/waypoint_types.h"

#include <algorithm>
#include <cmath>

namespace feathers::fx::waypoint {

namespace flt = fx::filter;



namespace {

/// WHERE A CONTROL'S VALUE COMES FROM WHEN THE ROW DOES NOT CARRY IT. device_row.h's rule: an absent
/// field is at its DECLARED DEFAULT, never at zero — which for Cutoff is log2(1000) and not log2(1).
[[nodiscard]] float fieldOr(const Row& row, size_t fieldsPresent, Param p) {
	const size_t i = static_cast<size_t>(p);
	return i < fieldsPresent ? row[i] : static_cast<float>(kParamDescs[i].def);
}

[[nodiscard]] constexpr size_t firstFieldOf(size_t waypoint) {
	return static_cast<size_t>(Param::Cutoff0) + waypoint * 4;
}

} // namespace

Settings stateOf(const Row& row, size_t fieldsPresent) {
	Settings s;
	s.enabled = fieldOr(row, fieldsPresent, Param::Enabled) >= 0.5f;
	const int type = static_cast<int>(std::lround(fieldOr(row, fieldsPresent, Param::Type)));
	s.type = static_cast<flt::Type>(
	    std::clamp(type, 0, kTypeCount - 1));
	s.morph = std::clamp(fieldOr(row, fieldsPresent, Param::Morph), 0.0f, 1.0f);
	s.keytrack = fieldOr(row, fieldsPresent, Param::Keytrack) >= 0.5f;
	s.poly = fieldOr(row, fieldsPresent, Param::Poly) >= 0.5f;
	for (size_t w = 0; w < kWaypoints; ++w) {
		const size_t at = firstFieldOf(w);
		s.point[w].cutoffLog2Hz = fieldOr(row, fieldsPresent, static_cast<Param>(at + 0));
		s.point[w].resonance = fieldOr(row, fieldsPresent, static_cast<Param>(at + 1));
		s.point[w].macro1 = fieldOr(row, fieldsPresent, static_cast<Param>(at + 2));
		s.point[w].macro2 = fieldOr(row, fieldsPresent, static_cast<Param>(at + 3));
	}
	return s;
}

Row rowOf(const Settings& s) {
	Row row {};
	row[static_cast<size_t>(Param::Enabled)] = s.enabled ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::Type)] = static_cast<float>(static_cast<int>(s.type));
	row[static_cast<size_t>(Param::Morph)] = s.morph;
	row[static_cast<size_t>(Param::Keytrack)] = s.keytrack ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::Poly)] = s.poly ? 1.0f : 0.0f;
	// THE FURNITURE'S DEFAULTS: the slot reads these, the device never does, and a zero Mix would be silence.
	row[static_cast<size_t>(Param::Mix)] = 1.0f;
	row[static_cast<size_t>(Param::WetGain)] = 0.5f;
	for (size_t w = 0; w < kWaypoints; ++w) {
		const size_t at = firstFieldOf(w);
		row[at + 0] = s.point[w].cutoffLog2Hz;
		row[at + 1] = s.point[w].resonance;
		row[at + 2] = s.point[w].macro1;
		row[at + 3] = s.point[w].macro2;
	}
	return row;
}

MorphAt morphAt(double morph) {
	// BY INDEX, IN EQUAL THIRDS. Three segments across four waypoints, so a morph of 1/3 lands
	// EXACTLY on waypoint 2 rather than a third of the way along a polyline whose dots may sit
	// anywhere. See the header: arc length is the thing this is deliberately not.
	constexpr double kSegments = static_cast<double>(kWaypoints - 1);
	const double t = std::clamp(morph, 0.0, 1.0) * kSegments;
	// FLOORED, THEN CLAMPED TO THE LAST SEGMENT, so morph == 1.0 lands on the final waypoint rather
	// than indexing one past the end — the fencepost this shape always has, and the one whose
	// known-bad came back GREEN until `morphAt` was split out to be asserted directly.
	MorphAt at;
	at.segment = static_cast<size_t>(std::min(std::floor(t), kSegments - 1.0));
	at.fraction = t - static_cast<double>(at.segment);
	return at;
}

flt::Settings settingsAtMorph(const Settings& s, double morph) {
	const MorphAt at = morphAt(morph);
	const size_t seg = at.segment;
	const double f = at.fraction;

	const Waypoint& a = s.point[seg];
	const Waypoint& b = s.point[seg + 1];
	const auto lerp = [f](float lo, float hi) {
		return static_cast<float>(static_cast<double>(lo)
		                          + (static_cast<double>(hi) - static_cast<double>(lo)) * f);
	};

	flt::Settings out;
	out.enabled = s.enabled;
	out.type = s.type;
	// LERPED IN LOG2 HERTZ, WHICH IS WHY THIS IS THE PARAMETER ROAD AND NOT THE COEFFICIENT ONE.
	// The field is already logarithmic, so a straight lerp of it is octave interpolation: the
	// midpoint of 1 kHz and 4 kHz is 2 kHz, the geometric mean, not 2.5 kHz. Lerping hertz would
	// crowd every intermediate corner toward the bottom of the sweep and sound wrong in exactly the
	// register a filter sweep is most audible.
	out.cutoffLog2Hz = lerp(a.cutoffLog2Hz, b.cutoffLog2Hz);
	out.resonance = lerp(a.resonance, b.resonance);
	out.macro1 = lerp(a.macro1, b.macro1);
	out.macro2 = lerp(a.macro2, b.macro2);
	return out;
}

} // namespace feathers::fx::waypoint
