/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not,
 * see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "device/transfer.h"
#include "plugin/ui/theme.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include <visage/ui.h>

// **A TRANSFER CURVE, PLOTTED — AND DRAGGED** (backlog PY). Two surfaces draw one: the Engine rail's route
// chip (`RouteList`) and the MATH card's Transfer glyph (`MathCard`). Both plot the curve their engine
// evaluates, sampled over the range the source can actually say, and both set a curve's coefficient by a
// vertical drag. This is the one copy of the plot and of the drag's arithmetic, so the two cannot drift
// apart: what differs is only the function each passes in (`device::shapeTransfer` for a route, MATH's own
// `mapCurve` for the device — each the function its own engine runs).

namespace feathers::ui {

/// **HOW MANY SEGMENTS A PLOT `width` WIDE TAKES** — a φ curve two points a pixel: twelve segments would draw a
/// low one as a tent and a high one as a few strokes; at two a pixel the busy ones read as the noise they are.
/// Every other curve is smooth at twelve.
[[nodiscard]] inline int transferPlotSteps(bool phi, float width) {
	return phi ? static_cast<int>(width * 2.0f) : 12;
}

/// **THE POINTS A PLOT STROKES** — `curve` sampled at `steps + 1` inputs evenly over −1..1 (bipolar) or 0..1, with
/// y = 0 on the box's centre line either way, so a rectifier's floor reads as the floor it is. A 1 px margin top
/// and bottom keeps a full-scale stroke inside the box.
template <class Curve>
[[nodiscard]] std::vector<visage::Point> transferPlotPoints(const visage::Bounds& box, bool bipolar, int steps,
                                                            Curve&& curve) {
	const float lo = bipolar ? -1.0f : 0.0f;
	const float half = box.height() * 0.5f - 1.0f;
	const float cy = box.y() + box.height() * 0.5f;
	std::vector<visage::Point> points;
	points.reserve(static_cast<size_t>(steps) + 1);
	for (int i = 0; i <= steps; ++i) {
		const float u = static_cast<float>(i) / static_cast<float>(steps);
		const float x = lo + (1.0f - lo) * u;
		points.push_back({ box.x() + box.width() * u, cy - static_cast<float>(curve(x)) * half });
	}
	return points;
}

/// **STROKE A PLOT** — the zero line, then `points` (from `transferPlotPoints`), dim when `quiet` (Lin: a list of
/// ordinary routes, or a card on its default curve, stays quiet).
inline void strokeTransferPlot(visage::Canvas& canvas, const visage::Bounds& box,
                               const std::vector<visage::Point>& points, bool quiet) {
	canvas.setColor(kDisabled);
	canvas.segment(box.x(), box.y() + box.height() * 0.5f, box.right(), box.y() + box.height() * 0.5f, 0.6f, false);
	canvas.setColor(quiet ? kMuted : kModulation);
	for (size_t i = 1; i < points.size(); ++i) {
		canvas.segment(points[i - 1].x, points[i - 1].y, points[i].x, points[i].y, 1.2f, true);
	}
}

/// **HOW FAR A PRESS MUST MOVE BEFORE IT IS A DRAG** rather than the click that opens the menu.
inline constexpr float kTransferDragThreshold = 3.0f;

/// THE PIXELS A COEFFICIENT STEP TAKES. PHI ONE STEP A PIXEL: 224 steps (seven octaves) in 224 px, a two-octave
/// move a 64 px drag. AN EXPONENT TWO A STEP: its 48 steps (three octaves, 1 to 8) in 96 px, so neither range is a
/// flick past the end.
[[nodiscard]] inline float transferDragPixelsPerStep(device::Transfer t) {
	return device::isPhiTransfer(t) ? 1.0f : 2.0f;
}

/// **THE CURVE A DRAG `pixelsUp` FROM `start` LANDS ON** — `start`'s kind at a moved step, clamped to the kind's
/// range; `start` itself for a curve with no coefficient. UP IS MORE: more periods, a steeper exponent (Log rises
/// faster, Exp starts slower).
[[nodiscard]] inline device::Transfer transferDragTarget(device::Transfer start, float pixelsUp) {
	if (!device::hasCoefficient(start)) {
		return start;
	}
	const int steps = static_cast<int>(std::lround(pixelsUp / transferDragPixelsPerStep(start)));
	return device::transferWithStep(start, device::transferStep(start) + steps);
}

/// "exponent 2.83", "16 per swing" — a curve's coefficient as a readout prints it; "" for none.
[[nodiscard]] inline std::string transferCoefficientText(device::Transfer t) {
	char text[32] = {};
	switch (device::transferKind(t)) {
	case device::Transfer::Log:
	case device::Transfer::Exp:
		// TWO DECIMALS: a step is 4.4% of the exponent, so 2.00 → 2.09 → 2.18 reads as a move.
		std::snprintf(text, sizeof(text), "exponent %.2f", static_cast<double>(device::transferExponent(t)));
		break;
	case device::Transfer::Phi: {
		// WHOLE PERIODS ABOVE TEN, ONE DECIMAL BELOW — a step is 2% of the value, so a tenth is its resolution
		// down there and a whole period is up here.
		const float periods = device::phiPeriods(t);
		std::snprintf(text, sizeof(text), periods < 10.0f ? "%.1f per swing" : "%.0f per swing",
		              static_cast<double>(periods));
		break;
	}
	default: break;
	}
	return text;
}

} // namespace feathers::ui
