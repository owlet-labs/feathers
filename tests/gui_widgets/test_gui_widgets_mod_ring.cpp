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

// THE MOD RING AND WHAT IT DRAWS. Ten cases gathered 2026-09-03 from the modulation panel, the source grid and the shaper plots, so the surface AO and BF will extend has one file: an armed knob writes an AMOUNT, the ring follows the live value, arming lights a card's knobs, the halo greys with the card, a host-modulated macro shows on both ends, every knob is wired to the modulation state, the coordinate boxes' coarse:fine pair, and what a bipolar or unipolar source draws. modRing() returns a STRUCT and these cases read values from it — never a render comparison.

#include "fixtures.h"
#include "../param_routes.h"

#include <visage/windowing.h>

namespace {

/// HOW FAR MACRO `macro` REACHES FEATHERS' PARAMETER DESTINATION `destination` — by endpoint (kill-lanes
/// step 2): a macro and a parameter are both host ends, at the indices the axes give them.
float macroAmount(const ParamBridge& bridge, size_t macro, int destination) {
	return bridge.routeAmount(macroSourceAt(macro), backplane::Destination::core(static_cast<size_t>(destination)));
}

/// WHETHER ANY ROUTE EXISTS AT ALL — the published list, which is every route the bridge holds.
bool anyRoute(const ParamBridge& bridge) { return bridge.routeList().count() > 0; }

} // namespace

TEST_CASE("a drag on an armed knob writes an AMOUNT, and leaves the parameter where it was") {
	ModFixture f;
	f.arm(macroSourceAt(0));

	ui::Knob* path = f.knobFor(ParamId::Path);
	REQUIRE(path != nullptr);
	REQUIRE(path->mappable());

	const double valueBefore = f.bridge.value(static_cast<clap_id>(ParamId::Path));
	const visage::Point centre = f.faceCentre(path);
	dragThroughRoot(f.root, centre, { centre.x, centre.y - 35.0f });

	const int destination = modDestinationIndexOf(ParamId::Path);
	REQUIRE(destination >= 0);
	const float amount = macroAmount(f.bridge, 0, destination);

	// The seed plus half the drag range: the press creates the mock's 0.4 and 35 of 70 pixels adds
	// another half.
	CHECK(amount == doctest::Approx(ui::Knob::kRouteSeedAmount + 0.5f).epsilon(0.02));
	// AND THE PARAMETER DID NOT MOVE. This is the whole difference between a routing gesture and a
	// value gesture, and the known-bad it guards is a knob that forgot which of the two it was in.
	CHECK(f.bridge.value(static_cast<clap_id>(ParamId::Path)) == valueBefore);

	SUBCASE("and the ring it draws is the amount, from the base") {
		// Read at the glass rather than inferred from the bridge: the ring is what a hand is steering
		// by, so it is what the case has to assert.
		const ui::Knob::ModRing ring = path->modRing();
		CHECK(ring.drawn);
		CHECK(ring.glow == doctest::Approx(1.0f));
		CHECK(ring.lo == doctest::Approx(path->normalized()));
		CHECK(ring.hi == doctest::Approx(std::clamp(path->normalized() + amount, 0.0f, 1.0f)));
	}

	SUBCASE("a second drag back to nothing takes the route away rather than zeroing it") {
		// The mock's `if(Math.abs(routeAmt(id))<.03)delete S.lfo.routes[id]`. A route left at 0 % would
		// keep a ring and a row for a connection the hand has just taken back. Dragging PAST zero is a
		// different thing and stays a route — an inverted one — which is why this walks back exactly the
		// distance the amount is worth rather than simply shoving the pointer downward.
		const visage::Point again = f.faceCentre(path);
		dragThroughRoot(f.root, again, { again.x, again.y + amount * ui::Knob::kRouteDragRange });
		CHECK(macroAmount(f.bridge, 0, destination) == 0.0f);
		CHECK_FALSE(anyRoute(f.bridge));
		CHECK_FALSE(path->modRing().drawn);
	}
}

TEST_CASE("the ring follows the live value when nothing is armed, and undo puts an amount back") {
	ModFixture f;
	f.arm(macroSourceAt(0));
	ui::Knob* path = f.knobFor(ParamId::Path);
	REQUIRE(path != nullptr);
	const visage::Point centre = f.faceCentre(path);
	dragThroughRoot(f.root, centre, { centre.x, centre.y - 35.0f });
	const int destination = modDestinationIndexOf(ParamId::Path);
	REQUIRE(destination >= 0);
	const float amount = macroAmount(f.bridge, 0, destination);
	REQUIRE(amount > 0.0f);

	SUBCASE("disarmed and at rest, the ring is the faint hint the mock draws") {
		f.arm(macroSourceAt(0)); // the same arrow again disarms
		CHECK_FALSE(path->mappable());
		const ui::Knob::ModRing ring = path->modRing();
		CHECK(ring.drawn);
		// THE AMOUNT ARC, a touch quieter than while assigning — and no dot: nothing is moving the value.
		CHECK(ring.glow == doctest::Approx(0.8f));
		CHECK(ring.hi == doctest::Approx(std::clamp(path->normalized() + amount, 0.0f, 1.0f)));
		CHECK_FALSE(ring.hasMark);
	}

	SUBCASE("disarmed and moving, the ring is the amount and the dot is the modulated result") {
		f.arm(macroSourceAt(0));
		// What the engine would publish with that macro turned up: base, and base + what the routing
		// adds. **THROUGH `publish`, WHICH IS ACTUALLY THE DOOR THE PLUGIN USES.**
		//
		// This said "fed through the same door the plugin uses" while doing something else: it built
		// the two arrays by hand — `params.base(...)` and `params.resolved(...)` per id — and pushed
		// them through `publishValuesForTest`. That hand-rolled loop IS `ParamBridge::publish`, so the
		// fixture was performing by hand the exact copy the production line performs, and `publish`
		// could have been broken with this case still green. The same shape test_mod_rack.cpp names in
		// its own comment about `setRoutes`, found again one file over.
		//
		// **THE ROUTES COME OFF THE PUBLISHED LIST, WHICH IS WHERE THE ENGINE TAKES THEM FROM.** This
		// went through `route_coords.h`, which built the dense table by walking every coordinate the
		// axes can express and asking the record about each — 464 questions for the one route this case
		// made. `ParamSet` is still indexed by coordinate on the audio thread (kill-lanes §5: the bus
		// goes when the plan does), so a crossing there still has to be, but it is a crossing of what
		// EXISTS rather than a sweep of what could.
		//
		// AND BOTH ENDS HERE ARE CORE ENDS — a macro and a parameter, indices into Feathers' own table
		// rather than device addresses — so the crossing is the identity and the REQUIRE below is what
		// says so. A device route would need the placement the audio thread compiles against, which is
		// `ParamSet::adoptRoutes`, and no case in this file has one.
		test::Routes routes;
		bool everyEndIsCore = true;
		f.bridge.forEachRoute([&](const backplane::Route& r) {
			if (r.source.isDevice() || r.destination.isDevice()) {
				everyEndIsCore = false;
				return;
			}
			routes.set(r.source, r.destination, r.amount);
		});
		REQUIRE(everyEndIsCore);

		ParamSet params;
		routes.publish(params);
		params.setBase(static_cast<clap_id>(macroParam(0)), 1.0);
		f.bridge.publish(params);
		f.bridge.refresh();

		const ui::Knob::ModRing ring = path->modRing();
		CHECK(ring.drawn);
		CHECK(ring.glow == doctest::Approx(0.8f));
		// THE ARC IS THE ROUTE'S REACH, NOT WHERE THE VALUE IS: unchanged by the macro turning up.
		CHECK(ring.lo == doctest::Approx(path->normalized()));
		CHECK(ring.hi == doctest::Approx(std::clamp(path->normalized() + amount, 0.0f, 1.0f)));
		// AND THE DOT IS WHERE IT IS — above the base, inside the reach.
		REQUIRE(ring.hasMark);
		CHECK(ring.mark > path->normalized());
		CHECK(ring.mark <= ring.hi + 1e-4f);
	}

	SUBCASE("undo walks the amount back, and redo walks it forward") {
		// Into the editor's ONE history — edit_history.h was written so a verb it has never heard of
		// can be recorded by whoever wrote the verb, and this is the first one that took it up.
		REQUIRE(f.gui.history() != nullptr);
		REQUIRE(f.gui.history()->canUndo());
		f.gui.history()->undo();
		CHECK(macroAmount(f.bridge, 0, destination) == 0.0f);
		f.gui.history()->redo();
		CHECK(macroAmount(f.bridge, 0, destination) == doctest::Approx(amount));
	}
}

TEST_CASE("a knob that was never handed the panel's state is exactly the knob it was") {
	// THE ABSENT-FEATURE CASE. Every other editor in this repo — the debug rows, the census tools, the
	// probes — builds knobs without ever mentioning modulation, and none of them should have changed.
	QuietGuiHost host;
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	ui::WidgetRegistry registry;
	auto knob_ptr = paramKnob(bridge, registry, findParam(static_cast<clap_id>(ParamId::Path)), "Path");
	ui::Knob& knob = *knob_ptr;
	knob.setBounds(0.0f, 0.0f, 60.0f, ui::Knob::kHeight);

	bridge.beginRouteGesture();
	bridge.setRouteAmount(macroSourceAt(0), backplane::Destination::core(static_cast<size_t>(modDestinationIndexOf(ParamId::Path))), 1.0f);
	bridge.endRouteGesture();

	CHECK(knob.isModDestination());
	CHECK_FALSE(knob.mappable());
	// A route exists in the patch and this knob still draws no ring, because nothing told it to care.
	CHECK_FALSE(knob.modRing().drawn);
}

TEST_CASE("arming a source lights the filter card's knobs, and a drag on one writes an amount") {
	// THE BUG: modulation could not be assigned to the filter's parameters. It could not,
	// and it was scope rather than breakage — the destination list was the mock's six, drawn up before
	// this device existed, so nothing could be routed to a filter parameter and no knob on this card
	// ever glowed.
	//
	// TWO INDEPENDENT FAILURES MAKE THE SAME PICTURE, which is why this drives the whole gesture rather
	// than asking the list: the parameter has to BE a destination (the list is now derived from the
	// tenant's own declarations), and the card's knobs have to have been handed the panel's state
	// (FilterCard::setModulation). This card was written while neither was true, so its knobs were the
	// only ones in the editor never told — and an untold knob is silently exactly as it was.
	FilterFixture f;
	f.arm(macroSourceAt(0));

	// DRAWN FIRST, AND LEFT SWITCHED OUT — the state a fresh instance opens in, and the state the card
	// greys itself in (the greying is applied in draw). So everything below happens to a GREY card, on
	// the design the dialling case states: a filter you have not switched on is still one you may set
	// up, and that now includes pointing a modulator at it.
	visage::Canvas canvas;
	REQUIRE(f.param(fx::filter::Param::Enabled) < 0.5);
	f.card->draw(canvas);

	// EVERY KNOB ON THIS CARD IS A DESTINATION, which is not a coincidence to be noted but the card's
	// shape: the six knobs are the six continuous controls, and the three the module withholds —
	// Type, Keytrack and Slope — are not knobs at all. It was five until Spread graduated from a
	// macro to a param and took a cell of its own; the count moves with the device's own list of
	// continuous controls, which is what makes it worth asserting rather than merely true.
	REQUIRE(f.card->knobsForTest().size() == 6);
	for (size_t i = 0; i < f.card->knobsForTest().size(); ++i) {
		const auto& knob = f.card->knobsForTest()[i];
		CAPTURE(i);
		CHECK(knob->isModDestination());
		CHECK(knob->mappable());
		CHECK_FALSE(knob->active());
	}

	ui::Knob* cutoff = f.card->knobsForTest().front().get();
	const double before = f.param(fx::filter::Param::Cutoff);
	const visage::Point centre = f.faceCentre(cutoff);
	// THROUGH THE TREE, at the coordinate the layout actually put the knob — the venue lesson: a
	// handler can be perfect and unreachable.
	REQUIRE(f.root->frameAtPoint(centre) == cutoff);
	dragThroughRoot(f.root, centre, { centre.x, centre.y - 35.0f });
	f.settle();

	// THE DESTINATION ASKED OF THE CONTROL, not of the parameter table. `modDestinationIndexOf` takes
	// a ParamId and Cutoff no longer has one; the route lands on the address the DEVICE's control
	// reports, which is the only address it has now.
	//
	// AND ASKED THROUGH THE SAME OBJECT THE DRAG WENT THROUGH, rather than recomputed from the slot
	// and the ordinal here. A restatement would agree with the widget by construction even if both
	// were wrong about which column the route reached; reading it off the control under test is what
	// makes this an assertion about where the amount actually went.
	// **`DeviceControl`, NOT `FxControl`.** The filter card reads its row through the seam now, so its
	// knobs hold the converged adapter — the cast is what tells this case which road it is on, and a
	// stale one fails loudly here rather than silently testing the wrong object.
	auto* control = dynamic_cast<ui::DeviceControl*>(cutoff->control());
	REQUIRE(control != nullptr);
	// AN ENDPOINT (kill-lanes step 2): the filter's own id and its control index — and it is the filter
	// the lane holds, which is what makes this the address the drag went to.
	const backplane::Destination destination = control->destination();
	REQUIRE(destination.isDevice());
	REQUIRE(destination.device == f.bridge.fxDeviceAt(0));
	const float amount = f.bridge.routeAmount(macroSourceAt(0), destination);
	// The seed plus half the drag range, the same arithmetic the tray's knobs answer to: the press
	// creates the mock's 0.4 and 35 of 70 pixels adds another half.
	CHECK(amount == doctest::Approx(ui::Knob::kRouteSeedAmount + 0.5f).epsilon(0.02));

	// AND THE CUTOFF DID NOT MOVE. The whole difference between a routing gesture and a value gesture,
	// on a knob that until now only knew how to do the second one.
	CHECK(f.param(fx::filter::Param::Cutoff) == doctest::Approx(before));

	SUBCASE("and the route is listed under the name the DEVICE gave the parameter") {
		// "Filter Cutoff", not "Cutoff" — the name the device qualified for ITSELF, because a device
		// does not get to assume what else is in the list it lands in.
		//
		// ASKED OF modDestinationName RATHER THAN OF THE PARAMETER TABLE, and the change is the point
		// rather than a detour around a compile error. This used to read
		// `findParam(kModDestinations[row])->name`, which had two assumptions baked into it: that a
		// destination index is a position in the ParamId list, and that a destination has a host
		// parameter at all. A swallowed device's control has neither. `modDestinationName` is the one
		// place all three surfaces — the list, the notifications and the Engine rail — ask, so asking it
		// here tests the road the list actually walks instead of a parallel one that happened to agree.
		auto* routes = findInTree<ui::RouteList>(f.root);
		REQUIRE(routes != nullptr);
		const std::vector<backplane::Destination> rows = routes->rows();
		REQUIRE(rows.size() == 1);
		CHECK(rows.front() == destination);
		// "Filter Cutoff 1" — AND THE NUMBER IS NOT A DEFECT, which is worth pinning rather than
		// trimming. The device names the control and the CHAIN qualifies the device, which is the same
		// division the rack already makes: modDestinationName appends the slot's number whenever the
		// chain it sits in can hold more than one device. The old assertion read "Filter Cutoff" with
		// no number because it was reading the HOST parameter's name, and a host parameter belongs to
		// no slot. A different road gives a different — and more informative — answer.
		const std::string named = ui::modDestinationName(f.bridge, rows.front());
		CHECK(named == "Filter Cutoff 1");
		// The device's half of that name, stated separately, so a change in the chain's numbering
		// cannot quietly take the control's own name with it.
		CHECK(named.starts_with("Filter Cutoff"));

		// AND IT IS NOT NAMING EVERYTHING "Route", which is what that function answers for a slot it
		// cannot resolve — the failure this would otherwise read straight past.
		CHECK(named != "Route");
	}

	SUBCASE("and the ring on the face draws that amount from the base") {
		const ui::Knob::ModRing ring = cutoff->modRing();
		CHECK(ring.drawn);
		CHECK(ring.glow == doctest::Approx(1.0f));
		CHECK(ring.lo == doctest::Approx(cutoff->normalized()));
		CHECK(ring.hi == doctest::Approx(std::clamp(cutoff->normalized() + amount, 0.0f, 1.0f)));
	}
}

