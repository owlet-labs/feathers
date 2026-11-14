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

#include "backplane/axes.h"
#include "backplane/routing.h"
#include "backplane/device_instance_id.h"
#include "backplane/route_list.h"
#include "device/registry.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <optional>
#include <vector>

// THE CONTAINER IS A GRAPH — nodes are device instances, edges are typed, and a chain is a path.
//
// Backlog W5: a graph data structure, since a modular env is on the distant roadmap; and on the two
// chains Feathers will stand on (D2), chains are trivial graphs and should be built as such. This is that structure, with nothing on it yet:
// step 1 of the vision doc's path lands it beside the code it will replace, and no host walks it
// until step 2.
//
//------------------------------------------------------------------------------------------------//
// WHAT A NODE IS, AND WHAT NAMES IT                                                                //
//------------------------------------------------------------------------------------------------//
//
// A node is one device instance: a descriptor and an identity. The identity is the existing
// `DeviceInstanceId` (D1) — a monotonic serial that is never reissued while the graph lives — and
// the counter that issues it is OWNED BY THE TOP-LEVEL HOST and handed to every graph it builds, so
// that a nested container's devices draw from the same sequence and one integer stays one integer.
//
// A NODE'S PLACE IN THE POOL IS ITS ROUTING LANE. The pool has `axes.deviceSlots` slots because that
// is how many lanes the route table has, and a node holds its slot for as long as it is live. That
// is the whole of Stage 2's trick, kept: the lane is handed out when the device is added and moves
// with nothing, so a reorder touches the table not at all. The lane is ALSO the index a host should
// keep its own per-node state under — the graph holds no device storage, no settings row, no
// readings, and it destroys nothing. A field on a node is earned by a reader, and the first reader is
// step 2.
//
// **THE LANE IS AN ADDRESS AND THE ID IS AN IDENTITY, AND THEY MUST NOT BE CONFUSED.** A lane is
// reused the moment its node is removed; an id never is. Every public entry point here takes an id.
// The lane is handed out for exactly two purposes: indexing the table, and indexing whatever the
// host keeps beside it.
//
//------------------------------------------------------------------------------------------------//
// WHAT AN EDGE IS — THREE KINDS, AND ONE OF THEM IS THE ROUTE TABLE                                //
//------------------------------------------------------------------------------------------------//
//
// The device vocabulary already carries exactly three things between devices — audio, modulation and
// notes — and declares each with a table whose null IS the capability (`AudioOps`, `ModOps`,
// `NoteOps`). An edge is typed by construction to one of the three, and an edge of a kind that
// either endpoint does not declare CANNOT BE CONSTRUCTED: there is no flag to check because there is
// no flag, only the pointer.
//
//   Audio and Notes edges are explicit, held in a fixed pool. Today a chain implies them; they exist
//   as their own thing so that the modular environment has somewhere to put an edge a chain does not
//   imply.
//
//   A MODULATION EDGE IS A CELL OF THE ROUTE TABLE, and this graph OWNS that table. `RouteTable`
//   already stores exactly (source, destination, amount) keyed by lane, the audio thread already
//   reads it under its own atomic contract, and every surface already draws from it. A second store
//   of the same fact would be the two-lists-must-agree shape this repo has a rule about. So `route()`
//   is the typed constructor for a modulation edge, and its refusal is what keeps the table honest
//   about which devices are modulators — AND about which way modulation flows, below.
//
//------------------------------------------------------------------------------------------------//
// WHAT A CHAIN IS — A PATH, NOT A CONTAINER                                                        //
//------------------------------------------------------------------------------------------------//
//
// A chain is an ordered path of node ids over the shared pool. The graph owns a fixed number of them
// (a runtime number, at construction), so two chains over one graph is the ordinary case and not a
// feature: Feathers' strip and lane are `chain(0)` and `chain(1)`, and a modulator in one reaching a
// control in the other is an ordinary cell of the one table. A node sits in at most one chain.
//
// **A CHAIN IS A PATH PER CAPABILITY.** Along the path, audio flows between the members that carry
// audio, notes between the members that carry notes, and a member that carries neither is passed
// over — which is the note lane's own rule, *"a device with no NoteOps is never consulted and the
// notes reaching it pass through untouched"*, applied to all three roads. So the precedence a chain
// imposes is: consecutive AUDIO members are ordered and consecutive NOTE members are ordered, both
// between their AUDIO instants (below). **Consecutive MODULATOR members are NOT ordered, since backlog
// IZ** — they were, and that made a modulator's slot in a lane a routing rule. A modulator sitting
// between two effects constrains neither, and floats to the front of the order, which is where §7.3
// of the modops doc needs it.
//
//------------------------------------------------------------------------------------------------//
// THE TWO INSTANTS — MOD BEFORE AUDIO (backlog IZ)                                 //
//------------------------------------------------------------------------------------------------//
//
// By design, for modulation "downstream" means COMPUTED FROM DOWNSTREAM AUDIO, and a modulator's position
// in a lane is only grouping. So every device has two precedence instants, and the relations are stated
// between instants rather than between devices:
//
//   - a device's MOD instant (tick, publish; every output is written there, every control is read
//     there — conservatively) PRECEDES its AUDIO instant (its audio and note roads), unless it declares
//     `ModOps::readsAudio` (or its row does, `readsAudioOf`: the Follower in Audio), when it FOLLOWS it.
//   - chain adjacency (audio, notes), explicit edges, and "the host's node precedes each chain's audio
//     head" link AUDIO instants.
//   - a route links the source's MOD instant to the destination's MOD instant.
//
// That is what already executes: `RackEngine::beginPiece` runs notes, then `advanceModulators`, then
// the audio walk, and Feathers ticks the whole engine's modulators before the voice reads a param. So
// SKLITTER's perform output reaching an fx BEFORE it, or the voice, is read in the same tick it is
// published — the graph used to refuse it only because it had one instant per device.
//
// **THE ORDER STAYS ONE ORDER OF DEVICES**, because a host walks one order twice (below). An instant
// precedence binds the two devices' places in it unless the passes already settle it — out of the
// modulators' pass into the second (`bindsOrder`). A route is refused if it would close a cycle among
// the instants OR in that device order (`routeWouldCycle`); the second is new, and exists because two
// SKLITTERs routed into each other's controls against lane order are acyclic as instants yet cannot be
// ticked in one order and run in the other.
//
//------------------------------------------------------------------------------------------------//
// THE ORDER — TOPOLOGICAL, MODULATORS FIRST, AND MODULATION IS PASS-THROUGH                         //
//------------------------------------------------------------------------------------------------//
//
// `order()` is a topological order over the explicit edges, the chain-derived precedences above, AND
// EVERY NON-ZERO CELL OF THE ROUTE TABLE, recomputed on every mutation and refused (the mutation is
// rolled back, or the cell never written) if it would close a cycle. Among the nodes ready at each
// step, the one earliest in its chain is taken (chain index, then position; a node in no chain last, by id), so
// the order is deterministic.
//
// **MODULATION IS PASS-THROUGH, BY DESIGN.** A device may be modulated only by sources UPSTREAM of it, exactly as it may only receive
// audio and notes from upstream — where, since backlog IZ, "upstream" is asked of the instants above.
// So a modulation route IS a precedence, and a route that would close
// a cycle — including a device routed to itself — is refused by `route()` before the cell is
// written, the same way `connect` refuses an audio edge. The first version of this file did the
// opposite on purpose (a one-block-delayed loop was "legitimate and always had been"); that is the
// silent ordering subtlety pass-through removes, and the one patch it forecloses — a follower on the
// output modulating the oscillator upstream of it — is, if ever wanted, an explicit and visibly
// delayed feedback device, not a table that permits loops.
//
// WHAT THIS DOES NOT CHANGE: the table's storage and its atomic contract; and the source list a
// surface offers, which is the CONSUMER'S job to filter by position — a card asking "may this source
// reach me" asks `mayRoute` for liveness and this graph for precedence, and a picker that offers a
// downstream source is offering a route `route()` will refuse. Feathers' present one-block-delayed
// loops stop being expressible only when its chains are on this graph (step 5).
//
//------------------------------------------------------------------------------------------------//
// WHICH VIEW A HOST WALKS — `chain(c)` FOR A CHAIN HOST, `order()` FOR A GRAPH HOST                //
//------------------------------------------------------------------------------------------------//
//
// Asked by the step-2 adopter before assuming, because the wrong answer is a silent wrong-order bug
// rather than a crash. The answer is BOTH exist for different hosts, and a chain host needs only one.
//
// **`order()` MAKES NO CONTIGUITY PROMISE.** It is one topological order over EVERY live node —
// every chain's members and the detached ones — and two chains' audio members can interleave in it,
// because nothing orders chain 0's path against chain 1's. Walking it with one buffer per chain would
// bounce between buffers. It is the view for a host that runs ONE pass over a general DAG with
// explicit edges, which is the modular environment and not a chain host.
//
// **AND THE CHAIN HOSTS HERE WALK `order()` AFTER ALL (kill-lanes 4c, 2026-09-13).** The record writes it
// into the `ChainState` it publishes and the converger runs the engine in it, because the two-pass walk
// below agreed with this order only while nothing was routed backwards across two chains — and nothing
// orders two chains against each other, so such a route is legal. The contiguity point above stands for
// the day there are two AUDIO buffers: then the sort's tie-break must keep one chain's audio members
// together, which is a constraint on `recompute`, not a reason to read a different view.
//
// **A CHAIN HOST WALKS `chain(c)` FILTERED BY CAPABILITY, IN TWO PASSES:** first every chain's
// members with `mod` ops, in path order, ticked; then each chain's members with `audio` ops, in path
// order, run through that chain's buffer; notes likewise along each chain. That is a valid
// linearisation of everything `order()` enforces, BY CONSTRUCTION AND NOT BY COINCIDENCE: chain
// precedence is per capability (consecutive audio members, consecutive mod members), and there is
// no cross-chain precedence at all unless an EXPLICIT edge or a ROUTE crosses chains — which no
// chain host makes on its own. A device that is both a modulator and an audio device is ticked in
// the first pass and run in the second, which is exactly where `order()` would put its two roles.
// The day a host consumes explicit edges, it walks `order()` and the per-chain buffers become the
// DAG's edges; until then `order()` is the assertion the tests read and `chain(c)` is what the block
// walks.
//
// **(SUPERSEDED IN PART BY BACKLOG IZ: a route may now run from a modulator into one to its LEFT, so the
// mod pass must walk `order()`, not the path — `RackEngine::publishOrder` does, and every product road
// converges the engine to a record's `order()` anyway. The paragraph below is the pre-IZ argument.)**
//
// **PASS-THROUGH CHANGES NOTHING IN THAT WALK.** A route is admitted only from a source upstream of
// its destination, so the mod pass order is the chain order restricted to upstream-of-destination —
// which is the chain order — and every source in the mod pass precedes every consumer in the audio
// pass because the passes are sequential. The one shape the two-pass walk does not reproduce is a
// route from a modulator in chain 1 to a modulator in chain 0 (admissible, since it closes no cycle,
// but pass 1 walks chain 0 first): a host with more than one chain either walks `order()`'s mod
// tier for pass 1, or ranks its chains so cross-chain routes only run forward. Feathers does the first:
// its engine holds ONE chain, and the converger places both record chains' devices into it in the record's
// `order()`, routes included, so a lane modulator routed into a strip modulator is ticked before it
// (backlog LN, measured 2026-10-10; test_mod_rack's "LN:" cases, whose known-bad is the converger ignoring
// that order). Only a state with no order — the by-value fixture road — still runs the chains in index
// order, and a lane modulator into a strip one is a tick late there.
//
// **`chain(c)` IS AS STABLE AS `order()`, WHICH IS NOT VERY.** It is a span into the chain's own
// storage, rewritten by `insert`, `erase` and `move` on THAT chain and by `remove` of one of its
// members — other chains' mutators leave it alone, but `recompute()` runs on every mutator and the
// audio thread cannot know which chain a message-thread edit is touching. So a host snapshots it
// under the same discipline as `order()`: publish a copy per block epoch, and read the descriptor
// through the host's own atomic per lane (`RackEngine::Slot::desc` is the pattern), never through
// `nodeAtLane(lane).desc`, which is a plain pointer the message thread overwrites.
//
//------------------------------------------------------------------------------------------------//
// THREADS                                                                                         //
//------------------------------------------------------------------------------------------------//
//
// EVERY MUTATOR IS MESSAGE THREAD. The graph allocates at construction — pool, chains, edge store,
// table, order — and never afterwards (D4: growth is a deactivate-and-resize, and there is a case
// with a counting allocator under that sentence). It destroys nothing, because it owns nothing that
// needs destroying: a device's storage is the host's, built and retired through the host's own pool.
//
// `routes()` carries the table's own audio-thread contract. `order()` and `slots()` are views into
// storage the message thread rewrites, so a host that walks them on the audio thread publishes them
// its own way — `RackEngine`'s block epoch is the pattern — which is step 2's decision and not this
// file's.
//
// **AND LANE REUSE RACING AN IN-FLIGHT BLOCK IS THE HOST'S RETIREMENT PROBLEM, NOT THE GRAPH'S.**
// `remove()` frees a lane the instant it returns, and `add()` may hand that lane out on the next
// call; the graph knows nothing about a block that is still reading the old device's storage under
// that lane. The host owns the storage, so the host owns the hazard: retire the instance through
// `DeviceInstancePool` (or the rack's parked-slot road), and do not reuse the lane's per-node state
// until the retirement has drained. That is the discipline Stage 1 paid for and this file relies on
// rather than restates.

