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
#include "fx/filter/filter_types.h"
#include "mod/dahdsr/dahdsr_types.h"
#include "mod/mseg/mseg_types.h"
#include "plugin/device_registry.h"
#include "rack/chain_record.h"
#include "rack/converger.h"
#include "rack/published_chain.h"
#include "rack/rack_engine.h"
#include "rack/rack_state.h"
#include "patch_values.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>
#include <tuple>
#include <vector>

// THE RECORD — what a host's chains ARE, on the message thread (desired state, for both hosts).
// Its claims: devices land in their chain, in order, with their default rows; every refused verb
// changes nothing; a load puts a device back exactly, a tag this build does not know included; a row
// travels with its device; the graph and the state never disagree; and what it publishes arrives whole.
//
// **AND EVERY CLAIM IS NOW MADE BY IDENTITY AND ORDER (kill-lanes step 3b).** A chain is its order, so
// `orderOf` — the ids in the order the block will walk them — is what a structural case asserts. Where a
// case used to name the pool slot a device was parked in, that is the record's private business now and
// no case may see it; what a reader is owed is WHICH device and WHERE IN THE CHAIN, and those are still
// here. The two places this cost a claim outright are marked where they stand.

using namespace feathers;
using rack::ChainRecord;
using rack::ChainState;
using rack::DeviceState;
using rack::ChainCapacity;

namespace {

/// FEATHERS' SHAPE, stated by the platform: the strip holds four modulators, the fx lane eight effects,
/// and the strip is chain 0 because it ranks first. A CAPACITY each — since kill-lanes step 4d a chain
/// owns no region of the pool, only a count.
constexpr ChainCapacity kFeathersCapacities[] = { kMaxRackSlots, kMaxFxSlots };
constexpr size_t kStrip = 0;
constexpr size_t kLane = 1;

const char* const kFilter = fx::filter::kTraits.tag;
const char* const kOwltt = fx::owltt::kTraits.tag;
const char* const kEnv = mod::dahdsr::kTraits.tag;

struct Rig {
	backplane::InstanceCounter ids;
	ChainRecord record { feathersAxes(), kFeathersCapacities, hostCatalogue(), ids, &backplane::permitAll };
};

/// **ONE CHAIN, AS THE BLOCK WILL WALK IT** — the devices by id, in order. What a chain IS.
std::vector<DeviceInstanceId> orderOf(const ChainRecord& r, size_t chain) {
	std::vector<DeviceInstanceId> out;
	for (size_t p = 0; p < r.length(chain); ++p) {
		out.push_back(r.deviceAt(chain, p));
	}
	return out;
}

DeviceState rowFor(const char* tag, DeviceInstanceId id, std::initializer_list<float> fields) {
	DeviceState row;
	row.tag = device::tagOf(tag);
	row.instanceId = id;
	size_t i = 0;
	for (const float f : fields) {
		row.field[i++] = f;
	}
	row.fieldsPresent = i;
	return row;
}

} // namespace

TEST_CASE("a device lands in its chain, in order, under a fresh id, with its default row") {
	Rig r;
	const DeviceInstanceId env = r.record.insert(kStrip, 0, kEnv);
	const DeviceInstanceId filter = r.record.insert(kLane, 0, kFilter);
	const DeviceInstanceId owltt = r.record.insert(kLane, 0, kOwltt); // at the FRONT of the lane
	REQUIRE(env != kNoInstance);
	REQUIRE(filter != kNoInstance);
	REQUIRE(owltt != kNoInstance);
	CHECK(env != filter);
	CHECK(filter != owltt);

	// EACH IN ITS OWN CHAIN, AND THE ONE ADDED AT THE FRONT IS AT THE FRONT.
	CHECK(orderOf(r.record, kStrip) == std::vector<DeviceInstanceId> { env });
	CHECK(orderOf(r.record, kLane) == std::vector<DeviceInstanceId> { owltt, filter });
	CHECK(r.record.chainOf(filter) == kLane);
	CHECK(r.record.positionOf(filter) == 1);
	CHECK(r.record.chainOf(env) == kStrip);
	CHECK(r.record.descriptorOf(filter) == deviceForTag(kFilter));

	// THE DEFAULT ROW, read back against the descriptor rather than against what the record wrote.
	const device::Descriptor* d = deviceForTag(kFilter);
	const DeviceState& row = r.record.rowOf(filter);
	CHECK(row.instanceId == filter);
	CHECK(row.fieldsPresent == d->params.size());
	for (size_t c = 0; c < d->params.size(); ++c) {
		CAPTURE(c);
		CHECK(row.field[c] == static_cast<float>(d->params[c].def));
	}
	CHECK(r.record.consistentForTest());
}

TEST_CASE("a refused verb changes nothing") {
	Rig r;
	const DeviceInstanceId held = r.record.insert(kLane, 0, kFilter);
	uint64_t revision = r.record.revision();
	ChainState before(r.record.state().capacity(), r.record.chainCount());
	REQUIRE(before.assign(r.record.state()));

	SUBCASE("a tag this build does not know, from a hand") {
		CHECK(r.record.insert(kLane, 0, "zzzz") == kNoInstance);
	}
	SUBCASE("no such chain") {
		CHECK(r.record.insert(7, 0, kFilter) == kNoInstance);
		CHECK_FALSE(r.record.move(7, 0, 0));
	}
	// ("A FULL CHAIN" WAS A REFUSAL HERE until unbounded devices (step 6); a chain at its capacity grows now, which
	// changes the state, so it has its own case below.)
	SUBCASE("a move out of range, a remove of a device nobody holds, a field nobody holds") {
		CHECK_FALSE(r.record.move(kLane, 0, 3));
		CHECK_FALSE(r.record.remove(500));
		CHECK_FALSE(r.record.remove(kNoInstance));
		CHECK_FALSE(r.record.setField(500, 0, 1.0f));
		CHECK_FALSE(r.record.setField(held, 0, std::nanf("")));
		CHECK_FALSE(r.record.setField(held, device::kMaxDeviceFields, 1.0f));
	}
	SUBCASE("a put-back that collides") {
		CHECK_FALSE(r.record.insertRow(kLane, 0, rowFor(kOwltt, kNoInstance, {})));  // under no id
		CHECK_FALSE(r.record.insertRow(kLane, 0, rowFor(kOwltt, held, {})));         // its id is in use
		CHECK_FALSE(r.record.insertRow(7, 0, rowFor(kOwltt, 500, {})));              // no such chain
	}
	// NOTHING CHANGED, AND NOTHING SAYS IT DID — a revision that moved would republish for nothing.
	CHECK(r.record.state() == before);
	CHECK(r.record.revision() == revision);
	CHECK(r.record.consistentForTest());
}

TEST_CASE("a load puts a device back exactly — a tag this build does not know included — and the counter "
          "moves past it") {
	Rig r;
	REQUIRE(r.record.insertRow(kLane, 0, rowFor(kFilter, 40, { 0.5f })));
	REQUIRE(r.record.insertRow(kLane, 1, rowFor("zzzz", 41, { 1.0f, 2.0f })));
	CHECK(orderOf(r.record, kLane) == std::vector<DeviceInstanceId> { 40, 41 });
	CHECK(r.record.rowOf(40).field[0] == 0.5f);
	CHECK(r.record.deviceAt(kLane, 1) == 41);
	// KEPT, IN ITS PLACE, WITH ITS OWN ROW — only not run: there is nothing that runs it.
	CHECK(r.record.descriptorOf(41) == nullptr);
	CHECK(r.record.rowOf(41).fieldsPresent == 2);
	CHECK(r.record.rowOf(41).tag == feathers::device::tagOf("zzzz"));
	// THE NEXT ID A HAND GETS IS PAST EVERY ONE A LOAD PUT BACK.
	const DeviceInstanceId next = r.record.insert(kLane, 99, kOwltt);
	CHECK(next > 41);
	CHECK(r.record.consistentForTest());
}

