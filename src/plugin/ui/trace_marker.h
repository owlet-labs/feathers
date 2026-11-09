/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <cmath>
#include <cstddef>
#include <span>

// WHERE A PARAMETER VALUE LANDS ON A SAMPLED CURVE — and nothing else.
//
// **THE GLYPH IS NOT HERE, DELIBERATELY.** `celestial.h`'s `starGlyph` already draws this project's
// star — a halo, a cross glint, a white core and a coloured heart — and three surfaces call it. A
// second star would be two places that must agree about what a star looks like, which is the
// commonest defect shape in this repo. What was actually missing is the PLACEMENT.
//
// **THE SECOND CALLER IS ALREADY DECIDED, WHICH IS WHY THIS IS SHARED BEFORE IT HAS ONE.** The same
// marker is committed for the MSEG's shape. Landing it as a call now means the second
// surface is a call rather than a redesign — and this file is a header with no state and no
// drawing, so nothing about it constrains a caller's geometry.
//
// **IT INTERPOLATES IN SCREEN COORDINATES, AND THAT IS THE CORRECT CHOICE RATHER THAN THE LAZY
// ONE.** A trace is DRAWN as straight segments between its vertices, so the point ON THE DRAWN LINE
// at a parameter is the screen-space blend of two screen positions. Interpolating in VALUE space
// and mapping afterwards puts the marker OFF the line wherever the axis is non-linear — which the
// automod strip's log-frequency axis is, and which the MSEG's curved segments are. A marker beside
// the curve it claims to be on is the whole of what this must not do.

namespace feathers::ui {

struct TraceMarker {
	/// WHERE `at` FALLS BETWEEN TWO VERTICES.
	struct Landing {
		size_t lo = 0;
		size_t hi = 0;
		/// 0 at `lo`, 1 at `hi`.
		float frac = 0.0f;
		bool valid = false;
	};

	/// LOCATE `at` IN A NON-DECREASING KEY.
	///
	/// CLAMPED TO THE ENDS rather than refused: a phase of exactly 1.0 and a phase of 0.999999 are
	/// the same instant to a reader, and a marker that vanished at the right-hand edge would look
	/// like a defect once per cycle. **A key with fewer than two vertices, or a non-finite `at`, is
	/// REFUSED** — there is no line to be on, and a marker at NaN is drawn nowhere at all, which is
	/// a silently missing element rather than a visibly wrong one.
	[[nodiscard]] static Landing locate(std::span<const float> key, float at) {
		return locateBy(key.size(), [key](size_t i) { return key[i]; }, at);
	}

	/// THE SAME SEARCH WHERE THE KEY IS NOT A CONTIGUOUS ARRAY OF FLOATS.
	///
	/// **THE SPAN FORM DELEGATES TO THIS ONE RATHER THAN REPEATING IT**, which is the guarantee
	/// `Lfo::setShape`'s pair of overloads is written under: two definitions that agree today are two
	/// definitions that can stop agreeing, and the only defence available in a header is that one of
	/// them cannot fork by construction.
	///
	/// It exists because the callers hold their polylines differently and none of them is a
	/// `vector<float>`: the MSEG's is `vector<pair<float,float>>` and the envelope card's is
	/// `vector<visage::Point>`. Asking each to build a parallel array of x's would be a copy per
	/// frame on a per-frame path, to satisfy a signature.
	template <typename KeyAt>
	[[nodiscard]] static Landing locateBy(size_t count, KeyAt keyAt, float at) {
		Landing out;
		if (count < 2 || !std::isfinite(at)) {
			return out;
		}
		const auto key = [&](size_t i) { return static_cast<float>(keyAt(i)); };
		out.valid = true;
		if (at <= key(0)) {
			out.lo = 0;
			out.hi = 1;
			out.frac = 0.0f;
			return out;
		}
		if (at >= key(count - 1)) {
			out.lo = count - 2;
			out.hi = count - 1;
			out.frac = 1.0f;
			return out;
		}
		// LINEAR, AND MEASURED RATHER THAN ASSUMED TO MATTER: the traces this serves are 55 points on
		// the automod strip and a few dozen on the MSEG. A binary search over that is a different way
		// to spell the same nanosecond, and this version is one anybody can check by reading.
		size_t hi = 1;
		while (hi + 1 < count && key(hi) < at) {
			++hi;
		}
		out.lo = hi - 1;
		out.hi = hi;
		const float span = key(out.hi) - key(out.lo);
		// A REPEATED KEY IS A ZERO-LENGTH SEGMENT, and dividing by its span is how a marker ends up
		// at infinity. The vertex itself is the honest answer there.
		out.frac = (span > 0.0f) ? (at - key(out.lo)) / span : 0.0f;
		return out;
	}

	/// BLEND TWO SCREEN COORDINATES AT A LANDING. See the file header for why this takes pixels.
	[[nodiscard]] static float at(const Landing& where, float lo, float hi) {
		return lo + (hi - lo) * where.frac;
	}
};

} // namespace feathers::ui
