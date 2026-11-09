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

#include "backplane/inner_routes.h"
#include "device/format_value.h"
#include "plugin/device_registry.h"
#include "plugin/gui_bridge.h"
#include "rack/export_table.h"
#include "rack/extra_state_harvest.h"
#include "rack/site_chains.h"
#include "rack/site_routes.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace feathers {

namespace {

/// **SIXTY-FOUR BITS, BECAUSE A TAG IS EIGHT BYTES.** It was a uint32 while a tag was four. The
/// width is `device::kTagBytes`, and the packing must stay ONE atomic word: this crosses the
/// audio/UI seam, and `std::atomic<uint64_t>` is lock-free on both targets while anything wider is
/// not. A static_assert below holds the two facts together rather than trusting them to agree.
uint64_t packTag(const device::Tag& tag) {
	uint64_t packed = 0;
	for (size_t i = 0; i < tag.size(); ++i) {
		packed |= static_cast<uint64_t>(static_cast<unsigned char>(tag[i])) << (8 * i);
	}
	return packed;
}

device::Tag unpackTag(uint64_t packed) {
	device::Tag tag {};
	for (size_t i = 0; i < tag.size(); ++i) {
		tag[i] = static_cast<char>((packed >> (8 * i)) & 0xFFull);
	}
	return tag;
}

static_assert(device::kTagBytes * 8 <= 64,
              "a packed tag must fit one atomic word: this crosses the audio/UI seam and a wider "
              "type would stop being lock-free");

} // namespace

//---------------//
// UiParamQueue  //
//---------------//

// The two indices only ever grow, and the wrap is a mask on the way into the array. Comparing them
// as unbounded counters is what makes "full" and "empty" distinguishable at all — with indices that
// wrapped, both would read as write == read.