TEST_CASE("the modulation halo DIMS with a greyed card (QK, 2026-11-08), and the knob stays armable") {
	// THE DECISION, on a collision the greying and the destination list make together: a knob on
	// a switched-out device is GREY BUT STILL LIVE — mappable() keys off enabled_, not active_, because
	// routing a source to a filter you have not switched on is legitimate for the same reason dialling
	// its cutoff is. But the halo was painted under `if (mappable())` before the face and never
	// consulted the greying, so arming a source lit five full-strength periwinkle glows on an otherwise
	// grey card — and on a DEFAULT patch, since the device ships switched off.
	//
	// DECISION: keep the affordance, take the halo down with active_. A bright glow on a grey card reads
	// as a rendering fault rather than as an invitation.
	//
	// ASKED OF THE PIXELS, because "does it glow" is not a question the widget's own state can answer
	// honestly — mappable() is TRUE in the state this case is about, which is the whole point. Two
	// renders that must be identical, and two that must differ so the first pair cannot pass by
	// everything being identical. Windowless; nothing here opens a window.
	FilterFixture f;
	ui::Knob* cutoff = f.card->knobsForTest().front().get();
	REQUIRE(cutoff != nullptr);
	const int width = static_cast<int>(std::ceil(cutoff->width()));
	const int height = static_cast<int>(std::ceil(cutoff->height()));
	REQUIRE(width > 0);
	REQUIRE(height > 0);

	visage::Canvas canvas;
	markedWindowless(canvas, width, height);
	visage::Region region;
	region.setBounds(0, 0, width, height);
	canvas.addRegion(&region);

	// The card's draw is what tells its controls whether the device is in the signal (showEnablement),
	// so it runs into a throwaway canvas before every capture rather than being assumed to have run.
	visage::Canvas state;
	const auto capture = [&]() {
		f.card->draw(state);
		canvas.setColor(0xff000000);
		canvas.fill(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
		canvas.beginRegion(&region);
		cutoff->draw(canvas);
		canvas.endRegion();
		// THREE CAPTURES INSIDE, and this is not superstition: the windowless renderer hands back a
		// frame that is behind the one just submitted, so a single call reads the PREVIOUS capture.
		// Caught here by a case that passed for the wrong reason: two frames compared equal because
		// both were the frame before them. MARKED as well (backlog IT), and the marker changes colour
		// every capture, so that trap now fails loudly instead of comparing a frame with itself.
		const visage::Screenshot shot = takeMarkedCapture(canvas, width, height);
		std::vector<uint32_t> pixels;
		pixels.reserve(static_cast<size_t>(width) * static_cast<size_t>(height));
		for (int y = 0; y < shot.height(); ++y) {
			for (int x = 0; x < shot.width(); ++x) {
				const visage::Color at = shot.sample(x, y);
				pixels.push_back((static_cast<uint32_t>(at.hexRed()) << 16)
				                 | (static_cast<uint32_t>(at.hexGreen()) << 8)
				                 | static_cast<uint32_t>(at.hexBlue()));
			}
		}
		return pixels;
	};
	const auto differing = [](const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
		REQUIRE(a.size() == b.size());
		size_t count = 0;
		for (size_t i = 0; i < a.size(); ++i) {
			count += a[i] != b[i] ? 1 : 0;
		}
		return count;
	};

	// SWITCHED OUT, which is the state a fresh instance opens in.
	REQUIRE(f.param(fx::filter::Param::Enabled) < 0.5);
	const std::vector<uint32_t> greyAtRest = capture();
	f.arm(macroSourceAt(0));
	const std::vector<uint32_t> greyArmed = capture();

	MESSAGE("grey card, pixels changed by arming = " << differing(greyAtRest, greyArmed));
	// SUPERSEDED 2026-11-08 (backlog QK, by design greyed controls glow dimmer): the grey card's knob DOES glow when
	// armed — dimmer than the live card's, which `energy` below compares. It asserted `== 0` under the earlier decision.
	CHECK(differing(greyAtRest, greyArmed) > 0);
	CHECK(cutoff->haloForTest().lit);
	CHECK(cutoff->haloForTest().dim);

	// AND THE AFFORDANCE SURVIVED IT: the knob is still a legal place to point the armed source.
	CHECK(cutoff->mappable());
	CHECK(cutoff->isModDestination());
	CHECK(cutoff->enabled());
	CHECK_FALSE(cutoff->active());

	// THE CONTROL, and without it the claim above would be satisfied by a knob that never draws
	// anything: switched IN, arming has to make a visible difference.
	f.click(f.card->enableBounds());
	f.settle();
	REQUIRE(f.param(fx::filter::Param::Enabled) >= 0.5);
	const std::vector<uint32_t> litArmed = capture();
	// The same arrow again disarms — the panel's own toggle, so the state this compares against is one
	// a hand can actually be in.
	f.arm(macroSourceAt(0));
	REQUIRE_FALSE(cutoff->mappable());
	const std::vector<uint32_t> litAtRest = capture();

	MESSAGE("live card, pixels changed by arming = " << differing(litArmed, litAtRest));
	CHECK(differing(litArmed, litAtRest) > 0);
	CHECK_FALSE(cutoff->haloForTest().dim);

	// DIMMER, ASKED OF THE PIXELS: the summed per-channel change arming makes on the grey card is less than on the live one.
	const auto energy = [](const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
		long long sum = 0;
		for (size_t i = 0; i < a.size(); ++i) {
			for (int shift : { 0, 8, 16 }) {
				sum += std::abs(static_cast<int>((a[i] >> shift) & 0xff) - static_cast<int>((b[i] >> shift) & 0xff));
			}
		}
		return sum;
	};
	const long long greyEnergy = energy(greyAtRest, greyArmed);
	const long long liveEnergy = energy(litAtRest, litArmed);
	MESSAGE("arming's change: grey card " << greyEnergy << ", live card " << liveEnergy);
	CHECK(greyEnergy > 0);
	CHECK(greyEnergy < liveEnergy);
}

TEST_CASE("a host modulating a macro shows on the macro AND on what the macro feeds") {
	// THE BUG: with a modulated signal routed into one of the internal macros, the value arc on the
	// macro knob did not move, and the values mapped to that macro did not show the modulated
	// value arc from the knock-on internal modulation either.
	//
	// THE ENGINE HAD IT RIGHT AND THE WIDGET DID NOT, which is why it read as a routing bug.
	// ParamSet::sourceValue reads resolved(macro) — base plus whatever the host is modulating IT by —
	// so a host modulator on a macro genuinely reaches that macro's destinations in the audio. What
	// failed was the drawing: Knob::modRing opened by refusing any parameter that is not an internal
	// routing DESTINATION, and a macro is a SOURCE. destinationIndex() is -1 for all four of them, so
	// the macro knob returned an empty ring before reading a single value, whatever the host did.
	//
	// The refusal was right for the REACH arcs, which are drawn from a route's amount and need a
	// destination to have one. It was wrong for the live reading, which is just "where is this
	// parameter actually sitting" — a fair question about anything a host can modulate.
	ParamSet params;
	QuietGuiHost host;
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	WidgetRegistry registry;
	ModState mod;

	// Macro 1 -> the first destination it is allowed to reach, at full amount.
	size_t destination = 0;
	while (destination < kParamDestinationCount
	       && !bridge.mayRoute(macroSourceAt(0), backplane::Destination::core(destination))) {
		++destination;
	}
	REQUIRE(destination < kParamDestinationCount);
	const ParamId macroId = macroParam(0);
	const ParamId destId = kModDestinations[destination];
	test::Routes routes;
	routes.set(test::macro(0), backplane::Destination::core(destination), 1.0f);
	routes.publish(params);
	bridge.setRouteAmount(macroSourceAt(0), backplane::Destination::core(destination), 1.0f);

	auto macroKnob_ptr = paramKnob(bridge, registry, findParam(static_cast<clap_id>(macroId)), "Macro 1", nullptr);
	ui::Knob& macroKnob = *macroKnob_ptr;
	auto destKnob_ptr = paramKnob(bridge, registry, findParam(static_cast<clap_id>(destId)), "Dest", nullptr);
	ui::Knob& destKnob = *destKnob_ptr;
	macroKnob.setModulation(&mod, nullptr);
	destKnob.setModulation(&mod, nullptr);

	bridge.publish(params);
	bridge.refresh();
	const Knob::ModRing macroAtRest = macroKnob.modRing();
	const Knob::ModRing destAtRest = destKnob.modRing();

	// THE HOST MOVES THE MACRO — a param-mod event on the macro's own id, which is exactly what a
	// Bitwig modulator pointed at the macro sends.
	params.setModOffset(static_cast<clap_id>(macroId), 0.5);
	bridge.publish(params);
	bridge.refresh();

	// THE MACRO'S OWN ARC. This is the half that could not happen at all before.
	const Knob::ModRing macroNow = macroKnob.modRing();
	CHECK(macroNow.drawn);
	CHECK_FALSE(macroAtRest.drawn); // and it was NOT drawn at rest, so the ring means something
	// A DOT where the modulator actually put it — asserted as a VALUE against an independent restatement, because
	// "a ring appeared" would pass on a mark anywhere. A macro has no amount of its own, so there is no arc.
	const ParamDesc& macroDesc = *findParam(static_cast<clap_id>(macroId));
	const auto normalise = [](const ParamDesc& d, double v) {
		return static_cast<float>((v - d.min) / (d.max - d.min));
	};
	REQUIRE(macroNow.hasMark);
	CHECK(macroNow.mark == doctest::Approx(normalise(macroDesc, params.resolved(macroId))).epsilon(0.01));
	CHECK_FALSE(macroNow.hi > macroNow.lo);

	// AND THE KNOCK-ON, on the parameter the macro feeds. This half was already working, and checking
	// it is what says so: the destination is a real destination, so it never hit the guard above.
	//
	// THE ARC IS THE REACH AND THE DOT IS THE POSITION: at rest the destination draws the route's REACH (base to
	// base+amount) and no dot; once the source is live the SAME reach stays and a dot appears where the parameter
	// actually IS — so `hi` does not move, and the dot is what says the source is live.
	const Knob::ModRing destNow = destKnob.modRing();
	CHECK(destNow.drawn);
	CHECK(destAtRest.drawn);
	CHECK_FALSE(destAtRest.hasMark);
	CHECK(destNow.glow == doctest::Approx(destAtRest.glow));
	CHECK(destNow.hi == doctest::Approx(destAtRest.hi));
	CHECK(destNow.lo == doctest::Approx(destAtRest.lo));
	const ParamDesc& destDesc = *findParam(static_cast<clap_id>(destId));
	REQUIRE(destNow.hasMark);
	CHECK(destNow.mark == doctest::Approx(normalise(destDesc, params.resolved(destId))).epsilon(0.01));
	CHECK(destNow.lo == doctest::Approx(normalise(destDesc, params.base(destId))).epsilon(0.01));
}

TEST_CASE("every knob in the editor is wired to the modulation state") {
	// THE BUG: the Width, FM and Smooth controls did not glow in mod mode.
	//
	// Gui::create hands the ModState to the tray, the Engine rail and the filter card by NAME, one line
	// each — and the Explore stage was simply not on that list, so its twenty-four per-sound Width,
	// FM Depth and Smoothness knobs had no modulation state at all. No arm glow, no reach arc, and no
	// drag could point a source at them.
	//
	// THE FAILURE IS INVISIBLE FROM OUTSIDE, which is why it survived: a knob with no ModState draws
	// exactly like a knob whose parameter is not a destination — modRing() returns an empty ring in
	// both cases. Nothing about the pixels could tell you which you were looking at.
	//
	// SO THIS WALKS THE TREE RATHER THAN NAMING SURFACES. A case that checked the Explore stage would
	// pin today's bug and catch none of the next one; a hand-kept list of surfaces is the same shape
	// as the hand-kept list in Gui::create that caused it. Every Knob the editor builds, wherever it
	// lives, must have been handed the state.
	QuietGuiHost host;
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	Gui gui(host, bridge);
	REQUIRE(Gui::isAvailable());
	REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));
	visage::Frame* root = gui.root();
	REQUIRE(root != nullptr);

	size_t knobs = 0;
	size_t unwired = 0;
	const auto walk = [&](visage::Frame* frame, auto&& self) -> void {
		if (auto* knob = dynamic_cast<Knob*>(frame); knob != nullptr) {
			++knobs;
			if (!knob->hasModulationForTest()) {
				++unwired;
			}
		}
		for (visage::Frame* child : frame->children()) {
			self(child, self);
		}
	};
	walk(root, walk);

	// A real editor has a great many of them; a walk that found none would pass vacuously.
	CHECK(knobs > 30);
	CHECK(unwired == 0);
}

