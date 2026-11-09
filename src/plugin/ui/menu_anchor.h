/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <visage/ui.h>

#include <string>

// THE ONE ROAD EVERY DROPDOWN IN THIS EDITOR OPENS BY — placement and the re-click toggle, once.
//
// Two defects: in some places (OWLTT among them) a dropdown did not appear next to its box, and
// re-clicking the box did not close the menu — it stayed open until a click anywhere else.
//
// **THE PLACEMENT FAULT IS visage's FLIP, NOT ANY CALL SITE'S ARITHMETIC.** `PopupMenuFrame::show`
// converts the anchor into window coordinates correctly and then, when the menu would run off the
// bottom, flips it with `top = point.y` — the anchor's LOCAL y, not its window y — so a menu opened
// from a box in the lower part of a window lands near the window's TOP, however far that is from the
// box. Horizontally it flips by `x - w` from the anchor's LEFT edge, so an overflowing menu is thrown
// a whole menu-width left of the box instead of being aligned to it. Both bite hardest in a small
// window — OWLTT's panel popped out is exactly that — and neither shows in a big one with the box
// near the top. `FxLane` and `ChainColumn` had each worked around it by estimating the menu's height
// and pre-flipping; every other site was exposed.
//
// **SO THE MENU IS SHOWN, AND THEN PUT WHERE IT BELONGS**, using the size visage itself computed for
// it: below the anchor if it fits, above it if that fits — its left edge on the anchor's, or its right
// edge on the anchor's right when the window is too narrow — and BESIDE the anchor when neither does,
// which in a popped-out panel is the ordinary case rather than an edge one.
// Done here rather than in the fork because the fix belongs to every menu in this editor today and a
// pin bump is a separate, coordinated act; the fork patch is the natural follow-up.
//
// **A SUBMENU NEVER PASSES THROUGH HERE** — visage opens it after the hover pause, from its own
// `PopupMenuFrame::subMenuSelected` — so its placement is the FORK's (owlet-labs/visage, 2026-10-06:
// a context menu running off the bottom clipped and could not be reached): capped to the
// window and scrolling when taller, hung off its row as drawn in a scrolled list, and kept inside the
// window on both axes. Upstream left a submenu taller than the window at a negative y, unscrollable.
//
// **AND A PRESS ON THE BOX THAT OWNS THE OPEN MENU CLOSES IT.** A `PopupMenuFrame` ignores mouse
// events, so a click on its box reaches the box — whose press opened a second menu over the first,
// which is "it stays open". visage takes the keyboard focus away from the popup BEFORE it delivers the
// press (`WindowEventHandler::handleMouseDown`), and the popup fades only on its next PAINT, so at the
// press a menu that was open until this click is still here and still fully opaque, while one closed
// by an earlier click elsewhere has faded at least one frame. That — plus a pick or a cancel, which
// this marks as they happen — is what "the menu is open" means below.

namespace feathers::ui {

/// OPEN `menu` AGAINST `anchor` — a rectangle in `source`'s own coordinates, usually the box that was
/// pressed; a zero-size rectangle for a menu that opens at a point. Returns false, having CLOSED the
/// menu instead, when the same source and anchor already have one open (the re-click toggle). A menu
/// opened from INSIDE another's pick — "Add before…", a Delete row's confirmation — is not a re-click:
/// the picked-from menu counts as closed from the moment its pick is delivered, so this replaces it.
bool showMenuAt(visage::PopupMenu menu, visage::Frame& source, const visage::Bounds& anchor);

/// WHERE THE LAST MENU WENT, in the coordinates of the frame it was parented on — a claim about
/// placement has to be checkable as geometry, and a popup is not reachable from its call site.
struct MenuPlacement {
	visage::Bounds anchor;   ///< the anchor, converted into the popup's frame
	visage::Bounds list;     ///< the menu's rows as placed
	float windowWidth = 0.0f;
	float windowHeight = 0.0f;
	bool shown = false;      ///< false after a re-click closed rather than opened
	/// THE CURRENT CHOICE'S ROW (the first top-level row flagged `select(true)`), or -1 for a menu with none.
	int current = -1;
	/// HOW FAR THE LIST WAS SCROLLED TO BRING `current` TO ITS MIDDLE, in the list's logical pixels; 0 for a
	/// list that shows every row (nothing to scroll) or has no current row.
	float scroll = 0.0f;
};

//------------------------------------------------------------------------------------------------//
// THE CURRENT CHOICE: MARKED, AND OPENED CENTRED ON                                              //
//------------------------------------------------------------------------------------------------//
//
// In HARM and other places with long option lists, the menu reopens centred on the current option,
// with an "x" next to the current choice.
//
// **ONE FACT, TWO EFFECTS.** A choice list adds its rows through `addChoiceRow`, which writes the mark
// into the label AND flags the row `select(true)`; `showMenuAt` reads that flag and, when the list is
// taller than the room it is given (so it scrolls), scrolls it so the flagged row sits in the middle of
// what is shown. The mark and the centring cannot disagree about which row is current, because neither
// works it out again.
//
// **SCROLLED, NOT MOVED OVER THE BOX.** The macOS habit — the list placed so the current row lies on the
// box — was the other way to do it, and it breaks this editor's standing rule that a menu hangs off its
// box and never covers it (`menu_placement_check.h`). Scrolling keeps the placement and
// still opens on the current row. A list that shows every row is not scrolled: its current row is
// already in view, and the mark is the whole change for it.
//
// **`select` IS READ HERE AND BY THE MACOS MENU BAR, AND BY NOTHING visage DRAWS** — which is why the
// mark is in the label rather than left to the flag (`ValueBox::choiceLabelAt` found that first).

/// THE LABEL OF A CHOICE ROW: "X" and two spaces on the current one, and an equal blank cell on the rest
/// so the names line up in a column. The house mark (`ValueBox`, the macro menu, the mod panel).
[[nodiscard]] std::string markedChoiceLabel(const std::string& name, bool current);

/// ADD ONE ROW OF A CHOICE LIST — marked, and flagged as the current one when it is. Returns the row.
visage::PopupMenu& addChoiceRow(visage::PopupMenu& menu, int id, const std::string& name, bool current);
[[nodiscard]] MenuPlacement lastMenuPlacementForTest();

/// Whether `source` owns a menu that is open right now, by the definition the toggle uses.
[[nodiscard]] bool menuOpenFor(const visage::Frame& source);

/// Close the menu `source` owns, if it has one open — what a pick or a click elsewhere does.
void dismissMenuOf(visage::Frame& source);

} // namespace feathers::ui
