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

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "fx/owltt/owltt_types.h"
#include "fx/featherverb/featherverb_types.h"
#include "fx/filter/filter_types.h"
#include "mod/dahdsr/dahdsr_types.h"
#include "mod/follower/follower_types.h"
#include "mod/modulator.h"
#include "plugin/device_registry.h"
#include "backplane/graph.h"
#include "rack/chain_state.h"
#include "rack/converger.h"
#include "rack/rack_engine.h"
#include "backplane/block_context.h"

#include <array>
#include <cmath>
#include <vector>

// THE DESIRED-STATE PROTOCOL, ASKED OF A BARE ENGINE — no Feathers, no bridge. Desired state, in
// both hosts: a host says what its engine should BE (`ChainState`) and `Converger` makes it
// so. Feathers' lane and strip are covered through `LaneRack` (test_lane_rack.cpp); these are the claims
// every host leans on, stated once where the rack's own policy (anything runs anywhere) applies.
//
// **BY ID SINCE KILL-LANES STEP 3a.** The engine keeps each device under the id its state names, in a
// slot the ENGINE chooses; a case asks the engine where device `id` is, never assumes the state's lane.

using namespace feathers;
using rack::ChainState;
using rack::Converger;
using rack::RackEngine;

namespace {

constexpr backplane::Axes kAxes {
	.coreSources = 8,
	.coreDestinations = 0,
	.deviceSlots = 4,
	.outputStride = 4,
};

/// PUT A DEVICE AT THE END OF A CHAIN — tag, identity and the first field; the rest at default.
void seat(ChainState& s, size_t chain, const char* tag, DeviceInstanceId id, float first) {
	rack::DeviceState row;
	row.tag = device::tagOf(tag);
	row.instanceId = id;
	row.fieldsPresent = 1;
	row.field[0] = first;
	REQUIRE(s.append(chain, row));
}

/// THE ROW A CHAIN HOLDS AT `position`, TO WRITE — a case editing what the state says next.
rack::DeviceState& rowAt(ChainState& s, size_t chain, size_t position) {
	rack::DeviceState* row = s.mutableAt(chain, position);
	REQUIRE(row != nullptr);
	return *row;
}

/// A VALUE THE CONTROL'S CONTRACT ALLOWS AND THAT IS NOT ITS DEFAULT — from the descriptor, never picked.
float legalNotDefault(const char* tag, size_t control) {
	const device::ParamDesc& p = deviceForTag(tag)->params[control];
	if (p.stepped) {
		return static_cast<float>(p.def + 1.0 <= p.max ? p.def + 1.0 : p.def - 1.0);
	}
	const double v = p.min + 0.75 * (p.max - p.min);
	return static_cast<float>(v != p.def ? v : p.min + 0.25 * (p.max - p.min));
}

/// THE ENGINE'S ORDER, AS THE IDS ITS SLOTS CARRY.
std::vector<DeviceInstanceId> orderOf(const RackEngine& e) {
	std::vector<DeviceInstanceId> out;
	for (size_t p = 0; p < e.chainLength(); ++p) {
		out.push_back(e.slotAtLane(e.deviceAtPosition(p)).id);
	}
	return out;
}

struct Rig {
	RackEngine engine { kAxes, hostCatalogue() };
	Converger converger { engine, &Converger::fitsAnywhere };
	Rig() { engine.prepare(48000.0, 4096); }
	/// ONE ROUND OF THE PROTOCOL: build on the "message thread", converge at the drain, reap.
	void apply(const ChainState& s, ContainerBodies* bodies = nullptr) {
		converger.provision(s, bodies, 0); // a bare engine: no publish sequence
		converger.converge(s, 0);
		converger.reap();
	}
	/// WHAT THE ENGINE RUNS FOR `id`, ASKED WHERE THE ENGINE KEEPS IT.
	const void* storageOf(DeviceInstanceId id) const {
		const size_t slot = engine.slotOf(id);
		return slot == RackEngine::kNoSlot ? nullptr : engine.slotAtLane(slot).storage.get();
	}
	float fieldOf(DeviceInstanceId id, size_t control) const {
		const size_t slot = engine.slotOf(id);
		REQUIRE(slot != RackEngine::kNoSlot);
		return engine.fieldAt(slot, control);
	}
	const device::Descriptor* running(DeviceInstanceId id) const { return converger.runningDescriptorOf(id); }
};

const char* const kFilter = fx::filter::kTraits.tag;
const char* const kOwltt = fx::owltt::kTraits.tag;
const char* const kEnv = mod::dahdsr::kTraits.tag;

} // namespace

