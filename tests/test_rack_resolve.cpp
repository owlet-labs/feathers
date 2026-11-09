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
#include "fx/filter/filter_types.h"
#include "fx/owltt/owltt_types.h"
#include "fx/sklitter/sklitter_types.h"
#include "mod/lfo/lfo_types.h"
#include "mod/arp/pattern_device.h"
#include "mod/arp/pattern_types.h"
#include "plugin/device_registry.h"
#include "rack/rack_axes.h"
#include "rack/rack_engine.h"
#include "persist_rig.h"
#include "rack_endpoints.h"

#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

// THE RACK RESOLVES FEATHERS' WAY — each control pulling its own inputs (`RackEngine::rebuildInputs`) through the
// span and landing rules `device::routeSpanOf` / `device::withRouteOffset`, the ones `ParamSet` uses.
// It had its own: a scan of macro cells plus a walk of the graph's edges, a span that ignored
// `modSpan`, and no rounding of a stepped control. These cases pin the shared rule as VALUES, and pin
// that the published list follows the graph through every mutation that can change it.

using namespace feathers;
using rack::RackEngine;

namespace {

size_t controlNamed(const device::Descriptor& desc, std::string_view name) {
	for (size_t i = 0; i < desc.params.size(); ++i) {
		if (name == desc.params[i].name) {
			return i;
		}
	}
	return desc.params.size();
}

/// The control, as an endpoint the engine takes — by the id the device at `lane` wears (kill-lanes
/// §2.4b). It computed a flat coordinate from the rack's axes; the descriptor argument stays so every
/// call site reads as "this device's control".
backplane::Destination destinationOf(const RackEngine& e, const device::Descriptor&, size_t lane, size_t control) {
	return test::controlOf(e, lane, control);
}

void block(RackEngine& e) {
	std::array<float, 64> l {};
	std::array<float, 64> r {};
	e.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
}

/// **HOW MANY INPUTS THE NODE AT `lane` HAS INTO `control`** — the pull machinery's own count, read from
/// the engine (`inputCountForTest`). It counted the compiled edge list's entries for a destination
/// coordinate; there is no list and no coordinate, and a node's inputs are the thing a resolve reads.
size_t inputsInto(const RackEngine& e, size_t lane, size_t control) {
	return e.inputCountForTest(lane, control);
}

} // namespace

TEST_CASE("a declared modSpan is what a route moves by in a rack — OWLTT's Character, one zone per unit") {
	// OWLTT's Character is a zone control over a drag window of ±2^23 with `modSpan = 1`: one unit of
	// route is one zone. The rack scaled by range / zones — 2^21 per unit — so a half-depth macro moved
	// it by about a million. `device::routeSpanOf` is the rule `ParamSet` has always applied.
	const device::Descriptor& owltt = fx::owltt::kDescriptor;
	const size_t character = controlNamed(owltt, "Character");
	REQUIRE(character < owltt.params.size());
	REQUIRE(owltt.params[character].modSpan == 1.0);
	REQUIRE(owltt.params[character].modulatable);

	RackEngine e { rack::kRackAxes, hostCatalogue() };
	e.prepare(48000.0, 4096);
	const size_t lane = e.addDevice(owltt.traits.tag);
	REQUIRE(lane != RackEngine::kNoSlot);
	const float base = e.fieldAt(lane, character);
	e.setRouteAmount(test::macro(0), destinationOf(e, owltt, lane, character), 0.5f);
	e.setMacro(0, 1.0f);
	block(e);
	CHECK(e.resolvedFieldForTest(lane, character) == doctest::Approx(base + 0.5f));
	// NON-VACUITY: the macro at zero leaves it at its base, so the half above is the route's.
	e.setMacro(0, 0.0f);
	block(e);
	CHECK(e.resolvedFieldForTest(lane, character) == base);
}

TEST_CASE("the published list follows the graph: a route written, a device removed, a lane reused") {
	const device::Descriptor& owltt = fx::owltt::kDescriptor;
	const size_t character = controlNamed(owltt, "Character");
	RackEngine e { rack::kRackAxes, hostCatalogue() };
	e.prepare(48000.0, 4096);
	const size_t lane = e.addDevice(owltt.traits.tag);
	REQUIRE(lane != RackEngine::kNoSlot);
	const backplane::Destination dest = destinationOf(e, owltt, lane, character);

	block(e);
	CHECK(inputsInto(e, lane, character) == 0);

	e.setRouteAmount(test::macro(0), dest, 0.25f);
	e.setRouteAmount(test::macro(3), dest, -0.5f);
	block(e);
	CHECK(inputsInto(e, lane, character) == 2);

	// AN AMOUNT MOVED TO ZERO IS NOT AN EDGE — and so not an input.
	e.setRouteAmount(test::macro(3), dest, 0.0f);
	block(e);
	CHECK(inputsInto(e, lane, character) == 1);

	// THE DEVICE LEAVES — the graph clears its routes, and the node's inputs go with the node: there is
	// no list left to count, which is the point.
	REQUIRE(e.removeDeviceAtLane(lane));
	block(e);
	e.reapRetired();
	CHECK(inputsInto(e, lane, character) == 0);

	// AND A NEW DEVICE ON THE SAME LANE ARRIVES WITH NOTHING ROUTED TO IT — the ABA guard, through the
	// list the resolve actually reads.
	const size_t again = e.addDevice(owltt.traits.tag);
	REQUIRE(again == lane);
	e.setMacro(0, 1.0f);
	block(e);
	CHECK(inputsInto(e, again, character) == 0);
	CHECK(e.resolvedFieldForTest(again, character) == e.fieldAt(again, character));
	(void)dest;
}

