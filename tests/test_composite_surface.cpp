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

#include "rack/rack_device.h"
#include "composite/composite_types.h"
#include "plugin/device_registry.h"
#include "plugin/device_row.h"

#include <algorithm>
#include <cstring>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

// THE OUTWARD SURFACE OF A CHAIN — the chain-as-a-device design, step 6a.
//
// WHAT THESE CASES CAN AND CANNOT PROVE. The descriptor has no ops and nothing constructs it, so
// there is no behaviour here to observe; what there is, is a DECLARATION that other code addresses
// through a fixed pair of functions. Every case below asserts a value that some future host will
// compute from this descriptor — which is the only kind of claim a surface-without-a-body can
// support, and is exactly the shape step 1 of the graph arc took.

using namespace feathers;

namespace {

constexpr size_t kMacro1 = static_cast<size_t>(composite::Param::Macro1);

} // namespace

TEST_CASE("a chain's outward surface is its macros, its mix and its gain — and nothing inside it") {
	const device::Descriptor& d = composite::kDescriptor;

	// ELEVEN DECLARED, ALL DESTINATIONS SINCE BACKLOG KW: the enable too, a gate at one half.
	CHECK(d.params.size() == 11);
	CHECK(d.modulatableCount() == 11);
	CHECK(d.params[static_cast<size_t>(composite::Param::Enabled)].modulatable);

	// AND THE TEN ARE THE ONES D6 NAMES. Asserted by NAME rather than by index, because the claim
	// being made is about which controls an outer host may route to, and an index would restate the
	// enum rather than check it.
	std::vector<std::string_view> destinations;
	for (size_t n = 0; n < d.modulatableCount(); ++n) {
		destinations.push_back(d.params[d.controlOfNthModulatable(n)].name);
	}
	CHECK(destinations
	      == std::vector<std::string_view> { "Chain Enabled", "Chain Mix", "Chain Gain", "Macro 1", "Macro 2",
		                                      "Macro 3", "Macro 4", "Macro 5", "Macro 6", "Macro 7",
		                                      "Macro 8" });
}

TEST_CASE("every macro's ordinal round-trips, which is how a host addresses one without knowing it "
          "is a chain") {
	const device::Descriptor& d = composite::kDescriptor;

	// THE PAIR EVERY HOST ADDRESSES A DESTINATION THROUGH. A route stores an ordinal; a card holds a
	// control index; these two are the only road between them. They differ whenever a device has a
	// non-modulatable control among its modulatable ones — which this device does, at index 0 — so
	// the round trip is a real claim here and not an identity.
	for (size_t n = 0; n < composite::kMacroCount; ++n) {
		const size_t control = kMacro1 + n;
		const size_t ordinal = d.nthModulatableOfControl(control);
		CHECK(ordinal == n + 3); // the enable (a destination since backlog KW), mix and gain take ordinals 0, 1 and 2
		CHECK(d.controlOfNthModulatable(ordinal) == control);
	}

	// AND THE ENABLE IS ORDINAL 0 SINCE BACKLOG KW (a destination, a gate), and a control past the end answers out of
	// range rather than plausibly — the registry's stated contract: "the out-of-range answer is a valid subscript
	// nowhere, so a caller that ignores it gets a bounds failure rather than a wrong control".
	CHECK(d.nthModulatableOfControl(static_cast<size_t>(composite::Param::Enabled)) == 0);
	CHECK(d.nthModulatableOfControl(d.params.size()) == d.modulatableCount());
	CHECK(d.controlOfNthModulatable(d.modulatableCount()) == d.params.size());
}

