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

#include "rack/site_chains.h"
#include "rack/site_routes.h"

#include "backplane/graph.h"
#include "rack/inner_chain_lanes.h"
#include "rack/rack_axes.h"
#include "rack/rack_bridge.h"

namespace feathers::rack {

SiteChains::SiteChains(DeviceInstanceId owner, const device::Catalogue& catalogue, double sampleRate, int maxFrames)
    : owner_(owner), ids_(std::make_unique<backplane::InstanceCounter>()), pre_(kRackAxes, ids_.get()),
      post_(kRackAxes, ids_.get()) {
	for (InnerChain* lane : { &pre_, &post_ }) {
		lane->adopt(catalogue);
		lane->setVoices(0); // BUS ONLY — see the banner
	}
	pre_.engine().setCostLane("pre"); // the spike recorder's paths: `FLT1/pre/DELA` (backlog SR)
	post_.engine().setCostLane("post");
	// A ROW OF THIS SITE'S TABLE THAT RE-EXPORTS A PRE HOLDER'S ROW KEEPS IT (NZ, any depth).
	if (pre_.bridge() != nullptr) {
		pre_.bridge()->setOwnerExports(&exports_);
	}
	if (sampleRate > 0.0) {
		prepare(sampleRate, maxFrames);
	}
}

SiteChains::~SiteChains() = default;

void SiteChains::prepare(double sampleRate, int maxFrames) {
	sampleRate_ = sampleRate;
	maxFrames_ = maxFrames;
	pre_.prepare(sampleRate, maxFrames);
	post_.prepare(sampleRate, maxFrames);
	// SIZED TO WHAT THE SITE DECLARES NOW (the cheap win's rule): a later rise restarts the host, which prepares again.
	dryDelay_.prepare(sampleRate, latencySamples(sampleRate));
}

namespace {

/// **A SITE BODY THAT EXPORTS** (backlog NZ): this magic, the bundle's length, the bundle, then the export table. A
/// site that exports nothing writes the bare bundle, so every body written before the table existed reads as it did.
constexpr uint32_t kSiteMagic = 0x31544953u; // "SIT1": a row's index u16
/// "SIT2" (NZ, any depth, 2026-10-31): the index u32, whole, so a row may name a PRE holder's export. SIT1 still reads.
constexpr uint32_t kSiteMagic2 = 0x32544953u;

void put32(std::vector<uint8_t>& out, uint32_t v) {
	for (int i = 0; i < 4; ++i) {
		out.push_back(static_cast<uint8_t>(v >> (8 * i)));
	}
}
void put16(std::vector<uint8_t>& out, uint16_t v) {
	out.push_back(static_cast<uint8_t>(v));
	out.push_back(static_cast<uint8_t>(v >> 8));
}
uint32_t get32(const uint8_t* p) {
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16)
	       | (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

/// **PRE'S OWN MACRO IN A ROW** — PRE's core sources are the site's macros (decision O7), and a core source's id is
/// `kNoInstance`, which as a row would read as empty. So a macro is filed under an id no counter issues.
constexpr DeviceInstanceId kMacroRowId = static_cast<DeviceInstanceId>(-1);
constexpr size_t kRowBytes = sizeof(uint32_t) + 2 * sizeof(uint16_t);
constexpr size_t kRowBytes2 = 2 * sizeof(uint32_t) + sizeof(uint16_t);

} // namespace

bool SiteChains::saveState(const device::ByteSink& sink) const {
	auto* self = const_cast<SiteChains*>(this); // the bundle walks lanes; saving changes neither
	const auto all = self->lanes();
	std::vector<RackPatch::Export> minted;
	exports_.save(&minted);
	const size_t rows = minted.size();
	if (rows == 0) {
		return writeChainBundle(all, sink);
	}
	std::vector<uint8_t> bundle;
	const device::ByteSink into { &bundle, [](void* ctx, const uint8_t* data, size_t n) {
		                             auto* v = static_cast<std::vector<uint8_t>*>(ctx);
		                             v->insert(v->end(), data, data + n);
	                             } };
	if (!writeChainBundle(all, into)) {
		return false;
	}
	// IN THREE PIECES, so the bundle is handed on as it was written rather than copied into a frame.
	std::vector<uint8_t> head;
	put32(head, kSiteMagic2);
	put32(head, static_cast<uint32_t>(bundle.size()));
	std::vector<uint8_t> table;
	put16(table, static_cast<uint16_t>(rows));
	for (const RackPatch::Export& e : minted) {
		put32(table, e.device == kNoInstance ? static_cast<uint32_t>(kMacroRowId) : e.device); // the site's own spelling
		put32(table, e.index);
		put16(table, static_cast<uint16_t>(e.row));
	}
	sink(head.data(), head.size());
	sink(bundle.data(), bundle.size());
	sink(table.data(), table.size());
	return true;
}

bool SiteChains::readBody(const uint8_t* data, size_t bytes, bool apply) {
	const auto all = lanes();
	std::vector<RackPatch::Export> rows;
	const uint8_t* bundle = data;
	size_t bundleBytes = bytes;
	if (bytes >= 2 * sizeof(uint32_t) && (get32(data) == kSiteMagic || get32(data) == kSiteMagic2)) {
		const bool wide = get32(data) == kSiteMagic2;
		const size_t rowBytes = wide ? kRowBytes2 : kRowBytes;
		bundleBytes = get32(data + 4);
		const size_t tableAt = 2 * sizeof(uint32_t) + bundleBytes;
		if (bundleBytes > bytes - 2 * sizeof(uint32_t) || bytes - tableAt < sizeof(uint16_t)) {
			return false;
		}
		bundle = data + 2 * sizeof(uint32_t);
		const size_t count = get16(data + tableAt);
		// THE WHOLE FRAME AND NOTHING BUT, as the bundle's own reader says.
		if (bytes - tableAt - sizeof(uint16_t) != count * rowBytes) {
			return false;
		}
		for (size_t i = 0; i < count; ++i) {
			const uint8_t* p = data + tableAt + sizeof(uint16_t) + i * rowBytes;
			const DeviceInstanceId id = get32(p);
			const uint32_t index = wide ? get32(p + 4) : get16(p + 4);
			const size_t row = get16(p + (wide ? 8 : 6));
			if (id == kNoInstance || row >= device::kSiteExportRows) {
				return false;
			}
			rows.push_back({ static_cast<uint32_t>(row), id == kMacroRowId ? kNoInstance : id, index });
		}
	}
	const bool ok = apply ? applyChainBundle(all, bundle, bundleBytes)
	                      : (bundleBytes == 0 || readChainBundle(all, bundle, bundleBytes));
	if (!ok) {
		return false;
	}
	exports_.load(rows);
	return true;
}

bool SiteChains::loadState(const uint8_t* data, size_t bytes) {
	if (bytes == 0) {
		return true; // AN EMPTY BODY IS AN EMPTY SITE, which is what a site is when it is built
	}
	return readBody(data, bytes, false);
}

bool SiteChains::applyState(const uint8_t* data, size_t bytes) { return readBody(data, bytes, true); }

void SiteChains::setOuterGate(const device::OuterGate& gate) {
	outerGate_ = gate;
	// **POST READS THIS SITE'S OWN PRE EXPORTS, AND IS THE ONLY ONE WHO DOES** (backlog NZ). Said here rather than by the
	// host's `sourceUpstreamOf` answering "the holder asks about its own export": since a Rack or a Chain may hold a site
	// (D3 lifted, 2026-10-29), the holder's own chains ask with the same id, and they must not reach PRE (PRE
	// reaches only the container's own controls). A container's own export is no source for its POST either.
	device::OuterGate post = gate;
	if (gate.upstream != nullptr) {
		post.host = this;
		post.upstream = [](const void* host, DeviceInstanceId self, backplane::Source s) {
			const auto* site = static_cast<const SiteChains*>(host);
			if (s.isExport() && s.device == site->owner_) {
				return s.isSiteExport() && self == site->owner_ && s.exportRow() < device::kSiteExportRows;
			}
			return site->outerGate_.upstream(site->outerGate_.host, self, s);
		};
	}
	post_.setOuterGate(post);
	device::OuterGate pre = gate;
	if (gate.upstream != nullptr) {
		pre.host = this;
		pre.upstream = [](const void* host, DeviceInstanceId self, backplane::Source s) {
			const auto* site = static_cast<const SiteChains*>(host);
			// Only this site's OWN exports are refused here; another holder's, placed before this one, reaches PRE as it
			// reaches any container after it (NZ rung 5).
			return !(s.isExport() && s.device == site->owner_) && site->outerGate_.upstream(site->outerGate_.host, self, s);
		};
	}
	pre_.setOuterGate(pre);
}

void SiteChains::setOuterSources(const device::OuterSources& outer) {
	pre_.setOuterSources(outer);
	post_.setOuterSources(outer);
}

size_t SiteChains::exportRowFor(backplane::Source inner) {
	if (inner.isOuter()) {
		return kNoRow;
	}
	const RackBridge* b = pre_.bridge();
	if (b == nullptr) {
		return kNoRow;
	}
	// A HOLDER IN PRE, RE-EXPORTED ONE HOP OUT (NZ, any depth) — never this site's own exports, which PRE cannot read.
	if (inner.isExport()) {
		if (inner.device == owner_ || !b->namesLiveExport(inner)) {
			return kNoRow;
		}
		const size_t row = exports_.mint(inner);
		return row == ExportTable::kNoRow ? kNoRow : row;
	}
	if (inner.isDevice()) {
		const device::Descriptor* d = b->descriptorOf(inner.device);
		if (d == nullptr || d->mod == nullptr || inner.index >= d->outputCount()) {
			return kNoRow; // not a modulator output in PRE — a POST source reaches only what follows it (O3)
		}
	}
	else if (inner.index >= b->axes().coreSources) {
		return kNoRow; // one of PRE's macros, the site's one set (decision O7), or nothing
	}
	// MINTED WITHOUT MARKING THE PATCH: arming a PRE source in the site's view spells it for the device and POST, which
	// mints its row before any route exists. The route a hand then makes is what marks the patch, and the row is in the
	// body that save writes.
	const size_t row = exports_.mint(inner);
	return row == ExportTable::kNoRow ? kNoRow : row;
}

backplane::Source SiteChains::exportedSource(size_t row) const noexcept { return exports_.source(row); }

void SiteChains::publishExports() noexcept {
	exports_.publish(
	    [](const void* ctx, backplane::Source s) { return static_cast<const RackEngine*>(ctx)->sourceCell(s); },
	    &pre_.engine());
}

device::RouteDrop SiteChains::service() {
	device::RouteDrop merged = pre_.service();
	const device::RouteDrop more = post_.service();
	if (merged.count == 0) {
		merged.firstSource = more.firstSource;
		merged.firstDestination = more.firstDestination;
	}
	merged.count += more.count;
	return merged;
}

size_t SiteChains::deviceCount(uint16_t lane) const noexcept {
	const InnerChain& chain = lane == kPreLane ? pre_ : post_;
	return chain.engine().chainLength(0);
}

void SiteChains::process(InnerChain& lane, float* left, float* right, int frames, const device::TransportClock& clock,
                         const device::NoteContext& note) {
	lane.setTransportClock(clock);
	lane.setNoteContext(note);
	lane.process(left, right, frames);
	// PRE HAS TICKED: its exports into the cells the host and POST read (backlog NZ).
	if (&lane == &pre_) {
		publishExports();
	}
}

void SiteChains::reset() noexcept {
	pre_.reset();
	post_.reset();
	dryDelay_.reset();
}

namespace {

std::array<BundleLane, 2> lanesOfSite(void* storage) { return static_cast<SiteChains*>(storage)->lanes(); }

/// **THE LANES' TABLE, WITH THE SITE'S OWN BODY AND GATE** (backlog NZ): the body carries the export table beside the
/// lanes, and PRE's gate refuses the site's own exports (`SiteChains::setOuterGate`).
constexpr device::ContainerOps siteOps() {
	device::ContainerOps ops = laneContainerOps<&lanesOfSite>();
	ops.saveState = [](const void* storage, const device::ByteSink& sink) {
		return static_cast<const SiteChains*>(storage)->saveState(sink);
	};
	ops.loadState = [](void* storage, const uint8_t* data, size_t bytes) {
		return static_cast<SiteChains*>(storage)->loadState(data, bytes);
	};
	ops.applyState = [](void* storage, const uint8_t* data, size_t bytes) {
		return static_cast<SiteChains*>(storage)->applyState(data, bytes);
	};
	ops.setOuterGate = [](void* storage, const device::OuterGate& gate) {
		static_cast<SiteChains*>(storage)->setOuterGate(gate);
	};
	ops.maxBodyBytes = bundleMaxBytes(2) + 2 * sizeof(uint32_t) + sizeof(uint16_t)
	                   + device::kSiteExportRows * (sizeof(uint32_t) + 2 * sizeof(uint16_t));
	return ops;
}

constexpr device::ContainerOps kSiteContainerOps = siteOps();

const device::Descriptor kSiteDescriptor {
	.traits = { .tag = "site", .name = "Site", .hasCard = false, .hasExpanded = false, .perVoiceControl = -1,
	            .siteLanes = device::Traits::SiteLanes::None },
	.params = {},
	.outputs = {},
	.container = &kSiteContainerOps,
};

} // namespace

const device::ContainerOps& siteContainerOps() noexcept { return kSiteContainerOps; }

const device::Descriptor& siteDescriptor() noexcept { return kSiteDescriptor; }

bool mapSiteBody(std::span<const uint8_t> bytes, LaneMap map, void* ctx, std::vector<uint8_t>* out) {
	if (bytes.empty()) {
		out->clear();
		return true;
	}
	const uint32_t magic = bytes.size() >= 2 * sizeof(uint32_t) ? get32(bytes.data()) : 0u;
	if (magic != kSiteMagic && magic != kSiteMagic2) {
		return mapChainBundle(bytes, map, ctx, out);
	}
	const size_t bundleBytes = get32(bytes.data() + 4);
	if (bundleBytes > bytes.size() - 2 * sizeof(uint32_t)) {
		return false;
	}
	std::vector<uint8_t> bundle;
	if (!mapChainBundle(bytes.subspan(2 * sizeof(uint32_t), bundleBytes), map, ctx, &bundle)) {
		return false;
	}
	std::vector<uint8_t> next;
	put32(next, magic); // the table is copied as it stands, so its framing stays the one it was written in
	put32(next, static_cast<uint32_t>(bundle.size()));
	next.insert(next.end(), bundle.begin(), bundle.end());
	const auto table = bytes.subspan(2 * sizeof(uint32_t) + bundleBytes);
	next.insert(next.end(), table.begin(), table.end());
	*out = std::move(next);
	return true;
}

const device::Descriptor* containerTableFor(DeviceInstanceId key, const device::Descriptor* own) noexcept {
	return isSiteKey(key) ? &siteDescriptor() : own;
}

std::string siteRoutePrefix(const void* surface, std::string holder, backplane::Destination inner) {
	const auto* site = static_cast<const SiteChains*>(surface);
	if (site == nullptr || !inner.isDevice()) {
		return holder;
	}
	// THE LANES SHARE ONE ID COUNTER, so an id is in at most one of them.
	const RackBridge* pre = site->pre().bridge();
	const bool inPre = pre != nullptr && pre->positionOf(inner.device) != RackBridge::kNoSlot;
	return holder + (inPre ? " PRE" : " POST");
}

} // namespace feathers::rack