TEST_CASE("an envelope dragged below the device it modulates keeps its route — the list still reads it (backlog IZ)") {
	// THE HALF OF THE CASE BELOW THAT IZ CHANGED, kept as its own claim: the drag clears nothing, and the
	// resolve goes on reading the route through the list — the input is still there after the move.
	RackEngine e { rack::kRackAxes, hostCatalogue() };
	e.prepare(48000.0, 4096);
	const size_t env = e.addDevice("DHDS");
	const device::Descriptor& owltt = fx::owltt::kDescriptor;
	const size_t character = controlNamed(owltt, "Character");
	const size_t dotLane = e.addDevice(owltt.traits.tag);
	REQUIRE(env != RackEngine::kNoSlot);
	REQUIRE(dotLane != RackEngine::kNoSlot);
	const backplane::Source source = test::outputOf(e, env, 0);
	const backplane::Destination dest = destinationOf(e, owltt, dotLane, character);
	e.setRouteAmount(source, dest, 0.5f);
	block(e);
	REQUIRE(inputsInto(e, dotLane, character) == 1);
	const std::optional<size_t> cleared = e.moveDevice(e.positionOfLane(env), e.chainLength() - 1);
	REQUIRE(cleared.has_value());
	CHECK(*cleared == 0);
	block(e);
	CHECK(inputsInto(e, dotLane, character) == 1);
	CHECK(e.routeAmount(source, dest) == doctest::Approx(0.5f));
}

TEST_CASE("a device-to-device route is in the list, and a move that forbids it takes it out") {
	// A DEVICE UPSTREAM OF ANOTHER: the route is an edge in the graph and a coordinate in the list.
	// Dragging the source below the destination makes the route run backwards; the graph clears it, and
	// the list must follow or the resolve would go on reading a route that no longer exists.
	//
	// **TWO SKLITTERs SINCE BACKLOG IZ.** An envelope into an OWLTT was the fixture, and no drag can
	// forbid that any more — the envelope ticks before the OWLTT runs wherever the two stand. What a drag
	// forbids is a route between two devices that are both ticked and run.
	RackEngine e { rack::kRackAxes, hostCatalogue() };
	e.prepare(48000.0, 4096);
	const size_t env = e.addDevice("SKLITTER");
	const device::Descriptor& owltt = fx::sklitter::kDescriptor;
	const size_t character = static_cast<size_t>(fx::sklitter::Param::Rate);
	const size_t dotLane = e.addDevice(owltt.traits.tag);
	REQUIRE(env != RackEngine::kNoSlot);
	REQUIRE(dotLane != RackEngine::kNoSlot);
	const backplane::Source source = test::outputOf(e, env, 0);
	const backplane::Destination dest = destinationOf(e, owltt, dotLane, character);
	e.setRouteAmount(source, dest, 0.5f);
	block(e);
	REQUIRE(inputsInto(e, dotLane, character) == 1);
	// AND IT IS THIS ROUTE — the one input is the envelope's, at the amount written. (It enumerated the
	// compiled edge list for the source coordinate; an input carries a pointer to a cell, not a name, so
	// the identity is asked of the engine's route store.)
	CHECK(e.routeAmount(source, dest) == doctest::Approx(0.5f));

	const std::optional<size_t> cleared = e.moveDevice(e.positionOfLane(env), e.chainLength() - 1);
	REQUIRE(cleared.has_value());
	REQUIRE(*cleared == 1);
	block(e);
	CHECK(inputsInto(e, dotLane, character) == 0);
	CHECK(e.routeAmount(source, dest) == 0.0f);
}

TEST_CASE("an unrouted control is its base, untouched by the landing rule") {
	// `withRouteOffset` runs only when something reached the control — Feathers' rule — so a control no
	// route reaches is exactly the base the hand set, including one sitting between steps.
	const device::Descriptor& owltt = fx::owltt::kDescriptor;
	const size_t character = controlNamed(owltt, "Character");
	RackEngine e { rack::kRackAxes, hostCatalogue() };
	e.prepare(48000.0, 4096);
	const size_t lane = e.addDevice(owltt.traits.tag);
	e.setField(lane, character, 3.375f);
	block(e);
	CHECK(e.resolvedFieldForTest(lane, character) == 3.375f);
}

TEST_CASE("a full-depth route moves a control by its declared span — one zone for a zoned control, modSpan where declared, the range otherwise — for every device") {
	// **PORTED FROM `test_gui_bridge.cpp` WHEN THE BUS WENT (kill-lanes §2.4b).** It asked
	// `ParamSet::rackOffsetAt` against a hand-built bus; a device's controls are the ENGINE's to resolve
	// now, from inputs that point at their sources' cells, so the rule is asked where it is applied.
	//
	// THE RULE (`device::routeSpanOf`): a declared `modSpan` wins; a zoned control moves in ZONE WIDTHS —
	// a zone knob is a relative, fine modulation destination, because without it a route
	// displaced Character across all eight zones and a zone control stopped being one; everything else
	// moves by its range. ASSERTED AS A VALUE — the resolved field against an independent restatement of
	// the offset — over every modulatable control of every device the registry holds, so a device that
	// declares zones or a span and is not moved by it is a red rather than a picture nobody looks at.
	size_t zonedSeen = 0;
	size_t checked = 0;
	for (const device::Descriptor& desc : kDevices) {
		CAPTURE(std::string(desc.traits.name));
		RackEngine e { rack::kRackAxes, hostCatalogue() };
		e.prepare(48000.0, 4096);
		const size_t lane = e.addDevice(desc.traits.tag);
		if (lane == RackEngine::kNoSlot) {
			continue; // a device this rack cannot run (a container's contents, say) has nothing to resolve here
		}
		e.setMacro(0, 1.0f);
		for (size_t control = 0; control < desc.params.size(); ++control) {
			const device::ParamDesc& pd = desc.params[control];
			if (!pd.modulatable) {
				continue;
			}
			CAPTURE(control);
			e.setRouteAmount(test::macro(0), test::controlOf(e, lane, control), 1.0f);
			block(e);
			const double full = pd.max - pd.min;
			const double expectedSpan =
			    pd.modSpan > 0.0 ? pd.modSpan : (pd.zones > 0 ? full / static_cast<double>(pd.zones) : full);
			const double base = e.fieldAt(lane, control);
			// THE ENGINE LANDS WHERE `withRouteOffset` SAYS — the one rule every host shares, restated here
			// from the declaration rather than borrowed from the resolve under test.
			const double expected = device::withRouteOffset(pd, base, 1.0 * 1.0 * expectedSpan);
			CHECK(e.resolvedFieldForTest(lane, control) == doctest::Approx(expected));
			++checked;
			if (pd.modSpan > 0.0) {
				++zonedSeen;
				CHECK(pd.modSpan < full);
			}
			if (pd.zones > 0) {
				++zonedSeen;
				REQUIRE(pd.zones > 1);
				CHECK(e.resolvedFieldForTest(lane, control) != doctest::Approx(device::withRouteOffset(pd, base, full)));
			}
			e.setRouteAmount(test::macro(0), test::controlOf(e, lane, control), 0.0f);
		}
	}
	CHECK(checked > 0);
	CHECK(zonedSeen > 0);
}

