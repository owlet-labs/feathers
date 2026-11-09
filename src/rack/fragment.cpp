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

#include "rack/fragment.h"

#include "rack/inner_chain_bundle.h"
#include "rack/nesting.h"
#include "rack/parallel_rack.h"
#include "rack/rack_device.h"
#include "rack/site_chains.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string_view>

namespace feathers::rack {

namespace {

bool contains(std::span<const DeviceInstanceId> ids, DeviceInstanceId id) {
	return id != kNoInstance && std::find(ids.begin(), ids.end(), id) != ids.end();
}

/// A ROUTE END THAT IS ONE OF `ids` — a device's output or its PRE export, never an `Outer` source (whose id is the
/// host OUTSIDE this record's).
bool sourceIn(std::span<const DeviceInstanceId> ids, backplane::Source s) {
	return !s.isOuter() && s.device != kNoInstance && contains(ids, s.device);
}
bool destinationIn(std::span<const DeviceInstanceId> ids, backplane::Destination d) {
	return d.isDevice() && contains(ids, d.device);
}

/// The ids `site`'s chain holds out of `ids`, in CHAIN order, with where each stands.
std::vector<std::pair<DeviceInstanceId, size_t>> heldInOrder(FragmentSite& site, std::span<const DeviceInstanceId> ids) {
	const ChainRecord& record = site.fragmentRecord();
	const size_t chain = site.fragmentChain();
	std::vector<std::pair<DeviceInstanceId, size_t>> out;
	for (size_t p = 0; p < record.length(chain); ++p) {
		const DeviceInstanceId id = record.deviceAt(chain, p);
		if (contains(ids, id)) {
			out.emplace_back(id, p);
		}
	}
	return out;
}

/// A DEVICE AS A HAND READS IT: its card name, or its tag for one this build cannot run.
std::string deviceName(const ChainRecord& record, DeviceInstanceId id) {
	if (const device::Descriptor* d = record.descriptorOf(id); d != nullptr && d->traits.name != nullptr) {
		return d->traits.name;
	}
	const DeviceState& row = record.rowOf(id);
	return std::string(row.tag.data(), strnlen(row.tag.data(), row.tag.size()));
}

/// ONE ROUTE IN WORDS for a tray — "LFO → Delay · Time". A host end is "the host".
std::string routeWords(const ChainRecord& record, backplane::Source s, backplane::Destination d) {
	std::string from = "the host";
	if (s.isOuter()) {
		from = "a source outside the chain";
	}
	else if (s.isExport()) {
		from = deviceName(record, s.device) + (s.isSiteExport() ? " PRE" : " (inside)");
	}
	else if (s.isDevice()) {
		from = deviceName(record, s.device);
		if (const device::Descriptor* desc = record.descriptorOf(s.device);
		    desc != nullptr && desc->outputs.size() > 1 && s.index < desc->outputs.size()
		    && desc->outputs[s.index].name != nullptr) {
			from += std::string(" ") + desc->outputs[s.index].name;
		}
	}
	std::string to = "the host";
	if (d.isDevice()) {
		to = deviceName(record, d.device);
		if (const device::Descriptor* desc = record.descriptorOf(d.device);
		    desc != nullptr && d.index < desc->params.size() && desc->params[d.index].name != nullptr) {
			to += std::string(" · ") + desc->params[d.index].name;
		}
	}
	return from + " → " + to;
}

/// THE REMAP A PLACEMENT HANDS `remapOuterSources`: an id in `map` follows its device; any other id is kept only
/// in the record the fragment came from (`keepOthers`), because elsewhere it would name whatever wears it there.
struct PlaceRemap {
	const std::vector<std::pair<DeviceInstanceId, DeviceInstanceId>>* map = nullptr;
	bool keepOthers = false;
};

std::optional<backplane::Source> placeRemap(void* ctx, backplane::Source outer, uint32_t level) {
	const auto* r = static_cast<const PlaceRemap*>(ctx);
	if (outer.outerHops() < level) {
		return outer; // a source inside the placed body: its ids moved with it
	}
	if (outer.outerHops() > level) {
		return r->keepOthers ? std::optional<backplane::Source> { outer } : std::nullopt; // past the record
	}
	for (const auto& [from, to] : *r->map) {
		if (outer.device != kNoInstance && outer.device == from) {
			return backplane::Source { to, outer.index };
		}
	}
	if (r->keepOthers) {
		return outer;
	}
	return std::nullopt;
}

/// THE REMAP A GROUP HANDS IT: a reference to a device that moves into the group with it still names that device
/// (the group keeps the ids, and is now the reference's outer record); anything else now lies ONE HOST FURTHER OUT, the
/// group being a host between them — spelled so since sources reach any depth (2026-10-29), and dropped only past the cap.
struct GroupRemap {
	std::span<const DeviceInstanceId> moved;
};

std::optional<backplane::Source> groupRemap(void* ctx, backplane::Source outer, uint32_t level) {
	const auto* r = static_cast<const GroupRemap*>(ctx);
	if (outer.outerHops() < level) {
		return outer; // inside the moved body
	}
	if (outer.outerHops() == level && outer.device != kNoInstance && contains(r->moved, outer.device)) {
		return outer;
	}
	const backplane::Source further = outer.asOuter();
	if (!further.isOuter()) {
		return std::nullopt; // past `kMaxOuterHops`
	}
	return further;
}

/// A DEVICE'S BODY AND SITE THROUGH A REMAP, in place. Bytes no framing here knows are left as they are — a
/// container the rack code does not write (none today; the framing case keeps it that way).
size_t remapDeviceBytes(RackPatch::Device* device, OuterRemap remap, void* ctx) {
	size_t dropped = 0;
	if (!device->body.empty()) {
		if (auto next = remapOuterSources(device->body, remap, ctx, &dropped); next.has_value()) {
			device->body = std::move(*next);
		}
	}
	if (!device->site.empty()) {
		if (auto next = remapOuterSources(device->site, remap, ctx, &dropped); next.has_value()) {
			device->site = std::move(*next);
		}
	}
	return dropped;
}

/// One chain's document through the remap: every route whose source is `Outer`, at `level` (see `OuterRemap`).
struct DocRemap {
	OuterRemap remap = nullptr;
	void* ctx = nullptr;
	size_t* dropped = nullptr;
	uint32_t level = 1;
};

bool remapFramed(const DocRemap& doc, std::span<const uint8_t> body, std::vector<uint8_t>* out);

bool remapDocument(void* ctx, std::span<const uint8_t> in, std::vector<uint8_t>* out) {
	const auto* r = static_cast<const DocRemap*>(ctx);
	RackPatch patch;
	if (!parseRackText(std::string_view(reinterpret_cast<const char*>(in.data()), in.size()), &patch)) {
		return false;
	}
	bool changed = false;
	std::vector<RackPatch::Route> kept;
	kept.reserve(patch.route.size());
	for (RackPatch::Route& route : patch.route) {
		if (route.source.kind != static_cast<uint32_t>(SourceKind::Outer)) {
			kept.push_back(std::move(route));
			continue;
		}
		const std::optional<backplane::Source> next =
		    r->remap(r->ctx, backplane::Source { route.source.a, route.source.b }, r->level);
		if (!next.has_value()) {
			++*r->dropped;
			changed = true;
			continue;
		}
		if (next->device != route.source.a || next->index != route.source.b) {
			route.source.a = next->device;
			route.source.b = next->index;
			changed = true;
		}
		kept.push_back(std::move(route));
	}
	// AND ONE HOST FURTHER IN: every device's own body and site, a level deeper. Bytes no framing knows (a leaf's own
	// body) are left as they are.
	DocRemap deeper = *r;
	deeper.level = r->level + 1;
	for (RackPatch::Device& device : patch.device) {
		for (std::vector<uint8_t>* bytes : { &device.body, &device.site }) {
			std::vector<uint8_t> next;
			if (!bytes->empty() && remapFramed(deeper, *bytes, &next) && next != *bytes) {
				*bytes = std::move(next);
				changed = true;
			}
		}
	}
	if (!changed) {
		out->assign(in.begin(), in.end());
		return true;
	}
	patch.route = std::move(kept);
	std::string text;
	if (!rackToText(patch, &text, false)) {
		return false;
	}
	out->assign(text.begin(), text.end());
	return true;
}

uint32_t leU32(std::span<const uint8_t> b) {
	return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) | (static_cast<uint32_t>(b[2]) << 16)
	       | (static_cast<uint32_t>(b[3]) << 24);
}

/// THE CHAIN'S OWN TAG FOR A GROUP KIND.
const device::Descriptor& containerOf(GroupKind kind) {
	return kind == GroupKind::Rack ? parallel::kDescriptor : kDeviceDescriptor;
}

/// Whether a moved device's Poly switch is on, by its row (a field past `fieldsPresent` is its default).
bool polyOn(const ChainRecord& record, DeviceInstanceId id) {
	const device::Descriptor* d = record.descriptorOf(id);
	if (d == nullptr || d->traits.perVoiceControl < 0) {
		return false;
	}
	const DeviceState& row = record.rowOf(id);
	const auto valueOf = [&](size_t c) {
		return static_cast<float>(c < row.fieldsPresent ? row.field[c] : d->params[c].def);
	};
	return valueOf(static_cast<size_t>(d->traits.perVoiceControl)) >= 0.5f && device::polyAllowed(*d, valueOf);
}

} // namespace

