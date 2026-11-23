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

// PIECE 3 OF 18 of the devices suite: its cases in their original order, with the helpers only they use. See devices_common.h.

#include "devices/devices_common.h"

TEST_CASE("automod strip: it watches the phase itself, and asks for a frame only when it moved") {
	AutomodStripFixture f;
	ui::AutomodStrip* pict = f.card->strip();
	REQUIRE(pict != nullptr);

	// **CONTENT-GATED, WHICH IS THE PROPERTY THAT MAKES THE LIVE STRIP AFFORDABLE.** The alternative
	// — raising the bridge's whole-tree repaint flag from a block-rate publisher — measured at 81% of
	// a core, and taking it off measured at -53%. So a still LFO must cost exactly nothing.
	f.bridge.publishAutomodReadings(f.slot, 0.0f, 0.0f);
	(void)pict->update(); // settle: the strip may not have drawn 0 yet
	CHECK_FALSE(pict->update());
	CHECK_FALSE(pict->update());

	f.bridge.publishAutomodReadings(f.slot, 0.25f, 0.0f);
	CHECK(pict->update());
	// AND ONCE, not once per ask: the second call has nothing new to report.
	CHECK_FALSE(pict->update());

	// THE CARD FORWARDS, and the lane counts what the card returns.
	f.bridge.publishAutomodReadings(f.slot, 0.5f, 0.0f);
	CHECK(f.card->update());
	CHECK_FALSE(f.card->update());

	// **AND A FOLDED CARD REPORTS NOTHING.** `redraw()` is refused for an invisible frame, so a
	// `true` here would be a frame TOLD rather than a frame DRAWN — the distinction playbook 8v
	// exists for, and `FxLane::updateCards()` counts exactly this.
	f.card->setFolded(true);
	f.bridge.publishAutomodReadings(f.slot, 0.75f, 0.0f);
	CHECK_FALSE(f.card->update());
	f.card->setFolded(false);
	CHECK(f.card->update());

	// AND THE LANE'S OWN COUNT REACHES IT, which is the road out of `Gui::Impl::tick()`. A count
	// produced BY the work is the only thing that cannot be had without it.
	f.bridge.publishAutomodReadings(f.slot, 0.9f, 0.0f);
	CHECK(f.lane->updateCards() >= 1);
	CHECK(f.lane->updateCards() == 0);
}
TEST_CASE("automod strip: the derive runs once per change, not once per read") {
	namespace amd = feathers::fx::automod;
	AutomodStripFixture f;
	f.card->setPicture(ui::AutomodCard::Picture::Dual);

	// **THE COUNT IS THE ONLY OBSERVABLE.** A gated derive and an ungated one return identical
	// numbers, so nothing about the traces can tell them apart — see `deriveCountForTest`.
	(void)f.card->view();
	const size_t after = f.card->deriveCountForTest();
	for (int i = 0; i < 20; ++i) {
		(void)f.card->previewTrace();
		(void)f.card->responseTrace();
	}
	CHECK(f.card->deriveCountForTest() == after);

	// AND A MOVED CONTROL RE-DERIVES — the half that stops the gate being a way to draw stale.
	f.set(amd::Param::Mod, AutomodStripFixture::kMod + 0.11f);
	(void)f.card->view();
	CHECK(f.card->deriveCountForTest() > after);
	CHECK(f.card->settings().mod == doctest::Approx(AutomodStripFixture::kMod + 0.11f));
}
TEST_CASE("automod card: the performance grid is 3 then 4, with the coordinates beneath it") {
	namespace amd = feathers::fx::automod;
	AutomodStripFixture f;
	REQUIRE(f.card->layout() == ui::AutomodCard::Layout::Compact);

	// **THE GRID ITSELF**, asked of the one place that decides it. Compact, by choice.
	//
	// **TWO ROWS, 3 THEN 4 — BACK WHERE IT WAS BEFORE `Env` ARRIVED.** Trim cluttered the card and
	// belongs in the window. The window now exists, so it went, and the grid
	// returns to seven.
	//
	// THE TWO ROWS REMAIN A HEIGHT BUDGET AND THE HISTORY STAYS HERE, because the pressure will come
	// back the next time somebody wants an eighth control: `{3,4,1}` was tried, and three performance
	// rows plus the coordinate row is four, which the open-height assertion further down this case
	// rejects — measured at 396px against its 330px bar. This case caught that, which is exactly why
	// the grid is pinned by name rather than merely summed.
	const std::span<const size_t> rows = ui::AutomodCard::performanceRows(f.card->layout());
	REQUIRE(rows.size() == 2);
	CHECK(rows[0] == 3);
	CHECK(rows[1] == 4);
	size_t total = 0;
	for (const size_t n : rows) {
		total += n;
	}
	CHECK(total == ui::AutomodCard::kBottomCells);

	// **THE PERFORMANCE CELLS, WHICH ARE NO LONGER ALL KNOBS.** Sync and LFO Mode are `ValueBox`es —
	// enumerations wearing knobs was the defect — so the grid is asked of the CELLS the layout placed
	// rather than of the knob vector, which now holds five.
	const auto& cells = f.card->cellsForTest();
	REQUIRE(cells.size() == ui::AutomodCard::kTopCells + ui::AutomodCard::kBottomCells);
	const auto perf = [&](size_t i) { return cells[ui::AutomodCard::kTopCells + i]; };

	// TWO ROWS, BY THEIR y: three cells on the first, four on the second.
	const float rowOneY = perf(0)->y();
	const float rowTwoY = perf(3)->y();
	CHECK(rowTwoY > rowOneY);
	for (size_t i = 4; i < ui::AutomodCard::kBottomCells; ++i) {
		CAPTURE(i);
		CHECK(perf(i)->y() == doctest::Approx(rowTwoY).epsilon(0.2));
	}

	// AND THE ROW OF THREE IS WIDER PER CELL THAN THE ROW OF FOUR, which is what gives the long
	// caption somewhere to live. Measured on the KNOBS, whose bounds ARE their cell's; a choice box is
	// inset inside its cell exactly as gamma is, so its width is not the cell's.
	const auto& knobs = f.card->knobsForTest();
	REQUIRE(knobs.size() == ui::AutomodCard::kBottomCells - 2);
	CHECK(knobs[0]->width() > knobs[1]->width());

	// **SYNC AND LFO MODE ARE BOXES, AND BOTH SIT IN THE ROW OF THREE.** The second half is the
	// constraint the grid exists for: `LFO MODE`'s caption is 53.32px against a four-wide cell of 54,
	// so it must land in a row of three however it is drawn. Every other caption clears 54 with room
	// (MIX 17.5 up to MANUAL 41.8).
	REQUIRE(f.card->choicesForTest().size() == 2);
	for (const auto& box : f.card->choicesForTest()) {
		CHECK(box->y() < rowTwoY);
	}

	// **AND THE COORDINATES SIT BENEATH EVERY PERFORMANCE CONTROL** — Type, Flavor and
	// Mod in the bottom row. Reading down: the picture, what plays it, what authors it.
	float lowestKnob = 0.0f;
	for (const auto& k : knobs) {
		lowestKnob = std::max(lowestKnob, k->y() + k->height());
	}
	REQUIRE_FALSE(f.card->coordsForTest().empty());
	for (const auto& c : f.card->coordsForTest()) {
		CHECK(c->y() >= lowestKnob - 1.0f);
	}
	CHECK(f.card->gammaForTest()->y() >= lowestKnob - 1.0f);

	// AND THE CARD IS SIZED FOR THE GRID IT HAS, not for the taller one. `openHeight` was static and
	// reserved the worst case, which the photograph showed as ~90px of dead space below the card.
	CHECK(f.card->openHeight() < ui::AutomodCard::kStripHeight + 4.0f * Knob::columnHeight(
	                                 ui::AutomodCard::kKnobDiameter));
}
TEST_CASE("automod card: gamma's drag is sixteen laps, not its whole seven-decade range") {
	namespace amd = feathers::fx::automod;
	AutomodStripFixture f;
	const ui::ValueBox* gamma = f.card->gammaForTest();
	REQUIRE(gamma != nullptr);

	// **WITHOUT A SPAN A `ValueBox` DRAGS ITS WHOLE DECLARED RANGE ACROSS `kDragRange`.** For gamma
	// that is 0..16,777,215 over 140px — 119,837 laps per pixel, one drag crossing everything, which
	// is why it felt far too fast. The span is what makes a lap a gesture rather than a
	// rounding error.
	CHECK(gamma->dragSpanForTest() == doctest::Approx(16.0));

	// PIXELS PER LAP, which is the number a hand feels — asked through the widget's own arithmetic
	// rather than restated, so a change to `kDragRange` moves this with it.
	const double pxPerLap = ui::ValueBox::kDragRange / gamma->dragSpanForTest();
	CHECK(pxPerLap > 5.0);
	CHECK(pxPerLap < 20.0);

	// **AND THE TOP IS OUT OF REACH, ASSERTED RATHER THAN IMPLIED.** A declared maximum no gesture
	// can reach is only acceptable when it is stated, and this is the statement: crossing the
	// declared range takes more full drags than anyone will ever make. The cap is a WALL — at 2^24 one
	// float ulp is 2, so a lap moves nothing — not a destination.
	const double span = amd::kParamDescs[static_cast<size_t>(amd::Param::Gamma)].max
	                    - amd::kParamDescs[static_cast<size_t>(amd::Param::Gamma)].min;
	CHECK(span / gamma->dragSpanForTest() > 1.0e6);

	// AND THE ONE GESTURE THAT DOES REACH A KNOWN VALUE STILL WORKS: double-click resets to the
	// declared default, which is the only way back to a stated number by hand.
	CHECK(amd::kParamDescs[static_cast<size_t>(amd::Param::Gamma)].def == doctest::Approx(0.0));

	// **THE CONSTANT IS THE SEATING'S, NOT THE CARD'S.** Read off `GammaBox` rather than restated as
	// 16 here, so moving it moves this with it — the card used to own it as a file-local constant in
	// `automod_card.cpp`, which is precisely why the window could not reuse it and drew a knob.
	CHECK(gamma->dragSpanForTest() == doctest::Approx(ui::GammaBox::kLapsPerDrag));
}
// ---------------------------------------------------------------------------------------------
// EACH HOST ANSWERS ITS OWN SLOT SPACE, AND THE PLAYHEAD IS WHERE THAT WAS WRONG.
//
// Feathers has TWO racks and `backplane::DeviceHost` has ONE slot index. `BridgeDeviceHost` addresses
// the FX chain; `ModRackDeviceHost` addresses the mod rack. **`BridgeDeviceHost::playheadAt` used to
// answer `rackPlayhead(slot)` — a MOD-RACK-keyed channel — from the fx host.** Unreached, because
// nothing asks an fx device for a playhead today; armed, because the automod strip has a
// playhead-shaped reading and the obvious next caller would have got the mod rack's ENVELOPE instead.
// With four rack slots against eight fx ones the low slots COLLIDE and the high ones decline: partial,
// plausible and silent.
//
// **THERE WAS NO INSTRUMENT AT ALL** — `playheadAt` was covered by exactly one case, in the standalone
// rack engine, asserting its decline. Both of Feathers' hosts were unpinned. These are that pin, and
// the ASYMMETRY is the assertion: same slot number, two hosts, two different answers.
// ---------------------------------------------------------------------------------------------