TEST_CASE("a row travels with its device: a move rotates, a remove takes only its own device") {
	Rig r;
	const DeviceInstanceId a = r.record.insert(kLane, 99, kFilter);
	const DeviceInstanceId b = r.record.insert(kLane, 99, kOwltt);
	const DeviceInstanceId c = r.record.insert(kLane, 99, kFilter);
	REQUIRE(r.record.setField(a, 0, 0.125f));
	REQUIRE(r.record.setField(c, 0, 0.875f));
	REQUIRE(orderOf(r.record, kLane) == std::vector<DeviceInstanceId> { a, b, c });

	REQUIRE(r.record.move(kLane, 0, 2)); // a rotation, not a swap: b and c shift up one
	CHECK(orderOf(r.record, kLane) == std::vector<DeviceInstanceId> { b, c, a });
	CHECK(r.record.rowOf(a).instanceId == a);
	CHECK(r.record.rowOf(a).field[0] == 0.125f); // ITS ROW CAME WITH IT
	CHECK(r.record.positionOf(a) == 2);

	REQUIRE(r.record.remove(b));
	CHECK(orderOf(r.record, kLane) == std::vector<DeviceInstanceId> { c, a });
	CHECK(r.record.rowOf(c).field[0] == 0.875f);
	CHECK_FALSE(r.record.holds(b));
	// AND THE NEXT DEVICE A HAND ADDS IS A NEW ONE — an id is never reissued, whatever the record does
	// with the storage the removed device gave back.
	const DeviceInstanceId d = r.record.insert(kLane, 0, kOwltt);
	CHECK(d != b);
	CHECK(orderOf(r.record, kLane) == std::vector<DeviceInstanceId> { d, c, a });
	CHECK(r.record.consistentForTest());
}

TEST_CASE("a row's fields: one at a time widens it, all at once replaces it") {
	Rig r;
	const DeviceInstanceId id = r.record.insert(kLane, 0, kFilter);
	const size_t declared = r.record.rowOf(id).fieldsPresent;
	REQUIRE(r.record.setField(id, declared + 3, 7.0f)); // extra state past the controls
	CHECK(r.record.rowOf(id).fieldsPresent == declared + 4);
	CHECK(r.record.rowOf(id).field[declared + 3] == 7.0f);

	const std::vector<float> fields { 0.25f, 0.5f };
	REQUIRE(r.record.setFields(id, fields));
	CHECK(r.record.rowOf(id).fieldsPresent == 2);
	CHECK(r.record.rowOf(id).field[1] == 0.5f);
	CHECK(r.record.rowOf(id).field[declared + 3] == 0.0f); // replaced, not merged
}

namespace {
rack::SlotBinding slotBinding(DeviceInstanceId device, size_t control, size_t slot) {
	return rack::SlotBinding { .device = device,
	                           .control = static_cast<uint16_t>(control),
	                           .slot = static_cast<uint16_t>(slot),
	                           .live = true };
}
size_t liveBindings(const ChainRecord& r) {
	size_t n = 0;
	for (const rack::SlotBinding& b : r.state().slotBindings()) {
		n += b.live ? 1 : 0;
	}
	return n;
}
} // namespace

TEST_CASE("a patch load clears slot bindings") {
	// HOST SLOTS, T5 REVIEW. A load re-reserves the saved instance ids, so a binding that survived it would
	// re-attach to whatever the patch puts under the same id — here an unrelated device, and the bus would
	// overwrite its control on every drain. The load road is `applyPatchToRecord`, which clears the record.
	Rig r;
	REQUIRE(r.record.insertRow(kLane, 0, rowFor(kFilter, 40, { 0.5f })));
	REQUIRE(r.record.setSlotBinding(3, slotBinding(40, 0, 3)));
	REQUIRE(r.record.setSlotBinding(7, slotBinding(40, 1, 7)));
	REQUIRE(liveBindings(r.record) == 2); // the case is not vacuous: there were bindings to lose

	rack::RackPatch patch;
	patch.present = true;
	rack::RackPatch::Device d;
	d.chain = static_cast<uint32_t>(kLane);
	d.tag = rack::patchTagOf(kOwltt); // AN UNRELATED DEVICE, under the SAME id
	d.instanceId = 40;
	test::setRow(d, fx::owltt::kDescriptor, std::vector<float>(fx::owltt::kDescriptor.params.size(), 0.25f));
	patch.device.push_back(d);
	const backplane::Axes axes = feathersAxes();
	(void)rack::applyPatchToRecord(patch, &r.record, nullptr, &r.ids, rack::rackCoreCodec(axes));
	REQUIRE(r.record.holds(40)); // the id WAS re-reserved — the hazard is real in this fixture
	REQUIRE(r.record.rowOf(40).tag == device::tagOf(kOwltt)); // ...for an unrelated device
	CHECK(liveBindings(r.record) == 0);
	for (const rack::SlotBinding& b : r.record.state().slotBindings()) {
		CHECK_FALSE(b.live);
	}
}

TEST_CASE("removing a device drops its bindings") {
	Rig r;
	const DeviceInstanceId a = r.record.insert(kLane, 99, kFilter);
	const DeviceInstanceId b = r.record.insert(kLane, 99, kFilter);
	REQUIRE(r.record.setSlotBinding(0, slotBinding(a, 0, 0)));
	REQUIRE(r.record.setSlotBinding(1, slotBinding(b, 0, 1)));
	REQUIRE(r.record.setSlotBinding(2, slotBinding(a, 1, 2)));
	const uint64_t before = r.record.revision();

	REQUIRE(r.record.remove(a));
	CHECK(r.record.revision() != before);
	const std::span<const rack::SlotBinding> t = r.record.state().slotBindings();
	CHECK_FALSE(t[0].live);
	CHECK_FALSE(t[2].live);
	// ONLY ITS OWN: the other device keeps its slot.
	CHECK(t[1] == slotBinding(b, 0, 1));
	CHECK(liveBindings(r.record) == 1);
	// AND A MOVE IS NOT A REMOVE — a reorder keeps every binding (design §6).
	const DeviceInstanceId c = r.record.insert(kLane, 99, kOwltt);
	REQUIRE(r.record.move(kLane, 0, 1));
	CHECK(r.record.state().slotBindings()[1] == slotBinding(b, 0, 1));
	(void)c;
}

TEST_CASE("one binding per host slot: the table is indexed by slot") {
	Rig r;
	const DeviceInstanceId a = r.record.insert(kLane, 99, kFilter);
	// A LIVE BINDING NAMING ANOTHER SLOT CANNOT BE WRITTEN at entry `n`, so two entries can never drive one slot.
	CHECK_FALSE(r.record.setSlotBinding(4, slotBinding(a, 0, 5)));
	CHECK_FALSE(r.record.state().slotBindings()[4].live);
	CHECK(r.record.setSlotBinding(5, slotBinding(a, 0, 5)));
	CHECK_FALSE(r.record.setSlotBinding(rack::kSlotBindingCapacity, slotBinding(a, 0, rack::kSlotBindingCapacity)));
	// A DEAD ENTRY IS WRITABLE ANYWHERE — that is how a slot is unbound.
	CHECK(r.record.setSlotBinding(5, rack::SlotBinding {}));
	CHECK_FALSE(r.record.state().slotBindings()[5].live);
	// AND THE PUBLISH CARRIES THE TABLE WHOLE, entry for entry.
	REQUIRE(r.record.setSlotBinding(9, slotBinding(a, 2, 9)));
	rack::PublishedChainState published(r.record.state().capacity(), r.record.state().chainCount());
	ChainState current(r.record.state().capacity(), r.record.state().chainCount());
	ChainState staging(r.record.state().capacity(), r.record.state().chainCount());
	uint32_t seq = 0;
	REQUIRE(published.publish(r.record.state()));
	REQUIRE(published.adopt(&current, &staging, &seq));
	CHECK(current == r.record.state());
	CHECK(current.slotBindings()[9] == slotBinding(a, 2, 9));
}

