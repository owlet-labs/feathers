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

#include "fx/owltt/owltt_types.h"
#include "fx/filter/filter_types.h"
#include "fx/eq/eq_types.h"
#include "fx/conv/ir_ref.h"
#include "fx/featherverb/featherverb.h"
#include "mod/dahdsr/dahdsr_types.h"
#include "rack/rack_engine.h"
#include "rack_endpoints.h"
#include "rack/rack_bridge.h"
#include "plugin/device_registry.h"
#include "rack/rack_state.h"
#include "device/macro_type.h"
#include "patch_values.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <chrono>
#include <vector>

// THE RACK'S PATCH — saved, loaded, and refused where it must be.
//
// THE CASE THAT MATTERS MOST IS THE HOLE. A device this build cannot run must leave its slot EMPTY
// rather than letting everything after it shift down, because the routes are keyed on slot: a dense
// re-fill would silently re-point every route above the missing device, which is the ABA failure
// arriving through a file instead of through a pool. It is silent, it survives another save, and the
// first symptom is a sound nobody asked for.

using namespace feathers;

namespace {

constexpr backplane::Axes kAxes {
	.coreSources = 8,
	.coreDestinations = 0,
	.deviceSlots = 8,
	.outputStride = 4,
};

/// A stream over a byte vector, readable and writable.
struct MemStream {
	std::vector<char> bytes;
	size_t at = 0;
	clap_ostream out {};
	clap_istream in {};

	MemStream() {
		out.ctx = this;
		out.write = [](const clap_ostream* s, const void* buf, uint64_t n) -> int64_t {
			auto* m = static_cast<MemStream*>(s->ctx);
			const auto* p = static_cast<const char*>(buf);
			m->bytes.insert(m->bytes.end(), p, p + n);
			return static_cast<int64_t>(n);
		};
		in.ctx = this;
		in.read = [](const clap_istream* s, void* buf, uint64_t n) -> int64_t {
			auto* m = static_cast<MemStream*>(s->ctx);
			const size_t left = m->bytes.size() - m->at;
			const size_t take = n < left ? static_cast<size_t>(n) : left;
			std::memcpy(buf, m->bytes.data() + m->at, take);
			m->at += take;
			return static_cast<int64_t>(take);
		};
	}
	void rewind() { at = 0; }
};

size_t controlNamed(const device::Descriptor& d, const char* name) {
	for (size_t i = 0; i < d.params.size(); ++i) {
		if (std::string(d.params[i].name).find(name) != std::string::npos) {
			return i;
		}
	}
	return d.params.size();
}

} // namespace

TEST_CASE("a rack round-trips: macros, devices, their settings, and the routes") {
	rack::RackEngine a(kAxes, hostCatalogue());
	const size_t fx = a.addDevice(fx::filter::kTraits.tag);
	const size_t env = a.addDevice(mod::dahdsr::kTraits.tag);
	const size_t cutoff = controlNamed(fx::filter::kDescriptor, "Cutoff");
	const size_t ordinal = fx::filter::kDescriptor.nthModulatableOfControl(cutoff);
	const backplane::Destination dest = test::ordinalOf(a, fx, ordinal);
	const backplane::Source source = test::outputOf(a, env, 0);

	// A CUTOFF FROM ITS OWN DECLARATION — log2 Hz, 4.32..14.29. This wrote 0.6, which is off the bottom of
	// the range and round-tripped only while the rack stored a base past it (backlog DV: clamped, always).
	const auto& cutoffDesc = fx::filter::kDescriptor.params[cutoff];
	const float cutoffValue = static_cast<float>(cutoffDesc.min + 0.6 * (cutoffDesc.max - cutoffDesc.min));
	a.setMacro(0, 0.25f);
	a.setMacro(5, 0.75f);
	a.setMacroBipolar(5, true); // AND ITS POLARITY (backlog EG), beside the value it reads
	a.setField(fx, cutoff, cutoffValue);
	a.setRouteAmount(test::macro(0), dest, 0.5f);
	a.setRouteAmount(source, dest, -0.25f);

	MemStream s;
	REQUIRE(rack::saveRack(&s.out, a));

	rack::RackEngine b(kAxes, hostCatalogue());
	rack::LoadReport refused;
	s.rewind();
	REQUIRE(rack::loadRack(&s.in, &b, &refused));
	REQUIRE(refused.routes.count == 0);

	REQUIRE(b.macro(0) == doctest::Approx(0.25f));
	REQUIRE(b.macro(5) == doctest::Approx(0.75f));
	// THE VALUE CAME BACK AS THE HAND HOLDS IT — 0..1 — AND THE MODE BESIDE IT, so the cell every route
	// reads is derived again on the far side rather than saved pre-mapped and mapped twice.
	CHECK(b.macroBipolar(5));
	CHECK_FALSE(b.macroBipolar(0));
	REQUIRE(b.slotAtLane(fx).live());
	REQUIRE(b.slotAtLane(env).live());
	REQUIRE(b.fieldAt(fx, cutoff) == doctest::Approx(cutoffValue));
	REQUIRE(b.routeAmount(test::macro(0), dest) == doctest::Approx(0.5f));
	REQUIRE(b.routeAmount(source, dest) == doctest::Approx(-0.25f));
}

TEST_CASE("every declared control survives, not just the modulatable ones") {
	rack::RackEngine a(kAxes, hostCatalogue());
	const size_t fx = a.addDevice(fx::filter::kTraits.tag);
	for (size_t c = 0; c < fx::filter::kDescriptor.params.size(); ++c) {
		const device::ParamDesc& p = fx::filter::kDescriptor.params[c];
		a.setField(fx, c, static_cast<float>(p.min + (p.max - p.min) * 0.3));
	}
	MemStream s;
	REQUIRE(rack::saveRack(&s.out, a));

	rack::RackEngine b(kAxes, hostCatalogue());
	rack::LoadReport refused;
	s.rewind();
	REQUIRE(rack::loadRack(&s.in, &b, &refused));
	for (size_t c = 0; c < fx::filter::kDescriptor.params.size(); ++c) {
		CAPTURE(c);
		const device::ParamDesc& p = fx::filter::kDescriptor.params[c];
		// A LABELLED CHOICE IS SAVED AS ITS LABEL (backlog JF §3.4), so it comes back ON its step — which is
		// what every reader of a stepped control already took from the value a hand left between steps.
		const float expected = p.unit == device::Unit::Choice && !p.names.empty()
		                           ? static_cast<float>(p.min + std::round(a.fieldAt(fx, c) - p.min))
		                           : a.fieldAt(fx, c);
		REQUIRE(b.fieldAt(fx, c) == doctest::Approx(expected));
	}
}

TEST_CASE("a device's row state past its parameters is held, saved and restored — the row rule") {
	// `params.size() + extraState` (device.h). The engine held only the parameters until the lane moved
	// onto it: automod's folded travel and the shaper's phase space never reached the device, and a rack
	// saved without them. Every device that declares extra state, found in the registry.
	int swept = 0;
	for (const device::Descriptor& d : kDevices) {
		if (d.traits.extraState == 0) {
			continue;
		}
		CAPTURE(std::string(d.traits.name));
		++swept;
		rack::RackEngine a(kAxes, hostCatalogue());
		const size_t lane = a.addDevice(d.traits.tag);
		REQUIRE(lane != rack::RackEngine::kNoSlot);
		REQUIRE(a.fieldsPresentAt(lane) == d.params.size() + d.traits.extraState);
		for (size_t k = 0; k < d.traits.extraState; ++k) {
			a.setField(lane, d.params.size() + k, 0.5f + static_cast<float>(k));
		}
		MemStream s;
		REQUIRE(rack::saveRack(&s.out, a));
		rack::RackEngine b(kAxes, hostCatalogue());
		rack::LoadReport refused;
		s.rewind();
		REQUIRE(rack::loadRack(&s.in, &b, &refused));
		for (size_t k = 0; k < d.traits.extraState; ++k) {
			CAPTURE(k);
			// STORED AS GIVEN — row state has no declaration to clamp it against.
			CHECK(b.fieldAt(lane, d.params.size() + k) == 0.5f + static_cast<float>(k));
		}
	}
	CHECK(swept >= 2);
}

TEST_CASE("the EQ's analyser settings survive a save and reload, per instance, with two EQs set differently") {
	// The row rule above proves ONE device of each kind. What it cannot see is two of the same kind in one
	// rack: settings kept by KIND rather than by device would pass it and swap these (the harm report of
	// 2026-09-08 was exactly that). Every analyser field differs between the two, and none is its default.
	namespace eq = fx::eq;
	rack::RackEngine a(kAxes, hostCatalogue());
	const size_t first = a.addDevice(eq::kTraits.tag);
	const size_t second = a.addDevice(eq::kTraits.tag);
	REQUIRE(first != rack::RackEngine::kNoSlot);
	REQUIRE(second != rack::RackEngine::kNoSlot);
	REQUIRE(first != second);
	struct Setting {
		eq::Extra field;
		float first, second;
	};
	const Setting settings[] = {
	    {eq::Extra::Pre, 1.0f, 0.0f},           // PRE off / on
	    {eq::Extra::Post, 0.0f, 1.0f},          // POST on / off
	    {eq::Extra::AnalyserTilt, 1.0f, 2.0f},  // 3 dB/oct / flat
	    {eq::Extra::AnalyserSpeed, 2.0f, 1.0f}, // slow / fast
	    {eq::Extra::AnalyserPeak, 1.0f, 2.0f},  // 2 s / infinite
	    {eq::Extra::DbRange, 2.0f, 1.0f},       // ±6 / ±12 dB
	    {eq::Extra::Selected, 3.0f, 6.0f},      // band 4 / band 7
	};
	for (const Setting& s : settings) {
		a.setField(first, eq::extraIndex(s.field), s.first);
		a.setField(second, eq::extraIndex(s.field), s.second);
	}
	MemStream stream;
	REQUIRE(rack::saveRack(&stream.out, a));
	rack::RackEngine b(kAxes, hostCatalogue());
	rack::LoadReport refused;
	stream.rewind();
	REQUIRE(rack::loadRack(&stream.in, &b, &refused));
	for (const Setting& s : settings) {
		CAPTURE(static_cast<int>(s.field));
		CHECK(b.fieldAt(first, eq::extraIndex(s.field)) == s.first);
		CHECK(b.fieldAt(second, eq::extraIndex(s.field)) == s.second);
	}
}

TEST_CASE("a device this build cannot run is skipped, and the route on the device after it still finds it — by id") {
	// THE ABA GUARD, ARRIVING THROUGH A FILE. Record 0 is something unknown; record 1 a filter with a route
	// at it. **THIS WAS "LEAVES A HOLE"** while a file named lanes: a loader that filled densely put the
	// filter in slot 0 and left a lane-addressed route pointing at slot 1. The file names no lane now
	// (by design: no lanes), and the route names the filter by ID — so the filter may land
	// wherever the engine puts it and the route goes with it. The claim that survives: skipping a device
	// cannot take a route to its neighbour.
	rack::RackEngine a(kAxes, hostCatalogue());
	MemStream s;
	{
		// HAND-WRITTEN, because no build can produce a device it does not have — and since backlog JF a
		// patch is a document a hand CAN write: device 0 something nobody has (id 7), device 1 a real
		// filter (id 8) that the route names by ID and its control by KEY, so the route finds the filter
		// wherever it lands. The filter's values are left out entirely: each takes its default.
		const std::string cutoff = fx::filter::kDescriptor.params[fx::filter::kDescriptor.controlOfNthModulatableBesideEnable(0)].key;
		const std::string doc = std::string(R"({
	"format": "feathers.rack",
	"major": 1,
	"chains": [[
		{"tag": "ZZZZ", "id": 7, "values": {"level": 1, "tone": 2}, "body": null},
		{"tag": ")") + fx::filter::kTraits.tag + R"(", "id": 8, "values": {}, "body": null}
	]],
	"routes": [{"src": {"macro": 0}, "dst": {"device": 8, "control": ")" + cutoff + R"("}, "amount": 0.5}],
	"macros": [0, 0, 0, 0, 0, 0, 0, 0],
	"bipolar": [0, 0, 0, 0, 0, 0, 0, 0],
	"canvas": {"extent": 0, "collapsed": false}
})";
		s.bytes.assign(doc.begin(), doc.end());
	}

	rack::LoadReport refused;
	s.rewind();
	REQUIRE(rack::loadRack(&s.in, &a, &refused));

	// ONE DEVICE RUNS — the filter — and the route is on IT, wherever the engine put it.
	REQUIRE(a.chainLength() == 1);
	const size_t filterLane = a.deviceAtPosition(0);
	REQUIRE(filterLane != rack::RackEngine::kNoSlot);
	REQUIRE(a.slotAtLane(filterLane).live_desc() != nullptr);
	CHECK(std::string(a.slotAtLane(filterLane).live_desc()->traits.tag) == fx::filter::kTraits.tag);
	CHECK(a.routeAmount(test::macro(0), test::ordinalOf(a, filterLane, 1)) == doctest::Approx(0.5f)); // ordinal 0 is Enabled (KW)
	// AND ON NOTHING ELSE — every other slot's first control is unrouted.
	for (size_t lane = 0; lane < kAxes.deviceSlots; ++lane) {
		if (lane != filterLane) {
			CAPTURE(lane);
			CHECK(a.routeAmount(test::macro(0), test::ordinalOf(a, lane, 0)) == 0.0f);
		}
	}
	REQUIRE(refused.routes.count == 0);
}

namespace {

/// A hand-written document whose first device names the TOP of the id space and whose second names a plain id
/// the route names; the route is the one that must survive the first being renamed.
std::string documentNamingTheTopId() {
	const std::string cutoff = fx::filter::kDescriptor.params[fx::filter::kDescriptor.controlOfNthModulatableBesideEnable(0)].key;
	return std::string(R"({
	"format": "feathers.rack",
	"major": 1,
	"chains": [[
		{"tag": ")") + fx::filter::kTraits.tag + R"(", "id": 4294967295, "values": {}, "body": null},
		{"tag": ")" + fx::filter::kTraits.tag + R"(", "id": 8, "values": {}, "body": null}
	]],
	"routes": [{"src": {"macro": 0}, "dst": {"device": 8, "control": ")" + cutoff + R"("}, "amount": 0.5}],
	"macros": [0, 0, 0, 0, 0, 0, 0, 0],
	"bipolar": [0, 0, 0, 0, 0, 0, 0, 0],
	"canvas": {"extent": 0, "collapsed": false}
})";
}

} // namespace