TEST_CASE("the host answers the mod rack's playhead, keyed by its devices' ids") {
	ModFixture f;
	// **A SECOND MODULATOR, BECAUSE THE HOST IS ADDRESSED BY DEVICE (BX step 4, then kill-lanes step
	// 2) AND AN EMPTY SLOT HAS NO DEVICE.** This case published playheads on two slots and read them
	// back by slot NUMBER,
	// which worked while a number was all the host needed. Under lane addressing the second read is a
	// question about a device, so there has to BE one — and that is not a weakening: publishing a
	// playhead for a slot holding nothing was always a channel with no device behind it, and the case
	// was quietly relying on the mirror not caring.
	REQUIRE(f.bridge.addRackDevice(feathers::mod::mseg::kTraits.tag) < feathers::kMaxRackSlots);
	f.bridge.refresh();
	REQUIRE(f.bridge.modRack().count >= 2);

	// DISTINCT PER SLOT, so a host reading the wrong KEY is caught rather than a host reading the
	// wrong channel. Identical values would let slot 0 stand in for slot 1 undetected.
	f.bridge.publishRackPlayhead(0, mod::Playhead { 2, 0.25f });
	f.bridge.publishRackPlayhead(1, mod::Playhead { 5, 0.75f });

	// AND THE TWO IDS ARE DIFFERENT NUMBERS FROM THE TWO SLOTS, said before they are relied on —
	// otherwise "read by id" and "read by slot" would be the same test.
	const DeviceInstanceId first = f.bridge.rackDeviceAt(0);
	const DeviceInstanceId second = f.bridge.rackDeviceAt(1);
	REQUIRE(first != 0);
	REQUIRE(second != 1);
	REQUIRE(first != second);

	uint32_t ordinal = mod::Playhead::kIdle;
	float phase = -1.0f;
	REQUIRE(f.deviceHost.playheadAt(first, ordinal, phase));
	CHECK(ordinal == 2u);
	CHECK(phase == doctest::Approx(0.25f));

	REQUIRE(f.deviceHost.playheadAt(second, ordinal, phase));
	CHECK(ordinal == 5u);
	CHECK(phase == doctest::Approx(0.75f));
}
TEST_CASE("the fx host has no playhead to give, and does not borrow the mod rack's") {
	// **THIS IS THE CASE THAT WOULD HAVE FAILED BEFORE THE FIX**, and it fails in the most dangerous
	// way: by SUCCEEDING with somebody else's number. An fx slot and a rack slot are both just an
	// index, so nothing about the call announces which space it meant.
	//
	// An fx device's live values travel by `readingAt` — automod's phase is reading 0 there. A
	// playhead is the modulator tier's shape, so the fx host having one at all was the error.
	ModFixture f;
	f.bridge.publishRackPlayhead(0, mod::Playhead { 3, 0.5f });

	// AN FX DEVICE, BY ITS ID — one host serves both chains now, so the question is about the DEVICE
	// being an effect rather than about which host was asked.
	const DeviceInstanceId effect = f.bridge.fxDeviceAt(0);
	REQUIRE(effect != kNoInstance);
	REQUIRE(f.bridge.rackSlotOf(effect) >= feathers::kMaxRackEnvelopes);

	uint32_t ordinal = 99u;
	float phase = -1.0f;
	CHECK_FALSE(f.deviceHost.playheadAt(effect, ordinal, phase));
	// THE OUT-PARAMETERS ARE UNTOUCHED BY A DECLINE, which is what lets a caller seed them with idle
	// and trust the seed. `MsegCard` depends on exactly that: ordinal 0 is a VALID SEGMENT for an
	// MSEG, so a host that returned true without writing would draw a marker frozen at segment 0.
	CHECK(ordinal == 99u);
	CHECK(phase == doctest::Approx(-1.0f));
}
TEST_CASE("in Feathers too, Delete removes the focused device — and this chain compacts") {
	// it was asked for delete-with-focus about EFFECTS, not about one host. The rack got it first; a
	// gesture that works there and not here is worse than one that works in neither, because the hand
	// learns it and then it fails somewhere.
	//
	// **AND THE TWO HOSTS DIFFER IN A WAY THAT MATTERS TO EXACTLY THIS FEATURE.** A rack slot is a
	// fixed position — routes are keyed by index, so `removeDevice` clears in place and the neighbours
	// do not move. THIS chain COMPACTS: everything below the hole slides up one. So a focus index kept
	// across a removal does not merely name an empty slot here, it names a DIFFERENT DEVICE, and
	// holding Delete would walk the chain deleting things nobody chose. That asymmetry is the reason
	// this case exists separately rather than being assumed to follow from the rack's.
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);

	const auto tags = [&] {
		std::vector<std::string> out;
		const feathers::FxChain chain = f.bridge.fxChain();
		for (size_t i = 0; i < chain.count; ++i) {
			out.emplace_back(fxDeviceAtPosition(chain, i).tag().data());
		}
		return out;
	};

	REQUIRE(f.bridge.addFxDevice(fx::owltt::kTraits.tag) == 1);
	REQUIRE(f.bridge.addFxDevice(fx::shaper::kTraits.tag) == 2);
	lane->resized();
	const std::vector<std::string> before = tags();
	REQUIRE(before.size() >= 3);

	const auto press = [&](visage::KeyCode code) {
		const visage::KeyEvent e { code, 0, true };
		return lane->onKeyPress().callback(e);
	};

	SUBCASE("nothing focused, key passed on") {
		REQUIRE(lane->focusedSlotForTest() == ui::FxLane::kNoFocus);
		CHECK_FALSE(press(visage::KeyCode::Delete));
		CHECK(tags() == before);
	}

	SUBCASE("the focused one goes and the rest close up") {
		lane->focusSlot(1);
		CHECK(press(visage::KeyCode::Delete));
		const std::vector<std::string> after = tags();
		REQUIRE(after.size() == before.size() - 1);
		CHECK(after[0] == before[0]);
		// THE COMPACTION, NAMED. This is the assertion the rack's case deliberately inverts.
		CHECK(after[1] == before[2]);
	}

	SUBCASE("focus does not survive, so a held key cannot walk the chain") {
		lane->focusSlot(1);
		REQUIRE(press(visage::KeyCode::Delete));
		CHECK(lane->focusedSlotForTest() == ui::FxLane::kNoFocus);
		// **THE SECOND PRESS TAKES NOTHING.** Without the reset it would take `before[2]`, which slid
		// into slot 1 — a device the hand never touched, removed with exactly the same evidence a
		// correct run leaves behind.
		CHECK_FALSE(press(visage::KeyCode::Delete));
		CHECK(tags().size() == before.size() - 1);
	}

	SUBCASE("Backspace is the same verb, and a press focuses") {
		ui::FxCard* card = lane->cardAt(1);
		REQUIRE(card != nullptr);
		lane->scrollCardIntoView(1);
		lane->resized();
		card->onMouseDown().callback(at(card->width() * 0.5f, ui::FxCard::kSlotHeadHeight * 0.5f));
		CHECK(lane->focusedSlotForTest() == 1);
		CHECK(press(visage::KeyCode::Backspace));
		CHECK(tags().size() == before.size() - 1);
	}
}
TEST_CASE("the fx lane's Backspace takes the card under the hand - even when the press landed on a control") {
	// **THE FEATHERS HALF OF ONE FIX.** it was reported it on a modulator; the mechanism is `FxCard`'s
	// and `FxCard` is both hosts', so the property is asserted in both. The rack's twin is
	// `tests/test_rack_editor.cpp`, "Backspace takes the card under the hand".
	//
	// `onCardPressed` — what tells this lane which slot a key acts on — fires from `noteCardPress`,
	// which runs only for a press the CARD frame itself receives. `visage::Frame::propagateMouseEvent`
	// gives a press to an ancestor only if it asked with `setReceiveChildMouseEvents`, and nothing in
	// this plugin does; a card is mostly its knobs and boxes. So a press that a hand reads as "I picked
	// this card" landed on a child, the card never heard it, and `focusedSlot_` went on naming the card
	// whose bare body was pressed LAST — while the keyboard focus moved all the same, because
	// `handleMouseDown` walks to the nearest keystroke-accepting ANCESTOR.
	//
	// **THE SURVIVOR'S ROUTES ARE THE GHOST, AND THEY ARE READ AS A VALUE.** A device that is never
	// removed never has `clearDeviceSlotRoutes` run on its lane, so it goes on modulating. "The chain
	// got shorter" is true on the broken build too — something always went, and it was the wrong thing.
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	REQUIRE(f.bridge.addFxDevice(fx::harm::kTraits.tag) == 1);
	REQUIRE(f.bridge.addFxDevice(fx::filter::kTraits.tag) == 2);
	lane->resized();
	REQUIRE(lane->deviceCount() == 3);

	// THE ROUTE INTO THE DEVICE THE HAND WILL PRESS, addressed by its ID — the only identity that
	// survives a removal above it.
	const backplane::Source kSource = backplane::Source::core(0); // Macro 1
	constexpr size_t kControl = static_cast<size_t>(fx::filter::Param::Cutoff);
	const DeviceInstanceId pressed = f.bridge.fxDeviceAt(2);
	REQUIRE(pressed != kNoInstance);
	const backplane::Destination destination = backplane::Destination::control(pressed, kControl);
	REQUIRE(f.bridge.mayRoute(kSource, destination));
	f.bridge.setRouteAmount(kSource, destination, 0.5f);
	REQUIRE(f.bridge.routeAmount(kSource, destination) == doctest::Approx(0.5f));

	// **A DIFFERENT CARD IS FOCUSED FIRST, the way a session actually goes** — one device was selected,
	// then the hand moved to another. Without it the stale index is `kNoFocus` and the broken build
	// simply does nothing, which is a milder failure than the one seen in use: the key would take HARM,
	// the device nobody was pointing at, and leave the filter modulated.
	lane->focusSlot(1);
	REQUIRE(lane->focusedSlotForTest() == 1);

	lane->scrollCardIntoView(2);
	lane->resized();
	ui::FxCard* card = lane->cardAt(2);
	REQUIRE(card != nullptr);
	visage::Frame* control = nullptr;
	for (visage::Frame* child : card->children()) {
		if (child->isVisible() && child->width() > 2.0f && child->height() > 2.0f) {
			control = child;
			break;
		}
	}
	REQUIRE(control != nullptr);

	// **BOTH HALVES OF `WindowEventHandler::handleMouseDown`.** `dragThroughRoot` delivers the press
	// and does NOT tell the new focus frame it has the focus, and `keyboardFocusAfterPress` computes
	// that frame without telling it either — which is the gap CP2 recorded between two sessions'
	// readings of this road. The focus notification IS the road under test here, so it is performed.
	const visage::Point where = control->positionInWindow()
	                            + visage::Point { control->width() * 0.5f, control->height() * 0.5f };
	visage::Frame* focus = keyboardFocusAfterPress(f.root, where);
	REQUIRE(focus != nullptr);
	// THE MECHANISM, PINNED: the first keystroke-accepting frame above that control is the CARD.
	CHECK(focus == static_cast<visage::Frame*>(card));
	focus->processFocusChanged(true, true);
	dragThroughRoot(f.root, where, where);

	CHECK(lane->focusedSlotForTest() == 2);

	// AND THE KEY TRAVELS FROM THE FOCUS UPWARD, which is what proves the lane still answers it after
	// the card started taking the focus. A card that swallowed Delete would break this feature in the
	// act of fixing its target.
	REQUIRE(keyFromFocus(focus, visage::KeyEvent { visage::KeyCode::Backspace, 0, true }));

	CHECK(lane->deviceCount() == 2);
	// **THE VALUE.** 0.5 on the broken build: the filter was never removed, so its lane was never
	// cleared and its cutoff is still being modulated.
	CHECK(f.bridge.routeAmount(kSource, destination) == doctest::Approx(0.0f));
	// AND THE TWO THE HAND DID NOT CHOOSE ARE STILL THERE, by tag — the half that catches "removed the
	// focused one after all" and "removed more than one".
	//
	// THROUGH `sameTag`, WHICH COMPARES THE FOUR BYTES. A `std::string` built from `tag().data()` is
	// not NUL-terminated — it read "HARM" plus whatever followed and compared unequal to "HARM" while
	// doctest printed both sides as `HARM`. The chain's own tag storage is a fixed four bytes and this
	// is the comparison written for it.
	const feathers::FxChain chain = f.bridge.fxChain();
	REQUIRE(chain.count == 2);
	CHECK(device::sameTag(fxDeviceAtPosition(chain, 1).tag().data(), fx::harm::kTraits.tag));
}
TEST_CASE("automod strip: the phase star wears the comb's pitch as a tint and its amount as a glow") {
	namespace amd = feathers::fx::automod;
	// The phase star glows with a tint set by the comb's frequency (red low, blue high) and a glow
	// radius set by the comb's intensity.
	//
	// **ASSERTED AS VALUES, NOT FROM A PICTURE.** "The star is blue here and red there" is a claim
	// about a colour, and CLAUDE.md's rule is that such a claim cannot be inferred from a rendered
	// buffer — a capture proves something differs, never that the right thing does. `combGlow()` is
	// the function `drawLfo` itself calls, so there is no second arithmetic for these to disagree
	// with.
	AutomodStripFixture f;
	ui::AutomodStrip* strip = f.card->strip();
	REQUIRE(strip != nullptr);

	// THE LFO VALUE IS PASSED, NOT INHERITED FROM WHEREVER THE STAR HAPPENS TO BE. The tint follows
	// the comb's pitch AS MODULATED, by design — so `combGlow` takes
	// the LFO's value at the mark. Holding it at 0 here is what makes the pitch subcases about the
	// BASE tuning; the sweep gets a subcase of its own below.
	const auto glowAt = [&](float type, float flavour, float lfo = 0.0f) {
		f.set(amd::Param::Type, type);
		f.set(amd::Param::Flavor, flavour);
		return strip->combGlow(lfo);
	};
	const auto blueness = [](uint32_t argb) {
		// How far toward blue, as the blue channel's lead over the red one. A signed number rather
		// than two comparisons, so "more blue than that one" is one assertion.
		return static_cast<int>(argb & 0xff) - static_cast<int>((argb >> 16) & 0xff);
	};

	SUBCASE("a short comb delay reads blue and a long one reads red") {
		// MEASURED positions, from a sweep of the Flavour axis at full Type: flavour 0.80 puts the
		// base delay at the floor of 6 samples — the highest pitch the comb reaches — and flavour 0.0
		// puts it at 1050, near the other end. Both have the same comb MIX, so this subcase moves the
		// pitch and holds the amount, which is what makes it about the tint alone.
		const ui::AutomodStrip::CombGlow high = glowAt(1.0f, 0.80f);
		const ui::AutomodStrip::CombGlow low = glowAt(1.0f, 0.0f);

		CHECK(blueness(high.tint) > 0);  // short delay -> blue
		CHECK(blueness(low.tint) < 0);   // long delay  -> red
		// AND THE GAP IS A MECHANISM RATHER THAN A ROUNDING DIFFERENCE.
		CHECK(blueness(high.tint) - blueness(low.tint) > 200);
	}

	SUBCASE("more comb opens the glow, and the two are independent of the tint") {
		// Type is what sets `comb.mix`; Flavour sets the delay. Measured across the Type axis: 1.0
		// gives mix 0.914 and 0.20 gives 0.183, at one fixed Flavour so the pitch does not move.
		const ui::AutomodStrip::CombGlow loud = glowAt(1.0f, 0.30f);
		const ui::AutomodStrip::CombGlow quiet = glowAt(0.20f, 0.30f);

		CHECK(loud.halo > quiet.halo);
		// NON-VACUITY: both are real haloes rather than one having collapsed to the fallback.
		CHECK(quiet.halo > 0.0f);
		CHECK(loud.halo > 1.5f);
	}

	SUBCASE("the tint follows the sweep, because the comb's pitch does") {
		// **THE MODULATION IS THE POINT.** `Comb::delayFor` adds `modRangeSamples * lfo` to the base,
		// so the pitch a listener hears moves with the LFO — and a tint that reported only the base
		// would be naming a frequency the comb is at twice a cycle and away from the rest of the time.
		//
		// ASSERTED THROUGH THE SAME FUNCTION THE DSP USES. `combDelayFor` is what `Comb::delayFor`
		// calls, so this cannot pass against a surface that models the modulation differently from the
		// sound.
		// MEASURED: at Type 1.0 the comb is fully mixed, and Flavour 0.10 gives a base delay of 566
		// samples with a modulation range of 247 — a sweep from 318.6 to 813.4 that touches NEITHER
		// end of the line. Chosen unclamped deliberately, so this subcase is about the direction of
		// the mapping and not about the flattening at the extremes. The first coordinate tried here
		// had `modRangeSamples` of exactly 0 and the guard below caught it, which is the field being
		// sparse rather than the case being wrong.
		f.set(amd::Param::Type, 1.0f);
		f.set(amd::Param::Flavor, 0.10f);
		const amd::Settings st = f.card->settings();
		const amd::AutomodDerived d = amd::deriveAutomod(amd::coordsOf(st, amd::nominalRateHz(st)));

		// NON-VACUITY FIRST: a coordinate whose comb does not MOVE would make this subcase empty, and
		// the field is sparse enough that such coordinates exist.
		REQUIRE(d.comb.modRangeSamples > 1.0f);
		REQUIRE(d.comb.mix > 0.02f);

		const ui::AutomodStrip::CombGlow down = strip->combGlow(-1.0f);
		const ui::AutomodStrip::CombGlow up = strip->combGlow(+1.0f);

		// A LONGER DELAY IS A LOWER PITCH IS REDDER. +1 of LFO lengthens the delay, so the mark at the
		// top of the sweep is the RED end — which is the sign this whole mapping turns on.
		CHECK(blueness(up.tint) < blueness(down.tint));
		// AND THE AMOUNT DOES NOT MOVE WITH IT: the glow is `comb.mix`, which the LFO does not touch.
		CHECK(up.halo == doctest::Approx(down.halo));
	}

	SUBCASE("an inaudible comb gets no colour of its own") {
		// **THE HONESTY RULE, IN A COLOUR.** `comb.mix` is how much comb is ADDED, so at zero there is
		// no comb to have a pitch — and a star tinted for a frequency nothing is playing would be a
		// surface reporting a mechanism the sound is not running. Type 0.0 measures mix at 0.000.
		const ui::AutomodStrip::CombGlow silent = glowAt(0.0f, 0.80f);
		CHECK(silent.tint == ui::kAccent);
		CHECK(silent.halo == doctest::Approx(1.0f));

		// AND THE SAME PLACE WITH THE COMB AUDIBLE IS NOT THE FALLBACK, which is what stops the
		// subcase above from passing on a device where the tint never changes at all.
		const ui::AutomodStrip::CombGlow heard = glowAt(1.0f, 0.80f);
		CHECK(heard.tint != ui::kAccent);
	}
}
TEST_CASE("automod strip: the envelope trace is drawn as strongly as the envelope is steering") {
	namespace amd = feathers::fx::automod;
	// it was asked for the trace's opacity to follow the envelope's influence, and for an ink that is
	// not the one it was sharing with the LFO waypoint marks.
	//
	// **ASSERTED AS A VALUE, AND THAT IS NOT OPTIONAL HERE.** An opacity pinned at 1.0 draws EXACTLY
	// the same pixels as a correct one wherever the steering happens to be full, so a photograph can
	// confirm the trace exists and can never confirm it is modulated. `envTrace()` is the function
	// `drawEnvelope` itself calls.
	AutomodStripFixture f;
	ui::AutomodStrip* strip = f.card->strip();
	REQUIRE(strip != nullptr);

	SUBCASE("the ink is not shared with anything else on this strip") {
		const ui::AutomodStrip::EnvTrace t = strip->envTrace();
		// THE THREE IT MUST NOT BE. `kInk` is the waypoint marks, which it WAS; `kAccent` is the LFO
		// waveform; `kValue` is the response curve.
		CHECK(t.ink != ui::kInk);
		CHECK(t.ink != ui::kAccent);
		CHECK(t.ink != ui::kValue);

		// AND IT MUST STAY CLEAR OF THE STAR, WHICH MOVES. Distinctness from the star's colour today
		// is not enough — the tint sweeps red to blue with the comb, so the trace has to differ from
		// BOTH ENDS or it collides once a cycle. Asserted as a green channel that leads both others,
		// which neither end of a red..blue ramp can ever satisfy.
		const int r = static_cast<int>((t.ink >> 16) & 0xff);
		const int g = static_cast<int>((t.ink >> 8) & 0xff);
		const int b = static_cast<int>(t.ink & 0xff);
		CHECK(g > r);
		CHECK(g > b);
	}

	// THE ENVELOPE IS A PUBLISHED READING, NOT A DERIVED ONE, so a headless fixture has to supply it.
	// The first draft of this case drove `EnvTrim` instead and every alpha came back zero — the trim
	// shapes the DSP's FOLLOWER, and there is no DSP here. `envelope()` reads reading 1, so that is
	// the lever, exactly as the phase cases on this strip drive reading 0.
	const auto publish = [&](float envelope) {
		f.bridge.publishAutomodReadings(f.slot, 0.30f, envelope);
	};

	SUBCASE("opacity follows the envelope's level, at a fixed influence") {
		f.set(amd::Param::Mod, 0.90f); // measured: |envDepthInfluence| 0.811
		publish(0.9f);
		const float high = strip->envTrace().alpha;
		publish(0.2f);
		const float low = strip->envTrace().alpha;

		CHECK(high > low);
		// NON-VACUITY: the strong end is a real opacity rather than both being zero.
		CHECK(high > 0.05f);
		// AND IT IS AN OPACITY, not an unbounded product.
		CHECK(high <= 1.0f);
	}

	SUBCASE("and it follows the field's influence, at a fixed level") {
		// TWO INFLUENCES AT ONE LEVEL — the other half, and the reason `envTrace` cannot be a function
		// of the envelope alone. MEASURED on the Mod axis at this fixture's other coordinates:
		// 0.90 gives 0.811 and 0.60 gives 0.129.
		publish(0.9f);
		f.set(amd::Param::Mod, 0.90f);
		const float strong = strip->envTrace().alpha;
		f.set(amd::Param::Mod, 0.60f);
		const float weak = strip->envTrace().alpha;

		CHECK(strong > weak);
		CHECK(weak >= 0.0f);
	}

	SUBCASE("an envelope that steers nothing draws nothing") {
		// **THE HONESTY RULE IN AN ALPHA.** `automod.cpp` computes `steer = 1 + level * influence`, so
		// a level of zero steers nothing whatever the field says — and a trace lit at full strength
		// while sitting at rest would be claiming a mechanism the sound is not running. This is the
		// case the FIRST version of this feature failed: it read the influence alone, which stays
		// non-zero at rest.
		f.set(amd::Param::Mod, 0.90f); // the field says the envelope matters a great deal here
		publish(0.0f);                 // and it is doing nothing
		const float idle = strip->envTrace().alpha;

		// **AND IT IS DRAWN ANYWAY, FAINTLY — the second half of the decision.** Opacity by influence
		// came first, which made the trace vanish at rest; then the envelope got a faint floor, because
		// a vanished trace is indistinguishable from a build that has no trace at all.
		// BOTH BOUNDS ARE ASSERTED, and that is the point rather than pedantry: a floor of 1.0 draws
		// exactly like a correct one wherever the steering is full, and a floor that swallowed the
		// whole opacity would draw exactly like no floor at all. One assertion cannot separate those.
		CHECK(idle > 0.05f);  // visible: it is ink, not background
		CHECK(idle < 0.35f);  // faint: it cannot compete with curves that live at 0.75 to 0.90

		// AND THE SAME AT A COORDINATE WHERE THE FIELD SAYS NOTHING. MEASURED: Mod 0.70 puts
		// `envDepthInfluence` at exactly 0, so this is the floor arriving by the other road.
		f.set(amd::Param::Mod, 0.70f);
		publish(1.0f);
		CHECK(strip->envTrace().alpha == doctest::Approx(idle));

		// THE FLOOR IS A FLOOR AND NOT THE WHOLE RANGE: steering hard must still be clearly brighter.
		f.set(amd::Param::Mod, 0.90f);
		publish(1.0f);
		CHECK(strip->envTrace().alpha > idle + 0.3f);
	}
}
TEST_CASE("automod strip: the drawn filter curve moves with the Freq knob") {
	namespace amd = feathers::fx::automod;
	// **THIS WAS REPORTED TWICE.** First the cutoff knob did not move the drawn curve (though the XY
	// pad and the actual modulation both did), and then — after a fix that made the strip REDRAW when
	// settings changed — the curve still did not follow the cutoff knob. The redraw was never the problem. `coordsOf` does not carry `freq`, so nothing
	// downstream of `deriveAutomod` knew about it, and both places the strip computed a corner passed
	// a literal 1.0 where the DSP passes `filterPitchRatio_ * filterFreqRatio_`. The curve was drawn
	// as though the knob were permanently centred, and redrawing it produced the same curve.
	//
	// **THE FIRST FIX PASSED ITS OWN TEST.** A case that asserted "the strip redraws when a setting
	// changes" is true and useless here — it tests the gate, and the gate was working. This one reads
	// the CORNER, which is the quantity the report is about.
	AutomodStripFixture f;
	ui::AutomodStrip* strip = f.card->strip();
	REQUIRE(strip != nullptr);

	const auto cornerAt = [&](float freq) {
		f.set(amd::Param::Freq, freq);
		const ui::AutomodStrip::ResponseTrace t = strip->responseTrace();
		return t.liveHz;
	};

	const float low = cornerAt(-1.0f);
	const float mid = cornerAt(0.0f);
	const float high = cornerAt(+1.0f);

	// NON-VACUITY: all three are real corners rather than one having collapsed to a floor.
	REQUIRE(low > 0.0f);
	REQUIRE(high > 0.0f);

	// THE KNOB MOVES THE CORNER, and in the direction its own description promises — *"it brightens
	// the filter" — so more Freq is a higher corner.
	CHECK(high > mid);
	CHECK(mid > low);

	// AND BY THE AMOUNT THE DSP APPLIES, not merely by something. `kFreqOctaves` is 2 either way, so
	// the span is four octaves — a factor of 16 — before the [0.25, 4] clamp and the Nyquist ceiling
	// take their share. Asserted as a generous floor rather than an equality, because both bounds are
	// real and either may bite first depending on where the field put the base corner.
	CHECK(high / low > 3.0f);

	// THE RATIO ITSELF, THROUGH THE FUNCTION THE SOUND USES. This is what stops the case passing
	// against a surface that moves the corner by some invented amount of its own.
	CHECK(amd::filterFreqRatioFor(+1.0f) == doctest::Approx(4.0f));
	CHECK(amd::filterFreqRatioFor(-1.0f) == doctest::Approx(0.25f));
	CHECK(amd::filterFreqRatioFor(0.0f) == doctest::Approx(1.0f));
}
//------------------------------------------------------------------------------------------------//
// THE AUTOMOD PANEL'S GEOMETRY, for the reason the Shaper block above states: the reachability     //
// guard CONSTRUCTS a panel and never lays it out, so a panel whose pad had zero side or whose knob //
// row fell off the bottom passes it intact. Every case here asserts a rectangle or a value.        //
//------------------------------------------------------------------------------------------------//


