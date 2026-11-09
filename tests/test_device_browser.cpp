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

#include "plugin/device_registry.h"
#include "rack/device_browser.h"
#include "device/device_groups.h"

#include <array>
#include <cstring>
#include <string>

using namespace feathers;

namespace {

size_t totalEntries(const std::vector<rack::BrowserGroup>& groups) {
	size_t n = 0;
	for (const auto& g : groups) {
		n += g.entries.size();
	}
	return n;
}

bool lists(const std::vector<rack::BrowserGroup>& groups, const char* tag) {
	for (const auto& g : groups) {
		for (const auto& e : g.entries) {
			if (std::strcmp(e.deviceTag, tag) == 0) {
				return true;
			}
		}
	}
	return false;
}

} // namespace

TEST_CASE("every device this build can run is offered exactly once") {
	const auto groups = rack::browseDevices(kDevices);
	size_t runnable = 0;
	for (const device::Descriptor& d : kDevices) {
		if (d.audio != nullptr || d.mod != nullptr) {
			++runnable;
		}
	}
	REQUIRE(runnable > 0);
	REQUIRE(totalEntries(groups) == runnable);

	// EXACTLY ONCE, which is what grouping by the FIRST tag buys. The MSEG declares three tags; listing
	// it under all of them would make picking the same device twice look like picking two.
	for (const device::Descriptor& d : kDevices) {
		if (d.audio == nullptr && d.mod == nullptr) {
			continue;
		}
		size_t seen = 0;
		for (const auto& g : groups) {
			for (const auto& e : g.entries) {
				if (std::strcmp(e.deviceTag, d.traits.tag) == 0) {
					++seen;
				}
			}
		}
		CAPTURE(d.traits.name);
		REQUIRE(seen == 1);
	}
}

TEST_CASE("a device the rack cannot run is not offered") {
	// THE HONESTY RULE APPLIED TO A LIST. An entry that cannot be added is a control offering
	// something it cannot do, and it fails in the worst way: the click looks like it worked and
	// nothing appears. `feathers_clap.cpp` makes the same refusal in its own enumeration.
	const std::array<device::Descriptor, 1> inert { device::Descriptor {
		.traits = device::Traits { "ZZZZ", "Inert", false, false },
		.params = {},
		.outputs = {},
	} };
	REQUIRE(inert[0].audio == nullptr);
	REQUIRE(inert[0].mod == nullptr);
	REQUIRE(rack::browseDevices(inert).empty());
}

TEST_CASE("modulators are offered too, and grouped away from the effects") {
	const auto groups = rack::browseDevices(kDevices);
	REQUIRE(lists(groups, mod::dahdsr::kTraits.tag));
	REQUIRE(lists(groups, mod::mseg::kTraits.tag));

	// The envelope and the filter must not land in the same group — a browser that put them together
	// would be grouping by nothing.
	std::string envGroup, filterGroup;
	for (const auto& g : groups) {
		for (const auto& e : g.entries) {
			if (std::strcmp(e.deviceTag, mod::dahdsr::kTraits.tag) == 0) {
				envGroup = g.name;
			}
			if (std::strcmp(e.deviceTag, fx::filter::kTraits.tag) == 0) {
				filterGroup = g.name;
			}
		}
	}
	REQUIRE_FALSE(envGroup.empty());
	REQUIRE_FALSE(filterGroup.empty());
	REQUIRE(envGroup != filterGroup);
}

TEST_CASE("a device that declares no category lands in Other rather than vanishing") {
	static const char* const kNames[] = { "x" };
	const std::array<device::Descriptor, 1> untagged { device::Descriptor {
		.traits = device::Traits { "UNTG", "Untagged", false, false },
		.params = {},
		.outputs = {},
		.audio = fx::filter::kDescriptor.audio, // borrowed only so it counts as runnable
	} };
	(void)kNames;
	const auto groups = rack::browseDevices(untagged);
	REQUIRE(groups.size() == 1);
	REQUIRE(groups[0].name == "Other");
	REQUIRE(groups[0].entries.size() == 1);
}

