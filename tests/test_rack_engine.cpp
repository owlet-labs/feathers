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

#include "fx/automod/automod_types.h"
#include "fx/harm/harm_types.h"
#include "fx/owltt/owltt_types.h"
#include "fx/filter/filter_types.h"
#include "fx/shaper/shaper_types.h"
#include "fx/featherverb/featherverb_types.h"
#include "fx/shaper/shaper.h"
#include "fx/sklitter/sklitter_types.h"
#include "fx/sklitter/perform.h" // waypointAxis — where the puck must be to play one waypoint whole
#include "mod/dahdsr/dahdsr_types.h"
#include "mod/lfo/lfo_types.h"
#include "mod/mseg/mseg_types.h"
#include "mod/tracker/tracker.h"
#include "mod/tracker/tracker_types.h"
// THE ONE TABLE BOTH HOSTS RUN FROM. `rack_engine.cpp` includes it for exactly the same reason —
// `deviceForTag` is how a tag becomes a descriptor — so a sweep here over `kDevices` is a sweep over
// what this engine can actually be given, rather than over a list written out beside it.
#include "plugin/device_registry.h"
#include "rack/device_publishers.h"
#include "rack/rack_engine.h"
#include "mod/noteexpr/noteexpr_types.h"
#include "fx/busutil/busutil_meter.h"
#include "fx/busutil/busutil_types.h"
#include "fx/comp/comp_types.h"
#include "fx/clip/clip_types.h"
#include "fx/spectral/spectral.h"
#include "fx/disperser/disperser_types.h"
#include "fx/resonate/resonate_dsp.h"
#include "fx/resonate/resonate_types.h"
#include "rack_endpoints.h"
#include "patch_values.h"
#include "audio_notes_probe.h"
#include "device/tick_window.h"

#include <algorithm>
#include <cmath>
#include "rack/converger.h"
#include "rack/published_chain.h"
#include "rack/rack_bridge.h"
#include "rack/rack_device.h"
#include "composite/composite_types.h"
#include "rack/rack_axes.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <new>
#include <thread>
#include <tuple>
#include <string>
#include <array>
#include <vector>

// THE RACK ENGINE — a chain, a route table and eight macros, hosted by nothing.
//
// **EVERY CLAIM ABOUT A VALUE IS ASSERTED AS A VALUE.** CLAUDE.md's rule, found independently by two
// sessions in one week and given its shape by two more: a comparison between two renders is evidence
// that SOMETHING changed, never that the right thing did, and it passes whenever anything differs.
// Every case below that is about modulation reads `resolvedFieldForTest`, and the buffer comparison
// appears only as a SECOND assertion proving the first is not vacuous.

using namespace feathers;
namespace amd = feathers::fx::automod;
namespace harm = feathers::fx::harm;
namespace filter = feathers::fx::filter;
namespace dahdsr = feathers::mod::dahdsr;
namespace mseg = feathers::mod::mseg;

namespace {

constexpr backplane::Axes kAxes {
	.coreSources = 8,
	.coreDestinations = 0,
	.deviceSlots = 4,
	.outputStride = 4,
};

/// A block of something audible, so a device that does nothing is distinguishable from one that does.
std::vector<float> tone(int n) {
	std::vector<float> v(static_cast<size_t>(n));
	for (int i = 0; i < n; ++i) {
		v[static_cast<size_t>(i)] = 0.5f * std::sin(6.2831853f * 220.0f * static_cast<float>(i) / 48000.0f);
	}
	return v;
}

size_t controlNamed(const device::Descriptor& d, const char* name) {
	for (size_t i = 0; i < d.params.size(); ++i) {
		if (std::string(d.params[i].name).find(name) != std::string::npos) {
			return i;
		}
	}
	return d.params.size();
}

} // namespace

TEST_CASE("a device arrives at its declared defaults, switch included — EVERY device in the registry") {
	// **THIS CASE USED TO ASSERT AN OVERRIDE AND NOW ASSERTS ITS ABSENCE.** `addDeviceAt` stamped the
	// enable to `max` after copying the declared defaults, because every fx device declared OFF and a
	// rack's add is a hand asking for the device. The same decision now holds for both hosts
	// (2026-09-04): every effect defaults to ON when it is added — so the descriptors say
	// ON and the override is gone. The old case's own premise line, `REQUIRE(def != max)`, is what
	// makes the change visible: it cannot hold any more, and could not have been left in place.
	//
	// **AND IT WALKS `kDevices` NOW RATHER THAN NAMING THE FILTER.** A case pinned to one device is
	// what let the two lanes disagree in the first place; this one is enrolled by registration, so a
	// device added next month is covered without anybody remembering. The filter alone would also
	// have gone green against a stamp that only ever reached the one device the case named.
	size_t checked = 0;
	for (const device::Descriptor& d : kDevices) {
		CAPTURE(std::string(d.traits.name));
		rack::RackEngine rack(kAxes, hostCatalogue());
		const size_t slot = rack.addDevice(d.traits.tag);
		if (slot == rack::RackEngine::kNoSlot) {
			// A DEVICE THIS RACK CANNOT RUN IS NOT A FAILURE, and it is not silently skipped either:
			// the tally below is what stops a rack that refused EVERYTHING from passing.
			continue;
		}
		for (size_t c = 0; c < d.params.size(); ++c) {
			CAPTURE(c);
			REQUIRE(rack.fieldAt(slot, c) == doctest::Approx(d.params[c].def));
		}
		const size_t enable = device::enableControlIn(d.traits, d.params);
		if (enable != device::kNoControl) {
			// SAID TWICE, AND ON PURPOSE. The loop above says the field equals the DECLARATION; this
			// says the declaration is ON. Either alone is satisfiable by a device that declares off
			// — the first because it would agree with it, the second because nothing would have
			// checked the row.
			CHECK(rack.fieldAt(slot, enable) >= 0.5f);
			CHECK(d.params[enable].def == doctest::Approx(1.0));
		}
		++checked;
	}
	CHECK(checked >= 10);
}

TEST_CASE("the rack refuses a tag it cannot run, and says so by refusing rather than by crashing") {
	rack::RackEngine rack(kAxes, hostCatalogue());
	REQUIRE(rack.addDevice("nope") == rack::RackEngine::kNoSlot);
	// A MODULATOR IS ACCEPTED NOW — see the ModOps cases below. It still has no `AudioOps`, and does
	// not need one: the rack runs it as a source rather than in the audio chain.
	REQUIRE(dahdsr::kDescriptor.audio == nullptr);
}

TEST_CASE("the rack fills up and then refuses") {
	rack::RackEngine rack(kAxes, hostCatalogue());
	for (size_t i = 0; i < kAxes.deviceSlots; ++i) {
		REQUIRE(rack.addDevice(filter::kTraits.tag) == i);
	}
	REQUIRE(rack.addDevice(filter::kTraits.tag) == rack::RackEngine::kNoSlot);
}

TEST_CASE("a device in the chain is heard") {
	rack::RackEngine rack(kAxes, hostCatalogue());
	rack.prepare(48000.0, 4096);
	const int n = 256;
	std::vector<float> l = tone(n), r = tone(n);
	const std::vector<float> dry = l;

	rack.processStereo(l.data(), r.data(), n, backplane::unknownBlock());
	REQUIRE(l == dry); // an EMPTY rack is a wire

	rack.addDevice(filter::kTraits.tag);
	rack.prepare(48000.0, 4096);
	l = dry;
	r = dry;
	rack.processStereo(l.data(), r.data(), n, backplane::unknownBlock());
	REQUIRE_FALSE(l == dry); // and a rack with a filter in it is not
}

TEST_CASE("a macro routed to a control moves that control's RESOLVED VALUE") {
	// THE CASE THE WHOLE PROJECT IS FOR, and it asserts the VALUE rather than the sound. A render
	// comparison here would pass if anything at all differed — including the filter simply running.
	rack::RackEngine rack(kAxes, hostCatalogue());
	rack.prepare(48000.0, 4096);
	const size_t slot = rack.addDevice(filter::kTraits.tag);
	const size_t cutoff = controlNamed(filter::kDescriptor, "Cutoff");
	REQUIRE(cutoff < filter::kDescriptor.params.size());
	REQUIRE(filter::kDescriptor.params[cutoff].modulatable);

	const size_t ordinal = filter::kDescriptor.nthModulatableOfControl(cutoff);
	const backplane::Destination dest = test::ordinalOf(rack, slot, ordinal);

	const float base = rack.fieldAt(slot, cutoff);
	const int n = 64;
	std::vector<float> l = tone(n), r = tone(n);

	// Unrouted: resolved is the base.
	rack.processStereo(l.data(), r.data(), n, backplane::unknownBlock());
	REQUIRE(rack.resolvedFieldForTest(slot, cutoff) == doctest::Approx(base));

	// Routed, macro at zero: still the base — a route with nothing behind it moves nothing.
	rack.setRouteAmount(test::macro(0), dest, 0.5f);
	rack.processStereo(l.data(), r.data(), n, backplane::unknownBlock());
	REQUIRE(rack.resolvedFieldForTest(slot, cutoff) == doctest::Approx(base));

	// Macro up: the control moves, by amount x the destination's own range.
	rack.setMacro(0, 1.0f);
	rack.processStereo(l.data(), r.data(), n, backplane::unknownBlock());
	const device::ParamDesc& p = filter::kDescriptor.params[cutoff];
	const double expected = static_cast<double>(base) + 0.5 * device::routeSpanOf(p);
	REQUIRE(rack.resolvedFieldForTest(slot, cutoff)
	        == doctest::Approx(std::min(expected, p.offsetsMayLeaveRange ? expected : p.max)));
	REQUIRE(rack.resolvedFieldForTest(slot, cutoff) != doctest::Approx(base));

	// AND THE SECOND ASSERTION, which exists only to prove the first is not vacuous: the sound moved
	// too. On its own this would prove nothing.
	std::vector<float> quiet = tone(n), quietR = tone(n);
	rack.setMacro(0, 0.0f);
	rack.processStereo(quiet.data(), quietR.data(), n, backplane::unknownBlock());
	std::vector<float> loud = tone(n), loudR = tone(n);
	rack.setMacro(0, 1.0f);
	rack.processStereo(loud.data(), loudR.data(), n, backplane::unknownBlock());
	REQUIRE_FALSE(quiet == loud);
}

TEST_CASE("backlog EG: a macro switched bipolar reaches its destination by 2v - 1 — the stored value is the hand's 0..1, the cell every route reads is the mode's") {
	// THE ENGINE HALF OF THE MODE, asserted BY VALUE against the arithmetic it declares: amount x
	// (2v - 1) x the destination's span, restated here, for a macro below and above its centre; and
	// the stored macro read back as the hand set it, because the DAW's parameter must not change shape.
	rack::RackEngine rack(kAxes, hostCatalogue());
	rack.prepare(48000.0, 4096);
	const size_t slot = rack.addDevice(filter::kTraits.tag);
	const size_t cutoff = controlNamed(filter::kDescriptor, "Cutoff");
	const size_t ordinal = filter::kDescriptor.nthModulatableOfControl(cutoff);
	const backplane::Destination dest = test::ordinalOf(rack, slot, ordinal);
	const device::ParamDesc& p = filter::kDescriptor.params[cutoff];
	// A BASE IN THE MIDDLE, so a negative reach has room to show below it.
	const float base = static_cast<float>(0.5 * (p.min + p.max));
	rack.setField(slot, cutoff, base);
	const int n = 64;
	std::vector<float> l = tone(n), r = tone(n);
	const float amount = 0.5f;
	rack.setRouteAmount(test::macro(0), dest, amount);

	// UNIPOLAR, a quarter up: the control moves by amount x 0.25 x span.
	rack.setMacro(0, 0.25f);
	rack.processStereo(l.data(), r.data(), n, backplane::unknownBlock());
	CHECK(rack.resolvedFieldForTest(slot, cutoff) == doctest::Approx(base + amount * 0.25 * device::routeSpanOf(p)));
	CHECK_FALSE(rack.macroBipolar(0));

	// BIPOLAR, the same quarter: it reads -0.5, and the control moves DOWN by amount x 0.5 x span.
	rack.setMacroBipolar(0, true);
	CHECK(rack.macroBipolar(0));
	CHECK(rack.macro(0) == doctest::Approx(0.25f)); // the stored value is what the hand set
	rack.processStereo(l.data(), r.data(), n, backplane::unknownBlock());
	CHECK(rack.resolvedFieldForTest(slot, cutoff) == doctest::Approx(base - amount * 0.5 * device::routeSpanOf(p)));

	// THE CENTRE IS THE REST: half way up publishes nothing, so the control sits at its base.
	rack.setMacro(0, 0.5f);
	rack.processStereo(l.data(), r.data(), n, backplane::unknownBlock());
	CHECK(rack.resolvedFieldForTest(slot, cutoff) == doctest::Approx(base));

	// FULLY UP reads +1: amount x span, the same reach a unipolar macro at the top has.
	rack.setMacro(0, 1.0f);
	rack.processStereo(l.data(), r.data(), n, backplane::unknownBlock());
	CHECK(rack.resolvedFieldForTest(slot, cutoff) == doctest::Approx(base + amount * device::routeSpanOf(p)));

	// AND BACK TO UNIPOLAR: the cell follows the mode at once, with the value untouched.
	rack.setMacroBipolar(0, false);
	rack.setMacro(0, 0.25f);
	rack.processStereo(l.data(), r.data(), n, backplane::unknownBlock());
	CHECK(rack.resolvedFieldForTest(slot, cutoff) == doctest::Approx(base + amount * 0.25 * device::routeSpanOf(p)));
}

TEST_CASE("the base value is untouched by modulation — a route displaces, it does not overwrite") {
	rack::RackEngine rack(kAxes, hostCatalogue());
	rack.prepare(48000.0, 4096);
	const size_t slot = rack.addDevice(filter::kTraits.tag);
	const size_t cutoff = controlNamed(filter::kDescriptor, "Cutoff");
	const size_t ordinal = filter::kDescriptor.nthModulatableOfControl(cutoff);
	const backplane::Destination dest = test::ordinalOf(rack, slot, ordinal);

	const float base = rack.fieldAt(slot, cutoff);
	rack.setRouteAmount(test::macro(0), dest, 0.5f);
	rack.setMacro(0, 1.0f);
	std::vector<float> l(64, 0.0f), r(64, 0.0f);
	rack.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());

	REQUIRE(rack.fieldAt(slot, cutoff) == doctest::Approx(base));
	REQUIRE(rack.resolvedFieldForTest(slot, cutoff) != doctest::Approx(base));
}

TEST_CASE("removing a device clears every route into its lane, in BOTH directions") {
	// THE ABA GUARD. A lane is an address in a fixed pool; a lane reissued while still carrying the
	// old device's amounts is a device that arrives pre-modulated by routes nobody drew.
	//
	// **THE REMOVED DEVICE IS A MODULATOR NOW, WITH A FILTER DOWNSTREAM OF IT.** It was a lone filter,
	// and the "out of it" route ran from the filter's output 0 to destination 0 — a source the filter
	// does not declare (it publishes nothing) into a destination that, with `coreDestinations = 0`, is
	// ANOTHER device's control on an empty lane. The engine's own table accepted that write because it
	// had no gate; `mayRoute` never would have. Since the rack routing rule's commit 3 the engine writes
	// through `Graph::route`, which refuses a slack source, so the fixture now says what the title
	// says: a device that really publishes, routed both INTO and OUT OF, both cleared by its removal.
	rack::RackEngine rack(kAxes, hostCatalogue());
	const size_t slot = rack.addDevice(dahdsr::kTraits.tag);
	const size_t downstream = rack.addDevice(filter::kTraits.tag);
	REQUIRE(slot != rack::RackEngine::kNoSlot);
	REQUIRE(downstream != rack::RackEngine::kNoSlot);
	const size_t ordinal = 0;
	const backplane::Destination dest = test::ordinalOf(rack, slot, ordinal);
	const backplane::Source out0 = test::outputOf(rack, slot, 0);
	const backplane::Destination intoDownstream = test::ordinalOf(rack, downstream, 0);

	rack.setRouteAmount(test::macro(0), dest, 0.5f);                 // into the lane, from a macro
	rack.setRouteAmount(out0, intoDownstream, 0.25f);   // and out of it, forward, to the filter
	REQUIRE(rack.routeAmount(test::macro(0), dest) == doctest::Approx(0.5f));
	REQUIRE(rack.routeAmount(out0, intoDownstream) == doctest::Approx(0.25f));

	rack.removeDeviceAtLane(slot);
	REQUIRE(rack.routeAmount(test::macro(0), dest) == doctest::Approx(0.0f));
	REQUIRE(rack.routeAmount(out0, intoDownstream) == doctest::Approx(0.0f));

	// And the reissued lane carries nothing.
	REQUIRE(rack.addDevice(dahdsr::kTraits.tag) == slot);
	REQUIRE(rack.routeAmount(test::macro(0), dest) == doctest::Approx(0.0f));
	REQUIRE(rack.routeAmount(out0, intoDownstream) == doctest::Approx(0.0f));
}

TEST_CASE("a zoned control is modulated in ZONE WIDTHS, not in full ranges") {
	// ParamDesc::zones bends the amount's unit, and BOTH readers must bend together — without this a
	// route displaces a zone control across all its zones and modulation jumps, which makes a
	// modulated zone control useless as a zone control.
	rack::RackEngine rack(kAxes, hostCatalogue());
	rack.prepare(48000.0, 4096);
	const size_t slot = rack.addDevice(filter::kTraits.tag);

	size_t zoned = filter::kDescriptor.params.size();
	for (size_t i = 0; i < filter::kDescriptor.params.size(); ++i) {
		if (filter::kDescriptor.params[i].zones > 0 && filter::kDescriptor.params[i].modulatable) {
			zoned = i;
			break;
		}
	}
	if (zoned == filter::kDescriptor.params.size()) {
		// The filter declares no modulatable zoned control; the arithmetic is still pinned by the
		// unzoned case above, and OWLTT's zone controls are covered by their own suite.
		return;
	}
	const device::ParamDesc& p = filter::kDescriptor.params[zoned];
	const size_t ordinal = filter::kDescriptor.nthModulatableOfControl(zoned);
	const backplane::Destination dest = test::ordinalOf(rack, slot, ordinal);
	const float base = rack.fieldAt(slot, zoned);

	rack.setRouteAmount(test::macro(0), dest, 1.0f); // ONE ZONE, not one range
	rack.setMacro(0, 1.0f);
	std::vector<float> l(64, 0.0f), r(64, 0.0f);
	rack.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());

	const double oneZone = (p.max - p.min) / static_cast<double>(p.zones);
	REQUIRE(rack.resolvedFieldForTest(slot, zoned)
	        == doctest::Approx(std::min<double>(base + oneZone, p.max)));
}

TEST_CASE("a mode cell: a full-depth route on the Disperser's Topo phase and Twist position never changes the zone the dropdown names") {
	// Increasing the offset must never change the zone the dropdown names. The mode is a Choice no
	// route reaches; its position is fenced by its range. Resolved by the ENGINE and read back the DEVICE's way —
	// the mode and the resolved position onto the coordinate, then the firmware's zone — at both ends of a macro's
	// travel, both signs of the amount, every Topo mode and every fenced Twist mode. TOPO'S VALUE IS AN INFINITE
	// PHASE since round 3 (2026-09-27): the route carries it out of 0..1, and the routing still does not move.
	namespace dspr = feathers::fx::disperser;
	rack::RackEngine rack(kAxes, hostCatalogue());
	rack.prepare(48000.0, 4096);
	const size_t slot = rack.addDevice(dspr::kTraits.tag);
	REQUIRE(slot != rack::RackEngine::kNoSlot);
	const auto zoneOf = [](double c) { return dspr::zoneOf(dspr::splitCoordinate(static_cast<double>(static_cast<float>(c))).knob).index; };
	std::vector<float> l(64, 0.0f), r(64, 0.0f);
	int moved = 0;
	int leftZone = 0;
	for (const auto& [modeParam, posParam, modes] : { std::tuple { dspr::Param::TopoMode, dspr::Param::TopoPhase, 8u },
	                                                   std::tuple { dspr::Param::TwistMode, dspr::Param::TwistPos, 5u } }) {
		const auto posControl = static_cast<size_t>(posParam);
		const backplane::Destination dest = test::ordinalOf(rack, slot, dspr::kDescriptor.nthModulatableOfControl(posControl));
		for (uint32_t mode = 0; mode < modes; ++mode) {
			rack.setField(slot, static_cast<size_t>(modeParam), static_cast<float>(mode));
			for (const float base : { 0.05f, 0.95f }) {
				rack.setField(slot, posControl, base);
				for (const float amount : { 1.0f, -1.0f }) {
					rack.setRouteAmount(test::macro(0), dest, amount);
					for (const float macro : { 0.0f, 1.0f }) {
						rack.setMacro(0, macro);
						rack.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
						const float pos = rack.resolvedFieldForTest(slot, posControl);
						CAPTURE(mode);
						CAPTURE(base);
						CAPTURE(amount);
						CAPTURE(macro);
						CAPTURE(pos);
						const double c = modeParam == dspr::Param::TopoMode ? dspr::topoPlaceOf(mode, pos).coordinate
						                                                    : dspr::topoCoordinateOf(mode, pos);
						CHECK(zoneOf(c) == static_cast<int>(mode));
						leftZone += (modeParam == dspr::Param::TopoMode && (pos < 0.0f || pos > 1.0f)) ? 1 : 0;
						moved += std::abs(pos - base) > 1e-3f ? 1 : 0;
					}
				}
			}
		}
		rack.setRouteAmount(test::macro(0), dest, 0.0f);
	}
	CHECK(moved >= 26); // non-vacuity: the routes still move the positions — inside their zones
	CHECK(leftZone >= 16); // and Topo's phase really was carried past its zone's travel, both ways
}

TEST_CASE("two devices both run, in slot order") {
	rack::RackEngine rack(kAxes, hostCatalogue());
	rack.prepare(48000.0, 4096);
	const int n = 128;
	const std::vector<float> dry = tone(n);

	rack.addDevice(filter::kTraits.tag);
	std::vector<float> one = dry, oneR = dry;
	rack.processStereo(one.data(), oneR.data(), n, backplane::unknownBlock());

	rack::RackEngine two(kAxes, hostCatalogue());
	two.prepare(48000.0, 4096);
	two.addDevice(filter::kTraits.tag);
	two.addDevice(filter::kTraits.tag);
	std::vector<float> both = dry, bothR = dry;
	two.processStereo(both.data(), bothR.data(), n, backplane::unknownBlock());

	REQUIRE_FALSE(one == both); // a second filter is audible
}

//------------------------------------------------------------------------------------------------//
// MODULATORS — the source side, driven through ModOps and never by type.                           //
//------------------------------------------------------------------------------------------------//

TEST_CASE("the rack now ACCEPTS a modulator, because a generic way to drive one exists") {
	// This case is the inverse of the refusal above, which it replaces. `ModOps` is what changed.
	rack::RackEngine rack(kAxes, hostCatalogue());
	REQUIRE(dahdsr::kDescriptor.mod != nullptr);
	REQUIRE(dahdsr::kDescriptor.audio == nullptr); // still not an audio device, and need not be
	REQUIRE(rack.addDevice(dahdsr::kTraits.tag) == 0);
}

TEST_CASE("an envelope publishes nothing with the gate down, and rises when it is raised") {
	rack::RackEngine rack(kAxes, hostCatalogue());
	rack.prepare(48000.0, 4096);
	const size_t slot = rack.addDevice(dahdsr::kTraits.tag);
	REQUIRE(slot != rack::RackEngine::kNoSlot);

	std::vector<float> l(512, 0.0f), r(512, 0.0f);
	rack.processStereo(l.data(), r.data(), 512, backplane::unknownBlock());
	const float idle = rack.publishedOutputAt(slot, 0);

	rack.setGate(true);
	for (int i = 0; i < 8; ++i) {
		rack.processStereo(l.data(), r.data(), 512, backplane::unknownBlock());
	}
	const float held = rack.publishedOutputAt(slot, 0);

	REQUIRE(idle == doctest::Approx(0.0f));
	REQUIRE(held > idle); // it went somewhere, and it went UP
}

TEST_CASE("an envelope routed to a filter control moves that control's RESOLVED VALUE") {
	// THE THING THE WHOLE ARC IS FOR: drop an effect, drop an envelope, route one to the other — with
	// the rack never naming either device type. Asserted as a VALUE, per this file's opening note.
	rack::RackEngine rack(kAxes, hostCatalogue());
	rack.prepare(48000.0, 4096);
	const size_t fx = rack.addDevice(filter::kTraits.tag);
	const size_t env = rack.addDevice(dahdsr::kTraits.tag);
	REQUIRE(fx != rack::RackEngine::kNoSlot);
	REQUIRE(env != rack::RackEngine::kNoSlot);

	const size_t cutoff = controlNamed(filter::kDescriptor, "Cutoff");
	const size_t ordinal = filter::kDescriptor.nthModulatableOfControl(cutoff);
	const backplane::Destination dest = test::ordinalOf(rack, fx, ordinal);
	const backplane::Source source = test::outputOf(rack, env, 0);

	rack.setRouteAmount(source, dest, 0.5f);
	const float base = rack.fieldAt(fx, cutoff);

	std::vector<float> l(512, 0.0f), r(512, 0.0f);
	rack.processStereo(l.data(), r.data(), 512, backplane::unknownBlock());
	// Gate down: the envelope publishes nothing, so the route moves nothing.
	REQUIRE(rack.resolvedFieldForTest(fx, cutoff) == doctest::Approx(base));

	rack.setGate(true);
	for (int i = 0; i < 8; ++i) {
		rack.processStereo(l.data(), r.data(), 512, backplane::unknownBlock());
	}
	REQUIRE(rack.publishedOutputAt(env, 0) > 0.0f);            // the envelope is up
	REQUIRE(rack.resolvedFieldForTest(fx, cutoff) > base); // and it moved the filter
	REQUIRE(rack.fieldAt(fx, cutoff) == doctest::Approx(base)); // without touching the base
}

TEST_CASE("a modulator's own controls are modulation destinations too") {
	// A device is a device. An envelope's Attack is a declared modulatable control like any other, so
	// a macro may reach it — which is only true because the rack resolves a modulator's row by the
	// same road it resolves an audio device's.
	rack::RackEngine rack(kAxes, hostCatalogue());
	rack.prepare(48000.0, 4096);
	const size_t env = rack.addDevice(dahdsr::kTraits.tag);
	size_t mod = dahdsr::kDescriptor.params.size();
	for (size_t i = 0; i < dahdsr::kDescriptor.params.size(); ++i) {
		if (dahdsr::kDescriptor.params[i].modulatable) {
			mod = i;
			break;
		}
	}
	REQUIRE(mod < dahdsr::kDescriptor.params.size());

	const size_t ordinal = dahdsr::kDescriptor.nthModulatableOfControl(mod);
	const backplane::Destination dest = test::ordinalOf(rack, env, ordinal);
	const float base = rack.fieldAt(env, mod);

	rack.setRouteAmount(test::macro(0), dest, 0.5f);
	rack.setMacro(0, 1.0f);
	std::vector<float> l(128, 0.0f), r(128, 0.0f);
	rack.processStereo(l.data(), r.data(), 128, backplane::unknownBlock());
	REQUIRE(rack.resolvedFieldForTest(env, mod) > base);
}

TEST_CASE("removing a modulator stops its source publishing") {
	rack::RackEngine rack(kAxes, hostCatalogue());
	rack.prepare(48000.0, 4096);
	const size_t env = rack.addDevice(dahdsr::kTraits.tag);
	rack.setGate(true);
	std::vector<float> l(512, 0.0f), r(512, 0.0f);
	for (int i = 0; i < 8; ++i) {
		rack.processStereo(l.data(), r.data(), 512, backplane::unknownBlock());
	}
	REQUIRE(rack.publishedOutputAt(env, 0) > 0.0f);

	rack.removeDeviceAtLane(env);
	REQUIRE(rack.publishedOutputAt(env, 0) == doctest::Approx(0.0f));
}

TEST_CASE("a source row past what the device declares publishes nothing") {
	rack::RackEngine rack(kAxes, hostCatalogue());
	rack.prepare(48000.0, 4096);
	const size_t env = rack.addDevice(dahdsr::kTraits.tag);
	REQUIRE(dahdsr::kDescriptor.outputs.size() < kAxes.outputStride); // there IS slack to test
	for (size_t out = dahdsr::kDescriptor.outputs.size(); out < kAxes.outputStride; ++out) {
		CAPTURE(out);
		REQUIRE(rack.publishedOutputAt(env, out) == doctest::Approx(0.0f));
	}
}

TEST_CASE("a held gate lets the envelope reach a steady value and stay there") {
	// **THIS CASE DOES NOT PIN THE RETRIGGER CLEAR, AND SAYING SO IS THE POINT.**
	//
	// Deleting `retrigger_ = false;` in the engine latches the edge so every tick retriggers, and
	// this suite stays GREEN — through two attempts to make it fail. That is not a gap in the tests;
	// the mutation is INERT at default settings, which I established by reading `dahdsr.cpp:328`
	// rather than by contorting the fixture further:
	//
	//     "From raw_ rather than from zero, so a note struck while the last one is still decaying
	//      picks the envelope up where it stands instead of dropping it to the floor first."
	//
	// A retrigger under a HELD gate re-attacks FROM THE CURRENT VALUE. At sustain that re-attacks
	// from sustain and converges straight back, so a latched edge is very nearly a no-op — by design,
	// because the design is click avoidance. It would be observable with a non-zero Delay stage (the
	// restart re-enters delay) or with loop on; neither is a default.
	//
	// Playbook 8b: a green known-bad needs a check that the mutation was OBSERVABLE at all before it
	// counts as a gap, and inert and uncovered report identically. Two failed attempts to make this
	// fail were me treating an inert mutation as a missing case.
	//
	// What the case DOES pin is worth keeping on its own: the envelope climbs and then holds, which a
	// tick loop that miscounted its accumulator would break.
	rack::RackEngine rack(kAxes, hostCatalogue());
	rack.prepare(48000.0, 4096);
	const size_t env = rack.addDevice(dahdsr::kTraits.tag);

	// ONE TICK PER BLOCK, WHICH IS THE WHOLE POINT OF THE FIXTURE. A first draft used 2048-frame
	// blocks — 32 ticks each — and stayed green under the latch, because an envelope that restarts
	// once per block still fully re-attacks within the other 31 ticks and settles at the same number.
	// **The block was long enough to hide a restart.** At one tick per block a latched edge restarts
	// EVERY tick, and the envelope can never leave the floor.
	//
	// Playbook 8l from the other end: there a dead default hid a bug that needed motion; here a coarse
	// window hid one that needed resolution.
	const int kTick = 64; // ModOps::blockSamples for both builtins
	std::vector<float> l(static_cast<size_t>(kTick), 0.0f), r(static_cast<size_t>(kTick), 0.0f);
	rack.setGate(true);
	for (int i = 0; i < 4000; ++i) {
		rack.processStereo(l.data(), r.data(), kTick, backplane::unknownBlock());
	}
	const float settled = rack.publishedOutputAt(env, 0);
	REQUIRE(settled > 0.1f); // it climbed, and a restart-every-tick envelope cannot

	// And two more ticks do not move it, because nothing retriggered.
	rack.processStereo(l.data(), r.data(), kTick, backplane::unknownBlock());
	const float a = rack.publishedOutputAt(env, 0);
	rack.processStereo(l.data(), r.data(), kTick, backplane::unknownBlock());
	const float b = rack.publishedOutputAt(env, 0);
	REQUIRE(a == doctest::Approx(settled).epsilon(0.01));
	REQUIRE(b == doctest::Approx(a).epsilon(0.001));
}

TEST_CASE("a note under a gate already up restarts a rack MSEG — the gate's edge alone cannot say so") {
	// Retrigger did nothing while the envelope had not completed. The engine derived its
	// retrigger from the gate's rising edge and from nothing else, so a note struck while another was
	// held — no edge — restarted nothing. `retrigger()` is the road for that note; this is the case
	// that says it reaches the modulator, BY VALUE, at the engine with no CLAP in the way.
	//
	// THE MSEG RATHER THAN THE DAHDSR, because its restart is observable at default settings: the
	// default shape rises from 0 to 1 and holds at the sustain breakpoint, so a restart from the top
	// reads near 0 on the very next tick. A DAHDSR at its defaults re-attacks from where it stands
	// to where it already is — the inert mutation the case above documents.
	rack::RackEngine rack(kAxes, hostCatalogue());
	rack.prepare(48000.0, 4096);
	const size_t slot = rack.addDevice(mseg::kTraits.tag);
	REQUIRE(slot != rack::RackEngine::kNoSlot);

	const int kTick = 64;
	std::vector<float> l(static_cast<size_t>(kTick), 0.0f), r(static_cast<size_t>(kTick), 0.0f);
	rack.setGate(true);
	// The default window is one second and the hold is at the end of segment 1; a second of ticks is
	// parked there.
	for (int i = 0; i < 800; ++i) {
		rack.processStereo(l.data(), r.data(), kTick, backplane::unknownBlock());
	}
	const float held = rack.publishedOutputAt(slot, 0);
	CAPTURE(held);
	REQUIRE(held > 0.99f);

	SUBCASE("the control: the gate re-set to what it already is raises no edge and moves nothing") {
		rack.setGate(true);
		rack.processStereo(l.data(), r.data(), kTick, backplane::unknownBlock());
		CHECK(rack.publishedOutputAt(slot, 0) == doctest::Approx(held).epsilon(0.001));
	}

	SUBCASE("a note announced under the held gate") {
		rack.retrigger();
		rack.processStereo(l.data(), r.data(), kTick, backplane::unknownBlock());
		const float after = rack.publishedOutputAt(slot, 0);
		CAPTURE(after);
		// One tick up a third-of-a-second first segment: about 0.004 from the start level of 0.
		CHECK(after < 0.05f);
	}
}

// ---------------------------------------------------------------------------------------------
// A modulator added as the first device in a new rack crashed the audio engine (seen in Bitwig).
// Reproduced here before anything was changed.

TEST_CASE("a modulator alone in a fresh rack survives a block") {
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	REQUIRE(engine.addDevice(dahdsr::kTraits.tag) != rack::RackEngine::kNoSlot);

	std::vector<float> left(512, 0.0f);
	std::vector<float> right(512, 0.0f);
	backplane::BlockContext block {};
	engine.processStereo(left.data(), right.data(), 512, block);
}

TEST_CASE("a modulator added FIRST, then an audio device, survives a block") {
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	REQUIRE(engine.addDevice(dahdsr::kTraits.tag) != rack::RackEngine::kNoSlot);
	REQUIRE(engine.addDevice(filter::kTraits.tag) != rack::RackEngine::kNoSlot);

	std::vector<float> left(512, 0.0f);
	std::vector<float> right(512, 0.0f);
	backplane::BlockContext block {};
	engine.processStereo(left.data(), right.data(), 512, block);
}

TEST_CASE("a modulator alone, processed before any prepare") {
	// THE ORDER A HOST ACTUALLY USES IS NOT THE ORDER A TEST NATURALLY WRITES. Bitwig activates, then
	// the player adds a device while audio is running — so the device is constructed against whatever
	// sample rate the engine already had, and a rack that only ever sees addDevice-then-prepare has
	// never run the path a hand takes.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	REQUIRE(engine.addDevice(dahdsr::kTraits.tag) != rack::RackEngine::kNoSlot);

	std::vector<float> left(512, 0.0f);
	std::vector<float> right(512, 0.0f);
	backplane::BlockContext block {};
	engine.processStereo(left.data(), right.data(), 512, block);
}

// THE SHAPE THE CRASH ACTUALLY HAD: not a value, a RACE. The editor calls `addDevice` on the UI
// thread while the audio thread is inside `processStereo`, and no single-threaded case can see that.
// **THE CHAIN VERBS, ON THE ROAD THEY ACTUALLY TAKE (AS 2) — rewritten 2026-09-04, not ported.**
//
// Until then these two cases drove `RackEngine::addDevice` and `RackEngine::removeDeviceAtLane` from a
// second thread while blocks ran: the RAW API, which commit 2 did not make safe and does not claim to.
// They were the source of five of the six ThreadSanitizer blocks this suite reports (BS/BV,
// `publishOrder` against `advanceNotes`) and they were exercising a road nothing in the plugin takes.
//
// **THE DIFFERENCE BETWEEN REWRITING AND PORTING IS THE WHOLE POINT.** Through the bridge a verb lands
// at the DRAIN, between blocks, by construction — so "does not tear a slot" and "does not free what a
// block is inside" become claims about a window the design has closed, and a case still asserting them
// would pass with the parking discipline deleted. That is the trap 82e5fc6 named: the tidy way to write
// the case is the vacuous way. So each one below asserts what it NOW proves, as a value.
//
// **AND THE PARKING BRANCH KEEPS ITS COVERAGE, which is the fact that makes the rewrite safe rather
// than a quiet deletion.** "a reused lane inherits nothing, and a block in flight keeps what it started
// with" drives it deterministically through `enterBlockForTest` / `retiredCountForTest` /
// `reapRetiredForTest`, single-threaded and therefore silent. A race is a poor instrument for a branch
// a seam can simply select.
TEST_CASE("a chain verb queued while blocks run is carried by a drain, in the order the hand asked for") {
	// THE SHAPE IS `MEASUREMENT VEHICLE (AS 2)`'s, because it is the shape `process()` has: one thread
	// does `drain()` then the block, and the hand is a second thread that only ever touches the queue.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);
	bridge.primeForTest();

	constexpr int kBlocks = 2000;
	// THE MODULATOR FIRST, kept from the case this replaces: `advanceModulators` runs at the top of
	// every block with no `isAudio()` gate in front of it, so it is the one that meets a block soonest.
	const std::array<const char*, 3> wanted { dahdsr::kTraits.tag, filter::kTraits.tag,
		                                      dahdsr::kTraits.tag };

	std::atomic<bool> stop { false };
	std::atomic<int> blocksRun { 0 };
	// THE LAST BLOCK COUNT THE HAND HAS SEEN — its acknowledgement, and the handshake below waits on it.
	std::atomic<int> handSaw { 0 };
	std::atomic<bool> handDone { false };
	// WRITTEN ONLY BY THE HAND AND READ ONLY AFTER `join()`, which IS the synchronisation. No atomic
	// here, and none pretended: a `std::atomic` around a value the join already orders would be
	// decoration standing where the reasoning should be.
	std::array<int, 3> queuedAtBlock { -1, -1, -1 };
	int queuedCount = 0;

	std::thread hand([&] {
		int seen = 0;
		size_t next = 0;
		while (!stop.load(std::memory_order_relaxed)) {
			const int now = blocksRun.load(std::memory_order_acquire);
			// **ONE VERB PER BLOCK, AND NOT UNTIL A BLOCK HAS RUN.** AS 2's pacing and AS 2's reason: a
			// hand is slower than the drain, and a spin loop measures the ring's capacity instead of the
			// product. The `now == 0` guard is the other half — the case this replaces had a first draft
			// that set `stop` before the other thread ran once and measured a window that never opened.
			if (now == 0 || now == seen) {
				std::this_thread::yield();
				continue;
			}
			seen = now;
			// ACKNOWLEDGED BEFORE THE VERB, so the block thread is released at once and the add below
			// races the next drain and block exactly as a hand's would — the handshake buys the hand its
			// turn, not a quiet moment to queue in.
			handSaw.store(now, std::memory_order_release);
			if (next >= wanted.size()) {
				continue;
			}
			if (bridge.addDevice(wanted[next]) == kNoInstance) {
				break;
			}
			queuedAtBlock[next] = now;
			++next;
			queuedCount = static_cast<int>(next);
			if (next == wanted.size()) {
				handDone.store(true, std::memory_order_release);
			}
		}
	});

	// AND THE BLOCK THREAD, doing exactly what `process()` does: drain, then the block.
	std::vector<float> l(256, 0.0f), r(256, 0.0f);
	int firstBlockAtFull = -1;
	int changedDuringABlock = 0;
	for (int i = 0; i < kBlocks; ++i) {
		bridge.drain();
		const size_t before = engine.chainLength();
		if (before == wanted.size() && firstBlockAtFull < 0) {
			firstBlockAtFull = i;
		}
		engine.processStereo(l.data(), r.data(), 256, backplane::unknownBlock());
		// **THE CHAIN DID NOT MOVE UNDER THE BLOCK.** The assertion the raw API cannot pass and this
		// road passes by construction: a verb reaches the engine only at the drain above, so the chain
		// the block started walking is the chain it finished walking. COUNTED rather than checked in
		// the loop, so a red names how many blocks were walked over rather than only the first.
		if (engine.chainLength() != before) {
			++changedDuringABlock;
		}
		const int ran = blocksRun.fetch_add(1, std::memory_order_release) + 1;
		// **THE HAND GETS ITS TURN BY HANDSHAKE, NOT BY LUCK.** Found by known-bad KB4, which has nothing
		// to do with adds and still turned this case red: on a loaded host the block thread can run the
		// whole loop before the hand is scheduled three times, and `queuedCount` then reads 2. That is a
		// red about the SCHEDULER, which is the worst kind — it names this case while the fault is
		// somewhere else entirely. One yield per block was the first answer and it was not enough
		// (backlog KB: `0 == 3`, 1 in 15 at load ~50 — a yield is a hint the kernel may ignore). So
		// while the hand still has work, each block waits until the hand has SEEN it. The hand acks
		// before it queues, so its verb still lands concurrently with the next drain and block. The wait
		// is bounded, generously, so a hand that is truly gone costs seconds rather than a hang and
		// still reaches the REQUIRE below, which names it.
		if (!handDone.load(std::memory_order_acquire)) {
			const auto giveUp = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			while (handSaw.load(std::memory_order_acquire) < ran && !handDone.load(std::memory_order_acquire)
			       && std::chrono::steady_clock::now() < giveUp) {
				std::this_thread::yield();
			}
		}
	}
	stop.store(true, std::memory_order_relaxed);
	hand.join();
	bridge.drain(); // whatever the hand queued on its last pass

	// BOTH HALVES RAN. A verdict over two threads that never met is a report about nothing, green or
	// silent — the half of the self-check the case this replaces already had.
	REQUIRE(queuedCount == static_cast<int>(wanted.size()));
	for (size_t i = 0; i < wanted.size(); ++i) {
		CAPTURE(i);
		// **THE WINDOW WAS OPEN AT BOTH ENDS**: queued after blocks had started, and before they
		// stopped. `> 0` is what the old case asserted; `< kBlocks` is the half it did not.
		CHECK(queuedAtBlock[i] > 0);
		CHECK(queuedAtBlock[i] < kBlocks);
	}
	// AND NOTHING WAS DROPPED — a ring that refused an edit makes every assertion below a report about
	// a shorter chain than the hand asked for. This is the half that disqualified AS 2's first attempt.
	CHECK(bridge.droppedEditsForTest() == 0);
	// **AND NOTHING IS LEFT WAITING IN A MAILBOX** — every device the record asked for was adopted. The
	// refused-edit count this used to read is gone with the verbs it counted (desired state, in
	// both hosts): an engine that cannot take a device is converged to again, never told a
	// verb it might refuse out of the editor's sight.
	CHECK(bridge.pendingProvisionsForTest() == 0);
	// **AND THEY LANDED DURING THE RUN, NOT ON THE FINAL DRAIN.** Without this the case passes on the
	// tidy-up drain alone, which is the vacuous reading: a queue emptied after the last block proves
	// nothing whatever about a queue emptied between blocks.
	CHECK(firstBlockAtFull > 0);
	CHECK(firstBlockAtFull < kBlocks);
	CHECK(changedDuringABlock == 0);

	// **THE VALUE: the engine holds what the hand asked for, in the order it asked for it.** Not "the
	// rack changed" — CLAUDE.md's rule; a difference check here passes for any three devices in any
	// order, which is exactly the bug worth catching.
	REQUIRE(engine.chainLength() == wanted.size());
	for (size_t at = 0; at < wanted.size(); ++at) {
		CAPTURE(at);
		const size_t lane = engine.deviceAtPosition(at);
		REQUIRE(lane != rack::RackEngine::kNoSlot);
		const device::Descriptor* d = engine.slotAtLane(lane).live_desc();
		REQUIRE(d != nullptr);
		CHECK(std::string(d->traits.tag) == std::string(wanted[at]));
		// AND THE MIRROR AGREES WITH THE ENGINE, which is the read half of the contract: a half-
		// published order is precisely a disagreement between these two. BY IDENTITY — the bridge names
		// a device by its id (kill-lanes step 2), and the engine's slot carries the same one.
		CHECK(bridge.deviceAtPosition(at) == engine.slotAtLane(lane).id);
	}
	CHECK(bridge.chainLength() == engine.chainLength());
}

// THE OTHER HALF OF THE HANDOFF. The case this replaces removed a device from a second thread while
// the audio thread was inside a block that might be holding it, and asserted that nothing crashed. On
// the bridge road that removal CANNOT land inside a block, so the old claim is unfalsifiable here —
// what is left to prove, and what this asserts as a value, is that it never lands inside one.
TEST_CASE("a remove queued while blocks run lands between blocks, so nothing is ever parked") {
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);
	bridge.primeForTest();

	const std::array<DeviceInstanceId, 3> added { bridge.addDevice(filter::kTraits.tag),
		                                          bridge.addDevice(dahdsr::kTraits.tag),
		                                          bridge.addDevice(filter::kTraits.tag) };
	for (const DeviceInstanceId id : added) {
		REQUIRE(id != kNoInstance);
	}
	bridge.drain();
	REQUIRE(engine.chainLength() == 3);
	REQUIRE(engine.retiredCountForTest() == 0);

	constexpr int kBlocks = 2000;
	std::atomic<bool> stop { false };
	std::atomic<int> blocksRun { 0 };
	std::atomic<int> handSaw { 0 };
	std::atomic<bool> handDone { false };
	std::array<int, 3> queuedAtBlock { -1, -1, -1 };
	int queuedCount = 0;

	std::thread hand([&] {
		int seen = 0;
		size_t next = 0;
		while (!stop.load(std::memory_order_relaxed)) {
			const int now = blocksRun.load(std::memory_order_acquire);
			if (now == 0 || now == seen) {
				std::this_thread::yield();
				continue;
			}
			seen = now;
			handSaw.store(now, std::memory_order_release); // before the verb — see the add case
			if (next >= queuedAtBlock.size()) {
				continue;
			}
			// DEVICE BY DEVICE, by the ids the adds answered — never by position, which the removals
			// before this one have moved.
			bridge.removeDevice(added[next]);
			queuedAtBlock[next] = now;
			++next;
			queuedCount = static_cast<int>(next);
			if (next == queuedAtBlock.size()) {
				handDone.store(true, std::memory_order_release);
			}
		}
	});

	std::vector<float> l(256, 0.0f), r(256, 0.0f);
	int firstBlockAtEmpty = -1;
	int changedDuringABlock = 0;
	size_t maxParked = 0;
	for (int i = 0; i < kBlocks; ++i) {
		bridge.drain();
		// **NOTHING WAS EVER PARKED — a SECOND assertion, and the comment here first claimed it was the
		// first. MEASURED and corrected 2026-09-04.** `removeDeviceAtLane` parks instead of destroying
		// when a block is in flight, so the reading "drive the same removal from another thread and
		// this reads 1" is what the code says. It is not what the instrument says: under known-bad KB2
		// — `RackBridge::removeDeviceAtLane` restored to the raw road, calling the engine straight from
		// the hand thread — this stayed **0** while `changedDuringABlock` read 3. The reason is
		// `blockPassed(kRemovalSpins)`, which YIELDS up to 20,000 times waiting for the block to end: a
		// 256-frame block over three cheap devices finishes far inside that, so the removal waits and
		// then destroys rather than parking. Parking needs a block that outlasts the spin, which this
		// case does not produce and should not be lengthened to produce.
		//
		// So it stays as the cheap catch for the long-block case and the assertion that actually
		// detects the defect is `changedDuringABlock` below. Written out rather than deleted because
		// the wrong version of this comment is exactly the reasoning a reader would repeat.
		const size_t parked = engine.retiredCountForTest();
		if (parked > maxParked) {
			maxParked = parked;
		}
		const size_t before = engine.chainLength();
		if (before == 0 && firstBlockAtEmpty < 0) {
			firstBlockAtEmpty = i;
		}
		engine.processStereo(l.data(), r.data(), 256, backplane::unknownBlock());
		if (engine.chainLength() != before) {
			++changedDuringABlock;
		}
		const int ran = blocksRun.fetch_add(1, std::memory_order_release) + 1;
		// A TURN FOR THE HAND while it still has work, BY HANDSHAKE — see the add case for why a yield
		// was not enough.
		if (!handDone.load(std::memory_order_acquire)) {
			const auto giveUp = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			while (handSaw.load(std::memory_order_acquire) < ran && !handDone.load(std::memory_order_acquire)
			       && std::chrono::steady_clock::now() < giveUp) {
				std::this_thread::yield();
			}
		}
	}
	stop.store(true, std::memory_order_relaxed);
	hand.join();
	bridge.drain();

	REQUIRE(queuedCount == static_cast<int>(queuedAtBlock.size()));
	for (size_t i = 0; i < queuedAtBlock.size(); ++i) {
		CAPTURE(i);
		CHECK(queuedAtBlock[i] > 0);
		CHECK(queuedAtBlock[i] < kBlocks);
	}
	CHECK(bridge.droppedEditsForTest() == 0);
	// THE RACK EMPTIED DURING THE RUN rather than on the tidy-up drain — the same anti-vacuity check
	// as the add case, and for the same reason.
	CHECK(firstBlockAtEmpty > 0);
	CHECK(firstBlockAtEmpty < kBlocks);
	CHECK(changedDuringABlock == 0);
	// **THE ONE THAT NAMES THE HAZARD, RESTATED FOR CF.** A remove on the drain now PARKS the device
	// whether or not a block is in flight — destruction frees, and the drain must not — so the parking
	// is non-empty during the run by design; what the hazard needs is that the drain carried no
	// removal INSIDE a block (`changedDuringABlock`, above) and that the message thread's reap
	// empties the parking afterwards. `maxParked > 0` is the anti-vacuity half: the parking was used.
	CHECK(maxParked > 0);

	// AND THE VALUE: every lane is empty, on both sides of the mirror.
	CHECK(engine.chainLength() == 0);
	CHECK(bridge.chainLength() == 0);
	for (size_t lane = 0; lane < engine.slotCount(); ++lane) {
		CAPTURE(lane);
		CHECK(engine.slotAtLane(lane).live_desc() == nullptr);
	}
	for (const DeviceInstanceId id : added) {
		CHECK(bridge.descriptorOf(id) == nullptr);
	}
	bridge.reapRetired();
	CHECK(engine.retiredCountForTest() == 0);
}

TEST_CASE("a device removed and re-added in the same breath is a fresh device") {
	// THE HANDOFF MUST NOT MAKE A SLOT UNUSABLE WHILE ITS OLD OCCUPANT IS PARKED. Retiring the storage
	// leaves the slot empty immediately; a new device gets its own allocation and does not wait for the
	// old one to be reaped.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	REQUIRE(engine.addDevice(filter::kTraits.tag) == 0);
	engine.removeDeviceAtLane(0);
	REQUIRE(engine.addDevice(dahdsr::kTraits.tag) == 0);
	CHECK(engine.slotAtLane(0).isMod());

	std::vector<float> l(64, 0.0f);
	std::vector<float> r(64, 0.0f);
	backplane::BlockContext block {};
	engine.processStereo(l.data(), r.data(), 64, block);
}

// ---------------------------------------------------------------------------------------------
// LIVE READINGS — the road that lets a device's own picture work in the rack.
//
// The seam carries anonymous floats and the device's UI knows what the indices mean; see
// `DeviceHost::readingAt`. These cases are about the CARRIER, not about OWLTT: a host that garbles the
// numbers, keeps them after the device is gone, or reports a count it cannot honour is broken for
// every device that ever publishes, not just the one that does today.

TEST_CASE("a device's readings are carried, and a host with none answers zero") {
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	REQUIRE(engine.addDevice(filter::kTraits.tag) == 0);

	// NOTHING PUBLISHED YET. Zero readings and zero VALUES are the same answer to a caller, which is
	// deliberate: a meter with no data and a device with no meters both draw as nothing.
	CHECK(engine.readingCountAt(0) == 0);
	CHECK(engine.readingAt(0, 0) == doctest::Approx(0.0f));

	const std::array<float, 4> values { -6.0f, -12.0f, 0.5f, 1.0f };
	engine.publishReadings(0, values.data(), values.size());
	REQUIRE(engine.readingCountAt(0) == 4);
	for (size_t i = 0; i < values.size(); ++i) {
		CAPTURE(i);
		CHECK(engine.readingAt(0, i) == doctest::Approx(values[i]));
	}
	// PAST THE COUNT IS ZERO, not a stale cell and not a read past the store.
	CHECK(engine.readingAt(0, 4) == doctest::Approx(0.0f));
	CHECK(engine.readingAt(0, 9999) == doctest::Approx(0.0f));
}

TEST_CASE("readings do not outlive the device that published them") {
	// A METER LEFT STANDING AFTER ITS DEVICE IS REMOVED IS A PICTURE OF SOMETHING THAT IS NOT THERE —
	// and worse, the next device in that slot would inherit it.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	REQUIRE(engine.addDevice(filter::kTraits.tag) == 0);
	const std::array<float, 3> values { 1.0f, 2.0f, 3.0f };
	engine.publishReadings(0, values.data(), values.size());
	REQUIRE(engine.readingCountAt(0) == 3);

	engine.removeDeviceAtLane(0);
	CHECK(engine.readingCountAt(0) == 0);

	REQUIRE(engine.addDevice(dahdsr::kTraits.tag) == 0);
	CHECK(engine.readingCountAt(0) == 0);
	CHECK(engine.readingAt(0, 0) == doctest::Approx(0.0f));
}

TEST_CASE("a device publishing more than the store holds is clamped, and says so") {
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	REQUIRE(engine.addDevice(filter::kTraits.tag) == 0);

	std::vector<float> tooMany(rack::RackEngine::kMaxReadings + 8, 7.0f);
	engine.publishReadings(0, tooMany.data(), tooMany.size());
	// THE COUNT REPORTED IS WHAT WAS STORED, so a caller reading `readingCountAt` cells never reads
	// past the store — a truncation that lied about its length would be the dangerous kind.
	CHECK(engine.readingCountAt(0) == rack::RackEngine::kMaxReadings);
	CHECK(engine.readingAt(0, rack::RackEngine::kMaxReadings - 1) == doctest::Approx(7.0f));
	CHECK(engine.readingAt(0, rack::RackEngine::kMaxReadings) == doctest::Approx(0.0f));
}

TEST_CASE("the connector is told about every device as it is built") {
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	std::vector<std::string> seen;
	std::vector<void*> storages;
	engine.setDspConnector([&](const char* tag, size_t, void* dsp) {
		seen.emplace_back(tag);
		storages.push_back(dsp);
	});

	REQUIRE(engine.addDevice(filter::kTraits.tag) != rack::RackEngine::kNoSlot);
	REQUIRE(engine.addDevice(dahdsr::kTraits.tag) != rack::RackEngine::kNoSlot);

	REQUIRE(seen.size() == 2);
	CHECK(seen[0] == std::string(filter::kTraits.tag));
	CHECK(seen[1] == std::string(dahdsr::kTraits.tag));
	// AND IT GOT REAL STORAGE, not a null it would have to guard: the connector's whole purpose is
	// reaching into the device, so being handed nothing would make it silently useless.
	for (void* s : storages) {
		CHECK(s != nullptr);
	}
}

// ---------------------------------------------------------------------------------------------
// THE NOTE LANE, AND ITS FIRST CALLER.
//
// Note input exists for pitch-tracking applications, and the design gives every device every note
// and lets each device decide what to do with them. So the host reports no pitch at all, and the
// tracker decides — which is what these cases
// assert. THE VALUE IS READ DIRECTLY rather than inferred from a rendered buffer: this repo's rule is
// that a claim about a value cannot be recovered from a signal that carries every value at once.

namespace trk = feathers::mod::tracker;

TEST_CASE("notes reach a device that declares NoteOps, and pass through it unchanged") {
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	const size_t slot = engine.addDevice(trk::kTraits.tag);
	if (slot == rack::RackEngine::kNoSlot) {
		// **THE TRACKER IS NOT IN `kDevices` AND THIS CASE SAYS SO RATHER THAN VANISHING.**
		//
		// Registering it broke Feathers: `ModPanel::CardKind` has two real cards and no generic
		// fallback, so every device in the shared catalogue that publishes an output obliges a
		// hand-written card in the modulator strip — and a tracker's card there would be a surface for
		// a device that cannot move, because Feathers' strip has no note road. That is a design
		// conversation, recorded in `device_registry.h`, and the registration is held until it is had.
		//
		// SO THE ENGINE'S NOTE LANE HAS NO END-TO-END COVERAGE UNTIL A NOTE DEVICE IS REGISTERED. The
		// tracker's own behaviour is covered directly below — the rule, the ordering, the repeated key
		// — but "a device in a slot receives the block's notes" is asserted by this case and nothing
		// else. **A skipped case that explains itself is worth more than a deleted one**, because the
		// gap is temporary and the next person needs to know it is a gap rather than a decision.
		MESSAGE("SKIPPED: no note device is registered — see device_registry.h on the held decision");
		return;
	}

	std::array<device::NoteEvent, 2> in {};
	in[0] = { device::NoteEvent::Kind::On, 0, 7, 0, 0, 60, 1.0 };
	in[1] = { device::NoteEvent::Kind::On, 0, 8, 0, 0, 64, 1.0 };
	engine.setBlockNotes(in.data(), in.size());

	std::vector<float> l(64, 0.0f);
	std::vector<float> r(64, 0.0f);
	backplane::BlockContext block {};
	engine.processStereo(l.data(), r.data(), 64, block);

	// PASS-THROUGH: what went in comes out, ids and all. The model — every note is passed to the
	// device output — is what makes the rack's note port honest at all times.
	const auto out = engine.blockNotesOut();
	REQUIRE(out.size() == 2);
	CHECK(out[0].key == 60);
	CHECK(out[1].key == 64);
	CHECK(out[0].noteId == 7);
	CHECK(out[1].noteId == 8);
}

// ---------------------------------------------------------------------------------------------
// A DEVICE THAT IS BOTH AUDIO AND NOTES — the road WEAVE rides (tests/audio_notes_probe.h).
//
// THE RACK PLUGIN'S OWN SEQUENCE, spelled as `rack_clap.cpp`'s `process` spells it: every note the host
// sent this block is collected with its header time as `timeSamples`, handed over by `setBlockNotes`,
// and the whole host block goes to `processStereo`. The plugin's catalogue is the build's, so a probe
// cannot be seated through CLAP; this is the engine the plugin drives, driven the same way, and
// test_rack_clap.cpp covers the plugin's translation of a note on each side of it.

namespace {

const device::Descriptor* findWithAudioNotesProbe(void*, const char* tag) {
	if (device::sameTag(test::kAudioNotesProbe.traits.tag, tag)) {
		return &test::kAudioNotesProbe;
	}
	return deviceForTag(tag);
}
const device::Catalogue kWithAudioNotesProbe { nullptr, &findWithAudioNotesProbe };

/// THE PROBE'S LOG, READ AS "WHICH NOTE, AFTER HOW MUCH AUDIO" — the frames the device had been handed
/// in this log before each note arrived, which is where in the host block the note was offered.
struct PieceReading {
	std::vector<test::AudioNotesProbe::Entry> notes;
	std::vector<int> framesBefore;
	int frames = 0;
	int pieces = 0;
	int firstPieceFrames = 0;
};
PieceReading readPieces(const test::AudioNotesProbe& probe) {
	PieceReading r;
	for (size_t i = 0; i < probe.count; ++i) {
		const test::AudioNotesProbe::Entry& e = probe.log[i];
		if (e.what == test::AudioNotesProbe::Entry::What::Audio) {
			if (r.pieces == 0) {
				r.firstPieceFrames = e.frames;
			}
			++r.pieces;
			r.frames += e.frames;
		}
		else {
			r.notes.push_back(e);
			r.framesBefore.push_back(r.frames);
		}
	}
	return r;
}

} // namespace

TEST_CASE("rack: an audio+notes device in the rack plugin's engine receives the block's note events") {
	rack::RackEngine engine { rack::kRackAxes, kWithAudioNotesProbe };
	engine.prepare(48000.0, 4096);
	REQUIRE(engine.addDevice(test::kAudioNotesProbe.traits.tag) != rack::RackEngine::kNoSlot);
	test::AudioNotesProbe* probe = test::g_audioNotesProbe;
	REQUIRE(probe != nullptr);
	probe->clear();

	std::array<device::NoteEvent, 2> in {};
	in[0] = { device::NoteEvent::Kind::On, 100, 7, 0, 0, 60, 0.625 };
	in[1] = { device::NoteEvent::Kind::Off, 300, 7, 0, 0, 60, 0.25 };
	engine.setBlockNotes(in.data(), in.size());
	std::vector<float> l = tone(512);
	std::vector<float> r = tone(512);
	engine.processStereo(l.data(), r.data(), 512, backplane::BlockContext {});

	// BOTH EVENTS, WITH EVERY FIELD WEAVE READS — the kind, the key and the VELOCITY, which is what the
	// one-pitch note context cannot carry.
	std::vector<test::AudioNotesProbe::Entry> notes;
	for (size_t i = 0; i < probe->count; ++i) {
		if (probe->log[i].what == test::AudioNotesProbe::Entry::What::Note) {
			notes.push_back(probe->log[i]);
		}
	}
	// DELIVERED ONCE: the block is cut into eight 64-sample pieces and each note is offered only in the
	// piece that holds it, so a road that re-offered them per piece would read 16 here.
	REQUIRE(probe->dropped == 0);
	REQUIRE(notes.size() == 2);
	CHECK(notes[0].kind == device::NoteEvent::Kind::On);
	CHECK(notes[0].key == 60);
	CHECK(notes[0].velocity == 0.625);
	CHECK(notes[1].kind == device::NoteEvent::Kind::Off);
	CHECK(notes[1].velocity == 0.25);
	// AND PASSED ON, which is what the plugin's note port emits.
	const auto out = engine.blockNotesOut();
	REQUIRE(out.size() == 2);
	CHECK(out[0].key == 60);
	CHECK(out[1].kind == device::NoteEvent::Kind::Off);

	// THE NEXT BLOCK, WITH NOTHING SENT, HANDS THE DEVICE NOTHING — `rack_clap` empties its collector
	// after the block, and the engine must not replay what it was given.
	probe->clear();
	engine.setBlockNotes(nullptr, 0);
	engine.processStereo(l.data(), r.data(), 512, backplane::BlockContext {});
	for (size_t i = 0; i < probe->count; ++i) {
		CHECK(probe->log[i].what == test::AudioNotesProbe::Entry::What::Audio);
	}
}

TEST_CASE("rack: an audio+notes device's notes land before its audio in the same block, at their sample offsets") {
	// **EACH NOTE IS OFFERED IN THE 64-FRAME PIECE THAT HOLDS IT, AT ITS OFFSET IN THAT TICK** — the
	// meaning `timeSamples` has in Feathers' lane (`Synth::noteOffsetInTick`), so a device reads it the
	// same way in both hosts. `processStereo` cuts the host block on the control tick (backlog FK); the
	// notes used to be handed over whole in the first piece, at host-block offsets, which put a note at
	// 300 four ticks early for every device that acts on the tick it is told in. Asserted as values so
	// WEAVE's author can read the contract here.
	rack::RackEngine engine { rack::kRackAxes, kWithAudioNotesProbe };
	engine.prepare(48000.0, 4096);
	REQUIRE(engine.addDevice(test::kAudioNotesProbe.traits.tag) != rack::RackEngine::kNoSlot);
	test::AudioNotesProbe* probe = test::g_audioNotesProbe;
	REQUIRE(probe != nullptr);
	probe->clear();

	std::array<device::NoteEvent, 3> in {};
	in[0] = { device::NoteEvent::Kind::On, 0, 1, 0, 0, 48, 1.0 };
	in[1] = { device::NoteEvent::Kind::On, 130, 2, 0, 0, 55, 0.5 };
	in[2] = { device::NoteEvent::Kind::Off, 411, 1, 0, 0, 48, 0.0 };
	engine.setBlockNotes(in.data(), in.size());
	std::vector<float> l = tone(512);
	std::vector<float> r = tone(512);
	engine.processStereo(l.data(), r.data(), 512, backplane::BlockContext {});

	REQUIRE(probe->dropped == 0);
	const PieceReading seen = readPieces(*probe);
	REQUIRE(seen.notes.size() == 3);
	// 130 IS PIECE 2 (frames 128..191) AT OFFSET 2; 411 IS PIECE 6 (384..447) AT OFFSET 27.
	CHECK(seen.notes[0].key == 48);
	CHECK(seen.notes[0].timeSamples == 0);
	CHECK(seen.framesBefore[0] == 0);
	CHECK(seen.notes[1].key == 55);
	CHECK(seen.notes[1].timeSamples == 2);
	CHECK(seen.framesBefore[1] == 128);
	CHECK(seen.notes[2].kind == device::NoteEvent::Kind::Off);
	CHECK(seen.notes[2].timeSamples == 27);
	CHECK(seen.framesBefore[2] == 384);
	// THE AUDIO, EIGHT PIECES OF ONE TICK EACH.
	CHECK(seen.pieces == 8);
	CHECK(seen.frames == 512);

	// **AND WHAT LEAVES THE CHAIN IS IN HOST-BLOCK TIME AGAIN**, so `emitNotes` hands the host back the
	// offsets it sent: the rebase is the device's view, not the port's.
	const auto out = engine.blockNotesOut();
	REQUIRE(out.size() == 3);
	CHECK(out[0].timeSamples == 0);
	CHECK(out[1].timeSamples == 130);
	CHECK(out[2].timeSamples == 411);
	CHECK(out[0].key == 48);
	CHECK(out[1].key == 55);
	CHECK(out[2].kind == device::NoteEvent::Kind::Off);
}

TEST_CASE("rack: a note on a piece boundary opens the later piece, and a note past the block clamps to its last") {
	rack::RackEngine engine { rack::kRackAxes, kWithAudioNotesProbe };
	engine.prepare(48000.0, 4096);
	REQUIRE(engine.addDevice(test::kAudioNotesProbe.traits.tag) != rack::RackEngine::kNoSlot);
	test::AudioNotesProbe* probe = test::g_audioNotesProbe;
	REQUIRE(probe != nullptr);
	probe->clear();

	std::array<device::NoteEvent, 3> in {};
	in[0] = { device::NoteEvent::Kind::On, 63, 1, 0, 0, 60, 1.0 };
	in[1] = { device::NoteEvent::Kind::On, 64, 2, 0, 0, 62, 1.0 };
	// PAST THE BLOCK — which CLAP forbids and a host may still send. It is not dropped: a lost off is
	// a stuck note. It plays at the block's last sample.
	in[2] = { device::NoteEvent::Kind::Off, 700, 1, 0, 0, 60, 0.0 };
	engine.setBlockNotes(in.data(), in.size());
	std::vector<float> l = tone(512);
	std::vector<float> r = tone(512);
	engine.processStereo(l.data(), r.data(), 512, backplane::BlockContext {});

	const PieceReading seen = readPieces(*probe);
	REQUIRE(seen.notes.size() == 3);
	CHECK(seen.framesBefore[0] == 0);
	CHECK(seen.notes[0].timeSamples == 63);
	CHECK(seen.framesBefore[1] == 64);
	CHECK(seen.notes[1].timeSamples == 0);
	CHECK(seen.framesBefore[2] == 448);
	CHECK(seen.notes[2].timeSamples == 63);
	// ORDER KEPT: the host's order is the stream's order, piece by piece.
	CHECK(seen.notes[0].key == 60);
	CHECK(seen.notes[1].key == 62);
}

TEST_CASE("rack: a block that does not start on a tick offers each note in the tick that holds it") {
	// **THE PIECE GRID CARRIES ACROSS HOST BLOCKS.** A 100-frame block leaves the engine 36 frames into
	// a tick, so the next block's first piece is 28 frames long and its second starts at 28. A note at
	// 10 is in that short first piece, 46 into its tick; a note at 30 is 2 into the next one.
	rack::RackEngine engine { rack::kRackAxes, kWithAudioNotesProbe };
	engine.prepare(48000.0, 4096);
	REQUIRE(engine.addDevice(test::kAudioNotesProbe.traits.tag) != rack::RackEngine::kNoSlot);
	test::AudioNotesProbe* probe = test::g_audioNotesProbe;
	REQUIRE(probe != nullptr);
	std::vector<float> l = tone(512);
	std::vector<float> r = tone(512);
	engine.setBlockNotes(nullptr, 0);
	engine.processStereo(l.data(), r.data(), 100, backplane::BlockContext {});
	probe->clear();

	std::array<device::NoteEvent, 2> in {};
	in[0] = { device::NoteEvent::Kind::On, 10, 1, 0, 0, 60, 1.0 };
	in[1] = { device::NoteEvent::Kind::On, 30, 2, 0, 0, 64, 1.0 };
	engine.setBlockNotes(in.data(), in.size());
	engine.processStereo(l.data(), r.data(), 512, backplane::BlockContext {});

	const PieceReading seen = readPieces(*probe);
	REQUIRE(seen.notes.size() == 2);
	CHECK(seen.framesBefore[0] == 0);
	CHECK(seen.notes[0].timeSamples == 46);
	CHECK(seen.framesBefore[1] == 28);
	CHECK(seen.notes[1].timeSamples == 2);
	REQUIRE(seen.firstPieceFrames == 28);
	const auto out = engine.blockNotesOut();
	REQUIRE(out.size() == 2);
	CHECK(out[0].timeSamples == 10);
	CHECK(out[1].timeSamples == 30);
}

TEST_CASE("rack: an envelope's note opens in the tick the note falls in, not at the block's start") {
	// **THE LIVE DEFECT THIS FIXES, MEASURED ON A REGISTERED DEVICE.** DAHDSR starts a note's envelope
	// on the tick it is told of it (`OpsEnvelope::processNotes` then `tick`), and reads no offset. When
	// the whole block's notes arrived in the first piece, a note at 300 of a 512 block started at 0.
	//
	// THE REFERENCE IS AN INDEPENDENT RESTATEMENT: the same note sent in a block that STARTS at the tick
	// holding it — 256 frames of nothing, then a note at 44 — is the note on the tick it belongs to.
	// The early reading is the control: a note at 0 must NOT match.
	const auto run = [](const std::vector<std::pair<int, int>>& blocks) {
		rack::RackEngine engine { rack::kRackAxes, hostCatalogue() };
		engine.prepare(48000.0, 4096);
		const size_t slot = engine.addDevice(dahdsr::kTraits.tag);
		REQUIRE(slot != rack::RackEngine::kNoSlot);
		std::vector<float> l(512, 0.0f), r(512, 0.0f);
		for (const auto& [frames, noteAt] : blocks) {
			device::NoteEvent on { device::NoteEvent::Kind::On, 0, 1, 0, 0, 60, 1.0 };
			on.timeSamples = static_cast<uint32_t>(noteAt);
			engine.setBlockNotes(&on, noteAt >= 0 ? 1 : 0);
			engine.processStereo(l.data(), r.data(), frames, backplane::unknownBlock());
		}
		return engine.outputForNoteAt(slot, 0, 1, 0, 60);
	};
	const float at300 = run({ { 512, 300 } });
	const float reference = run({ { 256, -1 }, { 256, 44 } });
	const float early = run({ { 512, 0 } });
	CAPTURE(at300);
	CAPTURE(reference);
	CAPTURE(early);
	REQUIRE(reference > 0.0f);
	CHECK(at300 == doctest::Approx(reference).epsilon(1e-6));
	CHECK(early != doctest::Approx(reference).epsilon(1e-3)); // the control: the instrument can tell
}

TEST_CASE("rack: a note held across blocks of different sizes releases, and the note context says so") {
	// **THE HELD STACK FOLDS EACH NOTE ONCE.** `updateNoteContext` runs in every piece and reads what
	// left the chain; while the whole block's notes stood there for all eight pieces, a note-on was
	// pushed eight times. A release in a block of one piece popped one of them, and the gate every
	// `setNoteContext` device reads stayed up for good.
	rack::RackEngine engine { rack::kRackAxes, kWithAudioNotesProbe };
	engine.prepare(48000.0, 4096);
	REQUIRE(engine.addDevice(test::kAudioNotesProbe.traits.tag) != rack::RackEngine::kNoSlot);
	test::AudioNotesProbe* probe = test::g_audioNotesProbe;
	REQUIRE(probe != nullptr);
	std::vector<float> l = tone(512);
	std::vector<float> r = tone(512);

	device::NoteEvent on { device::NoteEvent::Kind::On, 100, 1, 0, 0, 60, 1.0 };
	engine.setBlockNotes(&on, 1);
	engine.processStereo(l.data(), r.data(), 512, backplane::BlockContext {});
	REQUIRE(probe->lastContext.gate);

	device::NoteEvent off { device::NoteEvent::Kind::Off, 10, 1, 0, 0, 60, 0.0 };
	engine.setBlockNotes(&off, 1);
	engine.processStereo(l.data(), r.data(), 64, backplane::BlockContext {});
	engine.setBlockNotes(nullptr, 0);
	engine.processStereo(l.data(), r.data(), 64, backplane::BlockContext {});
	CHECK_FALSE(probe->lastContext.gate);
}

TEST_CASE("HeldKeys — the Tracker's stack, which the rack's note picks by too: once per key, wildcard, expressions lift nothing, a full stack drops its oldest") {
	using K = device::NoteEvent::Kind;
	const auto ev = [](K kind, int16_t key) {
		device::NoteEvent e {};
		e.kind = kind;
		e.key = key;
		return e;
	};
	trk::HeldKeys held;

	SUBCASE("a repeated key is one entry, so one off lifts it") {
		const std::array<device::NoteEvent, 3> in { ev(K::On, 60), ev(K::On, 60), ev(K::Off, 60) };
		CHECK(held.fold(in.data(), in.size()));
		CHECK(held.empty());
	}
	SUBCASE("an expression lifts no key") {
		const std::array<device::NoteEvent, 2> in { ev(K::On, 60), ev(K::Expression, 60) };
		held.fold(in.data(), in.size());
		CHECK(held.size() == 1);
	}
	SUBCASE("an off or choke with a negative key is CLAP's wildcard and lifts them all") {
		const std::array<device::NoteEvent, 4> in { ev(K::On, 60), ev(K::On, 64), ev(K::On, 67), ev(K::Choke, -1) };
		held.fold(in.data(), in.size());
		CHECK(held.empty());
	}
	SUBCASE("a full stack drops its OLDEST key, so Last is the note just played") {
		for (int16_t k = 0; k < static_cast<int16_t>(trk::HeldKeys::kMax) + 3; ++k) {
			const device::NoteEvent on = ev(K::On, k);
			held.fold(&on, 1);
		}
		CHECK(held.size() == trk::HeldKeys::kMax);
		CHECK(held.pick(trk::Rule::Last) == static_cast<int16_t>(trk::HeldKeys::kMax) + 2);
		CHECK(held.pick(trk::Rule::Lowest) == 3); // 0, 1 and 2 were the oldest
		CHECK(held.pick(trk::Rule::Highest) == static_cast<int16_t>(trk::HeldKeys::kMax) + 2);
	}
	SUBCASE("only an On reports a strike") {
		const std::array<device::NoteEvent, 1> off { ev(K::Off, 60) };
		CHECK_FALSE(held.fold(off.data(), off.size()));
	}
}

TEST_CASE("the tracker chooses its own pitch, and the rule is its control not the host's") {
	trk::Tracker t;
	t.prepare(48000.0);

	// A CHORD: 60 then 55 then 67. Every rule sees the same three notes and answers differently, which
	// is precisely why the host must not choose.
	std::array<device::NoteEvent, 3> in {};
	in[0] = { device::NoteEvent::Kind::On, 0, -1, 0, 0, 60, 1.0 };
	in[1] = { device::NoteEvent::Kind::On, 0, -1, 0, 0, 55, 1.0 };
	in[2] = { device::NoteEvent::Kind::On, 0, -1, 0, 0, 67, 1.0 };
	std::array<device::NoteEvent, 8> out {};
	size_t produced = 0;

	const auto fields = [](trk::Rule rule) {
		return std::array<float, 4> { static_cast<float>(static_cast<int>(rule)), 36.0f, 84.0f, 0.0f };
	};

	auto last = fields(trk::Rule::Last);
	t.setFields(last.data(), last.size());
	t.processNotes(in.data(), in.size(), out.data(), out.size(), &produced);
	CHECK(produced == 3);
	CHECK(t.heldCountForTest() == 3);
	CHECK(t.heldPitchForTest() == doctest::Approx(67.0f)); // most recent down

	// THE RULE CHANGES UNDER HELD NOTES AND THE ANSWER FOLLOWS. This is why the device keeps the STACK
	// rather than a pitch: whoever throws the notes away decides for everybody after them, which is the
	// same mistake the host was making before the correction.
	auto lowest = fields(trk::Rule::Lowest);
	t.setFields(lowest.data(), lowest.size());
	CHECK(t.heldPitchForTest() == doctest::Approx(55.0f));

	auto highest = fields(trk::Rule::Highest);
	t.setFields(highest.data(), highest.size());
	CHECK(t.heldPitchForTest() == doctest::Approx(67.0f));
}

TEST_CASE("releasing the middle of a chord leaves the rest in arrival order") {
	trk::Tracker t;
	t.prepare(48000.0);
	std::array<float, 4> fields { 0.0f, 36.0f, 84.0f, 0.0f }; // Rule::Last
	t.setFields(fields.data(), fields.size());

	// FOUR NOTES, AND THE ONE RELEASED IS THE OLDEST — WHICH IS THE ONLY SHAPE THAT DISCRIMINATES.
	//
	// MEASURED: the first draft held three and released the MIDDLE one, and a known-bad implementing
	// swap-with-back PASSED IT. With [60,64,67] removing 64, the swap moves 67 into the gap and the
	// order is [60,67] either way — the two implementations agree by coincidence. Releasing the oldest
	// of four is where they part: remove-by-value gives [64,67,72] and swap-with-back gives
	// [72,64,67], whose last element is 67 rather than 72.
	//
	// A CASE THAT CANNOT TELL TWO IMPLEMENTATIONS APART IS NOT A WEAK TEST, IT IS NOT A TEST OF THAT
	// THING AT ALL — the same rule this repo applies to venues, arriving in an array index.
	std::array<device::NoteEvent, 4> down {};
	down[0] = { device::NoteEvent::Kind::On, 0, -1, 0, 0, 60, 1.0 };
	down[1] = { device::NoteEvent::Kind::On, 0, -1, 0, 0, 64, 1.0 };
	down[2] = { device::NoteEvent::Kind::On, 0, -1, 0, 0, 67, 1.0 };
	down[3] = { device::NoteEvent::Kind::On, 0, -1, 0, 0, 72, 1.0 };
	std::array<device::NoteEvent, 8> out {};
	size_t produced = 0;
	t.processNotes(down.data(), down.size(), out.data(), out.size(), &produced);
	REQUIRE(t.heldPitchForTest() == doctest::Approx(72.0f));

	std::array<device::NoteEvent, 1> up {};
	up[0] = { device::NoteEvent::Kind::Off, 0, -1, 0, 0, 60, 0.0 };
	t.processNotes(up.data(), up.size(), out.data(), out.size(), &produced);
	CHECK(t.heldCountForTest() == 3);
	// STILL THE NEWEST NOTE DOWN. Under swap-with-back this reads 67.
	CHECK(t.heldPitchForTest() == doctest::Approx(72.0f));

	// AND RELEASING THE TOP FALLS BACK TO THE ONE BENEATH IT, not to silence.
	up[0].key = 72;
	t.processNotes(up.data(), up.size(), out.data(), out.size(), &produced);
	CHECK(t.heldCountForTest() == 2);
	CHECK(t.heldPitchForTest() == doctest::Approx(67.0f));
}

TEST_CASE("a repeated key is one held note, so one release frees it") {
	// A host that sends two ons for one key — or two sources in a chain — would otherwise need two
	// offs, and the second never comes: a note held forever that no hand can release.
	trk::Tracker t;
	t.prepare(48000.0);
	std::array<float, 4> fields { 0.0f, 36.0f, 84.0f, 0.0f };
	t.setFields(fields.data(), fields.size());

	std::array<device::NoteEvent, 2> twice {};
	twice[0] = { device::NoteEvent::Kind::On, 0, -1, 0, 0, 72, 1.0 };
	twice[1] = { device::NoteEvent::Kind::On, 0, -1, 0, 0, 72, 1.0 };
	std::array<device::NoteEvent, 8> out {};
	size_t produced = 0;
	t.processNotes(twice.data(), twice.size(), out.data(), out.size(), &produced);
	CHECK(t.heldCountForTest() == 1);

	std::array<device::NoteEvent, 1> up {};
	up[0] = { device::NoteEvent::Kind::Off, 0, -1, 0, 0, 72, 0.0 };
	t.processNotes(up.data(), up.size(), out.data(), out.size(), &produced);
	CHECK(t.heldCountForTest() == 0);
}

// ---------------------------------------------------------------------------------------------
// WHO PUBLISHES A LIVE READING INTO THE RACK.
//
// THE SYMPTOM: OWLTT was fixed but Automod was not — after the rack grew a tick road, OWLTT's analyser moved
// and automod's curves still did not. **THE TICK WAS NECESSARY AND NOT SUFFICIENT.**
// `AutomodStrip::update()` redraws only when the phase it reads DIFFERS from the one it drew, and
// nothing in the rack published a phase, so `readingCountAt` answered 0, `phase()` returned a
// constant 0.0f, and the card was CORRECT to report that nothing had changed.
//
// **THE WIRING WAS UNTESTABLE WHERE IT LIVED**, inside `RackPlugin`'s constructor in `rack_clap.cpp`,
// whose only suite cannot run headless. That is why one device could be wired and the other not for
// as long as it took a person to notice a still picture. It now lives in `device_publishers.cpp` and
// these cases are the thing that could not previously be written.
// ---------------------------------------------------------------------------------------------

TEST_CASE("an automod in the rack publishes its phase, and the phase actually moves") {
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	rack::installDevicePublishers(engine);
	const size_t slot = engine.addDevice(amd::kTraits.tag);
	REQUIRE(slot != rack::RackEngine::kNoSlot);

	// NOTHING HAS RUN YET, so there is nothing to have published. Asserting this first is what stops
	// the case passing on a store that was never written by the road under test.
	CHECK(engine.readingCountAt(slot) == 0);

	std::vector<float> left(512, 0.0f);
	std::vector<float> right(512, 0.0f);
	backplane::BlockContext block {};
	engine.processStereo(left.data(), right.data(), 512, block);

	// THE READINGS EXIST. This alone is what the missing arm cost.
	//
	// TWO NOW, NOT ONE: [0] the LFO phase, [1] the envelope follower's level. The count is asserted
	// EXACTLY because it is this device's published contract and a surface indexes into it — but note
	// that CONSUMERS must guard with `>` and never `==`, which is what lets a device publish more
	// later without breaking them (`automod_strip.cpp` guards `> 0` and `> 1`; OWLTT's graph guards
	// `< 10`). This equality is the contract's own case; it is not the shape to copy at a call site.
	REQUIRE(engine.readingCountAt(slot) == 2);
	const float first = engine.readingAt(slot, 0);
	CHECK(std::isfinite(first));

	// THE ENVELOPE IS A MAGNITUDE AND IS CLAMPED, NOT WRAPPED — the opposite guard from the phase
	// below, and deliberately so: wrapping a level of 1.2 would draw a full meter as nearly empty.
	const float envelope = engine.readingAt(slot, 1);
	CHECK(std::isfinite(envelope));
	CHECK(envelope >= 0.0f);
	CHECK(envelope <= 1.0f);
	// IN [0, 1), because the publication point wraps — the strip multiplies this by a width, so a
	// value outside the circle would draw off the end of the plot rather than merely look wrong.
	CHECK(first >= 0.0f);
	CHECK(first < 1.0f);

	// AND IT MOVES, WHICH IS THE WHOLE COMPLAINT. A reading that exists but never changes would leave
	// `update()` correctly answering false for ever — exactly the state that was on screen. Several
	// blocks, because one block at 512 frames may not move a slow LFO's phase far enough to matter,
	// and the comparison is bitwise by design (`AutomodStrip::update` argues against a tolerance).
	bool moved = false;
	for (int i = 0; i < 64 && !moved; ++i) {
		engine.processStereo(left.data(), right.data(), 512, block);
		moved = engine.readingAt(slot, 0) != first;
	}
	CHECK(moved);
}

TEST_CASE("a device with nothing to publish stays silent, and does not borrow another's readings") {
	// THE OTHER HALF OF THE LIST: a connector that wired everything would be as wrong as one that
	// wired too little, and a slot reporting readings it never published would draw a neighbour's
	// numbers. `filter` publishes nothing.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	rack::installDevicePublishers(engine);
	const size_t slot = engine.addDevice(filter::kTraits.tag);
	REQUIRE(slot != rack::RackEngine::kNoSlot);

	std::vector<float> left(512, 0.0f);
	std::vector<float> right(512, 0.0f);
	backplane::BlockContext block {};
	engine.processStereo(left.data(), right.data(), 512, block);

	CHECK(engine.readingCountAt(slot) == 0);
	CHECK(engine.readingAt(slot, 0) == 0.0f);
}

TEST_CASE("a modulator in the rack reports where it is, and the position moves") {
	// THE SYMPTOM: the modulators did not show their phase star. The rack DECLINED the playhead
	// because nothing published one — honest, and incomplete. `ModOps::playhead` is the road; this is
	// the case that says it carries something real rather than merely existing.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	const size_t slot = engine.addDevice(dahdsr::kTraits.tag);
	REQUIRE(slot != rack::RackEngine::kNoSlot);

	uint32_t ordinal = 123u;
	float phase = -1.0f;
	// IT ANSWERS AT ALL — this is what was missing.
	REQUIRE(engine.playheadAt(slot, ordinal, phase));

	std::vector<float> left(512, 0.0f);
	std::vector<float> right(512, 0.0f);
	backplane::BlockContext block {};
	// THE GATE IS THE ENGINE'S, NOT THE BLOCK'S — `setGate` raises it and latches a retrigger, which
	// is what starts the envelope. A modulator with no gate sits at idle for ever and the case would
	// be asserting that a stopped thing does not move.
	engine.setGate(true);
	engine.processStereo(left.data(), right.data(), 512, block);

	uint32_t firstOrdinal = 0;
	float firstPhase = 0.0f;
	REQUIRE(engine.playheadAt(slot, firstOrdinal, firstPhase));

	// **AND IT MOVES.** A playhead that answered but never changed would draw a star frozen in place,
	// which is a picture of a stopped modulator — indistinguishable from the bug, and the reason
	// "it answers" is the wrong assertion on its own.
	bool moved = false;
	for (int i = 0; i < 64 && !moved; ++i) {
		engine.processStereo(left.data(), right.data(), 512, block);
		uint32_t o = 0;
		float p = 0.0f;
		if (engine.playheadAt(slot, o, p)) {
			moved = (o != firstOrdinal) || (p != firstPhase);
		}
	}
	CHECK(moved);
}

TEST_CASE("a device with no shape to be part-way through declines, leaving the seed untouched") {
	// A DECLINE MUST NOT WRITE. Ordinal 0 is `Stage::Idle` for a DAHDSR but a VALID SEGMENT for an
	// MSEG, so a caller seeds idle and trusts the seed — a host that wrote zeros while returning
	// false would draw an MSEG's marker at the start of segment 0.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	const size_t slot = engine.addDevice(filter::kTraits.tag);
	REQUIRE(slot != rack::RackEngine::kNoSlot);

	uint32_t ordinal = 77u;
	float phase = -2.0f;
	CHECK_FALSE(engine.playheadAt(slot, ordinal, phase));
	CHECK(ordinal == 77u);
	CHECK(phase == doctest::Approx(-2.0f));
}

TEST_CASE("harm in the rack is silent until enabled, and audible after — the switch reaches the dsp") {
	// THE SYMPTOM: HARM produced no output in a chain, while it worked in Feathers.
	//
	// **HARM SHIPS OFF.** `kParamDescs[Enabled].def` is 0.0 and its own note says why: *"the default
	// is a hard bypass rather than a unity-gain pass through the device: processStereo returns without
	// touching the buffer. An fx chain holding a switched-off HARM has to render the same bits it
	// rendered before the device was added."* So silence from a freshly added HARM is CORRECT.
	//
	// **WHAT IS WORTH ASSERTING IS THE OTHER HALF** — that the card's switch actually reaches the DSP
	// in this host. "It is off by default" explains the report only if turning it on changes the
	// audio; if it did not, the default would be hiding a dead control, which is exactly the shape of
	// the dead enable switch this repo has already found once.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	const size_t slot = engine.addDevice(harm::kTraits.tag);
	REQUIRE(slot != rack::RackEngine::kNoSlot);

	const auto render = [&](bool enabled) {
		const size_t enableControl = static_cast<size_t>(harm::Param::Enabled);
		engine.setField(slot, enableControl, enabled ? 1.0f : 0.0f);
		std::vector<float> left(256, 0.0f);
		std::vector<float> right(256, 0.0f);
		// A SIGNAL TO ACT ON. Silence in gives silence out however the device is set, which would make
		// both halves of this case pass for the wrong reason.
		for (size_t i = 0; i < left.size(); ++i) {
			const auto phase = static_cast<float>(i) * 0.05f;
			left[i] = std::sin(phase);
			right[i] = std::sin(phase);
		}
		const std::vector<float> before = left;
		backplane::BlockContext block {};
		engine.setGate(true);
		engine.processStereo(left.data(), right.data(), static_cast<int>(left.size()), block);
		bool moved = false;
		for (size_t i = 0; i < left.size(); ++i) {
			moved = moved || left[i] != before[i];
		}
		return moved;
	};

	// OFF: THE BUFFER IS RETURNED UNTOUCHED, which is the identity guarantee its descriptor promises.
	CHECK_FALSE(render(false));
	// ON: THE SWITCH REACHES THE DSP. Without this the case would prove only that a disabled device is
	// quiet, which is not the question.
	CHECK(render(true));
}

TEST_CASE("a pitched device in the rack is given the note — harm is silent without one") {
	// **THE EARLIER CASE WAS NOT ENOUGH.** HARM stayed silent in a chain with the device enabled and
	// the volume up, which suggested something upstream was eating notes. Nothing upstream was
	// EATING notes; nothing was DELIVERING
	// them. `AudioOps::setNoteContext` was called in exactly one place in the tree — Feathers'
	// `FxRunner` — and `RackEngine` never called it at all.
	//
	// HARM SETS `settings.gate` FROM THAT CONTEXT and keys its envelope off it, so with no delivery
	// the gate stayed false, the envelope never opened, and an enabled device with its level up
	// produced silence.
	//
	// **THE EARLIER CASE PASSED BECAUSE IT ASKED A WEAKER QUESTION** — "does enabling change the
	// buffer" — and enabling changes the buffer for reasons that have nothing to do with a note. That
	// is the difference-check trap in CLAUDE.md: a comparison that passes whenever ANYTHING differs.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	const size_t slot = engine.addDevice(harm::kTraits.tag);
	REQUIRE(slot != rack::RackEngine::kNoSlot);
	engine.setField(slot, static_cast<size_t>(harm::Param::Enabled), 1.0f);

	const auto renderPeak = [&](bool withNote) {
		if (withNote) {
			device::NoteEvent on {};
			on.kind = device::NoteEvent::Kind::On;
			on.key = 60;
			engine.setBlockNotes(&on, 1);
		}
		else {
			engine.setBlockNotes(nullptr, 0);
		}
		float peak = 0.0f;
		backplane::BlockContext block {};
		// SEVERAL BLOCKS: an envelope needs time to open, so one block would report silence for a
		// device that is working and merely early.
		for (int b = 0; b < 24; ++b) {
			// **SILENCE IN, AND THAT IS THE WHOLE POINT.** My first version fed a sine and asserted the
			// peak was non-zero — which HARM satisfies by passing the DRY signal through, so the case
			// passed with the note delivery deleted. The known-bad caught it: a difference check that
			// passes whenever anything differs, in the very case whose comment cites that trap.
			//
			// From silence, any output is GENERATED, and HARM generates only when it has been given a
			// note.
			std::vector<float> left(256, 0.0f);
			std::vector<float> right(256, 0.0f);
			engine.processStereo(left.data(), right.data(), static_cast<int>(left.size()), block);
			for (const float v : left) {
				peak = std::max(peak, std::abs(v));
			}
			// THE NOTE IS ONLY IN THE FIRST BLOCK. A note event is an EDGE; repeating it every block
			// would be a case that only works while the hand is retriggering.
			engine.setBlockNotes(nullptr, 0);
		}
		return peak;
	};

	// WITH A NOTE: the device sounds. WITHOUT ONE: it does not. **Both halves, because either alone is
	// satisfiable by something other than the note reaching the device.**
	const float silent = renderPeak(false);
	CHECK(silent == doctest::Approx(0.0f));
	const float held = renderPeak(true);
	CHECK(held > 0.0f);
}

TEST_CASE("moving a device carries its routes, because a route is keyed on a lane") {
	// **THE WHOLE REASON `Slot::routeLane` EXISTS.** `routeLaneOf` used to answer the slot index, with
	// a comment predicting that *"a rack that gains reordering gains the distinction here and nowhere
	// else"*. If a route were keyed on the SLOT, moving a device would silently re-point every route
	// drawn to it onto whatever slid into its position — the repo's own identity rule, which is that a
	// position is not an identity because positions get reused.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	const size_t a = engine.addDevice(fx::filter::kTraits.tag);
	const size_t b = engine.addDevice(fx::owltt::kTraits.tag);
	const size_t c = engine.addDevice(fx::shaper::kTraits.tag);
	REQUIRE(a == 0);
	REQUIRE(b == 1);
	REQUIRE(c == 2);

	// **BY POSITION, THROUGH THE CHAIN — because `slots()` is LANE-indexed now.**
	//
	// This read `slots().descriptorAtLane(slot)` and meant "the device at this place in the chain". Step 2
	// made `slots()` a view over LANES, so that spelling now answers "the device that lives at this
	// index", which is a different question the moment anything is reordered. The signature did not
	// change, so nothing warned: this helper is where the meaning change would have gone unnoticed,
	// and the case below failing is what found it.
	const auto tagAt = [&](size_t position) {
		const size_t lane = engine.deviceAtPosition(position);
		if (lane == rack::RackEngine::kNoSlot) {
			return std::string();
		}
		const device::Descriptor* d = engine.slots().descriptorAtLane(lane);
		return std::string(d != nullptr ? d->traits.tag : "");
	};

	// A ROUTE ONTO THE MIDDLE DEVICE, addressed through the lane the ENGINE reports — not through the
	// slot index, which is the assumption under test.
	// `b` IS ALREADY A LANE — `addDevice` returns one — so this is the identity and is written as the
	// lane it is. Kept as a call rather than as `b` so the case still asserts the engine agrees.
	const size_t lane = b;
	REQUIRE(lane != rack::RackEngine::kNoLane);
	const backplane::Destination destination = test::ordinalOf(engine, lane, 0);
	engine.setRouteAmount(test::macro(0), destination, 0.75f);
	REQUIRE(engine.routeAmount(test::macro(0), destination) == doctest::Approx(0.75f));

	SUBCASE("the order changes and the route still lands on the same device") {
		REQUIRE(engine.moveDevice(2, 0));
		// ROTATE, NOT SWAP: shaper to the front, the other two each shifted one later.
		CHECK(tagAt(0) == fx::shaper::kTraits.tag);
		CHECK(tagAt(1) == fx::filter::kTraits.tag);
		CHECK(tagAt(2) == fx::owltt::kTraits.tag);

		// **THE VALUE, AT THE DEVICE'S OWN NEW ADDRESS.** OWLTT is at slot 2 now; asking the engine for
		// its lane and reading the route there is what proves the route travelled with the device
		// rather than staying at an index. A check that "some route still holds 0.75" would pass with
		// the amount stranded on whatever now sits at slot 1.
		// **ASKED BY POSITION, NOT THROUGH `routeLaneOf`.** That answered "the lane of the device at
		// this position" while `slots_` was position-indexed; under step 2 it is the IDENTITY — the
		// lane of the thing at lane N is N — so it can no longer answer a question about position.
		// Same meaning change as `tagAt` above, in a second spelling, and neither changed signature.
		const size_t movedLane = engine.deviceAtPosition(2);
		CHECK(movedLane == lane);
		CHECK(engine.routeAmount(test::macro(0), test::ordinalOf(engine, movedLane, 0))
		      == doctest::Approx(0.75f));

		// AND NOTHING WAS LEFT BEHIND at the address it used to occupy — the failure this case exists
		// to catch draws the route onto the device that moved INTO slot 1.
		const size_t nowAtOldSlot = engine.deviceAtPosition(1);
		if (nowAtOldSlot != lane) {
			CHECK(engine.routeAmount(test::macro(0), test::ordinalOf(engine, nowAtOldSlot, 0))
			      == doctest::Approx(0.0f));
		}
	}

	SUBCASE("and removing one after a reorder clears ITS lane, not the position's") {
		// **THIS WAS A LIVE BUG IN THE INSTALLED BUILD, AND THIS CASE IS WHERE IT SHOULD HAVE FAILED.**
		// The case above covers move-then-READ and passes; it never removed anything afterwards, and
		// the removal road had the same position-for-identity mistake the read road had been fixed for.
		//
		// `removeDevice` opened with `clearDeviceSlotRoutes(&routes_, slot)` — a POSITION where the
		// route table wants a LANE. `backplane::Graph::remove` calls the same helper with `laneOf(id)`,
		// which is the confirmation. Before a reorder the two agree and nothing shows.
		//
		// BOTH HALVES WERE WRONG AT ONCE, so both are asserted: the survivor keeps its amount and the
		// removed device's lane is emptied. Checking only one would have passed against a fix that
		// cleared everything, or against one that cleared nothing.
		//
		// MEASURED before the fix: 0.75 left standing on the dead device's lane, 0 on the survivor's —
		// "I deleted one effect and a different effect's modulation vanished", and later "a new device
		// arrived pre-modulated" when something took the uncleared lane.
		const size_t laneOfA = 0;
		const backplane::Destination destinationA = test::ordinalOf(engine, laneOfA, 0);
		engine.setRouteAmount(test::macro(0), destinationA, 0.25f);
		REQUIRE(engine.routeAmount(test::macro(0), destinationA) == doctest::Approx(0.25f));

		// **THE REORDER IS WHAT MAKES POSITION AND LANE DISAGREE**, and under step 2 that is all it
		// does: the devices keep their lanes and only their places change. The case is kept anyway —
		// what it pins is that removing a device empties the columns of the device REMOVED, and a
		// mutation can still get that wrong even when there is only one number to get right.
		REQUIRE(engine.moveDevice(1, 0));
		const size_t doomedLane = engine.deviceAtPosition(0);
		const size_t survivingLane = engine.deviceAtPosition(1);
		REQUIRE(doomedLane != survivingLane);
		// AND THE DEVICE AT POSITION 0 IS THE ONE THAT WAS AT 1, so the order really did change.
		REQUIRE(doomedLane == lane);

		engine.removeDeviceAtLane(doomedLane);

		// THE REMOVED DEVICE'S LANE IS EMPTY, or the next device to take it inherits somebody's routes.
		CHECK(engine.routeAmount(test::macro(0), test::ordinalOf(engine, doomedLane, 0))
		      == doctest::Approx(0.0f));
		// AND THE SURVIVOR STILL HAS ITS OWN, which is the half a player would notice first.
		CHECK(engine.routeAmount(test::macro(0), test::ordinalOf(engine, survivingLane, 0))
		      == doctest::Approx(0.25f));
	}

	SUBCASE("a device is told its new index, or its meters publish into the wrong slot") {
		// `AudioOps::setSlot` is how a device knows where it is; OWLTT publishes readings against
		// exactly that number. A move that did not re-issue it would leave a device writing into its
		// old slot's readings — a picture of the WRONG device, which is worse than no picture.
		REQUIRE(engine.moveDevice(1, 2));
		CHECK(tagAt(2) == fx::owltt::kTraits.tag);
		// Asked of the engine rather than of the device, because `slotOf` is not on the ops table —
		// what is observable here is that the publisher road still reaches the right slot.
		engine.publishReadings(2, nullptr, 0);
		CHECK(engine.readingCountAt(2) == 0);
	}

	SUBCASE("it refuses what it cannot do, and changes nothing when it refuses") {
		const std::string before = tagAt(0) + tagAt(1) + tagAt(2);
		CHECK_FALSE(engine.moveDevice(0, 0));          // nowhere
		CHECK_FALSE(engine.moveDevice(0, engine.slotCount()));  // past the end
		CHECK_FALSE(engine.moveDevice(engine.slotCount(), 0));
		CHECK_FALSE(engine.moveDevice(3, 0));          // an empty slot holds no device to move
		CHECK(tagAt(0) + tagAt(1) + tagAt(2) == before);
	}
}

TEST_CASE("SKLITTER's aux outputs are published in the rack — the first device that is both an audio device and a source") {
	// **THE WHOLE `kModOps` ROAD, THROUGH THE ENGINE RATHER THAN BY HAND.** `RackEngine::advanceModulators`
	// walks every chain's every step and ticks any slot whose descriptor has `mod` ops — it does not
	// exclude audio chains — so a dual device is configured, ticked and published exactly like a
	// modulator while it renders audio. That claim cannot be made by calling the device directly: the
	// whole question is whether the ENGINE takes it down both roads.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	const size_t slot = engine.addDevice(fx::sklitter::kTraits.tag);
	REQUIRE(slot != rack::RackEngine::kNoSlot);
	engine.prepare(48000.0, 4096);

	// **THE PUCK ON WAYPOINT 2, WHICH IS A DIAGONAL SINCE 2026-09-19.** `Perf X` is output 0 and
	// `Weight 2` is output 3; on a waypoint the centre's share is 0 and that one corner has the whole of
	// it, which is what makes the outputs readable here.
	//
	// **IT READ `PerfX = 1.0f` AND THAT STOPPED NAMING A WAYPOINT WHEN THEY ROTATED.** Straight out to
	// the right is now the boundary between waypoints 2 and 3, so the case measured 0.5 and reported it
	// as a publishing fault. `waypointAxis` is asked rather than a direction spelled here, so the next
	// rotation moves this fixture with the engine instead of leaving it asserting the old country.
	const auto [ex, ey] = fx::sklitter::waypointAxis(fx::sklitter::kCfg2);
	engine.setField(slot, static_cast<size_t>(fx::sklitter::Param::PerfX), ex);
	engine.setField(slot, static_cast<size_t>(fx::sklitter::Param::PerfY), ey);

	backplane::BlockContext block {};
	std::array<float, 64> left {};
	std::array<float, 64> right {};
	const auto run = [&] { engine.processStereo(left.data(), right.data(), 64, block); };
	// TWO BLOCKS: the first delivers the row and ticks, the second is what says the value HOLDS rather
	// than being a transient of the first publish.
	run();
	run();

	// THE AXES COME BACK AS THEY WENT IN — the puck is published, not the weights' own frame.
	CHECK(engine.outputAt(slot, outIndex(fx::sklitter::Out::X)) == doctest::Approx(ex));
	CHECK(engine.outputAt(slot, outIndex(fx::sklitter::Out::Y)) == doctest::Approx(ey));
	CHECK(engine.outputAt(slot, outIndex(fx::sklitter::Out::Weight2)) == doctest::Approx(1.0f));
	CHECK(engine.outputAt(slot, outIndex(fx::sklitter::Out::Weight1)) == doctest::Approx(0.0f)); // the axis nothing moved
	CHECK(engine.outputAt(slot, outIndex(fx::sklitter::Out::Weight3)) == doctest::Approx(0.0f));
	CHECK(engine.outputAt(slot, outIndex(fx::sklitter::Out::Weight4)) == doctest::Approx(0.0f));
	CHECK(engine.publishedOutputAt(slot, outIndex(fx::sklitter::Out::X)) == doctest::Approx(ex));
	CHECK(engine.publishedOutputAt(slot, outIndex(fx::sklitter::Out::Weight2)) == doctest::Approx(1.0f));

	SUBCASE("and the bypass gates the publish, exactly as it does for a modulator") {
		// **THE BYPASS TAKES THE OUTPUTS WITH IT, AND THAT IS A DESIGN DECISION (2026-09-17) rather than a
		// side-effect to be tidied away later.** It was weighed as a choice — a dual device could
		// keep publishing its gesture while its audio is bypassed, through a per-output survival switch
		// — and the answer was to keep ONE switch with ONE meaning and no carve-out in a rule that is
		// uniform across every device that has ever had an output. A player who wants the puck while the
		// stutter is off routes from something else. `sklitter_types.h` carries the decision beside
		// `kOutputs`; this subcase is what stops it being "fixed".
		//
		// **`outputAt` IS UNGATED AND `publishedOutputAt` IS NOT**, which is the distinction this
		// subcase is for: `advanceModulators` writes the REST value into the cell for a disabled
		// device, and every route reads the cell. A check on `outputAt` alone would pass on a build
		// where the enable did nothing.
		engine.setField(slot, static_cast<size_t>(fx::sklitter::Param::Enabled), 0.0f);
		run();
		CHECK(engine.publishedOutputAt(slot, outIndex(fx::sklitter::Out::X)) == doctest::Approx(0.0f));
		CHECK(engine.publishedOutputAt(slot, outIndex(fx::sklitter::Out::Weight2)) == doctest::Approx(0.0f));
		// AND THE DEVICE ITSELF IS STILL HOLDING THE PUCK — the gate is on the publish, not on the
		// device, so switching back on lands where the gesture is rather than at the centre.
		CHECK(engine.outputAt(slot, outIndex(fx::sklitter::Out::X)) == doctest::Approx(ex));
		CHECK(engine.outputAt(slot, outIndex(fx::sklitter::Out::Y)) == doctest::Approx(ey));
		// NOT VACUOUS: the puck is not at the centre, so "still holding it" is a claim about a real
		// position rather than about the rest value the publish was just gated down to.
		CHECK(ex != 0.0f);
	}
}

//------------------------------------------------------------------------------------------------//
// BACKLOG HS — A DEVICE'S OUTPUTS INTO ITS OWN CONTROLS, WHERE THE DEVICE DECLARED THEM DOWNSTREAM.   //
//------------------------------------------------------------------------------------------------//
//
// The main controls, other than those that feed Perform, sit downstream of Perform so Perform can
// modulate them. `device::SelfRouting` is the declaration; SKLITTER is its first
// user; trap T1's cases (`test_rack_nesting.cpp`) are untouched and still say a device with no
// declaration — and a HOLDER's inner chain, declaration or not — is not upstream of itself.

namespace {
/// THE PERFORM-IMPACTING SET, RESTATED HERE FROM THE MOD ROAD RATHER THAN READ FROM THE DECLARATION —
/// `setPerformFields` reads the puck; the engine's publish reads the bypass. A case that asked
/// `kSelfRouting` what to expect would agree with any list at all.
bool feedsPerform(size_t control) {
	// Perf Depth feeds nothing; it is refused by design (2026-09-22), restated here as such.
	return control == static_cast<size_t>(fx::sklitter::Param::PerfX)
	       || control == static_cast<size_t>(fx::sklitter::Param::PerfY)
	       || control == static_cast<size_t>(fx::sklitter::Param::PerfDepth)
	       || control == static_cast<size_t>(fx::sklitter::Param::Enabled);
}
} // namespace

TEST_CASE("HS: SKLITTER's perform outputs may reach its own controls — every one EXCEPT those that feed perform") {
	// THE STANDALONE RACK'S OWN AXES, not this file's `kAxes`: that one's `outputStride` is 4, so it
	// refuses outputs 4..6 for want of a column — a refusal that is not the one under test.
	rack::RackEngine engine { rack::kRackAxes, hostCatalogue() };
	const size_t slot = engine.addDevice(fx::sklitter::kTraits.tag);
	REQUIRE(slot != rack::RackEngine::kNoSlot);
	engine.prepare(48000.0, 4096);
	const device::Descriptor& d = fx::sklitter::kDescriptor;

	size_t offered = 0;
	size_t refusedForFeeding = 0;
	for (size_t output = 0; output < d.outputs.size(); ++output) {
		for (size_t control = 0; control < d.params.size(); ++control) {
			CAPTURE(output);
			CAPTURE(std::string(d.params[control].name));
			const backplane::Source s = test::outputOf(engine, slot, output);
			const backplane::Destination to = test::controlOf(engine, slot, control);
			const bool expected = d.params[control].modulatable && !feedsPerform(control);
			// THE GATE (what a surface glows by) AND THE STORE (what a drag writes) — asked separately,
			// because the honesty rule is broken the moment they disagree.
			CHECK(engine.mayRoute(s, to) == expected);
			engine.setRouteAmount(s, to, 0.25f);
			CHECK(engine.routeAmount(s, to) == (expected ? 0.25f : 0.0f));
			engine.setRouteAmount(s, to, 0.0f);
			offered += expected ? 1 : 0;
			refusedForFeeding += d.params[control].modulatable && feedsPerform(control) ? 1 : 0;
		}
	}
	// NEITHER HALF IS VACUOUS: the puck's two axes and Perf Depth (by design) are
	// modulatable AND refused, for all seven outputs; and there is something on the other side of the line.
	// FOUR SINCE BACKLOG KW: `Enabled` is a destination now, and a self-route into it is refused (the publish gates on it).
	CHECK(refusedForFeeding == 4 * d.outputs.size());
	CHECK(offered == (d.modulatableCount() - 4) * d.outputs.size());
	CHECK(offered > 0);
}

TEST_CASE("HS: the declaration's rule — nothing declared refuses all; the engine's own two publish controls are refused without being listed") {
	// **ASKED OF A DESCRIPTOR BUILT HERE, BECAUSE SKLITTER CANNOT SAY IT**: its `Enabled` is not
	// modulatable and it names no `amountControl`, so on the one device that declares, the clause about
	// the controls `advanceModulators` reads at the PUBLISH is refused by an earlier test every time and
	// could be deleted with everything green. A modulatable Amount knob is an ordinary thing for a
	// modulator to have (backlog DH), and a self-route into it is a loop the device never listed.
	static constexpr device::ParamDesc kParams[] = {
		{"feeds", "Feeds", 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, true},
		{"on", "On", 0.0, 1.0, 1.0, false, device::Unit::Percent, {}, true},
		{"amount", "Amount", 0.0, 1.0, 1.0, false, device::Unit::Percent, {}, true},
		{"downstream", "Downstream", 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, true},
	};
	static constexpr uint8_t kFeeds[] = { 0 };
	static constexpr device::SelfRouting kDeclared { .feedsOutputs = kFeeds };
	device::Descriptor d { .traits = { .tag = "TEST", .name = "test", .hasCard = false, .hasExpanded = false,
		                               .enableControl = 1, .amountControl = 2 },
		                   .params = kParams,
		                   .outputs = {} };

	for (size_t control = 0; control < std::size(kParams); ++control) {
		CAPTURE(control);
		CHECK_FALSE(d.mayRouteToSelf(control)); // NOTHING DECLARED: today's rule, for every control
	}
	d.selfRouting = &kDeclared;
	CHECK_FALSE(d.mayRouteToSelf(0)); // listed
	CHECK_FALSE(d.mayRouteToSelf(1)); // the enable the publish gates on — not listed, refused anyway
	CHECK_FALSE(d.mayRouteToSelf(2)); // the amount the publish scales by — likewise
	CHECK(d.mayRouteToSelf(3));       // and the one control that really is downstream
	CHECK_FALSE(d.mayRouteToSelf(4)); // past the end is not a control
}

TEST_CASE("HS: Perf X routed to SKLITTER's own Rate ARRIVES — the value, in the tick it was published") {
	rack::RackEngine engine { kAxes, hostCatalogue() };
	const size_t slot = engine.addDevice(fx::sklitter::kTraits.tag);
	REQUIRE(slot != rack::RackEngine::kNoSlot);
	engine.prepare(48000.0, 4096);

	const auto rate = static_cast<size_t>(fx::sklitter::Param::Rate);
	const auto perfX = static_cast<size_t>(fx::sklitter::Param::PerfX);
	const backplane::Source s = test::outputOf(engine, slot, outIndex(fx::sklitter::Out::X));
	const backplane::Destination to = test::controlOf(engine, slot, rate);
	const float base = engine.fieldAt(slot, rate);
	REQUIRE(base == doctest::Approx(0.5f)); // the fixture's arithmetic below leans on it

	backplane::BlockContext block {};
	std::array<float, 64> left {};
	std::array<float, 64> right {};
	const auto run = [&] { engine.processStereo(left.data(), right.data(), 64, block); };

	engine.setRouteAmount(s, to, 0.5f);
	REQUIRE(engine.routeAmount(s, to) == 0.5f);
	run();
	// THE PUCK AT THE CENTRE PUBLISHES 0, and a route with nothing behind it moves nothing.
	CHECK(engine.resolvedFieldForTest(slot, rate) == doctest::Approx(base));

	// **ONE BLOCK OF ONE TICK, AND THE VALUE IS ALREADY THERE** — the mod road resolves, ticks and
	// publishes before the audio road resolves the row the device is handed, so this is the same tick
	// and not the next. Rate is 0..1, so the span is 1: base + amount x Perf X = 0.5 + 0.5 x 0.6.
	engine.setField(slot, perfX, 0.6f);
	run();
	CHECK(engine.publishedOutputAt(slot, outIndex(fx::sklitter::Out::X)) == doctest::Approx(0.6f));
	CHECK(engine.resolvedFieldForTest(slot, rate) == doctest::Approx(0.8f));

	// AND IT FOLLOWS THE PUCK DOWN AS WELL AS UP — a bipolar source reaches both ways from the base.
	engine.setField(slot, perfX, -0.4f);
	run();
	CHECK(engine.resolvedFieldForTest(slot, rate) == doctest::Approx(0.3f));

	// THE PUCK ITSELF IS NOT MOVED BY ANY OF THIS — nothing reaches the control the output is made of.
	CHECK(engine.resolvedFieldForTest(slot, perfX) == doctest::Approx(-0.4f));
}

TEST_CASE("HS: a self-route survives a reorder of its chain — no drag moves a device relative to itself") {
	rack::RackEngine engine { kAxes, hostCatalogue() };
	const size_t first = engine.addDevice(fx::sklitter::kTraits.tag);
	const size_t second = engine.addDevice(filter::kTraits.tag);
	REQUIRE(first != rack::RackEngine::kNoSlot);
	REQUIRE(second != rack::RackEngine::kNoSlot);
	engine.prepare(48000.0, 4096);

	const backplane::Source s = test::outputOf(engine, first, outIndex(fx::sklitter::Out::Weight1));
	const backplane::Destination own = test::controlOf(engine, first, static_cast<size_t>(fx::sklitter::Param::Density));
	const size_t cutoff = controlNamed(filter::kDescriptor, "Cutoff");
	const backplane::Destination downstream = test::controlOf(engine, second, cutoff);
	engine.setRouteAmount(s, own, 0.5f);
	engine.setRouteAmount(s, downstream, 0.5f);
	REQUIRE(engine.routeAmount(s, own) == 0.5f);
	REQUIRE(engine.routeAmount(s, downstream) == 0.5f);

	// SKLITTER DRAGGED BEHIND THE FILTER. The route into SKLITTER itself is exactly as legal as it was.
	// **AND SINCE BACKLOG IZ SO IS THE ROUTE INTO THE FILTER**, which this case used to watch go: SKLITTER
	// publishes in the modulators' pass and the filter reads it when it runs, so "behind" in the lane is
	// not "downstream" for modulation. Nothing is cleared.
	const std::optional<size_t> cleared = engine.moveDevice(0, 1);
	REQUIRE(cleared.has_value());
	CHECK(*cleared == 0u);
	CHECK(engine.routeAmount(s, downstream) == 0.5f);
	CHECK(engine.routeAmount(s, own) == 0.5f);
}

//------------------------------------------------------------------------------------------------//
// BACKLOG IZ — MOD BEFORE AUDIO: A MODULATOR REACHES UPSTREAM WHEN IT DOES NOT READ AUDIO.          //
//------------------------------------------------------------------------------------------------//
//
// For modulation, "downstream" means COMPUTED FROM DOWNSTREAM AUDIO; a modulator's position in a
// lane is only grouping. Asserted by VALUE, in the block the source was moved in —
// the claim is that the graph now admits what `beginPiece` already executes in order.

TEST_CASE("IZ: SKLITTER's Perf X reaches a filter BEFORE it in the rack — the value, in the tick it was published") {
	rack::RackEngine engine { kAxes, hostCatalogue() };
	const size_t filterSlot = engine.addDevice(filter::kTraits.tag);
	const size_t sk = engine.addDevice(fx::sklitter::kTraits.tag);
	REQUIRE(filterSlot != rack::RackEngine::kNoSlot);
	REQUIRE(sk != rack::RackEngine::kNoSlot);
	engine.prepare(48000.0, 4096);
	// THE PREMISE: the filter RUNS FIRST, so this is a route against the lane.
	REQUIRE(engine.positionOfLane(filterSlot) < engine.positionOfLane(sk));

	const size_t cutoff = controlNamed(filter::kDescriptor, "Cutoff");
	const auto perfX = static_cast<size_t>(fx::sklitter::Param::PerfX);
	const backplane::Source s = test::outputOf(engine, sk, outIndex(fx::sklitter::Out::X));
	const backplane::Destination to = test::controlOf(engine, filterSlot, cutoff);

	CHECK(engine.mayRoute(s, to));
	engine.setRouteAmount(s, to, 0.5f);
	REQUIRE(engine.routeAmount(s, to) == 0.5f);
	// AND IT DID NOT REORDER THE AUDIO — the filter still runs first.
	CHECK(engine.positionOfLane(filterSlot) < engine.positionOfLane(sk));

	const float base = engine.fieldAt(filterSlot, cutoff);
	const device::ParamDesc& p = filter::kDescriptor.params[cutoff];
	REQUIRE(p.offsetsMayLeaveRange); // the arithmetic below does not clamp

	backplane::BlockContext block {};
	std::array<float, 64> left {};
	std::array<float, 64> right {};
	const auto run = [&] { engine.processStereo(left.data(), right.data(), 64, block); };
	run();
	CHECK(engine.resolvedFieldForTest(filterSlot, cutoff) == doctest::Approx(base)); // puck centred: 0

	// **ONE BLOCK OF ONE TICK, AND THE FILTER ALREADY READS IT.** The modulators' pass publishes Perf X
	// before the audio walk resolves the filter's row — though the filter runs before SKLITTER does.
	engine.setField(sk, perfX, 0.6f);
	run();
	REQUIRE(engine.publishedOutputAt(sk, outIndex(fx::sklitter::Out::X)) == doctest::Approx(0.6f));
	CHECK(engine.resolvedFieldForTest(filterSlot, cutoff)
	      == doctest::Approx(static_cast<double>(base) + 0.5 * 0.6 * device::routeSpanOf(p)));

	engine.setField(sk, perfX, -0.4f);
	run();
	CHECK(engine.resolvedFieldForTest(filterSlot, cutoff)
	      == doctest::Approx(static_cast<double>(base) - 0.5 * 0.4 * device::routeSpanOf(p)));
}

TEST_CASE("IZ: a modulator to the RIGHT of another reaches it — the value, in the block it was published") {
	// **THE ROUTE REORDERS THE MODULATORS' PASS, AND THE PASS IS WHAT IS ASSERTED.** Two LFOs, the second
	// into the first's Skew Pre-Peak (0..1, so the arithmetic is the plain one). The first's resolved
	// row is compared with the SECOND's output of THIS block and of the block before — an LFO moves every
	// block, so the two differ, and a pass that still ticked the first first would read the older one.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	const size_t first = engine.addDevice(mod::lfo::kTraits.tag);
	const size_t second = engine.addDevice(mod::lfo::kTraits.tag);
	REQUIRE(first != rack::RackEngine::kNoSlot);
	REQUIRE(second != rack::RackEngine::kNoSlot);
	engine.prepare(48000.0, 4096);
	REQUIRE(engine.positionOfLane(first) < engine.positionOfLane(second));

	const size_t skew = controlNamed(mod::lfo::kDescriptor, "Skew Pre-Peak");
	const device::ParamDesc& p = mod::lfo::kDescriptor.params[skew];
	REQUIRE(p.modulatable);
	const backplane::Source s = test::outputOf(engine, second, 0);
	const backplane::Destination to = test::controlOf(engine, first, skew);
	CHECK(engine.mayRoute(s, to));
	const float amount = 0.25f;
	engine.setRouteAmount(s, to, amount);
	REQUIRE(engine.routeAmount(s, to) == amount);
	// THE LOOP BACK IS STILL REFUSED — by the instants, not by the lane.
	CHECK_FALSE(engine.mayRoute(test::outputOf(engine, first, 0), test::controlOf(engine, second, skew)));

	// THE SOURCE RUNNING FREE AND FAST — Free needs no gate, and a sixteenth of a second per cycle puts
	// consecutive blocks' outputs far apart — with its smoothing at the bottom.
	const auto set = [&](const char* name, double value) {
		const size_t c = controlNamed(mod::lfo::kDescriptor, name);
		engine.setField(second, c, static_cast<float>(std::max(value, mod::lfo::kDescriptor.params[c].min)));
	};
	set("Mode", static_cast<double>(mod::Mode::Free));
	set("Rate", -4.0);
	set("Smoothing", -100.0);

	const float base = engine.fieldAt(first, skew);
	backplane::BlockContext block {};
	std::array<float, 64> left {};
	std::array<float, 64> right {};
	const auto run = [&] { engine.processStereo(left.data(), right.data(), 64, block); };
	const auto expected = [&](float source) {
		return std::clamp(static_cast<double>(base) + amount * source * (p.max - p.min), p.min, p.max);
	};
	size_t distinguished = 0;
	for (int b = 0; b < 16; ++b) {
		const float was = engine.publishedOutputAt(second, 0);
		run();
		const float now = engine.publishedOutputAt(second, 0);
		CAPTURE(b);
		CHECK(engine.resolvedFieldForTest(first, skew) == doctest::Approx(expected(now)));
		// DISTINGUISHED = further apart than `Approx` can blur: its tolerance is 100 float epsilons relative,
		// about 7.5e-6 at this value, so a gap over 2e-5 fails the CHECK above if the older one is read.
		distinguished += std::abs(expected(now) - expected(was)) > 2e-5 ? 1 : 0;
	}
	// NOT VACUOUS: the source moved between blocks often enough that reading last block's would fail.
	CHECK(distinguished >= 8);
}

TEST_CASE("IZ: two SKLITTERs — the later one's output into the earlier is refused; forward is not") {
	rack::RackEngine engine { kAxes, hostCatalogue() };
	const size_t first = engine.addDevice(fx::sklitter::kTraits.tag);
	const size_t second = engine.addDevice(fx::sklitter::kTraits.tag);
	REQUIRE(first != rack::RackEngine::kNoSlot);
	REQUIRE(second != rack::RackEngine::kNoSlot);
	engine.prepare(48000.0, 4096);
	// TWO DEVICES THAT BOTH TICK AND RUN: the second into the first would need the second ticked first
	// AND the first run first, in one order — refused, gate and store alike.
	const backplane::Source s = test::outputOf(engine, second, outIndex(fx::sklitter::Out::X));
	const backplane::Destination to =
	    test::controlOf(engine, first, static_cast<size_t>(fx::sklitter::Param::Rate));
	CHECK_FALSE(engine.mayRoute(s, to));
	engine.setRouteAmount(s, to, 0.5f);
	CHECK(engine.routeAmount(s, to) == 0.0f);
	// FORWARD, the non-vacuity control.
	const backplane::Source forward = test::outputOf(engine, first, outIndex(fx::sklitter::Out::X));
	const backplane::Destination second_rate =
	    test::controlOf(engine, second, static_cast<size_t>(fx::sklitter::Param::Rate));
	CHECK(engine.mayRoute(forward, second_rate));
}

TEST_CASE("HS: a device that declares nothing is refused its own controls, exactly as before") {
	// THE DEFAULT IS TODAY'S BEHAVIOUR. The DAHDSR publishes an envelope and declares no `SelfRouting`;
	// every control it has feeds that envelope, which is the loop the rule has always refused.
	REQUIRE(dahdsr::kDescriptor.selfRouting == nullptr);
	rack::RackEngine engine { kAxes, hostCatalogue() };
	const size_t slot = engine.addDevice(dahdsr::kTraits.tag);
	REQUIRE(slot != rack::RackEngine::kNoSlot);
	engine.prepare(48000.0, 4096);
	const device::Descriptor& d = dahdsr::kDescriptor;
	REQUIRE(d.outputs.size() > 0);
	REQUIRE(d.modulatableCount() > 0);
	for (size_t control = 0; control < d.params.size(); ++control) {
		CAPTURE(std::string(d.params[control].name));
		const backplane::Source s = test::outputOf(engine, slot, 0);
		const backplane::Destination to = test::controlOf(engine, slot, control);
		CHECK_FALSE(engine.mayRoute(s, to));
		engine.setRouteAmount(s, to, 0.25f);
		CHECK(engine.routeAmount(s, to) == 0.0f);
	}
}

TEST_CASE("a disabled modulator publishes its rest value, so the switch changes sound") {
	// The mod card headers share the unified header, enable button included.
	// Declaring `enableControl` is what DRAWS the square; this is the half that makes it do something.
	// Without it the switch would move a field nothing reads — a control that does nothing, which the
	// honesty rule forbids and which no card-level test would catch.
	//
	// **ASSERTED ON THE TABLE, NOT ON A RENDER.** The claim is "this source reads its rest value at
	// this instant", and a comparison of two rendered buffers cannot say that — it says only that
	// something differed. `sourceValue` is the number the routing road actually multiplies.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	const size_t slot = engine.addDevice(mod::dahdsr::kTraits.tag);
	REQUIRE(slot != rack::RackEngine::kNoSlot);
	engine.prepare(48000.0, 4096);

	const size_t lane = slot;
	REQUIRE(lane != rack::RackEngine::kNoLane);

	const auto enable = static_cast<size_t>(mod::dahdsr::Param::Enabled);
	// ON BY DEFAULT — a modulator added to a rack should modulate. If this ever ships off, the case
	// below would pass for the wrong reason.
	REQUIRE(engine.fieldAt(slot, enable) >= 0.5f);

	// DRIVE IT SOMEWHERE AWAY FROM REST, or "disabled reads 0" is indistinguishable from "it was
	// always 0". This is the non-vacuity guard and it is the whole difficulty of the case.
	// THROUGH `processStereo`, WHICH IS THE PUBLIC ROAD — it advances the modulators itself.
	// `advanceModulators` is private, and reaching past that to save two lines would be a case testing
	// a path no host takes.
	backplane::BlockContext block {};
	engine.setGate(true);
	std::array<float, 64> left {};
	std::array<float, 64> right {};
	const auto run = [&] { engine.processStereo(left.data(), right.data(), 64, block); };
	for (int i = 0; i < 64; ++i) {
		run();
	}
	const float running = engine.publishedOutputAt(lane, 0);
	REQUIRE(running > 0.01f);

	SUBCASE("switched off, the source reads rest and destinations sit at their base") {
		engine.setField(slot, enable, 0.0f);
		run();
		// **REST IS 0 BECAUSE MODULATION IS ADDITIVE.** `resolveInto` computes
		// `base + amount * unit * sourceValue`, so a source at 0 contributes nothing and every
		// destination sits at its own base — which is "stop moving things" rather than "slam to zero".
		CHECK(engine.publishedOutputAt(lane, 0) == doctest::Approx(0.0f));
	}

	SUBCASE("switched back on, it is where the music is rather than where it left") {
		engine.setField(slot, enable, 0.0f);
		run();
		REQUIRE(engine.publishedOutputAt(lane, 0) == doctest::Approx(0.0f));

		engine.setField(slot, enable, 1.0f);
		run();
		// THE TICK KEPT RUNNING WHILE MUTED — only the output was gated. A frozen modulator would come
		// back mid-shape, which makes the switch a state-editing gesture rather than a mute.
		CHECK(engine.publishedOutputAt(lane, 0) > 0.01f);
	}
}

TEST_CASE("a device added after a removal goes to the END, at its own defaults") {
	// The defect: adding a device of the same type as a deleted one put it where the deleted one had
	// been instead of at the end of the list. It must go to the end, and at its init state.
	//
	// **TWO CLAIMS, AND ONLY ONE OF THEM WAS TRUE.** The POSITION was wrong: `addDevice` walked from
	// zero and took the first free slot, so removing the middle and adding anything put it back in the
	// middle. **The STATE was always init** — `addDeviceAt` writes every field from the descriptor's
	// declared defaults and `removeDevice` clears the slot's routing columns — so what looked like the
	// old device returning was a new device standing where the old one stood. Both are asserted here,
	// because "it looked like the old one came back" deserves an answer about the state as well.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	REQUIRE(engine.addDevice(fx::filter::kTraits.tag) == 0);
	REQUIRE(engine.addDevice(fx::owltt::kTraits.tag) == 1);
	REQUIRE(engine.addDevice(fx::shaper::kTraits.tag) == 2);

	// MOVE THE MIDDLE ONE OFF ITS DEFAULTS FIRST, so "the state is init" is distinguishable from
	// "the state was never touched". Without this the state assertion below passes vacuously.
	const auto drive = static_cast<size_t>(fx::owltt::Param::Threshold);
	const float def = static_cast<float>(fx::owltt::kDescriptor.params[drive].def);
	engine.setField(1, drive, def == 0.0f ? 1.0f : 0.0f);
	REQUIRE(engine.fieldAt(1, drive) != doctest::Approx(def));

	engine.removeDeviceAtLane(1);
	REQUIRE(engine.slots().descriptorAtLane(1) == nullptr);

	// **THE END OF THE CHAIN — AND THE LANE IS ALLOWED TO BE THE HOLE.**
	//
	// This asserted `added == 3`, because a slot index was a position and "the end" meant "past the
	// last live one". Step 2 separated the two questions and only one of them was ever the defect: what
	// showed was a device appearing in the MIDDLE OF THE ORDER, and the order is what this now asserts.
	// The device reuses lane 1 — the freed one — which is correct and wanted: lanes are reused so the
	// route table stays dense, and the old device's columns were cleared when it left.
	const size_t added = engine.addDevice(fx::owltt::kTraits.tag);
	REQUIRE(added != rack::RackEngine::kNoSlot);
	// THE POSITION IS THE END: two devices remained, so the new one is third.
	CHECK(engine.chainLength(0) == 3);
	CHECK(engine.deviceAtPosition(2) == added);
	// AND IT IS NOT IN THE MIDDLE, which is the bug in its own terms.
	CHECK(engine.deviceAtPosition(1) != added);

	// AND IT ARRIVES AT ITS DECLARED DEFAULTS, not carrying what the removed one held.
	CHECK(engine.fieldAt(added, drive) == doctest::Approx(def));

	SUBCASE("and it refuses only when every lane is taken") {
		// **THIS SUBCASE USED TO ASSERT A FALLBACK THAT NO LONGER EXISTS, and its disappearance is the
		// point rather than a loss.** It read: *"once the last slot is occupied 'the end' cannot be
		// honoured, so a hole is still filled — better than refusing"*, and it looped until an add
		// returned slot 1. That fallback existed because position and storage were one number: a
		// device appended past the last live one had nowhere to go when the tail was full, and taking
		// a hole meant taking a POSITION in the middle of the order.
		//
		// Under step 2 the two are separate. A lane is always the lowest free one and the position is
		// always the end, so the hole is reused on the FIRST add after a removal — which the outer
		// case now asserts — and there is no state in which "the end" cannot be honoured until the
		// rack is genuinely full. What is left to pin is that boundary.
		size_t at = engine.addDevice(fx::filter::kTraits.tag);
		size_t added = 0;
		while (at != rack::RackEngine::kNoSlot) {
			++added;
			at = engine.addDevice(fx::filter::kTraits.tag);
		}
		// IT REFUSED, AND ONLY AT THE END: every lane is occupied and the chain holds them all.
		CHECK(at == rack::RackEngine::kNoSlot);
		CHECK(added > 0);
		CHECK(engine.chainLength(0) == engine.slotCount());
	}
}

TEST_CASE("the audio runs in the chain's order, not in lane order") {
	// **THE ONE CLAIM STEP 2 COULD HAVE GOT WRONG SILENTLY.** The graph owns the order and a device's
	// storage lives at its lane; those two agree until something is reordered, and then they do not.
	// A block that walked lanes would render the rack in the order devices were ADDED while the cards
	// drew the order they were DRAGGED into — audible, invisible to every case that renders without
	// reordering first, and exactly the "silent wrong-order bug rather than a crash" `graph.h` warns
	// the step-2 adopter about.
	//
	// **THE REFERENCE IS A SECOND ENGINE BUILT IN THE OPPOSITE ORDER**, which is an independent
	// restatement rather than a remembered buffer: if reordering works, the reordered engine must
	// render what the engine that was built that way renders. Comparing against "not what it rendered
	// before" would pass on any change at all, including a wrong one.
	const auto build = [](const char* first, const char* second) {
		auto engine = std::make_unique<rack::RackEngine>(kAxes, hostCatalogue());
		engine->prepare(48000.0, 4096);
		REQUIRE(engine->addDevice(first) != rack::RackEngine::kNoSlot);
		REQUIRE(engine->addDevice(second) != rack::RackEngine::kNoSlot);
		return engine;
	};
	const auto render = [](rack::RackEngine& engine) {
		std::vector<float> l(64), r(64);
		for (size_t i = 0; i < l.size(); ++i) {
			// A RAMP RATHER THAN SILENCE: two devices that do nothing to zero would render the same
			// buffer in either order and the case would pass without an opinion.
			l[i] = r[i] = -0.9f + 1.8f * (static_cast<float>(i) / 63.0f);
		}
		backplane::BlockContext block {};
		engine.processStereo(l.data(), r.data(), 64, block);
		return l;
	};

	// OWLTT AND THE FILTER: a clipper and a filter do not commute, which is what makes order audible.
	auto forward = build(fx::owltt::kTraits.tag, fx::filter::kTraits.tag);
	auto backward = build(fx::filter::kTraits.tag, fx::owltt::kTraits.tag);
	const std::vector<float> forwardOut = render(*forward);
	const std::vector<float> backwardOut = render(*backward);

	// **THE ORDER IS AUDIBLE AT ALL — without this the comparison below is vacuous.** If these two
	// rendered the same, every assertion after it would hold for a rack that ignored order entirely.
	bool differs = false;
	for (size_t i = 0; i < forwardOut.size(); ++i) {
		if (std::abs(forwardOut[i] - backwardOut[i]) > 1e-6f) {
			differs = true;
		}
	}
	REQUIRE(differs);

	// NOW REORDER THE FIRST ONE INTO THE SECOND'S ORDER and render it again.
	auto moved = build(fx::owltt::kTraits.tag, fx::filter::kTraits.tag);
	REQUIRE(moved->moveDevice(1, 0));
	const std::vector<float> movedOut = render(*moved);

	// IT RENDERS WHAT THE ENGINE BUILT THAT WAY RENDERS, sample for sample.
	for (size_t i = 0; i < movedOut.size(); ++i) {
		CAPTURE(i);
		CHECK(movedOut[i] == doctest::Approx(backwardOut[i]).epsilon(1e-5));
	}
}

TEST_CASE("a reused lane inherits nothing, and a block in flight keeps what it started with") {
	// **THE OTHER HALF OF THE LANE-REUSE CLOSURE.** The route columns are pinned by the subcase in
	// "moving a device carries its routes"; this is the STORAGE, which `graph.h`'s THREADS section
	// names as the step-2 adopter's hazard: a lane freed and reissued while a block is still rendering
	// the old device. Two things must hold at once, and they pull in opposite directions —
	//
	//   * the block must not have the OLD device's storage destroyed under it (a use-after-free, which
	//     `ctest` cannot see and the sanitizer gate can), and
	//   * the reissued lane must not hand the block the NEW device's storage, nor the new device
	//     anything the old one left behind.
	//
	// A REMOVAL THAT DESTROYS AND ONE THAT PARKS LEAVE THE SAME RACK BEHIND, which is why this needs
	// `retiredCountForTest`: the difference between safe and unsafe here is invisible in the rack's
	// own state and visible only in the queue.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	const size_t a = engine.addDevice(fx::filter::kTraits.tag);
	const size_t b = engine.addDevice(fx::owltt::kTraits.tag);
	REQUIRE(a == 0);
	REQUIRE(b == 1);

	// MOVE THE DOOMED DEVICE OFF ITS DEFAULTS, so "the new device inherits nothing" is distinguishable
	// from "nothing was ever set" — the vacuity this case would otherwise pass on.
	const auto cutoff = static_cast<size_t>(fx::filter::Param::Cutoff);
	const float def = static_cast<float>(fx::filter::kDescriptor.params[cutoff].def);
	engine.setField(a, cutoff, def == 0.0f ? 1.0f : 0.0f);
	REQUIRE(engine.fieldAt(a, cutoff) != doctest::Approx(def));
	const void* oldStorage = engine.slotAtLane(a).storage.get();
	REQUIRE(oldStorage != nullptr);

	SUBCASE("removed between blocks, the storage is parked with an even epoch and the reap frees it at once") {
		// THE COMMON CASE, RESTATED FOR CF: a remove never frees on the caller's thread any more — it
		// parks, and the message thread's reap frees. With no block in flight the epoch stamped on the
		// entry is even, so the very next reap takes it.
		REQUIRE(engine.retiredCountForTest() == 0);
		engine.removeDeviceAtLane(a);
		CHECK(engine.retiredCountForTest() == 1);
		engine.reapRetiredForTest();
		CHECK(engine.retiredCountForTest() == 0);
	}

	SUBCASE("removed DURING a block, the storage is parked rather than destroyed") {
		engine.enterBlockForTest();
		engine.removeDeviceAtLane(a);

		// **PARKED, NOT FREED.** This is the assertion the gate cannot make and the gate is the
		// instrument for the other direction: if this line were wrong — if the device were destructed
		// here — a real block inside `processStereo` would be reading freed memory, which reads as a
		// green `ctest` and an ASan report.
		CHECK(engine.retiredCountForTest() == 1);
		// **AND THE REAPER WILL NOT TAKE IT WHILE THAT BLOCK IS IN FLIGHT** (CF): the entry carries an
		// odd epoch, and the epoch has not moved past it.
		engine.reapRetiredForTest();
		CHECK(engine.retiredCountForTest() == 1);
		// AND THE SLOT IS EMPTY, so the next block cannot enter it: the storage moved OUT of the slot
		// into the queue rather than being shared between them.
		CHECK(engine.slotAtLane(a).storage.get() == nullptr);
		CHECK_FALSE(engine.slotAtLane(a).live());

		// **THE LANE IS REISSUED WHILE THE OLD DEVICE IS STILL PARKED**, which is the hazard in one
		// line: `lowestFreeLane` offers this index again the moment nothing lives at it.
		const size_t reused = engine.addDevice(fx::filter::kTraits.tag);
		REQUIRE(reused == a);
		const void* newStorage = engine.slotAtLane(reused).storage.get();
		REQUIRE(newStorage != nullptr);

		// **DIFFERENT BYTES.** A block that started before the removal holds the old pointer; the new
		// device is somewhere else entirely, so the block cannot be handed the new device's state
		// through the reused lane. Asserting the POINTERS differ is the claim — "the field reads a
		// default" would also pass if the new device had been built on top of the old bytes.
		CHECK(newStorage != oldStorage);
		// AND THE OLD DEVICE IS STILL ALIVE while the block that could see it runs.
		CHECK(engine.retiredCountForTest() == 1);

		// THE NEW DEVICE INHERITS NOTHING: its row is its declared defaults, not what the removed
		// device was left at.
		CHECK(engine.fieldAt(reused, cutoff) == doctest::Approx(def));

		// AND WHEN THE BLOCK ENDS, the parked device goes. Not before: `reapRetired` asks the epoch
		// itself rather than trusting the caller to know.
		engine.reapRetiredForTest();
		CHECK(engine.retiredCountForTest() == 1); // still inside the block
		engine.leaveBlockForTest();
		engine.reapRetiredForTest();
		CHECK(engine.retiredCountForTest() == 0);
	}
}

TEST_CASE("an MSEG routed to a filter control moves it, at its own declared defaults") {
	// **Symptom: an MSEG's modulation did not seem to reach other devices, whatever their relative
	// order.** This is the engine half of that symptom,
	// pinned so the search can start somewhere else: the road works, and it works with the MSEG at the
	// settings a freshly added one has.
	//
	// AT ITS DEFAULTS ON PURPOSE. A case that gave the MSEG a hand-built shape would prove the
	// mechanism and say nothing about what a player gets by dropping one in — and a modulator that
	// arrives flat would be indistinguishable from a route that never worked. The rack's own rule is
	// that adding a device is the intent to use it; this asserts the shape that intent produces.
	//
	// POSITION DOES NOT ENTER IT, which is the other half of what was being tested: a modulator is read
	// through `sourceValue` when a destination resolves, not through the audio walk, so where it sits
	// in the chain cannot change what it publishes. The subcase below moves it and asserts exactly
	// that rather than leaving it to the reader.
	rack::RackEngine rack(kAxes, hostCatalogue());
	rack.prepare(48000.0, 4096);
	const size_t fx = rack.addDevice(filter::kTraits.tag);
	const size_t ms = rack.addDevice(mseg::kTraits.tag);
	REQUIRE(fx != rack::RackEngine::kNoSlot);
	REQUIRE(ms != rack::RackEngine::kNoSlot);

	const size_t cutoff = controlNamed(filter::kDescriptor, "Cutoff");
	const size_t ordinal = filter::kDescriptor.nthModulatableOfControl(cutoff);
	const backplane::Destination dest = test::ordinalOf(rack, fx, ordinal);
	const backplane::Source source = test::outputOf(rack, ms, 0);
	rack.setRouteAmount(source, dest, 0.5f);
	const float base = rack.fieldAt(fx, cutoff);

	std::vector<float> l(512, 0.0f), r(512, 0.0f);
	rack.setGate(true);
	for (int i = 0; i < 16; ++i) {
		rack.processStereo(l.data(), r.data(), 512, backplane::unknownBlock());
	}

	// THE SOURCE IS PUBLISHING — without this the displacement below could be some other route's.
	REQUIRE(rack.publishedOutputAt(ms, 0) > 0.0f);
	CHECK(rack.resolvedFieldForTest(fx, cutoff) > base);

	// AND THE BASE IS UNTOUCHED: a route displaces, it does not overwrite.
	// AND THE BASE IS UNTOUCHED: a route displaces, it does not overwrite.
	CHECK(rack.fieldAt(fx, cutoff) == doctest::Approx(base));

	SUBCASE("and the destination follows the source as it moves") {
	// **AND THE DESTINATION FOLLOWS IT AS IT MOVES, which is the claim a single displaced reading
	// cannot make.** The destination stuck at a static value while the shape ran, and
	// a case that asserted only "it is displaced" would have been green through exactly that. Both
	// numbers are recorded so a failure says WHICH one stopped moving.
	float previousSource = rack.publishedOutputAt(ms, 0);
	float previousResolved = rack.resolvedFieldForTest(fx, cutoff);
	size_t sourceMoves = 0;
	size_t resolvedMoves = 0;
	for (int i = 0; i < 6; ++i) {
		rack.processStereo(l.data(), r.data(), 512, backplane::unknownBlock());
		const float nowSource = rack.publishedOutputAt(ms, 0);
		const float nowResolved = rack.resolvedFieldForTest(fx, cutoff);
		if (nowSource != doctest::Approx(previousSource)) {
			++sourceMoves;
		}
		if (nowResolved != doctest::Approx(previousResolved)) {
			++resolvedMoves;
		}
		previousSource = nowSource;
		previousResolved = nowResolved;
	}
	REQUIRE(sourceMoves > 0);          // the shape is running at all
	CHECK(resolvedMoves == sourceMoves); // and the destination moved every time it did
	}

	SUBCASE("and the order of the two does not change the displacement") {
		// **AGAINST A SECOND ENGINE BUILT THE OTHER WAY ROUND, NOT AGAINST THIS ONE A BLOCK LATER.**
		// A first version moved the MSEG and compared to the value from before the move — which failed
		// at 12.6037 against 12.4486, because the shape had advanced one more block. That is the
		// modulator working, not the order mattering, and the case was measuring elapsed time rather
		// than position. An independent engine rendered the same number of blocks is the honest
		// reference.
		rack::RackEngine reversed(kAxes, hostCatalogue());
		reversed.prepare(48000.0, 4096);
		const size_t rms = reversed.addDevice(mseg::kTraits.tag);
		const size_t rfx = reversed.addDevice(filter::kTraits.tag);
		REQUIRE(rms != rack::RackEngine::kNoSlot);
		REQUIRE(rfx != rack::RackEngine::kNoSlot);
		reversed.setRouteAmount(
		    test::outputOf(reversed, rms, 0),
		    test::ordinalOf(reversed, rfx, ordinal), 0.5f);
		std::vector<float> rl(512, 0.0f), rr(512, 0.0f);
		reversed.setGate(true);
		for (int i = 0; i < 16; ++i) {
			reversed.processStereo(rl.data(), rr.data(), 512, backplane::unknownBlock());
		}
		// THE MODULATOR IN FRONT OF ITS DESTINATION LANDS THE SAME DISPLACEMENT, because a source is
		// read when a destination resolves rather than walked with the audio.
		CHECK(reversed.resolvedFieldForTest(rfx, cutoff)
		      == doctest::Approx(rack.resolvedFieldForTest(fx, cutoff)).epsilon(1e-4));
	}
}

TEST_CASE("MEASUREMENT VEHICLE (AS): a knob drag and an audio block share a device control") {
	// **THIS CASE EXISTS TO BE RUN UNDER ThreadSanitizer. It asserts almost nothing on its own, and
	// that is deliberate — it is an instrument, not a claim.**
	//
	// AS's safety argument rests on a data race I READ rather than measured, and the difference
	// matters enough to build a vehicle for it. The pair:
	//
	//   `RackEngine::setField`   writes `slot.base[control]` as a PLAIN float, UI thread
	//                            (rack_engine.cpp, the clamp-and-store at the end)
	//   `RackEngine::resolveInto` reads `slot.base[control]` as a PLAIN float, audio thread
	//                            (the resolve loop)
	//
	// Neither is atomic. `resolved[]` beside it went behind `atomic_ref` for BE, and `readings_`
	// always was; `base` never did. If that is a race, then EVERY KNOB DRAG RACES EVERY BLOCK in the
	// shipping rack, and it is the live defect AS removes — the one its own entry does not mention,
	// because the entry describes a free that step 2 already bought.
	//
	// **WHY A CASE AND NOT AN ARGUMENT.** ASan cannot see data races; our gate is ASan; so nothing we
	// run today can answer this. Reading the code predicts a report, and a prediction is not a
	// measurement — I asserted a mechanism from a correct observation once tonight already and it was
	// wrong. Under TSan this either names the pair or it does not, and the commit message says
	// "measured" or "read, not measured" accordingly.
	//
	// **WHAT IT ASSERTS ANYWAY, so it is not vacuous in an ordinary build:** that the drag's last
	// write is what the control ends at, and that the blocks actually ran. A vehicle that silently
	// did neither would report a clean TSan run for a thread pair that never overlapped.
	namespace filter = fx::filter;

	rack::RackEngine engine(kAxes, hostCatalogue());
	engine.prepare(48000.0, 4096);
	const size_t lane = engine.addDevice(filter::kTraits.tag);
	REQUIRE(lane != rack::RackEngine::kNoSlot);

	const size_t cutoff = controlNamed(filter::kDescriptor, "Cutoff");
	REQUIRE(cutoff < filter::kDescriptor.params.size());
	const device::ParamDesc& pd = filter::kDescriptor.params[cutoff];

	constexpr int kBlocks = 200;
	std::atomic<int> blocksRun { 0 };
	std::atomic<bool> stop { false };
	std::atomic<float> lastWritten { 0.0f };
	std::atomic<int> edits { 0 };

	// THE DRAG. A hand sweeping the cutoff, which is the commonest gesture in the product and the
	// exact call the editor makes — `setField` through the DeviceHost the cards are handed.
	std::thread drag([&] {
		float v = static_cast<float>(pd.min);
		const float span = static_cast<float>(pd.max - pd.min);
		while (!stop.load(std::memory_order_relaxed)) {
			v += span / 64.0f;
			if (v > static_cast<float>(pd.max)) {
				v = static_cast<float>(pd.min);
			}
			engine.setField(lane, cutoff, v);
			lastWritten.store(v, std::memory_order_relaxed);
			edits.fetch_add(1, std::memory_order_release);
		}
	});

	// **WAIT FOR THE DRAG TO ACTUALLY START, WHICH THIS DID NOT DO AND SHOULD HAVE.**
	//
	// The self-checks below assert that both threads did work — without them a silent TSan run could
	// be two threads that never met. But nothing MADE the drag run: under a loaded parallel ctest the
	// writer could be descheduled for the whole block loop, and then `lastWritten` is still 0 and the
	// case goes red for a scheduling reason rather than for the property it tests. Measured by the
	// integrator: red once in a full parallel run, then ok three times alone and 1/1 twice more.
	//
	// **A CHECK THAT CAN FAIL WITHOUT A DEFECT IS WORSE THAN NO CHECK**, because a red that means
	// nothing teaches the reader to re-roll — which is the habit BO and BU are both about. So the
	// vehicle now WAITS for the first edit and requires it, and the assertions afterwards are about
	// the code rather than about the scheduler.
	for (int spins = 0; edits.load(std::memory_order_acquire) == 0 && spins < 1000000; ++spins) {
		std::this_thread::yield();
	}
	REQUIRE(edits.load(std::memory_order_acquire) > 0);

	// AND THE BLOCKS, on this thread, exactly as the host would drive them.
	std::vector<float> l(256, 0.0f), r(256, 0.0f);
	engine.setGate(true);
	for (int i = 0; i < kBlocks; ++i) {
		engine.processStereo(l.data(), r.data(), 256, backplane::unknownBlock());
		blocksRun.fetch_add(1, std::memory_order_relaxed);
	}
	stop.store(true, std::memory_order_relaxed);
	drag.join();

	// THE VEHICLE ACTUALLY MOVED — both halves. Without these a silent no-op would read as a clean
	// TSan run.
	CHECK(blocksRun.load() == kBlocks);
	CHECK(lastWritten.load() != 0.0f);
	// AND THE CONTROL ENDS AT THE DRAG'S LAST WRITE, which is also the ordinary-build claim: a field
	// written from the editor is the field the engine holds.
	CHECK(engine.fieldAt(lane, cutoff) == doctest::Approx(lastWritten.load()));
}

TEST_CASE("MEASUREMENT VEHICLE (AS 2): a drag through the BRIDGE while blocks run") {
	// **THE VEHICLE THE FIX IS JUDGED BY, and the reason the first one will not do.**
	//
	// `MEASUREMENT VEHICLE (AS)` drags `RackEngine::setField` directly, and that is STILL RACY BY
	// DESIGN — commit 2 does not change the engine's raw API, it changes who is allowed to call it.
	// Running the old vehicle after the fix would prove nothing about the road the UI takes, and a
	// green from it would be the worst kind of evidence: true, irrelevant, and reassuring.
	//
	// So this one drags through the BRIDGE, which is what a knob does now, against a thread doing what
	// the plugin does — drain, then block, on one thread, because `process()` is where the drain lives.
	//
	// **WHAT A SILENT RUN MEANS AND WHAT IT DOES NOT.** Silent means TSan saw no unsynchronised access
	// on this path: the queue is an SPSC ring of atomics, and every engine write happens on the block
	// thread between blocks. It does NOT mean the engine is thread-safe — call `engine.setField` from
	// another thread and the old report comes straight back, which is exactly what the invariant now
	// written at those mutators says. The fix is a road, not a lock.
	namespace filter = fx::filter;

	rack::RackEngine engine(kAxes, hostCatalogue());
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);

	const DeviceInstanceId device = bridge.addDevice(filter::kTraits.tag);
	REQUIRE(device != kNoInstance);
	bridge.drain();
	const size_t lane = engine.slotOf(device); // where the ENGINE chose to keep it
	REQUIRE(lane != rack::RackEngine::kNoSlot);

	const size_t cutoff = controlNamed(filter::kDescriptor, "Cutoff");
	REQUIRE(cutoff < filter::kDescriptor.params.size());
	const device::ParamDesc& pd = filter::kDescriptor.params[cutoff];

	// **THE DRAG IS PACED TO THE DRAIN, AND THE FIRST VERSION WAS NOT — that is a real finding about
	// the vehicle, caught by its own self-check rather than by anybody reading it.**
	//
	// The first draft spun `bridge.setField` as fast as a core allows against 200 drains. TSan was
	// SILENT and `droppedEditsForTest()` came back **1,388,749**: the ring saturated, so "no report"
	// covered only the edits that happened to cross. `edits > 0` passed, so the threads did meet — but
	// half the self-check was red, and the honest reading was "the edits that crossed produced no
	// report", not "the path is clean". The check existed for exactly this and it earned its place.
	//
	// **A HAND IS SLOWER THAN THE DRAIN, AND THAT IS THE POINT.** A drag emits at most a frame's worth
	// of values — call it 60/s — while the drain runs once a block, ~200/s at 256 frames and 48 kHz.
	// The drain is the faster of the two by 3x, so the queue's depth at UI rates is ~1, against a ring
	// of 1024. Modelling that is what makes this vehicle a measurement of the product rather than of a
	// spin loop; a saturated queue measures the queue's capacity and nothing else.
	constexpr int kBlocks = 2000;
	std::atomic<bool> stop { false };
	std::atomic<int> edits { 0 };
	std::atomic<int> blocksRun { 0 };

	std::thread drag([&] {
		float v = static_cast<float>(pd.min);
		const float span = static_cast<float>(pd.max - pd.min);
		int seen = 0;
		while (!stop.load(std::memory_order_relaxed)) {
			// ONE EDIT PER BLOCK AT MOST — the hand waits for the world, as a hand does.
			const int now = blocksRun.load(std::memory_order_acquire);
			if (now == seen) {
				std::this_thread::yield();
				continue;
			}
			seen = now;
			v += span / 64.0f;
			if (v > static_cast<float>(pd.max)) {
				v = static_cast<float>(pd.min);
			}
			bridge.setField(device, cutoff, v);
			edits.fetch_add(1, std::memory_order_relaxed);
		}
	});

	// AND THE AUDIO THREAD, doing what `process()` does: drain, then the block.
	std::vector<float> l(256, 0.0f), r(256, 0.0f);
	engine.setGate(true);
	for (int i = 0; i < kBlocks; ++i) {
		bridge.drain();
		engine.processStereo(l.data(), r.data(), 256, backplane::unknownBlock());
		blocksRun.fetch_add(1, std::memory_order_release);
	}
	stop.store(true, std::memory_order_relaxed);
	drag.join();

	// BOTH HALVES RAN, or a silent TSan run would be a report about two threads that never met.
	CHECK(edits.load() > 0);
	// **AND NOTHING WAS DROPPED.** This is the half that failed on the first attempt and disqualified
	// the run; a silent TSan verdict means nothing about edits the ring refused.
	CHECK(bridge.droppedEditsForTest() == 0);
	// AND THE EDITS ARRIVED: the last drained value is what the engine holds.
	bridge.drain();
	CHECK(engine.fieldAt(lane, cutoff) == doctest::Approx(bridge.fieldAt(device, cutoff)));
}

TEST_CASE("the churn that lost an add: under a hand that adds, removes and drops interior while blocks run, the view is the hand's every time and the engine ends as the record") {
	// **THE LOST-ADD PROBE'S RECIPE, KEPT, AND ITS VERDICT NOW AN ASSERTION.** A device that disappeared
	// right after being added was a verb the drain refused out of the editor's sight (backlog CP);
	// two families were found with a forensic ring, and the fix then was an OVERLAY composed over a
	// seqlock'd mirror. It was a skipped vehicle because its result depended on preemption. Under
	// desired state there is no verb to refuse and no overlay to compose — the view
	// IS the record — so the same churn is a deterministic claim about the view and an eventual one
	// about the engine, and it runs in every suite.
	//
	// THE RECIPE: an audio thread doing `drain()` then `processStereo`, exactly as `process()` does; the
	// hand PACED ONE OPERATION PER BLOCK (unpaced it measures a queue, not the product); add until full,
	// then remove the front — with DROPS of the front to INTERIOR positions two in every four ops, in
	// pairs, because a drop to the end re-applied onto a chain already holding it is a no-op and reads
	// green under the mutations interior drops catch.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);
	bridge.primeForTest();

	std::atomic<bool> stop { false };
	std::atomic<int> blocks { 0 };
	std::thread audio([&] {
		std::vector<float> l(64, 0.0f), r(64, 0.0f);
		while (!stop.load(std::memory_order_relaxed)) {
			bridge.drain();
			engine.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
			blocks.fetch_add(1, std::memory_order_release);
		}
	});

	// **THE HAND'S OWN MODEL OF THE CHAIN** — what it believes the chain is after every edit it made,
	// exact by construction. Compared WHOLE against the view before every op (the view is the record,
	// so any difference at all is a defect, not a race), and against the ENGINE at the end.
	// BY ID — the device the hand made, wherever it stands (kill-lanes step 2).
	std::vector<DeviceInstanceId> model;
	int seen = 0, ops = 0, moves = 0, viewMismatches = 0;
	auto frontOfView = [&](const char*) -> DeviceInstanceId {
		bool same = bridge.chainLength() == model.size();
		for (size_t at = 0; same && at < model.size(); ++at) {
			same = bridge.deviceAtPosition(at) == model[at];
		}
		viewMismatches += same ? 0 : 1;
		return model.empty() ? kNoInstance : model.front();
	};
	// BOUNDED BY OPS, with a wall-clock backstop so a starved audio thread ends the run rather than the
	// suite — and the REQUIREs below then say it was starved, rather than judging a window never opened.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	while (ops < 4000 && std::chrono::steady_clock::now() < deadline) {
		const int now = blocks.load(std::memory_order_acquire);
		if (now == seen) {
			std::this_thread::yield();
			continue;
		}
		seen = now;
		if ((ops % 4 == 2 || ops % 4 == 3) && bridge.chainLength() >= 2) {
			(void)frontOfView("move");
			// BY ID, so the model and the bridge move the SAME device whatever the view said: a
			// mismatch above is then the view's alone, and the engine check at the end the drain's.
			// TO AN INTERIOR POSITION, alternating — a move to the END re-applied onto a chain already
			// holding it is a no-op, so an end-only churn cannot see an order converged wrongly.
			const size_t target = std::min<size_t>(1 + (ops % 2), model.size() - 1);
			if (bridge.moveDeviceTo(model.front(), target)) {
				const DeviceInstanceId moved = model.front();
				model.erase(model.begin());
				model.insert(model.begin() + static_cast<std::ptrdiff_t>(target), moved);
				++moves;
			}
		}
		else if (bridge.chainLength() < engine.slotCount()) {
			const DeviceInstanceId id = bridge.addDevice(filter::kTraits.tag);
			if (id != kNoInstance) {
				model.push_back(id);
			}
		}
		else {
			const DeviceInstanceId id = frontOfView("remove");
			bridge.removeDevice(id);
			const auto it = std::find(model.begin(), model.end(), id);
			if (it != model.end()) {
				model.erase(it);
			}
		}
		++ops;
	}
	stop.store(true, std::memory_order_relaxed);
	audio.join();
	// ONE LAST SERVICE AND DRAIN — whatever the hand did on its final pass, and any lane the engine could
	// not take while the parking was full, built again and taken. The engine is then the record.
	bridge.reapRetired();
	bridge.drain();

	MESSAGE("churn: ops=" << ops << " moves=" << moves << " blocks=" << blocks.load());
	// BOTH HALVES RAN, and the hand did every kind of op, or the verdict is about a window never opened.
	REQUIRE(blocks.load() > 0);
	REQUIRE(ops >= 400);
	REQUIRE(moves > 0);
	CHECK(viewMismatches == 0);
	CHECK(bridge.droppedEditsForTest() == 0);
	// THE ENGINE ENDS AS THE HAND'S CHAIN, device for device — the value, not a count.
	REQUIRE(engine.chainLength() == model.size());
	for (size_t at = 0; at < model.size(); ++at) {
		CAPTURE(at);
		const size_t lane = engine.deviceAtPosition(at);
		REQUIRE(lane != rack::RackEngine::kNoSlot);
		CHECK(engine.slotAtLane(lane).id == model[at]);
	}
	CHECK(bridge.pendingProvisionsForTest() == 0);
}

namespace {
/// BY TAG, NOT BY ADDRESS: the registry's `kDevices` holds descriptors by value, so the pointer the
/// bridge hands back is not `&filter::kDescriptor`, and a case that compared addresses would fail
/// for a reason that has nothing to do with the bridge.
bool isFilter(const device::Descriptor* d) {
	return d != nullptr && device::sameTag(d->traits.tag, filter::kTraits.tag);
}
/// WHICH DEVICE THE ENGINE RUNS AT `position` — by the id its slot carries, the bridge's currency since
/// kill-lanes step 2 — or `kNoInstance`.
DeviceInstanceId engineIdAt(const rack::RackEngine& engine, size_t position) {
	const size_t lane = engine.deviceAtPosition(position);
	return lane == rack::RackEngine::kNoSlot ? kNoInstance : engine.slotAtLane(lane).id;
}
} // namespace

TEST_CASE("a publish landing between the hand's remove and its add cannot make the view forget the remove, and the add lands at the engine's end") {
	// **THE LOST ADD'S SECOND MECHANISM** (backlog CP), traced on the LOST-ADD PROBE with a forensic
	// ring at the refusal, twice, identical:
	//
	//   main:  remove position 0 (lane L)        mirror: chain 3, lane L empty
	//   audio: publish, its pop loop having ended before the remove was pushed — order and LENGTH
	//          written back from an engine that still holds four; the lanes not yet reached
	//   main:  addDevice → nextFreeLane sees L empty, chainLength() reads 4 → Add(L, position 4)
	//   audio: Remove(L) → chain 3;  addDeviceAt(4, …, L): 4 > 3 → REFUSED, lane not live
	//
	// A position refusal, not a lane refusal — the family da50a2b's claim guard could not touch, which
	// is why its residual survived that fix. Two things close it and this case pins both: the hand's
	// view is COMPOSED from the mirror plus its own pending edits, so a publish cannot un-remove a
	// lane the hand has removed; and the add carries no position at all (`RackEdit::kAppend`).
	//
	// **THE PLANT IS A WHOLE `publish` BETWEEN THE TWO EDITS** — the worst case, not the traced half:
	// the mirror gets the engine's four devices back, lane L included. Under the old design that made
	// `chainLength()` read 4 and `nextFreeLane()` skip L; every precondition below is asserted so the
	// case cannot pass vacuously, and the verdict is the ENGINE's chain and the lane's liveness.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);
	bridge.primeForTest();
	for (size_t i = 0; i < kAxes.deviceSlots; ++i) {
		REQUIRE(bridge.addDevice(filter::kTraits.tag) != kNoInstance);
	}
	bridge.drain();
	REQUIRE(engine.chainLength() == kAxes.deviceSlots);

	const DeviceInstanceId gone = bridge.deviceAtPosition(0);
	REQUIRE(gone != kNoInstance);
	bridge.removeDevice(gone);
	REQUIRE(bridge.chainLength() == kAxes.deviceSlots - 1);
	REQUIRE(bridge.descriptorOf(gone) == nullptr);

	// THE PLANT. The engine has not drained the remove, so the mirror gets all four back — and the
	// hand's view must not.
	bridge.publish(engine);
	REQUIRE(bridge.mirrorDescriptorOfForTest(gone) != nullptr);
	CHECK(bridge.chainLength() == kAxes.deviceSlots - 1);
	CHECK(bridge.descriptorOf(gone) == nullptr);

	// (THE PRECONDITION THIS CASE ONCE NEEDED — the add taking the very pool index the remove left — is
	// gone with the lane: the engine chooses its slot when it adopts, kill-lanes step 3a. What remains to
	// prove is the view and the order.)
	const DeviceInstanceId added = bridge.addDevice(filter::kTraits.tag);
	REQUIRE(added != kNoInstance);
	// THE VIEW BEFORE THE DRAIN: the new device at the END, and the length is four.
	CHECK(bridge.chainLength() == kAxes.deviceSlots);
	CHECK(bridge.deviceAtPosition(kAxes.deviceSlots - 1) == added);
	CHECK(bridge.deviceAtPosition(0) != added);
	bridge.drain();

	CHECK(engine.chainLength() == kAxes.deviceSlots);
	CHECK(engine.slotOf(added) != rack::RackEngine::kNoSlot);
	CHECK(engine.slotOf(gone) == rack::RackEngine::kNoSlot);
	// AT THE END — where "add" means — and not back at the front it was removed from.
	CHECK(engineIdAt(engine, kAxes.deviceSlots - 1) == added);
	CHECK(engineIdAt(engine, 0) != added);
	// AND THE VIEW AGREES WITH THE ENGINE ONCE THE DRAIN HAS SPOKEN.
	CHECK(bridge.pendingProvisionsForTest() == 0);
	CHECK(bridge.deviceAtPosition(kAxes.deviceSlots - 1) == added);
}

TEST_CASE("the hand's add is never written into the mirror: the view says the device, the mirror's word stays the engine's, until the drain has published") {
	// **THE FIRST MECHANISM'S SHAPE, PINNED STRUCTURALLY** (backlog CP). `publish` used to guard a
	// lane the UI had claimed, and the guard was a check-then-act the audio thread could be preempted
	// inside — two adds went out for one lane. The property that makes that impossible, asserted: the
	// hand's add lives in the RECORD, and the mirror (the engine's word) is untouched by it — the
	// editor's view never was the mirror's, and no publish can take a device back from it.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);
	bridge.primeForTest();

	const DeviceInstanceId added = bridge.addDevice(filter::kTraits.tag);
	REQUIRE(added != kNoInstance);
	CHECK(bridge.mirrorDescriptorOfForTest(added) == nullptr);
	CHECK(isFilter(bridge.descriptorOf(added)));
	CHECK(bridge.pendingProvisionsForTest() == 1); // built, waiting for the drain

	// A PUBLISH BEFORE THE DRAIN CANNOT ERASE IT — there is nothing in the mirror to erase.
	bridge.publish(engine);
	CHECK(bridge.mirrorDescriptorOfForTest(added) == nullptr);
	CHECK(isFilter(bridge.descriptorOf(added)));

	bridge.drain();
	bridge.publish(engine); // the block's publish, after the drain — `process()`'s order
	CHECK(bridge.pendingProvisionsForTest() == 0);
	CHECK(isFilter(bridge.mirrorDescriptorOfForTest(added)));
	CHECK(isFilter(bridge.descriptorOf(added)));
	CHECK(engine.slotOf(added) != rack::RackEngine::kNoSlot);
	CHECK(engine.chainLength() == 1);
}

TEST_CASE("a remove and a re-add of one lane queued in one block: the later edit wins in the view, and the engine ends with the device at the end") {
	// THE OVERLAY'S ORDERING RULE, BY VALUE: one entry per lane, the latest edit answers. The harness
	// does exactly this on every full rack — remove position 0, add — and the view must show the lane
	// gone from the front and back at the end before the drain has run, then agree with the engine.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);
	bridge.primeForTest();
	for (size_t i = 0; i < kAxes.deviceSlots; ++i) {
		REQUIRE(bridge.addDevice(filter::kTraits.tag) != kNoInstance);
	}
	bridge.drain();
	const DeviceInstanceId gone = bridge.deviceAtPosition(0);
	bridge.removeDevice(gone);
	CHECK(bridge.positionOf(gone) == rack::RackBridge::kNoSlot);
	const DeviceInstanceId back = bridge.addDevice(filter::kTraits.tag);
	REQUIRE(back != kNoInstance);
	CHECK(back != gone); // a re-add is a new device: ids are never reissued
	CHECK(bridge.positionOf(back) == kAxes.deviceSlots - 1);
	CHECK(bridge.chainLength() == kAxes.deviceSlots);
	CHECK(isFilter(bridge.descriptorOf(back)));

	bridge.drain();
	CHECK(engine.chainLength() == kAxes.deviceSlots);
	CHECK(engine.slotOf(gone) == rack::RackEngine::kNoSlot);
	CHECK(engineIdAt(engine, kAxes.deviceSlots - 1) == back);
	CHECK(bridge.positionOf(back) == kAxes.deviceSlots - 1);
}

TEST_CASE("a drop draws where the hand put it through the same composition as every other chain read, and a publish before its drain cannot undo it") {
	// THE DROP IS THE RECORD'S (backlog CP, the follow-on, and then desired state). `movePosition` used to
	// rotate the mirror's order in place — a word the drain also writes — and a publish between the drop
	// and its drain put the old order back for a frame. The view reads the record now; no publish of the
	// engine's word can move it.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);
	bridge.primeForTest();
	for (size_t i = 0; i < kAxes.deviceSlots; ++i) {
		REQUIRE(bridge.addDevice(filter::kTraits.tag) != kNoInstance);
	}
	bridge.drain();
	const DeviceInstanceId first = bridge.deviceAtPosition(0);
	const DeviceInstanceId second = bridge.deviceAtPosition(1);
	REQUIRE(first != kNoInstance);
	REQUIRE(first != second);

	REQUIRE(bridge.moveDevice(0, kAxes.deviceSlots - 1));
	CHECK(bridge.deviceAtPosition(kAxes.deviceSlots - 1) == first);
	CHECK(bridge.positionOf(first) == kAxes.deviceSlots - 1);
	CHECK(bridge.deviceAtPosition(0) == second);
	CHECK(bridge.chainLength() == kAxes.deviceSlots);

	// A PUBLISH BEFORE THE DRAIN: the engine still has the old order, and the view must keep the drop.
	bridge.publish(engine);
	CHECK(bridge.deviceAtPosition(kAxes.deviceSlots - 1) == first);
	CHECK(bridge.deviceAtPosition(0) == second);

	bridge.drain();
	CHECK(engineIdAt(engine, kAxes.deviceSlots - 1) == first);
	CHECK(engineIdAt(engine, 0) == second);
	CHECK(bridge.pendingProvisionsForTest() == 0);
	CHECK(bridge.deviceAtPosition(kAxes.deviceSlots - 1) == first);
}

TEST_CASE("a move names the DEVICE, so a remove ahead of it in the queue cannot make it move a different one") {
	// THE MOVE'S SECOND GUESS, RETIRED: a queued edit used to carry a FROM-position, and a remove ahead of
	// it in the same drain shifted every position under it — the drain then moved whatever stood at that
	// number. There is no queue to be ahead in now: the record applies the remove and the drop in the
	// order the hand made them, and the engine converges to the result.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);
	bridge.primeForTest();
	for (size_t i = 0; i < kAxes.deviceSlots; ++i) {
		REQUIRE(bridge.addDevice(filter::kTraits.tag) != kNoInstance);
	}
	bridge.drain();
	const DeviceInstanceId front = bridge.deviceAtPosition(0);
	const DeviceInstanceId last = bridge.deviceAtPosition(kAxes.deviceSlots - 1);

	bridge.removeDevice(front);
	REQUIRE(bridge.chainLength() == kAxes.deviceSlots - 1);
	// IN THE VIEW the last device now stands at position 2; the hand drags it to the front.
	REQUIRE(bridge.deviceAtPosition(kAxes.deviceSlots - 2) == last);
	REQUIRE(bridge.moveDevice(kAxes.deviceSlots - 2, 0));
	CHECK(bridge.deviceAtPosition(0) == last);

	bridge.drain();
	CHECK(engine.chainLength() == kAxes.deviceSlots - 1);
	CHECK(engineIdAt(engine, 0) == last);
	CHECK(engine.slotOf(front) == rack::RackEngine::kNoSlot);
	CHECK(bridge.deviceAtPosition(0) == last);
}

TEST_CASE("add before, through the bridge: the view draws the newcomer before THAT card at once, and the drain puts it there by the anchor's position at apply time") {
	// THE RACK PLUGIN'S HALF OF CR: the record inserts before the anchor wherever the anchor stands when
	// the hand asks, and a move or remove of the anchor made in the same block is simply the next change
	// to the record. Asserted by lane identities at positions, in the view and then in the engine.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);
	bridge.primeForTest();
	REQUIRE(bridge.addDevice(filter::kTraits.tag) != kNoInstance);
	REQUIRE(bridge.addDevice(filter::kTraits.tag) != kNoInstance);
	bridge.drain();
	const DeviceInstanceId a = bridge.deviceAtPosition(0);
	const DeviceInstanceId b = bridge.deviceAtPosition(1);
	REQUIRE(engine.chainLength() == 2);

	SUBCASE("before the second device") {
		const DeviceInstanceId born = bridge.addDeviceBefore(b, filter::kTraits.tag);
		REQUIRE(born != kNoInstance);
		// THE VIEW, BEFORE THE DRAIN: a, born, b.
		CHECK(bridge.chainLength() == 3);
		CHECK(bridge.deviceAtPosition(0) == a);
		CHECK(bridge.deviceAtPosition(1) == born);
		CHECK(bridge.deviceAtPosition(2) == b);
		// A PUBLISH BEFORE THE DRAIN CANNOT UNDO IT.
		bridge.publish(engine);
		CHECK(bridge.deviceAtPosition(1) == born);
		bridge.drain();
		CHECK(engine.chainLength() == 3);
		CHECK(engineIdAt(engine, 0) == a);
		CHECK(engineIdAt(engine, 1) == born);
		CHECK(engineIdAt(engine, 2) == b);
		CHECK(bridge.pendingProvisionsForTest() == 0);
		CHECK(bridge.deviceAtPosition(1) == born);
	}
	SUBCASE("the anchor moved to the front in the same block: the insert lands before it THERE") {
		REQUIRE(bridge.moveDeviceTo(b, 0)); // view: b, a
		const DeviceInstanceId born = bridge.addDeviceBefore(b, filter::kTraits.tag);
		REQUIRE(born != kNoInstance);
		CHECK(bridge.deviceAtPosition(0) == born);
		CHECK(bridge.deviceAtPosition(1) == b);
		bridge.drain();
		CHECK(engineIdAt(engine, 0) == born);
		CHECK(engineIdAt(engine, 1) == b);
		CHECK(engineIdAt(engine, 2) == a);
	}
	SUBCASE("the anchor removed in the same block: the insert appends, in the view and in the engine") {
		const DeviceInstanceId born = bridge.addDeviceBefore(b, filter::kTraits.tag);
		REQUIRE(born != kNoInstance);
		bridge.removeDevice(b);
		CHECK(bridge.chainLength() == 2);
		CHECK(bridge.deviceAtPosition(1) == born);
		bridge.drain();
		CHECK(engine.chainLength() == 2);
		CHECK(engineIdAt(engine, 1) == born);
	}
	SUBCASE("an anchor the view does not hold is refused, not appended") {
		CHECK(bridge.addDeviceBefore(b + 100, filter::kTraits.tag) == kNoInstance);
		CHECK(bridge.chainLength() == 2);
		CHECK(bridge.pendingProvisionsForTest() == 0);
	}
}

//------------------------------------------------------------------------------------------------//
// THE RECORD THE RACK IS (desired state, in both hosts)                                      //
//------------------------------------------------------------------------------------------------//

TEST_CASE("a field written through the bridge reads back as the engine will use it, before the drain and after") {
	// THE RECORD HOLDS THE NUMBER THE ENGINE USES — `device::coerceControl`, one rule for both — or the
	// editor would draw a value past a control's range that the engine clamps away, and the record would
	// save it. Before the drain the engine has not seen it at all, which is the half that shows the
	// view is the record's.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);
	const DeviceInstanceId device = bridge.addDevice(filter::kTraits.tag);
	REQUIRE(device != kNoInstance);
	bridge.drain();
	const size_t lane = engine.slotOf(device); // where the ENGINE chose to keep it
	REQUIRE(lane != rack::RackEngine::kNoSlot);
	size_t clamped = filter::kDescriptor.params.size();
	for (size_t c = 0; c < filter::kDescriptor.params.size(); ++c) {
		if (!filter::kDescriptor.params[c].offsetsMayLeaveRange && !filter::kDescriptor.params[c].stepped) {
			clamped = c;
			break;
		}
	}
	REQUIRE(clamped < filter::kDescriptor.params.size());
	const float max = static_cast<float>(filter::kDescriptor.params[clamped].max);

	bridge.setField(device, clamped, max + 1000.0f);
	CHECK(bridge.fieldAt(device, clamped) == max); // the view, at once, coerced
	CHECK(engine.fieldAt(lane, clamped) != max);   // the engine has not heard it
	bridge.drain();
	CHECK(engine.fieldAt(lane, clamped) == max);
	CHECK(test::rowOf(bridge.patch().device.at(0), filter::kDescriptor).at(clamped) == max); // and what a save would write
}

TEST_CASE("a bridge built over an engine that already runs devices takes them as its record and keeps every instance") {
	// **THE ENGINE AS IT IS FOUND BECOMES THE RECORD** — devices in order, rows and routes — and the
	// engine keeps running them (`Converger::adoptRunning`). Without that the first drain would see
	// devices it never built and rebuild every one: state lost, and a click where nothing was edited.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	const size_t env = engine.addDevice(dahdsr::kTraits.tag);
	const size_t filt = engine.addDevice(filter::kTraits.tag);
	REQUIRE(env != rack::RackEngine::kNoSlot);
	REQUIRE(filt != rack::RackEngine::kNoSlot);
	const backplane::Destination dest = test::controlOf(engine, filt, filter::kDescriptor.controlOfNthModulatableBesideEnable(0));
	const backplane::Source src = test::outputOf(engine, env, 0);
	engine.setRouteAmount(src, dest, 0.5f);
	REQUIRE(engine.routeAmount(src, dest) == doctest::Approx(0.5f));
	const void* envStorage = engine.slotAtLane(env).storage.get();
	const void* filtStorage = engine.slotAtLane(filt).storage.get();

	rack::RackBridge bridge(engine); // seeds, and settles once
	CHECK(bridge.chainLength() == 2);
	// THE RECORD HOLDS THE SAME DEVICES, IN ORDER, UNDER THE IDS THE ENGINE KNOWS THEM BY (kill-lanes step
	// 3a) — one device, one id, on both sides, so the first drain finds each one already running.
	const DeviceInstanceId envId = bridge.deviceAtPosition(0);
	const DeviceInstanceId filtId = bridge.deviceAtPosition(1);
	CHECK(envId == engine.slotAtLane(env).id);
	CHECK(filtId == engine.slotAtLane(filt).id);
	CHECK(bridge.routeAmount(backplane::Source::output(envId, 0),
	                         backplane::Destination::control(filtId, filter::kDescriptor.controlOfNthModulatableBesideEnable(0)))
	      == doctest::Approx(0.5f));
	bridge.drain();
	bridge.drain();
	CHECK(engine.slotAtLane(env).storage.get() == envStorage);
	CHECK(engine.slotAtLane(filt).storage.get() == filtStorage);
	// AND THE ENGINE RESOLVES THE ROUTE IT HAD, NOW FROM THE RECORD.
	CHECK(engine.routeAmount(src, dest) == doctest::Approx(0.5f));
}

TEST_CASE("a device this build cannot run survives a load and a save through the bridge, in its place") {
	// THE RECORD KEEPS WHAT THE ENGINE CANNOT HOLD. The engine-only save could not write a device it
	// never built, so a patch holding one lost it on the first save; the record keeps it where it was.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);
	rack::RackPatch patch;
	patch.present = true;
	patch.macro.assign(kAxes.coreSources, 0.0f);
	const auto device = [](const char* tag, rack::RackPatch::Value first) {
		rack::RackPatch::Device d;
		d.tag = rack::patchTagOf(tag);
		d.values.push_back(first);
		return d;
	};
	// THE TWO FILTERS TOLD APART BY THEIR FIRST CONTROL (Enabled, a choice, stored by label) — values from
	// its contract. The unknown device carries a key this build has never heard of, and must keep it.
	patch.device.push_back(device(filter::kTraits.tag, rack::RackPatch::Value::label("filter_enabled", "Off")));
	patch.device.push_back(device("zzzz", rack::RackPatch::Value::of("zz_level", 7.0f)));
	patch.device.push_back(device(filter::kTraits.tag, rack::RackPatch::Value::label("filter_enabled", "On")));
	REQUIRE(bridge.loadPatch(patch).routes.count == 0);
	bridge.drain();
	CHECK(engine.chainLength() == 2); // the engine runs what it can

	// IN ITS PLACE — its position in the chain's order, which is the only place a file names.
	const rack::RackPatch saved = bridge.patch();
	REQUIRE(saved.device.size() == 3);
	CHECK(saved.device[1].tag == rack::patchTagOf("zzzz"));
	CHECK(saved.device[1].values == std::vector<rack::RackPatch::Value> { rack::RackPatch::Value::of("zz_level", 7.0f) });
	CHECK(saved.device[2].tag == saved.device[0].tag);
	CHECK(test::rowOf(saved.device[0], filter::kDescriptor).front() == 0.0f);
	CHECK(test::rowOf(saved.device[2], filter::kDescriptor).front() == 1.0f);
}

TEST_CASE("a save made straight after an edit holds the edit — before any drain") {
	// THE RECORD IS WHAT IS SAVED, and it is current the moment the hand lets go. The engine-side save
	// wrote the engine, which is at most a drain behind — so a host saving on the heels of an edit, with
	// the transport stopped, saved the rack as it was before the edit.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);
	REQUIRE(bridge.addDevice(filter::kTraits.tag) != kNoInstance);
	REQUIRE(bridge.addDevice(dahdsr::kTraits.tag) != kNoInstance);
	CHECK(engine.chainLength() == 0); // no drain yet
	const rack::RackPatch saved = bridge.patch();
	REQUIRE(saved.device.size() == 2);
	CHECK(device::sameTag(saved.device[0].tag.data(), filter::kTraits.tag));
	CHECK(device::sameTag(saved.device[1].tag.data(), dahdsr::kTraits.tag));
}

//--------------------------------------------------------------------------------------------------//
// THE ALLOCATION COUNTER — backlog CF. A replacement operator new/delete for THIS TEST BINARY ONLY,   //
// counting while a case holds the gate open. It says what a code path allocates when run in this     //
// process; it says nothing about the plugin's own process, whose allocator is not hooked.            //
//--------------------------------------------------------------------------------------------------//
namespace {
std::atomic<bool> g_countAllocs { false };
std::atomic<uint64_t> g_allocs { 0 };
std::atomic<uint64_t> g_frees { 0 };
struct AllocWindow {
	AllocWindow() {
		g_allocs.store(0);
		g_frees.store(0);
		g_countAllocs.store(true);
	}
	~AllocWindow() { g_countAllocs.store(false); }
	uint64_t allocs() const { return g_allocs.load(); }
	uint64_t frees() const { return g_frees.load(); }
};
void noteAlloc() noexcept {
	if (g_countAllocs.load(std::memory_order_relaxed)) {
		g_allocs.fetch_add(1, std::memory_order_relaxed);
	}
}
void noteFree() noexcept {
	if (g_countAllocs.load(std::memory_order_relaxed)) {
		g_frees.fetch_add(1, std::memory_order_relaxed);
	}
}
} // namespace

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
[[gnu::noinline]] void* operator new(std::size_t n) {
	noteAlloc();
	void* p = feathers::test::heapAlloc(n == 0 ? 1 : n);
	if (p == nullptr) {
		throw std::bad_alloc();
	}
	return p;
}
[[gnu::noinline]] void* operator new[](std::size_t n) { return operator new(n); }
[[gnu::noinline]] void* operator new(std::size_t n, std::align_val_t al) {
	noteAlloc();
	void* p = nullptr;
	const std::size_t a = static_cast<std::size_t>(al);
	if (feathers::test::heapPosixMemalign(&p, a < sizeof(void*) ? sizeof(void*) : a, n == 0 ? 1 : n) != 0) {
		throw std::bad_alloc();
	}
	return p;
}
[[gnu::noinline]] void* operator new[](std::size_t n, std::align_val_t al) { return operator new(n, al); }
[[gnu::noinline]] void operator delete(void* p) noexcept {
	noteFree();
	feathers::test::heapFree(p);
}
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { operator delete(p); }
void operator delete(void* p, std::align_val_t) noexcept { operator delete(p); }
void operator delete[](void* p, std::align_val_t) noexcept { operator delete(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { operator delete(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { operator delete(p); }
#pragma GCC diagnostic pop

TEST_CASE("[measure] what each kind of drain ALLOCATES and FREES, counted through this binary's operator new") {
	// BACKLOG CF: `drain()` runs on the audio thread and the rule is no allocation there after
	// activate. This is the map — every edit kind drained once, allocations and frees counted — so the
	// fix is aimed at what allocates rather than at what the entry named. Reported, not judged: the
	// judging case comes with the fix.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);
	bridge.primeForTest();
	const auto drained = [&](const char* what) {
		uint64_t allocs = 0, frees = 0;
		{
			AllocWindow w;
			bridge.drain();
			allocs = w.allocs();
			frees = w.frees();
		}
		// REPORTED WITH THE WINDOW CLOSED: the message builds strings, and a count that included its own
		// reporting was the first draft's number.
		MESSAGE("CF " << std::string(what) << ": allocs=" << allocs << " frees=" << frees);
	};
	const DeviceInstanceId first = bridge.addDevice(filter::kTraits.tag);
	REQUIRE(first != kNoInstance);
	drained("add (first use of lane 0)");
	REQUIRE(bridge.addDevice(filter::kTraits.tag) != kNoInstance);
	drained("add (first use of lane 1)");
	const DeviceInstanceId second = bridge.deviceAtPosition(1);
	REQUIRE(bridge.addDeviceBefore(second, filter::kTraits.tag) != kNoInstance);
	drained("add before (first use of lane 2)");
	REQUIRE(bridge.moveDevice(0, 2));
	drained("move");
	bridge.setRouteAmount(backplane::Source::core(0),
	                      backplane::Destination::control(first, filter::kDescriptor.controlOfNthModulatableBesideEnable(0)), 0.5f);
	drained("route");
	bridge.setField(first, 0, 0.3f);
	drained("field");
	bridge.removeDevice(second);
	drained("remove (no block in flight: destroyed at once)");
	REQUIRE(bridge.addDevice(filter::kTraits.tag) != kNoInstance);
	drained("re-add on the freed lane");
	{
		const DeviceInstanceId victim = bridge.deviceAtPosition(0);
		engine.enterBlockForTest();
		bridge.removeDevice(victim);
		drained("remove (block in flight: parked)");
		engine.leaveBlockForTest();
		uint64_t allocs = 0, frees = 0;
		{
			AllocWindow w;
			engine.reapRetiredForTest();
			allocs = w.allocs();
			frees = w.frees();
		}
		MESSAGE("CF reap of the parked device: allocs=" << allocs << " frees=" << frees);
	}
	// ATTRIBUTION OF THE REMOVE'S ALLOCATIONS: the graph alone, add three and remove one.
	{
		backplane::InstanceCounter counter;
		backplane::Graph g(kAxes, 1, 8, counter);
		const feathers::DeviceInstanceId ids[3] = { 11, 12, 13 };
		for (size_t i = 0; i < 3; ++i) {
			REQUIRE(g.restore(filter::kDescriptor, ids[i], i));
			REQUIRE(g.insert(0, i, ids[i]));
		}
		uint64_t a1 = 0, f1 = 0;
		{
			AllocWindow w;
			g.remove(ids[1]); // recompute runs inside it
			a1 = w.allocs();
			f1 = w.frees();
		}
		MESSAGE("CF graph remove alone (recompute inside): allocs=" << a1 << " frees=" << f1);
	}
	// AND THE ENGINE ALONE: the remove, in its own window. (There was a second window here for the
	// route table's slice clear — `clearDeviceSlotRoutes` on the engine's table. Every route is an edge
	// since kill-lanes step 2 and the table is gone; a device's routes leave inside `Graph::remove`, which
	// the window below already counts.)
	{
		rack::RackEngine e2 { kAxes, hostCatalogue() };
		e2.prepare(48000.0, 4096);
		REQUIRE(e2.addDevice(filter::kTraits.tag) != rack::RackEngine::kNoSlot);
		REQUIRE(e2.addDevice(filter::kTraits.tag) != rack::RackEngine::kNoSlot);
		uint64_t a2 = 0, f2 = 0;
		{
			AllocWindow w;
			e2.removeDeviceAtLane(1);
			a2 = w.allocs();
			f2 = w.frees();
		}
		MESSAGE("CF engine alone: removeDeviceAtLane allocs=" << a2 << " frees=" << f2);
	}
}

TEST_CASE("the drain allocates nothing and frees nothing: every edit kind, a re-add, a parked remove, and a whole patch load") {
	// **BACKLOG CF, THE JUDGING CASE.** CLAUDE.md: no allocation on the audio thread after activate;
	// `drain()` is the audio thread. Every kind of edit is drained inside a counting window and the
	// count must be zero both ways — a FREE is the same rule from the other side.
	//
	// **WHAT THIS COUNTS AND WHAT IT DOES NOT.** The counter is this test binary's replacement
	// operator new/delete (top of this section). It hooks THIS PROCESS; the plugin's own allocator in
	// a host is not hooked and cannot be from here. So a green says the drain's CODE PATH allocates
	// nothing when run in this process — the same code the plugin runs — and says nothing about the
	// process a host puts it in. A device that allocated inside its own `process` would be caught here
	// too, because it runs under the same hook; none does.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);
	bridge.primeForTest();
	const auto drainedClean = [&](const char* what) {
		uint64_t allocs = 0, frees = 0;
		{
			AllocWindow w;
			bridge.drain();
			allocs = w.allocs();
			frees = w.frees();
		}
		CAPTURE(std::string(what));
		CHECK(allocs == 0);
		CHECK(frees == 0);
	};
	const DeviceInstanceId first = bridge.addDevice(filter::kTraits.tag);
	REQUIRE(first != kNoInstance);
	drainedClean("add");
	REQUIRE(bridge.addDevice(filter::kTraits.tag) != kNoInstance);
	drainedClean("second add");
	const DeviceInstanceId second = bridge.deviceAtPosition(1);
	REQUIRE(bridge.addDeviceBefore(second, filter::kTraits.tag) != kNoInstance);
	drainedClean("add before");
	REQUIRE(bridge.moveDevice(0, 2));
	drainedClean("move");
	bridge.setRouteAmount(backplane::Source::core(0),
	                      backplane::Destination::control(first, filter::kDescriptor.controlOfNthModulatableBesideEnable(0)), 0.5f);
	drainedClean("route");
	bridge.setField(first, 0, 0.3f);
	drainedClean("field");
	bridge.removeDevice(second);
	drainedClean("remove, no block in flight");
	REQUIRE(bridge.addDevice(filter::kTraits.tag) != kNoInstance);
	drainedClean("re-add on a lane whose rows kept their buffers");
	engine.enterBlockForTest();
	bridge.removeDevice(bridge.deviceAtPosition(0));
	drainedClean("remove with a block in flight");
	engine.leaveBlockForTest();

	// A WHOLE PATCH LOAD: the record replaced on this thread (every device built here), then the previous
	// chain removed and every device of the patch adopted, in one drain.
	rack::RackPatch patch;
	patch.present = true;
	patch.macro.assign(kAxes.coreSources, 0.0f);
	for (size_t lane = 0; lane < kAxes.deviceSlots; ++lane) {
		rack::RackPatch::Device d;
		d.tag = rack::patchTagOf(filter::kTraits.tag);
		test::setRow(d, filter::kDescriptor, std::vector<float>(filter::kDescriptor.params.size(), 0.25f));
		patch.device.push_back(d);
	}
	REQUIRE(bridge.loadPatch(patch).routes.count == 0);
	drainedClean("a whole patch load");
	CHECK(engine.chainLength() == kAxes.deviceSlots);

	// AND THE MESSAGE THREAD'S REAP IS WHERE THE FREES ARE — nonzero, which is what proves the drains
	// above had something to free and did not.
	uint64_t reapFrees = 0;
	{
		AllocWindow w;
		bridge.reapRetired();
		reapFrees = w.frees();
	}
	CHECK(reapFrees > 0);
	CHECK(engine.retiredCountForTest() == 0);
}

TEST_CASE("a full parking refuses the remove: the device stays, nothing is freed on the drain, and once the reap makes room the engine finishes the remove by itself") {
	// **THE FULL-CHANNEL PATH, WHICH IS THE CASE THAT MATTERS** (backlog CF):
	// the drain must neither block, drop nor free when the parking is full. The engine declines, the
	// device is exactly as it was — and under desired state that is not the end of
	// it: the RECORD still says the device is gone, so once the reap makes room the next drain completes
	// the remove without the hand repeating it. The verb protocol needed a refusal counter and a second
	// click; this needs neither.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	// ONE LIVE DEVICE, SEATED BEFORE THE BRIDGE — which takes the engine as it finds it. (A device the
	// engine gained behind a bridge's back would not be in its record, and the next drain would converge
	// the engine away from it.)
	const size_t live = engine.addDevice(filter::kTraits.tag);
	REQUIRE(live != rack::RackEngine::kNoSlot);
	rack::RackBridge bridge(engine);
	REQUIRE(bridge.chainLength() == 1); // the record took it
	REQUIRE(engine.slotAtLane(live).live());

	bridge.removeDevice(bridge.deviceAtPosition(0));
	CHECK(bridge.chainLength() == 0); // the hand's view, at once
	// FILL THE PARKING NOW, between the hand's edit and the drain: every commit reaps first, so the
	// parking cannot fill while the hand is active — only something reaching the engine behind it can,
	// with devices that come and go before any drain sees them.
	const size_t capacity = engine.retiredCapacityForTest();
	while (engine.retiredCountForTest() < capacity) {
		const size_t lane = engine.addDevice(filter::kTraits.tag);
		REQUIRE(lane != rack::RackEngine::kNoSlot);
		REQUIRE(engine.removeDeviceAtLane(lane));
	}
	uint64_t allocs = 0, frees = 0;
	{
		AllocWindow w;
		bridge.drain();
		allocs = w.allocs();
		frees = w.frees();
	}
	CHECK(allocs == 0);
	CHECK(frees == 0);
	CHECK(engine.chainLength() == 1);
	CHECK(engine.slotAtLane(live).live());
	CHECK(engine.retiredCountForTest() == capacity);

	// THE REAP MAKES ROOM, ON THE MESSAGE THREAD — and the NEXT DRAIN FINISHES THE REMOVE, the hand
	// having done nothing more.
	bridge.reapRetired();
	CHECK(engine.retiredCountForTest() == 0);
	bridge.drain();
	CHECK(engine.chainLength() == 0);
	CHECK_FALSE(engine.slotAtLane(live).live());
	CHECK(engine.retiredCountForTest() == 1);
}

TEST_CASE("a provision the engine refuses is handed back live, and a refused load's provisions die on the message thread") {
	// **ONE OWNER** (backlog CF): a provisioned device that is never adopted
	// is destroyed by whoever holds it, on that holder's thread. Engine-level: the refusal returns the
	// provision intact. Bridge-level: a load whose adopts are refused leaves its provisions in the
	// bridge, and the message thread's reap frees them — the frees are counted THERE and not on the
	// drain.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);

	SUBCASE("engine: a second provision under an id the engine already runs comes back live") {
		// ONE ID IS ONE DEVICE (kill-lanes step 3a): the engine chooses the slot, so a provision cannot be
		// refused for a slot being taken — only for naming a device that is already there.
		constexpr DeviceInstanceId kId = 50;
		rack::RackEngine::Provision first = engine.provision(filter::kTraits.tag);
		rack::RackEngine::Provision second = engine.provision(filter::kTraits.tag);
		REQUIRE(first.live());
		REQUIRE(second.live());
		const size_t slot = engine.adoptDevice(0, std::move(first), kId);
		REQUIRE(slot != rack::RackEngine::kNoSlot);
		CHECK_FALSE(first.live());
		CHECK(engine.slotOf(kId) == slot);
		CHECK(engine.slotAtLane(slot).id == kId); // the id it was handed, not one of its own
		CHECK(engine.adoptDevice(1, std::move(second), kId) == rack::RackEngine::kNoSlot);
		CHECK(second.live()); // still the caller's, still whole
		CHECK(engine.chainLength() == 1);
	}
	SUBCASE("bridge: a load with the parking full takes its device at once, keeps the old one until the reap makes room, and the engine then becomes the patch") {
		// **WHAT CHANGED WITH KILL-LANES STEP 3a, AND WHY IT IS BETTER.** The patch's device had to land on
		// the lane the old device held, so a remove the full parking declined also REFUSED the arrival: the
		// engine stayed wholly the old rack. The engine chooses its slot now, so the wanted device is adopted
		// at once in a free one, and only the unwanted one lingers — until the reap makes room.
		// THE LIVE DEVICE BEFORE THE BRIDGE — see the full-parking case for why.
		const size_t old = engine.addDevice(filter::kTraits.tag);
		REQUIRE(old == 0);
		rack::RackBridge bridge(engine);
		const void* oldStorage = engine.slotAtLane(0).storage.get();
		rack::RackPatch patch;
		patch.present = true;
		patch.macro.assign(kAxes.coreSources, 0.0f);
		rack::RackPatch::Device d;
		d.tag = rack::patchTagOf(filter::kTraits.tag);
		patch.device.push_back(d); // a new device: the old one's remove is what the full parking declines

		// THE LOAD BUILDS ITS DEVICE HERE, reaping first by design — so the parking is filled AFTER it,
		// with devices that come and go before any drain sees them.
		REQUIRE(bridge.loadPatch(patch).routes.count == 0);
		const size_t capacity = engine.retiredCapacityForTest();
		while (engine.retiredCountForTest() < capacity) {
			const size_t lane = engine.addDevice(filter::kTraits.tag);
			REQUIRE(lane != rack::RackEngine::kNoSlot);
			REQUIRE(engine.removeDeviceAtLane(lane));
		}
		uint64_t frees = 0;
		{
			AllocWindow w;
			bridge.drain();
			frees = w.frees();
		}
		CHECK(frees == 0);
		// THE OLD DEVICE STAYS (its remove was declined) AND THE PATCH'S DEVICE IS ALREADY RUNNING, in a slot
		// of the engine's own — the arrival did not need the old one's place.
		const DeviceInstanceId patched = bridge.deviceAtPosition(0);
		REQUIRE(patched != kNoInstance);
		CHECK(engine.chainLength() == 2);
		CHECK(engine.slotAtLane(0).storage.get() == oldStorage);
		CHECK(engine.slotOf(patched) != rack::RackEngine::kNoSlot);
		uint64_t reapFrees = 0;
		{
			AllocWindow w;
			bridge.reapRetired();
			reapFrees = w.frees();
		}
		CHECK(reapFrees > 0);
		// **AND THE ENGINE THEN BECOMES THE PATCH** — the reap made room, and the next drain removes what the
		// patch does not name. Nobody loaded twice.
		bridge.drain();
		CHECK(engine.chainLength() == 1);
		CHECK(engine.slotAtLane(engine.deviceAtPosition(0)).id == patched);
		CHECK_FALSE(engine.slotAtLane(0).live());
	}
}

// "BETWEEN THE DRAIN'S PUBLISH AND ITS COUNTER STORE" (backlog CP3) STOOD HERE and went with the window it
// stood in: the overlay's correctness rested on the order of two stores in the drain, and there is no
// overlay and no counter now (desired state, in both hosts). What it protected — a lane the
// engine is about to hold is never offered to the hand twice — is the record's by construction, and
// "the hand's add is never written into the mirror" above asserts it before and after a drain.

TEST_CASE("the rack's mayRoute answers the routing rule from the graph's LIVE order, and a move swaps it") {
	// **THE RULE, THROUGH THE REAL HOST.** `RackEngine::slots()` hands `mayRoute` the graph's rank
	// per lane, so the same rule Feathers runs runs here — and it is the graph's CURRENT order, not a
	// lane number: reorder, and the answers swap at unchanged lanes. That second half is what makes
	// this a case about the rule rather than about two devices happening to sit in lane order.
	//
	// **TWO SKLITTERS, NOT TWO DAHDSRs (2026-10-03).** The DAHDSRs were ordered only by their note road, and a
	// note road that only LISTENS orders nothing now (`NoteOps::listensOnly`) — so they route either way, as two
	// LFOs do (IZ). SKLITTER carries audio, and an audio chain is what still orders two devices in a lane.
	rack::RackEngine rack(kAxes, hostCatalogue());
	rack.prepare(48000.0, 4096);
	const size_t first = rack.addDevice(fx::sklitter::kTraits.tag);
	const size_t second = rack.addDevice(fx::sklitter::kTraits.tag);
	REQUIRE(first != rack::RackEngine::kNoSlot);
	REQUIRE(second != rack::RackEngine::kNoSlot);
	const size_t firstLane = first;
	const size_t secondLane = second;
	REQUIRE(firstLane != rack::RackEngine::kNoLane);
	REQUIRE(secondLane != rack::RackEngine::kNoLane);
	// PERF X: a control SKLITTER's own outputs are made of, so a route from a SKLITTER into ITSELF there is refused
	// (`kFeedsPerformOutputs`) — the self arms below need a control that is not self-routable.
	const size_t attack = static_cast<size_t>(fx::sklitter::Param::PerfX);
	REQUIRE(fx::sklitter::kDescriptor.params[attack].modulatable);
	REQUIRE(attack < fx::sklitter::kDescriptor.params.size());
	const size_t ordinal = fx::sklitter::kDescriptor.nthModulatableOfControl(attack);

	const backplane::Source fromFirst = test::outputOf(rack, firstLane, 0);
	const backplane::Source fromSecond = test::outputOf(rack, secondLane, 0);
	const backplane::Destination intoFirst = test::ordinalOf(rack, firstLane, ordinal);
	const backplane::Destination intoSecond = test::ordinalOf(rack, secondLane, ordinal);

	// AS ADDED: first before second.
	CHECK(rack.mayRoute(fromFirst, intoSecond));        // forward
	CHECK_FALSE(rack.mayRoute(fromSecond, intoFirst));  // backwards
	CHECK_FALSE(rack.mayRoute(fromFirst, intoFirst));   // itself
	CHECK_FALSE(rack.mayRoute(fromSecond, intoSecond)); // itself, the other one

	// MOVED: second now before first. Lanes unchanged, ranks swapped, answers swapped.
	REQUIRE(rack.moveDevice(1, 0));
	REQUIRE(rack.deviceAtPosition(0) == secondLane); // the lane travelled with the device
	CHECK_FALSE(rack.mayRoute(fromFirst, intoSecond));
	CHECK(rack.mayRoute(fromSecond, intoFirst));
	CHECK_FALSE(rack.mayRoute(fromFirst, intoFirst));

	// AND THE BRIDGE'S MIRROR SAYS THE SAME, because the editor's glow reads the bridge, not the
	// engine. A rank published wrong here is a knob glowing for a source it cannot take.
	// BY ENDPOINT on the bridge (kill-lanes step 2): the same two devices, under the ids the engine keeps
	// them by, which the record took (step 3a).
	rack::RackBridge bridge(rack);
	bridge.publish(rack);
	const DeviceInstanceId firstId = rack.slotAtLane(firstLane).id;
	const DeviceInstanceId secondId = rack.slotAtLane(secondLane).id;
	REQUIRE(firstId != kNoInstance);
	REQUIRE(secondId != kNoInstance);
	CHECK(bridge.mayRoute(backplane::Source::output(secondId, 0), backplane::Destination::control(firstId, attack)));
	CHECK_FALSE(bridge.mayRoute(backplane::Source::output(firstId, 0), backplane::Destination::control(secondId, attack)));
}

TEST_CASE("the engine writes routes through the graph's door: backwards and self are refused, forward and macro land, and a reorder deletes") {
	// **RACK ROUTING RULE, COMMIT 3.** `RackEngine::setRouteAmount` used to write its own table with no
	// gate while `Graph::route` — the pass-through enforcer — was called by nothing but its own tests
	// (plan §7.3). The engine's table is gone and the graph's is the table, so a device-to-device
	// route goes in through `route()`, which refuses a backwards or self route by construction.
	// `resolveInto` therefore needs no mask: an illegal route cannot exist in what it reads.
	//
	// EVERY CLAIM IS THE CELL'S VALUE through `routeAmount`, which is what the bridge and the resolver
	// both read — not "the sound moved". The FORWARD and MACRO arms are the controls: a door that
	// refused everything passes the refusals alone.
	rack::RackEngine rack(kAxes, hostCatalogue());
	rack.prepare(48000.0, 4096);
	const size_t first = rack.addDevice(fx::sklitter::kTraits.tag);
	const size_t second = rack.addDevice(fx::sklitter::kTraits.tag);
	REQUIRE(first != rack::RackEngine::kNoSlot);
	REQUIRE(second != rack::RackEngine::kNoSlot);
	const size_t firstLane = first;
	const size_t secondLane = second;
	const backplane::Source fromFirst = test::outputOf(rack, firstLane, 0);
	const backplane::Source fromSecond = test::outputOf(rack, secondLane, 0);
	// PERF X, for the self arm — see the case above.
	const size_t perfX = fx::sklitter::kDescriptor.nthModulatableOfControl(static_cast<size_t>(fx::sklitter::Param::PerfX));
	const backplane::Destination intoFirst = test::ordinalOf(rack, firstLane, perfX);
	const backplane::Destination intoSecond = test::ordinalOf(rack, secondLane, perfX);

	SUBCASE("FORWARD lands, BACKWARDS and SELF do not — the door is the rule") {
		rack.setRouteAmount(fromFirst, intoSecond, 0.5f);
		rack.setRouteAmount(fromSecond, intoFirst, 0.25f); // backwards
		rack.setRouteAmount(fromFirst, intoFirst, 0.75f);  // itself
		CHECK(rack.routeAmount(fromFirst, intoSecond) == doctest::Approx(0.5f));
		CHECK(rack.routeAmount(fromSecond, intoFirst) == doctest::Approx(0.0f));
		CHECK(rack.routeAmount(fromFirst, intoFirst) == doctest::Approx(0.0f));
	}

	SUBCASE("a MACRO reaches a device — the core path that has no node to order") {
		rack.setRouteAmount(test::macro(0), intoFirst, 0.4f);
		CHECK(rack.routeAmount(test::macro(0), intoFirst) == doctest::Approx(0.4f));
	}

	SUBCASE("an amount UPDATE on an existing forward route lands, and zeroing it lands") {
		// The drag shape: the same cell written many times. `route()` recomputes each time; the value
		// must follow, and clearing to zero must be a write rather than a refusal.
		rack.setRouteAmount(fromFirst, intoSecond, 0.5f);
		rack.setRouteAmount(fromFirst, intoSecond, 0.9f);
		CHECK(rack.routeAmount(fromFirst, intoSecond) == doctest::Approx(0.9f));
		rack.setRouteAmount(fromFirst, intoSecond, 0.0f);
		CHECK(rack.routeAmount(fromFirst, intoSecond) == doctest::Approx(0.0f));
	}

	SUBCASE("a reorder through the ENGINE deletes the route it breaks, counts it, and keeps the macro's") {
		// `Graph::move`'s semantics (commit 2) reaching a product path for the first time — the
		// graph's table was empty until this commit, so this is the first reorder that has anything
		// to delete.
		rack.setRouteAmount(fromFirst, intoSecond, 0.5f);
		rack.setRouteAmount(test::macro(0), intoSecond, 0.3f);
		const std::optional<size_t> cleared = rack.moveDevice(1, 0); // second above first
		REQUIRE(cleared.has_value());
		CHECK(*cleared == 1);
		CHECK(rack.routeAmount(fromFirst, intoSecond) == doctest::Approx(0.0f));
		CHECK(rack.routeAmount(test::macro(0), intoSecond) == doctest::Approx(0.3f));
		// AND THE NOW-LEGAL DIRECTION CAN BE WRITTEN — the rule follows the live order.
		rack.setRouteAmount(fromSecond, intoFirst, 0.6f);
		CHECK(rack.routeAmount(fromSecond, intoFirst) == doctest::Approx(0.6f));
	}
}

//------------------------------------------------------------------------------------------------//
// THE BRIDGE'S id -> MIRROR HOP (kill-lanes step 3a)                                               //
//------------------------------------------------------------------------------------------------//

TEST_CASE("the bridge reads each device's READINGS and PLAYHEAD by its id, before and after a reorder") {
	// **THE DG SHAPE, ONE LAYER UP.** OWLTT stopped updating its analyser bars after being moved
	// — a count asked with one number and a value with another, equal until the
	// first move. The engine keeps each device in a slot IT chooses now (step 3a) and the bridge's
	// mirror is per slot, stamped with the device it is about; every reader hops through
	// `mirrorSlotOf(id)`. Only `modulatedFieldAt` was covered: a wrong hop in `readingCountAt`,
	// `readingAt` or `playheadAt` shows one device's meters or its position in its shape for another,
	// and nothing was watching.
	//
	// THE VALUES ARE PUBLISHED BY THIS CASE, per ENGINE SLOT, so they are distinct BY CONSTRUCTION and
	// the assertion is about the hop rather than about what a device happens to meter.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);
	const DeviceInstanceId first = bridge.addDevice(dahdsr::kTraits.tag);
	const DeviceInstanceId second = bridge.addDevice(mseg::kTraits.tag);
	REQUIRE(first != kNoInstance);
	REQUIRE(second != kNoInstance);
	bridge.drain();
	const size_t firstSlot = engine.slotOf(first);
	const size_t secondSlot = engine.slotOf(second);
	REQUIRE(firstSlot != rack::RackEngine::kNoSlot);
	REQUIRE(secondSlot != rack::RackEngine::kNoSlot);
	REQUIRE(firstSlot != secondSlot);

	// DISTINCT READINGS, AND DIFFERENT COUNTS — so a hop that lands on the neighbour is wrong in the
	// count as well as in the values, and neither can be right by accident.
	const std::array<float, 3> firstReadings { 0.125f, 0.25f, 0.375f };
	const std::array<float, 2> secondReadings { -0.5f, 0.75f };
	engine.publishReadings(firstSlot, firstReadings.data(), firstReadings.size());
	engine.publishReadings(secondSlot, secondReadings.data(), secondReadings.size());
	bridge.publish(engine);

	const auto readsBack = [&](DeviceInstanceId id, const auto& expected) {
		CAPTURE(id);
		CHECK(bridge.readingCountAt(id) == expected.size());
		for (size_t i = 0; i < expected.size(); ++i) {
			CAPTURE(i);
			CHECK(bridge.readingAt(id, i) == doctest::Approx(expected[i]));
		}
	};
	readsBack(first, firstReadings);
	readsBack(second, secondReadings);
	// NOT VACUOUS: the two devices really do answer differently, so an id that hopped to the wrong slot
	// would have been caught above rather than agreeing by coincidence.
	REQUIRE(bridge.readingAt(first, 0) != doctest::Approx(bridge.readingAt(second, 0)));
	// AND A DEVICE THE BRIDGE DOES NOT HOLD READS NOTHING, rather than whatever sits at some slot.
	CHECK(bridge.readingCountAt(second + 100) == 0);
	CHECK(bridge.readingAt(second + 100, 0) == 0.0f);

	// **THE PLAYHEAD, WHICH IS THE DEVICE'S OWN AND CANNOT BE PLANTED** — so it is asserted against what
	// the ENGINE says for that device's slot, which is the claim: the bridge answers for the device
	// asked about, not for whoever holds the slot the record once named.
	engine.setGate(true);
	engine.tickModulators(64, backplane::unknownBlock());
	bridge.publish(engine);
	const auto playheadAgrees = [&](DeviceInstanceId id, size_t slot) {
		CAPTURE(id);
		uint32_t engineOrdinal = mod::Playhead::kIdle;
		float enginePhase = -1.0f;
		const bool engineHas = engine.playheadAt(slot, engineOrdinal, enginePhase);
		uint32_t bridgeOrdinal = mod::Playhead::kIdle;
		float bridgePhase = -1.0f;
		const bool bridgeHas = bridge.playheadAt(id, bridgeOrdinal, bridgePhase);
		CHECK(bridgeHas == engineHas);
		if (engineHas) {
			CHECK(bridgeOrdinal == engineOrdinal);
			CHECK(bridgePhase == doctest::Approx(enginePhase));
		}
		// AND THE LEVEL BESIDE IT (`mod::Playhead::level`), mirrored for the device that publishes one.
		float engineLevel = -1.0f;
		const bool engineHasLevel = engine.playheadLevelAt(slot, engineLevel);
		float bridgeLevel = -1.0f;
		CHECK(bridge.playheadLevelAt(id, bridgeLevel) == engineHasLevel);
		if (engineHasLevel) {
			CHECK(bridgeLevel == engineLevel);
		}
		return engineHas;
	};
	REQUIRE(playheadAgrees(first, firstSlot));  // a live modulator publishes one, or the case proves nothing
	REQUIRE(playheadAgrees(second, secondSlot));
	{
		// THE DAHDSR PUBLISHES ITS LEVEL AND THE MSEG DOES NOT — so the agreement above was about something.
		float level = -1.0f;
		CHECK(bridge.playheadLevelAt(first, level));
		CHECK(level > 0.0f);
		CHECK_FALSE(bridge.playheadLevelAt(second, level));
	}

	SUBCASE("and a REORDER moves neither: a device's readings follow the device") {
		// The devices swap places in the chain; the engine moves nothing between slots (a reorder is a
		// permutation of the order), so the mirror's stamps must still send each id to its own words.
		REQUIRE(bridge.moveDevice(0, 1));
		bridge.drain();
		bridge.publish(engine);
		CHECK(bridge.deviceAtPosition(0) == second); // the reorder really happened
		CHECK(bridge.deviceAtPosition(1) == first);
		readsBack(first, firstReadings);
		readsBack(second, secondReadings);
		playheadAgrees(first, engine.slotOf(first));
		playheadAgrees(second, engine.slotOf(second));
	}

	SUBCASE("and where the record's lane and the engine's slot DIVERGE, the words still follow the device") {
		// **THE SUBCASE THAT MAKES THIS CASE BITE, AND IT IS MEASURED RATHER THAN ASSUMED.** With the hop
		// written the old way — the record's lane — every assertion above still passes, because in a
		// standalone rack the record's lowest free lane and the engine's lowest free slot are the same
		// number in every ordinary sequence. They diverge exactly where backlog CF put them: the parking
		// is full, so the drain DECLINES a removal and the engine keeps that device in its slot, while the
		// record has already freed the lane and given it to the newcomer. The newcomer's words are then at
		// a slot its record lane does not name, and a lane-indexed hop reads the DEPARTING device's meters.
		bridge.removeDevice(first); // the record frees its lane at once
		const DeviceInstanceId third = bridge.addDevice(dahdsr::kTraits.tag);
		REQUIRE(third != kNoInstance);
		// THE PARKING IS FILLED **AFTER** THE ADD, because provisioning reaps first by design — filling it
		// before would leave it empty by the time the drain looked, and the removal would simply succeed.
		const size_t capacity = engine.retiredCapacityForTest();
		while (engine.retiredCountForTest() < capacity) {
			const size_t slot = engine.addDevice(filter::kTraits.tag);
			REQUIRE(slot != rack::RackEngine::kNoSlot);
			REQUIRE(engine.removeDeviceAtLane(slot));
		}
		bridge.drain(); // the removal is declined (parking full); the newcomer is adopted elsewhere
		const size_t thirdSlot = engine.slotOf(third);
		REQUIRE(thirdSlot != rack::RackEngine::kNoSlot);
		// THE PREMISE, ASSERTED: the two numbers really are different here, or this subcase proves nothing.
		// The record's own parking is its graph's business now (kill-lanes step 3b), so the premise is
		// asked where it lives — the point being that the engine's slot is NOT it.
		REQUIRE(bridge.recordForTest().graph().laneOf(third) != thirdSlot);
		REQUIRE(engine.slotOf(first) != rack::RackEngine::kNoSlot); // the declined device is still running

		const std::array<float, 4> thirdReadings { 0.0625f, 0.1875f, 0.3125f, 0.4375f };
		engine.publishReadings(thirdSlot, thirdReadings.data(), thirdReadings.size());
		bridge.publish(engine);
		readsBack(third, thirdReadings);
		// AND NOT THE DEPARTING DEVICE'S, which is what a lane-indexed hop would have answered.
		REQUIRE(bridge.readingCountAt(third) != firstReadings.size());
	}

	SUBCASE("and a device that has left answers nothing, rather than the device that took its slot") {
		// THE ABA GUARD ON THE MIRROR: the departed device's id must not read the words of whoever the
		// engine later puts in that slot.
		bridge.removeDevice(first);
		bridge.drain();
		bridge.publish(engine);
		CHECK(bridge.readingCountAt(first) == 0);
		uint32_t ordinal = mod::Playhead::kIdle;
		float phase = -1.0f;
		CHECK_FALSE(bridge.playheadAt(first, ordinal, phase));
		// AND THE SURVIVOR IS UNDISTURBED — not vacuous: the reader still works for a device that is there.
		readsBack(second, secondReadings);
	}
}

TEST_CASE("the bridge answers a device's DRIVE PEAK by its id, and a device that meters nothing declines") {
	// **THE DRIVE METER, ON THE ROAD NOTHING WATCHED.** A level meter behind the transfer-curve
	// display shows how hard the table is driven, and in the rack it did not show. The ENGINE's
	// `drivePeakAt` is covered directly; the BRIDGE's — the
	// id→mirror hop the editor actually asks — was not, and it declining silently is indistinguishable
	// from a shaper nobody is driving.
	//
	// **THE BOOL IS THE POINT.** "Unmetered" and "driven by silence" must stay different answers, which is
	// why this returns a bool and leaves the out-parameter untouched when declining.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);
	const DeviceInstanceId shaper = bridge.addDevice(fx::shaper::kTraits.tag);
	const DeviceInstanceId quiet = bridge.addDevice(filter::kTraits.tag); // declares no drive meter
	REQUIRE(shaper != kNoInstance);
	REQUIRE(quiet != kNoInstance);
	bridge.drain();
	bridge.publish(engine);

	// DRIVEN WITH SOMETHING AUDIBLE, or the peak is zero and "metered" is indistinguishable from "not".
	std::vector<float> left = tone(256);
	std::vector<float> right = tone(256);
	engine.beginBlock(256, backplane::unknownBlock());
	engine.processBus(left.data(), right.data(), 256);
	engine.endBlock();

	float peak = -1.0f;
	REQUIRE(bridge.drivePeakAt(shaper, peak));
	CHECK(peak > 0.0f); // the value, not merely the fact that it answered
	// AND IT IS THE ENGINE'S OWN NUMBER FOR THAT DEVICE, at the slot the engine chose for it.
	float enginePeak = -1.0f;
	REQUIRE(engine.drivePeakAt(engine.slotOf(shaper), enginePeak));
	CHECK(peak == doctest::Approx(enginePeak));

	// A DEVICE WITH NO METER DECLINES, AND LEAVES THE OUT-PARAMETER ALONE — the caller's seed survives, so
	// a card can seed "no meter" and trust it.
	float untouched = -7.5f;
	CHECK_FALSE(bridge.drivePeakAt(quiet, untouched));
	CHECK(untouched == -7.5f);
	// AND A DEVICE THE BRIDGE DOES NOT HOLD DECLINES TOO, rather than answering for whoever holds a slot.
	float stranger = -3.0f;
	CHECK_FALSE(bridge.drivePeakAt(shaper + 500, stranger));
	CHECK(stranger == -3.0f);
}

TEST_CASE("backlog MK: the engine meters what each audio device put out — its loudest sample, held half a second, then falling 24 dB a second; a modulator and a device that has not run decline") {
	// EVERY FX CARD'S COURTESY OUTPUT METER is this number. Measured by the ENGINE after the device's own process call,
	// so no device has to publish it. The expected values are restated from the spec (0.5 s hold, 24 dB/s fall) and
	// from the buffer itself: the device is the chain's only audio device and runs in place, so what it put out is
	// what the buffer holds afterwards.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);
	const DeviceInstanceId audio = bridge.addDevice(filter::kTraits.tag);
	const DeviceInstanceId modulator = bridge.addDevice(mod::lfo::kTraits.tag);
	REQUIRE(audio != kNoInstance);
	REQUIRE(modulator != kNoInstance);
	// SWITCHED OFF, so it passes its input through untouched and an over reaches the meter as an over rather than as
	// whatever the filter's drive makes of it. Switched off is still in the chain, and still metered: it is its output.
	bridge.setField(audio, static_cast<size_t>(filter::Param::Enabled), 0.0f);
	bridge.drain();
	bridge.publish(engine);
	const size_t slot = engine.slotOf(audio);
	REQUIRE(slot != rack::RackEngine::kNoSlot);

	// NOT RUN YET: declined, the out-parameter untouched — "no meter", never "silent".
	float untouched = -7.5f;
	CHECK_FALSE(engine.outputPeakAt(slot, untouched));
	CHECK_FALSE(bridge.outputPeakAt(audio, untouched));
	CHECK(untouched == -7.5f);

	constexpr int kChunk = 256;
	const auto run = [&](std::vector<float>& l, std::vector<float>& r) {
		engine.beginBlock(kChunk, backplane::unknownBlock());
		engine.processBus(l.data(), r.data(), kChunk);
		engine.endBlock();
	};
	std::vector<float> left = tone(kChunk);
	std::vector<float> right = tone(kChunk);
	run(left, right);
	float loud = 0.0f;
	for (int i = 0; i < kChunk; ++i) {
		loud = std::max({ loud, std::fabs(left[static_cast<size_t>(i)]), std::fabs(right[static_cast<size_t>(i)]) });
	}
	REQUIRE(loud > 0.1f); // not vacuous: something audible came out
	float peak = -1.0f;
	REQUIRE(engine.outputPeakAt(slot, peak));
	CHECK(peak == loud);
	float viaBridge = -1.0f;
	REQUIRE(bridge.outputPeakAt(audio, viaBridge)); // the rack's editor reads it by id
	CHECK(viaBridge == loud);
	// A MODULATOR PUTS OUT NO AUDIO, so it has no meter.
	float none = -3.0f;
	CHECK_FALSE(engine.outputPeakAt(engine.slotOf(modulator), none));
	CHECK(none == -3.0f);

	// THE HOLD: 0.5 s is 24000 samples, so 94 silent chunks (24064) leave it standing — the hold counts down past zero
	// on the 94th — and the 95th is the first to fall.
	for (int c = 0; c < 94; ++c) {
		std::vector<float> l(kChunk, 0.0f), r(kChunk, 0.0f);
		run(l, r);
	}
	REQUIRE(engine.outputPeakAt(slot, peak));
	CHECK(peak == loud);
	const auto fall = static_cast<float>(std::pow(10.0, -24.0 * (static_cast<double>(kChunk) / 48000.0) / 20.0));
	float expected = loud;
	for (int c = 0; c < 11; ++c) {
		std::vector<float> l(kChunk, 0.0f), r(kChunk, 0.0f);
		run(l, r);
		expected *= fall;
	}
	REQUIRE(engine.outputPeakAt(slot, peak));
	CHECK(peak == doctest::Approx(expected).epsilon(1e-5));
	// 11 chunks is 58.7 ms: 1.41 dB down, and not vacuous — it moved.
	CHECK(20.0f * std::log10(loud / peak) == doctest::Approx(24.0 * 11.0 * kChunk / 48000.0).epsilon(1e-3));

	// AND A LOUDER CHUNK TAKES OVER AT ONCE, over full scale and unclamped — the over is what the card draws.
	std::vector<float> hot(kChunk, 0.0f), hotR(kChunk, 0.0f);
	for (int i = 0; i < kChunk; ++i) {
		hot[static_cast<size_t>(i)] = 4.0f * std::sin(6.2831853f * 220.0f * static_cast<float>(i) / 48000.0f);
	}
	run(hot, hotR);
	float hottest = 0.0f;
	for (int i = 0; i < kChunk; ++i) {
		hottest = std::max({ hottest, std::fabs(hot[static_cast<size_t>(i)]), std::fabs(hotR[static_cast<size_t>(i)]) });
	}
	REQUIRE(hottest > 3.0f); // the switched-off filter passed the over through
	REQUIRE(engine.outputPeakAt(slot, peak));
	CHECK(peak == hottest);
}

TEST_CASE("a route survives a device REPLACED under its own id: the new instance is what the route reaches") {
	// **THE ROUTE NAMES THE DEVICE, AND THE PLAN NAMES THE SLOT.** A replacement in place keeps the id (the
	// record's remove-then-insert does), so the route list does not change at all — while the engine
	// destroys one instance and adopts another, possibly in a different slot of its own. The compiled plan
	// is in the ENGINE's slots, so it has to be compiled again on the engine's word rather than on the
	// route list's: keyed on the route sequence alone, this route would resolve onto whatever the plan
	// still pointed at. Asserted as the VALUE the engine resolves, never as a picture.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);
	const DeviceInstanceId first = bridge.addDevice(filter::kTraits.tag);
	const DeviceInstanceId target = bridge.addDevice(filter::kTraits.tag);
	REQUIRE(first != kNoInstance);
	REQUIRE(target != kNoInstance);
	bridge.drain();

	// A MACRO INTO THE TARGET'S FIRST MODULATABLE CONTROL, through the record's own gate.
	const size_t control = filter::kDescriptor.controlOfNthModulatableBesideEnable(0);
	const backplane::Source macro = backplane::Source::core(0);
	const backplane::Destination into = backplane::Destination::control(target, control);
	bridge.setRouteAmount(macro, into, 0.5f);
	bridge.setMacro(0, 1.0f);
	bridge.drain();
	// A BLOCK, so the row is RESOLVED before it is read — `resolvedFieldForTest` reads the row the last
	// block resolved. This read it before any block and passed on the zeros the row used to start with
	// (2026-09-15: a slot's resolved row starts as its BASE now, and the accident showed).
	std::vector<float> l(64, 0.0f), r(64, 0.0f);
	engine.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
	const float base = bridge.fieldAt(target, control);
	const float routed = engine.resolvedFieldForTest(engine.slotOf(target), control);
	REQUIRE(routed != doctest::Approx(base)); // the route is heard at all, or the case proves nothing

	// **THE REPLACEMENT**: same id, another device — and the route, which names the id, is untouched.
	rack::RackPatch patch = bridge.patch();
	REQUIRE(patch.device.size() == 2);
	patch.device[1].tag = rack::patchTagOf(dahdsr::kTraits.tag);
	// **AND THE ROUTE NAMES A CONTROL THE NEW DEVICE HAS** (backlog JF): a route names its control by KEY
	// now, and a filter's key names nothing on an envelope — it would be refused, which is the point of
	// keys. The envelope's control at the same index is what this case routes to, so the claim below is
	// still about the replacement and not about a refusal.
	REQUIRE(patch.route.size() == 1);
	patch.route[0].destination.key = dahdsr::kDescriptor.params[control].key;
	REQUIRE(bridge.loadPatch(patch).routes.count == 0);
	bridge.drain();
	bridge.reapRetired();
	bridge.drain();

	// WHATEVER THE LOADER MADE OF IT, THE PLAN AND THE ENGINE AGREE — the resolved value is the route's,
	// read at the slot the engine now keeps the routed device in.
	const DeviceInstanceId nowAt1 = bridge.deviceAtPosition(1);
	REQUIRE(nowAt1 != kNoInstance);
	const size_t slot = engine.slotOf(nowAt1);
	REQUIRE(slot != rack::RackEngine::kNoSlot);
	engine.processStereo(l.data(), r.data(), 64, backplane::unknownBlock()); // resolved, then read
	const float amount = bridge.routeAmount(macro, backplane::Destination::control(nowAt1, control));
	// **THE CLAIM, PINNED WHERE IT LIVES**: the route is wired into the NEW instance's node — one input
	// on that control — and the resolved value is exactly what that input makes of the base, THROUGH
	// THE DECLARED CLAMP. This read "resolved != base" and passed on a row no block had resolved
	// (zeros, before 2026-09-15); resolved for real, the filter's row value reinterpreted on the
	// envelope's control at the same index sits at that control's ceiling, so a positive offset moves
	// it nowhere — a fact about this replacement, not about the route.
	const device::ParamDesc& p = dahdsr::kDescriptor.params[control];
	const float newBase = bridge.fieldAt(nowAt1, control);
	if (amount != 0.0f) {
		// THE ROUTE SURVIVED: wired to the NEW instance's slot — not to wherever the old plan pointed —
		// and resolved onto it.
		CHECK(engine.inputCountForTest(slot, control) == 1);
		const double expected = device::withRouteOffset(
		    p, static_cast<double>(newBase),
		    static_cast<double>(amount) * static_cast<double>(engine.macro(0)) * device::routeSpanOf(p));
		CHECK(engine.resolvedFieldForTest(slot, control) == doctest::Approx(expected));
	}
	else {
		// THE ROUTE WENT WITH THE DEVICE IT NAMED — then nothing may be resolved onto the newcomer either,
		// which is the other half of the same claim: a stale plan would still be moving this control.
		CHECK(engine.inputCountForTest(slot, control) == 0);
		CHECK(engine.resolvedFieldForTest(slot, control) == doctest::Approx(newBase));
	}
}

//--------------------------------------------------------------------------------------------------//
// WHAT THE BY-ID LOOKUPS COST — measured, because nobody had (kill-lanes steps 2 and 3a)             //
//--------------------------------------------------------------------------------------------------//
//
// **NO BOUND IS ASSERTED, ON THIS HOST AND ON PURPOSE** — load here swings by a factor of eight between
// quiet and busy, so a threshold would be a flake generator. These cases assert CORRECTNESS and REPORT
// the time, which is what the bus-fill measurement in test_gui_bridge.cpp does for the same reason.
// A/B ALTERNATED WITHIN ONE RUN and the MEDIAN of several rounds taken, because a number from one
// uninterrupted stretch on a shared machine is a number about the machine.

namespace {

/// THE SHIPPED RACK'S SHAPE — sixteen slots, which is the population these lookups actually walk.
constexpr backplane::Axes kMeasureAxes {
	.coreSources = 8,
	.coreDestinations = 0,
	.deviceSlots = 16,
	.outputStride = feathers::device::kMaxDeviceOutputs,
};

/// THE MIDDLE OF SEVERAL ROUNDS — one round on a shared host is a measurement of the host.
double medianOf(std::vector<double> v) {
	std::sort(v.begin(), v.end());
	return v[v.size() / 2];
}

} // namespace

TEST_CASE("a per-voice source reaching a per-voice device resolves that device's control PER VOICE, and the collapsed row is untouched (backlog DU)") {
	// THE ENGINE'S HALF OF DU'S LAST ROW, asked without a synth: two voices, a note-expression device
	// whose Pressure output is declared per voice, its per-voice cells written by hand (`writeOutputForVoice`
	// is the road the synth takes), routed into a Poly filter's cutoff. Voice 0 under full pressure opens by
	// amount × span; voice 1 under none sits at the base; and the COLLAPSED row — what the editor draws and
	// what a bus device would read — follows the device's own block-rate cell (the one its tick publishes),
	// which is neither voice's.
	namespace nx = mod::noteexpr;
	rack::RackEngine rack(kAxes, hostCatalogue());
	rack.setVoices(2);
	rack.prepare(48000.0, 4096);
	const size_t nxSlot = rack.addDevice(nx::kTraits.tag);
	const size_t fSlot = rack.addDevice(filter::kTraits.tag);
	REQUIRE(nxSlot != rack::RackEngine::kNoSlot);
	REQUIRE(fSlot != rack::RackEngine::kNoSlot);
	const size_t cutoff = controlNamed(filter::kDescriptor, "Cutoff");
	const size_t poly = static_cast<size_t>(filter::Param::Poly);
	rack.setField(fSlot, poly, 1.0f);
	const float base = rack.fieldAt(fSlot, cutoff);
	// OUTPUT 0 (Vol), not Pressure: this suite's axes give a device four outputs and the seven declared all
	// vary per voice alike; the synth suite asks the same of Pressure through the real axes.
	constexpr size_t kPressure = 0;
	REQUIRE(nx::kDescriptor.outputs[kPressure].perVoice);
	rack.setRouteAmount(test::outputOf(rack, nxSlot, kPressure), test::controlOf(rack, fSlot, cutoff), 0.5f);
	REQUIRE(rack.routeAmount(test::outputOf(rack, nxSlot, kPressure), test::controlOf(rack, fSlot, cutoff)) == 0.5f);
	rack.writeOutputForVoice(test::idAt(rack, nxSlot), kPressure, 0, 1.0f);
	rack.writeOutputForVoice(test::idAt(rack, nxSlot), kPressure, 1, 0.0f);
	const int n = 64;
	std::vector<float> l = tone(n), r = tone(n);
	rack.processStereo(l.data(), r.data(), n, backplane::unknownBlock());
	REQUIRE(rack.polyForTest(fSlot));
	// THE CELLS AS WRITTEN — voice 0 pressed, voice 1 not — and the collapsed cell is the device's own
	// (Vol's default), which is what a bus device or the editor would read.
	const float* pv = rack.outputCellForVoiceOf(test::idAt(rack, nxSlot), kPressure);
	REQUIRE(pv != nullptr);
	REQUIRE(pv[0] == doctest::Approx(1.0f));
	REQUIRE(pv[1] == doctest::Approx(0.0f));
	const float collapsedCell = *rack.outputCellOf(test::idAt(rack, nxSlot), kPressure);
	const device::ParamDesc& p = filter::kDescriptor.params[cutoff];
	const double expected = std::min(static_cast<double>(base) + 0.5 * device::routeSpanOf(p), p.offsetsMayLeaveRange ? 1e9 : p.max);
	CHECK(rack.resolvedFieldForVoiceForTest(fSlot, cutoff, 0) == doctest::Approx(expected));
	CHECK(rack.resolvedFieldForVoiceForTest(fSlot, cutoff, 1) == doctest::Approx(base));
	const double expectedCollapsed = std::min(static_cast<double>(base) + 0.5 * collapsedCell * device::routeSpanOf(p), p.offsetsMayLeaveRange ? 1e9 : p.max);
	CHECK(rack.resolvedFieldForTest(fSlot, cutoff) == doctest::Approx(expectedCollapsed));
	// NOT VACUOUS: the two voices differ, and the collapsed row is the device's own cell, not voice 0's.
	CHECK(rack.resolvedFieldForVoiceForTest(fSlot, cutoff, 0) != doctest::Approx(base));
	// AND A SOURCE WITH NO PER-VOICE RUN — a macro — resolves the same for every voice as for the block.
	rack.setRouteAmount(test::outputOf(rack, nxSlot, kPressure), test::controlOf(rack, fSlot, cutoff), 0.0f);
	rack.setRouteAmount(test::macro(0), test::controlOf(rack, fSlot, cutoff), 0.25f);
	REQUIRE(rack.routeAmount(test::macro(0), test::controlOf(rack, fSlot, cutoff)) == 0.25f);
	rack.setMacro(0, 1.0f);
	rack.processStereo(l.data(), r.data(), n, backplane::unknownBlock());
	const float collapsed = rack.resolvedFieldForTest(fSlot, cutoff);
	CHECK(collapsed != doctest::Approx(base));
	CHECK(rack.resolvedFieldForVoiceForTest(fSlot, cutoff, 0) == doctest::Approx(collapsed));
	CHECK(rack.resolvedFieldForVoiceForTest(fSlot, cutoff, 1) == doctest::Approx(collapsed));
}

TEST_CASE("a metered device's readings reach the engine by the one road, and a bridge mirrors them to its surfaces") {
	// THE SYMPTOM: OWLTT's meters did not move in the rack. Meters reached a surface only
	// through per-device callbacks `feathers_clap.cpp` wired for its own lane; the engine's
	// `publishReadings` had NO CALLER, so a device in the standalone rack or inside a chain metered nothing
	// and `RackBridge::readingAt` mirrored zeros. `AudioOps::setReadingsSink` is the one road now: the
	// engine installs it on every device it adopts, the device publishes into its slot's readings, the
	// bridge mirrors them. ASSERTED AS VALUES: the count each surface expects, and a level that moved
	// under signal against one that did not under silence.
	namespace owltt = fx::owltt;
	const auto metered = [](const char* tag, bool loud) {
		rack::RackEngine rack(kAxes, hostCatalogue());
		rack.prepare(48000.0, 4096);
		const size_t slot = rack.addDevice(tag);
		REQUIRE(slot != rack::RackEngine::kNoSlot);
		const device::Descriptor* d = deviceForTag(tag);
		REQUIRE(d != nullptr);
		if (d->traits.enableControl >= 0) {
			rack.setField(slot, static_cast<size_t>(d->traits.enableControl), 1.0f);
		}
		const int n = 256;
		for (int block = 0; block < 8; ++block) {
			std::vector<float> l(static_cast<size_t>(n), 0.0f), r(static_cast<size_t>(n), 0.0f);
			if (loud) {
				l = tone(n);
				r = tone(n);
				for (float& x : l) { x *= 0.9f; }
				for (float& x : r) { x *= 0.9f; }
			}
			rack.processStereo(l.data(), r.data(), n, backplane::unknownBlock());
		}
		std::vector<float> readings;
		for (size_t i = 0; i < rack.readingCountAt(slot); ++i) {
			readings.push_back(rack.readingAt(slot, i));
		}
		return std::pair { slot, readings };
	};
	SUBCASE("OWLTT: ten readings, and the input level moves under signal") {
		const auto [slot, loud] = metered(owltt::kTraits.tag, true);
		const auto [slotQuiet, quiet] = metered(owltt::kTraits.tag, false);
		REQUIRE(loud.size() == 18); // ten meters + three band lights + combined GR (backlog EL) + the second channel's GR per band (LQ) + Key Track's note
		REQUIRE(quiet.size() == 18); // backlog EL, LQ, Key Track's note
		bool moved = false;
		for (size_t band = 0; band < 3; ++band) {
			moved = moved || loud[3 + band] > quiet[3 + band];
		}
		CHECK(moved);
	}
	SUBCASE("bus utility: its declared count") {
		const auto [slot, loud] = metered(fx::busutil::kTraits.tag, true);
		CHECK(loud.size() == static_cast<size_t>(fx::busutil::Reading::kCount));
	}
	SUBCASE("compressor: its declared count") {
		const auto [slot, loud] = metered(fx::comp::kTraits.tag, true);
		CHECK(loud.size() == static_cast<size_t>(fx::comp::Reading::kCount));
	}
	SUBCASE("clipper: its declared count") {
		const auto [slot, loud] = metered(fx::clip::kTraits.tag, true);
		CHECK(loud.size() == static_cast<size_t>(fx::clip::Reading::kCount));
	}
	SUBCASE("automod: phase and envelope") {
		const auto [slot, loud] = metered(fx::automod::kTraits.tag, true);
		CHECK(loud.size() == 2);
	}
	SUBCASE("and the bridge mirrors what the engine holds, by device") {
		rack::RackEngine rack(kAxes, hostCatalogue());
		rack.prepare(48000.0, 4096);
		const size_t slot = rack.addDevice(owltt::kTraits.tag);
		REQUIRE(slot != rack::RackEngine::kNoSlot);
		rack.setField(slot, static_cast<size_t>(owltt::kTraits.enableControl), 1.0f);
		rack::RackBridge bridge { rack };
		bridge.prepare(48000.0, 4096);
		const DeviceInstanceId id = test::idAt(rack, slot);
		REQUIRE(id != kNoInstance);
		std::vector<float> l = tone(256), r = tone(256);
		for (int block = 0; block < 8; ++block) {
			bridge.drain();
			rack.processStereo(l.data(), r.data(), 256, backplane::unknownBlock());
			bridge.publish(rack);
		}
		// SEVENTEEN since backlog LQ (`Owltt::kReadingCount`): ten meters, three band lights, combined GR, second-channel GR.
		REQUIRE(bridge.readingCountAt(id) == 18); // + Key Track's note (17)
		for (size_t i = 0; i < 17; ++i) {
			CAPTURE(i);
			CHECK(bridge.readingAt(id, i) == doctest::Approx(rack.readingAt(slot, i)));
		}
		CHECK(bridge.readingAt(id, 3) > 0.0f); // input level, band 0, under a 0 dBFS-ish tone
	}
}

TEST_CASE("MEASURED: what a by-id mirror read costs a surface, per call and per frame") {
	// **THE ROAD**: every per-device mirror read a card makes — `modulatedFieldAt` (per control, per
	// paint), `readingAt`, `readingCountAt`, `playheadAt`, `drivePeakAt` — goes through `mirrorSlotOf`,
	// which is THREE walks: the record's `laneOf` (linear over the node pool), the catalogue's tag
	// lookup behind `descriptorOf`, and the mirror's own id scan. Before step 3a a read was an array
	// index. This says what the three walks cost against the array index they replaced.
	rack::RackEngine engine { kMeasureAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);
	std::vector<feathers::DeviceInstanceId> ids;
	for (size_t i = 0; i < kMeasureAxes.deviceSlots; ++i) {
		const feathers::DeviceInstanceId id = bridge.addDevice(filter::kTraits.tag);
		REQUIRE(id != feathers::kNoInstance);
		ids.push_back(id);
	}
	bridge.drain();
	bridge.publish(engine);
	REQUIRE(engine.chainLength() == kMeasureAxes.deviceSlots);

	const feathers::DeviceInstanceId last = ids.back();  // the far end of every scan: the worst case
	const feathers::DeviceInstanceId first = ids.front(); // the near end: the best case
	const size_t lastSlot = engine.slotOf(last);
	REQUIRE(lastSlot != rack::RackEngine::kNoSlot);
	constexpr size_t kControl = 0;

	// THE CORRECTNESS HALF, so this is a case and not a stopwatch: the by-id road answers what the
	// engine holds at the slot it chose. Without this the timings could be of a road answering nothing.
	CHECK(bridge.modulatedFieldAt(last, kControl)
	      == doctest::Approx(engine.modulatedFieldAt(lastSlot, kControl)));

	static volatile float sink = 0.0f;
	constexpr int kReads = 20000;
	constexpr int kRounds = 7;
	const auto timeIt = [&](auto&& body) {
		const auto began = std::chrono::steady_clock::now();
		body();
		return static_cast<double>(
		           std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - began)
		               .count())
		       / kReads;
	};
	// WARM, so no round reports a cold cache.
	for (int i = 0; i < 2000; ++i) {
		sink = sink + bridge.modulatedFieldAt(last, kControl);
	}

	std::vector<double> byIdLast, byIdFirst, direct;
	for (int round = 0; round < kRounds; ++round) {
		// **ALTERNATED**, so a load spike lands on both sides of the comparison rather than on one.
		byIdLast.push_back(timeIt([&] {
			for (int i = 0; i < kReads; ++i) {
				sink = sink + bridge.modulatedFieldAt(last, kControl);
			}
		}));
		direct.push_back(timeIt([&] {
			for (int i = 0; i < kReads; ++i) {
				sink = sink + engine.modulatedFieldAt(lastSlot, kControl);
			}
		}));
		byIdFirst.push_back(timeIt([&] {
			for (int i = 0; i < kReads; ++i) {
				sink = sink + bridge.modulatedFieldAt(first, kControl);
			}
		}));
	}
	const double byId = medianOf(byIdLast);
	const double best = medianOf(byIdFirst);
	const double raw = medianOf(direct);
	// A CARD DRAWS ITS DEVICE'S CONTROLS: the widest device this build has, asked of the registry rather
	// than picked, and every card in a full rack drawn once a frame.
	size_t widest = 0;
	for (const feathers::device::Descriptor& d : feathers::kDevices) {
		widest = std::max(widest, d.params.size());
	}
	MESSAGE("by-id mirror read (16-slot rack, last device): " << byId << " ns; first device: " << best
	        << " ns; the engine's own array read: " << raw << " ns");
	MESSAGE("  a card of " << widest << " controls therefore pays " << byId * static_cast<double>(widest)
	        << " ns per paint, and a full rack of " << kMeasureAxes.deviceSlots << " cards "
	        << byId * static_cast<double>(widest * kMeasureAxes.deviceSlots)
	        << " ns per frame, against 16700000 ns for a 60 Hz frame");
	CHECK(byId > 0.0);
}

TEST_CASE("MEASURED: what a drain costs — idle, an arrival, a reorder") {
	// THE DRAIN IS WHERE THE CONVERGER'S ID LOOKUPS RUN (`rowOf` per slot, `slotOf` in the order pass,
	// `mustForget` per handed-back post). It runs at a drain point, not per block — so the question is
	// whether a drain is still cheap beside the block it sits at the top of.
	rack::RackEngine engine { kMeasureAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);
	constexpr size_t kDevices = 8; // a realistic chain, not the cap
	for (size_t i = 0; i < kDevices; ++i) {
		REQUIRE(bridge.addDevice(filter::kTraits.tag) != feathers::kNoInstance);
	}
	bridge.drain();
	REQUIRE(engine.chainLength() == kDevices);

	constexpr int kRounds = 7;
	constexpr int kDrains = 500;
	std::vector<double> idle, arrival, reorder;
	const auto nanos = [](auto began) {
		return static_cast<double>(
		    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - began).count());
	};
	for (int round = 0; round < kRounds; ++round) {
		auto began = std::chrono::steady_clock::now();
		for (int i = 0; i < kDrains; ++i) {
			bridge.drain(); // NOTHING CHANGED: what every ordinary block pays
		}
		idle.push_back(nanos(began) / kDrains);

		double arrivals = 0.0;
		for (int i = 0; i < kDrains / 10; ++i) {
			const feathers::DeviceInstanceId born = bridge.addDevice(filter::kTraits.tag);
			began = std::chrono::steady_clock::now();
			bridge.drain(); // THE DRAIN THAT ADOPTS IT — timed alone
			arrivals += nanos(began);
			bridge.removeDevice(born);
			bridge.drain();
			bridge.reapRetired();
		}
		arrival.push_back(arrivals / (kDrains / 10));

		double reorders = 0.0;
		for (int i = 0; i < kDrains / 10; ++i) {
			REQUIRE(bridge.moveDevice(0, kDevices - 1));
			began = std::chrono::steady_clock::now();
			bridge.drain(); // THE DRAIN THAT MOVES THEM — no device is rebuilt
			reorders += nanos(began);
		}
		reorder.push_back(reorders / (kDrains / 10));
	}
	MESSAGE("drain (8 devices): idle " << medianOf(idle) << " ns; adopting an arrival "
	        << medianOf(arrival) << " ns; after a reorder " << medianOf(reorder)
	        << " ns, against ~5333000 ns for a 256-frame block at 48 kHz");
	// THE CORRECTNESS HALF: the rack is what the hand left it, so the loops above did what they say.
	CHECK(engine.chainLength() == kDevices);
	CHECK(bridge.chainLength() == kDevices);
}

TEST_CASE("MEASURED: what one `slotOf` costs — the primitive every by-id road is built from") {
	// FEATHERS' BUS FILL CANNOT BE TIMED FROM HERE (`Synth::fillBusFor` is private and its suite is
	// another agent's file this round), so its new per-call work is DERIVED from this primitive instead:
	// the fill builds a slot→lane map per call, and each entry walks the chain asking `slotOf`.
	rack::RackEngine engine { kMeasureAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	std::vector<feathers::DeviceInstanceId> ids;
	for (size_t i = 0; i < kMeasureAxes.deviceSlots; ++i) {
		const size_t slot = engine.addDevice(filter::kTraits.tag);
		REQUIRE(slot != rack::RackEngine::kNoSlot);
		ids.push_back(engine.slotAtLane(slot).id);
	}
	static volatile size_t sink = 0;
	constexpr int kCalls = 50000;
	constexpr int kRounds = 7;
	std::vector<double> lastOf, firstOf;
	for (int round = 0; round < kRounds; ++round) {
		auto began = std::chrono::steady_clock::now();
		for (int i = 0; i < kCalls; ++i) {
			sink = sink + engine.slotOf(ids.back());
		}
		lastOf.push_back(static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		                                         std::chrono::steady_clock::now() - began)
		                                         .count())
		                 / kCalls);
		began = std::chrono::steady_clock::now();
		for (int i = 0; i < kCalls; ++i) {
			sink = sink + engine.slotOf(ids.front());
		}
		firstOf.push_back(static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		                                          std::chrono::steady_clock::now() - began)
		                                          .count())
		                  / kCalls);
	}
	MESSAGE("slotOf over 16 nodes: last " << medianOf(lastOf) << " ns, first " << medianOf(firstOf) << " ns");
	CHECK(engine.slotOf(ids.back()) != rack::RackEngine::kNoSlot);
}

// ---------------------------------------------------------------------------------------------
// RACK ROADMAP §7 — AN INNER CONTROL READS AN OUTER SOURCE (destinations volunteer to read the
// source, so only a read capability for upstream routes needs passing in). Two halves of one
// reader: the outer ENGINE hands the chain its cells at seating
// (audio side), the outer BRIDGE is asked whether a source is upstream (message thread).
// ---------------------------------------------------------------------------------------------

namespace {

struct NestedRig {
	feathers::rack::RackEngine outer { feathers::rack::kRackAxes, feathers::hostCatalogue() };
	feathers::rack::RackBridge outerBridge { outer };
	feathers::DeviceInstanceId chain = feathers::kNoInstance;
	feathers::rack::RackBridge* inner = nullptr;
	feathers::DeviceInstanceId filter = feathers::kNoInstance;
	size_t cutoff = 0;
	std::vector<float> l = std::vector<float>(64, 0.0f);
	std::vector<float> r = std::vector<float>(64, 0.0f);

	NestedRig() {
		outer.prepare(48000.0, 4096);
	}
	feathers::DeviceInstanceId addOuter(const char* tag) {
		const feathers::DeviceInstanceId id = outerBridge.addDevice(tag);
		outerBridge.drain(); // seats it in the engine — a chain is handed its reader here
		return id;
	}
	void addChainWithFilter() {
		chain = addOuter(feathers::composite::kTraits.tag);
		REQUIRE(chain != feathers::kNoInstance);
		const size_t lane = outer.slotOf(chain);
		REQUIRE(lane != feathers::rack::RackEngine::kNoSlot);
		inner = feathers::rack::bridgeOf(outer.slotAtLane(lane).storage.get());
		REQUIRE(inner != nullptr);
		REQUIRE(inner->hasOuterGate()); // the gate arrived at provision, with no panel open
		REQUIRE(inner->engineForTest().hasOuterSources()); // the engine's half arrived at seating
		filter = inner->addDevice(fx::filter::kTraits.tag);
		REQUIRE(filter != feathers::kNoInstance);
		cutoff = 0;
		while (cutoff < fx::filter::kDescriptor.params.size() && (!fx::filter::kDescriptor.params[cutoff].modulatable || static_cast<int>(cutoff) == fx::filter::kDescriptor.traits.enableControl)) {
			++cutoff;
		}
		REQUIRE(cutoff < fx::filter::kDescriptor.params.size());
	}
	backplane::Destination dest() const { return backplane::Destination::control(filter, cutoff); }
	void block() {
		outer.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
	}
};

} // namespace

TEST_CASE("§7: a control inside a chain reads the OUTER rack's macro — the inner route names the outer source, and the inner reading follows the outer macro") {
	NestedRig rig;
	rig.addChainWithFilter();
	const backplane::Destination dest = rig.dest();

	// THE SPELLING: one bit of scope, and the inner rack's own macro 0 is a different source.
	const backplane::Source outerMacro = backplane::Source::outerCore(0);
	CHECK(outerMacro.isOuter());
	CHECK(outerMacro.inner() == backplane::Source::core(0));
	CHECK_FALSE(backplane::Source::core(0).isOuter());
	CHECK_FALSE(backplane::Source::nobody().isOuter());
	// THE GATE: an outer macro is upstream of everything inside; one the outer host does not have is
	// refused; and without the outer host the inner gate refuses every outer source.
	CHECK(rig.inner->mayRoute(outerMacro, dest));
	CHECK_FALSE(rig.inner->mayRoute(backplane::Source::outerCore(feathers::composite::kMacroCount + 40), dest));
	{
		// AND A RACK THAT IS NOBODY'S CHAIN refuses every outer source: no gate, no road.
		feathers::rack::RackEngine alone { feathers::rack::kRackAxes, feathers::hostCatalogue() };
		feathers::rack::RackBridge aloneBridge { alone };
		const feathers::DeviceInstanceId f = aloneBridge.addDevice(fx::filter::kTraits.tag);
		aloneBridge.drain();
		CHECK_FALSE(aloneBridge.hasOuterGate());
		CHECK_FALSE(aloneBridge.mayRoute(outerMacro, backplane::Destination::control(f, rig.cutoff)));
	}

	// THE ROUTE, IN THE INNER TABLE, naming the outer source.
	rig.inner->setRouteAmount(outerMacro, dest, 1.0f);
	REQUIRE(rig.inner->routeAmount(outerMacro, dest) == doctest::Approx(1.0f)); // the record took it
	rig.block(); // drains the inner edit (the chain drains its bridge inside its block)
	rig.block();
	const float base = rig.inner->fieldAt(rig.filter, rig.cutoff);
	CHECK(rig.inner->modulatedFieldAt(rig.filter, rig.cutoff) == doctest::Approx(base)); // macro rests at 0

	// THE OUTER MACRO MOVES; THE INNER READING FOLLOWS, by the amount.
	rig.outerBridge.setMacro(0, 0.75f);
	rig.outerBridge.drain();
	rig.block();
	const float moved = rig.inner->modulatedFieldAt(rig.filter, rig.cutoff);
	CHECK(moved != doctest::Approx(base));
	CHECK(moved > base); // a unipolar source at 0.75, amount +1: the reading is pulled UP from the base
	// AND IN PROPORTION: half the macro is half the reach.
	rig.outerBridge.setMacro(0, 0.375f);
	rig.outerBridge.drain();
	rig.block();
	const float half = rig.inner->modulatedFieldAt(rig.filter, rig.cutoff);
	CHECK(half - base == doctest::Approx((moved - base) * 0.5f).epsilon(1e-2));
	// AND BACK.
	rig.outerBridge.setMacro(0, 0.0f);
	rig.outerBridge.drain();
	rig.block();
	CHECK(rig.inner->modulatedFieldAt(rig.filter, rig.cutoff) == doctest::Approx(base));
}

TEST_CASE("§7: an outer DEVICE output reaches inside exactly when the outer host would let it reach the chain — and its id may coincide with an inner one") {
	const device::Descriptor* modulator = nullptr;
	for (const device::Descriptor& d : kDevices) {
		// A MODULATOR, NOT MERELY A SOURCE — SKLITTER is an fx that also publishes, and is registered first.
		if (d.outputCount() > 0 && d.mod != nullptr && d.audio == nullptr) {
			modulator = &d;
			break;
		}
	}
	REQUIRE(modulator != nullptr);
	INFO(modulator->traits.tag); // which device this positional pick landed on, said out loud on a red
	NestedRig rig;
	const feathers::DeviceInstanceId before = rig.addOuter(modulator->traits.tag);
	rig.addChainWithFilter();
	const feathers::DeviceInstanceId after = rig.addOuter(modulator->traits.tag);
	REQUIRE(before != feathers::kNoInstance);
	REQUIRE(after != feathers::kNoInstance);
	const backplane::Destination dest = rig.dest();
	CHECK(rig.inner->mayRoute(backplane::Source::outerOutput(before, 0), dest));
	// THE INNER GATE ANSWERS EXACTLY WHAT THE OUTER HOST WOULD for a route into the chain itself — the
	// ordering rule is the outer host's, whichever way it rules on a modulator placed after the chain.
	CHECK(rig.inner->mayRoute(backplane::Source::outerOutput(after, 0), dest)
	      == rig.outerBridge.mayRoute(backplane::Source::output(after, 0), backplane::Destination::control(rig.chain, static_cast<size_t>(feathers::composite::Param::Macro1))));
	CHECK(rig.outerBridge.mayRoute(backplane::Source::output(before, 0), backplane::Destination::control(rig.chain, static_cast<size_t>(feathers::composite::Param::Macro1))));
	// A NONSENSE OUTPUT INDEX on the upstream one is refused too.
	CHECK_FALSE(rig.inner->mayRoute(backplane::Source::outerOutput(before, 17), dest));
	// THE UPSTREAM ONE'S OUTPUT REACHES THE INNER READING once routed: its cell is the outer engine's.
	rig.inner->setRouteAmount(backplane::Source::outerOutput(before, 0), dest, 1.0f);
	rig.block();
	rig.block();
	rig.block();
	CHECK(rig.inner->routeAmount(backplane::Source::outerOutput(before, 0), dest) == doctest::Approx(1.0f));
}

TEST_CASE("§7: an inner route from an outer source survives the outer patch — saved in the chain's body, gated again at load with no panel open, the outer device under the id the file gave it") {
	const device::Descriptor* modulator = nullptr;
	for (const device::Descriptor& d : kDevices) {
		// A MODULATOR, NOT MERELY A SOURCE — SKLITTER is an fx that also publishes, and is registered first.
		if (d.outputCount() > 0 && d.mod != nullptr && d.audio == nullptr) {
			modulator = &d;
			break;
		}
	}
	REQUIRE(modulator != nullptr);
	INFO(modulator->traits.tag); // which device this positional pick landed on, said out loud on a red
	NestedRig a;
	const feathers::DeviceInstanceId before = a.addOuter(modulator->traits.tag);
	REQUIRE(before != feathers::kNoInstance);
	a.addChainWithFilter();
	const backplane::Source outerMacro = backplane::Source::outerCore(0);
	const backplane::Source outerMod = backplane::Source::outerOutput(before, 0);
	a.inner->setRouteAmount(outerMacro, a.dest(), 0.6f);
	a.inner->setRouteAmount(outerMod, a.dest(), 0.4f);
	a.block();
	a.block();
	REQUIRE(a.inner->routeAmount(outerMacro, a.dest()) == doctest::Approx(0.6f));
	REQUIRE(a.inner->routeAmount(outerMod, a.dest()) == doctest::Approx(0.4f));
	feathers::rack::RackPatch saved = a.outerBridge.patch();
	saved.present = true; // as a parsed stream would be: a rack, not a fresh instance

	NestedRig b;
	const feathers::rack::RouteRefusals refused = b.outerBridge.loadPatch(saved).routes;
	CHECK(refused.count == 0);
	b.outerBridge.drain();
	b.block();
	b.block();
	// THE MODULATOR CAME BACK UNDER THE ID THE FILE GAVE IT — the record clears and then keeps every
	// file id (or drops the row), so there is no re-wearing for an inner route to follow.
	CHECK(b.outerBridge.deviceAtPosition(0) == before);
	// THE CHAIN CAME BACK, WITH ITS GATE, its filter, and both routes naming the outer sources.
	const feathers::DeviceInstanceId chain = b.outerBridge.deviceAtPosition(1);
	REQUIRE(chain != feathers::kNoInstance);
	const size_t lane = b.outer.slotOf(chain);
	REQUIRE(lane != feathers::rack::RackEngine::kNoSlot);
	feathers::rack::RackBridge* inner = feathers::rack::bridgeOf(b.outer.slotAtLane(lane).storage.get());
	REQUIRE(inner != nullptr);
	CHECK(inner->hasOuterGate());
	const feathers::DeviceInstanceId filter = inner->deviceAtPosition(0);
	REQUIRE(filter != feathers::kNoInstance);
	const backplane::Destination dest = backplane::Destination::control(filter, a.cutoff);
	CHECK(inner->routeAmount(outerMacro, dest) == doctest::Approx(0.6f));
	CHECK(inner->routeAmount(outerMod, dest) == doctest::Approx(0.4f));
	// AND IT WORKS: the outer macro moves the inner reading in the loaded rack.
	const float base = inner->fieldAt(filter, a.cutoff);
	b.outerBridge.setMacro(0, 0.75f);
	b.outerBridge.drain();
	b.block();
	CHECK(inner->modulatedFieldAt(filter, a.cutoff) > base);
}

TEST_CASE("§7/§8, the per-voice half: an inner engine running the outer host's voices reads entry v of a per-voice outer source for voice v, and the collapsed cell for the block") {
	// THE PASS-THROUGH MODEL: a chain's inner engine runs the outer host's voice count,
	// so an inner per-voice resolve for voice v reads entry v of an outer source's run — through the
	// reader's per-voice half — and the block resolve reads the collapsed cell. Exercised with a
	// hand-made outer host so the claim is about the seam: the run is a plain array.
	struct FakeOuter {
		float collapsed = 0.1f;
		float run[4] = { 0.2f, 0.4f, 0.6f, 0.8f };
	};
	FakeOuter host;
	feathers::rack::RackEngine engine { feathers::rack::kRackAxes, feathers::hostCatalogue() };
	feathers::rack::RackBridge bridge { engine };
	engine.prepare(48000.0, 4096);
	engine.setVoices(4); // what `ContainerOps::setVoices` hands a chain at provision
	device::OuterSources outer;
	outer.host = &host;
	outer.self = 99;
	outer.cell = [](const void* h, backplane::Source) -> const float* { return &static_cast<const FakeOuter*>(h)->collapsed; };
	outer.perVoice = [](const void* h, backplane::Source) -> const float* { return static_cast<const FakeOuter*>(h)->run; };
	outer.upstream = [](const void*, DeviceInstanceId, backplane::Source) { return true; };
	outer.voices = 4;
	engine.setOuterSources(outer);
	device::OuterGate gate;
	gate.host = &host;
	gate.self = 99;
	gate.upstream = [](const void*, DeviceInstanceId, backplane::Source) { return true; };
	bridge.setOuterGate(gate);
	const feathers::DeviceInstanceId filter = bridge.addDevice(fx::filter::kTraits.tag);
	REQUIRE(filter != feathers::kNoInstance);
	size_t cutoff = 0;
	while (cutoff < fx::filter::kDescriptor.params.size() && (!fx::filter::kDescriptor.params[cutoff].modulatable || static_cast<int>(cutoff) == fx::filter::kDescriptor.traits.enableControl)) {
		++cutoff;
	}
	const backplane::Destination dest = backplane::Destination::control(filter, cutoff);
	bridge.setRouteAmount(backplane::Source::outerOutput(7, 0), dest, 1.0f);
	bridge.drain();
	std::vector<float> l(64, 0.0f);
	std::vector<float> r(64, 0.0f);
	engine.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
	const size_t lane = engine.slotOf(filter);
	REQUIRE(lane != feathers::rack::RackEngine::kNoSlot);
	const float base = engine.fieldAt(lane, cutoff);
	const float block = engine.modulatedFieldAt(lane, cutoff) - base;
	const float v0 = engine.resolvedFieldForVoiceForTest(lane, cutoff, 0) - base;
	const float v2 = engine.resolvedFieldForVoiceForTest(lane, cutoff, 2) - base;
	const float v3 = engine.resolvedFieldForVoiceForTest(lane, cutoff, 3) - base;
	CHECK(block > 0.0f); // the collapsed 0.1
	CHECK(v0 > block);   // 0.2
	CHECK(v2 > v0);
	CHECK(v3 > v2);
	CHECK(v2 == doctest::Approx(v0 * 3.0f).epsilon(1e-2)); // 0.6 against 0.2: the reach is linear in the source
	CHECK(v3 == doctest::Approx(v0 * 4.0f).epsilon(1e-2));
}

TEST_CASE("§7 hygiene: an outer modulator removed, or moved downstream of the chain, takes its inner routes with it at the host's service — dropped, and kept to be said by the host and by the chain") {
	const device::Descriptor* modulator = nullptr;
	for (const device::Descriptor& d : kDevices) {
		// A MODULATOR, NOT MERELY A SOURCE — SKLITTER is an fx that also publishes, and is registered first.
		if (d.outputCount() > 0 && d.mod != nullptr && d.audio == nullptr) {
			modulator = &d;
			break;
		}
	}
	REQUIRE(modulator != nullptr);
	INFO(modulator->traits.tag); // which device this positional pick landed on, said out loud on a red
	SUBCASE("removed") {
		NestedRig rig;
		const feathers::DeviceInstanceId before = rig.addOuter(modulator->traits.tag);
		rig.addChainWithFilter();
		const backplane::Source outerMod = backplane::Source::outerOutput(before, 0);
		rig.inner->setRouteAmount(outerMod, rig.dest(), 0.5f);
		REQUIRE(rig.inner->routeAmount(outerMod, rig.dest()) == doctest::Approx(0.5f));
		(void)rig.outerBridge.takeLoadReport().routes; // nothing pending before the removal
		rig.outerBridge.removeDevice(before);
		rig.outerBridge.drain();
		// THE HOST'S SERVICE REACHES INSIDE: the inner route is gone, and both hosts hold the notice.
		rig.outerBridge.reapRetired();
		CHECK(rig.inner->routeAmount(outerMod, rig.dest()) == 0.0f);
		const feathers::rack::RouteRefusals outerSaid = rig.outerBridge.takeLoadReport().routes;
		CHECK(outerSaid.count == 1);
		CHECK(outerSaid.firstSource == backplane::Source::output(before, 0)); // spelled the outer way
		CHECK(outerSaid.firstDestination.device == rig.chain);                  // "inside the chain"
		const feathers::rack::RouteRefusals innerSaid = rig.inner->takeLoadReport().routes;
		CHECK(innerSaid.count == 1);
		CHECK(innerSaid.firstSource == outerMod);        // spelled the inner way
		CHECK(innerSaid.firstDestination == rig.dest()); // the exact inner pair
		// SAID ONCE: a second service has nothing new.
		rig.outerBridge.reapRetired();
		CHECK(rig.outerBridge.takeLoadReport().routes.count == 0);
	}
	SUBCASE("moved downstream") {
		NestedRig rig;
		const feathers::DeviceInstanceId before = rig.addOuter(modulator->traits.tag);
		rig.addChainWithFilter();
		const backplane::Source outerMod = backplane::Source::outerOutput(before, 0);
		rig.inner->setRouteAmount(outerMod, rig.dest(), 0.5f);
		REQUIRE(rig.inner->routeAmount(outerMod, rig.dest()) == doctest::Approx(0.5f));
		(void)rig.outerBridge.takeLoadReport().routes;
		// THE SAME PAIR THE OUTER HOST ITSELF WOULD REFUSE NOW decides: if the outer host still allows the
		// modulator to reach the chain after the move, the inner route stands; if not, it goes. The
		// rule (a reorder deletes the routes it breaks), asked of one authority.
		REQUIRE(rig.outerBridge.moveDeviceTo(before, 1));
		rig.outerBridge.drain();
		rig.outerBridge.reapRetired();
		const bool stillAllowed = rig.outerBridge.mayRoute(
		    backplane::Source::output(before, 0),
		    backplane::Destination::control(rig.chain, static_cast<size_t>(feathers::composite::Param::Macro1)));
		CHECK((rig.inner->routeAmount(outerMod, rig.dest()) != 0.0f) == stillAllowed);
		CHECK((rig.outerBridge.takeLoadReport().routes.count == 1) == !stillAllowed);
	}
	SUBCASE("a loaded body whose outer source is gone: refused at load, kept for the chain's panel to say") {
		NestedRig a;
		const feathers::DeviceInstanceId before = a.addOuter(modulator->traits.tag);
		a.addChainWithFilter();
		a.inner->setRouteAmount(backplane::Source::outerOutput(before, 0), a.dest(), 0.5f);
		a.block();
		feathers::rack::RackPatch saved = a.outerBridge.patch();
		saved.present = true;
		// THE MODULATOR IS TAKEN OUT OF THE FILE; the chain's body still names it.
		std::erase_if(saved.device, [&](const auto& d) { return d.instanceId == before; });
		NestedRig b;
		(void)b.outerBridge.loadPatch(saved);
		b.outerBridge.drain();
		b.block();
		const feathers::DeviceInstanceId chain = b.outerBridge.deviceAtPosition(0);
		REQUIRE(chain != feathers::kNoInstance);
		feathers::rack::RackBridge* inner = feathers::rack::bridgeOf(b.outer.slotAtLane(b.outer.slotOf(chain)).storage.get());
		REQUIRE(inner != nullptr);
		const feathers::rack::RouteRefusals said = inner->takeLoadReport().routes;
		CHECK(said.count == 1);
		CHECK(said.firstSource == backplane::Source::outerOutput(before, 0));
	}
}

TEST_CASE("§7f: an inner knob's \"routed elsewhere\" mark knows about a route from the host OUTSIDE — the inner graph holds the outer-sourced route, so the one graph the mark is asked of already has it; and an outer knob is never an inner destination") {
	// FILED AS A GAP IN §7f AND FOUND NOT TO BE ONE: the mark is asked per DESTINATION of the host that
	// holds the destination's routes. An inner control's routes — from inner sources and from outer ones
	// alike (`SourceKind::Outer`) — are in the inner record's graph, so `routedElsewhere` there sees an
	// outer route; and an outer control is in the outer graph, which no inner route can name.
	NestedRig rig;
	rig.addChainWithFilter();
	const backplane::Destination dest = rig.dest();
	const backplane::Source outerMacro = backplane::Source::outerCore(0);
	rig.inner->setRouteAmount(outerMacro, dest, 0.5f);
	REQUIRE(rig.inner->routeAmount(outerMacro, dest) == doctest::Approx(0.5f));
	// AN INNER MACRO ARMED: the outer route is "elsewhere". NOTHING ARMED: still elsewhere.
	CHECK(rig.inner->routedElsewhere(dest, backplane::Source::core(0)));
	CHECK(rig.inner->routedElsewhere(dest, backplane::Source::nobody()));
	// THE OUTER MACRO ARMED — spelled as the inner state spells it (`asOuter`): its own route is not.
	CHECK_FALSE(rig.inner->routedElsewhere(dest, outerMacro));
	// AND THE CHAIN'S OWN MACRO, AN OUTER KNOB, is reached by nothing — the inner route did not leak up.
	CHECK_FALSE(rig.outerBridge.routedElsewhere(backplane::Destination::control(rig.chain, 0),
	                                            backplane::Source::nobody()));
}

TEST_CASE("backlog DH: a modulator's Amount scales every output the host publishes — unity by default, half at 0.5, inverted at -1, nothing at 0, and itself a destination a macro can ride") {
	rack::RackEngine rack(kAxes, hostCatalogue());
	rack.prepare(48000.0, 4096);
	const size_t env = rack.addDevice(dahdsr::kTraits.tag);
	REQUIRE(env != rack::RackEngine::kNoSlot);
	// A FAST ATTACK TO A FULL SUSTAIN, so the envelope's own output settles at a number the case can
	// restate: the published cell is then the sustain level times the Amount, and nothing else.
	rack.setField(env, static_cast<size_t>(dahdsr::Param::Attack), dahdsr::kStageLog2SecondsMin);
	rack.setField(env, static_cast<size_t>(dahdsr::Param::Decay), dahdsr::kStageLog2SecondsMin);
	rack.setField(env, static_cast<size_t>(dahdsr::Param::Sustain), 1.0f);
	std::vector<float> l(512, 0.0f), r(512, 0.0f);
	const auto blocks = [&](int n) {
		for (int i = 0; i < n; ++i) {
			rack.processStereo(l.data(), r.data(), 512, backplane::unknownBlock());
		}
	};
	rack.setGate(true);
	blocks(16);
	const float unity = rack.publishedOutputAt(env, 0);
	REQUIRE(unity > 0.9f); // the envelope is up and holding its sustain
	CHECK(rack.fieldAt(env, static_cast<size_t>(dahdsr::Param::Amount)) == doctest::Approx(1.0f)); // the default

	rack.setField(env, static_cast<size_t>(dahdsr::Param::Amount), 0.5f);
	blocks(1);
	CHECK(rack.publishedOutputAt(env, 0) == doctest::Approx(0.5f * unity).epsilon(1e-3));
	rack.setField(env, static_cast<size_t>(dahdsr::Param::Amount), -1.0f);
	blocks(1);
	CHECK(rack.publishedOutputAt(env, 0) == doctest::Approx(-unity).epsilon(1e-3));
	rack.setField(env, static_cast<size_t>(dahdsr::Param::Amount), 0.0f);
	blocks(1);
	CHECK(rack.publishedOutputAt(env, 0) == doctest::Approx(0.0f));

	// A DESTINATION: the macro into the Amount lifts it from 0.5 to unity (0.25 × 1.0 × a span of 2),
	// and the published output follows — a macro riding a modulator's depth rides every route.
	rack.setField(env, static_cast<size_t>(dahdsr::Param::Amount), 0.5f);
	const size_t ordinal = dahdsr::kDescriptor.nthModulatableOfControl(static_cast<size_t>(dahdsr::Param::Amount));
	REQUIRE(ordinal < dahdsr::kDescriptor.modulatableCount());
	rack.setRouteAmount(test::macro(0), test::ordinalOf(rack, env, ordinal), 0.25f);
	rack.setMacro(0, 1.0f);
	blocks(1);
	CHECK(rack.resolvedFieldForTest(env, static_cast<size_t>(dahdsr::Param::Amount)) == doctest::Approx(1.0f));
	CHECK(rack.publishedOutputAt(env, 0) == doctest::Approx(unity).epsilon(1e-3));
	rack.setMacro(0, 0.0f);
	blocks(1);
	CHECK(rack.publishedOutputAt(env, 0) == doctest::Approx(0.5f * unity).epsilon(1e-3));
}

TEST_CASE("backlog FK: the standalone rack runs on the control tick — a routed envelope sounds the same at every host buffer size, and Bus Utility is handed tick windows") {
	// THE CLAIM: the rack resolves its rows once per 64-sample tick, not once per host block. Before, a 1024-sample
	// buffer resolved a route once per 1024 samples, so the sound depended on the buffer — and no device was handed
	// a window, because the rack set no tick phase. Asserted as VALUES: the same render at 64, 512 and 1024 is
	// bit-identical (the pieces are the same tick pieces), at 37 it matches to rounding, and Bus Utility's own count
	// says it followed windows.
	namespace bu = fx::busutil;
	struct Run {
		std::vector<float> left;
		size_t windowed = 0;
	};
	const auto render = [](int block) {
		rack::RackEngine rack(kAxes, hostCatalogue());
		rack.prepare(48000.0, 4096);
		const size_t fx = rack.addDevice(bu::kTraits.tag);
		const size_t env = rack.addDevice(dahdsr::kTraits.tag);
		REQUIRE(fx != rack::RackEngine::kNoSlot);
		REQUIRE(env != rack::RackEngine::kNoSlot);
		// A SLOW ATTACK, so the envelope moves across every tick of the render.
		const size_t attack = controlNamed(dahdsr::kDescriptor, "Attack");
		REQUIRE(attack < dahdsr::kDescriptor.params.size());
		rack.setField(env, attack, 0.6f);
		const size_t gain = static_cast<size_t>(bu::Param::Gain);
		rack.setRouteAmount(test::outputOf(rack, env, 0),
		                    test::ordinalOf(rack, fx, bu::kDescriptor.nthModulatableOfControl(gain)), 0.5f);
		rack.setGate(true);
		constexpr int kTotal = 4096;
		Run run;
		run.left.assign(kTotal, 0.0f);
		std::vector<float> right(kTotal, 0.0f);
		for (int i = 0; i < kTotal; ++i) {
			run.left[static_cast<size_t>(i)] = 0.25f;
			right[static_cast<size_t>(i)] = 0.25f;
		}
		for (int done = 0; done < kTotal; done += block) {
			const int n = std::min(block, kTotal - done);
			rack.processStereo(run.left.data() + done, right.data() + done, n, backplane::unknownBlock());
		}
		run.windowed = bu::windowedCallsForTest(rack.storageForTest(fx));
		return run;
	};
	const Run at64 = render(64);
	const Run at512 = render(512);
	const Run at1024 = render(1024);
	const Run at37 = render(37);
	// NOT VACUOUS: the envelope moved the gain across the render.
	REQUIRE(std::abs(at64.left.back() - at64.left[64]) > 0.01f);
	CHECK(at64.windowed > 0u);
	CHECK(at1024.windowed > 0u);
	CHECK(at512.left == at64.left);
	CHECK(at1024.left == at64.left);
	float worst = 0.0f;
	for (size_t i = 0; i < at64.left.size(); ++i) {
		worst = std::max(worst, std::abs(at37.left[i] - at64.left[i]));
	}
	CAPTURE(worst);
	CHECK(worst < 1e-4f);
}

//--------------------------------------------------------------------------------------------------//
// HOST SLOTS — the engine applies a slot's value before it resolves, and its modulation as an offset  //
// (host slots, task T5).                                                                            //
//                                                                                                   //
// Every case drives the WHOLE road a host drives: a `ChainState` carrying the bindings is published  //
// through `PublishedChainState`, adopted whole at a drain and converged (`Converger::converge`, which //
// hands the bindings to the engine), and the host's values reach the block through the               //
// `SlotValueBus` with no drain in between. Every claim is asserted as the VALUE `resolved` holds.     //
//--------------------------------------------------------------------------------------------------//
namespace {

/// THE HOST SIDE OF ONE ENGINE: a record's state published, drained and converged exactly as
/// `RackBridge::drain` does it, and a bus the engine reads its host slots from.
struct SlotRig {
	rack::RackEngine engine { kAxes, hostCatalogue() };
	rack::Converger converger { engine, &rack::Converger::fitsAnywhere };
	rack::PublishedChainState published { kAxes.deviceSlots, 1 };
	rack::ChainState current { kAxes.deviceSlots, 1 };
	rack::ChainState staging { kAxes.deviceSlots, 1 };
	uint32_t sequence = 0;
	rack::RackEngine::SlotBus bus;

	SlotRig() {
		engine.prepare(48000.0, 4096);
		engine.setHostSlotBus(&bus);
	}
	/// THE MESSAGE THREAD'S HALF: publish, then build what the state needs for that publish.
	void publish(const rack::ChainState& s) {
		REQUIRE(published.publish(s));
		(void)converger.provision(s, nullptr, published.sequence());
	}
	/// THE DRAIN — `RackBridge::drain`'s own two steps.
	void drain() {
		if (published.sequence() != sequence) {
			(void)published.adopt(&current, &staging, &sequence);
		}
		converger.converge(current, sequence);
	}
	void block() {
		std::vector<float> l(64, 0.0f), r(64, 0.0f);
		engine.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
	}
	size_t laneOf(DeviceInstanceId id) const {
		const size_t lane = engine.slotOf(id);
		REQUIRE(lane != rack::RackEngine::kNoSlot);
		return lane;
	}
};

constexpr DeviceInstanceId kSlotFilter = 7;

/// A FILTER UNDER `kSlotFilter` WHOSE ROW SAYS EVERY CONTROL, each at its default, cutoff at `cutoff`.
rack::ChainState filterState(float cutoff, size_t cutoffIndex) {
	rack::ChainState s(kAxes.deviceSlots, 1);
	rack::DeviceState row;
	row.tag = device::tagOf(filter::kTraits.tag);
	row.instanceId = kSlotFilter;
	row.fieldsPresent = filter::kDescriptor.params.size();
	for (size_t c = 0; c < filter::kDescriptor.params.size(); ++c) {
		row.field[c] = static_cast<float>(filter::kDescriptor.params[c].def);
	}
	row.field[cutoffIndex] = cutoff;
	REQUIRE(s.append(0, row));
	return s;
}

rack::SlotBinding bindingOf(DeviceInstanceId device, size_t control, size_t slot) {
	return rack::SlotBinding { .device = device,
	                           .control = static_cast<uint16_t>(control),
	                           .slot = static_cast<uint16_t>(slot),
	                           .live = true };
}

/// A POINT A FRACTION OF THE WAY UP A CONTROL'S RANGE — from the declaration, never picked.
float fractionOf(const device::ParamDesc& p, double t) {
	return static_cast<float>(p.min + t * (p.max - p.min));
}

} // namespace

TEST_CASE("a bus write moves the bound control's resolved value the same block") {
	SlotRig rig;
	const size_t cutoff = controlNamed(filter::kDescriptor, "Cutoff");
	REQUIRE(cutoff < filter::kDescriptor.params.size());
	const device::ParamDesc& p = filter::kDescriptor.params[cutoff];
	const float recordValue = fractionOf(p, 0.25);
	constexpr size_t kSlot = 3;

	rack::ChainState s = filterState(recordValue, cutoff);
	REQUIRE(s.setSlotBinding(kSlot, bindingOf(kSlotFilter, cutoff, kSlot)));
	rig.publish(s);
	rig.drain();
	// THE BINDING CROSSED THE PUBLISH WHOLE and resolved to the engine's device.
	CHECK(rig.current.slotBindings()[kSlot] == bindingOf(kSlotFilter, cutoff, kSlot));
	CHECK(rig.engine.hostSlotBindingCountForTest() == 1);
	const size_t lane = rig.laneOf(kSlotFilter);

	// A SLOT NOBODY HAS WRITTEN (generation 0) LEAVES THE RECORD'S VALUE.
	rig.block();
	CHECK(rig.engine.resolvedFieldForTest(lane, cutoff) == device::coerceControl(p, recordValue));

	// THE HOST WRITES, AND THE VERY NEXT BLOCK RESOLVES IT — no drain between them.
	const float hostValue = fractionOf(p, 0.6);
	rig.bus.onHostValue(kSlot, hostValue);
	rig.block();
	CHECK(rig.engine.resolvedFieldForTest(lane, cutoff) == device::coerceControl(p, hostValue));
	CHECK(rig.engine.fieldAt(lane, cutoff) == device::coerceControl(p, hostValue)); // the BASE moved
	CHECK(rig.engine.resolvedFieldForTest(lane, cutoff) != device::coerceControl(p, recordValue));

	// COERCED AS EVERY OTHER WRITE IS: a number past the range lands where `coerceControl` puts it.
	const float past = static_cast<float>(p.max + (p.max - p.min));
	REQUIRE(device::coerceControl(p, past) != past); // the case is not vacuous: coercion changes it
	rig.bus.onHostValue(kSlot, past);
	rig.block();
	CHECK(rig.engine.resolvedFieldForTest(lane, cutoff) == device::coerceControl(p, past));
}

TEST_CASE("an adopt of a stale record does not revert a host-written value") {
	// PLAN §0.3: host automation reaches the engine on the audio thread, the record learns of it later (T6's
	// mirror). A drain that adopts a record written BEFORE that host write rewrites the row from it — and the
	// host's value must still be what the next block resolves.
	SlotRig rig;
	const size_t cutoff = controlNamed(filter::kDescriptor, "Cutoff");
	REQUIRE(cutoff < filter::kDescriptor.params.size());
	const device::ParamDesc& p = filter::kDescriptor.params[cutoff];
	const float staleValue = fractionOf(p, 0.2);
	const float hostValue = fractionOf(p, 0.7);
	constexpr size_t kSlot = 11;

	rack::ChainState s = filterState(staleValue, cutoff);
	REQUIRE(s.setSlotBinding(kSlot, bindingOf(kSlotFilter, cutoff, kSlot)));
	rig.publish(s);
	rig.drain();
	const size_t lane = rig.laneOf(kSlotFilter);
	rig.bus.onHostValue(kSlot, hostValue);
	rig.block();
	REQUIRE(rig.engine.resolvedFieldForTest(lane, cutoff) == device::coerceControl(p, hostValue));

	// A NEW PUBLISH OF A RECORD THAT STILL SAYS THE OLD CUTOFF (another control moved, so it is a new
	// publish the drain must adopt).
	const size_t other = cutoff == 0 ? 1 : 0;
	rack::DeviceState* row = s.mutableAt(0, 0);
	REQUIRE(row != nullptr);
	row->field[other] = device::coerceControl(filter::kDescriptor.params[other],
	                                          fractionOf(filter::kDescriptor.params[other], 0.5));
	const uint32_t before = rig.sequence;
	rig.publish(s);
	rig.drain();
	REQUIRE(rig.sequence != before); // the drain DID adopt the stale record
	// AND IT DID WRITE THE STALE ROW — without this the case below could pass on a drain that wrote nothing.
	CHECK(rig.engine.fieldAt(lane, cutoff) == device::coerceControl(p, staleValue));

	rig.block();
	CHECK(rig.engine.resolvedFieldForTest(lane, cutoff) == device::coerceControl(p, hostValue));
	CHECK(rig.engine.fieldAt(lane, cutoff) == device::coerceControl(p, hostValue));

	// AND A DRAIN WITH NO NEW PUBLISH (the rack's drain runs every block) is the same: the row is rewritten
	// from the record every time, and the bus is put back over it every time.
	rig.drain();
	rig.block();
	CHECK(rig.engine.resolvedFieldForTest(lane, cutoff) == device::coerceControl(p, hostValue));
}

TEST_CASE("host mod adds span × mod through withRouteOffset") {
	SlotRig rig;
	const size_t cutoff = controlNamed(filter::kDescriptor, "Cutoff");
	REQUIRE(cutoff < filter::kDescriptor.params.size());
	const device::ParamDesc& p = filter::kDescriptor.params[cutoff];
	REQUIRE(p.modulatable);
	// A CONTROL THAT IS NO MODULATION DESTINATION, for the half of the rule that says it ignores host mod.
	size_t fixed = filter::kDescriptor.params.size();
	for (size_t c = 0; c < filter::kDescriptor.params.size(); ++c) {
		if (!filter::kDescriptor.params[c].modulatable) {
			fixed = c;
			break;
		}
	}
	REQUIRE(fixed < filter::kDescriptor.params.size());
	const device::ParamDesc& q = filter::kDescriptor.params[fixed];
	const float base = fractionOf(p, 0.4);
	constexpr size_t kSlot = 0;
	constexpr size_t kFixedSlot = 1;

	rack::ChainState s = filterState(base, cutoff);
	REQUIRE(s.setSlotBinding(kSlot, bindingOf(kSlotFilter, cutoff, kSlot)));
	REQUIRE(s.setSlotBinding(kFixedSlot, bindingOf(kSlotFilter, fixed, kFixedSlot)));
	rig.publish(s);
	rig.drain();
	const size_t lane = rig.laneOf(kSlotFilter);
	const float fixedBase = rig.engine.fieldAt(lane, fixed);

	// HOST MOD ALONE: base + mod × span, through the shared rule. RESTATED here from the declaration.
	const float mod = 0.25f;
	rig.bus.onHostMod(kSlot, mod);
	rig.block();
	const double span = device::routeSpanOf(p);
	const float expected = static_cast<float>(
	    device::withRouteOffset(p, static_cast<double>(base), static_cast<double>(mod) * span));
	CHECK(rig.engine.resolvedFieldForTest(lane, cutoff) == expected);
	CHECK(rig.engine.resolvedFieldForTest(lane, cutoff) != device::coerceControl(p, base)); // not vacuous
	CHECK(rig.engine.fieldAt(lane, cutoff) == device::coerceControl(p, base)); // an offset: the base stays

	// ON TOP OF A ROUTE: the route's offset first, then the host's, summed in double.
	const float amount = 0.125f;
	const size_t ordinal = filter::kDescriptor.nthModulatableOfControl(cutoff);
	rig.engine.setRouteAmount(test::macro(0), test::ordinalOf(rig.engine, lane, ordinal), amount);
	rig.engine.setMacro(0, 1.0f);
	rig.block();
	double offset = static_cast<double>(amount) * 1.0 * span;
	offset += static_cast<double>(mod) * span;
	CHECK(rig.engine.resolvedFieldForTest(lane, cutoff)
	      == static_cast<float>(device::withRouteOffset(p, static_cast<double>(base), offset)));

	// A CONTROL THAT IS NO MODULATION DESTINATION TAKES A SLOT'S VALUE AND IGNORES ITS MOD.
	rig.bus.onHostMod(kFixedSlot, 0.5f);
	rig.block();
	CHECK(rig.engine.resolvedFieldForTest(lane, fixed) == fixedBase);
	const float fixedValue = static_cast<float>(q.max);
	rig.bus.onHostValue(kFixedSlot, fixedValue);
	rig.block();
	CHECK(rig.engine.resolvedFieldForTest(lane, fixed) == device::coerceControl(q, fixedValue));

	// UNBOUND, THE OFFSET GOES WITH THE BINDING — the route alone is left.
	s.clearSlotBindings();
	rig.publish(s);
	rig.drain();
	rig.block();
	CHECK(rig.engine.resolvedFieldForTest(lane, cutoff)
	      == static_cast<float>(device::withRouteOffset(p, static_cast<double>(base),
	                                                    static_cast<double>(amount) * 1.0 * span)));
}

TEST_CASE("an unbound control ignores the bus") {
	SlotRig rig;
	const size_t cutoff = controlNamed(filter::kDescriptor, "Cutoff");
	REQUIRE(cutoff < filter::kDescriptor.params.size());
	const device::ParamDesc& p = filter::kDescriptor.params[cutoff];
	constexpr size_t kBound = 2;
	constexpr size_t kNowhere = 9; // a binding to a device this engine does not run; every other slot has none

	rack::ChainState s = filterState(fractionOf(p, 0.3), cutoff);
	REQUIRE(s.setSlotBinding(kBound, bindingOf(kSlotFilter, cutoff, kBound)));
	REQUIRE(s.setSlotBinding(kNowhere, bindingOf(kSlotFilter + 100, cutoff, kNowhere)));
	rig.publish(s);
	rig.drain();
	CHECK(rig.engine.hostSlotBindingCountForTest() == 1); // the one to nowhere does not resolve
	const size_t lane = rig.laneOf(kSlotFilter);
	rig.block();
	std::vector<float> before;
	for (size_t c = 0; c < filter::kDescriptor.params.size(); ++c) {
		before.push_back(rig.engine.resolvedFieldForTest(lane, c));
	}

	// EVERY SLOT BUT THE BOUND ONE, WRITTEN AND MODULATED: nothing moves.
	for (size_t slot = 0; slot < hostslots::kSlotBusCapacity; ++slot) {
		if (slot != kBound) {
			rig.bus.onHostValue(slot, fractionOf(p, 0.9));
			rig.bus.onHostMod(slot, 0.5f);
		}
	}
	rig.block();
	for (size_t c = 0; c < filter::kDescriptor.params.size(); ++c) {
		CAPTURE(c);
		CHECK(rig.engine.resolvedFieldForTest(lane, c) == before[c]);
	}

	// THE BOUND ONE MOVES ITS CONTROL AND ONLY ITS CONTROL — the rig can express a move.
	rig.bus.onHostValue(kBound, fractionOf(p, 0.8));
	rig.block();
	for (size_t c = 0; c < filter::kDescriptor.params.size(); ++c) {
		CAPTURE(c);
		CHECK(rig.engine.resolvedFieldForTest(lane, c)
		      == (c == cutoff ? device::coerceControl(p, fractionOf(p, 0.8)) : before[c]));
	}

	// AND ONCE UNBOUND, THE CONTROL IS THE RECORD'S AGAIN — the bus writes to its old slot move nothing.
	s.clearSlotBindings();
	rig.publish(s);
	rig.drain();
	rig.bus.onHostValue(kBound, fractionOf(p, 0.1));
	rig.bus.onHostMod(kBound, 0.5f);
	rig.block();
	CHECK(rig.engine.hostSlotBindingCountForTest() == 0);
	CHECK(rig.engine.resolvedFieldForTest(lane, cutoff) == before[cutoff]);
}

TEST_CASE("applyHostSlots allocates nothing") {
	// CLAUDE.md: no allocation on the audio thread after activate. `applyHostSlots` runs in every block and
	// `setHostSlotBindings` in every drain; both are counted here, through this binary's operator new.
	SlotRig rig;
	rack::ChainState s = filterState(fractionOf(filter::kDescriptor.params[0], 0.5), 0);
	size_t bound = 0;
	for (size_t c = 0; c < filter::kDescriptor.params.size() && bound < hostslots::kSlotBusCapacity; ++c) {
		REQUIRE(s.setSlotBinding(bound, bindingOf(kSlotFilter, c, bound)));
		++bound;
	}
	rig.publish(s);
	rig.drain();
	REQUIRE(rig.engine.hostSlotBindingCountForTest() == bound);
	for (size_t slot = 0; slot < bound; ++slot) {
		rig.bus.onHostValue(slot, 0.5f);
		rig.bus.onHostMod(slot, 0.25f);
	}
	// A REPUBLISH, MADE BEFORE THE WINDOW OPENS (the message thread's half), so the counted drain below
	// really ADOPTS it rather than finding nothing new.
	REQUIRE(rig.published.publish(s));
	const uint32_t adoptedBefore = rig.sequence;
	uint64_t allocs = 0, frees = 0;
	{
		AllocWindow w;
		rig.engine.applyHostSlots();
		rig.engine.setHostSlotBindings(rig.current.slotBindings());
		rig.engine.applyHostSlots();
		// AND THE DRAIN THAT CARRIES THEM: the republish adopted whole and converged.
		rig.drain();
		allocs = w.allocs();
		frees = w.frees();
	}
	CHECK(rig.sequence != adoptedBefore); // the counted drain DID adopt
	CHECK(allocs == 0);
	CHECK(frees == 0);
	// NOT VACUOUS: the bindings survived the counted drain and there are values to apply.
	CHECK(rig.engine.hostSlotBindingCountForTest() == bound);
	rig.block();
	const size_t lane = rig.laneOf(kSlotFilter);
	CHECK(rig.engine.fieldAt(lane, 0) == device::coerceControl(filter::kDescriptor.params[0], 0.5f));
}

namespace {
/// A DEVICE'S ROW AT ITS DECLARED DEFAULTS, every control said.
rack::DeviceState defaultRow(const char* tag, const device::Descriptor& d, DeviceInstanceId id) {
	rack::DeviceState row;
	row.tag = device::tagOf(tag);
	row.instanceId = id;
	row.fieldsPresent = d.params.size();
	for (size_t c = 0; c < d.params.size(); ++c) {
		row.field[c] = static_cast<float>(d.params[c].def);
	}
	return row;
}
} // namespace

TEST_CASE("host mod rides the per-sample window resolve too — sample by sample, on top of a moving route") {
	// A WINDOWED CONTROL (`AudioOps::windowedControls`) whose input MOVES is resolved per sample by
	// `resolveWindowInto`, not by `resolveControl` — the device reads the window, not `resolved`. So host mod
	// must be in the window as well, or a modulated slot would sound unmodulated exactly when a route moves.
	namespace bu = feathers::fx::busutil;
	namespace lfo = feathers::mod::lfo;
	SlotRig rig;
	constexpr DeviceInstanceId kLfo = 20;
	constexpr DeviceInstanceId kBus = 21;
	constexpr size_t kSlot = 6;
	const size_t gain = static_cast<size_t>(bu::Param::Gain);
	const device::ParamDesc& pd = bu::kDescriptor.params[gain];
	const double span = device::routeSpanOf(pd);
	REQUIRE(pd.modulatable);
	REQUIRE(span > 0.0);

	rack::ChainState s(kAxes.deviceSlots, 1);
	REQUIRE(s.append(0, defaultRow(lfo::kTraits.tag, lfo::kDescriptor, kLfo)));
	REQUIRE(s.append(0, defaultRow(bu::kTraits.tag, bu::kDescriptor, kBus)));
	REQUIRE(s.setSlotBinding(kSlot, bindingOf(kBus, gain, kSlot)));
	rig.publish(s);
	rig.drain();
	const size_t busLane = rig.laneOf(kBus);
	constexpr float kAmount = 0.25f;
	rig.engine.setRouteAmount(backplane::Source::output(kLfo, 0), backplane::Destination::control(kBus, gain), kAmount);
	const float mod = -0.125f;
	rig.bus.onHostMod(kSlot, mod);
	const float base = rig.engine.fieldAt(busLane, gain);

	int moving = 0;
	for (int t = 0; t < 64 && moving < 3; ++t) {
		rig.block();
		const float* lfoWindow = rig.engine.outputWindowOf(kLfo, 0);
		const float* w = rig.engine.controlWindowForTest(busLane, gain);
		REQUIRE(lfoWindow != nullptr);
		REQUIRE(w != nullptr);
		if (device::tickWindowFlat(w)) {
			continue;
		}
		++moving;
		for (const int i : { 0, 31, 63 }) {
			CAPTURE(t);
			CAPTURE(i);
			// RESTATED: the route's offset, then the host's, summed in double — `resolveWindowInto`'s order.
			double offset = static_cast<double>(kAmount) * static_cast<double>(lfoWindow[i]) * span;
			offset += static_cast<double>(mod) * span;
			const float expected =
			    offset != 0.0 ? static_cast<float>(device::withRouteOffset(pd, static_cast<double>(base), offset)) : base;
			CHECK(w[i] == expected);
		}
	}
	REQUIRE(moving >= 1); // or the loop asserted nothing
}

TEST_CASE("a lane refilled by another device outside a drain is not driven by the old device's binding") {
	// THE ADDRESS IS NOT THE IDENTITY (CLAUDE.md). A binding resolves to a lane at a drain; a lane emptied and
	// refilled with no drain between (the loader and the test extension still call the engine directly) holds
	// ANOTHER device under the same address, and the bus must not write that device's control.
	SlotRig rig;
	const size_t cutoff = controlNamed(filter::kDescriptor, "Cutoff");
	REQUIRE(cutoff < filter::kDescriptor.params.size());
	const device::ParamDesc& p = filter::kDescriptor.params[cutoff];
	constexpr size_t kSlot = 4;
	rack::ChainState s = filterState(fractionOf(p, 0.3), cutoff);
	REQUIRE(s.setSlotBinding(kSlot, bindingOf(kSlotFilter, cutoff, kSlot)));
	rig.publish(s);
	rig.drain();
	const size_t lane = rig.laneOf(kSlotFilter);
	rig.block();

	REQUIRE(rig.engine.removeDeviceAtLane(lane));
	const size_t again = rig.engine.addDevice(filter::kTraits.tag);
	REQUIRE(again == lane); // the same address...
	const DeviceInstanceId other = rig.engine.slotAtLane(lane).id;
	REQUIRE(other != kSlotFilter); // ...under another identity
	REQUIRE(rig.engine.hostSlotBindingCountForTest() == 1); // no drain has re-resolved the binding
	const float otherBase = rig.engine.fieldAt(lane, cutoff);
	REQUIRE(device::coerceControl(p, fractionOf(p, 0.9)) != otherBase); // a write WOULD show

	rig.bus.onHostValue(kSlot, fractionOf(p, 0.9));
	rig.bus.onHostMod(kSlot, 0.5f);
	rig.block();
	CHECK(rig.engine.fieldAt(lane, cutoff) == otherBase);
	CHECK(rig.engine.resolvedFieldForTest(lane, cutoff) == otherBase);
}

TEST_CASE("host mod goes back to zero: a bus mod of 0 while bound, and any mod once unbound") {
	SlotRig rig;
	const size_t cutoff = controlNamed(filter::kDescriptor, "Cutoff");
	REQUIRE(cutoff < filter::kDescriptor.params.size());
	const device::ParamDesc& p = filter::kDescriptor.params[cutoff];
	const float base = device::coerceControl(p, fractionOf(p, 0.45));
	constexpr size_t kSlot = 12;
	rack::ChainState s = filterState(base, cutoff);
	REQUIRE(s.setSlotBinding(kSlot, bindingOf(kSlotFilter, cutoff, kSlot)));
	rig.publish(s);
	rig.drain();
	const size_t lane = rig.laneOf(kSlotFilter);

	rig.bus.onHostMod(kSlot, 0.3f);
	rig.block();
	REQUIRE(rig.engine.resolvedFieldForTest(lane, cutoff) != base); // the mod reached it

	// BOUND, THE HOST SENDS 0: the offset is gone the next block — no route, so resolved IS the base.
	rig.bus.onHostMod(kSlot, 0.0f);
	rig.block();
	CHECK(rig.engine.resolvedFieldForTest(lane, cutoff) == base);

	// UNBOUND WHILE MODULATED: the bus still says 0.3, and the control no longer hears it.
	rig.bus.onHostMod(kSlot, 0.3f);
	rig.block();
	REQUIRE(rig.engine.resolvedFieldForTest(lane, cutoff) != base);
	s.clearSlotBindings();
	rig.publish(s);
	rig.drain();
	rig.block();
	CHECK(rig.engine.hostSlotBindingCountForTest() == 0);
	CHECK(rig.engine.resolvedFieldForTest(lane, cutoff) == base);
}

//--------------------------------------------------------------------------------------------------//
// RESONATE'S CYCLE THROUGH THE RACK ROAD (v11: the plucks land on the grid when playing)            //
//--------------------------------------------------------------------------------------------------//

namespace {
namespace rsn = feathers::fx::resonate;
/// A PLAYING TRANSPORT through the rack in host blocks of `block` frames, 120 bpm, a 1/16 Cycle, the song starting at
/// `start` beats; after `loopAfter` blocks the host loops back to `loopTo` (a block boundary, as a host announces it).
/// Returns each logged pluck's error in samples against its grid sample: the first sample at or past each 1/16 of the
/// song in its block, restated from the block's position and tempo.
std::vector<long> cycleGridErrors(int block, double start, int blocks, int loopAfter = -1, double loopTo = 0.0) {
	constexpr double sr = 48000.0;
	constexpr double spb = 0.5;
	constexpr double period = 0.25; // 1/16 in beats
	rack::RackEngine rack(kAxes, hostCatalogue());
	rack.prepare(sr, 4096);
	const size_t slot = rack.addDevice(rsn::kTraits.tag);
	REQUIRE(slot != rack::RackEngine::kNoSlot);
	size_t sync = 0;
	for (size_t i = 0; i < rsn::kCycleSyncNames.size(); ++i) {
		if (std::string_view(rsn::kCycleSyncNames[i]) == "1/16") {
			sync = i;
		}
	}
	REQUIRE(sync != 0);
	rack.setField(slot, static_cast<size_t>(rsn::Param::DroneCycleSync), static_cast<float>(sync));
	rack.setField(slot, static_cast<size_t>(rsn::Param::DroneOut), 0.5f);
	std::vector<uint64_t> want;
	std::vector<float> l(static_cast<size_t>(block), 0.0f);
	std::vector<float> r(static_cast<size_t>(block), 0.0f);
	double pos = start;
	uint64_t at = 0;
	std::vector<long> errors;
	int logged = 0;
	for (int b = 0; b < blocks; ++b) {
		if (b == loopAfter) {
			pos = loopTo;
		}
		backplane::BlockContext ctx = backplane::unknownBlock();
		ctx.clock = device::TransportClock::fromSecondsPerBeat(spb);
		ctx.clock.songPositionBeats = pos;
		ctx.clock.songPositionSeconds = pos * spb;
		ctx.clock.playing = true;
		const double end = pos + static_cast<double>(block) / (sr * spb);
		for (double k = std::ceil(pos / period - 1e-9); k * period < end - 1e-12; k += 1.0) {
			const double n = std::max(0.0, std::ceil((k * period - pos) * sr * spb - 1e-6));
			if (n < block) {
				want.push_back(at + static_cast<uint64_t>(n) + 1); // the pluck log counts samples from 1
			}
		}
		std::fill(l.begin(), l.end(), 0.0f);
		std::fill(r.begin(), r.end(), 0.0f);
		rack.processStereo(l.data(), r.data(), block, ctx);
		pos = end;
		at += static_cast<uint64_t>(block);
		// READ THE LOG AS IT FILLS (it keeps the last 64).
		const rsn::ResonateDsp& dsp = rsn::dspForTest(rack.storageForTest(slot));
		for (; logged < dsp.cyclePluckCountForTest(); ++logged) {
			const uint64_t got = dsp.cyclePluckForTest(logged).at;
			const long wantAt = logged < static_cast<int>(want.size()) ? static_cast<long>(want[static_cast<size_t>(logged)]) : -1;
			errors.push_back(static_cast<long>(got) - wantAt);
		}
	}
	CHECK(logged == static_cast<int>(want.size()));
	return errors;
}
} // namespace

TEST_CASE("RESONATE through the rack: every Cycle pluck lands on its grid sample at any host block size, from any start, across a loop") {
	// Plucks must land on the grid while the transport plays. A playing transport at 120 bpm through
	// the rack road (the engine's 64-frame pieces, each carrying its own position), a 1/16 Cycle, host blocks of 64, 256,
	// 441, 512 and 1024 frames, the song starting on beat 0 or at 0.37, and a loop back to beat 1 at a block boundary
	// half-way. Each logged pluck against its grid sample, restated from the block's position and tempo: MEASURED 0
	// samples of error everywhere, 24-25 plucks a run (v11, before any change: the scheduling was already per sample).
	for (const int block : { 64, 256, 441, 512, 1024 }) {
		for (const double start : { 0.0, 0.37 }) {
			CAPTURE(block);
			CAPTURE(start);
			const std::vector<long> e = cycleGridErrors(block, start, static_cast<int>(3.0 * 48000.0 / block),
			                                            static_cast<int>(1.5 * 48000.0 / block), 1.0);
			REQUIRE(e.size() >= 20u);
			long worst = 0;
			for (const long x : e) {
				worst = std::max(worst, std::abs(x));
			}
			CHECK(worst <= 1);
		}
	}
}

TEST_CASE("RESONATE through the rack: a held note's CLAP tuning expression reaches the device and bends that note's head") {
	// THE MPE BEND END TO END (v18): the rack folds a tuning expression into its own note context (updateNoteContext) from
	// what LEFT the chain, and the chain offers every event to every note device unfiltered, so the Expression reaches
	// Resonate's note lane. A Resonate alone in the rack, 48 and 55 held; then 48 tuned +2 semitones (CLAP_NOTE_EXPRESSION_TUNING,
	// 2 in clap/events.h): after 0.3 s 48's head runs at 2^(2/12) of its increment and 55's is unchanged to the bit. Known-bad:
	// the Expression case of ResonateDsp::applyNotes a no-op.
	constexpr double sr = 48000.0;
	rack::RackEngine rack(kAxes, hostCatalogue());
	rack.prepare(sr, 4096);
	const size_t slot = rack.addDevice(rsn::kTraits.tag);
	REQUIRE(slot != rack::RackEngine::kNoSlot);
	rack.setField(slot, static_cast<size_t>(rsn::Param::StringBlend), 1.0f);
	std::vector<float> l(256, 0.0f);
	std::vector<float> r(256, 0.0f);
	const auto run = [&](double seconds) {
		for (int b = 0; b < static_cast<int>(seconds * sr / 256.0); ++b) {
			for (size_t i = 0; i < l.size(); ++i) {
				l[i] = r[i] = 0.01f * static_cast<float>(std::sin(0.05 * static_cast<double>(b * 256 + static_cast<int>(i))));
			}
			rack.processStereo(l.data(), r.data(), 256, backplane::BlockContext {});
		}
	};
	std::array<device::NoteEvent, 2> on {};
	on[0] = { device::NoteEvent::Kind::On, 0, 1, 0, 0, 48, 1.0 };
	on[1] = { device::NoteEvent::Kind::On, 0, 2, 0, 0, 55, 1.0 };
	rack.setBlockNotes(on.data(), on.size());
	run(0.1);
	const rsn::ResonateDsp& dsp = rsn::dspForTest(rack.storageForTest(slot));
	int a = -1;
	int b = -1;
	for (int k = 0; k < static_cast<int>(rsn::kMaxHeads); ++k) {
		a = dsp.headKeyForTest(k) == 48 ? k : a;
		b = dsp.headKeyForTest(k) == 55 ? k : b;
	}
	REQUIRE(a >= 0);
	REQUIRE(b >= 0);
	const double a0 = dsp.headIncrementForTest(a);
	const double b0 = dsp.headIncrementForTest(b);
	device::NoteEvent tune {};
	tune.kind = device::NoteEvent::Kind::Expression;
	tune.noteId = 1;
	tune.key = 48;
	tune.expressionId = 2;
	tune.expressionValue = 2.0;
	rack.setBlockNotes(&tune, 1);
	run(0.3);
	MESSAGE("through the rack, 48 tuned +2: its head x" << dsp.headIncrementForTest(a) / a0 << " (2^(2/12) = " << std::exp2(2.0 / 12.0) << "), 55's x"
	                                                    << dsp.headIncrementForTest(b) / b0);
	CHECK(dsp.headIncrementForTest(a) / a0 == doctest::Approx(std::exp2(2.0 / 12.0)).epsilon(1e-9));
	CHECK(dsp.headIncrementForTest(b) == b0);
}

//--------------------------------------------------------------------------------------------------//
// ROUTE TRANSFER CHIPS. Each curve's output, BY VALUE,                                                //
// through the engine's own sums: the collapsed row, each voice's row and the per-sample window.       //
//--------------------------------------------------------------------------------------------------//

namespace {
/// EVERY CURVE, in the menu's order — the cases sweep them rather than pick a favourite.
/// AND EACH KIND WITH A COEFFICIENT OFF ITS DEFAULT TOO (backlog PY): Log at exponent 1, Exp at 4, Phi at 8 periods.
constexpr std::array<device::Transfer, device::kTransferCount + 2> kEveryTransfer {
	device::Transfer::Lin, device::Transfer::Log, device::Transfer::Exp,
	device::Transfer::Abs, device::Transfer::Pos, device::Transfer::Neg,
	device::transferWithStep(device::Transfer::Log, device::kExpStepMin),
	device::transferWithStep(device::Transfer::Exp, 16),
	device::transferWithStep(device::Transfer::Phi, 3 * device::kPhiStepsPerOctave),
};
/// THE TRIANGLE, 0 → 1 → 0 over a cycle — for the φ curve's restatement.
double restatedTri(double p) {
	const double x = p - std::floor(p);
	return x < 0.5 ? 2.0 * x : 2.0 * (1.0 - x);
}
/// THE CURVES RESTATED BY HAND, independently of `shapeTransfer`, at the readings these cases write.
double restatedCurve(device::Transfer t, double x) {
	const double c = std::clamp(x, -1.0, 1.0);
	const double sign = c < 0 ? -1.0 : 1.0;
	const double m = std::abs(c);
	if (device::transferKind(t) == device::Transfer::Log || device::transferKind(t) == device::Transfer::Exp) {
		const double k = 2.0 * std::pow(2.0, device::transferStep(t) / 16.0);
		return sign * (device::transferKind(t) == device::Transfer::Log ? 1.0 - std::pow(1.0 - m, k) : std::pow(m, k));
	}
	if (device::transferKind(t) == device::Transfer::Phi) {
		const double phi = (1.0 + std::sqrt(5.0)) / 2.0;
		const double p = std::pow(2.0, device::transferStep(t) / 32.0);
		const double warp = 0.2 * (2.0 * restatedTri(p / phi * m + 0.25) - 1.0)
		                    + 0.12 * (2.0 * restatedTri(p / std::sqrt(phi) * m + 0.25) - 1.0);
		return sign * restatedTri(p * m + warp);
	}
	switch (t) {
	case device::Transfer::Lin: return x;
	case device::Transfer::Abs: return std::abs(c);
	case device::Transfer::Pos: return std::max(c, 0.0);
	case device::Transfer::Neg: return std::min(c, 0.0);
	default: return 0.0;
	}
}
} // namespace

TEST_CASE("transfer chips, rack: a route's curve shapes its source BEFORE the amount, on the collapsed row and on each voice's") {
	namespace nx = mod::noteexpr;
	rack::RackEngine rack(kAxes, hostCatalogue());
	rack.setVoices(2);
	rack.prepare(48000.0, 4096);
	const size_t nxSlot = rack.addDevice(nx::kTraits.tag);
	const size_t fSlot = rack.addDevice(filter::kTraits.tag);
	REQUIRE(nxSlot != rack::RackEngine::kNoSlot);
	REQUIRE(fSlot != rack::RackEngine::kNoSlot);
	const size_t cutoff = controlNamed(filter::kDescriptor, "Cutoff");
	rack.setField(fSlot, static_cast<size_t>(filter::Param::Poly), 1.0f);
	const device::ParamDesc& p = filter::kDescriptor.params[cutoff];
	// A BASE IN THE MIDDLE, so a reach either way has room before the range stops it.
	const float base = static_cast<float>(0.5 * (p.min + p.max));
	rack.setField(fSlot, cutoff, base);
	const double span = device::routeSpanOf(p);
	const backplane::Source src = test::outputOf(rack, nxSlot, 0);
	const backplane::Destination dst = test::controlOf(rack, fSlot, cutoff);
	constexpr float kAmount = 0.25f;
	rack.setRouteAmount(src, dst, kAmount);
	REQUIRE(rack.routeAmount(src, dst) == kAmount);

	// TWO VOICES ON EITHER SIDE OF ZERO — the only readings that tell a rectifier from Lin.
	constexpr float kVoice0 = -0.5f;
	constexpr float kVoice1 = 0.75f;
	rack.writeOutputForVoice(test::idAt(rack, nxSlot), 0, 0, kVoice0);
	rack.writeOutputForVoice(test::idAt(rack, nxSlot), 0, 1, kVoice1);
	const int n = 64;
	std::vector<float> l = tone(n), r = tone(n);

	const auto expectedAt = [&](device::Transfer t, double reading) {
		const double offset = static_cast<double>(kAmount) * restatedCurve(t, reading) * span;
		return offset != 0.0 ? static_cast<float>(device::withRouteOffset(p, static_cast<double>(base), offset)) : base;
	};
	for (const device::Transfer t : kEveryTransfer) {
		CAPTURE(std::string(device::transferName(t)));
		CAPTURE(device::transferStep(t));
		REQUIRE(rack.setRouteTransfer(src, dst, t));
		rack.processStereo(l.data(), r.data(), n, backplane::unknownBlock());
		CHECK(rack.resolvedFieldForVoiceForTest(fSlot, cutoff, 0) == doctest::Approx(expectedAt(t, kVoice0)).epsilon(1e-6));
		CHECK(rack.resolvedFieldForVoiceForTest(fSlot, cutoff, 1) == doctest::Approx(expectedAt(t, kVoice1)).epsilon(1e-6));
		const float collapsedCell = *rack.outputCellOf(test::idAt(rack, nxSlot), 0);
		CHECK(rack.resolvedFieldForTest(fSlot, cutoff) == doctest::Approx(expectedAt(t, collapsedCell)).epsilon(1e-6));
	}
	// NOT VACUOUS: the curves disagree at the readings chosen — Lin pulls voice 0 down, Rectify pushes it
	// up, Pos half leaves it at the base.
	CHECK(expectedAt(device::Transfer::Lin, kVoice0) < base);
	CHECK(expectedAt(device::Transfer::Abs, kVoice0) > base);
	CHECK(expectedAt(device::Transfer::Pos, kVoice0) == base);

	SUBCASE("Lin is today's expression, to the bit") {
		REQUIRE(rack.setRouteTransfer(src, dst, device::Transfer::Lin));
		rack.processStereo(l.data(), r.data(), n, backplane::unknownBlock());
		// THE OLD RESOLVE, VERBATIM: `amount × value × span` in double, then `withRouteOffset`.
		const double old = static_cast<double>(kAmount) * static_cast<double>(kVoice0) * span;
		CHECK(rack.resolvedFieldForVoiceForTest(fSlot, cutoff, 0)
		      == static_cast<float>(device::withRouteOffset(p, static_cast<double>(base), old)));
	}
	SUBCASE("a curve is refused on a pair with no route, and a route made fresh is Lin") {
		const backplane::Source other = test::macro(1);
		CHECK_FALSE(rack.setRouteTransfer(other, dst, device::Transfer::Exp));
		rack.setRouteAmount(other, dst, 0.1f);
		bool seen = false;
		rack.forEachRoute([&](const backplane::Route& route) {
			if (route.source == other && route.destination == dst) {
				seen = true;
				CHECK(route.transfer == device::Transfer::Lin);
			}
		});
		CHECK(seen);
	}
}

TEST_CASE("transfer chips, rack: the per-sample window reads each sample through the curve, and allocates nothing") {
	namespace bu = feathers::fx::busutil;
	namespace lfo = feathers::mod::lfo;
	SlotRig rig;
	constexpr DeviceInstanceId kLfo = 20;
	constexpr DeviceInstanceId kBus = 21;
	const size_t gain = static_cast<size_t>(bu::Param::Gain);
	const device::ParamDesc& pd = bu::kDescriptor.params[gain];
	const double span = device::routeSpanOf(pd);
	rack::ChainState s(kAxes.deviceSlots, 1);
	rack::DeviceState lfoRow = defaultRow(lfo::kTraits.tag, lfo::kDescriptor, kLfo);
	// A FREE-RUNNING, BIPOLAR, QUICK LFO: both halves, and moving without a note (Gate mode would sit at rest).
	lfoRow.field[static_cast<size_t>(lfo::Param::Bipolar)] = 1.0f;
	lfoRow.field[static_cast<size_t>(lfo::Param::Mode)] = static_cast<float>(mod::Mode::Free);
	lfoRow.field[static_cast<size_t>(lfo::Param::TimeScale)] = -8.0f;
	lfoRow.field[static_cast<size_t>(lfo::Param::Phase)] = 0.5f; // starting on the way DOWN through zero
	REQUIRE(s.append(0, lfoRow));
	REQUIRE(s.append(0, defaultRow(bu::kTraits.tag, bu::kDescriptor, kBus)));
	rig.publish(s);
	rig.drain();
	const size_t busLane = rig.laneOf(kBus);
	constexpr float kAmount = 0.25f;
	const backplane::Source src = backplane::Source::output(kLfo, 0);
	const backplane::Destination dst = backplane::Destination::control(kBus, gain);
	rig.engine.setRouteAmount(src, dst, kAmount);
	REQUIRE(rig.engine.routeAmount(src, dst) == kAmount);
	const float base = rig.engine.fieldAt(busLane, gain);

	// THE BLOCK'S BUFFERS, made before the counter opens — `SlotRig::block` makes its own, which would be counted.
	std::vector<float> blockL(64, 0.0f), blockR(64, 0.0f);
	// AND A BENT EXPONENT AND A φ CURVE (backlog PY): `pow` and the φ triangles allocate nothing either.
	for (const device::Transfer t : { device::Transfer::Lin, device::Transfer::Abs, device::Transfer::Exp,
	                                  device::transferWithStep(device::Transfer::Exp, 16),
	                                  device::phiTransfer(device::kPhiOctavesDefault) }) {
		CAPTURE(std::string(device::transferName(t)));
		REQUIRE(rig.engine.setRouteTransfer(src, dst, t));
		int moving = 0;
		int negative = 0;
		for (int tick = 0; tick < 256 && moving < 64; ++tick) {
			uint64_t allocs = 0;
			{
				AllocWindow w;
				rig.engine.processStereo(blockL.data(), blockR.data(), 64, backplane::unknownBlock());
				allocs = w.allocs();
			}
			CHECK(allocs == 0);
			const float* lfoWindow = rig.engine.outputWindowOf(kLfo, 0);
			const float* win = rig.engine.controlWindowForTest(busLane, gain);
			REQUIRE(lfoWindow != nullptr);
			REQUIRE(win != nullptr);
			if (device::tickWindowFlat(win)) {
				continue;
			}
			++moving;
			for (int i = 0; i < device::kTickWindow; ++i) {
				negative += lfoWindow[i] < 0.0f ? 1 : 0;
				// RESTATED, in `resolveWindowInto`'s order; Lin's is the expression it always had.
				const double reading = t == device::Transfer::Lin
				                           ? static_cast<double>(lfoWindow[i])
				                           : restatedCurve(t, static_cast<double>(lfoWindow[i]));
				const double offset = static_cast<double>(kAmount) * reading * span;
				const float expected =
				    offset != 0.0 ? static_cast<float>(device::withRouteOffset(pd, static_cast<double>(base), offset)) : base;
				if (t == device::Transfer::Lin) {
					CHECK(win[i] == expected);
				}
				else {
					CHECK(win[i] == doctest::Approx(expected).epsilon(1e-6));
				}
			}
		}
		REQUIRE(moving >= 1); // or the loop asserted nothing
		CHECK(negative > 0);  // and the LFO said something a rectifier changes
	}
}

//------------------------------------------------------------------------------------------------//
// UNBOUNDED DEVICES, STEP 4 — THE ENGINE GROWS PAST ITS AXES                                     //
//------------------------------------------------------------------------------------------------//

TEST_CASE("unbounded devices step 4: the engine grows past its axes at a drain, moves nothing, and the drain "
          "and the block allocate nothing") {
	constexpr backplane::Axes kTwo {
		.coreSources = 8,
		.coreDestinations = 0,
		.deviceSlots = 2,
		.outputStride = 4,
	};
	rack::RackEngine engine { kTwo, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	REQUIRE(engine.addDevice(filter::kTraits.tag) == 0);
	REQUIRE(engine.addDevice(filter::kTraits.tag) == 1);
	REQUIRE(engine.capacity() == 2);
	// FULL: a third has nowhere to go until the engine grows. (The provision comes back whole and is freed here.)
	CHECK(engine.adoptDevice(engine.chainLength(), engine.provision(filter::kTraits.tag)) == rack::RackEngine::kNoSlot);
	const rack::Slot* first = &engine.slotAtLane(0);
	const DeviceInstanceId firstId = engine.slotAtLane(0).id;

	// THE MAIN THREAD'S HALF: the pools grow now, the room to PLACE arrives with the next drain.
	constexpr size_t kWant = 64;
	REQUIRE(engine.requestCapacity(kWant));
	CHECK(engine.capacity() == 2);
	CHECK(engine.requestedCapacity() >= kWant);
	CHECK_FALSE(engine.reapGrowth()); // nothing to free until a drain has taken it

	// THE DRAIN'S HALF, AND EVERY ADD AFTER IT: provisions are built first, where allocation is allowed, and
	// only the adoptions run inside the window.
	std::vector<rack::RackEngine::Provision> built;
	for (size_t i = 2; i < kWant; ++i) {
		built.push_back(engine.provision(filter::kTraits.tag));
		REQUIRE(built.back().live());
	}
	const int n = 256;
	std::vector<float> l = tone(n), r = tone(n);
	uint64_t allocs = 0;
	size_t placed = 0;
	{
		AllocWindow w;
		engine.adoptGrowth();
		for (rack::RackEngine::Provision& p : built) {
			placed += engine.adoptDevice(engine.chainLength(), std::move(p)) != rack::RackEngine::kNoSlot ? 1 : 0;
		}
		engine.processStereo(l.data(), r.data(), n, backplane::unknownBlock());
		allocs = w.allocs();
	}
	CHECK(allocs == 0);
	CHECK(placed == kWant - 2);
	CHECK(engine.capacity() >= kWant);
	CHECK(engine.chainLength() == kWant);
	// NOTHING MOVED: the first device is where it was, under the id it had — an input pointer into its outputs
	// taken before the growth would still be pointing at it.
	CHECK(&engine.slotAtLane(0) == first);
	CHECK(engine.slotAtLane(0).id == firstId);
	// AND EVERY DEVICE RAN: the order the block walks names all of them.
	for (size_t position = 0; position < kWant; ++position) {
		CAPTURE(position);
		CHECK(engine.deviceAtPosition(position) != rack::RackEngine::kNoSlot);
	}

	// THE OLD GRAPH GOES BACK TO THE MAIN THREAD, once.
	CHECK(engine.reapGrowth());
	CHECK_FALSE(engine.reapGrowth());
	// AND THE ROOM IS HELD: a request for less is answered and builds nothing.
	CHECK(engine.requestCapacity(8));
	CHECK_FALSE(engine.reapGrowth());
}

TEST_CASE("unbounded devices step 7: the rack grows to sixty-four devices through its bridge, and every drain and "
          "block allocates nothing") {
	// THE RACK PLUGIN'S OWN ROAD (and every nested rack's): the record grows past the axes it was built with, the
	// bridge publishes the bigger state, offers the drain bigger states, and the converger grows the engine.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);
	bridge.primeForTest();
	constexpr size_t kDevices = 64;
	REQUIRE(kAxes.deviceSlots < kDevices);
	const int n = 256;
	std::vector<float> l = tone(n), r = tone(n);
	uint64_t worst = 0;
	for (size_t i = 0; i < kDevices; ++i) {
		CAPTURE(i);
		REQUIRE(bridge.addDevice(filter::kTraits.tag) != kNoInstance);
		// TWO DRAINS PER ADD at most: one may take only the room, the next the device.
		for (int drain = 0; drain < 2; ++drain) {
			AllocWindow w;
			bridge.drain();
			engine.processStereo(l.data(), r.data(), n, backplane::unknownBlock());
			worst = std::max(worst, w.allocs());
		}
		bridge.reapRetired();
	}
	CHECK(worst == 0);
	CHECK(bridge.chainLength(0) == kDevices);
	CHECK(engine.chainLength() == kDevices);
	CHECK(engine.capacity() >= kDevices);
	for (size_t position = 0; position < kDevices; ++position) {
		CAPTURE(position);
		CHECK(engine.deviceAtPosition(position) != rack::RackEngine::kNoSlot);
	}
}

TEST_CASE("backlog OJ: a rack routes 433 times through its bridge, every store grows on the message thread, every drain "
          "and block allocates nothing, and the route that grew the list is heard on the next block") {
	// THE RACK'S OWN ROAD (and every nested rack's): the record's graph grows its edges, the published list grows
	// and, through its growth hook, grows the drain's stage and the engine's route stores BEFORE it publishes the
	// count that needs them. The engine starts with room for 264 (4 slots); the list is filled to exactly one of
	// its capacities (432) past that, and the hand's next route is the one that makes the list grow again.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.prepare(48000.0, 4096);
	rack::RackBridge bridge(engine);
	bridge.primeForTest();
	const int n = 256;
	std::vector<float> l = tone(n), r = tone(n);
	uint64_t worst = 0;
	const auto block = [&] {
		AllocWindow w;
		bridge.drain();
		engine.processStereo(l.data(), r.data(), n, backplane::unknownBlock());
		worst = std::max(worst, w.allocs());
	};
	constexpr size_t kDevices = 20;
	std::vector<DeviceInstanceId> ids;
	for (size_t i = 0; i < kDevices; ++i) {
		ids.push_back(bridge.addDevice(filter::kTraits.tag));
		REQUIRE(ids.back() != kNoInstance);
		block();
		block();
		bridge.reapRetired();
	}
	const device::Descriptor& desc = filter::kDescriptor;
	const size_t cutoff = controlNamed(desc, "Cutoff");
	REQUIRE(desc.params[cutoff].modulatable);
	constexpr size_t kFill = 432;
	REQUIRE(kFill > engine.routeRoom()); // past the engine's starting room
	// MACROS 1-7 INTO EVERY MODULATABLE CONTROL, device by device, until the list stands exactly at its capacity.
	std::vector<backplane::Route> made;
	for (size_t m = 0; m < 7 && made.size() < kFill; ++m) {
		for (const DeviceInstanceId id : ids) {
			for (size_t k = 0; k < desc.modulatableCount() && made.size() < kFill; ++k) {
				const backplane::Destination d = backplane::Destination::control(id, desc.controlOfNthModulatable(k));
				const float amount = 0.001f * static_cast<float>(1 + made.size());
				bridge.setRouteAmount(test::macro(m), d, amount);
				made.push_back({ test::macro(m), d, amount });
				if (made.size() % 16 == 0) {
					block();
				}
			}
		}
	}
	REQUIRE(made.size() == kFill);
	block();
	CHECK(bridge.routeListForTest().count() == kFill);
	REQUIRE(bridge.routeListForTest().capacity() == kFill); // the next route must grow it
	CHECK(engine.routeRoomRefusals() == 0);
	for (const backplane::Route& route : made) {
		CHECK(engine.routeAmount(route.source, route.destination) == route.amount); // the engine's own copy, by value
	}

	// THE HAND'S NEXT ROUTE — macro 8 into the last filter's Cutoff, which nothing else reaches from macro 8.
	const DeviceInstanceId last = ids.back();
	const size_t slot = engine.slotOf(last);
	REQUIRE(slot != rack::RackEngine::kNoSlot);
	bridge.setMacro(7, 1.0f);
	block();
	const float before = engine.resolvedFieldForTest(slot, cutoff);
	const size_t growths = bridge.routeListForTest().growthsForTest();
	const backplane::Destination toCutoff = backplane::Destination::control(last, cutoff);
	bridge.setRouteAmount(test::macro(7), toCutoff, 0.5f);
	CHECK(bridge.routeListForTest().growthsForTest() == growths + 1);
	block(); // THE NEXT BLOCK
	CHECK(engine.routeRoomRefusals() == 0);
	CHECK(engine.routeAmount(test::macro(7), toCutoff) == 0.5f);
	const device::ParamDesc& p = desc.params[cutoff];
	const double expected = static_cast<double>(before) + 0.5 * device::routeSpanOf(p);
	CHECK(engine.resolvedFieldForTest(slot, cutoff)
	      == doctest::Approx(std::min(expected, p.offsetsMayLeaveRange ? expected : p.max)));
	CHECK(engine.resolvedFieldForTest(slot, cutoff) != doctest::Approx(before));
	CHECK(worst == 0);
}

TEST_CASE("the engine hands the sidechain to a listening device at each piece's offset into the host block") {
	// THE VOCODER'S PLUMBING (design §10.1): a host block of 300 frames runs as pieces of the 64-frame tick window, and every
	// piece must hear the sidechain's OWN frames. A ramp makes the offset a value: the device's ring holds the ramp in order.
	rack::RackEngine rack(kAxes, hostCatalogue());
	const size_t slot = rack.addDevice(feathers::fx::spectral::kTraits.tag);
	REQUIRE(slot != rack::RackEngine::kNoSlot);
	rack.prepare(48000.0, 4096);
	constexpr int n = 300;
	std::vector<float> l(n, 0.0f), r(n, 0.0f), sideL(n), sideR(n);
	for (int i = 0; i < n; ++i) {
		sideL[static_cast<size_t>(i)] = static_cast<float>(i + 1);
		sideR[static_cast<size_t>(i)] = static_cast<float>(i + 1);
	}
	rack.setSidechain(sideL.data(), sideR.data(), true);
	rack.processStereo(l.data(), r.data(), n, backplane::unknownBlock());
	const auto* device = static_cast<const feathers::fx::spectral::SpectralDevice*>(rack.storageForTest(slot));
	REQUIRE(device != nullptr);
	size_t bad = 0;
	for (int ago = 0; ago < n; ++ago) {
		bad += device->stftForTest().sidechainAgoForTest(ago) != static_cast<float>(n - ago);
	}
	CHECK(bad == 0);
	CHECK(device->stftForTest().sidechainSounding());
	// AND THE POINTERS DIE WITH THE BLOCK: a block with no sidechain set writes silence, not the old buffers.
	rack.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
	CHECK(device->stftForTest().sidechainAgoForTest(0) == 0.0f);
}

TEST_CASE("a Chain hands the sidechain to the device inside it at each of ITS chunks' offsets, and drops it with the block") {
	// THE CONTAINER CUTS ITS OWN CHUNKS on its inner control tick. Each must hear its own frames of the sidechain, not the call's
	// first ones again — the ramp makes the offset a value, as in the case above.
	NestedRig rig;
	rig.chain = rig.addOuter(feathers::composite::kTraits.tag);
	REQUIRE(rig.chain != feathers::kNoInstance);
	const size_t lane = rig.outer.slotOf(rig.chain);
	REQUIRE(lane != rack::RackEngine::kNoSlot);
	rig.inner = feathers::rack::bridgeOf(rig.outer.slotAtLane(lane).storage.get());
	REQUIRE(rig.inner != nullptr);
	const feathers::DeviceInstanceId id = rig.inner->addDevice(feathers::fx::spectral::kTraits.tag);
	REQUIRE(id != feathers::kNoInstance);
	for (int b = 0; b < 4; ++b) {
		rig.block(); // the inner drain seats it
	}
	const rack::RackEngine& inner = rig.inner->engineForTest();
	const size_t slot = inner.slotOf(id);
	REQUIRE(slot != rack::RackEngine::kNoSlot);
	constexpr int n = 300;
	std::vector<float> l(n, 0.0f), r(n, 0.0f), sideL(n), sideR(n);
	for (int i = 0; i < n; ++i) {
		sideL[static_cast<size_t>(i)] = static_cast<float>(i + 1);
		sideR[static_cast<size_t>(i)] = static_cast<float>(i + 1);
	}
	rig.outer.setSidechain(sideL.data(), sideR.data(), true);
	rig.outer.processStereo(l.data(), r.data(), n, backplane::unknownBlock());
	const auto* device = static_cast<const feathers::fx::spectral::SpectralDevice*>(inner.storageForTest(slot));
	REQUIRE(device != nullptr);
	size_t bad = 0;
	for (int ago = 0; ago < n; ++ago) {
		bad += device->stftForTest().sidechainAgoForTest(ago) != static_cast<float>(n - ago);
	}
	CHECK(bad == 0);
	CHECK(device->stftForTest().sidechainSounding());
	// AND THE POINTERS DIE WITH THE BLOCK: the next block, with none set, writes silence.
	rig.outer.setSidechain(nullptr, nullptr, false);
	rig.block();
	CHECK(device->stftForTest().sidechainAgoForTest(0) == 0.0f);
}

TEST_CASE("JO: a compressor with Detector Side, on the rack's bus road, ducks the input by what the SIDECHAIN needs") {
	// INPUT -30 dBFS DC (under the -18 dB threshold) and SIDECHAIN -6 dBFS DC: the gain is the static curve's 9 dB at 4:1, read as a
	// value off the device, and it exists only if the engine handed the sidechain to the compressor's detector.
	namespace cp = feathers::fx::comp;
	rack::RackEngine rack(kAxes, hostCatalogue());
	const size_t slot = rack.addDevice(cp::kTraits.tag);
	REQUIRE(slot != rack::RackEngine::kNoSlot);
	rack.prepare(48000.0, 4096);
	rack.setField(slot, static_cast<size_t>(cp::Param::Detector), static_cast<float>(cp::Detector::Side));
	rack.setField(slot, static_cast<size_t>(cp::Param::Knee), 0.0f);
	rack.setField(slot, static_cast<size_t>(cp::Param::Attack), static_cast<float>(std::log2(cp::kAttackMinSeconds)));
	constexpr int n = 512;
	const float in = std::pow(10.0f, -30.0f / 20.0f), side = std::pow(10.0f, -6.0f / 20.0f);
	std::vector<float> l(n), r(n), sideL(n, side), sideR(n, side);
	for (int b = 0; b < 200; ++b) {
		std::fill(l.begin(), l.end(), in);
		std::fill(r.begin(), r.end(), in);
		rack.setSidechain(sideL.data(), sideR.data(), true);
		rack.processStereo(l.data(), r.data(), n, backplane::unknownBlock());
	}
	CHECK(cp::gainDbForTest(rack.storageForTest(slot)) == doctest::Approx(-9.0f).epsilon(1e-3));
	// WITH NONE SET, THE NEXT BLOCKS LET GO: the pointers died with the block, and the detector hears silence.
	for (int b = 0; b < 200; ++b) {
		std::fill(l.begin(), l.end(), in);
		std::fill(r.begin(), r.end(), in);
		rack.setSidechain(nullptr, nullptr, false);
		rack.processStereo(l.data(), r.data(), n, backplane::unknownBlock());
	}
	CHECK(cp::gainDbForTest(rack.storageForTest(slot)) > -0.01f);
}

TEST_CASE("JO: a compressor with Detector Side inside a Chain hears the sidechain too") {
	namespace cp = feathers::fx::comp;
	NestedRig rig;
	rig.chain = rig.addOuter(feathers::composite::kTraits.tag);
	REQUIRE(rig.chain != feathers::kNoInstance);
	const size_t lane = rig.outer.slotOf(rig.chain);
	REQUIRE(lane != rack::RackEngine::kNoSlot);
	rig.inner = feathers::rack::bridgeOf(rig.outer.slotAtLane(lane).storage.get());
	REQUIRE(rig.inner != nullptr);
	const feathers::DeviceInstanceId id = rig.inner->addDevice(cp::kTraits.tag);
	REQUIRE(id != feathers::kNoInstance);
	for (int b = 0; b < 4; ++b) {
		rig.block(); // the inner drain seats it
	}
	rig.inner->setField(id, static_cast<size_t>(cp::Param::Detector), static_cast<float>(cp::Detector::Side));
	rig.inner->setField(id, static_cast<size_t>(cp::Param::Knee), 0.0f);
	rig.inner->setField(id, static_cast<size_t>(cp::Param::Attack), static_cast<float>(std::log2(cp::kAttackMinSeconds)));
	const rack::RackEngine& inner = rig.inner->engineForTest();
	const size_t slot = inner.slotOf(id);
	REQUIRE(slot != rack::RackEngine::kNoSlot);
	constexpr int n = 512;
	const float in = std::pow(10.0f, -30.0f / 20.0f), side = std::pow(10.0f, -6.0f / 20.0f);
	std::vector<float> l(n), r(n), sideL(n, side), sideR(n, side);
	for (int b = 0; b < 200; ++b) {
		std::fill(l.begin(), l.end(), in);
		std::fill(r.begin(), r.end(), in);
		rig.outer.setSidechain(sideL.data(), sideR.data(), true);
		rig.outer.processStereo(l.data(), r.data(), n, backplane::unknownBlock());
	}
	CHECK(cp::gainDbForTest(inner.storageForTest(slot)) == doctest::Approx(-9.0f).epsilon(1e-3));
}

//------------------------------------------------------------------------------------------------//
// BACKLOG OM — THE TABLE SHAPER'S POLY SWITCH                                                       //
//------------------------------------------------------------------------------------------------//

namespace {

namespace shp = feathers::fx::shaper;

/// THE ONE SCRIPT BOTH OM VALUE CASES RUN: two voices, a note-expression device whose per-voice output 0 is written
/// by hand (1 for voice 0, 0 for voice 1), routed at 0.5 into the shaper's Drive, one block through the host's two
/// passes. Returns the two voice instances' Drive as each instance HOLDS it (`Shaper::targetForTest`), or -1 for an
/// instance that does not exist.
struct OmVoices {
	float drive0 = -1.0f;
	float drive1 = -1.0f;
	float busDrive = -1.0f;
	float collapsed = -1.0f;
	bool built = false;
	bool poly = false;
	float expected0 = 0.0f;
	float base = 0.0f;
};

OmVoices omRunTwoVoices(bool polyOn) {
	namespace nx = mod::noteexpr;
	rack::RackEngine rack(kAxes, hostCatalogue());
	rack.setVoices(2);
	rack.prepare(48000.0, 4096);
	const size_t nxSlot = rack.addDevice(nx::kTraits.tag);
	const size_t sSlot = rack.addDevice(shp::kTraits.tag);
	REQUIRE(nxSlot != rack::RackEngine::kNoSlot);
	REQUIRE(sSlot != rack::RackEngine::kNoSlot);
	const auto drive = static_cast<size_t>(shp::Param::Drive);
	rack.setField(sSlot, static_cast<size_t>(shp::Param::Poly), polyOn ? 1.0f : 0.0f);
	rack.setRouteAmount(test::outputOf(rack, nxSlot, 0), test::controlOf(rack, sSlot, drive), 0.5f);
	rack.writeOutputForVoice(test::idAt(rack, nxSlot), 0, 0, 1.0f);
	rack.writeOutputForVoice(test::idAt(rack, nxSlot), 0, 1, 0.0f);
	const int n = 64;
	rack.beginBlock(n, backplane::unknownBlock());
	for (size_t v = 0; v < 2; ++v) {
		std::vector<float> l = tone(n), r = tone(n);
		rack.processVoice(v, l.data(), r.data(), n, device::NoteContext { 48.0f + static_cast<float>(v), true });
	}
	std::vector<float> l = tone(n), r = tone(n);
	rack.processBus(l.data(), r.data(), n);
	rack.endBlock();
	OmVoices out;
	out.poly = rack.polyForTest(sSlot);
	out.base = rack.fieldAt(sSlot, drive);
	const device::ParamDesc& p = shp::kDescriptor.params[drive];
	out.expected0 = static_cast<float>(std::min(static_cast<double>(out.base) + 0.5 * device::routeSpanOf(p), p.max));
	if (const auto* v0 = static_cast<const shp::Shaper*>(rack.voiceStorageForTest(sSlot, 0))) {
		out.drive0 = v0->targetForTest().drive;
	}
	if (const auto* v1 = static_cast<const shp::Shaper*>(rack.voiceStorageForTest(sSlot, 1))) {
		out.drive1 = v1->targetForTest().drive;
	}
	out.busDrive = static_cast<const shp::Shaper*>(rack.storageForTest(sSlot))->targetForTest().drive;
	out.collapsed = rack.resolvedFieldForTest(sSlot, drive);
	out.built = rack.voiceStorageForTest(sSlot, 0) != nullptr && rack.voiceStorageForTest(sSlot, 1) != nullptr;
	return out;
}

} // namespace

TEST_CASE("OM: under Poly each voice's table shaper holds its OWN Drive, from that voice's modulation") {
	const OmVoices on = omRunTwoVoices(true);
	REQUIRE(on.poly);
	// THE VALUES, each voice's instance read: voice 0 under the full per-voice cell opens by amount x span, voice 1
	// under none sits at the base. An independent restatement of the route (base + 0.5 x span, clamped).
	CHECK(on.drive0 == doctest::Approx(on.expected0));
	CHECK(on.drive1 == doctest::Approx(on.base));
	// NOT VACUOUS: the two states differ.
	CHECK(on.drive0 != doctest::Approx(on.drive1));
}

TEST_CASE("OM: with Poly off (the default) the shaper runs once on the bus and no voice holds a state of its own") {
	REQUIRE(shp::kParamDescs[static_cast<size_t>(shp::Param::Poly)].def == 0.0);
	const OmVoices off = omRunTwoVoices(false);
	CHECK_FALSE(off.poly);
	// THE VOICE INSTANCES EXIST (a direct `addDevice` provisions eagerly), so the claim below is about real
	// instances: neither was fed, and the two voices hold IDENTICAL state. The one instance that runs is the bus's,
	// holding the collapsed row — the device's own block-rate cell, not either voice's.
	REQUIRE(off.built);
	CHECK(off.drive0 == off.drive1);
	CHECK(off.busDrive == doctest::Approx(off.collapsed));
}

// PLATFORM-PINNED (Linux x86-64 only): the pinned hash was measured there; libm and float contraction differ on Apple arm64/x86,
// so the bits do too. Fold, render helper and case are guarded together so macOS has no unused function.
#if defined(__x86_64__) && defined(__linux__)
namespace {

/// FNV-1a over the bits of a stereo render.
uint64_t omFold(uint64_t h, const std::vector<float>& v) {
	for (const float x : v) {
		uint32_t bits = 0;
		std::memcpy(&bits, &x, sizeof bits);
		for (int b = 0; b < 4; ++b) {
			h ^= (bits >> (8 * b)) & 0xffu;
			h *= 1099511628211ull;
		}
	}
	return h;
}

/// THE BUS RENDER OM PINS: a shaper well away from its defaults, in a voiceless rack, 48 blocks of a 220 Hz tone at
/// two levels. Every field is set by KEY's position at today's indices, none of which OM moved.
uint64_t omPinnedRender() {
	rack::RackEngine rack(kAxes, hostCatalogue());
	rack.prepare(48000.0, 4096);
	const size_t s = rack.addDevice(shp::kTraits.tag);
	REQUIRE(s != rack::RackEngine::kNoSlot);
	const auto set = [&](shp::Param p, float v) { rack.setField(s, static_cast<size_t>(p), v); };
	set(shp::Param::Knee, 0.8f);
	set(shp::Param::Colour, 1.3f);
	set(shp::Param::Drive, 0.6f);
	set(shp::Param::Emphasis, 6.0f);
	set(shp::Param::SubOn, 1.0f);
	set(shp::Param::FeedbackOn, 1.0f);
	set(shp::Param::LowpassOn, 1.0f);
	set(shp::Param::Bloom, 0.5f);
	set(shp::Param::Trim, 3.0f);
	uint64_t h = 1469598103934665603ull;
	for (int block = 0; block < 48; ++block) {
		std::vector<float> l = tone(256), r = tone(256);
		for (float& x : r) {
			x *= 1.7f;
		}
		rack.processStereo(l.data(), r.data(), 256, backplane::unknownBlock());
		h = omFold(omFold(h, l), r);
	}
	return h;
}

} // namespace

TEST_CASE("OM: Poly off is the shaper as it was, bit for bit — the bus render's hash is the one pinned before Poly existed") {
	// PINNED at 03005405 (before OM), by the same render compiled against that commit's sources: Poly off must leave
	// every sample's bits where they were.
	constexpr uint64_t kPinned = 1379927704356371911ull;
	const uint64_t h = omPinnedRender();
	MESSAGE("OM pinned render hash 0x" << std::hex << h);
	CHECK(h == kPinned);
}
#endif

//------------------------------------------------------------------------------------------------//
// BACKLOG KW — THE ENABLE AS A MODULATION DESTINATION: A GATE, A CROSSFADE, PARKED WHILE OFF        //
//------------------------------------------------------------------------------------------------//

namespace {

/// One block of `frames` through the engine's bus pass, `in` going in, the output coming back. `allocs`, when given,
/// counts what the block itself allocated (the copies around it are the test's own).
std::vector<float> kwBlock(rack::RackEngine& rack, const std::vector<float>& in, int frames, uint64_t* allocs = nullptr) {
	std::vector<float> l = in, r = in;
	{
		const AllocWindow window;
		rack.beginBlock(frames, backplane::unknownBlock());
		rack.processBus(l.data(), r.data(), frames);
		rack.endBlock();
		if (allocs != nullptr) {
			*allocs += window.allocs();
		}
	}
	return l;
}

std::vector<float> kwTone(int n, int offset) {
	std::vector<float> v(static_cast<size_t>(n));
	for (int i = 0; i < n; ++i) {
		v[static_cast<size_t>(i)] = 0.5f * std::sin(6.2831853f * 220.0f * static_cast<float>(i + offset) / 48000.0f);
	}
	return v;
}

constexpr int kKwRamp = 240; // the furniture's 5 ms at 48 kHz (`furnitureRampFor`)

} // namespace

TEST_CASE("KW: the enable is a destination read as a gate at one half — a macro routed into it switches the device, which is parked (not run) while off") {
	rack::RackEngine rack(kAxes, hostCatalogue());
	rack.prepare(48000.0, 4096);
	const size_t f = rack.addDevice(filter::kTraits.tag);
	REQUIRE(f != rack::RackEngine::kNoSlot);
	const auto en = static_cast<size_t>(filter::Param::Enabled);
	REQUIRE(filter::kDescriptor.params[en].modulatable);
	rack.setField(f, en, 0.0f);
	rack.setRouteAmount(test::macro(0), test::controlOf(rack, f, en), 1.0f);
	REQUIRE(rack.routeAmount(test::macro(0), test::controlOf(rack, f, en)) == 1.0f); // the store took it: a destination
	constexpr int n = 256;
	kwBlock(rack, kwTone(n, 0), n);
	// OFF, LOADED OFF: parked from the first block, never run.
	CHECK(rack.resolvedFieldForTest(f, en) == 0.0f);
	CHECK(rack.furnitureForTest(f)->parked);
	CHECK(rack.runsForTest(f) == 0);
	{
		// THE MACRO AT 0.7: the stepped resolve rounds 0 + 0.7 to the step 1 (`withRouteOffset`) — on; the device runs
		// and fades in. The blocks are the audio thread's half, and switching allocates nothing there.
		rack.setMacro(0, 0.7f);
		uint64_t allocs = 0;
		kwBlock(rack, kwTone(n, n), n, &allocs);
		CHECK(allocs == 0);
		CHECK(rack.resolvedFieldForTest(f, en) == 1.0f);
		CHECK(feathers::device::enableOn(rack.resolvedFieldForTest(f, en)));
		CHECK_FALSE(rack.furnitureForTest(f)->parked);
		CHECK(rack.runsForTest(f) > 0);
		CHECK(rack.furnitureForTest(f)->wet == 1.0f); // the 240-sample fade is done inside the 256
		// AND AT 0.3: off, faded, then parked — the run count stops.
		rack.setMacro(0, 0.3f);
		kwBlock(rack, kwTone(n, 2 * n), n, &allocs);
		kwBlock(rack, kwTone(n, 3 * n), n, &allocs);
		CHECK(allocs == 0);
		CHECK(rack.resolvedFieldForTest(f, en) == 0.0f); // 0.3 rounds to the step 0: off
		CHECK_FALSE(feathers::device::enableOn(rack.resolvedFieldForTest(f, en)));
		CHECK(rack.furnitureForTest(f)->parked);
		const uint64_t ran = rack.runsForTest(f);
		kwBlock(rack, kwTone(n, 4 * n), n);
		CHECK(rack.runsForTest(f) == ran);
	}
}

TEST_CASE("KW: an enable flip crossfades — the switched-off output is the device's own output blended to the input over 5 ms, then the input exactly") {
	// THE REFERENCE: the same filter left on, the same input — what the device was making. The switched engine must be
	// exactly that, blended linearly toward the dry, never a step from one to the other.
	const auto build = [] {
		auto rack = std::make_unique<rack::RackEngine>(kAxes, hostCatalogue());
		rack->prepare(48000.0, 4096);
		const size_t f = rack->addDevice(filter::kTraits.tag);
		REQUIRE(f != rack::RackEngine::kNoSlot);
		rack->setField(f, controlNamed(filter::kDescriptor, "Cutoff"), 8.0f); // a corner that audibly moves a 220 Hz tone
		return std::make_pair(std::move(rack), f);
	};
	auto [on, fOn] = build();
	auto [off, fOff] = build();
	constexpr int n = 512;
	const std::vector<float> warm = kwTone(n, 0);
	const std::vector<float> a0 = kwBlock(*on, warm, n);
	const std::vector<float> b0 = kwBlock(*off, warm, n);
	REQUIRE(a0 == b0); // identical before the flip
	off->setField(fOff, static_cast<size_t>(filter::Param::Enabled), 0.0f);
	const std::vector<float> in = kwTone(n, n);
	const std::vector<float> wetOut = kwBlock(*on, in, n);
	const std::vector<float> flipped = kwBlock(*off, in, n);
	double worst = 0.0;
	double differs = 0.0;
	for (int i = 0; i < n; ++i) {
		const auto k = static_cast<size_t>(i);
		const double t = std::min(1.0, static_cast<double>(i + 1) / kKwRamp); // the dry's share after this sample's step
		const double want = i + 1 >= kKwRamp ? static_cast<double>(in[k])
		                                     : static_cast<double>(wetOut[k]) * (1.0 - t) + static_cast<double>(in[k]) * t;
		worst = std::max(worst, std::abs(static_cast<double>(flipped[k]) - want));
		differs = std::max(differs, std::abs(static_cast<double>(wetOut[k]) - static_cast<double>(in[k])));
		if (i + 1 >= kKwRamp) {
			CHECK(flipped[k] == in[k]); // after the fade: the input, to the bit
		}
	}
	CAPTURE(worst);
	CHECK(worst < 1e-5);
	CHECK(differs > 0.05); // NOT VACUOUS: the device's output is not the input, so a hard cut would be a step
}

TEST_CASE("KW: a reverb switched off mid-tail is faded and parked — no tail, no SLEEP held — and switched on again it starts from silence, not the old tail") {
	namespace fv = feathers::fx::featherverb;
	rack::RackEngine rack(kAxes, hostCatalogue());
	rack.prepare(48000.0, 4096);
	const size_t v = rack.addDevice(fv::kTraits.tag);
	REQUIRE(v != rack::RackEngine::kNoSlot);
	rack.setField(v, static_cast<size_t>(fv::Param::Mix), 1.0f);
	constexpr int n = 512;
	const std::vector<float> silence(static_cast<size_t>(n), 0.0f);
	kwBlock(rack, kwTone(n, 0), n); // a burst
	const std::vector<float> ringing = kwBlock(rack, silence, n);
	float tail = 0.0f;
	for (const float x : ringing) {
		tail = std::max(tail, std::fabs(x));
	}
	REQUIRE(tail > 1e-4f); // the tail rings
	REQUIRE(rack.holdsTail());
	rack.setField(v, static_cast<size_t>(fv::Param::Enabled), 0.0f);
	kwBlock(rack, silence, n); // the fade
	const std::vector<float> parked = kwBlock(rack, silence, n);
	CHECK(rack.furnitureForTest(v)->parked);
	CHECK(std::all_of(parked.begin(), parked.end(), [](float x) { return x == 0.0f; })); // EXACTLY silent: SLEEP may come
	CHECK_FALSE(rack.holdsTail()); // and nothing parked holds it awake
	// ON AGAIN, into silence: the reset device has nothing to replay.
	rack.setField(v, static_cast<size_t>(fv::Param::Enabled), 1.0f);
	const std::vector<float> back = kwBlock(rack, silence, n);
	CHECK_FALSE(rack.furnitureForTest(v)->parked);
	CHECK(std::all_of(back.begin(), back.end(), [](float x) { return x == 0.0f; }));
}

TEST_CASE("KW: under Poly each voice instance is switched by the one enable — faded, parked, and its output the voice's input") {
	rack::RackEngine rack(kAxes, hostCatalogue());
	rack.setVoices(2);
	rack.prepare(48000.0, 4096);
	const size_t f = rack.addDevice(filter::kTraits.tag);
	REQUIRE(f != rack::RackEngine::kNoSlot);
	rack.setField(f, static_cast<size_t>(filter::Param::Poly), 1.0f);
	rack.setField(f, controlNamed(filter::kDescriptor, "Cutoff"), 8.0f);
	constexpr int n = 512;
	const auto voiceBlock = [&](const std::vector<float>& in) {
		std::vector<float> l = in, r = in;
		rack.beginBlock(n, backplane::unknownBlock());
		rack.processVoice(0, l.data(), r.data(), n, device::NoteContext { 60.0f, true });
		std::vector<float> bl(static_cast<size_t>(n), 0.0f), br(static_cast<size_t>(n), 0.0f);
		rack.processBus(bl.data(), br.data(), n);
		rack.endBlock();
		return l;
	};
	const std::vector<float> first = voiceBlock(kwTone(n, 0));
	REQUIRE(rack.polyForTest(f));
	REQUIRE(first != kwTone(n, 0)); // on: the voice is filtered
	rack.setField(f, static_cast<size_t>(filter::Param::Enabled), 0.0f);
	voiceBlock(kwTone(n, n)); // the fade
	const std::vector<float> in = kwTone(n, 2 * n);
	CHECK(voiceBlock(in) == in); // parked: the voice's input, to the bit
}

//------------------------------------------------------------------------------------------------//
// BACKLOG OM, DECIDED 2026-10-30 — ONE TABLE FOR EVERY VOICE UNDER POLY                               //
//------------------------------------------------------------------------------------------------//

namespace {

/// A SHAPER INSTANCE THROUGH ITS HOST CONTRACT (`AudioOps`), as the engine builds one.
struct OmInstance {
	void* mem;
	explicit OmInstance(bool voice) {
		const device::AudioOps& ops = shp::kAudioOps;
		mem = ::operator new(ops.stateBytes, std::align_val_t { ops.stateAlign });
		ops.construct(mem);
		ops.prepare(mem, 48000.0, 4096);
		if (voice) {
			ops.setVoice(mem, 0);
		}
	}
	~OmInstance() {
		shp::kAudioOps.destruct(mem);
		::operator delete(mem, std::align_val_t { shp::kAudioOps.stateAlign });
	}
	shp::Shaper& shaper() { return *static_cast<shp::Shaper*>(mem); }
};

/// A row: the defaults, with the table's settings (Knee, Colour) and a per-sample one (Drive) set.
std::vector<float> omRow(float knee, float colour, float drive) {
	shp::Settings s;
	s.knee = knee;
	s.colour = colour;
	s.drive = drive;
	s.poly = true;
	const shp::Row row = shp::rowOf(s);
	std::vector<float> out(row.begin(), row.end());
	out.resize(row.size() + shp::phase_space::kPhaseCount, 0.0f);
	return out;
}

/// The table a lone shaper at `knee`/`colour` settles on, on the bus road: the reference a shared table must equal.
std::array<float, shp::kTableEntries> omSettledTable(float knee, float colour) {
	shp::Shaper ref;
	ref.prepare(48000.0);
	shp::Settings s;
	s.knee = knee;
	s.colour = colour;
	ref.setTarget(s);
	std::vector<float> l(256, 0.0f), r(256, 0.0f);
	for (int b = 0; b < 64; ++b) {
		ref.processStereo(l.data(), r.data(), 256);
	}
	REQUIRE(ref.rebuildsForTest() >= 1);
	REQUIRE_FALSE(ref.buildingForTest());
	return ref.tableForTest();
}

} // namespace

TEST_CASE("OM shared: under Poly two voices play ONE table, baked from the device's row — a voice's own Knee is not heard, its own Drive is") {
	const device::AudioOps& ops = shp::kAudioOps;
	OmInstance bus(false), v0(true), v1(true);
	const std::vector<float> busRow = omRow(0.7f, 1.2f, 0.0f);
	ops.beginSharedBlock(bus.mem, busRow.data(), busRow.size(), 256);
	// TWO VOICES WHOSE OWN ROWS DISAGREE about the table (Knee, Colour) and about Drive.
	const std::vector<float> row0 = omRow(0.1f, 0.3f, 0.2f);
	const std::vector<float> row1 = omRow(0.9f, 2.4f, 0.8f);
	ops.setFields(v0.mem, row0.data(), row0.size());
	ops.setFields(v1.mem, row1.data(), row1.size());
	ops.pullHandBlocks(v0.mem, bus.mem);
	ops.pullHandBlocks(v1.mem, bus.mem);
	std::vector<float> l(256), r(256);
	for (OmInstance* v : { &v0, &v1 }) {
		std::fill(l.begin(), l.end(), 0.3f);
		std::fill(r.begin(), r.end(), 0.3f);
		ops.processStereo(v->mem, l.data(), r.data(), 256);
	}
	// ONE BANK: both voices read the bus instance's, by identity, and own none.
	CHECK(v0.shaper().bankForTest() == bus.shaper().bankForTest());
	CHECK(v1.shaper().bankForTest() == bus.shaper().bankForTest());
	// THE TABLE IS THE DEVICE ROW'S: what a lone shaper at Knee 0.7 / Colour 1.2 settles on, to the bit.
	const auto want = omSettledTable(0.7f, 1.2f);
	CHECK(v0.shaper().tableForTest() == want);
	CHECK(v1.shaper().tableForTest() == want);
	// THE BAKED SETTINGS ARE THE BUS'S IN EACH VOICE; THE PER-SAMPLE ONES STAY EACH VOICE'S OWN.
	CHECK(v0.shaper().targetForTest().knee == 0.7f);
	CHECK(v1.shaper().targetForTest().knee == 0.7f);
	CHECK(v0.shaper().targetForTest().colour == 1.2f);
	CHECK(v1.shaper().targetForTest().colour == 1.2f);
	CHECK(v0.shaper().targetForTest().drive == 0.2f);
	CHECK(v1.shaper().targetForTest().drive == 0.8f);
	CHECK(v0.shaper().sweepForTest() != v1.shaper().sweepForTest()); // and Drive reached each voice's read
}

TEST_CASE("OM shared: a voice seated when Poly comes on plays the device's table from its FIRST block — never the identity") {
	rack::RackEngine rack(kAxes, hostCatalogue());
	rack.setVoices(2);
	rack.prepare(48000.0, 4096);
	const size_t s = rack.addDevice(shp::kTraits.tag);
	REQUIRE(s != rack::RackEngine::kNoSlot);
	rack.setField(s, static_cast<size_t>(shp::Param::Knee), 0.7f);
	rack.setField(s, static_cast<size_t>(shp::Param::Colour), 1.2f);
	// ON THE BUS FIRST, settled — then Poly comes on: the bus instance is reset (the engine's flip rule), so its bank
	// is the identity until something builds it.
	for (int b = 0; b < 8; ++b) {
		std::vector<float> l = tone(256), r = tone(256);
		rack.processStereo(l.data(), r.data(), 256, backplane::unknownBlock());
	}
	rack.setField(s, static_cast<size_t>(shp::Param::Poly), 1.0f);
	const int n = 256;
	rack.beginBlock(n, backplane::unknownBlock());
	REQUIRE(rack.polyForTest(s));
	std::vector<float> l = tone(n), r = tone(n);
	rack.processVoice(0, l.data(), r.data(), n, device::NoteContext { 60.0f, true });
	std::vector<float> bl(static_cast<size_t>(n), 0.0f), br(static_cast<size_t>(n), 0.0f);
	rack.processBus(bl.data(), br.data(), n);
	rack.endBlock();
	const auto* v0 = static_cast<const shp::Shaper*>(rack.voiceStorageForTest(s, 0));
	REQUIRE(v0 != nullptr);
	CHECK(v0->tableForTest() == omSettledTable(0.7f, 1.2f)); // the device's table, in the first Poly block
}

TEST_CASE("OM mono: under Poly a baked param modulated per voice moves the ONE table by the BLOCK's value for every voice, while Drive still moves per voice") {
	namespace nx = mod::noteexpr;
	rack::RackEngine rack(kAxes, hostCatalogue());
	rack.setVoices(2);
	rack.prepare(48000.0, 4096);
	const size_t nxSlot = rack.addDevice(nx::kTraits.tag);
	const size_t s = rack.addDevice(shp::kTraits.tag);
	REQUIRE(nxSlot != rack::RackEngine::kNoSlot);
	REQUIRE(s != rack::RackEngine::kNoSlot);
	const auto knee = static_cast<size_t>(shp::Param::Knee);
	const auto drive = static_cast<size_t>(shp::Param::Drive);
	rack.setField(s, static_cast<size_t>(shp::Param::Poly), 1.0f);
	rack.setField(s, knee, 0.3f);
	// A PER-VOICE SOURCE INTO BOTH: the baked Knee and the per-sample Drive; voice 0 none, voice 1 full (the block's own
	// cell is the device's rest, 1, so voice 0's resolve is the one that differs from the block's).
	rack.setRouteAmount(test::outputOf(rack, nxSlot, 0), test::controlOf(rack, s, knee), 0.5f);
	REQUIRE(rack.routeAmount(test::outputOf(rack, nxSlot, 0), test::controlOf(rack, s, knee)) == 0.5f); // a destination
	rack.setRouteAmount(test::outputOf(rack, nxSlot, 0), test::controlOf(rack, s, drive), 0.5f);
	rack.writeOutputForVoice(test::idAt(rack, nxSlot), 0, 0, 0.0f);
	rack.writeOutputForVoice(test::idAt(rack, nxSlot), 0, 1, 1.0f);
	const int n = 256;
	for (int b = 0; b < 12; ++b) { // past one build and its crossfade
		rack.beginBlock(n, backplane::unknownBlock());
		for (size_t v = 0; v < 2; ++v) {
			std::vector<float> l = tone(n), r = tone(n);
			rack.processVoice(v, l.data(), r.data(), n, device::NoteContext { 48.0f + static_cast<float>(v), true });
		}
		std::vector<float> bl(static_cast<size_t>(n), 0.0f), br(static_cast<size_t>(n), 0.0f);
		rack.processBus(bl.data(), br.data(), n);
		rack.endBlock();
	}
	REQUIRE(rack.polyForTest(s));
	const auto* v0 = static_cast<const shp::Shaper*>(rack.voiceStorageForTest(s, 0));
	const auto* v1 = static_cast<const shp::Shaper*>(rack.voiceStorageForTest(s, 1));
	REQUIRE(v0 != nullptr);
	REQUIRE(v1 != nullptr);
	// THE BLOCK'S KNEE — the collapsed row the bus is handed — in both voices, and NOT the voice's own resolve.
	const float block = rack.resolvedFieldForTest(s, knee);
	const float own0 = rack.resolvedFieldForVoiceForTest(s, knee, 0);
	REQUIRE(own0 != doctest::Approx(block)); // the per-voice resolve really differs, so the next lines say something
	CHECK(v0->targetForTest().knee == block);
	CHECK(v1->targetForTest().knee == block);
	// ONE TABLE, the block's: what a lone shaper at that Knee settles on.
	CHECK(v0->bankForTest() == v1->bankForTest());
	CHECK(v0->tableForTest() == omSettledTable(block, 0.0f));
	// AND DRIVE, UNBAKED, STAYS EACH VOICE'S OWN.
	CHECK(v0->targetForTest().drive == doctest::Approx(rack.resolvedFieldForVoiceForTest(s, drive, 0)));
	CHECK(v1->targetForTest().drive == doctest::Approx(rack.resolvedFieldForVoiceForTest(s, drive, 1)));
	CHECK(v0->targetForTest().drive != v1->targetForTest().drive);
}
