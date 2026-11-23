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

// PIECE 4 OF 12 of the dahdsr suite: its cases in their original order, with the helpers only they use. See dahdsr_common.h.

#include "dahdsr/dahdsr_common.h"

TEST_CASE("the fx lane draws the CHAIN, not its one tenant") {
	// WHAT THE LANE HELD BEFORE THIS: a bare `FilterCard*`, handed in from Gui::create, placed at one
	// rectangle. That was true for as long as the chain could hold one device — and the chain has
	// been able to hold eight since the fx channel landed, so a lane that could draw one of them was
	// the visible half of a half-unpinned thing. rack_card.h opens with the same sentence about the
	// modulator strip, which had already been through it.
	FilterFixture f;
	visage::Canvas canvas;
	auto* lane = f.lane;
	REQUIRE(lane != nullptr);

	REQUIRE(lane->deviceCount() == 1);
	REQUIRE(lane->filterCard() != nullptr);
	REQUIRE(lane->cardAt(0) == lane->filterCard());

	SUBCASE("a second device in the chain gets a card of its own") {
		const size_t slot = f.bridge.addFxDevice(fx::harm::kTraits.tag);
		REQUIRE(slot == 1);
		lane->resized();

		CHECK(lane->deviceCount() == 2);
		// A CARD, NOT A GAP, AND NOW HARM'S OWN. This asked for a NoEditorCard until HARM had one —
		// the placeholder existed precisely because the device was audible and addable and had no
		// editor, and the lane chooses by what it can BUILD rather than by a list of devices missing
		// cards, so the placeholder went away by itself the moment HarmCard existed.
		REQUIRE(lane->cardAt(1) != nullptr);
		CHECK(dynamic_cast<ui::HarmCard*>(lane->cardAt(1)) != nullptr);
		CHECK(dynamic_cast<ui::MissingDeviceCard*>(lane->cardAt(1)) == nullptr);
		// AND IT IS NOT A FILTER CARD, which is the half that says the lane read the tag rather than
		// building whatever it knows how to build.
		CHECK(dynamic_cast<ui::FilterCard*>(lane->cardAt(1)) == nullptr);
		// The first slot is untouched: an append cannot move what is already there.
		CHECK(lane->cardAt(0) == lane->filterCard());
	}

	SUBCASE("a slot holding a device this build cannot name still draws as something") {
		// THE PLACEHOLDER'S REMAINING CONSUMER, named rather than assumed. HARM's card took the only
		// road that reached NoEditorCard through addFxDevice — the lane admits exactly the two devices
		// that declare audio ops, and both now draw themselves. What is left is a PATCH: a chain
		// restored from a file written by a build that had a device this one does not, whose tag
		// resolves to no descriptor at all.
		//
		// It is worth keeping a case on because the alternative is a blank slot, and a blank slot in a
		// lane is indistinguishable from a device that failed to load.
		feathers::FxChain chain = f.bridge.fxChain();
		REQUIRE(chain.count == 1);
		chain.device[1].row = feathers::DeviceRow {};
		chain.device[1].row.tag = { 'Z', 'Z', 'Z', 'Z' };
		chain.device[1].row.instanceId = chain.nextInstanceId;
		chain.nextInstanceId = chain.nextInstanceId + 1;
		chain.count = 2;
		// **WRITTEN AT POSITION 1 AND THAT IS THE WHOLE OF IT (kill-lanes step 3c).** This used to write
		// the row, then call `assignDenseFxLanes` to state the order for the pair — a second numbering
		// that had to be brought into line with the first. The array is dense in `[0, count)` by position
		// now, so `device[1]` with `count == 2` IS the second device and there is nothing left to align.
		f.bridge.restoreFxChain(chain);
		lane->resized();

		REQUIRE(lane->cardAt(1) != nullptr);
		const auto* missing = dynamic_cast<ui::MissingDeviceCard*>(lane->cardAt(1));
		REQUIRE(missing != nullptr);
		CHECK(dynamic_cast<ui::HarmCard*>(lane->cardAt(1)) == nullptr);
		// **"DEVICE MISSING", NAMED BY ITS TAG** — the only name a device that is not
		// here has.
		CHECK(missing->name() == "ZZZZ");
		CHECK(std::string(ui::MissingDeviceCard::kMessage) == "device missing");
	}

	SUBCASE("the cards stack, and a fold above moves the one below") {
		const size_t slot = f.bridge.addFxDevice(fx::harm::kTraits.tag);
		REQUIRE(slot == 1);
		lane->resized();

		const visage::Bounds first = lane->cardBounds(0);
		const visage::Bounds second = lane->cardBounds(1);
		// BELOW, BY THE LANE'S OWN GAP — restated from the constants rather than compared with a
		// number, so a change to the padding moves the claim with it.
		CHECK(second.y() == doctest::Approx(first.bottom() + ui::FxLane::kSlotGap));

		// AND FOLDING THE FIRST BRINGS THE SECOND UP. The lane stacks what its cards report, so a
		// card whose height it cached would leave a hole here — which is exactly what a stored height
		// does and why laneHeight() is asked every time.
		f.card->setFolded(true);
		lane->resized();
		const visage::Bounds afterFold = lane->cardBounds(1);
		CAPTURE(second.y());
		CAPTURE(afterFold.y());
		CHECK(afterFold.y() < second.y());
		CHECK(afterFold.y()
		      == doctest::Approx(lane->cardBounds(0).bottom() + ui::FxLane::kSlotGap));
	}

	SUBCASE("a card the lane builds is subscribed to the state road as it is built") {
		// THE FAILURE THIS PREVENTS IS SILENT, which is why the subscription moved into the lane. A
		// swallowed device's settings are not parameters, so nothing else will repaint its card; a
		// registration made once, outside, would be made for the card that happened to exist at
		// startup and never for one built later.
		const ui::FrameWatchers* watchers = f.gui.stateWatchers();
		REQUIRE(watchers != nullptr);
		CHECK(watchers->watches(lane->cardAt(0)));

		const size_t slot = f.bridge.addFxDevice(fx::harm::kTraits.tag);
		REQUIRE(slot == 1);
		lane->resized();
		REQUIRE(lane->cardAt(1) != nullptr);
		// THE CARD THAT DID NOT EXIST WHEN THE EDITOR OPENED. This is the assertion the old
		// arrangement could not have made at all.
		CHECK(watchers->watches(lane->cardAt(1)));
	}

	SUBCASE("and an empty chain draws its placeholder rather than a card") {
		feathers::FxChain empty;
		empty.count = 0;
		f.bridge.restoreFxChain(empty);
		lane->resized();

		CHECK(lane->deviceCount() == 0);
		CHECK(lane->cardAt(0) == nullptr);
		CHECK(lane->filterCard() == nullptr);
		// It still draws — the empty-slot box, which is what the lane said before any of this and
		// what it must go on saying.
		lane->draw(canvas);
		CHECK(lane->emptySlotBounds().height() > 0.0f);
	}
}
TEST_CASE("a device added by the (+) is on screen without waiting to be perturbed") {
	// The defect: a new device did not appear until something else (a scroll, say) disturbed the
	// UI. The add reached the rack and the card existed; nothing placed it or made it
	// visible, so the strip went on drawing the layout it had before — and a scroll, which calls
	// resized(), was what made the new device appear.
	//
	// THE OBSERVABLE HALF IS NOT A REPAINT AND SO IS ASSERTABLE HERE. Whether a picture changed
	// cannot be seen headlessly; whether a frame has been given a size and made visible can, and that
	// is what gates the picture. The rest of the panel gets both from resized(), which the add was not
	// calling.
	ModFixture f;
	const size_t before = f.panel->deviceCount();
	const std::vector<const feathers::device::Descriptor*> offered = f.panel->addableForTest();
	REQUIRE(offered.size() >= 2);

	const size_t slot = f.panel->addChoiceForTest(1);
	REQUIRE(slot == before);

	// NOTHING ELSE HAPPENS BETWEEN THE ADD AND THESE CHECKS — no resized(), no draw(), no scroll.
	// That is the whole point: the gesture has to leave the strip in a state a hand can see.
	const visage::Frame* card = f.panel->cardFrameForTest(slot);
	REQUIRE(card != nullptr);
	CHECK(card->isVisible());
	CHECK(card->width() > 0.0f);
	CHECK(card->height() > 0.0f);

	// AND IT IS WHERE THE STRIP SAYS IT IS, rather than at the window origin — the state a child frame
	// sits in when it has been made visible and never placed, which draws in the top-left corner of
	// everything.
	const visage::Bounds expected = f.panel->envCardBounds(slot);
	// **AT THE CARD'S OWN ORIGIN, because the card is the whole card now.** It used to be placed below
	// a head and an outputs row the STRIP drew, which is exactly why it had no frame in any other
	// host. `ui::drawCardFrame` is the one drawer and the card calls it, so the rectangle the strip
	// reserves and the rectangle the card fills are the same one — the outputs strip included, taken
	// out of the body by `ui::cardFrameOf` rather than laid over it.
	CHECK(card->y() == doctest::Approx(expected.y()));
	CHECK(card->x() == doctest::Approx(expected.x()));

	SUBCASE("and the card the strip drew for it is the device that was chosen") {
		// The defect: adding an MSEG produced a card labelled DAHDSR — the add was right and the
		// head row was wrong, which reads as the wrong device having been added. The head is the
		// STRIP's to draw, so the strip has to ask what the slot holds rather than name one device.
		const char* chosen = offered[1]->traits.tag;
		REQUIRE(f.bridge.modRack().holds(slot, chosen));
		CHECK(std::string(f.panel->deviceName(slot)) == std::string(offered[1]->traits.name));

		// AND THE FIRST SLOT STILL SAYS ITS OWN NAME, or "ask the slot" could be satisfied by naming
		// every card after the last device added.
		CHECK(std::string(f.panel->deviceName(0))
		      == std::string(mod::dahdsr::kDescriptor.traits.name));
		CHECK(std::string(f.panel->deviceName(0))
		      != std::string(f.panel->deviceName(slot)));
	}
}

namespace {
/// A OWLTT seated in the lane, with the meter road's five questions in reach.
struct OwlttMeterFixture : FilterFixture {
	static constexpr size_t kSlot = 1;
	ui::OwlttCard* card = nullptr;
	/// THE DEVICE, not the place it stands (kill-lanes step 5). A control write is addressed by id — the
	/// position is what the LANE draws at, and the two stop agreeing the moment the chain is edited.
	DeviceInstanceId id = kNoInstance;

	OwlttMeterFixture() {
		REQUIRE(bridge.addFxDevice(fx::owltt::kTraits.tag) == kSlot);
		id = bridge.fxDeviceAt(kSlot);
		REQUIRE(id != kNoInstance);
		lane->resized();
		card = dynamic_cast<ui::OwlttCard*>(lane->cardAt(kSlot));
		REQUIRE(card != nullptr);
		REQUIRE(card->graph() != nullptr);
		// Whatever the construction published, drawn and forgotten, so a case starts from "nothing has
		// moved" rather than from whatever the fixture happened to leave behind.
		gui.tickForTest();
	}

	/// Publish a reading the way the audio thread does. Levels only, so `clipping` is left alone and
	/// the clip case can move it by itself.
	void publish(float level, bool clipping = false) {
		bridge.publishOwlttMeters(kSlot, { -level, level, -level }, { level, level, level },
		                         { level, level, level }, clipping);
	}
};
} // namespace

