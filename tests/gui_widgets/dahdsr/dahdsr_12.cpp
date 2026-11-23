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

// PIECE 12 OF 12 of the dahdsr suite: its cases in their original order, with the helpers only they use. See dahdsr_common.h.

#include "dahdsr/dahdsr_common.h"

TEST_CASE("a folded modulator card still draws its head - folded, not hidden (backlog CZ)") {
	// **MEASURED ON THE INSTALLED f33e3d3 WITH A REAL POINTER: double-click the DAHDSR's head and
	// the strip shows a BLANK BAND — no head, no name, no square.** Bisected against a4b5813, before
	// the selection arc: the same frame, byte for byte. The strip HID a minimised card
	// (`setVisible(false)`) from the day the card frame took over drawing the head, and the panel's
	// own draw loop had already stopped painting one — so a folded card had nothing left to draw
	// with. `hitAt` still answered `Part::Fold` for the band, which is how it stayed reopenable and
	// how nobody's case noticed: a case that folds and asserts `cardMinimised` is green on a card
	// nobody can see.
	//
	// THE FIX IS THE FX LANE'S FOLD: the card is told `setFolded` and stays VISIBLE, reporting its
	// head height and hiding its own children — `GenericCard` already did all three; the envelope
	// and the MSEG now do too. **ASSERTED AS THE VALUES THE PAINT READS**: a visible frame of head
	// height at the card's rectangle inside the strip, the frame at the head point being that card,
	// its boxes hidden, the rows below moved up — and the double-click that folds it goes through
	// the real head, both ways.
	namespace dsr = feathers::mod::dahdsr;
	ModFixture f;
	auto* panel = f.panel;
	REQUIRE(panel != nullptr);
	REQUIRE(f.bridge.addRackDevice(dsr::kTraits.tag) == 1);
	panel->resized();
	REQUIRE(panel->deviceCount() == 2);

	const backplane::Source source = backplane::Source::output(f.bridge.rackDeviceAt(0), 0);
	auto* card = dynamic_cast<ui::FxCard*>(const_cast<visage::Frame*>(panel->cardFrameForTest(0)));
	auto* below = dynamic_cast<ui::FxCard*>(const_cast<visage::Frame*>(panel->cardFrameForTest(1)));
	REQUIRE(card != nullptr);
	REQUIRE(below != nullptr);
	REQUIRE(card->isVisible());
	REQUIRE_FALSE(card->folded());
	const float openHeight = static_cast<float>(card->height());
	const float belowOpenY = static_cast<float>(below->y());
	REQUIRE(openHeight > ui::kCardHeadHeight);
	size_t boxes = 0;
	for (visage::Frame* child : card->children()) {
		if (child->isVisible()) {
			++boxes;
		}
	}
	REQUIRE(boxes > 0);

	// THE HEAD, LEFT OF THE MARKS, in window coordinates — the same point before and after.
	const auto headPoint = [&] {
		const visage::Bounds head = panel->foldBounds(source);
		return panel->positionInWindow()
		       + visage::Point { head.x() + head.width() * 0.4f, head.y() + head.height() * 0.5f };
	};
	REQUIRE(f.root->frameAtPoint(headPoint()) == static_cast<visage::Frame*>(card));

	doubleClickHead(f.root, headPoint());
	REQUIRE(panel->cardMinimised(source));

	// **THE HEAD IS DRAWN**: the card is a visible frame of head height at the card's rectangle.
	CHECK(card->folded());
	CHECK(card->isVisible());
	CHECK(static_cast<float>(card->height()) == doctest::Approx(ui::kCardHeadHeight));
	CHECK(static_cast<float>(card->width()) == doctest::Approx(ui::ModPanel::kCardWidth));
	CHECK(card->y() >= 0);
	CHECK(card->y() + card->height() <= panel->height());
	{
		const visage::Bounds head = panel->foldBounds(source);
		CHECK(static_cast<float>(card->x()) == doctest::Approx(head.x()));
		CHECK(static_cast<float>(card->y()) == doctest::Approx(head.y()));
		CHECK(static_cast<float>(card->height()) == doctest::Approx(head.height()));
	}
	// AND IT IS THE CARD A PRESS ON THAT HEAD FINDS — not the panel behind a blank band.
	CHECK(f.root->frameAtPoint(headPoint()) == static_cast<visage::Frame*>(card));
	// ITS BOXES WENT WITH THE BODY — a child left visible under a folded head is still pressable.
	for (visage::Frame* child : card->children()) {
		CHECK_FALSE(child->isVisible());
	}
	// THE OUTPUT STRIP IS BODY TOO, so its arm has no rectangle while folded.
	CHECK(panel->outputChipBounds(source).width() == 0.0f);
	// AND THE CARD BELOW MOVED UP BY WHAT THE FOLD TOOK.
	CHECK(static_cast<float>(below->y())
	      == doctest::Approx(belowOpenY - (openHeight - ui::kCardHeadHeight)));

	// THE WAY BACK, THROUGH THE SAME HEAD.
	doubleClickHead(f.root, headPoint());
	REQUIRE_FALSE(panel->cardMinimised(source));
	CHECK_FALSE(card->folded());
	CHECK(card->isVisible());
	CHECK(static_cast<float>(card->height()) == doctest::Approx(openHeight));
	size_t shown = 0;
	for (visage::Frame* child : card->children()) {
		if (child->isVisible()) {
			++shown;
		}
	}
	CHECK(shown == boxes);
	CHECK(static_cast<float>(below->y()) == doctest::Approx(belowOpenY));
}
TEST_CASE("arming a device source selects its card, whichever road armed it - Delete takes the lit card") {
	// **The lit (armed) card is the one Delete takes.** Implemented as ARMING SELECTS: `ModState::arm(true)` tells one listener, the
	// strip's, and the strip selects the device the armed source belongs to — so whichever road arms
	// a device source, the lit card and the outlined card are the same card and Delete keeps resolving
	// the selection as it does. Disarming says nothing, so it deselects nothing.
	//
	// **WHICH ROADS EXIST, READ RATHER THAN ASSUMED:** the card's own chip (`SourceArm`, which already
	// selected on its press since 3fd50d0), the Engine rail's `SourceGrid` — which arms SOUND WEIGHTS,
	// not devices, so it cannot light a card and is not a road here — and `ModPanel::focusOrArm`'s
	// arrow, which reaches only the macros because the chips sit on top of it. So the road that needs
	// this today is the one nobody has written yet; the setter road below stands in for it, which is
	// exactly what makes the listener the right place: a road that arms without pressing the card.
	namespace dsr = feathers::mod::dahdsr;
	ModFixture f;
	auto* panel = f.panel;
	REQUIRE(panel != nullptr);
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);
	REQUIRE(f.bridge.addRackDevice(dsr::kTraits.tag) == 1);
	panel->resized();
	REQUIRE(panel->deviceCount() == 2);

	const auto idAt = [&](size_t slot) { return f.bridge.modRack().device[slot].instanceId; };
	const auto cardAt = [&](size_t slot) {
		auto* card = dynamic_cast<ui::FxCard*>(const_cast<visage::Frame*>(panel->cardFrameForTest(slot)));
		REQUIRE(card != nullptr);
		return card;
	};
	const backplane::Source srcB = backplane::Source::output(f.bridge.rackDeviceAt(1), 0);

	// A IS SELECTED FIRST, so a road that selects nothing leaves a WRONG card rather than none.
	panel->selectSlot(0);
	REQUIRE(panel->selectedInstanceForTest() == idAt(0));
	REQUIRE(cardAt(0)->frameStyle().outline);
	REQUIRE_FALSE(cardAt(1)->frameStyle().outline);

	// THE LIT CARD IS THE OUTLINED CARD — one predicate, read of both cards.
	const auto litIsSelected = [&](const char* after) {
		CAPTURE(after);
		for (size_t slot = 0; slot < 2; ++slot) {
			CAPTURE(slot);
			const bool selected = panel->selectedInstanceForTest() == idAt(slot);
			CHECK(cardAt(slot)->frameStyle().outline == selected);
			if (cardAt(slot)->frameStyle().glow) {
				CHECK(selected);
			}
		}
	};

	SUBCASE("a road that arms B without any press on its card — the setter road") {
		mod->focus(srcB);
		mod->arm(true);
		REQUIRE(mod->armed());
		CHECK(cardAt(1)->frameStyle().glow);
		// **THE RULE, AS A VALUE.**
		CHECK(panel->selectedInstanceForTest() == idAt(1));
		CHECK(panel->selectedSlotForTest() == 1);
		CHECK(cardAt(1)->frameStyle().outline);
		CHECK_FALSE(cardAt(0)->frameStyle().outline);
		litIsSelected("after arming B by the setter");

		// AND DELETE TAKES THE LIT CARD. Through the panel's handler, which is where the walk arrives
		// from the card the listener focused.
		const feathers::DeviceInstanceId lit = idAt(1);
		const feathers::DeviceInstanceId other = idAt(0);
		REQUIRE(keyFromFocus(panel, visage::KeyEvent { visage::KeyCode::Delete, 0, true }));
		REQUIRE(f.bridge.modRack().count == 1);
		CHECK(f.bridge.modRack().device[0].instanceId == other);
		CHECK(f.bridge.modRack().device[0].instanceId != lit);
	}

	SUBCASE("the chip road says the same thing") {
		f.arm(srcB);
		REQUIRE(mod->armed());
		CHECK(panel->selectedInstanceForTest() == idAt(1));
		litIsSelected("after arming B from its chip");
	}

	SUBCASE("disarming does not deselect") {
		mod->focus(srcB);
		mod->arm(true);
		REQUIRE(panel->selectedInstanceForTest() == idAt(1));
		mod->arm(false);
		CHECK(panel->selectedInstanceForTest() == idAt(1));
		CHECK(cardAt(1)->frameStyle().outline);
		CHECK_FALSE(cardAt(1)->frameStyle().glow);
		litIsSelected("after disarming B");
	}

	SUBCASE("arming a source with no card — a macro — leaves the selection where it was") {
		mod->focus(backplane::Source::core(0));
		mod->arm(true);
		REQUIRE(mod->armed());
		CHECK(panel->selectedInstanceForTest() == idAt(0));
		litIsSelected("after arming macro 1");
	}
}
TEST_CASE("right-clicking a modulated destination lists its inbound sources, and picking one arms it") {
	// **BACKLOG DI.** A right-click on a modulated param lists its sources; selecting one in the context
	// menu is the same as entering mod mode for that source, so its amount can be fine-tuned from the
	// destination.
	//
	// **THIS IS THE FIX FOR THE PROBLEM `ModDisplay::routedElsewhere` DOCUMENTS.** That flag exists
	// because `amount` is about the FOCUSED source only, so every OTHER route to a control draws as
	// nothing — an evening was once lost to a live route that looked untouched. The flag discloses THAT
	// something is routed; this discloses WHAT, and hands it to you.
	//
	// **DRIVEN THROUGH `showRouteMenu()` RATHER THAN A SYNTHESISED RIGHT-CLICK.** X11 pointer
	// synthesis is banned on the development display — `widget-shot` exists because of that ban — so the
	// gesture's entry point is public and a case calls it.
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	lane->setCollapsed(false);
	f.gui.tickForTest();

	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);

	std::vector<ui::Knob*> knobs;
	collectKnobs(lane, &knobs);
	REQUIRE(!knobs.empty());

	ui::Knob* knob = nullptr;
	for (ui::Knob* k : knobs) {
		if (k->control() != nullptr && k->control()->display().isDestination) {
			knob = k;
			break;
		}
	}
	REQUIRE(knob != nullptr);

	SUBCASE("a destination with NO inbound route shows nothing and lets the press fall through") {
		// **AN EMPTY MENU IS A GESTURE THAT ANSWERED NOTHING.** Returning false is what leaves the
		// ordinary press road intact, so a right-click on an unrouted knob is not silently swallowed.
		CHECK_FALSE(knob->showRouteMenu());
	}

	SUBCASE("with a route in, the control can name it and the menu opens") {
		const backplane::Source source = armingSource();
		// **THE ROUTE IS MADE THROUGH THE CONTROL'S OWN ROAD**, which is the one a drag takes:
		// `setAmount` writes the FOCUSED source, so focusing first is both how the editor does it and
		// what makes this independent of any destination arithmetic the case would otherwise have to
		// restate.
		mod->focus(source);
		mod->arm(true);
		knob->control()->beginAmountGesture();
		knob->control()->setAmount(0.4f);
		knob->control()->endAmountGesture();
		f.gui.tickForTest();

		// **THE NEW SEAM, ASSERTED AS A VALUE.** `display().amount` could have answered this while
		// the source is focused; `amountFor` is the question that survives the focus moving, which is
		// the whole reason it exists.
		CHECK(knob->control()->amountFor(source) == doctest::Approx(0.4f));

		// **AND IT STILL ANSWERS ONCE THE FOCUS HAS MOVED ON** — which is exactly the case
		// `routedElsewhere` was invented to paper over, and the one a menu has to get right.
		const backplane::Source other = backplane::Source::core(source.index + 1);
		mod->focus(other);
		CHECK(knob->control()->amountFor(source) == doctest::Approx(0.4f));
		CHECK(knob->control()->display().amount == doctest::Approx(0.0f));

		// AND THE MENU OPENS, which it refused to do a moment ago with nothing routed.
		REQUIRE(knob->showRouteMenu());

		// **THE LIST IS THE ROUTE, BY VALUE.**
		const auto inbound = knob->inboundSources();
		REQUIRE(inbound.size() == 1);
		CHECK(inbound[0] == source);

		SUBCASE("picking it focuses AND arms — which is the whole gesture it was asked for") {
			// The editor is left focused elsewhere and disarmed, so both halves have somewhere to move
			// from and neither can pass by already being true.
			mod->arm(false);
			REQUIRE(mod->source() != source);
			REQUIRE_FALSE(mod->armed());

			knob->chooseInbound(source);

			CHECK(mod->source() == source);
			CHECK(mod->armed());
			// **AND THE KNOB NOW EDITS THAT ROUTE'S AMOUNT**, which is the point of arming from the
			// destination: `display().amount` is the focused source's reach, and it is this route's.
			CHECK(knob->control()->display().amount == doctest::Approx(0.4f));
			CHECK(knob->control()->display().armed);
		}

		SUBCASE("Remove all clears every route and puts the focus back where it was") {
			// **ONE ROW, NOT ONE PER SOURCE** — one row removes all modulators, because the per-route verb
			// already existed as the
			// double-click on an armed destination; what the menu adds is clearing them together.
			const backplane::Source focusBefore = mod->source();
			CHECK(knob->removeAllInbound() == 1);
			CHECK(knob->control()->amountFor(source) == doctest::Approx(0.0f));
			// AND THE MENU HAS NOTHING LEFT TO SHOW, which is the same refusal an unrouted control
			// makes — so removal really removed rather than zeroing a route that still exists.
			CHECK(knob->inboundSources().empty());
			CHECK_FALSE(knob->showRouteMenu());
			// **THE FOCUS IS PUT BACK.** Clearing walks the focus across the routes it deletes, and a
			// delete that silently re-aimed the panel would be an editing side effect nobody asked for.
			CHECK(mod->source() == focusBefore);
		}
	}

	SUBCASE("a source below the removal threshold is not a route, and is not listed") {
		// **THE MENU'S MEMBERSHIP IS THE SAME TEST THE DRAG USES**, `kAmountUnroutedBelow` — so a
		// route this menu lists is one that exists, not one sitting at an amount the drag would have
		// deleted on the way past.
		const backplane::Source source = armingSource();
		mod->focus(source);
		mod->arm(true);
		knob->control()->beginAmountGesture();
		knob->control()->setAmount(feathers::device::kAmountUnroutedBelow * 0.5f);
		knob->control()->endAmountGesture();
		f.gui.tickForTest();
		CHECK_FALSE(knob->showRouteMenu());
	}
}
TEST_CASE("the route menu is generic: a VALUE BOX offers the same gesture, from the same code") {
	// The menu is generic to anything that implements modulation, and this is the case
	// that says so — the same three verbs on a widget that is not a knob, reached through a control
	// that was never handed the panel state by the widget. The state comes from the CONTROL
	// (`ui::RoutedControl`), which is what makes the gesture free of per-widget wiring: the seven
	// `ValueBox` construction sites needed no change, and none of them can forget it.
	namespace bu = feathers::fx::busutil;
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	lane->setCollapsed(false);
	f.gui.tickForTest();

	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);

	// A DEVICE CONTROL, held by a bare `ValueBox` built here — the widget a card would build, with no
	// modulation call of any kind made on it.
	const size_t slot = f.bridge.addFxDevice(bu::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	lane->resized();
	ui::DeviceControl control { f.deviceHost, f.bridge.fxDeviceAt(slot), bu::kDescriptor,
		                        static_cast<size_t>(bu::Param::Pan), mod, nullptr };
	ui::ValueBox box { control, "Pan" };

	REQUIRE(control.display().isDestination);
	// NOTHING ROUTED YET: no menu, and the press falls through.
	CHECK(ui::inboundSources(control).empty());
	CHECK_FALSE(ui::showRouteMenu(box, control));

	const backplane::Source source = armingSource();
	mod->focus(source);
	mod->arm(true);
	control.beginAmountGesture();
	control.setAmount(0.35f);
	control.endAmountGesture();

	// **THE SAME THREE ANSWERS AS THE KNOB'S, THROUGH THE SHARED CODE.**
	const auto inbound = ui::inboundSources(control);
	REQUIRE(inbound.size() == 1);
	CHECK(inbound[0] == source);
	CHECK(control.amountFor(source) == doctest::Approx(0.35f));

	mod->arm(false);
	ui::chooseInbound(control, source);
	CHECK(mod->armed());
	CHECK(mod->source() == source);

	CHECK(ui::removeAllInbound(control) == 1);
	CHECK(ui::inboundSources(control).empty());
}
TEST_CASE("EVERY output chip a device declares lands inside its own card") {
	// **THE GUARD WHOSE ABSENCE LET THREE BUGS THROUGH IN ONE DAY, all of them the same shape: every
	// device that had ever existed declared 0 or 1 modulation outputs, so "the outputs strip" was
	// implicitly "a strip for one chip" and nothing said so.**
	//
	// The defect: only three mod outputs were visible. The Note Expressions device declares
	// seven; the chips were laid right-to-left in ONE fixed-height row with no wrap and no clamp, so
	// the fourth onward had negative x and were drawn outside the card. Nothing in the geometry was
	// wrong for one chip and nothing in it was right for seven.
	//
	// **SWEPT OVER THE REGISTRY RATHER THAN OVER A LIST**, so a device added tomorrow joins this claim
	// without anybody remembering to — which is the property that would have caught it the first time.
	ModFixture f;
	visage::Canvas canvas;

	size_t checked = 0;
	bool checkedMultiOutput = false;
	for (const feathers::device::Descriptor& desc : feathers::kDevices) {
		if (desc.outputCount() == 0 || !feathers::maySitInModRack(desc)) {
			continue;
		}
		CAPTURE(std::string(desc.traits.name));

		// PUT IT IN A SLOT AND LET THE PANEL LAY ITSELF OUT, rather than computing what the geometry
		// ought to be — the bug was IN the geometry, so a restatement of it here would have agreed
		// with the bug.
		// **THROUGH THE BRIDGE'S OWN ADD, not a hand-built rack.** A rack assembled here and restored
		// wholesale left the panel holding cards for slots that no longer matched, and the first read
		// walked a null. The gesture road is the one every other panel case uses and the one a player
		// takes.
		// **ADDED, CHECKED, THEN REMOVED — because the rack holds four and the registry has more.**
		// The first draft only added, so the rack filled and every device after the fourth was skipped
		// with a `continue`. The device this case exists for is registered LAST and was one of the
		// skipped ones, which is why a known-bad forcing one row passed: the sweep never met a device
		// whose chips wrap.
		const size_t slot = f.bridge.addRackDevice(desc.traits.tag);
		REQUIRE(slot < feathers::kMaxRackSlots);
		f.panel->draw(canvas);
		f.panel->resized();
		f.panel->draw(canvas);

		const visage::Bounds card = f.panel->envCardBounds(slot);
		REQUIRE(card.width() > 0.0f);
		REQUIRE(card.height() > 0.0f);

		for (size_t output = 0; output < desc.outputCount(); ++output) {
			CAPTURE(output);
			const backplane::Source source = backplane::Source::output(f.bridge.rackDeviceAt(slot), output);
			const visage::Bounds chip = f.panel->outputChipBounds(source);
			if (((f.bridge.outputsShown(source.device) >> output) & 1u) == 0u) {
				CHECK(chip.width() == 0.0f); // NOT ON THE CARD (design §7.10)
				continue;
			}
			// **A CHIP THAT EXISTS MUST BE SOMEWHERE A HAND CAN REACH.** Width first: a zero-width
			// answer is the shape a clipped chip would also have, and would pass every bound below.
			REQUIRE(chip.width() > 0.0f);
			CHECK(chip.x() >= card.x());
			CHECK(chip.right() <= card.right());
			CHECK(chip.y() >= card.y());
			CHECK(chip.bottom() <= card.bottom());
			++checked;
		}
		if (desc.outputCount() > 1) {
			checkedMultiOutput = true;
		}
		REQUIRE(f.bridge.removeRackDevice(slot));
	}

	// NON-VACUITY, and it is not merely "something ran": the bug needed a device with MORE THAN ONE
	// output to appear at all, so a sweep that only met single-output devices would pass exactly as it
	// did before the fix.
	CHECK(checked > 0);
	// **THAT THE SWEEP REACHED A MULTI-OUTPUT DEVICE, not that the REGISTRY holds one.** The first
	// draft asserted the second, which is true whether or not the loop ever got there — and it did not,
	// because the rack filled. A non-vacuity guard that reads a different source from the thing it is
	// guarding is not a guard; a known-bad forcing one row passed against it.
	CHECK(checkedMultiOutput);
}
TEST_CASE("backlog EL: the OWLTT graph reads the band lights and the combined gain reduction through the bridge, "
          "and a publisher that still sends ten leaves them dark and zero") {
	// THE VALUES, NOT THE PICTURE: the graph's `meters()` is the seam the draw reads from, so a light
	// that is on here is a light the draw has to paint (`update()` marks it stale on the change).
	FilterFixture f;
	REQUIRE(f.bridge.addFxDevice(fx::owltt::kTraits.tag) == 1);
	ui::OwlttGraph graph { f.deviceHost, f.bridge.fxDeviceAt(1) };
	REQUIRE(f.deviceHost.readingCountAt(f.bridge.fxDeviceAt(1)) == feathers::fx::owltt::Owltt::kReadingCount); // + input peak (18), pre-clip output peak (19), second-channel GR, backlog LQ

	// THE OLD FIVE ARGUMENTS ONLY: the extras default dark and zero, as a device publishing ten does.
	f.bridge.publishOwlttMeters(1, { -0.5f, 0.5f, -0.5f }, { 0.5f, 0.5f, 0.5f }, { 0.4f, 0.4f, 0.4f }, false);
	ui::OwlttGraph::Meters m = graph.meters();
	CHECK(m.bandSaturating == std::array<bool, 3> { false, false, false });
	CHECK(m.combinedGrDb == 0.0f);
	REQUIRE(graph.update()); // the first read is always a change from the zero it was drawn at

	// WITH THE EXTRAS: the lights and the readout arrive by index, band for band.
	f.bridge.publishOwlttMeters(1, { -0.5f, 0.5f, -0.5f }, { 0.5f, 0.5f, 0.5f }, { 0.4f, 0.4f, 0.4f }, false,
	                            { true, false, true }, 3.25f);
	m = graph.meters();
	CHECK(m.bandSaturating == std::array<bool, 3> { true, false, true });
	CHECK(m.combinedGrDb == doctest::Approx(3.25f));
	CHECK(f.deviceHost.readingAt(f.bridge.fxDeviceAt(1), 10) == 1.0f);
	CHECK(f.deviceHost.readingAt(f.bridge.fxDeviceAt(1), 11) == 0.0f);
	CHECK(f.deviceHost.readingAt(f.bridge.fxDeviceAt(1), 12) == 1.0f);
	CHECK(f.deviceHost.readingAt(f.bridge.fxDeviceAt(1), 13) == doctest::Approx(3.25f));
	// AND A CHANGE IN THE EXTRAS ALONE IS A REPAINT: the ten originals are unchanged here.
	CHECK(graph.update());
	CHECK_FALSE(graph.update()); // nothing moved since
}
TEST_CASE("backlog ML: OWLTT's master clip light, on the output level meter, has three states — near from the pre-clip output peak (-2 dBFS to just under 0), clip from its own held flag or at full scale") {
	FilterFixture f;
	REQUIRE(f.bridge.addFxDevice(fx::owltt::kTraits.tag) == 1);
	ui::OwlttLevelMeter meter { f.deviceHost, f.bridge.fxDeviceAt(1), ui::OwlttLevelMeter::Side::Output };
	const auto stateAt = [&](double dbfs, bool flag) {
		f.bridge.publishOwlttMeters(1, { 0.0f, 0.0f, 0.0f }, { 0.5f, 0.5f, 0.5f }, { 0.4f, 0.4f, 0.4f }, flag, { false, false, false },
		                            0.0f, nullptr, 0.0f, 0.0f, static_cast<float>(std::pow(10.0, dbfs / 20.0)));
		return meter.read().state;
	};
	// NO PEAK PUBLISHED: the flag alone decides.
	f.bridge.publishOwlttMeters(1, { 0.0f, 0.0f, 0.0f }, { 0.5f, 0.5f, 0.5f }, { 0.4f, 0.4f, 0.4f }, false);
	CHECK(meter.read().state == ui::ClipLedState::Normal);
	f.bridge.publishOwlttMeters(1, { 0.0f, 0.0f, 0.0f }, { 0.5f, 0.5f, 0.5f }, { 0.4f, 0.4f, 0.4f }, true);
	CHECK(meter.read().state == ui::ClipLedState::Clip);

	CHECK(stateAt(-2.1, false) == ui::ClipLedState::Normal);
	CHECK(stateAt(-1.0, false) == ui::ClipLedState::Near);
	CHECK(stateAt(0.0, false) == ui::ClipLedState::Clip);
	CHECK(stateAt(-20.0, true) == ui::ClipLedState::Clip); // OWLTT's own held over is still a clip
	// AND A CHANGE OF STATE ALONE IS A REPAINT.
	(void)stateAt(-20.0, false);
	(void)meter.update();
	(void)stateAt(-1.0, false);
	CHECK(meter.update());
}
TEST_CASE("every control the DAHDSR declares is reachable from its card — the Mode collapse took Legato down with Loop") {
	// **A CONTROL NO SURFACE REACHES IS A CONTROL NOBODY HAS** (the honesty rule, from the other end:
	// there a drawn control must change the sound, here a control that changes the sound must be
	// drawn). The device declares it, the state stream carries it, the fingerprint covers it, the
	// engine reads it every tick — and the hand cannot move it.
	//
	// **IT SHIPPED THAT WAY IN THE MODE COLLAPSE.** `fd07c7f4` folded Loop into the new Mode dropdown
	// and correctly deleted `Chip { Param::Loop, "Loop" }`; standing next to it in the same array was
	// `Chip { Param::SingleTrigger, "Leg" }`, which Mode did NOT swallow — `Settings::singleTrigger`
	// survives as its own field, `"Env Legato"` survives in `kParamDescs`, and both of its readers
	// survive in `dahdsr.cpp` (`mayRetrigger`'s first term, and the legato take-up at the note-on
	// site). The chip went anyway, `kChipCount` went 5 → 3, and the six assertions in
	// `test_mod_dahdsr.cpp` that set the field were deleted in the same commit. `grep -rn singleTrigger
	// src/` then returned four hits and not one writer: nothing in `src/plugin`, nothing in `src/rack`,
	// no card, and `modulatable: false` keeps it off every route. The switch was unsettable.
	//
	// **WHICH ALSO DISARMED A FIX.** The regression the handoff's §1 found and repaired — `mayRetrigger`
	// never consulting `singleTrigger` — is correct and could not fire, because the term it added reads
	// a field with no road to it.
	//
	// ASKED OF THE WHOLE ENUM rather than of Legato by name, so the NEXT control dropped out of a
	// refactor fails this too. `addressedParamsForTest` is the seam, and it is asserted non-vacuous
	// before it is believed — a card claiming to reach nothing does not fail a sweep, it empties one.
	namespace dsr = feathers::mod::dahdsr;
	ModFixture f;
	f.panel->resized();
	// THROUGH THE PANEL, because `hasEnable()` is the shared head's and a card built in isolation has
	// no head to ask. The strip's slot 0 is the pinned envelope; asserted rather than assumed, or this
	// whole sweep could be run against somebody else's card and report a clean bill for it.
	const auto* card = dynamic_cast<const ui::EnvCard*>(f.panel->cardFrameForTest(0));
	REQUIRE(card != nullptr);

	const std::vector<dsr::Param> reached = card->addressedParamsForTest();
	REQUIRE(reached.size() >= static_cast<size_t>(dsr::Param::kCount));

	for (int i = 0; i < static_cast<int>(dsr::Param::kCount); ++i) {
		const auto param = static_cast<dsr::Param>(i);
		// `CAPTURE` on a `const char*` prints the POINTER — playbook. `std::string` is what makes the
		// failure name the control instead of a number.
		CAPTURE(std::string(dsr::kParamDescs[static_cast<size_t>(i)].name));
		CHECK(std::find(reached.begin(), reached.end(), param) != reached.end());
	}

	// AND NO CHIP CARRIES A CONTINUOUS CONTROL — a chip is a switch or a cycle, asked of the
	// descriptor's own `stepped` flag so a chip added later has to answer it too. The LFO's card is
	// where this one was paid for; the rule is the house's, not that card's.
	for (const ui::EnvCard::Chip& chip : ui::EnvCard::chips()) {
		CAPTURE(std::string(dsr::kParamDescs[static_cast<size_t>(chip.param)].name));
		CHECK(dsr::kParamDescs[static_cast<size_t>(chip.param)].stepped);
	}

	// AND EVERY BOX SITS ON A DIFFERENT CONTROL, or eight boxes could be one control eight times and
	// the sweep above would still be satisfied by the chips alone.
	const auto& boxes = ui::EnvCard::layout();
	std::vector<dsr::Param> sorted(boxes.begin(), boxes.end());
	std::sort(sorted.begin(), sorted.end());
	CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
}
TEST_CASE("the drawn curve is the SHAPE the controls set, whatever the smoothing: the sustain plateau sits at the sustain level and the release falls to the floor (\"R shows as off even though it's not, sustain shows as 100% even though it's not ... only sometimes\")") {
	// THE PICTURE IS THE SHAPE, NOT THE ROUTE. `Envelope::rawValue` says so in as many words — "what a card
	// draws... smoothing is a property of the route, not of the envelope" — and the card was drawing
	// `tick()`'s return, the SMOOTHED value. Its preview runs every stage in the same 48 ticks whatever the
	// stage's length, so a one-pole of real seconds, applied per preview tick, smears the picture by an
	// amount that has nothing to do with the shape: the plateau never gets down to the sustain level and
	// the release never gets to the floor. The reference below is the declaration, restated.
	ModFixture f;
	feathers::ui::ModState mod;
	feathers::ui::EditHistory history;
	ui::EnvCard card { f.deviceHost, f.bridge.rackDeviceAt(0), mod, history };
	card.setBounds(0, 0, static_cast<int>(ui::ModPanel::kCardWidth), static_cast<int>(ui::EnvCard::height()));
	namespace dsr = feathers::mod::dahdsr;

	for (const float smoothing : { feathers::mod::kSmoothingLog2SecondsMin, -5.0f, -2.0f,
	                               feathers::mod::kSmoothingLog2SecondsMax }) {
		CAPTURE(smoothing);
		dsr::Settings s = f.bridge.modRack().dahdsrAt(0);
		s.mode = dsr::Mode::Gate;
		s.bipolar = false;
		s.attack = dsr::stageValueFor(0.010);
		s.decay = dsr::stageValueFor(0.100);
		s.sustain = 0.3f;
		s.release = dsr::stageValueFor(0.500);
		s.smoothing = smoothing;
		f.bridge.setEnvelopeSettings(0, s);

		const std::vector<visage::Point> pts = card.curvePoints();
		REQUIRE(pts.size() > 8);
		const float decayEnd = card.stageEndX(dsr::Stage::Decay);
		const float sustainEnd = card.stageEndX(dsr::Stage::Sustain);
		REQUIRE(sustainEnd > decayEnd);
		// THE PLATEAU: every point strictly inside the sustain segment is the sustain level.
		int onPlateau = 0;
		float plateauWorst = 0.0f;
		for (const visage::Point& p : pts) {
			if (p.x > decayEnd + 1e-4f && p.x < sustainEnd - 1e-4f) {
				++onPlateau;
				plateauWorst = std::max(plateauWorst, std::abs(p.y - 0.3f));
			}
		}
		CAPTURE(plateauWorst);
		REQUIRE(onPlateau > 0);
		CHECK(plateauWorst < 1e-4f);
		// THE PEAK: the attack reaches the top.
		float peak = 0.0f;
		for (const visage::Point& p : pts) {
			peak = std::max(peak, p.y);
		}
		CAPTURE(peak);
		CHECK(peak > 0.999f);
		// THE RELEASE: it ends on the floor.
		CAPTURE(pts.back().y);
		CHECK(pts.back().x == doctest::Approx(1.0f));
		CHECK(pts.back().y == 0.0f);
	}
}
TEST_CASE("the envelope card's Poly chip: it is the device's Poly switch, live in every mode but Free, where it is dimmed and refuses the press (the honesty rule)") {
	// a Poly switch for envs. Free is ONE shape whatever the switch says (`mod::PolyShape::poly`),
	// so there the chip changes nothing and must say so rather than toggle a stored value nobody hears.
	namespace dsr = feathers::mod::dahdsr;
	ModFixture f;
	feathers::ui::ModState mod;
	feathers::ui::EditHistory history;
	ui::EnvCard card { f.deviceHost, f.bridge.rackDeviceAt(0), mod, history };
	card.setBounds(0, 0, static_cast<int>(ui::ModPanel::kCardWidth), static_cast<int>(ui::EnvCard::height()));
	size_t poly = ui::EnvCard::kChipCount;
	for (size_t i = 0; i < ui::EnvCard::kChipCount; ++i) {
		if (ui::EnvCard::chips()[i].param == dsr::Param::Poly) {
			poly = i;
		}
	}
	REQUIRE(poly < ui::EnvCard::kChipCount);
	dsr::Settings s = f.bridge.modRack().dahdsrAt(0);
	REQUIRE(s.poly); // ON by default, as declared
	CHECK(card.chipOn(poly));
	for (const dsr::Mode mode : { dsr::Mode::Gate, dsr::Mode::Loop, dsr::Mode::Once, dsr::Mode::LfoRetrig }) {
		s.mode = mode;
		f.bridge.setEnvelopeSettings(0, s);
		CHECK(card.chipLive(poly));
	}
	s.mode = dsr::Mode::Free;
	f.bridge.setEnvelopeSettings(0, s);
	CHECK_FALSE(card.chipLive(poly));
	// A PRESS ON THE DEAD CHIP CHANGES NOTHING.
	const visage::Bounds b = card.chipBounds(poly);
	REQUIRE(b.width() > 0.0f);
	card.onMouseDown().callback(at(b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f));
	CHECK(f.bridge.modRack().dahdsrAt(0).poly);
	// AND LIVE AGAIN, IT TOGGLES THE DEVICE'S OWN FIELD.
	s.mode = dsr::Mode::Gate;
	f.bridge.setEnvelopeSettings(0, s);
	card.onMouseDown().callback(at(b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f));
	CHECK_FALSE(f.bridge.modRack().dahdsrAt(0).poly);
}

