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
#include "backplane/container_bodies.h"
#include "backplane/graph.h"
#include "backplane/route_list.h"
#include "backplane/routing.h"
#include "device/registry.h"
#include "rack/chain_state.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace feathers::backplane {
class DeviceHost;
}

namespace feathers::rack {

/// **HOW MANY DEVICES A CHAIN MAY HOLD — a COUNT, not a range (kill-lanes step 4d).**
///
/// It was a `LaneBand { first, count }`: a contiguous run of pool slots reserved to one chain. The range
/// half is deleted, and what it really bought is worth stating because it was two different things
/// wearing one struct:
///   - **A CAPACITY**, which a chain genuinely has — the strip holds four modulators, the fx lane eight —
///     and which a hand can feel when a ninth is refused. That survives, as this.
///   - **AN ADDRESS RANGE**, which is what made a band a band: `isFxDeviceSlot` decided which CHAIN a
///     route's device belonged to from the NUMBER of its slot, and the ranking put one band ahead of the
///     other. Every one of those readers is gone — a route names an id, and ordering comes only from the
///     graph — so a chain no longer owns a region of anything. The record parks a
///     device in the lowest free slot of ONE pool and that slot says nothing about which chain it is in.
using ChainCapacity = size_t;

/// **THE RECORD — what a host's chains ARE, on the message thread.** The editor edits this; the host publishes its `state()` (`PublishedChainState`) and the
/// engine's drain converges to it (`Converger`). One record type for every host — Feathers' bridge, the
/// standalone rack's bridge, a rack inside a rack — so the chain's rules are written once.
///
/// **A DEVICE IS ITS ID; WHERE IT IS HELD IS THIS RECORD'S BUSINESS ALONE (kill-lanes step 3b).** Every
/// verb and every read names a device by `DeviceInstanceId`, or a place by `(chain, position)` — the two
/// things that are true of a chain. The graph's pool index does not leave this class, and no caller
/// numbers, sorts or addresses anything by it.
///
/// **STRUCTURE IN THE GRAPH, ROWS IN THE STATE.** `backplane::Graph` is the authority for which devices
/// exist, under which id, in which chain at which position (edges plan §22.1: *"the graph holds
/// STRUCTURE, not settings"*); every device's row — tag, identity, fields — lives in the `ChainState`
/// this publishes, in chain order, so there is nothing to translate at a publish. Every structural verb
/// updates both, and a case compares them (`consistentForTest`).
///
/// **A DEVICE THIS BUILD CANNOT RUN IS STILL A DEVICE OF THE RECORD.** A patch may hold a tag this build
/// does not know; it is kept, in its place, and saved back out — only not run. The graph needs a
/// descriptor per node, so such a node holds a placeholder (no roads, no controls) and the row keeps
/// the real tag. Only a LOAD can bring one in (`insertRow`); a hand cannot add what does not exist.
///
/// **AND THE ROUTES ARE THE RECORD'S TOO**, in the same graph — so a removal clears its device's routes
/// and a reorder deletes the ones the new order forbids by the graph's own rules, in the one place the
/// structure changes. A host publishes them (`publishRoutes`) beside the state.
///
/// EVERY VERB CHANGES NOTHING WHEN IT IS REFUSED. MESSAGE THREAD ONLY — the audio thread reads a
/// published copy and never this.
class ChainRecord {
public:
	static constexpr size_t kNoPosition = backplane::Graph::kNoPosition;
	static constexpr size_t kNoChain = static_cast<size_t>(-1);

	/// ALLOCATES. One chain per entry in `capacities`, each the most devices that chain may hold; they
	/// must sum to no more than `axes.deviceSlots` (a record whose do not is built with no chains, and
	/// refuses everything). `ids` is the top-level host's one counter (`graph.h`, D1) and must outlive
	/// this. `core` is the host's rules about its own axes (`routing.h`) — REQUIRED, and `permitAll` for
	/// a host with none, never a null meaning "permit".
	ChainRecord(const backplane::Axes& axes, std::span<const ChainCapacity> capacities,
	            const device::Catalogue& catalogue,
	            backplane::InstanceCounter& ids, backplane::CorePolicy core);

	ChainRecord(const ChainRecord&) = delete;
	ChainRecord& operator=(const ChainRecord&) = delete;

	//--------------------------------------------------------------------------------------------//
	// READS                                                                                        //
	//--------------------------------------------------------------------------------------------//

	/// **WHAT THE HOST PUBLISHES** — every chain's devices in order, current after every verb.
	[[nodiscard]] const ChainState& state() const noexcept { return state_; }