TEST_CASE("every offered entry names a device the registry can actually resolve") {
	// A row whose tag resolved to nothing would be a row that adds nothing when picked — the same
	// failure as offering an unrunnable device, arriving through a typo instead of a declaration.
	for (const auto& g : rack::browseDevices(kDevices)) {
		for (const auto& e : g.entries) {
			CAPTURE(e.name);
			REQUIRE(e.deviceTag != nullptr);
			REQUIRE(deviceForTag(e.deviceTag) != nullptr);
		}
	}
}

TEST_CASE("the browser's groups are the categories, in category order, devices alphabetical") {
	// THE SAME TAXONOMY AS EVERY POPUP ADD MENU, as values: the rack can run every registered device,
	// so every heading is here, in order.
	const auto groups = rack::browseDevices(kDevices);
	std::vector<std::string> names;
	for (const auto& g : groups) {
		names.push_back(g.name);
	}
	CHECK(names
	      == std::vector<std::string> { "Filter & EQ", "Dynamics", "Drive & Colour", "Time & Space", "Pitch",
	                                    "Rhythm & Glitch", "Modulation FX", "Utility", "Modulators", "Note FX", "Containers" });
	for (const auto& g : groups) {
		if (g.name == "Time & Space") {
			REQUIRE(g.entries.size() == 2);
			CHECK(std::string(g.entries[0].name) == "Delay");
			CHECK(std::string(g.entries[1].name) == "Featherverb");
		}
	}
}

//------------------------------------------------------------------------------------------------//
// CATEGORIES — the first level of every add menu.                                              //
//------------------------------------------------------------------------------------------------//

