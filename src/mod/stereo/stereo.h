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

#include "mod/stereo/stereo_types.h"

#include <cstddef>

// THE STEREO TOOL'S STATE — the resolved row as `setFields` last handed it, decoded, plus In's right ear when a
// stereo route reaches it (`setFieldsR`). `setFields` means mono: it forgets the right ear.

namespace feathers::mod::stereo {

class StereoTool {
public:
	void prepare(double sampleRate) noexcept;
	void setFields(const float* fields, size_t count) noexcept;
	void setFieldsR(const float* fieldsR, size_t count, const device::ControlMask& live) noexcept;
	void tick() noexcept {}

	/// The mono reading of an output — its LEFT ear (a mono destination reads the left ear).
	[[nodiscard]] float outputAt(size_t n) const noexcept { return pairAt(n).left; }
	[[nodiscard]] device::EarPair pairAt(size_t n) const noexcept;

	[[nodiscard]] float amountForTest() const noexcept { return amount_; }
	[[nodiscard]] float inForTest() const noexcept { return in_; }
	[[nodiscard]] float inRForTest() const noexcept { return inR_; }
	[[nodiscard]] float widthForTest() const noexcept { return width_; }
	[[nodiscard]] bool swapForTest() const noexcept { return swap_; }

private:
	float amount_ = 1.0f;
	float in_ = 0.0f;
	float inR_ = 0.0f;
	float width_ = 1.0f;
	bool swap_ = false;
};

} // namespace feathers::mod::stereo