TEST_CASE("a chain state holds each device once, in one chain, and compares only what it says") {
	ChainState s(4, 2);
	seat(s, 0, kFilter, 5, 0.25f);
	// ONE DEVICE IS ONE DEVICE, IN ONE PLACE — the same id twice is refused, in this chain or the other.
	rack::DeviceState again;
	again.instanceId = 5;
	CHECK_FALSE(s.append(1, again));
	CHECK_FALSE(s.append(0, again));
	CHECK_FALSE(s.append(2, again));               // no such chain
	CHECK_FALSE(s.append(0, rack::DeviceState {})); // under no id
	size_t chain = 99;
	size_t position = 99;
	CHECK(s.find(5, &chain, &position));
	CHECK(chain == 0);
	CHECK(position == 0);
	CHECK_FALSE(s.holds(6));

	ChainState t(4, 2);
	seat(t, 0, kFilter, 5, 0.25f);
	CHECK(s == t);
	// STORAGE PAST A CHAIN'S END SAYS NOTHING — a row erased is not a row compared.
	seat(t, 0, kOwltt, 7, 0.5f);
	REQUIRE(t.erase(0, 1));
	CHECK(s == t);
	rowAt(t, 0, 0).field[5] = 0.5f;
	CHECK_FALSE(s == t);

	ChainState wrong(3, 2);
	CHECK_FALSE(wrong.assign(s)); // a different shape is refused, never resized
	ChainState copy(4, 2);
	REQUIRE(copy.assign(t));
	CHECK(copy == t);
}

TEST_CASE("an engine becomes the state: the devices it names, in that order, with those rows") {
	Rig r;
	const float filterFirst = legalNotDefault(kFilter, 0);
	const float owlttFirst = legalNotDefault(kOwltt, 0);
	ChainState s(kAxes.deviceSlots, 1);
	seat(s, 0, kFilter, 11, filterFirst);
	seat(s, 0, kOwltt, 12, owlttFirst);
	r.apply(s);

	CHECK(orderOf(r.engine) == std::vector<DeviceInstanceId> { 11, 12 });
	CHECK(r.running(11) == deviceForTag(kFilter));
	CHECK(r.running(12) == deviceForTag(kOwltt));
	CHECK(r.running(13) == nullptr);
	// THE ROW IS THE STATE'S: the first field as written, the next at its DECLARED default.
	CHECK(r.fieldOf(11, 0) == filterFirst);
	const device::Descriptor* filter = deviceForTag(kFilter);
	REQUIRE(filter->params.size() > 1);
	CHECK(r.fieldOf(11, 1) == static_cast<float>(filter->params[1].def));
	CHECK(r.fieldOf(12, 0) == owlttFirst);
}

TEST_CASE("the engine keeps each device under the id its state names, in a slot of its own choosing") {
	// **ONE DEVICE, ONE ID, ON BOTH SIDES (kill-lanes step 3a).** The engine used to issue its OWN id at
	// every adoption, so its devices and its record's agreed only while two counters kept step — and cases
	// comparing them leaned on that without knowing. The state says WHICH devices in WHAT ORDER and nothing
	// about where they live; the engine picks its own slots, and the ids are what join the two.
	Rig r;
	ChainState s(kAxes.deviceSlots, 1);
	seat(s, 0, kFilter, 901, 0.25f);
	seat(s, 0, kOwltt, 907, 0.5f);
	r.apply(s);
	REQUIRE(r.engine.chainLength() == 2);
	CHECK(r.engine.slotOf(901) == 0); // the engine's own lowest free slot
	CHECK(r.engine.slotOf(907) == 1);
	CHECK(r.engine.slotAtLane(r.engine.slotOf(901)).id == 901);
	CHECK(r.engine.slotAtLane(r.engine.slotOf(907)).id == 907);
	CHECK(orderOf(r.engine) == std::vector<DeviceInstanceId> { 901, 907 });
}

TEST_CASE("a reorder MOVES devices and a knob keeps them; only a new identity rebuilds one") {
	Rig r;
	const float def = static_cast<float>(deviceForTag(kFilter)->params[0].def);
	ChainState s(kAxes.deviceSlots, 1);
	seat(s, 0, kFilter, 11, def);
	seat(s, 0, kFilter, 12, def); // TWO OF A KIND — identity, not kind, is what is compared
	r.apply(s);
	const void* first = r.storageOf(11);
	const void* second = r.storageOf(12);
	REQUIRE(first != nullptr);
	REQUIRE(second != nullptr);

	SUBCASE("a reorder") {
		ChainState t(kAxes.deviceSlots, 1);
		seat(t, 0, kFilter, 12, def);
		seat(t, 0, kFilter, 11, def);
		CHECK(r.converger.provision(t, nullptr, 0) == 0); // nothing to build
		r.converger.converge(t, 0);
		CHECK(orderOf(r.engine) == std::vector<DeviceInstanceId> { 12, 11 });
		CHECK(r.storageOf(11) == first);
		CHECK(r.storageOf(12) == second);
	}

	SUBCASE("a knob") {
		const float moved = legalNotDefault(kFilter, 0);
		REQUIRE(moved != rowAt(s, 0, 0).field[0]);
		rowAt(s, 0, 0).field[0] = moved;
		CHECK(r.converger.provision(s, nullptr, 0) == 0);
		r.converger.converge(s, 0);
		CHECK(r.storageOf(11) == first);
		CHECK(r.fieldOf(11, 0) == moved);
	}

	SUBCASE("a row that stops carrying a control puts it back at its default") {
		// THE CASE A FRESH DEVICE CANNOT TELL: it starts at its defaults, so "an absent control is at its
		// default" only shows when a control that WAS carried stops being carried — a load of an older
		// row, an undo past the edit that widened it.
		const float moved = legalNotDefault(kFilter, 1);
		rowAt(s, 0, 0).fieldsPresent = 2;
		rowAt(s, 0, 0).field[1] = moved;
		r.apply(s);
		REQUIRE(r.fieldOf(11, 1) == moved);
		rowAt(s, 0, 0).fieldsPresent = 1;
		r.apply(s);
		CHECK(r.fieldOf(11, 1) == static_cast<float>(deviceForTag(kFilter)->params[1].def));
		CHECK(r.storageOf(11) == first);
	}

	SUBCASE("a replacement under the same tag") {
		rowAt(s, 0, 0).instanceId = 13;
		CHECK(r.converger.provision(s, nullptr, 0) == 1);
		r.converger.converge(s, 0);
		r.converger.reap();
		CHECK(r.running(13) == deviceForTag(kFilter));
		CHECK(r.running(11) == nullptr);
		CHECK(r.storageOf(13) != nullptr);
		CHECK(r.storageOf(13) != first);
		CHECK(r.storageOf(12) == second); // its neighbour untouched
		CHECK(orderOf(r.engine) == std::vector<DeviceInstanceId> { 13, 12 }); // and in the replaced one's place
	}
}