TEST_CASE("every registered device files under the category it was assigned") {
	// THE TAXONOMY AS DECIDED, written out by NAME — a value table, not "has some category".
	// A device filed under the wrong heading fails here naming itself; a device registered with none
	// cannot reach this line (device_registry.h's static_assert), and a new device not in this table
	// fails the count check below until somebody decides on it.
	struct Ruled {
		const char* name;
		device::Category category;
	};
	static constexpr Ruled kRuled[] = {
		{ "Filter", device::Category::FilterEq },       { "Drawn", device::Category::FilterEq },
		{ "OWLTT", device::Category::Dynamics },        { "Shaper", device::Category::DriveColour },
		{ "Eroder", device::Category::DriveColour },    { "Delay", device::Category::TimeSpace },
		{ "Featherverb", device::Category::TimeSpace }, { "Pitch", device::Category::Pitch },
		{ "SKLITTER", device::Category::RhythmGlitch }, { "Automod", device::Category::ModulationFx },
		{ "Waypoint", device::Category::FilterEq }, { "Bus Utility", device::Category::Utility },
		{ "Harm", device::Category::Utility },          { "Chain", device::Category::Containers },
		{ "Rack", device::Category::Containers },
		// BACKLOG NW (2026-10-26): Sidechain Inject is a utility, it routes a signal into the chain and does not shape one.
		{ "Sidechain Inject", device::Category::Utility },
		{ "LFO", device::Category::Modulators },        { "MSEG", device::Category::Modulators },
		{ "DAHDSR", device::Category::Modulators },     { "Tracker", device::Category::Modulators },
		{ "Note Expressions", device::Category::Modulators },
		// Decided 2026-09-25: COMP under Dynamics, CLIP under Drive & Colour.
		{ "Compressor", device::Category::Dynamics },   { "Clipper", device::Category::DriveColour },
		// MACRO MODULATION (2026-09-25, at the macro-mod merge): both are
		// modulators — MACROS is four hand-set sources, MATH combines two.
		{ "Macros", device::Category::Modulators },     { "Math", device::Category::Modulators },
		// THE SMOOTH SHAPER: beside the Shaper under Drive & Colour — confirmed.
		{ "Smooth Shaper", device::Category::DriveColour },
		{ "EQ", device::Category::FilterEq },
		// RESONATE (2026-09-25, PROPOSED, not yet confirmed): a resonator is a
		// filter — the input convolved with the string's shape — beside Filter, Drawn and Waypoint.
		{ "Resonate", device::Category::FilterEq },
		// THE DISPERSER (2026-09-25): an allpass cascade is a phaser without its LFO, and the phaser's
		// family is Modulation FX. The implementer's pick, KEPT.
		{ "Disperser", device::Category::ModulationFx },
		// THE PHASER/FLANGER (2026-10-03): the category the approved plan names. Its name is a working title
		// (`phaser_types.h`'s `kDeviceName`), read from there.
		{ fx::phaser::kDeviceName, device::Category::ModulationFx },
		// THE STEREO TOOL (backlog KU, 2026-10-04): a modulator — the design names it one, beside Math.
		{ "Stereo Tool", device::Category::Modulators },
		{ "Twin Field", device::Category::Modulators },
		{ "Spectral", device::Category::FilterEq },
		// NOISE (backlog MZ, 2026-10-17): beside Harm, the other "one device for a named job" that adds to what passes.
		// The implementer's pick, not yet confirmed.
		{ "Noise", device::Category::Utility },
		// RANDOM TRIGGER (backlog NA (a), 2026-10-17): a modulator, NOISE's impulses as events.
		{ "Random Trigger", device::Category::Modulators },
		// SAMPLE & HOLD (backlog MW, 2026-10-17): a modulator, beside the LFO whose Time Base it shares.
		{ "Sample & Hold", device::Category::Modulators },
		// THE STEP SEQUENCER (backlog MX, 2026-10-17): a modulator, beside the LFO and the MSEG.
		{ "Step Seq", device::Category::Modulators },
		// THE FOLLOWER (backlog MY, 2026-10-17): a modulator, as the backlog files it (MU's table stakes).
		{ "Follower", device::Category::Modulators },
		// THE ARP AND PATTERN (2026-11-04): Note FX — a 'note' category, so Pattern and Arp do not ride along with the modulators.
		{ "Arp", device::Category::Notes },
		{ "Pattern", device::Category::Notes },
	};
	size_t matched = 0;
	for (const device::Descriptor& d : kDevices) {
		const std::string name = d.traits.name;
		CAPTURE(name);
		CHECK(d.category != device::Category::None);
		const Ruled* ruled = nullptr;
		for (const Ruled& r : kRuled) {
			if (name == r.name) {
				ruled = &r;
			}
		}
		REQUIRE_MESSAGE(ruled != nullptr, "a registered device the taxonomy has no category for");
		CHECK(std::string(device::categoryName(d.category)) == std::string(device::categoryName(ruled->category)));
		++matched;
	}
	CHECK(matched == std::size(kRuled));
}

