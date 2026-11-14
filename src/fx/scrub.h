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

#include <cmath>

namespace feathers::fx {

/// **A NON-FINITE INPUT SAMPLE IS TREATED AS 0, IN PLACE, BEFORE ANYTHING SEES IT** — the idiom EQ, Comp and Clip
/// already carry, in one place. One NaN in a recursive section, a delay line or an envelope is a NaN in every later
/// sample; zero costs a sample of the signal and leaves the device on the trajectory it would have had. A finite
/// sample is rewritten with itself, so a finite block is bit-identical. `right` may be null (the mono road).
inline void scrubNonFinite(float* left, float* right, int frames) noexcept {
	for (int i = 0; i < frames; ++i) {
		left[i] = std::isfinite(left[i]) ? left[i] : 0.0f;
	}
	if (right != nullptr) {
		for (int i = 0; i < frames; ++i) {
			right[i] = std::isfinite(right[i]) ? right[i] : 0.0f;
		}
	}
}

} // namespace feathers::fx