TEST_CASE("hardening: a file naming the top id is renamed like a missing id, on the engine road, and its neighbour's route holds") {
	rack::RackEngine a(kAxes, hostCatalogue());
	MemStream s;
	const std::string doc = documentNamingTheTopId();
	s.bytes.assign(doc.begin(), doc.end());
	rack::LoadReport report;
	REQUIRE(loadRack(&s.in, &a, &report));
	REQUIRE(a.chainLength() == 2);
	const size_t first = a.deviceAtPosition(0);
	const size_t second = a.deviceAtPosition(1);
	const DeviceInstanceId renamed = a.slotAtLane(first).id;
	CHECK(renamed != kNoInstance);
	CHECK(renamed != 0xFFFFFFFFu);
	CHECK(renamed != 8);
	CHECK(a.slotAtLane(second).id == 8);
	CHECK(a.routeAmount(test::macro(0), test::ordinalOf(a, second, 1)) == doctest::Approx(0.5f));
}

TEST_CASE("hardening: a file naming the top id is renamed like a missing id, on the record road, and its neighbour's route holds") {
	rack::RackPatch parsed;
	REQUIRE(rack::parseRackText(documentNamingTheTopId(), &parsed));
	rack::RackEngine engine(kAxes, hostCatalogue());
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge { engine };
	bridge.prepare(48000.0, 4096);
	(void)bridge.loadPatch(parsed);
	bridge.drain();
	const rack::RackPatch back = bridge.patch();
	REQUIRE(back.device.size() == 2);
	CHECK(back.device[0].instanceId != kNoInstance);
	CHECK(back.device[0].instanceId != 0xFFFFFFFFu);
	CHECK(back.device[0].instanceId != 8);
	CHECK(back.device[1].instanceId == 8);
	REQUIRE(back.route.size() == 1);
	CHECK(back.route[0].destination.a == 8);
	// AND A DEVICE ADDED AFTER IS NEITHER THE NO-ID NOR THE TOP ONE.
	const DeviceInstanceId added = bridge.addDevice(fx::filter::kTraits.tag);
	CHECK(added != kNoInstance);
	CHECK(added != 0xFFFFFFFFu);
}

TEST_CASE("hardening: a file naming an id no counter could reach is renamed, and the counter is not spent by it") {
	// A FILE NAMING MORE THAN SIXTEEN MILLION (2^24, the loader's bound) is read as naming none.
	for (const unsigned named : { 0x1000001u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFEu }) {
		CAPTURE(named);
		std::string doc = documentNamingTheTopId();
		const std::string was = "4294967295";
		doc.replace(doc.find(was), was.size(), std::to_string(named));
		rack::RackPatch parsed;
		REQUIRE(rack::parseRackText(doc, &parsed));
		rack::RackEngine engine(kAxes, hostCatalogue());
		engine.prepare(48000.0, 4096);
		rack::RackBridge bridge { engine };
		bridge.prepare(48000.0, 4096);
		(void)bridge.loadPatch(parsed);
		bridge.drain();
		const rack::RackPatch back = bridge.patch();
		REQUIRE(back.device.size() == 2);
		CHECK(back.device[0].instanceId != named);
		CHECK(back.device[0].instanceId < 0x1000001u);
		CHECK(back.device[1].instanceId == 8);
		// AND LATER ADDS STILL GET SMALL IDS.
		const DeviceInstanceId added = bridge.addDevice(fx::filter::kTraits.tag);
		CHECK(added != kNoInstance);
		CHECK(added < 0x1000001u);
	}
	// THE BOUND ITSELF IS A PLAUSIBLE ID, and is kept.
	std::string doc = documentNamingTheTopId();
	const std::string was = "4294967295";
	doc.replace(doc.find(was), was.size(), "16777216");
	rack::RackPatch parsed;
	REQUIRE(rack::parseRackText(doc, &parsed));
	rack::RackEngine engine(kAxes, hostCatalogue());
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge { engine };
	bridge.prepare(48000.0, 4096);
	(void)bridge.loadPatch(parsed);
	bridge.drain();
	const rack::RackPatch back = bridge.patch();
	REQUIRE(back.device.size() == 2);
	CHECK(back.device[0].instanceId == 16777216u);
}

TEST_CASE("a route naming a slot nothing landed in is dropped and COUNTED, not silently kept") {
	rack::RackEngine a(kAxes, hostCatalogue());
	const size_t fx = a.addDevice(fx::filter::kTraits.tag);
	REQUIRE(fx != rack::RackEngine::kNoSlot);
	a.setRouteAmount(test::macro(0), test::ordinalOf(a, fx, 1), 0.5f); // ordinal 0 is Enabled (backlog KW)
	// THE TWO WORDS THE FILE WILL CARRY FOR THAT DESTINATION — read off the SAVING rack and the descriptor,
	// not restated: the device's id, and the CONTROL index the ordinal names (which is what `describe`
	// writes, an ordinal being renumbered by a modulatable flag and a control index never).
	const DeviceInstanceId filterId = a.slotAtLane(fx).id;
	REQUIRE(filterId != kNoInstance);
	const size_t cutoffControl = fx::filter::kDescriptor.controlOfNthModulatableBesideEnable(0);
	MemStream s;
	REQUIRE(rack::saveRack(&s.out, a));

	// A rack with NO slots at all cannot hold the device the file names.
	constexpr backplane::Axes kNoSlots { .coreSources = 8, .coreDestinations = 0, .deviceSlots = 0 };
	rack::RackEngine narrow(kNoSlots, hostCatalogue());
	rack::LoadReport refused;
	s.rewind();
	REQUIRE(rack::loadRack(&s.in, &narrow, &refused));
	REQUIRE(refused.routes.count == 1);
	// **WHICH DEVICE, AND THAT IT IS NOT HERE — TWO CLAIMS WHERE THERE WAS ONE** (kill-lanes step 3b). This
	// asserted `firstDestination == kNoSlots.destinations()`: one past the axis, the coordinate sentinel for
	// "named something we could not place". That said the refusal happened and threw away WHAT was refused,
	// because a coordinate cannot carry an identity a host does not hold. A refusal now reports the endpoint
	// the FILE asked for, so both halves are sayable and both are said:
	//   1. it names the device and control the patch wrote — never a neighbour's, which is the claim the old
	//      sentinel was reaching for and could only make by elimination;
	//   2. this rack does not hold that device — the question the sentinel actually asked.
	CHECK(refused.routes.firstDestination == backplane::Destination::control(filterId, cutoffControl));
	CHECK(narrow.slotOf(filterId) == rack::RackEngine::kNoSlot);
	// AND THE SOURCE SURVIVED, which is what makes the destination's failure attributable: macro 0 is inside
	// this rack's own table, so it is placed, and only the device end went missing.
	CHECK(refused.routes.firstSource == backplane::Source::core(0));
}

TEST_CASE("an empty stream is a fresh rack, not an error — and NO patch, not an empty one") {
	// **THE DISTINCTION USED TO BE INVISIBLE AND IS NOW A VALUE.** `loadRack` expressed it by
	// returning before it emptied the rack, so nothing downstream could tell "there was no patch"
	// from "there was a patch and it was empty" — and the two must do opposite things to a rack that
	// already holds devices. `RackPatch::present` says which, and this asserts it.
	MemStream s;
	rack::RackPatch patch;
	REQUIRE(rack::parseRack(&s.in, &patch));
	CHECK_FALSE(patch.present);
	CHECK(patch.device.empty());
	CHECK(patch.route.empty());
	CHECK(patch.macro.empty());

	// AND THE RACK IS LEFT ALONE, which is what `present == false` is FOR — asserted through a rack
	// that has something to lose, because an empty rack cannot tell the two behaviours apart.
	rack::RackEngine a(kAxes, hostCatalogue());
	REQUIRE(a.addDevice(fx::filter::kTraits.tag) == 0);
	rack::LoadReport refused;
	s.rewind();
	REQUIRE(rack::loadRack(&s.in, &a, &refused));
	REQUIRE(refused.routes.count == 0);
	CHECK(a.slotAtLane(0).live());
	CHECK(a.chainLength() == 1);
}

TEST_CASE("a device this build cannot run is PARSED and then SKIPPED — the parse does not consult the registry") {
	// **THE SPLIT ASSERTED AS A DIFFERENCE, or it is only an internal rearrangement.** `parseRack`
	// walks the record and keeps it; `applyRack` is what asks whether this build can run the tag. If
	// the skip crept back into the parse, the patch would stop meaning what the FILE says and start
	// meaning what this build can do — and the record count would no longer match the stream.
	rack::RackEngine source(kAxes, hostCatalogue());
	REQUIRE(source.addDevice(fx::filter::kTraits.tag) == 0);
	REQUIRE(source.addDevice(mod::dahdsr::kTraits.tag) == 1);
	MemStream s;
	REQUIRE(rack::saveRack(&s.out, source));

	// THE FIRST DEVICE'S TAG BECOMES ONE NOTHING CAN RUN. The record keeps its length, so the walk is
	// unaffected — which is the property the length prefix exists for.
	const std::string body(s.bytes.data(), s.bytes.size());
	const size_t at = body.find(fx::filter::kTraits.tag);
	REQUIRE(at != std::string::npos);
	std::memcpy(s.bytes.data() + at, "ZZZZ", 4);

	rack::RackPatch patch;
	REQUIRE(rack::parseRack(&s.in, &patch));
	CHECK(patch.present);
	// **BOTH RECORDS SURVIVED THE PARSE**, the unknown one included, and it still says ZZZZ.
	REQUIRE(patch.device.size() == 2);
	CHECK(std::string(patch.device[0].tag.data()) == "ZZZZ");
	CHECK(std::string(patch.device[1].tag.data()) == mod::dahdsr::kTraits.tag);

	// AND THE APPLY IS WHERE IT GOES: the engine runs the dahdsr and skips the unknown one.
	rack::RackEngine a(kAxes, hostCatalogue());
	rack::LoadReport refused;
	rack::applyRack(patch, &a, &refused);
	REQUIRE(a.chainLength() == 1);
	CHECK(a.slotAtLane(a.deviceAtPosition(0)).isMod());
}

TEST_CASE("a major this build does not know refuses the whole document, and says so") {
	// Rather than reading a document whose SHAPE changed as though it had not — the silent corruption a
	// version exists to convert into a clean refusal. `major` is the envelope's own version (backlog JF): it
	// moves for the document's shape, never for a device edit, which keys absorb.
	// THE BODY MUST BE VALID, or this case cannot tell the version check from any other.
	//
	// A first draft of the binary form of this case wrote version 99 followed by 64 bytes of 'x', and a
	// known-bad deleting the version check left it GREEN — because the loader then read 'xxxx' as the
	// macro tag and refused there instead. Two guards agreeing, and a conclusion drawn about one of them.
	rack::RackEngine source(kAxes, hostCatalogue());
	source.addDevice(fx::filter::kTraits.tag);
	source.setMacro(0, 0.5f);
	MemStream s;
	REQUIRE(rack::saveRack(&s.out, source));
	std::string text(s.bytes.begin(), s.bytes.end());
	const size_t major = text.find("\"major\": 1,");
	REQUIRE(major != std::string::npos);
	text.replace(major, 11, "\"major\": 99,"); // the ONLY thing wrong with this file
	s.bytes.assign(text.begin(), text.end());

	rack::RackEngine a(kAxes, hostCatalogue());
	rack::LoadReport refused;
	REQUIRE_FALSE(rack::loadRack(&s.in, &a, &refused));
	REQUIRE_FALSE(a.slotAtLane(0).live()); // and nothing of it was applied
	CHECK(refused.countOf(rack::LoadReport::Kind::NotLoaded) == 1);
	CHECK(refused.firstOf(rack::LoadReport::Kind::NotLoaded).why.find("major 99") != std::string::npos);
}

TEST_CASE("a stream in the old binary format is refused whole, not read short — a clean break, and said") {
	// **WHAT A READER DOES WITH A PATCH FROM BEFORE THE KEYED FORMAT, pinned rather than assumed** (backlog
	// JF: a clean break, no reader for the old format). The binary rack stream it replaced
	// opened with its version byte (10) and the "MACR" section; there is one honest answer to it and this
	// is it: **refuse the whole document, apply nothing, and say why.**
	//
	// THE ALTERNATIVE IS WHAT THIS CASE EXISTS TO FORBID: a device row read by column is the silent
	// misread the whole format change exists to end.
	MemStream s;
	{
		const char header[] = { 10, 'M', 'A', 'C', 'R', 8, 0, 0, 0 };
		s.bytes.assign(header, header + sizeof(header));
		const float macro = 0.5f;
		for (int i = 0; i < 8; ++i) {
			const char* p = reinterpret_cast<const char*>(&macro);
			s.bytes.insert(s.bytes.end(), p, p + sizeof(macro));
		}
	}

	rack::RackEngine a(kAxes, hostCatalogue());
	a.addDevice(fx::owltt::kTraits.tag); // something to lose, so "untouched" is a claim and not a tautology
	rack::LoadReport refused;
	s.rewind();
	REQUIRE_FALSE(rack::loadRack(&s.in, &a, &refused));
	// NOTHING APPLIED, AND NOTHING TAKEN: the rack the load was aimed at still holds what it held.
	REQUIRE(a.slotAtLane(0).live());
	CHECK(a.macro(0) != doctest::Approx(0.5f));
	CHECK(refused.countOf(rack::LoadReport::Kind::NotLoaded) == 1);
	CHECK(refused.firstOf(rack::LoadReport::Kind::NotLoaded).why.find("older format") != std::string::npos);
}