TEST_CASE("the SURFACE declares no capability, and the chain in the catalogue is the RUNNABLE one") {
	// **THIS CASE USED TO SAY THE CHAIN WAS IN NO CATALOGUE, AND IT WAS THE GUARD ON THAT.** It went red
	// the day the chain was registered, which is exactly what it was for: its comment said *"this case
	// is what fails if somebody registers it first"* — first meaning before it had ops, a card and
	// eleven live controls. It was registered AFTER all three, so it is rewritten into the statement
	// that is now true rather than deleted.
	const device::Descriptor& surface = composite::kDescriptor;

	// THE SURFACE STILL DECLARES NOTHING IT CANNOT DO. It is the outward shape — tag, name, eleven
	// controls — and it lives in a header with no ops behind it, so it must keep saying so.
	CHECK(surface.audio == nullptr);
	CHECK(surface.mod == nullptr);
	CHECK(surface.notes == nullptr);
	CHECK(surface.container == nullptr);
	CHECK(surface.outputCount() == 0);

	// AND hasExpanded IS TRUE NOW THAT THERE IS A VIEW TO OPEN — `rack::ChainExpanded`, the inner rack's own
	// editor over the bridge inside the device (rack roadmap §6, 2026-09-13), which each GUI registers at
	// creation (`registerChainExpandedView`). Both halves or neither, still: this pinned FALSE while the state
	// it guarded against was the one that put "THERE IS NO EXPANDED VIEW AVAILABLE" on screen, and
	// the widget suite's "EVERY card whose device declares an expanded view can actually OPEN it" is the
	// half that proves the view exists.
	CHECK(surface.traits.hasExpanded == true);

	// **EXACTLY ONE CATALOGUE ENTRY CARRIES THE TAG, AND IT IS THE RUNNABLE ONE.** Two descriptors
	// under one tag is the hazard a duplicate-tag check exists for, and the two here must be one
	// device described twice rather than two devices sharing a name — so the entry is asserted to BE
	// the runnable descriptor, and to share the surface's controls by address rather than by copy.
	const device::Descriptor* registered = nullptr;
	size_t found = 0;
	for (const device::Descriptor& reg : kDevices) {
		if (device::sameTag(reg.traits.tag, surface.traits.tag)) {
			registered = &reg;
			++found;
		}
	}
	REQUIRE(found == 1);
	REQUIRE(registered != nullptr);
	CHECK(registered->audio == &rack::kDeviceAudioOps);
	CHECK(registered->container == &rack::kDeviceContainerOps);
	CHECK(registered->params.data() == surface.params.data());
	CHECK(registered->params.size() == surface.params.size());
	CHECK(registered->traits.hasExpanded == surface.traits.hasExpanded);
}

TEST_CASE("a chain's surface fits a settings row, with the budget the widest device left") {
	// THE ROW IS THE TRANSPORT and every device must fit it. The composite's eleven controls are not
	// the problem — its STATE is, and that is step 6d. This case pins the half that is settled, so
	// that a later widening of the surface cannot quietly become the thing that overflows.
	CHECK(composite::kDescriptor.params.size() <= kMaxDeviceFields);

	// AND ITS MODULATABLE COUNT FITS THE STRIDE a host lays its destination table out at.
}

TEST_CASE("the macro block is contiguous and last, which is what makes Macro1 + n an address") {
	// A COMPILE-TIME FACT RESTATED AS A CASE, deliberately. `composite_types.h` carries a
	// static_assert for this, which is the mechanism; a reader meeting the arithmetic in a host will
	// meet it here first, and a static_assert that fires takes the whole build down without naming
	// what a hand broke.
	CHECK(kMacro1 + composite::kMacroCount
	      == static_cast<size_t>(composite::Param::kCount));

	for (size_t n = 0; n < composite::kMacroCount; ++n) {
		const device::ParamDesc& p = composite::kParamDescs[kMacro1 + n];
		CHECK(p.modulatable);
		CHECK(p.min == doctest::Approx(0.0));
		CHECK(p.max == doctest::Approx(1.0));
		CHECK(p.def == doctest::Approx(0.0));
	}
}


//------------------------------------------------------------------------------------//
// THE CONTAINER CAPABILITY — step 6b.                                                  //
//------------------------------------------------------------------------------------//