TEST_CASE("the engine runs the order the state publishes — the graph's — and the chains in index order only when none is published") {
	// KILL-LANES 4c. The converger ran THE CHAINS IN INDEX ORDER, which agreed with the graph for every
	// shape a hand could build only because nothing across two chains had been routed backwards — and
	// nothing orders two chains against each other, so such a route is legal, and its reader would have
	// run before its source and read the tick before. The record now writes `Graph::order()` into the
	// state (`ChainState::order()`), and this is the converger's half: it runs what is published, moves
	// rather than rebuilds to get there, skips what it does not hold, and falls back to the chain walk only
	// for a state no graph spoke for. By design, ordering comes only from the graph.
	Rig r;
	ChainState s(4, 2);
	seat(s, 0, kEnv, 1, 0.1f);    // the strip
	seat(s, 1, kFilter, 2, 0.2f); // the lane
	seat(s, 1, kOwltt, 3, 0.3f);

	SUBCASE("no order published: the chains in index order, each by its path — the fallback") {
		REQUIRE(s.order().empty());
		r.apply(s);
		CHECK(orderOf(r.engine) == std::vector<DeviceInstanceId> { 1, 2, 3 });
	}
	SUBCASE("an order that runs a lane device before the strip's modulator is run as published") {
		const DeviceInstanceId want[] = { 2, 1, 3 };
		REQUIRE(s.setOrder(want));
		r.apply(s);
		CHECK(orderOf(r.engine) == std::vector<DeviceInstanceId> { 2, 1, 3 });

		// AND THE ORDER MOVING ALONE — no device added, none removed, no row changed — moves the engine,
		// and moves it: the storage stays, as a reorder's does.
		const DeviceInstanceId again[] = { 3, 2, 1 };
		REQUIRE(s.setOrder(again));
		const void* filterBefore = r.storageOf(2);
		REQUIRE(filterBefore != nullptr);
		r.apply(s);
		CHECK(orderOf(r.engine) == std::vector<DeviceInstanceId> { 3, 2, 1 });
		CHECK(r.storageOf(2) == filterBefore);
	}
	SUBCASE("an id the order names and the engine does not hold — the host's own node, say — is skipped") {
		const DeviceInstanceId want[] = { backplane::Graph::kCoreInstance, 3, 1, 2 };
		REQUIRE(s.setOrder(want));
		r.apply(s);
		CHECK(orderOf(r.engine) == std::vector<DeviceInstanceId> { 3, 1, 2 });
	}
	SUBCASE("the order travels through assign and swap, and two states that differ only in it differ") {
		ChainState t(4, 2);
		REQUIRE(t.assign(s));
		CHECK(t == s);
		const DeviceInstanceId want[] = { 3, 1, 2 };
		REQUIRE(t.setOrder(want));
		CHECK_FALSE(t == s);
		REQUIRE(s.assign(t));
		CHECK(s == t);
		CHECK(std::vector<DeviceInstanceId>(s.order().begin(), s.order().end()) == std::vector<DeviceInstanceId> { 3, 1, 2 });
		s.clear();
		CHECK(s.order().empty());
	}
}

