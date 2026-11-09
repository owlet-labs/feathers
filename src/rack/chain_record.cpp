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

#include "rack/chain_record.h"

#include "backplane/device_host.h"

#include "device/modulatable.h"
#include "rack/rack_device.h"

#include <algorithm>
#include <cmath>

namespace feathers::rack {

namespace {

/// THE NODE A DEVICE THIS BUILD CANNOT RUN IS GIVEN IN THE GRAPH — no roads, no controls, no outputs. The
/// row keeps the real tag; nothing ever runs this.
const device::Descriptor& unknownDevice() noexcept {
	static const device::Descriptor kUnknown {};
	return kUnknown;
}

/// THE CAPACITIES, IF THEY FIT THE POOL. Empty otherwise: a record whose chains together claim more
/// slots than exist is built with no chains and refuses every verb, rather than accepting devices it
/// has nowhere to park.
///
/// **IT VALIDATED A PARTITION AND NOW VALIDATES A SUM (kill-lanes step 4d).** Bands had to lie inside
/// the axis AND not overlap, because two chains sharing a slot meant two devices at one address. There
/// are no addresses: the pool is one, a device goes in the lowest free slot, and the only thing that can
/// go wrong is promising more room than there is.
std::vector<ChainCapacity> validCapacities(std::span<const ChainCapacity> capacities, size_t lanes) {
	size_t total = 0;
	for (const ChainCapacity c : capacities) {
		if (c == 0 || c > lanes - total) {
			return {};
		}
		total += c;
	}
	return std::vector<ChainCapacity>(capacities.begin(), capacities.end());
}

bool allFinite(std::span<const float> v) noexcept {
	return std::all_of(v.begin(), v.end(), [](float x) { return std::isfinite(x); });
}

} // namespace

ChainRecord::ChainRecord(const backplane::Axes& axes, std::span<const ChainCapacity> capacities,
                         const device::Catalogue& catalogue, backplane::InstanceCounter& ids,
                         backplane::CorePolicy core)
    : catalogue_(catalogue), capacities_(validCapacities(capacities, axes.deviceSlots)), ids_(&ids),
      core_(core != nullptr ? core : &backplane::permitAll),
      graph_(axes, capacities_.size(), backplane::Graph::edgeCapacityFor(axes), ids),
      state_(axes.deviceSlots, capacities_.size()) {
	carried_.reserve(axes.deviceSlots);
	// **A HOST WITH CORE DESTINATIONS HAS A NODE THAT OWNS THEM (kill-lanes step 4a).** Feathers' voice
	// is that node: a route into one of its parameters is a precedence into it, and it precedes the head
	// of every audio chain. That is what joins a strip modulator to an fx device — before it, the two
	// chains were disconnected components and a tie-break inside the sort was the only thing ordering
	// them. A host with no core destinations (the standalone rack, whose macros are inputs) declares
	// none, and every rule it induces is inert there.
	if (axes.coreDestinations > 0) {
		(void)graph_.setCoreNode();
	}
}

backplane::DeviceView ChainRecord::devices() const noexcept {
	// **WHAT THE DEVICE IS, AS THIS BUILD KNOWS IT — NULL FOR ONE IT DOES NOT HAVE.** The graph holds a
	// device this build cannot name under a tagless placeholder (`unknownDevice()`), so its node has a
	// descriptor to order by; handing THAT to a surface drew an empty generic card where "device missing"
	// belongs. The catalogue's answer is the one a card and the routing authority both want: null, which
	// is also no outputs and no controls to route. The rank is still the graph's.
	backplane::DeviceView view { this,
		                         [](const void* h, DeviceInstanceId id) -> const device::Descriptor* {
			                         return static_cast<const ChainRecord*>(h)->descriptorOf(id);
		                         },
		                         [](const void* h, backplane::Source s, backplane::Destination d) {
			                         return static_cast<const ChainRecord*>(h)->graph_.wouldCycle(s, d);
		                         } };
	// AND THE HOST OUTSIDE, when this record is a chain's (rack roadmap §7, `setOuterHost`).
	if (outer_.upstream != nullptr) {
		view.outerHost = this;
		view.outerUpstream = [](const void* h, backplane::Source s) {
			const auto* record = static_cast<const ChainRecord*>(h);
			return record->outer_.upstream != nullptr && record->outer_.upstream(record->outer_.host, record->outer_.self, s);
		};
	}
	return view;
}

bool ChainRecord::mayRoute(backplane::Source s, backplane::Destination d) const noexcept {
	return backplane::mayRoute(graph_.axes(), devices(), s, d, core_);
}

bool ChainRecord::setRoute(backplane::Source s, backplane::Destination d, float amount) {
	if (!std::isfinite(amount)) {
		return false;
	}
	amount = std::clamp(amount, -device::kMaxAmount, device::kMaxAmount);
	// A REMOVAL IS NEVER REFUSED — whatever the rules say of a pair now, taking a route away cannot break
	// them, and a route whose device has gone went with it — and a route the rules refuse is never written.
	if (amount == 0.0f) {
		const backplane::Transfer curve = graph_.transferOf(s, d);
		if (graph_.amountOf(s, d) != 0.0f && graph_.route(s, d, 0.0f)) {
			// THE CURVE IS KEPT for the next route between these ends — see `rememberedTransfers_`.
			if (curve != backplane::Transfer::Lin) {
				if (rememberedTransfers_.size() >= kRememberedTransfers) {
					rememberedTransfers_.erase(rememberedTransfers_.begin());
				}
				rememberedTransfers_.push_back(backplane::Route { s, d, 0.0f, curve });
			}
			syncEvaluationOrder();
			++revision_;
		}
		return true;
	}
	// THROUGH THE AUTHORITY, THEN THE GRAPH'S DOOR, which refuses a backwards or self route by construction
	// (`reaches(to, from)`) — the rule a second time, as the engine had it.
	const bool made = graph_.amountOf(s, d) == 0.0f;
	if (!mayRoute(s, d) || !graph_.route(s, d, amount)) {
		return false;
	}
	if (made) {
		for (auto it = rememberedTransfers_.begin(); it != rememberedTransfers_.end(); ++it) {
			if (it->source == s && it->destination == d) {
				(void)graph_.setTransfer(s, d, it->transfer);
				rememberedTransfers_.erase(it);
				break;
			}
		}
	}
	syncEvaluationOrder();
	++revision_;
	return true;
}

device::RouteDrop ChainRecord::pruneOuterRoutes() {
	device::RouteDrop dropped;
	// GATHERED FIRST, REMOVED AFTER: a removal swaps edges under an enumeration.
	std::vector<backplane::Route> gone;
	graph_.forEachRoute([&](const backplane::Route& r) {
		if (r.source.isOuter() && !mayRoute(r.source, r.destination)) {
			gone.push_back(r);
		}
	});
	for (const backplane::Route& r : gone) {
		if (graph_.route(r.source, r.destination, 0.0f)) {
			if (dropped.count == 0) {
				dropped.firstSource = r.source;
				dropped.firstDestination = r.destination;
			}
			++dropped.count;
		}
	}
	if (dropped.count > 0) {
		syncEvaluationOrder();
		++revision_;
	}
	return dropped;
}

void ChainRecord::clearRoutes() {
	graph_.clearRoutes();
	rememberedTransfers_.clear();
	syncEvaluationOrder();
	++revision_;
}

//------------------------------------------------------------------------------------------------//
// READS                                                                                          //
//------------------------------------------------------------------------------------------------//

size_t ChainRecord::chainOf(DeviceInstanceId id) const noexcept {
	size_t chain = 0;
	return state_.find(id, &chain, nullptr) ? chain : kNoChain;
}

size_t ChainRecord::positionOf(DeviceInstanceId id) const noexcept {
	size_t position = 0;
	return state_.find(id, nullptr, &position) ? position : kNoPosition;
}

const DeviceState& ChainRecord::rowOf(DeviceInstanceId id) const noexcept {
	static const DeviceState kNone {};
	size_t chain = 0;
	size_t position = 0;
	return state_.find(id, &chain, &position) ? state_.at(chain, position) : kNone;
}

DeviceState* ChainRecord::mutableRowOf(DeviceInstanceId id) noexcept {
	size_t chain = 0;
	size_t position = 0;
	return state_.find(id, &chain, &position) ? state_.mutableAt(chain, position) : nullptr;
}

const device::Descriptor* ChainRecord::descriptorOf(DeviceInstanceId id) const noexcept {
	const DeviceState& row = rowOf(id);
	return row.live() ? catalogue_(row.tag.data()) : nullptr;
}

/// **THE LOWEST FREE SLOT OF THE ONE POOL (kill-lanes step 4d).** It searched a chain's own BAND, because
/// a chain owned a contiguous run of slots and a device of that chain had to be inside it — the number
/// said which chain the device belonged to. Nothing reads a slot that way any more, so there is one pool
/// and a chain's CAPACITY is enforced where it belongs, on the chain's length, not on where a device
/// happens to land.
size_t ChainRecord::freeLane() const noexcept {
	for (size_t lane = 0; lane < graph_.capacity(); ++lane) {
		if (!graph_.nodeAtLane(lane).live()) {
			return lane;
		}
	}
	return backplane::Graph::kNoLane;
}

//------------------------------------------------------------------------------------------------//
// STRUCTURE                                                                                      //
//------------------------------------------------------------------------------------------------//

void ChainRecord::syncEvaluationOrder() noexcept {
	// THE GRAPH'S ORDER, ONE ID PER STEP, INTO THE STATE THE AUDIO THREAD WILL BE HANDED. Compared before it
	// is written so `orderRevision_` moves only when the order did — a route between two devices the order
	// already had the right way round is not a reorder, and a bridge should not publish the chains for it.
	const std::span<const backplane::Step> steps = graph_.order();
	const std::span<const DeviceInstanceId> was = state_.order();
	bool same = was.size() == steps.size();
	for (size_t i = 0; same && i < steps.size(); ++i) {
		same = was[i] == steps[i].id;
	}
	if (same) {
		return;
	}
	const std::span<DeviceInstanceId> ids = state_.resizeOrder(steps.size());
	for (size_t i = 0; i < ids.size(); ++i) {
		ids[i] = steps[i].id;
	}
	++orderRevision_;
}

void ChainRecord::syncOrder() noexcept {
	// **THE GRAPH SAYS THE ORDER; THE STATE CARRIES THE ROWS.** Every row this record holds is taken out
	// by identity and put back where the graph now names its device, so a move, an insert or a removal
	// costs no row its settings and the two structures cannot drift. A device the graph names and no row
	// answers for (it cannot happen; a verb writes its row before this) comes back as a default row under
	// its own id rather than as nothing, so the state never disagrees about WHICH devices exist.
	carried_.clear();
	for (size_t c = 0; c < state_.chainCount(); ++c) {
		const std::span<const DeviceState> path = state_.chain(c);
		carried_.insert(carried_.end(), path.begin(), path.end());
	}
	state_.clear();
	for (size_t c = 0; c < chainCount(); ++c) {
		for (const DeviceInstanceId id : graph_.chain(c)) {
			const auto held = std::find_if(carried_.begin(), carried_.end(),
			                               [id](const DeviceState& row) { return row.instanceId == id; });
			DeviceState row;
			if (held != carried_.end()) {
				row = *held;
			}
			else {
				row.instanceId = id;
			}
			(void)state_.append(c, row);
		}
	}
}

DeviceInstanceId ChainRecord::insert(size_t chain, size_t position, const char* tag) {
	if (chain >= chainCount() || tag == nullptr) {
		return kNoInstance;
	}
	const device::Descriptor* desc = catalogue_(tag);
	// THE DEPTH GATE (2026-10-31): a container adds a chain below this one, and that chain must be one a file gives
	// back full (`fitsNesting`). Every road that adds a device by tag lands here — the add menu of every surface, a
	// script, a test — so the gate cannot be a surface's alone.
	if (desc != nullptr && desc->container != nullptr && !fitsNesting(depth(), 1)) {
		return kNoInstance;
	}
	// THE CHAIN'S CAPACITY IS ON ITS LENGTH NOW, not on a band's width.
	// NEVER FULL (unbounded devices step 6): a chain at its capacity grows, and refuses only if it cannot.
	if (length(chain) >= capacity(chain) && !growChain(chain, length(chain) + 1)) {
		return kNoInstance;
	}
	const size_t lane = freeLane();
	if (desc == nullptr || lane == backplane::Graph::kNoLane) {
		return kNoInstance;
	}
	const DeviceInstanceId id = ids_->issue();
	if (!graph_.restore(*desc, id, lane)) {
		return kNoInstance;
	}
	if (!graph_.insert(chain, position, id)) {
		graph_.remove(id); // the id is spent — ids are never reissued, and nothing else changed
		return kNoInstance;
	}
	// THE DEVICE'S DEFAULT ROW: every declared control at its default, extra state at zero. Written into
	// the state at the graph's own position, which is where `syncOrder` then reads it back from.
	DeviceState row;
	// THROUGH `tagOf`: the caller hands a string literal, so copying the field's full width off it
	// reads past its end. Harmless while both were four; an overread the day the field widened.
	row.tag = device::tagOf(tag);
	row.instanceId = id;
	row.fieldsPresent = std::min(desc->params.size(), row.field.size());
	for (size_t c = 0; c < row.fieldsPresent; ++c) {
		row.field[c] = static_cast<float>(desc->params[c].def);
	}
	// **THE ROW GOES IN BEFORE THE ORDER IS TAKEN FROM THE GRAPH, AND A REFUSAL HERE UNDOES THE NODE.**
	// `syncOrder` carries rows across BY IDENTITY, so a device the graph names and no row answers for
	// would come back under a default row — an empty tag, silently. It cannot happen (the state's chains
	// are as wide as the whole pool and the id is fresh), and it is unwound rather than trusted.
	if (!state_.append(chain, row)) {
		graph_.remove(id);
		return kNoInstance;
	}
	syncOrder();
	pruned_.clear();
	syncReadsAudio(id); // the default row's answer; a fresh node has no route to prune
	syncEvaluationOrder();
	++revision_;
	return id;
}

bool ChainRecord::insertRow(size_t chain, size_t position, const DeviceState& row) {
	if (chain >= chainCount() || !row.live() || state_.holds(row.instanceId)
	    || row.fieldsPresent > row.field.size() || !allFinite(row.field)) {
		return false;
	}
	// THE CHAIN'S CAPACITY IS ON ITS LENGTH NOW, and a chain at it grows (unbounded devices step 6).
	if (length(chain) >= capacity(chain) && !growChain(chain, length(chain) + 1)) {
		return false;
	}
	const size_t lane = freeLane();
	if (lane == backplane::Graph::kNoLane || graph_.laneOf(row.instanceId) != backplane::Graph::kNoLane) {
		return false;
	}
	const device::Descriptor* desc = catalogue_(row.tag.data());
	if (!graph_.restore(desc != nullptr ? *desc : unknownDevice(), row.instanceId, lane)) {
		return false;
	}
	if (!graph_.insert(chain, position, row.instanceId)) {
		graph_.remove(row.instanceId);
		return false;
	}
	// SEE `insert`: a row the state could not take is a node with no row, and that is unwound, not trusted.
	if (!state_.append(chain, row)) {
		graph_.remove(row.instanceId);
		return false;
	}
	syncOrder();
	// A LOAD OR AN UNDO PUTS A FOLLOWER BACK IN ITS MODE BEFORE ITS ROUTES ARRIVE, so the gate asks the right question.
	pruned_.clear();
	syncReadsAudio(row.instanceId);
	syncEvaluationOrder();
	++revision_;
	return true;
}

bool ChainRecord::remove(DeviceInstanceId id) {
	if (!state_.holds(id) || !graph_.remove(id)) {
		return false;
	}
	// **ITS HOST SLOT BINDINGS GO WITH IT** (host slots, T5 review). Left standing they would name an id no
	// device holds — harmless only until a load re-reserves that id for an unrelated device, which the bus
	// would then overwrite on every drain. The pool (T6) frees the slot itself and restores it on undo.
	(void)state_.dropSlotBindingsOf(id);
	syncOrder();
	syncEvaluationOrder();
	++revision_;
	return true;
}

bool ChainRecord::move(size_t chain, size_t from, size_t to) {
	if (!graph_.move(chain, from, to).has_value()) {
		return false;
	}
	if (from != to) {
		syncOrder();
		syncEvaluationOrder();
		++revision_;
	}
	return true;
}

void ChainRecord::clear() {
	for (size_t c = 0; c < chainCount(); ++c) {
		while (state_.length(c) > 0) {
			const DeviceInstanceId id = state_.at(c, state_.length(c) - 1).instanceId;
			if (!graph_.remove(id)) { // and every route into or out of it
				break;
			}
			(void)state_.erase(c, state_.length(c) - 1);
		}
	}
	// AND THE ROUTES NO DEVICE OWNED — host source to host destination, which no removal reaches.
	graph_.clearRoutes();
	rememberedTransfers_.clear();
	// **AND EVERY HOST SLOT BINDING** (host slots, T5 review). This is the patch-load road
	// (`applyPatchToRecord`), and a load re-reserves the saved instance ids: a binding left standing would
	// re-attach to whatever unrelated device the patch puts under the same id. The pool republishes the
	// bindings it still owns after the load (T6).
	state_.clearSlotBindings();
	syncOrder();
	syncEvaluationOrder();
	++revision_;
}

bool ChainRecord::setSlotBinding(size_t n, const SlotBinding& b) noexcept {
	if (n < state_.slotBindings().size() && state_.slotBindings()[n] == b) {
		return true; // what it already says: not an edit
	}
	if (!state_.setSlotBinding(n, b)) {
		return false;
	}
	++revision_;
	return true;
}

size_t ChainRecord::replaceChain(size_t chain, std::span<const DeviceState> rows) {
	if (chain >= chainCount()) {
		return rows.size();
	}
	const auto wantedAt = [&](DeviceInstanceId id) -> size_t {
		for (size_t i = 0; i < rows.size(); ++i) {
			if (rows[i].instanceId == id) {
				return i;
			}
		}
		return rows.size();
	};
	// 1. WHAT IS NO LONGER NAMED GOES — a device the new rows do not name, or name under another tag.
	for (size_t p = length(chain); p-- > 0;) {
		const DeviceState& held = state_.at(chain, p);
		const size_t i = wantedAt(held.instanceId);
		if (i == rows.size() || rows[i].tag != held.tag) {
			(void)remove(held.instanceId);
		}
	}
	// 2. WHAT STAYS HAS ITS FIELDS WRITTEN; WHAT IS NEW IS INSERTED (at the end — step 3 places it).
	size_t skipped = 0;
	for (size_t i = 0; i < rows.size(); ++i) {
		if (state_.holds(rows[i].instanceId)) {
			if (chainOf(rows[i].instanceId) != chain) {
				++skipped; // a device another chain holds is not this chain's to name
				continue;
			}
			(void)setFields(rows[i].instanceId, std::span<const float>(rows[i].field.data(),
			                                                           std::min(rows[i].fieldsPresent,
			                                                                    rows[i].field.size())));
			continue;
		}
		if (!insertRow(chain, length(chain), rows[i])) {
			++skipped;
		}
	}
	// 3. THE ORDER, BY MOVES — each a rotation, and each deleting the routes it makes backwards.
	size_t target = 0;
	for (size_t i = 0; i < rows.size(); ++i) {
		const size_t at = positionOf(rows[i].instanceId);
		if (at == kNoPosition || chainOf(rows[i].instanceId) != chain) {
			continue; // a row that was skipped above
		}
		if (at != target) {
			(void)move(chain, at, target);
		}
		++target;
	}
	return skipped;
}

//------------------------------------------------------------------------------------------------//
// ROWS                                                                                           //
//------------------------------------------------------------------------------------------------//

bool ChainRecord::setField(DeviceInstanceId id, size_t field, float value) {
	DeviceState* row = std::isfinite(value) ? mutableRowOf(id) : nullptr;
	if (row == nullptr || field >= row->field.size()) {
		return false;
	}
	// **WIDENED WITH DECLARED DEFAULTS, NEVER ZEROS** (found 2026-10-31 by NZ any depth: a Rack made by "Group into Rack"
	// has a row of no fields, and the first write past them — its chain-slot mirror — left Enabled, Mix and the rest at
	// 0, so the new Rack stood DISABLED). Absent is the declared default (`device_row.h`'s rule); the fields this write
	// makes present below `field` keep meaning what they meant.
	if (field >= row->fieldsPresent) {
		const device::Descriptor* d = descriptorOf(id);
		for (size_t i = row->fieldsPresent; i < field && i < row->field.size(); ++i) {
			row->field[i] = d != nullptr && i < d->params.size() ? static_cast<float>(d->params[i].def) : 0.0f;
		}
	}
	row->field[field] = value;
	row->fieldsPresent = std::max(row->fieldsPresent, field + 1);
	pruned_.clear();
	syncReadsAudio(id);
	syncEvaluationOrder();
	++revision_;
	return true;
}

void ChainRecord::syncReadsAudio(DeviceInstanceId id) {
	const device::Descriptor* d = descriptorOf(id);
	if (d == nullptr || d->mod == nullptr || d->mod->readsAudioOf == nullptr) {
		return; // a static declaration, set when the node was placed
	}
	const DeviceState& row = rowOf(id);
	const size_t n = std::min(row.fieldsPresent, row.field.size());
	(void)graph_.setReadsAudio(id, device::modReadsAudio(*d, row.field.data(), n), &pruned_);
}

std::vector<backplane::Route> ChainRecord::takePrunedRoutes() {
	std::vector<backplane::Route> out;
	out.swap(pruned_);
	return out;
}

bool ChainRecord::setFields(DeviceInstanceId id, std::span<const float> fields) {
	DeviceState* row = allFinite(fields) ? mutableRowOf(id) : nullptr;
	if (row == nullptr || fields.size() > row->field.size()) {
		return false;
	}
	std::copy(fields.begin(), fields.end(), row->field.begin());
	std::fill(row->field.begin() + static_cast<std::ptrdiff_t>(fields.size()), row->field.end(), 0.0f);
	row->fieldsPresent = fields.size();
	pruned_.clear();
	syncReadsAudio(id);
	syncEvaluationOrder();
	++revision_;
	return true;
}

bool ChainRecord::consistentForTest() const {
	for (size_t c = 0; c < chainCount(); ++c) {
		const std::span<const DeviceInstanceId> ids = graph_.chain(c);
		const std::span<const DeviceState> rows = state_.chain(c);
		if (ids.size() != rows.size()) {
			return false;
		}
		for (size_t p = 0; p < ids.size(); ++p) {
			if (rows[p].instanceId != ids[p] || graph_.laneOf(ids[p]) == backplane::Graph::kNoLane) {
				return false;
			}
		}
	}
	return true;
}

} // namespace feathers::rack

