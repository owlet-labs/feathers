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

// **THE HEAD SWEEP'S ONE QUESTION, ASKED OF ONE CARD** — shared by every lane that holds cards, so the
// fx lane, SKLITTER's inner lanes, the modulator strip and the rack editor are held to the same rule
// by the same lines, after text collided with widgets in the headers.
//
// The rule: every occupant of a card's head — each mark, and each piece of text AS WIDE AS THE FONT
// MAKES IT — lies inside the head, no two overlap, and each piece of text fits the box it was given.
// The text is MEASURED HERE with the font the head draws it in, rather than taken from the layout's
// own box, because "the box fits" and "the words fit the box" are the two halves of the defect and a
// box that is right while the words overrun it is exactly what a long device name produced.

#include "plugin/ui/fx_card.h"
#include "plugin/ui/theme.h"

#include "doctest.h"

#include <string>
#include <vector>

namespace feathers::test {

struct HeadInk {
	std::string what;
	visage::Bounds box;
};

/// WIDTH OF ASCII TEXT, measured straight through visage — independent of `ui::textWidth`, which is
/// what the layout itself uses. Every device name and caption in the head is ASCII.
inline float asciiWidth(const std::string& text, float size) {
	return ui::font(size).stringWidth(std::u32string(text.begin(), text.end()));
}

/// **WHAT THE HEAD ACTUALLY INKS**, as rectangles in the card's own coordinates: the marks as laid
/// out, the name and caption at their MEASURED extents (left-aligned name on a top head; on a spine
/// the name is right-justified in its rotated axis, which puts its ink at the TOP of the band; the
/// caption is right-aligned).
inline std::vector<HeadInk> headInk(const ui::FxCard& card) {
	const ui::FxCard::HeadLayout head = card.headLayout();
	std::vector<HeadInk> out;
	for (const ui::FxCard::HeadPart& m : head.marks) {
		out.push_back({ m.what, m.box });
	}
	const bool spine = card.headEdge() == ui::CardHeadEdge::Left;
	if (!head.name.empty()) {
		const float w = asciiWidth(head.name, head.nameSize);
		out.push_back({ "name '" + head.name + "'",
		                spine ? visage::Bounds { head.nameBox.x(), head.nameBox.y(), head.nameBox.width(), w }
		                      : visage::Bounds { head.nameBox.x(), head.nameBox.y(), w, head.nameBox.height() } });
	}
	if (!head.caption.empty()) {
		const float w = asciiWidth(head.caption, ui::FxCard::HeadLayout::kCaptionSize);
		out.push_back({ "caption '" + head.caption + "'",
		                { head.captionBox.right() - w, head.captionBox.y(), w, head.captionBox.height() } });
	}
	return out;
}

inline bool overlaps(visage::Bounds a, visage::Bounds b) {
	return a.x() < b.right() && b.x() < a.right() && a.y() < b.bottom() && b.y() < a.bottom();
}

inline bool inside(visage::Bounds inner, visage::Bounds outer) {
	constexpr float kSlack = 0.01f;
	return inner.x() >= outer.x() - kSlack && inner.y() >= outer.y() - kSlack
	       && inner.right() <= outer.right() + kSlack && inner.bottom() <= outer.bottom() + kSlack;
}

inline std::string describe(visage::Bounds b) {
	return "[" + std::to_string(b.x()) + "," + std::to_string(b.y()) + " " + std::to_string(b.width())
	       + "x" + std::to_string(b.height()) + "]";
}

/// THE ROW OF THE REPORT'S TABLE for one card in one state: every occupant and its rectangle.
inline std::string headTable(const ui::FxCard& card) {
	std::string row;
	for (const HeadInk& ink : headInk(card)) {
		row += "  " + ink.what + " " + describe(ink.box) + "\n";
	}
	return row;
}

/// **THE ASSERTION.** `where` names the lane, device and state, so a red says which head.
inline void checkHead(const ui::FxCard& card, const std::string& where) {
	CAPTURE(where);
	const ui::FxCard::HeadLayout head = card.headLayout();
	const std::vector<HeadInk> ink = headInk(card);
	const std::string table = headTable(card);
	CAPTURE(table);
	CHECK_MESSAGE(!head.name.empty(), "the head lost its name altogether");

	// THE TEXT FITS THE BOX IT WAS GIVEN — measured, not the layout's own belief.
	const bool spine = card.headEdge() == ui::CardHeadEdge::Left;
	if (!head.name.empty()) {
		const float room = spine ? head.nameBox.height() : head.nameBox.width();
		CHECK_MESSAGE(asciiWidth(head.name, head.nameSize) <= room + 0.01f,
		              "the name '" << head.name << "' is wider than its box");
	}
	if (!head.caption.empty()) {
		CHECK_MESSAGE(asciiWidth(head.caption, ui::FxCard::HeadLayout::kCaptionSize)
		                  <= head.captionBox.width() + 0.01f,
		              "the caption '" << head.caption << "' is wider than its box");
	}

	for (size_t i = 0; i < ink.size(); ++i) {
		CHECK_MESSAGE(inside(ink[i].box, head.head),
		              ink[i].what << " " << describe(ink[i].box) << " is outside the head "
		                          << describe(head.head));
		for (size_t j = i + 1; j < ink.size(); ++j) {
			CHECK_MESSAGE(!overlaps(ink[i].box, ink[j].box),
			              ink[i].what << " " << describe(ink[i].box) << " overlaps " << ink[j].what << " "
			                          << describe(ink[j].box));
		}
	}

	// AND NO CHILD FRAME LIES OVER THE HEAD — a knob or a graph placed a few pixels high would cover
	// the marks as surely as text does, and it is a frame, so no rectangle above would name it.
	for (const visage::Frame* child : card.children()) {
		if (!child->isVisible() || child->width() <= 0.0f || child->height() <= 0.0f) {
			continue;
		}
		const visage::Bounds b { child->x(), child->y(), child->width(), child->height() };
		CHECK_MESSAGE(!overlaps(b, head.head),
		              "a child frame " << describe(b) << " lies over the head " << describe(head.head));
	}
}

} // namespace feathers::test