TEST_CASE("clear empties every chain, and capacities that do not fit build a record with none") {
	Rig r;
	const DeviceInstanceId filter = r.record.insert(kLane, 0, kFilter);
	const DeviceInstanceId env = r.record.insert(kStrip, 0, kEnv);
	r.record.clear();
	CHECK(r.record.length(kLane) == 0);
	CHECK(r.record.length(kStrip) == 0);
	CHECK_FALSE(r.record.holds(filter));
	CHECK_FALSE(r.record.holds(env));
	CHECK(r.record.consistentForTest());

	backplane::InstanceCounter ids;
	// **CAPACITIES THAT DO NOT FIT (kill-lanes step 4d).** The case was bands that OVERLAPPED — two chains
	// claiming one slot, which meant two devices at one address. There are no addresses; the only way a
	// set of chains can be impossible now is to promise more room than the pool has.
	const ChainCapacity kTooMany[] = { feathersAxes().deviceSlots, 1 };
	ChainRecord bad { feathersAxes(), kTooMany, hostCatalogue(), ids, &backplane::permitAll };
	CHECK(bad.chainCount() == 0);
	CHECK(bad.insert(0, 0, kFilter) == kNoInstance);
}

//------------------------------------------------------------------------------------------------//
// ROUTES                                                                                         //
//------------------------------------------------------------------------------------------------//

namespace {

/// THE RACK'S SHAPE, small: one chain over every lane, eight macros, no host destinations.
constexpr backplane::Axes kRackish {
	.coreSources = 8, .coreDestinations = 0, .deviceSlots = 4, .outputStride = 4,
};
constexpr ChainCapacity kWhole[] = { 4 };

struct RouteRig {
	backplane::InstanceCounter ids;
	ChainRecord record { kRackish, kWhole, hostCatalogue(), ids, &backplane::permitAll };
	DeviceInstanceId envId = kNoInstance;
	DeviceInstanceId filterId = kNoInstance;
	RouteRig() {
		envId = record.insert(0, 0, kEnv);       // a modulator, upstream
		filterId = record.insert(0, 1, kFilter); // an audio device after it
	}
	/// BY ENDPOINT (kill-lanes step 2): a device end is the device's id and its OWN control index — the
	/// first modulatable one, which for the filter is not its control 0.
	backplane::Source envOut() const { return backplane::Source::output(envId, 0); }
	backplane::Destination filterIn() const {
		return backplane::Destination::control(filterId, deviceForTag(kFilter)->controlOfNthModulatableBesideEnable(0));
	}
	backplane::Destination envIn() const {
		return backplane::Destination::control(envId, deviceForTag(kEnv)->controlOfNthModulatableBesideEnable(0));
	}
	static backplane::Source macro(size_t i) { return backplane::Source::core(i); }
};

} // namespace

TEST_CASE("a route the rules allow is written, one they refuse is not, and zero removes") {
	RouteRig r;
	REQUIRE(deviceForTag(kEnv)->outputCount() > 0);
	REQUIRE(deviceForTag(kFilter)->modulatableCount() > 0);

	CHECK(r.record.setRoute(r.envOut(), r.filterIn(), 0.5f)); // downstream: allowed
	CHECK(r.record.routeAmount(r.envOut(), r.filterIn()) == 0.5f);
	CHECK(r.record.setRoute(r.macro(0), r.filterIn(), -0.25f)); // a macro: upstream of everything
	CHECK(r.record.routeAmount(r.macro(0), r.filterIn()) == -0.25f);

	const uint64_t revision = r.record.revision();
	// REFUSED, AND NOTHING WRITTEN: a device into itself, a route into a device nobody holds, a non-finite
	// amount.
	CHECK_FALSE(r.record.setRoute(r.envOut(), r.envIn(), 0.5f));
	CHECK_FALSE(r.record.setRoute(r.macro(0), backplane::Destination::control(r.filterId + 100, 0), 0.5f));
	CHECK_FALSE(r.record.setRoute(r.macro(0), r.filterIn(), std::nanf("")));
	CHECK(r.record.routeAmount(r.envOut(), r.envIn()) == 0.0f);
	CHECK(r.record.revision() == revision);

	CHECK(r.record.setRoute(r.envOut(), r.filterIn(), 0.0f));
	CHECK(r.record.routeAmount(r.envOut(), r.filterIn()) == 0.0f);
}

TEST_CASE("backlog HS: the RECORD writes a self-route its device declared, refuses the ones that feed its outputs, keeps it across a reorder — and still says a holder is not upstream of itself") {
	// THE AUTHORITY BOTH HOSTS EDIT THROUGH (`ChainRecord`), so this is the road a drag takes in Feathers
	// and in the rack alike. The env-into-itself refusal in the case above is the undeclared half.
	namespace sk = fx::sklitter;
	RouteRig r;
	const DeviceInstanceId self = r.record.insert(0, 2, sk::kTraits.tag);
	REQUIRE(self != kNoInstance);
	const backplane::Source perfX = backplane::Source::output(self, sk::outIndex(sk::Out::X));
	const auto to = [&](sk::Param p) { return backplane::Destination::control(self, static_cast<size_t>(p)); };

	CHECK(r.record.mayRoute(perfX, to(sk::Param::Rate)));
	CHECK(r.record.setRoute(perfX, to(sk::Param::Rate), 0.5f));
	CHECK(r.record.routeAmount(perfX, to(sk::Param::Rate)) == 0.5f);

	const uint64_t revision = r.record.revision();
	for (const sk::Param feeds : { sk::Param::PerfX, sk::Param::PerfY, sk::Param::PerfDepth }) {
		CAPTURE(static_cast<int>(feeds));
		// NOT FOR WANT OF A DESTINATION — a macro reaches the puck, so the refusal below is the loop's.
		CHECK(r.record.mayRoute(RouteRig::macro(0), to(feeds)));
		CHECK_FALSE(r.record.mayRoute(perfX, to(feeds)));
		CHECK_FALSE(r.record.setRoute(perfX, to(feeds), 0.5f));
		CHECK(r.record.routeAmount(perfX, to(feeds)) == 0.0f);
	}
	CHECK(r.record.revision() == revision);

	// THE CHAIN INSIDE IT IS A DIFFERENT QUESTION, with the old answer (trap T1) — for every output, though
	// control 0 (Zone A), which is how this question spells "the chain", is one the declaration lets through.
	REQUIRE(r.record.mayRoute(perfX, backplane::Destination::control(self, 0)));
	for (size_t out = 0; out < sk::kPerformOutputs; ++out) {
		CAPTURE(out);
		CHECK_FALSE(r.record.sourceUpstreamOf(backplane::Source::output(self, out), self));
	}
	CHECK(r.record.sourceUpstreamOf(r.envOut(), self)); // and a sibling before it still is

	// DRAGGED TO THE FRONT AND BACK: no reorder moves a device relative to itself.
	REQUIRE(r.record.move(0, 2, 0));
	CHECK(r.record.routeAmount(perfX, to(sk::Param::Rate)) == 0.5f);
	REQUIRE(r.record.move(0, 0, 2));
	CHECK(r.record.routeAmount(perfX, to(sk::Param::Rate)) == 0.5f);
}