uint64_t mintFragmentOrigin() noexcept {
	static std::atomic<uint64_t> next { 1 };
	return next.fetch_add(1, std::memory_order_relaxed);
}

bool Fragment::names(DeviceInstanceId id) const noexcept {
	if (id == kNoInstance) {
		return false;
	}
	for (const RackPatch::Device& d : patch.device) {
		if (d.instanceId == id) {
			return true;
		}
	}
	return false;
}

std::vector<DeviceInstanceId> Fragment::ids() const {
	std::vector<DeviceInstanceId> out;
	out.reserve(patch.device.size());
	for (const RackPatch::Device& d : patch.device) {
		out.push_back(d.instanceId);
	}
	return out;
}

RouteClass classify(const Fragment& f, const RackPatch::Route& route) noexcept {
	const bool from = (route.source.kind == static_cast<uint32_t>(SourceKind::Device) || isExportKind(route.source.kind))
	                  && f.names(route.source.a);
	const bool to = route.destination.kind == static_cast<uint32_t>(DestKind::Device) && f.names(route.destination.a);
	if (from && to) {
		return RouteClass::Among;
	}
	return to ? RouteClass::Inbound : RouteClass::Outbound;
}

Fragment liftFragment(FragmentSite& site, std::span<const DeviceInstanceId> ids) {
	site.fragmentCatchUp();
	const ChainRecord& record = site.fragmentRecord();
	Fragment f;
	f.origin = site.fragmentOrigin();
	f.patch.present = true;
	const auto held = heldInOrder(site, ids);
	std::vector<DeviceInstanceId> lifted;
	for (const auto& [id, at] : held) {
		f.patch.device.push_back(patchDeviceOfRow(record, &site.fragmentBodies(), site.fragmentViews(),
		                                          site.fragmentLabels(), record.rowOf(id), 0));
		f.positions.push_back(at);
		lifted.push_back(id);
	}
	const CoreCodec codec = site.fragmentCodec();
	record.graph().forEachRoute([&](const backplane::Route& r) {
		if (!sourceIn(lifted, r.source) && !destinationIn(lifted, r.destination)) {
			return;
		}
		RackPatch::Route out;
		if (patchRouteOfRecord(record, codec, r, &out)) {
			f.patch.route.push_back(std::move(out));
		}
	});
	return f;
}