	/// BUMPED BY EVERY CHANGE, so a host can publish only when something moved.
	[[nodiscard]] uint64_t revision() const noexcept { return revision_; }

	/// HOW MANY TIMES THE EVALUATION ORDER THE STATE CARRIES HAS CHANGED (kill-lanes 4c) — what a bridge
	/// compares across a route write to know the chains must be published again: a route is a precedence,
	/// so it can move a device in the order without moving it in a chain.
	[[nodiscard]] uint64_t orderRevision() const noexcept { return orderRevision_; }

	[[nodiscard]] size_t chainCount() const noexcept { return capacities_.size(); }
	/// THE MOST DEVICES ONE CHAIN MAY HOLD.
	[[nodiscard]] size_t capacity(size_t chain) const noexcept {
		return chain < capacities_.size() ? capacities_[chain] : 0;
	}
	/// **MAKE ROOM FOR AT LEAST `atLeast` DEVICES IN `chain`** (unbounded devices step 5). MESSAGE THREAD, and it
	/// allocates: past the pool, the graph and the state are rebuilt bigger and everything they hold is carried
	/// across by identity (`Graph::assignGrown`, `ChainState::assignGrown`) — every device keeps its id and its
	/// lane. At least doubles, so a run of single adds costs a handful of rebuilds. A capacity never shrinks.
	/// Returns false only if the copy is refused, which a bigger graph of the same chains never does.
	bool growChain(size_t chain, size_t atLeast);
	[[nodiscard]] size_t length(size_t chain) const noexcept { return state_.length(chain); }

	/// THE DEVICE AT `position` IN `chain`, or `kNoInstance`.
	[[nodiscard]] DeviceInstanceId deviceAt(size_t chain, size_t position) const noexcept {
		return state_.at(chain, position).instanceId;
	}
	/// WHERE A DEVICE STANDS — its chain (`kNoChain`) and its place in it (`kNoPosition`).
	[[nodiscard]] size_t chainOf(DeviceInstanceId id) const noexcept;
	[[nodiscard]] size_t positionOf(DeviceInstanceId id) const noexcept;
	[[nodiscard]] bool holds(DeviceInstanceId id) const noexcept { return state_.holds(id); }

	/// THE ROW A DEVICE HAS — a dead row (`live()` false) for a device this record does not hold.
	[[nodiscard]] const DeviceState& rowOf(DeviceInstanceId id) const noexcept;
	/// WHETHER THE ROW ALREADY STORES `value` AT `field` — a container's mirror write (`OuterGate::setOwnField`) of the
	/// value already there is no edit, and is not made: an edit publishes and provisions (backlog NO).
	[[nodiscard]] bool storesField(DeviceInstanceId id, size_t field, float value) const noexcept {
		const DeviceState& row = rowOf(id);
		return row.live() && field < row.fieldsPresent && field < row.field.size() && row.field[field] == value;
	}
	/// WHAT A DEVICE RUNS AS — null for one this record does not hold AND for a tag this build does not know.
	[[nodiscard]] const device::Descriptor* descriptorOf(DeviceInstanceId id) const noexcept;
	/// WHAT A TAG WOULD RUN AS HERE — the record's catalogue, asked before a row is placed; null for a
	/// tag this build does not know.
	[[nodiscard]] const device::Descriptor* descriptorFor(const char* tag) const { return catalogue_(tag); }
	/// The catalogue the record builds from — and with it how many chains down this record's chains stand
	/// (`Catalogue::depth`), which is what the depth gate (`fitsNesting`) asks of every road that deepens.
	[[nodiscard]] const device::Catalogue& catalogue() const noexcept { return catalogue_; }
	[[nodiscard]] size_t depth() const noexcept { return catalogue_.depth; }

	/// THE STRUCTURE, for a host that asks routing questions of it (`axes()`, `routedElsewhere`).
	[[nodiscard]] const backplane::Graph& graph() const noexcept { return graph_; }

	//--------------------------------------------------------------------------------------------//
	// STRUCTURE                                                                                    //
	//--------------------------------------------------------------------------------------------//

	/// A HAND ADDS A DEVICE: `tag` at `position` in `chain` (clamped to the end), under a fresh id, with the
	/// device's DEFAULT row. `kNoInstance` — nothing changed — for a tag this build does not know, a full
	/// chain, or a bad chain.
	DeviceInstanceId insert(size_t chain, size_t position, const char* tag);

	/// A DEVICE PUT BACK EXACTLY — a load or an undo: this row, under its own id, at this place. Refused for
	/// a full or unknown chain, a position past its end, an id of `kNoInstance` or one this record already
	/// holds. The only road by which a tag this build does not know enters the record.
	bool insertRow(size_t chain, size_t position, const DeviceState& row);

