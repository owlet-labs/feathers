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

#include "fx/eq/eq_types.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace feathers::fx::eq {

namespace {

[[nodiscard]] float fieldOr(const Row& row, size_t fieldsPresent, size_t i) {
	return i < fieldsPresent ? row[i] : static_cast<float>(kParamDescs[i].def);
}

[[nodiscard]] float rangedOr(const Row& row, size_t fieldsPresent, size_t i) {
	const device::ParamDesc& d = kParamDescs[i];
	const double v = static_cast<double>(fieldOr(row, fieldsPresent, i));
	return static_cast<float>(std::isfinite(v) ? std::clamp(v, d.min, d.max) : d.def);
}

[[nodiscard]] int choiceOr(const Row& row, size_t fieldsPresent, size_t i) {
	const device::ParamDesc& d = kParamDescs[i];
	const float v = fieldOr(row, fieldsPresent, i);
	if (!std::isfinite(v)) {
		return static_cast<int>(d.def);
	}
	return std::clamp(static_cast<int>(std::lround(v)), static_cast<int>(d.min), static_cast<int>(d.max));
}

/// WHAT THE THREE CONTROLS WITH NO HOUSE UNIT PRINT. Q is stored as log2(Q) and read as Q; Offset is
/// octaves; Tilt is dB per octave. Doing it here rather than adding units to the device standard keeps
/// the EQ's vocabulary the EQ's (assumption A4).
bool formatText(size_t control, double value, const device::TextView&, char* out, size_t size) {
	if (control == paramIndex(Param::Offset)) {
		std::snprintf(out, size, "%+.2f oct", value);
		return true;
	}
	if (control == paramIndex(Param::Tilt)) {
		std::snprintf(out, size, "%+.1f dB/oct", value);
		return true;
	}
	if (control >= kGlobalCount && (control - kGlobalCount) % kBandFields == static_cast<size_t>(BandField::Q)) {
		std::snprintf(out, size, "Q %.2f", std::exp2(value));
		return true;
	}
	return false;
}

} // namespace

const device::TextOps kTextOps { .format = &formatText };

Settings stateOf(const Row& row, size_t fieldsPresent) {
	Settings s;
	s.enabled = fieldOr(row, fieldsPresent, paramIndex(Param::Enabled)) >= 0.5f;
	s.offsetOctaves = rangedOr(row, fieldsPresent, paramIndex(Param::Offset));
	s.amount = rangedOr(row, fieldsPresent, paramIndex(Param::Amount));
	s.tiltDbPerOctave = rangedOr(row, fieldsPresent, paramIndex(Param::Tilt));
	s.tiltPivotLog2 = rangedOr(row, fieldsPresent, paramIndex(Param::TiltPivot));
	s.outputDb = rangedOr(row, fieldsPresent, paramIndex(Param::Output));
	s.linearPhase = choiceOr(row, fieldsPresent, paramIndex(Param::Phase)) == 1;
	s.keyTrack = choiceOr(row, fieldsPresent, paramIndex(Param::KeyTrack)) == 1;
	for (int b = 0; b < kBandCount; ++b) {
		Settings::BandParams& p = s.bands[static_cast<size_t>(b)];
		p.on = fieldOr(row, fieldsPresent, bandIndex(b, BandField::On)) >= 0.5f;
		p.type = static_cast<BandType>(choiceOr(row, fieldsPresent, bandIndex(b, BandField::Type)));
		p.freqLog2 = rangedOr(row, fieldsPresent, bandIndex(b, BandField::Freq));
		p.gainDb = rangedOr(row, fieldsPresent, bandIndex(b, BandField::Gain));
		p.qLog2 = rangedOr(row, fieldsPresent, bandIndex(b, BandField::Q));
		p.slopeIndex = choiceOr(row, fieldsPresent, bandIndex(b, BandField::Slope));
	}
	return s;
}

Row rowOf(const Settings& s) {
	Row row {};
	row[paramIndex(Param::Enabled)] = s.enabled ? 1.0f : 0.0f;
	row[paramIndex(Param::Offset)] = s.offsetOctaves;
	row[paramIndex(Param::Amount)] = s.amount;
	row[paramIndex(Param::Tilt)] = s.tiltDbPerOctave;
	row[paramIndex(Param::TiltPivot)] = s.tiltPivotLog2;
	row[paramIndex(Param::Output)] = s.outputDb;
	row[paramIndex(Param::Phase)] = s.linearPhase ? 1.0f : 0.0f;
	row[paramIndex(Param::KeyTrack)] = s.keyTrack ? 1.0f : 0.0f;
	// THE FURNITURE'S DEFAULTS: the slot reads these, the device never does, and a zero Mix would be silence.
	row[paramIndex(Param::Mix)] = 1.0f;
	row[paramIndex(Param::WetGain)] = 0.5f;
	for (int b = 0; b < kBandCount; ++b) {
		const Settings::BandParams& p = s.bands[static_cast<size_t>(b)];
		row[bandIndex(b, BandField::On)] = p.on ? 1.0f : 0.0f;
		row[bandIndex(b, BandField::Type)] = static_cast<float>(static_cast<int>(p.type));
		row[bandIndex(b, BandField::Freq)] = p.freqLog2;
		row[bandIndex(b, BandField::Gain)] = p.gainDb;
		row[bandIndex(b, BandField::Q)] = p.qLog2;
		row[bandIndex(b, BandField::Slope)] = static_cast<float>(p.slopeIndex);
	}
	return row;
}

} // namespace feathers::fx::eq