namespace feathers::rack {

bool ChainRecord::growChain(size_t chain, size_t atLeast) {
	if (chain >= chainCount() || atLeast <= capacities_[chain]) {
		return chain < chainCount();
	}
	const size_t want = std::max(atLeast, 2 * capacities_[chain]);
	size_t total = want;
	for (size_t c = 0; c < chainCount(); ++c) {
		total += c == chain ? 0 : capacities_[c];
	}
	if (total > graph_.capacity()) {
		// THE POOL ITSELF IS TOO SMALL: a bigger graph and a bigger state, everything carried across.
		backplane::Axes axes = graph_.axes();
		axes.deviceSlots = std::max(total, 2 * graph_.capacity());
		backplane::Graph bigger(axes, chainCount(), std::max(backplane::Graph::edgeCapacityFor(axes), graph_.edgeCapacity()), *ids_);
		if (!bigger.assignGrown(graph_)) {
			return false;
		}
		ChainState grown(axes.deviceSlots, chainCount());
		if (!grown.assignGrown(state_)) {
			return false;
		}
		graph_ = std::move(bigger);
		state_ = std::move(grown);
		carried_.reserve(axes.deviceSlots);
		++revision_;
	}
	capacities_[chain] = want;
	return true;
}

} // namespace feathers::rack
