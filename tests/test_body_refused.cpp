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

#include "backplane/block_context.h"
#include "composite/composite_types.h"
#include "fx/featherverb/featherverb_types.h"
#include "plugin/device_registry.h"
#include "rack/rack_axes.h"
#include "rack/rack_bridge.h"
#include "rack/rack_device.h"
#include "rack/rack_engine.h"
#include "rack/rack_state.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// BACKLOG TM — A DEVICE BODY THE LOADER REFUSES IS CARRIED UNCHANGED, REPORTED, LOGGED AND NOT RUN, on the road the
// rack plugin saves and loads by (`RackBridge::loadPatch`, `patch`) — for a plain device, and for one inside a Rack.
// The converger's own claims are in test_converger.cpp.

using namespace feathers;
using rack::RackEngine;

namespace {

std::vector<uint8_t> badBody() { return std::vector<uint8_t>(41, 0xC7); }

rack::RackPatch::Device deviceOf(const char* tag, DeviceInstanceId id, std::vector<uint8_t> body) {
	rack::RackPatch::Device d;
	d.chain = 0;
	std::memcpy(d.tag.data(), tag, std::min(std::strlen(tag), device::kTagBytes));
	d.instanceId = id;
	d.body = std::move(body);
	return d;
}

rack::RackPatch patchOf(std::vector<rack::RackPatch::Device> devices) {
	rack::RackPatch p;
	p.present = true;
	p.device = std::move(devices);
	return p;
}

/// THE BRIDGE A PLUGIN HOLDS, standing on an engine, after the load has settled the way a drain settles it.
struct Loaded {
	RackEngine engine { rack::kRackAxes, hostCatalogue() };
	rack::RackBridge bridge { engine };
	rack::LoadReport report;
	explicit Loaded(const rack::RackPatch& patch) {
		engine.prepare(48000.0, 4096);
		bridge.prepare(48000.0, 4096);
		report = bridge.loadPatch(patch);
		bridge.drain();
		bridge.reapRetired();
		bridge.drain();
		bridge.publish(engine);
	}
	float through(float in) {
		float last = 0.0f;
		for (int i = 0; i < 8; ++i) {
			std::vector<float> l(256, in), r(256, in);
			engine.processStereo(l.data(), r.data(), 256, backplane::unknownBlock());
			bridge.publish(engine);
			last = l.back();
		}
		return last;
	}
};

const char* const kVerb = fx::featherverb::kTraits.tag;

/// THE BODY A SAVE WRITES FOR THE DEVICE AT POSITION 0, read back through the document text — a real save and re-read.
std::vector<uint8_t> savedBodyAt0(rack::RackBridge& bridge) {
	std::string text;
	REQUIRE(rack::rackToText(bridge.patch(), &text));
	rack::RackPatch parsed;
	REQUIRE(rack::parseRackText(text, &parsed));
	REQUIRE(parsed.device.size() >= 1);
	return parsed.device[0].body;
}

} // namespace

TEST_CASE("backlog TM: a plain device whose body is refused is saved back byte for byte, reported, logged and bypassed") {
	const std::vector<uint8_t> bad = badBody();
	Loaded l(patchOf({ deviceOf(kVerb, 7, bad) }));

	// SAVED UNCHANGED — the reproduced loss: this was the fresh device's own empty body.
	CHECK(savedBodyAt0(l.bridge) == bad);
	// AND AGAIN, from the reloaded save: still the same bytes.
	{
		std::string text;
		REQUIRE(rack::rackToText(l.bridge.patch(), &text));
		rack::RackPatch parsed;
		REQUIRE(rack::parseRackText(text, &parsed));
		Loaded again(parsed);
		CHECK(savedBodyAt0(again.bridge) == bad);
	}
	// REPORTED, naming kind, place and the reason.
	REQUIRE(l.report.countOf(rack::LoadReport::Kind::BodyRefused) == 1);
	const rack::LoadReport::First& f = l.report.firstOf(rack::LoadReport::Kind::BodyRefused);
	CHECK(f.where == rack::deviceWhere(deviceForTag(kVerb)->traits.name, 0, 0));
	CHECK(f.what == std::string(deviceForTag(kVerb)->traits.name));
	CHECK(f.why.find("41 bytes") != std::string::npos);
	// LOGGED, once.
	const std::vector<std::string> log = l.bridge.takeRefusalLog();
	REQUIRE(log.size() == 1);
	CHECK(log[0].find("body-refused") != std::string::npos);
	CHECK(log[0].find(f.where) != std::string::npos);
	CHECK(l.bridge.takeRefusalLog().empty());
	// SHOWN: the host's cards ask this.
	REQUIRE(l.bridge.bodyRefusalOf(7) != nullptr);
	// BYPASSED: nothing runs there, and the output is the input to the bit.
	CHECK(l.engine.slotOf(7) == RackEngine::kNoSlot);
	CHECK(l.through(0.2f) == 0.2f);
}

