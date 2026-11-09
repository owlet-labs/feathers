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

#include <cstdint>

// WHAT NAMES ONE DEVICE INSTANCE — the identity every route, every undo record and every saved
// patch keys on.
//
// IT LIVES IN THE BACKPLANE BECAUSE THE GRAPH NEEDS IT AND THE GRAPH MAY NOT INCLUDE THE PLUGIN.
// It was declared in `plugin/device_instance.h`, which was right while Feathers' two chains were
// the only things that held devices; the graph in `backplane/graph.h` is host-neutral by
// measurement — nothing under `src/backplane/` includes `plugin/` — and the vision doc's D1 rules
// that a graph node's identity IS this type, not a second integer beside it. So the three
// declarations moved down a layer, unrenamed, and the plugin header includes this one. Every
// existing includer compiles unchanged.
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
// WHO COUNTS. Today each of Feathers' chains counts separately from `kFirstInstance`, and a stored
// route disambiguates by carrying a distinct KIND for an fx device and a rack device — the
// arrangement `plugin/device_instance.h` records. D1 (2026-09-01) rules the other way for the
// graph: ONE counter, owned by the top-level host and handed down to nested containers, so that a
// nested rack's devices draw from the same sequence and one integer stays one integer
// (`backplane::InstanceCounter`). The two arrangements coexist until steps 3 and 4 move Feathers'
// chains onto the graph, at which point the per-chain counters and the route kind they required
// both retire.

// **THE THREE DECLARATIONS ARE IN `device/instance_id.h` SINCE KILL-LANES STEP 2** — a widget speaks a
// route's endpoints, an endpoint names a device by this, and the device vocabulary may include nothing
// from here. This header keeps the argument above and forwards.
#include "device/instance_id.h"
