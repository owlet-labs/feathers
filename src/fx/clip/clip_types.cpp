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

#include "fx/clip/clip_types.h"

#include <algorithm>
#include <cmath>

namespace feathers::fx::clip {

namespace {

/// WHERE A CONTROL'S VALUE COMES FROM WHEN THE ROW DOES NOT CARRY IT — its DECLARED default, never
/// zero (device_row.h's rule). An Oversample read as zero would switch the default 4x off.
[[nodiscard]] float fieldOr(const Row& row, size_t fieldsPresent, Param p) {
	const size_t i = static_cast<size_t>(p);
	return i < fieldsPresent ? row[i] : static_cast<float>(kParamDescs[i].def);
}

/// A CONTINUOUS FIELD, CLAMPED TO ITS DECLARED RANGE.
[[nodiscard]] float rangedOr(const Row& row, size_t fieldsPresent, Param p) {
	const auto& d = kParamDescs[static_cast<size_t>(p)];
	return std::clamp(fieldOr(row, fieldsPresent, p), static_cast<float>(d.min), static_cast<float>(d.max));
}

/// A CHOICE, ROUNDED AND CLAMPED INTO ITS LADDER.
[[nodiscard]] int choiceOr(const Row& row, size_t fieldsPresent, Param p) {
	const auto& d = kParamDescs[static_cast<size_t>(p)];
	return std::clamp(static_cast<int>(std::lround(fieldOr(row, fieldsPresent, p))), static_cast<int>(d.min),
	                  static_cast<int>(d.max));
}

} // namespace

Settings stateOf(const Row& row, size_t fieldsPresent) {
	Settings s;
	s.enabled = fieldOr(row, fieldsPresent, Param::Enabled) >= 0.5f;
	s.mode = choiceOr(row, fieldsPresent, Param::Mode);
	s.driveDb = rangedOr(row, fieldsPresent, Param::Drive);
	s.ceilingDb = rangedOr(row, fieldsPresent, Param::Ceiling);
	s.outputDb = rangedOr(row, fieldsPresent, Param::Output);
	s.oversample = fieldOr(row, fieldsPresent, Param::Oversample) >= 0.5f;
	s.knee = rangedOr(row, fieldsPresent, Param::Knee);
	s.freq = rangedOr(row, fieldsPresent, Param::Freq);
	s.foldback = rangedOr(row, fieldsPresent, Param::Foldback);
	s.grit = rangedOr(row, fieldsPresent, Param::Grit);
	s.depth = rangedOr(row, fieldsPresent, Param::Depth);
	s.bits = choiceOr(row, fieldsPresent, Param::Bits);
	return s;
}

Row rowOf(const Settings& s) {
	Row row {};
	row[static_cast<size_t>(Param::Enabled)] = s.enabled ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::Mode)] = static_cast<float>(s.mode);
	row[static_cast<size_t>(Param::Drive)] = s.driveDb;
	row[static_cast<size_t>(Param::Ceiling)] = s.ceilingDb;
	row[static_cast<size_t>(Param::Output)] = s.outputDb;
	row[static_cast<size_t>(Param::Oversample)] = s.oversample ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::Knee)] = s.knee;
	row[static_cast<size_t>(Param::Freq)] = s.freq;
	row[static_cast<size_t>(Param::Foldback)] = s.foldback;
	row[static_cast<size_t>(Param::Grit)] = s.grit;
	row[static_cast<size_t>(Param::Depth)] = s.depth;
	row[static_cast<size_t>(Param::Bits)] = static_cast<float>(s.bits);
	// THE FURNITURE'S DEFAULTS: the slot reads these, the device never does, and a zero Mix would be silence.
	row[static_cast<size_t>(Param::Mix)] = 1.0f;
	row[static_cast<size_t>(Param::WetGain)] = 0.5f;
	return row;
}

} // namespace feathers::fx::clip