TEST_CASE("a truncated stream refuses AND LEAVES THE RACK IT WAS LOADED INTO UNTOUCHED") {
	// **THIS IS A BEHAVIOUR CHANGE AND IT IS DELIBERATE (CD commit 1).** The old loader emptied the
	// rack as its first act and only then began reading, so a truncated file — a corrupt project, a
	// short read — cost the user the patch they had in exchange for nothing. Parsing to a value first
	// makes "nothing is written until the whole stream has parsed" true by construction rather than
	// by remembering to stage. `ParamSet::load` (params.cpp:1125) has staged into a local for exactly
	// this reason all along and says so; the rack now agrees with it.
	rack::RackEngine a(kAxes, hostCatalogue());
	a.addDevice(fx::filter::kTraits.tag);
	MemStream s;
	REQUIRE(rack::saveRack(&s.out, a));
	s.bytes.resize(s.bytes.size() / 2);

	rack::RackEngine b(kAxes, hostCatalogue());
	// SOMETHING TO LOSE, or the case cannot tell "left alone" from "emptied": a rack that was already
	// empty passes either way, which is the vacuity this case would otherwise ship with.
	REQUIRE(b.addDevice(fx::owltt::kTraits.tag) == 0);
	const auto threshold = controlNamed(fx::owltt::kDescriptor, "Threshold");
	REQUIRE(threshold < fx::owltt::kDescriptor.params.size());
	const float def = static_cast<float>(fx::owltt::kDescriptor.params[threshold].def);
	b.setField(0, threshold, def == 0.0f ? 1.0f : 0.0f);
	const float held = b.fieldAt(0, threshold);
	REQUIRE(held != doctest::Approx(def));

	rack::LoadReport refused;
	s.rewind();
	REQUIRE_FALSE(rack::loadRack(&s.in, &b, &refused));

	// **THE VALUE, not "the rack is non-empty".** The device is still there, still the one it was, and
	// still at the setting the hand left it at — a difference check would pass for a rack holding
	// anything at all.
	CHECK(b.chainLength() == 1);
	REQUIRE(b.slotAtLane(0).live());
	CHECK(std::string(b.slotAtLane(0).live_desc()->traits.tag) == fx::owltt::kTraits.tag);
	CHECK(b.fieldAt(0, threshold) == doctest::Approx(held));
}

TEST_CASE("a nonsense payload length is refused BEFORE it is turned into an allocation") {
	// THE CEILING THAT ACTUALLY MATTERS, and a known-bad said which one. Removing the MACRO count's
	// ceiling changes nothing observable — the loop reads floats from an exhausted stream and fails on
	// the first, so the file is refused either way and the guard only bounds wasted work.
	//
	// The payload length is different in kind: it is turned into a `std::vector<float>` before
	// anything is read, so a file claiming 4 GB gets 4 GB. That is a length from a file on somebody's
	// disk deciding what this process allocates.
	//
	// **AND THIS CASE STILL DOES NOT PIN THAT CEILING — measured, and said here rather than assumed.**
	// Deleting `payloadBytes > kMaxCount * sizeof(float)` leaves the suite green: under Linux
	// overcommit the billion-float vector is handed over lazily, the first `readF` then fails on an
	// exhausted stream, and the file is refused either way. The guard bounds MEMORY, not correctness,
	// so no functional assertion can distinguish its presence. Pinning it would take a resident-set
	// measurement, which is machine-dependent and worth less than the honest note.
	//
	// The guard stays: a malicious patch must not be able to ask this process for four gigabytes,
	// whether or not a test can watch it not happen. Playbook 8b — inert is not the same as uncovered,
	// and here the mutation is inert for a reason about the allocator rather than about the code.
	//
	// **AND UNTIL 2026-09-11 THIS STREAM NEVER REACHED THE LENGTH AT ALL.** It was written by hand with
	// version byte 1 and the old device layout; the format moved to 5 and the loader refused it at the
	// FIRST BYTE — green for a reason that had nothing to do with its title. So it is built the way the
	// version case is: a real save, with the one number under test made wrong.
	//
	// **SINCE BACKLOG JF THE LENGTH FROM A FILE IS A BODY'S `$bin` SIZE**, and it is refused on the number,
	// before the base64 is decoded or a byte reserved (`json::binOf`) — a real save, the one number made
	// wrong.
	rack::RackEngine source(kAxes, hostCatalogue());
	REQUIRE(source.addDevice(fx::filter::kTraits.tag) == 0);
	MemStream s;
	REQUIRE(rack::saveRack(&s.out, source));
	std::string text(s.bytes.begin(), s.bytes.end());
	{
		rack::RackEngine control(kAxes, hostCatalogue());
		rack::LoadReport refused;
		REQUIRE(rack::loadRack(&s.in, &control, &refused)); // the stream is good until it is made bad
		s.rewind();
	}
	const size_t body = text.find("\"body\": null");
	REQUIRE(body != std::string::npos);
	text.replace(body, 12, "\"body\": {\"$bin\": \"AAAA\", \"size\": 4294967280}"); // a body nobody has
	s.bytes.assign(text.begin(), text.end());
	rack::RackEngine a(kAxes, hostCatalogue());
	rack::LoadReport refused;
	REQUIRE_FALSE(rack::loadRack(&s.in, &a, &refused));
	CHECK(refused.countOf(rack::LoadReport::Kind::NotLoaded) == 1);
}

TEST_CASE("a nonsense count is refused BEFORE anything is reserved for it") {
	// A LIST A FILE CLAIMS IS HUGE IS HUGE ONLY IF IT IS WRITTEN OUT — a document names no counts, so the
	// ceiling is the reader's: every value counts against `json::Limits::maxNodes` as it is read, and a
	// document past it is refused rather than grown into a tree. Four hundred thousand routes, each tiny.
	rack::RackEngine source(kAxes, hostCatalogue());
	REQUIRE(source.addDevice(mod::dahdsr::kTraits.tag) == 0);
	MemStream s;
	REQUIRE(rack::saveRack(&s.out, source));
	std::string text(s.bytes.begin(), s.bytes.end());
	const size_t routes = text.find("\"routes\": []");
	REQUIRE(routes != std::string::npos);
	std::string many = "\"routes\": [";
	for (int i = 0; i < 400000; ++i) {
		many += i == 0 ? "{}" : ",{}";
	}
	text.replace(routes, 12, many + "]"); // well-formed: only its size is wrong
	rack::RackPatch parsed;
	rack::LoadReport report;
	bool ok = true;
	CHECK_NOTHROW(ok = rack::parseRackText(text, &parsed, &report));
	CHECK_FALSE(ok);
	CHECK(report.countOf(rack::LoadReport::Kind::NotLoaded) == 1);
}

TEST_CASE("loading REPLACES the rack rather than adding to it") {
	rack::RackEngine a(kAxes, hostCatalogue());
	a.addDevice(fx::filter::kTraits.tag);
	MemStream empty;
	REQUIRE(rack::saveRack(&empty.out, rack::RackEngine(kAxes, hostCatalogue())));

	rack::LoadReport refused;
	empty.rewind();
	REQUIRE(rack::loadRack(&empty.in, &a, &refused));
	REQUIRE_FALSE(a.slotAtLane(0).live()); // the filter is gone, not still there beside nothing
}

TEST_CASE("a reordered rack round-trips, and its route follows the device by id") {
	// **THIS WAS "BECAUSE THE LANE TRAVELS WITH THE DEVICE"**, and the device block gained a lane field for
	// it: a route was saved against a LANE, and a loader handing lanes out by slot order would point every
	// route at the wrong device. Routes name devices by ID now and the file names no lane (by
	// design, 2026-09-11), so the loader puts devices where IT likes and the claim is the one a
	// hand cares about: the order comes back, and the route is on the same device.
	rack::RackEngine source(kAxes, hostCatalogue());
	REQUIRE(source.addDevice(fx::filter::kTraits.tag) == 0);
	REQUIRE(source.addDevice(fx::owltt::kTraits.tag) == 1);

	// A ROUTE ONTO OWLTT. It took the lane through `routeLaneOf`, which was the identity plus a liveness
	// test and is deleted (kill-lanes step 5); the liveness question is asked plainly instead, which is
	// what that call was worth.
	const size_t lane = 1;
	REQUIRE(source.slots().descriptorAtLane(lane) != nullptr);
	source.setRouteAmount(test::macro(0), test::ordinalOf(source, lane, 0), 0.6f);

	// THEN MOVE IT TO THE FRONT, which is what separates POSITION from lane.
	REQUIRE(source.moveDevice(1, 0));
	// **ASKED BY POSITION.** `routeLaneOf` answered "the lane of the device at this position" while
	// slots were position-indexed; under step 2 it is the identity, so the question belongs to
	// `deviceAtPosition`. The device did not move — its lane is where it lives — and what changed is
	// which position names it.
	REQUIRE(source.deviceAtPosition(0) == lane);
	// NOT VACUOUS: the move must actually have made position and lane disagree, or this case proves
	// nothing about the order surviving a save and would pass on a build that ignored order entirely.
	REQUIRE(lane != 0);

	MemStream s;
	REQUIRE(rack::saveRack(&s.out, source));

	rack::RackEngine loaded(kAxes, hostCatalogue());
	rack::LoadReport refused;
	s.rewind();
	REQUIRE(rack::loadRack(&s.in, &loaded, &refused));

	// THE ORDER CAME BACK.
	REQUIRE(loaded.slots().descriptorAtLane(0) != nullptr);
	// **BY POSITION.** `slots()` is a view over LANES now, so `descriptorAtLane(0)` answers "what lives at
	// lane 0" — which is the filter, because it never moved. The order came back is a claim about the
	// CHAIN, and this is the fourth spelling in this refactor whose meaning moved without its
	// signature changing: `slotAt`, `routeLaneOf`, `tagAt`, and this.
	const auto tagAtPosition = [&](size_t position) {
		const size_t lane = loaded.deviceAtPosition(position);
		REQUIRE(lane != rack::RackEngine::kNoSlot);
		const device::Descriptor* d = loaded.slots().descriptorAtLane(lane);
		REQUIRE(d != nullptr);
		return std::string(d->traits.tag);
	};
	CHECK(tagAtPosition(0) == fx::owltt::kTraits.tag);
	CHECK(tagAtPosition(1) == fx::filter::kTraits.tag);

	// **AND THE ROUTE IS ON OWLTT — the device at position 0 — wherever the loader put it**, and not on the
	// filter.
	const size_t owlttLane = loaded.deviceAtPosition(0);
	const size_t filterLane = loaded.deviceAtPosition(1);
	CHECK(loaded.routeAmount(test::macro(0), test::ordinalOf(loaded, owlttLane, 0)) == doctest::Approx(0.6f));
	CHECK(loaded.routeAmount(test::macro(0), test::ordinalOf(loaded, filterLane, 0)) == 0.0f);
}

TEST_CASE("a patch carrying a route the routing rule forbids loads WITHOUT it, and is counted") {
	// **THE LOADER'S GATE IS `mayRoute`, so the routing rule reaches the file.** A route that can
	// never deliver is thrown away.
	//
	// **HOW A BACKWARDS ROUTE GETS INTO THE PATCH AT ALL, and why that changed between commits.** At
	// commit 1 this case wrote it through `setRouteAmount`, which then wrote the engine's own table
	// with no gate. Commit 3 closed that road on purpose — the engine now writes through
	// `Graph::route`, which refuses exactly this — so there is NO product road that can put an illegal
	// route into a patch, which is the point. The case therefore uses the seam that exists for it,
	// `routesForTest()`, to plant one past both gates; the forward route beside it goes in through the
	// ordinary road and survives, which is the control that the loader is not declining everything.
	rack::RackEngine source(kAxes, hostCatalogue());
	REQUIRE(source.addDevice(mod::dahdsr::kTraits.tag) == 0);
	REQUIRE(source.addDevice(mod::dahdsr::kTraits.tag) == 1);
	const size_t firstLane = 0;
	const size_t secondLane = 1;
	REQUIRE(firstLane != rack::RackEngine::kNoLane);
	REQUIRE(secondLane != rack::RackEngine::kNoLane);
	const backplane::Destination forward = test::ordinalOf(source, secondLane, 0);
	const backplane::Destination backwards = test::ordinalOf(source, firstLane, 0);
	const backplane::Source fromFirst = test::outputOf(source, firstLane, 0);
	const backplane::Source fromSecond = test::outputOf(source, secondLane, 0);
	// THE TWO WORDS THE FILE WILL CARRY FOR THE ILLEGAL ROUTE — read off the SAVING rack and the descriptor,
	// as the refusal will report them: each device by the id it wears here, and the destination by the
	// CONTROL index its modulatable ordinal 0 names.
	const DeviceInstanceId firstId = source.slotAtLane(firstLane).id;
	const DeviceInstanceId secondId = source.slotAtLane(secondLane).id;
	REQUIRE(firstId != kNoInstance);
	REQUIRE(secondId != kNoInstance);
	REQUIRE(firstId != secondId); // or "which device" below would be a claim with one answer
	const size_t envControl = mod::dahdsr::kDescriptor.controlOfNthModulatableBesideEnable(0);
	source.setRouteAmount(fromFirst, forward, 0.5f);    // legal
	// ILLEGAL, PLANTED PAST BOTH GATES. It was `routesForTest().set(...)` while a route was a cell;
	// a device route is an edge now and this is the same door on the new store.
	REQUIRE(source.plantRouteForTest(fromSecond, backwards, 0.25f));
	REQUIRE(source.routeAmount(fromSecond, backwards) == doctest::Approx(0.25f));

	MemStream s;
	REQUIRE(rack::saveRack(&s.out, source));
	rack::RackEngine loaded(kAxes, hostCatalogue());
	rack::LoadReport refused;
	s.rewind();
	REQUIRE(rack::loadRack(&s.in, &loaded, &refused));

	CHECK(loaded.routeAmount(fromFirst, forward) == doctest::Approx(0.5f));     // survived
	CHECK(loaded.routeAmount(fromSecond, backwards) == doctest::Approx(0.0f));  // thrown away
	CHECK(refused.routes.count == 1);
	// **AND IT WAS *THE* ROUTE — WHICH TWO DEVICES AND WHICH CONTROL** (kill-lanes step 3b). `count == 1`
	// alone passes whenever ANY one route is refused, including the legal one being dropped for some other
	// reason while the illegal one quietly lands; the two amount checks above narrow that but still read a
	// rendered table rather than the report. The refusal names endpoints now, so the report itself can be
	// asserted: the SECOND device's output reaching back into the FIRST device's control, which is exactly
	// the backwards edge that was planted.
	CHECK(refused.routes.firstSource == backplane::Source::output(secondId, 0));
	CHECK(refused.routes.firstDestination == backplane::Destination::control(firstId, envControl));
	// **AND BOTH ENDS ARE HERE**, which is what makes this refusal a different animal from the one above
	// ("a route naming a slot nothing landed in"): there the device was missing, here both devices loaded
	// and kept their ids and the ROUTING RULE is what refused. Two refusals that reported the same sentinel
	// under the coordinate shape are now distinguishable, and this is the assertion that says so.
	CHECK(loaded.slotOf(firstId) != rack::RackEngine::kNoSlot);
	CHECK(loaded.slotOf(secondId) != rack::RackEngine::kNoSlot);
}