namespace {
//------------------------------------------------------------------------------------------------//
// THE TRIG PAD (backlog MV addendum) — a trigger destination is not a knob                          //
//------------------------------------------------------------------------------------------------//

/// The centre of a frame, in the root's coordinates — where a hand presses it.
visage::Point trigPadCentre(const ui::TrigPad* pad) {
	return pad->positionInWindow() + visage::Point { pad->width() * 0.5f, pad->height() * 0.5f };
}
} // namespace

TEST_CASE("the Trig pad: a press fires the envelope by hand — the Trig held at 1 for the press and back at 0 after") {
	namespace dsr = feathers::mod::dahdsr;
	ModFixture f;
	f.panel->resized();
	auto* card = dynamic_cast<const ui::EnvCard*>(f.panel->cardFrameForTest(0));
	REQUIRE(card != nullptr);
	ui::TrigPad* pad = card->trigPadForTest();
	REQUIRE(pad != nullptr);
	const DeviceInstanceId id = f.bridge.rackDeviceAt(0);
	const size_t trig = static_cast<size_t>(dsr::Param::Trig);
	REQUIRE(f.deviceHost.fieldAt(id, trig) == 0.0f);

	pad->press();
	CHECK(f.deviceHost.fieldAt(id, trig) == 1.0f); // held at the top: the edge sees it rise
	CHECK(pad->lit());                              // lit under the hand
	pad->release();
	CHECK(f.deviceHost.fieldAt(id, trig) == 0.0f);
	CHECK_FALSE(pad->lit());

	// AND THROUGH THE REAL TREE: a click at the pad lands on it and leaves the Trig where it rests.
	dragThroughRoot(f.root, trigPadCentre(pad), trigPadCentre(pad));
	CHECK(f.deviceHost.fieldAt(id, trig) == 0.0f);
}
TEST_CASE("the Trig pad: with a source armed, a click routes it at depth 1 — the mapping set for you — and a second click unroutes it") {
	namespace mseg = feathers::mod::mseg;
	ModFixture f;
	const size_t msegSlot = f.bridge.addRackDevice(mseg::kTraits.tag);
	REQUIRE(msegSlot >= 1);
	f.bridge.refresh();
	f.panel->resized();
	auto* card = dynamic_cast<const ui::MsegCard*>(f.panel->cardFrameForTest(msegSlot));
	REQUIRE(card != nullptr);
	ui::TrigPad* pad = card->trigPadForTest();
	REQUIRE(pad != nullptr);
	const backplane::Source envelope = backplane::Source::output(f.bridge.rackDeviceAt(0), 0);
	const backplane::Destination dest =
	    backplane::Destination::control(f.bridge.rackDeviceAt(msegSlot), static_cast<size_t>(mseg::Param::Trig));
	REQUIRE(f.bridge.routeAmount(envelope, dest) == 0.0f);
	REQUIRE_FALSE(pad->mappable());

	f.arm(envelope);
	REQUIRE(pad->mappable());
	dragThroughRoot(f.root, trigPadCentre(pad), trigPadCentre(pad));
	CHECK(f.bridge.routeAmount(envelope, dest) == 1.0f);
	// NOT A FIRE: an armed press routes and leaves the Trig itself at rest.
	CHECK(f.deviceHost.fieldAt(f.bridge.rackDeviceAt(msegSlot), static_cast<size_t>(mseg::Param::Trig)) == 0.0f);

	pad->press(false, 2); // the second click of a pair
	pad->release();
	CHECK(f.bridge.routeAmount(envelope, dest) == 0.0f);
}
TEST_CASE("the Trig pad lights on each fire, for a flash, and not for a count it found already standing") {
	ModFixture f;
	f.panel->resized();
	auto* card = dynamic_cast<const ui::EnvCard*>(f.panel->cardFrameForTest(0));
	REQUIRE(card != nullptr);
	uint32_t count = 5;
	bool published = true;
	ui::TrigPad pad(card->trigPadForTest()->control(), [&](uint32_t& fires) {
		fires = count;
		return published;
	});
	CHECK_FALSE(pad.poll(1.0)); // the count standing at 5 is where it is, not a fire
	CHECK_FALSE(pad.lit());
	++count;
	CHECK(pad.poll(1.01));
	CHECK(pad.lit());
	CHECK_FALSE(pad.poll(1.01 + ui::TrigPad::kFlashSeconds * 0.5)); // still lit, nothing changed
	CHECK(pad.lit());
	CHECK(pad.poll(1.01 + ui::TrigPad::kFlashSeconds + 0.001));
	CHECK_FALSE(pad.lit());
	// A HOST THAT PUBLISHES NO COUNT NEVER LIGHTS IT.
	published = false;
	count += 3;
	CHECK_FALSE(pad.poll(2.0));
	CHECK_FALSE(pad.lit());
}
TEST_CASE("the Trig pad lights from the engine's own fire count, through the bridge's channel") {
	// THE ROAD THE LIGHT TAKES: the device counts (`PolyShape::trigFires`), the synth publishes it beside the playhead,
	// the bridge carries it, and `DeviceHost::trigFiresAt` answers the card. Published here as the audio side would.
	ModFixture f;
	const DeviceInstanceId id = f.bridge.rackDeviceAt(0);
	uint32_t fires = 0;
	CHECK_FALSE(f.deviceHost.trigFiresAt(id, fires)); // nobody has published one
	feathers::mod::Playhead where { 0u, 0.0f };
	where.trigFires = 7;
	where.haveTrigFires = true;
	f.bridge.publishRackPlayhead(0, where);
	REQUIRE(f.deviceHost.trigFiresAt(id, fires));
	CHECK(fires == 7u);
}
TEST_CASE("every modulator card with a Trig seats its pad at the shape's left, inside the body, clear of the shape") {
	namespace lfo = feathers::mod::lfo;
	ModFixture f;
	const size_t msegSlot = f.bridge.addRackDevice(feathers::mod::mseg::kTraits.tag);
	REQUIRE(msegSlot >= 1);
	const size_t lfoSlot = f.bridge.addRackDevice(lfo::kTraits.tag);
	REQUIRE(lfoSlot >= 1);
	f.bridge.refresh();
	f.panel->resized();
	struct Seat {
		const char* what;
		const ui::FxCard* card;
		visage::Bounds pad;
		visage::Bounds shape;
	};
	std::vector<Seat> seats;
	if (auto* c = dynamic_cast<const ui::EnvCard*>(f.panel->cardFrameForTest(0)); c != nullptr) {
		seats.push_back({ "DAHDSR", c, c->trigPadBounds(), c->curveBounds() });
	}
	if (auto* c = dynamic_cast<const ui::MsegCard*>(f.panel->cardFrameForTest(msegSlot)); c != nullptr) {
		seats.push_back({ "MSEG", c, c->trigPadBounds(), c->editorBounds() });
	}
	if (auto* c = dynamic_cast<const ui::LfoCard*>(f.panel->cardFrameForTest(lfoSlot)); c != nullptr) {
		seats.push_back({ "LFO", c, c->trigPadBounds(), c->editorBounds() });
	}
	REQUIRE(seats.size() == 3);
	for (const Seat& s : seats) {
		CAPTURE(std::string(s.what));
		CHECK(s.pad.width() == ui::TrigPad::kDiameter);
		CHECK(s.pad.x() >= 0.0f);
		CHECK(s.pad.right() <= s.shape.x());                              // left of the shape, not on it
		CHECK(s.pad.y() >= s.shape.y());                                  // and within its height
		CHECK(s.pad.bottom() <= s.shape.bottom());
		CHECK(s.pad.y() >= ui::kCardHeadHeight);                  // never in the header (by design)
	}
}