	/// THE DEVICE GOES, its routes and its host slot bindings with it. Its id is never reissued.
	bool remove(DeviceInstanceId id);

	/// THE DEVICE AT `from` MOVES TO `to` WITHIN `chain` — a rotation, every device between shifting by
	/// one; each keeps its identity and its row.
	bool move(size_t chain, size_t from, size_t to);

	/// EVERY DEVICE GOES — before a load — and every route and host slot binding with them. Ids are never
	/// reissued by a hand, but a load re-reserves its saved ones, which is why no binding may survive this.
	void clear();

	/// **HOST SLOT `n` DRIVES `b`** (or nothing, for a dead `b`) — the slot service's write (T6), published with
	/// the chains. False for an entry the state refuses (`ChainState::setSlotBinding`: out of range, or a live
	/// binding naming another slot). Bumps the revision only when the entry changed.
	bool setSlotBinding(size_t n, const SlotBinding& b) noexcept;

	/// **THIS CHAIN BECOMES THESE ROWS, IN THIS ORDER — BY THE LEAST CHANGE.** A device named before and
	/// after under the same id KEEPS ITS NODE AND ITS ROUTES and has only its fields written; one no longer
	/// named is removed, its routes with it; one newly named is inserted; a row naming a held device's id
	/// under ANOTHER TAG replaces it in place (a different device's controls are not the old one's, so its
	/// routes go). Then the chain is put in order by moves, which delete the routes the new order forbids
	/// A host whose rules answer with a whole chain value (Feathers' `fx_chain.h`,
	/// `mod_rack.h`) writes its answer back through this, so a verb that did not touch a device does not
	/// touch its routes. Ids must be unique — a row repeating one is skipped, and counted in the return.
	size_t replaceChain(size_t chain, std::span<const DeviceState> rows);

	//--------------------------------------------------------------------------------------------//
	// ROWS                                                                                         //
	//--------------------------------------------------------------------------------------------//

	/// ONE FIELD — a control or a float of extra state. Widens the row's `fieldsPresent` to reach it.
	/// Refused for a device this record does not hold, a field past the row, or a value that is not finite.
	bool setField(DeviceInstanceId id, size_t field, float value);

	/// THE WHOLE ROW'S FIELDS — `fields` becomes the row, `fieldsPresent` its size. Identity is not
	/// touched: a row's tag and id change only by replacing the device. Refused as `setField` is.
	bool setFields(DeviceInstanceId id, std::span<const float> fields);

	/// **THE ROUTES THE LAST ROW WRITE DELETED, TAKEN** (backlog OP) — a write that changed what a device may reach
	/// (`ModOps::readsAudioOf`: the Follower's Mode) deletes the routes the new answer forbids, `move()`'s rule, and
	/// leaves them here for the verb that made the write to name in the tray and put back on undo. Each write of a
	/// row (`setField`, `setFields`, `insert`, `insertRow`) starts the list again, so what is taken is that write's.
	[[nodiscard]] std::vector<backplane::Route> takePrunedRoutes();

	//--------------------------------------------------------------------------------------------//
	// ROUTES — the graph's edges, every one of them, BY ENDPOINT (kill-lanes step 2): a device end is  //
	// the device's id and its own output or control index, a host end the host's own index.          //
	//--------------------------------------------------------------------------------------------//

	/// WHAT THIS RECORD ANSWERS ABOUT ITS DEVICES, BY ID — for the routing authority.
	[[nodiscard]] backplane::DeviceView devices() const noexcept;

	/// WHETHER `s` MAY REACH `d` — the routing rule (`backplane::mayRoute`: a live source, upstream of a
	/// live modulatable destination, and the host's own policy) asked of this record's structure. The one
	/// gate a gesture, a loader and this record's own `setRoute` share.
	[[nodiscard]] bool mayRoute(backplane::Source s, backplane::Destination d) const noexcept;

	// **NO ROUTE IS REFUSED FOR ROOM** (backlog OJ). The route cap (EA's `setRouteCap`/`fullFor`) is deleted: the
	// graph's edge store grows (`Graph::makeEdgeRoom`) and so does everything downstream of the publish.

