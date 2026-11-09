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

#include "dsp/basic_shapes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

namespace feathers {

namespace shapes_detail {
// Static storage (~0.9 MB), zero until warmShapeTables fills it.
float gSine[kShapesTableSize + 1];
float gLadders[3][kShapesLadderLength];
} // namespace shapes_detail

namespace {

/// Harmonic k's amplitude in each shape's sine series (zero where the shape has no such partial).
double amplitude(Shape shape, int k) {
	const double kd = static_cast<double>(k);
	switch (shape) {
	case Shape::Saw:
		return 2.0 / (std::numbers::pi * kd);
	case Shape::Square:
		return (k % 2 == 1) ? 4.0 / (std::numbers::pi * kd) : 0.0;
	case Shape::Triangle:
		if (k % 2 == 0) {
			return 0.0;
		}
		return (((k - 1) / 2) % 2 == 0 ? 1.0 : -1.0) * 8.0 / (std::numbers::pi * std::numbers::pi * kd * kd);
	default:
		return k == 1 ? 1.0 : 0.0;
	}
}

/// One exact sine at length `len`: harmonic k at sample n is s[(k·n) mod len], read by an integer index, so no
/// partial carries any error but the double sum's.
std::vector<double> exactSine(int len) {
	std::vector<double> s(static_cast<size_t>(len));
	for (int n = 0; n < len; ++n) {
		s[static_cast<size_t>(n)] = std::sin(2.0 * std::numbers::pi * static_cast<double>(n) / static_cast<double>(len));
	}
	return s;
}

void build() {
	using namespace shapes_detail;
	// Built once, before any voice exists (warmShapeTables), so the vectors here are the message thread's.
	const std::vector<double> base = exactSine(kShapesTableSize);
	for (int n = 0; n < kShapesTableSize; ++n) {
		gSine[n] = static_cast<float>(base[static_cast<size_t>(n)]);
	}
	gSine[kShapesTableSize] = gSine[0];

	constexpr std::array<Shape, 3> kLadders { Shape::Triangle, Shape::Saw, Shape::Square };
	for (int m = 0; m < kShapesMipCount; ++m) {
		const int len = shapesMipLength(m);
		const int top = kShapesTopHarmonics >> m;
		const std::vector<double> s = exactSine(len);
		std::vector<double> acc(static_cast<size_t>(len));
		for (size_t l = 0; l < kLadders.size(); ++l) {
			std::fill(acc.begin(), acc.end(), 0.0);
			for (int k = 1; k <= top; ++k) {
				const double a = amplitude(kLadders[l], k);
				if (a == 0.0) {
					continue;
				}
				int at = 0; // (k·n) mod len, stepped rather than divided
				for (int n = 0; n < len; ++n) {
					acc[static_cast<size_t>(n)] += a * s[static_cast<size_t>(at)];
					at += k;
					if (at >= len) {
						at -= len;
					}
				}
			}
			float* row = gLadders[l] + shapesMipOffset(m);
			for (int n = 0; n < len; ++n) {
				row[n] = static_cast<float>(acc[static_cast<size_t>(n)]);
			}
			row[len] = row[0];
		}
	}
}

} // namespace

ShapesWeights normaliseShapes(const ShapesMix& mix) {
	const float sine = std::max(mix.sine, 0.0f) + kShapesSineEpsilon;
	const float triangle = std::max(mix.triangle, 0.0f);
	const float saw = std::max(mix.saw, 0.0f);
	const float square = std::max(mix.square, 0.0f);
	const float sum = sine + triangle + saw + square;
	return { sine / sum, triangle / sum, saw / sum, square / sum };
}

void warmShapeTables() {
	// Thread-safe one-time initialisation (C++11 statics); the data itself is the file-scope storage above.
	static const bool built = [] {
		build();
		return true;
	}();
	static_cast<void>(built);
}

const float* shapeTable(Shape shape, int mip) {
	warmShapeTables();
	const int m = std::clamp(mip, 0, kShapesMipCount - 1);
	const int at = shapesMipOffset(m);
	switch (shape) {
	case Shape::Triangle:
		return shapes_detail::gLadders[0] + at;
	case Shape::Saw:
		return shapes_detail::gLadders[1] + at;
	case Shape::Square:
		return shapes_detail::gLadders[2] + at;
	default:
		return shapes_detail::gSine;
	}
}

int shapesMipFor(double topHz, double limitHz) {
	if (!(topHz > 0.0) || !(limitHz > 0.0)) {
		return 0;
	}
	const double allowed = limitHz / topHz; // how many harmonics fit under the limit
	int m = 0;
	while (m < kShapesMipCount - 1 && static_cast<double>(kShapesTopHarmonics >> m) > allowed) {
		++m;
	}
	return m;
}

} // namespace feathers