bool UiParamQueue::push(const UiParamEvent& event) noexcept {
	const size_t write = write_.load(std::memory_order_relaxed);
	const size_t read = read_.load(std::memory_order_acquire);
	if (write - read >= kCapacity) {
		dropped_.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	ring_[write & (kCapacity - 1)] = event;
	// Release: the consumer that sees this index must also see the event written above it.
	write_.store(write + 1, std::memory_order_release);
	return true;
}

bool UiParamQueue::pop(UiParamEvent& out) noexcept {
	const size_t read = read_.load(std::memory_order_relaxed);
	if (write_.load(std::memory_order_acquire) == read) {
		return false;
	}
	out = ring_[read & (kCapacity - 1)];
	// Release: the slot is not free for the producer to overwrite until the read above has happened.
	read_.store(read + 1, std::memory_order_release);
	return true;
}

bool UiParamQueue::empty() const noexcept {
	return write_.load(std::memory_order_acquire) == read_.load(std::memory_order_acquire);
}

uint32_t UiParamQueue::droppedCount() const noexcept {
	return dropped_.load(std::memory_order_relaxed);
}

//-------------//
// ParamBridge //
//-------------//

ParamBridge::ParamBridge() {
	// THE ENGINE'S ROUTE STORES GROW WITH THE LIST, BEFORE IT PUBLISHES A COUNT THAT NEEDS THEM (backlog OJ).
	routeList_.setGrowthHook(
	    [](void* host, size_t capacity) {
		    auto* bridge = static_cast<ParamBridge*>(host);
		    if (bridge->routeGrowth_) {
			    bridge->routeGrowth_(capacity);
		    }
	    },
	    this);
	// THE DRAWN CURVE STARTS FLAT — unity in every band, the identity (spectral::unityCurve), not the atomics' zero.
	restoreSpectralState(SpectralState {});
	// A NESTED RACK'S VERB THAT CHANGED THE POOL (host slots T11) MARKS THIS PATCH, where the pool is saved.
	slots_.setDirtyHook(this, [](void* host) {
		static_cast<ParamBridge*>(host)->stateDirty_.store(true, std::memory_order_relaxed);
	});
	// THE HOST'S OWN SOURCES, NAMED ONCE (backlog EG). The macros' names are the parameter table's, so a
	// route list naming a source and a knob naming its parameter cannot disagree; a weight is named by
	// the sound it weighs. `bipolar` starts false everywhere and follows the mode (`setMacroBipolar`).
	static constexpr std::array<const char*, kWeightSourceCount> kWeightNames {
		"Sound 1 weight", "Sound 2 weight", "Sound 3 weight", "Sound 4 weight",
		"Sound 5 weight", "Sound 6 weight", "Sound 7 weight", "Sound 8 weight",
	};
	static_assert(kWeightSourceCount <= kWeightNames.size(), "every weight source needs a name");
	for (size_t i = 0; i < kMacroCount; ++i) {
		const ParamDesc* desc = findParam(static_cast<clap_id>(macroParam(i)));
		coreSourceDescs_[i].name = desc != nullptr ? desc->name : "Macro";
	}
	for (size_t i = 0; i < kWeightSourceCount; ++i) {
		coreSourceDescs_[kMacroCount + i].name = kWeightNames[i];
	}
	// THE STACK INDEX (backlog KE step 3): −1..+1 across a stacked voice's parts, so bipolar from the start.
	coreSourceDescs_[kStackIndexSource].name = "Stack Index";
	coreSourceDescs_[kStackIndexSource].bipolar = true;
	// (THE DRIVE METERS START AT THE SENTINEL, NOT AT ZERO — `FxTelemetry::drivePeak`'s own initialiser
	// says so now, which is what makes a position the pool grows later say it too.)
	const std::span<const ParamDesc> descs = paramDescs();
	for (size_t i = 0; i < kParamCount; ++i) {
		// Defaults rather than zero, so an editor that draws before the engine has published anything
		// draws the patch a fresh plugin actually has.
		publishedBase_[i].store(descs[i].def, std::memory_order_relaxed);
		publishedMod_[i].store(descs[i].def, std::memory_order_relaxed);
		publishedModR_[i].store(descs[i].def, std::memory_order_relaxed);
		publishedStereoLive_[i].store(false, std::memory_order_relaxed);
		seenModR_[i] = descs[i].def;
		lastPublishedModR_[i] = descs[i].def;
		seenBase_[i] = descs[i].def;
		seenMod_[i] = descs[i].def;
		lastPublishedBase_[i] = descs[i].def;
		lastPublishedMod_[i] = descs[i].def;
	}
	changed_.reserve(kParamCount);

	// THE ROUTES, PUBLISHED ONCE BEFORE ANY EDIT — so the generation is never 0 and the engine's first
	// comparison always copies. See `routesGeneration()`.
	publishRoutes();

	// THE RACK A FRESH INSTANCE HAS, for the same reason the parameters above take their defaults
	// rather than zero: an editor that draws before any patch is loaded must draw the envelope this
	// plugin actually has, and a table of zeros is an envelope with no attack, no decay and no
	// sustain — a shape nobody chose and one that would make the first note sound broken.
	restoreModRack(defaultModRack());
	// AND ITS ENVELOPE ROUTED TO LEVEL (backlog EF) — the default patch's amp envelope, published with it.
	routeDefaultEnvelope();
	publishRoutes();
	// And the modulated table starts as the unmodulated one, for the same reason: an editor drawing
	// before the engine has published a block must not draw every control at zero.
	// THE SENTINEL ARRAY, not a zeroed one: before a block has been processed nobody has published a
	// drive meter, and a zero here would paint every transfer curve as driven by silence. See
	// `publishFxDrivePeak`.
	const FxDrivePeaks unmetered(kMaxFxSlots, -1.0f);
	// UNKNOWN, WHICH IS WHAT THIS CALL IS SAYING ABOUT EVERYTHING ELSE IT PASSES: no rack has run
	// yet, so there is no transport to report either.
	publishModulated(defaultModRack(), FxChain {}, RackPlayheads(kMaxRackSlots),
	                 mod::TransportClock::unknown(), unmetered);

	// AND THE LANE A FRESH INSTANCE HAS, which is the same sentence one chain over and was missing
	// only because nothing had ever needed to READ the lane before. feathers_clap restores this on a
	// state load and had no counterpart for the instance that has not loaded one.
	//
	// IT IS NOT COSMETIC NOW THAT THE FILTER'S CONTROLS LIVE IN THE ROW. A slot with no tag holds no
	// device, so `deviceSlots()` resolves nothing there and every one of the filter's controls
	// reports itself NOT A MODULATION DESTINATION — a card whose knobs could not be routed to until
	// somebody happened to turn one, which is precisely the class of thing that ships unnoticed. The
	// declared defaults come out of the empty row either way; what the tag adds is the device's
	// IDENTITY, and identity is what the destination space is built on.
	restoreFxChain(defaultFxChain());
}

namespace {

/// THE IDENTITY AT `position` OF `chain` IN THE RECORD, or none.
///
/// **THERE IS ONLY ONE HALF OF IT LEFT (kill-lanes step 3c).** `DeviceIdentity` carried a serial and a
/// routing lane; step 3b stopped the record answering for the lane (it returned `kNoRouteLane` here) and
/// 3c deleted the field. Nothing on this road ever wanted it: every caller either takes `.instanceId`, or
/// hands the whole identity to `toDeviceRow` and sends the row it builds back into the record BY ID
/// (`storeRowOf`).
DeviceIdentity identityIn(const rack::ChainRecord& record, size_t chain, size_t position) noexcept {
	const DeviceInstanceId id = record.deviceAt(chain, position);
	if (id == kNoInstance) {
		return {};
	}
	return { id };
}

/// WHERE `device` STANDS IN `chain`, or `none` — a device the other chain holds is not this chain's.
size_t positionIn(const rack::ChainRecord& record, size_t chain, DeviceInstanceId device, size_t none) noexcept {
	if (record.chainOf(device) != chain) {
		return none;
	}
	const size_t at = record.positionOf(device);
	return at == rack::ChainRecord::kNoPosition ? none : at;
}

} // namespace

DeviceIdentity ParamBridge::fxIdentityAt(size_t slot) const noexcept {
	return identityIn(chains_, kLaneChain, slot);
}

DeviceIdentity ParamBridge::rackIdentityAt(size_t slot) const noexcept {
	return identityIn(chains_, kStripChain, slot);
}

size_t ParamBridge::fxSlotOf(DeviceInstanceId device) const noexcept {
	// A DEVICE THIS RECORD DOES NOT HOLD IS IN NO CHAIN, so `positionIn` answers `none` for it on its own —
	// the "is it held at all" lookup that used to stand in front of this was the lane's, and it has gone
	// with the lane (kill-lanes step 3b).
	return positionIn(chains_, kLaneChain, device, kNoPosition);
}

size_t ParamBridge::rackSlotOf(DeviceInstanceId device) const noexcept {
	return positionIn(chains_, kStripChain, device, kNoPosition);
}

//------------------------------------------------------------------------------------------------//
// THE ROUTES INSIDE A CHAIN, FROM ONE OF THIS HOST'S SOURCES (rack roadmap §7)                     //
//------------------------------------------------------------------------------------------------//

namespace {

/// A CHAIN IN THE LANE WITH A RUNNING BODY, and the ops that reach into it — or nulls.
struct ChainReach {
	const device::ContainerOps* ops = nullptr;
	void* surface = nullptr;
};

} // namespace

std::vector<ParamBridge::InnerRoute> ParamBridge::innerRoutesFrom(backplane::Source source) const {
	std::vector<InnerRoute> out;
	if (source.isOuter()) {
		return out; // this host's sources only: a source already scoped is not one of ours to walk
	}
	// AND THE ROUTES INSIDE EVERY DEVICE'S SITE (backlog NR), under its site key with the site's own table.
	for (size_t slot = 0; slot < chains_.length(kLaneChain); ++slot) {
		const DeviceInstanceId chain = fxDeviceAt(slot);
		if (chain != kNoInstance) {
			backplane::innerRoutesOf(descriptorOf(chain), fxBodies_.surfaceOf(chain), chain, source, out);
			const DeviceInstanceId site = siteKeyOf(chain);
			backplane::innerRoutesOf(&rack::siteDescriptor(), fxBodies_.surfaceOf(site), site, source, out);
		}
	}
	return out;
}

float ParamBridge::innerRouteAmount(backplane::Source source, DeviceInstanceId chain, backplane::Destination inner) const {
	if (chain == kNoInstance) {
		return 0.0f;
	}
	return backplane::innerRouteAmountOf(rack::containerTableFor(chain, descriptorOf(chain)), fxBodies_.surfaceOf(chain),
	                                     source, inner);
}

backplane::Source ParamBridge::fxContainerExport(DeviceInstanceId container, backplane::Source inner) {
	return rack::exportFromContainer(descriptorOf(container), fxBodies_.surfaceOf(container), container, inner);
}

size_t ParamBridge::sweepExports(uint32_t drained) {
	const uint32_t published = routesGeneration();
	// EVERY DEVICE THE BRIDGE HOLDS, in any chain: a container or a site is a table, and any of them may hold a reader.
	std::vector<DeviceInstanceId> devices;
	const rack::ChainState& state = chains_.state();
	for (size_t c = 0; c < state.chainCount(); ++c) {
		for (const auto& row : state.chain(c)) {
			devices.push_back(row.instanceId);
		}
	}
	// WHO READS `asHostSees` (spelled as this bridge does): a route here, or one inside a container or a site.
	const auto read = [&](backplane::Source asHostSees) {
		bool found = false;
		forEachRoute([&](const backplane::Route& r) { found = found || r.source == asHostSees; });
		std::vector<backplane::DeviceHost::InnerRoute> inner;
		for (size_t i = 0; i < devices.size() && !found; ++i) {
			const DeviceInstanceId other = devices[i];
			inner.clear();
			backplane::innerRoutesOf(descriptorOf(other), fxBodies_.surfaceOf(other), other, asHostSees, inner);
			backplane::innerRoutesOf(&rack::siteDescriptor(), fxBodies_.surfaceOf(siteKeyOf(other)), siteKeyOf(other),
			                         asHostSees, inner);
			found = !inner.empty();
		}
		return found;
	};
	size_t freed = 0;
	for (const DeviceInstanceId holder : devices) {
		const device::Descriptor* d = descriptorOf(holder);
		void* surface = fxBodies_.surfaceOf(holder);
		if (d != nullptr && surface != nullptr && device::exportsContainer(*d) && d->container->exportedSource != nullptr) {
			std::vector<bool> used(device::kSiteExportRows, false);
			for (size_t row = 0; row < used.size(); ++row) {
				used[row] = d->container->exportedSource(surface, row) != backplane::Source::nobody()
				            && read(backplane::Source::containerExported(holder, row));
			}
			freed += rack::sweepContainerExports(d, surface, used, published, drained);
		}
		auto* site = static_cast<rack::SiteChains*>(fxBodies_.surfaceOf(siteKeyOf(holder)));
		if (site != nullptr && site->owner() == holder && site->exportRowsUsed() > 0) {
			std::vector<bool> used(device::kSiteExportRows, false);
			for (size_t row = 0; row < used.size(); ++row) {
				used[row] = site->exportedSource(row) != backplane::Source::nobody()
				            && read(backplane::Source::exported(holder, row));
			}
			freed += site->sweepExports(used, published, drained);
		}
	}
	return freed;
}

bool ParamBridge::loadFxSite(DeviceInstanceId device, std::span<const uint8_t> body) {
	if (!openFxSite(device)) {
		return false;
	}
	const DeviceInstanceId key = siteKeyOf(device);
	if (void* surface = fxBodies_.surfaceOf(key); surface != nullptr) {
		const device::ContainerOps& ops = rack::siteContainerOps();
		if (ops.applyState == nullptr || !ops.applyState(surface, body.data(), body.size())) {
			return false;
		}
	}
	else if (!fxBodies_.store(key, body.data(), body.size())) {
		return false;
	}
	markStateDirty();
	return true;
}

backplane::Source ParamBridge::fxSiteExport(DeviceInstanceId owner, backplane::Source inner) {
	return rack::exportFromSite(fxBodies_.surfaceOf(siteKeyOf(owner)), owner, inner);
}

bool ParamBridge::setInnerRouteAmount(backplane::Source source, DeviceInstanceId chain, backplane::Destination inner,
                                      float amount) {
	if (chain == kNoInstance) {
		return false;
	}
	return backplane::setInnerRouteOf(rack::containerTableFor(chain, descriptorOf(chain)), fxBodies_.surfaceOf(chain),
	                                  source, inner, amount);
}

std::string ParamBridge::innerDestinationName(DeviceInstanceId chain, backplane::Destination inner) const {
	// A SITE'S ROW IS NAMED BY ITS HOLDER AND LANE ("Filter 2 PRE · Filter · Cutoff"); a container's by its place.
	if (rack::isSiteKey(chain)) {
		const DeviceInstanceId holder = chain & ~kSiteKeyBit;
		const device::Descriptor* d = descriptorOf(holder);
		const size_t at = fxSlotOf(holder);
		std::string name = d != nullptr ? std::string(d->traits.name) : std::string("Device");
		if (at != kNoPosition) {
			name += " " + std::to_string(at + 1);
		}
		void* surface = fxBodies_.surfaceOf(chain);
		return backplane::innerNameOf(&rack::siteDescriptor(), surface, inner,
		                              rack::siteRoutePrefix(surface, std::move(name), inner));
	}
	const size_t slot = chain != kNoInstance ? fxSlotOf(chain) : kNoPosition;
	std::string name = slot != kNoPosition ? "Chain " + std::to_string(slot + 1) : std::string("Chain");
	if (chain == kNoInstance) {
		return name;
	}
	return backplane::innerNameOf(descriptorOf(chain), fxBodies_.surfaceOf(chain), inner, std::move(name));
}

std::string ParamBridge::destinationName(backplane::Destination destination) const {
	// A PARAMETER SAYS ITS OWN NAME, and so does a device control — they simply say it from different
	// places. The DEVICE'S own name for its control ("Env Attack"), qualified by where the device stands
	// once there is more than one to tell apart: the device qualified the control, the host qualifies the
	// device. ASKED OF THE DEVICE THE ID NAMES, not of a device type. The endpoint carries the CONTROL
	// index itself (kill-lanes step 2), so there is no ordinal to map back.
	if (!destination.isDevice()) {
		// AN INTERNAL DESTINATION NAMES ITSELF WITH ITS SOUND, the way the per-sound parameters do
		// ("C Width"): the descriptor says "Radius" for every slot, and a routes list showing eight
		// rows all called "Radius" would make the sound the one thing a reader cannot tell.
		if (isInternalDestination(destination.index)) {
			const size_t internal = destination.index - kParamDestinationCount;
			std::string name(1, static_cast<char>('A' + internal / kInternalDestinationsPerSlot));
			return name + " " + internalDestinationDesc(internal).name;
		}
		if (const ParamDesc* desc = findParam(static_cast<clap_id>(destinationParamId(destination.index)));
		    desc != nullptr) {
			return desc->name;
		}
		return "Route";
	}
	const device::Descriptor* deviceDesc = descriptorOf(destination.device);
	if (deviceDesc == nullptr || destination.index >= deviceDesc->params.size()) {
		return "Route"; // an id nothing holds names nothing
	}
	std::string name = deviceDesc->params[destination.index].name;
	size_t position = kNoPosition;
	if (const size_t rack = rackSlotOf(destination.device); rack != kNoPosition) {
		position = rack;
	}
	else if (const size_t fx = fxSlotOf(destination.device); fx != kNoPosition) {
		position = fx;
	}
	if (position != kNoPosition) {
		name += " ";
		name += std::to_string(position + 1);
	}
	return name;
}

const device::Descriptor* ParamBridge::descriptorOf(DeviceInstanceId id) const noexcept {
	const backplane::DeviceView v = chains_.devices();
	return v.descriptorOf(v.host, id);
}

void ParamBridge::publishChains() noexcept {
	// THE READINGS REACH EVERY LANE THE RECORD CAN HOLD BEFORE A DEVICE CAN STAND IN ONE (unbounded devices
	// step 5): the pools grow here, on the message thread, ahead of the publish that names the device.
	const size_t lanes = chains_.state().capacity();
	fxTelemetry_.grow(lanes);
	rackTelemetry_.grow(lanes);
	(void)publishedChains_.publish(chains_.state());
	repaintDirty_.store(true, std::memory_order_relaxed);
}

void ParamBridge::rebuildChain(size_t chain, std::span<const DeviceRow* const> rows) noexcept {
	// **THE RULES' ANSWER, WRITTEN BACK BY THE LEAST CHANGE** (`ChainRecord::replaceChain`): a device
	// named before and after under the same id keeps its node AND ITS ROUTES, so an add or a
	// knob-free restore touches no route; a removed device's routes go with it; a reorder deletes the ones
	// it makes backwards. (The first C4 draft removed and re-inserted every device — harmless while the
	// routes lived beside the record, and a whole unrouted chain the moment they moved in.)
	//
	// **AND THE ORDER IS THE WHOLE ANSWER NOW (kill-lanes step 3b).** This carried a second span of lane
	// numbers beside the rows and hunted the chain's band for a free one per row — the row's own
	// `routeLane` where the band allowed it, the lowest free lane otherwise. Where the record parks a
	// device is the record's business, so what crosses is the rows in order and nothing here names a slot;
	// the chain's own width is the only bound left, and it is the record's to report.
	// **ROOM FIRST** (unbounded devices step 6): a chain is never full — the record grows to hold every row
	// before one is placed. The rows are the message thread's, so the scratch they are named in may allocate.
	(void)chains_.growChain(chain, rows.size());
	std::vector<rack::DeviceState> states(rows.size());
	size_t n = 0;
	const auto idTaken = [&](DeviceInstanceId id) {
		for (size_t i = 0; i < n; ++i) {
			if (states[i].instanceId == id) {
				return true;
			}
		}
		return chains_.holds(id) && chains_.chainOf(id) != chain;
	};
	for (const DeviceRow* row : rows) {
		// PAST THE CHAIN'S WIDTH NOTHING IS PLACED — the old band-full test, asked of the record. Tested
		// BEFORE an id is minted, so a row that cannot land does not burn a serial.
		if (row == nullptr || n >= states.size() || n >= chains_.capacity(chain)) {
			continue;
		}
		rack::DeviceState state = deviceStateOf(*row);
		// AN ID THE RECORD CAN KEY A DEVICE ON: a row with none, or one the other chain (or an earlier row)
		// already holds, is given a fresh one — a device under no id is a device nothing can route to, and
		// two under one are two devices at one address (D1).
		if (state.instanceId == kNoInstance || idTaken(state.instanceId)) {
			state.instanceId = ids_.issue();
		}
		states[n] = state;
		++n;
	}
	// A KEPT DEVICE'S NEW ROW MAY MOVE A BOUND CONTROL (host slots, T6b): what the rebuild CHANGED is seeded
	// before the caller publishes, and nothing it merely carried (`SlotOwner::seedChangedBound`). A device
	// `replaceChain` dropped keeps its binding until the next visit's reconcile frees its slot and the rebuild
	// drops it; meanwhile the engine resolves no device for it and drives nothing.
	const rack::SlotOwner::BoundBefore before = slots_.captureBound(kNoInstance);
	(void)chains_.replaceChain(chain, std::span<const rack::DeviceState>(states.data(), n));
	slots_.seedChangedBound(before);
	// A STRUCTURAL CHANGE CAN CHANGE WHAT REACHES WHAT — a removal, a reorder's deletions.
	publishRoutes();
}

void ParamBridge::storeRowOf(DeviceInstanceId device, const DeviceRow& row) noexcept {
	const size_t chain = chains_.chainOf(device);
	if (chain == rack::ChainRecord::kNoChain) {
		return;
	}
	const rack::DeviceState& held = chains_.rowOf(device);
	if (held.tag == row.tag) {
		// A BOUND CONTROL THIS ROW MOVED reaches the bus before the record publishes, or the next drain puts
		// the bus's older value back over it (host slots, T6b) — and ONLY one it moved: the row carries every
		// other control at the record's copy, which may be older than a host write not yet mirrored.
		const rack::SlotOwner::BoundBefore before = slots_.captureBound(device);
		(void)chains_.setFields(device, std::span<const float>(row.field.data(),
		                                                       std::min(row.fieldsPresent, row.field.size())));
		slots_.seedChangedBound(before);
	}
	else {
		// ANOTHER DEVICE STAMPED ONTO THE PLACE — a replacement, at the same position under the same id.
		const size_t at = chains_.positionOf(device);
		rack::DeviceState state = deviceStateOf(row);
		state.instanceId = held.instanceId;
		(void)chains_.remove(device);
		(void)chains_.insertRow(chain, at, state);
	}
	publishChains();
}

ModRack ParamBridge::modRack() const noexcept {
	// A VIEW OF THE RECORD'S STRIP — the one mapping `feathers_chains.h` states, so this and the audio
	// thread's snapshot cannot disagree about what the view puts where.
	FxChain unused;
	ModRack rack;
	// AS LONG AS THE STRIP IS — a view sized to what it shows (unbounded devices step 5).
	rack.device.resize(std::max(rack.device.size(), chains_.capacity(kStripChain)));
	chainsOfState(chains_.state(), &unused, &rack);
	rack.nextInstanceId = ids_.next();
	return rack;
}

void ParamBridge::setEnvelopeSettings(size_t slot, const mod::dahdsr::Settings& settings) noexcept {
	if (slot >= chains_.length(kStripChain)) {
		return;
	}
	// THROUGH THE SHARED CONVERSION, NOT A LOOP OF MY OWN — device_row.h stamps fieldsPresent from
	// the array's own width, so there is no count here to get wrong.
	const DeviceRow carried = toDeviceRow(mod::dahdsr::kTraits.tag, rackIdentityAt(slot),
	                                      mod::dahdsr::rowOf(settings));
	storeRackRow(slot, carried);
	repaintDirty_.store(true, std::memory_order_relaxed);
	// An edit, unlike the restore below — a hand moved an envelope knob, and the patch is modified.
	stateDirty_.store(true, std::memory_order_relaxed);
}

void ParamBridge::setMsegSettings(size_t slot, const mod::mseg::Settings& settings) noexcept {
	if (slot >= chains_.length(kStripChain)) {
		return;
	}
	// THE MSEG'S TAG AND THE MSEG'S FIELD COUNT, taken from its own declarations exactly as the
	// envelope's are from its. This is the line that would be wrong if a card assembled the row: the
	// numbers would be an MSEG's and the tag could be anything, and the slot would reload as whatever
	// the tag said.
	const DeviceRow carried = toDeviceRow(mod::mseg::kTraits.tag, rackIdentityAt(slot),
	                                      mod::mseg::rowOf(settings));
	storeRackRow(slot, carried);
	repaintDirty_.store(true, std::memory_order_relaxed);
	stateDirty_.store(true, std::memory_order_relaxed);
}

void ParamBridge::storeRackRow(size_t slot, const DeviceRow& row) noexcept {
	// INTO THE DEVICE THE SLOT HOLDS — its fields, and its tag if that changed.
	storeRowOf(rackIdentityAt(slot).instanceId, row);
}

void ParamBridge::restoreModRack(const ModRack& rack) noexcept {
	// **THE MAXIMUM, NOT THE VALUE — because there is one counter and two restores.** A state load
	// restores the rack and the lane, and whichever ran second used to overwrite the first; with one
	// sequence behind both that would wind the counter back over ids the other chain is already using
	// and the next add would collide. See `ids_`. First, so an id a row lacks is issued past the patch's.
	adoptInstanceCounter(rack.nextInstanceId);
	std::vector<const DeviceRow*> rows(rack.device.size(), nullptr);
	size_t n = 0;
	for (size_t slot = 0; slot < rack.count && slot < rack.device.size() && n < rows.size(); ++slot) {
		rows[n++] = &rack.device[slot];
	}
	rebuildChain(kStripChain, std::span<const DeviceRow* const>(rows.data(), n));
	publishChains();
	// A patch load is not an edit — the same rule every restore on this side follows.
}

size_t ParamBridge::addRackDevice(const char* tag) noexcept {
	// READ, ASK, WRITE BACK. The rack's rules live in mod_rack.h with the rack, so this reads the
	// current one, hands it to the function that knows what may go in it, and stores the answer.
	// Nothing here decides whether the device is allowed — see the header.
	ModRack rack = modRack();
	const size_t slot = feathers::addRackDevice(&rack, tag);
	if (slot == kNoPosition) {
		return kNoPosition; // refused, and nothing was written
	}
	restoreModRack(rack);
	// AND THIS ONE IS AN EDIT, unlike the restore above it. A hand added a device; the patch is
	// modified and a host that asks will be told so.
	stateDirty_.store(true, std::memory_order_relaxed);
	return slot;
}

size_t ParamBridge::insertRackDeviceBefore(DeviceInstanceId anchor, const char* tag) noexcept {
	ModRack rack = modRack();
	// **THE ANCHOR GOES STRAIGHT THROUGH — kill-lanes step 3c.** For one step this crossed the id into a
	// lane, because the record had stopped answering `laneOf` (3b) while `feathers::insertRackDeviceBefore`
	// still took one: it asked the record for the anchor's POSITION and read the lane the view had minted
	// for the row standing there. Both ends speak identity now, so there is nothing to cross. An anchor
	// the rack does not hold appends, which is the free function's rule rather than this one's.
	const size_t slot = feathers::insertRackDeviceBefore(&rack, anchor, tag);
	if (slot == kNoPosition) {
		return kNoPosition; // refused, and nothing was written
	}
	// THE WHOLE RACK: every row from `slot` on has a new position. Routes are untouched — a route names
	// its device's id (kill-lanes step 2) and a device keeps its id across a reorder.
	restoreModRack(rack);
	stateDirty_.store(true, std::memory_order_relaxed);
	repaintDirty_.store(true, std::memory_order_relaxed);
	return slot;
}

void ParamBridge::publishModulated(const ModRack& rack, const FxChain& fx,
                                      const RackPlayheads& playheads, mod::TransportClock clock,
                                      const FxDrivePeaks& drivePeaks, const RackPlayheads& fxPlayheads) noexcept {
	for (size_t slot = 0; slot < playheads.size(); ++slot) {
		publishRackPlayhead(slot, playheads[slot]);
	}
	// AND THE LANE'S MODULATORS' (2026-10-04), on the same terms.
	for (size_t position = 0; position < fxPlayheads.size(); ++position) {
		publishFxPlayhead(position, fxPlayheads[position]);
	}
	// **THE TEMPO TRAVELS WITH THEM — the same kind of fact on the same tick**, which is the argument
	// the drive meters make two lines below. One number for the whole rack rather than one per slot:
	// every device here runs against one transport.
	//
	// **ZERO WHEN THE HOST HAS NOT SAID, AND THE GUARD IS `haveTempo()` RATHER THAN A COMPARISON.**
	// `TransportClock` already owns the question of whether its fields mean anything; asking it is
	// what keeps "no transport" spelled one way. A non-finite or negative value would reach a card
	// that divides by it, so it is refused here rather than at every reader — the same shape
	// `publishRackPlayhead` uses for its phase, and for the same reason.
	rackSecondsPerBeat_.store(
	    clock.haveTempo() && std::isfinite(clock.secondsPerBeat) && clock.secondsPerBeat > 0.0
	        ? clock.secondsPerBeat
	        : 0.0,
	    std::memory_order_relaxed);
	// THE DRIVE METERS TRAVEL WITH THE PLAYHEADS — the same kind of fact on the same tick.
	for (size_t slot = 0; slot < drivePeaks.size(); ++slot) {
		publishFxDrivePeak(slot, drivePeaks[slot]);
	}
	// **THE DEVICE'S OWN ROW, NOT AN ENVELOPE'S (BK).** This read
	// `mod::dahdsr::rowOf(rack.dahdsrAt(slot))`, and `dahdsrAt` checks the tag and answers `{}` for
	// anything that is not an envelope — so an MSEG slot published DEFAULT DAHDSR SETTINGS. Not
	// garbage, which would have been noticed: a plausible row in the wrong control space. Nothing read
	// this channel generically, so nothing said so, and `modulatedEnvelope` had no coverage at all.
	//
	// It is the shape test_mod_rack.cpp already pins once: `modulatedMsegSettings` walked the
	// envelope's descriptor for an MSEG's controls, so a hand aiming a macro at Rate moved how many
	// segments the shape had. One device type's layout standing in for another's.
	//
	// THE STRIDE IS THE GENERIC ONE NOW, and the storage was always sized for it — the modulated row is
	// `kMaxDeviceFields` wide and only the writer and reader were envelope-shaped.
	for (size_t slot = 0; slot < rack.device.size(); ++slot) {
		RackTelemetry* t = rackTelemetry_.at(slot);
		if (t == nullptr) {
			break;
		}
		const DeviceRow row = slot < rack.device.size() ? rack.device[slot] : DeviceRow {};
		const size_t present = std::min({ row.fieldsPresent, kMaxDeviceFields, row.field.size() });
		// **THE ROW'S WIDTH, NOT THE ARRAY'S** — this is the audio thread, once a block, and the first
		// draft wrote all 128 cells per slot because that was the simplest correct loop. It is also
		// eight times the stores for a device using sixteen of them, which is precisely the waste BL
		// was raised about one file over. Writing what the row HAS costs what the old envelope-shaped
		// loop cost.
		//
		// NOTE THE WIDTH IS THE ROW'S AND NOT `params.size()`: device_row.h is explicit that the
		// floats past the parameter count are the device's OPAQUE STATE BLOB — the drawn curve's bands
		// live there — so a loop bounded by the descriptor would publish a control's modulation and
		// silently drop the state beside it.
		for (size_t f = 0; f < present; ++f) {
			t->modulated[f].store(row.field[f], std::memory_order_relaxed);
		}
		// AND THE TAIL A SHRINKING ROW LEFT BEHIND, zeroed down to the width last published rather
		// than across the whole array. A rack that has shrunk, or a slot whose device has been swapped
		// for a narrower one, must not go on reporting the old device's fields under the new one's
		// control indices — but only the cells that were actually written can be stale.
		const size_t previous =
		    std::min(t->modulatedFieldsPresent.load(std::memory_order_relaxed), kMaxDeviceFields);
		for (size_t f = present; f < previous; ++f) {
			t->modulated[f].store(0.0f, std::memory_order_relaxed);
		}
		// THE WIDTH AFTER THE FIELDS, never before — a reader that saw the width first would be told
		// the row reaches further than it has been written to.
		t->modulatedTag.store(packTag(row.tag), std::memory_order_relaxed);
		t->modulatedFieldsPresent.store(present, std::memory_order_relaxed);
	}
	// **AND THE LANE, ON THE SAME TERMS.** `Synth::modulatedFxChain()` is the chain `applyParams`
	// already builds to drive the runner — see the note there — so this publishes the engine's own
	// arithmetic rather than a second copy of it.
	for (size_t slot = 0; slot < fx.device.size(); ++slot) {
		FxTelemetry* t = fxTelemetry_.at(slot);
		if (t == nullptr) {
			break;
		}
		const DeviceRow row = fxDeviceAtPosition(fx, slot).row;
		const size_t present = std::min({ row.fieldsPresent, kMaxDeviceFields, row.field.size() });
		for (size_t f = 0; f < present; ++f) {
			t->modulated[f].store(row.field[f], std::memory_order_relaxed);
		}
		const size_t previous =
		    std::min(t->modulatedFieldsPresent.load(std::memory_order_relaxed), kMaxDeviceFields);
		for (size_t f = present; f < previous; ++f) {
			t->modulated[f].store(0.0f, std::memory_order_relaxed);
		}
		// THE WIDTH AFTER THE FIELDS, for the reason given on the rack's loop above.
		t->modulatedFieldsPresent.store(present, std::memory_order_relaxed);
	}
	// NOT repaintDirty_. This is published every block whether anything moved or not, and setting the
	// repaint flag here would mean the editor redraws at block rate forever — which is what the flag
	// exists to avoid. A widget that wants to follow a moving modulation asks for its own redraw; the
	// knobs already work that way, and their readouts deliberately do not chase.
}

namespace {

uint64_t packPlayhead(mod::Playhead where) noexcept {
	// WRAPPED AND MADE FINITE BEFORE PACKING, for `publishAutomodPhase`'s reason: the value crosses a
	// thread boundary into a drawing that multiplies it by a width, and a NaN there is an element
	// drawn nowhere at all rather than one drawn wrongly. A device cannot produce these — sst clamps
	// its phase and the MSEG's is a fraction of a segment — so this is a guard on the arithmetic.
	const float phase = std::isfinite(where.phase) ? std::clamp(where.phase, 0.0f, 1.0f) : 0.0f;
	return (static_cast<uint64_t>(where.ordinal) << 32) | static_cast<uint64_t>(std::bit_cast<uint32_t>(phase));
}

mod::Playhead unpackPlayhead(uint64_t packed) noexcept {
	return { static_cast<uint32_t>(packed >> 32),
		     std::bit_cast<float>(static_cast<uint32_t>(packed & 0xFFFFFFFFu)) };
}

/// THE LEVEL'S WORD — see `RackTelemetry::playheadLevel`: negative is "not published", and a published level is
/// made finite and kept in 0..1 for `packPlayhead`'s reason.
float packPlayheadLevel(const mod::Playhead& where) noexcept {
	if (!where.haveLevel) {
		return -1.0f;
	}
	return std::isfinite(where.level) ? std::clamp(where.level, 0.0f, 1.0f) : 0.0f;
}

mod::Playhead withPlayheadLevel(mod::Playhead where, float packed) noexcept {
	where.haveLevel = packed >= 0.0f;
	where.level = where.haveLevel ? packed : 0.0f;
	return where;
}

/// THE TRIG'S WORD — see `RackTelemetry::trigFires`: bit 32 says published, the low word is the count.
uint64_t packTrigFires(const mod::Playhead& where) noexcept {
	return where.haveTrigFires ? (uint64_t { 1 } << 32) | where.trigFires : 0;
}

mod::Playhead withTrigFires(mod::Playhead where, uint64_t packed) noexcept {
	where.haveTrigFires = (packed >> 32) != 0;
	where.trigFires = static_cast<uint32_t>(packed & 0xFFFFFFFFu);
	return where;
}

/// THE NEWEST EVENTS IN, AND OUT — a word each (`RackTelemetry::recent`).
template <typename Cells>
void storeRecent(Cells& cells, const mod::Playhead& where) noexcept {
	for (size_t i = 0; i < cells.size(); ++i) {
		cells[i].store(device::packRecentEvent(where.recent[i]), std::memory_order_relaxed);
	}
}
template <typename Cells>
mod::Playhead withRecent(mod::Playhead where, const Cells& cells) noexcept {
	for (size_t i = 0; i < cells.size(); ++i) {
		where.recent[i] = device::unpackRecentEvent(cells[i].load(std::memory_order_relaxed));
	}
	return where;
}

} // namespace

void ParamBridge::publishRackPlayhead(size_t slot, mod::Playhead where) noexcept {
	RackTelemetry* t = rackTelemetry_.at(slot);
	if (t == nullptr) {
		return;
	}
	t->playheadLevel.store(packPlayheadLevel(where), std::memory_order_relaxed);
	t->trigFires.store(packTrigFires(where), std::memory_order_relaxed);
	storeRecent(t->recent, where);
	t->playhead.store(packPlayhead(where), std::memory_order_relaxed);

	// NOT `repaintDirty_` — see the declaration, and the two publishers above it that refuse the same
	// flag for the same measured reason.
}

mod::Playhead ParamBridge::rackPlayhead(size_t slot) const noexcept {
	const RackTelemetry* t = rackTelemetry_.at(slot);
	if (t == nullptr) {
		return {};
	}
	// ONE LOAD, WHICH IS THE WHOLE POINT OF PACKING: the ordinal and the phase a reader gets are the
	// two halves of ONE publish, and no interleaving can hand back a mixture of two. (The level beside them is
	// its own word — see `RackTelemetry::playheadLevel` for why a tick apart is acceptable there.)
	return withRecent(withTrigFires(withPlayheadLevel(unpackPlayhead(t->playhead.load(std::memory_order_relaxed)),
	                                    t->playheadLevel.load(std::memory_order_relaxed)),
	                                t->trigFires.load(std::memory_order_relaxed)),
	                  t->recent);
}

void ParamBridge::publishRackExtraState(size_t slot, DeviceInstanceId device, uint64_t serial, const float* values,
                                        size_t count) noexcept {
	if (RackTelemetry* t = rackTelemetry_.at(slot); t != nullptr) {
		(void)t->extraState.publish(device, serial, values, count);
	}
}

void ParamBridge::publishFxExtraState(size_t position, DeviceInstanceId device, uint64_t serial, const float* values,
                                      size_t count) noexcept {
	if (FxTelemetry* t = fxTelemetry_.at(position); t != nullptr) {
		(void)t->extraState.publish(device, serial, values, count);
	}
}

void ParamBridge::harvestExtraState() noexcept {
	// EVERY SNAPSHOT THE BLOCK LEFT, strip and lane alike — each names its device, so a slot that has since held
	// another device writes nothing over it (`rack::harvestExtraState`'s guard).
	bool changed = false;
	device::ExtraStateSnapshot::Read read;
	for (size_t slot = 0; slot < rackTelemetry_.size(); ++slot) {
		if (const RackTelemetry* t = rackTelemetry_.at(slot); t != nullptr && t->extraState.read(&read)) {
			changed = rack::harvestExtraState(&chains_, read) || changed;
		}
	}
	for (size_t position = 0; position < fxTelemetry_.size(); ++position) {
		if (const FxTelemetry* t = fxTelemetry_.at(position); t != nullptr && t->extraState.read(&read)) {
			changed = rack::harvestExtraState(&chains_, read) || changed;
		}
	}
	// AND THE KEYED BLOCKS THAT ASK TO BE KEPT CURRENT (backlog NM `harvestOnTick`) — into the store, not the record:
	// nothing is republished for them.
	try {
		(void)rack::harvestBlocks(chains_, &fxBodies_);
	} catch (...) {
		// an allocation that failed leaves the store as it was; the next tick tries again
	}
	// NOT AN EDIT — the record was behind what is playing. No dirty mark, no undo step.
	if (changed) {
		publishChains();
		repaintDirty_.store(true, std::memory_order_relaxed);
	}
}

void ParamBridge::publishFxPlayhead(size_t position, mod::Playhead where) noexcept {
	if (FxTelemetry* t = fxTelemetry_.at(position); t != nullptr) {
		t->playheadLevel.store(packPlayheadLevel(where), std::memory_order_relaxed);
		t->trigFires.store(packTrigFires(where), std::memory_order_relaxed);
		storeRecent(t->recent, where);
		t->playhead.store(packPlayhead(where), std::memory_order_relaxed); // not `repaintDirty_`, as above
	}
}

mod::Playhead ParamBridge::fxPlayhead(size_t position) const noexcept {
	const FxTelemetry* t = fxTelemetry_.at(position);
	return t == nullptr
	           ? mod::Playhead {}
	           : withRecent(withTrigFires(withPlayheadLevel(unpackPlayhead(t->playhead.load(std::memory_order_relaxed)),
	                                                        t->playheadLevel.load(std::memory_order_relaxed)),
	                                      t->trigFires.load(std::memory_order_relaxed)),
	                        t->recent);
}

mod::dahdsr::Settings ParamBridge::modulatedEnvelope(size_t slot) const noexcept {
	const RackTelemetry* t = rackTelemetry_.at(slot);
	if (t == nullptr) {
		return {};
	}
	// **THE TAG IS CHECKED HERE NOW, BECAUSE THE CHANNEL STOPPED BEING ENVELOPE-SHAPED.** It used to
	// be checked for us: the publisher called `rack.dahdsrAt(slot)`, which refused a foreign tag and
	// handed back `{}`. Now that the publisher carries each device's OWN row, decoding an MSEG's
	// fields through the envelope's layout would be exactly the bug BK exists to remove, arriving by
	// the other door. `settingsOf` cannot tell — a row is floats.
	const device::Tag tag = unpackTag(t->modulatedTag.load(std::memory_order_relaxed));
	if (!device::sameTag(tag.data(), mod::dahdsr::kTraits.tag)) {
		return {};
	}
	const size_t present =
	    std::min(t->modulatedFieldsPresent.load(std::memory_order_relaxed), kDahdsrFieldCount);
	DahdsrRow row {};
	for (size_t f = 0; f < present; ++f) {
		row[f] = t->modulated[f].load(std::memory_order_relaxed);
	}
	return mod::dahdsr::settingsOf(row, present);
}

void ParamBridge::beginGesture(clap_id id) noexcept {
	if (paramIndexOf(id) < 0) {
		return;
	}
	// Counted where the event is PUSHED, not beside the call — a count taken anywhere else could stay
	// balanced while the queue went unfed, which is the failure it exists to catch.
	++openGestures_;
	queue_.push({ UiParamEvent::Kind::GestureBegin, id, 0.0 });
}

void ParamBridge::setValue(clap_id id, double value) noexcept {
	// Unknown ids are refused at the door rather than passed along for ParamSet to ignore.
	//
	// It used to be the other way round, on the reasoning that ParamSet already ignores what it does
	// not know and a second check here is a second place for the two to disagree. What changed is that
	// the queue is no longer the end of the road: the binding now remembers ids the host refused an
	// event for and re-sends them, so an id nobody recognises would take a permanent slot in that
	// list and be offered to the host forever. Refusing here keeps everything downstream addressable.
	const int index = paramIndexOf(id);
	if (index < 0 || !std::isfinite(value)) {
		return;
	}
	queue_.push({ UiParamEvent::Kind::Value, id, value });

	// Show the move immediately rather than waiting for the engine to hand it back a block later. The
	// value is sanitised on the way in so the editor never displays a number the engine will refuse —
	// a stepped param drawn between its steps, or one past the end of its range.
	{
		double sanitized = value;
		const ParamDesc& desc = paramDescs()[static_cast<size_t>(index)];
		if (sanitized < desc.min) {
			sanitized = desc.min;
		}
		if (sanitized > desc.max) {
			sanitized = desc.max;
		}
		if ((desc.flags & CLAP_PARAM_IS_STEPPED) != 0) {
			sanitized = std::round(sanitized);
		}
		seenBase_[static_cast<size_t>(index)] = sanitized;
		// Reported on the next refresh, so a widget that only WATCHES this parameter — the filter
		// card's curve watching the cutoff, say — redraws in the same frame as the knob rather than a
		// flush round trip behind it.
		editorTouched_[static_cast<size_t>(index)] = true;
		// The modulated reading is guessed, not known: this writes what the value would be with
		// nothing modulating it. On a parameter a host modulator is moving, the guess is wrong and the
		// editor draws the modulation mark in the wrong place. It survives exactly one refresh() — the
		// next snapshot overwrites both readings with the engine's own — so now that the diff runs on
		// every host callback rather than on stale frames, that is a single frame. Left as it is
		// because the alternative, holding the previous modulated reading, draws a mark disagreeing
		// with a base the user is actively dragging, which reads as the worse kind of wrong.
		seenMod_[static_cast<size_t>(index)] = sanitized;
	}
}

void ParamBridge::endGesture(clap_id id) noexcept {
	if (paramIndexOf(id) < 0) {
		return;
	}
	--openGestures_;
	queue_.push({ UiParamEvent::Kind::GestureEnd, id, 0.0 });
}

double ParamBridge::value(clap_id id) const noexcept {
	const int index = paramIndexOf(id);
	return index < 0 ? 0.0 : seenBase_[static_cast<size_t>(index)];
}

double ParamBridge::modulated(clap_id id) const noexcept {
	const int index = paramIndexOf(id);
	return index < 0 ? 0.0 : seenMod_[static_cast<size_t>(index)];
}

bool ParamBridge::modulatedRight(clap_id id, double& right) const noexcept {
	const int index = paramIndexOf(id);
	if (index < 0 || !seenStereoLive_[static_cast<size_t>(index)]) {
		return false;
	}
	right = seenModR_[static_cast<size_t>(index)];
	return true;
}

std::span<const uint32_t> ParamBridge::refresh() noexcept {
	// THE RECORD CATCHES UP WITH WHAT ITS DEVICES CHANGED THEMSELVES (backlog MX), so the cards draw what plays.
	harvestExtraState();
	changed_.clear();
	for (size_t i = 0; i < kParamCount; ++i) {
		const double base = publishedBase_[i].load(std::memory_order_relaxed);
		const double mod = publishedMod_[i].load(std::memory_order_relaxed);
		const double modR = publishedModR_[i].load(std::memory_order_relaxed);
		const bool stereoLive = publishedStereoLive_[i].load(std::memory_order_relaxed);
		// AGAINST WHAT THE ENGINE LAST SAID, not against what the widgets are showing. Diffing against
		// seenBase_ meant an editor-side setValue — which writes it optimistically so a hand sees its
		// own move at once — read back as a disagreement and got "corrected" to the value the engine
		// had not been told about yet. Every knob snapped to its old position for a frame. See the
		// declaration of lastPublishedBase_.
		const bool engineMoved = !everRefreshed_ || base != lastPublishedBase_[i]
		                         || mod != lastPublishedMod_[i] || modR != lastPublishedModR_[i]
		                         || stereoLive != lastPublishedStereoLive_[i];
		if (engineMoved) {
			// THE ENGINE'S WORD WINS WHEN IT SPEAKS, and it only speaks when the value really moved —
			// so an optimistic reading is overwritten by the echo that confirms it, by automation, or
			// by a preset load, and never by a republication of the value the editor has already moved
			// past.
			lastPublishedBase_[i] = base;
			lastPublishedMod_[i] = mod;
			lastPublishedModR_[i] = modR;
			lastPublishedStereoLive_[i] = stereoLive;
			seenModR_[i] = modR;
			seenStereoLive_[i] = stereoLive;
			seenBase_[i] = base;
			seenMod_[i] = mod;
		}
		if (engineMoved || editorTouched_[i]) {
			editorTouched_[i] = false;
			changed_.push_back(static_cast<uint32_t>(i));
		}
	}
	everRefreshed_ = true;
	return changed_;
}

void ParamBridge::publish(const ParamSet& params) noexcept {
	const std::span<const ParamDesc> descs = paramDescs();
	for (size_t i = 0; i < kParamCount; ++i) {
		const clap_id id = static_cast<clap_id>(descs[i].id);
		publishedBase_[i].store(params.base(id), std::memory_order_relaxed);
		publishedMod_[i].store(params.resolved(id), std::memory_order_relaxed);
		// THE RIGHT EAR, for a `stereo` row only (backlog KU): every other parameter is one value in both ears.
		if (descs[i].stereo) {
			const bool live = params.stereoLive(descs[i].id);
			publishedModR_[i].store(live ? params.resolvedRight(descs[i].id) : params.resolved(id), std::memory_order_relaxed);
			publishedStereoLive_[i].store(live, std::memory_order_relaxed);
		}
	}
}

void ParamBridge::setOrientation(const SliceOrientationPin& pin) noexcept {
	repaintDirty_.store(true, std::memory_order_relaxed);
	restoreOrientation(pin);
	// The pin is patch state, and it never said so until the drift offsets needed the same road.
	stateDirty_.store(true, std::memory_order_relaxed);
}

void ParamBridge::restoreOrientation(const SliceOrientationPin& pin) noexcept {
	repaintDirty_.store(true, std::memory_order_relaxed);
	orientationRotate_.store(pin.generator.rotate, std::memory_order_relaxed);
	// The flag last, so a reader that catches this write mid-flight sees the old turn rather than a
	// half-written new one. Not a fence and not claiming to be: it costs nothing and it is the right
	// order anyway.
	orientationPinned_.store(pin.pinned, std::memory_order_relaxed);
}

SliceOrientationPin ParamBridge::orientation() const noexcept {
	SliceOrientationPin pin;
	pin.pinned = orientationPinned_.load(std::memory_order_relaxed);
	pin.generator.rotate = orientationRotate_.load(std::memory_order_relaxed);
	return pin;
}

void ParamBridge::setFalloffPower(double power) noexcept {
	repaintDirty_.store(true, std::memory_order_relaxed);
	// Guarded rather than trusted, because this arrives from a text field in an environment variable as
	// well as from a slider. A zero exponent makes every weight 1 whatever the distance — a
	// constellation with no geometry left in it — and a negative one inverts it so the FURTHEST sound
	// wins; neither is a question anybody is asking, and both are silent to look at and loud to hear.
	if (!(power > 0.0) || !std::isfinite(power)) {
		return;
	}
	falloffPower_.store(power, std::memory_order_relaxed);
}

void ParamBridge::setBlueShiftScales(double overshoot, double doppler) noexcept {
	repaintDirty_.store(true, std::memory_order_relaxed);
	// Guarded like the falloff, and for the same reason: these arrive from text as well as from a
	// control. Negative would invert the shift so that RECEDING sounds brightened, which is not the
	// feature under a different sign — it is a different feature nobody described.
	if (std::isfinite(overshoot) && overshoot >= 0.0) {
		blueOvershootScale_.store(overshoot, std::memory_order_relaxed);
	}
	if (std::isfinite(doppler) && doppler >= 0.0) {
		blueDopplerScale_.store(doppler, std::memory_order_relaxed);
	}
}

void ParamBridge::setV1Crossfade(bool on) noexcept {
	// THE ONE EXCEPTION TO "EVERY CHANNEL RAISES THE REPAINT BIT", recorded rather than fixed. Its
	// only writer runs once at construction, from an environment variable, before any surface
	// exists — so there is nobody to repaint and flagging it would change nothing. What is worth
	// writing down is that the bridge suite's claim to have ENUMERATED that set has a hole here:
	// the test's list mirrors the implementation's, so a channel missing from both looks complete
	// from either side. A second writer for this would need the flag, and would get no warning.
	v1Crossfade_.store(on, std::memory_order_relaxed);
}

void ParamBridge::beginSoundTravelGesture() noexcept {
	// Nothing to open. The bracket exists so the drag reads the same as a parameter drag from the
	// editor's side, and so the END has somewhere to be — see the header.
}

void ParamBridge::setSoundTravel(size_t slot, AnchorPoint offset) noexcept {
	repaintDirty_.store(true, std::memory_order_relaxed);
	if (slot >= kMaxSounds || !std::isfinite(offset.x) || !std::isfinite(offset.y)) {
		return;
	}
	soundTravel_[slot * 2].store(offset.x, std::memory_order_relaxed);
	soundTravel_[slot * 2 + 1].store(offset.y, std::memory_order_relaxed);
	stateDirty_.store(true, std::memory_order_relaxed);
}

void ParamBridge::setSoundOrbit(size_t slot, PolarOffset orbit) noexcept {
	repaintDirty_.store(true, std::memory_order_relaxed);
	// THE PAIR OR NEITHER, the travel setter's own rule: a direction without its distance is not a
	// place. A negative radius is refused rather than flipped — the control never sends one.
	if (slot >= kMaxSounds || !std::isfinite(orbit.radius) || !std::isfinite(orbit.theta)
	    || orbit.radius < 0.0) {
		return;
	}
	soundOrbit_[slot * 2].store(orbit.radius, std::memory_order_relaxed);
	soundOrbit_[slot * 2 + 1].store(orbit.theta, std::memory_order_relaxed);
	stateDirty_.store(true, std::memory_order_relaxed);
}

void ParamBridge::endSoundTravelGesture() noexcept {
	stateDirty_.store(true, std::memory_order_relaxed);
}

void ParamBridge::setSoundTravel(const SoundTravel& travel) noexcept {
	repaintDirty_.store(true, std::memory_order_relaxed);
	// THE PAIR OR NEITHER, matching the singular overload one function up rather than merely being
	// safe: a point is one fact. Per SLOT, though — one bad drift must not cost another slot its own.
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		const AnchorPoint at = travel.offset[slot];
		const AnchorPoint use =
		    (std::isfinite(at.x) && std::isfinite(at.y)) ? at : AnchorPoint {};
		soundTravel_[slot * 2].store(use.x, std::memory_order_relaxed);
		soundTravel_[slot * 2 + 1].store(use.y, std::memory_order_relaxed);
		const PolarOffset orbit = travel.orbit[slot];
		const PolarOffset turn = (std::isfinite(orbit.radius) && std::isfinite(orbit.theta)
		                          && orbit.radius >= 0.0)
		                             ? orbit
		                             : PolarOffset {};
		soundOrbit_[slot * 2].store(turn.radius, std::memory_order_relaxed);
		soundOrbit_[slot * 2 + 1].store(turn.theta, std::memory_order_relaxed);
	}
	// A patch LOAD is not an edit. Marking dirty here would tell the host the project changed the
	// instant it finished opening it.
}

