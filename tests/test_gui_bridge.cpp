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

// The editor's road to the audio thread, with no window and no host at either end.
//
// Everything here is reachable by calling it, which is the point of the bridge being its own file:
// the parts that are easy to get subtly wrong — a ring buffer's full-versus-empty, a change detector
// that reports everything forever or nothing ever — are exactly the parts that need no GPU to test.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "param_routes.h"
#include "patch_values.h"

#include "plugin/device_registry.h"
#include "mod/mseg/mseg_types.h"
#include "mod/follower/follower_types.h"
#include "mod/lfo/lfo_types.h"
#include "mod/tracker/tracker_types.h"
#include "fx/automod/automod_types.h"
#include "fx/shaper/shaper_types.h"
#include "fx/sklitter/sklitter_types.h"
#include "plugin/device_row.h"
#include "plugin/audition.h"
#include "dsp/slice_build.h"
#include "plugin/character_params.h"
#include "plugin/rotate_sound.h"
#include "plugin/gui_bridge.h"
#include "device/subscribers.h"
#include "backplane/sources.h"
#include "plugin/params.h"
#include "plugin/state_block_order.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <atomic>
#include <limits>
#include <string>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

namespace {
/// THE RACK THESE CASES RESOLVE AGAINST — FULL, not the one-device default.
///
/// A routing now names its device by INSTANCE ID, so saving and loading one needs a rack to resolve
/// against. It is deliberately full rather than `defaultModRack()`, and the difference is not
/// cosmetic: a route into a slot holding NO device has no identity to be written with, so it cannot
/// round-trip and is dropped on save. Several cases here address "the far corner" — the last slot —
/// precisely to exercise the widest addressing, and against a one-device rack that corner is empty
/// and every such case would be asserting the drop rather than the round-trip it was written for.
const feathers::ModRack& rackForTest() {
	static const feathers::ModRack rack = [] {
		feathers::ModRack r = feathers::defaultModRack();
		r.count = feathers::kMaxRackSlots;
		for (size_t slot = 0; slot < feathers::kMaxRackSlots; ++slot) {
			// A SLOT MUST BE GIVEN A DEVICE BEFORE IT CAN BE GIVEN AN IDENTITY. Under the old
			// array-of-Settings rack every slot was implicitly a DAHDSR; now a slot holds whatever it
			// was put there, and a slot with no tag holds nothing.
			r.setDahdsrAt(slot, feathers::mod::dahdsr::Settings {});
			// **PAST THE DEFAULTS, BECAUSE THIS FIXTURE MINTS ITS OWN IDS AND THE SEQUENCE IS SHARED.**
			// It was `slot + 1`, which was safe while each chain counted from one and became a
			// COLLISION the moment they stopped: `defaultFxChain()`'s filter now holds
			// `kDefaultFxInstance`, which is this rack's slot 1 under the old arithmetic. Every case
			// pairing this rack with that lane then had a device two chains both claimed — refused by the
			// loader, as it should be, and four cases went red saying so.
			//
			// The fixture is what was wrong, not the refusal: production ids come from
			// `ParamBridge::nextInstance_` and cannot collide. A fixture that hand-stamps them has to
			// hand-stamp them from the same sequence.
			// AN IDENTITY IS THE SERIAL AND NOTHING ELSE (kill-lanes step 3c). It carried a routing lane
			// beside the serial and the fixture stamped `deviceSlotOfRack(slot)` into it; a route names
			// the instance id now, live and saved alike, so there is no second half to keep in step.
			r.setIdentityAt(slot, { static_cast<feathers::DeviceInstanceId>(feathers::kFirstFreeInstance
			                                                               + slot) });
		}
		r.nextInstanceId =
		    static_cast<feathers::DeviceInstanceId>(feathers::kFirstFreeInstance + feathers::kMaxRackSlots);
		return r;
	}();
	return rack;
}

/// THE SLOTS THESE CASES RESOLVE AGAINST — the fixture rack plus a default lane.
/// THE DEVICE IN THE FIXTURE RACK'S `slot`, by the row's own tag — no pool coordinate in between.
const feathers::device::Descriptor& rackDesc(size_t slot) {
	REQUIRE(slot < rackForTest().count);
	const feathers::device::Descriptor* d = feathers::descriptorForRow(rackForTest().device[slot]);
	REQUIRE(d != nullptr);
	return *d;
}

/// EVERY DEVICE THE FIXTURE CHAINS HOLD, by the id it wears — the strip's first, then the lane's.
std::vector<feathers::DeviceInstanceId> fixtureIds() {
	std::vector<feathers::DeviceInstanceId> ids;
	const feathers::ModRack& rack = rackForTest();
	for (size_t at = 0; at < rack.count && at < rack.device.size(); ++at) {
		ids.push_back(rack.device[at].instanceId);
	}
	const feathers::FxChain chain = feathers::defaultFxChain();
	for (size_t at = 0; at < feathers::fxChainLength(chain); ++at) {
		ids.push_back(feathers::fxDeviceAtPosition(chain, at).instanceId());
	}
	return ids;
}

/// THE DESCRIPTOR OF ANY DEVICE THE FIXTURE CHAINS HOLD, by id — nullptr for an id neither wears.
const feathers::device::Descriptor* fixtureDescOf(feathers::DeviceInstanceId id) {
	const feathers::ModRack& rack = rackForTest();
	for (size_t at = 0; at < rack.count && at < rack.device.size(); ++at) {
		if (rack.device[at].instanceId == id) {
			return feathers::descriptorForRow(rack.device[at]);
		}
	}
	const feathers::FxChain chain = feathers::defaultFxChain();
	for (size_t at = 0; at < feathers::fxChainLength(chain); ++at) {
		if (feathers::fxDeviceAtPosition(chain, at).instanceId() == id) {
			return feathers::descriptorForRow(feathers::fxDeviceAtPosition(chain, at).row);
		}
	}
	return nullptr;
}



/// A ROUTE NAMED BY HAND, in the one format's identity-keyed spelling (`rack/rack_state.h`).
///
/// THE KIND CODES ARE LITERALS HERE, not `rack::SourceKind`/`rack::DestKind` — deliberately. A case
/// that imported the writer's own enum would agree with the writer about what 2 means; these numbers
/// ARE the on-disk contract, and a test that restates them is what notices if someone renumbers them.
struct TestRoute {
	uint32_t sourceKind; ///< 0 macro, 1 weight, 2 device
	uint32_t sourceA;    ///< macro index | weight slot | device instance id
	uint32_t sourceB;    ///< device output index
	uint32_t destKind;   ///< 0 param, 1 device
	uint32_t destA;      ///< ParamId | device instance id
	uint32_t destB;      ///< device control index
	float amount;
};

TestRoute fromMacro(uint32_t macro) { return TestRoute { 0, macro, 0, 0, 0, 0, 0.0f }; }
TestRoute fromWeight(uint32_t slot) { return TestRoute { 1, slot, 0, 0, 0, 0, 0.0f }; }
TestRoute fromDevice(uint32_t instance, uint32_t output) {
	return TestRoute { 2, instance, output, 0, 0, 0, 0.0f };
}

TestRoute& toParam(TestRoute& r, feathers::ParamId id) {
	r.destKind = 0;
	r.destA = static_cast<uint32_t>(id);
	return r;
}
TestRoute& toDevice(TestRoute& r, uint32_t instance, uint32_t control) {
	r.destKind = 1;
	r.destA = instance;
	r.destB = control;
	return r;
}

/// WHO WEARS WHICH ID IN THE FIXTURE WORLD — the fixture rack's devices, and the default lane's. Read
/// off the fixtures themselves rather than asked of the saver.
/// EVERY SOURCE AND EVERY DESTINATION THE FIXTURE CAN NAME, BY IDENTITY — the host's own (the macros,
/// then the weights; every parameter destination) and, for every device in both chains, every output
/// and every control UP TO THE POOL'S WIDTH, so the sweep offers outputs a device does not declare and
/// controls it does not declare modulatable. Those must be refused, and the refusal is half the claim.
/// This is the enumeration `nameRoute` used to derive from a coordinate; there is no coordinate now
/// (kill-lanes §2.4b), so it is written as the list it always was.
std::vector<TestRoute> everySourceNamed() {
	std::vector<TestRoute> out;
	for (uint32_t macro = 0; macro < feathers::kMacroCount; ++macro) {
		out.push_back(fromMacro(macro));
	}
	for (uint32_t slot = 0; slot < feathers::kWeightSourceCount; ++slot) {
		out.push_back(fromWeight(slot));
	}
	for (const feathers::DeviceInstanceId id : fixtureIds()) {
		for (uint32_t output = 0; output < feathers::device::kMaxDeviceOutputs; ++output) {
			out.push_back(fromDevice(id, output));
		}
	}
	return out;
}

std::vector<TestRoute> everyDestinationNamed() {
	std::vector<TestRoute> out;
	for (size_t destination = 0; destination < feathers::kParamDestinationCount; ++destination) {
		const feathers::ParamId id = feathers::destinationParamId(destination);
		REQUIRE(id != static_cast<feathers::ParamId>(0));
		TestRoute r {};
		out.push_back(toParam(r, id));
	}
	for (const feathers::DeviceInstanceId id : fixtureIds()) {
		for (uint32_t control = 0; control < 32; ++control) { // 32: the old route budget, kept as a fixture width
			TestRoute r {};
			out.push_back(toDevice(r, id, control));
		}
	}
	return out;
}

/// **THE TWO ENDS A NAMED FILE ROUTE ASKS FOR** — the other half of `nameRoute`, and an independent
/// restatement of the READER the way that one restates the writer. A case that spells a route as the
/// file spells it needs to be able to ask what the load left AT it, and the record answers by endpoint
/// (kill-lanes step 5) — so the file's three source kinds and two destination kinds are resolved here,
/// once, rather than at each site. False for a kind this build cannot mean, which is what the loader
/// says of one too.
bool endpointsOfNamed(const TestRoute& r, feathers::backplane::Source* s,
                      feathers::backplane::Destination* d) {
	switch (r.sourceKind) {
	case 0: // a macro — the macros are the first of the host's own sources
		*s = feathers::backplane::Source::core(r.sourceA);
		break;
	case 1: // a weight, by the sound it belongs to
		*s = feathers::backplane::Source::core(feathers::weightSourceOf(r.sourceA));
		break;
	case 2: // a device's own output
		*s = feathers::backplane::Source::output(static_cast<feathers::DeviceInstanceId>(r.sourceA),
		                                         r.sourceB);
		break;
	default:
		return false;
	}
	if (r.destKind == 1) {
		*d = feathers::backplane::Destination::control(
		    static_cast<feathers::DeviceInstanceId>(r.destA), r.destB);
		return true;
	}
	if (r.destKind != 0) {
		return false;
	}
	const int index = feathers::modDestinationIndexOf(static_cast<feathers::ParamId>(r.destA));
	if (index < 0) {
		return false;
	}
	*d = feathers::backplane::Destination::core(static_cast<size_t>(index));
	return true;
}

//------------------------------------------------------------------------------------------------//
// THE CHAINS BLOCK, FROM A CASE — the one format (chain-as-a-device item 6)                        //
//------------------------------------------------------------------------------------------------//
//
// The lane, the strip and the routings were three blocks with their own device record; they are one
// block now, in the rack's format, written from the bridge's record (`ParamBridge::chainsPatch`) and
// read back into one (`restoreChains`). These are the two halves as a case drives them: a bridge made to
// hold exactly some chains and routes, asked for its patch; and a patch restored into a FRESH bridge,
// which is what a load does, read back through the same views the editor reads.

//------------------------------------------------------------------------------------------------//
// THE ROUTES AS A LIST OF ENDPOINTS — what these cases speak since kill-lanes step 5               //
//------------------------------------------------------------------------------------------------//
//
// A route is `(Source, Destination, amount)` and nothing else, so the vocabulary a case needs is a LIST
// rather than a table: what is routed, and how far one named end reaches another. `tests/route_coords.h`
// and the dense `ModRoutes` these cases compared are gone from here with it (kill-lanes step 5); these
// three are what replaced them, and each one is a thin read of the bridge's own endpoint API.
//
// `ModRoutes` ITSELF IS NOT GONE and a few cases below still build one: it is the ENGINE's vocabulary —
// `ParamSet::setRoutes`, `setDeviceSource`, `rackOffsetAt`, `DeviceBus` — where a route really is
// addressed by coordinate, and those cases say so where they do it.
//
// **THE LIST IS THE PUBLISHED ONE** (`ParamBridge::forEachRoute`), which is the record's routes as the
// audio thread will see them, the record's routes exactly: since backlog OJ nothing is dropped at the publish.

using TestRoutes = std::vector<feathers::backplane::Route>;

/// THE SAME ROUTES IN A STABLE ORDER — by the two ends, which are totally ordered. A hand-built
/// expectation goes through this too, so a case compares two lists rather than two orderings.
TestRoutes sortedRoutes(TestRoutes routes) {
	std::sort(routes.begin(), routes.end(),
	          [](const feathers::backplane::Route& a, const feathers::backplane::Route& b) {
		          if (a.source != b.source) {
			          return a.source < b.source;
		          }
		          return a.destination < b.destination;
	          });
	return routes;
}

/// EVERY ROUTE, IN THAT ORDER.
TestRoutes routesOn(const feathers::ParamBridge& bridge) {
	TestRoutes out;
	bridge.forEachRoute([&out](const feathers::backplane::Route& r) { out.push_back(r); });
	return sortedRoutes(std::move(out));
}

/// THE DEFAULT PATCH'S ONE ROUTE (backlog EF): the strip's envelope — the device a fresh instance has,
/// under the id it always wears — into Level at full amount. A fresh bridge carries it, and a restore
/// with NO patch recreates it; a patch that is present brings its own routes and nothing else.
feathers::backplane::Route defaultEnvelopeRoute() {
	const int level = feathers::modDestinationIndexOf(feathers::ParamId::VoiceLevel);
	REQUIRE(level >= 0);
	return { feathers::backplane::Source::output(feathers::kDefaultRackInstance, 0),
		     feathers::backplane::Destination::core(static_cast<size_t>(level)), 1.0f };
}

/// HOW FAR `s` REACHES `d` IN A LIST, or 0 — the list's answer to what `ModRoutes::at` answered.
float amountIn(const TestRoutes& routes, feathers::backplane::Source s, feathers::backplane::Destination d) {
	for (const feathers::backplane::Route& r : routes) {
		if (r.source == s && r.destination == d) {
			return r.amount;
		}
	}
	return 0.0f;
}

/// A BRIDGE HOLDING EXACTLY `chain`, `rack` AND `routes` — emptied first (a present, empty patch), so
/// the ids a fixture stamps are the ids the record holds, rather than ones re-issued around a fresh
/// instance's defaults.
///
/// The routes land the way a LOAD lands them (`loadRoutesForTest`): through the record's gate, past the
/// hand's cap, and not an edit — which is what `restoreRoutesAt` did for them in flat coordinates.
void holdChains(feathers::ParamBridge& bridge, const feathers::FxChain& chain, const feathers::ModRack& rack,
                const TestRoutes& routes = {}) {
	feathers::rack::RackPatch nothing;
	nothing.present = true;
	REQUIRE(bridge.restoreChains(nothing).routes.count == 0);
	bridge.restoreFxChain(chain);
	bridge.restoreModRack(rack);
	bridge.loadRoutesForTest(routes);
}

/// THE PATCH THE STATE ROAD WOULD WRITE for these chains and routes — the production writer's output.
feathers::rack::RackPatch patchOf(const feathers::FxChain& chain, const feathers::ModRack& rack,
                                  const TestRoutes& routes = {}) {
	feathers::ParamBridge bridge;
	holdChains(bridge, chain, rack, routes);
	return bridge.chainsPatch();
}

/// **A HAND-NAMED ROUTE AS THE FILE SPELLS IT** — since backlog JF a device end is its id and a KEY, so the
/// output or control index a case names is turned into the key of whatever device `patch` holds under that
/// id. An index that device does not have (or an id no device in the patch wears) becomes a key nothing
/// declares, which is what the index past the end used to be: a route naming no control.
feathers::rack::RackPatch::Route routeOf(const TestRoute& r, const feathers::rack::RackPatch& patch) {
	const auto keyOf = [&](uint32_t id, uint32_t index, bool output) -> std::string {
		for (const auto& d : patch.device) {
			const feathers::device::Descriptor* desc = d.instanceId == id ? feathers::deviceForTag(d.tag.data()) : nullptr;
			if (desc == nullptr) {
				continue;
			}
			if (output && index < desc->outputs.size()) {
				return desc->outputs[index].key;
			}
			if (!output && index < desc->params.size()) {
				return desc->params[index].key;
			}
		}
		return "no_such_" + std::string(output ? "output_" : "control_") + std::to_string(index);
	};
	const bool fromDevice = r.sourceKind == static_cast<uint32_t>(feathers::rack::SourceKind::Device);
	const bool toDevice = r.destKind == static_cast<uint32_t>(feathers::rack::DestKind::Device);
	return { { r.sourceKind, r.sourceA, fromDevice ? 0u : r.sourceB, fromDevice ? keyOf(r.sourceA, r.sourceB, true) : "" },
		     { r.destKind, r.destA, toDevice ? 0u : r.destB, toDevice ? keyOf(r.destA, r.destB, false) : "" },
		     r.amount };
}

/// **A PATCH THIS BUILD'S WRITER CANNOT PRODUCE** — the fixture world's devices, as the real writer
/// writes them, carrying routes named by hand. What the file's author meant, whatever the rules say.
feathers::rack::RackPatch patchWithRoutes(const std::vector<TestRoute>& routes,
                                          const feathers::FxChain& chain = feathers::defaultFxChain(),
                                          const feathers::ModRack& rack = rackForTest()) {
	feathers::rack::RackPatch patch = patchOf(chain, rack);
	patch.route.clear();
	for (const TestRoute& r : routes) {
		patch.route.push_back(routeOf(r, patch));
	}
	return patch;
}

/// WHAT A LOAD LEAVES — the chains, the routes and the refusals, read back from a fresh bridge the patch
/// was restored into.
struct Reloaded {
	feathers::FxChain chain;
	feathers::ModRack rack;
	TestRoutes routes;
	feathers::RouteRefusals refused;
	/// The whole load report (backlog JF), of which `refused` is the routes.
	feathers::rack::LoadReport report;
};

/// THE SAME DEVICES, the counter aside. `nextInstanceId` is not something a chain carries through a file
/// any more: it is the bridge's one counter, and a load reserves it past every id the patch names — so
/// a view read back from a fresh bridge carries THAT bridge's counter, which is at least as far along.
bool sameDevices(feathers::FxChain a, feathers::FxChain b) {
	a.nextInstanceId = b.nextInstanceId = 0;
	return a == b;
}
bool sameDevices(feathers::ModRack a, feathers::ModRack b) {
	a.nextInstanceId = b.nextInstanceId = 0;
	return a == b;
}

Reloaded reload(const feathers::rack::RackPatch& patch) {
	feathers::ParamBridge bridge;
	Reloaded out;
	out.report = bridge.restoreChains(patch);
	out.refused = out.report.routes;
	out.chain = bridge.fxChain();
	out.rack = bridge.modRack();
	out.routes = routesOn(bridge);
	return out;
}

/// **WHETHER THE LOAD LEFT A DEVICE UNDER `id` IN EITHER CHAIN.**
///
/// A REFUSAL NAMES THE ENDPOINT THE FILE ASKED FOR, NOT WHAT THE LOOKUP FOUND (`rack/rack_state.h`), so
/// "this end is gone" is two facts and not one: the report names device X, AND this build does not hold
/// X. The second is what separates a correct report from one that named a device the load went on to
/// place anyway, and a case that asserts only the first cannot tell them apart. `Reloaded` keeps the
/// views rather than the bridge, so the question is asked of them — which is also how `Gui::Impl::tick`
/// asks it, one object further in.
bool holdsDevice(const Reloaded& loaded, feathers::DeviceInstanceId id) {
	if (id == feathers::kNoInstance) {
		return false;
	}
	for (size_t position = 0; position < feathers::fxChainLength(loaded.chain); ++position) {
		// THE POSITION IS THE SUBSCRIPT NOW (kill-lanes step 3c) — this crossed from a position to the
		// row's storage index through the old `order[]`, and the chain IS its order, so there is nothing
		// to cross.
		if (feathers::fxDeviceAtPosition(loaded.chain, position).row.instanceId == id) {
			return true;
		}
	}
	for (size_t slot = 0; slot < loaded.rack.count && slot < loaded.rack.device.size(); ++slot) {
		if (loaded.rack.device[slot].instanceId == id) {
			return true;
		}
	}
	return false;
}

} // namespace

using namespace feathers;

namespace {

/// The readings the engine would publish for a patch sitting at its defaults.
std::vector<double> defaults() {
	std::vector<double> values;
	for (const ParamDesc& desc : paramDescs()) {
		values.push_back(desc.def);
	}
	return values;
}

clap_id idOf(ParamId id) {
	return static_cast<clap_id>(id);
}

} // namespace

TEST_CASE("queue delivers events in order") {
	UiParamQueue queue;
	CHECK(queue.empty());

	queue.push({ UiParamEvent::Kind::GestureBegin, 7, 0.0 });
	queue.push({ UiParamEvent::Kind::Value, 7, 0.25 });
	queue.push({ UiParamEvent::Kind::GestureEnd, 7, 0.0 });
	CHECK_FALSE(queue.empty());

	UiParamEvent event;
	REQUIRE(queue.pop(event));
	CHECK(event.kind == UiParamEvent::Kind::GestureBegin);
	REQUIRE(queue.pop(event));
	CHECK(event.kind == UiParamEvent::Kind::Value);
	CHECK(event.value == doctest::Approx(0.25));
	CHECK(event.id == 7);
	REQUIRE(queue.pop(event));
	CHECK(event.kind == UiParamEvent::Kind::GestureEnd);

	CHECK_FALSE(queue.pop(event));
	CHECK(queue.empty());
	CHECK(queue.droppedCount() == 0);
}

TEST_CASE("queue reports overflow rather than overwriting") {
	UiParamQueue queue;
	for (size_t i = 0; i < UiParamQueue::kCapacity; ++i) {
		CHECK(queue.push({ UiParamEvent::Kind::Value, 1, static_cast<double>(i) }));
	}
	// Full. The next push is refused and counted, and — the part that matters — the events already in
	// the queue are still the oldest ones, not overwritten by newer arrivals.
	CHECK_FALSE(queue.push({ UiParamEvent::Kind::Value, 1, 9999.0 }));
	CHECK(queue.droppedCount() == 1);

	UiParamEvent event;
	REQUIRE(queue.pop(event));
	CHECK(event.value == doctest::Approx(0.0));

	// And a pop frees exactly one slot.
	CHECK(queue.push({ UiParamEvent::Kind::Value, 1, 1234.0 }));
	CHECK_FALSE(queue.push({ UiParamEvent::Kind::Value, 1, 5678.0 }));
}

TEST_CASE("queue survives a producer and a consumer on different threads") {
	// Not a proof of correctness — no test is, for a lock-free structure — but it does catch a wrap
	// that only misbehaves past the capacity, which a single-threaded test never reaches.
	static constexpr int kCount = 50000;
	UiParamQueue queue;

	std::thread producer([&] {
		for (int i = 0; i < kCount;) {
			if (queue.push({ UiParamEvent::Kind::Value, 3, static_cast<double>(i) })) {
				++i;
			}
		}
	});

	int received = 0;
	UiParamEvent event;
	while (received < kCount) {
		if (queue.pop(event)) {
			// Every value arrives once and in the order it was sent.
			CHECK(event.value == doctest::Approx(static_cast<double>(received)));
			++received;
		}
	}
	producer.join();

	CHECK(received == kCount);
	CHECK(queue.empty());
	// droppedCount is deliberately not checked: this producer retries until a push succeeds, so every
	// moment the consumer was behind counts a refusal. What matters is above — nothing was lost, and
	// nothing arrived twice or out of order.
}

TEST_CASE("first refresh reports every parameter") {
	ParamBridge bridge;
	const std::span<const uint32_t> changed = bridge.refresh();
	CHECK(changed.size() == kParamCount);
}

TEST_CASE("refresh reports only what moved") {
	ParamBridge bridge;
	bridge.refresh(); // the initial full batch

	std::vector<double> base = defaults();
	std::vector<double> modulated = defaults();

	// Nothing published yet differs from the defaults the bridge started at.
	bridge.publishValuesForTest(base, modulated);
	CHECK(bridge.refresh().empty());

	const int pathIndex = paramIndexOf(idOf(ParamId::Path));
	REQUIRE(pathIndex >= 0);
	base[static_cast<size_t>(pathIndex)] += 0.5;
	modulated[static_cast<size_t>(pathIndex)] += 0.5;
	bridge.publishValuesForTest(base, modulated);

	const std::span<const uint32_t> changed = bridge.refresh();
	REQUIRE(changed.size() == 1);
	CHECK(changed[0] == static_cast<uint32_t>(pathIndex));

	// And a second look at an unchanged snapshot is empty — the detector reports edges, not levels.
	CHECK(bridge.refresh().empty());
}

TEST_CASE("modulation alone counts as a change") {
	ParamBridge bridge;
	bridge.refresh();

	std::vector<double> base = defaults();
	std::vector<double> modulated = defaults();
	bridge.publishValuesForTest(base, modulated);
	bridge.refresh();

	// A host modulator moving a parameter does not touch its base. The editor still has to notice, or
	// the two-state display Task 4 builds would never move.
	const int index = paramIndexOf(idOf(ParamId::Path));
	REQUIRE(index >= 0);
	modulated[static_cast<size_t>(index)] += 0.25;
	bridge.publishValuesForTest(base, modulated);

	REQUIRE(bridge.refresh().size() == 1);
	CHECK(bridge.value(idOf(ParamId::Path)) == doctest::Approx(base[static_cast<size_t>(index)]));
	CHECK(bridge.modulated(idOf(ParamId::Path)) ==
	      doctest::Approx(modulated[static_cast<size_t>(index)]));
}

TEST_CASE("an editor move is queued and shown immediately") {
	ParamBridge bridge;
	bridge.refresh();

	CHECK_FALSE(bridge.hasPendingWork());
	bridge.beginGesture(idOf(ParamId::Path));
	bridge.setValue(idOf(ParamId::Path), 0.75);
	bridge.endGesture(idOf(ParamId::Path));
	CHECK(bridge.hasPendingWork());

	// Shown at once rather than a block later, so a drag does not lag the cursor.
	CHECK(bridge.value(idOf(ParamId::Path)) == doctest::Approx(0.75));

	std::vector<UiParamEvent> drained;
	bridge.drain([&](const UiParamEvent& event) { drained.push_back(event); });

	REQUIRE(drained.size() == 3);
	CHECK(drained[0].kind == UiParamEvent::Kind::GestureBegin);
	CHECK(drained[1].kind == UiParamEvent::Kind::Value);
	CHECK(drained[1].value == doctest::Approx(0.75));
	CHECK(drained[2].kind == UiParamEvent::Kind::GestureEnd);
	CHECK_FALSE(bridge.hasPendingWork());
}

TEST_CASE("what the editor shows is what the engine will store") {
	ParamBridge bridge;
	bridge.refresh();

	// Out of range, and on a stepped parameter. The engine clamps and rounds on the way in, so an
	// editor that displayed the raw request would show a value the engine does not have — a slider
	// sitting between two steps, or past the end of its own track.
	const ParamDesc* voiceMode = findParam(idOf(ParamId::VoiceMode));
	REQUIRE(voiceMode != nullptr);
	REQUIRE((voiceMode->flags & CLAP_PARAM_IS_STEPPED) != 0);

	bridge.setValue(idOf(ParamId::VoiceMode), 1.4);
	CHECK(bridge.value(idOf(ParamId::VoiceMode)) == doctest::Approx(1.0));

	bridge.setValue(idOf(ParamId::VoiceMode), 99.0);
	CHECK(bridge.value(idOf(ParamId::VoiceMode)) == doctest::Approx(voiceMode->max));

	bridge.setValue(idOf(ParamId::VoiceMode), -99.0);
	CHECK(bridge.value(idOf(ParamId::VoiceMode)) == doctest::Approx(voiceMode->min));

	// The queue still carries the raw request; ParamSet::setBase is the one place that decides.
	UiParamEvent event;
	while (bridge.hasPendingWork()) {
		bridge.drain([&](const UiParamEvent& e) { event = e; });
	}
	CHECK(event.value == doctest::Approx(-99.0));
}

TEST_CASE("a value that is not a number is refused") {
	ParamBridge bridge;
	bridge.refresh();
	const double before = bridge.value(idOf(ParamId::Path));

	bridge.setValue(idOf(ParamId::Path), std::nan(""));
	CHECK(bridge.value(idOf(ParamId::Path)) == doctest::Approx(before));
	CHECK_FALSE(bridge.hasPendingWork());
}

TEST_CASE("unknown ids are refused at the door") {
	ParamBridge bridge;
	bridge.refresh();

	CHECK(bridge.value(0xdead) == doctest::Approx(0.0));
	CHECK(bridge.modulated(0xdead) == doctest::Approx(0.0));

	// Nothing unaddressable reaches the queue, on any of the three entry points. ParamSet would ignore
	// such an id anyway, but the queue is no longer the end of the road: the plugin binding remembers
	// ids the host had no room to accept an event for and re-sends them, and an id nobody recognises
	// would hold one of those slots forever, offered to the host on every block.
	bridge.beginGesture(0xdead);
	bridge.setValue(0xdead, 1.0);
	bridge.endGesture(0xdead);
	CHECK_FALSE(bridge.hasPendingWork());
}

TEST_CASE("every declared parameter round-trips through the bridge") {
	// The bridge proof the task asks for, over the whole surface rather than a sample of it: each of
	// the 24 can be written, is shown as the engine would store it, and prints as text.
	ParamBridge bridge;
	bridge.refresh();

	for (const ParamDesc& desc : paramDescs()) {
		const clap_id id = static_cast<clap_id>(desc.id);
		const double target = desc.min + (desc.max - desc.min) * 0.5;

		bridge.setValue(id, target);
		const double shown = bridge.value(id);
		CHECK(shown >= desc.min);
		CHECK(shown <= desc.max);
		if ((desc.flags & CLAP_PARAM_IS_STEPPED) != 0) {
			CHECK(shown == doctest::Approx(std::round(shown)));
		}
		else {
			CHECK(shown == doctest::Approx(target));
		}

		char printed[64] = {};
		CHECK(valueToText(id, shown, printed, sizeof(printed)));
		CHECK(printed[0] != '\0');
	}
}

TEST_CASE("the bridge and a real ParamSet agree") {
	// The engine's side of the road, joined up: what the bridge publishes is what a ParamSet holds.
	ParamBridge bridge;
	ParamSet params;
	bridge.refresh();

	bridge.setValue(idOf(ParamId::Path), 0.6);
	bridge.setValue(idOf(ParamId::Relief), 0.3);

	// Standing in for the plugin binding's drain: apply each move to the engine.
	bridge.drain([&](const UiParamEvent& event) {
		if (event.kind == UiParamEvent::Kind::Value) {
			params.setBase(event.id, event.value);
		}
	});
	bridge.publish(params);
	bridge.refresh();

	CHECK(bridge.value(idOf(ParamId::Path)) == doctest::Approx(params.base(idOf(ParamId::Path))));
	CHECK(bridge.value(idOf(ParamId::Relief)) == doctest::Approx(params.base(idOf(ParamId::Relief))));
	CHECK(bridge.modulated(idOf(ParamId::Path)) ==
	      doctest::Approx(params.resolved(idOf(ParamId::Path))));
}

TEST_CASE("the audition orientation survives the trip across the bridge") {
	// The channel the experimental pin travels on. It is not a parameter and has no id, so none of the
	// machinery above covers it, and the whole point of it is that the audio thread reads exactly what
	// the handle wrote.
	ParamBridge bridge;
	CHECK(bridge.orientation() == SliceOrientationPin {});
	CHECK_FALSE(bridge.orientation().pinned); // unpinned is the ordinary case: planes come from homes

	SliceOrientationPin turned;
	turned.pinned = true;
	turned.generator.rotate = -1.25f;
	bridge.setOrientation(turned);
	CHECK(bridge.orientation() == turned);
}

TEST_CASE("the falloff A/B travels on the same channel, and refuses nonsense") {
	// The second audition handle, and the one that arrives from a text field as well as from a slider.
	// Its default has to be the value every saved patch was made on, because a bridge that came up on
	// the B side would silently re-blend every patch with three or more sounds.
	ParamBridge bridge;
	CHECK(bridge.tuning().falloffPower == kFalloffPowerA);
	CHECK(bridge.tuning().orientation == SliceOrientationPin {});

	bridge.setFalloffPower(kFalloffPowerB);
	CHECK(bridge.tuning().falloffPower == kFalloffPowerB);

	// A zero exponent makes every weight 1 whatever the distance — a constellation with no geometry
	// left in it — and a negative one hands the blend to the FURTHEST sound. Both are silent to look at
	// and loud to hear, so the last good value is kept rather than the bad one taken.
	for (const double bad : { 0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
		                      std::numeric_limits<double>::infinity() }) {
		bridge.setFalloffPower(bad);
		CHECK(bridge.tuning().falloffPower == kFalloffPowerB);
	}

	// The two handles do not disturb each other on the way through.
	SliceOrientationPin turned;
	turned.pinned = true;
	turned.generator.rotate = 0.5f;
	bridge.setOrientation(turned);
	CHECK(bridge.tuning().orientation == turned);
	CHECK(bridge.tuning().falloffPower == kFalloffPowerB);
}

TEST_CASE("the blue-shift scales start live, and stay where they are put") {
	// LIVE IS THE DEFAULT, by design, and the bridge has to agree with AuditionTuning about that — two
	// places that both name a default are two places that can disagree, and the one the engine reads is
	// this one.
	ParamBridge bridge;
	CHECK(bridge.tuning().blueOvershootScale == kBlueShiftScaleOn);
	CHECK(bridge.tuning().blueDopplerScale == kBlueShiftScaleOn);
	CHECK(bridge.tuning().blueShiftActive());
	CHECK(bridge.tuning() == AuditionTuning {});

	bridge.setBlueShiftScales(kBlueShiftScaleOff, kBlueShiftScaleMore);
	CHECK(bridge.tuning().blueOvershootScale == kBlueShiftScaleOff);
	CHECK(bridge.tuning().blueDopplerScale == kBlueShiftScaleMore);
	CHECK(bridge.tuning().blueShiftActive());

	// Both down is the reference build, and it has to be reachable from here — it is what the audition
	// compares against.
	bridge.setBlueShiftScales(kBlueShiftScaleOff, kBlueShiftScaleOff);
	CHECK_FALSE(bridge.tuning().blueShiftActive());

	bridge.setBlueShiftScales(kBlueShiftScaleOn, kBlueShiftScaleMore);

	// Negative is not the feature with a sign on it — it is receding sounds brightening, which is a
	// different feature nobody described. Refused, along with the values that are not numbers.
	for (const double bad : { -1.0, std::numeric_limits<double>::quiet_NaN(),
		                      std::numeric_limits<double>::infinity() }) {
		bridge.setBlueShiftScales(bad, bad);
		CHECK(bridge.tuning().blueOvershootScale == kBlueShiftScaleOn);
		CHECK(bridge.tuning().blueDopplerScale == kBlueShiftScaleMore);
	}

	// Turning one down does not disturb the other, which is what the strip's two rows depend on.
	bridge.setBlueShiftScales(kBlueShiftScaleOff, kBlueShiftScaleMore);
	CHECK(bridge.tuning().blueOvershootScale == kBlueShiftScaleOff);
	CHECK(bridge.tuning().blueDopplerScale == kBlueShiftScaleMore);
}

TEST_CASE("parity mode is off unless something asks for it") {
	// The way back to the v1 crossfade, and it must be OFF by default in the most boring possible way:
	// a plugin that came up in parity mode would be playing an older engine while reporting nothing
	// unusual, and every judgment made on it would be about the wrong build.
	ParamBridge bridge;
	CHECK_FALSE(bridge.tuning().v1Crossfade);

	bridge.setV1Crossfade(true);
	CHECK(bridge.tuning().v1Crossfade);
	// It does not disturb the handles beside it on the way through.
	CHECK(bridge.tuning().blueOvershootScale == kBlueShiftScaleOn);
	CHECK(bridge.tuning().falloffPower == kFalloffPowerA);

	bridge.setV1Crossfade(false);
	CHECK_FALSE(bridge.tuning().v1Crossfade);
}

TEST_CASE("the puck's velocity travels engine to editor") {
	// The one thing on this channel that is measured rather than set, so the scope can draw the sound
	// being made mid-gesture. Zero until the engine says otherwise: an editor opened before the plugin
	// has rendered anything must not draw a gesture nobody made.
	ParamBridge bridge;
	CHECK(bridge.puckVelocity() == AnchorPoint {});

	bridge.publishPuckVelocity({ 1.25, -0.5 });
	CHECK(bridge.puckVelocity() == AnchorPoint { 1.25, -0.5 });
}

TEST_CASE("the environment's orientation is read the way it is written") {
	// A handle nobody can aim is not a handle. These are the spellings a person actually types, and
	// the failure that matters is the quiet one — a typo read as zero, which would look exactly like
	// an orientation that did nothing.
	CHECK(parseOrientation("") == SliceGenerator {});
	CHECK(parseOrientation("0") == SliceGenerator {});

	// One number is the turn itself — the spelling the shrunk generator invites.
	CHECK(parseOrientation("0.7") == SliceGenerator { 0.7f });

	// Two or more numbers are the legacy layout: the aperture rides first, unread by M, and the
	// SECOND field is the turn — the same reading the stream loaders give a legacy block.
	const SliceGenerator full { 0.6f };
	CHECK(parseOrientation("0.9,0.6,-0.25") == full);
	CHECK(parseOrientation("0.9 0.6 -0.25") == full);
	CHECK(parseOrientation("  0.9 , 0.6 ,  -0.25  ") == full);

	// Junk ends the parse rather than being read as a zero, and what was already read still stands.
	CHECK(parseOrientation("0.9,0.6,-0.25,oops,1.0") == full);

	// More fields than the generator carries are ignored rather than overrunning it.
	CHECK(parseOrientation("1,0.1,0.1,0.1,0.1,0.1,0.1,0.1,0.1") == SliceGenerator { 0.1f });
}

namespace {

/// A clap stream pair over a byte vector, with a chunk size so a partial read or write — which hosts
/// are allowed to do — is a case the serializer actually meets. Same shape as the parameter test's,
/// because it is the same contract.
struct MemoryStream {
	std::vector<uint8_t> bytes;
	size_t readPos{0};
	size_t chunk{SIZE_MAX};

	static int64_t CLAP_ABI writeFn(const clap_ostream* stream, const void* buffer, uint64_t size) {
		auto* self = static_cast<MemoryStream*>(stream->ctx);
		const size_t n = std::min<size_t>(size, self->chunk);
		const auto* src = static_cast<const uint8_t*>(buffer);
		self->bytes.insert(self->bytes.end(), src, src + n);
		return static_cast<int64_t>(n);
	}

	static int64_t CLAP_ABI readFn(const clap_istream* stream, void* buffer, uint64_t size) {
		auto* self = static_cast<MemoryStream*>(stream->ctx);
		const size_t available = self->bytes.size() - self->readPos;
		const size_t n = std::min<size_t>(std::min<size_t>(size, self->chunk), available);
		std::memcpy(buffer, self->bytes.data() + self->readPos, n);
		self->readPos += n;
		return static_cast<int64_t>(n);
	}

	clap_ostream out() { return clap_ostream{this, writeFn}; }
	clap_istream in() {
		readPos = 0;
		return clap_istream{this, readFn};
	}
};

/// **A PATCH THROUGH THE REAL BYTES** — `saveChainsBlock` into a stream, `loadChainsBlock` back out, at
/// `chunk` bytes per host call. The stream must be used up exactly: the next block in a real patch starts
/// where this reader left off.
feathers::rack::RackPatch throughBytes(const feathers::rack::RackPatch& patch, size_t chunk = SIZE_MAX) {
	MemoryStream stream;
	stream.chunk = chunk;
	clap_ostream out = stream.out();
	REQUIRE(feathers::saveChainsBlock(&out, patch));
	clap_istream in = stream.in();
	feathers::rack::RackPatch back;
	bool present = false;
	feathers::rack::LoadReport report;
	const bool loaded = feathers::loadChainsBlock(&in, &back, &present, &report);
	INFO("not loaded: " << report.firstOf(feathers::rack::LoadReport::Kind::NotLoaded).why); // WHY, when it is not
	REQUIRE(loaded);
	CHECK(present);
	CHECK(stream.readPos == stream.bytes.size());
	return back;
}

} // namespace

TEST_CASE("a patch carries its orientation, exactly and explicitly") {
	// The versioning obligation, as a test. The shipping default is expected to move as it is tuned by
	// ear, so a patch that leaned on it would move with it — silently, and years later. Written out in
	// full, and read back to the bit: an orientation that shifted in the last place would move every
	// sound in the patch a little.
	SliceOrientationPin saved;
	saved.pinned = true;
	saved.generator.rotate = 0.4321f; // awkward on purpose — an exact-float fixture cannot see a cast

	for (size_t chunk : {SIZE_MAX, size_t{3}, size_t{1}}) {
		CAPTURE(chunk);
		MemoryStream stream;
		stream.chunk = chunk;
		clap_ostream out = stream.out();
		REQUIRE(saveOrientation(&out, saved));

		SliceOrientationPin loaded;
		bool present = false;
		clap_istream in = stream.in();
		REQUIRE(loadOrientation(&in, &loaded, &present));
		CHECK(present);
		CHECK(loaded == saved);
	}

	// AND AN UNPINNED PATCH RECORDS THAT IT IS UNPINNED, rather than recording whatever plane the
	// derivation happened to produce. Otherwise a sound whose plane comes from its home would be frozen
	// on the day it was saved, and would stop following its home the moment anyone moved it.
	MemoryStream stream;
	clap_ostream out = stream.out();
	REQUIRE(saveOrientation(&out, SliceOrientationPin {}));
	SliceOrientationPin loaded;
	loaded.pinned = true;
	bool present = false;
	clap_istream in = stream.in();
	REQUIRE(loadOrientation(&in, &loaded, &present));
	CHECK(present);
	CHECK_FALSE(loaded.pinned);
}

TEST_CASE("a legacy orientation block — aperture and six angles — loads with its turn intact") {
	// THE MIGRATION GUARD FOR THE STAGE-2 SHRINK, built from raw bytes rather than from this build's
	// writer — the writer is exactly what the shrink changes, so a fixture that used it would test the
	// build against itself. These bytes are the legacy layout every pre-shrink build wrote: tag,
	// pinned, a count of SIX, the aperture, then six angles the corner spread at φ⁻ᵏ ratios with
	// ratio[0] = 1. The claim that must survive the shrink is the decided migration: THE TURN IS ANGLE
	// ZERO, exactly, and the pinned flag is honoured. The aperture and the trailing angles are unread
	// by M and are allowed to be dropped.
	MemoryStream stream;
	const auto putFloat = [&stream](float v) {
		const auto* p = reinterpret_cast<const uint8_t*>(&v);
		stream.bytes.insert(stream.bytes.end(), p, p + sizeof v);
	};
	stream.bytes = { 'F', 'S', 'L', '1' };
	putFloat(1.0f);    // pinned
	putFloat(6.0f);    // six angles — the legacy count
	putFloat(0.4321f); // aperture, unread by M
	const float turn = 1.5f;
	constexpr float kInvPhi = 0.6180339887498949f;
	float spread = turn;
	for (int k = 0; k < 6; ++k) {
		putFloat(spread); // the φ⁻ᵏ ratio spread the old corner wrote, angle 0 first
		spread *= kInvPhi;
	}

	SliceOrientationPin loaded;
	bool present = false;
	clap_istream in = stream.in();
	REQUIRE(loadOrientation(&in, &loaded, &present));
	CHECK(present);
	CHECK(loaded.pinned);
	CHECK(loaded.generator.rotate == turn); // exact — the whole migration, in one comparison
}

TEST_CASE("a legacy per-sound planes block loads every slot's turn intact") {
	// The same guard for the per-sound channel: eight slots, six angles each, as every pre-shrink
	// build wrote them. Slot 2 carries a turn and slot 5 is pinned AT ZERO — the two are different
	// states (a zero turn held by hand is not the absence of a pin), and both must come through.
	MemoryStream stream;
	const auto putFloat = [&stream](float v) {
		const auto* p = reinterpret_cast<const uint8_t*>(&v);
		stream.bytes.insert(stream.bytes.end(), p, p + sizeof v);
	};
	stream.bytes = { 'F', 'S', 'P', '1' };
	putFloat(8.0f); // slots
	putFloat(6.0f); // angles per slot — the legacy count
	for (int slot = 0; slot < 8; ++slot) {
		const bool pinned = slot == 2 || slot == 5;
		const float turn = slot == 2 ? -2.75f : 0.0f;
		putFloat(pinned ? 1.0f : 0.0f);
		putFloat(pinned ? 0.35f : 0.0f); // aperture, unread by M
		float spread = turn;
		constexpr float kInvPhi = 0.6180339887498949f;
		for (int k = 0; k < 6; ++k) {
			putFloat(spread);
			spread *= kInvPhi;
		}
	}

	SoundOrientations loaded;
	bool present = false;
	clap_istream in = stream.in();
	REQUIRE(loadSoundOrientations(&in, &loaded, &present));
	CHECK(present);
	CHECK(loaded.pin[2].pinned);
	CHECK(loaded.pin[2].generator.rotate == -2.75f);
	CHECK(loaded.pin[5].pinned);
	CHECK(loaded.pin[5].generator.rotate == 0.0f);
	for (const size_t slot : { size_t { 0 }, size_t { 1 }, size_t { 3 }, size_t { 4 }, size_t { 6 },
	                           size_t { 7 } }) {
		CAPTURE(slot);
		CHECK_FALSE(loaded.pin[slot].pinned);
	}
}

TEST_CASE("a patch from before the orientation existed is not an error") {
	// The stream ends where the parameters ended. That is a patch this build can still open — it gets
	// the orientation a new sound is born with — and it must not read as a corrupt one.
	MemoryStream empty;
	SliceOrientationPin loaded;
	loaded.generator.rotate = 99.0f; // must be left alone
	bool present = true;
	clap_istream in = empty.in();
	CHECK(loadOrientation(&in, &loaded, &present));
	CHECK_FALSE(present);
	CHECK(loaded.generator.rotate == 99.0f);

	// A stream that starts something and stops halfway IS corrupt, and saying so is the point of
	// telling the two apart: guessing there would load a sound nobody saved.
	MemoryStream truncated;
	truncated.bytes = {'F', 'S', 'L', '1', 0x00};
	SliceOrientationPin ignored;
	clap_istream half = truncated.in();
	CHECK_FALSE(loadOrientation(&half, &ignored, &present));

	// So does a trailing block that is not ours.
	MemoryStream foreign;
	foreign.bytes = {'X', 'Y', 'Z', 'W'};
	clap_istream other = foreign.in();
	CHECK_FALSE(loadOrientation(&other, &ignored, &present));
}

TEST_CASE("the orientation rides behind the parameters in one stream") {
	// The composition the plugin actually performs, rather than the two halves separately. The thing
	// that can go wrong here is positional: if the parameter reader consumed a byte more or less than
	// it wrote, the orientation would read as garbage — or, worse, as plausible.
	ParamSet params;
	params.setBase(static_cast<clap_id>(ParamId::AZone), 321.0);
	SliceOrientationPin orientation;
	orientation.pinned = true;
	orientation.generator.rotate = 1.125f;

	MemoryStream stream;
	clap_ostream out = stream.out();
	REQUIRE(params.save(&out));
	REQUIRE(saveOrientation(&out, orientation));

	ParamSet reloaded;
	SliceOrientationPin loaded;
	bool present = false;
	clap_istream in = stream.in();
	REQUIRE(reloaded.load(&in));
	REQUIRE(loadOrientation(&in, &loaded, &present));
	CHECK(present);
	CHECK(loaded == orientation);
	CHECK(reloaded.base(static_cast<clap_id>(ParamId::AZone)) == 321.0);
}

TEST_CASE("a patch carries the drift each sound has taken") {
	// DRIFT IS IDENTITY-LEVEL, so it has to survive a save or the sound is not the sound. Written to
	// the bit, and read back at three chunk sizes because a stream that arrives one byte at a time is
	// a real host and not a hypothetical one.
	// DRAG-REALISTIC VALUES, not float-exact ones. The first version of this fixture used 0.25 and
	// -0.125 — numbers a float represents exactly — so it passed while the block was persisting through
	// float casts and losing everything else. A drag produces 0.37, not 0.25.
	SoundTravel saved;
	saved.offset[0] = { 0.37, -0.5000001 };
	saved.offset[3] = { -1.0 / 3.0, 0.75 };
	saved.offset[7] = { 0.1 + 0.2, -1.0 };

	for (size_t chunk : { SIZE_MAX, size_t { 3 }, size_t { 1 } }) {
		CAPTURE(chunk);
		MemoryStream stream;
		stream.chunk = chunk;
		clap_ostream out = stream.out();
		REQUIRE(saveSoundTravel(&out, saved));

		SoundTravel loaded;
		bool present = false;
		clap_istream in = stream.in();
		REQUIRE(loadSoundTravel(&in, &loaded, &present));
		CHECK(present);
		CHECK(loaded == saved);
	}
}

TEST_CASE("radius-mod-jump: a saved orbit radius is stored in TRAVEL UNITS, so the knob's new top (2) leaves an old patch's radius where it was") {
	// THE LEAD'S QUESTION, 2026-11-07: Radius's knob range went 0..1 -> 0..2 (the face's top is now the whole plane). Is a
	// patch's Radius stored as a plain radius, or as a fraction of the range (so an old 0.3 would come back as 0.6)? Plain:
	// the travel block ("FST2", audition.cpp) writes `orbit.radius` as a little-endian double, and Radius is no CLAP
	// parameter (an internal destination), so there is no host-normalised value anywhere. Shown BY VALUE from the bytes,
	// restated here by hand rather than written by `saveSoundTravel` — the layout mainline 8dbf9bd9 writes (audition.cpp is
	// unchanged between it and this branch) — loaded by this build, and played with nothing routed.
	MemoryStream stream;
	const auto put = [&](const void* p, size_t n) {
		const auto* b = static_cast<const uint8_t*>(p);
		stream.bytes.insert(stream.bytes.end(), b, b + n);
	};
	const auto putDouble = [&](double v) {
		uint64_t bits = 0;
		std::memcpy(&bits, &v, sizeof(bits));
		for (int i = 0; i < 8; ++i) {
			const auto byte = static_cast<uint8_t>((bits >> (i * 8)) & 0xffU);
			put(&byte, 1);
		}
	};
	const auto putFloat = [&](float v) {
		uint32_t bits = 0;
		std::memcpy(&bits, &v, sizeof(bits));
		for (int i = 0; i < 4; ++i) {
			const auto byte = static_cast<uint8_t>((bits >> (i * 8)) & 0xffU);
			put(&byte, 1);
		}
	};
	put("FST2", 4);
	putFloat(static_cast<float>(kMaxSounds));
	putFloat(4.0f); // x, y, orbit radius, orbit theta
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		putDouble(0.0);
		putDouble(0.0);
		putDouble(slot == 1 ? 0.3 : 0.0); // the knob at 0.3 travel units — on the old face, 0.83 of its throw
		putDouble(0.0);
	}
	// THE SAME BYTES AS THIS BUILD WRITES — so the restatement is the format, not a guess at it.
	SoundTravel expect;
	expect.orbit[1] = { 0.3, 0.0 };
	MemoryStream written;
	clap_ostream out = written.out();
	REQUIRE(saveSoundTravel(&out, expect));
	CHECK(written.bytes == stream.bytes);

	SoundTravel loaded;
	bool present = false;
	clap_istream in = stream.in();
	REQUIRE(loadSoundTravel(&in, &loaded, &present));
	REQUIRE(present);
	CHECK(loaded.orbit[1].radius == 0.3); // exact: not 0.6, not 0.15
	// AND IT PLAYS AT 0.3 WITH NOTHING ROUTED, bit for bit — the face's new curve moves the knob's POINTER (0.3 now sits
	// at 0.73 of the throw) and never the sound.
	CHECK(playedRadius(loaded.orbit[1].radius, 0.0) == 0.3);
	CHECK(pulledPlace({ 0.0, 0.0 }, loaded.orbit[1], 0.0, 0.0).x == 0.3);
}

TEST_CASE("a patch from before drift existed loads as never having drifted") {
	// THE BACK-COMPAT CONTROL, and the reason the drift is a SECOND tagged block rather than more
	// fields in the first: a stream that ends where the orientation ends is not a short read, it is a
	// patch that predates this. It has to come back as exactly zero — the offset that reproduces what
	// that patch always sounded like — rather than as a failure or as garbage.
	MemoryStream stream;
	clap_ostream out = stream.out();
	SliceOrientationPin pin;
	pin.pinned = true;
	pin.generator.rotate = 0.6f;
	REQUIRE(saveOrientation(&out, pin));
	// ...and nothing after it, which is exactly what an older build wrote.

	clap_istream in = stream.in();
	SliceOrientationPin loadedPin;
	bool pinPresent = false;
	REQUIRE(loadOrientation(&in, &loadedPin, &pinPresent));
	REQUIRE(pinPresent);

	SoundTravel travel;
	travel.offset[2] = { 9.0, 9.0 }; // poisoned, so an untouched output cannot pass by accident
	bool present = true;
	CHECK(loadSoundTravel(&in, &travel, &present));
	CHECK_FALSE(present);
	// AND THE POISON IS STILL THERE, which is what makes it poison rather than decoration: an absent
	// block must leave the caller's value alone so the caller can substitute its own default.
	CHECK(travel.offset[2].x == 9.0);
	// The caller substitutes the default when absent — as feathers_clap.cpp does — and the default is
	// the zero that changes nothing.
	const SoundTravel none;
	CHECK(none.offset[2].x == 0.0);
}

TEST_CASE("the drift channel refuses what it cannot mean") {
	ParamBridge bridge;
	CHECK(bridge.soundTravel() == SoundTravel {});
	CHECK_FALSE(bridge.takeStateDirty()); // nothing has moved yet

	bridge.setSoundTravel(2, { 0.4, -0.3 });
	CHECK(bridge.soundTravel().offset[2].x == 0.4);
	// AND IT SAID SO. Drift is patch state the host cannot see as a parameter, so the only way the
	// host learns the project changed is this flag — without it a drifted sound is a sound that
	// silently fails to be saved.
	CHECK(bridge.takeStateDirty());
	CHECK_FALSE(bridge.takeStateDirty()); // taking it clears it

	// A slot that does not exist, and values that are not numbers, leave everything where it was.
	const SoundTravel before = bridge.soundTravel();
	bridge.setSoundTravel(kMaxSounds, { 1.0, 1.0 });
	bridge.setSoundTravel(0, { std::numeric_limits<double>::quiet_NaN(), 0.0 });
	CHECK(bridge.soundTravel() == before);

	// A patch LOAD is not an edit: replacing every offset must not tell the host its project changed
	// the instant it finished opening it.
	bridge.takeStateDirty();
	SoundTravel loaded;
	loaded.offset[1] = { -0.2, 0.1 };
	bridge.setSoundTravel(loaded);
	CHECK(bridge.soundTravel() == loaded);
	CHECK_FALSE(bridge.takeStateDirty());
}

TEST_CASE("drift accumulates on the sound it names") {
	// The second verb, in the shape rotate has: increments onto the state of record, so a rewind is a
	// stored number restored rather than the increments subtracted back off.
	SoundTravel travel;
	travel.offset[3] = { 0.2, -0.1 };

	const SoundTravel once = driftBy(travel, 3, { 0.05, 0.05 });
	CHECK(once.offset[3].x == doctest::Approx(0.25));
	CHECK(once.offset[3].y == doctest::Approx(-0.05));
	// Nobody else moved.
	CHECK(once.offset[0] == travel.offset[0]);
	CHECK(once.offset[7] == travel.offset[7]);

	// Cumulative, and a zero drift is exactly the identity — which is what lets an encoder send one
	// every frame while nobody is touching it.
	SoundTravel walked = travel;
	for (int i = 0; i < 10; ++i) {
		walked = driftBy(walked, 3, { 0.01, 0.0 });
	}
	CHECK(walked.offset[3].x > once.offset[3].x);
	CHECK(driftBy(travel, 3, { 0.0, 0.0 }) == travel);

	// A slot past the end is ignored rather than clamped: there is no sound there, and clamping would
	// silently drift a different one.
	CHECK(driftBy(travel, kMaxSounds, { 1.0, 1.0 }) == travel);
}

TEST_CASE("a load is not an edit, for the orientation as well as the drift") {
	// A HOST TOLD ITS PROJECT CHANGED THE INSTANT IT FINISHED OPENING IT gives the user a save prompt
	// for having done nothing, and teaches them to ignore the prompt — which is worse than not having
	// one, because the next prompt is the real one. The drift channel split these two paths when it
	// landed; the orientation did not, and stateLoad calls straight into it.
	ParamBridge bridge;
	SliceOrientationPin pin;
	pin.pinned = true;
	pin.generator.rotate = 0.5f;

	bridge.takeStateDirty(); // clear whatever construction left
	bridge.restoreOrientation(pin);
	CHECK(bridge.orientation() == pin);      // the plane still arrives
	CHECK_FALSE(bridge.takeStateDirty());    // and the host is not told the project changed

	// A HAND on the same channel does say so, or the pin goes unsaved — which is the hole A8 was
	// written to close.
	pin.generator.rotate = 0.75f;
	bridge.setOrientation(pin);
	CHECK(bridge.orientation() == pin);
	CHECK(bridge.takeStateDirty());
}

TEST_CASE("a saved patch rebuilds the character it saved") {
	// THE CLAIM THE ROUND-TRIP TEST WAS NOT MAKING. Comparing the offsets back is necessary and not
	// sufficient: what a player loses when precision is dropped is the SOUND, and the offsets could
	// agree to six places while the built table did not. Through the old float persistence 0.37 came
	// back as 0.37000000476837158, which is a different character — and A11's promise that a rewind is
	// a stored number RESTORED stops being true the moment the number goes through a file.
	SoundTravel saved;
	saved.offset[0] = { 0.37, -0.21 };
	saved.offset[1] = { 1.0 / 3.0, 0.6180339887 };
	saved.offset[2] = { -0.05, 0.1 + 0.2 };

	MemoryStream stream;
	clap_ostream out = stream.out();
	REQUIRE(saveSoundTravel(&out, saved));
	SoundTravel loaded;
	bool present = false;
	clap_istream in = stream.in();
	REQUIRE(loadSoundTravel(&in, &loaded, &present));
	REQUIRE(present);

	// A patch with three sounds, at the offsets that were saved and at the offsets that came back.
	const auto read = [](ParamId id) {
		if (id == ParamId::CEnable) {
			return 1.0;
		}
		const ParamDesc* desc = findParam(static_cast<clap_id>(id));
		return desc != nullptr ? desc->def : 0.0;
	};
	const Character before = characterFromParams(read, OscType::Morph, AuditionTuning {},
	                                             AnchorPoint {}, saved, SoundOrientations {},
                                             SoundResidue {}, SoundGamma {}, Preview {}, EarSide::Head);
	const Character after = characterFromParams(read, OscType::Morph, AuditionTuning {},
	                                            AnchorPoint {}, loaded, SoundOrientations {},
                                            SoundResidue {}, SoundGamma {}, Preview {}, EarSide::Head);
	REQUIRE(before.count == 3);

	for (int i = 0; i < before.count; ++i) {
		CAPTURE(i);
		CHECK(before.sounds[i].travelX == after.sounds[i].travelX);
		CHECK(before.sounds[i].travelY == after.sounds[i].travelY);
	}
	// AGAINST AN INDEPENDENTLY BUILT REFERENCE, not against another call to the same mapping. The
	// previous version compared `before` with `after` and disclaimed the very mistake it made: omitting
	// a defaulted argument IS defaulting it, so those were one call and the memcmp could not fail. The
	// reference below derives each sound's plane from its own home — rotateFor with an empty pin,
	// which is what "the engine before this channel existed" actually did — and blends them at the
	// character's own weights.
	std::array<owlet::dsp::PhiMorphWavetable, 3> tables {};
	std::array<double, 3> weights {};
	for (int i = 0; i < after.count; ++i) {
		const SoundCoord& sound = after.sounds[i];
		tables[static_cast<size_t>(i)] =
		    buildMorphSliced(sound.zone, sound.phaseOffset, sound.travelX, sound.travelY,
		                     sliceBasisFor<kMorphTriangles>(
		                         rotateFor(SliceOrientationPin {}),
		                         sliceHome(sound.zone, sound.phaseOffset, 0.0)),
		                     0.0);
		weights[static_cast<size_t>(i)] = sound.weight;
	}
	const owlet::dsp::PhiMorphWavetable expected =
	    buildMorphBlend({ tables.data(), 3 }, { weights.data(), 3 });
	const owlet::dsp::PhiMorphWavetable built = buildMorphFor(after);
	CHECK(std::memcmp(&expected, &built, sizeof(expected)) == 0);

	// THE "COMPATIBILITY HALF" IS GONE, and dropping the default arguments is what exposed it: it
	// compared this against a call that omitted the channel, and omitting a defaulted argument IS
	// defaulting it — the two were one call written twice. With no default to omit, the redundancy is
	// unwriteable. The independent reference above is the whole claim.
}

TEST_CASE("a nonsense count is refused rather than cast") {
	// CONVERTING AN OUT-OF-RANGE FLOAT IS UNDEFINED, and the value it lands on can pass a range check
	// written against the RESULT: 1e30 becomes 0 on this compiler, 0 is not greater than 64, so the
	// block was accepted, no entries were read, and the stream stayed parked in the middle of the data.
	// Everything read after it comes out of the wrong bytes.
	//
	// Both loaders had it. The orientation's is older than the drift block and inherited the pattern.

	// Travel: tag, then the count.
	for (const float bad : { 1e30f, -1.0f, std::numeric_limits<float>::quiet_NaN() }) {
		CAPTURE(bad);
		MemoryStream stream;
		clap_ostream out = stream.out();
		const uint8_t tag[4] = { 'F', 'S', 'T', '1' };
		REQUIRE(out.write(&out, tag, 4) == 4);
		uint32_t bits = 0;
		std::memcpy(&bits, &bad, sizeof(bits));
		uint8_t bytes[4];
		for (int i = 0; i < 4; ++i) {
			bytes[i] = static_cast<uint8_t>((bits >> (i * 8)) & 0xffU);
		}
		REQUIRE(out.write(&out, bytes, 4) == 4);

		SoundTravel travel;
		bool present = false;
		clap_istream in = stream.in();
		CHECK_FALSE(loadSoundTravel(&in, &travel, &present));
	}
}

TEST_CASE("a patch carries the plane each sound is on") {
	// NOT FLOAT-EXACT VALUES, which is the drift block's lesson applied rather than copied. There the
	// fixture used numbers a float represents exactly and so could not see a float cast eating a
	// double. Here the state IS float, so the honest fixture is one whose floats are awkward — and the
	// test is that they survive unchanged.
	SoundOrientations saved;
	saved.pin[0].pinned = true;
	saved.pin[0].generator.rotate = 1.0f / 3.0f;
	saved.pin[4].pinned = true;
	saved.pin[4].generator.rotate = 0.1f + 0.2f;
	// Slot 7 is left UNPINNED on purpose: "derives from its home" has to survive a round trip too, or
	// a save would quietly freeze every sound onto whatever its home said that day.

	for (size_t chunk : { SIZE_MAX, size_t { 3 }, size_t { 1 } }) {
		CAPTURE(chunk);
		MemoryStream stream;
		stream.chunk = chunk;
		clap_ostream out = stream.out();
		REQUIRE(saveSoundOrientations(&out, saved));

		SoundOrientations loaded;
		bool present = false;
		clap_istream in = stream.in();
		REQUIRE(loadSoundOrientations(&in, &loaded, &present));
		CHECK(present);
		CHECK(loaded == saved);
		CHECK_FALSE(loaded.pin[7].pinned);
	}
}

TEST_CASE("a patch with no per-sound planes derives exactly as it always did") {
	// THE MIGRATION GUARANTEE. What makes it non-tautological is written where the comparison is made,
	// twenty lines down, rather than claimed here — an earlier version of this header described a call
	// with "no SoundOrientations argument at all", which stopped existing the day the defaults were
	// dropped and could not have been written afterwards.
	const auto read = [](ParamId id) {
		if (id == ParamId::CEnable) {
			return 1.0;
		}
		const ParamDesc* desc = findParam(static_cast<clap_id>(id));
		return desc != nullptr ? desc->def : 0.0;
	};

	// TRAVEL, so the planes can reach the sounds at all — an at-home comparison would agree whatever
	// the channel did.
	const SoundTravel travelled { { { { 0.3, -0.2 }, { -0.15, 0.25 }, { 0.4, 0.1 } } } };
	const SoundOrientations none;
	const Character after = characterFromParams(read, OscType::Morph, AuditionTuning {},
	                                            AnchorPoint {}, travelled, none, SoundResidue {}, SoundGamma {}, Preview {}, EarSide::Head);
	REQUIRE(after.count == 3);

	for (int i = 0; i < after.count; ++i) {
		CAPTURE(i);
		CHECK_FALSE(after.sounds[i].orientation.pinned);
		// SIGN BIT TOO. A rotate of −0.0 and one of +0.0 are the same number and different bits, and
		// the plane is built from it where the difference would vanish — so equality alone could hide
		// a channel that had quietly replaced the value with its own zero.
		CHECK_FALSE(std::signbit(after.sounds[i].orientation.generator.rotate));
	}
	// AGAINST AN INDEPENDENTLY BUILT REFERENCE. The previous version compared this against a call that
	// simply omitted the channel — and omitting a defaulted argument IS defaulting it, so the two were
	// one call written twice and the memcmp could not fail. Dropping the defaults made that
	// unwriteable; the reference below derives each sound's plane from its own home, which is what
	// "the engine before this channel existed" actually did, and blends them at the character's own
	// weights.
	std::array<owlet::dsp::PhiMorphWavetable, 3> planeTables {};
	std::array<double, 3> planeWeights {};
	for (int i = 0; i < after.count; ++i) {
		const SoundCoord& sound = after.sounds[i];
		planeTables[static_cast<size_t>(i)] =
		    buildMorphSliced(sound.zone, sound.phaseOffset, sound.travelX, sound.travelY,
		                     sliceBasisFor<kMorphTriangles>(
		                         rotateFor(SliceOrientationPin {}),
		                         sliceHome(sound.zone, sound.phaseOffset, 0.0)),
		                     0.0);
		planeWeights[static_cast<size_t>(i)] = sound.weight;
	}
	const owlet::dsp::PhiMorphWavetable derived =
	    buildMorphBlend({ planeTables.data(), 3 }, { planeWeights.data(), 3 });
	const owlet::dsp::PhiMorphWavetable built = buildMorphFor(after);
	CHECK(std::memcmp(&derived, &built, sizeof(derived)) == 0);

	// AND A PINNED SOUND IS AUDIBLY DIFFERENT, or the channel reaches nothing and the case above is
	// only proving that two zeros are equal.
	//
	// ISOLATED, which the first version of this was not: it compared a rotated-and-drifted character
	// against an undrifted one, so the drift alone made them differ and a build that ignored the plane
	// entirely passed. Both sides carry the SAME drift now — the plane is the only thing that moves,
	// and a sound needs travel before a plane can reach it at all.
	const Character straight = characterFromParams(read, OscType::Morph, AuditionTuning {},
	                                               AnchorPoint {}, travelled, none, SoundResidue {}, SoundGamma {}, Preview {}, EarSide::Head);
	SoundOrientations turned;
	turned.pin[0].pinned = true;
	// A REAL TURN IS THE ROTATE, and this control had to be restated to keep biting: it used to pin an
	// aperture and one of the other five angles, all of which M left unread, so the "pinned" sound
	// decoded to exactly the unpinned one and the assertion below compared a build against itself. It
	// went red the moment the decode changed, which is the case doing its job. The unread fields are
	// gone from the type now, so "nothing rides along" is enforced where it belongs.
	turned.pin[0].generator.rotate = 0.9f;
	const Character rotated = characterFromParams(read, OscType::Morph, AuditionTuning {},
	                                              AnchorPoint {}, travelled, turned, SoundResidue {}, SoundGamma {}, Preview {}, EarSide::Head);
	const owlet::dsp::PhiMorphWavetable c = buildMorphFor(straight);
	const owlet::dsp::PhiMorphWavetable d = buildMorphFor(rotated);
	CHECK(std::memcmp(&c, &d, sizeof(c)) != 0);
	CHECK(std::memcmp(&built, &c, sizeof(built)) == 0); // `straight` is the unrotated build
}

TEST_CASE("the per-sound plane channel refuses what it cannot mean") {
	ParamBridge bridge;
	CHECK(bridge.soundOrientations() == SoundOrientations {});
	bridge.takeStateDirty();

	SliceOrientationPin pin;
	pin.pinned = true;
	pin.generator.rotate = 0.42f;
	bridge.setSoundOrientation(2, pin);
	CHECK(bridge.soundOrientations().pin[2] == pin);
	CHECK(bridge.takeStateDirty()); // patch state the host cannot see, so it has to be told

	const SoundOrientations before = bridge.soundOrientations();
	SliceOrientationPin bad = pin;
	bad.generator.rotate = std::numeric_limits<float>::quiet_NaN();
	bridge.setSoundOrientation(2, bad);
	bridge.setSoundOrientation(kMaxSounds, pin);
	CHECK(bridge.soundOrientations() == before);

	// A load is not an edit, the same rule the travel channel and the orientation follow.
	bridge.takeStateDirty();
	SoundOrientations loaded;
	loaded.pin[5].pinned = true;
	loaded.pin[5].generator.rotate = 0.25f;
	bridge.setSoundOrientations(loaded);
	CHECK(bridge.soundOrientations() == loaded);
	CHECK_FALSE(bridge.takeStateDirty());

	// AND THE PLURAL IS THE PATCH ROAD, WHICH IS NOT THE ONE GUARDED ABOVE. The singular refuses a
	// non-finite turn; `setSoundOrientations` writes every slot straight into the atomics, and
	// `feathers_clap.cpp` calls THE PLURAL when a patch arrives. So the asymmetry that found the
	// gamma hole has a second spelling: not sibling against sibling, but SINGULAR AGAINST PLURAL —
	// the guard sits on the road a hand takes and not on the road a file takes.
	SoundOrientations poisonedSet = loaded;
	poisonedSet.pin[5].generator.rotate = std::numeric_limits<float>::quiet_NaN();
	bridge.setSoundOrientations(poisonedSet);
	CHECK(std::isfinite(bridge.soundOrientations().pin[5].generator.rotate));

	// The loader is the other half, and it is where a file actually enters: the angle is stored with
	// `pin.generator.rotate = angle` and nothing looks at it. A real round trip through the real
	// writer, with a finite neighbour so the claim cannot be "it dropped everything".
	{
		SoundOrientations poisoned;
		poisoned.pin[1].pinned = true;
		poisoned.pin[1].generator.rotate = std::numeric_limits<float>::quiet_NaN();
		poisoned.pin[3].pinned = true;
		poisoned.pin[3].generator.rotate = 0.375f;
		MemoryStream stream;
		clap_ostream out = stream.out();
		REQUIRE(saveSoundOrientations(&out, poisoned));

		SoundOrientations back;
		bool present = false;
		clap_istream in = stream.in();
		REQUIRE(loadSoundOrientations(&in, &back, &present));
		CHECK(present);
		CHECK(std::isfinite(back.pin[1].generator.rotate));
		CHECK(back.pin[1].generator.rotate == doctest::Approx(SliceOrientationPin {}.generator.rotate));
		CHECK(back.pin[3].generator.rotate == doctest::Approx(0.375f));
	}

	// THE TRAVEL CHANNEL HAS BOTH HALVES OF THE SAME HOLE. `loadSoundTravel` reads x and y as raw
	// doubles into `travel.offset[slot]`, and the plural `setSoundTravel` stores the set without
	// looking — while the singular, one function above it, refuses a non-finite offset.
	{
		SoundTravel poisoned;
		poisoned.offset[0] = { std::numeric_limits<double>::quiet_NaN(), 2.5 };
		poisoned.offset[2] = { 1.25, std::numeric_limits<double>::infinity() };
		poisoned.offset[4] = { -0.75, 0.5 }; // the finite neighbour
		MemoryStream stream;
		clap_ostream out = stream.out();
		REQUIRE(saveSoundTravel(&out, poisoned));

		SoundTravel back;
		bool present = false;
		clap_istream in = stream.in();
		REQUIRE(loadSoundTravel(&in, &back, &present));
		CHECK(present);
		// THE PAIR OR NEITHER, which is the SINGULAR setter's own rule — it refuses a point unless
		// both coordinates are finite, so keeping the good half here would make a file and a hand
		// answer one question differently. (0.25, 0) is a place the sound has never been.
		CHECK(back.offset[0].x == doctest::Approx(AnchorPoint {}.x));
		CHECK(back.offset[0].y == doctest::Approx(AnchorPoint {}.y));
		CHECK(back.offset[2].x == doctest::Approx(AnchorPoint {}.x));
		CHECK(back.offset[2].y == doctest::Approx(AnchorPoint {}.y));
		CHECK(back.offset[4].x == doctest::Approx(-0.75));
		CHECK(back.offset[4].y == doctest::Approx(0.5));

		ParamBridge fresh;
		fresh.setSoundTravel(poisoned);
		CHECK(fresh.soundTravel().offset[0].x == doctest::Approx(AnchorPoint {}.x));
		CHECK(fresh.soundTravel().offset[0].y == doctest::Approx(AnchorPoint {}.y));
		CHECK(fresh.soundTravel().offset[2].x == doctest::Approx(AnchorPoint {}.x));
		CHECK(fresh.soundTravel().offset[2].y == doctest::Approx(AnchorPoint {}.y));
		// AND THE GOOD SLOT IS UNTOUCHED — per PAIR, but still per SLOT.
		CHECK(fresh.soundTravel().offset[4].x == doctest::Approx(-0.75));
		CHECK(fresh.soundTravel().offset[4].y == doctest::Approx(0.5));

		// AND THE FEATURE SURVIVES THE GUARD, which is the whole risk of adding one here: this
		// quantity is UNBOUNDED BY DESIGN — a sound carried far along its line is the point — so a
		// huge FINITE magnitude must still travel untouched on both roads. A guard that took this
		// with it would be a worse bug than the one it fixes, and quieter.
		SoundTravel far;
		far.offset[1] = { -1.0e9, 8.5e12 };
		MemoryStream farStream;
		clap_ostream farOut = farStream.out();
		REQUIRE(saveSoundTravel(&farOut, far));
		SoundTravel farBack;
		bool farPresent = false;
		clap_istream farIn = farStream.in();
		REQUIRE(loadSoundTravel(&farIn, &farBack, &farPresent));
		CHECK(farBack.offset[1].x == doctest::Approx(-1.0e9));
		CHECK(farBack.offset[1].y == doctest::Approx(8.5e12));

		ParamBridge farBridge;
		farBridge.setSoundTravel(far);
		CHECK(farBridge.soundTravel().offset[1].x == doctest::Approx(-1.0e9));
		CHECK(farBridge.soundTravel().offset[1].y == doctest::Approx(8.5e12));

		SoundOrientations farTurn;
		farTurn.pin[2].generator.rotate = 5.0e6f;
		ParamBridge turnBridge;
		turnBridge.setSoundOrientations(farTurn);
		CHECK(turnBridge.soundOrientations().pin[2].generator.rotate == doctest::Approx(5.0e6f));
	}
}

TEST_CASE("a sound's own plane beats the audition's") {
	// Precedence, recorded as A12 and pinned here: the specific beats the general, so the global pin
	// stays usable as a reference while one sound is being rotated.
	SoundOrientations planes;
	planes.pin[1].pinned = true;
	planes.pin[1].generator.rotate = 0.8f;

	SliceOrientationPin global;
	global.pinned = true;
	global.generator.rotate = 0.2f;

	CHECK(resolvedOrientation(planes, 1, global).generator.rotate == 0.8f); // its own
	CHECK(resolvedOrientation(planes, 0, global).generator.rotate == 0.2f); // the audition's
	CHECK_FALSE(resolvedOrientation(planes, 0, SliceOrientationPin {}).pinned); // neither: derive
}

TEST_CASE("a saved patch rebuilds the planes it saved") {
	// The character-level round trip, which the equality check above does not make: the pins could
	// compare equal while the built table differed, and what a player loses is the SOUND. Same claim
	// the drift block gets, at the same level.
	SoundOrientations saved;
	saved.pin[0].pinned = true;
	saved.pin[0].generator.rotate = 1.0f / 3.0f;
	saved.pin[2].pinned = true;
	saved.pin[2].generator.rotate = -2.7182818f;

	MemoryStream stream;
	clap_ostream out = stream.out();
	REQUIRE(saveSoundOrientations(&out, saved));
	SoundOrientations loaded;
	bool present = false;
	clap_istream in = stream.in();
	REQUIRE(loadSoundOrientations(&in, &loaded, &present));
	REQUIRE(present);

	const auto read = [](ParamId id) {
		if (id == ParamId::CEnable) {
			return 1.0;
		}
		const ParamDesc* desc = findParam(static_cast<clap_id>(id));
		return desc != nullptr ? desc->def : 0.0;
	};
	// Travel on every sound, because a plane reaches nothing at home — a round trip compared at zero
	// travel would agree however badly the planes had been mangled.
	const SoundTravel travelled { { { { 0.3, -0.2 }, { -0.15, 0.25 }, { 0.4, 0.1 } } } };
	const Character before = characterFromParams(read, OscType::Morph, AuditionTuning {},
	                                             AnchorPoint {}, travelled, saved, SoundResidue {}, SoundGamma {}, Preview {}, EarSide::Head);
	const Character after = characterFromParams(read, OscType::Morph, AuditionTuning {},
	                                            AnchorPoint {}, travelled, loaded, SoundResidue {}, SoundGamma {}, Preview {}, EarSide::Head);
	const owlet::dsp::PhiMorphWavetable a = buildMorphFor(before);
	const owlet::dsp::PhiMorphWavetable b = buildMorphFor(after);
	CHECK(std::memcmp(&a, &b, sizeof(a)) == 0);
}

TEST_CASE("rotating a sound's plane cannot reach it at home, and reaches it once it has travelled") {
	// THE VERB'S HONESTY, at the character level rather than the basis level. test_slice_basis pins the
	// same guarantee on phaseFor/rateFor; this is the stronger venue, because it is the built table —
	// what a player hears — and it is PER SOUND, which the basis-level case cannot express.
	const auto read = [](ParamId id) {
		if (id == ParamId::CEnable) {
			return 1.0;
		}
		const ParamDesc* desc = findParam(static_cast<clap_id>(id));
		return desc != nullptr ? desc->def : 0.0;
	};

	// Sound A has drifted off its home; sounds B and C have not.
	const SoundTravel travelled { { { { 0.35, -0.2 } } } };

	SoundOrientations flat;
	const Character base = characterFromParams(read, OscType::Morph, AuditionTuning {}, AnchorPoint {},
	                                           travelled, flat, SoundResidue {}, SoundGamma {}, Preview {}, EarSide::Head);
	const owlet::dsp::PhiMorphWavetable reference = buildMorphFor(base);

	// A REAL TURN IS THE ROTATE — M reads that and nothing else, and since the shrink the type carries
	// nothing else either, so "nothing rides along" holds by construction rather than by vigilance.
	SliceRotation turn;
	turn.rotate = 0.85f;

	// TURNING A SOUND THAT IS AT HOME CHANGES NOTHING, bit for bit. Not "almost": the decode multiplies
	// every basis component by that sound's travel, and its travel is exactly zero.
	for (const size_t atHome : { size_t { 1 }, size_t { 2 } }) {
		CAPTURE(atHome);
		const SoundOrientations turned = rotateSoundBy(flat, atHome, SliceGenerator {}, turn);
		const Character c = characterFromParams(read, OscType::Morph, AuditionTuning {}, AnchorPoint {},
		                                        travelled, turned, SoundResidue {}, SoundGamma {}, Preview {}, EarSide::Head);
		const owlet::dsp::PhiMorphWavetable built = buildMorphFor(c);
		CHECK(std::memcmp(&reference, &built, sizeof(reference)) == 0);
	}

	// AND TURNING THE ONE THAT HAS TRAVELLED DOES REACH IT, or the plane is decoration.
	const SoundOrientations movedSound = rotateSoundBy(flat, 0, SliceGenerator {}, turn);
	const Character c = characterFromParams(read, OscType::Morph, AuditionTuning {}, AnchorPoint {},
	                                        travelled, movedSound, SoundResidue {}, SoundGamma {}, Preview {}, EarSide::Head);
	const owlet::dsp::PhiMorphWavetable built = buildMorphFor(c);
	CHECK(std::memcmp(&reference, &built, sizeof(reference)) != 0);
}

TEST_CASE("the rotate overload works out which plane it is turning") {
	// THE THREE-ARGUMENT FORM IS CORRECT AND DANGEROUS. It takes the plane to turn FROM, so a control
	// that has not worked out where the sound currently is passes a default-constructed generator —
	// which is the DEGENERATE plane, the line the engine was before any of this. The sound jumps there
	// on the first click of an encoder, silently, and the call is perfectly well-formed so nothing
	// downstream can object.
	const uint16_t zone = 512;
	const float offset = 0.0f;
	SliceRotation nudge;
	nudge.rotate = 0.05f;

	// A sound that has never been pinned: the overload must turn from the plane it is CURRENTLY
	// PLAYING, and under M that plane is rotate 0 at its own home. So the turn it stores is the nudge
	// itself.
	//
	// RESTATED ONTO THE PLANE, WHICH IS WHERE THE CLAIM ALWAYS LIVED. This case used to assert that the
	// stored generator came back carrying the φ-derived angles with the nudge added to them, because
	// under the old decode those numbers WERE the plane and arriving at zero meant arriving at the
	// degenerate line. Under M the plane is `M(rotate, home)` and the five other numbers reach nothing,
	// so an assertion about them is an assertion about bookkeeping. What has to be true — and what the
	// sound actually depends on — is that the plane after a nudge is the plane the sound was on, nudged:
	// continuous, not a jump. That is checked against a freshly decoded basis rather than against the
	// stored fields.
	const SoundOrientations none;
	const RotatedSound turned =
	    rotateSoundBy(none, SoundResidue {}, 0, zone, offset, SliceOrientationPin {}, nudge, {}, 0.0);
	const double home = sliceHome(zone, offset, 0.0);
	CHECK(turned.planes.pin[0].pinned);
	CHECK(turned.planes.pin[0].generator.rotate == doctest::Approx(nudge.rotate));

	const MorphBasis before = sliceBasisFor<kMorphTriangles>(0.0, home);
	const MorphBasis after =
	    sliceBasisFor<kMorphTriangles>(rotateFor(turned.planes.pin[0]), home);
	// It moved — a turn that changed no coefficient would be an encoder that does nothing...
	CHECK(std::memcmp(before.x.data(), after.x.data(), sizeof(after.x)) != 0);
	// ...and it moved by a NUDGE rather than by a jump, which is the property the old assertion was
	// reaching for. The bound is DERIVED FROM THE DECODE'S OWN CONSTANTS rather than picked: a triangle
	// climbs 4 per unit of its argument, the fastest coordinate reads the turn at φ⁻¹, the rate rows
	// read it 1.7× faster again, and each half swings by its own spread. Anything within that is the
	// plane being turned; anything beyond it is the plane being replaced. My first draft of this
	// assertion used a round 0.1 and went red on correct code — a bound nobody derived is a bound
	// nobody can read.
	const double climb = 4.0 * detail::kPlaneTurnRates[0] * detail::kRateRowTurn
	                     * std::max(detail::kPhaseSpread, detail::kRateSpread)
	                     * static_cast<double>(nudge.rotate);
	// AND THE BOUND IS REACHED EXACTLY, which is worth knowing rather than papering over: the fastest
	// rate coordinate reads the turn on a straight segment of its triangle, so it moves by the whole
	// climb and not a hair less. That makes this a tight bound rather than a generous one — and it also
	// means the comparison sits exactly on the boundary, where a coefficient computed in float against
	// a bound computed in double can land one ULP the wrong side. The allowance is that ULP and nothing
	// more; it is the difference between two spellings of the same number, not slack in the claim.
	const double ulp = climb * 1e-6;
	INFO("a turn of " << nudge.rotate << " rad can move a coefficient by at most " << climb);
	for (size_t i = 0; i < 2 * kMorphTriangles; ++i) {
		INFO("coordinate " << i);
		CHECK(std::abs(after.x[i] - before.x[i]) <= climb + ulp);
		CHECK(std::abs(after.y[i] - before.y[i]) <= climb + ulp);
	}
	// AT ZERO DRIFT THE TURN BANKS EXACTLY NOTHING. Both travel terms are ±0.0 there, so the residue is
	// untouched — the old held-at-home guarantee surviving as the decision's corollary rather than as a
	// second mechanism.
	CHECK(turned.residue == SoundResidue {});

	// AND AT A DRIFT IT BANKS, on the slot that turned and nowhere else.
	const RotatedSound atDrift = rotateSoundBy(none, SoundResidue {}, 0, zone, offset,
	                                           SliceOrientationPin {}, nudge, { 0.37, 0.1 + 0.2 }, 0.0);
	CHECK_FALSE(atDrift.residue.banked[0] == PlaneResidue {});
	CHECK(atDrift.residue.banked[1] == PlaneResidue {});
	// BOTH ENGINES, because the oscillator type is a parameter: a morph patch rotated at a drift and
	// switched to stair afterwards must not find its stair character had never heard of the turn.
	CHECK_FALSE(atDrift.residue.banked[0].morph == SliceResidue<kMorphTriangles> {});
	CHECK_FALSE(atDrift.residue.banked[0].stair == SliceResidue<kStairTriangles> {});
	// The plane it turned to is the same one either way — the fold is a second write, not a different
	// rotation.
	CHECK(atDrift.planes.pin[0].generator == turned.planes.pin[0].generator);

	// THE TRAP IS GONE, AND THAT IS THE RESTATEMENT RATHER THAN A DELETION. This assertion used to
	// prove the danger: turning from a default-constructed generator landed on the DEGENERATE plane,
	// somewhere else entirely, and that is what a caller got for guessing. Under M a default-constructed
	// generator is rotate 0 — precisely the plane an unpinned sound is already playing — so the
	// dangerous value and the correct value have become the same value, and the guess a caller might
	// make is now right by construction. Asserted in its new form so that the day it stops being true
	// is a red case rather than a silent return of the old bug.
	const SoundOrientations guessed = rotateSoundBy(none, 0, SliceGenerator {}, nudge);
	CHECK(guessed.pin[0].generator.rotate == doctest::Approx(turned.planes.pin[0].generator.rotate));

	// A sound that IS pinned turns from its own plane, and the global pin does not reach it.
	SoundOrientations pinned;
	pinned.pin[1].pinned = true;
	pinned.pin[1].generator.rotate = 0.6f;
	SliceOrientationPin global;
	global.pinned = true;
	global.generator.rotate = 0.2f;
	const RotatedSound again =
	    rotateSoundBy(pinned, SoundResidue {}, 1, zone, offset, global, nudge, {}, 0.0);
	CHECK(again.planes.pin[1].generator.rotate == doctest::Approx(0.6f + nudge.rotate));

	// An unpinned sound under a global pin turns from the GLOBAL plane, which is what "the plane it is
	// currently playing" means while the audition handle is held.
	const RotatedSound underGlobal =
	    rotateSoundBy(none, SoundResidue {}, 2, zone, offset, global, nudge, {}, 0.0);
	CHECK(underGlobal.planes.pin[2].generator.rotate == doctest::Approx(0.2f + nudge.rotate));
}

TEST_CASE("the residue road carries a turn, and a load is not an edit") {
	// THE THIRD CHANNEL OF THE SAME SHAPE, and it earns its own case rather than inheriting the drift
	// channel's: this one validates ALL OR NOTHING, which the others do not need to.
	ParamBridge bridge;
	CHECK(bridge.soundResidue() == SoundResidue {});

	PlaneResidue banked;
	banked.morph.offset[3] = 0.37;
	banked.stair.offset[1] = -0.125;
	bridge.takeStateDirty();
	bridge.setSoundResidue(2, banked);
	CHECK(bridge.soundResidue().banked[2] == banked);
	// A turn IS an edit, and the patch has to be told: without this a rotated sound is a sound that
	// silently fails to save, which is the hole A8 was written to close.
	CHECK(bridge.takeStateDirty());

	// A SPOILT FOLD IS REFUSED WHOLE. One infinity among eighty-eight coordinates would otherwise leave
	// the sound standing at a point that is neither where it was nor where the turn would have put it —
	// worse than refusing, because it is plausible.
	const SoundResidue before = bridge.soundResidue();
	PlaneResidue spoilt = banked;
	spoilt.morph.offset[10] = std::numeric_limits<double>::quiet_NaN();
	bridge.setSoundResidue(2, spoilt);
	CHECK(bridge.soundResidue() == before);
	bridge.setSoundResidue(kMaxSounds, banked); // a slot that does not exist
	CHECK(bridge.soundResidue() == before);

	// The whole set at once is what a patch load does, and a load is not an edit.
	SoundResidue loaded;
	loaded.banked[5].morph.offset[0] = 1.5;
	bridge.takeStateDirty();
	bridge.setSoundResidue(loaded);
	CHECK(bridge.soundResidue() == loaded);
	CHECK_FALSE(bridge.takeStateDirty());
}

TEST_CASE("a patch that banked nothing writes a HEADER rather than five kilobytes, or nothing") {
	// THE SIZE GUARANTEE SURVIVES; THE ABSENCE DOES NOT, AND THE CHANGE IS THE POINT OF THIS CASE.
	//
	// This block is 88 doubles a slot — five kilobytes of zeros in every save for a feature most
	// patches never touch — so it used to write NO BLOCK AT ALL when nothing was banked. That was
	// correct while it was the last thing in the stream. It became a bug the moment gamma's block was
	// appended after it: an absent middle block leaves the NEXT block's tag sitting exactly where this
	// block's loader reads, so the residue loader rejected the tag it found and a perfectly good patch
	// failed to load. The round-trip through the plugin's real state road is what caught it; every
	// block-level test here passed throughout, because each one reads its own block in isolation.
	//
	// So the empty case now writes a header with a slot count of zero: sixteen bytes instead of five
	// kilobytes, and no hole in the middle of the stream.
	MemoryStream stream;
	clap_ostream out = stream.out();
	REQUIRE(saveSoundResidue(&out, SoundResidue {}));
	CHECK(stream.bytes.size() < 64);
	CHECK_FALSE(stream.bytes.empty());

	// It reads back as present and empty — the same residue an absent block always meant.
	SoundResidue loaded;
	bool present = false;
	clap_istream in = stream.in();
	REQUIRE(loadSoundResidue(&in, &loaded, &present));
	CHECK(present);
	CHECK(loaded == SoundResidue {});

	// AND A PATCH THAT ENDS BEFORE THE BLOCK EVER EXISTED still reads as absent rather than as an
	// error. That is the migration path for everything written before this feature, and it is
	// untouched by the change above.
	MemoryStream empty;
	SoundResidue fromOld;
	bool oldPresent = true;
	clap_istream endIn = empty.in();
	REQUIRE(loadSoundResidue(&endIn, &fromOld, &oldPresent));
	CHECK_FALSE(oldPresent);
	CHECK(fromOld == SoundResidue {});
}

TEST_CASE("a saved patch rebuilds the sound its rotations left standing") {
	// A11'S PROMISE, THROUGH A FILE: a rewind is a stored number RESTORED, which stops being true the
	// moment the number goes through a file at the wrong width.
	//
	// WHICH HALF CARRIES THE WIDTH CLAIM, stated because the known-bad said so rather than the other way
	// round: persisting as floats fails the bit-equality below and does NOT move the built table at this
	// fixture's magnitudes. So the exactness assertion is the live one here, and the table comparison
	// that follows it is the weaker check — kept because it is the claim a player would make, not
	// because it is the sensitive one.
	SoundTravel drift;
	drift.offset[0] = { 0.37, 0.1 + 0.2 };

	SliceRotation turn;
	turn.rotate = -0.77f;
	const RotatedSound rotated =
	    rotateSoundBy(SoundOrientations {}, SoundResidue {}, 0, 512, 0.0f, SliceOrientationPin {}, turn,
	                  drift.offset[0], 0.0);
	REQUIRE_FALSE(rotated.residue.banked[0] == PlaneResidue {});

	MemoryStream stream;
	clap_ostream out = stream.out();
	REQUIRE(saveSoundResidue(&out, rotated.residue));
	SoundResidue loaded;
	bool present = false;
	clap_istream in = stream.in();
	REQUIRE(loadSoundResidue(&in, &loaded, &present));
	REQUIRE(present);

	// Bit for bit, because that is what the standing point is guaranteed to.
	CHECK(loaded == rotated.residue);

	// AND THROUGH THE MAPPING, which is where a wrong width would show as a wrong sound. Built against
	// the character the patch had BEFORE it was saved — an independently taken reference rather than a
	// second reading of the same call.
	const auto read = [](ParamId id) {
		if (id == ParamId::AZone) {
			return 512.0;
		}
		const ParamDesc* desc = findParam(static_cast<clap_id>(id));
		return desc != nullptr ? desc->def : 0.0;
	};
	const Character live = characterFromParams(read, OscType::Morph, AuditionTuning {}, AnchorPoint {},
	                                           drift, rotated.planes, rotated.residue, SoundGamma {}, Preview {}, EarSide::Head);
	const Character restored = characterFromParams(read, OscType::Morph, AuditionTuning {},
	                                               AnchorPoint {}, drift, rotated.planes, loaded, SoundGamma {}, Preview {}, EarSide::Head);
	const owlet::dsp::PhiMorphWavetable a = buildMorphFor(live);
	const owlet::dsp::PhiMorphWavetable b = buildMorphFor(restored);
	CHECK(std::memcmp(&a, &b, sizeof(a)) == 0);
	// The residue is doing something to that table, or the memcmp above is comparing a sound the
	// residue never touched.
	const Character without = characterFromParams(read, OscType::Morph, AuditionTuning {},
	                                              AnchorPoint {}, drift, rotated.planes,
	                                              SoundResidue {}, SoundGamma {}, Preview {}, EarSide::Head);
	const owlet::dsp::PhiMorphWavetable c = buildMorphFor(without);
	CHECK(std::memcmp(&a, &c, sizeof(a)) != 0);
}

TEST_CASE("a residue block with a nonsense count is refused before it is cast") {
	// THE UB THE OTHER LOADERS WERE CAUGHT BY. Converting an out-of-range float to size_t is undefined,
	// and the value it lands on can pass a range check written against the RESULT — 1e30 became 0 and
	// sailed through a `> 64`. Three counts here, so three chances to get it wrong.
	for (const size_t field : { size_t { 0 }, size_t { 1 }, size_t { 2 } }) {
		CAPTURE(field);
		MemoryStream stream;
		const uint8_t tag[4] = { 'F', 'S', 'R', '1' };
		stream.bytes.insert(stream.bytes.end(), tag, tag + 4);
		for (size_t i = 0; i < 3; ++i) {
			const float value = i == field ? 1e30f : 1.0f;
			uint32_t bits = 0;
			std::memcpy(&bits, &value, sizeof(bits));
			for (int b = 0; b < 4; ++b) {
				stream.bytes.push_back(static_cast<uint8_t>((bits >> (8 * b)) & 0xFF));
			}
		}
		SoundResidue loaded;
		bool present = false;
		clap_istream in = stream.in();
		CHECK_FALSE(loadSoundResidue(&in, &loaded, &present));
		CHECK_FALSE(present);
	}
}

TEST_CASE("the gamma base road carries a carry, and a load is not an edit") {
	// THE FOURTH CHANNEL OF THE SAME SHAPE. What it adds over the others is that the quantity is
	// UNBOUNDED by design, so nothing here clamps it and the test says so rather than leaving the
	// absence of a clamp looking like an oversight.
	ParamBridge bridge;
	CHECK(bridge.soundGamma() == SoundGamma {});

	bridge.takeStateDirty();
	bridge.setSoundGamma(2, 137.91);
	CHECK(bridge.soundGamma().base[2] == 137.91);
	// Carrying a sound along its line IS an edit: without this a sound moved by the Explore corner
	// silently fails to save, which is the hole A8 was written to close.
	CHECK(bridge.takeStateDirty());

	// Unbounded means unbounded. A base far outside anything a parameter could hold is the point of
	// the feature, not an error to be clamped away.
	bridge.setSoundGamma(3, -98765.4321);
	CHECK(bridge.soundGamma().base[3] == -98765.4321);

	// A slot that does not exist is ignored rather than clamped into a neighbour's.
	const SoundGamma before = bridge.soundGamma();
	bridge.setSoundGamma(kMaxSounds, 1.0);
	CHECK(bridge.soundGamma() == before);

	// AND UNBOUNDED IS NOT THE SAME AS NON-FINITE. The paragraph above is about MAGNITUDE — a base
	// far outside anything a parameter could hold is the feature — but infinity and NaN are not far
	// along the line, they are off it. The two sibling channels already drew this line:
	// `setSoundTravel` and `setSoundOrientation` each refuse a non-finite value while accepting any
	// magnitude, and this one did not. Three setters written together, two of them guarded.
	//
	// THE ROAD THAT REACHES IT IS A STANCE RECALL: `writeRideAlong` hands the banked `gammaBase`
	// straight to this setter, and the variations loader reads that value as a raw double.
	const SoundGamma held = bridge.soundGamma();
	bridge.setSoundGamma(1, std::numeric_limits<double>::quiet_NaN());
	CHECK(bridge.soundGamma() == held);
	bridge.setSoundGamma(1, std::numeric_limits<double>::infinity());
	CHECK(bridge.soundGamma() == held);
	bridge.setSoundGamma(1, -std::numeric_limits<double>::infinity());
	CHECK(bridge.soundGamma() == held);

	// The whole set at once is what a patch load does, and A LOAD IS NOT AN EDIT — otherwise opening
	// a patch reports the project as changed the instant it arrives.
	SoundGamma loaded;
	loaded.base[5] = 12345.678;
	bridge.takeStateDirty();
	bridge.restoreSoundGamma(loaded);
	CHECK(bridge.soundGamma() == loaded);

	// AND THE PATCH ROAD IS GUARDED WHERE IT ENTERS, WHICH IS NOT HERE. `restoreSoundGamma` writes
	// the whole set straight into the atomics and never passes `setSoundGamma`, so the guard above
	// cannot see a patch at all — `loadSoundGamma` drops a non-finite base instead, and this is that
	// claim, round-tripped through the real writer and reader.
	{
		SoundGamma poisoned;
		poisoned.base[2] = std::numeric_limits<double>::quiet_NaN();
		poisoned.base[3] = 4.25; // a real neighbour, so the drop cannot be "it dropped everything"
		MemoryStream stream;
		clap_ostream out = stream.out();
		REQUIRE(saveSoundGamma(&out, poisoned));

		SoundGamma back;
		bool present = false;
		clap_istream in = stream.in();
		REQUIRE(loadSoundGamma(&in, &back, &present));
		CHECK(present);
		CHECK(std::isfinite(back.base[2]));
		CHECK(back.base[2] == doctest::Approx(SoundGamma {}.base[2]));
		CHECK(back.base[3] == doctest::Approx(4.25));
	}
	CHECK_FALSE(bridge.takeStateDirty());
}

TEST_CASE("the gamma block round-trips, and a patch from before it reads as zero") {
	// Awkward values on purpose — none exactly representable, and one enormous — because a block that
	// only ever carried tidy numbers would not show a width bug.
	SoundGamma gamma;
	gamma.base[0] = 0.37;
	gamma.base[1] = -1.87;
	gamma.base[4] = 1234567.89;
	gamma.base[kMaxSounds - 1] = -0.0;

	MemoryStream stream;
	clap_ostream out = stream.out();
	REQUIRE(saveSoundGamma(&out, gamma));

	clap_istream in = stream.in();
	SoundGamma read;
	bool present = false;
	REQUIRE(loadSoundGamma(&in, &read, &present));
	CHECK(present);
	CHECK(read == gamma);
	// A double survives as a double. Written as a float, 1234567.89 would come back as 1234567.875 —
	// which is the quantisation this block exists to avoid, so it is asserted rather than assumed.
	CHECK(read.base[4] == 1234567.89);

	// A STREAM THAT ENDS BEFORE THE TAG is a patch written before the vernier existed. Absent, not
	// broken, and the zero base it keeps is what reproduces that patch to the bit.
	MemoryStream empty;
	clap_istream endIn = empty.in();
	SoundGamma fromOld;
	bool oldPresent = true;
	CHECK(loadSoundGamma(&endIn, &fromOld, &oldPresent));
	CHECK_FALSE(oldPresent);
	CHECK(fromOld == SoundGamma {});
}

TEST_CASE("a gamma block with a nonsense count is refused before it is cast") {
	// VALIDATE THEN CAST, the stage-3 lesson: an out-of-range float converts to whatever the hardware
	// feels like, and the value it lands on can pass a range check written against the RESULT. So the
	// check is on the float, before the conversion.
	MemoryStream stream;
	clap_ostream out = stream.out();
	const uint8_t tag[4] = { 'F', 'S', 'G', '1' };
	REQUIRE(out.write(&out, tag, sizeof(tag)) == sizeof(tag));
	const float insane = 1e30f;
	REQUIRE(out.write(&out, &insane, sizeof(insane)) == sizeof(insane));

	clap_istream in = stream.in();
	SoundGamma read;
	bool present = false;
	CHECK_FALSE(loadSoundGamma(&in, &read, &present));
	CHECK_FALSE(present);
}

TEST_CASE("a patch carries the stances each sound remembers") {
	// The second level of the tree, in the stream. Written even when nothing is stored (see
	// saveSoundVariations), so this covers both: a patch where somebody kept four stances for one sound
	// and one where nobody kept any.
	SoundVariations saved;
	// With FULL ride-along records (V20 — a stance stores the explore box): the whole-object
	// compare below is what proves the wire carries every member — a writer that dropped one would
	// come back as its default and fail `loaded == saved`. The residue and the pin are set to
	// non-defaults for exactly that reason.
	RideAlongValues explored { 0.4, 512.0, 0.3, 61.25, 0.02, -0.01, { true, { 0.44f } }, {} };
	explored.residue.morph.offset[0] = 0.017;
	explored.residue.stair.offset[3] = -0.005;
	// And a partial row (the additive engine, 2026-10-08), the last part of the wire.
	explored.residue.partials.offset[kPartialResidueValues - 3] = 0.0031;
	// AND THE RADIUS/THETA CORNERS, which joined the explore box on 2026-09-21 and ride LAST on the wire.
	explored.orbitRadius = 0.0125;
	explored.orbitTheta = -2.0;
	REQUIRE(storeVariation(saved, 2, explored)); // materialises the implicit stance too
	REQUIRE(storeVariation(saved, 2, { 0.85, 90.0 }));
	REQUIRE(storeVariation(saved, 5, { 0.05 }));
	REQUIRE(saved.sound[2].count == 3);
	REQUIRE(saved.sound[2].active == 2);
	REQUIRE(saved.sound[0].empty());

	std::array<AnchorPoint, kMaxSounds> livePlaces {};
	livePlaces[2] = { 0.3, -0.4 };
	for (size_t chunk : { SIZE_MAX, size_t { 3 }, size_t { 1 } }) {
		CAPTURE(chunk);
		MemoryStream stream;
		stream.chunk = chunk;
		clap_ostream out = stream.out();
		REQUIRE(saveSoundVariations(&out, saved, livePlaces));

		SoundVariations loaded;
		bool present = false;
		clap_istream in = stream.in();
		REQUIRE(loadSoundVariations(&in, &loaded, &present));
		CHECK(present);
		// THE WHOLE OBJECT: counts, active indices and every stored record, compared as one thing so
		// a field this block forgets to write fails rather than passing where nobody looked.
		CHECK(loaded == saved);

		// AND THE DEAD x,y BYTES CARRY THE LIVE PLACE — pinned as DEFENSE-IN-DEPTH, with the
		// review's correction on the record: NO SHIPPED BUILD walks these bytes. Every pre-V20
		// loader refuses this block at the HEADER (per-stance cap 64 against our 96) — a whole-load
		// refusal, never a partial read — so this assertion guards the honest-value convention for
		// hypothetical future readers and the writer's determinism, not a live compat path.
		// Byte offset spelled from the layout: tag(4) + three header floats(12) + slots 0 and 1
		// empty (count+active, 8 each) + slot 2's count+active(8) = 40.
		const auto floatAt = [&](size_t offset) {
			uint32_t bits = 0;
			for (int i = 0; i < 4; ++i) {
				bits |= static_cast<uint32_t>(stream.bytes[offset + static_cast<size_t>(i)])
				        << (i * 8);
			}
			float value = 0.0f;
			std::memcpy(&value, &bits, sizeof(value));
			return value;
		};
		CHECK(floatAt(40) == doctest::Approx(0.3f));
		CHECK(floatAt(44) == doctest::Approx(-0.4f));
	}

	// A patch where nothing was ever stored still writes a block, and it reads back as nothing stored —
	// which is the same state a patch from before this existed loads with.
	MemoryStream empty;
	clap_ostream out = empty.out();
	REQUIRE(saveSoundVariations(&out, SoundVariations {}, {}));
	SoundVariations loaded;
	bool present = false;
	clap_istream in = empty.in();
	REQUIRE(loadSoundVariations(&in, &loaded, &present));
	CHECK(present);
	CHECK(loaded == SoundVariations {});
}

TEST_CASE("a patch from before variations existed is not an error, and is not changed by opening it") {
	// THE MIGRATION CASE, and it is the load-bearing one for this whole level: a patch saved by
	// yesterday's build ends where the gamma block ends. It must read as "carries none" rather than as
	// corruption, and "carries none" must mean every sound has exactly the placement its parameters
	// hold — which is what the editor and the engine did before any of this was written.
	SoundGamma gamma;
	gamma.base[1] = 12.5;
	MemoryStream stream;
	clap_ostream out = stream.out();
	REQUIRE(saveSoundGamma(&out, gamma));

	clap_istream in = stream.in();
	SoundGamma loadedGamma;
	bool gammaPresent = false;
	REQUIRE(loadSoundGamma(&in, &loadedGamma, &gammaPresent));
	REQUIRE(gammaPresent);
	CHECK(loadedGamma.base[1] == doctest::Approx(12.5));

	// And then the reader for the block that is not there.
	SoundVariations variations;
	bool present = true;
	REQUIRE(loadSoundVariations(&in, &variations, &present));
	CHECK_FALSE(present);
	CHECK(variations == SoundVariations {});
}

TEST_CASE("a truncated or foreign variations block is refused rather than guessed at") {
	SoundVariations saved;
	REQUIRE(storeVariation(saved, 0, { 0.5 }));
	MemoryStream stream;
	clap_ostream out = stream.out();
	REQUIRE(saveSoundVariations(&out, saved, {}));

	MemoryStream truncated;
	truncated.bytes.assign(stream.bytes.begin(), stream.bytes.begin() + 8);
	SoundVariations loaded;
	bool present = false;
	clap_istream shortIn = truncated.in();
	CHECK_FALSE(loadSoundVariations(&shortIn, &loaded, &present));

	MemoryStream foreign = stream;
	foreign.bytes[2] = 'X';
	clap_istream wrongIn = foreign.in();
	CHECK_FALSE(loadSoundVariations(&wrongIn, &loaded, &present));
}

namespace {

/// Raw FSV bytes, written by hand — the orientation blocks' fixture idiom: THE BYTES ARE THE
/// CONTRACT, so a legacy patch is spelled out as the bytes an old build actually wrote rather than
/// asked of a writer this build no longer has.
void pushFloat(std::vector<uint8_t>& bytes, float value) {
	uint32_t bits = 0;
	std::memcpy(&bits, &value, sizeof(bits));
	for (int i = 0; i < 4; ++i) {
		bytes.push_back(static_cast<uint8_t>((bits >> (i * 8)) & 0xffU));
	}
}

/// Ride-along members are DOUBLES on the wire — see saveSoundVariations.
void pushDouble(std::vector<uint8_t>& bytes, double value) {
	uint64_t bits = 0;
	std::memcpy(&bits, &value, sizeof(bits));
	for (int i = 0; i < 8; ++i) {
		bytes.push_back(static_cast<uint8_t>((bits >> (i * 8)) & 0xffU));
	}
}

} // namespace

TEST_CASE("a legacy FSV1 block reads forever, its widths flagged absent") {
	// BACK COMPAT, against the bytes themselves: this stream is what every build between the
	// variations landing and V18 wrote — tag "FSV1", two header counts, two floats per stance —
	// and it must load with its places intact and its values reported ABSENT, so the caller can
	// apply the migration fill (live width, argued in audition.h) instead of a silent zero.
	MemoryStream stream;
	stream.bytes = { 'F', 'S', 'V', '1' };
	pushFloat(stream.bytes, 8.0f); // slots
	pushFloat(stream.bytes, 8.0f); // stances per sound
	// Slot 0: two stances. Slots 1..7: none.
	pushFloat(stream.bytes, 2.0f); // count
	pushFloat(stream.bytes, 1.0f); // active
	pushFloat(stream.bytes, 0.25f);
	pushFloat(stream.bytes, -0.5f);
	pushFloat(stream.bytes, -0.125f);
	pushFloat(stream.bytes, 0.75f);
	for (int slot = 1; slot < 8; ++slot) {
		pushFloat(stream.bytes, 0.0f);
		pushFloat(stream.bytes, 0.0f);
	}

	SoundVariations loaded;
	bool present = false;
	size_t valuesRead = 99;
	clap_istream in = stream.in();
	REQUIRE(loadSoundVariations(&in, &loaded, &present, &valuesRead));
	CHECK(present);
	CHECK(valuesRead == 0);
	REQUIRE(loaded.sound[0].count == 2);
	CHECK(loaded.sound[0].active == 1);
	// The legacy places are DROPPED — place left the stance (V20) — and the record's values are
	// defaults until the fill runs.
	CHECK(loaded.sound[0].values[0] == RideAlongValues {});

	// The fill turns every absent member into the slot's live value — the recall is a no-op on
	// every axis the stance never carried. STATED PLAINLY, NOT A PERFECT EMULATION (the review's
	// note, on the record): under the old rule recall never touched these axes FOREVER; under
	// live-fill it restores the LOAD-TIME values — change one after loading an old patch and a
	// recall reverts to load time where the old build did nothing. Accepted divergence: dev-era
	// window, converges to concrete stored values after one save.
	std::array<RideAlongValues, kMaxSounds> live {};
	live[0] = { 0.65, 300.0, 0.1 };
	fillAbsentStanceValues(loaded, live, valuesRead);
	CHECK(loaded.sound[0].values[0].width == doctest::Approx(0.65));
	CHECK(loaded.sound[0].values[1].width == doctest::Approx(0.65));
	CHECK(loaded.sound[0].values[0].zone == doctest::Approx(300.0));
	CHECK(loaded.sound[0].values[1].offset == doctest::Approx(0.1));
}

TEST_CASE("a width-only FSV2 record keeps its widths and fills the rest") {
	// THE SHIPPED FSV2 SHAPE — one value member per stance, width — read by the build whose records
	// carry the whole explore box. Width holding wire member 0 is what makes this a plain
	// count-driven read: the stored width LANDS, everything after it is absent and live-fills, so a
	// patch from the width-only window keeps exactly what it stored.
	MemoryStream stream;
	stream.bytes = { 'F', 'S', 'V', '2' };
	pushFloat(stream.bytes, 8.0f); // slots
	pushFloat(stream.bytes, 8.0f); // stances per sound
	pushFloat(stream.bytes, 1.0f); // value members per stance — the width-only era
	pushFloat(stream.bytes, 1.0f); // slot 0: one stance
	pushFloat(stream.bytes, 0.0f);
	pushFloat(stream.bytes, 0.5f);   // the era's stored place x — dead now, dropped
	pushFloat(stream.bytes, -0.25f); // and y
	pushDouble(stream.bytes, 0.7);   // the stored width
	for (int slot = 1; slot < 8; ++slot) {
		pushFloat(stream.bytes, 0.0f);
		pushFloat(stream.bytes, 0.0f);
	}

	SoundVariations loaded;
	bool present = false;
	size_t valuesRead = 0;
	clap_istream in = stream.in();
	REQUIRE(loadSoundVariations(&in, &loaded, &present, &valuesRead));
	CHECK(present);
	CHECK(valuesRead == 1);
	REQUIRE(loaded.sound[0].count == 1);
	CHECK(loaded.sound[0].values[0].width == doctest::Approx(0.7));

	std::array<RideAlongValues, kMaxSounds> live {};
	live[0] = { 0.99, 640.0 };
	fillAbsentStanceValues(loaded, live, valuesRead);
	// The stored width SURVIVES the fill — only members past valuesRead are absent.
	CHECK(loaded.sound[0].values[0].width == doctest::Approx(0.7));
	CHECK(loaded.sound[0].values[0].zone == doctest::Approx(640.0));
}

TEST_CASE("an FSV2 stream with more value members than this build knows still reads true") {
	// FORWARD COMPAT, the reason the third header count exists: a future build's stances carry a
	// member this one has never heard of, and this build must land the members it knows, DROP the
	// rest, and leave the stream position exactly at the block's end — proven by a sentinel float
	// placed after the block and read back, not assumed.
	MemoryStream stream;
	stream.bytes = { 'F', 'S', 'V', '2' };
	pushFloat(stream.bytes, 8.0f); // slots
	pushFloat(stream.bytes, 8.0f); // stances per sound
	pushFloat(stream.bytes, static_cast<float>(kRideAlongWireValues + 1)); // one member from the future
	pushFloat(stream.bytes, 1.0f); // slot 0: one stance
	pushFloat(stream.bytes, 0.0f);
	pushFloat(stream.bytes, 0.0f); // the dead place slots
	pushFloat(stream.bytes, 0.0f);
	// Every member this build knows, written through the same spelled order the writer uses...
	RideAlongValues future { 0.7, 512.0, 0.25 };
	future.residue.morph.offset[1] = 0.03;
	for (size_t member = 0; member < kRideAlongWireValues; ++member) {
		pushDouble(stream.bytes, rideAlongWireValue(future, member));
	}
	pushDouble(stream.bytes, 42.0); // ...and the stranger, to be read and dropped
	for (int slot = 1; slot < 8; ++slot) {
		pushFloat(stream.bytes, 0.0f);
		pushFloat(stream.bytes, 0.0f);
	}
	const float kSentinel = 1234.5f;
	pushFloat(stream.bytes, kSentinel);

	SoundVariations loaded;
	bool present = false;
	size_t valuesRead = 0;
	clap_istream in = stream.in();
	REQUIRE(loadSoundVariations(&in, &loaded, &present, &valuesRead));
	CHECK(present);
	CHECK(valuesRead == kRideAlongWireValues + 1);
	REQUIRE(loaded.sound[0].count == 1);
	CHECK(loaded.sound[0].values[0] == future);

	// The stream stands exactly at the block's end: the next read is the sentinel, whole.
	float next = 0.0f;
	uint8_t bytes[4];
	bool ended = false;
	REQUIRE(in.read(&in, bytes, sizeof(bytes)) == static_cast<int64_t>(sizeof(bytes)));
	uint32_t bits = 0;
	for (int i = 0; i < 4; ++i) {
		bits |= static_cast<uint32_t>(bytes[i]) << (i * 8);
	}
	std::memcpy(&next, &bits, sizeof(next));
	(void)ended;
	CHECK(next == doctest::Approx(kSentinel));
}

TEST_CASE("a legacy FSR1 residue block reads forever, and its vox rows are the zeros it never banked") {
	// THE WIRE CONTRACT, AGAINST THE BYTES THEMSELVES — and the case that made the tag bump worth
	// paying for. This stream is what every build wrote between the fold landing and V-C2: tag "FSR1",
	// THREE counts, and 88 doubles a slot. It must load with every morph and stair number in the row it
	// was written from, and with the twenty-four vox rows at exactly zero, because the build that wrote
	// it had no vox fold and therefore banked no vox difference. Zero is not a fallback here; it is the
	// only reading of that patch that reproduces the sound it was saved as.
	MemoryStream stream;
	stream.bytes = { 'F', 'S', 'R', '1' };
	pushFloat(stream.bytes, static_cast<float>(kMaxSounds));
	pushFloat(stream.bytes, static_cast<float>(kMorphResidueValues));
	pushFloat(stream.bytes, static_cast<float>(kStairResidueValues));
	// Distinct per row and per slot, so a shifted read lands on a value that is somebody else's rather
	// than on a plausible one.
	const auto legacyValue = [](size_t slot, size_t i) { return 1.0 + static_cast<double>(slot) + 0.001 * static_cast<double>(i + 1); };
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		for (size_t i = 0; i < kMorphResidueValues + kStairResidueValues; ++i) {
			pushDouble(stream.bytes, legacyValue(slot, i));
		}
	}
	const float kSentinel = 987.5f;
	pushFloat(stream.bytes, kSentinel);

	SoundResidue loaded;
	bool present = false;
	clap_istream in = stream.in();
	REQUIRE(loadSoundResidue(&in, &loaded, &present));
	REQUIRE(present);

	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		CAPTURE(slot);
		for (size_t i = 0; i < kMorphResidueValues + kStairResidueValues; ++i) {
			CAPTURE(i);
			// Read through the FLAT order, which is the order the bytes were written in — so this
			// checks the placement and the order in one assertion, and a vox row inserted anywhere but
			// the end moves a stair number out from under it.
			CHECK(planeResidueValue(loaded.banked[slot], i) == legacyValue(slot, i));
		}
		for (size_t i = 0; i < kVoxResidueValues; ++i) {
			CAPTURE(i);
			CHECK(loaded.banked[slot].vox.offset[i] == 0.0);
		}
		for (size_t i = 0; i < kWeaveResidueValues; ++i) {
			CAPTURE(i);
			CHECK(loaded.banked[slot].weave.offset[i] == 0.0);
		}
	}

	// The stream stands exactly at the block's end — the same guarantee every block here owes the one
	// after it, and the reason an absent middle block was a bug rather than a saving.
	uint8_t bytes[4];
	REQUIRE(in.read(&in, bytes, sizeof(bytes)) == static_cast<int64_t>(sizeof(bytes)));
	uint32_t bits = 0;
	for (int i = 0; i < 4; ++i) {
		bits |= static_cast<uint32_t>(bytes[i]) << (i * 8);
	}
	float next = 0.0f;
	std::memcpy(&next, &bits, sizeof(next));
	CHECK(next == doctest::Approx(kSentinel));
}

TEST_CASE("an FSR2 residue block — four counts, no weave rows — still loads, with the weave rows zero") {
	// The tag written before the Resonate engine's rows joined the fold (2026-09-25). Its morph, stair
	// and vox rows land where they were written from; the weave rows are the zero that is what an FSR2
	// patch sounded like; and the stream stands exactly at the block's end.
	MemoryStream stream;
	stream.bytes = { 'F', 'S', 'R', '2' };
	pushFloat(stream.bytes, static_cast<float>(kMaxSounds));
	pushFloat(stream.bytes, static_cast<float>(kMorphResidueValues));
	pushFloat(stream.bytes, static_cast<float>(kStairResidueValues));
	pushFloat(stream.bytes, static_cast<float>(kVoxResidueValues));
	const auto legacyValue = [](size_t slot, size_t i) { return 2.0 + static_cast<double>(slot) + 0.001 * static_cast<double>(i + 1); };
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		for (size_t i = 0; i < kPreWeaveResidueValues; ++i) {
			pushDouble(stream.bytes, legacyValue(slot, i));
		}
	}
	const float kSentinel = 321.5f;
	pushFloat(stream.bytes, kSentinel);

	SoundResidue loaded;
	bool present = false;
	clap_istream in = stream.in();
	REQUIRE(loadSoundResidue(&in, &loaded, &present));
	REQUIRE(present);
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		CAPTURE(slot);
		for (size_t i = 0; i < kPreWeaveResidueValues; ++i) {
			CAPTURE(i);
			CHECK(planeResidueValue(loaded.banked[slot], i) == legacyValue(slot, i));
		}
		for (size_t i = 0; i < kWeaveResidueValues; ++i) {
			CAPTURE(i);
			CHECK(loaded.banked[slot].weave.offset[i] == 0.0);
		}
	}
	uint8_t bytes[4];
	REQUIRE(in.read(&in, bytes, sizeof(bytes)) == static_cast<int64_t>(sizeof(bytes)));
	uint32_t bits = 0;
	for (int i = 0; i < 4; ++i) {
		bits |= static_cast<uint32_t>(bytes[i]) << (i * 8);
	}
	float next = 0.0f;
	std::memcpy(&next, &bits, sizeof(next));
	CHECK(next == doctest::Approx(kSentinel));
}

TEST_CASE("an FSR3 residue block — five counts, no partial rows — still loads, with the partial rows zero") {
	// The tag written before the additive engine's rows joined the fold (2026-10-08). Every older row
	// lands where it was written from; the partial rows are the zero that is what an FSR3 patch sounded
	// like; and the stream stands exactly at the block's end.
	MemoryStream stream;
	stream.bytes = { 'F', 'S', 'R', '3' };
	pushFloat(stream.bytes, static_cast<float>(kMaxSounds));
	pushFloat(stream.bytes, static_cast<float>(kMorphResidueValues));
	pushFloat(stream.bytes, static_cast<float>(kStairResidueValues));
	pushFloat(stream.bytes, static_cast<float>(kVoxResidueValues));
	pushFloat(stream.bytes, static_cast<float>(kWeaveResidueValues));
	const auto legacyValue = [](size_t slot, size_t i) { return 3.0 + static_cast<double>(slot) + 0.001 * static_cast<double>(i + 1); };
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		for (size_t i = 0; i < kPrePartialResidueValues; ++i) {
			pushDouble(stream.bytes, legacyValue(slot, i));
		}
	}
	const float kSentinel = 654.25f;
	pushFloat(stream.bytes, kSentinel);

	SoundResidue loaded;
	bool present = false;
	clap_istream in = stream.in();
	REQUIRE(loadSoundResidue(&in, &loaded, &present));
	REQUIRE(present);
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		CAPTURE(slot);
		for (size_t i = 0; i < kPrePartialResidueValues; ++i) {
			CAPTURE(i);
			CHECK(planeResidueValue(loaded.banked[slot], i) == legacyValue(slot, i));
		}
		for (size_t i = 0; i < kPartialResidueValues; ++i) {
			CAPTURE(i);
			CHECK(loaded.banked[slot].partials.offset[i] == 0.0);
		}
	}
	uint8_t bytes[4];
	REQUIRE(in.read(&in, bytes, sizeof(bytes)) == static_cast<int64_t>(sizeof(bytes)));
	uint32_t bits = 0;
	for (int i = 0; i < 4; ++i) {
		bits |= static_cast<uint32_t>(bytes[i]) << (i * 8);
	}
	float next = 0.0f;
	std::memcpy(&next, &bits, sizeof(next));
	CHECK(next == doctest::Approx(kSentinel));
}

namespace {

/// One FSR4 slot with `weave` weave numbers and `partial` partial numbers, every number distinct, then a
/// sentinel. The older parts carry this build's counts.
MemoryStream fsr4OneSlot(size_t weave, size_t partial, float sentinel) {
	MemoryStream stream;
	stream.bytes = { 'F', 'S', 'R', '4' };
	pushFloat(stream.bytes, 1.0f);
	pushFloat(stream.bytes, static_cast<float>(kMorphResidueValues));
	pushFloat(stream.bytes, static_cast<float>(kStairResidueValues));
	pushFloat(stream.bytes, static_cast<float>(kVoxResidueValues));
	pushFloat(stream.bytes, static_cast<float>(weave));
	pushFloat(stream.bytes, static_cast<float>(partial));
	for (size_t i = 0; i < kPreWeaveResidueValues; ++i) {
		pushDouble(stream.bytes, 0.25 + static_cast<double>(i));
	}
	for (size_t i = 0; i < weave; ++i) {
		pushDouble(stream.bytes, 1000.0 + static_cast<double>(i));
	}
	for (size_t i = 0; i < partial; ++i) {
		pushDouble(stream.bytes, 2000.0 + static_cast<double>(i));
	}
	pushFloat(stream.bytes, sentinel);
	return stream;
}

float nextFloat(clap_istream& in) {
	uint8_t bytes[4] = {};
	REQUIRE(in.read(&in, bytes, sizeof(bytes)) == static_cast<int64_t>(sizeof(bytes)));
	uint32_t bits = 0;
	for (int i = 0; i < 4; ++i) {
		bits |= static_cast<uint32_t>(bytes[i]) << (i * 8);
	}
	float next = 0.0f;
	std::memcpy(&next, &bits, sizeof(next));
	return next;
}

} // namespace

TEST_CASE("an FSR4 block from a build with more weave rows still lands its partial rows at their start") {
	// A future build with a twenty-eighth weave triangle writes two more weave numbers a slot. This build
	// keeps the weave rows it has, drops the stranger's two, and lands the partial numbers at partial
	// row 0 — not two rows in, and not in the weave rows. The case the weave part's WRITTEN bound is for.
	const float kSentinel = 777.5f;
	MemoryStream stream = fsr4OneSlot(kWeaveResidueValues + 2, kPartialResidueValues, kSentinel);
	SoundResidue loaded;
	bool present = false;
	clap_istream in = stream.in();
	REQUIRE(loadSoundResidue(&in, &loaded, &present));
	REQUIRE(present);
	for (size_t i = 0; i < kPreWeaveResidueValues; ++i) {
		CAPTURE(i);
		CHECK(planeResidueValue(loaded.banked[0], i) == 0.25 + static_cast<double>(i));
	}
	for (size_t i = 0; i < kWeaveResidueValues; ++i) {
		CAPTURE(i);
		CHECK(loaded.banked[0].weave.offset[i] == 1000.0 + static_cast<double>(i));
	}
	for (size_t i = 0; i < kPartialResidueValues; ++i) {
		CAPTURE(i);
		CHECK(loaded.banked[0].partials.offset[i] == 2000.0 + static_cast<double>(i));
	}
	CHECK(nextFloat(in) == doctest::Approx(kSentinel));
}

TEST_CASE("an FSR4 block from a build with more partial rows drops the extras and keeps its place in the stream") {
	const float kSentinel = 888.25f;
	MemoryStream stream = fsr4OneSlot(kWeaveResidueValues, kPartialResidueValues + 2, kSentinel);
	SoundResidue loaded;
	bool present = false;
	clap_istream in = stream.in();
	REQUIRE(loadSoundResidue(&in, &loaded, &present));
	REQUIRE(present);
	for (size_t i = 0; i < kWeaveResidueValues; ++i) {
		CAPTURE(i);
		CHECK(loaded.banked[0].weave.offset[i] == 1000.0 + static_cast<double>(i));
	}
	for (size_t i = 0; i < kPartialResidueValues; ++i) {
		CAPTURE(i);
		CHECK(loaded.banked[0].partials.offset[i] == 2000.0 + static_cast<double>(i));
	}
	CHECK(nextFloat(in) == doctest::Approx(kSentinel));
}

TEST_CASE("a residue that banked only partial rows is banked, and writes its rows") {
	// anyBanked decides whether the block carries any slots at all, so a sound whose only difference is
	// in the partial rows must count, or a save drops it and the load stands the sound somewhere else.
	SoundResidue residue;
	residue.banked[1].partials.offset[kPartialTriangles + 2] = -0.0125;
	CHECK(residue.anyBanked());

	MemoryStream stream;
	clap_ostream out = stream.out();
	REQUIRE(saveSoundResidue(&out, residue));
	SoundResidue loaded;
	bool present = false;
	clap_istream in = stream.in();
	REQUIRE(loadSoundResidue(&in, &loaded, &present));
	REQUIRE(present);
	CHECK(loaded.banked[1].partials.offset[kPartialTriangles + 2] == -0.0125);
	CHECK(loaded == residue);
}

TEST_CASE("the stance wire carries the partial rows after the weave rows, after the orbit") {
	// The wire is the flat order offset by eight, then the two orbit members, then everything from the
	// weave rows on. The partial rows follow weave there as they do in the flat order, so every member a
	// shipped record carries keeps its position. Values distinct per member, read back by position.
	RideAlongValues values;
	for (size_t i = 0; i < kPlaneResidueValues; ++i) {
		setPlaneResidueValue(values.residue, i, 100.0 + static_cast<double>(i));
	}
	values.orbitRadius = -1.5;
	values.orbitTheta = -2.5;
	// THE CURVE, THE KNOB COUNT AND THE KNOBS follow the partial rows (2026-10-10), the fixed-size curve first and the
	// growable knob run last, so the residue now ends at kRideAlongCurveWire.
	CHECK(feathers::kRideAlongCurveWire == 10 + kPlaneResidueValues);
	CHECK(feathers::kRideAlongKnobCountWire == feathers::kRideAlongCurveWire + 1 + feathers::spectral::kCurveBands);
	CHECK(feathers::kRideAlongKnobsWire == feathers::kRideAlongKnobCountWire + 1);
	CHECK(kRideAlongWireValues == feathers::kRideAlongKnobsWire + feathers::kPerSoundKnobCount);
	CHECK(rideAlongWireValue(values, 8 + kPreWeaveResidueValues) == -1.5);
	CHECK(rideAlongWireValue(values, 9 + kPreWeaveResidueValues) == -2.5);
	CHECK(rideAlongWireValue(values, 10 + kPrePartialResidueValues - 1) == values.residue.weave.offset[kWeaveResidueValues - 1]);
	CHECK(rideAlongWireValue(values, 10 + kPrePartialResidueValues) == values.residue.partials.offset[0]);
	CHECK(values.residue.partials.offset[0] == 100.0 + static_cast<double>(kPrePartialResidueValues));
	CHECK(rideAlongWireValue(values, feathers::kRideAlongCurveWire - 1)
	      == values.residue.partials.offset[kPartialResidueValues - 1]);
	values.knobs[2] = 0.375;
	values.curve.on = true;
	values.curve.gain[63] = 0.125f;
	CHECK(rideAlongWireValue(values, feathers::kRideAlongCurveWire) == 1.0);                              // the curve's switch
	CHECK(rideAlongWireValue(values, feathers::kRideAlongCurveWire + 64) == 0.125);                       // its last band
	CHECK(rideAlongWireValue(values, feathers::kRideAlongKnobCountWire) == double(feathers::kPerSoundKnobCount)); // the count
	CHECK(rideAlongWireValue(values, feathers::kRideAlongKnobsWire + 2) == 0.375);                        // a knob
	CHECK(rideAlongWireValue(values, kRideAlongWireValues - 1) == values.knobs[feathers::kPerSoundKnobCount - 1]);
	CHECK(rideAlongWireValue(values, kRideAlongWireValues) == 0.0); // past the record's end

	// And the setter is the reader's inverse over the whole record.
	RideAlongValues copy;
	for (size_t i = 0; i < kRideAlongWireValues; ++i) {
		setRideAlongWireValue(copy, i, rideAlongWireValue(values, i));
	}
	CHECK(copy.residue == values.residue);
	CHECK(copy.residue.partials == values.residue.partials);
	CHECK(copy == values); // the curve and the knobs too
}

TEST_CASE("the flat residue order addresses every row exactly once, and vox is at the end of it") {
	// THE ORDER IS THE MIGRATION GUARANTEE, so it is pinned as a property rather than trusted to a
	// comment: the bridge's atomics, the state block and the stance wire all lay these numbers out by
	// this one order, and an index that moved would silently re-point every saved patch's rows.
	//
	// EXHAUSTIVE AND DISTINCT: each index is written a value only it has, then every index is read
	// back. Two indices sharing a storage location fail the read-back; an index addressing nothing
	// fails it too. Zero inert slots, the same claim the vox addressing case makes for the basis.
	PlaneResidue residue;
	for (size_t i = 0; i < kPlaneResidueValues; ++i) {
		setPlaneResidueValue(residue, i, 1.0 + static_cast<double>(i));
	}
	for (size_t i = 0; i < kPlaneResidueValues; ++i) {
		CAPTURE(i);
		CHECK(planeResidueValue(residue, i) == 1.0 + static_cast<double>(i));
	}

	// AND THE BOUNDARIES BY NAME, because "vox appends" is the claim the FSR1 fixture above depends on
	// and a property test over the whole range would still pass with the three parts in any order.
	CHECK(kPlaneResidueValues
	      == kMorphResidueValues + kStairResidueValues + kVoxResidueValues + kWeaveResidueValues + kPartialResidueValues);
	CHECK(kPreWeaveResidueValues == kMorphResidueValues + kStairResidueValues + kVoxResidueValues);
	CHECK(kPrePartialResidueValues == kPreWeaveResidueValues + kWeaveResidueValues);
	CHECK(residue.morph.offset[kMorphResidueValues - 1] == 1.0 + static_cast<double>(kMorphResidueValues - 1));
	CHECK(residue.stair.offset[0] == 1.0 + static_cast<double>(kMorphResidueValues));
	CHECK(residue.stair.offset[kStairResidueValues - 1]
	      == 1.0 + static_cast<double>(kMorphResidueValues + kStairResidueValues - 1));
	CHECK(residue.vox.offset[0] == 1.0 + static_cast<double>(kMorphResidueValues + kStairResidueValues));
	CHECK(residue.vox.offset[kVoxResidueValues - 1] == 1.0 + static_cast<double>(kPreWeaveResidueValues - 1));
	// And the weave rows (the Resonate engine, 2026-09-25) after vox.
	CHECK(residue.weave.offset[0] == 1.0 + static_cast<double>(kPreWeaveResidueValues));
	CHECK(residue.weave.offset[kWeaveResidueValues - 1] == 1.0 + static_cast<double>(kPrePartialResidueValues - 1));
	// And the partial rows (the additive engine, 2026-10-08) after weave, at the very end.
	CHECK(residue.partials.offset[0] == 1.0 + static_cast<double>(kPrePartialResidueValues));
	CHECK(residue.partials.offset[kPartialResidueValues - 1] == 1.0 + static_cast<double>(kPlaneResidueValues - 1));
}

TEST_CASE("a turn banks the vox rows too, and they survive the file bit for bit") {
	// EVERY ENGINE IS FOLDED, WHICHEVER ONE IS PLAYING — the rule stair joined under, applied to vox
	// before it has a voice to be heard through. A player who rotates a drifted sound today and
	// switches to vox tomorrow must find the turns already in its character; a fold that started the
	// day the engine made sound would leave every older patch reaching vox by a different road.
	SoundTravel drift;
	drift.offset[0] = { 0.37, 0.1 + 0.2 };

	SliceRotation turn;
	turn.rotate = -0.77f;
	const RotatedSound rotated =
	    rotateSoundBy(SoundOrientations {}, SoundResidue {}, 0, 512, 0.0f, SliceOrientationPin {}, turn,
	                  drift.offset[0], 0.0);
	// The claim this case exists for: the turn moved vox rows, not only the two older engines'.
	REQUIRE_FALSE(rotated.residue.banked[0].vox == SliceResidue<kVoxTriangles> {});
	// And the weave rows (Resonate) on the same argument.
	REQUIRE_FALSE(rotated.residue.banked[0].weave == SliceResidue<kWeaveTriangles> {});
	// And the partial rows (the additive engine), once more.
	REQUIRE_FALSE(rotated.residue.banked[0].partials == SliceResidue<kPartialTriangles> {});

	MemoryStream stream;
	clap_ostream out = stream.out();
	REQUIRE(saveSoundResidue(&out, rotated.residue));
	SoundResidue loaded;
	bool present = false;
	clap_istream in = stream.in();
	REQUIRE(loadSoundResidue(&in, &loaded, &present));
	REQUIRE(present);
	// Bit for bit, doubles all the way through, because the standing point is guaranteed to it.
	CHECK(loaded == rotated.residue);
	CHECK(loaded.banked[0].vox == rotated.residue.banked[0].vox);
	CHECK(loaded.banked[0].weave == rotated.residue.banked[0].weave);
	CHECK(loaded.banked[0].partials == rotated.residue.banked[0].partials);
}

TEST_CASE("a residue block from a build with more vox rows than this one keeps its place in the stream") {
	// FORWARD COMPAT, the reason each part carries its own count: a build with a thirteenth vox
	// triangle writes two more rows a slot, and this one must land the rows it has, DROP the rest, and
	// leave the stream exactly at the block's end — proven by a sentinel, not assumed.
	MemoryStream stream;
	stream.bytes = { 'F', 'S', 'R', '2' };
	pushFloat(stream.bytes, 1.0f); // one slot, to keep the fixture small
	pushFloat(stream.bytes, static_cast<float>(kMorphResidueValues));
	pushFloat(stream.bytes, static_cast<float>(kStairResidueValues));
	pushFloat(stream.bytes, static_cast<float>(kVoxResidueValues + 2)); // one triangle from the future
	for (size_t i = 0; i < kMorphResidueValues + kStairResidueValues + kVoxResidueValues; ++i) {
		pushDouble(stream.bytes, 0.5 + static_cast<double>(i));
	}
	pushDouble(stream.bytes, 42.0); // the stranger's two rows, to be read and dropped
	pushDouble(stream.bytes, 43.0);
	const float kSentinel = 555.25f;
	pushFloat(stream.bytes, kSentinel);

	SoundResidue loaded;
	bool present = false;
	clap_istream in = stream.in();
	REQUIRE(loadSoundResidue(&in, &loaded, &present));
	REQUIRE(present);
	for (size_t i = 0; i < kPreWeaveResidueValues; ++i) {
		CAPTURE(i);
		CHECK(planeResidueValue(loaded.banked[0], i) == 0.5 + static_cast<double>(i));
	}
	// FSR2 carries no weave count, so the weave rows are the zero that is what an FSR2 patch sounded like.
	for (size_t i = 0; i < kWeaveResidueValues; ++i) {
		CAPTURE(i);
		CHECK(loaded.banked[0].weave.offset[i] == 0.0);
	}
	// Nor a partial count: the partial rows are zero too.
	for (size_t i = 0; i < kPartialResidueValues; ++i) {
		CAPTURE(i);
		CHECK(loaded.banked[0].partials.offset[i] == 0.0);
	}

	uint8_t bytes[4];
	REQUIRE(in.read(&in, bytes, sizeof(bytes)) == static_cast<int64_t>(sizeof(bytes)));
	uint32_t bits = 0;
	for (int i = 0; i < 4; ++i) {
		bits |= static_cast<uint32_t>(bytes[i]) << (i * 8);
	}
	float next = 0.0f;
	std::memcpy(&next, &bits, sizeof(next));
	CHECK(next == doctest::Approx(kSentinel));
}

TEST_CASE("an FSR2 header that ends before its fourth count is refused rather than half-read") {
	// The bump's own failure mode: a truncated or forged header that claims FSR2 and then stops. The
	// fourth count is not optional under that tag, and a loader that let it default would place the
	// payload by a count nobody wrote.
	MemoryStream stream;
	stream.bytes = { 'F', 'S', 'R', '2' };
	pushFloat(stream.bytes, 1.0f);
	pushFloat(stream.bytes, static_cast<float>(kMorphResidueValues));
	pushFloat(stream.bytes, static_cast<float>(kStairResidueValues));

	SoundResidue loaded;
	bool present = false;
	clap_istream in = stream.in();
	CHECK_FALSE(loadSoundResidue(&in, &loaded, &present));
	CHECK_FALSE(present);
}

TEST_CASE("storing and forgetting stances follow the mock's own rules") {
	// The mock: children are stored records, exactly one active, and "a sound is never unplaced —
	// the last variation can't be removed". Since V20 the record is the explore box (width leads
	// the aggregate below), and the structural rules are unchanged.
	SoundVariations list;

	// THE FIRST STORE KEEPS TWO. A sound with no stored stances already HAS one — the implicit
	// record its live state holds — so storing on such a sound means "keep this, and keep the copy
	// I am about to make of it", or the stance you were on would vanish from the list the moment
	// you asked to remember it.
	REQUIRE(storeVariation(list, 1, { 0.5 }));
	CHECK(list.sound[1].count == 2);
	CHECK(list.sound[1].active == 1);
	CHECK(list.sound[1].values[0] == RideAlongValues { 0.5 });
	CHECK(list.sound[1].values[1] == RideAlongValues { 0.5 });

	REQUIRE(storeVariation(list, 1, { 0.25, 700.0 }));
	CHECK(list.sound[1].count == 3);
	CHECK(list.sound[1].active == 2);

	// Removing the active one leaves the neighbour before it live, where the eye already is.
	REQUIRE(forgetVariation(list, 1, 2));
	CHECK(list.sound[1].count == 2);
	CHECK(list.sound[1].active == 1);

	// Removing one BEFORE the active shifts the active with it, so the same stance stays live.
	REQUIRE(storeVariation(list, 1, { 0.1, 100.0 }));
	REQUIRE(list.sound[1].count == 3);
	REQUIRE(list.sound[1].active == 2);
	REQUIRE(forgetVariation(list, 1, 0));
	CHECK(list.sound[1].count == 2);
	CHECK(list.sound[1].active == 1);
	CHECK(list.sound[1].values[1] == RideAlongValues { 0.1, 100.0 });

	// THE LAST ONE STAYS. A sound is never unplaced.
	REQUIRE(forgetVariation(list, 1, 1));
	REQUIRE(list.sound[1].count == 1);
	CHECK_FALSE(forgetVariation(list, 1, 0));
	CHECK(list.sound[1].count == 1);

	// And a sound that has stored nothing has nothing to forget.
	CHECK_FALSE(forgetVariation(list, 4, 0));

	// The list fills and then refuses, rather than overwriting the oldest: a stance you stored is not
	// something a later one may quietly take the place of.
	SoundVariations full;
	size_t stored = 0;
	while (storeVariation(full, 3, { static_cast<double>(stored) })) {
		++stored;
		REQUIRE(stored <= kMaxVariations); // or the loop below is the one that never ends
	}
	// The first call kept two, so the list fills one call earlier than the cap suggests — stated as the
	// arithmetic rather than as a number, since the first-store rule is the thing being relied on.
	CHECK(stored == kMaxVariations - 1);
	CHECK(full.sound[3].count == kMaxVariations);
	CHECK_FALSE(storeVariation(full, 3, { 9.0 }));
	CHECK(full.sound[3].count == kMaxVariations);
}

TEST_CASE("the gamma and variations blocks are read in the order they are written") {
	// THE SILENT-PATCH-CORRUPTION CLASS, pinned for the pair this tree actually has. Two tagged blocks
	// written by two authors who each appended "last" is how a writer and a reader come to disagree
	// about an order with nothing in a compiler able to notice: every patch then loads one block's
	// bytes as the other's, no error anywhere, and a carried gamma reads as a list of placements or
	// the reverse.
	//
	// The v5 merge is why this is worth having NOW rather than when it bites: that branch and this one
	// were never compiled together, and the next block appended by either side lands exactly here.
	// This case is what says so.
	//
	// impl-ui-15 has the same tripwire for their own pair (variations then routings) at feat/mod-panel
	// 2e3b370, which cannot run in this tree because ModRoutes does not exist here yet. When that
	// branch lands, the two cases together pin the whole tail of the stream.
	SoundGamma gamma;
	gamma.base[3] = -17.25;
	SoundVariations variations;
	REQUIRE(storeVariation(variations, 1, { 0.25 }));

	MemoryStream stream;
	clap_ostream out = stream.out();
	REQUIRE(saveSoundGamma(&out, gamma));
	REQUIRE(saveSoundVariations(&out, variations, {}));

	SUBCASE("read in the order they were written, both arrive whole") {
		clap_istream in = stream.in();
		SoundGamma readGamma;
		SoundVariations readVariations;
		bool gammaPresent = false;
		bool variationsPresent = false;
		REQUIRE(loadSoundGamma(&in, &readGamma, &gammaPresent));
		REQUIRE(loadSoundVariations(&in, &readVariations, &variationsPresent));
		CHECK(gammaPresent);
		CHECK(variationsPresent);
		CHECK(readGamma.base[3] == doctest::Approx(-17.25));
		CHECK(readVariations == variations);
	}

	SUBCASE("read in the other order, the tag refuses rather than consuming what it found") {
		// This is what the tags are FOR, and why a length prefix would not have done this job: a reader
		// that arrives at the wrong block refuses it instead of taking its bytes as its own.
		clap_istream in = stream.in();
		SoundVariations readVariations;
		bool variationsPresent = false;
		CHECK_FALSE(loadSoundVariations(&in, &readVariations, &variationsPresent));
		CHECK_FALSE(variationsPresent);
	}

	// WHAT THIS CASE CANNOT REACH, said rather than faked. It pins the two LOADERS against the two
	// SAVERS; the order the PLUGIN emits lives in feathers_clap.cpp's stateSave and stateLoad, and
	// there is no way to set a variation or a gamma base through the plugin's public surface — both
	// are editor-written state with no parameter behind them — so a round trip through clap_entry
	// could not carry a value to check. Verified by reading instead (stateSave: gamma then variations;
	// stateLoad the same), and the weakness worth naming for whoever lands the next block: that order
	// is implicit in two function bodies rather than stated in one shared place, which is exactly the
	// shape that let two authors both append "last".
}

TEST_CASE("backlog EF: the default patch routes its envelope to Level at full amount — a fresh bridge carries the route, a restore with no patch recreates it, and a patch that is present brings only its own") {
	// The default patch sets up this route, with a long release. Level's declared base is zero, so this
	// route is what makes a fresh instance sound at all, and the two places the default rack is installed are the two places it must appear.
	const backplane::Route wanted = defaultEnvelopeRoute();
	ParamBridge bridge;
	CHECK(routesOn(bridge) == TestRoutes { wanted });
	CHECK(bridge.routeAmount(wanted.source, wanted.destination) == doctest::Approx(1.0f));
	// THE ROUTE NAMES THE DEVICE THE DEFAULT RACK HOLDS — under the id it always wears, which is what
	// makes the route resolve rather than dangle.
	CHECK(bridge.rackDeviceAt(0) == feathers::kDefaultRackInstance);
	CHECK(bridge.modRack().count == 1);

	SUBCASE("a hand can take it away, and a restore with NO patch puts it back") {
		bridge.setRouteAmount(wanted.source, wanted.destination, 0.0f);
		CHECK(routesOn(bridge).empty());
		feathers::rack::RackPatch none;
		none.present = false;
		(void)bridge.restoreChains(none);
		CHECK(routesOn(bridge) == TestRoutes { wanted });
	}

	SUBCASE("a patch that IS present brings its own routes and not this one") {
		// An empty, present patch: what a hand that emptied the rack and saved would have. The restore
		// must not put an envelope route back into a patch whose author took it out.
		feathers::rack::RackPatch emptied;
		emptied.present = true;
		(void)bridge.restoreChains(emptied);
		CHECK(routesOn(bridge).empty());
	}

	SUBCASE("and it survives the bytes, because the save carries it like any route") {
		const Reloaded back = reload(throughBytes(bridge.chainsPatch()));
		CHECK(back.routes == TestRoutes { wanted });
		CHECK(back.refused.count == 0);
	}
}

namespace {

/// WHAT `sourceIsBipolar` (bridge_device_host.h) COMPUTES, restated here because that header pulls the
/// window toolkit in and this binary has no include path for it: the bridge's declared core sources
/// and its device view, asked as one `SourceView`.
bool declaredBipolar(const ParamBridge& bridge, backplane::Source source) {
	return backplane::SourceView(bridge.coreSourceDescs(), bridge.devices()).isBipolar(source);
}

} // namespace

TEST_CASE("backlog EG: a macro's polarity is a mode on the bridge — an edit that marks the patch, published as a declared source, restored without marking") {
	// The main structural macros have a mode switch (a context menu) that turns them into bipolar knobs
	// defaulting to 0. The parameter under a macro never changes
	// shape; what changes is how it is READ, and that is what these assertions are about.
	ParamBridge bridge;
	(void)bridge.takeStateDirty();
	for (size_t i = 0; i < feathers::kMacroCount; ++i) {
		CAPTURE(i);
		CHECK_FALSE(bridge.macroBipolar(i)); // every macro starts unipolar, as every one always was
		CHECK_FALSE(declaredBipolar(bridge, backplane::Source::core(i)));
	}
	CHECK(bridge.macroBipolarMask() == 0u);
	CHECK_FALSE(bridge.takeStateDirty());

	// THE SWITCH IS AN EDIT, and it is declared where the surfaces ask: the route from macro 2 reaches
	// both ways from the notch now, and a ring on any knob it is armed at draws so.
	bridge.setMacroBipolar(2, true);
	CHECK(bridge.macroBipolar(2));
	CHECK(bridge.takeStateDirty());
	CHECK(declaredBipolar(bridge, backplane::Source::core(2)));
	CHECK(declaredBipolar(bridge, backplane::Source::core(2)));
	CHECK_FALSE(declaredBipolar(bridge, backplane::Source::core(1)));
	CHECK(bridge.macroBipolarMask() == (1u << 2));
	// THE NAME RIDES WITH THE DECLARATION, from the parameter table, so a list naming the source and
	// a knob naming its parameter cannot disagree.
	CHECK(std::string(bridge.coreSourceDescs()[2].name) == "Macro 3");
	CHECK(bridge.coreSourceDescs().size() == feathers::kCoreSourceCount); // the macros, the weights, the Stack Index

	// SWITCHING TO THE MODE IT IS IN IS NOT AN EDIT.
	bridge.setMacroBipolar(2, true);
	CHECK_FALSE(bridge.takeStateDirty());
	// A macro this host does not have is refused, quietly.
	bridge.setMacroBipolar(feathers::kMacroCount + 3, true);
	CHECK_FALSE(bridge.takeStateDirty());

	// A RESTORE REPLACES EVERY MODE AT ONCE AND IS NOT AN EDIT — a patch load.
	MacroModes modes;
	modes.bipolar[0] = 1;
	bridge.restoreMacroModes(modes);
	CHECK(bridge.macroBipolar(0));
	CHECK_FALSE(bridge.macroBipolar(2)); // the restore's word replaced the hand's
	CHECK(bridge.macroModes() == modes);
	CHECK(bridge.macroBipolarMask() == 1u);
	CHECK(declaredBipolar(bridge, backplane::Source::core(0)));
	CHECK_FALSE(declaredBipolar(bridge, backplane::Source::core(2)));
	CHECK_FALSE(bridge.takeStateDirty());
	// HOW THE ENGINE HEARS IT — `publishEditorLanes` — is pinned where a Synth can be built
	// (test_mod_rack "backlog EG"); this binary has no window toolkit on its include path.
}

TEST_CASE("macro type and name edits dirty the patch") {
	// THE POLARITY'S RULES, on the two fields beside it: an edit that moves something marks the patch
	// and repaints; one that moves nothing is not an edit; a macro or a type this host lacks is refused.
	ParamBridge bridge;
	(void)bridge.takeStateDirty();
	(void)bridge.takeRepaintDirty();
	for (size_t i = 0; i < feathers::kMacroCount; ++i) {
		CAPTURE(i);
		CHECK(bridge.macroType(i) == device::MacroType::Knob); // what every macro always was
		CHECK(bridge.macroName(i) == "Macro " + std::to_string(i + 1));
		CHECK_FALSE(bridge.macroRenamed(i));
	}

	bridge.setMacroType(1, device::MacroType::Latch);
	CHECK(bridge.macroType(1) == device::MacroType::Latch);
	CHECK(bridge.macroModes().type[1] == device::MacroType::Latch);
	CHECK(bridge.takeStateDirty());
	CHECK(bridge.takeRepaintDirty());
	bridge.setMacroType(1, device::MacroType::Latch);
	CHECK_FALSE(bridge.takeStateDirty()); // the type it is already
	bridge.setMacroType(feathers::kMacroCount, device::MacroType::Momentary);
	CHECK_FALSE(bridge.takeStateDirty()); // no such macro
	bridge.setMacroType(1, device::MacroType::kCount);
	CHECK_FALSE(bridge.takeStateDirty()); // no such type
	CHECK(bridge.macroType(1) == device::MacroType::Latch);

	bridge.setMacroName(2, "Wobble");
	CHECK(bridge.macroName(2) == "Wobble");
	CHECK(bridge.macroModes().name[2] == "Wobble");
	CHECK(bridge.macroRenamed(2));
	CHECK(bridge.takeStateDirty());
	CHECK(bridge.takeRepaintDirty());
	bridge.setMacroName(2, "Wobble");
	CHECK_FALSE(bridge.takeStateDirty()); // the name it has already
	bridge.setMacroName(feathers::kMacroCount, "Nobody");
	CHECK_FALSE(bridge.takeStateDirty());
	CHECK(bridge.macroName(feathers::kMacroCount) == "Macro " + std::to_string(feathers::kMacroCount + 1));

	// A NAME PAST THE CAP IS CLAMPED, and the clamp is `clampMacroName`'s, not a second one here.
	const std::string longName(device::kMacroNameMax + 9, 'n');
	bridge.setMacroName(0, longName);
	CHECK(bridge.macroName(0) == std::string(device::kMacroNameMax, 'n'));
	CHECK(bridge.takeStateDirty());

	// THE DEFAULT SPELLED OUT IS NOT A RENAME, and clearing a real one is an edit back to it.
	bridge.setMacroName(3, "Macro 4");
	CHECK_FALSE(bridge.macroRenamed(3));
	CHECK(bridge.macroModes().name[3].empty());
	CHECK_FALSE(bridge.takeStateDirty());
	bridge.setMacroName(2, "");
	CHECK_FALSE(bridge.macroRenamed(2));
	CHECK(bridge.macroName(2) == "Macro 3");
	CHECK(bridge.takeStateDirty());
	bridge.setMacroName(0, "Macro 1");
	CHECK_FALSE(bridge.macroRenamed(0));
	CHECK(bridge.macroModes().name[0].empty());
	CHECK(bridge.takeStateDirty()); // it WAS renamed, so going back is a change

	// A RESTORE IS A PATCH LOAD, NOT AN EDIT, for these as for the polarity.
	MacroModes modes;
	modes.type[0] = device::MacroType::Momentary;
	modes.name[0] = "Kick";
	bridge.restoreMacroModes(modes);
	CHECK(bridge.macroType(0) == device::MacroType::Momentary);
	CHECK(bridge.macroName(0) == "Kick");
	CHECK_FALSE(bridge.takeStateDirty());
}

TEST_CASE("the rename listener hears setMacroName and restore") {
	// WHAT A9 HANGS ON: the host is told a macro's name moved, once per macro that moved, so it can ask
	// for a rescan of that parameter's info. A call for a name that did not move is a rescan for nothing.
	ParamBridge bridge;
	std::vector<size_t> heard;
	device::Subscription token = bridge.subscribeMacroRename([&heard](size_t index) { heard.push_back(index); });

	bridge.setMacroName(1, "Sweep");
	REQUIRE(heard.size() == 1);
	CHECK(heard[0] == 1);
	bridge.setMacroName(1, "Sweep");
	CHECK(heard.size() == 1); // unchanged: not heard
	bridge.setMacroName(2, "Macro 3");
	CHECK(heard.size() == 1); // the default spelled out: still the default, not heard
	bridge.setMacroType(1, device::MacroType::Latch);
	CHECK(heard.size() == 1); // a type is not a name

	// A RESTORE CALLS FOR EXACTLY THE MACROS WHOSE EFFECTIVE NAME CHANGED. Macro 1 keeps "Sweep" from
	// the edit above, so the restore that names 1 and 3 is written against that: 1 goes back to its
	// default, 3 gains a name, 0 and 2 stay default — and 0's name spelled as its default is no change.
	heard.clear();
	MacroModes modes;
	modes.name[0] = "Macro 1";
	modes.name[3] = "Bass";
	modes.type[2] = device::MacroType::Momentary;
	bridge.restoreMacroModes(modes);
	REQUIRE(heard.size() == 2);
	CHECK(heard[0] == 1);
	CHECK(heard[1] == 3);
	CHECK(bridge.macroName(0) == "Macro 1");
	CHECK(bridge.macroName(1) == "Macro 2");
	CHECK(bridge.macroName(3) == "Bass");

	// The same restore again moves nothing.
	heard.clear();
	bridge.restoreMacroModes(modes);
	CHECK(heard.empty());

	// Unsubscribed, nothing is called and nothing breaks.
	token.reset();
	bridge.setMacroName(0, "Lead");
	CHECK(heard.empty());
	CHECK(bridge.macroName(0) == "Lead");
}

TEST_CASE("two rename subscribers both hear, and dropping one token leaves the other") {
	// **THE EDITOR AND THE HOST BOTH LISTEN (A7, A9)**, and the editor closes while the host lives on.
	// With one listener slot, the second to subscribe overwrote the first and the editor clearing its
	// own silenced the host's; each now holds a token that ends only itself.
	ParamBridge bridge;
	std::vector<size_t> editor;
	std::vector<size_t> host;
	device::Subscription hostToken = bridge.subscribeMacroRename([&host](size_t index) { host.push_back(index); });
	auto editorToken = std::make_unique<device::Subscription>(
	    bridge.subscribeMacroRename([&editor](size_t index) { editor.push_back(index); }));

	bridge.setMacroName(2, "Pluck");
	CHECK(host == std::vector<size_t> { 2 });
	CHECK(editor == std::vector<size_t> { 2 });

	// THE EDITOR CLOSES: its token goes, and the host still hears — a restore included.
	editorToken.reset();
	bridge.setMacroName(0, "Kick");
	MacroModes modes;
	modes.name[3] = "Air";
	bridge.restoreMacroModes(modes);
	CHECK(host == std::vector<size_t> { 2, 0, 0, 2, 3 }); // the rename of 0, then the restore: 0 and 2 back to default, 3 named
	CHECK(editor == std::vector<size_t> { 2 });

	// A TOKEN THAT OUTLIVES ITS BRIDGE ends nothing and breaks nothing.
	device::Subscription orphan;
	{
		ParamBridge brief;
		orphan = brief.subscribeMacroRename([](size_t) {});
		CHECK(orphan.active());
	}
	CHECK_FALSE(orphan.active());
	orphan.reset();
}

TEST_CASE("a subscriber that drops another's token during notify: the other is not called in that notify") {
	// **THE WALK IS OVER A SNAPSHOT, BUT THE SNAPSHOT IS NOT THE ANSWER.** The second entry is still in
	// the copy when the first ends it; `notify` asks the live list before each call, so it is skipped.
	device::Subscribers<size_t> list;
	std::vector<std::string> heard;
	device::Subscription second;
	device::Subscription first = list.subscribe([&](size_t index) {
		heard.push_back("first " + std::to_string(index));
		second.reset();
	});
	second = list.subscribe([&](size_t index) { heard.push_back("second " + std::to_string(index)); });
	REQUIRE(list.size() == 2);

	list.notify(7);
	CHECK(heard == std::vector<std::string> { "first 7" });
	CHECK_FALSE(second.active());
	CHECK(list.size() == 1);
	// AND IT STAYS GONE on the next one.
	list.notify(8);
	CHECK(heard == std::vector<std::string> { "first 7", "first 8" });
}

TEST_CASE("a subscriber added during notify is not called in that notify, and is called on the next") {
	device::Subscribers<size_t> list;
	std::vector<std::string> heard;
	device::Subscription late;
	device::Subscription first = list.subscribe([&](size_t index) {
		heard.push_back("first " + std::to_string(index));
		if (!late.active()) {
			late = list.subscribe([&](size_t i) { heard.push_back("late " + std::to_string(i)); });
		}
	});

	list.notify(1);
	CHECK(heard == std::vector<std::string> { "first 1" });
	REQUIRE(late.active());
	CHECK(list.size() == 2);

	list.notify(2);
	CHECK(heard == std::vector<std::string> { "first 1", "first 2", "late 2" });
}

TEST_CASE("a moved-from token and a self-moved token end nothing twice") {
	device::Subscribers<size_t> list;
	int a = 0;
	int b = 0;
	device::Subscription keepA = list.subscribe([&a](size_t) { ++a; });
	device::Subscription source = list.subscribe([&b](size_t) { ++b; });
	REQUIRE(list.size() == 2);

	// MOVED FROM: the new token holds the subscription; the old one is empty, and its end — reset, then
	// destruction — ends nothing, least of all the subscription it handed over.
	device::Subscription moved = std::move(source);
	CHECK_FALSE(source.active()); // NOLINT(bugprone-use-after-move): the moved-from state is the claim
	CHECK(moved.active());
	source.reset();
	{
		device::Subscription scratch = std::move(source); // an empty token moved again stays empty
		CHECK_FALSE(scratch.active());
	}
	CHECK(list.size() == 2);
	list.notify(0);
	CHECK(a == 1);
	CHECK(b == 1);

	// SELF-MOVED: nothing happens — the token still holds, and is still heard. Through an alias so the
	// compiler's self-move warning does not have to be silenced to state it.
	device::Subscription& alias = moved;
	moved = std::move(alias);
	CHECK(moved.active());
	CHECK(list.size() == 2);
	list.notify(0);
	CHECK(b == 2);

	// ENDED ONCE, it ends exactly one subscription: `keepA` is still heard.
	moved.reset();
	moved.reset();
	CHECK(list.size() == 1);
	list.notify(0);
	CHECK(a == 3);
	CHECK(b == 2);

	// AND A MOVE-ASSIGN ONTO A LIVE TOKEN ends the one it held before taking the new one.
	device::Subscription other = list.subscribe([&b](size_t) { ++b; });
	REQUIRE(list.size() == 2);
	keepA = std::move(other);
	CHECK(list.size() == 1);
	list.notify(0);
	CHECK(a == 3);
	CHECK(b == 3);
}

TEST_CASE("the routings ride the bridge like every other thing that is not a parameter") {
	// HOST ENDS THROUGHOUT — macros into parameters, which is what a route between two of Feathers' own
	// things is. No device is named here on purpose: what this case is about is the amount and the
	// bracket around it, and a device end would bring the chain's own rules into a case that has
	// nothing to say about them.
	const backplane::Source macro2 = backplane::Source::core(2);
	const backplane::Destination param1 = backplane::Destination::core(1);
	ParamBridge bridge;
	// A FRESH BRIDGE ROUTES EXACTLY ONE THING — the default patch's envelope into Level (backlog EF) —
	// and nothing between host ends, which is what this case is about.
	CHECK(routesOn(bridge) == TestRoutes { defaultEnvelopeRoute() });

	bridge.takeStateDirty();
	bridge.beginRouteGesture();
	bridge.setRouteAmount(macro2, param1, 0.75f);
	bridge.endRouteGesture();

	CHECK(bridge.routeAmount(macro2, param1) == doctest::Approx(0.75f));
	// No parameter underneath an amount for a host gesture to be about, so what the bracket does is
	// mark the project changed — the drift offsets' rule, and the gamma bases'.
	CHECK(bridge.takeStateDirty());

	SUBCASE("an amount past the declared reach is clamped rather than taken") {
		const backplane::Source macro0 = backplane::Source::core(0);
		const backplane::Destination param0 = backplane::Destination::core(0);
		bridge.setRouteAmount(macro0, param0, 9.0f);
		CHECK(bridge.routeAmount(macro0, param0) == doctest::Approx(feathers::kMaxRouteAmount));
		bridge.setRouteAmount(macro0, param0, -9.0f);
		CHECK(bridge.routeAmount(macro0, param0) == doctest::Approx(-feathers::kMaxRouteAmount));
	}

	SUBCASE("a non-finite amount is refused, and the old one stands") {
		bridge.setRouteAmount(macro2, param1, std::numeric_limits<float>::quiet_NaN());
		CHECK(bridge.routeAmount(macro2, param1) == doctest::Approx(0.75f));
	}

	SUBCASE("a host index past the host's own table is ignored") {
		// **THE END IS THE HOST'S TABLE, NOT A MATRIX'S ROW (kill-lanes step 5).** This asked what a flat
		// `ModRoutes` did with a coordinate one past the end, and the answer that mattered was that it
		// did not FOLD — an index past the last destination landed inside the array on the NEXT SOURCE'S
		// row, so a bound on the flattened index alone wrote somebody else's amount and drew a ring on a
		// knob nobody pointed at. There is no flat array to fold into any more: a route is an edge, and a
		// host end is an index into Feathers' own table of sources or parameters. **So the FOLDING half
		// of the claim is gone with the thing that could fold**, and what survives is the bound itself —
		// a host end past the host's table names nothing and is refused. `kFirstDeviceSource` and
		// `kParamDestinationCount` are where those tables end.
		const TestRoutes before = routesOn(bridge);
		bridge.setRouteAmount(backplane::Source::core(feathers::kMacroCount + feathers::kWeightSourceCount),
		                      backplane::Destination::core(0), 1.0f);
		// **THE DESTINATION TABLE ENDS AT `kCoreDestinationCount` SINCE 2026-09-21**, not at
		// `kParamDestinationCount`: the core grew a second region above the parameters — a sound's
		// travel Radius and Theta, internal destinations that spend no CLAP id. One past THAT is the
		// edge this subcase is about.
		bridge.setRouteAmount(backplane::Source::core(0),
		                      backplane::Destination::core(feathers::kCoreDestinationCount), 1.0f);
		CHECK(routesOn(bridge) == before);

		// AND WHAT USED TO BE THE EDGE IS A REAL DESTINATION NOW — the control that keeps the lines
		// above about the boundary rather than about a number, exactly as the macro check below does
		// for the source end. This is the line that went red when the table grew, which is how the
		// subcase announced that its edge had moved.
		bridge.setRouteAmount(backplane::Source::core(0),
		                      backplane::Destination::core(feathers::kParamDestinationCount), 1.0f);
		CHECK(bridge.routeAmount(backplane::Source::core(0),
		                         backplane::Destination::core(feathers::kParamDestinationCount))
		      == doctest::Approx(1.0f));
		bridge.setRouteAmount(backplane::Source::core(0),
		                      backplane::Destination::core(feathers::kParamDestinationCount), 0.0f);

		// And the macros' old end is a real source now rather than the edge — the control that keeps the
		// lines above about the boundary rather than about the number four.
		//
		// AT LEVEL RATHER THAN AT THE FIRST DESTINATION, because the first destination is Path and a
		// weight source may not be pointed at the puck. This case is about the end of the host's own
		// table, and aiming it at a refused pair would have made it quietly about the rule instead.
		const int level = feathers::modDestinationIndexOf(ParamId::VoiceLevel);
		REQUIRE(level >= 0);
		const backplane::Source firstWeight = backplane::Source::core(feathers::kMacroCount);
		const backplane::Destination atLevel = backplane::Destination::core(static_cast<size_t>(level));
		bridge.setRouteAmount(firstWeight, atLevel, 1.0f);
		CHECK(bridge.routeAmount(firstWeight, atLevel) == doctest::Approx(1.0f));
	}

	SUBCASE("a restore is a patch arriving, not a player editing") {
		// THE ROUTES ARRIVE WITH THE CHAINS' PATCH (item 6: one format) — the separate route restore is gone
		// with the dense table (kill-lanes step 2), so the load is `restoreChains`, and it is still not an edit.
		const backplane::Source macro3 = backplane::Source::core(3);
		const backplane::Destination param4 = backplane::Destination::core(4);
		bridge.setRouteAmount(macro3, param4, -1.25f);
		const TestRoutes loaded = routesOn(bridge);
		REQUIRE(amountIn(loaded, macro3, param4) == doctest::Approx(-1.25f));
		const feathers::rack::RackPatch patch = bridge.chainsPatch();
		bridge.setRouteAmount(macro3, param4, 0.0f);
		REQUIRE(bridge.routeAmount(macro3, param4) == 0.0f);
		bridge.takeStateDirty();
		CHECK(bridge.restoreChains(patch).routes.count == 0);
		CHECK(routesOn(bridge) == loaded);
		CHECK_FALSE(bridge.takeStateDirty());
	}
}

TEST_CASE("ONE SEQUENCE: no two devices in either chain ever wear the same serial") {
	// **THE CASE `Graph::restore` NEEDED AND NOBODY HAD WRITTEN.** `backplane::Graph` keys its nodes
	// on `DeviceInstanceId`, so two devices under one id is two nodes at one address and the second is
	// refused outright — which is what a fixture handed Feathers' real rows discovered
	// (`tests/test_feathers_graph_order.cpp`, arc A's gating measurement). Before that,
	// `defaultModRack()` and `defaultFxChain()` each stamped `kFirstInstance`, so **a fresh instance
	// held two devices under one id before any gesture at all**.
	//
	// ASSERTED OVER BOTH CHAINS AT ONCE, which is the whole content of the change: each chain on its
	// own was already distinct and stayed distinct through every one of this file's other cases. The
	// collision only existed ACROSS them, so a per-chain assertion could not see it — and 145 cases
	// did not.
	const auto everySerial = [](const feathers::ParamBridge& b) {
		std::vector<feathers::DeviceInstanceId> out;
		const feathers::FxChain chain = b.fxChain();
		for (size_t position = 0; position < chain.count; ++position) {
			out.push_back(feathers::fxDeviceAtPosition(chain, position).row.instanceId);
		}
		const feathers::ModRack rack = b.modRack();
		for (size_t slot = 0; slot < rack.count; ++slot) {
			out.push_back(rack.device[slot].instanceId);
		}
		return out;
	};
	const auto allDistinct = [](std::vector<feathers::DeviceInstanceId> ids) {
		std::sort(ids.begin(), ids.end());
		return std::adjacent_find(ids.begin(), ids.end()) == ids.end();
	};

	namespace flt = feathers::fx::filter;
	namespace hrm = feathers::fx::harm;
	namespace dsr = feathers::mod::dahdsr;

	feathers::ParamBridge bridge;

	SUBCASE("a fresh instance, before any gesture — which is where the collision used to live") {
		const std::vector<feathers::DeviceInstanceId> ids = everySerial(bridge);
		REQUIRE(ids.size() >= 2); // a default lane and a default strip, or this proves nothing
		CHECK(allDistinct(ids));
	}

	SUBCASE("after adds ALTERNATING between the chains, which is what one counter has to survive") {
		// Alternating on purpose: two counters running in step produce their collisions exactly here,
		// and a case that filled one chain and then the other could be green on a design that still
		// had two of them.
		for (int i = 0; i < 3; ++i) {
			REQUIRE(bridge.addFxDevice(i % 2 == 0 ? hrm::kTraits.tag : flt::kTraits.tag)
			        < feathers::kMaxFxSlots);
			REQUIRE(bridge.addRackDevice(dsr::kTraits.tag) < feathers::kMaxRackSlots);
		}
		const std::vector<feathers::DeviceInstanceId> ids = everySerial(bridge);
		REQUIRE(ids.size() == 8); // 1 + 3 in the lane, 1 + 3 in the strip
		CHECK(allDistinct(ids));
	}

	SUBCASE("and a restore of both chains leaves the counter past BOTH, not past whichever was last") {
		// **THE ORDER-DEPENDENT HALF.** A state load restores the rack and the lane; with one counter
		// behind both, a restore that STORED rather than took the maximum would wind it back over the
		// ids the other chain is already using, and the very next add would collide. Which of the two
		// runs second decides whether that shows, so the restore is done in the order that hides it.
		feathers::FxChain chain = bridge.fxChain();
		for (int i = 0; i < 4; ++i) {
			REQUIRE(feathers::addFxDevice(&chain, hrm::kTraits.tag) < feathers::kMaxFxSlots);
		}
		bridge.restoreFxChain(chain); // a high serial...

		// **AND THE SECOND RESTORE MUST CARRY A GENUINELY LOWER ONE — `bridge.modRack()` WILL NOT DO.**
		// The first draft of this subcase used it and was VACUOUS: `modRack()` SEEDS its
		// `nextInstanceId` from the counter, so restoring what it hands back can never wind the counter
		// backwards, and a mutation replacing the maximum with a plain store passed all 146 cases.
		// Found by that known-bad, which is the whole reason to run one. `defaultModRack()` carries
		// `kFirstFreeInstance` and nothing else does, so it is the shape a state load actually
		// presents: a block written when the counter stood lower.
		const feathers::ModRack lowly = feathers::defaultModRack();
		REQUIRE(lowly.nextInstanceId < chain.nextInstanceId);
		bridge.restoreModRack(lowly);

		REQUIRE(bridge.addFxDevice(flt::kTraits.tag) < feathers::kMaxFxSlots);
		REQUIRE(bridge.addRackDevice(dsr::kTraits.tag) < feathers::kMaxRackSlots);
		CHECK(allDistinct(everySerial(bridge)));
	}
}

// **THE UNIFICATION GUARD THAT STOOD HERE IS RETIRED, 2026-09-10, and this note is its receipt.**
//
// It was called *"UNIFICATION GUARD: two devices sharing instance id 1 resolve to DIFFERENT
// destinations"*, and it existed to stop somebody deleting the per-chain KIND from a saved route as
// redundant. Its premise was that the fx lane and the strip each counted instance ids from one, so an
// id alone named two devices and only the kind told them apart.
//
// **THE PREMISE IS GONE, AND DELETING THE CASE IS THE HONEST CONSEQUENCE RATHER THAN THE THING IT
// WARNED ABOUT.** The kind was not removed because somebody decided it looked redundant; it was
// removed because the ids stopped colliding — `ParamBridge::nextInstance_` is one sequence across
// both chains (`plugin/device_instance.h`), which `backplane::Graph` required and which arc A's
// gating measurement forced. A guard whose premise is false does not fail loudly; it passes on a
// hand-built collision forever while asserting nothing about the shipping code.
//
// WHAT REPLACES IT, so the deletion is not a hole: `ONE SEQUENCE: no two devices in either chain ever
// wear the same serial` above, which asserts the property the kind used to work around, and the one
// format's loader (`rack_state.cpp`'s `Placement`), which REFUSES a route naming an id two devices in
// the patch wear rather than arbitrating — so the corruption this case was about is now impossible to
// express rather than merely disambiguated.


TEST_CASE("a stored route naming an id BOTH chains hold is refused rather than arbitrated") {
	// **THE LOAD SIDE OF THE ALIASING RULE.** One counter behind both chains means our writer never
	// produces two devices under one id; a hand-edited patch, or one from a build whose chains counted
	// separately, can. The loader gives the second device a fresh id — two nodes at one address is not
	// a state the record can hold — and then must REFUSE a route naming the doubled id rather than pick
	// whichever device was placed first.
	//
	// **FOUND BY A KNOWN-BAD, WHICH IS THE ONLY REASON IT EXISTS.** Deleting the refusal — arbitrating
	// to whichever chain was searched first — left all 145 cases in this file green, because the
	// shipping writer cannot produce that file and every fixture was built by the shipping writer.
	//
	// SO THE PATCH IS WRITTEN HONESTLY AND DOCTORED AFTERWARDS: the real writer writes a state whose ids
	// are distinct, then the strip's device is made to wear the fx device's id too.
	namespace flt = feathers::fx::filter;
	namespace dsr = feathers::mod::dahdsr;

	feathers::FxChain chain;
	chain.count = 0;
	chain.nextInstanceId = feathers::kFirstFreeInstance;
	REQUIRE(feathers::addFxDevice(&chain, flt::kTraits.tag) == 0);
	feathers::ModRack rack;
	rack.count = 0;
	rack.nextInstanceId = chain.nextInstanceId;
	REQUIRE(feathers::addRackDevice(&rack, dsr::kTraits.tag) == 0);

	// **THE TWO ENDS NAME THEIR DEVICES (kill-lanes step 5).** These were band coordinates, minted from
	// the positions, and the band bookkeeping that went with them — whether the two allocators had handed
	// out the same lane, and a static_assert that the bands do not overlap — was about an address space
	// that no longer exists. What the case is about is an IDENTITY worn by two devices at once, which is
	// exactly what an endpoint names, so the setup below is the whole of what it needs.
	const feathers::DeviceInstanceId fxId = feathers::instanceOfFxSlot(chain, 0);
	const feathers::DeviceInstanceId rackId = feathers::instanceOfSlot(rack, 0);
	REQUIRE(fxId != rackId); // written by a state that is NOT aliased — the file is honest

	const feathers::device::Descriptor* fxDesc = feathers::deviceForTag(flt::kTraits.tag);
	REQUIRE(fxDesc != nullptr);
	const size_t fxControl = fxDesc->controlOfNthModulatableBesideEnable(0);
	const backplane::Source macro0 = backplane::Source::core(0);
	const backplane::Destination into = backplane::Destination::control(fxId, fxControl);
	const feathers::rack::RackPatch honest = patchOf(chain, rack, { { macro0, into, 0.5f } });
	REQUIRE(honest.route.size() == 1);

	SUBCASE("THE CONTROL: read back as written, the route survives") {
		// Without this the refusal below is indistinguishable from a loader that drops everything.
		const Reloaded back = reload(honest);
		CHECK(amountIn(back.routes, macro0, into) == doctest::Approx(0.5f));
		CHECK(back.refused.count == 0);
	}

	SUBCASE("and with the strip's device ALSO wearing the fx device's id, the route is dropped") {
		feathers::rack::RackPatch aliased = honest;
		size_t doctored = 0;
		for (auto& d : aliased.device) {
			if (d.instanceId == rackId) {
				d.instanceId = fxId;
				++doctored;
			}
		}
		REQUIRE(doctored == 1);
		const Reloaded back = reload(aliased);
		// BOTH DEVICES ARE STILL THERE — the second under a fresh id — so the refusal is about the route,
		// not about a device the load lost.
		REQUIRE(back.chain.count == 1);
		REQUIRE(back.rack.count == 1);
		CHECK(feathers::instanceOfFxSlot(back.chain, 0) != feathers::instanceOfSlot(back.rack, 0));
		// **DROPPED, NOT MOVED.** The failure this refuses is not "no modulation" — it is the route
		// arriving on whichever device happened to be placed first, which is a patch that reopens
		// mis-wired and says nothing about it.
		CHECK(back.routes.empty());
		// Named by the two devices the load actually placed rather than by the coordinate each band's
		// first position answers to — which is the sharper form of the same assertion, because a device
		// that moved would take its name with it and a coordinate would not.
		const feathers::DeviceInstanceId placedFx = feathers::instanceOfFxSlot(back.chain, 0);
		const feathers::DeviceInstanceId placedRack = feathers::instanceOfSlot(back.rack, 0);
		CHECK(amountIn(back.routes, macro0, backplane::Destination::control(placedFx, fxControl))
		      == doctest::Approx(0.0f));
		const feathers::device::Descriptor* rackDesc = feathers::deviceForTag(dsr::kTraits.tag);
		REQUIRE(rackDesc != nullptr);
		CHECK(amountIn(back.routes, macro0,
		               backplane::Destination::control(placedRack, rackDesc->controlOfNthModulatableBesideEnable(0)))
		      == doctest::Approx(0.0f));
		// AND IT IS SAID, not swallowed.
		CHECK(back.refused.count == 1);
	}
}
TEST_CASE("the routes round-trip through the chains block, and a patch from before it reads as nothing routed") {
	// THE FAR CORNER OF THE RACK, NAMED BY ITS DEVICE — the last slot's LAST modulatable control. It was
	// the last live DESTINATION COORDINATE, and the qualifier mattered because the stride left slack
	// columns past each device's real width; a control index is the device's own name for a control, so
	// there is no slack to avoid and the "last live" is just the last one the descriptor declares. The
	// widest addressing this fixture can express is still what the case is reaching for.
	const feathers::DeviceInstanceId farId =
	    feathers::instanceOfSlot(rackForTest(), feathers::kMaxRackSlots - 1);
	REQUIRE(farId != feathers::kNoInstance);
	const feathers::device::Descriptor& farDesc =
	    rackDesc(feathers::kMaxRackSlots - 1);
	const backplane::Destination farCorner =
	    backplane::Destination::control(farId, farDesc.controlOfNthModulatable(farDesc.modulatableCount() - 1));

	const TestRoutes routes = sortedRoutes({
	    { backplane::Source::core(0), backplane::Destination::core(0), 0.37f },
	    { backplane::Source::core(1), backplane::Destination::core(3), -1.87f },
	    { backplane::Source::core(feathers::kMacroCount - 1), farCorner, feathers::kMaxRouteAmount },
	});

	const Reloaded read = reload(throughBytes(patchOf(feathers::defaultFxChain(), rackForTest(), routes)));
	CHECK(read.routes == routes);
	CHECK(read.refused.count == 0);

	// A STREAM THAT ENDS BEFORE THE TAG is a patch from before the block. Absent, not broken — and a
	// fresh instance's chains, which route exactly one thing: the default patch's envelope into Level
	// (backlog EF), recreated by the restore that finds no patch, and nothing the stream could have said.
	MemoryStream empty;
	clap_istream endIn = empty.in();
	feathers::rack::RackPatch fromOld;
	bool oldPresent = true;
	CHECK(feathers::loadChainsBlock(&endIn, &fromOld, &oldPresent));
	CHECK_FALSE(oldPresent);
	const Reloaded old = reload(fromOld);
	CHECK(old.routes == TestRoutes { defaultEnvelopeRoute() });
	CHECK(old.refused.count == 0);
}

TEST_CASE("a route the rules refuse is dropped and SAID, rather than dropped in silence") {
	// THE DEFECT (in the routings block this replaced, and the reason the one format's loader counts):
	// the loader asked maySourceRouteTo — correctly, because a file outlives the build
	// that wrote it and "the editor refuses to make one" and "one cannot exist" have to be the same
	// claim — and then dropped the amount with no word to anybody. The loss then persists into the
	// next save, so the patch comes back quieter than it was left and is written that way.
	//
	// NOT REACHABLE IN ANY SHIPPED BUILD: the rule and this gate arrived together, and the grid that
	// can arm a weight source lands after them, so no build has ever made a weight→puck route; routes
	// are not CLAP params either, so a host could not have made one behind the UI. That is what makes
	// this cheap to fix rather than unimportant — the moment any rule tightens, a source class added,
	// a destination withdrawn, a tenant removed from the lane, it becomes the silent-patch-corruption
	// class this repo has already fixed twice.
	//
	// A HAND-BUILT STREAM, because no writer in this build can produce one: the refusal is the point.
	size_t weightSource = feathers::kMacroCount + feathers::kWeightSourceCount;
	for (size_t source = 0; source < feathers::kMacroCount + feathers::kWeightSourceCount; ++source) {
		if (feathers::isWeightSource(source)) {
			weightSource = source;
			break;
		}
	}
	size_t puckDestination = feathers::kParamDestinationCount;
	for (size_t destination = 0; destination < feathers::kParamDestinationCount; ++destination) {
		if (feathers::isPuckDestination(destination)) {
			puckDestination = destination;
			break;
		}
	}
	REQUIRE(weightSource < feathers::kMacroCount + feathers::kWeightSourceCount);
	REQUIRE(puckDestination < feathers::kParamDestinationCount);
	// **ASKED OF THE AUTHORITY THE LOADER ASKS (kill-lanes step 5).** This asked
	// `retiredBandRule::maySourceRouteTo`, which compares two band-derived ranks — a rule the product
	// stopped having at step 4b. Both ends here are HOST ends, so the half of the answer that matters is
	// Feathers' own weight-may-not-reach-the-puck policy, and `ParamBridge::mayRoute` is where every
	// surface and the loader read it. A fresh bridge is the whole fixture a host-to-host pair needs.
	const backplane::Source weightEnd = backplane::Source::core(weightSource);
	const backplane::Destination puckEnd = backplane::Destination::core(puckDestination);
	ParamBridge gate;
	REQUIRE_FALSE(gate.mayRoute(weightEnd, puckEnd));

	// One refused route and one ordinary one, so the case can tell "dropped the bad one" from
	// "dropped everything".
	size_t goodSource = 0;
	size_t goodDestination = 0;
	const backplane::Source goodEnd = backplane::Source::core(goodSource);
	const backplane::Destination goodInto = backplane::Destination::core(goodDestination);
	REQUIRE(gate.mayRoute(goodEnd, goodInto));

	// HAND-NAMED, because the point is a file the EDITOR would never have produced. The refused route is
	// spelled by identity like any other; what makes it illegal is the rule, not the format. Through the
	// real bytes, so the refusal is the loader's and not an artefact of skipping the file.
	TestRoute bad = fromWeight(static_cast<uint32_t>(feathers::slotOfWeightSource(weightSource)));
	toParam(bad, feathers::destinationParamId(puckDestination));
	bad.amount = 0.75f;
	TestRoute good = fromMacro(static_cast<uint32_t>(goodSource));
	toParam(good, feathers::destinationParamId(goodDestination));
	good.amount = -0.5f;
	const Reloaded read = reload(throughBytes(patchWithRoutes({ bad, good })));

	// THE REFUSAL STILL HAPPENS. This half is what stops a future "fix" from simply admitting the
	// route, which would silence the message by making the rule a lie.
	CHECK(amountIn(read.routes, weightEnd, puckEnd) == doctest::Approx(0.0f));
	// And the ordinary route beside it is untouched, so this is a refusal rather than a wipe.
	CHECK(amountIn(read.routes, goodEnd, goodInto) == doctest::Approx(-0.5f));

	// AND IT IS SAID. One report for the load, naming what was dropped specifically enough to act on
	// — which source and which destination — because "my modulation stopped working" has to be able
	// to become "the build told me why".
	CHECK(read.refused.count == 1);
	// BY ENDPOINT (kill-lanes step 3b), and spelled the way every surface spells a Feathers host end:
	// `backplane::Source::core` over the mod-source index for a weight, `Destination::core` over the
	// destination index for a parameter — the same values `ui::weightSourceAt` and `modDestinationName`
	// work in, taken from the source of truth rather than from the editor's header, which a bridge test
	// has no business pulling in.
	//
	// BOTH ENDS RESOLVED HERE — a weight and the puck both exist — so this is the branch where the
	// report names two real things and the refusal is the RULE's, not a missing device's. That is what
	// makes it the counterpart of the "named a device this patch does not hold" case further down.
	CHECK(read.refused.firstSource == weightEnd);
	CHECK(read.refused.firstDestination == puckEnd);

	SUBCASE("and a patch with nothing refused reports nothing") {
		// The other half of the same claim: a message that appears on every load teaches nobody
		// anything. An ordinary patch must leave the report empty.
		const Reloaded back = reload(
		    throughBytes(patchOf(feathers::defaultFxChain(), rackForTest(), { { goodEnd, goodInto, 0.25f } })));
		CHECK(amountIn(back.routes, goodEnd, goodInto) == doctest::Approx(0.25f));
		CHECK(back.refused.count == 0);
	}
}

// "a routings block with a nonsense count is refused before it is cast" STOOD HERE and went with the
// routings block (chain-as-a-device item 6). Validate-then-cast on the one format's counts is
// `rack_state`'s to prove, in `tests/test_rack_state.cpp`, where the parser lives.


// A ROUTE'S LEGALITY, RESTATED INDEPENDENTLY OF THE CODE THAT DECIDES IT.
//
// The loader asks `ChainRecord::mayRoute`. A case that asked the same function would agree with it
// whatever it did — that is exactly the vacuity that let a device-blind predicate keep every slack
// column routable with the whole suite green. So this walks the declarations from the other side: the
// device's own `modulatable` flags for a destination, its own `outputs` span for a source, and the
// fixture's own knowledge of which chain holds which device for the order between them.
//
// It restates the RULES, not the implementation: weights may not reach the puck (by design), an
// output a device does not declare and a control it does not declare modulatable name nothing, and a
// device reaches only a device downstream of it (the strip first, then the lane, each in order).
namespace {

bool legalRouteIndependently(const TestRoute& r) {
	if (r.sourceKind == 2) {
		const feathers::device::Descriptor* sourceDesc =
		    fixtureDescOf(static_cast<feathers::DeviceInstanceId>(r.sourceA));
		if (sourceDesc == nullptr || r.sourceB >= sourceDesc->outputs.size()) {
			return false;
		}
	}
	if (r.destKind == 1) {
		const feathers::device::Descriptor* destDesc =
		    fixtureDescOf(static_cast<feathers::DeviceInstanceId>(r.destA));
		if (destDesc == nullptr || r.destB >= destDesc->params.size() || !destDesc->params[r.destB].modulatable) {
			return false;
		}
	}
	// THE ORDER IS THE FIXTURE'S OWN READING OF ITS CHAINS, not the authority's: the strip in order,
	// then the lane in order, a gap between so the two cannot tie.
	const auto placeOf = [](feathers::DeviceInstanceId id) -> size_t {
		const feathers::ModRack& rack = rackForTest();
		for (size_t at = 0; at < rack.count && at < rack.device.size(); ++at) {
			if (rack.device[at].instanceId == id) {
				return 1 + at;
			}
		}
		const feathers::FxChain chain = feathers::defaultFxChain();
		for (size_t at = 0; at < feathers::fxChainLength(chain); ++at) {
			if (feathers::fxDeviceAtPosition(chain, at).instanceId() == id) {
				return 1 + feathers::kMaxRackSlots + 1 + at;
			}
		}
		return 0; // a device nothing holds is in no order — and its descriptor lookup already refused it
	};
	// **TWO STRIP ENVELOPES ARE NOT ORDERED** (backlog IZ: a modulator's place in a lane is only grouping). They
	// were held in lane order by their NOTE road until 2026-10-03, when a road that only listens stopped ordering
	// anything (`NoteOps::listensOnly`) — so between two of them only a device reaching ITSELF is refused.
	const size_t sourcePlace = placeOf(static_cast<feathers::DeviceInstanceId>(r.sourceA));
	const size_t destPlace = placeOf(static_cast<feathers::DeviceInstanceId>(r.destA));
	const auto inStrip = [](size_t place) { return place >= 1 && place <= feathers::kMaxRackSlots; };
	if (r.sourceKind == 2 && r.destKind == 1 && inStrip(sourcePlace) && inStrip(destPlace)) {
		if (sourcePlace == destPlace) {
			return false;
		}
	}
	else if (r.sourceKind == 2 && r.destKind == 1 && sourcePlace >= destPlace) {
		return false;
	}
	if (r.sourceKind == 1 && r.destKind == 0) {
		const feathers::ParamId id = static_cast<feathers::ParamId>(r.destA);
		if (id == feathers::ParamId::Path || id == feathers::ParamId::PosX || id == feathers::ParamId::PosY) {
			return false;
		}
	}
	return true;
}

// THE GENERATOR'S STEPS, DERIVED FROM THE ENUMERATIONS RATHER THAN TUNED TO THEM. Hand-picked constants
// broke this file twice, both times the same way: the table grew, the largest generated amount crossed
// `kMaxRouteAmount`, and a placement check turned into a clamp check. Sized from the two lists' lengths,
// the largest amount stays under 1 whatever they hold.
float generatedAmount(size_t sourceIndex, size_t sourceCount, size_t destinationIndex, size_t destinationCount) {
	const float sourceStep = 1.0f / static_cast<float>(sourceCount + 1);
	const float destStep = sourceStep / static_cast<float>(destinationCount + 1);
	return static_cast<float>(sourceIndex) * sourceStep + static_cast<float>(destinationIndex) * destStep;
}

} // namespace

TEST_CASE("a reference this build cannot resolve is dropped, and the rest still load") {
	// THE SUCCESSOR TO "a table written by a build with more sources loads what it can".
	//
	// That case tested the DENSE format's forward compatibility: both table extents were written as
	// data, so a reader could walk a wider table and keep the rows it had. The sparse format has no
	// extents to disagree about — a route names its source and destination BY NAME — so the property
	// worth testing changed rather than disappeared. What can still go wrong is a name this build
	// does not know, and what must happen is that it is dropped, counted, and does not disturb its
	// neighbours.
	TestRoute good = fromMacro(0);
	toParam(good, feathers::ParamId::VoiceLevel);
	good.amount = -0.5f;

	// A DEVICE THAT IS NOT IN THIS RACK. The id is past anything rackForTest hands out, which is
	// exactly what a patch saved before a device was deleted looks like.
	//
	// NAMED ONCE, because the refusal is now asserted to REPORT THIS ID back (see the endpoint checks
	// at the end): a literal repeated at the write and the read could drift apart and still pass.
	constexpr auto kGoneId = static_cast<feathers::DeviceInstanceId>(9999);
	TestRoute goneSource = fromDevice(kGoneId, 0);
	toParam(goneSource, feathers::ParamId::VoiceLevel);
	goneSource.amount = 0.4f;

	TestRoute goneDest = fromMacro(1);
	toDevice(goneDest, kGoneId, 0);
	goneDest.amount = 0.4f;

	// A PARAMETER THIS BUILD HAS NEVER HEARD OF.
	TestRoute unknownParam = fromMacro(2);
	toParam(unknownParam, static_cast<feathers::ParamId>(0xBEEF));
	unknownParam.amount = 0.4f;

	// A SOURCE KIND FROM SOME LATER BUILD. Read and dropped rather than misread as one of ours.
	TestRoute alienKind = fromMacro(3);
	toParam(alienKind, feathers::ParamId::VoiceLevel);
	alienKind.sourceKind = 77;
	alienKind.amount = 0.4f;

	const Reloaded read = reload(throughBytes(patchWithRoutes({ goneSource, good, goneDest, unknownParam, alienKind })));

	// THE GOOD ONE SURVIVES, and it survives sitting between two bad ones — which is the half that
	// proves the reader stayed in step with the stream rather than giving up at the first refusal.
	const int level = feathers::modDestinationIndexOf(feathers::ParamId::VoiceLevel);
	REQUIRE(level >= 0);
	CHECK(amountIn(read.routes, backplane::Source::core(0),
	               backplane::Destination::core(static_cast<size_t>(level)))
	      == doctest::Approx(-0.5f));

	// AND THE FOUR BAD ONES ARE COUNTED, NOT SILENT. feathers-1a's point and the right one: silence
	// is how a dropped route becomes a bug report three weeks later — a patch loads, one modulation
	// is missing, and nothing anywhere says why. A count is what makes "swap is closed by
	// construction" different from "swap silently eats routes".
	CHECK(read.refused.count == 4);

	// **NAMED AS THE FILE NAMED IT, AND NOT HELD — TWO ASSERTIONS, AND THE CLAIM NEEDS BOTH.**
	//
	// The claim was "the first refusal named a device this patch does not hold, so it has no source in
	// this build's tables and the tray must not invent one", written as the one-past-the-end coordinate
	// `kModSourceCount`. A refusal reports the endpoint the FILE asked for now (`rack/rack_state.h`),
	// which makes the claim sharper rather than retiring it: the report says device 9999's first
	// output — the thing the file actually said — and the record does not hold 9999, which is the half
	// that makes it a refusal at all.
	//
	// ASSERTING ONLY "IT IS NOT A REAL SOURCE" WOULD BE THE WEAK HALF, and weak in the exact direction
	// this contract was changed to fix: it is satisfied by a report that threw the identity away, which
	// is precisely what the coordinate did. Asserting only the name would be satisfied by a build that
	// loaded device 9999 anyway and then refused the route for some other reason. Neither alone
	// distinguishes a correct report from a plausible wrong one; together they pin it.
	CHECK(read.refused.firstSource == feathers::backplane::Source::output(kGoneId, 0));
	CHECK_FALSE(holdsDevice(read, kGoneId));
	// AND IT IS A NAME RATHER THAN THE "CANNOT BE SPELLED" VALUE, which a file naming `kNoInstance`
	// would have produced — the tray prints those two differently and only this tells them apart.
	CHECK_FALSE(read.refused.firstSource == feathers::backplane::Source::nobody());
}


TEST_CASE("a macro reference lands on its macro however the tables have grown") {
	// THE SUCCESSOR TO "a patch from before the weight sources still lands its macros in the macro
	// rows". That case existed because the dense format addressed sources POSITIONALLY: appending a
	// class of sources shifted nothing only because the count was written as data, and the case
	// checked that the macro rows had not been transposed.
	//
	// The sparse format cannot express that bug — a macro is written as "macro 2", not as "row 2 of
	// however many rows this build has" — so this asserts the property that replaced it: a reference
	// is a NAME, and names do not move when tables grow. Written by hand rather than by the saver,
	// because the saver can only produce this build's own idea of the world.
	std::vector<TestRoute> routes;
	for (uint32_t macro = 0; macro < feathers::kMacroCount; ++macro) {
		TestRoute r = fromMacro(macro);
		toParam(r, feathers::ParamId::VoiceLevel);
		r.amount = 0.1f + 0.1f * static_cast<float>(macro);
		routes.push_back(r);
	}
	const Reloaded read = reload(throughBytes(patchWithRoutes(routes)));

	const int level = feathers::modDestinationIndexOf(feathers::ParamId::VoiceLevel);
	REQUIRE(level >= 0);
	const backplane::Destination atLevel = backplane::Destination::core(static_cast<size_t>(level));
	for (size_t macro = 0; macro < feathers::kMacroCount; ++macro) {
		CAPTURE(macro);
		CHECK(amountIn(read.routes, backplane::Source::core(macro), atLevel)
		      == doctest::Approx(0.1f + 0.1f * static_cast<float>(macro)));
	}
	// AND NOTHING LANDED ANYWHERE ELSE — the not-vacuous half. A reader that had transposed the table
	// would still satisfy the loop above if it happened to be off by a whole class.
	//
	// **STATED AS "THERE ARE NO OTHER ROUTES" (kill-lanes step 5)** rather than as a sweep of every other
	// source coordinate reading zero. The list holds what is routed and nothing else, so a route that
	// landed on the wrong source, on the wrong destination, or on a DEVICE is caught by the same two
	// lines — which the old sweep, bounded to the Level column, could not say.
	CHECK(read.routes.size() == feathers::kMacroCount);
	for (const backplane::Route& r : read.routes) {
		CAPTURE(r.source.index);
		CHECK_FALSE(r.source.isDevice());
		CHECK(r.source.index < feathers::kMacroCount);
		CHECK(r.destination == atLevel);
	}
	CHECK(read.refused.count == 0);
}


TEST_CASE("the activation weights travel the bridge, mask and all") {
	// THE PUBLICATION CHANNEL, in the shape the puck velocity's is: the engine writes, the editor
	// reads, nobody sets it and no gesture brackets it.
	feathers::ParamBridge bridge;

	// EMPTY BEFORE ANYTHING IS PLAYED, and empty means absent rather than eight zeros — a fresh bridge
	// has not been told what is sounding, and a meter drawn from it should show nothing rather than
	// eight silent sounds.
	CHECK(bridge.soundWeights().live == 0);

	feathers::SoundWeights published;
	published.set(0, 0.25);
	published.set(3, 0.75);
	bridge.publishSoundWeights(published);

	const feathers::SoundWeights read = bridge.soundWeights();
	CHECK(read == published);
	CHECK(read.isLive(0));
	CHECK(read.isLive(3));
	CHECK_FALSE(read.isLive(1));
	CHECK(read.at(0) == doctest::Approx(0.25));
	CHECK(read.at(3) == doctest::Approx(0.75));

	// PUBLISHING IS NOT AN EDIT. Nothing here is patch state — the weights are derived from the patch
	// rather than part of it — so a host must not be told the project changed because a puck moved.
	CHECK_FALSE(bridge.takeStateDirty());
}

TEST_CASE("a weight source reaches a destination the way a macro does") {
	// THE DORMANT MECHANISM, exercised. Nothing in the editor can create one of these routes yet — that
	// is a decision still to make — but the road has to exist and be right before a surface is drawn on
	// top of it, and a mechanism nobody has ever run is a mechanism that does not work.
	feathers::ParamSet params;
	params.setBase(static_cast<clap_id>(ParamId::Path), 0.0);

	const int destination = feathers::modDestinationIndexOf(ParamId::Path);
	REQUIRE(destination >= 0);
	feathers::test::Routes routes;
	routes.set(feathers::backplane::Source::core(feathers::weightSourceOf(2)),
	           feathers::backplane::Destination::core(static_cast<size_t>(destination)), 1.0f);
	routes.publish(params);

	// Nothing published yet: the source reads zero and the destination has not moved.
	CHECK(params.resolved(ParamId::Path) == doctest::Approx(0.0));

	feathers::SoundWeights weights;
	weights.set(2, 0.5);
	params.setWeightSources(weights);
	// Half a share at a whole amount is half of Path's range, which is the same arithmetic a macro at
	// half would have produced — the point of the two kinds sharing everything after `sourceValue`.
	CHECK(params.resolved(ParamId::Path) == doctest::Approx(0.5));

	// AND ABSENT PUSHES NOWHERE. A sound switched off is not a source holding still at zero, but for a
	// destination the two are the same thing, and this is the only place in the design where they are.
	feathers::SoundWeights gone;
	gone.set(1, 1.0);
	params.setWeightSources(gone);
	CHECK(params.resolved(ParamId::Path) == doctest::Approx(0.0));
}

TEST_CASE("the weight sources are invisible to the host") {
	// THE MEMBRANE, checked rather than promised. By standing design these are internal
	// sources: no ids, no enumeration, never routable from outside. A parameter table that had grown a
	// "Sound C Weight" entry would have broken it silently, and the fingerprint cases would have caught
	// the fact without saying why.
	for (const ParamDesc& desc : paramDescs()) {
		CAPTURE(std::string(desc.name));
		CHECK(std::string_view { desc.name }.find("Weight") == std::string_view::npos);
	}
	// And the count is what it was: the sources spend no ids, so the table did not grow.
	CHECK(paramDescs().size() == feathers::kParamCount);
}

// **"a route to the LAST destination reaches the engine, through both copies of the table" STOOD HERE AND
// IS DELETED (kill-lanes §2.4b).** It built the engine's route table at the far corner of the flat
// coordinate space beside the bridge's, to catch an off-by-one between two copies of one table. There is
// one store — the record's, by identity — and no coordinate space to have a far corner.

TEST_CASE("a weight source may be pointed at a filter parameter") {
	// THE OTHER HALF OF THE PUCK RULE, stated where the rule is: what maySourceRouteTo refuses is a
	// weight source aiming at PATH, POS X or POS Y, because a weight is a function of where the puck
	// stands. A filter parameter does not feed the blend — nothing downstream of it reaches a weight —
	// so there is no loop to foreclose and every source may reach it.
	//
	// Written as its own case because the refusal widening by accident is the plausible failure: the
	// predicate is asked by the gate, by the glow and by the loader, and a version of it that refused
	// weight sources generally would pass every existing case about the puck.
	feathers::ParamBridge bridge;
	// THE LANE IS RESTORED ONTO THE BRIDGE FIRST, and it is not ceremony: a device destination is
	// resolved through the bridge's OWN chain, so a bridge whose slot carries no tag resolves no
	// device and refuses every write to it. That is the fixture trap in the playbook — a fixture that
	// takes its values from what the code expects rather than from the platform's contract — and the
	// contract here is that a slot holds a device only once something has put one there.
	//
	// A FRESH INSTANCE GETS THIS IN ParamBridge'S CONSTRUCTOR, alongside the restoreModRack already
	// beside it; that line is feathers-1a's half of the swallowing. Stated here explicitly so this
	// case tests the routing rule rather than silently testing whether that line has landed.
	bridge.restoreFxChain(feathers::defaultFxChain());
	// THE FILTER'S CUTOFF ON THE FILTER, which is its only address now. The claim is unchanged — every
	// weight source may reach it, because a weight cannot feed itself through a filter control — and
	// only the way the destination is named has moved: it is the DEVICE and its own control index,
	// asked of the bridge that holds the chain rather than computed from a slot the chain sits in.
	const feathers::DeviceInstanceId filterId = bridge.fxDeviceAt(0);
	REQUIRE(filterId != feathers::kNoInstance);
	const backplane::Destination cutoff = backplane::Destination::control(
	    filterId, static_cast<size_t>(feathers::fx::filter::Param::Cutoff));

	for (size_t slot = 0; slot < feathers::kMaxSounds; ++slot) {
		const backplane::Source weight = backplane::Source::core(feathers::weightSourceOf(slot));
		CAPTURE(slot);
		// **THE AUTHORITY, NOT THE RETIRED BAND RULE (kill-lanes step 5).** This asked
		// `retiredBandRule::maySourceRouteTo`; the gate a surface and the loader ask is
		// `ParamBridge::mayRoute`, and it is the one that has to agree with the write below.
		CHECK(bridge.mayRoute(weight, cutoff));
		bridge.setRouteAmount(weight, cutoff, 0.4f);
		CHECK(bridge.routeAmount(weight, cutoff) == doctest::Approx(0.4f));
	}

	// AND THE PUCK IS STILL REFUSED THEM, which is what makes the line above a boundary rather than a
	// removal.
	const int path = feathers::modDestinationIndexOf(ParamId::Path);
	REQUIRE(path >= 0);
	CHECK_FALSE(bridge.mayRoute(backplane::Source::core(feathers::weightSourceOf(0)),
	                            backplane::Destination::core(static_cast<size_t>(path))));
}

TEST_CASE("a destination is stored by ParamId, so appending to the table cannot move it") {
	// THE SUCCESSOR TO "a patch from before the filter was a destination keeps its six columns".
	//
	// That case guarded the DENSE format's weakest point: a destination was stored as its INDEX in
	// kModDestinations, so appending the filter's controls — or the per-sound knobs after them — had
	// to be done in an order that did not shift what came before. Getting it wrong would have
	// re-pointed every saved routing silently, and the modulator-rack work says so in as many words.
	//
	// The sparse format stores a ParamId instead, which is forever. So the guard becomes: a routing
	// written against a parameter resolves to that parameter WHEREVER it now sits in the table, and
	// the case can prove it by checking the resolved index against the id rather than against a
	// remembered number.
	//
	// A CORE PARAMETER RATHER THAN THE FILTER'S, because the filter no longer has one. What this case
	// is about is the format — that a routing resolves through its ID wherever the table now sits it —
	// and any parameter destination proves that. The filter's own road is covered at its device
	// address by the device-routing cases.
	TestRoute cutoff = fromMacro(0);
	toParam(cutoff, feathers::ParamId::Weather);
	cutoff.amount = -0.6f;
	TestRoute path = fromMacro(1);
	toParam(path, feathers::ParamId::Path);
	path.amount = 0.3f;
	const Reloaded read = reload(throughBytes(patchWithRoutes({ cutoff, path })));

	// RESOLVED THROUGH THE ID, not through a literal column number. The filter's destinations sit
	// after the core six and before the per-sound knobs today; that is precisely the arrangement this
	// format stops depending on, so the case must not restate it either.
	const int cutoffIndex = feathers::modDestinationIndexOf(feathers::ParamId::Weather);
	const int pathIndex = feathers::modDestinationIndexOf(feathers::ParamId::Path);
	REQUIRE(cutoffIndex >= 0);
	REQUIRE(pathIndex >= 0);
	const backplane::Destination atWeather = backplane::Destination::core(static_cast<size_t>(cutoffIndex));
	const backplane::Destination atPath = backplane::Destination::core(static_cast<size_t>(pathIndex));
	CHECK(amountIn(read.routes, backplane::Source::core(0), atWeather) == doctest::Approx(-0.6f));
	CHECK(amountIn(read.routes, backplane::Source::core(1), atPath) == doctest::Approx(0.3f));
	CHECK(read.refused.count == 0);

	// AND THE WRITER SPELLS IT THE SAME WAY — by ParamId, not by the index it happens to have today.
	const feathers::rack::RackPatch written = patchOf(feathers::defaultFxChain(), rackForTest(),
	                                                  { { backplane::Source::core(0), atWeather, -0.6f } });
	REQUIRE(written.route.size() == 1);
	CHECK(written.route[0].destination.kind == 0u); // a parameter
	CHECK(written.route[0].destination.a == static_cast<uint32_t>(feathers::ParamId::Weather));
}


TEST_CASE("the chains block round-trips a route to a filter control, under its own tag") {
	// GROWING THE DESTINATION AXIS IS DATA, NOT SHAPE — checked in the direction that matters for this
	// build's own patches: a route to a filter control is saved and read back identical. THE FILTER'S
	// DESTINATIONS ARE DEVICE DESTINATIONS, so this exercises the device road rather than the parameter
	// road, and it is the stronger case for it: a device destination is stored by instance id and
	// CONTROL INDEX rather than by a column number.
	// BOTH ENDS BY IDENTITY — the filter that `defaultFxChain()` puts at the head, and the far corner of
	// the fixture rack. The control index IS the file's spelling, so naming it here is naming what the
	// block carries rather than converting to it.
	const feathers::DeviceInstanceId filterId =
	    feathers::instanceOfFxSlot(feathers::defaultFxChain(), 0);
	REQUIRE(filterId != feathers::kNoInstance);
	const backplane::Destination cutoff = backplane::Destination::control(
	    filterId, static_cast<size_t>(feathers::fx::filter::Param::Cutoff));
	const backplane::Destination macro2 = backplane::Destination::control(
	    filterId, static_cast<size_t>(feathers::fx::filter::Param::Macro2));
	REQUIRE(cutoff != macro2);
	// THE FAR CORNER, BY IDENTITY: the LAST rack device that publishes an output, and its last output.
	// Searched rather than computed — the corner has moved five times — and by the device's own descriptor
	// rather than by an address, since there is no address (kill-lanes §2.4b).
	backplane::Source farSource = backplane::Source::nobody();
	for (size_t slot = feathers::kMaxRackSlots; farSource == backplane::Source::nobody() && slot-- > 0;) {
		const feathers::DeviceRow& row = rackForTest().device[slot];
		const feathers::device::Descriptor* far = feathers::descriptorForRow(row);
		if (far != nullptr && far->outputCount() > 0) {
			farSource = backplane::Source::output(feathers::instanceOfSlot(rackForTest(), slot), far->outputCount() - 1);
		}
	}
	REQUIRE(farSource != backplane::Source::nobody());
	REQUIRE(farSource.isDevice());
	const TestRoutes routes = sortedRoutes({
	    { backplane::Source::core(0), cutoff, -0.6f },
	    { farSource, macro2, feathers::kMaxRouteAmount },
	});

	const feathers::rack::RackPatch patch = patchOf(feathers::defaultFxChain(), rackForTest(), routes);
	MemoryStream stream;
	clap_ostream out = stream.out();
	REQUIRE(feathers::saveChainsBlock(&out, patch));

	// THE TAG, stated as bytes rather than as "the constant", because a constant is not something a
	// reader of a file can check and four bytes are.
	REQUIRE(stream.bytes.size() > 4);
	CHECK(stream.bytes[0] == 'F');
	CHECK(stream.bytes[1] == 'S');
	CHECK(stream.bytes[2] == 'C');
	CHECK(stream.bytes[3] == '1');

	clap_istream in = stream.in();
	feathers::rack::RackPatch parsed;
	bool present = false;
	REQUIRE(feathers::loadChainsBlock(&in, &parsed, &present));
	CHECK(present);
	CHECK(parsed == patch);
	const Reloaded read = reload(parsed);
	CHECK(read.routes == routes);
	CHECK(amountIn(read.routes, backplane::Source::core(0), cutoff) == doctest::Approx(-0.6f));
	CHECK(read.refused.count == 0);
}

TEST_CASE("the last two blocks are written in the order they are read, and swapping them is caught") {
	// THE SILENT-PATCH-CORRUPTION CLASS, made mechanical. The variations block and the routings block
	// were written in parallel trees, both appended "last", and both landed on the same position in
	// stateSave. Nothing in a compiler notices that: the writer emits one order, the reader expects the
	// other, and every patch loads one block's bytes as the other's with no error anywhere — a routing
	// table read as a list of placements, or the reverse.
	//
	// So the pair is pinned here rather than trusted to whoever lands third. If a future block is
	// appended between these two, this case is the thing that says so. THE PAIR IS HUES AND CHAINS NOW:
	// the routings block that used to follow the variations folded into `chains` (chain-as-a-device
	// item 6), which is the last block.
	SoundHues hues;
	hues.index = { 3, 0, 7, 1, 6, 2, 5, 4 };

	const TestRoutes routes { { backplane::Source::core(2), backplane::Destination::core(1), 0.75f } };
	const feathers::rack::RackPatch patch = patchOf(feathers::defaultFxChain(), rackForTest(), routes);
	REQUIRE(patch.route.size() == 1);

	MemoryStream stream;
	clap_ostream out = stream.out();
	REQUIRE(saveSoundHues(&out, hues));
	REQUIRE(feathers::saveChainsBlock(&out, patch));

	SUBCASE("read in the order they were written, both arrive") {
		clap_istream in = stream.in();
		SoundHues readHues;
		feathers::rack::RackPatch readPatch;
		bool huesPresent = false;
		bool chainsPresent = false;
		REQUIRE(loadSoundHues(&in, &readHues, &huesPresent));
		REQUIRE(feathers::loadChainsBlock(&in, &readPatch, &chainsPresent));
		CHECK(huesPresent);
		CHECK(chainsPresent);
		CHECK(readHues == hues);
		CHECK(readPatch == patch);
		CHECK(reload(readPatch).routes == routes);
		CHECK(stream.readPos == stream.bytes.size());
	}

	SUBCASE("read in the other order, the tag refuses rather than the bytes being taken") {
		// This is what the tags are FOR, and why a length prefix would not have been enough: a reader
		// that arrives at the wrong block does not silently consume it.
		clap_istream in = stream.in();
		feathers::rack::RackPatch readPatch;
		bool chainsPresent = false;
		CHECK_FALSE(feathers::loadChainsBlock(&in, &readPatch, &chainsPresent));
		CHECK_FALSE(chainsPresent);
	}
}

TEST_CASE("the state blocks come in one pinned order, and it is append-only") {
	// THE SEQUENCE, PINNED — the half the tag-refusal cases cannot cover. Each of those constructs
	// its own stream and proves a reader at the WRONG block refuses; none of them would notice a
	// block INSERTED between two others, because the stream is positional and an insertion shifts
	// every later tag: existing patches would load one block's bytes as another's, silently. The
	// order lives in exactly one place (state_block_order.h — feathers_clap.cpp's table draws its
	// names from it positionally, so the two cannot drift apart in size), and this case is what goes
	// red when anyone moves it.
	//
	// APPEND ONLY. If this case is failing you, the fix is to append your block at the END, not to
	// edit these expectations — a mid-sequence change here is a stream-format migration wearing a
	// small diff's clothes, and it needs the design conversation that implies. (Two authors both
	// appending "last" is the collision the shared table already ended; this pins what the table
	// itself cannot: that the sequence a patch was saved under is the sequence forever.)
	REQUIRE(feathers::kStateBlockOrder.size() == 13);
	CHECK(std::string(feathers::kStateBlockOrder[0]) == "orientation");
	CHECK(std::string(feathers::kStateBlockOrder[1]) == "drift");
	CHECK(std::string(feathers::kStateBlockOrder[2]) == "planes");
	CHECK(std::string(feathers::kStateBlockOrder[3]) == "residue");
	CHECK(std::string(feathers::kStateBlockOrder[4]) == "gamma");
	CHECK(std::string(feathers::kStateBlockOrder[5]) == "variations");
	CHECK(std::string(feathers::kStateBlockOrder[6]) == "hues");
	// **`fxchain`, `modrack` AND `routings` BECAME ONE BLOCK, 2026-09-11** (chain-as-a-device item 6;
	// one Feathers encoding): both chains and every route in the rack's own format. This case going
	// red is what forced the argument rather than allowing the edit, and the three things that had to be
	// true — the bar the 2026-08-22 move of `routings` to last set — all were:
	//
	//   1. THE ORDER WAS WRONG, not merely awkward: two device-record formats for one idea, the lane's
	//      and the rack's, and three blocks whose correctness depended on being read in one order
	//      (routings resolved ids against chains read before it). One block cannot be read out of order
	//      against itself.
	//   2. THE SILENT-MISREAD HAZARD WAS PAID FOR. kStateVersion went 3 -> 4 in the same change, so a
	//      patch written under the old order refuses at the FIRST BYTE rather than reaching this table
	//      and reading `fxchain`'s bytes where it expected `chains`.
	//   3. THE FORMAT WAS ALLOWED TO BREAK: the project's standing policy, while there are no saved
	//      patches to protect, is to take the clean design and break the format.
	//
	// Absent any of the three, append. The bar is deliberately high and was deliberately met.
	CHECK(std::string(feathers::kStateBlockOrder[7]) == "chains");
	// **`macros` APPENDED 2026-09-15 (backlog EG)** — the structural macros' polarity. Appended at the
	// END, as this case demands, and the version byte did not move: a patch from before it ends before
	// the tag and reads as every macro unipolar, which is what every such patch was.
	CHECK(std::string(feathers::kStateBlockOrder[8]) == "macros");
	// **`spectral` APPENDED 2026-10-10 (additive design §3.1)**, at the END. The version byte moved (50 -> 51) because a 50
	// stream must carry every block; 50 and 49 are still read, and the block they predate is not asked of them
	// (`kStateBlockSince`), which the clap-render case "a v50 patch loads with the spectral drawer at its defaults" pins.
	CHECK(std::string(feathers::kStateBlockOrder[9]) == "spectral");
	CHECK(feathers::kStateBlockSince[9] == 51);
	// **`likes` APPENDED 2026-10-26 (backlog ON)**, at the END, on the spectral block's terms: 51 -> 52, older streams read.
	CHECK(std::string(feathers::kStateBlockOrder[10]) == "likes");
	CHECK(feathers::kStateBlockSince[10] == 52);
	// **`preset` APPENDED 2026-11-06 (backlog QC)**, at the END, on the same terms: 52 -> 53, older streams read with no name.
	CHECK(std::string(feathers::kStateBlockOrder[11]) == "preset");
	CHECK(feathers::kStateBlockSince[11] == 53);
	// **`drumlanes` APPENDED 2026-11-07 (drum proposal §17 P4)**, at the END after it, on the same terms: 53 -> 54, older streams
	// read with every kit pad's fx lane empty. (Built as 53 on drum-engine; renumbered when it merged after the preset.)
	CHECK(std::string(feathers::kStateBlockOrder[12]) == "drumlanes");
	CHECK(feathers::kStateBlockSince[12] == 54);
	// **4 -> 5 ON 2026-09-14, AND THE THREE-PART BAR ABOVE WAS MET RATHER THAN WAIVED.**
	//   1. THE FORMAT CHANGED: `device::kTagBytes` went four to eight, so every serialised device tag
	//      is four bytes longer. That is a format change, not a reordering of meanings.
	//   2. THE SILENT-MISREAD HAZARD IS PAID FOR BY THE BUMP: without it a v4 patch would hand a
	//      four-byte tag to an eight-byte read and carry on with whatever followed it, which is the
	//      exact failure this case exists to prevent. With it the patch refuses at the first byte.
	//   3. A LONGER IDENTIFIER WAS ACCEPTABLE (2026-09-14), and the project's standing policy is to take
	//      the clean design and break the format.
	// The block ORDER is untouched, which is what the assertion above still guards.
	//
	// **5 -> 6 ON 2026-09-15, and this one is the plainest case the bar has seen.**
	//   1. THE FORMAT CHANGED: SKLITTER's `Macro Cfg Phase` was DELETED, so Gamma, Pitch Scale and
	//      Stereo Width each moved down one column in that device's row.
	//   2. THE SILENT-MISREAD HAZARD IS EXACTLY WHAT A BUMP PREVENTS: without it a v5 row would read
	//      Gamma out of the deleted phase's column and every later control one place out — a patch
	//      that loads, plays, and is wrong. With it the patch refuses.
	//   3. THE CHANGE WAS INTENDED: Macro Cfg Phase was the push-and-twist offset that belonged folded
	//      into Macro Config's multi-speed value box — and the project's standing policy is to take the
	//      clean design and break the format.
	//   7 (2026-09-15): AUTOMOD's Rate changed MEANING in place — 0..2 as a percentage became ±2 octaves
	//      as a multiple (backlog DT). Same column, different number: a saved 1.0 meant 1X and would
	//      read as 2X. That is the "stored number changes meaning" half of the rule, exactly.
	//   8 (2026-09-15): the MSEG's Amount (backlog DH) is INSERTED before its segment block, as that
	//      device's own note requires, so every segment column moved down one.
	//   9 (2026-09-16): SKLITTER gained TWO controls at the end of its row — `Seq Length` and
	//      `Buffer Length` (a sequence of 4 bars and lower, and a buffer ladder of
	//      1/4 1/2 1 2 4 bars). APPENDED rather than inserted, so no existing column moved; the bump is
	//      needed anyway because the registry's stored-meaning fingerprint covers the SHAPE of every
	//      row, and a v8 patch has two fewer numbers in SKLITTER's than a v9 reader expects.
	//
	//      **BOTH DEFAULT TO THE BEHAVIOUR THAT SHIPPED** — 4 bars and 1 bar — which is the
	//      correctness half of the rule rather than a compatibility concession: four was the hard-wired
	//      period `planFor` already had, and one bar was the only length the looper could capture. A
	//      device that loses these fields sounds as it did.
	// **9 -> 10 ON 2026-09-16 (backlog EL):** OWLTT gained a `Soft Clip` switch, APPENDED to its row so
	// no other column moves, defaulting to On — the behaviour that shipped. The row's shape changed,
	// which the stored-meaning fingerprint sees; a v9 OWLTT row is one number short of a v10 reader's
	// expectation, and the version byte refuses it rather than reading the next device's first column
	// as the switch.
	// **10 -> 11 ON 2026-09-16:** SKLITTER gained `Min Attack` and `Mix`, APPENDED to its row (a minimum
	// attack knob and a basic mix knob). No existing column moves; the bump is for the
	// fingerprint, which covers the SHAPE of every row, and a v10 SKLITTER row is two numbers short of
	// what a v11 reader expects.
	//
	// **BOTH DEFAULTS ARE THE BEHAVIOUR THAT SHIPPED, which is correctness rather than a compatibility
	// concession.** Min Attack defaults to the BOTTOM of its `LogSeconds` range, which that unit
	// declares to mean "no time at all" — so it is off by default. Mix
	// defaults to 1.0, fully wet: a stutter that arrives half-mixed is a different effect, and the dry
	// roads a player actually wants are Density and the plan's dry votes, which thin the grain stream
	// deliberately rather than veiling it.
	//
	// **AND 11 STAYED 11 WHEN `Min Attack` BECAME `Min Env`** (hours later, so that it affects both
	// attack and release). The stored-meaning
	// FINGERPRINT moved — it covers the declared name and the relevance gate, and both changed — but
	// **the stored NUMBER did not**: same column, same range, same unit, same default. A patch written
	// by the 342dbdd1 build reads correctly against this one, so bumping the version would refuse
	// patches for a change that cannot misread one.
	//
	// **THAT IS THE DISTINCTION THIS CASE EXISTS FOR, and its own failure message names the path:** *"If
	// nothing about the MEANING moved, only this constant needs updating."* A rename is not a meaning.
	//
	// **12 (backlog FH): the MSEG's Depth was DELETED**, moving every later column in its row — a meaning moved, so the
	// version did.
	//
	// **13: THE DAHDSR's AND THE MSEG's Loop / SingleTrigger / Trigger BECAME ONE `Mode` DROPDOWN.** Three columns
	// became one and everything after them moved up two, which is a stored NUMBER changing its meaning — the case this
	// constant exists for, not a rename.
	//
	// **AND THIS CASE COULD NOT HAVE CAUGHT IT, WHICH IS WORTH KNOWING ABOUT THE GUARD.** It pins the version against a
	// number a human wrote down, so it fires on a bump nobody explained — and says nothing at all about a bump nobody
	// MADE. The collapse arrived with `kStoredMeaningFingerprint` updated and a note beside it reading "MOVED WITH
	// kStateVersion 13", while the constant still read 12: the guard described in the past tense and never armed, and
	// this case green throughout. The asymmetry is not fixable here — whether a moved fingerprint deserves a version
	// byte is a judgement, and `state_block_order.h` carries a real example of one that does not — so what closes it is
	// the two constants being read together, which is why the note above each of them now says so.
	//
	// **14: the LFO's `Gamma` was DELETED**, so every column after it in that row moved up one.
	//
	// **15 (2026-09-19): SKLITTER LOST SIX CONTROLS FROM THE MIDDLE OF ITS ROW.** `Macro Cfg` and `Macro`
	// were columns 2 and 3, and `Macro N/E/S/W` were 19..22; the twenty-five columns after the first pair
	// moved up two and the rest moved up six more. A v14 SKLITTER row read by a v15 reader takes its
	// Macro Cfg into P Write and goes on mis-reading to the end of the row, which is the silent-misread
	// hazard this byte exists for. Both halves were intended: the centre macro goes and becomes zero,
	// and the waypoints need no macro knobs because only the pad controls them. The
	// circle's centre is a constant zero push and a waypoint is a configuration whose push is built at
	// macro 1.0, with the puck's RADIUS supplying the amount.
	//
	// **16 (2026-09-19): THE MERGE OF THE TWO 15s.** The entry above and the modulators' `Smoothing On`
	// deletion were written on separate branches the same day, each correctly taking the byte to 15
	// against a registry without the other. Two different formats both called themselves 15 and the
	// union is a third, so the merge took it to 16 — see `state_block_order.h`, which carries the
	// argument. **That is this case earning its keep in a way its own comment above did not predict:**
	// the literal here is what makes a contended bump visible at all, because both branches were
	// individually right and only the number they shared was wrong.
	// **17 (2026-09-19): AND THIS CASE IS THE ONLY THING THAT ASKED.** The LFO's skews were rescaled
	// from 0..100 to 0..1 — a stored 50.0 went from neutral to past the top — and the fingerprint
	// guard stayed GREEN throughout, because it digests names and Choice tables and deliberately not
	// ranges. A literal pinned by hand is what makes a bump a decision someone took rather than a
	// number that drifted; here it is also the only surface on which the decision appears at all.
	//
	// **18 (2026-09-19): SKLITTER GAINED `Send Override` AS AN INSERT.** The delay send's always/never
	// knob (backlog EW) went in at column 17, directly after `Mix`, so the seven perform columns after
	// it each moved down one and a v16 row read by a v17 reader takes its `Cfg N` out of the override's
	// column. Column 24 was free and an append would have cost no byte at all — which is exactly the
	// trade the 2026-08-22 format policy refuses: with no saved rows to protect, the only thing an
	// append buys is a knob living on the far side of the perform circle because the FORMAT put it
	// there. `state_block_order.h` carries the argument.
	//
	// **AND 18 RATHER THAN 17 BECAUSE BOTH OF THE ABOVE CLAIMED 17.** Two branches bumped 16 → 17 the
	// same day for unrelated format changes, and `git` merged the CONSTANT silently because both sides
	// wrote the same digit — only the prose conflicted. This hand-pinned literal is the surface that
	// would have caught it had the comments agreed too, which is the argument for pinning it by hand.
	//
	// **AND 19 SINCE SKLITTER's `Transport Locked` (2026-09-21, backlog GT)**, inserted at column 15 so
	// that everything from `Min Env` up shifted one — the insert case again, and the byte again.
	//
	// **AND 20 SINCE SKLITTER's FEEDBACK CIRCUIT (2026-09-21, backlog GU/GV)** — `Dry Write`,
	// `Feedback` and `FB Tap` inserted at columns 19..21, directly after `Send Override`, so the four
	// waypoints and the perform trio each moved down THREE. The insert case a third time.
	//
	// **AND 21 SINCE THE DELAY'S `Mode` GAINED `Xfade` (2026-09-21)** — a new Xfade mode beside
	// Granular and Clean, with the pitched behaviour staying the default. **The first entry on this list
	// that is NOT an insert**, and the first where the bump is a judgement rather than a consequence:
	// the third choice is APPENDED, so `Clean` is still 0 and `Granular` still 1 and no stored number
	// changes meaning. `state_block_order.h`'s rule lists "a `Choice` table that grows" because a
	// table usually grows IN PLACE — as `kDivisionBeats` did, nine entries to twenty-seven, moving
	// every saved MSEG shape onto a different division. This one does not.
	//
	// **Bumped anyway, and the reason is worth keeping: the rule as written says grows, there is
	// nothing saved to protect, and a byte costs nothing** — so of the two ways to be wrong, this is
	// the one that cannot hurt. Recorded here rather than argued silently, because the NEXT appended
	// Choice table will want the same answer and the digest cannot give it.
	// **IF YOU ARE HERE BECAUSE THIS WENT RED, THE OTHER SITE IS `src/plugin/state_block_order.h`** —
	// and that is the whole hazard of a hand-pinned literal: it is nowhere near the constant it pins,
	// nothing points from one to the other, and a digest cannot point at either. Moving the byte means
	// editing TWO places that do not know about each other. Named here on feathers-96's report, whose
	// own bump an hour earlier missed this only because their agent happened to update it.
	//
	// **AND 22 SINCE THE DELAY GAINED `Tape`** — a fourth `Mode` and four appended controls. The
	// first bump made under the forward-direction rule rather than reasoned past the old one: no
	// stored number changes meaning, but a row can now hold `Mode = 3`, which a build without Tape
	// cannot represent.
	//
	// **AND 23 SINCE THE DELAY GAINED `Multi-Tap`** — a fifth `Mode` and four appended controls, under
	// the same forward-direction rule as 22: a row can now hold `Mode = 4`.
	//
	// **AND 24 SINCE SKLITTER GAINED `Grain Order`** (backlog HU) — an AUDITION control inserted at
	// column 22, so the four waypoints and the perform trio each moved down one. **It is expected to
	// be DELETED once one of the two blend orders is chosen by ear, which will owe another byte**; see
	// `state_block_order.h` for why both edits are correct rather than one repairing the other.
	// **AND 28 SINCE SKLITTER GAINED `Groove`** (backlog IG), inserted at column 23 before the perform
	// block — after the shaper's five took 27. **AND 31 SINCE IT SPLIT into `Groove Mode` and `Groove Amount`
	// (backlog IO)**: column 23 changes meaning and the perform block moves down one.
	// **AND 32 SINCE `Free Phase`** (backlog IO), inserted at column 25. **AND 35 SINCE ITS MEANING CHANGED**
	// (backlog IO addendum 2): phase 0 is straight, so a stored phase names a different pattern.
	// **AND 38 SINCE FEATHERVERB LOST `Dry Minus` AND GAINED `Character`** — its last column changed meaning.
	// **AND 39 SINCE FEATHERVERB'S SIZE MODE PUT OWL AFTER VAST** — a stored 6 now means Vast.
	// **AND 40 SINCE FEATHERVERB APPENDED `Conv Mode` AND `Conv Blend`** (backlog JD).
	// **AND 42/43 FOR FEATHERVERB'S EXCITER AND AURORA, AND 44 SINCE THE FILTER DROPPED ITS FOUR GPL-DERIVED
	// TYPES** — Comb moved from 12 to 8.
	// **AND 45 SINCE FEATHERVERB GAINED `Oversample`** (backlog JE) — its Conv columns moved down one.
	// **AND 46 SINCE FEATHERVERB GAINED `Owl Comp`** (an audition switch) — its Conv columns moved down one more.
	// **AND 47 SINCE THE MACROS BLOCK GAINED EACH MACRO'S TYPE AND NAME** (`FSMB`, after the polarity floats;
	// built as 41 on macro-mod).
	// **AND 48 SINCE FEATHERVERB DROPPED `Owl Comp`** (always on in Owl) — the Conv columns moved back up.
	// **AND 49 SINCE THE CHAINS BLOCK BECAME A KEYED JSON DOCUMENT** (backlog JF step 1c) — the block's shape,
	// and the last number a device edit moves: from JF step 1d on, the schema ledger guards those instead.
	// **AND 50 SINCE THE STREAM ENDS IN A TRAILER** (`state/state_trailer.h`) — a shape change, the kind this byte
	// still moves for; 49 is still read (`kStateVersionUntrailered`).
	// **AND 51 SINCE THE `spectral` BLOCK WAS APPENDED** (additive design §3.1): a block every stream must now carry; 50 and
	// 49 are still read, and the block they predate is not asked of them (`kStateBlockSince`).
	// **AND 52 SINCE THE `likes` BLOCK WAS APPENDED** (backlog ON), on the same terms.
	// **AND 53 SINCE THE `preset` BLOCK WAS APPENDED** (backlog QC: which preset the patch came from), on the same terms.
	// **AND 54 SINCE THE `drumlanes` BLOCK WAS APPENDED after it** (drum proposal §17 P4), on the same terms.
	CHECK(static_cast<int>(feathers::kStateVersion) == 54);
}

TEST_CASE("backlog DN: the master meter's readings cross the bridge as published — at rest they read silence, no peak and no clip; a non-finite reading is dropped rather than crossed") {
	feathers::ParamBridge bridge;
	const feathers::MasterMeterReadings rest = bridge.masterMeter();
	CHECK(rest.momentaryLufs == feathers::fx::busutil::kSilenceLufs);
	CHECK(rest.rawPeak == 0.0f);
	CHECK(rest.clipAge == feathers::fx::busutil::kClipAgeMax);

	bridge.publishMasterMeter({ -12.5f, 0.7f, 0.25f });
	const feathers::MasterMeterReadings got = bridge.masterMeter();
	CHECK(got.momentaryLufs == -12.5f);
	CHECK(got.rawPeak == 0.7f);
	CHECK(got.clipAge == 0.25f);

	bridge.publishMasterMeter({ std::nanf(""), std::nanf(""), std::nanf("") });
	const feathers::MasterMeterReadings kept = bridge.masterMeter();
	CHECK(kept.momentaryLufs == -12.5f);
	CHECK(kept.rawPeak == 0.7f);
	CHECK(kept.clipAge == 0.25f);
}

TEST_CASE("the hues block round-trips, and a patch from before it wears the identity") {
	// COLOUR IS A NAME, AND A NAME HAS TO SURVIVE THE FILE — "reordering retains the original colour"
	// is a claim about the next session as much as this one. A reorder that was not
	// saved would put the colours back the moment the patch reopened, which is the same bug arriving
	// later.
	SoundHues hues;
	hues.index = { 3, 0, 7, 1, 6, 2, 5, 4 }; // a real permutation, not a shifted identity

	MemoryStream stream;
	clap_ostream out = stream.out();
	REQUIRE(saveSoundHues(&out, hues));

	clap_istream in = stream.in();
	SoundHues read;
	bool present = false;
	REQUIRE(loadSoundHues(&in, &read, &present));
	CHECK(present);
	CHECK(read == hues);

	// A STREAM THAT ENDS BEFORE THE TAG is a patch written before colours could travel. Absent, not
	// broken — and the IDENTITY it keeps is exactly what such a patch drew, because a colour used to
	// be its slot's index. That is what makes this block safe to append: every existing patch opens
	// looking the way it looked when it was saved.
	MemoryStream empty;
	clap_istream endIn = empty.in();
	SoundHues fromOld;
	bool oldPresent = true;
	CHECK(loadSoundHues(&endIn, &fromOld, &oldPresent));
	CHECK_FALSE(oldPresent);
	CHECK(fromOld == SoundHues {});
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		CAPTURE(slot);
		CHECK(fromOld.index[slot] == slot);
	}
}

TEST_CASE("a colour index off the end of the palette cannot reach a draw site") {
	// A PATCH IS A FILE ON SOMEBODY'S DISK. An index past the eighth colour would run off a palette
	// of eight, which is the one way a name becomes a crash — so it is folded on the way in, and the
	// reader is asked with a value no honest writer of ours would produce.
	SoundHues wild;
	wild.index = { 200, 8, 15, 0, 1, 2, 3, 4 };

	MemoryStream stream;
	clap_ostream out = stream.out();
	REQUIRE(saveSoundHues(&out, wild));

	clap_istream in = stream.in();
	SoundHues read;
	bool present = false;
	REQUIRE(loadSoundHues(&in, &read, &present));
	CHECK(present);
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		CAPTURE(slot);
		CHECK(read.index[slot] < kMaxSounds);
	}
	// And the accessor every draw site goes through folds too, so a value that arrived some other way
	// — a bridge setter called with a stale index — still cannot index off the end.
	SoundHues unchecked;
	unchecked.index[0] = 250;
	CHECK(hueIndexOf(unchecked, 0) < kMaxSounds);
	CHECK(hueIndexOf(unchecked, kMaxSounds + 5) < kMaxSounds);
}

TEST_CASE("the bridge's hue channel marks the patch dirty, and a load does not") {
	// The variations channel's rule, and for the same reason: there is no parameter under a colour, so
	// the bracket is the only thing that tells the host a patch changed. But a LOAD is not an edit —
	// a patch that reported itself modified the instant it opened would ask to be saved back.
	ParamBridge bridge;
	bridge.takeStateDirty();

	bridge.beginSoundHuesGesture();
	bridge.setSoundHue(2, 5);
	bridge.endSoundHuesGesture();
	CHECK(bridge.soundHues().index[2] == 5);
	CHECK(bridge.takeStateDirty());

	SoundHues loaded;
	loaded.index = { 7, 6, 5, 4, 3, 2, 1, 0 };
	bridge.takeStateDirty();
	bridge.restoreSoundHues(loaded);
	CHECK(bridge.soundHues() == loaded);
	CHECK_FALSE(bridge.takeStateDirty());
}

TEST_CASE("the chains block round-trips the lane, and a patch from before it can still reach the filter") {
	// WHAT IS IN THE LANE is a fact the parameters cannot carry: which devices are there at all, and
	// what each is set to. Through the real bytes at three chunk sizes, because a host may hand the
	// stream over in pieces.
	FxChain chain = defaultFxChain();
	REQUIRE(chain.count == 1);
	CHECK(fxChainHas(chain, feathers::fx::filter::kTraits.tag));

	for (size_t chunk : {SIZE_MAX, size_t{3}, size_t{1}}) {
		CAPTURE(chunk);
		const Reloaded read = reload(throughBytes(patchOf(chain, defaultModRack()), chunk));
		CHECK(sameDevices(read.chain, chain));
		CHECK(sameDevices(read.rack, defaultModRack()));
	}

	// A STREAM THAT ENDS BEFORE THE TAG is a patch saved before the block existed. Absent, not broken
	// — and the load gives it the lane a fresh instance has rather than an empty one. The two are
	// indistinguishable for such a patch, which has its filter switched off; they come apart only in
	// that an empty lane, in a build with no way to add a device, would be a patch that could never have
	// a filter again.
	MemoryStream empty;
	clap_istream endIn = empty.in();
	feathers::rack::RackPatch fromOld;
	bool oldPresent = true;
	CHECK(feathers::loadChainsBlock(&endIn, &fromOld, &oldPresent));
	CHECK_FALSE(oldPresent);
	const Reloaded old = reload(fromOld);
	CHECK(fxChainHas(old.chain, feathers::fx::filter::kTraits.tag));
	CHECK(old.chain.count == 1);
	// AND THE LOAD REALLY PUT IT BACK, rather than leaving a fresh bridge's lane in place: a bridge
	// holding something else, handed the same absent block, ends with the fresh instance's lane.
	ParamBridge used;
	holdChains(used, FxChain {}, ModRack {});
	REQUIRE(used.fxChain().count == 0);
	(void)used.restoreChains(fromOld);
	CHECK(sameDevices(used.fxChain(), defaultFxChain()));
	CHECK(sameDevices(used.modRack(), defaultModRack()));
}

TEST_CASE("a device this build has never heard of is kept, with its settings, and saved back out") {
	// A PATCH SAVED BY A LATER BUILD NAMES DEVICES THIS ONE DOES NOT HAVE. A device is a slot in the
	// chain whether or not this build can render it, and dropping the ones it cannot would silently
	// rewrite somebody's chain the next time they saved.
	//
	// **AND NOW ITS SETTINGS SURVIVE TOO, IN BOTH CHAINS.** The old lane kept an unknown device's slot and
	// dropped its payload, so it came back PRESENT AND AT ITS DEFAULTS — "the plugin reset my
	// compressor" — and the old strip dropped the device outright. The one format keeps every field an
	// unknown device came with (`applyPatchToRecord`: only a device this build KNOWS is bounded by what
	// it declares), in either chain.
	//
	// A PATCH VALUE rather than a hand-written stream: what a later build wrote, as its values. The
	// parse of a device record this build cannot run is `rack_state`'s own case.
	const auto wob = feathers::rack::patchTagOf("wob1");
	const auto dly = feathers::rack::patchTagOf("dly2");
	const auto zap = feathers::rack::patchTagOf("zap3");
	feathers::rack::RackPatch later = patchOf(defaultFxChain(), defaultModRack());
	// Values BY KEY (backlog JF) — keys this build has never heard of, on devices it has never heard of.
	using Value = feathers::rack::RackPatch::Value;
	const auto unknown = [](uint32_t chain, std::array<char, feathers::device::kTagBytes + 1> tag, uint32_t id,
	                        std::vector<Value> values) {
		feathers::rack::RackPatch::Device d;
		d.chain = chain;
		d.tag = tag;
		d.instanceId = id;
		d.values = std::move(values);
		return d;
	};
	const std::vector<Value> wobValues { Value::of("depth", 0.25f), Value::of("rate", 0.5f), Value::label("shape", "Saw") };
	const std::vector<Value> zapValues { Value::of("level", 9.0f) };
	later.device.push_back(unknown(1, wob, 70, wobValues)); // the fx chain: settings aboard
	later.device.push_back(unknown(1, dly, 71, {}));         // and nothing in it
	later.device.push_back(unknown(0, zap, 72, zapValues));  // and one in the strip

	ParamBridge bridge;
	CHECK(bridge.restoreChains(later).routes.count == 0);
	const FxChain read = bridge.fxChain();
	const ModRack rack = bridge.modRack();
	// ALL KEPT, in order: the filter first, then the two this build has never heard of.
	REQUIRE(read.count == 3);
	CHECK(fxChainHas(read, feathers::fx::filter::kTraits.tag));
	CHECK(fxChainHas(read, "wob1"));
	CHECK(fxChainHas(read, "dly2"));
	CHECK_FALSE(fxChainHas(read, "none"));
	CHECK(std::string_view(fxDeviceAtPosition(read, 1).tag().data(), 4) == "wob1");
	REQUIRE(rack.count == 2);
	CHECK(std::string_view(rack.device[1].tag.data(), 4) == "zap3");

	// THE ROW HOLDS NOTHING — a device this build cannot run has no controls here, so its values cannot be
	// a row (backlog JF: a row is floats by index, a patch is values by key). They wait beside its body.
	CHECK(fxDeviceAtPosition(read, 1).row.fieldsPresent == 0);

	// AND SAVED BACK OUT AS IT ARRIVED, EVERY KEY — the next save is where the old lane lost them for good.
	const feathers::rack::RackPatch resaved = throughBytes(bridge.chainsPatch());
	size_t found = 0;
	for (const auto& d : resaved.device) {
		if (d.tag == wob) {
			CHECK(d.values == wobValues);
			++found;
		}
		if (d.tag == zap) {
			CHECK(d.chain == 0u);
			CHECK(d.values == zapValues);
			++found;
		}
		if (d.tag == dly) {
			CHECK(d.values.empty());
			++found;
		}
	}
	CHECK(found == 3);
}

TEST_CASE("a chain block that is not a chain block is refused rather than interpreted") {
	// The same rule every tagged block here follows: a reader that finds something else at its
	// position has been handed a stream it does not understand, and says so rather than reading the
	// next bytes as a version and a count.
	{
		MemoryStream stream;
		clap_ostream out = stream.out();
		REQUIRE(saveSoundHues(&out, SoundHues {}));
		clap_istream in = stream.in();
		feathers::rack::RackPatch read;
		bool present = false;
		CHECK_FALSE(feathers::loadChainsBlock(&in, &read, &present));
		CHECK_FALSE(present);
	}
	// AND A TAG WITH NOTHING BEHIND IT IS A TRUNCATED BLOCK, NOT AN ABSENT ONE. Absent is a clean end
	// BEFORE the tag; a stream that stops after it promised a block and did not deliver one.
	{
		MemoryStream stream;
		const uint8_t tag[4] = { 'F', 'S', 'C', '1' };
		stream.bytes.assign(tag, tag + 4);
		clap_istream in = stream.in();
		feathers::rack::RackPatch read;
		bool present = false;
		CHECK_FALSE(feathers::loadChainsBlock(&in, &read, &present));
		CHECK_FALSE(present);
	}
	// THE CONTROL: the same framing with a real block behind it loads — so both refusals above are
	// about what was changed, not about the shape around it.
	{
		MemoryStream stream;
		clap_ostream out = stream.out();
		REQUIRE(feathers::saveChainsBlock(&out, patchOf(defaultFxChain(), defaultModRack())));
		clap_istream in = stream.in();
		feathers::rack::RackPatch read;
		bool present = false;
		CHECK(feathers::loadChainsBlock(&in, &read, &present));
		CHECK(present);
	}
}

// "a chain record with impossible numbers in it cannot walk off the end" STOOD HERE, and went with the
// lane's device record (chain-as-a-device item 6). Its property — a count or a payload length read out
// of a file is bounded BEFORE it becomes a size — is the one format's now, and is proved where that
// parser lives: `test_rack_state.cpp`'s "a nonsense payload length is refused BEFORE it is turned into
// an allocation" and "a truncated stream refuses". (Its own lesson stands for whoever writes the next
// hand-built stream: MEASURED 2026-08-28, a hand-written record missing one field passed every row on
// an EOF four bytes early. A hand-written stream is a second copy of the format.)

TEST_CASE("a weight source cannot be pointed at the puck") {
	// A DESIGN DECISION, and the comment on maySourceRouteTo says why it is a decision rather than a
	// necessity: a weight is a function of where the puck stands, so a weight routed to Path closes a
	// loop. The one-block-delay analysis says that loop is stable — the published reading is a block
	// old, so it feeds back at block rate rather than recursing — and it is foreclosed
	// anyway. A future reader wondering whether this can be lifted should know the answer is taste.
	feathers::ParamBridge bridge;

	const int path = feathers::modDestinationIndexOf(ParamId::Path);
	const int posX = feathers::modDestinationIndexOf(ParamId::PosX);
	const int posY = feathers::modDestinationIndexOf(ParamId::PosY);
	const int level = feathers::modDestinationIndexOf(ParamId::VoiceLevel);
	REQUIRE(path >= 0);
	REQUIRE(posX >= 0);
	REQUIRE(posY >= 0);
	REQUIRE(level >= 0);

	const backplane::Source weight = backplane::Source::core(feathers::weightSourceOf(2));
	const backplane::Destination atPath = backplane::Destination::core(static_cast<size_t>(path));
	const backplane::Destination atPosX = backplane::Destination::core(static_cast<size_t>(posX));
	const backplane::Destination atPosY = backplane::Destination::core(static_cast<size_t>(posY));
	const backplane::Destination atLevel = backplane::Destination::core(static_cast<size_t>(level));

	SUBCASE("refused at the gate rather than zeroed afterwards") {
		bridge.setRouteAmount(weight, atPath, 0.8f);
		bridge.setRouteAmount(weight, atPosX, 0.8f);
		bridge.setRouteAmount(weight, atPosY, 0.8f);
		CHECK(bridge.routeAmount(weight, atPath) == 0.0f);
		CHECK(bridge.routeAmount(weight, atPosX) == 0.0f);
		CHECK(bridge.routeAmount(weight, atPosY) == 0.0f);
		// Nothing was written, so nothing changed — a refused gesture must not mark the project dirty
		// either, or a patch would be asked for again because of a route that was never made.
		CHECK_FALSE(bridge.takeStateDirty());
	}

	SUBCASE("and the rest of the destinations are untouched by the refusal") {
		// THE CONTROL. A refusal that took the whole source with it would pass the case above and be
		// wrong: level, glide and fine remain legal, and they are what these sources are FOR.
		bridge.setRouteAmount(weight, atLevel, 0.8f);
		CHECK(bridge.routeAmount(weight, atLevel) == doctest::Approx(0.8f));
	}

	SUBCASE("and it is the SOURCE CLASS that is refused, not the destination") {
		// THE OTHER CONTROL, and the one that stops this becoming a blanket over the puck: a macro
		// reaches Path exactly as it always has, which is most of what the modulation panel is for.
		bridge.setRouteAmount(backplane::Source::core(0), atPath, 0.8f);
		CHECK(bridge.routeAmount(backplane::Source::core(0), atPath) == doctest::Approx(0.8f));
	}

	// **"the predicate agrees with the gate, for every pair in the table" STOOD HERE AND IS DELETED,
	// because it had become true by construction (kill-lanes step 5).**
	//
	// It swept every coordinate pair and compared whether a write LANDED against a second
	// implementation of the rule — first `feathers::maySourceRouteTo`, latterly
	// `retiredBandRule::maySourceRouteTo` — and its whole value was that the two were separate bodies of
	// code that could disagree. They are not: `ParamBridge::setRouteAmount` refuses exactly the pairs
	// `ParamBridge::mayRoute` refuses, both through `ChainRecord::mayRoute`, so the ported sweep would
	// have asked one function whether it agrees with itself. `device_registry.h` names the day that
	// became so; the retired rule the sweep was left comparing against is the BAND rule, which the
	// product stopped having at kill-lanes step 4b — a green sweep asserting it would have been worse
	// than none, because it reads as a guarantee about the rule the build actually has.
	//
	// WHAT IT WAS PROTECTING IS NOT LOST: that the glow and the drag give one answer is now a property
	// of there being one function, which the compiler holds. The three subcases above keep the part that
	// is a DECISION rather than a tautology — which pairs are foreclosed, and that the foreclosure is
	// about the source class rather than the destination.
}

TEST_CASE("the bridge's gate refuses a route whose SOURCE names a device nothing holds — and the destination half too") {
	// BACKLOG DZ. The RULE was covered (`test_backplane_routing`: `Source::output(99, 0)` through
	// `backplane::mayRoute` with a stub `DeviceView` that answers null because the case told it to) and the
	// LOADER was covered (a patch naming id 9999 is dropped and named in `refused.firstSource`). What was
	// not asked anywhere is the PRODUCT'S ASSEMBLED GATE — `ParamBridge::mayRoute` and the `setRouteAmount`
	// behind it, with the real record supplying the view — for a source the record never held. The record
	// answering null for an unheld id is the record's own behaviour, not the stub's, and that is the fact
	// this pins. Both halves, because the destination half's only bridge-level pin sat in a widget case.
	feathers::ParamBridge bridge;
	constexpr auto kUnheld = static_cast<feathers::DeviceInstanceId>(9999);
	REQUIRE(bridge.descriptorOf(kUnheld) == nullptr); // the premise, asked of the record
	const feathers::DeviceInstanceId envelope = bridge.rackDeviceAt(0);
	REQUIRE(envelope != feathers::kNoInstance);
	const size_t attack = static_cast<size_t>(feathers::mod::dahdsr::Param::Attack);
	REQUIRE(feathers::mod::dahdsr::kDescriptor.params[attack].modulatable);
	const TestRoutes before = routesOn(bridge);

	SUBCASE("a source nothing holds reaches nothing — not a voice parameter, not a held device's control") {
		const auto from = backplane::Source::output(kUnheld, 0);
		CHECK_FALSE(bridge.mayRoute(from, backplane::Destination::core(0)));
		CHECK_FALSE(bridge.mayRoute(from, backplane::Destination::control(envelope, attack)));
		bridge.setRouteAmount(from, backplane::Destination::core(0), 0.5f);
		bridge.setRouteAmount(from, backplane::Destination::control(envelope, attack), 0.5f);
		CHECK(routesOn(bridge) == before);
		CHECK(bridge.routeAmount(from, backplane::Destination::core(0)) == 0.0f);
	}
	SUBCASE("and a destination nothing holds is reached by nothing") {
		const auto into = backplane::Destination::control(kUnheld, 0);
		CHECK_FALSE(bridge.mayRoute(backplane::Source::core(0), into));
		CHECK_FALSE(bridge.mayRoute(backplane::Source::output(envelope, 0), into));
		bridge.setRouteAmount(backplane::Source::core(0), into, 0.5f);
		CHECK(routesOn(bridge) == before);
	}
	SUBCASE("NOT VACUOUS: the same pair with a held source is admitted") {
		CHECK(bridge.mayRoute(backplane::Source::core(0), backplane::Destination::control(envelope, attack)));
	}
}

TEST_CASE("a patch cannot carry in a route the editor would refuse to make") {
	// A FILE IS EDITABLE BY HAND and outlives the build that wrote it, so "the editor will not create
	// one" is a different claim from "one cannot exist". The loader asks the same authority, which is
	// what makes them the same claim.
	//
	// EVERY NAMEABLE PAIR, each at its OWN amount rather than all at one value. Spelled by the fixture's
	// own enumerations and `routeOf`, which restate the saver's translation rather than calling it — the
	// file this case is about is one the saver would never write.
	//
	// DISTINCT AMOUNTS BECAUSE A CONSTANT CANNOT SEE A TRANSPOSITION. With every cell at 0.5 the
	// loader could land the whole table one row over and this case would not notice; generatedAmount
	// makes each cell's value unique, so a misplacement shows up as a wrong VALUE rather than only as
	// a wrong count. Its steps are derived from the table's own extent, so growth cannot push the
	// sweep past kMaxRouteAmount and turn a placement check into a clamp check.
	//
	// ONE PATCH PER SOURCE, NOT ONE FOR THE WHOLE TABLE — each patch asks this case's question and nothing else.
	// (A source's row may pass the engine's starting room — the bridge's list grows, backlog OJ — and the table at once
	// is several times that.)
	const feathers::rack::RackPatch base = patchWithRoutes({});
	REQUIRE(base.route.empty());
	const std::vector<TestRoute> sources = everySourceNamed();
	const std::vector<TestRoute> destinations = everyDestinationNamed();
	REQUIRE(sources.size() > feathers::kMacroCount + feathers::kWeightSourceCount); // device outputs are on offer
	REQUIRE(destinations.size() > feathers::kParamDestinationCount);                // and device controls
	bool anyLanded = false;
	for (size_t si = 0; si < sources.size(); ++si) {
		const auto pairAt = [&](size_t di) {
			TestRoute r = sources[si];
			r.destKind = destinations[di].destKind;
			r.destA = destinations[di].destA;
			r.destB = destinations[di].destB;
			r.amount = generatedAmount(si, sources.size(), di, destinations.size());
			return r;
		};
		feathers::rack::RackPatch patch = base;
		for (size_t di = 0; di < destinations.size(); ++di) {
			patch.route.push_back(routeOf(pairAt(di), patch));
		}
		const TestRoutes read = reload(throughBytes(patch)).routes;
		anyLanded = anyLanded || !read.empty();
		for (size_t di = 0; di < destinations.size(); ++di) {
			CAPTURE(si);
			CAPTURE(di);
			const TestRoute named = pairAt(di);
			// THE INDEPENDENT RESTATEMENT DECIDES WHAT SHOULD HAVE LANDED, not the authority — see the
			// namespace above `legalRouteIndependently` for why a sweep that asked the authority would be
			// asking one function whether it agrees with itself. It regressed that way once, in a
			// rewrite rather than by anyone deciding it, and feathers-cc refused to delete the dead
			// restatement to clear the warning: "a dead independent restatement is the single most
			// load-bearing-looking thing a warning can point at".
			const bool legal = legalRouteIndependently(named);
			const float expected = legal ? named.amount : 0.0f;
			// READ AT THE ENDS THE FILE NAMED. The record answers by endpoint, and a pair the rules
			// refuse has nothing landed at it — an undeclared output or an unmodulatable control is a
			// perfectly nameable pair (an id and an index) that the loader must drop, which is the
			// claim in the title.
			backplane::Source at {};
			backplane::Destination into {};
			REQUIRE(endpointsOfNamed(named, &at, &into));
			CHECK(amountIn(read, at, into) == doctest::Approx(expected));
		}
	}
	// NOT VACUOUS: something the rules allow does land.
	CHECK(anyLanded);
}

TEST_CASE("every channel the editor draws from raises the repaint flag, and a load raises it too") {
	// THE HALF OF THE REPAINT FIX A FUTURE CHANNEL WOULD FORGET. The editor's tick drains one bit and
	// redraws everything subscribed to it; that bit is only as good as the set of setters that raise
	// it, and adding a channel without raising it produces a surface that is silently a frame — or an
	// hour — out of date. So the set is enumerated here rather than trusted.
	//
	// A LOAD RAISES IT AND DOES NOT RAISE stateDirty_, which is the distinction worth pinning: the
	// host does not need telling about state it just handed us, but the SCREEN is never more wrong
	// than immediately after a load, when every channel moved at once and nothing on it knows.
	ParamBridge bridge;
	bridge.refresh();

	auto moved = [&bridge](auto&& mutate) {
		bridge.takeRepaintDirty(); // start clean
		mutate();
		return bridge.takeRepaintDirty();
	};

	SUBCASE("the editing setters") {
		CHECK(moved([&] { bridge.setOrientation(SliceOrientationPin { true, { 0.25 } }); }));
		CHECK(moved([&] { bridge.setFalloffPower(1.5); }));
		CHECK(moved([&] { bridge.setBlueShiftScales(0.5, 0.25); }));
		CHECK(moved([&] { bridge.setSoundTravel(2, AnchorPoint { 0.3, -0.1 }); }));
		CHECK(moved([&] { bridge.setSoundOrientation(2, SliceOrientationPin { true, { 0.5 } }); }));
		CHECK(moved([&] { bridge.setSoundGamma(2, 3.5); }));
		CHECK(moved([&] { bridge.setSoundResidue(2, PlaneResidue {}); }));
		CHECK(moved([&] { bridge.setSoundVariations(2, SoundVariations::List {}); }));
		CHECK(moved([&] { bridge.setSoundHue(2, 3); }));
		CHECK(moved([&] {
			bridge.setRouteAmount(backplane::Source::core(0), backplane::Destination::core(0), 0.5f);
		}));
		CHECK(moved([&] { bridge.setPreview(4); }));
		CHECK(moved([&] { bridge.clearPreview(); }));
	}

	SUBCASE("and the loads, which stateDirty_ deliberately ignores") {
		CHECK(moved([&] { bridge.restoreOrientation(SliceOrientationPin { true, { 0.75 } }); }));
		CHECK(moved([&] { bridge.setSoundTravel(SoundTravel {}); }));
		CHECK(moved([&] { bridge.setSoundOrientations(SoundOrientations {}); }));
		CHECK(moved([&] { bridge.restoreSoundGamma(SoundGamma {}); }));
		CHECK(moved([&] { bridge.setSoundResidue(SoundResidue {}); }));
		CHECK(moved([&] { bridge.restoreSoundVariations(SoundVariations {}); }));
		CHECK(moved([&] { bridge.restoreSoundHues(SoundHues {}); }));
		CHECK(moved([&] { bridge.restoreFxChain(defaultFxChain()); }));
		// THE ROUTES' LOAD IS THE CHAINS' PATCH (the separate route restore went with the dense table).
		CHECK(moved([&] { (void)bridge.restoreChains(bridge.chainsPatch()); }));

		// The other half of that distinction, stated as a measurement: a load leaves the host's flag
		// down and the screen's flag up.
		bridge.takeStateDirty();
		bridge.takeRepaintDirty();
		bridge.restoreSoundHues(SoundHues {});
		CHECK_FALSE(bridge.takeStateDirty());
		CHECK(bridge.takeRepaintDirty());
	}

	SUBCASE("the engine's own publishes do NOT raise it") {
		// Written from the audio thread once per block. Flagging them would mark the editor dirty at
		// block rate and turn a coalesced once-a-frame signal into an unconditional redraw of
		// everything, forever — which is the cost the batching exists to avoid.
		CHECK_FALSE(moved([&] { bridge.publishPuckVelocity(AnchorPoint { 0.5, 0.5 }); }));
		CHECK_FALSE(moved([&] { bridge.publishSoundWeights(SoundWeights {}); }));
		// AND THE AUTOMODULATOR'S PHASE, which is the newest one and the one most likely to have
		// been wired to the flag by habit: it publishes 750 times a second per slot, and the flag's
		// blast radius is the whole watched tree. The strip watches this value itself instead.
		CHECK_FALSE(moved([&] { bridge.publishAutomodReadings(0, 0.25f, 0.0f); }));
		// AND THE RACK PLAYHEAD, on the same terms: published every block, watched by the card that
		// draws it, never on the whole-tree flag.
		CHECK_FALSE(moved([&] {
			bridge.publishRackPlayhead(0, feathers::mod::Playhead { 1, 0.5f });
		}));
		CHECK_FALSE(moved([&] {
			bridge.publishModulated(defaultModRack(), feathers::FxChain {}, feathers::RackPlayheads {}, feathers::mod::TransportClock::unknown());
		}));
	}

	SUBCASE("it coalesces, and draining clears it") {
		bridge.takeRepaintDirty();
		bridge.setSoundGamma(1, 1.0);
		bridge.setSoundGamma(1, 2.0);
		bridge.setSoundHue(1, 2);
		// Three moves, one bit — which is the whole point of a payload-free signal.
		CHECK(bridge.takeRepaintDirty());
		CHECK_FALSE(bridge.takeRepaintDirty());
	}
}

TEST_CASE("the automod phase channel carries one number per slot, and refuses to invent one") {
	// THE ENGINE->EDITOR ROAD FOR A MARKER ON THE LFO'S WAVEFORM. One relaxed float per slot, written
	// once per tick by the DSP and read by the strip that draws it.
	//
	// **A VALUE, NOT A DIFFERENCE.** The claim is "the number the editor reads is the number the
	// device published", and no comparison of two readings can say that — see CLAUDE.md.
	ParamBridge bridge;
	bridge.refresh();

	// A fresh bridge has published nothing, and answers the origin rather than garbage.
	CHECK(bridge.automodPhase(0) == 0.0f);

	bridge.publishAutomodReadings(0, 0.25f, 0.0f);
	CHECK(bridge.automodPhase(0) == doctest::Approx(0.25f));

	// **AND THE ENVELOPE IS ITS OWN CHANNEL, NOT THE PHASE'S.** Written as two DIFFERENT numbers in
	// one call and read back separately, because a reader that returned the phase for both would pass
	// any check where they happened to be equal — which is exactly the defect this case was extended
	// for. `BridgeDeviceHost::readingAt` did precisely that: it answered `automodPhase` for ANY index
	// while declaring two readings, so the strip's envelope trace drew at rest in the plugin while
	// working in the rack. `ctest` was 72/72 through it; a PHOTOGRAPH of the card is what found it.
	bridge.publishAutomodReadings(0, 0.25f, 0.8f);
	CHECK(bridge.automodPhase(0) == doctest::Approx(0.25f));
	CHECK(bridge.automodEnvelope(0) == doctest::Approx(0.8f));
	CHECK(bridge.automodEnvelope(0) != doctest::Approx(bridge.automodPhase(0)));

	// CLAMPED, NOT WRAPPED — a level of 1.2 wrapped would read as 0.2 and draw a full meter as nearly
	// empty. The phase's guard one paragraph down is the opposite and deliberately so.
	bridge.publishAutomodReadings(0, 0.25f, 1.4f);
	CHECK(bridge.automodEnvelope(0) == doctest::Approx(1.0f));
	bridge.publishAutomodReadings(0, 0.25f, -0.3f);
	CHECK(bridge.automodEnvelope(0) == doctest::Approx(0.0f));
	bridge.publishAutomodReadings(0, 0.25f, 0.0f);

	// **KEYED BY SLOT, so two automodulators cannot collide.** Written as two different numbers in
	// two slots and read back in the opposite order, because a channel that ignored the key would
	// pass a test that only ever wrote one slot.
	bridge.publishAutomodReadings(3, 0.75f, 0.0f);
	CHECK(bridge.automodPhase(3) == doctest::Approx(0.75f));
	CHECK(bridge.automodPhase(0) == doctest::Approx(0.25f));
	CHECK(bridge.automodPhase(1) == 0.0f);

	// AN OUT-OF-RANGE SLOT IS DROPPED RATHER THAN WRITTEN, and reads as the origin. The publisher
	// runs on the audio thread, so the alternative to a bounds check is a stray store.
	bridge.publishAutomodReadings(bridge.fxTelemetryCapacity(), 0.9f, 0.0f);
	bridge.publishAutomodReadings(bridge.fxTelemetryCapacity() + 40, 0.9f, 0.0f);
	CHECK(bridge.automodPhase(bridge.fxTelemetryCapacity()) == 0.0f);
	for (size_t slot = 0; slot < kMaxFxSlots; ++slot) {
		CAPTURE(slot);
		const float expected = (slot == 0) ? 0.25f : (slot == 3) ? 0.75f : 0.0f;
		CHECK(bridge.automodPhase(slot) == doctest::Approx(expected));
	}

	// WRAPPED RATHER THAN CLAMPED, because a phase is a position on a circle and there is no end to
	// clamp to. The device cannot produce these — its phase is a `uint32_t` turn — so this is a
	// guard on the arithmetic at a thread boundary, stated here rather than left to be rediscovered.
	bridge.publishAutomodReadings(2, 1.25f, 0.0f);
	CHECK(bridge.automodPhase(2) == doctest::Approx(0.25f));
	bridge.publishAutomodReadings(2, -0.25f, 0.0f);
	CHECK(bridge.automodPhase(2) == doctest::Approx(0.75f));

	// AND A NON-FINITE READING IS ANSWERED WITH THE ORIGIN RATHER THAN STORED. A NaN reaching the
	// drawing multiplies a width by NaN, and a marker at NaN is not drawn anywhere at all — a
	// silently missing element rather than a visibly wrong one.
	bridge.publishAutomodReadings(2, std::numeric_limits<float>::quiet_NaN(), 0.0f);
	CHECK(bridge.automodPhase(2) == 0.0f);
	bridge.publishAutomodReadings(2, std::numeric_limits<float>::infinity(), 0.0f);
	CHECK(bridge.automodPhase(2) == 0.0f);
}

TEST_CASE("the rack playhead channel carries an ordinal and a phase that cannot come apart") {
	namespace md = feathers::mod;
	ParamBridge bridge;
	bridge.refresh();

	// **THE PROPERTY THE PACKING EXISTS FOR, STATED WHERE IT CAN BE CHECKED.** A pair of relaxed
	// atomics would let a reader take the ordinal from one publish and the phase from another; one
	// 64-bit atomic cannot, because there is one store and one load. Single-threaded a case cannot
	// WITNESS a straddle — so what is asserted is the thing that makes it impossible: the storage is
	// one lock-free word, and every read returns a pair that was written together.
	static_assert(std::atomic<uint64_t>::is_always_lock_free,
	              "the playhead's ordinal and phase share one word so they cannot be read apart; a "
	              "lock-free 64-bit atomic is what makes that a single store and a single load");

	// A FRESH BRIDGE HAS PUBLISHED NOTHING, and answers IDLE rather than segment 0 at phase 0. Those
	// are different claims: one says "nothing is playing", the other draws a marker at the start.
	CHECK_FALSE(bridge.rackPlayhead(0).running());
	CHECK(bridge.rackPlayhead(0).ordinal == md::Playhead::kIdle);

	// EVERY PAIR SURVIVES AS A PAIR. The ordinals and phases are chosen to be mutually inconsistent —
	// no phase belongs with more than one ordinal here — so a mixture of two publishes could not
	// masquerade as a third.
	const std::array<md::Playhead, 5> written {
		md::Playhead { 0, 0.0f },   md::Playhead { 1, 0.25f }, md::Playhead { 2, 0.5f },
		md::Playhead { 3, 0.75f },  md::Playhead { 4, 1.0f },
	};
	for (const md::Playhead& w : written) {
		bridge.publishRackPlayhead(1, w);
		const md::Playhead got = bridge.rackPlayhead(1);
		CAPTURE(w.ordinal);
		CHECK(got.ordinal == w.ordinal);
		CHECK(got.phase == w.phase); // EXACT: a bit pattern went in and the same one must come out
		CHECK(got == w);
	}

	// **KEYED BY SLOT**, written in two and read back in the opposite order — a channel that ignored
	// the key would pass a test that only ever wrote one slot.
	bridge.publishRackPlayhead(0, md::Playhead { 7, 0.125f });
	bridge.publishRackPlayhead(2, md::Playhead { 9, 0.875f });
	CHECK(bridge.rackPlayhead(2).ordinal == 9);
	CHECK(bridge.rackPlayhead(2).phase == doctest::Approx(0.875f));
	CHECK(bridge.rackPlayhead(0).ordinal == 7);
	CHECK(bridge.rackPlayhead(0).phase == doctest::Approx(0.125f));
	CHECK(bridge.rackPlayhead(1).ordinal == 4); // untouched by the two writes above

	// AN OUT-OF-RANGE SLOT IS DROPPED RATHER THAN WRITTEN, and reads idle. The publisher runs on the
	// audio thread, so the alternative to a bounds check is a stray store.
	bridge.publishRackPlayhead(bridge.rackTelemetryCapacity(), md::Playhead { 3, 0.5f });
	bridge.publishRackPlayhead(bridge.rackTelemetryCapacity() + 17, md::Playhead { 3, 0.5f });
	CHECK_FALSE(bridge.rackPlayhead(bridge.rackTelemetryCapacity()).running());

	// A NON-FINITE OR OUT-OF-RANGE PHASE IS ANSWERED WITH SOMETHING DRAWABLE. A NaN reaching the card
	// multiplies a width by NaN and the marker is drawn NOWHERE — a silently missing element rather
	// than a visibly wrong one, which is the worse of the two.
	bridge.publishRackPlayhead(3, md::Playhead { 2, std::numeric_limits<float>::quiet_NaN() });
	CHECK(bridge.rackPlayhead(3).phase == 0.0f);
	CHECK(bridge.rackPlayhead(3).ordinal == 2); // the ordinal was fine and is kept
	bridge.publishRackPlayhead(3, md::Playhead { 2, 4.0f });
	CHECK(bridge.rackPlayhead(3).phase == doctest::Approx(1.0f));
	bridge.publishRackPlayhead(3, md::Playhead { 2, -4.0f });
	CHECK(bridge.rackPlayhead(3).phase == doctest::Approx(0.0f));

	// AND THE WHOLE-RACK PUBLISH WRITES EVERY SLOT, so a rack that has shrunk cannot leave a marker
	// standing where its device used to be.
	feathers::RackPlayheads all(kMaxRackSlots);
	all[2] = md::Playhead { 5, 0.5f };
	bridge.publishModulated(defaultModRack(), feathers::FxChain {}, all, feathers::mod::TransportClock::unknown());
	CHECK(bridge.rackPlayhead(2).ordinal == 5);
	for (size_t slot = 0; slot < kMaxRackSlots; ++slot) {
		CAPTURE(slot);
		if (slot != 2) {
			CHECK_FALSE(bridge.rackPlayhead(slot).running());
		}
	}
}

TEST_CASE("a routing follows its device, and is dropped when that device is gone") {
	// THIS IS THE CASE THE WHOLE IDENTITY CHANGE EXISTS FOR, and it is two claims that only a
	// name-based reference can make at once. It was decided at the start of the arc that a saved routing
	// points at a stable instance id and a control rather than at a position; an earlier draft built
	// positional addressing anyway, on the reasoning that fixed strides made positions stable. They
	// are stable against adding and removing a device. They are not stable against REPLACING one.
	//
	// A rack with two devices: instance 7 in slot 0, instance 9 in slot 1.
	feathers::ModRack rack = feathers::defaultModRack();
	rack.count = 2;
	rack.setDahdsrAt(0, mod::dahdsr::Settings {});
	rack.setIdentityAt(0, { 7 });
	rack.setDahdsrAt(1, mod::dahdsr::Settings {});
	rack.setIdentityAt(1, { 9 });
	rack.nextInstanceId = 10;

	const feathers::device::Descriptor& desc =
	    rackDesc(0);
	const size_t control = desc.controlOfNthModulatableBesideEnable(0);

	TestRoute r = fromMacro(0);
	toDevice(r, 7, static_cast<uint32_t>(control)); // -> instance 7's first modulatable control
	r.amount = 0.5f;
	const feathers::rack::RackPatch written = patchWithRoutes({ r }, feathers::defaultFxChain(), rack);

	SUBCASE("it follows the device to a different slot") {
		// THE SAME DEVICE, MOVED. Instance 7 is now in slot 1 and instance 9 in slot 0 — a reorder,
		// which is the browser's most ordinary gesture. A positional reference would now be pointing
		// at instance 9's control; a name follows the thing it names.
		feathers::rack::RackPatch reordered = written;
		size_t swapped = 0;
		for (auto& d : reordered.device) {
			if (d.instanceId == 7 || d.instanceId == 9) {
				d.instanceId = d.instanceId == 7 ? 9 : 7;
				++swapped;
			}
		}
		REQUIRE(swapped == 2);
		const Reloaded back = reload(throughBytes(reordered));
		REQUIRE(feathers::instanceOfSlot(back.rack, 1) == 7);
		REQUIRE(feathers::instanceOfSlot(back.rack, 0) == 9);
		const TestRoutes& read = back.routes;
		const feathers::RouteRefusals& refused = back.refused;

		// **THE TWO DEVICES BY NAME (kill-lanes step 5).** These were the coordinate slot 1 answers to
		// and the one slot 0 answers to — which made the assertion "the amount moved with the device"
		// only because the case had checked which slot held which device. Naming the devices says the
		// same thing directly, and says it about the thing that cannot be re-pointed.
		const backplane::Source macro0 = backplane::Source::core(0);
		// THE VALUE, on the device that was routed...
		CHECK(amountIn(read, macro0, backplane::Destination::control(7, control))
		      == doctest::Approx(0.5f));
		// ...and NOT on the device it swapped places with, which is the half that fails for a positional
		// reference. Without this second check the first passes for a loader that ignored the move.
		CHECK(amountIn(read, macro0, backplane::Destination::control(9, control))
		      == doctest::Approx(0.0f));
		CHECK(refused.count == 0);
	}

	SUBCASE("it is dropped when that device has been replaced") {
		// THE SWAP. Slot 0 now holds a DIFFERENT device — a new instance, as a browser would stamp
		// when it put something else there. The route named instance 7 and instance 7 is not here.
		//
		// DROPPED, NOT RE-POINTED. A positional reference would silently have become a route to
		// whatever this device's first modulatable control happens to be — the same silent
		// re-pointing this arc has now killed on the destination axis, on the source axis, and here.
		feathers::rack::RackPatch swapped = written;
		size_t replaced = 0;
		for (auto& d : swapped.device) {
			if (d.instanceId == 7) {
				d.instanceId = 11; // something else entirely
				++replaced;
			}
		}
		REQUIRE(replaced == 1);
		const Reloaded back = reload(throughBytes(swapped));
		REQUIRE(back.rack.count == 2); // the device is there; only the route has nothing to name
		const TestRoutes& read = back.routes;
		const feathers::RouteRefusals& refused = back.refused;

		// **NOTHING LANDED ANYWHERE — said of the list rather than of every slot's coordinate.** The
		// sweep walked each rack slot's destination column and read zero; the list holds what is routed
		// and nothing else, so "the route was dropped, not re-pointed" is the whole list being empty —
		// which also covers a re-pointing onto the fx chain, or onto a parameter, that the old sweep
		// could not have seen.
		CHECK(read.empty());
		// AND IT IS COUNTED. feathers-1a's point: silence is how a dropped route becomes a bug report
		// three weeks later. "Swap is closed by construction" and "swap silently eats routes" are the
		// same code without this line.
		CHECK(refused.count == 1);
	}
}

TEST_CASE("a route to a device survives a real save and load of the chains block") {
	// THE ROUND TRIP THE SWAP CASE DOES NOT MAKE. That case doctors a patch's identities by hand, so it
	// proves the RESOLUTION rule and says nothing about whether the identities the writer writes survive
	// a file. They are two different claims.
	//
	// This walks the real road: the bridge's own patch, the real bytes, a fresh bridge — and the
	// identities asserted as values on the far side.
	feathers::ModRack rack = feathers::defaultModRack();
	rack.count = 2;
	rack.setDahdsrAt(0, mod::dahdsr::Settings {});
	rack.setIdentityAt(0, { 7 });
	rack.setDahdsrAt(1, mod::dahdsr::Settings {});
	rack.setIdentityAt(1, { 9 });
	rack.nextInstanceId = 10;

	const feathers::device::Descriptor& desc =
	    rackDesc(1);
	const size_t control = desc.controlOfNthModulatableBesideEnable(0);
	// **THE SECOND DEVICE'S OWN CONTROL, by the id the fixture stamped on it.** It was that control's
	// destination COORDINATE, computed from the slot the device sits in; naming the device is what makes
	// "pointing at the same control" mean the same control rather than the same address.
	const backplane::Source macro0 = backplane::Source::core(0);
	const backplane::Destination destination = backplane::Destination::control(9, control);

	const Reloaded back =
	    reload(throughBytes(patchOf(feathers::defaultFxChain(), rack, { { macro0, destination, 0.5f } })));

	// THE IDENTITIES SURVIVED. Asserted as VALUES rather than inferred from the route landing,
	// because a route landing is also what a positional loader would produce.
	CHECK(feathers::instanceOfSlot(back.rack, 0) == 7);
	CHECK(feathers::instanceOfSlot(back.rack, 1) == 9);
	CHECK(back.rack.nextInstanceId >= 10);

	// AND THE ROUTE CAME BACK POINTING AT THE SAME CONTROL.
	CHECK(amountIn(back.routes, macro0, destination) == doctest::Approx(0.5f));
	CHECK(back.refused.count == 0);
}

TEST_CASE("the rack's identities survive the bridge, not just the file") {
	// THE AIMED CASE FOR A LOSS THAT ONLY A CATCH-ALL NOTICED. The rack gained instance ids, they rode
	// the FILE correctly, and the bridge's flattened channel zeroed every one on the way through — so
	// a saved patch's device routes resolved against a rack with no identities and were dropped, with
	// the whole suite green.
	//
	// What found it was the bytes-identical resave check in test_clap_render: a loaded patch no longer
	// saved back to itself. That is a real catch and it is the wrong shape to rely on — it names an
	// OFFSET, not a cause, and it only fires because the loss happens to change the bytes. A field
	// that vanished without changing a length would have passed it.
	//
	// THE HAZARD IS GENERAL TO FLATTENED CHANNELS: the table is the only road this state takes from
	// the editor to the engine and to a save, so a field the table does not carry is a field that does
	// not exist downstream of it. Nothing warns; the value is simply the default forever.
	ParamBridge bridge;

	feathers::ModRack rack = feathers::defaultModRack();
	rack.count = 3;
	rack.setDahdsrAt(0, mod::dahdsr::Settings {});
	rack.setIdentityAt(0, { 4 });
	rack.setDahdsrAt(1, mod::dahdsr::Settings {});
	rack.setIdentityAt(1, { 8 });
	rack.setDahdsrAt(2, mod::dahdsr::Settings {});
	rack.setIdentityAt(2, { 15 });
	rack.nextInstanceId = 16;
	{
		// A SETTING TOO, so the case cannot pass by carrying identity and losing everything else.
		// Read-modify-write through the typed accessors now that a slot holds a device row rather
		// than a Settings that could be assigned into.
		mod::dahdsr::Settings s = rack.dahdsrAt(1);
		s.sustain = 0.375f;
		rack.setDahdsrAt(1, s);
	}

	bridge.restoreModRack(rack);
	const feathers::ModRack back = bridge.modRack();

	// THE VALUES, asserted one by one rather than as "something came through".
	CHECK(back.count == 3);
	CHECK(feathers::instanceOfSlot(back, 0) == 4);
	CHECK(feathers::instanceOfSlot(back, 1) == 8);
	CHECK(feathers::instanceOfSlot(back, 2) == 15);
	CHECK(back.nextInstanceId == 16);
	CHECK(back.dahdsrAt(1).sustain == doctest::Approx(0.375f));

	SUBCASE("and a route made against them still resolves after the round trip") {
		// The consequence, end to end: the identities are only worth carrying if a routing written
		// against them still finds its control on the way back.
		const feathers::device::Descriptor& desc =
		    rackDesc(1);
		const size_t control = desc.controlOfNthModulatableBesideEnable(0);
		// BY THE ID THE FIXTURE GAVE SLOT 1, which is the identity this whole case is about carrying.
		const backplane::Source macro0 = backplane::Source::core(0);
		const backplane::Destination destination = backplane::Destination::control(8, control);

		const Reloaded read = reload(
		    throughBytes(patchOf(feathers::defaultFxChain(), back, { { macro0, destination, 0.25f } })));
		CHECK(feathers::instanceOfSlot(read.rack, 1) == 8);
		CHECK(amountIn(read.routes, macro0, destination) == doctest::Approx(0.25f));
		CHECK(read.refused.count == 0);
	}
}

TEST_CASE("a device row says what it is and how much of it is real") {
	// THE TRANSPORT CONVENTION, exercised so that the header is COMPILED by the suite rather than
	// merely present. A header no translation unit includes is not checked by the build at all, which
	// would be a poor start for a file whose whole subject is state that silently is not carried.
	using feathers::DeviceRow;

	SUBCASE("an empty row names nothing") {
		DeviceRow row;
		CHECK_FALSE(feathers::rowIsPresent(row));
		CHECK(feathers::descriptorForRow(row) == nullptr);
	}

	SUBCASE("a row names its device, and the registry can resolve it") {
		DeviceRow row;
		// THROUGH `tagOf`, NOT A BARE LOOP. `traits.tag` is a string literal, so a loop running to
		// the field's width reads past its end -- harmless while the field was four bytes and the
		// literal five, an overrun the moment it widened.
		row.tag = feathers::device::tagOf(feathers::mod::dahdsr::kTraits.tag);
		row.instanceId = 3;
		row.fieldsPresent = feathers::kDahdsrFieldCount;
		CHECK(feathers::rowIsPresent(row));
		const feathers::device::Descriptor* desc = feathers::descriptorForRow(row);
		REQUIRE(desc != nullptr);
		CHECK(std::string(desc->traits.name) == "DAHDSR");
	}

	SUBCASE("a device this build has never heard of resolves to nothing, which is not the same as empty") {
		// The distinction the chain record was designed around: "unknown device" and "no device" are
		// different facts, and a caller has to be able to tell them apart to carry one and skip the
		// other.
		DeviceRow row;
		row.tag = { 'w', 'o', 'b', '1' };
		row.instanceId = 9;
		row.fieldsPresent = 4;
		CHECK(feathers::rowIsPresent(row));                       // it names A device...
		CHECK(feathers::descriptorForRow(row) == nullptr);        // ...just not one we know
	}

	SUBCASE("every registered device fits the row, including the widest") {
		// The runtime twin of the static_assert, here so a failure names the device rather than only
		// the line. The MSEG is why the bound is not the retired control stride: it declares
		// thirty-seven controls of which four are modulatable, so a row sized by the MODULATABLE
		// stride would truncate it silently.
		size_t widest = 0;
		for (const feathers::device::Descriptor& d : feathers::kDevices) {
			CAPTURE(std::string(d.traits.name));
			CHECK(d.params.size() <= feathers::kMaxDeviceFields);
			widest = std::max(widest, d.params.size());
		}
		CHECK(widest > feathers::device::kMaxDeviceFields / 2); // not vacuous: something is wide
	}
}

TEST_CASE("a device row converts both ways without anyone getting fieldsPresent wrong") {
	// THE CONVERSION IS WHERE THE RULE IS ENFORCED OR BROKEN — feathers-1a's catch. The carrier alone
	// left four small correct-looking copy loops to be written twice, in two files, at the exact seam
	// the header exists to unify.
	using feathers::DeviceRow;
	using feathers::fromDeviceRow;
	using feathers::toDeviceRow;

	SUBCASE("a real device round-trips through the carrier") {
		// The MSEG, because it is the widest and the one a naive stride would truncate.
		feathers::mod::mseg::Row row {};
		for (size_t i = 0; i < row.size(); ++i) {
			row[i] = 0.5f + static_cast<float>(i);
		}
		const DeviceRow carried =
		    toDeviceRow(feathers::mod::mseg::kTraits.tag, { 5 }, row);

		CHECK(carried.instanceId == 5);
		CHECK(feathers::rowIsPresent(carried));
		// STAMPED FROM THE DEVICE'S OWN WIDTH, not from the carrier's.
		CHECK(carried.fieldsPresent == row.size());
		CHECK(carried.fieldsPresent != feathers::kMaxDeviceFields);
		REQUIRE(feathers::descriptorForRow(carried) != nullptr);
		CHECK(std::string(feathers::descriptorForRow(carried)->traits.name) == "MSEG");

		feathers::mod::mseg::Row back {};
		CHECK(fromDeviceRow(carried, back) == row.size());
		CHECK(back == row);
	}

	SUBCASE("a short row reports what it really has, so absent fields can be defaulted") {
		// THE RETURN VALUE IS THE POINT. The caller's next move is stateOf(row, fieldsPresent), and
		// that argument is what puts absent fields at their DECLARED DEFAULTS rather than at zero.
		feathers::fx::filter::Row row {};
		row.fill(7.0f);
		DeviceRow carried =
		    toDeviceRow(feathers::fx::filter::kTraits.tag, { 2 }, row);
		carried.fieldsPresent = 4; // as an older build would have written it

		feathers::fx::filter::Row back {};
		back.fill(-1.0f);
		CHECK(fromDeviceRow(carried, back) == 4);
		for (size_t i = 0; i < 4; ++i) {
			CAPTURE(i);
			CHECK(back[i] == doctest::Approx(7.0f));
		}
		// UNTOUCHED, not zeroed — the caller decides what absent means.
		for (size_t i = 4; i < back.size(); ++i) {
			CAPTURE(i);
			CHECK(back[i] == doctest::Approx(-1.0f));
		}
	}

	SUBCASE("a row claiming more fields than the device has does not walk off the end") {
		// A count read out of a FILE may claim anything, and must not be believed. Two clamps guard
		// two different things: what the carrier can hold, and what this device can take.
		feathers::fx::filter::Row row {};
		row.fill(3.0f);
		DeviceRow carried =
		    toDeviceRow(feathers::fx::filter::kTraits.tag, { 1 }, row);
		carried.fieldsPresent = 50; // a ten-field device, a file claiming fifty

		feathers::fx::filter::Row back {};
		CHECK(fromDeviceRow(carried, back) == back.size()); // clamped to the device, not to 50
	}

	SUBCASE("and a count past the carrier itself is clamped too") {
		feathers::mod::mseg::Row row {};
		DeviceRow carried =
		    toDeviceRow(feathers::mod::mseg::kTraits.tag, { 1 }, row);
		carried.fieldsPresent = feathers::kMaxDeviceFields * 4;
		feathers::mod::mseg::Row back {};
		const size_t took = fromDeviceRow(carried, back);
		CHECK(took <= feathers::kMaxDeviceFields);
		CHECK(took == back.size());
	}
}

TEST_CASE("the fx lane's settings survive the bridge, keytrack included") {
	// THE CHANNEL THE SWALLOWING NEEDS. The filter's controls are still parameters today, so nothing
	// in the engine reads these settings yet — but the road is built before the traffic, because the
	// rack's road was built after and it presented as three unrelated bugs.
	//
	// KEYTRACK IS THE FIELD THIS CASE EXISTS FOR. `Settings` is what the DSP reads and deliberately
	// omits it — keytracking decides how many filters there are and where they sit, which is the
	// owner's architecture rather than a number the sample loop reads. But it IS a declared control
	// and IS patch state, so a channel that took a `Settings` would drop the bit on every write and it
	// would read back as "the user had keytracking off": a plausible patch rather than a corrupt one,
	// which is the kind of loss a flattened row makes invisible.
	feathers::ParamBridge bridge;

	feathers::fx::filter::State state;
	state.settings.enabled = true;
	state.settings.type = feathers::fx::filter::Type::Bandpass;
	state.settings.cutoffLog2Hz = 10.5f;
	state.settings.resonance = 0.75f;
	state.settings.drive = 0.4f;
	state.settings.macro1 = -0.6f;
	state.settings.macro2 = 0.3f;
	state.settings.slope = feathers::fx::filter::Slope::db24;
	state.settings.spread = -0.8f;
	state.keytrack = true; // the bit with no home in Settings

	bridge.setFilterSettings(0, state);

	const feathers::FxChain chain = bridge.fxChain();
	const feathers::DeviceRow& row = fxDeviceAtPosition(chain, 0).row;

	// THE CARRIER SAYS WHAT IT IS AND HOW MUCH OF IT IS REAL.
	CHECK(std::string(row.tag.data(), 4) == std::string(feathers::fx::filter::kTraits.tag));
	CHECK(row.fieldsPresent == static_cast<size_t>(feathers::fx::filter::Param::kCount));

	// AND THE STATE COMES BACK WHOLE — read through the shared helper, whose return value is the
	// count that puts absent fields at their declared defaults rather than at zero.
	feathers::fx::filter::Row decoded {};
	const size_t present = feathers::fromDeviceRow(row, decoded);
	const feathers::fx::filter::State back = feathers::fx::filter::stateOf(decoded, present);

	CHECK(back == state);
	// Named separately as well as compared, so a failure says WHICH field rather than "{?} == {?}".
	CHECK(back.keytrack == true);
	CHECK(back.settings.cutoffLog2Hz == doctest::Approx(10.5f));
	CHECK(back.settings.spread == doctest::Approx(-0.8f));
	CHECK(back.settings.slope == feathers::fx::filter::Slope::db24);

	SUBCASE("and a row that carries fewer fields keeps the DECLARED defaults, not zeros") {
		// The rule the whole convention exists for: a reader must be able to tell "this device had
		// fewer fields" from "these fields were zero". Zero is a real cutoff and a real resonance, so
		// a short row read as zeros is a different filter rather than an obviously broken one.
		feathers::DeviceRow shortRow = row;
		shortRow.fieldsPresent = 3; // enabled, type, cutoff — and nothing after

		feathers::fx::filter::Row partial {};
		const size_t took = feathers::fromDeviceRow(shortRow, partial);
		CHECK(took == 3);
		const feathers::fx::filter::State recovered = feathers::fx::filter::stateOf(partial, took);

		// What the payload reached is what it said.
		CHECK(recovered.settings.enabled == true);
		CHECK(recovered.settings.cutoffLog2Hz == doctest::Approx(10.5f));

		// What it did not reach is the DECLARATION's default rather than zero. Resonance defaults to
		// 0.2 — a value a zero-filled read would silently replace with 0.
		const auto& resDesc =
		    feathers::fx::filter::kParamDescs[static_cast<size_t>(feathers::fx::filter::Param::Resonance)];
		CHECK(recovered.settings.resonance == doctest::Approx(static_cast<float>(resDesc.def)));
		CHECK(resDesc.def != 0.0); // or the check above proves nothing
	}

	SUBCASE("a row claiming more fields than the device has is clamped, not believed") {
		// A file may claim anything. Two clamps: what the carrier holds, and what this device can
		// take. Neither may overrun.
		feathers::DeviceRow tooMany = row;
		tooMany.fieldsPresent = 9999;

		feathers::fx::filter::Row decoded2 {};
		const size_t took = feathers::fromDeviceRow(tooMany, decoded2);
		CHECK(took == static_cast<size_t>(feathers::fx::filter::Param::kCount));
	}
}

TEST_CASE("a patch load fills the lane and an edit marks it dirty, restore does not") {
	// The same split every channel on this bridge keeps, and the one a load must not get wrong:
	// opening a patch is not modifying it.
	feathers::ParamBridge bridge;

	feathers::FxChain chain = feathers::defaultFxChain();
	feathers::fx::filter::State state;
	state.settings.resonance = 0.9f;
	state.keytrack = true;
	chain.device[0].row =
	    feathers::toDeviceRow(feathers::fx::filter::kTraits.tag,
	                          { 7 }, feathers::fx::filter::rowOf(state));
	chain.count = 1;

	bridge.takeStateDirty(); // clear whatever construction left
	bridge.restoreFxChain(chain);
	CHECK_FALSE(bridge.takeStateDirty());

	const feathers::FxChain read = bridge.fxChain();
	CHECK(read.count == 1);
	CHECK(fxDeviceAtPosition(read, 0).instanceId() == 7);

	feathers::fx::filter::Row decoded {};
	const size_t present = feathers::fromDeviceRow(fxDeviceAtPosition(read, 0).row, decoded);
	const feathers::fx::filter::State back = feathers::fx::filter::stateOf(decoded, present);
	CHECK(back.settings.resonance == doctest::Approx(0.9f));
	CHECK(back.keytrack == true);

	SUBCASE("and an edit through the channel DOES mark it dirty") {
		feathers::fx::filter::State moved = back;
		moved.settings.drive = 0.5f;
		bridge.setFilterSettings(0, moved);
		CHECK(bridge.takeStateDirty());
	}

	SUBCASE("and the identity survives an edit that did not mention it") {
		// setFilterSettings writes a payload and a tag; the instance id is the slot's and must not be
		// reset to nothing by a control edit. A slot whose id vanished is a slot nothing can route to,
		// and the loss is silent — the rack shipped exactly that bug.
		feathers::fx::filter::State moved = back;
		moved.settings.drive = 0.5f;
		bridge.setFilterSettings(0, moved);
		CHECK(bridge.fxChain().device[0].instanceId() == 7);
	}
}

TEST_CASE("a rack holding two different device types survives a real save and load") {
	// THE OTHER HALF OF test_mod_rack's "a rack slot holds a device" — that one proves the rack can
	// EXPRESS two types at once; this proves the FILE can carry them. They are different claims and
	// the second is where the old serializer was pinned: it wrote kDahdsrTag and kDahdsrPayloadBytes
	// for every slot, which was correct only while a slot could not hold anything else.
	feathers::ModRack rack = feathers::defaultModRack();
	rack.count = 2;

	feathers::mod::mseg::Settings shape;
	shape.startLevel = 0.25f;
	shape.count = 3;
	rack.setMsegAt(1, shape);
	rack.setIdentityAt(1, { 9 });
	rack.nextInstanceId = 10;

	const feathers::ModRack back = reload(throughBytes(patchOf(feathers::defaultFxChain(), rack))).rack;

	CHECK(back.count == 2);
	CHECK(back.holds(0, feathers::mod::dahdsr::kTraits.tag));
	CHECK(back.holds(1, feathers::mod::mseg::kTraits.tag));
	// THE VALUES, not merely the tags — a loader that kept the tag and dropped the payload would
	// satisfy every check above and none of these.
	CHECK(back.msegAt(1).startLevel == doctest::Approx(0.25f));
	CHECK(back.msegAt(1).count == shape.count);
	CHECK(feathers::instanceOfSlot(back, 1) == 9);
	// AND THE ROWS CAME BACK DIFFERENT WIDTHS, which a single-type serializer could not produce.
	CHECK(back.device[0].fieldsPresent != back.device[1].fieldsPresent);
}

// ---------------------------------------------------------------------------------------------
// ADDING A DEVICE — the primitive the browser's (+) is built on.

TEST_CASE("adding a device to the lane stamps its tag, a fresh serial, and its DECLARED defaults") {
	namespace flt = feathers::fx::filter;
	feathers::FxChain chain;
	REQUIRE(chain.count == 0);

	const size_t slot = feathers::addFxDevice(&chain, feathers::fx::filter::kTraits.tag);
	REQUIRE(slot == 0);
	REQUIRE(chain.count == 1);
	CHECK(feathers::fxHolds(chain, 0, flt::kTraits.tag));

	// A SERIAL, NOT A ZERO. kNoInstance is a slot nothing can route to, which is the one thing a
	// device the user just added must not be.
	CHECK(fxDeviceAtPosition(chain, 0).instanceId() != feathers::kNoInstance);
	CHECK(feathers::slotOfFxInstance(chain, fxDeviceAtPosition(chain, 0).instanceId()) == 0);

	// THE DEFAULTS ARE THE DEVICE'S OWN, ASSERTED AS VALUES rather than as "not zero" — the whole
	// point of defaultRowFor is that a fresh device sounds the way its declarations say, and zero is
	// a different sound for most controls. The cutoff is the one that would be loudest if wrong: zero
	// in log2(Hz) is 1 Hz, not the knob's default.
	const flt::State state = feathers::fxFilterAt(chain, 0);
	CHECK(state.settings.cutoffLog2Hz
	      == doctest::Approx(flt::kParamDescs[static_cast<size_t>(flt::Param::Cutoff)].def));
	CHECK(state.settings.resonance
	      == doctest::Approx(flt::kParamDescs[static_cast<size_t>(flt::Param::Resonance)].def));
	// AND EVERY FIELD IS PRESENT rather than absent: a device added by a browser has no earlier state
	// to be absent from, so its width is stated. This is what stops a later reader inferring defaults
	// it was supposed to be told.
	CHECK(fxDeviceAtPosition(chain, 0).row.fieldsPresent == std::size(flt::kParamDescs));

	SUBCASE("and its controls are immediately routable") {
		// THE HONESTY RULE AT THE MOMENT OF CREATION. A device whose knobs are visible but whose
		// addresses do not resolve is the exact failure the swallowing's destination work exists to
		// prevent, and adding one is when it would first show.
		// **ASKED OF THE GATE A SURFACE ASKS (kill-lanes step 5).** This built a `DeviceSlots` and asked
		// `retiredBandRule::maySourceRouteTo` — the band rule, which the product stopped having at step
		// 4b. The authority is the record's, reached through a bridge holding this chain, and the
		// destination is the device's own control index rather than a column derived from its slot.
		feathers::ParamBridge bridge;
		bridge.restoreFxChain(chain);
		const feathers::DeviceInstanceId added = bridge.fxDeviceAt(0);
		REQUIRE(added != feathers::kNoInstance);
		CHECK(bridge.mayRoute(
		    backplane::Source::core(0),
		    backplane::Destination::control(added, static_cast<size_t>(flt::Param::Cutoff))));
	}
}

TEST_CASE("a serial is never reused, so a routing cannot be inherited by a later device") {
	namespace flt = feathers::fx::filter;
	feathers::FxChain chain;
	REQUIRE(feathers::addFxDevice(&chain, feathers::fx::filter::kTraits.tag) == 0);
	const feathers::DeviceInstanceId first = fxDeviceAtPosition(chain, 0).instanceId();

	REQUIRE(feathers::addFxDevice(&chain, feathers::fx::filter::kTraits.tag) == 1);
	const feathers::DeviceInstanceId second = fxDeviceAtPosition(chain, 1).instanceId();

	// THE ABA RULE, at the one place identities are issued. Two devices of the SAME KIND in the same
	// lane differ only by this number; if it repeated, a routing naming the first would silently
	// begin naming the second — which is the failure device_instance.h exists to prevent, and the
	// reason the counter lives on the lane rather than being derived from the count.
	CHECK(first != second);
	CHECK(chain.nextInstanceId > second);
	CHECK(feathers::slotOfFxInstance(chain, first) == 0);
	CHECK(feathers::slotOfFxInstance(chain, second) == 1);
}

TEST_CASE("add before: a new device lands at the anchor's CURRENT position, and the anchor keeps its routing") {
	// BACKLOG CR — 'add before' inserts a new effect into the chain before the effect whose ellipsis
	// menu was used. The anchor is the card's DEVICE (kill-lanes step 2), resolved to a
	// position when the insert runs; the anchor's routings must survive the insert — the BX property.
	//
	// **RENAMED AT kill-lanes STEP 3b: "on the lowest free lane" IS A DELETED INVARIANT, NOT A DROPPED
	// ASSERTION.** Where the record parks a device became its own business in 3b, and the surface numbers
	// devices by POSITION (`chainsOfState` mints each view row's lane from its place in the chain). So
	// there is no "lane" for a case to watch: two devices at two positions have two coordinates by
	// construction, which made the old "a lane of its own, not the anchor's" checks tautologies rather
	// than tests. What is left is the claim that was always the point — WHERE the newcomer lands, and that
	// the anchor's ROUTE comes through — and both are about identity, so both are asserted by identity.
	//
	// AND THE ROUTE IS ASSERTED BY ENDPOINT, which is what makes this case survive the change at all: a
	// coordinate recomputed after the insert names whatever now stands at that position, so comparing one
	// to itself would have passed while the route moved to another device.
	namespace flt = feathers::fx::filter;
	namespace hrm = feathers::fx::harm;
	ParamBridge bridge;
	// A FRESH BRIDGE IS NOT AN EMPTY CHAIN — it ships with its default filter — so every position
	// below is relative to what was there.
	const size_t base = bridge.fxChain().count;
	REQUIRE(bridge.addFxDevice(flt::kTraits.tag) == base);
	REQUIRE(bridge.addFxDevice(hrm::kTraits.tag) == base + 1);
	const feathers::DeviceInstanceId harm = feathers::instanceOfFxSlot(bridge.fxChain(), base + 1);
	const feathers::DeviceInstanceId filter = feathers::instanceOfFxSlot(bridge.fxChain(), base);
	const feathers::DeviceInstanceId front = bridge.fxDeviceAt(0);

	// A ROUTE ON THE ANCHOR, NAMED BY THE ANCHOR — macro 0 into harm's Width. Nothing here is a
	// coordinate, so nothing here moves when the chain does.
	constexpr size_t kControl = static_cast<size_t>(hrm::Param::Width);
	const feathers::backplane::Source macro = feathers::backplane::Source::core(0);
	const feathers::backplane::Destination atHarm = feathers::backplane::Destination::control(harm, kControl);
	bridge.setRouteAmount(macro, atHarm, -0.25f);
	REQUIRE(bridge.routeAmount(macro, atHarm) == doctest::Approx(-0.25f));

	SUBCASE("before the last device: it lands at the anchor's position and the anchor moves up one") {
		REQUIRE(bridge.insertFxDeviceBefore(harm, flt::kTraits.tag) == base + 1);
		CHECK(bridge.fxChain().count == base + 3);
		CHECK(feathers::fxHolds(bridge.fxChain(), base + 1, flt::kTraits.tag));
		CHECK(feathers::instanceOfFxSlot(bridge.fxChain(), base + 2) == harm);
		CHECK(feathers::instanceOfFxSlot(bridge.fxChain(), base) == filter);
		// THE ANCHOR'S ROUTE IS STILL THE ANCHOR'S, by value and by name.
		CHECK(bridge.routeAmount(macro, atHarm) == doctest::Approx(-0.25f));
		// AND THE NEWCOMER ARRIVED UNROUTED — asked of the device rather than of one control of it,
		// because "no route reaches it" is the claim and a single control cannot say that.
		const feathers::DeviceInstanceId arrived = bridge.fxDeviceAt(base + 1);
		REQUIRE(arrived != feathers::kNoInstance);
		REQUIRE(arrived != harm); // or "unrouted" would be a question about the anchor
		CHECK(bridge.routesOf(arrived).empty());
	}
	SUBCASE("before the first device: it lands at position 0") {
		REQUIRE(bridge.insertFxDeviceBefore(bridge.fxDeviceAt(0), hrm::kTraits.tag) == 0);
		CHECK(bridge.fxChain().count == base + 3);
		CHECK(feathers::fxHolds(bridge.fxChain(), 0, hrm::kTraits.tag));
		// EVERYONE ELSE SHIFTED UP ONE AND KEPT THEIR ORDER — by identity, which is the only thing a
		// shift preserves.
		CHECK(bridge.fxDeviceAt(1) == front);
		CHECK(bridge.fxDeviceAt(base + 2) == harm);
		CHECK(bridge.routeAmount(macro, atHarm) == doctest::Approx(-0.25f));
	}
	SUBCASE("the anchor is an IDENTITY: moved after the menu captured it, the insert still lands before it") {
		REQUIRE(bridge.moveFxDevice(base + 1, 0)); // harm to the front; the captured ID still names harm
		REQUIRE(bridge.insertFxDeviceBefore(harm, flt::kTraits.tag) == 0);
		CHECK(feathers::instanceOfFxSlot(bridge.fxChain(), 1) == harm);
		// AND THE MOVE PLUS THE INSERT LEFT THE ROUTE ON HARM — the whole point of the anchor being an
		// identity is that neither operation could take its routing somewhere else.
		CHECK(bridge.routeAmount(macro, atHarm) == doctest::Approx(-0.25f));
	}
	SUBCASE("an anchor that has left the chain appends") {
		REQUIRE(bridge.removeFxDevice(base + 1));
		REQUIRE(bridge.insertFxDeviceBefore(harm, flt::kTraits.tag) == base + 1);
		CHECK(bridge.fxChain().count == base + 2);
	}
	SUBCASE("a chain at its starting size grows: the insert lands before the anchor and nothing else moves") {
		// IT REFUSED HERE until unbounded devices (step 6): a chain is never full now. What survives of the old
		// claim is the part that was about writing: every device keeps its identity, and the ones after the
		// anchor stand one further along — said by identity, position by position.
		while (bridge.fxChain().count < feathers::kMaxFxSlots) {
			REQUIRE(bridge.addFxDevice(flt::kTraits.tag) != feathers::kNoPosition);
		}
		const feathers::FxChain before = bridge.fxChain();
		const size_t anchorAt = feathers::slotOfFxInstance(before, harm);
		REQUIRE(anchorAt < before.count);
		CHECK(bridge.insertFxDeviceBefore(harm, flt::kTraits.tag) == anchorAt);
		CHECK(bridge.fxChain().count == before.count + 1);
		for (size_t at = 0; at < before.count; ++at) {
			CAPTURE(at);
			const size_t now = at < anchorAt ? at : at + 1;
			CHECK(feathers::instanceOfFxSlot(bridge.fxChain(), now) == feathers::instanceOfFxSlot(before, at));
		}
	}
}

TEST_CASE("a settings edit is not a new device: every write road preserves the instance id") {
	// BACKLOG 8, AND ITS OWN TRIGGER FIRED BEFORE THIS WAS WRITTEN. The entry said `toDeviceRow`
	// taking the instance id as an ARGUMENT while `fieldsPresent` comes from the type is a correct
	// asymmetry — the id genuinely is the caller's to supply — but that after the convention removed
	// every other way to get a row wrong, *"the one number a caller can still pass wrongly is the
	// identity"*. It said to look again at the second and third caller. There are SIX.
	//
	// ALL SIX HAND-MAINTAIN ONE INVARIANT AND NOTHING ENFORCED IT: a settings edit keeps the id the
	// slot already had. Three read it back out of the row (`mod_rack.cpp:56,73`, `fx_chain.h:178`,
	// all spelled `const DeviceInstanceId keep = ...`) and three load it from the parallel atomic the
	// bridge keeps (`gui_bridge.cpp:164,181,825`). Same intent, two mechanisms, and a seventh caller
	// that passed `kNoInstance` or a fresh id would compile and look right.
	//
	// AND THE COST IS NOT A WRONG NUMBER, IT IS AN ORPHANED ROUTING. Routings name a device by
	// instance and resolve through `slotOfFxInstance`; an edit that changed the id would make that
	// lookup answer "not found" for a device sitting right there. So this asserts the CONSEQUENCE —
	// the device is still findable by the id it had — rather than only comparing two integers.
	namespace flt = feathers::fx::filter;
	namespace dsr = feathers::mod::dahdsr;
	namespace msg = feathers::mod::mseg;

	SUBCASE("FxChain::setFxFilterAt — the id is read back out of the row") {
		feathers::FxChain chain;
		REQUIRE(feathers::addFxDevice(&chain, feathers::fx::filter::kTraits.tag) == 0);
		const feathers::DeviceInstanceId before = fxDeviceAtPosition(chain, 0).instanceId();
		REQUIRE(before != feathers::kNoInstance);

		flt::State edited = flt::stateOf(flt::Row {}, 0);
		edited.settings.resonance = 0.42f;
		feathers::setFxFilterAt(&chain, 0, edited);

		CHECK(fxDeviceAtPosition(chain, 0).instanceId() == before);
		// THE CONSEQUENCE, which is the assertion that would survive someone "simplifying" the line
		// above into a comparison of the row against itself.
		CHECK(feathers::slotOfFxInstance(chain, before) == 0);
		// AND THE EDIT REALLY HAPPENED — read back through the chain, not off the local, or this
		// passes on a write that did nothing at all: exactly the vacuous shape this file keeps
		// catching.
		flt::Row back {};
		const size_t fields = feathers::fromDeviceRow(fxDeviceAtPosition(chain, 0).row, back);
		CHECK(flt::stateOf(back, fields).settings.resonance == doctest::Approx(0.42f));
	}

	SUBCASE("ModRack::setDahdsrAt and setMsegAt — same rule on the other chain") {
		feathers::ModRack rack;
		rack.device[0] = feathers::toDeviceRow(dsr::kTraits.tag, { 7u }, dsr::rowOf(dsr::Settings {}));
		rack.device[1] = feathers::toDeviceRow(msg::kTraits.tag, { 9u }, msg::rowOf(msg::Settings {}));

		dsr::Settings env {};
		env.attack = 0.33f;
		rack.setDahdsrAt(0, env);
		CHECK(rack.device[0].instanceId == 7u);
		CHECK(rack.dahdsrAt(0).attack == doctest::Approx(0.33f));

		msg::Settings shape {};
		shape.startLevel = 0.75f;
		rack.setMsegAt(1, shape);
		CHECK(rack.device[1].instanceId == 9u);
		CHECK(rack.msegAt(1).startLevel == doctest::Approx(0.75f));

		// AND NEITHER EDIT REACHED THE OTHER SLOT — two devices of DIFFERENT kinds here, so a write
		// that stamped the wrong slot would also be a write that changed a tag.
		CHECK(feathers::device::sameTag(rack.device[0].tag.data(), dsr::kTraits.tag));
		CHECK(feathers::device::sameTag(rack.device[1].tag.data(), msg::kTraits.tag));
	}

	SUBCASE("ParamBridge's three roads — where the id lives in a parallel atomic, not in the row") {
		// THE MECHANISM DIFFERS HERE AND THE INVARIANT DOES NOT, which is the whole reason this
		// subcase exists beside the two above rather than trusting them to cover it. The bridge cannot
		// read the id out of the row it is about to build, so it loads the atomic it keeps beside the
		// fields — a different line to get wrong, holding the same promise.
		ParamBridge bridge;
		bridge.refresh();
		const size_t fx = bridge.addFxDevice(flt::kTraits.tag);
		REQUIRE(fx < feathers::kMaxFxDevices);
		const feathers::DeviceInstanceId fxBefore = bridge.fxChain().device[fx].instanceId();
		REQUIRE(fxBefore != feathers::kNoInstance);

		flt::State edited = flt::stateOf(flt::Row {}, 0);
		edited.settings.resonance = 0.61f;
		bridge.setFilterSettings(fx, edited);
		bridge.refresh();
		CHECK(bridge.fxChain().device[fx].instanceId() == fxBefore);
		CHECK(feathers::slotOfFxInstance(bridge.fxChain(), fxBefore) == fx);

		const size_t rackSlot = bridge.addRackDevice(dsr::kTraits.tag);
		REQUIRE(rackSlot < feathers::kMaxRackEnvelopes);
		const feathers::DeviceInstanceId envBefore =
		    bridge.modRack().device[rackSlot].instanceId;
		REQUIRE(envBefore != feathers::kNoInstance);

		dsr::Settings env {};
		env.decay = 0.27f;
		bridge.setEnvelopeSettings(rackSlot, env);
		bridge.refresh();
		CHECK(bridge.modRack().device[rackSlot].instanceId == envBefore);
		CHECK(bridge.modRack().dahdsrAt(rackSlot).decay == doctest::Approx(0.27f));
	}
}

TEST_CASE("which lane a device may sit in is derived from what it declares, not from where it is put") {
	namespace flt = feathers::fx::filter;
	feathers::FxChain chain;
	// EMPTIED DELIBERATELY: a default-constructed ModRack already holds one envelope, which is the
	// rack a fresh instance gets rather than an empty container. Starting from it would have made the
	// counts below say nothing.
	feathers::ModRack rack;
	rack.count = 0;

	// A MODULATOR MAY SIT IN THE FX LANE — INVERTED 2026-10-04, not deleted. This said the lane refused the
	// DAHDSR because it declares no AudioOps. Every mod device is in scope in the
	// Feathers fx rack, so the lane asks `maySitInLane` — still of the device, never of a table — and a
	// modulator answers yes. Taken here and removed again, so the counts below start from empty.
	CHECK(feathers::addFxDevice(&chain, mod::dahdsr::kTraits.tag) == 0);
	CHECK(chain.count == 1);
	CHECK(feathers::maySitInLane(*feathers::deviceForTag(mod::dahdsr::kTraits.tag)));
	CHECK_FALSE(feathers::maySitInAudioChain(*feathers::deviceForTag(mod::dahdsr::kTraits.tag)));
	chain = feathers::FxChain {};

	// AND AN EFFECT IS NOT A MODULATOR. The filter publishes no outputs, so there is no signal for a
	// rack slot to carry.
	CHECK(feathers::addRackDevice(&rack, flt::kTraits.tag) == feathers::kNoPosition);
	CHECK(rack.count == 0);

	// Each in its own lane is accepted, so the refusals above are a real test rather than a mutation
	// that broke both roads.
	CHECK(feathers::addFxDevice(&chain, feathers::fx::filter::kTraits.tag) == 0);
	CHECK(feathers::addRackDevice(&rack, mod::dahdsr::kTraits.tag) == 0);

	// A TAG THIS BUILD DOES NOT KNOW IS REFUSED RATHER THAN STAMPED, which is what keeps a slot from
	// holding a device nothing can render or route to.
	CHECK(feathers::addFxDevice(&chain, "zzzz") == feathers::kNoPosition);
	CHECK(feathers::addRackDevice(&rack, "zzzz") == feathers::kNoPosition);
	CHECK(chain.count == 1);
	CHECK(rack.count == 1);
}

TEST_CASE("HARM may be seated now that it declares a way to render, and it renders in both lanes' terms") {
	// THIS CASE WAS WRITTEN TO GO RED AND THIS IS IT GOING RED, INVERTED RATHER THAN DELETED.
	//
	// Until HARM had a DSP it declared no AudioOps and the lane refused it — correct, because a
	// browser offering it would have been offering a device that renders silence. The refusal was
	// written as a case precisely so that the day it stopped being true, something would say so: a
	// refusal quietly becoming an acceptance is indistinguishable from the feature landing, and the
	// deleted version of this case would have told nobody which had happened.
	feathers::FxChain chain;
	const feathers::device::Descriptor* harm = feathers::deviceForTag(feathers::fx::harm::kTraits.tag);
	REQUIRE(harm != nullptr);
	REQUIRE(harm->audio != nullptr);
	CHECK(feathers::addFxDevice(&chain, feathers::fx::harm::kTraits.tag) == 0);
	CHECK(feathers::fxHolds(chain, 0, feathers::fx::harm::kTraits.tag));

	// AND IT IS STILL NOT A MODULATOR. HARM publishes no outputs, so the rack refuses it for the
	// reason it always did — the two predicates are independent and gaining one did not grant both.
	feathers::ModRack rack;
	rack.count = 0;
	CHECK(feathers::addRackDevice(&rack, feathers::fx::harm::kTraits.tag) == feathers::kNoPosition);

	SUBCASE("and its modulatable controls are addressable the moment it is seated") {
		// A DEVICE THE BROWSER CAN ADD MUST BE A DEVICE THE MATRIX CAN REACH. HARM declares eight
		// modulatable controls of ten, and this is the first time anything but the filter has occupied
		// an fx slot.
		//
		// **ASKED OF THE GATE A SURFACE ASKS (kill-lanes step 5).** It built a `DeviceSlots` and asked
		// `retiredBandRule::maySourceRouteTo`, the retired band rule; the authority is the record's. And
		// a control is named by its own index, so "the stride or the ordinal walk could not carry a
		// second fx device" is no longer a way this can fail — what is left, and what the case still
		// says, is that the device's controls are reachable and its architecture switches are not.
		feathers::ParamBridge bridge;
		bridge.restoreFxChain(chain);
		const feathers::DeviceInstanceId seated = bridge.fxDeviceAt(0);
		REQUIRE(seated != feathers::kNoInstance);
		CHECK(bridge.mayRoute(backplane::Source::core(0),
		                      backplane::Destination::control(
		                          seated, static_cast<size_t>(feathers::fx::harm::Param::Level))));

		// AND ITS ARCHITECTURE SWITCHES ARE NOT DESTINATIONS, the same rule the filter's Poly and
		// Keytrack carry: they decide how many of the device there are, not what one of them does.
		for (const feathers::fx::harm::Param p :
		     {feathers::fx::harm::Param::Poly, feathers::fx::harm::Param::Harmonic}) {
			CAPTURE(static_cast<int>(p));
			CHECK_FALSE(bridge.mayRoute(
			    backplane::Source::core(0),
			    backplane::Destination::control(seated, static_cast<size_t>(p))));
		}
	}
}

TEST_CASE("a lane at its starting size grows for the next device rather than overwriting the last") {
	// IT REFUSED HERE until unbounded devices (step 6). The half of the old claim that still matters is that
	// the device at the end is untouched and the serial moves by exactly one: an add past the old size takes a
	// new row and the next serial, never the last device's.
	feathers::FxChain chain;
	for (size_t i = 0; i < feathers::kMaxFxSlots; ++i) {
		CAPTURE(i);
		REQUIRE(feathers::addFxDevice(&chain, feathers::fx::filter::kTraits.tag) == i);
	}
	const feathers::DeviceInstanceId last = fxDeviceAtPosition(chain, feathers::kMaxFxSlots - 1).instanceId();

	CHECK(feathers::addFxDevice(&chain, feathers::fx::filter::kTraits.tag) == feathers::kMaxFxSlots);
	CHECK(chain.count == feathers::kMaxFxSlots + 1);
	CHECK(chain.device.size() > feathers::kMaxFxSlots);
	CHECK(fxDeviceAtPosition(chain, feathers::kMaxFxSlots - 1).instanceId() == last);
	CHECK(fxDeviceAtPosition(chain, feathers::kMaxFxSlots).instanceId() == last + 1);
	CHECK(chain.nextInstanceId == last + 2);
}

// ---------------------------------------------------------------------------------------------
// REMOVING AND REORDERING — and the live routing table that has to move with the devices.
//
// THE CLAIM EVERY CASE HERE MAKES IS ABOUT A VALUE AT AN ADDRESS, never about a table being
// "different". A permutation applied wrongly produces a table that differs from the old one in
// exactly the way a correct one does; only asking what is at a specific address separates them.

namespace {

/// A lane of `n` filters, each with a serial of its own.
feathers::FxChain laneOf(size_t n) {
	feathers::FxChain chain;
	for (size_t i = 0; i < n; ++i) {
		REQUIRE(feathers::addFxDevice(&chain, feathers::fx::filter::kTraits.tag) == i);
	}
	return chain;
}

// **`fxCutoffOf` WAS HERE AND IS DELETED WITH ITS LAST CALLER (kill-lanes step 3c).** It answered the
// flat-table destination naming one control of the device in fx slot `slot`, and it went through
// `laneOfFxSlot` — the lane a device had STORED — because, as its own note said, reading
// `deviceSlotOfFx` instead *"would agree with the truth in every case here up to the first mutation and
// disagree after it — which is the only interesting half"*. That note now reads exactly backwards: a
// device stores no lane, the surface mints a coordinate from where the device is drawn, and the position
// IS the answer. Every case that used it was asking what a flat `ModRoutes` looked like after a verb
// that no longer touches one, and each is now asked of the bridge by endpoint instead.

} // namespace

// **THE ROUTING HALF OF THIS SECTION LEFT THE FREE FUNCTIONS (kill-lanes step 3c).** `removeFxDevice`,
// `removeRackDevice`, `insertFxDevice` and `insertRackDevice` took a `ModRoutes*` — required, not
// optional — because a device's routing columns were addressed by a LANE, and a removal had to clear
// them while the lane was still readable out of the row. None of them takes one now: a route names an
// instance id, and the routes a structural verb must drop or bring back are the RECORD's
// (`ChainRecord::remove`, `ParamBridge::routesOf`).
//
// SO A CASE HERE THAT ASKED WHAT A FLAT `ModRoutes` LOOKED LIKE AFTER A VERB WOULD BE ASKING ABOUT A
// TABLE THE VERB NO LONGER TOUCHES — a question whose answer is "unchanged" whatever the verb does,
// which is the most convincing shape a vacuous check comes in. The claims themselves were real and are
// asked of the BRIDGE by endpoint below, which is the one place they can still go red.

TEST_CASE("removing a device closes the gap, keeps the survivors' identities, and clears the vacated tail") {
	// WHAT THE STRUCTURAL VERB ITSELF PROMISES, WITH NO TABLE IN SIGHT (kill-lanes step 3c). This case
	// was "removing a device leaves the later devices' routings exactly where they are", and that claim
	// was about the lane: a device carried its routing address in its row, so a removal moved the rows
	// and left every survivor's columns where they were. **Both halves of that are deleted** — a device
	// has no address, and its coordinate now follows its position, so a removal renumbers the survivors
	// by construction and no flat table follows them. The claim that replaces it is about a route and a
	// device rather than a number, and it is asked of the bridge in the case below this one.
	//
	// WHAT IS LEFT IS THE VERB, AND IT IS WORTH ASKING: the shift itself, restated from the serials.
	feathers::FxChain chain = laneOf(3);
	const feathers::DeviceInstanceId second = fxDeviceAtPosition(chain, 1).instanceId();
	const feathers::DeviceInstanceId third = fxDeviceAtPosition(chain, 2).instanceId();

	REQUIRE(feathers::removeFxDevice(&chain, 0));
	REQUIRE(chain.count == 2);

	// THE DEVICES MOVED, IDENTITIES INTACT — from the serials rather than from the tags, so a shift
	// that dropped or duplicated a row cannot pass (all three devices here are filters, and "slot 0
	// holds a filter" is true either way).
	CHECK(fxDeviceAtPosition(chain, 0).instanceId() == second);
	CHECK(fxDeviceAtPosition(chain, 1).instanceId() == third);

	// AND THE VACATED TAIL IS A CLEARED ROW RATHER THAN THE OLD ONE — the guarantee `removeFxDevice`
	// took on when the array became the order. Every reader bounds by `count`, so a stale row past the
	// end cannot resurrect a device; it still NAMES one to anything reading the storage directly, which
	// is the trap the clear exists to avoid. **Asserted on `chain.device[2]` on purpose**:
	// `fxDeviceAtPosition` answers a static empty device past the count, so it would report this as
	// clean whether the tail were cleared or not.
	CHECK(chain.device[2] == feathers::FxDevice {});
	CHECK(chain.device[2].row.instanceId == feathers::kNoInstance);
}

TEST_CASE("a removed device's own routings are dropped rather than landing on its neighbour") {
	// THE SAME CLAIM THIS CASE ALWAYS MADE, ASKED WHERE IT NOW LIVES (kill-lanes step 3c). It drove
	// `feathers::removeFxDevice` against a flat `ModRoutes` and asked what was left at the survivor's
	// columns; the verb takes no table any more, so that reading would assert that an untouched array is
	// untouched. A removal's routes are the record's to drop, so the bridge is the venue and the ends
	// are named rather than numbered.
	//
	// THE FAILURE IT EXISTS TO CATCH IS UNCHANGED AND STILL SILENT: the player deletes one effect and
	// finds a control they never pointed at being driven, because the dead device's amount was carried
	// down onto its neighbour instead of dropped.
	namespace flt = feathers::fx::filter;
	ParamBridge bridge;
	const size_t base = bridge.fxChain().count; // a fresh bridge ships with its default filter
	REQUIRE(bridge.addFxDevice(flt::kTraits.tag) == base);
	REQUIRE(bridge.addFxDevice(flt::kTraits.tag) == base + 1);
	const feathers::DeviceInstanceId doomed = bridge.fxDeviceAt(base);
	const feathers::DeviceInstanceId survivor = bridge.fxDeviceAt(base + 1);
	REQUIRE(doomed != feathers::kNoInstance);
	REQUIRE(survivor != feathers::kNoInstance);
	REQUIRE(doomed != survivor);

	// TWO SOURCES, ONE PER DEVICE, so an amount landing on the wrong device is a different NUMBER and
	// not merely a different address.
	const size_t cutoff = static_cast<size_t>(flt::Param::Cutoff);
	const feathers::backplane::Source macroA = feathers::backplane::Source::core(0);
	const feathers::backplane::Source macroB = feathers::backplane::Source::core(1);
	const feathers::backplane::Destination atDoomed = feathers::backplane::Destination::control(doomed, cutoff);
	const feathers::backplane::Destination atSurvivor =
	    feathers::backplane::Destination::control(survivor, cutoff);
	bridge.setRouteAmount(macroA, atDoomed, 0.75f);
	bridge.setRouteAmount(macroB, atSurvivor, 0.3f);
	REQUIRE(bridge.routeAmount(macroA, atDoomed) == doctest::Approx(0.75f));
	REQUIRE(bridge.routeAmount(macroB, atSurvivor) == doctest::Approx(0.3f));

	REQUIRE(bridge.removeFxDevice(base));
	// THE SURVIVOR REALLY MOVED INTO THE DEAD DEVICE'S PLACE, which is what makes the next two lines
	// about a device rather than about an index nothing disturbed.
	REQUIRE(bridge.fxDeviceAt(base) == survivor);

	// IT KEEPS ITS OWN AMOUNT...
	CHECK(bridge.routeAmount(macroB, atSurvivor) == doctest::Approx(0.3f));
	// ...AND ONLY ITS OWN: the dead device's source must not now reach it.
	CHECK(bridge.routeAmount(macroA, atSurvivor) == doctest::Approx(0.0f));
	// AND THE DEAD DEVICE TOOK ITS EDGES WITH IT — asked of the device rather than of one control,
	// because "nothing of it is left" is the claim and a single cell cannot say that.
	CHECK(bridge.routesOf(doomed).empty());
}

TEST_CASE("reordering a chain rotates the devices, and its inverse puts every one of them back") {
	// A CHAIN IS REORDERABLE BY DESIGN, and the order of an audio chain is its sound.
	//
	// **THE ROUTING HALF OF THIS CASE IS AT THE BRIDGE (kill-lanes step 3c).** It set amounts in a flat
	// `ModRoutes` and asserted that each one was at the coordinate its device had moved to — which was a
	// claim about a lane travelling in a row, and lanes are gone. "A reorder cannot take a route
	// anywhere, because a route names the device" is asserted by endpoint in the MSEG-survives-a-reorder
	// case and again in the six-verb case below; what is left here is the permutation itself.
	feathers::FxChain chain = laneOf(3);
	std::array<feathers::DeviceInstanceId, 3> order {};
	for (size_t slot = 0; slot < 3; ++slot) {
		order[slot] = fxDeviceAtPosition(chain, slot).instanceId();
	}
	REQUIRE(order[0] != order[1]);
	REQUIRE(order[1] != order[2]);

	// The first device to the end: 0,1,2 becomes 1,2,0 — all three change places, so a move that only
	// touched its own two slots is caught by the middle one.
	REQUIRE(feathers::moveFxDevice(&chain, 0, 2));
	CHECK(fxDeviceAtPosition(chain, 0).instanceId() == order[1]);
	CHECK(fxDeviceAtPosition(chain, 1).instanceId() == order[2]);
	CHECK(fxDeviceAtPosition(chain, 2).instanceId() == order[0]);

	SUBCASE("and moving it back restores exactly the chain it started with") {
		// A PERMUTATION AND ITS INVERSE, which is the cheapest way to catch an off-by-one that is
		// symmetric — a bug that shifts by one in both directions passes every single-direction case
		// and fails this.
		REQUIRE(feathers::moveFxDevice(&chain, 2, 0));
		for (size_t slot = 0; slot < 3; ++slot) {
			CAPTURE(slot);
			CHECK(fxDeviceAtPosition(chain, slot).instanceId() == order[slot]);
		}
	}
}

TEST_CASE("a mutation in one chain leaves the other chain's routings exactly where they were") {
	// THE TWO CHAINS SHARE ONE ROUTING RECORD, which is what makes an effect and a modulator the same
	// kind of thing to it — and what makes this worth asserting rather than assuming. A removal that
	// walked the whole record instead of the edges naming one device would drag the rack's routes down
	// behind an fx removal.
	//
	// **AT THE BRIDGE, BY ENDPOINT (kill-lanes step 3c).** It drove `feathers::removeFxDevice` against a
	// flat `ModRoutes` addressed by lane; the verb takes no table, and the coordinates of both bands
	// follow their positions now, so the old reading would have compared an untouched array with itself.
	namespace flt = feathers::fx::filter;
	namespace dsr = feathers::mod::dahdsr;
	ParamBridge bridge;
	const size_t base = bridge.fxChain().count;
	REQUIRE(bridge.addFxDevice(flt::kTraits.tag) == base);
	const size_t env = bridge.addRackDevice(dsr::kTraits.tag);
	REQUIRE(env < feathers::kMaxRackSlots);

	const feathers::DeviceInstanceId doomed = bridge.fxDeviceAt(0); // the default filter, at the front
	const feathers::DeviceInstanceId keptFx = bridge.fxDeviceAt(base);
	const feathers::DeviceInstanceId envId = bridge.rackDeviceAt(env);
	REQUIRE(doomed != feathers::kNoInstance);
	REQUIRE(keptFx != feathers::kNoInstance);
	REQUIRE(envId != feathers::kNoInstance);

	// ONE ROUTE INTO THE RACK DEVICE and one OUT of it, because a modulator is a source as well as a set
	// of destinations — a removal that walked only the destination half would look complete on the fx
	// lane and lose the other direction here.
	const feathers::backplane::Source macro = feathers::backplane::Source::core(0);
	const feathers::backplane::Destination intoEnv =
	    feathers::backplane::Destination::control(envId, static_cast<size_t>(dsr::Param::Attack));
	const feathers::backplane::Source outOfEnv = feathers::backplane::Source::output(envId, 0);
	const feathers::backplane::Destination atKeptFx =
	    feathers::backplane::Destination::control(keptFx, static_cast<size_t>(flt::Param::Cutoff));
	bridge.setRouteAmount(macro, intoEnv, 0.6f);
	bridge.setRouteAmount(outOfEnv, atKeptFx, 0.9f);
	REQUIRE(bridge.routeAmount(macro, intoEnv) == doctest::Approx(0.6f));
	REQUIRE(bridge.routeAmount(outOfEnv, atKeptFx) == doctest::Approx(0.9f));

	// REMOVE THE FX DEVICE THAT NOTHING NAMES, which is what makes this a question about collateral
	// damage rather than about the removal's own edges.
	REQUIRE(bridge.removeFxDevice(0));
	REQUIRE(bridge.fxDeviceAt(0) == keptFx); // the survivor really moved

	// BOTH DIRECTIONS OF THE RACK DEVICE'S ROUTING ARE UNTOUCHED.
	CHECK(bridge.routeAmount(macro, intoEnv) == doctest::Approx(0.6f));
	CHECK(bridge.routeAmount(outOfEnv, atKeptFx) == doctest::Approx(0.9f));
	// AND THE REMOVED DEVICE LEFT NOTHING BEHIND, so "untouched" above is not "the removal did nothing".
	CHECK(bridge.routesOf(doomed).empty());
}

TEST_CASE("a removal does not reissue the serial it freed") {
	// THE ABA RULE ACROSS A DELETION. If nextInstanceId were derived from the count, removing the
	// last device and adding another would hand the newcomer the dead device's id — and any routing
	// that still named it, in a patch or in another session's undo stack, would silently attach.
	feathers::FxChain chain = laneOf(2);
	const feathers::DeviceInstanceId dead = fxDeviceAtPosition(chain, 1).instanceId();

	REQUIRE(feathers::removeFxDevice(&chain, 1));
	REQUIRE(chain.count == 1);

	REQUIRE(feathers::addFxDevice(&chain, feathers::fx::filter::kTraits.tag) == 1);
	CHECK(fxDeviceAtPosition(chain, 1).instanceId() != dead);
	CHECK(fxDeviceAtPosition(chain, 1).instanceId() > dead);
}

TEST_CASE("removing and moving refuse a slot that names no device") {
	feathers::FxChain chain = laneOf(1);
	const feathers::FxChain before = chain;

	// **THE `nullptr` ROUTES GUARD WENT WITH THE ARGUMENT (kill-lanes step 3c).** `removeFxDevice` took a
	// `ModRoutes*` it refused to run without; there is no table to hand it, so there is no null to pass.
	CHECK_FALSE(feathers::removeFxDevice(&chain, 1));
	CHECK_FALSE(feathers::moveFxDevice(&chain, 0, 1));
	CHECK_FALSE(feathers::moveFxDevice(&chain, 1, 0));
	// A REFUSAL COSTS NOTHING, and "nothing" is now the WHOLE CHAIN rather than a table beside it — a
	// guard that returned false after already shifting the rows would be worse than no guard, and since
	// the array IS the order that is a single comparison.
	CHECK(chain.count == 1);
	CHECK(chain == before);

	// AND A MOVE ONTO ITSELF SUCCEEDS AND CHANGES NOTHING, because a drag that ends where it started
	// is a real gesture and refusing it would make the caller special-case it.
	CHECK(feathers::moveFxDevice(&chain, 0, 0));
	CHECK(chain == before);
}


// ---------------------------------------------------------------------------------------------
// WHAT THE LANE WAS FOR — the four claims the design stood or fell on, and what is left of them.
//
// EACH OF THESE HAD ITS MUTATION FAIL. A case that has never been red is a case nobody has shown can
// go red, and three of the four were about something NOT happening — which is the class of claim a
// vacuous test satisfies most easily. **That is exactly why they needed deciding one at a time when
// kill-lanes step 3c deleted the lane, rather than being made to compile.** Each is answered in place
// below: #1 deleted with its mechanism, #2 and #3 moved to the bridge and asked by endpoint, #4
// narrowed to the refusal that is still a guard. The reasoning is kept where the case was.

TEST_CASE("an MSEG routed to an fx device survives a reorder of the fx chain through the bridge, and the MSEG can still be routed to every fx device after it") {
	// THE DEFECT, on f172900: with a looped (free-mode) MSEG mapped to fx controls such as filter cutoff,
	// reordering the audio effects chain lost every route to the audio effects (not to synth params),
	// and new working routes from the MSEG to audio effects could no longer be made.
	//
	// EXACTLY THAT, THROUGH THE BRIDGE'S REAL VERBS: a rack MSEG, a second fx device, a route from the
	// MSEG's output to an fx control, then `moveFxDevice` — the road the lane's drag and its menu end
	// in. Asserted by VALUE on the route table and on the routing rule, for every fx device.
	namespace msg = feathers::mod::mseg;
	namespace flt = feathers::fx::filter;
	namespace hrm = feathers::fx::harm;
	ParamBridge bridge;
	const size_t base = bridge.fxChain().count; // the default filter sits at position 0
	REQUIRE(bridge.addFxDevice(hrm::kTraits.tag) == base);
	const size_t mseg = bridge.addRackDevice(msg::kTraits.tag);
	REQUIRE(mseg < feathers::kMaxRackSlots);
	// **BY IDENTITY THROUGHOUT (kill-lanes step 3b).** This captured each device's LANE and re-read the
	// routes and the rule through it after the reorder. A device's coordinate follows its POSITION now
	// (`chainsOfState` mints the view's lane from the place in the chain) and the record parks devices
	// where it likes, so a lane captured before a move names whatever stands there after it — and
	// re-deriving the coordinate after the move would have compared it to itself and gone green while
	// the route sat on the wrong device. The defect was about ROUTES and DEVICES, never about
	// numbers, so the case now says exactly that and cannot drift again.
	const feathers::DeviceInstanceId msegId = bridge.rackDeviceAt(mseg);
	const feathers::DeviceInstanceId harmId = bridge.fxDeviceAt(base);
	const feathers::DeviceInstanceId filterId = bridge.fxDeviceAt(0);
	REQUIRE(msegId != feathers::kNoInstance);
	REQUIRE(harmId != feathers::kNoInstance);
	REQUIRE(filterId != feathers::kNoInstance);
	REQUIRE(filterId != harmId);

	const feathers::backplane::Source source = feathers::backplane::Source::output(msegId, 0);
	const feathers::backplane::Destination toHarm =
	    feathers::backplane::Destination::control(harmId, static_cast<size_t>(hrm::Param::Width));
	const feathers::backplane::Destination toFilter =
	    feathers::backplane::Destination::control(filterId, static_cast<size_t>(flt::Param::Cutoff));

	// BEFORE THE REORDER: both routes legal, both set. THE RULE IS ASKED OF THE BRIDGE, which is the
	// authority the mod panel and the drag both ask (`ChainRecord::mayRoute`) — the coordinate-space
	// predicate this used took a lane, and a lane is what the case no longer has to give it.
	REQUIRE(bridge.mayRoute(source, toHarm));
	REQUIRE(bridge.mayRoute(source, toFilter));
	bridge.setRouteAmount(source, toHarm, 0.5f);
	bridge.setRouteAmount(source, toFilter, 0.25f);
	REQUIRE(bridge.routeAmount(source, toHarm) == doctest::Approx(0.5f));
	REQUIRE(bridge.routeAmount(source, toFilter) == doctest::Approx(0.25f));

	// THE REORDER, AS THE HAND MAKES IT: harm in front of the filter.
	REQUIRE(bridge.moveFxDevice(base, 0));
	REQUIRE(bridge.fxDeviceAt(0) == harmId);
	REQUIRE(bridge.fxDeviceAt(1) == filterId);

	// THE ROUTES ARE STILL THERE, AND STILL ON THE DEVICES THEY WERE ON — the first half of the
	// defect.
	CHECK(bridge.routeAmount(source, toHarm) == doctest::Approx(0.5f));
	CHECK(bridge.routeAmount(source, toFilter) == doctest::Approx(0.25f));
	// AND A NEW ROUTE FROM THE MSEG TO EVERY FX DEVICE IS STILL ADMITTED — the second half of the
	// defect.
	CHECK(bridge.mayRoute(source, toHarm));
	CHECK(bridge.mayRoute(source, toFilter));
	// AND THE SYNTH'S OWN PARAMS, the half that kept working — a core destination.
	CHECK(bridge.mayRoute(source, feathers::backplane::Destination::core(0)));
	// AND BACK THE OTHER WAY, for the road that moves a device later rather than earlier.
	REQUIRE(bridge.moveFxDevice(0, 1));
	CHECK(bridge.fxDeviceAt(1) == harmId);
	CHECK(bridge.routeAmount(source, toHarm) == doctest::Approx(0.5f));
	CHECK(bridge.mayRoute(source, toHarm));
	CHECK(bridge.mayRoute(source, toFilter));
}

TEST_CASE("the LOAD road: a reordered, routed chain saved and loaded through the real writers and loaders keeps its fx devices RANKED, its routes on the right devices, and the rule admitting every fx destination — and a plain init load into the used instance too") {
	// The last two facts of the defect: the rings were present but did not move, and on a fresh init
	// preset in the same instance there was no glow/amount on audio effects, only on the synth params.
	// A preset is a LOAD. Every green walk so far BUILT its chain; none loaded one.
	// The fx chain's stream carries no addresses at all — devices by id, in order — and everything the
	// glow and the routes are keyed on is rebuilt on the way in, so a save-and-load is where a chain's
	// order, its ranks and its routes can come apart from each other.
	//
	// THE PLUGIN'S OWN ROAD: the bridge's patch through the chains block's real bytes, restored into a
	// bridge exactly as `feathers_clap.cpp`'s `chains` block restores it (`restoreChains`). It was three
	// blocks — fx chain, rack, routes — until chain-as-a-device item 6.
	namespace flt = feathers::fx::filter;
	namespace hrm = feathers::fx::harm;
	namespace msg = feathers::mod::mseg;
	ParamBridge saved;
	REQUIRE(saved.addFxDevice(hrm::kTraits.tag) == 1);
	const size_t msegSlot = saved.addRackDevice(msg::kTraits.tag);
	REQUIRE(msegSlot < feathers::kMaxRackSlots);
	const size_t cutoff = static_cast<size_t>(flt::Param::Cutoff);
	const size_t width = static_cast<size_t>(hrm::Param::Width);
	// **NAMED BEFORE ANYTHING MOVES, AND NAMED BY ID (kill-lanes step 3b).** The routes went on by
	// coordinate and came back by coordinate, and the setup line below used to assert that the saved
	// chain's lanes were NOT its positions — which is what made the save-and-load a real test of the
	// loader's lane-minting. THAT INVARIANT IS GONE, not merely unasserted: the surface numbers devices
	// by position now (`chainsOfState`), so a view's lane IS its position and no reorder can separate
	// them. What made the case worth having is untouched — a chain whose ORDER is not its creation
	// order, saved and loaded, must come back with the same order and the same routes on the same
	// devices — and that is stated by identity, which no numbering can move.
	const feathers::DeviceInstanceId filterId = saved.fxDeviceAt(0);
	const feathers::DeviceInstanceId harmId = saved.fxDeviceAt(1);
	const feathers::DeviceInstanceId msegId = saved.rackDeviceAt(msegSlot);
	REQUIRE(filterId != feathers::kNoInstance);
	REQUIRE(harmId != feathers::kNoInstance);
	REQUIRE(msegId != feathers::kNoInstance);
	const feathers::backplane::Source msegSource = feathers::backplane::Source::output(msegId, 0);
	const feathers::backplane::Destination toCutoff = feathers::backplane::Destination::control(filterId, cutoff);
	const feathers::backplane::Destination toWidth = feathers::backplane::Destination::control(harmId, width);
	saved.setRouteAmount(msegSource, toCutoff, 0.25f);
	saved.setRouteAmount(msegSource, toWidth, 0.5f);
	REQUIRE(saved.routeAmount(msegSource, toCutoff) == doctest::Approx(0.25f));
	REQUIRE(saved.routeAmount(msegSource, toWidth) == doctest::Approx(0.5f));
	// THE REORDER, so the saved chain's ORDER is not the order its devices were added in — which is the
	// thing a save-and-load has to carry and the reason this case loads rather than builds.
	REQUIRE(saved.moveFxDevice(0, 1));
	REQUIRE(saved.fxDeviceAt(0) == harmId);
	REQUIRE(saved.fxDeviceAt(1) == filterId);

	const auto saveAll = [](const ParamBridge& b, MemoryStream& stream) {
		clap_ostream out = stream.out();
		REQUIRE(feathers::saveChainsBlock(&out, b.chainsPatch()));
	};
	const auto loadAll = [](ParamBridge& b, MemoryStream& stream) {
		bool present = false;
		clap_istream in = stream.in();
		feathers::rack::RackPatch patch;
		REQUIRE(feathers::loadChainsBlock(&in, &patch, &present));
		REQUIRE(present);
		return b.restoreChains(patch).routes;
	};
	// **EVERY FX DEVICE REACHABLE FROM `source` — the claim the rank check was a proxy for (kill-lanes
	// step 5).**
	//
	// It was "every fx lane RANKED", read off `DeviceSlots::order`, plus
	// `retiredBandRule::maySourceRouteTo` over a destination coordinate derived from the lane. The rank
	// and the rule were one fact: an unranked lane is a lane the band rule refuses, so the mask zeroes
	// and the rule were one fact: an unranked lane is a lane the band rule refuses, so the mask zeroes
	// every source for it and the glow goes out. **The band rule is gone (step 4b)**, so asserting it here
	// would be asserting the old rule — the exact hollow shape `device_registry.h` names. What a hand
	// notices is the ROUTE being refused, and that is asked here of the authority, on every fx device, by
	// name.
	//
	// **THE RANK ARRAY ITSELF IS NOT DEAD, and this sentence used to imply it was.** It said the array was
	// "read by nothing in `src/` but the retired fixture", which was true until step 5 deleted that
	// fixture and would now read as "read by nothing at all". It is not: `slotsOf` still fills
	// `DeviceSlots::order`, and the backlog-CV case in this file guards the hazard that made the sentinel
	// its own value — a rank colliding with the "no order" mark, which would make an fx device at position
	// 6 or 7 silently unrankable. The array goes with the coordinate space when the modulation bus stops
	// being indexed by one, not before.
	const auto fxReachableFrom = [&](const ParamBridge& b, backplane::Source source) {
		REQUIRE(b.fxChain().count > 0); // or the loop below asserts nothing at all
		for (size_t position = 0; position < b.fxChain().count; ++position) {
			CAPTURE(position);
			const feathers::DeviceInstanceId id = b.fxDeviceAt(position);
			REQUIRE(id != feathers::kNoInstance);
			const feathers::device::Descriptor* desc = b.descriptorOf(id);
			REQUIRE(desc != nullptr);
			REQUIRE(desc->modulatableCount() > 0);
			CHECK(b.mayRoute(source,
			                 backplane::Destination::control(id, desc->controlOfNthModulatableBesideEnable(0))));
		}
	};

	SUBCASE("the reordered patch, loaded into a fresh instance") {
		MemoryStream stream;
		saveAll(saved, stream);
		ParamBridge loaded;
		const feathers::RouteRefusals refused = loadAll(loaded, stream);
		CHECK(refused.count == 0);
		REQUIRE(loaded.fxChain().count == 2);
		// THE ORDER IS THE SAVED ORDER: harm first, filter second — by instance.
		CHECK(feathers::instanceOfFxSlot(loaded.fxChain(), 0) == harmId);
		CHECK(feathers::instanceOfFxSlot(loaded.fxChain(), 1) == filterId);
		// THE MSEG CAME BACK UNDER ITS OWN ID, which is what lets the routes below be named rather than
		// looked up — and is itself the claim that a load preserves identity.
		CHECK(loaded.rackDeviceAt(msegSlot) == msegId);
		fxReachableFrom(loaded, backplane::Source::output(loaded.rackDeviceAt(msegSlot), 0));
		// THE ROUTES ARE ON THE RIGHT DEVICES — asked by NAME, so "the right devices" is the assertion
		// rather than a coordinate that would name whatever the load happened to put at that position.
		CHECK(loaded.routeAmount(msegSource, toCutoff) == doctest::Approx(0.25f));
		CHECK(loaded.routeAmount(msegSource, toWidth) == doctest::Approx(0.5f));
	}
	SUBCASE("a plain init state, loaded into the instance that had been through the reorder") {
		MemoryStream stream;
		ParamBridge init;
		saveAll(init, stream);
		const feathers::RouteRefusals refused = loadAll(saved, stream);
		CHECK(refused.count == 0);
		REQUIRE(saved.fxChain().count == 1);
		// THE DEFAULT FILTER TAKES A MACRO AND THE DEFAULT DAHDSR — synth params were fine in the defect;
		// the fx destination is the one that lost its glow.
		fxReachableFrom(saved, backplane::Source::core(0));
		fxReachableFrom(saved, backplane::Source::output(saved.rackDeviceAt(0), 0));
	}
}

// **REQUIRED ASSERTION #1 IS DELETED, AND IT IS THE ONE THE MECHANISM TOOK WITH IT.** It was "a reorder
// changes no route, asserted on the table": not one cell of the flat matrix differed across a reorder,
// because a lane lived in the row that moved. Its own note recorded that the TABLE half stayed green
// under the mutation it was aimed at — the value half was the whole test — and the value half is now
// FALSE by design: a device's coordinate is its position, so a reorder renumbers every device that
// moved and nothing follows them in a flat table.
//
// WHAT REPLACES IT, so this is not a hole: "a reorder cannot take a route anywhere, because a route
// names the device" — asserted by endpoint through the bridge's real verb in the MSEG-survives-a-reorder
// case above and again in the six-verb case below, and the permutation itself in "reordering a chain rotates
// the devices". The claim survived; only the coordinate it was spelled in did not.

TEST_CASE("a removed rack device comes back on undo carrying the same routes, in both directions") {
	// REQUIRED ASSERTION #2, ON THE RACK — because a rack device is a SOURCE as well as a set of
	// destinations, so this is the only chain on which "the same routes" means both directions. The
	// fx lane's devices declare no outputs, so a road that lost the source half would look complete
	// there and be wrong here. **That finding is why this case is still about the RACK, and it is the
	// half of it that survives kill-lanes step 3c untouched.**
	//
	// **THE SETUP IT NEEDED IS GONE WITH THE ALLOCATOR.** It removed a device FIRST, whose only job was
	// to leave a lower lane free, because the pool handed out the lowest free lane — so an undo that
	// ignored the recorded lane and allocated a fresh one would otherwise have been indistinguishable
	// from a correct one. There is no pool and no lane: the device comes back under its own instance id,
	// which nothing can reissue, so there is no wrong answer for the undo to accidentally agree with.
	// The old KNOWN-BAD ("make insertRackDevice allocate rather than honour `row.routeLane`") cannot be
	// written any more.
	//
	// **AND THE ROUTES ARE NO LONGER CARRIED AS A SLICE OF A TABLE.** `insertRackDevice` took a
	// `DeviceRouteSlice` lifted before the removal, because a removal CLEARED the columns and they were
	// not recoverable after. The bridge's undo asks the record for `routesOf(id)` and hands those back;
	// so this drives the bridge, which is where that road is.
	namespace dsr = feathers::mod::dahdsr;
	ParamBridge bridge;
	const size_t slot = bridge.addRackDevice(dsr::kTraits.tag);
	REQUIRE(slot < feathers::kMaxRackSlots);
	const feathers::DeviceInstanceId id = bridge.rackDeviceAt(slot);
	REQUIRE(id != feathers::kNoInstance);

	const int fine = feathers::modDestinationIndexOf(feathers::ParamId::FineTune);
	REQUIRE(fine >= 0);
	const feathers::backplane::Source macro = feathers::backplane::Source::core(0);
	const feathers::backplane::Destination intoAttack =
	    feathers::backplane::Destination::control(id, static_cast<size_t>(dsr::Param::Attack));
	const feathers::backplane::Source outOf = feathers::backplane::Source::output(id, 0);
	const feathers::backplane::Destination toFine =
	    feathers::backplane::Destination::core(static_cast<size_t>(fine));
	bridge.setRouteAmount(macro, intoAttack, 0.4f);  // a macro INTO its attack
	bridge.setRouteAmount(outOf, toFine, -0.3f);     // and its envelope OUT to a parameter
	REQUIRE(bridge.routeAmount(macro, intoAttack) == doctest::Approx(0.4f));
	REQUIRE(bridge.routeAmount(outOf, toFine) == doctest::Approx(-0.3f));

	// WHAT AN UNDO KEEPS: the row, and every edge naming this device AT EITHER END. The count is
	// asserted because "both directions" is the claim this case exists for, and a `routesOf` that
	// walked only destinations would hand back one.
	const feathers::DeviceRow row = bridge.modRack().device[slot];
	const std::vector<feathers::backplane::Route> kept = bridge.routesOf(id);
	REQUIRE(kept.size() == 2);

	REQUIRE(bridge.removeRackDevice(slot));
	// THE REMOVAL REALLY TOOK THEM, or the restore below proves nothing.
	REQUIRE(bridge.routeAmount(macro, intoAttack) == doctest::Approx(0.0f));
	REQUIRE(bridge.routeAmount(outOf, toFine) == doctest::Approx(0.0f));

	REQUIRE(bridge.insertRackDevice(slot, row, kept));

	// THE SAME DEVICE, said before the amounts are checked — this is the line that names what went
	// wrong rather than merely that something did.
	CHECK(bridge.rackDeviceAt(slot) == id);
	CHECK(bridge.routeAmount(macro, intoAttack) == doctest::Approx(0.4f));
	CHECK(bridge.routeAmount(outOf, toFine) == doctest::Approx(-0.3f));
	// AND NOTHING ELSE CAME BACK WITH THEM — two routes went in, two came back, so an undo that
	// duplicated an edge or restored one nobody named is caught here rather than by inspection.
	CHECK(bridge.routesOf(id).size() == 2);
}

TEST_CASE("a device added after a removal arrives unrouted — the ABA hazard the lane introduced") {
	// REQUIRED ASSERTION #3, AND THE HAZARD IS NOW UNWRITEABLE RATHER THAN MERELY GUARDED. It read "a
	// lane reissued to a new device carries none of the old device's routes": a lane was an address in
	// a fixed pool, a fixed pool exists precisely so that the thing at an address can be replaced, and
	// an address handed out again with the old device's amounts still in it is a device that arrives
	// pre-modulated by routes no card drew. **`kill-lanes` step 3c deletes the pool and the address.** A
	// route names an instance id, `nextInstanceId` only ever climbs, and nothing is ever handed out
	// twice — so the old KNOWN-BAD (delete the `clearDeviceSlotRoutes` call in `removeRackDevice`) names
	// a call that no longer exists.
	//
	// **THE CLAIM IS KEPT ANYWAY, BECAUSE IT IS THE ONE A PLAYER FEELS**, and "impossible by
	// construction" is a thing to assert rather than a reason not to: the device you just added is not
	// modulated by anything. Asked of the RECORD through the bridge, which is where a removal's edges
	// are now dropped — and on the rack again for the reason the case above gives, since this is where a
	// drop that walked only the destination half would show.
	namespace dsr = feathers::mod::dahdsr;
	ParamBridge bridge;
	const size_t slot = bridge.addRackDevice(dsr::kTraits.tag);
	REQUIRE(slot < feathers::kMaxRackSlots);
	const feathers::DeviceInstanceId doomed = bridge.rackDeviceAt(slot);
	REQUIRE(doomed != feathers::kNoInstance);

	const int fine = feathers::modDestinationIndexOf(feathers::ParamId::FineTune);
	REQUIRE(fine >= 0);
	const feathers::backplane::Source macro = feathers::backplane::Source::core(0);
	const feathers::backplane::Destination intoAttack =
	    feathers::backplane::Destination::control(doomed, static_cast<size_t>(dsr::Param::Attack));
	const feathers::backplane::Source outOf = feathers::backplane::Source::output(doomed, 0);
	const feathers::backplane::Destination toFine =
	    feathers::backplane::Destination::core(static_cast<size_t>(fine));
	bridge.setRouteAmount(macro, intoAttack, 0.4f);
	bridge.setRouteAmount(outOf, toFine, -0.3f);
	REQUIRE(bridge.routesOf(doomed).size() == 2); // the setup really routed something, both ways

	REQUIRE(bridge.removeRackDevice(slot));
	const size_t took = bridge.addRackDevice(dsr::kTraits.tag);
	REQUIRE(took < feathers::kMaxRackSlots);
	const feathers::DeviceInstanceId fresh = bridge.rackDeviceAt(took);
	REQUIRE(fresh != feathers::kNoInstance);

	// THE NEWCOMER TOOK THE VACATED PLACE, which is what made this the ABA shape: same slot, same kind,
	// nothing about it visibly different from the device that was there.
	REQUIRE(took == slot);
	// AND IT IS A DIFFERENT DEVICE. The place is the same; the serial must not be.
	CHECK(fresh != doomed);

	// NOTHING REACHES IT AND NOTHING LEAVES IT — asked of the device rather than of the two cells this
	// case wrote, because a drop that missed a corner would still pass the narrow version and the
	// corner it would miss is the source half.
	CHECK(bridge.routesOf(fresh).empty());
	// AND THE DEAD DEVICE'S OWN EDGES ARE GONE ALTOGETHER, which is the honest end state: both routes
	// named a device that is not here, and a dropped route is the answer instance ids were chosen for.
	CHECK(bridge.routesOf(doomed).empty());
	CHECK(bridge.routeAmount(macro, intoAttack) == doctest::Approx(0.0f));
	CHECK(bridge.routeAmount(outOf, toFine) == doctest::Approx(0.0f));
}

TEST_CASE("a chain at its starting size grows for the next device, in both chains") {
	// IT REFUSED HERE until unbounded devices (step 6): the two guards were separate functions, so the growth
	// is asserted in each — sixty-four devices past where either used to stop, every one under its own serial.
	SUBCASE("the fx lane") {
		feathers::FxChain chain;
		for (size_t i = 0; i < 64; ++i) {
			CAPTURE(i);
			REQUIRE(feathers::addFxDevice(&chain, feathers::fx::filter::kTraits.tag) == i);
		}
		CHECK(chain.count == 64);
		for (size_t i = 1; i < 64; ++i) {
			CHECK(fxDeviceAtPosition(chain, i).instanceId() == fxDeviceAtPosition(chain, i - 1).instanceId() + 1);
		}
	}

	SUBCASE("the modulator rack") {
		feathers::ModRack rack;
		rack.count = 0;
		for (size_t i = 0; i < 64; ++i) {
			CAPTURE(i);
			REQUIRE(feathers::addRackDevice(&rack, mod::dahdsr::kTraits.tag) == i);
		}
		CHECK(rack.count == 64);
		for (size_t i = 1; i < 64; ++i) {
			CHECK(rack.device[i].instanceId == rack.device[i - 1].instanceId + 1);
		}
	}
}

TEST_CASE("the editor can add a device, and the channel carries it") {
	// THE (+)'s ROAD, ASKED OF THE CHANNEL RATHER THAN OF THE FREE FUNCTION. fx_chain.h's addFxDevice
	// is already covered where it lives; what is unproven until here is that the result reaches the
	// atomics the audio thread reads, in the order that table requires, and that a hand doing it
	// marks the patch modified.
	namespace flt = feathers::fx::filter;
	ParamBridge bridge;

	SUBCASE("a modulator lands in the rack and reads back as itself") {
		const size_t before = bridge.modRack().count;
		bridge.takeStateDirty();

		const size_t slot = bridge.addRackDevice(mod::mseg::kTraits.tag);
		REQUIRE(slot == before);

		const feathers::ModRack rack = bridge.modRack();
		CHECK(rack.count == before + 1);
		// THE VALUE, NOT THE COUNT. A slot that was counted but never written is exactly the failure a
		// count-only assertion cannot see, and the ordering this channel is built on is what makes it
		// possible: the count is stored last precisely so it can never be ahead of the row.
		CHECK(rack.holds(slot, mod::mseg::kTraits.tag));
		CHECK_FALSE(rack.holds(slot, mod::dahdsr::kTraits.tag));

		// AND ITS SETTINGS ARE THE DEVICE'S DECLARED DEFAULTS, which is what defaultRowFor promises
		// and what makes a newly added device usable rather than a row of zeros. Restated from the
		// descriptor rather than read back through the same road.
		const mod::mseg::Settings fresh = rack.msegAt(slot);
		const auto& descs = mod::mseg::kParamDescs;
		CHECK(fresh.timeScale
		      == doctest::Approx(descs[static_cast<size_t>(mod::mseg::Param::TimeScale)].def));
		CHECK(fresh.count == static_cast<size_t>(descs[static_cast<size_t>(mod::mseg::Param::SegmentCount)].def));

		// A HAND DID IT, so the patch is modified. The restore road deliberately does not do this and
		// the difference is the whole reason there are two roads.
		CHECK(bridge.takeStateDirty());
	}

	SUBCASE("an effect lands in the lane, with an identity of its own") {
		const size_t before = bridge.fxChain().count;
		const size_t slot = bridge.addFxDevice(flt::kTraits.tag);
		REQUIRE(slot == before);

		const feathers::FxChain chain = bridge.fxChain();
		CHECK(chain.count == before + 1);
		CHECK(device::sameTag(fxDeviceAtPosition(chain, slot).tag().data(), flt::kTraits.tag));

		// A SERIAL THAT NO OTHER SLOT HOLDS, which is device_instance.h's rule and not decoration: a
		// routing names its device by identity, so two live devices sharing one would silently point
		// one device's routes at the other.
		std::set<feathers::DeviceInstanceId> seen;
		for (size_t i = 0; i < chain.count; ++i) {
			CHECK(seen.insert(fxDeviceAtPosition(chain, i).row.instanceId).second);
		}
		CHECK(chain.nextInstanceId > fxDeviceAtPosition(chain, slot).row.instanceId);
	}

	SUBCASE("and the device's own declarations decide, not the lane it is offered to") {
		// THE REFUSING DEVICE USED TO BE HARM AND IS NOW THE MSEG, and the swap is the point rather
		// than a repair. HARM declared no audio entry point while its DSP was unwritten, so the lane
		// refused it; 5386d51 gave it a real AudioOps and the lane now accepts it. That is the
		// feature landing, and this case fired to say so — which is exactly what f7 designed the
		// deliberately-red case for. It is INVERTED here rather than deleted, in both halves:
		// HARM's acceptance is asserted below, and the claim this subcase actually makes — that the
		// DEVICE's declarations decide, not the lane it is offered to — needs a device that still
		// cannot render. An MSEG is one: it publishes a modulation output and no audio entry point.
		//
		// **AND THE MSEG IS ACCEPTED NOW TOO — INVERTED AGAIN, 2026-10-04.** Every mod device
		// is in scope in the Feathers fx rack. The claim stands — the device's declarations decide (`maySitInLane`:
		// audio, mod or notes) — and every device this build has now answers yes to the lane, so the refusal
		// left to assert is a tag nobody declares, below. The MSEG's acceptance is asserted so a later change
		// that narrowed the lane again would say so here.
		const size_t fxBefore = bridge.fxChain().count;
		bridge.takeStateDirty();
		const size_t msegAt = bridge.addFxDevice(mod::mseg::kTraits.tag);
		CHECK(msegAt == fxBefore);
		CHECK(bridge.fxChain().count == fxBefore + 1);
		CHECK(bridge.takeStateDirty());
		CHECK(bridge.removeFxDevice(msegAt));
		bridge.takeStateDirty();
		// A REFUSAL IS NOT AN EDIT. Nothing changed, so nothing may claim the patch is modified —
		// otherwise a player who opened a picker and closed it would be told they had unsaved work.
		CHECK(bridge.addFxDevice("ZZZZ") == feathers::kNoPosition);
		CHECK(bridge.fxChain().count == fxBefore);
		CHECK_FALSE(bridge.takeStateDirty());

		// AND HARM IS ACCEPTED NOW, asserted here so the two facts cannot drift apart: if a later
		// change took HARM's ops away, this line goes red rather than the refusal above quietly
		// becoming true again for the wrong reason.
		const size_t harmSlot = bridge.addFxDevice(fx::harm::kTraits.tag);
		CHECK(harmSlot < feathers::kMaxFxSlots);
		CHECK(feathers::fxHolds(bridge.fxChain(), harmSlot, fx::harm::kTraits.tag));
		CHECK(bridge.takeStateDirty());

		// And the same refusal the other way round: a filter publishes no modulation signal, so the
		// rack will not take it.
		const size_t rackBefore = bridge.modRack().count;
		CHECK(bridge.addRackDevice(flt::kTraits.tag) == feathers::kNoPosition);
		CHECK(bridge.modRack().count == rackBefore);

		// AND A TAG THIS BUILD HAS NEVER HEARD OF, which is what a patch from a later version looks
		// like arriving at an older one.
		CHECK(bridge.addRackDevice("ZZZZ") == feathers::kNoPosition);
		CHECK(bridge.addFxDevice(nullptr) == feathers::kNoPosition);
	}

	SUBCASE("a rack past its starting size grows rather than overwriting its last slot") {
		// IT REFUSED AT `kMaxRackSlots` until unbounded devices (step 6). The failure it guarded against is
		// the one that still matters, and it is silent: an append that clamped to the last slot would
		// REPLACE a device a hand had already placed, and the count would look right afterwards. So: sixty-
		// four adds, each landing at the end, and every device still there under its own id.
		const size_t start = bridge.modRack().count;
		for (size_t i = 0; i < 64; ++i) {
			CAPTURE(i);
			REQUIRE(bridge.addRackDevice(mod::mseg::kTraits.tag) == start + i);
		}
		CHECK(bridge.modRack().count == start + 64);

		// Every slot still holds a device, so nothing was overwritten on the way to full.
		const feathers::ModRack rack = bridge.modRack();
		for (size_t i = 0; i < rack.count; ++i) {
			CAPTURE(i);
			CHECK(device::sameTag(rack.device[i].tag.data(), rack.device[i].tag.data()));
			CHECK(rack.device[i].instanceId != feathers::kNoInstance);
		}
	}
}

//--------------------------------------------------------------------------------------------//
// THE BROWSER, END TO END                                                                     //
//--------------------------------------------------------------------------------------------//

TEST_CASE("a chain built, edited, routed, reordered, trimmed and reloaded still says what it is") {
	// SIX VERBS LANDED IN ONE DAY AND EVERY CASE DRIVES ONE OF THEM. addDevice, remove, reorder, the
	// live routing remap, the lane's scroll, and the two (+) menus are each well covered in
	// isolation; nothing drives a SEQUENCE, and nothing carries a patch across a save. This is the
	// coverage that arc does not have, and the failures it is aimed at are the ones that need two
	// verbs to appear: a remap that is right for a move and wrong for a move-after-a-remove, an
	// identity that survives reorder in memory and not on disk.
	//
	// EVERY DEVICE IS NAMED BY SERIAL, NEVER BY SLOT. A slot index is exactly what reorder and remove
	// invalidate — a venue that named things by position would pass while testing nothing, because
	// "slot 1 holds a filter" stays true through a swap of two filters. device_instance.h's whole
	// subject is that the serial is the only identity that survives, and this case is written as if
	// that were true and would fail loudly if it stopped being.
	//
	// AND THE TWO FILTERS ARE MADE DIFFERENT. A chain of filter/HARM/HARM cannot see a swap of its
	// last two, so the chain here is filter/HARM/filter and the two filters carry DIFFERENT cutoffs.
	// Serial tells them apart as identity; the cutoff tells them apart as payload, and a reorder that
	// moved the serial without the settings — or the settings without the serial — fails one or the
	// other.
	namespace flt = feathers::fx::filter;
	namespace hrm = feathers::fx::harm;
	namespace mseg = feathers::mod::mseg;

	feathers::ParamBridge bridge;
	bridge.restoreFxChain(feathers::defaultFxChain());

	//-- 1. ADD ------------------------------------------------------------------------------//
	// The lane starts with the filter a fresh instance has. Two more devices arrive by the road the
	// (+) menu takes — `addFxDevice(tag)` — rather than by a chain assembled in the fixture, because
	// a fixture-built chain proves nothing about the verb that builds one.
	REQUIRE(bridge.fxChain().count == 1);
	REQUIRE(bridge.addFxDevice(hrm::kTraits.tag) == 1);
	REQUIRE(bridge.addFxDevice(flt::kTraits.tag) == 2);

	const feathers::FxChain built = bridge.fxChain();
	REQUIRE(built.count == 3);
	const feathers::DeviceInstanceId filterA = feathers::instanceOfFxSlot(built, 0);
	const feathers::DeviceInstanceId harm = feathers::instanceOfFxSlot(built, 1);
	const feathers::DeviceInstanceId filterC = feathers::instanceOfFxSlot(built, 2);
	// THREE DISTINCT SERIALS, stated because two of the three devices are the same KIND and a serial
	// that came from the kind rather than from the lane's counter would collide here and nowhere else.
	CHECK(filterA != harm);
	CHECK(harm != filterC);
	CHECK(filterA != filterC);

	// A modulator to route FROM. The rack's own (+) road, beside the lane's.
	REQUIRE(bridge.modRack().count >= 1);
	const size_t msegSlot = bridge.addRackDevice(mseg::kTraits.tag);
	REQUIRE(msegSlot < feathers::kMaxRackSlots);
	const feathers::DeviceInstanceId msegId = feathers::instanceOfSlot(bridge.modRack(), msegSlot);

	//-- 2. EDIT SOME CONTROLS ---------------------------------------------------------------//
	// Numbers a later step can recognise, and deliberately different between the two filters.
	//
	// TAKEN FROM THE CONTROL'S OWN DECLARATION, and the first draft was not. Cutoff is stored as
	// log2(Hz) — 4.32 to 14.29 — so a fixture that reached for 0.2 and 0.8 because they "look like a
	// knob" had both writes COERCED to the range floor by setFxDeviceControl, which is doing exactly
	// its job. Both filters then held the identical 4.32193 and the two of them stopped being
	// distinguishable at all.
	//
	// It is CLAUDE.md's fixture rule arriving on its own case: *take values from the platform's
	// contract, never from what our code expects.* Worth recording rather than quietly fixing,
	// because the case CAUGHT it — the "make the two neighbours different" discipline is what turned
	// a silently-clamped fixture into six red assertions instead of a venue that passed while
	// comparing a device against itself.
	const float kCutoffA = static_cast<float>(std::log2(200.0));  // 7.64, well inside the range
	const float kCutoffC = static_cast<float>(std::log2(5000.0)); // 12.29, and far from it
	constexpr float kHarmLevel = 0.55f;
	// NEITHER IS THE DEFAULT, or an edit that silently did nothing would read as an edit that worked.
	REQUIRE(kCutoffA != doctest::Approx(static_cast<float>(flt::kCutoffDefaultLog2)));
	REQUIRE(kCutoffC != doctest::Approx(static_cast<float>(flt::kCutoffDefaultLog2)));
	REQUIRE(kHarmLevel != doctest::Approx(0.0f)); // HARM's Level defaults to silence
	const auto cutoffIndex = static_cast<size_t>(flt::Param::Cutoff);
	const auto harmLevelIndex = static_cast<size_t>(hrm::Param::Level);
	// WRITTEN AT THE SERIAL, not at a slot — the discipline this case's banner states, applied to its
	// own setup. These went in at lanes 0, 1 and 2, which named the right devices only because nothing
	// had moved yet; a case whose subject is that a serial survives six verbs should not open by
	// addressing its devices with the thing those verbs invalidate.
	bridge.setDeviceControl(filterA, cutoffIndex, kCutoffA);
	bridge.setDeviceControl(harm, harmLevelIndex, kHarmLevel);
	bridge.setDeviceControl(filterC, cutoffIndex, kCutoffC);

	/// ONE CONTROL OF WHICHEVER SLOT NOW HOLDS A SERIAL. Every assertion below reads through this, so
	/// no assertion in this case can be satisfied by a device that merely happens to sit somewhere.
	const auto controlOfInstance = [](const feathers::FxChain& chain,
	                                  feathers::DeviceInstanceId id, size_t control) {
		const size_t slot = feathers::slotOfFxInstance(chain, id);
		REQUIRE(slot < chain.count);
		// THE POSITION IS THE SUBSCRIPT (kill-lanes step 3c) — a chain IS its order, so the crossing
		// through `fxLaneAtPosition` that stood here has nothing left to cross.
		return feathers::fxDeviceAtPosition(chain, slot).row.field[control];
	};

	{
		const feathers::FxChain edited = bridge.fxChain();
		CHECK(controlOfInstance(edited, filterA, cutoffIndex) == doctest::Approx(kCutoffA));
		CHECK(controlOfInstance(edited, filterC, cutoffIndex) == doctest::Approx(kCutoffC));
		CHECK(controlOfInstance(edited, harm, harmLevelIndex) == doctest::Approx(kHarmLevel));
	}

	//-- 3. ROUTE THE MODULATOR AT ONE OF THEM -----------------------------------------------//
	// THE MSEG'S OUTPUT INTO HARM'S LEVEL, AND THE CHOICE OF TARGET IS THE CASE'S OWN KNOWN-BAD
	// TALKING. The first draft routed at filter C, the LAST device — which after the reorder below
	// sits at slot 0, BELOW everything. Removing a device above it then shifts nothing, so deleting
	// the remap inside `removeFxDevice` left this whole case GREEN: the probe was aimed where the
	// right answer and the missing-remap answer agree.
	//
	// HARM is the MIDDLE device, so it MOVES under the reorder (slot 1 to slot 2) and MOVES AGAIN
	// under the removal below it (slot 2 to slot 1). Both remaps are load-bearing now, and both
	// mutations turn this red.
	constexpr float kAmount = 0.42f;
	// **NAMED BY ENDPOINT, BECAUSE EVERY VERB BELOW MOVES A COORDINATE (kill-lanes step 3b).** The route
	// went on at a `(source, destination)` pair recomputed from HARM's position at each step, and the
	// case then asserted things ABOUT THAT NUMBER. A device's coordinate follows its position now, so
	// those numbers change at every reorder and removal — and, worse, a number recomputed after a verb
	// and compared with one recomputed after the next verb is a comparison of two lookups rather than a
	// claim about a route. This case's subject is that a SERIAL survives six verbs; its routing half now
	// says so in the same vocabulary as its settings half.
	const feathers::backplane::Source source = feathers::backplane::Source::output(msegId, 0);
	const feathers::backplane::Destination atHarmLevel =
	    feathers::backplane::Destination::control(harm, harmLevelIndex);
	REQUIRE(bridge.mayRoute(source, atHarmLevel));
	bridge.setRouteAmount(source, atHarmLevel, kAmount);
	REQUIRE(bridge.routeAmount(source, atHarmLevel) == doctest::Approx(kAmount));

	//-- 4. REORDER ---------------------------------------------------------------------------//
	// The last device to the front, which moves all three: filter C to slot 0, and the other two down
	// one. A move that only touched its own two slots would leave HARM where it was and pass a case
	// that checked only the moved device.
	REQUIRE(bridge.moveFxDevice(2, 0));
	{
		const feathers::FxChain moved = bridge.fxChain();
		REQUIRE(moved.count == 3);
		// THE ORDER, BY SERIAL.
		CHECK(feathers::instanceOfFxSlot(moved, 0) == filterC);
		CHECK(feathers::instanceOfFxSlot(moved, 1) == filterA);
		CHECK(feathers::instanceOfFxSlot(moved, 2) == harm);
		// AND THE PAYLOAD TRAVELLED WITH THE SERIAL. This is the half that filter/HARM/HARM could not
		// have asked: slot 0 and slot 1 are both filters now, so "slot 0 holds a filter" is true
		// either way and only the cutoff says which one it is.
		CHECK(controlOfInstance(moved, filterC, cutoffIndex) == doctest::Approx(kCutoffC));
		CHECK(controlOfInstance(moved, filterA, cutoffIndex) == doctest::Approx(kCutoffA));
		CHECK(controlOfInstance(moved, harm, harmLevelIndex) == doctest::Approx(kHarmLevel));
	}

	// **THE LIVE ROUTING FOLLOWED THE DEVICE, AND THAT IS THE WHOLE CLAIM.** The route names HARM, so a
	// reorder cannot take it anywhere: after all three devices changed places the amount is still on
	// HARM's Level, asked for by name.
	//
	// THIS PARAGRAPH HAS NOW ARGUED BOTH WAYS ABOUT AN ADDRESS AND IS DONE WITH ADDRESSES. It first
	// asserted the destination index CHANGED (positional addressing: a correct move had to renumber);
	// then that it did NOT change (a lane lived in the device's row and travelled with it); and under
	// kill-lanes step 3b it would have to change again, because a device's coordinate is its position
	// once more. Three readings of one line, none of them the thing anybody cares about — which is that
	// the modulator still reaches the device it was aimed at.
	CHECK(bridge.routeAmount(source, atHarmLevel) == doctest::Approx(kAmount));
	// NO SMEAR. A remap that wrote the amount somewhere broader would satisfy the line above and be a
	// route from things nobody touched, so nothing but the MSEG may reach HARM's Level. Asked of the
	// bridge's own predicate, which is what the control's "routed elsewhere" mark reads.
	//
	// THE FIRST DRAFT OF THE LOOP THIS REPLACES PASSED A DESTINATION INDEX AS THE SOURCE ARGUMENT. It
	// asserted that some unrelated source has nothing routed to destination 0 — true throughout,
	// checked nothing, and read like a real guard. Found by re-reading rather than by any failure,
	// which is the only way an assertion that is merely vacuous ever is; naming both ends removes the
	// argument order it got wrong.
	CHECK_FALSE(bridge.routedElsewhere(atHarmLevel, source));
	// AND NEITHER DEVICE THAT MOVED PAST HARM ACQUIRED A ROUTE. The old check compared their LANES with
	// HARM's, which two devices at two positions can no longer share — a tautology since step 3b. What
	// it was reaching for is that the reorder created nothing, and that is asked of the devices.
	for (const feathers::DeviceInstanceId other : { filterA, filterC }) {
		CAPTURE(other);
		CHECK(bridge.routesOf(other).empty());
	}

	//-- 5. REMOVE A DIFFERENT DEVICE ---------------------------------------------------------//
	// Filter A, which nothing is routed at — so this step's claim is that removing an UNRELATED
	// device disturbs neither the survivors' settings nor the routing that was already correct.
	{
		const size_t slotOfA = feathers::slotOfFxInstance(bridge.fxChain(), filterA);
		REQUIRE(slotOfA == 1);
		REQUIRE(bridge.removeFxDevice(slotOfA));
	}
	{
		const feathers::FxChain trimmed = bridge.fxChain();
		REQUIRE(trimmed.count == 2);
		CHECK(feathers::instanceOfFxSlot(trimmed, 0) == filterC);
		CHECK(feathers::instanceOfFxSlot(trimmed, 1) == harm);
		// GONE, AND SAID SO AS A LOOKUP RATHER THAN AS A COUNT. A count of two is also what a chain
		// that dropped the wrong device has.
		CHECK(feathers::slotOfFxInstance(trimmed, filterA) >= trimmed.count);
		CHECK(controlOfInstance(trimmed, filterC, cutoffIndex) == doctest::Approx(kCutoffC));
		CHECK(controlOfInstance(trimmed, harm, harmLevelIndex) == doctest::Approx(kHarmLevel));
	}
	// AND THE ROUTING STILL REACHES HARM, WHICH IS THE CLAIM AN UNRELATED REMOVAL HAS TO MEET. HARM was
	// at slot 2 and the device removed sat below it, so HARM moved; the route names HARM and so is not
	// affected by that, which is exactly what a player means by "removing something else left my
	// modulation alone".
	//
	// THE OTHER HALF is what the removal had to clear. Filter A's storage went back into the record's
	// pool, and a removal that did not take its routes with it would leave them for whatever is placed
	// there next — the case named "a lane reissued to a new device carries none of the old device's
	// routes" owns that; this one asserts the part it can see, which is that filter A left nothing
	// behind and the survivor's amount is untouched.
	CHECK(bridge.routesOf(filterA).empty());
	CHECK(bridge.routeAmount(source, atHarmLevel) == doctest::Approx(kAmount));

	//-- 6. SAVE AND LOAD ---------------------------------------------------------------------//
	// THE ONE BLOCK THAT CARRIES THIS STATE, through the same functions feathers_clap.cpp's
	// stateSave/stateLoad call: the bridge's patch, the real bytes, a fresh bridge.
	const feathers::FxChain savedChain = bridge.fxChain();
	const Reloaded loaded = reload(throughBytes(bridge.chainsPatch()));
	const feathers::FxChain& loadedChain = loaded.chain;
	const feathers::ModRack& loadedRack = loaded.rack;
	const TestRoutes& loadedRoutes = loaded.routes;
	const feathers::RouteRefusals& refused = loaded.refused;

	// THE CHAIN CAME BACK AS ITSELF, by serial and by value.
	REQUIRE(loadedChain.count == 2);
	CHECK(feathers::instanceOfFxSlot(loadedChain, 0) == filterC);
	CHECK(feathers::instanceOfFxSlot(loadedChain, 1) == harm);
	CHECK(controlOfInstance(loadedChain, filterC, cutoffIndex) == doctest::Approx(kCutoffC));
	CHECK(controlOfInstance(loadedChain, harm, harmLevelIndex) == doctest::Approx(kHarmLevel));
	// AND THE COUNTER IS PAST EVERY ID THE PATCH NAMED, or the next device added after a load would be
	// issued a number some saved routing already means. Not EQUAL to the saved one any more: the file
	// carries the ids, not the counter, and the loader reserves past every id it reads.
	CHECK(loadedChain.nextInstanceId > filterC);
	CHECK(loadedChain.nextInstanceId > harm);
	CHECK(loadedChain.nextInstanceId > msegId);
	CHECK(loadedChain.nextInstanceId <= savedChain.nextInstanceId);

	// THE ROUTING RESOLVED TO THE SAME DEVICE, ASKED BY NAME (kill-lanes step 5).
	//
	// It was two coordinates recomputed off the loaded views, and the recomputation was doing real work:
	// the file stores an instance id and a control index, so the COLUMN a route lands in is whatever
	// this build's tables make it, and comparing against a remembered column would have asserted that
	// the tables had not changed rather than that the route found its device. There is no column now —
	// the id and the control index the file stores ARE the endpoint — so the two ids remembered from
	// before the save are exactly the right thing to ask about, and the question is the one the case
	// always meant: did this route come back on THESE two devices.
	const backplane::Source loadedSource = backplane::Source::output(msegId, 0);
	const backplane::Destination loadedDestination =
	    backplane::Destination::control(harm, harmLevelIndex);
	CHECK(amountIn(loadedRoutes, loadedSource, loadedDestination) == doctest::Approx(kAmount));
	CHECK(refused.count == 0);
	// AND THE MODULATOR IS BACK IN ITS OWN SLOT — a separate claim from the one above, which an endpoint
	// cannot make for it: a rack that came back in a different order would still carry the route on the
	// right device, and this is what says the ORDER survived too.
	CHECK(feathers::instanceOfSlot(loadedRack, msegSlot) == msegId);
}

TEST_CASE("removing the device a routing points AT leaves nothing dangling, by either road") {
	// TWO ROADS TO ONE STATE, AND THEY ARE NOT THE SAME ROAD.
	//
	//   A. The device is REMOVED and then the patch is saved. `removeFxDevice` drops the routes to
	//      and from it, so the file is written with nothing pointing at it and the loader is never
	//      asked a question.
	//   B. The patch was saved WITH the device, and the chain it is loaded against no longer holds
	//      one — a device that vanished from the build, or a row that failed to resolve. Here the
	//      file DOES carry the route and the loader is the thing that drops it.
	//
	// Both must end with no route and no phantom column, and B must SAY it refused rather than
	// silently discarding — audition.h's RouteRefusals exists so "my modulation stopped working"
	// can be answered. Asserting both in one case is the point: they are the same end state reached
	// by different code, and a build could easily get one right and the other wrong.
	namespace flt = feathers::fx::filter;
	namespace hrm = feathers::fx::harm;
	namespace mseg = feathers::mod::mseg;

	feathers::ParamBridge bridge;
	bridge.restoreFxChain(feathers::defaultFxChain());
	REQUIRE(bridge.addFxDevice(hrm::kTraits.tag) == 1);
	const size_t msegSlot = bridge.addRackDevice(mseg::kTraits.tag);
	REQUIRE(msegSlot < feathers::kMaxRackSlots);

	const feathers::DeviceInstanceId doomed = feathers::instanceOfFxSlot(bridge.fxChain(), 1);
	const feathers::DeviceInstanceId survivor = feathers::instanceOfFxSlot(bridge.fxChain(), 0);
	const auto harmLevel = static_cast<size_t>(hrm::Param::Level);
	const auto cutoff = static_cast<size_t>(flt::Param::Cutoff);

	// **THE THREE ENDS BY NAME (kill-lanes step 5).** They were coordinates built off the chain the
	// bridge held at this instant, which is exactly the thing a removal changes — so the two
	// destinations had to be captured before the removal and were only still correct afterwards by
	// arithmetic. A device's name does not move when its neighbour is removed.
	const backplane::Source source = backplane::Source::output(bridge.rackDeviceAt(msegSlot), 0);
	const backplane::Destination atDoomed = backplane::Destination::control(doomed, harmLevel);
	const backplane::Destination atSurvivor = backplane::Destination::control(survivor, cutoff);
	REQUIRE(atDoomed != atSurvivor);
	bridge.setRouteAmount(source, atDoomed, 0.5f);
	bridge.setRouteAmount(source, atSurvivor, 0.25f);
	REQUIRE(bridge.routeAmount(source, atDoomed) == doctest::Approx(0.5f));

	SUBCASE("A: removed first, so the file never carries it") {
		REQUIRE(bridge.removeFxDevice(1));
		const feathers::FxChain trimmed = bridge.fxChain();
		REQUIRE(trimmed.count == 1);
		CHECK(feathers::slotOfFxInstance(trimmed, doomed) >= trimmed.count);
		// THE RECORD DROPPED IT AT THE MOMENT OF REMOVAL.
		CHECK(bridge.routeAmount(source, atDoomed) == doctest::Approx(0.0f));

		const Reloaded reloaded = reload(throughBytes(bridge.chainsPatch()));
		const TestRoutes& backRoutes = reloaded.routes;
		const feathers::RouteRefusals& refused = reloaded.refused;
		// NOTHING TO REFUSE, because nothing pointing at the dead device was ever written. That is
		// the whole difference from road B and it is asserted rather than assumed.
		CHECK(refused.count == 0);
		// AND THE SURVIVOR'S ROUTE IS STILL THERE — the claim is that removal took one route and not the
		// table. It was recomputed against the loaded chain, because the survivor's COORDINATE could
		// have moved; its name could not, so the end named at the top is the end asked about here.
		CHECK(feathers::slotOfFxInstance(reloaded.chain, survivor) < reloaded.chain.count);
		CHECK(amountIn(backRoutes, source, atSurvivor) == doctest::Approx(0.25f));
	}

	SUBCASE("B: saved with it, loaded without it — dropped and COUNTED") {
		// The patch is written with the whole chain, then the doomed device's record is taken out of it
		// — a hand-edited file, or one whose device a later edit of the format lost. The route that
		// named it is still in the file, and the loader is the thing that drops it.
		feathers::rack::RackPatch patch = bridge.chainsPatch();
		const auto before = patch.device.size();
		std::erase_if(patch.device, [&](const feathers::rack::RackPatch::Device& d) { return d.instanceId == doomed; });
		REQUIRE(patch.device.size() == before - 1);
		const Reloaded reloaded = reload(throughBytes(patch));
		const feathers::FxChain& trimmed = reloaded.chain;
		const TestRoutes& backRoutes = reloaded.routes;
		const feathers::RouteRefusals& refused = reloaded.refused;
		REQUIRE(trimmed.count == 1);

		// SAID, NOT SWALLOWED — and said in the form the tray needs. **THE END THAT NAMED THE DEAD
		// DEVICE IS REPORTED AS THE FILE NAMED IT** (kill-lanes step 3b): the doomed device's Level
		// control, under the doomed device's own id, whether or not the lookup found it. It used to be
		// reported OUT OF RANGE, on the reasoning that it "genuinely has no coordinate in this build's
		// tables" — true of a coordinate, and the reason the contract changed: it left the tray able to
		// say only that something was dropped, never which device. The end that DID resolve — the MSEG,
		// still here — is reported as itself, as it always was.
		//
		// AND THAT IS WHY THE ENDPOINTS ARE THE ASSERTION RATHER THAN THE COUNT. There are two refusal
		// branches — this one, and the rule check that refuses a pair no gesture could have made — and
		// only the ends tell them apart. A count of one is what both produce.
		//
		// THE SECOND HALF OF THE DEAD END IS THAT IT REALLY IS DEAD: naming device `doomed` is only a
		// correct report if this load did not place `doomed`. Both are asserted, for the reason the
		// "a reference this build cannot resolve" case above sets out at length.
		CHECK(refused.count == 1);
		const feathers::DeviceInstanceId msegId = source.device;
		REQUIRE(msegId != feathers::kNoInstance);
		CHECK(refused.firstSource == source);
		// **THE DEVICE, AND THE CONTROL IN WORDS** (backlog JF): the file names the control by KEY, and a
		// key becomes an index only on a device that declares it — this one is in neither the patch nor
		// the build's record, so the endpoint carries the id and the report carries the key.
		CHECK(refused.firstDestination.device == atDoomed.device);
		CHECK(reloaded.report.firstOf(feathers::rack::LoadReport::Kind::RouteDropped)
		          .what.find(std::string("'") + hrm::kDescriptor.params[harmLevel].key + "'")
		      != std::string::npos);
		CHECK_FALSE(holdsDevice(reloaded, doomed));
		CHECK(holdsDevice(reloaded, msegId)); // the end that resolved is still here, which is why it resolved
		// AND THE SURVIVOR CAME THROUGH, so the loader dropped one route rather than the block.
		CHECK(feathers::slotOfFxInstance(trimmed, survivor) < trimmed.count);
		CHECK(amountIn(backRoutes, source, atSurvivor) == doctest::Approx(0.25f));
		// AND NOTHING LANDED ANYWHERE ELSE. A loader that resolved a missing instance to the first
		// device rather than dropping it would put 0.5 somewhere real, and the count above would still
		// read 1 — so this is the assertion that separates "refused" from "redirected onto the
		// survivor". Stated as the whole list rather than as two columns reading zero, which is
		// stronger: a redirection onto a device this case never named is caught too. The list is the
		// survivor AND the default patch's envelope route (backlog EF), which the bridge was born with
		// and the save carried — two, and the second is named so the count cannot hide a redirection.
		CHECK(backRoutes.size() == 2);
		CHECK(amountIn(backRoutes, defaultEnvelopeRoute().source, defaultEnvelopeRoute().destination)
		      == doctest::Approx(1.0f));
		CHECK(amountIn(backRoutes, source, atDoomed) == doctest::Approx(0.0f));
	}
}

TEST_CASE("a serial is never reissued, not even to the slot a removal just freed") {
	// THE ABA RULE, DRIVEN THROUGH THE GESTURES RATHER THAN ASSERTED ON THE CHAIN. There is a case
	// for the counter itself; this one fills the lane, grows past its starting size, frees a slot and adds
	// again — the sequence a hand actually performs — and requires that the device arriving in the
	// freed slot is not wearing a number some saved routing already means.
	//
	// An address is only a valid identity while the thing at it cannot be replaced, and a fixed-size pool exists precisely to replace things at fixed addresses. A slot
	// freed and refilled is that pool. Eight is a better number than two, but the shape is identical.
	namespace flt = feathers::fx::filter;
	namespace hrm = feathers::fx::harm;

	feathers::ParamBridge bridge;
	bridge.restoreFxChain(feathers::defaultFxChain());

	std::vector<feathers::DeviceInstanceId> everIssued;
	everIssued.push_back(feathers::instanceOfFxSlot(bridge.fxChain(), 0));

	// FILL IT. Alternating kinds, so nothing here can pass by every device being the same one.
	while (bridge.fxChain().count < feathers::kMaxFxSlots) {
		const char* tag = (bridge.fxChain().count % 2 == 1) ? hrm::kTraits.tag : flt::kTraits.tag;
		const size_t slot = bridge.addFxDevice(tag);
		REQUIRE(slot < feathers::kMaxFxSlots);
		everIssued.push_back(feathers::instanceOfFxSlot(bridge.fxChain(), slot));
	}
	REQUIRE(bridge.fxChain().count == feathers::kMaxFxSlots);
	REQUIRE(everIssued.size() == feathers::kMaxFxSlots);

	// PAST THE STARTING SIZE: it grows now (unbounded devices step 6), the last device untouched rather
	// than replaced, and the newcomer takes the next serial like any other.
	const feathers::FxChain full = bridge.fxChain();
	REQUIRE(bridge.addFxDevice(flt::kTraits.tag) == feathers::kMaxFxSlots);
	CHECK(bridge.fxChain().count == feathers::kMaxFxSlots + 1);
	CHECK(feathers::instanceOfFxSlot(bridge.fxChain(), feathers::kMaxFxSlots - 1)
	      == feathers::instanceOfFxSlot(full, feathers::kMaxFxSlots - 1));
	everIssued.push_back(feathers::instanceOfFxSlot(bridge.fxChain(), feathers::kMaxFxSlots));

	// FREE ONE FROM THE MIDDLE, so the survivors shift and the freed slot index is reused by a
	// DIFFERENT existing device — which is the arrangement that makes a slot-keyed identity wrong.
	const feathers::DeviceInstanceId removed = feathers::instanceOfFxSlot(bridge.fxChain(), 3);
	REQUIRE(bridge.removeFxDevice(3));
	REQUIRE(bridge.fxChain().count == feathers::kMaxFxSlots);
	CHECK(feathers::slotOfFxInstance(bridge.fxChain(), removed) >= bridge.fxChain().count);

	// AND ADD AGAIN.
	const size_t reborn = bridge.addFxDevice(hrm::kTraits.tag);
	REQUIRE(reborn == feathers::kMaxFxSlots);
	const feathers::DeviceInstanceId fresh = feathers::instanceOfFxSlot(bridge.fxChain(), reborn);

	// THE CLAIM, AS A VALUE AND NOT AS AN INEQUALITY AGAINST ONE NEIGHBOUR. Greater than every serial
	// this lane has ever handed out — "different from the one just removed" would be satisfied by a
	// counter that wrapped onto an older device, which is the same bug wearing a longer fuse.
	for (const feathers::DeviceInstanceId issued : everIssued) {
		CAPTURE(issued);
		CAPTURE(fresh);
		CHECK(fresh > issued);
	}
	CHECK(fresh != removed);
	// **AND THE STRIP'S SERIALS COUNT TOO, WHICH IS NEW AND IS THE POINT.** This line was
	// `fresh == kMaxFxSlots + 1` — the lane numbering from one, on its own counter. There is one
	// counter now (`ParamBridge::nextInstance_`, vision D1), so the lane no longer starts at 1 and a
	// number pinned here would only be re-pinning where the sequence happens to begin.
	//
	// What is asserted instead is the property that number was standing in for, over BOTH chains:
	// the fresh serial is exactly one past the highest anyone has ever issued. Greater-than alone
	// would pass on a counter that jumped; equality to max+1 says it advanced by one and neither
	// skipped nor reset.
	std::vector<feathers::DeviceInstanceId> everIssuedAnywhere = everIssued;
	for (size_t slot = 0; slot < bridge.modRack().count; ++slot) {
		everIssuedAnywhere.push_back(bridge.rackIdentityAt(slot).instanceId);
	}
	const feathers::DeviceInstanceId highest
	    = *std::max_element(everIssuedAnywhere.begin(), everIssuedAnywhere.end());
	CAPTURE(highest);
	CAPTURE(fresh);
	CHECK(fresh == static_cast<feathers::DeviceInstanceId>(highest + 1));
	// AND NO TWO DEVICES ANYWHERE WEAR THE SAME NUMBER — the collision `Graph::restore` refuses, and
	// the whole reason the two counters became one.
	std::sort(everIssuedAnywhere.begin(), everIssuedAnywhere.end());
	CHECK(std::adjacent_find(everIssuedAnywhere.begin(), everIssuedAnywhere.end())
	      == everIssuedAnywhere.end());

	SUBCASE("and the counter survives a save, or the next add after a load collides") {
		// THE HALF THAT NEEDS A FILE. A counter kept only in memory reads back as 1 on the next load,
		// and the very next device added would wear a serial some saved routing already means — the
		// ABA bug arriving a session later, which is the hardest possible time to find it.
		//
		// THE FILE CARRIES THE IDS, NOT THE COUNTER, since the one format: a load reserves the counter
		// past every id the patch names before it places any. So the claim is asserted as what it is
		// for — the next device added after a load wears a number nobody in the file does.
		ParamBridge loaded;
		CHECK(loaded.restoreChains(throughBytes(bridge.chainsPatch())).routes.count == 0);
		CHECK(loaded.fxChain().nextInstanceId > fresh);
		const size_t slot = loaded.addRackDevice(feathers::mod::dahdsr::kTraits.tag);
		REQUIRE(slot < feathers::kMaxRackSlots);
		const feathers::DeviceInstanceId next = loaded.rackIdentityAt(slot).instanceId;
		CAPTURE(next);
		CHECK(next > highest);
		for (size_t s = 0; s < loaded.fxChain().count; ++s) {
			CHECK(feathers::instanceOfFxSlot(loaded.fxChain(), s) != next);
		}
	}
}

namespace {

/// A FIELD VALUE THAT IS DISTINCT PER INDEX AND EXACT THROUGH A FLOAT ROUND TRIP. A power-of-two
/// step, so nothing below can pass or fail on a rounding question rather than on the transport.
float probeField(size_t index) {
	return 0.03125f * static_cast<float>(index + 1);
}

/// **AND A VALUE THIS CONTROL CAN HOLD.** The one format's loader stores a base by the one rule
/// (`device::coerceControl`, backlog DV: rounded if stepped, clamped always), so a probe outside a
/// control's range would come back CORRECTED — a coercion, not a transport failure. The probe is taken
/// through the control's own range, at a power-of-two fraction, and then through the same rule, so the
/// value written is one the transport must carry exactly. A blob float has no declaration and keeps
/// the raw probe.
float probeFor(const feathers::device::Descriptor& desc, size_t index) {
	if (index >= desc.params.size()) {
		return probeField(index);
	}
	const feathers::device::ParamDesc& p = desc.params[index];
	const double t = 0.03125 * static_cast<double>((index % 31) + 1);
	double v = p.min + (p.max - p.min) * t;
	// A LABELLED CHOICE IS SAVED AS ITS LABEL (backlog JF §3.4), so a value between its steps comes back ON
	// the step every reader already took it as — the probe is a value the file can hold.
	if (p.unit == feathers::device::Unit::Choice && !p.names.empty()) {
		v = p.min + std::round(v - p.min);
	}
	return feathers::device::coerceControl(p, static_cast<float>(v));
}

} // namespace

TEST_CASE("every device this build ships carries its whole row through a save and a load") {
	// DRIVEN FROM THE REGISTRY, NEVER FROM A LIST SPELLED HERE. The suite's existing round-trip cases
	// name the filter, HARM, the DAHDSR and the MSEG one at a time — which covered the devices that
	// existed when each was written and stopped covering the moment a fifth was registered. Three
	// devices (owltt, waypoint, drawn) have never been through this door, and the widest of them is
	// the one that forced kMaxDeviceFields from 64 to 128, which is exactly the width a transport
	// bug would hide in.
	//
	// WHAT IT ASKS, of every device and in whichever lane the device's own declarations allow it:
	// seat it, write a distinct value into EVERY declared field, save the block, load it, and compare
	// the whole row — tag, serial, lane, fieldsPresent and every field.
	for (const feathers::device::Descriptor& desc : feathers::kDevices) {
		// AS A STRING, because a `const char*` capture prints as a pointer-shaped `1` and a sweep
		// whose failure does not say WHICH device is a sweep you have to re-run by hand to read.
		const std::string deviceName(desc.traits.name);
		CAPTURE(deviceName);
		// **THE ROW RULE: DECLARED PARAMS PLUS THE DEVICE'S OPAQUE STATE BLOB.** By design
		// (2026-09-02), an arbitrary state blob is expected. A device's floats past its
		// parameters are state the host stores and returns without parsing — `device_row.h` states the
		// contract and the ceiling on `kMaxDeviceFields`.
		//
		// **THIS CASE CAUGHT THE WIDENING THE HOUR IT LANDED**, which is the argument for it existing:
		// it read `desc.params.size()` and went red the moment one device declared a blob, before any
		// blob was written or any new case for it was added.
		const size_t fields = desc.params.size() + desc.traits.extraState;
		// THE TRANSPORT IS NOT TRUNCATING ANYBODY, said before anything is saved. device_row.h's
		// static_assert already refuses a device wider than the carrier at compile time; this is the
		// same claim where a reader of a failure can see it.
		REQUIRE(fields <= feathers::kMaxDeviceFields);

		// EVERY DEVICE HAS A HOME. A device that may sit in neither chain is a device nothing below
		// exercises, and it would pass this case by being skipped.
		CHECK((feathers::maySitInAudioChain(desc) || feathers::maySitInModRack(desc)));

		if (feathers::maySitInAudioChain(desc)) {
			feathers::FxChain chain;
			REQUIRE(feathers::addFxDevice(&chain, desc.traits.tag) == 0);
			for (size_t f = 0; f < fields; ++f) {
				chain.device[0].row.field[f] = probeFor(desc, f);
			}
			const feathers::DeviceRow before = fxDeviceAtPosition(chain, 0).row;
			REQUIRE(before.fieldsPresent == fields);

			const feathers::FxChain back = reload(throughBytes(patchOf(chain, feathers::ModRack {}))).chain;
			REQUIRE(back.count == 1);
			// THE WHOLE ROW, so a field nobody thought to name is covered by the same line.
			CHECK(fxDeviceAtPosition(back, 0).row == before);
			const feathers::DeviceRow& got = fxDeviceAtPosition(back, 0).row;

			// **AND A DEVICE THAT DECLARES NO BLOB ROUND-TRIPS EXACTLY AS IT DID BEFORE BLOBS
			// EXISTED.** Widening a shared contract has to cost nothing to everyone not using it, and
			// that is a claim about THIS device rather than about the feature — so it is asserted
			// here, inside the sweep, for every device the build ships.
			if (desc.traits.extraState == 0) {
				CHECK(got.fieldsPresent == desc.params.size());
			}
			else {
				CHECK(got.fieldsPresent == desc.params.size() + desc.traits.extraState);
			}
			// AND SAID AGAIN AS THE VALUES, so a failure names WHICH field rather than only that the
			// rows differ — and so the claim is legible without knowing what DeviceRow compares.
			CHECK(got.fieldsPresent == fields);
			for (size_t f = 0; f < fields; ++f) {
				CAPTURE(f);
				CHECK(got.field[f] == probeFor(desc, f));
			}
		}

		if (feathers::maySitInModRack(desc)) {
			feathers::ModRack rack;
			rack.count = 0;
			REQUIRE(feathers::addRackDevice(&rack, desc.traits.tag) == 0);
			for (size_t f = 0; f < fields; ++f) {
				rack.device[0].field[f] = probeFor(desc, f);
			}
			const feathers::DeviceRow before = rack.device[0];
			REQUIRE(before.fieldsPresent == fields);

			const feathers::ModRack back = reload(throughBytes(patchOf(feathers::FxChain {}, rack))).rack;
			REQUIRE(back.count == 1);
			// **EXCEPT A SELF-CHANGING DEVICE'S BASIS, WHICH A LOAD STAMPS AFRESH BY DESIGN** (`ModOps::extraStateBasis`,
			// backlog MX): it is what makes the running instance play what was loaded. It is the one float that may
			// differ, and it must be a fresh one — a basis no hand and no device wrote.
			const size_t stamped = desc.mod != nullptr && desc.mod->publishExtraState != nullptr && desc.mod->extraStateBasis >= 0
			                           ? desc.params.size() + static_cast<size_t>(desc.mod->extraStateBasis)
			                           : fields;
			if (stamped == fields) {
				CHECK(back.device[0] == before);
			}
			CHECK(back.device[0].fieldsPresent == fields);
			for (size_t f = 0; f < fields; ++f) {
				CAPTURE(f);
				if (f == stamped) {
					CHECK(back.device[0].field[f] < 0.0f);
				}
				else {
					CHECK(back.device[0].field[f] == probeFor(desc, f));
				}
			}
		}
	}
}

TEST_CASE("every control every device declares modulatable is routable, and the route survives the file") {
	// THE SECOND HALF OF THE SAME SWEEP, and the half where a device's own sparse `modulatable` flags
	// meet the host's ordinal arithmetic. The existing cases ask this of the DAHDSR and the MSEG; the
	// drawn filter declares 103 controls of which only a handful are modulatable, which is the widest
	// gap between a control index and an ordinal this build contains — and the control index is what
	// goes in the file while the ordinal is what addresses the matrix.
	for (const feathers::device::Descriptor& desc : feathers::kDevices) {
		// AS A STRING, because a `const char*` capture prints as a pointer-shaped `1` and a sweep
		// whose failure does not say WHICH device is a sweep you have to re-run by hand to read.
		const std::string deviceName(desc.traits.name);
		CAPTURE(deviceName);

		// SEATED ALONE, in whichever chain its declarations allow, so nothing below depends on what
		// else happens to be in a fixture.
		feathers::FxChain chain;
		chain.count = 0;
		feathers::ModRack rack;
		rack.count = 0;
		// **NO COORDINATE IS MINTED HERE AT ALL (kill-lanes step 5).** It computed the lane of whichever
		// band the device sits in, because the destination it was about to route to was derived from
		// that lane. A route names the DEVICE, so where it sits is no longer part of addressing it, and
		// which chain it went into is left as what it is — a fact about the device's declarations.
		if (feathers::maySitInModRack(desc)) {
			REQUIRE(feathers::addRackDevice(&rack, desc.traits.tag) == 0);
		}
		else {
			REQUIRE(feathers::addFxDevice(&chain, desc.traits.tag) == 0);
		}
		// **THE BRIDGE IS THE FIXTURE NOW.** It was a `DeviceSlots` built from the two views, which is
		// all the retired band rule needed; the authority is the record's, so the chains have to be on a
		// bridge for it to be asked at all — and the device's id comes back off the same object, which
		// is what a route names.
		//
		// **THROUGH `holdChains`, WHICH IS WHAT MAKES THE ID THE SAME ONE THE PATCH CARRIES.** A bridge
		// handed a chain without being emptied first keeps the default instance it was constructed with
		// and re-issues, so the id read off it is not the id `patchOf` writes — and the route below,
		// which NAMES that id, then describes a device the patch does not hold and is dropped. MEASURED
		// while porting: `patch.route.size()` came back 0 for the filter. That trap did not exist while
		// the destination was a coordinate, which is the shape of most of this port.
		feathers::ParamBridge bridge;
		holdChains(bridge, chain, rack);
		const feathers::DeviceInstanceId seated =
		    feathers::maySitInModRack(desc) ? bridge.rackDeviceAt(0) : bridge.fxDeviceAt(0);
		REQUIRE(seated != feathers::kNoInstance);
		const backplane::Source macro0 = backplane::Source::core(0);

		for (size_t ordinal = 0; ordinal < desc.modulatableCount(); ++ordinal) {
			CAPTURE(ordinal);

			// THE ORDINAL AND THE CONTROL INDEX AGREE IN BOTH DIRECTIONS, restated here rather than
			// asked of one function twice: the file stores the control index, and the ordinal is still
			// what a saved route was once bounded by — so a pair that did not invert would
			// put a saved route on a different control of the same device.
			const size_t control = desc.controlOfNthModulatable(ordinal);
			REQUIRE(control < desc.params.size());
			CHECK(desc.params[control].modulatable);
			CHECK(desc.nthModulatableOfControl(control) == ordinal);

			const backplane::Destination destination =
			    backplane::Destination::control(seated, control);
			// AND THE ONE AUTHORITY AGREES IT IS REACHABLE — the gate the editor's glow and the
			// loader both ask.
			CHECK(bridge.mayRoute(macro0, destination));

			const TestRoutes routes { { macro0, destination, 0.25f } };
			const feathers::rack::RackPatch patch = patchOf(chain, rack, routes);
			// ON THE WIRE BY THE CONTROL'S KEY (backlog JF) — not the ordinal, and not the index either.
			REQUIRE(patch.route.size() == 1);
			CHECK(patch.route[0].destination.key == desc.params[control].key);
			const Reloaded back = reload(throughBytes(patch));
			CHECK(back.refused.count == 0);

			// THE AMOUNT LANDS ON THE SAME CONTROL, and the whole list is compared so a route that
			// arrived somewhere ELSE as well is caught by the same line rather than passing because
			// the one end this case names happens to be right.
			CHECK(amountIn(back.routes, macro0, destination) == doctest::Approx(0.25f));
			CHECK(back.routes == routes);
		}

		// AND THE CONTROLS IT DOES NOT DECLARE MODULATABLE ARE NOT REACHABLE — nor is an index past its
		// control list at all.
		//
		// **THIS WAS "THE SLACK COLUMNS THE STRIDE LEAVES" and it is a better claim now.** The stride
		// reserved a fixed run of columns per slot and the ones past a device's width named nothing,
		// which is a property of the ADDRESS SPACE; a route names the device's own control index, so
		// what has to be refused is a control the device really has but did not offer, and one it does
		// not have. The first of those the stride could not express at all — every non-modulatable
		// control simply had no column — and it is the case that shipped a bug once (the DAHDSR's
		// "Smoothing On" switch, declared not modulatable, resolving to a routable address).
		for (size_t control = 0; control < desc.params.size(); ++control) {
			if (desc.params[control].modulatable) {
				continue;
			}
			CAPTURE(control);
			CHECK_FALSE(bridge.mayRoute(macro0, backplane::Destination::control(seated, control)));
		}
		CHECK_FALSE(
		    bridge.mayRoute(macro0, backplane::Destination::control(seated, desc.params.size())));
	}
}

TEST_CASE("every output every device declares is a source, and the slack past it is not") {
	// THE SOURCE AXIS, SWEPT THE SAME WAY. `isLiveDeviceSource` was written after the destination
	// side's leak was found, and its own note records that a case caught the widening rather than a
	// reading did — so it is the half most worth asking of every device rather than of the two that
	// happened to have outputs when it was written.
	for (const feathers::device::Descriptor& desc : feathers::kDevices) {
		// AS A STRING, because a `const char*` capture prints as a pointer-shaped `1` and a sweep
		// whose failure does not say WHICH device is a sweep you have to re-run by hand to read.
		const std::string deviceName(desc.traits.name);
		CAPTURE(deviceName);

		feathers::FxChain chain;
		chain.count = 0;
		feathers::ModRack rack;
		rack.count = 0;
		// **NO COORDINATE IS MINTED HERE AT ALL (kill-lanes step 5)** — see the destination sweep above.
		if (feathers::maySitInModRack(desc)) {
			REQUIRE(feathers::addRackDevice(&rack, desc.traits.tag) == 0);
		}
		else {
			REQUIRE(feathers::addFxDevice(&chain, desc.traits.tag) == 0);
		}
		// **A DEVICE THAT RENDERS AUDIO CANNOT REACH A CORE DESTINATION, AND THE RULE REFUSING IT IS THE
		// RULE WORKING** — so the destination this sweep asks about depends on what the device IS.
		//
		// `Graph::forEachPrecedence` makes the host's own node precede every audio-carrying device in
		// every chain ("the voice's output is what an audio chain is a chain of"), and `wouldCycle` then
		// refuses any route from such a device back into a voice parameter: it would have to be read
		// before it is written. That held for every device in the registry with outputs until SKLITTER
		// declared some, because before it the two sets were disjoint — every `src/fx/*` descriptor
		// published nothing. **The claim this case makes is about the SOURCE axis** ("every declared
		// output is a source, the slack past it is not"), and the destination is incidental to it, so a
		// dual device is asked about one it can legally reach rather than being asked to break a rule.
		//
		// **A FILTER AS THE SINK**: it declares no outputs of its own (so it cannot become the subject of
		// the loop below by accident), it has modulatable controls, and it goes in the fx chain where
		// nothing the sweep seats reaches it backwards.
		size_t sinkSlot = feathers::kMaxFxSlots;
		if (feathers::maySitInAudioChain(desc)) {
			sinkSlot = feathers::addFxDevice(&chain, feathers::fx::filter::kTraits.tag);
			REQUIRE(sinkSlot < feathers::kMaxFxSlots);
		}
		// THROUGH `holdChains`, so the id this reads is the id `patchOf` writes — see the destination
		// sweep above for what happens when it is not.
		feathers::ParamBridge bridge;
		holdChains(bridge, chain, rack);
		const feathers::DeviceInstanceId seated =
		    feathers::maySitInModRack(desc) ? bridge.rackDeviceAt(0) : bridge.fxDeviceAt(0);
		REQUIRE(seated != feathers::kNoInstance);

		const int fine = feathers::modDestinationIndexOf(feathers::ParamId::FineTune);
		REQUIRE(fine >= 0);
		backplane::Destination destination = backplane::Destination::core(static_cast<size_t>(fine));
		if (sinkSlot < feathers::kMaxFxSlots) {
			const feathers::DeviceInstanceId sink = bridge.fxDeviceAt(sinkSlot);
			REQUIRE(sink != feathers::kNoInstance);
			REQUIRE(sink != seated); // or the route would be a device reaching itself, refused for a
			                         // different reason than the one under test
			const size_t control = feathers::fx::filter::kDescriptor.controlOfNthModulatableBesideEnable(0);
			REQUIRE(control < feathers::fx::filter::kDescriptor.params.size());
			destination = backplane::Destination::control(sink, control);
		}

		// **SWEPT PAST THE DEVICE'S OWN OUTPUTS, BOUNDED BY THE STRIDE** — kept deliberately: the stride
		// is still what `mayRoute` bounds a device source against (`Axes::outputStride`), so it is the
		// right place to stop, and the outputs past a device's declared span are exactly the "slack" the
		// case was written for. What changed is only that an output is asked of its DEVICE rather than
		// of the column the device's lane would have owned.
		//
		// **THE SLACK HALF HAS NO SUBJECT FOR SKLITTER, AND THAT IS THE BUDGET BEING FULL RATHER THAN
		// ANYTHING THIS CASE DOES.** The loop stops at `kMaxDeviceOutputs`, which is 7, and SKLITTER
		// declares exactly 7 — so `declared` is true on every one of its iterations and the negative half
		// is never exercised for the one device the destination retarget above was written for. It is
		// exercised for every other device in the registry, so the half is not dead; it simply cannot
		// say anything about a device that fills the budget. **Whoever raises `kMaxDeviceOutputs` gives
		// this case its subject back for free** — no edit here, the loop just runs past SKLITTER's span.
		for (size_t output = 0; output < feathers::device::kMaxDeviceOutputs; ++output) {
			CAPTURE(output);
			const backplane::Source source = backplane::Source::output(seated, output);
			// DECLARED OR SLACK, ASKED OF THE DEVICE'S OWN SPAN rather than of the file under test —
			// the independent restatement that made the destination-side version worth having.
			const bool declared = output < desc.outputs.size();
			CHECK(bridge.mayRoute(source, destination) == declared);
			if (!declared) {
				continue;
			}

			const TestRoutes routes { { source, destination, -0.4f } };
			const Reloaded back = reload(throughBytes(patchOf(chain, rack, routes)));
			CHECK(back.refused.count == 0);
			CHECK(amountIn(back.routes, source, destination) == doctest::Approx(-0.4f));
			CHECK(back.routes == routes);
		}
	}
}

TEST_CASE("a hand's value is coerced by the control's own declaration, for every device and every control") {
	// THE KNOB ROAD FOR EVERY SWALLOWED CONTROL, AND IT WAS NOT TESTED AT ALL. `setFxDeviceControl`
	// is what an `FxControl` drag writes (fx_control.h:98, :165), and its own comment states the
	// contract it performs: "COERCED BY THE CONTROL'S OWN DECLARATION — the same rule every other
	// write in this file obeys, and the reason a widget may hand over whatever its drag produced."
	//
	// MEASURED BEFORE THIS CASE EXISTED: deleting the clamp reddens NOTHING — 113 of 113 cases in
	// this suite still pass, and the only other callers anywhere (test_gui_widgets.cpp, fifteen
	// sites) pass in-range values, so neither the clamp nor the stepped rounding had a case. A
	// widget that "may hand over whatever its drag produced" was relying on a line nothing checked.
	//
	// ASKED OF THE DEVICE'S OWN ParamDesc rather than of the bridge, which is what makes it an
	// independent restatement: the declaration is the device's, the coercion is the bridge's, and
	// the case is that the second honours the first.
	for (const feathers::device::Descriptor& desc : feathers::kDevices) {
		if (!feathers::maySitInAudioChain(desc)) {
			continue; // the rack has no generic control road; see the note in the next case
		}
		const std::string deviceName(desc.traits.name);
		CAPTURE(deviceName);

		feathers::ParamBridge bridge;
		feathers::FxChain empty;
		empty.count = 0;
		bridge.restoreFxChain(empty);
		REQUIRE(bridge.addFxDevice(desc.traits.tag) == 0);

		const auto fieldAt = [&bridge](size_t control) {
			return bridge.fxChain().device[0].row.field[control];
		};
		// **THE DEVICE, NOT THE LANE (kill-lanes step 5).** `setControlAtLane(bridge, 0, …)` resolved a
		// coordinate to whatever the surface had at it; `setDeviceControl` is the road the widget takes
		// and it names the device.
		const feathers::DeviceInstanceId seated = bridge.fxDeviceAt(0);
		REQUIRE(seated != feathers::kNoInstance);
		const auto setControl = [&bridge, seated](size_t control, float value) {
			bridge.setDeviceControl(seated, control, value);
		};

		for (size_t control = 0; control < desc.params.size(); ++control) {
			CAPTURE(control);
			const feathers::device::ParamDesc& pd = desc.params[control];

			// PAST THE TOP OF THE TRAVEL, and past the bottom. A drag does not stop at a declared
			// range; the hand stops at the end of the widget and the value it produces is whatever
			// the geometry said.
			setControl(control, static_cast<float>(pd.max) + 1000.0f);
			CHECK(fieldAt(control) == doctest::Approx(static_cast<float>(pd.max)));
			setControl(control, static_cast<float>(pd.min) - 1000.0f);
			CHECK(fieldAt(control) == doctest::Approx(static_cast<float>(pd.min)));

			// AND A STEPPED CONTROL IS STORED AS THE HAND LEFT IT — NOT ROUNDED, by design. This said "lands on a step" and asserted round-then-clamp; every reader takes the
			// nearest step on its way in (the device's decode, `withRouteOffset`, `steppedIndex`), so the
			// store keeps the fraction. Clamped still, which the two lines above assert.
			if (pd.stepped && pd.max > pd.min) {
				const double between = std::min(pd.min + 0.6, pd.max);
				setControl(control, static_cast<float>(between));
				CHECK(fieldAt(control) == doctest::Approx(static_cast<float>(between)));
			}

			// AND WHAT IS NOT A NUMBER DOES NOT REACH THE ROW. The old value stands, which is the
			// posture every other write on this bridge takes.
			const float held = fieldAt(control);
			setControl(control, std::numeric_limits<float>::quiet_NaN());
			CHECK(fieldAt(control) == doctest::Approx(held));
		}
	}
}

TEST_CASE("editing a control past a short row widens the row rather than losing the edit") {
	// A ROW MAY ARRIVE SHORTER THAN THE DEVICE IS — a patch from a build that declared fewer
	// controls, or a hand-written record — and `fieldsPresent` is what says so. Fields past it read
	// as ABSENT, which the device's own stateOf answers with its DECLARED DEFAULT rather than with
	// whatever is in the array.
	//
	// SO AN EDIT BEYOND THE END HAS TO WIDEN THE ROW, and `setFxDeviceControl` says it does: "A row
	// that reached only part of the way across a device must not stay short after a hand has edited
	// a field beyond its end, or the edit reads back as absent."
	//
	// MEASURED BEFORE THIS CASE EXISTED: deleting that widening reddens NOTHING in this suite —
	// 113 of 113 still pass. The symptom it leaves is the honesty rule's own failure case: a knob
	// that moves, writes, and changes no sound, because the value it wrote is past the count and
	// every reader treats it as absent.
	namespace flt = feathers::fx::filter;
	const feathers::device::Descriptor* desc = feathers::deviceForTag(flt::kTraits.tag);
	REQUIRE(desc != nullptr);
	REQUIRE(desc->params.size() > 3); // or there is no "beyond the end" to reach

	feathers::ParamBridge bridge;
	feathers::FxChain chain;
	chain.count = 0;
	REQUIRE(feathers::addFxDevice(&chain, flt::kTraits.tag) == 0);
	// SHORTENED DELIBERATELY, the way a file from a narrower build would arrive.
	chain.device[0].row.fieldsPresent = 2;
	bridge.restoreFxChain(chain);
	REQUIRE(bridge.fxChain().device[0].row.fieldsPresent == 2);

	const size_t control = desc->params.size() - 1;
	const feathers::device::ParamDesc& pd = desc->params[control];
	// A VALUE THE CONTROL CAN REALLY HOLD, and one that is NOT its declared default — otherwise the
	// "absent reads as the default" failure and a correct write are the same number.
	const double wanted = pd.def == pd.max ? pd.min : pd.max;
	REQUIRE(wanted != pd.def);

	// BY DEVICE (kill-lanes step 5) — `setControlAtLane(bridge, 0, …)` resolved a coordinate to whatever
	// the surface held at it; the widget's own road names the device.
	const feathers::DeviceInstanceId seated = bridge.fxDeviceAt(0);
	REQUIRE(seated != feathers::kNoInstance);
	bridge.setDeviceControl(seated, control, static_cast<float>(wanted));

	const feathers::DeviceRow row = bridge.fxChain().device[0].row;
	// THE COUNT MOVED, said before the value is read: a row still claiming two fields would make
	// every check below pass on a value nothing downstream can see.
	CHECK(row.fieldsPresent == control + 1);
	CHECK(row.field[control] == doctest::Approx(static_cast<float>(wanted)));

	// AND THE DEVICE ACTUALLY READS IT, through the same door the engine uses — which is the claim
	// that matters, since `fieldsPresent` is only interesting because `fromDeviceRow` bounds by it.
	flt::Row unpacked {};
	const size_t present = feathers::fromDeviceRow(row, unpacked);
	CHECK(present == control + 1);
	CHECK(unpacked[control] == doctest::Approx(static_cast<float>(wanted)));
}

TEST_CASE("a stance index a patch names but this build cannot hold is folded, not believed") {
	// THE ONE LINE BETWEEN A MALFORMED PATCH AND AN OUT-OF-BOUNDS WRITE, and it had no case.
	//
	// MEASURED BEFORE THIS CASE EXISTED: deleting the clamp in loadSoundVariations — the line whose
	// comment reads "An active index a shorter build cannot address lands on its last entry rather
	// than out of bounds" — reddens NOTHING. 115 of 115 cases in this suite still passed.
	//
	// WHAT IT GUARDS IS NOT A WRONG VALUE, IT IS A WRITE PAST AN ARRAY. `active` is read out of the
	// file as a float and sanity-bounded at 64, while `SoundVariations::List::values` holds
	// kMaxVariations (8) entries of ~1 KB each. Two roads then subscript it with `active` and only
	// check that the list is non-empty:
	//
	//     feathers_clap.cpp:443   list.values[list.active] = liveRideAlongFor(slot);   // on SAVE
	//     sound_stack.cpp:1036    list.values[list.active] = before.values;            // on switch
	//
	// Neither the bridge's `restoreSoundVariations` nor `setSoundVariations` clamps — they copy the
	// struct verbatim — so the loader is the only door, and a file saying `active = 64` would put a
	// kilobyte fifty-six entries past the end of an eight-entry array. It is a file on somebody's
	// disk, which is this reader's whole threat model.
	//
	// FSV1 IS THE SHORTEST STREAM THAT REACHES THE LINE: two header counts, then per slot a count,
	// an active, and two floats per stance. What is under test is the header pair, not the payload.
	struct Malformed {
		const char* what;
		float count;
		float active;
		uint8_t expected;
	};
	// THE LAST ROW IS THE CONTROL, and it is what stops this case from passing by folding
	// everything: a legal index must come back UNTOUCHED, or "always answers zero" would satisfy
	// every other row here.
	const Malformed cases[] = {
		{ "past the array itself", 1.0f, 64.0f, 0 },
		{ "inside the array but past the count", 2.0f, 7.0f, 1 },
		{ "named on a sound that stored none", 0.0f, 3.0f, 0 },
		{ "a legal index is left alone", 3.0f, 1.0f, 1 },
	};

	for (const Malformed& bad : cases) {
		CAPTURE(bad.what);
		MemoryStream stream;
		stream.bytes = { 'F', 'S', 'V', '1' };
		pushFloat(stream.bytes, 8.0f); // slots
		pushFloat(stream.bytes, 8.0f); // stances per sound
		pushFloat(stream.bytes, bad.count);
		pushFloat(stream.bytes, bad.active);
		for (int at = 0; at < static_cast<int>(bad.count); ++at) {
			pushFloat(stream.bytes, 0.0f);
			pushFloat(stream.bytes, 0.0f);
		}
		for (int slot = 1; slot < 8; ++slot) {
			pushFloat(stream.bytes, 0.0f); // count
			pushFloat(stream.bytes, 0.0f); // active
		}

		SoundVariations loaded;
		bool present = false;
		size_t valuesRead = 99;
		clap_istream in = stream.in();
		// ACCEPTED, NOT REFUSED. The block is well formed — a count and an index are both legal
		// numbers — so the honest answer is to open the patch and hold the nearest thing this build
		// can address, which is what the comment on the clamp says it does.
		REQUIRE(loadSoundVariations(&in, &loaded, &present, &valuesRead));
		REQUIRE(present);
		const SoundVariations::List& list = loaded.sound[0];
		CHECK(list.count == static_cast<uint8_t>(bad.count));

		// THE MEMORY-SAFETY HALF, said first and on its own: whatever else is true, the index the
		// two write roads above are about to subscript with is inside the array.
		CHECK(list.active < kMaxVariations);
		// AND THE STRUCT'S OWN DECLARED INVARIANT — "never past it" — which is the stronger claim
		// and the one that says the patch's meaning was preserved rather than merely made safe.
		if (list.count > 0) {
			CHECK(list.active < list.count);
		}
		// AND THE EXACT INDEX, so a fold to something arbitrary is not mistaken for the nearest
		// thing this build can hold.
		CHECK(list.active == bad.expected);
	}
}

// **TWO CASES ABOUT THE LANE POOL WERE HERE AND ARE DELETED WITH IT (kill-lanes step 3c).** Both were
// aimed at real guards, both had been MEASURED as reddening nothing when their guard was deleted, and
// both guards are now gone rather than merely untested — so keeping either would have meant rewriting it
// into a statement that cannot fail.
//
//   - "an insert whose row names a lane somebody else now holds is reallocated, never aliased". The
//     guard was `insertRackDevice` / `insertFxDevice` checking `laneIsFree` on the ARRIVING ROW and
//     reallocating when it was taken, because — the header's own words — *"this is a public road,
//     `slice` and `device` are just arguments, and a caller handing over a lane somebody else is holding
//     would ALIAS two devices' columns silently."* A row carries no lane, neither function takes a
//     slice, and there are no columns to alias: a route names an instance id, and the id an undo puts
//     back is the id that was removed, which nothing else can be holding.
//   - "a row sitting past the count holds no lane, so a lane is never unallocatable". It made a stale
//     row BY HAND, because the mutators are careful enough never to leave one, and asserted that
//     `rackLaneIsFree` / `fxLaneIsFree` bounded by `count` rather than by the array — *"treating a stale
//     row as a claimant would make a lane unallocatable forever."* All four of those functions are
//     deleted; there is no pool to exhaust.
//
// WHAT CARRIES THE INTENT FORWARD: "ONE SEQUENCE: no two devices in either chain ever wear the same
// serial" is the modern form of "no two devices share an address", and it is a stronger claim, because a
// serial is never reissued where an address was reissued by design. The undo road's own claim — the
// device comes back under its own id with its own routes and disturbs nobody else's — is asserted
// through the bridge in "a removed rack device comes back on undo carrying the same routes".

/// A BUS CARRYING THE WHOLE SOURCE SET — what `Synth` hands every node today (the bus, commit 1).
///
/// **THE ARRAY IS HELD BESIDE THE VIEW, AND THAT IS THE POINT OF THE TYPE.** `DeviceBus` is a view;
/// a helper returning one by value would hand back a span into its own dead local, and the read would
/// be a use-after-free that no test would notice because the stack is still warm. Owning both together
/// makes the lifetime unrepresentable rather than remembered.
///
/// THE CASES BELOW WANT EVERY SOURCE PRESENT, and that is a judgement rather than a default: they are
/// about the SCALING a route gets — `modSpan`, `zones`, the declared range — and not about which
/// sources a node can see. A case about the second would build a narrower bus deliberately.
// **`WholeBus` AND FIVE BUS CASES STOOD HERE AND ARE DELETED (kill-lanes §2.4b):** the resolver's
// indifference to which source a bus carried; the bus's fill cost; a route resolving from THE BUS IT WAS
// HANDED rather than a global reading; and two cases on zoned controls reaching one zone per unit amount.
// They tested `ParamSet::rackOffsetAt` against a hand-built `DeviceBus`, and both are gone — a device's
// controls are resolved by the engine, from inputs that point at their sources' cells, with no bus
// between (`RackEngine::resolveInto`). **The zone claim was the one with a rule in it and it moved rather
// than died:** `test_rack_resolve.cpp`, "a full-depth route moves a control by its declared span — one
// zone for a zoned control, `modSpan` where declared, the range otherwise — for every device". The
// fill-cost measurement measured a mechanism that no longer runs.

TEST_CASE("the chains block never promises more routes than it writes, whatever table it is handed") {
	// A COUNT AND THE RECORDS BEHIND IT. The routings block this replaced counted in one pass and wrote
	// in a second, and a counting pass that skipped the describability test — counting a route the
	// writing pass then declined — reddened nothing (MEASURED, 119 of 119). The failure was not a wrong
	// value but a PATCH THAT WOULD NOT OPEN: the count is how far the reader walks.
	//
	// THE ONE FORMAT CANNOT DISAGREE WITH ITSELF THAT WAY — the writer counts the list it writes — and
	// the undescribable route never reaches the list: the record's door refuses a route to a lane
	// nothing holds, so the bridge never holds one to describe. Both halves are asserted here, and the
	// assertion the case exists for is kept: the reader stops exactly where the next block starts.
	//
	// THE UNDESCRIBABLE ROUTE IS MADE HONESTLY: `loadRoutesForTest` is a LOAD's road rather than a
	// gesture's, so this is a public door being handed a list the editor would not have made — a route
	// naming a device that is not here, which is exactly what a hand-edited file carries.
	namespace flt = feathers::fx::filter;

	// **THE TWO CONTAINERS DRAW FROM ONE SEQUENCE, as `ParamBridge` keeps them.** A default-constructed
	// chain and rack each start their counter at `kFirstInstance`, so building one device in each by
	// hand mints the same id twice. Production cannot reach that state; a fixture that seeds its own
	// counters can, so it seeds them the way the bridge does.
	feathers::FxChain chain;
	chain.count = 0;
	chain.nextInstanceId = feathers::kFirstFreeInstance;
	REQUIRE(feathers::addFxDevice(&chain, flt::kTraits.tag) == 0);
	feathers::ModRack rack;
	rack.count = 0;
	rack.nextInstanceId = chain.nextInstanceId;
	REQUIRE(feathers::addRackDevice(&rack, feathers::mod::dahdsr::kTraits.tag) == 0);

	const feathers::device::Descriptor* dsrDesc =
	    feathers::deviceForTag(feathers::mod::dahdsr::kTraits.tag);
	REQUIRE(dsrDesc != nullptr);
	const backplane::Destination live = backplane::Destination::control(
	    feathers::instanceOfSlot(rack, 0), dsrDesc->controlOfNthModulatableBesideEnable(0));
	// **AN END NOTHING HOLDS.** It was a LANE no device sat in — the rack has one device, so its later
	// lanes named nothing. A route names a device, so the undescribable end is an ID nobody issued:
	// past `nextInstanceId`, which is what a patch saved before a device was deleted looks like.
	const auto kGoneId = static_cast<feathers::DeviceInstanceId>(9999);
	REQUIRE(kGoneId > rack.nextInstanceId);
	REQUIRE(kGoneId > chain.nextInstanceId);
	const backplane::Destination gone = backplane::Destination::control(kGoneId, 0);

	const TestRoutes routes {
		// ONE ROUTE THAT CAN BE WRITTEN, so the block is not empty and a desync is a PARTIAL one — the
		// case that actually desynchronises a reader rather than merely producing a header.
		{ backplane::Source::core(0), live, 0.5f },
		// AND ONE THAT CANNOT.
		{ backplane::Source::core(1), gone, -0.25f },
	};

	const feathers::rack::RackPatch patch = patchOf(chain, rack, routes);
	// ONE ROUTE IN THE PATCH: the one that names something.
	CHECK(patch.route.size() == 1);

	// WRITTEN WITH A BLOCK BEHIND IT, which is what turns "the reader stopped in the wrong place"
	// from an invisible offset into a failed read of something identifiable. The same idiom the
	// block-order cases use.
	MemoryStream stream;
	{
		clap_ostream out = stream.out();
		REQUIRE(feathers::saveChainsBlock(&out, patch));
		REQUIRE(saveSoundHues(&out, SoundHues {}));
	}

	feathers::rack::RackPatch parsed;
	bool present = false;
	clap_istream in = stream.in();
	REQUIRE(feathers::loadChainsBlock(&in, &parsed, &present));
	REQUIRE(present);
	const Reloaded back = reload(parsed);
	CHECK(back.refused.count == 0);

	// THE DESCRIBABLE ROUTE SURVIVED and the undescribable one is simply absent — never stored, which is
	// the honest outcome for a route naming a slot that holds nothing.
	CHECK(amountIn(back.routes, backplane::Source::core(0), live) == doctest::Approx(0.5f));
	CHECK(amountIn(back.routes, backplane::Source::core(1), gone) == doctest::Approx(0.0f));
	CHECK(back.routes.size() == 1);

	// AND THE READER STOPPED WHERE THE NEXT BLOCK STARTS. This is the assertion the case exists for:
	// a count larger than the records makes this read fail, and it fails on a tag that names the
	// block rather than on an offset nobody can interpret.
	SoundHues hues;
	bool huesPresent = false;
	REQUIRE(loadSoundHues(&in, &hues, &huesPresent));
	CHECK(huesPresent);
	// AND THE STREAM IS EXACTLY USED UP, so a reader that stopped SHORT is caught by the same case
	// as one that overran.
	CHECK(stream.readPos == stream.bytes.size());
}

TEST_CASE("a route arm in each chain resolves by the device's id, on both sides of a save") {
	// **THE QUESTION THE NUMBER-TEST COULD NOT ASK (BX step 3), AND WHY IT HAS NO SUBJECT NOW.** The
	// routings block named a device end by the LANE it found the device at, so a lane two chains both
	// claimed was a route that could mean either device. This case then asked what a patch whose devices
	// claimed ONE lane did. **The file names no lane at all now** (lanes are gone: the
	// system is a graph, addresses are not sequential, and ordering comes only from the graph), so there
	// is nothing two devices can claim; the ambiguity left is an ID two devices wear, which is
	// "a stored route naming an id BOTH chains hold is refused rather than arbitrated".
	//
	// WHAT IS LEFT IS THE CONTROL IT ALWAYS HAD, and it is worth keeping: one arm ending at an fx device and
	// one starting at a strip device, each resolved by id through a real save.
	namespace flt = feathers::fx::filter;

	// **THE TWO CONTAINERS DRAW FROM ONE SEQUENCE, as `ParamBridge` keeps them.** A default-constructed
	// chain and rack each start their counter at `kFirstInstance`, so building one device in each by
	// hand mints the same id twice. Production cannot reach that state; a fixture that seeds its own
	// counters can, so it seeds them the way the bridge does.
	feathers::FxChain chain;
	chain.count = 0;
	chain.nextInstanceId = feathers::kFirstFreeInstance;
	REQUIRE(feathers::addFxDevice(&chain, flt::kTraits.tag) == 0);
	feathers::ModRack rack;
	rack.count = 0;
	rack.nextInstanceId = chain.nextInstanceId;
	REQUIRE(feathers::addRackDevice(&rack, feathers::mod::dahdsr::kTraits.tag) == 0);

	const feathers::DeviceInstanceId fxId = feathers::instanceOfFxSlot(chain, 0);
	const feathers::DeviceInstanceId rackId = feathers::instanceOfSlot(rack, 0);
	const feathers::device::Descriptor* fxDesc = feathers::deviceForTag(flt::kTraits.tag);
	REQUIRE(fxDesc != nullptr);

	// **A ROUTE FOR EACH ARM** — one ending at the fx device, one starting at the rack device (only a
	// modulator publishes an output) and aimed at a plain parameter no chain owns — so a loader that got
	// either arm's resolution wrong is caught by its own line.
	const int fine = feathers::modDestinationIndexOf(feathers::ParamId::FineTune);
	REQUIRE(fine >= 0);
	const backplane::Destination plainDestination =
	    backplane::Destination::core(static_cast<size_t>(fine));
	const backplane::Destination intoFx =
	    backplane::Destination::control(fxId, fxDesc->controlOfNthModulatableBesideEnable(0));
	const backplane::Source outOfRack = backplane::Source::output(rackId, 0);
	const TestRoutes routes = sortedRoutes({
	    { backplane::Source::core(0), intoFx, 0.5f },
	    { outOfRack, plainDestination, -0.25f },
	});
	const feathers::rack::RackPatch honest = patchOf(chain, rack, routes);
	REQUIRE(honest.route.size() == 2);

	// **READ BACK BY NAME (kill-lanes step 5), WHICH IS THE CLAIM ITSELF.** This found each device's
	// landing POSITION by id and then minted the coordinate that position answers to — a re-derivation
	// that existed only because the table was addressed positionally. A route names the device, so the
	// two ends written above are the two ends asked about, and "wherever each device landed" is stated
	// as its own assertion rather than folded into the lookup.
	const Reloaded back = reload(throughBytes(honest));
	CHECK(back.refused.count == 0);
	CHECK(feathers::slotOfFxInstance(back.chain, fxId) < feathers::fxChainLength(back.chain));
	CHECK(feathers::slotOfInstance(back.rack, rackId) < back.rack.count);

	// Both routes survive the round trip AT THEIR OWN VALUES, and nothing else was routed — a route
	// arriving somewhere else as well would be the mis-wiring.
	CHECK(amountIn(back.routes, backplane::Source::core(0), intoFx) == doctest::Approx(0.5f));
	CHECK(amountIn(back.routes, outOfRack, plainDestination) == doctest::Approx(-0.25f));
	CHECK(back.routes.size() == 2);
}

TEST_CASE("a file's route amount is clamped to the declared reach, the same as a hand's") {
	// TWO DOORS ONTO ONE TABLE, AND ONLY ONE WAS TESTED. `ParamBridge::setRouteAmount` clamps to
	// ±kMaxRouteAmount and has a case for it ("an amount past the declared reach is clamped rather
	// than taken"); the file's loader clamped the same way and had none (MEASURED then: deleting its
	// clamp reddened nothing, 120 of 120). The clamp is the record's own door now (`ChainRecord::
	// setRoute`, ±device::kMaxAmount), which both roads go through.
	//
	// WHAT AN UNCLAMPED AMOUNT COSTS is not the resolved value — `resolved()` clamps the sum to the
	// parameter's own range, so the sound is safe. It is everything drawn FROM the amount:
	// `Knob::modRing` draws an arc scaled by it, and `kMaxRouteAmount` is the declared reach that
	// arc is drawn against ("an amount reaches twice the destination's whole range... which is what
	// lets one ring on one face be drawn from the amount without asking what it is attached to").
	// It also survives a save, because the writer describes what it finds faithfully — so one edited
	// byte would be permanent.
	//
	// THE ROUTES ARE NAMED BY HAND, because this is a patch the writer cannot produce: its own door
	// clamps.
	const int fine = feathers::modDestinationIndexOf(feathers::ParamId::FineTune);
	REQUIRE(fine >= 0);

	TestRoute high = fromMacro(0);
	toParam(high, feathers::ParamId::FineTune);
	high.amount = 1.0e6f;
	TestRoute low = fromMacro(1);
	toParam(low, feathers::ParamId::Path);
	low.amount = -1.0e6f;
	// AND ONE INSIDE THE REACH, which is the control: a loader that clamped EVERYTHING to the bound
	// would satisfy the two rows above and be just as wrong.
	TestRoute ordinary = fromMacro(2);
	toParam(ordinary, feathers::ParamId::PosX);
	ordinary.amount = 0.375f;

	const Reloaded read = reload(throughBytes(patchWithRoutes({ high, low, ordinary })));
	const TestRoutes& routes = read.routes;
	const feathers::RouteRefusals& refused = read.refused;
	// ACCEPTED, NOT REFUSED. An amount past the reach is a legal number badly chosen, not a route
	// naming something that is not there — the refusal road is for the latter.
	CHECK(refused.count == 0);

	const int path = feathers::modDestinationIndexOf(feathers::ParamId::Path);
	const int posx = feathers::modDestinationIndexOf(feathers::ParamId::PosX);
	REQUIRE(path >= 0);
	REQUIRE(posx >= 0);
	const backplane::Source macro0 = backplane::Source::core(0);
	const backplane::Destination atFine = backplane::Destination::core(static_cast<size_t>(fine));
	CHECK(amountIn(routes, macro0, atFine) == doctest::Approx(feathers::kMaxRouteAmount));
	CHECK(amountIn(routes, backplane::Source::core(1),
	               backplane::Destination::core(static_cast<size_t>(path)))
	      == doctest::Approx(-feathers::kMaxRouteAmount));
	CHECK(amountIn(routes, backplane::Source::core(2),
	               backplane::Destination::core(static_cast<size_t>(posx)))
	      == doctest::Approx(0.375f));

	// AND THE TWO DOORS AGREE, said as a comparison rather than as two numbers that happen to match:
	// a build that moved one bound and not the other would draw rings the loader cannot reproduce.
	feathers::ParamBridge bridge;
	bridge.setRouteAmount(macro0, atFine, 1.0e6f);
	CHECK(bridge.routeAmount(macro0, atFine) == doctest::Approx(amountIn(routes, macro0, atFine)));
}

TEST_CASE("a route the file records at zero is not a route, and is not reported as a refusal") {
	// The loader skips a route whose amount is exactly zero, before it tries to resolve or judge it
	// (`applyPatchToRecord`; the routings block's loader before it). MEASURED before this case existed:
	// deleting that skip reddened NOTHING, 121 of 121.
	//
	// AND THE GUARD IS NOT INERT — its failure is a LIE TO THE PLAYER rather than a silent nothing.
	// A zero amount is not a route (`kRouteUnroutedBelow`: "a route dragged back to nothing is not a
	// route at 0 %, it is gone"), so setting it changes no table. But without the skip the record
	// walks on into `maySourceRouteTo`, and a pair the rules refuse — a weight source pointed at the
	// puck, by design — is COUNTED as a refusal and latched as the first one. `Gui::Impl::tick`
	// then says "Dropped route: Sound A weight -> Position X" in the tray about a route that was
	// never a route, on opening a patch, with nothing dropped.
	//
	// THE ROUTES ARE NAMED BY HAND because the writer cannot produce one: a zero is not a route, so the
	// record never holds one to write — which is why the reader's skip has never had a producer to meet.
	const size_t weight = feathers::weightSourceOf(0);
	const int posX = feathers::modDestinationIndexOf(feathers::ParamId::PosX);
	REQUIRE(posX >= 0);
	const backplane::Source weightEnd = backplane::Source::core(weight);
	const backplane::Destination atPosX = backplane::Destination::core(static_cast<size_t>(posX));
	// THE PAIR REALLY IS ONE THE RULES REFUSE, asked of the one authority the loader asks rather than
	// assumed — otherwise this case would be about a route that simply resolved and landed. It used to
	// ask `retiredBandRule::maySourceRouteTo`, which is the band rule the product no longer has
	// (kill-lanes step 4b); `ParamBridge::mayRoute` is the gate, and both these ends are host ends, so a
	// fresh bridge is the whole fixture.
	REQUIRE_FALSE(feathers::ParamBridge {}.mayRoute(weightEnd, atPosX));

	TestRoute zero = fromWeight(0);
	toParam(zero, feathers::ParamId::PosX);
	zero.amount = 0.0f;
	// AND A LEGAL ROUTE BESIDE IT, so the block is not a single record and the reader is seen to
	// carry on past the skipped one rather than to stop at it.
	TestRoute real = fromMacro(0);
	toParam(real, feathers::ParamId::FineTune);
	real.amount = 0.5f;

	const Reloaded read = reload(throughBytes(patchWithRoutes({ zero, real })));
	const TestRoutes& routes = read.routes;
	const feathers::RouteRefusals& refused = read.refused;

	// NOTHING WAS REFUSED, which is the whole claim: the tray must stay silent.
	CHECK(refused.count == 0);
	// The zero left no route, and the legal one landed — the reader did not stop at the skip.
	CHECK(amountIn(routes, weightEnd, atPosX) == doctest::Approx(0.0f));
	const int fine = feathers::modDestinationIndexOf(feathers::ParamId::FineTune);
	REQUIRE(fine >= 0);
	CHECK(amountIn(routes, backplane::Source::core(0),
	               backplane::Destination::core(static_cast<size_t>(fine)))
	      == doctest::Approx(0.5f));

	SUBCASE("and the same pair at a real amount IS refused and counted") {
		// THE CONTROL, and without it "nothing is ever refused" would satisfy every line above. The
		// refusal road has to be reachable through this same block for the silence to mean anything.
		TestRoute loud = fromWeight(0);
		toParam(loud, feathers::ParamId::PosX);
		loud.amount = 0.8f;

		const Reloaded second = reload(throughBytes(patchWithRoutes({ loud })));
		const TestRoutes& table = second.routes;
		const feathers::RouteRefusals& said = second.refused;
		CHECK(said.count == 1);
		// AND IT NAMES THE PAIR, since a refusal that resolved reports both ends — the tray prints
		// them, so they are part of the claim rather than bookkeeping. BY ENDPOINT since kill-lanes
		// step 3b: two host ends, which is what a weight and a parameter are, at the same indices the
		// dense coordinates used. This is the exact pair the banner above quotes the tray as printing
		// ("Sound A weight -> Position X"), so it is also what stops that sentence drifting from the
		// thing `Gui::Impl::tick` actually builds.
		CHECK(said.firstSource == weightEnd);
		CHECK(said.firstDestination == atPosX);
		CHECK(amountIn(table, weightEnd, atPosX) == doctest::Approx(0.0f));
	}
}

TEST_CASE("a patch cannot carry a NaN into a device control") {
	// IT COULD, AND NOTHING DOWNSTREAM COULD STOP IT. The lane's old device record read control fields
	// as raw floats and checked nothing, while the ROUTE loader two hundred lines away had always checked
	// `std::isfinite(record.amount)` before taking one — same file, same patch, two lanes, one of them
	// distrusting the bytes. The asymmetry was the tell. The one format's loader (`applyPatchToRecord`)
	// carries the fix: a non-finite base is the control's DECLARED DEFAULT.
	//
	// AND THE DEVICES' OWN CLAMPS CANNOT COVER FOR IT, which is the part worth knowing: every device
	// clamps its row on the way in and shaper's comment says exactly why — *"A row is whatever
	// reached the transport — a host's automation, a file..."* — but **`std::clamp(NaN, lo, hi)`
	// returns NaN**. It is `v < lo ? lo : hi < v ? hi : v`, and both comparisons are false for a NaN.
	// Measured before the fix: the NaN survived the round trip AND survived `stateOf`, arriving in
	// the live Settings, which is the audio path.
	//
	// SHAPER RATHER THAN A FIXTURE DEVICE, because the question is about a real declaration: the fix
	// substitutes the control's DECLARED DEFAULT, and a device with no declarations would not have
	// one to check.
	namespace shp = feathers::fx::shaper;
	feathers::FxChain chain;
	const size_t slot = feathers::addFxDevice(&chain, shp::kTraits.tag);
	REQUIRE(slot == 0);

	const auto knee = static_cast<size_t>(shp::Param::Knee);
	// THE NaN GOES INTO THE PATCH, not into a bridge: a bridge's record refuses a non-finite row at its
	// own door, so the only road a NaN has is a file — which is this one, written and read for real.
	feathers::rack::RackPatch patch = patchOf(chain, feathers::ModRack {});
	size_t poisoned = 0;
	for (auto& d : patch.device) {
		if (d.tag == feathers::rack::patchTagOf(shp::kTraits.tag)) {
			std::vector<float> row = feathers::test::rowOf(d, shp::kDescriptor);
			REQUIRE(row.size() > knee);
			row[knee] = std::numeric_limits<float>::quiet_NaN();
			feathers::test::setRow(d, shp::kDescriptor, row);
			++poisoned;
		}
	}
	REQUIRE(poisoned == 1);

	const feathers::FxChain loaded = reload(throughBytes(patch)).chain;
	REQUIRE(loaded.count == 1);

	// THE FIELD ITSELF IS FINITE AGAIN — the loader's own half of the claim.
	const float field = fxDeviceAtPosition(loaded, 0).row.field[knee];
	CHECK(std::isfinite(field));
	// AND IT IS THE DECLARED DEFAULT rather than an arbitrary finite number, which is the difference
	// between "sanitised" and "silently zeroed": zero is a different sound for most controls.
	CHECK(field == doctest::Approx(shp::kParamDescs[knee].def));
	// THE DEVICE'S OWN VIEW OF THE ROW, copied the way the card copies it — `stateOf` takes the
	// device's fixed-size Row, not the transport's DeviceRow.
	shp::Row row {};
	const feathers::DeviceRow& loadedRow = fxDeviceAtPosition(loaded, 0).row;
	const size_t fields = std::min(loadedRow.fieldsPresent, row.size());
	for (size_t i = 0; i < fields; ++i) {
		row[i] = loadedRow.field[i];
	}
	const shp::Settings s = shp::stateOf(row, fields);
	// AND THE DEVICE'S LIVE VIEW IS FINITE, which is the half that matters: this is what reaches the
	// audio path, and no clamp between here and there could have rescued it.
	CHECK(std::isfinite(s.knee));
	CHECK(s.knee == doctest::Approx(shp::kParamDescs[knee].def));
}

TEST_CASE("a patch cannot carry a non-finite TURN, on either orientation block") {
	// THE THIRD ROAD OF THE SAME SHAPE, found the same way: one lane of a loader distrusts its bytes
	// and the lane beside it does not. `setSoundOrientation` refuses a non-finite rotate — it has
	// always done — but a PATCH does not go through it. `restoreOrientation` stores the global turn
	// straight into its atomic, and the per-sound restore stores each plane straight into
	// `soundPlane_`, so the loaders are the only door either road has.
	//
	// AND THE COUNT BESIDE IT WAS ALREADY GUARDED, which is what makes this an omission rather than a
	// policy: `!(count >= 0.0f)` is false for a NaN and the comment above it explains the technique
	// in detail. The VALUE never got the same treatment.
	//
	// BOTH BLOCKS, because they are two loaders with two copies of the same three lines — the global
	// orientation and the per-sound planes — and the fix had to go in both.
	const float nan = std::numeric_limits<float>::quiet_NaN();

	SUBCASE("the global orientation") {
		SliceOrientationPin saved;
		saved.pinned = true;
		saved.generator.rotate = nan;

		MemoryStream stream;
		clap_ostream out = stream.out();
		REQUIRE(saveOrientation(&out, saved));

		SliceOrientationPin loaded;
		bool present = false;
		clap_istream in = stream.in();
		REQUIRE(loadOrientation(&in, &loaded, &present));
		CHECK(present);
		// THE TURN IS FINITE AND IT IS THE DEFAULT — dropped, not zeroed by accident.
		CHECK(std::isfinite(loaded.generator.rotate));
		CHECK(loaded.generator.rotate == doctest::Approx(SliceOrientationPin {}.generator.rotate));
		// AND THE REST OF THE BLOCK SURVIVED IT: what is dropped is the value, not the record.
		CHECK(loaded.pinned);
	}

	SUBCASE("a sound's own plane") {
		SoundOrientations saved;
		saved.pin[1].pinned = true;
		saved.pin[1].generator.rotate = nan;
		// A REAL NEIGHBOUR IN THE SAME BLOCK, so "it dropped everything" cannot pass for the fix.
		saved.pin[2].pinned = true;
		saved.pin[2].generator.rotate = 0.375f;

		MemoryStream stream;
		clap_ostream out = stream.out();
		REQUIRE(saveSoundOrientations(&out, saved));

		SoundOrientations loaded;
		bool present = false;
		clap_istream in = stream.in();
		REQUIRE(loadSoundOrientations(&in, &loaded, &present));
		CHECK(present);
		CHECK(std::isfinite(loaded.pin[1].generator.rotate));
		CHECK(loaded.pin[1].generator.rotate
		      == doctest::Approx(SliceOrientationPin {}.generator.rotate));
		CHECK(loaded.pin[1].pinned);
		CHECK(loaded.pin[2].generator.rotate == doctest::Approx(0.375f));
	}
}

TEST_CASE("a spoilt residue is dropped as a whole fold, not half-kept") {
	// THE FOURTH ROAD OF THE SAME SHAPE, and the first where the RULE was already written down and
	// only one of the two setters obeyed it. `ParamBridge::setSoundResidue(slot, banked)` refuses a
	// residue with any non-finite coordinate, all or nothing, and says why: *"a residue is one banked
	// difference spread across 88 coordinates, and storing the finite half of a spoilt fold would
	// leave the sound standing at a point that is neither where it was nor where the turn would have
	// put it."* The WHOLE-SET `setSoundResidue(const SoundResidue&)` — the one a patch load takes —
	// stores every value without looking, and `loadSoundResidue` read them as raw doubles.
	//
	// SO THE ASSERTION IS ALL-OR-NOTHING TOO, not merely "finite": a fix that clamped or zeroed the
	// one bad coordinate would satisfy an isfinite check and leave exactly the half-fold the setter's
	// comment refuses.
	SoundResidue saved;
	saved.banked[1].morph.offset[0] = 0.125;
	saved.banked[1].morph.offset[1] = std::numeric_limits<double>::quiet_NaN();
	saved.banked[1].stair.offset[0] = -0.5;
	// A WHOLE SOUND THAT IS FINE, in the same block, so "it dropped everything" cannot pass for it.
	saved.banked[2].morph.offset[0] = 0.75;

	MemoryStream stream;
	clap_ostream out = stream.out();
	REQUIRE(saveSoundResidue(&out, saved));

	SoundResidue loaded;
	bool present = false;
	clap_istream in = stream.in();
	REQUIRE(loadSoundResidue(&in, &loaded, &present));
	CHECK(present);

	// THE SPOILT SOUND KEEPS NO FOLD AT ALL — including the two coordinates that were perfectly good.
	CHECK(loaded.banked[1] == PlaneResidue {});
	// AND ITS NEIGHBOUR IS UNTOUCHED, to the value.
	CHECK(loaded.banked[2].morph.offset[0] == doctest::Approx(0.75));
}

TEST_CASE("two automods in one rack keep two state blobs") {
	namespace amd = feathers::fx::automod;

	// **PER-INSTANCE, WHICH IS THE WHOLE ARGUMENT FOR THE BLOB LIVING IN THE ROW.** A top-level state
	// block would have had to re-derive the device-instance association the row gets for free — and
	// would have been wrong here, silently, for anyone running two of the same device. This is that
	// claim asserted rather than argued.
	feathers::FxChain chain;
	REQUIRE(feathers::addFxDevice(&chain, amd::kTraits.tag) == 0);
	REQUIRE(feathers::addFxDevice(&chain, amd::kTraits.tag) == 1);

	const size_t first = amd::kFoldedTravelFirst;
	for (size_t i = 0; i < amd::kFoldedTravelFloats; ++i) {
		// TWO DIFFERENT JOURNEYS, distinguishable per coordinate so a swap or a shared buffer shows.
		chain.device[0].row.field[first + i] = 0.125f + static_cast<float>(i);
		chain.device[1].row.field[first + i] = -3.5f - static_cast<float>(i) * 2.0f;
	}
	const feathers::DeviceRow a = fxDeviceAtPosition(chain, 0).row;
	const feathers::DeviceRow b = fxDeviceAtPosition(chain, 1).row;
	REQUIRE_FALSE(a == b);

	const feathers::FxChain back = reload(throughBytes(patchOf(chain, feathers::ModRack {}))).chain;
	REQUIRE(back.count == 2);
	const feathers::DeviceRow& backA = fxDeviceAtPosition(back, 0).row;
	const feathers::DeviceRow& backB = fxDeviceAtPosition(back, 1).row;

	// EACH ROW WHOLE, AND STILL DIFFERENT FROM THE OTHER. The second half is what a shared or
	// last-writer-wins blob would fail while the first half passed.
	CHECK(backA == a);
	CHECK(backB == b);
	CHECK_FALSE(backA == backB);

	// AND EVERY FLOAT OF EACH BLOB, named individually so a failure says WHICH coordinate was lost
	// rather than that two rows differ.
	for (size_t i = 0; i < amd::kFoldedTravelFloats; ++i) {
		CAPTURE(i);
		CHECK(backA.field[first + i] == a.field[first + i]);
		CHECK(backB.field[first + i] == b.field[first + i]);
	}
}

TEST_CASE("toDeviceRow carries a device's state blob, which nothing else exercises") {
	// **THIS CASE EXISTS BECAUSE A KNOWN-BAD CAME BACK SILENT.** Restoring `toDeviceRow`'s truncation
	// (`fieldsPresent = N`) changed no test at all: automod is the only device with a blob, and it does
	// not go through `toDeviceRow` — the devices that do (dahdsr, mseg, filter, harm) all declare zero
	// extra state. So the fix there was correct and completely unexercised, which is the same thing as
	// untested.
	//
	// **A MUTATION THAT KILLS NOTHING IS EVIDENCE ABOUT THE SUITE, NOT ABOUT THE CODE**, and the
	// evidence here was "this road has no coverage". Asked directly rather than waiting for a device to
	// grow a blob and discover it.
	constexpr size_t kParams = 4;
	const std::array<float, kParams> row { 1.0f, 2.0f, 3.0f, 4.0f };
	const std::array<float, 3> blob { 10.0f, 20.0f, 30.0f };
	const feathers::DeviceIdentity identity { 7 };

	const feathers::DeviceRow carried = feathers::toDeviceRow("TEST", identity, row, blob);

	// THE ROW RULE: params plus blob, never more, never less.
	CHECK(carried.fieldsPresent == kParams + blob.size());
	for (size_t i = 0; i < kParams; ++i) {
		CAPTURE(i);
		CHECK(carried.field[i] == row[i]);
	}
	for (size_t i = 0; i < blob.size(); ++i) {
		CAPTURE(i);
		CHECK(carried.field[kParams + i] == blob[i]);
	}

	// AND WITHOUT A BLOB IT IS EXACTLY WHAT IT ALWAYS WAS — the promise that widening the contract
	// costs nothing to a device not using it, asserted on this road too and not only on the stream.
	const feathers::DeviceRow plain = feathers::toDeviceRow("TEST", identity, row);
	CHECK(plain.fieldsPresent == kParams);
	for (size_t i = kParams; i < feathers::kMaxDeviceFields; ++i) {
		CAPTURE(i);
		CHECK(plain.field[i] == 0.0f);
	}
}

TEST_CASE("the modulated-rack channel is DAHDSR-shaped, which is why BK is not one const reader") {
	// **A GAP PINNED WHERE IT WILL BE FOUND, not left in a conversation.**
	//
	// BK is BE's other half: Feathers' own cards still draw the BASE, because
	// `DeviceHost::modulatedFieldAt` defaults to `fieldAt` and only `RackEngine` overrode it. The
	// obvious next step looked like one const reader — the bridge ALREADY carries modulated rack rows,
	// written once a block by `publishModulated`, so `BridgeDeviceHost::modulatedFieldAt` would
	// just read them.
	//
	// **IT CANNOT, AND THIS CASE IS WHY.** That channel does not carry a slot's OWN row. It carries
	// `mod::dahdsr::rowOf(rack.dahdsrAt(slot))`, and `dahdsrAt` checks the tag and answers `{}` for
	// anything that is not an envelope. So an MSEG slot publishes DEFAULT DAHDSR SETTINGS: not
	// garbage, which would have been noticed, but a plausible row in the wrong control space. A
	// generic reader over it would draw an MSEG's arcs from an envelope's defaults.
	//
	// THE SAME SHAPE THIS SUITE'S NEIGHBOUR ALREADY PAID FOR — `modulatedMsegSettings` once walked the
	// envelope's descriptor for an MSEG's controls, so a hand aiming a macro at Rate moved how many
	// segments the shape had. One device type's layout standing in for another's.
	//
	// **WHEN BK LANDS, THIS CASE CHANGES.** It is written to be the thing that has to be edited: the
	// channel must carry the device's own row, and the storage is already sized for it
	// (`envModulated_` is `kMaxRackEnvelopes * kMaxDeviceFields`, while writer and reader both stride
	// by `kDahdsrFieldCount`). Until then, what is true is what is asserted here.
	namespace dsr = feathers::mod::dahdsr;
	namespace msg = feathers::mod::mseg;

	feathers::ParamBridge bridge;

	feathers::ModRack rack = feathers::defaultModRack();
	rack.count = 2;

	// SLOT 0: AN MSEG, with settings nothing would produce by accident.
	msg::Settings shape;
	shape.timeScale = 1.75f;
	shape.count = 4;
	rack.setMsegAt(0, shape);

	// SLOT 1: AN ENVELOPE, so the case can tell "the channel is broken" from "the channel is typed".
	dsr::Settings env;
	env.sustain = 0.125f;
	rack.setDahdsrAt(1, env);

	bridge.publishModulated(rack, feathers::FxChain {}, feathers::RackPlayheads {}, feathers::mod::TransportClock::unknown());

	const dsr::Settings defaults;

	SUBCASE("an envelope slot carries its own values — the channel works for what it was built for") {
		// FIRST, so the assertion below cannot pass on a channel that publishes nothing at all.
		const dsr::Settings got = bridge.modulatedEnvelope(1);
		CHECK(got.sustain == doctest::Approx(0.125f));
		CHECK(got.sustain != doctest::Approx(defaults.sustain));
	}

	SUBCASE("an MSEG slot reads back as a DEFAULT envelope, carrying none of the MSEG's own state") {
		const dsr::Settings got = bridge.modulatedEnvelope(0);
		// Every field is the envelope's declared default, because `dahdsrAt` refused the tag and the
		// publisher encoded the `{}` it got back.
		CHECK(got.attack == doctest::Approx(defaults.attack));
		CHECK(got.decay == doctest::Approx(defaults.decay));
		CHECK(got.sustain == doctest::Approx(defaults.sustain));
		CHECK(got.release == doctest::Approx(defaults.release));
		// AND THE MSEG'S OWN NUMBERS ARE NOWHERE IN IT. 1.75 was chosen to be findable: if a future
		// publisher started carrying the device's row without this case being updated, the timeScale
		// would land in whichever envelope field shares its ordinal and this would go red.
		CHECK(got.attack != doctest::Approx(1.75f));
		CHECK(got.decay != doctest::Approx(1.75f));
		CHECK(got.sustain != doctest::Approx(1.75f));
		CHECK(got.release != doctest::Approx(1.75f));
	}
}

TEST_CASE("the bridge answers a rack control's base and where modulation left it, per field (BK, BL)") {
	// **BE'S OTHER HALF, AT THE SEAM FEATHERS' OWN CARDS READ THROUGH.**
	//
	// `DeviceControl::display()` asks its host for `modulatedFieldAt`. `RackEngine` has answered it
	// since BE; `ModRackDeviceHost` did not, so `DeviceHost`'s default kicked in — it returns
	// `fieldAt`, the BASE — and a modulator card's ring sat still however much was routed at it. These
	// three readers are what the host now forwards to, one relaxed load each.
	//
	// **AND `fieldAt` USED TO COPY A WHOLE ModRack PER FIELD (BL).** inside measured the fx twin of
	// that at 5.85 us for one card and 14-40 us for eight; the rack host had the identical defect and
	// was not in the finding, so fixing only the fx half would have read as done.
	//
	// WHAT THIS CASE CANNOT REACH, said out loud rather than left implied: that
	// `ModRackDeviceHost` actually OVERRIDES `modulatedFieldAt`. Delete the override and the default
	// silently answers the base again, and nothing here would go red. That case needs the host, the
	// host needs visage, and the only suite with both is test_gui_widgets.cpp — frozen for inside's
	// split. It is owed there, beside the modulator-strip enable cases already parked for it.
	namespace dsr = feathers::mod::dahdsr;

	feathers::ParamBridge bridge;
	const auto sustain = static_cast<size_t>(dsr::Param::Sustain);

	// THE BASE — what the player set, on the raw channel.
	feathers::ModRack raw = feathers::defaultModRack();
	dsr::Settings set;
	set.sustain = 0.25f;
	raw.setDahdsrAt(0, set);
	bridge.restoreModRack(raw);

	// AND WHERE THE ENGINE ARRIVED, on the modulated channel. A separate rack rather than a mutation
	// of the first, because that is what the engine hands over: `modulatedRack()` is its own snapshot.
	feathers::ModRack moved = feathers::defaultModRack();
	dsr::Settings after = set;
	after.sustain = 0.75f;
	moved.setDahdsrAt(0, after);
	bridge.publishModulated(moved, feathers::FxChain {}, feathers::RackPlayheads {}, feathers::mod::TransportClock::unknown());

	CHECK(bridge.rackFieldAt(0, sustain) == doctest::Approx(0.25f));
	CHECK(bridge.rackModulatedFieldAt(0, sustain) == doctest::Approx(0.75f));
	// AND THEY DIFFER, or both lines above would pass on a bridge answering one value twice — which
	// is precisely the defaulted behaviour this replaces.
	CHECK(bridge.rackFieldAt(0, sustain) != doctest::Approx(bridge.rackModulatedFieldAt(0, sustain)));
	CHECK(bridge.rackFieldsPresentAt(0) > sustain);

	SUBCASE("the base is not disturbed by the modulated publish — a reading is never an edit") {
		bridge.publishModulated(moved, feathers::FxChain {}, feathers::RackPlayheads {}, feathers::mod::TransportClock::unknown());
		bridge.publishModulated(moved, feathers::FxChain {}, feathers::RackPlayheads {}, feathers::mod::TransportClock::unknown());
		CHECK(bridge.rackFieldAt(0, sustain) == doctest::Approx(0.25f));
	}

	SUBCASE("a control past the modulated row's width reads the base, not zero") {
		// device_row.h's rule, and the one `RackEngine::modulatedFieldAt` already follows: a control
		// the modulated row does not reach is UNMODULATED, not silent. Answering zero would pin every
		// such control's arc at the bottom of its travel.
		const size_t beyond = feathers::kMaxDeviceFields - 1;
		CHECK(bridge.rackModulatedFieldAt(0, beyond) == doctest::Approx(bridge.rackFieldAt(0, beyond)));
	}

	SUBCASE("out of range is zero rather than a read past the end") {
		CHECK(bridge.rackFieldAt(feathers::kMaxRackEnvelopes, 0) == doctest::Approx(0.0f));
		CHECK(bridge.rackModulatedFieldAt(feathers::kMaxRackEnvelopes, 0) == doctest::Approx(0.0f));
		CHECK(bridge.rackFieldsPresentAt(feathers::kMaxRackEnvelopes) == 0);
	}
}

TEST_CASE("the bridge answers an FX control's base and where modulation left it (BK, fx half)") {
	// **THE LANE'S TWIN OF THE RACK CASE ABOVE, and the last piece of BE's road.**
	//
	// The engine end is already in: `applyParams` builds a whole modulated FxChain every block to
	// drive `fxRunner_.sync` — it has to, or a route to a cutoff would not be heard — and it used to
	// drop it as a local. `Synth::modulatedFxChain()` keeps it. This is the channel that carries it to
	// the editor, so an fx card's ring draws the ENGINE's arithmetic rather than a second copy of
	// `rackOffsetAt` run on the far side of a thread boundary, which is what synth.h warns off.
	//
	// PUBLISHED ON THE SAME CALL AS THE RACK'S, by a required parameter rather than a second
	// function — the bridge's own rule for the three sites that hand over a snapshot: "a required
	// parameter beats a remembered convention." Adding it turned every one of those sites into a
	// compile error until it was passed, which is the property that was wanted.
	namespace filter = feathers::fx::filter;

	feathers::ParamBridge bridge;

	size_t cutoff = filter::kDescriptor.params.size();
	for (size_t i = 0; i < filter::kDescriptor.params.size(); ++i) {
		if (std::string(filter::kDescriptor.params[i].name).find("Cutoff") != std::string::npos) {
			cutoff = i;
			break;
		}
	}
	REQUIRE(cutoff < filter::kDescriptor.params.size());

	// THE BASE — what the player set, on the raw channel.
	feathers::FxChain raw;
	const size_t slot = feathers::addFxDevice(&raw, filter::kTraits.tag);
	REQUIRE(slot < raw.device.size());
	const float base = raw.device[slot].row.field[cutoff];
	bridge.restoreFxChain(raw);

	// AND WHERE THE ENGINE ARRIVED. A separate chain, because `modulatedFxChain()` is its own
	// snapshot and not a mutation of the one the editor published.
	feathers::FxChain moved = raw;
	const float after = base + 1.5f;
	moved.device[slot].row.field[cutoff] = after;
	bridge.publishModulated(feathers::defaultModRack(), moved, feathers::RackPlayheads {}, feathers::mod::TransportClock::unknown());

	CHECK(bridge.fxFieldAt(slot, cutoff) == doctest::Approx(base));
	CHECK(bridge.fxModulatedFieldAt(slot, cutoff) == doctest::Approx(after));
	// AND THEY DIFFER, or both lines pass on a bridge answering one value twice — the defaulted
	// behaviour this replaces.
	CHECK(bridge.fxFieldAt(slot, cutoff) != doctest::Approx(bridge.fxModulatedFieldAt(slot, cutoff)));

	SUBCASE("the base survives repeated modulated publishes — a reading is never an edit") {
		bridge.publishModulated(feathers::defaultModRack(), moved, feathers::RackPlayheads {}, feathers::mod::TransportClock::unknown());
		bridge.publishModulated(feathers::defaultModRack(), moved, feathers::RackPlayheads {}, feathers::mod::TransportClock::unknown());
		CHECK(bridge.fxFieldAt(slot, cutoff) == doctest::Approx(base));
	}

	SUBCASE("a control past the modulated row's width reads the base, not zero") {
		const size_t beyond = feathers::kMaxDeviceFields - 1;
		CHECK(bridge.fxModulatedFieldAt(slot, beyond) == doctest::Approx(bridge.fxFieldAt(slot, beyond)));
	}

	SUBCASE("a lane that has shrunk stops reporting the departed device's fields") {
		// THE SHRINK TAIL. The publish writes the row's width and zeroes down to the width it last
		// published; without that second loop a narrower device would inherit its predecessor's
		// numbers under its own control indices.
		bridge.publishModulated(feathers::defaultModRack(), feathers::FxChain {},
		                            feathers::RackPlayheads {}, feathers::mod::TransportClock::unknown());
		CHECK(bridge.fxModulatedFieldAt(slot, cutoff) == doctest::Approx(bridge.fxFieldAt(slot, cutoff)));
	}
}


TEST_CASE("a saved-and-reloaded patch whose rack is then emptied leaves NO route behind") {
	// THE DEFECT: a Feathers with modulations, its track duplicated in the DAW, then edited to have no
	// mod sources — and the modulation persisted. Duplicating a track is a SAVE followed
	// by a LOAD into a fresh instance, so this drives exactly that and then empties the rack.
	//
	// **THIS CASE DID NOT REPRODUCE THE REPORT, AND IT IS KEPT FOR THAT REASON.** It is the sequence
	// described, driven end to end, and every value comes out right: the amounts survive the trip,
	// the instance-keyed routes resolve onto whatever lanes the devices hold, and emptying the rack
	// leaves the whole table at zero. So this is a NEGATIVE result written down where the next
	// session can find it — the state road is not where the modulation persists, and whoever picks
	// the report up next should start somewhere else rather than re-walking this.
	//
	// It earns its keep as a guard even so: the save is instance-keyed and the load resolves each id
	// to whatever lane the device now holds, and there is no other case asserting that a route
	// survives a round trip AND that removing its device afterwards takes it away.
	//
	// SWEPT RATHER THAN SAMPLED at the end, which is the half that makes it worth having: the
	// question is whether ANY cell still reaches anything, and checking the two cells this case
	// happens to know about would pass while a route sat somewhere nobody thought to look.
	using namespace feathers;

	// ---- THE SOURCE INSTANCE: a rack with two modulators and a route out of each.
	ParamBridge source;
	const size_t second = source.addRackDevice(mod::dahdsr::kTraits.tag);
	MESSAGE("second rack slot = " << second << " (kMaxRackSlots=" << kMaxRackSlots << ")");
	REQUIRE(second < kMaxRackSlots);

	const ModRack sourceRack = source.modRack();
	MESSAGE("source rack count=" << sourceRack.count << " id0=" << sourceRack.device[0].instanceId
	                             << " id1=" << sourceRack.device[1].instanceId);

	// **NAMED BY DEVICE (kill-lanes step 5).** These were source COORDINATES minted from each
	// modulator's rack lane, which is why the copy's had to be read back rather than assumed to match:
	// a lane is a place and the load chooses places. An id is the same on both sides of the file, which
	// is the property the whole round trip is about.
	const backplane::Destination dest =
	    backplane::Destination::core(static_cast<size_t>(modDestinationIndexOf(ParamId::Path)));
	const backplane::Source srcA = backplane::Source::output(sourceRack.device[0].instanceId, 0);
	const backplane::Source srcB = backplane::Source::output(sourceRack.device[1].instanceId, 0);
	source.setRouteAmount(srcA, dest, 0.25f);
	source.setRouteAmount(srcB, dest, 0.5f);
	REQUIRE(source.routeAmount(srcA, dest) == doctest::Approx(0.25f));
	REQUIRE(source.routeAmount(srcB, dest) == doctest::Approx(0.5f));

	// ---- SAVE, as the host does when it duplicates the track, and LOAD into a FRESH instance, as
	// feathers_clap.cpp's chains block does.
	ParamBridge copy;
	CHECK(copy.restoreChains(throughBytes(source.chainsPatch())).routes.count == 0);

	// WHAT SURVIVED THE TRIP, as values — and the two devices came back under their own ids, which is
	// what makes asking at `srcA` and `srcB` the same question on this side of the file.
	CHECK(copy.rackDeviceAt(0) == srcA.device);
	CHECK(copy.rackDeviceAt(1) == srcB.device);
	MESSAGE("copy rack count=" << copy.modRack().count << " amounts: srcA="
	                           << copy.routeAmount(srcA, dest) << " srcB="
	                           << copy.routeAmount(srcB, dest));
	CHECK(copy.routeAmount(srcA, dest) == doctest::Approx(0.25f));
	CHECK(copy.routeAmount(srcB, dest) == doctest::Approx(0.5f));

	// ---- NOW EDIT IT TO HAVE NO MOD SOURCES, which is the defect's next step.
	while (copy.modRack().count > 0) {
		REQUIRE(copy.removeRackDevice(0));
	}
	REQUIRE(copy.modRack().count == 0);

	// ---- AND NOTHING MUST BE ROUTED AT ALL. Swept rather than sampled: the question is whether ANY
	// route still reaches anything, and a check on the two we happen to know about would pass while a
	// route sat somewhere nobody thought to look. **The sweep is the LIST now (kill-lanes step 5)** —
	// which is the same question asked of the thing that holds the answer, rather than of a table whose
	// empty cells vastly outnumber its full ones.
	const TestRoutes after = routesOn(copy);
	for (const backplane::Route& r : after) {
		MESSAGE("surviving route: source device=" << r.source.device << " index=" << r.source.index
		                                          << " -> destination device=" << r.destination.device
		                                          << " index=" << r.destination.index << " amount="
		                                          << r.amount);
	}
	CHECK(after.empty());
}

//------------------------------------------------------------------------------------------------//
// A REORDER DELETES THE ROUTES IT INVALIDATES — the first gesture in this plugin that destroys      //
// a player's work as a normal consequence of a normal action.                                      //
//------------------------------------------------------------------------------------------------//

TEST_CASE("a reorder DELETES the routes it makes illegal, and leaves the others alone") {
	// **DECIDED, with the consequence weighed first.** A reorder that breaks routes deletes them, and
	// they are gone for good: an amount is a cell and deleting it means writing zero —
	// indistinguishable from never-routed — so reordering back does NOT restore it and the hand
	// rebuilds by hand.
	//
	// **IT HAPPENS AT THE REORDER, WHICH IS THE ONLY HONEST MOMENT.** Leaving the route in the table
	// and relying on the loader to drop it would make it silently inert until a save-and-reload: still
	// drawn, still listed, delivering nothing, vanishing later for no reason a player can see.
	//
	// **AND THIS CASE NAMES WHAT IT DESTROYS**, because that is the standard for a commit that deletes
	// user data: the route that dies is asserted dead BY VALUE, and a route that must survive is
	// asserted alive beside it. A clearing that cleared everything would satisfy the first alone.
	//
	// **TWO SKLITTERS IN THE FX LANE, NOT TWO STRIP ENVELOPES (2026-10-03).** A reorder can only make illegal what
	// the order decides. Two strip envelopes were ordered by their note road, which only listens; since that
	// stopped ordering anything (`NoteOps::listensOnly`), IZ's decision — a modulator's place in a lane is only
	// grouping — covers them, and no move of theirs breaks a route. Two devices that both TICK and RUN are what an
	// order still binds (graph.h, "NO ONE ORDER COULD RUN IT"), and SKLITTER is that device.
	namespace sk = feathers::fx::sklitter;
	namespace flt = feathers::fx::filter;

	feathers::ParamBridge bridge;
	bridge.refresh();

	// TWO SKLITTERS AFTER THE DEFAULT FILTER: one route that the reorder must kill, one it must not touch.
	const size_t first = bridge.addFxDevice(sk::kTraits.tag);
	REQUIRE(first < feathers::kMaxFxDevices);
	const size_t second = bridge.addFxDevice(sk::kTraits.tag);
	REQUIRE(second < feathers::kMaxFxDevices);
	REQUIRE(second > first);
	bridge.refresh();

	// **BY IDENTITY, BECAUSE A REORDER IS PRECISELY WHAT BREAKS A COORDINATE (kill-lanes step 3b).** A
	// coordinate built from `first` names the OTHER device after the move, and the "gone" assertion would then
	// pass as a question about two devices that never had a route — zero for the wrong reason.
	const feathers::DeviceInstanceId firstId = bridge.fxDeviceAt(first);
	const feathers::DeviceInstanceId secondId = bridge.fxDeviceAt(second);
	const feathers::DeviceInstanceId filterId = bridge.fxDeviceAt(0);
	REQUIRE(firstId != feathers::kNoInstance);
	REQUIRE(secondId != feathers::kNoInstance);
	REQUIRE(filterId != feathers::kNoInstance);
	REQUIRE(firstId != secondId);
	REQUIRE(bridge.descriptorOf(filterId) != nullptr);
	REQUIRE(feathers::device::sameTag(bridge.descriptorOf(filterId)->traits.tag, flt::kTraits.tag));

	const feathers::backplane::Source fromFirst = feathers::backplane::Source::output(firstId, 0);
	const feathers::backplane::Destination intoSecond =
	    feathers::backplane::Destination::control(secondId, static_cast<size_t>(sk::Param::PerfX));
	const feathers::backplane::Destination intoFilter =
	    feathers::backplane::Destination::control(filterId, static_cast<size_t>(flt::Param::Cutoff));

	// BOTH LEGAL BEFORE THE MOVE — said as values, or "it is gone afterwards" proves nothing.
	bridge.setRouteAmount(fromFirst, intoSecond, 0.5f);
	bridge.setRouteAmount(fromFirst, intoFilter, 0.25f);
	REQUIRE(bridge.routeAmount(fromFirst, intoSecond) == doctest::Approx(0.5f));
	REQUIRE(bridge.routeAmount(fromFirst, intoFilter) == doctest::Approx(0.25f));

	SUBCASE("moving the source BELOW its destination destroys that route") {
		REQUIRE(bridge.moveFxDevice(first, second));
		// THE MOVE REALLY HAPPENED, or everything below is about a chain that never changed.
		REQUIRE(bridge.fxDeviceAt(second) == firstId);
		REQUIRE(bridge.fxDeviceAt(first) == secondId);

		// **GONE, BY VALUE.** The first SKLITTER now runs after the second, so its output can no longer reach
		// the second's Perf X — and the amount is not merely unreachable, it is zero.
		CHECK(bridge.routeAmount(fromFirst, intoSecond) == doctest::Approx(0.0f));

		// **AND THE FILTER ROUTE IS UNTOUCHED, WHICH IS THE CONTROL.** A perform output reaches an effect
		// before it wherever the two stand (IZ), so this route was never in danger — and a clearing that took
		// it too would have passed the assertion above.
		CHECK(bridge.routeAmount(fromFirst, intoFilter) == doctest::Approx(0.25f));
	}

	SUBCASE("and moving it back does NOT bring the route back — gone means gone") {
		// **THE HALF THAT WAS DECIDED EXPLICITLY**, and the reason the answer matters: a route is
		// an amount in a table, so deleting it writes zero, which is what "never routed" also looks
		// like. There is nothing left to restore, and this case exists so nobody later reads the
		// deletion as recoverable and builds an undo on top of it.
		REQUIRE(bridge.moveFxDevice(first, second));
		REQUIRE(bridge.routeAmount(fromFirst, intoSecond) == doctest::Approx(0.0f));
		REQUIRE(bridge.moveFxDevice(second, first)); // put it back where it was
		CHECK(bridge.routeAmount(fromFirst, intoSecond) == doctest::Approx(0.0f));
		// AND THE ORDER REALLY WAS RESTORED, or the check above passes because nothing moved.
		CHECK(bridge.fxDeviceAt(first) == firstId);
		CHECK(bridge.fxDeviceAt(second) == secondId);
	}
}

TEST_CASE("a full fx chain is routable at EVERY position, the seventh and eighth included — backlog CV and its twin, retired") {
	// **TWO CASES STOOD HERE AND THE MACHINERY THEY GUARDED IS DELETED (kill-lanes step 5).** Both were
	// about ONE sentinel collision, found twice a day apart: Feathers ranked its lanes in bands — core,
	// rack `1..4`, the synth, fx `6..13` — so fx position 6 ranked `1 + 4 + 1 + 6 = 12`, which was also
	// `kMaxDeviceSlots`, which was also what two different functions answered "no order" with.
	// `DeviceSlots::order` had it (backlog CV) and `orderIndexOfLane` had it one call earlier (CV's
	// twin), and the consequence was that the seventh fx device read as unranked at both ends: no
	// source could reach it, it could reach nothing, the bus mask zeroed every source for it. Latent
	// until a chain reached seven devices, which no fixture had built — so the cases built one and
	// pinned the ranks as numbers.
	//
	// **THE HAZARD IS NOT FIXED HERE, IT IS UNREACHABLE: there is no rank.** `DeviceSlots::order`,
	// `orderAt`, `kUnordered`, `orderIndexOfLane`, `kUnrankedLane`, `sourceLaneIsUpstreamOf` and
	// `clearRoutesTheOrderForbids` are all gone, and with them every number that could collide with a
	// sentinel. A case asserting those values would not compile; one asserting them through a shim
	// would be asserting a shim. So the value half is retired rather than ported.
	//
	// **WHAT IS KEPT IS THE CONSEQUENCE A PLAYER WOULD HAVE MET**, which was always the independent
	// half of both cases — asked of the gate a surface asks, naming the two DEVICES rather than two
	// lanes. A full chain of eight, a real modulator in the rack, and a route into every position
	// including the two that used to be unroutable.
	namespace flt = feathers::fx::filter;
	namespace env = feathers::mod::dahdsr;
	ParamBridge bridge;
	while (bridge.fxChain().count < feathers::kMaxFxSlots) {
		REQUIRE(bridge.addFxDevice(flt::kTraits.tag) < feathers::kMaxFxSlots);
	}
	REQUIRE(bridge.fxChain().count == feathers::kMaxFxSlots);
	REQUIRE(feathers::kMaxFxSlots >= 8);
	const size_t envSlot = bridge.addRackDevice(env::kTraits.tag);
	REQUIRE(envSlot < feathers::kMaxRackSlots);
	const feathers::DeviceInstanceId envId = bridge.rackDeviceAt(envSlot);
	REQUIRE(envId != feathers::kNoInstance);
	const backplane::Source envSource = backplane::Source::output(envId, 0);

	const auto cutoff = static_cast<size_t>(flt::Param::Cutoff);
	for (size_t position = 0; position < feathers::kMaxFxSlots; ++position) {
		CAPTURE(position);
		const feathers::DeviceInstanceId at = bridge.fxDeviceAt(position);
		REQUIRE(at != feathers::kNoInstance);
		const backplane::Destination dest = backplane::Destination::control(at, cutoff);
		CHECK(bridge.mayRoute(envSource, dest));
		// AND THE ROUTE IS REALLY MADE, which is what stops `mayRoute` answering for itself: a gate that
		// says yes and a store that refuses is the disagreement the honesty rule is about.
		bridge.setRouteAmount(envSource, dest, 0.5f);
		CHECK(bridge.routeAmount(envSource, dest) == doctest::Approx(0.5f));
	}
}

TEST_CASE("MEASURED: what the per-paint route reads cost, at ten routes and near the cap") {
	// **THE ROAD**: a ring draws `routeAmount` and its "routed elsewhere" mark draws `routedElsewhere`,
	// both per control per paint, and both are walks of the record's edges since kill-lanes step 2 (the
	// dense table they replaced was an array index into 111 KB). The population that matters is a real
	// patch — about ten routes — with the cap as the pathological end.
	//
	// NO BOUND IS ASSERTED: load on this host swings, so the case asserts correctness and reports time.
	feathers::ParamBridge bridge;
	const feathers::backplane::Source macro = feathers::backplane::Source::core(0);

	// EVERY ROUTE THROUGH THE HAND'S OWN ROAD, so what is measured is what a patch really holds.
	std::vector<feathers::backplane::Destination> routed;
	const auto fill = [&](size_t want) {
		for (size_t d = 0; d < feathers::kParamDestinationCount && routed.size() < want; ++d) {
			const feathers::backplane::Destination dest = feathers::backplane::Destination::core(d);
			if (!bridge.mayRoute(macro, dest)) {
				continue;
			}
			bridge.setRouteAmount(macro, dest, 0.25f);
			if (bridge.routeAmount(macro, dest) != 0.0f) {
				routed.push_back(dest);
			}
		}
	};
	fill(10);
	REQUIRE(routed.size() == 10);
	const feathers::backplane::Destination probe = routed.back(); // the far end of the walk
	CHECK(bridge.routeAmount(macro, probe) == doctest::Approx(0.25f));
	CHECK(bridge.routedElsewhere(probe, feathers::backplane::Source::nobody()));

	static volatile float sinkF = 0.0f;
	static volatile bool sinkB = false;
	constexpr int kReads = 20000;
	constexpr int kRounds = 7;
	const auto timeIt = [&](auto&& body) {
		const auto began = std::chrono::steady_clock::now();
		body();
		return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		                               std::chrono::steady_clock::now() - began)
		                               .count())
		       / kReads;
	};
	const auto measure = [&](std::vector<double>& amounts, std::vector<double>& elsewheres) {
		// ALTERNATED, so a load spike lands on both.
		amounts.push_back(timeIt([&] {
			for (int i = 0; i < kReads; ++i) {
				sinkF = sinkF + bridge.routeAmount(macro, probe);
			}
		}));
		elsewheres.push_back(timeIt([&] {
			for (int i = 0; i < kReads; ++i) {
				sinkB = sinkB != bridge.routedElsewhere(probe, macro);
			}
		}));
	};
	std::vector<double> tenAmount, tenElsewhere;
	for (int round = 0; round < kRounds; ++round) {
		measure(tenAmount, tenElsewhere);
	}

	// AND AT THE STARTING ROOM — every legal macro→parameter pair this build has, up to it.
	fill(feathers::kInitialRouteRoom);
	const size_t atCap = routed.size();
	std::vector<double> capAmount, capElsewhere;
	for (int round = 0; round < kRounds; ++round) {
		measure(capAmount, capElsewhere);
	}

	const auto median = [](std::vector<double> v) {
		std::sort(v.begin(), v.end());
		return v[v.size() / 2];
	};
	MESSAGE("route reads at 10 routes: routeAmount " << median(tenAmount) << " ns, routedElsewhere "
	        << median(tenElsewhere) << " ns");
	MESSAGE("route reads at " << atCap << " routes: routeAmount " << median(capAmount)
	        << " ns, routedElsewhere " << median(capElsewhere)
	        << " ns; a 127-control card drawing both per control pays "
	        << (median(capAmount) + median(capElsewhere)) * 127.0
	        << " ns per paint, against 16700000 ns for a 60 Hz frame");
	CHECK(bridge.routeAmount(macro, probe) == doctest::Approx(0.25f));
}

TEST_CASE("the canvas's view rides in Feathers' patch: an edit when it changes, restored by a load, replaced by the next patch") {
	// The partition size and the expanded state persist with the patch.
	ParamBridge maker;
	CHECK_FALSE(maker.takeStateDirty());
	maker.setCanvasView({ 300.0f, true });
	CHECK(maker.takeStateDirty()); // an edit, as a fold is
	maker.setCanvasView({ 300.0f, true });
	CHECK_FALSE(maker.takeStateDirty()); // the same view again is not one
	const rack::RackPatch saved = maker.chainsPatch();
	CHECK(saved.canvas == rack::CanvasView { 300.0f, true });

	ParamBridge reader;
	REQUIRE(reader.restoreChains(saved).routes.count == 0);
	CHECK(reader.canvasView() == rack::CanvasView { 300.0f, true });
	CHECK_FALSE(reader.takeStateDirty()); // a load is not an edit
	// THE NEXT PATCH REPLACES IT — one patch's partition never survives into another's.
	ParamBridge other;
	REQUIRE(reader.restoreChains(other.chainsPatch()).routes.count == 0);
	CHECK(reader.canvasView() == rack::CanvasView {});
}

TEST_CASE("a fold rides in the patch, and one patch's folds never reach another patch's cards") {
	// **WHY THIS COULD NOT BE A DESK PREFERENCE, which is where it lived.** `EditorPrefs` is a desk that
	// outlives every patch, so a fold kept there had to be keyed by something the desk can say — and the
	// only thing it could say was the card's POSITION in the strip. A position is not an identity: reorder
	// the strip and the fold moved to whichever card arrived at that number. The obvious repair, keying the
	// desk file by device id, is worse and silently so: **IDS ARE PATCH-SCOPED**, so patch A's device 1 and
	// patch B's device 1 are different devices wearing one key, and loading B would hand its first card
	// A's fold. By decision (2026-09-12) the fold lives in the patch.
	//
	// The two halves of that decision are the two halves of this case: a fold SURVIVES a save and load, and
	// it does not CROSS from one patch to another. The second is the one only the patch can get right.
	ParamBridge maker;
	rack::RackPatch empty;
	empty.present = true;
	REQUIRE(maker.restoreChains(empty).routes.count == 0);
	REQUIRE(maker.addRackDevice(mod::mseg::kTraits.tag) < kMaxRackSlots);
	REQUIRE(maker.addRackDevice(mod::dahdsr::kTraits.tag) < kMaxRackSlots);
	const DeviceInstanceId folded = maker.rackDeviceAt(1);
	const DeviceInstanceId open = maker.rackDeviceAt(0);
	REQUIRE(folded != kNoInstance);
	REQUIRE(open != kNoInstance);
	REQUIRE_FALSE(maker.deviceFolded(folded));
	maker.setDeviceFolded(folded, true);
	REQUIRE(maker.deviceFolded(folded));
	REQUIRE_FALSE(maker.deviceFolded(open)); // one of each, so neither answer is the only one available
	const rack::RackPatch withFold = throughBytes(maker.chainsPatch());

	// A SECOND PATCH, made by a SECOND BRIDGE — which is what makes its ids collide with the first's.
	// Both counters start where a fresh instance starts, so this patch's devices wear the same numbers.
	ParamBridge other;
	REQUIRE(other.restoreChains(empty).routes.count == 0);
	REQUIRE(other.addRackDevice(mod::mseg::kTraits.tag) < kMaxRackSlots);
	REQUIRE(other.addRackDevice(mod::dahdsr::kTraits.tag) < kMaxRackSlots);
	const rack::RackPatch noFold = throughBytes(other.chainsPatch());
	REQUIRE(noFold.device.size() == withFold.device.size());
	bool anyIdShared = false;
	for (size_t i = 0; i < noFold.device.size(); ++i) {
		anyIdShared = anyIdShared || noFold.device[i].instanceId == withFold.device[i].instanceId;
	}
	REQUIRE(anyIdShared); // the premise: without a shared id there is nothing for a fold to leak ALONG

	ParamBridge reader;
	REQUIRE(reader.restoreChains(withFold).routes.count == 0);
	// **BY DEVICE.** The record may have put these devices anywhere; ask by the id each one wears.
	const DeviceInstanceId loadedFolded = reader.rackDeviceAt(1);
	const DeviceInstanceId loadedOpen = reader.rackDeviceAt(0);
	REQUIRE(loadedFolded != kNoInstance);
	CHECK(reader.deviceFolded(loadedFolded));
	CHECK_FALSE(reader.deviceFolded(loadedOpen));

	SUBCASE("the next patch loaded over it arrives with its OWN folds, which are none") {
		REQUIRE(reader.restoreChains(noFold).routes.count == 0);
		int asked = 0;
		for (size_t slot = 0; slot < 2; ++slot) {
			const DeviceInstanceId id = reader.rackDeviceAt(slot);
			REQUIRE(id != kNoInstance);
			CAPTURE(slot);
			CAPTURE(id);
			CHECK_FALSE(reader.deviceFolded(id));
			++asked;
		}
		CHECK(asked == 2);
	}

	SUBCASE("and a fold is an EDIT — the host is told the state moved") {
		// A fold that never marks the patch dirty is a fold the host does not save, which is the same
		// bug wearing a different coat. `setDeviceFolded` sets the dirty flag; folding to the value it
		// already has does not, because nothing changed.
		ParamBridge fresh;
		REQUIRE(fresh.restoreChains(withFold).routes.count == 0);
		(void)fresh.takeStateDirty();
		const DeviceInstanceId id = fresh.rackDeviceAt(0);
		REQUIRE(id != kNoInstance);
		REQUIRE_FALSE(fresh.deviceFolded(id));
		fresh.setDeviceFolded(id, false); // already open: not an edit
		CHECK_FALSE(fresh.takeStateDirty());
		fresh.setDeviceFolded(id, true);
		CHECK(fresh.takeStateDirty());
	}
}

TEST_CASE("the travel pulls cross the bridge whole, and a bridge nobody published to reads zero") {
	// ZERO IS THE AUTHORED PLACE (`pulledPlace`), so an editor with no engine behind it draws true.
	ParamBridge bridge;
	CHECK(bridge.travelPulls() == TravelPulls {});

	TravelPulls pulls;
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		pulls.radius[slot] = 0.01 * static_cast<double>(slot + 1);
		pulls.theta[slot] = -1.5 * static_cast<double>(slot + 1); // unwrapped: past -pi on purpose
	}
	(void)bridge.takeRepaintDirty(); // whatever construction raised is not this case's subject
	bridge.publishTravelPulls(pulls);
	CHECK(bridge.travelPulls() == pulls);
	// Written every block by the engine, so it must NOT be what wakes the editor's repaint.
	CHECK_FALSE(bridge.takeRepaintDirty());
}

TEST_CASE("a sound's polar offset survives the patch, exactly, and an FST1 patch still loads") {
	// THE RADIUS AND THETA CORNERS ARE PATCH STATE, beside the place they are added to — and double all
	// the way, for the reason the place is: what is lost to a float is the SOUND.
	SoundTravel saved;
	saved.offset[1] = { 0.37, -0.21 };
	saved.orbit[1] = { 1.0 / 3.0, -2.0 };
	saved.orbit[7] = { 0.0125, 3.0 };

	MemoryStream stream;
	clap_ostream out = stream.out();
	REQUIRE(saveSoundTravel(&out, saved));
	SoundTravel loaded;
	bool present = false;
	clap_istream in = stream.in();
	REQUIRE(loadSoundTravel(&in, &loaded, &present));
	REQUIRE(present);
	CHECK(loaded == saved); // exact: doubles written, doubles read
	REQUIRE(loaded.orbit[1].radius == 1.0 / 3.0);

	// AND THROUGH THE BRIDGE, the road a load takes: the whole struct in, the whole struct back.
	ParamBridge bridge;
	bridge.setSoundTravel(loaded);
	CHECK(bridge.soundTravel() == saved);

	SUBCASE("FST1 — two doubles a slot, no per-slot count — reads with every offset at zero") {
		// Written by hand in the OLD shape rather than by a saver kept around for the purpose.
		MemoryStream old;
		clap_ostream oldOut = old.out();
		const uint8_t tag[4] = { 'F', 'S', 'T', '1' };
		REQUIRE(oldOut.write(&oldOut, tag, 4) == 4);
		const float count = static_cast<float>(kMaxSounds);
		REQUIRE(oldOut.write(&oldOut, &count, sizeof(count)) == static_cast<int64_t>(sizeof(count)));
		for (size_t slot = 0; slot < kMaxSounds; ++slot) {
			const double pair[2] = { 0.1 * static_cast<double>(slot), -0.5 };
			REQUIRE(oldOut.write(&oldOut, pair, sizeof(pair)) == static_cast<int64_t>(sizeof(pair)));
		}
		SoundTravel fromOld;
		bool oldPresent = false;
		clap_istream oldIn = old.in();
		REQUIRE(loadSoundTravel(&oldIn, &fromOld, &oldPresent));
		REQUIRE(oldPresent);
		CHECK(fromOld.offset[3].x == 0.1 * 3.0);
		CHECK(fromOld.offset[3].y == -0.5);
		CHECK(fromOld.orbit[3] == PolarOffset {});
	}

	SUBCASE("a spoilt offset is dropped whole, and costs the place beside it nothing") {
		SoundTravel poisoned = saved;
		poisoned.orbit[1].theta = std::numeric_limits<double>::quiet_NaN();
		MemoryStream bad;
		clap_ostream badOut = bad.out();
		REQUIRE(saveSoundTravel(&badOut, poisoned));
		SoundTravel back;
		bool there = false;
		clap_istream badIn = bad.in();
		REQUIRE(loadSoundTravel(&badIn, &back, &there));
		CHECK(back.orbit[1] == PolarOffset {});
		CHECK(back.offset[1].x == 0.37);
		CHECK(back.orbit[7] == saved.orbit[7]);
		// The hand's road refuses the same things, and a negative radius with them.
		ParamBridge guarded;
		guarded.setSoundOrbit(2, { -0.1, 0.0 });
		guarded.setSoundOrbit(2, { 0.1, std::numeric_limits<double>::infinity() });
		CHECK(guarded.soundTravel().orbit[2] == PolarOffset {});
	}
}


//------------------------------------------------------------------------------------------------//
// PER-INSTANCE OUTPUT LABELS — what a player calls each of a device's outputs (`outputLabel`), held   //
// beside the folds and saved with them.                                                           //
//------------------------------------------------------------------------------------------------//

namespace {

/// A MACROS DEVICE ADDED TO THE STRIP, by the hand's own verb, and its id.
DeviceInstanceId addMacros(ParamBridge& bridge) {
	const size_t slot = bridge.addRackDevice("MACROS");
	REQUIRE(slot < kMaxRackSlots);
	const DeviceInstanceId id = bridge.rackDeviceAt(slot);
	REQUIRE(id != kNoInstance);
	REQUIRE(bridge.descriptorOf(id) != nullptr);
	REQUIRE(bridge.descriptorOf(id)->outputCount() == 4);
	return id;
}

using Heard = std::vector<std::pair<DeviceInstanceId, size_t>>;

} // namespace

TEST_CASE("a relabelled output names its source; clearing restores the declared name") {
	ParamBridge bridge;
	const DeviceInstanceId id = addMacros(bridge);
	const device::Descriptor& macros = *bridge.descriptorOf(id);
	const std::string declared = macros.outputs[1].name;
	CHECK(bridge.outputLabel(id, 1) == declared);
	CHECK_FALSE(bridge.outputRelabelled(id, 1));

	bridge.setOutputLabel(id, 1, "Wobble");
	CHECK(bridge.outputLabel(id, 1) == "Wobble");
	CHECK(bridge.outputRelabelled(id, 1));
	CHECK(bridge.outputLabel(id, 0) == std::string(macros.outputs[0].name)); // only the one it was given

	SUBCASE("cleared by an empty label") { bridge.setOutputLabel(id, 1, ""); }
	SUBCASE("cleared by the declared name spelled out") { bridge.setOutputLabel(id, 1, declared); }
	CHECK(bridge.outputLabel(id, 1) == declared);
	CHECK_FALSE(bridge.outputRelabelled(id, 1));

	// CLAMPED by the macros' own clamp; an output the device does not declare, and an id nothing holds,
	// take nothing and answer "Source".
	bridge.setOutputLabel(id, 2, std::string(device::kMacroNameMax + 5, 'n'));
	CHECK(bridge.outputLabel(id, 2) == std::string(device::kMacroNameMax, 'n'));
	bridge.setOutputLabel(id, macros.outputCount(), "Nobody");
	CHECK(bridge.outputLabel(id, macros.outputCount()) == "Source");
	bridge.setOutputLabel(id + 1000, 0, "Nobody");
	CHECK(bridge.outputLabel(id + 1000, 0) == "Source");
}

TEST_CASE("two MACROS instances keep separate labels") {
	ParamBridge bridge;
	const DeviceInstanceId a = addMacros(bridge);
	const DeviceInstanceId b = addMacros(bridge);
	REQUIRE(a != b);
	bridge.setOutputLabel(a, 0, "Wobble");
	bridge.setOutputLabel(b, 0, "Swell");
	CHECK(bridge.outputLabel(a, 0) == "Wobble");
	CHECK(bridge.outputLabel(b, 0) == "Swell");
	bridge.setOutputLabel(a, 0, "");
	CHECK(bridge.outputLabel(a, 0) == std::string(bridge.descriptorOf(a)->outputs[0].name));
	CHECK(bridge.outputLabel(b, 0) == "Swell"); // clearing one is not clearing the other

	// AND BOTH SURVIVE A SAVE AND A LOAD INTO A FRESH BRIDGE, each on its own device.
	bridge.setOutputLabel(a, 3, "Kick");
	ParamBridge loaded;
	REQUIRE(loaded.restoreChains(bridge.chainsPatch()).routes.count == 0);
	CHECK(loaded.outputLabel(a, 3) == "Kick");
	CHECK(loaded.outputLabel(b, 0) == "Swell");
	CHECK_FALSE(loaded.outputRelabelled(a, 0));
	CHECK_FALSE(loaded.outputRelabelled(b, 3));
}

TEST_CASE("a one-output device's source name is unchanged") {
	// The LFO declares ONE output: its label is its declared name, and `modSourceName` adds no suffix for
	// it (asserted as text in the widget suite).
	ParamBridge bridge;
	const size_t slot = bridge.addRackDevice("LFO");
	REQUIRE(slot < kMaxRackSlots);
	const DeviceInstanceId id = bridge.rackDeviceAt(slot);
	const device::Descriptor* lfo = bridge.descriptorOf(id);
	REQUIRE(lfo != nullptr);
	REQUIRE(lfo->outputCount() == 1);
	CHECK(bridge.outputLabel(id, 0) == std::string(lfo->outputs[0].name));
}

TEST_CASE("label edits dirty the patch and notify subscribers") {
	ParamBridge bridge;
	const DeviceInstanceId id = addMacros(bridge);
	(void)bridge.takeStateDirty();
	(void)bridge.takeRepaintDirty();
	Heard heard;
	Heard other;
	device::Subscription token =
	    bridge.subscribeOutputLabel([&heard](DeviceInstanceId d, size_t o) { heard.emplace_back(d, o); });
	device::Subscription second =
	    bridge.subscribeOutputLabel([&other](DeviceInstanceId d, size_t o) { other.emplace_back(d, o); });

	bridge.setOutputLabel(id, 3, "Kick");
	CHECK(bridge.takeStateDirty());   // the patch carries it now, so the host is told
	CHECK(bridge.takeRepaintDirty()); // and every surface naming this source reads it differently
	REQUIRE(heard.size() == 1);
	CHECK(heard[0] == std::make_pair(id, size_t { 3 }));
	CHECK(other == heard);

	bridge.setOutputLabel(id, 3, "Kick");
	bridge.setOutputLabel(id, 2, bridge.descriptorOf(id)->outputs[2].name);
	bridge.setOutputLabel(id, 9, "x");
	CHECK_FALSE(bridge.takeStateDirty()); // unchanged, the declared name, no such output: none an edit
	CHECK(heard.size() == 1);

	second.reset();
	bridge.setOutputLabel(id, 3, "");
	CHECK(bridge.takeStateDirty());
	REQUIRE(heard.size() == 2);
	CHECK(heard[1] == std::make_pair(id, size_t { 3 }));
	CHECK(other.size() == 1); // its token ended
}

TEST_CASE("a patch load relabels and notifies once per changed label") {
	ParamBridge bridge;
	const DeviceInstanceId id = addMacros(bridge);
	const device::Descriptor& macros = *bridge.descriptorOf(id);
	bridge.setOutputLabel(id, 0, "Wobble"); // cleared by the load
	bridge.setOutputLabel(id, 1, "Keep");   // the same after it
	rack::RackPatch patch = bridge.chainsPatch();
	rack::RackPatch::Device* mine = nullptr;
	for (rack::RackPatch::Device& d : patch.device) {
		if (d.instanceId == id) {
			mine = &d;
		}
	}
	REQUIRE(mine != nullptr);
	// BY OUTPUT KEY (backlog JF), only the outputs that carry one: 0 unnamed, 3 the declared name spelled out.
	using Value = rack::RackPatch::Value;
	mine->labels = { Value::label(macros.outputs[1].key, "Keep"), Value::label(macros.outputs[2].key, "Swell"),
		             Value::label(macros.outputs[3].key, macros.outputs[3].name) };

	(void)bridge.takeStateDirty();
	Heard heard;
	device::Subscription token =
	    bridge.subscribeOutputLabel([&heard](DeviceInstanceId d, size_t o) { heard.emplace_back(d, o); });
	REQUIRE(bridge.restoreChains(patch).routes.count == 0);
	std::sort(heard.begin(), heard.end());
	CHECK(heard == Heard { { id, 0 }, { id, 2 } });
	CHECK(bridge.outputLabel(id, 0) == std::string(macros.outputs[0].name));
	CHECK(bridge.outputLabel(id, 1) == "Keep");
	CHECK(bridge.outputLabel(id, 2) == "Swell");
	CHECK_FALSE(bridge.outputRelabelled(id, 3));
	CHECK_FALSE(bridge.takeStateDirty()); // opening a patch is not an edit

	// The same load again moves nothing.
	heard.clear();
	REQUIRE(bridge.restoreChains(patch).routes.count == 0);
	CHECK(heard.empty());

	// AND NO PATCH AT ALL — a fresh instance — clears every label, and says so for each.
	rack::RackPatch none;
	(void)bridge.restoreChains(none);
	std::sort(heard.begin(), heard.end());
	CHECK(heard == Heard { { id, 1 }, { id, 2 } });
	CHECK_FALSE(bridge.outputRelabelled(id, 1));
}

//--------------------------------------------------------------------------------------------------//
// ROUTE TRANSFER CHIPS — stored, saved by label, reloaded,                                           //
// published to the engine, kept across zero, and refused where it would be drawn and never heard.     //
//--------------------------------------------------------------------------------------------------//

namespace {
feathers::backplane::Transfer transferIn(const TestRoutes& routes, feathers::backplane::Source s,
                                         feathers::backplane::Destination d) {
	for (const feathers::backplane::Route& r : routes) {
		if (r.source == s && r.destination == d) {
			return r.transfer;
		}
	}
	return feathers::backplane::Transfer::Lin;
}
} // namespace

TEST_CASE("transfer chips: a route's curve is saved BY LABEL, comes back from the text, and a Lin patch writes what it always wrote") {
	using feathers::backplane::Transfer;
	ParamBridge bridge;
	const feathers::backplane::Source macro0 = feathers::backplane::Source::core(0);
	const int fineAt = feathers::modDestinationIndexOf(feathers::ParamId::FineTune);
	REQUIRE(fineAt >= 0);
	const feathers::backplane::Destination fine = feathers::backplane::Destination::core(static_cast<size_t>(fineAt));
	bridge.setRouteAmount(macro0, fine, 0.5f);
	REQUIRE(bridge.routeAmount(macro0, fine) == 0.5f);
	CHECK(bridge.routeTransfer(macro0, fine) == Transfer::Lin);

	// A LIN PATCH SAYS NOTHING OF CURVES — byte-for-byte what it wrote before this feature existed.
	std::string lin;
	REQUIRE(feathers::rack::rackToText(bridge.chainsPatch(), &lin));
	CHECK(lin.find("transfer") == std::string::npos);

	REQUIRE(bridge.setRouteTransfer(macro0, fine, Transfer::Exp));
	CHECK(bridge.routeTransfer(macro0, fine) == Transfer::Exp);
	// PUBLISHED: the list the engine snapshots carries it.
	CHECK(transferIn(routesOn(bridge), macro0, fine) == Transfer::Exp);
	bool published = false;
	bridge.routeList().forEach([&](const feathers::backplane::Route& r) {
		if (r.source == macro0 && r.destination == fine) {
			published = r.transfer == Transfer::Exp;
		}
	});
	CHECK(published);

	std::string text;
	REQUIRE(feathers::rack::rackToText(bridge.chainsPatch(), &text));
	CHECK(text.find("\"transfer\"") != std::string::npos);
	CHECK(text.find("\"exp\"") != std::string::npos);
	// THE TEXT ITSELF, through the real reader and a fresh bridge.
	feathers::rack::RackPatch back;
	feathers::rack::LoadReport parsed;
	REQUIRE(feathers::rack::parseRackText(text, &back, &parsed));
	const Reloaded loaded = reload(back);
	CHECK(amountIn(loaded.routes, macro0, fine) == doctest::Approx(0.5f));
	CHECK(transferIn(loaded.routes, macro0, fine) == Transfer::Exp);

	SUBCASE("a label this build lacks loads the route Lin and says so") {
		std::string unknown = text;
		const size_t at = unknown.find("\"exp\"", unknown.find("\"transfer\""));
		REQUIRE(at != std::string::npos);
		unknown.replace(at, 5, "\"sqrt\"");
		feathers::rack::RackPatch odd;
		feathers::rack::LoadReport report;
		REQUIRE(feathers::rack::parseRackText(unknown, &odd, &report));
		CHECK(report.count[static_cast<size_t>(feathers::rack::LoadReport::Kind::LabelUnknown)] == 1);
		const Reloaded lin2 = reload(odd);
		CHECK(amountIn(lin2.routes, macro0, fine) == doctest::Approx(0.5f));
		CHECK(transferIn(lin2.routes, macro0, fine) == Transfer::Lin);
	}
	SUBCASE("an amount dragged through zero keeps its curve, and a route removed and remade gets it back") {
		bridge.setRouteAmount(macro0, fine, 0.0f);
		REQUIRE(bridge.routeAmount(macro0, fine) == 0.0f);
		CHECK(bridge.routeTransfer(macro0, fine) == Transfer::Lin); // no route, no curve
		bridge.setRouteAmount(macro0, fine, -0.25f);
		CHECK(bridge.routeTransfer(macro0, fine) == Transfer::Exp);
	}
	SUBCASE("a curve is refused on a pair with no route, and on the stack index, whose consumer is linear") {
		const feathers::backplane::Source macro1 = feathers::backplane::Source::core(1);
		CHECK_FALSE(bridge.setRouteTransfer(macro1, fine, Transfer::Log));
		const feathers::backplane::Source stack = feathers::backplane::Source::core(feathers::kStackIndexSource);
		bridge.setRouteAmount(stack, fine, 0.5f);
		if (bridge.routeAmount(stack, fine) != 0.0f) {
			CHECK_FALSE(bridge.setRouteTransfer(stack, fine, Transfer::Abs));
			CHECK(bridge.routeTransfer(stack, fine) == Transfer::Lin);
		}
		CHECK_FALSE(bridge.transferAllowedFor(stack, Transfer::Exp));
		CHECK(bridge.transferAllowedFor(stack, Transfer::Lin));
		CHECK(bridge.transferAllowedFor(macro0, Transfer::Exp));
	}
}

TEST_CASE("PY: a route's φ curve saves as \"phi\" with its periods keyed beside it, and comes back into a FRESH bridge at the same periods") {
	using feathers::backplane::Transfer;
	namespace dv = feathers::device;
	ParamBridge bridge;
	const feathers::backplane::Source macro0 = feathers::backplane::Source::core(0);
	const int fineAt = feathers::modDestinationIndexOf(feathers::ParamId::FineTune);
	REQUIRE(fineAt >= 0);
	const feathers::backplane::Destination fine = feathers::backplane::Destination::core(static_cast<size_t>(fineAt));
	bridge.setRouteAmount(macro0, fine, 0.5f);
	// AN ODD STEP, so a load that fell back to the default or rounded to a whole octave answers differently.
	const Transfer phi = dv::phiTransferAtStep(173);
	REQUIRE(dv::phiPeriods(phi) != 16.0f);
	REQUIRE(bridge.setRouteTransfer(macro0, fine, phi));
	CHECK(bridge.routeTransfer(macro0, fine) == phi);
	CHECK(transferIn(routesOn(bridge), macro0, fine) == phi); // published with its periods
	// A CODE THAT IS NOT A CURVE IS REFUSED, and leaves the curve alone.
	CHECK_FALSE(bridge.setRouteTransfer(
	    macro0, fine, static_cast<Transfer>(static_cast<uint16_t>(Transfer::Phi) | ((dv::kPhiSteps + 1) << 8))));
	CHECK(bridge.routeTransfer(macro0, fine) == phi);

	std::string text;
	REQUIRE(feathers::rack::rackToText(bridge.chainsPatch(), &text));
	CHECK(text.find("\"phi\"") != std::string::npos);
	CHECK(text.find("\"transfer_octaves\"") != std::string::npos);
	feathers::rack::RackPatch back;
	feathers::rack::LoadReport parsed;
	REQUIRE(feathers::rack::parseRackText(text, &back, &parsed));
	const Reloaded loaded = reload(back);
	CHECK(amountIn(loaded.routes, macro0, fine) == doctest::Approx(0.5f));
	CHECK(transferIn(loaded.routes, macro0, fine) == phi);
	CHECK(dv::phiStep(transferIn(loaded.routes, macro0, fine)) == 173);

	SUBCASE("a \"phi\" with no periods beside it loads at the default, not at a guess") {
		std::string bare = text;
		const size_t key = bare.find("\"transfer_octaves\"");
		REQUIRE(key != std::string::npos);
		bare.replace(key, std::strlen("\"transfer_octaves\""), "\"transfer_unknown\"");
		feathers::rack::RackPatch odd;
		feathers::rack::LoadReport report;
		REQUIRE(feathers::rack::parseRackText(bare, &odd, &report));
		CHECK(transferIn(reload(odd).routes, macro0, fine) == dv::phiTransfer(dv::kPhiOctavesDefault));
	}
	SUBCASE("Log's and Exp's exponent saves under its own key and comes back; the default writes no key") {
		for (const Transfer kind : { Transfer::Log, Transfer::Exp }) {
			CAPTURE(static_cast<int>(kind));
			const Transfer bent = dv::transferWithStep(kind, 23); // exponent 2·2^(23/16) ≈ 5.42
			REQUIRE(bridge.setRouteTransfer(macro0, fine, bent));
			std::string saved;
			REQUIRE(feathers::rack::rackToText(bridge.chainsPatch(), &saved));
			CHECK(saved.find("\"transfer_exponent\"") != std::string::npos);
			CHECK(saved.find("\"transfer_octaves\"") == std::string::npos);
			feathers::rack::RackPatch again;
			feathers::rack::LoadReport report;
			REQUIRE(feathers::rack::parseRackText(saved, &again, &report));
			const Transfer loadedCurve = transferIn(reload(again).routes, macro0, fine);
			CHECK(loadedCurve == bent);
			CHECK(dv::transferStep(loadedCurve) == 23);
			// THE DEFAULT EXPONENT WRITES WHAT A Log OR Exp ALWAYS WROTE: the label and nothing beside it.
			REQUIRE(bridge.setRouteTransfer(macro0, fine, kind));
			std::string plain;
			REQUIRE(feathers::rack::rackToText(bridge.chainsPatch(), &plain));
			CHECK(plain.find("\"transfer_exponent\"") == std::string::npos);
		}
	}
}

//------------------------------------------------------------------------------------------------//
// UNBOUNDED DEVICES, STEP 7 — SIXTY-FOUR DEVICES THROUGH A SAVE                                    //
//------------------------------------------------------------------------------------------------//

TEST_CASE("unbounded devices step 7: sixty-four devices and a route to the last of them save and reload whole") {
	namespace flt = feathers::fx::filter;
	namespace bp = feathers::backplane;
	ParamBridge bridge;
	constexpr size_t kDevices = 64;
	while (bridge.fxChain().count < kDevices) {
		REQUIRE(bridge.addFxDevice(flt::kTraits.tag) != feathers::kNoPosition);
	}
	std::vector<feathers::DeviceInstanceId> ids;
	for (size_t i = 0; i < kDevices; ++i) {
		ids.push_back(bridge.fxDeviceAt(i));
	}
	const size_t cutoff = flt::kDescriptor.controlOfNthModulatableBesideEnable(0); // the filter's first modulatable control
	REQUIRE(cutoff < flt::kDescriptor.params.size());
	bridge.setRouteAmount(bp::Source::core(0), bp::Destination::control(ids.back(), cutoff), 0.5f);

	// THROUGH THE BYTES — the file a host keeps — into a bridge that starts at the old sizes and must grow to it.
	ParamBridge loaded;
	(void)loaded.restoreChains(throughBytes(bridge.chainsPatch()));
	REQUIRE(loaded.fxChain().count == kDevices);
	for (size_t i = 0; i < kDevices; ++i) {
		CAPTURE(i);
		CHECK(loaded.fxDeviceAt(i) == ids[i]);
	}
	CHECK(loaded.routeAmount(bp::Source::core(0), bp::Destination::control(ids.back(), cutoff)) == doctest::Approx(0.5f));
}

TEST_CASE("lane-mods: modulators in the fx lane survive a real save and load — the devices in their places, a route from one into a lane device, a fold") {
	// THE LANE TAKES MODULATORS SINCE 2026-10-04, and a patch must carry them as the lane holds them: where they
	// stand, what they are set to, what they modulate, and how their card was left. Through the real bytes and a
	// FRESH bridge, every claim asserted as a value on the far side.
	namespace flt = feathers::fx::filter;
	feathers::ParamBridge bridge;
	REQUIRE(bridge.fxChain().count == 1); // the default filter, at position 0
	const size_t lfoAt = bridge.addFxDevice(feathers::mod::lfo::kTraits.tag);
	const size_t trackerAt = bridge.addFxDevice(feathers::mod::tracker::kTraits.tag);
	const size_t filterAt = bridge.addFxDevice(flt::kTraits.tag);
	REQUIRE(lfoAt == 1);
	REQUIRE(trackerAt == 2);
	REQUIRE(filterAt == 3);
	const feathers::DeviceInstanceId lfo = bridge.fxDeviceAt(lfoAt);
	const feathers::DeviceInstanceId tracker = bridge.fxDeviceAt(trackerAt);
	const feathers::DeviceInstanceId filter = bridge.fxDeviceAt(filterAt);
	const backplane::Source from = backplane::Source::output(lfo, 0);
	const backplane::Destination to = backplane::Destination::control(filter, static_cast<size_t>(flt::Param::Cutoff));
	bridge.setRouteAmount(from, to, 0.4f);
	REQUIRE(bridge.routeAmount(from, to) == doctest::Approx(0.4f)); // the route is LEGAL from the lane
	bridge.setDeviceFolded(lfo, true);
	const feathers::ModRack stripBefore = bridge.modRack();

	feathers::ParamBridge fresh;
	const feathers::rack::LoadReport report = fresh.restoreChains(throughBytes(bridge.chainsPatch()));
	CHECK(report.routes.count == 0);
	CHECK(sameDevices(fresh.fxChain(), bridge.fxChain())); // the whole rows, in their places
	CHECK(feathers::fxHolds(fresh.fxChain(), lfoAt, feathers::mod::lfo::kTraits.tag));
	CHECK(feathers::fxHolds(fresh.fxChain(), trackerAt, feathers::mod::tracker::kTraits.tag));
	CHECK(fresh.fxDeviceAt(lfoAt) == lfo);
	CHECK(fresh.fxDeviceAt(trackerAt) == tracker);
	CHECK(fresh.routeAmount(from, to) == doctest::Approx(0.4f));
	CHECK(fresh.deviceFolded(lfo));
	CHECK_FALSE(fresh.deviceFolded(tracker));
	// AND NOTHING MOVED INTO THE STRIP on the way: a loader that "corrected" a modulator into chain 0 would still
	// pass the route and the fold above.
	CHECK(sameDevices(fresh.modRack(), stripBefore));
}

// ---- THE SPECTRUM DRAWER'S PLOT (D2): the engine's frame to the editor, whole or not at all -----------------------------

namespace {

/// A frame whose every float is a function of `n`, so a reader can tell one publish's values from another's.
feathers::SpectralPlotFrame plotFrameNumbered(int n) {
	feathers::SpectralPlotFrame f;
	f.f0 = static_cast<float>(n);
	f.count = feathers::SpectralPlotFrame::kPartials;
	f.stereo = n % 2;
	f.playing = 1;
	for (size_t i = 0; i < f.ratio.size(); ++i) {
		f.ratio[i] = static_cast<float>(n) + 0.25f;
		f.terrain[i] = static_cast<float>(n) + 0.5f;
		f.left[i] = static_cast<float>(n) + 0.75f;
		f.right[i] = static_cast<float>(n) + 1.0f;
	}
	return f;
}

} // namespace

TEST_CASE("spectral plot: a published frame reads back whole under a new serial, and an unchanged frame publishes nothing") {
	ParamBridge bridge;
	feathers::SpectralPlotFrame got;
	uint32_t serial = 99;
	CHECK_FALSE(bridge.readPlot(got, serial)); // nothing published yet
	CHECK(serial == 99);
	CHECK(bridge.plotSerial() == 0);

	const feathers::SpectralPlotFrame one = plotFrameNumbered(1);
	CHECK(bridge.publishPlot(one));
	REQUIRE(bridge.readPlot(got, serial));
	CHECK(serial == 1);
	CHECK(got.sameAs(one));
	CHECK(got.count == feathers::SpectralPlotFrame::kPartials);
	CHECK(got.stereo == 1);
	CHECK(got.right.back() == 2.0f);

	CHECK_FALSE(bridge.publishPlot(one)); // the same frame: no new serial, nothing for the editor to redraw
	CHECK(bridge.plotSerial() == 1);

	feathers::SpectralPlotFrame two = one;
	two.left[17] = 0.0f; // one float moved is a new frame
	CHECK(bridge.publishPlot(two));
	REQUIRE(bridge.readPlot(got, serial));
	CHECK(serial == 2);
	CHECK(got.sameAs(two));
}

TEST_CASE("spectral plot: a reader racing the writer takes only whole frames") {
	// Not a proof — a race test never is, and TSan is not available here (the ordering argument is in gui_bridge.cpp) —
	// but it catches a reader that copies across two publishes: every float of a frame is a function of one number.
	static constexpr int kPublishes = 20000;
	ParamBridge bridge;
	std::atomic<bool> done { false };
	std::thread writer([&] {
		for (int n = 1; n <= kPublishes; ++n) {
			(void)bridge.publishPlot(plotFrameNumbered(n));
		}
		done.store(true);
	});
	int taken = 0;
	int torn = 0;
	uint32_t lastSerial = 0;
	feathers::SpectralPlotFrame got;
	while (!done.load() || taken == 0) {
		uint32_t serial = 0;
		if (!bridge.readPlot(got, serial)) {
			continue;
		}
		++taken;
		const float n = got.f0;
		bool whole = got.stereo == static_cast<int>(n) % 2;
		for (size_t i = 0; i < got.ratio.size(); ++i) {
			whole = whole && got.ratio[i] == n + 0.25f && got.terrain[i] == n + 0.5f && got.left[i] == n + 0.75f
			        && got.right[i] == n + 1.0f;
		}
		torn += whole ? 0 : 1;
		CHECK(serial >= lastSerial); // the serial only climbs
		lastSerial = serial;
	}
	writer.join();
	CHECK(torn == 0);
	CHECK(taken > 0);
	uint32_t serial = 0;
	REQUIRE(bridge.readPlot(got, serial));
	CHECK(got.sameAs(plotFrameNumbered(kPublishes)));
	CHECK(serial == static_cast<uint32_t>(kPublishes));
}

TEST_CASE("spectral plot: a drawer edit asks for a flush only while the drawer is open, until the engine has taken it") {
	// The master switch and the curve are not params, so on a sleeping plugin nothing else asks the host for the flush that
	// hands them to the engine and publishes the plot they make (ParamBridge::hasPendingWork).
	ParamBridge bridge;
	REQUIRE_FALSE(bridge.hasPendingWork());
	bridge.setSpectralCurveBand(0, 3, 0.5f);
	CHECK_FALSE(bridge.hasPendingWork()); // the drawer is closed: no plot to make
	bridge.setPlotWanted(true);
	CHECK(bridge.hasPendingWork()); // the edit the open plot has not shown
	bridge.clearSpectralMoved(); // the flush took it
	CHECK_FALSE(bridge.hasPendingWork());
	bridge.setSpectralOn(false);
	CHECK(bridge.hasPendingWork());
	bridge.clearSpectralMoved();
	bridge.flattenSpectralCurve(0);
	CHECK(bridge.hasPendingWork());
}

TEST_CASE("spectral plot: opening the drawer asks for a flush and republishes the frame it last published, unchanged or not") {
	// A SLEEPING PLUGIN RUNS NO process(), so opening the drawer must itself ask for the flush that publishes a frame; and
	// the editor dropped what it held on closing, so the next publish goes out even if it equals the last.
	ParamBridge bridge;
	REQUIRE_FALSE(bridge.hasPendingWork());
	bridge.setPlotWanted(true);
	CHECK(bridge.hasPendingWork());
	bridge.clearSpectralMoved();
	bridge.setPlotWanted(true); // already open: nothing new to ask for
	CHECK_FALSE(bridge.hasPendingWork());
	const feathers::SpectralPlotFrame frame = plotFrameNumbered(3);
	CHECK(bridge.publishPlot(frame));
	CHECK_FALSE(bridge.publishPlot(frame)); // open, unchanged: nothing
	bridge.setPlotWanted(false);
	bridge.setPlotWanted(true); // reopened
	CHECK(bridge.hasPendingWork());
	const uint32_t before = bridge.plotSerial();
	CHECK(bridge.publishPlot(frame)); // the same frame, republished
	CHECK(bridge.plotSerial() == before + 1);
	CHECK_FALSE(bridge.publishPlot(frame)); // and only once
}

TEST_CASE("OP: the Follower's Mode never takes a host slot — a touch seats none, and a saved map naming it loads without it, said") {
	// A DAW LANE FLIPPING MODE WOULD DELETE ROUTES MID-SONG (`ParamDesc::hostSlot` false). Attack, beside it, slots.
	namespace ff = feathers::mod::follower;
	const auto mode = static_cast<size_t>(ff::Param::Mode);
	const auto attack = static_cast<size_t>(ff::Param::Attack);
	REQUIRE_FALSE(ff::kParamDescs[mode].hostSlot);
	feathers::ParamBridge bridge;
	const size_t slot = bridge.addRackDevice(ff::kTraits.tag);
	REQUIRE(slot < feathers::kMaxRackSlots);
	const feathers::DeviceInstanceId f = bridge.rackDeviceAt(slot);
	bridge.touchControl(f, mode);
	CHECK(bridge.slotOwner().boundTo(f, mode) == feathers::hostslots::kNoSlot);
	CHECK_FALSE(bridge.hostSlotStatusOf(f, mode).slotted());
	CHECK_FALSE(bridge.hostSlotStatusOf(f, mode).canExpose);
	CHECK_FALSE(bridge.exposeToHost(f, mode, nullptr));
	// NOT VACUOUS: a touch on Attack does seat it.
	bridge.touchControl(f, attack);
	REQUIRE(bridge.hostSlotStatusOf(f, attack).slotted());

	// A SAVED MAP THAT NAMES MODE (written before Mode said so): the entry is re-pointed at Mode by key, and the load
	// drops it and reports it, while the Attack entry beside it is seated.
	feathers::rack::RackPatch patch = bridge.chainsPatch(true);
	REQUIRE(patch.hostSlots.has_value());
	REQUIRE(patch.hostSlots->slots.size() == 1);
	feathers::hostslots::SlotRecord modeEntry = patch.hostSlots->slots[0];
	modeEntry.index = patch.hostSlots->slots[0].index + 1;
	modeEntry.target.key = ff::kParamDescs[mode].key;
	patch.hostSlots->slots.push_back(modeEntry);
	feathers::ParamBridge fresh;
	const feathers::rack::LoadReport report = fresh.restoreChains(patch);
	CHECK(report.countOf(feathers::rack::LoadReport::Kind::SlotsDropped) == 1);
	CHECK_FALSE(fresh.hostSlotStatusOf(f, mode).slotted());
	CHECK(fresh.hostSlotStatusOf(f, attack).slotted());
}

//------------------------------------------------------------------------------------------------//
// BACKLOG PA: STEREO PHASE PERSISTS BY ITS KEY — saved, loaded into a FRESH bridge, the same value  //
//------------------------------------------------------------------------------------------------//

TEST_CASE("backlog PA: an MSEG's and an LFO's Stereo Phase are saved under `stereo_phase` and come back in a fresh instance, value for value") {
	namespace mseg = feathers::mod::mseg;
	namespace lfo = feathers::mod::lfo;
	feathers::ParamBridge bridge;
	const size_t mSlot = bridge.addRackDevice(mseg::kTraits.tag);
	const size_t lSlot = bridge.addRackDevice(lfo::kTraits.tag);
	REQUIRE(mSlot < feathers::kMaxRackSlots);
	REQUIRE(lSlot < feathers::kMaxRackSlots);
	const feathers::DeviceInstanceId m = bridge.rackDeviceAt(mSlot);
	const feathers::DeviceInstanceId l = bridge.rackDeviceAt(lSlot);
	// TWO DIFFERENT VALUES, NEITHER A DEFAULT NOR AN END: a load that swapped them or read either back as 0 or π fails.
	constexpr float kMseg = 1.0471976f;  // 60°
	constexpr float kLfo = 2.3561945f;   // 135°
	bridge.setDeviceControl(m, static_cast<size_t>(mseg::Param::StereoPhase), kMseg);
	bridge.setDeviceControl(l, static_cast<size_t>(lfo::Param::StereoPhase), kLfo);

	const feathers::rack::RackPatch saved = bridge.chainsPatch();
	// ON DISK BY KEY: each device's values carry `stereo_phase`, at its own number.
	const auto keyed = [&](feathers::DeviceInstanceId id) -> std::optional<float> {
		for (const feathers::rack::RackPatch::Device& d : saved.device) {
			if (d.instanceId != id) {
				continue;
			}
			for (const feathers::rack::RackPatch::Value& v : d.values) {
				if (v.key == "stereo_phase") {
					return v.number;
				}
			}
		}
		return std::nullopt;
	};
	REQUIRE(keyed(m).has_value());
	REQUIRE(keyed(l).has_value());
	CHECK(*keyed(m) == kMseg);
	CHECK(*keyed(l) == kLfo);

	feathers::ParamBridge fresh;
	(void)fresh.restoreChains(saved);
	const auto backOf = [&](const char* tag) {
		for (size_t slot = 0; slot < feathers::kMaxRackSlots; ++slot) {
			const feathers::DeviceInstanceId at = fresh.rackDeviceAt(slot);
			const feathers::device::Descriptor* d = at != feathers::kNoInstance ? fresh.chainRecord().descriptorOf(at) : nullptr;
			if (d != nullptr && feathers::device::sameTag(d->traits.tag, tag)) {
				return at;
			}
		}
		return feathers::kNoInstance;
	};
	const feathers::DeviceInstanceId m2 = backOf(mseg::kTraits.tag);
	const feathers::DeviceInstanceId l2 = backOf(lfo::kTraits.tag);
	REQUIRE(m2 != feathers::kNoInstance);
	REQUIRE(l2 != feathers::kNoInstance);
	CHECK(fresh.chainRecord().rowOf(m2).field[static_cast<size_t>(mseg::Param::StereoPhase)] == kMseg);
	CHECK(fresh.chainRecord().rowOf(l2).field[static_cast<size_t>(lfo::Param::StereoPhase)] == kLfo);
}
