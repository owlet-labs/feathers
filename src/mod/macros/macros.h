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

#include "mod/macros/macros_types.h"

#include <array>
#include <cstddef>

// THE MACROS' STATE — four values and four polarities, as `setFields` last handed them.

namespace feathers::mod::macros {

class Macros {
public:
	void prepare(double sampleRate) noexcept;
	/// THE RESOLVED ROW — each macro after the routes into it. See `macros_types.h`.
	void setFields(const float* fields, size_t count) noexcept;
	/// NOTHING MOVES BETWEEN ROWS: a macro is where the row says, so a tick has nothing to advance.
	void tick() noexcept {}

	/// OUTPUT n: macro n's value, or `2v − 1` when its Bipolar is on. Zero past the fourth.
	[[nodiscard]] float outputAt(size_t n) const noexcept;
	/// THE RIGHT EAR OF EACH MACRO (backlog KU) — taken where `live` marks it, its left otherwise.
	void setFieldsR(const float* fieldsR, size_t count, const device::ControlMask& live) noexcept;
	/// OUTPUT n IN BOTH EARS — each ear's value through the same polarity. `left` is `outputAt`.
	[[nodiscard]] device::EarPair pairAt(size_t n) const noexcept;

	/// **WHAT `setFields` DECODED**, for the registry-wide round-trip case (`test_device_ranges.cpp`), which
	/// has no other road back from a device that decodes into members. The types and the enable are
	/// absent because this device does not read them — see `Param::Type1` and `Param::Enabled`.
	[[nodiscard]] float valueForTest(size_t n) const noexcept { return n < kMacroCount ? value_[n] : 0.0f; }
	[[nodiscard]] bool bipolarForTest(size_t n) const noexcept { return n < kMacroCount && bipolar_[n]; }

private:
	std::array<float, kMacroCount> value_ {};
	std::array<bool, kMacroCount> bipolar_ {};
	/// The right ear (backlog KU): equal to `value_` after every `setFields`, until `setFieldsR` says otherwise.
	std::array<float, kMacroCount> valueR_ {};
};

} // namespace feathers::mod::macros
