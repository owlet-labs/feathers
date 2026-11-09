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

#include "device/modulatable.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace visage {
class Frame;
class PopupMenu; // complete in `visage_ui/popup_menu.h`, which a caller of the builders below includes
}

// **THE INBOUND-ROUTE MENU, FOR ANY WIDGET THAT HOLDS A MODULATABLE — backlog DI.**
//
// Right-click a destination and every source routed into it is listed with its amount; picking one
// focuses and arms that source, so the ordinary drag on that same widget then tunes THAT route.
//
// **IT LIVES HERE BECAUSE NOTHING ABOUT IT IS A KNOB.** It was written inside `Knob`, but it belongs to
// anything that implements modulation: the
// logic touches a `Modulatable` and the panel state and nothing else. A second copy in `ValueBox`
// would have been the fifth spelling of a gesture in this editor, and this repo has a note about what
// that costs every time (`card_factory.h`: two hosts disagreeing about a device's UI is a device that
// looks like two devices).
//
// **AND IT NEEDS NO PER-WIDGET WIRING, WHICH IS THE PART THAT MATTERS.** The obvious shape was a
// `setModulation` on every widget that wants the menu — seven `ValueBox` construction sites, each
// free to forget it. That is precisely the trap that shipped Bus Utility with no arm glow: a card
// that forwarded the panel state to its controls and not to its knobs. So the state is asked of the
// CONTROL, which already holds it, and a widget passes only what it has.

