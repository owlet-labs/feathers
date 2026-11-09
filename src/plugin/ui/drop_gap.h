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

#include <cstddef>
#include <optional>
#include <span>

// WHERE A DRAGGED ROW WOULD LAND — AS A GAP, NOT AS A ROW.
//
// ON THE SOUND LIST, AND THE MOD AND EFFECT LISTS ALIKE: the drop indicator highlights the gap
// ("bumper") before the row where the item will actually land, not a row.
//
// THAT IS A CHANGE OF MODEL, NOT A NUDGE. The first implementation answered with the ROW you would
// land on, which differs from where the thing actually goes by one at every position — and is
// ambiguous at both ends, because a row index cannot say "before the first" or "after the last" at
// all. Those are exactly the two places a reorder is least obvious and most worth showing.
//
// SO: N ROWS HAVE N+1 GAPS. Gap 0 is above the first row, gap N is below the last, and gap g sits
// between rows g-1 and g. A drop indicator drawn at a gap can express every outcome a drag has,
// which the row version could not.
//
// THIS FILE IS THE ARITHMETIC AND NOTHING ELSE. Three lists want it at once — the sound rail, the
// modulator strip and the fx lane — which makes a shared helper COPY ONE rather than copy three; the
// slot menu is left duplicated, deliberately. What is NOT here is the drawing: a
// bumper is a different shape in a rail of cards than in a lane of them, and each list already knows
// its own geometry.

namespace feathers::ui {

/// One row's extent ALONG THE FLOW AXIS, in whatever coordinates the caller is asking about.
///
/// **ALONG, NOT VERTICAL.** Three callers stack downward and one — the rack's horizontal lane — stacks
/// sideways, and the arithmetic is identical: a span is a pair of scalars on the axis the list flows
/// in. The fields kept their names because `top`/`bottom` read correctly for the three vertical
/// callers and a rename would have made every one of them worse to serve the fourth. The caller passes
/// these because each list computes them differently — a sound slot's height includes its stance
/// rows, an fx card's depends on whether it is folded, and both may be scrolled.
struct RowSpan {
	float top = 0.0f;
	float bottom = 0.0f;
};

/// WHICH GAP A POINTER AT `along` IS ASKING FOR, 0..rows.size() inclusive.
///
/// **THIS WAS `gapAtY` AND THE NAME WAS THE ONLY THING VERTICAL ABOUT IT.** The rack's lane flows
/// horizontally and needs exactly this function with an x — so the choice was a second name for one
/// piece of arithmetic, or one honest name. Two names for one rule is how the seven-copy defects in
/// this UI started.
///
/// BY THE ROW'S MIDPOINT, which is what makes the answer feel like a bumper rather than a row: above
/// the middle of a row means "before this one", below it means "after". A rule that used the whole
/// row would make the top and bottom halves of one card mean the same thing, and the hand would have
/// to aim at the 1px seam between cards to say anything about the gap there.
inline size_t gapAlong(float along, std::span<const RowSpan> rows) {
	for (size_t row = 0; row < rows.size(); ++row) {
		const float middle = (rows[row].top + rows[row].bottom) * 0.5f;
		if (along < middle) {
			return row;
		}
	}
	// PAST EVERY MIDPOINT IS THE LAST GAP, which is how "after the last row" is expressible at all.
	return rows.size();
}

/// WHERE THE ROW AT `from` ENDS UP IF IT IS DROPPED IN `gap` — or nullopt when the drop changes
/// nothing.
///
/// TWO GAPS ARE A NO-OP FOR ANY ROW, AND THAT IS THE OFF-BY-ONE THIS FUNCTION EXISTS TO GET RIGHT.
/// The gap ABOVE the dragged row and the gap BELOW it are both the place it already is, so a drop in
/// either must move nothing — not "move by zero", which would still spend an undo step and still
/// announce itself. A caller that treated them as a move would produce a history full of edits that
/// changed nothing.
///
/// AND THE ANSWER IS A FINAL INDEX, which is what both existing move roads already take:
/// SoundStack::reorder splices by erasing then inserting, and moveFxDevice/moveRackDevice shift the
/// span between the two ends — different code, same meaning, verified against both. A gap BELOW the
/// dragged row therefore answers `gap - 1`, because removing the row first shifts everything above it
/// down by one.
constexpr std::optional<size_t> rowForGap(size_t from, size_t gap, size_t rowCount) {
	if (rowCount == 0 || from >= rowCount || gap > rowCount) {
		return std::nullopt;
	}
	if (gap == from || gap == from + 1) {
		return std::nullopt; // the place it already is
	}
	return gap < from ? gap : gap - 1;
}

} // namespace feathers::ui
