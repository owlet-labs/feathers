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

#include "mod/math/math_types.h"

#include <cstddef>

// THE MATH DEVICE'S STATE — the resolved row as `setFields` last handed it, decoded.

namespace feathers::mod::math {

class Math {
public:
	void prepare(double sampleRate) noexcept;
	/// THE RESOLVED ROW — A and B after the routes into them. See `math_types.h`.
	void setFields(const float* fields, size_t count) noexcept;
	/// NOTHING MOVES BETWEEN ROWS: the output is a function of the row, so a tick has nothing to advance.
	void tick() noexcept {}

	/// OUTPUT 0: `evaluate` over the decoded row. Zero past the first.
	[[nodiscard]] float outputAt(size_t n) const noexcept;
	/// THE RIGHT EAR OF A, B AND AMOUNT (backlog KU) — each taken where `live` marks it, its left otherwise.
	void setFieldsR(const float* fieldsR, size_t count, const device::ControlMask& live) noexcept;
	/// **THE OUTPUT IN BOTH EARS: `evaluate` once per ear** — the whole arithmetic per ear, so A × identity is
	/// (−A, +A). `left` is `outputAt` for the same output.
	[[nodiscard]] device::EarPair pairAt(size_t n) const noexcept {
		if (n != 0) {
			return {};
		}
		return { evaluate(a_, b_, op_, map_, amount_, bipolar_, octaves_, exponent_),
			     evaluate(aR_, bR_, op_, map_, amountR_, bipolar_, octaves_, exponent_) };
	}

	/// **WHAT `setFields` DECODED**, for the registry-wide round-trip case (`test_device_ranges.cpp`). The
	/// enable is absent because this device does not read it — see `Param::Enabled`.
	[[nodiscard]] float aForTest() const noexcept { return a_; }
	[[nodiscard]] float bForTest() const noexcept { return b_; }
	[[nodiscard]] Op opForTest() const noexcept { return op_; }
	[[nodiscard]] Map mapForTest() const noexcept { return map_; }
	[[nodiscard]] float amountForTest() const noexcept { return amount_; }
	[[nodiscard]] bool bipolarForTest() const noexcept { return bipolar_; }
	[[nodiscard]] float periodsForTest() const noexcept { return octaves_; }
	[[nodiscard]] float exponentForTest() const noexcept { return exponent_; }

private:
	/// THE φ MAP'S PERIODS, AS log2 (`Param::Periods`).
	float octaves_ = device::kPhiOctavesDefault;
	/// LOG'S AND EXP'S EXPONENT (`Param::Exponent`).
	float exponent_ = kMapExponent;
	float a_ = 0.0f;
	float b_ = 0.0f;
	Op op_ = Op::Multiply;
	Map map_ = Map::Lin;
	float amount_ = 1.0f;
	bool bipolar_ = true;
	/// The right ear (backlog KU): equal to the left after every `setFields`, until `setFieldsR` says otherwise.
	float aR_ = 0.0f;
	float bR_ = 0.0f;
	float amountR_ = 1.0f;
};

} // namespace feathers::mod::math
