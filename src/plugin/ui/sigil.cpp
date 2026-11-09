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
#include "plugin/ui/sigil.h"

#include "plugin/ui/theme.h"

#include <cmath>

namespace feathers::ui {

namespace {

/// SPLITMIX64 — a well-mixed hash, so neighbouring laps get unrelated marks.
[[nodiscard]] uint64_t mix(uint64_t x) {
	x += 0x9E3779B97F4A7C15ull;
	x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
	x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
	return x ^ (x >> 31);
}

/// THE EDGES, grid points numbered row-major 0..8: six horizontal, six vertical, then each cell's two diagonals.
constexpr std::array<std::array<int, 2>, Sigil::kEdges> kEdgeList { {
	{ 0, 1 }, { 1, 2 }, { 3, 4 }, { 4, 5 }, { 6, 7 }, { 7, 8 },
	{ 0, 3 }, { 3, 6 }, { 1, 4 }, { 4, 7 }, { 2, 5 }, { 5, 8 },
	{ 0, 4 }, { 1, 3 }, { 1, 5 }, { 2, 4 }, { 3, 7 }, { 4, 6 }, { 4, 8 }, { 5, 7 },
} };

} // namespace

Sigil::Sigil(device::Modulatable& control, double perLap) : control_(control), perLap_(perLap > 0.0 ? perLap : 1.0) {
	setIgnoresMouseEvents(true, false);
}

void Sigil::setEnabled(bool on) {
	enabled_ = on;
	redraw();
}

std::array<int, 2> Sigil::endpointsOf(int e) {
	return kEdgeList[static_cast<size_t>(e)];
}

uint32_t Sigil::strokesFor(int64_t lap) {
	uint64_t h = mix(static_cast<uint64_t>(lap));
	const int want = 4 + static_cast<int>(h % 3); // four to six strokes
	uint32_t mask = 0;
	int have = 0;
	while (have < want) {
		h = mix(h);
		const uint32_t bit = 1u << (h % kEdges);
		if ((mask & bit) == 0) {
			mask |= bit;
			++have;
		}
	}
	return mask;
}

int64_t Sigil::lap() const {
	const double v = control_.value();
	return std::isfinite(v) ? static_cast<int64_t>(std::floor(v / perLap_)) : 0;
}

void Sigil::draw(visage::Canvas& canvas) {
	const float side = std::min({ kSide, static_cast<float>(width()), static_cast<float>(height()) });
	if (side <= 2.0f) {
		return;
	}
	// CENTRED IN THE FRAME, the grid inset so a stroke's end is not clipped.
	const float x0 = (static_cast<float>(width()) - side) * 0.5f;
	const float y0 = (static_cast<float>(height()) - side) * 0.5f;
	const float inset = 1.5f;
	const float step = (side - 2.0f * inset) * 0.5f;
	const auto point = [&](int p) {
		return visage::Point { x0 + inset + static_cast<float>(p % 3) * step, y0 + inset + static_cast<float>(p / 3) * step };
	};
	// THE DOTS, FAINT — the grid the strokes are drawn on, so an empty-looking lap still reads as a mark.
	canvas.setColor(withAlpha(kLine, enabled_ ? 0.45f : 0.2f));
	for (int p = 0; p < 9; ++p) {
		const visage::Point at = point(p);
		canvas.circle(at.x - 0.75f, at.y - 0.75f, 1.5f);
	}
	canvas.setColor(enabled_ ? kInk : kDisabled);
	const uint32_t mask = strokesFor(lap());
	for (int e = 0; e < kEdges; ++e) {
		if ((mask & (1u << e)) == 0) {
			continue;
		}
		const visage::Point a = point(kEdgeList[static_cast<size_t>(e)][0]);
		const visage::Point b = point(kEdgeList[static_cast<size_t>(e)][1]);
		canvas.segment(a.x, a.y, b.x, b.y, 1.5f, true);
	}
}

} // namespace feathers::ui