TEST_CASE("a device the state stops naming is removed, and one the build or the policy cannot run is not run") {
	Rig r;
	ChainState s(kAxes.deviceSlots, 1);
	seat(s, 0, kFilter, 11, 0.25f);
	seat(s, 0, kOwltt, 12, 0.5f);
	r.apply(s);
	REQUIRE(r.engine.chainLength() == 2);

	ChainState t(kAxes.deviceSlots, 1);
	seat(t, 0, kOwltt, 12, 0.5f);
	seat(t, 0, "zzzz", 14, 0.0f); // a tag this build does not know
	r.apply(t);
	CHECK(orderOf(r.engine) == std::vector<DeviceInstanceId> { 12 });
	CHECK(r.running(11) == nullptr);
	CHECK(r.running(14) == nullptr);

	SUBCASE("and the host's policy is asked per chain") {
		RackEngine engine { kAxes, hostCatalogue() };
		engine.prepare(48000.0, 4096);
		// A POLICY THAT RUNS ONLY MODULATORS IN CHAIN 0 — Feathers' strip, stated as a lambda.
		Converger strict { engine, [](const device::Descriptor& d, size_t chain) {
			                  return chain != 0 || d.mod != nullptr;
		                  } };
		ChainState u(kAxes.deviceSlots, 2);
		seat(u, 0, kFilter, 21, 0.25f); // an audio device where only modulators may run
		seat(u, 0, kEnv, 22, 0.0f);
		seat(u, 1, kFilter, 23, 0.25f); // the same device where it may
		strict.provision(u, nullptr, 0);
		strict.converge(u, 0);
		strict.reap();
		CHECK(strict.runningDescriptorOf(21) == nullptr);
		CHECK(strict.runningDescriptorOf(22) == deviceForTag(kEnv));
		CHECK(strict.runningDescriptorOf(23) == deviceForTag(kFilter));
		// CHAINS IN INDEX ORDER: chain 0's devices, then chain 1's.
		CHECK(orderOf(engine) == std::vector<DeviceInstanceId> { 22, 23 });
	}
}

TEST_CASE("an arrival built for one device is never installed under another's name") {
	// THE STATE MOVED ON BETWEEN THE BUILD AND THE DRAIN: the message thread built id 11, and by the time
	// the drain looked the record said id 12 in its place. The arrival must be handed back, not run as 12.
	Rig r;
	ChainState built(kAxes.deviceSlots, 1);
	seat(built, 0, kFilter, 11, 0.25f);
	REQUIRE(r.converger.provision(built, nullptr, 0) == 1);

	ChainState now(kAxes.deviceSlots, 1);
	seat(now, 0, kFilter, 12, 0.25f);
	r.converger.converge(now, 0);
	CHECK(r.running(12) == nullptr);
	CHECK(r.running(11) == nullptr);
	CHECK(r.engine.chainLength() == 0);
	CHECK(r.converger.pendingCountForTest() == 0); // taken out of the mailbox and handed back

	// AND THE NEXT ROUND BUILDS WHAT THE STATE NOW SAYS.
	r.apply(now);
	CHECK(r.running(12) == deviceForTag(kFilter));
}

TEST_CASE("a device handed back unadopted, and still wanted, is built again — never believed built") {
	// **THE LOST ADD, FOR A HOST WITH NO PUBLISH SEQUENCE** (Feathers' lane today): the message thread
	// builds from the newer chain, the drain converges to the older one it copied, and hands the arrival
	// back as unwanted. The message thread's mirror still says "built", so without the reap forgetting it
	// the device is never built again and never runs — a card with no sound behind it.
	Rig r;
	ChainState older(kAxes.deviceSlots, 1); // what the drain still holds: nothing
	ChainState newer(kAxes.deviceSlots, 1); // what the hand made: one filter
	seat(newer, 0, kFilter, 11, 0.25f);
	REQUIRE(r.converger.provision(newer, nullptr, 0) == 1);
	r.converger.converge(older, 0); // handed back — nothing in the older state wants it
	CHECK(r.converger.pendingCountForTest() == 0);
	r.converger.converge(newer, 0); // the drain catches up, and there is nothing to adopt
	CHECK(r.running(11) == nullptr);

	// THE CLAIM: the next service builds it again, and the drain after that runs it.
	CHECK(r.converger.provision(newer, nullptr, 0) == 1);
	r.converger.converge(newer, 0);
	CHECK(r.running(11) == deviceForTag(kFilter));
}

TEST_CASE("a device the drain removed, wanted back by a later state, is built again with no provision between") {
	// **A HOLE THE LANE-KEYED CONVERGER HAD, CLOSED BY SAYING WHAT LEFT** (`Converger::departed_`). The hand
	// removes a device and puts it back (an undo, under the same id) and the message thread provisions only
	// once, after both: its mirror still says "built". Meanwhile a drain took the removal and parked the
	// device. The later state wants it, the engine does not run it, and nothing would ever build it again —
	// a card with no sound. The drain now reports the departure, and the next service forgets and rebuilds.
	Rig r;
	ChainState with(kAxes.deviceSlots, 1);
	seat(with, 0, kFilter, 11, 0.25f);
	r.apply(with);
	REQUIRE(r.running(11) == deviceForTag(kFilter));

	ChainState without(kAxes.deviceSlots, 1);
	r.converger.converge(without, 0); // the drain takes the removal; no provision ran for it
	REQUIRE(r.running(11) == nullptr);
	REQUIRE(r.engine.chainLength() == 0);

	// THE STATE WANTS IT BACK. The service that follows must build it — it cannot believe it built.
	CHECK(r.converger.provision(with, nullptr, 0) == 1);
	r.converger.converge(with, 0);
	r.converger.reap();
	CHECK(r.running(11) == deviceForTag(kFilter));
	CHECK(r.engine.chainLength() == 1);
}