TEST_CASE("automod's coordinate cells each address their own coordinate, and it is the one with the names") {
	// **THIS CASE USED TO BE ABOUT WHICH HALF WAS WHICH, AND THERE ARE NO HALVES ANY MORE (BJ piece
	// 3).** It asserted that the ZONED control sat in the widget's `fine` slot and the phase counter
	// in the other — and it existed because I had swapped them and shipped it: extracting the seating
	// into `automod_coords.h` I renamed the spec's fields from `{fine, coarse}` to `{coarse, fine}`
	// and did not re-check the call that consumed them positionally, so **every automod box had its
	// halves reversed from `a0e6a25` until this case found it**, in the card and the window, in the
	// installed build.
	//
	// Nothing else caught it: the boxes still drew, still edited two real controls, and the
	// reachability sweep counts WHICH controls a surface reaches, not which slot each sits in.
	//
	// **THE COLLAPSE RETIRES THE BUG AND THE CASE INHERITS WHAT IS LEFT OF THE QUESTION.** One param
	// per coordinate means there is no second slot to transpose it with. What can still go wrong is a
	// cell seated on the WRONG coordinate — row order and spec order disagreeing — and that the
	// control it holds is genuinely the one carrying the names. Both are asserted below, and the
	// second is what stops a cell naming eight regions of something that has none.
	namespace amd = feathers::fx::automod;
	AutomodPanelFixture f;
	REQUIRE(f.panel != nullptr);

	const auto& coords = f.panel->coordsForTest();
	REQUIRE(coords.size() == 3);

	const std::array<amd::Param, 3> want { {
	    amd::Param::Type,
	    amd::Param::Flavor,
	    amd::Param::Mod,
	} };

	for (size_t i = 0; i < coords.size(); ++i) {
		CAPTURE(i);
		const ui::ZoneCoordCell& cell = *coords[i];
		const std::string boxName = cell.box().control().desc().name;
		CAPTURE(boxName);
		CHECK(boxName == amd::kParamDescs[static_cast<size_t>(want[i])].name);

		// **AND THE CONTROL IT HOLDS IS THE ONE WITH THE NAMED REGIONS.** A cell seated on a control
		// declaring no zones would draw a dropdown over a partition that does not exist.
		CHECK(cell.box().control().desc().zones > 0);

		// **THE SELECTOR AND THE BOX ARE THE SAME CONTROL, WHICH IS THE WHOLE POINT OF THE COLLAPSE.**
		// Two widgets over one quantity: the number and the name of where that number is. If these
		// ever address different controls, the caption is describing somewhere the box is not.
		CHECK(&cell.select().control() == &cell.box().control());
		// **AND SINCE backlog IW THE SECOND WIDGET IS A SIGIL, READING THE SAME COORDINATE'S LAP.** Automod's
		// coordinates are Free-only — continuous, no modes — so the dropdown is gone and a mark for the lap is in its
		// room. Each coordinate is driven to a DIFFERENT lap first, so a sigil reading a neighbour's coordinate reads
		// the wrong lap rather than the same default.
		f.f.bridge.setDeviceControl(f.device, static_cast<size_t>(want[i]), 2.4f + static_cast<float>(i));
		REQUIRE(cell.sigil() != nullptr);
		CHECK_FALSE(cell.select().isVisible());
		CHECK(cell.sigil()->lap() == static_cast<int64_t>(2 + i));
		// AND BELOW THE ORIGIN THE LAP FLOORS, it does not truncate: -0.4 is lap -1, not a second lap 0.
		f.f.bridge.setDeviceControl(f.device, static_cast<size_t>(want[i]), -0.4f - static_cast<float>(i));
		CHECK(cell.sigil()->lap() == -1 - static_cast<int64_t>(i));
	}
}

namespace {

/// **A CONTROL WHOSE `ModDisplay` THE CASE DICTATES.** The first one in the repo, and it exists
/// because nothing had ever asserted a mod ring: `modRingForTest` had no callers, which is how "every
/// ring draws a unipolar rest" survived unnoticed.
///
/// Driving the ring through the real route plumbing means arming a source, publishing a rack device
/// and setting routes — three subsystems to exercise one arc's arithmetic, and a failure anywhere in
/// them reads as a failure here. The display IS the ring's whole input; dictating it asks the one
/// question the ring answers.
class FakeModulatable final : public device::Modulatable {
public:
	device::ModDisplay shown {};

	const device::ParamDesc& desc() const override {
		static const device::ParamDesc kDesc { "fake", "Fake", 0.0, 1.0, 0.0, false, device::Unit::Number, {} };
		return kDesc;
	}
	double value() const override { return shown.base; }
	void setValue(double v) override { shown.base = v; }
	device::ModDisplay display() const override { return shown; }
	void setAmount(float) override {}
	void beginGesture() override {}
	void endGesture() override {}
	std::string text() const override { return {}; }
};

} // namespace

TEST_CASE("a bipolar source's mod ring straddles the value; a unipolar one reaches one way") {
	// **`SourceDesc::bipolar` HAD NO ROAD TO ANY SURFACE, AND THAT IS STRONGER THAN THE BACKLOG SAID.**
	// AO recorded "one reader, zero callers". MEASURED: `SourceView::isBipolarAt` was the only reader,
	// nothing called it, and **nothing in the repo constructed a `SourceView` at all** — the type was
	// unbuilt. `sources.h` says the flag exists *"so a surface can draw its rest position"; no surface
	// did. The DAHDSR declares `{ .name = "Envelope", .bipolar = true }` and its ring drew a one-sided
	// reach, exactly as a macro does.
	//
	// **ASKED OF THE RING'S GEOMETRY, NOT OF A RENDER.** The claim is "the arc straddles the value",
	// and a picture of an arc does not say which side of anything it is on.
	FakeModulatable control;
	control.shown.isDestination = true;
	control.shown.armed = true;
	control.shown.base = 0.5;
	control.shown.modulated = 0.5;
	control.shown.amount = 0.3f;

	ui::Knob knob(control, "Fake", nullptr);

	const auto ringWith = [&](bool bipolar) {
		control.shown.bipolar = bipolar;
		return knob.modRingForTest();
	};

	const ui::Knob::ModRing unipolar = ringWith(false);
	const ui::Knob::ModRing bipolar = ringWith(true);

	// **HONEST FAILURE WHEN THERE IS NOTHING TO MEASURE.** A ring case that finds no ring is the
	// empty-filter trap wearing a new hat — it would report success having exercised nothing.
	if (!unipolar.drawn || !bipolar.drawn) {
		MESSAGE("no ring drawn — the polarity claim was not exercised, so this is a failure, not a pass");
		CHECK(false);
		return;
	}

	const float base = 0.5f;
	CAPTURE(unipolar.lo);
	CAPTURE(unipolar.hi);
	CAPTURE(bipolar.lo);
	CAPTURE(bipolar.hi);

	// A UNIPOLAR SOURCE RESTS AT THE VALUE: one edge of the arc IS the current position.
	CHECK(std::fabs(unipolar.lo - base) < 1e-4f);
	CHECK(unipolar.hi > base);

	// **A BIPOLAR ONE STRADDLES IT**, reaching the same distance either side — which is the rest
	// position appearing, and the whole point of the flag.
	CHECK(bipolar.lo < base);
	CHECK(bipolar.hi > base);
	CHECK(base - bipolar.lo == doctest::Approx(bipolar.hi - base).epsilon(0.01));

	// AND THE BIPOLAR ARC IS TWICE THE UNIPOLAR ONE, which a reach that merely moved would not be.
	CHECK(bipolar.hi - bipolar.lo == doctest::Approx(2.0f * (unipolar.hi - unipolar.lo)).epsilon(0.02));
}

TEST_CASE("while assigning, the amount is the span and the displacement is a mark inside it") {
	// THE BUG: in modulation mode, with modulation running, it was hard to see what was going on:
	// the amount arc masked the displacement arc.
	//
	// **IT MASKED BY ONE `return`.** `Knob::modRing`'s armed branch drew the reach and returned before
	// it read the live value at all, so while a route was being assigned — exactly when a hand wants
	// to watch it — the displacement was not merely hidden, it was never computed.
	//
	// **AND LAYERING COULD NOT HAVE FIXED IT.** Both facts were the same `[lo, hi]` arc, in the same
	// colour, at the same radius: ONE channel, so whichever was drawn second replaced the other. The
	// amount keeps the arc, which is the shape of a REACH; the displacement takes a mark, which is the
	// shape of a POSITION.
	FakeModulatable control;
	control.shown.isDestination = true;
	control.shown.armed = true;
	control.shown.bipolar = true;
	control.shown.base = 0.5;
	control.shown.amount = 0.3f;

	ui::Knob knob(control, "Fake", nullptr);

	const auto ringAt = [&](double modulated, float amount) {
		control.shown.modulated = modulated;
		control.shown.amount = amount;
		return knob.modRingForTest();
	};

	// AT REST the mark is absent, and that is not an oversight: a route whose source sits at zero has
	// no displacement to show, and a mark pinned at the base would be a permanent second state drawn
	// on top of the first — the thing `PerformStage::showsModulation`'s 0.001 threshold exists to
	// prevent, and the reason this reuses it rather than inventing one.
	const ui::Knob::ModRing resting = ringAt(0.5, 0.3f);
	REQUIRE(resting.drawn);
	CHECK_FALSE(resting.hasMark);
	CHECK(resting.hi > resting.lo);

	// AND WHILE IT MOVES, BOTH ARE THERE. This is the whole report: the span survives, and the
	// position is now visible inside it.
	const ui::Knob::ModRing moving = ringAt(0.68, 0.3f);
	REQUIRE(moving.drawn);
	CAPTURE(moving.lo);
	CAPTURE(moving.hi);
	CAPTURE(moving.mark);
	CHECK(moving.hasMark);
	CHECK(moving.mark == doctest::Approx(0.68f).epsilon(0.001));
	// THE SPAN IS UNCHANGED BY THE MOVEMENT — it is the reach, not the reading.
	CHECK(moving.lo == doctest::Approx(resting.lo).epsilon(0.001));
	CHECK(moving.hi == doctest::Approx(resting.hi).epsilon(0.001));
	// AND THE MARK IS INSIDE IT, or it is drawing a position the reach says is unreachable.
	CHECK(moving.mark >= moving.lo - 1e-4f);
	CHECK(moving.mark <= moving.hi + 1e-4f);

	SUBCASE("POLARITY: the span cannot show the sign, and the mark can") {
		// THE CONCERN: possible polarity confusion with the amount arc in modulation assign mode.
		// Read as "you cannot TELL the polarity", not as "it points the wrong way" — and
		// MEASURED, that reading is the true one.**
		//
		// For a BIPOLAR source the reach is `[base - a, base + a]`, so `+a` and `-a` describe the
		// SAME SET OF VALUES and draw the identical arc. That is not a drawing bug: the reach really
		// is symmetric, and an arc that differed would be claiming a range the route does not have.
		// The sign is simply not a fact about the reach.
		//
		// **SO THE DIFFERENTIAL CLAUSE FIRST, and it is what makes this not vacuous:** the two
		// displays must DIFFER somewhere, or the surface is lying about a real distinction. Then each
		// direction is asserted, so a build that made them differ in some arbitrary way could not
		// pass. Asserting only a direction would have missed this entirely, because the arcs agree.
		const ui::Knob::ModRing plus = ringAt(0.68, 0.3f);
		const ui::Knob::ModRing minus = ringAt(0.32, -0.3f);

		// THE ARCS AGREE — asserted, because it is the fact that makes the mark necessary.
		CHECK(plus.lo == doctest::Approx(minus.lo).epsilon(0.001));
		CHECK(plus.hi == doctest::Approx(minus.hi).epsilon(0.001));

		// THE DISPLAYS DIFFER, and the mark is where the difference lives.
		REQUIRE(plus.hasMark);
		REQUIRE(minus.hasMark);
		CHECK(std::fabs(plus.mark - minus.mark) > 0.05f);

		// AND EACH ONE IS ON THE SIDE ITS SIGN SAYS: a source driven positive with a positive amount
		// sits ABOVE the base, and with a negative amount BELOW it. This is the clause that would
		// catch an arc drawn backwards; the one above is the clause that catches an arc that cannot
		// be read at all.
		CHECK(plus.mark > 0.5f);
		CHECK(minus.mark < 0.5f);
	}
}