namespace feathers::backplane {

/// THE ONE COUNTER (D1). Owned by the top-level host, handed by reference to every graph it builds,
/// so a nested container's devices draw from the same sequence.
///
/// `reserve` exists for a loader and an undo stack, which put back a device under the id it had; the
/// counter must then be past it, or the next `add` collides with a restored node.
class InstanceCounter {
public:
	/// THE TOP OF THE ID SPACE, which is never issued: the host's own core node stands under it, and one
	/// past it would wrap to `kNoInstance`.
	static constexpr DeviceInstanceId kCeiling = 0xFFFFFFFFu;

	/// A fresh id, or `kNoInstance` once the space below `kCeiling` is spent (four billion devices).
	DeviceInstanceId issue() noexcept {
		if (next_ >= kCeiling) {
			return kNoInstance;
		}
		return next_++;
	}

	/// Move the counter past `id`. An id at or above `kCeiling` moves it to the ceiling and no further, so it
	/// can neither wrap to `kNoInstance` nor hand out the core node's id.
	void reserve(DeviceInstanceId id) noexcept {
		if (id >= next_) {
			next_ = id >= kCeiling ? kCeiling : id + 1;
		}
	}

	DeviceInstanceId next() const noexcept { return next_; }

private:
	DeviceInstanceId next_ = kFirstInstance;
};

/// WHAT AN EDGE CARRIES. Each kind is exactly one of the device vocabulary's three ops tables.
enum class EdgeKind : uint8_t { Audio, Modulation, Notes };

/// A CHAIN THAT IS NOT A CHAIN, and a position that is not one. What `chainOf` and `positionOf`
/// answer for a node that sits in no path.
inline constexpr size_t kNoChain = static_cast<size_t>(-1);
inline constexpr size_t kNoPosition = static_cast<size_t>(-1);

/// ONE NODE, as the graph sees it.
struct Node {
	DeviceInstanceId id = kNoInstance;
	const device::Descriptor* desc = nullptr;
	/// Which chain's path holds it, or `kNoChain`.
	size_t chain = kNoChain;
	/// **ITS MOD ROAD READS ITS AUDIO, AS ITS ROW SAYS NOW** (backlog OP) — the descriptor's static `readsAudio` when
	/// placed, then whatever the host last told `Graph::setReadsAudio` for a device whose row decides it.
	bool readsAudio = false;

	bool live() const noexcept { return id != kNoInstance; }
};

/// ONE STEP OF THE EVALUATION ORDER — the id for a reader that reasons, the lane for a reader that
/// indexes. Both are read in the same pass and so are contemporaneous by construction.
struct Step {
	DeviceInstanceId id = kNoInstance;
	size_t lane = static_cast<size_t>(-1); // Graph::kNoLane

	friend bool operator==(const Step&, const Step&) = default;
};

class Graph {
public:
	static constexpr size_t kNoLane = static_cast<size_t>(-1);
	static constexpr size_t kNoChain = backplane::kNoChain;
	static constexpr size_t kNoPosition = backplane::kNoPosition;

	/// MESSAGE THREAD. Sizes everything from the axes, the chain count and the explicit-edge capacity,
	/// and allocates once. `ids` must outlive the graph.
	///
	/// **THE POOL IS ONE LANE LONGER THAN THE AXES' DEVICE SLOTS (kill-lanes step 4a), AND THAT LANE IS
	/// NOT A DEVICE SLOT.** `kCoreLane` is reserved for the HOST'S OWN NODE — Feathers' voice — which a
	/// host declares with `setCoreNode`. It sits outside `axes.deviceSlots` deliberately: that number is
	/// still the surface's coordinate space until step 5, so growing it would renumber every coordinate
	/// in the tree to make room for a node no coordinate names. The allocator never hands it out
	/// (`lowestFreeLane` stops at the device slots) and no chain holds it.
	Graph(const Axes& axes, size_t chains, size_t edgeCapacity, InstanceCounter& ids)
	    : axes_(axes), ids_(&ids), nodes_(axes.deviceSlots + 1),
	      descAtLane_(axes.deviceSlots + 1, nullptr), chains_(chains), edges_(edgeCapacity),
	      order_(axes.deviceSlots + 1), rankAtLane_(axes.deviceSlots + 1, kNoLane),
	      indeg_(axes.deviceSlots + 1, 0), emitted_(axes.deviceSlots + 1, 0), orderKey_(axes.deviceSlots + 1, 0),
	      adj_(4 * (axes.deviceSlots + 1) * (axes.deviceSlots + 1), 0),
	      orderAdj_((axes.deviceSlots + 1) * (axes.deviceSlots + 1), 0),
	      instantIndeg_(2 * (axes.deviceSlots + 1), 0), instantDone_(2 * (axes.deviceSlots + 1), 0),
	      coreLane_(axes.deviceSlots), seen_(2 * (axes.deviceSlots + 1), 0), stack_(2 * (axes.deviceSlots + 1), 0) {
		for (ChainPath& chain : chains_) {
			chain.ids.assign(axes.deviceSlots, kNoInstance);
		}
	}

	Graph(const Graph&) = delete;
	Graph& operator=(const Graph&) = delete;
	/// MOVABLE, so a record can REPLACE its graph with a bigger one (unbounded devices step 5) — the message thread's
	/// road; the engine, whose graph the drain mutates, swaps a pointer instead.
	Graph(Graph&&) noexcept = default;
	Graph& operator=(Graph&&) noexcept = default;

	const Axes& axes() const noexcept { return axes_; }
	/// HOW MANY DEVICE SLOTS — NOT counting the reserved core lane, which is not a device slot and
	/// which no caller may place into (see the constructor).
	size_t capacity() const noexcept { return coreLane_; }

	/// **THIS GRAPH TAKES ANOTHER'S NODES, CHAINS AND EDGES, INTO ROOM IT ALREADY HAS — NO ALLOCATION**
	/// (unbounded devices step 4). The rack engine's graph is mutated at the drain, so a bigger one is built
	/// on the message thread and adopted there: this is the copy that makes the adoption a move. Every device
	/// keeps its lane (the engine keeps its per-node state under it); the host's own node moves to THIS
	/// graph's core lane, which is past every device lane by construction; the order and every derived table
	/// are recomputed. Refused (false, nothing promised) for a graph this one cannot hold: other chains, more
	/// lanes, more edges.
	bool assignGrown(const Graph& from) noexcept {
		if (from.chains_.size() != chains_.size() || from.coreLane_ > coreLane_ || from.edgeCount_ > edges_.size()) {
			return false;
		}
		for (size_t c = 0; c < chains_.size(); ++c) {
			if (from.chains_[c].count > chains_[c].ids.size()) {
				return false;
			}
		}
		for (size_t lane = 0; lane < nodes_.size(); ++lane) {
			nodes_[lane] = Node {};
			descAtLane_[lane] = nullptr;
		}
		for (size_t lane = 0; lane < from.coreLane_; ++lane) {
			nodes_[lane] = from.nodes_[lane];
			descAtLane_[lane] = from.descAtLane_[lane];
		}
		nodes_[coreLane_] = from.nodes_[from.coreLane_];
		descAtLane_[coreLane_] = from.descAtLane_[from.coreLane_];
		nodeCount_ = from.nodeCount_;
		for (size_t c = 0; c < chains_.size(); ++c) {
			const ChainPath& src = from.chains_[c];
			std::fill(chains_[c].ids.begin(), chains_[c].ids.end(), kNoInstance);
			std::copy_n(src.ids.begin(), src.count, chains_[c].ids.begin());
			chains_[c].count = src.count;
		}
		std::copy_n(from.edges_.begin(), from.edgeCount_, edges_.begin());
		edgeCount_ = from.edgeCount_;
		return recompute();
	}
	size_t nodeCount() const noexcept { return nodeCount_; }
	size_t chainCount() const noexcept { return chains_.size(); }
	size_t edgeCapacity() const noexcept { return edges_.size(); }

	/// **ROOM FOR ONE MORE EDGE — ALLOCATES** (backlog OJ: no route is refused for room). A full edge store grows
	/// by x1.5 (at least 16), called by every door that ADDS an edge (`route`, `connect`, `plantRouteForTest`).
	/// So those doors allocate, and that is legal only where they are called: the record's, on the message
	/// thread; the engine's, by `RackEngine::setRouteAmount` under its no-block-in-flight contract. A DRAIN NEVER
	/// ADDS AN EDGE (it removes nodes and adopts a bigger graph by `assignGrown`, both moves), which is what keeps
	/// it allocation-free. Never shrinks.
	void makeEdgeRoom() {
		if (edgeCount_ < edges_.size()) {
			return;
		}
		edges_.resize(std::max<size_t>(16, edges_.size() + edges_.size() / 2));
		++edgeGrowths_;
	}
	/// How many times `makeEdgeRoom` reallocated — a case counts it against log1.5 of the routes.
	size_t edgeGrowthsForTest() const noexcept { return edgeGrowths_; }

