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

#include "plugin/ui/mod_route_menu.h"
#include "plugin/ui/menu_anchor.h"

#include "plugin/ui/mod_state.h"

#include "visage_ui/frame.h"
#include "visage_ui/popup_menu.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace feathers::ui {

ModState* routingStateOf(const device::Modulatable& control) {
	const auto* routed = dynamic_cast<const RoutedControl*>(&control);
	return routed != nullptr ? routed->routingState() : nullptr;
}

namespace {

/// The name this file has always used for it, kept so the menu code below reads unchanged.
[[nodiscard]] ModState* stateOf(const device::Modulatable& control) { return routingStateOf(control); }

} // namespace

std::vector<device::Source> inboundSources(const device::Modulatable& control) {
	std::vector<device::Source> inbound;
	ModState* mod = stateOf(control);
	// **NOT A DESTINATION MEANS NOTHING TO LIST**, and the honesty rule's shape applies to a menu as
	// much as to a knob: offering one on a control nothing can reach would be offering a gesture that
	// can never have content.
	if (mod == nullptr || !control.display().isDestination) {
		return inbound;
	}
	// **THE SOURCES THE PANEL SAYS EXIST** (`ModState::sources`), each asked of the control — there is no
	// flat index to count to since a source became an endpoint (kill-lanes step 2).
	for (const device::Source source : mod->sources()) {
		if (std::fabs(control.amountFor(source)) >= device::kAmountUnroutedBelow) {
			inbound.push_back(source);
		}
	}
	return inbound;
}

void chooseInbound(device::Modulatable& control, device::Source source) {
	ModState* mod = stateOf(control);
	if (mod == nullptr) {
		return;
	}
	// **A SOURCE FROM THE HOST OUTSIDE IS ARMED WHERE IT LIVES** (the inbound-route menu, DI,
	// works for routes from outside a chain to inside it). An inner control's menu lists the
	// outer host's sources spelled as the inner state reads them (`asOuter`); picking one arms the
	// OUTER state with the outer's own spelling, and the inner state follows it as a child — so the
	// outer's arm lights, its Escape clears it, and the inner rings show the amount. Armed locally
	// instead, the inner panel would be in a mode the editor around it could not see. The local arm is
	// taken off so the child follows rather than shadows; the climb repeats for a chain in a chain.
	while (source.isOuter() && mod->parent() != nullptr) {
		mod->arm(false);
		mod = mod->parent();
		source = source.inner();
	}
	// **FOCUS THEN ARM, IN THAT ORDER.** `arm()` tells its listener WHICH source was armed, so a
	// focus after it would announce the previous one.
	mod->focus(source);
	// **PICKING A SOURCE IS ENTERING ASSIGN MODE FOR IT**, by design. The widget under the
	// pointer now edits this route's amount with its ordinary drag, which is the whole gesture.
	mod->arm(true);
}

size_t removeAllInbound(device::Modulatable& control) {
	ModState* mod = stateOf(control);
	const std::vector<device::Source> inbound = inboundSources(control);
	if (mod == nullptr || inbound.empty()) {
		return 0;
	}
	// **THE FOCUS IS PUT BACK.** `setAmount` writes the FOCUSED source, so clearing several routes
	// means walking the focus across them — and leaving it wherever the walk ended would re-aim the
	// panel as a side effect of a delete.
	const device::Source focusBefore = mod->source();
	// **ARMED LOCALLY FOR THE WALK.** A state that follows an armed parent answers `source()` with the
	// parent's, whatever it is focused on, and `setAmount` writes that — so the walk below would clear
	// the parent's route instead of each listed one. Arming this state itself makes its own focus the
	// answer; it is put back as it was, with the focus.
	//
	// **AND IT IS A BORROWED ARM, NOT A HAND'S** (backlog HM). An arm a hand makes clears every other
	// arm in the follow-tree — one gesture, one glowing surface — and this one is the editor arming a
	// state for the length of a walk and putting it straight back. Left as a hand's, it would silently
	// take the OUTER state out of assign mode as a side effect of picking "Remove all modulators",
	// which `test_rack_editor.cpp`'s DI-across-the-boundary case asserts against in so many words:
	// *"while the outer stays armed as it was"*.
	const bool armedBefore = mod->armedLocally();
	if (!armedBefore) {
		mod->arm(true, ModState::Arm::Borrowed);
	}
	for (const device::Source source : inbound) {
		mod->focus(source);
		// EACH THROUGH THE SAME BRACKETED ROAD A DRAG TAKES, so the routing table is never written
		// behind the history's back.
		//
		// **THAT MAKES THIS N UNDO STEPS RATHER THAN ONE, AND THE HISTORY HAS NO GROUPING TO OFFER.**
		// `EditHistory::record` takes one record and there is no transaction; inventing one for this
		// menu would be a design decision about the whole editor's undo taken inside a popup handler.
		// The honest cost is that clearing three routes takes three undos to put back, which is
		// recoverable and visible, where a bespoke grouped record would be neither.
		control.beginAmountGesture();
		control.setAmount(0.0f);
		control.endAmountGesture();
	}
	mod->focus(focusBefore);
	if (!armedBefore) {
		mod->arm(false, ModState::Arm::Borrowed);
	}
	return inbound.size();
}