TEST_CASE("[measure] what the graph's door costs per write and per load, on a FULL 16-device rack") {
	// **THE COST GATE FOR ROUTING THROUGH `Graph::route`.** Every device-to-device write now runs
	// `reaches()` (a DFS over pre-allocated buffers) and `recompute()` (O(lanes^2), no allocation) on
	// the drain — which is the audio thread inside `process()`. A drag sends one write per knob tick;
	// a load sends one per saved route in a burst. Both are measured here rather than estimated, on
	// the population that would actually be hit: the rack full, every lane holding a modulator.
	//
	// NO BOUND IS ASSERTED, on this host and on purpose — load here swings by a factor of eight — the
	// case asserts correctness and REPORTS the numbers, as `test_rack_clap`'s load measurement does.
	rack::RackEngine full(kAxes, hostCatalogue());
	full.prepare(48000.0, 4096);
	std::vector<size_t> lanes;
	for (size_t i = 0; i < kAxes.deviceSlots; ++i) {
		const size_t slot = full.addDevice(mod::dahdsr::kTraits.tag);
		if (slot == rack::RackEngine::kNoSlot) {
			break;
		}
		lanes.push_back(slot);
	}
	REQUIRE(lanes.size() == kAxes.deviceSlots);
	// FIRST MODULATOR INTO EVERY LATER ONE: 15 forward routes, all legal.
	const backplane::Source src = test::outputOf(full, lanes[0], 0);
	for (size_t i = 1; i < lanes.size(); ++i) {
		full.setRouteAmount(src, test::ordinalOf(full, lanes[i], 0), 0.5f);
	}
	const backplane::Destination hot = test::ordinalOf(full, lanes[1], 0);
	REQUIRE(full.routeAmount(src, hot) == doctest::Approx(0.5f));

	// (a) PER WRITE — the drag shape: the same existing cell, many amounts. Warm first.
	for (int i = 0; i < 64; ++i) {
		full.setRouteAmount(src, hot, 0.25f + 0.001f * static_cast<float>(i % 7));
	}
	constexpr int kWrites = 5000;
	const auto w0 = std::chrono::steady_clock::now();
	for (int i = 0; i < kWrites; ++i) {
		full.setRouteAmount(src, hot, 0.25f + 0.001f * static_cast<float>(i % 7));
	}
	const auto wNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - w0).count();
	const double perWriteNs = static_cast<double>(wNs) / kWrites;

	// (b) PER LOAD — a full 16-device, 15-route patch, saved once and loaded many times.
	MemStream s;
	REQUIRE(rack::saveRack(&s.out, full));
	constexpr int kLoads = 200;
	double loadTotalUs = 0.0;
	for (int i = 0; i < kLoads; ++i) {
		rack::RackEngine loaded(kAxes, hostCatalogue());
		loaded.prepare(48000.0, 4096);
		rack::LoadReport refused;
		s.rewind();
		const auto l0 = std::chrono::steady_clock::now();
		REQUIRE(rack::loadRack(&s.in, &loaded, &refused));
		loadTotalUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - l0).count();
		if (i == 0) {
			REQUIRE(refused.routes.count == 0);
			REQUIRE(loaded.routeAmount(src, hot) == doctest::Approx(full.routeAmount(src, hot)));
		}
	}
	MESSAGE("rack routing rule, commit 3: setRouteAmount through Graph::route on a full 16-device rack = "
	        << perWriteNs << " ns per write (reaches + recompute, on the drain); loadRack of a 16-device, "
	           "15-route patch = " << (loadTotalUs / kLoads) << " us per load, against ~5333 us for a "
	           "256-frame block at 48 kHz");
	CHECK(full.routeAmount(src, hot) > 0.0f);
}

TEST_CASE("[measure] what the DRAIN costs for a 16-device patch load — the audio thread's share of a load (backlog CF)") {
	// THE DRAIN'S OWN TIME, with a 16-device patch pending: the previous chain removed and every
	// device adopted, inside one `drain()`. Three samples of 200 loads each, so the number is a
	// distribution and not a reading. Only APIs the base tree also has, so the same text measures the
	// before as well as the after.
	rack::RackEngine full(kAxes, hostCatalogue());
	full.prepare(48000.0, 4096);
	for (size_t i = 0; i < kAxes.deviceSlots; ++i) {
		REQUIRE(full.addDevice(mod::dahdsr::kTraits.tag) != rack::RackEngine::kNoSlot);
	}
	MemStream s;
	REQUIRE(rack::saveRack(&s.out, full));
	rack::RackPatch patch;
	s.rewind();
	REQUIRE(rack::parseRack(&s.in, &patch));
	REQUIRE(patch.device.size() == kAxes.deviceSlots);

	rack::RackEngine engine(kAxes, hostCatalogue());
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);
	bridge.primeForTest();
	std::vector<float> l(64, 0.0f), r(64, 0.0f);
	for (int sample = 0; sample < 3; ++sample) {
		constexpr int kLoads = 200;
		double drainUs = 0.0;
		for (int i = 0; i < kLoads; ++i) {
			REQUIRE(bridge.loadPatch(patch).routes.count == 0); // the record replaced; its devices built here
			const auto t0 = std::chrono::steady_clock::now();
			bridge.drain();
			drainUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
			engine.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
		}
		REQUIRE(engine.chainLength() == kAxes.deviceSlots);
		MESSAGE("CF drain cost, sample " << sample << ": 16-device patch load = " << (drainUs / kLoads)
		                                 << " us per drain (removes + adopts), against ~1333 us for a 64-frame block at 48 kHz");
	}
}

TEST_CASE("the canvas's partition and fold ride in the patch, after the routes (v8)") {
	// The partition size and expanded state persist with the patch. One view for the
	// editor's one band, beside the chains rather than in a device's row.
	rack::RackPatch patch;
	patch.present = true;
	patch.canvas = { 333.0f, true };
	MemStream stream;
	REQUIRE(rack::writeRack(&stream.out, patch));
	stream.rewind();
	rack::RackPatch back;
	REQUIRE(rack::parseRack(&stream.in, &back));
	CHECK(back.canvas.extent == doctest::Approx(333.0f));
	CHECK(back.canvas.collapsed);
	CHECK(back == patch);
	// AND THE DEFAULT ROUND-TRIPS AS THE DEFAULT: zero extent is "never set", open.
	rack::RackPatch bare;
	bare.present = true;
	MemStream second;
	REQUIRE(rack::writeRack(&second.out, bare));
	second.rewind();
	REQUIRE(rack::parseRack(&second.in, &back));
	CHECK(back.canvas == rack::CanvasView {});
}

TEST_CASE("a card's fold travels in the patch, by DEVICE — a reorder between save and load moves nothing") {
	// **A FOLD LIVES IN THE PATCH, BY DESIGN (2026-09-12).** It was a desk preference keyed by the
	// card's POSITION in the strip, and a position is not an identity: reorder the cards and the remembered
	// fold landed on whichever card had moved into that place. It could not be keyed by device id on the
	// desk either, because ids belong to a patch and two patches both hold a device 1 — which is exactly
	// what makes the patch the right home. `rack_state.h` argues the line: state about a DEVICE travels
	// with it; state about a window (a collapsed rail) stays on the desk.
	//
	// THE CLAIM IS THE ONE A HAND CARES ABOUT: fold the second card, reorder the rack, save, load, and the
	// SAME DEVICE is folded — not the same position.
	rack::RackEngine engine(kAxes, hostCatalogue());
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);
	const DeviceInstanceId first = bridge.addDevice(fx::filter::kTraits.tag);
	const DeviceInstanceId second = bridge.addDevice(fx::owltt::kTraits.tag);
	REQUIRE(first != kNoInstance);
	REQUIRE(second != kNoInstance);
	bridge.setDeviceFolded(second, true);
	REQUIRE(bridge.deviceFolded(second));
	REQUIRE_FALSE(bridge.deviceFolded(first)); // not vacuous: one folded, one not

	// THE REORDER, which is what told the old key apart from a real identity: the folded device is now at
	// position 0, so a fold remembered by POSITION would come back on the other card.
	REQUIRE(bridge.moveDevice(1, 0));
	REQUIRE(bridge.deviceAtPosition(0) == second);

	const rack::RackPatch saved = bridge.patch();
	MemStream s;
	REQUIRE(rack::writeRack(&s.out, saved));
	rack::RackPatch parsed;
	REQUIRE(rack::parseRack(&s.in, &parsed));
	CHECK(parsed == saved); // the view word survives the wire

	rack::RackEngine other(kAxes, hostCatalogue());
	other.prepare(48000.0, 4096);
	rack::RackBridge loaded(other);
	REQUIRE(loaded.loadPatch(parsed).routes.count == 0);
	// BY DEVICE: the OWLTT is folded and the filter is not, whatever position either ended up in.
	const DeviceInstanceId frontId = loaded.deviceAtPosition(0);
	const DeviceInstanceId backId = loaded.deviceAtPosition(1);
	REQUIRE(frontId != kNoInstance);
	REQUIRE(backId != kNoInstance);
	CHECK(device::sameTag(loaded.descriptorOf(frontId)->traits.tag, fx::owltt::kTraits.tag));
	CHECK(loaded.deviceFolded(frontId));
	CHECK_FALSE(loaded.deviceFolded(backId));

	SUBCASE("and one patch's folds never reach another's cards") {
		// IDS ARE PATCH-SCOPED, so the incoming patch's device 1 is not the outgoing one's. A map that
		// survived a load would hand it the departed device's fold; the load empties it.
		rack::RackEngine third(kAxes, hostCatalogue());
		third.prepare(48000.0, 4096);
		rack::RackBridge plain(third);
		const DeviceInstanceId own = plain.addDevice(fx::filter::kTraits.tag);
		plain.setDeviceFolded(own, true);
		REQUIRE(plain.deviceFolded(own));
		// A PATCH WITH NOTHING FOLDED, loaded over it.
		rack::RackPatch unfolded = saved;
		for (rack::RackPatch::Device& d : unfolded.device) {
			d.view = 0;
		}
		REQUIRE(plain.loadPatch(unfolded).routes.count == 0);
		for (size_t at = 0; at < plain.chainLength(); ++at) {
			CAPTURE(at);
			CHECK_FALSE(plain.deviceFolded(plain.deviceAtPosition(at)));
		}
	}
}

//------------------------------------------------------------------------------------------------//
// THE MACROS' TYPES AND NAMES — patch state beside the polarity, the rack's copy of what Feathers'    //
// FSMB block carries (macro-mod; in the keyed document since backlog JF: `macroTypes`, `macroNames`). //
//------------------------------------------------------------------------------------------------//

TEST_CASE("rack macro type and name round-trip") {
	// EVERY MACRO, EACH A DIFFERENT CASE: empty (the default), plain, UTF-8 whose bytes outnumber its
	// characters, exactly the cap, one byte, and the types cycled so no two neighbours agree. Asserted
	// field by field against the literals the hand wrote, not against "what the writer wrote", so a
	// writer and reader that agreed on dropping a field could not pass together.
	const std::string utf8 = "Fl\xC3\xBCgel \xE2\x86\x92 Sweep"; // "Flügel → Sweep", 17 bytes
	const std::string full(device::kMacroNameMax, 'x');            // exactly the cap
	const std::vector<std::string> names = { "", "Filter", utf8, full, "", "a", "Kick", "" };
	const std::vector<device::MacroType> types = {
		device::MacroType::Knob,  device::MacroType::Latch,     device::MacroType::Momentary,
		device::MacroType::Latch, device::MacroType::Momentary, device::MacroType::Knob,
		device::MacroType::Latch, device::MacroType::Momentary,
	};
	REQUIRE(names.size() == kAxes.coreSources);
	REQUIRE(types.size() == kAxes.coreSources);
	REQUIRE(utf8.size() == 17);

	SUBCASE("a patch value, through the wire") {
		rack::RackPatch p;
		p.present = true;
		p.macro.assign(kAxes.coreSources, 0.0f);
		p.macroBipolar.assign(kAxes.coreSources, 0);
		for (size_t i = 0; i < kAxes.coreSources; ++i) {
			p.macroType.push_back(static_cast<uint8_t>(types[i]));
			p.macroName.push_back(names[i]);
		}
		MemStream s;
		REQUIRE(rack::writeRack(&s.out, p));
		rack::RackPatch back;
		REQUIRE(rack::parseRack(&s.in, &back));
		REQUIRE(back.macroType.size() == kAxes.coreSources);
		REQUIRE(back.macroName.size() == kAxes.coreSources);
		for (size_t i = 0; i < kAxes.coreSources; ++i) {
			CAPTURE(i);
			CHECK(back.macroType[i] == static_cast<uint8_t>(types[i]));
			CHECK(back.macroName[i] == names[i]);
		}
		CHECK(back == p);
	}

	SUBCASE("a bridge, saved and loaded into another") {
		rack::RackEngine engine(kAxes, hostCatalogue());
		engine.prepare(48000.0, 4096);
		rack::RackBridge bridge(engine);
		for (size_t i = 0; i < kAxes.coreSources; ++i) {
			bridge.setMacroType(i, types[i]);
			bridge.setMacroName(i, names[i]);
		}
		MemStream s;
		REQUIRE(rack::writeRack(&s.out, bridge.patch()));
		rack::RackPatch parsed;
		REQUIRE(rack::parseRack(&s.in, &parsed));

		rack::RackEngine other(kAxes, hostCatalogue());
		other.prepare(48000.0, 4096);
		rack::RackBridge loaded(other);
		(void)loaded.loadPatch(parsed);
		for (size_t i = 0; i < kAxes.coreSources; ++i) {
			CAPTURE(i);
			CHECK(loaded.macroType(i) == types[i]);
			CHECK(loaded.macroName(i) == (names[i].empty() ? "Macro " + std::to_string(i + 1) : names[i]));
			CHECK(loaded.macroRenamed(i) == !names[i].empty());
		}
	}

	SUBCASE("an engine with no bridge — a rack inside a rack before its catalogue is adopted") {
		rack::RackEngine a(kAxes, hostCatalogue());
		for (size_t i = 0; i < kAxes.coreSources; ++i) {
			a.setMacroType(i, types[i]);
			a.setMacroName(i, names[i]);
		}
		MemStream s;
		REQUIRE(rack::saveRack(&s.out, a));
		rack::RackEngine b(kAxes, hostCatalogue());
		rack::LoadReport refused;
		s.rewind();
		REQUIRE(rack::loadRack(&s.in, &b, &refused));
		for (size_t i = 0; i < kAxes.coreSources; ++i) {
			CAPTURE(i);
			CHECK(b.macroType(i) == types[i]);
			CHECK(b.macroName(i) == (names[i].empty() ? "Macro " + std::to_string(i + 1) : names[i]));
		}
		// AND A BRIDGE BUILT ON THAT ENGINE LATER TAKES THEM, as it takes the devices it already runs —
		// the order `rack_device.cpp` and `inner_chain.cpp` follow when a body loads before adoption.
		b.prepare(48000.0, 4096);
		rack::RackBridge late(b);
		for (size_t i = 0; i < kAxes.coreSources; ++i) {
			CAPTURE(i);
			CHECK(late.macroType(i) == types[i]);
			CHECK(late.macroRenamed(i) == !names[i].empty());
		}
	}
}