SoundTravel ParamBridge::soundTravel() const noexcept {
	SoundTravel travel;
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		travel.offset[slot] = { soundTravel_[slot * 2].load(std::memory_order_relaxed),
			                    soundTravel_[slot * 2 + 1].load(std::memory_order_relaxed) };
		travel.orbit[slot] = { soundOrbit_[slot * 2].load(std::memory_order_relaxed),
			                   soundOrbit_[slot * 2 + 1].load(std::memory_order_relaxed) };
	}
	return travel;
}

void ParamBridge::beginSoundOrientationGesture() noexcept {
	// Nothing to open, for the reason the travel bracket states: there is no parameter for a host
	// gesture to be about. The END is what the host needs.
}

void ParamBridge::setSoundOrientation(size_t slot, const SliceOrientationPin& pin) noexcept {
	repaintDirty_.store(true, std::memory_order_relaxed);
	if (slot >= kMaxSounds || !std::isfinite(pin.generator.rotate)) {
		return;
	}
	soundPlane_[slot].store(pin.generator.rotate, std::memory_order_relaxed);
	// The flag last, so a reader catching this mid-write sees the old turn rather than half a new one.
	soundPinned_[slot].store(pin.pinned, std::memory_order_relaxed);
	stateDirty_.store(true, std::memory_order_relaxed);
}