namespace {
//------------------------------------------------------------------------------------------------//
// BACKLOG MN — the DAHDSR's curves take the MSEG's gesture ("per segment         //
// curvature for DAHSDR same gesture as mseg"). The three curves existed; these pin the gesture.   //
//------------------------------------------------------------------------------------------------//

namespace mn_dsr = feathers::mod::dahdsr;
/// AN ENVELOPE WHOSE THREE CURVED STAGES ARE WIDE ENOUGH TO AIM AT: half a second each, no delay or hold,
/// sustain at a half, every curve straight. `mode` decides whether the picture is one run or two cycles.
mn_dsr::Settings mnWide(mn_dsr::Mode mode = mn_dsr::Mode::Gate) {
	mn_dsr::Settings s;
	s.delay = mn_dsr::kStageLog2SecondsMin;
	s.hold = mn_dsr::kStageLog2SecondsMin;
	s.attack = mn_dsr::stageValueFor(0.5);
	s.decay = mn_dsr::stageValueFor(0.5);
	s.release = mn_dsr::stageValueFor(0.5);
	s.sustain = 0.5f;
	s.attackShape = 0.0f;
	s.decayShape = 0.0f;
	s.releaseShape = 0.0f;
	s.mode = mode;
	return s;
}
/// The drawn curve's value at unit x, from the point nearest it (the card samples each stage at 24 points, so
/// a stage's midpoint is one of them).
float mnCurveAt(const std::vector<visage::Point>& points, float x) {
	const auto nearest = std::min_element(points.begin(), points.end(), [x](const visage::Point& a, const visage::Point& b) {
		return std::abs(a.x - x) < std::abs(b.x - x);
	});
	return nearest->y;
}
/// Every Trig pad under `root`, in tree order.
void collectTrigPads(visage::Frame* root, std::vector<ui::TrigPad*>* out) {
	if (auto* pad = dynamic_cast<ui::TrigPad*>(root)) {
		out->push_back(pad);
	}
	for (visage::Frame* child : root->children()) {
		collectTrigPads(child, out);
	}
}
/// A route of `source` into `control` at `amount`, made the way a drag makes one: focused, armed, bracketed.
void routeInto(ui::ModState& mod, feathers::device::Modulatable& control, backplane::Source source, float amount) {
	mod.focus(source);
	mod.arm(true);
	control.beginAmountGesture();
	control.setAmount(amount);
	control.endAmountGesture();
	mod.arm(false);
}
} // namespace

