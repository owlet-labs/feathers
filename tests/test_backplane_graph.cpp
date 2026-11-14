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
#include "test_heap.h"
#include "doctest.h"
#include "portable_aligned_alloc.h"

#include "backplane/chunked_pool.h"
#include "backplane/graph.h"
#include "backplane/route_list.h"
#include "backplane/routing.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <new>
#include <thread>
#include <utility>
#include <vector>

// THE GRAPH, WITH NO HOST ON IT — step 1 of the vision doc's path, verified per its §3.2 and the
// containers doc's required assertions: a reorder changes no route, ASSERTED ON THE TABLE; a lane
// reissued to a new node carries none of the old node's routes; two chains over one graph; the
// order; and the pool's allocation contract.
//
// EVERY DESCRIPTOR HERE IS A FAKE WITH NULL ENTRY POINTS, and that is enough: the graph never calls a
// device, it only asks which tables a device declares. Null IS the capability, so a fake whose
// `audio` is null is an honest "not an audio device" and not a stub.

using namespace feathers;
using namespace feathers::backplane;

namespace {

constexpr Axes kAxes {
	.coreSources = 4,
	.coreDestinations = 8,
	.deviceSlots = 6,
	.outputStride = 2,
};

/// Three controls of which two are modulatable, and NOT the first two — so an ordinal and a control
/// index differ, which is the distinction a route's destination must keep (`controlOf` below turns the
/// ordinal these cases speak into the control index the graph takes).
constexpr device::ParamDesc kParams[] = {
	{ .key = "mode", .name = "Mode", .min = 0, .max = 1, .def = 0, .stepped = true, .unit = device::Unit::Number,
	  .names = {}, .modulatable = false },
	{ .key = "cut", .name = "Cut", .min = 0, .max = 1, .def = 0.5, .stepped = false, .unit = device::Unit::Number,
	  .names = {}, .modulatable = true },
	{ .key = "res", .name = "Res", .min = 0, .max = 1, .def = 0, .stepped = false, .unit = device::Unit::Number,
	  .names = {}, .modulatable = true },
};

constexpr device::SourceDesc kOneOutput[] = { { .key = "out", .name = "Out", .bipolar = false } };

constexpr device::AudioOps kFakeAudio { .stateBytes = 16, .stateAlign = 16, .construct = nullptr,
	                                    .destruct = nullptr, .prepare = nullptr, .reset = nullptr,
	                                    .setSlot = nullptr, .setFields = nullptr,
	                                    .setNoteContext = nullptr, .setTransportClock = nullptr,
	                                    .processStereo = nullptr, .processMono = nullptr };
constexpr device::ModOps kFakeMod { .stateBytes = 16, .stateAlign = 16, .construct = nullptr,
	                                .destruct = nullptr, .prepare = nullptr, .reset = nullptr,
	                                .setFields = nullptr, .blockSamples = 32, .tick = nullptr,
	                                .outputAt = nullptr, .playhead = nullptr };
constexpr device::NoteOps kFakeNotes { .stateBytes = 16, .stateAlign = 16, .construct = nullptr,
	                                   .destruct = nullptr, .prepare = nullptr, .setFields = nullptr,
	                                   .processNotes = nullptr };

constexpr device::Traits kFxTraits { .tag = "TFX_", .name = "Fake fx", .hasCard = false,
	                                 .hasExpanded = false };
constexpr device::Traits kModTraits { .tag = "TMOD", .name = "Fake mod", .hasCard = false,
	                                  .hasExpanded = false };
constexpr device::Traits kNoteTraits { .tag = "TNOT", .name = "Fake note fx", .hasCard = false,
	                                   .hasExpanded = false };
constexpr device::Traits kBothTraits { .tag = "TBTH", .name = "Fake both", .hasCard = false,
	                                   .hasExpanded = false };

/// An effect: audio only, two modulatable controls, no outputs.
constexpr device::Descriptor kFx { .traits = kFxTraits, .params = kParams, .outputs = {},
	                               .audio = &kFakeAudio };
/// A modulator: one output, and it is also a destination through its own controls.
constexpr device::Descriptor kMod { .traits = kModTraits, .params = kParams, .outputs = kOneOutput,
	                                .mod = &kFakeMod };
/// A note effect that also processes audio.
constexpr device::Descriptor kNoteFx { .traits = kNoteTraits, .params = kParams, .outputs = {},
	                                   .audio = &kFakeAudio, .notes = &kFakeNotes };
/// A device that is BOTH an audio device and a modulator — the case a lane enum could not express.
constexpr device::Descriptor kBoth { .traits = kBothTraits, .params = kParams,
	                                 .outputs = kOneOutput, .audio = &kFakeAudio, .mod = &kFakeMod };

/// **A FOLLOWER — TEST-ONLY, BECAUSE NO DEVICE CAN HONESTLY DECLARE THIS YET (backlog IZ).** Its mod road
/// reads its own audio (`ModOps::readsAudio`), so its MOD instant FOLLOWS its AUDIO instant and its
/// outputs are downstream of everything that feeds it. It is `kBoth` with that one flag set, so any
/// difference between the two in a case below is the flag's.
constexpr device::ModOps kFakeFollowerMod { .stateBytes = 16, .stateAlign = 16, .construct = nullptr,
	                                        .destruct = nullptr, .prepare = nullptr, .reset = nullptr,
	                                        .setFields = nullptr, .blockSamples = 32, .tick = nullptr,
	                                        .outputAt = nullptr, .outputForNote = nullptr,
	                                        .playhead = nullptr, .readsAudio = true };
constexpr device::Traits kFollowerTraits { .tag = "TFOL", .name = "Fake follower", .hasCard = false,
	                                       .hasExpanded = false };
constexpr device::Descriptor kFollower { .traits = kFollowerTraits, .params = kParams,
	                                     .outputs = kOneOutput, .audio = &kFakeAudio,
	                                     .mod = &kFakeFollowerMod };

std::vector<DeviceInstanceId> idsOf(std::span<const Step> order) {
	std::vector<DeviceInstanceId> out;
	for (const Step& step : order) {
		out.push_back(step.id);
	}
	return out;
}

std::vector<DeviceInstanceId> idsOf(std::span<const DeviceInstanceId> path) {
	return std::vector<DeviceInstanceId>(path.begin(), path.end());
}

/// **HOW MANY ROUTES ARE LIVE — ASKED OF THE GRAPH, NOT OF ITS STORE.**
///
/// This took a `RouteTable&` and counted non-zero cells, which pinned the graph's storage rather than
/// its behaviour: six cases in this file would have gone red the moment a device-to-device route
/// stopped being a cell, while claiming nothing about what the graph does. The claim each of them
/// makes — "three routes survive that reorder", "the reissued lane carries none" — is about ROUTES,
/// and is unchanged.
///
/// An attempt to move the store found these six, and re-aiming them is the commit that has to come
/// first.
/// **AND SINCE KILL-LANES STEP 2 THERE IS NO STORE BESIDE THE EDGES TO COUNT** — every route, a host cell
/// included, is an edge between endpoints, so this walks them.
size_t countRoutes(const Graph& g) {
	size_t n = 0;
	g.forEachRoute([&n](const Route& r) { n += r.amount != 0.0f ? 1u : 0u; });
	return n;
}

/// **A ROUTE BETWEEN TWO NODES, SPELLED AS THESE CASES ALWAYS SPELLED IT** — an output of `from` and the
/// n-th MODULATABLE control of `to` — handed to the graph as its endpoints: the device's id and the
/// CONTROL INDEX that ordinal names (kill-lanes step 2). An ordinal past `to`'s modulatable controls
/// names the index one past its params, which the graph refuses exactly as it refused the ordinal.
Destination controlOf(const Graph& g, DeviceInstanceId to, size_t ordinal) {
	const Node* n = g.nodeOf(to);
	return Destination::control(to, n != nullptr ? n->desc->controlOfNthModulatable(ordinal) : ordinal);
}
bool route(Graph& g, Source s, Destination d, float amount) {
	return g.route(s, d, amount);
}
bool route(Graph& g, DeviceInstanceId from, size_t output, DeviceInstanceId to, size_t ordinal, float amount) {
	return g.route(Source::output(from, output), controlOf(g, to, ordinal), amount);
}
float routeAmount(const Graph& g, DeviceInstanceId from, size_t output, DeviceInstanceId to, size_t ordinal) {
	return g.amountOf(Source::output(from, output), controlOf(g, to, ordinal));
}

//------------------------------------------------------------------------------------//
// THE ALLOCATION PROBE — the idiom from tests/test_device_instance_pool.cpp, armed only  //
// around the calls under claim, because doctest itself allocates constantly.            //
//------------------------------------------------------------------------------------//

struct AllocCounter {
	bool armed = false;
	int allocations = 0;
};

AllocCounter allocs;

void* countedAlloc(size_t bytes, size_t align) {
	if (allocs.armed) {
		++allocs.allocations;
	}
	// An aligned request may ask for less than a pointer: see portable_aligned_alloc.h.
	void* p = feathers::test::portableAlignedAlloc(bytes, align);
	if (p == nullptr) {
		throw std::bad_alloc {};
	}
	return p;
}

} // namespace

void* operator new(size_t bytes) {
	return countedAlloc(bytes, alignof(std::max_align_t));
}
void* operator new[](size_t bytes) {
	return countedAlloc(bytes, alignof(std::max_align_t));
}
void* operator new(size_t bytes, std::align_val_t align) {
	return countedAlloc(bytes, static_cast<size_t>(align));
}
void* operator new[](size_t bytes, std::align_val_t align) {
	return countedAlloc(bytes, static_cast<size_t>(align));
}
void operator delete(void* p) noexcept {
	feathers::test::heapFree(p);
}
void operator delete[](void* p) noexcept {
	feathers::test::heapFree(p);
}
void operator delete(void* p, size_t) noexcept {
	feathers::test::heapFree(p);
}
void operator delete[](void* p, size_t) noexcept {
	feathers::test::heapFree(p);
}
void operator delete(void* p, std::align_val_t) noexcept {
	feathers::test::heapFree(p);
}
void operator delete[](void* p, std::align_val_t) noexcept {
	feathers::test::heapFree(p);
}
void operator delete(void* p, size_t, std::align_val_t) noexcept {
	feathers::test::heapFree(p);
}
void operator delete[](void* p, size_t, std::align_val_t) noexcept {
	feathers::test::heapFree(p);
}
#include "nothrow_new_forwarding.h"

//------------------------------------------------------------------------------------//
// IDENTITY AND THE POOL                                                                //
//------------------------------------------------------------------------------------//

TEST_CASE("hardening: reserving the top of the id space neither wraps to the no-id nor hands out the core's id") {
	InstanceCounter ids;
	ids.reserve(0xFFFFFFFFu);
	CHECK(ids.next() == InstanceCounter::kCeiling);
	CHECK(ids.next() != kNoInstance);
	// SPENT: the counter refuses rather than issue the core's id or wrap.
	CHECK(ids.issue() == kNoInstance);
	CHECK(ids.issue() == kNoInstance);

	InstanceCounter near;
	near.reserve(InstanceCounter::kCeiling - 3);
	const DeviceInstanceId a = near.issue();
	const DeviceInstanceId b = near.issue();
	CHECK(a == InstanceCounter::kCeiling - 2);
	CHECK(b == InstanceCounter::kCeiling - 1);
	CHECK(near.issue() == kNoInstance); // the next would be the core's id

	// A GRAPH REFUSES TO RESTORE A DEVICE UNDER THE CORE'S ID.
	InstanceCounter other;
	Graph g(kAxes, 1, 8, other);
	CHECK_FALSE(g.restore(kFx, 0xFFFFFFFFu, 0));
	CHECK(other.next() == kFirstInstance);
}