namespace feathers::ui {

class ModState;

/// **A CONTROL THAT KNOWS WHICH PANEL STATE IT BELONGS TO.**
///
/// `device::Modulatable` is the DEVICE tier's vocabulary and cannot name `ui::ModState` — the whole
/// point of that boundary is that a device knows nothing about this editor. So the answer is a small
/// interface on THIS side, implemented by the two adapters that already hold a `ModState*`, and found
/// by a `dynamic_cast` from the free functions below.
///
/// A CONTROL THAT DOES NOT IMPLEMENT IT OFFERS NO MENU, which is the same honest refusal
/// `Modulatable::amountFor` makes by defaulting to zero: not a claim that nothing is routed, an
/// admission that this control cannot say.
class RoutedControl {
public:
	virtual ~RoutedControl() = default;
	/// May be null — a control never handed the panel's state, which is the ordinary case in a test
	/// fixture and in the rack's editor before a panel exists.
	[[nodiscard]] virtual ModState* routingState() const = 0;
};

/// **THE PANEL STATE BEHIND A CONTROL, or null** — the cast rather than an argument every widget
/// would have to remember to pass. See `RoutedControl` for why.
///
/// Public because a widget needs it for more than the menu: a `ValueBox` uses it to put ITSELF on the
/// state's watcher list, which is the repaint road it never had. Doing that through the control keeps
/// the no-wiring promise this header was written for — no `setModulation` at seven construction
/// sites, and so no site free to forget one.
[[nodiscard]] ModState* routingStateOf(const device::Modulatable& control);

/// EVERY SOURCE WITH A LIVE ROUTE INTO THIS CONTROL, in the order `ModState::sources` lists them.
///
/// Membership is `device::kAmountUnroutedBelow`, the same threshold a drag uses to REMOVE a route —
/// so a route this lists is one that exists, not one sitting at an amount the drag would have deleted
/// on the way past. Empty for a control that is not a destination, has no panel state, or has nothing
/// routed to it; all three draw the same and should.
[[nodiscard]] std::vector<device::Source> inboundSources(const device::Modulatable& control);

/// **THE VERB BEHIND A MENU ENTRY** — focus and arm the source, so the widget under the pointer then
/// tunes THAT route with its ordinary drag.
///
/// Separate from the menu because a popup's selection callback cannot be reached from a case: a popup
/// needs a window, and synthesising the click is banned on the development display. A gesture whose behaviour
/// lives in an untestable closure is a gesture nothing can assert.
void chooseInbound(device::Modulatable& control, device::Source source);

/// **CLEAR EVERY ROUTE INTO THIS CONTROL.** One menu row that removes all modulators, rather than one
/// per source, because a double-click already removes one.
///
/// **PER-ROUTE REMOVAL ALREADY EXISTED AND THE MENU WAS DUPLICATING IT.** A double-click on an armed
/// destination unroutes the focused source — `Knob` and `ValueBox` both implement it and both say so
/// — so a Remove row per source was a second spelling of a verb the editor already had, which is the
/// thing `source_grid.cpp` warns about: *"a second glyph for the same action would read as a second
/// action"*. What the menu adds is the one thing the double-click cannot do, which is clear ALL of
/// them at once.
///
/// **IT LEAVES THE FOCUS WHERE IT FOUND IT.** Removal walks the sources — `setAmount` writes the
/// focused one — and a gesture that silently re-aimed the panel at whichever route happened to be
/// last would be an editing side effect nobody asked for.
///
/// RETURNS HOW MANY ROUTES WERE REMOVED, so a caller can say so and a case can assert it.
size_t removeAllInbound(device::Modulatable& control);

/// **THE ROUTE ROWS AND THEIR PICK**, appended to an EMPTY `menu` — the builder `showRouteMenu` and `controlMenu` share,
/// public so a widget with rows of its own after them (the TrigPad's normal, backlog NU) lists the same routes. False,
/// and nothing appended, when the control has no panel state or nothing routed. The pick reaches the control through a
/// raw pointer: a caller whose menu may outlive the control guards it.
bool appendRouteRows(visage::PopupMenu& menu, device::Modulatable& control);

/// OPEN THE MENU AT `anchor`. Returns whether one was shown — false when there is nothing to list, so
/// the caller's press can fall through to its ordinary road rather than being swallowed by an empty
/// popup.
bool showRouteMenu(visage::Frame& anchor, device::Modulatable& control);

//---------------------------------------------------------------------------------------------------//
// THE HOST ROWS (backlog JU, host slots T7): "Expose to host", and for a control that has a slot, which //
// slot and in what state, with the one verb that state admits.                                         //
//---------------------------------------------------------------------------------------------------//

/// The host rows' ids, far past any row a route list or a macro menu numbers from zero — so a host section
/// can be appended to either and its picks told apart by the id alone.
enum ControlMenuEntry : int {
	kControlMenuExpose = 1000, ///< `exposeToHost`: "Expose to host" on an unslotted control, "Pin" on a Default or Recent
	kControlMenuUnpin,         ///< `unpinFromHost`, on a Pinned control
	/// THE LAST HOST ROW. There is no "Release anchor": the host's lanes stay on an
	/// automated slot's id whatever the plugin does, so no menu offers to let it go.
	kControlMenuLast = kControlMenuUnpin,
};

/// ONE ROW AS A HAND SEES IT. A separator is id -1 with an empty label; the slot line is id -2, never
/// enabled: it says where the control stands and is not a verb.
struct ControlMenuRow {
	int id = -1;
	std::string label;
	bool enabled = true;
};

/// **WHAT THE HOST SECTION LISTS FOR THIS CONTROL** — empty for a control whose host has no pool
/// (`HostSlotStatus::Unavailable`), so a menu offers nothing that could not be done. Otherwise one of:
/// - no slot: `Expose to host`, or `Expose to host (no free slot)` SWITCHED OFF when every slot is
///   Pinned or Anchored — shown rather than hidden, so the reason the verb is missing is on the face;
/// - a slot: `Host: Slot N · <State>` (not selectable), then `Pin` (Default, Recent) or `Unpin` (Pinned). An
///   Anchored slot shows its line alone, named `Automated` — the player's word for it;
///   the code's state stays `Anchored`. N counts from 1, as the host's own "Slot N" names do.
[[nodiscard]] std::vector<ControlMenuRow> hostMenuRows(const device::Modulatable& control);

/// **THE VERB BEHIND A HOST ROW**, callable without a popup. Returns whether it changed anything; any other
/// id does nothing. `displaced`, when not null, gets the label a recycled Recent slot held (see
/// `Modulatable::exposeToHost`).
bool chooseHostMenuEntry(device::Modulatable& control, int id, std::string* displaced);

/// What a host verb that changed something tells its widget: the label it displaced, or "" (see above).
using HostMenuChanged = std::function<void(const std::string& displaced)>;

/// **APPENDS THE HOST SECTION TO `menu`** behind a break (none when `menu` is empty), and routes its picks:
/// an id from `ControlMenuEntry` runs `chooseHostMenuEntry`, then `changed`; every other id goes to the
/// pick `menu` already had. Both run ONLY WHILE `alive` HOLDS — the token of whatever owns `control`, so a
/// popup still open when its card is destroyed writes nothing (the shape `MathCard::guardedMenuFor` and
/// `RackEditor::alive_` have). The earlier pick is guarded by the same token. Returns whether it appended.
bool appendHostRows(visage::PopupMenu& menu, device::Modulatable& control, std::weak_ptr<void> alive,
                    HostMenuChanged changed);

/// **THE RIGHT-CLICK ON A CONTROL: its inbound routes, then its host rows** — either may be absent, and
/// nullopt when both are, so a caller's press falls through as it does from `showRouteMenu`. The route
/// rows are the ones `showRouteMenu` lists, doing the same; every pick is guarded by `alive`.
[[nodiscard]] std::optional<visage::PopupMenu> controlMenu(device::Modulatable& control, std::weak_ptr<void> alive,
                                                           HostMenuChanged changed);

} // namespace feathers::ui
