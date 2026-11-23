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

// PIECE 11 OF 12 of the dahdsr suite: its cases in their original order, with the helpers only they use. See dahdsr_common.h.

#include "dahdsr/dahdsr_common.h"

namespace {
/// WHETHER A SOURCE IS LIVE — the declaration of the device under its id, asked of the record.
bool sourceIsLive(const ParamBridge& bridge, backplane::Source s) {
	return backplane::SourceView({}, bridge.devices()).isLive(s);
}
} // namespace

TEST_CASE("EVERY card that declares an enable switch can actually be switched") {
	// FOUND BY SWEEPING RATHER THAN BY REPORT. it was reported "only first effect in chain is
	// processing audio". The engine was cleared by measurement — provision builds every slot, sync
	// hands each device its own row, processStereo visits every slot, a mixed bus/per-voice chain
	// runs both — and so was the second card's reachability. This case is what found something: it
	// asks the same question of EVERY card type instead of the one card every other case drives.
	//
	// WAYPOINT FAILED IT. `hasEnable()` is true because the DEVICE declares an Enabled control, but
	// WaypointCard neither drew the switch nor tested a press against it, and no other surface
	// reaches that control — the expanded view has no toggle either. Enabled defaults to 0, so a
	// Waypoint added to a chain was PERMANENTLY SILENT and could not be turned on by any means.
	//
	// From a player's chair that is indistinguishable from "the device does not process audio",
	// which is exactly how it arrived. A per-card-type sweep is the cheapest instrument for a class
	// of bug whose whole signature is "one of the N does nothing".
	// ASKED OF THE LANE, NOT SPELLED HERE, and that is a repair rather than a tidy-up. This loop used
	// to name four tags — filter, harm, owltt, waypoint — so a device added to `kDevices` afterwards was
	// simply not swept, and the guard went on passing while saying "EVERY card". MEASURED: with the
	// drawn card's enable branch deleted, this case stayed GREEN and only the fold sweep next door
	// caught it, because that one asks `addableForTest()`. The coverage table two thousand lines up
	// states the rule this was breaking — "a table keyed on a list it does not have to cover is a
	// table that stops covering it".
	ModFixture probe;
	auto* probeLane = findInTree<ui::FxLane>(probe.root);
	REQUIRE(probeLane != nullptr);
	std::vector<std::string> enableTags;
	for (const device::Descriptor* d : probeLane->addableForTest()) {
		enableTags.emplace_back(d->traits.tag);
	}
	REQUIRE(enableTags.size() >= 5);

	for (const std::string& tagString : enableTags) {
		const char* tag = tagString.c_str();
		CAPTURE(tag);
		ModFixture f;
		auto* lane = findInTree<ui::FxLane>(f.root);
		REQUIRE(lane != nullptr);
		const size_t second = f.bridge.addFxDevice(tag);
		REQUIRE(second == 1);
		lane->resized();
		lane->scrollCardIntoView(second);
		lane->resized();

		auto* card = lane->cardAt(second);
		REQUIRE(card != nullptr);
		// A CARD THAT DECLARES A SWITCH MUST ANSWER ONE. Skipping here rather than asserting would
		// make the case silently vacuous for any card that stopped declaring it.
		REQUIRE(card->hasEnable());

		const visage::Bounds b = card->enableBounds();
		const visage::Point at =
		    card->positionInWindow()
		    + visage::Point { b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f };
		REQUIRE(f.root->frameAtPoint(at) == card); // the press reaches this card, not through it

		const size_t ec = static_cast<size_t>(card->enableControl());
		const float before0 = f.bridge.fxChain().device[0].row.field[ec];
		const float beforeN = f.bridge.fxChain().device[second].row.field[ec];

		dragThroughRoot(f.root, at, at);
		ParamSet params;
		f.bridge.drain([&](const UiParamEvent& e) {
			if (e.kind == UiParamEvent::Kind::Value) {
				params.setBase(e.id, e.value);
			}
		});
		f.bridge.publish(params);
		f.bridge.refresh();

		const FxChain after = f.bridge.fxChain();
		// IT MOVED ITS OWN DEVICE...
		CHECK(after.device[second].row.field[ec] != beforeN);
		// ...AND NOBODY ELSE'S. Both rows start at 0, so without this the case passes on a card that
		// writes row 0 — the bug filter_card.cpp shipped a week ago.
		CHECK(after.device[0].row.field[ec] == before0);
	}

	// **AND THE STRIP, THE OTHER HOST** (backlog FY, 2026-10-30). "EVERY card" was the LANE's cards: a modulator seated
	// in the strip is built by `ModPanel`, a different host with a different card road, and none was ever pressed.
	// Its own list, each device seated there, and the count held to that list.
	ModFixture stripProbe;
	REQUIRE(stripProbe.panel != nullptr);
	std::vector<std::string> stripTags;
	for (const device::Descriptor* d : stripProbe.panel->addableForTest()) {
		if (d->traits.enableControl >= 0) {
			stripTags.emplace_back(d->traits.tag);
		}
	}
	REQUIRE(stripTags.size() >= 3);
	size_t stripPressed = 0;
	for (const std::string& tagString : stripTags) {
		CAPTURE(tagString);
		ModFixture f;
		const size_t slot = f.bridge.addRackDevice(tagString.c_str());
		REQUIRE(slot < feathers::kMaxRackSlots);
		f.bridge.refresh();
		f.panel->resized();
		const auto* card = dynamic_cast<const ui::FxCard*>(f.panel->cardFrameForTest(slot));
		REQUIRE(card != nullptr);
		REQUIRE(card->hasEnable());
		const visage::Bounds b = card->enableBounds();
		const visage::Point at =
		    card->positionInWindow() + visage::Point { b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f };
		REQUIRE(f.root->frameAtPoint(at) == static_cast<const visage::Frame*>(card));
		const DeviceInstanceId id = f.bridge.rackDeviceAt(slot);
		const size_t ec = static_cast<size_t>(card->enableControl());
		const float before = f.deviceHost.fieldAt(id, ec);
		dragThroughRoot(f.root, at, at);
		ParamSet params;
		f.bridge.drain([&](const UiParamEvent& e) {
			if (e.kind == UiParamEvent::Kind::Value) {
				params.setBase(e.id, e.value);
			}
		});
		f.bridge.publish(params);
		f.bridge.refresh();
		CHECK(f.deviceHost.fieldAt(id, ec) != before);
		++stripPressed;
	}
	CHECK(stripPressed == stripTags.size());
}
TEST_CASE("EVERY card whose device declares an expanded view can actually OPEN it") {
	// THE SWEEP THAT `hasExpandedView` CANNOT BE. That predicate is
	// `traits.hasExpanded && entryFor(desc) != nullptr`, and there is no third term for "and
	// something calls it". The waypoint filter's plan known-badded BOTH halves of it — declared-false
	// with the arm present, declared-true with the arm removed — precisely to avoid *"OWLTT's exact
	// shipped state, where the panel existed, was correctly constructed by its own tests, and was
	// unreachable". Both known-bads went red as intended.
	//
	// AND THE PANEL WAS STILL UNREACHABLE, because `FxLane::ensureCard` wires `onExpand` through an
	// explicit `dynamic_cast` per card type and had no Waypoint arm, and `WaypointCard` had no
	// button to wire. The guard written to prevent OWLTT's blind spot reproduced it one level up.
	//
	// SO THIS ASKS THE ONLY QUESTION THAT CANNOT BE ANSWERED BY A DECLARATION: a press, at real
	// window coordinates, through the real tree, and the LAYER is asked what happened. Driven from
	// the registry rather than from a list typed here, so a fifth device that declares a panel and
	// forgets its button fails here instead of in the user's hands.
	// **BOTH HOSTS, AND COUNTED AGAINST THE REGISTRY** (backlog FY, 2026-10-30). This walked only devices that may sit
	// in the AUDIO chain, so every modulator with a window (the Arp, the Pattern, the MSEG) was never pressed, and
	// `swept >= 3` proved only that the loop ran. A modulator is now seated in the strip, as the editor seats it, and
	// the count is held to EVERY device that declares a view: one that neither host can seat fails by name.
	size_t swept = 0;
	size_t declared = 0;
	for (const device::Descriptor& desc : feathers::kDevices) {
		if (!ui::hasExpandedView(desc)) {
			continue;
		}
		++declared;
		CAPTURE(std::string(desc.traits.name));
		const bool inLane = feathers::maySitInAudioChain(desc);
		const bool inStrip = !inLane && feathers::maySitInModRack(desc) && feathers::belongsInModulationRack(desc);
		REQUIRE((inLane || inStrip)); // a declared view no host can seat is a view nobody can open
		++swept;

		ModFixture f;
		auto* lane = findInTree<ui::FxLane>(f.root);
		REQUIRE(lane != nullptr);
		auto* overlay = f.gui.expandedForTest(); // the EDITOR's overlay — the canvas holds one of its own now, and a tree walk finds that first
		REQUIRE(overlay != nullptr);
		REQUIRE_FALSE(overlay->open());

		ui::FxCard* card = nullptr;
		if (inLane) {
			const size_t slot = f.bridge.addFxDevice(desc.traits.tag);
			REQUIRE(slot == 1);
			lane->resized();
			lane->scrollCardIntoView(slot);
			lane->resized();
			card = lane->cardAt(slot);
		}
		else {
			const size_t slot = f.bridge.addRackDevice(desc.traits.tag);
			REQUIRE(slot < feathers::kMaxRackSlots);
			f.bridge.refresh();
			f.panel->resized();
			card = const_cast<ui::FxCard*>(dynamic_cast<const ui::FxCard*>(f.panel->cardFrameForTest(slot)));
		}
		REQUIRE(card != nullptr);

		// THE BUTTON'S RECTANGLE, ASKED OF THE CARD rather than recomputed from the enable square's
		// neighbours. Restating a geometry in a test is how the test and the drawing start to
		// disagree, and this lane has shipped four "drawn here, grabbable there" bugs.
		//
		// **ONE CALL, WHICH IS WHAT THIS COMMENT USED TO ASK FOR.** It was a cast per card type, over
		// its own note: *"`expandBounds` is on `FxCard` now; this chain predates that and could be one
		// call." It has been hoisted since, and the base answers for every card — returning EMPTY
		// when there is nothing to open, which is the same fall-through the chain gave minus the
		// hand-list.
		//
		// THE HAND-LIST FAILED THE WAY ITS OWN COMMENT PREDICTED, TWICE. It named six card types;
		// Automod fell off the end when it grew a panel, and then the first device to reach a panel
		// through `GenericCard` rather than a bespoke card fell off it again — reported as "declares a
		// panel and cannot open it" when the truth was "this chain has never heard of you". A list of
		// types stops covering the day a seventh arrives.
		const visage::Bounds ex = card->expandBounds();
		REQUIRE(ex.width() > 0.0f);
		// IN THE HEAD AND CLEAR OF ITS NEIGHBOURS. The head has four occupants and the trap these
		// cards already document is a button drawn inside another button's rectangle.
		CHECK(ex.bottom() <= ui::FxCard::kSlotHeadHeight);
		CHECK_FALSE(card->slotMenuBounds().contains({ ex.x() + 1.0f, ex.y() + 1.0f }));
		CHECK_FALSE(card->enableBounds().contains({ ex.x() + 1.0f, ex.y() + 1.0f }));
		// ON THE OVERLAY, which is what this case is about: the canvas is folded FIRST, because the
		// fold re-lays the editor out and a window position taken before it is a press into space.
		(void)f.gui.canvasForTest()->setCollapsed(true);

		const visage::Point on =
		    card->positionInWindow()
		    + visage::Point { ex.x() + ex.width() * 0.5f, ex.y() + ex.height() * 0.5f };
		// NOT VACUOUS: the press has to reach THIS card rather than through it, or every assertion
		// after it is about a click into space.
		REQUIRE(f.root->frameAtPoint(on) == card);

		dragThroughRoot(f.root, on, on);

		REQUIRE(overlay->open());
		const ui::ExpandedView* view = overlay->contentForTest();
		REQUIRE(view != nullptr);
		// AND IT IS THIS DEVICE'S PANEL, not merely some panel. The title comes from the device's own
		// traits, so a lane that opened slot 0's view for a press on slot 1 fails here — which is the
		// bug shape one cast-arm copied from another produces.
		CHECK(view->title() == std::string(desc.traits.name));
	}
	// THE SWEEP IS NOT EMPTY. A filter added to `maySitInAudioChain` that stopped declaring a panel
	// would otherwise leave this case green over nothing.
	CAPTURE(swept);
	CHECK(swept == declared);
	CHECK(declared >= 3);
}
TEST_CASE("EVERY card in the lane folds to its head when the head is pressed") {
	// THE SAME INSTRUMENT AS THE ENABLE SWEEP, aimed at the other affordance every sibling card has
	// and Waypoint did not: it had no `folded_`, no `setFolded`, no `headBounds` and no press branch,
	// so its head answered nothing at all.
	//
	// AND IT ASSERTS THE HEIGHT TWICE, ON PURPOSE. `laneHeight()` shrinking is the card's own answer;
	// `height()` shrinking is the LANE having been told and having re-placed it. The defect was
	// exactly the gap between them — collapsing an audio effect hid its UI without changing the card's
	// size — a fold that hid its controls, called `redraw()`, and repainted at the
	// size it already had. A case asserting only the first would have been green through that bug.
	// A CLICK, NOT A DRAG, AND THAT IS A FINDING RATHER THAN A CONVENIENCE. `dragThroughRoot` sends a
	// move between the press and the release, and a fold MOVES THE CARD OUT FROM UNDER THE POINTER —
	// this lane's cards shift by up to 226px when one of them folds and the scroll re-clamps. The
	// host recomputes a drag event's frame-local position against the frame's CURRENT place
	// (`window_event_handler.cpp:257`), so the next move measures against an anchor taken before the
	// jump, clears the 4px threshold on its own, and picks the card up. MEASURED on all four cards:
	// press the head, send one zero-distance move, and `cardDragging()` is true and the lane is
	// holding a slot.
	//
	// THAT IS A REAL BUG AND IT IS `FxCard`'S, not this case's — it is reported rather than fixed
	// here, because that file is held for another session. What this case must not do is drive it: a
	// fold is a CLICK, and a gesture that also reorders the chain is asking two questions at once.
	const auto clickThroughRoot = [](visage::Frame* root, visage::Point where) {
		visage::Frame* down = root->frameAtPoint(where);
		if (down == nullptr) {
			return;
		}
		const auto eventAt = [&](bool isDown) {
			visage::MouseEvent e;
			e.window_position = where;
			// RE-DERIVED PER EVENT, exactly as the host does it, so a frame that moved between the
			// press and the release is described honestly rather than as it was.
			e.position = where - down->positionInWindow();
			e.event_frame = down;
			e.button_id = visage::kMouseButtonLeft;
			e.button_state = isDown ? visage::kMouseButtonLeft : visage::kMouseButtonNone;
			e.is_down = isDown;
			e.repeat_click_count = 1;
			return e;
		};
		down->processMouseDown(eventAt(true));
		down->processMouseUp(eventAt(false));
	};

	ModFixture probe;
	auto* probeLane = findInTree<ui::FxLane>(probe.root);
	REQUIRE(probeLane != nullptr);
	std::vector<std::string> tags;
	for (const device::Descriptor* d : probeLane->addableForTest()) {
		tags.emplace_back(d->traits.tag);
	}
	REQUIRE(tags.size() >= 4);

	for (const std::string& tag : tags) {
		CAPTURE(tag);
		ModFixture f;
		auto* lane = findInTree<ui::FxLane>(f.root);
		REQUIRE(lane != nullptr);
		const size_t slot = f.bridge.addFxDevice(tag.c_str());
		REQUIRE(slot == 1);
		lane->resized();
		lane->scrollCardIntoView(slot);
		lane->resized();
		ui::FxCard* card = lane->cardAt(slot);
		REQUIRE(card != nullptr);

		const float open = card->laneHeight();
		REQUIRE(open > ui::FxCard::kSlotHeadHeight);
		// THE CARD IS THE SIZE IT SAYS IT IS BEFORE THE PRESS, or "it shrank" below is a claim about
		// a card that was never placed.
		REQUIRE(card->height() == doctest::Approx(open));

		// THE LEFT QUARTER OF THE HEAD, which is head and nothing else: the slot menu, the enable
		// square and the expand button all sit at the right end of it. Aimed through the ONE constant
		// every card's head is measured from rather than through a per-card `headBounds`, which is not
		// on `FxCard`.
		const visage::Point on = card->positionInWindow()
		                         + visage::Point { card->width() * 0.25f,
			                                       ui::FxCard::kSlotHeadHeight * 0.5f };
		REQUIRE(f.root->frameAtPoint(on) == card);

		doubleClickHead(f.root, on);

		CHECK(card->laneHeight() == doctest::Approx(ui::FxCard::kSlotHeadHeight));
		// AND THE LANE ACTED ON IT. This is the half `redraw()` cannot buy.
		CHECK(card->height() == doctest::Approx(ui::FxCard::kSlotHeadHeight));
		// EVERYTHING BELOW THE HEAD IS GONE, not merely undrawn — a control still visible outside the
		// rectangle its card now occupies is a control a hand can reach over whatever is underneath.
		size_t visibleBelowHead = 0;
		for (visage::Frame* child : card->children()) {
			if (child->isVisible() && child->y() >= ui::FxCard::kSlotHeadHeight) {
				++visibleBelowHead;
			}
		}
		CHECK(visibleBelowHead == 0);

		// AND THE HEAD STILL FITS INSIDE THE CARD IT IS NOW THE WHOLE OF. A folded card is its head,
		// so a `foldedHeight()` that disagreed with the head's own rectangle would clip the head's
		// buttons off the bottom of the frame — and every card DRAWS its chrome to `height()`, so the
		// ground would shrink out from under them with nothing to say so.
		//
		// THIS IS THE HONEST HALF OF "does the card draw its background" (the defect: the card's own
		// background was not drawn). The rectangle itself is a RENDER and cannot be
		// read back headlessly — and a case comparing two canvases passes whenever anything differs,
		// which is the failure this whole card is an example of. So what is asserted is the geometry
		// the chrome is drawn FROM, and it is said plainly that the four chrome lines themselves are
		// covered by nothing.
		CHECK(card->slotMenuBounds().bottom() <= card->height());
		if (card->hasEnable()) {
			CHECK(card->enableBounds().bottom() <= card->height());
		}

		// AND THE SWITCH IS STILL REACHABLE, which is the claim that matters and the one geometry
		// alone does not make — a card can leave the square inside its rectangle and still refuse the
		// press. FilterCard's fold documents the rule and nothing asserted it: the square is *"the one
		// thing a fold leaves behind, which is right — it is the only one you need to get the device
		// back". A fold that hid the way back is a device switched out of the signal with no way to
		// switch it in, which is the bug this lane already shipped once by a different road.
		REQUIRE(card->hasEnable());
		const visage::Bounds sw = card->enableBounds();
		const visage::Point onSwitch =
		    card->positionInWindow()
		    + visage::Point { sw.x() + sw.width() * 0.5f, sw.y() + sw.height() * 0.5f };
		REQUIRE(f.root->frameAtPoint(onSwitch) == card);
		const size_t ec = static_cast<size_t>(card->enableControl());
		const float enabledBefore = f.bridge.fxChain().device[slot].row.field[ec];
		clickThroughRoot(f.root, onSwitch);
		ParamSet params;
		f.bridge.drain([&](const UiParamEvent& e) {
			if (e.kind == UiParamEvent::Kind::Value) {
				params.setBase(e.id, e.value);
			}
		});
		f.bridge.publish(params);
		f.bridge.refresh();
		CHECK(f.bridge.fxChain().device[slot].row.field[ec] != enabledBefore);

		// AND IT COMES BACK. A fold that could not be undone would satisfy every assertion above.
		doubleClickHead(f.root, card->positionInWindow()
		                             + visage::Point { card->width() * 0.25f,
			                                           ui::FxCard::kSlotHeadHeight * 0.5f });
		CHECK(card->laneHeight() == doctest::Approx(open));
		CHECK(card->height() == doctest::Approx(open));
	}
}
TEST_CASE("the waypoint card's head carries all four verbs, and a folded one puts its plane away") {
	// THE CARD-SPECIFIC HALF of the two sweeps above: they prove the verbs ACT, this proves the
	// Waypoint's own geometry and its own folded state are what they should be.
	namespace wp = feathers::fx::waypoint;
	namespace flt = feathers::fx::filter;
	FilterFixture f;
	const size_t slot = f.bridge.addFxDevice(wp::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	f.lane->resized();
	auto* card = dynamic_cast<ui::WaypointCard*>(f.lane->cardAt(slot));
	REQUIRE(card != nullptr);
	card->setBounds(0.0f, 0.0f, 240.0f, card->laneHeight());
	card->resized();

	// FOUR DISTINCT WAYPOINTS, or every dot is drawn on top of every other and `dotAt` answers the
	// same one for every point in the plane — which is what a DEFAULTED device does, and it made the
	// first draft of the folded subcase below fail against the fixture rather than against the card.
	const auto write = [&](wp::Param p, double v) {
		ui::FxControl c { f.bridge, f.bridge.fxDeviceAt(slot), wp::kDescriptor, static_cast<size_t>(p) };
		c.setValue(v);
		f.bridge.refresh();
	};
	for (size_t i = 0; i < wp::kWaypoints; ++i) {
		const size_t base = static_cast<size_t>(wp::Param::Cutoff0) + i * 4;
		write(static_cast<wp::Param>(base), std::log2(250.0 * std::pow(2.0, static_cast<double>(i))));
		write(static_cast<wp::Param>(base + 1), 0.1 * static_cast<double>(i + 1));
	}

	SUBCASE("the four occupants of the head do not overlap each other") {
		// SLOT MENU, ENABLE SQUARE, EXPAND BUTTON, and the head itself. Each of the first three is
		// inside the head's rectangle, which is why the press order is load-bearing and why this asks
		// for their rectangles rather than trusting the order alone.
		const visage::Bounds menu = card->slotMenuBounds();
		const visage::Bounds sw = card->enableBounds();
		const visage::Bounds ex = card->expandBounds();
		REQUIRE(sw.width() > 0.0f);
		REQUIRE(ex.width() > 0.0f);
		for (const visage::Bounds& b : { menu, sw, ex }) {
			CHECK(b.bottom() <= ui::FxCard::kSlotHeadHeight);
			CHECK(card->headBounds().contains({ b.x() + 1.0f, b.y() + 1.0f }));
		}
		// LEFT TO RIGHT: expand, then the square, then the dots. Asserted as an ORDER rather than as
		// three numbers, so the head can be re-spaced without editing this.
		CHECK(ex.right() <= sw.x());
		CHECK(sw.right() <= menu.x());
	}

	SUBCASE("the expand button is empty for a build that cannot construct the panel") {
		// THE HONESTY RULE IN ITS GEOMETRY FORM: `expandBounds` is derived from `hasExpandedView`, so
		// a button cannot be drawn a moment before the panel it promises exists. Both halves of that
		// conjunction hold for this device today, which is what makes the rectangle non-empty above.
		CHECK(wp::kTraits.hasExpanded);
		CHECK(ui::hasExpandedView(wp::kDescriptor));
	}

	SUBCASE("a folded card is its head: every knob is away and the plane refuses a press") {
		// WHERE A DOT IS WHILE THE CARD IS OPEN — captured first, because `planeBounds` is pure
		// geometry and goes on answering a rectangle after the card is 24px tall.
		const visage::Point dot = card->dotCentre(2);
		REQUIRE(card->dotAt(dot) == 2);
		const auto cutoffOf = [&] {
			ui::FxControl c { f.bridge, f.bridge.fxDeviceAt(slot), wp::kDescriptor,
				              static_cast<size_t>(wp::Param::Cutoff0) + 2 * 4 };
			return c.value();
		};
		const double before = cutoffOf();

		card->setFolded(true);
		CHECK(card->folded());
		CHECK(card->laneHeight() == doctest::Approx(ui::WaypointCard::foldedHeight()));
		for (const auto& knob : card->knobsForTest()) {
			CHECK_FALSE(knob->isVisible());
		}

		// AND THE PRESS AT THE DOT'S REMEMBERED PLACE DOES NOTHING. A hit test that still answered
		// there would hand a drag to a control drawn nowhere — which is the same class of bug as a
		// button under another button, from the other side.
		card->onMouseDown().callback(at(dot.x, dot.y));
		const visage::Bounds box = card->planeBounds();
		card->onMouseDrag().callback(at(box.x() + box.width() * 0.9f, box.y() + 2.0f));
		card->onMouseUp().callback(at(box.x() + box.width() * 0.9f, box.y() + 2.0f));
		f.bridge.refresh();
		CHECK(cutoffOf() == doctest::Approx(before));

		// UNFOLDED, THE SELECTED WAYPOINT'S PAIR COMES BACK AND THE OTHER SIX STAY AWAY. Fold and
		// selection are two questions about one knob's visibility, and answering them in two places is
		// how a fold-then-unfold leaves all eight macro knobs on screen.
		card->select(3);
		card->setFolded(false);
		CHECK_FALSE(card->folded());
		size_t visible = 0;
		for (const auto& knob : card->knobsForTest()) {
			visible += knob->isVisible() ? 1 : 0;
		}
		// THE THREE DEVICE-WIDE CHOICES ARE NOT KNOBS (a box and two chips), and are cells of the same layout.
		visible += card->typeForTest()->isVisible() ? 1 : 0;
		visible += card->keytrackForTest()->isVisible() ? 1 : 0;
		visible += card->polyForTest()->isVisible() ? 1 : 0;
		CHECK(visible == ui::WaypointCard::kKnobCells);
	}

	SUBCASE("a drag that takes a dot does not also pick the card out of the lane") {
		// ONE GESTURE, ONE MEANING. The card draws its plane itself, so a press on a dot is a press on
		// the CARD — and the base's reorder drag arms from `noteCardPress`, which this card called
		// unconditionally in its first draft. That would have moved a waypoint and picked the card out
		// of the lane with the same hand.
		//
		// OWLTT DOES NOT NEED THIS GATE and its comment says why: its plot is a child frame and consumes
		// its own presses, so a press that reaches its card handler was never on the graph. This card
		// has no such frame, so the gate is the card's to apply.
		const size_t target = 1;
		const visage::Point from = card->dotCentre(target);
		REQUIRE(card->dotAt(from) == target);
		const auto cutoffOf = [&] {
			ui::FxControl c { f.bridge, f.bridge.fxDeviceAt(slot), wp::kDescriptor,
				              static_cast<size_t>(wp::Param::Cutoff0) + target * 4 };
			return c.value();
		};
		const double before = cutoffOf();

		const visage::Bounds box = card->planeBounds();
		const visage::Point to { box.x() + box.width() * 0.85f,
			                     from.y + ui::FxCard::kDragThreshold * 4.0f };
		card->onMouseDown().callback(at(from.x, from.y));
		card->onMouseDrag().callback(at(to.x, to.y));
		CHECK_FALSE(card->cardDragging());
		CHECK_FALSE(f.lane->dragSlot().has_value());
		card->onMouseUp().callback(at(to.x, to.y));
		CHECK_FALSE(f.lane->dragSlot().has_value());

		// AND THE DOT DID MOVE — the drag is well past the card's own threshold, so without this the
		// assertions above would be satisfied by a gesture that did nothing at all.
		f.bridge.refresh();
		CHECK(cutoffOf() != doctest::Approx(before));
	}

	SUBCASE("the folded head says which filter this Waypoint is, clear of the buttons") {
		// A FOLDED CARD IS A HEAD AND ONE FACT. The fact is the TYPE, because the design rule —
		// the types are constant for all morph points — is what makes one name honest for a device
		// holding four states.
		card->setFolded(true);
		// THE LABEL'S BOX ENDS AT THE LEFTMOST BUTTON, which is the expand button and NOT the enable
		// square. Handing `foldedLabelBounds` the square would print the type name straight over the
		// button — the filter card's old defect, reintroduced by the fix's own mechanism going
		// unused, and it is invisible to every assertion that does not do this arithmetic.
		const visage::Bounds label = card->foldedLabelBounds(card->expandBounds().x());
		CHECK(label.right() <= card->expandBounds().x());
		CHECK(label.right() <= card->enableBounds().x());
		CHECK(label.width() > 0.0f);
		// AND THERE IS A NAME TO PRINT for whatever type the device is set to — an index past the end
		// of `kTypeNames` is a blank head rather than a crash, so it is asserted rather than trusted.
		CHECK(static_cast<size_t>(flt::Type::kCount) > 0);
	}
}
TEST_CASE("EVERY card in the lane can be picked up and dragged to reorder") {
	// THE THIRD SWEEP, AND THE THIRD THING IT FOUND. Cards reorder by drag, not only through 'move
	// earlier', and `FxCard` implements it once for every
	// card: the base's constructor installs `onMouseDrag` and `onMouseUp`, each card calls
	// `noteCardPress` from its own `onMouseDown`, and the lane binds `onCardDrag`.
	//
	// EXCEPT THAT `visage::CallbackList::operator=` IS `set()`, WHICH CLEARS THE LIST. A card that
	// writes `onMouseDrag() = ...` in its own constructor does not add a handler, it REPLACES the
	// base's — so the card reported no drag, the lane never learned a slot had been picked up, and
	// the only way left to reorder that device was the slot menu. WaypointCard was the only card that
	// wrote to those two lists, and it is the only one that could not be dragged.
	//
	// AND THE ONE EXISTING CASE FOR THIS GESTURE COULD NOT SEE IT, because it drives a HARM — a card
	// with no drag of its own, so the question it asks is answered by the base every time. Third
	// instance this week of the same shape: a bug whose whole signature is "one of the N does
	// nothing" is nearly invisible to a case that drives one of the N.
	ModFixture probe;
	auto* probeLane = findInTree<ui::FxLane>(probe.root);
	REQUIRE(probeLane != nullptr);
	std::vector<std::string> tags;
	for (const device::Descriptor* d : probeLane->addableForTest()) {
		tags.emplace_back(d->traits.tag);
	}
	REQUIRE(tags.size() >= 4);

	for (const std::string& tag : tags) {
		CAPTURE(tag);
		ModFixture f;
		auto* lane = findInTree<ui::FxLane>(f.root);
		REQUIRE(lane != nullptr);
		const size_t slot = f.bridge.addFxDevice(tag.c_str());
		REQUIRE(slot == 1);
		lane->resized();
		ui::FxCard* card = lane->cardAt(slot);
		REQUIRE(card != nullptr);

		// THE CARD'S OWN HANDLERS, in the card's own coordinates. Driving this through the root would
		// fold the card on the press and move it out from under the pointer, which is a different
		// question — see the note in the fold sweep about what that costs.
		const float head = ui::FxCard::kSlotHeadHeight * 0.5f;
		// ARMED WITH A DOUBLE-CLICK — a single press neither folds nor drags now.
		card->onMouseDown().callback(doubleAt(20.0f, head));
		// A NUDGE UNDER THE THRESHOLD IS STILL A CLICK, which is what keeps the fold working.
		card->onMouseDrag().callback(at(20.0f, head + ui::FxCard::kDragThreshold * 0.5f));
		CHECK_FALSE(card->cardDragging());
		CHECK_FALSE(lane->dragSlot().has_value());

		// PAST IT, and the card is travelling — through the callback the LANE installed, which is the
		// hop a card that replaced the base's handler silently removes.
		card->onMouseDrag().callback(at(20.0f, head + ui::FxCard::kDragThreshold * 3.0f));
		CHECK(card->cardDragging());
		REQUIRE(lane->dragSlot().has_value());
		CHECK(*lane->dragSlot() == slot);

		// AND THE RELEASE LETS GO. A card that could be picked up and never put down would leave the
		// lane holding a slot for the rest of the session.
		card->onMouseUp().callback(at(20.0f, head + ui::FxCard::kDragThreshold * 3.0f));
		CHECK_FALSE(card->cardDragging());
		CHECK_FALSE(lane->dragSlot().has_value());
	}
}
TEST_CASE("a card that MOVES under a live press is not thereby being dragged") {
	// THE BUG: `e.position` is recomputed by the host against the frame's CURRENT place every move
	// (visage window_event_handler.cpp:257). So anything that moves a card under a held button
	// shifts `e.position` by that distance with no hand movement at all, and FxCard's 4px threshold
	// compared it against an anchor taken when the card was somewhere else. Folding a card does
	// exactly this — the lane re-places everything below it and the scroll re-clamps — so a click
	// that never moved could pick the card up and REORDER THE CHAIN.
	//
	// WRITTEN AGAINST THE MECHANISM RATHER THAN THE SCENARIO, deliberately. Reproducing it through a
	// real fold needs a card the fold actually displaces, which depends on how many cards the lane
	// holds and where the scroll happens to be; a first card folds and moves nothing, and the case
	// passes while the bug is fully present. Moving the frame directly asks the one question that
	// matters and cannot be defeated by geometry.
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	auto* card = lane->cardAt(0);
	REQUIRE(card != nullptr);

	const visage::Point origin = card->positionInWindow();
	const visage::Point pressAt { 20.0f, 8.0f }; // card-local, in the head

	visage::MouseEvent down {};
	down.position = pressAt;
	down.window_position = origin + pressAt;
	down.button_state = visage::kMouseButtonLeft;
	// ARMED, because this case is about a LAYOUT shift masquerading as hand movement — it has to get
	// as far as a drag being possible for the distinction to mean anything. A single press would make
	// it pass for the wrong reason: nothing drags on a single press now.
	down.repeat_click_count = 2;
	card->processMouseDown(down);
	REQUIRE_FALSE(card->cardDragging());

	// THE CARD MOVES UP UNDER THE HELD BUTTON — what a fold above it does. Far past the 4px
	// threshold, so a card comparing raw positions arms instantly.
	const float carried = 226.0f;
	card->setBounds(card->x(), card->y() - static_cast<int>(carried), card->width(), card->height());

	// AND THE HAND HAS NOT MOVED: same window position, so the host hands over a card-local
	// position that is `carried` larger.
	visage::MouseEvent move {};
	move.window_position = origin + pressAt;
	move.position = pressAt + visage::Point { 0.0f, carried };
	move.button_state = visage::kMouseButtonLeft;
	card->processMouseDrag(move);

	// NOT A DRAG. The hand did not move; only the layout did.
	CHECK_FALSE(card->cardDragging());
	CHECK_FALSE(lane->dragSlot().has_value());

	// AND THE CARD IS STILL DRAGGABLE — without this the case passes on a card whose drag is simply
	// broken, which is how Waypoint was accidentally immune to this bug until its drag was fixed.
	visage::MouseEvent real {};
	real.window_position = origin + pressAt + visage::Point { 0.0f, 40.0f };
	real.position = pressAt + visage::Point { 0.0f, carried + 40.0f };
	real.button_state = visage::kMouseButtonLeft;
	card->processMouseDrag(real);
	CHECK(card->cardDragging());
}
// RECOVERED FROM AN ABANDONED WORKTREE, 2026-08-28. The `owltt` worktree sat 139 commits behind with
// this case uncommitted and no live process. The
// SOURCE half of that work — dropping the default from `ValueBox::mappable(bool)` so no call site
// can silently forget the modifier — had already landed on mainline independently. This case had
// not, and it is the ONLY coverage of ValueBox's Alt bypass: the existing "holding Alt edits the
// parameter underneath" case drives a Knob and never touches a ValueBox.
TEST_CASE("holding Alt on a ValueBox edits the value directly, not the amount") {
	// THE ADDENDUM'S ACCEPTANCE CRITERION: on an env card with a source armed that may reach it,
	// hold Alt and drag a box → the box's VALUE moved, and its amount did not.
	//
	// This is the Alt bypass parity with Knob — when Alt is held, a drag edits the value directly
	// instead of creating a route. Without the bypass, every value edit in a mapping session costs
	// two clicks somewhere else.
	ModFixture f;
	feathers::ui::ModState mod;
	namespace dsr = feathers::mod::dahdsr;

	ui::RackControl attack { f.bridge, f.bridge.rackDeviceAt(0), dsr::Param::Attack, &mod };
	ui::ValueBox box { attack, "A" };
	box.setBounds(0, 0, 60, 34);

	// ARM A SOURCE THAT MAY REACH THIS DESTINATION.
	mod.focus(armingSource());
	mod.arm(true);
	CHECK(attack.display().routable == true); // The gate genuinely opens.

	// RECORD the starting value and amount.
	const double valueBefore = attack.value();
	const float amountBefore = attack.display().amount;

	// ADD THE BOX TO A PARENT FRAME so it has a real position in the window.
	visage::Frame container;
	container.setBounds(100, 100, 200, 100);
	container.addChild(&box);
	// DRAG WITH ALT HELD — send events directly to the box with the Alt modifier set.
	// THE BOX'S CENTER in window coordinates.
	const visage::Point box_pos = box.positionInWindow();
	visage::Point centre = {box_pos.x + box.width() * 0.5f, box_pos.y + box.height() * 0.5f};
	const auto eventAt = [&](visage::Point windowPosition, bool isDown) {
		visage::MouseEvent e;
		e.window_position = windowPosition;
		e.position = windowPosition - box_pos;
		e.event_frame = &box;
		e.button_id = visage::kMouseButtonLeft;
		e.button_state = isDown ? visage::kMouseButtonLeft : visage::kMouseButtonNone;
		e.is_down = isDown;
		e.repeat_click_count = 1;
		e.modifiers = visage::kModifierAlt; // ALT HELD THROUGHOUT
		return e;
	};
	// ALT + drag downward (increases value). Use a large enough travel to actually move the value —
	// kDragRange is 140px for the full range, so 70px moves ~50% of the range.
	box.processMouseDown(eventAt(centre, true));
	box.processMouseDrag(eventAt({centre.x, centre.y + 70.0f}, true));
	box.processMouseUp(eventAt({centre.x, centre.y + 70.0f}, false));

	// ASSERT BOTH HALVES: the value moved AND the amount did not.
	CHECK(attack.value() != doctest::Approx(valueBefore)); // Value moved.
	CHECK(attack.display().amount == doctest::Approx(amountBefore)); // Amount unchanged.
}
TEST_CASE("a card destroyed in the fx lane leaves nothing behind on the MODULATION watcher list") {
	// The reproduction: add an MSEG, delete the filter, add Drawn, try to assign the MSEG — a crash as
	// soon as the assign is clicked. Reproduced, and it is a use-after-free.
	//
	// THERE ARE TWO WATCHER LISTS AND A CARD IS ON BOTH. `FxLane` holds a `FrameWatchers*` for the
	// state road; `Knob::setModulation` separately puts every knob on `ModState`'s list so the glow
	// follows arming. Destroying a card cleaned the first and not the second, so the deleted
	// filter's knobs stayed on the modulation list as freed pointers, and the next
	// `ModState::focus` — which is what clicking a modulator's arrow does — walked them.
	//
	// IT SURVIVED THREE NOTIFICATIONS BEFORE DYING, which is why it looked unrelated to the delete.
	// A dangling read is undefined rather than reliably fatal: the crash landed on whichever gesture
	// came after the memory was REUSED, and adding a device is what reuses it. That is also why this
	// case does the whole sequence rather than just deleting and notifying — a shorter case passes
	// against the bug.
	ModFixture f;
	auto* panel = findInTree<ui::ModPanel>(f.root);
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(panel != nullptr);
	REQUIRE(lane != nullptr);
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);

	const size_t mseg = f.bridge.addRackDevice(feathers::mod::mseg::kTraits.tag);
	f.bridge.refresh();
	panel->resized();

	REQUIRE(lane->removeDeviceAtLane(0)); // the filter
	f.bridge.refresh();
	lane->resized();

	// THE ALLOCATION THAT REUSES THE FREED KNOBS. Without it the stale pointers are still there and
	// still read as valid, so this line is load-bearing rather than scene-setting.
	f.bridge.addFxDevice(fx::drawn::kTraits.tag);
	f.bridge.refresh();
	lane->resized();

	// AND THE PRESS. `focus` notifies the modulation list, which is where the freed frames were.
	const backplane::Source source = backplane::Source::output(f.bridge.rackDeviceAt(mseg), 0);
	visage::Point at { -1.0f, -1.0f };
	for (float y = 1.0f; y < panel->height() && at.x < 0.0f; y += 2.0f) {
		for (float x = 1.0f; x < panel->width(); x += 2.0f) {
			const ui::ModPanel::Hit h = panel->hitAt({ x, y });
			if (h.part == ui::ModPanel::Part::Arrow && h.source == source) { at = { x, y }; break; }
		}
	}
	REQUIRE(at.x >= 0.0f); // the arrow exists, so the press below is the real gesture
	dragThroughRoot(f.root, panel->positionInWindow() + at, panel->positionInWindow() + at);

	// SURVIVING IS THE ASSERTION — a use-after-free has no value to check. What CAN be checked is
	// that the press did its job, so the case cannot pass by the press having reached nothing.
	CHECK(mod->source() == source);
	CHECK(mod->armed());
}
TEST_CASE("a modulator's arrow arms ITS OWN output after the rack has been shortened or reordered") {
	// The symptoms: modulation routes not being deleted, and apparently phantom modulation devices
	// still running after deletion, hard to describe precisely. Both halves are this one fault, and
	// "hard to describe" is the fault's own
	// shape: it needs TWO MODULATORS OF THE SAME KIND and it leaves the engine perfectly correct.
	//
	// **THE ENGINE IS NOT WHERE IT GOES WRONG.** The removal drops the departing device's routes and
	// leaves the survivor's standing on the survivor — measured below and asserted first, so this case
	// cannot pass by the rack having quietly broken in some other way. (The mechanism underneath that
	// sentence has been rebuilt twice since: it was "clears the departing LANE's columns and every
	// surviving device keeps the lane that travels in its row", then routes by endpoint at step 2, and
	// since kill-lanes step 3c there is no lane at all. What is asserted has not moved — the survivor,
	// by id, still has its route.)
	//
	// WHAT GOES WRONG IS THE ARM'S IDENTITY. `ensureOutputArm` baked a SOURCE COORDINATE, minted from
	// the slot, into the widget when it built it, and `pruneOutputArms` decided staleness from the
	// SLOT, the OUTPUT ORDINAL and the LABEL. A removal or a reorder shifts rows, so the device under a
	// slot changes without any of the three changing — and two DAHDSRs both call their output
	// "Envelope", which is what closes the last gap. The arm survives, and it goes on arming whatever
	// it was born pointing at. **That is exactly the shape kill-lanes exists to make unstateable**: an
	// arm names its device by id now, and the case asks in those terms below.
	//
	// WHAT A PLAYER SEES, and it is why this is not a cosmetic bug: the surviving modulator's real
	// route draws no ring, so it cannot be found; dragging that ring to nothing writes to the DEAD
	// lane and leaves the real amount standing, so the route CANNOT BE DELETED and goes on
	// modulating; and a fresh assignment from that card is refused outright by `maySourceRouteTo`,
	// because the source it names has no device behind it.
	//
	// THE ASSERTIONS ARE VALUES — the armed source index and the route table's own cells — never a
	// comparison between two paints. A ring is drawn from an amount, and an amount read back out of
	// a picture is the measurement CLAUDE.md's rule forbids.
	//
	// **TWO TEST CASES RATHER THAN TWO SUBCASES, and that is the known-bad discipline rather than
	// taste.** A `REQUIRE` aborts the whole TEST_CASE, so a subcase that goes fatal takes every
	// subcase after it with it — and against the unfixed source the first one DOES go fatal, at
	// `path->mappable()`. The reorder half would then have been reported as neither passed nor
	// failed by a run that never executed it, which is exactly the short-tally failure CLAUDE.md's
	// `--each` rule exists to prevent, reappearing one level down.
	namespace dsr = feathers::mod::dahdsr;

	{
		ModFixture f;
		auto* panel = findInTree<ui::ModPanel>(f.root);
		REQUIRE(panel != nullptr);
		ui::ModState* mod = f.gui.modForTest();
		REQUIRE(mod != nullptr);

		// TWO OF THE SAME KIND, which is the arrangement the label test cannot see through. A DAHDSR
		// beside an MSEG is caught today, and that is why this looked intermittent.
		REQUIRE(f.bridge.addRackDevice(dsr::kTraits.tag) == 1);
		f.bridge.refresh();
		panel->resized();
		REQUIRE(panel->deviceCount() == 2);

		const DeviceInstanceId idA = f.bridge.rackDeviceAt(0);
		const DeviceInstanceId idB = f.bridge.rackDeviceAt(1);
		REQUIRE(idA != idB);
		const backplane::Source srcA = backplane::Source::output(idA, 0);
		const backplane::Source srcB = backplane::Source::output(idB, 0);
		const backplane::Destination dest =
		    backplane::Destination::core(static_cast<size_t>(feathers::modDestinationIndexOf(ParamId::Path)));

		// THE SURVIVOR IS ROUTED BEFORE THE REMOVAL — the state a hand would be in.
		f.bridge.setRouteAmount(srcB, dest, 0.5f);
		REQUIRE(f.bridge.routeAmount(srcB, dest) == doctest::Approx(0.5f));

		REQUIRE(panel->removeDeviceAtLane(0));
		f.bridge.refresh();
		panel->resized();

		// THE ENGINE FIRST, so a red below is about the arm and not about the rack.
		REQUIRE(panel->deviceCount() == 1);
		CHECK(f.bridge.rackDeviceAt(0) == idB);
		CHECK(f.bridge.routeAmount(srcA, dest) == doctest::Approx(0.0f));
		CHECK(f.bridge.routeAmount(srcB, dest) == doctest::Approx(0.5f));

		// AND THE ARROW, PRESSED WHERE THE PANEL DRAWS IT, through the real tree. The panel's own hit
		// test already answers `srcB` for this rectangle — REQUIREd, so the case cannot pass by the
		// geometry having moved instead.
		const visage::Bounds chip = panel->outputChipBounds(srcB);
		REQUIRE(chip.width() > 0.0f);
		const visage::Point at { chip.x() + 4.0f, chip.y() + chip.height() * 0.5f };
		REQUIRE(panel->hitAt(at).source == srcB);
		const visage::Point inWindow = panel->positionInWindow() + at;
		dragThroughRoot(f.root, inWindow, inWindow);
		CHECK(mod->armed());
		CHECK(mod->source() == srcB);

		// AND THE ROUTE IS REACHABLE AGAIN, which is the half it was reported. The ring draws the
		// amount that is really there, and dragging it to nothing really deletes it.
		ui::Knob* path = f.knobFor(ParamId::Path);
		REQUIRE(path != nullptr);
		CHECK(path->modRing().drawn);
		// THE GESTURE THAT DELETES A ROUTE — a double press on an armed destination, which is
		// `Knob::unroute`. Chosen over dragging the ring down because a drag has a distance and this
		// has an outcome: the question is which CELL the verb lands on, not how far a hand moved.
		REQUIRE(path->mappable());
		const visage::Point knobAt = f.faceCentre(path);
		path->onMouseDown().callback(doubleAt(knobAt.x, knobAt.y));
		CHECK(f.bridge.routeAmount(srcB, dest) == doctest::Approx(0.0f));
		// AND IT TOOK THE SURVIVOR'S ROUTE RATHER THAN ANY OTHER — the departed device's was already
		// gone, so this second reading is what says the first is not vacuous.
		CHECK(f.bridge.routeAmount(srcA, dest) == doctest::Approx(0.0f));
	}
}
TEST_CASE("a modulator's arrow arms ITS OWN output after the rack has been REORDERED") {
	// THE SAME FAULT WITHOUT A DELETE IN IT, which is worth its own case because it says the
	// mechanism is the SHIFT and not the removal. A rack whose two envelopes have been put in the
	// order the player thinks about them in has both arrows pointing at each other's lanes — no
	// device has to be destroyed for a route to become unreachable.
	//
	// See the removal case above for the mechanism and for what a player sees.
	namespace dsr = feathers::mod::dahdsr;

	{
		ModFixture f;
		auto* panel = findInTree<ui::ModPanel>(f.root);
		REQUIRE(panel != nullptr);
		ui::ModState* mod = f.gui.modForTest();
		REQUIRE(mod != nullptr);

		REQUIRE(f.bridge.addRackDevice(dsr::kTraits.tag) == 1);
		f.bridge.refresh();
		panel->resized();

		const DeviceInstanceId idA = f.bridge.rackDeviceAt(0);
		const DeviceInstanceId idB = f.bridge.rackDeviceAt(1);
		REQUIRE(idA != idB);

		REQUIRE(panel->slotChoiceForTest(0, ui::ModPanel::SlotVerb::MoveDown));
		f.bridge.refresh();
		panel->resized();
		// THE DEVICES REALLY SWAPPED — the premise the rest of this rests on.
		REQUIRE(f.bridge.rackDeviceAt(0) == idB);
		REQUIRE(f.bridge.rackDeviceAt(1) == idA);

		// BOTH ARROWS, not one: an arm that merely swapped would satisfy half of this.
		for (size_t slot = 0; slot < 2; ++slot) {
			CAPTURE(slot);
			const backplane::Source want = backplane::Source::output(f.bridge.rackDeviceAt(slot), 0);
			const visage::Bounds chip = panel->outputChipBounds(want);
			REQUIRE(chip.width() > 0.0f);
			const visage::Point at { chip.x() + 4.0f, chip.y() + chip.height() * 0.5f };
			REQUIRE(panel->hitAt(at).source == want);
			const visage::Point inWindow = panel->positionInWindow() + at;
			dragThroughRoot(f.root, inWindow, inWindow);
			CHECK(mod->source() == want);
		}
	}
}
TEST_CASE("deleting the ARMED modulator disarms, rather than leaving the arrow pointed at a ghost") {
	// **THE SECOND STALE IDENTITY, AND IT IS NOT THE ARM'S.** `ModState::source_` is never revalidated
	// against liveness, so removing the modulator that is currently armed left the surface saying
	// ARMED with its source naming a lane no device holds. Measured before this fix: `armed()` true,
	// `source()` 44, and the lane behind 44 empty.
	//
	// **WHAT IT COST WAS NOT A WRONG WRITE.** The honesty gate downstream holds — `maySourceRouteTo`
	// refuses a source with no device behind it, so the knob reported `mappable() == false` and a drag
	// wrote nothing to any cell. What the player got was an INERT ARMED STATE: the editor claiming a
	// gesture is available and then doing nothing, with no explanation, which is the honesty rule's
	// own failure case wearing the opposite sign.
	//
	// **DISARM RATHER THAN RE-POINT** (decided 2026-09-05). The obvious repair — fall the
	// focus back to macro 0, or to the nearest live device source — ARMS A SOURCE THE HAND DID NOT
	// CHOOSE, and the next click would route to it. `ModState` deliberately has no "no source" state
	// because the rings must be about something, but ARMED is a separate flag and `arm(false)` is a
	// real call. So the source is left pointing wherever it pointed, harmlessly, and nothing is armed
	// until a hand arms it.
	namespace dsr = feathers::mod::dahdsr;

	ModFixture f;
	auto* panel = findInTree<ui::ModPanel>(f.root);
	REQUIRE(panel != nullptr);
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);

	REQUIRE(f.bridge.addRackDevice(dsr::kTraits.tag) == 1);
	f.bridge.refresh();
	panel->resized();
	REQUIRE(panel->deviceCount() == 2);

	const backplane::Source doomed = backplane::Source::output(f.bridge.rackDeviceAt(0), 0);
	f.arm(doomed);
	REQUIRE(mod->armed());
	REQUIRE(mod->source() == doomed);

	REQUIRE(panel->removeDeviceAtLane(0));
	f.bridge.refresh();
	panel->resized();

	// THE SOURCE IS REALLY GONE — asserted rather than assumed, so a red below cannot be a case that
	// deleted the wrong thing.
	REQUIRE_FALSE(sourceIsLive(f.bridge, doomed));
	CHECK_FALSE(mod->armed());
}
TEST_CASE("but deleting somebody ELSE's modulator leaves the armed one armed") {
	// **THE HALF THAT MAKES THE OTHER CASE MEAN SOMETHING.** A panel that simply disarmed on every
	// layout would satisfy the case above and would destroy the mock's sticky mapping — *"mapping
	// stays STICKY — route several in one session". So this asks the opposite question of the same
	// road: a removal that does NOT touch the armed device must leave the arming alone.
	//
	// AND IT IS THE REORDER-SURVIVOR ARRANGEMENT AGAIN: the armed device is at slot 1 holding lane 9,
	// slot 0 goes, and the survivor slides to slot 0 KEEPING lane 9 — so its source is untouched and
	// there is nothing to disarm.
	namespace dsr = feathers::mod::dahdsr;

	ModFixture f;
	auto* panel = findInTree<ui::ModPanel>(f.root);
	REQUIRE(panel != nullptr);
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);

	REQUIRE(f.bridge.addRackDevice(dsr::kTraits.tag) == 1);
	f.bridge.refresh();
	panel->resized();

	const backplane::Source keeper = backplane::Source::output(f.bridge.rackDeviceAt(1), 0);
	f.arm(keeper);
	REQUIRE(mod->armed());
	REQUIRE(mod->source() == keeper);

	REQUIRE(panel->removeDeviceAtLane(0));
	f.bridge.refresh();
	panel->resized();

	// STILL LIVE, because the lane travelled with the row that survived.
	REQUIRE(sourceIsLive(f.bridge, keeper));
	CHECK(mod->armed());
	CHECK(mod->source() == keeper);
}
TEST_CASE("a layout with every device still present does not disarm — assign mode is sticky") {
	// **THE CASE THE OTHER TWO DO NOT COVER, AND I CHECKED RATHER THAN ASSUMED.** The control beside
	// `disarmIfArmedSourceIsGone`'s own case deletes SOMEBODY ELSE's modulator, so it proves the
	// arming survives a layout in which the armed device is present AND another was removed. It does
	// not ask the plainer question: does a layout with NOTHING removed leave the arming alone?
	//
	// **IT MATTERS BECAUSE THERE ARE NOW TWO WRITERS OF `arm(false)`.** `esc-exits-assign` (merged at
	// `0b04800`) put one at the editor root — `Impl::leaveAssignMode`, reached by Escape — and this
	// branch put one on the LAYOUT road. Nothing makes them conflict textually, and the reasoning that
	// they cannot conflict behaviourally is that this one fires only when the armed source's DEVICE is
	// gone. That reasoning is correct and it is still only reasoning; a layout happens for many
	// reasons that have nothing to do with a removal — a resize, a scroll, a card rebuilt — and assign
	// mode is deliberately STICKY across all of them ("mapping stays STICKY — route several in one
	// session").
	//
	// So this drives layouts with the rack untouched and requires the arming to survive every one.
	namespace dsr = feathers::mod::dahdsr;

	ModFixture f;
	auto* panel = findInTree<ui::ModPanel>(f.root);
	REQUIRE(panel != nullptr);
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);

	REQUIRE(f.bridge.addRackDevice(dsr::kTraits.tag) == 1);
	f.bridge.refresh();
	panel->resized();
	REQUIRE(panel->deviceCount() == 2);

	const backplane::Source armed = backplane::Source::output(f.bridge.rackDeviceAt(0), 0);
	f.arm(armed);
	REQUIRE(mod->armed());
	REQUIRE(mod->source() == armed);

	// SEVERAL LAYOUTS, NOT ONE — a disarm that fired on the second visit rather than the first would
	// survive a single call, and `resized()` is reached repeatedly in normal use.
	for (int pass = 0; pass < 3; ++pass) {
		CAPTURE(pass);
		panel->resized();
		CHECK(mod->armed());
		CHECK(mod->source() == armed);
	}

	// AND A SCROLL, which reaches `resized()` by its own road rather than by this case calling it —
	// so the claim is about the layout ROAD and not about one function a test happened to invoke.
	panel->setScroll(4.0f);
	CHECK(mod->armed());
	CHECK(mod->source() == armed);

	// THE RACK IS UNTOUCHED THROUGHOUT, which is the premise the whole case rests on and is therefore
	// asserted rather than assumed.
	CHECK(panel->deviceCount() == 2);
	CHECK(sourceIsLive(f.bridge, armed));
}
TEST_CASE("a fold belongs to its DEVICE: reordering the strip does not move which card is folded") {
	// **A POSITION IS NOT AN IDENTITY (backlog DX.3).** The fold was remembered against the head
	// source's flat index — a number derived from a routing lane — so dragging one modulator past
	// another handed the fold to whichever card arrived at the folded one's number. The strip holds a
	// set of folded DEVICE IDS now, which is what makes this true; the case is here because nothing
	// pinned it, and an unpinned property is one edit from going back.
	//
	// **THE SET MOVED HOUSE, 2026-09-12, AND ITS KEY DID NOT.** It was `ModPanel::minimised_`, and beside
	// it a desk preference keyed by POSITION tried to make the same fold survive a restart — two roads
	// writing one fact, and the desk's road was the wrong one for the reason this case's title gives.
	// The decision: the fold lives in the patch. The set is `rack::DeviceViews` on the bridge now and the panel asks
	// it; `modulatorCardKey` and its preference are gone. WITHIN a session this is still the claim, and
	// "a fold belongs to its DEVICE across a save" is the case below.
	namespace dsr = feathers::mod::dahdsr;
	namespace mseg = feathers::mod::mseg;

	ModFixture f;
	auto* panel = findInTree<ui::ModPanel>(f.root);
	REQUIRE(panel != nullptr);

	// TWO MODULATORS OF DIFFERENT KINDS, so "the fold moved" and "the fold stayed" are distinguishable
	// by the device rather than by a number that both of them wear in turn.
	REQUIRE(f.bridge.addRackDevice(mseg::kTraits.tag) == 1);
	f.bridge.refresh();
	panel->resized();
	REQUIRE(panel->deviceCount() == 2);

	const DeviceInstanceId first = f.bridge.rackDeviceAt(0);
	const DeviceInstanceId second = f.bridge.rackDeviceAt(1);
	REQUIRE(first != feathers::kNoInstance);
	REQUIRE(second != feathers::kNoInstance);
	const backplane::Source firstSource = backplane::Source::output(first, 0);
	const backplane::Source secondSource = backplane::Source::output(second, 0);

	REQUIRE(panel->setCardMinimised(secondSource, true));
	REQUIRE(panel->cardMinimised(secondSource));
	REQUIRE_FALSE(panel->cardMinimised(firstSource));

	// THE REORDER — the second modulator dragged to the front, which is the gesture that used to hand
	// the fold over.
	REQUIRE(f.bridge.moveRackDevice(1, 0));
	f.bridge.refresh();
	panel->resized();
	REQUIRE(f.bridge.rackDeviceAt(0) == second); // the premise: they really did swap places
	REQUIRE(f.bridge.rackDeviceAt(1) == first);

	// THE ASSERTION: the fold went with the device, not with the position.
	CHECK(panel->cardMinimised(secondSource));
	CHECK_FALSE(panel->cardMinimised(firstSource));
}
TEST_CASE("a fold belongs to its DEVICE across a SAVE: the strip reorders, the patch reloads, the right card is folded") {
	// **THE HALF THE SESSION-ONLY SET COULD NOT DO.** `ModPanel::minimised_` was right about its key and
	// wrong about its home: it died with the editor, so a desk preference keyed by the card's POSITION was
	// bolted on beside it to survive a restart — and that one handed the fold to whichever card arrived at
	// the number. Keying the desk file by device id instead is worse: ids are patch-scoped, so two patches
	// both have a device 1 and the desk would hand the incoming patch the outgoing one's folds. So, from
	// 2026-09-12, the fold lives in the patch: a word in the device's own record.
	//
	// **THE PANEL'S OWN ANSWER IS THE ASSERTION**, not a picture of a card: `cardMinimised` is what the
	// layout reads, and a render is evidence that something differs, never that this did.
	namespace dsr = feathers::mod::dahdsr;
	namespace mseg = feathers::mod::mseg;

	ModFixture f;
	auto* panel = findInTree<ui::ModPanel>(f.root);
	REQUIRE(panel != nullptr);
	REQUIRE(f.bridge.addRackDevice(mseg::kTraits.tag) == 1);
	f.bridge.refresh();
	panel->resized();
	REQUIRE(panel->deviceCount() == 2);

	const DeviceInstanceId first = f.bridge.rackDeviceAt(0);
	const DeviceInstanceId second = f.bridge.rackDeviceAt(1);
	REQUIRE(first != feathers::kNoInstance);
	REQUIRE(second != feathers::kNoInstance);
	REQUIRE(panel->setCardMinimised(backplane::Source::output(second, 0), true));

	// **THE REORDER HAPPENS BEFORE THE SAVE**, so the patch records the folded device at a position it
	// did not hold when the hand folded it. A fold remembered by position has nothing to hold on to here.
	REQUIRE(f.bridge.moveRackDevice(1, 0));
	f.bridge.refresh();
	panel->resized();
	REQUIRE(f.bridge.rackDeviceAt(0) == second);
	const feathers::rack::RackPatch saved = f.bridge.chainsPatch();

	// **AND THE LIVE STATE IS SET TO THE OPPOSITE ANSWER BEFORE THE LOAD.** Without this the case would
	// pass on a load that carried no fold at all, because the session's own set already says the truth.
	REQUIRE(panel->setCardMinimised(backplane::Source::output(second, 0), false));
	REQUIRE(panel->setCardMinimised(backplane::Source::output(first, 0), true));
	REQUIRE(panel->cardMinimised(backplane::Source::output(first, 0)));
	REQUIRE_FALSE(panel->cardMinimised(backplane::Source::output(second, 0)));

	REQUIRE(f.bridge.restoreChains(saved).routes.count == 0);
	f.bridge.refresh();
	panel->resized();
	REQUIRE(panel->deviceCount() == 2);

	// BY DEVICE — the ids the load left, asked one at a time. The mseg is the folded one whatever slot
	// either of them came back in.
	const feathers::ModRack after = f.bridge.modRack();
	int asked = 0;
	for (size_t slot = 0; slot < 2; ++slot) {
		const DeviceInstanceId id = f.bridge.rackDeviceAt(slot);
		REQUIRE(id != feathers::kNoInstance);
		const bool isMseg = device::sameTag(after.device[slot].tag.data(), mseg::kTraits.tag);
		CAPTURE(slot);
		CAPTURE(std::string(after.device[slot].tag.data()));
		CHECK(panel->cardMinimised(backplane::Source::output(id, 0)) == isMseg);
		++asked;
	}
	CHECK(asked == 2);
	// THE ORDER CAME BACK AS SAVED, which is what makes "the mseg is the folded one" a statement about a
	// device rather than about slot 0: the mseg is at 0 here and was at 1 when the hand folded it.
	CHECK(device::sameTag(after.device[0].tag.data(), mseg::kTraits.tag));
	CHECK(device::sameTag(after.device[1].tag.data(), dsr::kTraits.tag));
}
TEST_CASE("a double-click on a modulator card MINIMISES it - the card goes and the device stays") {
	// The symptoms: a double-click anywhere on a modulator made its card disappear, and in the same
	// episode Backspace took several presses to delete the modulator and, when it did, left ghost
	// modulation.
	//
	// **THIS CASE EXISTS TO SAY WHICH OF THE TWO THINGS HAPPENED, AS VALUES.** "The card disappeared"
	// is a report about pixels and is compatible with two completely different events: the device was
	// removed, or the card was MINIMISED and the device is still running. Those have opposite fixes —
	// one is a missing route clear on a removal road, the other is no defect in the routing at all.
	//
	// The double-click fold is a deliberate feature (it replaces the (-) mark) and the strip has two
	// roads into it, `ModPanel::focusOrArm`'s `Part::Fold`
	// and the card's own `onHeadFold`. What this pins is the CONSEQUENCE: after the gesture, the rack
	// still holds the device and the route table still holds its amounts.
	namespace dsr = feathers::mod::dahdsr;

	ModFixture f;
	auto* panel = findInTree<ui::ModPanel>(f.root);
	REQUIRE(panel != nullptr);

	REQUIRE(f.bridge.addRackDevice(dsr::kTraits.tag) == 1);
	f.bridge.refresh();
	panel->resized();
	const size_t devices = panel->deviceCount();
	REQUIRE(devices == 2);

	// THE MODULATOR'S OWN ROUTE, BY COORDINATE, INTO A CONTROL NOTHING IN THIS CASE TOUCHES.
	const backplane::Source source = backplane::Source::output(f.bridge.rackDeviceAt(1), 0);
	const backplane::Destination destination = backplane::Destination::core(static_cast<size_t>(modDestinationIndexOf(ParamId::Path)));
	f.bridge.setRouteAmount(source, destination, 0.5f);
	REQUIRE(f.bridge.routeAmount(source, destination) == doctest::Approx(0.5f));
	REQUIRE(sourceIsLive(f.bridge, source));
	REQUIRE_FALSE(panel->cardMinimised(source));

	// THE GESTURE, ON THE CARD'S HEAD, THROUGH THE TREE.
	const visage::Bounds open = panel->sourceBoundsForTest(source);
	const visage::Bounds card = open;
	const visage::Point head = panel->positionInWindow()
	                           + visage::Point { card.x() + card.width() * 0.5f, card.y() + 6.0f };
	// **AND IT IS THE CARD FRAME THAT TAKES BOTH POINTS, WHICH IS WHY THE RULE LIVES IN `FxCard`.**
	// MEASURED before any click: `frameAtPoint` at the head AND at three-quarters down the body both
	// answer an `FxCard`, not the panel. `ModPanel::hitAt` answers `Part::Fold` for the head rectangle
	// and is telling the truth about itself — that road is the way BACK into a card already minimised,
	// when the card frame no longer covers its own head. While the card is open the panel never sees
	// either press. Two objects, two answers, one gesture.
	CHECK(dynamic_cast<ui::FxCard*>(f.root->frameAtPoint(head)) != nullptr);
	doubleClickHead(f.root, head);
	f.bridge.refresh();
	panel->resized();

	// **THE CARD IS PUT AWAY.** This is the half that matches what it was seen.
	CHECK(panel->cardMinimised(source));

	// **AND HOW FAR "AWAY" IS, AS A NUMBER, because that is the whole of what it was seen.** MEASURED:
	// the card goes from **190 px to 24 px** — an 87% collapse, with every card below sliding up into
	// the space. It does NOT vanish: what is left is the head, and the head is the fold target, so the
	// gesture is its own undo. But a 24-pixel sliver where a card was, in a strip that has just
	// reflowed, is what "the card disappear !?" describes — and nothing on screen says the device is
	// still running.
	//
	// The head must stay PRESSABLE or the collapse is one-way, which is the defect `foldBounds` was
	// changed to fix once already: with the mark gone and the card frame no longer covering its own
	// head, `frameAtPoint` answered the PANEL and a folded modulator could not be reopened at all.
	const visage::Bounds shut = panel->sourceBoundsForTest(source);
	CHECK(shut.height() < open.height() * 0.5f);
	CHECK(shut.height() >= 16.0f);

	// **AND THE DEVICE IS STILL THERE, WITH ITS ROUTES.** This is the half that says the "ghost
	// modulation" is not a ghost at all: nothing was deleted, so nothing needed clearing, and the
	// modulator goes on modulating exactly as it should.
	CHECK(panel->deviceCount() == devices);
	CHECK(sourceIsLive(f.bridge, source));
	CHECK(f.bridge.routeAmount(source, destination) == doctest::Approx(0.5f));

	SUBCASE("and a double-click on the BODY does NOT fold it - head only") {
		// **DECIDED 2026-09-05: the double-click fold is on the head only.** Before that decision this
		// SUBCASE was the opposite assertion and it PASSED:
		// measured three-quarters of the way down the body, the card folded, 190 px to 24. The road is
		// `FxCard::noteCardRelease`, which collapses on "armed and never moved", and `noteCardPress`
		// armed on the click count alone wherever the press landed.
		//
		// **THIS HALF IS THE ONE THAT MATTERS, because the other half passes on the broken build.** A
		// case asserting only "a head double-click folds" is satisfied by a card that folds from
		// anywhere, which is exactly what was there.
		REQUIRE(panel->setCardMinimised(source, false));
		panel->resized();
		REQUIRE_FALSE(panel->cardMinimised(source));
		const visage::Bounds again = panel->sourceBoundsForTest(source);
		const visage::Point body =
		    panel->positionInWindow()
		    + visage::Point { again.x() + again.width() * 0.5f, again.y() + again.height() * 0.75f };
		// **AND THE PRESS MUST REACH THE CARD, in the CARD's own coordinates**, or this is a case about
		// a point nothing owns. The head test runs on `noteCardPress`'s `position`, so if a body press
		// could arrive through a CHILD it would be measured against the wrong origin — the hazard that
		// excludes `setReceiveChildMouseEvents(true)`, which delivers a child-local `position` to the
		// parent. Nothing in `src/` sets that flag, so the card only ever hears its OWN presses; this
		// asserts the frame identity rather than trusting the grep.
		visage::Frame* atBody = f.root->frameAtPoint(body);
		REQUIRE(dynamic_cast<ui::FxCard*>(atBody) != nullptr);
		CHECK(atBody->parent() == static_cast<visage::Frame*>(panel));
		doubleClickHead(f.root, body);
		panel->resized();
		CHECK_FALSE(panel->cardMinimised(source));
		// AND NOTHING ELSE HAPPENED EITHER — the device and its routes are untouched, so a body
		// double-click is not quietly doing something else instead.
		CHECK(sourceIsLive(f.bridge, source));
		CHECK(f.bridge.routeAmount(source, destination) == doctest::Approx(0.5f));
	}

	SUBCASE("and the menu's Remove really does clear them, which is the contrast") {
		// (...) -> Remove appeared not to leave ghost modulation — an observation made by eye, so the
		// cells are asserted rather than believed. If this went red the whole diagnosis would change.
		REQUIRE(panel->removeDeviceAtLane(1));
		f.bridge.refresh();
		panel->resized();
		CHECK_FALSE(sourceIsLive(f.bridge, source));
		CHECK(f.bridge.routeAmount(source, destination) == doctest::Approx(0.0f));
	}
}
TEST_CASE("arm draws the GLOW and select draws the OUTLINE - two instruments that cannot collide") {
	// Mod assign shows a glow rather than an outline, and a mod card is outlined when selected by a
	// click anywhere on its face.
	//
	// WHY THIS IS A RULE AND NOT A TWEAK. The strip drew the armed source's card with a BORDER — the
	// same one-pixel line the fx lane and the rack draw for their SELECTED card. Selection had not
	// reached the strip yet, so nothing collided; the day it did, one line would have had to say two
	// things (backlog CN2/CO, "the two-outlines problem"). The macro row never had the problem because
	// it never used a border: a faint ground at 0.16 alpha behind the focused column, *"which is what
	// a card's border used to say." This case pins that the card now speaks the column's language for
	// modulation and keeps the border for selection.
	//
	// **ASSERTED AS THE STYLE THE FRAME WILL BE DRAWN WITH, NOT AS PIXELS.** `ui::cardFrameStyleOf` is
	// the one rule and `FxCard::frameStyle()` is what `FxCard::draw` hands the drawer — so this reads
	// the value the paint is made from, which is CLAUDE.md's rule for a claim about a value.
	ModFixture f;
	auto* panel = f.panel;
	REQUIRE(panel != nullptr);
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);
	while (panel->deviceCount() < 2) {
		REQUIRE(panel->addChoiceForTest(0) < feathers::kMaxRackSlots);
	}
	panel->resized();

	auto* card = dynamic_cast<ui::FxCard*>(const_cast<visage::Frame*>(panel->cardFrameForTest(0)));
	auto* other = dynamic_cast<ui::FxCard*>(const_cast<visage::Frame*>(panel->cardFrameForTest(1)));
	REQUIRE(card != nullptr);
	REQUIRE(other != nullptr);
	const backplane::Source source = backplane::Source::output(f.bridge.rackDeviceAt(0), 0);

	const uint32_t modGlow = ui::withAlpha(ui::kModulation, ui::kArmGlowAlpha);
	// STRONGER THAN THE MACRO ROW'S FURNITURE GROUND, and that is the point of the constant: the
	// first version used the row's 0.16 and photographed as a wash that did not read as a glow.
	REQUIRE(ui::kArmGlowAlpha > 0.16f);

	// THE RULE ON ITS OWN, every combination that matters — so a red below can be read as "the card
	// reports the wrong state" rather than "the rule folds it wrong".
	{
		const ui::CardFrameStyle none = ui::cardFrameStyleOf(false, false, false);
		CHECK_FALSE(none.outline);
		CHECK_FALSE(none.glow);
		CHECK_FALSE(none.nameLit);

		const ui::CardFrameStyle selected = ui::cardFrameStyleOf(true, false, false);
		CHECK(selected.outline);
		CHECK_FALSE(selected.glow);
		CHECK(selected.nameLit);

		const ui::CardFrameStyle armed = ui::cardFrameStyleOf(false, true, true);
		CHECK_FALSE(armed.outline);
		CHECK(armed.glow);
		CHECK(armed.glowColor == modGlow);

		// **FOCUSED BUT NOT ARMED DOES NOT GLOW.** The first version lit it in the accent and the
		// install photographed the two tints as indistinguishable (29,34,54 against 26,35,52) — a
		// glow that stays after disarming is not a glow for arming. The name still lights.
		const ui::CardFrameStyle focusedOnly = ui::cardFrameStyleOf(false, true, false);
		CHECK_FALSE(focusedOnly.outline);
		CHECK_FALSE(focusedOnly.glow);
		CHECK(focusedOnly.nameLit);

		// AND ARMED ALONE GLOWS, so a report that says armed without saying focused still glows.
		CHECK(ui::cardFrameStyleOf(false, false, true).glow);

		const ui::CardFrameStyle both = ui::cardFrameStyleOf(true, true, true);
		CHECK(both.outline);
		CHECK(both.glow);
		CHECK(both.glowColor == modGlow);

		// **DISTINGUISHABLE, STATED AS THE THING IT MEANS**: the selected-only style and the armed-only
		// style differ in BOTH fields, so no card can be read as the other.
		CHECK(selected.outline != armed.outline);
		CHECK(selected.glow != armed.glow);
	}

	// NOTHING YET. The fixture's focus is a macro's, and nobody has selected anything.
	REQUIRE_FALSE(card->headFocused());
	REQUIRE_FALSE(card->headSourceFocused());
	REQUIRE_FALSE(card->headArmed());
	CHECK_FALSE(card->frameStyle().outline);
	CHECK_FALSE(card->frameStyle().glow);

	SUBCASE("arming the card's source glows it, and a glow without an outline is a card the hand left") {
		// **ARMING SELECTS NOW** — Delete takes the lit card, by design — so
		// arming from the state (the road a grid or anything written tomorrow would take) lights AND
		// outlines the card. The two instruments still cannot collide: press ANOTHER card and the
		// armed one keeps its glow and gives up its outline, which is the state this subcase is about.
		mod->focus(source);
		mod->arm(true);
		REQUIRE(mod->armed());
		CHECK(card->headSourceFocused());
		CHECK(card->headArmed());
		CHECK(card->headFocused());
		CHECK(card->frameStyle().outline);

		panel->selectSlot(1);
		CHECK_FALSE(card->headFocused());
		const ui::CardFrameStyle style = card->frameStyle();
		CHECK(style.glow);
		CHECK(style.glowColor == modGlow);
		CHECK_FALSE(style.outline);

		// THE NEIGHBOUR: outlined, and no glow — the glow is the armed source's, not the strip's.
		CHECK_FALSE(other->frameStyle().glow);
		CHECK(other->frameStyle().outline);

		// DISARMED BUT STILL THE FOCUSED SOURCE: THE GLOW GOES. A glow that survived the disarm is
		// what the install photographed as "not a glow" — the card's ground says the MODE and the
		// mode is off. The name stays lit, which is the quiet form of "the rings are about me".
		// Disarming deselects nothing either: the neighbour stays the selected one.
		mod->arm(false);
		CHECK_FALSE(card->headArmed());
		CHECK(card->headSourceFocused());
		CHECK_FALSE(card->frameStyle().glow);
		CHECK(card->frameStyle().nameLit);
		CHECK_FALSE(card->frameStyle().outline);
		CHECK(other->frameStyle().outline);
	}

	SUBCASE("the real arm chip on the card's face arms AND selects — glow and outline together") {
		f.arm(source);
		REQUIRE(mod->armed());
		REQUIRE(mod->source() == source);
		CHECK(card->headArmed());
		// THE CHIP IS THE FACE — see the every-gesture case for the road and the report behind it.
		CHECK(card->headFocused());
		const ui::CardFrameStyle style = card->frameStyle();
		CHECK(style.glow);
		CHECK(style.glowColor == modGlow);
		CHECK(style.outline);
		CHECK_FALSE(other->frameStyle().glow);
		CHECK_FALSE(other->frameStyle().outline);
	}

	SUBCASE("selecting the card outlines it, and draws no glow") {
		// THE HOST'S FLAG, SET DIRECTLY — which card is selected is the strip's choice and the press
		// that makes it is the next case's subject. Here the claim is what the flag draws.
		card->setFocused(true);
		CHECK(card->headFocused());
		CHECK_FALSE(card->headSourceFocused());
		const ui::CardFrameStyle style = card->frameStyle();
		CHECK(style.outline);
		CHECK_FALSE(style.glow);
		CHECK(style.nameLit);
	}

	SUBCASE("both on one card at once draws both, and they are still telling different things") {
		card->setFocused(true);
		f.arm(source);
		REQUIRE(mod->armed());
		const ui::CardFrameStyle style = card->frameStyle();
		CHECK(style.outline);
		CHECK(style.glow);
		CHECK(style.glowColor == modGlow);
		// TAKE THE SELECTION AWAY AND THE GLOW STAYS; TAKE THE ARM AWAY AND THE OUTLINE STAYS.
		card->setFocused(false);
		CHECK_FALSE(card->frameStyle().outline);
		CHECK(card->frameStyle().glow);
		card->setFocused(true);
		mod->arm(false);
		CHECK(card->frameStyle().outline);
		CHECK_FALSE(card->frameStyle().glow);
	}
}
TEST_CASE("clicking a KNOB on a mod card selects that card - anywhere on the face") {
	// A mod card is outlined when selected by a click anywhere on its face. The strip had NO
	// selection — no focused slot, no `setFocused` on
	// its cards, and `FxCard::onCardPressed` bound by the fx lane and the rack and never here. The
	// road existed; the strip had not subscribed.
	//
	// **THE SHAPE OF `test_rack_editor.cpp`'s "clicking a KNOB on a rack card selects that card",
	// because the mechanism is the same one: a press on a control moves the keyboard focus to the
	// nearest keystroke-accepting ANCESTOR, which is the card, and the card's `focusChanged` fires
	// the hook. So the knob is picked BY TYPE (`dynamic_cast`), the press is shown to land on it and
	// not on the card behind it, the card is shown to be that ancestor, and the selection is read as
	// a value. The tracker's generic card is the one with a `ui::Knob` on it — the DAHDSR's face is
	// value boxes.
	ModFixture f;
	auto* panel = f.panel;
	REQUIRE(panel != nullptr);
	REQUIRE(panel->deviceCount() >= 1);
	const size_t trak = f.bridge.addRackDevice(feathers::mod::tracker::kTraits.tag);
	REQUIRE(trak < feathers::kMaxRackSlots);
	REQUIRE(trak >= 1);
	panel->resized();
	REQUIRE(panel->cardKindOf(trak) == ui::ModPanel::CardKind::Generic);

	auto* card = dynamic_cast<ui::FxCard*>(const_cast<visage::Frame*>(panel->cardFrameForTest(trak)));
	auto* other = dynamic_cast<ui::FxCard*>(const_cast<visage::Frame*>(panel->cardFrameForTest(0)));
	REQUIRE(card != nullptr);
	REQUIRE(other != nullptr);
	ui::Knob* knob = nullptr;
	for (visage::Frame* child : card->children()) {
		if (auto* k = dynamic_cast<ui::Knob*>(child); k != nullptr && k->isVisible() && k->width() > 2.0f) {
			knob = k;
			break;
		}
	}
	REQUIRE(knob != nullptr);

	// NOTHING SELECTED AT REST — the strip does not pick a card for the hand.
	REQUIRE(panel->selectedSlotForTest() == ui::ModPanel::kNoFocus);
	REQUIRE_FALSE(card->headFocused());

	// SELECT THE OTHER CARD FIRST, or a build where the press does nothing would pass on `kNoFocus`
	// being a milder failure than the one a hand meets: the key would take the card pressed LAST.
	panel->selectSlot(0);
	REQUIRE(panel->selectedSlotForTest() == 0);
	REQUIRE(other->headFocused());

	// BOTH HALVES OF `WindowEventHandler::handleMouseDown` — the press, and the focus told to the
	// nearest keystroke-accepting ancestor. `dragThroughRoot` performs the first and not the second,
	// and the second is the road under test.
	const visage::Point where =
	    knob->positionInWindow() + visage::Point { knob->width() * 0.5f, knob->faceDiameter() * 0.5f };
	visage::Frame* target = f.root->frameAtPoint(where);
	REQUIRE(target != nullptr);
	// THE PRESS REALLY LANDS ON THE KNOB, not on the card behind it — otherwise this is the ordinary
	// body press and the case would be about the wrong road.
	CHECK(dynamic_cast<ui::Knob*>(target) != nullptr);
	visage::Frame* focus = keyboardFocusAfterPress(f.root, where);
	REQUIRE(focus != nullptr);
	CHECK(focus == static_cast<visage::Frame*>(card));
	focus->processFocusChanged(true, true);

	// **THE RULE, AS A VALUE** — the position, the identity, and the flag the outline is drawn from.
	CHECK(panel->selectedSlotForTest() == trak);
	CHECK(panel->selectedInstanceForTest() == f.bridge.modRack().device[trak].instanceId);
	CHECK(card->headFocused());
	CHECK(card->frameStyle().outline);
	// AND ONLY THAT CARD — a selection is one card, and the one the hand left is released.
	CHECK_FALSE(other->headFocused());
	CHECK_FALSE(other->frameStyle().outline);
	// AND IT SAID NOTHING ABOUT MODULATION: selection is not arming.
	CHECK_FALSE(card->frameStyle().glow);

	SUBCASE("a press on a FOLDED card's head selects it — the head is the card's, drawn and pressable") {
		// **THIS USED TO PRESS THE PANEL, AND THAT WAS THE BUG (backlog CZ).** The strip HID a
		// minimised card, so its head was not drawn at all and the panel answered the press for a
		// blank band — this subcase measured that invisible control and called it a road. A folded
		// card is now folded, not hidden: its head is a visible frame of head height, and the press
		// reaches the card's own hook like any other press on its face.
		const backplane::Source source = backplane::Source::output(f.bridge.rackDeviceAt(0), 0);
		REQUIRE(panel->setCardMinimised(source, true));
		panel->resized();
		REQUIRE(panel->cardMinimised(source));
		const visage::Bounds head = panel->foldBounds(source);
		const visage::Point at = panel->positionInWindow()
		                         + visage::Point { head.x() + head.width() * 0.5f,
			                                       head.y() + head.height() * 0.5f };
		REQUIRE(f.root->frameAtPoint(at) == static_cast<visage::Frame*>(other));
		visage::Frame* focus = keyboardFocusAfterPress(f.root, at);
		REQUIRE(focus == static_cast<visage::Frame*>(other));
		focus->processFocusChanged(true, true);
		dragThroughRoot(f.root, at, at);
		CHECK(panel->selectedSlotForTest() == 0);
		CHECK(other->headFocused());
		CHECK_FALSE(card->headFocused());
		// A SINGLE CLICK SELECTS AND DOES NOT UNFOLD — the fold is the second click's.
		CHECK(panel->cardMinimised(source));
	}

	SUBCASE("the selection follows the DEVICE across a reorder and a removal above it, and dies with it") {
		// THE KEY IS THE INSTANCE ID, so a position is never stale in the direction that removes the
		// wrong device. Each step asks the resolved position and the id together.
		const feathers::DeviceInstanceId chosen = panel->selectedInstanceForTest();
		REQUIRE(chosen != feathers::kNoInstance);
		REQUIRE(f.bridge.addRackDevice(feathers::mod::dahdsr::kTraits.tag) == 2);
		panel->resized();
		REQUIRE(panel->deviceCount() == 3);
		CHECK(panel->selectedSlotForTest() == 1);

		REQUIRE(panel->moveDevice(1, 2));
		CHECK(panel->selectedSlotForTest() == 2);
		CHECK(panel->selectedInstanceForTest() == chosen);
		CHECK(f.bridge.modRack().device[2].instanceId == chosen);
		auto* moved = dynamic_cast<ui::FxCard*>(const_cast<visage::Frame*>(panel->cardFrameForTest(2)));
		REQUIRE(moved != nullptr);
		CHECK(moved->headFocused());
		CHECK_FALSE(other->headFocused());

		REQUIRE(panel->removeDeviceAtLane(0));
		CHECK(panel->selectedSlotForTest() == 1);
		CHECK(panel->selectedInstanceForTest() == chosen);
		CHECK(f.bridge.modRack().device[1].instanceId == chosen);

		REQUIRE(panel->removeDeviceAtLane(1));
		CHECK(panel->selectedSlotForTest() == ui::ModPanel::kNoFocus);
		CHECK(panel->selectedInstanceForTest() == feathers::kNoInstance);
		for (size_t slot = 0; slot < feathers::kMaxRackSlots; ++slot) {
			const auto* left = dynamic_cast<const ui::FxCard*>(panel->cardFrameForTest(slot));
			if (left != nullptr) {
				CHECK_FALSE(left->headFocused());
			}
		}
	}
}
TEST_CASE("Backspace removes the SELECTED modulator by identity - and a third device's route is untouched") {
	// Delete/Backspace deletes the selected mod card. Modelled on the fx
	// lane's "Backspace takes the card under the hand", and asserted the way the strip's own removal
	// case is: WHICH instance died, WHICH survived, and what the route table READS at each device's
	// lane afterwards. "The rack got shorter" is green whichever device went, and that is the whole
	// of last week.
	//
	// **THREE DEVICES, BECAUSE TWO CAN PASS FOR THE WRONG REASON.** With A and B, "B's cells are zero
	// and A's are not" is also true of a build that cleared the whole table and then happened to keep
	// one row — and "the survivor is untouched" has only one witness. C is the device the key had no
	// business near, above OR below the target.
	//
	// **THROUGH THE ROAD A KEYSTROKE ACTUALLY TRAVELS.** The press is delivered to the real control on
	// the real card, the focus is computed the way `handleMouseDown` computes it — the nearest
	// keystroke-accepting ancestor — and the key walks UP from there. A case that called the panel's
	// handler would prove it works and say nothing about whether a key reaches it (backlog CO).
	namespace dsr = feathers::mod::dahdsr;
	ModFixture f;
	auto* panel = f.panel;
	REQUIRE(panel != nullptr);
	while (panel->deviceCount() < 3) {
		REQUIRE(f.bridge.addRackDevice(dsr::kTraits.tag) < feathers::kMaxRackSlots);
	}
	panel->resized();
	REQUIRE(panel->deviceCount() == 3);

	// A ROUTE OUT OF EACH, to a destination no rack device owns, so the table can be read by
	// coordinate rather than by total — the sibling case's arrangement exactly.
	const int fixedIndex = feathers::modDestinationIndexOf(ParamId::FineTune);
	REQUIRE(fixedIndex >= 0);
	const backplane::Destination fixedDest = backplane::Destination::core(static_cast<size_t>(fixedIndex));
	const std::array<float, 3> mark { 0.2f, 0.5f, 0.8f };
	const feathers::ModRack before = f.bridge.modRack();
	std::array<feathers::DeviceInstanceId, 3> id {};
	for (size_t slot = 0; slot < 3; ++slot) {
		id[slot] = before.device[slot].instanceId;
		f.bridge.setRouteAmount(backplane::Source::output(id[slot], 0), fixedDest, mark[slot]);
		REQUIRE(f.bridge.routeAmount(backplane::Source::output(id[slot], 0), fixedDest) == doctest::Approx(mark[slot]));
	}
	REQUIRE(id[0] != id[1]);
	REQUIRE(id[1] != id[2]);
	/// THE ROUTE OUT OF THE DEVICE `device`, by its id.
	const auto routeOf = [&](DeviceInstanceId device) {
		return f.bridge.routeAmount(backplane::Source::output(device, 0), fixedDest);
	};

	// SELECT BY THE REAL ROAD: a press on one of the card's CONTROLS, the focus told to the card.
	const auto selectByPress = [&](size_t slot) {
		panel->resized();
		// SCROLLED INTO VIEW FIRST — three envelope cards are taller than the rail (the strip's own
		// header records why it scrolls), and a press aimed below the rail's edge lands on whatever
		// is drawn there rather than on the card. MEASURED: slot 2 of three answered the rail, not
		// the control, until this line.
		{
			const visage::Bounds box =
			    panel->sourceBoundsForTest(backplane::Source::output(f.bridge.rackDeviceAt(slot), 0));
			panel->setScroll(panel->scroll() + box.y() - ui::ModPanel::kPadTop);
		}
		auto* card = dynamic_cast<ui::FxCard*>(const_cast<visage::Frame*>(panel->cardFrameForTest(slot)));
		REQUIRE(card != nullptr);
		visage::Frame* control = nullptr;
		for (visage::Frame* child : card->children()) {
			if (child->isVisible() && child->width() > 2.0f && child->height() > 2.0f) {
				control = child;
				break;
			}
		}
		REQUIRE(control != nullptr);
		const visage::Point where = control->positionInWindow()
		                            + visage::Point { control->width() * 0.5f, control->height() * 0.5f };
		// THE PRESS LANDS ON THE CONTROL — the card's own body press is a different, easier road.
		REQUIRE(f.root->frameAtPoint(where) == control);
		visage::Frame* focus = keyboardFocusAfterPress(f.root, where);
		REQUIRE(focus != nullptr);
		// THE MECHANISM, PINNED: the first keystroke-accepting frame above the control is the CARD,
		// and the card's parent is the panel the key handler sits on.
		REQUIRE(focus == static_cast<visage::Frame*>(card));
		REQUIRE(focus->parent() == static_cast<visage::Frame*>(panel));
		REQUIRE(panel->acceptsKeystrokes());
		focus->processFocusChanged(true, true);
		dragThroughRoot(f.root, where, where);
		REQUIRE(panel->selectedSlotForTest() == slot);
		return focus;
	};

	// **A DIFFERENT CARD IS SELECTED FIRST, the way a session actually goes** — the hand was on one
	// device, then moved to another. Without it the broken build's stale target is "nothing", which
	// is a milder failure than the one a hand meets.
	selectByPress(0);

	SUBCASE("Backspace from the pressed card removes THAT device, and only that device") {
		visage::Frame* focus = selectByPress(1);
		REQUIRE(panel->selectedInstanceForTest() == id[1]);

		REQUIRE(keyFromFocus(focus, visage::KeyEvent { visage::KeyCode::Backspace, 0, true }));

		const feathers::ModRack after = f.bridge.modRack();
		REQUIRE(after.count == 2);
		CHECK(after.device[0].instanceId == id[0]);
		CHECK(after.device[1].instanceId == id[2]);

		// **THE VALUES.** B's cells read zero — the route clear the menu's verb performs, reached by
		// the key — and A's and C's read what they were set to.
		CHECK(routeOf(id[1]) == doctest::Approx(0.0f));
		CHECK(routeOf(id[0]) == doctest::Approx(mark[0]));
		CHECK(routeOf(id[2]) == doctest::Approx(mark[2]));

		// AND THE SELECTION WENT WITH IT, so a held key finds nothing: a second Backspace, from the
		// panel the walk would reach next, is declined and removes nobody.
		CHECK(panel->selectedSlotForTest() == ui::ModPanel::kNoFocus);
		CHECK(panel->selectedInstanceForTest() == feathers::kNoInstance);
		CHECK_FALSE(keyFromFocus(panel, visage::KeyEvent { visage::KeyCode::Backspace, 0, true }));
		CHECK(f.bridge.modRack().count == 2);
		CHECK(routeOf(id[0]) == doctest::Approx(mark[0]));
		CHECK(routeOf(id[2]) == doctest::Approx(mark[2]));
	}

	SUBCASE("Delete and the keypad's Backspace are the same gesture, and any other key is not the strip's") {
		visage::Frame* focus = selectByPress(2);
		CHECK_FALSE(keyFromFocus(focus, visage::KeyEvent { visage::KeyCode::A, 0, true }));
		CHECK(f.bridge.modRack().count == 3);
		CHECK(panel->selectedSlotForTest() == 2);
		REQUIRE(keyFromFocus(focus, visage::KeyEvent { visage::KeyCode::Delete, 0, true }));
		CHECK(f.bridge.modRack().count == 2);
		CHECK(f.bridge.modRack().device[0].instanceId == id[0]);
		CHECK(f.bridge.modRack().device[1].instanceId == id[1]);
		CHECK(routeOf(id[2]) == doctest::Approx(0.0f));
		CHECK(routeOf(id[0]) == doctest::Approx(mark[0]));
		CHECK(routeOf(id[1]) == doctest::Approx(mark[1]));

		focus = selectByPress(0);
		REQUIRE(keyFromFocus(focus, visage::KeyEvent { visage::KeyCode::KPBackspace, 0, true }));
		CHECK(f.bridge.modRack().count == 1);
		CHECK(f.bridge.modRack().device[0].instanceId == id[1]);
		CHECK(routeOf(id[0]) == doctest::Approx(0.0f));
		CHECK(routeOf(id[1]) == doctest::Approx(mark[1]));
	}

	SUBCASE("after a removal ABOVE the selection, the key still takes the one the hand chose") {
		// **THE WEEK'S BUG, AIMED AT THE NEW ROAD.** A position remembered across a removal names the
		// device that slid into it. The selection is an ID, so the removal above moves the answer
		// rather than the target. A FOURTH device below, so that both neighbours of the target — one
		// above that moved, one below that did not — are witnesses.
		REQUIRE(f.bridge.addRackDevice(dsr::kTraits.tag) == 3);
		panel->resized();
		const feathers::DeviceInstanceId idD = f.bridge.modRack().device[3].instanceId;
		f.bridge.setRouteAmount(backplane::Source::output(idD, 0), fixedDest, 0.9f);

		selectByPress(2); // C
		REQUIRE(panel->selectedInstanceForTest() == id[2]);
		// A GOES THROUGH THE MENU'S VERB, the other road to the same removal.
		REQUIRE(panel->slotChoiceForTest(0, ui::ModPanel::SlotVerb::Remove));
		REQUIRE(f.bridge.modRack().count == 3);
		REQUIRE(f.bridge.modRack().device[1].instanceId == id[2]);
		CHECK(panel->selectedSlotForTest() == 1);

		// THE KEY ARRIVES AT THE PANEL. The removal above rebuilt every card below it (the lane key),
		// and visage drops a removed frame from its focus pointer — so the card the hand pressed is no
		// longer the focused frame, and the walk from the next press starts wherever that press lands.
		// From the strip's own face it starts here.
		REQUIRE(keyFromFocus(panel, visage::KeyEvent { visage::KeyCode::Backspace, 0, true }));

		const feathers::ModRack after = f.bridge.modRack();
		REQUIRE(after.count == 2);
		// **C DIED, BY IDENTITY — NOT B, WHICH IS WHAT A STALE POSITION 2 WOULD HAVE NAMED (D), NOR
		// WHAT A RESET-ON-REMOVAL WOULD HAVE DONE (NOTHING).**
		CHECK(after.device[0].instanceId == id[1]);
		CHECK(after.device[1].instanceId == idD);
		CHECK(routeOf(id[2]) == doctest::Approx(0.0f));
		CHECK(routeOf(id[1]) == doctest::Approx(mark[1]));
		CHECK(routeOf(idD) == doctest::Approx(0.9f));
	}

	SUBCASE("and after a REORDER, likewise") {
		selectByPress(1); // B
		REQUIRE(panel->moveDevice(2, 0)); // C A B
		REQUIRE(f.bridge.modRack().device[2].instanceId == id[1]);
		CHECK(panel->selectedSlotForTest() == 2);

		REQUIRE(keyFromFocus(panel, visage::KeyEvent { visage::KeyCode::Backspace, 0, true }));
		const feathers::ModRack after = f.bridge.modRack();
		REQUIRE(after.count == 2);
		CHECK(after.device[0].instanceId == id[2]);
		CHECK(after.device[1].instanceId == id[0]);
		CHECK(routeOf(id[1]) == doctest::Approx(0.0f));
		CHECK(routeOf(id[0]) == doctest::Approx(mark[0]));
		CHECK(routeOf(id[2]) == doctest::Approx(mark[2]));
	}

	SUBCASE("with nothing selected the key is not the strip's") {
		panel->clearSelection();
		REQUIRE(panel->selectedSlotForTest() == ui::ModPanel::kNoFocus);
		CHECK_FALSE(keyFromFocus(panel, visage::KeyEvent { visage::KeyCode::Backspace, 0, true }));
		CHECK(f.bridge.modRack().count == 3);
	}
}
TEST_CASE("every press on a mod card's face selects it - the outline and the key agree on every card after every gesture") {
	// The defect: a mod card could be outlined without really being selected, so Delete hit
	// something else.
	//
	// THE ROAD, MEASURED HERE: the ARM CHIP. It sits on the card's face — the panel's own widget,
	// `setOnTop` of the card, the most-pressed thing on a modulator — and its press lit the card (armed,
	// glowing) while the card's `onCardPressed` never heard it, because the chip is neither the card
	// nor its child. It accepts no keystrokes either, so the focus fell to the PANEL and Delete took
	// the card selected before: a lit card, a different card deleted. No rack device has a popped-out
	// view (`expanded_views.cpp` lists fx devices only), so that road does not exist here.
	//
	// **THE INVARIANT, ASSERTED ON EVERY CARD AFTER EVERY GESTURE**: the outline a card draws is
	// computed from the panel's `selected_` and nothing else — `frameStyle().outline` equals "my id is
	// the selected id" for every card in the strip — and the pressed card IS the selected one for any
	// press on its face. Each gesture goes through the real tree the way `handleMouseDown` goes: the
	// frame at the point, the nearest keystroke-accepting ancestor told it has the focus, the press
	// delivered.
	namespace dsr = feathers::mod::dahdsr;
	ModFixture f;
	auto* panel = f.panel;
	REQUIRE(panel != nullptr);
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);
	while (panel->deviceCount() < 2) {
		REQUIRE(f.bridge.addRackDevice(dsr::kTraits.tag) < feathers::kMaxRackSlots);
	}
	panel->resized();
	REQUIRE(panel->deviceCount() == 2);

	const auto idAt = [&](size_t slot) { return f.bridge.modRack().device[slot].instanceId; };
	const auto cardAt = [&](size_t slot) {
		auto* card = dynamic_cast<ui::FxCard*>(const_cast<visage::Frame*>(panel->cardFrameForTest(slot)));
		REQUIRE(card != nullptr);
		return card;
	};
	const auto sourceAt = [&](size_t slot) {
		return backplane::Source::output(f.bridge.rackDeviceAt(slot), 0);
	};
	const auto scrollTo = [&](size_t slot) {
		panel->resized();
		const visage::Bounds box = panel->sourceBoundsForTest(sourceAt(slot));
		panel->setScroll(panel->scroll() + box.y() - ui::ModPanel::kPadTop);
	};
	// THE INVARIANT.
	const auto agree = [&](const char* after) {
		CAPTURE(after);
		const feathers::DeviceInstanceId chosen = panel->selectedInstanceForTest();
		for (size_t slot = 0; slot < panel->deviceCount(); ++slot) {
			CAPTURE(slot);
			const bool mine = chosen != feathers::kNoInstance && idAt(slot) == chosen;
			CHECK(cardAt(slot)->headFocused() == mine);
			CHECK(cardAt(slot)->frameStyle().outline == mine);
		}
	};
	// A PRESS AS THE WINDOW DELIVERS IT. Returns the frame that holds the keyboard focus afterwards.
	const auto pressAt = [&](visage::Point where) {
		visage::Frame* focus = keyboardFocusAfterPress(f.root, where);
		REQUIRE(focus != nullptr);
		focus->processFocusChanged(true, true);
		dragThroughRoot(f.root, where, where);
		return focus;
	};
	const auto inWindow = [&](visage::Bounds b, float fx, float fy) {
		return panel->positionInWindow() + visage::Point { b.x() + b.width() * fx, b.y() + b.height() * fy };
	};

	REQUIRE(panel->selectedSlotForTest() == ui::ModPanel::kNoFocus);
	agree("at rest");

	// THE OTHER CARD FIRST, every time, so a gesture that selects nothing leaves a WRONG answer rather
	// than "nothing" — the milder failure.
	const auto selectOther = [&](size_t target) {
		panel->selectSlot(1 - target);
		REQUIRE(panel->selectedSlotForTest() == 1 - target);
		agree("after selecting the other card");
	};

	SUBCASE("a control on the card") {
		selectOther(1);
		scrollTo(1);
		ui::FxCard* card = cardAt(1);
		visage::Frame* control = nullptr;
		for (visage::Frame* child : card->children()) {
			if (child->isVisible() && child->width() > 2.0f && child->height() > 2.0f) {
				control = child;
				break;
			}
		}
		REQUIRE(control != nullptr);
		const visage::Point where = control->positionInWindow()
		                            + visage::Point { control->width() * 0.5f, control->height() * 0.5f };
		REQUIRE(f.root->frameAtPoint(where) == control);
		visage::Frame* focus = pressAt(where);
		CHECK(focus == static_cast<visage::Frame*>(card));
		CHECK(panel->selectedInstanceForTest() == idAt(1));
		agree("after a press on a control");
	}

	SUBCASE("the card's own body, and its head") {
		selectOther(0);
		scrollTo(0);
		const visage::Bounds box = panel->sourceBoundsForTest(sourceAt(0));
		// THE BODY: a point below the head and away from every child — the value boxes start lower
		// than the curve, so the curve's own rectangle at its left is bare card.
		const visage::Point body = inWindow(box, 0.05f, 0.6f);
		visage::Frame* under = f.root->frameAtPoint(body);
		REQUIRE(under != nullptr);
		// Whatever child or the card itself takes it, the nearest acceptor must be the card.
		visage::Frame* focus = pressAt(body);
		CHECK(focus == static_cast<visage::Frame*>(cardAt(0)));
		CHECK(panel->selectedInstanceForTest() == idAt(0));
		agree("after a press on the body");

		selectOther(0);
		// THE HEAD, left of the marks: the name. A single press must select and must not fold.
		const visage::Point head = inWindow(box, 0.4f, ui::ModPanel::kHeadHeight * 0.5f / box.height());
		REQUIRE(f.root->frameAtPoint(head) == static_cast<visage::Frame*>(cardAt(0)));
		pressAt(head);
		CHECK(panel->selectedInstanceForTest() == idAt(0));
		CHECK_FALSE(panel->cardMinimised(sourceAt(0)));
		agree("after a press on the head");
	}

	SUBCASE("the ARM CHIP on the card's face - the road the defect came down") {
		selectOther(1);
		scrollTo(1);
		const visage::Point chip = f.armPoint(sourceAt(1));
		// THE PRESS LANDS ON THE CHIP, which is the panel's widget and not the card's: this is the
		// whole reason the card's own hook could not hear it.
		auto* arm = dynamic_cast<ui::SourceArm*>(f.root->frameAtPoint(chip));
		REQUIRE(arm != nullptr);
		REQUIRE(arm->parent() == static_cast<visage::Frame*>(panel));
		visage::Frame* focus = pressAt(chip);
		// AND THE FOCUS FALLS TO THE PANEL — the chip accepts no keystrokes — which is why a key
		// after this press resolved the panel's selection and not the lit card.
		CHECK(focus == static_cast<visage::Frame*>(panel));
		REQUIRE(mod->armed());
		REQUIRE(mod->source() == sourceAt(1));
		CHECK(cardAt(1)->frameStyle().glow);
		// **THE RULE: the lit card is the selected card.**
		CHECK(panel->selectedInstanceForTest() == idAt(1));
		CHECK(panel->selectedSlotForTest() == 1);
		agree("after a press on the arm chip");

		// AND DELETE FROM THAT FOCUS TAKES THE LIT CARD, not the one selected before.
		const feathers::DeviceInstanceId lit = idAt(1);
		const feathers::DeviceInstanceId other = idAt(0);
		REQUIRE(keyFromFocus(focus, visage::KeyEvent { visage::KeyCode::Delete, 0, true }));
		REQUIRE(f.bridge.modRack().count == 1);
		CHECK(f.bridge.modRack().device[0].instanceId == other);
		CHECK(f.bridge.modRack().device[0].instanceId != lit);
		agree("after Delete");

		// A SECOND PRESS ON THE SAME CHIP DISARMS AND STILL SELECTS — the chip is the face either way.
		panel->clearSelection();
		scrollTo(0);
		const visage::Point chip0 = f.armPoint(sourceAt(0));
		pressAt(chip0);
		REQUIRE(mod->armed());
		pressAt(chip0);
		CHECK_FALSE(mod->armed());
		CHECK(panel->selectedInstanceForTest() == idAt(0));
		agree("after disarming from the chip");
	}

	SUBCASE("a FOLDED card's head — the card's own, now that a folded card is drawn (backlog CZ)") {
		selectOther(0);
		REQUIRE(panel->setCardMinimised(sourceAt(0), true));
		scrollTo(0);
		const visage::Bounds head = panel->foldBounds(sourceAt(0));
		const visage::Point at = inWindow(head, 0.4f, 0.5f);
		REQUIRE(f.root->frameAtPoint(at) == static_cast<visage::Frame*>(cardAt(0)));
		pressAt(at);
		CHECK(panel->selectedInstanceForTest() == idAt(0));
		CHECK(panel->cardMinimised(sourceAt(0)));
		agree("after a press on a folded head");
	}

	SUBCASE("a press OFF every card leaves the selection where it was, and the outline with it") {
		selectOther(1); // selects card 0
		// THE MACRO ROW'S FIRST KNOB — the strip's own furniture, no card under it.
		ui::Knob* macro = f.knobFor(static_cast<ParamId>(feathers::macroParam(0)));
		REQUIRE(macro != nullptr);
		const visage::Point where = macro->positionInWindow()
		                            + visage::Point { macro->width() * 0.5f, macro->faceDiameter() * 0.5f };
		visage::Frame* focus = pressAt(where);
		CHECK(focus == static_cast<visage::Frame*>(panel));
		CHECK(panel->selectedInstanceForTest() == idAt(0));
		agree("after a press on a macro knob");
		// AND DELETE FROM HERE TAKES THE OUTLINED CARD, which is the one the strip says is selected.
		const feathers::DeviceInstanceId outlined = idAt(0);
		REQUIRE(keyFromFocus(focus, visage::KeyEvent { visage::KeyCode::Delete, 0, true }));
		REQUIRE(f.bridge.modRack().count == 1);
		CHECK(f.bridge.modRack().device[0].instanceId != outlined);
		agree("after Delete from the panel");
	}
}

