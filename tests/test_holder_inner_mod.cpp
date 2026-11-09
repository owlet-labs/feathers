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

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "fx/comp/comp_lanes.h"
#include "fx/comp/comp_types.h"
#include "fx/delay/delay_lanes.h"
#include "fx/delay/delay_types.h"
#include "fx/resonate/resonate_lanes.h"
#include "fx/resonate/resonate_types.h"
#include "fx/sklitter/sklitter.h"
#include "fx/sklitter/sklitter_types.h"
#include "mod/lfo/lfo_types.h"
#include "plugin/device_registry.h"
#include "rack/inner_chain.h"
#include "rack/parallel_rack.h"
#include "rack/rack_bridge.h"
#include "rack/rack_device.h"

#include <algorithm>
#include <cstdint>
#include <new>
#include <string>
#include <string_view>
#include <vector>

// **BACKLOG ME: A MODULATOR INSIDE A HOLDER'S INNER CHAIN REACHING THE HOLDER'S OWN CONTROLS** (the
// requirement: modulators in the chain can modulate the Delay's parameters; SKLITTER looked like a regression, and the
// other effects were to be audited too).
//
// **THE REGRESSION WAS NOT ONE: THE ROAD HAS NEVER EXISTED** (measured 2026-10-17, ME's entry has the evidence). Both
// holders' chain axes declare `coreDestinations = 0` and have since each chain was born, so the inner graph refuses
// `output(innerLfo) -> core(c)` — the only spelling the route vocabulary has for "the holder's control c" from inside
// — and nothing in `RackEngine` or `device::ContainerOps` would carry such a modulation out to the holder if it were
// admitted. What does exist, and is green, is backlog HS: SKLITTER's own PERFORM outputs reaching its own controls,
// pinned by value in test_rack_engine ("HS: Perf X routed to SKLITTER's own Rate ARRIVES …") and test_synth.
//
// So this file holds three things, and the table at the top is what makes them an AUDIT rather than two anecdotes:
//
//   1. the table is COMPLETE — every registered device that holds a chain is in it, so the Delay joins the day it
//      gains its chains (a holder left out of the table fails here, not silently);
//   2. THE REQUIREMENT, written as it will be asserted once the road is built, decorated `may_fail` because it is
//      a known gap and not a regression: admitted, kept across a chain rebuild, kept across a save/load;
//   3. THE GAP AS MEASURED, undecorated: every lane of every holder refuses the route today. The day the road is
//      built this goes red on purpose; delete it, and take `may_fail` off the requirement in the same commit.
//
// **WHAT THE REQUIREMENT DOES NOT YET ASSERT, AND WHY.** "The holder's control moves at the engine" is a claim about
// a VALUE (CLAUDE.md), and there is no seam that could report it, because there is no road for it to travel. The
// design sketch in ME names the seam the road brings with it (the chain's per-tick sum for a core destination);
// that is the assertion to add to case 2 when it exists.

using namespace feathers;