TEST_CASE("a negative amount is stored negative on BOTH seatings — the device road and the synth road") {
	// THE BUG: negative modulation amounts always displayed as positive — narrowed down to effects
	// only, with synth params apparently fine.
	//
	// **THE NARROWING IS REAL AND IT IS NOT IN THE VALUE.** This case pins the half that is settled:
	// a negative amount reaches the route table as a negative number on BOTH roads, so the SOUND is
	// right and nothing downstream of the drag is losing a sign. `ParamSet::rackOffsetAt` and
	// `ParamSet::sourceOffsetAt` both compute `amount * sourceValue * span` off exactly this number.
	//
	// **ASSERTED AS A VALUE, NEVER FROM A PICTURE**, which is the standing rule and is load-bearing
	// here in particular: the two roads DRAW a negative amount differently (see the note at the foot
	// of this case), so any comparison of rendered geometry would have answered a question nobody
	// asked. The claim is "this number is −0.5 at this instant", so the number is what is read.
	//
	// **AND BOTH SEATINGS IN ONE CASE, DELIBERATELY.** `ParamControl` and `DeviceControl` fill the
	// same `device::ModDisplay` from two different tables; a case that walked one of them could not
	// have seen them disagree, which is the shape of every bug this seam has produced. A third
	// seating arriving later is caught by the same assertion.
	namespace dsr = feathers::mod::dahdsr;
	FilterFixture f;
	ui::ModState mod;
	ui::EditHistory history;

	// A REAL BIPOLAR SOURCE, because the polarity question is only asked of one. The DAHDSR declares
	// `{ .name = "Envelope", .bipolar = true }`; a macro is unipolar and would leave the interesting
	// half of the seam unexercised.
	feathers::ModRack rack = f.bridge.modRack();
	rack.count = std::max<size_t>(rack.count, 1);
	rack.setDahdsrAt(0, dsr::Settings {});
	f.bridge.restoreModRack(rack);
	const backplane::Source envelope = backplane::Source::output(f.bridge.rackDeviceAt(0), 0);
	REQUIRE(envelope.device != kNoInstance);
	// THE FIXTURE'S OWN PREMISE, CHECKED. A rack whose envelope failed to install would make every
	// assertion below true of a source that is not the one this case is about.
	REQUIRE(feathers::sourceIsBipolar(f.bridge, envelope));

	mod.focus(envelope);
	mod.arm(true);
	REQUIRE(mod.source() == envelope);

	// THE DEVICE ROAD — the filter's cutoff, addressed by (device, control) with no clap_id at all.
	ui::DeviceControl device { f.deviceHost, f.bridge.fxDeviceAt(0), fx::filter::kDescriptor,
		                       static_cast<size_t>(fx::filter::Param::Cutoff), &mod, &history };
	REQUIRE(device.display().isDestination);

	// THE SYNTH ROAD — a plain host parameter, which is the road it is reported as working.
	const ParamDesc* path = findParam(static_cast<clap_id>(ParamId::Path));
	REQUIRE(path != nullptr);
	ui::ParamControl param { f.bridge, path, &mod, &history };
	REQUIRE(param.display().isDestination);

	constexpr float kNegative = -0.5f;
	device.setAmount(kNegative);
	param.setAmount(kNegative);

	// **THE INDEPENDENT RESTATEMENT: the store, read by an address built here.** The two `display()`
	// calls below are the SURFACES' answer; this is the storage's, reached without going through either
	// seating — the destinations are spelled from the filter's id and the parameter's index rather than
	// asked of the controls. If only the displays were asserted, a seating that read its own cached copy
	// of a sign it had already lost would agree with itself.
	const backplane::Destination deviceDestination = backplane::Destination::control(
	    f.bridge.fxDeviceAt(0), static_cast<size_t>(fx::filter::Param::Cutoff));
	const int paramDestination = modDestinationIndexOf(ParamId::Path);
	REQUIRE(paramDestination >= 0);
	CHECK(f.bridge.routeAmount(envelope, deviceDestination) == doctest::Approx(kNegative));
	CHECK(f.bridge.routeAmount(envelope, backplane::Destination::core(static_cast<size_t>(paramDestination)))
	      == doctest::Approx(kNegative));
	// AND IT IS THE ADDRESS THE DEVICE SEATING REPORTS — so the restatement is about the same route.
	CHECK(device.destination() == deviceDestination);

	// AND WHAT EACH SEATING REPORTS, which is what every widget draws from.
	CHECK(device.display().amount == doctest::Approx(kNegative));
	CHECK(param.display().amount == doctest::Approx(kNegative));
	// THE TWO ROADS AGREE ABOUT THE NUMBER — the clause a third seating inherits.
	CHECK(device.display().amount == doctest::Approx(param.display().amount));

	SUBCASE("and it is a sign rather than a magnitude — the positive of the same size is a different number") {
		// **THE DIFFERENTIAL CLAUSE, AND IT IS NOT DECORATION.** A seating that stored `std::abs` of
		// what it was handed passes every assertion above the moment the amount happens to be
		// positive; asserting only `== -0.5` catches that, but asserting only "a negative amount is
		// stored" would not. So: the same magnitude, the other sign, and the two must differ.
		device.setAmount(-kNegative);
		param.setAmount(-kNegative);
		CHECK(device.display().amount == doctest::Approx(-kNegative));
		CHECK(param.display().amount == doctest::Approx(-kNegative));
		CHECK(device.display().amount != doctest::Approx(kNegative));
		CHECK(param.display().amount != doctest::Approx(kNegative));
	}

	SUBCASE("the two seatings answer the SAME source's polarity differently — the open half of the polarity report") {
		// **THIS IS THE FAULT LINE, AND IT IS NOT IN THE AMOUNT.** Same source, same instant, two
		// seatings:
		//
		//   * `DeviceControl::display()` asks a `backplane::SourceView` and reports the envelope's
		//     declared `bipolar = true`.
		//   * `ParamControl::display()` never assigned `ModDisplay::bipolar` at all, so it defaulted
		//     false; `RackControl::display()` did not either. (Backlog CQ closed that on 2026-09-16:
		//     both ask `sourceIsBipolarNow` now, and for this defaulted envelope the answer is the
		//     same false — see "backlog CQ" below for the switch-ON half on these two roads.)
		//
		// `Knob::modRing` draws a bipolar reach as `[base − a, base + a]` — SYMMETRIC, so +a and −a
		// are the identical arc — and a unipolar one as `[base, base + a]`, which is one-sided and
		// carries the sign. MEASURED on this fixture at base 0.566 with a = ∓0.5: the device knob
		// drew `[0.066, 1]` for BOTH signs while the synth knob drew `[0, 0.5]` against `[0.5, 1]`.
		// That is exactly "works for synth params, effects only".
		//
		// **SETTLED, AND THE CONFLICT DISSOLVED RATHER THAN BEING DECIDED.** This clause was written to
		// hold the disagreement open, and read it as a fight between two documents — `bridge_device_host
		// .h`'s *"the four control seatings that fill a `ModDisplay` cannot disagree" against backlog
		// BF's decision that the symmetric arc is correct. Both were right about their own question. It
		// was ONE FLAG ANSWERING TWO: `SourceDesc::bipolar` is the WIDEST an output can publish, which
		// is what ROUTING must be sized by, and the ring was reading it as what the source is doing
		// NOW. `Env Bipolar` defaults OFF, so the envelope publishes 0..1 and the symmetric arc was
		// claiming a reach nothing could make.
		//
		// **SO THE SEATINGS AGREE HERE NOW, AND THE AGREEMENT IS THE ASSERTION.** Neither was changed
		// to match the other: the device road stopped asking the declaration and started asking
		// `sourceIsBipolarNow`, which for this defaulted envelope answers what the synth road has been
		// answering all along. The case that pins the OTHER half — that the same source with its
		// switch ON still straddles — is the next one in this file, and it is what stops this
		// agreement being reached by making everything unipolar.
		CHECK_FALSE(device.display().bipolar);
		CHECK_FALSE(param.display().bipolar);
		CHECK(device.display().bipolar == param.display().bipolar);

		// AND THE DECLARATION IS UNTOUCHED — the routing question still answers TRUE, which is what
		// keeps this an agreement about DRAWING rather than a descriptor edit that would have
		// re-scaled every route from this envelope.
		CHECK(feathers::sourceIsBipolar(f.bridge, envelope));
	}
}

TEST_CASE("backlog CQ: the synth and rack roads ask the source's CURRENT polarity too — a switched-on envelope at a "
          "synth parameter and a bipolar macro at a rack control straddle the notch instead of reaching one way") {
	// **THE MIRROR OF THE EFFECTS POLARITY BUG, FILED WHEN THAT ONE WAS FIXED.** `ParamControl::display()` and
	// `RackControl::display()` never assigned `ModDisplay::bipolar`, so the synth road and the rack road
	// always drew a one-sided reach. Right by accident while every source they could see was unipolar;
	// wrong — understating the route by HALF — the moment a rack envelope with its switch ON, or a macro
	// in bipolar mode (backlog EG), was routed at one of their controls. MEASURED before the fix
	// (backlog CQ): `sourceIsBipolarNow(envelope) = true`, amount −0.3 landed, `display().bipolar =
	// false`, ring `[0.2, 0.5]` on a base of 0.5. The device road's case above pins the same model; this
	// pins it on the two seatings that had been left out, with the switch moved BOTH ways so a fix that
	// simply set the flag true would fail the first half.
	namespace dsr = feathers::mod::dahdsr;
	FilterFixture f;
	ui::ModState mod;
	ui::EditHistory history;

	feathers::ModRack rack = f.bridge.modRack();
	rack.count = std::max<size_t>(rack.count, 1);
	rack.setDahdsrAt(0, dsr::Settings {});
	f.bridge.restoreModRack(rack);
	const backplane::Source envelope = backplane::Source::output(f.bridge.rackDeviceAt(0), 0);
	REQUIRE(envelope.device != kNoInstance);
	REQUIRE(feathers::sourceIsBipolar(f.bridge, envelope));

	const auto setSwitch = [&](bool on) {
		dsr::Settings env = f.bridge.modRack().dahdsrAt(0);
		dsr::setValueOf(&env, dsr::Param::Bipolar, on ? 1.0f : 0.0f);
		f.bridge.setEnvelopeSettings(0, env);
	};
	constexpr float kAmount = 0.3f;
	const auto clamp01 = [](float v) { return std::clamp(v, 0.0f, 1.0f); };

	// ONE SHAPE OF ASSERTION FOR BOTH ROADS: with the source one-sided, a negative amount hangs below the
	// value and nothing is drawn above it; with the source straddling, the ring is the whole route
	// `[base − a, base + a]` and ±a are the identical arc. Every number restated from the base rather
	// than read off the ring.
	const auto expectOneWay = [&](const ui::Knob::ModRing& ring, float base) {
		REQUIRE(ring.drawn);
		CHECK(ring.hi == doctest::Approx(base).epsilon(0.001));
		CHECK(ring.lo == doctest::Approx(clamp01(base - kAmount)).epsilon(0.01));
	};
	const auto expectStraddle = [&](const ui::Knob::ModRing& minus, const ui::Knob::ModRing& plus, float base) {
		REQUIRE(minus.drawn);
		REQUIRE(plus.drawn);
		CHECK(minus.lo == doctest::Approx(clamp01(base - kAmount)).epsilon(0.01));
		CHECK(minus.hi == doctest::Approx(clamp01(base + kAmount)).epsilon(0.01));
		CHECK(plus.lo == doctest::Approx(minus.lo).epsilon(0.001));
		CHECK(plus.hi == doctest::Approx(minus.hi).epsilon(0.001));
	};

	SUBCASE("THE SYNTH ROAD — a rack envelope routed at Path") {
		mod.focus(envelope);
		mod.arm(true);
		const ParamDesc* path = findParam(static_cast<clap_id>(ParamId::Path));
		REQUIRE(path != nullptr);
		// PATH DEFAULTS TO ITS RAIL, so the engine is told a mid-range base first — through the same
		// publish road the engine takes — or a clamp decides the answer instead of the polarity.
		{
			ParamSet params;
			params.setBase(static_cast<clap_id>(ParamId::Path), 0.5);
			f.bridge.publish(params);
			f.bridge.refresh();
		}
		ui::ParamControl param { f.bridge, path, &mod, &history };
		ui::Knob knob { param, "Path", nullptr };
		REQUIRE(param.display().isDestination);
		const float base = knob.normalized();
		REQUIRE(base > 0.1f);
		REQUIRE(base < 0.9f);

		setSwitch(false);
		REQUIRE_FALSE(feathers::sourceIsBipolarNow(f.bridge, envelope));
		CHECK_FALSE(param.display().bipolar);
		param.setAmount(-kAmount);
		expectOneWay(knob.modRingForTest(), base);

		setSwitch(true);
		REQUIRE(feathers::sourceIsBipolarNow(f.bridge, envelope));
		CHECK(param.display().bipolar);
		const ui::Knob::ModRing minus = knob.modRingForTest();
		param.setAmount(kAmount);
		expectStraddle(minus, knob.modRingForTest(), base);
	}

	SUBCASE("THE RACK ROAD — a macro routed at the envelope's Attack, in unipolar then bipolar mode") {
		const backplane::Source macro = backplane::Source::core(2);
		mod.focus(macro);
		mod.arm(true);
		ui::RackControl attack { f.bridge, f.bridge.rackDeviceAt(0), dsr::Param::Attack, &mod, &history };
		ui::Knob knob { attack, "Attack", nullptr };
		REQUIRE(attack.display().isDestination);
		const float base = knob.normalized();
		CAPTURE(base);

		f.bridge.setMacroBipolar(2, false);
		REQUIRE_FALSE(feathers::sourceIsBipolarNow(f.bridge, macro));
		CHECK_FALSE(attack.display().bipolar);
		attack.setAmount(-kAmount);
		REQUIRE(attack.display().amount == doctest::Approx(-kAmount)); // the route landed, not refused
		expectOneWay(knob.modRingForTest(), base);

		f.bridge.setMacroBipolar(2, true);
		REQUIRE(feathers::sourceIsBipolarNow(f.bridge, macro));
		CHECK(attack.display().bipolar);
		const ui::Knob::ModRing minus = knob.modRingForTest();
		attack.setAmount(kAmount);
		expectStraddle(minus, knob.modRingForTest(), base);
	}
}

