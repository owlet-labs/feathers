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

#include "device/registry.h"
#include "rack/rack_state.h"

#include <cstddef>
#include <cstdint>
#include <span>

// **HOW MANY CHAINS DOWN SOMETHING REACHES** — the depth gate's measure (2026-10-31). A chain `d` chains down may take
// content `h` tall only where `fitsNesting(d, h)` (`rack_device.h`): its deepest chain, at `d + h`, must be one a file
// gives back full. Measured from the bytes a road is about to place — a fragment, a preset, a container's body — so
// one walk answers for a paste, a drag, a group, a preset and an undo alike. Message thread; it parses.

namespace feathers::rack {

/// A DEVICE AS A PATCH HOLDS IT: 0 for a leaf; for a container (its descriptor says so) or a device with a site, one
/// chain for its own and as many again as the tallest device in any chain it holds. An empty container is 1 tall.
[[nodiscard]] size_t deviceHeight(const RackPatch::Device& device, const device::Catalogue& catalogue);
/// A CHAIN'S DOCUMENT: its tallest device (the chain itself is the caller's).
[[nodiscard]] size_t patchHeight(const RackPatch& chain, const device::Catalogue& catalogue);
/// A CONTAINER'S OR A SITE'S BODY: the tallest of its chains, each counted as one plus its tallest device; 0 for none.
[[nodiscard]] size_t bodyHeight(std::span<const uint8_t> body, const device::Catalogue& catalogue);
/// WHETHER A CONTAINER (OR A SITE) HOLDING `body` MAY STAND IN A CHAIN `holderDepth` DOWN — it is at least one chain
/// tall, empty or not.
[[nodiscard]] bool containerBodyFits(size_t holderDepth, std::span<const uint8_t> body, const device::Catalogue& catalogue);
/// WHETHER A CONTAINER WHOSE OWN CHAINS STAND `chainsDepth` DOWN may take `body`: what each chain holds must fit there.
[[nodiscard]] bool bodyFitsChainsAt(size_t chainsDepth, std::span<const uint8_t> body, const device::Catalogue& catalogue);

} // namespace feathers::rack