TEST_CASE("a reorder deletes the routes it makes backwards, and a removal takes its device's routes") {
	RouteRig r;
	REQUIRE(r.record.setRoute(r.envOut(), r.filterIn(), 0.5f));
	REQUIRE(r.record.setRoute(r.macro(0), r.filterIn(), 0.25f));

	SUBCASE("a reorder that breaks nothing: an envelope dragged after the filter it modulates") {
		// **THIS SUBCASE WATCHED THE ROUTE GO, AND SINCE BACKLOG IZ IT STAYS.** By design, for
		// modulation "downstream" means computed from downstream audio; a modulator's position in a lane
		// is only grouping. The envelope is ticked before the filter runs wherever the two stand, so the
		// drag breaks nothing and the 2026-09-06 deletion rule has nothing to delete. The deletion
		// itself is asked below, of two devices that both tick and run.
		REQUIRE(r.record.move(0, 0, 1)); // the envelope now AFTER the filter it modulated
		CHECK(r.record.routeAmount(r.envOut(), r.filterIn()) == 0.5f);
		CHECK(r.record.routeAmount(r.macro(0), r.filterIn()) == 0.25f); // a macro's route breaks no order
		REQUIRE(r.record.move(0, 1, 0));
		CHECK(r.record.routeAmount(r.envOut(), r.filterIn()) == 0.5f);
	}

	SUBCASE("a reorder that DOES break one: SKLITTER into a SKLITTER, dragged after it") {
		// A reorder that broke routes deleted them for good; a broken route must survive the reorder.
		// Two devices that are both ticked and run cannot be held in one order both ways (backlog IZ).
		const DeviceInstanceId first = r.record.insert(0, 99, "SKLITTER");
		const DeviceInstanceId second = r.record.insert(0, 99, "SKLITTER");
		REQUIRE(first != kNoInstance);
		REQUIRE(second != kNoInstance);
		const size_t length = r.record.length(0);
		const backplane::Source out = backplane::Source::output(first, 0);
		const backplane::Destination rate =
		    backplane::Destination::control(second, static_cast<size_t>(fx::sklitter::Param::Rate));
		REQUIRE(r.record.setRoute(out, rate, 0.5f));
		REQUIRE(r.record.move(0, length - 2, length - 1)); // first now AFTER second
		CHECK(r.record.routeAmount(out, rate) == 0.0f);
		CHECK(r.record.routeAmount(r.envOut(), r.filterIn()) == 0.5f); // the control: nothing else went
		REQUIRE(r.record.move(0, length - 1, length - 2));
		CHECK(r.record.routeAmount(out, rate) == 0.0f); // gone, not suspended
	}

	SUBCASE("a removal, and the device that takes its place arrives unrouted") {
		REQUIRE(r.record.remove(r.filterId));
		CHECK(r.record.routeAmount(r.macro(0), r.filterIn()) == 0.0f);
		CHECK(r.record.routeAmount(r.envOut(), r.filterIn()) == 0.0f);
		// THE ABA GUARD: a device handed the storage the removed one gave back must not arrive
		// pre-modulated by routes nobody drew — which is why a route names an ID and not a place.
		const DeviceInstanceId owltt = r.record.insert(0, 99, kOwltt);
		REQUIRE(owltt != kNoInstance);
		const size_t control = deviceForTag(kOwltt)->controlOfNthModulatableBesideEnable(0);
		CHECK(r.record.routeAmount(r.macro(0), backplane::Destination::control(owltt, control)) == 0.0f);
		// AND THE ROUTE NAMED THE FILTER, WHICH IS GONE — an id is never reissued, so nothing can inherit it.
		CHECK(owltt != r.filterId);
		CHECK(r.record.routeAmount(r.macro(0), r.filterIn()) == 0.0f);
	}
}

TEST_CASE("replacing a chain by the least change keeps an untouched device's routes, and a reorder deletes only the ones it makes backwards") {
	// **THE WRITE-BACK A HOST WITH WHOLE-CHAIN RULES NEEDS.** Feathers' rules answer with a chain value;
	// writing it back by removing and re-inserting every device would take every route with them, so a
	// knob-free add would unroute the rack. `replaceChain` keeps what it can.
	RouteRig r;
	REQUIRE(r.record.setRoute(r.envOut(), r.filterIn(), 0.5f));
	REQUIRE(r.record.setRoute(r.macro(0), r.filterIn(), 0.25f));
	const DeviceState env = r.record.rowOf(r.envId);
	DeviceState filt = r.record.rowOf(r.filterId);
	const DeviceInstanceId envId = env.instanceId;

	SUBCASE("an add, and a field changed on a kept device") {
		const DeviceState owltt = rowFor(kOwltt, 900, { 0.5f });
		filt.field[0] = filt.field[0] + 1.0f;
		const DeviceState rows[] = { env, filt, owltt };
		CHECK(r.record.replaceChain(0, rows) == 0);
		CHECK(r.record.routeAmount(r.envOut(), r.filterIn()) == 0.5f); // KEPT — the whole point
		CHECK(r.record.routeAmount(r.macro(0), r.filterIn()) == 0.25f);
		CHECK(r.record.rowOf(r.filterId).field[0] == filt.field[0]);
		CHECK(orderOf(r.record, 0) == std::vector<DeviceInstanceId> { envId, r.filterId, 900 });
	}
	SUBCASE("a reorder: an envelope after its filter breaks nothing (backlog IZ), and the macro's stays") {
		// The envelope ticks before the filter runs wherever the two stand, so the route is not backwards.
		const DeviceState rows[] = { filt, env };
		CHECK(r.record.replaceChain(0, rows) == 0);
		CHECK(orderOf(r.record, 0) == std::vector<DeviceInstanceId> { r.filterId, envId });
		CHECK(r.record.routeAmount(r.envOut(), r.filterIn()) == 0.5f);
		CHECK(r.record.routeAmount(r.macro(0), r.filterIn()) == 0.25f);
	}
	SUBCASE("a removal takes its device's routes; another device stamped onto a kept id is a replacement") {
		const DeviceState only[] = { env };
		CHECK(r.record.replaceChain(0, only) == 0);
		CHECK(r.record.length(0) == 1);
		CHECK(r.record.routeAmount(r.macro(0), r.filterIn()) == 0.0f);

		DeviceState stamped = rowFor(kOwltt, 0, { 0.5f });
		stamped.instanceId = envId; // the envelope's id, another device
		const DeviceState rows[] = { stamped };
		CHECK(r.record.replaceChain(0, rows) == 0);
		CHECK(r.record.descriptorOf(envId) == deviceForTag(kOwltt));
		CHECK(r.record.length(0) == 1);
		CHECK(orderOf(r.record, 0) == std::vector<DeviceInstanceId> { envId });
	}
	CHECK(r.record.consistentForTest());
}

TEST_CASE("the state a record publishes carries the graph's evaluation order, and a route across two chains moves it") {
	// KILL-LANES 4c, THE RECORD'S HALF. `Graph::order()` is one topological order over every node, and it
	// is what the engine must run — so the record writes it into the state it publishes after every
	// mutation, and counts the times it actually changed (`orderRevision`) so a bridge knows when a ROUTE,
	// which moves nothing in any chain, has moved a device in the order and the chains must go out again.
	//
	// THE SHAPE IS THE ONE THE CHAIN-INDEX WALK GOT WRONG: a modulator in the lane routed into a modulator
	// in the strip. Nothing orders the two chains against each other, so the route is legal, and under
	// "the strip, then the lane" the strip's envelope would have run first and read the MSEG's output from
	// the tick before. In the graph's order the MSEG runs first. The core node is in the order too and is
	// filtered out here: this case is about the devices.
	backplane::InstanceCounter ids;
	constexpr ChainCapacity kTwoByTwo[] = { 2, 2 };
	ChainRecord record { feathersAxes(), kTwoByTwo, hostCatalogue(), ids, &backplane::permitAll };
	const DeviceInstanceId env = record.insert(kStrip, 0, kEnv);
	const DeviceInstanceId mseg = record.insert(kLane, 0, mod::mseg::kTraits.tag);
	REQUIRE(env != kNoInstance);
	REQUIRE(mseg != kNoInstance);
	const auto devicesInOrder = [&] {
		std::vector<DeviceInstanceId> out;
		for (const DeviceInstanceId id : record.state().order()) {
			if (id != backplane::Graph::kCoreInstance) {
				out.push_back(id);
			}
		}
		return out;
	};
	// UNROUTED: nothing orders them, so the sort takes the lower lane first — the envelope, inserted first.
	CHECK(devicesInOrder() == std::vector<DeviceInstanceId> { env, mseg });
	CHECK(record.state().order().size() == 3); // and the host's own node is in it

	const size_t attack = static_cast<size_t>(mod::dahdsr::Param::Attack);
	REQUIRE(mod::dahdsr::kDescriptor.params[attack].modulatable);
	const backplane::Source from = backplane::Source::output(mseg, 0);
	const backplane::Destination into = backplane::Destination::control(env, attack);
	REQUIRE(record.mayRoute(from, into)); // legal: no precedence between the chains says otherwise

	const uint64_t before = record.orderRevision();
	REQUIRE(record.setRoute(from, into, 0.5f));
	// THE ROUTE IS A PRECEDENCE: the MSEG now runs before the envelope that reads it, and the count moved.
	CHECK(devicesInOrder() == std::vector<DeviceInstanceId> { mseg, env });
	CHECK(record.orderRevision() == before + 1);

	// A SECOND ROUTE THE ORDER ALREADY HAD THE RIGHT WAY ROUND MOVES NOTHING — and says so.
	const size_t decay = static_cast<size_t>(mod::dahdsr::Param::Decay);
	REQUIRE(mod::dahdsr::kDescriptor.params[decay].modulatable);
	REQUIRE(record.setRoute(from, backplane::Destination::control(env, decay), 0.25f));
	CHECK(record.orderRevision() == before + 1);

	// ZEROING BOTH PUTS THE ORDER BACK, counted again.
	REQUIRE(record.setRoute(from, into, 0.0f));
	REQUIRE(record.setRoute(from, backplane::Destination::control(env, decay), 0.0f));
	CHECK(devicesInOrder() == std::vector<DeviceInstanceId> { env, mseg });
	CHECK(record.orderRevision() == before + 2);

	// AND THE PUBLISHED STATE CARRIES IT ACROSS THE THREAD BOUNDARY WHOLE — the snapshot a drain takes has
	// the order the record wrote, not an empty one.
	REQUIRE(record.setRoute(from, into, 0.5f));
	rack::PublishedChainState published(record.state().capacity(), record.state().chainCount());
	REQUIRE(published.publish(record.state()));
	rack::ChainState current(record.state().capacity(), record.state().chainCount());
	rack::ChainState staging(record.state().capacity(), record.state().chainCount());
	uint32_t sequence = 0;
	REQUIRE(published.adopt(&current, &staging, &sequence));
	CHECK(current == record.state());
	CHECK(std::vector<DeviceInstanceId>(current.order().begin(), current.order().end())
	      == std::vector<DeviceInstanceId>(record.state().order().begin(), record.state().order().end()));
}