TEST_CASE("SKLITTER's Engage can be driven by a macro — a gesture control has to be automatable") {
	// **THE SYMPTOM:** the Engage button could not be automated, by any road.
	// `DeviceControl::isDestination` reads `ParamDesc::modulatable` and nothing else, and Engage
	// declared `false` — so no macro could route to it. There is no second road either: no device
	// control is a CLAP parameter in either plugin (`rack_clap.cpp` publishes `kMacroCount` and the
	// synth publishes its own list), so the host has no automation lane to draw. A control whose
	// description read *"Automate it"* and which no road could reach is the honesty rule failing.
	//
	// **ASSERTED THROUGH THE ENGINE, NOT OFF THE DECLARATION.** Checking `modulatable == true` would
	// pass for a flag nobody consults. This drives a macro and reads the resolved field, which is the
	// number the DSP is handed.
	namespace sc = feathers::fx::sklitter;
	RackEngine e { rack::kRackAxes, hostCatalogue() };
	e.prepare(48000.0, 4096);
	const size_t lane = e.addDevice(sc::kTraits.tag);
	REQUIRE(lane != RackEngine::kNoSlot);

	const auto engage = static_cast<size_t>(sc::Param::Engage);
	const device::ParamDesc& pd = sc::kDescriptor.params[engage];
	REQUIRE(pd.modulatable);

	// AT REST IT IS OFF — the declared default, and what a patch starts at.
	e.setMacro(0, 0.0f);
	e.setRouteAmount(test::macro(0), test::controlOf(e, lane, engage), 0.0f);
	block(e);
	const double atRest = e.resolvedFieldForTest(lane, engage);
	CHECK(atRest == doctest::Approx(pd.def));

	// A FULL-DEPTH ROUTE AND A MACRO AT ONE MOVES IT THE WHOLE WAY — 0 to 1, which for a two-value
	// stepped control is exactly "engaged".
	e.setMacro(0, 1.0f);
	e.setRouteAmount(test::macro(0), test::controlOf(e, lane, engage), 1.0f);
	block(e);
	const double driven = e.resolvedFieldForTest(lane, engage);
	CAPTURE(driven);
	CHECK(driven == doctest::Approx(pd.max));
	CHECK(driven > atRest);

	// AND IT COMES BACK when the macro does, so an automation lane can hold the gesture and release
	// it rather than latching it on for ever.
	e.setMacro(0, 0.0f);
	block(e);
	CHECK(e.resolvedFieldForTest(lane, engage) == doctest::Approx(atRest));
}

TEST_CASE("SKLITTER's picture survives the rack's readings channel, all of it") {
	// **THE SYMPTOM:** the strip did not show. The band was drawing an empty bed, and BOTH roads to it were
	// broken — in different ways, which is why one fix would not have been enough.
	//
	// **THE RACK'S WAS A SILENT TRUNCATION.** `RackEngine::kMaxReadings` was 16, sized for OWLTT's
	// nine meters and its clipping flag; SKLITTER publishes a PICTURE — a header plus two floats for
	// each of sixteen steps, 37 in all. `publishReadings` CLAMPS, so the rack stored the header and
	// five and a half steps and reported a count to match. The card would have drawn five chunks of a
	// sixteen-step bar and been entirely plausible about it.
	//
	// (The other road was Feathers' own fx lane, where the generic sink publishes into the LANE
	// ENGINE's readings while `BridgeDeviceHost::readingAt` reads the BRIDGE's per-slot store — so
	// nothing reached the card at all. That one is wired in `feathers_clap.cpp` and cannot be reached
	// from here, which is exactly why it shipped.)
	namespace sc = feathers::fx::sklitter;
	RackEngine e { rack::kRackAxes, hostCatalogue() };
	e.prepare(48000.0, 4096);
	const size_t lane = e.addDevice(sc::kTraits.tag);
	REQUIRE(lane != RackEngine::kNoSlot);

	// ENGAGE IT AND DRIVE A BAR. A looper waits for a grid line, so a still playhead leaves it in
	// Standby for ever — `test_fx_sklitter.cpp` carries that note and the device sweeps learned it the
	// hard way the same day.
	e.setField(lane, static_cast<size_t>(sc::Param::Engage), 1.0f);
	e.setField(lane, static_cast<size_t>(sc::Param::ZoneA), 0.6f);
	e.setField(lane, static_cast<size_t>(sc::Param::ZoneB), 0.8f);
	double beats = 0.0;
	std::array<float, 480> l {};
	std::array<float, 480> r {};
	for (int n = 0; n < (96000 + 9600) / 480; ++n) {
		for (size_t i = 0; i < l.size(); ++i) {
			l[i] = 0.5f * std::sin(0.02 * static_cast<double>(i));
			r[i] = l[i];
		}
		// THE TRANSPORT RIDES ON THE BLOCK HERE — the rack has no `setTransportClock` of its own; it
		// takes the clock per block, as a host does, and hands it down to each device.
		backplane::BlockContext block {};
		block.clock = device::TransportClock::fromSecondsPerBeat(0.5);
		block.clock.songPositionBeats = beats;
		block.clock.playing = true;
		e.processStereo(l.data(), r.data(), static_cast<int>(l.size()), block);
		beats += static_cast<double>(l.size()) / (0.5 * 48000.0);
	}

	// **THE WHOLE PICTURE ARRIVED, not a prefix of it.** This is the assertion the clamp defeated: the
	// count is what the device published, and at 16 it would have read 16.
	const size_t count = e.readingCountForTest(lane);
	CAPTURE(count);
	CHECK(count == sc::kReadingCount);

	// AND IT IS A REAL PICTURE: the device is playing, and the steps it reports fit the picture.
	const auto status = static_cast<int>(
	    std::lround(e.readingForTest(lane, static_cast<size_t>(sc::Reading::Status))));
	CAPTURE(status);
	CHECK(status == 3); // Playing

	const auto steps = static_cast<size_t>(
	    std::lround(e.readingForTest(lane, static_cast<size_t>(sc::Reading::Steps))));
	CAPTURE(steps);
	CHECK(steps > 1);
	CHECK(steps <= sc::kPictureSteps);

	// **AT LEAST ONE STEP HAS ACTUALLY BEEN WALKED**, or the picture is a header over silence — which
	// is what a count check alone would have accepted.
	size_t known = 0;
	for (size_t i = 0; i < steps; ++i) {
		const size_t base = sc::stepReading(i);
		const auto flags = static_cast<uint32_t>(std::lround(e.readingForTest(lane, base + 1u)));
		if ((flags & static_cast<uint32_t>(sc::StepFlag::Known)) != 0u) {
			++known;
			const float source = e.readingForTest(lane, base);
			CAPTURE(i);
			CAPTURE(source);
			CHECK(source >= 0.0f);
			CHECK(source <= 1.0f);
		}
	}
	CAPTURE(known);
	CHECK(known > 0);
}