TEST_CASE("the device vocabulary includes nothing from the plugin or the backplane, which is what "
          "forces a container's services to be function pointers") {
	// **THIS IS THE PROPERTY ContainerOps IS SHAPED BY**, so it is checked by reading the include
	// graph rather than trusted. A single convenient `#include "plugin/device_instance_pool.h"` in
	// this directory would compile, work, and quietly end the property that lets params.h stop
	// including a device header — and the next person would reasonably hand a container a
	// `DeviceInstancePool&` because the include was already there.
	//
	// MODELLED ON `fx::filter`'s "the module includes nothing from the plugin", including its
	// non-vacuity guard: a walk that finds no files passes every assertion it never made.
	const std::filesystem::path root(FEATHERS_DEVICE_SOURCE_DIR);
	REQUIRE(std::filesystem::exists(root));

	int filesRead = 0;
	for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
		if (!entry.is_regular_file()) {
			continue;
		}
		const auto ext = entry.path().extension().string();
		if (ext != ".h" && ext != ".cpp") {
			continue;
		}
		filesRead++;
		CAPTURE(entry.path().string());

		std::ifstream in(entry.path());
		REQUIRE(in.good());
		std::string line;
		int lineNumber = 0;
		while (std::getline(in, line)) {
			lineNumber++;
			if (line.rfind("#include", 0) != 0) {
				continue;
			}
			CAPTURE(lineNumber);
			CAPTURE(line);
			CHECK(line.find("plugin/") == std::string::npos);
			CHECK(line.find("backplane/") == std::string::npos);
		}
	}
	// If the walk found nothing, the checks above passed by not running.
	CHECK(filesRead >= 3);
}

TEST_CASE("the device SDK includes only itself and the standard library, so its MIT licence holds") {
	// **THE SDK IS MIT AND FEATHERS IS GPL**. The SDK's own
	// CMake targets see only its include/ directory, so a reach into `src/` fails THEIR compile — but
	// only for headers the example happens to include. This walk covers every file: a quoted include
	// must name a file that exists inside the SDK's include/, and nothing else. Angle includes are the
	// standard library's; the SDK has no third-party dependency to name that way.
	const std::filesystem::path root(FEATHERS_DEVICE_SDK_DIR);
	const std::filesystem::path includeRoot = root / "include";
	REQUIRE(std::filesystem::exists(includeRoot));

	int filesRead = 0;
	int quotedIncludes = 0;
	for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
		if (!entry.is_regular_file()) {
			continue;
		}
		const auto ext = entry.path().extension().string();
		if (ext != ".h" && ext != ".cpp") {
			continue;
		}
		filesRead++;
		CAPTURE(entry.path().string());

		std::ifstream in(entry.path());
		REQUIRE(in.good());
		std::string line;
		int lineNumber = 0;
		bool sawMit = false;
		while (std::getline(in, line)) {
			lineNumber++;
			if (line.find("SPDX-License-Identifier: MIT") != std::string::npos) {
				sawMit = true;
			}
			// PUBLIC TEXT: no person is named in it (the SDK states its rules in its own voice).
			CHECK(line.find(std::string("To") + "ny") == std::string::npos); // assembled, so this file does not trip its own check
			if (line.rfind("#include", 0) != 0) {
				continue;
			}
			CAPTURE(lineNumber);
			CAPTURE(line);
			const auto open = line.find('"');
			if (open == std::string::npos) {
				continue; // <...>: the standard library
			}
			const auto close = line.find('"', open + 1);
			REQUIRE(close != std::string::npos);
			quotedIncludes++;
			// Inside the SDK: under include/, or beside the including file (the example includes its own header).
			const std::string named = line.substr(open + 1, close - open - 1);
			CHECK(named.find("..") == std::string::npos); // and never by climbing out of it
			CHECK((std::filesystem::exists(includeRoot / named) || std::filesystem::exists(entry.path().parent_path() / named)));
		}
		CHECK(sawMit);
	}
	CHECK(filesRead >= 10);
	CHECK(quotedIncludes >= 3); // non-vacuity: the SDK's headers do include one another
}

TEST_CASE("the one thing a host hands a device that holds devices is its catalogue, callable through "
          "the vocabulary alone") {
	// **A DEVICE THAT HOLDS DEVICES MAY BE BUILT SEPARATELY FROM WHATEVER HOSTS IT** — a third party's
	// binary, or ours — so it cannot have a device list compiled in. The host hands it one. This case is
	// that hand-off, driven through `device::Catalogue` from a host this header has never heard of: if
	// the vocabulary grew a dependency on a host type, it would not compile.
	struct ForeignHost {
		int asked = 0;
	};
	ForeignHost host;
	const device::Catalogue catalogue {
		&host,
		[](void* h, const char* tag) -> const device::Descriptor* {
			static_cast<ForeignHost*>(h)->asked++;
			return deviceForTag(tag);
		},
	};

	const device::Descriptor* filt = catalogue("flt1");
	REQUIRE(filt != nullptr);
	CHECK(device::sameTag(filt->traits.tag, "flt1"));
	// NULL IS A REAL ANSWER — a tag this host has never heard of, which is what a saved chain naming a
	// device this build lacks meets.
	CHECK(catalogue("zzzz") == nullptr);
	CHECK(host.asked == 2);

	// AND AN EMPTY CATALOGUE ANSWERS NOTHING RATHER THAN CRASHING — what a device holding devices has
	// between being built and being adopted.
	const device::Catalogue none {};
	CHECK(none("flt1") == nullptr);
}