//------------------------------------------------------------------------------------------------//
// THE PASTEBOARD                                                                                   //
//------------------------------------------------------------------------------------------------//

namespace {
constexpr std::string_view kClipHead = "{\"feathersClip\":1,\"patch\":";
}

std::string clipText(const Fragment& f) {
	std::string doc;
	if (!rackToText(f.patch, &doc, false)) {
		return {};
	}
	return std::string(kClipHead) + doc + "}";
}

std::optional<Fragment> fragmentOfClipText(std::string_view text) {
	if (text.size() < kClipHead.size() + 1 || text.substr(0, kClipHead.size()) != kClipHead || text.back() != '}') {
		return std::nullopt;
	}
	Fragment f;
	if (!parseRackText(text.substr(kClipHead.size(), text.size() - kClipHead.size() - 1), &f.patch)) {
		return std::nullopt;
	}
	f.patch.present = true;
	f.origin = 0; // from nowhere: its inbound routes go wherever it lands
	return f;
}

namespace {
struct Pasteboard {
	std::string text;
	uint64_t origin = 0;
	OsClipboard os;
};
Pasteboard& board() {
	static Pasteboard held;
	return held;
}
} // namespace

bool isClipText(std::string_view text) noexcept {
	return text.size() > kClipHead.size() && text.substr(0, kClipHead.size()) == kClipHead && text.back() == '}';
}

void setOsClipboard(OsClipboard port) { board().os = std::move(port); }

void clearOsClipboard() noexcept { board().os = OsClipboard {}; }

void setPasteboard(const Fragment& f) {
	board().text = clipText(f);
	board().origin = board().text.empty() ? 0 : f.origin;
	if (!board().text.empty() && board().os.write) {
		board().os.write(board().text);
	}
}

// **PASTE READS THE SYSTEM'S CLIPBOARD FIRST AND TRUSTS IT ONLY FOR A CLIP OF OURS THAT IS NOT THE ONE WE PUT THERE.**
// Text equal to the in-process board's is our own copy coming back, so the board answers and keeps its origin (a paste
// into the record it was lifted from keeps its inbound routes). A different clip of ours is newer or from another
// plugin instance or session: it wins, and it is from nowhere. Anything else — plain text, a foreign document, a clip
// that does not parse — is refused CLEANLY: it never reaches the record, and the in-process board (if any) answers as it
// did before the OS road existed.
std::optional<Fragment> pasteboard() {
	if (board().os.read) {
		const std::string system = board().os.read();
		if (!system.empty() && system != board().text && isClipText(system)) {
			if (std::optional<Fragment> foreign = fragmentOfClipText(system); foreign.has_value()) {
				return foreign;
			}
		}
	}
	std::optional<Fragment> f = fragmentOfClipText(board().text);
	if (f.has_value()) {
		f->origin = board().origin;
	}
	return f;
}

