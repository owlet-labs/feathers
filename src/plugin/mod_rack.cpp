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

#include "plugin/mod_rack.h"

// WHAT IS IN THE RACK — and nothing about what any of them IS.
//
// This file used to carry the DAHDSR's field order, which was wrong in the way that matters: the
// order of a device's controls is the device's own knowledge, index-aligned with the descriptors it
// publishes, and a host holding a second copy is a host that has to be edited when the device grows
// a knob. It moved to mod/dahdsr/dahdsr_types.h. What is left here is genuinely the host's: how many
// devices the rack holds, and which slot each one is in.
//
// The rack's stream functions are gone — it saves with the lane as one chains block (audition.cpp,
// `rack/rack_state.h`). This file was separate from them because BOTH SIDES need it —
// the engine holds a ModRack and the editor publishes one — and leaving it there would have made
// feathers-synth link feathers-gui-bridge, a voice layer that cannot be built without an editor's
// parameter road.

namespace feathers {

mod::dahdsr::Settings ModRack::dahdsrAt(size_t slot) const {
	if (slot >= device.size() || !holds(slot, mod::dahdsr::kTraits.tag)) {
		return {};
	}
	mod::dahdsr::Row row {};
	// THROUGH THE SHARED CONVERSION, whose return value is the count of fields that were really
	// there — and settingsOf uses it to put the ABSENT ones at their declared defaults rather than at
	// zero. That is the whole reason device_row.h marks fromDeviceRow [[nodiscard]].
	const size_t fields = fromDeviceRow(device[slot], row);
	return mod::dahdsr::settingsOf(row, fields);
}

void ModRack::setDahdsrAt(size_t slot, const mod::dahdsr::Settings& settings) {
	if (slot >= device.size()) {
		return;
	}
	// THE WHOLE IDENTITY IS KEPT — the serial a saved routing names this device by, and the LANE the
	// live table addresses it at. A settings edit is not a new device on either count.
	const DeviceIdentity keep = identityOf(device[slot]);
	device[slot] = toDeviceRow(mod::dahdsr::kTraits.tag, keep, mod::dahdsr::rowOf(settings));
}

mod::mseg::Settings ModRack::msegAt(size_t slot) const {
	if (slot >= device.size() || !holds(slot, mod::mseg::kTraits.tag)) {
		return {};
	}
	mod::mseg::Row row {};
	const size_t fields = fromDeviceRow(device[slot], row);
	return mod::mseg::settingsOf(row, fields);
}

void ModRack::setMsegAt(size_t slot, const mod::mseg::Settings& settings) {
	if (slot >= device.size()) {
		return;
	}
	const DeviceIdentity keep = identityOf(device[slot]);
	device[slot] = toDeviceRow(mod::mseg::kTraits.tag, keep, mod::mseg::rowOf(settings));
}

bool ModRack::holds(size_t slot, const char* tag) const {
	return slot < device.size() && device::sameTag(device[slot].tag.data(), tag);
}

DeviceInstanceId ModRack::instanceAt(size_t slot) const {
	return slot < device.size() ? device[slot].instanceId : kNoInstance;
}

void ModRack::setIdentityAt(size_t slot, DeviceIdentity identity) {
	if (slot < device.size()) {
		device[slot].instanceId = identity.instanceId;
	}
}

ModRack defaultModRack() {
	ModRack rack;
	rack.count = 1;
	// THE FIRST DEVICE GETS THE FIRST SERIAL. A default rack whose one device had kNoInstance would
	// be a device nothing could route to, which is a worse starting point than an empty rack.
	// AND IT IS A DAHDSR, SAID OUT LOUD. The slot used to be one by the array's element type; now
	// the rack has to stamp the tag, which is the change in one line.
	// AND IT IS THE DEFAULT PATCH'S AMP ENVELOPE (backlog EF): the bridge routes it to Level at full
	// amount (`ParamBridge::routeDefaultEnvelope`), so it holds at FULL rather than the device's half and
	// lets go over a long second. Every other stage keeps the device's default.
	mod::dahdsr::Settings envelope;
	envelope.sustain = 1.0f;
	envelope.release = 0.0f; // log2 seconds: one second
	rack.setDahdsrAt(0, envelope);
	// ITS PLACE IN THE ONE SEQUENCE, not "the first serial" — the fx default takes the next one. See
	// plugin/device_instance.h: both defaults stamped kFirstInstance, so a fresh instance held two
	// devices under one id before any gesture. (It stamped a LANE here too, for the same reason and
	// against the same failure — a device nothing could route to, with nothing saying so. A route
	// names the id now, so the id is the whole of it.)
	rack.setIdentityAt(0, { kDefaultRackInstance });
	rack.nextInstanceId = kFirstFreeInstance;
	return rack;
}

size_t slotOfInstance(const ModRack& rack, DeviceInstanceId id) {
	if (id == kNoInstance) {
		return kNoPosition;
	}
	for (size_t slot = 0; slot < rack.count && slot < rack.device.size(); ++slot) {
		if (rack.instanceAt(slot) == id) {
			return slot;
		}
	}
	return kNoPosition;
}

DeviceInstanceId instanceOfSlot(const ModRack& rack, size_t slot) {
	if (slot >= rack.count || slot >= rack.device.size()) {
		return kNoInstance;
	}
	return rack.instanceAt(slot);
}

} // namespace feathers
