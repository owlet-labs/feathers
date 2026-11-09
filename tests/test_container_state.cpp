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

#include "composite/composite_types.h"
#include "backplane/container_bodies.h"
#include "plugin/audition.h"
#include "plugin/device_registry.h"
#include "plugin/feathers_chains.h"
#include "plugin/fx_chain.h"
#include "plugin/gui_bridge.h"
#include "plugin/lane_rack.h"
#include "plugin/mod_rack.h"
#include "rack/rack_axes.h"
#include "rack/rack_device.h"
#include "rack/rack_engine.h"
#include "rack/rack_state.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

// A RACK'S CONTENTS ON FEATHERS' REAL SAVE, LOAD AND PROVISION ROAD.
//
// A rack in the fx lane is a device whose state is the rack's own file — `saveRack`'s bytes. These
// cases drive the bridge's `chainsPatch`/`restoreChains`, the chains block's `saveChainsBlock`/
// `loadChainsBlock`, and `LaneRack::provision` — the functions the plugin calls — with exactly those
// bytes, so a rack in the lane is saved, reloaded and rebuilt the way a patch does.

using namespace feathers;

namespace {

/// A clap stream pair over a byte vector, with a chunk size, so a partial read or write — which a
/// host may deliver — is a case the serialiser meets.
struct MemoryStream {
	std::vector<uint8_t> bytes;
	size_t readPos { 0 };
	size_t chunk { SIZE_MAX };

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
		std::memcpy(buffer, self->data() + self->readPos, n);
		self->readPos += n;
		return static_cast<int64_t>(n);
	}
	const uint8_t* data() const { return bytes.data(); }
	clap_ostream out() { return clap_ostream { this, writeFn }; }
	clap_istream in() {
		readPos = 0;
		return clap_istream { this, readFn };
	}
};

