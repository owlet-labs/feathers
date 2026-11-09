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
 */

#include "rack/rack_bridge.h"
#include "rack/export_table.h"
#include "rack/extra_state_harvest.h"
#include "rack/nesting.h"
#include "rack/rack_device.h"
#include "rack/site_chains.h"
#include "rack/site_routes.h"

#include "composite/composite_types.h" // `Param::Macro1`: a nested rack's macros are its device's controls (T11)

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <utility>

namespace feathers::rack {

namespace {

/// A playhead packed into one word, so the ordinal and the phase cannot be read from two different
/// blocks. The same packing `ParamBridge::publishRackPlayhead` uses, and for the same reason.
uint64_t packPlayhead(uint32_t ordinal, float phase) noexcept {
	const float clamped = std::isfinite(phase) ? std::clamp(phase, 0.0f, 1.0f) : 0.0f;
	return (static_cast<uint64_t>(ordinal) << 32) | std::bit_cast<uint32_t>(clamped);
}

void storeFloat(float& cell, float value) noexcept {
	std::atomic_ref<float>(cell).store(value, std::memory_order_relaxed);
}

void storeSize(size_t& cell, size_t value) noexcept {
	std::atomic_ref<size_t>(cell).store(value, std::memory_order_relaxed);
}

/// THE RACK'S ONE CHAIN, holding the whole pool — the record's private band, which nothing orders by.
std::array<ChainCapacity, 1> wholeRack(const RackEngine& engine) noexcept {
	return { engine.slotCount() };
}

} // namespace

RackBridge::RackBridge(RackEngine& engine, backplane::InstanceCounter* shared, HostSlots slots)
    : engine_(engine), ids_(shared != nullptr ? *shared : own_),
      record_(engine.axes(), wholeRack(engine), engine.catalogue(), ids_, engine.corePolicy()),
      // **THE RECORD'S OWN SHAPE, ASKED OF THE RECORD** — a publish and the drain's two states must have the
      // shape the record's `ChainState` has or `publish`/`adopt` refuse every time, silently.
      published_(record_.state().capacity(), record_.state().chainCount()),
      publishedRoutes_(),
      converger_(engine, &Converger::fitsAnywhere),
      drained_(record_.state().capacity(), record_.state().chainCount()),
      drainStaging_(record_.state().capacity(), record_.state().chainCount()),
      routeStaging_(engine.routeRoom()) {
	// THE DRAIN'S STAGE AND THE ENGINE'S STORES GROW WITH THE LIST, BEFORE IT PUBLISHES A COUNT THAT NEEDS THEM
	// (backlog OJ): the drain then always has room for a whole snapshot.
	publishedRoutes_.setGrowthHook(
	    [](void* host, size_t capacity) {
		    auto* bridge = static_cast<RackBridge*>(host);
		    bridge->routeStaging_.grow(capacity);
		    bridge->engine_.reserveRoutes(capacity);
	    },
	    this);
	// A CHAIN THIS RACK PROVISIONS IS TOLD THIS RACK IS ITS HOST (rack roadmap §7): the record's answer
	// to "is that source upstream of you", handed at provision, before the chain's body loads.
	converger_.setOuterGateMaker(
	    [](const void* host, DeviceInstanceId self) { return static_cast<const RackBridge*>(host)->outerGateFor(self); },
	    this);
	// **THE OUTERMOST RACK OWNS THE HOST SLOTS** (T10), and the engine reads their values from its bus for
	// this bridge's whole life — handed before any block runs, as Feathers hands its lane the bridge's.
	if (slots == HostSlots::OwnPool) {
		slots_ = std::make_unique<SlotOwner>(record_);
		macroOutbox_ = std::make_unique<MacroOutbox>(); // its macros are the plugin's host parameters (NG)
		engine_.setHostSlotBus(&slots_->bus());
		// A NESTED VERB THAT CHANGED THE POOL (T11) MARKS THIS PATCH, where the pool is saved.
		slots_->setDirtyHook(this, [](void* host) { static_cast<RackBridge*>(host)->markStateDirty(); });
	}
	else {
		// **A RACK WITH NO POOL OF ITS OWN TAKES ITS SLOTS FROM THE OUTERMOST PLUGIN'S** (T11), once a host links it.
		nested_ = std::make_unique<NestedSlots>(record_);
	}
	// AND A CONTAINER THIS RACK PROVISIONS IS HANDED THE POOL, beside the gate (T11).
	converger_.setSlotLinkMaker(
	    [](const void* host, DeviceInstanceId self) {
		    return const_cast<RackBridge*>(static_cast<const RackBridge*>(host))->slotLinkFor(self);
	    },
	    this);
	const size_t lanes = engine_.slotCount();
	controlStride_ = device::kMaxDeviceFields; // the CONTROL INDEX bound (a row is at most this wide), not the routing budget
	readingStride_ = RackEngine::kMaxReadings;
	growMirror(lanes);
	// AND AS THE ENGINE GROWS (unbounded devices step 6) — told on the main thread, before a device can stand in a
	// lane the mirror does not reach.
	growthToken_ = engine_.addGrowthListener([this](size_t capacity) { growMirror(capacity); });
	m_.macro.assign(engine_.axes().coreSources, 0.0f);
	// **THE MODES THE ENGINE ALREADY HOLDS — POLARITY, TYPE AND NAME.** A body loaded before this bridge
	// existed (`rack_device.cpp`, `inner_chain.cpp`) carried them into the engine, and from here the bridge
	// is where they are read and saved. The polarity was once assigned unipolar here, so a nested rack
	// saved its bipolar macros as unipolar — and `setMacroBipolar(i, false)` then returned early, leaving
	// the engine bipolar with no road back. Message thread, before any block runs on this bridge.
	macroBipolar_.resize(engine_.axes().coreSources);
	macroType_.resize(engine_.axes().coreSources);
	macroName_.resize(engine_.axes().coreSources);
	for (size_t i = 0; i < macroType_.size(); ++i) {
		macroBipolar_[i] = engine_.macroBipolar(i) ? 1 : 0;
		macroType_[i] = engine_.macroType(i);
		macroName_[i] = engine_.macroRenamed(i) ? engine_.macroName(i) : std::string {};
	}

	// **WHAT THE ENGINE ALREADY HOLDS BECOMES THE RECORD** — its devices in chain order, each UNDER THE ID
	// THE ENGINE KNOWS IT BY (kill-lanes step 3a: one device, one id, on both sides) with its whole row, and
	// every route between them, by endpoint. Then the converger is told the engine already runs them, so
	// the first drain keeps every instance rather than rebuilding it.
	for (size_t position = 0; position < engine_.chainLength(); ++position) {
		const size_t lane = engine_.deviceAtPosition(position);
		if (lane == kNoSlot || !engine_.slotAtLane(lane).live()) {
			continue;
		}
		const device::Descriptor* desc = engine_.slotAtLane(lane).live_desc();
		DeviceState row;
		for (size_t c = 0; c < row.tag.size() && desc->traits.tag != nullptr && desc->traits.tag[c] != '\0'; ++c) {
			row.tag[c] = desc->traits.tag[c];
		}
		row.instanceId = engine_.slotAtLane(lane).id != kNoInstance ? engine_.slotAtLane(lane).id : ids_.issue();
		row.fieldsPresent = std::min(engine_.fieldsPresentAt(lane), row.field.size());
		for (size_t c = 0; c < row.fieldsPresent; ++c) {
			row.field[c] = engine_.fieldAt(lane, c);
		}
		// AT THE END OF THE CHAIN, IN THE ENGINE'S ORDER — the record picks its own slot for the row; where
		// the ENGINE keeps it is the engine's, and the two are joined by the id alone.
		(void)record_.insertRow(0, record_.length(0), row);
	}
	engine_.forEachRoute([this](const backplane::Route& r) {
		if (record_.setRoute(r.source, r.destination, r.amount)) {
			(void)record_.setRouteTransfer(r.source, r.destination, r.transfer);
		}
	});
	converger_.adoptRunning(record_.state());

	// **THE ENGINE RESOLVES THROUGH THE RECORD'S ROUTES** — the copy the drain adopts whole — with its
	// own sources (a null reader). Its own graph keeps structure only.
	engine_.setHostRoutes(std::span<const backplane::Route> {});
	commit();
	// NO BLOCK HAS RUN, so this thread may take the drain's part once: the engine settles on the record
	// now, and an editor opened before the first block draws the rack rather than nothing.
	drain();
	publish(engine_);
}

RackBridge::~RackBridge() {
	// **FIRST, NOTHING INSIDE MAY ASK THIS RACK ANYTHING ON ITS WAY OUT** (backlog PM, ASan). Every container this rack
	// provisioned holds a gate into `record_`, and lives in an engine that can outlive this bridge (a Chain device's
	// `State` frees the bridge first). Their teardown runs their own services — a nested rack's converger reaps and
	// prunes as it dies — and each prune walks `mayRoute` up through every host. Unhooked here, the walk stops at them.
	converger_.unhookOuterGates();
	// **AND NOTHING INSIDE MAY ASK THE SLOT POOL ANYTHING EITHER** (rack-teardown-uaf, ASan): every container this rack
	// provisioned holds a link to the pool's owner — this bridge's own `slots_` when it is the outermost rack, the
	// outermost one's otherwise — and outlives this bridge in the engine. A nested rack dying there services its sites,
	// and a site's bridge pulls its bindings through the link (`NestedSlots::service` -> `bindingGeneration`): a read of
	// a freed `SlotOwner`, silent on Linux and a fault on Windows. Unlinked here, each level hands its children none.
	converger_.unhookSlotLinks();
	// **THEN THE CONVERGER IS DRAINED, HERE, WHILE EVERY MEMBER IS STILL ALIVE — an explicit order, not a declaration
	// order.** Its reap destroys what the engine parked and services the sites it holds (`serviceSites`), and a
	// container dying there runs its own service: a nested rack's bridge reads its slot link, which calls back into
	// THIS bridge's `slots_` (`SlotOwner::nestedBindingGeneration`, through the link's `bindingGeneration`). Left to the
	// member destructors, `converger_` (declared before `slots_`) died AFTER it, and that call read a freed owner —
	// silent on Linux, a fault on Windows. Every member a reap can reach is alive at this line: `slots_`, `nested_`,
	// `macroOutbox_`, `record_`, `bodies_`, the engine. `~Converger` then has nothing left to do.
	converger_.shutdown();
	engine_.removeGrowthListener(growthToken_);
	engine_.clearHostRoutes();
	if (slots_ != nullptr) {
		engine_.setHostSlotBus(nullptr); // the bus dies with this bridge; the engine may not
	}
	// A NESTED BRIDGE CALLS NOTHING UP ON ITS WAY OUT (T11): the owner may already be gone — Feathers destroys
	// its bridge before the synth whose lane holds this rack — and what this level reported is forgotten by the
	// owner's own reconcile, once the container that held it no longer stands.
}

void RackBridge::setHostSlots(const device::HostSlotLink& link) {
	if (nested_ == nullptr || !nested_->setLink(link)) {
		return; // the outermost rack owns its pool; or the same link, re-handed
	}
	// THE ENGINE READS THE ONE BUS. Handed at provision, before a block runs here; a re-handed link reaches the
	// same owner's bus (one per plugin), so a running engine is never re-pointed.
	if (link.live() && engine_.hostSlotBus() == nullptr) {
		engine_.setHostSlotBus(link.bus);
	}
	pushMacroCaptions(); // the renamed macros, told to the new owner (`setOwnCaptions` sends on a new link)
	nested_->report();   // and what this rack holds, before anything asks through it
	// EVERY CONTAINER ALREADY HERE IS LINKED ANEW — a body loaded before this link arrived built them unlinked.
	for (size_t chain = 0; chain < record_.chainCount(); ++chain) {
		for (size_t position = 0; position < record_.length(chain); ++position) {
			const DeviceInstanceId id = record_.deviceAt(chain, position);
			const device::Descriptor* d = record_.descriptorOf(id);
			void* surface = bodies_.surfaceOf(id);
			if (d != nullptr && d->container != nullptr && d->container->setHostSlots != nullptr && surface != nullptr) {
				d->container->setHostSlots(surface, slotLinkFor(id));
			}
		}
	}
	// AND EVERY SITE IT HOLDS (rack-teardown-uaf): not rows of the record, so the walk above misses them — an unlinked
	// level would otherwise leave its sites linked to the pool it just lost.
	converger_.relinkSites();
}

device::HostSlotLink RackBridge::slotLinkFor(DeviceInstanceId self) noexcept {
	if (slots_ != nullptr) {
		return slots_->linkFor(self);
	}
	// **EACH LEVEL PUTS ITSELF ON THE PATH** (plan §3): the child's link is this one with the child's id
	// appended, so a device two racks down is `[outer rack, inner rack, device]` and never collides with an
	// equal id in a sibling rack.
	return nested_ != nullptr ? nested_->linkForChild(self) : device::HostSlotLink {};
}

//-------------------------------------------------------------------------------------------------//
// THE RECORD                                                                                      //
//-------------------------------------------------------------------------------------------------//

void RackBridge::commit() {
	// PUBLISHED, THEN BUILT FOR THAT PUBLISH — each arrival names the publish it belongs to, so a drain
	// still on the previous state keeps it for later instead of handing it back as unwanted. The other
	// order lost an add in three under a hand adding while blocks ran; see `Converger::provision`.
	//
	// **AND A NESTED RACK TELLS THE POOL WHAT IT HOLDS, AT ONCE** (T11): a level's report is how the owner
	// resolves every path THROUGH it, so a report one edit stale reads "no such device" to a rack added inside
	// this one — and a reconcile frees what it cannot resolve. Unchanged is one compare, and nothing is sent.
	if (nested_ != nullptr) {
		nested_->report();
	}
	// THE DRAIN'S STATES FIRST, if the record has outgrown them (unbounded devices step 6): a drain that reads the
	// bigger publish before its own states have grown is refused by `adopt` and asks again — one block late.
	offerDrainGrowth();
	(void)published_.publish(record_.state());
	record_.publishRoutes(publishedRoutes_);
	converger_.provision(record_.state(), &bodies_, published_.sequence());
}

void RackBridge::harvestExtraState() {
	bool changed = false;
	for (size_t lane = 0, lanes = mirrorLanes_.load(std::memory_order_acquire); lane < lanes && lane < m_.extraState.size(); ++lane) {
		device::ExtraStateSnapshot::Read read;
		if (m_.extraState[lane].read(&read)) {
			changed = rack::harvestExtraState(&record_, read) || changed;
		}
	}
	// AND THE KEYED BLOCKS THAT ASK TO BE KEPT CURRENT (backlog NM `harvestOnTick`) — into the store, not the record:
	// nothing is republished for them.
	(void)rack::harvestBlocks(record_, &bodies_);
	// NOT AN EDIT: the record was behind what is playing, and is not any more. Nothing is marked dirty.
	if (changed) {
		commit();
	}
}

void RackBridge::reapRetired() {
	harvestExtraState();
	reapDrainGrowth();
	offerDrainGrowth(); // and a growth a busy drain could not take before
	// THE SERVICE: parked devices destroyed, and any lane the engine could not take built again — the
	// state has not changed, so this is `provision` finding the lane it was told to forget.
	converger_.provision(record_.state(), &bodies_, published_.sequence());
	// WHAT THE CHAINS INSIDE DROPPED AT THEIR SERVICE, kept to be said by this rack's surface (§7 hygiene).
	noteRefusedRoutes(converger_.takeOuterDrops());
	(void)sweepExports(); // AND THE EXPORT ROWS NO ROUTE READS ANY MORE (backlog NZ), once the audio thread has caught up
}

bool RackBridge::namesLiveExport(backplane::Source source) const {
	if (source.isOuter() || !source.isExport() || !record_.holds(source.device)) {
		return false;
	}
	const size_t row = source.exportRow();
	if (source.isSiteExport()) {
		const auto* site = static_cast<const SiteChains*>(bodies_.surfaceOf(siteKeyOf(source.device)));
		return site != nullptr && site->owner() == source.device && site->exportedSource(row) != backplane::Source::nobody();
	}
	const device::Descriptor* d = descriptorOf(source.device);
	void* surface = bodies_.surfaceOf(source.device);
	return d != nullptr && surface != nullptr && device::exportsContainer(*d) && d->container->exportedSource != nullptr
	       && d->container->exportedSource(surface, row) != backplane::Source::nobody();
}

size_t RackBridge::sweepExports() {
	size_t freed = 0;
	// THE OWNER'S TABLE IS A READER TOO (NZ, any depth): a row of the container this rack is the inside of that names a
	// holder's row here re-exports it, and keeps it as surely as a route does.
	const auto ownerReads = [&](backplane::Source s) {
		if (ownerExports_ == nullptr) {
			return false;
		}
		for (size_t r = 0; r < device::kSiteExportRows; ++r) {
			if (ownerExports_->source(r) == s) {
				return true;
			}
		}
		return false;
	};
	const uint32_t published = published_.sequence();
	const uint32_t drained = drainedMirror_.load(std::memory_order_acquire);
	for (size_t position = 0; position < chainLength(0); ++position) {
		const DeviceInstanceId holder = deviceAtPosition(position);
		const device::Descriptor* d = holder != kNoInstance ? descriptorOf(holder) : nullptr;
		void* surface = holder != kNoInstance ? bodies_.surfaceOf(holder) : nullptr;
		if (d == nullptr || surface == nullptr || !device::exportsContainer(*d) || d->container->exportedSource == nullptr) {
			continue;
		}
		// WHO READS A ROW: a route in this record, a route inside a chain this rack holds (the borrowed export carried
		// inward), or a route in a device's site.
		std::vector<bool> used(device::kSiteExportRows, false);
		record_.graph().forEachRoute([&](const backplane::Route& r) {
			// THE CONTAINER'S ROAD ONLY, read from the bit: a route reading the holder's SITE row k is no reader of
			// this table's row k (2026-10-29), and the site's rows are its own to sweep.
			if (r.source.isContainerExport() && r.source.device == holder && r.source.exportRow() < used.size()) {
				used[r.source.exportRow()] = true;
			}
		});
		for (size_t row = 0; row < used.size(); ++row) {
			if (used[row] || d->container->exportedSource(surface, row) == backplane::Source::nobody()) {
				continue;
			}
			const backplane::Source asHostSees = backplane::Source::containerExported(holder, row);
			if (ownerReads(asHostSees)) {
				used[row] = true;
				continue;
			}
			std::vector<InnerRoute> inner;
			for (size_t p = 0; p < chainLength(0) && !used[row]; ++p) {
				const DeviceInstanceId other = deviceAtPosition(p);
				if (other == kNoInstance) {
					continue;
				}
				inner.clear();
				backplane::innerRoutesOf(descriptorOf(other), bodies_.surfaceOf(other), other, asHostSees, inner);
				// A SITE IS WALKED WITH THE SITE'S OWN TABLE, never its device's: a Rack or a Chain holding a site (D3
				// lifted, 2026-10-29) would hand its own container ops a `SiteChains` (and a leaf's had no table at all,
				// so a route inside a later device's site never counted as a reader).
				backplane::innerRoutesOf(&siteDescriptor(), bodies_.surfaceOf(siteKeyOf(other)), siteKeyOf(other), asHostSees,
				                         inner);
				used[row] = !inner.empty();
			}
		}
		freed += sweepContainerExports(d, surface, used, published, drained);
	}
	// AND EACH DEVICE'S SITE'S OWN TABLE (backlog NZ: the rows a PRE source was minted under), by the same rule. Its
	// readers: a route in this record reading the holder's SITE row (the device itself, or one after it, rung 4), and a
	// route inside a site or a container — the holder's own POST among them, which reads its PRE's export from outside.
	for (size_t position = 0; position < chainLength(0); ++position) {
		const DeviceInstanceId holder = deviceAtPosition(position);
		auto* site = holder != kNoInstance ? static_cast<SiteChains*>(bodies_.surfaceOf(siteKeyOf(holder))) : nullptr;
		if (site == nullptr || site->owner() != holder || site->exportRowsUsed() == 0) {
			continue;
		}
		std::vector<bool> used(device::kSiteExportRows, false);
		record_.graph().forEachRoute([&](const backplane::Route& r) {
			if (r.source.isSiteExport() && r.source.device == holder && r.source.exportRow() < used.size()) {
				used[r.source.exportRow()] = true;
			}
		});
		for (size_t row = 0; row < used.size(); ++row) {
			if (used[row] || site->exportedSource(row) == backplane::Source::nobody()) {
				continue;
			}
			const backplane::Source asHostSees = backplane::Source::exported(holder, row);
			if (ownerReads(asHostSees)) {
				used[row] = true;
				continue;
			}
			std::vector<InnerRoute> inner;
			for (size_t p = 0; p < chainLength(0) && !used[row]; ++p) {
				const DeviceInstanceId other = deviceAtPosition(p);
				if (other == kNoInstance) {
					continue;
				}
				inner.clear();
				backplane::innerRoutesOf(descriptorOf(other), bodies_.surfaceOf(other), other, asHostSees, inner);
				backplane::innerRoutesOf(&siteDescriptor(), bodies_.surfaceOf(siteKeyOf(other)), siteKeyOf(other), asHostSees,
				                         inner);
				used[row] = !inner.empty();
			}
		}
		freed += site->sweepExports(used, published, drained);
	}
	return freed;
}

// **EVERY DEVICE BY ITS ID (kill-lanes steps 2 and 3b)** — the record answers by id, and the mirrors below
// are indexed by the ENGINE's slot, found by that same id. No number crosses between them.

float RackBridge::fieldAt(DeviceInstanceId device, size_t control) const {
	const DeviceState& row = record_.rowOf(device);
	if (!row.live()) {
		return 0.0f;
	}
	if (control < row.fieldsPresent && control < row.field.size()) {
		return row.field[control];
	}
	const device::Descriptor* d = record_.descriptorOf(device);
	return d != nullptr && control < d->params.size() ? static_cast<float>(d->params[control].def) : 0.0f;
}

void RackBridge::setField(DeviceInstanceId device, size_t control, float value) {
	// COERCED AS THE ENGINE WILL COERCE IT (`device::coerceControl`), so the record holds — and the
	// editor draws — the number the engine uses.
	const device::Descriptor* d = record_.descriptorOf(device);
	if (d != nullptr && control < d->params.size()) {
		value = device::coerceControl(d->params[control], value);
	}
	// WHETHER THE RECORD ALREADY HELD IT, read before the write: a write of the value already there is not an
	// edit and tells the host nothing (`SlotOwner::onHandWrite`, the T6b review's rule, Feathers' too). The
	// dirty mark below is mainline's (JV) and asks the same question of the record's value.
	heldFalls_.drop(device, control); // a plain write supersedes a held release
	const float before = fieldAt(device, control);
	const bool recordHadIt = slots_ != nullptr ? slots_->recordHolds(device, control, value)
	                                           : (nested_ != nullptr && nested_->recordHolds(device, control, value));
	if (record_.setField(device, control, value)) {
		// A BOUND CONTROL: THE BUS FIRST, THEN THE RECORD PUBLISHES, and the host hears the value on its slot
		// (`SlotOwner::onHandWrite`) — or the next block puts the host's last value back over the hand's. A
		// nested rack's slot is the outermost plugin's; the same rule, over the same bus (`NestedSlots`, T11).
		if (slots_ != nullptr) {
			slots_->onHandWrite(device, control, value, recordHadIt);
		}
		else if (nested_ != nullptr) {
			nested_->onHandWrite(device, control, value, recordHadIt);
		}
		commit();
		if (value != before) {
			markStateDirty(); // a device's controls are not the rack's parameters: only this tells the host
		}
		heldFalls_.wrote(device, control, before, value, published_.sequence());
	}
}

float RackBridge::modulatedFieldAt(DeviceInstanceId device, size_t control) const {
	const size_t slot = mirrorSlotOf(device);
	if (slot == kNoSlot || control >= controlStride_ || control >= loadSize(m_.fieldsPresent[slot])) {
		return fieldAt(device, control);
	}
	return loadFloat(m_.modulated[slot * controlStride_ + control]);
}

bool RackBridge::modulatedFieldRAt(DeviceInstanceId device, size_t control, float& right) const {
	const size_t slot = mirrorSlotOf(device);
	if (slot == kNoSlot || control >= controlStride_ || control >= device::kControlMaskBits
	    || control >= loadSize(m_.fieldsPresent[slot])) {
		return false;
	}
	const uint64_t word = relaxedLoad(m_.stereoLive[slot * device::kControlMaskWords + (control >> 6)]);
	if (((word >> (control & 63)) & 1u) == 0) {
		return false;
	}
	right = loadFloat(m_.modulatedR[slot * controlStride_ + control]);
	return true;
}

bool RackBridge::playheadAt(DeviceInstanceId device, uint32_t& ordinal, float& phase) const {
	const size_t slot = mirrorSlotOf(device);
	if (slot == kNoSlot) {
		return false;
	}
	// ONE WORD, so the ordinal and the phase cannot come from two different blocks.
	const uint64_t packed = relaxedLoad(m_.playhead[slot]);
	const auto at = static_cast<uint32_t>(packed >> 32);
	if (at == mod::Playhead::kIdle) {
		return false;
	}
	ordinal = at;
	phase = std::bit_cast<float>(static_cast<uint32_t>(packed & 0xFFFFFFFFu));
	return true;
}

bool RackBridge::trigFiresAt(DeviceInstanceId device, uint32_t& fires) const {
	const size_t slot = mirrorSlotOf(device);
	if (slot == kNoSlot) {
		return false;
	}
	const uint64_t packed = relaxedLoad(m_.trigFires[slot]);
	if ((packed >> 32) == 0) {
		return false;
	}
	fires = static_cast<uint32_t>(packed & 0xFFFFFFFFu);
	return true;
}

size_t RackBridge::recentEventsAt(DeviceInstanceId device, device::RecentEvent* out, size_t capacity) const {
	const size_t slot = mirrorSlotOf(device);
	if (slot == kNoSlot || out == nullptr) {
		return 0;
	}
	size_t n = 0;
	for (; n < capacity && n < device::kRecentEvents; ++n) {
		const device::RecentEvent e = device::unpackRecentEvent(relaxedLoad(m_.recent[slot * device::kRecentEvents + n]));
		if (e.ordinal == 0) {
			break;
		}
		out[n] = e;
	}
	return n;
}

bool RackBridge::playheadLevelAt(DeviceInstanceId device, float& level) const {
	const size_t slot = mirrorSlotOf(device);
	if (slot == kNoSlot) {
		return false;
	}
	const float stored = loadFloat(m_.playheadLevel[slot]);
	if (!(stored >= 0.0f)) {
		return false;
	}
	level = stored;
	return true;
}

void RackBridge::setRouteAmount(backplane::Source source, backplane::Destination destination, float amount) {
	const float before = record_.routeAmount(source, destination);
	if (record_.setRoute(source, destination, amount)) {
		commit();
		if (record_.routeAmount(source, destination) != before) {
			markStateDirty();
		}
	}
}

bool RackBridge::setRouteTransfer(backplane::Source source, backplane::Destination destination, backplane::Transfer t) {
	// ON AN EXISTING ROUTE ONLY (the record refuses none), and a curve already held is no edit.
	if (record_.routeAmount(source, destination) == 0.0f) {
		return false;
	}
	if (record_.routeTransfer(source, destination) == t) {
		return true;
	}
	if (!record_.setRouteTransfer(source, destination, t)) {
		return false;
	}
	commit();
	markStateDirty();
	return true;
}

DeviceInstanceId RackBridge::addDevice(const char* tag) {
	const DeviceInstanceId id = record_.insert(0, record_.length(0), tag);
	if (id != kNoInstance) {
		commit();
		markStateDirty();
	}
	return id;
}

DeviceInstanceId RackBridge::addDeviceBefore(DeviceInstanceId anchor, const char* tag) {
	// THE ANCHOR MUST BE A DEVICE THE HAND CAN SEE — inserting before nothing is not appending.
	const size_t at = positionOf(anchor);
	if (at == kNoSlot) {
		return kNoInstance;
	}
	const DeviceInstanceId id = record_.insert(0, at, tag);
	if (id != kNoInstance) {
		commit();
		markStateDirty();
	}
	return id;
}

backplane::Source RackBridge::containerExport(DeviceInstanceId container, backplane::Source inner) {
	return exportFromContainer(descriptorOf(container), bodies_.surfaceOf(container), container, inner);
}

backplane::Source RackBridge::siteExport(DeviceInstanceId owner, backplane::Source inner) {
	return exportFromSite(bodies_.surfaceOf(siteKeyOf(owner)), owner, inner);
}

bool RackBridge::openSite(DeviceInstanceId device) {
	const device::Descriptor* d = descriptorOf(device);
	if (d == nullptr || siteLanesOf(*d) == device::Traits::SiteLanes::None) {
		return false;
	}
	const DeviceInstanceId key = siteKeyOf(device);
	if (bodies_.has(key)) {
		return true;
	}
	// THE DEPTH GATE (`fitsNesting`): a site is a chain below this one, which a file must give back full.
	if (!fitsNesting(record_.depth(), 1)) {
		return false;
	}
	// FILED EMPTY — "this device has a site, holding nothing" — and committed, so the converger builds it on this
	// thread and offers it to the engine. Nothing else in the record moves.
	(void)bodies_.store(key, nullptr, 0);
	commit();
	markStateDirty();
	return true;
}

bool RackBridge::holdsBody(std::span<const uint8_t> body) const {
	return containerBodyFits(record_.depth(), body, record_.catalogue());
}

bool RackBridge::loadSite(DeviceInstanceId device, std::span<const uint8_t> body) {
	// THE DEPTH GATE, ON THE WHOLE SITE AS IT WILL STAND: refused before anything is opened, so nothing changes.
	if (!holdsBody(body)) {
		return false;
	}
	if (!openSite(device)) {
		return false;
	}
	const DeviceInstanceId key = siteKeyOf(device);
	if (void* surface = bodies_.surfaceOf(key); surface != nullptr) {
		const device::ContainerOps& ops = siteContainerOps();
		if (ops.applyState == nullptr || !ops.applyState(surface, body.data(), body.size())) {
			return false;
		}
	}
	else if (!bodies_.store(key, body.data(), body.size())) {
		return false;
	}
	commit();
	markStateDirty();
	return true;
}

void RackBridge::removeDevice(DeviceInstanceId device) {
	if (record_.remove(device)) {
		(void)bodies_.erase(device); // what it held goes with it
		(void)bodies_.erase(siteKeyOf(device)); // and its site (backlog NR)
		// AND WHAT ITS OUTPUTS WERE CALLED. Safe here, unlike Feathers' bridge, because nothing on this
		// bridge re-inserts a device under an id it had — `addDevice` always issues a fresh one.
		outputLabels_.erase(device);
		// AND ITS HOST SLOTS, anchored ones too (design §6). Each keeps
		// its note, which nothing here reads back: this bridge never re-inserts under an old id (above).
		if (slots_ != nullptr) {
			slots_->pool().freeUnder(std::span<const DeviceInstanceId>(&device, 1));
		}
		// A NESTED RACK FREES THEM IN THE OUTERMOST POOL, under its path (T11) — and everything under the device,
		// when it was itself a rack.
		if (nested_ != nullptr) {
			nested_->deviceRemoved(device);
		}
		commit();
		markStateDirty();
	}
}

void RackBridge::fragmentFreeSlots(DeviceInstanceId device) {
	if (slots_ != nullptr) {
		(void)slots_->pool().freeUnder(std::span<const DeviceInstanceId>(&device, 1));
	}
	if (nested_ != nullptr) {
		nested_->deviceRemoved(device);
	}
}

void RackBridge::fragmentRestoreSlots(DeviceInstanceId device) {
	// THE UNDO OF THE FREE ABOVE, under the same path: this is the road that makes "nothing on this bridge
	// re-inserts a device under an id it had" (`removeDevice`) no longer true, and so the one that must give back
	// what the removal took. The labels come back with the fragment's patch; the slots come back here.
	if (slots_ != nullptr) {
		(void)slots_->restoreUnderPath(std::span<const DeviceInstanceId>(&device, 1));
	}
	if (nested_ != nullptr) {
		nested_->deviceRestored(device);
	}
}

bool RackBridge::fragmentRebaseSlots(DeviceInstanceId device, DeviceInstanceId container, bool into) {
	if (slots_ != nullptr) {
		const std::array<DeviceInstanceId, 1> flat { device };
		const std::array<DeviceInstanceId, 2> deep { container, device };
		return into ? slots_->rebasePath(flat, deep) : slots_->rebasePath(deep, flat);
	}
	if (nested_ != nullptr) {
		return nested_->deviceRebased(device, container, into);
	}
	return true;
}

void RackBridge::fragmentChanged(const DeviceLabels& labelsBefore) {
	commit();
	markStateDirty();
	DeviceLabels::forEachDifference(labelsBefore, outputLabels_, [this](DeviceInstanceId id, size_t output) {
		outputLabelSubscribers_.notify(id, output);
	});
}

std::string RackBridge::outputLabel(DeviceInstanceId device, size_t output) const {
	if (const std::string_view label = outputLabels_.labelOf(device, output); !label.empty()) {
		return std::string(label);
	}
	const std::string_view declared = declaredOutputName(descriptorOf(device), output);
	return declared.empty() ? std::string("Source") : std::string(declared);
}

void RackBridge::setOutputLabel(DeviceInstanceId device, size_t output, std::string_view label) {
	const device::Descriptor* desc = descriptorOf(device);
	if (desc == nullptr || output >= desc->outputCount()) {
		return; // an output nobody declares has nothing to be called
	}
	std::string next = device::clampMacroName(label);
	if (next == declaredOutputName(desc, output)) {
		next.clear(); // the declared name spelled out is no label
	}
	if (next == outputLabels_.labelOf(device, output)) {
		return; // the label it has already; not an edit
	}
	outputLabels_.set(device, output, next);
	markStateDirty();
	outputLabelSubscribers_.notify(device, output);
}

bool RackBridge::moveDevice(size_t from, size_t to) {
	if (from == to || !record_.move(0, from, to)) {
		return false;
	}
	commit();
	markStateDirty();
	return true;
}

bool RackBridge::moveDeviceTo(DeviceInstanceId device, size_t to) {
	const size_t from = positionOf(device);
	return from != kNoSlot && moveDevice(from, to);
}

void RackBridge::setMacro(size_t index, float value) {
	if (!queue_.push(RackEdit { RackEdit::Kind::Macro, index, value })) {
		return; // no mirror write for an event the engine will not get
	}
	if (index < m_.macro.size()) {
		// THE HAND SEES ITS OWN DRAG AT ONCE; the engine's own value replaces this at the next publish.
		storeFloat(m_.macro[index], value);
	}
}

void RackBridge::beginMacroGesture(size_t index) noexcept {
	if (macroOutbox_ != nullptr) {
		(void)macroOutbox_->begin(index);
	}
}

void RackBridge::setMacroByHand(size_t index, float value) {
	setMacro(index, value);
	// WHAT THE MIRROR NOW HOLDS, not what was asked: a write the queue refused stored nothing, and the host is
	// told the value the rack has (Feathers reports `params_.base`, the stored value, for the same reason).
	if (macroOutbox_ != nullptr && index < m_.macro.size()) {
		(void)macroOutbox_->value(index, macro(index));
	}
}

void RackBridge::endMacroGesture(size_t index) noexcept {
	if (macroOutbox_ != nullptr) {
		(void)macroOutbox_->end(index);
	}
}

void RackBridge::setMacroBipolar(size_t index, bool bipolar) {
	if (index >= macroBipolar_.size() || macroBipolar(index) == bipolar) {
		return;
	}
	if (!queue_.push(RackEdit { RackEdit::Kind::MacroPolarity, index, bipolar ? 1.0f : 0.0f })) {
		return; // no mirror write for an event the engine will not get
	}
	macroBipolar_[index] = bipolar ? 1 : 0;
	markStateDirty();
}

void RackBridge::setMacroType(size_t index, device::MacroType type) {
	// THE RACK HAS NO RELATIVE ROAD YET (backlog PQ): Relative is refused like a type past the last.
	if (index >= macroType_.size() || !device::isAbsoluteMacroType(type) || macroType_[index] == type) {
		return;
	}
	macroType_[index] = type; // held, not queued: see the header
	markStateDirty();
}

std::string RackBridge::macroName(size_t index) const {
	return macroRenamed(index) ? macroName_[index] : device::defaultMacroName(index);
}

void RackBridge::setMacroName(size_t index, std::string_view name) {
	if (index >= macroName_.size()) {
		return;
	}
	std::string next = device::normalisedMacroName(index, name);
	if (next == macroName_[index]) {
		return; // the name it has already — including the default spelled out; not an edit
	}
	macroName_[index] = std::move(next);
	markStateDirty();
	pushMacroCaptions(); // a slot on this macro's device control is relabelled (T11)
	macroRenameSubscribers_.notify(index);
}

void RackBridge::setMacrosAreDeviceControls(bool yes) {
	macrosAreDeviceControls_ = yes;
	pushMacroCaptions();
}

void RackBridge::pushMacroCaptions() {
	if (nested_ == nullptr || !macrosAreDeviceControls_) {
		return;
	}
	std::vector<device::HostSlotCaption> captions;
	const size_t first = static_cast<size_t>(composite::Param::Macro1);
	for (size_t i = 0; i < macroName_.size() && i < composite::kMacroCount; ++i) {
		if (!macroName_[i].empty()) { // held EMPTY when it is the default
			captions.push_back(device::HostSlotCaption { .control = first + i, .name = macroName_[i] });
		}
	}
	nested_->setOwnCaptions(std::move(captions));
}

//-------------------------------------------------------------------------------------------------//
// THE AUDIO SIDE                                                                                  //
//-------------------------------------------------------------------------------------------------//

void RackBridge::drain() {
	adoptDrainGrowth(); // room first (unbounded devices step 6)
	// **EACH PUBLISH, WHOLE.** A snapshot that saw a publish land is refused and leaves `drained_` as it was;
	// the engine keeps converging to the state it had, and takes this one at the next drain. (This
	// snapshotted INTO `drained_` and converged it either way — a torn state, and the churn's lost add.)
	if (published_.sequence() != drainedSequence_) {
		(void)published_.current().adopt(&drained_, &drainStaging_, &drainedSequence_);
		drainedMirror_.store(drainedSequence_, std::memory_order_release);
	}
	// EVERY DRAIN, not only a new one: an arrival the engine refused lands after its state did. **UNLESS THE LAST
	// PASS WAS OF THIS SAME PUBLISH AND LEFT NOTHING BEHIND IT** (backlog NR's first cheap win): then the engine
	// already is `drained_`, and the pass would rewrite every row with the value it holds. An arrival, a held
	// post, a refused removal or a pending growth each keeps the pass running (`Converger::settledAt`).
	if (!converger_.settledAt(drainedSequence_)) {
		converger_.converge(drained_, drainedSequence_);
		++converges_;
	}
	// **THE ROUTES, COMPILED INTO THE ENGINE'S COORDINATES** against where the engine now keeps each device
	// (kill-lanes §2.4, step 3a: the engine chooses its slots) — when either moved: new routes, or a device
	// arriving in or leaving a slot, however many drains after its state. A torn snapshot of the routes
	// keeps the list this drain already had.
	//
	// **THE LAYOUT HALF IS DEFENSIVE HERE AND A CASE SAYS SO RATHER THAN A READING.** MEASURED 2026-09-11:
	// freezing `layoutRevision_` leaves all 65 rack cases green (the mutation was confirmed in the object,
	// not merely on disk), because in THIS host every change that moves a device between slots also
	// republishes the route list, so the sequence branch always fires first. It is kept because the two
	// facts are independent — a host may adopt a device with no route having moved, which is exactly what
	// Feathers does — and because the cost is one comparison. Do not read any rack case as covering it.
	// **THE ROUTES, BY IDENTITY, HANDED TO THE ENGINE WHOLE (kill-lanes §2.4b).** A whole snapshot of what
	// the message thread last published, and the engine builds every node's inputs from it at the top of
	// its next block. Nothing is compiled here and no layout is tracked: a device the engine adopts a drain
	// after a route to it arrived is picked up by the engine's own rebuild, because adopting a device marks
	// its inputs dirty. A torn snapshot keeps the list the engine already had.
	if (publishedRoutes_.sequence() != routesSequence_) {
		size_t n = 0;
		uint32_t seen = 0;
		if (publishedRoutes_.snapshot(routeStaging_, &n, &seen)) {
			routesSequence_ = seen;
			engine_.setHostRoutes(backplane::RouteRun { std::as_const(routeStaging_).view(), n });
		}
	}
	RackEdit e {};
	while (queue_.pop(e)) {
		switch (e.kind) {
		case RackEdit::Kind::Macro: engine_.setMacro(e.a, e.value); break;
		case RackEdit::Kind::MacroPolarity: engine_.setMacroBipolar(e.a, e.value != 0.0f); break;
		case RackEdit::Kind::Gate: engine_.setGate(e.value != 0.0f); break;
		case RackEdit::Kind::Retrigger: engine_.retrigger(); break;
		}
	}
}

void RackBridge::prepare(double sampleRate, int maxFrames) {
	converger_.dropPending();
	engine_.prepare(sampleRate, maxFrames);
	converger_.provision(record_.state(), &bodies_, published_.sequence());
}

void RackBridge::publish(const RackEngine& engine) noexcept {
	// THE TEMPO, Feathers' rule (`ParamBridge::publishModulated`): zero unless the clock HAS one, and never a
	// non-finite or negative number a card would divide by.
	const device::TransportClock& clock = engine.clock();
	secondsPerBeat_.store(clock.haveTempo() && std::isfinite(clock.secondsPerBeat) && clock.secondsPerBeat > 0.0
	                          ? clock.secondsPerBeat
	                          : 0.0,
	                      std::memory_order_relaxed);
	// THE AS-MODULATED READOUT'S PAIR, IF THE EDITOR IS WATCHING ONE (backlog PU).
	engine.answerRouteProbe(routeProbe_);
	// PER ENGINE SLOT, EACH SLOT'S WORDS STAMPED WITH THE DEVICE THEY ARE ABOUT — see `Mirror`. Skipped whole when
	// the last publish already wrote every lane empty and nothing has arrived since (`publishedEmpty_`).
	const size_t lanes = mirrorLanes_.load(std::memory_order_acquire);
	const bool stillEmpty = publishedEmpty_ && &engine == &engine_ && engine.layoutRevision() == publishedEmptyLayout_
	                        && lanes == publishedEmptyLanes_;
	bool anyLive = false;
	for (size_t lane = 0; lane < lanes && !stillEmpty; ++lane) {
		const device::Descriptor* d = engine.slotAtLane(lane).desc.load(std::memory_order_acquire);
		anyLive = anyLive || d != nullptr;
		std::atomic_ref<const device::Descriptor*>(m_.desc[lane]).store(d, std::memory_order_relaxed);
		std::atomic_ref<DeviceInstanceId>(m_.id[lane])
		    .store(d != nullptr ? engine.slotAtLane(lane).id : kNoInstance, std::memory_order_relaxed);
		if (d == nullptr) {
			// AN EMPTY LANE PUBLISHES A ZERO WIDTH AND NOTHING ELSE — the width gates every reader.
			storeSize(m_.fieldsPresent[lane], 0);
			storeSize(m_.readingCount[lane], 0);
			// IDLE, NOT ZERO — ordinal 0 is a valid stage, so `kIdle` is the only honest "nothing runs".
			std::atomic_ref<uint64_t>(m_.playhead[lane])
			    .store(packPlayhead(mod::Playhead::kIdle, 0.0f), std::memory_order_relaxed);
			storeFloat(m_.playheadLevel[lane], -1.0f);
			std::atomic_ref<uint64_t>(m_.trigFires[lane]).store(0, std::memory_order_relaxed);
			for (size_t i = 0; i < device::kRecentEvents; ++i) {
				std::atomic_ref<uint64_t>(m_.recent[lane * device::kRecentEvents + i]).store(0, std::memory_order_relaxed);
			}
			continue;
		}
		// **BOUNDED BY THE ROW'S WIDTH** — a device using two controls costs two stores, not thirty-two.
		const size_t present = std::min(engine.fieldsPresentAt(lane), controlStride_);
		device::ControlMask live {};
		for (size_t c = 0; c < present; ++c) {
			const float modulated = engine.modulatedFieldAt(lane, c);
			storeFloat(m_.modulated[lane * controlStride_ + c], modulated);
			// THE RIGHT EAR (backlog KU): the engine's while a stereo route reaches the control, the left otherwise.
			float right = 0.0f;
			if (engine.modulatedFieldRAt(lane, c, right)) {
				live.set(c);
			} else {
				right = modulated;
			}
			storeFloat(m_.modulatedR[lane * controlStride_ + c], right);
		}
		// THE MASK BEFORE THE WIDTH, as the values are: the width gates every reader.
		for (size_t w = 0; w < device::kControlMaskWords; ++w) {
			std::atomic_ref<uint64_t>(m_.stereoLive[lane * device::kControlMaskWords + w]).store(live.words[w], std::memory_order_relaxed);
		}
		storeSize(m_.fieldsPresent[lane], present);

		const size_t readings = std::min(engine.readingCountAt(lane), readingStride_);
		for (size_t i = 0; i < readings; ++i) {
			storeFloat(m_.readings[lane * readingStride_ + i], engine.readingAt(lane, i));
		}
		storeSize(m_.readingCount[lane], readings);

		uint32_t ordinal = 0;
		float phase = 0.0f;
		const uint64_t packed = engine.playheadAt(lane, ordinal, phase) ? packPlayhead(ordinal, phase)
		                                                                : packPlayhead(mod::Playhead::kIdle, 0.0f);
		std::atomic_ref<uint64_t>(m_.playhead[lane]).store(packed, std::memory_order_relaxed);
		// AND THE LEVEL BESIDE IT (`mod::Playhead::level`) — negative is "not published", a level is never below zero.
		float level = 0.0f;
		storeFloat(m_.playheadLevel[lane],
		           engine.playheadLevelAt(lane, level) ? (std::isfinite(level) ? std::clamp(level, 0.0f, 1.0f) : 0.0f)
		                                               : -1.0f);
		// AND THE TRIG'S FIRE COUNT (backlog MV): bit 32 set when published, the count in the low word.
		uint32_t fires = 0;
		std::atomic_ref<uint64_t>(m_.trigFires[lane])
		    .store(engine.trigFiresAt(lane, fires) ? (uint64_t { 1 } << 32) | fires : 0, std::memory_order_relaxed);
		// AND THE NEWEST EVENTS BESIDE IT, a word each, the rest empty.
		{
			std::array<device::RecentEvent, device::kRecentEvents> recent {};
			(void)engine.recentEventsAt(lane, recent.data(), recent.size());
			for (size_t i = 0; i < recent.size(); ++i) {
				std::atomic_ref<uint64_t>(m_.recent[lane * device::kRecentEvents + i])
				    .store(device::packRecentEvent(recent[i]), std::memory_order_relaxed);
			}
		}
		// AND THE EXTRA STATE THE DEVICE CHANGED ITSELF (backlog MX), into its snapshot when the serial moved.
		size_t extraCount = 0;
		if (const uint64_t serial = engine.extraStateAt(lane, extraScratch_.data(), extraScratch_.size(), &extraCount);
		    serial != 0) {
			(void)m_.extraState[lane].publish(engine.slotAtLane(lane).id, serial, extraScratch_.data(), extraCount);
		}
	}
	if (!stillEmpty) {
		++mirrorWalks_;
		publishedEmpty_ = !anyLive && &engine == &engine_;
		publishedEmptyLayout_ = engine.layoutRevision();
		publishedEmptyLanes_ = lanes;
	}
	for (size_t i = 0; i < m_.macro.size(); ++i) {
		storeFloat(m_.macro[i], engine.macro(i));
	}
	std::atomic_ref<double>(m_.sampleRate).store(engine.sampleRate(), std::memory_order_relaxed);
}

//-------------------------------------------------------------------------------------------------//
// STATE                                                                                           //
//-------------------------------------------------------------------------------------------------//

RackPatch RackBridge::patch() const {
	// THE RECORD AS A PATCH — `patchOfRecord`, the one Feathers' bridge takes too (chain-as-a-device item
	// 6). Every route from the list this thread last published, which is the record's whole; a body from
	// where the record keeps it — a container a hand added holds nothing until a patch fills it.
	//
	// **FIRST THE RECORD CATCHES UP WITH WHAT ITS DEVICES CHANGED THEMSELVES** (backlog MX): a save of the pattern a
	// hand last drew, while a Turing machine has rewritten it, would not be what was heard. Every road a patch is
	// taken by (the plugin's save, a nested rack's body) comes here, so the catch-up is here; it changes what the
	// record is behind on, never what the patch means, hence the cast rather than a non-const save.
	const_cast<RackBridge*>(this)->harvestExtraState();
	std::vector<float> macros;
	for (size_t i = 0; i < engine_.axes().coreSources; ++i) {
		macros.push_back(macro(i));
	}
	RackPatch out = patchOfRecord(record_, &bodies_, macros, publishedRoutes_, rackCoreCodec(engine_.axes()),
	                              &cardViews_, macroBipolar_, &outputLabels_);
	for (size_t i = 0; i < macroType_.size(); ++i) {
		out.macroType.push_back(static_cast<uint8_t>(macroType_[i]));
	}
	out.macroName = macroName_; // held normalised: the default is saved as no name
	out.canvas = canvasView_;
	out.macrosCollapsed = macrosCollapsed_; // backlog QN
	out.preset = topPreset_; // backlog QC
	// THE HOST SLOTS, ONLY FROM THE BRIDGE THAT OWNS THEM (plan §2): the outermost patch stores them by path,
	// so a nested rack's body never carries any (design §7) — a rack preset moved to another project brings
	// no stale slots. And only when some slot is mapped (`snapshotForSave`).
	if (slots_ != nullptr) {
		out.hostSlots = slots_->snapshotForSave();
	}
	return out;
}

LoadReport RackBridge::loadPatch(const RackPatch& patch) {
	if (!patch.present) {
		return {}; // a fresh instance, not an empty rack: nothing is cleared
	}
	// **THE RECORD BECOMES THE PATCH, and what did not load as saved is known now** — there is no
	// landing to wait for. `applyPatchToRecord` is the one Feathers' bridge takes too.
	// **AND IT IS NOT AN EDIT** (backlog JV): the verbs below would each mark the patch dirty, and opening
	// a project must not report it modified. A mark an edit left before the load stays for the host.
	heldFalls_.forget(); // a held fall belongs to the patch being replaced
	loading_ = true;
	const DeviceLabels labelsBefore = outputLabels_;
	LoadReport refused = applyPatchToRecord(patch, &record_, &bodies_, &ids_, rackCoreCodec(engine_.axes()),
	                                              &cardViews_, &outputLabels_);
	// A LOAD GIVES EVERY SELF-CHANGING DEVICE A FRESH BASIS (backlog MX): what was loaded plays, not what it had become.
	stampExtraStateBases(&record_);
	// THE LISTENER HEARS EACH OUTPUT WHOSE LABEL MOVED, as the macros' rename listener does below.
	DeviceLabels::forEachDifference(labelsBefore, outputLabels_, [this](DeviceInstanceId id, size_t output) {
		outputLabelSubscribers_.notify(id, output);
	});
	canvasView_ = patch.canvas;
	macrosCollapsed_ = patch.macrosCollapsed; // backlog QN
	topPreset_ = patch.preset; // backlog QC: which preset the rack plugin's browser came from
	for (size_t i = 0; i < patch.macro.size(); ++i) {
		setMacro(i, patch.macro[i]);
	}
	// THE MODES, EVERY ONE: a patch that lists fewer leaves the rest unipolar rather than as they were.
	for (size_t i = 0; i < macroBipolar_.size(); ++i) {
		setMacroBipolar(i, i < patch.macroBipolar.size() && patch.macroBipolar[i] != 0);
	}
	// AND THE TYPES AND NAMES, on the same rule. A load is not a rename, but the listener hears each macro
	// whose effective name moved, exactly as a rename would tell it — through `setMacroName`, whose
	// "unchanged moves nothing" is what keeps a reload of the same patch silent.
	for (size_t i = 0; i < macroType_.size(); ++i) {
		setMacroType(i, i < patch.macroType.size() ? static_cast<device::MacroType>(patch.macroType[i])
		                                           : device::MacroType::Knob);
		setMacroName(i, i < patch.macroName.size() ? std::string_view(patch.macroName[i]) : std::string_view {});
	}
	// **THE HOST SLOTS AFTER THE RECORD, never before** (plan §0.7: the load may re-id a device): the owner
	// puts the patch's pool in place — an absent member is an empty pool — and frees what no longer
	// resolves. A bridge with no pool IGNORES `hostSlots` (a nested rack's body should carry none; one that
	// does is not this level's to apply). The bindings follow at the next main-thread visit
	// (`serviceHostSlots`), which the plugin runs before its load returns.
	if (slots_ != nullptr) {
		// AN ENTRY NAMING A CONTROL NO SLOT MAY CARRY (`ParamDesc::hostSlot`) IS DROPPED, AND SAID.
		const SlotOwner::Refused dropped = slots_->restore(patch.hostSlots);
		for (uint32_t i = 0; i < dropped.count; ++i) {
			refused.note(LoadReport::Kind::SlotsDropped, "patch", dropped.first, "no host slot may carry it");
		}
	}
	commit();
	loading_ = false;
	// KEPT UNTIL A SURFACE TAKES THEM, as well as returned: a chain's load has no surface at the time.
	noteLoadReport(refused);
	return refused;
}

uint32_t RackBridge::serviceHostSlots() noexcept {
	// THE FALLS A QUICK PRESS LEFT WAITING, once the engine has adopted their rises (`HeldFalls`). Before the slots: a
	// bridge with no pool (a nested rack) lands them too.
	try {
		heldFalls_.land(drainedMirror_.load(std::memory_order_acquire),
		                [this](DeviceInstanceId d, size_t c, float v) { setField(d, c, v); });
	}
	catch (...) {
		// Out of memory on the main thread: the next visit lands them.
	}
	if (slots_ == nullptr) {
		return 0;
	}
	try {
		(void)slots_->reconcileIfMoved();
	}
	catch (...) {
		// Out of memory on the main thread: the slot stays until the next visit, which tries again.
	}
	// The bindings and the host's values, published; the engine adopts them at its next drain. The rebound
	// slots' declarations reach the audio thread only after this publish (the remap window); a commit that
	// throws leaves them withheld and the publish owed to the next visit (`SlotOwner::service`).
	//
	// DIRTY ONLY FOR A NEW ANCHOR (`visit.poolChanged`): that the slot now stays on the host is saved with the
	// patch. THE MIRROR MARKS NOTHING — the host's automation moved the value and the host owns that, the rule
	// a host-set macro follows — which is why it writes the record directly rather than through `setField`.
	const SlotOwner::Visit visit = slots_->service([this] { commit(); });
	if (visit.poolChanged) {
		markStateDirty();
	}
	// AND EVERY RACK THIS ONE HOLDS (T11): it reports, pulls what the pool binds inside it, mirrors the host's
	// writes into its record and adopts — on this visit, editor or none (the rack's GUI tick is the only other
	// road that reaches a chain's service).
	try {
		converger_.serviceContainers(record_.state(), &bodies_);
		noteRefusedRoutes(converger_.takeOuterDrops());
		// AND THE CONTAINERS' OWN CONTROLS (backlog NO): released ones pruned before anything may take them again, a mirror a load owes written.
		(void)serviceOwnControls(record_, bodies_, [this](backplane::Source s, backplane::Destination d) {
			setRouteAmount(s, d, 0.0f);
		});
	}
	catch (...) {
		// Out of memory on the main thread: the racks inside catch up at the next visit.
	}
	return visit.anchored;
}

//----------------------------------------------------------------------------------//
// THE ARMED SOURCE'S ROUTE LIST (rack roadmap §7f) — the rack's own ends, named by //
// descriptor and position, and the routes inside the chains this rack holds.       //
//----------------------------------------------------------------------------------//

std::string RackBridge::destinationName(backplane::Destination destination) const {
	// THE RACK HAS NO DESTINATIONS OF ITS OWN — its macros are sources — so a host end names nothing.
	if (!destination.isDevice()) {
		return "Route";
	}
	const device::Descriptor* d = descriptorOf(destination.device);
	if (d == nullptr || destination.index >= d->params.size()) {
		return "Route"; // an id nothing holds names nothing
	}
	std::string name = d->params[destination.index].name;
	if (const size_t position = positionOf(destination.device); position != kNoSlot) {
		name += " ";
		name += std::to_string(position + 1);
	}
	return name;
}

std::vector<RackBridge::InnerRoute> RackBridge::innerRoutesFrom(backplane::Source source) const {
	std::vector<InnerRoute> out;
	if (source.isOuter()) {
		return out; // this host's sources only: a source already scoped is not one of ours to walk
	}
	// AND THE ROUTES INSIDE EVERY DEVICE'S SITE (backlog NR), filed under its site key with the site's own table — the
	// key is what a row carries back to the verbs below.
	for (size_t position = 0; position < chainLength(0); ++position) {
		const DeviceInstanceId chain = deviceAtPosition(position);
		if (chain != kNoInstance) {
			backplane::innerRoutesOf(descriptorOf(chain), bodies_.surfaceOf(chain), chain, source, out);
			const DeviceInstanceId site = siteKeyOf(chain);
			backplane::innerRoutesOf(&siteDescriptor(), bodies_.surfaceOf(site), site, source, out);
		}
	}
	return out;
}

float RackBridge::innerRouteAmount(backplane::Source source, DeviceInstanceId chain,
                                   backplane::Destination inner) const {
	if (chain == kNoInstance) {
		return 0.0f;
	}
	return backplane::innerRouteAmountOf(containerTableFor(chain, descriptorOf(chain)), bodies_.surfaceOf(chain), source,
	                                     inner);
}

bool RackBridge::setInnerRouteAmount(backplane::Source source, DeviceInstanceId chain, backplane::Destination inner,
                                     float amount) {
	if (chain == kNoInstance) {
		return false;
	}
	return backplane::setInnerRouteOf(containerTableFor(chain, descriptorOf(chain)), bodies_.surfaceOf(chain), source,
	                                  inner, amount);
}

backplane::Transfer RackBridge::innerRouteTransfer(backplane::Source source, DeviceInstanceId chain,
                                                   backplane::Destination inner) const {
	if (chain == kNoInstance) {
		return backplane::Transfer::Lin;
	}
	return backplane::innerRouteTransferOf(containerTableFor(chain, descriptorOf(chain)), bodies_.surfaceOf(chain),
	                                       source, inner);
}

bool RackBridge::setInnerRouteTransfer(backplane::Source source, DeviceInstanceId chain, backplane::Destination inner,
                                       backplane::Transfer t) {
	if (chain == kNoInstance) {
		return false;
	}
	return backplane::setInnerRouteTransferOf(containerTableFor(chain, descriptorOf(chain)), bodies_.surfaceOf(chain),
	                                          source, inner, t);
}

std::string RackBridge::innerDestinationName(DeviceInstanceId chain, backplane::Destination inner) const {
	// A SITE'S ROW IS NAMED BY ITS HOLDER AND LANE ("Filter 2 PRE · Filter · Cutoff"); a container's by its place.
	if (isSiteKey(chain)) {
		const DeviceInstanceId holder = chain & ~kSiteKeyBit;
		const device::Descriptor* d = descriptorOf(holder);
		const size_t at = positionOf(holder);
		std::string name = d != nullptr ? std::string(d->traits.name) : std::string("Device");
		if (at != kNoSlot) {
			name += " " + std::to_string(at + 1);
		}
		void* surface = bodies_.surfaceOf(chain);
		return backplane::innerNameOf(&siteDescriptor(), surface, inner, siteRoutePrefix(surface, std::move(name), inner));
	}
	const size_t position = chain != kNoInstance ? positionOf(chain) : kNoSlot;
	std::string name = position != kNoSlot ? "Chain " + std::to_string(position + 1) : std::string("Chain");
	if (chain == kNoInstance) {
		return name;
	}
	return backplane::innerNameOf(descriptorOf(chain), bodies_.surfaceOf(chain), inner, std::move(name));
}

} // namespace feathers::rack

