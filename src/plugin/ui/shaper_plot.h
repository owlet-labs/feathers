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

#include "fx/shaper/shaper_curve.h"
#include "fx/shaper/shaper_types.h"
#include "plugin/ui/shaper_plot_curve.h"
#include "plugin/ui/theme.h"

#include <visage/ui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

// THE TRANSFER CURVE, DRAWN — one drawing, two hosts.
//
// A WAVESHAPER'S CURVE IS THE ONLY HONEST PICTURE OF IT, which is why both surfaces show it and why
// the drawing lives here rather than in either of them. The card draws it small and the panel draws
// it large, and if each had its own copy the two would agree on the day they were written. This
// project has paid for that twice — `owltt_types.h` says so about a threshold, and `waypoint_card`
// about a frequency axis.
//
// AND IT IS DRAWN FROM `buildTable`, THE SAME FUNCTION THE ENGINE BUILDS ITS TABLE WITH, at a coarser
// resolution. Not a second formula that ought to agree: the same one, given a smaller span. That is
// the whole reason `shaper_curve.h` takes a `std::span` rather than a fixed array — a plot is a
// 160-entry table and a voice is a 1025-entry one, and they are the same object.

namespace feathers::ui {

/// DRAW THE CURVE THE GIVEN SETTINGS PRODUCE, into `box`.
///
/// @param dimmed  draw it as an inert shape — the device is off or bypassed. The honesty rule for a
///                picture: a live-looking curve on a device that is not processing is the same lie
///                as an enabled control that reaches nothing.
/// `drive` is the decaying peak INPUT the device is seeing (`Shaper::drivePeak`), or 0 when no host
/// publishes one. It colours the reached part of the curve — see the note at the drawing loop.

inline void drawShaperCurve(visage::Canvas& canvas, const visage::Bounds& box,
                            const fx::shaper::Settings& settings, bool dimmed, float drive = 0.0f) {
	if (box.width() <= 2.0f || box.height() <= 2.0f) {
		return;
	}

	// THE FRAME AND THE TWO AXES. The diagonal is what makes the picture readable at a glance: it is
	// the identity, so how far the curve departs from it IS the amount of shaping, without the viewer
	// having to remember what a straight line would have looked like.
	canvas.setColor(withAlpha(kLine, 0.25f));
	canvas.rectangleBorder(box.x(), box.y(), box.width(), box.height(), 1.0f);
	canvas.setColor(withAlpha(kLine, 0.35f));
	canvas.segment(box.x(), box.y() + box.height() * 0.5f, box.right(),
	               box.y() + box.height() * 0.5f, 1.0f, true);
	canvas.segment(box.x() + box.width() * 0.5f, box.y(), box.x() + box.width() * 0.5f, box.bottom(),
	               1.0f, true);
	canvas.setColor(withAlpha(kLine, 0.22f));
	canvas.segment(box.x(), box.bottom(), box.right(), box.y(), 1.0f, true);

	// **THE DEVICE'S OWN TRANSFER, NOT THE RAW TABLE.** This drew `buildTable(deriveCurve(...))`
	// straight — the table across the whole domain — while the device passes input through UNCHANGED
	// below its threshold and reads the table only on the EXCESS above it. Those were never the same
	// function, and a scope showed it: the displayed transfer disagreed with the output even with every
	// modifier off.
	//
	// **DRAWN THROUGH `staticTransferAt`, THE DEFINITION THE ENGINE ITSELF READS.** A restatement here
	// is what drifted, and the DSP has moved twice today — so a second restatement would drift again.
	// `Shaper` calls `readTableAt`, `stagingFor` and `sweepFor` too, and a case pins the two together.
	//
	// MODIFIERS AND HYSTERESIS ARE ABSENT, AND HONESTLY SO: they are STATE — they depend on where the
	// signal came from, not only where it is — so no static curve can carry them, and drawing one that
	// pretended to would show a shape no single sample follows.
	// **ONE DEFINITION, ASKED FOR RATHER THAN RESTATED.** `shaperPlotCurve` is the value this used to
	// compute here; a restatement is what dropped the travel argument in the first place.
	const std::array<float, kShaperPlotPoints> curve = shaperPlotCurve(settings);

	const bool inert = dimmed || !settings.enabled;

	//---------------------------------------------------------------------------------------------//
	// THE PART OF THE CURVE THE SIGNAL IS ACTUALLY REACHING, IN ITS OWN COLOUR.                    //
	//---------------------------------------------------------------------------------------------//
	//
	// A post-mix level reading on the transfer curve display shows how hard the actual table is driven,
	// by painting the curve a different colour where the level reaches it.
	//
	// **THAT IS A BETTER INSTRUMENT THAN A BAR, and for a reason worth writing down: a meter beside a
	// curve makes the reader do the join.** The question is which part of THIS shape the signal is
	// exercising, and the answer belongs on the shape. Everything chased through backlog BI was
	// invisible from here — that the read reached only the first fifth of the table, that three
	// colours were silent, that the folding sat past where the signal ever went — and every one of
	// them would have been visible at a glance as a curve mostly drawn in the cold colour.
	//
	// `drive` is the decaying peak INPUT (`Shaper::drivePeak`), and the x axis is the input axis, so
	// the live region is simply `|x| <= drive`. It is the post-mix reading the design calls for because the
	// curve already carries the threshold: the stretch from the crossing to the edge of the hot
	// region IS the excess reaching the table, drawn against what the table does to it.
	//
	// **ZERO MEANS "NOBODY IS PUBLISHING", NOT "SILENCE".** A host with no meter road must draw the
	// whole curve in one colour rather than a curve that reads as permanently unreached — the same
	// argument `DeviceHost::playheadAt` makes for declining rather than answering 0.0f, and for the
	// same reason: a plausible picture of a wrong thing is worse than no picture.
	const bool metered = drive > 0.0001f && !inert;
	const auto colourAt = [&](float t) {
		if (!metered) {
			return inert ? withAlpha(kMuted, 0.55f) : withAlpha(kAccent, 0.95f);
		}
		// THE SAME t THE POINT WAS COMPUTED FROM, mapped back to the domain, so the boundary lands
		// where the signal actually stops rather than a pixel away from it.
		const float domain = std::fabs(t * 2.0f - 1.0f);
		return domain <= drive ? withAlpha(kAccent, 0.95f) : withAlpha(kMuted, 0.45f);
	};

	float prevX = box.x();
	float prevY = box.y() + box.height() * 0.5f;
	for (size_t i = 0; i < kShaperPlotPoints; ++i) {
		const float t = static_cast<float>(i) / static_cast<float>(kShaperPlotPoints - 1);
		const float x = box.x() + t * box.width();
		// THE TABLE SPANS [-1, +1] AFTER NORMALISATION, so the vertical map is fixed and the picture
		// does not rescale as the curve changes shape. A plot that renormalised itself would make
		// every position look equally driven.
		const float y = box.y() + (1.0f - (curve[i] + 1.0f) * 0.5f) * box.height();
		if (i > 0) {
			// COLOURED PER SEGMENT rather than in two passes: a segment belongs to the region its far
			// end is in, so the boundary is drawn once and the two runs meet without a gap.
			canvas.setColor(colourAt(t));
			canvas.segment(prevX, prevY, x, y, inert ? 1.2f : 1.8f, true);
		}
		prevX = x;
		prevY = y;
	}
}

} // namespace feathers::ui