void ParamBridge::endSoundOrientationGesture() noexcept {
	stateDirty_.store(true, std::memory_order_relaxed);
}

void ParamBridge::beginSoundGammaGesture() noexcept {}

void ParamBridge::setSoundGamma(size_t slot, double base) noexcept {
	repaintDirty_.store(true, std::memory_order_relaxed);
	// UNBOUNDED IS NOT THE SAME AS NON-FINITE, and the two sibling channels already draw that line:
	// `setSoundTravel` and `setSoundOrientation` each refuse a non-finite value while accepting any
	// magnitude. This one did not, and three setters written together with two of them guarded is the
	// asymmetry rather than the argument. A stance recall reaches here with whatever a patch put in
	// it — `writeRideAlong` hands the banked `gammaBase` straight over.
	if (slot >= kMaxSounds || !std::isfinite(base)) {
		return;
	}
	soundGamma_[slot].store(base, std::memory_order_relaxed);
	stateDirty_.store(true, std::memory_order_relaxed);
}

void ParamBridge::endSoundGammaGesture() noexcept {
	stateDirty_.store(true, std::memory_order_relaxed);
}

SoundGamma ParamBridge::soundGamma() const noexcept {
	SoundGamma gamma;
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		gamma.base[slot] = soundGamma_[slot].load(std::memory_order_relaxed);
	}
	return gamma;
}

void ParamBridge::restoreSoundGamma(const SoundGamma& gamma) noexcept {
	repaintDirty_.store(true, std::memory_order_relaxed);
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		soundGamma_[slot].store(gamma.base[slot], std::memory_order_relaxed);
	}
	// DELIBERATELY NOT DIRTY. This is a patch arriving, not a player editing — see the header.
}

SoundOrientations ParamBridge::soundOrientations() const noexcept {
	SoundOrientations planes;
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		planes.pin[slot].pinned = soundPinned_[slot].load(std::memory_order_relaxed);
		planes.pin[slot].generator.rotate = soundPlane_[slot].load(std::memory_order_relaxed);
	}
	return planes;
}

void ParamBridge::setSoundOrientations(const SoundOrientations& planes) noexcept {
	repaintDirty_.store(true, std::memory_order_relaxed);
	// THE PLURAL IS THE ROAD A FILE TAKES, and the guard was only on the road a hand takes. A set is
	// substituted per slot rather than refused whole — one bad turn must not cost every other slot
	// its own.
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		const float turn = planes.pin[slot].generator.rotate;
		soundPlane_[slot].store(std::isfinite(turn) ? turn : SliceOrientationPin {}.generator.rotate,
			std::memory_order_relaxed);
		soundPinned_[slot].store(planes.pin[slot].pinned, std::memory_order_relaxed);
	}
	// A patch load is not an edit — the same rule the travel channel and restoreOrientation follow.
}

void ParamBridge::setSoundResidue(size_t slot, const PlaneResidue& banked) noexcept {
	repaintDirty_.store(true, std::memory_order_relaxed);
	if (slot >= kMaxSounds) {
		return;
	}
	// ALL OR NOTHING ON THE VALIDATION, which is the difference between this and the plane channel's
	// per-field guard: a residue is one banked difference spread across 88 coordinates, and storing the
	// finite half of a spoilt fold would leave the sound standing at a point that is neither where it
	// was nor where the turn would have put it.
	for (size_t i = 0; i < kPlaneResidueValues; ++i) {
		if (!std::isfinite(planeResidueValue(banked, i))) {
			return;
		}
	}
	const size_t base = slot * kPlaneResidueValues;
	for (size_t i = 0; i < kPlaneResidueValues; ++i) {
		soundResidue_[base + i].store(planeResidueValue(banked, i), std::memory_order_relaxed);
	}
	stateDirty_.store(true, std::memory_order_relaxed);
}

SoundResidue ParamBridge::soundResidue() const noexcept {
	SoundResidue residue;
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		const size_t base = slot * kPlaneResidueValues;
		for (size_t i = 0; i < kPlaneResidueValues; ++i) {
			setPlaneResidueValue(residue.banked[slot], i,
			                     soundResidue_[base + i].load(std::memory_order_relaxed));
		}
	}
	return residue;
}

void ParamBridge::setSoundResidue(const SoundResidue& residue) noexcept {
	repaintDirty_.store(true, std::memory_order_relaxed);
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		const size_t base = slot * kPlaneResidueValues;
		for (size_t i = 0; i < kPlaneResidueValues; ++i) {
			soundResidue_[base + i].store(planeResidueValue(residue.banked[slot], i),
			                              std::memory_order_relaxed);
		}
	}
	// A patch load is not an edit — the same rule the travel and plane channels follow.
}

void ParamBridge::beginSoundVariationsGesture() noexcept {}

void ParamBridge::setSoundVariations(size_t slot, const SoundVariations::List& list) noexcept {
	repaintDirty_.store(true, std::memory_order_relaxed);
	if (slot >= kMaxSounds) {
		return;
	}
	variations_.sound[slot] = list;
}

void ParamBridge::endSoundVariationsGesture() noexcept {
	// The bracket's whole job on a channel with no parameter under it: tell the host the project
	// changed, so it asks for the patch again. Without it a stored stance is a stance that silently
	// fails to be saved — the hole the drift channel's comment describes, closed the same way.
	stateDirty_.store(true, std::memory_order_relaxed);
}

void ParamBridge::restoreSoundVariations(const SoundVariations& variations) noexcept {
	repaintDirty_.store(true, std::memory_order_relaxed);
	variations_ = variations;
	// A patch load is not an edit — the same rule every channel above follows.
}

void ParamBridge::beginSoundHuesGesture() noexcept {}

void ParamBridge::setSoundHue(size_t slot, uint8_t index) noexcept {
	repaintDirty_.store(true, std::memory_order_relaxed);
	if (slot >= kMaxSounds) {
		return;
	}
	// FOLDED ON THE WAY IN as well as on the way out of a patch file. A caller that computed an index
	// off the end of the palette would otherwise store it and every reader would have to fold it back.
	hues_.index[slot] = static_cast<uint8_t>(index % kMaxSounds);
}

void ParamBridge::endSoundHuesGesture() noexcept {
	// The bracket's whole job on a channel with no parameter under it — see the variations channel
	// above. A colour that moved and was not saved is a colour that moves back when the patch reopens.
	stateDirty_.store(true, std::memory_order_relaxed);
}

void ParamBridge::restoreSoundHues(const SoundHues& hues) noexcept {
	repaintDirty_.store(true, std::memory_order_relaxed);
	hues_ = hues;
	// A patch load is not an edit — the same rule every channel above follows.
}

bool ParamBridge::macroBipolar(size_t index) const noexcept {
	return index < kMacroCount && macroModes_.bipolar[index] != 0;
}

void ParamBridge::setMacroBipolar(size_t index, bool bipolar) noexcept {
	if (index >= kMacroCount) {
		return;
	}
	if (macroBipolar(index) == bipolar) {
		return; // the mode it is in already; not an edit
	}
	macroModes_.bipolar[index] = bipolar ? 1 : 0;
	coreSourceDescs_[index].bipolar = bipolar;
	// AN EDIT ON A CHANNEL WITH NO PARAMETER UNDER IT — the hues' rule: the patch is modified (a mode
	// that moved and was not saved would snap back when the patch reopened) and the editor repaints,
	// because every knob reading this macro's routes draws a different reach now.
	stateDirty_.store(true, std::memory_order_relaxed);
	repaintDirty_.store(true, std::memory_order_relaxed);
}

// THE NAMING RULE IS `device::normalisedMacroName`'s (and the default `device::defaultMacroName`'s) — shared
// with the rack's bridge, so a macro typed "Macro 3" is not a rename in either host.
using device::defaultMacroName;
using device::normalisedMacroName;

void ParamBridge::restoreMacroModes(const MacroModes& modes) {
	// THE NAMES BEFORE, so the listener hears exactly the macros whose effective name moved — a restore
	// of the patch already loaded is a rescan for nothing, and a host asked to rescan all four on every
	// load would be told about three names that are what they were.
	const std::array<std::string, kMacroCount> before = macroModes_.name;
	macroModes_ = modes;
	for (size_t i = 0; i < kMacroCount; ++i) {
		coreSourceDescs_[i].bipolar = modes.bipolar[i] != 0;
		macroModes_.name[i] = normalisedMacroName(i, modes.name[i]);
	}
	repaintDirty_.store(true, std::memory_order_relaxed);
	// A patch load is not an edit.
	for (size_t i = 0; i < kMacroCount; ++i) {
		if (macroModes_.name[i] != before[i]) {
			macroRenameSubscribers_.notify(i);
		}
	}
}

device::MacroType ParamBridge::macroType(size_t index) const noexcept {
	return index < kMacroCount ? macroModes_.type[index] : device::MacroType::Knob;
}

