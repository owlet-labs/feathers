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

#include "backplane/device_instance_id.h"

// WHAT NAMES ONE DEVICE IN ONE PATCH, for every chain that holds devices.
//
// IT HAS ITS OWN HEADER BECAUSE BOTH CHAINS NEED IT AND NEITHER OWNS IT. It was declared in
// mod_rack.h when the rack was the only chain with identities; the fx lane needs the same concept
// now, and having the lane include the rack to borrow a typedef would be the fx chain depending on
// the modulator rack for no reason but where a name happened to be typed first.
//
// A MONOTONIC SERIAL, STAMPED WHEN A DEVICE IS ADDED, CARRIED IN THE PATCH, NEVER REUSED WITHIN ONE.
//
// THIS IS CLAUDE.md's ABA RULE ARRIVING WHERE THAT RULE SAID IT WOULD. The realtime constraint
// forbids allocation after activate, so devices live in fixed slots; a fixed slot exists precisely
// so the thing in it can be replaced. The rule's own words: *"under no-allocation-after-activate an
// address is never an identity, only a contemporaneous handle"*, with the remedy *"key on a
// monotonic build serial stamped where the thing is built"*. A slot index is that address. This is
// that serial.
//
// WHAT IT BUYS IS SWAP, and swap is the case a slot index cannot survive. Replace a slot's device
// and every routing addressed to "that slot, control 3" silently becomes a routing to a different
// device's third control. With an id, a route whose instance is gone is simply not installed when
// the table is rebuilt: the route DROPS, which is the honest outcome, instead of landing somewhere
// nobody aimed it.
//
// This was decided at the start of the arc: a saved routing points at a stable instance id and a
// control, not at a position.

// THE DECLARATIONS THEMSELVES NOW LIVE IN `backplane/device_instance_id.h`, unrenamed, because the
// graph in `backplane/graph.h` keys its nodes on this type (vision doc D1) and the backplane may not
// include the plugin. This header keeps its argument and its includers; the type is the same type.
//
// The first serial any chain hands out.
//
// EACH CHAIN COUNTS SEPARATELY, AND THAT IS ONLY SAFE BECAUSE A ROUTE SAYS WHICH CHAIN IT MEANS.
// The fx lane and the rack both start at 1, so instance 1 is ambiguous on its own — the route
// record disambiguates it by carrying a distinct KIND for an fx device and a rack device rather
// than one shared "device" kind.
//
// A first draft of this comment claimed the record already did that. It did not: `StoredRoute` had
// a single Device kind, so separate counters would have made a route to fx instance 1 and a route
// to rack instance 1 indistinguishable — an ambiguity invented by a comment asserting a property
// nothing implemented. Written down because the alternative reading is available and wrong: a
// SHARED counter would also work, and is worse, because it needs one number owned by neither chain
// and saved in neither block.
//
// THAT LAST SENTENCE IS NOW THE GRAPH'S DESIGN, and it stops being worse the moment there is one
// owner: D1 (2026-09-01) puts the counter on the top-level host and hands it down, so the graph's
// two chains share one sequence and no route needs a kind to say which chain it means. The
// per-chain counters here stand until steps 3 and 4 move Feathers' chains onto the graph.
//
//------------------------------------------------------------------------------------------------//
// AND THAT DAY IS TODAY (2026-09-10) — ONE SEQUENCE, AND THE DEFAULTS SIT AT ITS HEAD.              //
//------------------------------------------------------------------------------------------------//
//
// The per-chain counters are gone: `ParamBridge::nextInstance_` is the one owner, and each block's
// `nextInstanceId` field is now TRANSPORT — seeded from it, folded back into it, and adopted as a
// MAXIMUM on restore because a state load restores both chains and whichever ran second would
// otherwise wind the counter back over the first.
//
// **WHAT FORCED IT WAS NOT TIDINESS.** `backplane::Graph` keys its nodes on `DeviceInstanceId`, so
// two devices under one id is two nodes at one address and `Graph::restore` refuses the second
// outright. A fixture handed Feathers' real rows could not build a graph at all
// (`tests/test_feathers_graph_order.cpp`), which is how this was found — measured, not predicted.
//
// **THE DEFAULTS ARE THE PART THAT IS EASY TO MISS.** `defaultModRack()` and `defaultFxChain()` each
// stamped `kFirstInstance` on their one device, so the collision was not something a gesture had to
// reach — it was every session's STARTING state, and `restoreFxChain(defaultFxChain())` reintroduced
// it on any load of a patch with no fxchain block. The two constants below are what stop that, by
// giving each default its own place in the one sequence rather than each starting over at 1.

namespace feathers {

/// **THE HEAD OF THE ONE SEQUENCE, SPELT OUT SO NEITHER DEFAULT HAS TO GUESS.** A default chain
/// builds its device by writing the row rather than by drawing from a counter — it has none — so
/// these are where its serial comes from. Change them together or not at all.
inline constexpr DeviceInstanceId kDefaultRackInstance = kFirstInstance;
inline constexpr DeviceInstanceId kDefaultFxInstance = kFirstInstance + 1;
/// The first serial NOT spoken for by a default, and where a fresh `ParamBridge` starts counting.
inline constexpr DeviceInstanceId kFirstFreeInstance = kFirstInstance + 2;

static_assert(kDefaultRackInstance != kDefaultFxInstance,
              "the two default chains must not mint the same serial — Graph::restore refuses the "
              "second node under an id the first already holds");
static_assert(kFirstFreeInstance > kDefaultRackInstance && kFirstFreeInstance > kDefaultFxInstance,
              "a fresh counter must start past every serial the defaults occupy");

} // namespace feathers