TEST_CASE("a rack macro name over the cap is cut at the cap on read, never kept longer") {
	// **IN THE BINARY FORMAT THIS WAS A REFUSAL**, because a length past the cap desynchronised every section
	// after it. A document has no lengths to desynchronise — each name is its own string — so the cap is
	// what it bounds: a name a hand wrote too long is cut where the writer would have cut it
	// (`clampMacroName`, which never splits a code point), and nothing else in the patch moves.
	rack::RackEngine engine(kAxes, hostCatalogue());
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);
	bridge.setMacroName(0, "abc");
	MemStream s;
	REQUIRE(rack::writeRack(&s.out, bridge.patch()));
	std::string text(s.bytes.begin(), s.bytes.end());
	const size_t at = text.find("\"abc\"");
	REQUIRE(at != std::string::npos);
	const std::string tooLong(device::kMacroNameMax + 5, 'y');
	text.replace(at, 5, "\"" + tooLong + "\"");
	rack::RackPatch back;
	REQUIRE(rack::parseRackText(text, &back));
	REQUIRE(back.macroName.size() == kAxes.coreSources);
	CHECK(back.macroName[0] == std::string(device::kMacroNameMax, 'y'));
	CHECK(back.macroName[1].empty());
}

TEST_CASE("a rack macro type out of range is clamped on read") {
	// A TYPE IS CLAMPED, NOT REFUSED — FSMB's rule: unreadable (here `null`, what a NaN is written as) reads
	// as Knob, what every macro was; past the last as the last. A knob that behaves wrongly is recoverable;
	// a lost patch is not.
	rack::RackEngine engine(kAxes, hostCatalogue());
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);
	MemStream s;
	REQUIRE(rack::writeRack(&s.out, bridge.patch()));
	std::string text(s.bytes.begin(), s.bytes.end());
	const std::string types = "\"macroTypes\": [\n\t\t0,\n\t\t0,";
	const size_t at = text.find(types);
	REQUIRE(at != std::string::npos);
	text.replace(at, types.size(), "\"macroTypes\": [\n\t\tnull,\n\t\t7,");
	rack::RackPatch back;
	REQUIRE(rack::parseRackText(text, &back));
	CHECK(back.macroType[0] == static_cast<uint8_t>(device::MacroType::Knob));
	CHECK(back.macroType[1] == static_cast<uint8_t>(device::MacroType::Momentary));
}

TEST_CASE("rack bridge: macro type and name verbs, and the rename listener") {
	// ParamBridge's rules on the rack's eight (A2): out of range ignored, unchanged is not an edit, a name
	// clamped and held EMPTY when it is the default spelled out, and the listener told once per macro
	// whose effective name moved — by a rename and by a load.
	rack::RackEngine engine(kAxes, hostCatalogue());
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);
	for (size_t i = 0; i < kAxes.coreSources; ++i) {
		CAPTURE(i);
		CHECK(bridge.macroType(i) == device::MacroType::Knob);
		CHECK(bridge.macroName(i) == "Macro " + std::to_string(i + 1));
		CHECK_FALSE(bridge.macroRenamed(i));
	}
	std::vector<size_t> heard;
	device::Subscription token = bridge.subscribeMacroRename([&heard](size_t index) { heard.push_back(index); });

	bridge.setMacroType(1, device::MacroType::Latch);
	CHECK(bridge.macroType(1) == device::MacroType::Latch);
	bridge.setMacroType(1, device::MacroType::kCount);
	CHECK(bridge.macroType(1) == device::MacroType::Latch); // no such type: refused
	bridge.setMacroType(kAxes.coreSources, device::MacroType::Momentary); // no such macro: nothing
	CHECK(bridge.macroType(kAxes.coreSources) == device::MacroType::Knob);
	CHECK(heard.empty()); // a type is not a name

	bridge.setMacroName(2, "Wobble");
	CHECK(bridge.macroName(2) == "Wobble");
	CHECK(bridge.macroRenamed(2));
	REQUIRE(heard.size() == 1);
	CHECK(heard[0] == 2);
	bridge.setMacroName(2, "Wobble");
	CHECK(heard.size() == 1); // unchanged: not heard
	bridge.setMacroName(3, "Macro 4");
	CHECK_FALSE(bridge.macroRenamed(3));
	CHECK(heard.size() == 1); // the default spelled out: still the default, not heard
	bridge.setMacroName(kAxes.coreSources, "Nobody");
	CHECK(heard.size() == 1);
	CHECK(bridge.macroName(kAxes.coreSources) == "Macro " + std::to_string(kAxes.coreSources + 1));

	// A NAME PAST THE CAP IS CLAMPED, and the clamp is `clampMacroName`'s — and the patch holds the clamp.
	bridge.setMacroName(0, std::string(device::kMacroNameMax + 9, 'n'));
	CHECK(bridge.macroName(0) == std::string(device::kMacroNameMax, 'n'));
	CHECK(heard.size() == 2);
	const rack::RackPatch saved = bridge.patch();
	REQUIRE(saved.macroName.size() == kAxes.coreSources);
	CHECK(saved.macroName[0] == std::string(device::kMacroNameMax, 'n'));
	CHECK(saved.macroName[2] == "Wobble");
	CHECK(saved.macroName[3].empty()); // the default is saved as no name
	CHECK(saved.macroType[1] == static_cast<uint8_t>(device::MacroType::Latch));

	// CLEARING A REAL NAME IS A CHANGE BACK TO THE DEFAULT.
	heard.clear();
	bridge.setMacroName(0, "Macro 1");
	CHECK_FALSE(bridge.macroRenamed(0));
	REQUIRE(heard.size() == 1);
	CHECK(heard[0] == 0);

	// A LOAD CALLS FOR EXACTLY THE MACROS WHOSE EFFECTIVE NAME CHANGED.
	heard.clear();
	rack::RackPatch patch;
	patch.present = true;
	patch.macroName.assign(kAxes.coreSources, "");
	patch.macroName[5] = "Bass";
	patch.macroName[6] = "Macro 7";
	patch.macroType.assign(kAxes.coreSources, static_cast<uint8_t>(device::MacroType::Knob));
	patch.macroType[4] = static_cast<uint8_t>(device::MacroType::Momentary);
	(void)bridge.loadPatch(patch);
	REQUIRE(heard.size() == 2);
	CHECK(heard[0] == 2);
	CHECK(heard[1] == 5);
	CHECK(bridge.macroName(2) == "Macro 3");
	CHECK(bridge.macroName(5) == "Bass");
	CHECK_FALSE(bridge.macroRenamed(6));
	CHECK(bridge.macroType(4) == device::MacroType::Momentary);
	CHECK(bridge.macroType(1) == device::MacroType::Knob); // a load sets every type, not only the listed

	heard.clear();
	(void)bridge.loadPatch(patch);
	CHECK(heard.empty());

	rack::RackPatch bare;
	bare.present = true;
	(void)bridge.loadPatch(bare);
	REQUIRE(heard.size() == 1);
	CHECK(heard[0] == 5);
	CHECK(bridge.macroType(4) == device::MacroType::Knob);

	heard.clear();
	token.reset();
	bridge.setMacroName(7, "Late");
	CHECK(bridge.macroName(7) == "Late");
	CHECK(heard.empty());
}

TEST_CASE("the rack's rename subscribers both hear, and dropping one token leaves the other") {
	rack::RackEngine engine(kAxes, hostCatalogue());
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);
	std::vector<size_t> editor;
	std::vector<size_t> host;
	device::Subscription hostToken = bridge.subscribeMacroRename([&host](size_t index) { host.push_back(index); });
	device::Subscription editorToken =
	    bridge.subscribeMacroRename([&editor](size_t index) { editor.push_back(index); });

	bridge.setMacroName(6, "Swell");
	CHECK(host == std::vector<size_t> { 6 });
	CHECK(editor == std::vector<size_t> { 6 });

	editorToken.reset();
	bridge.setMacroName(1, "Grit");
	rack::RackPatch patch;
	patch.present = true;
	patch.macroName.assign(kAxes.coreSources, "");
	(void)bridge.loadPatch(patch); // clears 1 and 6
	CHECK(host == std::vector<size_t> { 6, 1, 1, 6 });
	CHECK(editor == std::vector<size_t> { 6 });
}

TEST_CASE("a bridge built over a loaded engine keeps its macro polarity — the nested rack's order") {
	// THE ORDER A RACK INSIDE A RACK FOLLOWS (`rack_device.cpp`, `inner_chain.cpp`): a body that arrives
	// before the catalogue is adopted is loaded into the ENGINE with no bridge over it, and the bridge is
	// built later. Its constructor once assigned every polarity unipolar.
	rack::RackEngine source(kAxes, hostCatalogue());
	source.setMacroBipolar(2, true);
	source.setMacroBipolar(6, true);
	MemStream s;
	REQUIRE(rack::saveRack(&s.out, source));

	rack::RackEngine engine(kAxes, hostCatalogue());
	rack::LoadReport refused;
	s.rewind();
	REQUIRE(rack::loadRack(&s.in, &engine, &refused));
	REQUIRE(engine.macroBipolar(2));
	REQUIRE(engine.macroBipolar(6));
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);
	for (size_t i = 0; i < kAxes.coreSources; ++i) {
		CAPTURE(i);
		CHECK(bridge.macroBipolar(i) == (i == 2 || i == 6));
	}

	MemStream out;
	REQUIRE(rack::writeRack(&out.out, bridge.patch()));
	rack::RackPatch parsed;
	REQUIRE(rack::parseRack(&out.in, &parsed));
	REQUIRE(parsed.macroBipolar.size() == kAxes.coreSources);
	for (size_t i = 0; i < kAxes.coreSources; ++i) {
		CAPTURE(i);
		CHECK(parsed.macroBipolar[i] == ((i == 2 || i == 6) ? 1 : 0));
	}

	bridge.setMacroBipolar(2, false);
	bridge.drain();
	CHECK_FALSE(engine.macroBipolar(2));
	CHECK(engine.macroBipolar(6));
}

//------------------------------------------------------------------------------------------------//
// PER-INSTANCE OUTPUT LABELS — what a player calls each of a device's outputs, saved beside its view //
// flags; since backlog JF BY OUTPUT KEY (`RackPatch::Device::labels`, `DeviceLabels`).              //
//------------------------------------------------------------------------------------------------//

namespace {

const device::Descriptor& macrosDescriptor() {
	const device::Descriptor* d = deviceForTag("MACROS");
	REQUIRE(d != nullptr);
	REQUIRE(d->outputCount() == 4);
	return *d;
}

rack::RackPatch::Value labelFor(const device::Descriptor& d, size_t output, std::string text) {
	return rack::RackPatch::Value::label(d.outputs[output].key, std::move(text));
}

} // namespace

TEST_CASE("device labels round-trip") {
	const std::string utf8 = "Fl\xC3\xBCgel \xE2\x86\x92 Sweep"; // "Flügel → Sweep", 17 bytes
	const std::string full(device::kMacroNameMax, 'x');            // exactly the cap
	REQUIRE(utf8.size() == 17);
	const std::vector<std::string> first = { "", "Wobble", utf8, full };
	const std::vector<std::string> second = { "Swell" };

	SUBCASE("a patch value, through the wire") {
		const device::Descriptor& macros = macrosDescriptor();
		rack::RackPatch p;
		p.present = true;
		for (int i = 0; i < 3; ++i) {
			rack::RackPatch::Device d;
			d.tag = rack::patchTagOf("MACROS");
			d.instanceId = static_cast<DeviceInstanceId>(i + 1);
			d.view = i == 0 ? rack::kViewFolded : 0u; // a view word beside the labels, so both stay in step
			test::setRow(d, macros, { 0.25f, 0.5f });
			p.device.push_back(d);
		}
		for (size_t o = 1; o < first.size(); ++o) {
			p.device[0].labels.push_back(labelFor(macros, o, first[o]));
		}
		p.device[2].labels.push_back(labelFor(macros, 0, second[0]));
		MemStream s;
		REQUIRE(rack::writeRack(&s.out, p));
		rack::RackPatch back;
		REQUIRE(rack::parseRack(&s.in, &back));
		REQUIRE(back.device.size() == 3);
		CHECK(back.device[0].labels == p.device[0].labels);
		CHECK(back.device[1].labels.empty());
		CHECK(back.device[2].labels == p.device[2].labels);
		CHECK(back.device[0].view == rack::kViewFolded);
		CHECK(back == p);
	}

	SUBCASE("a bridge, saved and loaded into another — under the id each device ends up wearing") {
		const device::Descriptor& macros = macrosDescriptor();
		rack::RackEngine engine(kAxes, hostCatalogue());
		engine.prepare(48000.0, 4096);
		rack::RackBridge bridge(engine);
		const DeviceInstanceId a = bridge.addDevice(macros.traits.tag);
		const DeviceInstanceId plain = bridge.addDevice(fx::filter::kTraits.tag);
		const DeviceInstanceId b = bridge.addDevice(macros.traits.tag);
		REQUIRE(a != kNoInstance);
		REQUIRE(plain != kNoInstance);
		REQUIRE(b != kNoInstance);
		for (size_t o = 0; o < first.size(); ++o) {
			bridge.setOutputLabel(a, o, first[o]);
		}
		bridge.setOutputLabel(b, 0, second[0]);
		// THE REORDER a fold's case makes too: a label remembered by POSITION would come back on another card.
		REQUIRE(bridge.moveDevice(2, 0));

		MemStream s;
		REQUIRE(rack::writeRack(&s.out, bridge.patch()));
		rack::RackPatch parsed;
		REQUIRE(rack::parseRack(&s.in, &parsed));

		rack::RackEngine other(kAxes, hostCatalogue());
		other.prepare(48000.0, 4096);
		rack::RackBridge loaded(other);
		REQUIRE(loaded.loadPatch(parsed).routes.count == 0);
		REQUIRE(loaded.chainLength() == 3);
		const DeviceInstanceId la = loaded.deviceAtPosition(1);
		const DeviceInstanceId lplain = loaded.deviceAtPosition(2);
		const DeviceInstanceId lb = loaded.deviceAtPosition(0);
		for (size_t o = 0; o < macros.outputCount(); ++o) {
			CAPTURE(o);
			const std::string declared = macros.outputs[o].name;
			const std::string want = o < first.size() && !first[o].empty() ? first[o] : declared;
			CHECK(loaded.outputLabel(la, o) == want);
			CHECK(loaded.outputRelabelled(la, o) == (o < first.size() && !first[o].empty()));
			CHECK(loaded.outputLabel(lb, o) == (o == 0 ? second[0] : declared));
		}
		CHECK_FALSE(loaded.outputRelabelled(lplain, 0));
	}
}