TEST_CASE("the routes a record publishes are exactly the routes it holds — by identity, and back") {
	RouteRig r;
	REQUIRE(r.record.setRoute(r.envOut(), r.filterIn(), 0.5f));
	REQUIRE(r.record.setRoute(r.macro(3), r.filterIn(), 0.125f));
	backplane::RouteList out;
	r.record.publishRoutes(out);
	std::vector<backplane::Route> got;
	out.forEach([&got](const backplane::Route& route) { got.push_back(route); });

	// **BY IDENTITY** (kill-lanes step 1): each device end is the device's id and its own output or
	// CONTROL index — the filter's first modulatable control, which is not its control 0 — never a lane.
	const DeviceInstanceId envId = r.envId;
	const DeviceInstanceId filterId = r.filterId;
	const size_t control = fx::filter::kDescriptor.controlOfNthModulatableBesideEnable(0);
	REQUIRE(control != 0);
	std::sort(got.begin(), got.end(), [](const backplane::Route& a, const backplane::Route& b) {
		return a.source < b.source;
	});
	CHECK(got == std::vector<backplane::Route> {
	                 { backplane::Source::core(3), backplane::Destination::control(filterId, control), 0.125f },
	                 { backplane::Source::output(envId, 0), backplane::Destination::control(filterId, control), 0.5f } });

	// **AND BACK** — each published route read from the record by its own endpoints (the coordinate
	// adapter this went through is gone with kill-lanes step 2).
	for (const backplane::Route& route : got) {
		CHECK(route.destination == r.filterIn());
		CHECK(r.record.routeAmount(route.source, route.destination) == route.amount);
	}
}

//------------------------------------------------------------------------------------------------//
// THE PATCH — one format for every host, in Feathers' encoding                                  //
//------------------------------------------------------------------------------------------------//

namespace {

/// A MEMORY STREAM, both ways.
struct Mem {
	std::vector<char> bytes;
	size_t at = 0;
	clap_ostream out {};
	clap_istream in {};
	Mem() {
		out.ctx = this;
		out.write = [](const clap_ostream* s, const void* p, uint64_t n) -> int64_t {
			auto* m = static_cast<Mem*>(s->ctx);
			m->bytes.insert(m->bytes.end(), static_cast<const char*>(p), static_cast<const char*>(p) + n);
			return static_cast<int64_t>(n);
		};
		in.ctx = this;
		in.read = [](const clap_istream* s, void* p, uint64_t n) -> int64_t {
			auto* m = static_cast<Mem*>(s->ctx);
			const size_t take = std::min<size_t>(static_cast<size_t>(n), m->bytes.size() - m->at);
			std::memcpy(p, m->bytes.data() + m->at, take);
			m->at += take;
			return static_cast<int64_t>(take);
		};
	}
};

} // namespace

TEST_CASE("a record saved and loaded through the one format is the same record — devices, rows, ids, order and routes") {
	RouteRig a;
	REQUIRE(a.record.setRoute(a.envOut(), a.filterIn(), 0.5f));
	REQUIRE(a.record.setRoute(RouteRig::macro(2), a.filterIn(), -0.25f));
	backplane::RouteList routes;
	a.record.publishRoutes(routes);
	const rack::CoreCodec codec = rack::rackCoreCodec(kRackish);
	const rack::RackPatch patch = rack::patchOfRecord(a.record, nullptr, {}, routes, codec);

	Mem m;
	REQUIRE(rack::writeRack(&m.out, patch));
	rack::RackPatch parsed;
	REQUIRE(rack::parseRack(&m.in, &parsed));
	CHECK(parsed == patch);

	backplane::InstanceCounter ids;
	ChainRecord b { kRackish, kWhole, hostCatalogue(), ids, &backplane::permitAll };
	const rack::LoadReport refused = rack::applyPatchToRecord(parsed, &b, nullptr, &ids, codec);
	CHECK(refused.empty()); // nothing refused, and — keyed — nothing missing, unknown or coerced either
	CHECK(b.state() == a.record.state()); // same devices, same ids, same order, same rows
	CHECK(b.routeAmount(a.envOut(), a.filterIn()) == 0.5f);
	CHECK(b.routeAmount(RouteRig::macro(2), a.filterIn()) == -0.25f);
}

TEST_CASE("a route names its device by id and its control by key — so it follows the device wherever it lands") {
	// **WHY FEATHERS' ENCODING WAS KEPT.** The rack's old format named a device end by LANE and ORDINAL: a
	// device loading anywhere but where it was saved took its routes' ends to whatever sat there. By id,
	// the route follows the device. And the file names no lane at all (lanes are gone), so a loader
	// places devices where IT likes — here, not where the saving record had them.
	backplane::InstanceCounter srcIds;
	ChainRecord a { kRackish, kWhole, hostCatalogue(), srcIds, &backplane::permitAll };
	const DeviceInstanceId filler = a.insert(0, 0, kOwltt);
	const DeviceInstanceId envId = a.insert(0, 1, kEnv);
	const DeviceInstanceId filterId = a.insert(0, 2, kFilter);
	REQUIRE(a.remove(filler));
	// **A CLAIM THIS CASE HAS LOST, SAID PLAINLY (kill-lanes step 3b).** It used to REQUIRE that the saving
	// record's pool slots were NOT dense — the filler's removal leaving the envelope parked at 1 — because a
	// format naming a place would then have to survive a loader that packs from 0. The record's placement is
	// private now and no case can see it, so this setup still MAKES that condition and can no longer assert
	// it. What is left below is the claim that matters and is still observable: the file names no place at
	// all, and the route lands on the same two devices in a record that placed them itself.
	REQUIRE(orderOf(a, 0) == std::vector<DeviceInstanceId> { envId, filterId });

	// THE CONTROL'S KEY ON THE WIRE — NOT THE ORDINAL, AND SINCE BACKLOG JF NOT THE INDEX EITHER. An ordinal
	// whose control index differs from it, so a file that wrote either number would be caught.
	const device::Descriptor& filter = fx::filter::kDescriptor;
	size_t ordinal = 0;
	while (ordinal < filter.modulatableCount() && filter.controlOfNthModulatable(ordinal) == ordinal) {
		++ordinal;
	}
	REQUIRE(ordinal < filter.modulatableCount());
	const backplane::Destination into = backplane::Destination::control(filterId, filter.controlOfNthModulatable(ordinal));
	REQUIRE(a.setRoute(backplane::Source::output(envId, 0), into, 0.5f));
	backplane::RouteList routes;
	a.publishRoutes(routes);
	const rack::CoreCodec codec = rack::rackCoreCodec(kRackish);
	const rack::RackPatch patch = rack::patchOfRecord(a, nullptr, {}, routes, codec);

	REQUIRE(patch.route.size() == 1);
	CHECK(patch.route[0].destination.kind == static_cast<uint32_t>(rack::DestKind::Device));
	CHECK(patch.route[0].destination.a == filterId);
	CHECK(patch.route[0].destination.key == filter.params[filter.controlOfNthModulatable(ordinal)].key);
	CHECK(patch.route[0].destination.b == 0);
	CHECK(patch.route[0].source.a == envId);
	CHECK(patch.route[0].source.key == mod::dahdsr::kOutputs[0].key);

	backplane::InstanceCounter ids;
	ChainRecord b { kRackish, kWhole, hostCatalogue(), ids, &backplane::permitAll };
	CHECK(rack::applyPatchToRecord(patch, &b, nullptr, &ids, codec).routes.count == 0);
	// THE LOADER'S OWN PLACES — not the saver's — and the route on the same two devices there.
	REQUIRE(orderOf(b, 0) == std::vector<DeviceInstanceId> { envId, filterId });
	CHECK(b.routeAmount(backplane::Source::output(envId, 0), into) == 0.5f);
	CHECK(b.consistentForTest());
}