TEST_CASE("an identity is issued once and never reissued while the graph lives") {
	InstanceCounter ids;
	Graph g(kAxes, 1, 8, ids);

	const DeviceInstanceId a = g.add(kFx);
	const DeviceInstanceId b = g.add(kFx);
	REQUIRE(a == kFirstInstance);
	REQUIRE(b == a + 1);
	REQUIRE(g.laneOf(a) == 0);
	REQUIRE(g.laneOf(b) == 1);

	REQUIRE(g.remove(a));
	REQUIRE(g.laneOf(a) == Graph::kNoLane);
	REQUIRE(g.nodeOf(a) == nullptr);

	// The lane comes back; the identity does not.
	const DeviceInstanceId c = g.add(kFx);
	REQUIRE(g.laneOf(c) == 0);
	REQUIRE(c != a);
	REQUIRE(c > b);
	REQUIRE(ids.next() == c + 1);
}

TEST_CASE("the pool refuses a node past its capacity rather than growing") {
	InstanceCounter ids;
	Graph g(kAxes, 1, 8, ids);
	REQUIRE(g.capacity() == kAxes.deviceSlots);

	for (size_t i = 0; i < kAxes.deviceSlots; ++i) {
		REQUIRE(g.add(kFx) != kNoInstance);
	}
	REQUIRE(g.nodeCount() == kAxes.deviceSlots);

	const DeviceInstanceId next = ids.next();
	REQUIRE(g.add(kFx) == kNoInstance);
	REQUIRE(g.nodeCount() == kAxes.deviceSlots);
	REQUIRE(g.capacity() == kAxes.deviceSlots);
	// A refusal spends no identity either.
	REQUIRE(ids.next() == next);
}

TEST_CASE("the graph allocates at construction and never afterwards") {
	InstanceCounter ids;
	Graph g(kAxes, 2, 8, ids);
	const DeviceInstanceId m = g.add(kMod);
	REQUIRE(g.insert(0, 0, m));

	// CONTROL: the probe can see an allocation. Without this the zero below is a probe that may be
	// disarmed rather than a graph that does not allocate.
	allocs.armed = true;
	allocs.allocations = 0;
	{
		int* volatile p = new int(7);
		delete p;
	}
	allocs.armed = false;
	REQUIRE(allocs.allocations == 1);

	allocs.armed = true;
	allocs.allocations = 0;

	const DeviceInstanceId a = g.add(kFx);
	const DeviceInstanceId b = g.add(kBoth);
	const DeviceInstanceId n = g.add(kNoteFx);
	g.insert(1, 0, a);
	g.insert(1, 1, b);
	g.insert(1, 2, n);
	g.move(1, 0, 2);
	g.connect(EdgeKind::Audio, a, n);
	g.connect(EdgeKind::Notes, n, a); // refused: a is not a note device
	route(g, m, 0, a, 0, 0.5f);
	route(g, b, 0, m, 1, -0.25f);
	(void)g.order();
	(void)g.slots();
	(void)routeAmount(g, m, 0, a, 0);
	g.disconnect(EdgeKind::Audio, a, n);
	g.erase(b);
	g.remove(a);
	g.remove(m);
	REQUIRE(g.restore(kMod, m, 5));
	g.remove(n);
	g.remove(b);

	allocs.armed = false;
	REQUIRE(allocs.allocations == 0);
}

TEST_CASE("restore puts a node back at its saved lane under its saved id and the counter moves past it") {
	InstanceCounter ids;
	Graph g(kAxes, 1, 8, ids);

	// A loader: nothing has been added, and the patch says lane 3 held instance 5.
	REQUIRE(g.restore(kFx, 5, 3));
	REQUIRE(g.laneOf(5) == 3);
	REQUIRE(g.nodeOf(5)->desc == &kFx);
	REQUIRE(ids.next() == 6);

	// The next fresh node cannot collide with the restored one.
	const DeviceInstanceId fresh = g.add(kFx);
	REQUIRE(fresh == 6);
	REQUIRE(g.laneOf(fresh) == 0);

	// Refusals: a taken lane, a live id, an id that is not one, a lane past the pool.
	REQUIRE_FALSE(g.restore(kFx, 9, 3));
	REQUIRE_FALSE(g.restore(kFx, 5, 1));
	REQUIRE_FALSE(g.restore(kFx, kNoInstance, 1));
	REQUIRE_FALSE(g.restore(kFx, 9, kAxes.deviceSlots));
	REQUIRE(g.nodeCount() == 2);
}

//------------------------------------------------------------------------------------//
// CHAINS                                                                               //
//------------------------------------------------------------------------------------//

TEST_CASE("a reorder within a chain moves no LANE, deletes the routes it makes backwards, and leaves a core source's route alone") {
	// **THIS CASE WAS "a reorder within a chain changes no route asserted on the table", AND THAT
	// CLAIM IS NOW HALF TRUE.** It was written for step 2 — `slots_` indexed by lane, the graph
	// owning the order — to pin that a reorder moves nothing in the table because the cells are keyed
	// on lanes and lanes do not move. **That half still holds and is still asserted below.** The
	// other half — that the table is UNTOUCHED — was overturned on 2026-09-06: a reorder that breaks
	// routes deletes them, and they are gone for good. This reorder puts the
	// modulator `a` at the END of the chain, downstream of both effects it modulates, so both of its
	// routes are exactly the routes the decision deletes. A case asserting they survive would now be
	// asserting the design that was rejected.
	//
	// **THE CONTROL IS THE MACRO ROUTE.** A core source is the host's, not a device's; nothing can be
	// reordered past it, and `move` must not touch its cell — a clearing that took it would pass the
	// deletion assertions and be over-reaching. It survives, and it is asserted at its value.
	//
	// **AND SINCE BACKLOG IZ ONLY ONE OF `a`'s TWO ROUTES IS BACKWARDS.** A chain orders AUDIO instants
	// only, so a modulator's output into a plain effect (`c`) is legal wherever the two stand — it is
	// ticked in the modulators' pass and read when the effect runs — and survives the drag as the SECOND
	// control. What the drag forbids is the route into `b`, which ticks too: `a` would have to be ticked
	// before `b` and run after it, in one order. So `a` and `b` are both kinds here, and `c` an effect.
	InstanceCounter ids;
	Graph g(kAxes, 1, 8, ids);
	const DeviceInstanceId a = g.add(kBoth);
	const DeviceInstanceId b = g.add(kBoth);
	const DeviceInstanceId c = g.add(kFx);
	REQUIRE(g.insert(0, 0, a));
	REQUIRE(g.insert(0, 1, b));
	REQUIRE(g.insert(0, 2, c));

	REQUIRE(route(g, a, 0, c, 0, 0.75f));
	REQUIRE(route(g, a, 0, b, 1, -0.5f));
	// A core source (a macro, say) into b's first modulatable control — an edge with a HOST end since
	// kill-lanes step 2, with no node behind it and so no precedence.
	const Destination bFirst = controlOf(g, b, 0);
	REQUIRE(route(g, Source::core(1), bFirst, 0.25f));

	const size_t laneA = g.laneOf(a);
	const size_t laneB = g.laneOf(b);
	const size_t laneC = g.laneOf(c);
	REQUIRE(countRoutes(g) == 3);

	const std::optional<size_t> cleared = g.move(0, 0, 2); // a to the end: [b, c, a]
	REQUIRE(cleared.has_value());
	REQUIRE(idsOf(g.chain(0)) == std::vector<DeviceInstanceId> { b, c, a });

	// THE LANES DID NOT MOVE — step 2's claim, intact. (A route names its device by id now, so what
	// survives means what it meant whatever the lanes did; the lanes staying put is still the graph's.)
	REQUIRE(g.laneOf(a) == laneA);
	REQUIRE(g.laneOf(b) == laneB);
	REQUIRE(g.laneOf(c) == laneC);

	// THE ONE ROUTE `a` CAN NO LONGER DELIVER IS GONE, AND THE COUNT SAYS ONE.
	CHECK(*cleared == 1);
	CHECK(routeAmount(g, a, 0, b, 1) == doctest::Approx(0.0f));
	// THE ROUTE INTO THE EFFECT IS NOT (backlog IZ) — the effect is now UPSTREAM of `a` in the chain.
	CHECK(routeAmount(g, a, 0, c, 0) == doctest::Approx(0.75f));
	// AND THE MACRO'S IS NOT — the control that the clearing reaches only what the order forbids.
	CHECK(g.amountOf(Source::core(1), bFirst) == doctest::Approx(0.25f));
	CHECK(countRoutes(g) == 2);

	// MOVING BACK RESTORES NOTHING. A deleted route is gone — no edge, which is what "never routed"
	// also looks like. That consequence was put explicitly and confirmed.
	const std::optional<size_t> back = g.move(0, 2, 0);
	REQUIRE(back.has_value());
	CHECK(*back == 0);
	REQUIRE(idsOf(g.chain(0)) == std::vector<DeviceInstanceId> { a, b, c });
	CHECK(routeAmount(g, a, 0, b, 1) == doctest::Approx(0.0f));
	CHECK(routeAmount(g, a, 0, c, 0) == doctest::Approx(0.75f));
	CHECK(g.amountOf(Source::core(1), bFirst) == doctest::Approx(0.25f));
}

TEST_CASE("a lane reissued to a new node carries none of the old node's routes") {
	InstanceCounter ids;
	Graph g(kAxes, 1, 8, ids);
	const DeviceInstanceId m = g.add(kMod);
	const DeviceInstanceId x = g.add(kBoth); // routed INTO and OUT OF, so both directions are tested
	const DeviceInstanceId f = g.add(kFx);
	REQUIRE(g.insert(0, 0, m));
	REQUIRE(g.insert(0, 1, x));
	REQUIRE(g.insert(0, 2, f));
	REQUIRE(route(g, m, 0, x, 0, 0.5f));
	REQUIRE(route(g, x, 0, f, 1, 0.5f));
	REQUIRE_FALSE(route(g, x, 0, x, 1, 0.5f)); // to itself: pass-through refuses it
	// AND A HOST END EACH WAY — a macro into x, and x into the host's own parameter 3 — which were
	// cells of the lane's table slice until kill-lanes step 2 and are edges naming x now.
	REQUIRE(route(g, Source::core(2), controlOf(g, x, 1), 0.25f));
	REQUIRE(route(g, Source::output(x, 0), Destination::core(3), -0.75f));
	REQUIRE(countRoutes(g) == 4);
	const size_t laneX = g.laneOf(x);

	REQUIRE(g.remove(x));
	// EVERY ROUTE NAMING x IS GONE, WHICHEVER END IT WAS — the claim the table slice's clear made.
	REQUIRE(countRoutes(g) == 0);

	// The new node takes the freed lane AND the freed position, which is the strongest form of the
	// question: same address, same place in the chain, a different device.
	const DeviceInstanceId y = g.add(kBoth);
	REQUIRE(y != x);
	REQUIRE(g.laneOf(y) == laneX);
	REQUIRE(g.insert(0, 1, y));
	REQUIRE(g.positionOf(y) == 1);

	// THE NEW NODE INHERITS NOTHING at its reused lane — no route, from any end.
	REQUIRE(countRoutes(g) == 0);
	REQUIRE(routeAmount(g, m, 0, y, 0) == 0.0f);
	REQUIRE(routeAmount(g, y, 0, f, 1) == 0.0f);
	REQUIRE(g.amountOf(Source::core(2), controlOf(g, y, 1)) == 0.0f);
	REQUIRE(g.amountOf(Source::output(y, 0), Destination::core(3)) == 0.0f);
	// And a route made now is the new node's own.
	REQUIRE(route(g, m, 0, y, 0, 0.125f));
	REQUIRE(routeAmount(g, m, 0, y, 0) == doctest::Approx(0.125f));
}

