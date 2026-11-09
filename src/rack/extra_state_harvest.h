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

#include "device/extra_state.h"
#include "rack/chain_record.h"

// THE MESSAGE THREAD'S HALF OF `device/extra_state.h` — the record catching up with a device that changes its own
// extra state, and a load giving every such device a fresh basis. One rule for both hosts (the rack's bridge and
// Feathers'), so they cannot answer it two ways.

namespace feathers::rack {

/// **AT A LOAD**: every device in `record` that publishes its own extra state (`ModOps::publishExtraState`) gets a
/// fresh basis, so the device drops what it had rewritten and plays what was loaded — even when the load keeps the
/// running instance (the same id) and the file's basis equals the one it last took.
void stampExtraStateBases(ChainRecord* record);

/// **ONE SNAPSHOT INTO THE RECORD**, under the basis guard: written only when the snapshot is about a device the record
/// holds, the device publishes its extra state, the counts agree and the record's basis still equals the snapshot's.
/// Only the floats that differ are written. True when any did.
bool harvestExtraState(ChainRecord* record, const device::ExtraStateSnapshot::Read& read);

} // namespace feathers::rack