TEST_CASE("backlog TM: a device whose body was taken is not reported and runs") {
	Loaded l(patchOf({ deviceOf(kVerb, 7, {}) }));
	CHECK(l.report.countOf(rack::LoadReport::Kind::BodyRefused) == 0);
	CHECK(l.bridge.bodyRefusalOf(7) == nullptr);
	CHECK(l.bridge.takeRefusalLog().empty());
	CHECK(l.engine.slotOf(7) != RackEngine::kNoSlot);
}

TEST_CASE("backlog TM: a Rack holding a device whose body is refused keeps that body through the outer save") {
	const std::vector<uint8_t> bad = badBody();
	// THE INNER RACK'S OWN FILE, as a Rack device carries it: one Featherverb with a body it will refuse.
	std::string innerText;
	REQUIRE(rack::rackToText(patchOf({ deviceOf(kVerb, 3, bad) }), &innerText, false));
	const std::vector<uint8_t> innerBody(innerText.begin(), innerText.end());

	Loaded outer(patchOf({ deviceOf(composite::kTraits.tag, 9, innerBody) }));
	// THE OUTER TOOK ITS BODY; the refusal is the inner level's, and the inner level says it.
	CHECK(outer.bridge.bodyRefusalOf(9) == nullptr);
	rack::RackBridge* inner = rack::bridgeOf(outer.bridge.containerSurfaceOf(9));
	REQUIRE(inner != nullptr);
	REQUIRE(inner->bodyRefusalOf(3) != nullptr);
	CHECK(inner->takeLoadReport().countOf(rack::LoadReport::Kind::BodyRefused) == 1);
	CHECK(inner->takeRefusalLog().size() == 1);

	// THE OUTER SAVE carries the Rack's body, and in it the inner device's body, unchanged.
	const std::vector<uint8_t> savedOuter = savedBodyAt0(outer.bridge);
	rack::RackPatch innerPatch;
	REQUIRE(rack::parseRackText(std::string(savedOuter.begin(), savedOuter.end()), &innerPatch));
	REQUIRE(innerPatch.device.size() == 1);
	CHECK(innerPatch.device[0].body == bad);
}

namespace {

/// A RACK FILE AS TEXT, then a stream over it — what `loadRack` reads.
struct TextStream {
	std::string text;
	size_t at = 0;
	static int64_t CLAP_ABI readFn(const clap_istream* s, void* buffer, uint64_t size) {
		auto* self = static_cast<TextStream*>(s->ctx);
		const size_t n = std::min<size_t>(size, self->text.size() - self->at);
		std::memcpy(buffer, self->text.data() + self->at, n);
		self->at += n;
		return static_cast<int64_t>(n);
	}
	clap_istream in() { return clap_istream { this, readFn }; }
};

} // namespace

TEST_CASE("backlog TM: the engine-direct load refuses a body holding a refused device body whole, and builds no fresh device") {
	// THE ROAD (`loadRack`): `InnerChain::loadState` and `rack_device.cpp`'s `loadState` when no record stands over the
	// engine. An engine holds no opaque bytes, so a fresh empty device here would be what the next save wrote.
	for (const bool refusedBody : { true, false }) {
		CAPTURE(refusedBody);
		TextStream file;
		REQUIRE(rack::rackToText(patchOf({ deviceOf(kVerb, 3, refusedBody ? badBody() : std::vector<uint8_t> {}) }), &file.text, false));
		RackEngine engine { rack::kRackAxes, hostCatalogue() };
		engine.prepare(48000.0, 4096);
		rack::LoadReport report;
		clap_istream in = file.in();
		const bool took = rack::loadRack(&in, &engine, &report);
		CHECK(took == !refusedBody);
		CHECK(report.countOf(rack::LoadReport::Kind::BodyRefused) == (refusedBody ? 1u : 0u));
		CHECK(engine.chainLength(0) == (refusedBody ? 0u : 1u)); // nothing fresh stands where the refused one was
	}
}