TEST_CASE("the Automod panel lays out inside the size it asks for") {
	AutomodPanelFixture f;
	REQUIRE(f.panel != nullptr);

	const visage::Point min = f.panel->minimumSize();
	CHECK(min.x > 0.0f);
	CHECK(min.y > 0.0f);

	SUBCASE("every part is inside the panel") {
		const auto within = [&](const visage::Bounds& b, const char* what) {
			CAPTURE(what);
			CHECK(b.x() >= 0.0f);
			CHECK(b.y() >= 0.0f);
			CHECK(b.right() <= min.x + 0.5f);
			CHECK(b.bottom() <= min.y + 0.5f);
		};
		within(f.panel->padBounds(), "pad");
		// (NO "enable" ANY MORE: the square is the window header's chip, 2026-09-26.)
	}

	// **THE PAD IS SQUARE AND IT IS THE WHOLE POINT.** Its decode reads an ANGLE off the puck, so a
	// pad one pixel wider than tall makes 45 degrees on screen mean something else in the phases —
	// the puck and the spokes stop agreeing, and they agree in a way a reader trusts. `unitFrom`
	// divides by a single half-side for exactly this reason, and this is the assertion that the
	// layout hands it the shape it assumes.
	SUBCASE("the pad is square") {
		const visage::Bounds pad = f.panel->padBounds();
		CHECK(pad.width() > 1.0f);
		CHECK(pad.width() == doctest::Approx(pad.height()).epsilon(0.01));
	}

	SUBCASE("the knob row is below the pad and every cell has a real rectangle") {
		size_t placed = 0;
		for (const auto* child : f.panel->children()) {
			// **A `GammaBox` COUNTS AS A CELL AND IS NOT A KNOB**, which is the whole point of the
			// seating: gamma occupies a cell in this row and must not be a `Knob` in it. A check that
			// walked only knobs would report the row correctly laid out while ignoring the cell most
			// likely to be wrong.
			// **AND A `ZoneCoordCell` COUNTS TOO** — the widget automod's coordinates moved to when
			// their pairs collapsed (BJ piece 3). This listed the old `ZoneCoordBox`, so after the swap
			// the three coordinate cells stopped being counted and the case read `10 == 13`.
			const bool isCell = dynamic_cast<const ui::Knob*>(child) != nullptr
			                    || dynamic_cast<const ui::ZoneCoordCell*>(child) != nullptr
			                    || dynamic_cast<const ui::GammaBox*>(child) != nullptr
			                    || dynamic_cast<const ui::SwitchChip*>(child) != nullptr // Keytrack (backlog IS.5)
			                    || dynamic_cast<const ui::ValueBox*>(child) != nullptr; // Sync, LFO Mode: choices are boxes
			// **ROTATE IS A `GammaBox` AND NOT A CELL** — it sits inside the pad (backlog IJ), which
			// its own case asserts. Excluded by identity, not by type, so gamma is still counted.
			if (!isCell || child == f.panel->rotateForTest()) {
				continue;
			}
			++placed;
			CAPTURE(placed);
			CHECK(child->width() > 0.0f);
			CHECK(child->height() > 0.0f);
			CHECK(child->y() >= f.panel->padBounds().bottom());
			CHECK(child->bounds().bottom() <= min.y + 0.5f);
		}
		// **EVERY CELL IN THE GRID, NOT ONE ROW'S WORTH.** The panel grew from one knob row to four
		// when it became a superset of its card (by design), and the coordinate row's cells are
		// `ZoneCoordCell`s rather than knobs — a count that still said `kKnobCells` would have been
		// asserting the shape of a panel that no longer exists.
		CHECK(placed == ui::AutomodExpanded::kCellCount);
	}
}
TEST_CASE("the Automod pad writes TRAVEL through the plane, not two coordinates") {
	namespace amd = feathers::fx::automod;
	AutomodPanelFixture f;
	REQUIRE(f.panel != nullptr);
	const auto* pad = f.panel->padForTest();
	REQUIRE(pad != nullptr);
	REQUIRE(pad->width() > 1.0f);
	auto* mutablePad = const_cast<ui::AutomodPad*>(pad);

	const auto fieldOf = [&](size_t index) {
		return f.f.bridge.fxChain().device[f.slot].row.field[index];
	};
	const auto blobOf = [&](size_t i) { return fieldOf(amd::kFoldedTravelFirst + i); };
	const auto press = [&](float x, float y) { mutablePad->onMouseDown().callback(at(x, y)); };
	const auto drag = [&](float x, float y) { mutablePad->onMouseDrag().callback(at(x, y)); };
	const auto release = [&](float x, float y) { mutablePad->onMouseUp().callback(at(x, y)); };

	const float cx = pad->width() * 0.5f;
	const float cy = pad->height() * 0.5f;
	const float half = std::min(pad->width(), pad->height()) * 0.5f;

	// **THE PAD NO LONGER WRITES `Type` AND `Flavour`, AND THAT IS THE POINT.** It used to, which was
	// right while the axes WERE those two coordinates. The plane comes from the rotate generator now,
	// so X is not Type and a drag is a displacement in a 39-space that no pair of knobs expresses.
	SUBCASE("a drag moves the folded travel and leaves the coordinates alone") {
		const float typeBefore = fieldOf(static_cast<size_t>(amd::Param::Type));
		const float flavourBefore = fieldOf(static_cast<size_t>(amd::Param::Flavor));
		press(cx, cy);
		drag(cx + half * 0.5f, cy - half * 0.5f);
		release(cx + half * 0.5f, cy - half * 0.5f);

		CHECK(fieldOf(static_cast<size_t>(amd::Param::Type)) == doctest::Approx(typeBefore));
		CHECK(fieldOf(static_cast<size_t>(amd::Param::Flavor)) == doctest::Approx(flavourBefore));

		// AND THE BLOB CARRIES WHAT THE GESTURE COVERED — most coordinates, not one. A plane that had
		// collapsed to a line, or a write that reached only the first float, would fail here while a
		// "something changed" check passed.
		size_t moved = 0;
		for (size_t i = 0; i < amd::kFoldedTravelFloats; ++i) {
			if (blobOf(i) != 0.0f) {
				++moved;
			}
		}
		CAPTURE(moved);
		CHECK(moved * 2 > amd::kFoldedTravelFloats);
	}

	// AT REST THE PUCK IS AT THE CENTRE — the origin IS the current position, so "no displacement yet"
	// is the middle of the square whatever the device is set to.
	SUBCASE("the puck returns to centre after a gesture") {
		press(cx, cy);
		drag(cx + half * 0.7f, cy);
		release(cx + half * 0.7f, cy);
		mutablePad->finishGlideForTest(); // backlog IJ: the view glides home
		CHECK(pad->puck().x == doctest::Approx(cx).epsilon(0.01));
		CHECK(pad->puck().y == doctest::Approx(cy).epsilon(0.01));
	}

	// **THE MECHANISM ITSELF: A ROTATE BETWEEN TWO DRAGS SENDS THE SECOND SOMEWHERE THE FIRST DID
	// NOT.** This is only assertable once the pad writes through the basis, and it is what the whole
	// topological complex is for.
	//
	// **ASSERTED ON THE VECTOR, NOT ON A SUM.** `rotatePhases` preserved the phase SUM by construction,
	// and a sum assertion was green on that defect — so this compares the 39 coordinates one by one and
	// counts how many differ.
	SUBCASE("turning the plane between two drags changes where the second one goes") {
		const auto dragOnce = [&] {
			press(cx, cy);
			drag(cx + half * 0.6f, cy - half * 0.3f);
			release(cx + half * 0.6f, cy - half * 0.3f);
			mutablePad->finishGlideForTest();
			std::array<float, amd::kFoldedTravelFloats> out {};
			for (size_t i = 0; i < out.size(); ++i) {
				out[i] = blobOf(i);
			}
			return out;
		};
		const auto first = dragOnce();

		f.f.bridge.setDeviceControl(f.device, static_cast<size_t>(amd::Param::Rotate), 1.0f);
		f.panel->resized();
		const auto second = dragOnce();

		// THE SECOND DRAG'S CONTRIBUTION IS NOT THE FIRST'S. Both drags were the same gesture; only the
		// plane changed between them.
		size_t differing = 0;
		for (size_t i = 0; i < amd::kFoldedTravelFloats; ++i) {
			const double firstStep = static_cast<double>(first[i]);
			const double secondStep = static_cast<double>(second[i]) - firstStep;
			if (std::fabs(secondStep - firstStep) > 1e-4) {
				++differing;
			}
		}
		CAPTURE(differing);
		CHECK(differing * 2 > amd::kFoldedTravelFloats);
	}
}
TEST_CASE("the Automod terrain is the plane, and follows the puck") {
	// The terrain did not update while the puck moved. It did not, and it
	// could not: the map was built ONCE at construction from `activityAt(ux, uy)` — the Type x Flavour
	// coordinate square — while a drag commits travel through the 39-space plane. Nothing the puck did
	// was an input to the picture, so the picture had nothing to change for. The shaper's pad carried
	// exactly this defect and `the terrain is the plane, and follows it` is its case; this is the same
	// case for the same class of bug, one device over.
	//
	// **DRIVEN THROUGH THE TREE**, not by calling the pad's callbacks: `dragThroughRoot` resolves the
	// frame under the press and sends the three events the window would, so a pad that stopped
	// receiving them (a hit-test change, a parent that swallowed the press) fails here too.
	namespace amd = feathers::fx::automod;
	namespace ps = amd::phase_space;
	AutomodPanelFixture f;
	REQUIRE(f.panel != nullptr);
	auto* pad = const_cast<ui::AutomodPad*>(f.panel->padForTest());
	REQUIRE(pad != nullptr);
	REQUIRE(pad->width() > 1.0f);

	const auto rowField = [&](size_t index) {
		return f.f.bridge.fxChain().device[f.slot].row.field[index];
	};
	const auto snapshot = [&] { return pad->mapForTest(); };
	const auto differs = [](const auto& a, const auto& b) {
		size_t n = 0;
		for (size_t i = 0; i < a.size(); ++i) {
			if (a[i] != b[i]) {
				++n;
			}
		}
		return n;
	};

	const auto atRest = snapshot();
	// NON-VACUITY: the map has content at all. A terrain of all zeros would "follow" everything.
	bool anyInk = false;
	for (const float v : atRest) {
		anyInk = anyInk || v > 0.001f;
	}
	REQUIRE(anyInk);
	const size_t settled = pad->rebuildsForTest();

	// THE PAD'S CENTRE, IN THE PANEL'S COORDINATES. The panel sits at (0,0) so these are also window
	// coordinates, which is what `dragThroughRoot` measures its events in.
	const visage::Bounds padBounds = f.panel->padBounds();
	const visage::Point centre { padBounds.x() + padBounds.width() * 0.5f,
		                         padBounds.y() + padBounds.height() * 0.5f };
	const visage::Point away { centre.x + padBounds.width() * 0.3f, centre.y - padBounds.height() * 0.2f };

	SUBCASE("a drag through the tree moves the picture, because the plane's origin moved") {
		dragThroughRoot(f.panel, centre, away);
		pad->finishGlideForTest(); // backlog IJ: the frame re-origins when the glide lands

		// **THE TRAVEL LANDED** — checked first, so what follows is about the picture and not about a
		// gesture that never reached the pad.
		size_t moved = 0;
		std::array<double, amd::kFoldedTravelFloats> standingNow {};
		for (size_t i = 0; i < amd::kFoldedTravelFloats; ++i) {
			standingNow[i] = static_cast<double>(rowField(amd::kFoldedTravelFirst + i));
			if (standingNow[i] != 0.0) {
				++moved;
			}
		}
		CAPTURE(moved);
		REQUIRE(moved * 2 > amd::kFoldedTravelFloats);

		// **THE VALUE: THE TERRAIN WAS BUILT FOR WHERE THE PUCK NOW IS.** The map is a function of its
		// inputs and the standing travel is one of them, so the picture is current exactly when the
		// travel it was built from IS the row's travel. Read from the key the rebuild stamped, not
		// inferred from cells: a rebuild that happened to land on similar cells would pass a picture
		// comparison and still be a picture of the wrong place.
		CHECK(pad->rebuildsForTest() > settled);
		const ui::AutomodPad::View& built = pad->builtForTest();
		for (size_t i = 0; i < amd::kFoldedTravelFloats; ++i) {
			CAPTURE(i);
			CHECK(built.standing[i] == standingNow[i]);
		}

		// AND THE PICTURE DID MOVE — the second assertion, which keeps the first from being vacuous
		// on a rebuild that reads the key and then draws the old square anyway.
		CAPTURE(differs(atRest, snapshot()));
		CHECK(differs(atRest, snapshot()) > 0);
	}

	SUBCASE("one cell of the map is the plane's activity at that cell, restated independently") {
		// **NOT A DIFFERENCE, A NUMBER.** The cell's brightness is what the device would sound like if
		// you dragged there: home plus standing travel, carried along the basis by the cell's offset in
		// the same units a drag commits. Computed here from the phase space's own primitives and the
		// derive, so a map that sampled the coordinate square — or the plane at the wrong scale — reads
		// a different value.
		dragThroughRoot(f.panel, centre, away);
		pad->finishGlideForTest(); // backlog IJ: the frame re-origins when the glide lands
		const auto field = [&](amd::Param p) {
			return static_cast<double>(rowField(static_cast<size_t>(p)));
		};
		// ONE PARAM EACH SINCE THE COLLAPSE (BJ piece 3) — the same quantity, read from one field.
		ps::Phases home = ps::homeFor(field(amd::Param::Type), field(amd::Param::Flavor),
		                              field(amd::Param::Mod));
		ps::pushAlong(home, ps::kGammaAxis, field(amd::Param::Gamma));
		for (size_t i = 0; i < ps::kTriangles; ++i) {
			home[i] += static_cast<double>(rowField(amd::kFoldedTravelFirst + i));
		}
		const ps::Basis basis = ps::basisFor(field(amd::Param::Rotate), ps::planeHomeOf(home));
		const double carry = static_cast<double>(ui::AutomodPad::kZonesPerHalfDrag)
		                     / static_cast<double>(ui::automod_terrain::kZonesShown) / pad->zoom()
		                     // THE MAP IS BUILT `kMapMargin` WIDER THAN THE VIEW (backlog IJ), around the landed view.
		                     * ui::AutomodPad::kMapMargin;
		REQUIRE(pad->width() == pad->height());
		REQUIRE(pad->mapCentreForTest() == std::pair<double, double> { 0.0, 0.0 });

		// A CELL OFF THE ORIGIN, so the travel term is not zero and the basis is actually exercised.
		constexpr size_t kX = 31;
		constexpr size_t kY = 7;
		const double ux = (static_cast<double>(kX) + 0.5) / static_cast<double>(ui::AutomodPad::kMapWide);
		const double uy = (static_cast<double>(kY) + 0.5) / static_cast<double>(ui::AutomodPad::kMapHigh);
		const ps::Phases at = ps::phasesAt(home, basis, (ux * 2.0 - 1.0) * carry, (uy * 2.0 - 1.0) * carry);
		amd::AutomodCoords coords {};
		coords.lfoHz = 1.0f;
		const float expected = ui::automod_terrain::activityOf(amd::deriveAutomodAt(coords, at));
		const float cell = snapshot()[kY * ui::AutomodPad::kMapWide + kX];
		CAPTURE(expected);
		CAPTURE(cell);
		CHECK(cell == doctest::Approx(expected).epsilon(1e-4));
		// AND THE COORDINATE SQUARE'S ANSWER IS NOT THIS ONE, or the equality above would not tell the
		// plane from the square. Checked so the assertion is known to discriminate.
		const float square = ui::automod_terrain::activityAt(static_cast<float>(ux), static_cast<float>(uy));
		CAPTURE(square);
		CHECK(std::fabs(square - cell) > 1e-4f);
	}

	SUBCASE("ROTATE moves the picture and NOT the sound") {
		// **THE ONE PLACE ROTATE IS ALLOWED TO SHOW.** It turns the basis, so the plane's contents are
		// re-aimed and the terrain must redraw — while the sound at rest cannot move.
		const FxChain before = f.f.bridge.fxChain();
		f.f.bridge.setDeviceControl(f.device, static_cast<size_t>(amd::Param::Rotate), 0.4f);
		REQUIRE(rowField(static_cast<size_t>(amd::Param::Rotate)) == doctest::Approx(0.4f));
		pad->redraw();
		visage::Canvas canvas;
		pad->draw(canvas);
		CHECK(pad->rebuildsForTest() > settled);
		CHECK(pad->builtForTest().rotate == doctest::Approx(0.4));
		CAPTURE(differs(atRest, snapshot()));
		CHECK(differs(atRest, snapshot()) > 0);

		const FxChain after = f.f.bridge.fxChain();
		for (const amd::Param p : { amd::Param::Type, amd::Param::Flavor, amd::Param::Mod }) {
			CHECK(after.device[f.slot].row.field[static_cast<size_t>(p)]
			      == before.device[f.slot].row.field[static_cast<size_t>(p)]);
		}
		for (size_t i = 0; i < amd::kFoldedTravelFloats; ++i) {
			CAPTURE(i);
			CHECK(after.device[f.slot].row.field[amd::kFoldedTravelFirst + i]
			      == before.device[f.slot].row.field[amd::kFoldedTravelFirst + i]);
		}
	}

	SUBCASE("a coordinate knob moves the picture, because the home moved") {
		// A KNOB EDIT IS A MOVE OF THE PLANE'S ORIGIN WITH NO GESTURE ON THE PAD, which is the case
		// the staleness key exists for: the next draw notices and rebuilds.
		f.f.bridge.setDeviceControl(f.device, static_cast<size_t>(amd::Param::Type), 0.6f);
		REQUIRE(rowField(static_cast<size_t>(amd::Param::Type)) == doctest::Approx(0.6f));
		visage::Canvas canvas;
		pad->draw(canvas);
		CHECK(pad->rebuildsForTest() > settled);
		CHECK(pad->builtForTest().type == doctest::Approx(0.6));
		CAPTURE(differs(atRest, snapshot()));
		CHECK(differs(atRest, snapshot()) > 0);
	}

	SUBCASE("zoom moves the picture, because the span it covers changed") {
		pad->setZoom(8.0);
		CHECK(pad->builtForTest().zoom == doctest::Approx(8.0));
		CAPTURE(differs(atRest, snapshot()));
		CHECK(differs(atRest, snapshot()) > 0);
	}
}
TEST_CASE("the Automod puck follows the hand during a drag, and rests at centre") {
	// The shaper's pad had this and its case records the cause: `commitAndReanchor` commits the
	// travel and then ZEROES it, on every drag event, and `puck()` read exactly that travel — so the
	// puck sat at centre for the whole gesture while the field moved under it. Automod's pad was
	// written from the same draft and read the same quantity.
	//
	// **THE VALUE, NOT JUST A DIFFERENCE.** The deflection is the pointer's offset from where the
	// gesture began, so it is computable independently; a puck tracking the per-event delta would
	// pass a difference check and sit at a tenth of the pad here instead of a fifth.
	AutomodPanelFixture f;
	REQUIRE(f.panel != nullptr);
	auto* pad = const_cast<ui::AutomodPad*>(f.panel->padForTest());
	REQUIRE(pad != nullptr);
	REQUIRE(pad->width() > 1.0f);

	const visage::Point centre { pad->width() * 0.5f, pad->height() * 0.5f };
	CHECK(pad->puck().x == doctest::Approx(centre.x));
	CHECK(pad->puck().y == doctest::Approx(centre.y));

	pad->onMouseDown().callback(at(centre.x, centre.y));
	CHECK(pad->puck().x == doctest::Approx(centre.x));

	const float step = pad->width() * 0.1f;
	pad->onMouseDrag().callback(at(centre.x + step, centre.y));
	const visage::Point one = pad->puck();
	pad->onMouseDrag().callback(at(centre.x + step * 2.0f, centre.y));
	const visage::Point two = pad->puck();

	CHECK(one.x == doctest::Approx(centre.x + step));
	CHECK(two.x == doctest::Approx(centre.x + step * 2.0f));
	CHECK(two.x > one.x);
	CHECK(two.y == doctest::Approx(centre.y));

	pad->onMouseUp().callback(at(centre.x + step * 2.0f, centre.y));
	pad->finishGlideForTest(); // backlog IJ: the view glides home, then the puck is at centre
	CHECK(pad->puck().x == doctest::Approx(centre.x));
	CHECK(pad->puck().y == doctest::Approx(centre.y));
}
TEST_CASE("the Automod terrain does not rebuild inside a gesture") {
	// The shaper's rule, and the shaper's trap: a drag commits travel on every mouse event, which
	// moves the view, which would fire the staleness check's rebuild once per frame for the whole
	// drag. `draw()` is gated on the gesture so the picture catches up on release instead.
	//
	// **COUNTED, NOT COMPARED.** A rebuild landing on similar cells is invisible to a picture
	// comparison while costing the whole build.
	AutomodPanelFixture f;
	REQUIRE(f.panel != nullptr);
	auto* pad = const_cast<ui::AutomodPad*>(f.panel->padForTest());
	REQUIRE(pad != nullptr);
	REQUIRE(pad->width() > 1.0f);

	visage::Canvas canvas;
	const visage::Point centre { pad->width() * 0.5f, pad->height() * 0.5f };

	pad->draw(canvas);
	const size_t settled = pad->rebuildsForTest();

	pad->onMouseDown().callback(at(centre.x, centre.y));
	for (int i = 1; i <= 6; ++i) {
		pad->onMouseDrag().callback(at(centre.x + pad->width() * 0.05f * static_cast<float>(i), centre.y));
		pad->draw(canvas);
	}
	CHECK(pad->rebuildsForTest() == settled);

	pad->onMouseUp().callback(at(centre.x + pad->width() * 0.3f, centre.y));
	pad->finishGlideForTest(); // backlog IJ: the picture catches up when the glide LANDS
	CHECK(pad->rebuildsForTest() > settled);

	const size_t afterRelease = pad->rebuildsForTest();
	pad->draw(canvas);
	CHECK(pad->rebuildsForTest() == afterRelease);
}
TEST_CASE("a click away from the centre moves the Automod field, and the puck with it") {
	// Clicking somewhere moves the puck too. Automod's pad anchored
	// the gesture where the press landed, so the distance from the centre to the click was nobody's
	// delta and a click committed nothing. The press now anchors at the CENTRE, as the shaper's does.
	namespace amd = feathers::fx::automod;
	AutomodPanelFixture f;
	REQUIRE(f.panel != nullptr);
	auto* pad = const_cast<ui::AutomodPad*>(f.panel->padForTest());
	REQUIRE(pad != nullptr);
	REQUIRE(pad->width() > 1.0f);

	const auto committed = [&] {
		double magnitude = 0.0;
		for (size_t i = 0; i < amd::kFoldedTravelFloats; ++i) {
			magnitude += std::fabs(f.f.bridge.fxChain().device[f.slot].row.field[amd::kFoldedTravelFirst + i]);
		}
		return magnitude;
	};
	REQUIRE(committed() == doctest::Approx(0.0));

	const visage::Point centre { pad->width() * 0.5f, pad->height() * 0.5f };
	const float off = pad->width() * 0.25f;

	pad->onMouseDown().callback(at(centre.x + off, centre.y));
	CHECK(pad->puck().x == doctest::Approx(centre.x + off));
	CHECK(pad->puck().y == doctest::Approx(centre.y));
	pad->onMouseUp().callback(at(centre.x + off, centre.y));
	pad->finishGlideForTest(); // backlog IJ: the view glides home, then the puck is at centre

	CAPTURE(committed());
	CHECK(committed() > 1e-6);
	CHECK(pad->puck().x == doctest::Approx(centre.x));

	// **A CLICK EXACTLY ON THE CENTRE COMMITS NOTHING** — the displacement is nil. Without this the
	// case above would pass on a pad that commits a fixed lurch on every press.
	AutomodPanelFixture g;
	REQUIRE(g.panel != nullptr);
	auto* pad2 = const_cast<ui::AutomodPad*>(g.panel->padForTest());
	REQUIRE(pad2 != nullptr);
	const visage::Point c2 { pad2->width() * 0.5f, pad2->height() * 0.5f };
	pad2->onMouseDown().callback(at(c2.x, c2.y));
	pad2->onMouseUp().callback(at(c2.x, c2.y));
	double still = 0.0;
	for (size_t i = 0; i < amd::kFoldedTravelFloats; ++i) {
		still += std::fabs(g.f.bridge.fxChain().device[g.slot].row.field[amd::kFoldedTravelFirst + i]);
	}
	CHECK(still == doctest::Approx(0.0));
}
TEST_CASE("Automod zoom scales what a drag commits") {
	// The shaper's case, and its warning: `wrapForStorage` reduces the blob modulo each triangle's
	// period, so each drag happens from a FRESH device and stays small enough that nothing wraps.
	namespace amd = feathers::fx::automod;
	const auto committedAt = [](double zoom) {
		AutomodPanelFixture f;
		REQUIRE(f.panel != nullptr);
		auto* pad = const_cast<ui::AutomodPad*>(f.panel->padForTest());
		REQUIRE(pad != nullptr);
		REQUIRE(pad->width() > 1.0f);
		pad->setZoom(zoom);
		const visage::Point centre { pad->width() * 0.5f, pad->height() * 0.5f };
		pad->onMouseDown().callback(at(centre.x, centre.y));
		pad->onMouseDrag().callback(at(centre.x + pad->width() * 0.1f, centre.y));
		pad->onMouseUp().callback(at(centre.x, centre.y));
		double magnitude = 0.0;
		for (size_t i = 0; i < amd::kFoldedTravelFloats; ++i) {
			magnitude += std::fabs(f.f.bridge.fxChain().device[f.slot].row.field[amd::kFoldedTravelFirst + i]);
		}
		return magnitude;
	};

	const double atOne = committedAt(1.0);
	const double atFour = committedAt(4.0);
	CAPTURE(atOne);
	CAPTURE(atFour);
	CHECK(atOne > 1e-6);
	CHECK(atFour < atOne * 0.5);
	CHECK(atFour > atOne * 0.1);

	SUBCASE("the wheel is a notch on Explore's ladder") {
		AutomodPanelFixture f;
		REQUIRE(f.panel != nullptr);
		auto* pad = const_cast<ui::AutomodPad*>(f.panel->padForTest());
		REQUIRE(pad != nullptr);
		visage::MouseEvent wheel = at(pad->width() * 0.5f, pad->height() * 0.5f);
		wheel.wheel_delta_y = 1.0f;
		CHECK(pad->mouseWheel(wheel));
		CHECK(pad->zoom() == doctest::Approx(ui::AutomodPad::kZoomStep));
		wheel.wheel_delta_y = -1.0f;
		CHECK(pad->mouseWheel(wheel));
		CHECK(pad->mouseWheel(wheel));
		CHECK(pad->zoom() == doctest::Approx(1.0 / ui::AutomodPad::kZoomStep));
	}
}
TEST_CASE("IJ: the Automod pad pans as the Shaper's does — the press lands the pictured point, a hand at the edge pans on time, the release glides home") {
	// The Shaper pad's treatment, given to the Automod pad.
	// The joystick committed a delta per event and re-derived the plane from wherever the field had got
	// to, so the plane turned under the hand and the picture froze until release.
	namespace amd = feathers::fx::automod;
	namespace ps = amd::phase_space;
	AutomodPanelFixture f;
	REQUIRE(f.panel != nullptr);
	auto* pad = const_cast<ui::AutomodPad*>(f.panel->padForTest());
	REQUIRE(pad != nullptr);
	f.f.bridge.setDeviceControl(f.device, static_cast<size_t>(amd::Param::Type), 0.3f);
	f.f.bridge.setDeviceControl(f.device, static_cast<size_t>(amd::Param::Rotate), 0.7f);
	visage::Canvas canvas;
	pad->draw(canvas); // the pad takes the knobs: a view that moved re-anchors
	const float w = pad->width();
	const float h = pad->height();
	REQUIRE(w > 1.0f);
	const visage::Point centre { w * 0.5f, h * 0.5f };
	// PLANE UNITS PER PIXEL AT x1, from the pad's own contract: a half-drag carries one zone of eight.
	const double k = static_cast<double>(ui::AutomodPad::kZonesPerHalfDrag)
	                 / static_cast<double>(ui::automod_terrain::kZonesShown) / (std::min(w, h) * 0.5);
	CHECK(pad->planePerPixelForTest() == doctest::Approx(k));

	const auto field = [&](size_t index) {
		return f.f.bridge.fxChain().device[f.slot].row.field[index];
	};
	const auto rowStored = [&] {
		std::array<float, amd::kFoldedTravelFloats> out {};
		for (size_t i = 0; i < out.size(); ++i) {
			out[i] = field(amd::kFoldedTravelFirst + i);
		}
		return out;
	};
	// THE RESTATEMENT — the standing travel plus the basis the frame is anchored on, carried to a plane
	// point, wrapped for storage exactly as the device stores it.
	std::array<double, amd::kFoldedTravelFloats> origin {};
	for (size_t i = 0; i < origin.size(); ++i) {
		origin[i] = static_cast<double>(field(amd::kFoldedTravelFirst + i));
	}
	const auto knob = [&](amd::Param p) { return static_cast<double>(field(static_cast<size_t>(p))); };
	ps::Phases home = ps::homeFor(knob(amd::Param::Type), knob(amd::Param::Flavor), knob(amd::Param::Mod));
	ps::pushAlong(home, ps::kGammaAxis, knob(amd::Param::Gamma));
	for (size_t i = 0; i < ps::kTriangles; ++i) {
		home[i] += origin[i];
	}
	const ps::Basis basis = ps::basisFor(knob(amd::Param::Rotate), ps::planeHomeOf(home));
	const auto expectAt = [&](double px, double py) {
		std::array<double, amd::kFoldedTravelFloats> travel {};
		for (size_t i = 0; i < travel.size(); ++i) {
			travel[i] = origin[i] + basis.travelTerm(i, px, py);
		}
		std::array<float, amd::kFoldedTravelFloats> stored {};
		amd::wrapForStorage(travel, stored);
		return stored;
	};
	const auto worst = [](const auto& a, const auto& b) {
		double d = 0.0;
		for (size_t i = 0; i < a.size(); ++i) {
			d = std::max(d, std::fabs(static_cast<double>(a[i]) - static_cast<double>(b[i])));
		}
		return d;
	};
	using Pad = ui::AutomodPad;
	const double viewWide = static_cast<double>(w) * k;
	const double inset = Pad::kStarInsetPixels;

	SUBCASE("the press commits the travel of the point pressed, in the frame the picture is drawn in") {
		pad->onMouseDown().callback(at(centre.x + 90.0f, centre.y - 60.0f));
		CHECK(worst(rowStored(), expectAt(90.0 * k, 60.0 * k)) < 1e-6);
		CHECK(pad->puck().x == doctest::Approx(centre.x + 90.0f));
		CHECK(pad->puck().y == doctest::Approx(centre.y - 60.0f));
		pad->onMouseUp().callback(at(centre.x + 90.0f, centre.y - 60.0f));
		pad->finishGlideForTest();
	}
	SUBCASE("a drag holds the frame: the second point is the frame's, not re-derived from the first") {
		// THE JOYSTICK'S DEFECT, BY VALUE: it re-derived the basis from where the first event had put the
		// field, so a two-event drag landed somewhere a one-event press to the same point did not.
		pad->onMouseDown().callback(at(centre.x, centre.y));
		pad->onMouseDrag().callback(at(centre.x - 70.0f, centre.y + 40.0f));
		pad->onMouseDrag().callback(at(centre.x + 110.0f, centre.y + 80.0f));
		CHECK(worst(rowStored(), expectAt(110.0 * k, -80.0 * k)) < 1e-6);
		CHECK(pad->viewForTest().first == 0.0); // well inside: the view did not move
		CHECK(pad->rebuildsForTest() > 0);
		pad->onMouseUp().callback(at(centre.x + 110.0f, centre.y + 80.0f));
	}
	SUBCASE("a hand held still past the edge keeps panning, frame after frame, with no mouse event") {
		pad->onMouseDown().callback(at(centre.x, centre.y));
		pad->onMouseDrag().callback(at(w + 400.0f, centre.y));
		CHECK(pad->viewForTest().first == 0.0);             // an EVENT pans nothing: time does
		CHECK(pad->puck().x == doctest::Approx(w - inset)); // the puck stays whole on the edge crossed
		double lastView = 0.0;
		for (int i = 0; i < 10; ++i) {
			CAPTURE(i);
			CHECK(pad->tickForTest(0.05));
			const double v = pad->viewForTest().first;
			CHECK(v - lastView == doctest::Approx(Pad::kPanViewsPerSecond * 0.05 * viewWide));
			lastView = v;
		}
		CHECK(pad->viewForTest().second == 0.0);
		const double star = lastView + (w - inset - w * 0.5) * k;
		CHECK(pad->starForTest().first == doctest::Approx(star));
		CHECK(worst(rowStored(), expectAt(star, 0.0)) < 1e-5);
		pad->onMouseUp().callback(at(w + 400.0f, centre.y));
		CHECK_FALSE(pad->tickForTest(0.05));
	}
	SUBCASE("the band is at the real edge: 30 px inside is still, 12 px inside scrolls at the ramp") {
		pad->onMouseDown().callback(at(centre.x, centre.y));
		pad->onMouseDrag().callback(at(w - 30.0f, centre.y));
		CHECK_FALSE(pad->tickForTest(0.05));
		pad->onMouseDrag().callback(at(w - 12.0f, centre.y));
		CHECK(pad->tickForTest(0.05));
		const double ramp = (Pad::kEdgeBandPixels - 12.0) / Pad::kRampPixels;
		CHECK(pad->viewForTest().first == doctest::Approx(Pad::kPanViewsPerSecond * ramp * 0.05 * viewWide));
		pad->onMouseUp().callback(at(w - 12.0f, centre.y));
	}
	SUBCASE("above the top edge pans UP — up is more — and the left edge pans left") {
		pad->onMouseDown().callback(at(centre.x, centre.y));
		pad->onMouseDrag().callback(at(-200.0f, -200.0f));
		CHECK(pad->tickForTest(0.05));
		CHECK(pad->viewForTest().first < 0.0);
		CHECK(pad->viewForTest().second > 0.0);
		pad->onMouseUp().callback(at(-200.0f, -200.0f));
	}
	SUBCASE("a still PRESS in the band is a click, not a pan") {
		pad->onMouseDown().callback(at(w - 5.0f, centre.y));
		CHECK_FALSE(pad->tickForTest(0.05));
		pad->onMouseUp().callback(at(w - 5.0f, centre.y));
	}
	SUBCASE("the FRAME is what drives it: a draw while a hand dwells past the edge moves the view") {
		pad->onMouseDown().callback(at(centre.x, centre.y));
		pad->onMouseDrag().callback(at(w + 400.0f, centre.y));
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		pad->draw(canvas);
		CHECK(pad->viewForTest().first > 0.0);
		pad->onMouseUp().callback(at(w + 400.0f, centre.y));
	}
	SUBCASE("a long dwell keeps the map covering the view") {
		pad->onMouseDown().callback(at(centre.x, centre.y));
		pad->onMouseDrag().callback(at(w + 400.0f, centre.y));
		const size_t before = pad->rebuildsForTest();
		for (int i = 0; i < 40; ++i) {
			pad->tickForTest(0.05);
		}
		CHECK(pad->rebuildsForTest() > before);
		pad->onMouseUp().callback(at(w + 400.0f, centre.y));
	}
	SUBCASE("the release glides the view home, then the frame re-origins on the sound") {
		pad->onMouseDown().callback(at(centre.x + 100.0f, centre.y));
		pad->onMouseUp().callback(at(centre.x + 100.0f, centre.y));
		const auto sound = rowStored();
		if (!ui::reducedMotionRequested()) {
			CHECK(pad->glidingForTest());
			CHECK(pad->puck().x == doctest::Approx(centre.x + 100.0f)); // the view has not moved yet
		}
		pad->finishGlideForTest();
		CHECK_FALSE(pad->glidingForTest());
		CHECK(pad->puck().x == doctest::Approx(centre.x));
		CHECK(pad->starForTest().first == 0.0);
		// AND THE SOUND IS WHERE THE PRESS PUT IT — landing moved the view, never the travel.
		CHECK(worst(rowStored(), sound) == 0.0);
		CHECK(worst(rowStored(), expectAt(100.0 * k, 0.0)) < 1e-6);
	}
}
TEST_CASE("IS.5: Automod's Keytrack is a chip on the panel's last row, and a click turns BOTH filters' tracking off") {
	namespace amd = feathers::fx::automod;
	AutomodPanelFixture f;
	REQUIRE(f.panel != nullptr);
	const ui::SwitchChip* chip = f.panel->keytrackForTest();
	REQUIRE(chip != nullptr);
	CHECK(chip->width() > 0.0f);
	CHECK(chip->y() >= f.panel->padBounds().bottom());
	const auto keytrack = [&] {
		return fxDeviceAtPosition(f.f.bridge.fxChain(), f.slot).row.field[static_cast<size_t>(amd::Param::Keytrack)];
	};
	CHECK(chip->on());
	CHECK(keytrack() == 1.0f);
	const_cast<ui::SwitchChip*>(chip)->onMouseDown().callback(at(chip->width() * 0.5f, chip->height() * 0.5f));
	const_cast<ui::SwitchChip*>(chip)->onMouseUp().callback(at(chip->width() * 0.5f, chip->height() * 0.5f));
	CHECK(keytrack() == 0.0f);
	CHECK_FALSE(chip->on());
}
TEST_CASE("IS.5: the Automod terrain counts NEGATIVE comb feedback as activity — a hollow comb rings as hard as a bright one") {
	feathers::fx::automod::AutomodDerived up {};
	up.comb.feedback = 0.5f;
	feathers::fx::automod::AutomodDerived down = up;
	down.comb.feedback = -0.5f;
	CHECK(ui::automod_terrain::activityOf(down) == ui::automod_terrain::activityOf(up));
	feathers::fx::automod::AutomodDerived none = up;
	none.comb.feedback = 0.0f;
	CHECK(ui::automod_terrain::activityOf(down) > ui::automod_terrain::activityOf(none));
}
TEST_CASE("IJ: Automod's Rotate sits INSIDE the terrain box, drawn over the pad") {
	AutomodPanelFixture f;
	REQUIRE(f.panel != nullptr);
	const auto* rotate = f.panel->rotateForTest();
	REQUIRE(rotate != nullptr);
	const visage::Bounds pad = f.panel->padBounds();
	CHECK(rotate->width() > 0.0f);
	CHECK(rotate->height() > 0.0f);
	CHECK(rotate->x() >= pad.x());
	CHECK(rotate->y() >= pad.y());
	CHECK(rotate->bounds().right() <= pad.right());
	CHECK(rotate->bounds().bottom() <= pad.bottom());
	const auto& kids = f.panel->children();
	const auto at = [&](const visage::Frame* frame) {
		return std::find(kids.begin(), kids.end(), frame) - kids.begin();
	};
	CHECK(at(rotate) > at(f.panel->padForTest()));
	// AND NOT A CELL OF THE KNOB ROWS ANY MORE.
	const auto& cells = f.panel->cellsForTest();
	CHECK(std::find(cells.begin(), cells.end(), static_cast<const visage::Frame*>(rotate)) == cells.end());
}
TEST_CASE("automod: gamma is a GammaBox on BOTH surfaces, and a drag moves the same sixteen laps") {
	namespace amd = feathers::fx::automod;

	// **THE VALUE, NOT THE CONFIGURATION.** Asserting `dragSpanForTest() == 16` on both surfaces would
	// prove they were configured alike and NOT that a hand gets the same result — the span is an input
	// to `ValueBox::dragTo`, and a surface could still sit its box at a different size or seat a
	// different control. So this drags each box through the mouse road and reads the PARAMETER.
	//
	// This is the case the window's defect would have failed: gamma there was a `Knob` across
	// 0..16,777,215, so one full drag moved 16,777,215 laps where the card moved 16.
	AutomodStripFixture card;
	AutomodPanelFixture panel;

	const ui::GammaBox* cardBox = card.card->gammaForTest();
	const ui::GammaBox* panelBox = panel.panel->gammaForTest();
	REQUIRE(cardBox != nullptr);
	REQUIRE(panelBox != nullptr);

	const size_t gammaIndex = static_cast<size_t>(amd::Param::Gamma);
	const auto gammaOn = [&](ParamBridge& bridge, size_t slot) {
		return bridge.fxChain().device[slot].row.field[gammaIndex];
	};

	// A FULL DRAG IS `kDragRange` PIXELS UPWARD from the press, which is the widget's own definition
	// of "a full drag" — taken from the constant rather than from a number that happens to match it.
	const auto fullDragOn = [&](const ui::ValueBox* box) {
		auto* m = const_cast<ui::ValueBox*>(box);
		const float x = box->width() * 0.5f;
		const float y = box->height() * 0.5f;
		m->onMouseDown().callback(at(x, y));
		m->onMouseDrag().callback(at(x, y - ui::ValueBox::kDragRange));
		m->onMouseUp().callback(at(x, y - ui::ValueBox::kDragRange));
	};

	const float cardBefore = gammaOn(card.bridge, card.slot);
	fullDragOn(cardBox);
	const float cardMoved = gammaOn(card.bridge, card.slot) - cardBefore;

	const float panelBefore = gammaOn(panel.f.bridge, panel.slot);
	fullDragOn(panelBox);
	const float panelMoved = gammaOn(panel.f.bridge, panel.slot) - panelBefore;

	CHECK(cardMoved == doctest::Approx(ui::GammaBox::kLapsPerDrag).epsilon(0.05));
	CHECK(panelMoved == doctest::Approx(ui::GammaBox::kLapsPerDrag).epsilon(0.05));
	// AND THE SECOND ASSERTION THAT PROVES THE FIRST IS NOT VACUOUS: a drag that moved NOTHING on both
	// would satisfy neither Approx, but one that moved nothing on one and 16 on the other would still
	// read oddly without this — the two surfaces agree, which is the property it was asked for.
	CHECK(cardMoved == doctest::Approx(panelMoved).epsilon(0.01));
	CHECK(std::abs(cardMoved) > 1.0f);
}
// "STRUCTURAL: an expanded window is a SUPERSET of its card's controls" WAS HERE. It is REPLACED
// by the tooltips suite's "a device's window offers every control its card offers": this one put every device in ONE
// fx lane, capped at kMaxFxSlots = 8, and `continue`d past any it could not place, so it checked about eight devices
// (the Phaser/Flanger was not among them, which is how its window shipped short).

