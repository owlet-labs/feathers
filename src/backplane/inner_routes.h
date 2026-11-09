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

#include "backplane/device_host.h"
#include "backplane/endpoint.h"
#include "device/registry.h"

#include <string>
#include <vector>

// THE ROUTES INSIDE ONE CONTAINER, FROM ONE OF THE HOST'S SOURCES — `DeviceHost`'s four inner roads
// (rack roadmap §7f), restated over what every host has for a container it holds: the descriptor and
// the surface. Feathers' lane and the rack's record walk their containers differently and this is the
// part that is the same, so it is written once. Every function declines — none, zero, false, the
// prefix alone — for a device that is not a container or a surface that is not there.

namespace feathers::backplane {

/// Append every route inside `chain` whose source is `source`.
inline void innerRoutesOf(const device::Descriptor* d, void* surface, DeviceInstanceId chain, Source source,
                          std::vector<DeviceHost::InnerRoute>& out) {
	if (d == nullptr || d->container == nullptr || d->container->forEachRouteFrom == nullptr || surface == nullptr) {
		return;
	}
	struct Ctx {
		std::vector<DeviceHost::InnerRoute>* out;
		DeviceInstanceId chain;
	} ctx { &out, chain };
	d->container->forEachRouteFrom(surface, source, &ctx, [](void* c, Destination inner, float amount) {
		auto* self = static_cast<Ctx*>(c);
		self->out->push_back(DeviceHost::InnerRoute { self->chain, inner, amount });
	});
}

inline float innerRouteAmountOf(const device::Descriptor* d, void* surface, Source source, Destination inner) {
	if (d == nullptr || d->container == nullptr || d->container->routeAmountFrom == nullptr || surface == nullptr) {
		return 0.0f;
	}
	return d->container->routeAmountFrom(surface, source, inner);
}

/// Through the chain's own gate; whether it was taken.
inline bool setInnerRouteOf(const device::Descriptor* d, void* surface, Source source, Destination inner,
                            float amount) {
	if (d == nullptr || d->container == nullptr || d->container->setRouteFrom == nullptr || surface == nullptr) {
		return false;
	}
	return d->container->setRouteFrom(surface, source, inner, amount);
}

inline Transfer innerRouteTransferOf(const device::Descriptor* d, void* surface, Source source, Destination inner) {
	if (d == nullptr || d->container == nullptr || d->container->routeTransferFrom == nullptr || surface == nullptr) {
		return Transfer::Lin;
	}
	return d->container->routeTransferFrom(surface, source, inner);
}

/// Through the chain's own record; whether it was taken (refused for a route that is not there).
inline bool setInnerRouteTransferOf(const device::Descriptor* d, void* surface, Source source, Destination inner,
                                    Transfer t) {
	if (d == nullptr || d->container == nullptr || d->container->setRouteTransferFrom == nullptr || surface == nullptr) {
		return false;
	}
	return d->container->setRouteTransferFrom(surface, source, inner, t);
}

/// `prefix` is how the host names the chain ("Chain 2"); the rest is as the chain spells it.
inline std::string innerNameOf(const device::Descriptor* d, void* surface, Destination inner, std::string prefix) {
	if (d == nullptr || d->container == nullptr || d->container->nameDestination == nullptr || surface == nullptr) {
		return prefix;
	}
	char buffer[96] = {};
	if (d->container->nameDestination(surface, inner, buffer, sizeof(buffer)) > 0) {
		prefix += " · ";
		prefix += buffer;
	}
	return prefix;
}

/// **WHAT THE CONTAINER CALLS ITS MACRO `macro` RIGHT NOW** (backlog OX) — the name its own window renames, read from the
/// running instance rather than the host's stored body (which moves only when the host saves). Empty for a macro with no
/// name of its own, a device that is no container, or no surface: the caller then prints the default.
inline std::string innerMacroLabelOf(const device::Descriptor* d, void* surface, size_t macro) {
	if (d == nullptr || d->container == nullptr || d->container->macroLabel == nullptr || surface == nullptr) {
		return {};
	}
	char buffer[96] = {};
	const size_t n = d->container->macroLabel(surface, macro, buffer, sizeof(buffer));
	return std::string(buffer, n);
}

} // namespace feathers::backplane
