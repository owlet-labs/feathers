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

#include "backplane/axes.h"
#include "composite/composite_types.h"
#include "device/registry.h"

// THE RACK'S SHAPE — ONE DECLARATION, because the standalone rack and a rack sitting inside a chain
// are the same engine and must be built the same way. It lived in rack_clap.cpp, where only the plugin
// could see it; a rack that is also a device needs it too, and two copies of a shape is two shapes.

namespace feathers::rack {

/// Sixteen slots, eight macros, and no parameters of its own — so every destination in the table is a
/// device destination. See the brief §11.1 for why the count is a runtime value that happens to be
/// fixed at construction rather than a compile-time constant.
inline constexpr backplane::Axes kRackAxes {
	// THE CORE SOURCES ARE THE MACROS, as many as the rack's surface declares.
	.coreSources = composite::kMacroCount,
	.coreDestinations = 0,
	.deviceSlots = 16,
	.outputStride = device::kMaxDeviceOutputs,
};

} // namespace feathers::rack