TEST_CASE("a cross-chain route resolves and reordering the source chain does not move it") {
	InstanceCounter ids;
	Graph g(kAxes, 2, 8, ids);
	const DeviceInstanceId m1 = g.add(kMod);
	const DeviceInstanceId m2 = g.add(kMod);
	const DeviceInstanceId f1 = g.add(kFx);
	const DeviceInstanceId f2 = g.add(kFx);
	REQUIRE(g.insert(0, 0, m1));
	REQUIRE(g.insert(0, 1, m2));
	REQUIRE(g.insert(1, 0, f1));
	REQUIRE(g.insert(1, 1, f2));
	REQUIRE(g.chainOf(m2) == 0);
	REQUIRE(g.chainOf(f1) == 1);

	// The edge crosses chains and is an ordinary edge.
	REQUIRE(route(g, m2, 0, f1, 0, 0.6f));
	REQUIRE(routeAmount(g, m2, 0, f1, 0) == doctest::Approx(0.6f));
	const auto allRoutes = [&g] {
		std::vector<Route> out;
		g.forEachRoute([&out](const Route& r) { out.push_back(r); });
		return out;
	};
	const std::vector<Route> before = allRoutes();
	REQUIRE(before.size() == 1);

	// Reorder the SOURCE chain, then the destination chain. THE ROUTE — both its endpoints and its
	// amount — is untouched by either, which is what "does not move it" means by identity.
	REQUIRE(g.move(0, 1, 0));
	REQUIRE(idsOf(g.chain(0)) == std::vector<DeviceInstanceId> { m2, m1 });
	REQUIRE(allRoutes() == before);
	REQUIRE(g.move(1, 0, 1));
	REQUIRE(idsOf(g.chain(1)) == std::vector<DeviceInstanceId> { f2, f1 });
	REQUIRE(allRoutes() == before);
	REQUIRE(routeAmount(g, m2, 0, f1, 0) == doctest::Approx(0.6f));
}

TEST_CASE("a move along a chain is a rotation not a swap") {
	InstanceCounter ids;
	Graph g(kAxes, 1, 8, ids);
	const DeviceInstanceId a = g.add(kFx);
	const DeviceInstanceId b = g.add(kFx);
	const DeviceInstanceId c = g.add(kFx);
	const DeviceInstanceId d = g.add(kFx);
	for (DeviceInstanceId id : { a, b, c, d }) {
		REQUIRE(g.insert(0, 99, id)); // clamped to the end
	}
	REQUIRE(idsOf(g.chain(0)) == std::vector<DeviceInstanceId> { a, b, c, d });

	REQUIRE(g.move(0, 0, 3));
	REQUIRE(idsOf(g.chain(0)) == std::vector<DeviceInstanceId> { b, c, d, a });
	REQUIRE(g.move(0, 3, 1));
	REQUIRE(idsOf(g.chain(0)) == std::vector<DeviceInstanceId> { b, a, c, d });
	REQUIRE(g.positionOf(c) == 2);

	// The order follows the path, which is what makes the path an order and not a display list.
	REQUIRE(idsOf(g.order()) == std::vector<DeviceInstanceId> { b, a, c, d });

	REQUIRE_FALSE(g.move(0, 4, 0));
	REQUIRE_FALSE(g.move(1, 0, 1));
	REQUIRE(g.move(0, 2, 2));
}

TEST_CASE("a node sits in at most one chain") {
	InstanceCounter ids;
	Graph g(kAxes, 2, 8, ids);
	const DeviceInstanceId a = g.add(kFx);
	REQUIRE(g.insert(0, 0, a));
	REQUIRE_FALSE(g.insert(1, 0, a));
	REQUIRE_FALSE(g.insert(0, 1, a));
	REQUIRE(g.chain(1).empty());
	REQUIRE(g.chain(0).size() == 1);
	REQUIRE(g.chainOf(a) == 0);
	// A node that is nowhere cannot be placed either.
	REQUIRE_FALSE(g.insert(1, 0, 77));
	REQUIRE_FALSE(g.insert(1, 0, kNoInstance));
}

TEST_CASE("leaving a chain is not leaving the graph") {
	InstanceCounter ids;
	Graph g(kAxes, 1, 8, ids);
	const DeviceInstanceId m = g.add(kMod);
	const DeviceInstanceId f = g.add(kFx);
	REQUIRE(g.insert(0, 0, m));
	REQUIRE(g.insert(0, 1, f));
	REQUIRE(route(g, m, 0, f, 0, 0.3f));

	REQUIRE(g.erase(m));
	REQUIRE(g.chainOf(m) == Graph::kNoChain);
	REQUIRE(g.positionOf(m) == Graph::kNoPosition);
	REQUIRE(idsOf(g.chain(0)) == std::vector<DeviceInstanceId> { f });
	// Still in the pool, still routed, still in the order.
	REQUIRE(g.nodeOf(m) != nullptr);
	REQUIRE(g.laneOf(m) == 0);
	REQUIRE(routeAmount(g, m, 0, f, 0) == doctest::Approx(0.3f));
	REQUIRE(g.order().size() == 2);
	REQUIRE_FALSE(g.erase(m));
	// And it can rejoin.
	REQUIRE(g.insert(0, 1, m));
	REQUIRE(idsOf(g.chain(0)) == std::vector<DeviceInstanceId> { f, m });
}

//------------------------------------------------------------------------------------//
// EDGES                                                                                //
//------------------------------------------------------------------------------------//

TEST_CASE("an edge of a kind an endpoint does not declare cannot be constructed") {
	InstanceCounter ids;
	Graph g(kAxes, 1, 8, ids);
	const DeviceInstanceId fx = g.add(kFx);
	const DeviceInstanceId mod = g.add(kMod);
	const DeviceInstanceId note = g.add(kNoteFx);
	const DeviceInstanceId both = g.add(kBoth);

	// Audio: both ends must process audio.
	REQUIRE_FALSE(g.connect(EdgeKind::Audio, mod, fx));
	REQUIRE_FALSE(g.connect(EdgeKind::Audio, fx, mod));
	REQUIRE(g.connect(EdgeKind::Audio, fx, note));
	REQUIRE(g.connect(EdgeKind::Audio, both, fx));

	// Notes: both ends must handle notes.
	REQUIRE_FALSE(g.connect(EdgeKind::Notes, note, fx));
	REQUIRE_FALSE(g.connect(EdgeKind::Notes, fx, note));
	REQUIRE_FALSE(g.connect(EdgeKind::Notes, mod, note));

	// Modulation is not an explicit edge at all.
	REQUIRE_FALSE(g.connect(EdgeKind::Modulation, mod, fx));

	// And a route: only FROM a modulator, only to a modulatable ordinal.
	REQUIRE_FALSE(route(g, fx, 0, mod, 0, 1.0f));
	REQUIRE_FALSE(route(g, note, 0, fx, 0, 1.0f));
	REQUIRE_FALSE(route(g, mod, 1, fx, 0, 1.0f)); // the modulator has ONE output
	REQUIRE_FALSE(route(g, mod, 0, fx, 2, 1.0f)); // the fx has TWO modulatable controls
	REQUIRE_FALSE(route(g, mod, 0, 99, 0, 1.0f));
	REQUIRE(route(g, mod, 0, fx, 1, 1.0f));
	REQUIRE(route(g, both, 0, mod, 0, 1.0f));

	// THE TWO EXPLICIT EDGES THIS CASE MADE, asked BY KIND — `edgeCount()` counts routes too now
	// (a modulation route is an edge), so the bare number would fold this case's two routes in and
	// stop saying what it means.
	REQUIRE(g.edgeCount(EdgeKind::Audio) + g.edgeCount(EdgeKind::Notes) == 2);
	REQUIRE(g.edgeCount(EdgeKind::Modulation) == 2);
	REQUIRE(countRoutes(g) == 2);
	// Self-loops and duplicates are refused too.
	REQUIRE_FALSE(g.connect(EdgeKind::Audio, fx, fx));
	REQUIRE_FALSE(g.connect(EdgeKind::Audio, fx, note));
	// STILL THE SAME TWO EXPLICIT EDGES — a refusal made none. By kind, for the reason above.
	REQUIRE(g.edgeCount(EdgeKind::Audio) + g.edgeCount(EdgeKind::Notes) == 2);
	// THE REFUSED ENDS ARE NOT ADMITTED, whatever the other end — an fx publishes no output, and its
	// third ordinal names no control (the index one past its params). They were one-past-the-end
	// coordinates while routes were cells; an endpoint the graph does not admit is the same refusal.
	REQUIRE_FALSE(g.admits(Source::output(fx, 0), Destination::core(0)));
	REQUIRE_FALSE(g.admits(Source::core(0), controlOf(g, fx, 2)));
	REQUIRE(g.admits(Source::core(0), controlOf(g, fx, 1))); // and the control, not vacuous
}

TEST_CASE("an edge that would close a cycle is refused and nothing changes") {
	InstanceCounter ids;
	Graph g(kAxes, 1, 8, ids);
	const DeviceInstanceId a = g.add(kFx);
	const DeviceInstanceId b = g.add(kFx);
	const DeviceInstanceId c = g.add(kFx);
	REQUIRE(g.insert(0, 0, a));
	REQUIRE(g.insert(0, 1, b));
	REQUIRE(g.connect(EdgeKind::Audio, b, c));
	const std::vector<DeviceInstanceId> before = idsOf(g.order());
	REQUIRE(before == std::vector<DeviceInstanceId> { a, b, c });

	// c -> a would close a -> b -> c -> a, through one chain edge and one explicit edge.
	REQUIRE_FALSE(g.connect(EdgeKind::Audio, c, a));
	REQUIRE(g.edgeCount() == 1);
	REQUIRE_FALSE(g.connected(EdgeKind::Audio, c, a));
	REQUIRE(idsOf(g.order()) == before);
	REQUIRE(g.order().size() == g.nodeCount());

	// The same cycle attempted through a chain placement: put c BEFORE a while b -> c stands.
	REQUIRE_FALSE(g.insert(0, 0, c));
	REQUIRE(g.chainOf(c) == Graph::kNoChain);
	REQUIRE(idsOf(g.order()) == before);
	// And through a move: c after b is fine, then moving c to the front closes c -> a ... b -> c.
	REQUIRE(g.insert(0, 2, c));
	REQUIRE_FALSE(g.move(0, 2, 0));
	REQUIRE(idsOf(g.chain(0)) == std::vector<DeviceInstanceId> { a, b, c });
	REQUIRE(g.order().size() == 3);
}

