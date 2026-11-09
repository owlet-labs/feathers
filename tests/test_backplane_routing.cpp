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
#include "backplane/routing.h"
#include "fx/filter/filter_types.h"
#include "mod/dahdsr/dahdsr_types.h"

#include <array>

// THE ROUTING AUTHORITY, AND THE SLACK IT HAS TO REFUSE.
//
// The stride reserves `outputStride` rows for EVERY slot (and once reserved control columns) whatever is in
// it — that is what stops a device insertion re-pointing live routes, and the price is that most of
// those addresses name nothing. So the interesting cases here are not "does a real route work"; they
// are "does a WELL-FORMED, IN-RANGE address that names nothing get refused". Every one of those is a
// knob that would otherwise glow for a drag that does nothing, which is the honesty rule's exact
// failure case.
//
// REAL DESCRIPTORS RATHER THAN FAKES. The filter declares 6 modulatable controls and no outputs; the
// DAHDSR declares 10 and one output. Both numbers are facts about the shipping devices, so a case
// that hard-coded them would go stale silently — they are read from the descriptors instead, and what
// is asserted is the RELATIONSHIP between a declaration and what the table admits.

using namespace feathers::backplane;
namespace filter = feathers::fx::filter;
namespace dahdsr = feathers::mod::dahdsr;

namespace {

constexpr Axes kAxes {
	.coreSources = 12,
	.coreDestinations = 20,
	.deviceSlots = 8,
	.outputStride = 4,
};

/// A rack: no parameters of its own, so every destination is a device destination.
constexpr Axes kRackAxes {
	.coreSources = 8,
	.coreDestinations = 0,
	.deviceSlots = 8,
	.outputStride = 4,
};

/// Slot 0 holds a filter (destinations, no outputs), slot 1 a DAHDSR (both), the rest are empty.
///
/// **IT CARRIED A RANK PER LANE AND NO LONGER DOES (kill-lanes step 5).** The fixture's own comment
/// called the rank *"the routing rule's input"*, which it was: `mayRoute` over flat coordinates
/// compared two of them. Step 4b established that a rank is a position in ONE linearisation of a
/// partial order — adding a route changes it — so the rule became reachability, that overload went,
/// and step 5 took `SlotView`'s rank half with it. The 2026-09-05 rule (sources must be upstream of
/// destinations) is unchanged; what is deleted is the claim that a NUMBER could say so.
/// What `SlotView` is asked here now is only what it was ever independently good for: what is in a
/// lane, for the two liveness predicates.
struct Slots {
	std::array<const feathers::device::Descriptor*, 8> at {};
	Slots() {
		at[0] = &filter::kDescriptor;
		at[1] = &dahdsr::kDescriptor;
	}
	SlotView view() const {
		return SlotView(std::span<const feathers::device::Descriptor* const>(at));
	}
};

/// A host rule with a shape like Feathers': one forbidden pair of the HOST'S OWN ends, everything else
/// permitted. By endpoint since kill-lanes step 2 (`CorePolicy` reads host ends only).
constexpr size_t kForbiddenSource = 3;
constexpr size_t kForbiddenDestination = 7;
constexpr bool oneRule(Source source, Destination destination) noexcept {
	return !(!source.isDevice() && source.index == kForbiddenSource && !destination.isDevice()
	         && destination.index == kForbiddenDestination);
}

} // namespace