	/// **HOW MANY EDGES OF ONE KIND** — and it exists because `edgeCount()`'s meaning widened.
	///
	/// A modulation route IS an edge now, so the bare count no longer answers *"was an audio edge
	/// made?"* — which is what two cases in `test_backplane_graph.cpp` were asking it. Asking by kind
	/// is both what they meant and STRONGER than what they had: the old count could only conflate
	/// audio with notes because routes were not in the pool to conflate with.
	size_t edgeCount(EdgeKind kind) const noexcept {
		size_t n = 0;
		for (size_t i = 0; i < edgeCount_; ++i) {
			if (edges_[i].kind == kind) {
				++n;
			}
		}
		return n;
	}
	size_t edgeCount() const noexcept { return edgeCount_; }

	//--------------------------------------------------------------------------------------------//
	// NODES                                                                                        //
	//--------------------------------------------------------------------------------------------//

	/// Put a device in the lowest free lane under a fresh id, in no chain. `kNoInstance` when the pool
	/// is full — the pool REFUSES rather than grows, and that refusal is the D4 contract.
	DeviceInstanceId add(const device::Descriptor& desc) {
		const size_t lane = lowestFreeLane();
		if (lane == kNoLane) {
			return kNoInstance;
		}
		const DeviceInstanceId id = ids_->issue();
		if (id == kNoInstance) {
			return kNoInstance; // the counter is spent
		}
		place(lane, id, desc);
		recompute();
		return id;
	}

	/// PUT A DEVICE BACK UNDER THE IDENTITY AND LANE IT HAD — for a loader and an undo stack, and for
	/// nothing else. Refused when the lane is taken or out of range, the id is `kNoInstance`, or a
	/// node already carries that id. The counter is moved past the id so a later `add` cannot collide.
	///
	/// The routes the node had are NOT restored here: they were cleared when its lane was freed, and
	/// they come back through `writeSliceOfDeviceSlot` on `routes()`, which is what the slice exists
	/// for. Two separate acts because they are undone separately too.
	bool restore(const device::Descriptor& desc, DeviceInstanceId id, size_t lane) {
		if (id == kNoInstance || id >= InstanceCounter::kCeiling || lane >= coreLane_ || nodes_[lane].live()
		    || laneOf(id) != kNoLane) {
			return false;
		}
		ids_->reserve(id);
		place(lane, id, desc);
		recompute();
		return true;
	}

	//--------------------------------------------------------------------------------------------//
	// THE HOST'S OWN NODE (kill-lanes step 4a)                                                     //
	//--------------------------------------------------------------------------------------------//

	/// **DECLARE THE HOST ITSELF AS A NODE — Feathers' voice.** Without this a host with two chains is TWO DISCONNECTED COMPONENTS: nothing
	/// joins a strip modulator to an fx device, so a topological sort may emit them in any interleaving
	/// and the only thing that kept modulators first was a tie-break inside `recompute` — the band rule
	/// hiding in the sort. With it, the real relations appear and the tie-break is not needed:
	///
	///   - **a route into a CORE DESTINATION is a precedence into this node** (a modulator reaching a
	///     voice parameter must be computed before the voice reads it), and
	///   - **this node precedes the first AUDIO-carrying device of every chain** (the voice's output is
	///     what an audio chain is a chain of).
	///
	/// **AND IT CLOSES A HOLE IN THE CYCLE RULE RATHER THAN ADDING ONE.** `route()` refused a cycle only
	/// when BOTH ends were devices, so every route pointing at a voice parameter escaped the check
	/// entirely — the rule: modulation is pass-through, a cycle
	/// is refused, a delayed loop is not expressible) simply did not reach them. It does now.
	///
	/// A HOST WITH NO CORE NODE (the standalone rack, whose macros are inputs and whose core
	/// destinations are none) never calls this, and every rule above is inert for it.
	///
	/// Refused if a core node is already set. The node stands under the graph's OWN descriptor rather
	/// than one a host supplies, so nobody can hand it a real device and have it treated as the host.
	/// **THE ID THE HOST'S OWN NODE STANDS UNDER.** Not issued from the host's counter and never
	/// reserved on it: the node is not a device, nothing addresses it by id, and taking a serial for it
	/// would shift every device's id by one for a node no file or route names. A counter that issues
	/// from `kFirstInstance` upward would need four billion devices to reach this.
	static constexpr DeviceInstanceId kCoreInstance = InstanceCounter::kCeiling;

	bool setCoreNode() {
		if (nodes_[coreLane_].live()) {
			return false;
		}
		place(coreLane_, kCoreInstance, coreDescriptor());
		recompute();
		return true;
	}

	/// THE HOST'S OWN NODE, or `kNoInstance` for a host that declared none.
	[[nodiscard]] DeviceInstanceId coreNode() const noexcept {
		return nodes_[coreLane_].live() ? nodes_[coreLane_].id : kNoInstance;
	}

	/// TAKE A NODE OUT: out of its chain, out of every edge — its routes included, whichever end it was
	/// — and out of the pool. Its lane is free again; its id is never reissued.
	///
	/// **ITS ROUTES GO WITH IT, AND THAT IS THE ABA GUARD.** A device arriving pre-modulated by routes
	/// nobody drew was the hazard of a lane-keyed table (a lane handed on still carried amounts, so a
	/// table slice had to be cleared); a route names its device's id now, and the edges naming this one
	/// are removed in the same walk as its audio edges, so nothing can outlive it to be inherited.
	bool remove(DeviceInstanceId id) {
		const size_t lane = laneOf(id);
		if (lane == kNoLane) {
			return false;
		}
		if (nodes_[lane].chain != kNoChain) {
			eraseFromChain(nodes_[lane].chain, id);
		}
		for (size_t i = 0; i < edgeCount_;) {
			if (edges_[i].from == id || edges_[i].to == id) {
				edges_[i] = edges_[--edgeCount_];
			}
			else {
				++i;
			}
		}
		nodes_[lane] = Node {};
		descAtLane_[lane] = nullptr;
		--nodeCount_;
		recompute();
		return true;
	}

	/// The lane a live node holds, or `kNoLane`. LINEAR IN THE POOL, and that is fine where it is
	/// called: every caller is a message-thread mutator, and a host on the audio thread walks
	/// `order()`, which carries the lane already.
	size_t laneOf(DeviceInstanceId id) const noexcept {
		if (id == kNoInstance) {
			return kNoLane;
		}
		for (size_t lane = 0; lane < nodes_.size(); ++lane) {
			if (nodes_[lane].id == id) {
				return lane;
			}
		}
		return kNoLane;
	}

	/// The node under an id, or nullptr.
	const Node* nodeOf(DeviceInstanceId id) const noexcept {
		const size_t lane = laneOf(id);
		return lane == kNoLane ? nullptr : &nodes_[lane];
	}

	/// The node in a lane, live or not. Out of range answers an empty node rather than a fault.
	const Node& nodeAtLane(size_t lane) const noexcept {
		static const Node kEmpty {};
		return lane < nodes_.size() ? nodes_[lane] : kEmpty;
	}

	/// THE DESCRIPTOR IN EVERY LANE, as the routing authority wants it. Handing this to
	/// `backplane::mayRoute` is what makes the existing authority the graph's authority too, rather
	/// than a second one growing beside it.
	/// **DEVICE SLOTS ONLY** — the reserved core lane is not one, and a caller indexing a slot view by a
	/// coordinate must not find a node no coordinate names.
	/// **DESCRIPTORS ONLY SINCE STEP 5.** It handed the rank table across as well, for the `mayRoute`
	/// that compared two ranks; that overload went in 4b and `SlotView`'s rank half went with it. The
	/// graph's own order is still the graph's — `order()`, `rankAtLane()`, `rankOf()` — and it is what
	/// the engine RUNS in. What it is not is a routing rule.
	SlotView slots() const noexcept {
		return SlotView(std::span<const device::Descriptor* const>(descAtLane_.data(), coreLane_));
	}

	/// WHERE THE DEVICE AT `lane` STANDS IN `order()`, or `kNoLane` for a lane holding nothing —
	/// the number `mayRoute` compares, exposed for a host that mirrors it (the rack's bridge does).
	size_t rankAtLane(size_t lane) const noexcept {
		return lane < rankAtLane_.size() ? rankAtLane_[lane] : kNoLane;
	}
	/// WHERE THE DEVICE `id` STANDS IN `order()`, or `kNoLane` — `rankAtLane` asked by identity, which is
	/// how the routing authority asks now (kill-lanes step 2).
	size_t rankOf(DeviceInstanceId id) const noexcept { return rankAtLane(laneOf(id)); }

	/// **WOULD A ROUTE FROM `s` TO `d` CLOSE A CYCLE? THE ROUTING RULE, AND SINCE STEP 4B THE WHOLE OF
	/// IT.** The design rule is that modulation is pass-through
	/// and a route that would have to be read before it is written is not a route. This is that question
	/// asked directly — of the PRECEDENCES, which is where the answer lives — rather than through a
	/// comparison of two ranks, which is an answer about one arbitrary linearisation. See `mayRoute`.
	///
	/// A host end resolves to the host's own node (`precedenceLaneOf`), so a route into a voice
	/// parameter is under the rule like any other.
	///
	/// **EXCEPT A SELF-ROUTE THE DEVICE DECLARED** (backlog HS, `device::SelfRouting`): a device whose
	/// outputs are published before the control in question is read has said that THIS pair is not read
	/// before it is written, which is the question. No declaration, the answer above, unchanged.
	///
	/// **ASKED OF TWO INSTANTS PER DEVICE SINCE BACKLOG IZ** — see `routeWouldCycle`, which `route()`
	/// asks too, and "THE TWO INSTANTS" in the file's header for why.
	[[nodiscard]] bool wouldCycle(Source s, Destination d) const noexcept {
		return !declaredSelfRoute(s, d) && routeWouldCycle(precedenceLaneOf(s), precedenceLaneOf(d));
	}

	/// **THE ORDER RELATION ITSELF: must the device at `from` come before the device at `to` in the one
	/// order a host walks?** (Since backlog IZ this is the DEVICE relation `order()` linearises, not the
	/// instant relation a route's legality is asked of first — see `routeWouldCycle`.)
	///
	/// **IT IS PUBLIC SO A TEST CAN ASK THE RELATION DIRECTLY** — the graph-order suite pins the gate
	/// (`mayRoute`) against it pair for pair. It was public first so a host's own COPY of the relation
	/// (`Synth::compileReach`, the audio thread's reach mask) could be shown to agree with this one; that
	/// copy is deleted (kill-lanes §2.4b — a node pulls its inputs, and legality is asked here, once, at
	/// the edit). **MEASURED 2026-09-12, while the copy existed: chain adjacency deleted from
	/// `forEachPrecedence` put four suites red and left every synth and mod-rack case green** — two
	/// restatements of one rule, unpinned in both directions, which is the shape that argued for one.
	///
	/// `from == to` IS FALSE, because a device is not upstream of itself — the same answer `wouldCycle`
	/// leans on, and the reason a node's bus does not carry its own output.
	///
	/// MESSAGE THREAD ONLY, like every other read of this graph: the scratch behind it is `mutable`
	/// and this is not reentrant. It is a test-and-proof road, not a per-block one.
	[[nodiscard]] bool reachesLane(size_t from, size_t to) const noexcept {
		return from != to && reaches(orderAdj_, nodes_.size(), from, to);
	}