TEST_CASE("an over-cap label is cut at the cap on read; a label for no declared output is kept, not applied") {
	// **IN THE BINARY FORMAT BOTH OF THESE WERE REFUSALS** (a length or a count past its cap desynchronised
	// what followed). In the keyed document neither can: a label too long is cut where the writer cuts it,
	// and a label under a key this build's device does not declare — a newer build's output — names no
	// output here, so it is carried and saved back out, as an unknown value is.
	const device::Descriptor& macros = macrosDescriptor();
	rack::RackEngine engine(kAxes, hostCatalogue());
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);
	const DeviceInstanceId id = bridge.addDevice(macros.traits.tag);
	REQUIRE(id != kNoInstance);
	rack::RackPatch patch = bridge.patch();
	REQUIRE(patch.device.size() == 1);
	patch.device[0].labels = { labelFor(macros, 1, std::string(device::kMacroNameMax + 6, 'Q')),
		                       rack::RackPatch::Value::label("macro_9", "Future") };
	REQUIRE(bridge.loadPatch(patch).routes.count == 0);
	CHECK(bridge.outputLabel(id, 1) == std::string(device::kMacroNameMax, 'Q'));
	const rack::RackPatch resaved = bridge.patch();
	REQUIRE(resaved.device.size() == 1);
	CHECK(std::find(resaved.device[0].labels.begin(), resaved.device[0].labels.end(),
	                rack::RackPatch::Value::label("macro_9", "Future"))
	      != resaved.device[0].labels.end());
}

TEST_CASE("rack bridge: a relabelled output names its source; clearing restores the declared name") {
	const device::Descriptor& macros = macrosDescriptor();
	rack::RackEngine engine(kAxes, hostCatalogue());
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);
	const DeviceInstanceId id = bridge.addDevice(macros.traits.tag);
	REQUIRE(id != kNoInstance);
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
	CHECK(bridge.patch().device.at(0).labels.empty()); // and the patch carries nothing for it

	bridge.setOutputLabel(id, 2, std::string(device::kMacroNameMax + 5, 'n'));
	CHECK(bridge.outputLabel(id, 2) == std::string(device::kMacroNameMax, 'n'));
	bridge.setOutputLabel(id, macros.outputCount(), "Nobody");
	CHECK(bridge.outputLabel(id, macros.outputCount()) == "Source");
	bridge.setOutputLabel(id + 100, 0, "Nobody");
	CHECK(bridge.outputLabel(id + 100, 0) == "Source");

	bridge.removeDevice(id);
	CHECK_FALSE(bridge.outputRelabelled(id, 2));
}

TEST_CASE("rack bridge: two MACROS instances keep separate labels") {
	const device::Descriptor& macros = macrosDescriptor();
	rack::RackEngine engine(kAxes, hostCatalogue());
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);
	const DeviceInstanceId a = bridge.addDevice(macros.traits.tag);
	const DeviceInstanceId b = bridge.addDevice(macros.traits.tag);
	REQUIRE(a != kNoInstance);
	REQUIRE(b != kNoInstance);
	bridge.setOutputLabel(a, 0, "Wobble");
	bridge.setOutputLabel(b, 0, "Swell");
	CHECK(bridge.outputLabel(a, 0) == "Wobble");
	CHECK(bridge.outputLabel(b, 0) == "Swell");
	bridge.setOutputLabel(a, 0, "");
	CHECK(bridge.outputLabel(a, 0) == std::string(macros.outputs[0].name));
	CHECK(bridge.outputLabel(b, 0) == "Swell"); // clearing one is not clearing the other
}

TEST_CASE("rack bridge: a one-output device's source name is unchanged") {
	const device::Descriptor* lfo = deviceForTag("LFO");
	REQUIRE(lfo != nullptr);
	REQUIRE(lfo->outputCount() == 1);
	rack::RackEngine engine(kAxes, hostCatalogue());
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);
	const DeviceInstanceId id = bridge.addDevice(lfo->traits.tag);
	REQUIRE(id != kNoInstance);
	CHECK(bridge.outputLabel(id, 0) == std::string(lfo->outputs[0].name));
}

TEST_CASE("rack bridge: label edits dirty the patch and notify subscribers") {
	const device::Descriptor& macros = macrosDescriptor();
	rack::RackEngine engine(kAxes, hostCatalogue());
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);
	const DeviceInstanceId id = bridge.addDevice(macros.traits.tag);
	REQUIRE(id != kNoInstance);
	std::vector<std::pair<DeviceInstanceId, size_t>> heard;
	std::vector<std::pair<DeviceInstanceId, size_t>> other;
	device::Subscription token =
	    bridge.subscribeOutputLabel([&heard](DeviceInstanceId d, size_t o) { heard.emplace_back(d, o); });
	device::Subscription second =
	    bridge.subscribeOutputLabel([&other](DeviceInstanceId d, size_t o) { other.emplace_back(d, o); });

	bridge.setOutputLabel(id, 3, "Kick");
	REQUIRE(heard.size() == 1);
	CHECK(heard[0] == std::make_pair(id, size_t { 3 }));
	CHECK(other == heard);
	const rack::RackPatch saved = bridge.patch();
	REQUIRE(saved.device.size() == 1);
	CHECK(saved.device[0].labels == std::vector<rack::RackPatch::Value> { labelFor(macros, 3, "Kick") });

	bridge.setOutputLabel(id, 3, "Kick");
	CHECK(heard.size() == 1);
	bridge.setOutputLabel(id, 2, macros.outputs[2].name);
	CHECK(heard.size() == 1);
	bridge.setOutputLabel(id, macros.outputCount(), "x");
	CHECK(heard.size() == 1);
	second.reset();
	bridge.setOutputLabel(id, 3, "");
	REQUIRE(heard.size() == 2);
	CHECK(heard[1] == std::make_pair(id, size_t { 3 }));
	CHECK(other.size() == 1);
	CHECK(bridge.patch().device[0].labels.empty());
}

TEST_CASE("rack bridge: a patch load relabels and notifies once per changed label") {
	const device::Descriptor& macros = macrosDescriptor();
	rack::RackEngine engine(kAxes, hostCatalogue());
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);
	const DeviceInstanceId id = bridge.addDevice(macros.traits.tag);
	REQUIRE(id != kNoInstance);
	bridge.setOutputLabel(id, 0, "Wobble"); // will be cleared by the load
	bridge.setOutputLabel(id, 1, "Keep");   // will be the same after it
	rack::RackPatch patch = bridge.patch();
	REQUIRE(patch.device.size() == 1);
	REQUIRE(patch.device[0].instanceId == id);
	patch.device[0].labels = { labelFor(macros, 1, "Keep"), labelFor(macros, 2, "Swell"),
		                       labelFor(macros, 3, macros.outputs[3].name) }; // 3: the declared name spelled out

	std::vector<std::pair<DeviceInstanceId, size_t>> heard;
	device::Subscription token =
	    bridge.subscribeOutputLabel([&heard](DeviceInstanceId d, size_t o) { heard.emplace_back(d, o); });
	REQUIRE(bridge.loadPatch(patch).routes.count == 0);
	std::sort(heard.begin(), heard.end());
	CHECK(heard == std::vector<std::pair<DeviceInstanceId, size_t>> { { id, 0 }, { id, 2 } });
	CHECK(bridge.outputLabel(id, 0) == std::string(macros.outputs[0].name));
	CHECK(bridge.outputLabel(id, 1) == "Keep");
	CHECK(bridge.outputLabel(id, 2) == "Swell");
	CHECK_FALSE(bridge.outputRelabelled(id, 3));

	heard.clear();
	REQUIRE(bridge.loadPatch(patch).routes.count == 0);
	CHECK(heard.empty());
}

//------------------------------------------------------------------------------------------------//
// A LEAF'S BODY (backlog JD): Featherverb's IR, as a filename, rides every road a chain's body does  //
//------------------------------------------------------------------------------------------------//

namespace {

namespace fvb = feathers::fx::featherverb;
namespace cv = feathers::fx::conv;

/// A body naming a file that is not there — the device keeps the name and the shape, so what it saves is
/// what it was given, which is what these cases compare.
std::vector<uint8_t> leafBody() {
	cv::IrRef ref;
	ref.kind = cv::IrRef::Kind::File;
	ref.path = "/rooms/not-here.wav";
	ref.shape.begin = 0.2f;
	ref.shape.end = 0.8f;
	ref.shape.stretch = 1.25f;
	return cv::encodeIrRef(ref);
}

cv::IrRef decoded(const std::vector<uint8_t>& body) {
	cv::IrRef ref;
	REQUIRE(cv::decodeIrRef(body.data(), body.size(), ref));
	return ref;
}

} // namespace

TEST_CASE("a leaf device's body survives the rack's save and load through the engine road") {
	rack::RackEngine a(kAxes, hostCatalogue());
	a.prepare(48000.0, 4096);
	REQUIRE(a.addDevice(fvb::kTraits.tag) == 0);
	auto& verb = *static_cast<fvb::Featherverb*>(static_cast<void*>(a.slotAtLane(0).storage.get()));
	const std::vector<uint8_t> body = leafBody();
	REQUIRE(verb.loadBody(body.data(), body.size()));
	MemStream s;
	REQUIRE(rack::saveRack(&s.out, a));

	rack::RackEngine b(kAxes, hostCatalogue());
	b.prepare(48000.0, 4096);
	rack::LoadReport refused;
	s.rewind();
	REQUIRE(rack::loadRack(&s.in, &b, &refused));
	REQUIRE(b.chainLength() == 1);
	const auto& back = *static_cast<const fvb::Featherverb*>(static_cast<const void*>(b.slotAtLane(b.deviceAtPosition(0)).storage.get()));
	CHECK(back.irPath() == "/rooms/not-here.wav");
	CHECK(back.irTrim() == cv::IrTrim {0.2f, 0.8f});
	CHECK(back.irShape().stretch == 1.25f);
}

TEST_CASE("a leaf device's body survives the bridge road: filed at provision, saved live, loaded into a fresh rack") {
	rack::RackEngine engine(kAxes, hostCatalogue());
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge { engine };
	bridge.prepare(48000.0, 4096);
	const DeviceInstanceId id = bridge.addDevice(fvb::kTraits.tag);
	REQUIRE(id != kNoInstance);
	// A LEAF WITH A BODY IS FILED as a container is, so the host's save asks the LIVE instance.
	void* handle = bridge.containerSurfaceOf(id);
	REQUIRE(handle != nullptr);
	const std::vector<uint8_t> body = leafBody();
	REQUIRE(static_cast<fvb::Featherverb*>(handle)->loadBody(body.data(), body.size()));
	bridge.drain();
	bridge.publish(engine);

	const rack::RackPatch saved = bridge.patch();
	REQUIRE(saved.device.size() == 1);
	const cv::IrRef ref = decoded(saved.device[0].body);
	CHECK(ref.path == "/rooms/not-here.wav");
	CHECK(ref.shape.begin == 0.2f);
	CHECK(ref.shape.stretch == 1.25f);

	rack::RackEngine engine2(kAxes, hostCatalogue());
	engine2.prepare(48000.0, 4096);
	rack::RackBridge bridge2 { engine2 };
	bridge2.prepare(48000.0, 4096);
	(void)bridge2.loadPatch(saved);
	bridge2.drain();
	REQUIRE(saved.device[0].instanceId != kNoInstance);
	void* handle2 = bridge2.containerSurfaceOf(saved.device[0].instanceId);
	REQUIRE(handle2 != nullptr);
	const auto& back = *static_cast<const fvb::Featherverb*>(handle2);
	CHECK(back.irPath() == "/rooms/not-here.wav");
	CHECK(back.irTrim() == cv::IrTrim {0.2f, 0.8f});
	CHECK(back.irShape().stretch == 1.25f);
}

//------------------------------------------------------------------------------------------------//
// THE HOST SLOT POOL IN THE DOCUMENT (backlog JU; host-slots plan §2, T4)                        //
//------------------------------------------------------------------------------------------------//

namespace {

namespace hs = hostslots;

hs::Target slotTarget(std::vector<DeviceInstanceId> path, std::string key, std::string tag, std::string label) {
	return hs::Target { std::move(path), std::move(key), std::move(tag), std::move(label) };
}

/// A patch with one device and one route, so a case can see that the REST of a document loaded.
rack::RackPatch patchWithADevice() {
	rack::RackPatch p;
	p.present = true;
	rack::RackPatch::Device d;
	std::memcpy(d.tag.data(), "flt1", 4);
	d.instanceId = 7;
	d.values.push_back(rack::RackPatch::Value::of("cutoff", 0.25f));
	p.device.push_back(d);
	return p;
}

} // namespace

