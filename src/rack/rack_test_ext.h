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

#include "clap/clap.h"

#include <stddef.h>
#include <stdint.h>

// A PRIVATE EXTENSION SO A VENUE CAN BUILD A CHAIN.
//
// THE SAME DEVICE `feathers_clap.cpp` ALREADY USES, for the same reason: adding a device is not a
// parameter and must not become one, so there is no road through the standard API for a test to put
// something in the rack. A host that has never heard of this extension gets nullptr and is unaffected.
//
// IT IS DELIBERATELY THIN — enough to build a chain and draw a route, and nothing that duplicates a
// verb the real API already has. Macros go in as ordinary parameter events, because that is how a DAW
// would send them and a venue that used a private road for them would be testing the private road.

// **/3 (2026-09-25, backlog JV) appended `bridge`** — bumped, not appended under /2, for the reason
// `feathers_test_chain.h` gives: a caller built against the longer struct would read past a /2 one's end.
static const char CLAP_EXT_FEATHERS_RACK_TEST[] = "com.owlet.feathers.rack-test/3";

/// **THE RACK'S TEST ROAD, BY IDENTITY (kill-lanes §2.4b).** Version 1 spoke the axes' flat coordinates —
/// a venue computed a route's indices from strides it asked for — and `axes()` existed to hand those
/// strides out. A device is named by the id `add_device` answers; a host end by `0` and its index; an
/// output or control by the device's own index for it. Nothing here is an address.
typedef struct clap_plugin_feathers_rack_test {
	/// Add a device by tag. Answers its instance id, or 0 (`kNoInstance`) when refused.
	uint32_t(CLAP_ABI* add_device)(const clap_plugin* plugin, const char* tag);

	/// Draw a route. A device end is `(id, output)` / `(id, control)`; a host end is `(0, index)`.
	void(CLAP_ABI* set_route)(const clap_plugin* plugin, uint32_t sourceDevice, uint32_t sourceIndex,
	                          uint32_t destinationDevice, uint32_t destinationIndex, float amount);

	/// The engine's resolved value of `control` on the device `id`, as of the last block.
	float(CLAP_ABI* resolved_field)(const clap_plugin* plugin, uint32_t id, size_t control);

	void*(CLAP_ABI* editor)(const clap_plugin* plugin);

	void(CLAP_ABI* sync_cards)(const clap_plugin* plugin);

	/// The editor's host (`feathers::rack::RackBridge*`), for a venue in the same process that has to make
	/// the edits an editor makes — a fold, a label, a macro's mode — and watch what the host is told.
	/// MAIN THREAD, as every edit through it is.
	void*(CLAP_ABI* bridge)(const clap_plugin* plugin);

} clap_plugin_feathers_rack_test;