TEST_CASE("removing a node drops its edges and its chain membership and frees its lane") {
	InstanceCounter ids;
	Graph g(kAxes, 1, 8, ids);
	const DeviceInstanceId a = g.add(kNoteFx);
	const DeviceInstanceId b = g.add(kNoteFx);
	const DeviceInstanceId c = g.add(kNoteFx);
	REQUIRE(g.insert(0, 0, a));
	REQUIRE(g.insert(0, 1, b));
	REQUIRE(g.connect(EdgeKind::Audio, a, c));
	REQUIRE(g.connect(EdgeKind::Notes, c, b));
	REQUIRE(g.connect(EdgeKind::Notes, a, b));
	REQUIRE(g.edgeCount() == 3);
	const size_t laneC = g.laneOf(c);

	REQUIRE(g.remove(c));
	REQUIRE(g.edgeCount() == 1);
	REQUIRE(g.connected(EdgeKind::Notes, a, b));
	REQUIRE_FALSE(g.connected(EdgeKind::Audio, a, c));
	REQUIRE_FALSE(g.connected(EdgeKind::Notes, c, b));
	REQUIRE_FALSE(g.nodeAtLane(laneC).live());
	REQUIRE(g.slots().descriptorAtLane(laneC) == nullptr);

	REQUIRE(g.remove(b));
	REQUIRE(g.edgeCount() == 0);
	REQUIRE(idsOf(g.chain(0)) == std::vector<DeviceInstanceId> { a });
	REQUIRE(g.nodeCount() == 1);
	REQUIRE(g.order().size() == 1);
	REQUIRE_FALSE(g.remove(b));
	REQUIRE_FALSE(g.remove(kNoInstance));
}

//------------------------------------------------------------------------------------//
// ORDER                                                                                //
//------------------------------------------------------------------------------------//

TEST_CASE("the order is the graph's: each tier follows its own chain, and an unrouted modulator is ordered by nothing") {
	// **THE CASE WAS `modulators precede audio devices in the order`, AND THAT CLAIM IS DELETED
	// (kill-lanes step 4b).** It was true because `recompute` preferred a modulator over any other ready
	// node — a tie-break by KIND, which is the band rule hiding in the sort. By design, ordering comes
	// only from the graph. A modulator that modulates nothing has no relation to any
	// audio device, so nothing says when it runs, and asserting that it runs first would be asserting
	// the tie-break rather than the graph.
	//
	// **WHAT SURVIVES IS THE REAL CLAIM AND IT IS THE ONE WORTH HAVING: the two tiers are independent,
	// and each follows its own chain.** Chain adjacency is per EDGE KIND, so the modulators order among
	// themselves and the audio devices among themselves, and neither constrains the other.
	InstanceCounter ids;
	Graph g(kAxes, 1, 8, ids);
	const DeviceInstanceId a = g.add(kFx);
	const DeviceInstanceId m1 = g.add(kMod);
	const DeviceInstanceId m2 = g.add(kMod);
	const DeviceInstanceId b = g.add(kFx);
	REQUIRE(g.insert(0, 0, a));
	REQUIRE(g.insert(0, 1, m1));
	REQUIRE(g.insert(0, 2, m2));
	REQUIRE(g.insert(0, 3, b));

	const auto rank = [&g](DeviceInstanceId id) { return g.rankOf(id); };
	// EACH TIER IN ITS OWN CHAIN ORDER.
	REQUIRE(rank(m1) < rank(m2));
	REQUIRE(rank(a) < rank(b));
	// The lanes ride along, contemporaneous with the ids.
	REQUIRE(g.order().size() == 4);
	REQUIRE(g.order()[0].lane == g.laneOf(g.order()[0].id));

	// Swap the two modulators along the chain: the modulator tier follows the path.
	REQUIRE(g.move(0, 2, 1));
	REQUIRE(rank(m2) < rank(m1));
	REQUIRE(rank(a) < rank(b)); // and the audio tier is untouched by it
	// And the audio tier reorders independently.
	REQUIRE(g.move(0, 3, 0));
	REQUIRE(idsOf(g.chain(0)) == std::vector<DeviceInstanceId> { b, a, m2, m1 });
	REQUIRE(rank(b) < rank(a));
	REQUIRE(rank(m2) < rank(m1));

	SUBCASE("and a ROUTE into a device that TICKS is what orders a modulator against it") {
		// The relation the tie-break was standing in for, stated as the thing that actually creates it.
		// **INTO A DEVICE THAT TICKS, SINCE BACKLOG IZ.** A route into a plain effect orders nothing: the
		// effect reads it when it runs, after every tick, so the passes keep it and the order need not.
		const DeviceInstanceId both = g.add(kBoth);
		REQUIRE(g.insert(0, 0, both)); // at the FRONT, so only the route can put m1 before it
		REQUIRE(g.rankOf(both) < g.rankOf(m1));
		REQUIRE(g.route(Source::output(m1, 0), Destination::control(both, kParams[1].modulatable ? 1 : 2), 0.5f));
		CHECK(g.rankOf(m1) < g.rankOf(both));
		// And into the effect: admitted, and the effect keeps its place.
		const size_t aWas = g.rankOf(a);
		REQUIRE(g.route(Source::output(m1, 0), Destination::control(a, kParams[1].modulatable ? 1 : 2), 0.5f));
		CHECK(g.rankOf(a) == aWas);
	}
}

TEST_CASE("the sort's tie-break is the chain's own order, not the lane — after a remove and an add the order still follows the chain") {
	// KILL-LANES 4c FOUND THIS THE DAY THE ENGINE BEGAN RUNNING `order()`: among ready nodes nothing
	// orders, the sort took THE LOWEST LANE — a pool address. Lanes follow the chain only until a device is
	// removed and another added: the newcomer takes the freed low lane and the chain's LAST device sorted
	// FIRST, so a rack ran in an order no surface showed (rack-editor's "a drag still MOVES after devices
	// have been removed and added" went red on it). The tie falls to the chain position now.
	InstanceCounter ids;
	Graph g(kAxes, 1, 8, ids);
	const DeviceInstanceId a = g.add(kMod);
	const DeviceInstanceId b = g.add(kFx);
	const DeviceInstanceId c = g.add(kMod);
	REQUIRE(g.insert(0, 0, a));
	REQUIRE(g.insert(0, 1, b));
	REQUIRE(g.insert(0, 2, c));
	const size_t laneOfA = g.laneOf(a);
	REQUIRE(g.remove(a));
	const DeviceInstanceId d = g.add(kFx);
	REQUIRE(g.laneOf(d) == laneOfA); // the freed lane, the lowest — the shape that sorted d first
	REQUIRE(g.insert(0, 2, d));       // at the END of the chain
	REQUIRE(idsOf(g.chain(0)) == std::vector<DeviceInstanceId> { b, c, d });
	// b and d are audio and path-ordered; c is a modulator nothing orders. By lane it would come out
	// { d, b, c } or { b, d, c }; by the chain it is the chain.
	CHECK(idsOf(g.order()) == std::vector<DeviceInstanceId> { b, c, d });
	// AND A NODE IN NO CHAIN COMES AFTER EVERY CHAIN, so a detached device never jumps a chained one.
	const DeviceInstanceId loose = g.add(kMod);
	REQUIRE(g.laneOf(loose) != Graph::kNoLane);
	CHECK(idsOf(g.order()) == std::vector<DeviceInstanceId> { b, c, d, loose });
}

TEST_CASE("a modulator between two effects constrains neither and a route is a precedence") {
	InstanceCounter ids;
	Graph g(kAxes, 1, 8, ids);
	const DeviceInstanceId a = g.add(kFx);
	const DeviceInstanceId m = g.add(kMod);
	const DeviceInstanceId b = g.add(kFx);
	REQUIRE(g.insert(0, 0, a));
	REQUIRE(g.insert(0, 1, m));
	REQUIRE(g.insert(0, 2, b));
	// a -> m -> b as a path; audio flows a -> b past m, so m owes nothing to a — and since kill-lanes
	// step 4b nothing owes anything to m either, so its place in the order is unconstrained. What the
	// case is about is the PAIR below: a route is a precedence, and a backwards one is refused.
	REQUIRE(idsOf(g.chain(0)) == std::vector<DeviceInstanceId> { a, m, b });
	REQUIRE(g.rankOf(a) < g.rankOf(b));

	// PASS-THROUGH: m modulates a device that is both, downstream of it; that device may NOT
	// modulate m back, because m already reaches it — the route is a precedence and the reverse
	// would close a cycle. The edge is never written.
	const DeviceInstanceId both = g.add(kBoth);
	REQUIRE(g.insert(0, 3, both));
	REQUIRE(route(g, m, 0, both, 0, 0.5f));
	REQUIRE_FALSE(route(g, both, 0, m, 0, 0.5f));
	REQUIRE(routeAmount(g, both, 0, m, 0) == 0.0f);
	REQUIRE(countRoutes(g) == 1);
	REQUIRE(g.order().size() == 4);
	// **AS RELATIONS, NOT AS A SEQUENCE (kill-lanes step 4b).** The whole order was assertable only while
	// a tie-break by KIND fixed the interleaving; what the case is about is the precedences, and those
	// are exactly what a rank comparison between two related nodes says.
	REQUIRE(g.rankOf(m) < g.rankOf(both));  // the route m -> both
	REQUIRE(g.rankOf(a) < g.rankOf(b));     // the audio tier's chain order
	REQUIRE(g.rankOf(b) < g.rankOf(both));

	// **A ROUTE FROM `both` INTO THE EFFECT BEFORE IT IS ADMITTED SINCE BACKLOG IZ, AND IT REVERSES
	// NOTHING.** It was refused — b precedes `both` in the audio tier, so "both -> b would cycle". It
	// would not: `both` publishes in the modulators' pass and b reads it when it runs, after every tick.
	// The audio tier's order is untouched by it, which is the assertion that makes the admission honest.
	REQUIRE(route(g, both, 0, b, 0, 0.5f));
	REQUIRE(g.rankOf(b) < g.rankOf(both));
	REQUIRE(route(g, both, 0, b, 0, 0.0f));
	// A detached modulator is ordered by nothing ...
	const DeviceInstanceId c = g.add(kMod);
	REQUIRE(g.rankOf(m) < g.rankOf(both)); // c is detached: nothing orders it, and nothing may assert one
	// ... until a route from `both` makes it downstream of `both`, which is downstream of a and b.
	REQUIRE(route(g, both, 0, c, 0, 0.5f));
	REQUIRE(g.rankOf(both) < g.rankOf(c)); // ... until the route makes it downstream of `both`
	// And removing the route removes the precedence.
	REQUIRE(route(g, both, 0, c, 0, 0.0f));
	REQUIRE(g.rankOf(m) < g.rankOf(both)); // c is detached: nothing orders it, and nothing may assert one
}