bool pasteboardHolds() noexcept {
	if (!board().text.empty()) {
		return true;
	}
	if (board().os.read) {
		try {
			return isClipText(board().os.read());
		}
		catch (...) {
			return false;
		}
	}
	return false;
}

void clearPasteboard() noexcept {
	board().text.clear();
	board().origin = 0;
}

//------------------------------------------------------------------------------------------------//
// PLACE AND REMOVE                                                                                 //
//------------------------------------------------------------------------------------------------//

bool fragmentFits(FragmentSite& site, const Fragment& f) {
	const ChainRecord& record = site.fragmentRecord();
	return fitsNesting(record.depth(), patchHeight(f.patch, record.catalogue()));
}

PlaceResult placeFragment(FragmentSite& site, const Fragment& f, size_t position, PlaceIds ids, bool outbound) {
	PlaceResult result;
	if (f.empty()) {
		return result;
	}
	ChainRecord& record = site.fragmentRecord();
	// THE DEPTH GATE, ON THE WHOLE FRAGMENT AS IT WILL STAND HERE (2026-10-31): a paste, a duplicate, a drag into another
	// chain and an undo's put-back all land here, and content too tall for this chain is refused whole — nothing placed,
	// nothing changed — rather than built live and given back empty by the next load.
	if (!fragmentFits(site, f)) {
		result.devicesRefused = f.size();
		result.tooDeep = true;
		return result;
	}
	DeviceLabels* labels = site.fragmentLabels();
	const DeviceLabels labelsBefore = labels != nullptr ? *labels : DeviceLabels {};
	const bool sameOrigin = f.origin != 0 && f.origin == site.fragmentOrigin();

	// WHICH ROUTES TRAVEL (D3): among always; inbound only back into the record it was lifted from; outbound only on
	// an undo's put-back — or, where the site says a copy keeps its routes out (the modulator strip, backlog PN), on a
	// fresh copy back into the record it was lifted from, the only record whose devices those routes can name.
	// What stays behind is counted.
	const bool outboundTravels =
	    outbound || (ids == PlaceIds::Fresh && sameOrigin && site.fragmentCopiesOutbound());
	RackPatch patch = f.patch;
	patch.route.clear();
	for (const RackPatch::Route& route : f.patch.route) {
		const RouteClass c = classify(f, route);
		const bool travels = c == RouteClass::Among || (c == RouteClass::Inbound && sameOrigin)
		                     || (c == RouteClass::Outbound && outboundTravels);
		if (travels) {
			patch.route.push_back(route);
		}
		else if (c == RouteClass::Outbound) {
			++result.outboundLeft; // by design (D3): a copy does not reach what the original reached
		}
		else {
			++result.routesDropped;
		}
	}

	// FRESH IDS, TAKEN NOW, so every reference to a device can be re-pointed before anything lands.
	std::vector<std::pair<DeviceInstanceId, DeviceInstanceId>> map;
	if (ids == PlaceIds::Fresh) {
		for (RackPatch::Device& device : patch.device) {
			const DeviceInstanceId fresh = site.fragmentIds().issue();
			map.emplace_back(device.instanceId, fresh);
			device.instanceId = fresh;
		}
		const auto follow = [&](PatchCoord& c, bool device) {
			if (!device) {
				return;
			}
			for (const auto& [from, to] : map) {
				if (c.a == from) {
					c.a = to;
					return;
				}
			}
		};
		for (RackPatch::Route& route : patch.route) {
			follow(route.source, route.source.kind == static_cast<uint32_t>(SourceKind::Device)
			                         || isExportKind(route.source.kind));
			follow(route.destination, route.destination.kind == static_cast<uint32_t>(DestKind::Device));
		}
	}
	// AND WHAT EACH BODY AND SITE NAMES OF THIS RECORD — unchanged only on a put-back into the record it came from.
	if (ids == PlaceIds::Fresh || !sameOrigin) {
		PlaceRemap remap { &map, sameOrigin };
		for (RackPatch::Device& device : patch.device) {
			result.innerRoutesDropped += remapDeviceBytes(&device, &placeRemap, &remap);
		}
	}

	PatchPlacement placement;
	placement.chain = site.fragmentChain();
	placement.position = position == kOriginalPositions ? ChainRecord::kNoPosition : position;
	if (position == kOriginalPositions) {
		placement.positions = f.positions;
	}
	placement.outsideEnds = true;
	placement.worn = &result.worn;
	const size_t landed = placePatchInRecord(patch, &record, &site.fragmentBodies(), site.fragmentCodec(),
	                                         site.fragmentViews(), labels, placement, &result.report);
	result.devicesRefused = patch.device.size() - landed;
	result.routesDropped += result.report.routes.count;
	// THE WORN MAP NAMES WHAT THE PATCH SAID; THE CALLER WANTS WHAT THE FRAGMENT SAID.
	if (ids == PlaceIds::Fresh) {
		for (auto& [named, worn] : result.worn) {
			for (const auto& [from, to] : map) {
				if (to == named) {
					named = from;
					break;
				}
			}
		}
	}
	for (const auto& [named, worn] : result.worn) {
		result.placed.push_back(worn);
		// AN UNDO'S PUT-BACK: the host slots this device held come back from their notes, under the same path.
		if (ids == PlaceIds::Keep) {
			site.fragmentRestoreSlots(worn);
		}
	}
	if (landed > 0) {
		site.fragmentChanged(labelsBefore);
	}
	return result;
}

