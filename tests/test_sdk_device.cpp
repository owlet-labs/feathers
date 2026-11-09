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

// **A DEVICE BUILT AGAINST THE DEVICE SDK ALONE, REGISTERED AND RUN BY THE HOST** — the SDK's stage 2 claim, made
// by value. `libs/feathers-device-sdk/example/sine_mod.h` includes nothing of Feathers': this case hands its
// `Descriptor` to the rack through a catalogue, routes its one output to a filter's Cutoff, and reads both the
// value the modulator published and the value the Cutoff resolved to — each against an independent restatement:
// the sine from its own definition, the resolve as `base + amount × output × span` (CLAUDE.md, "assert the value").
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "fx/filter/filter_types.h"
#include "plugin/device_registry.h"
#include "rack/rack_engine.h"
#include "rack_endpoints.h"
#include "sine_mod.h" // the SDK's example, from libs/feathers-device-sdk/example

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <vector>

using namespace feathers;
namespace filter = feathers::fx::filter;

namespace {

constexpr backplane::Axes kAxes {
	.coreSources = 8,
	.coreDestinations = 0,
	.deviceSlots = 4,
	.outputStride = 4,
};

/// THE HOST'S CATALOGUE, PLUS ONE GUEST — what a loader would hand the rack after finding the example's module.
const device::Descriptor* findWithGuest(void*, const char* tag) {
	if (std::strcmp(tag, sine_mod::kTraits.tag) == 0) {
		return &sine_mod::kDescriptor;
	}
	return hostCatalogue()(tag);
}

} // namespace

TEST_CASE("a device built against the SDK alone registers, runs as a modulator, and moves a routed control by value") {
	REQUIRE(hostCatalogue()(sine_mod::kTraits.tag) == nullptr); // a guest: Feathers' own table does not know it
	rack::RackEngine rack(kAxes, device::Catalogue { nullptr, findWithGuest });
	constexpr double kRate = 48000.0;
	rack.prepare(kRate, 4096);
	const size_t fx = rack.addDevice(filter::kTraits.tag);
	const size_t sine = rack.addDevice(sine_mod::kTraits.tag);
	REQUIRE(fx != rack::RackEngine::kNoSlot);
	REQUIRE(sine != rack::RackEngine::kNoSlot);

	rack.setField(sine, sine_mod::kRate, 0.0f); // log2(1 Hz)
	rack.setField(sine, sine_mod::kDepth, 0.8f);

	const size_t cutoff = static_cast<size_t>(filter::Param::Cutoff);
	REQUIRE(cutoff < filter::kDescriptor.params.size());
	const device::ParamDesc& p = filter::kDescriptor.params[cutoff];
	const backplane::Destination dest = test::controlOf(rack, fx, cutoff);
	constexpr float kAmount = 0.25f;
	rack.setRouteAmount(test::outputOf(rack, sine, 0), dest, kAmount);
	const double base = rack.fieldAt(fx, cutoff);

	std::vector<float> l(sine_mod::kBlockSamples, 0.0f), r(sine_mod::kBlockSamples, 0.0f);
	int moved = 0;
	for (int block = 0; block < 400; ++block) {
		rack.processStereo(l.data(), r.data(), static_cast<int>(sine_mod::kBlockSamples), backplane::unknownBlock());
		const float published = rack.publishedOutputAt(sine, 0);

		// THE SINE, FROM ITS DEFINITION: the block-th tick publishes where `block` ticks of 64 samples at 1 Hz put it.
		const double phase = std::fmod(static_cast<double>(block) * sine_mod::kBlockSamples / kRate, 1.0);
		const double sineExpected = 0.8 * std::sin(2.0 * std::numbers::pi * phase);
		CAPTURE(block);
		REQUIRE(std::fabs(static_cast<double>(published) - sineExpected) < 1e-5);

		// THE RESOLVE, RESTATED: base + amount × output × span, inside the control's range unless it may leave it.
		double expected = base + static_cast<double>(kAmount) * static_cast<double>(published) * device::routeSpanOf(p);
		if (!p.offsetsMayLeaveRange) {
			expected = std::clamp(expected, p.min, p.max);
		}
		const double resolved = rack.resolvedFieldForTest(fx, cutoff);
		REQUIRE(std::fabs(resolved - expected) < 1e-4 * std::max(1.0, std::fabs(expected)));
		moved += std::fabs(resolved - base) > 1e-3 ? 1 : 0;
	}
	// NOT VACUOUS: the route moved the control on most blocks (the sine crosses zero twice a second).
	CHECK(moved > 300);
	CHECK(rack.fieldAt(fx, cutoff) == doctest::Approx(base)); // and never wrote the base
}