TEST_CASE("a load whose ids collide gives the second device a fresh one that no later device is wearing") {
	// A MALFORMED FILE — two devices under one id — and a third wearing the id a fresh counter would issue
	// next. The loader reserves past every id the patch names BEFORE placing any, so the second device's
	// fresh id cannot be the third's.
	const auto device = [](const char* tag, DeviceInstanceId id) {
		rack::RackPatch::Device d;
		d.tag = rack::patchTagOf(tag);
		d.instanceId = id;
		return d;
	};
	rack::RackPatch patch;
	patch.present = true;
	patch.device.push_back(device(kEnv, kFirstInstance));
	patch.device.push_back(device(kFilter, kFirstInstance));
	patch.device.push_back(device(kOwltt, kFirstInstance + 1));
	// A MACRO TO THE THIRD DEVICE BY ITS ID, which a collision on that id would take elsewhere or refuse.
	rack::RackPatch::Route r;
	r.source = { static_cast<uint32_t>(rack::SourceKind::Macro), 3, 0, {} };
	r.destination = { static_cast<uint32_t>(rack::DestKind::Device), static_cast<uint32_t>(kFirstInstance + 1), 0,
		              fx::owltt::kDescriptor.params[fx::owltt::kDescriptor.controlOfNthModulatableBesideEnable(0)].key };
	r.amount = 0.5f;
	patch.route.push_back(r);

	backplane::InstanceCounter ids;
	ChainRecord b { kRackish, kWhole, hostCatalogue(), ids, &backplane::permitAll };
	CHECK(rack::applyPatchToRecord(patch, &b, nullptr, &ids, rack::rackCoreCodec(kRackish)).routes.count == 0);
	REQUIRE(b.length(0) == 3);
	// IN THE PATCH'S OWN ORDER: the envelope under its own id, the filter under a fresh one, the owltt under
	// the id it named — which the second device must not have taken.
	CHECK(b.deviceAt(0, 0) == kFirstInstance);
	CHECK(b.deviceAt(0, 2) == kFirstInstance + 1); // its own, not taken by the second device
	CHECK(b.deviceAt(0, 1) == kFirstInstance + 2); // fresh, past everything the patch named
	const auto firstIn = [](const ChainRecord& rec, size_t position) {
		const DeviceInstanceId id = rec.deviceAt(0, position);
		return backplane::Destination::control(id, rec.descriptorOf(id)->controlOfNthModulatableBesideEnable(0));
	};
	CHECK(b.routeAmount(backplane::Source::core(3), firstIn(b, 2)) == 0.5f);
	CHECK(b.consistentForTest());

	SUBCASE("and a route naming the id two devices wore is REFUSED, landing on neither") {
		// Answering would mean picking whichever was placed first — a patch that reopens mis-wired, and
		// quietly. The envelope's first modulatable control and the filter's are both destinations here,
		// so "landed on neither" is a claim with two places to be wrong.
		rack::RackPatch doubled = patch;
		rack::RackPatch::Route toDoubled;
		// THE KEY THE ENVELOPE ALSO HAS AT THAT INDEX would be the tell of a positional read; by key, the id
		// resolving to the envelope (the one that kept it) is what must be refused.
		toDoubled.source = { static_cast<uint32_t>(rack::SourceKind::Macro), 1, 0, {} };
		toDoubled.destination = { static_cast<uint32_t>(rack::DestKind::Device), static_cast<uint32_t>(kFirstInstance), 0,
			                      fx::filter::kDescriptor.params[fx::filter::kDescriptor.controlOfNthModulatableBesideEnable(0)].key };
		toDoubled.amount = 0.25f;
		doubled.route.push_back(toDoubled);
		backplane::InstanceCounter ids2;
		ChainRecord c { kRackish, kWhole, hostCatalogue(), ids2, &backplane::permitAll };
		const rack::LoadReport report =
		    rack::applyPatchToRecord(doubled, &c, nullptr, &ids2, rack::rackCoreCodec(kRackish));
		const rack::RouteRefusals& refused = report.routes;
		CHECK(refused.count == 1);
		// **THE REFUSAL NAMES WHAT THE FILE ASKED FOR, NOT WHAT THE LOOKUP FOUND** (kill-lanes step 3b):
		// the macro it named, and the device-and-control it named. **SINCE BACKLOG JF THE CONTROL IS A KEY**,
		// and an index exists for it only on a device that declares that key: the id resolves to the envelope
		// here (it kept the id), which does not, so the endpoint is `nobody()` — an index made up for it would
		// name one of the envelope's controls, the silent re-point keys exist to end — and the report says
		// the device and the key IN WORDS instead.
		CHECK(refused.firstSource == backplane::Source::core(1));
		CHECK(refused.firstDestination == backplane::Destination::nobody());
		const std::string& said = report.firstOf(rack::LoadReport::Kind::RouteDropped).what;
		CHECK(said.find(std::string("'") + fx::filter::kDescriptor.params[fx::filter::kDescriptor.controlOfNthModulatableBesideEnable(0)].key + "'")
		      != std::string::npos);
		CHECK(said.find("device " + std::to_string(kFirstInstance)) != std::string::npos);
		CHECK(c.routeAmount(backplane::Source::core(1), firstIn(c, 0)) == 0.0f);
		CHECK(c.routeAmount(backplane::Source::core(1), firstIn(c, 1)) == 0.0f);
		CHECK(c.routeAmount(backplane::Source::core(3), firstIn(c, 2)) == 0.5f); // the rest land
	}
}