TEST_CASE("the ring draws the source's CURRENT polarity — a switched-off envelope reaches one way") {
	// THE REPORT: when the modulator pulls negative, the dot is counter-clockwise of the knob notch;
	// and when the modulation amount is negative, the amount arc goes counter-clockwise of the notch —
	// positive modulation applied negatively pulls below the knob notch.
	//
	// **ONE FLAG WAS ANSWERING TWO QUESTIONS.** `SourceDesc::bipolar` is the WIDEST signal an output
	// can publish, and ROUTING is sized by it deliberately — `dahdsr_types.h` says why, and it is
	// right: *"a host that resized its thinking every time a switch moved would be a host whose route
	// amounts change meaning under the user's hand." But `Env Bipolar` DEFAULTS OFF, so the envelope
	// a user routed publishes 0..1, and a ring drawn from the declaration reached `[base − a, base + a]`
	// — both ways, for a source that can only pull one. That made −a and +a the identical arc, which
	// is "it always displays as positive", and it is why the fault line ran through the effects: the
	// synth road never asks about polarity at all.
	//
	// **BOTH PROPERTIES, IN ONE CASE, AND THE SECOND IS WHY.** A "fix" that simply inverted the flag —
	// or deleted the bipolar branch — passes any test that only checks the one-sided draw. So the same
	// source is asked twice with its own switch moved between, and the symmetric draw must come back.
	namespace dsr = feathers::mod::dahdsr;
	FilterFixture f;
	ui::ModState mod;
	ui::EditHistory history;

	feathers::ModRack rack = f.bridge.modRack();
	rack.count = std::max<size_t>(rack.count, 1);
	rack.setDahdsrAt(0, dsr::Settings {});
	f.bridge.restoreModRack(rack);
	const backplane::Source envelope = backplane::Source::output(f.bridge.rackDeviceAt(0), 0);
	REQUIRE(envelope.device != kNoInstance);

	// **THE DECLARATION IS UNTOUCHED AND THIS SAYS SO.** The whole fix is that drawing stopped reading
	// this; a change that "fixed" the picture by editing the descriptor would break routing's sizing
	// and would pass every assertion below. So the declared answer is pinned as still TRUE.
	REQUIRE(feathers::sourceIsBipolar(f.bridge, envelope));

	mod.focus(envelope);
	mod.arm(true);

	ui::DeviceControl cutoff { f.deviceHost, f.bridge.fxDeviceAt(0), fx::filter::kDescriptor,
		                       static_cast<size_t>(fx::filter::Param::Cutoff), &mod, &history };
	ui::Knob knob { cutoff, "Cutoff", nullptr };
	REQUIRE(cutoff.display().isDestination);

	const auto setSwitch = [&](bool on) {
		dsr::Settings env = f.bridge.modRack().dahdsrAt(0);
		dsr::setValueOf(&env, dsr::Param::Bipolar, on ? 1.0f : 0.0f);
		f.bridge.setEnvelopeSettings(0, env);
	};
	const auto ringWith = [&](float amount) {
		cutoff.setAmount(amount);
		return knob.modRingForTest();
	};

	SUBCASE("switch OFF: the envelope publishes 0..1, so the reach is one-sided and carries the sign") {
		setSwitch(false);
		// THE FIXTURE'S PREMISE, READ RATHER THAN ASSUMED — the switch is what this subcase is about.
		REQUIRE_FALSE(f.deviceHost.sourceIsBipolarNow(envelope));
		CHECK_FALSE(cutoff.display().bipolar);

		const float base = knob.normalized();
		// A BASE OFF BOTH RAILS, or a clamp decides the answer instead of the polarity. Cutoff's
		// default sits mid-sweep; asserting it is what makes the two draws below comparable.
		REQUIRE(base > 0.1f);
		REQUIRE(base < 0.9f);

		const ui::Knob::ModRing minus = ringWith(-0.3f);
		const ui::Knob::ModRing plus = ringWith(0.3f);
		REQUIRE(minus.drawn);
		REQUIRE(plus.drawn);
		CAPTURE(base);
		CAPTURE(minus.lo);
		CAPTURE(minus.hi);
		CAPTURE(plus.lo);
		CAPTURE(plus.hi);

		// **BELOW THE NOTCH, AND THE NOTCH IS AN EDGE.** The report, as geometry: a negative
		// amount reaches down FROM the value, so the value is the top of the arc and nothing is drawn
		// above it.
		CHECK(minus.hi == doctest::Approx(base).epsilon(0.001));
		CHECK(minus.lo < base);
		// A FULL-DEPTH ROUTE MOVES THE CONTROL ITS ROUTE SPAN (`device::routeSpanOf`) — the cutoff's is pinned to the old knob's 9.97
		// octaves since its range widened (Z8), so on the normalised sweep an amount reaches span/(max − min) of itself.
		const feathers::device::ParamDesc& cd = fx::filter::kParamDescs[static_cast<size_t>(fx::filter::Param::Cutoff)];
		const float reach = 0.3f * static_cast<float>(feathers::device::routeSpanOf(cd) / (cd.max - cd.min));
		CHECK(minus.lo == doctest::Approx(base - reach).epsilon(0.01));

		// AND THE POSITIVE ONE REACHES THE OTHER WAY, from the same edge.
		CHECK(plus.lo == doctest::Approx(base).epsilon(0.001));
		CHECK(plus.hi > base);

		// **THE DIFFERENTIAL CLAUSE**: the two arcs are not merely correct one at a time, they are
		// DISTINGUISHABLE — which is the whole of the report. `[lo, hi]` is one visual channel, so
		// this is the assertion that the channel carries the sign at all.
		CHECK(minus.lo != doctest::Approx(plus.lo));
		CHECK(minus.hi != doctest::Approx(plus.hi));
	}

	SUBCASE("switch ON: the same source straddles again, and ±a are identical BY CONSTRUCTION") {
		// **BF IS STILL RIGHT ABOUT A GENUINELY BIPOLAR SOURCE**, and this is the clause that keeps it:
		// *"for a bipolar source the reach is [base − a, base + a], so +a and −a draw the IDENTICAL arc
		// by construction — not a drawing bug; an arc that differed would claim a range the route does
		// not have." The sign is then the MARK's to carry. Nothing above may be achieved by making
		// the ring one-sided always.
		setSwitch(true);
		REQUIRE(f.deviceHost.sourceIsBipolarNow(envelope));
		CHECK(cutoff.display().bipolar);

		const float base = knob.normalized();
		const ui::Knob::ModRing minus = ringWith(-0.3f);
		const ui::Knob::ModRing plus = ringWith(0.3f);
		REQUIRE(minus.drawn);
		REQUIRE(plus.drawn);
		CAPTURE(minus.lo);
		CAPTURE(minus.hi);

		CHECK(minus.lo < base);
		CHECK(minus.hi > base);
		CHECK(base - minus.lo == doctest::Approx(minus.hi - base).epsilon(0.01));
		// IDENTICAL, WHICH IS THE POINT — the reach really is the same set either way round.
		CHECK(minus.lo == doctest::Approx(plus.lo));
		CHECK(minus.hi == doctest::Approx(plus.hi));
	}

	SUBCASE("a source with no polarity switch is unmoved — kPolarityFixed is not 'unipolar'") {
		// **THE TRACKER'S Pitch DECLARES BIPOLAR AND NAMES NO SWITCH**, because a note below the root
		// modulates the other way always. A fix that read "currently bipolar" as "look up a control
		// and default to false when there isn't one" would quietly make it unipolar — one-sided, and
		// wrong in the direction nobody would look at. Asked of the DECLARATIONS, so a device added
		// tomorrow is covered without an exemptions list.
		size_t fixedBipolar = 0;
		for (const device::Descriptor& desc : kDevices) {
			for (const device::SourceDesc& out : desc.outputs) {
				if (out.bipolar && out.bipolarControl == device::kPolarityFixed) {
					++fixedBipolar;
				}
				// A SWITCH THAT IS NAMED MUST EXIST, or the field read below it is somebody else's
				// control. The cheap structural clause that stops a renumbered enum going silent.
				if (out.bipolarControl != device::kPolarityFixed) {
					const std::string who = desc.traits.name != nullptr ? desc.traits.name : "<unnamed>";
					CAPTURE(who);
					CHECK(static_cast<size_t>(out.bipolarControl) < desc.params.size());
					// AND IT MUST BE A SWITCH: a polarity read as `>= 0.5` off a continuous control
					// would be a threshold nobody chose.
					CHECK(desc.params[static_cast<size_t>(out.bipolarControl)].stepped);
				}
			}
		}
		// THE SWEEP MUST HAVE SWEPT SOMETHING — the tracker's Pitch is the witness, and a build that
		// lost it would otherwise pass this subcase by finding nothing to check.
		CAPTURE(fixedBipolar);
		CHECK(fixedBipolar >= 1);
	}
}

TEST_CASE("holding Alt over an armed knob puts its glow out, because the press would edit rather than route") {
	// BACKLOG CU. Alt seemed not to work on Linux, possibly Bitwig's fault. MEASURED 2026-09-06
	// through a real X11 window (tools/alt-probe.cpp): the PRESS road
	// was fine — Alt held, the value moved and the route did not. What was wrong was the GLOW: draw()
	// asked `mappable()` with the default, so an armed knob went on glowing "this press will route"
	// while the press edited. A hand that reads the halo, presses, and sees the value move has every
	// reason to say the bypass "is not working" — the look and the verb disagreed.
	//
	// THE FLAG IS FED BY MOTION, because visage's X11 road translates no modifier keysym — no frame
	// is ever told Alt went down — while every mouse event it delivers carries the server's modifier
	// state. So the hand moving over the knob is what tells the editor, and that is the road this
	// case drives: move events with the bit set and cleared, and the predicate the halo reads.
	ModFixture f;
	ui::Knob* path = f.knobFor(ParamId::Path);
	REQUIRE(path != nullptr);
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);
	// ON THE LIST, so the flag's repaint reaches this knob — the subscription is the only headless
	// witness a redraw has.
	REQUIRE(mod->watches(path));

	f.arm(macroSourceAt(0));
	REQUIRE(path->mappable());
	REQUIRE(path->haloLit());
	REQUIRE_FALSE(mod->bypassed());

	const visage::Point centre = f.faceCentre(path);
	const auto moveAt = [&](bool alt) {
		visage::MouseEvent e;
		e.window_position = centre;
		e.position = centre - path->positionInWindow();
		e.event_frame = path;
		e.button_id = visage::kMouseButtonNone;
		e.button_state = visage::kMouseButtonNone;
		e.is_down = false;
		e.modifiers = alt ? visage::kModifierAlt : 0;
		return e;
	};

	// ALT ARRIVES BY MOTION: the halo goes out, and the gate the press will ask agrees with it.
	path->processMouseMove(moveAt(true));
	CHECK(mod->bypassed());
	CHECK_FALSE(path->haloLit());
	CHECK_FALSE(path->mappable(true));
	// THE DEFAULT IS STILL THE DEFAULT — `mappable()` with no event answers about the gate with no hand
	// on it, which is what the tools and the other cases ask. The halo must not read it; that was the
	// defect.
	CHECK(path->mappable());

	// THE FLIP TOLD THE KNOB, and a flip that does not move tells nobody — ModState's own contract,
	// asserted through its return because a repaint cannot be observed here any other way.
	CHECK(mod->setBypass(false) >= 1);
	CHECK(mod->setBypass(false) == 0);
	CHECK(mod->setBypass(true) >= 1);

	// ALT LEAVES BY MOTION: the halo comes back.
	path->processMouseMove(moveAt(false));
	CHECK_FALSE(mod->bypassed());
	CHECK(path->haloLit());

	SUBCASE("the press carries it too, so a press with no motion before it agrees with its own glow") {
		visage::MouseEvent down = moveAt(true);
		down.button_id = visage::kMouseButtonLeft;
		down.button_state = visage::kMouseButtonLeft;
		down.is_down = true;
		down.repeat_click_count = 1;
		path->processMouseDown(down);
		CHECK(mod->bypassed());
		CHECK_FALSE(path->haloLit());
		visage::MouseEvent up = down;
		up.is_down = false;
		up.button_state = visage::kMouseButtonNone;
		path->processMouseUp(up);
		// AND THE PRESS TOOK THE VALUE ROAD, which is the half the glow now agrees with: no route was
		// made. (The value itself does not move on a press with no travel — a click is only an anchor.)
		CHECK_FALSE(anyRoute(f.bridge));
	}

	SUBCASE("and the exit carries it, so Alt released while leaving a knob is not remembered as held") {
		path->processMouseMove(moveAt(true));
		REQUIRE(mod->bypassed());
		path->processMouseExit(moveAt(false));
		CHECK_FALSE(mod->bypassed());
		CHECK(path->haloLit());
	}
}

namespace {

/// The two answers a test can make the tick's modifier poll give — see Gui::setModifierProbeForTest.
int altHeldProbe() {
	return visage::kModifierAlt;
}
int nothingHeldProbe() {
	return 0;
}
/// A platform with no live modifier read (`Window::hasModifierState()` false) — see Gui::setModifierProbeForTest.
int cannotReportProbe() {
	return -1;
}

} // namespace

TEST_CASE("Alt going down with NO mouse event puts out every armed halo on the next tick, and lifting it brings them all back") {
	// THE BUG: Alt worked better, but the glow's redraw seemed gated on hovering over a knob. It was — the only road a held Alt had to
	// the draw path was the stamp on the next mouse event over a knob, so one knob at a time and
	// only after the pointer moved. This is the road that replaces it: the editor's TICK reads the
	// live modifiers from the platform once a frame (`Window::modifierState()`, our visage patch)
	// and feeds `ModState::setBypass`, which tells every watcher in one notify.
	//
	// NO MOUSE EVENT ANYWHERE IN THIS CASE. That is the whole claim, and it is what separates this
	// road from the motion feed the case above proves: a build with only the motion feed passes that
	// case and fails this one (the known-bad that cuts the tick read while leaving the feed in).
	// The probe stands in for the window's read because a headless Gui has no window to ask.
	ModFixture f;
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);
	f.arm(macroSourceAt(0));

	// EVERY LIT HALO, COLLECTED — "every" needs more than one, or a single-knob feed would pass.
	std::vector<ui::Knob*> knobs;
	collectKnobs(f.root, &knobs);
	std::vector<ui::Knob*> lit;
	for (ui::Knob* knob : knobs) {
		if (knob->haloLit()) {
			lit.push_back(knob);
		}
	}
	CAPTURE(lit.size());
	REQUIRE(lit.size() >= 2);
	REQUIRE_FALSE(mod->bypassed());

	// ALT DOWN, SEEN BY THE TICK ALONE. The count is the notification's own return: at least every
	// lit knob was told, because each is on ModState's list, and a tick that told nobody is exactly
	// the defect.
	f.gui.setModifierProbeForTest(&altHeldProbe);
	const Gui::TickReport down = f.gui.tickForTest();
	CHECK(mod->bypassed());
	CHECK(down.bypassFrames >= lit.size());
	for (ui::Knob* knob : lit) {
		CAPTURE(knob->label());
		CHECK_FALSE(knob->haloLit());
	}
	// ALT STILL DOWN ON THE NEXT FRAME TELLS NOBODY — the poll runs every frame, and a flag that did
	// not move must not repaint the editor sixty times a second.
	CHECK(f.gui.tickForTest().bypassFrames == 0);

	// ALT UP: all of them back, in one tick, again with no mouse event.
	f.gui.setModifierProbeForTest(&nothingHeldProbe);
	const Gui::TickReport up = f.gui.tickForTest();
	CHECK_FALSE(mod->bypassed());
	CHECK(up.bypassFrames >= lit.size());
	for (ui::Knob* knob : lit) {
		CAPTURE(knob->label());
		CHECK(knob->haloLit());
	}

	SUBCASE("with no probe and no window, the tick leaves the flag alone rather than forcing it") {
		// A headless Gui has nothing to read. Forcing false here would have every existing
		// motion-fed case un-fed by the next tick, silently; the poll only writes when it has read.
		f.gui.setModifierProbeForTest(nullptr);
		mod->setBypass(true);
		CHECK(f.gui.tickForTest().bypassFrames == 0);
		CHECK(mod->bypassed());
	}
	f.gui.setModifierProbeForTest(nullptr);
}