TEST_CASE("an arrival from the drain's future is kept for the drain that has its state, not handed back") {
	// **THE ADD THE DESIRED-STATE PROTOCOL LOST, PINNED WHERE IT HAPPENED.** Found by the rack's
	// hand-and-blocks case on the protocol's first run: the message thread published state 4 and built
	// its device; a drain still holding state 2 emptied the mailbox, found nothing in state 2 wanting it,
	// and handed it back — and the message thread, having built it, never built it again. So an arrival
	// names the publish it was built for, and a drain behind that publish puts it back.
	Rig r;
	ChainState older(kAxes.deviceSlots, 1); // publish 2: an empty rack
	ChainState newer(kAxes.deviceSlots, 1); // publish 4: one filter
	seat(newer, 0, kFilter, 11, 0.25f);
	REQUIRE(r.converger.provision(newer, nullptr, 4) == 1);

	r.converger.converge(older, 2); // the drain has not taken publish 4 yet
	CHECK(r.engine.chainLength() == 0);
	CHECK(r.converger.pendingCountForTest() == 1); // KEPT, not handed back

	r.converger.converge(newer, 4); // the drain that has it
	CHECK(r.running(11) == deviceForTag(kFilter));
	CHECK(r.engine.chainLength() == 1);
	CHECK(r.converger.pendingCountForTest() == 0);

	SUBCASE("and one from a publish the drain has seen past is stale, and goes back") {
		ChainState later(kAxes.deviceSlots, 1); // publish 6: the filter replaced by another
		seat(later, 0, kFilter, 12, 0.25f);
		REQUIRE(r.converger.provision(later, nullptr, 6) == 1);
		ChainState latest(kAxes.deviceSlots, 1); // publish 8: the rack emptied before any drain saw 6
		r.converger.converge(latest, 8);
		CHECK(r.converger.pendingCountForTest() == 0); // handed back: nothing will ever want it
		CHECK(r.engine.chainLength() == 0);
	}
}

TEST_CASE("a device REPLACED UNDER ITS OWN ID is built once, and the round after it builds nothing") {
	// **THE DEFECT THIS CASE EXISTS FOR, found by review 2026-09-11.** The record replaces a device in
	// place under the SAME id — `gui_bridge.cpp`'s remove-then-`insertRow` keeps it, which is what makes a
	// replacement "the same card, another device" rather than a new slot. `converge` then reported a
	// DEPARTURE for that id and adopted it again in the same call; `reap` read the departure and forgot a
	// device that IS running, so the next service built it again, the next drain threw the rebuild away,
	// and the two of them did that forever. For a container it was worse than waste: the live nested rack
	// was removed and re-adopted on every drain.
	//
	// **THE ASSERTION THE OLD CASES WERE MISSING IS THE SECOND `provision`.** Every existing case
	// provisions once and asks what runs; a rebuild loop is invisible to that. This asks the round AFTER.
	Rig r;
	ChainState before(kAxes.deviceSlots, 1);
	seat(before, 0, kFilter, 11, 0.25f);
	r.apply(before);
	REQUIRE(r.running(11) == deviceForTag(kFilter));
	const void* firstStorage = r.storageOf(11);
	REQUIRE(firstStorage != nullptr);

	// THE REPLACEMENT: one id, another device. It IS a rebuild — the kind changed — so exactly one build.
	ChainState after(kAxes.deviceSlots, 1);
	seat(after, 0, kOwltt, 11, 0.5f);
	CHECK(r.converger.provision(after, nullptr, 0) == 1);
	r.converger.converge(after, 0);
	r.converger.reap();
	CHECK(r.running(11) == deviceForTag(kOwltt));
	CHECK(r.storageOf(11) != firstStorage); // really rebuilt, not the old instance renamed

	// **THE CLAIM: THE ROUND AFTER BUILDS NOTHING.** The state has not changed and the device is running,
	// so there is nothing to build — and nothing for the next drain to throw away.
	CHECK(r.converger.provision(after, nullptr, 0) == 0);
	CHECK(r.converger.pendingCountForTest() == 0);

	// AND THE DEVICE IS UNDISTURBED BY THAT ROUND — the instance the replacement built is still the one
	// running, which is what a rebuild loop would break (it swaps the instance on every drain).
	const void* settled = r.storageOf(11);
	r.converger.converge(after, 0);
	r.converger.reap();
	CHECK(r.running(11) == deviceForTag(kOwltt));
	CHECK(r.storageOf(11) == settled);
	// NOT VACUOUS: a third round is just as quiet, so this is a settled state and not a phase of a cycle.
	CHECK(r.converger.provision(after, nullptr, 0) == 0);
	CHECK(r.storageOf(11) == settled);
}