TEST_CASE("groupByCategory files every offered device exactly once, headings in order, names alphabetical") {
	std::vector<const device::Descriptor*> offered;
	for (const device::Descriptor& d : kDevices) {
		offered.push_back(&d);
	}
	const std::vector<device::CategoryGroup> groups = device::groupByCategory(offered);

	// EVERY INDEX ONCE: none missing, none twice.
	std::vector<int> seen(offered.size(), 0);
	for (const auto& g : groups) {
		for (const size_t i : g.members) {
			REQUIRE(i < offered.size());
			++seen[i];
			// AND UNDER ITS OWN HEADING.
			CHECK(offered[i]->category == g.category);
		}
	}
	for (size_t i = 0; i < seen.size(); ++i) {
		CAPTURE(offered[i]->traits.name);
		CHECK(seen[i] == 1);
	}
	// HEADINGS IN DECLARATION ORDER, each once, none empty — and every one the registry uses is here.
	const std::vector<std::string> expected = { "Filter & EQ",     "Dynamics",      "Drive & Colour",
		                                        "Time & Space",    "Pitch",         "Rhythm & Glitch",
		                                        "Modulation FX",   "Utility",       "Modulators",      "Note FX",
		                                        "Containers" };
	std::vector<std::string> headings;
	for (const auto& g : groups) {
		headings.emplace_back(device::categoryName(g.category));
		CHECK_FALSE(g.members.empty());
	}
	CHECK(headings == expected);
	// NAMES INSIDE A HEADING, as read: the two with more than two members, spelled out.
	const auto namesOf = [&](device::Category c) {
		std::vector<std::string> out;
		for (const auto& g : groups) {
			if (g.category == c) {
				for (const size_t i : g.members) {
					out.emplace_back(offered[i]->traits.name);
				}
			}
		}
		return out;
	};
	CHECK(namesOf(device::Category::Modulators)
	      == std::vector<std::string> { "DAHDSR", "Follower", "LFO", "Macros", "Math", "MSEG", "Note Expressions", "Random Trigger", "Sample & Hold", "Step Seq", "Stereo Tool", "Tracker", "Twin Field" });
	CHECK(namesOf(device::Category::Notes) == std::vector<std::string> { "Arp", "Pattern" });
	CHECK(namesOf(device::Category::Utility) == std::vector<std::string> { "Bus Utility", "Harm", "Noise", "Sidechain Inject" });
	CHECK(namesOf(device::Category::FilterEq) == std::vector<std::string> { "Drawn", "EQ", "Filter", "Resonate", "Spectral", "Waypoint" });
}

TEST_CASE("groupByCategory omits a category nothing in the offer belongs to, and files a guest's None last") {
	const device::Descriptor guest {
		.traits = device::Traits { "GST1", "Guest", false, false },
		.params = {},
		.outputs = {},
		.audio = fx::filter::kDescriptor.audio,
	};
	// A LANE THAT OFFERS THREE DEVICES FROM TWO CATEGORIES, plus one that declared nothing.
	const std::vector<const device::Descriptor*> offered = { &fx::delay::kDescriptor, &guest,
		                                                     &fx::filter::kDescriptor, &fx::drawn::kDescriptor };
	const auto groups = device::groupByCategory(offered);
	REQUIRE(groups.size() == 3);
	CHECK(groups[0].category == device::Category::FilterEq);
	CHECK(groups[0].members == std::vector<size_t> { 3, 2 }); // Drawn, Filter
	CHECK(groups[1].category == device::Category::TimeSpace);
	CHECK(groups[1].members == std::vector<size_t> { 0 });
	CHECK(groups[2].category == device::Category::None);
	CHECK(std::string(device::categoryName(groups[2].category)) == "Other");
	CHECK(groups[2].members == std::vector<size_t> { 1 });

	CHECK(device::groupByCategory(std::vector<const device::Descriptor*> {}).empty());
}

TEST_CASE("kDevices keeps the order the registry had when devices began to register themselves") {
	// Backlog NY.1: kDevices is generated from every device's own `device.cmake` (ORDER, then the
	// descriptor's text). This pins the sequence of the devices that existed on 2026-10-19, so a
	// manifest edit that reshuffles them fails HERE and not in the browser's grouping. A new device
	// takes any ORDER above these; it is not added to this list, which is a pin and not an enumeration
	// (every other test walks kDevices itself).
	static constexpr const char* kPinned[] = {
		"flt1", "HARM", "OWLTT", "SKLITTER", "wayp", "drwn", "SHPR", "AMOD", "DHDS", "MSEG", "TRAK",
		"NEXP", "busu", "cmps", "LFO", "DELAY", "PITCH", "ERODER", "FVERB", "comp", "clip", "RESONATE",
		"MACROS", "MATH", "smth", "eq", "DISPERSE", "PHFL", "STEREO", "spectral", "noise", "rtrig",
		"SNH", "STEPSEQ", "FOLLOWER", "RACK",
	};
	REQUIRE(kDeviceCount >= std::size(kPinned));
	for (size_t i = 0; i < std::size(kPinned); ++i) {
		INFO("position " << i);
		CHECK(device::sameTag(kDevices[i].traits.tag, kPinned[i]));
	}
}
