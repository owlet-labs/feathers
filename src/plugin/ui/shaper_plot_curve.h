/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "fx/shaper/shaper_curve.h"
#include "fx/shaper/shaper_types.h"

#include <array>
#include <cstddef>

// THE PLOT'S CURVE, WITHOUT THE PLOT.
//
// **THIS HEADER EXISTS SO THE PICTURE CAN BE TESTED BESIDE THE ENGINE.** `shaper_plot.h` includes
// visage, and `test_fx_shaper` links `feathers-fx` and nothing else — which is deliberate, it is how
// the device boundary is enforced — so the suite that owns the engine could not ask what the plot
// draws. The curve itself needs no visage at all: it is `curveFor`, `buildTable` and
// `staticTransferAt`, every one of them already in `feathers-fx`.
//
// That gap is not incidental to the bug this was extracted for. The plot drew the no-travel curve
// while the engine passed the travel, and the case that would have caught it — "the drawn transfer IS
// the device's" — lives in the suite that could not see the drawing code. The oracle and the subject
// were in different binaries.

namespace feathers::ui {

/// HOW MANY POINTS A PLOTTED CURVE IS SAMPLED AT. Enough that the Chebyshev folder's fine structure
/// survives — at 64 the folds alias into a picture of something else, which is a plot that lies.
inline constexpr size_t kShaperPlotPoints = 192;

/// **THE CURVE THE PLOT DRAWS, AS A VALUE.**
///
/// Split out of `drawShaperCurve` so the shape can be ASSERTED rather than inferred from pixels. It
/// exists because of a bug no picture comparison could have caught: `curveFor` takes `travel` with a
/// default of `nullptr`, the draw omitted it, and the plot drew the no-travel curve while the engine
/// passed `target_.travel.data()` to the same function. The pad reached the sound and never reached
/// the picture, and the only honest test of that is to read the numbers the plot would draw.
///
/// One definition, called by the draw and by the cases, so the two cannot come to disagree about what
/// is on screen — the same reason the draw goes through `staticTransferAt` rather than restating the
/// engine's read.
///
/// MODIFIERS AND HYSTERESIS ARE ABSENT, AND HONESTLY SO: they are STATE — they depend on where the
/// signal came from, not only where it is — so no static curve can carry them.
[[nodiscard]] inline std::array<float, kShaperPlotPoints>
shaperPlotCurve(const fx::shaper::Settings& settings) {

	std::array<float, kShaperPlotPoints> table {};
	std::array<float, kShaperPlotPoints> curve {};
	const bool linear = settings.linear();
	fx::shaper::TableBound bound;
	fx::shaper::Staging staging;
	float sweep = 1.0f;
	fx::shaper::ReadShapes shapes;
	if (!linear) {
		const fx::shaper::CurveParams params = fx::shaper::curveOf(settings);
		shapes = fx::shaper::readShapesOf(params); // Stair and Cheby, read-side since backlog IM
		fx::shaper::buildTable(params,
		                       table);
		bound = fx::shaper::boundTable(table);
		staging = fx::shaper::stagingFor(settings.threshold, settings.aggression);
		sweep = fx::shaper::sweepFor(settings.drive);
	}
	for (size_t i = 0; i < kShaperPlotPoints; ++i) {
		const float x =
		    static_cast<float>(i) * (2.0f / static_cast<float>(kShaperPlotPoints - 1)) - 1.0f;
		// AND THE CROSSFADE MIX (backlog FA) by the engine's own law: the curve drawn is the curve heard. The input
		// filters are not here, for the reason the modifiers are not — they depend on frequency, not on `x`.
		curve[i] = linear ? x
		                  : fx::shaper::dryWetMix(x,
		                                          fx::shaper::staticTransferAt(table, bound, staging, sweep,
		                                                                       fx::shaper::kneeFadeFor(settings.knee),
		                                                                       x, shapes),
		                                          settings.dryWet);
	}
	return curve;
}

} // namespace feathers::ui