TEST_CASE("MN: a drag on a DAHDSR stage bends that stage's curve and only it, by the MSEG's travel, and the drawn midpoint is the curve law") {
	// THE VALUE IN THE ROW, NOT THE PICTURE (CLAUDE.md): the engine reads `attackShape` & co., so that is what is
	// asserted; the drawn midpoint is then checked against `stageCurve`, the law the engine plays, by value.
	ModFixture f;
	feathers::ui::ModState mod;
	feathers::ui::EditHistory history;
	f.bridge.setEnvelopeSettings(0, mnWide());
	ui::EnvCard card { f.deviceHost, f.bridge.rackDeviceAt(0), mod, history };
	card.setBounds(0, 0, static_cast<int>(ui::ModPanel::kCardWidth), static_cast<int>(ui::EnvCard::height()));
	const visage::Bounds box = card.curveBounds();

	struct Case {
		const char* name;
		mn_dsr::Stage stage;
		mn_dsr::Stage before; ///< the stage whose end is this one's start
		float mn_dsr::Settings::*field;
	};
	const Case cases[] = {
		{ "attack", mn_dsr::Stage::Attack, mn_dsr::Stage::Delay, &mn_dsr::Settings::attackShape },
		{ "decay", mn_dsr::Stage::Decay, mn_dsr::Stage::Hold, &mn_dsr::Settings::decayShape },
		{ "release", mn_dsr::Stage::Release, mn_dsr::Stage::Sustain, &mn_dsr::Settings::releaseShape },
	};
	for (const Case& c : cases) {
		CAPTURE(c.name);
		f.bridge.setEnvelopeSettings(0, mnWide());
		const float from = card.stageEndX(c.before);
		const float to = card.stageEndX(c.stage);
		REQUIRE(to - from > 0.1f);
		const float midUnit = 0.5f * (from + to);
		const float x = box.x() + midUnit * box.width();
		const float y = box.y() + 0.5f * box.height();

		// THE MIDPOINT AT ZERO IS THE STRAIGHT LINE — the identity, by value.
		const mn_dsr::Settings flat = mnWide();
		const auto lawAt = [&](float shape) -> double {
			const double f05 = mn_dsr::stageCurve(0.5, shape);
			switch (c.stage) {
			case mn_dsr::Stage::Attack: return f05;                                            // 0 -> 1
			case mn_dsr::Stage::Decay: return flat.sustain + (1.0 - flat.sustain) * f05;       // read backwards: 1-p = 0.5
			default: return flat.sustain * f05;                                                // release from sustain
			}
		};
		CHECK(mnCurveAt(card.curvePoints(), midUnit) == doctest::Approx(lawAt(0.0f)).epsilon(2e-4));

		// A QUARTER OF THE BOX UPWARD IS +0.5 — the MSEG's rate, a full bend per half box — as a number.
		const uint64_t serialBefore = history.lastSerial();
		card.onMouseDown().callback(at(x, y));
		CHECK(card.highlightForTest().kind == ui::EnvCard::BendTarget::Kind::Stage);
		CHECK(card.highlightForTest().stages == ui::EnvCard::BendTarget::bit(c.stage));
		// FOUR MOVES TO ONE PLACE: absolute from the anchor, so the answer is the place, not the route.
		const float travel = 0.25f * box.height();
		for (int step = 1; step <= 4; ++step) {
			card.onMouseDrag().callback(at(x, y - travel * static_cast<float>(step) / 4.0f));
		}
		card.onMouseUp().callback(at(x, y - travel));
		CHECK(card.highlightForTest().kind == ui::EnvCard::BendTarget::Kind::None);

		const mn_dsr::Settings bent = f.bridge.modRack().dahdsrAt(0);
		CHECK(bent.*c.field == doctest::Approx(0.5f).epsilon(0.02));
		// ITS NEIGHBOURS UNTOUCHED, and nothing else in the row either.
		mn_dsr::Settings expect = mnWide();
		expect.*c.field = bent.*c.field;
		CHECK(bent == expect);
		// AND THE DRAWN MIDPOINT IS THE LAW AT THE STORED CURVE, and no longer the line.
		CHECK(mnCurveAt(card.curvePoints(), midUnit) == doctest::Approx(lawAt(bent.*c.field)).epsilon(2e-4));
		CHECK(std::abs(lawAt(bent.*c.field) - lawAt(0.0f)) > 0.05);

		// ONE UNDO STEP, which takes the curve back to exactly zero, and redo puts the bend back.
		CHECK(history.lastSerial() != serialBefore);
		history.undo();
		CHECK(f.bridge.modRack().dahdsrAt(0).*c.field == 0.0f);
		history.redo();
		CHECK(f.bridge.modRack().dahdsrAt(0).*c.field == bent.*c.field);
	}

	SUBCASE("a press that moves nothing records nothing, and a press over a flat stage bends nothing") {
		f.bridge.setEnvelopeSettings(0, mnWide());
		const float sustainX =
		    box.x() + 0.5f * (card.stageEndX(mn_dsr::Stage::Decay) + card.stageEndX(mn_dsr::Stage::Sustain)) * box.width();
		const float y = box.y() + 0.5f * box.height();
		const uint64_t serial = history.lastSerial();
		card.onMouseDown().callback(at(sustainX, y));
		CHECK(card.highlightForTest().kind == ui::EnvCard::BendTarget::Kind::None);
		card.onMouseDrag().callback(at(sustainX, y - 0.25f * box.height()));
		card.onMouseUp().callback(at(sustainX, y - 0.25f * box.height()));
		CHECK(f.bridge.modRack().dahdsrAt(0) == mnWide());
		const float attackX = box.x() + 0.5f * card.stageEndX(mn_dsr::Stage::Attack) * box.width();
		card.onMouseDown().callback(at(attackX, y));
		card.onMouseUp().callback(at(attackX, y));
		CHECK(history.lastSerial() == serial);
	}
}
TEST_CASE("MN: an Opt-drag on the DAHDSR's peak bends attack and decay together, lights both, and undoes as one step") {
	ModFixture f;
	feathers::ui::ModState mod;
	feathers::ui::EditHistory history;
	f.bridge.setEnvelopeSettings(0, mnWide());
	ui::EnvCard card { f.deviceHost, f.bridge.rackDeviceAt(0), mod, history };
	card.setBounds(0, 0, static_cast<int>(ui::ModPanel::kCardWidth), static_cast<int>(ui::EnvCard::height()));
	const visage::Bounds box = card.curveBounds();
	using Bend = ui::EnvCard::BendTarget;

	// THE PEAK: the attack's end, two pixels under the top of the box.
	const float peakX = box.x() + card.stageEndX(mn_dsr::Stage::Attack) * box.width();
	const float peakY = box.y() + 2.0f;
	const uint8_t both = Bend::bit(mn_dsr::Stage::Attack) | Bend::bit(mn_dsr::Stage::Decay);

	const Bend caught = card.bendTargetAt({ peakX, peakY }, true);
	CHECK(caught.kind == Bend::Kind::Peak);
	CHECK(caught.stages == both);
	// WITHOUT OPT THE SAME POINT IS THE STAGE UNDER IT, ONE CURVE: the modifier is load-bearing.
	CHECK(card.bendTargetAt({ peakX, peakY }, false).kind == Bend::Kind::Stage);
	CHECK(card.bendTargetAt({ peakX, peakY }, false).stages == Bend::bit(mn_dsr::Stage::Attack));
	// AND OPT AWAY FROM THE PEAK IS STILL A STAGE, so Opt does not take the single-stage bend away.
	const float lowY = box.bottom() - 2.0f;
	CHECK(card.bendTargetAt({ peakX + 30.0f, lowY }, true).kind == Bend::Kind::Stage);

	// THE OPT-HOVER LIGHTS IT BEFORE ANY PRESS, and leaving the card puts it out.
	card.onMouseMove().callback(optAt(peakX, peakY));
	CHECK(card.highlightForTest() == caught);
	card.onMouseExit().callback(optAt(peakX, peakY));
	CHECK(card.highlightForTest().kind == Bend::Kind::None);

	card.onMouseDown().callback(optAt(peakX, peakY));
	CHECK(card.highlightForTest() == caught);
	const float travel = 0.25f * box.height();
	card.onMouseDrag().callback(optAt(peakX, peakY + travel)); // DOWN: both sides sag, -0.5 each
	card.onMouseUp().callback(optAt(peakX, peakY + travel));

	const mn_dsr::Settings bent = f.bridge.modRack().dahdsrAt(0);
	CHECK(bent.attackShape == doctest::Approx(-0.5f).epsilon(0.02));
	CHECK(bent.decayShape == doctest::Approx(-0.5f).epsilon(0.02));
	CHECK(bent.releaseShape == 0.0f);
	// THE SAME SIGN ON BOTH, which on THIS device is the MSEG's inverse pair: the peak's two shoulders move with
	// the pointer together. Read off the law: below the line on both sides of the peak.
	CHECK(mn_dsr::stageCurve(0.5, bent.attackShape) < 0.5);       // attack: under its line
	CHECK(mn_dsr::stageCurve(0.5, bent.decayShape) < 0.5);        // decay (read backwards): under its line too

	history.undo();
	CHECK(f.bridge.modRack().dahdsrAt(0) == mnWide()); // ONE step takes both back
	history.redo();
	CHECK(f.bridge.modRack().dahdsrAt(0) == bent);

	SUBCASE("a peak with no attack bends the decay alone") {
		mn_dsr::Settings noAttack = mnWide();
		noAttack.attack = mn_dsr::kStageLog2SecondsMin;
		f.bridge.setEnvelopeSettings(0, noAttack);
		const float decayStart = box.x() + card.stageEndX(mn_dsr::Stage::Hold) * box.width();
		const Bend peak = card.bendTargetAt({ decayStart, peakY }, true);
		CHECK(peak.kind == Bend::Kind::Peak);
		CHECK(peak.stages == Bend::bit(mn_dsr::Stage::Decay));
	}

	SUBCASE("withBend clamps to the curve range and moves only the stages it names") {
		mn_dsr::Settings s = mnWide();
		s.releaseShape = 0.3f;
		const mn_dsr::Settings out = ui::EnvCard::withBend(s, caught, 5.0f);
		CHECK(out.attackShape == mn_dsr::kShapeMax);
		CHECK(out.decayShape == mn_dsr::kShapeMax);
		CHECK(out.releaseShape == 0.3f);
		CHECK(ui::EnvCard::withBend(s, caught, 0.0f) == s);
	}
}
TEST_CASE("MN: a looping DAHDSR's FIRST cycle bends too, not only the second") {
	// THE DEFECT THIS FOUND: the drag asked `stageAtX`, which answers from each stage's LAST edge, so while the
	// shape loops (two cycles drawn) everything left of the SECOND attack's end read as Attack: a drag on the first
	// decay bent the attack.
	ModFixture f;
	feathers::ui::ModState mod;
	feathers::ui::EditHistory history;
	f.bridge.setEnvelopeSettings(0, mnWide(mn_dsr::Mode::Loop));
	ui::EnvCard card { f.deviceHost, f.bridge.rackDeviceAt(0), mod, history };
	card.setBounds(0, 0, static_cast<int>(ui::ModPanel::kCardWidth), static_cast<int>(ui::EnvCard::height()));
	const visage::Bounds box = card.curveBounds();
	// TWO CYCLES OF ATTACK AND DECAY, half a second each: the first decay spans 0.25..0.5 of the width.
	REQUIRE(card.stageEndX(mn_dsr::Stage::Decay) == doctest::Approx(1.0f).epsilon(1e-4));
	const float x = box.x() + 0.375f * box.width();
	const float y = box.y() + 0.5f * box.height();
	CHECK(card.bendTargetAt({ x, y }, false).stages == ui::EnvCard::BendTarget::bit(mn_dsr::Stage::Decay));

	card.onMouseDown().callback(at(x, y));
	card.onMouseDrag().callback(at(x, y - 0.25f * box.height()));
	card.onMouseUp().callback(at(x, y - 0.25f * box.height()));
	CHECK(f.bridge.modRack().dahdsrAt(0).decayShape == doctest::Approx(0.5f).epsilon(0.02));
	CHECK(f.bridge.modRack().dahdsrAt(0).attackShape == 0.0f);
}
TEST_CASE("MV: every Trig pad's right-click lists the routes into it, and removing one removes that one") {
	// BACKLOG MV's open item: the pad gets the route menu every other destination has (`mod_route_menu.h`), on every
	// card that seats one — DAHDSR, MSEG, LFO, Random Trigger, Sample & Hold, and the sequencer's Advance and Reset.
	ModFixture f;
	for (const char* tag : { feathers::mod::mseg::kTraits.tag, feathers::mod::lfo::kTraits.tag,
	                         feathers::mod::rtrig::kTraits.tag, feathers::mod::samplehold::kTraits.tag,
	                         feathers::mod::stepseq::kTraits.tag }) {
		CAPTURE(std::string(tag));
		REQUIRE(f.bridge.addRackDevice(tag) >= 1);
	}
	f.bridge.refresh();
	f.panel->resized();
	f.gui.tickForTest();
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);

	std::vector<ui::TrigPad*> pads;
	for (size_t slot = 0; slot < 6; ++slot) {
		visage::Frame* card = const_cast<visage::Frame*>(static_cast<const visage::Frame*>(f.panel->cardFrameForTest(slot)));
		REQUIRE(card != nullptr);
		collectTrigPads(card, &pads);
	}
	REQUIRE(pads.size() == 12 + 2 * (feathers::mod::lfo::kTabs - 1)); // one per card, seven on the sequencer (Advance, Reset, a Randomise per lane — backlog NT), and one a tab on the LFO and the S&H

	const backplane::Source a = backplane::Source::core(0);
	const backplane::Source b = backplane::Source::core(1);
	for (ui::TrigPad* pad : pads) {
		feathers::device::Modulatable& control = pad->control();
		CAPTURE(std::string(control.desc().name));
		REQUIRE(control.display().isDestination);
		// NOTHING ROUTED: no menu, and the press falls through — unless the pad carries a normal (backlog NU), whose row
		// is a menu of its own.
		CHECK(ui::inboundSources(control).empty());
		CHECK(pad->showRouteMenu() == pad->hasNormal());
		ui::dismissMenuOf(*pad);

		routeInto(*mod, control, a, 1.0f);
		routeInto(*mod, control, b, 0.6f);
		REQUIRE(control.amountFor(a) == 1.0f);
		REQUIRE(control.amountFor(b) == doctest::Approx(0.6f));

		// THE MENU LISTS BOTH ROUTES, BY VALUE — the list it is built from — and opens.
		const std::vector<feathers::device::Source> listed = ui::inboundSources(control);
		REQUIRE(listed.size() == 2);
		CHECK(listed[0] == a);
		CHECK(listed[1] == b);

		// THROUGH THE REAL GESTURE: a right-click on the pad opens it, and fires nothing.
		const double before = control.value();
		visage::MouseEvent e;
		e.position = { pad->width() * 0.5f, pad->height() * 0.5f };
		e.window_position = pad->positionInWindow() + e.position;
		e.event_frame = pad;
		e.button_id = visage::kMouseButtonRight;
		e.button_state = visage::kMouseButtonRight;
		e.is_down = true;
		e.repeat_click_count = 1;
		pad->onMouseDown().callback(e);
		CHECK(ui::menuOpenFor(*pad));
		CHECK_FALSE(pad->lit());
		CHECK(control.value() == before);
		ui::dismissMenuOf(*pad);

		// REMOVING ONE: the pick arms that source, and the pad's second click of a pair unroutes THAT one only.
		ui::chooseInbound(control, a);
		REQUIRE(mod->source() == a);
		REQUIRE(pad->mappable());
		pad->press(false, 2);
		pad->release();
		mod->arm(false);
		CHECK(control.amountFor(a) == 0.0f);
		CHECK(control.amountFor(b) == doctest::Approx(0.6f));
		const std::vector<feathers::device::Source> left = ui::inboundSources(control);
		REQUIRE(left.size() == 1);
		CHECK(left[0] == b);
		CHECK(pad->showRouteMenu());
		ui::dismissMenuOf(*pad);

		// AND THE MENU'S OWN REMOVE ROW TAKES THE REST.
		CHECK(ui::removeAllInbound(control) == 1);
		CHECK(control.amountFor(b) == 0.0f);
		CHECK(pad->showRouteMenu() == pad->hasNormal());
		ui::dismissMenuOf(*pad);
	}
}