	/// **WHAT EACH DEVICE IS AND WHERE IT STANDS, BY ID** — the view the routing authority asks
	/// (`backplane::mayRoute`), over this graph. Valid while the graph is.
	DeviceView devices() const noexcept {
		return { this,
			     [](const void* g, DeviceInstanceId id) -> const device::Descriptor* {
				     const Node* n = static_cast<const Graph*>(g)->nodeOf(id);
				     return n != nullptr ? n->desc : nullptr;
			     },
			     [](const void* g, Source s, Destination d) {
				     return static_cast<const Graph*>(g)->wouldCycle(s, d);
			     } };
	}

	//--------------------------------------------------------------------------------------------//
	// CHAINS                                                                                      //
	//--------------------------------------------------------------------------------------------//

	/// The path of one chain, in order. Empty for a chain index out of range.
	std::span<const DeviceInstanceId> chain(size_t chain) const noexcept {
		if (chain >= chains_.size()) {
			return {};
		}
		return std::span<const DeviceInstanceId>(chains_[chain].ids).first(chains_[chain].count);
	}

	/// Which chain holds a node, or `kNoChain`.
	size_t chainOf(DeviceInstanceId id) const noexcept {
		const Node* node = nodeOf(id);
		return node == nullptr ? kNoChain : node->chain;
	}

	/// Where in its chain a node sits, or `kNoPosition`.
	size_t positionOf(DeviceInstanceId id) const noexcept {
		const size_t c = chainOf(id);
		if (c == kNoChain) {
			return kNoPosition;
		}
		const ChainPath& path = chains_[c];
		for (size_t i = 0; i < path.count; ++i) {
			if (path.ids[i] == id) {
				return i;
			}
		}
		return kNoPosition;
	}

	/// PUT A LIVE NODE INTO A CHAIN at `position` (clamped to the end). Refused for a node that is
	/// already in a chain — a node sits in at most one — and for a placement that would close a cycle
	/// against the explicit edges, in which case nothing changes.
	bool insert(size_t chain, size_t position, DeviceInstanceId id) {
		const size_t lane = laneOf(id);
		if (chain >= chains_.size() || lane == kNoLane || nodes_[lane].chain != kNoChain) {
			return false;
		}
		ChainPath& path = chains_[chain];
		if (path.count >= path.ids.size()) {
			return false;
		}
		if (position > path.count) {
			position = path.count;
		}
		for (size_t i = path.count; i > position; --i) {
			path.ids[i] = path.ids[i - 1];
		}
		path.ids[position] = id;
		++path.count;
		nodes_[lane].chain = chain;
		if (!recompute()) {
			eraseFromChain(chain, id);
			recompute();
			return false;
		}
		return true;
	}

	/// TAKE A NODE OUT OF ITS CHAIN and leave it in the pool, detached. The node keeps its lane, its
	/// id and its routes: leaving a chain is not leaving the graph.
	bool erase(DeviceInstanceId id) {
		const size_t c = chainOf(id);
		if (c == kNoChain) {
			return false;
		}
		eraseFromChain(c, id);
		recompute();
		return true;
	}

	/// **MOVE A NODE ALONG ITS CHAIN, AND NOTHING ELSE MOVES.** Every node between the two positions
	/// shifts by one — a rotation, not a swap, because that is what a drag along a chain means. The
	/// route table is not touched, and there is a case asserting that on the table rather than on a
	/// redraw. Refused, with nothing changed, if the move would close a cycle.
	/// MOVE THE DEVICE AT `from` TO `to` WITHIN ONE CHAIN, DELETING THE ROUTES THE NEW ORDER FORBIDS —
	/// and report how many. `nullopt` only for a move that cannot be made at all: a bad chain or
	/// position, or an order that would close an AUDIO or NOTE cycle through explicit edges.
	///
	/// An amount is a cell, so deleting it writes zero,
	/// indistinguishable from never-routed, and reordering back does NOT restore it; that is accepted.
	/// A reorder is never refused on account of a route; the route goes.
	///
	/// **THIS REPLACED A REFUSAL, AND THE REFUSAL HAD NEVER FIRED.** The body used to rotate,
	/// recompute, and on a cycle rotate BACK and return false — so a modulation route that the new
	/// order made backwards would have refused the reorder, the opposite of the design rule. It never did,
	/// because until the rack routes through this graph's door (`route()`, the next commit) this
	/// table is EMPTY in every product path and a route-closed cycle could not exist here. So the
	/// semantic change has no observable before-state to preserve, and it is made where the rule
	/// says rather than left as a comment predicting a bug it could not catch.
	///
	/// **THE ORDER THAT DECIDES IS THE CHAIN'S, NOT `order()`'s.** `recompute()` treats every
	/// non-zero route as a precedence, so asking it which routes are now illegal would be circular —
	/// the routes are part of what it computes. So the chains and explicit edges are laid down alone,
	/// and each route is then re-admitted against them by the same question `route()` asks. **Since
	/// backlog IZ that is NOT "its source is at or after its destination"**: a chain orders AUDIO
	/// instants only, so a modulator's output into an earlier fx is legal wherever the two stand, and
	/// what a reorder can forbid is a route that would bind the one order both ways — SKLITTER's
	/// perform output into ANOTHER SKLITTER's control, dragged ahead of it.
	///
	/// **THE COUNT IS THE RETURN, NOT A COUNTER BESIDE THE CALL.** This deletes a player's work as a
	/// normal consequence of a normal gesture, and a number produced BY the deletion is the only
	/// number that cannot be had without it — the same reason `FrameWatchers::notify` reports how
	/// many frames it told. A host's reorder verb hands it on; a case proves the clearing is not
	/// vacuous with it.
	std::optional<size_t> move(size_t chain, size_t from, size_t to) {
		if (chain >= chains_.size()) {
			return std::nullopt;
		}
		ChainPath& path = chains_[chain];
		if (from >= path.count || to >= path.count) {
			return std::nullopt;
		}
		if (from == to) {
			return 0;
		}
		rotate(path, from, to);
		// **THE EXPLICIT EDGES ARE CHECKED BEFORE ANYTHING IS DELETED, and the order of these two
		// steps is the whole of what makes this non-lossy.** A first draft cleared first and refused
		// after, which on an audio cycle would have rolled the order back and left the routes gone —
		// a player's work destroyed by a gesture that then did nothing. So: recompute WITHOUT routes,
		// which is the order the explicit edges and the chains impose on their own. If that closes a
		// cycle, the reorder is refused with nothing touched.
		if (!recompute(/*withRoutes=*/false)) {
			// AN AUDIO OR NOTE CYCLE THROUGH EXPLICIT EDGES — not a route, so not the rule's to
			// delete, and the one thing a reorder is still refused for. (No product path holds explicit
			// edges today, so this arm is the test's, not yet a player's.)
			rotate(path, to, from);
			recompute();
			return std::nullopt;
		}
		// THE ORDER IS LEGAL; now the routes that the new order forbids go, and the full recompute
		// cannot fail — every surviving route was re-admitted against the adjacency the one above built,
		// and the explicit edges were just shown acyclic against it. The rollback below is a defence
		// against a bug, not a path.
		const size_t cleared = clearRoutesTheChainForbids();
		if (!recompute()) {
			rotate(path, to, from);
			recompute();
			return std::nullopt;
		}
		return cleared;
	}

	/// **A DEVICE'S MOD ROAD STARTS OR STOPS READING ITS AUDIO, AND THE ROUTES THAT FORBIDS GO** (backlog OP) — the
	/// Follower's Mode flipped. `move()`'s idiom exactly: the instants swap, the order WITHOUT routes is laid down,
	/// each route is re-admitted against it (`clearRoutesTheChainForbids`), and the count is the return. A flip to
	/// Audio forbids a route into what feeds the device's audio; a flip back to Control forbids one from a source
	/// computed from audio downstream of it into its controls. `cleared`, when given, receives each route that went —
	/// the record's undo puts them back. `nullopt` for an id with no node or no mod road; 0 when nothing changed.
	///
	/// MESSAGE THREAD for a graph whose routes are a player's; an engine's own graph calls it at the drain with no
	/// `cleared`, where it allocates nothing (`recompute` and the clearing are allocation-free).
	std::optional<size_t> setReadsAudio(DeviceInstanceId id, bool reads, std::vector<Route>* cleared = nullptr) {
		const size_t lane = laneOf(id);
		if (lane == kNoLane || nodes_[lane].desc == nullptr || nodes_[lane].desc->mod == nullptr) {
			return std::nullopt;
		}
		if (nodes_[lane].readsAudio == reads) {
			return 0;
		}
		nodes_[lane].readsAudio = reads;
		// THE TWO INSTANTS ARE ONE DEVICE'S, so no explicit edge can close a cycle through the swap; asked anyway, and
		// on a refusal the flag goes back with nothing deleted, `move()`'s rule.
		if (!recompute(/*withRoutes=*/false)) {
			nodes_[lane].readsAudio = !reads;
			recompute();
			return std::nullopt;
		}
		const size_t n = clearRoutesTheChainForbids(cleared);
		recompute();
		return n;
	}

	//--------------------------------------------------------------------------------------------//
	// EXPLICIT EDGES — AUDIO AND NOTES                                                             //
	//--------------------------------------------------------------------------------------------//

	/// CONNECT TWO NODES BY AN EXPLICIT EDGE OF ONE KIND. Refused unless BOTH endpoints declare the
	/// ops that kind names — null is the capability, and an edge to a device that cannot carry it is
	/// not an edge. Also refused for a self-loop, a duplicate, a full store, and an edge that would
	/// close a cycle, in every case with nothing changed.
	///
	/// `EdgeKind::Modulation` IS REFUSED HERE BY DESIGN: a modulation edge is a cell of the route
	/// table and is made with `route()`. One kind of thing, one door.
	bool connect(EdgeKind kind, DeviceInstanceId from, DeviceInstanceId to) {
		if (kind == EdgeKind::Modulation || from == to) {
			return false;
		}
		const Node* a = nodeOf(from);
		const Node* b = nodeOf(to);
		if (a == nullptr || b == nullptr || !carries(*a->desc, kind) || !carries(*b->desc, kind)) {
			return false;
		}
		if (connected(kind, from, to)) {
			return false;
		}
		makeEdgeRoom();
		edges_[edgeCount_++] = Edge { kind, from, to };
		if (!recompute()) {
			--edgeCount_;
			recompute();
			return false;
		}
		return true;
	}