TEST_CASE("hostSlots round-trips through rackToText/parseRackText") {
	// EVERY FIELD OF `SlotRecord`, EACH SET TO SOMETHING ITS DEFAULT IS NOT, and every state; asserted against
	// the literals below rather than against the snapshot, so a writer and a reader that agreed on dropping a
	// field could not pass together. The clock is past 2^32 (a u64 read as u32 would fail), a level is
	// negative (a signed field read unsigned would fail), a label is UTF-8, and the released slot's
	// `beforeAnchor` is Pinned although it is not Anchored now.
	const std::string utf8 = "Rack 2 \xC2\xB7 Delay \xC2\xB7 Mix";
	hs::PoolSnapshot snap;
	snap.capacity = 96;
	snap.clock = (uint64_t { 1 } << 33) + 5;
	snap.slots = {
		hs::SlotRecord { 0, hs::SlotState::Default, slotTarget({ 3 }, "cutoff", "flt1", "Filter \xC2\xB7 Cutoff"), 0,
		                 1, 2, 3, hs::SlotState::Recent },
		hs::SlotRecord { 5, hs::SlotState::Recent, slotTarget({ 4, 11 }, "mix", "dly1", utf8), 17, -1, 0, 9,
		                 hs::SlotState::Pinned },
		hs::SlotRecord { 63, hs::SlotState::Pinned, slotTarget({ 9 }, "amount", "shp1", "Shaper \xC2\xB7 Amount"),
		                 snap.clock - 1, 0, 4, 0, hs::SlotState::Recent },
		hs::SlotRecord { 95, hs::SlotState::Anchored, slotTarget({ 4, 12, 2 }, "time", "dly1", "Deep"), snap.clock, 2,
		                 7, 1, hs::SlotState::Pinned },
	};
	REQUIRE(hs::SlotPool {}.restore(snap)); // a snapshot a pool takes, or the case proves nothing

	rack::RackPatch p = patchWithADevice();
	p.hostSlots = snap;
	std::string text;
	REQUIRE(rack::rackToText(p, &text));
	// STATES ARE WORDS, never the enum's number.
	CHECK(text.find("\"state\": \"anchored\"") != std::string::npos);
	CHECK(text.find("\"beforeAnchor\": \"pinned\"") != std::string::npos);

	rack::RackPatch back;
	rack::LoadReport report;
	REQUIRE(rack::parseRackText(text, &back, &report));
	CHECK(report.empty());
	REQUIRE(back.hostSlots.has_value());
	const hs::PoolSnapshot& b = *back.hostSlots;
	CHECK(b.capacity == 96);
	CHECK(b.clock == (uint64_t { 1 } << 33) + 5);
	REQUIRE(b.slots.size() == 4);

	CHECK(b.slots[0].index == 0);
	CHECK(b.slots[0].state == hs::SlotState::Default);
	CHECK(b.slots[0].target.path == std::vector<DeviceInstanceId> { 3 });
	CHECK(b.slots[0].target.key == "cutoff");
	CHECK(b.slots[0].target.tag == "flt1");
	CHECK(b.slots[0].target.label == "Filter \xC2\xB7 Cutoff");
	CHECK(b.slots[0].recency == 0);
	CHECK(b.slots[0].level == 1);
	CHECK(b.slots[0].rank == 2);
	CHECK(b.slots[0].chainOrder == 3);
	CHECK(b.slots[0].beforeAnchor == hs::SlotState::Recent);

	CHECK(b.slots[1].index == 5);
	CHECK(b.slots[1].state == hs::SlotState::Recent);
	CHECK(b.slots[1].target.path == std::vector<DeviceInstanceId> { 4, 11 });
	CHECK(b.slots[1].target.key == "mix");
	CHECK(b.slots[1].target.tag == "dly1");
	CHECK(b.slots[1].target.label == utf8);
	CHECK(b.slots[1].recency == 17);
	CHECK(b.slots[1].level == -1);
	CHECK(b.slots[1].rank == 0);
	CHECK(b.slots[1].chainOrder == 9);
	CHECK(b.slots[1].beforeAnchor == hs::SlotState::Pinned);

	CHECK(b.slots[2].index == 63);
	CHECK(b.slots[2].state == hs::SlotState::Pinned);
	CHECK(b.slots[2].target.path == std::vector<DeviceInstanceId> { 9 });
	CHECK(b.slots[2].target.key == "amount");
	CHECK(b.slots[2].target.tag == "shp1");
	CHECK(b.slots[2].target.label == "Shaper \xC2\xB7 Amount");
	CHECK(b.slots[2].recency == (uint64_t { 1 } << 33) + 4);
	CHECK(b.slots[2].level == 0);
	CHECK(b.slots[2].rank == 4);
	CHECK(b.slots[2].chainOrder == 0);
	CHECK(b.slots[2].beforeAnchor == hs::SlotState::Recent);

	CHECK(b.slots[3].index == 95);
	CHECK(b.slots[3].state == hs::SlotState::Anchored);
	CHECK(b.slots[3].target.path == std::vector<DeviceInstanceId> { 4, 12, 2 });
	CHECK(b.slots[3].target.key == "time");
	CHECK(b.slots[3].target.tag == "dly1");
	CHECK(b.slots[3].target.label == "Deep");
	CHECK(b.slots[3].recency == (uint64_t { 1 } << 33) + 5);
	CHECK(b.slots[3].level == 2);
	CHECK(b.slots[3].rank == 7);
	CHECK(b.slots[3].chainOrder == 1);
	CHECK(b.slots[3].beforeAnchor == hs::SlotState::Pinned);

	// AND THE WHOLE, second: the field checks above are what name a failure, this is what catches a field
	// added to `SlotRecord` and forgotten here.
	CHECK(back == p);
	// AND SAVE → LOAD → SAVE IS BYTE-IDENTICAL (state-format §3.2).
	std::string again;
	REQUIRE(rack::rackToText(back, &again));
	CHECK(again == text);
}

TEST_CASE("a document without hostSlots parses with the member absent") {
	const rack::RackPatch p = patchWithADevice();
	std::string text;
	REQUIRE(rack::rackToText(p, &text));
	CHECK(text.find("hostSlots") == std::string::npos); // nothing written for a patch that owns no pool
	rack::RackPatch back;
	rack::LoadReport report;
	REQUIRE(rack::parseRackText(text, &back, &report));
	CHECK(report.empty());
	CHECK_FALSE(back.hostSlots.has_value());
	CHECK(back.device.size() == 1);
	// AN EMPTY POOL IS NOT AN ABSENT ONE: present, with no slots, it is written and read back as present.
	rack::RackPatch empty = p;
	empty.hostSlots = hs::PoolSnapshot {};
	REQUIRE(rack::rackToText(empty, &text));
	REQUIRE(rack::parseRackText(text, &back, &report));
	REQUIRE(back.hostSlots.has_value());
	CHECK(back.hostSlots->slots.empty());
	CHECK(back.hostSlots->capacity == hs::kHostSlotCapacity);
}

TEST_CASE("an anchored slot is anchored after a load into a fresh instance, with no host event") {
	// DESIGN §6, LOAD-BEARING: a lane the host recorded last session is not written again until playback
	// reaches it, so an anchor that did not survive the reload would leave its slot recyclable in exactly
	// that window. Saved from a live pool, through the text, into a pool that has never seen a host event.
	const hs::Target anchoredTarget = slotTarget({ 2, 5 }, "mix", "dly1", "Rack \xC2\xB7 Delay \xC2\xB7 Mix");
	hs::SlotPool live;
	const hs::Allocation a = live.touch(anchoredTarget);
	REQUIRE(a.slot != hs::kNoSlot);
	REQUIRE(live.anchor(a.slot));
	// THE REST OF THE POOL FULL OF ORDINARY RECENT SLOTS, so a fresh pool's first takeover has to choose.
	for (uint32_t i = 0; i + 1 < hs::kHostSlotCapacity; ++i) {
		REQUIRE(live.touch(slotTarget({ 100 + i }, "k", "flt1", "x")).slot != hs::kNoSlot);
	}

	rack::RackPatch p = patchWithADevice();
	p.hostSlots = live.snapshot();
	std::string text;
	REQUIRE(rack::rackToText(p, &text));
	rack::RackPatch back;
	REQUIRE(rack::parseRackText(text, &back));
	REQUIRE(back.hostSlots.has_value());

	hs::SlotPool fresh;
	REQUIRE(fresh.restore(*back.hostSlots));
	CHECK(fresh.slotOf(anchoredTarget) == a.slot);
	CHECK(fresh.state(a.slot) == hs::SlotState::Anchored);
	CHECK(fresh.target(a.slot).label == "Rack \xC2\xB7 Delay \xC2\xB7 Mix");

	// FILLING THE POOL NEVER TAKES IT — defaults (there is nothing Empty to fill), then more touches than
	// the pool has slots, each of which recycles the least recently used Recent.
	std::vector<hs::Candidate> candidates;
	for (uint32_t i = 0; i < 2 * hs::kHostSlotCapacity; ++i) {
		candidates.push_back(hs::Candidate { slotTarget({ 500 + i }, "d", "flt1", "d"), 0, 0, static_cast<int32_t>(i) });
	}
	(void)fresh.fillDefaults(candidates);
	uint32_t recycled = 0;
	for (uint32_t i = 0; i < 2 * hs::kHostSlotCapacity; ++i) {
		const hs::Allocation took = fresh.touch(slotTarget({ 1000 + i }, "t", "flt1", "t"));
		CHECK(took.slot != a.slot);
		recycled += took.took == hs::Allocation::Took::Recent ? 1 : 0;
	}
	CHECK(recycled == 2 * hs::kHostSlotCapacity); // the pool WAS under pressure: every touch recycled
	CHECK(fresh.state(a.slot) == hs::SlotState::Anchored);
	CHECK(fresh.slotOf(anchoredTarget) == a.slot);
}

TEST_CASE("the host mark survives save and reload, and a pool without marks writes no member for them") {
	// By design, an id the host has touched keeps its lanes after the slot is released or freed, so the
	// mark is saved with the pool — Empty slots' marks included — and a reopened project reuses those ids last.
	hs::SlotPool live;
	const hs::Target t = slotTarget({ 7 }, "cutoff", "flt1", "Filter \xC2\xB7 Cutoff");
	REQUIRE(live.touch(t).slot == 0);
	REQUIRE(live.markHost(0));
	REQUIRE(live.markHost(5)); // Empty, and marked: its lane outlived its mapping
	rack::RackPatch p = patchWithADevice();
	p.hostSlots = live.snapshot();
	std::string text;
	REQUIRE(rack::rackToText(p, &text, false));
	CHECK(text.find("\"hostMarked\":[0,5]") != std::string::npos);
	rack::RackPatch back;
	rack::LoadReport report;
	REQUIRE(rack::parseRackText(text, &back, &report));
	CHECK(report.empty());
	REQUIRE(back.hostSlots.has_value());
	CHECK(back.hostSlots->hostMarked == std::vector<hs::SlotIndex> { 0, 5 });
	hs::SlotPool fresh;
	REQUIRE(fresh.restore(*back.hostSlots));
	CHECK(fresh.hostMarked(0));
	CHECK(fresh.hostMarked(5));
	CHECK_FALSE(fresh.hostMarked(1));
	// AND THE REOPENED POOL REUSES THE MARKED EMPTY LAST: the next touch takes clean slot 1, not 5.
	CHECK(fresh.touch(slotTarget({ 8 }, "mix", "dly1", "Delay \xC2\xB7 Mix")).slot == 1);

	// A POOL THE HOST NEVER TOUCHED writes no `hostMarked` at all — a document from before the mark, byte for byte.
	hs::SlotPool clean;
	REQUIRE(clean.touch(t).slot == 0);
	p.hostSlots = clean.snapshot();
	REQUIRE(rack::rackToText(p, &text, false));
	CHECK(text.find("hostMarked") == std::string::npos);
	REQUIRE(rack::parseRackText(text, &back, &report));
	REQUIRE(back.hostSlots.has_value());
	CHECK(back.hostSlots->hostMarked.empty());
}