TEST_CASE("a mod card's drag reaches the panel's reorder, which it never did") {
	// Collapse worked for mod cards; drag did not.
	//
	// **THE PANEL HAD A COMPLETE REORDER ROAD AND NO WAY TO REACH IT.** `dragSlot_`, `dropGapAt`,
	// `rowForGap`, `moveDevice` — all present, all armed from `ModPanel`'s OWN `onMouseDown`. But a
	// press on an open card goes to the CARD: `frameAtPoint` answers the card frame, so the panel's
	// handler never ran and `dragSlot_` was never set. The road was reachable only by pressing the
	// strip's bare background, which is not where anyone grabs a card.
	//
	// The fx lane and the rack both bind `onCardDrag`. The mod panel was the third host and the one
	// that did not — so this asserts the WIRE, which is the thing that was missing, rather than the
	// reorder arithmetic that was already shared and already tested.
	ModFixture f;
	const size_t second = f.bridge.addRackDevice(mod::mseg::kTraits.tag);
	REQUIRE(second < kMaxRackSlots);
	f.panel->resized();
	REQUIRE(f.panel->deviceCount() >= 2);

	// THROUGH `holds`, which is the seam the other rack cases use — a tag comparison rather than
	// reaching into the struct's storage.
	const auto holdsAt = [&](size_t slot, const char* tag) {
		return f.bridge.modRack().holds(slot, tag);
	};
	REQUIRE(holdsAt(0, mod::dahdsr::kTraits.tag));
	REQUIRE(holdsAt(1, mod::mseg::kTraits.tag));

	ui::FxCard* card = const_cast<ui::FxCard*>(
	    dynamic_cast<const ui::FxCard*>(f.panel->cardFrameForTest(1)));
	REQUIRE(card != nullptr);
	// THE WIRE ITSELF. Without it the card's drag fires into an empty `std::function` and every
	// assertion below would be about a gesture nobody received.
	REQUIRE(static_cast<bool>(card->onCardDrag));

	// **THE INTENDED GESTURE: double-click the header and HOLD, then move.** That is the designed
	// drag mechanism, and that is what `FxCard::noteCardPress` arms (`armed_ = repeatClickCount >= 2`) and what
	// `onMouseDrag` requires before it reports anything.
	//
	// **A FIRST DRAFT OF THIS CASE CALLED `onCardDrag` DIRECTLY.** That asserts the WIRE and skips the
	// gesture — the same "half a gesture" trap that let the rack toggle twice under a green suite. The
	// wire is still required above; this drives the road a hand takes.
	const float lift = static_cast<float>(f.panel->sourceHeight(
	                       backplane::Source::output(f.bridge.rackDeviceAt(0), 0)));
	const visage::Point start { 20.0f, ui::ModPanel::kHeadHeight * 0.5f };
	card->onMouseDown().callback(at(start.x, start.y));
	card->onMouseDown().callback(doubleAt(start.x, start.y));
	// PAST THE THRESHOLD, or the card correctly reports nothing and this asserts a drag that never
	// began. `kDragThreshold` is the card's, so overshooting a whole row is unambiguous.
	card->onMouseDrag().callback(doubleAt(start.x, start.y - lift));
	card->onMouseUp().callback(doubleAt(start.x, start.y - lift));

	// THEY SWAPPED — the MSEG that was second is first now.
	CHECK(holdsAt(0, mod::mseg::kTraits.tag));
	CHECK(holdsAt(1, mod::dahdsr::kTraits.tag));
}
TEST_CASE("automod's terrain decodes the field, and the map it makes has STRUCTURE") {
	namespace ter = ui::automod_terrain;

	// **A PICTURE IS ONLY WORTH DRAWING IF IT VARIES.** The map exists so a player can see where the
	// field is quiet and where it is busy before landing there — the shaper's pad makes the same
	// argument for itself. A map that came back flat, or saturated, would draw perfectly and tell a
	// reader nothing, and no rendering check would catch either.
	//
	// So: assert the DISTRIBUTION, which is the property that makes it useful, rather than the value
	// at any one point.
	constexpr size_t kGrid = 64;
	std::vector<float> map;
	map.reserve(kGrid * kGrid);
	for (size_t y = 0; y < kGrid; ++y) {
		for (size_t x = 0; x < kGrid; ++x) {
			// OVER THE COORDINATE'S OWN RANGE, 0..1 — see `automod_terrain.h` on why an earlier
			// version multiplied by the zone count and sampled country no control can reach.
			const float t = (static_cast<float>(x) + 0.5f) / kGrid;
			const float f = (static_cast<float>(y) + 0.5f) / kGrid;
			map.push_back(ter::activityAt(t, f));
		}
	}

	SUBCASE("every cell is a drawable brightness") {
		for (const float a : map) {
			REQUIRE(std::isfinite(a));
			REQUIRE(a >= 0.0f);
			REQUIRE(a <= 1.0f);
		}
	}

	SUBCASE("the map is neither flat nor saturated") {
		const auto [lo, hi] = std::minmax_element(map.begin(), map.end());
		CAPTURE(*lo);
		CAPTURE(*hi);
		// SPANS MOST OF THE RANGE. A normaliser that was too large would compress everything into the
		// floor — measured, normalising by the field's MAXIMUM leaves 0.18% of the plane above 0.8.
		CHECK(*lo < 0.25f);
		CHECK(*hi > 0.75f);

		size_t dark = 0;
		size_t bright = 0;
		for (const float a : map) {
			if (a < 0.2f) {
				++dark;
			}
			if (a > 0.8f) {
				++bright;
			}
		}
		CAPTURE(dark);
		CAPTURE(bright);
		// **BOTH ENDS POPULATED, WHICH IS THE WHOLE POINT.** Measured at this normaliser: about 9% of
		// the plane dark and 10% bright. Bounds set wide enough that a field tweak does not fail this
		// case, tight enough that a map which lost one end does.
		CHECK(dark * 100 > map.size() * 2);
		CHECK(bright * 100 > map.size() * 2);
		CHECK(dark * 100 < map.size() * 40);
		CHECK(bright * 100 < map.size() * 40);
	}

	SUBCASE("it varies along BOTH axes, not just Type") {
		// The trap this catches: summing only the comb and filter-mix terms makes a map that is a
		// function of Type alone, so every column is identical and the Flavour axis is decoration.
		// Compare rows against rows and columns against columns.
		const auto differs = [&](size_t aStart, size_t bStart, size_t stride) {
			double sum = 0.0;
			for (size_t i = 0; i < kGrid; ++i) {
				sum += std::fabs(map[aStart + i * stride] - map[bStart + i * stride]);
			}
			return sum / static_cast<double>(kGrid);
		};
		const double acrossFlavour = differs(0, kGrid * (kGrid / 2), 1);          // two rows
		const double acrossType = differs(0, kGrid / 2, kGrid);                   // two columns
		CAPTURE(acrossFlavour);
		CAPTURE(acrossType);
		CHECK(acrossFlavour > 0.02);
		CHECK(acrossType > 0.02);
	}
}
TEST_CASE("the drag threshold is a distance, and a card's own displacement is subtracted on both axes") {
	// **BOTH HALVES OF THIS COMPARISON MEASURED Y ALONE, AND FIXING ONLY THE OBVIOUS HALF WOULD HAVE
	// BROKEN THE OTHER.**
	//
	// THE THRESHOLD: `std::abs(moved.y) < kDragThreshold` never passes for a horizontal drag, so in a
	// lane that flows in X — the rack's — reorder did not start at all. The user gets the bumper because a
	// hand on a trackpad drifts vertically by a pixel or two; a mouse on a rail would not, and would
	// have found the gesture simply dead.
	//
	// THE CORRECTION: `carried` subtracts however far the CARD moved under a live press, so a layout
	// change cannot masquerade as hand movement. It existed only for Y. Making the threshold a
	// magnitude and stopping there would have fixed the hand's axis and left the card's — a card
	// re-placed HORIZONTALLY under a held press would read as horizontal hand movement, and
	// fold-reorders-by-accident would come back sideways in the lane this change is for. The third
	// subcase is that regression, and it is the reason the fix is three parts rather than one.
	FilterFixture f;
	auto* lane = f.lane;
	REQUIRE(f.bridge.addFxDevice(fx::harm::kTraits.tag) == 1);
	lane->resized();
	ui::FxCard* card = lane->cardAt(1);
	REQUIRE(card != nullptr);
	const float head = ui::FxCard::kSlotHeadHeight * 0.5f;
	// ONE PAST THE THRESHOLD, not three: the claim is about where the line IS, and a generous nudge
	// would pass on a threshold set to anything smaller than itself.
	const float past = ui::FxCard::kDragThreshold + 1.0f;

	SUBCASE("a purely horizontal drag past the threshold arms") {
		card->onMouseDown().callback(doubleAt(20.0f, head));
		card->onMouseDrag().callback(at(20.0f + past, head));
		CHECK(card->cardDragging());
	}

	SUBCASE("a purely vertical drag past the threshold still arms") {
		// TODAY'S BEHAVIOUR AS A STRICT SUBSET. `hypot(0, dy)` is `|dy|`, so the modulator strip's
		// gesture — the one already tested by hand — must not move while the rack's is fixed.
		card->onMouseDown().callback(doubleAt(20.0f, head));
		card->onMouseDrag().callback(at(20.0f, head + past));
		CHECK(card->cardDragging());
	}

	SUBCASE("a movement shorter than the threshold on either axis is still a click") {
		// AND THE LINE HOLDS IN BOTH DIRECTIONS, or "it is a distance" would be satisfied by having no
		// threshold at all.
		card->onMouseDown().callback(doubleAt(20.0f, head));
		card->onMouseDrag().callback(at(20.0f + ui::FxCard::kDragThreshold * 0.5f, head));
		CHECK_FALSE(card->cardDragging());
		card->onMouseDrag().callback(at(20.0f, head + ui::FxCard::kDragThreshold * 0.5f));
		CHECK_FALSE(card->cardDragging());
	}

	SUBCASE("a card displaced horizontally under a held press does not arm") {
		const visage::Point before = card->positionInWindow();
		card->onMouseDown().callback(doubleAt(20.0f, head));

		// THE CARD MOVES, THE HAND DOES NOT. A lane re-placing its cards is exactly this, and the host
		// recomputes a drag's frame-local position against the frame's CURRENT place
		// (`window_event_handler.cpp:257`) — so `e.position` shifts by the card's displacement with no
		// hand movement at all. Shifted well past the threshold, because the real case is up to 226px.
		const float shift = ui::FxCard::kDragThreshold * 10.0f;
		card->setBounds(card->x() + static_cast<int>(shift), card->y(), card->width(),
		                card->height());
		const visage::Point after = card->positionInWindow();
		// THE DISPLACEMENT REALLY HAPPENED — without this the subcase passes on a card that never
		// moved, which is the one way it could be green while proving nothing.
		REQUIRE(after.x - before.x == doctest::Approx(shift));

		// THE HAND IS STILL AT THE SAME WINDOW POINT, so its frame-local x has dropped by the shift.
		card->onMouseDrag().callback(at(20.0f - shift, head));
		CHECK_FALSE(card->cardDragging());
	}
}
TEST_CASE("a Note-unit control reads as a note name, on the hardware's convention") {
	// **THE UNIT EXISTS BECAUSE NOBODY PICKS A ROOT NOTE BY ITS MIDI NUMBER.** The tracker card
	// selects a root note, and a knob reading "60" does not do that. `Unit`
	// gained `Note` rather than the tracker gaining a 128-entry `Choice` table — see device.h on why
	// that was the wrong shape twice over.
	//
	// **THE CONVENTION IS THE REFERENCE'S AND IT IS A REAL DISAGREEMENT, NOT AN ARBITRARY PICK.**
	// `noteCodeToString` computes `octave = noteCode / 12 - 2`, so 60 is C3 — the Yamaha reading,
	// against the C4 that Roland and scientific pitch notation use. Following the instrument is
	// what settles it; a case that asserted C4 would be equally defensible arithmetic and wrong here.
	device::ParamDesc note { .key = "root_note",
		                     .name = "Root Note",
		                     .min = 0.0,
		                     .max = 127.0,
		                     .def = 60.0,
		                     .stepped = true,
		                     .unit = device::Unit::Note,
		                     .names = {} };

	CHECK(ui::formatValue(note, 60.0) == "C3");
	CHECK(ui::formatValue(note, 61.0) == "C#3");

	// **THE OCTAVE BOUNDARY, EITHER SIDE — the arithmetic this could plausibly get wrong.** One point
	// pins nothing: `n/12 - 2` and a dozen other formulas agree at 60 and diverge at the edges, and
	// integer division toward zero is a different function from a floor once the number is negative.
	CHECK(ui::formatValue(note, 59.0) == "B2");
	CHECK(ui::formatValue(note, 72.0) == "C4");

	// AND THE ENDS OF THE DECLARED RANGE, where the negative octave lives. 0 is C-2 on this
	// convention, which is exactly where truncation-toward-zero would have printed C-1 instead.
	CHECK(ui::formatValue(note, 0.0) == "C-2");
	CHECK(ui::formatValue(note, 127.0) == "G8");

	// **A VALUE BELOW THE DECLARED RANGE, WHICH NO CALLER CAN PRODUCE — AND THE GUARD STAYS ANYWAY.**
	//
	// A known-bad removing the negative guard (`note % 12` instead of `((note % 12) + 12) % 12`) left
	// this case GREEN, because every value on every real road is 0..127 and the two agree there. That
	// is a guard nothing proves, and this repo's rule is to find out what an apparent redundancy is
	// buying before removing it. What this one buys is an ARRAY INDEX: C's `%` keeps the sign, so a
	// negative note would index `kLetters` backwards and read off the front of it. Deleting it would
	// trade an unprovable branch for a silent out-of-bounds read — the class the sanitizer gate exists
	// for and that `ctest` cannot see.
	//
	// SO IT IS PINNED HERE RATHER THAN DELETED, and the assertion says out loud that it is defence
	// against a programming error rather than a value a player can reach.
	CHECK(ui::formatValue(note, -1.0) == "B-3");

	SUBCASE("and the tracker's Root Note reads through the real road") {
		// NOT A UNIT TEST OF A FORMATTER. The claim is that the DEVICE's declared control reaches this
		// formatting — a descriptor built here proves the function works and nothing about whether the
		// tracker is wired to it.
		namespace trk = feathers::mod::tracker;
		const device::ParamDesc& root =
		    trk::kDescriptor.params[static_cast<size_t>(trk::Param::RootNote)];
		REQUIRE(root.unit == device::Unit::Note);
		CHECK(ui::formatValue(root, root.def) == "C3");
	}
}
//------------------------------------------------------------------------------------------------//
// THE SHAPER'S TWO PLOTS, AND THE FOUR PLACES A CHANGE COMES FROM.                                  //
//                                                                                                  //
// The table display did not always follow puck and knob changes, in the window and perhaps in the  //
// card when changed from the window. Two surfaces draw the same                                    //
// curve — the card and the panel — and a change reaches them from the card's knobs, the panel's    //
// knobs, the panel's pad, or the bridge (a patch, the host). That is a 4x2 table, and every cell   //
// is asserted here by a COUNT THE DELIVERY PRODUCES and a VALUE THE SURFACE RECORDS, never by a    //
// picture: `FrameWatchers::notify` returns how many frames it told, `update()` returns whether it  //
// asked for one, and `drawnSettingsForTest` is what it asked with.                                 //
//------------------------------------------------------------------------------------------------//