	bool disconnect(EdgeKind kind, DeviceInstanceId from, DeviceInstanceId to) {
		for (size_t i = 0; i < edgeCount_; ++i) {
			if (edges_[i].kind == kind && edges_[i].from == from && edges_[i].to == to) {
				edges_[i] = edges_[--edgeCount_];
				recompute();
				return true;
			}
		}
		return false;
	}

	bool connected(EdgeKind kind, DeviceInstanceId from, DeviceInstanceId to) const noexcept {
		for (size_t i = 0; i < edgeCount_; ++i) {
			if (edges_[i].kind == kind && edges_[i].from == from && edges_[i].to == to) {
				return true;
			}
		}
		return false;
	}

	//--------------------------------------------------------------------------------------------//
	// MODULATION EDGES — EVERY ROUTE, BY ITS ENDPOINTS                                             //
	//--------------------------------------------------------------------------------------------//

	/// **WHETHER THIS GRAPH COULD HOLD A ROUTE FROM `s` TO `d` AT ALL** — the structural half, before any
	/// rule: a device end names a live node that has such an output (and is a modulator) or such a
	/// modulatable CONTROL; a host end is inside the host's own table.
	/// A control is NOT bounded by any stride: whatever the device declares modulatable is a destination, and the edge capacity is a TOTAL (`edgeCapacityFor`).
	bool admits(Source s, Destination d) const noexcept {
		// A SOURCE OUTSIDE THIS GRAPH (rack roadmap §7) is not this graph's to vouch for — the host
		// outside did, through the gate (`mayRoute`'s `outerUpstream`) — so only the destination is
		// checked here. Its precedence is `kNoLane` (a device this graph has no node for) or the host
		// node (an outer core), and neither can close a cycle in here.
		if (s.isOuter()) {
			// falls through to the destination checks
		}
		else if (s.isExport()) {
			// A SOURCE IN A DEVICE'S PRE CHAIN, EXPORTED AS THE DEVICE'S OWN (backlog NZ). It reaches the holder's own
			// controls (a declared self-route) and, out the back, the AUDIO devices after it — "after" is the cycle rule's
			// to say, in `route`. Its POST is reached from inside the site. A device with a mod road is no destination for
			// it: its controls are read in the modulators' pass, before the holder's PRE has run (decision O5 for the strip).
			const Node* n = nodeOf(s.device);
			if (n == nullptr || !device::exportsRoadOf(*n->desc, s) || s.exportRow() >= device::kSiteExportRows
			    || !d.isDevice()) {
				return false;
			}
			const Node* to = nodeOf(d.device);
			// A CONTAINER'S EXPORT (NZ (b)) never reaches the container's own controls: its sources are inside it.
			if (d.device == s.device ? !s.isSiteExport()
			                         : (to == nullptr || to->desc->audio == nullptr || to->desc->mod != nullptr)) {
				return false;
			}
		}
		else if (s.isDevice()) {
			const Node* n = nodeOf(s.device);
			if (n == nullptr || n->desc->mod == nullptr || s.index >= n->desc->outputCount()
			    || s.index >= axes_.outputStride) {
				return false;
			}
		}
		else if (s.index >= axes_.coreSources) {
			return false;
		}
		if (d.isDevice()) {
			const Node* n = nodeOf(d.device);
			if (n == nullptr || d.index >= n->desc->params.size() || !n->desc->params[d.index].modulatable) {
				return false;
			}
		}
		else if (d.index >= axes_.coreDestinations) {
			return false;
		}
		return true;
	}

	/// MAKE, CHANGE OR REMOVE A ROUTE: `amount` onto the edge from `s` to `d`, and zero IS removal,
	/// because an amount of nothing is not a route. Refused — nothing changed — for a pair this graph
	/// does not `admit`, OR ONE THAT WOULD CLOSE A CYCLE — pass-through: between two devices, `d`'s must
	/// not already reach `s`'s, and a device does not reach itself. A host end has no node and so no
	/// precedence (kill-lanes step 4 gives the host one).
	///
	/// THE CYCLE IS CHECKED BEFORE THE EDGE IS WRITTEN, not written-then-rolled-back as `connect` does:
	/// a refused route must never be visible, even to a publish taken in between.
	bool route(Source s, Destination d, float amount) {
		if (!admits(s, d)) {
			return false;
		}
		const size_t at = findModulation(s, d);
		if (amount == 0.0f) {
			// **ZERO IS A DELETION, NOT A STORED ZERO.** An edge holding one says a route exists that
			// reaches nowhere, and it would count as a precedence and order the graph for a route nobody
			// made.
			if (at != kNoEdge) {
				edges_[at] = edges_[--edgeCount_];
				recompute();
			}
			return true;
		}
		if (at == kNoEdge) {
			// **A NEW PRECEDENCE. REFUSE IF THE DESTINATION ALREADY REACHES THE SOURCE** (itself
			// included) — the routing rule in one line: modulation is pass-through, so a route
			// that would have to be read before it is written is not a route.
			//
			// **IT ASKED `s.isDevice() && d.isDevice()` AND THAT WAS A HOLE (kill-lanes step 4a).** Every
			// route pointing at a CORE destination — anything modulating a voice parameter — skipped the
			// test entirely, so the one rule left standing did not reach the endpoints most
			// likely to close a loop through the voice. Asking `precedenceLaneOf` instead puts a host end
			// on the host's own node and the rule applies to it like any other.
			const size_t fromLane = precedenceLaneOf(s);
			const size_t toLane = precedenceLaneOf(d);
			// A DECLARED SELF-ROUTE IS NOT ONE (backlog HS) — see `wouldCycle`, which must agree with this
			// line pair for pair (`test_feathers_graph_order`), and `forEachPrecedence`, which has never
			// made a precedence of `from == to`, so the edge orders nothing.
			//
			// **AND THE CYCLE IS ASKED OF TWO INSTANTS PER DEVICE (backlog IZ)** — `routeWouldCycle`.
			if (!declaredSelfRoute(s, d) && routeWouldCycle(fromLane, toLane)) {
				return false;
			}
			makeEdgeRoom(); // room AFTER the cycle test, so a refused route grows nothing
			Edge e;
			e.kind = EdgeKind::Modulation;
			e.from = s.isOuter() ? kNoInstance : s.device; // an outer source has no device in THIS graph (rack roadmap §7)
			e.to = d.device;
			e.src = s;
			e.dst = d;
			e.amount = amount;
			edges_[edgeCount_++] = e;
			recompute();
			return true;
		}
		// **AN AMOUNT CHANGING ON AN EXISTING EDGE IS NOT A NEW PRECEDENCE**, so no cycle test and no
		// reorder — the whole of a modulation-ring drag.
		edges_[at].amount = amount;
		return true;
	}

	/// **EVERY ROUTE GONE** — the explicit audio and note edges kept. What a host emptying itself needs for
	/// the routes no device owns (host to host), which no node's removal reaches.
	void clearRoutes() {
		for (size_t i = 0; i < edgeCount_;) {
			if (edges_[i].kind == EdgeKind::Modulation) {
				edges_[i] = edges_[--edgeCount_];
			}
			else {
				++i;
			}
		}
		recompute();
	}

	/// HOW FAR `s` REACHES `d`, or 0.
	float amountOf(Source s, Destination d) const noexcept {
		const size_t at = findModulation(s, d);
		return at == kNoEdge ? 0.0f : edges_[at].amount;
	}

	/// **THE CURVE ON AN EXISTING ROUTE**. False — nothing
	/// changed — when there is no route from `s` to `d`: zero is not a route, so neither is a curve on one.
	/// Not a precedence, so no cycle test and no reorder, exactly as an amount change on an edge.
	bool setTransfer(Source s, Destination d, Transfer t) noexcept {
		const size_t at = findModulation(s, d);
		if (at == kNoEdge || !device::transferValid(t)) {
			return false;
		}
		edges_[at].transfer = t;
		return true;
	}

	/// THE CURVE `s` REACHES `d` THROUGH — Lin for no route.
	Transfer transferOf(Source s, Destination d) const noexcept {
		const size_t at = findModulation(s, d);
		return at == kNoEdge ? Transfer::Lin : edges_[at].transfer;
	}

	/// **EVERY ROUTE**: `f(const Route&)`. `Edge` stays private — a caller wants what reaches what and by
	/// how much, and the representation is not part of the interface.
	template <typename F>
	void forEachRoute(F&& f) const {
		for (size_t i = 0; i < edgeCount_; ++i) {
			const Edge& e = edges_[i];
			if (e.kind == EdgeKind::Modulation) {
				f(Route { e.src, e.dst, e.amount, e.transfer });
			}
		}
	}

	/// **WHETHER ANY SOURCE BUT `except` REACHES `d`** — the "routed elsewhere" mark a control draws beside
	/// the armed source's ring (`DeviceHost::routedElsewhere`).
	bool routedElsewhere(Destination d, Source except) const noexcept {
		for (size_t i = 0; i < edgeCount_; ++i) {
			const Edge& e = edges_[i];
			if (e.kind == EdgeKind::Modulation && e.dst == d && e.src != except) {
				return true;
			}
		}
		return false;
	}

	/// **PLANT A ROUTE PAST EVERY CHECK — TESTS ONLY.** A patch file may carry a route no gesture could
	/// make, and a case must be able to leave the graph holding one to show the reader refusing it. It
	/// does not recompute, exactly as the table's cell write this replaced did not.
	bool plantRouteForTest(Source s, Destination d, float amount) {
		makeEdgeRoom();
		Edge e;
		e.kind = EdgeKind::Modulation;
		e.from = s.isOuter() ? kNoInstance : s.device; // an outer source has no device in THIS graph (rack roadmap §7)
		e.to = d.device;
		e.src = s;
		e.dst = d;
		e.amount = amount;
		edges_[edgeCount_++] = e;
		return true;
	}

	// **`endpointsOf`, `placement()` AND `publishRoutes(RouteEdges&, …)` STOOD HERE AND ARE DELETED
	// (kill-lanes §2.4b).** All three were the crossing from a device's identity to a flat coordinate — a
	// lane × stride — and back, for a resolve that read an edge list by coordinate. A node pulls through
	// its inputs now (`RackEngine::rebuildInputs`), and the only thing that leaves this graph about a
	// route is `forEachRoute`'s `Route` by identity. `publishRouteList` below is that, for a host that
	// publishes across a thread.

	/// **HOW MANY EDGES A HOST'S GRAPH STARTS WITH** — two per node and a first sixteen routes. An INITIAL size,
	/// not a bound (backlog OJ): the store grows by `makeEdgeRoom`, and a bigger graph built to replace this one
	/// takes `max(this, the old graph's capacity)`. One number for every graph a host builds.
	static constexpr size_t edgeCapacityFor(const Axes& axes) { return axes.deviceSlots * 2 + 16; }