namespace feathers::rack {

//-------------------------------------------------------------------------------------------------//
// GROWTH — unbounded devices step 6                                                                 //
//-------------------------------------------------------------------------------------------------//

void RackBridge::growMirror(size_t lanes) {
	m_.id.grow(lanes, [](DeviceInstanceId& id, size_t) { id = kNoInstance; });
	m_.desc.grow(lanes);
	m_.fieldsPresent.grow(lanes);
	m_.readingCount.grow(lanes);
	m_.playhead.grow(lanes);
	m_.playheadLevel.grow(lanes, [](float& level, size_t) { level = -1.0f; });
	m_.trigFires.grow(lanes);
	m_.recent.grow(lanes * device::kRecentEvents);
	m_.extraState.grow(lanes);
	m_.modulated.grow(lanes * controlStride_);
	m_.modulatedR.grow(lanes * controlStride_);
	m_.stereoLive.grow(lanes * device::kControlMaskWords);
	m_.readings.grow(lanes * readingStride_);
	// LAST, once every pool reaches `lanes`.
	mirrorLanes_.store(std::max(mirrorLanes_.load(std::memory_order_relaxed), lanes), std::memory_order_release);
}

void RackBridge::offerDrainGrowth() {
	reapDrainGrowth();
	const size_t want = record_.state().capacity();
	if (drainCapacity_ == 0) {
		drainCapacity_ = drained_.capacity(); // the shape the constructor gave the drain
	}
	if (want <= drainCapacity_ || drainGrowthState_.load(std::memory_order_acquire) != 0) {
		return; // big enough, or the last growth has not met its drain (the next commit asks again)
	}
	auto next = std::make_unique<DrainStates>();
	next->drained = ChainState(want, record_.state().chainCount());
	next->staging = ChainState(want, record_.state().chainCount());
	drainGrowth_ = std::move(next);
	drainCapacity_ = want;
	drainGrowthState_.store(1, std::memory_order_release);
}

void RackBridge::adoptDrainGrowth() noexcept {
	if (drainGrowthState_.load(std::memory_order_acquire) != 1) {
		return;
	}
	(void)drainGrowth_->drained.assignGrown(drained_);
	(void)drainGrowth_->staging.assignGrown(drainStaging_);
	swap(drained_, drainGrowth_->drained);
	swap(drainStaging_, drainGrowth_->staging);
	drainGrowthState_.store(2, std::memory_order_release);
}

void RackBridge::reapDrainGrowth() noexcept {
	if (drainGrowthState_.load(std::memory_order_acquire) != 2) {
		return;
	}
	drainGrowth_.reset();
	drainGrowthState_.store(0, std::memory_order_release);
}

} // namespace feathers::rack

namespace feathers::rack {

void RackBridge::releaseField(DeviceInstanceId device, size_t control, float value) {
	// A MOMENTARY'S RELEASE (`HeldFalls`; 2026-11-07, Pattern's Go pressed and released between two blocks was LOST): a fall
	// whose rise no drain has adopted waits, the record keeping the field up; `serviceHostSlots` lands it. An engine that
	// has never drained (an editor fixture) holds nothing.
	const device::Descriptor* d = record_.descriptorOf(device);
	const float coerced = d != nullptr && control < d->params.size() ? device::coerceControl(d->params[control], value) : value;
	const uint32_t adopted = drainedMirror_.load(std::memory_order_acquire);
	if (adopted != 0 && heldFalls_.hold(device, control, fieldAt(device, control), coerced, adopted)) {
		return;
	}
	setField(device, control, value);
}

} // namespace feathers::rack
