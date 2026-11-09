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

#include "plugin/ui/rail.h"

#include "plugin/ui/theme.h"

namespace feathers::ui {

bool Rail::setCollapsed(bool collapsed) {
	if (collapsed_ == collapsed) {
		return false;
	}
	collapsed_ = collapsed;
	// The panel hides its own contents first, then the frame is re-laid-out at its new width by
	// whoever owns the columns — a rail does not resize itself, because its width is one term in an
	// arithmetic that belongs to the editor.
	collapsedChanged();
	// The editor's columns are a sum over these widths, so the sum has to be taken again before any of
	// this is on screen. Announced rather than acted on: a rail that set its own bounds would be a
	// second place deciding where the rails end, and the two would drift.
	if (widthChanged_) {
		widthChanged_();
	}
	redraw();
	return true;
}

void Rail::collapsedChanged() {
	for (visage::Frame* child : children()) {
		child->setVisible(!collapsed());
	}
	// Any scroll a panel is holding is left exactly where it was: reopening a rail is not a
	// navigation, and content nobody can see cannot be scrolled to somewhere wrong.
}

visage::Bounds Rail::toggleBounds() const {
	// TOP OF THE RAIL, INBOARD EDGE, in both states — the mock puts the button first in the head row
	// and first in the head COLUMN once the rail is closed, which is the same corner either way.
	// Inboard, so the button on a right-hand rail sits against the stage rather than against the
	// window edge, and the two rails' buttons frame the instrument instead of hugging the frame.
	const float inset = collapsed() ? kCollapsedPadX : kCollapsedPadX * 2.0f;
	const float x = side_ == Side::Left ? width() - kToggleSize - inset : inset;
	return { x, kPadTop * 0.5f, kToggleSize, kToggleSize };
}

void Rail::drawRailChrome(visage::Canvas& canvas) {
	const visage::Bounds toggle = toggleBounds();

	// THE GLYPH IS GEOMETRY, not a character. A16's lesson, and the arrow on the modulator cards
	// already follows it: a mark drawn from points is the same mark in every font the atlas happens
	// to have, while `◂` is one missing glyph away from being an empty box in the one control the
	// user needs to get a closed rail back.
	//
	// A DOUBLE CHEVRON, AND NOT THE MOCK'S SOLID TRIANGLE. On the first render the expand/compress
	// glyph was overloaded with the modulators' and solo's. It is a counting
	// problem, not a taste one — this editor already spends a filled triangle on the modulator card's
	// arrow and another on a sound's solo, so a third made three different verbs look like one
	// family. Two open strokes read as a different KIND of mark: no fill, no single arrowhead, and
	// the doubling is the widespread "move this whole panel aside" idiom rather than "go there".
	//
	// Solo's own collision is settled separately, by the decision that it becomes an S in a box.
	// **THERE IS A SECOND COPY OF THIS MARK IN THE FLOOR — `theme.h`'s `drawChevronPair` — AND THIS
	// LOOP IS DELIBERATELY NOT IT.** It was lifted there on 2026-09-19 for a fold-down section class
	// that collapsed on the other axis; that class is gone (SKLITTER's sections are rails now, see
	// `sklitter_expanded.h`) and the floor's copy currently has no caller. The rail was never moved
	// onto it, because the two anchor differently and the difference is visible:
	//
	//   - **THIS LOOP ANCHORS BY SIDE AND FLIPS ONLY THE ARROW.** `front` is `toggle.x() + step` on a
	//     LEFT rail and `toggle.right() - 2·step` on a right one, whichever way the chevrons point. So
	//     a rail toggling reverses the mark in place and leaves its anchor where it was.
	//   - **THE FLOOR'S VERSION ANCHORS BY DIRECTION**, which is the symmetric rule and the only one a
	//     caller with no "side" can state.
	//   - At `kToggleSize` 20 and `step` 4 those two differ by **8 pixels inside a 20-pixel button** —
	//     DERIVED here and it is why no capture was taken to settle it: the answer is not close.
	//
	// **WHICH OF THE TWO IS RIGHT IS A QUESTION FOR THE EYE.** Restyling the fx lane's control as a
	// side effect of another arc is an unrequested visual change to a surface nobody asked about, and
	// this editor's own rule is that a render is believed only when it has been looked at. So the
	// duplication is kept, the reason is here, and unifying them is a deliberate change with a capture
	// behind it.
	const bool left = side_ == Side::Left;
	// Points the way the panel will travel: outboard to close it, inboard to bring it back.
	const bool pointsLeft = left == !collapsed();
	const float mid = toggle.y() + toggle.height() * 0.5f;
	const float half = toggle.height() * 0.22f;
	const float step = toggle.width() * 0.20f;
	const float front = left ? toggle.x() + step : toggle.right() - step * 2.0f;

	canvas.setColor(kMuted);
	for (int chevron = 0; chevron < 2; ++chevron) {
		const float tipX = front + static_cast<float>(chevron) * step * (pointsLeft ? 1.0f : -1.0f)
		                   + (pointsLeft ? 0.0f : step);
		const float tailX = pointsLeft ? tipX + step : tipX - step;
		canvas.segment(tipX, mid, tailX, mid - half, kChevronWeight, true);
		canvas.segment(tipX, mid, tailX, mid + half, kChevronWeight, true);
	}

	if (!collapsed() || !uprightLabel_) {
		return;
	}
	// THE LABEL, UPRIGHT, which is the whole of what a closed rail says. visage rotates text for us
	// (Direction), so this is the mock's `writing-mode:vertical-rl` and not a column of single
	// letters — the trick that would have been needed without it reads as a ransom note.
	//
	// AT THE TOP AND AT READING SIZE, both of which the first render got wrong: the text sat at the
	// bottom of the collapsed panel instead of the top, and about 2.5-3x too small. Bottom-to-top is
	// the direction vertical labels read in, so the text
	// keeps its direction and is justified to the far end of its own rotated frame, which is the top
	// of the rail; and the size is the rail's own rather than the knob label's, because a 34px strip
	// has one thing to say and the whole width to say it in.
	const float top = toggle.bottom() + kPadTop;
	canvas.setColor(kMuted);
	canvas.text(railLabel(), font(kCollapsedLabelSize), visage::Font::kRight, 0.0f, top, width(),
	            height() - top, visage::Direction::Left);
}

} // namespace feathers::ui
