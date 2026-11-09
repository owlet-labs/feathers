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

#include "plugin/ui/theme.h"

#include <visage/graphics.h>

#include <cmath>

// THE GRID BEHIND A MODULATOR'S PLOT, AND THE DIVISIONS A DRAG STICKS TO — one file, because those
// two have to be the same lines.
//
// **THAT IS THE WHOLE REASON THIS IS NOT JUST A DRAW CALL.** The grid and stickiness on the time
// subdivision lines are one feature (2026-09-19), and a snap that quantises to
// eighths while the picture draws sixths is a control that fights the hand for reasons nothing on
// screen explains. The subdivision count is declared once below; `drawGrid` reads it and
// `stickToDivision` reads it, so the two cannot drift apart without the compiler noticing.
//
// It is a free function over `visage::Bounds` rather than a base class: the two cards that want it
// already inherit from `FxCard`, and a plot grid is a thing you DRAW rather than a thing you ARE.

namespace feathers::ui::plot {

/// **THE COARSE DIVISIONS — quarters of the window.** Four is what a musician counts in; the eighths
/// below are the subdivision under it, and anything finer stops reading as structure and starts
/// reading as texture.
inline constexpr int kCoarseDivisions = 4;

/// **THE FINE DIVISIONS — eighths.** Drawn fainter than the quarters and, on the MSEG, the finest
/// thing a drag sticks to. A multiple of `kCoarseDivisions`, which is what lets `drawGrid` tell a
/// quarter from an eighth by asking whether the index divides.
inline constexpr int kFineDivisions = 8;
static_assert(kFineDivisions % kCoarseDivisions == 0,
              "a quarter must also be an eighth, or the two sets of lines disagree about where the "
              "quarters are and the fainter set draws over the brighter one");

/// **HOW FAINT EACH SET IS — AND THESE NUMBERS WERE MEASURED, NOT CHOSEN.**
///
/// `kLine` is `0x2B3452`, whose brightest channel is 82 and whose red is 43. Over this project's
/// near-black plot ground that makes an alpha into a pixel value almost directly: at 0.18 the red
/// channel lands on **8**, and at 0.09 on **4**. Both were verified against an xvfb capture, and both
/// are invisible on a real display — the MSEG's brand-new grid could not be seen at all.
///
/// **THE FIRST DRAFT ANCHORED TO THE WRONG REFERENCE.** It matched the LFO's existing crosshatch at
/// 0.2, reasoning that a shipped value must be visible. It was not: 0.2 of `kLine` is a red of 9, and
/// that is why the LFO's card was asked for gridlines it had already had for weeks. **A control that
/// has always been invisible is not evidence about what visible looks like.**
///
/// ANCHORED NOW TO SOMETHING KNOWN-VISIBLE: the MSEG's zero line, `withAlpha(kLine, 0.55f)`, which
/// has been read off that card since it was written. The quarters sit below it so the zero still
/// wins where both are drawn, and the eighths sit below the quarters.
/// **AND THE SECOND ROUND MISSED TOO, WHICH IS WHY THE WIDTH IS HERE AS WELL.** 0.42 measured at
/// rgb(16,19,32), which was visible, but barely. Nudging the alpha a
/// third time would have been the same mistake twice: **the limiter was never the alpha, it was the
/// LINE WIDTH.**
///
/// The grid drew at 0.5 px. A sub-pixel line is antialiased across two pixels at partial coverage, so
/// it loses about half its ink before the alpha is even consulted — and it can never be a crisp line,
/// only a smear whose brightness depends on where the geometry happens to land. At a 1.75×
/// display scale that is 0.875 physical pixels: a grid line that is a whole pixel NOWHERE.
///
/// **SO THE WIDTH IS ONE PIXEL AND THE ALPHA CARRIES THE FAINTNESS**, which is the right division of
/// labour: a line you can see the edges of, drawn dim, rather than a bright line you can only half
/// see. It is also what the MSEG's zero line has always done — 1.0 px at 0.55 — which is the control
/// that has been legible on that card all along and the reference these were re-anchored to.
inline constexpr float kCoarseAlpha = 0.55f;
inline constexpr float kFineAlpha = 0.30f;

/// ONE PIXEL. See the note above: this was 0.5 and that, rather than the alpha, is what made two
/// rounds of tuning look like a brightness problem.
inline constexpr float kLineWidth = 1.0f;

/// **THE NEAREST DRAWN DIVISION TO `ux`, WHEN ONE IS WITHIN `tolerance` — otherwise `ux` itself.**
///
/// `ux` and `tolerance` are both in UNIT x (0..1 across the plot), so a caller converts a pixel
/// tolerance by dividing by the plot's width. **Pixels rather than a fixed fraction is the point:**
/// a snap radius stated in unit x would grab harder on a narrow card than a wide one, and the card
/// this first lands on changes width with the window.
///
/// STICKY, NOT QUANTISED. Everything outside the radius passes through untouched, so a hand that
/// wants 0.31 can have it; what it cannot easily have is 0.2499 when it meant a quarter. The design is
/// a little stickiness, which is this and not a grid the value is rounded onto.
[[nodiscard]] inline float stickToDivision(float ux, float tolerance) {
	if (!std::isfinite(ux) || !std::isfinite(tolerance) || tolerance <= 0.0f) {
		return ux;
	}
	const float scaled = ux * static_cast<float>(kFineDivisions);
	const float nearest = std::round(scaled) / static_cast<float>(kFineDivisions);
	// THE ENDS COUNT TOO: 0 and 1 are divisions, and a shape's first and last breakpoints are exactly
	// where a hand most wants to land squarely.
	return (std::abs(ux - nearest) <= tolerance) ? nearest : ux;
}

/// **DRAW IT.** One line across the middle of the amplitude, and vertical lines at the quarters with
/// the eighths fainter between them: faint grid lines in both x and y, x at 50% amplitude, y at 1/4
/// of the window and even fainter at 1/8.
///
/// **THE EDGES ARE NOT DRAWN.** `i` runs 1..n-1, so the lines at 0 and 1 are left to the plot's own
/// border; drawing them would double its weight and make the box look heavier than the grid inside it.
inline void drawGrid(visage::Canvas& canvas, const visage::Bounds& box) {
	// THE FINE SET FIRST, so a quarter drawn over an eighth wins rather than the other way round —
	// they land on the same pixels by construction (see the static_assert).
	for (int i = 1; i < kFineDivisions; ++i) {
		if (i % (kFineDivisions / kCoarseDivisions) == 0) {
			continue; // this one is a quarter; it belongs to the brighter pass below
		}
		const float px = box.x() + static_cast<float>(i) / static_cast<float>(kFineDivisions)
		                               * box.width();
		canvas.setColor(withAlpha(kLine, kFineAlpha));
		canvas.segment(px, box.y() + 1.0f, px, box.bottom() - 1.0f, kLineWidth, true);
	}
	for (int i = 1; i < kCoarseDivisions; ++i) {
		const float px = box.x() + static_cast<float>(i) / static_cast<float>(kCoarseDivisions)
		                               * box.width();
		canvas.setColor(withAlpha(kLine, kCoarseAlpha));
		canvas.segment(px, box.y() + 1.0f, px, box.bottom() - 1.0f, kLineWidth, true);
	}

	// **AND ONE HORIZONTAL, AT HALF THE PLOT'S HEIGHT.** Half the AMPLITUDE rather than "where zero
	// is", which is a different line and each card draws its own: a unipolar shape's zero sits on the
	// floor and its half-amplitude sits here, while a bipolar one has them in the same place. Asked
	// for as "50% amplitude", and that is what it is.
	const float py = box.y() + 0.5f * box.height();
	canvas.setColor(withAlpha(kLine, kCoarseAlpha));
	canvas.segment(box.x() + 1.0f, py, box.right() - 1.0f, py, kLineWidth, true);
}

} // namespace feathers::ui::plot