/// **A RACK'S BODY, AS THE RACK ITSELF WRITES IT** — a real engine holding `tags`, saved with
/// `saveRack`. Not hand-built: the point of these cases is that the lane carries the rack's own file.
std::vector<uint8_t> rackBody(const std::vector<std::string>& tags) {
	rack::RackEngine engine { rack::kRackAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	for (const std::string& t : tags) {
		REQUIRE(engine.addDevice(t.c_str()) != rack::RackEngine::kNoSlot);
	}
	MemoryStream sink;
	clap_ostream out = sink.out();
	// COMPACT, AS A RACK DEVICE WRITES ITS OWN BODY (`rack_device.cpp`, backlog JF): the parent splices it
	// in as a document and hands its compact text back, so a body comes back byte for byte.
	REQUIRE(rack::saveRack(&out, engine, false));
	return sink.bytes;
}

/// THE TAGS A RACK HOLDS, IN CHAIN ORDER — read off the engine rather than trusted from a count.
std::vector<std::string> tagsIn(const rack::RackEngine& engine) {
	std::vector<std::string> out;
	for (size_t pos = 0; pos < engine.chainLength(0); ++pos) {
		const size_t lane = engine.deviceAtPosition(pos);
		if (lane == rack::RackEngine::kNoSlot) {
			continue;
		}
		const device::Descriptor* d = engine.slotAtLane(lane).live_desc();
		out.emplace_back(d != nullptr ? std::string(d->traits.tag) : std::string("????"));
	}
	return out;
}

/// A LANE HOLDING ONE RACK, and the id its row was given — the key its body travels under.
struct OneRack {
	FxChain chain {};
	/// WHERE THE RACK STANDS IN THE CHAIN. `addFxDevice` answers with the POSITION and there is no second
	/// number to reconcile it against (kill-lanes step 3c took the lane pool away), so this is the same
	/// thing the state and every reader below speak.
	size_t slot = kMaxFxSlots;
	DeviceInstanceId id = kNoInstance;

	OneRack() {
		chain.nextInstanceId = kFirstFreeInstance;
		slot = addFxDevice(&chain, composite::kTraits.tag);
		REQUIRE(slot < kMaxFxSlots);
		id = fxDeviceAtPosition(chain, slot).row.instanceId;
		REQUIRE(id != kNoInstance);
	}
};

/// **THE FILTER'S CUTOFF AND A VALUE INSIDE ITS DECLARED RANGE** — from the declaration, not chosen here:
/// a load coerces every base by its control (backlog DV), so a value outside the contract would read back
/// as something else and the case would be about the coercion. (These wrote 0.3141 into control 1, the
/// filter's Type — a choice — which a load now rounds to 0.)
constexpr size_t kCutoff = static_cast<size_t>(fx::filter::Param::Cutoff);
float cutoffAt(double t) {
	const auto& p = fx::filter::kDescriptor.params[kCutoff];
	REQUIRE_FALSE(p.stepped);
	return static_cast<float>(p.min + t * (p.max - p.min));
}

bool sameBytes(std::span<const uint8_t> got, const std::vector<uint8_t>& want) {
	return got.size() == want.size() && std::equal(want.begin(), want.end(), got.begin());
}

/// **THE LANE, WRITTEN AS THE PLUGIN WRITES IT** — the chain and its bodies into a bridge's record, out
/// as the chains block. `chunk` is the stream's delivery size.
MemoryStream savedLane(const FxChain& chain, const ContainerBodies& bodies, size_t chunk = SIZE_MAX) {
	ParamBridge writer;
	writer.restoreFxBodies(bodies);
	writer.restoreFxChain(chain);
	MemoryStream stream;
	stream.chunk = chunk;
	clap_ostream out = stream.out();
	REQUIRE(saveChainsBlock(&out, writer.chainsPatch()));
	return stream;
}

/// AND READ BACK INTO A FRESH BRIDGE — false when the block refuses.
bool loadLane(MemoryStream& stream, ParamBridge* reader) {
	clap_istream in = stream.in();
	rack::RackPatch patch;
	bool present = false;
	if (!loadChainsBlock(&in, &patch, &present)) {
		return false;
	}
	REQUIRE(present);
	CHECK(reader->restoreChains(patch).routes.count == 0);
	return true;
}

/// FEATHERS' LANE — the rack engine it runs on — and the rack device inside it.
struct Runner {
	LaneRack lane;

	Runner() { lane.prepare(48000.0, 0, 4096); }
	/// THE RACK STANDING AT `position` OF THE FX CHAIN, AS THE AUDIO THREAD HOLDS IT — after `sync`, which
	/// adopts what provisioning posted. A place in a chain is `(chain, position)` since kill-lanes step 3b,
	/// and `storageOfForTest` takes the DEVICE the state names there, never a number.
	rack::RackEngine& rackAt(const FxChain& chain, size_t position) {
		lane.sync(chain, device::NoteContext {}, device::TransportClock::unknown());
		lane.finish();
		const DeviceInstanceId id = lane.deviceOfStatePosition(kLaneChain, position);
		REQUIRE(id != kNoInstance);
		void* storage = const_cast<void*>(lane.storageOfForTest(id));
		REQUIRE(storage != nullptr);
		return rack::engineForTest(storage);
	}
};

} // namespace

TEST_CASE("a rack's contents survive a save and a load of the lane, byte for byte, keyed by its id") {
	OneRack lane;
	const auto body = rackBody({ "flt1", "OWLTT" });
	ContainerBodies bodies;
	REQUIRE(bodies.store(lane.id, body.data(), body.size()));

	for (size_t chunk : { SIZE_MAX, size_t { 3 }, size_t { 1 } }) {
		CAPTURE(chunk);
		MemoryStream stream = savedLane(lane.chain, bodies, chunk);
		ParamBridge reader;
		REQUIRE(loadLane(stream, &reader));
		const FxChain read = reader.fxChain();
		REQUIRE(read.count == lane.chain.count);
		CHECK(fxDeviceAtPosition(read, lane.slot).row.instanceId == lane.id);
		CHECK(fxDeviceAtPosition(read, lane.slot).tag() == fxDeviceAtPosition(lane.chain, lane.slot).tag());
		REQUIRE(reader.fxBodies().has(lane.id));
		CHECK(sameBytes(reader.fxBodies().bodyFor(lane.id), body));
	}
}