TEST_CASE("exactly four devices in the tree hold others, and this case is the register of which") {
	// **THIS CASE HAS NOW BEEN RIGHT TWICE ABOUT SOMETHING NOBODY ELSE WOULD HAVE SAID.** It first read
	// "no device in the tree says it yet" — the guard that the capability existed before anything
	// claimed it. The chain claimed it, and it became "exactly one, and which", with the note that *"a
	// second container appearing would be a design event worth a line in the plan, and this is what
	// would notice it."* SKLITTER became the second (backlog EW, its post-effects lane), and this is
	// the case that went red to say so. The list is spelled out rather than counted, because the COUNT
	// is not the interesting fact — WHICH devices hold, and why each one does, is.
	//
	//   composite (`chn `) — a chain IS a container; holding is its whole identity.
	//   SKLITTER  (`sklt`) — an ordinary effect that ALSO holds, which is the shape this file's last
	//                        paragraph predicted the four independent pointers would one day express.
	//
	//   RESONATE (`RESONATE`) — the THIRD (2026-10-05, Resonate takes fx): two fx lanes, DRONE FX and WET
	//                        FX, on `rack::laneContainerOps` — the post-fx pattern lifted out of SKLITTER
	//                        (backlog LC), which the next holder (Featherverb) is meant to take as well.
	//
	//   RACK (`RACK`) — the FOURTH (backlog NO, 2026-10-18): parallel chains, as many as a hand adds — the first
	//                        holder whose number of chains is not fixed by its declaration.
	//
	//   DELAY (`DELAY`) — the FIFTH (delay-love, 2026-11-04): one FEEDBACK FX lane INSIDE the loop, built lazily —
	//                        the first holder whose chain is not in series with its output but in its own feedback.
	//
	//   COMP (`comp`) — the SIXTH (backlog QG, 2026-11-07): the Compander's encode/decode lane BETWEEN its two halves,
	//                        the Delay's lazy-lane component (`rack::LazyLane`), run only in Compander mode.
	//
	// A SEVENTH would be the next design event, and this is still what notices it.
	std::vector<std::string_view> holders;
	for (const device::Descriptor& d : kDevices) {
		if (d.container != nullptr) {
			holders.emplace_back(d.traits.tag);
		}
	}
	REQUIRE(holders.size() == 6);
	CHECK(std::find(holders.begin(), holders.end(), std::string_view(fx::comp::kTraits.tag)) != holders.end());
	CHECK(std::find(holders.begin(), holders.end(), std::string_view(fx::delay::kTraits.tag)) != holders.end());
	CHECK(std::find(holders.begin(), holders.end(), std::string_view(rack::parallel::kTraits.tag)) != holders.end());
	CHECK(std::find(holders.begin(), holders.end(), std::string_view(fx::resonate::kTraits.tag)) != holders.end());
	CHECK(std::find(holders.begin(), holders.end(), std::string_view(composite::kTraits.tag))
	      != holders.end());
	CHECK(std::find(holders.begin(), holders.end(), std::string_view(fx::sklitter::kTraits.tag))
	      != holders.end());

	// EVERY HOLDER ALSO RENDERS — it holds AND is an audio device. A `kind` enum could not have said
	// both, which is the reason `Descriptor` has four independent pointers rather than one field.
	for (const device::Descriptor& d : kDevices) {
		if (d.container != nullptr) {
			CHECK(d.audio != nullptr);
		}
	}

	// **AND ONE OF THEM DECLARES ALL FOUR ROADS AT ONCE, which is the strongest version of the same
	// argument.** SKLITTER renders, modulates, publishes sources AND holds. The composite declares two.
	// If the four pointers were one field, the pair below could not both exist.
	CHECK(fx::sklitter::kDescriptor.audio != nullptr);
	CHECK(fx::sklitter::kDescriptor.mod != nullptr);
	CHECK(!fx::sklitter::kDescriptor.outputs.empty());
	CHECK(fx::sklitter::kDescriptor.container != nullptr);
	CHECK(composite::kTraits.tag != std::string_view(fx::sklitter::kTraits.tag));

	// THE FOUR CAPABILITIES ARE INDEPENDENT — asserted on the struct rather than argued in a comment.
	device::Descriptor probe {};
	CHECK(probe.audio == nullptr);
	CHECK(probe.mod == nullptr);
	CHECK(probe.notes == nullptr);
	CHECK(probe.container == nullptr);
	static device::ContainerOps ops {};
	probe.container = &ops;
	CHECK(probe.container != nullptr);
	CHECK(probe.audio == nullptr); // declaring one said nothing about the others
}