TEST_CASE("a modulation cycle is refused like an audio cycle") {
	InstanceCounter ids;
	Graph g(kAxes, 1, 8, ids);
	const DeviceInstanceId a = g.add(kBoth);
	const DeviceInstanceId b = g.add(kBoth);
	REQUIRE(g.insert(0, 0, a));
	REQUIRE(g.insert(0, 1, b));

	// PASS-THROUGH (by design): a modulates b, so b may not modulate a. The reverse route is
	// refused BEFORE its edge is written, and the order is untouched.
	REQUIRE(route(g, a, 0, b, 0, 0.5f));
	REQUIRE_FALSE(route(g, b, 0, a, 0, 0.5f));
	REQUIRE(routeAmount(g, a, 0, b, 0) == doctest::Approx(0.5f));
	REQUIRE(routeAmount(g, b, 0, a, 0) == 0.0f);
	REQUIRE(countRoutes(g) == 1);
	REQUIRE(g.order().size() == 2);
	REQUIRE(idsOf(g.order()) == std::vector<DeviceInstanceId> { a, b });
	// A self-route, the smallest cycle of all, is refused too.
	REQUIRE_FALSE(route(g, a, 0, a, 1, 0.25f));
	REQUIRE(countRoutes(g) == 1);
	// Changing an EXISTING route's amount is not a new precedence and is always admitted.
	REQUIRE(route(g, a, 0, b, 0, -0.75f));
	REQUIRE(routeAmount(g, a, 0, b, 0) == doctest::Approx(-0.75f));

	// A route IS a precedence: two detached modulators sit in lane order until a route orders them.
	const DeviceInstanceId m = g.add(kMod);
	const DeviceInstanceId n = g.add(kMod);
	REQUIRE(idsOf(g.order()) == std::vector<DeviceInstanceId> { a, b, m, n });
	REQUIRE(route(g, n, 0, m, 0, 0.5f));
	REQUIRE(idsOf(g.order()) == std::vector<DeviceInstanceId> { a, b, n, m });
	REQUIRE_FALSE(route(g, m, 0, n, 0, 0.5f));
	REQUIRE(routeAmount(g, m, 0, n, 0) == 0.0f);
	// And a longer cycle through the chain: n -> m by route, m -> a by route, then a -> n is refused
	// because a already reaches n through b? It does not — b reaches nothing — so a -> n is
	// admitted, and THEN m -> a would close n -> m -> a -> n.
	REQUIRE(route(g, m, 0, a, 0, 0.5f));
	REQUIRE(route(g, a, 0, n, 0, 0.5f) == false); // a -> n: n -> m -> a already, so refused
	REQUIRE(idsOf(g.order()) == std::vector<DeviceInstanceId> { n, m, a, b });

	// The same pair, audio: the chain already says a -> b, so b -> a is refused and the order stands.
	REQUIRE_FALSE(g.connect(EdgeKind::Audio, b, a));
	// NO AUDIO EDGE WAS MADE, which is this line's whole claim and is now said exactly. It read
	// `edgeCount() == 0` when routes were not edges; the routes above are edges now, so the bare
	// count is 3 and would have been asserting something else entirely.
	REQUIRE(g.edgeCount(EdgeKind::Audio) == 0);
	REQUIRE(idsOf(g.order()) == std::vector<DeviceInstanceId> { n, m, a, b });
	// Detached from the chain, b -> a is STILL refused, because the route a -> b is a precedence
	// and an audio edge against it would close a cycle across kinds; a -> b is admitted.
	REQUIRE(g.erase(a));
	REQUIRE(g.erase(b));
	REQUIRE_FALSE(g.connect(EdgeKind::Audio, b, a));
	REQUIRE(g.connect(EdgeKind::Audio, a, b));
	REQUIRE_FALSE(g.connect(EdgeKind::Audio, b, a));
	// ONE AUDIO EDGE — a -> b was admitted and b -> a refused, which is the claim. The routes still
	// standing from earlier in this case are edges too and are not what this line is about.
	REQUIRE(g.edgeCount(EdgeKind::Audio) == 1);
	REQUIRE(idsOf(g.order()) == std::vector<DeviceInstanceId> { n, m, a, b });
}

TEST_CASE("two chains keep their own paths, and each orders within itself") {
	InstanceCounter ids;
	Graph g(kAxes, 2, 8, ids);
	const DeviceInstanceId f1 = g.add(kFx);
	const DeviceInstanceId f2 = g.add(kFx);
	const DeviceInstanceId m1 = g.add(kMod);
	const DeviceInstanceId m2 = g.add(kMod);
	REQUIRE(g.insert(1, 0, f1));
	REQUIRE(g.insert(1, 1, f2));
	REQUIRE(g.insert(0, 0, m1));
	REQUIRE(g.insert(0, 1, m2));
	// **EACH CHAIN IN ITS OWN ORDER; THE INTERLEAVING BETWEEN THEM IS NOT A CLAIM (kill-lanes step 4b).**
	// The case was named *"the order interleaves modulators first"* and asserted the whole sequence — a
	// claim about the tie-break by KIND rather than about the graph. Two chains with no edge between them
	// are two disconnected components, and which one a topological sort emits first is arbitrary and
	// nothing may depend on it. (A host that needs them related declares its own node: `setCoreNode`.)
	REQUIRE(g.rankOf(m1) < g.rankOf(m2));
	REQUIRE(g.rankOf(f1) < g.rankOf(f2));
	REQUIRE(g.move(0, 0, 1));
	REQUIRE(g.rankOf(m2) < g.rankOf(m1));
	REQUIRE(g.rankOf(f1) < g.rankOf(f2));
	REQUIRE(idsOf(g.chain(1)) == std::vector<DeviceInstanceId> { f1, f2 });
	REQUIRE(g.chain(2).empty());
}

//------------------------------------------------------------------------------------//
// THE AUTHORITY OVER THE GRAPH'S OWN DEVICES                                           //
//------------------------------------------------------------------------------------//

TEST_CASE("the graph feeds the routing authority so only devices it holds admit a route") {
	// **IT WENT THROUGH `Graph::slots()` AND A `SlotView`, AND THAT ROAD IS DELETED (kill-lanes step 5).**
	// The `SlotView` overload of `mayRoute` compared two RANKS, which step 4b established is not the rule:
	// a rank is a position in one linearisation of a partial order, and adding a route changes the
	// linearisation. Keeping a case on it would have kept a second, wrong authority alive for the sake of
	// the case that used it.
	//
	// **NOT ONE OF THIS CASE'S CLAIMS WAS ABOUT THE ORDER**, which is why the port costs nothing: every
	// pair here is core-to-device or device-to-core, and the ordering clause never applied to those. What
	// it is about is LIVENESS and SLACK — a device the graph holds, an output it declares, a control it
	// declares modulatable — asked now of `Graph::devices()`, which is the authority the product uses.
	InstanceCounter ids;
	Graph g(kAxes, 1, 8, ids);
	const DeviceInstanceId m = g.add(kMod);
	const DeviceInstanceId f = g.add(kFx);
	const size_t modulatable = kFx.controlOfNthModulatableBesideEnable(0);
	REQUIRE(modulatable < kFx.params.size());

	// A macro into f's first modulatable control: admitted.
	REQUIRE(mayRoute(kAxes, g.devices(), Source::core(1), Destination::control(f, modulatable), &permitAll));
	// Into a control f does NOT declare modulatable: refused — a control is not a destination by existing.
	size_t notModulatable = kFx.params.size();
	for (size_t c = 0; c < kFx.params.size(); ++c) {
		if (!kFx.params[c].modulatable) {
			notModulatable = c;
			break;
		}
	}
	REQUIRE(notModulatable < kFx.params.size());
	REQUIRE_FALSE(mayRoute(kAxes, g.devices(), Source::core(1), Destination::control(f, notModulatable),
	                       &permitAll));
	// From m's one output into a core destination: admitted; from a second output it does not declare:
	// refused.
	REQUIRE(mayRoute(kAxes, g.devices(), Source::output(m, 0), Destination::core(3), &permitAll));
	REQUIRE_FALSE(mayRoute(kAxes, g.devices(), Source::output(m, 1), Destination::core(3), &permitAll));
	// **A DEVICE THE GRAPH DOES NOT HOLD ADMITS NOTHING IN EITHER DIRECTION** — the endpoint form of the
	// old "an empty lane", and a better claim: an id names one device forever, so this cannot be confused
	// with a slot that is merely vacant today.
	constexpr DeviceInstanceId kNobody = 9999;
	REQUIRE(g.laneOf(kNobody) == Graph::kNoLane);
	REQUIRE_FALSE(mayRoute(kAxes, g.devices(), Source::core(1), Destination::control(kNobody, 0), &permitAll));
	REQUIRE_FALSE(mayRoute(kAxes, g.devices(), Source::output(kNobody, 0), Destination::core(3), &permitAll));

	// And it tracks the pool: remove f and every route naming it goes dark.
	REQUIRE(g.remove(f));
	REQUIRE_FALSE(mayRoute(kAxes, g.devices(), Source::core(1), Destination::control(f, modulatable),
	                       &permitAll));
}

TEST_CASE("a move DELETES the routes the new order forbids, counts them, and refuses only for an explicit-edge cycle") {
	// **DECIDED: a reorder that breaks routes deletes them, and they are gone for good.** Confirmed
	// with the consequence spelled out first — an amount is a cell, deleting it writes zero, and
	// reordering back restores nothing.
	//
	// THIS REPLACED A REFUSAL THAT HAD NEVER FIRED: `move` used to roll back on any cycle, routes
	// included, and only never did because this table is empty in every product path until the rack
	// routes through `route()`. So there is no before-state to preserve; there is a decision to land.
	//
	// THREE CLAIMS, EACH WITH THE ASSERTION THAT MAKES IT NON-VACUOUS: the forbidden routes go (and
	// the count says how many); a route the order still permits SURVIVES (or "everything went" would
	// pass the first); and an explicit-edge cycle still REFUSES the move and deletes NOTHING — the
	// non-lossy half, which a first draft of `move` got wrong by clearing before it checked.
	//
	// **THE THREE DEVICES TICK AND RUN BOTH SINCE BACKLOG IZ.** A plain modulator's route into a plain
	// effect is legal wherever the two stand now (a chain orders audio instants only), so no drag could
	// forbid it and the case would have nothing to delete. What a drag forbids is a route between two
	// devices that are BOTH ticked and run, which the one order would have to hold both ways.
	InstanceCounter ids;
	Graph g(kAxes, 1, 8, ids);
	const DeviceInstanceId m = g.add(kBoth);
	const DeviceInstanceId fxA = g.add(kBoth);
	const DeviceInstanceId fxB = g.add(kBoth);
	REQUIRE(g.insert(0, 0, m));
	REQUIRE(g.insert(0, 1, fxA));
	REQUIRE(g.insert(0, 2, fxB));
	REQUIRE(route(g, m, 0, fxA, 0, 0.5f));
	REQUIRE(route(g, m, 0, fxB, 0, 0.25f));
	REQUIRE(routeAmount(g, m, 0, fxA, 0) == doctest::Approx(0.5f));
	REQUIRE(routeAmount(g, m, 0, fxB, 0) == doctest::Approx(0.25f));

	SUBCASE("moving the modulator BELOW both effects deletes both routes, and says two") {
		const std::optional<size_t> cleared = g.move(0, 0, 2); // [fxA, fxB, m]
		REQUIRE(cleared.has_value());
		CHECK(*cleared == 2);
		CHECK(routeAmount(g, m, 0, fxA, 0) == doctest::Approx(0.0f));
		CHECK(routeAmount(g, m, 0, fxB, 0) == doctest::Approx(0.0f));
		REQUIRE(idsOf(g.chain(0)) == std::vector<DeviceInstanceId> { fxA, fxB, m });

		// AND MOVING IT BACK RESTORES NOTHING — gone means gone.
		const std::optional<size_t> back = g.move(0, 2, 0); // [m, fxA, fxB] again
		REQUIRE(back.has_value());
		CHECK(*back == 0);
		CHECK(routeAmount(g, m, 0, fxA, 0) == doctest::Approx(0.0f));
		REQUIRE(idsOf(g.chain(0)) == std::vector<DeviceInstanceId> { m, fxA, fxB });
	}

	SUBCASE("moving the modulator between the effects deletes ONE route and keeps the other") {
		// THE CONTROL: a clearing that took everything would pass the subcase above.
		const std::optional<size_t> cleared = g.move(0, 0, 1); // [fxA, m, fxB]
		REQUIRE(cleared.has_value());
		CHECK(*cleared == 1);
		CHECK(routeAmount(g, m, 0, fxA, 0) == doctest::Approx(0.0f));  // fxA is now upstream of m
		CHECK(routeAmount(g, m, 0, fxB, 0) == doctest::Approx(0.25f)); // fxB is still downstream
	}

	SUBCASE("an explicit AUDIO cycle still refuses the move — and deletes nothing") {
		// THE NON-LOSSY HALF, AND THE FIRST DRAFT OF THIS SUBCASE COULD NOT SEE IT. It refused a move
		// of fxB above fxA — which never moves the modulator, so at the moment of clearing no route was
		// backwards and the lossy order (clear first, refuse after) had nothing to lose. Green under
		// the very mutation it was written against. **The fixture that bites needs ONE device that is
		// both a modulator and an audio device**, so the same move closes an audio cycle AND makes its
		// own route backwards: under the lossy order the route is cleared and then the move is refused,
		// a gesture that did nothing and destroyed work; under the correct order the edges are checked
		// first and nothing is touched.
		InstanceCounter ids2;
		Graph h(kAxes, 1, 8, ids2);
		const DeviceInstanceId both = h.add(kBoth); // publishes an output AND carries audio
		const DeviceInstanceId fx1 = h.add(kFx);
		// `kBoth`, since backlog IZ: a route into a plain effect is legal wherever it stands, so a lossy
		// clearing would have nothing to take and this subcase would be green under it.
		const DeviceInstanceId fx2 = h.add(kBoth);
		REQUIRE(h.insert(0, 0, fx1));
		REQUIRE(h.insert(0, 1, both));
		REQUIRE(h.insert(0, 2, fx2));                        // [fx1, both, fx2]
		REQUIRE(route(h, both, 0, fx2, 0, 0.5f));            // forward: both is upstream of fx2
		REQUIRE(h.connect(EdgeKind::Audio, both, fx2));      // agrees with the chain
		REQUIRE(routeAmount(h, both, 0, fx2, 0) == doctest::Approx(0.5f));

		// MOVE `both` TO THE END: [fx1, fx2, both]. The chain now says fx2 before both, the explicit
		// audio edge says both before fx2 — a cycle through an edge, so the move is REFUSED. And the
		// route both -> fx2 is now backwards — exactly what a clearing done BEFORE the check would
		// delete. It must survive.
		const std::optional<size_t> refused = h.move(0, 1, 2);
		CHECK_FALSE(refused.has_value());
		REQUIRE(idsOf(h.chain(0)) == std::vector<DeviceInstanceId> { fx1, both, fx2 });
		CHECK(routeAmount(h, both, 0, fx2, 0) == doctest::Approx(0.5f));
	}
}