TEST_CASE("a device after a rack in the lane still reads its own settings — the body shifted nothing") {
	OneRack lane;
	const size_t filterSlot = addFxDevice(&lane.chain, fx::filter::kTraits.tag);
	REQUIRE(filterSlot < kMaxFxSlots);
	const float cutoff = cutoffAt(0.3141);
	lane.chain.device[filterSlot].row.field[kCutoff] = cutoff;

	const auto body = rackBody({ "HARM" });
	ContainerBodies bodies;
	REQUIRE(bodies.store(lane.id, body.data(), body.size()));

	MemoryStream stream = savedLane(lane.chain, bodies);
	ParamBridge reader;
	REQUIRE(loadLane(stream, &reader));
	const FxChain read = reader.fxChain();
	REQUIRE(read.count == 2);
	CHECK(fxDeviceAtPosition(read, filterSlot).row.field[kCutoff] == doctest::Approx(cutoff));
	CHECK(sameBytes(reader.fxBodies().bodyFor(lane.id), body));
}

TEST_CASE("an applier with nowhere to put a body drops it and still places everything after it") {
	// `applyPatchToRecord` with no `bodies` — a host that holds no containers. The body is parsed (the
	// format length-prefixes it, so the walk is right either way) and dropped; the devices after it land.
	OneRack lane;
	const size_t filterSlot = addFxDevice(&lane.chain, fx::filter::kTraits.tag);
	REQUIRE(filterSlot < kMaxFxSlots);
	const float cutoff = cutoffAt(0.2718);
	lane.chain.device[filterSlot].row.field[kCutoff] = cutoff;
	const auto body = rackBody({ "flt1", "OWLTT" });
	ContainerBodies bodies;
	REQUIRE(bodies.store(lane.id, body.data(), body.size()));

	MemoryStream stream = savedLane(lane.chain, bodies);
	clap_istream in = stream.in();
	rack::RackPatch patch;
	bool present = false;
	REQUIRE(loadChainsBlock(&in, &patch, &present));
	backplane::InstanceCounter ids;
	rack::ChainRecord record { feathersAxes(), kFeathersChainCapacities, hostCatalogue(), ids, &backplane::permitAll };
	CHECK(rack::applyPatchToRecord(patch, &record, nullptr, &ids, feathersCoreCodec()).routes.count == 0);
	REQUIRE(record.length(kLaneChain) == 2);
	const DeviceInstanceId filter = record.deviceAt(kLaneChain, filterSlot);
	const device::Descriptor* d = record.descriptorOf(filter);
	REQUIRE(d != nullptr);
	REQUIRE(device::sameTag(d->traits.tag, fx::filter::kTraits.tag));
	CHECK(record.rowOf(filter).field[kCutoff] == doctest::Approx(cutoff));
}

TEST_CASE("a FULL rack's body still loads") {
	// **A FULL RACK'S BODY IS THE LARGEST A LANE CAN SAVE** — sixteen devices' records inside one row's
	// body. The format's ceiling (`rack::kMaxBodyBytes`) has to hold it, or a legitimately full rack is
	// refused as corrupt and lost on reload.
	OneRack lane;
	std::vector<std::string> sixteen(rack::kRackAxes.deviceSlots, std::string(fx::drawn::kTraits.tag, 4));
	const auto body = rackBody(sixteen);
	REQUIRE(body.size() <= rack::kMaxBodyBytes);

	ContainerBodies bodies;
	REQUIRE(bodies.store(lane.id, body.data(), body.size()));
	MemoryStream stream = savedLane(lane.chain, bodies);
	ParamBridge reader;
	REQUIRE(loadLane(stream, &reader));
	REQUIRE(reader.fxBodies().has(lane.id));
	CHECK(sameBytes(reader.fxBodies().bodyFor(lane.id), body));
}

