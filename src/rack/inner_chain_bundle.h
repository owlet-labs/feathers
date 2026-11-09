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

#include "device/registry.h"
#include "rack/inner_chain.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// **SEVERAL CHAINS INSIDE ONE CONTAINER BODY** — gap G2 of the bolt-on chains work, whose first caller is SKLITTER's pair of lanes (backlog EW).
//
// **THE PROBLEM, EXACTLY.** `device::ContainerOps` has ONE `saveState`, ONE `loadState` and ONE
// `maxBodyBytes`. A device holding two chains must therefore put two rack bodies inside one body, and
// there was no house framing for "N sub-bodies" — so the second device to want one would have invented
// a different one. This is that framing, written once, above the device rather than inside it.
//
// **IT IS NOT A NEW SERIALISATION.** Each lane's bytes are exactly what `InnerChain::saveState` writes
// and `InnerChain::loadState` reads, unchanged. All this adds is a way to tell them apart.
//
//------------------------------------------------------------------------------------------------//
// THE FRAME                                                                                        //
//------------------------------------------------------------------------------------------------//
//
//     magic    u32   kBundleMagic
//     version  u16   kBundleVersion
//     count    u16   how many lanes follow
//     per lane:
//       laneId u16   WHICH lane these bytes belong to — a stable id, not a position
//       bytes  u32   the lane's body length, bounded by `rack::kMaxBodyBytes`
//       body   ...   that many bytes, exactly as `InnerChain::saveState` wrote them
//
// **THE LANE ID IS STABLE AND EXPLICIT, AND THAT IS THE ONE DESIGN DECISION HERE.** A position would
// make "a third lane was added" a format migration and "this lane saved nothing" a silent shift of
// everything after it. An id makes both a non-event: a reader takes the lanes it recognises, skips the
// rest BY THEIR LENGTH — which is the only thing the length prefix buys that plain concatenation does
// not — and a lane that is absent is simply left as it was.
//
// **THE ORDER OF THE LANES IS NOT IN THE FORMAT, DELIBERATELY.** SKLITTER's grain lane runs before its
// master lane, but that is a property of the DEVICE and not of the patch — a player cannot reorder
// them. Storing it would be a second answer to a question the code already answers, and a saved order
// that disagreed with the code would be unresolvable. The id says which lane; the device says where it
// sits.
//
//------------------------------------------------------------------------------------------------//
// ALL-OR-NOTHING, WHICH `device::ContainerOps::loadState` REQUIRES                                  //
//------------------------------------------------------------------------------------------------//
//
// *"a device that returns false has left itself as it was: the host cannot tell a truncated body from
// a short one, so the device validates its own bytes, all or nothing."* With two chains that is a real
// constraint rather than a formality: applying lane 1 and then discovering lane 2 is corrupt would
// leave the device half-loaded and reporting failure.
//
// **IT IS BOUGHT WITH A SECOND PASS RATHER THAN A ROLLBACK.** `readChainBundle` walks the whole frame
// first — checking every length against the bytes remaining and against `kMaxBodyBytes`, and asking
// each lane `InnerChain::canLoad` — and only then loads any of them. No copy of the old state is kept
// and nothing has to be put back. See `InnerChain::canLoad` for why asking twice is 2x here and not
// the 2^depth that the same question about SAVING once cost this vocabulary.

namespace feathers::rack {

/// 'I','C','B','1' — little-endian, so it reads as the tag in a hex dump.
inline constexpr uint32_t kBundleMagic = 0x31424349u;
inline constexpr uint16_t kBundleVersion = 1;

/// ONE LANE THE HOLDER OWNS, as the codec sees it: a stable id and the chain it names.
struct BundleLane {
	uint16_t id = 0;
	InnerChain* chain = nullptr;
};

/// **THE LARGEST BUNDLE `lanes.size()` CHAINS COULD LEGITIMATELY WRITE** — what a holder returns from
/// `ContainerOps::maxBodyBytes`, so a host can bound a length it reads out of a file.
///
/// It is large: `kMaxBodyBytes` is 32 MiB and this is a multiple of it. That number was already
/// generous for one chain and is a ceiling rather than an expectation — a bound on what a reader will
/// walk, not a size anything allocates.
[[nodiscard]] inline constexpr size_t bundleMaxBytes(size_t lanes) {
	return sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint16_t)
	       + lanes * (sizeof(uint16_t) + sizeof(uint32_t) + kMaxBodyBytes);
}

/// **WRITE EVERY LANE INTO ONE BODY.** False if any lane could not be written; a holder returns that
/// straight out of `ContainerOps::saveState`.
///
/// A null `chain` is written as a lane with an empty body rather than skipped, so the count and the
/// ids describe the device rather than its luck.
bool writeChainBundle(std::span<const BundleLane> lanes, const device::ByteSink& sink);

/// **TAKE A BUNDLE BACK, ALL OR NOTHING.** Every lane named in the frame AND present in `lanes` is
/// loaded; a lane in the frame that `lanes` does not name is skipped by its length; a lane in `lanes`
/// that the frame does not name is left exactly as it was.
///
/// False — with nothing loaded and nothing touched — for a frame whose magic, version, count or any
/// length is wrong, or whose lanes any chain would refuse.
bool readChainBundle(std::span<const BundleLane> lanes, const uint8_t* data, size_t bytes);

/// **THE SAME, INTO LANES THAT MAY BE RUNNING** — `ContainerOps::applyState`. The same two passes, with
/// `InnerChain::applyState` in place of `loadState`: each lane's record becomes its body and its engine
/// converges at its own drain. Empty bytes empty every lane (false, nothing touched, if any lane has no
/// bridge to empty it through).
bool applyChainBundle(std::span<const BundleLane> lanes, const uint8_t* data, size_t bytes);

/// ONE LANE'S BYTES IN, ITS REPLACEMENT OUT — false refuses the whole walk.
using LaneMap = bool (*)(void* ctx, std::span<const uint8_t> in, std::vector<uint8_t>* out);

/// **EVERY LANE'S BODY THROUGH `map`, THE FRAME KEPT** (backlog OA: a copied container's body re-points what it names
/// of the record it sat in). Ids, order and count as they were; an empty lane is not handed over. False — `out`
/// untouched — for bytes that are not a whole bundle, or a lane `map` refuses.
bool mapChainBundle(std::span<const uint8_t> bytes, LaneMap map, void* ctx, std::vector<uint8_t>* out);

} // namespace feathers::rack