//------------------------------------------------------------------------------------------------//
// THE HOST'S OWN NODE (kill-lanes step 4a)                                                         //
//------------------------------------------------------------------------------------------------//

TEST_CASE("the host's own node joins the chains: a route into a core destination is a precedence, and the node precedes every audio head") {
	// **WITHOUT IT, A HOST WITH TWO CHAINS IS TWO DISCONNECTED COMPONENTS.** Nothing relates a strip
	// modulator to an fx device, so the order may interleave them freely and the only thing that kept
	// modulators first was a tie-break by KIND inside `recompute`. By design, ordering comes
	// only from the graph. This is the node that supplies the missing relations.
	InstanceCounter ids;
	Graph g(kAxes, 2, 8, ids);
	REQUIRE(g.coreNode() == kNoInstance); // a host declares one; it is not automatic
	REQUIRE(g.setCoreNode());
	REQUIRE(g.coreNode() != kNoInstance);
	REQUIRE_FALSE(g.setCoreNode()); // once only

	const DeviceInstanceId f1 = g.add(kFx);
	const DeviceInstanceId f2 = g.add(kFx);
	const DeviceInstanceId m = g.add(kMod);
	REQUIRE(g.insert(1, 0, f1));
	REQUIRE(g.insert(1, 1, f2));
	REQUIRE(g.insert(0, 0, m));

	// THE NODE IS NOT A DEVICE SLOT, and nothing that counts device slots may see it: a caller indexing
	// a slot view by a coordinate must not find a node no coordinate names.
	CHECK(g.capacity() == kAxes.deviceSlots);
	CHECK(g.slots().size() == kAxes.deviceSlots);

	// **IT PRECEDES THE HEAD OF THE AUDIO CHAIN**, and the chain's own adjacency carries the rest — so
	// the relation is derived at every recompute rather than stored as an edge somebody has to maintain
	// when the head changes. Asserted as ORDER rather than as an edge count, because the order is the
	// thing anything downstream reads.
	const std::vector<DeviceInstanceId> order = idsOf(g.order());
	const auto at = [&order](DeviceInstanceId id) {
		return std::find(order.begin(), order.end(), id) - order.begin();
	};
	CHECK(at(g.coreNode()) < at(f1));
	CHECK(at(f1) < at(f2));

	SUBCASE("a route into a CORE destination orders its source before the node") {
		// The modulator reaches a voice parameter, so it must be computed before the voice reads it.
		// Nothing about the two CHAINS says that; the route does.
		REQUIRE(g.route(Source::output(m, 0), Destination::core(0), 0.5f));
		const std::vector<DeviceInstanceId> after = idsOf(g.order());
		const auto pos = [&after](DeviceInstanceId id) {
			return std::find(after.begin(), after.end(), id) - after.begin();
		};
		CHECK(pos(m) < pos(g.coreNode()));
		CHECK(pos(g.coreNode()) < pos(f1)); // and the node still heads the audio chain
	}
}

TEST_CASE("a route that would loop through the host's node is REFUSED — the hole the rank rule never covered") {
	// **DECIDED (edges design §20, as written): modulation is pass-through, a route that
	// would close a cycle is refused, and a delayed loop is not expressible.** `route()` applied that
	// rule only when BOTH ends were devices, so every route pointing at a core destination — anything
	// modulating a voice parameter — escaped it entirely. With the host's node in the graph the rule
	// reaches them, which is a hole closed rather than a capability removed.
	//
	// **`kBoth` RATHER THAN `kFx`, AND THE FIRST DRAFT OF THIS CASE GOT IT WRONG.** `kFx` declares no
	// outputs, so `Source::output(f1, 0)` is refused by `admits` before any cycle test runs — the case
	// went green while proving nothing about the thing it names. The source has to be a device that can
	// BE a source and that is genuinely downstream of the node.
	//
	// **SINCE BACKLOG IZ THE LOOP NEEDS A FOLLOWER.** A device downstream of the voice that is ticked in
	// the modulators' pass (`kBoth`, SKLITTER) publishes BEFORE the voice reads a parameter, so its route
	// into one closes nothing and is admitted — "downstream" means computed from downstream audio.
	// A device whose outputs ARE computed from its audio (`kFollower`) is the loop.
	InstanceCounter ids;
	Graph g(kAxes, 2, 8, ids);
	REQUIRE(g.setCoreNode());
	const DeviceInstanceId follower = g.add(kFollower);
	const DeviceInstanceId both = g.add(kBoth);
	REQUIRE(g.insert(1, 0, follower));
	REQUIRE(g.insert(1, 1, both));

	// THE PREMISE, ASSERTED, or the refusal below proves nothing: the follower carries audio, so it heads
	// the audio chain and the node precedes it.
	const std::vector<DeviceInstanceId> order = idsOf(g.order());
	REQUIRE(std::find(order.begin(), order.end(), g.coreNode())
	        < std::find(order.begin(), order.end(), follower));
	REQUIRE(std::find(order.begin(), order.end(), follower) < std::find(order.begin(), order.end(), both));
	// AND IT IS A LEGAL SOURCE, which is the half the first draft skipped: the refusal below must be the
	// cycle rather than "that device publishes nothing".
	const size_t control = kParams[1].modulatable ? 1 : 2;
	REQUIRE(g.route(Source::output(follower, 0), Destination::control(both, control), 0.5f));
	REQUIRE(g.route(Source::output(follower, 0), Destination::control(both, control), 0.0f));

	// THE FOLLOWER is downstream of the voice, so it modulating a voice parameter closes the loop.
	CHECK(g.wouldCycle(Source::output(follower, 0), Destination::core(0)));
	CHECK_FALSE(g.route(Source::output(follower, 0), Destination::core(0), 0.5f));
	CHECK(g.amountOf(Source::output(follower, 0), Destination::core(0)) == 0.0f);

	// AND `both`, further downstream still, is ADMITTED — its tick precedes the voice's read.
	CHECK_FALSE(g.wouldCycle(Source::output(both, 0), Destination::core(0)));
	CHECK(g.route(Source::output(both, 0), Destination::core(0), 0.5f));
	CHECK(g.amountOf(Source::output(both, 0), Destination::core(0)) == doctest::Approx(0.5f));

	SUBCASE("and a host with NO core node is unaffected — the rack, whose macros are inputs") {
		InstanceCounter rackIds;
		Graph bare(kAxes, 1, 8, rackIds);
		const DeviceInstanceId d = bare.add(kBoth);
		REQUIRE(bare.insert(0, 0, d));
		REQUIRE(bare.coreNode() == kNoInstance);
		// No node owns the core destinations, so nothing is downstream of them and the route stands.
		CHECK(bare.route(Source::output(d, 0), Destination::core(0), 0.5f));
	}
}

TEST_CASE("WHAT A HAND GAINS: an effect may modulate a modulator that reaches only effects after it") {
	// **THE CONSEQUENCE §2.3 PROMISED AND THE POINT OF STEP 4.** `fx1 -> env -> fx2` — an fx device's
	// output modulating a strip modulator whose own routes reach only fx devices DOWNSTREAM of it. The
	// band rule refused it because a modulator was defined to be ahead of every audio device by KIND;
	// nothing about it is cyclic, and the graph admits it.
	//
	// `kBoth` for the effects because `kFx` publishes no outputs and so cannot be a source at all — the
	// trap that made the first draft of the refusal case above vacuous.
	InstanceCounter ids;
	Graph g(kAxes, 2, 8, ids);
	const DeviceInstanceId fx1 = g.add(kBoth);
	const DeviceInstanceId fx2 = g.add(kBoth);
	const DeviceInstanceId env = g.add(kMod);
	REQUIRE(g.insert(1, 0, fx1));
	REQUIRE(g.insert(1, 1, fx2));
	REQUIRE(g.insert(0, 0, env));
	const size_t control = kParams[1].modulatable ? 1 : 2;

	// fx1 -> env, then env -> fx2. Both admitted, and each is a precedence.
	REQUIRE(g.route(Source::output(fx1, 0), Destination::control(env, control), 0.5f));
	REQUIRE(g.route(Source::output(env, 0), Destination::control(fx2, control), 0.5f));
	CHECK(g.rankOf(fx1) < g.rankOf(env));
	CHECK(g.rankOf(env) < g.rankOf(fx2));

	SUBCASE("and the loop it would make IS still refused — the rule is a cycle, not a kind") {
		// env now reaches fx2, so fx2 modulating env would close the loop. Refused, and the edge is
		// never written — which is what makes the permission above a rule rather than an absence of one.
		CHECK_FALSE(g.route(Source::output(fx2, 0), Destination::control(env, control), 0.5f));
		CHECK(g.amountOf(Source::output(fx2, 0), Destination::control(env, control)) == 0.0f);
	}
}

//------------------------------------------------------------------------------------------------//
// BACKLOG IZ — TWO INSTANTS PER DEVICE: MOD BEFORE AUDIO                                           //
//------------------------------------------------------------------------------------------------//
//
// For modulation, "downstream" means COMPUTED FROM DOWNSTREAM AUDIO; a modulator's position in a
// lane is only grouping. Every case below asks `wouldCycle` (the gate) and
// `route()` (the store) separately, because the honesty rule is broken the moment they disagree.