//------------------------------------------------------------------------------------//
// A CONTAINER'S BODY TRAVELS AS BYTES — step 6c, the vocabulary half.                   //
//------------------------------------------------------------------------------------//

namespace {

/// A CONTAINER THAT IS NOT A CHAIN, holding the least a container can hold: some members, each with
/// an id and a tag. It exists to exercise `ContainerOps` through its declared entry points and
/// nothing else — a real chain arrives in a later commit and will be a different object entirely.
///
/// WHY A FAKE AT ALL, when the real thing is coming. The claim these cases make is about the
/// VOCABULARY: that a body can be sized, written, read back and validated without the host parsing
/// a byte of it. A real chain would prove that too, and would also drag in a pool, a counter and a
/// graph — so a failure would have four possible homes instead of one.
struct FakeContainer {
	struct Member {
		uint32_t id = 0;
		std::array<char, 4> tag {};
	};
	std::vector<Member> members;

	static constexpr uint32_t kMagic = 0x434e5231; // "CNR1"
};

bool fakeSave(const void* storage, const device::ByteSink& sink) {
	const auto& c = *static_cast<const FakeContainer*>(storage);
	const auto put = [&sink](const void* p, size_t n) { sink(static_cast<const uint8_t*>(p), n); };
	const uint32_t magic = FakeContainer::kMagic;
	put(&magic, sizeof(magic));
	const auto count = static_cast<uint32_t>(c.members.size());
	put(&count, sizeof(count));
	for (const auto& m : c.members) {
		put(&m.id, sizeof(m.id));
		put(m.tag.data(), 4);
	}
	return true;
}

/// A BODY, SAVED ONCE INTO A VECTOR — the host's side of `ContainerOps::saveState`.
std::vector<uint8_t> saved(const device::ContainerOps& ops, const void* storage) {
	std::vector<uint8_t> out;
	const device::ByteSink sink { &out, [](void* ctx, const uint8_t* d, size_t n) {
		                             auto* v = static_cast<std::vector<uint8_t>*>(ctx);
		                             v->insert(v->end(), d, d + n);
	                             } };
	REQUIRE(ops.saveState(storage, sink));
	return out;
}

bool fakeLoad(void* storage, const uint8_t* data, size_t bytes) {
	auto& c = *static_cast<FakeContainer*>(storage);
	if (data == nullptr || bytes < sizeof(uint32_t) * 2) {
		return false;
	}
	size_t at = 0;
	uint32_t magic = 0;
	std::memcpy(&magic, data + at, sizeof(magic));
	at += sizeof(magic);
	if (magic != FakeContainer::kMagic) {
		return false;
	}
	uint32_t count = 0;
	std::memcpy(&count, data + at, sizeof(count));
	at += sizeof(count);
	// **THE WHOLENESS CHECK, which is the thing only a container can do.** The host handed these
	// bytes across without parsing them and cannot tell a truncated body from a short one.
	if (bytes - at != static_cast<size_t>(count) * (sizeof(uint32_t) + 4)) {
		return false;
	}
	c.members.clear();
	for (uint32_t i = 0; i < count; ++i) {
		FakeContainer::Member m;
		std::memcpy(&m.id, data + at, sizeof(m.id));
		at += sizeof(m.id);
		std::memcpy(m.tag.data(), data + at, 4);
		at += 4;
		c.members.push_back(m);
	}
	return true;
}

constexpr device::ContainerOps kFakeContainerOps {
	.adopt = nullptr,
	.saveState = &fakeSave,
	.loadState = &fakeLoad,
};

} // namespace

