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

#include "device/registry.h"
#include "rack/inner_chain.h"
#include "rack/inner_chain_bundle.h"
#include "rack/rack_bridge.h"

#include <cstddef>
#include <cstdint>
#include <tuple>

// **A DEVICE THAT HOLDS SEVERAL FX LANES, AS ONE `device::ContainerOps` TABLE** — backlog LC's step 1 ("extract the
// post-fx pattern"), taken as far as Resonate's two lanes needed it (2026-10-05).
//
// SKLITTER's `kContainerOps` (sklitter.cpp) is a table of one-line forwards over its two `rack::InnerChain`s: the
// fan-out verbs to every lane, the body as one `rack::writeChainBundle`, and the four route verbs to the lane that
// holds the inner device (`laneHolding`, sound because the lanes share one instance counter — gap G3). Nothing in it
// is SKLITTER's but the list of lanes. This is that table, written once over a holder's own `LanesOf`:
//
//     std::array<rack::BundleLane, N> lanesOf(void* storage);   // stable ids, the holder's chains (null allowed)
//     inline constexpr device::ContainerOps kContainerOps = rack::laneContainerOps<&lanesOf>();
//
// **THE HOLDER STILL OWES THREE THINGS THIS CANNOT DO FOR IT**: one `backplane::InstanceCounter` shared by every lane
// (passed to each `InnerChain`, behind a pointer if the holder is movable); the AUDIO half — prepare, reset, the
// clock, the note context, the outer sources and the chains' `process` where they sit in its signal; and the
// latency its signal makes of theirs. SKLITTER is NOT moved onto this yet (it predates it, and its table is
// verified as it stands); the next holder — Featherverb, backlog LC — should take this one.

namespace feathers::rack {

namespace lanes_detail {

template <auto LanesOf>
constexpr size_t laneCount() {
	return std::tuple_size_v<decltype(LanesOf(static_cast<void*>(nullptr)))>;
}

/// THE LANE THAT HOLDS INNER DEVICE `id`, or null — see the banner.
template <auto LanesOf>
InnerChain* holding(void* storage, DeviceInstanceId id) {
	for (const BundleLane& lane : LanesOf(storage)) {
		if (lane.chain != nullptr && lane.chain->bridge() != nullptr && lane.chain->bridge()->descriptorOf(id) != nullptr) {
			return lane.chain;
		}
	}
	return nullptr;
}

} // namespace lanes_detail

/// THE TABLE. `LanesOf` is a function `std::array<BundleLane, N>(void* storage)`.
template <auto LanesOf>
constexpr device::ContainerOps laneContainerOps() {
	using lanes_detail::holding;
	return device::ContainerOps {
		.adopt =
		    [](void* storage, const device::Catalogue& catalogue) {
			    for (const BundleLane& lane : LanesOf(storage)) {
				    if (lane.chain != nullptr) {
					    lane.chain->adopt(catalogue);
				    }
			    }
		    },
		// EVERY LANE IN ONE BODY, by stable id (`rack::writeChainBundle`).
		.saveState =
		    [](const void* storage, const device::ByteSink& sink) {
			    const auto lanes = LanesOf(const_cast<void*>(storage));
			    return writeChainBundle(lanes, sink);
		    },
		// ALL OR NOTHING ACROSS THEM (`rack::readChainBundle`).
		.loadState =
		    [](void* storage, const uint8_t* data, size_t bytes) {
			    const auto lanes = LanesOf(storage);
			    return readChainBundle(lanes, data, bytes);
		    },
		.maxBodyBytes = bundleMaxBytes(lanes_detail::laneCount<LanesOf>()),
		.setOuterGate =
		    [](void* storage, const device::OuterGate& gate) {
			    for (const BundleLane& lane : LanesOf(storage)) {
				    if (lane.chain != nullptr) {
					    lane.chain->setOuterGate(gate);
				    }
			    }
		    },
		// EACH LANE STAMPED WITH ITS OWN `lane`, in the order the holder lists them (SKLITTER's rule).
		.setHostSlots =
		    [](void* storage, const device::HostSlotLink& link) {
			    uint8_t index = 0;
			    for (const BundleLane& lane : LanesOf(storage)) {
				    if (lane.chain != nullptr) {
					    device::HostSlotLink stamped = link;
					    stamped.lane = index;
					    lane.chain->setHostSlots(stamped);
				    }
				    ++index;
			    }
		    },
		// THE DROPS MERGED: counts add, and the first pair is the first lane's that dropped anything (gap G7).
		.service =
		    [](void* storage) {
			    device::RouteDrop merged {};
			    for (const BundleLane& lane : LanesOf(storage)) {
				    if (lane.chain == nullptr) {
					    continue;
				    }
				    const device::RouteDrop one = lane.chain->service();
				    if (one.count == 0) {
					    continue;
				    }
				    if (merged.count == 0) {
					    merged.firstSource = one.firstSource;
					    merged.firstDestination = one.firstDestination;
				    }
				    merged.count += one.count;
			    }
			    return merged;
		    },
		.setVoices =
		    [](void* storage, size_t voices) {
			    for (const BundleLane& lane : LanesOf(storage)) {
				    if (lane.chain != nullptr) {
					    lane.chain->setVoices(voices);
				    }
			    }
		    },
		.forEachRouteFrom =
		    [](const void* storage, device::Source outer, void* ctx,
		       void (*fn)(void* ctx, device::Destination inner, float amount)) {
			    for (const BundleLane& lane : LanesOf(const_cast<void*>(storage))) {
				    if (lane.chain != nullptr) {
					    lane.chain->forEachRouteFrom(outer, ctx, fn);
				    }
			    }
		    },
		.routeAmountFrom =
		    [](const void* storage, device::Source outer, device::Destination inner) {
			    const InnerChain* c = holding<LanesOf>(const_cast<void*>(storage), inner.device);
			    return c != nullptr ? c->routeAmountFrom(outer, inner) : 0.0f;
		    },
		.setRouteFrom =
		    [](void* storage, device::Source outer, device::Destination inner, float amount) {
			    InnerChain* c = holding<LanesOf>(storage, inner.device);
			    return c != nullptr && c->setRouteFrom(outer, inner, amount);
		    },
		.routeTransferFrom =
		    [](const void* storage, device::Source outer, device::Destination inner) {
			    const InnerChain* c = holding<LanesOf>(const_cast<void*>(storage), inner.device);
			    return c != nullptr ? c->routeTransferFrom(outer, inner) : device::Transfer::Lin;
		    },
		.setRouteTransferFrom =
		    [](void* storage, device::Source outer, device::Destination inner, device::Transfer t) {
			    InnerChain* c = holding<LanesOf>(storage, inner.device);
			    return c != nullptr && c->setRouteTransferFrom(outer, inner, t);
		    },
		.nameDestination =
		    [](const void* storage, device::Destination inner, char* out, size_t capacity) {
			    const InnerChain* c = holding<LanesOf>(const_cast<void*>(storage), inner.device);
			    return c != nullptr ? c->nameDestination(inner, out, capacity) : size_t { 0 };
		    },
		// A PRESET'S ROAD: every lane, all or nothing, through their records while running.
		.applyState =
		    [](void* storage, const uint8_t* data, size_t bytes) {
			    const auto lanes = LanesOf(storage);
			    return applyChainBundle(lanes, data, bytes);
		    },
	};
}

} // namespace feathers::rack