TEST_CASE("stereo modulation's zero cost: a filter in a rack with mono routes renders the bits it rendered before it (pinned at 616d4dad)") {
#if !(defined(__x86_64__) && defined(__linux__))
	MESSAGE("bit identity is pinned on Linux x86-64 only — skipped");
	return;
#endif
	namespace flt = fx::filter;
	RackEngine e { rack::kRackAxes, hostCatalogue() };
	e.prepare(48000.0, 4096);
	const size_t lfo = e.addDevice(mod::lfo::kTraits.tag);
	const size_t f = e.addDevice(flt::kTraits.tag);
	REQUIRE(lfo != RackEngine::kNoSlot);
	REQUIRE(f != RackEngine::kNoSlot);
	e.setField(f, static_cast<size_t>(flt::Param::Drive), 0.3f);
	e.setRouteAmount(test::outputOf(e, lfo, 0), test::controlOf(e, f, static_cast<size_t>(flt::Param::Cutoff)), 0.2f);
	e.setRouteAmount(test::macro(0), test::controlOf(e, f, static_cast<size_t>(flt::Param::Macro1)), 0.5f);
	e.setMacro(0, 0.7f);
	uint64_t h = 1469598103934665603ull;
	uint32_t seed = 0x5eed1234u;
	for (int b = 0; b < 200; ++b) {
		std::array<float, 64> l {};
		std::array<float, 64> r {};
		for (size_t i = 0; i < l.size(); ++i) {
			seed = seed * 1664525u + 1013904223u;
			l[i] = static_cast<float>(static_cast<int32_t>(seed >> 8) - (1 << 23)) / static_cast<float>(1 << 23);
			r[i] = -l[i] * 0.5f;
		}
		e.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
		for (const std::array<float, 64>* ch : { &l, &r }) {
			for (const float x : *ch) {
				uint32_t bits = 0;
				std::memcpy(&bits, &x, sizeof bits);
				for (int k = 0; k < 4; ++k) {
					h ^= (bits >> (8 * k)) & 0xffu;
					h *= 1099511628211ull;
				}
			}
		}
	}
	constexpr uint64_t kPinned = 1725138510344769375ull; // captured at 616d4dad
	CAPTURE(h);
	CHECK(h == kPinned);
}

TEST_CASE("at rest a row is resolved whole once, then only its routed controls — and every control still holds the value a whole resolve gives") {
	// THE IDLE-OVERHEAD PASS (2026-11-05; `Slot::baseSerial`). A control no input reaches resolves from its base, span and
	// host offset alone, so `resolveInto` re-resolves the whole row only when one of those, or the routes, moved — and
	// otherwise only the routed controls. MEASURED before it: 26k controls resolved a 1024-frame block at rest on a real
	// patch. Pinned BY VALUE: the whole-resolve count does not move at rest, the routed control follows its macro every
	// block, and every unrouted control is its base; then each thing that must make the next resolve whole does.
	const device::Descriptor& fd = fx::filter::kDescriptor;
	const size_t cutoff = static_cast<size_t>(fx::filter::Param::Cutoff);
	const size_t resonance = static_cast<size_t>(fx::filter::Param::Resonance);
	const size_t drive = static_cast<size_t>(fx::filter::Param::Drive);
	RackEngine e { rack::kRackAxes, hostCatalogue() };
	e.prepare(48000.0, 4096);
	const size_t lane = e.addDevice(fd.traits.tag);
	REQUIRE(lane != RackEngine::kNoSlot);
	constexpr float kAmount = 0.25f;
	e.setRouteAmount(test::macro(0), destinationOf(e, fd, lane, cutoff), kAmount);
	const auto want = [&](size_t control, float macro) {
		const device::ParamDesc& p = fd.params[control];
		return static_cast<float>(device::withRouteOffset(p, static_cast<double>(e.fieldAt(lane, control)),
		                                                  static_cast<double>(kAmount) * static_cast<double>(macro) * device::routeSpanOf(p)));
	};
	const auto everyUnroutedIsItsBase = [&] {
		for (size_t c = 0; c < fd.params.size(); ++c) {
			if (c != cutoff && e.resolvedFieldForTest(lane, c) != e.fieldAt(lane, c)) {
				return false;
			}
		}
		return true;
	};
	for (int b = 0; b < 4; ++b) {
		block(e); // the drains, the install, the first whole resolves
	}
	const uint64_t rested = e.fullResolvesForTest();
	float last = e.resolvedFieldForTest(lane, cutoff);
	int moved = 0;
	for (int b = 0; b < 32; ++b) {
		const float macro = static_cast<float>(b % 8) / 8.0f;
		e.setMacro(0, macro);
		block(e);
		CHECK(e.resolvedFieldForTest(lane, cutoff) == want(cutoff, macro));
		moved += e.resolvedFieldForTest(lane, cutoff) != last ? 1 : 0;
		last = e.resolvedFieldForTest(lane, cutoff);
	}
	CHECK(e.fullResolvesForTest() == rested); // AT REST: no whole resolve, whatever the routed control did
	CHECK(moved > 20);                        // NOT VACUOUS: the routed control was resolved every block
	CHECK(everyUnroutedIsItsBase());
	// THE SAME BITS WRITTEN AGAIN (what the converger does at every drain) are no change.
	e.setField(lane, resonance, e.fieldAt(lane, resonance));
	block(e);
	CHECK(e.fullResolvesForTest() == rested);
	// A BASE WRITE makes the next resolve whole, and the control is the new base.
	const float newResonance = e.fieldAt(lane, resonance) == 0.5f ? 0.25f : 0.5f;
	e.setField(lane, resonance, newResonance);
	block(e);
	CHECK(e.fullResolvesForTest() > rested);
	CHECK(e.resolvedFieldForTest(lane, resonance) == newResonance);
	CHECK(everyUnroutedIsItsBase());
	// A NEW ROUTE makes it whole, and the newly routed control follows its macro from that block.
	const uint64_t beforeRoute = e.fullResolvesForTest();
	e.setRouteAmount(test::macro(0), destinationOf(e, fd, lane, drive), kAmount);
	e.setMacro(0, 0.75f);
	block(e);
	CHECK(e.fullResolvesForTest() > beforeRoute);
	CHECK(e.resolvedFieldForTest(lane, drive) == want(drive, 0.75f));
	CHECK(e.resolvedFieldForTest(lane, drive) != e.fieldAt(lane, drive)); // NOT VACUOUS: the route moved it
	// AND THE ROUTE TAKEN AWAY puts the control back on its base, through a whole resolve.
	e.setRouteAmount(test::macro(0), destinationOf(e, fd, lane, drive), 0.0f);
	block(e);
	CHECK(e.resolvedFieldForTest(lane, drive) == e.fieldAt(lane, drive));
	CHECK(everyUnroutedIsItsBase());
}