TEST_CASE("a Shaper change from any of four sources reaches BOTH plots — counted and read back") {
	namespace shp = feathers::fx::shaper;
	ShaperFixture f;
	REQUIRE(f.card != nullptr);

	// THE PANEL, OPENED THE WAY A HAND OPENS IT — in the editor's overlay, on the state channel in
	// its own right. The fixture's own detached `panel` is deliberately not used: it is not in the
	// tree, and a surface not in the tree is on no road at all.
	auto* overlay = f.f.gui.expandedForTest();
	REQUIRE(overlay != nullptr);
	REQUIRE(f.f.lane->onExpandDevice != nullptr);
	(void)f.f.gui.canvasForTest()->setCollapsed(true); // on the overlay, the host this case reads
	f.f.lane->onExpandDevice(f.slot);
	REQUIRE(overlay->open());
	auto* panel = presentedAs<ui::ShaperExpanded>(overlay);
	REQUIRE(panel != nullptr);
	REQUIRE(panel->width() > 0.0f);
	REQUIRE(panel->isVisible());
	REQUIRE(f.card->isVisible());

	// THE STATE CHANNEL HOLDS BOTH — the road every write travels, by subscription.
	const ui::FrameWatchers* watchers = f.f.gui.stateWatchers();
	REQUIRE(watchers != nullptr);
	CHECK(watchers->watches(f.card));
	CHECK(watchers->watches(panel));

	// QUIET FIRST. Other surfaces ask for a frame on their first tick; the counts below are about
	// what THIS change caused, so the tick is drained until nothing asks.
	for (int i = 0; i < 8 && f.f.gui.tickForTest().deviceFrames != 0; ++i) {
	}
	REQUIRE(f.f.gui.tickForTest().deviceFrames == 0);
	REQUIRE(f.f.gui.tickForTest().stateFrames == 0);

	const auto rowField = [&](shp::Param p) {
		const FxChain chain = f.f.bridge.fxChain();
		return fxDeviceAtPosition(chain, f.slot).row.field[static_cast<size_t>(p)];
	};
	const auto settingsNow = [&] {
		shp::Row row {};
		const FxChain chain = f.f.bridge.fxChain();
		for (size_t i = 0; i < row.size(); ++i) {
			row[i] = fxDeviceAtPosition(chain, f.slot).row.field[i];
		}
		shp::Settings s = shp::stateOf(row, fxDeviceAtPosition(chain, f.slot).row.fieldsPresent);
		// **AND THE FOLDED TRAVEL, because the surfaces read it and `stateOf` cannot.** Without this the
		// expectation carries zeros while the card carries the real thing, and the case fails on a
		// difference it created itself.
		for (size_t i = 0; i < shp::phase_space::kPhaseCount; ++i) {
			const size_t at = shp::kTravelFirst + i;
			s.travel[i] = at < fxDeviceAtPosition(chain, f.slot).row.fieldsPresent
			                  ? fxDeviceAtPosition(chain, f.slot).row.field[at]
			                  : 0.0f;
		}
		return s;
	};
	const auto knobFor = [](const auto& surface, shp::Param p) -> ui::Knob* {
		for (const auto& knob : surface.knobsForTest()) {
			if (surface.paramOfKnobForTest(knob.get()) == p) {
				return knob.get();
			}
		}
		return nullptr;
	};

	// ONE TICK AFTER A CHANGE, BOTH SURFACES MUST HAVE (1) BEEN TOLD BY THE STATE CHANNEL, (2) ASKED
	// FOR A FRAME OFF THEIR OWN COMPARISON, AND (3) RECORDED THE SETTINGS THE ENGINE NOW HOLDS — so
	// the curve `deriveCurve` makes of them is the curve the row makes.
	const auto bothPlotsFollow = [&](const char* source) {
		CAPTURE(source);
		const shp::Settings now = settingsNow();
		const Gui::TickReport report = f.f.gui.tickForTest();
		// (1) the channel fired, and both plots are on it — a count produced BY the notification.
		CHECK(report.stateFrames > 0);
		CHECK(watchers->watches(f.card));
		CHECK(watchers->watches(panel));
		// (2) the card and the panel each asked for a frame: two device surfaces, and it is these two
		// — read off the surfaces rather than inferred from the total.
		CHECK(report.deviceFrames >= 2);
		CHECK(f.card->drawnSettingsForTest() == now);
		CHECK(panel->drawnSettingsForTest() == now);
		// (3) the curve each will draw is the row's curve, through the device's own mapper.
		const shp::CurveParams truth = shp::deriveCurve(now.knee, now.colour, now.gamma, shp::kSlewHarmonics);
		for (const shp::Settings& drawn : { f.card->drawnSettingsForTest(), panel->drawnSettingsForTest() }) {
			const shp::CurveParams p = shp::deriveCurve(drawn.knee, drawn.colour, drawn.gamma, shp::kSlewHarmonics);
			CHECK(p.drive == truth.drive);
			CHECK(p.inflatorWeight == truth.inflatorWeight);
			CHECK(p.polyWeight == truth.polyWeight);
			CHECK(p.chebyWeight == truth.chebyWeight);
		}
		// AND THEN QUIET: the same settings a tick later ask for nothing, which is what makes the
		// count above a count of THIS change and not of a surface that asks every tick.
		CHECK(f.f.gui.tickForTest().deviceFrames == 0);
	};

	SUBCASE("from a knob on the CARD") {
		ui::Knob* knee = knobFor(*f.card, shp::Param::Knee);
		REQUIRE(knee != nullptr);
		const float before = rowField(shp::Param::Knee);
		knee->onMouseDown().callback(at(knee->width() * 0.5f, 60.0f));
		knee->onMouseDrag().callback(at(knee->width() * 0.5f, 20.0f));
		knee->onMouseUp().callback(at(knee->width() * 0.5f, 20.0f));
		REQUIRE(rowField(shp::Param::Knee) != before); // the gesture wrote, or the rest is vacuous
		bothPlotsFollow("card knob");
	}

	SUBCASE("from a knob on the PANEL") {
		ui::Knob* knee = knobFor(*panel, shp::Param::Knee);
		REQUIRE(knee != nullptr);
		const float before = rowField(shp::Param::Knee);
		knee->onMouseDown().callback(at(knee->width() * 0.5f, 60.0f));
		knee->onMouseDrag().callback(at(knee->width() * 0.5f, 20.0f));
		knee->onMouseUp().callback(at(knee->width() * 0.5f, 20.0f));
		REQUIRE(rowField(shp::Param::Knee) != before);
		bothPlotsFollow("panel knob");
	}

	SUBCASE("from the PAD on the panel — the puck") {
		auto* pad = const_cast<ui::ShaperPad*>(panel->padForTest());
		REQUIRE(pad != nullptr);
		REQUIRE(pad->width() > 1.0f);
		const float colourBefore = rowField(shp::Param::Colour);
		const float gammaBefore = rowField(shp::Param::Gamma);
		pad->onMouseDown().callback(at(pad->width() * 0.1f, pad->height() * 0.9f));
		pad->onMouseDrag().callback(at(pad->width() * 0.6f, pad->height() * 0.3f));
		pad->onMouseUp().callback(at(pad->width() * 0.6f, pad->height() * 0.3f));
		pad->finishGlideForTest(); // the release glide, landed as a frame loop would (backlog IJ)
		// **THE PAD MOVES THE TRAVEL NOW, NOT THE COORDINATES.** This required Colour and Gamma to have
		// changed, which was true of the pad that wrote them directly and is the behaviour that made it
		// a line drawn as a square. What must still hold is what this case is ABOUT: a hand on the pad
		// reaches both plots. The source is checked at its own seam — the folded travel — and the plots
		// are checked below exactly as before.
		CHECK(rowField(shp::Param::Colour) == colourBefore);
		CHECK(rowField(shp::Param::Gamma) == gammaBefore);
		int travelMoved = 0;
		for (size_t i = 0; i < shp::phase_space::kPhaseCount; ++i) {
			const FxChain chain = f.f.bridge.fxChain();
			if (fxDeviceAtPosition(chain, f.slot).row.field[shp::kTravelFirst + i] != 0.0f) {
				++travelMoved;
			}
		}
		CAPTURE(travelMoved);
		REQUIRE(travelMoved >= 2);
		bothPlotsFollow("panel pad");
	}

	SUBCASE("from the BRIDGE — a patch, the host, anything that is not a hand on a widget") {
		const float before = rowField(shp::Param::Colour);
		f.f.bridge.setDeviceControl(f.device, static_cast<size_t>(shp::Param::Colour),
		                               before < 0.5f ? 0.8f : 0.2f);
		f.f.bridge.setDeviceControl(f.device, static_cast<size_t>(shp::Param::Knee), 0.9f);
		REQUIRE(rowField(shp::Param::Colour) != before);
		bothPlotsFollow("bridge write");
	}

	overlay->dismiss();
}
TEST_CASE("the shaper card keeps every control inside itself") {
	namespace shp = feathers::fx::shaper;

	// **SEEN IN A SCREENSHOT: a mode button drawn past the card's right edge**, the fifth of five
	// cut in half at the boundary. MEASURED from the declarations: the row is
	// `kPad + 5 * (kExtraWidth + kExtraGap) - kExtraGap + kPad` = 10 + 250 - 4 + 10 = **266px** of
	// content, laid out unconditionally at a card width that is narrower in the rack.
	//
	// **NOTHING CHECKED CONTAINMENT, WHICH IS WHY IT SHIPPED.** The geometry cases this repo has ask
	// whether a row is laid out, whether cells overlap, whether the open height fits a bar — every one
	// of them a question about the row's INTERNAL consistency. A row can be internally perfect and
	// hang off the edge of its parent, and that is what happened.
	FilterFixture f;
	const size_t slot = f.bridge.addFxDevice(shp::kTraits.tag);
	REQUIRE(slot != static_cast<size_t>(-1));
	f.lane->resized();
	auto* card = dynamic_cast<ui::ShaperCard*>(f.lane->cardAt(slot));
	REQUIRE(card != nullptr);
	REQUIRE(card->width() > 1.0f);

	CAPTURE(card->width());

	// EVERY EXTRA SWITCH, by name, so a failure says WHICH one hangs off rather than that something
	// does.
	for (size_t i = 0; i < shp::kExtraSwitchCount; ++i) {
		CAPTURE(i);
		CAPTURE(shp::shortLabelOf(shp::kExtraSwitches[i]));
		const visage::Bounds b = card->extraBounds(i);
		CHECK(b.x() >= 0.0f);
		CHECK(b.right() <= card->width());
	}

	// AND EVERY CHILD WIDGET, which is the general form of the same claim — a knob, a box or a switch
	// added later is covered without editing this case.
	for (const visage::Frame* child : card->children()) {
		const visage::Bounds b = child->bounds();
		CAPTURE(b.x());
		CAPTURE(b.right());
		CHECK(b.x() >= 0.0f);
		CHECK(b.right() <= card->width());
		CHECK(b.y() >= 0.0f);
	}
}
TEST_CASE("a modulator card's enable square toggles the device, not just the declaration") {
	// **Disable on the MSEG did nothing.** It did nothing because
	// `MsegCard::onMouseDown` returned for any press above `bodyTopEdge()` — and the enable square the
	// shared head draws lives there. The square was painted, hit-tested by nothing, and the verb
	// existed on no road at all. `EnvCard` was identical.
	//
	// **AND THE REACHABILITY SWEEP SAID IT WAS FINE.** Both cards DECLARE the enable in
	// `addressedParamsForTest`, with a comment asserting "the press from `FxCard::enableBounds`" — a
	// press the base never dispatches, because `FxCard`'s constructor binds only the drag and the
	// release ("the press is the card's"). The guard whose whole job is catching a control drawn but
	// reachable by nothing was satisfied by a DECLARATION rather than by a fact.
	//
	// **SO THIS CASE PRESSES THE SQUARE AND READS THE FIELD.** That is the difference between asking a
	// card what it reaches and finding out: a declaration cannot fail, and this can.
	ModFixture f;
	REQUIRE(f.panel != nullptr);
	f.panel->resized();

	const auto pressEnable = [&](size_t slot) {
		auto* card = const_cast<visage::Frame*>(f.panel->cardFrameForTest(slot));
		REQUIRE(card != nullptr);
		auto* asCard = dynamic_cast<ui::FxCard*>(card);
		REQUIRE(asCard != nullptr);
		REQUIRE(asCard->hasEnable());
		const visage::Bounds sw = asCard->enableBounds();
		REQUIRE(sw.width() > 0.0f);
		visage::MouseEvent e {};
		e.button_id = visage::kMouseButtonLeft;
		e.button_state = visage::kMouseButtonLeft;
		e.repeat_click_count = 1;
		e.is_down = true;
		e.position = { sw.x() + sw.width() * 0.5f, sw.y() + sw.height() * 0.5f };
		asCard->onMouseDown().callback(e);
	};

	// THE ENVELOPE IN SLOT 0, which is what a default rack holds.
	const auto enableOf = [&](size_t slot) {
		const feathers::ModRack rack = f.bridge.modRack();
		const feathers::device::Descriptor* d = feathers::descriptorForRow(rack.device[slot]);
		REQUIRE(d != nullptr);
		REQUIRE(d->traits.enableControl >= 0);
		return rack.device[slot].field[static_cast<size_t>(d->traits.enableControl)];
	};

	const float before = enableOf(0);
	// NOT VACUOUS: it starts ON, so "it changed" is a change away from on rather than a no-op that
	// happens to read differently.
	REQUIRE(before >= 0.5f);
	pressEnable(0);
	CHECK(enableOf(0) < 0.5f);

	// **AND BACK — the half was hit on OWLTT ("disables but wont reenable").** A toggle that only goes
	// one way is a switch with one position, and asserting only the first press would pass on one.
	pressEnable(0);
	CHECK(enableOf(0) >= 0.5f);
}
TEST_CASE("toggling a modulator's enable is UNDOABLE, like every other control's edit (BM)") {
	// **A DEFECT FOUND BY REFACTORING, NOT BY A REPORT, AND IT WAS MINE.**
	//
	// The ten cards' enable presses were ten copies, and not even the same copy. The fx cards toggled
	// through a `DeviceControl` — `beginGesture` / `setValue` / `endGesture` — while the two MODULATOR
	// cards wrote the field straight through the host with `setField`. `DeviceControl` is where the
	// undo step is recorded, so **turning a modulator off was not undoable while turning an audio
	// effect off was.**
	//
	// Those two host-direct arms were added on 2026-09-02 while fixing the reported dead enable
	// presses. The surrounding style was matched without asking what the fx cards' extra three lines
	// were buying: CLAUDE.md's rule about load-bearing redundancy, met from the wrong side. The
	// bracketing read as ceremony and it was the history.
	//
	// NOTHING ASSERTED IT EITHER WAY. The sweep beside this one presses every card's square and checks
	// the DEVICE toggled, which BOTH spellings pass — an undo step is invisible to it. So the
	// difference between the two roads was unobservable to the whole suite.
	//
	// **BOTH MODULATOR CARDS, AND THE SECOND ONE IS HERE BECAUSE THE KNOWN-BAD CAUGHT ME.** The first
	// draft drove the panel's slot 0, which a default rack fills with a DAHDSR — so mutating
	// `MsegCard` back to a host-direct write left the case GREEN. An inert mutation reported as a
	// pass, which is the trap `green-mutation-may-be-inert` names. The card kinds are enumerated now,
	// so neither road is pinned by accident.
	namespace mseg = feathers::mod::mseg;

	const auto pressEnableOn = [](ui::FxCard& card, ui::EditHistory& history,
	                              const std::function<float()>& enableOf) {
		card.setBounds(0, 0, static_cast<int>(ui::ModPanel::kCardWidth), 400);
		REQUIRE(card.hasEnable());
		const visage::Bounds sw = card.enableBounds();
		REQUIRE(sw.width() > 0.0f);

		const float before = enableOf();
		visage::MouseEvent e {};
		e.button_id = visage::kMouseButtonLeft;
		e.button_state = visage::kMouseButtonLeft;
		e.repeat_click_count = 1;
		e.is_down = true;
		e.position = { sw.x() + sw.width() * 0.5f, sw.y() + sw.height() * 0.5f };
		card.onMouseDown().callback(e);

		// THE TOGGLE HAPPENED, SAID FIRST — an undo assertion over an edit that never occurred would
		// pass for a card whose square does nothing at all, which is the bug the neighbour exists for.
		CHECK(enableOf() != doctest::Approx(before));

		// **AND IT IS ON THE HISTORY.** This is the claim: the write went down the road that records an
		// undo step, not the one that forgets it.
		REQUIRE(history.canUndo());
		history.undo();
		CHECK(enableOf() == doctest::Approx(before));
	};

	SUBCASE("the envelope card") {
		ModFixture f;
		ui::ModState mod;
		ui::EditHistory history;
		feathers::ModRack rack = feathers::defaultModRack();
		f.bridge.restoreModRack(rack);
		REQUIRE(f.bridge.modRack().holds(0, feathers::mod::dahdsr::kTraits.tag));

		ui::EnvCard card { f.deviceHost, f.bridge.rackDeviceAt(0), mod, history };
		pressEnableOn(card, history, [&] { return f.deviceHost.fieldAt(f.bridge.rackDeviceAt(0), card.enableControl()); });
	}

	SUBCASE("and the MSEG card, which the first draft of this case never reached") {
		ModFixture f;
		ui::ModState mod;
		ui::EditHistory history;
		feathers::ModRack rack = feathers::defaultModRack();
		mseg::Settings shape;
		shape.count = 3;
		rack.setMsegAt(0, shape);
		f.bridge.restoreModRack(rack);
		REQUIRE(f.bridge.modRack().holds(0, mseg::kTraits.tag));

		ui::MsegCard card { f.deviceHost, f.bridge.rackDeviceAt(0), mod, history };
		pressEnableOn(card, history, [&] { return f.deviceHost.fieldAt(f.bridge.rackDeviceAt(0), card.enableControl()); });
	}
}
TEST_CASE("both device hosts OVERRIDE modulatedFieldAt — the default would answer the base") {
	// **THE GAP BK NAMED IN ITS OWN COMMENT AND COULD NOT CLOSE.** `DeviceHost::modulatedFieldAt`
	// defaults to returning `fieldAt` — the base — which is the honest answer for a host that has no
	// modulated channel. Feathers' host DOES have one now, for both its chains, and overrides it. But
	// delete the override, or either chain's branch of it, and the default quietly takes over: every
	// ring on that surface stops moving,
	// the sound is unchanged, and NOT ONE case goes red. That is the shape it was reported on
	// 2026-09-03 and it would return silently.
	//
	// It could not be written until now. The claim is about the HOSTS, the hosts pull in visage, and
	// the only suite with visage was `test_gui_widgets.cpp` — frozen for the split. The split landed,
	// so this is the debt being paid rather than a new idea.
	//
	// ASKED OF THE HOSTS, NOT OF THE BRIDGE. `test_gui_bridge.cpp` already pins the three readers
	// underneath; what is unpinned is the two-line forward on top of them, and a case that asked the
	// bridge again would pass with both overrides deleted.
	namespace dsr = feathers::mod::dahdsr;
	namespace filter = feathers::fx::filter;

	ModFixture f;
	const auto sustain = static_cast<size_t>(dsr::Param::Sustain);

	// ---- the rack side: a base on the raw channel, a different value on the modulated one ----
	feathers::ModRack raw = feathers::defaultModRack();
	dsr::Settings set;
	set.sustain = 0.25f;
	raw.setDahdsrAt(0, set);
	f.bridge.restoreModRack(raw);

	feathers::ModRack moved = feathers::defaultModRack();
	dsr::Settings after = set;
	after.sustain = 0.75f;
	moved.setDahdsrAt(0, after);

	// ---- the fx side, published on the same call ----
	feathers::FxChain rawFx;
	const size_t fxSlot = feathers::addFxDevice(&rawFx, filter::kTraits.tag);
	REQUIRE(fxSlot < rawFx.device.size());
	size_t cutoff = filter::kDescriptor.params.size();
	for (size_t i = 0; i < filter::kDescriptor.params.size(); ++i) {
		if (std::string(filter::kDescriptor.params[i].name).find("Cutoff") != std::string::npos) {
			cutoff = i;
			break;
		}
	}
	REQUIRE(cutoff < filter::kDescriptor.params.size());
	const float fxBase = rawFx.device[fxSlot].row.field[cutoff];
	f.bridge.restoreFxChain(rawFx);

	feathers::FxChain movedFx = rawFx;
	const float fxAfter = fxBase + 1.5f;
	movedFx.device[fxSlot].row.field[cutoff] = fxAfter;

	f.bridge.publishModulated(moved, movedFx, feathers::RackPlayheads {}, feathers::mod::TransportClock::unknown());

	SUBCASE("a device in the strip") {
		CHECK(f.deviceHost.fieldAt(f.bridge.rackDeviceAt(0), sustain) == doctest::Approx(0.25f));
		// THE CLAIM: the host reports where modulation left it, which only an override can do.
		CHECK(f.deviceHost.modulatedFieldAt(f.bridge.rackDeviceAt(0), sustain) == doctest::Approx(0.75f));
		// AND THE TWO DIFFER, which is what the default cannot produce — it returns one value twice.
		CHECK(f.deviceHost.modulatedFieldAt(f.bridge.rackDeviceAt(0), sustain) != doctest::Approx(f.deviceHost.fieldAt(f.bridge.rackDeviceAt(0), sustain)));
	}

	SUBCASE("a device in the lane") {
		CHECK(f.deviceHost.fieldAt(f.bridge.fxDeviceAt(fxSlot), cutoff) == doctest::Approx(fxBase));
		CHECK(f.deviceHost.modulatedFieldAt(f.bridge.fxDeviceAt(fxSlot), cutoff) == doctest::Approx(fxAfter));
		CHECK(f.deviceHost.modulatedFieldAt(f.bridge.fxDeviceAt(fxSlot), cutoff)
		      != doctest::Approx(f.deviceHost.fieldAt(f.bridge.fxDeviceAt(fxSlot), cutoff)));
	}
}
TEST_CASE("an fx control's ring draws where modulation left it, and its printed value stays on the base") {
	// **THE OTHER READER OF THE SAME CHANNEL, AND IT DREW THE BASE (backlog DX.1).** The case above pins
	// `BridgeDeviceHost::modulatedFieldAt` — the road `DeviceControl` takes. `FxControl` is the seating
	// every fx CARD's knob is built from, and it assigned `out.modulated = out.base` under a comment
	// saying the fx lane had no modulated channel. `publishModulated` carries the lane (BK); the comment
	// had simply stopped being true, so every fx ring sat still however much was routed at it — the
	// defect BE cost an evening of rings that were present and never moved), left standing on this one.
	//
	// THE VALUE, NOT A PICTURE (CLAUDE.md): `display()` is read for its numbers. A rendered comparison
	// would pass whenever anything differed, which is exactly what a half-working ring does.
	namespace filter = feathers::fx::filter;
	ModFixture f;

	feathers::FxChain raw;
	const size_t slot = feathers::addFxDevice(&raw, filter::kTraits.tag);
	REQUIRE(slot < raw.device.size());
	// A CONTROL THAT IS ACTUALLY A DESTINATION, found through the device's own declaration: `display()`
	// returns early for one that is not, so a control picked by eye could pass this case having proved
	// nothing about the line it is here to pin.
	size_t control = filter::kDescriptor.params.size();
	for (size_t c = 0; c < filter::kDescriptor.params.size(); ++c) {
		if (filter::kDescriptor.params[c].modulatable) {
			control = c;
			break;
		}
	}
	REQUIRE(control < filter::kDescriptor.params.size());
	const float base = raw.device[slot].row.field[control];
	f.bridge.restoreFxChain(raw);

	ui::FxControl knob { f.bridge, f.bridge.fxDeviceAt(slot), filter::kDescriptor, control };
	REQUIRE(knob.display().isDestination); // or the early return answers, and this proves nothing

	// NOTHING PUBLISHED YET: the ring is the base, which is also what the old line always answered —
	// so this half passes either way and is here to make the half below attributable.
	CHECK(knob.display().modulated == doctest::Approx(knob.display().base));

	// THE ENGINE'S OWN ARITHMETIC, PUBLISHED: the same row with this control somewhere else.
	feathers::FxChain moved = raw;
	const float after = base + 1.5f;
	moved.device[slot].row.field[control] = after;
	f.bridge.publishModulated(feathers::defaultModRack(), moved, feathers::RackPlayheads {}, feathers::mod::TransportClock::unknown());

	// THE ASSERTION THIS CASE EXISTS FOR — the moving part of the indicator is where modulation left it.
	CHECK(knob.display().modulated == doctest::Approx(after));
	// AND THE PRINTED VALUE AND THE POINTER STAY PUT: `modulated` is the only thing that moved.
	CHECK(knob.display().base == doctest::Approx(base));
	CHECK(knob.text() == ui::formatValue(knob.desc(), static_cast<double>(base)));
	// NOT VACUOUS: the two differ, which a line that answers the base twice cannot produce.
	CHECK(knob.display().modulated != doctest::Approx(knob.display().base));
}
TEST_CASE("zoom scales what a drag commits") {
	// **THIS STARTED AS A SUBCASE AND PROVED NOTHING.** It compared the stored travel before and after
	// a drag at two zooms — but `wrapForStorage` reduces the blob MODULO ONE, so a difference of
	// stored values is a difference of WRAPPED numbers and not a magnitude. Deleting the zoom divisor
	// left it green. Each drag therefore happens from a FRESH device here, so the stored value IS the
	// committed travel and nothing has wrapped.
	namespace shp = feathers::fx::shaper;

	const auto committedAt = [](double zoom) {
		ShaperFixture f;
		REQUIRE(f.panel != nullptr);
		auto* pad = const_cast<ui::ShaperPad*>(f.panel->padForTest());
		REQUIRE(pad != nullptr);
		REQUIRE(pad->width() > 1.0f);
		pad->setZoom(zoom);
		const visage::Point centre { pad->width() * 0.5f, pad->height() * 0.5f };
		pad->onMouseDown().callback(at(centre.x, centre.y));
		// A SMALL DRAG, so even at x1 the committed travel stays well under one turn and the wrap
		// cannot reach it. The claim is about the RATIO, so the absolute size is free to be safe.
		pad->onMouseDrag().callback(at(centre.x + pad->width() * 0.1f, centre.y));
		pad->onMouseUp().callback(at(centre.x, centre.y));
		pad->finishGlideForTest(); // the release glide, landed as a frame loop would (backlog IJ)

		const FxChain chain = f.f.bridge.fxChain();
		double magnitude = 0.0;
		for (size_t i = 0; i < shp::phase_space::kPhaseCount; ++i) {
			magnitude += std::fabs(fxDeviceAtPosition(chain, f.slot).row.field[shp::kTravelFirst + i]);
		}
		return magnitude;
	};

	const double atOne = committedAt(1.0);
	const double atFour = committedAt(4.0);
	CAPTURE(atOne);
	CAPTURE(atFour);

	// NON-VACUITY FIRST: the drag committed something at all, or the ratio below is 0 < 0.
	CHECK(atOne > 1e-6);
	// **AND FOUR TIMES IN IS A QUARTER OF THE GROUND.** Asserted as a ratio with room either side, not
	// as an equality: the basis distributes the travel differently to each phase, so the sum is not
	// required to scale exactly — only to scale.
	CHECK(atFour < atOne * 0.5);
	CHECK(atFour > atOne * 0.1);
}
TEST_CASE("the terrain is the plane, and follows it") {
	// **THE PICTURE HAS TO BE THE SPACE THE PUCK MOVES IN.** It used to sample `activityAt(colour,
	// gamma)` — the two-knob square — while a drag travelled through the plane. MEASURED, those two
	// coordinates point the same way (0 degrees apart at colour 0, 5.5 at colour 1), so the square was
	// a line and the picture was of country no drag visits.
	//
	// **ASSERTED AS "IT MOVED", NOT AS PARTICULAR CELLS.** What the terrain shows at a point is the
	// total variation of the curve you would get by dragging there; pinning cell values would pin the
	// field's shape, which is not this case's business and would go red on any character change.
	namespace shp = feathers::fx::shaper;
	ShaperFixture f;
	REQUIRE(f.panel != nullptr);
	auto* pad = const_cast<ui::ShaperPad*>(f.panel->padForTest());
	REQUIRE(pad != nullptr);
	REQUIRE(pad->width() > 1.0f);
	// ONE FRAME FIRST: no map is built at construction, at width 0 (the phaser plan's final review), so the first is
	// the first draw's, at the pad's real size. This case used to snapshot the width-0 one, never drawn on screen.
	{
		visage::Canvas canvas;
		pad->draw(canvas);
	}
	REQUIRE(pad->rebuildsForTest() >= 1);

	const auto snapshot = [&] { return pad->mapForTest(); };
	const auto differs = [](const auto& a, const auto& b) {
		size_t n = 0;
		for (size_t i = 0; i < a.size(); ++i) {
			if (a[i] != b[i]) {
				++n;
			}
		}
		return n;
	};

	const auto atRest = snapshot();
	// NON-VACUITY: the map has content at all. A terrain of all zeros would "follow" everything.
	bool anyInk = false;
	for (const float v : atRest) {
		anyInk = anyInk || v > 0.001f;
	}
	CHECK(anyInk);

	SUBCASE("a drag moves the picture, because the plane's origin moved") {
		const visage::Point centre { pad->width() * 0.5f, pad->height() * 0.5f };
		pad->onMouseDown().callback(at(centre.x, centre.y));
		pad->onMouseDrag().callback(at(centre.x + pad->width() * 0.3f, centre.y - pad->height() * 0.2f));
		pad->onMouseUp().callback(at(centre.x, centre.y));
		pad->finishGlideForTest(); // the release glide, landed as a frame loop would (backlog IJ)
		CAPTURE(differs(atRest, snapshot()));
		CHECK(differs(atRest, snapshot()) > 0);
	}

	SUBCASE("ROTATE moves the picture and NOT the sound") {
		// **THE ONE PLACE ROTATE IS ALLOWED TO SHOW.** It turns the basis, so the plane's contents are
		// re-aimed and the terrain must redraw — while the sound at rest cannot move, which is what
		// `Shaper :: Shaper Rotate` being on the dead list asserts from the other side. A rotate that
		// changed neither would be a control that does nothing at all.
		const FxChain before = f.f.bridge.fxChain();
		f.f.bridge.setDeviceControl(f.device, static_cast<size_t>(shp::Param::Rotate), 0.4f);
		// THE WRITE LANDED, checked before anything is concluded from what the picture did: a case that
		// asserted a redraw after a write that never happened would be reporting on nothing.
		REQUIRE(f.f.bridge.fxChain().device[f.slot].row.field[static_cast<size_t>(shp::Param::Rotate)]
		        == doctest::Approx(0.4f));
		pad->redraw();
		visage::Canvas canvas;
		pad->draw(canvas);
		CAPTURE(differs(atRest, snapshot()));
		CHECK(differs(atRest, snapshot()) > 0);

		// AND THE COORDINATES AND TRAVEL ARE UNTOUCHED — the sound is where it was.
		const FxChain after = f.f.bridge.fxChain();
		CHECK(after.device[f.slot].row.field[static_cast<size_t>(shp::Param::Colour)]
		      == before.device[f.slot].row.field[static_cast<size_t>(shp::Param::Colour)]);
		for (size_t i = 0; i < shp::phase_space::kPhaseCount; ++i) {
			CAPTURE(i);
			CHECK(after.device[f.slot].row.field[shp::kTravelFirst + i]
			      == before.device[f.slot].row.field[shp::kTravelFirst + i]);
		}
	}

	SUBCASE("zoom moves the picture, because the span it covers changed") {
		pad->setZoom(8.0);
		CAPTURE(differs(atRest, snapshot()));
		CHECK(differs(atRest, snapshot()) > 0);
	}
}
TEST_CASE("the puck follows the hand during a drag, and rests at centre") {
	// The joystick did not move, and the cause is that the
	// puck was reading the wrong quantity. `commitAndReanchor` commits the travel and then ZEROES it —
	// on every drag event, because that is how the origin re-anchors — and `puck()` was reading exactly
	// that travel. So the puck was pinned at the centre for the whole gesture while the field moved
	// underneath it.
	//
	// **THE DEFLECTION IS A VIEW OF THE HAND, NOT A SECOND COPY OF THE TRAVEL.** It is measured from
	// where the gesture started, which is fixed for the gesture; the commit keeps measuring deltas from
	// the anchor, which moves. That is why this case can assert a GROWING deflection across a
	// continuous drag: the two quantities answer different questions.
	ShaperFixture f;
	REQUIRE(f.panel != nullptr);
	auto* pad = const_cast<ui::ShaperPad*>(f.panel->padForTest());
	REQUIRE(pad != nullptr);
	REQUIRE(pad->width() > 1.0f);

	const visage::Point centre { pad->width() * 0.5f, pad->height() * 0.5f };
	CHECK(pad->puck().x == doctest::Approx(centre.x));
	CHECK(pad->puck().y == doctest::Approx(centre.y));

	pad->onMouseDown().callback(at(centre.x, centre.y));
	// STILL AT CENTRE ON THE PRESS: nothing has been dragged yet.
	CHECK(pad->puck().x == doctest::Approx(centre.x));

	const float step = pad->width() * 0.1f;
	pad->onMouseDrag().callback(at(centre.x + step, centre.y));
	const visage::Point one = pad->puck();
	pad->onMouseDrag().callback(at(centre.x + step * 2.0f, centre.y));
	const visage::Point two = pad->puck();

	// **THE VALUE, NOT JUST A DIFFERENCE.** The deflection is the pointer's offset from where the
	// gesture began, so it is computable independently — and computing it is what distinguishes "the
	// puck moved" from "the puck moved by the right amount". A puck that tracked the per-event delta
	// would pass a difference check and sit at a tenth of the pad here instead of a fifth.
	CHECK(one.x == doctest::Approx(centre.x + step));
	CHECK(two.x == doctest::Approx(centre.x + step * 2.0f));
	// AND IT DID MOVE, which keeps the two equalities from being a pair of coincidences.
	CHECK(two.x > one.x);
	// THE AXIS THAT WAS NOT DRAGGED DOES NOT MOVE.
	CHECK(two.y == doctest::Approx(centre.y));

	// **AND IT COMES HOME ON RELEASE.** The travel is committed, so the chart's origin IS the new
	// position: there is no standing deflection left for a turn of the plane to move.
	pad->onMouseUp().callback(at(centre.x + step * 2.0f, centre.y));
	pad->finishGlideForTest(); // the release glide, landed as a frame loop would (backlog IJ)
	CHECK(pad->puck().x == doctest::Approx(centre.x));
	CHECK(pad->puck().y == doctest::Approx(centre.y));
}
TEST_CASE("the terrain does not rebuild inside a gesture") {
	// The terrain seemed to skip redraws. It was not skipping them — it was
	// spending 10.6 ms on each one, once per frame, for the whole drag.
	//
	// `onMouseUp` already carried the rule ("the picture catches up on release, not inside the
	// gesture") and `draw()` was quietly defeating it: a drag commits travel on every mouse event, that
	// moves the view, and `draw()`'s own staleness check fired the rebuild. Two correct-looking guards
	// whose composition was the bug.
	//
	// **COUNTED, NOT COMPARED.** The claim is that no rebuild HAPPENED, and a rebuild landing on
	// similar cells is invisible to a picture comparison while costing the whole 10.6 ms.
	ShaperFixture f;
	REQUIRE(f.panel != nullptr);
	auto* pad = const_cast<ui::ShaperPad*>(f.panel->padForTest());
	REQUIRE(pad != nullptr);
	REQUIRE(pad->width() > 1.0f);

	visage::Canvas canvas;
	const visage::Point centre { pad->width() * 0.5f, pad->height() * 0.5f };

	pad->draw(canvas);
	const size_t settled = pad->rebuildsForTest();

	pad->onMouseDown().callback(at(centre.x, centre.y));
	for (int i = 1; i <= 6; ++i) {
		pad->onMouseDrag().callback(at(centre.x + pad->width() * 0.05f * static_cast<float>(i), centre.y));
		pad->draw(canvas);
	}
	// SIX FRAMES INSIDE THE GESTURE, EACH WITH THE VIEW GENUINELY MOVED, AND NOT ONE REBUILD.
	CHECK(pad->rebuildsForTest() == settled);

	pad->onMouseUp().callback(at(centre.x + pad->width() * 0.3f, centre.y));
	pad->finishGlideForTest(); // the release glide, landed as a frame loop would (backlog IJ)
	// **AND THE PICTURE DOES CATCH UP.** Without this the case would pass on a pad that never rebuilds
	// at all, which is the same stale terrain by another route.
	CHECK(pad->rebuildsForTest() > settled);

	// A SECOND DRAW AFTER RELEASE COSTS NOTHING: the view has not moved since the release rebuilt it.
	const size_t afterRelease = pad->rebuildsForTest();
	pad->draw(canvas);
	CHECK(pad->rebuildsForTest() == afterRelease);
}