TEST_CASE("a body longer than the format's ceiling is refused as a bad file, not read") {
	OneRack lane;
	std::vector<uint8_t> tooLong(rack::kMaxBodyBytes + 1, 0x5A);
	ContainerBodies bodies;
	REQUIRE(bodies.store(lane.id, tooLong.data(), tooLong.size()));
	MemoryStream stream = savedLane(lane.chain, bodies);

	ParamBridge reader;
	CHECK_FALSE(loadLane(stream, &reader));
	CHECK_FALSE(reader.fxBodies().has(lane.id));
}

TEST_CASE("provisioning hands a rack in the lane its contents before it reaches the audio thread") {
	OneRack lane;
	const auto body = rackBody({ "flt1", "OWLTT", "HARM" });
	ContainerBodies bodies;
	REQUIRE(bodies.store(lane.id, body.data(), body.size()));

	Runner r;
	CHECK(r.lane.provision(lane.chain, &bodies) == 1);
	// **THE MEMBERS, BY TAG AND ORDER** — read off the rack engine inside the lane's device.
	CHECK(tagsIn(r.rackAt(lane.chain, lane.slot)) == std::vector<std::string> { "flt1", "OWLTT", "HARM" });
}

TEST_CASE("a NEW VERSION of a rack's contents rebuilds it, even though its id did not change") {
	// THE ABA CASE: a second patch whose rack carries the same id passes the runner's descriptor, tag
	// and id checks; only the body's generation can see the change.
	OneRack lane;
	Runner r;
	ContainerBodies first;
	const auto a = rackBody({ "flt1" });
	REQUIRE(first.store(lane.id, a.data(), a.size()));
	REQUIRE(r.lane.provision(lane.chain, &first) == 1);
	REQUIRE(tagsIn(r.rackAt(lane.chain, lane.slot)) == std::vector<std::string> { "flt1" });

	CHECK(r.lane.provision(lane.chain, &first) == 0); // the same version builds nothing

	ContainerBodies reloaded;
	const auto b = rackBody({ "OWLTT", "HARM" });
	REQUIRE(reloaded.store(lane.id, b.data(), b.size()));
	CHECK(r.lane.provision(lane.chain, &reloaded) == 1);
	CHECK(tagsIn(r.rackAt(lane.chain, lane.slot)) == std::vector<std::string> { "OWLTT", "HARM" });
}

TEST_CASE("a rack with no body is provisioned EMPTY, which is what a rack just added is") {
	OneRack lane;
	ContainerBodies bodies;
	Runner r;
	REQUIRE(r.lane.provision(lane.chain, &bodies) == 1);
	CHECK(r.rackAt(lane.chain, lane.slot).chainLength(0) == 0);
}

TEST_CASE("a rack the lane parked is destroyed with the lane, and its replacement holds the new contents") {
	// A LOAD OVER A RACK WITH THE SAME ID: the lane parks the old rack and adopts the new one in one
	// drain. The old one — a whole engine of devices — is still parked when the lane is destroyed, and
	// dies there; under the sanitizer gate a use-after-free on that road is a finding.
	OneRack one;
	{
		Runner r;
		ContainerBodies first;
		const auto a = rackBody({ "flt1", "OWLTT" });
		REQUIRE(first.store(one.id, a.data(), a.size()));
		REQUIRE(r.lane.provision(one.chain, &first) == 1);
		REQUIRE(tagsIn(r.rackAt(one.chain, one.slot)) == std::vector<std::string> { "flt1", "OWLTT" });
		ContainerBodies second;
		const auto b = rackBody({ "HARM" });
		REQUIRE(second.store(one.id, b.data(), b.size()));
		REQUIRE(r.lane.provision(one.chain, &second) == 1);
		// THE NEW CONTENTS ARE TAKEN THOUGH THE DEVICE IS "THE SAME" — same kind, tag and id.
		CHECK(tagsIn(r.rackAt(one.chain, one.slot)) == std::vector<std::string> { "HARM" });
		// AND THE OLD RACK WENT TO THE PARKING, not to a destructor on the audio thread.
		CHECK(r.lane.engineForTest().retiredCountForTest() > 0);
	}
}