	/// **THE ROUTES BY IDENTITY** — what a host PUBLISHES (`RouteList`), each end a device's id and its own
	/// output or control index, or the host's own index. The
	/// resolving side builds each node's inputs from it (`RackEngine::rebuildInputs`); nothing past the thread
	/// boundary is told a lane. The host's cells and the graph's edges alike. Allocates nothing.
	void publishRouteList(RouteList& out) const noexcept {
		out.begin();
		forEachRoute([&out](const Route& r) { out.add(r); });
		out.end();
	}

	//--------------------------------------------------------------------------------------------//
	// ORDER                                                                                        //
	//--------------------------------------------------------------------------------------------//

	/// EVERY LIVE NODE, IN EVALUATION ORDER. Recomputed by every mutator; valid until the next one.
	std::span<const Step> order() const noexcept {
		return std::span<const Step>(order_).first(orderCount_);
	}

private:
	struct Edge {
		EdgeKind kind = EdgeKind::Audio;
		DeviceInstanceId from = kNoInstance;
		DeviceInstanceId to = kNoInstance;

		/// **A MODULATION EDGE'S OWN FIELDS — ITS TWO ENDPOINTS AND ITS AMOUNT.** Unused by Audio and Note
		/// edges, which are a bare pair. `from`/`to` above are the endpoints' devices (`kNoInstance` for a
		/// host end), kept beside them because every precedence and removal walk asks them.
		///
		/// **THIS IS WHERE EVERY ROUTE LIVES — HOST CELLS INCLUDED, SINCE KILL-LANES STEP 2.** A route was a
		/// cell of `RouteTable`, a dense matrix keyed by lane; device-to-device routes became edges first,
		/// and a route with a HOST end (a macro into a device, a device into the host's own parameter) was
		/// the cell that kept the table alive. It is an edge with a core end now, so the table and its lane
		/// keys are gone: one store, by identity.
		Source src {};
		Destination dst {};
		float amount = 0.0f;
		Transfer transfer = Transfer::Lin;
	};

	struct ChainPath {
		std::vector<DeviceInstanceId> ids;
		size_t count = 0;
	};

	static constexpr size_t kNoEdge = static_cast<size_t>(-1);

	/// WHERE THE ROUTE FROM `s` TO `d` IS, or `kNoEdge`.
	///
	/// A LINEAR SCAN, AND THAT IS THE POINT RATHER THAN A CONCESSION: this array holds what a patch
	/// actually routes — tens — where the matrix it replaced held tens of thousands of cells to say the
	/// same thing. `remove()` already scans it by id and nobody has minded.
	size_t findModulation(Source s, Destination d) const noexcept {
		for (size_t i = 0; i < edgeCount_; ++i) {
			const Edge& e = edges_[i];
			if (e.kind == EdgeKind::Modulation && e.src == s && e.dst == d) {
				return i;
			}
		}
		return kNoEdge;
	}

	static bool carries(const device::Descriptor& desc, EdgeKind kind) noexcept {
		switch (kind) {
		case EdgeKind::Audio:
			return desc.audio != nullptr;
		case EdgeKind::Modulation:
			return desc.mod != nullptr;
		case EdgeKind::Notes:
			return desc.notes != nullptr;
		}
		return false;
	}


	/// **THE DESCRIPTOR THE HOST'S OWN NODE STANDS UNDER.** It declares AUDIO and nothing else, because
	/// that is the node's whole structural claim: the voice's output is what an audio chain is a chain
	/// of. **Its ops are never called** — the node is in no chain, no converger provisions it, no engine
	/// holds a slot for it, and `slots()` does not expose its lane. It is the graph's own rather than a
	/// host's argument so that "the host's node" cannot be pointed at a real device.
	static const device::Descriptor& coreDescriptor() noexcept {
		static const device::AudioOps kOps {};
		static const device::Descriptor kDesc { .traits = {}, .params = {}, .outputs = {}, .audio = &kOps };
		return kDesc;
	}
	/// **THE LOWEST FREE DEVICE SLOT — NEVER THE CORE LANE (kill-lanes step 4a).** The reserved lane is
	/// one past the device slots and belongs to the host's own node; handing it to a device would put a
	/// device where no coordinate can name it and where every chain walk would miss it.
	size_t lowestFreeLane() const noexcept {
		for (size_t lane = 0; lane < coreLane_; ++lane) {
			if (!nodes_[lane].live()) {
				return lane;
			}
		}
		return kNoLane;
	}

	/// THE ONE WRITER of a lane's node and of the descriptor-per-lane the slot view reads, so the two
	/// cannot disagree.
	void place(size_t lane, DeviceInstanceId id, const device::Descriptor& desc) {
		nodes_[lane] = Node { id, &desc, kNoChain, desc.mod != nullptr && desc.mod->readsAudio };
		descAtLane_[lane] = &desc;
		++nodeCount_;
	}

	void eraseFromChain(size_t chain, DeviceInstanceId id) {
		ChainPath& path = chains_[chain];
		size_t at = path.count;
		for (size_t i = 0; i < path.count; ++i) {
			if (path.ids[i] == id) {
				at = i;
				break;
			}
		}
		if (at == path.count) {
			return;
		}
		for (size_t i = at + 1; i < path.count; ++i) {
			path.ids[i - 1] = path.ids[i];
		}
		--path.count;
		path.ids[path.count] = kNoInstance;
		const size_t lane = laneOf(id);
		if (lane != kNoLane) {
			nodes_[lane].chain = kNoChain;
		}
	}

	/// ZERO EVERY ROUTE THE CHAINS NOW FORBID, and count them.
	///
	/// **CALLED WITH THE ADJACENCY `recompute(/*withRoutes=*/false)` BUILT** — the chains and the
	/// explicit edges alone; see `move()` for why asking `recompute()` with the routes would be circular.
	/// Each route is then RE-ADMITTED by the question `route()` asks (`routeWouldCycle`), and a route
	/// that passes lays its own precedence down before the next is asked — so the survivors are a set
	/// `route()` could have built one by one, which is the only claim that makes the full recompute
	/// after this unable to fail.
	///
	/// **THIS WAS "SOURCE AT OR AFTER DESTINATION IN THE SAME CHAIN" UNTIL BACKLOG IZ**, and that rule
	/// is now false in both directions: a chain orders AUDIO instants only, so a modulator's output into
	/// an fx before it is legal wherever the two stand; and a route between CHAINS can be forbidden by a
	/// reorder, through a modulator in the strip that binds a lane device both ways. A route that closes
	/// a cycle only TOGETHER with another is decided by the edge store's order — the later one goes —
	/// which is deterministic, though not necessarily the one drawn last.
	size_t clearRoutesTheChainForbids(std::vector<Route>* out = nullptr) {
		const size_t n = nodes_.size();
		const size_t m = 2 * n;
		size_t cleared = 0;
		for (size_t i = 0; i < edgeCount_;) {
			const Edge& e = edges_[i];
			if (e.kind != EdgeKind::Modulation) {
				++i;
				continue;
			}
			const size_t fromLane = precedenceLaneOf(e.src);
			const size_t toLane = precedenceLaneOf(e.dst);
			// **A SELF-ROUTE IS NOT THE CHAIN'S TO FORBID** (backlog HS): no reorder moves a device relative
			// to itself, and the only self-route `route()` ever wrote is one its device declared. An end
			// with no node (a host's own source, an outer one) orders nothing and cannot be forbidden.
			if (fromLane == kNoLane || toLane == kNoLane || fromLane == toLane) {
				++i;
				continue;
			}
			if (routeWouldCycle(fromLane, toLane)) {
				// **REMOVED BY SWAPPING THE LAST EDGE DOWN, SO `i` IS NOT ADVANCED** — the edge now at `i`
				// has not been examined.
				if (out != nullptr) {
					out->push_back(Route { e.src, e.dst, e.amount, e.transfer });
				}
				edges_[i] = edges_[--edgeCount_];
				++cleared;
				continue;
			}
			// ADMITTED: its precedence goes down now, so the next route is asked against it — the same
			// two cells `forEachPrecedence` gives a route.
			const size_t from = modInstant(fromLane);
			const size_t to = modInstant(toLane);
			adj_[from * m + to] = 1;
			if (bindsOrder(from, to)) {
				orderAdj_[fromLane * n + toLane] = 1;
			}
			++i;
		}
		return cleared;
	}

	static void rotate(ChainPath& path, size_t from, size_t to) {
		const DeviceInstanceId moving = path.ids[from];
		if (from < to) {
			for (size_t i = from; i < to; ++i) {
				path.ids[i] = path.ids[i + 1];
			}
		}
		else {
			for (size_t i = from; i > to; --i) {
				path.ids[i] = path.ids[i - 1];
			}
		}
		path.ids[to] = moving;
	}

