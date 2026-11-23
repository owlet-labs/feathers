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

#pragma once

#include "backplane/device_host.h"
#include "device/registry.h"
#include "rack/export_table.h"
#include "rack/inner_chain.h"
#include "rack/parallel_rack.h"
#include "rack/rack_bridge.h"
#include "rack/rack_device.h"

#include <vector>

namespace feathers::rack {

// **A CONTAINER'S OWN MACROS' ROUTES, AS ITS HOST'S LIST READS THEM.** A rack's (or a chain's) macro n reaches devices inside it through the
// chains' own route tables, keyed by each chain's own macro n, not by a source the host spells. The host sees that macro as
// `Source::containerExported(holder, row)` (what arming it from the host's editor arms), and the row's inner source is the container's own
// `ExportTable::source(row)`. These read and write the routes of that inner source, through each chain's own bridge and gate, so a host's
// route list shows rows for an armed container macro. Message thread. Every function declines (none, zero, false) for a source that is not
// a live export of a Rack or a Chain, and so cannot reach anything else.

namespace local_routes {

/// The container's inner source behind `source`, or `nobody` (not a live container export of a container this knows).
[[nodiscard]] inline backplane::Source innerOf(const device::Descriptor* d, void* surface, backplane::Source source) {
	if (d == nullptr || surface == nullptr || !source.isContainerExport() || !device::exportsContainer(*d)
	    || d->container->exportedSource == nullptr) {
		return backplane::Source::nobody();
	}
	return d->container->exportedSource(surface, source.exportRow());
}

/// The chain bridges the container holds: a Rack's, one per chain, or a Chain's own.
[[nodiscard]] inline std::vector<RackBridge*> bridgesOf(const device::Descriptor* d, void* surface) {
	std::vector<RackBridge*> out;
	if (d == nullptr || surface == nullptr) {
		return out;
	}
	if (device::sameTag(d->traits.tag, parallel::kTraits.tag)) {
		parallel::Rack* rack = parallel::rackOf(surface);
		for (size_t i = 0; rack != nullptr && i < rack->chainCount(); ++i) {
			if (rack->chainAt(i) != nullptr && rack->chainAt(i)->bridge() != nullptr) {
				out.push_back(rack->chainAt(i)->bridge());
			}
		}
	}
	else if (device::sameTag(d->traits.tag, kDeviceDescriptor.traits.tag)) {
		if (RackBridge* bridge = bridgeOf(surface)) {
			out.push_back(bridge);
		}
	}
	return out;
}

/// Every route inside the container from its own macro behind `source`; each row's `chain` is the holder, its destination in the chains' id space.
[[nodiscard]] inline std::vector<backplane::DeviceHost::InnerRoute> routesFrom(const device::Descriptor* d, void* surface,
                                                                              DeviceInstanceId holder, backplane::Source source) {
	std::vector<backplane::DeviceHost::InnerRoute> out;
	const backplane::Source inner = innerOf(d, surface, source);
	if (inner == backplane::Source::nobody()) {
		return out;
	}
	for (const RackBridge* bridge : bridgesOf(d, surface)) {
		bridge->forEachRoute([&](const backplane::Route& r) {
			if (r.source == inner) {
				out.push_back(backplane::DeviceHost::InnerRoute { holder, r.destination, r.amount });
			}
		});
	}
	return out;
}

/// The chain bridge that holds `destination` (a Rack's chains share one id space), or null.
[[nodiscard]] inline RackBridge* holding(const device::Descriptor* d, void* surface, backplane::Destination destination) {
	for (RackBridge* bridge : bridgesOf(d, surface)) {
		if (bridge->descriptorOf(destination.device) != nullptr) {
			return bridge;
		}
	}
	return nullptr;
}

} // namespace local_routes

} // namespace feathers::rack
