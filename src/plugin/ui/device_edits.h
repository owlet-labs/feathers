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

#include "plugin/fx_chain.h"
#include "plugin/gui_bridge.h"
#include "plugin/mod_rack.h"
#include "plugin/ui/edit_history.h"
#include <utility>
#include <vector>

// UNDOING A LANE'S STRUCTURAL VERBS — add, remove, reorder — FOR BOTH LANES.
//
// IT HAS BEEN A STATED GAP SINCE THE RACK'S (+) LANDED, and the reason was never the history: it was
// that undoing an add means REMOVING a device, which moves every later device's routing column, and
// no road did that. The lane's remove built it. This is what it unblocks.
//
//=================================================================================================//
// THE ONE RULE: THESE RECORDS CONTAIN NO REMAP.                                                    //
//=================================================================================================//
//
// Every direction of every record below is one call to a bridge road — `removeFxDevice`,
// `insertFxDevice`, `moveFxDevice` and the rack's three — and none of them re-derives where anything
// went. Half a remap inside an undo record is how a remap acquires a second implementation, and the
// second one is the one that is wrong. (There is no remap left to do: a route names a device's id.)
//
// WHAT A RECORD DOES CARRY IS WHAT THE ROADS CANNOT RECOVER:
//
//   - THE DEVICE ITSELF, row and ID. `addFxDevice` mints a fresh id, so a redo that re-added rather
//     than re-inserting would hand the player a device with a new identity — and every route names the
//     id. Redo is therefore an INSERT of the recorded device, not an add, and the id survives a round
//     trip.
//   - ITS ROUTES. A removal takes the device's routes with it, which is the honest answer — but it
//     means they cannot be read back afterwards. They are lifted out BEFORE the removal
//     (`ParamBridge::routesOf`) and put back by the insert. An undo that restored the device and not
//     its routes would put back half of what it took, and it would look completely correct.
//
// A SNAPSHOT WOULD HAVE BEEN SIMPLER AND THE MEASUREMENT KILLED IT. The first design recorded the
// whole FxChain and the whole ModRoutes before and after — trivially correct, and 204 KB per record on
// an undo stack with no bound. Take the shape from the model, the number from the instrument.

namespace feathers::ui {

/// WHICH LANE A SLOT BELONGS TO. Not a new concept — device_row.h's slot space has been divided into
/// an fx half and a rack half since the routing table learned about devices (`deviceSlotOfFx`,
/// `deviceSlotOfRack`). This is that division named, so one record can serve both.
enum class DeviceLane { Fx, Rack };

/// A DEVICE ARRIVING OR LEAVING A SLOT — one record for both DIRECTIONS and both LANES.
///
/// ONE CLASS RATHER THAN FOUR, and that is deliberate rather than tidy. Add and remove are the same
/// act read in two directions; the fx lane's and the rack's are the same act on two lanes. Four
/// classes would be four chances for them to disagree about the thing they all have to get right —
/// that the device, its settings and its ROUTINGS travel together. The only per-lane knowledge is
/// which pair of bridge roads to call, and it is two lines.
class DeviceSlotEdit : public EditRecord {
public:
	/// `added` true for an add (undo removes), false for a remove (undo puts back). `row` and `routes`
	/// are what the slot held; for an add the routes are what a fresh device has, which is none. The
	/// routes name the device by its id, which it keeps (`ParamBridge::routesOf`).
	DeviceSlotEdit(ParamBridge& bridge, DeviceLane lane, size_t slot, DeviceRow row,
	               std::vector<backplane::Route> routes, bool added)
	    : bridge_(bridge), lane_(lane), slot_(slot), row_(row), routes_(std::move(routes)), added_(added) {}

	void undo() const override { added_ ? take() : give(); }
	void redo() const override { added_ ? give() : take(); }
	const char* verb() const override { return added_ ? "Add device" : "Remove device"; }

private:
	void give() const {
		if (lane_ == DeviceLane::Fx) {
			FxDevice device;
			device.row = row_;
			bridge_.insertFxDevice(slot_, device, routes_);
		}
		else {
			bridge_.insertRackDevice(slot_, row_, routes_);
		}
	}
	void take() const {
		if (lane_ == DeviceLane::Fx) {
			bridge_.removeFxDevice(slot_);
		}
		else {
			bridge_.removeRackDevice(slot_);
		}
	}

	/// A REFERENCE TO THE BRIDGE AND NOTHING ELSE OF THE EDITOR — edit_history.h's rule, and the
	/// reason it is a rule: the history outlives every widget by construction, so a record reaching
	/// back into a card would be a use-after-free waiting for somebody to press undo.
	ParamBridge& bridge_;
	DeviceLane lane_ = DeviceLane::Fx;
	size_t slot_ = 0;
	DeviceRow row_ {};
	std::vector<backplane::Route> routes_;
	bool added_ = true;
};

/// A DEVICE CHANGING PLACES. Its own inverse — undoing a move from A to B is a move from B to A with
/// the arguments swapped — so this carries no state beyond the two ends and needs no road the
/// forward direction did not.
class DeviceMoveEdit : public EditRecord {
public:
	DeviceMoveEdit(ParamBridge& bridge, DeviceLane lane, size_t from, size_t to)
	    : bridge_(bridge), lane_(lane), from_(from), to_(to) {}

	void undo() const override { move(to_, from_); }
	void redo() const override { move(from_, to_); }
	const char* verb() const override { return "Reorder"; }

private:
	void move(size_t from, size_t to) const {
		lane_ == DeviceLane::Fx ? bridge_.moveFxDevice(from, to) : bridge_.moveRackDevice(from, to);
	}

	ParamBridge& bridge_;
	DeviceLane lane_ = DeviceLane::Fx;
	size_t from_ = 0;
	size_t to_ = 0;
};

} // namespace feathers::ui