/// **THE ROUTE ROWS AND THEIR PICK — ONE BUILDER FOR BOTH MENUS** (T7 review). `showRouteMenu` and the
/// knob's `controlMenu` used to carry a copy each, so the route-menu cases, which drive `showRouteMenu`,
/// passed without exercising the menu a hand actually reaches on a knob. Appends to an EMPTY `menu` and
/// sets its pick; false (and nothing appended) when the control has no panel state or nothing routed. The
/// pick reaches the control through a raw pointer: a caller whose menu may outlive the control guards it
/// (`controlMenu` does, by the knob's token).
bool appendRouteRows(visage::PopupMenu& menu, device::Modulatable& control) {
	ModState* mod = stateOf(control);
	const std::vector<device::Source> inbound = mod != nullptr ? inboundSources(control) : std::vector<device::Source> {};
	if (inbound.empty()) {
		return false;
	}
	for (size_t i = 0; i < inbound.size(); ++i) {
		char label[96] = {};
		// THE AMOUNT IN THE LABEL, because the whole point of the gesture is to tune it — a list of
		// bare names would make you pick one to find out which one you meant.
		std::snprintf(label, sizeof(label), "%s   %+.0f%%", mod->sourceName(inbound[i]).c_str(),
		              static_cast<double>(control.amountFor(inbound[i])) * 100.0);
		menu.addOption(static_cast<int>(i), label);
	}
	// **ONE ROW, NOT ONE PER SOURCE.** Per-route removal is the double-click on an armed destination,
	// which both widgets already implement; see `removeAllInbound`.
	menu.addBreak();
	menu.addOption(static_cast<int>(inbound.size()),
	               inbound.size() == 1 ? "Remove modulator" : "Remove all modulators");

	// **ONE LINE, CALLING THE VERB A CASE CAN ALSO CALL.** Everything this gesture DOES lives in
	// `chooseInbound`; this only decides which half of the list was picked.
	device::Modulatable* target = &control;
	menu.onSelection() = [target, inbound](int id) {
		if (id < 0 || static_cast<size_t>(id) > inbound.size()) {
			return;
		}
		const size_t index = static_cast<size_t>(id);
		if (index == inbound.size()) {
			removeAllInbound(*target);
			return;
		}
		chooseInbound(*target, inbound[index]);
	};
	return true;
}

bool showRouteMenu(visage::Frame& anchor, device::Modulatable& control) {
	// **AN EMPTY MENU IS A GESTURE THAT ANSWERED NOTHING.** Refusing lets the caller's press continue
	// on its ordinary road rather than being swallowed by a popup with no entries in it.
	visage::PopupMenu menu;
	if (!appendRouteRows(menu, control)) {
		return false;
	}
	showMenuAt(std::move(menu), anchor, { 0.0f, 0.0f, anchor.width(), anchor.height() });
	return true;
}

//---------------//
// The host rows //
//---------------//

namespace {

/// The slot line's id: a row that is never a verb, told apart from a separator (-1) in `ControlMenuRow`.
constexpr int kControlMenuSlotLine = -2;

const char* stateName(device::HostSlotStatus::State state) {
	using State = device::HostSlotStatus::State;
	switch (state) {
		case State::Default: return "Default";
		case State::Recent: return "Recent";
		case State::Pinned: return "Pinned";
		case State::Anchored: return "Automated"; // the player's word; the code keeps Anchored
		case State::Unslotted:
		case State::Unavailable: break;
	}
	return "";
}

} // namespace

