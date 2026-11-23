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

// PIECE 6 OF 12 of the dahdsr suite: its cases in their original order, with the helpers only they use. See dahdsr_common.h.

#include "dahdsr/dahdsr_common.h"

namespace {
/// **AN ID NOTHING WEARS** — what "a lane the rack does not hold" became when sources started naming
/// devices (kill-lanes step 2). Ids are issued from 1 and climb one at a time, so no fixture here comes
/// near it; the cases that use it REQUIRE the bridge to hold nothing under it rather than assume so.
constexpr DeviceInstanceId kNobody = 999999;
} // namespace

TEST_CASE("a card can be dragged to reorder it, in both lanes, and the bumper says where") {
	// Cards reorder by drag, not only through 'move earlier' — menu entries alone were judged not
	// enough. The entries stay beside it, because they cost nothing and they are
	// the only reorder a small window or a keyboard can reach.
	//
	// THE ASSERTIONS ARE VALUES, NOT "THE ORDER CHANGED". A drag whose only claim is that something
	// moved passes for an off-by-one — which is the exact bug being fixed one commit earlier, so it
	// would be a strange thing to reintroduce in the case that covers the fix.
	//
	// AND THE GESTURE IS DRIVEN THE WAY THE WIDGET WILL RECEIVE IT: the fx card reports in its OWN
	// coordinates and the lane converts, so a case that made up lane coordinates would be testing its
	// own arithmetic instead of the lane's.

	SUBCASE("the fx lane") {
		FilterFixture f;
		auto* lane = f.lane;
		REQUIRE(lane != nullptr);
		REQUIRE(f.bridge.addFxDevice(fx::harm::kTraits.tag) == 1);
		REQUIRE(f.bridge.addFxDevice(fx::filter::kTraits.tag) == 2);
		lane->resized();
		REQUIRE(lane->deviceCount() == 3);

		std::array<feathers::DeviceInstanceId, 3> order {};
		for (size_t slot = 0; slot < 3; ++slot) {
			order[slot] = feathers::instanceOfFxSlot(f.bridge.fxChain(), slot);
		}

		// A PRESS IS NOT YET A DRAG — below the threshold the head's own verb still wins, which is
		// what keeps fold working.
		const visage::Bounds card = lane->cardBounds(2);
		lane->cardDragForTest(2, ui::FxCard::DragPhase::Begin, { 20.0f, 4.0f });

		// AIM AT THE GAP ABOVE THE FIRST CARD, which is the position no card-shaped target could ask
		// for. Asked of the widget, then expressed back in the dragged card's coordinates.
		const visage::Bounds bumper = lane->gapBounds(0);
		const float inCardY = bumper.y() + bumper.height() * 0.5f - card.y();
		lane->cardDragForTest(2, ui::FxCard::DragPhase::Move, { 20.0f, inCardY });

		REQUIRE(lane->dragSlot().has_value());
		CHECK(*lane->dragSlot() == 2);
		REQUIRE(lane->dropGap().has_value());
		CHECK(*lane->dropGap() == 0);

		lane->cardDragForTest(2, ui::FxCard::DragPhase::End, { 20.0f, inCardY });
		lane->resized();

		// THE VALUE: the last device is now the first, by serial, and the other two moved down one.
		CHECK(feathers::instanceOfFxSlot(f.bridge.fxChain(), 0) == order[2]);
		CHECK(feathers::instanceOfFxSlot(f.bridge.fxChain(), 1) == order[0]);
		CHECK(feathers::instanceOfFxSlot(f.bridge.fxChain(), 2) == order[1]);
		// And the drag state is released, or the next press is still holding this card.
		CHECK_FALSE(lane->dragSlot().has_value());
		CHECK_FALSE(lane->dropGap().has_value());

		// AND IT IS UNDOABLE like any other reorder — the drag reaches the same road the menu does.
		REQUIRE(f.gui.history()->canUndo());
		f.gui.history()->undo();
		for (size_t slot = 0; slot < 3; ++slot) {
			CAPTURE(slot);
			CHECK(feathers::instanceOfFxSlot(f.bridge.fxChain(), slot) == order[slot]);
		}
	}

	SUBCASE("the modulator rack") {
		ModFixture f;
		auto* panel = f.panel;
		REQUIRE(panel != nullptr);
		while (panel->deviceCount() < 3) {
			REQUIRE(panel->addChoiceForTest(0) < feathers::kMaxRackSlots);
		}

		std::array<feathers::DeviceInstanceId, 3> order {};
		for (size_t slot = 0; slot < 3; ++slot) {
			order[slot] = f.bridge.modRack().device[slot].instanceId;
		}

		// THE STRIP OWNS THE WHOLE GESTURE HERE — a press on the head row lands on the strip, because
		// it draws every head itself and hands a card only its body. So this drives the real handlers
		// through the panel rather than through a card callback.
		// SCROLLED SO BOTH ENDS ARE REACHABLE FIRST. The strip is taller than its frame with three
		// devices in it, and the gap below the last card is off the bottom until it is scrolled to —
		// which `dropGapAt` correctly reports as no target at all, because outside the strip is the
		// cancel. The first draft of this case aimed there anyway and failed on its own setup.
		panel->setScroll(panel->maxScroll());
		const visage::Bounds head = panel->envCardBounds(0);
		const visage::Point start { head.x() + 40.0f, head.y() + 6.0f };
		REQUIRE(panel->hitAt(start).part != ui::ModPanel::Part::None); // or the press lands nowhere
		panel->onMouseDown().callback(at(start.x, start.y));
		CHECK_FALSE(panel->dragSlot().has_value()); // a press is not yet a drag

		const visage::Bounds bumper = panel->gapBounds(3);
		const visage::Point end { start.x, bumper.y() + bumper.height() * 0.5f };
		// NOT VACUOUS: the aim has to be inside the strip, or every assertion after it is about a
		// gesture that never reached a target.
		REQUIRE(end.y >= 0.0f);
		REQUIRE(end.y < panel->height());
		panel->onMouseDrag().callback(at(end.x, end.y));

		REQUIRE(panel->dragSlot().has_value());
		CHECK(*panel->dragSlot() == 0);
		REQUIRE(panel->dropGap().has_value());
		// THE GAP BELOW THE LAST CARD — the other position a card-shaped target could not name.
		CHECK(*panel->dropGap() == 3);

		panel->onMouseUp().callback(at(end.x, end.y));

		CHECK(f.bridge.modRack().device[0].instanceId == order[1]);
		CHECK(f.bridge.modRack().device[1].instanceId == order[2]);
		CHECK(f.bridge.modRack().device[2].instanceId == order[0]);
		CHECK_FALSE(panel->dragSlot().has_value());

		REQUIRE(f.gui.history()->canUndo());
		f.gui.history()->undo();
		for (size_t slot = 0; slot < 3; ++slot) {
			CAPTURE(slot);
			CHECK(f.bridge.modRack().device[slot].instanceId == order[slot]);
		}
	}

	SUBCASE("the modulator rack's drag follows its device by id when the rack is reordered or shortened under the hand") {
		ModFixture f;
		auto* panel = f.panel;
		REQUIRE(panel != nullptr);
		while (panel->deviceCount() < 3) {
			REQUIRE(panel->addChoiceForTest(0) < feathers::kMaxRackSlots);
		}
		std::array<feathers::DeviceInstanceId, 3> order {};
		for (size_t slot = 0; slot < 3; ++slot) {
			order[slot] = f.bridge.modRack().device[slot].instanceId;
		}
		panel->setScroll(panel->maxScroll());
		const auto grab = [&](size_t slot) {
			const visage::Bounds head = panel->envCardBounds(slot);
			const visage::Point start { head.x() + 40.0f, head.y() + 6.0f };
			REQUIRE(panel->hitAt(start).part != ui::ModPanel::Part::None);
			panel->onMouseDown().callback(at(start.x, start.y));
			return start;
		};
		// REORDERED UNDER THE HAND: A is grabbed at slot 0 and dropped at the bottom gap, but another edit has moved A to the end
		// meanwhile. The drop is A's (a no-op: it is already last); an index kept across the gesture would have moved B.
		{
			const visage::Point start = grab(0);
			const visage::Bounds bumper = panel->gapBounds(3);
			const visage::Point end { start.x, bumper.y() + bumper.height() * 0.5f };
			REQUIRE(end.y < panel->height());
			panel->onMouseDrag().callback(at(end.x, end.y));
			REQUIRE(panel->dragSlot().has_value());
			REQUIRE(*panel->dragSlot() == 0);
			REQUIRE(f.bridge.moveRackDevice(0, 2));
			CHECK(f.bridge.modRack().device[2].instanceId == order[0]);
			CHECK(panel->dragSlot() == std::optional<size_t>(2)); // found again by id
			const size_t steps = f.gui.history()->length();
			panel->onMouseUp().callback(at(end.x, end.y));
			CHECK(f.bridge.modRack().device[0].instanceId == order[1]);
			CHECK(f.bridge.modRack().device[1].instanceId == order[2]);
			CHECK(f.bridge.modRack().device[2].instanceId == order[0]);
			CHECK(f.gui.history()->length() == steps); // moved nothing, spent nothing
			CHECK_FALSE(panel->dragSlot().has_value());
		}
		// REMOVED UNDER THE HAND: the grabbed device is gone, the drag is cancelled cleanly and nothing else moves.
		{
			const feathers::DeviceInstanceId held = f.bridge.modRack().device[1].instanceId;
			const visage::Point start = grab(1);
			const visage::Bounds bumper = panel->gapBounds(0);
			const visage::Point end { start.x, bumper.y() + bumper.height() * 0.5f };
			panel->onMouseDrag().callback(at(end.x, end.y));
			REQUIRE(panel->dragSlot().has_value());
			REQUIRE(f.bridge.removeRackDevice(1));
			panel->resized();
			CHECK_FALSE(panel->dragSlot().has_value()); // gone, not whoever slid into slot 1
			const std::array<feathers::DeviceInstanceId, 2> left { f.bridge.modRack().device[0].instanceId,
				                                                   f.bridge.modRack().device[1].instanceId };
			const size_t steps = f.gui.history()->length();
			panel->onMouseUp().callback(at(end.x, end.y));
			CHECK(f.bridge.modRack().device[0].instanceId == left[0]);
			CHECK(f.bridge.modRack().device[1].instanceId == left[1]);
			CHECK(f.bridge.modRack().device[0].instanceId != held);
			CHECK(f.bridge.modRack().device[1].instanceId != held);
			CHECK(f.gui.history()->length() == steps);
			CHECK_FALSE(panel->dropGap().has_value());
		}
	}

	SUBCASE("and dropping a card back where it came from moves nothing and spends no undo step") {
		// THE NO-OP, IN A REAL WIDGET. A hand that picks a card up, thinks better of it and lets go
		// must leave the history alone — otherwise the undo stack fills with edits that changed
		// nothing and the next real undo appears to do nothing.
		FilterFixture f;
		auto* lane = f.lane;
		REQUIRE(f.bridge.addFxDevice(fx::harm::kTraits.tag) == 1);
		lane->resized();
		REQUIRE(lane->deviceCount() == 2);
		const feathers::FxChain before = f.bridge.fxChain();
		const size_t steps = f.gui.history()->length();

		// SCROLLED FIRST, for the reason the rack arm records: two real fx cards do not fit in the
		// lane, so the gap below the second one is off the bottom until it is scrolled to — and
		// `dropGapAt` correctly answers "no target" outside the lane. Card bounds are re-read AFTER
		// the scroll, because they move with it.
		lane->setScroll(lane->maxScroll());
		const visage::Bounds card = lane->cardBounds(1);
		lane->cardDragForTest(1, ui::FxCard::DragPhase::Begin, { 20.0f, 4.0f });
		for (const size_t gap : { size_t {1}, size_t {2} }) {
			CAPTURE(gap);
			const visage::Bounds bumper = lane->gapBounds(gap);
			const float inLaneY = bumper.y() + bumper.height() * 0.5f;
			// NOT VACUOUS: the aim has to be inside the lane, or the assertion below is about a
			// gesture that never reached a target.
			REQUIRE(inLaneY >= 0.0f);
			REQUIRE(inLaneY < lane->height());
			lane->cardDragForTest(1, ui::FxCard::DragPhase::Move, { 20.0f, inLaneY - card.y() });
			REQUIRE(lane->dropGap().has_value());
			CHECK(*lane->dropGap() == gap);
		}
		lane->cardDragForTest(1, ui::FxCard::DragPhase::End, { 20.0f, 0.0f });

		CHECK(f.bridge.fxChain() == before);
		CHECK(f.gui.history()->length() == steps);
	}
}
TEST_CASE("a small movement is still a click, so the head's own verbs survive the drag") {
	// THE THRESHOLD, AND A HOLE IN MY OWN CASE ABOVE. That one asserts "a press is not yet a drag" by
	// checking the state straight after the press — which is true whether or not a threshold exists,
	// because a press alone never sets it. A known-bad that deleted the threshold outright left it
	// GREEN. The probe was aimed where the right and wrong answers agree.
	//
	// WHAT ACTUALLY DISTINGUISHES THEM is a drag event that MOVES A LITTLE: with a threshold nothing
	// starts, without one the card is already travelling and the fold under the pointer never fires.
	//
	// AND THIS DRIVES THE REAL HANDLERS. The lane's `cardDragForTest` seam is below the threshold —
	// it is where the card reports TO — so a case using it cannot see the threshold at all. This one
	// presses the card itself.

	SUBCASE("the fx lane, through the card's own handlers") {
		FilterFixture f;
		auto* lane = f.lane;
		REQUIRE(f.bridge.addFxDevice(fx::harm::kTraits.tag) == 1);
		lane->resized();
		ui::FxCard* card = lane->cardAt(1);
		REQUIRE(card != nullptr);

		const float head = ui::FxCard::kSlotHeadHeight * 0.5f;
		// ARMED WITH A DOUBLE-CLICK: a single press is no longer a drag at all.
		card->onMouseDown().callback(doubleAt(20.0f, head));
		// A NUDGE — under the threshold, so nothing is being dragged.
		card->onMouseDrag().callback(at(20.0f, head + ui::FxCard::kDragThreshold * 0.5f));
		CHECK_FALSE(card->cardDragging());
		CHECK_FALSE(lane->dragSlot().has_value());

		// PAST IT, and the card is travelling — through the callback the lane installed, which is the
		// wiring the seam-driven case never exercises.
		card->onMouseDrag().callback(at(20.0f, head + ui::FxCard::kDragThreshold * 3.0f));
		CHECK(card->cardDragging());
		REQUIRE(lane->dragSlot().has_value());
		CHECK(*lane->dragSlot() == 1);

		// AND THE RELEASE LETS GO whether or not it moved anything.
		card->onMouseUp().callback(at(20.0f, head + ui::FxCard::kDragThreshold * 3.0f));
		CHECK_FALSE(card->cardDragging());
		CHECK_FALSE(lane->dragSlot().has_value());
	}

	SUBCASE("the modulator rack, through the strip's own handlers") {
		ModFixture f;
		auto* panel = f.panel;
		while (panel->deviceCount() < 2) {
			REQUIRE(panel->addChoiceForTest(0) < feathers::kMaxRackSlots);
		}
		const visage::Bounds head = panel->envCardBounds(0);
		const visage::Point start { head.x() + 40.0f, head.y() + 6.0f };
		REQUIRE(panel->hitAt(start).part != ui::ModPanel::Part::None);

		panel->onMouseDown().callback(at(start.x, start.y));
		panel->onMouseDrag().callback(
		    at(start.x, start.y + ui::ModPanel::kDragThreshold * 0.5f));
		CHECK_FALSE(panel->dragSlot().has_value());
		CHECK_FALSE(panel->dropGap().has_value());

		panel->onMouseDrag().callback(
		    at(start.x, start.y + ui::ModPanel::kDragThreshold * 3.0f));
		REQUIRE(panel->dragSlot().has_value());
		CHECK(*panel->dragSlot() == 0);

		panel->onMouseUp().callback(at(start.x, start.y));
		CHECK_FALSE(panel->dragSlot().has_value());
	}

	SUBCASE("and a press on the head that never moves still folds the fx card") {
		// THE VERB THE THRESHOLD IS PROTECTING, asserted rather than implied — this is what "still a
		// click" is for.
		FilterFixture f;
		auto* lane = f.lane;
		REQUIRE(f.bridge.addFxDevice(fx::harm::kTraits.tag) == 1);
		lane->resized();
		auto* harm = dynamic_cast<ui::HarmCard*>(lane->cardAt(1));
		REQUIRE(harm != nullptr);
		REQUIRE_FALSE(harm->folded());

		const float head = ui::FxCard::kSlotHeadHeight * 0.5f;
		// TWO PRESSES — the fold is the second click. See `FxCard::foldOnHeadPress`.
		harm->onMouseDown().callback(at(20.0f, head));
		harm->onMouseDown().callback(doubleAt(20.0f, head));
		harm->onMouseUp().callback(at(20.0f, head));
		CHECK(harm->folded());
	}
}
TEST_CASE("folding an fx card shrinks it, moves the cards below it, and shortens the lane") {
	// The defect: collapsing an audio effect hid its UI but did not change the card's size.
	//
	// EXACTLY WHAT HAPPENED, AND IT IS A LAYOUT BUG WEARING A DRAWING BUG'S CLOTHES. A fold hid the
	// controls and called `redraw()` — which marks the frame stale and repaints it AT THE SIZE IT
	// ALREADY HAD. `laneHeight()` answered the folded height from the first instant and nothing ever
	// asked it again.
	//
	// THE RACK ALREADY HAD THIS RIGHT: `setCardMinimised` calls `resized()`, which re-places every
	// card and re-clamps the scroll. The two lanes disagreeing, for the fourth time.
	//
	// AND MY OWN REACHABILITY SWEEP COULD NOT HAVE CAUGHT IT, which is worth saying: it asks whether
	// every card can be scrolled to, and a card that is too TALL is still reachable.
	FilterFixture f;
	auto* lane = f.lane;
	REQUIRE(lane != nullptr);
	REQUIRE(f.bridge.addFxDevice(fx::harm::kTraits.tag) == 1);
	lane->resized();
	REQUIRE(lane->deviceCount() == 2);

	auto* filter = lane->filterCard();
	auto* harm = dynamic_cast<ui::HarmCard*>(lane->cardAt(1));
	REQUIRE(filter != nullptr);
	REQUIRE(harm != nullptr);

	const float openHeight = filter->height();
	const float belowBefore = lane->cardBounds(1).y();
	const float contentBefore = lane->contentHeight();
	REQUIRE(openHeight > ui::FilterCard::foldedHeight());

	// FOLD IT THE WAY A HAND DOES — through the card's own head, not by calling resized() afterwards.
	// The whole bug was that nothing called resized(), so a case that called it would hide the defect.
	filter->setFolded(true);

	// THE CARD ITSELF SHRANK.
	CHECK(filter->laneHeight() == doctest::Approx(ui::FilterCard::foldedHeight()));
	CHECK(filter->height() == doctest::Approx(ui::FilterCard::foldedHeight()));
	// THE CARD BELOW MOVED UP by what the fold saved.
	CHECK(lane->cardBounds(1).y() < belowBefore);
	CHECK(harm->y() == doctest::Approx(lane->cardBounds(1).y()));
	// AND THE LANE NEEDS LESS SCROLL, which is the half that ties this to the scroll road: a lane of
	// folded cards must not demand the extent an open one did.
	CHECK(lane->contentHeight() < contentBefore);

	SUBCASE("and unfolding puts it all back") {
		filter->setFolded(false);
		CHECK(filter->height() == doctest::Approx(openHeight));
		CHECK(lane->cardBounds(1).y() == doctest::Approx(belowBefore));
		CHECK(lane->contentHeight() == doctest::Approx(contentBefore));
	}

	SUBCASE("and with everything folded the lane stops needing to scroll at all") {
		// THE END-TO-END OF IT. Two open fx cards do not fit — that is why the lane scrolls — and two
		// folded ones are two head rows, which comfortably do.
		harm->setFolded(true);
		CHECK(lane->maxScroll() == doctest::Approx(0.0f));
		CHECK(lane->cardBounds(1).bottom() <= lane->height());
	}
}
TEST_CASE("a folded card's label does not print over the buttons in its head") {
	// The defect: on a collapsed filter, the mode label ('lowpass', etc.) moved into the header and
	// overlapped the active button.
	//
	// IT DID, AND ON BOTH CARDS. The label's box ran to `width() - kPad - 20` on the filter and to
	// `width() - kPad` on HARM, while the enable square starts further left than either — so a
	// right-aligned label printed straight across it. HARM's was worse because its box reached the
	// full padding and crossed the slot menu too.
	//
	// I MADE HARM'S WORSE MYSELF: the square and the dots are both mine, added after that label was
	// written, and neither moved it. A head that grows occupants without moving the text under them
	// is the shape to watch for.
	FilterFixture f;
	auto* lane = f.lane;
	REQUIRE(f.bridge.addFxDevice(fx::harm::kTraits.tag) == 1);
	lane->resized();
	auto* filter = lane->filterCard();
	auto* harm = dynamic_cast<ui::HarmCard*>(lane->cardAt(1));
	REQUIRE(filter != nullptr);
	REQUIRE(harm != nullptr);

	filter->setFolded(true);
	harm->setFolded(true);

	// THE LABEL'S BOX ENDS BEFORE THE LEFTMOST BUTTON — asserted as rectangles, which is the same
	// vocabulary the slot menu's non-overlap uses. Here the geometry was not merely ugly, it was
	// wrong, so the assertion has an easy job.
	const visage::Bounds fLabel = filter->foldedLabelBounds(filter->enableBounds().x());
	CHECK(fLabel.right() <= filter->enableBounds().x());
	CHECK(fLabel.right() <= filter->slotMenuBounds().x());
	CHECK(fLabel.width() > 0.0f); // and there is still room to write in

	const visage::Bounds hLabel = harm->foldedLabelBounds(harm->enableBounds().x());
	CHECK(hLabel.right() <= harm->enableBounds().x());
	CHECK(hLabel.right() <= harm->slotMenuBounds().x());
	CHECK(hLabel.width() > 0.0f);

	SUBCASE("and the head's buttons are still reachable while folded") {
		// THE OTHER HALF OF THE SAME COMPLAINT: it was called the active button, so what was
		// reaching for was the switch. A fold must leave it pressable — it is the only way back.
		const auto centre = [](visage::Bounds b) {
			return visage::Point { b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f };
		};
		CHECK(filter->partAt(centre(filter->enableBounds())) == ui::FilterCard::Part::Enable);
		CHECK(filter->partAt(centre(filter->slotMenuBounds())) == ui::FilterCard::Part::SlotMenu);
		CHECK(harm->partAt(centre(harm->enableBounds())) == ui::HarmCard::Part::Enable);
		CHECK(harm->partAt(centre(harm->slotMenuBounds())) == ui::HarmCard::Part::SlotMenu);
	}

	SUBCASE("and the label region tracks the buttons rather than a constant") {
		// DERIVED, NOT GUESSED. A fourth occupant in the head should move the text, not hide under it.
		const float moved = filter->enableBounds().x() - 40.0f;
		CHECK(filter->foldedLabelBounds(moved).right() <= moved);
		CHECK(filter->foldedLabelBounds(moved).right() < fLabel.right());
	}
}
TEST_CASE("a harmonic reads as an interval a musician would say, beside the ratio it really is") {
	// The dropdown carries helpful interval names beside each ratio (e.g. 1 oct, perfect 5th).
	//
	// BOTH, ALWAYS. The ratio is what the DSP multiplies by; the name is what the hand is reaching
	// for. A name that replaced the ratio would hide the exact thing, and a ratio alone is the
	// unhelpful display this replaced.
	namespace hrm = feathers::fx::harm;

	SUBCASE("the intervals this table really contains are named exactly") {
		// EXACT INTEGER MATCHES, not a tolerance — every one of these IS the just interval.
		CHECK(hrm::harmonicLabel(hrm::kUnisonHarmonic) == "1:1  ·  unison");
		CHECK(hrm::harmonicLabel(14) == "3:2  ·  perfect 5th");
		CHECK(hrm::harmonicLabel(12) == "4:3  ·  perfect 4th");
		CHECK(hrm::harmonicLabel(11) == "5:4  ·  major 3rd");
		CHECK(hrm::harmonicLabel(19) == "2:1  ·  1 oct");
		CHECK(hrm::harmonicLabel(39) == "4:1  ·  2 oct");
		CHECK(hrm::harmonicLabel(29) == "3:1  ·  1 oct + perfect 5th");
	}

	SUBCASE("and a ratio that names NO interval honestly shows the ratio alone") {
		// THE HALF THAT MATTERS MOST. 11:8 is 551 cents — 51 from a perfect fourth and 49 from a
		// tritone. Calling it either would not be colour text, it would be a number a player tunes
		// against and finds wrong. So it says nothing extra.
		CHECK(hrm::harmonicLabel(13) == "11:8");
		CHECK(hrm::harmonicLabel(15) == "13:8");
		// AND THE CENTS PROVE IT RATHER THAN THE ASSERTION ASSUMING IT — restated here from the
		// ratio, so this stays true if the table ever moves.
		const double cents = 1200.0 * std::log2(static_cast<double>(hrm::kHarmonics[13].ratio));
		CHECK(std::abs(cents - 500.0) > 40.0);
		CHECK(std::abs(cents - 600.0) > 40.0);
	}

	SUBCASE("a descending ratio is named by its reciprocal, which is how anyone says it") {
		// FOLDING A DESCENDING RATIO UPWARD IS EXACT AND UNREADABLE: 1:3 comes out as "2 oct down +
		// perfect 4th" (-2400 + 498 = -1902), which is correct and not what a musician says.
		CHECK(hrm::harmonicLabel(2) == "1:3  ·  1 oct + perfect 5th down");
		CHECK(hrm::harmonicLabel(6) == "2:3  ·  perfect 5th down");
		CHECK(hrm::harmonicLabel(4) == "1:2  ·  1 oct down");
		// AND IT NAMES ONES THE TABLE HAS NO ROW FOR. 5:8's reciprocal is 8:5, a plain minor sixth
		// with no entry of its own — the first version searched the table and lost this.
		CHECK(hrm::harmonicLabel(5) == "5:8  ·  minor 6th down");
	}

	SUBCASE("every entry says its ratio, whatever else it says") {
		// THE INVARIANT ACROSS ALL 102, so a naming bug cannot quietly drop the exact number.
		for (size_t i = 0; i < hrm::kHarmonicCount; ++i) {
			CAPTURE(i);
			const std::string want = std::to_string(static_cast<int>(hrm::kHarmonics[i].num)) + ":"
			                         + std::to_string(static_cast<int>(hrm::kHarmonics[i].den));
			CHECK(hrm::harmonicLabel(i).rfind(want, 0) == 0);
		}
		// AND ENOUGH OF THEM ARE NAMED TO BE WORTH DOING — a bound, stated loosely, so it catches a
		// matcher that stopped matching without pinning a number nobody chose.
		size_t named = 0;
		for (size_t i = 0; i < hrm::kHarmonicCount; ++i) {
			named += hrm::harmonicLabel(i).find("·") != std::string::npos ? 1 : 0;
		}
		CAPTURE(named);
		CHECK(named > hrm::kHarmonicCount / 4);
	}

	SUBCASE("and the card shows the label, with the list and the arrows both reaching it") {
		HarmFixture f;
		CHECK(f.expanded().harmonicRow()->label() == hrm::harmonicLabel(hrm::kUnisonHarmonic));
		// THE RATIO BOX SAYS THE BARE RATIO, which is what the folded row has room for.
		CHECK(f.expanded().harmonicRow()->ratioText() == "1:1");

		// THE RATIO BOX IS A LIST, and the arrows still step — it was found the arrows and wanted the
		// dropdown as well, not instead. Asked of the ROW now, which is where those parts live since
		// both surfaces build the same widget.
		ui::HarmonicRow* row = f.expanded().harmonicRow();
		REQUIRE(row != nullptr);
		const visage::Bounds box = row->boxBounds();
		CHECK(row->partAt({ box.x() + box.width() * 0.5f, box.y() + box.height() * 0.5f })
		      == ui::HarmonicRow::Part::Menu);
		CHECK(row->partAt({ row->prevArrowBounds().x() + 2.0f, row->prevArrowBounds().y() + 8.0f })
		      == ui::HarmonicRow::Part::Prev);

		f.pressRow(f.rowNext());
		CHECK(f.expanded().harmonicRow()->label() == hrm::harmonicLabel(hrm::kUnisonHarmonic + 1));
	}
}
TEST_CASE("HARM's card holds the selected tab's frame over the master row") {
	// THE CARD IS A TABBED PANEL OVER THE DEVICE'S MASTERS. The frame under the tabs holds the selected partial's Active chip,
	// harmonic box and Level; below it, outside it, the masters. Counted from the declared list, never a literal.
	HarmFixture f;
	const auto& knobs = f.card->knobsForTest();

	REQUIRE(knobs.size() == ui::HarmCard::kCardKnobCount);
	std::set<int> onCard;
	for (const auto& entry : ui::HarmCard::kCardKnobs) {
		onCard.insert(static_cast<int>(entry.param));
	}
	CHECK(onCard.count(static_cast<int>(fx::harm::Param::MasterLevel)) == 1);
	CHECK(onCard.count(static_cast<int>(fx::harm::Param::MasterDepth)) == 1);
	CHECK(onCard.count(static_cast<int>(fx::harm::Param::Attack)) == 1);
	CHECK(onCard.count(static_cast<int>(fx::harm::Param::Release)) == 1);
	CHECK(onCard.size() == ui::HarmCard::kCardKnobCount);

	SUBCASE("the second row of knobs and the two switch chips are gone from the card") {
		// THE REMOVED CONTROLS: the envelope's other four, Poly and Env Depth. Absent from the card, present in the window.
		const fx::harm::Param removed[] = { fx::harm::Param::Delay, fx::harm::Param::Hold, fx::harm::Param::Decay,
			                                fx::harm::Param::Sustain, fx::harm::Param::Poly,
			                                fx::harm::Param::DepthFollowsEnv };
		const std::vector<fx::harm::Param> onTheCard = f.card->addressedParamsForTest();
		const std::vector<fx::harm::Param> inTheWindow = f.expanded().addressedParamsForTest();
		for (const fx::harm::Param p : removed) {
			CAPTURE(std::string(fx::harm::kParamDescs[static_cast<size_t>(p)].name));
			CHECK(std::find(onTheCard.begin(), onTheCard.end(), p) == onTheCard.end());
			CHECK(std::find(inTheWindow.begin(), inTheWindow.end(), p) != inTheWindow.end());
		}
		// And no widget of the card's tree carries one: a knob or chip left behind would be a control the list does not admit.
		for (const auto& k : knobs) {
			CAPTURE(std::string(k->label()));
			CHECK(k->label() != std::string("Delay"));
			CHECK(k->label() != std::string("Hold"));
			CHECK(k->label() != std::string("Decay"));
			CHECK(k->label() != std::string("Sustain"));
		}
	}

	SUBCASE("one row of four masters, below the frame and outside it, every knob inside the card") {
		std::set<int> rows;
		const visage::Bounds frame = f.card->frameBounds();
		for (const auto& k : knobs) {
			rows.insert(static_cast<int>(std::lround(k->y())));
		}
		CHECK(rows.size() == 1);
		for (const auto& k : knobs) {
			CAPTURE(std::string(k->label()));
			CHECK(k->y() >= frame.bottom());
			CHECK(k->x() >= 0.0f);
			CHECK(k->x() + k->width() <= f.card->width() + 1.0f);
			CHECK(k->y() + k->height() <= f.card->height() + 1.0f);
			CHECK(k->width() == doctest::Approx(knobs.front()->width()));
		}
	}

	SUBCASE("the frame is attached to the tab row and holds the selected partial's three controls") {
		const visage::Bounds frame = f.card->frameBounds();
		for (int k = 0; k < fx::harm::kPartialCount; ++k) {
			CHECK(f.card->tabBounds(k).bottom() == doctest::Approx(frame.y()));
		}
		for (int k = 0; k < fx::harm::kPartialCount; ++k) {
			const bool shown = k == f.card->selectedPartial();
			CAPTURE(k);
			CHECK(f.card->activeChipForTest(k)->isVisible() == shown);
			CHECK(f.card->harmonicRowForTest(k)->isVisible() == shown);
			CHECK(f.card->levelPillForTest(k)->isVisible() == shown);
			const visage::Bounds b[] = {
				{ f.card->activeChipForTest(k)->x(), f.card->activeChipForTest(k)->y(), f.card->activeChipForTest(k)->width(),
				  f.card->activeChipForTest(k)->height() },
				{ f.card->harmonicRowForTest(k)->x(), f.card->harmonicRowForTest(k)->y(), f.card->harmonicRowForTest(k)->width(),
				  f.card->harmonicRowForTest(k)->height() },
				{ f.card->levelPillForTest(k)->x(), f.card->levelPillForTest(k)->y(), f.card->levelPillForTest(k)->width(),
				  f.card->levelPillForTest(k)->height() },
			};
			for (const visage::Bounds& c : b) {
				CHECK(c.x() >= frame.x());
				CHECK(c.right() <= frame.right() + 0.5f);
				CHECK(c.y() >= frame.y());
				CHECK(c.bottom() <= frame.bottom() + 0.5f);
			}
		}
	}

	SUBCASE("every caption is whole: the cell is wider than the knob by the margin the grid was chosen for") {
		REQUIRE_FALSE(knobs.empty());
		const float cell = knobs.front()->width();
		CAPTURE(cell);
		CHECK(cell > ui::HarmCard::kKnobDiameter * 1.4f);
	}

	SUBCASE("the card's height is the layout's, and the layout is the lane's") {
		CHECK(f.card->height() == doctest::Approx(f.card->laneHeight()));
		CHECK(f.card->laneHeight() == doctest::Approx(ui::HarmCard::openHeight()));
	}

	SUBCASE("nothing became unreachable: every control the card offers the window offers too") {
		std::set<int> reachable;
		for (const fx::harm::Param p : f.expanded().addressedParamsForTest()) {
			reachable.insert(static_cast<int>(p));
		}
		for (const fx::harm::Param p : f.card->addressedParamsForTest()) {
			if (harmSlotFurniture(static_cast<size_t>(p)) || p == fx::harm::Param::Enabled) {
				continue; // the head square is the window header's chip, as it always was
			}
			CAPTURE(std::string(fx::harm::kParamDescs[static_cast<size_t>(p)].name));
			CHECK(reachable.count(static_cast<int>(p)) == 1);
		}
	}
}
TEST_CASE("HARM's card frame follows the tab: its chip, harmonic and level are the selected partial's own") {
	// VALUE-LEVEL. On each tab the ONE visible set is driven through its own widget and the device's parameter for THAT partial is
	// read back, with the other three partials' parameters asserted unmoved. Bound to partial 0 whatever the tab, the first
	// assertion would fail on tab 2 (partial 2 never moves).
	HarmFixture f;
	const auto visible = [&](auto get) {
		int found = -1;
		int count = 0;
		for (int k = 0; k < fx::harm::kPartialCount; ++k) {
			if (get(k)->isVisible()) {
				found = k;
				++count;
			}
		}
		REQUIRE(count == 1);
		return found;
	};
	const auto pressTab = [&](int tab) {
		const visage::Bounds b = f.card->tabBounds(tab);
		f.card->onMouseDown().callback(at(b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f));
	};
	using fx::harm::PartialField;
	using fx::harm::partialParam;

	for (const int tab : { 2, 1, 3, 0 }) {
		CAPTURE(tab);
		pressTab(tab);
		REQUIRE(f.card->selectedPartial() == tab);

		double before[fx::harm::kPartialCount][3];
		for (int k = 0; k < fx::harm::kPartialCount; ++k) {
			before[k][0] = f.param(partialParam(k, PartialField::Level));
			before[k][1] = f.param(partialParam(k, PartialField::Harmonic));
			before[k][2] = f.param(partialParam(k, PartialField::Active));
		}

		// LEVEL: the visible pill's own amount, moved to a value that differs from the default.
		const int levelAt = visible([&](int k) { return f.card->levelPillForTest(k); });
		CHECK(levelAt == tab);
		device::Modulatable& level = f.card->levelPillForTest(levelAt)->amountForTest();
		const double target = before[tab][0] > 0.5 ? 0.25 : 0.75;
		level.setValue(target);
		CHECK(f.param(partialParam(tab, PartialField::Level)) == doctest::Approx(target).epsilon(0.02));

		// HARMONIC: one step up on the visible row.
		const int rowAt = visible([&](int k) { return f.card->harmonicRowForTest(k); });
		CHECK(rowAt == tab);
		f.card->harmonicRowForTest(rowAt)->step(1);
		CHECK(f.param(partialParam(tab, PartialField::Harmonic)) == doctest::Approx(before[tab][1] + 1.0));

		// ACTIVE: the visible chip flips that partial's switch.
		const int chipAt = visible([&](int k) { return f.card->activeChipForTest(k); });
		CHECK(chipAt == tab);
		f.card->activeChipForTest(chipAt)->toggleForTest();
		CHECK(f.param(partialParam(tab, PartialField::Active)) == doctest::Approx(before[tab][2] > 0.5 ? 0.0 : 1.0));

		// AND NOBODY ELSE MOVED.
		for (int k = 0; k < fx::harm::kPartialCount; ++k) {
			if (k == tab) {
				continue;
			}
			CAPTURE(k);
			CHECK(f.param(partialParam(k, PartialField::Level)) == doctest::Approx(before[k][0]));
			CHECK(f.param(partialParam(k, PartialField::Harmonic)) == doctest::Approx(before[k][1]));
			CHECK(f.param(partialParam(k, PartialField::Active)) == doctest::Approx(before[k][2]));
		}
	}
}
TEST_CASE("the generic card answers which device it is the face of") {
	// THE SEAM THE SHARED HEAD WILL ASK THROUGH, landed before the head exists because it is the only
	// part of that change payable in advance — it alters nothing until something asks.
	//
	// WHY IT NEEDS A CASE AT ALL, given it changes no behaviour today: after the head unification the
	// LANE draws one head for every device and derives the name and the enable square from
	// `descriptor()`. FilterCard and HarmCard each return their own `kDescriptor`; this card is
	// written for ALL devices and must return the one it was built over. **Omit the override and
	// every device using the generic card silently loses its head name and its enable square** — and
	// it would read as a defect in the shared head rather than a missing line here.
	//
	// ASKED OF OWLTT, which is the device that will actually use this card, rather than of a fixture
	// device that could differ from the real case.
	FilterFixture f;
	ui::WidgetRegistry registry;
	namespace dt = feathers::fx::owltt;

	feathers::BridgeDeviceHost host { f.bridge };
	ui::GenericCard card { host, f.bridge.fxDeviceAt(0), dt::kDescriptor, nullptr };

	// THE DESCRIPTOR IT WAS BUILT OVER, by identity and not by value — two devices could compare
	// equal field-by-field and still be the wrong one.
	CHECK(card.descriptor() == &dt::kDescriptor);

	// AND THE ENABLE SQUARE RESOLVES THROUGH IT. This is the consumer that exists today, so it is the
	// one that proves the seam is connected rather than merely present.
	REQUIRE(dt::kDescriptor.traits.enableControl >= 0);
	CHECK(card.enableControl() == static_cast<size_t>(dt::kDescriptor.traits.enableControl));
	CHECK(card.hasEnable());
}
TEST_CASE("the generic card renders every declared control as a knob, addressed by index") {
	// THE FALLBACK device.h PROMISE: a device that declines to draw itself (hasCard=false) gets a
	// generic knob-per-row card built by walking its descriptor. That promise was empty until now —
	// every device said true and no code branched on the field. GenericCard is the branch.
	//
	// THIS CASE uses HARM's descriptor as the exercise device without flipping HARM's own hasCard: a
	// GenericCard built over any descriptor must render one knob per param, labelled by the descriptor's
	// own name, addressed by the control's index in the descriptor's param list.
	FilterFixture f;
	ui::WidgetRegistry registry;
	namespace hrm = feathers::fx::harm;

	// BUILT THE WAY THE LANE BUILDS IT — over a real descriptor, in a slot, with the registry the lane
	// would pass. The card does not own the descriptor, so the descriptor must outlive it; HARM's is
	// constexpr and static, which is the common case.
	feathers::BridgeDeviceHost host { f.bridge };
	// OVER AN ID THE HOST HOLDS NOTHING UNDER — the card is asked about its DECLARATIONS, which it
	// takes from `desc`, and every read of a device nothing holds answers the declared default.
	ui::GenericCard card { host, kNoInstance, hrm::kDescriptor, nullptr };

	// ONE KNOB PER DECLARED PARAM, COUNTED FROM THE DECLARATION.
	//
	// THIS LINE USED TO HAVE A LITERAL BESIDE IT — `CHECK(size() == 11)` — and it went red WITHIN THE
	// HOUR, when HARM gained its Depth control and declared twelve. The count was never the property:
	// a device growing a knob is a legitimate change and a test that must be edited every time the
	// thing it guards changes is a change-detector, not a guard. The `std::size` form states what is
	// actually being claimed — the card renders one knob per declared control, whatever that number is.
	//
	// WHAT REPLACES THE LITERAL IS A NON-VACUITY GUARD, not another number. Comparing a count against
	// its own source passes vacuously if both are zero, so this says the fixture is real without
	// pinning a value that is allowed to move.
	REQUIRE(std::size(hrm::kParamDescs) > 1);
	// **ONE KNOB PER DECLARED CONTROL, MINUS THE ENABLE.** A generic Filter in the rack listed `FILTER
	// ENABLED / On` as a knob while the active switch sat in its head — one field with two controls,
	// which a hand can leave disagreeing with itself. The head's switch is the one that stays, so the
	// list is short by exactly the device's `enableControl` when it declares one.
	//
	// STILL NOT A LITERAL. The note above records why: a count beside a literal went red within the
	// hour when HARM gained a control. This states the RELATIONSHIP, which is what the card promises.
	REQUIRE(hrm::kTraits.enableControl >= 0);
	// **ONE WIDGET PER CONTROL, AND NOT ALL OF THEM ARE KNOBS SINCE 2026-09-10.** A stepped `Unit::Choice`
	// with names becomes a `ValueBox` dropdown — the collapse rule is a dropdown rather than a knob, the
	// general form of which `40506c9` had already established. So the count that states
	// the card's promise is the CONTROLS it rendered, not the knobs among them.
	REQUIRE(card.paramIndicesForTest().size() == std::size(hrm::kParamDescs) - 1);

	// EACH KNOB LABELLED BY THE DESCRIPTOR'S OWN NAME — not a host name, not a guess. Indexed through
	// `paramIndicesForTest` rather than by position, because the list and the descriptor no longer
	// line up: HARM declares `Enabled` FIRST, so knob 0 is descriptor param 1.
	// **THROUGH THE KNOB-ONLY INDEX LIST**, which is not `paramIndicesForTest` any more: that one holds
	// every control in declaration order and `knobs_` skips the ones that became dropdowns, so indexing
	// one by the other labels a knob with its neighbour's name the first time a device declares a
	// Choice before a dial.
	//
	// **AND THE CAPTION IS THE DECLARED NAME MINUS THE DEVICE'S OWN, since 2026-09-20.** A descriptor
	// name is required to be device-qualified for the HOST PARAMETER TABLE, which needs distinct
	// names across every device; on a card already titled `Harm` that qualification is six characters
	// of every row spent repeating the title. So `GenericCard` strips a leading `<device name> ` when
	// it captions, and the descriptor keeps its obligation to the table intact.
	//
	// **THE EXPECTATION IS RESTATED INDEPENDENTLY RATHER THAN COMPUTED BY THE CODE UNDER TEST.** The
	// obvious rewrite here is to call the card's own stripper and compare — which passes whatever
	// that function does, including nothing at all. These names are known: HARM declares
	// `Harm Harmonic` and `Harm Level`, so the captions are `Harmonic` and `Level`, written out.
	REQUIRE(card.knobsForTest().size() >= 2);
	const char* const kExpected[] = { "Harmonic", "Level" };
	for (size_t i = 0; i < 2; ++i) {
		const size_t param = card.knobParamIndicesForTest()[i];
		const std::string declared = hrm::kParamDescs[param].name;
		CAPTURE(declared);
		CHECK(card.knobsForTest()[i]->label() == std::string(kExpected[i]));
		// NON-VACUITY: the declared name really did carry the prefix, or the check above is passing
		// because the two happened to agree rather than because anything was stripped.
		CHECK(declared == std::string("Harm ") + kExpected[i]);
	}

	// ADDRESSED BY INDEX IN THE DESCRIPTOR — device.h's permanent rule. The param index each knob
	// corresponds to matches its position in the knob list.
	// AND THE ENABLE IS NOT AMONG THEM — the claim stated directly, rather than inferred from a count
	// that could also be short for the wrong reason.
	for (const size_t param : card.paramIndicesForTest()) {
		CHECK(param != static_cast<size_t>(hrm::kTraits.enableControl));
	}

	// THE CARD HAS A HEIGHT — open shows all knobs, folded is the head alone.
	const float open = card.openHeight();
	const float folded = card.foldedHeight();
	CHECK(open > folded);
	CHECK(open > ui::GenericCard::kHeadHeight);

	// FOLDING HIDES THE KNOBS AND SHRINKS THE CARD. Same contract as every other card in the lane.
	//
	// AND THE KNOBS, NOT ONLY THE HEIGHT — THE COMMENT ABOVE MADE A CLAIM THE ASSERTIONS DID NOT.
	// MEASURED: `knob->setVisible(!folded_)` in `GenericCard::setFolded` could be forced open with
	// all 473 cases green. The height lines below are a different fact and pass either way, which is
	// how a card can shrink to its head with every knob still on.
	//
	// THE SEVENTH CARD. The same mutation is caught for harm, shaper, drawn, owltt, filter and
	// waypoint — six siblings, each by a case of its own — and this is the one the fallback builds
	// for a device that has no card, which is to say the one nobody was looking at.
	//
	// EVERY KNOB, COUNTED FROM THE CARD rather than spot-checked: `setFolded` walks a container, and
	// a loop that missed its last element would leave one knob on and satisfy any single probe.
	REQUIRE_FALSE(card.knobsForTest().empty());
	for (const auto& knob : card.knobsForTest()) {
		REQUIRE(knob->isVisible()); // the premise: they are on before the fold turns them off
	}
	card.setFolded(true);
	CHECK(card.laneHeight() == card.foldedHeight());
	for (const auto& knob : card.knobsForTest()) {
		CHECK_FALSE(knob->isVisible());
	}
	card.setFolded(false);
	CHECK(card.laneHeight() == card.openHeight());
	// AND BACK ON AGAIN, because `setFolded` writes the flag in both directions and a case that only
	// closed it would pass on a card that could never be reopened.
	for (const auto& knob : card.knobsForTest()) {
		CHECK(knob->isVisible());
	}

	// THE CARD DRAWS ITS DEVICE NAME IN THE HEAD — the descriptor's own traits.name.
	// (Asserted indirectly: construction with a valid descriptor and a successful openHeight() call
	// mean the name was read without throwing. The draw path is exercised by the paint cases below.)
}
TEST_CASE("the lane routes unknown devices to MissingDeviceCard and known ones without cards to GenericCard") {
	// THE TWO BRANCHES OF THE FALLBACK. ensureCard's else branch has two cases:
	//   - desc != nullptr: a device this build has heard of but has no specific card for → GenericCard
	//   - desc == nullptr: a device this build does not have → MissingDeviceCard ("device missing")
	// This case asserts the nullptr branch, which a hasCard-keyed branch cannot serve.
	FilterFixture f;

	// A TAG THIS BUILD HAS NEVER HEARD OF — written directly into the chain, the way a saved chain
	// from a newer build would arrive. addFxDevice refuses unknown tags, so we bypass it and write
	// the row ourselves, exactly as the chain deserialization would.
	// AT THE POSITION PAST THE END, WHICH IS THE WHOLE ADDRESS (kill-lanes step 3c) — see "every card in
	// the lane carries the lane's verbs". A `routeLane` and an `order[]` entry stood here because the
	// array was lane-keyed; without the order entry the old channel drew an empty device here and this
	// case passed over the wrong one. Dense by position now, so appending IS writing `device[count]`.
	feathers::FxChain chain = f.bridge.fxChain();
	const size_t slot = chain.count;
	chain.device[slot].row = feathers::DeviceRow {};
	chain.device[slot].row.tag = { 'Z', 'Z', 'Z', 'Z' };
	chain.device[slot].row.instanceId = chain.nextInstanceId;
	chain.nextInstanceId = chain.nextInstanceId + 1;
	chain.count = slot + 1;
	f.bridge.restoreFxChain(chain);
	f.lane->resized();

	// THE UNKNOWN DEVICE GETS NoEditorCard, NOT GenericCard — proving the nullptr arm works.
	REQUIRE(f.lane->cardAt(slot) != nullptr);
	CHECK(dynamic_cast<ui::MissingDeviceCard*>(f.lane->cardAt(slot)) != nullptr);
	CHECK(dynamic_cast<ui::GenericCard*>(f.lane->cardAt(slot)) == nullptr);
}
TEST_CASE("the enable square is declared by the device, not assumed by the lane") {
	// THE SHARED HEAD CAN ONLY CARRY WHAT EVERY DEVICE HAS, and an enable is not that: every fx
	// device declares exactly one, no modulator declares any. So the square is conditional, and the
	// 2026-08-24 decision is explicit that the condition must come from the DESCRIPTOR — a branch on
	// which lane we happen to be in would be right today and wrong the first time a modulator wants
	// switching off.
	//
	// AN INDEX AND NOT A BOOL, for `perVoiceControl`'s reason: a control is addressed by its position
	// in `params`. A `hasEnable` flag would be a claim ABOUT the params that can disagree with them.

	SUBCASE("every device declares one now, audio and modulator alike") {
		// THE INVARIANT ACROSS THE WHOLE REGISTRY, from declarations on both sides — so it is the RULE
		// being asserted rather than a list of today's four devices.
		for (const feathers::device::Descriptor& d : feathers::kDevices) {
			CAPTURE(std::string(d.traits.name));
			// **MODULATORS DECLARE ONE TOO NOW.** This read `== maySitInAudioChain(d)` — audio devices
			// have a switch and modulators do not. Mod card headers get the same unified treatment,
			// disable button included, and the missing square was exactly this
			// declaration being absent rather than a card defect. The head draws a switch iff the
			// descriptor declares one, so declaring it IS the feature.
			CHECK(d.traits.enableControl >= 0);

			if (d.traits.enableControl < 0) {
				continue;
			}
			// AND IT NAMES A REAL SWITCH — in range, stepped, and a Choice. An index that named a
			// continuous control would draw a square that set a filter cutoff to 1.
			const auto at = static_cast<size_t>(d.traits.enableControl);
			REQUIRE(at < d.params.size());
			CHECK(d.params[at].stepped);
			CHECK(d.params[at].unit == feathers::device::Unit::Choice);
			CHECK(d.params[at].min == doctest::Approx(0.0));
			CHECK(d.params[at].max == doctest::Approx(1.0));
			// **AND THE DEFAULT IS ON FOR EVERY DEVICE, AUDIO AND MODULATOR ALIKE** (2026-09-04): every
			// effect defaults to on when it is added (some, including Shaper, had defaulted off).
			// Extended the same day to the mod rack: **modulators default on as well**, both lanes, no exceptions.
			//
			// **SO THIS IS ONE COMPARISON AND NOT A BRANCH, WHICH IS THE PART THAT MATTERS TO WHOEVER
			// READS IT NEXT.** It was `def == (maySitInAudioChain(d) ? 0.0 : 1.0)` — a rule with a
			// seam in it, and a seam is a place a future device can be put on the wrong side of by
			// accident. There is no side to be on now: a device declaring an enable declares it ON.
			//
			// THIS ASSERTED THE OPPOSITE FOR AUDIO DEVICES and the reasoning it gave is worth keeping
			// because it is what was overturned: *"an AUDIO device defaults OFF because it sits IN the
			// signal: enabled on arrival it would change the sound of a chain the moment it was added,
			// and the identity guarantee is that adding a device does nothing until a hand asks." The
			// press on (+) IS the hand asking, which the MODULATOR half of the old rule already said
			// in its own words — *"the route is the hand asking" — and the rack's own add path had
			// already overridden the declared default to reach the same behaviour by a second road.
			// One rule now, in the descriptor, read by both add paths.
			//
			// **THIS IS THE CASE THAT CANNOT BE OUTGROWN.** It walks `kDevices`, so a device added
			// next month is covered by having been registered, with nobody remembering. It is also
			// the case that DID get outgrown once in the other direction: `fx::automod`'s own comment
			// records shipping with the wrong default because *"the sweep iterates `kDevices`, and
			// this device was not in it." Registration is the whole of what enrols a device here.
			CHECK(d.params[at].def == doctest::Approx(1.0));
		}
	}

	SUBCASE("and ADDING any device, in either lane, lands its switch ON") {
		// **THE DECLARATION IS NOT THE BEHAVIOUR, AND THIS IS THE HALF THAT COSTS SOMETHING TO GET
		// WRONG.** The sweep above reads a number out of a descriptor. A number nothing reads at add
		// time would be a fix that changes nothing — the shape CLAUDE.md's value rule is about, from
		// the declaration end: `defaultRowFor` is what turns `ParamDesc::def` into a row, and until
		// that is asserted the sweep is guarding a constant rather than an outcome.
		//
		// THROUGH THE REAL ADD PATH, not `defaultRowFor` directly. `ParamBridge::addFxDevice` is a
		// one-line forward to `feathers::addFxDevice`, which is what a press on (+) reaches; calling
		// the row builder would skip exactly the layer that could stop reading the default.
		//
		// AND IT WALKS `kDevices`, so a device registered next month is enrolled by being registered.
		// It splits by what the device DECLARES rather than by a list of names: an audio device goes
		// to the lane, a modulator to the rack, and a device that is both is checked in both.
		size_t checkedFx = 0;
		size_t checkedRack = 0;
		for (const feathers::device::Descriptor& d : feathers::kDevices) {
			CAPTURE(std::string(d.traits.name));
			const size_t at = feathers::device::enableControlIn(d.traits, d.params);
			if (at == feathers::device::kNoControl) {
				continue;
			}
			if (feathers::maySitInAudioChain(d)) {
				feathers::FxChain chain {};
				const size_t slot = feathers::addFxDevice(&chain, d.traits.tag);
				REQUIRE(slot < chain.device.size());
				REQUIRE(at < chain.device[slot].row.fieldsPresent);
				// THE VALUE, NOT A DIFFERENCE. ">= 0.5" is how every reader in the repo asks this
				// question (`owltt.cpp`, `automod.cpp`, `FxCard::isEnabled`), so it is the same
				// threshold the sound is decided by rather than a second one invented here.
				CHECK(chain.device[slot].row.field[at] >= 0.5f);
				++checkedFx;
			}
			if (feathers::maySitInModRack(d)) {
				feathers::ModRack rack {};
				const size_t slot = feathers::addRackDevice(&rack, d.traits.tag);
				REQUIRE(slot < rack.device.size());
				REQUIRE(at < rack.device[slot].fieldsPresent);
				CHECK(rack.device[slot].field[at] >= 0.5f);
				++checkedRack;
			}
		}
		// **AND THE SWEEP IS NOT VACUOUS**, which a loop over a table with a `continue` in it cannot
		// promise on its own. A registry that stopped resolving tags, or a `maySitIn…` that answered
		// false for everything, would leave every CHECK above unexecuted and the subcase green.
		CHECK(checkedFx >= 7);
		CHECK(checkedRack >= 3);
	}

	SUBCASE("EVERY fx card actually draws and hits its declared switch, OWLTT included") {
		// THE GAP THAT LET A DEVICE SHIP UNSWITCHABLE. The sweep above asserts each device DECLARES a
		// valid enable; nothing asserted that its card DRAWS one or that a press reaches it. Those
		// are different claims and only the first was guarded.
		//
		// OWLTT shipped with `Enabled` defaulting to off and no square on its card at all — two
		// individually-correct changes composing into a device that could not be turned on. The
		// hand-rolled switch was deleted because the shared head would draw it after stage 4, and
		// the default was set to off to match the other devices. Neither was wrong alone.
		//
		// ASKED THROUGH A REAL PRESS ON THE REAL TREE, not by calling a handler: a square that is
		// drawn but unreachable is exactly the failure this is for.
		FilterFixture f;
		auto* lane = f.lane;
		REQUIRE(f.bridge.addFxDevice(fx::owltt::kTraits.tag) == 1);
		lane->resized();

		auto* owltt = lane->cardAt(1);
		REQUIRE(owltt != nullptr);
		auto* asFx = dynamic_cast<ui::FxCard*>(owltt);
		REQUIRE(asFx != nullptr);
		REQUIRE(asFx->descriptor() == &fx::owltt::kDescriptor);
		REQUIRE(asFx->hasEnable());
		CHECK(asFx->enableControl() == static_cast<size_t>(fx::owltt::Param::Enabled));

		const size_t enableIdx = asFx->enableControl();
		const auto before = f.bridge.fxChain().device[1].row.field[enableIdx];
		// ON BY DEFAULT since 2026-09-04; this read `before < 0.5f` and called it *"the
		// convention every fx device follows", which is still true and now points the other way. The
		// press below is asserted as a FLIP rather than as a value, so the reachability claim — the
		// only thing this subcase is for — survives the next change of default too.
		REQUIRE(before >= 0.5f);

		// THE SQUARE HAS AREA AND THE PRESS LANDS ON THE CARD, so this is a reachability claim and
		// not an arithmetic one.
		const visage::Bounds sw = asFx->enableBounds();
		REQUIRE(sw.width() > 0.0f);
		const visage::Point pressAt { sw.x() + sw.width() * 0.5f, sw.y() + sw.height() * 0.5f };
		REQUIRE(owltt->frameAtPoint(pressAt) == owltt);

		owltt->onMouseDown().callback(at(pressAt.x, pressAt.y));
		CHECK(f.bridge.fxChain().device[1].row.field[enableIdx] < 0.5f);
	}

	SUBCASE("and the card reaches its switch through that index rather than a literal") {
		FilterFixture f;
		auto* lane = f.lane;
		REQUIRE(f.bridge.addFxDevice(fx::harm::kTraits.tag) == 1);
		lane->resized();

		auto* filter = lane->filterCard();
		auto* harm = dynamic_cast<ui::HarmCard*>(lane->cardAt(1));
		REQUIRE(filter != nullptr);
		REQUIRE(harm != nullptr);

		// THE CARD KNOWS WHICH DEVICE IT DRAWS — the seam the shared head will ask through.
		REQUIRE(filter->descriptor() == &fx::filter::kDescriptor);
		REQUIRE(harm->descriptor() == &fx::harm::kDescriptor);

		CHECK(filter->hasEnable());
		CHECK(harm->hasEnable());
		CHECK(filter->enableControl() == static_cast<size_t>(fx::filter::Param::Enabled));
		CHECK(harm->enableControl() == static_cast<size_t>(fx::harm::Param::Enabled));

		// AND THE SQUARE IS REALLY THERE AND REALLY WORKS, so the derivation is not merely arithmetic
		// that agrees with a hardcoded square drawn beside it.
		const visage::Bounds sw = harm->enableBounds();
		CHECK(sw.width() > 0.0f);
		// ARRIVES ON since 2026-09-04, so the press this subcase makes is on→off. The
		// claim — the square is at the derived index and a press moves THAT control — is unchanged,
		// and it is asserted as a flip off a read arrival rather than against a literal 0, so it does
		// not have to be revisited if the default moves again.
		const auto arrival =
		    f.bridge.fxChain().device[1].row.field[static_cast<size_t>(fx::harm::Param::Enabled)];
		REQUIRE(arrival == doctest::Approx(1.0f));
		harm->onMouseDown().callback(at(sw.x() + sw.width() * 0.5f, sw.y() + sw.height() * 0.5f));
		CHECK(f.bridge.fxChain().device[1].row.field[static_cast<size_t>(fx::harm::Param::Enabled)]
		      == doctest::Approx(1.0f - arrival));
	}

	SUBCASE("and it reads the INDEX, which today's devices cannot prove because theirs is all zero") {
		// A KNOWN-BAD FOUND THIS. Replacing the whole lookup with `return 0` left every assertion above
		// GREEN — because `Enabled` is control 0 on both the filter and HARM, so "reads the declared
		// index" and "assumes the first control" give the same answer for every device that exists.
		//
		// THAT IS THE EXACT SHAPE THIS RULE IS ABOUT: correct only by coincidence, in a subsystem
		// whose whole direction is that devices vary. The point of an index is a device whose enable
		// is NOT first, so the case supplies one rather than waiting for OWLTT to be the first to find
		// out.
		static constexpr feathers::device::ParamDesc kProbeParams[] = {
		    { "probe_cutoff", "Probe Cutoff", 0.0, 1.0, 0.5, false, feathers::device::Unit::Percent, {}, true },
		    { "probe_drive", "Probe Drive", 0.0, 1.0, 0.0, false, feathers::device::Unit::Percent, {}, true },
		    { "probe_enabled", "Probe Enabled", 0.0, 1.0, 0.0, true, feathers::device::Unit::Choice, {} },
		};
		static constexpr feathers::device::Descriptor kProbe {
		    .traits = { .tag = "PRB1",
		                .name = "Probe",
		                .hasCard = true,
		                .hasExpanded = false,
		                .perVoiceControl = -1,
		                .enableControl = 2 },
		    .params = kProbeParams,
		    .outputs = {},
		};

		struct ProbeCard : ui::FxCard {
			const feathers::device::Descriptor* descriptor() const override { return &kProbe; }
			float laneHeightAt(float) const override { return 40.0f; }
		};
		ProbeCard card;
		card.setBounds(0.0f, 0.0f, 236.0f, 40.0f);

		CHECK(card.hasEnable());
		// THE ASSERTION THE REAL DEVICES CANNOT MAKE.
		CHECK(card.enableControl() == 2);
		CHECK(card.enableControl() != 0);
		CHECK(card.enableBounds().width() > 0.0f);

		SUBCASE("and an index past the end names nothing rather than being trusted") {
			static constexpr feathers::device::Descriptor kBadIndex {
			    .traits = { .tag = "PRB2",
			                .name = "Probe",
			                .hasCard = true,
			                .hasExpanded = false,
			                .perVoiceControl = -1,
			                .enableControl = 9 },
			    .params = kProbeParams,
			    .outputs = {},
			};
			struct BadCard : ui::FxCard {
				const feathers::device::Descriptor* descriptor() const override { return &kBadIndex; }
				float laneHeightAt(float) const override { return 40.0f; }
			};
			BadCard bad;
			bad.setBounds(0.0f, 0.0f, 236.0f, 40.0f);
			CHECK_FALSE(bad.hasEnable());
			CHECK(bad.enableBounds().width() == doctest::Approx(0.0f));
		}
	}

	SUBCASE("a card whose device declares no enable draws no square and answers no press") {
		// THE CASE THE WHOLE DERIVATION EXISTS FOR, and the one no shipping device exercises today —
		// so it is asked of a card built over a descriptor that declines. NoEditorCard returns nullptr
		// from `descriptor()`, which is the strongest form of "declares no enable": it does not even
		// know what it is drawing.
		FilterFixture f;
		auto* lane = f.lane;
		// AT POSITION 1, THE WHOLE ADDRESS SINCE KILL-LANES STEP 3C — see "every card in the lane carries
		// the lane's verbs" for what the `routeLane` and `order[]` pair that stood here used to buy.
		feathers::FxChain chain = f.bridge.fxChain();
		chain.device[1].row = feathers::DeviceRow {};
		chain.device[1].row.tag = { 'Z', 'Z', 'Z', 'Z' };
		chain.device[1].row.instanceId = chain.nextInstanceId;
		chain.nextInstanceId = chain.nextInstanceId + 1;
		chain.count = 2;
		f.bridge.restoreFxChain(chain);
		lane->resized();

		auto* unknown = dynamic_cast<ui::MissingDeviceCard*>(lane->cardAt(1));
		REQUIRE(unknown != nullptr);
		CHECK(unknown->descriptor() == nullptr);
		CHECK_FALSE(unknown->hasEnable());
		CHECK(unknown->enableControl() == ui::FxCard::kNoEnable);
		// AN EMPTY RECTANGLE CONTAINS NO POINT, so the hit test refuses it without a second branch —
		// which is why the absent case needs no special handling anywhere else.
		CHECK(unknown->enableBounds().width() == doctest::Approx(0.0f));
		CHECK_FALSE(unknown->enableBounds().contains({ 0.0f, 0.0f }));
	}
}
TEST_CASE("the strip asks the RACK whether a source is its own, not the lane's number") {
	// **THE OCCUPANCY FIX, AND THE THIRD ARM IT COST.** `ModPanel` decided "is this source mine" with
	// `isRackSource(source)` — `isDeviceSource && isRackDeviceSlot(deviceSlotOfSource(source))` — a
	// test on the lane's NUMBER. A number cannot know whether anything is at that lane, so an EMPTY
	// rack lane answered YES, and each of the seven callers went on to fetch a slot that came back out
	// of range and relied on whatever it did next to notice. `rackSlotOfSource` asks the rack's live
	// occupancy and answers the slot in the same lookup, so the question and the answer cannot part.
	//
	// **AND THE NUMBER-TEST WAS LOAD-BEARING, WHICH IS WHY THIS CASE EXISTS RATHER THAN A COMMENT.**
	// It was what kept a dead device source off the MACRO road: `arrowBounds` had no macro guard at
	// all, because every device source went down the rack road and was declined there. Made an
	// occupancy test without the third arm, it handed a source that names nothing a 19-wide rectangle
	// out of `macroColumnBounds`, which indexes without bounding. Caught by
	// "a device that declares no outputs gets no arrow at all" below, which is the neighbour of this.
	ModFixture f;
	auto* panel = f.panel;
	REQUIRE(panel != nullptr);
	REQUIRE(panel->deviceCount() >= 1);
	// THE RACK MUST NOT BE FULL, or "a lane the rack does not hold" is unreachable and every check
	// below passes by having nothing to say.
	REQUIRE(panel->deviceCount() < feathers::kMaxRackSlots);

	// A SOURCE NAMING A DEVICE NOBODY HOLDS — what "a lane the rack does not hold" became when a
	// source started naming its device by id.
	REQUIRE(f.bridge.descriptorOf(kNobody) == nullptr);
	const backplane::Source dead = backplane::Source::output(kNobody, 0);
	const backplane::Source live = backplane::Source::output(f.bridge.rackDeviceAt(0), 0);

	SUBCASE("a device source naming a device the rack does not hold is nobody's") {
		// EVERY SURFACE, BECAUSE THE FILTER WAS AT SEVEN OF THEM and a fix at six would read as done.
		CHECK(panel->sourceBounds(dead).width() == doctest::Approx(0.0f));
		CHECK(panel->outputChipBounds(dead).width() == doctest::Approx(0.0f));
		CHECK(panel->arrowBounds(dead).width() == doctest::Approx(0.0f));
		CHECK(panel->foldBounds(dead).width() == doctest::Approx(0.0f));
		CHECK(panel->slotMenuBounds(dead).width() == doctest::Approx(0.0f));
		// `cardMinimised` IS THE PUBLIC ROAD TO `instanceOfSource`, which is private — it answers false
		// exactly when the source resolves to no device, so it is the identity road asked from outside.
		CHECK_FALSE(panel->cardMinimised(dead));
		// AND NO HEIGHT RESERVED FOR IT. This reached `bodyHeightOf` with an out-of-range slot and got
		// a dormant card's height — a reservation for a card that does not exist.
		CHECK(panel->sourceHeight(dead) == doctest::Approx(0.0f));
		// AN EMPTY RECTANGLE HITS NOTHING, which is what makes the answers above sufficient rather
		// than merely tidy — no caller needs a second branch for this source.
		CHECK_FALSE(panel->arrowBounds(dead).contains({ 0.0f, 0.0f }));
		CHECK_FALSE(panel->sourceBounds(dead).contains({ 0.0f, 0.0f }));
	}

	SUBCASE("THE CONTROL: a source the rack DOES hold is still the strip's own") {
		// **WITHOUT THIS THE SUBCASE ABOVE PASSES ON A PANEL THAT ANSWERS NOTHING TO EVERYTHING.** A
		// filter that refused every source would satisfy every check up there, so the same surfaces
		// are asked about a LIVE device and required to answer.
		CHECK(panel->sourceBounds(live).width() > 0.0f);
		CHECK(panel->foldBounds(live).width() > 0.0f);
		CHECK(panel->slotMenuBounds(live).width() > 0.0f);
		CHECK(panel->sourceHeight(live) > 0.0f);
		// THE SLOT ITSELF, NOT MERELY "SOMETHING NON-EMPTY" — the rule about asserting the value. The
		// device at rack slot 0 must resolve to slot 0, which is the claim `rackSlotOfSource` makes
		// and the one a difference check cannot see.
		CHECK(panel->sourceBounds(live) == panel->envCardBounds(0));
		// AND THE ARROW, WHICH IS THE SURFACE THAT REGRESSED: a live device that declares an output
		// has one, so the third arm added to `arrowBounds` cannot have swallowed the live road too.
		REQUIRE(panel->outputCountOf(0) > 0);
		CHECK(panel->arrowBounds(live).width() > 0.0f);
	}

	SUBCASE("a macro is still a macro, and a weight source is still nobody's") {
		// THE OTHER TWO ARMS OF THE DISPATCH, so "not the rack's" is not quietly reading as "a macro".
		const backplane::Source macro = backplane::Source::core(0);
		REQUIRE(macro.index < feathers::kMacroCount);
		CHECK(panel->sourceBounds(macro).width() > 0.0f);
		CHECK(panel->arrowBounds(macro).width() > 0.0f);
		CHECK(panel->sourceHeight(macro) == doctest::Approx(ui::ModPanel::kMacroRowHeight));
		// A WEIGHT SOURCE HAS NO RECTANGLE IN THIS STRIP AT ALL — `sourceBounds` has said so since it
		// was written, and `arrowBounds` now agrees with it instead of computing a column past the end.
		const backplane::Source weight = backplane::Source::core(feathers::kFirstWeightSource);
		CHECK(panel->sourceBounds(weight).width() == doctest::Approx(0.0f));
		CHECK(panel->arrowBounds(weight).width() == doctest::Approx(0.0f));
	}
}
TEST_CASE("a device's arrows are its DECLARED outputs, in a row of their own below the head") {
	// THE 2026-08-24 DECISION'S CORE. An arrow is a modulation OUTPUT, and outputs are not a per-card
	// singleton — they only look like one. Both rack devices declare exactly ONE, so a head-mounted
	// arrow was correct only by coincidence of N=1, and nothing in the code could express N≠1.
	//
	// A HEAD SLOT MUST BE JUSTIFIED BY SOMETHING TRUE OF EVERY DEVICE THE LANE CAN HOLD, IN A FIXED
	// COUNT. An output count is not fixed, so it goes in the body.
	ModFixture f;
	auto* panel = f.panel;
	REQUIRE(panel != nullptr);
	REQUIRE(panel->deviceCount() >= 1);

	const backplane::Source source = backplane::Source::output(f.bridge.rackDeviceAt(0), 0);
	const visage::Bounds card = panel->sourceBounds(source);
	const visage::Bounds head { card.x(), card.y(), card.width(), ui::ModPanel::kHeadHeight };

	SUBCASE("the count comes from the descriptor, not from the strip") {
		// ASKED OF EVERY RACK SLOT AGAINST ITS OWN DECLARATION, so this is the RULE rather than a
		// statement about today's two devices.
		const feathers::ModRack rack = f.bridge.modRack();
		for (size_t slot = 0; slot < panel->deviceCount(); ++slot) {
			CAPTURE(slot);
			const feathers::device::Descriptor* desc =
			    feathers::deviceForTag(rack.device[slot].tag.data());
			REQUIRE(desc != nullptr);
			CHECK(panel->outputCountOf(slot) == desc->outputCount());
			// AND THE ROW EXISTS IFF THERE IS SOMETHING TO PUT IN IT — zero outputs costs zero height,
			// which is how a device that publishes nothing gets no arrow FROM THE RULE.
			CHECK((panel->outputRowHeight(slot) > 0.0f) == (desc->outputCount() > 0));
		}
	}

	SUBCASE("the arrow is at the FOOT of the card, not in the head") {
		// **IT WAS 'below the head' AND IT IS NOW 'at the bottom'.** The mod assign arrow is out of the
		// head, and by convention a card's mod outputs sit at its bottom. The strip it lives in is reserved by `ui::cardFrameOf`, taken out of
		// the BODY before the card draws — so this checks it is below the head AND inside the card,
		// which together say "in the outputs strip" without restating the strip's arithmetic.
		const visage::Bounds arrow = panel->arrowBounds(source);
		REQUIRE(arrow.width() > 0.0f);
		CHECK(arrow.y() >= head.bottom());
		CHECK(arrow.bottom() <= panel->sourceBoundsForTest(source).bottom() + 1.0f);
		CHECK(arrow.y() >= panel->sourceBoundsForTest(source).bottom()
		                       - ui::kCardOutputStripHeight - 1.0f);
		// AND IT IS STILL PRESSABLE THERE — the move must not put it under the card's own body, which
		// is exactly what happened when the row was added to the height and not to the placement.
		const visage::Point at { arrow.x() + arrow.width() * 0.5f, arrow.y() + arrow.height() * 0.5f };
		CHECK(panel->hitAt(at).part == ui::ModPanel::Part::Arrow);
		CHECK(panel->hitAt(at).source == source);
	}

	SUBCASE("the head folds instead of arming, as an audio effect's head does") {
		// `Part::Card` — the head rectangle arming the source — is gone. The conflict surfaced from the
		// other end before the arrow question existed: modulator cards could not be collapsed like audio
		// effects, because a press on the head expecting a fold armed the source instead.
		const visage::Point onHead { head.x() + 40.0f, head.y() + head.height() * 0.5f };
		CHECK(panel->hitAt(onHead).part == ui::ModPanel::Part::Fold);

		// AND IT REALLY FOLDS, driven through the real handler rather than asserted from the hit test.
		REQUIRE_FALSE(panel->cardMinimised(source));
		// TWO PRESSES — the panel's fold acts on the SECOND, so the whole head can be the target
		// without every press on a card's title bar collapsing it. Same rule as `FxCard`'s.
		panel->onMouseDown().callback(at(onHead.x, onHead.y));
		panel->onMouseDown().callback(doubleAt(onHead.x, onHead.y));
		CHECK(panel->cardMinimised(source));
	}

	SUBCASE("a folded card has no outputs row, because the row is body") {
		// AN ARROW THAT SURVIVED A FOLD WOULD BE THE HEAD CARRYING AN OUTPUT AGAIN BY THE BACK DOOR.
		REQUIRE(panel->outputRowHeight(0) > 0.0f);
		const float open = panel->sourceHeightForTest(source);
		REQUIRE(panel->setCardMinimised(source, true));
		CHECK(panel->sourceHeightForTest(source)
		      == doctest::Approx(ui::ModPanel::kHeadHeight));
		CHECK(panel->sourceHeightForTest(source) < open);
	}

	SUBCASE("and a device that declares no outputs gets no arrow at all") {
		// THE CASE THE DECISION NAMES AS ITS TEST, reached from the same rule that gives the MSEG one
		// rather than from a branch on which lane we are in. No rack device declares zero outputs
		// (`maySitInModRack` requires at least one), so this asks the geometry directly: a slot whose
		// declared count is zero has a zero-height row and an empty arrow rectangle.
		//
		// DRIVEN THROUGH A SLOT PAST THE RACK'S COUNT, which reports zero outputs by the same road an
		// unknown tag does — `outputCountOf` answers 0 and everything downstream follows.
		const size_t empty = feathers::kMaxRackSlots - 1;
		REQUIRE(panel->deviceCount() <= empty);
		CHECK(panel->outputCountOf(empty) == 0);
		CHECK(panel->outputRowHeight(empty) == doctest::Approx(0.0f));
		CHECK(panel->outputRowBounds(empty).height() == doctest::Approx(0.0f));
		const backplane::Source noSource = backplane::Source::output(kNobody, 0);
		CHECK(panel->arrowBounds(noSource).width() == doctest::Approx(0.0f));
		// AN EMPTY RECTANGLE HITS NOTHING, so no second branch is needed anywhere downstream.
		CHECK_FALSE(panel->arrowBounds(noSource).contains({ 0.0f, 0.0f }));
	}
}
TEST_CASE("an output's arrow wears the device's own name for it, and the name is part of the button") {
	// THE ARROW MOVED OUT OF THE HEAD (2026-08-24, stage 3) and nothing replaced it there, so
	// a player who had learned the head-press finds an inert head and no clue where the verb went.
	// A control nobody can see is a control that does not exist — the same lesson the missing chevron
	// on HARM's harmonic box taught this week — so the arrow's new home has to announce itself.
	//
	// AND THE NAME IS NOT ONLY FOR DISCOVERABILITY. A device publishing two outputs draws two
	// IDENTICAL glyphs; nothing but the label says which one is which. The bare arrow was legible
	// only while every rack device declared exactly one, which is the same N=1 that put it in the
	// head in the first place.
	ModFixture f;
	auto* panel = f.panel;
	REQUIRE(panel != nullptr);

	namespace dsr = feathers::mod::dahdsr;
	namespace mseg = feathers::mod::mseg;

	// TWO DEVICES WITH DIFFERENT-LENGTH OUTPUT NAMES, which is what makes the width claim below a
	// measurement rather than a coincidence: "Envelope" and "MSEG" cannot both fit a fixed step.
	feathers::ModRack rack = f.bridge.modRack();
	rack.setMsegAt(1, mseg::Settings {});
	rack.count = 2; // `setMsegAt` writes a slot; the rack's length is a separate fact
	// **AND NOTHING ELSE, SINCE KILL-LANES STEP 3C.** An `assignDenseRackLanes(&rack)` stood here because
	// a slot written by hand had not been through the lane allocator, and a device with no lane was one
	// the strip could find no card for — the geometry resolved a source index back to a slot by asking
	// which slot held that LANE. There is no lane to allocate now: a device's place IS its slot, and the
	// one thing a hand-written row still lacks, an instance id, `restoreModRack` issues for it.
	f.bridge.restoreModRack(rack);
	REQUIRE(f.bridge.modRack().holds(0, dsr::kTraits.tag));
	REQUIRE(f.bridge.modRack().holds(1, mseg::kTraits.tag));
	REQUIRE(panel->deviceCount() == 2);

	SUBCASE("the name is the DESCRIPTOR's, asked of the device rather than written by the strip") {
		const feathers::ModRack held = f.bridge.modRack();
		for (size_t slot = 0; slot < panel->deviceCount(); ++slot) {
			CAPTURE(slot);
			const feathers::device::Descriptor* desc =
			    feathers::deviceForTag(held.device[slot].tag.data());
			REQUIRE(desc != nullptr);
			for (size_t output = 0; output < desc->outputCount(); ++output) {
				CAPTURE(output);
				// UNLABELLED, the label IS the declared name (`ParamBridge::outputLabel`).
				CHECK(panel->outputName(slot, output) == std::string(desc->outputs[output].name));
			}
			// AND NOTHING PAST WHAT IT DECLARES, which is what keeps a chip from being drawn for an
			// output the device does not have.
			CHECK(panel->outputName(slot, desc->outputCount()).empty());
		}
		// THE VALUES THE TWO DEVICES ACTUALLY DECLARE, pinned. If a device renames its output this
		// goes red and somebody reads the line above, which is the point of pinning it.
		CHECK(panel->outputName(0, 0) == "Envelope");
		CHECK(panel->outputName(1, 0) == "MSEG");
	}

	SUBCASE("the chip is as wide as its NAME MEASURES, not a step somebody chose") {
		// THE VALUE, COMPUTED INDEPENDENTLY. Both chips carry the same arrow box and the same padding,
		// so the DIFFERENCE between their widths is exactly the difference between their measured
		// labels — and that is a quantity this case works out for itself from the font, not one it
		// reads back out of the layout.
		const float envelope =
		    ui::trackedWidth(ui::font(ui::kKnobLabelSize), "Envelope", ui::kKnobLabelTracking);
		const float shorter =
		    ui::trackedWidth(ui::font(ui::kKnobLabelSize), "MSEG", ui::kKnobLabelTracking);
		REQUIRE(envelope > shorter); // else the case cannot tell a measured layout from a fixed one

		CHECK(panel->outputChipWidth(0, 0) - panel->outputChipWidth(1, 0)
		      == doctest::Approx(envelope - shorter).epsilon(0.001));

		// AND THE CHIP REALLY HOLDS THE LABEL rather than merely being wider than its neighbour: it
		// is at least the arrow's box plus the measured name. A chip that had lost its label term
		// would be the bare arrow box and fail this.
		CHECK(panel->outputChipWidth(0, 0) >= ui::ModPanel::kArrowSize + 6.0f + envelope);
	}

	SUBCASE("the label PRESSES — it is the button, not a caption beside one") {
		// A LABEL THAT COULD BE READ AND NOT PRESSED WOULD PUT THE READABLE HALF AND THE PRESSABLE
		// HALF IN DIFFERENT PLACES, which is worse than no label: it teaches the wrong edge.
		const backplane::Source source = backplane::Source::output(f.bridge.rackDeviceAt(0), 0);
		const visage::Bounds chip = panel->outputChipBounds(source);
		const visage::Bounds arrow = panel->arrowBounds(source);
		REQUIRE(chip.width() > 0.0f);
		// THE ARROW IS INSIDE THE CHIP, so the older case that presses the glyph keeps meaning what
		// it meant — this widens the target, it does not move it.
		CHECK(chip.contains(visage::Point { arrow.x() + 1.0f, arrow.y() + 1.0f }));
		CHECK(chip.contains(visage::Point { arrow.right() - 1.0f, arrow.bottom() - 1.0f }));

		// A POINT PAST THE ARROW, IN THE LABEL. Chosen from the geometry rather than guessed, and
		// REQUIREd to be past the glyph so the case cannot pass by landing on the arrow after all.
		const visage::Point onLabel { chip.right() - 4.0f, chip.y() + chip.height() * 0.5f };
		REQUIRE(onLabel.x > arrow.right());
		CHECK(panel->hitAt(onLabel).part == ui::ModPanel::Part::Arrow);
		CHECK(panel->hitAt(onLabel).source == source);

		// AND IT ARMS, driven through the real tree at the real place rather than asserted from the
		// hit test — hitAt naming a part is not the handler acting on it.
		//
		// READ OFF THE DESTINATION, as "an envelope can be armed from its card" reads it: what
		// matters is that a knob would now take a route from THIS source, which is what a hand can
		// see. A flag is a thing the panel set; a mappable knob is the arm having happened.
		ui::Knob* path = f.knobFor(ParamId::Path);
		REQUIRE(path != nullptr);
		REQUIRE(path->isModDestination());
		REQUIRE_FALSE(path->mappable());

		const visage::Point inWindow = panel->positionInWindow() + onLabel;
		dragThroughRoot(f.root, inWindow, inWindow);
		CHECK(path->mappable());

		// AND THE ROUTE COMES FROM THE OUTPUT THE LABEL NAMES, not from whatever was focused before.
		// The VALUE, not a difference: the amount lands at `source`'s row and at nobody else's.
		const visage::Point knobAt = f.faceCentre(path);
		dragThroughRoot(f.root, knobAt, knobAt - visage::Point { 0.0f, 40.0f });
		const int destination = feathers::modDestinationIndexOf(ParamId::Path);
		REQUIRE(destination >= 0);
		const backplane::Destination pathDest = backplane::Destination::core(static_cast<size_t>(destination));
		CHECK(f.bridge.routeAmount(source, pathDest) > 0.0f);
		for (size_t macro = 0; macro < kMacroCount; ++macro) {
			CAPTURE(macro);
			CHECK(f.bridge.routeAmount(backplane::Source::core(macro), pathDest) == 0.0f);
		}
	}

	SUBCASE("chips are laid end to end and stay inside the card") {
		// THE RULE, asked of every slot and every output it declares. N IS 1 FOR BOTH OF TODAY'S RACK
		// DEVICES, so the no-overlap half of this is unexercised and says so out loud rather than
		// reading as proven — the running-sum layout is what a second output will exercise, and this
		// is here to go red the day one arrives badly.
		for (size_t slot = 0; slot < panel->deviceCount(); ++slot) {
			CAPTURE(slot);
			const size_t outputs = panel->outputCountOf(slot);
			const visage::Bounds card =
			    panel->sourceBounds(backplane::Source::output(f.bridge.rackDeviceAt(slot), 0));
			float previousRight = -1.0f;
			for (size_t output = 0; output < outputs; ++output) {
				CAPTURE(output);
				const visage::Bounds chip = panel->outputChipBounds(
				    backplane::Source::output(f.bridge.rackDeviceAt(slot), output));
				CHECK(chip.width() == doctest::Approx(panel->outputChipWidth(slot, output)));
				CHECK(chip.x() > previousRight);
				CHECK(chip.right() <= card.right());
				previousRight = chip.right();
			}
		}
	}
}
TEST_CASE("backlog FL: EVERY output of EVERY device the strip can hold has a chip, and it is inside "
          "its own card") {
	// **THE RULE, ASKED OF THE WHOLE REGISTRY.** For every modulator, all of its outputs are on its
	// card.
	//
	// **THE SUBCASE ABOVE IS THE SAME CLAIM ABOUT WHATEVER THE FIXTURE HAPPENS TO HOLD**, and says so:
	// *"N IS 1 FOR BOTH OF TODAY'S RACK DEVICES, so the no-overlap half of this is unexercised." It
	// is not any more — Note Expressions declares seven and SKLITTER declares seven — but a case that
	// walks the fixture's rack cannot see a device that is not in it. This one puts EVERY device the
	// strip can hold into a rack of its own, which is what makes the next seven-output device's
	// version of this line impossible to omit quietly.
	//
	// **THE CARD IS `kCardWidth` WIDE AND THAT IS THE ONLY WIDTH THIS HOST HAS** — the strip is a
	// fixed-width rail, so the narrowest card it can produce is the card it always produces. The two
	// lanes that can vary a card's width have their own version of this case.
	//
	// **AND EVERY OUTPUT IS REQUIRED TO BE PRESENT, WHICH THE HIDING RULE PERMITS RATHER THAN
	// CONTRADICTS** (`ui::layoutOutputChips`, 2026-09-19). A card in this strip GROWS to hold its own
	// foot — `sourceHeight` asks the card, and the card counts the rows — so no chip can fail to fit
	// at this one width, and "all of them are drawn" is the right assertion here. The host that can
	// run out of room is the rack, whose card height is granted rather than asked for.
	size_t devicesChecked = 0;
	size_t outputsChecked = 0;
	size_t multiOutputDevices = 0;
	for (const feathers::device::Descriptor& d : feathers::kDevices) {
		if (!feathers::maySitInModRack(d)) {
			continue;
		}
		// THROUGH `std::string`, BECAUSE A BARE `const char*` PRINTS AS `1` HERE — measured while
		// reading a known-bad's output, where a device name would have said which device.
		CAPTURE(std::string(d.traits.name));
		// A FIXTURE PER DEVICE, so each card is the first in its strip and is laid out.
		ModFixture g;
		const size_t slot = g.bridge.addRackDevice(d.traits.tag);
		if (slot >= feathers::kMaxRackSlots) {
			continue; // the strip refused it; that is a different case's claim
		}
		g.bridge.refresh();
		auto* panel = g.panel;
		REQUIRE(panel != nullptr);
		REQUIRE(panel->outputCountOf(slot) == d.outputCount());

		const visage::Bounds card =
		    panel->sourceBounds(backplane::Source::output(g.bridge.rackDeviceAt(slot), 0));
		REQUIRE(card.width() > 0.0f);
		// **THE FOOT REALLY HOLDS THE ROWS IT SAYS IT DOES** — the value, not a difference: the strip
		// the frame reserves is the row count times the row height.
		//
		// **THIS ONE IS NOT A TAUTOLOGY AND ITS TWIN IN THE FX-LANE SUITE ONCE WAS — the two lines read
		// almost identically, so which is which is written down here rather than left to be worked out
		// again.** `outputRowBounds` goes through `ui::cardFrameOf`, so the left side is a rectangle
		// the frame computed and the right side is the count that was passed to it: they can disagree,
		// and the clamp for a card too short to hold its foot is one way they would. The lane's version
		// asked `FxCard::outputStripHeight()`, which is DEFINED as that product, so it read `22*r ==
		// 22*r`; it asks `ownFrame().outputs.height()` now, for the reason this one always could.
		CHECK(panel->outputRowBounds(slot).height()
		      == doctest::Approx(ui::kCardOutputStripHeight
		                         * static_cast<float>(panel->outputRowsOf(slot))));

		float previousRight = -1.0f;
		float previousTop = -1.0f;
		for (size_t output = 0; output < d.outputCount(); ++output) {
			CAPTURE(output);
			CAPTURE(nameOrUnnamed(d.outputs[output].name));
			const visage::Bounds chip = panel->outputChipBounds(
			    backplane::Source::output(g.bridge.rackDeviceAt(slot), output));
			if (((g.bridge.outputsShown(g.bridge.rackDeviceAt(slot)) >> output) & 1u) == 0u) {
				CHECK(chip.width() == 0.0f); // NOT ON THE CARD (design §7.10): no room, and its route reads on
				continue;
			}
			REQUIRE(chip.width() > 0.0f);
			// A GRID DEVICE'S CHIP IS ITS CELL, NOT ITS NAME'S WIDTH — the grid case pins the cell.
			if (d.traits.outputGridColumns == 0) {
				CHECK(chip.width() == doctest::Approx(panel->outputChipWidth(slot, output)));
			}
			// INSIDE ITS CARD, ALL FOUR EDGES — `visage::Bounds` has `contains(Point)` and not
			// `contains(Bounds)`, so the four inequalities are written out.
			CHECK(chip.x() >= card.x() - 0.5f);
			CHECK(chip.right() <= card.right() + 0.5f);
			CHECK(chip.y() >= card.y() - 0.5f);
			CHECK(chip.bottom() <= card.bottom() + 0.5f);
			// **AND NOT ON TOP OF THE ONE BEFORE IT: further RIGHT on the same row, or on a row below.**
			// It was "further left" until 2026-09-20, because `layoutOutputChips` packed from the right
			// and drew a seven-output device's chips in reverse, which read as badly formatted. The
			// claim is unchanged; the direction that counts as forward is now
			// the one the names are read in.
			const bool sameRow = std::abs(chip.y() - previousTop) < 0.5f;
			if (output > 0 && sameRow) {
				CHECK(chip.x() >= previousRight);
			}
			else if (output > 0) {
				CHECK(chip.y() > previousTop);
			}
			previousRight = chip.right();
			previousTop = chip.y();
			++outputsChecked;
		}
		if (d.outputCount() > 1) {
			++multiOutputDevices;
		}
		++devicesChecked;
	}
	// NOT VACUOUS, AND NOT VACUOUS IN THE WAY THAT MATTERS: a registry of one-output devices would
	// satisfy every assertion above without exercising the wrap at all.
	CHECK(devicesChecked >= 3);
	CHECK(outputsChecked >= 7);
	CHECK(multiOutputDevices >= 1);
}
TEST_CASE("FxCard::laneExtent's DEFAULT spends the cross it is given, on a card that never thought "
          "about width") {
	// **THE DEFAULT WAS THE TRAP, NOT THE CALL SITES.** `laneExtent(float cross)` opened `(void)cross;`
	// and its vertical branch returned `laneHeight()` — the width-blind road — so the three hosts that
	// pass a width precisely to close the zero-width trap were all defeated one level up:
	// `FxLane::cardBounds` computes `width() - 2 * kPadX`, `RackEditor` passes the 480 it is about to
	// set, and `ModPanel::resized` passes the width in the same call. Every one was correct only
	// because the card that happened to arrive was a `GenericCard`, the single class that overrode it.
	//
	// **NO SHIPPING CARD CAN EXPRESS THIS, WHICH IS WHY THE CARD HERE IS BUILT FOR IT.** The trap needs
	// a card that (a) takes the default `laneExtent` and (b) has a device declaring enough outputs to
	// WRAP — and no hand-written fx card declares an output at all today. That is the whole of why the
	// instance was latent, and a latent instance nothing exercises is one refactor from being live.
	//
	// **SO THE PROBE IS HONEST ABOUT WHAT IT IS**: a minimal `FxCard` that overrides nothing but its
	// descriptor and its width-independent body, exactly as the next hand-written card would.
	constexpr float kProbeBody = 40.0f;
	static constexpr feathers::device::SourceDesc kSeven[] = {
		{ .key = "alpha", .name = "Alpha" },  { .key = "bravo", .name = "Bravo" },
		{ .key = "charlie", .name = "Charlie" }, { .key = "delta", .name = "Delta" },
		{ .key = "echo", .name = "Echo" },   { .key = "foxtrot", .name = "Foxtrot" },
		{ .key = "golf", .name = "Golf" },
	};
	static constexpr feathers::device::Descriptor kWide {
	    .traits = { .tag = "PRB3", .name = "Probe", .hasCard = true, .hasExpanded = false,
		            .perVoiceControl = -1, .enableControl = -1 },
	    .params = {},
	    .outputs = kSeven,
	};
	struct PlainCard : ui::FxCard {
		const feathers::device::Descriptor* descriptor() const override { return &kWide; }
		// **THE CONTRACT IS THE WHOLE CARD, FOOT INCLUDED — the base does not compose it**, and the
		// first draft of this case assumed it did. That assumption was worth having written down
		// wrongly once: `cardFrameOf` CARVES the foot out of the rectangle the card is given, so a
		// card that omits the strip term is asking its own body to be short by it. Every shipping
		// card whose device publishes outputs includes it (`EnvCard`, `MsegCard`, `LfoCard`,
		// `GenericCard::liveHeight`); the hand-written fx cards omit it and are right to, because
		// their devices declare none and `outputStripHeightAt` is then zero either way.
		//
		// THE BODY IS A CONSTANT AND ONLY THE FOOT CONSULTS THE WIDTH, which is the shape of every
		// real card here and is what makes this probe a fair stand-in for the next one.
		float laneHeightAt(float cardWidth) const override {
			return kProbeBody + outputStripHeightAt(cardWidth);
		}
	};
	PlainCard card;
	REQUIRE(card.descriptor()->outputCount() == 7);

	// **THE TWO WIDTHS ARE CHOSEN FROM THE WALK, NOT GUESSED**, and the case REQUIREs they disagree
	// before asserting anything about them — otherwise every line below would pass on a card that
	// ignored the cross entirely, which is precisely the bug.
	constexpr float kNarrow = 120.0f;
	constexpr float kWideCross = 900.0f;
	const size_t narrowRows = ui::outputChipRowsFor(kWide, kNarrow);
	const size_t wideRows = ui::outputChipRowsFor(kWide, kWideCross);
	REQUIRE(narrowRows > wideRows);
	REQUIRE(wideRows >= 1);

	// **THE VALUE, RESTATED WITHOUT THE ROAD UNDER TEST**: the card's own body plus one strip row per
	// row of chips at THAT cross. A difference check between the two answers would pass for a card
	// that varied for any reason at all.
	CHECK(card.laneExtent(kNarrow)
	      == doctest::Approx(kProbeBody
	                         + ui::kCardOutputStripHeight * static_cast<float>(narrowRows)));
	CHECK(card.laneExtent(kWideCross)
	      == doctest::Approx(kProbeBody
	                         + ui::kCardOutputStripHeight * static_cast<float>(wideRows)));

	// **AND IT IS NOT READING `width()` INSTEAD.** The card has never been placed, so `width()` is 0;
	// the old road answered `laneHeight()`, which at width 0 cannot wrap and reports ONE row — the
	// same number for both crosses. Stated as its own assertion because it is the actual defect, and
	// because a card that returned a constant would satisfy neither of the two above.
	REQUIRE(card.width() == 0.0f);
	CHECK(card.laneExtent(kNarrow) != doctest::Approx(card.laneExtent(kWideCross)));

	// THE FOLD AND THE HORIZONTAL AXIS ARE STILL THE BASE'S, and the cross must not leak into either —
	// a folded card is its spine and a horizontal lane wants a WIDTH back.
	card.setLaneAxis(ui::FxCard::LaneAxis::Horizontal);
	CHECK(card.laneExtent(kNarrow) == doctest::Approx(ui::kCardUnitWidth));
	card.setLaneAxis(ui::FxCard::LaneAxis::Vertical);
	card.setFolded(true);
	CHECK(card.laneExtent(kNarrow) == doctest::Approx(ui::kCardHeadHeight));
}
TEST_CASE("every declared output chip fits the narrowest strip it can be laid into") {
	// **THE ACROSS HALF OF THE HIDING RULE, PINNED AS A NUMBER.** `ui::layoutOutputChips` hides a
	// chip wider than the strip's usable width — no row could hold it — and `GenericCard::hiddenCount`
	// deliberately does not signal that (backlog FT). The reason it is safe to defer is that no chip
	// is that wide, and THAT is a property of the DEVICE'S OUTPUT NAME rather than of any layout: it
	// holds by a margin, and a margin is a claim nothing checks until something checks it.
	//
	// **SO THIS MEASURES THE MARGIN INSTEAD OF ASSERTING IT IN PROSE.** A new output called something
	// long goes red here rather than quietly vanishing off a card, which is the failure mode the
	// justification in `generic_card.cpp` would otherwise be carrying on trust.
	//
	// **AT THE NARROWEST STRIP A CHIP CAN BE LAID INTO**, which is the modulator strip's: `kCardWidth`
	// with a top head, so the strip spans the card (`FxCard::outputStripWidthAt`). The two lanes that
	// can vary a card's width are wider and have their own cases.
	const float usable =
	    ui::ModPanel::kCardWidth - 2.0f * ui::kCardOutputChipPadX;
	REQUIRE(usable > 0.0f);

	std::string widestName;
	float widest = 0.0f;
	size_t namesChecked = 0;
	for (const feathers::device::Descriptor& d : feathers::kDevices) {
		if (!feathers::maySitInModRack(d)) {
			continue;
		}
		for (const feathers::device::SourceDesc& out : d.outputs) {
			const char* name = out.name != nullptr ? out.name : "";
			const float w = ui::SourceArm::widthFor(name);
			CAPTURE(std::string(d.traits.name));
			CAPTURE(std::string(name));
			CAPTURE(w);
			CAPTURE(usable);
			// THE VALUE: this chip fits a row of this strip. A chip that does not is DRAWN NOWHERE,
			// and nothing in the picture says so.
			CHECK(w <= usable);
			if (w > widest) {
				widest = w;
				widestName = name;
			}
			++namesChecked;
		}
	}
	// NOT VACUOUS: a registry whose devices published nothing would satisfy every line above.
	REQUIRE(namesChecked >= 7);
	// **THE MARGIN, REPORTED RATHER THAN ASSUMED.** Logged unconditionally so the number is in the
	// record the day somebody asks how close this is, and so the comment in `generic_card.cpp` can
	// point at a measurement instead of a recollection.
	MESSAGE("widest output chip: \"" << widestName << "\" at " << widest << " of " << usable
	                                 << " usable (" << (usable - widest) << " to spare)");
	CHECK(widest < usable);
}
TEST_CASE("backlog HA: a card's output chips READ in declared order — chip n left of chip n+1 on a "
          "row — every row starts at one x — and a strip too small hides a TAIL of the outputs") {
	// **A BOUNDS CASE THAT DOES NOT ASSERT ORDER IS SATISFIED BY A STRIP LAID BACKWARDS, which is how
	// this shipped.** Until 2026-09-20 `ui::layoutOutputChips` packed from the RIGHT, so SKLITTER's
	// card drew `Weight 1` to the right of `Weight 2` and four right-anchored rows whose LEFT edges
	// stepped in and out, which read as badly formatted. Four cases
	// asserted each chip was inside its card and did not overlap its neighbour — all true of the
	// backwards strip. **The missing claim was the ORDER**, and this is it.
	//
	// **THE REFERENCE IS RESTATED, NOT TRANSCRIBED** (CLAUDE.md: compute the reference by an
	// independent restatement of the intended path). The implementation walks a running CURSOR and
	// wraps when the cursor plus the chip would cross the far pad; this walks a CUMULATIVE WIDTH per
	// row — chips plus the gaps between them against the usable width — and derives x by summing what
	// precedes the chip on its own row. The two agree only if the wrap points agree, and a
	// right-packing implementation disagrees with this one on every x while agreeing on every width.
	struct Placed {
		size_t row = 0;
		float x = 0.0f;
		float w = 0.0f;
		bool shown = false;
	};
	const auto reference = [](const feathers::device::Descriptor& d, visage::Bounds strip,
	                          bool fromLeft) {
		const float pad = ui::kCardOutputChipPadX;
		const float gap = ui::kCardOutputChipGap;
		const float usableWidth = std::max(0.0f, strip.width() - 2.0f * pad);
		std::vector<Placed> out;
		float used = 0.0f; // widths plus gaps already committed to this row
		size_t row = 0;
		for (size_t i = 0; i < d.outputs.size(); ++i) {
			const char* name = d.outputs[i].name != nullptr ? d.outputs[i].name : "";
			const float w = ui::SourceArm::widthFor(name);
			const float need = used == 0.0f ? w : used + gap + w;
			if (i > 0 && need > usableWidth && w <= usableWidth) {
				++row;
				used = w;
			}
			else {
				used = need;
			}
			// **THE ROW'S CONTENT IS WHAT DECIDES x, FROM WHICHEVER END.** `used` is everything up to
			// and including this chip; `used - w` is everything before it on the row.
			const float before = used - w;
			const float x = fromLeft ? strip.x() + pad + before : strip.right() - pad - used;
			const bool fits = w <= usableWidth
			                  && static_cast<float>(row + 1) * ui::kCardOutputStripHeight
			                         <= strip.height() + 0.01f;
			out.push_back(Placed { row, x, w, fits });
		}
		return out;
	};

	size_t devicesChecked = 0;
	size_t rowsSeen = 0;
	size_t wrappedDevices = 0;
	size_t hidingStrips = 0;

	for (const feathers::device::Descriptor& d : feathers::kDevices) {
		if (d.outputCount() == 0) {
			continue;
		}
		// A GRID DEVICE (`Traits::outputGridColumns`) IS LAID BY THE GRID, NOT THE FLOW: its own case follows.
		if (d.traits.outputGridColumns > 0) {
			continue;
		}
		CAPTURE(std::string(d.traits.name));
		++devicesChecked;
		// **THREE WIDTHS, because wrapping is what the order claim is really about.** The modulator
		// strip's own card width, the fx lane's, and one narrow enough to force several rows out of
		// every publishing device in the build.
		for (const float stripWidth : { ui::ModPanel::kCardWidth, ui::FxLane::kWidth - 20.0f, 150.0f }) {
			CAPTURE(stripWidth);
			const size_t rows = ui::outputChipRowsFor(d, stripWidth);
			REQUIRE(rows >= 1);
			rowsSeen += rows;
			if (rows > 1) {
				++wrappedDevices;
			}
			// A STRIP TALL ENOUGH FOR EVERY ROW, so this pass is about ORDER and nothing is hidden.
			const visage::Bounds strip { 40.0f, 12.0f, stripWidth,
				                         ui::kCardOutputStripHeight * static_cast<float>(rows) };
			const std::vector<visage::Bounds> chips = ui::outputChipBoundsFor(d, strip);
			REQUIRE(chips.size() == d.outputCount());
			const std::vector<Placed> want = reference(d, strip, /*fromLeft=*/true);

			// **THE ONE x EVERY ROW STARTS AT**, taken from the FIRST chip and then required of every
			// later row's first chip. Read off the placement rather than restated, so the claim is
			// "the rows agree with each other" rather than "they agree with a number I wrote here".
			const float rowLeft = chips.front().x();
			for (size_t i = 0; i < chips.size(); ++i) {
				CAPTURE(i);
				CAPTURE(nameOrUnnamed(d.outputs[i].name));
				REQUIRE(chips[i].width() > 0.0f); // nothing is hidden at this height
				// **THE VALUE, AGAINST THE RESTATEMENT** — where this chip goes, not merely that it
				// does not collide with its neighbour.
				CHECK(chips[i].x() == doctest::Approx(want[i].x).epsilon(0.001));
				CHECK(chips[i].width() == doctest::Approx(want[i].w).epsilon(0.001));
				CHECK(chips[i].y()
				      == doctest::Approx(strip.y() + static_cast<float>(want[i].row)
				                         * ui::kCardOutputStripHeight
				                         + (ui::kCardOutputStripHeight - ui::SourceArm::kHeight) * 0.5f)
				             .epsilon(0.001));
				// **AND THE READING ORDER AS ITS OWN CLAIM**, so the case says the thing a reader
				// would say: chip n is left of chip n+1 on a row, and a new row starts below.
				if (i > 0 && want[i].row == want[i - 1].row) {
					CHECK(chips[i].x() >= chips[i - 1].right());
					CHECK(chips[i].y() == doctest::Approx(chips[i - 1].y()));
				}
				else if (i > 0) {
					CHECK(chips[i].y() > chips[i - 1].y());
					// **EVERY ROW STARTS AT ONE x** — the half a right-anchored strip cannot have, and
					// the one that made four rows step in and out at four different left edges.
					CHECK(chips[i].x() == doctest::Approx(rowLeft));
				}
			}
			// AND THE FIRST CHIP SITS AT THE STRIP'S OWN LEFT PAD, which is what "one x" is anchored to.
			CHECK(chips.front().x() == doctest::Approx(strip.x() + ui::kCardOutputChipPadX));

			// **WHICH CHIPS A STRIP TOO SHORT HIDES, MEASURED AND NOT ARGUED — and it is the SAME SET
			// either way the strip is packed.** The lead's question on this change was whether the
			// mirror moved the hiding: right-packing hid the leftmost chip of a row, left-packing hides
			// the rightmost, and those are different OUTPUTS. **They are not.** Hiding is decided by
			// `fitsAcross` (this chip's own width against the usable width) and `fitsDown` (this chip's
			// ROW against the strip's height), and the reference above computes both from the widths
			// alone — so running it `fromLeft=false` produces a right-packed placement over the same
			// row partition, and the shown/hidden flags are compared between the two. The positions
			// differ; the set does not.
			//
			// **AND THAT IS MEASURED RATHER THAN DERIVED, which matters because the derivation is the
			// kind that reads as obviously true.** Under the right-packing known-bad the ORDER
			// assertions above go red on every publishing device — position, same-row order and the
			// shared row-left, all three — and **these hiding assertions stay green**, including the
			// one comparing the LIVE implementation against a LEFT-packed reference's flags. A
			// right-packed build hiding exactly what a left-packed reference says it hides is the
			// claim, and it was run in both directions rather than reasoned about in one.
			for (size_t shortRows = 0; shortRows < rows; ++shortRows) {
				const visage::Bounds shortStrip { 40.0f, 12.0f, stripWidth,
					                              ui::kCardOutputStripHeight
					                                  * static_cast<float>(shortRows) };
				const std::vector<visage::Bounds> got = ui::outputChipBoundsFor(d, shortStrip);
				const std::vector<Placed> left = reference(d, shortStrip, /*fromLeft=*/true);
				const std::vector<Placed> right = reference(d, shortStrip, /*fromLeft=*/false);
				REQUIRE(got.size() == d.outputCount());
				bool anyHidden = false;
				for (size_t i = 0; i < got.size(); ++i) {
					CAPTURE(shortRows);
					CAPTURE(i);
					CHECK((got[i].width() > 0.0f) == left[i].shown);
					// THE TWO DIRECTIONS AGREE ABOUT WHAT IS HIDDEN, chip for chip.
					CHECK(left[i].shown == right[i].shown);
					anyHidden = anyHidden || !left[i].shown;
				}
				if (anyHidden) {
					++hidingStrips;
				}
				// **AND WHAT IS SHOWN IS A PREFIX OF THE DECLARED OUTPUTS**, which is the answer to
				// "is that acceptable": shortening a card drops `Perf Speed` before it drops `Perf Y`.
				// It never hides an interior chip while a later one survives — that would need a name
				// wider than the whole strip, which the margin case above measures does not exist.
				bool seenHidden = false;
				for (size_t i = 0; i < got.size(); ++i) {
					CAPTURE(shortRows);
					CAPTURE(i);
					if (!left[i].shown) {
						seenHidden = true;
						continue;
					}
					CHECK_FALSE(seenHidden); // a shown chip after a hidden one would break the prefix
				}
			}
		}
	}
	// NOT VACUOUS, AND EACH COUNTER COUNTS SOMETHING A BROKEN LOOP WOULD HAVE MISSED: devices that
	// publish at all, rows actually laid, devices that really WRAP (order across a row break is the
	// claim), and strips that really HID something (the tail claim).
	CHECK(devicesChecked >= 4);
	CHECK(rowsSeen >= 12);
	CHECK(wrappedDevices >= 1);
	CHECK(hidingStrips >= 1);
}