TEST_CASE("a OWLTT meter reaches the screen without the state channel") {
	// THE BUG THIS IS MOST AFRAID OF IS THE ONE THE OLD LINE WAS WRITTEN TO FIX. `publishOwlttMeters`
	// used to set `repaintDirty_`, because through three wiring attempts the meters were correct and
	// nothing repainted them. That line is gone — it made the editor redraw its whole tree at block
	// rate — and this is the case that says the replacement road actually delivers.
	//
	// IT ASSERTS THE VALUE, NOT A PICTURE: the count comes BACK FROM the update that made the
	// request, so it cannot be had without the request being made. A counter beside the call could
	// not promise that, and a repaint is not observable headlessly at all.
	OwlttMeterFixture f;

	// Nothing has moved: the road is silent, and that half matters as much as the other. A road that
	// always fires is a block-rate repaint wearing a new name.
	CHECK(f.gui.tickForTest().deviceFrames == 0);

	f.publish(0.5f);
	CHECK(f.gui.tickForTest().deviceFrames == 1);

	// And it goes quiet again once that reading has been drawn.
	CHECK(f.gui.tickForTest().deviceFrames == 0);

	SUBCASE("and the state channel is not what carried it") {
		// THE WHOLE POINT OF THE CHANGE. If the publish still raised the state flag, the number below
		// would be the tree — 100-odd frames — instead of nothing.
		f.publish(0.75f);
		const Gui::TickReport report = f.gui.tickForTest();
		CHECK(report.deviceFrames == 1);
		CHECK(report.stateFrames == 0);
	}
}
TEST_CASE("an OPEN OWLTT panel's meters reach the screen too, on its own graph") {
	// THE OTHER HALF OF THE SAME FAILURE, AND THE ONE THIS DEVICE HAS BEEN THROUGH THREE TIMES. An
	// expanded OWLTT builds its OWN `OwlttGraph` over the same slot, so a road that ticked only the
	// lane's card would leave the panel's meters dead while the card's moved — correct values, a
	// picture that never updates, and nothing red anywhere.
	//
	// The panel goes in the EDITOR'S OVERLAY rather than a popout window, because a real window
	// inside this process takes the process down with it; `gui-windowed` is where that road is asked
	// about. The `updateContent()` hook is on both hosts for exactly that reason and only one of them
	// can be exercised here — which is a stated limit of this case, not a claim about the other host.
	OwlttMeterFixture f;
	f.gui.setPopoutWindowForTest(false);
	ui::ExpandedOverlay* overlay = f.gui.expandedForTest();
	REQUIRE(overlay != nullptr);
	// ON THE OVERLAY, which is the host this case reads: the canvas is folded first.
	(void)f.gui.canvasForTest()->setCollapsed(true);
	f.lane->onExpandDevice(OwlttMeterFixture::kSlot);
	REQUIRE(overlay->open());
	f.gui.tickForTest();

	// A publish now has TWO surfaces to reach: the card's graph and the panel's.
	f.publish(0.6f);
	CHECK(f.gui.tickForTest().deviceFrames == 2);
	CHECK(f.gui.tickForTest().deviceFrames == 0);

	SUBCASE("and it is the PANEL's own graph that was told, not only the card's") {
		// Asked of the panel's graph directly, so a count of two could not be satisfied by the card
		// answering twice.
		f.publish(0.85f);
		auto* view = const_cast<ui::ExpandedView*>(overlay->contentForTest());
		REQUIRE(view != nullptr);
		CHECK(view->update() == true);
		// And once it has drawn that reading it goes quiet, like every other surface on this road.
		CHECK(view->update() == false);
	}
}
TEST_CASE("a OWLTT meter that only clips still reaches the screen") {
	// THE PEAK-HOLD TRAP, AND IT IS THE ONE A CONTENT GATE INVITES. `clipping_` is a HELD state on
	// the audio side with its own decay counter (owltt.cpp), so it can turn on while all nine level
	// numbers stay exactly where they were. A comparison over the three level arrays alone would sit
	// on a dark clip light for as long as a steady signal held, which is a WRONG READING rather than
	// a late one — the worse failure, because nothing reports it.
	OwlttMeterFixture f;
	f.publish(0.5f, false);
	CHECK(f.gui.tickForTest().deviceFrames == 1);
	CHECK(f.gui.tickForTest().deviceFrames == 0);

	// The identical levels, and only the clip bit moves.
	f.publish(0.5f, true);
	CHECK(f.gui.tickForTest().deviceFrames == 1);

	// And back off again, which is the decay road.
	CHECK(f.gui.tickForTest().deviceFrames == 0);
	f.publish(0.5f, false);
	CHECK(f.gui.tickForTest().deviceFrames == 1);
}
TEST_CASE("a OWLTT nobody can see asks for nothing, and asks again the moment it can be seen") {
	// A HIDDEN CARD MUST COST NOTHING AND MUST NOT GO PERMANENTLY DEAD. Those are two failures with
	// one road between them, and the second is the worse one: a device whose meters never come back
	// after a scroll is a dead panel that looks like a working one.
	OwlttMeterFixture f;
	f.publish(0.25f);
	REQUIRE(f.gui.tickForTest().deviceFrames == 1);

	SUBCASE("hidden, it is not told") {
		f.card->setVisible(false);
		f.publish(0.9f);
		CHECK(f.gui.tickForTest().deviceFrames == 0);

		// AND THE READING IT MISSED IS NOT LOST. Shown again, the very next tick notices that what is
		// published differs from what was last DRAWN — which is why the comparison is against drawn
		// state and not against "the previous publish".
		f.card->setVisible(true);
		CHECK(f.gui.tickForTest().deviceFrames == 1);
	}

	SUBCASE("and the lane collapsing is one of the ways it becomes hidden") {
		f.lane->setCollapsed(true);
		f.lane->resized();
		REQUIRE(f.card->isVisible() == false);
		f.publish(0.9f);
		CHECK(f.gui.tickForTest().deviceFrames == 0);

		f.lane->setCollapsed(false);
		f.lane->resized();
		REQUIRE(f.card->isVisible());
		CHECK(f.gui.tickForTest().deviceFrames == 1);
	}
}
TEST_CASE("one OWLTT's meters do not repaint another's") {
	// THE ADDRESSING THE STATE CHANNEL DID NOT HAVE, put to the road that replaced it. Under the old
	// line either device's meters redrew the whole editor, so "only the right one" was not a question
	// that could be asked.
	FilterFixture f;
	REQUIRE(f.bridge.addFxDevice(fx::owltt::kTraits.tag) == 1);
	REQUIRE(f.bridge.addFxDevice(fx::owltt::kTraits.tag) == 2);
	f.lane->resized();
	auto* first = dynamic_cast<ui::OwlttCard*>(f.lane->cardAt(1));
	auto* second = dynamic_cast<ui::OwlttCard*>(f.lane->cardAt(2));
	REQUIRE(first != nullptr);
	REQUIRE(second != nullptr);
	// ASKED OF EACH GRAPH DIRECTLY RATHER THAN THROUGH THE LANE, and that is the point rather than a
	// convenience: two OWLTT cards overflow the lane, so the second is scrolled out and invisible —
	// which the lane would answer for, hiding the question. Going straight to each graph takes
	// visibility out of it and leaves only the addressing under test.
	//
	// (That the second card IS hidden here is the scroll-visibility rule working; a fixture that
	// required both visible would be asking this lane to be a size it is not.)
	f.gui.tickForTest();
	REQUIRE(first->graph()->update() == false);
	REQUIRE(second->graph()->update() == false);

	f.bridge.publishOwlttMeters(1, { -0.5f, 0.5f, -0.5f }, { 0.5f, 0.5f, 0.5f },
	                           { 0.5f, 0.5f, 0.5f }, false);

	// The one published to has something to draw; the one beside it has not.
	CHECK(first->graph()->update() == true);
	CHECK(second->graph()->update() == false);
}
TEST_CASE("taking the meters off the state channel does not take anything else off it") {
	// THE OTHER THIRTY-FOUR WRITERS. `repaintDirty_` is set in thirty-five places and only
	// `publishOwlttMeters` was ever block-rate; the rest are gestures and patch loads, for which a
	// whole-tree notify is correct. A change that narrowed the wrong thing would show up here as a
	// gesture that stopped repainting.
	OwlttMeterFixture f;
	f.gui.tickForTest();

	// A gesture on a device control — one of the thirty-four — still tells the tree.
	f.bridge.setDeviceControl(f.id, static_cast<size_t>(fx::owltt::Param::Enabled), 1.0f);
	CHECK(f.gui.tickForTest().stateFrames > 0);

	// And a meter publish still does not.
	f.publish(0.4f);
	CHECK(f.gui.tickForTest().stateFrames == 0);
}
TEST_CASE("a card scrolled out of the lane is not visible, and comes back when it is scrolled in") {
	// THE SCROLL WAS MISSING FROM THE VISIBILITY RULE, and `FrameWatchers::notify()` redraws every
	// VISIBLE frame under every watcher — so each state notify repainted every card in the chain,
	// including the ones off the top of the lane. MEASURED with seven seated devices, five of them at
	// y = -1574 .. -332 and every one reporting visible: the same block-rate workload ran 56% longer
	// than with a single device, and hiding those cards made it 4.4x faster while telling the same
	// 138 frames.
	//
	// THE HALF THAT MATTERS MORE IS THE WAY BACK. Hiding is easy to get right and easy to get right
	// ONCE — a card that goes invisible and never returns is a blank lane after a scroll, which is a
	// worse bug than the cost it was fixing. So the assertions below are mostly about scrolling IN.
	HarmFixture f;
	REQUIRE(f.lane != nullptr);

	// Enough devices to overflow the lane, which is the only condition under which this rule has
	// anything to say. Asked of maxScroll rather than assumed from a card count.
	for (int more = 0; more < 6; ++more) {
		const size_t took = f.bridge.addFxDevice(fx::harm::kTraits.tag);
		REQUIRE(took < feathers::kMaxFxSlots);
		f.lane->resized();
	}
	REQUIRE(f.lane->maxScroll() > 0.0f);

	// AT THE TOP: slot 0 is on screen. Its own answer, from the frame.
	f.lane->setScroll(0.0f);
	ui::FxCard* first = f.lane->cardAt(0);
	REQUIRE(first != nullptr);
	CHECK(first->isVisible());

	// AT THE BOTTOM: slot 0 has left the lane, and says so.
	f.lane->setScroll(f.lane->maxScroll());
	REQUIRE(f.lane->cardBounds(0).bottom() <= 0.0f); // really gone, not merely low
	CHECK(first->isVisible() == false);

	SUBCASE("and scrolling it back in makes it drawable again") {
		f.lane->setScroll(0.0f);

		// BOTH HALVES OF THE GATE `Frame::redraw()` USES — `isVisible() && isDrawing()`. Visible alone
		// is not enough: `setDrawing` is what walks the subtree, and a card whose children were left
		// not-drawing would come back as an empty box with its knobs missing.
		CHECK(first->isVisible());
		CHECK(first->isDrawing());
		for (visage::Frame* child : first->children()) {
			if (child->isVisible()) {
				CHECK(child->isDrawing());
			}
		}

		// AND IT IS STILL LAID OUT. A card must not be rebuilt by coming back into view — it is sized
		// whether shown or not, which is the discipline the line above this fix already followed.
		CHECK(first->width() > 0.0f);
		CHECK(first->height() > 0.0f);
	}

	SUBCASE("a card straddling the top edge is still shown") {
		// THE OFF-BY-ONE THIS RULE INVITES. Half a card is half a card a player can see, and a
		// predicate written with the wrong comparison hides it.
		const float firstHeight = f.lane->cardBounds(0).height();
		REQUIRE(firstHeight > 4.0f);
		f.lane->setScroll(firstHeight - 2.0f);
		REQUIRE(f.lane->cardBounds(0).bottom() > 0.0f);
		REQUIRE(f.lane->cardBounds(0).y() < 0.0f);
		CHECK(first->isVisible());
	}

	SUBCASE("the card the lane scrolls to is the one that is visible") {
		// THROUGH THE LANE'S OWN VERB rather than through a scroll value, because that is the road a
		// selection takes and the one a hand never sees.
		const size_t last = f.bridge.fxChain().count - 1;
		f.lane->scrollCardIntoView(last);
		ui::FxCard* tail = f.lane->cardAt(last);
		REQUIRE(tail != nullptr);
		CHECK(tail->isVisible());
		CHECK(tail->isDrawing());
	}
}
TEST_CASE("an fx control says it is modulated even when its source is not the focused one") {
	// THE FOURTH LANE, AND THE ONLY ONE NOBODY ASKED THIS OF. `Modulatable::display()` has four
	// implementations — ParamControl, RackControl, MsegControl, FxControl — and "a control says it is
	// modulated even when its source is not the focused one" pins the disclosure on the RACK's. This
	// is the same question put to the fx lane's.
	//
	// IT WAS UNCOVERED, AND THAT IS MEASURED RATHER THAN ASSUMED. `FxControl::display()`'s
	// `routedElsewhere` loop was mutated to set the flag unconditionally — the opposite of the
	// field's default, so no optimiser may drop it, and the store was confirmed present in the object
	// (`movb $0x1`) — and all 499 cases in this binary passed one-process-per-case. A whole lane's
	// disclosure could be deleted or inverted in silence.
	//
	// WHY IT MATTERS BEYOND THE FLAG: this is the lane whose knobs the fx cards and every device
	// panel are built from, so it is the disclosure a hand actually meets on a OWLTT or HARM control.
	HarmFixture f;
	feathers::ui::ModState mod;
	namespace hrm = fx::harm;

	// A control that IS a destination, found through the device's own declaration rather than by
	// picking an index and hoping.
	size_t control = hrm::kDescriptor.params.size();
	for (size_t c = 0; c < hrm::kDescriptor.params.size(); ++c) {
		if (hrm::kDescriptor.params[c].modulatable) {
			control = c;
			break;
		}
	}
	REQUIRE(control < hrm::kDescriptor.params.size());
	const backplane::Destination dest = backplane::Destination::control(f.device, control);

	ui::FxControl knob { f.bridge, f.device, hrm::kDescriptor, control, &mod };
	REQUIRE(knob.display().isDestination);

	// Nothing routed anywhere: nothing to disclose.
	CHECK(knob.display().routedElsewhere == false);

	// Arm a source and put a route on this control through the control's own road, which is what a
	// hand does — not a direct write to the table, which would prove the table and not the control.
	const backplane::Source armed = backplane::Source::core(0); // a macro
	mod.focus(armed);
	mod.arm(true);
	knob.setAmount(1.0f);
	CHECK(knob.display().amount == doctest::Approx(1.0f));
	// While the route's own source is focused the AMOUNT carries it, and the disclosure stays quiet:
	// this flag is about the routes the amount cannot speak for.
	CHECK(knob.display().routedElsewhere == false);

	SUBCASE("and the moment the focus moves, the control still says something is on it") {
		mod.focus(backplane::Source::core(1)); // a different macro — one press on another card is all it takes
		mod.arm(false);

		// The bar is correctly gone: nothing is routed here from the NEWLY focused source.
		CHECK(knob.display().amount == doctest::Approx(0.0f));

		// THE ASSERTION THIS CASE EXISTS FOR.
		CHECK(knob.display().routedElsewhere == true);

		// And the route is really still there, or the flag above would be disclosing nothing.
		CHECK(f.bridge.routeAmount(armed, dest) == doctest::Approx(1.0f));
	}

	SUBCASE("a control with nothing on it never claims otherwise") {
		// The other half, so the flag is not simply always true — which is exactly what the mutation
		// that survived 499 cases did. A DIFFERENT destination on the same device, focus moved the
		// same way, has nothing to disclose.
		size_t otherControl = hrm::kDescriptor.params.size();
		for (size_t c = control + 1; c < hrm::kDescriptor.params.size(); ++c) {
			if (hrm::kDescriptor.params[c].modulatable) {
				otherControl = c;
				break;
			}
		}
		REQUIRE(otherControl < hrm::kDescriptor.params.size());

		ui::FxControl other { f.bridge, f.device, hrm::kDescriptor, otherControl, &mod };
		mod.focus(backplane::Source::core(1));
		mod.arm(false);
		CHECK(other.display().routedElsewhere == false);
		CHECK(knob.display().routedElsewhere == true);
	}
}
TEST_CASE("a seam control over Feathers' host says it is modulated even when its source is not the focused one") {
	// **THE HOST'S OWN ROAD, WHICH NOTHING ASKED.** `DeviceControl` — every generic card's control, and
	// the seam every card is converging onto — asks `DeviceHost::routedElsewhere`, and Feathers' host
	// answers by forwarding to the bridge. MEASURED UNCOVERED (kill-lanes step 2): that override was
	// mutated to answer false and all six widget binaries stayed green one process per case, because the
	// fx seating above asks the bridge directly and never passes through the host.
	HarmFixture f;
	feathers::ui::ModState mod;
	namespace hrm = fx::harm;
	size_t control = hrm::kDescriptor.params.size();
	for (size_t c = 0; c < hrm::kDescriptor.params.size(); ++c) {
		if (hrm::kDescriptor.params[c].modulatable) {
			control = c;
			break;
		}
	}
	REQUIRE(control < hrm::kDescriptor.params.size());
	const backplane::Destination dest = backplane::Destination::control(f.device, control);

	ui::DeviceControl knob { f.deviceHost, f.device, hrm::kDescriptor, control, &mod };
	REQUIRE(knob.display().isDestination);
	CHECK(knob.display().routedElsewhere == false);

	// A ROUTE THROUGH THE CONTROL'S OWN ROAD, from the armed macro.
	const backplane::Source armed = backplane::Source::core(0);
	mod.focus(armed);
	mod.arm(true);
	knob.setAmount(1.0f);
	REQUIRE(f.deviceHost.routeAmount(armed, dest) == doctest::Approx(1.0f));
	CHECK(knob.display().routedElsewhere == false); // the amount speaks for the focused source's route

	// FOCUS ELSEWHERE: the route is still on the control, and the control must say so.
	mod.focus(backplane::Source::core(1));
	mod.arm(false);
	CHECK(knob.display().amount == doctest::Approx(0.0f));
	CHECK(knob.display().routedElsewhere == true);
}
TEST_CASE("HARM's card is placed, sized and visible in the slot it was added to") {
	// BEFORE ASKING WHETHER SOMETHING REDREW, ASK WHETHER IT WAS EVER PLACED. A visage frame is
	// zero-initialised and drawable, so a card added to the tree and never given bounds paints at the
	// window origin at zero size — which reads exactly like a card that failed to repaint. That is a
	// bug this lane has already had once, and it is the half of "did the picture arrive" that a
	// headless suite CAN answer.
	HarmFixture f;

	CHECK(f.card->isVisible());
	CHECK(f.card->width() > 0.0f);
	CHECK(f.card->height() > 0.0f);

	// AND WHERE THE LANE SAYS IT IS, not at the origin.
	const visage::Bounds expected = f.lane->cardBounds(f.slot);
	CHECK(f.card->x() == doctest::Approx(expected.x()));
	CHECK(f.card->y() == doctest::Approx(expected.y()));
	CHECK(f.card->width() == doctest::Approx(expected.width()));
	// The lane asks the card how tall it is; a card reporting one height and drawn at another would
	// leave a hole or an overlap below it.
	CHECK(f.card->height() == doctest::Approx(f.card->laneHeight()));

	// EVERY CONTROL ON IT IS INSIDE IT — a knob laid out past the card's bottom is hit-testable by
	// arithmetic and unreachable by a hand, which is the failure the Engine rail's rails already paid
	// for once.
	REQUIRE(!f.card->knobsForTest().empty());
	for (const auto& knob : f.card->knobsForTest()) {
		CAPTURE(std::string(knob->label() != nullptr ? knob->label() : "?"));
		CHECK(knob->isVisible());
		CHECK(knob->width() > 0.0f);
		CHECK(knob->height() > 0.0f);
		CHECK(knob->x() >= 0.0f);
		CHECK(knob->y() >= 0.0f);
		CHECK(knob->x() + knob->width() <= f.card->width() + 1.0f);
		CHECK(knob->y() + knob->height() <= f.card->height() + 1.0f);
	}
	// THE POLY SWITCH IS NOT ON THIS CARD ANY MORE — it moved to `HarmExpanded` with six knobs when
	// the card was reduced to three. The claim it made here (a control laid out past the card's
	// bottom) is made about the PANEL in the expanded view's own case rather than dropped.
	CHECK(f.card->knobsForTest().size() == ui::HarmCard::kCardKnobCount);

	// AND IT IS REACHABLE, WHICH IS NOT THE SAME AS BEING ON SCREEN AT SCROLL ZERO.
	//
	// THIS IS WHERE THE LANE'S OVERFLOW WAS FOUND. Two real cards do not fit: the filter's is 356 and
	// HARM's is 330 in a lane 534 tall, so HARM was placed with its bottom 86px past the window and
	// its last knob row and Poly switch were where no hand could go. The NoEditorCard that stood
	// there before only fit because a placeholder is short. The contract the modulator strip already
	// states is the right one — every card reachable from SOME scroll position — so that is what is
	// asserted, and the "nothing is outside" form would have been the wrong claim to make.
	REQUIRE(f.lane->maxScroll() > 0.0f); // or two cards fit and this case is asking nothing
	f.lane->setScroll(f.lane->maxScroll());
	const visage::Bounds scrolled = f.lane->cardBounds(f.slot);
	CAPTURE(scrolled.bottom());
	CAPTURE(f.lane->height());
	CHECK(scrolled.bottom() <= f.lane->height() + 1.0f);
	// The frames moved with the geometry rather than the geometry moving alone.
	CHECK(f.card->y() == doctest::Approx(scrolled.y()));
	// AND THE WHOLE CARD IS INSIDE THE LANE IN WINDOW COORDINATES, which is where a hand actually
	// arrives. NOT firstFrameOutside: once anything scrolls, the card ABOVE is legitimately off the
	// top and that helper would report it — a lane where nothing is ever out of frame is a lane that
	// does not scroll.
	CHECK(f.card->positionInWindow().y >= f.lane->positionInWindow().y - 1.0f);
	CHECK(f.card->positionInWindow().y + f.card->height()
	      <= f.lane->positionInWindow().y + f.lane->height() + 1.0f);
	CHECK(f.lane->positionInWindow().y + f.lane->height() <= f.root->height() + 1.0f);

	// AND THE FIRST CARD IS REACHABLE FROM THE OTHER END, which is the half that a lane scrolled to a
	// fixed offset would fail.
	f.lane->setScroll(0.0f);
	CHECK(f.lane->cardBounds(0).y() >= 0.0f);
	CHECK(f.lane->cardBounds(0).bottom() <= f.lane->height() + 1.0f);
}
/// EVERY SURFACE HARM PUTS CONTROLS ON, each one's own answer to "what can a hand reach here".
///
/// A VECTOR OF SURFACES AND NOT A SINGLE LIST, WHILE THERE IS STILL ONLY ONE. The split moves six
/// knobs and a switch off the card and into an overlay, and the invariant below has to become a UNION on that day. Writing the
/// union now — with one member, where it passes trivially — is the difference between a guard that
/// is already load-bearing when the split lands and a guard that gets edited BY the split.
///
/// THAT DISTINCTION IS THE WHOLE POINT. A test rewritten by the change it is meant to police has
/// stopped policing it, and the rewrite always looks like the maintenance the change requires.
struct HarmSurfaces {
	/// One entry per surface, in no particular order. Add the expanded view here and nothing below
	/// changes.
	std::vector<std::vector<fx::harm::Param>> surfaces;

