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

#include "backplane/axes.h"
#include "backplane/block_context.h"
#include "composite/composite_types.h"
#include "backplane/container_bodies.h"
#include "plugin/device_registry.h"
#include "plugin/fx_chain.h"
#include "plugin/editor_lanes.h"
#include "plugin/gui_bridge.h"
#include "plugin/lane_rack.h"
#include "plugin/synth.h"
#include "mod/dahdsr/dahdsr_types.h"
#include "fx/busutil/busutil_types.h"
#include "fx/delay/delay_types.h"
#include "device/format_value.h"
#include "hostslots/slot_bus.h"
#include "hostslots/slot_pool.h"
#include "fx/filter/filter_types.h"
#include "fx/harm/harm_types.h"
#include "fx/sklitter/sklitter.h"
#include "fx/sklitter/sklitter_types.h"
#include "rack/inner_chain.h"
#include "rack/parallel_rack.h"
#include "mod/lfo/lfo_types.h"
#include "mod/stepseq/stepseq_types.h"
#include "rack/chain_record.h"
#include "rack/rack_axes.h"
#include "rack/rack_bridge.h"
#include "rack/rack_device.h"
#include "rack/rack_engine.h"
#include "rack_endpoints.h"
#include "spectrum.h"
#include "rack/rack_state.h"
#include "rack/site_chains.h"
#include "mod/macros/macros_types.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// A RACK INSIDE A RACK — AND IT IS NOT A FEATURE, IT IS THE RACK.
//
// THE DESIGN: the lane, the standalone effect and the chain inside the chain are one piece of code,
// so the standalone plugin works without any separate "chain in chain" concept. Twelve earlier commits built a
// separate imitation chain instead, and not one of their tests ever put a chain inside a chain. These
// cases do nothing else.
//
// EVERY RACK HERE IS THE SAME `RackEngine` THE STANDALONE PLUGIN RUNS. A rack in a slot is that engine
// as a device (`rack::kDeviceDescriptor`), built by the outer rack the way it builds any device.

using namespace feathers;
using rack::RackEngine;

namespace {

const std::string kRack(composite::kTraits.tag);

/// THE RACK INSIDE THE DEVICE AT `lane` OF `outer`, or null if that device is not a rack.
RackEngine* rackIn(RackEngine& outer, size_t lane) {
	const rack::Slot& slot = outer.slotAtLane(lane);
	const device::Descriptor* d = slot.live_desc();
	if (d == nullptr || !device::sameTag(d->traits.tag, composite::kTraits.tag) || slot.storage == nullptr) {
		return nullptr;
	}
	return &rack::engineForTest(slot.storage.get());
}

std::vector<std::string> tagsIn(const RackEngine& engine) {
	std::vector<std::string> out;
	for (size_t pos = 0; pos < engine.chainLength(0); ++pos) {
		const size_t lane = engine.deviceAtPosition(pos);
		const device::Descriptor* d = lane == RackEngine::kNoSlot ? nullptr : engine.slotAtLane(lane).live_desc();
		out.emplace_back(d != nullptr ? std::string(d->traits.tag) : std::string("????"));
	}
	return out;
}

/// **THE CHAIN'S OWN BRIDGE — the editing road into the rack at `lane`** (rack roadmap §6, Stage B). A chain
/// device carries a `RackBridge` over its engine, so its RECORD is the truth and the engine is the bridge's
/// to reconcile at every drain: a device added to the inner engine directly is removed at the next block and
/// never saved — the standalone plugin's semantics, arrived inside a device. These cases add through it.
rack::RackBridge& bridgeIn(RackEngine& outer, size_t lane) {
	rack::RackBridge* bridge = rack::bridgeOf(outer.slotAtLane(lane).storage.get());
	REQUIRE(bridge != nullptr);
	return *bridge;
}

/// Add a device inside the chain at `lane` through its bridge and drain it there (nothing else is running
/// the inner engine in these cases; a block of the outer would do the same), so the inner engine holds it.
DeviceInstanceId addInside(RackEngine& outer, size_t lane, const char* tag) {
	rack::RackBridge& bridge = bridgeIn(outer, lane);
	const DeviceInstanceId id = bridge.addDevice(tag);
	bridge.drain();
	bridge.publish(*rackIn(outer, lane));
	return id;
}

/// The same, answering the inner engine's slot for the device — for a case that walks racks by lane.
size_t addInsideLane(RackEngine& outer, size_t lane, const char* tag) {
	const DeviceInstanceId id = addInside(outer, lane, tag);
	RackEngine* inner = rackIn(outer, lane);
	return inner != nullptr && id != kNoInstance ? inner->slotOf(id) : RackEngine::kNoSlot;
}

/// **A FILE NESTING `levels` CHAINS, `bottom` IN THE DEEPEST** — written as bytes, because since the depth gate
/// (2026-10-31) no hand can build past the limit, and a file is the one thing left that can be deeper than it.
std::vector<uint8_t> chainsNestedAsFile(size_t levels, const char* bottom) {
	rack::RackPatch inner;
	inner.present = true;
	rack::RackPatch::Device leaf;
	std::memcpy(leaf.tag.data(), bottom, std::min(std::strlen(bottom), device::kTagBytes));
	leaf.instanceId = 1;
	inner.device.push_back(leaf);
	std::string text;
	REQUIRE(rack::rackToText(inner, &text, false));
	for (size_t level = 0; level < levels; ++level) {
		rack::RackPatch outer;
		outer.present = true;
		rack::RackPatch::Device chain;
		std::memcpy(chain.tag.data(), kRack.c_str(), std::min(kRack.size(), device::kTagBytes));
		chain.instanceId = 1;
		chain.body.assign(text.begin(), text.end());
		outer.device.push_back(chain);
		REQUIRE(rack::rackToText(outer, &text, false));
	}
	return std::vector<uint8_t>(text.begin(), text.end());
}

size_t controlNamed(const device::Descriptor& desc, std::string_view name) {
	for (size_t i = 0; i < desc.params.size(); ++i) {
		if (name == desc.params[i].name) {
			return i;
		}
	}
	return desc.params.size();
}

struct ByteStream {
	std::vector<uint8_t> bytes;
	size_t at = 0;
	static int64_t CLAP_ABI write(const clap_ostream* s, const void* b, uint64_t n) {
		auto* self = static_cast<ByteStream*>(s->ctx);
		self->bytes.insert(self->bytes.end(), static_cast<const uint8_t*>(b),
		                   static_cast<const uint8_t*>(b) + n);
		return static_cast<int64_t>(n);
	}
	static int64_t CLAP_ABI read(const clap_istream* s, void* b, uint64_t n) {
		auto* self = static_cast<ByteStream*>(s->ctx);
		const size_t k = std::min<size_t>(static_cast<size_t>(n), self->bytes.size() - self->at);
		std::memcpy(b, self->bytes.data() + self->at, k);
		self->at += k;
		return static_cast<int64_t>(k);
	}
	clap_ostream out() { return { this, &write }; }
	clap_istream in() {
		at = 0;
		return { this, &read };
	}
};

std::vector<float> renderConstant(RackEngine& engine, float value, size_t frames) {
	std::vector<float> l(frames, value);
	std::vector<float> r(frames, value);
	engine.processStereo(l.data(), r.data(), static_cast<int>(frames), backplane::unknownBlock());
	return l;
}

} // namespace

TEST_CASE("a rack holds a rack, and the rack inside can hold devices of its own") {
	RackEngine outer { rack::kRackAxes, hostCatalogue() };
	outer.prepare(48000.0, 4096);
	const size_t lane = outer.addDevice(kRack.c_str());
	REQUIRE(lane != RackEngine::kNoSlot);

	RackEngine* inner = rackIn(outer, lane);
	REQUIRE(inner != nullptr);
	// **THE INNER RACK WAS HANDED A CATALOGUE WHEN ITS HOST BUILT IT** — without one it could build
	// nothing, and this add is what would fail.
	REQUIRE(addInside(outer, lane, "OWLTT") != kNoInstance);
	REQUIRE(addInside(outer, lane, "flt1") != kNoInstance);
	CHECK(tagsIn(*inner) == std::vector<std::string> { "OWLTT", "flt1" });
}

TEST_CASE("a rack inside a rack RENDERS: the signal goes through the inner rack's device") {
	// **A VALUE, NOT "IT BUILT".** The inner rack holds Bus Utility with its fader at the top, which is
	// exactly x4 — (2 * 1.0)^2, restated. 0.25 comes out as 1.0 only if the signal really passes
	// through both racks and the device inside.
	RackEngine outer { rack::kRackAxes, hostCatalogue() };
	outer.prepare(48000.0, 4096);
	const size_t lane = outer.addDevice(kRack.c_str());
	RackEngine* inner = rackIn(outer, lane);
	REQUIRE(inner != nullptr);
	const DeviceInstanceId bus = addInside(outer, lane, "busu");
	REQUIRE(bus != kNoInstance);
	const device::Descriptor* bu = deviceForTag("busu");
	REQUIRE(bu != nullptr);
	const size_t gain = controlNamed(*bu, "Bus Gain");
	REQUIRE(gain < bu->params.size());
	bridgeIn(outer, lane).setField(bus, gain, 1.0f);
	bridgeIn(outer, lane).drain(); // the field reaches the engine at its drain, as an edit does

	// Every ramp at every level arrives well inside this.
	const std::vector<float> out = renderConstant(outer, 0.25f, 4096);
	CHECK(out.back() == doctest::Approx(1.0f));
}

TEST_CASE("a rack inside a rack survives its outer rack's save and reload") {
	RackEngine outer { rack::kRackAxes, hostCatalogue() };
	outer.prepare(48000.0, 4096);
	outer.addDevice("flt1");
	const size_t lane = outer.addDevice(kRack.c_str());
	RackEngine* inner = rackIn(outer, lane);
	REQUIRE(inner != nullptr);
	REQUIRE(addInside(outer, lane, "HARM") != kNoInstance);
	REQUIRE(addInside(outer, lane, "OWLTT") != kNoInstance);

	ByteStream file;
	clap_ostream out = file.out();
	REQUIRE(rack::saveRack(&out, outer));

	RackEngine loaded { rack::kRackAxes, hostCatalogue() };
	loaded.prepare(48000.0, 4096);
	rack::LoadReport refused;
	clap_istream in = file.in();
	REQUIRE(rack::loadRack(&in, &loaded, &refused));
	REQUIRE(tagsIn(loaded) == std::vector<std::string> { "flt1", kRack });
	RackEngine* innerLoaded = rackIn(loaded, loaded.deviceAtPosition(1));
	REQUIRE(innerLoaded != nullptr);
	CHECK(tagsIn(*innerLoaded) == std::vector<std::string> { "HARM", "OWLTT" });
}

TEST_CASE("racks nest as deep as a hand builds them, and survive a reload at every level") {
	constexpr size_t kLevels = 6;
	RackEngine top { rack::kRackAxes, hostCatalogue() };
	top.prepare(48000.0, 4096);
	// THE TOP IS A PLAIN ENGINE and takes a device directly; every rack below it is a chain device with a
	// bridge, and takes one through it.
	RackEngine* parent = nullptr;
	RackEngine* at = &top;
	size_t lane = RackEngine::kNoSlot;
	for (size_t level = 0; level < kLevels; ++level) {
		lane = parent == nullptr ? at->addDevice(kRack.c_str()) : addInsideLane(*parent, lane, kRack.c_str());
		REQUIRE(lane != RackEngine::kNoSlot);
		parent = at;
		at = rackIn(*at, lane);
		REQUIRE(at != nullptr);
	}
	REQUIRE(addInside(*parent, lane, "OWLTT") != kNoInstance); // something at the bottom

	ByteStream file;
	clap_ostream out = file.out();
	REQUIRE(rack::saveRack(&out, top));
	RackEngine loaded { rack::kRackAxes, hostCatalogue() };
	loaded.prepare(48000.0, 4096);
	rack::LoadReport refused;
	clap_istream in = file.in();
	REQUIRE(rack::loadRack(&in, &loaded, &refused));

	RackEngine* walk = &loaded;
	for (size_t level = 0; level < kLevels; ++level) {
		CAPTURE(level);
		REQUIRE(walk->chainLength(0) == 1);
		walk = rackIn(*walk, walk->deviceAtPosition(0));
		REQUIRE(walk != nullptr);
	}
	CHECK(tagsIn(*walk) == std::vector<std::string> { "OWLTT" });
}

TEST_CASE("a file nesting racks past the load limit is refused at the limit rather than recursed into") {
	// **BYTES DO NOT BOUND DEPTH**, so a hostile or corrupt file could force recursion without end.
	// Written deeper than the limit as a FILE — a hand is stopped at the limit since the depth gate (2026-10-31).
	const size_t levels = rack::kMaxDepth + 2;
	ByteStream file;
	file.bytes = chainsNestedAsFile(levels, "OWLTT");

	RackEngine loaded { rack::kRackAxes, hostCatalogue() };
	loaded.prepare(48000.0, 4096);
	rack::LoadReport refused;
	clap_istream in = file.in();
	REQUIRE(rack::loadRack(&in, &loaded, &refused));

	size_t depth = 0;
	RackEngine* walk = &loaded;
	while (walk->chainLength(0) == 1) {
		RackEngine* next = rackIn(*walk, walk->deviceAtPosition(0));
		if (next == nullptr) {
			break;
		}
		walk = next;
		++depth;
	}
	// The loaded top is not itself a device load; every rack below it is, and the one that would have
	// been the `kMaxDepth`-th device load refused its body — so it is there, and empty.
	CHECK(depth == rack::kMaxDepth);
	CHECK(walk->chainLength(0) == 0);
}

TEST_CASE("the depth a hand may build to and the depth a file may be loaded from are ONE number, and the bytes are nowhere near the other ceiling") {
	// **NESTING IS BOUNDED AT RUNTIME: past about 20 deep, or as far as the plugin can no longer be
	// saved, the editor says so.** The counter once guarded only `loadState`, and nesting by hand had
	// no limit at all. The second
	// clause is the engineering requirement and this case is it: the runtime bound must be EXACTLY the
	// depth at which a load would refuse, so that a patch a hand can build is always a patch that
	// reopens. Two numbers that nearly agree is the same bug wearing a different hat.**
	//
	// `ChainColumn::nestingFull()` is the runtime half and tests `nest_.depth + 1 >= kMaxDepth`;
	// `InnerChain::loadState` and `rack_device.cpp`'s test `containerLoadDepth() + 1 >= kMaxDepth`.
	// The claim here is that those are the same quantity, asserted by BUILDING to the bound and
	// reloading rather than by reading the two expressions and agreeing they look alike.
	// **THE DISCRIMINATOR IS A DEVICE AT THE BOTTOM, AND THE FIRST DRAFT OF THIS CASE DID NOT HAVE
	// ONE — it went red and was RIGHT TO.** It walked the reloaded nesting and compared its DEPTH, and
	// a depth count cannot see this: the deepest container is listed in its PARENT's body, so it comes
	// back whether or not its own body was refused, and when it is the last level it has nothing inside
	// it to lose either way. The probe reported 16 == 15 and the guard it was accusing was correct.
	// **A count of levels is a difference check; what "this patch reopens" means is that the thing you
	// put at the bottom is still there.** So every build below leaves an OWLTT in its deepest chain and
	// asks after it by name.
	const size_t deepest = rack::kMaxUsefulDepth; // the deepest chain whose CONTENTS survive a reload

	const auto build = [](size_t levels) {
		auto top = std::make_unique<RackEngine>(rack::kRackAxes, hostCatalogue());
		top->prepare(48000.0, 4096);
		RackEngine* parent = nullptr;
		RackEngine* at = top.get();
		size_t lane = RackEngine::kNoSlot;
		for (size_t level = 0; level < levels; ++level) {
			lane = parent == nullptr ? at->addDevice(kRack.c_str())
			                         : addInsideLane(*parent, lane, kRack.c_str());
			REQUIRE(lane != RackEngine::kNoSlot);
			parent = at;
			at = rackIn(*at, lane);
			REQUIRE(at != nullptr);
		}
		REQUIRE(addInside(*parent, lane, "OWLTT") != kNoInstance); // the thing that must come back
		return top;
	};
	/// Save, reload, walk `levels` deep, and answer what is in the chain down there.
	const auto roundTrip = [](RackEngine& top, size_t levels) {
		ByteStream file;
		clap_ostream out = file.out();
		REQUIRE(rack::saveRack(&out, top));
		auto loaded = std::make_unique<RackEngine>(rack::kRackAxes, hostCatalogue());
		loaded->prepare(48000.0, 4096);
		rack::LoadReport refused;
		clap_istream in = file.in();
		REQUIRE(rack::loadRack(&in, loaded.get(), &refused));
		RackEngine* walk = loaded.get();
		for (size_t level = 0; level < levels; ++level) {
			if (walk->chainLength(0) == 0) {
				return std::pair { file.bytes.size(), std::vector<std::string> {} };
			}
			RackEngine* next = rackIn(*walk, walk->deviceAtPosition(0));
			if (next == nullptr) {
				return std::pair { file.bytes.size(), std::vector<std::string> {} };
			}
			walk = next;
		}
		return std::pair { file.bytes.size(), tagsIn(*walk) };
	};

	// **AT THE BOUND: what a hand put at the bottom comes back.** The half that says the runtime limit
	// is not one level too STRICT — everything `ChainColumn` still allows really does reopen.
	{
		const std::unique_ptr<RackEngine> top = build(deepest);
		const auto [bytes, atBottom] = roundTrip(*top, deepest);
		CHECK(atBottom == std::vector<std::string> { "OWLTT" });

		// **AND THE BYTE CEILING IS NOWHERE NEAR, MEASURED RATHER THAN ASSUMED** (CLAUDE.md: take the
		// shape from the model, the number from the instrument). `kMaxBodyBytes` is the OTHER thing that
		// can refuse a body, and if it bit first then IT would be the real limit and the bound would
		// belong there instead. It does not, and not by a margin that could move: MEASURED 2026-09-20,
		// 2960 bytes for 15 levels in the binary format. **RE-MEASURED 2026-09-25 AFTER THE KEYED FORMAT
		// (backlog JF): 57,845 bytes for 20 levels** — about 2.9 KB a level, because every level now names
		// its values by key, and LINEAR because a rack's body is spliced into its parent as a document. With
		// every body in a `$bin` it was 926,992 — 4/3 per level, exponential — which is why it is not. The
		// 32 MiB ceiling binds at roughly eleven thousand levels. Bytes do not bound depth, which is the very
		// reason `kMaxDepth` exists at all, and it is why raising the depth limit costs nothing here.
		INFO("bytes at the deepest legal nesting (" << deepest << " levels): " << bytes
		                                            << " against kMaxBodyBytes " << rack::kMaxBodyBytes);
		CHECK(bytes < rack::kMaxBodyBytes / 100);
	}

	// **ONE DEEPER: the bottom is empty when it comes back.** The half that says the limit is not one
	// level too GENEROUS. The deepest CONTAINER still appears — it is named in its parent's body — but
	// its own body was refused, so the device a hand put inside it is gone. That is exactly "a patch you
	// can build and cannot reopen", and it is what the bound exists to make unreachable.
	//
	// **AND SINCE THE DEPTH GATE (2026-10-31) A HAND CANNOT GET THERE**: the container that would hold that chain is
	// refused at the record, so the only patch one deeper is a FILE — written here as bytes and read back.
	{
		const std::unique_ptr<RackEngine> top = build(deepest);
		RackEngine* at = top.get();
		RackEngine* parent = nullptr;
		size_t lane = RackEngine::kNoSlot;
		for (size_t level = 0; level < deepest; ++level) {
			lane = at->deviceAtPosition(0);
			parent = at;
			at = rackIn(*at, lane);
			REQUIRE(at != nullptr);
		}
		const size_t before = at->chainLength(0);
		CHECK(addInside(*parent, lane, kRack.c_str()) == kNoInstance);
		CHECK(at->chainLength(0) == before);

		ByteStream file;
		file.bytes = chainsNestedAsFile(deepest + 1, "OWLTT");
		RackEngine loaded { rack::kRackAxes, hostCatalogue() };
		loaded.prepare(48000.0, 4096);
		rack::LoadReport refused;
		clap_istream in = file.in();
		REQUIRE(rack::loadRack(&in, &loaded, &refused));
		RackEngine* walk = &loaded;
		for (size_t level = 0; level < deepest + 1 && walk != nullptr && walk->chainLength(0) > 0; ++level) {
			walk = rackIn(*walk, walk->deviceAtPosition(0));
		}
		REQUIRE(walk != nullptr);
		CHECK(tagsIn(*walk).empty());
	}
}

TEST_CASE("a chain's own bridge edits the rack inside it while the outer runs, and the outer's save carries the edit (rack roadmap §6, 6j)") {
	// THE EDITING ROAD INTO A CHAIN. Until this, a chain's contents arrived only as a body at provision:
	// the inner engine had no record and no bridge, so nothing on the message thread could add a device
	// to it, and the outer's save wrote whatever bytes it had stored. Now the chain device carries a
	// `RackBridge` over its engine (`rack::bridgeOf`), drained by the chain's own process — the standalone
	// plugin's exact pair — and the outer's save asks the running instance for its contents.
	//
	// ASSERTED AT THE ENGINE AND IN THE FILE, not on a render: what the inner engine holds after the drain,
	// and what the body the outer writes parses to.
	RackEngine outer { rack::kRackAxes, hostCatalogue() };
	outer.prepare(48000.0, 4096);
	const size_t lane = outer.addDevice(kRack.c_str());
	REQUIRE(lane != RackEngine::kNoSlot);
	void* storage = outer.slotAtLane(lane).storage.get();
	REQUIRE(storage != nullptr);
	rack::RackBridge* bridge = rack::bridgeOf(storage);
	REQUIRE(bridge != nullptr); // built when the device adopted its catalogue

	SUBCASE("through the bridge: the inner engine holds the device after the outer's block drains it") {
		RackEngine* inner = rackIn(outer, lane);
		REQUIRE(inner != nullptr);
		REQUIRE(tagsIn(*inner).empty());
		const DeviceInstanceId added = bridge->addDevice("OWLTT");
		REQUIRE(added != kNoInstance);
		// NOT YET: the edit is in the record and posted; the engine adopts it at its own drain.
		CHECK(tagsIn(*inner).empty());
		std::vector<float> l(64, 0.0f), r(64, 0.0f);
		outer.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
		CHECK(tagsIn(*inner) == std::vector<std::string> { "OWLTT" });
		// AND THE OUTER'S SAVE CARRIES IT — the body comes from the running instance's own record.
		rack::RackPatch saved;
		REQUIRE(rack::patchOf(outer, &saved));
		REQUIRE(saved.device.size() == 1);
		REQUIRE(!saved.device[0].body.empty());
		ByteStream file;
		file.bytes = saved.device[0].body;
		rack::RackPatch innerPatch;
		clap_istream in = file.in();
		REQUIRE(rack::parseRack(&in, &innerPatch));
		REQUIRE(innerPatch.device.size() == 1);
		CHECK(std::string(innerPatch.device[0].tag.data()) == "OWLTT");
	}
	SUBCASE("the standalone bridge road, whole: the outer's record files the chain's handle, and its patch takes the live contents") {
		// THE OUTER AS FEATHERS HOLDS IT — a record and a converger over the engine (`RackBridge`), which is
		// what files each running container's handle (`ContainerBodies::attach`) and what `patchOfRecord`
		// asks for the body. The engine above is left as it is; a second outer is built for this road.
		RackEngine engine2 { rack::kRackAxes, hostCatalogue() };
		engine2.prepare(48000.0, 4096);
		rack::RackBridge outerBridge { engine2 };
		outerBridge.prepare(48000.0, 4096);
		const DeviceInstanceId chain = outerBridge.addDevice(kRack.c_str());
		REQUIRE(chain != kNoInstance);
		// FILED AT PROVISION, before the drain — an editor opened early queues into a real bridge.
		void* handle = outerBridge.containerSurfaceOf(chain);
		REQUIRE(handle != nullptr);
		rack::RackBridge* innerBridge = rack::bridgeOf(handle);
		REQUIRE(innerBridge != nullptr);
		outerBridge.drain();
		outerBridge.publish(engine2);
		REQUIRE(innerBridge->addDevice("OWLTT") != kNoInstance);
		std::vector<float> l(64, 0.0f), r(64, 0.0f);
		engine2.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
		const rack::RackPatch saved = outerBridge.patch();
		REQUIRE(saved.device.size() == 1);
		REQUIRE(!saved.device[0].body.empty());
		ByteStream file;
		file.bytes = saved.device[0].body;
		rack::RackPatch innerPatch;
		clap_istream in = file.in();
		REQUIRE(rack::parseRack(&in, &innerPatch));
		REQUIRE(innerPatch.device.size() == 1);
		CHECK(std::string(innerPatch.device[0].tag.data()) == "OWLTT");
		// AND THE HANDLE GOES WITH THE INSTANCE: remove the chain, drain, reap — nothing on file.
		outerBridge.removeDevice(chain);
		outerBridge.drain();
		outerBridge.reapRetired();
		CHECK(outerBridge.containerSurfaceOf(chain) == nullptr);
	}
}

TEST_CASE("a metered device INSIDE a chain meters through the chain's own bridge — OWLTT in the rack, and its meters move") {
	// The symptom: OWLTT's meters did not move in the rack. The engine installs the readings
	// sink on every device it adopts (`AudioOps::setReadingsSink`) — the inner engine included — and the
	// chain's own process publishes the inner bridge's mirror, so the nested editor's OWLTT card reads its
	// ten readings from `bridgeOf(storage)->readingAt`. Asserted as a level that moved under signal.
	RackEngine outer { rack::kRackAxes, hostCatalogue() };
	outer.prepare(48000.0, 4096);
	const size_t lane = outer.addDevice(kRack.c_str());
	REQUIRE(lane != RackEngine::kNoSlot);
	const DeviceInstanceId owlttId = addInside(outer, lane, "OWLTT");
	REQUIRE(owlttId != kNoInstance);
	const device::Descriptor* owltt = deviceForTag("OWLTT");
	REQUIRE(owltt != nullptr);
	bridgeIn(outer, lane).setField(owlttId, static_cast<size_t>(owltt->traits.enableControl), 1.0f);
	for (int block = 0; block < 8; ++block) {
		std::vector<float> l(256, 0.0f), r(256, 0.0f);
		for (size_t i = 0; i < l.size(); ++i) {
			l[i] = r[i] = 0.9f * std::sin(static_cast<float>(i) * 0.05f);
		}
		outer.processStereo(l.data(), r.data(), 256, backplane::unknownBlock());
	}
	rack::RackBridge& bridge = bridgeIn(outer, lane);
	REQUIRE(bridge.readingCountAt(owlttId) == 18); // ten meters + 3 band lights + combined GR (backlog EL) + second-channel GR (LQ) + Key Track's note
	bool moved = false;
	for (size_t band = 0; band < 3; ++band) {
		moved = moved || bridge.readingAt(owlttId, 3 + band) > 0.0f;
	}
	CHECK(moved);
}

TEST_CASE("backlog EH: a chain's bypass FADES on the per-voice road too — the inner per-voice devices leave each voice over the same 5 ms the bus fades over, not in one sample") {
	// THE DEVICE'S OWN COMMENT PROMISES "a bypass fades to the dry path rather than cutting", and the
	// bus kept it: wet and dry ramp over `kRampSeconds`. But the bus blends against the signal that
	// ENTERED the bus stage, which on the per-voice road already carries the inner per-voice devices —
	// and the per-voice road returned untouched on `!enabled`. A Poly filter inside a chain vanished
	// from every voice the sample Enabled went false. Asserted BY VALUE against a twin that stays
	// enabled: half way through the ramp a voice reads `dry + w * (twin - dry)` with w restated from
	// the ramp's length; after it, the road is not entered at all and the voice is the dry signal.
	const device::AudioOps& ops = rack::kDeviceAudioOps;
	const auto build = [&](std::vector<unsigned char>& raw) {
		void* mem = raw.data();
		ops.construct(mem);
		ops.prepare(mem, 48000.0, 4096);
		rack::kDeviceContainerOps.adopt(mem, hostCatalogue());
		// THE VOICES A HOST WOULD ALLOT — without them no inner device has a per-voice instance and the
		// per-voice road is never entered, enabled or not.
		REQUIRE(rack::kDeviceContainerOps.setVoices != nullptr);
		rack::kDeviceContainerOps.setVoices(mem, 1);
		rack::RackBridge* bridge = rack::bridgeOf(mem);
		REQUIRE(bridge != nullptr);
		const DeviceInstanceId filterId = bridge->addDevice(fx::filter::kTraits.tag);
		REQUIRE(filterId != kNoInstance);
		const device::Descriptor* filt = deviceForTag(fx::filter::kTraits.tag);
		REQUIRE(filt != nullptr);
		REQUIRE(filt->traits.perVoiceControl >= 0);
		bridge->setField(filterId, static_cast<size_t>(filt->traits.perVoiceControl), 1.0f); // Poly: per voice
		const size_t cutoff = controlNamed(*filt, "Filter Cutoff");
		REQUIRE(cutoff < filt->params.size());
		bridge->setField(filterId, cutoff, static_cast<float>(filt->params[cutoff].min)); // well below the tone
		bridge->drain();
		return mem;
	};
	std::vector<unsigned char> rawA(ops.stateBytes + ops.stateAlign), rawB(ops.stateBytes + ops.stateAlign);
	void* test = build(rawA);
	void* twin = build(rawB);

	std::vector<float> row;
	for (const device::ParamDesc& p : composite::kParamDescs) {
		row.push_back(static_cast<float>(p.def));
	}
	row[static_cast<size_t>(composite::Param::Enabled)] = 1.0f;
	ops.setFields(test, row.data(), row.size());
	ops.setFields(twin, row.data(), row.size());

	std::vector<float> dry(64);
	for (size_t i = 0; i < dry.size(); ++i) {
		dry[i] = 0.5f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 2000.0 * static_cast<double>(i) / 48000.0));
	}
	std::vector<float> busL(128, 0.0f), busR(128, 0.0f);
	// ONE HOST BLOCK: the per-voice road, then the bus stage (which is what advances the ramp).
	const auto block = [&](void* mem, std::vector<float>& voice) {
		const bool perVoice = ops.beginPerVoiceBlock(mem);
		if (perVoice) {
			ops.processVoice(mem, 0, voice.data(), nullptr, static_cast<int>(voice.size()), device::NoteContext {});
		}
		std::fill(busL.begin(), busL.end(), 0.0f);
		std::fill(busR.begin(), busR.end(), 0.0f);
		ops.processStereo(mem, busL.data(), busR.data(), static_cast<int>(busL.size()));
		return perVoice;
	};

	// ENABLED: both filter the voice, identically, and the filter is doing something.
	std::vector<float> a = dry, b = dry;
	REQUIRE(block(test, a));
	REQUIRE(block(twin, b));
	REQUIRE(a == b);
	float moved = 0.0f;
	for (size_t i = 0; i < a.size(); ++i) {
		moved = std::max(moved, std::fabs(a[i] - dry[i]));
	}
	REQUIRE(moved > 0.01f);

	// BYPASSED: the ramp is 5 ms = 240 samples; the bus ran 128 of them in the block below, so the
	// voice road's next call sees w = 1 - 128/240.
	row[static_cast<size_t>(composite::Param::Enabled)] = 0.0f;
	ops.setFields(test, row.data(), row.size());
	a = dry;
	b = dry;
	REQUIRE(block(test, a)); // still on the per-voice road: the fade has something to fade
	REQUIRE(block(twin, b));
	a = dry;
	b = dry;
	REQUIRE(block(test, a));
	REQUIRE(block(twin, b));
	const float w = 1.0f - 128.0f / 240.0f;
	int wrong = 0;
	for (size_t i = 0; i < a.size(); ++i) {
		const float want = dry[i] + w * (b[i] - dry[i]);
		if (std::fabs(a[i] - want) > 1e-4f) {
			++wrong;
		}
	}
	CHECK(wrong == 0);
	// AND NOT A CUT, AND NOT UNFADED: the voice sits strictly between the dry and the twin.
	CHECK(a != dry);
	CHECK(a != b);

	// PAST THE RAMP: the road is not entered and the voice is the dry signal, untouched.
	a = dry;
	CHECK_FALSE(block(test, a));
	CHECK(a == dry);

	ops.destruct(test);
	ops.destruct(twin);
}