TEST_CASE("a re-prepare drops what was built at the old rate, and the next round builds it again") {
	// `Converger::dropPending` had no case at all. A device provisioned at the old sample rate must never
	// be adopted at the new one — `LaneRack::prepare` and `RackBridge::prepare` call this between the two
	// rates — and having dropped it, the message thread must not still believe it built it, or the device
	// is never built again and never runs.
	Rig r;
	ChainState s(kAxes.deviceSlots, 1);
	seat(s, 0, kFilter, 11, 0.25f);
	REQUIRE(r.converger.provision(s, nullptr, 0) == 1);
	REQUIRE(r.converger.pendingCountForTest() == 1); // built at 48k, waiting for a drain

	r.converger.dropPending();
	r.engine.prepare(96000.0, 4096);
	CHECK(r.converger.pendingCountForTest() == 0); // the old-rate device went

	// AND A DRAIN NOW ADOPTS NOTHING — there is nothing to adopt, and nothing built at 48k reaches 96k.
	r.converger.converge(s, 0);
	CHECK(r.running(11) == nullptr);

	// **THE OTHER HALF: THE MIRROR FORGOT IT**, so the next service builds it again at the new rate.
	CHECK(r.converger.provision(s, nullptr, 0) == 1);
	r.converger.converge(s, 0);
	r.converger.reap();
	CHECK(r.running(11) == deviceForTag(kFilter));
	CHECK(r.engine.sampleRate() == doctest::Approx(96000.0));
}

TEST_CASE("backlog GP: a re-prepare that drops a waiting instance takes its handle out of the store before it frees it") {
	// `provision` files a device-with-a-body's handle at BUILD (`ContainerBodies::attach`), so the host's editor can
	// reach it before any drain adopts it. `dropPending` frees that instance; a handle left filed is an address of
	// freed memory, and the next `follow` from an open editor — or a host save asking it for its contents — reads it.
	// `reap` has always detached; this road once did not (filed 2026-09-20, repaired only incidentally by the
	// `provision` on the next line of `RackBridge::prepare`). So the claim is asked BETWEEN the two, where nothing
	// has repaired it yet.
	const device::Descriptor* verb = deviceForTag(fx::featherverb::kTraits.tag);
	REQUIRE(verb != nullptr);
	REQUIRE(verb->hasStoredState()); // else nothing is filed and the case is vacuous
	Rig r;
	ContainerBodies bodies;
	ChainState s(kAxes.deviceSlots, 1);
	seat(s, 0, fx::featherverb::kTraits.tag, 21, 0.25f);
	REQUIRE(r.converger.provision(s, &bodies, 0) == 1);
	REQUIRE(r.converger.pendingCountForTest() == 1);
	// NOT VACUOUS: the waiting instance's handle is filed, and it is the instance the drop will free.
	REQUIRE(bodies.surfaceOf(21) != nullptr);

	r.converger.dropPending();
	CHECK(r.converger.pendingCountForTest() == 0);
	CHECK(bodies.surfaceOf(21) == nullptr);
	// AND WHAT A HOST'S SAVE DOES WITH A FILED HANDLE — asks it for its body. Through a handle left behind that is a
	// read of freed storage: invisible to the CHECK's neighbours, a report under ASan (`sanitize-converger`).
	if (const void* filed = bodies.surfaceOf(21); filed != nullptr && verb->bodySaver() != nullptr) {
		std::vector<uint8_t> body;
		const device::ByteSink sink { &body, [](void* ctx, const uint8_t* data, size_t bytes) {
			                             auto* out = static_cast<std::vector<uint8_t>*>(ctx);
			                             out->insert(out->end(), data, data + bytes);
		                             } };
		(void)verb->bodySaver()(filed, sink);
	}

	// AND THE NEXT ROUND FILES THE INSTANCE IT BUILDS — the handle comes back, naming live storage.
	r.engine.prepare(96000.0, 4096);
	REQUIRE(r.converger.provision(s, &bodies, 0) == 1);
	r.converger.converge(s, 0);
	r.converger.reap();
	CHECK(bodies.surfaceOf(21) != nullptr);
	CHECK(bodies.surfaceOf(21) == r.storageOf(21));
}

//==================================================================================================
// BACKLOG NS — A POLY DEVICE'S VOICE INSTANCES EXIST ONLY WHILE ITS SWITCH IS ON
//==================================================================================================

namespace {

constexpr size_t kNsVoices = 4;
constexpr auto kFilterPoly = static_cast<size_t>(fx::filter::Param::Poly);

/// AN ENGINE WITH VOICES — Feathers' lane, shrunk: per-voice devices have a road here.
struct VoicedRig : Rig {
	VoicedRig() {
		engine.setVoices(kNsVoices);
		engine.prepare(48000.0, 4096);
	}
	size_t voicesOf(DeviceInstanceId id) const {
		const size_t slot = engine.slotOf(id);
		REQUIRE(slot != RackEngine::kNoSlot);
		return engine.slotAtLane(slot).voices.size();
	}
};

/// A ROW OF `tag` WITH EVERY CONTROL AT ITS DECLARED DEFAULT AND CONTROL `poly` AT `value`.
rack::DeviceState polyRow(const char* tag, DeviceInstanceId id, size_t poly, float value) {
	const device::Descriptor* d = deviceForTag(tag);
	REQUIRE(d != nullptr);
	rack::DeviceState row;
	row.tag = device::tagOf(tag);
	row.instanceId = id;
	row.fieldsPresent = d->params.size();
	for (size_t c = 0; c < d->params.size(); ++c) {
		row.field[c] = static_cast<float>(d->params[c].def);
	}
	row.field[poly] = value;
	return row;
}

ChainState stateWith(const rack::DeviceState& row) {
	ChainState s(kAxes.deviceSlots, 1);
	REQUIRE(s.append(0, row));
	return s;
}

} // namespace