namespace {

constexpr double kRate = 48000.0;

/// ONE CHAIN A HOLDER HOLDS, reached through the accessor the holder publishes for its editor.
struct LaneRow {
	const char* name;
	rack::InnerChain* (*of)(void* storage);
};

/// **EVERY DEVICE THAT HOLDS A `rack::InnerChain`, AND EVERY LANE IT HOLDS.** A new holder adds one row here, and
/// case 1 refuses to pass until it does.
struct HolderRow {
	const device::Descriptor* desc;
	std::vector<LaneRow> lanes;
};

const std::vector<HolderRow>& holders() {
	static const std::vector<HolderRow> rows {
		{ &fx::sklitter::kDescriptor,
		  { { "POST FX", [](void* s) { return fx::sklitter::postChainOf(s); } },
		    { "GRAIN FX", [](void* s) { return fx::sklitter::grainChainOf(s); } } } },
		{ &fx::resonate::kDescriptor,
		  { { "DRONE FX", [](void* s) { return fx::resonate::laneChainOf(s, fx::resonate::Lane::Drone); } },
		    { "WET FX", [](void* s) { return fx::resonate::laneChainOf(s, fx::resonate::Lane::Wet); } } } },
		// THE DELAY (delay-love S1): one FEEDBACK FX lane, LAZY — so its row is the accessor that builds it.
		{ &fx::delay::kDescriptor, { { "FEEDBACK FX", [](void* s) { return fx::delay::ensureFeedbackLane(s); } } } },
		// COMP (backlog QG): one Compander lane, LAZY — the Delay's component, so the Delay's kind of row.
		{ &fx::comp::kDescriptor, { { "COMPANDER", [](void* s) { return fx::comp::ensureCompanderLane(s); } } } },
		// THE RACK (backlog NO): N chains; a fresh one holds one, which is the row.
		{ &rack::parallel::kDescriptor, { { "CHAIN 1", [](void* s) { return rack::parallel::rackOf(s)->chainAt(0); } } } },
	};
	return rows;
}

/// **THE ONE CONTAINER THAT IS NOT AN `InnerChain` HOLDER**, and the reason it is left out of the table rather than
/// forgotten: the rack device (`rack_device.cpp`) is the chain's own body, written before `rack::InnerChain` and
/// deliberately not ported onto it (inner_chain.h's banner). Its outward controls are its MACROS, which are its
/// chain's core SOURCES — published inward, the other direction from the one this file asks about.
bool isTheRackDevice(const device::Descriptor& d) {
	return std::string_view(d.traits.tag) == std::string_view(rack::kDeviceDescriptor.traits.tag);
}

/// The holder's first modulatable control — the destination the requirement routes to.
size_t firstModulatable(const device::Descriptor& d) {
	for (size_t i = 0; i < d.params.size(); ++i) {
		if (d.params[i].modulatable) {
			return i;
		}
	}
	FAIL("holder " << d.traits.name << " declares no modulatable control");
	return 0;
}

/// ONE INSTANCE OF A HOLDER, through its own ops, adopted with the host's catalogue so its lanes take devices.
struct Instance {
	const device::Descriptor& desc;
	void* mem = nullptr;

	explicit Instance(const device::Descriptor& d) : desc(d) {
		mem = ::operator new(desc.audio->stateBytes, std::align_val_t { desc.audio->stateAlign });
		desc.audio->construct(mem);
		desc.container->adopt(mem, hostCatalogue());
		desc.audio->prepare(mem, kRate, 4096);
	}
	~Instance() {
		desc.audio->destruct(mem);
		::operator delete(mem, std::align_val_t { desc.audio->stateAlign });
	}
	Instance(const Instance&) = delete;
	Instance& operator=(const Instance&) = delete;

	/// Re-asked every time: a contemporaneous handle, never an identity (trap T9).
	rack::RackBridge& bridge(const LaneRow& lane) const {
		rack::InnerChain* chain = lane.of(mem);
		REQUIRE(chain != nullptr);
		REQUIRE(chain->bridge() != nullptr);
		return *chain->bridge();
	}
};

/// **HOW MANY ROUTES IN THIS LANE LAND ON `dst` FROM AN INNER DEVICE AT `amount`** — counted rather than looked up by
/// the LFO's id, so a load that re-issues ids cannot make the route look lost.
size_t routesOnto(const rack::RackBridge& b, backplane::Destination dst, float amount) {
	size_t n = 0;
	b.forEachRoute([&](const backplane::Route& r) {
		if (r.source.isDevice() && r.destination == dst && r.amount == amount) {
			++n;
		}
	});
	return n;
}

std::vector<uint8_t> saveBody(const Instance& in) {
	std::vector<uint8_t> body;
	const device::ByteSink sink { &body, [](void* ctx, const uint8_t* d, size_t n) {
		                             auto* out = static_cast<std::vector<uint8_t>*>(ctx);
		                             out->insert(out->end(), d, d + n);
	                             } };
	REQUIRE(in.desc.container->saveState(in.mem, sink));
	return body;
}

constexpr float kAmount = 0.5f;

} // namespace

TEST_CASE("backlog ME: the holder table is COMPLETE — every registered device that holds an inner chain is in it, "
          "with a lane row for each chain") {
	std::vector<std::string> registered;
	for (const device::Descriptor& d : kDevices) {
		if (d.container != nullptr && !isTheRackDevice(d)) {
			registered.emplace_back(d.traits.tag);
		}
	}
	std::vector<std::string> tabled;
	for (const HolderRow& h : holders()) {
		tabled.emplace_back(h.desc->traits.tag);
		CHECK(!h.lanes.empty());
	}
	std::sort(registered.begin(), registered.end());
	std::sort(tabled.begin(), tabled.end());
	// A HOLDER MISSING HERE IS A HOLDER THE AUDIT BELOW NEVER ASKS — the Delay, when ME's first part is built.
	CHECK(registered == tabled);
	// Not vacuous: the two holders the backlog names are among them.
	CHECK(registered.size() >= 2);

	// EVERY LANE ROW REACHES A REAL, ADOPTED CHAIN — a row whose accessor answered null would be skipped silently.
	for (const HolderRow& h : holders()) {
		Instance in(*h.desc);
		for (const LaneRow& lane : h.lanes) {
			const std::string holder = h.desc->traits.name;
			CAPTURE(holder);
			const std::string laneName = lane.name;
			CAPTURE(laneName);
			rack::InnerChain* chain = lane.of(in.mem);
			REQUIRE(chain != nullptr);
			CHECK(chain->bridge() != nullptr);
		}
	}
}

