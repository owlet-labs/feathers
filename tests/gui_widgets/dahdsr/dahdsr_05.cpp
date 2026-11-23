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

// PIECE 5 OF 12 of the dahdsr suite: its cases in their original order, with the helpers only they use. See dahdsr_common.h.

#include "dahdsr/dahdsr_common.h"

namespace {
/// **"ORDINAL 0" AS AN ENDPOINT** — the first control the device under `id` offers as a destination,
/// named by its own control index (kill-lanes step 2: a destination is a device and its control). Every
/// device with any modulatable control has one, which is why the whole-table cases route through it.
backplane::Destination firstDestinationOf(const ParamBridge& bridge, DeviceInstanceId id) {
	const device::Descriptor* d = bridge.descriptorOf(id);
	REQUIRE(d != nullptr);
	REQUIRE(d->modulatableCount() > 0);
	return backplane::Destination::control(id, d->controlOfNthModulatable(0));
}
/// WHETHER A VERB IS ON A MENU AND ENABLED, looked up by the verb: a row index would make a case about
/// the menu's shape, and both menus' shapes changed when "Add before" joined them (backlog CR).
template <typename Options, typename Verb>
bool offeredVerb(const Options& options, Verb verb) {
	for (const auto& option : options) {
		if (option.verb == verb) {
			return option.enabled;
		}
	}
	return false;
}
} // namespace

