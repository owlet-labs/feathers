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

#include "backplane/endpoint.h"
#include "backplane/route_list.h"
#include "plugin/params.h"

#include <cstddef>
#include <memory>
#include <vector>

// **HOW A CASE WRITES ROUTES INTO A `ParamSet` NOW — BY IDENTITY, THE WAY THE EDITOR DOES.**
//
// The suites were written against `ModRoutes`, a dense `sources × destinations` matrix a case filled by
// flat coordinate and handed to `ParamSet::setRoutes`. Kill-lanes §2.4b took the matrix, the coordinates
// and `setRoutes` out: a route is `{Source, Destination, amount}`, the editor publishes a `RouteList`,
// `ParamSet::adoptRoutes` snapshots it, and at a drain the synth's own node builds its inputs from the
// snapshot (`rebuildCoreInputs`). This is that road, in the shape a case wants — a small list a case edits,
// published whole — and it is deliberately the PRODUCT's road rather than a back door into the store.
//
// A CASE WITH NO SYNTH still has a node: `publish` rebuilds the core inputs itself, with whatever device
// cells the case hands in (`DeviceCells`), so a source that is a device output can be a float the case
// owns. That is what `setDeviceSource(lane, output, value)` used to be for.
namespace feathers::test {

struct Routes {
	/// THE LIST, OWNED THROUGH A POINTER so a `Routes` can be returned from a helper — `RouteList` itself is
	/// pinned in place because a reader may be snapshotting it.
	std::unique_ptr<backplane::RouteList> list = std::make_unique<backplane::RouteList>();
	std::vector<backplane::Route> pending;

	/// One route, replacing an earlier one between the same ends. Zero deletes it, as `Graph::route` does.
	void set(backplane::Source s, backplane::Destination d, float amount) {
		for (size_t i = 0; i < pending.size(); ++i) {
			if (pending[i].source == s && pending[i].destination == d) {
				if (amount == 0.0f) {
					pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(i));
				}
				else {
					pending[i].amount = amount;
				}
				return;
			}
		}
		if (amount != 0.0f) {
			pending.push_back(backplane::Route { s, d, amount });
		}
	}

	/// THE CURVE ON A ROUTE ALREADY SET — false for none, as
	/// `Graph::setTransfer`.
	bool setTransfer(backplane::Source s, backplane::Destination d, backplane::Transfer t) {
		for (backplane::Route& r : pending) {
			if (r.source == s && r.destination == d) {
				r.transfer = t;
				return true;
			}
		}
		return false;
	}

	[[nodiscard]] float at(backplane::Source s, backplane::Destination d) const {
		for (const backplane::Route& r : pending) {
			if (r.source == s && r.destination == d) {
				return r.amount;
			}
		}
		return 0.0f;
	}

	/// **PUBLISH WHOLE AND ADOPT** — the editor's publish and the engine's snapshot in one call — then
	/// rebuild the params' own node against `cells` (none by default: a route from a device source then
	/// places nowhere, exactly as it would in a synth not running that device).
	///
	/// **A FRESH LIST'S SEQUENCE CAN COLLIDE WITH THE ONE THE PARAMS LAST ADOPTED**, and `adoptRoutes` would
	/// rightly see "nothing moved" — the editor's one list never restarts, but a case's second `Routes`
	/// does. So the publish repeats until its sequence is one the params has not seen; the list is
	/// republished whole each time and the routes do not change.
	bool publish(ParamSet& params, const ParamSet::DeviceCells& cells = {}) {
		do {
			list->begin();
			for (const backplane::Route& r : pending) {
				list->add(r);
			}
			list->end();
		} while (list->sequence() == params.routesGeneration());
		const bool adopted = params.adoptRoutes(*list);
		params.rebuildCoreInputs(cells);
		return adopted;
	}
};

inline backplane::Source macro(size_t index) {
	return backplane::Source::core(index);
}

/// A synth parameter as a destination — `Destination::core` of its place in the destination list.
inline backplane::Destination param(ParamId id) {
	const int at = modDestinationIndexOf(id);
	return at < 0 ? backplane::Destination::nobody() : backplane::Destination::core(static_cast<size_t>(at));
}

/// HOW FAR `s` REACHES `d` IN WHAT THE PARAMS ADOPTED — read back from the snapshot, which is what the
/// nodes' inputs are built from. The dense table's `at` answered this for the compiled store.
inline float adoptedAmount(const ParamSet& params, backplane::Source s, backplane::Destination d) {
	for (const backplane::Route& r : params.stagedRoutes()) {
		if (r.source == s && r.destination == d) {
			return r.amount;
		}
	}
	return 0.0f;
}

/// **A CELL A CASE OWNS, STANDING IN FOR A DEVICE'S OUTPUT** — the `DeviceCells` that answers every
/// device source with the same float. What `setDeviceSource(lane, output, value)` was: a number the
/// resolve reads for a device source, without a device.
inline ParamSet::DeviceCells oneCell(const float* cell) {
	return ParamSet::DeviceCells { cell, [](const void* h, DeviceInstanceId, size_t) { return static_cast<const float*>(h); },
		                           nullptr, 0 };
}

} // namespace feathers::test