size_t removeFragmentDevices(FragmentSite& site, std::span<const DeviceInstanceId> ids) {
	ChainRecord& record = site.fragmentRecord();
	DeviceLabels* labels = site.fragmentLabels();
	const DeviceLabels labelsBefore = labels != nullptr ? *labels : DeviceLabels {};
	size_t gone = 0;
	for (const auto& [id, at] : heldInOrder(site, ids)) {
		(void)at;
		if (!record.remove(id)) {
			continue;
		}
		ContainerBodies& bodies = site.fragmentBodies();
		(void)bodies.erase(id);
		(void)bodies.erase(siteKeyOf(id));
		if (labels != nullptr) {
			labels->erase(id);
		}
		if (DeviceViews* views = site.fragmentViews(); views != nullptr) {
			views->set(id, 0);
		}
		site.fragmentFreeSlots(id);
		++gone;
	}
	if (gone > 0) {
		site.fragmentChanged(labelsBefore);
	}
	return gone;
}

//------------------------------------------------------------------------------------------------//
// GROUP                                                                                            //
//------------------------------------------------------------------------------------------------//

std::string groupRefusal(FragmentSite& site, std::span<const DeviceInstanceId> ids) {
	if (!site.fragmentGroups()) {
		return "Modulators do not group here";
	}
	const auto held = heldInOrder(site, ids);
	if (held.empty()) {
		return "Select the devices to group first";
	}
	const ChainRecord& record = site.fragmentRecord();
	const size_t chain = site.fragmentChain();
	// D4: AN AUDIO DEVICE THE SELECTION SPLITS AROUND would be moved by the group — after it, or into it — and that
	// is a change to the signal nobody asked for. A modulator between is no obstacle: its place is not a precedence.
	for (size_t p = held.front().second; p <= held.back().second; ++p) {
		const DeviceInstanceId id = record.deviceAt(chain, p);
		if (contains(ids, id)) {
			continue;
		}
		const device::Descriptor* d = record.descriptorOf(id);
		if (d == nullptr || d->audio != nullptr) {
			return "The selection is split by " + deviceName(record, id) + ": select it too, or move it";
		}
	}
	// THE DEPTH GATE: the group is a chain below this one holding the selection, so it stands one taller than it.
	if (!fitsNesting(record.depth(), 1 + patchHeight(liftFragment(site, ids).patch, record.catalogue()))) {
		return kNestingFullMessage;
	}
	return {};
}