TEST_CASE("a rack device's macro control reaches a control inside it, through the rack's own edges") {
	// **CITED BY test_fx_devices's HONESTY SWEEP** for "Chain :: Macro 1..8". The rack's edges carrying
	// a macro to a control are `test_rack_engine`'s "a macro routed to a control moves that control's
	// RESOLVED VALUE"; this is the other half — that the DEVICE's Macro controls are what set them.
	const device::AudioOps& ops = rack::kDeviceAudioOps;
	std::vector<unsigned char> raw(ops.stateBytes + ops.stateAlign);
	void* mem = raw.data();
	ops.construct(mem);
	ops.prepare(mem, 48000.0, 4096);
	rack::kDeviceContainerOps.adopt(mem, hostCatalogue());
	RackEngine& inner = rack::engineForTest(mem);

	// THROUGH THE CHAIN'S OWN BRIDGE (rack roadmap §6): the record is the truth inside a chain device and its
	// drain — at the device's own process — reconciles the engine to it, so the device and the route go in
	// the way a hand's would.
	rack::RackBridge* bridge = rack::bridgeOf(mem);
	REQUIRE(bridge != nullptr);
	const DeviceInstanceId filterId = bridge->addDevice("flt1");
	REQUIRE(filterId != kNoInstance);
	const device::Descriptor* filt = deviceForTag("flt1");
	const size_t cutoff = controlNamed(*filt, "Filter Cutoff");
	REQUIRE(cutoff < filt->params.size());
	bridge->setRouteAmount(test::macro(0), backplane::Destination::control(filterId, cutoff), 0.5f);
	bridge->drain(); // the drain the device's own process would do; nothing else runs this engine
	const size_t slot = inner.slotOf(filterId);
	REQUIRE(slot != RackEngine::kNoSlot);
	const float base = inner.fieldAt(slot, cutoff);

	std::vector<float> row;
	for (const device::ParamDesc& p : composite::kParamDescs) {
		row.push_back(static_cast<float>(p.def));
	}
	std::vector<float> l(64, 0.1f);
	std::vector<float> r(64, 0.1f);
	ops.setFields(mem, row.data(), row.size());
	ops.processStereo(mem, l.data(), r.data(), 64);
	CHECK(inner.resolvedFieldForTest(slot, cutoff) == doctest::Approx(base)); // macro at 0

	row[static_cast<size_t>(composite::Param::Macro1)] = 1.0f; // THE DEVICE'S CONTROL
	ops.setFields(mem, row.data(), row.size());
	ops.processStereo(mem, l.data(), r.data(), 64);
	CHECK(inner.resolvedFieldForTest(slot, cutoff) != doctest::Approx(base));
	CHECK(inner.macro(0) == doctest::Approx(1.0f));

	// **AND THROUGH ITS POLARITY, NESTED (backlog EG).** The macro is the chain device's parameter — a
	// destination the outer host resolves by READING its routes — and the device hands the resolved 0..1
	// to this rack as a plain macro value, where the rack's own mode maps it. So the mode is the inner
	// bridge's, exactly as standalone, and the reach is amount x (2v - 1) x span, restated here.
	const device::ParamDesc& p = filt->params[cutoff];
	bridge->setMacroBipolar(0, true);
	bridge->drain();
	row[static_cast<size_t>(composite::Param::Macro1)] = 0.25f; // reads -0.5 through the mode
	ops.setFields(mem, row.data(), row.size());
	ops.processStereo(mem, l.data(), r.data(), 64);
	const double below = static_cast<double>(base) - 0.5 * 0.5 * device::routeSpanOf(p);
	CHECK(inner.resolvedFieldForTest(slot, cutoff)
	      == doctest::Approx(p.offsetsMayLeaveRange ? below : std::max(below, p.min)));
	CHECK(inner.macro(0) == doctest::Approx(0.25f)); // the stored value is the device's parameter, unmapped
	row[static_cast<size_t>(composite::Param::Macro1)] = 0.5f; // the centre: nothing
	ops.setFields(mem, row.data(), row.size());
	ops.processStereo(mem, l.data(), r.data(), 64);
	CHECK(inner.resolvedFieldForTest(slot, cutoff) == doctest::Approx(base));
	// AND THE MODE RIDES THE DEVICE'S BODY, which is the inner patch: saved and loaded with it.
	CHECK(bridge->patch().macroBipolar.size() > 0);
	CHECK(bridge->patch().macroBipolar[0] == 1);
	ops.destruct(mem);
}

TEST_CASE("a rack device hands its host's note to what it holds") {
	// **CITED BY test_fx_devices's NOTE-ORDER SWEEP**, which sweeps a rack empty and so cannot see its
	// note road. A keytracking filter inside the rack: the note moves the rack's output.
	const auto render = [](float pitch) {
		const device::AudioOps& ops = rack::kDeviceAudioOps;
		std::vector<unsigned char> raw(ops.stateBytes + ops.stateAlign);
		void* mem = raw.data();
		ops.construct(mem);
		ops.prepare(mem, 48000.0, 4096);
		rack::kDeviceContainerOps.adopt(mem, hostCatalogue());
		(void)rack::engineForTest(mem); // the inner engine is reached by the bridge below
		rack::RackBridge* bridge = rack::bridgeOf(mem); // as the macro case: the record is the road in
		REQUIRE(bridge != nullptr);
		const DeviceInstanceId filterId = bridge->addDevice("flt1");
		REQUIRE(filterId != kNoInstance);
		const device::Descriptor* filt = deviceForTag("flt1");
		bridge->setField(filterId, controlNamed(*filt, "Filter Keytrack"), 1.0f);
		// **AND POLY, BECAUSE THE FILTER'S KEYTRACK FOLLOWS A NOTE ONLY IN ITS PER-VOICE MODE** — measured
		// on the filter alone with no rack anywhere near it: keytrack on and Poly off gives bit-identical
		// output at notes four octaves apart; Poly on moves it. That is the filter's rule, not the rack's,
		// and it is why `test_fx_devices`'s note sweep puts every switch at its ceiling.
		bridge->setField(filterId, controlNamed(*filt, "Filter Poly"), 1.0f);
		std::vector<float> row;
		for (const device::ParamDesc& p : composite::kParamDescs) {
			row.push_back(static_cast<float>(p.def));
		}
		ops.setFields(mem, row.data(), row.size());
		ops.setNoteContext(mem, device::NoteContext { pitch, true });
		// NOISE, so there is content between the two corners the notes put the filter at (about 250 Hz
		// and 4 kHz). The first draft used a pulse every seven samples — a fundamental at 6.9 kHz, above
		// BOTH corners — so both lowpasses passed only DC and the case read zero movement through a
		// perfectly working note road. Measured on the filter alone before blaming the rack.
		std::vector<float> l(2048), r(2048);
		uint32_t seed = 0x9e3779b9u;
		for (size_t i = 0; i < l.size(); ++i) {
			seed = seed * 1664525u + 1013904223u;
			l[i] = r[i] = static_cast<float>(static_cast<int32_t>(seed)) / 2147483648.0f * 0.5f;
		}
		ops.processStereo(mem, l.data(), r.data(), static_cast<int>(l.size()));
		ops.destruct(mem);
		return l;
	};
	const auto low = render(36.0f);
	const auto high = render(84.0f);
	int moved = 0;
	for (size_t i = 0; i < low.size(); ++i) {
		if (low[i] != high[i]) {
			++moved;
		}
	}
	CHECK(moved > 0);
}

TEST_CASE("a rack in Feathers' lane holds a rack, through the lane's own save and provision road") {
	// THE SAME RACK, NOW AS AN EFFECT IN FEATHERS' fx LANE — its contents are the rack's own file,
	// carried by the lane's record and handed over at provisioning.
	RackEngine source { rack::kRackAxes, hostCatalogue() };
	source.prepare(48000.0, 4096);
	const size_t sourceLane = source.addDevice(kRack.c_str());
	RackEngine* inner = rackIn(source, sourceLane);
	REQUIRE(inner != nullptr);
	REQUIRE(addInside(source, sourceLane, "OWLTT") != kNoInstance);
	ByteStream file;
	clap_ostream out = file.out();
	REQUIRE(rack::saveRack(&out, source));

	FxChain chain {};
	chain.nextInstanceId = kFirstFreeInstance;
	const size_t slot = addFxDevice(&chain, composite::kTraits.tag);
	REQUIRE(slot < kMaxFxSlots);
	ContainerBodies bodies;
	REQUIRE(bodies.store(fxDeviceAtPosition(chain, slot).row.instanceId, file.bytes.data(), file.bytes.size()));

	// FEATHERS' LANE — itself a rack engine now, so this is a rack, in a rack, in a rack.
	LaneRack lane;
	lane.prepare(48000.0, 0, 4096);
	REQUIRE(lane.provision(chain, &bodies) == 1);
	lane.sync(chain, device::NoteContext {}, device::TransportClock::unknown());
	lane.finish();
	// **THE RACK, BY THE PLACE IT STANDS IN THE CHAIN** — a place is `(chain, position)` since kill-lanes
	// step 3b, and it is the only device the chain holds, so position 0 names it. `storageOfForTest` takes
	// the device, never a number that could be a slot or a lane.
	REQUIRE(fxChainLength(chain) == 1);
	const DeviceInstanceId inChain = lane.deviceOfStatePosition(kLaneChain, 0);
	REQUIRE(inChain != kNoInstance);
	const void* laneStorage = lane.storageOfForTest(inChain);
	REQUIRE(laneStorage != nullptr);
	RackEngine& inLane = rack::engineForTest(const_cast<void*>(laneStorage));
	REQUIRE(tagsIn(inLane) == std::vector<std::string> { kRack });
	RackEngine* nested = rackIn(inLane, inLane.deviceAtPosition(0));
	REQUIRE(nested != nullptr);
	CHECK(tagsIn(*nested) == std::vector<std::string> { "OWLTT" });
}

//------------------------------------------------------------------------------------//
// A THIRD PARTY'S CHAIN, HOSTING OUR RACK, INSIDE FEATHERS' LANE.                        //
//------------------------------------------------------------------------------------//

namespace foreign {

// A third party can implement its own chain device, which can host our chain and nest inside
// Feathers. This namespace is that third party. It uses ONLY `device/registry.h`: it
// builds its members from the descriptors its host's catalogue hands it, and it adopts any member that
// holds devices with that same catalogue — the one rule every host obeys. Nothing here knows what a rack
// is. If our rack needed anything from its host beyond the catalogue, this would be where it failed.

constexpr size_t kMax = 4;

struct Member {
	const device::Descriptor* desc = nullptr;
	void* storage = nullptr;
};

struct State {
	device::Catalogue catalogue {};
	double rate = 48000.0;
	std::vector<Member> members;
	~State() {
		for (Member& m : members) {
			m.desc->audio->destruct(m.storage);
			feathers::test::heapFree(m.storage);
		}
	}
};

void construct(void* s) { new (s) State(); }
void destruct(void* s) { static_cast<State*>(s)->~State(); }
void prepare(void* s, double rate, int) { static_cast<State*>(s)->rate = rate; }
void reset(void*) {}
void setFields(void*, const float*, size_t) {}
void process(void* s, float* l, float* r, int n) {
	for (Member& m : static_cast<State*>(s)->members) {
		m.desc->audio->processStereo(m.storage, l, r, n);
	}
}
void adopt(void* s, const device::Catalogue& c) { static_cast<State*>(s)->catalogue = c; }

/// ITS OWN FORMAT: a count, then per member a tag, a u32 body length and the body.
bool load(void* s, const uint8_t* data, size_t bytes) {
	State& st = *static_cast<State*>(s);
	size_t at = 0;
	const auto u32 = [&](uint32_t& v) {
		if (bytes - at < 4) {
			return false;
		}
		std::memcpy(&v, data + at, 4);
		at += 4;
		return true;
	};
	uint32_t count = 0;
	if (!u32(count) || count > kMax) {
		return false;
	}
	for (uint32_t i = 0; i < count; ++i) {
		if (bytes - at < 4) {
			return false;
		}
		char tag[5] = {};
		std::memcpy(tag, data + at, 4);
		at += 4;
		uint32_t bodyLen = 0;
		if (!u32(bodyLen) || bytes - at < bodyLen) {
			return false;
		}
		const device::Descriptor* d = st.catalogue(tag);
		if (d != nullptr && d->audio != nullptr) {
			const size_t align = std::max<size_t>(d->audio->stateAlign, alignof(std::max_align_t));
			void* mem = feathers::test::heapAlignedAlloc(align, ((d->audio->stateBytes + align - 1) / align) * align);
			d->audio->construct(mem);
			d->audio->prepare(mem, st.rate, 4096);
			// **THE RULE: A MEMBER THAT HOLDS DEVICES IS HANDED THIS CHAIN'S CATALOGUE.**
			if (d->container != nullptr && d->container->adopt != nullptr) {
				d->container->adopt(mem, st.catalogue);
				if (bodyLen != 0 && d->container->loadState != nullptr) {
					d->container->loadState(mem, data + at, bodyLen);
				}
			}
			st.members.push_back({ d, mem });
		}
		at += bodyLen;
	}
	return at == bytes;
}

bool save(const void*, const device::ByteSink&) { return true; } // not exercised here

const device::AudioOps kAudio { sizeof(State), alignof(State), &construct, &destruct, &prepare, &reset,
	                            nullptr, &setFields, nullptr, nullptr, nullptr, &process, nullptr };
const device::ContainerOps kContainer { &adopt, &save, &load, 1u << 20 };
const device::Traits kTraits { .tag = "XCHN", .name = "Foreign Chain", .hasCard = false, .hasExpanded = false };
const device::Descriptor kDescriptor { .traits = kTraits, .params = {}, .outputs = {}, .audio = &kAudio,
	                                   .mod = nullptr, .notes = nullptr, .container = &kContainer };

/// FEATHERS' CATALOGUE WITH THE THIRD PARTY'S DEVICE IN IT — what a directory-indexed browser would
/// hand the lane once the foreign binary was found.
const device::Descriptor* find(void*, const char* tag) {
	if (device::sameTag(tag, kTraits.tag)) {
		return &kDescriptor;
	}
	return deviceForTag(tag);
}

} // namespace foreign

TEST_CASE("a third party's chain hosts our rack, inside Feathers' lane, and our rack holds devices") {
	// OUR RACK, holding OWLTT, saved as its own file.
	RackEngine ours { rack::kRackAxes, hostCatalogue() };
	ours.prepare(48000.0, 4096);
	ours.addDevice("OWLTT");
	ByteStream rackFile;
	clap_ostream rackOut = rackFile.out();
	REQUIRE(rack::saveRack(&rackOut, ours));

	// THE FOREIGN CHAIN'S BODY, in its own format: one member, our rack, with our rack's file as its body.
	std::vector<uint8_t> foreignBody;
	const auto put = [&foreignBody](uint32_t v) {
		const auto* p = reinterpret_cast<const uint8_t*>(&v);
		foreignBody.insert(foreignBody.end(), p, p + 4);
	};
	put(1);
	foreignBody.insert(foreignBody.end(), kRack.begin(), kRack.end());
	put(static_cast<uint32_t>(rackFile.bytes.size()));
	foreignBody.insert(foreignBody.end(), rackFile.bytes.begin(), rackFile.bytes.end());

	// FEATHERS' LANE, whose catalogue has found the foreign binary. The lane holds the foreign chain.
	const device::Catalogue withForeign { nullptr, &foreign::find };
	FxChain chain {};
	chain.nextInstanceId = kFirstFreeInstance;
	chain.device[0].row.tag = { 'X', 'C', 'H', 'N' };
	chain.device[0].row.instanceId = chain.nextInstanceId++;
	// ONE DEVICE AT POSITION 0, AND THAT IS THE WHOLE OF ITS PLACE (kill-lanes step 3c). This used to
	// stamp a `routeLane` and an `order[]` entry beside the row; the chain is dense in `[0, count)` now,
	// so `device[0]` IS the first device and there is no second number to keep agreeing with it.
	chain.count = 1;
	ContainerBodies bodies;
	REQUIRE(bodies.store(chain.device[0].row.instanceId, foreignBody.data(), foreignBody.size()));

	// **THE LANE RESOLVES TAGS THROUGH `kDevices`, SO A DEVICE THE BUILD WAS NOT COMPILED WITH CANNOT BE
	// PROVISIONED BY IT YET** — that is the directory-indexed catalogue still to come. The lane's half of
	// the road is exercised by the case above; this one builds the foreign chain the way the lane would —
	// construct, prepare, adopt with the lane's catalogue, load — and asserts everything below it.
	const device::Descriptor& fd = foreign::kDescriptor;
	std::vector<unsigned char> raw(fd.audio->stateBytes + fd.audio->stateAlign);
	void* mem = raw.data();
	fd.audio->construct(mem);
	fd.audio->prepare(mem, 48000.0, 4096);
	fd.container->adopt(mem, withForeign);
	const auto body = bodies.bodyFor(chain.device[0].row.instanceId);
	REQUIRE(fd.container->loadState(mem, body.data(), body.size()));

	auto& st = *static_cast<foreign::State*>(mem);
	REQUIRE(st.members.size() == 1);
	REQUIRE(device::sameTag(st.members[0].desc->traits.tag, composite::kTraits.tag));
	// **OUR RACK, INSIDE THEIR CHAIN, HOLDS WHAT IT SAVED** — which it can only do because the foreign
	// chain handed it a catalogue, and nothing else.
	CHECK(tagsIn(rack::engineForTest(st.members[0].storage)) == std::vector<std::string> { "OWLTT" });

	// AND IT RENDERS THROUGH BOTH.
	std::vector<float> l(256, 0.2f), r(256, 0.2f);
	fd.audio->processStereo(mem, l.data(), r.data(), 256);
	CHECK(std::isfinite(l.back()));
	fd.audio->destruct(mem);
}

TEST_CASE("the rack's engine, state and device include nothing from a host — so it could be built alone") {
	// **THE SEPARABILITY CLAIM, CHECKED BY READING THE INCLUDE GRAPH.** A rack in a third party's chain,
	// or a rack in its own binary, must not reach into the host that built it. One convenient include of
	// the host's device list would compile, work, and end that.
	const std::filesystem::path root(FEATHERS_RACK_SOURCE_DIR);
	int filesRead = 0;
	for (const char* name : { "rack_engine.h", "rack_engine.cpp", "rack_state.h", "rack_state.cpp",
	                          "rack_device.h", "rack_device.cpp", "rack_axes.h" }) {
		const std::filesystem::path p = root / name;
		CAPTURE(p.string());
		std::ifstream in(p);
		REQUIRE(in.good());
		++filesRead;
		std::string line;
		while (std::getline(in, line)) {
			if (line.rfind("#include", 0) != 0) {
				continue;
			}
			CAPTURE(line);
			CHECK(line.find("plugin/") == std::string::npos);
		}
	}
	CHECK(filesRead == 7);
}



// ---------------------------------------------------------------------------------------------
// RACK ROADMAP §7 — THE FEATHERS ROAD: the plugin entry wires `Synth::setOuterGateMaker` over the
// bridge's record (`feathers_clap.cpp`); this pins that wiring, without the plugin shell, by doing
// exactly what it does and asking the chain the synth provisioned.
// ---------------------------------------------------------------------------------------------