TEST_CASE("IZ: a modulator's output reaches an effect BEFORE it in the chain, and the audio order does not move") {
	InstanceCounter ids;
	Graph g(kAxes, 1, 8, ids);
	const DeviceInstanceId fx = g.add(kFx);
	const DeviceInstanceId both = g.add(kBoth);
	const DeviceInstanceId mod = g.add(kMod);
	REQUIRE(g.insert(0, 0, fx));
	REQUIRE(g.insert(0, 1, both));
	REQUIRE(g.insert(0, 2, mod));
	const Destination fxControl = controlOf(g, fx, 0);

	// SKLITTER'S SHAPE: a device that ticks AND runs, after the effect it modulates.
	CHECK_FALSE(g.wouldCycle(Source::output(both, 0), fxControl));
	CHECK(g.route(Source::output(both, 0), fxControl, 0.5f));
	// A PLAIN MODULATOR after both of them, into each: grouping, not a routing rule.
	CHECK_FALSE(g.wouldCycle(Source::output(mod, 0), fxControl));
	CHECK(route(g, mod, 0, fx, 1, 0.25f));
	CHECK_FALSE(g.wouldCycle(Source::output(mod, 0), controlOf(g, both, 0)));
	CHECK(route(g, mod, 0, both, 0, 0.25f));
	CHECK(countRoutes(g) == 3);

	// THE AUDIO ORDER IS THE CHAIN'S, whatever was routed backwards along it — the effect runs first.
	CHECK(g.rankOf(fx) < g.rankOf(both));
	// And the modulator that reaches `both`, which ticks, is ordered before it — the one precedence here
	// that DOES bind the order.
	CHECK(g.rankOf(mod) < g.rankOf(both));
}

TEST_CASE("IZ: a strip modulator may reach one to its LEFT, and the route orders them") {
	InstanceCounter ids;
	Graph g(kAxes, 1, 8, ids);
	const DeviceInstanceId m1 = g.add(kMod);
	const DeviceInstanceId m2 = g.add(kMod);
	REQUIRE(g.insert(0, 0, m1));
	REQUIRE(g.insert(0, 1, m2));
	// THE PREMISE: with nothing routed the chain's own order is the order (the tie-break).
	REQUIRE(g.rankOf(m1) < g.rankOf(m2));

	CHECK_FALSE(g.wouldCycle(Source::output(m2, 0), controlOf(g, m1, 0)));
	REQUIRE(route(g, m2, 0, m1, 0, 0.5f));
	// Ticked in the order a route needs, which is not the lane's.
	CHECK(g.rankOf(m2) < g.rankOf(m1));
	// AND THE LOOP BACK IS STILL A LOOP — the instants refuse it, not the chain.
	CHECK(g.wouldCycle(Source::output(m1, 0), controlOf(g, m2, 0)));
	CHECK_FALSE(route(g, m1, 0, m2, 0, 0.5f));
	CHECK(routeAmount(g, m1, 0, m2, 0) == 0.0f);
}

TEST_CASE("IZ: two devices that both TICK and RUN cannot bind the one order both ways") {
	// **THE ONE REFUSAL THE INSTANTS ALONE WOULD MISS.** `a` before `b` in the chain, and `b`'s output
	// into `a`: as instants that is MOD(b) -> MOD(a) -> AUDIO(a) -> AUDIO(b), acyclic. But a host walks
	// ONE device order twice — the modulators' pass, then the audio pass — so it would have to tick `b`
	// before `a` and run `a` before `b` at once. Whichever it chose, one of them reads a block late.
	InstanceCounter ids;
	Graph g(kAxes, 1, 8, ids);
	const DeviceInstanceId a = g.add(kBoth);
	const DeviceInstanceId b = g.add(kBoth);
	REQUIRE(g.insert(0, 0, a));
	REQUIRE(g.insert(0, 1, b));

	CHECK(g.wouldCycle(Source::output(b, 0), controlOf(g, a, 0)));
	CHECK_FALSE(route(g, b, 0, a, 0, 0.5f));
	CHECK(routeAmount(g, b, 0, a, 0) == 0.0f);
	// FORWARD IS FINE — the non-vacuity control: a refusal of everything would pass the three above.
	CHECK_FALSE(g.wouldCycle(Source::output(a, 0), controlOf(g, b, 0)));
	CHECK(route(g, a, 0, b, 0, 0.5f));

	SUBCASE("and a plain effect between them does not change the answer") {
		// Refused through the audio chain rather than adjacency: a -> fx -> b.
		InstanceCounter ids2;
		Graph h(kAxes, 1, 8, ids2);
		const DeviceInstanceId first = h.add(kBoth);
		const DeviceInstanceId fx = h.add(kFx);
		const DeviceInstanceId last = h.add(kBoth);
		REQUIRE(h.insert(0, 0, first));
		REQUIRE(h.insert(0, 1, fx));
		REQUIRE(h.insert(0, 2, last));
		CHECK(h.wouldCycle(Source::output(last, 0), controlOf(h, first, 0)));
		CHECK_FALSE(route(h, last, 0, first, 0, 0.5f));
		// …while `last` into the effect between them is IZ's new reach.
		CHECK_FALSE(h.wouldCycle(Source::output(last, 0), controlOf(h, fx, 0)));
		CHECK(route(h, last, 0, fx, 0, 0.5f));
	}
}

TEST_CASE("IZ: a device that DECLARES its mod road reads audio stays downstream-only — the follower") {
	// `kFollower` is `kBoth` with `ModOps::readsAudio` set. Every pair below is asked of both, so a
	// difference is the flag's and nothing else's.
	InstanceCounter ids;
	Graph g(kAxes, 2, 8, ids);
	REQUIRE(g.setCoreNode());
	const DeviceInstanceId before = g.add(kFx);
	const DeviceInstanceId follower = g.add(kFollower);
	const DeviceInstanceId both = g.add(kBoth);
	const DeviceInstanceId after = g.add(kFx);
	const DeviceInstanceId strip = g.add(kMod);
	REQUIRE(g.insert(1, 0, before));
	REQUIRE(g.insert(1, 1, follower));
	REQUIRE(g.insert(1, 2, both));
	REQUIRE(g.insert(1, 3, after));
	REQUIRE(g.insert(0, 0, strip));

	const Source fromFollower = Source::output(follower, 0);
	const Source fromBoth = Source::output(both, 0);

	// UPSTREAM EFFECT: the follower's output is computed from audio that effect produced — a loop.
	// `both`'s is not, and reaches it.
	CHECK(g.wouldCycle(fromFollower, controlOf(g, before, 0)));
	CHECK_FALSE(g.route(fromFollower, controlOf(g, before, 0), 0.5f));
	CHECK_FALSE(g.wouldCycle(fromBoth, controlOf(g, before, 0)));
	CHECK(g.route(fromBoth, controlOf(g, before, 0), 0.5f));

	// THE VOICE: upstream of the whole lane, so the same answer.
	CHECK(g.wouldCycle(fromFollower, Destination::core(0)));
	CHECK_FALSE(g.route(fromFollower, Destination::core(0), 0.5f));
	CHECK_FALSE(g.wouldCycle(fromBoth, Destination::core(0)));
	CHECK(g.route(fromBoth, Destination::core(0), 0.5f));

	// DOWNSTREAM EFFECT: legal for both — the follower is downstream-ONLY, not nowhere.
	CHECK_FALSE(g.wouldCycle(fromFollower, controlOf(g, after, 0)));
	CHECK(g.route(fromFollower, controlOf(g, after, 0), 0.5f));
	CHECK(g.route(fromBoth, controlOf(g, after, 1), 0.5f));

	// A STRIP MODULATOR that reaches the voice is upstream of the follower's audio; the follower routed
	// into it would close MOD(strip) -> voice -> the lane -> MOD(follower) -> MOD(strip).
	REQUIRE(g.route(Source::output(strip, 0), Destination::core(1), 0.5f));
	CHECK(g.wouldCycle(fromFollower, controlOf(g, strip, 0)));
	CHECK_FALSE(g.route(fromFollower, controlOf(g, strip, 0), 0.5f));
	CHECK_FALSE(g.wouldCycle(fromBoth, controlOf(g, strip, 0)));

	// ITS OWN CONTROL: refused, as for any device that declares no `SelfRouting` (trap T1: a node
	// reaches itself).
	CHECK(g.wouldCycle(fromFollower, controlOf(g, follower, 0)));
	CHECK_FALSE(g.route(fromFollower, controlOf(g, follower, 0), 0.5f));
}

TEST_CASE("IZ: a drag re-admits every route by the rule route() asks — a plain modulator's routes survive any drag") {
	InstanceCounter ids;
	Graph g(kAxes, 1, 8, ids);
	const DeviceInstanceId m = g.add(kMod);
	const DeviceInstanceId fx = g.add(kFx);
	const DeviceInstanceId both = g.add(kBoth);
	REQUIRE(g.insert(0, 0, m));
	REQUIRE(g.insert(0, 1, fx));
	REQUIRE(g.insert(0, 2, both));
	REQUIRE(route(g, m, 0, fx, 0, 0.5f));
	REQUIRE(route(g, m, 0, both, 0, 0.25f));
	REQUIRE(route(g, both, 0, fx, 1, 0.75f)); // backwards along the chain, legal (IZ)

	// EVERY PERMUTATION A DRAG CAN REACH, and no route is ever deleted: none of these three binds the one
	// order against the audio chain (`m` does not run; `fx` does not tick).
	for (size_t from = 0; from < 3; ++from) {
		for (size_t to = 0; to < 3; ++to) {
			CAPTURE(from);
			CAPTURE(to);
			const std::optional<size_t> cleared = g.move(0, from, to);
			REQUIRE(cleared.has_value());
			CHECK(*cleared == 0);
			CHECK(routeAmount(g, m, 0, fx, 0) == doctest::Approx(0.5f));
			CHECK(routeAmount(g, m, 0, both, 0) == doctest::Approx(0.25f));
			CHECK(routeAmount(g, both, 0, fx, 1) == doctest::Approx(0.75f));
			// And the order still honours every route that binds it.
			CHECK(g.rankOf(m) < g.rankOf(both));
		}
	}
}

/// **A NOTE LISTENER THAT MODULATES — the DAHDSR's, the MSEG's and the LFO's shape since 2026-10-03**: a per-note
/// table fed by the note road, every event passed through (`NoteOps::listensOnly`).
constexpr device::NoteOps kFakeListenerNotes { .stateBytes = 16, .stateAlign = 16, .construct = nullptr,
	                                           .destruct = nullptr, .prepare = nullptr, .setFields = nullptr,
	                                           .processNotes = nullptr, .listensOnly = true };
constexpr device::Traits kListenerTraits { .tag = "TLIS", .name = "Fake listener", .hasCard = false,
	                                       .hasExpanded = false };
constexpr device::Descriptor kListener { .traits = kListenerTraits, .params = kParams, .outputs = kOneOutput,
	                                     .mod = &kFakeMod, .notes = &kFakeListenerNotes };
/// And one that WRITES notes as well as ticking — what a listener must still be ordered against.
constexpr device::Traits kWriterTraits { .tag = "TWRI", .name = "Fake writer", .hasCard = false,
	                                     .hasExpanded = false };
