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

#include "fx/comp/comp_types.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace feathers::fx::comp {

namespace {

/// WHERE A CONTROL'S VALUE COMES FROM WHEN THE ROW DOES NOT CARRY IT — its DECLARED default, never
/// zero (device_row.h's rule). A Ratio read as zero would divide by it.
[[nodiscard]] float fieldOr(const Row& row, size_t fieldsPresent, Param p) {
	const size_t i = static_cast<size_t>(p);
	return i < fieldsPresent ? row[i] : static_cast<float>(kParamDescs[i].def);
}

/// A CONTINUOUS FIELD, CLAMPED TO ITS DECLARED RANGE.
[[nodiscard]] float rangedOr(const Row& row, size_t fieldsPresent, Param p) {
	const auto& d = kParamDescs[static_cast<size_t>(p)];
	return std::clamp(fieldOr(row, fieldsPresent, p), static_cast<float>(d.min), static_cast<float>(d.max));
}

/// A CHOICE, ROUNDED AND CLAMPED INTO ITS LADDER, so a row from a build with more steps lands on a
/// real one rather than indexing past the table.
[[nodiscard]] int choiceOr(const Row& row, size_t fieldsPresent, Param p) {
	const auto& d = kParamDescs[static_cast<size_t>(p)];
	return std::clamp(static_cast<int>(std::lround(fieldOr(row, fieldsPresent, p))), static_cast<int>(d.min),
	                  static_cast<int>(d.max));
}

/// RATIO AS A RATIO. One decimal because the knob is continuous and 4.0 and 4.4 are different curves;
/// "∞:1" from `kRatioInfinite` up, because there the slope IS zero (`slopeFor`) and "20.0:1" would
/// promise a gentler curve than the DSP runs.
bool formatText(size_t control, double value, const device::TextView&, char* out, size_t size) {
	// THE COMPANDER'S RATIO (backlog QG) prints the same way; its travel stops at 4, so it is never infinite.
	if (control == static_cast<size_t>(Param::CmpRatio) || control == static_cast<size_t>(Param::CmpRatio2)) {
		std::snprintf(out, size, "%.1f:1", value);
		return true;
	}
	if (control != static_cast<size_t>(Param::Ratio)) {
		return false;
	}
	if (value >= static_cast<double>(kRatioInfinite)) {
		std::snprintf(out, size, "\xE2\x88\x9E:1"); // U+221E
	}
	else {
		std::snprintf(out, size, "%.1f:1", value);
	}
	return true;
}

} // namespace

const device::TextOps kTextOps { .format = &formatText };

Settings stateOf(const Row& row, size_t fieldsPresent) {
	Settings s;
	s.enabled = fieldOr(row, fieldsPresent, Param::Enabled) >= 0.5f;
	s.mode = choiceOr(row, fieldsPresent, Param::Mode);
	s.thresholdDb = rangedOr(row, fieldsPresent, Param::Threshold);
	s.ratio = rangedOr(row, fieldsPresent, Param::Ratio);
	s.kneeDb = rangedOr(row, fieldsPresent, Param::Knee);
	s.attack = rangedOr(row, fieldsPresent, Param::Attack);
	s.release = rangedOr(row, fieldsPresent, Param::Release);
	s.detect = choiceOr(row, fieldsPresent, Param::Detect);
	s.scHpf = choiceOr(row, fieldsPresent, Param::ScHpf);
	s.makeupDb = rangedOr(row, fieldsPresent, Param::Makeup);
	s.mix = rangedOr(row, fieldsPresent, Param::Mix);
	s.driveDb = rangedOr(row, fieldsPresent, Param::Drive);
	s.ceilingDb = rangedOr(row, fieldsPresent, Param::Ceiling);
	s.stereoLink = rangedOr(row, fieldsPresent, Param::StereoLink);
	s.stereoMode = choiceOr(row, fieldsPresent, Param::StereoMode);
	s.detector = choiceOr(row, fieldsPresent, Param::Detector);
	s.cmpRatio = rangedOr(row, fieldsPresent, Param::CmpRatio);
	s.cmpRangeDb = rangedOr(row, fieldsPresent, Param::CmpRange);
	s.topology = choiceOr(row, fieldsPresent, Param::Topology);
	s.cmpLink = fieldOr(row, fieldsPresent, Param::CmpLink) >= 0.5f;
	s.cmpRatio2 = rangedOr(row, fieldsPresent, Param::CmpRatio2);
	s.cmpAttack2 = rangedOr(row, fieldsPresent, Param::CmpAttack2);
	s.cmpRelease2 = rangedOr(row, fieldsPresent, Param::CmpRelease2);
	return s;
}

Row rowOf(const Settings& s) {
	Row row {};
	row[static_cast<size_t>(Param::Enabled)] = s.enabled ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::Mode)] = static_cast<float>(s.mode);
	row[static_cast<size_t>(Param::Threshold)] = s.thresholdDb;
	row[static_cast<size_t>(Param::Ratio)] = s.ratio;
	row[static_cast<size_t>(Param::Knee)] = s.kneeDb;
	row[static_cast<size_t>(Param::Attack)] = s.attack;
	row[static_cast<size_t>(Param::Release)] = s.release;
	row[static_cast<size_t>(Param::Detect)] = static_cast<float>(s.detect);
	row[static_cast<size_t>(Param::ScHpf)] = static_cast<float>(s.scHpf);
	row[static_cast<size_t>(Param::Makeup)] = s.makeupDb;
	row[static_cast<size_t>(Param::Mix)] = s.mix;
	row[static_cast<size_t>(Param::Drive)] = s.driveDb;
	row[static_cast<size_t>(Param::Ceiling)] = s.ceilingDb;
	row[static_cast<size_t>(Param::StereoLink)] = s.stereoLink;
	row[static_cast<size_t>(Param::StereoMode)] = static_cast<float>(s.stereoMode);
	row[static_cast<size_t>(Param::Detector)] = static_cast<float>(s.detector);
	row[static_cast<size_t>(Param::CmpRatio)] = s.cmpRatio;
	row[static_cast<size_t>(Param::CmpRange)] = s.cmpRangeDb;
	row[static_cast<size_t>(Param::Topology)] = static_cast<float>(s.topology);
	row[static_cast<size_t>(Param::CmpLink)] = s.cmpLink ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::CmpRatio2)] = s.cmpRatio2;
	row[static_cast<size_t>(Param::CmpAttack2)] = s.cmpAttack2;
	row[static_cast<size_t>(Param::CmpRelease2)] = s.cmpRelease2;
	return row;
}

} // namespace feathers::fx::comp