TEST_CASE("backlog ME (KNOWN GAP, may_fail): an LFO inside a holder's inner chain reaches the HOLDER's own control — "
          "admitted, kept across a chain rebuild and a save/load"
          * doctest::may_fail()) {
	// MAY_FAIL BECAUSE THE ROAD HAS NEVER BEEN BUILT (backlog ME, measured 2026-10-17), not because it broke. When it
	// is built: take `may_fail` off, delete the GAP case below, and add the VALUE assertion this case still lacks.
	for (const HolderRow& h : holders()) {
		const size_t control = firstModulatable(*h.desc);
		const backplane::Destination dst = backplane::Destination::core(control);
		for (const LaneRow& lane : h.lanes) {
			const std::string holder = h.desc->traits.name;
			CAPTURE(holder);
			const std::string laneName = lane.name;
			CAPTURE(laneName);
			Instance in(*h.desc);
			rack::RackBridge& b = in.bridge(lane);
			const DeviceInstanceId lfo = b.addDevice(mod::lfo::kTraits.tag);
			REQUIRE(lfo != kNoInstance);
			const backplane::Source src = backplane::Source::output(lfo, 0);
			b.setRouteAmount(src, dst, kAmount);
			b.drain();

			// ADMITTED: the route reads back from the authority that would run it.
			CHECK(b.routeAmount(src, dst) == kAmount);

			// A CHAIN REBUILD: the holder's own body, applied back onto the running instance.
			const std::vector<uint8_t> body = saveBody(in);
			REQUIRE(in.desc.container->applyState != nullptr);
			REQUIRE(in.desc.container->applyState(in.mem, body.data(), body.size()));
			in.bridge(lane).drain();
			CHECK(routesOnto(in.bridge(lane), dst, kAmount) == 1);

			// A SAVE/LOAD into a fresh instance.
			Instance loaded(*h.desc);
			REQUIRE(loaded.desc.container->loadState(loaded.mem, body.data(), body.size()));
			loaded.bridge(lane).drain();
			CHECK(routesOnto(loaded.bridge(lane), dst, kAmount) == 1);
		}
	}
}

TEST_CASE("backlog ME: THE GAP AS MEASURED — every lane of every holder refuses a route from an inner LFO to the "
          "holder's own control, while the same LFO routes to a sibling inside") {
	// RED ON PURPOSE THE DAY THE ROAD IS BUILT: delete this case then, and take `may_fail` off the one above.
	for (const HolderRow& h : holders()) {
		const backplane::Destination dst = backplane::Destination::core(firstModulatable(*h.desc));
		for (const LaneRow& lane : h.lanes) {
			const std::string holder = h.desc->traits.name;
			CAPTURE(holder);
			const std::string laneName = lane.name;
			CAPTURE(laneName);
			Instance in(*h.desc);
			rack::RackBridge& b = in.bridge(lane);
			const DeviceInstanceId lfo = b.addDevice(mod::lfo::kTraits.tag);
			const DeviceInstanceId sibling = b.addDevice(mod::lfo::kTraits.tag);
			REQUIRE(lfo != kNoInstance);
			REQUIRE(sibling != kNoInstance);
			const backplane::Source src = backplane::Source::output(lfo, 0);
			b.setRouteAmount(src, dst, kAmount);
			// THE CONTROL: the same source, the same amount, to the sibling LFO's first modulatable control. Without
			// it a lane that refused EVERY route would pass for one that refuses only the holder.
			const backplane::Destination inner =
			    backplane::Destination::control(sibling, firstModulatable(mod::lfo::kDescriptor));
			b.setRouteAmount(src, inner, kAmount);
			b.drain();
			CHECK(b.routeAmount(src, dst) == 0.0f);
			CHECK(b.routeAmount(src, inner) == kAmount);
		}
	}
}