std::optional<GroupPlan> planGroup(FragmentSite& site, std::span<const DeviceInstanceId> ids, GroupKind kind) {
	if (!groupRefusal(site, ids).empty()) {
		return std::nullopt;
	}
	const Fragment before = liftFragment(site, ids);
	const ChainRecord& record = site.fragmentRecord();
	GroupPlan plan;
	plan.kind = kind;
	plan.moved = before.ids();
	plan.position = before.positions.front();
	plan.containerId = site.fragmentIds().issue();
	plan.inner.present = true;
	plan.inner.device = before.patch.device;
	const auto lose = [&plan](std::string words) {
		if (plan.routesLost == 0) {
			plan.firstLost = std::move(words);
		}
		++plan.routesLost;
	};
	// THE MOVED DEVICES' OWN BODIES: what they named of this record now names the group's (the same ids), or goes.
	GroupRemap remap { plan.moved };
	for (RackPatch::Device& device : plan.inner.device) {
		device.chain = 0;
		const size_t dropped = remapDeviceBytes(&device, &groupRemap, &remap);
		for (size_t i = 0; i < dropped; ++i) {
			lose(deviceName(record, device.instanceId) + ": a route inside it from outside the group");
		}
	}
	const CoreCodec codec = site.fragmentCodec();
	record.graph().forEachRoute([&](const backplane::Route& r) {
		const bool from = sourceIn(plan.moved, r.source);
		const bool to = destinationIn(plan.moved, r.destination);
		if (!from && !to) {
			return;
		}
		RackPatch::Route out;
		if (!patchRouteOfRecord(record, codec, r, &out)) {
			lose(routeWords(record, r.source, r.destination));
			return;
		}
		if (from && to) {
			plan.inner.route.push_back(std::move(out)); // among: the same ids inside
			return;
		}
		if (to) {
			// INBOUND: the source stays out here and is read through the group's outer gate (rack roadmap §7) — kept
			// only if that gate admits it, which is asked once the group stands where it will (`applyGroup`). A holder's
			// PRE export left outside is one too (NZ rung 5): the group reads it as any container after the holder does.
			// ONE HOP FURTHER OUT, already-outer sources included (any depth, 2026-10-29) — `asOuter`, not the bit.
			out.source = PatchCoord { static_cast<uint32_t>(SourceKind::Outer), r.source.device, r.source.asOuter().index, {} };
			plan.inbound.emplace_back(r.source, std::move(out));
			return;
		}
		// OUTBOUND FROM A MOVED MODULATOR (backlog NZ (b)): the group exports it, and the route outside reads the export
		// — one row per distinct source, in the group's own table. Kept only if the gate admits it once the group stands
		// (`applyGroup`): "downstream" is the cycle rule's to say, as for any route. AND FROM A MOVED HOLDER'S OWN EXPORT
		// (its site's PRE, or the inside of a Chain or a Rack): the group re-exports that row one hop out, and the moved
		// holder's table stays its owner (NZ, any depth —).
		if (from && r.source.isDevice() && !r.source.isOuter()) {
			size_t row = plan.inner.exports.size();
			for (const RackPatch::Export& e : plan.inner.exports) {
				if (e.device == r.source.device && e.index == r.source.index) {
					row = e.row;
				}
			}
			if (row == plan.inner.exports.size()) {
				if (row >= device::kSiteExportRows) {
					lose(routeWords(record, r.source, r.destination));
					return;
				}
				plan.inner.exports.push_back({ static_cast<uint32_t>(row), r.source.device, r.source.index });
			}
			plan.outbound.push_back({ backplane::Source::containerExported(plan.containerId, row), r.destination, r.amount, r.transfer });
			return;
		}
		// WHAT IS LEFT CANNOT BE SPELLED out of the group (a source with no device). Said, not silently dropped.
		lose(routeWords(record, r.source, r.destination));
	});
	if (kind == GroupKind::Rack && site.fragmentHostsVoices()) {
		for (const DeviceInstanceId id : plan.moved) {
			plan.polyOnBus += polyOn(record, id) ? 1 : 0;
		}
	}
	plan.before = before;
	return plan;
}

DeviceInstanceId applyGroup(FragmentSite& site, GroupPlan& plan) {
	ChainRecord& record = site.fragmentRecord();
	const size_t chain = site.fragmentChain();
	if (record.holds(plan.containerId)) {
		return kNoInstance;
	}
	for (const DeviceInstanceId id : plan.moved) {
		if (!record.holds(id) || record.chainOf(id) != chain) {
			return kNoInstance; // the record is not what the plan was made over
		}
	}
	// AND AGAIN AT THE APPLY, which a redo reaches without a plan of its own (`groupRefusal` is the plan's).
	if (!fitsNesting(record.depth(), 1 + patchHeight(plan.inner, record.catalogue()))) {
		return kNoInstance;
	}
	DeviceLabels* labels = site.fragmentLabels();
	const DeviceLabels labelsBefore = labels != nullptr ? *labels : DeviceLabels {};
	ContainerBodies& bodies = site.fragmentBodies();
	// THE MOVED DEVICES LEAVE — and NOT their host slots, which move with them (below).
	for (const DeviceInstanceId id : plan.moved) {
		(void)record.remove(id);
		(void)bodies.erase(id);
		(void)bodies.erase(siteKeyOf(id));
		if (labels != nullptr) {
			labels->erase(id);
		}
		if (DeviceViews* views = site.fragmentViews(); views != nullptr) {
			views->set(id, 0);
		}
	}
	DeviceState row;
	const device::Descriptor& container = containerOf(plan.kind);
	const device::Tag tag = device::tagOf(container.traits.tag);
	row.tag = tag;
	row.instanceId = plan.containerId;
	row.fieldsPresent = 0; // every control at its declared default
	if (!record.insertRow(chain, std::min(plan.position, record.length(chain)), row)) {
		return kNoInstance;
	}
	// THE GATE ANSWERS NOW, with the group standing where it will: each inbound route kept or counted.
	RackPatch inner = plan.inner;
	plan.inboundLost = 0;
	for (const auto& [source, route] : plan.inbound) {
		if (record.sourceUpstreamOf(source, plan.containerId)) {
			inner.route.push_back(route);
		}
		else {
			if (plan.routesLost + plan.inboundLost == 0) {
				plan.firstLost = routeWords(record, source, backplane::Destination::control(plan.containerId, 0));
			}
			++plan.inboundLost;
		}
	}
	// A RACK KEEPS ITS EXPORT TABLE BESIDE ITS CHAINS, a Chain inside its own body (NZ (b)).
	std::vector<RackPatch::Export> exports;
	if (plan.kind == GroupKind::Rack) {
		exports.swap(inner.exports);
	}
	std::string text;
	(void)rackToText(inner, &text, false);
	std::vector<uint8_t> chainBody(text.begin(), text.end());
	const std::vector<uint8_t> body =
	    plan.kind == GroupKind::Rack ? parallel::bodyOfOneChain(chainBody, exports) : chainBody;
	(void)bodies.store(plan.containerId, body.data(), body.size());
	// THE ROUTES OUT, through the group's export, where the gate admits them with the group in place (NZ (b)).
	for (const backplane::Route& r : plan.outbound) {
		if (record.mayRoute(r.source, r.destination) && record.setRoute(r.source, r.destination, r.amount)) {
			(void)record.setRouteTransfer(r.source, r.destination, r.transfer);
		}
		else {
			if (plan.routesLost + plan.inboundLost == 0) {
				plan.firstLost = routeWords(record, r.source, r.destination);
			}
			++plan.inboundLost;
		}
	}
	// AND THE HOST SLOTS GO DOWN A LEVEL WITH THEIR DEVICES: the same DAW parameter now drives the same control
	// inside the group. One that cannot move is counted.
	plan.slotsLost = 0;
	for (const DeviceInstanceId id : plan.moved) {
		plan.slotsLost += site.fragmentRebaseSlots(id, plan.containerId, true) ? 0 : 1;
	}
	site.fragmentChanged(labelsBefore);
	return plan.containerId;
}

