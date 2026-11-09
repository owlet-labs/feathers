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

#include "plugin/gui_bridge.h"
#include "plugin/ui/mod_panel.h"
#include "plugin/ui/mod_state.h"
#include "plugin/ui/notify_tray.h"
#include "plugin/ui/tooltip.h"

#include <cstddef>
#include <visage/ui.h>

// THE SOURCES THAT ARE NOT CARDS — eight arrows in the Engine rail, one per sound.
//
// WHAT A WEIGHT IS, because it is the sentence that explains the whole feature: it is not a property
// of a sound, it is a property of WHERE YOU ARE STANDING relative to one. Edit a sound's identity all
// day — its zone, its offset, its drift — and its weight does not move. Move the puck an inch and
// every weight moves. That is why these live neither in Perform (where the standing point is) nor in
// Explore (where the identity is) but in the Engine rail, which rides both: the decision, and it
// dissolves the question rather than answering it.
//
// AND THAT DECISION KEEPS EXPLORE'S OWN INVARIANT INTACT, which is worth stating because it was the
// argument against the other placement. Explore promises that nothing it draws is a function of a
// performance parameter, so a host automating the puck cannot repaint it (explore_stage.h). These
// arrows are STATIC AFFORDANCES — they say a source exists and whether it may be pointed, and nothing
// here reads a weight VALUE — so no part of this repaints at modulation rate and the chart stays a
// picture of the patch. No meters, by design, and the decision and the invariant agree.
//
// EIGHT ALWAYS, TWO ROWS OF FOUR, IN SLOT ORDER — the chosen shape. The grid position IS the identity,
// reinforced by the sound's own seed hue so an arrow and its star on the circle wear the same colour.
// A sound that is switched off keeps its place and greys out rather than vanishing: a grid that
// resequenced itself as sounds came and went would make the position mean nothing, and "switched off"
// is a thing worth being able to see.
//
// A GREYED ARROW REFUSES ARMING, which is the honesty rule in its usual form — a disabled sound takes
// no place in the blend, so its weight is absent, so there is nothing to route FROM. Pointing it would
// arm a source that reaches nowhere.
//
// THE GROWTH SEAM, named rather than built (voice-stack modulators are planned to go here later).
// This is a grid of FAMILIES of sources and it currently has one. A second family is:
// a heading row and a run of cells appended below `familyBottom()`, its own hit range in `hitAt`, and
// its own naming in modSourceName — the layout below is written against a family's origin rather than
// against the frame's, so the second one does not move the first. What it must NOT become is a
// general widget parameterised over families that has only ever had one; the seam is here so the day
// it is needed is a day of adding, not of rewriting.
//
// **AND THE DAY CAME (backlog KE step 3): the Stack Index was the second family** — one full-width cell below the
// sounds. **AND IT LEFT (backlog MO: "stack index mod to move by the stack controls"):** it is
// `StackIndexArrow` below, on the Engine rail's STACK page under the Shape, Mapping and Curve that make it, so the
// source reads as part of the stack rather than as a ninth sound. The grid is one family again.

namespace feathers::ui {

class SourceGrid : public visage::Frame, public TooltipSource {
public:
	SourceGrid(ParamBridge& bridge, ModState& mod, Notifier* notifier = nullptr);

	void draw(visage::Canvas& canvas) override;

	/// **WHAT EACH ARROW IS** (voice stacking step 5): a sound's weight, or the Stack Index — the cells have no control of
	/// their own to declare it, so the grid says it.
	const char* tooltipAt(visage::Point local) const override;
	/// The modulation sources' grid: routes, not a device's controls.
	void offeredControls(std::vector<Offered>&) const override {}

	/// How tall this wants to be, so the Engine rail can place it without knowing what a cell is.
	float preferredHeight() const;

	/// The chosen shape: two rows of four, and the sounds fill it exactly.
	static constexpr size_t kColumns = 4;
	static constexpr size_t kRows = kMaxSounds / kColumns;
	static_assert(kColumns * kRows == kMaxSounds, "the grid holds every sound and no empty cells");

	/// SIZED TO WHAT THE RAIL HAS LEFT, which is a constraint rather than a taste. The Engine rail does
	/// not scroll — unlike the modulator strip beside it — so a section added here is spending a fixed
	/// budget, and the first draft of this grid spent 20px more than there was: the routings list below
	/// went off the bottom of the rail, where its rows were still laid out and still hit-testable by
	/// arithmetic but unreachable by a hand. Four cases failed and not one of them was about the grid.
	/// The row height is the route list's own, so the two sections read as one column.
	static constexpr float kHeadingHeight = 12.0f;
	static constexpr float kCellHeight = 18.0f;
	static constexpr float kCellGap = 2.0f;
	/// The arrow glyph's reach, the modulator strip's own — these are the same mark as a card's arrow
	/// and have to read as the same verb.
	static constexpr float kArrowSize = 11.0f;

	/// Where the arrow for `slot` sits. Public so a test can ask rather than aim.
	visage::Bounds cellBounds(size_t slot) const;

	enum class Part { None, Arrow };

	struct Hit {
		Part part = Part::None;
		size_t slot = 0;
	};

	Hit hitAt(visage::Point pixels) const;

	/// Whether the sound in `slot` is switched on. A greyed arrow is one whose sound is off, and it
	/// refuses to arm — read from the parameter rather than from any state of our own, so the grid and
	/// the rail's switch cannot disagree.
	bool slotEnabled(size_t slot) const;

	/// Whether this slot's arrow is the armed one. Public for the same reason the geometry is: a test
	/// should be able to ask what the surface believes rather than read its pixels.
	bool slotArmed(size_t slot) const;

private:
	/// **THE SUBSCRIPTION, HELD** — see `ui::Watch`. Declared LAST so it is destroyed FIRST, which
	/// releases into a list that is still alive.
	Watch watch_;
	/// Focus this slot's source and arm it, exactly as a card's arrow does for a macro.
	void focusOrArm(const Hit& hit);

	ParamBridge& bridge_;
	ModState& mod_;
	Notifier* notifier_ = nullptr;
};

/// **THE STACK INDEX'S ARROW** (backlog KE step 3; moved by backlog MO): one cell, named in words, with a sound arrow's
/// mark, border grammar and verb. It lives on the Engine rail's STACK page, the full width of the rail under the Shape,
/// Mapping and Curve that make the index, so the source sits with its own controls.
class StackIndexArrow : public visage::Frame, public TooltipSource {
public:
	StackIndexArrow(ParamBridge& bridge, ModState& mod, Notifier* notifier = nullptr);

	void draw(visage::Canvas& canvas) override;
	const char* tooltipAt(visage::Point local) const override;
	/// A modulation source, not a device control.
	void offeredControls(std::vector<Offered>&) const override {}

	/// A sound arrow's height, so the two read as the same mark.
	static constexpr float kHeight = SourceGrid::kCellHeight;

	/// Whether the Stack Index can be heard: a stack of one has one part, at index 0, so a route from it moves
	/// nothing and the cell greys and refuses to arm, as a switched-off sound's does.
	bool stackEnabled() const;
	bool stackArmed() const;

private:
	Watch watch_;
	void focusOrArm();

	ParamBridge& bridge_;
	ModState& mod_;
	Notifier* notifier_ = nullptr;
};

} // namespace feathers::ui