TEST_CASE("an internal destination is spelled on disk by its own stable id, and an old build would refuse it") {
	// **THE CORE'S INTERNAL DESTINATIONS HAVE NO PARAMETER ID TO BE SAVED UNDER** (they are internal params
	// precisely so as not to spend CLAP params), so the codec gives them a kind of their
	// own and a STABLE id — not the table position, which is `kParamDestinationCount + n` and moves
	// every time a parameter destination is added ahead of it.
	const feathers::rack::CoreCodec codec = feathers::feathersCoreCodec();

	for (size_t slot : { size_t { 0 }, size_t { 5 }, feathers::kMaxSounds - 1 }) {
		for (const feathers::TravelAxis axis : { feathers::TravelAxis::Radius, feathers::TravelAxis::Theta }) {
			CAPTURE(slot);
			CAPTURE(static_cast<int>(axis));
			const size_t destination = feathers::travelDestinationIndex(slot, axis);
			uint32_t kind = 99;
			uint32_t a = 99;
			REQUIRE(codec.nameDestination(codec.host, destination, &kind, &a));
			CHECK(kind == static_cast<uint32_t>(feathers::rack::DestKind::Internal));
			// THE ID, PINNED — this is the promise a saved patch relies on.
			CHECK(a == 0x0100u + static_cast<uint32_t>(slot) * 0x10u + static_cast<uint32_t>(axis));
			size_t back = 0;
			REQUIRE(codec.placeDestination(codec.host, kind, a, &back));
			CHECK(back == destination);
		}
	}

	// A PARAMETER DESTINATION IS SPELLED EXACTLY AS IT WAS — the new kind displaced nothing.
	uint32_t kind = 99;
	uint32_t a = 99;
	REQUIRE(codec.nameDestination(codec.host, 0, &kind, &a));
	CHECK(kind == static_cast<uint32_t>(feathers::rack::DestKind::Param));
	CHECK(a == static_cast<uint32_t>(feathers::kModDestinations[0]));

	// WHAT IS REFUSED, which is the "no state version bump" argument made checkable: an id this build
	// does not have places nowhere, and an Internal coordinate read as a Param places nowhere either —
	// so a build from BEFORE this kind existed (whose `placeDestination` refuses every kind but Param)
	// drops such a route into its refusals rather than landing it on some other knob.
	size_t nowhere = 0;
	CHECK_FALSE(codec.placeDestination(codec.host, static_cast<uint32_t>(feathers::rack::DestKind::Internal),
	                                   0x0180u, &nowhere));
	CHECK_FALSE(codec.placeDestination(codec.host, static_cast<uint32_t>(feathers::rack::DestKind::Internal),
	                                   0x0102u, &nowhere));
	// 0x0170 (H's radius): since backlog PQ the internal ids of slots 0..5 (0x0100..0x0151) COINCIDE with C..H's
	// Zone/Offset, which are now edit-only Param destinations — harmless while every route names its kind, but the
	// defence this line makes holds only for an id that does not collide, so it is asked of one that does not.
	CHECK_FALSE(codec.placeDestination(codec.host, static_cast<uint32_t>(feathers::rack::DestKind::Param),
	                                   0x0170u, &nowhere));
	// PAST THE WHOLE TABLE NAMES NOTHING.
	CHECK_FALSE(codec.nameDestination(codec.host, feathers::kCoreDestinationCount, &kind, &a));
}