TEST_CASE("backlog NS: a Poly device's voice instances exist only while its switch is on — built on the message "
          "thread, swapped in at the drain, given back when it goes off, and nothing churns in between") {
	VoicedRig r;
	const ChainState off = stateWith(polyRow(kFilter, 21, kFilterPoly, 0.0f));
	const ChainState on = stateWith(polyRow(kFilter, 21, kFilterPoly, 1.0f));

	// OFF: THE BUS INSTANCE ALONE.
	r.apply(off);
	REQUIRE(r.running(21) == deviceForTag(kFilter));
	CHECK(r.voicesOf(21) == 0);
	// AND A ROUND THAT CHANGES NOTHING POSTS NOTHING — the mirror believes what the engine holds.
	CHECK(r.converger.provision(off, nullptr, 0) == 0);
	CHECK(r.converger.pendingCountForTest() == 0);

	// ON: ONE POST, OF THE VOICES ALONE (the device is not rebuilt), seated by the drain.
	const void* bus = r.storageOf(21);
	CHECK(r.converger.provision(on, nullptr, 0) == 0); // no DEVICE built
	CHECK(r.converger.pendingCountForTest() == 1);     // its voices, waiting
	r.converger.converge(on, 0);
	r.converger.reap();
	CHECK(r.voicesOf(21) == kNsVoices);
	CHECK(r.storageOf(21) == bus); // the same device: a switch is a setting
	CHECK(r.fieldOf(21, kFilterPoly) == 1.0f);
	CHECK(r.converger.provision(on, nullptr, 0) == 0);
	CHECK(r.converger.pendingCountForTest() == 0);

	// OFF AGAIN: GIVEN BACK BY THE DRAIN, and the round after posts nothing.
	r.apply(off);
	CHECK(r.voicesOf(21) == 0);
	CHECK(r.storageOf(21) == bus);
	CHECK(r.fieldOf(21, kFilterPoly) == 0.0f);
	CHECK(r.converger.provision(off, nullptr, 0) == 0);
	CHECK(r.converger.pendingCountForTest() == 0);

	// AND ON AGAIN — the belief round-trips, so the second flip builds as the first did.
	r.apply(on);
	CHECK(r.voicesOf(21) == kNsVoices);

	// A DEVICE ADDED WITH ITS SWITCH ON ARRIVES WITH ITS VOICES, in the one post.
	VoicedRig fresh;
	REQUIRE(fresh.converger.provision(on, nullptr, 0) == 1);
	CHECK(fresh.converger.pendingCountForTest() == 1);
	fresh.converger.converge(on, 0);
	fresh.converger.reap();
	CHECK(fresh.voicesOf(21) == kNsVoices);
}

TEST_CASE("backlog NS: a drain holding a Poly-on row before its voices have come runs the device as it was — the "
          "switch reads off — and flips it on the drain that seats them") {
	// FEATHERS PROVISIONS ON ITS EDITOR'S TICK, AFTER THE PUBLISH, so a drain can take the state that turned the
	// switch on before the instances it needs exist. The engine must not run that block with Poly reading on.
	VoicedRig r;
	const ChainState off = stateWith(polyRow(kFilter, 22, kFilterPoly, 0.0f));
	const ChainState on = stateWith(polyRow(kFilter, 22, kFilterPoly, 1.0f));
	r.apply(off);
	r.converger.converge(on, 0); // the new state, and nothing provisioned for it yet
	CHECK(r.voicesOf(22) == 0);
	CHECK(r.fieldOf(22, kFilterPoly) == 0.0f); // held off, by value
	std::array<float, 64> l {};
	std::array<float, 64> rr {};
	r.engine.processStereo(l.data(), rr.data(), 64, backplane::unknownBlock());
	CHECK_FALSE(r.engine.polyForTest(r.engine.slotOf(22)));
	// THE VOICES COME, AND THE SAME STATE NOW READS AS IT SAYS.
	CHECK(r.converger.provision(on, nullptr, 0) == 0);
	r.converger.converge(on, 0);
	r.converger.reap();
	CHECK(r.voicesOf(22) == kNsVoices);
	CHECK(r.fieldOf(22, kFilterPoly) == 1.0f);
	r.engine.beginBlock(64, backplane::unknownBlock());
	r.engine.endBlock();
	CHECK(r.engine.polyForTest(r.engine.slotOf(22)));
}