TEST_CASE("on a platform that cannot report its modifiers, the tick's poll leaves the motion-fed Alt alone: the halo stays "
          "out while Alt is held, and comes back when it lifts") {
	// THE macOS SHAPE OF THE DEFECT. visage's v1 port kept the live modifier read on X11 alone; elsewhere the base
	// `Window::modifierState()` answers zero, which reads exactly like "Alt is up". The tick polled it anyway, so every
	// frame cleared the bypass that the knob's own mouse events had just set: the glow stayed lit under a held Option
	// while the press (which reads its own event) still bypassed. The poll now runs only where the platform says it
	// can answer (`Window::hasModifierState`); the probe's negative answer stands in for one that cannot.
	ModFixture f;
	ui::Knob* path = f.knobFor(ParamId::Path);
	REQUIRE(path != nullptr);
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);
	f.arm(macroSourceAt(0));
	REQUIRE(path->haloLit());

	f.gui.setModifierProbeForTest(&cannotReportProbe);
	const visage::Point centre = f.faceCentre(path);
	const auto moveAt = [&](bool alt) {
		visage::MouseEvent e;
		e.window_position = centre;
		e.position = centre - path->positionInWindow();
		e.event_frame = path;
		e.button_id = visage::kMouseButtonNone;
		e.button_state = visage::kMouseButtonNone;
		e.is_down = false;
		e.modifiers = alt ? visage::kModifierAlt : 0;
		return e;
	};

	// ALT (OPTION) ARRIVES BY MOTION, the only witness such a platform has.
	path->processMouseMove(moveAt(true));
	REQUIRE(mod->bypassed());
	// AND FRAMES PASS: the poll must not take it away.
	for (int frame = 0; frame < 3; ++frame) {
		CAPTURE(frame);
		CHECK(f.gui.tickForTest().bypassFrames == 0);
		CHECK(mod->bypassed());
		CHECK_FALSE(path->haloLit());
	}
	// ALT LIFTS, by motion again: the halo is back, and stays back through the next frames.
	path->processMouseMove(moveAt(false));
	CHECK_FALSE(mod->bypassed());
	(void)f.gui.tickForTest();
	CHECK(path->haloLit());
	f.gui.setModifierProbeForTest(nullptr);
}

TEST_CASE("the macOS frame rate cap is 60: visage's shared constant, which both of its macOS pacing roads read") {
	// THE ONE PART OF THE CAP A LINUX HOST CAN ASK. The macOS backend sets its CADisplayLink's preferred range
	// (macOS 14+) and its MTKView's preferred rate (below 14) from `Window::kMaxFramesPerSecond`; neither road
	// compiles here, so this pins the number, not that macOS honours it. At 120 the main-thread draw saturated
	// the thread and controls lagged; the cap was dropped once, in the visage v1 port, and restored.
	CHECK(visage::Window::kMaxFramesPerSecond == 60);
}

TEST_CASE("Feathers' own routing rule is asked THROUGH the host: a weight may not reach the puck") {
	// **THE GATE AND THE GLOW ARE ONE QUESTION, AND THE HOST IS WHERE A CARD ASKS IT.**
	// `DeviceHost::mayRoute` is non-virtual precisely so a surface cannot answer it a second way; what a
	// host supplies is its POLICY (`corePolicy`), and Feathers' is the one rule it has about its own axes —
	// a per-sound weight may not reach the puck (`detail::maySourceRouteToIgnoringDevice`).
	//
	// IT WAS PINNED AT `params.h` LEVEL OVER THE WHOLE PAIR SPACE AND NEVER THROUGH THE HOST, so a host
	// answering `permitAll` would have lit a weight's ring on a puck control and offered a drag the write
	// road then refuses — the honesty rule's failure case, and invisible to every existing case.
	ModFixture f;
	const backplane::Source weight = backplane::Source::core(feathers::kFirstWeightSource);
	const backplane::Source macro = macroSourceAt(0);
	REQUIRE(feathers::isWeightSource(weight.index));
	REQUIRE_FALSE(feathers::isWeightSource(macro.index));

	const int path = modDestinationIndexOf(ParamId::Path);
	const int level = modDestinationIndexOf(ParamId::VoiceLevel);
	REQUIRE(path >= 0);
	REQUIRE(level >= 0);
	const backplane::Destination puck = backplane::Destination::core(static_cast<size_t>(path));
	const backplane::Destination elsewhere = backplane::Destination::core(static_cast<size_t>(level));
	REQUIRE(feathers::isPuckDestination(puck.index));
	REQUIRE_FALSE(feathers::isPuckDestination(elsewhere.index));

	// THE RULE.
	CHECK_FALSE(f.deviceHost.mayRoute(weight, puck));
	// AND THE THREE NEIGHBOURS THAT MAKE IT A RULE RATHER THAN A REFUSAL OF EVERYTHING: the same source
	// elsewhere, another source at the same destination, and the ordinary pair.
	CHECK(f.deviceHost.mayRoute(weight, elsewhere));
	CHECK(f.deviceHost.mayRoute(macro, puck));
	CHECK(f.deviceHost.mayRoute(macro, elsewhere));

	// AND THE WRITE ROAD AGREES WITH THE GLOW — one authority, asked twice.
	f.bridge.setRouteAmount(weight, puck, 0.5f);
	CHECK(f.bridge.routeAmount(weight, puck) == 0.0f);
	f.bridge.setRouteAmount(macro, puck, 0.5f);
	CHECK(f.bridge.routeAmount(macro, puck) == doctest::Approx(0.5f));
}

TEST_CASE("backlog IZ: arming SKLITTER's perform output lights the VOICE's Fine Tune and the filter card BEFORE it — and the press writes the route") {
	// *for modulation, "downstream" means COMPUTED FROM DOWNSTREAM AUDIO. A modulator's
	// position in a lane is only grouping.* SKLITTER sits AFTER the filter in the lane and after the voice,
	// and its perform outputs are ticked before either reads a parameter — so both are destinations of it.
	// **THE GLOW (`mappable`, what the halo reads) AND THE AUTHORITY (`mayRoute`) ARE ASKED SEPARATELY**,
	// because the honesty rule is broken the moment a glow and a drag disagree (backlog HM).
	namespace sk = feathers::fx::sklitter;
	FilterFixture f;
	const size_t position = f.bridge.addFxDevice(sk::kTraits.tag);
	REQUIRE(position == 1); // AFTER the filter
	f.settle();
	const DeviceInstanceId skId = f.bridge.fxDeviceAt(position);
	const DeviceInstanceId filterId = f.bridge.fxDeviceAt(0);
	REQUIRE(skId != kNoInstance);
	REQUIRE(filterId != kNoInstance);

	const backplane::Source perfX = backplane::Source::output(skId, sk::outIndex(sk::Out::X));
	const int fineIndex = modDestinationIndexOf(ParamId::FineTune);
	REQUIRE(fineIndex >= 0);
	const backplane::Destination fineTune = backplane::Destination::core(static_cast<size_t>(fineIndex));
	ui::Knob* fine = f.knobFor(ParamId::FineTune);
	REQUIRE(fine != nullptr);
	ui::Knob* cutoff = f.card->knobsForTest().front().get();
	auto* cutoffControl = dynamic_cast<ui::DeviceControl*>(cutoff->control());
	REQUIRE(cutoffControl != nullptr);
	REQUIRE(cutoffControl->destination() == backplane::Destination::control(filterId, static_cast<size_t>(fx::filter::Param::Cutoff)));

	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);
	mod->focus(perfX);
	REQUIRE(mod->arm(true) > 0);
	REQUIRE(mod->armed());

	// THE AUTHORITY.
	CHECK(f.bridge.mayRoute(perfX, fineTune));
	CHECK(f.bridge.mayRoute(perfX, cutoffControl->destination()));
	// THE GLOW.
	CHECK(fine->mappable());
	CHECK(cutoff->mappable());
	// THE CONTROL: the puck the outputs are made of stays dark — the refusal is a rule, not an absence.
	CHECK_FALSE(f.bridge.mayRoute(perfX, backplane::Destination::control(skId, static_cast<size_t>(sk::Param::PerfX))));

	// AND THE PRESS WRITES THE ROUTE THE GLOW PROMISED — through the tree, at the knob.
	const visage::Point centre = f.faceCentre(fine);
	REQUIRE(f.root->frameAtPoint(centre) == fine);
	dragThroughRoot(f.root, centre, { centre.x, centre.y - 35.0f });
	f.settle();
	CHECK(f.bridge.routeAmount(perfX, fineTune) != 0.0f);
}

TEST_CASE("Feathers' host answers a control's right ear from the bridge it was published to, lane and strip alike") {
	// **BACKLOG KU, THE DISPATCH:** `BridgeDeviceHost::modulatedFieldRAt` reaches the strip's telemetry for a strip
	// device and the lane's for a lane device — `modulatedFieldAt`'s own dispatch — and answers false (mono) for any
	// control the published mask leaves out. The values come from the engine in test_mod_rack; this pins the road.
	feathers::ParamBridge bridge;
	feathers::BridgeDeviceHost host { bridge };
	const feathers::DeviceInstanceId filterId = bridge.fxDeviceAt(0);
	const feathers::DeviceInstanceId envelopeId = bridge.rackDeviceAt(0);
	REQUIRE(filterId != feathers::kNoInstance);
	REQUIRE(envelopeId != feathers::kNoInstance);
	float right = 0.0f;
	REQUIRE_FALSE(host.modulatedFieldRAt(filterId, 2, right));
	REQUIRE_FALSE(host.modulatedFieldRAt(envelopeId, 1, right));

	std::array<float, feathers::kMaxDeviceFields> values {};
	values[2] = 0.75f;
	values[3] = 9.0f; // NOT in the mask: must never be read
	feathers::device::ControlMask mask {};
	mask.set(2);
	bridge.publishFxRightEar(bridge.fxSlotOf(filterId), mask, values);
	values[1] = -0.5f;
	feathers::device::ControlMask strip {};
	strip.set(1);
	bridge.publishRackRightEar(bridge.rackSlotOf(envelopeId), strip, values);

	REQUIRE(host.modulatedFieldRAt(filterId, 2, right));
	CHECK(right == 0.75f);
	CHECK_FALSE(host.modulatedFieldRAt(filterId, 3, right));
	REQUIRE(host.modulatedFieldRAt(envelopeId, 1, right));
	CHECK(right == -0.5f);
	CHECK_FALSE(host.modulatedFieldRAt(envelopeId, 2, right));

	// AND AN EMPTY MASK TAKES IT BACK: the route went, the ring is mono again.
	bridge.publishFxRightEar(bridge.fxSlotOf(filterId), {}, values);
	CHECK_FALSE(host.modulatedFieldRAt(filterId, 2, right));
}

TEST_CASE("running, a stereo-live control shows one dot per ear: the left's at the left value, the right's at the right") {
	// **BACKLOG KU, state 1 (not mapping):** the as-modulated position is two DOTS, the same element the armed state
	// uses — not an arc each. Read as values, not a picture.
	FakeModulatable control;
	control.shown.isDestination = true;
	control.shown.stereo = true;
	control.shown.stereoLive = true;
	control.shown.base = 0.5;
	control.shown.modulated = 0.3;
	control.shown.modulatedR = 0.7;
	ui::Knob knob(control, "Fake", nullptr);
	const ui::Knob::ModRing ring = knob.modRingForTest();
	REQUIRE(ring.drawn);
	CHECK(ring.stereo);
	REQUIRE(ring.hasMark);
	REQUIRE(ring.hasMarkR);
	CHECK(std::fabs(ring.mark - 0.3f) < 1e-4f);
	// THE RIGHT EAR'S DOT AT THE RIGHT VALUE — reading the left value here fails.
	CHECK(std::fabs(ring.markR - 0.7f) < 1e-4f);
	// NO AMOUNT, NO ARC: the dots are the whole picture, and no arc stands in for the position.
	CHECK_FALSE(ring.hi > ring.lo);
	// THE CONTROL: the same values with stereoLive false draw one mono dot.
	control.shown.stereoLive = false;
	const ui::Knob::ModRing mono = knob.modRingForTest();
	CHECK_FALSE(mono.stereo);
	CHECK_FALSE(mono.hasMarkR);
	REQUIRE(mono.hasMark);
	CHECK(std::fabs(mono.mark - 0.3f) < 1e-4f);
}

TEST_CASE("the amount arc sits clear of the face arc, and the dot rides it and is wider than it") {
	for (const float diameter : { ui::Knob::kDiameter, 32.0f, 64.0f }) {
		CAPTURE(diameter);
		const ui::ModRingGeometry g = ui::modRingGeometry(diameter);
		// THE FACE TRACK'S OUTER EDGE IS 19 OF 52, and the amount arc starts beyond it by a gap (the keyline's).
		CHECK(std::fabs(g.faceTrackOuter - diameter * (19.0f / ui::Knob::kDiameter)) < 1e-5f);
		CHECK(g.arcInner > g.faceTrackOuter);
		CHECK(std::fabs(g.arcCentre - (g.arcInner + g.arcWidth * 0.5f)) < 1e-5f);
		// INSIDE THE KNOB'S OWN FACE, or the widget would clip it.
		CHECK(g.arcInner + g.arcWidth < diameter * 0.5f);
		CHECK(g.arcCentre + g.dotRadius < diameter * 0.5f);
		// THE DOT STANDS PROUD OF THE ARC IT RIDES.
		CHECK(g.dotRadius * 2.0f > g.arcWidth);
		// THE HOST-SLOT MARK is clear of the arc AND its keyline: the circle of radius `hostMarkRadius` about
		// `hostMarkCentre` starts beyond `arcOuterKeyline`, so no arc at any angle runs under it.
		CHECK(g.arcOuterKeyline >= g.arcInner + g.arcWidth);
		CHECK(g.hostMarkCentre - g.hostMarkRadius > g.arcOuterKeyline);
		// AND IT STAYS INSIDE THE KNOB'S OWN WIDGET BOX: its 45-degree offset plus its radius fits the face's half.
		CHECK(g.hostMarkCentre * 0.70710678f + g.hostMarkRadius <= diameter * 0.5f);
	}
}