namespace {
//------------------------------------------------------------------------------------------------//
// BACKLOG NU — the Trig pad carries the normal: "Notes fire Trig", on its right-click             //
//------------------------------------------------------------------------------------------------//

/// ONE CARD'S PAD, ASKED FOR ITS NORMAL by value: the stored switch, the checked row, the pick, the undo, and a route
/// that never moves it. `normal` is the device's Notes Trig control; `trig` its Trig.
template <typename Card>
void checkTrigNormal(ModFixture& f, size_t slot, size_t normal, size_t trig) {
	ui::ModState mod;
	ui::EditHistory history;
	const DeviceInstanceId id = f.bridge.rackDeviceAt(slot);
	Card card { f.deviceHost, id, mod, history };
	card.setBounds(0, 0, static_cast<int>(ui::ModPanel::kCardWidth), 260);
	ui::TrigPad* pad = card.trigPadForTest();
	REQUIRE(pad != nullptr);

	// MADE BY DEFAULT, and the pad says so.
	REQUIRE(f.deviceHost.fieldAt(id, normal) == 1.0f);
	CHECK(pad->hasNormal());
	CHECK(pad->normalled());

	// THE RIGHT-CLICK WITH NOTHING ROUTED IN IS ONE ROW, CHECKED — a menu now, where there was none.
	{
		visage::PopupMenu menu = pad->menuForTest();
		REQUIRE(menu.size() == 1);
		CHECK(menu.options()[0].id() == ui::TrigPad::kNormalRow);
		CHECK(menu.options()[0].selected());
		// THE PICK BREAKS IT: the stored switch, as one undo step.
		menu.onSelection().callback(ui::TrigPad::kNormalRow);
	}
	CHECK(f.deviceHost.fieldAt(id, normal) == 0.0f);
	CHECK_FALSE(pad->normalled());
	CHECK_FALSE(pad->menuForTest().options()[0].selected());
	history.undo();
	CHECK(f.deviceHost.fieldAt(id, normal) == 1.0f);
	history.redo();
	CHECK(f.deviceHost.fieldAt(id, normal) == 0.0f);

	// A ROUTE INTO TRIG LEAVES IT WHERE IT IS, made or broken, and lists above it behind a break — on the PANEL's card,
	// whose panel state knows the sources a route can come from.
	f.panel->resized();
	auto* panelCard = dynamic_cast<const Card*>(f.panel->cardFrameForTest(slot));
	REQUIRE(panelCard != nullptr);
	ui::TrigPad* panelPad = panelCard->trigPadForTest();
	REQUIRE(panelPad != nullptr);
	const backplane::Source envelope = backplane::Source::output(f.bridge.rackDeviceAt(slot == 0 ? 1 : 0), 0);
	f.arm(envelope);
	for (const float stored : { 0.0f, 1.0f }) {
		CAPTURE(stored);
		panelPad->setNormalled(stored == 1.0f);
		REQUIRE(f.deviceHost.fieldAt(id, normal) == stored);
		REQUIRE(panelPad->mappable());
		panelPad->press(); // armed: routes the source at depth 1
		panelPad->release();
		REQUIRE(f.bridge.routeAmount(envelope, backplane::Destination::control(id, trig)) == 1.0f);
		CHECK(f.deviceHost.fieldAt(id, normal) == stored);
		CHECK(f.deviceHost.fieldAt(id, trig) == 0.0f);
		const visage::PopupMenu menu = panelPad->menuForTest();
		REQUIRE(menu.size() >= 3);
		CHECK(menu.options()[static_cast<size_t>(menu.size() - 2)].isBreak());
		CHECK(menu.options().back().id() == ui::TrigPad::kNormalRow);
		CHECK(menu.options().back().selected() == (stored == 1.0f));
		CHECK(ui::removeAllInbound(panelPad->control()) == 1u);
		CHECK(f.bridge.routeAmount(envelope, backplane::Destination::control(id, trig)) == 0.0f);
		CHECK(f.deviceHost.fieldAt(id, normal) == stored);
	}
}
} // namespace