TEST_CASE("backlog NS: a Poly switch the audio thread can flip keeps its voices built while it is off — a host slot "
          "bound to it, or a switch a route can reach") {
	VoicedRig r;
	ChainState off = stateWith(polyRow(kFilter, 23, kFilterPoly, 0.0f));
	REQUIRE(off.setSlotBinding(3, rack::SlotBinding { .device = 23, .control = static_cast<uint16_t>(kFilterPoly),
	                                                 .slot = 3, .live = true }));
	r.apply(off);
	CHECK(r.voicesOf(23) == kNsVoices); // the host can turn it on at the top of any block
	// UNBOUND, THE SWITCH IS THE RECORD'S AGAIN, and off: the voices go back.
	off.clearSlotBindings();
	r.apply(off);
	CHECK(r.voicesOf(23) == 0);

	// A MODULATABLE SWITCH IS ONE A ROUTE MOVES IN THE RESOLVE — the same answer, asked of the rule itself.
	const device::Descriptor* filter = deviceForTag(kFilter);
	std::vector<device::ParamDesc> params(filter->params.begin(), filter->params.end());
	params[kFilterPoly].modulatable = true;
	device::Descriptor routable = *filter;
	routable.params = params;
	const rack::DeviceState row = polyRow(kFilter, 24, kFilterPoly, 0.0f);
	const ChainState plain = stateWith(row);
	CHECK(Converger::wantsVoices(routable, row, plain, kNsVoices));
	CHECK_FALSE(Converger::wantsVoices(*filter, row, plain, kNsVoices));
	CHECK_FALSE(Converger::wantsVoices(routable, row, plain, 0)); // a rack with no voices has no road
}

TEST_CASE("backlog NS: a voice post dropped by a re-prepare leaves the device as it runs, and the next round asks "
          "again at the new rate") {
	VoicedRig r;
	const ChainState off = stateWith(polyRow(kFilter, 25, kFilterPoly, 0.0f));
	const ChainState on = stateWith(polyRow(kFilter, 25, kFilterPoly, 1.0f));
	r.apply(off);
	REQUIRE(r.converger.provision(on, nullptr, 0) == 0);
	REQUIRE(r.converger.pendingCountForTest() == 1);
	r.converger.dropPending();
	r.engine.prepare(96000.0, 4096);
	CHECK(r.converger.pendingCountForTest() == 0);
	CHECK(r.running(25) == deviceForTag(kFilter)); // the device was not forgotten
	r.apply(on);
	CHECK(r.voicesOf(25) == kNsVoices);
}

TEST_CASE("backlog NS: an Audio-mode Follower whose voices arrive with its switch publishes from them — each note's own "
          "voice, and the newest note's for the card") {
	namespace ff = mod::follower;
	constexpr size_t kTick = static_cast<size_t>(mod::kBlockSamples);
	constexpr auto kTapPoly = static_cast<size_t>(ff::Param::Poly);
	VoicedRig r;
	rack::DeviceState row = polyRow(ff::kTraits.tag, 26, kTapPoly, 0.0f);
	row.field[static_cast<size_t>(ff::Param::Mode)] = static_cast<float>(ff::Mode::Audio);
	row.field[static_cast<size_t>(ff::Param::Detector)] = static_cast<float>(ff::Detector::Peak);
	row.field[static_cast<size_t>(ff::Param::Attack)] = static_cast<float>(ff::kAttackLog2Min);
	row.field[static_cast<size_t>(ff::Param::Release)] = static_cast<float>(ff::kReleaseLog2Min);
	const ChainState off = stateWith(row);
	row.field[kTapPoly] = 1.0f;
	const ChainState on = stateWith(row);
	r.apply(off);
	REQUIRE(r.voicesOf(26) == 0);
	r.apply(on);
	REQUIRE(r.voicesOf(26) == kNsVoices);
	const size_t tap = r.engine.slotOf(26);
	std::array<float, kTick> l {};
	std::array<float, kTick> rr {};
	device::NoteContext note;
	const auto block = [&] {
		r.engine.beginBlock(static_cast<int>(kTick), backplane::unknownBlock());
		for (size_t v = 0; v < 2; ++v) {
			l.fill(v == 0 ? 0.8f : 0.3f);
			rr.fill(v == 0 ? 0.8f : 0.3f);
			r.engine.processVoice(v, l.data(), rr.data(), static_cast<int>(kTick), note);
		}
		l.fill(0.0f);
		rr.fill(0.0f);
		r.engine.processBus(l.data(), rr.data(), static_cast<int>(kTick));
		r.engine.endBlock();
	};
	r.engine.setVoiceNote(0, 11, 0, 60);
	r.engine.setVoiceNote(1, 12, 0, 67);
	block();
	block();
	REQUIRE(r.engine.polyForTest(tap));
	const size_t env = static_cast<size_t>(ff::Output::Env);
	CHECK(std::abs(r.engine.outputForNoteAt(tap, env, 11, 0, 60) - 0.8f) <= 1e-5f);
	CHECK(std::abs(r.engine.outputForNoteAt(tap, env, 12, 0, 67) - 0.3f) <= 1e-5f);
	// THE NEWEST NOTE'S VOICE (c7b9a4b9's rule) is what the card and a destination with no voice read.
	float level = -1.0f;
	REQUIRE(r.engine.playheadLevelAt(tap, level));
	CHECK(std::abs(level - 0.3f) <= 1e-5f);
}