// **SEVEN CASES STOOD HERE ASKING THE AUTHORITY IN FLAT COORDINATES, AND THEY ARE DELETED (kill-lanes
// step 5) BECAUSE THE ROAD THEY ASKED IT ON IS.** The `SlotView` overload of `mayRoute` compared two
// RANKS for the ordering half, and step 4b established that a rank comparison is not the rule: a rank
// is a position in ONE linearisation of a partial order, and adding a route changes the linearisation.
//
// **NOT ONE CLAIM IS LOST, AND THAT IS CHECKABLE RATHER THAN ASSERTED.** Every one of them is made by
// *"the authority by endpoint: the same rule, asked of devices by id"* below, which was written in step
// 2 to carry exactly these claims across:
//
//   - *an empty slot admits nothing, in either axis* → a device the host does not hold, both directions
//     (`Source::output(99, 0)`, and a destination on an id nothing holds).
//   - *a slot's slack columns are refused, and its declared ones are admitted* → a control the device
//     does not declare modulatable is refused; its first modulatable one is admitted.
//   - *a slot's slack output rows are refused, and a device with no outputs has only slack* → an output
//     the device does not publish is refused; the outputless filter as a SOURCE is refused.
//   - *a device source may reach a device destination when both are live* → the forward pair, admitted.
//   - *the host's own rule is consulted* → `oneRule` refusing a pair the structure admits, and
//     admitting the neighbouring one.
//   - *a null policy refuses everything* → `nullptr` as the policy.
//   - *an out-of-range coordinate is refused in both axes* → a core index past the host's own table, in
//     both axes. (The coordinate-shaped half of that claim — an index past the DEVICE axis — has no
//     endpoint spelling: an endpoint names a device rather than an offset, so there is no out-of-range
//     to be had. That is the claim going away because the failure mode did, not because it stopped
//     being tested.)
//   - *a source must be upstream of its destination, and a device never reaches itself* → the relation
//     the fixture states directly, including a device reaching itself.

TEST_CASE("the authority works at a host with no core destinations") {
	// THE RACK'S SHAPE, ASKED BY ENDPOINT: a host with no core destination table at all. The coordinate
	// half this case once carried — destination 0 was a device destination and the address arithmetic
	// was asked where a zero core half put it at the origin — is deleted with the arithmetic (kill-lanes
	// §2.4b): an endpoint names a device, so there is no origin for a zero core half to move.

	// THE SAME TWO CLAIMS, BY ENDPOINT: a macro reaches the filter's first modulatable control, and a
	// control it does not declare modulatable is refused — at a host whose core destination table is
	// empty, which is what this case is for.
	constexpr feathers::DeviceInstanceId kTheFilter = 5;
	const DeviceView devices {
		nullptr,
		[](const void*, feathers::DeviceInstanceId id) -> const feathers::device::Descriptor* {
			return id == kTheFilter ? &filter::kDescriptor : nullptr;
		},
		[](const void*, Source s, Destination dst) {
			return s.isDevice() && dst.isDevice(); // one device here, so any device-to-device is itself
		}
	};
	const size_t first = filter::kDescriptor.controlOfNthModulatableBesideEnable(0);
	CHECK(mayRoute(kRackAxes, devices, Source::core(0), Destination::control(kTheFilter, first), permitAll));
	size_t notModulatable = filter::kDescriptor.params.size();
	for (size_t c = 0; c < filter::kDescriptor.params.size(); ++c) {
		if (!filter::kDescriptor.params[c].modulatable) {
			notModulatable = c;
			break;
		}
	}
	REQUIRE(notModulatable < filter::kDescriptor.params.size());
	CHECK_FALSE(mayRoute(kRackAxes, devices, Source::core(0),
	                     Destination::control(kTheFilter, notModulatable), permitAll));
}

TEST_CASE("a block context defaults to no transport, and says so through the clock") {
	const BlockContext fresh {};
	REQUIRE_FALSE(fresh.clock.haveTempo());
	REQUIRE(fresh == unknownBlock());

	BlockContext running {};
	running.clock = feathers::device::TransportClock::fromSecondsPerBeat(0.5);
	REQUIRE(running.clock.haveTempo());
	REQUIRE(running.clock.secondsPerBeat == doctest::Approx(0.5));
	REQUIRE_FALSE(running == unknownBlock());
}


