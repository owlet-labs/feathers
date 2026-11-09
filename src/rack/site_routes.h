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

#include "backplane/container_bodies.h"
#include "backplane/endpoint.h"
#include "device/registry.h"

#include <string>

// **A HOST'S ROUTE LIST WALKS ITS DEVICES' SITES TOO** (backlog NR). The host's inner-route verbs (`DeviceHost::
// innerRoutesFrom` and its five siblings) were written over containers in the chain: a key is a device, its descriptor's
// `ContainerOps` the road in. A route from a host source into a device in a site's PRE or POST went nowhere a list could
// see. A site is a container in the same carrier, filed under `siteKeyOf(device)`, with a table of its own
// (`siteDescriptor()`), so a site is walked by handing the same helpers that key and that table — one road, as
// `siteDescriptor`'s own note says.

namespace feathers::rack {

/// Whether `key` names a site (`siteKeyOf`) rather than a device.
[[nodiscard]] constexpr bool isSiteKey(DeviceInstanceId key) noexcept {
	return key != kNoInstance && (key & kSiteKeyBit) != 0;
}

/// THE TABLE A CONTAINER KEY IS WALKED WITH: a site's key, the site's table; any other key, the device's own
/// descriptor `own` (which the caller looked up by that key).
[[nodiscard]] const device::Descriptor* containerTableFor(DeviceInstanceId key, const device::Descriptor* own) noexcept;

/// **WHAT A ROUTE LIST CALLS A SITE'S LANE**: `holder` (the host's name for the device, "Filter 2") and which lane of the
/// running site `surface` holds `inner`'s device — "Filter 2 PRE" or "Filter 2 POST".
[[nodiscard]] std::string siteRoutePrefix(const void* surface, std::string holder, backplane::Destination inner);

} // namespace feathers::rack