void ParamBridge::setMacroType(size_t index, device::MacroType type) noexcept {
	if (index >= kMacroCount || type >= device::MacroType::kCount) {
		return;
	}
	if (macroModes_.type[index] == type) {
		return; // the type it is already; not an edit
	}
	macroModes_.type[index] = type;
	// setMacroBipolar's rule: the patch is modified and the knob redraws as the control it now is.
	stateDirty_.store(true, std::memory_order_relaxed);
	repaintDirty_.store(true, std::memory_order_relaxed);
}

std::string ParamBridge::macroName(size_t index) const {
	if (index < kMacroCount && !macroModes_.name[index].empty()) {
		return macroModes_.name[index];
	}
	return defaultMacroName(index);
}

bool ParamBridge::macroRenamed(size_t index) const noexcept {
	return index < kMacroCount && !macroModes_.name[index].empty();
}

void ParamBridge::setMacroName(size_t index, std::string_view name) {
	if (index >= kMacroCount) {
		return;
	}
	std::string next = normalisedMacroName(index, name);
	if (next == macroModes_.name[index]) {
		return; // the name it has already — including the default spelled out; not an edit
	}
	macroModes_.name[index] = std::move(next);
	stateDirty_.store(true, std::memory_order_relaxed);
	repaintDirty_.store(true, std::memory_order_relaxed);
	macroRenameSubscribers_.notify(index);
}

SpectralState ParamBridge::spectralState() const noexcept {
	SpectralState state;
	state.on = spectralOn_.load(std::memory_order_relaxed);
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		state.curve[slot] = spectralCurve(slot);
	}
	state.vocodeFreezeLoads = vocodeFreezeLoads_.load(std::memory_order_acquire);
	(void)vocodeFreeze_.read(state.vocodeFreeze.data(), state.vocodeFreeze.size());
	return state;
}

spectral::DrawnCurve ParamBridge::spectralCurve(size_t slot) const noexcept {
	spectral::DrawnCurve curve;
	if (slot >= kMaxSounds) {
		return curve;
	}
	curve.on = spectralCurveOn_[slot].load(std::memory_order_relaxed);
	for (size_t i = 0; i < curve.gain.size(); ++i) {
		curve.gain[i] = spectralCurve_[slot][i].load(std::memory_order_relaxed);
	}
	return curve;
}

// THE DRAWER'S EDITS: setMacroBipolar's rule — an edit on a channel with no parameter under it marks the patch
// modified and repaints the editor; one that changes nothing is not an edit.
void ParamBridge::setSpectralOn(bool on) noexcept {
	if (spectralOn_.exchange(on, std::memory_order_relaxed) == on) {
		return;
	}
	stateDirty_.store(true, std::memory_order_relaxed);
	repaintDirty_.store(true, std::memory_order_relaxed);
	spectralMoved_.store(true, std::memory_order_release);
}

void ParamBridge::setSpectralCurveOn(size_t slot, bool on) noexcept {
	if (slot >= kMaxSounds || spectralCurveOn_[slot].exchange(on, std::memory_order_relaxed) == on) {
		return;
	}
	stateDirty_.store(true, std::memory_order_relaxed);
	repaintDirty_.store(true, std::memory_order_relaxed);
	spectralMoved_.store(true, std::memory_order_release);
}

void ParamBridge::setSpectralCurveBand(size_t slot, size_t band, float gain) noexcept {
	if (slot >= kMaxSounds || band >= spectral::kCurveBands || !std::isfinite(gain)) {
		return;
	}
	gain = std::clamp(gain, 0.0f, kSpectralCurveGainMax); // the loader's bound too (spectral_state.h)
	if (spectralCurve_[slot][band].exchange(gain, std::memory_order_relaxed) == gain) {
		return;
	}
	stateDirty_.store(true, std::memory_order_relaxed);
	repaintDirty_.store(true, std::memory_order_relaxed);
	spectralMoved_.store(true, std::memory_order_release);
}

void ParamBridge::flattenSpectralCurve(size_t slot) noexcept {
	if (slot >= kMaxSounds) {
		return;
	}
	bool moved = false;
	for (std::atomic<float>& band : spectralCurve_[slot]) {
		moved = band.exchange(1.0f, std::memory_order_relaxed) != 1.0f || moved;
	}
	if (moved) {
		stateDirty_.store(true, std::memory_order_relaxed);
		repaintDirty_.store(true, std::memory_order_relaxed);
		spectralMoved_.store(true, std::memory_order_release);
	}
}

void ParamBridge::setSpectralCurve(size_t slot, const spectral::DrawnCurve& given) noexcept {
	// VALIDATED HERE, the road a recall, a clone and an undo all take (stances review, 2026-10-10): a stance's curve comes
	// off the wire through setRideAlongWireValue, which casts any double, so a corrupt record would otherwise hand the voice
	// NaN, negative or huge gains. The same rule as the edit (setSpectralCurveBand) and the loader: NaN to unity, the rest
	// pinned to [0, kSpectralCurveGainMax].
	spectral::DrawnCurve curve = given;
	for (float& g : curve.gain) {
		g = std::isfinite(g) ? std::clamp(g, 0.0f, kSpectralCurveGainMax) : 1.0f;
	}
	if (slot >= kMaxSounds || spectralCurve(slot) == curve) {
		return; // not an edit
	}
	// THE GAINS FIRST, THEN THE SWITCH: a block reading between the two sees the old curve off or the new one's gains under
	// the old switch, never a curve switched on over gains it was not drawn with.
	for (size_t i = 0; i < curve.gain.size(); ++i) {
		spectralCurve_[slot][i].store(curve.gain[i], std::memory_order_relaxed);
	}
	spectralCurveOn_[slot].store(curve.on, std::memory_order_relaxed);
	stateDirty_.store(true, std::memory_order_relaxed);
	repaintDirty_.store(true, std::memory_order_relaxed);
	spectralMoved_.store(true, std::memory_order_release);
}

void ParamBridge::restoreSpectralState(const SpectralState& state) noexcept {
	spectralOn_.store(state.on, std::memory_order_relaxed);
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		spectralCurveOn_[slot].store(state.curve[slot].on, std::memory_order_relaxed);
		for (size_t i = 0; i < spectral::kCurveBands; ++i) {
			spectralCurve_[slot][i].store(state.curve[slot].gain[i], std::memory_order_relaxed);
		}
	}
	vocodeFreeze_.publish(state.vocodeFreeze.data(), state.vocodeFreeze.size());
	vocodeFreezeLoads_.store(state.vocodeFreezeLoads, std::memory_order_release);
	repaintDirty_.store(true, std::memory_order_relaxed);
	spectralMoved_.store(true, std::memory_order_release);
}

void ParamBridge::setTerrainLikes(const TerrainLikes& likes) {
	TerrainLikes capped = likes;
	if (capped.likes.size() > kMaxTerrainLikes) {
		capped.likes.resize(kMaxTerrainLikes);
	}
	if (capped == terrainLikes_) {
		return; // the likes it holds already; not an edit
	}
	terrainLikes_ = std::move(capped);
	// AN EDIT ON A CHANNEL WITH NO PARAMETER UNDER IT — the hues' rule: the patch is modified (a like made and not saved
	// would be gone when the patch reopened) and the editor repaints.
	stateDirty_.store(true, std::memory_order_relaxed);
	repaintDirty_.store(true, std::memory_order_relaxed);
}

void ParamBridge::restoreTerrainLikes(const TerrainLikes& likes) {
	terrainLikes_ = likes;
	if (terrainLikes_.likes.size() > kMaxTerrainLikes) {
		terrainLikes_.likes.resize(kMaxTerrainLikes);
	}
	repaintDirty_.store(true, std::memory_order_relaxed);
	// A patch load is not an edit.
}

// THE PLOT'S TRANSPORT: race-free by construction, and never torn. Every byte the two threads share is an atomic, so
// there is no data race to be undefined behaviour (a classic seqlock's plain memcpy beside a writer is one, retry or no
// retry). The ordering argument, which TSan cannot check here:
//   WRITER:  A := s (relaxed); RELEASE FENCE; every value (relaxed); B := s (release).
//   READER:  b := B (acquire); every value (relaxed); ACQUIRE FENCE; a := A (relaxed); the frame is whole iff a == b.
// If the reader's B load reads s, it synchronises with B's release store, so every value of publish s happens-before
// the reader's loads: each value read is publish s's or a later one's. If any value read was written by a later
// publish t > s, that store is sequenced after t's release fence, and the reader's acquire fence follows the load that
// read it — so t's A := t happens-before the reader's A load, which therefore reads t or later, never s: a != b, and the
// frame is dropped. So a == b proves every value is publish s's. (A spurious drop — the writer has stored A for t but
// no value yet — costs one poll, never a wrong frame.) The serial is the frame's key, never an address (the ABA rule):
// it only climbs, and it moves only when the frame's contents do.
namespace {

void plotToFloats(const SpectralPlotFrame& f, float* out) {
	out[0] = f.f0;
	out[1] = static_cast<float>(f.count);
	out[2] = static_cast<float>(f.stereo);
	out[3] = static_cast<float>(f.playing);
	constexpr size_t n = SpectralPlotFrame::kPartials;
	std::copy_n(f.ratio.data(), n, out + 4);
	std::copy_n(f.terrain.data(), n, out + 4 + n);
	std::copy_n(f.left.data(), n, out + 4 + 2 * n);
	std::copy_n(f.right.data(), n, out + 4 + 3 * n);
}

void plotFromFloats(const float* in, SpectralPlotFrame& f) {
	f.f0 = in[0];
	f.count = static_cast<int32_t>(in[1]);
	f.stereo = static_cast<int32_t>(in[2]);
	f.playing = static_cast<int32_t>(in[3]);
	constexpr size_t n = SpectralPlotFrame::kPartials;
	std::copy_n(in + 4, n, f.ratio.data());
	std::copy_n(in + 4 + n, n, f.terrain.data());
	std::copy_n(in + 4 + 2 * n, n, f.left.data());
	std::copy_n(in + 4 + 3 * n, n, f.right.data());
}

} // namespace

bool ParamBridge::publishPlot(const SpectralPlotFrame& frame) noexcept {
	const bool republish = plotRepublish_.exchange(false, std::memory_order_acq_rel);
	if (!republish && frame.sameAs(plotLast_)) {
		return false;
	}
	plotLast_ = frame;
	std::array<float, kSpectralPlotFloats> flat {};
	plotToFloats(frame, flat.data());
	const uint32_t s = ++plotWriterSerial_;
	plotSerialA_.store(s, std::memory_order_relaxed);
	std::atomic_thread_fence(std::memory_order_release);
	for (size_t i = 0; i < kSpectralPlotFloats; ++i) {
		plot_[i].store(flat[i], std::memory_order_relaxed);
	}
	plotSerialB_.store(s, std::memory_order_release);
	return true;
}

bool ParamBridge::readPlot(SpectralPlotFrame& out, uint32_t& serial) const noexcept {
	const uint32_t b = plotSerialB_.load(std::memory_order_acquire);
	if (b == 0) {
		return false;
	}
	std::array<float, kSpectralPlotFloats> flat {};
	for (size_t i = 0; i < kSpectralPlotFloats; ++i) {
		flat[i] = plot_[i].load(std::memory_order_relaxed);
	}
	std::atomic_thread_fence(std::memory_order_acquire);
	if (plotSerialA_.load(std::memory_order_relaxed) != b) {
		return false;
	}
	plotFromFloats(flat.data(), out);
	serial = b;
	return true;
}

uint32_t ParamBridge::macroBipolarMask() const noexcept {
	uint32_t mask = 0;
	for (size_t i = 0; i < kMacroCount; ++i) {
		mask |= macroModes_.bipolar[i] != 0 ? (1u << i) : 0u;
	}
	return mask;
}

uint32_t ParamBridge::macroRelativeMask() const noexcept {
	uint32_t mask = 0;
	for (size_t i = 0; i < kMacroCount; ++i) {
		mask |= macroModes_.type[i] == device::MacroType::Relative ? (1u << i) : 0u;
	}
	return mask;
}

namespace {

/// A four-byte tag as one word, so a slot's identity is written and read in a single atomic step.
///
/// FOUR SEPARATE CHAR ATOMICS WOULD BE FOUR CHANCES TO STRADDLE — a reader could see two bytes of
/// "flt1" beside two of whatever replaced it and go looking for a device called "fl:2". A tag is one
/// fact and it travels as one word.

} // namespace

size_t ParamBridge::fxDeviceCount() const noexcept {
	// THE RECORD'S LENGTH — the count a copy of the chain would carry, without the copy.
	return chains_.length(kLaneChain);
}

namespace {

/// ONE FIELD OF THE DEVICE AT `position` IN `chain`, or zero for a position nothing holds.
///
/// STRAIGHT OFF THE STATE (kill-lanes step 3b): a chain's rows ARE its order now, so the row at a
/// position is one lookup and a position nothing holds answers with a dead row rather than a no-lane.
float fieldIn(const rack::ChainRecord& record, size_t chain, size_t position, size_t control) noexcept {
	const rack::DeviceState& row = record.state().at(chain, position);
	if (!row.live() || control >= kMaxDeviceFields) {
		return 0.0f;
	}
	return row.field[control];
}

size_t presentIn(const rack::ChainRecord& record, size_t chain, size_t position) noexcept {
	const rack::DeviceState& row = record.state().at(chain, position);
	return row.live() ? std::min(row.fieldsPresent, kMaxDeviceFields) : 0;
}

} // namespace

float ParamBridge::fxFieldAt(size_t slot, size_t control) const noexcept {
	return fieldIn(chains_, kLaneChain, slot, control);
}

float ParamBridge::fxModulatedFieldAt(size_t slot, size_t control) const noexcept {
	const FxTelemetry* t = fxTelemetry_.at(slot);
	if (t == nullptr || control >= kMaxDeviceFields) {
		return 0.0f;
	}
	// PAST THE PUBLISHED WIDTH IS THE RAW FIELD, not zero — the rule `rackModulatedFieldAt` and
	// `RackEngine::modulatedFieldAt` both follow: unmodulated is not silent.
	if (control >= t->modulatedFieldsPresent.load(std::memory_order_relaxed)) {
		return fxFieldAt(slot, control);
	}
	return t->modulated[control].load(std::memory_order_relaxed);
}

namespace {

/// ONE TELEMETRY CELL'S RIGHT EAR — the values whose bit is set, then the mask (backlog KU).
template <class Telemetry>
void storeRightEar(Telemetry* t, const device::ControlMask& mask,
                   const std::array<float, kMaxDeviceFields>& values) noexcept {
	if (t == nullptr) {
		return;
	}
	if (mask.any()) {
		for (size_t c = 0; c < kMaxDeviceFields; ++c) {
			if (mask.test(c)) {
				t->modulatedR[c].store(values[c], std::memory_order_relaxed);
			}
		}
	}
	for (size_t w = 0; w < device::kControlMaskWords; ++w) {
		t->stereoLive[w].store(mask.words[w], std::memory_order_relaxed);
	}
}

template <class Telemetry>
bool loadRightEar(const Telemetry* t, size_t control, float& right) noexcept {
	if (t == nullptr || control >= kMaxDeviceFields || control >= device::kControlMaskBits) {
		return false;
	}
	const uint64_t word = t->stereoLive[control >> 6].load(std::memory_order_relaxed);
	if (((word >> (control & 63)) & 1u) == 0) {
		return false;
	}
	right = t->modulatedR[control].load(std::memory_order_relaxed);
	return true;
}

} // namespace

void ParamBridge::publishFxRightEar(size_t position, const device::ControlMask& mask,
                                    const std::array<float, kMaxDeviceFields>& values) noexcept {
	storeRightEar(fxTelemetry_.at(position), mask, values);
}

void ParamBridge::publishRackRightEar(size_t slot, const device::ControlMask& mask,
                                      const std::array<float, kMaxDeviceFields>& values) noexcept {
	storeRightEar(rackTelemetry_.at(slot), mask, values);
}

bool ParamBridge::fxModulatedFieldRAt(size_t slot, size_t control, float& right) const noexcept {
	return loadRightEar(fxTelemetry_.at(slot), control, right);
}

bool ParamBridge::rackModulatedFieldRAt(size_t slot, size_t control, float& right) const noexcept {
	return loadRightEar(rackTelemetry_.at(slot), control, right);
}

float ParamBridge::rackFieldAt(size_t slot, size_t control) const noexcept {
	return fieldIn(chains_, kStripChain, slot, control);
}

size_t ParamBridge::rackFieldsPresentAt(size_t slot) const noexcept {
	return presentIn(chains_, kStripChain, slot);
}

