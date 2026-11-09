/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "plugin/device_registry.h"

#include <algorithm>
#include <cctype>
#include <string>

TEST_CASE("every registered device control has a tooltip") {
	// A control with no tooltip draws its name on hover — which is the fallback, not a promise.
	// A control whose tooltip is nullptr is a control that says nothing at all. This walks the
	// entire registry and fails on the first one it finds, naming the device and control so the
	// fix is one string, not a hunt. Backlog EX: 89 controls shipped without one.
	for (const auto& d : feathers::kDevices) {
		for (size_t p = 0; p < d.params.size(); ++p) {
			const auto& desc = d.params[p];
			CAPTURE(d.traits.tag);
			CAPTURE(p);
			CAPTURE(desc.name == nullptr ? "<unnamed>" : desc.name);
			CHECK(desc.tooltip != nullptr);
		}
	}

	// AND THE SWEEP SAW THE WHOLE REGISTRY, so a green is a statement about every device rather than
	// about an empty loop.
	CHECK(std::size(feathers::kDevices) >= 8);
}

namespace {

bool mentionsHardwareName(const char* text) {
	if (text == nullptr) {
		return false;
	}
	std::string lower(text);
	std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
	// The name is assembled rather than spelled so the source itself does not carry it: a repository-wide
	// search for the name then finds only licence notices, and this guard still finds it in any string.
	static const std::string kName = std::string("del") + "uge";
	return lower.find(kName) != std::string::npos;
}

} // namespace

TEST_CASE("no device's user-facing text names the hardware reference: tooltips, choice names and zone names") {
	// Ported devices keep their provenance in comments, not in what a user reads. A string a
	// user can see on hover or in a menu is checked here, in every device.
	size_t strings = 0;
	for (const auto& d : feathers::kDevices) {
		for (const auto& desc : d.params) {
			const std::string tag(d.traits.tag);
			const std::string control(desc.name == nullptr ? "<unnamed>" : desc.name);
			CAPTURE(tag);
			CAPTURE(control);
			for (const char* text : { desc.name, desc.tooltip, desc.shortName }) {
				INFO(std::string(text == nullptr ? "" : text));
				CHECK_FALSE(mentionsHardwareName(text));
				++strings;
			}
			for (const char* choice : desc.names) {
				INFO(std::string(choice));
				CHECK_FALSE(mentionsHardwareName(choice));
				++strings;
			}
			for (const auto& zone : desc.zoneNames) {
				INFO(std::string(zone.name));
				CHECK_FALSE(mentionsHardwareName(zone.name));
				CHECK_FALSE(mentionsHardwareName(zone.description));
				strings += 2;
			}
		}
	}
	// THE SWEEP READ THE REGISTRY, not an empty loop.
	CHECK(strings > 500);
}