std::vector<ControlMenuRow> hostMenuRows(const device::Modulatable& control) {
	using State = device::HostSlotStatus::State;
	const device::HostSlotStatus status = control.hostSlot();
	std::vector<ControlMenuRow> rows;
	switch (status.state) {
		case State::Unavailable:
			return rows; // no pool behind this control: nothing to offer
		case State::Unslotted:
			// **SWITCHED OFF, NOT HIDDEN, WHEN THE POOL HAS NOTHING TO GIVE** — every slot Pinned or Anchored.
			// A missing row would read as "this control cannot be exposed"; the dimmed one says why.
			rows.push_back(status.canExpose ? ControlMenuRow { kControlMenuExpose, "Expose to host", true }
			                                : ControlMenuRow { kControlMenuExpose, "Expose to host (no free slot)", false });
			return rows;
		case State::Default:
		case State::Recent:
		case State::Pinned:
		case State::Anchored: break;
	}
	// WHICH SLOT, IN THE HOST'S OWN COUNTING ("Slot 1" is id 0x1000), and in what state — the fact a hand
	// needs to find the lane in the DAW, and the reason the verb below is the one offered.
	rows.push_back({ kControlMenuSlotLine,
	                 "Host: Slot " + std::to_string(status.slot + 1) + " \xc2\xb7 " + stateName(status.state), false });
	// ONE VERB PER STATE, the one that state admits (design §2). A Default or Recent slot is on the host but
	// recyclable, so it is offered the pin that makes it stay; `Pin` rather than "Expose to host", because
	// it is exposed already. AN AUTOMATED (Anchored) SLOT OFFERS NOTHING: the host keeps its
	// lanes and modulators on the id whatever the plugin does, so "release" could only hand them to the next
	// control that took the slot.
	switch (status.state) {
		case State::Default:
		case State::Recent: rows.push_back({ kControlMenuExpose, "Pin", true }); break;
		case State::Pinned: rows.push_back({ kControlMenuUnpin, "Unpin", true }); break;
		case State::Anchored:
		case State::Unslotted:
		case State::Unavailable: break;
	}
	return rows;
}

bool chooseHostMenuEntry(device::Modulatable& control, int id, std::string* displaced) {
	if (displaced != nullptr) {
		displaced->clear();
	}
	switch (id) {
		case kControlMenuExpose: return control.exposeToHost(displaced);
		case kControlMenuUnpin: return control.unpinFromHost();
		default: return false;
	}
}

bool appendHostRows(visage::PopupMenu& menu, device::Modulatable& control, std::weak_ptr<void> alive,
                    HostMenuChanged changed) {
	const std::vector<ControlMenuRow> rows = hostMenuRows(control);
	if (rows.empty()) {
		return false;
	}
	if (menu.size() > 0) {
		menu.addBreak();
	}
	for (const ControlMenuRow& row : rows) {
		menu.addOption(row.id, row.label).enable(row.enabled);
	}
	// **THE PICK MENU ALREADY HAD, KEPT AND GUARDED, AND THE HOST'S IN FRONT OF IT.** By value: the menu
	// outlives this call, and the only thing it may reach through is the control, while `alive` says the
	// control's owner still stands. A pick after the card's destruction finds the token gone and writes
	// NOTHING — neither the host verb nor the earlier pick, which reached the same control.
	const visage::CallbackList<void(int)> earlier = menu.onSelection();
	device::Modulatable* target = &control;
	menu.onSelection() = [target, alive = std::move(alive), changed = std::move(changed), earlier](int id) {
		if (alive.expired()) {
			return;
		}
		if (id >= kControlMenuExpose && id <= kControlMenuLast) {
			std::string displaced;
			if (chooseHostMenuEntry(*target, id, &displaced) && changed) {
				changed(displaced);
			}
			return;
		}
		earlier.callback(id);
	};
	return true;
}

std::optional<visage::PopupMenu> controlMenu(device::Modulatable& control, std::weak_ptr<void> alive,
                                             HostMenuChanged changed) {
	visage::PopupMenu menu;
	// THE ROUTE ROWS, FROM THE BUILDER `showRouteMenu` USES — with the pick guarded below: it reaches the
	// control through a raw pointer, which is what a card's destruction dangles.
	(void)appendRouteRows(menu, control);
	const std::weak_ptr<void> token = alive;
	if (!appendHostRows(menu, control, std::move(alive), std::move(changed))) {
		if (menu.size() == 0) {
			return std::nullopt; // nothing to list: the press goes on to its ordinary road
		}
		// NO HOST SECTION, SO NOTHING WRAPPED THE ROUTE PICK: guard it here, by the same token.
		const visage::CallbackList<void(int)> earlier = menu.onSelection();
		menu.onSelection() = [token, earlier](int id) {
			if (!token.expired()) {
				earlier.callback(id);
			}
		};
	}
	return menu;
}

} // namespace feathers::ui
