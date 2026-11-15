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

#include "backplane/device_host.h"

#include <algorithm>
#include <cstddef>

namespace feathers::ui {

/// TRUE WHILE ANY ROUTE REACHES `control` of `device`. The modulated copy is the engine's, published a block at a time, so
/// with nothing routed it can only be a stale echo of the stored value — and the engine publishes nothing at all while
/// the plugin sleeps or is inactive, which is when a hand's edit must still reach the plot at once.
inline bool routedInto(const backplane::DeviceHost& host, DeviceInstanceId device, size_t control) {
	bool any = false;
	host.forEachRouteInto(backplane::Destination::control(device, control), &any,
	    [](void* ctx, backplane::Source) { *static_cast<bool*>(ctx) = true; });
	return any;
}

/// ONE CONTROL, by the same rule — for a plot that reads fields one at a time (a curve built from a lambda).
template <class Descriptor>
float readModulatedField(const backplane::DeviceHost& host, DeviceInstanceId device, const Descriptor& descriptor,
    size_t control, bool right = false) {
	const auto& params = descriptor.params;
	if (control >= params.size() || !params[control].modulatable || !routedInto(host, device, control)) {
		return host.fieldAt(device, control);
	}
	float r = 0.0f;
	return right && host.modulatedFieldRAt(device, control, r) ? r : host.modulatedFieldAt(device, control);
}

/// **A DEVICE'S ROW AS IT IS WHERE MODULATION LEFT IT** — what a plot draws, so it shows the filter, the shaper,
/// the envelope the engine is running and not the knob's base. A card that read `fieldAt` while the knob's ring
/// read `modulatedFieldAt` drew a picture the sound had left, the moment a modulator moved the control.
///
/// **A CONTROL NOTHING CAN MODULATE, OR NOTHING IS ROUTED TO, IS READ STORED** — a Type, a Mode, a switch, anything past the
/// declared params, a knob no route reaches.
/// A control nothing can modulate: Its modulated value is the engine's copy of the stored one a block late, and on an inactive
/// plugin no block comes, so a choice made there would not reach the picture until audio ran. A modulatable
/// control takes the modulated road: the left ear's `modulatedFieldAt`, and with `right` the right ear's
/// `modulatedFieldRAt` wherever a stereo route reaches it.
///
/// Fills `row` (which the caller may have started at the device's defaults) as far as the host has fields, and
/// returns how many it filled. NOT FOR AN EDIT'S BASE: a drag that writes the row back must start from the
/// stored one, or it bakes the modulation into the patch.
template <class Row, class Descriptor>
size_t readModulatedRow(const backplane::DeviceHost& host, DeviceInstanceId device, const Descriptor& descriptor,
    Row& row, bool right = false) {
	const size_t present = std::min(host.fieldsPresentAt(device), row.size());
	for (size_t i = 0; i < present; ++i) {
		row[i] = readModulatedField(host, device, descriptor, i, right);
	}
	return present;
}

} // namespace feathers::ui

namespace feathers::ui {

/// **THE STORED ROW** — what the knobs are set to, read without any modulation: the AT-REST shape of a plot, drawn beside
/// the as-modulated one (`readModulatedRow`) while a route moves it. The same fill `readModulatedRow` does, minus the
/// modulated road.
template <class Row>
size_t readStoredRow(const backplane::DeviceHost& host, DeviceInstanceId device, Row& row) {
	const size_t present = std::min(host.fieldsPresentAt(device), row.size());
	for (size_t i = 0; i < present; ++i) {
		row[i] = host.fieldAt(device, i);
	}
	return present;
}

/// **A SECOND SHAPE IS DRAWN ONLY WHERE MODULATION MOVED THE PLOT**: the as-modulated settings differ from the stored
/// ones. Unrouted, or routed with the modulator at its resting value, only the rest shape is drawn. One predicate for
/// every plot, so the draw and the test seam cannot disagree.
template <class Settings>
bool drawsModulatedShape(const Settings& rest, const Settings& modulated) {
	return !(rest == modulated);
}

}