TEST_CASE("a menu opened from the bottom of the lane does not open off the window") {
	// THE HAZARD: a popup opening off the window. Both of this lane's menus hang off
	// rows that can sit at the very bottom: the (+) is the last row by construction, and a slot menu
	// belongs to whichever card the hand pressed. A popup that opened downward from there would be
	// hit-testable by arithmetic and unreachable by a hand — the same overflow this lane was already
	// caught by once, one layer up.
	//
	// IT FLIPS ABOVE THE ROW RATHER THAN CLAMPING TO THE EDGE. Clamping would put the menu over the
	// control that opened it, so the row a hand is aiming at is covered by the row it lands on.
	FilterFixture f;
	auto* lane = f.lane;
	REQUIRE(lane != nullptr);

	// WITH THE CANVAS FOLDED: these subcases are about the LANE's foot against the WINDOW's, and the
	// open canvas band puts 480 logical pixels between them, which would leave the flip unreachable.
	(void)f.gui.canvasForTest()->setCollapsed(true);
	// AND THE HOST GRANTING THE FOLDED SIZE — otherwise the window keeps the open canvas's height and
	// the fluid layout hands the spare room to the lane, which is not the geometry these numbers are
	// about (lane top 52 + height 534, window 700 + the strip).
	REQUIRE(f.gui.setSize(Gui::kLogicalWidth, Gui::kLogicalHeight + static_cast<uint32_t>(ui::Rail::kCollapsedWidth)));
	const float windowBottom = f.root->height();
	const float laneTop = lane->positionInWindow().y;
	// **ASKED OF A REAL MENU NOW, not of an estimate.** This case used to question `menuOriginFor`, a
	// pre-flip that guessed the popup's height; the flip is `showMenuAt`'s since backlog GG's placement
	// sweep, done with the size visage measured, so the case opens a menu of `rows` entries off the
	// lane's own anchor and reads where it went.
	const auto openRows = [&](float below, int rows) {
		visage::PopupMenu menu;
		for (int i = 0; i < rows; ++i) {
			menu.addOption(i, "Row");
		}
		// A ROW ENDING AT `below`, the lane's own width — what `menuAnchorFor` hands a head or the (+).
		const visage::Bounds row { 0.0f, below - ui::FxCard::kSlotHeadHeight, lane->width(), ui::FxCard::kSlotHeadHeight };
		REQUIRE(ui::showMenuAt(std::move(menu), *lane, row));
		return ui::lastMenuPlacementForTest();
	};
	constexpr int rows = 3;

	SUBCASE("a row with room below it opens downward") {
		const float near = 20.0f;
		const ui::MenuPlacement placed = openRows(near, rows);
		CHECK(placed.list.y() == doctest::Approx(laneTop + near));
		CHECK(placed.list.bottom() <= windowBottom);
	}

	SUBCASE("a row at the very bottom of the LANE still fits, because the window is taller") {
		// MEASURED, AND IT OVERTURNED WHAT I EXPECTED TO FIND. I flagged this to the team as a likely
		// overflow; at this editor's real geometry it is not one. The lane ends 114px above the
		// window's bottom edge (lane top 52 + height 534, window 700), so a three-row menu opening
		// downward from the lane's LAST pixel still lands inside the window with room to spare.
		//
		// THE CLAIM IS THEREFORE "IT FITS" AND NOT "IT FLIPS". Asserting the mechanism would be
		// asserting a branch that never runs here, and a case that pins an unreachable path is a case
		// that will be deleted by whoever next changes the layout — for the wrong reason.
		const float bottom = lane->height();
		const ui::MenuPlacement placed = openRows(bottom, rows);
		CAPTURE(bottom);
		CAPTURE(placed.list.y());
		CAPTURE(windowBottom - (laneTop + bottom));
		CHECK(placed.list.y() == doctest::Approx(laneTop + bottom)); // no flip needed
		CHECK(placed.list.bottom() <= windowBottom);
	}

	SUBCASE("and a menu too tall for the room below it flips rather than running off") {
		// THE BRANCH THE SUBCASE ABOVE CANNOT REACH, driven with a row count that forces it — because
		// the protection is cheap and the day a card grows or the window shrinks is the day it earns
		// its keep. Four rows fit under the lane's last pixel; eight do not.
		constexpr int kTall = 8;
		const float bottom = lane->height();
		const ui::MenuPlacement placed = openRows(bottom, kTall);
		REQUIRE(laneTop + bottom + placed.list.height() > windowBottom); // or this asks nothing
		CAPTURE(placed.list.y());
		CHECK(placed.list.y() < laneTop + bottom);                     // it went UP
		CHECK(placed.list.bottom() <= windowBottom + 1.0f);             // and it fits
		CHECK(placed.list.y() >= 0.0f);                                 // without leaving the top
		// AND IT SITS ON THE ROW IT HANGS OFF — its bottom at the row's top, not somewhere near the
		// window's top edge, which is where visage's own flip had put it.
		CHECK(placed.list.bottom() == doctest::Approx(placed.anchor.y()));
	}

	SUBCASE("and the (+) on a full chain is one of those rows") {
		// NOT A HYPOTHETICAL. Filled through the button itself and scrolled to the end, which is the
		// configuration a player reaches by using the feature.
		while (lane->deviceCount() < feathers::kMaxFxSlots) {
			REQUIRE(lane->addChoiceForTest(1) < feathers::kMaxFxSlots);
		}
		lane->setScroll(lane->maxScroll());
		const float below = lane->addBounds().bottom();
		const ui::MenuPlacement placed = openRows(below, 2);
		CAPTURE(below);
		CAPTURE(placed.list.y());
		CHECK(placed.list.bottom() <= windowBottom + 1.0f);
	}
}
TEST_CASE("undoing a remove puts the device back with the routings that came out with it") {
	// THE HARD HALF OF THIS FEATURE, AND THE ONE THAT WOULD HAVE LOOKED CORRECT IF IT WERE WRONG.
	//
	// The remove takes routings with the device — that is what makes it safe. So the undo has to put
	// them back, and "put the device back" ALONE would silently drop them: the chain would be right,
	// the card would be right, the count would be right, and a macro the player had assigned would
	// simply have stopped working. Removing a device takes every route naming it, so the amounts are
	// not recoverable from the record afterwards and must have been lifted out first.
	//
	// AND THE ID ITSELF IS PART OF WHAT COMES BACK. The record carries the whole DeviceRow, id
	// included, so an undo puts back the device the routes were saved against. An undo that
	// re-ISSUED would put a new device back, write routes that name the old one, and look entirely
	// correct — until the ids are compared, which is what this case does.
	FilterFixture f;
	auto* lane = f.lane;
	REQUIRE(lane != nullptr);
	REQUIRE(f.gui.history() != nullptr);

	REQUIRE(f.bridge.addFxDevice(fx::harm::kTraits.tag) == 1);
	REQUIRE(f.bridge.addFxDevice(fx::filter::kTraits.tag) == 2);
	lane->resized();

	const backplane::Source kSource = backplane::Source::core(0);
	constexpr size_t kControl = static_cast<size_t>(fx::harm::Param::Level);
	constexpr float kAmount = 0.625f;

	// THE DEVICE THAT WILL BE REMOVED, and a routing INTO it — the thing whose survival is the claim.
	const feathers::DeviceInstanceId doomed = feathers::instanceOfFxSlot(f.bridge.fxChain(), 1);
	REQUIRE(doomed == f.bridge.fxDeviceAt(1));
	const backplane::Destination dest = backplane::Destination::control(doomed, kControl);
	REQUIRE(f.bridge.mayRoute(kSource, dest));
	f.bridge.setRouteAmount(kSource, dest, kAmount);
	// AND ONE INTO THE DEVICE BELOW IT, which must be where it started throughout — the half that
	// catches an undo that moves one device's routes and not the other's.
	const size_t belowControl = static_cast<size_t>(fx::filter::Param::Cutoff);
	const backplane::Destination below = backplane::Destination::control(f.bridge.fxDeviceAt(2), belowControl);
	f.bridge.setRouteAmount(kSource, below, 0.25f);

	// Also give the doomed device a NON-DEFAULT setting, so "the device came back" is a claim about
	// its row and not only about its tag.
	f.bridge.setDeviceControl(doomed, static_cast<size_t>(fx::harm::Param::Width), 0.5f);
	const feathers::FxDevice snapshot = f.bridge.fxChain().device[1];

	REQUIRE(lane->removeDeviceAtLane(1));
	REQUIRE(lane->deviceCount() == 2);
	// THE DEVICE AND ITS ROUTES ARE GONE, and this is where the case's history is worth keeping. A
	// draft written against POSITIONAL addressing asserted 0.25 here, because slot 1's destination
	// space then belonged to the filter that shifted down into it. Nothing shifts now: the doomed
	// device's routes went with it, and the neighbour's amount is untouched on the neighbour.
	REQUIRE(f.bridge.routeAmount(kSource, dest) == doctest::Approx(0.0f));
	REQUIRE(f.bridge.routesOf(doomed).empty());
	REQUIRE(f.bridge.routeAmount(kSource, below) == doctest::Approx(0.25f));

	// UNDO.
	REQUIRE(f.gui.history()->canUndo());
	f.gui.history()->undo();
	lane->resized();

	CHECK(lane->deviceCount() == 3);
	// BACK IN ITS OWN SLOT, not appended at the end.
	CHECK(feathers::instanceOfFxSlot(f.bridge.fxChain(), 1) == doomed);
	// WITH ITS ROW — the whole device, compared field for field, which is what the defaulted
	// operator== on FxDevice is for.
	CHECK(f.bridge.fxChain().device[1] == snapshot);

	// AND WITH ITS ROUTING. This is the assertion the feature exists for.
	// THE SAME ID, NOT MERELY THE SAME SLOT. This is the assertion that separates an undo which
	// restored the record from one which put the device back under a fresh identity: the second would
	// satisfy every other line in this case, and its routes would name nobody.
	CHECK(f.bridge.fxDeviceAt(1) == doomed);
	CHECK(f.bridge.routeAmount(kSource, dest) == doctest::Approx(kAmount));
	// AND THE DEVICE BELOW IT NEVER MOVED — its address is the one it has had all along.
	CHECK(f.bridge.fxDeviceAt(2) == below.device);
	CHECK(f.bridge.routeAmount(kSource, below) == doctest::Approx(0.25f));

	SUBCASE("and redoing takes it out again, routings and all") {
		REQUIRE(f.gui.history()->canRedo());
		f.gui.history()->redo();
		lane->resized();
		CHECK(lane->deviceCount() == 2);
		// SAME AS THE FORWARD DIRECTION: HARM's routes go again and the neighbour's amount stays
		// exactly where it has been throughout, on the neighbour.
		CHECK(f.bridge.routeAmount(kSource, below) == doctest::Approx(0.25f));
		CHECK(f.bridge.routesOf(doomed).empty());
		// AND HARM'S OWN ROUTING IS NOT ANYWHERE. Asked of every route rather than of one address —
		// which is exactly why the amount had to be recorded rather than looked up.
		size_t carrying = 0;
		f.bridge.forEachRoute([&](const backplane::Route& r) {
			if (r.amount == doctest::Approx(kAmount)) {
				++carrying;
			}
		});
		CHECK(carrying == 0);
	}
}
TEST_CASE("undoing an add removes the device, and redoing it keeps the same identity") {
	// THE GAP THAT HAS BEEN STATED SINCE THE RACK'S (+) LANDED. It was never the history that was
	// missing — it was that undoing an add means REMOVING a device, which moves every later routing
	// column, and no road did that. The lane's remove built it.
	FilterFixture f;
	auto* lane = f.lane;
	REQUIRE(lane != nullptr);
	REQUIRE(f.gui.history() != nullptr);
	const size_t before = lane->deviceCount();

	const size_t slot = lane->addChoiceForTest(1);
	REQUIRE(slot == before);
	const feathers::DeviceInstanceId born = feathers::instanceOfFxSlot(f.bridge.fxChain(), slot);
	REQUIRE(born != feathers::kNoInstance);

	REQUIRE(f.gui.history()->canUndo());
	f.gui.history()->undo();
	lane->resized();
	CHECK(lane->deviceCount() == before);
	CHECK(lane->cardAt(slot) == nullptr);

	f.gui.history()->redo();
	lane->resized();
	CHECK(lane->deviceCount() == before + 1);

	// THE SAME DEVICE, NOT A NEW ONE WITH THE SAME TAG. addFxDevice mints a fresh serial every time,
	// so a redo that re-ADDED would hand the player a device with a new identity — and a routing
	// saved in a patch names the serial, so a round trip through undo would quietly orphan it. Redo
	// re-INSERTS the recorded device instead.
	CHECK(feathers::instanceOfFxSlot(f.bridge.fxChain(), slot) == born);

	SUBCASE("and the serial counter never rewinds, so a later add cannot collide with it") {
		// THE ABA RULE. Putting a device back must not make its id available again — nextInstanceId
		// only ever climbs, and an undo that rewound it would let the next add be handed an id a live
		// routing already names.
		const size_t next = lane->addChoiceForTest(1);
		REQUIRE(next < feathers::kMaxFxSlots);
		CHECK(feathers::instanceOfFxSlot(f.bridge.fxChain(), next) != born);
	}
}
TEST_CASE("add before: the slot menu offers it, the pick lands before THAT card, and one undo step takes it out") {
	// BACKLOG CR: the slot menu's 'add before' inserts a new effect into the chain before the effect
	// whose menu was opened. Three things by value: the verb is on the menu
	// and enabled; the device lands at the anchor's position with the anchor one later, its routing
	// intact; and the insert is ONE undo step that removes exactly the newcomer.
	FilterFixture f;
	auto* lane = f.lane;
	REQUIRE(lane != nullptr);
	REQUIRE(f.gui.history() != nullptr);
	REQUIRE(f.bridge.addFxDevice(fx::harm::kTraits.tag) == 1);
	lane->resized();
	REQUIRE(lane->deviceCount() == 2);
	const feathers::DeviceInstanceId harm = feathers::instanceOfFxSlot(f.bridge.fxChain(), 1);
	REQUIRE(harm == f.bridge.fxDeviceAt(1));

	const backplane::Source kSource = backplane::Source::core(0);
	constexpr size_t kControl = static_cast<size_t>(fx::harm::Param::Width);
	const backplane::Destination dest = backplane::Destination::control(harm, kControl);
	REQUIRE(f.bridge.mayRoute(kSource, dest));
	f.bridge.setRouteAmount(kSource, dest, -0.25f);

	// THE VERB IS OFFERED, first on the menu, enabled while the chain has room.
	const std::vector<ui::FxLane::SlotOption> options = lane->slotOptionsForTest(1);
	REQUIRE(!options.empty());
	CHECK(options[0].verb == ui::FxLane::SlotVerb::AddBefore);
	CHECK(options[0].enabled);

	// THE PICK: the second offered device, before the card at position 1 (harm).
	const size_t landed = lane->insertChoiceForTest(1, 1);
	REQUIRE(landed == 1);
	lane->resized();
	CHECK(lane->deviceCount() == 3);
	CHECK(feathers::instanceOfFxSlot(f.bridge.fxChain(), 2) == harm);
	CHECK(f.bridge.fxDeviceAt(2) == harm);
	CHECK(f.bridge.routeAmount(kSource, dest) == doctest::Approx(-0.25f));
	const feathers::DeviceInstanceId born = feathers::instanceOfFxSlot(f.bridge.fxChain(), 1);
	REQUIRE(born != feathers::kNoInstance);
	CHECK(born != harm);

	// ONE STEP OUT, AND THE SAME DEVICE BACK IN AT THE SAME PLACE.
	REQUIRE(f.gui.history()->canUndo());
	f.gui.history()->undo();
	lane->resized();
	CHECK(lane->deviceCount() == 2);
	CHECK(feathers::instanceOfFxSlot(f.bridge.fxChain(), 1) == harm);
	f.gui.history()->redo();
	lane->resized();
	CHECK(lane->deviceCount() == 3);
	CHECK(feathers::instanceOfFxSlot(f.bridge.fxChain(), 1) == born);
	CHECK(feathers::instanceOfFxSlot(f.bridge.fxChain(), 2) == harm);

	SUBCASE("a chain past its starting size still offers the verb, and the pick lands") {
		// IT GREYED HERE until unbounded devices (step 6): a chain is never full now.
		while (lane->deviceCount() < feathers::kMaxFxSlots) {
			REQUIRE(lane->addChoiceForTest(0) != feathers::kNoPosition);
		}
		lane->resized();
		CHECK(lane->slotOptionsForTest(0)[0].enabled);
		CHECK(lane->insertChoiceForTest(0, 0) == 0);
		CHECK(lane->deviceCount() == feathers::kMaxFxSlots + 1);
	}
}
TEST_CASE("add before, on the modulator strip: the pick lands before THAT card and one undo step takes it out") {
	// THE RACK'S HALF OF CR — the same verb on the same menu shape, for the symmetry the entry argued
	// for. Asserted the same way: by the device identities at each position, and by the undo count.
	namespace dsr = feathers::mod::dahdsr;
	ModFixture f;
	auto* panel = findInTree<ui::ModPanel>(f.root);
	REQUIRE(panel != nullptr);
	REQUIRE(f.gui.history() != nullptr);
	REQUIRE(f.bridge.addRackDevice(dsr::kTraits.tag) == 1);
	f.bridge.refresh();
	panel->resized();
	REQUIRE(panel->deviceCount() == 2);
	const feathers::DeviceInstanceId second = f.bridge.modRack().device[1].instanceId;
	const feathers::DeviceInstanceId first = f.bridge.modRack().device[0].instanceId;

	const std::vector<ui::ModPanel::SlotOption> options = panel->slotOptionsForTest(1);
	REQUIRE(!options.empty());
	CHECK(options[0].verb == ui::ModPanel::SlotVerb::AddBefore);
	CHECK(options[0].enabled);

	const size_t landed = panel->insertChoiceForTest(1, 0);
	REQUIRE(landed == 1);
	panel->resized();
	CHECK(panel->deviceCount() == 3);
	CHECK(f.bridge.modRack().device[0].instanceId == first);
	CHECK(f.bridge.modRack().device[2].instanceId == second);
	const feathers::DeviceInstanceId born = f.bridge.modRack().device[1].instanceId;
	CHECK(born != second);
	CHECK(born != first);

	REQUIRE(f.gui.history()->canUndo());
	f.gui.history()->undo();
	panel->resized();
	CHECK(panel->deviceCount() == 2);
	CHECK(f.bridge.modRack().device[1].instanceId == second);
	f.gui.history()->redo();
	panel->resized();
	CHECK(panel->deviceCount() == 3);
	CHECK(f.bridge.modRack().device[1].instanceId == born);
}
TEST_CASE("a looping MSEG mapped onto two fx cards keeps its routes and its reach after the fx lane is reordered between two positions after it") {
	// The defect: with a looped (free-mode) MSEG mapped to fx parameters such as filter cutoff,
	// reordering the audio effects lane lost every route to the audio effects (not to synth params)
	// and the ability to map new working routes to them from the MSEG. The reorder was within the
	// audio effects lane, between two positions both after the modulator, and it disrupted every mod
	// route, not just those to the moved effect.
	//
	// THE BRIDGE ROAD ALONE IS GREEN (test_gui_bridge), so this walks the UI road: the cards, the
	// arm, real amount gestures, the lane's own move verb, and then the rebuilt cards asked BY VALUE.
	namespace flt = fx::filter;
	namespace hrm = fx::harm;
	namespace msg = feathers::mod::mseg;
	FilterFixture f;
	auto* lane = f.lane;
	REQUIRE(lane != nullptr);
	auto* panel = findInTree<ui::ModPanel>(f.root);
	REQUIRE(panel != nullptr);
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);

	// A SECOND FX DEVICE, so there are two positions to swap, both after the modulator strip.
	REQUIRE(f.bridge.addFxDevice(hrm::kTraits.tag) == 1);
	lane->resized();
	REQUIRE(lane->deviceCount() == 2);
	const DeviceInstanceId filterDev = f.bridge.fxDeviceAt(0);
	const DeviceInstanceId harmDev = f.bridge.fxDeviceAt(1);

	// A LOOPING MSEG IN THE RACK — free mode: loop markers set — armed as a hand arms it.
	const size_t msegSlot = f.bridge.addRackDevice(msg::kTraits.tag);
	REQUIRE(msegSlot < feathers::kMaxRackSlots);
	{
		feathers::ModRack rack = f.bridge.modRack();
		msg::Settings looping {};
		looping.loopStart = 0;
		looping.loopEnd = 1;
		rack.setMsegAt(msegSlot, looping);
		f.bridge.restoreModRack(rack);
	}
	f.bridge.refresh();
	panel->resized();
	const backplane::Source source = backplane::Source::output(f.bridge.rackDeviceAt(msegSlot), 0);
	mod->focus(source);
	mod->arm(true);
	REQUIRE(mod->armed());

	// TWO REAL GESTURES: cutoff on the filter, width on the harm.
	const auto destOf = [&](DeviceInstanceId device, size_t control) {
		return backplane::Destination::control(device, control);
	};
	const size_t cutoff = static_cast<size_t>(flt::Param::Cutoff);
	const size_t width = static_cast<size_t>(hrm::Param::Width);
	ui::DeviceControl& cutoffBefore = f.card->control(flt::Param::Cutoff);
	REQUIRE(cutoffBefore.destination() == destOf(filterDev, cutoff));
	cutoffBefore.beginAmountGesture();
	cutoffBefore.setAmount(0.25f);
	cutoffBefore.endAmountGesture();
	// THE HARM'S ROUTE GOES ON THROUGH THE BRIDGE: its knobs do not hand out their adapter, and the
	// filter card above carries every gesture assertion this case needs.
	f.bridge.setRouteAmount(source, destOf(harmDev, width), 0.5f);
	REQUIRE(f.bridge.routeAmount(source, destOf(filterDev, cutoff)) == doctest::Approx(0.25f));
	REQUIRE(f.bridge.routeAmount(source, destOf(harmDev, width)) == doctest::Approx(0.5f));

	// THE REORDER, THROUGH THE LANE'S OWN VERB — the road the drag and the menu end in.
	REQUIRE(lane->moveDevice(0, 1));
	lane->resized();
	REQUIRE(f.bridge.fxDeviceAt(0) == harmDev);
	REQUIRE(f.bridge.fxDeviceAt(1) == filterDev);

	// THE ROUTES ARE STILL THERE, read through the DEVICES.
	CHECK(f.bridge.routeAmount(source, destOf(filterDev, cutoff)) == doctest::Approx(0.25f));
	CHECK(f.bridge.routeAmount(source, destOf(harmDev, width)) == doctest::Approx(0.5f));
	// THE MSEG IS STILL THE ARMED SOURCE.
	CHECK(mod->armed());
	CHECK(mod->source() == source);

	// THE REBUILT CARDS ADDRESS THEIR OWN LANES: the filter now at position 1, the harm at 0.
	auto* filterAfter = dynamic_cast<ui::FilterCard*>(lane->cardAt(1));
	auto* harmAfter = dynamic_cast<ui::HarmCard*>(lane->cardAt(0));
	REQUIRE(filterAfter != nullptr);
	REQUIRE(harmAfter != nullptr);
	CHECK(filterAfter->control(flt::Param::Cutoff).destination() == destOf(filterDev, cutoff));
	// AND THE RULE STILL ADMITS THE MSEG ONTO BOTH.
	CHECK(f.bridge.mayRoute(source, destOf(filterDev, cutoff)));
	CHECK(f.bridge.mayRoute(source, destOf(harmDev, width)));

	// A FRESH GESTURE ON THE MOVED FILTER LANDS ON THE FILTER'S LANE, and the harm's route is untouched.
	ui::DeviceControl& cutoffAfter = filterAfter->control(flt::Param::Cutoff);
	cutoffAfter.beginAmountGesture();
	cutoffAfter.setAmount(0.6f);
	cutoffAfter.endAmountGesture();
	CHECK(f.bridge.routeAmount(source, destOf(filterDev, cutoff)) == doctest::Approx(0.6f));
	CHECK(f.bridge.routeAmount(source, destOf(harmDev, width)) == doctest::Approx(0.5f));
}
TEST_CASE("a looping MSEG mapped onto two fx cards, the steps: filter first of four, dragged to the end by three real drags, an armed looping MSEG routed into two of them — the routes and the cards after every drag") {
	// The reproduction: the lane held OWLTT, HARM, Shaper, Filter, with the filter originally first and
	// reordered to the end by several short drags (the column does not scroll when a dragged effect is
	// held over the bottom of the visible region); no crash.
	//
	// So: four fx devices, the filter at 0 dragged to 3 in three successive drag-drops — each a real
	// gesture on the lane, each rebuilding cards — with the lane scrolled between them. After EVERY
	// drag: the routes on the table by lane, the filter's drawn card addressing its own lane, and the
	// MSEG still admitted onto both routed devices. A single swap through the move verb is green; this
	// is the sequence that is not the same thing.
	namespace flt = fx::filter;
	namespace hrm = fx::harm;
	namespace msg = feathers::mod::mseg;
	FilterFixture f;
	auto* lane = f.lane;
	REQUIRE(lane != nullptr);
	auto* panel = findInTree<ui::ModPanel>(f.root);
	REQUIRE(panel != nullptr);
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);

	// FOUR: filter (already at 0), then owltt, harm, shaper.
	REQUIRE(f.bridge.addFxDevice(fx::owltt::kTraits.tag) == 1);
	REQUIRE(f.bridge.addFxDevice(hrm::kTraits.tag) == 2);
	REQUIRE(f.bridge.addFxDevice(fx::shaper::kTraits.tag) == 3);
	lane->resized();
	REQUIRE(lane->deviceCount() == 4);
	const DeviceInstanceId filterDev = f.bridge.fxDeviceAt(0);
	const DeviceInstanceId harmDev = f.bridge.fxDeviceAt(2);
	const feathers::DeviceInstanceId filterId = feathers::instanceOfFxSlot(f.bridge.fxChain(), 0);

	// A LOOPING MSEG, ARMED.
	const size_t msegSlot = f.bridge.addRackDevice(msg::kTraits.tag);
	REQUIRE(msegSlot < feathers::kMaxRackSlots);
	{
		feathers::ModRack rack = f.bridge.modRack();
		msg::Settings looping {};
		looping.loopStart = 0;
		looping.loopEnd = 1;
		rack.setMsegAt(msegSlot, looping);
		f.bridge.restoreModRack(rack);
	}
	f.bridge.refresh();
	panel->resized();
	const backplane::Source source = backplane::Source::output(f.bridge.rackDeviceAt(msegSlot), 0);
	mod->focus(source);
	mod->arm(true);

	const auto destOf = [&](DeviceInstanceId device, size_t control) {
		return backplane::Destination::control(device, control);
	};
	const size_t cutoff = static_cast<size_t>(flt::Param::Cutoff);
	const size_t width = static_cast<size_t>(hrm::Param::Width);
	// TWO ROUTES, one by a real gesture on the filter's card, one through the bridge.
	ui::DeviceControl& cutoffKnob = f.card->control(flt::Param::Cutoff);
	cutoffKnob.beginAmountGesture();
	cutoffKnob.setAmount(0.25f);
	cutoffKnob.endAmountGesture();
	f.bridge.setRouteAmount(source, destOf(harmDev, width), 0.5f);
	REQUIRE(f.bridge.routeAmount(source, destOf(filterDev, cutoff)) == doctest::Approx(0.25f));
	REQUIRE(f.bridge.routeAmount(source, destOf(harmDev, width)) == doctest::Approx(0.5f));

	// THREE DRAGS, EACH A REAL GESTURE: the filter from p to p + 1.
	for (size_t p = 0; p < 3; ++p) {
		CAPTURE(p);
		REQUIRE(feathers::instanceOfFxSlot(f.bridge.fxChain(), p) == filterId);
		// THE TARGET IN VIEW FIRST — the lane does not scroll under a held drag (the "mini drags"), so
		// the hand scrolls, then drags; the geometry is read AFTER the scroll.
		lane->scrollCardIntoView(std::min<size_t>(p + 2, lane->deviceCount() - 1));
		lane->resized();
		const visage::Bounds card = lane->cardBounds(p);
		lane->cardDragForTest(p, ui::FxCard::DragPhase::Begin, { 20.0f, 4.0f });
		// THE GAP BELOW THE NEXT CARD — index p + 2 among the gaps, which lands the card at p + 1.
		const visage::Bounds bumper = lane->gapBounds(p + 2);
		// CLAMPED INSIDE THE LANE: the gap after the LAST card sits a few pixels below the lane's own
		// bounds, where `dropGapAt` cannot see it — the unreachable bottom was hit (filed separately as
		// the lane not auto-scrolling under a held drag). A hand nudges until it lands; this nudges once.
		const float inLaneY = std::min(bumper.y() + bumper.height() * 0.5f, static_cast<float>(lane->height()) - 1.0f);
		const float inCardY = inLaneY - card.y();
		CAPTURE(card.y());
		CAPTURE(bumper.y());
		CAPTURE(lane->height());
		lane->cardDragForTest(p, ui::FxCard::DragPhase::Move, { 20.0f, inCardY });
		REQUIRE(lane->dropGap().has_value());
		lane->cardDragForTest(p, ui::FxCard::DragPhase::End, { 20.0f, inCardY });
		lane->resized();
		lane->scrollCardIntoView(p + 1);
		lane->releaseRetiredCards();
		REQUIRE(feathers::instanceOfFxSlot(f.bridge.fxChain(), p + 1) == filterId);

		// AFTER THIS DRAG: routes by lane, the filter's DRAWN card addressing its lane, the reach.
		CHECK(f.bridge.routeAmount(source, destOf(filterDev, cutoff)) == doctest::Approx(0.25f));
		CHECK(f.bridge.routeAmount(source, destOf(harmDev, width)) == doctest::Approx(0.5f));
		auto* drawn = dynamic_cast<ui::FilterCard*>(lane->cardAt(p + 1));
		REQUIRE(drawn != nullptr);
		CHECK(drawn->control(flt::Param::Cutoff).destination() == destOf(filterDev, cutoff));
		CHECK(f.bridge.mayRoute(source, destOf(filterDev, cutoff)));
		CHECK(f.bridge.mayRoute(source, destOf(harmDev, width)));
		CHECK(mod->armed());
		CHECK(mod->source() == source);
	}

	// THE FINAL ORDER: owltt harm shaper filter. A FRESH GESTURE ON THE FILTER'S DRAWN CARD LANDS
	// ON THE FILTER'S LANE.
	REQUIRE(feathers::fxHolds(f.bridge.fxChain(), 0, fx::owltt::kTraits.tag));
	REQUIRE(feathers::fxHolds(f.bridge.fxChain(), 3, flt::kTraits.tag));
	auto* filterAtEnd = dynamic_cast<ui::FilterCard*>(lane->cardAt(3));
	REQUIRE(filterAtEnd != nullptr);
	ui::DeviceControl& cutoffAtEnd = filterAtEnd->control(flt::Param::Cutoff);
	cutoffAtEnd.beginAmountGesture();
	cutoffAtEnd.setAmount(0.6f);
	cutoffAtEnd.endAmountGesture();
	CHECK(f.bridge.routeAmount(source, destOf(filterDev, cutoff)) == doctest::Approx(0.6f));
	CHECK(f.bridge.routeAmount(source, destOf(harmDev, width)) == doctest::Approx(0.5f));
}
TEST_CASE("undoing a reorder is the reorder backwards, and the routings follow both ways") {
	// A MOVE IS ITS OWN INVERSE only in the sense that the ROAD is the same one — the ARGUMENTS have
	// to be swapped, and that is the thing this case pins.
	//
	// THREE DEVICES AND A NON-ADJACENT MOVE, deliberately. The first draft used two devices and moved
	// 1 to 0, and a known-bad that made undo repeat the move rather than reverse it stayed GREEN: with
	// exactly two slots a swap is symmetric, so `move(1,0)` twice lands where `move(1,0)` then
	// `move(0,1)` does. A two-element permutation cannot tell a reversal from a repetition — the same
	// shape as a fixture whose neighbours are identical being unable to see a swap between them.
	FilterFixture f;
	auto* lane = f.lane;
	REQUIRE(lane != nullptr);
	REQUIRE(f.bridge.addFxDevice(fx::harm::kTraits.tag) == 1);
	REQUIRE(f.bridge.addFxDevice(fx::filter::kTraits.tag) == 2);
	lane->resized();
	REQUIRE(lane->deviceCount() == 3);

	const backplane::Source kSource = backplane::Source::core(0);
	// BY ORDINAL 0, so every device really carries one whichever kind it is — see the whole-table case
	// for what routing by a fixed control index cost there.
	// THROUGH THE DEVICE NOW AT `slot`, fetched fresh from whichever slot it sits in — so every
	// assertion below is a claim about the DEVICE a route names, which a reorder must not change.
	const auto destOf = [&f](size_t slot) { return firstDestinationOf(f.bridge, f.bridge.fxDeviceAt(slot)); };
	std::array<feathers::DeviceInstanceId, 3> order {};
	for (size_t slot = 0; slot < 3; ++slot) {
		order[slot] = feathers::instanceOfFxSlot(f.bridge.fxChain(), slot);
		f.bridge.setRouteAmount(kSource, destOf(slot), 0.1f * static_cast<float>(slot + 1));
	}

	// THE LAST DEVICE TO THE FRONT — two slots, so the shift has an interior to get wrong.
	REQUIRE(lane->moveDevice(2, 0));
	CHECK(feathers::instanceOfFxSlot(f.bridge.fxChain(), 0) == order[2]);
	CHECK(feathers::instanceOfFxSlot(f.bridge.fxChain(), 1) == order[0]);
	CHECK(feathers::instanceOfFxSlot(f.bridge.fxChain(), 2) == order[1]);
	CHECK(f.bridge.routeAmount(kSource, destOf(0)) == doctest::Approx(0.3f));

	REQUIRE(f.gui.history()->canUndo());
	f.gui.history()->undo();
	lane->resized();

	// EVERY SLOT BACK WHERE IT WAS, by serial — which a repetition of the move would not produce.
	for (size_t slot = 0; slot < 3; ++slot) {
		CAPTURE(slot);
		CHECK(feathers::instanceOfFxSlot(f.bridge.fxChain(), slot) == order[slot]);
		CHECK(f.bridge.routeAmount(kSource, destOf(slot)) == doctest::Approx(0.1f * static_cast<float>(slot + 1)));
	}
	// AND THE CARDS CAME BACK WITH IT.
	CHECK(dynamic_cast<ui::FilterCard*>(lane->cardAt(0)) != nullptr);
	CHECK(dynamic_cast<ui::HarmCard*>(lane->cardAt(1)) != nullptr);

	SUBCASE("and redoing it puts the new order back") {
		REQUIRE(f.gui.history()->canRedo());
		f.gui.history()->redo();
		lane->resized();
		CHECK(feathers::instanceOfFxSlot(f.bridge.fxChain(), 0) == order[2]);
		CHECK(f.bridge.routeAmount(kSource, destOf(0)) == doctest::Approx(0.3f));
	}
}
TEST_CASE("a remove and its undo leave the routing table exactly as they found it") {
	// THE WHOLE-TABLE CLAIM, and the one that catches a remap that is right about the device under
	// test and wrong about something else. Every case above names the destinations it cares about;
	// this one names none and compares every route by identity, so a route lost or moved somewhere
	// nobody was looking is caught by the same assertion.
	//
	// IT IS ALSO THE PIN ON INSERT BEING THE TRUE INVERSE OF REMOVE — that the device a remove takes
	// comes back under the same id, carrying exactly the routes that went out with it and disturbing
	// nobody else's.
	FilterFixture f;
	auto* lane = f.lane;
	REQUIRE(lane != nullptr);
	REQUIRE(f.bridge.addFxDevice(fx::harm::kTraits.tag) == 1);
	REQUIRE(f.bridge.addFxDevice(fx::filter::kTraits.tag) == 2);
	REQUIRE(f.bridge.addFxDevice(fx::harm::kTraits.tag) == 3);
	lane->resized();

	// ROUTES INTO EVERY DEVICE, from two sources, BY ORDINAL rather than by control index.
	//
	// THE FIRST DRAFT ROUTED BY CONTROL INDEX 1 AND SILENTLY ROUTED HALF OF THEM. Filter control 1 is
	// `Type`, a Choice, which is not a modulation destination — so deviceDestinationOfControl refused
	// it, the `if` skipped it, and only the two HARMs ended up carrying anything. The device whose
	// column the forInsertion mutation loses is the one at the removed slot's position, which in that
	// arrangement was a FILTER with no routes at all: the known-bad stayed green because it destroyed
	// an empty column. An ordinal is the device's own numbering of the controls it actually offers,
	// so ordinal 0 exists for every device that has any.
	for (size_t slot = 0; slot < 4; ++slot) {
		const backplane::Destination dest = firstDestinationOf(f.bridge, f.bridge.fxDeviceAt(slot));
		for (size_t source : { size_t {0}, size_t {1} }) {
			f.bridge.setRouteAmount(backplane::Source::core(source), dest,
			                        0.1f * static_cast<float>(slot + 1)
			                            + 0.01f * static_cast<float>(source));
		}
		// NOT VACUOUS, PER SLOT. The whole point of this case is that every slot carries something, so
		// that a route lost anywhere is a route lost somewhere the comparison can see.
		REQUIRE(f.bridge.routeAmount(backplane::Source::core(0), dest) != 0.0f);
	}
	const std::vector<backplane::Route> before = routeListOf(f.bridge);
	const feathers::FxChain chainBefore = f.bridge.fxChain();

	// REMOVE FROM THE MIDDLE, which is where the shift has the most to get wrong.
	REQUIRE(lane->removeDeviceAtLane(1));
	REQUIRE(routeListOf(f.bridge) != before); // or the setup routed nothing

	REQUIRE(f.gui.history()->canUndo());
	f.gui.history()->undo();

	CHECK(f.bridge.fxChain() == chainBefore);
	CHECK(routeListOf(f.bridge) == before);

	SUBCASE("and a reorder does not touch the table at all, which is stronger") {
		// ASSERTED OVER EVERY ROUTE, and this is required assertion #1 of the routing-lane stage. Under
		// positional addressing this line read `!=` — a correct reorder HAD to rewrite the columns, so
		// an unchanged table meant the remap had been forgotten. A route names a device's id, so the
		// correct outcome is now that not one route differs.
		//
		// NOT VACUOUS: the chain really did change, which the serial order below pins, and the table
		// was really non-empty, which the setup's per-slot REQUIRE pins. Both halves are needed —
		// "nothing changed" is trivially true of a move that did not happen and of a table with
		// nothing in it.
		REQUIRE(lane->moveDevice(0, 3));
		REQUIRE(f.bridge.fxChain() != chainBefore);
		CHECK(routeListOf(f.bridge) == before);
		f.gui.history()->undo();
		CHECK(f.bridge.fxChain() == chainBefore);
		CHECK(routeListOf(f.bridge) == before);
	}
}
TEST_CASE("removing a rack device takes the routings OUT of it as well as INTO it") {
	// THE ONE WAY THE RACK DIFFERS FROM THE FX LANE, AND THE HALF NO FX CASE COULD HAVE CAUGHT.
	//
	// In the fx lane a slot is a position in a signal chain — a set of destinations and nothing else,
	// because both devices that may sit there declare `.outputs = {}`. A RACK SLOT IS ALSO A
	// MODULATION SOURCE: a DAHDSR and an MSEG publish outputs, so `deviceSourceOf(slot, output)` is a
	// live address and removing the device drops every route leading OUT of it.
	//
	// THE TRAVERSAL ALWAYS HANDLED THIS — the remap this replaced walked sources beside destinations,
	// and so does the clear that replaced it. What did not was the UNDO's capture, which held only the
	// destination side and was complete for the lane it was written for. A helper is only as general
	// as the first thing that used it.
	ModFixture f;
	auto* panel = f.panel;
	REQUIRE(panel != nullptr);
	REQUIRE(f.gui.history() != nullptr);

	// Three devices, so the middle one has a neighbour below to be shifted.
	while (panel->deviceCount() < 3) {
		REQUIRE(panel->addChoiceForTest(0) < feathers::kMaxRackSlots);
	}
	REQUIRE(panel->deviceCount() == 3);

	// BOTH ENDS THROUGH THE DEVICE AT THE SLOT, read from the slot it currently occupies.
	const auto sourceOf = [&f](size_t slot) { return backplane::Source::output(f.bridge.rackDeviceAt(slot), 0); };
	const auto destOf = [&f](size_t slot) { return firstDestinationOf(f.bridge, f.bridge.rackDeviceAt(slot)); };
	constexpr float kInto = 0.4f;
	constexpr float kOutOf = -0.3f;
	const backplane::Source kMacro = backplane::Source::core(0);
	// A PARAMETER DESTINATION, so the out-of route points somewhere that does NOT move when the rack
	// does — otherwise both ends would shift together and the case could not tell a source remap from
	// a destination one.
	//
	// THROUGH modDestinationIndexOf AND NOT THE RAW ParamId. A ParamId is an identifier; a destination
	// is an index into the routing table's second axis, and they are different numbers. Casting one to
	// the other produced an out-of-table address that setRouteAmount correctly refused — so the route
	// was never made and the case failed on its own setup.
	const int fixedIndex = feathers::modDestinationIndexOf(ParamId::FineTune);
	REQUIRE(fixedIndex >= 0);
	const backplane::Destination fixedDest = backplane::Destination::core(static_cast<size_t>(fixedIndex));

	// INTO the middle device's control, and OUT OF the middle device's output.
	f.bridge.setRouteAmount(kMacro, destOf(1), kInto);
	f.bridge.setRouteAmount(sourceOf(1), fixedDest, kOutOf);
	// And one into the device BELOW it, which must shift and shift back.
	f.bridge.setRouteAmount(kMacro, destOf(2), 0.2f);
	REQUIRE(f.bridge.routeAmount(sourceOf(1), fixedDest) == doctest::Approx(kOutOf));

	const feathers::ModRack before = f.bridge.modRack();
	const std::vector<backplane::Route> routesBefore = routeListOf(f.bridge);

	// THE MIDDLE DEVICE, named while it is still there.
	const DeviceInstanceId doomed = f.bridge.rackDeviceAt(1);
	REQUIRE(doomed != kNoInstance);

	REQUIRE(panel->removeDeviceAtLane(1));
	CHECK(panel->deviceCount() == 2);
	// BOTH DIRECTIONS WENT, asserted of the DEVICE rather than of a position. The out-of route is the
	// one a destination-only clear would have left behind — a modulator's output feeding somebody's
	// Fine Tune with no card anywhere saying so.
	CHECK(f.bridge.routesOf(doomed).empty());
	// And the device below did not move at all — its own route is where it always was.
	CHECK(f.bridge.routeAmount(kMacro, destOf(1)) == doctest::Approx(0.2f));

	SUBCASE("and the undo brings both directions back") {
		REQUIRE(f.gui.history()->canUndo());
		f.gui.history()->undo();

		CHECK(panel->deviceCount() == 3);
		CHECK(f.bridge.modRack().device[1] == before.device[1]);
		// THE ASSERTION THIS WHOLE CASE EXISTS FOR: the route OUT of the restored device is back.
		CHECK(f.bridge.routeAmount(sourceOf(1), fixedDest) == doctest::Approx(kOutOf));
		CHECK(f.bridge.routeAmount(kMacro, destOf(1)) == doctest::Approx(kInto));
		CHECK(f.bridge.routeAmount(kMacro, destOf(2)) == doctest::Approx(0.2f));

		// AND EVERY ROUTE, so one moved somewhere nobody named is caught too.
		CHECK(routeListOf(f.bridge) == routesBefore);
	}

	SUBCASE("and a reorder moves both directions, and its undo puts them back") {
		f.gui.history()->undo(); // put the removed one back first
		REQUIRE(panel->deviceCount() == 3);

		REQUIRE(panel->moveDevice(2, 0));
		// THE DEVICE THAT WAS AT SLOT 1 IS NOW AT SLOT 2, and its outgoing route still names it —
		// which sourceOf(2) now reads, because the id moved with the device.
		CHECK(f.bridge.routeAmount(sourceOf(2), fixedDest) == doctest::Approx(kOutOf));
		// AND THE ROUTES THEMSELVES NEVER MOVED. A reorder writes no route here (none runs backwards),
		// so the whole comparison holds across the move as well as across its undo — which is a
		// stronger claim than the old design could make and is exactly what this stage bought.
		CHECK(routeListOf(f.bridge) == routesBefore);
		f.gui.history()->undo();
		CHECK(f.bridge.routeAmount(sourceOf(1), fixedDest) == doctest::Approx(kOutOf));
		CHECK(routeListOf(f.bridge) == routesBefore);
	}
}
TEST_CASE("the rack strip carries remove and reorder in the head row it draws itself") {
	// WHY THE RACK'S VERBS LIVE IN THE STRIP AND THE FX LANE'S HAD TO GO IN THE CARDS. fx_card.h
	// records that the two chains divide the head differently — an fx card draws its own head and
	// fills its lane, so a lane-drawn target would be underneath it; the modulator strip draws every
	// head row itself and hands a card only its BODY. This is the first place that difference paid
	// rather than cost, and it is why there is no third copy of the fx cards' base-class affordance.
	ModFixture f;
	auto* panel = f.panel;
	REQUIRE(panel != nullptr);
	REQUIRE(panel->deviceCount() >= 1);

	const backplane::Source source = backplane::Source::output(f.bridge.rackDeviceAt(0), 0);
	const visage::Bounds dots = panel->slotMenuBounds(source);
	CHECK(dots.width() > 0.0f);

	// THE SMALL VERBS BEFORE THE CARD THEY SIT ON, and before each other — three buttons in one head
	// row, each inside the last one's rectangle if the order were wrong.
	const auto centre = [](visage::Bounds b) {
		return visage::Point { b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f };
	};
	CHECK(panel->hitAt(centre(dots)).part == ui::ModPanel::Part::SlotMenu);
	CHECK(panel->hitAt(centre(panel->arrowBounds(source))).part == ui::ModPanel::Part::Arrow);
	// **THE MARK IS GONE AND THE TARGET IS THE HEAD ITSELF.**
	//
	// This asserted an EMPTY rectangle when the `(-)` rule was retired, on the reasoning that the
	// card would take its own head press. **MEASURED, and that was wrong for a MINIMISED card: the
	// frame at a folded card's head is the PANEL, because the card no longer covers it** — so with the
	// mark gone there was no road back and a collapsed modulator could not be reopened at all.
	//
	// So the panel keeps a fold target and it is the whole head, acted on at the second click. Not an
	// invisible control: the head is drawn and the whole of it is the button.
	{
		const visage::Bounds fold = panel->foldBounds(source);
		const visage::Bounds card = panel->sourceBounds(source);
		CHECK(fold.width() == doctest::Approx(card.width()));
		CHECK(fold.height() == doctest::Approx(ui::ModPanel::kHeadHeight));
		CHECK(fold.y() == doctest::Approx(card.y()));
	}
	// THE ARROW IS NO LONGER IN THIS ROW, which is the stronger claim and replaces the ordering
	// assertion that used to sit here. It is a modulation OUTPUT and outputs vary in number — one
	// today, none for an fx device, possibly two — so it lives in a row of its own below the head.
	const visage::Bounds head { panel->sourceBounds(source).x(), panel->sourceBounds(source).y(),
		                        panel->sourceBounds(source).width(), ui::ModPanel::kHeadHeight };
	CHECK(panel->arrowBounds(source).y() >= head.bottom());
	CHECK(dots.y() >= head.y());
	CHECK(dots.bottom() <= head.bottom());
	// Inside the card that owns them.
	CHECK(dots.x() >= panel->sourceBounds(source).x());

	SUBCASE("and the menu will not offer a move that has nowhere to go") {
		while (panel->deviceCount() < 2) {
			REQUIRE(panel->addChoiceForTest(0) < feathers::kMaxRackSlots);
		}
		// BY VERB, NOT BY ROW — the menu gained "Add before" at the top (backlog CR).
		CHECK_FALSE(offeredVerb(panel->slotOptionsForTest(0), ui::ModPanel::SlotVerb::MoveUp));
		CHECK(offeredVerb(panel->slotOptionsForTest(0), ui::ModPanel::SlotVerb::Remove)); // always, for a slot that holds something
		CHECK(panel->slotOptionsForTest(0)[0].verb == ui::ModPanel::SlotVerb::AddBefore);
		CHECK_FALSE(panel->slotChoiceForTest(0, ui::ModPanel::SlotVerb::MoveUp));
	}
}
TEST_CASE("a modulator card follows the position it is drawn at - across a removal and a reorder") {
	// The symptom: remove appeared to remove a different modulator from the one whose menu was
	// used. The removal is correct. What is wrong is everything BELOW it.
	//
	// **`ModPanel::ensureCard` KEYED ITS CACHE ON THE CARD'S KIND AND THE CARD HELD A LANE.** `cards_`
	// is indexed by POSITION; since BX step 4 (`2fb1e11`) a card is constructed with the LANE at that
	// position and every control inside it bakes its own copy (`DeviceControl::slot_`). Those two
	// addresses meet exactly once, at construction — so a key that does not name the lane lets a
	// reused card keep an address that has moved out from under it. Two DAHDSRs are the same kind and
	// the same tag, so the key hits and the card is never rebuilt.
	//
	// **IT IS A REGRESSION AND THE OLD SHAPE IS WHY IT WAS INVISIBLE.** Before step 4 the card held
	// its POSITION and `ModRackDeviceHost` translated position -> lane on every read, so a reused card
	// followed whatever moved into its position by construction. Step 4 moved the address into the
	// card and left the key alone. Its own safety argument — that a rack slot and its lane are never
	// equal, so a missed conversion reads out of range and is LOUD — does not reach this: the card
	// holds a perfectly valid lane, just not the one at its position.
	//
	// **ASSERTED AS AN ADDRESS AND AS A VALUE, NOT AS A COUNT.** "The strip got shorter" is green
	// whichever device went, and that is the entire bug. So: which instance survived, which lane each
	// surviving card addresses, and what the value under that address READS — the last one because a
	// card that draws the wrong device is what the hand actually sees.
	namespace dsr = feathers::mod::dahdsr;
	const auto sustain = static_cast<size_t>(dsr::Param::Sustain);

	ModFixture f;
	auto* panel = f.panel;
	REQUIRE(panel != nullptr);
	while (panel->deviceCount() < 3) {
		REQUIRE(panel->addChoiceForTest(0) < feathers::kMaxRackSlots);
	}
	REQUIRE(panel->deviceCount() == 3);

	// **A VALUE PER DEVICE, SO A CARD SHOWING THE WRONG ONE SAYS SO.** Three DAHDSRs are otherwise
	// indistinguishable, which is exactly the case the kind key cannot tell apart.
	const std::array<float, 3> mark { 0.2f, 0.5f, 0.8f };
	for (size_t slot = 0; slot < 3; ++slot) {
		f.bridge.setDeviceControl(f.bridge.rackDeviceAt(slot), sustain, mark[slot]);
		REQUIRE(f.bridge.rackFieldAt(slot, sustain) == doctest::Approx(mark[slot]));
	}

	// **A ROUTE OUT OF EACH**, so the removal's effect on the table can be read by coordinate rather
	// than by total. `fixedDest` is a destination NO rack device owns, so it survives whichever
	// modulator goes — the trap `test_rack_editor.cpp` records paying for.
	// THROUGH `modDestinationIndexOf` AND NOT THE RAW ParamId — the neighbouring case records paying
	// for that: an id cast to a destination is an out-of-table address the bridge correctly refuses,
	// so the route is never made and the case fails on its own setup.
	const int fixedIndex = feathers::modDestinationIndexOf(ParamId::FineTune);
	REQUIRE(fixedIndex >= 0);
	const backplane::Destination fixedDest = backplane::Destination::core(static_cast<size_t>(fixedIndex));
	const auto sourceOf = [&](size_t slot) { return backplane::Source::output(f.bridge.rackDeviceAt(slot), 0); };
	for (size_t slot = 0; slot < 3; ++slot) {
		f.bridge.setRouteAmount(sourceOf(slot), fixedDest, mark[slot]);
		REQUIRE(f.bridge.routeAmount(sourceOf(slot), fixedDest) == doctest::Approx(mark[slot]));
	}

	// WHICH DEVICE THE CARD AT A POSITION IS BUILT ON — `cardFrameForTest` answers a `visage::Frame`,
	// and `FxCard::device()` is where the baked address is readable (an id now; it was a lane).
	const auto cardDeviceAt = [&](size_t slot) {
		const auto* card = dynamic_cast<const ui::FxCard*>(panel->cardFrameForTest(slot));
		REQUIRE(card != nullptr);
		return card->device();
	};

	const feathers::ModRack before = f.bridge.modRack();
	const std::array<feathers::DeviceInstanceId, 3> id {
		before.device[0].instanceId, before.device[1].instanceId, before.device[2].instanceId
	};
	REQUIRE(id[0] != id[1]);
	REQUIRE(id[1] != id[2]);
	for (size_t slot = 0; slot < 3; ++slot) {
		REQUIRE(cardDeviceAt(slot) == id[slot]);
	}

	SUBCASE("the card below a removal stops addressing the device that was removed") {
		// THE MENU RESOLVES CORRECTLY AND ALWAYS DID — asserted so the case says which half moved.
		// Through the tree, because which FRAME takes the press is half of "the menu I called".
		{
			const visage::Bounds dots = panel->slotMenuBounds(sourceOf(1));
			const visage::Point at = panel->positionInWindow()
			                         + visage::Point { dots.x() + dots.width() * 0.5f,
				                                       dots.y() + dots.height() * 0.5f };
			auto* pressed = dynamic_cast<ui::FxCard*>(f.root->frameAtPoint(at));
			REQUIRE(pressed != nullptr);
			REQUIRE(pressed == panel->cardFrameForTest(1));
			CHECK(pressed->headMarkPressed(at - pressed->positionInWindow()));
			CHECK(panel->lastSlotMenuForTest() == 1);
		}

		REQUIRE(panel->slotChoiceForTest(1, ui::ModPanel::SlotVerb::Remove));

		// THE RIGHT ONE DIED, BY IDENTITY.
		const feathers::ModRack after = f.bridge.modRack();
		REQUIRE(after.count == 2);
		CHECK(after.device[0].instanceId == id[0]);
		CHECK(after.device[1].instanceId == id[2]);

		// **AND THE CARD AT POSITION 1 IS NOW C'S, NOT B'S.** This is the assertion that was red:
		// MEASURED on 84e70bb it read `9 == 10` with the lanes 8, 9, 10.
		for (size_t slot = 0; slot < 2; ++slot) {
			CHECK(cardDeviceAt(slot) == f.bridge.rackDeviceAt(slot));
		}
		CHECK(cardDeviceAt(1) == id[2]);

		// **AND THE VALUE UNDER THAT ADDRESS, which is what the card draws.** The address assertion
		// above is the mechanism; this is what the player sees. Read through the same host the card's
		// controls read through, at the same device they hold.
		CHECK(f.deviceHost.fieldAt(cardDeviceAt(0), sustain) == doctest::Approx(mark[0]));
		CHECK(f.deviceHost.fieldAt(cardDeviceAt(1), sustain) == doctest::Approx(mark[2]));

		// **THE ACTIVE LIGHT, WHICH IS THE THIRD SYMPTOM AND THE ONE THAT NAMED THE MECHANISM:** a
		// modulator's active light switched off just after the modulator before it was deleted, and no
		// amount of clicking brought it back.
		//
		// BOTH HALVES FALL OUT OF THE STALE LANE AND BOTH ARE ASSERTED, because they are different
		// defects and the symptom alone cannot tell them apart:
		//
		// **THE READ.** `FxCard::headEnabled()` is `h->fieldAt(device(), enableControl())` and
		// `device()` is the baked address — a lane then, an id now. A survivor holding a departed lane asked the host about a
		// row nothing holds, `rackSlotOfRouteLane` answers out of range, `rackFieldAt` answers 0.0 —
		// so the light draws OFF for a device that is on. Not a stale paint: it is the honest answer
		// to the wrong question, which is why a repaint never fixed it.
		//
		// **THE WRITE.** `toggleEnable()` goes through `enableModulatable()` -> `DeviceControl`,
		// which baked the SAME lane in its own `slot_`. So the press writes to the dead row and the
		// live device never hears it — no amount of clicking, exactly.
		auto* survivor = const_cast<ui::FxCard*>(
		    dynamic_cast<const ui::FxCard*>(panel->cardFrameForTest(1)));
		REQUIRE(survivor != nullptr);
		// OR THE WHOLE THING IS VACUOUS — a card with no enable answers `true` unconditionally.
		REQUIRE(survivor->hasEnable());
		const size_t enable = survivor->enableControl();
		// DEVICE C ARRIVED SWITCHED ON, and the rack agrees it still is.
		REQUIRE(f.bridge.rackFieldAt(1, enable) >= 0.5f);
		// THE READ HALF. MEASURED false on the stale build, for the reason above.
		CHECK(survivor->headEnabled());
		// THE WRITE HALF, THROUGH THE REAL VERB. The value is read back off the RACK at the position
		// the hand is pointing at, not off the card — a card that wrote to a dead lane and then read
		// its own dead lane back would agree with itself perfectly.
		const auto nowOn = survivor->toggleEnable();
		REQUIRE(nowOn.has_value());
		CHECK_FALSE(*nowOn);
		CHECK(f.bridge.rackFieldAt(1, enable) < 0.5f);
		// AND IT COMES BACK, which is the half that was broken.
		const auto backOn = survivor->toggleEnable();
		REQUIRE(backOn.has_value());
		CHECK(*backOn);
		CHECK(f.bridge.rackFieldAt(1, enable) >= 0.5f);
		// AND THE NEIGHBOUR WAS NOT TOUCHED BY EITHER PRESS — "deleting one device disabled another"
		// is the destructive reading, and this is what refuses it.
		CHECK(f.bridge.rackFieldAt(0, enable) >= 0.5f);

		// **THE ROUTES: THE REMOVED DEVICE'S COLUMN CLEARED, THE SURVIVORS' UNTOUCHED.** A removal
		// preserves relative order, so nothing it does may make a surviving route illegal —
		// `clearRoutesTheOrderForbids` runs on a MOVE and must not have run here.
		CHECK(f.bridge.routeAmount(backplane::Source::output(id[1], 0), fixedDest) == doctest::Approx(0.0f));
		CHECK(f.bridge.routeAmount(backplane::Source::output(id[0], 0), fixedDest) == doctest::Approx(mark[0]));
		CHECK(f.bridge.routeAmount(backplane::Source::output(id[2], 0), fixedDest) == doctest::Approx(mark[2]));
	}

	SUBCASE("and a reorder moves the cards with the devices, not just the rows") {
		// THE SAME MECHANISM WEARING THE OTHER VERB — a reorder permutes the devices at positions
		// without changing any card's KIND, so a stale key hits here too.
		REQUIRE(panel->moveDevice(2, 0));
		REQUIRE(f.bridge.rackDeviceAt(0) == id[2]);
		for (size_t slot = 0; slot < 3; ++slot) {
			CHECK(cardDeviceAt(slot) == f.bridge.rackDeviceAt(slot));
		}
		CHECK(f.deviceHost.fieldAt(cardDeviceAt(0), sustain) == doctest::Approx(mark[2]));
	}

	SUBCASE("and a rebuilt card is RETIRED rather than freed, because a drag reorder is dispatching") {
		// **A CLAIM ABOUT A DEFERRAL, ASSERTED AS A COUNT OF WHAT IS WAITING.** `FxLane` states why a
		// weaker check will not do: "a case that only checks the editor survived would pass on a build
		// that freed it and got lucky — a dangling read is undefined, not reliably fatal".
		//
		// THIS PANEL DID NOT NEED THE DEFERRAL UNTIL THE KEY LEARNED ABOUT LANES. A reorder rebuilds
		// cards now, and the road a hand takes to one is `FxCard::noteCardRelease` ->
		// `onCardDrag(End)` -> `moveDevice` -> `resized()` — so the free must not happen in the loop
		// that is rebuilding, or it frees the card whose handler is on the stack.
		// DRAINED FIRST, because the two adds in this case's setup retired cards of their own — a
		// dormant slot's placeholder is a different KIND from the envelope that replaces it, so those
		// rebuilds predate the lane key and are not what this subcase is about.
		panel->releaseRetiredCards();
		REQUIRE(panel->retiredCountForTest() == 0);
		REQUIRE(panel->moveDevice(2, 0));

		// **EXACTLY THREE, AND `> 0` WOULD HAVE BEEN VACUOUS — MEASURED.** This case's first draft
		// asserted "something is waiting", and the known-bad that puts `retiredCards_.clear()` back
		// at the top of `ensureCard` was GREEN under it: the loop frees slot 0's card when it reaches
		// slot 1 and slot 1's when it reaches slot 2, so the LAST retirement always survives and a
		// non-empty list says nothing about the two that were destroyed mid-dispatch. Exactly the
		// shape CLAUDE.md's rule describes — a difference check blind where the bug is subtle.
		//
		// `moveDevice(2, 0)` rotates all three lanes, so all three positions change address and all
		// three cards are rebuilt. Three retirements must be WAITING; the buggy build leaves one.
		CHECK(panel->retiredCountForTest() == 3);
		// AND THE ONE PLACE THAT MAY FREE THEM DOES.
		panel->releaseRetiredCards();
		CHECK(panel->retiredCountForTest() == 0);
	}

	SUBCASE("and an assign after a removal routes FROM the survivor, not from the departed lane") {
		// The suspicion: after assigning a modulator, the curve may have been coming from a different
		// modulator.
		//
		// **THE DISCRIMINATOR IS WHICH SOURCE THE ROUTE NAMES, AND IT SEPARATES TWO DIFFERENT BUGS
		// THE DESCRIPTION CANNOT.** If the cell lands under the wrong source, the ASSIGN is misaddressed.
		// If it lands under the right one and the wrong value arrives, the RESOLVE is. Only reading
		// the cell by coordinate tells them apart — "a modulator is modulating" is true either way.
		//
		// **THE BUS IS ALREADY EXCLUDED BY CONSTRUCTION**, which is why this case only has to answer
		// the assign half: `ParamSet::fillModulationLane` writes `out[source]`,
		// `backplane::DeviceBus::sourceAt` reads `modulation[source]`, and `ParamSet::rackOffsetAt`
		// walks that same coordinate into `routes_`. One index throughout, no mapping between them to
		// disagree, and no node is ever handed a re-based subspan — both construction sites assign
		// the whole `kModSourceCount` array and `Synth::fillBusFor` masks IN PLACE. It removes
		// sources; it never shifts them.
		//
		// **THIS SUBCASE IS A NEGATIVE CONTROL AND MUST BE READ AS ONE. MEASURED: every assertion
		// below stays GREEN under the stale-lane known-bad**, while the sibling subcases go red at
		// `9 == 10`. That is the finding, not a gap: **the assign road never touches the card's baked
		// lane.** `SourceArm` asks for its source at every press (282f244) and
		// `ParamControl::setAmount` writes `bridge_.setRouteAmount(mod_->source(), ...)`, so the
		// route is addressed correctly even on a build where the card is a row stale.
		//
		// **SO THIS SYMPTOM IS A DISPLAY FAULT, NOT A ROUTING ONE, AND THE DISTINCTION IS THE WHOLE
		// VALUE OF ASKING.** The modulation went where it was pointed. What was wrong is that the
		// CARD it was pointed from was drawing the deleted device's curve — the sibling subcase measures
		// exactly that, `fieldAt(cardLane, sustain)` reading 0 where 0.8 belongs. "The curve came from a
		// different modulator" is literally true and literally about the
		// curve. Do not let a later reader mistake these greens for coverage of the stale lane; the
		// case above is what covers that.
		REQUIRE(panel->slotChoiceForTest(1, ui::ModPanel::SlotVerb::Remove));
		REQUIRE(panel->deviceCount() == 2);

		ui::ModState* mod = f.gui.modForTest();
		REQUIRE(mod != nullptr);

		// ARM THE SURVIVOR'S ARROW, through the real tree at the rectangle the panel draws it in.
		const backplane::Source survivorSource = sourceOf(1);
		REQUIRE(survivorSource == backplane::Source::output(id[2], 0));
		REQUIRE(panel->outputChipBounds(survivorSource).width() > 0.0f);
		f.arm(survivorSource);
		REQUIRE(mod->armed());
		// THE ARM ITSELF IS ALREADY RIGHT — `SourceArm` asks for its source at every press rather than
		// remembering one (282f244), so this half survived the stale card. Asserted anyway, so a red
		// below is attributable to the assign rather than to the arming.
		CHECK(mod->source() == survivorSource);

		// THE ASSIGN, THROUGH THE CONTROL THE KNOB OWNS — `ParamControl::setAmount` is
		// `bridge_.setRouteAmount(mod_->source(), destination, amount)`, so this is the exact write a
		// hand makes by dragging a ring.
		ui::Knob* path = f.knobFor(ParamId::Path);
		REQUIRE(path != nullptr);
		REQUIRE(path->mappable());
		const backplane::Destination pathDest =
		    backplane::Destination::core(static_cast<size_t>(feathers::modDestinationIndexOf(ParamId::Path)));
		constexpr float kAssigned = 0.6f;
		path->control()->setAmount(kAssigned);

		// **THE ROUTE, BY ENDPOINT.** From the survivor and from nothing else.
		CHECK(f.bridge.routeAmount(backplane::Source::output(id[2], 0), pathDest) == doctest::Approx(kAssigned));
		// AND NOT FROM THE DEPARTED ONE — the reading that would have been the suspected bug.
		CHECK(f.bridge.routeAmount(backplane::Source::output(id[1], 0), pathDest) == doctest::Approx(0.0f));
		// AND NOT FROM THE OTHER SURVIVOR — which is what makes the first check non-vacuous: a write
		// that went everywhere would satisfy it.
		CHECK(f.bridge.routeAmount(backplane::Source::output(id[0], 0), pathDest) == doctest::Approx(0.0f));
	}

	SUBCASE("and a DRAG reorder does not free the card that is dispatching it") {
		// **THE ROAD THE DEFERRAL EXISTS FOR, DRIVEN AS A GESTURE RATHER THAN AS A VERB.**
		//
		// The subcase below asserts the deferral at `moveDevice`, which is what the drag CALLS. This
		// one goes through the card's own handlers, because the hazard is not that a card is freed —
		// it is that a card is freed WHILE ITS OWN `onMouseUp` IS ON THE STACK. `FxCard::
		// noteCardRelease` -> `onCardDrag(DragPhase::End)` -> `ModPanel::moveDevice` -> `resized()`
		// -> `ensureCard`, and with the free back at the top of `ensureCard` the second rebuilt slot
		// frees the card the first retired. That is `fx_lane.cpp`'s documented signal 11 in
		// `Frame::on_mouse_exit_`, resolved from the crash dump, and the rack was one rebuild
		// away from it.
		//
		// **THE INSTRUMENT FOR THIS IS ASAN, NOT THIS ASSERTION.** A dangling read is undefined, not
		// reliably fatal — the fx lane's use-after-free survived three notifications before it killed
		// anything — so what this subcase contributes is a case that TRAVELS the road; the sanitizer
		// gate is what watches it. `scripts/sanitize-gate.sh` runs the widget suites, so this case
		// runs under ASan by being here.
		// **THE FIRST CARD, DRAGGED DOWN — AND THE DIRECTION IS THE WHOLE EXPERIMENT.**
		//
		// `resized()` walks slots in ascending order, and the buggy free is at the TOP of
		// `ensureCard`, so slot N's `clear()` frees whatever slot N-1 retired. Dragging the LAST card
		// UP therefore proves nothing: its own card is retired by the FINAL `ensureCard` and no later
		// call frees it, so the dispatching object survives and ASan stays silent on a broken build.
		// MEASURED — that was this subcase's first draft.
		//
		// Dragging the FIRST card DOWN puts the dispatching card in slot 0, retired first and freed
		// by slot 1's `clear()` while `FxCard::noteCardRelease` is still on its stack. That is
		// `fx_lane.cpp`'s crash exactly, and it is the only arrangement that reproduces it.
		panel->releaseRetiredCards();
		auto* dragged = const_cast<ui::FxCard*>(
		    dynamic_cast<const ui::FxCard*>(panel->cardFrameForTest(0)));
		REQUIRE(dragged != nullptr);

		// **THE DROP POINT COMPUTED FROM THE REAL GEOMETRY, NOT GUESSED.** The card reports in its
		// OWN coordinates and only the panel knows where the card sits, so the conversion here is the
		// inverse of the one `onCardDrag` makes. A first draft used a fixed pixel offset, landed the
		// pointer ABOVE the strip, and `dropGapAt` answered nullopt — the release then moved nothing
		// and every assertion below passed vacuously. That is the whole reason this is arithmetic.
		const visage::Bounds from = panel->sourceBounds(sourceOf(0));
		const auto inCardY = [&](float panelY) { return panelY - from.y(); };

		// **THROUGH `process*` RATHER THAN THE CALLBACK LISTS — the fuller road, though MEASUREMENT
		// SAYS IT IS NOT WHAT MAKES THE BUG VISIBLE, and the difference is worth stating because I
		// got it wrong first.**
		//
		// I believed the raw callback road could not express the defect, on the reasoning that
		// `visage::CallbackList::callback` resolves `callbacks_.back()` BEFORE invoking and returns
		// void, so nothing reads the card after it dies — leaving only
		// `Frame::propagateMouseEvent`'s `auto frame = parent_;` after `(this->*handler)(e)` as the
		// freed read. **MEASURED, and that reasoning was wrong about WHERE the read is.** Both roads
		// red identically, at the same address and the same line:
		//
		//   heap-use-after-free, READ of size 8, ../src/plugin/ui/mod_panel.cpp:1622 in operator()
		//     #5 FxCard::noteCardRelease  fx_card.h:195
		//     freed by: unique_ptr<FxCard>::~unique_ptr  <- retiredCards_.clear()
		//
		// The read is the LAMBDA'S OWN CAPTURED STATE. `onCardDrag` is a `std::function` member OF
		// THE CARD, its two-word capture lives in that function's inline storage, and the card is
		// destroyed part-way through the call — so the lambda is still running inside memory that has
		// been freed underneath it. `noteCardRelease` invoking `onCardDrag(End)` is sufficient; no
		// dispatch layer is needed to make it fatal.
		//
		// `process*` is kept anyway because it is what the editor does and it additionally covers the
		// `parent_` read above — a second freed access on the same gesture. It is the more faithful
		// road, not the load-bearing one.
		//
		// **AND THE GREEN THAT PRODUCED THE WRONG THEORY WAS AN INSTRUMENT FAULT, NOT A ROAD FAULT.**
		// `scripts/sanitize-gate.sh` sets its source root from its OWN location, so running the
		// mainline copy from a worktree builds MAINLINE while honouring `FEATHERS_ASAN_DIR` for the
		// output. Two gate runs reported green on a tree that did not contain this fix at all. A
		// worktree runs its own `scripts/sanitize-gate.sh`.
		const float head = ui::FxCard::kSlotHeadHeight * 0.5f;
		dragged->processMouseDown(doubleAt(20.0f, head));
		// PAST THE THRESHOLD FIRST, because a nudge under it is still a click.
		dragged->processMouseDrag(at(20.0f, head + ui::ModPanel::kDragThreshold * 3.0f));
		REQUIRE(dragged->cardDragging());
		REQUIRE(panel->dragSlot().has_value());
		CHECK(*panel->dragSlot() == 0);
		// AND THEN INTO THE GAP BELOW THE LAST CARD, in panel coordinates converted back to the
		// card's. REQUIREd, so a drop that lands nowhere fails here rather than passing silently
		// below — a fixed pixel offset did exactly that in this subcase's first draft.
		// **GAP 2, NOT THE FOOT OF THE STRIP — AND THE STRIP'S HEIGHT IS WHY.** MEASURED: the panel is
		// 539 tall and card 2 spans 530..720, so most of the last card and the gap below it are off
		// the bottom, and `dropGapAt` refuses a point outside the frame. Gap 2 sits at y=524 and is
		// the last one reachable without scrolling.
		//
		// IT IS STILL THE ARRANGEMENT THIS SUBCASE NEEDS: `rowForGap(0, 2, 3)` moves the dragged card
		// to position 1, so positions 0 and 1 both change lane and BOTH rebuild — slot 0 retiring the
		// card that is dispatching and slot 1's `clear()` being what would free it.
		const visage::Bounds gap = panel->gapBounds(2);
		REQUIRE(gap.y() < panel->height());
		const float dropY = inCardY(gap.y() + 1.0f);
		dragged->processMouseDrag(at(20.0f, dropY));
		REQUIRE(panel->dropGap().has_value());
		CHECK(*panel->dropGap() == 2);

		// **THE RELEASE, WHICH IS THE WHOLE POINT** — the rebuild happens INSIDE this call, and the
		// dispatch reads the card again on the way out.
		dragged->processMouseUp(at(20.0f, dropY));
		CHECK_FALSE(panel->dragSlot().has_value());
		// THE CHAIN REALLY MOVED — otherwise nothing was rebuilt and this subcase asks nothing.
		REQUIRE(f.bridge.rackDeviceAt(1) == id[0]);

		// THE REORDER HAPPENED AND EVERY CARD FOLLOWED ITS DEVICE.
		for (size_t slot = 0; slot < 3; ++slot) {
			CHECK(cardDeviceAt(slot) == f.bridge.rackDeviceAt(slot));
		}
		// AND THE CARDS THE REBUILD RETIRED ARE STILL WAITING, not freed under the gesture.
		CHECK(panel->retiredCountForTest() > 0);
		panel->releaseRetiredCards();
	}

	SUBCASE("and deleting the LAST one rebuilds nothing, because no survivor's lane moved") {
		// **THE CONTROL THAT STOPS THE FIX BEING "REBUILD EVERYTHING".** A key that compared unequal
		// whenever anything changed would pass every assertion above and be a different bug: cards
		// are retired and rebuilt through a road that exists because destroying one at the wrong
		// moment is a use-after-free this repo has already shipped, so a card must be replaced only
		// when it genuinely must be.
		//
		// Deleting the LAST position shifts nobody. Both survivors keep their lanes, so both must
		// keep their card OBJECTS — asserted by pointer, because `retiredCountForTest() == 0` alone
		// would pass on a build that rebuilt in place without retiring.
		panel->releaseRetiredCards();
		REQUIRE(panel->retiredCountForTest() == 0);
		const std::array<const visage::Frame*, 2> was { panel->cardFrameForTest(0),
			                                            panel->cardFrameForTest(1) };
		REQUIRE(panel->slotChoiceForTest(2, ui::ModPanel::SlotVerb::Remove));
		REQUIRE(panel->deviceCount() == 2);
		// **THE SURVIVORS' CARDS, BY POINTER — NOT THE RETIRED COUNT.** MEASURED: the count is 1 here
		// and correctly so, because slot 2 itself goes from Envelope to Dormant and its PLACEHOLDER
		// is rebuilt. That retirement is the empty slot's, not a survivor's, and asserting the total
		// would have failed the fix for doing the right thing.
		CHECK(panel->cardFrameForTest(0) == was[0]);
		CHECK(panel->cardFrameForTest(1) == was[1]);
		// AND THEY STILL ADDRESS WHAT THEY ALWAYS DID.
		CHECK(cardDeviceAt(0) == id[0]);
		CHECK(cardDeviceAt(1) == id[1]);
	}

	SUBCASE("and a layout that changes nothing rebuilds nothing") {
		// **THE OTHER SIDE OF THE KEY, AND IT IS NOT A NICETY.** A key that compared unequal on every
		// pass would rebuild every card on every `resized()` — the strip lays out on scroll, on
		// resize and on every add — which is a fresh set of widgets per frame and a retired card per
		// slot piling up until the next tick drains them. Cheap to assert and expensive to discover.
		panel->releaseRetiredCards();
		REQUIRE(panel->retiredCountForTest() == 0);
		const std::array<const visage::Frame*, 3> was { panel->cardFrameForTest(0),
			                                            panel->cardFrameForTest(1),
			                                            panel->cardFrameForTest(2) };
		panel->resized();
		panel->resized();
		CHECK(panel->retiredCountForTest() == 0);
		// AND THE SAME OBJECTS, which is the claim `retiredCountForTest` alone cannot make — a
		// rebuild that retired nothing would still have replaced them.
		for (size_t slot = 0; slot < 3; ++slot) {
			CHECK(panel->cardFrameForTest(slot) == was[slot]);
		}
	}
}
TEST_CASE("an fx card follows the position it is drawn at, across a removal - the same defect, one lane over") {
	// **THE MODULATOR STRIP'S BUG IN A SECOND FILE, FOUND BY ASKING WHETHER IT WAS THERE.** See the
	// modulator case above for the mechanism; `FxLane::ensureCard` had the same omission wearing a
	// different key. `cards_` is keyed by POSITION, a card is built with the LANE at that position,
	// and the cache key named only the TAG — which cannot tell two devices of the same kind apart.
	//
	// **THAT IS WHY IT SURVIVED: A DIFFERENT TAG MASKS IT ENTIRELY.** The neighbouring case "two
	// filters in one chain are two devices, not one device drawn twice" builds exactly the
	// arrangement that exposes this and never removes one, so the key was never asked a question it
	// could get wrong.
	//
	// MEASURED before the fix, three filters on lanes 0, 1, 2 with the middle removed: the card at
	// position 1 answered lane 1 where the chain had lane 2 — `CHECK( 1 == 2 )`.
	namespace flt = feathers::fx::filter;
	FilterFixture f;
	auto* lane = f.lane;
	REQUIRE(lane != nullptr);
	// TWO MORE OF THE SAME TAG AS THE ONE THE EDITOR OPENS WITH, because same-tag neighbours are the
	// whole condition.
	REQUIRE(f.bridge.addFxDevice(flt::kTraits.tag) < feathers::kMaxFxSlots);
	REQUIRE(f.bridge.addFxDevice(flt::kTraits.tag) < feathers::kMaxFxSlots);
	f.bridge.refresh();
	lane->resized();
	const size_t count = f.bridge.fxChain().count;
	REQUIRE(count >= 3);
	for (size_t slot = 0; slot < count; ++slot) {
		const auto* card = lane->cardAt(slot);
		REQUIRE(card != nullptr);
		REQUIRE(card->device() == f.bridge.fxDeviceAt(slot));
	}

	// THE MIDDLE OF THE THREE — the one whose removal shifts a same-tag device up into its place.
	REQUIRE(f.bridge.removeFxDevice(count - 2));
	f.bridge.refresh();
	lane->resized();
	const size_t after = f.bridge.fxChain().count;
	REQUIRE(after == count - 1);
	for (size_t slot = 0; slot < after; ++slot) {
		const auto* card = lane->cardAt(slot);
		REQUIRE(card != nullptr);
		CHECK(card->device() == f.bridge.fxDeviceAt(slot));
	}
}
TEST_CASE("in the modulator strip the enable square clears the head mark THIS strip draws") {
	// The defect: the (...) mark still sat over the MSEG and DAHDSR active box. f4d099f fixed a real
	// bug and fixed it properly — on the other road.
	//
	// **A CARD HEAD HAS TWO OVERFLOW MENUS AND THEY ARE DIFFERENT RECTANGLES.** `slotMenuBounds()` is
	// the CARD's own, 12px, drawn when a host binds `onSlotMenu`; `ui::cardMenuBounds` is the HOST's
	// head mark, 18px, drawn by `ui::drawCardFrame` when `headMarks().menu`. This strip is the ONLY
	// host that draws the second one, and it binds no `onSlotMenu` at all — so the square, which was
	// anchored to `slotMenuBounds()`, was placed against a rectangle nothing here draws or presses,
	// and the mark landed on top of it.
	//
	// **THE TITLE IS THE PARTICULAR CLAIM, DELIBERATELY.** Its predecessor in `test_rack_editor.cpp`
	// is called *"no card's overflow menu is drawn on top of its enable square" — true, well built,
	// known-bad validated, and it holds ONE of the two rectangles, in the two hosts that draw the
	// other one, behind a `hasSlotMenu()` gate that is false in the host where the bug lives. Nothing
	// in it is wrong; its NAME is universal and its body is particular. A second case wearing a
	// universal name beside it would be the same joke told twice, so this one says which strip, which
	// mark, and nothing wider.
	//
	// **MEASURED BEFORE IT WAS ASSERTED**, on the rendered strip under the private venue: DAHDSR at
	// card x∈[178,394) with its head top at 182 gave square x[354,366) y[188,200) against menu
	// x[347,365) y[185,203) — an 11×12 overlap with two of the three painted dots (351, 355, 359)
	// inside the square. MSEG and TRAK to the pixel. Both rectangles anchor to the card's RIGHT EDGE
	// and its top, so the overlap is constant at every width and height: BG's 40-to-60 band is a fact
	// about the SPINE and has nothing to say here, which is why every height sweep stayed green.
	//
	// WHAT WAS NOT PHOTOGRAPHED, stated because it stays inference: the switched-OFF appearance.
	// `widget-shot` refuses `--set` for rack devices and a rack device's controls are swallowed rather
	// than CLAP params, so neither tool can reach the state. That `drawEnableSwitch` paints an OUTLINE
	// when off — leaving the two dots visible INSIDE the box, a design choice so that it does not look
	// 'checked' — is read off eight lines of that function, not seen.
	const char* const tags[] = {
		feathers::mod::dahdsr::kTraits.tag,
		feathers::mod::mseg::kTraits.tag,
		feathers::mod::tracker::kTraits.tag,
	};

	for (const char* tag : tags) {
		const std::string who = tag;
		CAPTURE(who);
		// A FIXTURE PER DEVICE. The strip's layout is a function of what is in it, and a case that
		// added three and asked about the first would be asking about a card three neighbours have
		// moved.
		ModFixture f;
		REQUIRE(f.panel != nullptr);
		const size_t slot = f.bridge.addRackDevice(tag);
		REQUIRE(slot < feathers::kMaxRackSlots);
		f.panel->resized();

		auto* frame = const_cast<visage::Frame*>(f.panel->cardFrameForTest(slot));
		REQUIRE(frame != nullptr);
		auto* card = dynamic_cast<ui::FxCard*>(frame);
		REQUIRE(card != nullptr);
		REQUIRE(card->hasEnable());

		// **THE VACUITY GUARDS, AND THEY ARE NOT DECORATION.** Flip `mod_panel`'s `setHeadMarks` to
		// `{ false, … }` and there is no mark to collide with, at which point "they do not overlap"
		// is true of a strip with no menu at all — a mutation that would leave the assertion below
		// green while removing the only road to Remove and MoveUp. These two lines are what make that
		// mutation red instead.
		REQUIRE(card->headMarks().menu);
		const visage::Bounds menu =
		    ui::cardMenuBounds({ 0.0f, 0.0f, static_cast<float>(card->width()),
			                     static_cast<float>(card->height()) },
		                       card->headEdge());
		REQUIRE(menu.width() > 0.0f);

		// AND THE SQUARE MUST STILL EXIST. The spine's remedy is to HIDE a mark that cannot fit; the
		// remedy here is to MOVE the square, and "they do not overlap" is equally true of a square
		// that vanished.
		const visage::Bounds sw = card->enableBounds();
		REQUIRE(sw.width() > 0.0f);
		CAPTURE(sw.x());
		CAPTURE(sw.right());
		CAPTURE(menu.x());

		// **THE WHOLE CLAIM, IN THE SHAPE THE FX LANE'S NEIGHBOUR ALREADY USES** — `sw.right() <=
		// dots.x()` is asserted of HARM a few thousand lines up, against the OTHER menu, in the lane
		// where it holds. This is that same inequality asked of the strip and of the mark the strip
		// actually paints.
		CHECK(sw.right() <= menu.x());
		// STILL IN THE HEAD ROW AND STILL ON THE CARD — a square shoved off the left edge would
		// satisfy the line above and be no better than the overlap.
		CHECK(sw.x() >= 0.0f);
		CHECK(sw.y() >= 0.0f);
		CHECK(sw.bottom() <= ui::kCardHeadHeight);

		// ------------------------------------------------------------------------------------------
		// **AND IT WAS NEVER ONLY PAINT.** `headControlAt` counts the square as a head control, so
		// `noteCardPress` runs `headMarkPressed`, whose rectangle covered 132 of the square's 144 px;
		// `EnvCard::onMouseDown` then falls through to `enablePressAt`, which also matched. One click
		// on 11 of the square's 12 columns opened the menu AND flipped the device.
		//
		// THE PAIR IS THE POINT. Deleting `headMarkPressed` outright would satisfy "a press on the
		// square does not open the menu" on a build where NOTHING opens it, so the menu's own centre
		// is pressed too and must still reach the verb.
		// ------------------------------------------------------------------------------------------
		bool sawMenu = false;
		card->onHeadMenu = [&sawMenu] { sawMenu = true; };

		const auto enabled = [&] {
			const feathers::ModRack rack = f.bridge.modRack();
			const feathers::device::Descriptor* d = feathers::descriptorForRow(rack.device[slot]);
			REQUIRE(d != nullptr);
			REQUIRE(d->traits.enableControl >= 0);
			return rack.device[slot].field[static_cast<size_t>(d->traits.enableControl)];
		};
		const auto pressAt = [&](visage::Bounds b) {
			visage::MouseEvent e {};
			e.button_id = visage::kMouseButtonLeft;
			e.button_state = visage::kMouseButtonLeft;
			e.repeat_click_count = 1;
			e.is_down = true;
			e.position = { b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f };
			card->onMouseDown().callback(e);
		};

		// NOT VACUOUS: a modulator is added switched ON, so the flip below is a change away from on
		// rather than a write that happens to read differently from nothing.
		REQUIRE(enabled() >= 0.5f);
		pressAt(sw);
		CHECK(enabled() < 0.5f);
		CHECK_FALSE(sawMenu);

		// THE OTHER HALF, which is what keeps the line above from passing on a dead menu.
		const float held = enabled();
		pressAt(menu);
		CHECK(sawMenu);
		// AND THE MENU'S PRESS IS NOT ALSO THE SWITCH'S — the collision read both ways.
		CHECK(enabled() == doctest::Approx(held));
	}
}
TEST_CASE("undoing a rack add removes the device, which the (+) shipped unable to do") {
	// THE GAP THE RACK'S (+) HAS CARRIED SINCE IT LANDED, and the reason was never the history: it
	// was that undoing an add means REMOVING a device, and `removeRackDevice` had no caller anywhere
	// in src/plugin — a mechanism built, tested, and never connected.
	ModFixture f;
	auto* panel = f.panel;
	REQUIRE(panel != nullptr);
	const size_t before = panel->deviceCount();

	const size_t slot = panel->addChoiceForTest(1);
	REQUIRE(slot == before);
	const feathers::DeviceInstanceId born = f.bridge.modRack().device[slot].instanceId;
	REQUIRE(born != feathers::kNoInstance);

	REQUIRE(f.gui.history()->canUndo());
	f.gui.history()->undo();
	CHECK(panel->deviceCount() == before);

	f.gui.history()->redo();
	CHECK(panel->deviceCount() == before + 1);
	// THE SAME DEVICE, NOT A NEW ONE WITH THE SAME TAG — addRackDevice mints a fresh serial, so a redo
	// that re-added would orphan any routing a patch saved against the old id.
	CHECK(f.bridge.modRack().device[slot].instanceId == born);
}
TEST_CASE("both lanes answer the same sequence the same way") {
	// TWO LANES THAT NOW BEHAVE IDENTICALLY AND ARE IMPLEMENTED DIFFERENTLY ON PURPOSE. The rack
	// draws its own head rows and hangs its verbs there; the fx cards own theirs through a base
	// class, because a card that fills its lane would swallow a lane-drawn target. Neither is going
	// to change, and this case is NOT about how they are built.
	//
	// IT IS ABOUT DRIFT. Each lane's own cases stay green when only one of them gets a fix, so the
	// failure this exists to catch is silent by construction: a hand meets two chains that used to
	// agree and now do not, and nothing in the suite says a word. So one case drives BOTH through the
	// same sequence and requires the same SHAPE of outcome.
	//
	// EVERYTHING IT ASKS IS OBSERVABLE FROM OUTSIDE — counts, the order of instance ids, and the
	// routing table. It names no widget, no head row and no card.
	//
	//===========================================================================================//
	// THE SAME SHAPE OF OUTCOME IS NOT THE SAME ROUTINGS. DO NOT "FIX" THIS INTO A CROSS-LANE     //
	// COMPARISON.                                                                                //
	//===========================================================================================//
	//
	// The two lanes differ in what a SLOT MEANS. An fx slot is a position in a signal chain — a set
	// of destinations, and nothing else, because both devices that may sit there declare
	// `.outputs = {}`. A RACK SLOT IS ALSO A MODULATION SOURCE: a DAHDSR and an MSEG publish outputs,
	// so removing one drops every route leading OUT of it as well as in.
	//
	// SO THE TWO LANES' ROUTING TABLES ARE NOT THE SAME SHAPE AND MUST NEVER BE COMPARED TO EACH
	// OTHER. A case that expected them to match would be asserting something false, and it would look
	// like the obvious tightening of the two `amount ==` checks below.
	//
	// WHAT THOSE CHECKS ACTUALLY COMPARE IS ONE LANE AGAINST ITSELF: the table before a verb against
	// the table after the verb and its undo, with the snapshot taken inside this arm. That is a claim
	// about REVERSIBILITY, which both lanes really do share — and it is what lets one assertion cover
	// both routing directions without the case ever asking which directions this lane has.
	//
	// THE PARITY IS IN THE VERBS AND THEIR REVERSIBILITY, NOT IN THE ROUTING ARITHMETIC.
	struct Lane {
		const char* name;
		size_t maxSlots;
		std::function<size_t(size_t)> add;          ///< add the nth offered device, return its slot
		std::function<size_t()> count;
		std::function<bool(size_t)> remove;
		std::function<bool(size_t, size_t)> move;
		std::function<DeviceInstanceId(size_t)> deviceAt; ///< this lane's slot -> the device there, by id
		std::function<std::vector<feathers::DeviceInstanceId>()> order;
		std::function<bool(size_t)> moveUpOffered;
		std::function<bool(size_t)> moveDownOffered;
		std::function<bool(size_t)> removeOffered;
	};

	const auto exercise = [](FilterFixture& f, const Lane& lane) {
		CAPTURE(lane.name);
		ui::EditHistory* history = f.gui.history();
		REQUIRE(history != nullptr);

		//-- ADD, UNDO, REDO ----------------------------------------------------------------------//
		const size_t before = lane.count();
		const size_t slot = lane.add(0);
		REQUIRE(slot == before);
		CHECK(lane.count() == before + 1);
		const feathers::DeviceInstanceId born = lane.order()[slot];
		CHECK(born != feathers::kNoInstance);

		REQUIRE(history->canUndo());
		history->undo();
		CHECK(lane.count() == before);
		history->redo();
		CHECK(lane.count() == before + 1);
		// THE SAME IDENTITY BOTH SIDES OF A ROUND TRIP. Both lanes mint a fresh serial on an add, so
		// both have to redo by re-inserting rather than re-adding.
		CHECK(lane.order()[slot] == born);

		//-- ROUTE INTO EVERY DEVICE ----------------------------------------------------------------//
		// BY ORDINAL, which is a device's own numbering of the controls it offers — so this asks the
		// same question of a filter, a HARM, a DAHDSR and an MSEG without naming any of them.
		while (lane.count() < 3 && lane.count() < lane.maxSlots) {
			REQUIRE(lane.add(0) < lane.maxSlots);
		}
		REQUIRE(lane.count() >= 3);
		const auto destOf = [&](size_t at) { return firstDestinationOf(f.bridge, lane.deviceAt(at)); };
		const backplane::Source macro = backplane::Source::core(0);
		for (size_t at = 0; at < lane.count(); ++at) {
			f.bridge.setRouteAmount(macro, destOf(at), 0.1f * static_cast<float>(at + 1));
			// NOT VACUOUS, PER SLOT — a lane whose devices declared no modulatable control would make
			// every comparison below a comparison of two empty lists.
			REQUIRE(f.bridge.routeAmount(macro, destOf(at)) != 0.0f);
		}
		const std::vector<backplane::Route> table = routeListOf(f.bridge);
		const std::vector<feathers::DeviceInstanceId> arrangement = lane.order();

		//-- REMOVE FROM THE MIDDLE, AND UNDO IT ----------------------------------------------------//
		REQUIRE(lane.remove(1));
		CHECK(lane.count() == arrangement.size() - 1);
		// THE DEVICE BELOW THE HOLE CAME UP, by serial.
		CHECK(lane.order()[1] == arrangement[2]);
		// AND ITS ROUTING CAME WITH IT, to the place it moved to.
		CHECK(f.bridge.routeAmount(macro, destOf(1)) == doctest::Approx(0.3f));

		REQUIRE(history->canUndo());
		history->undo();
		CHECK(lane.order() == arrangement);
		// EVERY ROUTE, so one moved somewhere nobody named is caught too — and so that the two
		// DIRECTIONS are both covered without the case knowing whether this lane's devices publish
		// outputs. The rack's do and the fx lane's do not; the comparison does not care.
		CHECK(routeListOf(f.bridge) == table);

		//-- REORDER, AND UNDO IT -------------------------------------------------------------------//
		REQUIRE(lane.move(2, 0));
		CHECK(lane.order()[0] == arrangement[2]);
		CHECK(f.bridge.routeAmount(macro, destOf(0)) == doctest::Approx(0.3f));
		history->undo();
		CHECK(lane.order() == arrangement);
		CHECK(routeListOf(f.bridge) == table);

		//-- WHAT THE MENU OFFERS AT THE ENDS -------------------------------------------------------//
		const size_t last = lane.count() - 1;
		CHECK_FALSE(lane.moveUpOffered(0));
		CHECK(lane.moveDownOffered(0));
		CHECK(lane.moveUpOffered(last));
		CHECK_FALSE(lane.moveDownOffered(last));
		CHECK(lane.removeOffered(0));
		CHECK(lane.removeOffered(last));

		//-- AND A SLOT THAT HOLDS NOTHING REFUSES --------------------------------------------------//
		CHECK_FALSE(lane.remove(lane.maxSlots));
		CHECK_FALSE(lane.move(0, lane.maxSlots));
		CHECK(lane.order() == arrangement);
	};

	SUBCASE("the fx lane") {
		FilterFixture f;
		auto* lane = f.lane;
		REQUIRE(lane != nullptr);
		Lane adapter {
			"fx lane",
			feathers::kMaxFxSlots,
			[lane](size_t i) { return lane->addChoiceForTest(i); },
			[lane] { return lane->deviceCount(); },
			[lane](size_t s) { return lane->removeDeviceAtLane(s); },
			[lane](size_t a, size_t b) { return lane->moveDevice(a, b); },
			[&f](size_t s) { return f.bridge.fxDeviceAt(s); },
			[&f, lane] {
				std::vector<feathers::DeviceInstanceId> out;
				for (size_t s = 0; s < lane->deviceCount(); ++s) {
					out.push_back(feathers::instanceOfFxSlot(f.bridge.fxChain(), s));
				}
				return out;
			},
			// BY VERB, NOT BY ROW — the fx menu gained "Add before" at the top (backlog CR).
			[lane](size_t s) { return offeredVerb(lane->slotOptionsForTest(s), ui::FxLane::SlotVerb::MoveUp); },
			[lane](size_t s) { return offeredVerb(lane->slotOptionsForTest(s), ui::FxLane::SlotVerb::MoveDown); },
			[lane](size_t s) { return offeredVerb(lane->slotOptionsForTest(s), ui::FxLane::SlotVerb::Remove); },
		};
		exercise(f, adapter);
	}

	SUBCASE("the modulator rack") {
		FilterFixture f;
		auto* panel = f.panel;
		REQUIRE(panel != nullptr);
		Lane adapter {
			"modulator rack",
			feathers::kMaxRackSlots,
			[panel](size_t i) { return panel->addChoiceForTest(i); },
			[panel] { return panel->deviceCount(); },
			[panel](size_t s) { return panel->removeDeviceAtLane(s); },
			[panel](size_t a, size_t b) { return panel->moveDevice(a, b); },
			[&f](size_t s) { return f.bridge.rackDeviceAt(s); },
			[&f, panel] {
				std::vector<feathers::DeviceInstanceId> out;
				const feathers::ModRack rack = f.bridge.modRack();
				for (size_t s = 0; s < panel->deviceCount(); ++s) {
					out.push_back(rack.device[s].instanceId);
				}
				return out;
			},
			[panel](size_t s) { return offeredVerb(panel->slotOptionsForTest(s), ui::ModPanel::SlotVerb::MoveUp); },
			[panel](size_t s) { return offeredVerb(panel->slotOptionsForTest(s), ui::ModPanel::SlotVerb::MoveDown); },
			[panel](size_t s) { return offeredVerb(panel->slotOptionsForTest(s), ui::ModPanel::SlotVerb::Remove); },
		};
		exercise(f, adapter);
	}
}
TEST_CASE("a drop indicator is a GAP, and the two gaps beside a row are a no-op") {
	// The drop indicator highlights the gap ("bumper") before the row where the item will actually
	// land, not a row. That is a change of model. A row
	// index differs from where the thing lands by one at every position, and cannot say "before the
	// first" or "after the last" at all — which are the two places a reorder is least obvious.
	//
	// N ROWS HAVE N+1 GAPS, and this pins the arithmetic that turns one into a destination. It is the
	// piece all three lists share, so getting it wrong is getting it wrong three times.

	SUBCASE("gapAlong answers by the row's MIDPOINT, so each half of a row means a different gap") {
		const std::array<ui::RowSpan, 3> rows { ui::RowSpan { 0.0f, 10.0f },
			                                    ui::RowSpan { 10.0f, 20.0f },
			                                    ui::RowSpan { 20.0f, 30.0f } };
		const std::span<const ui::RowSpan> span { rows };
		// ABOVE EVERYTHING is the first gap, which is how "before the first row" is expressible.
		CHECK(ui::gapAlong(-50.0f, span) == 0);
		CHECK(ui::gapAlong(0.0f, span) == 0);
		CHECK(ui::gapAlong(4.9f, span) == 0);
		// Past the first midpoint, the gap AFTER the first row.
		CHECK(ui::gapAlong(5.1f, span) == 1);
		CHECK(ui::gapAlong(14.9f, span) == 1);
		CHECK(ui::gapAlong(15.1f, span) == 2);
		CHECK(ui::gapAlong(24.9f, span) == 2);
		// AND PAST EVERY MIDPOINT IS THE LAST GAP — "after the last row", the other thing a row index
		// could not say.
		CHECK(ui::gapAlong(25.1f, span) == 3);
		CHECK(ui::gapAlong(500.0f, span) == 3);
		// An empty list has exactly one gap and every point is in it.
		CHECK(ui::gapAlong(7.0f, {}) == 0);
	}

	SUBCASE("rowForGap refuses the two gaps beside the dragged row") {
		// THE OFF-BY-ONE THIS EXISTS FOR. The gap above the dragged row and the gap below it are both
		// the place it already is. A drop in either must move NOTHING — not "move by zero", which
		// would still spend an undo step and still announce itself.
		constexpr size_t kRows = 4;
		CHECK_FALSE(ui::rowForGap(1, 1, kRows).has_value());
		CHECK_FALSE(ui::rowForGap(1, 2, kRows).has_value());
		// Every OTHER gap is a real move, including both ends.
		CHECK(ui::rowForGap(1, 0, kRows) == 0);
		CHECK(ui::rowForGap(1, 3, kRows) == 2);
		CHECK(ui::rowForGap(1, 4, kRows) == 3);

		// THE FIRST ROW: its two no-op gaps are 0 and 1, so it can go anywhere below.
		CHECK_FALSE(ui::rowForGap(0, 0, kRows).has_value());
		CHECK_FALSE(ui::rowForGap(0, 1, kRows).has_value());
		CHECK(ui::rowForGap(0, 2, kRows) == 1);
		CHECK(ui::rowForGap(0, 4, kRows) == 3);

		// THE LAST ROW: its two are 3 and 4.
		CHECK(ui::rowForGap(3, 0, kRows) == 0);
		CHECK(ui::rowForGap(3, 2, kRows) == 2);
		CHECK_FALSE(ui::rowForGap(3, 3, kRows).has_value());
		CHECK_FALSE(ui::rowForGap(3, 4, kRows).has_value());

		// AND NONSENSE IS REFUSED rather than clamped, because a clamp here is a silent move.
		CHECK_FALSE(ui::rowForGap(0, 0, 0).has_value());
		CHECK_FALSE(ui::rowForGap(4, 0, kRows).has_value());
		CHECK_FALSE(ui::rowForGap(0, 5, kRows).has_value());
	}

	SUBCASE("and the destination is what a bumper physically means, checked without the formula") {
		// AN INDEPENDENT RESTATEMENT, because re-deriving `gap - (gap > from)` in the test would be
		// the same arithmetic wearing a disguise and would agree with any bug in it.
		//
		// WHAT A BUMPER MEANS, PHYSICALLY: put a marker in the gap; lift the dragged row out; the
		// marker is standing where the row should go. That is modelled here with an actual marker in
		// an actual vector, and it knows nothing about the formula.
		const std::vector<char> original { 'A', 'B', 'C', 'D' };
		for (size_t from = 0; from < original.size(); ++from) {
			for (size_t gap = 0; gap <= original.size(); ++gap) {
				CAPTURE(from);
				CAPTURE(gap);

				std::vector<char> marked = original;
				marked.insert(marked.begin() + static_cast<long>(gap), '*');
				const char dragged = original[from];
				marked.erase(std::find(marked.begin(), marked.end(), dragged));
				const auto marker = static_cast<size_t>(
				    std::find(marked.begin(), marked.end(), '*') - marked.begin());

				const std::optional<size_t> answer = ui::rowForGap(from, gap, original.size());
				if (!answer.has_value()) {
					// A NO-OP MUST BE A NO-OP IN THE MODEL TOO: the marker is standing exactly where
					// the dragged row came from.
					CHECK(marker == from);
					continue;
				}
				CHECK(*answer == marker);

				// AND THE MOVE REALLY PRODUCES THAT ORDER, through the same splice both engine roads
				// perform — erase, then insert at the answer.
				std::vector<char> moved = original;
				moved.erase(moved.begin() + static_cast<long>(from));
				moved.insert(moved.begin() + static_cast<long>(*answer), dragged);

				std::vector<char> expected = marked;
				*std::find(expected.begin(), expected.end(), '*') = dragged;
				CHECK(moved == expected);
			}
		}
	}
}
TEST_CASE("the bumper can say things a highlighted row could not") {
	// THE POINT OF THE WHOLE CHANGE, and the two positions the old model had no way to name. A row
	// index can only ever mean "this sound" — so "above the first" and "below the last" were
	// unreachable, and they are precisely where a reorder is least obvious.
	//
	// EVERY AIM HERE IS ASKED OF THE WIDGET (`gapBounds`) rather than restated, because a case that
	// recomputes a widget's geometry is a case that can disagree with it — which is how this file's
	// own drag cases came to aim at a card's centre and mean something else once a slot grew taller
	// than its card.
	StackFixture f;
	f.consume(0, 300.0);
	f.consume(1, 500.0);
	f.consume(2, 700.0);
	f.stack.setBounds(0.0f, 0.0f, SoundStack::kWidth, 400.0f);
	REQUIRE(f.stack.visibleCount() == 3);

	const auto aim = [&](size_t gap) {
		const visage::Bounds b = f.stack.gapBounds(gap);
		return visage::Point { b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f };
	};

	SUBCASE("the gaps are in order, between the cards, and there is one more of them than there are rows") {
		// N ROWS, N+1 GAPS — stated as geometry so it cannot be satisfied by an arithmetic that
		// happens to count right while drawing in the wrong place.
		float last = -1.0f;
		for (size_t gap = 0; gap <= 3; ++gap) {
			CAPTURE(gap);
			const visage::Bounds b = f.stack.gapBounds(gap);
			CHECK(b.height() > 0.0f);
			CHECK(b.y() > last);
			last = b.y();
		}
		// The first gap is ABOVE the first card and the last is BELOW the last card.
		CHECK(f.stack.gapBounds(0).y() < f.stack.cardBoundsForTest(0).y());
		const visage::Bounds lastCard = f.stack.cardBoundsForTest(2);
		CHECK(f.stack.gapBounds(3).y() >= lastCard.y() + f.stack.slotHeightForTest(2) - 1.0f);
		// And a gap past the end is refused rather than clamped.
		CHECK(f.stack.gapBounds(4).height() == 0.0f);
	}

	SUBCASE("a sound can be dropped ABOVE THE FIRST, which no row index could ask for") {
		f.press(f.cardPoint(2));
		f.dragTo(aim(0));
		REQUIRE(f.stack.dropGap().has_value());
		CHECK(*f.stack.dropGap() == 0);
		f.release(aim(0));
		f.settle();

		// THE VALUE: the third sound is now the first, and the other two moved down one.
		CHECK(f.zoneOf(0) == doctest::Approx(700.0));
		CHECK(f.zoneOf(1) == doctest::Approx(300.0));
		CHECK(f.zoneOf(2) == doctest::Approx(500.0));
	}

	SUBCASE("and BELOW THE LAST, which no row index could ask for either") {
		f.press(f.cardPoint(0));
		f.dragTo(aim(3));
		REQUIRE(f.stack.dropGap().has_value());
		CHECK(*f.stack.dropGap() == 3);
		f.release(aim(3));
		f.settle();

		CHECK(f.zoneOf(0) == doctest::Approx(500.0));
		CHECK(f.zoneOf(1) == doctest::Approx(700.0));
		CHECK(f.zoneOf(2) == doctest::Approx(300.0));
	}

	SUBCASE("and the two gaps beside the dragged sound move nothing and spend no undo step") {
		// NOT "MOVE BY ZERO". A drop where the sound already is must leave the history alone, or a
		// hand that picks a card up and puts it back has silently filled the undo stack with edits
		// that changed nothing — and the next real undo appears to do nothing.
		const std::array<SlotState, kMaxSounds> before = f.everySlot();
		REQUIRE(f.history.length() == 0);

		for (const size_t gap : { size_t {1}, size_t {2} }) {
			CAPTURE(gap);
			f.press(f.cardPoint(1));
			f.dragTo(aim(gap));
			REQUIRE(f.stack.dropGap().has_value());
			CHECK(*f.stack.dropGap() == gap);
			f.release(aim(gap));
			f.settle();

			CHECK(f.everySlot() == before);
			CHECK(f.history.length() == 0);
		}

		// AND THE GESTURE STILL WORKS AFTERWARDS — a no-op must not leave the drag latched.
		CHECK_FALSE(f.stack.dragSlot().has_value());
		CHECK_FALSE(f.stack.dropGap().has_value());
	}
}