constexpr device::Descriptor kWriter { .traits = kWriterTraits, .params = kParams, .outputs = kOneOutput,
	                                   .audio = &kFakeAudio, .mod = &kFakeMod, .notes = &kFakeNotes };

TEST_CASE("a note LISTENER is ordered against note writers only: two listening modulators route either way, and a writer after them stays after them") {
	// 2026-10-03: the MSEG and the LFO took a per-note table and so a note road, and consecutive note members are
	// ordered — which MEASURED refused an LFO's route into an LFO to its left (IZ's own case went red). A road that
	// changes nothing orders nothing. But a writer still hands what it wrote to whatever follows it.
	InstanceCounter ids;
	Graph g(kAxes, 1, 8, ids);
	const DeviceInstanceId l1 = g.add(kListener);
	const DeviceInstanceId l2 = g.add(kListener);
	const DeviceInstanceId w = g.add(kWriter);
	REQUIRE(g.insert(0, 0, l1));
	REQUIRE(g.insert(0, 1, l2));
	REQUIRE(g.insert(0, 2, w));

	// TWO LISTENERS: either way.
	CHECK_FALSE(g.wouldCycle(Source::output(l2, 0), controlOf(g, l1, 0)));
	CHECK(route(g, l2, 0, l1, 0, 0.5f));
	// THE WRITER FOLLOWS THEM: its output into a listener before it would need it ticked first and run last.
	CHECK(g.wouldCycle(Source::output(w, 0), controlOf(g, l1, 0)));
	CHECK(g.wouldCycle(Source::output(w, 0), controlOf(g, l2, 0)));
	CHECK_FALSE(route(g, w, 0, l1, 0, 0.5f));
	// …and forward into it is fine — the control a refusal of everything would fail.
	CHECK(route(g, l1, 0, w, 0, 0.5f));
}

//------------------------------------------------------------------------------------//
// THE CHUNKED POOL — storage that grows without moving (unbounded devices step 2)     //
//------------------------------------------------------------------------------------//

namespace {

struct PoolCell {
	std::atomic<int> value { -1 };
};

} // namespace

TEST_CASE("chunked pool: growth keeps every element where it was, with its value") {
	ChunkedPool<PoolCell, 4> pool(3);
	REQUIRE(pool.capacity() == 4); // whole chunks
	CHECK(pool.at(4) == nullptr);
	std::vector<PoolCell*> before;
	for (size_t i = 0; i < pool.capacity(); ++i) {
		CHECK(pool.at(i)->value.load() == -1); // the element's own initialiser, not zero
		pool.at(i)->value.store(static_cast<int>(i));
		before.push_back(pool.at(i));
	}

	// A RUN OF SMALL GROWTHS AT LEAST DOUBLES, so a hundred adds cost a handful of directories.
	for (size_t want = 5; want <= 100; ++want) {
		REQUIRE(pool.grow(want) >= want);
	}
	CHECK(pool.capacity() >= 100);
	CHECK(pool.directoryCountForTest() <= 7);

	// THE POINT OF THE TYPE: the addresses a reader held before every growth are the addresses now, and
	// what was written at them is still there. A `std::vector` would fail both — its growth moves.
	for (size_t i = 0; i < before.size(); ++i) {
		CAPTURE(i);
		CHECK(pool.at(i) == before[i]);
		CHECK(pool.at(i)->value.load() == static_cast<int>(i));
	}
	// AND A GROWN ELEMENT STARTS AT ITS INITIALISER, like the first ones did.
	CHECK(pool.at(99)->value.load() == -1);
	CHECK(pool.at(pool.capacity()) == nullptr);
}

TEST_CASE("chunked pool: a reader allocates nothing, and a grow is the only thing that does") {
	ChunkedPool<PoolCell> pool(16);

	// CONTROL: the probe can see an allocation.
	allocs.armed = true;
	allocs.allocations = 0;
	pool.grow(17);
	allocs.armed = false;
	REQUIRE(allocs.allocations > 0);

	allocs.armed = true;
	allocs.allocations = 0;
	int sum = 0;
	for (size_t i = 0; i < pool.capacity() + 8; ++i) {
		if (PoolCell* c = pool.at(i)) {
			c->value.store(1, std::memory_order_relaxed);
			sum += c->value.load(std::memory_order_relaxed);
		}
	}
	sum += static_cast<int>(pool.capacity());
	pool.grow(pool.capacity()); // already there: no new directory, no allocation
	allocs.armed = false;
	CHECK(allocs.allocations == 0);
	CHECK(sum == 64);
}

TEST_CASE("chunked pool: a writer on another thread keeps writing across growths") {
	// THE AUDIO THREAD'S SHAPE: one thread stores into every element it can see, continuously, while
	// this one grows the pool. Every store must land in an element that still exists (the sanitizer
	// gate is what turns a stray one into a report), and when the writer stops, each element it could
	// see at the end holds what it wrote.
	ChunkedPool<PoolCell, 8> pool(8);
	std::atomic<bool> stop { false };
	std::atomic<uint64_t> passes { 0 };
	std::thread writer([&] {
		while (!stop.load(std::memory_order_acquire)) {
			const size_t n = pool.capacity();
			for (size_t i = 0; i < n; ++i) {
				pool.at(i)->value.store(7, std::memory_order_relaxed);
			}
			passes.fetch_add(1, std::memory_order_relaxed);
		}
	});
	for (size_t want = 9; want <= 4096; want += 37) {
		pool.grow(want);
	}
	const uint64_t at = passes.load();
	while (passes.load() < at + 2) {
		std::this_thread::yield();
	}
	stop.store(true, std::memory_order_release);
	writer.join();
	for (size_t i = 0; i < pool.capacity(); ++i) {
		CAPTURE(i);
		REQUIRE(pool.at(i)->value.load() == 7);
	}
}

//------------------------------------------------------------------------------------//
// UNBOUNDED ROUTES — backlog OJ                                                          //
//------------------------------------------------------------------------------------//

TEST_CASE("backlog OJ: a directory view taken before a growth stays valid, reaches only what existed when it was taken, and every element it reaches was initialised") {
	// THE HOT RESOLVE'S CONTRACT (`RackEngine::inputs_`): the drain takes a view once and indexes it until the next
	// rebuild, while the message thread may grow the pool under it. Each growth here INITIALISES its new elements to
	// 42 (the `init` road), so an element seen at anything else was read before its initialisation was published.
	using Pool = ChunkedPool<PoolCell, 4, 3, 2>;
	Pool pool;
	pool.grow(4, [](PoolCell& c, size_t) { c.value.store(42, std::memory_order_relaxed); });
	const Pool::View stale = pool.view();
	REQUIRE(stale.capacity() == 4);
	std::vector<PoolCell*> addresses;
	for (size_t i = 0; i < stale.capacity(); ++i) {
		addresses.push_back(&stale[i]);
	}

	// A READER ON ANOTHER THREAD, TAKING A FRESH VIEW EACH PASS, while this thread grows to 4000 in single steps.
	std::atomic<bool> stop { false };
	std::atomic<uint64_t> passes { 0 };
	std::atomic<uint64_t> uninitialised { 0 };
	std::thread reader([&] {
		while (!stop.load(std::memory_order_acquire)) {
			const Pool::ConstView v = std::as_const(pool).view();
			for (size_t i = 0; i < v.capacity(); ++i) {
				if (v[i].value.load(std::memory_order_relaxed) != 42) {
					uninitialised.fetch_add(1, std::memory_order_relaxed);
				}
			}
			passes.fetch_add(1, std::memory_order_relaxed);
		}
	});
	size_t directories = pool.directoryCountForTest();
	for (size_t want = 5; want <= 4000; ++want) {
		pool.grow(want, [](PoolCell& c, size_t) { c.value.store(42, std::memory_order_relaxed); });
	}
	directories = pool.directoryCountForTest() - directories;
	const uint64_t at = passes.load();
	while (passes.load() < at + 2) {
		std::this_thread::yield();
	}
	stop.store(true, std::memory_order_release);
	reader.join();
	CHECK(uninitialised.load() == 0);

	// THE STALE VIEW: the same four elements at the same addresses, nothing past them, and still writable.
	CHECK(stale.capacity() == 4);
	CHECK(stale.at(4) == nullptr);
	for (size_t i = 0; i < 4; ++i) {
		CAPTURE(i);
		CHECK(&stale[i] == addresses[i]);
		CHECK(&stale[i] == pool.at(i));
		stale[i].value.store(static_cast<int>(i));
		CHECK(pool.at(i)->value.load() == static_cast<int>(i));
	}
	// AND x1.5: single steps to 4000 (1000 chunks of 4) cost ceil(log1.5(1000)) + 1 = 19 directories at most.
	CHECK(pool.capacity() >= 4000);
	CHECK(directories <= 19);
	CHECK(directories >= 10); // and it is x1.5, not x2 (which would be 9)
}

TEST_CASE("backlog OJ: a route list grows past any capacity under a concurrent snapshotting reader, and every snapshot it takes is one whole publish") {
	// THE SEQLOCK ACROSS A GROWTH. The writer publishes lists of 1, 2, ... 3000 routes, each route of the publish of
	// length n reading { source n, destination i, amount n }, so a snapshot is whole exactly when every route says
	// the same n and the count is n. The reader's stage is grown by the list's growth hook before each publish
	// that needs it, so the reader is never short of room: `NoRoom` would mean the hook ran after the count landed.
	RouteList list;
	RouteStage stage { 16 };
	list.setGrowthHook([](void* s, size_t capacity) { static_cast<RouteStage*>(s)->grow(capacity); }, &stage);
	std::atomic<bool> stop { false };
	std::atomic<uint64_t> whole { 0 };
	std::atomic<uint64_t> torn { 0 };
	std::atomic<uint64_t> noRoom { 0 };
	std::atomic<uint64_t> wrong { 0 };
	std::thread reader([&] {
		while (!stop.load(std::memory_order_acquire)) {
			size_t count = 0;
			uint32_t sequence = 0;
			RouteList::Refused why = RouteList::Refused::None;
			if (list.snapshot(stage, &count, &sequence, &why)) {
				const RouteRun run { std::as_const(stage).view(), count };
				for (size_t i = 0; i < run.size(); ++i) {
					if (run[i].source.index != count || run[i].destination.index != i
					    || run[i].amount != static_cast<float>(count)) {
						wrong.fetch_add(1, std::memory_order_relaxed);
					}
				}
				whole.fetch_add(1, std::memory_order_relaxed);
			}
			else {
				(why == RouteList::Refused::NoRoom ? noRoom : torn).fetch_add(1, std::memory_order_relaxed);
			}
		}
	});
	for (uint32_t n = 1; n <= 3000; ++n) {
		list.begin();
		for (uint32_t i = 0; i < n; ++i) {
			REQUIRE(list.add(Route { Source::core(n), Destination::core(i), static_cast<float>(n) }));
		}
		list.end();
	}
	const uint64_t at = whole.load();
	// BOUNDED: a reader that can never take one (the known-bad: no room) must fail the case below, not hang it.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (whole.load() < at + 2 && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::yield();
	}
	stop.store(true, std::memory_order_release);
	reader.join();
	CHECK(list.count() == 3000);
	CHECK(wrong.load() == 0);
	CHECK(noRoom.load() == 0);
	CHECK(whole.load() > 0);
	MESSAGE("whole snapshots " << whole.load() << ", torn " << torn.load());
	// x1.5 FROM 16: ceil(log1.5(3000 / 16)) + 1 = 14 growths at most.
	CHECK(list.growthsForTest() <= 14);
	CHECK(stage.capacity() >= 3000);
}