bool undoGroup(FragmentSite& site, DeviceInstanceId container, const Fragment& before) {
	ChainRecord& record = site.fragmentRecord();
	if (!record.holds(container)) {
		return false;
	}
	for (const DeviceInstanceId id : before.ids()) {
		(void)site.fragmentRebaseSlots(id, container, false);
	}
	DeviceLabels* labels = site.fragmentLabels();
	const DeviceLabels labelsBefore = labels != nullptr ? *labels : DeviceLabels {};
	(void)record.remove(container);
	(void)site.fragmentBodies().erase(container);
	(void)site.fragmentBodies().erase(siteKeyOf(container));
	if (labels != nullptr) {
		labels->erase(container);
	}
	site.fragmentChanged(labelsBefore);
	// THE DEVICES BACK WHERE THEY STOOD, UNDER THEIR OWN IDS, every route with them. Their slots came back up
	// above, so the put-back's restore finds nothing to do for them — and does not need to.
	const PlaceResult back = placeFragment(site, before, kOriginalPositions, PlaceIds::Keep, true);
	return back.devicesRefused == 0;
}

//------------------------------------------------------------------------------------------------//
// MOVE AS A BLOCK                                                                                  //
//------------------------------------------------------------------------------------------------//

std::vector<backplane::Route> routesTouching(FragmentSite& site, std::span<const DeviceInstanceId> ids) {
	std::vector<backplane::Route> out;
	site.fragmentRecord().graph().forEachRoute([&](const backplane::Route& r) {
		if (sourceIn(ids, r.source) || destinationIn(ids, r.destination)) {
			out.push_back(r);
		}
	});
	return out;
}

bool moveBlock(FragmentSite& site, std::span<const DeviceInstanceId> ids, size_t gap) {
	ChainRecord& record = site.fragmentRecord();
	const size_t chain = site.fragmentChain();
	const auto held = heldInOrder(site, ids);
	if (held.empty()) {
		return false;
	}
	// WHERE THE BLOCK LANDS, counted among the devices that are NOT moving: `gap` names a gap of the whole chain, and
	// every selected device before it is one fewer in front of the landing place.
	size_t before = 0;
	for (const auto& [id, at] : held) {
		before += at < gap ? 1 : 0;
	}
	const size_t length = record.length(chain);
	const size_t landing = std::min(gap, length) - before;
	// THE ORDER THE CHAIN SHOULD END IN — the others as they stand, the block inserted at `landing` — and then each
	// position filled from the front, so a move never disturbs a place already settled.
	std::vector<DeviceInstanceId> next;
	for (size_t p = 0; p < length; ++p) {
		if (!contains(ids, record.deviceAt(chain, p))) {
			next.push_back(record.deviceAt(chain, p));
		}
	}
	std::vector<DeviceInstanceId> block;
	for (const auto& [id, at] : held) {
		block.push_back(id);
	}
	next.insert(next.begin() + static_cast<std::ptrdiff_t>(std::min(landing, next.size())), block.begin(), block.end());
	bool moved = false;
	for (size_t i = 0; i < next.size(); ++i) {
		const size_t from = record.positionOf(next[i]);
		if (from != i && record.move(chain, from, i)) {
			moved = true;
		}
	}
	if (moved) {
		const DeviceLabels* labels = site.fragmentLabels();
		site.fragmentChanged(labels != nullptr ? *labels : DeviceLabels {});
	}
	return moved;
}