TEST_CASE("a load's base values obey the one rule — a non-finite field is the control's declared default") {
	// **`std::clamp(NaN, lo, hi)` IS NaN**, so no device's own guard sees one: the loader is the only place
	// it can be caught. The DEFAULT rather than zero, because zero is a different sound for most controls.
	const device::Descriptor& filter = fx::filter::kDescriptor;
	size_t continuous = filter.params.size();
	for (size_t i = 0; i < filter.params.size(); ++i) {
		if (!filter.params[i].stepped && filter.params[i].def != 0.0) {
			continuous = i;
			break;
		}
	}
	REQUIRE(continuous < filter.params.size()); // a control whose default is not the zero a bad repair writes
	rack::RackPatch patch;
	patch.present = true;
	rack::RackPatch::Device d;
	std::copy(kFilter, kFilter + 4, d.tag.begin());
	d.instanceId = kFirstInstance;
	std::vector<float> row(filter.params.size(), 0.0f);
	for (size_t i = 0; i < filter.params.size(); ++i) {
		row[i] = static_cast<float>(filter.params[i].def);
	}
	row[continuous] = std::nanf("");
	// AND AN OUT-OF-RANGE ONE, clamped by the same rule a hand's edit is.
	const size_t other = continuous + 1 < filter.params.size() && !filter.params[continuous + 1].stepped ? continuous + 1 : continuous;
	if (other != continuous) {
		row[other] = static_cast<float>(filter.params[other].max) + 1000.0f;
	}
	test::setRow(d, filter, row);
	patch.device.push_back(d);

	// AND THROUGH THE FILE, which is where a non-finite value really arrives from: written `null`.
	Mem m;
	REQUIRE(rack::writeRack(&m.out, patch));
	rack::RackPatch parsed;
	REQUIRE(rack::parseRack(&m.in, &parsed));
	REQUIRE(parsed.device.size() == 1);
	CHECK(test::valueOf(parsed.device[0], filter.params[continuous].key)->kind == rack::RackPatch::Value::Kind::Null);

	backplane::InstanceCounter ids;
	ChainRecord b { kRackish, kWhole, hostCatalogue(), ids, &backplane::permitAll };
	const rack::LoadReport report = rack::applyPatchToRecord(parsed, &b, nullptr, &ids, rack::rackCoreCodec(kRackish));
	REQUIRE(b.length(0) == 1);
	const DeviceState& loaded = b.rowOf(b.deviceAt(0, 0));
	CHECK(loaded.field[continuous] == static_cast<float>(filter.params[continuous].def));
	if (other != continuous) {
		CHECK(loaded.field[other] == static_cast<float>(filter.params[other].max));
	}
	// AND BOTH ARE SAID — a value corrected, the first by its key.
	CHECK(report.countOf(rack::LoadReport::Kind::ValueCoerced) == (other != continuous ? 2u : 1u));
	CHECK(report.firstOf(rack::LoadReport::Kind::ValueCoerced).what == filter.params[continuous].key);
}

TEST_CASE("a device this build cannot run is kept with every value it came with, and saved back out") {
	// ONE RULE FOR EVERY CHAIN (the lane kept one; Feathers' strip used to drop one). Its values are kept
	// too — the old device record dropped an unknown tag's payload, so it came back "present and at its
	// defaults", a worse report than "gone" because there was nothing left to look for. **SINCE BACKLOG JF
	// THEY WAIT BESIDE ITS BODY** (`ContainerBodies::carry`): a row is floats by index and has no room for a
	// key, so a record's host carries them there — which is why this case hands the applier a store.
	rack::RackPatch patch;
	patch.present = true;
	rack::RackPatch::Device d;
	// Through a named pointer: `"zzz9" + 4` is the end of the literal, which clang's -Wstring-plus-int reads
	// as a mistaken append (macOS CI).
	const char* const unknownTag = "zzz9";
	std::copy(unknownTag, unknownTag + 4, d.tag.begin());
	d.instanceId = 7;
	d.schema = 3;
	d.values = { rack::RackPatch::Value::of("gain", 0.25f), rack::RackPatch::Value::label("mode", "Wide"),
		         rack::RackPatch::Value::of("far", 1e6f) };
	d.extra = { rack::RackPatch::Value { "travel", rack::RackPatch::Value::Kind::Raw, 0.0f, "[1,-3.5]" } };
	patch.device.push_back(d);

	backplane::InstanceCounter ids;
	ContainerBodies bodies;
	ChainRecord b { kRackish, kWhole, hostCatalogue(), ids, &backplane::permitAll };
	const rack::LoadReport report = rack::applyPatchToRecord(patch, &b, &bodies, &ids, rack::rackCoreCodec(kRackish));
	CHECK(report.countOf(rack::LoadReport::Kind::DeviceUnknown) == 1);
	CHECK(report.firstOf(rack::LoadReport::Kind::DeviceUnknown).what == "zzz9");
	REQUIRE(b.length(0) == 1);
	REQUIRE(b.holds(7));
	CHECK(b.descriptorOf(7) == nullptr);
	backplane::RouteList routes;
	b.publishRoutes(routes);
	const rack::RackPatch back = rack::patchOfRecord(b, &bodies, {}, routes, rack::rackCoreCodec(kRackish));
	REQUIRE(back.device.size() == 1);
	CHECK(back.device[0] == d);
}

TEST_CASE("removing a device takes every route naming it — whichever end, host ends included — and none else") {
	// **THE RECORD'S HALF OF THE ABA GUARD** (kill-lanes step 2). A route names its device's id; a stale
	// one naming a dead id compiles to nothing and so cannot be heard — but it would still be published,
	// saved in the patch, and reported as refused on the next load. Found by the 2a port's known-bad:
	// with `Graph::remove` keeping route edges, nothing at this level went red.
	namespace bp = backplane;
	Rig r;
	const DeviceInstanceId env = r.record.insert(kStrip, 0, kEnv);
	const DeviceInstanceId flt = r.record.insert(kLane, 0, kFilter);
	REQUIRE(env != kNoInstance);
	REQUIRE(flt != kNoInstance);
	const bp::Destination filterIn = bp::Destination::control(flt, fx::filter::kDescriptor.controlOfNthModulatableBesideEnable(0));
	const bp::Source envOut = bp::Source::output(env, 0);
	REQUIRE(r.record.setRoute(bp::Source::core(0), filterIn, 0.25f)); // a macro into the filter
	REQUIRE(r.record.setRoute(envOut, filterIn, 0.5f));             // device to device
	REQUIRE(r.record.setRoute(envOut, bp::Destination::core(3), 0.75f)); // the envelope into a host parameter
	REQUIRE(r.record.setRoute(bp::Source::core(1), bp::Destination::core(3), -0.5f)); // host to host
	const auto count = [&] {
		size_t n = 0;
		r.record.graph().forEachRoute([&n](const bp::Route&) { ++n; });
		return n;
	};
	REQUIRE(count() == 4);

	REQUIRE(r.record.remove(env));
	CHECK(r.record.routeAmount(envOut, filterIn) == 0.0f);
	CHECK(r.record.routeAmount(envOut, bp::Destination::core(3)) == 0.0f);
	CHECK(r.record.routeAmount(bp::Source::core(0), filterIn) == 0.25f); // not the envelope's
	CHECK(r.record.routeAmount(bp::Source::core(1), bp::Destination::core(3)) == -0.5f);
	CHECK(count() == 2);

	REQUIRE(r.record.remove(flt));
	CHECK(r.record.routeAmount(bp::Source::core(0), filterIn) == 0.0f);
	CHECK(count() == 1); // host to host names no device and stays
}

TEST_CASE("a route's amount is clamped to the declared reach, whoever sets it") {
	RouteRig r;
	REQUIRE(r.record.setRoute(RouteRig::macro(0), r.filterIn(), 9.0f));
	CHECK(r.record.routeAmount(RouteRig::macro(0), r.filterIn()) == device::kMaxAmount);
	REQUIRE(r.record.setRoute(r.envOut(), r.filterIn(), -9.0f));
	CHECK(r.record.routeAmount(r.envOut(), r.filterIn()) == -device::kMaxAmount);
}

//------------------------------------------------------------------------------------------------//
// PUBLISHED                                                                                      //
//------------------------------------------------------------------------------------------------//

TEST_CASE("what a record publishes is what an adopt hands back, and a shape mismatch is refused") {
	Rig r;
	r.record.insert(kStrip, 0, kEnv);
	r.record.insert(kLane, 0, kFilter);
	r.record.insertRow(kLane, 0, rowFor("zzzz", 90, { 3.0f }));

	const size_t capacity = r.record.state().capacity();
	rack::PublishedChainState pub(capacity, r.record.chainCount());
	CHECK(pub.sequence() == 0);
	REQUIRE(pub.publish(r.record.state()));
	CHECK(pub.sequence() == 2);

	ChainState got(capacity, r.record.chainCount());
	ChainState staging(capacity, r.record.chainCount());
	uint32_t sequence = 0;
	REQUIRE(pub.adopt(&got, &staging, &sequence));
	CHECK(sequence == 2);
	CHECK(got == r.record.state());

	ChainState wrong(3, 2);
	ChainState wrongStaging(3, 2);
	CHECK_FALSE(pub.adopt(&wrong, &wrongStaging, &sequence));
	CHECK_FALSE(pub.publish(wrong));
}