	/// **THE HOST THIS CHAIN SITS IN** — rack roadmap §7. An inner route may name a source OUTSIDE this
	/// record (`Source::isOuter`); whether that source is upstream of this chain is the outer host's
	/// answer (`DeviceHost::sourceUpstreamOf`), asked through `devices()` by the gate `mayRoute` and
	/// `setRoute` share. Null standalone, and then every outer source is refused.
	void setOuterGate(const device::OuterGate& gate) noexcept { outer_ = gate; }
	/// **DROP EVERY ROUTE WHOSE OUTER SOURCE THE GATE NO LONGER ADMITS** — the outer device removed, or
	/// moved downstream of this chain. Asked on the host's beat (`ContainerOps::service`). Returns what went, so it
	/// can be said; nothing changes when nothing went.
	device::RouteDrop pruneOuterRoutes();
	[[nodiscard]] bool hasOuterGate() const noexcept { return outer_.upstream != nullptr; }
	/// **THE GATE A HOST HANDS A CHAIN IT HOLDS**, over this record — the maker `Converger` wants, for
	/// every host whose record this is (Feathers' bridge, the rack's, a rack in a rack).
	static device::OuterGate gateOver(const void* record, DeviceInstanceId self) noexcept {
		device::OuterGate gate;
		gate.host = record;
		gate.self = self;
		gate.upstream = [](const void* host, DeviceInstanceId who, backplane::Source s) {
			return static_cast<const ChainRecord*>(host)->sourceUpstreamOf(s, who);
		};
		return gate;
	}
	/// THE ANSWER THIS RECORD GIVES A CHAIN IT HOLDS — `backplane::sourceUpstreamOf` over this record.
	[[nodiscard]] bool sourceUpstreamOf(backplane::Source s, DeviceInstanceId self) const noexcept {
		return backplane::sourceUpstreamOf(graph_.axes(), devices(), s, self);
	}

	/// HOW FAR `s` REACHES `d`, or 0.
	[[nodiscard]] float routeAmount(backplane::Source s, backplane::Destination d) const noexcept {
		return graph_.amountOf(s, d);
	}

	/// MAKE, CHANGE OR REMOVE ONE ROUTE — zero removes (a route that reaches nowhere is not a route).
	/// Refused, with nothing changed, for a pair `mayRoute` refuses (a removal is never refused), a value
	/// that is not finite, or a graph with no room for another edge. **CLAMPED TO `device::kMaxAmount`
	/// each way** — the one reach every host stores an amount by, a hand's or a file's, as `coerceControl`
	/// is for a base.
	bool setRoute(backplane::Source s, backplane::Destination d, float amount);
	/// **THE CURVE ON AN EXISTING ROUTE** (route transfer chips) — refused for a pair with
	/// no route, since zero is not a route. Not a precedence: the order and its revision do not move.
	bool setRouteTransfer(backplane::Source s, backplane::Destination d, backplane::Transfer t) noexcept {
		if (!graph_.setTransfer(s, d, t)) {
			return false;
		}
		++revision_;
		return true;
	}
	/// THE CURVE `s` REACHES `d` THROUGH — Lin for no route.
	[[nodiscard]] backplane::Transfer routeTransfer(backplane::Source s, backplane::Destination d) const noexcept {
		return graph_.transferOf(s, d);
	}
	/// EVERY ROUTE GOES — a device's and the host's own alike; the devices stay.
	void clearRoutes();

	/// **EVERY ROUTE, BY IDENTITY** — `Graph::publishRouteList`: what a host publishes across the thread
	/// boundary, each end a device's id or the host's own index. Into `out`, which
	/// it grows (backlog OJ). The resolving side builds its nodes' inputs from it (`RackEngine::rebuildInputs`).
	void publishRoutes(backplane::RouteList& out) const noexcept { graph_.publishRouteList(out); }

	//--------------------------------------------------------------------------------------------//
	// TEST SEAMS                                                                                   //
	//--------------------------------------------------------------------------------------------//

	/// WHETHER THE GRAPH AND THE STATE SAY THE SAME THING — every chain's devices, in order, id for id.
	/// The invariant every verb keeps; a case asks it after each.
	[[nodiscard]] bool consistentForTest() const;

private:
	/// THE STATE'S ROWS, FROM THE GRAPH'S ORDER — called by every structural verb, so the two cannot drift.
	/// Carries each device's row across by identity, so a move costs no row.
	void syncOrder() noexcept;
	/// WRITE `graph_.order()` INTO THE STATE (kill-lanes 4c), after every mutation; counts a change.
	void syncEvaluationOrder() noexcept;
	[[nodiscard]] size_t freeLane() const noexcept;
	[[nodiscard]] DeviceState* mutableRowOf(DeviceInstanceId id) noexcept;
	/// TELL THE GRAPH WHETHER THE DEVICE READS ITS AUDIO, BY ITS ROW NOW — routes that forbids go into `pruned_`.
	void syncReadsAudio(DeviceInstanceId id);