TEST_CASE("the amount arc is the reach and the dot is the modulated value, in BOTH assign and normal state") {
	// ONE ELEMENT FOR "WHERE THE VALUE IS", ONE FOR "HOW FAR IT CAN GO". The expectations are stated here by hand
	// (the reach is [min(b, b+a), max(b, b+a)] clamped to the face), not read back from the code under test.
	struct Row {
		const char* what;
		double base;
		float amount;
		bool bipolar;
		double modulated;
		float expectLo;
		float expectHi;
		bool expectOverFace;
	};
	const Row rows[] = {
		{ "base 80%, amount -60%", 0.8, -0.6f, false, 0.5, 0.2f, 0.8f, true },
		{ "base 20%, amount -20% clamped at the floor", 0.2, -0.2f, false, 0.05, 0.0f, 0.2f, true },
		{ "base 50%, bipolar +-30%", 0.5, 0.3f, true, 0.65, 0.2f, 0.8f, true },
		{ "base 20%, amount +30% reaches up, clear of the face arc", 0.2, 0.3f, false, 0.4, 0.2f, 0.5f, false },
	};
	for (const bool armed : { true, false }) {
		for (const Row& row : rows) {
			CAPTURE(armed);
			CAPTURE(row.what);
			FakeModulatable control;
			control.shown.isDestination = true;
			control.shown.routable = armed;
			control.shown.armed = armed;
			control.shown.bipolar = row.bipolar;
			control.shown.base = row.base;
			control.shown.amount = row.amount;
			control.shown.modulated = row.modulated;
			ui::Knob knob(control, "Fake", nullptr);
			const ui::Knob::ModRing ring = knob.modRingForTest();
			REQUIRE(ring.drawn);
			// THE ARC IS THE REACH.
			CHECK(ring.lo == doctest::Approx(row.expectLo).epsilon(1e-4));
			CHECK(ring.hi == doctest::Approx(row.expectHi).epsilon(1e-4));
			CHECK(ring.glow == doctest::Approx(armed ? 1.0f : 0.8f));
			CHECK(ring.overFace == row.expectOverFace);
			// THE DOT IS WHERE THE VALUE IS, in the state it is asked in — and the only element that says so.
			REQUIRE(ring.hasMark);
			CHECK(ring.mark == doctest::Approx(static_cast<float>(row.modulated)).epsilon(1e-4));
			CHECK_FALSE(ring.hasMarkR);
			CHECK_FALSE(ring.stereo);
			// THE ARC DOES NOT MOVE WITH THE VALUE: the same reach with the source parked at the base.
			control.shown.modulated = row.base;
			const ui::Knob::ModRing parked = knob.modRingForTest();
			CHECK(parked.lo == doctest::Approx(ring.lo));
			CHECK(parked.hi == doctest::Approx(ring.hi));
			CHECK_FALSE(parked.hasMark);
		}
	}
}

TEST_CASE("an unmodulated knob draws nothing, and an armed unrouted one draws the glow but no arc and no dot") {
	FakeModulatable control;
	control.shown.base = 0.5;
	control.shown.modulated = 0.5;
	ui::Knob plain(control, "Fake", nullptr);
	const ui::Knob::ModRing none = plain.modRingForTest();
	CHECK_FALSE(none.drawn);
	CHECK_FALSE(none.hasMark);
	CHECK_FALSE(none.hi > none.lo);

	control.shown.isDestination = true;
	control.shown.routable = true;
	control.shown.armed = true;
	control.shown.amount = 0.0f;
	ui::Knob armed(control, "Fake", nullptr);
	const ui::Knob::ModRing ring = armed.modRingForTest();
	CHECK_FALSE(ring.hi > ring.lo);
	CHECK_FALSE(ring.hasMark);
	CHECK_FALSE(ring.routedDot);
	CHECK(armed.haloForTest().lit);
}

namespace {

/// THE SEPARATION METRIC: distance in OKLab (the perceptual space the chrome hue already turns colours in), the
/// Euclidean norm of (dL, da, db) with a = C cos h, b = C sin h. About 0.02 is the smallest step an eye can name, and
/// 0.12 is "plainly two colours" for flat fills the size of an arc or a dot.
double separation(uint32_t x, uint32_t y) {
	const ui::Oklch a = ui::toOklch(x);
	const ui::Oklch b = ui::toOklch(y);
	const double pi = 3.14159265358979323846;
	const double ar = a.h * pi / 180.0;
	const double br = b.h * pi / 180.0;
	const double dL = a.L - b.L;
	const double da = a.C * std::cos(ar) - b.C * std::cos(br);
	const double db = a.C * std::sin(ar) - b.C * std::sin(br);
	return std::sqrt(dL * dL + da * da + db * db);
}

constexpr double kMinSeparation = 0.12;

} // namespace

TEST_CASE("the face arc, the amount arc and the modulated-value dot stay distinct at every chrome and accent hue") {
	// SWEPT THROUGH THE WHOLE RANGE of both hue settings, through the pure palette (`rotatedPalette`) so no global is
	// touched. The face arc reads `kValue`, the amount arc `kModulation`, the dot `kModulatedDot` (the chrome ink).
	double worst = 1.0;
	double worstEar = 1.0;
	for (double chrome = ui::kChromeHueMin; chrome <= ui::kChromeHueMax; chrome += 15.0) {
		for (const double accent : { -180.0, -90.0, -45.0, 0.0, 45.0, 90.0, 180.0 }) {
			const ui::ChromePalette p = ui::rotatedPalette(ui::kChromeBase, chrome, accent);
			CAPTURE(chrome);
			CAPTURE(accent);
			const double faceAmount = separation(p.value, p.modulation);
			const double faceDot = separation(p.value, p.ink);
			const double amountDot = separation(p.modulation, p.ink);
			CHECK(faceAmount >= kMinSeparation);
			CHECK(faceDot >= kMinSeparation);
			CHECK(amountDot >= kMinSeparation);
			worst = std::min({ worst, faceAmount, faceDot, amountDot });
			// A STEREO CONTROL'S DOTS keep their ear colours and wear the dot's bright core: the core must stand out
			// from each ear, since the ears are data and do not turn with the hue.
			for (const uint32_t ear : { ui::kModulationL, ui::kModulationR }) {
				const double core = separation(ear, p.ink);
				CHECK(core >= kMinSeparation);
				worstEar = std::min(worstEar, core);
			}
		}
	}
	MESSAGE("worst pairwise separation over the sweep: " << worst << " (ear core: " << worstEar << ")");
}

TEST_CASE("the selected outline and the destination outline differ in COLOUR at every hue setting, and from the face arc") {
	// `kSelectedOutline` (the chrome ink) against `kModulation` and `kValue`, over the whole of both hue ranges.
	double worst = 1.0;
	for (double chrome = ui::kChromeHueMin; chrome <= ui::kChromeHueMax; chrome += 15.0) {
		for (const double accent : { -180.0, -90.0, -45.0, 0.0, 45.0, 90.0, 180.0 }) {
			const ui::ChromePalette p = ui::rotatedPalette(ui::kChromeBase, chrome, accent);
			CAPTURE(chrome);
			CAPTURE(accent);
			// THE SELECTED STROKE IS THE PALETTE'S `ink`: the same entry `kSelectedOutline` reads.
			const double selDest = separation(p.ink, p.modulation);
			const double selValue = separation(p.ink, p.value);
			const double destValue = separation(p.modulation, p.value);
			CHECK(selDest >= kMinSeparation);
			CHECK(selValue >= kMinSeparation);
			CHECK(destValue >= kMinSeparation);
			worst = std::min({ worst, selDest, selValue, destValue });
		}
	}
	MESSAGE("worst selected/destination/value separation over the sweep: " << worst);
	// AND THE LIVE ROLE IS THAT ENTRY: the constant the widgets read is the ink, not the accent.
	CHECK(ui::kSelectedOutline == ui::kInk);
	CHECK(ui::boxOutlineOf(true, false, false).outerColour == ui::kSelectedOutline);
	CHECK(ui::boxOutlineOf(false, true, false).outerColour == ui::kModulation);
	CHECK(ui::kSelectedOutline != ui::kModulation);
}

TEST_CASE("a selected box and a modulation-destination box differ in STYLE as well as colour, and the glow is assign-only") {
	const ui::BoxOutline none = ui::boxOutlineOf(false, false, false);
	const ui::BoxOutline sel = ui::boxOutlineOf(true, false, false);
	const ui::BoxOutline dest = ui::boxOutlineOf(false, true, false);
	const ui::BoxOutline both = ui::boxOutlineOf(true, true, false);
	CHECK(none.kind == ui::BoxOutlineKind::None);
	CHECK(none.strokes == 0);
	CHECK(sel.kind == ui::BoxOutlineKind::Selected);
	CHECK(dest.kind == ui::BoxOutlineKind::Destination);
	CHECK(both.kind == ui::BoxOutlineKind::Both);
	// SELECTED: ONE accent stroke. DESTINATION: TWO modulation strokes. A colour-blind eye counts strokes.
	CHECK(sel.strokes == 1);
	CHECK(sel.outerColour == ui::kSelectedOutline);
	CHECK(dest.strokes == 2);
	CHECK(dest.outerColour == ui::kModulation);
	CHECK(dest.innerColour == ui::kModulation);
	CHECK(sel.strokes != dest.strokes);
	CHECK(sel.outerColour != dest.outerColour);
	// BOTH draws both, each in its own colour and shape: the selected stroke outside, the destination's line inside.
	CHECK(both.strokes == 2);
	CHECK(both.outerColour == ui::kSelectedOutline);
	CHECK(both.innerColour == ui::kModulation);
	// THE INNER GLOW is assign-mode and destination-only.
	CHECK_FALSE(dest.innerGlow);
	CHECK_FALSE(ui::boxOutlineOf(true, false, true).innerGlow);
	CHECK(ui::boxOutlineOf(false, true, true).innerGlow);
	CHECK(ui::boxOutlineOf(true, true, true).innerGlow);
}

TEST_CASE("the inner glow stays in a band at the edge and never reaches the text of a default-size box") {
	// THE GLOW'S INNER EXTENT comes from the draw plan's own constant; the TEXT is the glyph ink of the row it is drawn
	// in — cap height 0.72 of the font size, centred in its row — which is what the glow must not touch.
	const auto inkTop = [](float rowTop, float rowHeight, float fontSize) {
		return rowTop + (rowHeight - 0.72f * fontSize) * 0.5f;
	};
	// A stacked ValueBox at its default height: the caption row is the nearest text to the top edge, the value row to
	// the bottom (both checked from their own edge).
	const float vbTop = inkTop(ui::ValueBox::kPadY, ui::ValueBox::kLabelHeight, ui::kKnobLabelSize);
	const float vbValueBottom = ui::ValueBox::kHeight - (ui::ValueBox::kPadY + ui::ValueBox::kLabelHeight +
	                                                       inkTop(0.0f, ui::ValueBox::kValueHeight, ui::kKnobReadoutSize) +
	                                                       0.72f * ui::kKnobReadoutSize);
	CHECK(ui::kBoxGlowExtent <= vbTop);
	CHECK(ui::kBoxGlowExtent <= vbValueBottom);
	// THE COMPACT ROW: one row of the box's own height.
	CHECK(ui::kBoxGlowExtent <= inkTop(0.0f, ui::ValueBox::kCompactHeight, ui::kKnobReadoutSize));
	// A SWITCH CHIP centres its caption in its whole height.
	CHECK(ui::kBoxGlowExtent <= inkTop(0.0f, ui::SwitchChip::kHeight, ui::kKnobLabelSize));
	// A FADER PILL at the smallest height the cards give it.
	CHECK(ui::kBoxGlowExtent <= inkTop(0.0f, 20.0f, ui::kKnobLabelSize));
	// AND THE PLAN DRAWS NOTHING FURTHER IN than the extent: its two strokes are at insets 1 and 3, a pixel each.
	CHECK(3.0f + 1.0f <= ui::kBoxGlowExtent);
}

TEST_CASE("ModHandle: the plan for one axis, in both states, through a plot mapping that is not linear") {
	// A LOGARITHMIC AXIS, RESTATED HERE: a handle's plot need not be linear in the value, and the helper must not assume
	// it. coord(v) = 100 * log2(1 + v) over a 0..1 control, so equal steps in the value are unequal on the plot.
	const auto coord = [](double v) { return static_cast<float>(100.0 * std::log2(1.0 + v)); };
	FakeModulatable control;
	control.shown.isDestination = true;
	control.shown.base = 0.4;
	control.shown.amount = 0.3f;
	control.shown.modulated = 0.55;
	ui::ModHandle handle({ ui::ModHandleAxis { &control, true, coord } });
	for (const bool armed : { true, false }) {
		CAPTURE(armed);
		control.shown.armed = armed;
		control.shown.routable = armed;
		const ui::ModHandlePlan plan = handle.plan();
		REQUIRE(plan.axes.size() == 1);
		const ui::ModAxisPlan& a = plan.axes[0];
		REQUIRE(a.offered);
		CHECK(plan.halo == armed);
		// THE STROKE: base to base + amount, each end through the mapping — NOT a linear interpolation between them.
		REQUIRE(a.hasStroke);
		CHECK(a.strokeLo == doctest::Approx(coord(0.4)).epsilon(1e-5));
		CHECK(a.strokeHi == doctest::Approx(coord(0.7)).epsilon(1e-5));
		CHECK(a.glow == doctest::Approx(armed ? 1.0f : 0.8f));
		// THE DOT: at the modulated value, in both states; and the base is where the handle sits.
		REQUIRE(a.hasDot);
		CHECK(a.dot == doctest::Approx(coord(0.55)).epsilon(1e-5));
		CHECK(a.base == doctest::Approx(coord(0.4)).epsilon(1e-5));
	}
	// A NEGATIVE AMOUNT hangs below the base; the stroke clamps to the control's range at the floor.
	control.shown.amount = -0.6f;
	control.shown.modulated = 0.4;
	const ui::ModAxisPlan down = handle.plan().axes[0];
	CHECK(down.strokeLo == doctest::Approx(coord(0.0)).epsilon(1e-5));
	CHECK(down.strokeHi == doctest::Approx(coord(0.4)).epsilon(1e-5));
	CHECK_FALSE(down.hasDot); // the value is at its base
	// A BIPOLAR SOURCE reaches both ways from the base.
	control.shown.amount = 0.2f;
	control.shown.bipolar = true;
	const ui::ModAxisPlan both = handle.plan().axes[0];
	CHECK(both.strokeLo == doctest::Approx(coord(0.2)).epsilon(1e-5));
	CHECK(both.strokeHi == doctest::Approx(coord(0.6)).epsilon(1e-5));
	// STEREO: two dots, each at its own ear.
	control.shown.stereo = true;
	control.shown.stereoLive = true;
	control.shown.modulated = 0.3;
	control.shown.modulatedR = 0.5;
	const ui::ModAxisPlan stereo = handle.plan().axes[0];
	CHECK(stereo.stereo);
	CHECK(stereo.dot == doctest::Approx(coord(0.3)).epsilon(1e-5));
	REQUIRE(stereo.hasDotR);
	CHECK(stereo.dotR == doctest::Approx(coord(0.5)).epsilon(1e-5));
}