//==================================================================================================
// PER-DEVICE OUTPUT COUNTS (2026-11-07)
//==================================================================================================

namespace {

/// A MODULATOR WITH 43 OUTPUTS — Pattern's coming count — each a stereo pair: output o publishes `outValue(o)` on the left
/// and its negative on the right, so a case can name, by value, which output reached a destination in each ear.
constexpr size_t kManyOutputs = 43;
struct ManyKeys {
	char key[kManyOutputs][4] {};
};
constexpr ManyKeys kManyKeys = [] {
	ManyKeys k {};
	for (size_t i = 0; i < kManyOutputs; ++i) {
		k.key[i][0] = 'o';
		k.key[i][1] = static_cast<char>('0' + i / 10);
		k.key[i][2] = static_cast<char>('0' + i % 10);
	}
	return k;
}();
constexpr std::array<device::SourceDesc, kManyOutputs> kManySources = [] {
	std::array<device::SourceDesc, kManyOutputs> s {};
	for (size_t i = 0; i < kManyOutputs; ++i) {
		s[i].key = kManyKeys.key[i];
		s[i].name = kManyKeys.key[i];
		s[i].bipolar = true;
		s[i].pairAlways = true;
	}
	return s;
}();
[[nodiscard]] float outValue(size_t o) {
	return static_cast<float>(o + 1) / 64.0f;
}
struct Many {
	int ticks = 0;
};
constexpr device::ModOps kManyOps {
	.stateBytes = sizeof(Many),
	.stateAlign = alignof(Many),
	.construct = [](void* s) { new (s) Many {}; },
	.destruct = [](void* s) { static_cast<Many*>(s)->~Many(); },
	.prepare = [](void*, double) {},
	.reset = [](void*) {},
	.setFields = [](void*, const float*, size_t) {},
	.blockSamples = 64,
	.tick = [](void* s, const device::ModTickContext&) { ++static_cast<Many*>(s)->ticks; },
	.outputAt = [](const void*, size_t o) { return outValue(o); },
	.playhead = nullptr,
	.outputPairAt = [](const void*, size_t o) { return device::EarPair { outValue(o), -outValue(o) }; },
};
const device::Descriptor kManyDesc {
	.traits = { .tag = "TMNY", .name = "Many outputs", .hasCard = false, .hasExpanded = false, .perVoiceControl = -1 },
	.params = {},
	.outputs = kManySources,
	.mod = &kManyOps,
};
const device::Descriptor* findWithMany(void*, const char* tag) {
	return device::sameTag(tag, kManyDesc.traits.tag) ? &kManyDesc : deviceForTag(tag);
}
const device::Catalogue kManyCatalogue { nullptr, &findWithMany };

} // namespace

TEST_CASE("per-device outputs: a 43-output device's outputs route and read by value, and one past bit 7 is a stereo pair in both ears") {
	// THE STRIDE WAS 7 FOR EVERY DEVICE; each slot's outputs are its own device's count now. Each of these outputs, the
	// first, either side of the old byte-wide pair mask, and the last, routed alone into a filter's stereo Resonance: the
	// left ear moves by its value and the right by its negative — a pair past bit 7 is a pair (the mask is 64 bits).
	namespace flt = fx::filter;
	const size_t resonance = static_cast<size_t>(flt::Param::Resonance);
	const device::ParamDesc& p = flt::kDescriptor.params[resonance];
	REQUIRE(p.stereo);
	constexpr float kAmount = 0.5f;
	for (const size_t o : { size_t { 0 }, size_t { 7 }, size_t { 8 }, size_t { 42 } }) {
		CAPTURE(o);
		RackEngine e { rack::kRackAxes, kManyCatalogue };
		e.prepare(48000.0, 4096);
		const size_t many = e.addDevice(kManyDesc.traits.tag);
		const size_t f = e.addDevice(flt::kTraits.tag);
		REQUIRE(many != RackEngine::kNoSlot);
		REQUIRE(f != RackEngine::kNoSlot);
		e.setRouteAmount(test::outputOf(e, many, o), test::controlOf(e, f, resonance), kAmount);
		for (int b = 0; b < 3; ++b) {
			block(e);
		}
		const double base = static_cast<double>(e.fieldAt(f, resonance));
		const auto want = [&](float v) {
			return static_cast<float>(device::withRouteOffset(p, base, static_cast<double>(kAmount) * static_cast<double>(v) * device::routeSpanOf(p)));
		};
		CHECK(e.resolvedFieldForTest(f, resonance) == want(outValue(o)));
		CHECK(e.resolvedFieldRForTest(f, resonance) == want(-outValue(o)));
		CHECK(e.resolvedFieldForTest(f, resonance) != e.resolvedFieldRForTest(f, resonance)); // NOT VACUOUS: two ears
	}
}

