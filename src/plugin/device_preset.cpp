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

#include "plugin/device_preset.h"

#include "plugin/preset_store.h"
#include "backplane/container_bodies.h"
#include "rack/fragment.h"
#include "rack/rack_state.h"
#include "rack/site_chains.h"

#include <algorithm>
#include <cstring>

namespace feathers {

namespace {

/// A clap_ostream over a growing byte vector.
struct VecSink {
	std::vector<uint8_t>* out = nullptr;
	static int64_t CLAP_ABI write(const clap_ostream_t* self, const void* buffer, uint64_t size) {
		auto* impl = static_cast<VecSink*>(self->ctx);
		const auto* bytes = static_cast<const uint8_t*>(buffer);
		impl->out->insert(impl->out->end(), bytes, bytes + size);
		return static_cast<int64_t>(size);
	}
};

/// A clap_istream over a byte span.
struct SpanSource {
	std::span<const uint8_t> in;
	size_t at = 0;
	static int64_t CLAP_ABI read(const clap_istream_t* self, void* buffer, uint64_t size) {
		auto* impl = static_cast<SpanSource*>(self->ctx);
		const size_t n = static_cast<size_t>(std::min<uint64_t>(size, impl->in.size() - impl->at));
		if (n > 0) {
			std::memcpy(buffer, impl->in.data() + impl->at, n);
		}
		impl->at += n;
		return static_cast<int64_t>(n);
	}
};

/// How many floats a row of this device carries in full: its controls and then its extra state.
size_t rowWidth(const device::Descriptor& desc) {
	return std::min(desc.params.size() + desc.traits.extraState, device::kMaxDeviceFields);
}

std::vector<uint8_t> writeOne(const device::Descriptor& desc, const std::vector<float>& field, std::vector<uint8_t> body,
                              const DevicePresetSite* site = nullptr) {
	rack::RackPatch patch;
	patch.present = true;
	rack::RackPatch::Device d;
	std::strncpy(d.tag.data(), desc.traits.tag, device::kTagBytes);
	d.instanceId = kFirstInstance;
	// BY KEY (backlog JF): a control inserted into the device later changes nothing for this preset's others.
	rack::valuesOfRow(desc, field, field.size(), &d);
	d.body = std::move(body);
	// ITS SITE: the bytes in the field a full patch writes, and PRE's routes into the device's own
	// controls as routes from the device to itself — the only routes a preset may hold.
	if (site != nullptr) {
		d.hasSite = true;
		d.site = site->body;
		for (const DevicePresetSite::Route& r : site->routes) {
			rack::RackPatch::Route route;
			route.source = rack::PatchCoord { static_cast<uint32_t>(rack::SourceKind::Export), kFirstInstance, r.index, {} };
			route.destination = rack::PatchCoord { static_cast<uint32_t>(rack::DestKind::Device), kFirstInstance, 0, r.key };
			route.amount = r.amount;
			route.transfer = r.transfer;
			patch.route.push_back(std::move(route));
		}
	}
	patch.device.push_back(std::move(d));
	std::vector<uint8_t> out;
	VecSink sink { &out };
	const clap_ostream stream { &sink, &VecSink::write };
	if (!rack::writeRack(&stream, patch)) {
		out.clear();
	}
	return out;
}

bool sameTag(const char* a, const char* b) {
	return std::strncmp(a, b, device::kTagBytes) == 0;
}

/// THE OWNER A SITE BODY'S READS OF PRE NAME, AND WHAT THEY BECOME: `from` → `to`; every other `Outer` source refused.
struct OwnerMap {
	DeviceInstanceId from = kNoInstance;
	DeviceInstanceId to = kNoInstance;
};

std::optional<backplane::Source> reOwn(void* ctx, backplane::Source outer, uint32_t level) {
	const auto* m = static_cast<const OwnerMap*>(ctx);
	if (outer.outerHops() < level) {
		return outer; // inside the site: a container in PRE or POST naming its own
	}
	// POST READING ITS OWN PRE (`exported(owner, row)`, as many hops out as it is deep) travels, re-pointed; a host
	// source, another device's export or anything else from outside the site is dropped (OU: routes across the edge go).
	if (outer.outerHops() == level && outer.isSiteExport() && outer.device == m->from) {
		return backplane::Source { m->to, outer.index };
	}
	return std::nullopt;
}

/// A SITE BODY WITH ITS OWNER RE-POINTED, `from` → `to`. The bytes unchanged when nothing named it; nullopt for bytes
/// that are not a site.
std::optional<std::vector<uint8_t>> siteBodyOwnedBy(std::span<const uint8_t> body, DeviceInstanceId from, DeviceInstanceId to) {
	OwnerMap map { from, to };
	return rack::remapOuterSources(body, &reOwn, &map, nullptr);
}

/// THE CONTROL WHOSE KEY IS `key`, or `SIZE_MAX`.
size_t controlOfKey(const device::Descriptor& desc, const std::string& key) {
	for (size_t i = 0; i < desc.params.size(); ++i) {
		if (desc.params[i].key != nullptr && key == desc.params[i].key) {
			return i;
		}
	}
	return SIZE_MAX;
}

/// THE ROUTES FROM THE DEVICE'S PRE INTO ITS OWN CONTROLS, as this host holds them.
std::vector<backplane::Route> selfRoutesOf(const backplane::DeviceHost& host, DeviceInstanceId id) {
	std::vector<backplane::Route> out;
	for (size_t row = 0; row < device::kSiteExportRows; ++row) {
		const backplane::Source s = backplane::Source::exported(id, row);
		struct Ctx {
			const backplane::DeviceHost* host;
			backplane::Source s;
			DeviceInstanceId id;
			std::vector<backplane::Route>* out;
		} ctx { &host, s, id, &out };
		host.forEachRouteFrom(s, &ctx, [](void* c, backplane::Destination d) {
			auto* x = static_cast<Ctx*>(c);
			if (d.isDevice() && d.device == x->id) {
				x->out->push_back({ x->s, d, x->host->routeAmount(x->s, d), x->host->routeTransfer(x->s, d) });
			}
		});
	}
	return out;
}

} // namespace

std::vector<uint8_t> writeDevicePreset(const device::Descriptor& desc, const std::vector<float>& field,
                                       std::vector<uint8_t> body, const DevicePresetSite* site) {
	return writeOne(desc, field, std::move(body), site);
}

size_t devicePresetRowWidth(const device::Descriptor& desc) { return rowWidth(desc); }

bool captureDevicePreset(const backplane::DeviceHost& host, const device::Descriptor& desc, DeviceInstanceId id,
                         std::vector<uint8_t>* out) {
	out->clear();
	std::vector<float> field(rowWidth(desc));
	for (size_t i = 0; i < field.size(); ++i) {
		field[i] = host.fieldAt(id, i);
	}
	std::vector<uint8_t> body;
	// A CONTAINER'S CHAINS OR A LEAF'S OWN BODY (Featherverb's IR): `hasBody` and `bodySaver` answer for either.
	if (desc.hasBody()) {
		if (const void* surface = host.containerSurfaceOf(id); surface != nullptr) {
			if (!rack::bodyOf(&desc, surface, &body)) {
				return false;
			}
		}
		else {
			const std::span<const uint8_t> stored = host.containerBodyOf(id);
			body.assign(stored.begin(), stored.end());
		}
	}
	// ITS SITE, when it has one: the running site's bytes (current only there while its panel edits it) or the filed ones,
	// POST's reads of PRE re-pointed at the preset's own device, and PRE's routes into this device's controls.
	std::optional<DevicePresetSite> site;
	if (host.hasSite(id)) {
		std::vector<uint8_t> bytes;
		if (const void* surface = host.containerSurfaceOf(siteKeyOf(id)); surface != nullptr) {
			if (!rack::bodyOf(&rack::siteDescriptor(), surface, &bytes)) {
				return false;
			}
		}
		else {
			const std::span<const uint8_t> stored = host.containerBodyOf(siteKeyOf(id));
			bytes.assign(stored.begin(), stored.end());
		}
		const std::optional<std::vector<uint8_t>> owned = siteBodyOwnedBy(bytes, id, kFirstInstance);
		if (!owned.has_value()) {
			return false;
		}
		site.emplace();
		site->body = *owned;
		for (const backplane::Route& r : selfRoutesOf(host, id)) {
			if (r.destination.index < desc.params.size() && desc.params[r.destination.index].key != nullptr) {
				site->routes.push_back({ r.source.index, desc.params[r.destination.index].key, r.amount, r.transfer });
			}
		}
	}
	*out = writeOne(desc, field, std::move(body), site.has_value() ? &*site : nullptr);
	return !out->empty();
}

std::vector<uint8_t> initDevicePreset(const device::Descriptor& desc) {
	std::vector<float> field(rowWidth(desc), 0.0f);
	for (size_t i = 0; i < desc.params.size() && i < field.size(); ++i) {
		field[i] = static_cast<float>(desc.params[i].def);
	}
	return writeOne(desc, field, {});
}

bool readDevicePreset(std::span<const uint8_t> bytes, DevicePresetContents* out, const device::Descriptor* desc,
                      rack::LoadReport* report) {
	if (bytes.empty()) {
		return false;
	}
	SpanSource source { bytes, 0 };
	const clap_istream stream { &source, &SpanSource::read };
	rack::RackPatch patch;
	// EXACTLY ONE DEVICE AND NOTHING ELSE THAT WOULD NEED A HOST TO MEAN ANYTHING. A patch with several devices
	// is a rack, not a preset — loading "the first device of it" would be a guess. ITS ONLY ROUTES are its site's PRE
	// into its own controls (2026-10-29): from the device's own export to the device, and only with a site.
	if (!rack::parseRack(&stream, &patch, report) || source.at != bytes.size() || !patch.present
	    || patch.device.size() != 1) {
		return false;
	}
	const rack::RackPatch::Device& d = patch.device.front();
	for (const rack::RackPatch::Route& route : patch.route) {
		const backplane::Source s { route.source.a, route.source.b };
		if (!d.hasSite || route.source.kind != static_cast<uint32_t>(rack::SourceKind::Export) || route.source.a != d.instanceId
		    || !s.isSiteExport() || s.isOuter() || route.destination.kind != static_cast<uint32_t>(rack::DestKind::Device)
		    || route.destination.a != d.instanceId) {
			return false;
		}
	}
	out->tag.assign(d.tag.data(), strnlen(d.tag.data(), device::kTagBytes));
	out->device = d;
	out->body = d.body;
	out->site.reset();
	if (d.hasSite) {
		out->site.emplace();
		// RE-POINTED AT THE FILE'S OWN ID FOR THE DEVICE, which a capture made `kFirstInstance`; a load re-points it again.
		out->site->body = d.site;
		for (const rack::RackPatch::Route& route : patch.route) {
			out->site->routes.push_back({ route.source.b, route.destination.key, route.amount, route.transfer });
		}
	}
	out->field.clear();
	// THE ROW, WHEN THE CALLER SAYS WHICH DEVICE IT IS FOR — by key, so a control this build inserted takes
	// its default and every other keeps its value; what did not load as saved goes in `report`.
	if (desc != nullptr && sameTag(out->tag.c_str(), desc->traits.tag)) {
		out->field.assign(rowWidth(*desc), 0.0f);
		const size_t present = rack::rowOfValues(*desc, d, out->field, report, desc->traits.name, nullptr);
		out->field.resize(present);
	}
	return true;
}

DevicePresetLoad applyDevicePreset(backplane::DeviceHost& host, const device::Descriptor& desc, DeviceInstanceId id,
                                   std::span<const uint8_t> bytes, std::span<const uint8_t> keep,
                                   rack::LoadReport* report) {
	DevicePresetContents preset;
	rack::LoadReport scratch;
	if (!readDevicePreset(bytes, &preset, &desc, &scratch)) {
		if (report != nullptr) {
			report->merge(scratch);
		}
		return DevicePresetLoad::Malformed;
	}
	if (!sameTag(preset.tag.c_str(), desc.traits.tag)) {
		return DevicePresetLoad::OtherDevice;
	}
	if (report != nullptr) {
		report->merge(scratch);
	}
	// **THE DEPTH GATE, BEFORE ANYTHING IS APPLIED** (2026-10-31): the preset's chains, and what stands inside them, must
	// fit where this device stands — or none of it loads, rather than all of it live and the deep end empty after a
	// reload. The body and the site are each at least a chain tall.
	if ((desc.container != nullptr && !host.holdsBody(preset.body))
	    || (preset.site.has_value() && !host.holdsBody(preset.site->body))) {
		return DevicePresetLoad::TooDeep;
	}
	// **THE BODY FIRST, because it is the half that can be refused.** Fields cannot be — `setField`
	// coerces — so applying them first would leave a device half-loaded whenever a chain said no.
	if (desc.container != nullptr) {
		void* surface = host.containerSurfaceOf(id);
		if (surface == nullptr || desc.container->applyState == nullptr
		    || !desc.container->applyState(surface, preset.body.data(), preset.body.size())) {
			return DevicePresetLoad::BodyRefused;
		}
	}
	else if (desc.body != nullptr) {
		// A LEAF'S BODY — Featherverb's IR, as a filename — loaded into the running instance, on this (the message)
		// thread, which is where its loader reads the file. An EMPTY body is the device's own start: "Init".
		void* surface = host.containerSurfaceOf(id);
		if (surface == nullptr || desc.body->loadState == nullptr
		    || !desc.body->loadState(surface, preset.body.data(), preset.body.size())) {
			return DevicePresetLoad::BodyRefused;
		}
	}
	// **ITS SITE, REPLACED WHOLE, when the preset carries one** — before the fields, as the body is, because a host can
	// refuse it. A preset without a site leaves the device's site and its PRE routes alone (the header says why).
	if (preset.site.has_value()) {
		const std::optional<std::vector<uint8_t>> owned = siteBodyOwnedBy(preset.site->body, kFirstInstance, id);
		if (!owned.has_value() || !host.loadSite(id, *owned)) {
			return DevicePresetLoad::BodyRefused;
		}
		for (const backplane::Route& r : selfRoutesOf(host, id)) {
			host.setRouteAmount(r.source, r.destination, 0.0f);
		}
		for (const DevicePresetSite::Route& r : preset.site->routes) {
			const size_t control = controlOfKey(desc, r.key);
			if (control == SIZE_MAX) {
				continue; // a control this build no longer has: the route has nowhere to land
			}
			const backplane::Source s { id, r.index };
			const backplane::Destination dst = backplane::Destination::control(id, control);
			host.setRouteAmount(s, dst, r.amount);
			if (r.transfer != device::Transfer::Lin && host.supportsRouteTransfers()) {
				(void)host.setRouteTransfer(s, dst, r.transfer);
			}
		}
	}
	const auto kept = [&](size_t control) {
		if (desc.traits.enableControl >= 0 && static_cast<size_t>(desc.traits.enableControl) == control) {
			return true;
		}
		return std::find(keep.begin(), keep.end(), control) != keep.end();
	};
	const size_t width = rowWidth(desc);
	for (size_t i = 0; i < width; ++i) {
		if (kept(i)) {
			continue;
		}
		const float def = i < desc.params.size() ? static_cast<float>(desc.params[i].def) : 0.0f;
		host.setField(id, i, i < preset.field.size() ? preset.field[i] : def);
	}
	// THE BODY WENT IN THROUGH THE SURFACE, which no host verb carried — so the host is told (backlog JV).
	if (desc.hasBody()) {
		host.bodyEdited(id);
	}
	return DevicePresetLoad::Loaded;
}

std::string devicePresetBank(const device::Descriptor& desc) {
	return std::string("Devices/") + PresetStore::sanitise(desc.traits.tag);
}

} // namespace feathers