TEST_CASE("ModHandle: the honesty rule — a control that is not a destination gets no halo, stroke or dot, and no route") {
	FakeModulatable control;
	control.shown.isDestination = false;
	control.shown.routable = true;
	control.shown.amount = 0.3f;
	control.shown.base = 0.4;
	control.shown.modulated = 0.6;
	ui::ModHandle handle({ ui::ModHandleAxis { &control, true, [](double v) { return static_cast<float>(v * 100.0); } } });
	const ui::ModHandlePlan plan = handle.plan();
	CHECK_FALSE(plan.halo);
	CHECK_FALSE(plan.axes[0].offered);
	CHECK_FALSE(plan.axes[0].hasStroke);
	CHECK_FALSE(plan.axes[0].hasDot);
}

TEST_CASE("ModHandle: two axes each draw their own stroke, and a press takes the axis it is nearer") {
	FakeModulatable cx;
	FakeModulatable cy;
	for (FakeModulatable* c : { &cx, &cy }) {
		c->shown.isDestination = true;
		c->shown.routable = true;
		c->shown.armed = true;
		c->shown.base = 0.5;
		c->shown.modulated = 0.5;
	}
	cx.shown.amount = 0.2f;
	cy.shown.amount = -0.3f;
	ui::ModHandle handle({ ui::ModHandleAxis { &cx, true, [](double v) { return static_cast<float>(10.0 + v * 100.0); } },
	                       ui::ModHandleAxis { &cy, false, [](double v) { return static_cast<float>(110.0 - v * 100.0); } } });
	const ui::ModHandlePlan plan = handle.plan();
	REQUIRE(plan.axes.size() == 2);
	CHECK(plan.axes[0].strokeLo == doctest::Approx(60.0f));
	CHECK(plan.axes[0].strokeHi == doctest::Approx(80.0f));
	CHECK(plan.axes[1].strokeLo == doctest::Approx(60.0f)); // value 0.5 is at y 60, the higher value is the lower y
	CHECK(plan.axes[1].strokeHi == doctest::Approx(90.0f));
	// A press just off the horizontal stroke's line takes X; just off the vertical's takes Y; Alt takes neither.
	CHECK(handle.routeAxisFor(false, { 62.0f, 61.0f }, 60.0f, 60.0f) == 0);
	CHECK(handle.routeAxisFor(false, { 61.0f, 62.0f }, 60.0f, 60.0f) == 1);
	CHECK(handle.routeAxisFor(true, { 62.0f, 61.0f }, 60.0f, 60.0f) == ui::ModHandle::npos);
	// AN UNARMED CONTROL TAKES NO ROUTE PRESS.
	cx.shown.routable = false;
	cy.shown.routable = false;
	CHECK(handle.routeAxisFor(false, { 62.0f, 61.0f }, 60.0f, 60.0f) == ui::ModHandle::npos);
}

TEST_CASE("ModHandle: the grab radius is at least 8 whatever the drawn radius, and the nearest centre wins") {
	CHECK(ui::ModHandle::hitRadius(2.0f) == doctest::Approx(8.0f));
	CHECK(ui::ModHandle::hitRadius(4.0f) == doctest::Approx(8.0f));
	CHECK(ui::ModHandle::hitRadius(12.0f) == doctest::Approx(12.0f));
	const std::array<visage::Point, 2> handles { visage::Point { 100.0f, 50.0f }, visage::Point { 108.0f, 50.0f } };
	// A 3px handle: a press 7px from one is still taken (the grab, not the drawing, decides).
	CHECK(ui::ModHandle::nearest(handles, { 93.0f, 50.0f }, 3.0f) == 0);
	CHECK(ui::ModHandle::nearest(handles, { 115.0f, 50.0f }, 3.0f) == 1);
	// OVERLAPPING GRABS: between the two, the NEARER centre wins — whichever order they are listed in.
	CHECK(ui::ModHandle::nearest(handles, { 103.0f, 50.0f }, 3.0f) == 0);
	CHECK(ui::ModHandle::nearest(handles, { 105.0f, 50.0f }, 3.0f) == 1);
	const std::array<visage::Point, 2> swapped { handles[1], handles[0] };
	CHECK(ui::ModHandle::nearest(swapped, { 103.0f, 50.0f }, 3.0f) == 1);
	CHECK(ui::ModHandle::nearest(swapped, { 105.0f, 50.0f }, 3.0f) == 0);
	// FAR FROM BOTH: nothing is taken.
	CHECK(ui::ModHandle::nearest(handles, { 100.0f, 80.0f }, 3.0f) == ui::ModHandle::npos);
}

TEST_CASE("mapping a stereo-live control keeps one reach arc and puts a dot at each ear, at that ear's value") {
	// **State 2 (armed):** "for the modulation strength we can use the normal arc since its a strength anyway" — one
	// reach arc, not split, and a dot per ear: `mark` at the LEFT ear, `markR` at the RIGHT.
	FakeModulatable control;
	control.shown.isDestination = true;
	control.shown.stereo = true;
	control.shown.stereoLive = true;
	control.shown.armed = true;
	control.shown.base = 0.5;
	control.shown.modulated = 0.3;
	control.shown.modulatedR = 0.7;
	control.shown.amount = 0.1f;
	ui::Knob knob(control, "Fake", nullptr);
	const ui::Knob::ModRing armed = knob.modRingForTest();
	REQUIRE(armed.drawn);
	CHECK(armed.stereo);
	// THE REACH, FROM THE BASE — a strength.
	CHECK(std::fabs(armed.lo - 0.5f) < 1e-4f);
	CHECK(std::fabs(armed.hi - 0.6f) < 1e-4f);
	// TWO MARKS, EACH AT ITS OWN EAR — a right mark reading the left value fails here.
	REQUIRE(armed.hasMark);
	REQUIRE(armed.hasMarkR);
	CHECK(std::fabs(armed.mark - 0.3f) < 1e-4f);
	CHECK(std::fabs(armed.markR - 0.7f) < 1e-4f);
	// EQUAL EARS: the marks coincide.
	control.shown.modulatedR = 0.3;
	CHECK(knob.modRingForTest().markR == knob.modRingForTest().mark);
	// THE CONTROL: mapping a mono-live control — one mark, no right ear.
	control.shown.stereoLive = false;
	CHECK_FALSE(knob.modRingForTest().hasMarkR);
	CHECK_FALSE(knob.modRingForTest().stereo);
}

TEST_CASE("a stereo-capable destination's potential-destination glow takes the stereo tint; a mono one does not") {
	// the glow advertises the CAPABILITY, whatever source is armed.
	FakeModulatable control;
	control.shown.isDestination = true;
	control.shown.armed = true;
	control.shown.routable = true;
	control.shown.stereo = true;
	ui::Knob knob(control, "Fake", nullptr);
	REQUIRE(knob.haloForTest().lit);
	CHECK(knob.haloForTest().stereo);
	control.shown.stereo = false;
	CHECK(knob.haloForTest().lit);
	CHECK_FALSE(knob.haloForTest().stereo);
	// NOT ROUTABLE: no glow at all, so no tint either.
	control.shown.stereo = true;
	control.shown.routable = false;
	CHECK_FALSE(knob.haloForTest().lit);
	CHECK_FALSE(knob.haloForTest().stereo);
}

TEST_CASE("mod ring × stereo: the Phase knob's display carries stereo, stereoLive and the right ear when a PAIR is routed into Phase — and a mono route does not light it") {
	// THE ENGINE STOOD IN BY CELLS (`ParamSet::DeviceCells` is function pointers over void* so a case can): one device,
	// output 0, left 0.3 and right −0.3, and a pair byte this case flips. Everything downstream is the real road —
	// ParamSet's inputs, `ParamBridge::publish`/`refresh`, `ParamControl::display`.
	struct Cells {
		float left = 0.3f;
		float right = -0.3f;
		std::array<float, device::kTickWindowStride> window {};
		std::array<float, device::kTickWindowStride> windowR {};
		uint64_t pairs = 1; // bit 0: output 0 is a pair
	} cells;
	cells.window.fill(cells.left);
	cells.windowR.fill(cells.right);
	cells.window[device::kTickWindow] = 1.0f;
	cells.windowR[device::kTickWindow] = 1.0f;
	constexpr DeviceInstanceId kDevice { 7 };
	ParamSet params;
	constexpr double kBase = 0.1;
	constexpr float kAmount = 0.5f;
	params.setBase(static_cast<clap_id>(ParamId::OscPhase), kBase);
	const int phase = modDestinationIndexOf(ParamId::OscPhase);
	REQUIRE(phase >= 0);
	test::Routes routes;
	routes.set(backplane::Source::output(kDevice, 0), backplane::Destination::core(static_cast<size_t>(phase)), kAmount);
	routes.publish(params);
	ParamSet::DeviceCells where;
	where.host = &cells;
	where.cellOf = [](const void* h, DeviceInstanceId, size_t o) -> const float* {
		return o == 0 ? &static_cast<const Cells*>(h)->left : nullptr;
	};
	where.windowOf = [](const void* h, DeviceInstanceId, size_t o) -> const float* {
		return o == 0 ? static_cast<const Cells*>(h)->window.data() : nullptr;
	};
	where.cellROf = [](const void* h, DeviceInstanceId, size_t o) -> const float* {
		return o == 0 ? &static_cast<const Cells*>(h)->right : nullptr;
	};
	where.windowROf = [](const void* h, DeviceInstanceId, size_t o) -> const float* {
		return o == 0 ? static_cast<const Cells*>(h)->windowR.data() : nullptr;
	};
	where.pairsOf = [](const void* h, DeviceInstanceId, size_t o) -> const uint64_t* {
		return o == 0 ? &static_cast<const Cells*>(h)->pairs : nullptr;
	};
	params.rebuildCoreInputs(where);

	ParamBridge bridge;
	const ParamDesc* desc = findParam(static_cast<clap_id>(ParamId::OscPhase));
	REQUIRE(desc != nullptr);
	ui::ParamControl knob { bridge, desc };
	bridge.publish(params);
	bridge.refresh();
	const device::ModDisplay shown = knob.display();
	// RESTATED: left = base + a·L, right = base + a·R (Phase's span is one cycle).
	CHECK(shown.stereo);
	CHECK(shown.stereoLive);
	CHECK(shown.modulated == doctest::Approx(kBase + kAmount * 0.3).epsilon(1e-6));
	CHECK(shown.modulatedR == doctest::Approx(kBase - kAmount * 0.3).epsilon(1e-6));

	// THE CONTROL: the same route from a source that is NOT a pair now — one ring, both ears at the left's value.
	cells.pairs = 0;
	cells.right = cells.left;
	bridge.publish(params);
	bridge.refresh();
	const device::ModDisplay mono = knob.display();
	CHECK(mono.stereo); // the capability stays: Phase is a stereo destination whatever is routed
	CHECK_FALSE(mono.stereoLive);
	CHECK(mono.modulatedR == mono.modulated);
	// AND A MONO ROW is never stereo.
	const ParamDesc* fine = findParam(static_cast<clap_id>(ParamId::OscPhaseWidth));
	REQUIRE(fine != nullptr);
	CHECK_FALSE(ui::ParamControl { bridge, fine }.display().stereo);
}

TEST_CASE("QK: a greyed knob glows DIMMER for an armed source and takes the route its glow promises, never the value "
          "(greyed controls glow dimmer, by design)") {
	// BOTH KINDS OF GREYED — `setActive(false)` (not in the sound, still takes a hand) and `setEnabled(false)` (takes no
	// hand) — are a real, routable destination, so each GLOWS when a source is armed, at `kGreyedHaloAlpha` rather than the
	// active knob's brightness; and the press that glow invites makes a route. Disarmed, the greyed knob's value press still
	// does nothing (the disabled one), which is the half the decision keeps.
	ModFixture f;
	ui::Knob* path = f.knobFor(ParamId::Path);
	REQUIRE(path != nullptr);
	const int destination = modDestinationIndexOf(ParamId::Path);
	REQUIRE(destination >= 0);
	f.arm(macroSourceAt(0));
	REQUIRE(path->mappable());

	// THE ACTIVE KNOB, the reference brightness: lit, not dim.
	{
		const ui::Knob::Halo h = path->haloForTest();
		CHECK(h.lit);
		CHECK_FALSE(h.dim);
	}
	CHECK(ui::Knob::kGreyedHaloAlpha > 0.0f);
	CHECK(ui::Knob::kGreyedHaloAlpha < 0.55f);

	const auto greyedRoutes = [&](const char* how) {
		CAPTURE(how);
		const ui::Knob::Halo h = path->haloForTest();
		CHECK(h.lit);
		CHECK(h.dim);
		CHECK(path->mappable());
		const double valueBefore = f.bridge.value(static_cast<clap_id>(ParamId::Path));
		const visage::Point centre = f.faceCentre(path);
		dragThroughRoot(f.root, centre, { centre.x, centre.y - 35.0f });
		// THE ROUTE, and a non-zero amount on it: the glow's promise kept.
		CHECK(macroAmount(f.bridge, 0, destination) != 0.0f);
		// AND NOT THE VALUE: a greyed knob's value takes no hand through a routing gesture.
		CHECK(f.bridge.value(static_cast<clap_id>(ParamId::Path)) == valueBefore);
	};

	SUBCASE("inactive (not in the sound)") {
		path->setActive(false);
		greyedRoutes("setActive(false)");
	}
	SUBCASE("disabled (takes no hand)") {
		path->setEnabled(false);
		greyedRoutes("setEnabled(false)");
		SUBCASE("and disarmed, its value press still does nothing") {
			f.arm(macroSourceAt(0)); // the same arrow again disarms
			REQUIRE_FALSE(path->mappable());
			CHECK_FALSE(path->haloForTest().lit);
			const double before = f.bridge.value(static_cast<clap_id>(ParamId::Path));
			const visage::Point centre = f.faceCentre(path);
			dragThroughRoot(f.root, centre, { centre.x, centre.y - 35.0f });
			CHECK(f.bridge.value(static_cast<clap_id>(ParamId::Path)) == before);
		}
	}
}