	/// WALK EVERY PRECEDENCE — each node's own two instants, explicit edges, each chain's path once per
	/// capability, and every non-zero route between two nodes — calling `f(fromInstant, toInstant)`
	/// (`modInstant`/`audioInstant`). One traversal, so the adjacency it builds cannot disagree with
	/// itself about what an edge is.
	template <typename F>
	/// `withRoutes` false walks the explicit edges and the chains ONLY — the order the block would have
	/// if no route existed. `move()` asks that first, so it can tell an audio or note cycle (refused)
	/// from a route the new order has made backwards (deleted) before it deletes anything.
	void forEachPrecedence(bool withRoutes, F&& f) const {
		// **A NODE'S OWN TWO INSTANTS (backlog IZ): MOD BEFORE AUDIO**, because that is what every host
		// runs — the modulators' pass, then the audio walk — and a mod road is handed no signal to read.
		// A device that DECLARES its mod road reads its audio (`ModOps::readsAudio`, set by nobody yet)
		// has them the other way round, and its outputs are then downstream of everything feeding it.
		for (size_t lane = 0; lane < nodes_.size(); ++lane) {
			if (!nodes_[lane].live()) {
				continue;
			}
			if (modReadsAudio(lane)) {
				f(audioInstant(lane), modInstant(lane));
			}
			else {
				f(modInstant(lane), audioInstant(lane));
			}
		}
		for (size_t i = 0; i < edgeCount_; ++i) {
			// **EXPLICIT EDGES ONLY — A MODULATION EDGE IS HANDLED BELOW, UNDER `withRoutes`.**
			// This loop predates routes being edges and emitted every edge in the pool; once they
			// joined it, a route became a precedence even for callers that asked for the order
			// WITHOUT routes. `move()` is one, and it refused every reorder as cyclic.
			if (edges_[i].kind == EdgeKind::Modulation) {
				continue;
			}
			f(audioInstant(laneOf(edges_[i].from)), audioInstant(laneOf(edges_[i].to)));
		}
		// **AUDIO AND NOTES ONLY, AND BOTH BETWEEN AUDIO INSTANTS (backlog IZ).** A chain ordered its
		// consecutive MODULATORS too, so a strip modulator could not reach one to its left and SKLITTER
		// could not reach an fx before it. downstream, for modulation, means computed from downstream audio.
		//
		// **A NOTE LISTENER IS ORDERED AGAINST WRITERS ONLY** (`NoteOps::listensOnly`, 2026-10-03): it follows the
		// last member that writes notes and precedes the next, and two listeners are not ordered at all — what
		// one hands the other is what it was handed. So `prev` is the last WRITER, and a writer also follows every
		// listener since the one before it.
		constexpr EdgeKind kKinds[] = { EdgeKind::Audio, EdgeKind::Notes };
		for (const ChainPath& path : chains_) {
			for (EdgeKind kind : kKinds) {
				size_t prev = kNoLane;
				size_t prevAt = 0; // where `prev` stands in the path, so a writer can find the listeners after it
				for (size_t i = 0; i < path.count; ++i) {
					const size_t lane = laneOf(path.ids[i]);
					const device::Descriptor& desc = *nodes_[lane].desc;
					if (!carries(desc, kind)) {
						continue;
					}
					if (prev != kNoLane) {
						f(audioInstant(prev), audioInstant(lane));
					}
					if (kind == EdgeKind::Notes && desc.notes->listensOnly) {
						continue;
					}
					for (size_t j = prev == kNoLane ? 0 : prevAt + 1; j < i; ++j) {
						const size_t before = laneOf(path.ids[j]);
						const device::Descriptor& d = *nodes_[before].desc;
						if (kind == EdgeKind::Notes && carries(d, kind) && d.notes->listensOnly) {
							f(audioInstant(before), audioInstant(lane));
						}
					}
					prev = lane;
					prevAt = i;
				}
			}
		}
		// **AND THE HOST'S NODE PRECEDES THE FIRST AUDIO DEVICE OF EVERY CHAIN.** Derived here rather
		// than stored as an explicit edge, so nothing has to maintain it when the head of a chain
		// changes — which is the "remap per road forever" shape this arc has been deleting. A chain with
		// no audio-carrying device (Feathers' strip, which holds modulators) gets none, which is correct:
		// the voice does not feed it.
		//
		// **STRUCTURE, NOT A ROUTE, SO IT IS LAID WITHOUT ROUTES TOO** (backlog OP). It stood after the `withRoutes`
		// cut-off, so the order `move()` and `setReadsAudio()` re-admit routes against lacked it: an Audio-mode
		// Follower heading the strip kept its route into the voice it hears, a loop through the voice. MEASURED by
		// test_mod_follower "in the strip, Audio hears the voice ...", red before this move.
		if (coreLane_ < nodes_.size() && nodes_[coreLane_].live()) {
			for (const ChainPath& path : chains_) {
				for (size_t i = 0; i < path.count; ++i) {
					const size_t lane = laneOf(path.ids[i]);
					if (lane == kNoLane || !carries(*nodes_[lane].desc, EdgeKind::Audio)) {
						continue;
					}
					f(audioInstant(coreLane_), audioInstant(lane));
					break; // the HEAD only; the chain's own adjacency carries the rest
				}
			}
		}
		// ROUTES ARE PRECEDENCES (pass-through). A source lane reaches a destination lane if any
		// output-to-ordinal cell between them is non-zero; only the outputs and ordinals the two
		// devices declare are read, which is also all `route()` can write.
		if (!withRoutes) {
			return;
		}
		// **THE ROUTED PRECEDENCES, WALKED RATHER THAN SEARCHED FOR.** This was a scan of every live
		// (from, to) LANE PAIR and, inside it, every (output, ordinal) cell between them: at the rack's
		// shape roughly eighteen thousand table probes on EVERY mutation — every `route`, `connect`,
		// `move`, add and remove — to discover what is typically one route. It is now a walk of what
		// is actually routed.
		//
		// A PAIR STATED TWICE IS STILL ONE EDGE in the matrix `recompute` builds, which is what keeps
		// in-degree and release in step; two routes between the same pair of devices set the same cell.
		//
		// **A ROUTE INTO A CORE DESTINATION IS A PRECEDENCE INTO THE HOST'S OWN NODE (kill-lanes step
		// 4a)** — `precedenceLaneOf` is what says so, and it answers `kNoLane` for a host that declared
		// none, which leaves this exactly as it was for the standalone rack.
		//
		// **FROM THE SOURCE'S MOD INSTANT TO THE DESTINATION'S MOD INSTANT (backlog IZ)** — every output
		// is published at its device's MOD instant, and every control is read there (conservatively: a
		// control only the audio road reads is read LATER, which is `SelfRouting`'s finer claim).
		for (size_t i = 0; i < edgeCount_; ++i) {
			const Edge& e = edges_[i];
			if (e.kind != EdgeKind::Modulation) {
				continue;
			}
			const size_t from = precedenceLaneOf(e.src);
			const size_t to = precedenceLaneOf(e.dst);
			if (from != kNoLane && to != kNoLane && from != to) {
				f(modInstant(from), modInstant(to));
			}
		}
	}

	/// **WHICH NODE AN ENDPOINT'S PRECEDENCE BELONGS TO** — a device's own lane, or the host node's for a
	/// CORE end. `kNoLane` when nothing owns it: a host that declared no core node, or a device the
	/// graph does not hold.
	///
	/// **A CORE SOURCE ANSWERS `kNoLane`, AND THAT IS A DECISION RATHER THAN AN OMISSION.** A host's own
	/// sources are INPUTS — a macro is set by a hand or a host, not computed by the voice — so nothing
	/// has to run before one can be read. Feathers' weight sources are the case that tests this: they
	/// are derived from the puck, which is itself a parameter, so they are an input to the modulation
	/// tier rather than an output of the audio node. If a host ever grows a core source that IS produced
	/// by its node, this is the line that has to learn the difference, and a loop through it would go
	/// undetected until it does.
	size_t precedenceLaneOf(Source s) const noexcept {
		// **AN OUTER SOURCE HAS NO LANE HERE** (rack roadmap §7) — and its device id is spelled in the
		// OUTER host's id space, which may coincide with one of this graph's ids. Looking it up would
		// have made an outer output "the inner device that wears the same number": MEASURED, the first
		// route from an outer modulator was refused as a route from a device into itself.
		return s.isDevice() && !s.isOuter() ? laneOf(s.device) : kNoLane;
	}
	size_t precedenceLaneOf(Destination d) const noexcept {
		if (d.isDevice()) {
			return laneOf(d.device);
		}
		return nodes_[coreLane_].live() ? coreLane_ : kNoLane;
	}

	/// **IS THIS A DEVICE'S OUTPUT INTO ITS OWN CONTROL, AND DID THE DEVICE DECLARE THAT CONTROL
	/// DOWNSTREAM OF ITS OUTPUTS?** (backlog HS, `device::Descriptor::mayRouteToSelf`.) The one pair
	/// for which `reaches(lane, lane)` is the wrong question. An OUTER source is never this: its id is
	/// spelled in another host's id space (see `precedenceLaneOf`).
	bool declaredSelfRoute(Source s, Destination d) const noexcept {
		if (!s.isDevice() || s.isOuter() || !d.isDevice() || s.device != d.device) {
			return false;
		}
		// **A PRE SOURCE INTO ITS HOLDER IS ONE TOO** (backlog NZ): the PRE chain runs before the device, and its
		// modulators publish at the end of each tick, which is what the holder's next resolve reads. Nothing the device
		// does reaches back into its PRE chain, so the pair is not read before it is written.
		if (s.isExport()) {
			const Node* holder = nodeOf(s.device); // a SITE's PRE only: a container's export never reaches it (NZ (b))
			return s.isSiteExport() && holder != nullptr && holder->desc != nullptr && device::exportsSite(*holder->desc);
		}
		const Node* n = nodeOf(s.device);
		return n != nullptr && n->desc != nullptr && n->desc->mayRouteToSelf(d.index);
	}

	//--------------------------------------------------------------------------------------------//
	// THE TWO INSTANTS (backlog IZ)                                                                //
	//--------------------------------------------------------------------------------------------//

	/// A LANE'S MOD INSTANT — its tick and publish, where its outputs are written and (conservatively)
	/// its controls read. The lane itself, so the instant matrix's first half is indexed as lanes are.
	size_t modInstant(size_t lane) const noexcept { return lane; }
	/// A LANE'S AUDIO INSTANT — its audio and note roads. One pool-length on from its MOD instant.
	size_t audioInstant(size_t lane) const noexcept { return lane + nodes_.size(); }
	size_t laneOfInstant(size_t instant) const noexcept {
		return instant >= nodes_.size() ? instant - nodes_.size() : instant;
	}

	/// DID THE DEVICE AT `lane` DECLARE THAT ITS MOD ROAD READS ITS AUDIO? (`ModOps::readsAudio`.)
	/// Per node since backlog OP: a device whose row decides it (`ModOps::readsAudioOf`) is told by its host.
	bool modReadsAudio(size_t lane) const noexcept {
		const device::Descriptor* desc = nodes_[lane].desc;
		return desc != nullptr && desc->mod != nullptr && nodes_[lane].readsAudio;
	}

	/// **DOES THIS INSTANT RUN IN THE MODULATORS' PASS?** A host walks ONE order twice — every device
	/// with mod ops is ticked in it, then every device with audio or note ops is run in it
	/// (`RackEngine::beginPiece`; `graph.h`'s "TWO PASSES"). A device's MOD instant is in the first pass
	/// when it HAS mod ops that read no audio. Every other instant is read in the second: an AUDIO
	/// instant is run there, and a device with no mod ops has its controls resolved there, when its
	/// audio runs — so its MOD instant, as a destination, is met in the second pass too.
	bool inModPass(size_t instant) const noexcept {
		if (instant >= nodes_.size()) {
			return false;
		}
		const device::Descriptor* desc = nodes_[instant].desc;
		return desc != nullptr && desc->mod != nullptr && !nodes_[instant].readsAudio;
	}

	/// **DOES THE INSTANT PRECEDENCE `from → to` BIND THE ORDER OF THEIR TWO DEVICES?** Yes, unless the
	/// passes already settle it: an edge out of the modulators' pass into the second pass is kept by the
	/// passes being sequential, wherever the two devices stand in the order — which is the whole of
	/// backlog IZ's new reach (SKLITTER's perform output into an fx before it, into the voice). Two
	/// instants of ONE device bind nothing between devices.
	bool bindsOrder(size_t from, size_t to) const noexcept {
		return laneOfInstant(from) != laneOfInstant(to) && !(inModPass(from) && !inModPass(to));
	}

	/// **WOULD A ROUTE FROM THE DEVICE AT `fromLane` TO THE ONE AT `toLane` CLOSE A CYCLE? THE ROUTING
	/// RULE, ASKED OF TWO INSTANTS (backlog IZ).** `wouldCycle`, `route()` and `move()`'s re-admission all
	/// ask exactly this, so they cannot disagree. Two halves, and the route is refused if either fails:
	///
	///   1. **READ BEFORE IT IS WRITTEN** — the destination's MOD instant already reaches the source's.
	///      A device reaching itself counts (`reaches` is true at `from == to`, trap T1), so an
	///      undeclared self-route is refused here; a declared one never gets this far.
	///   2. **NO ONE ORDER COULD RUN IT** — the route binds the order (`bindsOrder`) and the destination
	///      device already precedes the source in it. The host walks ONE device order in two passes, so
	///      a route from SKLITTER into ANOTHER SKLITTER before it in the lane would need the second
	///      ticked first and run first at once. Half 1 cannot see that: the instants alone are acyclic.
	///
	/// An end with no node (`kNoLane`: a host's own source, an outer source, no core node) orders
	/// nothing and closes nothing.
	bool routeWouldCycle(size_t fromLane, size_t toLane) const noexcept {
		if (fromLane == kNoLane || toLane == kNoLane) {
			return false;
		}
		const size_t from = modInstant(fromLane);
		const size_t to = modInstant(toLane);
		if (reaches(adj_, 2 * nodes_.size(), to, from)) {
			return true;
		}
		return bindsOrder(from, to) && reaches(orderAdj_, nodes_.size(), toLane, fromLane);
	}