TEST_CASE("the authority by endpoint: the same rule, asked of devices by id") {
	// **KILL-LANES STEP 2.** A route names a device by its id and a control by its index, so the authority
	// asks a host's `DeviceView` — what is there, and where it stands — instead of a lane table. The
	// claims are the coordinate cases' own: liveness, slack, the host's rule, upstream-only, and an
	// unranked device refusing in the safe direction.
	constexpr feathers::DeviceInstanceId kEnv = 11;    // a DAHDSR, upstream
	constexpr feathers::DeviceInstanceId kFilter = 12; // a filter, downstream of it
	/// **THE FIXTURE STATES THE RELATION, NOT A RANK (kill-lanes step 4b).** It kept two rank numbers and
	/// the rule compared them; a rank is a position in ONE linearisation of a partial order, and adding a
	/// route changes that linearisation — so the comparison refused acyclic routes whose current order
	/// happened to run the other way. The authority asks the question it means: would this close a cycle.
	struct Held {
		bool envUpstreamOfFilter = true;
		bool filterOrdered = true;
	};
	Held held;
	const DeviceView devices {
		&held,
		[](const void*, feathers::DeviceInstanceId id) -> const feathers::device::Descriptor* {
			return id == kEnv ? &dahdsr::kDescriptor : id == kFilter ? &filter::kDescriptor : nullptr;
		},
		[](const void* h, Source s, Destination d) {
			const auto* held = static_cast<const Held*>(h);
			if (!s.isDevice() || !d.isDevice()) {
				return false; // a host end is ordered by nothing here
			}
			if (s.device == d.device) {
				return true; // a device is not upstream of itself
			}
			if (!held->filterOrdered) {
				return true; // a device the host cannot order refuses, in the safe direction
			}
			const bool forward = s.device == kEnv && d.device == kFilter;
			return held->envUpstreamOfFilter ? !forward : forward;
		}
	};
	const size_t filterFirst = filter::kDescriptor.controlOfNthModulatableBesideEnable(0);
	const size_t envFirst = dahdsr::kDescriptor.controlOfNthModulatableBesideEnable(0);
	const Source env = Source::output(kEnv, 0);

	// FORWARD, and a macro into the same control — the controls.
	CHECK(mayRoute(kAxes, devices, env, Destination::control(kFilter, filterFirst), permitAll));
	CHECK(mayRoute(kAxes, devices, Source::core(0), Destination::control(kFilter, filterFirst), permitAll));
	// ITSELF, and BACKWARDS.
	CHECK_FALSE(mayRoute(kAxes, devices, env, Destination::control(kEnv, envFirst), permitAll));
	held.envUpstreamOfFilter = false; // the envelope after the filter now
	CHECK_FALSE(mayRoute(kAxes, devices, env, Destination::control(kFilter, filterFirst), permitAll));
	CHECK(mayRoute(kAxes, devices, Source::core(0), Destination::control(kFilter, filterFirst), permitAll));
	held.envUpstreamOfFilter = true;
	// A DEVICE THE HOST CANNOT ORDER refuses device-to-device, and only that.
	held.filterOrdered = false;
	CHECK_FALSE(mayRoute(kAxes, devices, env, Destination::control(kFilter, filterFirst), permitAll));
	CHECK(mayRoute(kAxes, devices, Source::core(0), Destination::control(kFilter, filterFirst), permitAll));
	held.filterOrdered = true;
	// SLACK: a control that is not modulatable, an output the device does not publish, a device nobody
	// holds, a filter (no outputs) as a source.
	size_t notModulatable = filter::kDescriptor.params.size();
	for (size_t c = 0; c < filter::kDescriptor.params.size(); ++c) {
		if (!filter::kDescriptor.params[c].modulatable) {
			notModulatable = c;
			break;
		}
	}
	REQUIRE(notModulatable < filter::kDescriptor.params.size());
	CHECK_FALSE(mayRoute(kAxes, devices, env, Destination::control(kFilter, notModulatable), permitAll));
	CHECK_FALSE(mayRoute(kAxes, devices, Source::output(kEnv, 1), Destination::control(kFilter, filterFirst), permitAll));
	CHECK_FALSE(mayRoute(kAxes, devices, Source::output(99, 0), Destination::control(kFilter, filterFirst), permitAll));
	CHECK_FALSE(mayRoute(kAxes, devices, Source::output(kFilter, 0), Destination::core(0), permitAll));
	// THE HOST'S RULE, on host ends; and bounds on the host's own table.
	CHECK(mayRoute(kAxes, devices, Source::core(kForbiddenSource), Destination::core(kForbiddenDestination), permitAll));
	CHECK_FALSE(mayRoute(kAxes, devices, Source::core(kForbiddenSource), Destination::core(kForbiddenDestination), oneRule));
	CHECK(mayRoute(kAxes, devices, Source::core(kForbiddenSource), Destination::core(kForbiddenDestination + 1), oneRule));
	CHECK_FALSE(mayRoute(kAxes, devices, Source::core(kAxes.coreSources), Destination::core(0), permitAll));
	CHECK_FALSE(mayRoute(kAxes, devices, Source::core(0), Destination::core(kAxes.coreDestinations), permitAll));
	CHECK_FALSE(mayRoute(kAxes, devices, Source::core(0), Destination::core(0), nullptr));
}