float ParamBridge::rackModulatedFieldAt(size_t slot, size_t control) const noexcept {
	const RackTelemetry* t = rackTelemetry_.at(slot);
	if (t == nullptr || control >= kMaxDeviceFields) {
		return 0.0f;
	}
	// PAST THE PUBLISHED WIDTH IS THE RAW FIELD, not zero — a control the modulated row does not
	// reach is unmodulated, not silent, which is the same rule `RackEngine::modulatedFieldAt` follows.
	if (control >= t->modulatedFieldsPresent.load(std::memory_order_relaxed)) {
		return rackFieldAt(slot, control);
	}
	return t->modulated[control].load(std::memory_order_relaxed);
}

size_t ParamBridge::fxFieldsPresentAt(size_t slot) const noexcept {
	return presentIn(chains_, kLaneChain, slot);
}

FxChain ParamBridge::fxChain() const noexcept {
	++fxChainCopies_; // UI-side bookkeeping for fxChainCopiesForTest(); see the header.
	// A VIEW OF THE RECORD'S LANE — POSITION-INDEXED and dense since kill-lanes step 3c (it was rows by
	// lane with the order beside them), built through the one mapping `feathers_chains.h` states, which
	// the audio thread's snapshot takes too.
	FxChain chain;
	ModRack unused;
	chain.device.resize(std::max(chain.device.size(), chains_.capacity(kLaneChain)));
	chainsOfState(chains_.state(), &chain, &unused);
	chain.nextInstanceId = ids_.next();
	return chain;
}

size_t ParamBridge::addFxDevice(const char* tag) noexcept {
	FxChain chain = fxChain();
	const size_t slot = feathers::addFxDevice(&chain, tag);
	if (slot == kNoPosition) {
		return kNoPosition; // refused, and nothing was written
	}
	restoreFxChain(chain);
	stateDirty_.store(true, std::memory_order_relaxed); // an edit, unlike a restore
	return slot;
}

size_t ParamBridge::insertFxDeviceBefore(DeviceInstanceId anchor, const char* tag) noexcept {
	FxChain chain = fxChain();
	// **THE ANCHOR GOES STRAIGHT THROUGH — kill-lanes step 3c.** It was `chains_.laneOf(anchor)` while the
	// record had lanes, and for one step a crossing made here: the record answered where the anchor STOOD
	// and `fxLaneAtPosition` turned that into the lane this copy of the chain had minted for it. The view
	// is position-indexed now and the free function takes the DEVICE, so the id the menu holds is the id
	// that arrives. An anchor no longer in the chain appends — the free function's rule, stated there.
	const size_t slot = feathers::insertFxDeviceBefore(&chain, anchor, tag);
	if (slot == kNoPosition) {
		return kNoPosition; // refused, and nothing was written
	}
	// THE WHOLE CHAIN, because every device from `slot` on has a new position and the words here are
	// per POSITION — see the declaration. The routes are untouched: a route names its device's id, and a
	// device keeps its id across a reorder.
	restoreFxChain(chain);
	stateDirty_.store(true, std::memory_order_relaxed);
	repaintDirty_.store(true, std::memory_order_relaxed);
	return slot;
}

// **THE STRUCTURAL VERBS ARE THE RECORD'S, AND THE ROUTES NEED NO HELP.** Each of these used to read the
// whole dense table out (111 KB), hand it to a free function that shifted slot-addressed columns, and
// restore it — because the table was addressed by slot and a removal or a move re-pointed every later
// device's routes. A route names a device's id now (kill-lanes step 2): the record's removal takes the
// device's edges with it, its move deletes what the new order makes backwards, and nothing else moves.

bool ParamBridge::removeDevice(size_t chain, size_t slot) noexcept {
	const DeviceInstanceId device = chains_.deviceAt(chain, slot);
	if (device == kNoInstance || !chains_.remove(device)) {
		return false;
	}
	// ITS HOST SLOTS ARE FREED, anchored ones too (design §6), and
	// each keeps a note of what it held, so the undo below gets the same slots back if nothing took them.
	slots_.pool().freeUnder(std::span<const DeviceInstanceId>(&device, 1));
	publishRoutes();
	publishChains();
	// ITS FOLD AND ITS OUTPUT LABELS STAY HELD, deliberately: an undo of this remove re-inserts the device
	// UNDER THE SAME ID (`insertDevice`), and they come back with it. An id is never reissued, so nothing
	// else can inherit them; a save names only the devices the record holds, so they never reach a patch.
	// AN EDIT, NOT A PATCH ARRIVING: a hand did this, and the patch really has changed.
	stateDirty_.store(true, std::memory_order_relaxed);
	return true;
}

bool ParamBridge::moveDevice(size_t chain, size_t from, size_t to) noexcept {
	if (from >= chains_.length(chain) || to >= chains_.length(chain) || !chains_.move(chain, from, to)) {
		return false;
	}
	publishRoutes();
	publishChains();
	stateDirty_.store(true, std::memory_order_relaxed);
	return true;
}

bool ParamBridge::insertDevice(size_t chain, size_t slot, const DeviceRow& row,
                               std::span<const backplane::Route> routes) noexcept {
	if (slot > chains_.length(chain)) {
		return false;
	}
	rack::DeviceState state = deviceStateOf(row);
	// UNDER THE ID IT HAD — which is what lets its routes name it again as they are. An id is never
	// reissued, so nothing else can be wearing it; a row with none is given one.
	if (state.instanceId == kNoInstance) {
		state.instanceId = ids_.issue();
	}
	// **AND WHEREVER THE RECORD KEEPS IT (kill-lanes step 3b).** This read the row's own `routeLane`, took
	// it if the chain's band held it and it was free, and otherwise walked the band for the lowest free
	// one — a pool allocation written out here, where a caller could get it wrong. `insertRow` places the
	// device itself now; the only thing this says is the POSITION, which is the one fact a chain has.
	if (!chains_.insertRow(chain, slot, state)) {
		return false;
	}
	// AND ITS HOST SLOTS, the undo of `removeDevice`'s free: under the same id, so the notes find it. A
	// device that never had slots (or whose slots were reused meanwhile) restores nothing.
	slots_.pool().restoreUnder(std::span<const DeviceInstanceId>(&state.instanceId, 1));
	// AND ITS ROUTES, THROUGH THE RECORD'S GATE — one whose other end has gone meanwhile is refused, and
	// none is refused for room (backlog OJ).
	for (const backplane::Route& r : routes) {
		if (chains_.setRoute(r.source, r.destination, r.amount)) {
			(void)chains_.setRouteTransfer(r.source, r.destination, r.transfer); // and its curve, which no amount carries
		}
	}
	publishRoutes();
	publishChains();
	stateDirty_.store(true, std::memory_order_relaxed);
	return true;
}

bool ParamBridge::removeFxDevice(size_t slot) noexcept { return removeDevice(kLaneChain, slot); }
bool ParamBridge::moveFxDevice(size_t from, size_t to) noexcept { return moveDevice(kLaneChain, from, to); }
bool ParamBridge::removeRackDevice(size_t slot) noexcept { return removeDevice(kStripChain, slot); }
bool ParamBridge::moveRackDevice(size_t from, size_t to) noexcept { return moveDevice(kStripChain, from, to); }

bool ParamBridge::insertRackDevice(size_t slot, const DeviceRow& row,
                                   std::span<const backplane::Route> routes) noexcept {
	return insertDevice(kStripChain, slot, row, routes);
}

bool ParamBridge::insertFxDevice(size_t slot, const FxDevice& device,
                                 std::span<const backplane::Route> routes) noexcept {
	return insertDevice(kLaneChain, slot, device.row, routes);
}

void ParamBridge::fragmentChanged(const rack::DeviceLabels& labelsBefore) {
	publishRoutes();
	publishChains();
	// AN EDIT, as every structural verb here is: a hand did it.
	stateDirty_.store(true, std::memory_order_relaxed);
	repaintDirty_.store(true, std::memory_order_relaxed);
	rack::DeviceLabels::forEachDifference(labelsBefore, outputLabels_, [this](DeviceInstanceId id, size_t output) {
		outputLabelSubscribers_.notify(id, output);
	});
}

std::vector<backplane::Route> ParamBridge::routesOf(DeviceInstanceId device) const {
	std::vector<backplane::Route> out;
	if (device == kNoInstance) {
		return out; // a host end is not a device; the host's own routes belong to no removal
	}
	chains_.graph().forEachRoute([&](const backplane::Route& r) {
		if (r.source.device == device || r.destination.device == device) {
			out.push_back(r);
		}
	});
	return out;
}

/// **WRITE ONE FLOAT OF A DEVICE'S OPAQUE STATE BLOB. A DIFFERENT ROAD FROM `setFxDeviceControl`, AND
/// DELIBERATELY SO.**
///
/// That function refuses `control >= desc->params.size()`, and its reason is written beside it: a
/// field with no declaration behind it has "no declarations to coerce against, and writing one field
/// of it blind would be editing bytes whose meaning nobody here knows". **A blob float has no
/// `ParamDesc` either** — no range to clamp to, no stepped flag, no default. Widening that guard to
/// `params.size() + extraState` would send unvalidated writes down the same road as validated ones
/// and lose the distinction the guard exists to make.
///
/// So this is its own entry point, bounded by `extraState` rather than by the params, and it coerces
/// NOTHING because there is nothing to coerce to. What it does keep is the finiteness check and the
/// `fieldsPresent` growth, which are about the transport rather than about meaning.
///
/// @param index the offset WITHIN the blob, 0-based — not a row index. The caller says "the third
///        float of this device's state", and where that lands is this function's business.
void ParamBridge::setDeviceState(DeviceInstanceId device, size_t index, float value) noexcept {
	// BY ID, like `setDeviceControl` below — the two write roads agree about their first argument, and
	// since kill-lanes step 3b the record does too: nothing here turns the id into a place.
	if (!chains_.holds(device) || !std::isfinite(value)) {
		return;
	}
	const device::Descriptor* desc = chains_.descriptorOf(device);
	if (desc == nullptr || index >= desc->traits.extraState) {
		return;
	}
	const size_t field = desc->params.size() + index;
	if (field >= kMaxDeviceFields || !chains_.setField(device, field, value)) {
		return;
	}
	publishChains();
	stateDirty_.store(true, std::memory_order_relaxed);
}

void ParamBridge::setDeviceControl(DeviceInstanceId device, size_t control, float value) noexcept {
	// **AN ID IN, AND AN ID ALL THE WAY DOWN (kill-lanes step 2, then 3b).** This was two roads — by fx lane
	// and by mod-rack slot — and before that by fx position, and each change of address was a `size_t`
	// changing meaning under an unchanged signature. Step 2 made the caller's half an id; step 3b made the
	// record's half one too, so the translation this function used to perform no longer exists anywhere.
	if (!chains_.holds(device) || control >= kMaxDeviceFields || !std::isfinite(value)) {
		return;
	}
	// WHAT IS ACTUALLY THERE, asked of the record rather than assumed. A device this build cannot name
	// has no declarations to coerce against, and writing one field of it blind would be editing bytes
	// whose meaning nobody here knows.
	const device::Descriptor* desc = chains_.descriptorOf(device);
	if (desc == nullptr || control >= desc->params.size()) {
		return;
	}

	// COERCED BY THE CONTROL'S OWN DECLARATION — the same rule every other write in this file obeys,
	// and the reason a widget may hand over whatever its drag produced. Clamped to the declared range and
	// never rounded (backlog DV); the cutoff's kind is NOT special-cased here,
	// because this is the knob road and a hand stops at the end of the travel whatever an offset may do.
	// `device::coerceControl` — the one rule every host stores a base by (backlog DV).
	const float coerced = device::coerceControl(desc->params[control], value);

	// THE WIDTH IS WIDENED IF IT HAS TO BE, never narrowed — the record's `setField` rule. A row that
	// reached only part of the way across a device — a patch from a build that declared fewer controls —
	// must not stay short after a hand has edited a field beyond its end, or the edit reads back as absent.
	// THE RISE IS REMEMBERED (`rack::HeldFalls`) so a momentary's release (`releaseDeviceControl`) can wait for it to be read.
	heldFalls_.drop(device, control); // and a plain write supersedes a held release
	const rack::DeviceState& rowBefore = chains_.rowOf(device);
	const float before = rowBefore.live() && control < kMaxDeviceFields ? rowBefore.field[control] : 0.0f;
	const bool recordHadIt = slots_.recordHolds(device, control, coerced);
	if (!chains_.setField(device, control, coerced)) {
		return;
	}
	// **A BOUND CONTROL: THE BUS FIRST, THEN THE RECORD PUBLISHES** (host slots, T6b; `rack_engine.h`'s
	// banner). The bus is what the engine applies over the record at every drain, so a hand's value that
	// reached only the record would be put back to the host's last one by the very next block. The seed's
	// generation is taken as committed, so the mirror does not read the hand back as the host. And the host
	// hears the new value on the slot — inside the gesture `beginControlGesture` opened, so it records.
	// Undo and redo come this way too (`DeviceValueEdit` writes through `setField`).
	// A WRITE OF THE VALUE ALREADY THERE (`recordHadIt`, and a bus the host has not moved) is not an edit and
	// tells the host nothing (T6b review) — `SlotOwner::onHandWrite` has the rule; the rack's `setField` too.
	slots_.onHandWrite(device, control, coerced, recordHadIt);
	publishChains();
	try {
		heldFalls_.wrote(device, control, before, coerced, publishedChains_.sequence());
	}
	catch (...) {
		// Out of memory: this rise is not remembered, so its release writes at once, as before.
	}
	stateDirty_.store(true, std::memory_order_relaxed);
}

void ParamBridge::setFilterSettings(size_t slot, const fx::filter::State& state) noexcept {
	if (slot >= chains_.length(kLaneChain)) {
		return;
	}
	// THROUGH THE SHARED CONVERSION, NOT A LOOP OF MY OWN. device_row.h stamps fieldsPresent from the
	// array's own width, so there is no count here to get wrong — which is the whole reason that
	// helper takes the row by type rather than taking a number beside it.
	const fx::filter::Row row = fx::filter::rowOf(state);
	const DeviceRow carried =
	    toDeviceRow(fx::filter::kTraits.tag, fxIdentityAt(slot), row);
	// INTO THE DEVICE THE SLOT HOLDS — the filter's tag stamped with its numbers, as it always was. The
	// row carries the identity `fxIdentityAt` just read, so the name it is stored under is the one it was
	// built for — and since kill-lanes step 3c that serial is the whole of the identity a row carries.
	storeRowOf(carried.instanceId, carried);
	// An edit, unlike the restore below — a hand moved a filter control, and the patch is modified.
	stateDirty_.store(true, std::memory_order_relaxed);
}

void ParamBridge::restoreFxChain(const FxChain& chain) noexcept {
	// The maximum, for the reason `restoreModRack` states: one counter, two restores. First, so an id a
	// row lacks is issued past the patch's.
	adoptInstanceCounter(chain.nextInstanceId);
	// IN THE CHAIN'S ORDER, WHICH IS NOW THE CHAIN'S STORAGE (kill-lanes step 3c). This walked positions
	// through `FxChain::order` to find the row at each, copied it into a local array and stamped its lane
	// from the order rather than trusting the row's own — the one thing the view's remaining lane readers
	// wanted of it. The subscript IS the position now, so the walk hands over the rows where they lie: no
	// copy, and nothing to stamp. The record takes them in the order they arrive (step 3b).
	std::vector<const DeviceRow*> rows(chain.device.size(), nullptr);
	size_t n = 0;
	for (size_t position = 0; position < fxChainLength(chain) && n < rows.size(); ++position) {
		rows[n] = &chain.device[position].row;
		++n;
	}
	rebuildChain(kLaneChain, std::span<const DeviceRow* const>(rows.data(), n));
	publishChains();
	// Same rule again: opening a patch must not report it as modified.
}

void ParamBridge::setDeviceFolded(DeviceInstanceId device, bool folded) noexcept {
	const uint32_t was = cardViews_.flagsOf(device);
	const uint32_t now = folded ? (was | rack::kViewFolded) : (was & ~rack::kViewFolded);
	if (now == was) {
		return; // a fold that folds nothing is not an edit
	}
	cardViews_.set(device, now);
	stateDirty_.store(true, std::memory_order_relaxed);
}

std::string ParamBridge::outputLabel(DeviceInstanceId device, size_t output) const {
	if (const std::string_view label = outputLabels_.labelOf(device, output); !label.empty()) {
		return std::string(label);
	}
	const std::string_view declared = rack::declaredOutputName(descriptorOf(device), output);
	return declared.empty() ? std::string("Source") : std::string(declared);
}