TEST_CASE("§7: a chain in Feathers' lane is handed the gate the plugin wires — over the bridge's record, through the synth, before its body loads — so a control inside may read a Feathers macro or strip modulator") {
	namespace cmp = feathers::composite;
	feathers::ParamSet params;
	feathers::Synth synth { params };
	feathers::ParamBridge bridge;
	synth.activate(48000.0, 4096);
	params.setBase(static_cast<clap_id>(feathers::ParamId::VoiceLevel), 1.0); // the default base is zero (EF)
	synth.setCatalogue(feathers::hostCatalogue());
	synth.setOuterGateMaker(&feathers::rack::ChainRecord::gateOver, &bridge.chainRecord());

	const size_t slot = bridge.addFxDevice(cmp::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	const size_t strip = bridge.addRackDevice(feathers::mod::dahdsr::kTraits.tag);
	REQUIRE(strip < feathers::kMaxRackSlots);
	bridge.refresh();
	const feathers::DeviceInstanceId chain = bridge.fxDeviceAt(slot);
	const feathers::DeviceInstanceId envelope = bridge.rackDeviceAt(strip);
	REQUIRE(chain != feathers::kNoInstance);
	REQUIRE(envelope != feathers::kNoInstance);
	REQUIRE(synth.provisionDevices(bridge.chainState(), bridge.publishedChains().sequence(), &bridge.fxBodies()) >= 1);

	void* storage = bridge.fxBodies().surfaceOf(chain);
	REQUIRE(storage != nullptr);
	feathers::rack::RackBridge* inner = feathers::rack::bridgeOf(storage);
	REQUIRE(inner != nullptr);
	CHECK(inner->hasOuterGate());

	const feathers::DeviceInstanceId filter = inner->addDevice(feathers::fx::filter::kTraits.tag);
	REQUIRE(filter != feathers::kNoInstance);
	size_t cutoff = 0;
	while (cutoff < feathers::fx::filter::kDescriptor.params.size()
	       && !feathers::fx::filter::kDescriptor.params[cutoff].modulatable) {
		++cutoff;
	}
	const feathers::backplane::Destination dest = feathers::backplane::Destination::control(filter, cutoff);
	// FEATHERS' MACRO 1 AND ITS STRIP ENVELOPE ARE UPSTREAM OF A CHAIN IN THE FX LANE; a core index past
	// Feathers' table is not a source at all.
	CHECK(inner->mayRoute(feathers::backplane::Source::outerCore(0), dest));
	CHECK(inner->mayRoute(feathers::backplane::Source::outerOutput(envelope, 0), dest));
	CHECK_FALSE(inner->mayRoute(feathers::backplane::Source::outerCore(feathers::feathersAxes().coreSources + 5), dest));
	// AND THE ROUTE IS TAKEN.
	inner->setRouteAmount(feathers::backplane::Source::outerOutput(envelope, 0), dest, 0.5f);
	CHECK(inner->routeAmount(feathers::backplane::Source::outerOutput(envelope, 0), dest) == doctest::Approx(0.5f));
}

// ---------------------------------------------------------------------------------------------
// RACK ROADMAP §8 — THE PASS-THROUGH MODEL. A chain
// is not Poly; its devices are, each for itself, exactly as in the lane: the host's voice count runs
// inside, and the host's two stages run inside — per voice for the devices that ask, on the bus for the
// rest — on ONE instance, with no voice instances of the chain itself.
// ---------------------------------------------------------------------------------------------

namespace {

struct FeathersChainRig {
	feathers::ParamSet params;
	feathers::Synth synth { params };
	feathers::ParamBridge bridge;
	feathers::DeviceInstanceId chain = feathers::kNoInstance;
	feathers::rack::RackBridge* inner = nullptr;
	std::vector<float> l = std::vector<float>(64, 0.0f);
	std::vector<float> r = std::vector<float>(64, 0.0f);

	FeathersChainRig() {
		synth.activate(48000.0, 4096);
		// A FIXED LEVEL: the default patch's Level base is zero and its envelope route is the bridge's
		// (backlog EF), which this rig never provisions into the synth; the old fixed level, said.
		params.setBase(static_cast<clap_id>(feathers::ParamId::VoiceLevel), 1.0);
		synth.setCatalogue(feathers::hostCatalogue());
		synth.setOuterGateMaker(&feathers::rack::ChainRecord::gateOver, &bridge.chainRecord());
		const size_t slot = bridge.addFxDevice(feathers::composite::kTraits.tag);
		REQUIRE(slot < feathers::kMaxFxSlots);
		bridge.refresh();
		chain = bridge.fxDeviceAt(slot);
		REQUIRE(chain != feathers::kNoInstance);
		provision();
		block();
		inner = feathers::rack::bridgeOf(bridge.fxBodies().surfaceOf(chain));
		REQUIRE(inner != nullptr);
	}
	void provision() {
		feathers::publishEditorLanes(bridge, params, synth);
		(void)synth.provisionDevices(bridge.chainState(), bridge.publishedChains().sequence(), &bridge.fxBodies());
	}
	void block() { synth.render(l.data(), r.data(), 64); }
	const feathers::rack::RackEngine& lane() const { return synth.laneForTest().engineForTest(); }
	static size_t polyOf(const device::Descriptor& d) { return static_cast<size_t>(d.traits.perVoiceControl); }
};

} // namespace

TEST_CASE("§8: a chain in Feathers' lane runs the lane's voices INSIDE it — its inner engine has the lane's voice count, an inner Poly filter gets one instance per outer voice, and the chain itself has none") {
	FeathersChainRig rig;
	const feathers::rack::RackEngine& lane = rig.lane();
	REQUIRE(lane.voiceCount() > 0);
	const size_t at = lane.slotOf(rig.chain);
	REQUIRE(at != feathers::rack::RackEngine::kNoSlot);
	const auto& seat = lane.slotAtLane(at);
	CHECK(seat.voices.empty()); // the chain is not a Poly device
	feathers::rack::RackEngine* innerEngine = feathers::rack::engineOf(rig.bridge.fxBodies().surfaceOf(rig.chain));
	REQUIRE(innerEngine != nullptr);
	CHECK(innerEngine->voiceCount() == lane.voiceCount());

	// A POLY FILTER INSIDE: one instance per OUTER voice, built at the inner provision — once its switch is on
	// (backlog NS: with Poly off it has none, inside a chain as anywhere else).
	const feathers::DeviceInstanceId filter = rig.inner->addDevice(feathers::fx::filter::kTraits.tag);
	REQUIRE(filter != feathers::kNoInstance);
	rig.provision();
	rig.block();
	const size_t innerLane = innerEngine->slotOf(filter);
	REQUIRE(innerLane != feathers::rack::RackEngine::kNoSlot);
	CHECK(innerEngine->slotAtLane(innerLane).voices.empty());
	rig.inner->setField(filter, FeathersChainRig::polyOf(*innerEngine->slotAtLane(innerLane).live_desc()), 1.0f);
	rig.provision();
	rig.block();
	CHECK(innerEngine->slotAtLane(innerLane).voices.size() == lane.voiceCount());
}

TEST_CASE("§8: the chain is per voice exactly when something inside it is — the lane's per-voice stage reaches the inner Poly filter's voice instance while a note sounds, and the bus stage still runs") {
	FeathersChainRig rig;
	const feathers::rack::RackEngine& lane = rig.lane();
	const size_t at = lane.slotOf(rig.chain);
	REQUIRE(at != feathers::rack::RackEngine::kNoSlot);
	const auto& seat = lane.slotAtLane(at);
	feathers::rack::RackEngine* innerEngine = feathers::rack::engineOf(rig.bridge.fxBodies().surfaceOf(rig.chain));
	REQUIRE(innerEngine != nullptr);
	const feathers::DeviceInstanceId filter = rig.inner->addDevice(feathers::fx::filter::kTraits.tag);
	REQUIRE(filter != feathers::kNoInstance);
	rig.provision();
	rig.block();
	const size_t innerLane = innerEngine->slotOf(filter);
	REQUIRE(innerLane != feathers::rack::RackEngine::kNoSlot);

	// POLY OFF INSIDE: the chain runs on the bus, nothing per voice.
	rig.block();
	CHECK_FALSE(seat.polyThisBlock);
	CHECK_FALSE(innerEngine->slotAtLane(innerLane).polyThisBlock);

	// POLY ON INSIDE: the chain is per voice this block, and a sounding note reaches the filter's voice
	// instance through the chain's per-voice road — the two stages the lane runs, run inside.
	rig.inner->setField(filter, FeathersChainRig::polyOf(feathers::fx::filter::kDescriptor), 1.0f);
	rig.provision();
	rig.block();
	rig.block();
	CHECK(seat.polyThisBlock);
	CHECK(innerEngine->slotAtLane(innerLane).polyThisBlock);
	rig.synth.noteOn(0, 60, 1, 0.8);
	rig.block();
	rig.block();
	CHECK(seat.polyThisBlock);
	float peak = 0.0f;
	for (const float x : rig.l) {
		peak = std::max(peak, std::abs(x));
	}
	CHECK(peak > 0.05f); // the note sounded through the chain
	rig.synth.noteOff(0, 60, 1, 0.0);
	for (int i = 0; i < 40; ++i) {
		rig.block();
	}
	// POLY OFF AGAIN: back to the bus alone.
	rig.inner->setField(filter, FeathersChainRig::polyOf(feathers::fx::filter::kDescriptor), 0.0f);
	rig.provision();
	rig.block();
	rig.block();
	CHECK_FALSE(seat.polyThisBlock);
}

TEST_CASE("§8: a Poly filter inside a chain renders what the same Poly filter in the lane renders — the chain's per-voice road is transparent") {
	// THE IDENTITY THAT MEANS SOMETHING under the pass-through model: the chain adds nothing of its own
	// to the per-voice stage (mirror, run, average), so a per-voice filter inside it must sound like the
	// same filter placed directly in the lane, to the noise floor.
	const auto renderWith = [](bool inChain) {
		feathers::ParamSet params;
		feathers::Synth synth { params };
		feathers::ParamBridge bridge;
		synth.activate(48000.0, 4096);
		params.setBase(static_cast<clap_id>(feathers::ParamId::VoiceLevel), 1.0); // the default base is zero (EF)
		synth.setCatalogue(feathers::hostCatalogue());
		synth.setOuterGateMaker(&feathers::rack::ChainRecord::gateOver, &bridge.chainRecord());
		const auto provision = [&] {
			feathers::publishEditorLanes(bridge, params, synth);
			(void)synth.provisionDevices(bridge.chainState(), bridge.publishedChains().sequence(), &bridge.fxBodies());
		};
		std::vector<float> l(64, 0.0f);
		std::vector<float> r(64, 0.0f);
		const auto block = [&] { synth.render(l.data(), r.data(), 64); };
		const size_t poly = static_cast<size_t>(feathers::fx::filter::kDescriptor.traits.perVoiceControl);
		if (inChain) {
			const size_t slot = bridge.addFxDevice(feathers::composite::kTraits.tag);
			REQUIRE(slot < feathers::kMaxFxSlots);
			bridge.refresh();
			const feathers::DeviceInstanceId chain = bridge.fxDeviceAt(slot);
			provision();
			block();
			feathers::rack::RackBridge* inner = feathers::rack::bridgeOf(bridge.fxBodies().surfaceOf(chain));
			REQUIRE(inner != nullptr);
			const feathers::DeviceInstanceId filter = inner->addDevice(feathers::fx::filter::kTraits.tag);
			REQUIRE(filter != feathers::kNoInstance);
			inner->setField(filter, poly, 1.0f);
		}
		else {
			const size_t slot = bridge.addFxDevice(feathers::fx::filter::kTraits.tag);
			REQUIRE(slot < feathers::kMaxFxSlots);
			bridge.refresh();
			bridge.setDeviceControl(bridge.fxDeviceAt(slot), poly, 1.0f);
		}
		provision();
		for (int i = 0; i < 4; ++i) {
			block();
		}
		synth.noteOn(0, 60, 1, 0.8);
		std::vector<float> out;
		for (int i = 0; i < 24; ++i) {
			block();
			out.insert(out.end(), l.begin(), l.end());
		}
		return out;
	};
	const std::vector<float> direct = renderWith(false);
	const std::vector<float> inChain = renderWith(true);
	REQUIRE(direct.size() == inChain.size());
	float peak = 0.0f;
	float worst = 0.0f;
	for (size_t i = 0; i < direct.size(); ++i) {
		peak = std::max(peak, std::abs(direct[i]));
		worst = std::max(worst, std::abs(direct[i] - inChain[i]));
	}
	REQUIRE(peak > 0.05f);
	CHECK(worst < peak * 1e-3f);
}

TEST_CASE("§8: a modulator inside a chain in Feathers' lane advances by the audio rendered — the lane begins its segment with no frame count, and the chain clocks its inner modulators off its bus chunks") {
	// THE TRAP THIS GUARDS (2026-09-14): the lane's `begin` passes ZERO frames to its engine, because its
	// own strip ticks on Feathers' control grid — so a chain that advanced its inner modulators by the
	// frame count of the host's begin advanced them by nothing, and an envelope inside a chain never
	// left rest. The chain clocks them off its bus stage instead.
	FeathersChainRig rig;
	feathers::rack::RackEngine* innerEngine = feathers::rack::engineOf(rig.bridge.fxBodies().surfaceOf(rig.chain));
	REQUIRE(innerEngine != nullptr);
	const feathers::DeviceInstanceId envelope = rig.inner->addDevice(feathers::mod::dahdsr::kTraits.tag);
	REQUIRE(envelope != feathers::kNoInstance);
	// A ONE-SECOND ATTACK (log2 seconds, so 0 is one second): 750 blocks of 64, so eight blocks in it is
	// still rising and sixteen is higher still.
	rig.inner->setField(envelope, static_cast<size_t>(feathers::mod::dahdsr::Param::Attack), 0.0f);
	rig.provision();
	rig.block();
	const size_t lane = innerEngine->slotOf(envelope);
	REQUIRE(lane != feathers::rack::RackEngine::kNoSlot);
	rig.block();
	CHECK(innerEngine->outputAt(lane, 0) == doctest::Approx(0.0f)); // at rest: no note, no gate inside

	rig.synth.noteOn(0, 60, 1, 0.8);
	for (int i = 0; i < 8; ++i) {
		rig.block();
	}
	const float atEight = innerEngine->outputAt(lane, 0);
	for (int i = 0; i < 8; ++i) {
		rig.block();
	}
	const float atSixteen = innerEngine->outputAt(lane, 0);
	CHECK(atEight > 0.0f);
	CHECK(atSixteen > atEight);
	CHECK(atSixteen < 0.5f); // and not the ten-millisecond default, which would have finished by now
}

TEST_CASE("backlog FK: a chain in Feathers' lane runs its inner modulators on the control tick, and an inner Bus Utility follows an inner envelope's tick window") {
	// THE CLAIM: inside a chain, every inner modulator advances a whole tick when it closes, and the inner devices are
	// handed that tick's windows at their phase — so a route inside a chain is smoothed exactly as one in the lane.
	// Asserted as values: the inner Gain window is the resolve of the envelope's published ramp at its last sample,
	// and Bus Utility's own count says it followed windows.
	namespace bu = feathers::fx::busutil;
	FeathersChainRig rig;
	feathers::rack::RackEngine* innerEngine = feathers::rack::engineOf(rig.bridge.fxBodies().surfaceOf(rig.chain));
	REQUIRE(innerEngine != nullptr);
	const feathers::DeviceInstanceId bus = rig.inner->addDevice(bu::kTraits.tag);
	const feathers::DeviceInstanceId envelope = rig.inner->addDevice(feathers::mod::dahdsr::kTraits.tag);
	REQUIRE(bus != feathers::kNoInstance);
	REQUIRE(envelope != feathers::kNoInstance);
	rig.inner->setField(envelope, static_cast<size_t>(feathers::mod::dahdsr::Param::Attack), 0.0f); // one second
	const size_t gain = static_cast<size_t>(bu::Param::Gain);
	constexpr float kAmount = 0.5f;
	rig.inner->setRouteAmount(feathers::backplane::Source::output(envelope, 0),
	                          feathers::backplane::Destination::control(bus, gain), kAmount);
	rig.provision();
	rig.block();
	rig.block();
	const size_t busLane = innerEngine->slotOf(bus);
	const size_t envLane = innerEngine->slotOf(envelope);
	REQUIRE(busLane != feathers::rack::RackEngine::kNoSlot);
	REQUIRE(envLane != feathers::rack::RackEngine::kNoSlot);
	const device::ParamDesc& pd = bu::kDescriptor.params[gain];
	const double base = innerEngine->fieldAt(busLane, gain);

	rig.synth.noteOn(0, 60, 1, 0.8);
	int moving = 0;
	float prev = innerEngine->outputAt(envLane, 0);
	for (int i = 0; i < 16; ++i) {
		rig.block();
		const float cur = innerEngine->outputAt(envLane, 0);
		const float* w = innerEngine->controlWindowForTest(busLane, gain);
		REQUIRE(w != nullptr);
		if (cur != prev) {
			++moving;
			CAPTURE(i);
			CHECK_FALSE(feathers::device::tickWindowFlat(w));
			const double expected = std::clamp(base + kAmount * static_cast<double>(cur) * (pd.max - pd.min), pd.min, pd.max);
			CHECK(w[63] == doctest::Approx(expected).epsilon(1e-4));
		}
		prev = cur;
	}
	REQUIRE(moving >= 3);
	CHECK(bu::windowedCallsForTest(innerEngine->storageForTest(busLane)) > 0u);
}

//------------------------------------------------------------------------------------------------//
// T1 — THE SELF-ROUTE REFUSAL (bolt-on chains, backlog FZ).                                       //
//------------------------------------------------------------------------------------------------//
//
// The claim, made by READING three files: `RackEngine::upstreamOf(self, s)` ends in
// `!graph_.wouldCycle(s, Destination::control(self, 0))`; `wouldCycle` is
// `reaches(precedenceLaneOf(d), precedenceLaneOf(s))`; and `reaches` answers TRUE when `from == to`,
// deliberately (*"a device is not upstream of itself"*). Asked with `s = Source::output(self, …)`
// both ends name the SAME DEVICE, so the gate refuses a holder's own output into its own inner chain.
//
// THESE CASES ASSERT THE BOOLEAN `upstreamOf` RETURNS, not what a surface draws with it — the value
// rule (CLAUDE.md). `precedenceLaneOf` is private to `Graph` and no accessor reaches it; what is
// public is `slotOf`, which IS `graph_.laneOf`, and the two branch conditions the private function
// tests (`Source::isDevice`/`isOuter`, `Destination::isDevice`). Those are asserted instead, and they
// pin both lanes to one number without a new seam.
//
// **SKLITTER IS THE DEVICE T1 NAMES, AND IT ALREADY HAS THE OUTPUTS** — seven of them (T7). It is not
// a container today; `upstreamOf` never reads `Descriptor::container`, so the question it answers for
// SKLITTER-as-a-device is the question it will answer for SKLITTER-as-a-holder.

TEST_CASE("T1: upstreamOf refuses a device's own output — every one of them") {
	RackEngine outer { rack::kRackAxes, hostCatalogue() };
	outer.prepare(48000.0, 4096);
	const size_t lane = outer.addDevice("SKLITTER");
	REQUIRE(lane != RackEngine::kNoSlot);
	const DeviceInstanceId self = test::idAt(outer, lane);
	REQUIRE(self != kNoInstance);
	const device::Descriptor* desc = outer.slotAtLane(lane).live_desc();
	REQUIRE(desc != nullptr);
	REQUIRE(desc->outputs.size() > 0); // a device with no outputs cannot ask this question at all

	// THE TWO LANES, as far as a public seam can read them: both ends name `self`, `slotOf` is
	// `laneOf`, and both branch conditions in `precedenceLaneOf` take the device road.
	CHECK(outer.slotOf(self) == lane);
	CHECK(backplane::Destination::control(self, 0).isDevice());

	for (size_t out = 0; out < desc->outputs.size(); ++out) {
		CAPTURE(out);
		const backplane::Source own = backplane::Source::output(self, out);
		CHECK(own.isDevice());
		CHECK_FALSE(own.isOuter());
		// NOT REFUSED BY THE EARLIER GUARD: the engine really publishes this cell, so the answer
		// below is the cycle rule's and nothing else's.
		REQUIRE(outer.sourceCell(own) != nullptr);
		CHECK_FALSE(outer.upstreamOf(self, own)); // T1: the value.
	}
}

TEST_CASE("HS beside T1: a declared self-route is about the holder's CONTROLS and never answers for the chain inside it") {
	// **ADDED BY BACKLOG HS, WITH THE T1 CASES AROUND IT UNTOUCHED.** SKLITTER now declares which of its
	// own controls its outputs are computed from (`device::SelfRouting`), so `wouldCycle(output(self, n),
	// control(self, k))` is FALSE for a control outside that set — and `upstreamOf` spells "the chain" as
	// `control(self, 0)`, which for SKLITTER is Zone A: outside the set. Left alone, the case above would
	// have gone red through a control index its own banner says *"is not consulted"*.
	//
	// So `upstreamOf` (and `backplane::sourceUpstreamOf`) now refuse `s.device == self` OUTRIGHT, one line
	// before the cycle test. **That supersedes one sentence in the case above** — "the answer below is the
	// cycle rule's and nothing else's" — for a device that declares; the ANSWER it pins is unchanged, and
	// for a device that declares nothing both lines still say no. This case pins that the two questions
	// have come apart, in both directions, on the device where they do.
	RackEngine outer { rack::kRackAxes, hostCatalogue() };
	outer.prepare(48000.0, 4096);
	const size_t lane = outer.addDevice("SKLITTER");
	REQUIRE(lane != RackEngine::kNoSlot);
	const DeviceInstanceId self = test::idAt(outer, lane);
	const device::Descriptor* desc = outer.slotAtLane(lane).live_desc();
	REQUIRE(desc != nullptr);
	REQUIRE(desc->selfRouting != nullptr);
	REQUIRE(desc->params[0].modulatable);
	REQUIRE(desc->mayRouteToSelf(0)); // control 0 IS one the declaration lets through — the trap's premise

	for (size_t out = 0; out < desc->outputs.size(); ++out) {
		CAPTURE(out);
		const backplane::Source own = backplane::Source::output(self, out);
		CHECK(outer.mayRoute(own, backplane::Destination::control(self, 0))); // the holder's own control: a road
		CHECK_FALSE(outer.upstreamOf(self, own));                             // the chain inside it: still not
	}
}

TEST_CASE("T1: a SIBLING earlier in the chain IS upstream — and since IZ one later is too, unless the holder ticks") {
	// THE CONTRAST THAT MAKES THE REFUSAL ABOVE MEAN SOMETHING. The same call, the same code path,
	// a different device at the source end — so a blanket `false` is ruled out as the explanation.
	RackEngine outer { rack::kRackAxes, hostCatalogue() };
	outer.prepare(48000.0, 4096);
	const size_t earlyLane = outer.addDevice("SKLITTER");
	const size_t holderLane = outer.addDevice(kRack.c_str());
	const size_t lateLane = outer.addDevice("SKLITTER");
	REQUIRE(earlyLane != RackEngine::kNoSlot);
	REQUIRE(holderLane != RackEngine::kNoSlot);
	REQUIRE(lateLane != RackEngine::kNoSlot);
	REQUIRE(tagsIn(outer) == std::vector<std::string> { "SKLITTER", kRack, "SKLITTER" });

	const DeviceInstanceId holder = test::idAt(outer, holderLane);
	const backplane::Source early = backplane::Source::output(test::idAt(outer, earlyLane), 0);
	const backplane::Source late = backplane::Source::output(test::idAt(outer, lateLane), 0);
	REQUIRE(outer.sourceCell(early) != nullptr);
	REQUIRE(outer.sourceCell(late) != nullptr);

	CHECK(outer.upstreamOf(holder, early)); // a sibling before it: a road
	// **A SIBLING AFTER IT IS A ROAD TOO, SINCE BACKLOG IZ.** This said "that one really is a cycle", and
	// it is not: the later SKLITTER publishes in the modulators' pass, and the rack's chain reads it when
	// the rack RUNS, after every tick. "Downstream" means computed from downstream
	// audio. The refusal that survives is the subcase below.
	CHECK(outer.upstreamOf(holder, late));
	// AND A CORE SOURCE IS UPSTREAM OF EVERYTHING — the third road out of `upstreamOf`, pinned so a
	// change that made the function answer `false` everywhere would not read as T1.
	CHECK(outer.upstreamOf(holder, backplane::Source::core(0)));

	SUBCASE("a HOLDER THAT TICKS — SKLITTER holding its lanes — refuses the later sibling: one order cannot hold both") {
		// SKLITTER is ticked in the modulators' pass and run in the audio pass, so a later SKLITTER's
		// output into it would need the later one ticked first AND run second, in one order. The
		// contrast that keeps a blanket `true` from reading as IZ.
		RackEngine rack { rack::kRackAxes, hostCatalogue() };
		rack.prepare(48000.0, 4096);
		const size_t first = rack.addDevice("SKLITTER");
		const size_t held = rack.addDevice("SKLITTER");
		const size_t last = rack.addDevice("SKLITTER");
		REQUIRE(first != RackEngine::kNoSlot);
		REQUIRE(held != RackEngine::kNoSlot);
		REQUIRE(last != RackEngine::kNoSlot);
		const DeviceInstanceId self = test::idAt(rack, held);
		CHECK(rack.upstreamOf(self, backplane::Source::output(test::idAt(rack, first), 0)));
		CHECK_FALSE(rack.upstreamOf(self, backplane::Source::output(test::idAt(rack, last), 0)));
	}
}

TEST_CASE("T1: today's only container publishes no outputs, so the question stops a guard earlier") {
	// **THE RACK DEVICE IS THE ONLY DESCRIPTOR IN THE TREE WITH A NON-NULL `container`, AND ITS
	// `.outputs` IS `{}`.** So for the container that exists today `upstreamOf` refuses a self-output
	// at `sourceCell(s) == nullptr` — "not a source this engine publishes" — and never reaches the
	// cycle rule. The refusal T1 describes is real for it; the MECHANISM T1 names is the case above.
	REQUIRE(rack::kDeviceDescriptor.container != nullptr);
	REQUIRE(rack::kDeviceDescriptor.outputs.size() == 0);

	RackEngine outer { rack::kRackAxes, hostCatalogue() };
	outer.prepare(48000.0, 4096);
	const size_t lane = outer.addDevice(kRack.c_str());
	REQUIRE(lane != RackEngine::kNoSlot);
	const DeviceInstanceId holder = test::idAt(outer, lane);
	const backplane::Source own = backplane::Source::output(holder, 0);

	CHECK(outer.sourceCell(own) == nullptr);
	CHECK_FALSE(outer.upstreamOf(holder, own));
}

TEST_CASE("backlog IZ, NESTED: inside a rack device, SKLITTER reaches a filter BEFORE it and an LFO reaches one to its LEFT — the values, in the block") {
	// For modulation, "downstream" means COMPUTED FROM DOWNSTREAM AUDIO; a modulator's position in a
	// lane is only grouping. The chain inside a rack device is a graph of its own, asked by
	// its own record — the same rule, one level in, through the road a hand's edit takes (the chain's
	// bridge, drained as the device's process would drain it).
	namespace sk = fx::sklitter;
	const device::AudioOps& ops = rack::kDeviceAudioOps;
	std::vector<unsigned char> raw(ops.stateBytes + ops.stateAlign);
	void* mem = raw.data();
	ops.construct(mem);
	ops.prepare(mem, 48000.0, 4096);
	rack::kDeviceContainerOps.adopt(mem, hostCatalogue());
	RackEngine& inner = rack::engineForTest(mem);
	rack::RackBridge* bridge = rack::bridgeOf(mem);
	REQUIRE(bridge != nullptr);

	const DeviceInstanceId filterId = bridge->addDevice("flt1");
	const DeviceInstanceId skId = bridge->addDevice("SKLITTER");
	const DeviceInstanceId lfo1 = bridge->addDevice(mod::lfo::kTraits.tag);
	const DeviceInstanceId lfo2 = bridge->addDevice(mod::lfo::kTraits.tag);
	REQUIRE(filterId != kNoInstance);
	REQUIRE(skId != kNoInstance);
	REQUIRE(lfo1 != kNoInstance);
	REQUIRE(lfo2 != kNoInstance);

	const device::Descriptor* filt = deviceForTag("flt1");
	const size_t cutoff = controlNamed(*filt, "Filter Cutoff");
	const size_t skew = controlNamed(mod::lfo::kDescriptor, "Skew Pre-Peak");
	const backplane::Source perfX = backplane::Source::output(skId, sk::outIndex(sk::Out::X));
	const backplane::Destination intoFilter = backplane::Destination::control(filterId, cutoff);
	const backplane::Source fromLfo2 = backplane::Source::output(lfo2, 0);
	const backplane::Destination intoLfo1 = backplane::Destination::control(lfo1, skew);

	// THE GATE, ONE LEVEL IN — both backwards along the chain, both a road.
	CHECK(bridge->mayRoute(perfX, intoFilter));
	CHECK(bridge->mayRoute(fromLfo2, intoLfo1));
	// THE LOOP BACK IS OPEN UNTIL THE ROUTE EXISTS — nothing orders two unrouted modulators…
	CHECK(bridge->mayRoute(backplane::Source::output(lfo1, 0), backplane::Destination::control(lfo2, skew)));
	bridge->setRouteAmount(perfX, intoFilter, 0.5f);
	constexpr float kAmount = 0.25f;
	bridge->setRouteAmount(fromLfo2, intoLfo1, kAmount);
	// LFO 2 FREE AND FAST, so consecutive blocks differ; its smoothing at the bottom.
	const auto setLfo2 = [&](const char* name, double value) {
		const size_t c = controlNamed(mod::lfo::kDescriptor, name);
		bridge->setField(lfo2, c, static_cast<float>(std::max(value, mod::lfo::kDescriptor.params[c].min)));
	};
	setLfo2("Mode", static_cast<double>(mod::Mode::Free));
	setLfo2("Rate", -4.0);
	setLfo2("Smoothing", -100.0);
	bridge->setField(skId, static_cast<size_t>(sk::Param::PerfX), 0.6f);
	bridge->drain();
	CHECK(bridge->routeAmount(perfX, intoFilter) == 0.5f);
	CHECK(bridge->routeAmount(fromLfo2, intoLfo1) == kAmount);
	// …AND CLOSED ONCE IT DOES: LFO 1 into LFO 2 would now be read before it is written.
	CHECK_FALSE(bridge->mayRoute(backplane::Source::output(lfo1, 0), backplane::Destination::control(lfo2, skew)));

	const size_t filterSlot = inner.slotOf(filterId);
	const size_t skSlot = inner.slotOf(skId);
	const size_t lfo1Slot = inner.slotOf(lfo1);
	const size_t lfo2Slot = inner.slotOf(lfo2);
	REQUIRE(filterSlot != RackEngine::kNoSlot);
	REQUIRE(skSlot != RackEngine::kNoSlot);
	// THE AUDIO ORDER IS THE CHAIN'S — the filter still runs before SKLITTER.
	CHECK(inner.positionOfLane(filterSlot) < inner.positionOfLane(skSlot));

	std::vector<float> row;
	for (const device::ParamDesc& p : composite::kParamDescs) {
		row.push_back(static_cast<float>(p.def));
	}
	ops.setFields(mem, row.data(), row.size());
	const float base = inner.fieldAt(filterSlot, cutoff);
	const device::ParamDesc& cp = filt->params[cutoff];
	const device::ParamDesc& sp = mod::lfo::kDescriptor.params[skew];
	const float skewBase = inner.fieldAt(lfo1Slot, skew);
	std::vector<float> l(64, 0.1f);
	std::vector<float> r(64, 0.1f);
	size_t distinguished = 0;
	for (int b = 0; b < 16; ++b) {
		const float was = inner.publishedOutputAt(lfo2Slot, 0);
		ops.processStereo(mem, l.data(), r.data(), 64);
		const float now = inner.publishedOutputAt(lfo2Slot, 0);
		CAPTURE(b);
		REQUIRE(inner.publishedOutputAt(skSlot, sk::outIndex(sk::Out::X)) == doctest::Approx(0.6f));
		CHECK(inner.resolvedFieldForTest(filterSlot, cutoff)
		      == doctest::Approx(static_cast<double>(base) + 0.5 * 0.6 * device::routeSpanOf(cp)));
		const auto expected = [&](float source) {
			return std::clamp(static_cast<double>(skewBase) + kAmount * source * (sp.max - sp.min), sp.min, sp.max);
		};
		CHECK(inner.resolvedFieldForTest(lfo1Slot, skew) == doctest::Approx(expected(now)));
		distinguished += std::abs(expected(now) - expected(was)) > 2e-5 ? 1 : 0;
	}
	CHECK(distinguished >= 8); // not vacuous: last block's LFO 2 would have failed the check above
	ops.destruct(mem);
}

//--------------------------------------------------------------------------------------------------//
// THE 2026-09-24 CRUSH, ONE LEVEL IN. A host that cuts one call into pieces must hand each piece its  //
// own song position (`RackEngine::setClock`). Feathers' lane did not (fixed in `Synth::render`); the  //
// two nested hosts — a rack device and SKLITTER's own inner chains — cut at the control tick and had  //
// the same shape: every piece of a call was told the call's first sample. A device that seats itself //
// on the song (SKLITTER's write head, backlog IN) then wrote the same piece of its ring once per piece //
// and left the rest stale. Asserted on the RING, which is the value: after a tone and then silence,  //
// the ring's live span must hold exactly the silence.                                                 //
//--------------------------------------------------------------------------------------------------//

namespace {

namespace sk = fx::sklitter;

/// The peak of `s`'s ring over its live span — the frames a read can reach.
float livePeak(const sk::Sklitter& s) {
	const sk::Ring& ring = s.ringForTest();
	const float* d = ring.dataForTest();
	float peak = 0.0f;
	for (size_t i = 0; d != nullptr && i < ring.length() * 2; ++i) {
		peak = std::max(peak, std::fabs(d[i]));
	}
	return peak;
}

/// `callFrames`-frame calls with a playing 120 BPM transport stamped at each call's first sample, as a host
/// does. At 64 a call is one control tick and the host under test runs it as one piece; at 256 it cuts
/// every call into four, which is the road under test.
template <typename Process>
void playCalls(Process&& process, double seconds, float amplitude, double& beats, int callFrames) {
	std::vector<float> l(callFrames);
	std::vector<float> r(callFrames);
	const auto calls = static_cast<int>(seconds * 48000.0 / callFrames);
	for (int c = 0; c < calls; ++c) {
		for (int i = 0; i < callFrames; ++i) {
			l[static_cast<size_t>(i)] = amplitude * std::sin(0.058f * static_cast<float>(c * callFrames + i));
			r[static_cast<size_t>(i)] = l[static_cast<size_t>(i)];
		}
		device::TransportClock clock = device::TransportClock::fromSecondsPerBeat(0.5);
		clock.playing = true;
		clock.songPositionBeats = beats;
		clock.songPositionSeconds = beats * 0.5;
		process(clock, l.data(), r.data(), callFrames);
		beats += callFrames / 24000.0;
	}
}

} // namespace

TEST_CASE("the crush, one level in: SKLITTER inside a RACK DEVICE fed calls longer than a tick records every frame — silence after a tone leaves an empty ring") {
	const device::AudioOps& ops = rack::kDeviceAudioOps;
	std::vector<unsigned char> raw(ops.stateBytes + ops.stateAlign);
	void* mem = raw.data();
	ops.construct(mem);
	ops.prepare(mem, 48000.0, 4096);
	rack::kDeviceContainerOps.adopt(mem, hostCatalogue());
	RackEngine& inner = rack::engineForTest(mem);
	rack::RackBridge* bridge = rack::bridgeOf(mem);
	REQUIRE(bridge != nullptr);
	const DeviceInstanceId skId = bridge->addDevice("SKLITTER");
	REQUIRE(skId != kNoInstance);
	bridge->drain();
	const auto* s = reinterpret_cast<const sk::Sklitter*>(inner.slotAtLane(inner.slotOf(skId)).storage.get());
	REQUIRE(s != nullptr);

	double beats = 0.0;
	const auto process = [&](const device::TransportClock& clock, float* l, float* r, int n) {
		ops.setTransportClock(mem, clock);
		ops.processStereo(mem, l, r, n);
	};
	// THE TONE IN ONE-TICK CALLS, so every ring frame holds it whatever the host does with a piece; the
	// SILENCE in four-tick calls, the road under test. A host that told each piece the call's first sample
	// rewrites the first quarter of every call and leaves three quarters of the tone standing.
	playCalls(process, 3.0, 0.5f, beats, 64); // disengaged: the ring listens to the tone
	REQUIRE(livePeak(*s) > 0.1f);         // NOT VACUOUS: it heard it
	playCalls(process, 3.0, 0.0f, beats, 256); // and then to more than a bar of silence
	CHECK(livePeak(*s) == 0.0f);
	ops.destruct(mem);
}

TEST_CASE("the crush, one level in: SKLITTER inside SKLITTER's MASTER LANE fed calls longer than a tick records every frame — silence after a tone leaves an empty ring") {
	// The outer device ENGAGED at Density 0 plays the input as its wet (every slice dry), so the master
	// lane — an inner chain — is handed the input, and the inner SKLITTER, disengaged, listens to it.
	auto outer = std::make_unique<sk::Sklitter>();
	outer->prepare(48000.0, 4096);
	sk::kDescriptor.container->adopt(outer.get(), hostCatalogue());
	rack::InnerChain* lane = outer->postChain();
	REQUIRE(lane != nullptr);
	const DeviceInstanceId innerId = lane->bridge()->addDevice("SKLITTER");
	REQUIRE(innerId != kNoInstance);
	lane->bridge()->drain();
	sk::Settings cfg;
	cfg.engage = true;
	cfg.density = 0.0f;
	outer->setTarget(cfg);
	double beats = 0.0;
	const auto process = [&](const device::TransportClock& clock, float* l, float* r, int n) {
		outer->setTransportClock(clock);
		outer->processStereo(l, r, n);
	};
	const auto innerDevice = [&]() {
		const RackEngine& e = lane->engine();
		return reinterpret_cast<const sk::Sklitter*>(e.slotAtLane(e.slotOf(innerId)).storage.get());
	};
	playCalls(process, 5.0, 0.5f, beats, 64); // one-tick calls: every ring frame holds the tone
	REQUIRE(innerDevice() != nullptr);
	REQUIRE(livePeak(*innerDevice()) > 0.1f); // NOT VACUOUS: the inner ring heard the tone
	playCalls(process, 3.0, 0.0f, beats, 256); // four-tick calls: the road under test
	CHECK(livePeak(*innerDevice()) == 0.0f);
}

//------------------------------------------------------------------------------------//
// HOST SLOTS T11 — A CONTROL IN A NESTED RACK TAKES ITS SLOT FROM THE OUTERMOST PLUGIN   //
// (host slots, T11). One pool, the outermost                                             //
// plugin's; a target is a PATH [outer device … nested device] and a key; values and      //
// gestures travel the path; the nested rack's own body stores no slots.                  //
//------------------------------------------------------------------------------------//

namespace {

namespace hs = feathers::hostslots;
using SlotState = hs::SlotState;
using Status = device::HostSlotStatus::State;
const std::string kDot = "\xc2\xb7"; // " · ", the separator every slot label uses

struct SlotEvent {
	hs::SlotEventKind kind;
	uint16_t slot;
	float value;
};

/// EVERYTHING THE OUTBOX HOLDS, in order — what the plugin would hand the host at its next block.
std::vector<SlotEvent> drainEvents(hs::SlotOutbox<>& outbox) {
	std::vector<SlotEvent> out;
	outbox.drain([&out](hs::SlotEventKind kind, uint16_t slot, float value) noexcept {
		try {
			out.push_back({ kind, slot, value });
		}
		catch (...) {
		}
		return true;
	});
	return out;
}

const device::ParamDesc& delayMix() {
	return fx::delay::kDescriptor.params[controlNamed(fx::delay::kDescriptor, "Delay Mix")];
}
size_t delayMixIndex() { return controlNamed(fx::delay::kDescriptor, "Delay Mix"); }

/// WHAT THE ENGINE RUNS FOR A HOST WRITE OF `normal` — restated from the declaration: denormalised, then
/// coerced as `RackEngine::applyHostSlots` coerces.
float hostValueOf(const device::ParamDesc& desc, double normal) {
	return device::coerceControl(desc, static_cast<float>(device::fromNormal(desc, normal)));
}

std::string fileOf(const rack::RackPatch& patch) {
	ByteStream file;
	clap_ostream out = file.out();
	REQUIRE(rack::writeRack(&out, patch, false));
	return std::string(file.bytes.begin(), file.bytes.end());
}

/// **FEATHERS, WITH ITS POOL WIRED AS THE PLUGIN WIRES IT** (`feathers_clap.cpp`): the lane reads the bridge's
/// bus, and a chain in the lane is handed the bridge's slot link beside its gate. `visit` is the plugin's
/// `serviceSlots`: the pool's service, then every chain's (`Synth::serviceContainers`).
struct SlotNestRig {
	ParamSet params;
	Synth synth { params };
	ParamBridge bridge;
	std::vector<float> l = std::vector<float>(64, 0.0f);
	std::vector<float> r = std::vector<float>(64, 0.0f);

	SlotNestRig() {
		synth.activate(48000.0, 4096);
		params.setBase(static_cast<clap_id>(ParamId::VoiceLevel), 1.0);
		synth.setCatalogue(hostCatalogue());
		synth.setHostSlotBus(&bridge.slotBus());
		synth.setOuterGateMaker(&rack::ChainRecord::gateOver, &bridge.chainRecord());
		synth.setSlotLinkMaker(&ParamBridge::slotLinkOver, &bridge);
	}
	DeviceInstanceId addChain() {
		const size_t slot = bridge.addFxDevice(composite::kTraits.tag);
		REQUIRE(slot < kMaxFxSlots);
		bridge.refresh();
		const DeviceInstanceId id = bridge.fxDeviceAt(slot);
		REQUIRE(id != kNoInstance);
		provision();
		block();
		return id;
	}
	rack::RackBridge& inner(DeviceInstanceId chain) {
		rack::RackBridge* b = rack::bridgeOf(bridge.fxBodies().surfaceOf(chain));
		REQUIRE(b != nullptr);
		return *b;
	}
	RackEngine& innerEngine(DeviceInstanceId chain) {
		RackEngine* e = rack::engineOf(bridge.fxBodies().surfaceOf(chain));
		REQUIRE(e != nullptr);
		return *e;
	}
	DeviceInstanceId addInside(DeviceInstanceId chain, const char* tag) {
		const DeviceInstanceId id = inner(chain).addDevice(tag);
		REQUIRE(id != kNoInstance);
		provision();
		block();
		return id;
	}
	void provision() {
		publishEditorLanes(bridge, params, synth);
		(void)synth.provisionDevices(bridge.chainState(), bridge.publishedChains().sequence(), &bridge.fxBodies());
	}
	/// The pool's half of the plugin's visit alone — the declarations it may hand the audio thread.
	void ownerVisit() {
		(void)bridge.reconcileSlotsIfMoved();
		(void)bridge.serviceSlotRoads();
	}
	void visit() {
		provision();
		ownerVisit();
		synth.serviceContainers(bridge.chainState(), &bridge.fxBodies());
	}
	void block() { synth.render(l.data(), r.data(), 64); }
	void settle() {
		for (int i = 0; i < 3; ++i) {
			visit();
			block();
		}
	}
	const hs::SlotPool& pool() const { return bridge.slotPool(); }
	std::string chainName(DeviceInstanceId chain) const {
		// NUMBERED ONLY BESIDE A TWIN, by its place in the lane (`SlotOwner::labelOf`).
		size_t chains = 0;
		for (size_t pos = 0; pos < bridge.chainRecord().length(kLaneChain); ++pos) {
			const device::Descriptor* d = bridge.chainRecord().descriptorOf(bridge.chainRecord().deviceAt(kLaneChain, pos));
			if (d != nullptr && device::sameTag(d->traits.tag, composite::kTraits.tag)) {
				++chains;
			}
		}
		return chains > 1 ? "Chain " + std::to_string(bridge.chainRecord().positionOf(chain) + 1) : std::string("Chain");
	}
};

/// **THE STANDALONE RACK, OWNING ITS POOL** (T10): the top bridge is the owner; `visit` is the plugin's
/// `serviceHostSlots`, which services the chains it holds too.
struct StandaloneSlotRig {
	RackEngine engine { rack::kRackAxes, hostCatalogue() };
	std::unique_ptr<rack::RackBridge> top;
	std::vector<float> l = std::vector<float>(64, 0.0f);
	std::vector<float> r = std::vector<float>(64, 0.0f);

	StandaloneSlotRig() {
		engine.prepare(48000.0, 4096);
		top = std::make_unique<rack::RackBridge>(engine, nullptr, rack::RackBridge::HostSlots::OwnPool);
	}
	DeviceInstanceId addRack() {
		const DeviceInstanceId id = top->addDevice(kRack.c_str());
		REQUIRE(id != kNoInstance);
		block();
		return id;
	}
	rack::RackBridge& inner(DeviceInstanceId rackId) {
		rack::RackBridge* b = rack::bridgeOf(top->containerSurfaceOf(rackId));
		REQUIRE(b != nullptr);
		return *b;
	}
	RackEngine& innerEngine(DeviceInstanceId rackId) {
		RackEngine* e = rack::engineOf(top->containerSurfaceOf(rackId));
		REQUIRE(e != nullptr);
		return *e;
	}
	DeviceInstanceId addInside(DeviceInstanceId rackId, const char* tag) {
		const DeviceInstanceId id = inner(rackId).addDevice(tag);
		REQUIRE(id != kNoInstance);
		block();
		return id;
	}
	rack::SlotOwner& owner() { return *top->hostSlots(); }
	void visit() { (void)top->serviceHostSlots(); }
	void block() {
		top->drain();
		std::fill(l.begin(), l.end(), 0.1f);
		std::fill(r.begin(), r.end(), 0.1f);
		engine.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
		top->publish(engine);
	}
	void settle() {
		for (int i = 0; i < 3; ++i) {
			visit();
			block();
		}
	}
};

} // namespace

TEST_CASE("T11: a knob in a nested rack gets slot 0 from Feathers named 'Chain · Delay · Mix'") {
	SlotNestRig rig;
	const DeviceInstanceId chain = rig.addChain();
	const DeviceInstanceId delay = rig.addInside(chain, fx::delay::kTraits.tag);
	const size_t mix = delayMixIndex();
	rack::RackBridge& inner = rig.inner(chain);
	// THE CHAIN HAS A LINK AND ITS CONTROL NO SLOT YET: the right-click offers "Expose to host".
	REQUIRE(inner.nestedSlots() != nullptr);
	REQUIRE(inner.nestedSlots()->linked());
	const device::HostSlotStatus before = inner.hostSlotOf(delay, mix);
	CHECK(before.state == Status::Unslotted);
	CHECK(before.canExpose);
	(void)rig.bridge.takeStateDirty();

	std::string displaced = "untouched";
	REQUIRE(inner.exposeToHost(delay, mix, &displaced));
	CHECK(displaced.empty());
	// ONE POOL, FEATHERS': slot 0, by PATH [the chain in the lane, the delay inside it] and the control's key.
	CHECK(rig.pool().state(0) == SlotState::Pinned);
	CHECK(rig.pool().target(0).path == std::vector<DeviceInstanceId> { chain, delay });
	CHECK(rig.pool().target(0).key == "delay_mix");
	CHECK(rig.pool().target(0).tag == fx::delay::kTraits.tag);
	CHECK(rig.pool().target(0).label == "Chain " + kDot + " Delay " + kDot + " Mix");
	// THE KNOB ASKS AND IS ANSWERED BY THE SAME POOL; and FEATHERS' patch is the one marked (the pool is saved there).
	const device::HostSlotStatus after = inner.hostSlotOf(delay, mix);
	CHECK(after.state == Status::Pinned);
	CHECK(after.slot == 0u);
	CHECK(rig.bridge.takeStateDirty());
	CHECK(inner.hostSlotGeneration() == rig.pool().generation());
	// THE HOST'S LOOKUP resolves the path to the delay's own declaration (its name and value text).
	const device::ParamDesc* desc = nullptr;
	double value = -1.0;
	REQUIRE(rig.bridge.resolveSlotTarget(rig.pool().target(0), &desc, &value));
	CHECK(desc == &delayMix());
	// THE CHAIN'S OWN CONTROLS ARE OTHER CONTROLS — one level up, path `[chain]` — and have no slot.
	CHECK(rig.bridge.hostSlotStatusOf(chain, static_cast<size_t>(composite::Param::Mix)).state == Status::Unslotted);
	// UNPIN GOES UP THE SAME ROAD.
	CHECK(inner.unpinFromHost(delay, mix));
	CHECK(rig.pool().state(0) == SlotState::Recent);
}

TEST_CASE("T11: its drag reaches the host on that slot") {
	SlotNestRig rig;
	const DeviceInstanceId chain = rig.addChain();
	const DeviceInstanceId delay = rig.addInside(chain, fx::delay::kTraits.tag);
	const size_t mix = delayMixIndex();
	rack::RackBridge& inner = rig.inner(chain);
	REQUIRE(inner.exposeToHost(delay, mix, nullptr));
	rig.settle();
	// BOUND IN THE CHAIN'S OWN RECORD, on slot 0 — which is what its engine applies and its hand tells.
	REQUIRE(inner.recordForTest().state().slotBindings()[0].live);
	CHECK(inner.recordForTest().state().slotBindings()[0].device == delay);
	(void)drainEvents(rig.bridge.slotOutbox());

	const float a = 0.2f;
	const float b = 0.65f;
	inner.beginControlGesture(delay, mix);
	inner.setField(delay, mix, a);
	inner.setField(delay, mix, b);
	inner.endControlGesture(delay, mix);
	const std::vector<SlotEvent> ev = drainEvents(rig.bridge.slotOutbox());
	REQUIRE(ev.size() == 4);
	CHECK(ev[0].kind == hs::SlotEventKind::Begin);
	CHECK(ev[1].kind == hs::SlotEventKind::Value);
	CHECK(ev[2].kind == hs::SlotEventKind::Value);
	CHECK(ev[3].kind == hs::SlotEventKind::End);
	for (const SlotEvent& e : ev) {
		CHECK(e.slot == 0);
	}
	// THE STORED VALUES' POSITIONS, restated from the declaration.
	CHECK(ev[1].value == static_cast<float>(device::toNormal(delayMix(), device::coerceControl(delayMix(), a))));
	CHECK(ev[2].value == static_cast<float>(device::toNormal(delayMix(), device::coerceControl(delayMix(), b))));
	// AND THE BUS WAS SEEDED, so the chain's engine runs the hand's value, not the host's last one.
	rig.block();
	RackEngine& engine = rig.innerEngine(chain);
	CHECK(engine.fieldAt(engine.slotOf(delay), mix) == device::coerceControl(delayMix(), b));

	// A DRAG WHILE THE HOST RECORDS ANCHORS THE SLOT (design §5), from inside the rack too.
	CHECK(rig.pool().state(0) == SlotState::Pinned);
	rig.bridge.setHostRecording(true);
	inner.beginControlGesture(delay, mix);
	inner.setField(delay, mix, 0.4f);
	inner.endControlGesture(delay, mix);
	rig.visit();
	CHECK(rig.pool().state(0) == SlotState::Anchored);
}

TEST_CASE("T9 x T11: a hand on a nested knob gets a Recent slot from Feathers' pool, and its next drag reaches the host") {
	SlotNestRig rig;
	const DeviceInstanceId chain = rig.addChain();
	const DeviceInstanceId delay = rig.addInside(chain, fx::delay::kTraits.tag);
	const size_t mix = delayMixIndex();
	rack::RackBridge& inner = rig.inner(chain);
	rig.settle();
	hs::Target target;
	target.path = { chain, delay };
	target.key = "delay_mix";
	REQUIRE(rig.pool().slotOf(target) == hs::kNoSlot);
	(void)rig.bridge.takeStateDirty();

	// THE HAND'S ROAD, as `DeviceControl::beginHostGesture` runs it: the touch, then the Begin.
	inner.touchControl(delay, mix);
	inner.beginControlGesture(delay, mix);
	inner.setField(delay, mix, 0.3f);
	inner.endControlGesture(delay, mix);
	const hs::SlotIndex n = rig.pool().slotOf(target);
	REQUIRE(n != hs::kNoSlot);
	CHECK(rig.pool().state(n) == SlotState::Recent);
	CHECK(rig.pool().target(n).label == "Chain " + kDot + " Delay " + kDot + " Mix");
	CHECK(rig.bridge.takeStateDirty()); // Feathers' patch: the pool is saved there
	// A SECOND TOUCH allocates nothing.
	const uint64_t gen = rig.pool().generation();
	inner.touchControl(delay, mix);
	CHECK(rig.pool().generation() == gen);

	// BOUND AT THE NEXT VISITS; THEN AN ORDINARY DRAG RECORDS ON THE SLOT.
	rig.settle();
	(void)drainEvents(rig.bridge.slotOutbox());
	inner.touchControl(delay, mix);
	inner.beginControlGesture(delay, mix);
	inner.setField(delay, mix, 0.7f);
	inner.endControlGesture(delay, mix);
	const std::vector<SlotEvent> ev = drainEvents(rig.bridge.slotOutbox());
	REQUIRE(ev.size() == 3);
	CHECK(ev[0].kind == hs::SlotEventKind::Begin);
	CHECK(ev[1].kind == hs::SlotEventKind::Value);
	CHECK(ev[1].value == static_cast<float>(device::toNormal(delayMix(), device::coerceControl(delayMix(), 0.7f))));
	CHECK(ev[2].kind == hs::SlotEventKind::End);
	for (const SlotEvent& e : ev) {
		CHECK(e.slot == n);
	}
}

TEST_CASE("T11: host automation moves the nested control in the same block") {
	SlotNestRig rig;
	const DeviceInstanceId chain = rig.addChain();
	const DeviceInstanceId delay = rig.addInside(chain, fx::delay::kTraits.tag);
	const size_t mix = delayMixIndex();
	rack::RackBridge& inner = rig.inner(chain);
	REQUIRE(inner.exposeToHost(delay, mix, nullptr));
	rig.visit();
	rig.block();
	// THE REMAP WINDOW ACROSS LEVELS: the pool's own visit binds the slot and HOLDS its declaration; only once
	// the chain has pulled the binding, seeded the bus from its record and published does the audio thread get
	// it — so a host write is never scaled for one control and applied by an engine to another.
	rig.ownerVisit();
	CHECK(rig.bridge.slotDescOf(0) == &delayMix());
	RackEngine& engine = rig.innerEngine(chain);
	const size_t lane = engine.slotOf(delay);
	REQUIRE(lane != RackEngine::kNoSlot);

	const double normal = 0.8;
	const float expected = hostValueOf(delayMix(), normal);
	REQUIRE(engine.fieldAt(lane, mix) != expected); // NOT VACUOUS: the control does not already stand there
	REQUIRE(rig.bridge.slotOwner().onHostValue(0, normal));
	rig.block();
	// IN THAT BLOCK, no visit between: the chain's engine read the ONE bus itself.
	CHECK(engine.fieldAt(lane, mix) == expected);
	CHECK(engine.modulatedFieldAt(lane, mix) == expected);
	// NOT ANCHORED UNTIL THE VISIT; then the chain's knob (its record) follows, and the host reads it back.
	CHECK(rig.pool().state(0) == SlotState::Pinned);
	rig.visit();
	CHECK(rig.pool().state(0) == SlotState::Anchored);
	CHECK(inner.fieldAt(delay, mix) == expected);
	const device::ParamDesc* desc = nullptr;
	double value = -1.0;
	REQUIRE(rig.bridge.resolveSlotTarget(rig.pool().target(0), &desc, &value));
	CHECK(value == static_cast<double>(expected));
}

TEST_CASE("T11: the pool's declaration for a slot moved into a nested rack waits for the rack to adopt it") {
	SlotNestRig rig;
	const DeviceInstanceId chain = rig.addChain();
	const DeviceInstanceId delay = rig.addInside(chain, fx::delay::kTraits.tag);
	rack::RackBridge& inner = rig.inner(chain);
	REQUIRE(inner.exposeToHost(delay, delayMixIndex(), nullptr));
	rig.provision();
	rig.ownerVisit();
	// HELD: the rack has not pulled the binding yet, so a host write now is dropped rather than applied wrong.
	CHECK(rig.bridge.slotOwner().heldForTest(0));
	CHECK(rig.bridge.slotDescOf(0) == nullptr);
	CHECK_FALSE(rig.bridge.slotOwner().onHostValue(0, 0.5));
	// THE RACK'S SERVICE: it pulls, seeds, publishes and adopts — and the declaration goes over at once.
	rig.synth.serviceContainers(rig.bridge.chainState(), &rig.bridge.fxBodies());
	CHECK_FALSE(rig.bridge.slotOwner().heldForTest(0));
	CHECK(rig.bridge.slotDescOf(0) == &delayMix());
	CHECK(inner.recordForTest().state().slotBindings()[0].live);
}

TEST_CASE("T11: a nested rack's saved body carries no hostSlots") {
	SlotNestRig rig;
	const DeviceInstanceId chain = rig.addChain();
	const DeviceInstanceId delay = rig.addInside(chain, fx::delay::kTraits.tag);
	rack::RackBridge& inner = rig.inner(chain);
	REQUIRE(inner.exposeToHost(delay, delayMixIndex(), nullptr));
	rig.settle();
	// THE RACK'S OWN BODY — what a rack preset moved elsewhere would carry — has no slots at all.
	CHECK_FALSE(inner.patch().hostSlots.has_value());
	CHECK(fileOf(inner.patch()).find("hostSlots") == std::string::npos);
	// THE OUTERMOST PATCH STORES THE SLOT, BY PATH — and the search above is not vacuous: its file names it.
	const rack::RackPatch outer = rig.bridge.chainsPatch();
	REQUIRE(outer.hostSlots.has_value());
	REQUIRE(outer.hostSlots->slots.size() == 1);
	CHECK(outer.hostSlots->slots[0].target.path == std::vector<DeviceInstanceId> { chain, delay });
	CHECK(fileOf(outer).find("hostSlots") != std::string::npos);
}

TEST_CASE("T11: a third-party-hosted rack answers no slot") {
	// A HOST THAT HANDS NO LINK — here a bare engine, the shape a third party's chain has: our rack holds devices
	// and edits them, and its controls simply have no slot. The menu shows no host entry.
	RackEngine outer { rack::kRackAxes, hostCatalogue() };
	outer.prepare(48000.0, 4096);
	const size_t lane = outer.addDevice(kRack.c_str());
	rack::RackBridge& bridge = bridgeIn(outer, lane);
	const DeviceInstanceId delay = addInside(outer, lane, fx::delay::kTraits.tag);
	REQUIRE(bridge.descriptorOf(delay) != nullptr);
	REQUIRE(device::sameTag(bridge.descriptorOf(delay)->traits.tag, fx::delay::kTraits.tag));
	REQUIRE(bridge.nestedSlots() != nullptr);
	CHECK_FALSE(bridge.nestedSlots()->linked());
	CHECK(bridge.hostSlotOf(delay, delayMixIndex()).state == Status::Unavailable);
	std::string displaced;
	CHECK_FALSE(bridge.exposeToHost(delay, delayMixIndex(), &displaced));
	CHECK(bridge.hostSlotGeneration() == 0u);
	CHECK_FALSE(bridge.patch().hostSlots.has_value());
}

TEST_CASE("T11: nested removal frees the slot; the chain's own removal and its undo free and restore it") {
	SlotNestRig rig;
	const DeviceInstanceId chain = rig.addChain();
	const DeviceInstanceId delay = rig.addInside(chain, fx::delay::kTraits.tag);
	rack::RackBridge& inner = rig.inner(chain);
	REQUIRE(inner.exposeToHost(delay, delayMixIndex(), nullptr));
	rig.settle();
	REQUIRE(rig.pool().state(0) == SlotState::Pinned);
	// A DEVICE REMOVED INSIDE THE RACK frees its slot in Feathers' pool, at once.
	inner.removeDevice(delay);
	CHECK(rig.pool().state(0) == SlotState::Empty);
	rig.settle();
	CHECK_FALSE(inner.recordForTest().state().slotBindings()[0].live);

	// THE RACK ITSELF REMOVED FROM FEATHERS' LANE frees every slot under its path; the undo brings them back.
	const DeviceInstanceId second = rig.addInside(chain, fx::delay::kTraits.tag);
	REQUIRE(inner.exposeToHost(second, delayMixIndex(), nullptr));
	rig.settle();
	// WHICHEVER SLOT THE POOL GAVE IT (a freed slot's note is kept for an undo, so it may not be slot 0).
	const hs::SlotIndex n = inner.hostSlotOf(second, delayMixIndex()).slot;
	REQUIRE(n < rig.pool().capacity());
	REQUIRE(rig.pool().state(n) == SlotState::Pinned);
	const size_t position = rig.bridge.chainRecord().positionOf(chain);
	const FxDevice saved = fxDeviceAtPosition(rig.bridge.fxChain(), position);
	REQUIRE(rig.bridge.removeFxDevice(position));
	CHECK(rig.pool().state(n) == SlotState::Empty);
	REQUIRE(rig.bridge.insertFxDevice(position, saved, {}));
	CHECK(rig.pool().state(n) == SlotState::Pinned);
	CHECK(rig.pool().target(n).path == std::vector<DeviceInstanceId> { chain, second });
}

TEST_CASE("T11: two nested racks' devices with the same instance id get distinct slots") {
	SlotNestRig rig;
	const DeviceInstanceId chainA = rig.addChain();
	const DeviceInstanceId chainB = rig.addChain();
	const DeviceInstanceId a = rig.addInside(chainA, fx::delay::kTraits.tag);
	const DeviceInstanceId b = rig.addInside(chainB, fx::delay::kTraits.tag);
	REQUIRE(a == b); // THE HAZARD: each rack issues ids from its own counter, so an id alone is ambiguous
	const size_t mix = delayMixIndex();
	REQUIRE(rig.inner(chainA).exposeToHost(a, mix, nullptr));
	REQUIRE(rig.inner(chainB).exposeToHost(b, mix, nullptr));
	CHECK(rig.pool().state(0) == SlotState::Pinned);
	CHECK(rig.pool().state(1) == SlotState::Pinned);
	CHECK(rig.pool().target(0).path == std::vector<DeviceInstanceId> { chainA, a });
	CHECK(rig.pool().target(1).path == std::vector<DeviceInstanceId> { chainB, b });
	CHECK(rig.pool().target(0).label == rig.chainName(chainA) + " " + kDot + " Delay " + kDot + " Mix");
	CHECK(rig.pool().target(1).label == rig.chainName(chainB) + " " + kDot + " Delay " + kDot + " Mix");
	CHECK(rig.pool().target(0).label != rig.pool().target(1).label);
	CHECK(rig.inner(chainA).hostSlotOf(a, mix).slot == 0u);
	CHECK(rig.inner(chainB).hostSlotOf(b, mix).slot == 1u);

	// AND THE VALUES TRAVEL THE PATH: a host write on B's slot moves B's delay and not A's.
	rig.settle();
	RackEngine& ea = rig.innerEngine(chainA);
	RackEngine& eb = rig.innerEngine(chainB);
	const float aBefore = ea.fieldAt(ea.slotOf(a), mix);
	const float expected = hostValueOf(delayMix(), 0.9);
	REQUIRE(eb.fieldAt(eb.slotOf(b), mix) != expected);
	REQUIRE(rig.bridge.slotOwner().onHostValue(1, 0.9));
	rig.block();
	CHECK(eb.fieldAt(eb.slotOf(b), mix) == expected);
	CHECK(ea.fieldAt(ea.slotOf(a), mix) == aBefore);
}

TEST_CASE("T11, standalone: a knob in a rack nested in the standalone rack gets slot 0 named 'Chain · Delay · Mix', its drag reaches the host on it, and host automation moves it in the same block") {
	StandaloneSlotRig rig;
	const DeviceInstanceId rackId = rig.addRack();
	const DeviceInstanceId delay = rig.addInside(rackId, fx::delay::kTraits.tag);
	const size_t mix = delayMixIndex();
	rack::RackBridge& inner = rig.inner(rackId);
	REQUIRE(inner.nestedSlots()->linked());
	(void)rig.top->takeStateDirty();
	REQUIRE(inner.exposeToHost(delay, mix, nullptr));
	const hs::SlotPool& pool = rig.owner().pool();
	CHECK(pool.state(0) == SlotState::Pinned);
	CHECK(pool.target(0).path == std::vector<DeviceInstanceId> { rackId, delay });
	CHECK(pool.target(0).label == "Chain " + kDot + " Delay " + kDot + " Mix");
	CHECK(inner.hostSlotOf(delay, mix).slot == 0u);
	CHECK(rig.top->takeStateDirty()); // the standalone rack's patch holds the pool
	rig.settle();
	REQUIRE(rig.owner().descOf(0) == &delayMix());

	// THE DRAG.
	(void)drainEvents(rig.owner().outbox());
	inner.beginControlGesture(delay, mix);
	inner.setField(delay, mix, 0.3f);
	inner.endControlGesture(delay, mix);
	const std::vector<SlotEvent> ev = drainEvents(rig.owner().outbox());
	REQUIRE(ev.size() == 3);
	CHECK(ev[0].kind == hs::SlotEventKind::Begin);
	CHECK(ev[1].kind == hs::SlotEventKind::Value);
	CHECK(ev[1].value == static_cast<float>(device::toNormal(delayMix(), device::coerceControl(delayMix(), 0.3f))));
	CHECK(ev[2].kind == hs::SlotEventKind::End);
	CHECK((ev[0].slot == 0 && ev[1].slot == 0 && ev[2].slot == 0));

	// THE AUTOMATION, IN THE SAME BLOCK.
	RackEngine& engine = rig.innerEngine(rackId);
	const size_t lane = engine.slotOf(delay);
	const float expected = hostValueOf(delayMix(), 0.15);
	REQUIRE(engine.fieldAt(lane, mix) != expected);
	REQUIRE(rig.owner().onHostValue(0, 0.15));
	rig.block();
	CHECK(engine.fieldAt(lane, mix) == expected);
	rig.visit();
	CHECK(pool.state(0) == SlotState::Anchored);
	CHECK(inner.fieldAt(delay, mix) == expected);
	// AND THE NESTED BODY STILL CARRIES NO SLOTS, while the standalone rack's own patch does.
	CHECK_FALSE(inner.patch().hostSlots.has_value());
	CHECK(rig.top->patch().hostSlots.has_value());
}

TEST_CASE("T11, standalone: two racks nested in the standalone rack, with devices of one instance id, get distinct slots") {
	StandaloneSlotRig rig;
	const DeviceInstanceId rackA = rig.addRack();
	const DeviceInstanceId rackB = rig.addRack();
	const DeviceInstanceId a = rig.addInside(rackA, fx::delay::kTraits.tag);
	const DeviceInstanceId b = rig.addInside(rackB, fx::delay::kTraits.tag);
	REQUIRE(a == b);
	REQUIRE(rig.inner(rackA).exposeToHost(a, delayMixIndex(), nullptr));
	REQUIRE(rig.inner(rackB).exposeToHost(b, delayMixIndex(), nullptr));
	const hs::SlotPool& pool = rig.owner().pool();
	CHECK(pool.state(0) == SlotState::Pinned);
	CHECK(pool.state(1) == SlotState::Pinned);
	CHECK(pool.target(0).path == std::vector<DeviceInstanceId> { rackA, a });
	CHECK(pool.target(1).path == std::vector<DeviceInstanceId> { rackB, b });
	CHECK(pool.target(0).label == "Chain 1 " + kDot + " Delay " + kDot + " Mix");
	CHECK(pool.target(1).label == "Chain 2 " + kDot + " Delay " + kDot + " Mix");
	CHECK(rig.inner(rackB).hostSlotOf(b, delayMixIndex()).slot == 1u);
}

TEST_CASE("T11: a rack two levels down takes its slot by the whole path, and its host write reaches it") {
	StandaloneSlotRig rig;
	const DeviceInstanceId outerRack = rig.addRack();
	const DeviceInstanceId innerRack = rig.addInside(outerRack, kRack.c_str());
	rack::RackBridge* deepBridge = rack::bridgeOf(rig.inner(outerRack).containerSurfaceOf(innerRack));
	REQUIRE(deepBridge != nullptr);
	REQUIRE(deepBridge->nestedSlots()->linked());
	const DeviceInstanceId delay = deepBridge->addDevice(fx::delay::kTraits.tag);
	REQUIRE(delay != kNoInstance);
	rig.block();
	rig.block();
	// THE MIDDLE RACK TOLD THE POOL IT HOLDS THE DEEP ONE THE MOMENT IT DID (a report per commit), so the
	// path resolves through it: the deep knob is offered a slot before any visit has run.
	CHECK(deepBridge->hostSlotOf(delay, delayMixIndex()).state == Status::Unslotted);
	CHECK(rig.owner().nestedLevelCountForTest() == 2u);
	REQUIRE(deepBridge->exposeToHost(delay, delayMixIndex(), nullptr));
	const hs::SlotPool& pool = rig.owner().pool();
	CHECK(pool.target(0).path == std::vector<DeviceInstanceId> { outerRack, innerRack, delay });
	CHECK(pool.target(0).label == "Chain " + kDot + " Chain " + kDot + " Delay " + kDot + " Mix");
	for (int i = 0; i < 4; ++i) {
		rig.visit();
		rig.block();
	}
	REQUIRE(rig.owner().descOf(0) == &delayMix());
	RackEngine* deep = rack::engineOf(rig.inner(outerRack).containerSurfaceOf(innerRack));
	REQUIRE(deep != nullptr);
	const float expected = hostValueOf(delayMix(), 0.05);
	REQUIRE(deep->fieldAt(deep->slotOf(delay), delayMixIndex()) != expected);
	REQUIRE(rig.owner().onHostValue(0, 0.05));
	rig.block();
	CHECK(deep->fieldAt(deep->slotOf(delay), delayMixIndex()) == expected);
}

TEST_CASE("T11: a nested rack's Macro 1 can be exposed, and the host sees its name") {
	// THE MACRO IS THE RACK DEVICE'S OWN CONTROL, one level up: exposed through the host that holds the device.
	SlotNestRig rig;
	const DeviceInstanceId chain = rig.addChain();
	const size_t m1 = static_cast<size_t>(composite::Param::Macro1);
	REQUIRE(rig.bridge.hostSlotStatusOf(chain, m1).state == Status::Unslotted);
	REQUIRE(rig.bridge.exposeToHost(chain, m1, nullptr));
	CHECK(rig.pool().target(0).path == std::vector<DeviceInstanceId> { chain });
	CHECK(rig.pool().target(0).key == "macro_1");
	CHECK(rig.pool().target(0).label == "Chain " + kDot + " Macro 1");
	CHECK(rig.bridge.hostSlotStatusOf(chain, m1).state == Status::Pinned);
}

TEST_CASE("T11: host automation moves a nested rack's macro") {
	StandaloneSlotRig rig;
	const DeviceInstanceId rackId = rig.addRack();
	const size_t m1 = static_cast<size_t>(composite::Param::Macro1);
	REQUIRE(rig.top->exposeToHost(rackId, m1, nullptr));
	rig.settle();
	REQUIRE(rig.owner().descOf(0) == &composite::kParamDescs[m1]);
	RackEngine& inside = rig.innerEngine(rackId);
	const float expected = hostValueOf(composite::kParamDescs[m1], 0.7);
	REQUIRE(inside.macro(0) != expected);
	REQUIRE(rig.owner().onHostValue(0, 0.7));
	rig.block();
	// THE OUTER ENGINE'S BASE, AND THE RACK INSIDE'S MACRO — the value the rack's own edges read.
	CHECK(rig.engine.fieldAt(rig.engine.slotOf(rackId), m1) == expected);
	CHECK(inside.macro(0) == expected);
	// AND A DRAG ON IT RECORDS: the outer host's gesture on the slot.
	(void)drainEvents(rig.owner().outbox());
	rig.top->beginControlGesture(rackId, m1);
	rig.top->setField(rackId, m1, 0.25f);
	rig.top->endControlGesture(rackId, m1);
	const std::vector<SlotEvent> ev = drainEvents(rig.owner().outbox());
	REQUIRE(ev.size() == 3);
	CHECK(ev[0].kind == hs::SlotEventKind::Begin);
	CHECK(ev[1].value == 0.25f);
	CHECK(ev[2].kind == hs::SlotEventKind::End);
}

TEST_CASE("T11: a rename of a nested rack's macro renames the slot") {
	SlotNestRig rig;
	const DeviceInstanceId chain = rig.addChain();
	const size_t m1 = static_cast<size_t>(composite::Param::Macro1);
	const size_t m2 = m1 + 1;
	REQUIRE(rig.bridge.exposeToHost(chain, m1, nullptr));
	REQUIRE(rig.bridge.exposeToHost(chain, m2, nullptr));
	(void)rig.bridge.slotPool().takeRenamed();
	rack::RackBridge& inner = rig.inner(chain);
	inner.setMacroName(0, "Wobble");
	CHECK(rig.pool().target(0).label == "Chain " + kDot + " Wobble");
	CHECK(rig.pool().target(1).label == "Chain " + kDot + " Macro 2"); // the other macro keeps its name
	// THE HOST HEARS IT: the slot is owed a rename, and it is still the same slot in the same state.
	const std::vector<bool> renamed = rig.bridge.slotPool().takeRenamed();
	REQUIRE(renamed.size() > 0);
	CHECK(renamed[0]);
	CHECK(rig.pool().state(0) == SlotState::Pinned);
	CHECK(rig.pool().target(0).key == "macro_1");
	// A MACRO NAMED BEFORE IT IS EXPOSED IS EXPOSED UNDER ITS NAME.
	inner.setMacroName(2, "Grit");
	REQUIRE(rig.bridge.exposeToHost(chain, m1 + 2, nullptr));
	CHECK(rig.pool().target(2).label == "Chain " + kDot + " Grit");
}

TEST_CASE("backlog KT: a Poly device inside a chain in Feathers' lane processes BOTH ears of a stereo voice — a Poly filter attenuates the right as it does the left, and a Poly HARM's sub sounds in both") {
	// THE DEFECT: the two-stage hook took one channel, so a stereo voice (here a spread stack) had its right ear
	// pass every Poly device inside a chain dry. MEASURED before the fix, at C4's seventh partial through a 400 Hz
	// lowpass: -26.9 dB on the left, 0 dB on the right. Asserted as values per ear, against the same render with
	// the device switched off, and against a mono voice (stack of one) as the reference the stereo voice must meet.
	namespace flt = feathers::fx::filter;
	namespace hrm = feathers::fx::harm;
	constexpr int kBlocks = 256;
	constexpr int kFrames = kBlocks * 64;
	constexpr double kC4 = 261.626;
	struct Ears {
		std::vector<float> left, right;
	};
	const auto run = [&](const char* tag, bool enabled, bool stereo) {
		FeathersChainRig rig;
		rig.params.setBase(static_cast<clap_id>(feathers::ParamId::StackCount), stereo ? 3.0 : 1.0);
		rig.params.setBase(static_cast<clap_id>(feathers::ParamId::StackSpread), stereo ? 1.0 : 0.0);
		const feathers::DeviceInstanceId id = rig.inner->addDevice(tag);
		REQUIRE(id != feathers::kNoInstance);
		if (std::string_view(tag) == flt::kTraits.tag) {
			rig.inner->setField(id, static_cast<size_t>(flt::Param::Enabled), enabled ? 1.0f : 0.0f);
			rig.inner->setField(id, static_cast<size_t>(flt::Param::Cutoff), static_cast<float>(std::log2(400.0)));
			rig.inner->setField(id, static_cast<size_t>(flt::Param::Resonance), 0.0f);
			rig.inner->setField(id, static_cast<size_t>(flt::Param::Poly), 1.0f);
		}
		else {
			rig.inner->setField(id, static_cast<size_t>(hrm::Param::Enabled), enabled ? 1.0f : 0.0f);
			rig.inner->setField(id, static_cast<size_t>(hrm::Param::Harmonic), 4.0f); // 1:2, a sub-octave
			rig.inner->setField(id, static_cast<size_t>(hrm::Param::Level), 0.5f);
			rig.inner->setField(id, static_cast<size_t>(hrm::Param::Poly), 1.0f);
		}
		rig.provision();
		rig.block();
		rig.synth.noteOn(0, 60, 1, 0.8);
		Ears ears;
		for (int b = 0; b < kBlocks; ++b) {
			rig.block();
			ears.left.insert(ears.left.end(), rig.l.begin(), rig.l.end());
			ears.right.insert(ears.right.end(), rig.r.begin(), rig.r.end());
		}
		return ears;
	};
	const feathers::test::Window win(kFrames);
	const auto at = [&](const std::vector<float>& x, double hz) { return feathers::test::magnitudeAt(x, win, 48000.0, hz); };
	const auto dB = [](double on, double off) { return 20.0 * std::log10(on / off); };

	SUBCASE("a Poly filter") {
		const Ears monoOn = run(flt::kTraits.tag, true, false);
		const Ears monoOff = run(flt::kTraits.tag, false, false);
		const Ears on = run(flt::kTraits.tag, true, true);
		const Ears off = run(flt::kTraits.tag, false, true);
		const double reference = dB(at(monoOn.left, 7 * kC4), at(monoOff.left, 7 * kC4));
		const double leftCut = dB(at(on.left, 7 * kC4), at(off.left, 7 * kC4));
		const double rightCut = dB(at(on.right, 7 * kC4), at(off.right, 7 * kC4));
		CAPTURE(reference);
		CAPTURE(leftCut);
		CAPTURE(rightCut);
		REQUIRE(reference < -20.0); // not vacuous: the filter cuts the partial on a mono voice
		CHECK(leftCut == doctest::Approx(reference).epsilon(0.02));
		CHECK(rightCut == doctest::Approx(reference).epsilon(0.02));
		// AND THE VOICE REALLY IS STEREO, or the right ear would be the left one and pass for the wrong reason.
		CHECK(off.left != off.right);
	}

	SUBCASE("a Poly HARM") {
		const Ears on = run(hrm::kTraits.tag, true, true);
		const Ears off = run(hrm::kTraits.tag, false, true);
		const double sub = kC4 / 2.0;
		CAPTURE(at(on.left, sub));
		CAPTURE(at(on.right, sub));
		CHECK(at(off.left, sub) < 1e-3);
		CHECK(at(off.right, sub) < 1e-3);
		CHECK(at(on.left, sub) > 0.1);
		CHECK(at(on.right, sub) > 0.1);
		CHECK(off.left != off.right);
	}
}

namespace {

/// EVERY CONTROL OF THE DEVICE AT `id` SET AWAY FROM ITS DEFAULT BY THE HAND'S ROAD (`DeviceControl`'s: the touch,
/// the Begin, the value, the End — each touch taking a Recent slot from the OUTERMOST pool, T9 x T11), one at a
/// time with `settle` between, then every one read back from the level's record and its engine. Returns the
/// number checked. Expected values are the declaration's, never read back.
template <class Rig>
size_t setEveryControlAndReadBack(Rig& rig, rack::RackBridge& inner, RackEngine& engine, DeviceInstanceId id) {
	const device::Descriptor* desc = inner.descriptorOf(id);
	REQUIRE(desc != nullptr);
	std::vector<std::pair<size_t, float>> wants;
	for (size_t c = 0; c < desc->params.size(); ++c) {
		const device::ParamDesc& p = desc->params[c];
		const double normal = device::toNormal(p, p.def) < 0.5 ? 0.7 : 0.3;
		const float value = device::coerceControl(p, static_cast<float>(device::fromNormal(p, normal)));
		if (value == device::coerceControl(p, static_cast<float>(p.def))) {
			continue;
		}
		wants.emplace_back(c, value);
	}
	for (const auto& [c, value] : wants) {
		inner.touchControl(id, c);
		inner.beginControlGesture(id, c);
		inner.setField(id, c, value);
		inner.endControlGesture(id, c);
		rig.settle();
	}
	rig.settle();
	size_t checked = 0;
	for (const auto& [c, value] : wants) {
		CAPTURE(std::string(desc->params[c].key));
		CHECK(inner.fieldAt(id, c) == value);
		CHECK(engine.fieldAt(engine.slotOf(id), c) == value);
		++checked;
	}
	return checked;
}

std::vector<const char*> audioTags() {
	std::vector<const char*> tags;
	for (const device::Descriptor& d : kDevices) {
		if (d.audio != nullptr && d.traits.tag != nullptr && !device::sameTag(d.traits.tag, composite::kTraits.tag)
		    && !device::sameTag(d.traits.tag, kRack.c_str())) {
			tags.push_back(d.traits.tag);
		}
	}
	return tags;
}

} // namespace

TEST_CASE("KO x T11: editing one control of a device inside a chain in Feathers' lane never puts another back") {
	// THE SYMPTOM: device controls randomly snapped back to their defaults after another control on the
	// same device changed — on devices, not on the synth. A device inside a chain takes its slots from
	// FEATHERS' pool and its level applies the outermost bus (T11), which is a road a lane device does not have.
	size_t checked = 0;
	for (const char* tag : audioTags()) {
		CAPTURE(std::string(tag));
		SlotNestRig rig;
		const DeviceInstanceId chain = rig.addChain();
		const DeviceInstanceId id = rig.addInside(chain, tag);
		rig.settle();
		checked += setEveryControlAndReadBack(rig, rig.inner(chain), rig.innerEngine(chain), id);
	}
	MESSAGE("controls checked: " << checked);
	CHECK(checked > 100);
}

TEST_CASE("KO x T11: editing one control of a device inside a rack in the standalone rack never puts another back") {
	size_t checked = 0;
	for (const char* tag : audioTags()) {
		CAPTURE(std::string(tag));
		StandaloneSlotRig rig;
		const DeviceInstanceId rackId = rig.addRack();
		const DeviceInstanceId id = rig.addInside(rackId, tag);
		rig.settle();
		checked += setEveryControlAndReadBack(rig, rig.inner(rackId), rig.innerEngine(rackId), id);
	}
	MESSAGE("controls checked: " << checked);
	CHECK(checked > 100);
}

// ---------------------------------------------------------------------------------------------
// A STEP SEQUENCER INSIDE A CHAIN KEEPS ITS TURING REWRITES THROUGH THE OUTER PLUGIN'S SAVE (backlog MX). The chain's
// body is saved from the running instance (`ContainerOps::saveState` → its bridge's `patch()`), and `patch()` catches
// the inner record up first (`RackBridge::harvestExtraState`) — so the outer save carries the register, in both hosts.
// ---------------------------------------------------------------------------------------------

namespace {

namespace sqn = feathers::mod::stepseq;

/// THE INNER SEQUENCER, set as the engine-side cases set it: free, 8 steps a second, Turing at Lock 0, a ramp.
void seatTuring(rack::RackBridge& inner, DeviceInstanceId id) {
	inner.setField(id, sqn::at(sqn::Param::Retrig), 0.0f);
	inner.setField(id, sqn::at(sqn::Param::TimeBase), 0.0f);
	inner.setField(id, sqn::at(sqn::Param::Rate), 3.0f);
	inner.setField(id, sqn::at(sqn::Param::Turing), 1.0f);
	inner.setField(id, sqn::at(sqn::Param::TuringLock), 0.0f);
	for (size_t s = 0; s < 16; ++s) {
		inner.setField(id, sqn::valueIndex(s), static_cast<float>(s) / 16.0f);
	}
}

/// The running inner device's register: what it would publish now, step values only.
std::vector<float> registerOf(rack::RackEngine& innerEngine, DeviceInstanceId id) {
	std::array<float, device::kMaxExtraStateFloats> extra {};
	size_t n = 0;
	(void)innerEngine.extraStateAt(innerEngine.slotOf(id), extra.data(), extra.size(), &n);
	std::vector<float> out;
	for (size_t s = 0; s < sqn::kMaxSteps && s * sqn::kStepFloats < n; ++s) {
		out.push_back(extra[s * sqn::kStepFloats]);
	}
	return out;
}

/// The step values a bridge's record holds for `id`.
std::vector<float> recordedSteps(const rack::RackBridge& b, DeviceInstanceId id) {
	std::vector<float> out;
	for (size_t s = 0; s < sqn::kMaxSteps; ++s) {
		out.push_back(b.fieldAt(id, sqn::valueIndex(s)));
	}
	return out;
}

bool rewrote(const std::vector<float>& steps) {
	for (size_t s = 0; s < 16 && s < steps.size(); ++s) {
		if (steps[s] != static_cast<float>(s) / 16.0f) {
			return true;
		}
	}
	return false;
}

} // namespace

TEST_CASE("MX: a step sequencer inside a chain keeps its Turing rewrites through the OUTER rack's save and load") {
	RackEngine engine { rack::kRackAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	rack::RackBridge outer { engine };
	outer.prepare(48000.0, 4096);
	const DeviceInstanceId chain = outer.addDevice(kRack.c_str());
	REQUIRE(chain != kNoInstance);
	void* handle = outer.containerSurfaceOf(chain);
	rack::RackBridge* inner = rack::bridgeOf(handle);
	RackEngine* innerEngine = rack::engineOf(handle);
	REQUIRE(inner != nullptr);
	REQUIRE(innerEngine != nullptr);
	const DeviceInstanceId seq = inner->addDevice(sqn::kTraits.tag);
	REQUIRE(seq != kNoInstance);
	seatTuring(*inner, seq);
	std::vector<float> l(64, 0.0f), r(64, 0.0f);
	for (int b = 0; b < 600; ++b) {
		outer.drain();
		engine.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
		outer.publish(engine);
	}
	const std::vector<float> heard = registerOf(*innerEngine, seq);
	REQUIRE(rewrote(heard)); // NOT VACUOUS: the machine has moved off the hand's ramp

	// THE OUTER'S SAVE — nothing harvested by hand, inside or out.
	std::string text;
	REQUIRE(rack::rackToText(outer.patch(), &text));
	rack::RackPatch parsed;
	REQUIRE(rack::parseRackText(text, &parsed));

	RackEngine engine2 { rack::kRackAxes, hostCatalogue() };
	engine2.prepare(48000.0, 4096);
	rack::RackBridge loaded { engine2 };
	loaded.prepare(48000.0, 4096);
	(void)loaded.loadPatch(parsed);
	loaded.drain();
	engine2.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
	loaded.publish(engine2);
	const DeviceInstanceId chain2 = loaded.deviceAtPosition(0);
	rack::RackBridge* inner2 = rack::bridgeOf(loaded.containerSurfaceOf(chain2));
	REQUIRE(inner2 != nullptr);
	const DeviceInstanceId seq2 = inner2->deviceAtPosition(0);
	REQUIRE(seq2 != kNoInstance);
	CHECK(recordedSteps(*inner2, seq2) == heard);
}

TEST_CASE("MX: a step sequencer inside a chain in Feathers' lane keeps its Turing rewrites through Feathers' save and load") {
	FeathersChainRig rig;
	rack::RackEngine* innerEngine = rack::engineOf(rig.bridge.fxBodies().surfaceOf(rig.chain));
	REQUIRE(innerEngine != nullptr);
	const DeviceInstanceId seq = rig.inner->addDevice(sqn::kTraits.tag);
	REQUIRE(seq != kNoInstance);
	seatTuring(*rig.inner, seq);
	rig.provision();
	for (int b = 0; b < 600; ++b) {
		rig.block();
	}
	const std::vector<float> heard = registerOf(*innerEngine, seq);
	REQUIRE(rewrote(heard));

	// FEATHERS' SAVE, as the plugin makes it: the record caught up, then the chains block taken.
	rig.bridge.harvestExtraState();
	const rack::RackPatch saved = rig.bridge.chainsPatch(false);

	FeathersChainRig other;
	(void)other.bridge.restoreChains(saved);
	other.bridge.refresh();
	other.provision();
	other.block();
	DeviceInstanceId chain2 = kNoInstance;
	for (size_t p = 0; p < feathers::kMaxFxSlots && chain2 == kNoInstance; ++p) {
		const DeviceInstanceId id = other.bridge.fxDeviceAt(p);
		const device::Descriptor* d = id != kNoInstance ? other.bridge.descriptorOf(id) : nullptr;
		if (d != nullptr && device::sameTag(d->traits.tag, feathers::composite::kTraits.tag)) {
			chain2 = id;
		}
	}
	REQUIRE(chain2 != kNoInstance);
	rack::RackBridge* inner2 = rack::bridgeOf(other.bridge.fxBodies().surfaceOf(chain2));
	REQUIRE(inner2 != nullptr);
	const DeviceInstanceId seq2 = inner2->deviceAtPosition(0);
	REQUIRE(seq2 != kNoInstance);
	CHECK(recordedSteps(*inner2, seq2) == heard);
}

// ---------------------------------------------------------------------------------------------
// BACKLOG NZ — A MODULATOR IN A DEVICE'S PRE CHAIN REACHES THE DEVICE, IN FEATHERS' LANE (a modulator
// in the pre chain could not modulate the main effect). The road through the plugin's own
// bridge, record and synth, by value.
// ---------------------------------------------------------------------------------------------

TEST_CASE("NZ: in Feathers' lane, a Macros modulator in the Filter's PRE moves the Filter's cutoff, by value") {
	feathers::ParamSet params;
	feathers::Synth synth { params };
	feathers::ParamBridge bridge;
	synth.activate(48000.0, 4096);
	params.setBase(static_cast<clap_id>(feathers::ParamId::VoiceLevel), 1.0);
	synth.setCatalogue(feathers::hostCatalogue());
	synth.setOuterGateMaker(&feathers::rack::ChainRecord::gateOver, &bridge.chainRecord());
	std::vector<float> l(64, 0.0f);
	std::vector<float> r(64, 0.0f);
	const auto provision = [&] {
		feathers::publishEditorLanes(bridge, params, synth);
		(void)synth.provisionDevices(bridge.chainState(), bridge.publishedChains().sequence(), &bridge.fxBodies());
	};
	const auto blocks = [&](int n) {
		for (int i = 0; i < n; ++i) {
			provision();
			synth.render(l.data(), r.data(), 64);
		}
	};

	const size_t slot = bridge.addFxDevice(feathers::fx::filter::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	bridge.refresh();
	const feathers::DeviceInstanceId filter = bridge.fxDeviceAt(slot);
	REQUIRE(filter != feathers::kNoInstance);
	REQUIRE(bridge.openFxSite(filter));
	blocks(2);
	auto* site = static_cast<feathers::rack::SiteChains*>(bridge.fxBodies().surfaceOf(feathers::siteKeyOf(filter)));
	REQUIRE(site != nullptr);
	const feathers::DeviceInstanceId macros = site->pre().bridge()->addDevice(feathers::mod::macros::kTraits.tag);
	REQUIRE(macros != feathers::kNoInstance);
	site->pre().bridge()->setField(macros, 0, 0.5f);
	blocks(2);

	size_t cutoff = 0;
	const auto& desc = feathers::fx::filter::kDescriptor;
	while (cutoff < desc.params.size() && (!desc.params[cutoff].modulatable || static_cast<int>(cutoff) == desc.traits.enableControl)) {
		++cutoff;
	}
	REQUIRE(cutoff < desc.params.size());
	const feathers::rack::RackEngine& lane = synth.laneForTest().engineForTest();
	const size_t at = lane.slotOf(filter);
	REQUIRE(at != feathers::rack::RackEngine::kNoSlot);
	const float base = lane.resolvedFieldForTest(at, cutoff);

	const feathers::backplane::Source out = bridge.fxSiteExport(filter, feathers::backplane::Source::output(macros, 0));
	REQUIRE(out.isExport());
	const auto dest = feathers::backplane::Destination::control(filter, cutoff);
	REQUIRE(bridge.mayRoute(out, dest));
	bridge.setRouteAmount(out, dest, 0.25f);
	CHECK(bridge.routeAmount(out, dest) == doctest::Approx(0.25f));
	blocks(4);
	// THE INTENDED PATH RESTATED: the base plus amount · the macro's 0.5 · the control's span, as a route offset lands.
	const double expected =
	    feathers::device::withRouteOffset(desc.params[cutoff], base, 0.25 * 0.5 * feathers::device::routeSpanOf(desc.params[cutoff]));
	CHECK(expected != doctest::Approx(base)); // the claim below is about a value that moved
	CHECK(lane.resolvedFieldForTest(at, cutoff) == doctest::Approx(expected).epsilon(1e-5));
}

TEST_CASE("NZ rung 4: in Feathers' lane, a Macros modulator in the Filter's PRE moves the cutoff of the NEXT device, out "
          "the back of the site, by value — and is refused for the device before it") {
	feathers::ParamSet params;
	feathers::Synth synth { params };
	feathers::ParamBridge bridge;
	synth.activate(48000.0, 4096);
	params.setBase(static_cast<clap_id>(feathers::ParamId::VoiceLevel), 1.0);
	synth.setCatalogue(feathers::hostCatalogue());
	synth.setOuterGateMaker(&feathers::rack::ChainRecord::gateOver, &bridge.chainRecord());
	std::vector<float> l(64, 0.0f);
	std::vector<float> r(64, 0.0f);
	const auto blocks = [&](int n) {
		for (int i = 0; i < n; ++i) {
			feathers::publishEditorLanes(bridge, params, synth);
			(void)synth.provisionDevices(bridge.chainState(), bridge.publishedChains().sequence(), &bridge.fxBodies());
			synth.render(l.data(), r.data(), 64);
		}
	};
	// BEFORE, HOLDER, AFTER — three Filters in the lane, the site on the middle one.
	const auto add = [&] {
		const size_t slot = bridge.addFxDevice(feathers::fx::filter::kTraits.tag);
		REQUIRE(slot < feathers::kMaxFxSlots);
		bridge.refresh();
		return bridge.fxDeviceAt(slot);
	};
	const feathers::DeviceInstanceId before = add();
	const feathers::DeviceInstanceId holder = add();
	const feathers::DeviceInstanceId after = add();
	REQUIRE(bridge.openFxSite(holder));
	blocks(2);
	auto* site = static_cast<feathers::rack::SiteChains*>(bridge.fxBodies().surfaceOf(feathers::siteKeyOf(holder)));
	REQUIRE(site != nullptr);
	const feathers::DeviceInstanceId macros = site->pre().bridge()->addDevice(feathers::mod::macros::kTraits.tag);
	REQUIRE(macros != feathers::kNoInstance);
	site->pre().bridge()->setField(macros, 0, 0.5f);
	blocks(2);

	const auto& desc = feathers::fx::filter::kDescriptor;
	size_t cutoff = 0;
	while (cutoff < desc.params.size() && (!desc.params[cutoff].modulatable || static_cast<int>(cutoff) == desc.traits.enableControl)) {
		++cutoff;
	}
	REQUIRE(cutoff < desc.params.size());
	const feathers::rack::RackEngine& lane = synth.laneForTest().engineForTest();
	const size_t at = lane.slotOf(after);
	REQUIRE(at != feathers::rack::RackEngine::kNoSlot);
	const float base = lane.resolvedFieldForTest(at, cutoff);

	const feathers::backplane::Source out = bridge.fxSiteExport(holder, feathers::backplane::Source::output(macros, 0));
	REQUIRE(out.isExport());
	// UPSTREAM OF THE HOLDER: refused, and no route is made.
	CHECK_FALSE(bridge.mayRoute(out, feathers::backplane::Destination::control(before, cutoff)));
	bridge.setRouteAmount(out, feathers::backplane::Destination::control(before, cutoff), 0.25f);
	CHECK(bridge.routeAmount(out, feathers::backplane::Destination::control(before, cutoff)) == 0.0f);
	// OUT THE BACK: admitted, and the value arrives.
	const auto dest = feathers::backplane::Destination::control(after, cutoff);
	REQUIRE(bridge.mayRoute(out, dest));
	bridge.setRouteAmount(out, dest, 0.25f);
	blocks(4);
	const double expected =
	    feathers::device::withRouteOffset(desc.params[cutoff], base, 0.25 * 0.5 * feathers::device::routeSpanOf(desc.params[cutoff]));
	CHECK(expected != doctest::Approx(base));
	CHECK(lane.resolvedFieldForTest(at, cutoff) == doctest::Approx(expected).epsilon(1e-5));
	// AND THE ORDER IS HELD: the route made the holder an upstream of the device after it.
	CHECK(bridge.chainRecord().graph().rankOf(holder) < bridge.chainRecord().graph().rankOf(after));
}

// ---------------------------------------------------------------------------------------------
// NZ RUNG 5 — A PRE EXPORT REACHES THE DEVICES INSIDE A CHAIN OR A RACK PLACED AFTER ITS HOLDER. The container
// passes the outer export inward through rack roadmap §7's reader: the inner route is spelled `out.asOuter()` and
// lives in the inner record, and the outer gate answers whether the holder is upstream of the container.
// ---------------------------------------------------------------------------------------------

namespace {

/// FEATHERS' LANE: [a Chain before] [the holder Filter with a Macros at 0.5 in its PRE] [a Chain] [a Rack], each
/// container with one Filter inside.
struct RungFive {
	feathers::ParamSet params;
	feathers::Synth synth { params };
	feathers::ParamBridge bridge;
	std::vector<float> l = std::vector<float>(64, 0.0f);
	std::vector<float> r = std::vector<float>(64, 0.0f);
	feathers::DeviceInstanceId before = feathers::kNoInstance;
	feathers::DeviceInstanceId holder = feathers::kNoInstance;
	feathers::DeviceInstanceId chain = feathers::kNoInstance;
	feathers::DeviceInstanceId rackId = feathers::kNoInstance;
	feathers::DeviceInstanceId macros = feathers::kNoInstance;
	feathers::rack::RackBridge* beforeInner = nullptr;
	feathers::rack::RackBridge* chainInner = nullptr;
	feathers::rack::RackBridge* rackInner = nullptr;
	feathers::DeviceInstanceId beforeFilter = feathers::kNoInstance;
	feathers::DeviceInstanceId chainFilter = feathers::kNoInstance;
	feathers::DeviceInstanceId rackFilter = feathers::kNoInstance;
	size_t cutoff = 0;

	RungFive() {
		synth.activate(48000.0, 4096);
		params.setBase(static_cast<clap_id>(feathers::ParamId::VoiceLevel), 1.0);
		synth.setCatalogue(feathers::hostCatalogue());
		synth.setOuterGateMaker(&feathers::rack::ChainRecord::gateOver, &bridge.chainRecord());
		const auto& desc = feathers::fx::filter::kDescriptor;
		while (cutoff < desc.params.size() && (!desc.params[cutoff].modulatable || static_cast<int>(cutoff) == desc.traits.enableControl)) {
			++cutoff;
		}
		REQUIRE(cutoff < desc.params.size());
	}
	void blocks(int n) {
		for (int i = 0; i < n; ++i) {
			feathers::publishEditorLanes(bridge, params, synth);
			(void)synth.provisionDevices(bridge.chainState(), bridge.publishedChains().sequence(), &bridge.fxBodies());
			synth.render(l.data(), r.data(), 64);
		}
	}
	feathers::DeviceInstanceId add(const char* tag) {
		const size_t slot = bridge.addFxDevice(tag);
		REQUIRE(slot < feathers::kMaxFxSlots);
		bridge.refresh();
		return bridge.fxDeviceAt(slot);
	}
	void build() {
		before = add(feathers::composite::kTraits.tag);
		holder = add(feathers::fx::filter::kTraits.tag);
		chain = add(feathers::composite::kTraits.tag);
		rackId = add(feathers::rack::parallel::kTraits.tag);
		REQUIRE(bridge.openFxSite(holder));
		blocks(2);
		auto* site = static_cast<feathers::rack::SiteChains*>(bridge.fxBodies().surfaceOf(feathers::siteKeyOf(holder)));
		REQUIRE(site != nullptr);
		macros = site->pre().bridge()->addDevice(feathers::mod::macros::kTraits.tag);
		REQUIRE(macros != feathers::kNoInstance);
		site->pre().bridge()->setField(macros, 0, 0.5f);
		beforeInner = feathers::rack::bridgeOf(bridge.fxBodies().surfaceOf(before));
		chainInner = feathers::rack::bridgeOf(bridge.fxBodies().surfaceOf(chain));
		feathers::rack::parallel::Rack* rack = feathers::rack::parallel::rackOf(bridge.fxBodies().surfaceOf(rackId));
		REQUIRE(beforeInner != nullptr);
		REQUIRE(chainInner != nullptr);
		REQUIRE(rack != nullptr);
		if (rack->chainCount() == 0) {
			(void)rack->addChain();
		}
		REQUIRE(rack->chainAt(0) != nullptr);
		rackInner = rack->chainAt(0)->bridge();
		REQUIRE(rackInner != nullptr);
		beforeFilter = beforeInner->addDevice(feathers::fx::filter::kTraits.tag);
		chainFilter = chainInner->addDevice(feathers::fx::filter::kTraits.tag);
		rackFilter = rackInner->addDevice(feathers::fx::filter::kTraits.tag);
		REQUIRE(beforeFilter != feathers::kNoInstance);
		REQUIRE(chainFilter != feathers::kNoInstance);
		REQUIRE(rackFilter != feathers::kNoInstance);
		blocks(2);
	}
	/// The export of the PRE macro, minted.
	feathers::backplane::Source out() { return bridge.fxSiteExport(holder, feathers::backplane::Source::output(macros, 0)); }
	/// What the engine inside `container` resolved for `id`'s cutoff in the block just run.
	float resolved(feathers::DeviceInstanceId container, feathers::DeviceInstanceId id) {
		const feathers::rack::RackEngine* engine = nullptr;
		if (container == rackId) {
			auto* rack = feathers::rack::parallel::rackOf(bridge.fxBodies().surfaceOf(rackId));
			REQUIRE(rack != nullptr);
			engine = &rack->chainAt(0)->engine();
		}
		else {
			engine = feathers::rack::engineOf(bridge.fxBodies().surfaceOf(container));
		}
		REQUIRE(engine != nullptr);
		const size_t at = engine->slotOf(id);
		REQUIRE(at != feathers::rack::RackEngine::kNoSlot);
		return engine->resolvedFieldForTest(at, cutoff);
	}
	/// The intended path restated: the base plus amount · the macro's 0.5 · the control's span.
	double expected(float base, float amount) const {
		const auto& p = feathers::fx::filter::kDescriptor.params[cutoff];
		return feathers::device::withRouteOffset(p, base, amount * 0.5 * feathers::device::routeSpanOf(p));
	}
};

} // namespace

TEST_CASE("NZ rung 5: in Feathers' lane, a PRE Macros moves a Filter inside a Chain AFTER the holder and one inside a "
          "Rack's chain, by value — and is refused inside a Chain before the holder") {
	RungFive rig;
	rig.build();
	const float chainBase = rig.resolved(rig.chain, rig.chainFilter);
	const float rackBase = rig.resolved(rig.rackId, rig.rackFilter);
	const feathers::backplane::Source out = rig.out();
	REQUIRE(out.isExport());
	const auto inChain = feathers::backplane::Destination::control(rig.chainFilter, rig.cutoff);
	const auto inRack = feathers::backplane::Destination::control(rig.rackFilter, rig.cutoff);
	const auto inBefore = feathers::backplane::Destination::control(rig.beforeFilter, rig.cutoff);

	// UPSTREAM OF THE HOLDER: the Chain before it refuses, and no route is made.
	CHECK_FALSE(rig.beforeInner->mayRoute(out.asOuter(), inBefore));
	rig.beforeInner->setRouteAmount(out.asOuter(), inBefore, 0.25f);
	CHECK(rig.beforeInner->routeAmount(out.asOuter(), inBefore) == 0.0f);
	// AFTER IT: both containers admit, through their inner records.
	REQUIRE(rig.chainInner->mayRoute(out.asOuter(), inChain));
	REQUIRE(rig.rackInner->mayRoute(out.asOuter(), inRack));
	// AND THE ENGINES SAY THE SAME (their gate is what a patch loaded straight into an engine is asked of): the inner
	// engine asks the lane's engine through §7's reader.
	const feathers::rack::RackEngine* chainEngine = feathers::rack::engineOf(rig.bridge.fxBodies().surfaceOf(rig.chain));
	const feathers::rack::RackEngine* beforeEngine = feathers::rack::engineOf(rig.bridge.fxBodies().surfaceOf(rig.before));
	REQUIRE(chainEngine != nullptr);
	REQUIRE(beforeEngine != nullptr);
	CHECK(chainEngine->mayRoute(out.asOuter(), inChain));
	CHECK_FALSE(beforeEngine->mayRoute(out.asOuter(), inBefore));
	rig.chainInner->setRouteAmount(out.asOuter(), inChain, 0.25f);
	rig.rackInner->setRouteAmount(out.asOuter(), inRack, -0.25f);
	rig.blocks(4);
	const double wantChain = rig.expected(chainBase, 0.25f);
	const double wantRack = rig.expected(rackBase, -0.25f);
	CHECK(wantChain != doctest::Approx(chainBase)); // the claims below are about values that moved
	CHECK(wantRack != doctest::Approx(rackBase));
	CHECK(rig.resolved(rig.chain, rig.chainFilter) == doctest::Approx(wantChain).epsilon(1e-5));
	CHECK(rig.resolved(rig.rackId, rig.rackFilter) == doctest::Approx(wantRack).epsilon(1e-5));

	// THE MACRO MOVES AND BOTH FOLLOW IT: at 0 every route offset is 0.
	auto* site = static_cast<feathers::rack::SiteChains*>(rig.bridge.fxBodies().surfaceOf(feathers::siteKeyOf(rig.holder)));
	REQUIRE(site != nullptr);
	site->pre().bridge()->setField(rig.macros, 0, 0.0f);
	rig.blocks(4);
	CHECK(rig.resolved(rig.chain, rig.chainFilter) == doctest::Approx(chainBase).epsilon(1e-5));
	CHECK(rig.resolved(rig.rackId, rig.rackFilter) == doctest::Approx(rackBase).epsilon(1e-5));

	// THE ORDER DECIDES, NOT THE SPELLING: the Chain moved before the holder loses the route the gate no longer admits.
	REQUIRE(rig.bridge.moveFxDevice(rig.bridge.fxSlotOf(rig.chain), rig.bridge.fxSlotOf(rig.before)));
	REQUIRE(rig.bridge.fxSlotOf(rig.chain) < rig.bridge.fxSlotOf(rig.holder));
	rig.blocks(4);
	CHECK_FALSE(rig.chainInner->mayRoute(out.asOuter(), inChain));
	CHECK(rig.chainInner->routeAmount(out.asOuter(), inChain) == 0.0f);
	CHECK(rig.resolved(rig.chain, rig.chainFilter) == doctest::Approx(chainBase).epsilon(1e-5));
}

TEST_CASE("NZ rung 5: a PRE export's routes into a Chain's and a Rack's inner devices save with Feathers' patch and "
          "arrive in a fresh plugin, by value") {
	feathers::rack::RackPatch saved;
	float chainBase = 0.0f;
	float rackBase = 0.0f;
	{
		RungFive rig;
		rig.build();
		chainBase = rig.resolved(rig.chain, rig.chainFilter);
		rackBase = rig.resolved(rig.rackId, rig.rackFilter);
		const feathers::backplane::Source out = rig.out();
		REQUIRE(out.isExport());
		rig.chainInner->setRouteAmount(out.asOuter(), feathers::backplane::Destination::control(rig.chainFilter, rig.cutoff), 0.25f);
		rig.rackInner->setRouteAmount(out.asOuter(), feathers::backplane::Destination::control(rig.rackFilter, rig.cutoff), -0.25f);
		rig.blocks(4);
		rig.bridge.harvestExtraState();
		saved = rig.bridge.chainsPatch(false);
	}
	RungFive other;
	(void)other.bridge.restoreChains(saved);
	other.bridge.refresh();
	other.blocks(6);
	// THE SAME LANE, FOUND BY POSITION AFTER WHATEVER THE DEFAULT PATCH HOLDS: Chain, holder, Chain, Rack.
	const size_t first = other.bridge.fxDeviceCount() - 4;
	other.before = other.bridge.fxDeviceAt(first + 0);
	other.holder = other.bridge.fxDeviceAt(first + 1);
	other.chain = other.bridge.fxDeviceAt(first + 2);
	other.rackId = other.bridge.fxDeviceAt(first + 3);
	other.chainInner = feathers::rack::bridgeOf(other.bridge.fxBodies().surfaceOf(other.chain));
	auto* rack = feathers::rack::parallel::rackOf(other.bridge.fxBodies().surfaceOf(other.rackId));
	REQUIRE(other.chainInner != nullptr);
	REQUIRE(rack != nullptr);
	REQUIRE(rack->chainAt(0) != nullptr);
	other.rackInner = rack->chainAt(0)->bridge();
	other.chainFilter = other.chainInner->deviceAtPosition(0);
	other.rackFilter = other.rackInner->deviceAtPosition(0);
	REQUIRE(other.chainFilter != feathers::kNoInstance);
	REQUIRE(other.rackFilter != feathers::kNoInstance);
	CHECK(other.resolved(other.chain, other.chainFilter) == doctest::Approx(other.expected(chainBase, 0.25f)).epsilon(1e-5));
	CHECK(other.resolved(other.rackId, other.rackFilter) == doctest::Approx(other.expected(rackBase, -0.25f)).epsilon(1e-5));
}

// ---------------------------------------------------------------------------------------------
// NZ (b) — A MODULATOR INSIDE A CHAIN OR A RACK REACHES THE DEVICES AFTER THE CONTAINER (downstream modulators
// flow out the back of the compound chain, as normal). The container exports it (its own table, in its
// body), and the lane reads `Source::exported(container, row)`.
// ---------------------------------------------------------------------------------------------

TEST_CASE("NZ (b): in Feathers' lane, a Macros inside a Chain and one inside a Rack's chain move the cutoff of a Filter "
          "AFTER each container, by value; a Filter before is refused; the patch carries them to a fresh plugin") {
	feathers::rack::RackPatch saved;
	double wantChain = 0.0;
	double wantRack = 0.0;
	{
		RungFive rig;
		const feathers::DeviceInstanceId before = rig.add(feathers::fx::filter::kTraits.tag);
		const feathers::DeviceInstanceId chain = rig.add(feathers::composite::kTraits.tag);
		const feathers::DeviceInstanceId mid = rig.add(feathers::fx::filter::kTraits.tag);
		const feathers::DeviceInstanceId rackId = rig.add(feathers::rack::parallel::kTraits.tag);
		const feathers::DeviceInstanceId last = rig.add(feathers::fx::filter::kTraits.tag);
		rig.blocks(2);
		feathers::rack::RackBridge* inChain = feathers::rack::bridgeOf(rig.bridge.fxBodies().surfaceOf(chain));
		auto* rack = feathers::rack::parallel::rackOf(rig.bridge.fxBodies().surfaceOf(rackId));
		REQUIRE(inChain != nullptr);
		REQUIRE(rack != nullptr);
		if (rack->chainCount() == 0) {
			(void)rack->addChain();
		}
		feathers::rack::RackBridge* inRack = rack->chainAt(0)->bridge();
		REQUIRE(inRack != nullptr);
		const feathers::DeviceInstanceId m1 = inChain->addDevice(feathers::mod::macros::kTraits.tag);
		const feathers::DeviceInstanceId m2 = inRack->addDevice(feathers::mod::macros::kTraits.tag);
		REQUIRE(m1 != feathers::kNoInstance);
		REQUIRE(m2 != feathers::kNoInstance);
		inChain->setField(m1, 0, 0.5f);
		inRack->setField(m2, 0, 0.5f);
		rig.blocks(2);
		const feathers::rack::RackEngine& lane = rig.synth.laneForTest().engineForTest();
		const auto value = [&](feathers::DeviceInstanceId id) {
			const size_t at = lane.slotOf(id);
			REQUIRE(at != feathers::rack::RackEngine::kNoSlot);
			return lane.resolvedFieldForTest(at, rig.cutoff);
		};
		const float midBase = value(mid);
		const float lastBase = value(last);
		const feathers::backplane::Source outChain = rig.bridge.fxContainerExport(chain, feathers::backplane::Source::output(m1, 0));
		const feathers::backplane::Source outRack = rig.bridge.fxContainerExport(rackId, feathers::backplane::Source::output(m2, 0));
		REQUIRE(outChain.isExport());
		REQUIRE(outRack.isExport());
		CHECK_FALSE(rig.bridge.mayRoute(outChain, feathers::backplane::Destination::control(before, rig.cutoff)));
		const auto toMid = feathers::backplane::Destination::control(mid, rig.cutoff);
		const auto toLast = feathers::backplane::Destination::control(last, rig.cutoff);
		REQUIRE(rig.bridge.mayRoute(outChain, toMid));
		REQUIRE(rig.bridge.mayRoute(outRack, toLast));
		rig.bridge.setRouteAmount(outChain, toMid, 0.25f);
		rig.bridge.setRouteAmount(outRack, toLast, -0.25f);
		rig.blocks(4);
		wantChain = rig.expected(midBase, 0.25f);
		wantRack = rig.expected(lastBase, -0.25f);
		CHECK(wantChain != doctest::Approx(midBase));
		CHECK(wantRack != doctest::Approx(lastBase));
		CHECK(value(mid) == doctest::Approx(wantChain).epsilon(1e-5));
		CHECK(value(last) == doctest::Approx(wantRack).epsilon(1e-5));
		rig.bridge.harvestExtraState();
		saved = rig.bridge.chainsPatch(false);
	}
	RungFive other;
	(void)other.bridge.restoreChains(saved);
	other.bridge.refresh();
	other.blocks(6);
	const size_t first = other.bridge.fxDeviceCount() - 5;
	const feathers::rack::RackEngine& lane = other.synth.laneForTest().engineForTest();
	const auto value = [&](feathers::DeviceInstanceId id) {
		const size_t at = lane.slotOf(id);
		REQUIRE(at != feathers::rack::RackEngine::kNoSlot);
		return lane.resolvedFieldForTest(at, other.cutoff);
	};
	CHECK(value(other.bridge.fxDeviceAt(first + 2)) == doctest::Approx(wantChain).epsilon(1e-5));
	CHECK(value(other.bridge.fxDeviceAt(first + 4)) == doctest::Approx(wantRack).epsilon(1e-5));
}

// ---------------------------------------------------------------------------------------------
// D3 LIFTED: A RACK AND A CHAIN HOLD A SITE. One holder then has TWO export tables — its site's PRE
// and its container's inside — each counting rows from 0, so the two roads are two BITS (`Source::isSiteExport`,
// `Source::isContainerExport`) and two codec kinds. Every claim a value: the cells, the resolved controls, the signal.
// ---------------------------------------------------------------------------------------------

namespace {

/// THE RACK PLUGIN'S OWN HOST (an `InnerChain` over the real catalogue): [a Rack] [a Chain] [a Filter], the Rack and the
/// Chain each with a site, a Macros at `kPre` in each site's PRE, and a Macros at `kInside` in the Rack's first chain.
struct BothRoads {
	static constexpr float kPre = 0.5f;
	static constexpr float kChainPre = 0.25f;
	static constexpr float kInside = 0.75f;
	feathers::rack::InnerChain host { feathers::rack::kRackAxes };
	feathers::DeviceInstanceId rackId = feathers::kNoInstance;
	feathers::DeviceInstanceId chain = feathers::kNoInstance;
	feathers::DeviceInstanceId after = feathers::kNoInstance;
	feathers::DeviceInstanceId rackPre = feathers::kNoInstance;
	feathers::DeviceInstanceId chainPre = feathers::kNoInstance;
	feathers::DeviceInstanceId inside = feathers::kNoInstance;
	size_t cutoff = 0;
	std::vector<float> l = std::vector<float>(64, 0.0f);
	std::vector<float> r = std::vector<float>(64, 0.0f);

	feathers::rack::RackBridge& b() { return *host.bridge(); }
	feathers::rack::SiteChains* siteOf(feathers::DeviceInstanceId id) {
		return static_cast<feathers::rack::SiteChains*>(b().containerSurfaceOf(feathers::siteKeyOf(id)));
	}
	feathers::rack::RackBridge* rackChain() {
		auto* rack = feathers::rack::parallel::rackOf(b().containerSurfaceOf(rackId));
		REQUIRE(rack != nullptr);
		if (rack->chainCount() == 0) {
			(void)rack->addChain();
		}
		REQUIRE(rack->chainAt(0) != nullptr);
		return rack->chainAt(0)->bridge();
	}
	void blocks(int n, float fill = 0.25f) {
		for (int i = 0; i < n; ++i) {
			std::fill(l.begin(), l.end(), fill);
			std::fill(r.begin(), r.end(), fill);
			host.process(l.data(), r.data(), static_cast<int>(l.size()));
		}
	}
	explicit BothRoads(bool build = true) {
		host.prepare(48000.0, 4096);
		host.adopt(feathers::hostCatalogue());
		const auto& desc = feathers::fx::filter::kDescriptor;
		while (cutoff < desc.params.size() && (!desc.params[cutoff].modulatable || static_cast<int>(cutoff) == desc.traits.enableControl)) {
			++cutoff;
		}
		REQUIRE(cutoff < desc.params.size());
		if (!build) {
			return;
		}
		rackId = b().addDevice(feathers::rack::parallel::kTraits.tag);
		chain = b().addDevice(feathers::composite::kTraits.tag);
		after = b().addDevice(feathers::fx::filter::kTraits.tag);
		REQUIRE(rackId != feathers::kNoInstance);
		REQUIRE(chain != feathers::kNoInstance);
		REQUIRE(after != feathers::kNoInstance);
		REQUIRE(b().openSite(rackId));
		REQUIRE(b().openSite(chain));
		blocks(2);
		REQUIRE(siteOf(rackId) != nullptr);
		REQUIRE(siteOf(chain) != nullptr);
		rackPre = siteOf(rackId)->pre().bridge()->addDevice(feathers::mod::macros::kTraits.tag);
		chainPre = siteOf(chain)->pre().bridge()->addDevice(feathers::mod::macros::kTraits.tag);
		inside = rackChain()->addDevice(feathers::mod::macros::kTraits.tag);
		REQUIRE(rackPre != feathers::kNoInstance);
		REQUIRE(chainPre != feathers::kNoInstance);
		REQUIRE(inside != feathers::kNoInstance);
		siteOf(rackId)->pre().bridge()->setField(rackPre, 0, kPre);
		siteOf(chain)->pre().bridge()->setField(chainPre, 0, kChainPre);
		rackChain()->setField(inside, 0, kInside);
		blocks(2);
	}
	feathers::backplane::Source siteOut() {
		return b().siteExport(rackId, feathers::backplane::Source::output(rackPre, 0));
	}
	feathers::backplane::Source chainSiteOut() {
		return b().siteExport(chain, feathers::backplane::Source::output(chainPre, 0));
	}
	feathers::backplane::Source insideOut() {
		return b().containerExport(rackId, feathers::backplane::Source::output(inside, 0));
	}
	float resolved(feathers::DeviceInstanceId id, size_t control) {
		const size_t at = host.engine().slotOf(id);
		REQUIRE(at != feathers::rack::RackEngine::kNoSlot);
		return host.engine().resolvedFieldForTest(at, control);
	}
};

constexpr size_t kRackMacro1 = static_cast<size_t>(feathers::rack::parallel::Param::Macro1);
constexpr size_t kChainMacro1 = static_cast<size_t>(feathers::composite::Param::Macro1);

/// The intended path restated: a route offset of amount · value · the control's span, landed on `base`.
double landed(const feathers::device::ParamDesc& p, double base, double amount, double value) {
	return feathers::device::withRouteOffset(p, base, amount * value * feathers::device::routeSpanOf(p));
}

} // namespace

TEST_CASE("D3 lifted: a Rack's SITE row 0 and its CONTAINER row 0 are two sources and two cells — each reads its own "
          "modulator, by value") {
	BothRoads h;
	const feathers::backplane::Source site = h.siteOut();
	const feathers::backplane::Source inside = h.insideOut();
	REQUIRE(site.isSiteExport());
	REQUIRE(inside.isContainerExport());
	CHECK_FALSE(site.isContainerExport());
	CHECK_FALSE(inside.isSiteExport());
	// THE SAME INDEX ON THE SAME HOLDER — the collision the second bit exists for.
	CHECK(site.device == inside.device);
	CHECK(site.exportRow() == 0);
	CHECK(inside.exportRow() == 0);
	CHECK(site != inside);
	h.blocks(2);
	const float* siteCell = h.host.engine().sourceCell(site);
	const float* insideCell = h.host.engine().sourceCell(inside);
	REQUIRE(siteCell != nullptr);
	REQUIRE(insideCell != nullptr);
	CHECK(siteCell != insideCell);
	CHECK(*siteCell == doctest::Approx(BothRoads::kPre));
	CHECK(*insideCell == doctest::Approx(BothRoads::kInside));
}

TEST_CASE("D3 lifted: a PRE modulator moves a Rack's Macro 1 and a Chain's Macro 1 by value; the Rack's inside reaches "
          "the Filter after it and never the Rack's own controls") {
	BothRoads h;
	const auto& rackDesc = feathers::rack::parallel::kDescriptor;
	const auto& chainDesc = feathers::composite::kDescriptor;
	const auto& filterDesc = feathers::fx::filter::kDescriptor;
	const float rackBase = h.resolved(h.rackId, kRackMacro1);
	const float chainBase = h.resolved(h.chain, kChainMacro1);
	const float filterBase = h.resolved(h.after, h.cutoff);
	const auto toRackMacro = feathers::backplane::Destination::control(h.rackId, kRackMacro1);
	const auto toChainMacro = feathers::backplane::Destination::control(h.chain, kChainMacro1);
	const auto toFilter = feathers::backplane::Destination::control(h.after, h.cutoff);
	REQUIRE(h.b().mayRoute(h.siteOut(), toRackMacro));
	REQUIRE(h.b().mayRoute(h.chainSiteOut(), toChainMacro));
	REQUIRE(h.b().mayRoute(h.insideOut(), toFilter));
	// A CONTAINER'S INSIDE NEVER REACHES ITS OWN CONTROLS — the same row index the site's road may.
	CHECK_FALSE(h.b().mayRoute(h.insideOut(), toRackMacro));
	h.b().setRouteAmount(h.siteOut(), toRackMacro, 0.5f);
	h.b().setRouteAmount(h.chainSiteOut(), toChainMacro, 0.5f);
	h.b().setRouteAmount(h.insideOut(), toFilter, 0.25f);
	h.blocks(4);
	const double wantRack = landed(rackDesc.params[kRackMacro1], rackBase, 0.5, BothRoads::kPre);
	const double wantChain = landed(chainDesc.params[kChainMacro1], chainBase, 0.5, BothRoads::kChainPre);
	const double wantFilter = landed(filterDesc.params[h.cutoff], filterBase, 0.25, BothRoads::kInside);
	CHECK(wantRack != doctest::Approx(rackBase));
	CHECK(wantChain != doctest::Approx(chainBase));
	CHECK(wantFilter != doctest::Approx(filterBase));
	CHECK(h.resolved(h.rackId, kRackMacro1) == doctest::Approx(wantRack).epsilon(1e-6));
	CHECK(h.resolved(h.chain, kChainMacro1) == doctest::Approx(wantChain).epsilon(1e-6));
	CHECK(h.resolved(h.after, h.cutoff) == doctest::Approx(wantFilter).epsilon(1e-5));
	// THE HELD VALUE MOVES AND THE MACRO FOLLOWS — a route reading its own modulator, not a constant.
	h.siteOf(h.rackId)->pre().bridge()->setField(h.rackPre, 0, 0.0f);
	h.blocks(4);
	CHECK(h.resolved(h.rackId, kRackMacro1) == doctest::Approx(rackBase).epsilon(1e-6));
}

TEST_CASE("D3 lifted: a PRE modulator does not reach the devices INSIDE its Rack (rule 5 stays out), and a Rack's chain "
          "cannot read its own Rack's PRE") {
	BothRoads h;
	feathers::rack::RackBridge* inner = h.rackChain();
	const feathers::DeviceInstanceId filter = inner->addDevice(feathers::fx::filter::kTraits.tag);
	REQUIRE(filter != feathers::kNoInstance);
	h.blocks(2);
	CHECK_FALSE(inner->mayRoute(h.siteOut().asOuter(), feathers::backplane::Destination::control(filter, h.cutoff)));
	// …while the POST of the same site does read it.
	feathers::rack::RackBridge* post = h.siteOf(h.rackId)->post().bridge();
	const feathers::DeviceInstanceId postFilter = post->addDevice(feathers::fx::filter::kTraits.tag);
	REQUIRE(postFilter != feathers::kNoInstance);
	h.blocks(2);
	CHECK(post->mayRoute(h.siteOut().asOuter(), feathers::backplane::Destination::control(postFilter, h.cutoff)));
	CHECK_FALSE(post->mayRoute(h.insideOut().asOuter(), feathers::backplane::Destination::control(postFilter, h.cutoff)));
}

TEST_CASE("D3 lifted: the export sweep frees a Rack's dead CONTAINER row 0 while a route still reads its SITE row 0, and "
          "the site row stays live") {
	BothRoads h;
	const feathers::backplane::Source site = h.siteOut();
	const feathers::backplane::Source inside = h.insideOut();
	REQUIRE(site.exportRow() == inside.exportRow());
	const auto toRackMacro = feathers::backplane::Destination::control(h.rackId, kRackMacro1);
	h.b().setRouteAmount(site, toRackMacro, 0.5f);
	// THE CONTAINER ROW WAS READ, AND IS NO LONGER: a route to the Filter after the Rack, made and taken away (a row
	// minted and never routed is kept for the arming gesture, so it must have been read once to be swept).
	const auto toFilter = feathers::backplane::Destination::control(h.after, h.cutoff);
	h.b().setRouteAmount(inside, toFilter, 0.25f);
	h.blocks(2);
	(void)h.b().sweepExports();
	h.b().setRouteAmount(inside, toFilter, 0.0f);
	REQUIRE(h.b().routeAmount(inside, toFilter) == 0.0f);
	const void* rack = h.b().containerSurfaceOf(h.rackId);
	REQUIRE(rack != nullptr);
	const auto& ops = *feathers::rack::parallel::kDescriptor.container;
	CHECK(ops.exportedSource(rack, inside.exportRow()) == feathers::backplane::Source::output(h.inside, 0));
	for (int i = 0; i < 6; ++i) {
		h.blocks(1);
		(void)h.b().sweepExports();
	}
	CHECK(ops.exportedSource(rack, inside.exportRow()) == feathers::backplane::Source::nobody());
	// THE SITE ROW IS NOT THE CONTAINER'S TO FREE: same row, same source, same value arriving.
	CHECK(h.siteOut() == site);
	CHECK(h.b().routeAmount(site, toRackMacro) == 0.5f);
	REQUIRE(h.host.engine().sourceCell(site) != nullptr);
	CHECK(*h.host.engine().sourceCell(site) == doctest::Approx(BothRoads::kPre));
}

TEST_CASE("D3 lifted: POST runs AFTER a Rack's own mix — at Rack Mix 0 a POST that inverts the left still inverts it") {
	BothRoads h(false);
	const feathers::DeviceInstanceId rackId = h.b().addDevice(feathers::rack::parallel::kTraits.tag);
	REQUIRE(rackId != feathers::kNoInstance);
	h.b().setField(rackId, static_cast<size_t>(feathers::rack::parallel::Param::Mix), 0.0f);
	REQUIRE(h.b().openSite(rackId));
	h.blocks(2);
	REQUIRE(h.siteOf(rackId) != nullptr);
	feathers::rack::RackBridge* post = h.siteOf(rackId)->post().bridge();
	const feathers::DeviceInstanceId bus = post->addDevice(feathers::fx::busutil::kTraits.tag);
	REQUIRE(bus != feathers::kNoInstance);
	post->setField(bus, static_cast<size_t>(feathers::fx::busutil::Param::InvertL), 1.0f);
	h.blocks(40, 0.25f);
	// THE RACK AT MIX 0 PASSES ITS INPUT; POST, A SERIES INSERT AFTER IT, FLIPS THE LEFT. Inside the mix it would be gone.
	CHECK(h.l[63] == doctest::Approx(-0.25f).epsilon(1e-6));
	CHECK(h.r[63] == doctest::Approx(0.25f).epsilon(1e-6));
}

TEST_CASE("D3 lifted: a Rack and a Chain with sites save and load into a fresh host — both roads' routes by value, and "
          "the codec spells them as two kinds that round-trip") {
	std::vector<uint8_t> saved;
	feathers::rack::RackPatch patch;
	double wantRack = 0.0;
	double wantChain = 0.0;
	double wantFilter = 0.0;
	feathers::DeviceInstanceId rackId = feathers::kNoInstance;
	feathers::DeviceInstanceId chain = feathers::kNoInstance;
	feathers::DeviceInstanceId after = feathers::kNoInstance;
	size_t cutoff = 0;
	{
		BothRoads h;
		rackId = h.rackId;
		chain = h.chain;
		after = h.after;
		cutoff = h.cutoff;
		const float rackBase = h.resolved(h.rackId, kRackMacro1);
		const float chainBase = h.resolved(h.chain, kChainMacro1);
		const float filterBase = h.resolved(h.after, h.cutoff);
		h.b().setRouteAmount(h.siteOut(), feathers::backplane::Destination::control(h.rackId, kRackMacro1), 0.5f);
		h.b().setRouteAmount(h.chainSiteOut(), feathers::backplane::Destination::control(h.chain, kChainMacro1), 0.5f);
		h.b().setRouteAmount(h.insideOut(), feathers::backplane::Destination::control(h.after, h.cutoff), 0.25f);
		h.blocks(4);
		wantRack = landed(feathers::rack::parallel::kDescriptor.params[kRackMacro1], rackBase, 0.5, BothRoads::kPre);
		wantChain = landed(feathers::composite::kDescriptor.params[kChainMacro1], chainBase, 0.5, BothRoads::kChainPre);
		wantFilter = landed(feathers::fx::filter::kDescriptor.params[h.cutoff], filterBase, 0.25, BothRoads::kInside);
		patch = h.b().patch();
		const feathers::device::ByteSink sink { &saved, [](void* ctx, const uint8_t* data, size_t n) {
			                                       auto* v = static_cast<std::vector<uint8_t>*>(ctx);
			                                       v->insert(v->end(), data, data + n);
		                                       } };
		REQUIRE(h.host.saveState(sink));
	}
	// THE CODEC: two kinds, each carrying its own bit, through text and back.
	size_t site = 0;
	size_t inside = 0;
	for (const auto& route : patch.route) {
		if (route.source.kind == static_cast<uint32_t>(feathers::rack::SourceKind::Export)) {
			++site;
			CHECK(feathers::backplane::Source { route.source.a, route.source.b }.isSiteExport());
		}
		if (route.source.kind == static_cast<uint32_t>(feathers::rack::SourceKind::ContainerExport)) {
			++inside;
			CHECK(feathers::backplane::Source { route.source.a, route.source.b }.isContainerExport());
		}
	}
	CHECK(site == 2);
	CHECK(inside == 1);
	std::string text;
	REQUIRE(feathers::rack::rackToText(patch, &text, false));
	feathers::rack::RackPatch back;
	REQUIRE(feathers::rack::parseRackText(text, &back));
	REQUIRE(back.route.size() == patch.route.size());
	for (size_t i = 0; i < patch.route.size(); ++i) {
		CHECK(back.route[i].source.kind == patch.route[i].source.kind);
		CHECK(back.route[i].source.a == patch.route[i].source.a);
		CHECK(back.route[i].source.b == patch.route[i].source.b);
	}
	// A FRESH HOST, by value.
	BothRoads again(false);
	REQUIRE(again.host.loadState(saved.data(), saved.size()));
	again.blocks(6);
	CHECK(again.resolved(rackId, kRackMacro1) == doctest::Approx(wantRack).epsilon(1e-6));
	CHECK(again.resolved(chain, kChainMacro1) == doctest::Approx(wantChain).epsilon(1e-6));
	CHECK(again.resolved(after, cutoff) == doctest::Approx(wantFilter).epsilon(1e-5));
}

// ---------------------------------------------------------------------------------------------
// THE AUDITION PATCH, 2026-10-29: lane → Chain → an effect with a site → a Rack in its PRE. The enclosing Chain's macros
// must reach the PRE Rack's own controls (and a Filter's in PRE, and in POST) — by value.
// ---------------------------------------------------------------------------------------------

namespace {

/// LANE → CHAIN (Macro 1 at 0.5) → a Filter holding a site → a Rack and a Filter in its PRE, a Filter in its POST.
struct NestedSite {
	RungFive rig;
	feathers::DeviceInstanceId chain = feathers::kNoInstance;
	feathers::rack::RackBridge* inChain = nullptr;
	feathers::DeviceInstanceId holder = feathers::kNoInstance;
	feathers::rack::SiteChains* site = nullptr;
	feathers::DeviceInstanceId preRack = feathers::kNoInstance;
	feathers::DeviceInstanceId preFilter = feathers::kNoInstance;
	feathers::DeviceInstanceId postFilter = feathers::kNoInstance;

	NestedSite() {
		chain = rig.add(feathers::composite::kTraits.tag);
		rig.blocks(2);
		inChain = feathers::rack::bridgeOf(rig.bridge.fxBodies().surfaceOf(chain));
		REQUIRE(inChain != nullptr);
		holder = inChain->addDevice(feathers::fx::filter::kTraits.tag);
		REQUIRE(holder != feathers::kNoInstance);
		REQUIRE(inChain->openSite(holder));
		rig.blocks(2);
		site = static_cast<feathers::rack::SiteChains*>(inChain->containerSurfaceOf(feathers::siteKeyOf(holder)));
		REQUIRE(site != nullptr);
		preRack = site->pre().bridge()->addDevice(feathers::rack::parallel::kTraits.tag);
		preFilter = site->pre().bridge()->addDevice(feathers::fx::filter::kTraits.tag);
		postFilter = site->post().bridge()->addDevice(feathers::fx::filter::kTraits.tag);
		REQUIRE(preRack != feathers::kNoInstance);
		REQUIRE(preFilter != feathers::kNoInstance);
		REQUIRE(postFilter != feathers::kNoInstance);
		rig.bridge.setDeviceControl(chain, kChainMacro1, 0.5f);
		rig.blocks(3);
	}
	static float resolvedIn(const feathers::rack::RackEngine& e, feathers::DeviceInstanceId id, size_t control) {
		const size_t at = e.slotOf(id);
		REQUIRE(at != feathers::rack::RackEngine::kNoSlot);
		return e.resolvedFieldForTest(at, control);
	}
};

} // namespace

TEST_CASE("Audition 2026-10-29: the enclosing Chain's Macro 1 moves a PRE Rack's Mix, a PRE Filter's cutoff and a POST "
          "Filter's cutoff, by value") {
	NestedSite h;
	const auto mix = static_cast<size_t>(feathers::rack::parallel::Param::Mix);
	const feathers::backplane::Source macro = feathers::backplane::Source::outerCore(0); // the Chain's Macro 1, from inside a lane
	feathers::rack::RackBridge* pre = h.site->pre().bridge();
	feathers::rack::RackBridge* post = h.site->post().bridge();
	const auto toRackMix = feathers::backplane::Destination::control(h.preRack, mix);
	const auto toPreCut = feathers::backplane::Destination::control(h.preFilter, h.rig.cutoff);
	const auto toPostCut = feathers::backplane::Destination::control(h.postFilter, h.rig.cutoff);
	const float mixBase = NestedSite::resolvedIn(h.site->pre().engine(), h.preRack, mix);
	const float preBase = NestedSite::resolvedIn(h.site->pre().engine(), h.preFilter, h.rig.cutoff);
	const float postBase = NestedSite::resolvedIn(h.site->post().engine(), h.postFilter, h.rig.cutoff);
	CHECK(pre->mayRoute(macro, toRackMix));
	CHECK(pre->mayRoute(macro, toPreCut));
	CHECK(post->mayRoute(macro, toPostCut));
	pre->setRouteAmount(macro, toRackMix, -0.5f);
	pre->setRouteAmount(macro, toPreCut, 0.25f);
	post->setRouteAmount(macro, toPostCut, 0.25f);
	CHECK(pre->routeAmount(macro, toRackMix) == -0.5f);
	h.rig.blocks(6);
	const auto& rackP = feathers::rack::parallel::kDescriptor.params[mix];
	const auto& cutP = feathers::fx::filter::kDescriptor.params[h.rig.cutoff];
	CHECK(landed(rackP, mixBase, -0.5, 0.5) != doctest::Approx(mixBase)); // each claim is about a value that moved
	CHECK(landed(cutP, preBase, 0.25, 0.5) != doctest::Approx(preBase));
	CHECK(NestedSite::resolvedIn(h.site->pre().engine(), h.preRack, mix)
	      == doctest::Approx(landed(rackP, mixBase, -0.5, 0.5)).epsilon(1e-6));
	CHECK(NestedSite::resolvedIn(h.site->pre().engine(), h.preFilter, h.rig.cutoff)
	      == doctest::Approx(landed(cutP, preBase, 0.25, 0.5)).epsilon(1e-5));
	CHECK(NestedSite::resolvedIn(h.site->post().engine(), h.postFilter, h.rig.cutoff)
	      == doctest::Approx(landed(cutP, postBase, 0.25, 0.5)).epsilon(1e-5));
}

// ---------------------------------------------------------------------------------------------
// ANY NUMBER OF HOSTS OUT (modulation works more than two levels deep). The top host's
// Macro 1 reaches a Filter's cutoff 1, 2, 3 and 4 hosts in, through a Chain, a Rack, a site's PRE and a Chain inside it;
// each spelled with its hops (`asOuter()` once per host), each read by value, each through a fresh host after a save.
// ---------------------------------------------------------------------------------------------

namespace {

/// THE SOURCE `hops` HOSTS OUT, as the reader spells it.
feathers::backplane::Source hopsOut(feathers::backplane::Source s, uint32_t hops) {
	for (uint32_t i = 0; i < hops; ++i) {
		s = s.asOuter();
	}
	return s;
}

/// TOP (the rack plugin's host, Macro 1 at 0.5): [a Chain C] then a Filter after it. Inside C: a Filter (depth 1) and
/// a Rack R; in R's first chain a Filter F with a site (depth 2); in F's PRE a Filter (depth 3) and a Chain D; in D a
/// Filter (depth 4).
struct Deep {
	feathers::rack::InnerChain host { feathers::rack::kRackAxes };
	feathers::DeviceInstanceId chainC = feathers::kNoInstance;
	feathers::DeviceInstanceId follower = feathers::kNoInstance;
	std::array<feathers::rack::RackBridge*, 5> bridge {};        ///< the host each depth's Filter sits in (1..4)
	std::array<feathers::DeviceInstanceId, 5> filter {};         ///< each depth's Filter
	size_t cutoff = 0;
	std::vector<float> l = std::vector<float>(64, 0.0f);
	std::vector<float> r = std::vector<float>(64, 0.0f);

	void blocks(int n) {
		for (int i = 0; i < n; ++i) {
			std::fill(l.begin(), l.end(), 0.1f);
			std::fill(r.begin(), r.end(), 0.1f);
			host.process(l.data(), r.data(), static_cast<int>(l.size()));
		}
	}
	void prepare() {
		host.prepare(48000.0, 4096);
		host.adopt(feathers::hostCatalogue());
		const auto& desc = feathers::fx::filter::kDescriptor;
		while (cutoff < desc.params.size() && (!desc.params[cutoff].modulatable || static_cast<int>(cutoff) == desc.traits.enableControl)) {
			++cutoff;
		}
	}
	/// Find the bridges and Filters of a tree already standing (built, or loaded).
	void find() {
		feathers::rack::RackBridge& top = *host.bridge();
		chainC = top.deviceAtPosition(0);
		follower = top.deviceAtPosition(1);
		feathers::rack::RackBridge* c = feathers::rack::bridgeOf(top.containerSurfaceOf(chainC));
		REQUIRE(c != nullptr);
		bridge[1] = c;
		filter[1] = c->deviceAtPosition(0);
		auto* rk = feathers::rack::parallel::rackOf(c->containerSurfaceOf(c->deviceAtPosition(1)));
		REQUIRE(rk != nullptr);
		REQUIRE(rk->chainAt(0) != nullptr);
		bridge[2] = rk->chainAt(0)->bridge();
		filter[2] = bridge[2]->deviceAtPosition(0);
		auto* site = static_cast<feathers::rack::SiteChains*>(bridge[2]->containerSurfaceOf(feathers::siteKeyOf(filter[2])));
		REQUIRE(site != nullptr);
		bridge[3] = site->pre().bridge();
		filter[3] = bridge[3]->deviceAtPosition(0);
		bridge[4] = feathers::rack::bridgeOf(bridge[3]->containerSurfaceOf(bridge[3]->deviceAtPosition(1)));
		REQUIRE(bridge[4] != nullptr);
		filter[4] = bridge[4]->deviceAtPosition(0);
	}
	void build() {
		prepare();
		feathers::rack::RackBridge& top = *host.bridge();
		chainC = top.addDevice(feathers::composite::kTraits.tag);
		follower = top.addDevice(feathers::fx::filter::kTraits.tag);
		blocks(2);
		feathers::rack::RackBridge* c = feathers::rack::bridgeOf(top.containerSurfaceOf(chainC));
		REQUIRE(c != nullptr);
		REQUIRE(c->addDevice(feathers::fx::filter::kTraits.tag) != feathers::kNoInstance);
		const feathers::DeviceInstanceId rackId = c->addDevice(feathers::rack::parallel::kTraits.tag);
		blocks(2);
		auto* rk = feathers::rack::parallel::rackOf(c->containerSurfaceOf(rackId));
		REQUIRE(rk != nullptr);
		if (rk->chainCount() == 0) {
			(void)rk->addChain();
		}
		feathers::rack::RackBridge* inRack = rk->chainAt(0)->bridge();
		const feathers::DeviceInstanceId f2 = inRack->addDevice(feathers::fx::filter::kTraits.tag);
		REQUIRE(inRack->openSite(f2));
		blocks(2);
		auto* site = static_cast<feathers::rack::SiteChains*>(inRack->containerSurfaceOf(feathers::siteKeyOf(f2)));
		REQUIRE(site != nullptr);
		REQUIRE(site->pre().bridge()->addDevice(feathers::fx::filter::kTraits.tag) != feathers::kNoInstance);
		const feathers::DeviceInstanceId d = site->pre().bridge()->addDevice(feathers::composite::kTraits.tag);
		blocks(2);
		feathers::rack::RackBridge* inD = feathers::rack::bridgeOf(site->pre().bridge()->containerSurfaceOf(d));
		REQUIRE(inD != nullptr);
		REQUIRE(inD->addDevice(feathers::fx::filter::kTraits.tag) != feathers::kNoInstance);
		top.setMacroByHand(0, 0.5f);
		blocks(3);
		find();
	}
	float resolved(size_t depth) {
		const feathers::rack::RackEngine& e = bridge[depth]->engineForTest();
		const size_t at = e.slotOf(filter[depth]);
		REQUIRE(at != feathers::rack::RackEngine::kNoSlot);
		return e.resolvedFieldForTest(at, cutoff);
	}
};

} // namespace

TEST_CASE("Any depth: the top host's Macro 1 moves a Filter's cutoff 1, 2, 3 and 4 hosts in — through a Chain, a Rack, a "
          "site's PRE and a Chain inside it — by value, and again in a fresh host after a save") {
	std::vector<uint8_t> saved;
	std::array<double, 5> want {};
	const auto& p = feathers::fx::filter::kDescriptor.params;
	{
		Deep h;
		h.build();
		for (uint32_t depth = 1; depth <= 4; ++depth) {
			CAPTURE(depth);
			const float base = h.resolved(depth);
			const auto source = hopsOut(feathers::backplane::Source::core(0), depth);
			CHECK(source.outerHops() == depth);
			const auto dest = feathers::backplane::Destination::control(h.filter[depth], h.cutoff);
			REQUIRE(h.bridge[depth]->mayRoute(source, dest));
			const float amount = 0.1f * static_cast<float>(depth);
			h.bridge[depth]->setRouteAmount(source, dest, amount);
			want[depth] = landed(p[h.cutoff], base, amount, 0.5);
			CHECK(want[depth] != doctest::Approx(base));
		}
		h.blocks(6);
		for (uint32_t depth = 1; depth <= 4; ++depth) {
			CAPTURE(depth);
			CHECK(h.resolved(depth) == doctest::Approx(want[depth]).epsilon(1e-5));
		}
		// THE MACRO MOVES AND EVERY DEPTH FOLLOWS — it is the top's Macro 1 they read, not a constant.
		h.host.bridge()->setMacroByHand(0, 0.0f);
		h.blocks(6);
		for (uint32_t depth = 1; depth <= 4; ++depth) {
			CAPTURE(depth);
			CHECK(h.resolved(depth) != doctest::Approx(want[depth]).epsilon(1e-5));
		}
		h.host.bridge()->setMacroByHand(0, 0.5f);
		h.blocks(3);
		const feathers::device::ByteSink sink { &saved, [](void* ctx, const uint8_t* data, size_t n) {
			                                       auto* v = static_cast<std::vector<uint8_t>*>(ctx);
			                                       v->insert(v->end(), data, data + n);
		                                       } };
		REQUIRE(h.host.saveState(sink));
	}
	Deep again;
	again.prepare();
	REQUIRE(again.host.loadState(saved.data(), saved.size()));
	again.blocks(10);
	again.find();
	for (uint32_t depth = 1; depth <= 4; ++depth) {
		CAPTURE(depth);
		CHECK(again.resolved(depth) == doctest::Approx(want[depth]).epsilon(1e-5));
	}
}

namespace {

/// TOP (the rack plugin's host, Macro 1 at 0.5): a Chain; inside it a Filter and the next Chain; and so on, `kMaxUsefulDepth`
/// Chains down, so there is a Filter at EVERY depth from 1 to the deepest a hand may build and a file reopens.
struct Deepest {
	static constexpr size_t kLevels = feathers::rack::kMaxUsefulDepth;
	feathers::rack::InnerChain host { feathers::rack::kRackAxes };
	std::array<feathers::rack::RackBridge*, kLevels + 1> bridge {}; ///< the host each depth's Filter sits in
	std::array<feathers::DeviceInstanceId, kLevels + 1> filter {};  ///< each depth's Filter
	size_t cutoff = 0;
	std::vector<float> l = std::vector<float>(64, 0.0f);
	std::vector<float> r = std::vector<float>(64, 0.0f);

	void blocks(int n) {
		for (int i = 0; i < n; ++i) {
			std::fill(l.begin(), l.end(), 0.1f);
			std::fill(r.begin(), r.end(), 0.1f);
			host.process(l.data(), r.data(), static_cast<int>(l.size()));
		}
	}
	void prepare() {
		host.prepare(48000.0, 4096);
		host.adopt(feathers::hostCatalogue());
		const auto& desc = feathers::fx::filter::kDescriptor;
		while (cutoff < desc.params.size() && (!desc.params[cutoff].modulatable || static_cast<int>(cutoff) == desc.traits.enableControl)) {
			++cutoff;
		}
	}
	void find() {
		feathers::rack::RackBridge* at = host.bridge();
		for (size_t depth = 1; depth <= kLevels; ++depth) {
			CAPTURE(depth);
			const size_t chainAt = depth == 1 ? 0 : 1;
			at = feathers::rack::bridgeOf(at->containerSurfaceOf(at->deviceAtPosition(chainAt)));
			REQUIRE(at != nullptr);
			bridge[depth] = at;
			filter[depth] = at->deviceAtPosition(0);
			REQUIRE(filter[depth] != feathers::kNoInstance);
		}
	}
	void build() {
		prepare();
		feathers::rack::RackBridge* at = host.bridge();
		for (size_t depth = 1; depth <= kLevels; ++depth) {
			CAPTURE(depth);
			const feathers::DeviceInstanceId chain = at->addDevice(feathers::composite::kTraits.tag);
			REQUIRE(chain != feathers::kNoInstance);
			blocks(2);
			at = feathers::rack::bridgeOf(at->containerSurfaceOf(chain));
			REQUIRE(at != nullptr);
			REQUIRE(at->addDevice(feathers::fx::filter::kTraits.tag) != feathers::kNoInstance);
		}
		host.bridge()->setMacroByHand(0, 0.5f);
		blocks(3);
		find();
	}
	float resolved(size_t depth) {
		const feathers::rack::RackEngine& e = bridge[depth]->engineForTest();
		const size_t at = e.slotOf(filter[depth]);
		REQUIRE(at != feathers::rack::RackEngine::kNoSlot);
		return e.resolvedFieldForTest(at, cutoff);
	}
};

} // namespace

TEST_CASE("Deepest: the top host's Macro 1 moves a Filter's cutoff at EVERY depth a hand may nest to, 1 to 20, by value — "
          "and again in a fresh host after a save; no wall below the nesting limit") {
	// **MODULATION KEEPS FLOWING: no depth is a wall where the recursion breaks.** The hop
	// count was four bits (16 hosts) while a device may sit `kMaxUsefulDepth` (20) in, so a host macro silently stopped
	// reaching depths 17–20. Every depth is routed and read here, so a wall at any depth is a red at that depth.
	static_assert(Deepest::kLevels == 20, "the nesting limit; if it moves, this case moves with it");
	std::vector<uint8_t> saved;
	std::array<double, Deepest::kLevels + 1> want {};
	const auto& p = feathers::fx::filter::kDescriptor.params;
	{
		Deepest h;
		h.build();
		for (uint32_t depth = 1; depth <= Deepest::kLevels; ++depth) {
			CAPTURE(depth);
			const float base = h.resolved(depth);
			const auto source = hopsOut(feathers::backplane::Source::core(0), depth);
			CHECK(source.outerHops() == depth);
			const auto dest = feathers::backplane::Destination::control(h.filter[depth], h.cutoff);
			CHECK(h.bridge[depth]->mayRoute(source, dest));
			const float amount = 0.02f + 0.03f * static_cast<float>(depth);
			if (h.bridge[depth]->mayRoute(source, dest)) {
				h.bridge[depth]->setRouteAmount(source, dest, amount);
			}
			want[depth] = landed(p[h.cutoff], base, amount, 0.5);
			REQUIRE(want[depth] != doctest::Approx(base));
		}
		h.blocks(6);
		for (uint32_t depth = 1; depth <= Deepest::kLevels; ++depth) {
			CAPTURE(depth);
			CHECK(h.resolved(depth) == doctest::Approx(want[depth]).epsilon(1e-5));
		}
		// THE MACRO MOVES AND EVERY DEPTH FOLLOWS — they read the top's Macro 1, not a value stamped once.
		h.host.bridge()->setMacroByHand(0, 0.0f);
		h.blocks(6);
		for (uint32_t depth = 1; depth <= Deepest::kLevels; ++depth) {
			CAPTURE(depth);
			CHECK(h.resolved(depth) != doctest::Approx(want[depth]).epsilon(1e-5));
		}
		h.host.bridge()->setMacroByHand(0, 0.5f);
		h.blocks(3);
		const feathers::device::ByteSink sink { &saved, [](void* ctx, const uint8_t* data, size_t n) {
			                                       auto* v = static_cast<std::vector<uint8_t>*>(ctx);
			                                       v->insert(v->end(), data, data + n);
		                                       } };
		REQUIRE(h.host.saveState(sink));
	}
	// INTO A FRESH HOST AS FEATHERS CHAIN LOADS ITS OWN STATE: parsed and handed to the top's bridge (`rack_clap.cpp`'s
	// `stateLoad`). Not `InnerChain::loadState`, which is a Chain DEVICE's body road and counts itself one level deep —
	// the top host is level 0, which is what makes a Filter twenty Chains in twenty hops out.
	Deepest again;
	again.prepare();
	feathers::rack::RackPatch patch;
	REQUIRE(feathers::rack::parseRackText(std::string_view(reinterpret_cast<const char*>(saved.data()), saved.size()), &patch));
	(void)again.host.bridge()->loadPatch(patch);
	again.blocks(10);
	again.find();
	for (uint32_t depth = 1; depth <= Deepest::kLevels; ++depth) {
		CAPTURE(depth);
		CHECK(again.resolved(depth) == doctest::Approx(want[depth]).epsilon(1e-5));
	}
}

TEST_CASE("Deepest: an outer source's hop count saves as its own key at every depth to 32, and a file from before it "
          "(the count in bits 24–27 of the index) still reads") {
	using feathers::rack::PatchCoord;
	using feathers::rack::RackPatch;
	using feathers::rack::SourceKind;
	const auto patchWith = [](uint32_t index) {
		RackPatch patch;
		RackPatch::Route r;
		r.source = PatchCoord { static_cast<uint32_t>(SourceKind::Outer), feathers::kNoInstance, index, {} };
		r.destination = PatchCoord { static_cast<uint32_t>(feathers::rack::DestKind::Device), 7, 0, "cutoff" };
		r.amount = 0.25f;
		patch.route.push_back(r);
		return patch;
	};
	for (const uint32_t hops : { 1u, 2u, 16u, 17u, 20u, 32u }) {
		CAPTURE(hops);
		const auto source = hopsOut(feathers::backplane::Source::core(3), hops);
		REQUIRE(source.outerHops() == hops);
		std::string text;
		REQUIRE(feathers::rack::rackToText(patchWith(source.index), &text, false));
		CHECK((text.find("\"hops\"") != std::string::npos) == (hops > 1));
		RackPatch back;
		REQUIRE(feathers::rack::parseRackText(text, &back));
		REQUIRE(back.route.size() == 1);
		CHECK(back.route[0].source.b == source.index);
		CHECK(feathers::backplane::Source { back.route[0].source.a, back.route[0].source.b }.outerHops() == hops);
	}
	// ONE HOP PAST THE FIELD names nothing — `asOuter()` gives up rather than wrapping to a near host.
	CHECK(hopsOut(feathers::backplane::Source::core(3), 33).index == static_cast<uint32_t>(-1));

	// THE OLD SPELLING: four hosts out, written before the `hops` key, carried the count (3 more) at bit 24.
	const auto four = hopsOut(feathers::backplane::Source::core(3), 4);
	std::string text;
	REQUIRE(feathers::rack::rackToText(patchWith(four.index), &text, false));
	const uint32_t clean = four.index & ~feathers::backplane::Source::kOuterDepthMask;
	const std::string now = "\"index\":" + std::to_string(clean) + ",\"hops\":3";
	const size_t at = text.find(now);
	REQUIRE(at != std::string::npos);
	text.replace(at, now.size(), "\"index\":" + std::to_string(clean | (3u << 24)));
	RackPatch back;
	REQUIRE(feathers::rack::parseRackText(text, &back));
	REQUIRE(back.route.size() == 1);
	CHECK(back.route[0].source.b == four.index);
	CHECK(feathers::backplane::Source { back.route[0].source.a, back.route[0].source.b }.outerHops() == 4);
}

TEST_CASE("Any depth: a source that would close a loop is refused at every depth — the Chain's own export, read back "
          "inside the Chain however deep — while the top's macro, spelled the same way, is admitted") {
	Deep h;
	h.build();
	// A MODULATOR INSIDE THE CHAIN, EXPORTED: the top host reads it as the Chain's own output (NZ (b)).
	const feathers::DeviceInstanceId inner = h.bridge[1]->addDevice(feathers::mod::macros::kTraits.tag);
	REQUIRE(inner != feathers::kNoInstance);
	h.blocks(2);
	const auto exported = h.host.bridge()->containerExport(h.chainC, feathers::backplane::Source::output(inner, 0));
	REQUIRE(exported.isContainerExport());
	for (uint32_t depth = 1; depth <= 4; ++depth) {
		CAPTURE(depth);
		const auto dest = feathers::backplane::Destination::control(h.filter[depth], h.cutoff);
		// THE CHAIN'S OUTPUT DEPENDS ON EVERYTHING INSIDE IT: read back in, it is a loop at any depth.
		CHECK_FALSE(h.bridge[depth]->mayRoute(hopsOut(exported, depth), dest));
		h.bridge[depth]->setRouteAmount(hopsOut(exported, depth), dest, 0.5f);
		CHECK(h.bridge[depth]->routeAmount(hopsOut(exported, depth), dest) == 0.0f);
		CHECK(h.bridge[depth]->mayRoute(hopsOut(feathers::backplane::Source::core(0), depth), dest));
		// AND ONE HOP TOO FAR NAMES NOTHING (past the top host).
		CHECK_FALSE(h.bridge[depth]->mayRoute(hopsOut(feathers::backplane::Source::core(0), depth + 1), dest));
		// THE ENGINE ANSWERS THE SAME, through its own readers (`RackEngine::upstreamOf` forwarding hop by hop).
		const feathers::backplane::DeviceView view = h.bridge[depth]->engineForTest().devices();
		REQUIRE(view.outerUpstream != nullptr);
		CHECK(view.outerUpstream(view.outerHost, hopsOut(feathers::backplane::Source::core(0), depth).inner()));
		CHECK_FALSE(view.outerUpstream(view.outerHost, hopsOut(exported, depth).inner()));
	}
}

TEST_CASE("Any depth: when the source device at the top is REMOVED and its slot taken by another, every reader 1 to 4 "
          "hosts in lets go — none goes on reading the slot's new occupant") {
	Deep h;
	h.build();
	feathers::rack::RackBridge& top = *h.host.bridge();
	const feathers::DeviceInstanceId m = top.addDevice(feathers::mod::macros::kTraits.tag);
	REQUIRE(m != feathers::kNoInstance);
	REQUIRE(top.moveDeviceTo(m, 0)); // ahead of the Chain, so it is upstream at every depth
	top.setField(m, 0, 0.5f);
	h.blocks(3);
	const auto& p = feathers::fx::filter::kDescriptor.params;
	std::array<float, 5> base {};
	for (uint32_t depth = 1; depth <= 4; ++depth) {
		CAPTURE(depth);
		base[depth] = h.resolved(depth);
		const auto source = hopsOut(feathers::backplane::Source::output(m, 0), depth);
		const auto dest = feathers::backplane::Destination::control(h.filter[depth], h.cutoff);
		REQUIRE(h.bridge[depth]->mayRoute(source, dest));
		h.bridge[depth]->setRouteAmount(source, dest, 0.25f);
	}
	h.blocks(6);
	for (uint32_t depth = 1; depth <= 4; ++depth) {
		CAPTURE(depth);
		CHECK(h.resolved(depth) == doctest::Approx(landed(p[h.cutoff], base[depth], 0.25, 0.5)).epsilon(1e-5));
	}
	// THE SOURCE GOES, AND ANOTHER MACROS — at a different value — TAKES WHAT IT LEFT.
	top.removeDevice(m);
	h.blocks(3);
	const feathers::DeviceInstanceId other = top.addDevice(feathers::mod::macros::kTraits.tag);
	REQUIRE(other != feathers::kNoInstance);
	REQUIRE(top.moveDeviceTo(other, 0));
	top.setField(other, 0, 1.0f);
	h.blocks(10);
	for (uint32_t depth = 1; depth <= 4; ++depth) {
		CAPTURE(depth);
		CHECK(h.resolved(depth) == doctest::Approx(base[depth]).epsilon(1e-5)); // the route went with its source
	}
}

TEST_CASE("NZ release in Feathers' lane: a site's and a Chain's export rows are freed when their last route goes, once the engine has adopted a later publish — and a live route keeps its row") {
	feathers::ParamSet params;
	feathers::Synth synth { params };
	feathers::ParamBridge bridge;
	synth.activate(48000.0, 4096);
	synth.setCatalogue(feathers::hostCatalogue());
	synth.setOuterGateMaker(&feathers::rack::ChainRecord::gateOver, &bridge.chainRecord());
	std::vector<float> l(64, 0.0f);
	std::vector<float> r(64, 0.0f);
	size_t freed = 0;
	// THE PLUGIN'S ORDER: blocks (the engine adopts the routes), then the service beat's sweep.
	const auto blocks = [&](int n) {
		for (int i = 0; i < n; ++i) {
			feathers::publishEditorLanes(bridge, params, synth);
			(void)synth.provisionDevices(bridge.chainState(), bridge.publishedChains().sequence(), &bridge.fxBodies());
			synth.render(l.data(), r.data(), 64);
			freed += bridge.sweepExports(params.routesGeneration());
		}
	};
	const auto add = [&](const char* tag) {
		const size_t slot = bridge.addFxDevice(tag);
		REQUIRE(slot < feathers::kMaxFxSlots);
		bridge.refresh();
		return bridge.fxDeviceAt(slot);
	};
	const feathers::DeviceInstanceId holder = add(feathers::fx::filter::kTraits.tag);
	const feathers::DeviceInstanceId chain = add(feathers::composite::kTraits.tag);
	const feathers::DeviceInstanceId after = add(feathers::fx::filter::kTraits.tag);
	REQUIRE(bridge.openFxSite(holder));
	blocks(2);
	auto* site = static_cast<feathers::rack::SiteChains*>(bridge.fxBodies().surfaceOf(feathers::siteKeyOf(holder)));
	REQUIRE(site != nullptr);
	const feathers::DeviceInstanceId preMacros = site->pre().bridge()->addDevice(feathers::mod::macros::kTraits.tag);
	REQUIRE(preMacros != feathers::kNoInstance);
	feathers::rack::RackBridge* inChain = feathers::rack::bridgeOf(bridge.fxBodies().surfaceOf(chain));
	REQUIRE(inChain != nullptr);
	const feathers::DeviceInstanceId chainMacros = inChain->addDevice(feathers::mod::macros::kTraits.tag);
	REQUIRE(chainMacros != feathers::kNoInstance);
	blocks(2);

	const auto& desc = feathers::fx::filter::kDescriptor;
	size_t cutoff = 0;
	while (cutoff < desc.params.size() && (!desc.params[cutoff].modulatable || static_cast<int>(cutoff) == desc.traits.enableControl)) {
		++cutoff;
	}
	const auto dest = feathers::backplane::Destination::control(after, cutoff);
	const feathers::backplane::Source fromSite = bridge.fxSiteExport(holder, feathers::backplane::Source::output(preMacros, 0));
	const feathers::backplane::Source fromChain =
	    bridge.fxContainerExport(chain, feathers::backplane::Source::output(chainMacros, 0));
	REQUIRE(fromSite.isSiteExport());
	REQUIRE(fromChain.isContainerExport());
	bridge.setRouteAmount(fromSite, dest, 0.25f);
	bridge.setRouteAmount(fromChain, dest, 0.25f);
	blocks(4);
	// LIVE ROUTES KEEP THEIR ROWS.
	CHECK(site->exportedSource(fromSite.exportRow()) == feathers::backplane::Source::output(preMacros, 0));
	const feathers::device::Descriptor* cd = bridge.descriptorOf(chain);
	void* cs = bridge.fxBodies().surfaceOf(chain);
	CHECK(cd->container->exportedSource(cs, fromChain.exportRow()) == feathers::backplane::Source::output(chainMacros, 0));
	CHECK(freed == 0);

	// THE ROUTES GO; the rows go after the engine has adopted a later publish.
	bridge.setRouteAmount(fromSite, dest, 0.0f);
	bridge.setRouteAmount(fromChain, dest, 0.0f);
	// CONDEMNED, NOT YET FREED, however many beats come before the engine adopts the publish that dropped them.
	CHECK(bridge.sweepExports(params.routesGeneration()) == 0);
	CHECK(bridge.sweepExports(params.routesGeneration()) == 0);
	CHECK(site->exportedSource(fromSite.exportRow()) == feathers::backplane::Source::output(preMacros, 0));
	blocks(4);
	CHECK(freed == 2);
	CHECK(site->exportedSource(fromSite.exportRow()) == feathers::backplane::Source::nobody());
	CHECK(cd->container->exportedSource(cs, fromChain.exportRow()) == feathers::backplane::Source::nobody());
}

// ---------------------------------------------------------------------------------------------
// NZ, ANY DEPTH (modulation keeps flowing so no depth is a wall where the recursion breaks, with minimal overhead for
// hypothetical routes until they are actually routed, then full recursion). A container's export row may name an INNER
// holder's export row, re-minted one hop out per level; the
// inner table stays its owner, and a row exists only while a route reaches through it.
// ---------------------------------------------------------------------------------------------

namespace {

/// FEATHERS' LANE: [C1] [a Filter], where C1 holds C2 holds … holds C`depth` (Chains), and the innermost holds a Macros at
/// `kValue`. The plugin's road: blocks, then its service beat (the containers' service, their own controls, the sweep).
struct DeepExport {
	static constexpr float kValue = 0.5f;
	RungFive rig;
	std::vector<feathers::DeviceInstanceId> chains;           ///< C1 … Cdepth, as their hosts name them
	std::vector<feathers::rack::RackBridge*> insides;          ///< the bridge inside each
	feathers::DeviceInstanceId after = feathers::kNoInstance;  ///< the Filter after C1
	feathers::DeviceInstanceId macros = feathers::kNoInstance; ///< in the innermost
	explicit DeepExport(size_t depth) {
		chains.push_back(rig.add(feathers::composite::kTraits.tag));
		after = rig.add(feathers::fx::filter::kTraits.tag);
		rig.blocks(2);
		insides.push_back(feathers::rack::bridgeOf(rig.bridge.fxBodies().surfaceOf(chains[0])));
		REQUIRE(insides.back() != nullptr);
		for (size_t level = 1; level < depth; ++level) {
			const feathers::DeviceInstanceId c = insides.back()->addDevice(feathers::composite::kTraits.tag);
			REQUIRE(c != feathers::kNoInstance);
			rig.blocks(2);
			service();
			feathers::rack::RackBridge* in = feathers::rack::bridgeOf(insides.back()->containerSurfaceOf(c));
			REQUIRE(in != nullptr);
			chains.push_back(c);
			insides.push_back(in);
		}
		macros = insides.back()->addDevice(feathers::mod::macros::kTraits.tag);
		REQUIRE(macros != feathers::kNoInstance);
		insides.back()->setField(macros, 0, kValue);
		settle();
	}
	void service() {
		rig.synth.serviceContainers(rig.bridge.chainState(), &rig.bridge.fxBodies());
		(void)rig.bridge.serviceOwnControls();
		(void)rig.bridge.sweepExports(rig.params.routesGeneration());
	}
	void settle(int beats = 4) {
		for (int i = 0; i < beats; ++i) {
			rig.blocks(2);
			service();
		}
	}
	/// MINTED ROWS, level by level: C1's table first.
	[[nodiscard]] std::vector<size_t> rows() const {
		std::vector<size_t> out;
		for (size_t level = 0; level < chains.size(); ++level) {
			void* surface = level == 0 ? rig.bridge.fxBodies().surfaceOf(chains[0])
			                           : insides[level - 1]->containerSurfaceOf(chains[level]);
			const feathers::device::Descriptor* d = level == 0 ? rig.bridge.descriptorOf(chains[0])
			                                                   : insides[level - 1]->descriptorOf(chains[level]);
			REQUIRE((d != nullptr && d->container != nullptr && surface != nullptr));
			size_t n = 0;
			for (size_t r = 0; r < feathers::device::kSiteExportRows; ++r) {
				n += d->container->exportedSource(surface, r) != feathers::backplane::Source::nobody() ? 1 : 0;
			}
			out.push_back(n);
		}
		return out;
	}
	/// THE ROUTE'S SOURCE AS FEATHERS' LANE SPELLS IT: the innermost Macros re-exported out, one mint per level.
	[[nodiscard]] feathers::backplane::Source mintOutward() {
		feathers::backplane::Source s = feathers::backplane::Source::output(macros, 0);
		for (size_t level = chains.size(); level-- > 1;) {
			s = insides[level - 1]->containerExport(chains[level], s);
			if (!s.isExport()) {
				return feathers::backplane::Source::nobody();
			}
		}
		return rig.bridge.fxContainerExport(chains[0], s);
	}
	[[nodiscard]] float cutoff() const {
		const feathers::rack::RackEngine& lane = rig.synth.laneForTest().engineForTest();
		const size_t at = lane.slotOf(after);
		REQUIRE(at != feathers::rack::RackEngine::kNoSlot);
		return lane.resolvedFieldForTest(at, rig.cutoff);
	}
};

} // namespace

TEST_CASE("NZ any depth: a Macros 1, 2, 3 and 4 Chains deep moves a Filter after the outermost, by value — an unrouted nest publishes ZERO rows, a route mints exactly one row per level, and unrouting returns every level to zero") {
	for (const size_t depth : { size_t { 1 }, size_t { 2 }, size_t { 3 }, size_t { 4 } }) {
		CAPTURE(depth);
		DeepExport d(depth);
		// UNROUTED: nothing minted anywhere — what the nest cost before re-export existed.
		CHECK(d.rows() == std::vector<size_t>(depth, 0));
		const float base = d.cutoff();
		const feathers::backplane::Source out = d.mintOutward();
		REQUIRE(out.isContainerExport());
		CHECK(d.rows() == std::vector<size_t>(depth, 1)); // exactly the chain it needs
		const auto dest = feathers::backplane::Destination::control(d.after, d.rig.cutoff);
		REQUIRE(d.rig.bridge.mayRoute(out, dest));
		d.rig.bridge.setRouteAmount(out, dest, 0.25f);
		d.settle();
		const double want = d.rig.expected(base, 0.25f);
		CHECK(want != doctest::Approx(base));
		CHECK(d.cutoff() == doctest::Approx(want).epsilon(1e-5));
		CHECK(d.rows() == std::vector<size_t>(depth, 1)); // a live route keeps its whole chain, however many beats
		// UNROUTED: the outermost row goes after the engine catches up, then each level's as the one outside lets go.
		d.rig.bridge.setRouteAmount(out, dest, 0.0f);
		d.settle(4 + 2 * static_cast<int>(depth));
		CHECK(d.rows() == std::vector<size_t>(depth, 0));
		CHECK(d.cutoff() == doctest::Approx(base).epsilon(1e-5));
	}
}

TEST_CASE("NZ any depth: a route four Chains deep saves and loads into a FRESH plugin and moves the Filter by value") {
	feathers::rack::RackPatch saved;
	double want = 0.0;
	{
		DeepExport d(4);
		const float base = d.cutoff();
		const feathers::backplane::Source out = d.mintOutward();
		REQUIRE(out.isContainerExport());
		d.rig.bridge.setRouteAmount(out, feathers::backplane::Destination::control(d.after, d.rig.cutoff), 0.25f);
		d.settle();
		want = d.rig.expected(base, 0.25f);
		REQUIRE(d.cutoff() == doctest::Approx(want).epsilon(1e-5));
		d.rig.bridge.harvestExtraState();
		saved = d.rig.bridge.chainsPatch(false);
	}
	RungFive other;
	(void)other.bridge.restoreChains(saved);
	other.bridge.refresh();
	for (int i = 0; i < 8; ++i) {
		other.blocks(2);
		other.synth.serviceContainers(other.bridge.chainState(), &other.bridge.fxBodies());
		(void)other.bridge.sweepExports(other.params.routesGeneration());
	}
	const feathers::rack::RackEngine& lane = other.synth.laneForTest().engineForTest();
	const feathers::DeviceInstanceId filter = other.bridge.fxDeviceAt(other.bridge.fxDeviceCount() - 1);
	const size_t at = lane.slotOf(filter);
	REQUIRE(at != feathers::rack::RackEngine::kNoSlot);
	CHECK(lane.resolvedFieldForTest(at, other.cutoff) == doctest::Approx(want).epsilon(1e-5));
}

TEST_CASE("NZ any depth, every road: a Macros in the PRE of a Filter, in a Chain, in a Rack's chain, in a Chain in Feathers' lane, moves a Filter after the outermost by value — and saves and loads into a FRESH plugin") {
	// FOUR HOPS, EACH A DIFFERENT TABLE: the site's (SIT2), a Chain's (its patch's export list), a Rack's (body v3), a
	// Chain's again. Each re-exports the row inside it, minted only now that a route reaches through.
	feathers::rack::RackPatch saved;
	double want = 0.0;
	{
		RungFive rig;
		const auto service = [&](int beats) {
			for (int i = 0; i < beats; ++i) {
				rig.blocks(2);
				rig.synth.serviceContainers(rig.bridge.chainState(), &rig.bridge.fxBodies());
				(void)rig.bridge.sweepExports(rig.params.routesGeneration());
			}
		};
		const feathers::DeviceInstanceId c1 = rig.add(feathers::composite::kTraits.tag);
		const feathers::DeviceInstanceId after = rig.add(feathers::fx::filter::kTraits.tag);
		service(2);
		feathers::rack::RackBridge* in1 = feathers::rack::bridgeOf(rig.bridge.fxBodies().surfaceOf(c1));
		REQUIRE(in1 != nullptr);
		const feathers::DeviceInstanceId r2 = in1->addDevice(feathers::rack::parallel::kTraits.tag);
		REQUIRE(r2 != feathers::kNoInstance);
		service(2);
		auto* rack = feathers::rack::parallel::rackOf(in1->containerSurfaceOf(r2));
		REQUIRE(rack != nullptr);
		feathers::rack::RackBridge* inR = rack->chainAt(0)->bridge();
		REQUIRE(inR != nullptr);
		const feathers::DeviceInstanceId c3 = inR->addDevice(feathers::composite::kTraits.tag);
		REQUIRE(c3 != feathers::kNoInstance);
		service(2);
		feathers::rack::RackBridge* in3 = feathers::rack::bridgeOf(inR->containerSurfaceOf(c3));
		REQUIRE(in3 != nullptr);
		const feathers::DeviceInstanceId holder = in3->addDevice(feathers::fx::filter::kTraits.tag);
		REQUIRE(holder != feathers::kNoInstance);
		REQUIRE(in3->openSite(holder));
		service(2);
		auto* site = static_cast<feathers::rack::SiteChains*>(in3->containerSurfaceOf(feathers::siteKeyOf(holder)));
		REQUIRE(site != nullptr);
		const feathers::DeviceInstanceId m = site->pre().bridge()->addDevice(feathers::mod::macros::kTraits.tag);
		REQUIRE(m != feathers::kNoInstance);
		site->pre().bridge()->setField(m, 0, 0.5f);
		service(4);
		const feathers::rack::RackEngine& lane = rig.synth.laneForTest().engineForTest();
		const auto cutoff = [&] {
			const size_t at = lane.slotOf(after);
			REQUIRE(at != feathers::rack::RackEngine::kNoSlot);
			return lane.resolvedFieldForTest(at, rig.cutoff);
		};
		const float base = cutoff();
		feathers::backplane::Source s = in3->siteExport(holder, feathers::backplane::Source::output(m, 0));
		REQUIRE(s.isSiteExport());
		s = inR->containerExport(c3, s);
		REQUIRE(s.isContainerExport());
		s = in1->containerExport(r2, s);
		REQUIRE(s.isContainerExport());
		s = rig.bridge.fxContainerExport(c1, s);
		REQUIRE(s.isContainerExport());
		const auto dest = feathers::backplane::Destination::control(after, rig.cutoff);
		REQUIRE(rig.bridge.mayRoute(s, dest));
		rig.bridge.setRouteAmount(s, dest, 0.25f);
		service(6);
		want = rig.expected(base, 0.25f);
		CHECK(want != doctest::Approx(base));
		CHECK(cutoff() == doctest::Approx(want).epsilon(1e-5));
		rig.bridge.harvestExtraState();
		saved = rig.bridge.chainsPatch(false);
	}
	RungFive other;
	(void)other.bridge.restoreChains(saved);
	other.bridge.refresh();
	for (int i = 0; i < 10; ++i) {
		other.blocks(2);
		other.synth.serviceContainers(other.bridge.chainState(), &other.bridge.fxBodies());
		(void)other.bridge.sweepExports(other.params.routesGeneration());
	}
	const feathers::rack::RackEngine& lane = other.synth.laneForTest().engineForTest();
	const size_t at = lane.slotOf(other.bridge.fxDeviceAt(other.bridge.fxDeviceCount() - 1));
	REQUIRE(at != feathers::rack::RackEngine::kNoSlot);
	CHECK(lane.resolvedFieldForTest(at, other.cutoff) == doctest::Approx(want).epsilon(1e-5));
}

TEST_CASE("Depth gate: a site on the depth-20 Filter (found built live, reloading EMPTY) is refused and nothing changes; on the depth-19 Filter its PRE's Filter survives a reload into a fresh host") {
	// THE PROBE THAT FOUND THE WALL (2026-10-31): the site opened, its PRE took a Filter, and the reload gave back an
	// empty PRE — a chain at 21, which no file fills. Now the site is refused where its chain would stand past 20.
	{
		Deepest h;
		h.build();
		std::vector<uint8_t> before;
		const feathers::device::ByteSink sinkBefore { &before, [](void* ctx, const uint8_t* data, size_t n) {
			                                             auto* v = static_cast<std::vector<uint8_t>*>(ctx);
			                                             v->insert(v->end(), data, data + n);
		                                             } };
		REQUIRE(h.host.saveState(sinkBefore));
		CHECK_FALSE(h.bridge[20]->openSite(h.filter[20]));
		h.blocks(3);
		CHECK_FALSE(h.bridge[20]->hasSite(h.filter[20]));
		std::vector<uint8_t> after;
		const feathers::device::ByteSink sinkAfter { &after, [](void* ctx, const uint8_t* data, size_t n) {
			                                            auto* v = static_cast<std::vector<uint8_t>*>(ctx);
			                                            v->insert(v->end(), data, data + n);
		                                            } };
		REQUIRE(h.host.saveState(sinkAfter));
		CHECK(after == before);
	}
	std::vector<uint8_t> saved;
	{
		Deepest h;
		h.build();
		REQUIRE(h.bridge[19]->openSite(h.filter[19]));
		h.blocks(2);
		auto* site = static_cast<feathers::rack::SiteChains*>(h.bridge[19]->containerSurfaceOf(feathers::siteKeyOf(h.filter[19])));
		REQUIRE(site != nullptr);
		CHECK(site->pre().bridge()->nestingDepth() == 20);
		REQUIRE(site->pre().bridge()->addDevice(feathers::fx::filter::kTraits.tag) != feathers::kNoInstance);
		h.blocks(3);
		const feathers::device::ByteSink sink { &saved, [](void* ctx, const uint8_t* data, size_t n) {
			                                       auto* v = static_cast<std::vector<uint8_t>*>(ctx);
			                                       v->insert(v->end(), data, data + n);
		                                       } };
		REQUIRE(h.host.saveState(sink));
	}
	Deepest again;
	again.prepare();
	feathers::rack::RackPatch patch;
	REQUIRE(feathers::rack::parseRackText(std::string_view(reinterpret_cast<const char*>(saved.data()), saved.size()), &patch));
	(void)again.host.bridge()->loadPatch(patch);
	again.blocks(10);
	again.find();
	auto* site = static_cast<feathers::rack::SiteChains*>(again.bridge[19]->containerSurfaceOf(feathers::siteKeyOf(again.filter[19])));
	REQUIRE(site != nullptr);
	CHECK(site->pre().bridge()->chainLength() == 1);
	CHECK(feathers::device::sameTag(site->pre().bridge()->heldTagOf(site->pre().bridge()->deviceAtPosition(0)).data(),
	                                feathers::fx::filter::kTraits.tag));
}

// ---------------------------------------------------------------------------------------------
// PER-DEVICE OUTPUT COUNTS (2026-11-07): a slot's output buffers are its
// device's own, so a device swap at a slot REPLACES them. Every pointer held across blocks must be re-taken at the drain
// that installs, and the old buffers must outlive the block.
// ---------------------------------------------------------------------------------------------

TEST_CASE("per-device outputs: a device swapped at a slot is read through every holder's re-taken pointer — the host's core route, its per-voice run, and an engine input — never the old buffers") {
	// AN LFO IN THE STRIP (a per-voice source, so the host's core input holds a per-voice pointer too) routes into Pos X
	// (the HOST node, ParamSet) and into the lane filter's Cutoff (an ENGINE input). Then one publish removes it and adds a
	// MACROS device, which lands in the LFO's engine slot with buffers of its own count, routed the same two ways. In the
	// order a host runs — publish, provision, render — every holder must read MACROS' cell, BY VALUE, on the first block
	// and on blocks after the message thread has reaped (when the LFO's husk is freed: a holder still pointing into it
	// would read freed memory, which ASan reports).
	namespace lfo = feathers::mod::lfo;
	namespace mc = feathers::mod::macros;
	namespace flt = feathers::fx::filter;
	feathers::ParamSet params;
	feathers::Synth synth { params };
	feathers::ParamBridge bridge;
	synth.activate(48000.0, 4096);
	params.setBase(static_cast<clap_id>(feathers::ParamId::VoiceLevel), 1.0);
	synth.setCatalogue(feathers::hostCatalogue());
	std::vector<float> l(64, 0.0f);
	std::vector<float> r(64, 0.0f);
	const auto provision = [&] {
		feathers::publishEditorLanes(bridge, params, synth);
		(void)synth.provisionDevices(bridge.chainState(), bridge.publishedChains().sequence(), &bridge.fxBodies());
	};
	const auto render = [&](int blocks) {
		for (int b = 0; b < blocks; ++b) {
			synth.render(l.data(), r.data(), 64);
		}
	};
	const auto& engine = synth.laneForTest().engineForTest();
	const feathers::DeviceInstanceId filter = bridge.fxDeviceAt(0);
	REQUIRE(filter != feathers::kNoInstance);
	const int posX = feathers::modDestinationIndexOf(feathers::ParamId::PosX);
	REQUIRE(posX >= 0);
	const auto cutoff = static_cast<size_t>(flt::Param::Cutoff);
	constexpr float kAmount = 0.25f;
	const auto routeFrom = [&](feathers::DeviceInstanceId source) {
		bridge.setRouteAmount(feathers::backplane::Source::output(source, 0),
		                      feathers::backplane::Destination::core(static_cast<size_t>(posX)), kAmount);
		bridge.setRouteAmount(feathers::backplane::Source::output(source, 0),
		                      feathers::backplane::Destination::control(filter, cutoff), kAmount);
	};
	// THE LFO, routed both ways, and running.
	const size_t lfoAt = bridge.addRackDevice(lfo::kTraits.tag);
	REQUIRE(lfoAt < feathers::kMaxRackSlots);
	bridge.refresh();
	const feathers::DeviceInstanceId lfoId = bridge.rackDeviceAt(lfoAt);
	REQUIRE(lfoId != feathers::kNoInstance);
	routeFrom(lfoId);
	provision();
	render(4);
	const size_t lane = engine.slotOf(lfoId);
	REQUIRE(lane != feathers::rack::RackEngine::kNoSlot);
	bool lfoPerVoice = false;
	for (const auto& out : lfo::kDescriptor.outputs) {
		lfoPerVoice = lfoPerVoice || out.perVoice;
	}
	REQUIRE(lfoPerVoice); // the host's per-voice pointer is one of the holders under test
	// THE SWAP: the LFO out, MACROS in, one publish.
	REQUIRE(bridge.removeRackDevice(lfoAt));
	const size_t macrosAt = bridge.addRackDevice(mc::kTraits.tag);
	REQUIRE(macrosAt < feathers::kMaxRackSlots);
	bridge.refresh();
	const feathers::DeviceInstanceId macrosId = bridge.rackDeviceAt(macrosAt);
	REQUIRE(macrosId != feathers::kNoInstance);
	constexpr float kKnob = 0.7f;
	bridge.setDeviceControl(macrosId, mc::macroParam(0), kKnob);
	routeFrom(macrosId);
	provision();
	// THE CHECK, by value: the host's Pos X (collapsed and voice 0) and the filter's Cutoff are MACROS' output 0, read
	// through the engine's own cell for it — restated from the rules (`coreRouteTerm`, `device::withRouteOffset`).
	const feathers::ParamDesc* px = feathers::findParam(static_cast<clap_id>(feathers::ParamId::PosX));
	REQUIRE(px != nullptr);
	const feathers::device::ParamDesc& cp = flt::kDescriptor.params[cutoff];
	const auto check = [&] {
		const size_t at = engine.slotOf(macrosId);
		REQUIRE(at == lane); // MACROS took the LFO's slot, so its buffers REPLACED the LFO's there
		const float src = engine.outputAt(at, 0);
		CHECK(src == doctest::Approx(kKnob));
		const double base = params.base(static_cast<clap_id>(feathers::ParamId::PosX));
		const double want = feathers::landCore(*px, base + static_cast<double>(kAmount) * static_cast<double>(src) * (px->max - px->min));
		CHECK(params.resolved(feathers::ParamId::PosX) == want);
		CHECK(params.resolved(feathers::ParamId::PosX, 0) == want);
		const size_t fLane = engine.slotOf(filter);
		REQUIRE(fLane != feathers::rack::RackEngine::kNoSlot);
		REQUIRE(engine.slotAtLane(fLane).live_desc() != nullptr);
		REQUIRE(std::string_view(engine.slotAtLane(fLane).live_desc()->traits.tag) == std::string_view(flt::kTraits.tag));
		const double fBase = static_cast<double>(engine.fieldAt(fLane, cutoff));
		CHECK(engine.resolvedFieldForTest(fLane, cutoff)
		      == static_cast<float>(feathers::device::withRouteOffset(cp, fBase, static_cast<double>(kAmount) * static_cast<double>(src) * feathers::device::routeSpanOf(cp))));
		CHECK(want != base); // NOT VACUOUS: the route moved it
	};
	render(1);
	check();
	// AFTER THE MESSAGE THREAD REAPS (the husk's block has passed): a holder left pointing into the LFO's buffers would now
	// read freed memory.
	render(2);
	synth.drainRetiredDevices();
	render(2);
	synth.drainRetiredDevices();
	render(2);
	check();
}
