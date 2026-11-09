/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not,
 * see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "device/registry.h"

#include <cstddef>
#include <vector>

namespace feathers::hostslots {

/// **WHERE A SLOT'S TARGET PRINTS ITS OWN WORDS** (rack stage 2, §3, "the chain in the DAW") — what
/// `SlotHost::valueToText` hands a device's `TextOps`: the hook, the control's index, the device's controls as
/// the owner's record holds them, and the instance's message-thread surface. Built on the main thread for one
/// call and dropped; nothing in it is kept. Its own header so the owner (`rack::SlotOwner`, no CLAP) and the
/// CLAP glue (`slot_host.h`) can both name it.
struct TextSite {
	const device::TextOps* text = nullptr;
	size_t control = 0;
	/// Every control of the device, absent ones at their declared default — what `TextView::field` answers.
	std::vector<float> fields;
	/// `DeviceHost::containerSurfaceOf` for the device, or null.
	const void* surface = nullptr;
};

} // namespace feathers::hostslots
