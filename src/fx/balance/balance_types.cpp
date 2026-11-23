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

#include "fx/balance/balance_types.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace feathers::fx::balance {

namespace {

/// WHERE A CONTROL'S VALUE COMES FROM WHEN THE ROW DOES NOT CARRY IT — its DECLARED default, never zero.
[[nodiscard]] float fieldOr(const Row& row, size_t fieldsPresent, Param p) {
	const size_t i = static_cast<size_t>(p);
	return i < fieldsPresent ? row[i] : static_cast<float>(kParamDescs[i].def);
}

[[nodiscard]] float rangedOr(const Row& row, size_t fieldsPresent, Param p) {
	const auto& d = kParamDescs[static_cast<size_t>(p)];
	const float v = fieldOr(row, fieldsPresent, p);
	return std::clamp(std::isfinite(v) ? v : static_cast<float>(d.def), static_cast<float>(d.min), static_cast<float>(d.max));
}

} // namespace

Settings stateOf(const Row& row, size_t fieldsPresent) {
	Settings s;
	s.enabled = fieldOr(row, fieldsPresent, Param::Enabled) >= 0.5f;
	s.bands = fieldOr(row, fieldsPresent, Param::Bands) >= 0.5f ? 3 : 2;
	s.spacing = rangedOr(row, fieldsPresent, Param::Spacing);
	s.spacing2 = rangedOr(row, fieldsPresent, Param::Spacing2);
	s.keyTrack = fieldOr(row, fieldsPresent, Param::KeyTrack) >= 0.5f;
	s.sharp = fieldOr(row, fieldsPresent, Param::Sharp) >= 0.5f;
	s.strength = rangedOr(row, fieldsPresent, Param::Strength);
	s.attack = rangedOr(row, fieldsPresent, Param::Attack);
	s.release = rangedOr(row, fieldsPresent, Param::Release);
	s.pan = rangedOr(row, fieldsPresent, Param::Pan);
	s.hpWet = rangedOr(row, fieldsPresent, Param::HpWet);
	s.subThru = fieldOr(row, fieldsPresent, Param::SubThru) >= 0.5f;
	return s;
}

Row rowOf(const Settings& s) {
	Row row {};
	row[static_cast<size_t>(Param::Enabled)] = s.enabled ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::Bands)] = s.bands >= 3 ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::Spacing)] = s.spacing;
	row[static_cast<size_t>(Param::Spacing2)] = s.spacing2;
	row[static_cast<size_t>(Param::KeyTrack)] = s.keyTrack ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::Sharp)] = s.sharp ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::Strength)] = s.strength;
	row[static_cast<size_t>(Param::Attack)] = s.attack;
	row[static_cast<size_t>(Param::Release)] = s.release;
	row[static_cast<size_t>(Param::Pan)] = s.pan;
	row[static_cast<size_t>(Param::HpWet)] = s.hpWet;
	row[static_cast<size_t>(Param::SubThru)] = s.subThru ? 1.0f : 0.0f;
	// THE FURNITURE'S DEFAULTS: the slot reads these, the device never does, and a zero Mix would be silence.
	row[static_cast<size_t>(Param::Mix)] = 1.0f;
	row[static_cast<size_t>(Param::WetGain)] = 0.5f;
	return row;
}

namespace {

[[nodiscard]] bool viewTracking(const device::TextView& view) noexcept {
	constexpr size_t kNoteHz = static_cast<size_t>(Reading::NoteHz);
	return view.readingCount != nullptr && view.reading != nullptr && view.readingCount(view.host) > kNoteHz && view.reading(view.host, kNoteHz) != 0.0f;
}

[[nodiscard]] bool isSpacing(size_t control) noexcept {
	return control == static_cast<size_t>(Param::Spacing) || control == static_cast<size_t>(Param::Spacing2);
}

} // namespace

const device::TextOps kTextOps {
	.format = [](size_t control, double value, const device::TextView& view, char* out, size_t size) {
		if (!isSpacing(control) || !viewTracking(view)) {
			return false;
		}
		const std::string text = owltt::combRatioText(owltt::combRatioOfKnob(value, control == static_cast<size_t>(Param::Spacing2)));
		std::snprintf(out, size, "%s", text.c_str());
		return true;
	},
	// "x2", "2", "x1/2", "1/3" — a ratio to the note, back to the knob's own units.
	.parse = [](size_t control, const char* text, const device::TextView& view, double* value) {
		if (!isSpacing(control) || !viewTracking(view) || text == nullptr) {
			return false;
		}
		const char* s = text;
		while (*s == ' ' || *s == 'x' || *s == 'X') {
			++s;
		}
		char* end = nullptr;
		double ratio = std::strtod(s, &end);
		if (end == s || !std::isfinite(ratio)) {
			return false;
		}
		if (*end == '/') {
			const char* d = end + 1;
			const double den = std::strtod(d, &end);
			if (end == d || !(den > 0.0)) {
				return false;
			}
			ratio /= den;
		}
		if (!(ratio > 0.0)) {
			return false;
		}
		*value = owltt::combKnobOfRatio(ratio, control == static_cast<size_t>(Param::Spacing2));
		return true;
	},
};

} // namespace feathers::fx::balance