TEST_CASE("per-device outputs: each slot's output storage is exactly its own device's count, per voice too") {
	// THE SAVING, BY VALUE: a slot holds its device's outputs and no more — the 43-output device 43, an LFO its own few, a
	// filter none — in each ear, in windows of `kTickWindowStride`; and per voice `voices × count` only for a device that
	// declares a `perVoice` output (the LFO), none for one whose outputs are one value for every voice (the 43).
	namespace flt = fx::filter;
	namespace lfo = mod::lfo;
	RackEngine e { rack::kRackAxes, kManyCatalogue };
	e.prepare(48000.0, 4096);
	e.setVoices(4);
	const size_t many = e.addDevice(kManyDesc.traits.tag);
	const size_t lf = e.addDevice(lfo::kTraits.tag);
	const size_t f = e.addDevice(flt::kTraits.tag);
	const auto stride = static_cast<size_t>(device::kTickWindowStride);
	bool lfoPerVoice = false;
	for (const device::SourceDesc& s : lfo::kDescriptor.outputs) {
		lfoPerVoice = lfoPerVoice || s.perVoice;
	}
	REQUIRE(lfoPerVoice); // NOT VACUOUS: one of the three keeps outputs per voice
	for (const auto& [lane, count, voiced] : { std::tuple { many, kManyOutputs, size_t { 0 } },
	                                           std::tuple { lf, lfo::kDescriptor.outputCount(), lfo::kDescriptor.outputCount() },
	                                           std::tuple { f, size_t { 0 }, size_t { 0 } } }) {
		CAPTURE(lane);
		const RackEngine::OutputStorage s = e.outputStorageForTest(lane);
		CHECK(s.cells == count);
		CHECK(s.cellsR == count);
		CHECK(s.windows == count * stride);
		CHECK(s.windowsR == count * stride);
		CHECK(s.perVoice == 4 * voiced);
		CHECK(s.perVoiceWindows == 4 * voiced * stride);
	}
	CHECK(lfo::kDescriptor.outputCount() < device::kMaxDeviceOutputs); // NOT the bound: the device's own
	// AND A VOICE COUNT CHANGED LATER resizes each slot to ITS device's count, not the bound.
	e.setVoices(2);
	CHECK(e.outputStorageForTest(lf).perVoice == 2 * lfo::kDescriptor.outputCount());
	CHECK(e.outputStorageForTest(many).perVoice == 0);
	CHECK(e.outputStorageForTest(f).perVoice == 0);
}

TEST_CASE("per-device outputs: an install hands the last device's output buffers back in the husk, for the message thread to free") {
	// THE LIFETIME (lead's condition 1): the drain never frees a slot's output storage. A slot that held an LFO, emptied and
	// then given the 43-output device, swaps the LFO's buffers OUT into the adopted provision — the husk the converger
	// retires to the message thread — and holds the new device's own. Read by value off both sides of the swap.
	namespace lfo = mod::lfo;
	RackEngine e { rack::kRackAxes, kManyCatalogue };
	e.prepare(48000.0, 4096);
	const size_t lane = e.addDevice(lfo::kTraits.tag);
	REQUIRE(lane != RackEngine::kNoSlot);
	const size_t lfoCount = lfo::kDescriptor.outputCount();
	REQUIRE(e.outputStorageForTest(lane).cells == lfoCount);
	REQUIRE(e.removeDeviceAtLane(lane));
	block(e);
	e.reapRetired();
	// AN EMPTY SLOT KEEPS ITS LAST DEVICE'S BUFFERS (a removal zeroes, never frees).
	CHECK(e.outputStorageForTest(lane).cells == lfoCount);
	RackEngine::Provision p = e.provision(kManyDesc.traits.tag);
	REQUIRE(p.outputs.size() == kManyOutputs);
	const size_t placed = e.adoptDevice(e.chainLength(0), std::move(p));
	REQUIRE(placed == lane);
	CHECK(e.outputStorageForTest(lane).cells == kManyOutputs);
	// THE HUSK HOLDS THE LFO'S: they left the slot by swap, so whoever owns the provision frees them, off the drain.
	CHECK(p.outputs.size() == lfoCount);                                  // NOLINT(bugprone-use-after-move): the husk
	CHECK(p.outputsWindow.size() == lfoCount * static_cast<size_t>(device::kTickWindowStride)); // NOLINT
}