	/// Whether `from` reaches `to` over one of the two matrices the last `recompute` built — the
	/// instants' (`adj_`, `count` = two per lane) or the devices' order (`orderAdj_`, one per lane).
	/// `from == to` counts, because a device is not upstream of itself. Depth-first, no allocation and
	/// no recursion. `reachesLane` above is this without the self-hit.
	///
	/// **IT USED `emitted_` AND `order_` AS SCRATCH AND CALLED `recompute()` TO REPAIR THEM** — so a
	/// question cost a full topological sort, and asking it was a mutation. That was tolerable while the
	/// only caller was `route()`, which was about to recompute anyway; it is not tolerable now that the
	/// ROUTING RULE asks it (kill-lanes step 4b), because a surface asks the rule about every pair it
	/// draws. Its own scratch costs two vectors of the pool's length and makes the query `const`.
	///
	/// MESSAGE THREAD ONLY, like every other read of this graph — the scratch is `mutable` and this is
	/// not reentrant.
	bool reaches(const std::vector<uint8_t>& adj, size_t count, size_t from, size_t to) const noexcept {
		if (from >= count || to >= count) {
			return false;
		}
		if (from == to) {
			return true;
		}
		std::fill(seen_.begin(), seen_.end(), static_cast<uint8_t>(0));
		size_t top = 0;
		stack_[top++] = from;
		seen_[from] = 1;
		while (top > 0) {
			const size_t at = stack_[--top];
			for (size_t next = 0; next < count; ++next) {
				if (adj[at * count + next] == 0 || seen_[next] != 0) {
					continue;
				}
				if (next == to) {
					return true;
				}
				seen_[next] = 1;
				stack_[top++] = next;
			}
		}
		return false;
	}


	/// KAHN'S ALGORITHM over the precedences, modulators preferred among the ready. Returns false —
	/// with `order()` truncated to what could be emitted — when a cycle leaves nodes unreachable.
	/// Every mutator that can create a cycle checks this and rolls itself back (or, for a route,
	/// asks `reaches` first and never writes).
	bool recompute(bool withRoutes = true) {
		const size_t n = nodes_.size();
		const size_t m = 2 * n;
		for (size_t lane = 0; lane < n; ++lane) {
			indeg_[lane] = 0;
			emitted_[lane] = 0;
		}
		std::fill(adj_.begin(), adj_.end(), static_cast<uint8_t>(0));
		std::fill(orderAdj_.begin(), orderAdj_.end(), static_cast<uint8_t>(0));
		forEachPrecedence(withRoutes, [this, n, m](size_t from, size_t to) {
			// A precedence stated twice — a pair that is both chain-adjacent and routed — is one
			// edge; the matrix makes that so, and is what keeps in-degree and release in step.
			adj_[from * m + to] = 1;
			// **AND THE DEVICE ORDER IT BINDS, IF ANY (backlog IZ)** — the matrix `order()` is sorted from.
			if (bindsOrder(from, to)) {
				orderAdj_[laneOfInstant(from) * n + laneOfInstant(to)] = 1;
			}
		});
		const bool instantsAcyclic = instantsAreAcyclic();
		for (size_t from = 0; from < n; ++from) {
			for (size_t to = 0; to < n; ++to) {
				if (orderAdj_[from * n + to] != 0) {
					++indeg_[to];
				}
			}
		}
		orderCount_ = 0;
		// EVERY LANE UNRANKED UNTIL THE WALK RANKS IT, so a lane whose node was removed since the last
		// recompute cannot keep the rank it had — the same reason `emitted_` is cleared above rather
		// than trusted. A stale rank is a device that looks upstream of things it no longer precedes.
		for (size_t lane = 0; lane < rankAtLane_.size(); ++lane) {
			rankAtLane_[lane] = kNoLane;
		}
		// THE TIE-BREAK KEY, PER LANE (see the pick below): chain index then position for a chained node;
		// after every chain, by id, for one in no chain — the host's own node among them.
		const size_t stride = nodes_.size() + 1;
		const size_t detached = chains_.size() * stride;
		for (size_t lane = 0; lane < orderKey_.size(); ++lane) {
			orderKey_[lane] = detached + (lane < nodes_.size() ? static_cast<size_t>(nodes_[lane].id) : 0);
		}
		for (size_t c = 0; c < chains_.size(); ++c) {
			for (size_t i = 0; i < chains_[c].count; ++i) {
				const size_t lane = laneOf(chains_[c].ids[i]);
				if (lane < orderKey_.size()) {
					orderKey_[lane] = c * stride + i;
				}
			}
		}
		for (;;) {
			// **THE EARLIEST READY CHAIN POSITION DECIDES, AND NOTHING ELSE.** Among the nodes ready at a
			// step nothing in the graph orders, and the sort has to pick one. Two tie-breaks have been
			// deleted here. Step 4b's: it preferred a MODULATOR over any other ready node — *"the lowest
			// modulator lane wins outright"* — the band rule hiding in the sort, ordering by KIND rather
			// than by any relation (it could not go until the host's own node supplied the relations it
			// stood in for and the rule became reachability). Then, until kill-lanes 4c, THE LOWEST LANE —
			// a pool address, which is exactly what that arc deletes: after a remove and an add the lanes
			// no longer follow a chain, and the day the engine began running this order it ran a rack in
			// an order no surface showed. The chain paths ARE graph structure, so the tie falls to the
			// earliest chain position (`orderKey_`, computed above); a node in no chain comes after every
			// chain, by its id. Where nothing is routed against it the result IS the chain walk, and it
			// differs exactly where a route requires.
			size_t pick = kNoLane;
			for (size_t lane = 0; lane < nodes_.size(); ++lane) {
				if (!nodes_[lane].live() || emitted_[lane] != 0 || indeg_[lane] != 0) {
					continue;
				}
				if (pick == kNoLane || orderKey_[lane] < orderKey_[pick]) {
					pick = lane;
				}
			}
			if (pick == kNoLane) {
				break;
			}
			emitted_[pick] = 1;
			// THE RANK IS THE STEP'S INDEX — the same number written two ways, so a host asking "is
			// lane A before lane B" reads two integers instead of searching `order_` twice.
			rankAtLane_[pick] = orderCount_;
			order_[orderCount_++] = Step { nodes_[pick].id, pick };
			for (size_t to = 0; to < n; ++to) {
				if (orderAdj_[pick * n + to] != 0) {
					--indeg_[to];
				}
			}
		}
		// **BOTH GRAPHS, BECAUSE NEITHER'S ACYCLICITY IMPLIES THE OTHER'S** (backlog IZ): two SKLITTERs
		// routed into each other's controls in lane order are acyclic as instants and cannot be put in
		// one order; a follower routed into a modulator that routes back is the reverse.
		return orderCount_ == nodeCount_ && instantsAcyclic;
	}

	/// **KAHN'S ALGORITHM OVER THE INSTANTS, FOR ITS VERDICT ONLY** — whether every live node's two
	/// instants can be emitted. No order is kept: a host walks devices, and `order()` is the device
	/// order. `adj_` must be the matrix `recompute` just built.
	bool instantsAreAcyclic() {
		const size_t n = nodes_.size();
		const size_t m = 2 * n;
		std::fill(instantIndeg_.begin(), instantIndeg_.end(), static_cast<size_t>(0));
		std::fill(instantDone_.begin(), instantDone_.end(), static_cast<uint8_t>(0));
		for (size_t from = 0; from < m; ++from) {
			for (size_t to = 0; to < m; ++to) {
				if (adj_[from * m + to] != 0) {
					++instantIndeg_[to];
				}
			}
		}
		size_t emitted = 0;
		for (bool progress = true; progress;) {
			progress = false;
			for (size_t at = 0; at < m; ++at) {
				if (!nodes_[laneOfInstant(at)].live() || instantDone_[at] != 0 || instantIndeg_[at] != 0) {
					continue;
				}
				instantDone_[at] = 1;
				++emitted;
				progress = true;
				for (size_t to = 0; to < m; ++to) {
					if (adj_[at * m + to] != 0) {
						--instantIndeg_[to];
					}
				}
			}
		}
		return emitted == 2 * nodeCount_;
	}

	Axes axes_ {};
	InstanceCounter* ids_ = nullptr;
	std::vector<Node> nodes_;
	std::vector<const device::Descriptor*> descAtLane_;
	size_t nodeCount_ = 0;
	std::vector<ChainPath> chains_;
	std::vector<Edge> edges_;
	size_t edgeCount_ = 0;
	size_t edgeGrowths_ = 0;
	std::vector<Step> order_;
	size_t orderCount_ = 0;
	/// `order_` inverted: the step index at which each lane was emitted, `kNoLane` for a lane not in
	/// the order. Written by `recompute()` beside `order_` and read by `slots()` for `mayRoute`.
	std::vector<size_t> rankAtLane_;
	std::vector<size_t> indeg_;
	std::vector<uint8_t> emitted_;
	std::vector<size_t> orderKey_; // the sort's tie-break, per lane — chain position, never the lane itself
	/// THE INSTANT PRECEDENCE MATRIX the last `recompute` built, `(2 × lanes)²` bytes — a lane's MOD
	/// instant at its lane, its AUDIO instant one pool-length on (`modInstant`/`audioInstant`). What
	/// "read before written" is asked of.
	std::vector<uint8_t> adj_;
	/// **THE DEVICE ORDER MATRIX**, `lanes²` bytes — the instant edges that bind the ONE order a host
	/// walks in two passes (`bindsOrder`). What `order()` is sorted from and what the second half of
	/// `routeWouldCycle` is asked of.
	std::vector<uint8_t> orderAdj_;
	/// Scratch for the instant graph's own acyclicity check in `recompute`.
	std::vector<size_t> instantIndeg_;
	std::vector<uint8_t> instantDone_;
	/// **THE RESERVED LANE FOR THE HOST'S OWN NODE** — `axes.deviceSlots`, one past every device slot.
	/// A constant of the shape rather than a `constexpr`, because it is the axes' number plus one and
	/// the axes are a constructor argument. See the constructor for why it is not IN the device slots.
	size_t coreLane_ = 0;
	/// SCRATCH FOR `reaches`, so a reachability question costs no allocation and does not disturb the
	/// order it is asked about. `mutable` because the question is `const`; message thread only.
	mutable std::vector<uint8_t> seen_;
	mutable std::vector<size_t> stack_;
};

} // namespace feathers::backplane