TEST_CASE("NU: each shape's Trig pad carries Notes fire Trig — a checked row on its right-click, flipping the stored switch as one undo step, never moved by a route") {
	namespace dsr = feathers::mod::dahdsr;
	namespace lfo = feathers::mod::lfo;
	namespace mseg = feathers::mod::mseg;
	ModFixture f;
	const size_t msegSlot = f.bridge.addRackDevice(mseg::kTraits.tag);
	REQUIRE(msegSlot >= 1);
	const size_t lfoSlot = f.bridge.addRackDevice(lfo::kTraits.tag);
	REQUIRE(lfoSlot >= 1);
	f.bridge.refresh();
	SUBCASE("DAHDSR") {
		checkTrigNormal<ui::EnvCard>(f, 0, static_cast<size_t>(dsr::Param::NotesTrig), static_cast<size_t>(dsr::Param::Trig));
	}
	SUBCASE("MSEG") {
		checkTrigNormal<ui::MsegCard>(f, msegSlot, static_cast<size_t>(mseg::Param::NotesTrig),
		                              static_cast<size_t>(mseg::Param::Trig));
	}
	SUBCASE("LFO") {
		checkTrigNormal<ui::LfoCard>(f, lfoSlot, static_cast<size_t>(lfo::Param::NotesTrig),
		                             static_cast<size_t>(lfo::Param::Trig));
	}
}
TEST_CASE("NU: a pad with no normal offers none — Random Trigger's — and its right-click with nothing routed is still no menu") {
	ModFixture f;
	f.panel->resized();
	auto* card = dynamic_cast<const ui::EnvCard*>(f.panel->cardFrameForTest(0));
	REQUIRE(card != nullptr);
	// A PAD BUILT WITHOUT ONE, as Random Trigger's and the sequencer's Advance are.
	ui::TrigPad pad(card->trigPadForTest()->control(), [](uint32_t&) { return false; });
	CHECK_FALSE(pad.hasNormal());
	CHECK_FALSE(pad.normalled());
	CHECK(pad.menuForTest().size() == 0);
	CHECK_FALSE(pad.showRouteMenu());
}

namespace {
//------------------------------------------------------------------------------------------------//
// BACKLOG OD: THE WINDOW CONTROLS' SIDE — a desk preference for which end of a header holds them.
//------------------------------------------------------------------------------------------------//

struct RestoreWindowSide {
	ui::WindowControlsSide before = ui::windowControlsSide();
	~RestoreWindowSide() { (void)ui::setWindowControlsSide(before); }
};
/// A content with a title-row accessory, so the case can place it on either side.
class AccessoryStubView final : public ui::ExpandedView {
public:
	AccessoryStubView() { accessory_.setBounds(0.0f, 0.0f, 60.0f, 20.0f); }
	visage::Point minimumSize() const override { return { 420.0f, 300.0f }; }
	std::string title() const override { return "Side"; }
	visage::Frame* titleAccessory() const override { return const_cast<visage::Frame*>(&accessory_); }

private:
	visage::Frame accessory_;
};
} // namespace