TEST_CASE("per-device outputs: a route from output 42 survives save and load into a fresh instance, by value") {
	// A ROUTE STORES ITS SOURCE AS (instance, output index) — no stride in it — so a source past the old bound of 7 saves and
	// loads like any other: the fresh rack's filter hears output 42's value, not another output's.
	namespace flt = fx::filter;
	const size_t resonance = static_cast<size_t>(flt::Param::Resonance);
	const device::ParamDesc& p = flt::kDescriptor.params[resonance];
	persist::Rig run { kManyCatalogue, rack::kRackAxes }; // the rack's source bound, kMaxDeviceOutputs
	run.seat(kManyDesc.traits.tag);
	const DeviceInstanceId many = run.id;
	const DeviceInstanceId f = run.bridge->addDevice(flt::kTraits.tag);
	run.block(2);
	run.bridge->setRouteAmount(backplane::Source::output(many, 42), backplane::Destination::control(f, resonance), 0.5f);
	run.block(3);
	{
		// THE ROUTE REACHES IN THE RUNNING RACK FIRST, so the check after the load is about the load.
		const size_t at = run.engine.slotOf(f);
		REQUIRE(at != RackEngine::kNoSlot);
		const double base = static_cast<double>(run.engine.fieldAt(at, resonance));
		CHECK(run.engine.resolvedFieldForTest(at, resonance)
		      == static_cast<float>(device::withRouteOffset(p, base, 0.5 * static_cast<double>(outValue(42)) * device::routeSpanOf(p))));
	}
	const std::string saved = run.save();
	persist::Rig back { kManyCatalogue, rack::kRackAxes };
	back.load(saved);
	back.block(3);
	const size_t lane = back.engine.slotOf(back.bridge->deviceAtPosition(1));
	REQUIRE(lane != RackEngine::kNoSlot);
	const double base = static_cast<double>(back.engine.fieldAt(lane, resonance));
	const auto want = static_cast<float>(device::withRouteOffset(p, base, 0.5 * static_cast<double>(outValue(42)) * device::routeSpanOf(p)));
	CHECK(back.engine.resolvedFieldForTest(lane, resonance) == want);
	CHECK(want != static_cast<float>(base)); // NOT VACUOUS: the route moved it
}

TEST_CASE("pattern sources: a route from Tab 4 Lane 8 (output 42) reads that lane by value through the rack, and again after save and load into a fresh rack") {
	namespace pat = mod::arp::pat;
	namespace genes = mod::arp::genes;
	namespace flt = fx::filter;
	const size_t resonance = static_cast<size_t>(flt::Param::Resonance);
	const device::ParamDesc& p = flt::kDescriptor.params[resonance];
	const size_t lane = pat::laneOutput(3, 7);
	REQUIRE(lane == 42);
	REQUIRE(std::string(pat::kOutputs[lane].key) == "tab4.lane8");
	// TAB 4 ALONE, IN MODULATION, Leap up (the ride, lane 8, hits); tab 1 is the one a card opens on.
	pat::Settings s;
	for (size_t t = 0; t < pat::kPatterns; ++t) {
		s.patterns[t].enable = t == 3;
	}
	s.patterns[3].mode = genes::Mode::Modulation;
	s.patterns[3].pins[static_cast<size_t>(genes::Pin::Leap)] = 1.0f;
	s.patterns[3].pins[static_cast<size_t>(genes::Pin::Density)] = 1.0f;
	const pat::Row row = pat::rowOf(s);
	const auto clockAt = [](int64_t sample) {
		device::TransportClock c = device::TransportClock::fromSecondsPerBeat(0.5);
		c.playing = true;
		c.songPositionBeats = static_cast<double>(sample) / 24000.0;
		c.songPositionSeconds = c.songPositionBeats * 0.5;
		return c;
	};
	// RUN `blocks` BLOCKS, the notes asked before each as a host asks; count the blocks whose lane value held still across
	// the block and check the filter's resolved resonance against it, by value.
	const auto run = [&](persist::Rig& rig, DeviceInstanceId pattern, DeviceInstanceId filter, int64_t* sample, int blocks) {
		int checked = 0;
		int nonzero = 0;
		for (int b = 0; b < blocks; ++b) {
			const size_t ps = rig.engine.slotOf(pattern);
			REQUIRE(ps != RackEngine::kNoSlot);
			const auto& dev = *reinterpret_cast<const pat::PatternDevice*>(rig.engine.slotAtLane(ps).storage.get());
			const float before = dev.outputAt(lane);
			// ONE BLOCK UNDER A PLAYING TRANSPORT: the rack hands its note devices the block's clock.
			rig.bridge->drain();
			rig.engine.processStereo(rig.l.data(), rig.r.data(), 64, backplane::BlockContext { clockAt(*sample) });
			rig.bridge->publish(rig.engine);
			*sample += 64;
			const float after = dev.outputAt(lane);
			if (before != after) {
				continue;
			}
			const size_t at = rig.engine.slotOf(filter);
			REQUIRE(at != RackEngine::kNoSlot);
			const double base = static_cast<double>(rig.engine.fieldAt(at, resonance));
			const auto want = static_cast<float>(
			    device::withRouteOffset(p, base, 0.5 * static_cast<double>(after) * device::routeSpanOf(p)));
			CHECK(rig.engine.resolvedFieldForTest(at, resonance) == want);
			++checked;
			nonzero += after != 0.0f ? 1 : 0;
		}
		CHECK(checked > blocks / 2);
		CHECK(nonzero > 0); // NOT VACUOUS: the lane moved the filter
	};
	persist::Rig rig { hostCatalogue(), rack::kRackAxes };
	rig.seat(pat::kTraits.tag, row);
	const DeviceInstanceId pattern = rig.id;
	const DeviceInstanceId filter = rig.bridge->addDevice(flt::kTraits.tag);
	rig.block(2);
	rig.bridge->setRouteAmount(backplane::Source::output(pattern, lane), backplane::Destination::control(filter, resonance), 0.5f);
	int64_t sample = 0;
	run(rig, pattern, filter, &sample, 1500);
	const std::string saved = rig.save();
	persist::Rig back { hostCatalogue(), rack::kRackAxes };
	back.load(saved);
	const DeviceInstanceId pattern2 = back.bridge->deviceAtPosition(0);
	const DeviceInstanceId filter2 = back.bridge->deviceAtPosition(1);
	REQUIRE(back.bridge->descriptorOf(pattern2) != nullptr);
	REQUIRE(std::string(back.bridge->descriptorOf(pattern2)->traits.tag) == pat::kTraits.tag);
	int64_t sample2 = 0;
	run(back, pattern2, filter2, &sample2, 1500);
}

