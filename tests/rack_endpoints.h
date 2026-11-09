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

#include "backplane/endpoint.h"
#include "rack/rack_engine.h"

#include <cstddef>

// **A CASE THAT HOLDS A POOL SLOT AND WANTS AN ENDPOINT** — the rack suites' one crossing, kept in one
// place so it can be counted and, when the suites stop holding slots, deleted.
//
// The rack suites were written against `RackEngine`'s coordinate API: a source was
// `deviceSourceOf(axes, lane, output)`, a destination `deviceDestinationOfOrdinal(axes, lane, ordinal)`,
// a macro its bare index. Kill-lanes §2.4b took that API to endpoints — a device by the id it wears, an
// output or control by the device's own index — and these five helpers are the old spellings answered
// from the engine rather than computed from strides. `ordinalOf` exists because the old destination took
// the N-TH MODULATABLE control and a case that said `0` meant "its first modulatable one"; the endpoint
// takes the control index, so the ordinal goes through the device's declaration exactly once, here.
namespace feathers::test {

inline DeviceInstanceId idAt(const rack::RackEngine& e, size_t lane) {
	return e.slotAtLane(lane).id;
}

inline backplane::Source macro(size_t index) {
	return backplane::Source::core(index);
}

inline backplane::Source outputOf(const rack::RackEngine& e, size_t lane, size_t output) {
	return backplane::Source::output(idAt(e, lane), output);
}

inline backplane::Destination controlOf(const rack::RackEngine& e, size_t lane, size_t control) {
	return backplane::Destination::control(idAt(e, lane), control);
}

inline backplane::Destination ordinalOf(const rack::RackEngine& e, size_t lane, size_t ordinal) {
	const device::Descriptor* desc = e.slotAtLane(lane).live_desc();
	const size_t control = desc != nullptr ? desc->controlOfNthModulatable(ordinal) : static_cast<size_t>(-1);
	return backplane::Destination::control(idAt(e, lane), control);
}

} // namespace feathers::test