TEST_CASE("an adopt racing a publisher always leaves one whole state — the refused ones included") {
	// A WRITER ALTERNATES TWO STATES THAT DIFFER AT EVERY POSITION — different tags, ids, fields and order;
	// the reader must only ever hold one or the other. A torn copy here is a device under another
	// device's id, which the converger would build — or a running device the state seems not to want,
	// which the converger removes and nothing rebuilds.
	//
	// **THE REFUSED ONES ARE THE HALF THIS CASE MISSED.** It was "a snapshot racing a publisher is always
	// one whole state" and it inspected only the snapshots that SUCCEEDED — `continue` on a false. But a
	// snapshot writes as it reads, and both of its callers ran the state after a false: the rack's drain
	// and Feathers' lane. The churn case caught it one run in twenty under the sanitizer. So the state is
	// checked after EVERY call, and the case requires that refusals happened, or it did not test them.
	constexpr size_t kDevices = 8;
	ChainState a(kDevices, 1);
	ChainState b(kDevices, 1);
	for (size_t i = 0; i < kDevices; ++i) {
		// DIFFERENT TAGS, IDS AND FIELDS — and `b`'s ids run the other way, so a copy that took one
		// state's rows in the other's order is neither, which is what `torn` counts.
		REQUIRE(a.append(0, rowFor(kFilter, static_cast<DeviceInstanceId>(100 + i), { 0.25f })));
		REQUIRE(b.append(0, rowFor(kOwltt, static_cast<DeviceInstanceId>(200 + (kDevices - 1 - i)), { 0.75f })));
	}
	rack::PublishedChainState pub(kDevices, 1);
	REQUIRE(pub.publish(a));
	ChainState got(kDevices, 1);
	ChainState staging(kDevices, 1);
	uint32_t sequence = 0;
	REQUIRE(pub.adopt(&got, &staging, &sequence)); // a whole state to start from, before the writer runs
	REQUIRE(got == a);

	std::atomic<bool> stop { false };
	std::thread writer([&] {
		for (size_t i = 0; !stop.load(std::memory_order_relaxed); ++i) {
			pub.publish((i & 1) != 0 ? b : a);
			std::this_thread::yield();
		}
	});
	size_t taken = 0;
	size_t refused = 0;
	size_t torn = 0;
	for (size_t attempt = 0; attempt < 20000000 && (taken < 5000 || refused < 100); ++attempt) {
		const bool whole = pub.adopt(&got, &staging, &sequence);
		++(whole ? taken : refused);
		torn += (got == a || got == b) ? 0 : 1; // AFTER EVERY CALL, the refused ones too
	}
	stop.store(true, std::memory_order_relaxed);
	writer.join();
	CHECK(torn == 0);
	CHECK(taken > 0);
	REQUIRE(refused > 0); // THE HALF UNDER TEST RAN
}

TEST_CASE("record, publish, snapshot, converge: the engine runs what the record says") {
	// THE WHOLE PROTOCOL ON THE RACK'S SHAPE — one chain over every lane, anything runs anywhere.
	constexpr backplane::Axes kAxes {
		.coreSources = 8, .coreDestinations = 0, .deviceSlots = 4, .outputStride = 4,
	};
	constexpr ChainCapacity kOne[] = { 4 };
	backplane::InstanceCounter ids;
	ChainRecord record { kAxes, kOne, hostCatalogue(), ids, &backplane::permitAll };
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	rack::Converger converger { engine, &rack::Converger::fitsAnywhere };
	rack::PublishedChainState pub(kAxes.deviceSlots, 1);
	ChainState drained(kAxes.deviceSlots, 1);
	ChainState staging(kAxes.deviceSlots, 1);

	const auto round = [&] {
		pub.publish(record.state());                                   // the message thread publishes
		converger.provision(record.state(), nullptr, pub.sequence()); // and builds for that publish
		uint32_t sequence = 0;
		REQUIRE(pub.adopt(&drained, &staging, &sequence)); // the drain takes it whole
		converger.converge(drained, sequence);
		converger.reap();
	};

	// BY ID — the engine keeps each device under the record's id, in a slot of its own (kill-lanes 3a).
	const auto idAt = [&engine](size_t position) {
		return engine.slotAtLane(engine.deviceAtPosition(position)).id;
	};
	const DeviceInstanceId filter = record.insert(0, 0, kFilter);
	const DeviceInstanceId owltt = record.insert(0, 0, kOwltt);
	round();
	CHECK(engine.chainLength() == 2);
	CHECK(idAt(0) == owltt);
	CHECK(idAt(1) == filter);
	const void* filterStorage = engine.slotAtLane(engine.slotOf(filter)).storage.get();

	REQUIRE(record.move(0, 0, 1));
	REQUIRE(record.remove(owltt));
	round();
	CHECK(engine.chainLength() == 1);
	CHECK(idAt(0) == filter);
	CHECK(engine.slotAtLane(engine.slotOf(filter)).storage.get() == filterStorage);
}

TEST_CASE("backlog OJ: a record holds every route a hand makes, 512 of them, and its edge store grows a logarithmic number of times") {
	// THE CAP (backlog EA's `setRouteCap`, then `fullFor`) IS GONE: no route is refused for room. A record whose host
	// has 128 sources routes each of them to four controls of one filter — 512 new pairs, every one written, every
	// amount read back by value — and the edge store reallocated at most ceil(log1.5(512 / 24)) + 1 times.
	constexpr backplane::Axes kWide {
		.coreSources = 128, .coreDestinations = 0, .deviceSlots = 4, .outputStride = 4,
	};
	backplane::InstanceCounter ids;
	ChainRecord record { kWide, kWhole, hostCatalogue(), ids, &backplane::permitAll };
	const DeviceInstanceId filter = record.insert(0, 0, kFilter);
	const device::Descriptor& desc = *deviceForTag(kFilter);
	REQUIRE(desc.modulatableCount() >= 4);
	const size_t start = record.graph().edgeCapacity();
	REQUIRE(start < 512); // the store starts below the patch, so the patch must grow it
	const auto amountOf = [](size_t source, size_t n) { return 0.001f * static_cast<float>(1 + source * 4 + n); };
	for (size_t source = 0; source < 128; ++source) {
		for (size_t n = 0; n < 4; ++n) {
			const backplane::Destination d = backplane::Destination::control(filter, desc.controlOfNthModulatable(n));
			CHECK(record.mayRoute(backplane::Source::core(source), d));
			REQUIRE(record.setRoute(backplane::Source::core(source), d, amountOf(source, n)));
		}
	}
	size_t routes = 0;
	record.graph().forEachRoute([&](const backplane::Route&) { ++routes; });
	CHECK(routes == 512);
	for (size_t source = 0; source < 128; ++source) {
		for (size_t n = 0; n < 4; ++n) {
			const backplane::Destination d = backplane::Destination::control(filter, desc.controlOfNthModulatable(n));
			CHECK(record.routeAmount(backplane::Source::core(source), d) == amountOf(source, n));
		}
	}
	const auto bound = static_cast<size_t>(std::ceil(std::log(512.0 / static_cast<double>(start)) / std::log(1.5))) + 1;
	CAPTURE(start);
	CHECK(record.graph().edgeGrowthsForTest() >= 1);
	CHECK(record.graph().edgeGrowthsForTest() <= bound);
}

TEST_CASE("unbounded devices step 6: a chain at its capacity grows for the next device, past the record's whole pool") {
	// IT WAS REFUSED until unbounded devices: the strip held four and the fifth went nowhere. Now the record grows —
	// past its chain's capacity, and past the pool the record was built with — and every device it held keeps its
	// id, its chain and its place. Asked of the record's own consistency check as well as of the order.
	Rig r;
	REQUIRE(r.record.capacity(kStrip) == 4);
	std::vector<DeviceInstanceId> strip;
	for (int i = 0; i < 40; ++i) {
		CAPTURE(i);
		const DeviceInstanceId id = r.record.insert(kStrip, 99, kEnv);
		REQUIRE(id != kNoInstance);
		strip.push_back(id);
	}
	CHECK(r.record.capacity(kStrip) >= 40);
	CHECK(r.record.length(kStrip) == 40);
	CHECK(orderOf(r.record, kStrip) == strip);
	CHECK(r.record.consistentForTest());
}