TEST_CASE("pattern Go's pending state through the rack's own road: a press arms the fill and the bridge says so beside the playhead, block by block, until the fill fires at its boundary — then it says nothing is pending") {
	namespace pat = mod::arp::pat;
	// TAB 1 PLAYS (Selector), TAB 2 IS A FILL; nothing else in range.
	pat::Settings s;
	s.patterns[0].enable = true;
	s.patterns[0].trig = 1; // Transport: it runs off the transport, no key held (a rack with no note input)
	s.patterns[1].trig = 1;
	s.patterns[1].enable = false;
	s.phase[1].fill = true;
	const pat::Row row = pat::rowOf(s);
	persist::Rig rig { hostCatalogue(), rack::kRackAxes };
	rig.seat(pat::kTraits.tag, row);
	const DeviceInstanceId pattern = rig.id;
	int64_t sample = 0;
	const auto clockAt = [](int64_t at) {
		device::TransportClock c = device::TransportClock::fromSecondsPerBeat(0.5);
		c.playing = true;
		c.songPositionBeats = static_cast<double>(at) / 24000.0;
		c.songPositionSeconds = c.songPositionBeats * 0.5;
		return c;
	};
	const auto block = [&] {
		rig.bridge->drain();
		rig.engine.processStereo(rig.l.data(), rig.r.data(), 64, backplane::BlockContext { clockAt(sample) });
		rig.bridge->publish(rig.engine);
		sample += 64;
	};
	const auto pending = [&] {
		float level = -1.0f;
		REQUIRE(rig.bridge->playheadLevelAt(pattern, level));
		return pat::PatternDevice::armedFillOfLevel(level);
	};
	const auto& dev = [&]() -> const pat::PatternDevice& {
		return *reinterpret_cast<const pat::PatternDevice*>(rig.engine.slotAtLane(rig.engine.slotOf(pattern)).storage.get());
	};
	for (int i = 0; i < 40; ++i) {
		block();
	}
	CHECK(pending() == pat::kPatterns); // nothing pressed: nothing pending
	// THE PRESS, as a host's parameter write: up, a block, down.
	rig.bridge->setField(pattern, pat::at(1, pat::Field::FillTrigger), 1.0f);
	block();
	rig.bridge->setField(pattern, pat::at(1, pat::Field::FillTrigger), 0.0f);
	block();
	CHECK(pending() == 1u);
	CHECK(dev().armedFillForTest() == 1u);
	// PENDING EVERY BLOCK UNTIL IT FIRES; then not pending, and the fill is the one sounding.
	int blocks = 0;
	while (pending() == 1u && blocks < 2000) {
		CHECK(dev().activeFillForTest() == pat::kPatterns);
		block();
		++blocks;
	}
	CHECK(blocks > 0);
	CHECK(pending() == pat::kPatterns);
	CHECK(dev().activeFillForTest() == 1u);
}

TEST_CASE("pattern Go pressed and released between two blocks still arms the fill (lead, 2026-11-07: \"press and release inside one block: the press is LOST\" is a bug) — the rack's road") {
	namespace pat = mod::arp::pat;
	pat::Settings s;
	s.patterns[0].enable = true;
	s.patterns[0].trig = 1;
	s.patterns[1].trig = 1;
	s.patterns[1].enable = false;
	s.phase[1].fill = true;
	const pat::Row row = pat::rowOf(s);
	persist::Rig rig { hostCatalogue(), rack::kRackAxes };
	rig.seat(pat::kTraits.tag, row);
	const DeviceInstanceId pattern = rig.id;
	int64_t sample = 0;
	const auto clockAt = [](int64_t at) {
		device::TransportClock c = device::TransportClock::fromSecondsPerBeat(0.5);
		c.playing = true;
		c.songPositionBeats = static_cast<double>(at) / 24000.0;
		c.songPositionSeconds = c.songPositionBeats * 0.5;
		return c;
	};
	const auto block = [&] {
		rig.bridge->drain();
		rig.engine.processStereo(rig.l.data(), rig.r.data(), 64, backplane::BlockContext { clockAt(sample) });
		rig.bridge->publish(rig.engine);
		sample += 64;
		(void)rig.bridge->serviceHostSlots(); // the plugin's main-thread visit, as its timer runs it
	};
	const auto& dev = [&]() -> const pat::PatternDevice& {
		return *reinterpret_cast<const pat::PatternDevice*>(rig.engine.slotAtLane(rig.engine.slotOf(pattern)).storage.get());
	};
	const size_t go = pat::at(1, pat::Field::FillTrigger);
	for (int i = 0; i < 40; ++i) {
		block();
	}
	REQUIRE(dev().armedFillForTest() == pat::kPatterns);
	// THE PRESS AND THE RELEASE, BOTH BEFORE THE NEXT BLOCK — a quick click on a long buffer.
	rig.bridge->setField(pattern, go, 1.0f);
	rig.bridge->releaseField(pattern, go, 0.0f); // the momentary's release road (`SwitchChip`)
	CHECK(rig.bridge->fieldAt(pattern, go) == 1.0f); // the fall waits for the rise to be read
	block();
	CHECK(dev().armedFillForTest() == 1u); // the press counted
	// AND THE FALL LANDS ONCE A BLOCK HAS READ THE RISE: the trigger is back at rest, nothing left high.
	block();
	CHECK(rig.bridge->fieldAt(pattern, go) == 0.0f);
	// A SECOND QUICK CLICK AFTER THE FILL FIRED: counted too (not stuck behind the first).
	int blocks = 0;
	while (dev().armedFillForTest() == 1u && blocks++ < 4000) {
		block();
	}
	REQUIRE(dev().armedFillForTest() == pat::kPatterns);
	while (dev().activeFillForTest() != pat::kPatterns && blocks++ < 8000) {
		block();
	}
	rig.bridge->setField(pattern, go, 1.0f);
	rig.bridge->releaseField(pattern, go, 0.0f);
	block();
	CHECK(dev().armedFillForTest() == 1u);
	// A PLAIN WRITE (a preset, an undo) IS NEVER HELD: up and down at once, as it always was.
	const size_t latch = pat::at(pat::Global::Latch);
	rig.bridge->setField(pattern, latch, 1.0f);
	rig.bridge->setField(pattern, latch, 0.0f);
	CHECK(rig.bridge->fieldAt(pattern, latch) == 0.0f);
	// AN ORDINARY CONTROL IS NOT HELD: a fall with no rise in flight writes at once.
	const size_t solo = pat::at(0, pat::Field::Solo);
	rig.bridge->setField(pattern, solo, 0.0f);
	CHECK(rig.bridge->fieldAt(pattern, solo) == 0.0f);
}