void ParamBridge::setOutputLabel(DeviceInstanceId device, size_t output, std::string_view label) {
	const device::Descriptor* desc = descriptorOf(device);
	if (desc == nullptr || output >= desc->outputCount()) {
		return; // an output nobody declares has nothing to be called
	}
	std::string next = device::clampMacroName(label);
	if (next == rack::declaredOutputName(desc, output)) {
		next.clear(); // the declared name spelled out is no label
	}
	if (next == outputLabels_.labelOf(device, output)) {
		return; // the label it has already; not an edit
	}
	outputLabels_.set(device, output, next);
	// setMacroName's rule: the patch is modified, and the editor repaints because every surface naming
	// this source in prose now reads it differently.
	stateDirty_.store(true, std::memory_order_relaxed);
	repaintDirty_.store(true, std::memory_order_relaxed);
	outputLabelSubscribers_.notify(device, output);
}

void ParamBridge::setCanvasView(rack::CanvasView view) noexcept {
	if (view == canvasView_) {
		return; // a view that changes nothing is not an edit
	}
	canvasView_ = view;
	stateDirty_.store(true, std::memory_order_relaxed);
}

rack::RackPatch ParamBridge::chainsPatch(bool withSlots) const {
	// NO MACRO VALUES: Feathers' macros are CLAP parameters, saved on the parameter road.
	rack::RackPatch out =
	    rack::patchOfRecord(chains_, &fxBodies_, {}, routeList_, feathersCoreCodec(), &cardViews_, {}, &outputLabels_);
	out.canvas = canvasView_;
	// ONLY WHEN SOMETHING IS MAPPED, so a patch that never exposed a control is byte-for-byte what it was
	// before slots existed; an absent member restores an empty pool, which is the same thing.
	if (withSlots) {
		out.hostSlots = slots_.snapshotForSave();
	}
	return out;
}

uint32_t ParamBridge::serviceSlotRoads() noexcept {
	// NOT MARKED DIRTY when only the mirror moved: the host's automation moved the value, and the host owns
	// that — the rule the core params follow (`consumeParamEvents` never marks dirty). The owner calls the
	// publish BEFORE it hands the audio thread the rebound slots' declarations (the remap window).
	const rack::SlotOwner::Visit visit = slots_.service([this] { publishChains(); });
	// A NEWLY ANCHORED SLOT IS A CHANGE TO THE SAVED POOL (T7 review): the value the host wrote is the host's
	// and marks nothing, but that the slot now stays on the host is the patch's, as releasing it is.
	(void)markedIf(visit.poolChanged);
	return visit.anchored;
}

rack::LoadReport ParamBridge::restoreChains(const rack::RackPatch& patch, SlotsOnLoad slots) {
	rack::LoadReport report = restoreChainRecord(patch);
	// THE POOL FOLLOWS THE CHAINS (see the declaration). A project load puts the patch's pool in place — an
	// absent member, or one `restore` refuses, is an empty pool — and a preset load keeps the project's.
	if (slots == SlotsOnLoad::Restore) {
		// AN ENTRY NAMING A CONTROL NO SLOT MAY CARRY (`ParamDesc::hostSlot`) IS DROPPED, AND SAID.
		const rack::SlotOwner::Refused refused = slots_.restore(patch.present ? patch.hostSlots : std::nullopt);
		for (uint32_t i = 0; i < refused.count; ++i) {
			report.note(rack::LoadReport::Kind::SlotsDropped, "patch", refused.first, "no host slot may carry it");
		}
	}
	else {
		(void)slots_.reconcile();
	}
	return report;
}

rack::LoadReport ParamBridge::restoreChainRecord(const rack::RackPatch& patch) {
	// THE LABELS BEFORE, so the listener hears exactly the outputs whose label moved — `restoreMacroModes`'
	// rule, and told only once the bridge holds the new ones (the tail of each branch below).
	const rack::DeviceLabels labelsBefore = outputLabels_;
	const auto tellLabels = [&] {
		rack::DeviceLabels::forEachDifference(labelsBefore, outputLabels_, [this](DeviceInstanceId id, size_t output) {
			outputLabelSubscribers_.notify(id, output);
		});
	};
	if (!patch.present) {
		// A STREAM FROM BEFORE THIS BLOCK — the chains a fresh instance has, not empty ones (fx_chain.h and
		// mod_rack.h argue it: with no add-a-device affordance an empty chain would be permanent), nothing
		// routed, no bodies. The bodies first, for the order `provisionAndDrain` reads them in.
		fxBodies_.clear();
		cardViews_.clear(); // a fresh instance's cards are shown, whatever the last patch folded
		outputLabels_.clear(); // and its outputs wear their declared names
		restoreFxChain(defaultFxChain());
		restoreModRack(defaultModRack());
		chains_.clearRoutes();
		routeDefaultEnvelope(); // the one route a fresh instance has (backlog EF)
		publishRoutes();
		tellLabels();
		return {};
	}
	// ONE APPLIER FOR EVERY HOST (`rack_state.h`): the record emptied, each device placed by id in its
	// chain's order as far as the chain reaches, the counter reserved past every id the patch names,
	// bodies stored under the id each device ends up wearing, routes resolved by identity through the
	// record's own gate.
	const rack::LoadReport refused =
	    rack::applyPatchToRecord(patch, &chains_, &fxBodies_, &ids_, feathersCoreCodec(), &cardViews_, &outputLabels_);
	// A LOAD GIVES EVERY SELF-CHANGING DEVICE A FRESH BASIS (backlog MX): what was loaded plays, not what it had become.
	rack::stampExtraStateBases(&chains_);
	canvasView_ = patch.canvas;
	publishRoutes();
	publishChains();
	tellLabels();
	// Not an edit: opening a patch must not report it as modified.
	return refused;
}

void ParamBridge::setPreview(size_t slot) noexcept {
	repaintDirty_.store(true, std::memory_order_relaxed);
	// Anything that is not a slot means nobody, so a caller with an out-of-range index turns preview
	// OFF rather than aiming the audio thread at a sound that does not exist.
	preview_.store(slot < kMaxSounds ? slot : Preview::kNone, std::memory_order_relaxed);
	// AND NOTHING ELSE HAPPENS HERE. No queue, no gesture, no dirty flag — listening is not editing,
	// and every one of those would make a preview something a patch or a host could see.
}

void ParamBridge::clearPreview() noexcept {
	repaintDirty_.store(true, std::memory_order_relaxed);
	preview_.store(Preview::kNone, std::memory_order_relaxed);
}

Preview ParamBridge::preview() const noexcept {
	return { preview_.load(std::memory_order_relaxed) };
}

bool ParamBridge::takeStateDirty() noexcept {
	return stateDirty_.exchange(false, std::memory_order_relaxed);
}

bool ParamBridge::takeRepaintDirty() noexcept {
	return repaintDirty_.exchange(false, std::memory_order_relaxed);
}

void ParamBridge::noteRefusedRoutes(const RouteRefusals& refused) {
	// WHOLE, NOT PACKED, AND NOT ATOMIC — the declaration argues both, including the measurement that
	// ruled out `std::atomic<RouteRefusals>` and the thread rule that makes a plain member right.
	loadReport_.mergeRoutes(refused);
}

void ParamBridge::noteLoadReport(const rack::LoadReport& report) {
	if (report.empty()) {
		// A LOAD WITH NOTHING TO SAY CLEARS NOTHING. Two patches in a row, the first with a refusal and
		// the second clean, would otherwise lose the first one's report before an editor opened — and the
		// report is about a patch, not about the last thing that happened.
		return;
	}
	loadReport_ = report;
}

rack::LoadReport ParamBridge::takeLoadReport() {
	// An empty report is "nothing latched", which is what a default-constructed one says — so the clear
	// and the read are the same statement, exactly as the exchange was.
	rack::LoadReport out = std::move(loadReport_);
	loadReport_ = rack::LoadReport {};
	return out;
}

void ParamBridge::publishPuckVelocity(AnchorPoint velocity) noexcept {
	puckVelocityX_.store(velocity.x, std::memory_order_relaxed);
	puckVelocityY_.store(velocity.y, std::memory_order_relaxed);
}

AnchorPoint ParamBridge::puckVelocity() const noexcept {
	return { puckVelocityX_.load(std::memory_order_relaxed),
	     puckVelocityY_.load(std::memory_order_relaxed) };
}

void ParamBridge::publishTravelPulls(const TravelPulls& pulls) noexcept {
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		travelPulls_[slot * 2].store(pulls.radius[slot], std::memory_order_relaxed);
		travelPulls_[slot * 2 + 1].store(pulls.theta[slot], std::memory_order_relaxed);
	}
}

TravelPulls ParamBridge::travelPulls() const noexcept {
	TravelPulls pulls;
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		pulls.radius[slot] = travelPulls_[slot * 2].load(std::memory_order_relaxed);
		pulls.theta[slot] = travelPulls_[slot * 2 + 1].load(std::memory_order_relaxed);
	}
	return pulls;
}

void ParamBridge::publishOwlttMeters(size_t slot, const std::array<float, 3>& grDB,
                                    const std::array<float, 3>& inputLevelDB,
                                    const std::array<float, 3>& outputLevelDB, bool clipping,
                                    const std::array<bool, 3>& bandSaturating, float combinedGrDb,
                                    const std::array<float, 3>* secondChannelGr, float noteHz) noexcept {
	auto* t = fxTelemetry_.at(slot);
	if (t == nullptr) {
		return;
	}
	for (size_t b = 0; b < 3; ++b) {
		t->owlttGrDB[b].store(grDB[b], std::memory_order_relaxed);
		t->owlttInputLevelDB[b].store(inputLevelDB[b], std::memory_order_relaxed);
		t->owlttOutputLevelDB[b].store(outputLevelDB[b], std::memory_order_relaxed);
		t->owlttBandSaturating[b].store(bandSaturating[b], std::memory_order_relaxed);
	}
	t->owlttClipping.store(clipping, std::memory_order_relaxed);
	t->owlttCombinedGrDb.store(std::isfinite(combinedGrDb) ? combinedGrDb : 0.0f, std::memory_order_relaxed);
	for (size_t b = 0; b < 3; ++b) {
		t->owlttGrSecond[b].store(secondChannelGr != nullptr ? (*secondChannelGr)[b] : grDB[b], std::memory_order_relaxed);
	}
	t->owlttNoteHz.store(std::isfinite(noteHz) ? noteHz : 0.0f, std::memory_order_relaxed);

	// NOT `repaintDirty_`, AND THE SIBLING TWENTY LINES UP ALREADY SAID WHY.
	//
	// `publishModulated` refuses the same flag with the same reasoning: *"This is published every
	// block whether anything moved or not, and setting the repaint flag here would mean the editor
	// redraws at block rate forever — which is what the flag exists to avoid."* Two block-rate
	// publishers in one file, one guarded and one not, and the guarded one's comment turned out to be
	// a prediction of what the instrument read.
	//
	// WHAT THE OLD LINE GOT RIGHT AND WHAT IT MISSED. It was written fixing a real bug — the meters
	// did not react to audio through three wiring attempts — and its argument that `tick()` coalesces
	// the flag once a frame is sound. What it missed is that COALESCING BOUNDS THE NOTIFY RATE AND
	// NOT ITS BLAST RADIUS: `stateWatchers.notify()` redraws the whole subtree of every watcher, so
	// one meter update told 108 frames, 85 of which draw no meter.
	//
	// The frame is still asked for, by the widget that draws the reading: `OwlttGraph::update()`, via
	// `FxLane::updateCards()` and the panel hosts, out of `Gui::Impl::tick()`. Content-gated, so a
	// silent plugin asks for nothing at all.
}

ParamBridge::OwlttMeterReading ParamBridge::owlttMeters(size_t slot) const noexcept {
	OwlttMeterReading reading {};
	auto* t = fxTelemetry_.at(slot);
	if (t == nullptr) {
		return reading;
	}
	for (size_t b = 0; b < 3; ++b) {
		reading.grDB[b] = t->owlttGrDB[b].load(std::memory_order_relaxed);
		reading.inputLevelDB[b] = t->owlttInputLevelDB[b].load(std::memory_order_relaxed);
		reading.outputLevelDB[b] = t->owlttOutputLevelDB[b].load(std::memory_order_relaxed);
	}
	reading.clipping = t->owlttClipping.load(std::memory_order_relaxed);
	for (size_t b = 0; b < 3; ++b) {
		reading.bandSaturating[b] = t->owlttBandSaturating[b].load(std::memory_order_relaxed);
	}
	reading.combinedGrDb = t->owlttCombinedGrDb.load(std::memory_order_relaxed);
	for (size_t b = 0; b < 3; ++b) {
		reading.grSecond[b] = t->owlttGrSecond[b].load(std::memory_order_relaxed);
	}
	reading.noteHz = t->owlttNoteHz.load(std::memory_order_relaxed);
	return reading;
}

void ParamBridge::publishAutomodReadings(size_t slot, float phase, float envelope) noexcept {
	auto* t = fxTelemetry_.at(slot);
	if (t == nullptr) {
		return;
	}
	// WRAPPED RATHER THAN CLAMPED, and NaN answered with 0. A phase is a position on a circle, so
	// there is no "past the end" to clamp to — and the device hands over a `uint32_t` turn scaled
	// into 0..1, which cannot leave the range, so this is a guard on the arithmetic rather than a
	// behaviour anything reaches. It is here because the value crosses a thread boundary into a
	// drawing that multiplies it by a width.
	const float safe = std::isfinite(phase) ? (phase - std::floor(phase)) : 0.0f;
	t->automodPhase.store(safe, std::memory_order_relaxed);
	// CLAMPED, NOT WRAPPED, and the difference matters: a level of 1.2 wrapped would read as 0.2 and
	// draw a full meter as nearly empty. See `device_publishers.cpp` for the same pair of guards.
	const float safeEnv = std::isfinite(envelope) ? std::clamp(envelope, 0.0f, 1.0f) : 0.0f;
	t->automodEnvelope.store(safeEnv, std::memory_order_relaxed);

	// NOT `repaintDirty_` — see the declaration, and `publishOwlttMeters` twenty lines up, which
	// refuses the same flag for the same measured reason.
}

void ParamBridge::publishDeviceReadings(size_t slot, const float* values, size_t count) noexcept {
	auto* t = fxTelemetry_.at(slot);
	if (t == nullptr || values == nullptr) {
		return;
	}
	const size_t take = std::min(count, kMaxDeviceReadings);
	for (size_t i = 0; i < take; ++i) {
		// **EVERY VALUE MADE FINITE BEFORE IT CROSSES, and NOT clamped to a range.** The road is
		// generic, so it has no idea what any of these numbers MEAN — a loudness is negative, a
		// correlation is signed, a peak is not bounded above by one — and a guard that clamped would
		// be inventing a range for a number it cannot name. NaN is the one thing that is wrong
		// whatever the number is, because it draws as whatever the card's arithmetic does with one.
		// `device_publishers.cpp` makes the same split for the same reason.
		const float v = values[i];
		t->readings[i].store(std::isfinite(v) ? v : 0.0f, std::memory_order_relaxed);
	}
	// **THE COUNT IS STORED LAST**, so a reader that sees a count has values behind it rather than a
	// count promising values still being written. It is not a lock and does not pretend to be; it is
	// the cheap ordering that makes the common case right.
	t->readingCount.store(static_cast<uint32_t>(take), std::memory_order_relaxed);
}

size_t ParamBridge::deviceReadingCount(size_t slot) const noexcept {
	auto* t = fxTelemetry_.at(slot);
	if (t == nullptr) {
		return 0;
	}
	return t->readingCount.load(std::memory_order_relaxed);
}

float ParamBridge::deviceReading(size_t slot, size_t index) const noexcept {
	auto* t = fxTelemetry_.at(slot);
	if (t == nullptr || index >= kMaxDeviceReadings) {
		return 0.0f;
	}
	return t->readings[index].load(std::memory_order_relaxed);
}

float ParamBridge::automodPhase(size_t slot) const noexcept {
	auto* t = fxTelemetry_.at(slot);
	if (t == nullptr) {
		return 0.0f;
	}
	return t->automodPhase.load(std::memory_order_relaxed);
}

float ParamBridge::automodEnvelope(size_t slot) const noexcept {
	auto* t = fxTelemetry_.at(slot);
	if (t == nullptr) {
		return 0.0f;
	}
	return t->automodEnvelope.load(std::memory_order_relaxed);
}

void ParamBridge::publishSoundWeights(const SoundWeights& weights) noexcept {
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		soundWeight_[slot].store(weights.weight[slot], std::memory_order_relaxed);
	}
	// THE MASK LAST. A reader that catches this mid-write should see the shares it is about to have or
	// the shares it just had — never a slot lit with a share from before it was switched on.
	soundWeightLive_.store(weights.live, std::memory_order_relaxed);
}