TEST_CASE("a container's body round-trips as bytes the host never parses, and the VALUES come back") {
	FakeContainer source;
	source.members.push_back({ 41, { 'f', 'i', 'l', 't' } });
	source.members.push_back({ 42, { 'd', 'e', 'l', 'y' } });
	source.members.push_back({ 43, { 'v', 'e', 'r', 'b' } });

	// **ONE WRITE, INTO A SINK THE HOST OWNS.** The first contract asked for the size and then for the
	// bytes, which made a rack inside a rack serialise its child twice per level — 2^depth.
	const std::vector<uint8_t> body = saved(kFakeContainerOps, &source);
	CHECK(body.size() == 4 + 4 + 3 * 8);

	// **THE VALUES, NOT "IT LOADED".** CLAUDE.md's rule, applied to a state road rather than a
	// buffer: a round trip that only asserts `loadState` returned true passes for a container that
	// read the magic and threw the rest away.
	FakeContainer restored;
	restored.members.push_back({ 999, { 'x', 'x', 'x', 'x' } }); // must be REPLACED, not appended to
	REQUIRE(kFakeContainerOps.loadState(&restored, body.data(), body.size()));

	REQUIRE(restored.members.size() == 3);
	CHECK(restored.members[0].id == 41);
	CHECK(restored.members[1].id == 42);
	CHECK(restored.members[2].id == 43);
	CHECK(std::string_view(restored.members[0].tag.data(), 4) == "filt");
	CHECK(std::string_view(restored.members[1].tag.data(), 4) == "dely");
	CHECK(std::string_view(restored.members[2].tag.data(), 4) == "verb");
}

TEST_CASE("a container validates its own bytes, because the host that carried them cannot") {
	FakeContainer source;
	source.members.push_back({ 7, { 'h', 'a', 'r', 'm' } });
	const std::vector<uint8_t> body = saved(kFakeContainerOps, &source);

	FakeContainer sink;

	// A TRUNCATED BODY IS REFUSED RATHER THAN READ SHORT. This is the failure the entry point exists
	// to prevent: the host length-prefixes the payload but has no idea what a WHOLE body looks like,
	// so a body cut in half arrives as a perfectly well-formed short payload.
	CHECK(kFakeContainerOps.loadState(&sink, body.data(), body.size() - 1) == false);
	CHECK(sink.members.empty()); // and a refusal leaves the container alone

	// BYTES FROM SOMETHING ELSE ENTIRELY are refused too — an unknown tag's payload, or a body
	// written by a container this build does not have.
	std::vector<uint8_t> foreign(body.size(), 0x5A);
	CHECK(kFakeContainerOps.loadState(&sink, foreign.data(), foreign.size()) == false);
	CHECK(sink.members.empty());

	// AND NOTHING AT ALL, which is what a record with no body hands over.
	CHECK(kFakeContainerOps.loadState(&sink, nullptr, 0) == false);

	// THE WHOLE BODY IS ACCEPTED, which is the assertion proving the three above are not vacuous —
	// a `loadState` that returned false unconditionally would pass every one of them.
	CHECK(kFakeContainerOps.loadState(&sink, body.data(), body.size()));
	REQUIRE(sink.members.size() == 1);
	CHECK(sink.members[0].id == 7);
}

TEST_CASE("an empty container has an empty body, and that is a real answer rather than a failure") {
	FakeContainer empty;
	const std::vector<uint8_t> body = saved(kFakeContainerOps, &empty);
	CHECK(body.size() == 8); // magic and a zero count, and no members

	FakeContainer sink;
	sink.members.push_back({ 1, { 'a', 'a', 'a', 'a' } });
	REQUIRE(kFakeContainerOps.loadState(&sink, body.data(), body.size()));
	// AN EMPTY CHAIN LOADS AS EMPTY rather than leaving whatever was there — the distinction
	// device_row.h keeps making, between "carried nothing" and "was not carried".
	CHECK(sink.members.empty());
}