TEST_CASE("OD: a window's close, fold and title sit by the desk's side — x positions per setting, and the head follows a "
          "change without a reopen") {
	RestoreWindowSide restore;
	ui::ExpandedOverlay overlay;
	overlay.setBounds(0, 0, 1240, 700);
	overlay.show(makeStub({ 420.0f, 300.0f }, "Harm"));
	REQUIRE(overlay.open());
	ui::ExpandedPanel* panel = overlay.panelForTest();
	REQUIRE(panel != nullptr);
	constexpr float kPad = ui::ExpandedOverlay::kPanelPad;
	constexpr float kSize = ui::ExpandedOverlay::kCloseSize;
	const float w = panel->width();

	(void)ui::setWindowControlsSide(ui::WindowControlsSide::Right);
	CHECK(panel->closeBounds().x() == doctest::Approx(w - kPad - kSize));
	CHECK(panel->foldBounds().right() < panel->closeBounds().x());
	CHECK(panel->titleX() == doctest::Approx(kPad));

	// THE SIDE MOVES UNDER AN OPEN PANEL, and the overlay's tick is what tells it — counted, since a repaint
	// cannot be observed headlessly.
	(void)ui::setWindowControlsSide(ui::WindowControlsSide::Left);
	CHECK(overlay.updateContent());
	CHECK_FALSE(overlay.updateContent()); // and only once
	CHECK(panel->closeBounds().x() == doctest::Approx(kPad));
	CHECK(panel->foldBounds().x() > panel->closeBounds().right());
	CHECK(panel->foldBounds().right() < w * 0.5f);
	CHECK(panel->titleX() > panel->foldBounds().right());
	// BOTH CLICK TARGETS stay inside the head.
	CHECK(panel->closeBounds().bottom() <= ui::ExpandedOverlay::kTitleHeight);
	// AND THE OVERLAY'S OWN ✕ answer (what a press tests) is the panel's, in window coordinates.
	CHECK(overlay.closeBounds().x() == doctest::Approx(overlay.panelBounds().x() + kPad));
}
TEST_CASE("OD: a title accessory takes the far end on the left setting and stays clear of the title; the ✕ closes "
          "from either side") {
	RestoreWindowSide restore;
	ui::ExpandedOverlay overlay;
	overlay.setBounds(0, 0, 1240, 700);
	int dismissals = 0;
	overlay.onDismiss = [&dismissals] { ++dismissals; };
	for (const auto side : { ui::WindowControlsSide::Right, ui::WindowControlsSide::Left }) {
		CAPTURE(static_cast<int>(side));
		(void)ui::setWindowControlsSide(side);
		overlay.show(std::make_unique<AccessoryStubView>());
		ui::ExpandedPanel* panel = overlay.panelForTest();
		REQUIRE(panel != nullptr);
		panel->resized();
		const visage::Bounds acc = panel->accessoryBounds();
		REQUIRE(acc.width() > 0.0f);
		const visage::Bounds close = panel->closeBounds();
		const visage::Bounds fold = panel->foldBounds();
		const auto clear = [](visage::Bounds a, visage::Bounds b) {
			return a.right() <= b.x() + 0.01f || b.right() <= a.x() + 0.01f;
		};
		CHECK(clear(acc, close));
		CHECK(clear(acc, fold));
		if (side == ui::WindowControlsSide::Left) {
			CHECK(acc.right() == doctest::Approx(panel->width() - ui::ExpandedOverlay::kPanelPad));
			CHECK(acc.x() > panel->titleX());
		}
		else {
			CHECK(acc.right() < fold.x());
		}
		// THE ✕ CLOSES, wherever it is.
		const int before = dismissals;
		panel->onMouseDown().callback(at(close.x() + close.width() * 0.5f, close.y() + close.height() * 0.5f));
		CHECK_FALSE(overlay.open());
		CHECK(dismissals == before + 1);
		CHECK(overlay.lastDismissalForTest() == ui::ExpandedOverlay::Dismissal::CloseButton);
	}
}
TEST_CASE("OV: maximize fills the overlay's rectangle, restore returns the exact earlier bounds, a resize while maximized "
          "follows, and a hand cannot drag or resize it") {
	ui::ExpandedOverlay overlay;
	overlay.setBounds(0, 0, 1240, 700);
	const auto id = overlay.show(makeStub({ 420.0f, 300.0f }, "Harm"));
	overlay.beginPanelDrag();
	overlay.dragPanelBy({ 37.0f, 21.0f });
	overlay.beginPanelResize();
	overlay.resizePanelBy({ 55.0f, 33.0f });
	const visage::Bounds before = overlay.panelBounds(id);
	const visage::Bounds contentBefore = overlay.contentBounds();
	REQUIRE(before.width() > 0.0f);
	REQUIRE_FALSE(overlay.maximized(id));

	overlay.setMaximized(id, true);
	CHECK(overlay.maximized(id));
	const visage::Bounds full = overlay.panelBounds(id);
	CHECK(full.x() == 0.0f);
	CHECK(full.y() == 0.0f);
	CHECK(full.width() == doctest::Approx(1240.0f));
	CHECK(full.height() == doctest::Approx(700.0f));
	// THE PANEL FRAME ITSELF, not only the answer: what is drawn and hit-tested.
	ui::ExpandedPanel* panel = overlay.panelForTest(id);
	REQUIRE(panel != nullptr);
	CHECK(panel->width() == doctest::Approx(1240.0f));
	CHECK(panel->height() == doctest::Approx(700.0f));
	CHECK(panel->gripBounds().width() == 0.0f);

	// A HAND CANNOT MOVE OR RESIZE IT while the overlay owns the rectangle.
	overlay.beginPanelDrag();
	overlay.dragPanelBy({ 200.0f, 100.0f });
	overlay.beginPanelResize();
	overlay.resizePanelBy({ -100.0f, -100.0f });
	CHECK(overlay.panelBounds(id).width() == doctest::Approx(1240.0f));
	CHECK(overlay.panelBounds(id).x() == 0.0f);

	// A RESIZE OF THE EDITOR WHILE MAXIMIZED keeps it filling.
	overlay.setBounds(0, 0, 1000, 640);
	CHECK(overlay.panelBounds(id).width() == doctest::Approx(1000.0f));
	CHECK(overlay.panelBounds(id).height() == doctest::Approx(640.0f));
	CHECK(panel->width() == doctest::Approx(1000.0f));
	overlay.setBounds(0, 0, 1240, 700);

	// RESTORE: THE EXACT BOUNDS, to the pixel — placement and size, and the content inside.
	overlay.setMaximized(id, false);
	CHECK_FALSE(overlay.maximized(id));
	const visage::Bounds after = overlay.panelBounds(id);
	CHECK(after.x() == before.x());
	CHECK(after.y() == before.y());
	CHECK(after.width() == before.width());
	CHECK(after.height() == before.height());
	CHECK(overlay.contentBounds().width() == contentBefore.width());
	CHECK(overlay.contentBounds().height() == contentBefore.height());
	CHECK(panel->width() == before.width());
}
TEST_CASE("OV: maximize and fold exclude each other, the press on the button toggles, and it is view state kept per "
          "device across a close") {
	ui::ExpandedOverlay overlay;
	overlay.setBounds(0, 0, 1240, 700);
	auto id = overlay.show(makeStub({ 420.0f, 300.0f }, "Harm"));
	const visage::Bounds before = overlay.panelBounds(id);
	ui::ExpandedPanel* panel = overlay.panelForTest(id);
	REQUIRE(panel != nullptr);
	const auto press = [&](visage::Bounds b) {
		panel->onMouseDown().callback(at(b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f));
	};

	// MAXIMIZING A FOLDED PANEL UNFOLDS IT.
	overlay.setFolded(id, true);
	REQUIRE(overlay.folded(id));
	press(panel->maximizeBounds());
	CHECK(overlay.maximized(id));
	CHECK_FALSE(overlay.folded(id));
	CHECK(overlay.panelBounds(id).height() == doctest::Approx(700.0f));

	// FOLDING A MAXIMIZED PANEL RESTORES IT FIRST, then folds to the strip where it was.
	overlay.setFolded(id, true);
	CHECK_FALSE(overlay.maximized(id));
	CHECK(overlay.folded(id));
	CHECK(overlay.panelBounds(id).height() == ui::ExpandedOverlay::kTitleHeight);
	overlay.setFolded(id, false);
	CHECK(overlay.panelBounds(id).x() == before.x());
	CHECK(overlay.panelBounds(id).width() == before.width());

	// THE SECOND PRESS RESTORES.
	press(panel->maximizeBounds());
	REQUIRE(overlay.maximized(id));
	press(panel->maximizeBounds());
	CHECK_FALSE(overlay.maximized(id));
	CHECK(overlay.panelBounds(id).width() == before.width());

	// KEPT PER DEVICE for this layer's life, as the fold is: closed maximized, it reopens maximized.
	overlay.setMaximized(id, true);
	overlay.dismiss(id);
	id = overlay.show(makeStub({ 420.0f, 300.0f }, "Harm"));
	CHECK(overlay.maximized(id));
	CHECK(overlay.panelBounds(id).width() == doctest::Approx(1240.0f));
}
TEST_CASE("OV: the maximize button sits beside the fold on the desk's side, inboard of it, and clear of the enable "
          "chip and the title") {
	RestoreWindowSide restore;
	ui::ExpandedOverlay overlay;
	overlay.setBounds(0, 0, 1240, 700);
	overlay.show(makeStub({ 420.0f, 300.0f }, "Harm"));
	ui::ExpandedPanel* panel = overlay.panelForTest();
	REQUIRE(panel != nullptr);
	for (const auto side : { ui::WindowControlsSide::Right, ui::WindowControlsSide::Left }) {
		CAPTURE(static_cast<int>(side));
		(void)ui::setWindowControlsSide(side);
		panel->resized();
		const visage::Bounds close = panel->closeBounds();
		const visage::Bounds fold = panel->foldBounds();
		const visage::Bounds max = panel->maximizeBounds();
		CHECK(max.width() == ui::ExpandedOverlay::kCloseSize);
		CHECK(max.y() == close.y());
		CHECK(max.bottom() <= ui::ExpandedOverlay::kTitleHeight);
		if (side == ui::WindowControlsSide::Right) {
			CHECK(max.right() <= fold.x());
			CHECK(fold.right() <= close.x());
		}
		else {
			CHECK(max.x() >= fold.right());
			CHECK(fold.x() >= close.right());
			CHECK(panel->titleX() > max.right());
		}
	}
}
TEST_CASE("PHOTO OD: a window's head and the settings screen, controls left and right (writes only under "
          "FEATHERS_SIDE_SHOT_DIR)") {
	// A CAMERA, NOT A CLAIM: the x positions are asserted above. Off unless the directory is named.
	const char* dir = std::getenv("FEATHERS_SIDE_SHOT_DIR");
	if (dir == nullptr) {
		return;
	}
	RestoreWindowSide restore;
	const auto shoot = [](visage::Frame& frame, const std::string& path) {
		const int width = static_cast<int>(std::ceil(frame.width()));
		const int height = static_cast<int>(std::ceil(frame.height()));
		REQUIRE(width > 0);
		visage::Canvas canvas;
		markedWindowless(canvas, width, height);
		canvas.setColor(0xff000000);
		canvas.fill(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
		visage::Region region;
		region.setBounds(0, 0, width, height);
		canvas.addRegion(&region);
		canvas.beginRegion(&region);
		frame.draw(canvas);
		canvas.endRegion();
		takeMarkedCapture(canvas, width, height).save(path.c_str());
	};
	for (const auto side : { ui::WindowControlsSide::Left, ui::WindowControlsSide::Right }) {
		const std::string name = side == ui::WindowControlsSide::Left ? "left" : "right";
		(void)ui::setWindowControlsSide(side);
		ui::ExpandedOverlay overlay;
		overlay.setBounds(0, 0, 1240, 700);
		overlay.show(makeStub({ 420.0f, 120.0f }, "Reverb"));
		ui::ExpandedPanel* panel = overlay.panelForTest();
		REQUIRE(panel != nullptr);
		shoot(*panel, std::string(dir) + "/window-" + name + ".png");
		ui::SettingsScreen screen;
		screen.uiScaleNow = [] { return 1.0; };
		screen.setBounds(0.0f, 0.0f, 640.0f, 420.0f);
		shoot(screen, std::string(dir) + "/settings-" + name + ".png");
	}
}

namespace {
// ---------------------------------------------------------------------------------------------------------------
// PLOT HANDLES AS MODULATION TARGETS (`ModHandle`): the OWLTT crossover handles and the Drawn puck.
// ---------------------------------------------------------------------------------------------------------------

/// OWLTT's frequency axis, RESTATED (20 Hz..20 kHz, log2, across the plot's width) — not the graph's own `hzToX`.
float restatedHzToX(const visage::Bounds& plot, double hz) {
	const double t = std::log2(hz / 20.0) / std::log2(20000.0 / 20.0);
	return plot.x() + static_cast<float>(std::clamp(t, 0.0, 1.0)) * plot.width();
}
} // namespace

TEST_CASE("OWLTT's crossover handles are full modulation targets: halo, route on press, amount stroke, live dot") {
	namespace owl = feathers::fx::owltt;
	FilterFixture f;
	const size_t slot = f.bridge.addFxDevice(owl::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	f.lane->resized();
	auto* card = dynamic_cast<ui::OwlttCard*>(f.lane->cardAt(slot));
	REQUIRE(card != nullptr);
	ui::OwlttGraph* graph = card->graph();
	REQUIRE(graph != nullptr);
	graph->setCrossoversDraggable(true);
	REQUIRE(graph->rail().width() > 0.0f);
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);

	const backplane::Source macro = backplane::Source::core(0);
	const DeviceInstanceId id = f.bridge.fxDeviceAt(slot);
	const size_t lowIndex = static_cast<size_t>(owl::Param::LowCrossover);
	const backplane::Destination lowX = backplane::Destination::control(id, lowIndex);
	const auto baseHz = [&] { return static_cast<double>(f.bridge.fxChain().device[slot].row.field[lowIndex]); };
	const visage::Bounds plot = graph->plot();
	constexpr double kSpan = 2000.0 - 20.0; // Low Crossover's declared range: a full-depth route moves it this far

	// NOTHING ARMED: offered (it is a destination) but quiet — no halo, no stroke, no dot.
	ui::ModHandlePlan quiet = graph->crossoverModPlan(0);
	REQUIRE(quiet.axes.size() == 1);
	CHECK_FALSE(quiet.halo);
	CHECK_FALSE(quiet.axes[0].hasStroke);
	CHECK_FALSE(quiet.axes[0].hasDot);

	// ARMED AND UNROUTED: the halo, and nothing else — no stroke, no dot.
	mod->focus(macro);
	mod->arm(true);
	REQUIRE(f.bridge.mayRoute(macro, lowX));
	const ui::ModHandlePlan armed = graph->crossoverModPlan(0);
	CHECK(armed.halo);
	CHECK_FALSE(armed.axes[0].hasStroke);
	CHECK_FALSE(armed.axes[0].hasDot);
	// AND THE OTHER HANDLE, whose control the macro may also reach, lights too.
	CHECK(graph->crossoverModPlan(1).halo);

	SUBCASE("an armed press creates exactly one route, one undo entry, and starts no value drag") {
		REQUIRE(f.gui.history() != nullptr);
		const double before = baseHz();
		const visage::Point h = graph->crossoverHandle(0);
		graph->onMouseDown().callback(at(h.x, h.y));
		// THE DRAG THAT FOLLOWS MOVES THE AMOUNT ALONG THE HANDLE'S OWN AXIS, NOT THE FREQUENCY: a crossover slides sideways, so
		// the amount follows sideways travel, and VERTICAL travel changes neither the amount (it stays the seed) nor the base.
		graph->onMouseDrag().callback(at(h.x, h.y + 40.0f));
		graph->onMouseUp().callback(at(h.x, h.y + 40.0f));
		CHECK(f.bridge.routeAmount(macro, lowX) == doctest::Approx(ui::Knob::kRouteSeedAmount));
		CHECK(f.bridge.routeAmount(macro, backplane::Destination::control(id, static_cast<size_t>(owl::Param::HighCrossover))) == 0.0f);
		CHECK(baseHz() == doctest::Approx(before));
		REQUIRE(f.gui.history()->canUndo());
		f.gui.history()->undo();
		CHECK(f.bridge.routeAmount(macro, lowX) == 0.0f);
		CHECK_FALSE(f.gui.history()->canUndo()); // ONE entry, not two
	}

	SUBCASE("a SIDEWAYS drag while armed edits the amount along the knob's own gearing, and a vertical one of the same length does nothing") {
		const visage::Point h = graph->crossoverHandle(0);
		graph->onMouseDown().callback(at(h.x, h.y));
		graph->onMouseDrag().callback(at(h.x + 35.0f, h.y));
		graph->onMouseUp().callback(at(h.x + 35.0f, h.y));
		CHECK(f.bridge.routeAmount(macro, lowX) == doctest::Approx(ui::Knob::kRouteSeedAmount + 35.0f / ui::Knob::kRouteDragRange));
		// TOWARD HIGHER VALUES RAISES IT: leftward travel lowers it by the same.
		graph->onMouseDown().callback(at(h.x, h.y));
		graph->onMouseDrag().callback(at(h.x - 35.0f, h.y));
		graph->onMouseUp().callback(at(h.x - 35.0f, h.y));
		CHECK(f.bridge.routeAmount(macro, lowX) == doctest::Approx(ui::Knob::kRouteSeedAmount + 35.0f / ui::Knob::kRouteDragRange - 35.0f / ui::Knob::kRouteDragRange));
	}

	SUBCASE("the amount stroke, the live dot and the base handle sit where the plot's own axis puts them") {
		const double base = baseHz();
		REQUIRE(base == doctest::Approx(200.0));
		for (const bool isArmed : { true, false }) {
			CAPTURE(isArmed);
			mod->arm(isArmed);
			for (const float amount : { 0.05f, -0.05f }) {
				CAPTURE(amount);
				f.bridge.setRouteAmount(macro, lowX, amount);
				// THE STROKE: from the BASE to base + amount * span (a unipolar macro reaches one way), clamped to
				// the declared range, through the restated axis.
				const double end = std::clamp(base + static_cast<double>(amount) * kSpan, 20.0, 2000.0);
				const float a = restatedHzToX(plot, std::min(base, end));
				const float b = restatedHzToX(plot, std::max(base, end));
				const ui::ModHandlePlan plan = graph->crossoverModPlan(0);
				REQUIRE(plan.axes[0].hasStroke);
				CHECK(plan.axes[0].strokeLo == doctest::Approx(a).epsilon(1e-3));
				CHECK(plan.axes[0].strokeHi == doctest::Approx(b).epsilon(1e-3));
				CHECK(plan.axes[0].base == doctest::Approx(restatedHzToX(plot, base)).epsilon(1e-3));
				// THE HANDLE STAYS AT THE BASE.
				CHECK(graph->crossoverHandle(0).x == doctest::Approx(restatedHzToX(plot, base)).epsilon(1e-3));
				CHECK(plan.halo == isArmed);
				// NOT MEASURED FROM THE MODULATED VALUE: moving the live value does not move the stroke.
				f.bridge.setFxModulatedForTest(slot, lowIndex, 260.0f);
				const ui::ModHandlePlan live = graph->crossoverModPlan(0);
				CHECK(live.axes[0].strokeLo == doctest::Approx(a).epsilon(1e-3));
				CHECK(live.axes[0].strokeHi == doctest::Approx(b).epsilon(1e-3));
				// THE DOT: at the modulated value's coordinate, in BOTH states.
				REQUIRE(live.axes[0].hasDot);
				CHECK(live.axes[0].dot == doctest::Approx(restatedHzToX(plot, 260.0)).epsilon(1e-3));
				f.bridge.setFxModulatedForTest(slot, lowIndex, static_cast<float>(base));
				CHECK_FALSE(graph->crossoverModPlan(0).axes[0].hasDot);
			}
		}
	}

	SUBCASE("unarmed, dragging the handle edits the base and leaves the route and the dot alone") {
		f.bridge.setRouteAmount(macro, lowX, 0.05f);
		f.bridge.setFxModulatedForTest(slot, lowIndex, 260.0f);
		mod->arm(false);
		const visage::Point h = graph->crossoverHandle(0);
		const float toX = h.x + 60.0f;
		graph->onMouseDown().callback(at(h.x, h.y));
		graph->onMouseDrag().callback(at(toX, h.y));
		graph->onMouseUp().callback(at(toX, h.y));
		CHECK(baseHz() > 200.0);
		CHECK(f.bridge.routeAmount(macro, lowX) == doctest::Approx(0.05f));
		const ui::ModHandlePlan plan = graph->crossoverModPlan(0);
		CHECK(plan.axes[0].base == doctest::Approx(restatedHzToX(plot, baseHz())).epsilon(1e-3));
		CHECK(plan.axes[0].dot == doctest::Approx(restatedHzToX(plot, 260.0)).epsilon(1e-3));
	}

	SUBCASE("a knob over the same control shows the same amount and modulated value as the handle") {
		f.bridge.setRouteAmount(macro, lowX, 0.05f);
		f.bridge.setFxModulatedForTest(slot, lowIndex, 260.0f);
		device::Modulatable& control = *graph->crossoverModHandle(0).axis(0).control;
		ui::Knob knob(control, "Low", nullptr);
		const ui::Knob::ModRing ring = knob.modRingForTest();
		const ui::ModHandlePlan plan = graph->crossoverModPlan(0);
		// THE KNOB'S NORMALISED SWEEP IS LINEAR IN HERTZ HERE; READ THE HANDLE'S COORDINATES BACK TO HERTZ through the
		// restated axis's inverse and then to the same sweep.
		const auto hzAt = [&](float x) { return 20.0 * std::pow(1000.0, (x - plot.x()) / plot.width()); };
		const auto sweep = [](double hz) { return (hz - 20.0) / (2000.0 - 20.0); };
		CHECK(sweep(hzAt(plan.axes[0].strokeLo)) == doctest::Approx(ring.lo).epsilon(2e-3));
		CHECK(sweep(hzAt(plan.axes[0].strokeHi)) == doctest::Approx(ring.hi).epsilon(2e-3));
		REQUIRE(ring.hasMark);
		REQUIRE(plan.axes[0].hasDot);
		CHECK(sweep(hzAt(plan.axes[0].dot)) == doctest::Approx(ring.mark).epsilon(2e-3));
	}

	SUBCASE("both crossovers are declared to ControlFace, so the window-superset guard sees them") {
		std::vector<ui::ControlFace::Offered> offered;
		graph->offeredControls(offered);
		for (size_t which = 0; which < 2; ++which) {
			const device::Modulatable* c = graph->crossoverModHandle(which).axis(0).control;
			bool found = false;
			for (const auto& o : offered) {
				found = found || o.control == c;
			}
			CHECK(found);
		}
	}
}
TEST_CASE("the Drawn puck is a full modulation target on both axes: route on press, a stroke per axis, a dot per axis") {
	namespace dr = feathers::fx::drawn;
	FilterFixture f;
	const size_t slot = f.bridge.addFxDevice(dr::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	f.lane->resized();
	auto* card = dynamic_cast<ui::DrawnCard*>(f.lane->cardAt(slot));
	REQUIRE(card != nullptr);
	card->setBounds(0.0f, 0.0f, 240.0f, card->laneHeight());
	card->resized();
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);

	const backplane::Source macro = backplane::Source::core(0);
	const DeviceInstanceId id = f.bridge.fxDeviceAt(slot);
	const size_t xIndex = static_cast<size_t>(dr::Param::MorphX);
	const size_t yIndex = static_cast<size_t>(dr::Param::MorphY);
	const backplane::Destination dx = backplane::Destination::control(id, xIndex);
	const backplane::Destination dy = backplane::Destination::control(id, yIndex);
	const auto valueOf = [&](size_t i) { return static_cast<double>(f.bridge.fxChain().device[slot].row.field[i]); };
	const visage::Bounds plane = card->planeBounds();
	// THE PLANE'S MAPPING, RESTATED: x grows right across the plane, y is drawn up from its bottom edge.
	const auto xOf = [&](double v) { return plane.x() + plane.width() * static_cast<float>(std::clamp(v, 0.0, 1.0)); };
	const auto yOf = [&](double v) { return plane.bottom() - plane.height() * static_cast<float>(std::clamp(v, 0.0, 1.0)); };

	mod->focus(macro);
	mod->arm(true);
	REQUIRE(f.bridge.mayRoute(macro, dx));
	REQUIRE(f.bridge.mayRoute(macro, dy));
	const ui::ModHandlePlan armed = card->puckModPlan();
	REQUIRE(armed.axes.size() == 2);
	CHECK(armed.halo);

	SUBCASE("an armed press creates exactly one route on the nearer axis, one undo entry, and drags no morph") {
		const visage::Point p = card->puckCentre();
		const double bx = valueOf(xIndex);
		const double by = valueOf(yIndex);
		// A PRESS A FEW PIXELS ABOVE THE PUCK'S CENTRE LINE: nearer the horizontal (Morph X) stroke.
		card->onMouseDown().callback(at(p.x, p.y - 1.0f));
		card->onMouseDrag().callback(at(p.x + 20.0f, p.y - 20.0f));
		card->onMouseUp().callback(at(p.x + 20.0f, p.y - 20.0f));
		const float ax = f.bridge.routeAmount(macro, dx);
		const float ay = f.bridge.routeAmount(macro, dy);
		CHECK(((ax != 0.0f) != (ay != 0.0f))); // exactly one of the two axes got a route
		CHECK(valueOf(xIndex) == doctest::Approx(bx));
		CHECK(valueOf(yIndex) == doctest::Approx(by));
		REQUIRE(f.gui.history()->canUndo());
		f.gui.history()->undo();
		CHECK(f.bridge.routeAmount(macro, dx) == 0.0f);
		CHECK(f.bridge.routeAmount(macro, dy) == 0.0f);
		CHECK_FALSE(f.gui.history()->canUndo());
	}

	SUBCASE("each axis draws its own stroke through the plane's mapping, and its own dot") {
		const double bx = valueOf(xIndex);
		const double by = valueOf(yIndex);
		f.bridge.setRouteAmount(macro, dx, 0.2f);
		f.bridge.setRouteAmount(macro, dy, -0.3f);
		f.bridge.setFxModulatedForTest(slot, xIndex, static_cast<float>(bx + 0.1));
		f.bridge.setFxModulatedForTest(slot, yIndex, static_cast<float>(by - 0.2));
		for (const bool isArmed : { true, false }) {
			CAPTURE(isArmed);
			mod->arm(isArmed);
			const ui::ModHandlePlan plan = card->puckModPlan();
			// X: base to base + 0.2 (span 1), unipolar, along x.
			REQUIRE(plan.axes[0].hasStroke);
			CHECK(plan.axes[0].strokeLo == doctest::Approx(xOf(bx)).epsilon(1e-3));
			CHECK(plan.axes[0].strokeHi == doctest::Approx(xOf(bx + 0.2)).epsilon(1e-3));
			// Y: base - 0.3 .. base, drawn up, so the LOW coordinate is the higher value.
			REQUIRE(plan.axes[1].hasStroke);
			CHECK(plan.axes[1].strokeLo == doctest::Approx(yOf(by)).epsilon(1e-3));
			CHECK(plan.axes[1].strokeHi == doctest::Approx(yOf(std::max(0.0, by - 0.3))).epsilon(1e-3));
			REQUIRE(plan.axes[0].hasDot);
			REQUIRE(plan.axes[1].hasDot);
			CHECK(plan.axes[0].dot == doctest::Approx(xOf(bx + 0.1)).epsilon(1e-3));
			CHECK(plan.axes[1].dot == doctest::Approx(yOf(by - 0.2)).epsilon(1e-3));
			// THE PUCK STAYS AT THE BASE.
			CHECK(card->puckCentre().x == doctest::Approx(xOf(bx)).epsilon(1e-3));
			CHECK(card->puckCentre().y == doctest::Approx(yOf(by)).epsilon(1e-3));
		}
	}

	SUBCASE("unarmed, dragging the puck edits the bases only") {
		f.bridge.setRouteAmount(macro, dx, 0.2f);
		mod->arm(false);
		const visage::Point p = card->puckCentre();
		card->onMouseDown().callback(at(p.x, p.y));
		card->onMouseDrag().callback(at(p.x + 25.0f, p.y));
		card->onMouseUp().callback(at(p.x + 25.0f, p.y));
		CHECK(valueOf(xIndex) > 0.5);
		CHECK(f.bridge.routeAmount(macro, dx) == doctest::Approx(0.2f));
	}

	SUBCASE("both morph axes are declared to ControlFace") {
		std::vector<ui::ControlFace::Offered> offered;
		card->offeredControls(offered);
		for (size_t axis = 0; axis < 2; ++axis) {
			const device::Modulatable* c = card->puckModHandle().axis(axis).control;
			bool found = false;
			for (const auto& o : offered) {
				found = found || o.control == c;
			}
			CHECK(found);
		}
	}
}
TEST_CASE("HARM's card tabs and the window's strip are one selection, both ways") {
	HarmFixture f;
	const std::shared_ptr<ui::HarmView> view = ui::harmViewFor(f.device);
	REQUIRE(view != nullptr);
	CHECK(f.card->selectedPartial() == 0);
	CHECK(f.expanded().selectedPartial() == 0);
	CHECK(view->partial == 0);

	const auto centreOf = [](visage::Bounds b) {
		return visage::Point { b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f };
	};

	SUBCASE("a tab pressed on the card selects that partial in the window") {
		const visage::Point p = centreOf(f.card->tabBounds(2));
		f.card->onMouseDown().callback(at(p.x, p.y));
		CHECK(f.card->selectedPartial() == 2);
		CHECK(view->partial == 2);
		f.expanded().updateContent();
		CHECK(f.expanded().selectedPartial() == 2);
		CHECK(f.expanded().stripForTest()->selected() == 2);
	}

	SUBCASE("a column pressed on the window's strip moves the card's tabs") {
		auto* strip = const_cast<ui::HarmPartialStrip*>(f.expanded().stripForTest());
		REQUIRE(strip != nullptr);
		const visage::Point p = centreOf(strip->columnBounds(3));
		strip->onMouseDown().callback(at(p.x, p.y));
		CHECK(f.expanded().selectedPartial() == 3);
		CHECK(view->partial == 3);
		CHECK(f.card->updateContent());
		CHECK(f.card->selectedPartial() == 3);
	}

	SUBCASE("each tab says its partial's ratio and whether the partial is on") {
		CHECK(f.card->tabTextForTest(0) == "1  1:1 on");
		CHECK(f.card->tabTextForTest(1) == "2  2:1 off");
		CHECK(f.card->tabTextForTest(2) == "3  3:1 off");
		CHECK(f.card->tabTextForTest(3) == "4  4:1 off");
	}

	SUBCASE("the global controls sit below the tabs, outside their scope") {
		const float tabsBottom = f.card->tabBounds(0).bottom();
		REQUIRE(!f.card->knobsForTest().empty());
		for (const auto& knob : f.card->knobsForTest()) {
			CAPTURE(std::string(knob->label()));
			CHECK(knob->y() > tabsBottom);
		}
	}
}