SoundWeights ParamBridge::soundWeights() const noexcept {
	SoundWeights out;
	// AND THE MASK FIRST on the way back, which is the other half of the same argument.
	out.live = soundWeightLive_.load(std::memory_order_relaxed);
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		out.weight[slot] = soundWeight_[slot].load(std::memory_order_relaxed);
	}
	return out;
}

AuditionTuning ParamBridge::tuning() const noexcept {
	AuditionTuning t;
	t.orientation = orientation();
	t.falloffPower = falloffPower_.load(std::memory_order_relaxed);
	t.blueOvershootScale = blueOvershootScale_.load(std::memory_order_relaxed);
	t.blueDopplerScale = blueDopplerScale_.load(std::memory_order_relaxed);
	t.v1Crossfade = v1Crossfade_.load(std::memory_order_relaxed);
	return t;
}

void ParamBridge::beginRouteGesture() noexcept {}

void ParamBridge::setRouteAmount(backplane::Source source, backplane::Destination destination,
                                 float amount) noexcept {
	repaintDirty_.store(true, std::memory_order_relaxed);
	// WHETHER THE PAIR IS ALLOWED AT ALL, asked of the one authority rather than restated here. This is
	// the road every editor gesture takes, so a refusal here is a route that cannot be MADE, rather than
	// an amount that gets quietly zeroed somewhere downstream.
	// A ROUTE INTO A SOUND'S IDENTITY (backlog PQ) is made only from a Relative macro — but REMOVING one is always
	// allowed, or a macro switched back to Absolute would leave a row nobody could take away.
	if (!chains_.mayRoute(source, destination) || !std::isfinite(amount)
	    || (amount != 0.0f && !mayReachEditOnly(source, destination))) {
		return;
	}
	const float clamped = std::clamp(amount, -kMaxRouteAmount, kMaxRouteAmount);
	stateDirty_.store(true, std::memory_order_relaxed);

	const bool present = chains_.routeAmount(source, destination) != 0.0f;
	if (clamped == 0.0f && !present) {
		// **ZERO IS NOT A ROUTE** — nothing there to take back, so nothing changes and nothing publishes.
		return;
	}
	// **INTO THE RECORD — the one store of Feathers' routes (chain-as-a-device 5b, C5)** — beside the
	// devices they join, so a removal takes its device's routes and a reorder deletes the ones it makes
	// backwards by the graph's own rules. The record gates again (a live source upstream of a live
	// destination); the two gates are one rule over one structure, and a case holds them together.
	const uint64_t orderBefore = chains_.orderRevision();
	if (!chains_.setRoute(source, destination, clamped)) {
		return;
	}
	// **A ROUTE IS A PRECEDENCE, SO IT CAN REORDER THE ENGINE (kill-lanes 4c).** The record's state carries
	// the graph's evaluation order and the converger follows it; a route that moved a device in that order
	// is published like a move, so from the next drain the source runs before the control that reads it.
	if (chains_.orderRevision() != orderBefore) {
		publishChains();
	}
	// **THE PUBLISH MOVES THE GENERATION**, so the engine copies this on its next block and skips
	// every block after. See routesGeneration().
	publishRoutes();
}

void ParamBridge::endRouteGesture() noexcept {
	stateDirty_.store(true, std::memory_order_relaxed);
}

void ParamBridge::publishRoutes() noexcept {
	// THE RECORD'S ROUTES BY IDENTITY — `Graph::publishRouteList`, the same publish the rack's record takes.
	// The engine compiles them into its own coordinates (`ParamSet::adoptRoutes`, kill-lanes step 1).
	chains_.publishRoutes(routeList_);
}

bool ParamBridge::transferAllowedFor(backplane::Source source, backplane::Transfer t) const noexcept {
	if (!device::transferValid(t)) {
		return false;
	}
	// THE STACK INDEX IS LINEAR BY CONSTRUCTION: its cell reads 0 to every resolve, and the synth sums the
	// amounts routed from it and gives part k `amount × index[k]` itself — a curve here would be drawn and
	// never heard, so it is not offered.
	const bool stackIndex = !source.isDevice() && !source.isOuter() && source.index == kStackIndexSource;
	return t == backplane::Transfer::Lin || !stackIndex;
}

bool ParamBridge::setRouteTransfer(backplane::Source source, backplane::Destination destination,
                                   backplane::Transfer t) noexcept {
	if (!transferAllowedFor(source, t) || chains_.routeAmount(source, destination) == 0.0f) {
		return false;
	}
	if (chains_.routeTransfer(source, destination) == t) {
		return true; // nothing moved, so nothing publishes
	}
	if (!chains_.setRouteTransfer(source, destination, t)) {
		return false;
	}
	repaintDirty_.store(true, std::memory_order_relaxed);
	stateDirty_.store(true, std::memory_order_relaxed);
	// NOT A PRECEDENCE, so no chains publish — only the routes, which carry the curve to the engine.
	publishRoutes();
	return true;
}

backplane::Transfer ParamBridge::innerRouteTransfer(backplane::Source source, DeviceInstanceId chain,
                                                    backplane::Destination inner) const {
	if (chain == kNoInstance) {
		return backplane::Transfer::Lin;
	}
	return backplane::innerRouteTransferOf(rack::containerTableFor(chain, descriptorOf(chain)), fxBodies_.surfaceOf(chain),
	                                       source, inner);
}

bool ParamBridge::setInnerRouteTransfer(backplane::Source source, DeviceInstanceId chain, backplane::Destination inner,
                                        backplane::Transfer t) {
	if (chain == kNoInstance || !transferAllowedFor(source, t)) {
		return false;
	}
	if (!backplane::setInnerRouteTransferOf(rack::containerTableFor(chain, descriptorOf(chain)), fxBodies_.surfaceOf(chain),
	                                        source, inner, t)) {
		return false;
	}
	repaintDirty_.store(true, std::memory_order_relaxed);
	stateDirty_.store(true, std::memory_order_relaxed);
	return true;
}

float ParamBridge::routeAmount(backplane::Source source, backplane::Destination destination) const noexcept {
	// THE RECORD, WHICH IS WHAT WAS PUBLISHED — this thread's own, so there is nothing to straddle.
	return chains_.routeAmount(source, destination);
}

bool ParamBridge::routedElsewhere(backplane::Destination destination, backplane::Source except) const noexcept {
	return chains_.graph().routedElsewhere(destination, except);
}

void ParamBridge::loadRoutesForTest(std::span<const backplane::Route> routes) noexcept {
	chains_.clearRoutes();
	for (const backplane::Route& r : routes) {
		if (chains_.setRoute(r.source, r.destination, r.amount)) {
			(void)chains_.setRouteTransfer(r.source, r.destination, r.transfer); // and its curve, which no amount carries
		}
	}
	publishRoutes();
	publishChains(); // the routes are precedences; the order they give travels with the chains (4c)
	repaintDirty_.store(true, std::memory_order_relaxed);
	// NOT DIRTY: a load, not a hand.
}

void ParamBridge::publishValuesForTest(std::span<const double> base,
                                std::span<const double> modulated) noexcept {
	const size_t count = std::min({ base.size(), modulated.size(), kParamCount });
	for (size_t i = 0; i < count; ++i) {
		publishedBase_[i].store(base[i], std::memory_order_relaxed);
		publishedMod_[i].store(modulated[i], std::memory_order_relaxed);
	}
}


void ParamBridge::routeDefaultEnvelope() {
	// **THE DEFAULT PATCH'S ONE ROUTE** (backlog EF): the strip's envelope — the device a fresh instance has, under the id it always
	// wears — into Voice Level at full amount, so with its base at zero the envelope IS each note's level
	// (Voice Level since 2026-10-03; the Level fader has no default route).
	// Through the record's gate like any route; a modulator in the strip is upstream of the host's core,
	// so the gate admits it, and a case pins that it did.
	const int level = modDestinationIndexOf(ParamId::VoiceLevel);
	if (level < 0) {
		return;
	}
	(void)chains_.setRoute(backplane::Source::output(kDefaultRackInstance, 0),
	                       backplane::Destination::core(static_cast<size_t>(level)), 1.0f);
}

void ParamBridge::publishMasterMeter(const MasterMeterReadings& readings) noexcept {
	// FINITE BEFORE IT CROSSES — `publishFxDrivePeak`'s reason: a NaN on the far side draws nowhere.
	if (std::isfinite(readings.momentaryLufs)) {
		masterMomentary_.store(readings.momentaryLufs, std::memory_order_relaxed);
	}
	if (std::isfinite(readings.rawPeak)) {
		masterRawPeak_.store(readings.rawPeak, std::memory_order_relaxed);
	}
	if (std::isfinite(readings.clipAge)) {
		masterClipAge_.store(readings.clipAge, std::memory_order_relaxed);
	}
}

MasterMeterReadings ParamBridge::masterMeter() const noexcept {
	return { masterMomentary_.load(std::memory_order_relaxed), masterRawPeak_.load(std::memory_order_relaxed),
		     masterClipAge_.load(std::memory_order_relaxed) };
}

void ParamBridge::publishFxDrivePeak(size_t slot, float peak) noexcept {
	auto* t = fxTelemetry_.at(slot);
	if (t == nullptr) {
		return;
	}
	// FINITE BEFORE IT CROSSES, for `publishRackPlayhead`'s reason: the value is multiplied by a width
	// on the far side, and a NaN there draws an element nowhere at all rather than wrongly. A device
	// cannot produce one from `fabs` of a sample, so this guards the arithmetic and not the source.
	//
	// **AND THE SENTINEL SURVIVES THE CLAMP, WHICH IT DID NOT** (found 2026-09-11 by the case that first
	// asked this road what it declines). `std::clamp(peak, 0.0f, 1.0f)` turned every "nobody meters this
	// device" (-1.0f) into 0.0f — including the array this object fills at construction and the
	// `unmetered` row the synth sends for every slot it does not meter — so `fxDrivePeak` could never
	// decline and EVERY fx device answered "driven, at zero" from the first block. That is precisely the
	// failure the bool road exists to prevent: a card with no meter drew one pinned cold, and the shaper's
	// meter could never say "not published". A negative
	// is the message, not a value: it crosses as itself.
	//
	// A NaN NOW DECLINES RATHER THAN READING AS SILENCE, which is a deliberate change of the guard's
	// answer: a device that produced one has published nothing meaningful, and "no meter" is the honest
	// picture of that where "driven by silence" is a made-up one.
	const float clean = !std::isfinite(peak) ? -1.0f : peak < 0.0f ? -1.0f : std::clamp(peak, 0.0f, 1.0f);
	t->drivePeak.store(clean, std::memory_order_relaxed);
}

void ParamBridge::publishFxOutputPeaks(const FxDrivePeaks& peaks) noexcept {
	for (size_t slot = 0; slot < peaks.size(); ++slot) {
		publishFxOutputPeak(slot, peaks[slot]);
	}
}

void ParamBridge::publishFxOutputPeak(size_t slot, float peak) noexcept {
	auto* t = fxTelemetry_.at(slot);
	if (t == nullptr) {
		return;
	}
	// `publishFxDrivePeak`'s guard WITHOUT ITS CLAMP: a NaN or a negative crosses as the sentinel, and a peak over
	// full scale crosses as itself — the amount over 0 dBFS is what the card draws in the clip colour.
	t->outputPeak.store(!std::isfinite(peak) || peak < 0.0f ? -1.0f : peak, std::memory_order_relaxed);
}

bool ParamBridge::fxOutputPeak(size_t slot, float& peak) const noexcept {
	auto* t = fxTelemetry_.at(slot);
	if (t == nullptr) {
		return false;
	}
	const float stored = t->outputPeak.load(std::memory_order_relaxed);
	if (stored < 0.0f) {
		return false;
	}
	peak = stored;
	return true;
}

bool ParamBridge::fxDrivePeak(size_t slot, float& peak) const noexcept {
	auto* t = fxTelemetry_.at(slot);
	if (t == nullptr) {
		return false;
	}
	const float stored = t->drivePeak.load(std::memory_order_relaxed);
	// **THE SENTINEL, WHICH IS WHY THIS RETURNS A BOOL AND NOT A FLOAT.** A slot nobody publishes for
	// holds a negative, and answering 0.0f for it would make an unmetered device indistinguishable
	// from a silent one — the failure `DeviceHost::playheadAt` documents and declines the same way.
	if (stored < 0.0f) {
		return false;
	}
	peak = stored;
	return true;
}
} // namespace feathers

namespace feathers {

bool ParamBridge::reserveChain(size_t chain, size_t devices) {
	if (devices <= chains_.capacity(chain)) {
		return true;
	}
	const bool grown = chains_.growChain(chain, devices);
	publishChains();
	return grown;
}

} // namespace feathers

namespace feathers {

void ParamBridge::setRouteGrowthListener(std::function<void(size_t capacity)> listener) {
	routeGrowth_ = std::move(listener);
	if (routeGrowth_) {
		routeGrowth_(routeList_.capacity()); // level with the list now
	}
}

} // namespace feathers

namespace feathers {

void ParamBridge::publishLikeWarp(size_t slot, uint8_t engine, const LikeWarp& warp) {
	if (slot >= kMaxSounds) {
		return;
	}
	if (likeMain_.engine[slot] == engine && likeMain_.warps[slot] == warp) {
		return;
	}
	likeMain_.engine[slot] = engine;
	likeMain_.warps[slot] = warp;
	++likeMain_.serial;
	// THE BACK SET, its warps copied into its own storage and no owner carried: the audio thread reads this one.
	LikeWarpSet& back = likeSets_[static_cast<size_t>(likeBack_)];
	back.serial = likeMain_.serial;
	back.engine = likeMain_.engine;
	for (size_t s = 0; s < kMaxSounds; ++s) {
		assignLikeWarpInto(back.warps[s], likeMain_.warps[s], (*likeStores_)[static_cast<size_t>(likeBack_)][s]);
	}
	// THE SWAP: the back set becomes the mailbox, marked fresh (4); whatever was in the mailbox becomes the next back.
	likeBack_ = likeMailbox_.exchange(likeBack_ | 4, std::memory_order_acq_rel) & 3;
}

const ParamBridge::LikeWarpSet& ParamBridge::takeLikeWarps() noexcept {
	if ((likeMailbox_.load(std::memory_order_relaxed) & 4) != 0) {
		likeFront_ = likeMailbox_.exchange(likeFront_, std::memory_order_acq_rel) & 3;
	}
	return likeSets_[static_cast<size_t>(likeFront_)];
}

} // namespace feathers

namespace feathers {

void ParamBridge::publishPadHits(const PadHits& hits) noexcept {
	for (size_t pad = 0; pad < kMaxSounds; ++pad) {
		padHitCount_[pad].store(hits.count[pad], std::memory_order_relaxed);
		if (std::isfinite(hits.velocity[pad])) {
			padHitVelocity_[pad].store(hits.velocity[pad], std::memory_order_relaxed);
		}
	}
}

PadHits ParamBridge::padHits() const noexcept {
	PadHits out;
	for (size_t pad = 0; pad < kMaxSounds; ++pad) {
		out.count[pad] = padHitCount_[pad].load(std::memory_order_relaxed);
		out.velocity[pad] = padHitVelocity_[pad].load(std::memory_order_relaxed);
	}
	return out;
}

} // namespace feathers

namespace feathers {

void ParamBridge::landHeldControls(uint32_t adopted) noexcept {
	// LIVE FROM THE FIRST VISIT ON: a bridge no host services (an editor fixture with no synth) never holds, or a quick
	// fall would wait for an adoption that never comes.
	heldFallsLive_ = true;
	chainsAdopted_ = adopted;
	try {
		heldFalls_.land(adopted, [this](DeviceInstanceId d, size_t c, float v) { setDeviceControl(d, c, v); });
	}
	catch (...) {
		// Out of memory on the main thread: the next visit lands them.
	}
}

} // namespace feathers

namespace feathers {

void ParamBridge::releaseDeviceControl(DeviceInstanceId device, size_t control, float value) noexcept {
	// A MOMENTARY'S RELEASE (`rack::HeldFalls`; 2026-11-07, Pattern's Go pressed and released between two blocks was LOST):
	// a fall whose rise the synth has not adopted waits, the record keeping the field up; `landHeldControls` lands it.
	const device::Descriptor* desc = chains_.descriptorOf(device);
	if (heldFallsLive_ && desc != nullptr && control < desc->params.size() && std::isfinite(value)) {
		const float coerced = device::coerceControl(desc->params[control], value);
		const rack::DeviceState& row = chains_.rowOf(device);
		const float before = row.live() && control < kMaxDeviceFields ? row.field[control] : 0.0f;
		try {
			if (heldFalls_.hold(device, control, before, coerced, chainsAdopted_)) {
				return;
			}
		}
		catch (...) {
			// Out of memory: written at once, as before this road existed.
		}
	}
	setDeviceControl(device, control, value);
}

} // namespace feathers