	device::Catalogue catalogue_;
	std::vector<ChainCapacity> capacities_;
	/// THE HOST'S COUNTER — `insert` issues from it directly rather than letting `Graph::add` do it, so
	/// that a refused insert can hand the id back to nothing rather than spending it.
	backplane::InstanceCounter* ids_;
	backplane::CorePolicy core_;
	/// See `setOuterGate`.
	device::OuterGate outer_ {};
	backplane::Graph graph_;
	ChainState state_;
	/// SCRATCH FOR `syncOrder`, so a verb on the message thread does not allocate per call.
	std::vector<DeviceState> carried_;
	uint64_t revision_ = 0;
	/// **A CURVE OUTLIVES ITS ROUTE GOING TO ZERO**. Zero is not a
	/// route, so an amount dragged through zero, a removal and its undo would each come back Lin — every one of
	/// those roads writes an AMOUNT and knows nothing of curves. So a removal that drops a non-Lin curve leaves
	/// it here, and the next route made between the same two ends takes it back. Bounded; cleared with the
	/// routes. Message thread, as every verb here.
	std::vector<backplane::Route> rememberedTransfers_;
	static constexpr size_t kRememberedTransfers = 64;
	/// See `takePrunedRoutes`.
	std::vector<backplane::Route> pruned_;
	uint64_t orderRevision_ = 0;
};

/// **A CONTAINER'S OWN CONTROLS, SERVICED** (backlog NO: a Rack's chain slot params) — on the host's service beat, never
/// inside its provisioning. Two jobs per container in `record`:
/// - THE CONTROLS IT GAVE UP, PRUNED: every route into each control it reports (`ContainerOps::releasedControls`) is
///   removed through `remove(source, destination)` (the host's own route verb, so it publishes and marks as any removal
///   does), and only then is it told (`controlsReleased`) that it may hand those controls to another;
/// - THE MIRROR IT OWES, WRITTEN (`serviceMirror`): after a load or a new gate, its controls' bases put to its body.
/// The containers are gathered first: both jobs are host edits, and an edit may provision. Returns how many routes went.
template <typename Remove>
size_t serviceOwnControls(const ChainRecord& record, const ::feathers::ContainerBodies& bodies, Remove&& remove) {
	struct Site {
		const device::ContainerOps* ops;
		DeviceInstanceId id;
		void* surface;
	};
	std::vector<Site> sites;
	const ChainState& state = record.state();
	for (size_t c = 0; c < state.chainCount(); ++c) {
		for (const DeviceState& row : state.chain(c)) {
			const device::Descriptor* d = record.descriptorOf(row.instanceId);
			if (d == nullptr || d->container == nullptr
			    || (d->container->releasedControls == nullptr && d->container->serviceMirror == nullptr
			        && d->container->adoptOwnFields == nullptr)) {
				continue;
			}
			if (void* surface = bodies.surfaceOf(row.instanceId); surface != nullptr) {
				sites.push_back({ d->container, row.instanceId, surface });
			}
		}
	}
	size_t gone = 0;
	for (const Site& site : sites) {
		std::array<size_t, 3 * 64> controls {};
		const size_t n = site.ops->releasedControls != nullptr
		                     ? site.ops->releasedControls(site.surface, controls.data(), controls.size())
		                     : 0;
		if (n > 0) {
			const auto first = controls.begin();
			const auto last = controls.begin() + static_cast<std::ptrdiff_t>(n);
			std::vector<backplane::Route> doomed;
			record.graph().forEachRoute([&](const backplane::Route& r) {
				if (r.destination.isDevice() && r.destination.device == site.id
				    && std::find(first, last, r.destination.index) != last) {
					doomed.push_back(r);
				}
			});
			for (const backplane::Route& r : doomed) {
				remove(r.source, r.destination);
				++gone;
			}
			if (site.ops->controlsReleased != nullptr) {
				site.ops->controlsReleased(site.surface);
			}
		}
		// A BASE THE HOST MOVED (a DAW on the control's host slot) INTO THE BODY, then whatever mirror is owed.
		if (site.ops->adoptOwnFields != nullptr) {
			const DeviceState& row = record.rowOf(site.id);
			if (row.live()) {
				(void)site.ops->adoptOwnFields(site.surface, row.field.data(), std::min(row.fieldsPresent, row.field.size()));
			}
		}
		if (site.ops->serviceMirror != nullptr) {
			(void)site.ops->serviceMirror(site.surface);
		}
	}
	return gone;
}

} // namespace feathers::rack
