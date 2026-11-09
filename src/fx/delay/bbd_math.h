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

// **BUCKETS' PER-FRAME MATHS, PRICED** (backlog PV). The clock whine's
// `std::sin` per frame, replaced, its drift pinned by value in `test_delay_lane` ("PV: ..."). The compander's `powf` was
// tried as an exp2/log2 polynomial too and measured SLOWER, so it stays.

#include <cmath>

namespace feathers::fx::delay {

/// **THE CLOCK'S WHINE, sin(2π·phase), AS A ROTATING PHASOR** — four multiplies a frame instead of `std::sin`. The phase
/// advance is constant through a block (the clock is resolved once a block), so the rotation is too; the phasor is
/// RE-ANCHORED from the exact phase at every block's start, so its error never accumulates past one block: in double,
/// about 1e-15 a step, 1e-12 over the longest block, well under the float the whine is added in.
struct WhineRotor {
	double s = 0.0;
	double c = 1.0;
	double ds = 0.0;
	double dc = 1.0;

	/// AT `phase` (cycles), ADVANCING `ratio` CYCLES A FRAME.
	void anchor(double phase, double ratio) noexcept {
		constexpr double kTwoPi = 6.283185307179586;
		s = std::sin(kTwoPi * phase);
		c = std::cos(kTwoPi * phase);
		ds = std::sin(kTwoPi * ratio);
		dc = std::cos(kTwoPi * ratio);
	}
	/// ONE FRAME ON; returns the sine at the new phase.
	double step() noexcept {
		const double sn = s * dc + c * ds;
		c = c * dc - s * ds;
		s = sn;
		return s;
	}
};

} // namespace feathers::fx::delay