	/// Everything reachable from anywhere.
	std::set<int> reachable() const {
		std::set<int> out;
		for (const auto& surface : surfaces) {
			for (const fx::harm::Param p : surface) {
				out.insert(static_cast<int>(p));
			}
		}
		return out;
	}

	/// How many surfaces offer this control. ONE IS THE ORDINARY ANSWER, zero is the bug the
	/// invariant exists for, and more than one is a decision rather than a defect — the scoping's §6
	/// leaves duplication permitted and unused, so this counts rather than forbidding.
	size_t offering(fx::harm::Param p) const {
		size_t count = 0;
		for (const auto& surface : surfaces) {
			for (const fx::harm::Param q : surface) {
				if (q == p) {
					++count;
					break; // twice on ONE surface is that surface's bug, checked separately
				}
			}
		}
		return count;
	}
};
static constexpr size_t kHarmSurfaceControls = static_cast<size_t>(fx::harm::Param::kCount) - 2;
TEST_CASE("every control HARM declares is reachable from at least one of its surfaces") {
	// THE GAP NOTHING LOOKS FOR IS A DECLARED CONTROL WITH NOTHING ON THE GLASS. Walking the widget
	// tree finds only widgets that exist; walking the DECLARATIONS and asking each surface is what
	// finds the control that does not — which is how the filter's Poly shipped with the engine
	// reading it and no hand able to set it.
	//
	// WHOEVER SPLITS HARM ACROSS TWO SURFACES: ADD TO `surfaces`, CHANGE NOTHING ELSE. If a control
	// leaving the card makes this go red, the fix is the missing surface, never a smaller sweep.
	HarmFixture f;
	HarmSurfaces surfaces;
	surfaces.surfaces.push_back(f.card->addressedParamsForTest());
	// THE SECOND SURFACE, ADDED AS PROMISED AND WITH NOTHING ELSE CHANGED. The union mechanism was
	// written a commit earlier, while there was one surface and it passed trivially — this line is
	// the whole of what the split cost the invariant, which is the point of having written it early.
	//
	// BUILT DIRECTLY RATHER THAN OPENED, because the question is what the SURFACE offers, not whether
	// a button opens it. The button has its own case.
	//
	// **THE SECOND SURFACE IS THE WINDOW, AND ITS HEADER IS PART OF IT.** Since 2026-09-26 `Enabled` is
	// the window header's chip, which every window carries for its device (`ExpandedView::
	// deviceEnableControl`), not a square in HARM's body. So the panel is built the way the editor
	// builds it — `makeExpandedView`, which stamps the device the header asks about — and the window's
	// surface is its body's list plus that one chip.
	ui::WidgetRegistry registry;
	std::unique_ptr<ui::ExpandedView> built =
	    ui::makeExpandedView(f.deviceHost, &f.bridge, registry, f.device, fx::harm::kDescriptor);
	auto* body = dynamic_cast<ui::HarmExpanded*>(built.get());
	REQUIRE(body != nullptr);
	REQUIRE(built->deviceEnableControl() == static_cast<size_t>(fx::harm::Param::Enabled));
	struct {
		ui::HarmExpanded* body;
		size_t header;
		std::vector<fx::harm::Param> addressedParamsForTest() const {
			std::vector<fx::harm::Param> out = body->addressedParamsForTest();
			out.push_back(static_cast<fx::harm::Param>(header));
			return out;
		}
	} expanded { body, built->deviceEnableControl() };
	surfaces.surfaces.push_back(expanded.addressedParamsForTest());
	REQUIRE(surfaces.surfaces.size() == 2);

	SUBCASE("no surface addresses the same control twice") {
		for (const auto& surface : surfaces.surfaces) {
			std::set<int> seen;
			for (const fx::harm::Param p : surface) {
				CAPTURE(std::string(fx::harm::kParamDescs[static_cast<size_t>(p)].name));
				CHECK(seen.insert(static_cast<int>(p)).second);
			}
		}
	}

	SUBCASE("every declared control is offered by at least one surface") {
		// THE SWEEP IS OVER THE DECLARATIONS, so a control added to the device tomorrow is named here
		// tomorrow without this file changing. That is what caught the MSEG's TimeBase.
		for (size_t i = 0; i < static_cast<size_t>(fx::harm::Param::kCount); ++i) {
			if (harmSlotFurniture(i)) {
				continue;
			}
			const auto param = static_cast<fx::harm::Param>(i);
			CAPTURE(std::string(fx::harm::kParamDescs[i].name));
			CHECK(surfaces.offering(param) >= 1);
		}
	}

	SUBCASE("THE PANEL ALONE REACHES ALL TWELVE — self-contained means self-contained") {
		// The expanded panel holds every control: it is self-contained. So this is the one subcase that
		// does NOT take the union: a panel that needed the card to complete it would satisfy every other
		// assertion here and still fail the requirement.
		//
		// SWEPT FROM `kParamDescs`, so a control added to HARM tomorrow is required here tomorrow
		// without this file changing.
		std::set<int> onPanel;
		for (const fx::harm::Param p : expanded.addressedParamsForTest()) {
			onPanel.insert(static_cast<int>(p));
		}
		for (size_t i = 0; i < static_cast<size_t>(fx::harm::Param::kCount); ++i) {
			if (harmSlotFurniture(i)) {
				CHECK(onPanel.count(static_cast<int>(i)) == 0); // the slot's, never the window's too
				continue;
			}
			CAPTURE(std::string(fx::harm::kParamDescs[i].name));
			CHECK(onPanel.count(static_cast<int>(i)) == 1);
		}
		CHECK(onPanel.size() == kHarmSurfaceControls);
		// AND THE CARD DOES NOT — or "the panel is complete" would be true of a surface that had
		// simply been given everything back, and the reduction would have been undone.
		std::set<int> onCard;
		for (const fx::harm::Param p : f.card->addressedParamsForTest()) {
			onCard.insert(static_cast<int>(p));
		}
		CHECK(onCard.size() < static_cast<size_t>(fx::harm::Param::kCount));
	}

	SUBCASE("and no surface offers a control the device does not declare") {
		// A SURFACE INVENTING A CONTROL is the other direction and is just as wrong — which is
		// precisely what a notch-centre box would be, since the centre is derived and not a setting.
		const std::set<int> reachable = surfaces.reachable();
		CHECK(reachable.size() == kHarmSurfaceControls);
		for (const int p : reachable) {
			CHECK(p >= 0);
			CHECK(p < static_cast<int>(fx::harm::Param::kCount));
		}
	}

	SUBCASE("duplication across surfaces is COUNTED, not forbidden") {
		// THE SCOPING'S §6 LEAVES THIS OPEN AND ANSWERS IT "allow it, do not use it yet". The
		// invariant therefore PERMITS a control on two surfaces, and this records how much of it
		// there is as a VALUE — so duplication arriving by accident is visible, and duplication
		// arriving by decision is written down.
		//
		// UPDATING THIS NUMBER IS NOT THE THING THIS CASE FORBIDS. The forbidden edit is narrowing
		// the SWEEP — removing a control from the reachability check because it moved. Recording a
		// changed count is stating a fact; deleting a control from the sweep is switching the guard
		// off. The three subcases above are what must survive untouched, and they do.
		// FIVE, AND EVERY ONE IS A DECISION. Ten while the card carried everything; THREE after the
		// reduction — Level, Attack and Release, which the panel keeps because it is meant to be
		// complete on its own; FOUR once the harmonic row became a shared widget; FIVE once the panel
		// was required to hold `Enabled` too — which it still does, in the window's header chip
		// rather than its body (2026-09-26).
		// SIX with the Notch / High-pass chip (backlog NL, 2026-10-26): on the card beside the harmonic row it acts on,
		// and in the window because the window is the card's superset — a duplicate by rule, not by accident.
		//
		// THE CARD HOLDS FOUR MASTER KNOBS (Master, Depth, Attack, Release), the head square, and the selected tab's frame: each partial's Active, harmonic and Level (twelve). The window holds every one: seventeen, all by rule. Before the frame: the two masters, the envelope's six,
		// Poly, Env Depth, and the head square. Eleven, all by rule (the card is the window's subset). The per-partial controls
		// are on the window alone, so none of them is counted. (The harmonic row and the partial switches left the card.)
		constexpr size_t kExpectedDuplicates = 17;
		size_t duplicated = 0;
		for (size_t i = 0; i < static_cast<size_t>(fx::harm::Param::kCount); ++i) {
			if (surfaces.offering(static_cast<fx::harm::Param>(i)) > 1) {
				CAPTURE(std::string(fx::harm::kParamDescs[i].name));
				++duplicated;
			}
		}
		CHECK(duplicated == kExpectedDuplicates);
	}
}
TEST_CASE("a drag on HARM's card leaves the value in the row the engine decodes") {
	// A DIFFERENCE IS NOT A VALUE. "The row changed" passes for a drag that wrote the right number to
	// the wrong control, the wrong slot, or the right slot of the wrong device — and the wrong-slot
	// bug is the live hazard here, because HARM sits behind the filter and both are rows of floats.
	//
	// SO THE EXPECTED NUMBER IS RESTATED FROM THE KNOB'S OWN DECLARED TRAVEL rather than read back
	// from anything the drag touched: Knob::dragTo moves `travel / kDragRange` of the declared range
	// per pixel, anchored at the value the knob held when the press landed.
	HarmFixture f;

	// THE SUBJECT IS LEVEL AND IT USED TO BE WIDTH. Width moved to the expanded view with the card
	// reduction, and this case drives THROUGH THE ROOT — so it has to name a control the card still
	// carries or it is testing a frame that is not in the tree. The claim is unchanged and Level's
	// declared range is Width's (0..1 Percent), so the arithmetic below is the same arithmetic.
	//
	// THE PANEL'S OWN ROAD IS ASSERTED SEPARATELY, in "HARM's head opens its expanded view" — there
	// through the overlay, which is the only place the panel is really in the tree.
	ui::Knob* width = f.cardKnob("Master");
	REQUIRE(width != nullptr);

	// STARTED FROM A STATED PLACE RATHER THAN FROM THE DEFAULT. This read `REQUIRE(Level == 0)` and
	// called it "off by default, always"; the default Level became 1.0 and the word "always" became false.
	// A knob drag is anchored at whatever the knob held when the press landed, so the case has to say
	// where that is instead of inheriting it.
	{
		ui::FxControl start { f.bridge, f.device, fx::harm::kDescriptor,
			                  static_cast<size_t>(fx::harm::Param::MasterLevel) };
		start.setValue(0.0);
		f.bridge.refresh();
	}
	REQUIRE(f.param(fx::harm::Param::MasterLevel) == doctest::Approx(0.0));

	constexpr float kTravel = 35.0f;
	const device::ParamDesc& desc = fx::harm::kParamDescs[static_cast<size_t>(fx::harm::Param::MasterLevel)];
	const double expected = desc.min
	                        + static_cast<double>(kTravel / ui::Knob::kDragRange)
	                              * (desc.max - desc.min);
	REQUIRE(expected > 0.0);

	// SCROLLED SO THE KNOB IS ACTUALLY THERE TO BE DRAGGED. The card hangs off the end of the lane
	// until it is scrolled to, and this case broke the day Width moved from the upper knob row to the
	// lower one — the drag went on landing wherever the pointer fell through to. `reveal()` exists
	// for this, and the hit test below is what stops it being silent next time.
	f.reveal();
	const visage::Point centre = f.faceCentre(width);
	REQUIRE(f.root->frameAtPoint(centre) == width);
	dragThroughRoot(f.root, centre, { centre.x, centre.y - kTravel });

	// THE VALUE, THROUGH THE DECODER THE AUDIO THREAD USES.
	CHECK(f.state().settings.masterLevel == doctest::Approx(static_cast<float>(expected)));
	// AND IN HARM'S SLOT ONLY. The filter is in slot 0 and its own controls must not have moved — the
	// assertion a card wired to slot 0 fails on.
	//
	// **COMPARED AGAINST WHAT THE FIXTURE STARTED WITH, NOT AGAINST `State {}`.** It used to be the
	// same object: the pinned filter was stamped from a default-constructed State. It stopped being
	// so on 2026-09-04, when every fx device's enable default became ON and `defaultFxChain` began
	// switching the PINNED filter off by name (see audition.cpp for why the factory patch is not an
	// "added" device). Reading the reference out of `defaultFxChain()` is what this case always
	// meant — "slot 0 is as we found it" — and it cannot drift from the fixture again.
	const fx::filter::State filter = fxFilterAt(f.bridge.fxChain(), 0);
	CHECK(filter == fxFilterAt(feathers::defaultFxChain(), 0));

	SUBCASE("and a second control writes its own field rather than its neighbour's") {
		// ONE FIELD PER CONTROL. A row is an array of floats and an off-by-one in the control index
		// writes a plausible number into the wrong control — which sounds like a bug in the DSP.
		// THE SECOND CONTROL IS ATTACK, and it is a LogSeconds control where the first was a Percent
		// — so its expected value is computed from its own declaration rather than from the first's,
		// which is what makes "each control writes its own field" a claim about two different fields
		// rather than about one arithmetic repeated.
		ui::Knob* attack = f.cardKnob("Attack");
		REQUIRE(attack != nullptr);
		const device::ParamDesc& attackDesc =
		    fx::harm::kParamDescs[static_cast<size_t>(fx::harm::Param::Attack)];
		const double attackKnob = static_cast<double>(kTravel / ui::Knob::kDragRange)
		                          * (attackDesc.max - attackDesc.min);
		f.reveal();
		const visage::Point at = f.faceCentre(attack);
		REQUIRE(f.root->frameAtPoint(at) == attack);
		dragThroughRoot(f.root, at, { at.x, at.y - kTravel });

		// THE STORED FIELD IS SECONDS AND THE KNOB IS LOG2 OF IT — the device's own declared domain,
		// so the expectation is exponentiated here rather than the row being read raw.
		CHECK(static_cast<double>(f.state().settings.attackSeconds)
		      == doctest::Approx(std::exp2(attackDesc.min + attackKnob)).epsilon(1e-4));
		// AND LEVEL IS STILL WHERE THE FIRST DRAG LEFT IT.
		CHECK(f.state().settings.masterLevel == doctest::Approx(static_cast<float>(expected)));
	}
}
TEST_CASE("HARM's arrows step the harmonic table by exactly one entry") {
	// THE RATIO IS THE ONE SETTING THE CARD CAN STATE EXACTLY, and the arrows are how it is set. A
	// step is a specific number — the next index — rather than "somewhere further along", which is
	// what a difference check would accept.
	HarmFixture f;

	// The default is unison, and the box says so in the vocabulary a player reads.
	CHECK(f.param(fx::harm::Param::Harmonic)
	      == doctest::Approx(static_cast<double>(fx::harm::kUnisonHarmonic)));
	CHECK(f.expanded().harmonicRow()->ratioText() == "1:1");

	f.pressRow(f.rowNext());
	CHECK(f.param(fx::harm::Param::Harmonic)
	      == doctest::Approx(static_cast<double>(fx::harm::kUnisonHarmonic) + 1.0));
	// AND THE BOX SAYS THE TABLE'S OWN RATIO, not a decimal and not the index.
	CHECK(f.expanded().harmonicRow()->ratioText() == "9:8");

	f.pressRow(f.rowPrev());
	CHECK(f.param(fx::harm::Param::Harmonic)
	      == doctest::Approx(static_cast<double>(fx::harm::kUnisonHarmonic)));
	CHECK(f.expanded().harmonicRow()->ratioText() == "1:1");

	SUBCASE("and it stops at the ends of the table rather than wrapping") {
		for (size_t i = 0; i < fx::harm::kHarmonicCount + 4; ++i) {
			f.pressRow(f.rowPrev());
		}
		CHECK(f.param(fx::harm::Param::Harmonic) == doctest::Approx(0.0));
		CHECK(f.expanded().harmonicRow()->ratioText() == "1:8");

		for (size_t i = 0; i < fx::harm::kHarmonicCount + 4; ++i) {
			f.pressRow(f.rowNext());
		}
		CHECK(f.param(fx::harm::Param::Harmonic)
		      == doctest::Approx(static_cast<double>(fx::harm::kHarmonicCount) - 1.0));
		CHECK(f.expanded().harmonicRow()->ratioText() == "32:1");
	}

	SUBCASE("and the head is still the fold, so a step cannot collapse the card") {
		CHECK_FALSE(f.card->folded());
		f.pressRow(f.rowNext());
		CHECK_FALSE(f.card->folded());
		f.foldPress(f.card->headBounds());
		CHECK(f.card->folded());
		CHECK(f.card->laneHeight() == doctest::Approx(ui::HarmCard::foldedHeight()));
		// A folded card hides its controls rather than drawing them under the card above.
		for (const auto& knob : f.card->knobsForTest()) {
			CHECK_FALSE(knob->isVisible());
		}
	}
}
TEST_CASE("HARM's card never says HPF, and its width control says what it is") {
	// THE FIRMWARE'S RENAME NEVER FINISHED — thirteen HPF-named identifiers survive in the source this
	// was ported from, its design doc says HPF thirty-six times and notch zero, and the knob those
	// names describe is WIDTH with the meaning inverted: 1 is surgical, 127 is wide.
	//
	// A NAME IS CHECKED AGAINST NOTHING AND TRAVELS INTO EVERY CALL SITE, which is why the lie greps.
	// This is the line where it would have reached a player, so this is where it is stopped.
	HarmFixture f;

	// SWEPT ACROSS BOTH SURFACES. Width moved to the expanded view with the card reduction, and the
	// claim "no surface of this device calls it HPF" is the one worth holding — narrowing the sweep
	// to whichever surface still happened to carry it would have let the lie back in through the
	// other one.
	bool sawWidth = false;
	const auto sweep = [&](const std::vector<std::unique_ptr<ui::Knob>>& knobs) {
		for (const auto& knob : knobs) {
			REQUIRE(knob->label() != nullptr);
			const std::string label = knob->label();
			CAPTURE(label);
			CHECK(label.find("HPF") == std::string::npos);
			CHECK(label.find("Hpf") == std::string::npos);
			CHECK(label.find("Cutoff") == std::string::npos); // nor the other half of the same lie
			sawWidth = sawWidth || label == "Width";
		}
	};
	sweep(f.card->knobsForTest());
	sweep(f.expanded().knobsForTest());
	CHECK(sawWidth);

	// AND THE DEVICE'S OWN DECLARATION AGREES, which is what stops the card and the engine drifting
	// apart: the control the knob addresses is the one the DSP reads as width.
	ui::Knob* width = f.knob("Width");
	REQUIRE(width != nullptr);
	REQUIRE(width->control() != nullptr);
	CHECK(std::string(width->control()->desc().name) == "Harm Width");

	// WIDE MEANS WIDE. The width control runs the notch's Q the OTHER way, and a card whose knob
	// climbed toward "surgical" while its label said Width would be the same lie with a new spelling.
	// Asserted through the device's own maths rather than restated here.
	fx::harm::Settings narrow;
	narrow.width = 0.05f;
	fx::harm::Settings wide;
	wide.width = 1.0f;
	CHECK(fx::harm::notchQ(narrow) > fx::harm::notchQ(wide));
}
TEST_CASE("HARM's card offers no way to set the notch centre") {
	// THE CENTRE IS NOT A CONTROL AND MUST NOT LOOK LIKE ONE. It is derived per buffer from the note
	// being played, the harmonic ratio, fine tune and pitch bend — so a box a hand could drag would be
	// inventing a control the device does not have, and a hertz reading on the card would be true for
	// one note and wrong for every other.
	HarmFixture f;

	// THE CARD HAS NO NOTE, which is the whole reason. What it states instead is the RATIO, and a
	// ratio is true at every pitch.
	CHECK(f.expanded().harmonicRow()->ratioText().find(':') != std::string::npos);
	CHECK(f.expanded().harmonicRow()->ratioText().find("Hz") == std::string::npos);

	// AND THE SAME SETTING READS AS TWO DIFFERENT CENTRES depending only on the note — the fact that
	// makes a drawn frequency dishonest, asserted rather than asserted-about.
	fx::harm::Settings low = f.state().settings;
	low.noteSemitones = 36.0f;
	fx::harm::Settings high = low;
	high.noteSemitones = 72.0f;
	CHECK(fx::harm::notchCentreHz(low) != doctest::Approx(fx::harm::notchCentreHz(high)));

	// The controls the card addresses are exactly the device's declared ones — see the reachability
	// case — so there is no centre among them. This states the other half: none of the ten is a
	// frequency at all.
	for (size_t i = 0; i < static_cast<size_t>(fx::harm::Param::kCount); ++i) {
		CAPTURE(std::string(fx::harm::kParamDescs[i].name));
		CHECK(fx::harm::kParamDescs[i].unit != device::Unit::LogHertz);
	}
}
TEST_CASE("HARM's Poly switch is live now, and the card learned that from the device") {
	// THIS CASE USED TO ASSERT THE OPPOSITE AND WAS RIGHT TO. HARM declared no per-voice road, so the
	// switch changed nothing and the card drew it DEAD with the reason in its caption. The road exists
	// now, the declaration followed it, and the switch came alive — **without this file being
	// edited to say so**, which was the whole point of deriving it from `perVoiceControl` instead of
	// hardcoding a false.
	//
	// INVERTED RATHER THAN DELETED, on this repo's rule: a refusal quietly becoming an acceptance must
	// not be indistinguishable from the feature landing. What changed is the ANSWER; the question is
	// the same one, and it is still the honesty rule's — a control is enabled iff it changes sound.
	//
	// AND IT IS ASKED OF THE EXPANDED VIEW NOW, not the card. Poly moved there with six knobs when
	// the split reduced the card to three — so this case follows the CONTROL rather than staying
	// pointed at the surface that used to hold it. Deleting it would have been the split paying for
	// itself with a guarantee.
	HarmFixture f;
	ui::WidgetRegistry registry;
	ui::HarmExpanded panel { f.deviceHost, registry, f.device };
	ui::SwitchChip* poly = panel.polyForTest();
	REQUIRE(poly != nullptr);

	// THE DEVICE'S OWN DECLARATION, and the card agreeing with it. Stated as an equivalence rather
	// than as two constants, because what is being pinned is that the card ASKS: a card that
	// hardcoded `true` would pass a bare `polyIsLive()` check and would be wrong again the day some
	// other device's road is the one missing.
	CHECK(fx::harm::kDescriptor.traits.perVoiceControl
	      == static_cast<int>(fx::harm::Param::Poly));
	// THE SURFACE ASKS THE DEVICE, stated as an equivalence rather than as a constant: a panel that
	// hardcoded `true` would pass a bare "it is enabled" check and would be wrong again the day some
	// other device's road is the one missing.
	CHECK(poly->usable() == (fx::harm::kDescriptor.traits.perVoiceControl >= 0));
	CHECK(poly->usable());

	// AND A HAND MOVES IT. This is the assertion that was inverted: the same routed click that had to
	// leave the value alone must now change it, in the row the engine decodes.
	CHECK(f.param(fx::harm::Param::Poly) == doctest::Approx(0.0));
	CHECK_FALSE(f.state().poly);
	// DRIVEN THROUGH THE SWITCH'S OWN HANDLER at a place inside it. The panel is not in the editor's
	// tree here — it is constructed directly, the way the surface-union case constructs it — so the
	// press goes to the widget rather than through the root. What is being asserted is the CONTROL,
	// and the road from the head to the panel has its own case.
	panel.setBounds(0, 0, 420, 340);
	const visage::Bounds sw = panel.polyBounds();
	poly->onMouseDown().callback(
	    at(sw.width() * 0.5f, sw.height() * 0.5f));
	CHECK(f.param(fx::harm::Param::Poly) == doctest::Approx(1.0));
	CHECK(f.state().poly);

	// AND BACK, so the switch is a switch rather than a one-way latch.
	poly->onMouseDown().callback(at(sw.width() * 0.5f, sw.height() * 0.5f));
	CHECK(f.param(fx::harm::Param::Poly) == doctest::Approx(0.0));

	SUBCASE("and the caption stopped making an excuse for it") {
		// THE OLD CAPTION IS NOW A LIE and leaving it would be the honesty rule failing in the other
		// direction — a working control labelled as unbuilt. A word on its own is the right caption
		// for a control that works.
		CHECK(panel.polyText() == "Poly");
		CHECK(panel.polyText().find("not built") == std::string::npos);
		CHECK(panel.polyText().find("yet") == std::string::npos);
	}

	SUBCASE("and the chip's word fits the chip") {
		panel.setBounds(0, 0, panel.minimumSize().x, panel.minimumSize().y);
		const float drawn = ui::trackedWidth(ui::font(ui::kKnobLabelSize), poly->label(), 0.0f);
		CAPTURE(drawn);
		CHECK(drawn <= static_cast<float>(poly->width()));
		CHECK(poly->x() + poly->width() <= panel.width());
	}
}
TEST_CASE("the fx lane scrolls, because two real cards do not fit in it") {
	// THE WALL THE MODULATOR STRIP HIT WITH FIVE CARDS, reached here with TWO — because an fx card is
	// most of the lane's height. Measured at the canvas the layout is written for: the lane is 534
	// tall, the filter's card is 356 and HARM's is 330.
	//
	// THE ANSWER IS NEVER A TALLER WINDOW. The logical canvas has a floor and everything is written to
	// fit inside it, so the lane scrolls and nothing is unreachable. This case is the contract:
	// never past the end, never off the top, every card reachable from some scroll position.
	HarmFixture f;

	CHECK(f.lane->contentHeight() > f.lane->height()); // or there is nothing to be scrolled
	REQUIRE(f.lane->maxScroll() > 0.0f);
	CHECK(f.lane->maxScroll()
	      == doctest::Approx(f.lane->contentHeight() - f.lane->height()));

	SUBCASE("it will not scroll past either end") {
		f.lane->setScroll(-500.0f);
		CHECK(f.lane->scroll() == doctest::Approx(0.0f));
		f.lane->setScroll(f.lane->maxScroll() + 500.0f);
		CHECK(f.lane->scroll() == doctest::Approx(f.lane->maxScroll()));
	}

	SUBCASE("every card is reachable from some scroll position") {
		// THE CONTRACT STATED AS A SWEEP rather than as two endpoints, because "the first and last
		// fit" is satisfied by a lane that skips whatever is between them.
		//
		// AND THIS IS THE OVERFLOW GUARD IN THE ONLY FORM THAT SURVIVES SCROLLING. The check that
		// FOUND the overflow was "no frame is outside the window", and that check cannot be kept once
		// the lane scrolls: a card above the viewport is legitimately off the top, so a lane where
		// nothing is ever out of frame is a lane that does not scroll. What the old check was really
		// asking — can a hand get to this — is this, and it is strictly stronger, because scrolling
		// does not rescue a card that overflows for the OTHER reason.
		for (size_t slot = 0; slot < f.lane->deviceCount(); ++slot) {
			CAPTURE(slot);
			bool reachable = false;
			for (float y = 0.0f; y <= f.lane->maxScroll() + 1.0f; y += 8.0f) {
				f.lane->setScroll(y);
				const visage::Bounds box = f.lane->cardBounds(slot);
				if (box.y() >= -1.0f && box.bottom() <= f.lane->height() + 1.0f) {
					reachable = true;
					break;
				}
			}
			CHECK(reachable);
		}
	}

	SUBCASE("and no card is taller than the lane, which scrolling cannot rescue") {
		// THE FAILURE THE THIRD FX CARD WILL HAVE, and the one the scroll does NOT fix. A lane that
		// scrolls answers "the cards together are taller than the lane". It has no answer at all to
		// "ONE card is taller than the lane" — such a card has no scroll position at which its head
		// and its bottom row are on screen together, so some control of it is unreachable from every
		// position. HARM's is 330 of 534 and the filter's is 356, so there is real room left; this is
		// the line that says how much, and it is the guard a fourth device trips before a player does.
		//
		// ASKED OF WHATEVER THE LANE HOLDS rather than of the two devices that exist today, so it
		// keeps asking when a card nobody here has written arrives.
		f.lane->setScroll(0.0f);
		REQUIRE(f.lane->deviceCount() > 0);
		for (size_t slot = 0; slot < f.lane->deviceCount(); ++slot) {
			ui::FxCard* card = f.lane->cardAt(slot);
			REQUIRE(card != nullptr);
			CAPTURE(slot);
			CAPTURE(card->laneHeight());
			CAPTURE(f.lane->height());
			// The gaps above and below it have to fit too, or the card is reachable only by being
			// jammed against both edges of the lane at once.
			CHECK(card->laneHeight() + 2.0f * ui::FxLane::kSlotGap <= f.lane->height());
		}
	}

	SUBCASE("a fold shortens the content and pulls the scroll back with it") {
		// THE FOLD THAT DID NOT REACH THE SCROLL EXTENT is the modulator strip's own recorded bug, one
		// lane over: a card that shrank while the lane stayed scrolled past its new end would place
		// every card above the top and there would be no way back down.
		f.lane->setScroll(f.lane->maxScroll());
		const float before = f.lane->scroll();
		REQUIRE(before > 0.0f);

		auto* filter = f.lane->filterCard();
		REQUIRE(filter != nullptr);
		filter->setFolded(true);
		f.lane->resized();

		CHECK(f.lane->contentHeight() < before + f.lane->height() + 1.0f);
		CHECK(f.lane->scroll() <= f.lane->maxScroll() + 0.001f);
		// And the card the fold was meant to make room for is on screen without touching the wheel.
		const visage::Bounds box = f.lane->cardBounds(f.slot);
		CHECK(box.y() >= -1.0f);
		CHECK(box.bottom() <= f.lane->height() + 1.0f);
	}

	SUBCASE("and the wheel is not consumed by a lane with nothing to scroll") {
		// A LANE THAT SWALLOWED THE EVENT REGARDLESS would eat the wheel for whatever is behind it, and
		// it would do so in exactly the configuration where it has nothing to offer in return.
		visage::MouseEvent wheel;
		wheel.precise_wheel_delta_y = -1.0f;
		CHECK(f.lane->mouseWheel(wheel)); // two cards: there is something to scroll
		CHECK(f.lane->scroll() > 0.0f);

		feathers::FxChain empty;
		empty.count = 0;
		f.bridge.restoreFxChain(empty);
		f.lane->resized();
		REQUIRE(f.lane->deviceCount() == 0);
		REQUIRE(f.lane->maxScroll() == doctest::Approx(0.0f));
		CHECK_FALSE(f.lane->mouseWheel(wheel));
		// And an emptied lane is not left scrolled to where its cards used to be.
		CHECK(f.lane->scroll() == doctest::Approx(0.0f));
	}
}
TEST_CASE("HARM's card reads as inert until it is SWITCHED ON, like every other effect") {
	// THIS CASE ASSERTED THE OPPOSITE AND WAS RIGHT TO. HARM declared no Enabled control, so the card
	// dimmed on `Level > 0 || Width > 0` — a device is inert when both halves are down — and drawing
	// a switch the device did not have is what the honesty rule forbids.
	//
	// CHANGED THE DEVICE, NOT THE CARD, 2026-08-24: HARM gets the same active/inactive switch as
	// every other effect, and the 'level=0 means off' semantics go — that convention came from the
	// hardware HARM was ported from, which this is not. So HARM declares Enabled, and the card asks
	// the one control every other effect's card asks.
	//
	// INVERTED RATHER THAN DELETED. What changed is which control decides; the question — does this
	// card tell the truth about whether the device is doing anything — is the one it always asked.
	HarmFixture f;
	visage::Canvas canvas;

	// **ON BY DEFAULT NOW, LIKE EVERY OTHER EFFECT** — every effect defaults to on when it is added.
	// This said "OFF BY DEFAULT, like the filter. A device added to a chain does nothing until a hand
	// asks", and the second sentence is what changed: the press on (+) is the hand asking.
	//
	// **SO THE OFF STATE IS NOW REACHED BY A PRESS RATHER THAN BY ARRIVING, and the case is stronger
	// for it.** What it claims is that the card is dim IFF the device is switched off — the starting
	// state was never the subject. Asserting the arrival value first is what keeps the press below
	// from being a no-op that would leave every dim-check reading a state nobody set.
	REQUIRE(f.param(fx::harm::Param::Enabled) == doctest::Approx(1.0));
	{
		const visage::Bounds arrival = f.card->enableBounds();
		f.card->onMouseDown().callback(
		    at(arrival.x() + arrival.width() * 0.5f, arrival.y() + arrival.height() * 0.5f));
	}
	REQUIRE(f.param(fx::harm::Param::Enabled) == doctest::Approx(0.0));
	f.card->draw(canvas);
	for (const auto& knob : f.card->knobsForTest()) {
		CAPTURE(std::string(knob->label()));
		CHECK_FALSE(knob->active());
	}

	SUBCASE("the switch alone wakes it, with every other control still at zero") {
		// THE WHOLE OF THE CHANGE IN ONE ASSERTION. Under the retired convention this arrangement —
		// switched on, both halves down — was the definition of "off" and the card would have drawn it
		// dim. It is IN THE SIGNAL now and the card says so, whatever its knobs are set to.
		f.card->partAt({ 0.0f, 0.0f }); // no-op, keeps the geometry honest below
		const visage::Bounds sw = f.card->enableBounds();
		f.card->onMouseDown().callback(
		    at(sw.x() + sw.width() * 0.5f, sw.y() + sw.height() * 0.5f));

		CHECK(f.param(fx::harm::Param::Enabled) == doctest::Approx(1.0));
		// BOTH HALVES DOWN, SET EXPLICITLY. The claim is that a switched-ON HARM reads as live even
		// with nothing dialled in — which used to be the default state and is not any more. Stating
		// it keeps the case testing the STRONGEST version rather than whichever one the defaults
		// happen to make easy.
		for (const auto param : { fx::harm::Param::MasterLevel, fx::harm::Param::MasterDepth }) {
			ui::FxControl c { f.bridge, f.device, fx::harm::kDescriptor, static_cast<size_t>(param) };
			c.setValue(0.0);
		}
		f.bridge.refresh();
		REQUIRE(f.state().settings.masterLevel == doctest::Approx(0.0f));
		REQUIRE(f.state().settings.masterDepth == doctest::Approx(0.0f));
		f.card->draw(canvas);
		for (const auto& knob : f.card->knobsForTest()) {
			CAPTURE(std::string(knob->label()));
			CHECK(knob->active());
		}
	}

	SUBCASE("and turning the halves up does NOT wake it, which is the retired convention") {
		// THE OTHER DIRECTION, and the one that pins the removal rather than the addition. Level and
		// Width no longer double as a power switch: a HARM with both up and its switch off is off.
		f.knob("Master")->control()->setValue(1.0);
		f.knob("Depth")->control()->setValue(1.0);
		REQUIRE(f.state().settings.masterLevel == doctest::Approx(1.0f));
		REQUIRE(f.state().settings.masterDepth == doctest::Approx(1.0f));
		f.card->draw(canvas);
		for (const auto& knob : f.card->knobsForTest()) {
			CAPTURE(std::string(knob->label()));
			CHECK_FALSE(knob->active());
		}
	}

	SUBCASE("the switch is in the head, does not collide with the dots, and is hit before the fold") {
		// THE HEAD NOW HAS THREE OCCUPANTS — the name, the switch and the slot menu — and the head's
		// own verb is FOLD, so both small targets have to be tested before it or they are unclickable
		// and the bug reads as "the button does nothing". FilterCard records the same trap.
		const visage::Bounds sw = f.card->enableBounds();
		const visage::Bounds dots = f.card->slotMenuBounds();
		CHECK(sw.right() <= dots.x());
		CHECK(sw.y() >= 0.0f);
		CHECK(sw.bottom() <= ui::HarmCard::kHeadHeight);
		const auto centre = [](visage::Bounds b) {
			return visage::Point { b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f };
		};
		CHECK(f.card->partAt(centre(sw)) == ui::HarmCard::Part::Enable);
		CHECK(f.card->partAt(centre(dots)) == ui::HarmCard::Part::SlotMenu);
		CHECK(f.card->partAt({ 20.0f, ui::HarmCard::kHeadHeight * 0.5f })
		      == ui::HarmCard::Part::Fold);
	}

	SUBCASE("and the card's controls wake with the device") {
		// THIS ASKED ABOUT THE POLY SWITCH and now asks about the knobs, because Poly moved to the
		// expanded view with the card reduction. The claim is unchanged — pressing the enable square
		// takes the card's controls out of their switched-out grey — and it is asked of whatever the
		// card still holds rather than dropped along with the control it used to name.
		//
		// WHAT DID NOT MOVE WITH IT, stated because it is a real difference and not an oversight:
		// `HarmExpanded` does NOT dim while the device is switched out. The card dims because it sits
		// in a lane beside devices that are running; a panel you deliberately opened to edit one
		// device is not making that comparison. If it is ever wanted, it is one line and a case.
		REQUIRE_FALSE(f.card->knobsForTest().empty());
		for (const auto& knob : f.card->knobsForTest()) {
			CHECK_FALSE(knob->active());
		}
		const visage::Bounds sw = f.card->enableBounds();
		f.card->onMouseDown().callback(
		    at(sw.x() + sw.width() * 0.5f, sw.y() + sw.height() * 0.5f));
		f.card->draw(canvas);
		for (const auto& knob : f.card->knobsForTest()) {
			CAPTURE(std::string(knob->label() != nullptr ? knob->label() : "?"));
			CHECK(knob->active());
		}
	}
}
TEST_CASE("the fx lane's (+) offers what the DEVICES say can render audio, and every modulator the strip offers") {
	// DERIVED, NEVER LISTED, and this is the case that says so. The lane asks a descriptor's own `audio`
	// pointer, and — since 2026-10-04 (every mod effect is in scope in the fx rack too) — the strip's own
	// pair of questions (`maySitInModRack` and the `modulator` tag), so a device
	// earns a seat in this menu by what it declares rather than by anyone remembering to type its name here.
	//
	// INVERTED, NOT DELETED: this said the menu offered the audio devices and "not the two modulators".
	// The load-bearing half was always that the offered set equals an INDEPENDENT RESTATEMENT of the rule
	// over the whole registry — a menu that offered every device, or only the old audio set, fails it.
	FilterFixture f;
	auto* lane = f.lane;
	REQUIRE(lane != nullptr);

	const std::vector<const feathers::device::Descriptor*> offered = lane->addableForTest();
	REQUIRE(!offered.empty());

	// EVERY DEVICE IN THE REGISTRY, ITS OFFER RESTATED HERE: an audio device, or a modulator the strip offers.
	size_t audio = 0;
	size_t modulators = 0;
	for (const feathers::device::Descriptor& d : feathers::kDevices) {
		const bool isOffered = std::find(offered.begin(), offered.end(), &d) != offered.end();
		const bool stripOffers = d.outputCount() > 0 && feathers::belongsInModulationRack(d);
		CAPTURE(std::string(d.traits.name));
		CHECK(isOffered == (d.audio != nullptr || stripOffers));
		audio += isOffered && d.audio != nullptr ? 1 : 0;
		modulators += isOffered && d.audio == nullptr ? 1 : 0;
	}
	// BOTH HALVES ARE PRESENT, so the restatement above is not satisfied by a registry that happens to hold one kind.
	CHECK(audio >= 2);
	CHECK(modulators >= 4);

	SUBCASE("which today is the filter and HARM, and the modulators — DAHDSR, MSEG, LFO and the Tracker among them") {
		// THE CONCRETE READING OF THE RULE ABOVE, so a change to the registry that quietly stops offering a
		// modulator in the lane has to come through here and be argued for.
		std::set<std::string> names;
		for (const feathers::device::Descriptor* d : offered) {
			names.insert(d->traits.name);
		}
		CHECK(names.count(fx::filter::kTraits.name) == 1);
		CHECK(names.count(fx::harm::kTraits.name) == 1);
		CHECK(names.count(mod::dahdsr::kDescriptor.traits.name) == 1);
		CHECK(names.count(mod::mseg::kDescriptor.traits.name) == 1);
		CHECK(names.count(std::string(feathers::deviceForTag(mod::lfo::kTraits.tag)->traits.name)) == 1);
		CHECK(names.count(std::string(feathers::deviceForTag(mod::tracker::kTraits.tag)->traits.name)) == 1);
	}

	SUBCASE("and choosing a modulator seats it in the lane with its own card, on screen") {
		// THE ADD ROAD, not only the list: the bridge's lane verb takes it (`maySitInLane`) and the lane builds
		// the strip's card for it — an EnvCard for a DAHDSR — rather than a generic or missing one.
		const size_t before = lane->deviceCount();
		size_t index = offered.size();
		for (size_t i = 0; i < offered.size(); ++i) {
			if (feathers::device::sameTag(offered[i]->traits.tag, mod::dahdsr::kTraits.tag)) {
				index = i;
			}
		}
		REQUIRE(index < offered.size());
		const size_t slot = lane->addChoiceForTest(index);
		REQUIRE(slot == before);
		CHECK(feathers::fxHolds(f.bridge.fxChain(), slot, mod::dahdsr::kTraits.tag));
		ui::FxCard* card = lane->cardAt(slot);
		REQUIRE(card != nullptr);
		CHECK(dynamic_cast<ui::EnvCard*>(card) != nullptr);
		CHECK(card->isVisible());
		CHECK(card->height() > 0.0f);
	}
}
TEST_CASE("the fx lane's (+) appends a device that is on screen without being perturbed") {
	// The defect: a new device did not appear until something else (a scroll, say) disturbed the
	// UI. The add reached the chain and nothing placed the
	// card, so the lane went on drawing the layout it had — and a scroll, which calls resized(), was
	// what made the device appear.
	//
	// THE OBSERVABLE HALF IS NOT A REPAINT AND SO IS ASSERTABLE HERE. Whether a picture changed
	// cannot be seen headlessly; whether a frame has been BUILT, sized and made visible can, and on
	// this lane resized() is also what CONSTRUCTS the card — ensureCard runs there — so a (+) that
	// only redrew would leave the new slot with no card at all.
	FilterFixture f;
	auto* lane = f.lane;
	REQUIRE(lane != nullptr);
	const size_t before = lane->deviceCount();
	REQUIRE(before == 1);

	const std::vector<const feathers::device::Descriptor*> offered = lane->addableForTest();
	REQUIRE(offered.size() >= 2);
	// The SECOND offering, not the first, so "it added something" cannot be satisfied by a road that
	// always appends a filter.
	const size_t slot = lane->addChoiceForTest(1);
	REQUIRE(slot == before);

	// NOTHING ELSE HAPPENS BETWEEN THE ADD AND THESE CHECKS — no resized(), no draw(), no scroll.
	CHECK(lane->deviceCount() == before + 1);
	ui::FxCard* card = lane->cardAt(slot);
	REQUIRE(card != nullptr);
	CHECK(card->isVisible());
	CHECK(card->width() > 0.0f);
	CHECK(card->height() > 0.0f);
	// AND WHERE THE LANE SAYS IT IS, rather than at the window origin — the state a child frame sits
	// in when it has been made visible and never placed.
	const visage::Bounds expected = lane->cardBounds(slot);
	CHECK(card->y() == doctest::Approx(expected.y()));
	CHECK(card->x() == doctest::Approx(expected.x()));

	SUBCASE("and the card it built is the device that was chosen") {
		// THE RACK SHIPPED THIS EXACT BUG — adding an MSEG produced a card labelled DAHDSR. The add
		// was right and the drawing was wrong, which reads as the wrong device having
		// been added. Asked of the CHAIN and of the CARD, so the two must agree.
		const char* chosen = offered[1]->traits.tag;
		CHECK(feathers::fxHolds(f.bridge.fxChain(), slot, chosen));
		// HARM is offering[1] today; the claim is that the lane built ITS card and not the other one.
		if (feathers::device::sameTag(chosen, fx::harm::kTraits.tag)) {
			CHECK(dynamic_cast<ui::HarmCard*>(card) != nullptr);
		}
		CHECK(dynamic_cast<ui::MissingDeviceCard*>(card) == nullptr);
		// And the first slot is untouched: an append cannot move what is already there.
		CHECK(lane->cardAt(0) == lane->filterCard());
	}

	SUBCASE("and the (+) moved down to sit under it") {
		// THE BUTTON IS A PICTURE OF WHERE THE NEXT DEVICE GOES, so it has to follow the chain it
		// grows. A (+) that stayed put would be drawn over the card it just made.
		const visage::Bounds add = lane->addBounds();
		CHECK(add.y() >= lane->cardBounds(slot).bottom());
		CHECK(lane->partAt({ add.x() + add.width() * 0.5f, add.y() + add.height() * 0.5f })
		      == ui::FxLane::Part::Add);
		// AND ITS NEIGHBOUR STILL ANSWERS FOR ITSELF, which is what catches a target drawn one row too
		// tall — a (+) that had swallowed the card above it would take every press meant for it.
		const visage::Bounds above = lane->cardBounds(slot);
		CHECK(lane->partAt({ above.x() + above.width() * 0.5f, above.y() + 4.0f })
		      != ui::FxLane::Part::Add);
	}
}
TEST_CASE("the fx lane's (+) is reachable however full the chain is") {
	// THE ROW WITH NOTHING BELOW IT TO HIDE A SHORTFALL. The modulator strip's own recorded bug is a
	// scroll extent that did not know about its (+); it passed a naive full-scroll check because the
	// card below the button was slack enough to absorb the missing pixels. The fx lane's (+) is the
	// LAST row, so there is no slack at all — which makes this both the easier case to get wrong and
	// the easier one to catch.
	FilterFixture f;
	auto* lane = f.lane;
	REQUIRE(lane != nullptr);

	SUBCASE("with an empty chain it sits under the placeholder, not off the top") {
		feathers::FxChain empty;
		empty.count = 0;
		f.bridge.restoreFxChain(empty);
		lane->resized();
		REQUIRE(lane->deviceCount() == 0);

		const visage::Bounds add = lane->addBounds();
		CHECK(add.y() >= lane->emptySlotBounds().bottom());
		CHECK(add.bottom() <= lane->height() + 1.0f);
		CHECK(lane->partAt({ add.x() + 4.0f, add.y() + add.height() * 0.5f })
		      == ui::FxLane::Part::Add);
	}

	SUBCASE("and with the chain full it can still be scrolled to") {
		// FILLED THROUGH THE (+) ITSELF, so this is also the case that says the button keeps working
		// all the way to the bound rather than up to some slack the layout happened to have.
		while (lane->deviceCount() < feathers::kMaxFxSlots) {
			const size_t at = lane->addChoiceForTest(1);
			REQUIRE(at < feathers::kMaxFxSlots);
		}
		CHECK(lane->deviceCount() == feathers::kMaxFxSlots);

		// AND THE NINTH LANDS — the lane grows (unbounded devices step 6); it refused here before. The scroll
		// below is then asked of a lane longer than its starting size, which is the case scrolling is for.
		CHECK(lane->addChoiceForTest(1) == feathers::kMaxFxSlots);
		CHECK(lane->deviceCount() == feathers::kMaxFxSlots + 1);

		// THE CONTENT EXTENT KNOWS ABOUT THE BUTTON. Stated against addBounds rather than against a
		// restated sum, so a change to either moves the claim with it.
		lane->setScroll(lane->maxScroll());
		const visage::Bounds add = lane->addBounds();
		CAPTURE(add.y());
		CAPTURE(add.bottom());
		CAPTURE(lane->height());
		CHECK(add.bottom() <= lane->height() + 1.0f);
		CHECK(add.y() >= -1.0f);
		// And a hand can land on it there.
		CHECK(lane->partAt({ add.x() + 4.0f, add.y() + add.height() * 0.5f })
		      == ui::FxLane::Part::Add);
	}
}
TEST_CASE("removing a device from the lane takes its routings with it") {
	// THE HAZARD THAT MAKES THIS A DIFFERENT GESTURE FROM THE (+), AND IT MOVED. It used to be that a
	// remove shifted every later device down a slot while the table was addressed BY SLOT, so a
	// remove that forgot the table pointed every later routing at its NEIGHBOUR. A device carries its
	// routing LANE in its row now, so nothing shifts and that failure is unwritable.
	//
	// WHAT REPLACED IT IS THE ONE THIS CASE ASSERTS: the survivors' routings must be exactly where
	// they were, at the same addresses, untouched by a removal that happened above them.
	//
	// SO THE ASSERTION IS ABOUT THE ROUTING AND NOT ABOUT THE COUNT. "One fewer device" passes for
	// every version of this bug there is.
	//
	// FILTER, HARM, FILTER — THREE DEVICES OF TWO KINDS, AND THE ARRANGEMENT IS LOAD-BEARING. The
	// first draft of this case used filter, HARM, HARM, and a known-bad that deleted the relayout
	// stayed GREEN: with the same device either side of the hole, "the card below was rebuilt as what
	// it now holds" is true before the rebuild as well as after, and cardAt() answers null past the
	// count whether or not anything moved. Both halves of the claim were satisfied by doing nothing.
	// A probe aimed where the right and wrong answers agree is not a probe.
	FilterFixture f;
	auto* lane = f.lane;
	REQUIRE(lane != nullptr);

	REQUIRE(f.bridge.addFxDevice(fx::harm::kTraits.tag) == 1);
	REQUIRE(f.bridge.addFxDevice(fx::filter::kTraits.tag) == 2);
	lane->resized();
	REQUIRE(lane->deviceCount() == 3);
	REQUIRE(dynamic_cast<ui::HarmCard*>(lane->cardAt(1)) != nullptr);
	REQUIRE(dynamic_cast<ui::FilterCard*>(lane->cardAt(2)) != nullptr);

	const backplane::Source kSource = backplane::Source::core(0); // Macro 1
	constexpr size_t kControl = static_cast<size_t>(fx::filter::Param::Cutoff);
	constexpr float kAmount = 0.375f;

	// THE DEVICE THAT WILL MOVE, NAMED BY ITS SERIAL rather than by where it sits — which is the only
	// identity that survives the operation being tested.
	const feathers::DeviceInstanceId moving = feathers::instanceOfFxSlot(f.bridge.fxChain(), 2);
	REQUIRE(moving != feathers::kNoInstance);
	REQUIRE(moving == f.bridge.fxDeviceAt(2));

	// THROUGH THE DEVICE'S ID, which is what a route names (kill-lanes step 2).
	const backplane::Destination before = backplane::Destination::control(moving, kControl);
	REQUIRE(f.bridge.mayRoute(kSource, before));
	f.bridge.setRouteAmount(kSource, before, kAmount);
	REQUIRE(f.bridge.routeAmount(kSource, before) == doctest::Approx(kAmount));

	// THE DEVICE ABOUT TO GO, and a route of its own so its going has something to take. Its routes
	// must be GONE afterwards: an id is never reissued, so nothing could inherit them the way a reused
	// lane once could, but a route to nobody is a route nothing can hear and the record must not keep one.
	const DeviceInstanceId doomed = f.bridge.fxDeviceAt(1);
	REQUIRE(doomed != kNoInstance);
	const backplane::Destination doomedWidth =
	    backplane::Destination::control(doomed, static_cast<size_t>(fx::harm::Param::Width));
	f.bridge.setRouteAmount(kSource, doomedWidth, kAmount);
	REQUIRE_FALSE(f.bridge.routesOf(doomed).empty());

	// REMOVE THE MIDDLE ONE, through the lane's own verb.
	REQUIRE(lane->removeDeviceAtLane(1));
	CHECK(lane->deviceCount() == 2);

	// THE SAME DEVICE IS NOW IN SLOT 1 — by serial, so this cannot be satisfied by a chain that
	// happens to hold two devices of the same type.
	CHECK(feathers::instanceOfFxSlot(f.bridge.fxChain(), 1) == moving);

	// AND THE ROUTING DID NOT MOVE, WHICH IS THE CLAIM NOW. The device changed places and its address
	// did not, because the address is the device's id. `after == before` is the whole difference
	// between this design and the one it replaced, where a correct remove had to produce a different
	// address.
	const backplane::Destination after = backplane::Destination::control(f.bridge.fxDeviceAt(1), kControl);
	CHECK(after == before);
	CHECK(f.bridge.routeAmount(kSource, after) == doctest::Approx(kAmount));

	// AND THE DEVICE THAT WENT TOOK EVERY ROUTE NAMING IT — asked of the record over every edge rather
	// than over the one control this case routed, because a clear that walked only the destination half
	// would look complete here — no fx device publishes an output — and would leave a rack device's
	// outgoing routes behind.
	CHECK(f.bridge.routesOf(doomed).empty());
	CHECK(f.bridge.routeAmount(kSource, doomedWidth) == 0.0f);

	SUBCASE("and the card below the hole is rebuilt as the device it now holds") {
		// SLOT 1 HELD A HARM AND NOW HOLDS A FILTER. A lane that only redrew would leave HARM's card
		// there — a card editing a row nobody is rendering, with the previous device's controls on it.
		REQUIRE(lane->cardAt(1) != nullptr);
		CHECK(dynamic_cast<ui::FilterCard*>(lane->cardAt(1)) != nullptr);
		CHECK(dynamic_cast<ui::HarmCard*>(lane->cardAt(1)) == nullptr);
		CHECK(lane->cardAt(2) == nullptr); // the vacated slot draws nothing
		CHECK(lane->cardAt(0) == lane->filterCard());
	}

	SUBCASE("and removing the last one empties the lane rather than leaving a ghost") {
		REQUIRE(lane->removeDeviceAtLane(1));
		REQUIRE(lane->removeDeviceAtLane(0));
		CHECK(lane->deviceCount() == 0);
		CHECK(lane->cardAt(0) == nullptr);
		CHECK(lane->filterCard() == nullptr);
		// AND THE (+) IS STILL THERE, which is the way back. A lane a player can empty and cannot
		// refill is a worse trap than one they cannot empty.
		const visage::Bounds add = lane->addBounds();
		CHECK(lane->partAt({ add.x() + 4.0f, add.y() + add.height() * 0.5f })
		      == ui::FxLane::Part::Add);
		CHECK(lane->addChoiceForTest(0) == 0);
	}

	SUBCASE("and a slot that holds nothing refuses rather than corrupting the count") {
		CHECK_FALSE(lane->removeDeviceAtLane(7));
		CHECK_FALSE(lane->removeDeviceAtLane(2));
		CHECK(lane->deviceCount() == 2);
	}
}
TEST_CASE("moving a device in the lane moves its routings with it") {
	// THE ORDER OF AN AUDIO CHAIN IS ITS SOUND, which is why reorder exists — and a move shifts
	// everything between the two ends, so it carries exactly the same routing hazard a remove does.
	FilterFixture f;
	auto* lane = f.lane;
	REQUIRE(lane != nullptr);
	REQUIRE(f.bridge.addFxDevice(fx::harm::kTraits.tag) == 1);
	lane->resized();
	REQUIRE(lane->deviceCount() == 2);

	const backplane::Source kSource = backplane::Source::core(0);
	constexpr size_t kControl = static_cast<size_t>(fx::harm::Param::Width);
	constexpr float kAmount = -0.25f;

	const feathers::DeviceInstanceId harm = feathers::instanceOfFxSlot(f.bridge.fxChain(), 1);
	REQUIRE(harm == f.bridge.fxDeviceAt(1));
	const backplane::Destination before = backplane::Destination::control(harm, kControl);
	REQUIRE(f.bridge.mayRoute(kSource, before));
	f.bridge.setRouteAmount(kSource, before, kAmount);

	// HARM IN FRONT OF THE FILTER — the reorder a player would actually make, since stripping a
	// fundamental before filtering is a different sound from after.
	REQUIRE(lane->moveDevice(1, 0));
	CHECK(feathers::instanceOfFxSlot(f.bridge.fxChain(), 0) == harm);
	CHECK(feathers::fxHolds(f.bridge.fxChain(), 1, fx::filter::kTraits.tag));

	// THE SAME ADDRESS, AND THAT IS THE POINT. HARM moved from slot 1 to slot 0 and its id came with
	// it, so the destination is unchanged and the amount is still under it. Read through the device
	// NOW at slot 0 rather than through the id held above, which is what makes "unchanged" a
	// measurement rather than a tautology.
	const backplane::Destination after = backplane::Destination::control(f.bridge.fxDeviceAt(0), kControl);
	CHECK(after == before);
	CHECK(f.bridge.routeAmount(kSource, after) == doctest::Approx(kAmount));
	// AND THE DEVICE IT SWAPPED WITH DID NOT ACQUIRE IT. The filter now sits at slot 1; nothing of
	// this route may be on it.
	CHECK(f.bridge.fxDeviceAt(1) != f.bridge.fxDeviceAt(0));
	CHECK(f.bridge.routeAmount(kSource, backplane::Destination::control(f.bridge.fxDeviceAt(1), kControl))
	      == doctest::Approx(0.0f));

	// AND THE CARDS FOLLOWED. Slot 0 was the filter's card and must now be HARM's.
	CHECK(dynamic_cast<ui::HarmCard*>(lane->cardAt(0)) != nullptr);
	CHECK(dynamic_cast<ui::FilterCard*>(lane->cardAt(1)) != nullptr);

	SUBCASE("and the menu will not offer a move that has nowhere to go") {
		// DISABLED RATHER THAN ABSENT: a menu whose entries shuffle depending on which card you
		// pressed puts Remove under a different finger every time.
		// BY VERB, NOT BY ROW: the menu gained "Add before" at the top (backlog CR), and a case that
		// counted rows or indexed them would be a case about the menu's shape rather than its answers.
		const auto offered = [lane](size_t slot, ui::FxLane::SlotVerb verb) {
			for (const auto& option : lane->slotOptionsForTest(slot)) {
				if (option.verb == verb) {
					return option.enabled;
				}
			}
			FAIL("verb not on the menu");
			return false;
		};
		CHECK_FALSE(offered(0, ui::FxLane::SlotVerb::MoveUp));   // nothing above the first
		CHECK(offered(0, ui::FxLane::SlotVerb::MoveDown));       // but it can go down
		CHECK(offered(0, ui::FxLane::SlotVerb::Remove));         // and it can go away
		CHECK_FALSE(offered(1, ui::FxLane::SlotVerb::MoveDown)); // nothing below the last
		CHECK(offered(1, ui::FxLane::SlotVerb::MoveUp));
		// AND THE ROWS KEEP THEIR PLACES — the reason they are disabled rather than absent.
		CHECK(lane->slotOptionsForTest(0)[0].verb == ui::FxLane::SlotVerb::AddBefore);
		CHECK(lane->slotOptionsForTest(0).back().verb == ui::FxLane::SlotVerb::Remove);

		// AND THE VERB REFUSES rather than doing something else when it is not offered.
		CHECK_FALSE(lane->slotChoiceForTest(0, ui::FxLane::SlotVerb::MoveUp));
		CHECK(feathers::instanceOfFxSlot(f.bridge.fxChain(), 0) == harm); // unmoved
	}
}
TEST_CASE("every card in the lane carries the lane's verbs where a hand can reach them") {
	// THE VERBS LIVE IN THE CARD'S HEAD AND NOT ON THE LANE, because a card fills the lane's width
	// less its padding — a lane-drawn target would be underneath the card and the child frame would
	// take the press. Ten pixels of lane either side is not a button.
	//
	// SO EVERY CARD HAS TO CARRY IT, and this is the case that says all three do rather than the two
	// somebody remembered. The placeholder is the one most likely to be forgotten and the one that
	// needs it most: it is the card a patch's UNKNOWN device gets, so it is exactly the device a
	// player cannot edit and must still be able to remove.
	FilterFixture f;
	auto* lane = f.lane;
	REQUIRE(lane != nullptr);
	REQUIRE(f.bridge.addFxDevice(fx::harm::kTraits.tag) == 1);
	// A third slot holding a tag this build cannot name.
	// **AT POSITION 2, WHICH IS NOW THE WHOLE ADDRESS (kill-lanes step 3c).** This stamped a `routeLane`
	// and an `order[]` entry beside the row because the array was lane-keyed and the order said where a
	// device was drawn; without the order entry the old channel stored an EMPTY device at position 2 —
	// also a tag nothing knows, so the case passed over the wrong device. The array is dense by position
	// now, so `device[2]` with `count == 3` is the third card and the hazard is gone with the second number.
	feathers::FxChain chain = f.bridge.fxChain();
	chain.device[2].row = feathers::DeviceRow {};
	chain.device[2].row.tag = { 'Z', 'Z', 'Z', 'Z' };
	chain.device[2].row.instanceId = chain.nextInstanceId;
	chain.nextInstanceId = chain.nextInstanceId + 1;
	chain.count = 3;
	f.bridge.restoreFxChain(chain);
	lane->resized();
	REQUIRE(lane->deviceCount() == 3);

	auto* filter = dynamic_cast<ui::FilterCard*>(lane->cardAt(0));
	auto* harm = dynamic_cast<ui::HarmCard*>(lane->cardAt(1));
	auto* unknown = dynamic_cast<ui::MissingDeviceCard*>(lane->cardAt(2));
	REQUIRE(filter != nullptr);
	REQUIRE(harm != nullptr);
	REQUIRE(unknown != nullptr);

	// THE DOTS ARE IN THE SAME PLACE ON ALL THREE, which is what the base class buys: three cards
	// agreeing by hand is how one ends up 4px off and unreachable.
	CHECK(filter->slotMenuBounds().x() == doctest::Approx(harm->slotMenuBounds().x()));
	CHECK(filter->slotMenuBounds().x() == doctest::Approx(unknown->slotMenuBounds().x()));
	CHECK(filter->slotMenuBounds().width() > 0.0f);
	CHECK(filter->slotMenuBounds().right() <= filter->width());

	// AND THE HIT TEST FINDS THEM BEFORE THE HEAD THEY SIT IN. The head's verb is FOLD; a card that
	// tested the head first would make the dots unclickable and the bug would read as "the button
	// does nothing" — the trap FilterCard's enable square already records.
	const auto centre = [](visage::Bounds b) {
		return visage::Point { b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f };
	};
	CHECK(filter->partAt(centre(filter->slotMenuBounds())) == ui::FilterCard::Part::SlotMenu);
	CHECK(harm->partAt(centre(harm->slotMenuBounds())) == ui::HarmCard::Part::SlotMenu);

	// AND THEY DO NOT COLLIDE WITH THE ENABLE SQUARE, which shares that head and is the other
	// irreversible-ish thing a finger can land on. Asked as geometry AND as a hit test, because two
	// rectangles that merely fail to overlap can still be ordered wrongly.
	CHECK(filter->enableBounds().right() <= filter->slotMenuBounds().x());
	CHECK(filter->partAt(centre(filter->enableBounds())) == ui::FilterCard::Part::Enable);

	SUBCASE("and pressing them opens on the slot that was pressed") {
		// THE SLOT IS BOUND WHERE THE CARD IS BUILT, so a card rebuilt after a remove is the face of
		// whatever is there NOW. Driven through the callback rather than the popup, which needs a
		// window.
		size_t opened = feathers::kMaxFxSlots;
		for (size_t slot = 0; slot < 3; ++slot) {
			ui::FxCard* card = lane->cardAt(slot);
			REQUIRE(card != nullptr);
			REQUIRE(static_cast<bool>(card->onSlotMenu)); // every card got one, placeholder included
		}
		// The placeholder's own handler is the one that did not exist before this arc.
		unknown->onSlotMenu = [&] { opened = 2; };
		unknown->onMouseDown().callback(at(centre(unknown->slotMenuBounds()).x,
		                                   centre(unknown->slotMenuBounds()).y));
		CHECK(opened == 2);
	}
}