bool restoreOrder(FragmentSite& site, std::span<const DeviceInstanceId> order, std::span<const backplane::Route> routes) {
	ChainRecord& record = site.fragmentRecord();
	const size_t chain = site.fragmentChain();
	bool changed = false;
	for (size_t i = 0; i < order.size(); ++i) {
		const size_t from = record.positionOf(order[i]);
		if (from == ChainRecord::kNoPosition || record.chainOf(order[i]) != chain) {
			continue;
		}
		if (from != i && record.move(chain, from, i)) {
			changed = true;
		}
	}
	for (const backplane::Route& r : routes) {
		if (record.routeAmount(r.source, r.destination) != r.amount && record.setRoute(r.source, r.destination, r.amount)) {
			(void)record.setRouteTransfer(r.source, r.destination, r.transfer);
			changed = true;
		}
	}
	if (changed) {
		const DeviceLabels* labels = site.fragmentLabels();
		site.fragmentChanged(labels != nullptr ? *labels : DeviceLabels {});
	}
	return changed;
}

//------------------------------------------------------------------------------------------------//
// A BODY'S REFERENCES                                                                              //
//------------------------------------------------------------------------------------------------//

namespace {

bool remapFramed(const DocRemap& doc, std::span<const uint8_t> body, std::vector<uint8_t>* out) {
	out->clear();
	if (body.empty()) {
		return true;
	}
	// A CHAIN'S DOCUMENT IS JSON; THE THREE FRAMES ARE MAGICS. The order matters only in that a document cannot begin
	// with any of the magics' first bytes.
	void* ctx = const_cast<DocRemap*>(&doc);
	if (body.front() == '{') {
		return remapDocument(ctx, body, out);
	}
	if (body.size() >= sizeof(uint32_t)) {
		const uint32_t magic = leU32(body);
		if (magic == kBundleMagic) {
			return mapChainBundle(body, &remapDocument, ctx, out);
		}
		if (magic == parallel::kBodyMagic) {
			return parallel::mapRackBody(body, &remapDocument, ctx, out);
		}
		return mapSiteBody(body, &remapDocument, ctx, out); // SIT1, or nothing it knows
	}
	return false;
}

} // namespace

std::optional<std::vector<uint8_t>> remapOuterSources(std::span<const uint8_t> body, OuterRemap remap, void* ctx,
                                                      size_t* dropped) {
	size_t local = 0;
	const DocRemap doc { remap, ctx, dropped != nullptr ? dropped : &local, 1 };
	std::vector<uint8_t> out;
	if (!remapFramed(doc, body, &out)) {
		return std::nullopt;
	}
	return out;
}

namespace {

/// One chain's document with its `Outer` routes and its export table gone (`withoutOutsideRoutes`).
bool documentWithoutOutside(void* ctx, std::span<const uint8_t> in, std::vector<uint8_t>* out) {
	auto* dropped = static_cast<size_t*>(ctx);
	RackPatch patch;
	if (!parseRackText(std::string_view(reinterpret_cast<const char*>(in.data()), in.size()), &patch)) {
		return false;
	}
	const size_t before = patch.route.size() + patch.exports.size();
	std::erase_if(patch.route, [](const RackPatch::Route& route) {
		return route.source.kind == static_cast<uint32_t>(SourceKind::Outer);
	});
	patch.exports.clear();
	const size_t gone = before - patch.route.size();
	if (gone == 0) {
		out->assign(in.begin(), in.end());
		return true;
	}
	*dropped += gone;
	std::string text;
	if (!rackToText(patch, &text, false)) {
		return false;
	}
	out->assign(text.begin(), text.end());
	return true;
}

} // namespace

std::optional<std::vector<uint8_t>> withoutOutsideRoutes(std::span<const uint8_t> whole, size_t* dropped) {
	size_t local = 0;
	size_t* count = dropped != nullptr ? dropped : &local;
	std::vector<uint8_t> out;
	if (whole.empty()) {
		return out;
	}
	// FIRST EVERY DEPTH BELOW THE PRESET'S OWN CHAINS: a nested route that reaches past the preset (as many hops as its
	// depth, or more) goes; one naming something inside stays. The preset's own chains are then stripped below.
	const std::optional<std::vector<uint8_t>> deep = remapOuterSources(
	    whole,
	    [](void*, backplane::Source outer, uint32_t level) -> std::optional<backplane::Source> {
		    if (level > 1 && outer.outerHops() >= level) {
			    return std::nullopt;
		    }
		    return outer;
	    },
	    nullptr, count);
	if (!deep.has_value()) {
		return std::nullopt;
	}
	const std::span<const uint8_t> body(*deep);
	if (body.front() == '{') {
		if (!documentWithoutOutside(count, body, &out)) {
			return std::nullopt;
		}
		return out;
	}
	if (body.size() >= sizeof(uint32_t) && leU32(body) == parallel::kBodyMagic) {
		// EACH CHAIN'S DOCUMENT COUNTS ITS OWN; the rack's export rows are counted by the walk.
		size_t rows = 0;
		if (!parallel::mapRackBody(body, &documentWithoutOutside, count, &out, &rows)) {
			return std::nullopt;
		}
		*count += rows;
		return out;
	}
	return std::nullopt;
}

} // namespace feathers::rack