TEST_CASE("a malformed hostSlots member is reported and dropped, and the rest of the patch loads") {
	// THE ONE MEMBER DROPPED ALONE (the header says why): the devices load, the map does not, and the report
	// says so. Each malformation is made from a valid document, so the rest of it is known readable.
	hs::PoolSnapshot snap;
	snap.clock = 3;
	snap.slots = {
		hs::SlotRecord { 1, hs::SlotState::Anchored, slotTarget({ 7 }, "cutoff", "flt1", "Filter \xC2\xB7 Cutoff"), 2,
		                 0, 0, 0, hs::SlotState::Recent },
		hs::SlotRecord { 2, hs::SlotState::Recent, slotTarget({ 7 }, "res", "flt1", "Filter \xC2\xB7 Res"), 3, 0, 0,
		                 0, hs::SlotState::Recent },
	};
	rack::RackPatch p = patchWithADevice();
	p.hostSlots = snap;
	std::string good;
	REQUIRE(rack::rackToText(p, &good, false)); // compact, so each edit below is one exact substring
	const auto replaced = [&](const std::string& from, const std::string& to) {
		std::string t = good;
		const size_t at = t.find(from);
		REQUIRE(at != std::string::npos);
		t.replace(at, from.size(), to);
		return t;
	};

	// EACH MALFORMATION WITH THE REASON THE REPORT MUST GIVE — which check failed, not only that one did.
	std::string text;
	std::string reason;
	SUBCASE("a state as a number") {
		text = replaced("\"state\":\"anchored\"", "\"state\":4");
		reason = "entry 1 of the slot list could not be read";
	}
	SUBCASE("a state this build does not know") {
		text = replaced("\"state\":\"anchored\"", "\"state\":\"locked\"");
		reason = "entry 1 of the slot list could not be read";
	}
	SUBCASE("a missing field") {
		text = replaced("\"beforeAnchor\":\"recent\"", "\"beforeAnchorX\":\"recent\"");
		reason = "entry 1 of the slot list could not be read";
	}
	SUBCASE("two slots on one control") {
		text = replaced("\"key\":\"res\"", "\"key\":\"cutoff\"");
		reason = "slot 2 names the control slot 1 names";
	}
	SUBCASE("a path through no device") {
		text = replaced("\"path\":[7]", "\"path\":[7,0]"); // the first slot's
		reason = "slot 1 has a path through no device";
	}
	SUBCASE("a capacity past the pool's limit") {
		text = replaced("\"capacity\":" + std::to_string(hs::kHostSlotCapacity), "\"capacity\":4097");
		reason = "a capacity of 4097, past the most a pool may have (4096)";
	}
	SUBCASE("host marks that are not a list") {
		text = replaced("\"slots\":[", "\"hostMarked\":7,\"slots\":[");
		reason = "the host marks are not a list";
	}
	SUBCASE("a host mark that is not a slot index") {
		text = replaced("\"slots\":[", "\"hostMarked\":[-1],\"slots\":[");
		reason = "a host mark is not a slot index";
	}
	SUBCASE("host marks out of order") {
		text = replaced("\"slots\":[", "\"hostMarked\":[3,1],\"slots\":[");
		reason = "host-marked slot 1 is out of order or listed twice";
	}
	SUBCASE("not an object") {
		const size_t at = good.find("\"hostSlots\":{");
		REQUIRE(at != std::string::npos);
		text = good.substr(0, at) + "\"hostSlots\":[1,2]}";
		reason = "not an object";
	}
	REQUIRE_FALSE(text.empty());
	// A CONTROL: an unchanged document keeps its map, so the drop below is the malformation's doing.
	{
		rack::RackPatch back;
		rack::LoadReport report;
		REQUIRE(rack::parseRackText(good, &back, &report));
		REQUIRE(report.empty());
		REQUIRE(back.hostSlots.has_value());
	}

	rack::RackPatch back;
	rack::LoadReport report;
	REQUIRE(rack::parseRackText(text, &back, &report)); // the patch is NOT refused
	CHECK(back.present);
	CHECK_FALSE(back.hostSlots.has_value());
	REQUIRE(back.device.size() == 1);
	CHECK(back.device[0].instanceId == 7);
	REQUIRE(back.device[0].values.size() == 1);
	CHECK(back.device[0].values[0].number == 0.25f);
	CHECK(report.countOf(rack::LoadReport::Kind::SlotsDropped) == 1);
	CHECK(report.countOf(rack::LoadReport::Kind::NotLoaded) == 0);
	CHECK(report.firstOf(rack::LoadReport::Kind::SlotsDropped).what == "hostSlots");
	CHECK(report.firstOf(rack::LoadReport::Kind::SlotsDropped).why == reason);
	const std::string line = rack::loadReportLine(report, {});
	CHECK(line.rfind("Loaded with changes: the host slot map could not be read (" + reason + ")", 0) == 0);
}

TEST_CASE("hostSlots numbers are exact: a clock of 2^53+1 round-trips, and a path id of -1, 3.0 or 2^32 is refused") {
	// 2^53+1 IS THE FIRST INTEGER A DOUBLE CANNOT HOLD, so a reader that went through one would hand back
	// 2^53 and a pool whose clock had quietly stepped backwards.
	constexpr uint64_t kClock = (uint64_t { 1 } << 53) + 1;
	hs::PoolSnapshot snap;
	snap.clock = kClock;
	snap.slots = { hs::SlotRecord { 1, hs::SlotState::Recent, slotTarget({ 7 }, "cutoff", "flt1", "c"), kClock, 0, 0, 0,
		                            hs::SlotState::Recent } };
	rack::RackPatch p = patchWithADevice();
	p.hostSlots = snap;
	std::string text;
	REQUIRE(rack::rackToText(p, &text, false));
	CHECK(text.find("\"clock\":9007199254740993") != std::string::npos);
	CHECK(text.find("\"recent\":9007199254740993") != std::string::npos);
	{
		rack::RackPatch back;
		rack::LoadReport report;
		REQUIRE(rack::parseRackText(text, &back, &report));
		CHECK(report.empty());
		REQUIRE(back.hostSlots.has_value());
		CHECK(back.hostSlots->clock == kClock);
		REQUIRE(back.hostSlots->slots.size() == 1);
		CHECK(back.hostSlots->slots[0].recency == kClock);
	}

	// A PATH ID IS A WHOLE UNSIGNED 32-BIT NUMBER OR NOTHING — never wrapped, truncated or rounded into some
	// other device's id.
	const std::string pathOf7 = "\"path\":[7]";
	REQUIRE(text.find(pathOf7) != std::string::npos);
	for (const char* bad : { "-1", "3.0", "4294967296" }) {
		CAPTURE(bad);
		std::string t = text;
		t.replace(t.find(pathOf7), pathOf7.size(), std::string("\"path\":[") + bad + "]");
		rack::RackPatch back;
		rack::LoadReport report;
		REQUIRE(rack::parseRackText(t, &back, &report));
		CHECK_FALSE(back.hostSlots.has_value());
		CHECK(report.countOf(rack::LoadReport::Kind::SlotsDropped) == 1);
		CHECK(report.firstOf(rack::LoadReport::Kind::SlotsDropped).why == "slot 1 has a path that is not device ids");
	}
	// AND THE LARGEST ID THAT FITS IS ACCEPTED, so the refusals above are about the number, not about editing.
	std::string t = text;
	t.replace(t.find(pathOf7), pathOf7.size(), "\"path\":[4294967295]");
	rack::RackPatch back;
	REQUIRE(rack::parseRackText(t, &back));
	REQUIRE(back.hostSlots.has_value());
	CHECK(back.hostSlots->slots[0].target.path == std::vector<DeviceInstanceId> { 4294967295u });
}

// NO DEVICE IS CAPPED BY A STRIDE (2026-10-19; the retired control stride was 32 and a refusal, and no
// bound replaces it; no route budget remains, backlog OJ). The devices here are FAKES with
// trivial ops, as the graph suite's are, because no shipped device is 64 wide. Their axes are the
// shipped shape, so a 64-control device is admitted whole.
namespace {

constexpr size_t kWideCount = 127; // the row width (128) is the ceiling now: 127 is Drawn's width

struct WideKeys {
	char k[kWideCount + 1][5] {};
	constexpr WideKeys() {
		for (size_t i = 0; i < kWideCount; ++i) {
			k[i][0] = 'c';
			k[i][1] = static_cast<char>('0' + i / 100);
			k[i][2] = static_cast<char>('0' + i / 10 % 10);
			k[i][3] = static_cast<char>('0' + i % 10);
		}
	}
};
constexpr WideKeys kWideKeys;

/// `first` controls that are NOT modulatable, then `kWideCount` keyed c00..c63 where those from `firstMod` up are;
/// `lead` puts one extra non-modulatable control "new" in front (an INSERTED control).
template <size_t Lead> struct WideParams {
	device::ParamDesc p[kWideCount + Lead] {};
	constexpr WideParams(size_t firstMod) {
		if constexpr (Lead == 1) {
			p[0] = { .key = "new", .name = "New", .min = 0, .max = 1, .def = 0, .stepped = false,
				     .unit = device::Unit::Number, .names = {}, .modulatable = false };
		}
		for (size_t i = 0; i < kWideCount; ++i) {
			p[Lead + i] = { .key = kWideKeys.k[i], .name = "Wide", .min = 0, .max = 1, .def = 0.25, .stepped = false,
				            .unit = device::Unit::Number, .names = {}, .modulatable = i >= firstMod };
		}
	}
};
constexpr WideParams<0> kAllMod(0);
constexpr WideParams<0> kLateMod(36);   // control indices 36..63 are the modulatable ones: ordinal != index, index >= 32
constexpr WideParams<1> kInserted(0);   // kAllMod with a control inserted at the front

void wideNoop(void*) {}
void widePrepare(void*, double, int) {}
void wideSetFields(void*, const float*, size_t) {}
void wideProcess(void*, float*, float*, int) {}

constexpr device::AudioOps kWideAudio { .stateBytes = 16, .stateAlign = 16, .construct = wideNoop,
	                                    .destruct = wideNoop, .prepare = widePrepare, .reset = wideNoop,
	                                    .setSlot = nullptr, .setFields = wideSetFields,
	                                    .setNoteContext = nullptr, .setTransportClock = nullptr,
	                                    .processStereo = wideProcess, .processMono = nullptr };
constexpr device::Traits kWideTraits { .tag = "WIDE", .name = "Wide", .hasCard = false, .hasExpanded = false };
constexpr device::Traits kLateTraits { .tag = "WLAT", .name = "Wide late", .hasCard = false, .hasExpanded = false };
constexpr device::Descriptor kWide { .traits = kWideTraits, .params = kAllMod.p, .outputs = {}, .audio = &kWideAudio };
constexpr device::Descriptor kLate { .traits = kLateTraits, .params = kLateMod.p, .outputs = {}, .audio = &kWideAudio };
constexpr device::Descriptor kWideShifted { .traits = kWideTraits, .params = kInserted.p, .outputs = {}, .audio = &kWideAudio };

bool gShifted = false; ///< which WIDE the catalogue answers: the original, or one with a control inserted in front

const device::Descriptor* wideFind(void*, const char* tag) {
	const std::string t(tag);
	if (t == kWideTraits.tag) {
		return gShifted ? &kWideShifted : &kWide;
	}
	return t == kLateTraits.tag ? &kLate : hostCatalogue()(tag);
}

constexpr backplane::Axes kWideAxes { .coreSources = 8, .coreDestinations = 0, .deviceSlots = 4,
	                                  .outputStride = 4 };

} // namespace

TEST_CASE("a device wider than the route budget routes to every control, and its routes survive save and load") {
	const device::Catalogue catalogue { .host = nullptr, .find = wideFind };
	gShifted = false;
	REQUIRE(kWide.modulatableCount() == kWideCount);
	REQUIRE(kWide.modulatableCount() > 32); // wider than the old route budget, and not refused for it
	const size_t last = kWideCount - 1;

	rack::RackEngine a(kWideAxes, catalogue);
	a.prepare(48000.0, 256);
	const size_t slot = a.addDevice(kWideTraits.tag);
	REQUIRE(slot != rack::RackEngine::kNoSlot);
	a.setRouteAmount(test::macro(0), test::controlOf(a, slot, last), 0.5f);
	a.setRouteAmount(test::macro(1), test::controlOf(a, slot, 40), -0.25f);
	a.setField(slot, 36, 0.875f); // a BASE past 32 must come back too
	REQUIRE(a.routeAmount(test::macro(0), test::controlOf(a, slot, last)) == doctest::Approx(0.5f));
	a.setMacro(0, 1.0f);
	a.setMacro(1, 1.0f);
	std::vector<float> l(64, 0.0f), r(64, 0.0f);
	a.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
	CHECK(a.resolvedFieldForTest(slot, last) == doctest::Approx(0.75f));
	CHECK(a.resolvedFieldForTest(slot, 40) == doctest::Approx(0.0f));
	CHECK(a.resolvedFieldForTest(slot, 8) == doctest::Approx(0.25f)); // nothing aliased onto an earlier control
	CHECK(a.inputCountForTest(slot, last) == 1);

	MemStream s;
	REQUIRE(rack::saveRack(&s.out, a));
	rack::RackEngine b(kWideAxes, catalogue);
	rack::LoadReport refused;
	s.rewind();
	REQUIRE(rack::loadRack(&s.in, &b, &refused));
	REQUIRE(refused.routes.count == 0);
	REQUIRE(b.slotAtLane(0).live());
	CHECK(b.routeAmount(test::macro(0), test::controlOf(b, 0, last)) == doctest::Approx(0.5f));
	CHECK(b.routeAmount(test::macro(1), test::controlOf(b, 0, 40)) == doctest::Approx(-0.25f));
	CHECK(b.routeAmount(test::macro(0), test::controlOf(b, 0, 31)) == doctest::Approx(0.0f));
	CHECK(b.fieldAt(0, 36) == doctest::Approx(0.875f));
}

TEST_CASE("a control inserted before a saved route's control does not move the route: it is saved by key") {
	const device::Catalogue catalogue { .host = nullptr, .find = wideFind };
	gShifted = false;
	rack::RackEngine a(kWideAxes, catalogue);
	a.prepare(48000.0, 256);
	const size_t slot = a.addDevice(kWideTraits.tag);
	REQUIRE(slot != rack::RackEngine::kNoSlot);
	a.setRouteAmount(test::macro(0), test::controlOf(a, slot, 40), 0.5f); // key "c040"
	MemStream s;
	REQUIRE(rack::saveRack(&s.out, a));

	gShifted = true; // the same device, one control inserted at the front: c40 is now index 41
	rack::RackEngine b(kWideAxes, catalogue);
	rack::LoadReport refused;
	s.rewind();
	REQUIRE(rack::loadRack(&s.in, &b, &refused));
	REQUIRE(refused.routes.count == 0);
	REQUIRE(std::string(b.slotAtLane(0).live_desc()->params[41].key) == "c040");
	CHECK(b.routeAmount(test::macro(0), test::controlOf(b, 0, 41)) == doctest::Approx(0.5f));
	CHECK(b.routeAmount(test::macro(0), test::controlOf(b, 0, 40)) == doctest::Approx(0.0f));
	gShifted = false;
}

TEST_CASE("the bridge publishes a modulated value for a control INDEX past 32, even when its modulatable ordinal is small") {
	const device::Catalogue catalogue { .host = nullptr, .find = wideFind };
	rack::RackEngine engine(kWideAxes, catalogue);
	engine.prepare(48000.0, 256);
	rack::RackBridge bridge(engine);
	const DeviceInstanceId id = bridge.addDevice(kLateTraits.tag);
	REQUIRE(id != kNoInstance);
	bridge.drain();
	// Controls 36..63 are the modulatable ones, so control 40 is ordinal 4 and index 40: the routing bound compared
	// the ordinal, the bridge's pool the index, and the two disagreed from 32.
	REQUIRE(kLate.nthModulatableOfControl(40) == 4);
	bridge.setRouteAmount(test::macro(0), backplane::Destination::control(id, 40), 0.5f);
	bridge.setMacro(0, 1.0f);
	std::vector<float> l(64, 0.0f), r(64, 0.0f);
	for (int i = 0; i < 2; ++i) {
		bridge.drain();
		engine.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
		bridge.publish(engine);
	}
	CHECK(bridge.modulatedFieldAt(id, 40) == doctest::Approx(0.75f)); // base 0.25 + 0.5 x span 1
	CHECK(bridge.modulatedFieldAt(id, 63) == doctest::Approx(0.25f));
}
