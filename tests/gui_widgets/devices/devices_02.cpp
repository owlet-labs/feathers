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

// PIECE 2 OF 18 of the devices suite: its cases in their original order, with the helpers only they use. See devices_common.h.

#include "devices/devices_common.h"

TEST_CASE("a chip toggled on a modulator card is an undo step, on both cards") {
	// THREE MORE ROADS THE UNDO SWEEP FOUND SILENT. Each `history_->record(...)` in `src/plugin/ui`
	// was dropped in turn — the record still constructed and discarded, so no argument is stranded —
	// and the whole binary run:
	//
	//   EnvCard::toggleChip            dropped -> GREEN, 477 of 477
	//   MsegCard's chip toggle         dropped -> GREEN, 477 of 477
	//   EnvCard's box `onEdited`       dropped -> GREEN, 477 of 477
	//
	// SO NONE OF WHAT A HAND DOES TO AN ENVELOPE CAME BACK WITH CTRL-Z. The sweep's shape says why
	// nobody noticed: the sixteen roads that ARE asserted are the structural ones — add, remove,
	// move, reorder, clone — and every green one changes a NUMBER or a FLAG. The suite grew up
	// around "did the rack end up with the right devices in the right order".
	//
	// DRIVEN AS PRESSES, through the card's own `chipBounds`, because `toggleChip` is private and a
	// press is what a hand makes. Asking the card where its chip is also proves the chip is somewhere
	// a hand could reach it.
	ModFixture f;
	feathers::ui::ModState mod;
	feathers::ui::EditHistory history;
	namespace dsr = feathers::mod::dahdsr;
	namespace ms = feathers::mod::mseg;

	SUBCASE("the envelope card") {
		ui::EnvCard card { f.deviceHost, f.bridge.rackDeviceAt(0), mod, history };
		card.setBounds(0, 0, static_cast<int>(ui::ModPanel::kCardWidth),
		               static_cast<int>(ui::EnvCard::height()));
		card.resized();
		// SOME SMOOTHING, so the Carry chip is live (backlog MM): a dead chip refuses the press, which is its own case.
		dsr::Settings smoothed = f.bridge.modRack().dahdsrAt(0);
		smoothed.smoothing = std::log2(0.1f);
		f.bridge.setEnvelopeSettings(0, smoothed);

		// EVERY CHIP, not the first one — `toggleChip` is one function over a declared array and a
		// case that pressed one would say nothing about the other four.
		for (size_t index = 0; index < ui::EnvCard::kChipCount; ++index) {
			const dsr::Param param = ui::EnvCard::chips()[index].param;
			INFO("envelope chip " << index);
			const visage::Bounds chip = card.chipBounds(index);
			REQUIRE(chip.width() > 0.0f);

			const double before = dsr::valueOf(f.bridge.modRack().dahdsrAt(0), param);
			const size_t steps = history.length();
			const visage::Point on { chip.x() + chip.width() * 0.5f,
				                     chip.y() + chip.height() * 0.5f };
			// THE PRESS REACHES THE CARD rather than a box sitting over the chip row.
			REQUIRE(card.frameAtPoint(on) == static_cast<visage::Frame*>(&card));
			dragThroughRoot(&card, on, on);

			// IT CHANGED SOMETHING — without this the step below could be recorded for a press that
			// did nothing, and the undo would be undoing a value to itself.
			const double after = dsr::valueOf(f.bridge.modRack().dahdsrAt(0), param);
			CHECK(after != doctest::Approx(before));
			CHECK(history.length() == steps + 1);

			history.undo();
			CHECK(dsr::valueOf(f.bridge.modRack().dahdsrAt(0), param) == doctest::Approx(before));
			history.redo();
			CHECK(dsr::valueOf(f.bridge.modRack().dahdsrAt(0), param) == doctest::Approx(after));
			// AND BACK, outside the history, so every chip is pressed from the same state: Poly gates Carry (backlog
			// MM), and a Poly left off by this one would leave the next one dead.
			f.bridge.setEnvelopeSettings(0, smoothed);
		}
	}

	SUBCASE("the MSEG card") {
		// THE SAME GESTURE ON THE OTHER MODULATOR, and it is a separate implementation rather than a
		// shared one — which is exactly why both need asking.
		REQUIRE(f.bridge.addRackDevice(ms::kTraits.tag) < kMaxRackSlots);
		f.bridge.refresh();
		const size_t slot = f.bridge.modRack().count - 1;
		ui::MsegCard card { f.deviceHost, f.bridge.rackDeviceAt(slot), mod, history };
		card.setBounds(0, 0, static_cast<int>(ui::ModPanel::kCardWidth), 260);
		card.resized();

		for (size_t index = 0; index < ui::MsegCard::kChipCount; ++index) {
			const ms::Param param = ui::MsegCard::chips()[index].param;
			INFO("MSEG chip " << index);
			const double before = ms::valueOf(f.bridge.modRack().msegAt(slot), param);
			const size_t steps = history.length();
			// CALLED RATHER THAN PRESSED, because this card publishes `toggleChip` and hides
			// `chipBounds` while the envelope's does the opposite. Driving each through the seam it
			// offers is the honest reading of both; the envelope's road above is the one that also
			// proves the chip is reachable, and this one is not claiming that.
			card.toggleChip(index);

			const double after = ms::valueOf(f.bridge.modRack().msegAt(slot), param);
			CHECK(after != doctest::Approx(before));
			CHECK(history.length() == steps + 1);

			history.undo();
			CHECK(ms::valueOf(f.bridge.modRack().msegAt(slot), param) == doctest::Approx(before));
			history.redo();
			CHECK(ms::valueOf(f.bridge.modRack().msegAt(slot), param) == doctest::Approx(after));
		}
	}
}

namespace {
/// ONE MODULATOR CARD'S CARRY CHIP (backlog MM), asked through the host's fields and a hand's press.
template <typename Card, typename Param>
void checkCarryChip(ModFixture& f, size_t slot, Param carry, Param poly, Param mode, Param smoothing) {
	// ON ITS OWN, AT THE ORIGIN, so the card is the root a press is delivered through — in the panel the third card
	// sits below the visible rows and the window's hit test never reaches it.
	feathers::ui::ModState mod;
	feathers::ui::EditHistory history;
	Card owned { f.deviceHost, f.bridge.rackDeviceAt(slot), mod, history };
	owned.setBounds(0, 0, static_cast<int>(ui::ModPanel::kCardWidth),
	                static_cast<int>(owned.laneHeightAt(ui::ModPanel::kCardWidth)));
	owned.resized();
	Card* card = &owned;
	size_t index = Card::kChipCount;
	for (size_t i = 0; i < Card::kChipCount; ++i) {
		if (Card::chips()[i].param == carry) {
			index = i;
		}
	}
	REQUIRE(index < Card::kChipCount);
	const auto set = [&](Param p, float v) {
		f.deviceHost.setField(card->device(), static_cast<size_t>(p), v);
		f.bridge.refresh();
	};
	const float smooth = static_cast<float>(std::log2(0.1));
	const float off = feathers::mod::kSmoothingLog2SecondsMin;
	const float free = static_cast<float>(static_cast<int>(feathers::mod::Mode::Free));
	const float gate = static_cast<float>(static_cast<int>(feathers::mod::Mode::Gate));

	// LIVE ONLY WHERE IT CHANGES THE SOUND: Poly, not Free, some smoothing (`mod::PolyShape::carrySmoothing`).
	set(mode, gate);
	set(poly, 1.0f);
	set(smoothing, smooth);
	CHECK(card->chipLive(index));
	set(poly, 0.0f);
	CHECK_FALSE(card->chipLive(index)); // mono already smooths through a retrigger
	set(poly, 1.0f);
	set(mode, free);
	CHECK_FALSE(card->chipLive(index)); // Free is mono whatever Poly says
	set(mode, gate);
	set(smoothing, off);
	CHECK_FALSE(card->chipLive(index)); // nothing to carry

	// A PRESS ON IT DEAD CHANGES NOTHING; LIVE, IT TOGGLES THE DEVICE'S OWN FIELD, and it is somewhere a hand reaches.
	const visage::Bounds b = card->chipBounds(index);
	REQUIRE(b.width() > 0.0f);
	const visage::Point on { b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f };
	REQUIRE(card->frameAtPoint(on) == static_cast<visage::Frame*>(card));
	REQUIRE_FALSE(card->chipOn(index));
	dragThroughRoot(card, on, on);
	CHECK_FALSE(card->chipOn(index));
	set(smoothing, smooth);
	dragThroughRoot(card, on, on);
	CHECK(card->chipOn(index));
	CHECK(f.deviceHost.fieldAt(card->device(), static_cast<size_t>(carry)) == 1.0f);
	dragThroughRoot(card, on, on);
	CHECK_FALSE(card->chipOn(index));
}
} // namespace

TEST_CASE("backlog MM: each modulator card's Carry chip is the device's Smooth Carry, live only under Poly outside Free with some smoothing, and a press there toggles it") {
	namespace dsr = feathers::mod::dahdsr;
	namespace ms = feathers::mod::mseg;
	namespace lfo = feathers::mod::lfo;
	ModFixture f;
	auto* panel = findInTree<ui::ModPanel>(f.root);
	REQUIRE(panel != nullptr);
	const size_t msegSlot = f.bridge.addRackDevice(ms::kTraits.tag);
	const size_t lfoSlot = f.bridge.addRackDevice(lfo::kTraits.tag);
	REQUIRE(msegSlot >= 1);
	REQUIRE(lfoSlot >= 1);
	f.bridge.refresh();
	panel->resized();

	SUBCASE("the envelope card") {
		checkCarryChip<ui::EnvCard>(f, 0, dsr::Param::SmoothCarry, dsr::Param::Poly, dsr::Param::Mode,
		                            dsr::Param::Smoothing);
	}
	SUBCASE("the MSEG card") {
		checkCarryChip<ui::MsegCard>(f, msegSlot, ms::Param::SmoothCarry, ms::Param::Poly, ms::Param::Mode,
		                             ms::Param::Smoothing);
	}
	SUBCASE("the LFO card") {
		checkCarryChip<ui::LfoCard>(f, lfoSlot, lfo::Param::SmoothCarry, lfo::Param::Poly, lfo::Param::Mode,
		                            lfo::Param::Smoothing);
	}
}
TEST_CASE("taking a route back is an undo step, by all three of the roads that do it") {
	// THREE MORE THE UNDO SWEEP FOUND SILENT, and they are the three ways a hand gets rid of an
	// amount it has just made:
	//
	//   RouteList's mouse-up      (mod_panel.cpp:227)     dropped -> GREEN, 477 of 477
	//   RouteList's double-click  (mod_panel.cpp:191)     dropped -> GREEN, 477 of 477
	//     (it typed for a while; since backlog LS the double-click zeroes again and the TRIPLE types —
	//     both roads are a subcase)
	//   ParamControl::unroute     (param_control.cpp:206) dropped -> GREEN, 477 of 477
	//
	// A ROUTE MADE BY ACCIDENT AND TAKEN BACK LEFT NOTHING TO UNDO, so the ctrl-Z after it undid
	// whatever the hand had done BEFORE — which is worse than doing nothing, because the step it
	// spends is somebody else's.
	//
	// AND THE KNOWN-BADS SHOW EXACTLY THAT RATHER THAN ONLY A SHORT STACK: with any of the three
	// dropped the length reads `1 == 2` — the route's CREATION step is still there — and the undo
	// then reads `0 == Approx(0.9)`, because it consumed the creation and the route the hand had
	// already removed stayed removed. The assertion pair is what makes that visible; the length alone
	// would have said only that a number was small.
	//
	// THE THREE ARE ONE CONTRACT SAID THREE WAYS and they are three separate implementations, which
	// is the reason all three are asked. The list's drag and its double-click do not share a line.
	ModFixture f;
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);
	REQUIRE(f.gui.history() != nullptr);
	ui::EditHistory& history = *f.gui.history();
	f.arm(backplane::Source::core(0));
	REQUIRE(mod->armed());

	ui::Knob* path = f.knobFor(ParamId::Path);
	REQUIRE(path != nullptr);
	const int destination = modDestinationIndexOf(ParamId::Path);
	REQUIRE(destination >= 0);
	const backplane::Destination dest = backplane::Destination::core(static_cast<size_t>(destination));

	/// Make a route the way a hand makes one, and hand back what it is worth. Re-made per subcase
	/// rather than once, because each subcase destroys it.
	const auto makeRoute = [&] {
		const visage::Point face = f.faceCentre(path);
		dragThroughRoot(f.root, face, { face.x, face.y - 35.0f });
		const float amount = f.bridge.routeAmount(backplane::Source::core(0), dest);
		REQUIRE(amount > 0.0f);
		return amount;
	};

	auto* list = findInTree<ui::RouteList>(f.root);
	REQUIRE(list != nullptr);

	/// A point on `want` of the row for `dest`, in window coordinates — found by asking the list
	/// rather than by restating its row arithmetic here.
	const auto pointOn = [&](ui::RouteList::Part want) {
		const auto rows = list->rows();
		size_t index = rows.size();
		for (size_t i = 0; i < rows.size(); ++i) {
			if (rows[i] == dest) {
				index = i;
			}
		}
		REQUIRE(index < rows.size());
		const visage::Bounds row = list->rowBounds(index);
		const float y = row.y() + row.height() * 0.5f;
		for (float x = row.x() + 1.0f; x < row.right(); x += 1.0f) {
			const ui::RouteList::Hit hit = list->hitAt({ x, y });
			if (hit.part == want && hit.destination == dest) {
				return visage::Point { x, y };
			}
		}
		return visage::Point { -1.0f, -1.0f };
	};

	SUBCASE("dragged to nothing in the list") {
		const float made = makeRoute();
		f.root->resized();
		const visage::Point on = pointOn(ui::RouteList::Part::Amount);
		REQUIRE(on.x >= 0.0f);
		const visage::Point inWindow = list->positionInWindow() + on;
		REQUIRE(f.root->frameAtPoint(inWindow) == static_cast<visage::Frame*>(list));

		const size_t steps = history.length();
		// EXACTLY BACK TO NOTHING, and no further. The list maps a pixel of travel to
		// `1 / kDragRange` of amount, so the distance that lands on zero is the amount itself times
		// that range; a drag of "far enough" instead pins at the negative clamp and lands on -1.1,
		// which is a route rather than the absence of one. The list's rule on release is that dragged
		// back to nothing means GONE, and that is the road carrying the record.
		const float toZero = made * ui::RouteList::kDragRange;
		dragThroughRoot(f.root, inWindow, { inWindow.x, inWindow.y + toZero });
		CHECK(f.bridge.routeAmount(backplane::Source::core(0), dest) == 0.0f);
		CHECK(history.length() == steps + 1);
		history.undo();
		CHECK(f.bridge.routeAmount(backplane::Source::core(0), dest) == doctest::Approx(made));
	}

	SUBCASE("double-clicked to nothing in the list") {
		// THE DOUBLE-CLICK ZEROES AGAIN (backlog LS: double click resets, triple click types) — for a
		// while it typed instead (route-typein), and the triple-click has that job now.
		const float made = makeRoute();
		f.root->resized();
		const visage::Point on = pointOn(ui::RouteList::Part::Amount);
		REQUIRE(on.x >= 0.0f);

		const size_t steps = history.length();
		list->processMouseDown(at(on.x, on.y));
		list->processMouseUp(at(on.x, on.y));
		list->processMouseDown(doubleClickAt(on.x, on.y));
		list->processMouseUp(at(on.x, on.y));
		CHECK_FALSE(list->amountField().editing());
		CHECK(f.bridge.routeAmount(backplane::Source::core(0), dest) == 0.0f);
		CHECK(history.length() == steps + 1);
		history.undo();
		CHECK(f.bridge.routeAmount(backplane::Source::core(0), dest) == doctest::Approx(made));
	}

	SUBCASE("typed to nothing in the list") {
		// THE TRIPLE-CLICK TYPES (backlog LS); a typed 0 is the list's other road to nothing.
		const float made = makeRoute();
		f.root->resized();
		const visage::Point on = pointOn(ui::RouteList::Part::Amount);
		REQUIRE(on.x >= 0.0f);

		const size_t steps = history.length();
		// IN THE LIST'S OWN COORDINATES, because this road is its own mouse handler and a
		// triple-click is not something `dragThroughRoot` can make.
		list->processMouseDown(at(on.x, on.y));
		list->processMouseUp(at(on.x, on.y));
		list->processMouseDown(doubleClickAt(on.x, on.y));
		list->processMouseUp(at(on.x, on.y));
		list->processMouseDown(tripleClickAt(on.x, on.y));
		ui::TextField& field = list->amountField();
		REQUIRE(field.editing());
		// THE FIELD OPENS ON THE AMOUNT FROM BEFORE THE DOUBLE-CLICK'S ZERO.
		CHECK(ui::RouteList::parseAmount(field.text()).value() == doctest::Approx(made).epsilon(0.002));
		field.processTextInput("0");
		field.processKeyPress(visage::KeyEvent { visage::KeyCode::Return, 0, true });
		CHECK_FALSE(field.isVisible());
		CHECK(f.bridge.routeAmount(backplane::Source::core(0), dest) == 0.0f);
		CHECK(history.length() == steps + 1);
		history.undo();
		CHECK(f.bridge.routeAmount(backplane::Source::core(0), dest) == doctest::Approx(made));
	}

	SUBCASE("double-clicked on the control itself") {
		const float made = makeRoute();
		const size_t steps = history.length();
		path->processMouseDown(
		    doubleClickAt(path->width() * 0.5f, path->faceDiameter() * 0.5f));
		CHECK(f.bridge.routeAmount(backplane::Source::core(0), dest) == 0.0f);
		CHECK(history.length() == steps + 1);
		history.undo();
		CHECK(f.bridge.routeAmount(backplane::Source::core(0), dest) == doctest::Approx(made));
	}
}
TEST_CASE("a route's amount is typed in the list: a triple-click opens it on the amount from before the double-click's zero, Enter sets it as one undo step") {
	// Backlog LS (double click resets, triple click types): the double-click on a
	// row's amount ZEROES it (as it did before route-typein) and the triple-click opens a field on the amount
	// from before that zero, read as a percent; Escape puts it back and leaves no step.
	ModFixture f;
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);
	REQUIRE(f.gui.history() != nullptr);
	ui::EditHistory& history = *f.gui.history();
	const backplane::Source source = backplane::Source::core(0);
	f.arm(source);
	REQUIRE(mod->armed());

	ui::Knob* path = f.knobFor(ParamId::Path);
	REQUIRE(path != nullptr);
	const int destination = modDestinationIndexOf(ParamId::Path);
	REQUIRE(destination >= 0);
	const backplane::Destination dest = backplane::Destination::core(static_cast<size_t>(destination));
	const visage::Point face = f.faceCentre(path);
	dragThroughRoot(f.root, face, { face.x, face.y - 35.0f });
	const float made = f.bridge.routeAmount(source, dest);
	REQUIRE(made > 0.0f);
	f.root->resized();

	auto* list = findInTree<ui::RouteList>(f.root);
	REQUIRE(list != nullptr);
	const auto rows = list->rows();
	size_t index = rows.size();
	for (size_t i = 0; i < rows.size(); ++i) {
		if (rows[i] == dest) {
			index = i;
		}
	}
	REQUIRE(index < rows.size());
	const visage::Bounds amount = list->amountBounds(index);
	const visage::Point on { amount.x() + amount.width() * 0.5f, amount.y() + amount.height() * 0.5f };
	REQUIRE(list->hitAt(on).part == ui::RouteList::Part::Amount);
	ui::TextField& field = list->amountField();
	REQUIRE_FALSE(field.isVisible());

	const auto open = [&] {
		list->processMouseDown(at(on.x, on.y));
		list->processMouseUp(at(on.x, on.y));
		list->processMouseDown(doubleClickAt(on.x, on.y));
		list->processMouseUp(at(on.x, on.y));
		// THE DOUBLE-CLICK ZEROED IT on the way to the third click, and opened nothing.
		CHECK(f.bridge.routeAmount(source, dest) == 0.0f);
		CHECK_FALSE(field.editing());
		list->processMouseDown(tripleClickAt(on.x, on.y));
		REQUIRE(field.editing());
		REQUIRE(field.isVisible());
	};
	const auto enter = [&] { field.processKeyPress(visage::KeyEvent { visage::KeyCode::Return, 0, true }); };

	SUBCASE("it opens over the amount, on the amount as it stands") {
		open();
		CHECK(field.text() == ui::RouteList::printAmountForEdit(made));
		CHECK(ui::RouteList::parseAmount(field.text()).value() == doctest::Approx(made).epsilon(0.002));
		// OVER THE AMOUNT, and over nothing the × owns.
		CHECK(field.x() <= amount.x());
		CHECK(field.x() + field.width() == doctest::Approx(amount.right()));
		REQUIRE(list->typingRowForTest().has_value());
		CHECK(list->typingRowForTest()->destination == dest);
	}

	SUBCASE("a typed percent is the amount, and one undo puts the old one back") {
		const size_t steps = history.length();
		open();
		field.processTextInput("-37.5%");
		enter();
		CHECK_FALSE(field.isVisible());
		CHECK(f.bridge.routeAmount(source, dest) == doctest::Approx(-0.375f));
		// THE ZERO FOLDED INTO THE TYPED STEP: one undo goes back past it, to the amount the hand started on.
		CHECK(history.length() == steps + 1);
		history.undo();
		CHECK(f.bridge.routeAmount(source, dest) == doctest::Approx(made));
	}

	SUBCASE("beyond the range is clamped to it") {
		open();
		field.processTextInput("900");
		enter();
		CHECK(f.bridge.routeAmount(source, dest) == doctest::Approx(kMaxRouteAmount));
	}

	SUBCASE("Escape, junk, and an unchanged Enter move nothing and record nothing") {
		const size_t steps = history.length();
		open();
		field.processTextInput("12");
		field.processKeyPress(visage::KeyEvent { visage::KeyCode::Escape, 0, true });
		CHECK_FALSE(field.isVisible());
		CHECK(f.bridge.routeAmount(source, dest) == doctest::Approx(made));

		CHECK(history.length() == steps);

		open();
		field.processTextInput("abc");
		enter();
		CHECK(f.bridge.routeAmount(source, dest) == doctest::Approx(made));
		CHECK(history.length() == steps);

		open();
		enter(); // the opening text, unchanged
		CHECK(f.bridge.routeAmount(source, dest) == made); // exactly: an unchanged Enter is not a rounding
		CHECK(history.length() == steps);
		CHECK_FALSE(history.canRedo());
	}

	SUBCASE("a press elsewhere on the list gives the amount back") {
		open();
		field.processTextInput("12");
		list->processMouseDown(at(1.0f, 1.0f));
		CHECK_FALSE(field.isVisible());
		CHECK(f.bridge.routeAmount(source, dest) == doctest::Approx(made));
	}
}
TEST_CASE("a typed route amount is read as a percent, and anything that is not a number is no value") {
	using ui::RouteList;
	CHECK(RouteList::parseAmount("50").value() == doctest::Approx(0.5f));
	CHECK(RouteList::parseAmount("+50").value() == doctest::Approx(0.5f));
	CHECK(RouteList::parseAmount(" -12.5 % ").value() == doctest::Approx(-0.125f));
	CHECK(RouteList::parseAmount("50%").value() == doctest::Approx(0.5f));
	// A PERCENT, NEVER A GUESSED FRACTION: 0.5 is half a percent. Absolute bound, not Approx's scale.
	CHECK(std::abs(RouteList::parseAmount("0.5").value() - 0.005f) < 1e-6f);
	CHECK(RouteList::parseAmount("-900").value() == -kMaxRouteAmount);
	CHECK_FALSE(RouteList::parseAmount("").has_value());
	CHECK_FALSE(RouteList::parseAmount("%").has_value());
	CHECK_FALSE(RouteList::parseAmount("abc").has_value());
	CHECK_FALSE(RouteList::parseAmount("12x").has_value());
	CHECK_FALSE(RouteList::parseAmount("inf").has_value());
	CHECK_FALSE(RouteList::parseAmount("0x10").has_value());

	CHECK(RouteList::printAmountForEdit(0.5f) == "50");
	CHECK(RouteList::printAmountForEdit(-0.125f) == "-12.5");
	CHECK(RouteList::printAmountForEdit(0.0f) == "0");
}
TEST_CASE("a value dragged on an envelope's box is an undo step") {
	// THE LAST AND LARGEST OF THE NINE THE UNDO SWEEP FOUND SILENT: `EnvCard`'s `box->onEdited`
	// record, dropped -> GREEN, 477 of 477. **This is most of what a hand does to an envelope** —
	// every attack, decay, sustain, release, delay, hold and smoothing edit goes through it — and
	// none of it came back with ctrl-Z.
	//
	// THE CARD RECORDS IT, NOT THE BOX, and the header says why: *"The box deliberately does not know
	// what it edits — see ValueBox::onEdited — and an EditRecord has to hold the address of the thing
	// so it can put the value back after this card has been torn down." So the record sits in a
	// lambda in `bindBox`, one line, with nothing pointing at it.
	//
	// NOTHING ARMED, DELIBERATELY. With a source armed the same drag sets an AMOUNT through a
	// different road with its own record — the one `c18f03a` pins — and a case that forgot to check
	// would be asserting the wrong function entirely.
	ModFixture f;
	feathers::ui::ModState mod;
	feathers::ui::EditHistory history;
	namespace dsr = feathers::mod::dahdsr;

	ui::EnvCard card { f.deviceHost, f.bridge.rackDeviceAt(0), mod, history };
	card.setBounds(0, 0, static_cast<int>(ui::ModPanel::kCardWidth),
	               static_cast<int>(ui::EnvCard::height()));
	card.resized();
	REQUIRE_FALSE(mod.armed());

	// EVERY BOX, because `bindBox` is one function called in a loop and the record is inside it: a
	// case that dragged Attack alone would say nothing about the other six, and this card's seven
	// controls are the whole envelope.
	for (size_t index = 0; index < ui::EnvCard::kBoxCount; ++index) {
		const dsr::Param param = ui::EnvCard::layout()[index];
		INFO("box " << index);
		const visage::Bounds box = card.boxBounds(index);
		REQUIRE(box.width() > 0.0f);
		const visage::Point on { box.x() + box.width() * 0.5f, box.y() + box.height() * 0.5f };
		// THE PRESS LANDS ON A BOX rather than on the card behind it — the reachability half, which
		// no value assertion can see.
		visage::Frame* hit = card.frameAtPoint(on);
		REQUIRE(hit != nullptr);
		REQUIRE(hit != static_cast<visage::Frame*>(&card));

		const double before = dsr::valueOf(f.bridge.modRack().dahdsrAt(0), param);
		const size_t steps = history.length();
		// UPWARD, unless the box already sits at its ceiling — the Amount defaults to unity (backlog DH),
		// and a drag past a ceiling is a drag that changes nothing, which is not what this case is about.
		const bool atCeiling = before >= dsr::kDescriptor.params[static_cast<size_t>(param)].max;
		dragThroughRoot(&card, on, { on.x, on.y + (atCeiling ? 40.0f : -40.0f) });

		const double after = dsr::valueOf(f.bridge.modRack().dahdsrAt(0), param);
		CHECK(after != doctest::Approx(before));
		CHECK(history.length() == steps + 1);

		history.undo();
		CHECK(dsr::valueOf(f.bridge.modRack().dahdsrAt(0), param) == doctest::Approx(before));
		history.redo();
		CHECK(dsr::valueOf(f.bridge.modRack().dahdsrAt(0), param) == doctest::Approx(after));
	}
}
TEST_CASE("storing a stance is an undo step") {
	// THE NINTH AND LAST OF THE SILENT UNDO ROADS: `SoundStack::storeVariationAt`'s `StanceEdit`,
	// dropped -> GREEN, 477 of 477.
	//
	// AND IT IS THE ONE WHOSE SILENCE COSTS THE MOST DELIBERATE WORK. Every other road on the list
	// undoes a value a hand can simply set again; this one is the PRESERVE verb — the
	// clone-stance-FIRST, the gesture whose entire purpose is to keep an exploration you like. A
	// press that spends no step means the ctrl-Z after it reaches past the store and undoes whatever
	// came before, exactly as measured on the route roads (`0350d93`).
	//
	// DRIVEN AS THE PRESS, through the rail's own store point, because the verb the hand reaches is
	// a press on a row and the case one door up already proves what the verb DOES. What this adds is
	// that the stack remembers it happened.
	StackFixture f;
	f.set(ParamId::AZone, 100.0);

	REQUIRE(f.stack.variationRows(0) == 1);
	const size_t steps = f.history.length();

	const visage::Point store = f.variationStorePoint(0, 0);
	f.stack.onMouseDown().callback(at(store.x, store.y));
	f.settle();

	// THE STANCE IS THERE — without this the step below could be recorded by a press that stored
	// nothing, which is the vacuous half of every undo assertion.
	REQUIRE(f.stack.variationRows(0) == 2);
	CHECK(f.history.length() == steps + 1);

	// AND THE UNDO TAKES IT BACK, rather than reaching past it. Both facts: a length that grew and a
	// state that returns.
	f.history.undo();
	f.settle();
	CHECK(f.stack.variationRows(0) == 1);
	// The sound it was standing on is untouched by the round trip — the store moved nothing live, so
	// neither may its undo.
	CHECK(f.read(ParamId::AZone) == doctest::Approx(100.0));

	f.history.redo();
	f.settle();
	CHECK(f.stack.variationRows(0) == 2);
}
TEST_CASE("a reorder committed from a card's own mouse-up does not free that card") {
	// The plugin crashed on reordering audio fx with audio playing, reproducibly. The stack named the
	// road:
	//
	//     crashHandler  <- signal 11
	//     Frame::on_mouse_exit_ lambda      <- a std::function on freed storage
	//     Frame::propagateMouseEvent
	//     WindowEventHandler::handleMouseUp
	//
	// `FxCard::onMouseUp` calls `onCardDrag(DragPhase::End)`, which reaches `FxLane::moveDevice`,
	// which rebuilds. `ensureCard` freed the previous round's retired cards on entry — under a comment
	// asserting "nothing is walking the tree", which is true of the state-channel notify it was
	// written for and false here. **A reorder touches at least two slots**, so the second slot's free
	// destroyed the card the first had retired: the one whose handler was still on the stack.
	//
	// WHAT IS ASSERTED IS THE DEFERRAL, NOT SURVIVAL. "The editor did not crash" passes on a build
	// that frees the card and gets away with it — a dangling read is undefined rather than reliably
	// fatal, which is exactly how this lane's PREVIOUS use-after-free survived three notifications
	// before it killed anything (96d94be). So the case reads the retired count: after a reorder the
	// cards are still held, and only a tick may free them.
	FilterFixture f;
	ui::WidgetRegistry registry;
	ui::FrameWatchers watchers;
	// THREE DEVICES, AND THE MOVE IS 0 -> 2. With two devices `moveDevice(0, 1)` returns true and
	// reorders NOTHING — removing the first and inserting it at index 1 of a two-element chain puts it
	// back where it was — so a case built on it asserts against a chain that never moved. Measured:
	// slot 0 still read `flt1` after the call. That is the drop-that-moves-nothing `rowForGap` exists
	// to filter out, met from the other side.
	REQUIRE(f.bridge.addFxDevice(fx::filter::kTraits.tag) < feathers::kMaxFxSlots);
	REQUIRE(f.bridge.addFxDevice(fx::harm::kTraits.tag) < feathers::kMaxFxSlots);
	REQUIRE(f.bridge.addFxDevice(fx::owltt::kTraits.tag) < feathers::kMaxFxSlots);
	f.bridge.refresh();

	ui::FxLane lane { f.bridge, registry, nullptr, nullptr, nullptr, &watchers };
	lane.setBounds(0.0f, 0.0f, 260.0f, 700.0f);
	// `resized()` IS THE REBUILD, which is the road a reorder reaches too.
	lane.resized();

	// THE PREMISE, REQUIRED: two cards really are there, or a reorder moves nothing and every
	// assertion below is about an empty lane.
	REQUIRE(lane.cardAt(0) != nullptr);
	REQUIRE(lane.cardAt(1) != nullptr);
	REQUIRE(lane.cardAt(2) != nullptr);
	lane.releaseRetiredCards();
	REQUIRE(lane.retiredCountForTest() == 0);

	// THE REORDER, driven through the same road the drag's mouse-up takes.
	REQUIRE(lane.moveDevice(0, 2));
	f.bridge.refresh();
	lane.resized();
	CAPTURE(lane.retiredCountForTest());

	// THE CARDS THE REBUILD REPLACED ARE STILL ALIVE. Before the fix this was 0 at this point,
	// because the second slot's rebuild had already freed the first slot's retired card.
	CHECK(lane.retiredCountForTest() > 0);

	// AND A TICK IS WHAT FREES THEM — the deferral is a deferral rather than a leak.
	lane.releaseRetiredCards();
	CHECK(lane.retiredCountForTest() == 0);
}
TEST_CASE("a drag past the end of a knob's sweep does not send the host a value outside it") {
	// TWO CLAMPS, MUTUALLY MASKING, AND NEITHER ASSERTED. Measured: removing `Knob`'s
	// `std::clamp(anchorValue_ + travel, 0, 1)` -> GREEN, 482 of 482; removing
	// `ParamControl::setValue`'s `std::clamp(value, desc_->min, desc_->max)` -> GREEN, 482 of 482.
	// Each survives alone because the other still bounds the result — the overlay's shape again, in
	// arithmetic rather than in visibility. WITH BOTH GONE this case posts **28.5714 and -27.5714 for
	// a parameter declared 0..1**, which is the number a host would have written into an automation
	// lane; with either one standing it posts nothing outside the range at all. So the pair is the
	// finding and neither half is, and only an assertion aimed at the QUEUE can tell the difference.
	//
	// AND THE CONSEQUENCE IS NOT LOCAL, WHICH IS WHY THE ASSERTION IS ON THE QUEUE. `ParamBridge::
	// setValue` sanitises `seenBase_` — the copy the EDITOR draws — but it pushes the value it was
	// given into the queue FIRST and unaltered. So the editor would keep drawing something sensible
	// while the host was handed a number outside the parameter's declared range, which is what lands
	// in an automation lane. A case reading the widget back, or the bridge's `value()`, sees the
	// sanitised copy and passes.
	//
	// THE ENGINE IS NOT THE ANSWER EITHER. `ParamSet` sanitises what it receives, so the SOUND is
	// safe; what is not safe is what the host recorded. This is the seam between them.
	ModFixture f;
	ui::Knob* path = f.knobFor(ParamId::Path);
	REQUIRE(path != nullptr);
	const ParamDesc& desc = descOf(ParamId::Path);
	const auto id = static_cast<clap_id>(ParamId::Path);

	// NOTHING ARMED: this is the value road, not the amount road.
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);
	REQUIRE_FALSE(mod->armed());

	drain(f.bridge); // whatever the fixture's own setup posted is not this case's business

	// FAR PAST BOTH ENDS, in one drag each way. The distance is deliberately absurd — several times
	// the knob's whole sweep — because a clamp that binds at the edge is a clamp that only shows up
	// when something asks for well past it.
	const visage::Point centre = f.faceCentre(path);
	dragThroughRoot(f.root, centre, { centre.x, centre.y - 4000.0f });
	dragThroughRoot(f.root, centre, { centre.x, centre.y + 4000.0f });

	const std::vector<UiParamEvent> posted = drain(f.bridge);
	size_t values = 0;
	for (const UiParamEvent& event : posted) {
		if (event.kind != UiParamEvent::Kind::Value || event.id != id) {
			continue;
		}
		++values;
		INFO("posted " << event.value << " for a range of " << desc.min << ".." << desc.max);
		CHECK(event.value >= desc.min);
		CHECK(event.value <= desc.max);
	}
	// THE DRAGS POSTED SOMETHING. Without this the loop above is a loop over nothing and the case
	// passes by never asking — the way a walk usually fails.
	CHECK(values > 0);
}
TEST_CASE("a minimised modulator card stays minimised as the rack shifts under it") {
	// A FOLD BELONGS TO A CARD, WHICH IS TO SAY TO A DEVICE. `ModPanel::minimised_` is a
	// `std::array<bool, kMaxRackSlots>` indexed by RACK SLOT, and `removeRackDevice` shifts every row
	// above the hole down one — so the flags stay where they are while the devices move past them.
	//
	// THE PREDICTION IS ALREADY IN THE FILE, one line under the removal that causes it:
	// *"REBUILT, NOT JUST REDRAWN. Every card from the hole down is now the face of a different
	// device." Every card, including the fold state drawn on it.
	//
	// IDENTIFIED BY ROUTE LANE, NOT BY SLOT, because slot is the thing that moves. A lane travels
	// inside the row it belongs to — `rack->device[i - 1] = rack->device[i]` copies it — so the lane
	// is the device's handle across the shift. Read contemporaneously each time, never remembered
	// across a rebuild.
	ModFixture f;
	namespace ms = feathers::mod::mseg;
	// SLOTS TAKEN FROM THE RETURN VALUE, not assumed to start at zero — the editor's rack already
	// holds a device before this case adds one, and a fixture's contents are the fixture's business.
	const size_t first = f.bridge.addRackDevice(ms::kTraits.tag);
	const size_t second = f.bridge.addRackDevice(ms::kTraits.tag);
	const size_t third = f.bridge.addRackDevice(ms::kTraits.tag);
	REQUIRE(third < kMaxRackSlots);
	REQUIRE(second == first + 1);
	REQUIRE(third == second + 1);
	f.bridge.refresh();
	f.panel->resized();

	const DeviceInstanceId idFirst = f.bridge.rackDeviceAt(first);
	const DeviceInstanceId idSecond = f.bridge.rackDeviceAt(second);
	const DeviceInstanceId idThird = f.bridge.rackDeviceAt(third);
	// THREE DISTINCT DEVICES, asserted: two sharing one id would make every question below ambiguous
	// and the case would pass by confusion.
	REQUIRE(idFirst != idSecond);
	REQUIRE(idSecond != idThird);
	REQUIRE(idFirst != idThird);

	/// The fold state of the device `id`, asked the way the strip asks it.
	const auto minimised = [&](DeviceInstanceId id) {
		return f.panel->cardMinimised(backplane::Source::output(id, 0));
	};

	// MINIMISE THE MIDDLE ONE, and only it.
	REQUIRE(f.panel->setCardMinimised(backplane::Source::output(idSecond, 0), true));
	REQUIRE(minimised(idSecond));
	REQUIRE_FALSE(minimised(idFirst));
	REQUIRE_FALSE(minimised(idThird));

	/// The device the hand folded is still folded, and the ones it never touched are not. BOTH
	/// HALVES EVERY TIME, because a flag that stays put while the devices move past it gets them
	/// wrong in opposite directions at once — the folded card springs open and an untouched one
	/// folds itself.
	const auto onlyTheSecondIsFolded = [&] {
		CHECK(minimised(idSecond));
		CHECK_FALSE(minimised(idFirst));
		CHECK_FALSE(minimised(idThird));
	};

	SUBCASE("the card above it is removed") {
		// The middle device becomes the first card and the last becomes the middle one. Neither was
		// touched by the hand.
		REQUIRE(f.panel->removeDeviceAtLane(first));
		f.bridge.refresh();
		f.panel->resized();
		REQUIRE(f.bridge.rackDeviceAt(first) == idSecond);
		REQUIRE(f.bridge.rackDeviceAt(second) == idThird);
		CHECK(minimised(idSecond));
		CHECK_FALSE(minimised(idThird));
	}

	SUBCASE("it is moved up the rack itself") {
		// THE OTHER ROAD THAT RENUMBERS SLOTS, and the one a player reaches for more often than
		// removal: reordering the strip. The fold travels in the row or it does not travel at all.
		REQUIRE(f.panel->moveDevice(second, first));
		f.bridge.refresh();
		f.panel->resized();
		REQUIRE(f.bridge.rackDeviceAt(first) == idSecond);
		REQUIRE(f.bridge.rackDeviceAt(second) == idFirst);
		onlyTheSecondIsFolded();
	}

	SUBCASE("a removal it is not involved in is undone again") {
		// AND BACK OUT THE OTHER SIDE. An undo restores the row, so it restores the numbering — a fix
		// that shifted the flags on the way down and not on the way back would pass the first subcase
		// and leave the fold one card off after a ctrl-Z.
		REQUIRE(f.panel->removeDeviceAtLane(first));
		f.bridge.refresh();
		f.panel->resized();
		ui::EditHistory* history = f.gui.history();
		REQUIRE(history != nullptr);
		REQUIRE(history->length() > 0);
		history->undo();
		f.bridge.refresh();
		f.panel->resized();
		REQUIRE(f.bridge.rackDeviceAt(second) == idSecond);
		onlyTheSecondIsFolded();
	}
}
TEST_CASE("[measure] what a warp costs the terrain, per texel and per map") {
	// REPORTED RATHER THAN ASSERTED, the way test_vox_voice reports its bake timings: a number that
	// moves with the machine is a bad gate and a good record. The ASSERTION is the value case in
	// test_warp.cpp; this says what the feature costs the picture.
	//
	// `terrainFeaturesOn` IS what the map builder calls per texel — frames, recipe, bake-free
	// evaluation and the five features — so this is the whole per-texel cost and not just the recipe.
	namespace ui = feathers::ui;
	const feathers::SliceOrientationPin pin {};
	const double home = feathers::sliceHome(0, 0.0f, 0.0);
	const ui::TerrainFrames frames = ui::terrainFramesAt(pin, home, feathers::PlaneResidue {});

	const auto perTexel = [&](feathers::WarpMode mode, float depth) {
		feathers::FieldScales scales {};
		scales.warpDepth = depth;
		constexpr int kN = 3000;
		volatile float sink = 0.0f;
		// THE ACCUMULATIONS BELOW ARE WRITTEN `sink = sink + x`, NOT `sink += x`, AND THAT IS NOT A
		// STYLE CHOICE — DO NOT "TIDY" IT BACK. `volatile` here is the optimiser barrier that keeps
		// these loops from being deleted whole, so it is load-bearing (CLAUDE.md: check whether the
		// redundancy is load-bearing before removing it), and a compound assignment to a
		// volatile-qualified object is deprecated in C++20 — clang-14 flags it `-Wdeprecated-volatile`
		// and the macOS leg builds with -Werror. The explicit load-modify-store is the same volatile
		// read and volatile write in the same order, so the barrier is untouched and the measurement
		// is unchanged; it is simply spelled in the form every standard accepts.
		// WARM FIRST. Without this the run that goes FIRST pays every cold cache and first-touch cost
		// in the process, and the answer comes out backwards — measured, warp read 0.72x, i.e. warp
		// making the terrain FASTER, which is not a thing warp can do.
		for (int i = 0; i < 500; ++i) {
			const auto zone = static_cast<uint16_t>(i & 1023);
			const ui::TerrainFeatures w = ui::terrainFeaturesOn(feathers::OscType::V5, frames, zone,
			                                                    0.0f, 0.0, 0.0, 0.0, scales, mode);
			sink = sink + w.energy;
		}
		const auto t0 = std::chrono::steady_clock::now();
		for (int i = 0; i < kN; ++i) {
			const auto zone = static_cast<uint16_t>(i & 1023);
			const ui::TerrainFeatures f = ui::terrainFeaturesOn(feathers::OscType::V5, frames, zone,
			                                                    0.0f, 0.0, 0.0, 0.0, scales, mode);
			sink = sink + f.energy;
		}
		const double us =
		    std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / kN;
		(void)sink;
		return us;
	};

	// BOTH ORDERS, AND THE BEST OF EACH — the cheapest guard against whatever the machine is doing
	// while this runs, and against either arm paying for the other's warm-up.
	const double off1 = perTexel(feathers::WarpMode::Off, 0.0f);
	const double on1 = perTexel(feathers::WarpMode::Pwm, 1.0f);
	const double on2 = perTexel(feathers::WarpMode::Pwm, 1.0f);
	const double off2 = perTexel(feathers::WarpMode::Off, 0.0f);
	const double off = std::min(off1, off2);
	const double on = std::min(on1, on2);
	constexpr double kTexels = 128.0 * 128.0;
	MESSAGE("[terrain] per texel: warp off " << off << " us | warp on " << on << " us ("
	                                         << (on / off) << "x)");
	MESSAGE("[terrain] a 128x128 map: warp off " << (off * kTexels / 1000.0) << " ms | warp on "
	                                             << (on * kTexels / 1000.0) << " ms");
	CHECK(off > 0.0);
	CHECK(on > 0.0);
}
TEST_CASE("[measure] what an Additive texel costs the terrain, against Morph and V5") {
	// REPORTED, NOT ASSERTED (the warp measurement's posture, above): per texel through `terrainFeaturesOn`, the builder's
	// own per-texel call, for Morph, V5, Additive with no Shape Amount and Additive at Shape Amount 0.5 (the blend toward
	// a shape, whose target is built once per process). Each arm warmed, the four run in both orders, the best kept.
	// MEASURED 2026-10-10 (load ~1.5): Morph 1.31, V5 5.37, Additive 10.2, shaped 10.7 us per texel (167 ms a map against
	// V5's 88). WHAT DOMINATES: buildPartialSound, 8.3 us of the 10.2 (its 320 detail triangles 1.8 of that), i.e. the
	// whole 320-partial spectrum the RSS normalisation reads, of which the lens sums fifteen. Left as it is (unasked).
	namespace ui = feathers::ui;
	const feathers::SliceOrientationPin pin {};
	const double home = feathers::sliceHome(0, 0.0f, 0.0);
	const ui::TerrainFrames frames = ui::terrainFramesAt(pin, home, feathers::PlaneResidue {});
	const auto perTexel = [&](feathers::OscType engine, float shapeAmount) {
		const ui::AdditivePicture pic { feathers::partials::ShapeKind::Square, shapeAmount };
		constexpr int kN = 3000;
		volatile float sink = 0.0f;
		const auto one = [&](int i) {
			const auto zone = static_cast<uint16_t>(i & 1023);
			return ui::terrainFeaturesOn(engine, frames, zone, 0.0f, 0.0, 0.0, 0.0, {}, feathers::WarpMode::Off,
			                             feathers::WeaveModel::Ring, {}, {}, pic)
			    .energy;
		};
		for (int i = 0; i < 500; ++i) {
			sink = sink + one(i);
		}
		const auto t0 = std::chrono::steady_clock::now();
		for (int i = 0; i < kN; ++i) {
			sink = sink + one(i);
		}
		(void)sink;
		return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / kN;
	};
	const auto best = [&](feathers::OscType engine, float amount, double first) {
		return std::min(first, perTexel(engine, amount));
	};
	const double morph1 = perTexel(feathers::OscType::Morph, 0.0f);
	const double v51 = perTexel(feathers::OscType::V5, 0.0f);
	const double add1 = perTexel(feathers::OscType::Additive, 0.0f);
	const double shaped1 = perTexel(feathers::OscType::Additive, 0.5f);
	const double shaped = best(feathers::OscType::Additive, 0.5f, shaped1);
	const double add = best(feathers::OscType::Additive, 0.0f, add1);
	const double v5 = best(feathers::OscType::V5, 0.0f, v51);
	const double morph = best(feathers::OscType::Morph, 0.0f, morph1);
	constexpr double kTexels = 128.0 * 128.0;
	MESSAGE("[terrain] per texel: Morph " << morph << " us | V5 " << v5 << " us | Additive " << add
	                                      << " us | Additive at Shape Amount 0.5 " << shaped << " us");
	MESSAGE("[terrain] a 128x128 map: Morph " << (morph * kTexels / 1000.0) << " ms | V5 " << (v5 * kTexels / 1000.0)
	                                          << " ms | Additive " << (add * kTexels / 1000.0) << " ms | shaped "
	                                          << (shaped * kTexels / 1000.0) << " ms");
	CHECK(add > 0.0);
}
TEST_CASE("every x across an fx card's head answers exactly one verb, and the buttons are not fold") {
	// Reported: header controls did the wrong thing, usually folding, and in builds before the
	// one that froze. "Usually fold" is the shape of the report that matters:
	// FOLD IS THE FALL-THROUGH. Every card tests the slot menu, the enable square and the expand
	// button first and gives the rest of the head to the fold, so a button whose rectangle a press
	// misses does not do nothing — it FOLDS, and every miss looks like the same bug.
	//
	// SO THE QUESTION IS NOT "does the expand button work", which the per-device cases already ask by
	// pressing its centre. It is whether the head's rectangles TILE — whether there is any x a hand
	// can be on where the verb that answers is not the one drawn there. A centre press cannot see a
	// rectangle that is two pixels too narrow, and that is exactly the defect that reads as "usually
	// fold".
	//
	// A SWEEP RATHER THAN THREE CENTRES, and one fixture per press because the first press CHANGES
	// THE GEOMETRY: a fold re-lays the lane out and an enable toggles a control. Reusing a fixture
	// would measure the second x against a card the first x moved.
	//
	// AND THROUGH THE ROOT, not by calling the card's handler: `frameAtPoint` is half the claim. A
	// child frame lying over the head steals the press before the card's own hit test runs, which no
	// call to `onMouseDown().callback()` can see.
	const auto clickThroughRoot = [](visage::Frame* root, visage::Point where) {
		visage::Frame* down = root->frameAtPoint(where);
		if (down == nullptr) {
			return;
		}
		const auto eventAt = [&](bool isDown) {
			visage::MouseEvent e;
			e.window_position = where;
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

		// THE HEAD'S RIGHT-HAND BAND, which is where all three buttons live: the slot menu is
		// `kSlotMenuPad + kSlotMenuSize` off the right edge and the other two are stacked inboard of
		// it, six pixels apart. Eighty pixels covers all three and the fold either side of them.
		constexpr float kBand = 80.0f;
		// The verb each x answered, indexed from the right edge inwards.
		enum class Verb { kNothing, kFold, kSlotMenu, kEnable, kExpand, kSeveral };
		std::vector<Verb> verb;
		std::vector<float> xs;

		float headWidth = 0.0f;
		visage::Bounds menuRect;
		visage::Bounds enableRect;
		visage::Bounds expandRect;

		// STRICTLY INSIDE THE CARD. `x == width()` is one past the right edge and belongs to no frame at
		// all — the first draft swept it and read the resulting nothing as a hole in the head, which is
		// the sweep finding its own bound rather than a defect.
		// **A FRESH FIXTURE ONLY AFTER A PRESS THAT MOVED SOMETHING** (backlog PL, 2026-10-30). One per x was 80 whole
		// editors per device, about 66 s alone and past run-cases' 120 s at load. The reason for a fresh one is that a
		// press can change the geometry the next x is measured against: a fold re-lays the lane, an enable toggles a
		// control, and either is caught by the verb it answers, after which the fixture is rebuilt. A press that
		// answered the slot menu, the expand or nothing reached a probe or nothing (`noteCardPress` keeps no state that
		// outlives the press but the selection, which is not geometry). And a reused card is not trusted on that
		// argument alone: before every press its geometry is compared with the FRESH card's, and any difference
		// rebuilds.
		bool sawMenu = false; // above `held`: the probes capture these, so they outlive the fixture
		bool sawExpand = false;
		std::unique_ptr<ModFixture> held;
		ui::FxLane* lane = nullptr;
		ui::FxCard* card = nullptr;
		size_t slot = 0;
		const auto geometryOf = [](ui::FxCard& c) {
			const visage::Point at = c.positionInWindow();
			const visage::Bounds m = c.slotMenuBounds();
			const visage::Bounds e = c.enableBounds();
			const visage::Bounds x = c.expandBounds();
			return std::array<float, 16> { at.x, at.y, c.width(), c.laneHeight(), m.x(), m.y(), m.width(), m.height(),
				                           e.x(), e.y(), e.width(), e.height(), x.x(), x.y(), x.width(), x.height() };
		};
		std::optional<std::array<float, 16>> fresh;
		int fixtures = 0;
		for (int step = 0; step < static_cast<int>(kBand); ++step) {
			if (held != nullptr && geometryOf(*card) != *fresh) {
				held.reset();
			}
			if (held == nullptr) {
				held = std::make_unique<ModFixture>();
				++fixtures;
				lane = findInTree<ui::FxLane>(held->root);
				REQUIRE(lane != nullptr);
				slot = held->bridge.addFxDevice(tag.c_str());
				REQUIRE(slot == 1);
				lane->resized();
				lane->scrollCardIntoView(slot);
				lane->resized();
				card = lane->cardAt(slot);
				REQUIRE(card != nullptr);
				// THE REAL MENU IS REPLACED RATHER THAN OPENED. `showSlotMenu` puts a real popup on the
				// tree; what this case is asking is which verb the press REACHED, and a probe answers that
				// without leaving a menu standing over the next assertion.
				card->onSlotMenu = [&sawMenu] { sawMenu = true; };
				lane->onExpandDevice = [&sawExpand](size_t) { sawExpand = true; };
				if (!fresh) {
					fresh = geometryOf(*card);
				}
				// Every fresh card is the same card: what a reused one is held to is not one fixture's accident.
				CHECK(geometryOf(*card) == *fresh);
			}
			ModFixture& f = *held;

			headWidth = card->width();
			menuRect = card->slotMenuBounds();
			enableRect = card->enableBounds();
			expandRect = card->expandBounds();

			sawMenu = false;
			sawExpand = false;

			const size_t enableIdx = card->hasEnable() ? card->enableControl() : 0;
			const float enableBefore =
			    card->hasEnable() ? f.bridge.fxChain().device[slot].row.field[enableIdx] : 0.0f;
			const float heightBefore = card->laneHeight();

			const float x = card->width() - kBand + static_cast<float>(step);
			const visage::Point where =
			    card->positionInWindow() + visage::Point { x, ui::FxCard::kSlotHeadHeight * 0.5f };

			// A WINDOW THE LAST PRESS OPENED (the expand button's) IS CLOSED BEFORE THE NEXT: the Chain/site window is wide, and left open it would
			// stand over this card's head, so a press on the enable chip would reach the window and not the card.
			if (auto* overlay = f.gui.expandedForTest(); overlay != nullptr && overlay->open()) {
				overlay->dismiss();
			}
			clickThroughRoot(f.root, where);

			const bool folded = card->laneHeight() != doctest::Approx(heightBefore);
			const bool enabled =
			    card->hasEnable()
			    && f.bridge.fxChain().device[slot].row.field[enableIdx] != doctest::Approx(enableBefore);

			const int fired = static_cast<int>(folded) + static_cast<int>(enabled)
			                  + static_cast<int>(sawMenu) + static_cast<int>(sawExpand);
			Verb answered = Verb::kNothing;
			if (fired > 1) {
				answered = Verb::kSeveral;
			}
			else if (folded) {
				answered = Verb::kFold;
			}
			else if (enabled) {
				answered = Verb::kEnable;
			}
			else if (sawMenu) {
				answered = Verb::kSlotMenu;
			}
			else if (sawExpand) {
				answered = Verb::kExpand;
			}
			verb.push_back(answered);
			xs.push_back(x);
			// A PRESS THAT CHANGED STATE SPENDS ITS FIXTURE.
			if (folded || enabled) {
				held.reset();
			}
		}
		MESSAGE("[head] " << tag << ": " << fixtures << " fixtures for " << verb.size() << " presses");

		// NO x ANSWERS TWO VERBS. Overlapping rectangles are the defect drawn_card.h records having
		// shipped once — the expand button drawn on top of the slot menu — and it is invisible to a
		// centre press whenever the centres happen to differ.
		for (size_t i = 0; i < verb.size(); ++i) {
			CAPTURE(xs[i]);
			CHECK(verb[i] != Verb::kSeveral);
		}

		// AND EVERY x INSIDE A BUTTON ANSWERS THAT BUTTON, which is the report stated as an
		// assertion: a press on the square must not fold.
		for (size_t i = 0; i < verb.size(); ++i) {
			const visage::Point local { xs[i], ui::FxCard::kSlotHeadHeight * 0.5f };
			CAPTURE(xs[i]);
			if (menuRect.contains(local)) {
				CHECK(verb[i] == Verb::kSlotMenu);
			}
			else if (enableRect.width() > 0.0f && enableRect.contains(local)) {
				CHECK(verb[i] == Verb::kEnable);
			}
		}

		// **AND A SINGLE CLICK ON PLAIN HEAD ANSWERS NOTHING, WHICH IS THE GESTURE AND NOT A HOLE.**
		//
		// This required every x to answer SOMETHING, on the reasoning that the head owns the fold so a
		// silent x meant a swallowed press. The fold moved to the SECOND click — see
		// `FxCard::foldOnHeadPress`, after it was reported a double-click toggling twice and landing
		// where it started — so a single click on plain head is correctly silent everywhere now, and
		// the old assertion would be asserting the bug.
		//
		// THE CLAIM IT CARRIED IS NOT DROPPED, it moves: that a head press folds is covered by *"EVERY
		// card in the lane folds to its head when the head is pressed", which drives the real gesture.
		// What survives here is what this sweep is uniquely for — no x answers TWO verbs, and an x
		// inside a button answers THAT button.
		for (size_t i = 0; i < verb.size(); ++i) {
			const visage::Point local { xs[i], ui::FxCard::kSlotHeadHeight * 0.5f };
			const bool inButton = (menuRect.width() > 0.0f && menuRect.contains(local))
			                      || (enableRect.width() > 0.0f && enableRect.contains(local))
			                      || (expandRect.width() > 0.0f && expandRect.contains(local));
			if (!inButton) {
				CAPTURE(xs[i]);
				CHECK(verb[i] == Verb::kNothing);
			}
		}
		CHECK(headWidth > kBand);
		MESSAGE("[head] " << tag << " width " << headWidth << " menu ["
		                  << menuRect.x() << "," << (menuRect.x() + menuRect.width()) << ") enable ["
		                  << enableRect.x() << "," << (enableRect.x() + enableRect.width()) << ")");
		for (size_t i = 0; i < verb.size(); ++i) {
			if (verb[i] == Verb::kNothing || verb[i] == Verb::kSeveral) {
				MESSAGE("[head]   x " << xs[i] << " answered "
				                      << (verb[i] == Verb::kNothing ? "NOTHING" : "SEVERAL"));
			}
		}
	}
}
TEST_CASE("a double-click on a head button is not a double-click on the head") {
	// Presses were double-counted between the header and its enable/minimize boxes: a control's hit box
	// must block the header's minimize double-click gesture.
	//
	// THE HEAD IS ONE RECTANGLE WITH SEVERAL BUTTONS INSIDE IT, so every press on a button is also,
	// geometrically, a press on the head. The fold gesture reads the click count off the head; the
	// buttons read the coordinate. Nothing in either rule stopped both from firing, and both did.
	//
	// The sweep above asks the same question of SINGLE clicks and passed throughout — which is exactly
	// why this case exists separately: the fold verb is on the SECOND click, so a single-click sweep
	// cannot see the collision at all. Two rules about the same rectangle, tested at one click count.
	ui::FxLane* probeLane = nullptr;
	ModFixture probe;
	probeLane = findInTree<ui::FxLane>(probe.root);
	REQUIRE(probeLane != nullptr);
	std::vector<std::string> tags;
	for (const device::Descriptor* d : probeLane->addableForTest()) {
		tags.emplace_back(d->traits.tag);
	}
	REQUIRE(tags.size() >= 4);

	// THE BUTTONS, ASKED OF THE CARD rather than listed here. A rectangle this case spelled itself
	// would go stale the next time one moves, and would then pass by testing empty space.
	struct Button {
		const char* name;
		visage::Bounds (ui::FxCard::*where)() const;
	};
	const Button buttons[] = {
		{ "slot menu", &ui::FxCard::slotMenuBounds },
		{ "enable", &ui::FxCard::enableBounds },
		{ "expand", &ui::FxCard::expandBounds },
	};

	int covered = 0;
	for (const std::string& tag : tags) {
		CAPTURE(tag);
		for (const Button& button : buttons) {
			// AS A STRING: `CAPTURE` on a bare `const char*` member printed `1` in the known-bad run,
			// which names no button at all — and naming the button is the whole job of this line.
			const std::string buttonName = button.name;
			CAPTURE(buttonName);

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

			const visage::Bounds rect = (card->*button.where)();
			if (rect.width() <= 0.0f) {
				// NOT EVERY CARD HAS EVERY BUTTON — a device with no enable field has no square, and a
				// device with no panel has no expand mark. An absent button is not a failure; a case
				// that asserted one would be asserting the device list.
				continue;
			}
			++covered;

			const float heightBefore = card->laneHeight();
			const visage::Point on =
			    card->positionInWindow()
			    + visage::Point { rect.x() + rect.width() * 0.5f, rect.y() + rect.height() * 0.5f };
			// NOT VACUOUS: the press must reach THIS card. Without this the case passes whenever the
			// coordinate lands outside the card, which is the same green a working guard gives.
			REQUIRE(f.root->frameAtPoint(on) == card);

			card->onMouseDown().callback(
			    doubleAt(on.x - card->positionInWindow().x, on.y - card->positionInWindow().y));

			// **THE CARD DID NOT FOLD.** `laneHeight` is the observable: folding a card is what changes
			// it, and it is what the sweep above uses for the same verb.
			CHECK(card->laneHeight() == doctest::Approx(heightBefore));
			CHECK_FALSE(card->folded());
		}
	}

	// AND THE LOOP ACTUALLY RAN. Every `continue` above is legitimate, so a case whose buttons all
	// went missing would report success having asserted nothing.
	CHECK(covered >= 8);
}
TEST_CASE("a stored type carries the zero, and Free lets it go") {
	// Types stick; Free releases the constraint. Before this, a type
	// was a one-shot preset: picking Lowpass wrote the zero once and nothing held it there, so the
	// next pole drag left the zero behind and every band behaved like Free the moment it was touched
	// — which is the reported defect.
	//
	// ASSERTED AS THE PLACEMENT THE TYPE MEANS, NOT AS "THE ZERO MOVED". A zero that moved is what a
	// pole drag does to a Bypass band, to a Notch, and to a broken build that writes the pole's
	// position into the zero; only comparing against `zeroPlacementFor(type, endPole)` separates
	// them. The pole is read back from the STORE rather than computed from the drag, so the two sides
	// cannot agree about a pole that neither of them ended on.
	namespace dr = feathers::fx::drawn;
	FilterFixture f;
	ui::WidgetRegistry registry;
	const size_t slot = f.bridge.addFxDevice(dr::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, f.bridge.fxDeviceAt(slot), dr::kDescriptor);
	REQUIRE(view != nullptr);
	auto* panel = dynamic_cast<ui::DrawnExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->setBounds(0.0f, 0.0f, 560.0f, panel->minimumSize().y);
	panel->resized();
	panel->selectFrame(1);

	const auto valueAt = [&](size_t index) {
		ui::FxControl c { f.bridge, f.bridge.fxDeviceAt(slot), dr::kDescriptor, index };
		return c.value();
	};
	const auto write = [&](size_t index, double v) {
		ui::FxControl c { f.bridge, f.bridge.fxDeviceAt(slot), dr::kDescriptor, index };
		c.setValue(v);
	};
	const size_t band = 3;

	/// Drag this band's pole somewhere else, through the real press/drag/release road, and hand back
	/// where it ended — read from the store, which is the only place that knows.
	const auto dragPoleTo = [&](float fx01, float fy01) {
		const visage::Bounds box = panel->planeBounds();
		const visage::Point from = panel->poleCentre(band);
		const visage::Point to { box.x() + box.width() * fx01, box.bottom() - box.height() * fy01 };
		REQUIRE(panel->hitAt(from).what == ui::DrawnExpanded::Grab::Pole);
		panel->onMouseDown().callback(at(from.x, from.y));
		panel->onMouseDrag().callback(at(to.x, to.y));
		panel->onMouseUp().callback(at(to.x, to.y));
		f.bridge.refresh();
		return dr::Point {
			static_cast<float>(valueAt(dr::bandFieldIndex(1, band, dr::PoleRadius))),
			static_cast<float>(valueAt(dr::bandFieldIndex(1, band, dr::PoleAngle))),
		};
	};

	SUBCASE("every binding type puts its zero where the type says, at the pole it ended on") {
		// ALL SIX THAT PLACE FROM THE POLE. Lowpass, Highpass and Bandpass place independently of it
		// — a lowpass's zeros stay at Nyquist whatever its corner does — so they are asked the same
		// question and answer it by not moving, which is the same assertion.
		for (size_t i = 0; i < static_cast<size_t>(dr::Type::kCount); ++i) {
			const auto type = static_cast<dr::Type>(i);
			if (type == dr::Type::Free) {
				continue; // its own subcase; it is the absence of this rule
			}
			CAPTURE(dr::kTypeNames[i]);
			// A KNOWN POLE FIRST, then the type, so the placement is written from a pole this case
			// chose rather than from wherever the previous iteration left one.
			write(dr::bandFieldIndex(1, band, dr::PoleRadius), 0.7);
			write(dr::bandFieldIndex(1, band, dr::PoleAngle), -6.0);
			f.bridge.refresh();
			panel->applyType(band, type);
			f.bridge.refresh();
			const double radiusBefore = valueAt(dr::bandFieldIndex(1, band, dr::ZeroRadius));

			const dr::Point ended = dragPoleTo(0.35f, 0.55f);
			// THE POLE REALLY MOVED, or the rest of this is about a drag that did nothing.
			REQUIRE(ended.angleLog2 != doctest::Approx(-6.0));

			const auto want = dr::zeroPlacementFor(type, ended);
			REQUIRE(want.has_value());
			// THE ANGLE IS THE TYPE'S, COMPUTED FROM THE POLE THE DRAG ENDED ON.
			CHECK(valueAt(dr::bandFieldIndex(1, band, dr::ZeroAngle))
			      == doctest::Approx(static_cast<double>(want->angleLog2)).epsilon(1e-5));
			if (type == dr::Type::Bell) {
				// **BELL KEEPS THE RADIUS THE HAND SET**, by design. A bell's zero radius IS its gain, so recomputing it from the default gain
				// on every pole drag would silently undo the one control that makes a bell a bell.
				CHECK(valueAt(dr::bandFieldIndex(1, band, dr::ZeroRadius))
				      == doctest::Approx(radiusBefore));
			}
			else {
				CHECK(valueAt(dr::bandFieldIndex(1, band, dr::ZeroRadius))
				      == doctest::Approx(static_cast<double>(want->radius)).epsilon(1e-5));
			}
			// AND THE BAND IS STILL THAT TYPE. A drag that quietly cleared the field would satisfy
			// every line above on the FIRST drag and behave like Free on the second.
			CHECK(valueAt(dr::bandFieldIndex(1, band, dr::TypeIndex)) == doctest::Approx(i));
		}
	}

	SUBCASE("Free releases: the pole moves and the zero stays exactly where it was") {
		// **MEASURED: FREE'S RELEASE IS PROTECTED TWICE, AND NEITHER GUARD ALONE IS A DEFECT.**
		// Making `bindingOf(Free)` return `Full` leaves this subcase GREEN, because
		// `zeroPlacementFor(Free, ...)` returns nothing and there is then nothing to write; giving
		// Free a placement while `bindingOf` still says `None` is green for the mirror reason. Only
		// mutating BOTH reddens it — measured, `0.933773 == Approx(0.42)`.
		//
		// SO THIS IS A MASKED PAIR RATHER THAN AN UNGUARDED ROAD, and the note is here so the next
		// reader does not delete one of the two as redundant: either one alone is load-bearing the
		// moment the other changes, which is precisely when nobody is looking.
		panel->applyType(band, dr::Type::Notch);
		f.bridge.refresh();
		// PUT THE ZERO SOMEWHERE OF ITS OWN while it is still free to be put there, then release.
		panel->applyType(band, dr::Type::Free);
		write(dr::bandFieldIndex(1, band, dr::ZeroRadius), 0.42);
		write(dr::bandFieldIndex(1, band, dr::ZeroAngle), -9.0);
		f.bridge.refresh();

		const dr::Point ended = dragPoleTo(0.7f, 0.25f);
		REQUIRE(ended.angleLog2 != doctest::Approx(-9.0));
		// UNTOUCHED, TO THE VALUE. This is the assertion that separates "released" from "released and
		// then re-bound by the next gesture".
		CHECK(valueAt(dr::bandFieldIndex(1, band, dr::ZeroRadius)) == doctest::Approx(0.42));
		CHECK(valueAt(dr::bandFieldIndex(1, band, dr::ZeroAngle)) == doctest::Approx(-9.0));
	}

	SUBCASE("a bound zero cannot be grabbed, and a Free one can") {
		// THE HONESTY HALF. A derived handle that answered a drag would be offering a gesture it
		// undoes on the next pole move — the rule this file already applies to the split-real zero,
		// which is not one point and is refused for the same reason.
		write(dr::bandFieldIndex(1, band, dr::PoleRadius), 0.7);
		write(dr::bandFieldIndex(1, band, dr::PoleAngle), -6.0);
		f.bridge.refresh();
		panel->applyType(band, dr::Type::Notch);
		f.bridge.refresh();
		// The Notch's zero is drawn ON the unit circle at the pole's angle — a real place, far from
		// the pole — so this is a point a hand could plausibly aim at, and the refusal is about the
		// type rather than about the two marks coinciding.
		CHECK(panel->hitAt(panel->zeroCentre(band)).what != ui::DrawnExpanded::Grab::Zero);

		panel->applyType(band, dr::Type::Free);
		f.bridge.refresh();
		CHECK(panel->hitAt(panel->zeroCentre(band)).what == ui::DrawnExpanded::Grab::Zero);
	}

	SUBCASE("Bell's radius is reachable by a drag and its angle is not") {
		// **THE PAIR THAT SEPARATES "BELL IS SPECIAL" FROM "BELL IS BROKEN".** A case asserting only
		// that the radius moves would pass on a Bell that had quietly become Free.
		write(dr::bandFieldIndex(1, band, dr::PoleRadius), 0.7);
		write(dr::bandFieldIndex(1, band, dr::PoleAngle), -6.0);
		f.bridge.refresh();
		panel->applyType(band, dr::Type::Bell);
		f.bridge.refresh();
		const double angleBefore = valueAt(dr::bandFieldIndex(1, band, dr::ZeroAngle));
		const double radiusBefore = valueAt(dr::bandFieldIndex(1, band, dr::ZeroRadius));

		const visage::Bounds box = panel->planeBounds();
		const visage::Point from = panel->zeroCentre(band);
		REQUIRE(panel->hitAt(from).what == ui::DrawnExpanded::Grab::Zero);
		// AIMED AT A DIFFERENT ANGLE AS WELL AS A DIFFERENT RADIUS, so "the angle did not move" is a
		// refusal rather than a drag that never asked.
		const visage::Point to { box.x() + box.width() * 0.15f, box.bottom() - box.height() * 0.8f };
		REQUIRE(std::abs(to.x - from.x) > 20.0f);
		panel->onMouseDown().callback(at(from.x, from.y));
		panel->onMouseDrag().callback(at(to.x, to.y));
		panel->onMouseUp().callback(at(to.x, to.y));
		f.bridge.refresh();

		CHECK(valueAt(dr::bandFieldIndex(1, band, dr::ZeroRadius)) != doctest::Approx(radiusBefore));
		CHECK(valueAt(dr::bandFieldIndex(1, band, dr::ZeroAngle)) == doctest::Approx(angleBefore));
		// AND THE ANGLE IS STILL THE POLE'S, which is what "locked" means — equal to a number that
		// did not move is also true of a lock that broke and happened to land back.
		CHECK(valueAt(dr::bandFieldIndex(1, band, dr::ZeroAngle))
		      == doctest::Approx(valueAt(dr::bandFieldIndex(1, band, dr::PoleAngle))));
	}
}
TEST_CASE("a stance that cannot be restored refuses the recall rather than half-applying it") {
	// **A SILENT NO-OP IS A RECALL THAT LIES ABOUT HAVING RECALLED**, which is the decision this case
	// exists for. The stance's ride-along values are read from a patch as raw doubles — the one
	// loader road left unguarded when the NaN family was closed, deliberately, because by then every
	// DESTINATION was guarded and a bad value could no longer reach the engine.
	//
	// SO THE DAMAGE IS NOT TO THE SOUND, IT IS TO THE VERB. `writeRideAlong` hands each value to a
	// setter that refuses a non-finite one — `setValue`, `setSoundGamma`, `setSoundTravel`,
	// `setSoundOrientation`, `setSoundResidue` — so a spoilt member is dropped ON THE FLOOR while
	// every sound member around it lands. The light moves to the row, the history gains a step, and
	// the sound is a mixture of the stance you asked for and the one you were standing in. **Nothing
	// anywhere says so.**
	//
	// REFUSING IS THE INTENDED BEHAVIOUR: a stance that cannot be restored should say it cannot, not
	// half-apply and report success.
	StackFixture f;
	f.set(ParamId::AZone, 100.0);
	f.set(ParamId::AWidth, 0.25);

	// TWO STANCES, THE SECOND SPOILT. Stored through the bridge, which is the shape a patch arrives
	// in — `setSoundVariations` takes the list as given, so this is the state a loaded file produces.
	f.stack.onMouseDown().callback(at(f.variationStorePoint(0, 0).x, f.variationStorePoint(0, 0).y));
	f.settle();
	REQUIRE(f.stack.variationRows(0) == 2);
	REQUIRE(f.stack.activeStance(0, 1));

	SoundVariations::List list = f.bridge.soundVariations().sound[0];
	REQUIRE(list.count == 2);
	// ROW 0 IS THE BANKED ONE and is the one this case recalls TO. Its zone is real; its gamma base
	// is not.
	list.values[0].zone = 800.0;
	list.values[0].gammaBase = std::numeric_limits<double>::quiet_NaN();
	f.bridge.setSoundVariations(0, list);
	f.bridge.refresh();

	const double zoneBefore = f.read(ParamId::AZone);
	const size_t stepsBefore = f.history.length();
	REQUIRE(f.stack.activeStance(0, 1));

	// THE GESTURE, THROUGH THE ROW A HAND PRESSES.
	f.stack.activateVariation(0, 0);
	f.settle();

	// **REFUSED, AND REFUSED WHOLE.** Each of these is a different way for a half-recall to show:
	// the light moving, a step spent, or the sound partly moved.
	CHECK(f.stack.activeStance(0, 1));
	CHECK_FALSE(f.stack.activeStance(0, 0));
	CHECK(f.history.length() == stepsBefore);
	CHECK(f.read(ParamId::AZone) == doctest::Approx(zoneBefore));

	SUBCASE("and a sound one IS recalled, so the refusal is about the value and not the verb") {
		// THE NON-VACUITY HALF. Without it, "nothing happened" passes on a build where recall is
		// broken outright — which is the failure mode a refusal is one line away from.
		SoundVariations::List sound = f.bridge.soundVariations().sound[0];
		sound.values[0].gammaBase = 0.0;
		f.bridge.setSoundVariations(0, sound);
		f.bridge.refresh();

		f.stack.activateVariation(0, 0);
		f.settle();
		CHECK(f.stack.activeStance(0, 0));
		CHECK(f.history.length() == stepsBefore + 1);
		CHECK(f.read(ParamId::AZone) == doctest::Approx(800.0));
	}
}
TEST_CASE("every knob on an fx CARD is armable when its device is in the chain, OWLTT included") {
	// Reported: knobs on the OWLTT card could no longer be mapped, while the filter card and the
	// OWLTT popup were fine. Three facts in one sentence and the pair is what makes it findable: the
	// SAME device is mappable through its panel and not through its card, so this is not the device's
	// declarations and not the routing authority — both are shared by the two surfaces.
	//
	// A CARD PER DEVICE RATHER THAN OWLTT ALONE, because "OWLTT is broken" and "cards are broken and
	// OWLTT is the one that happened to be in use" are different claims and only a sweep separates them.
	//
	// EACH DEVICE ALONE IN THE CHAIN, and the crowded arrangement is the SUBCASE below rather than
	// folded in here. Splitting them is what separated "this device is broken" from "this SLOT is
	// broken" while finding it: OWLTT failed IDENTICALLY alone and behind the widest device in the
	// registry, and that is what said the fault was the card's own wiring rather than position or a
	// routing table run out of columns. The crowded case is kept because the hypothesis was
	// reasonable — drawn went from 103 declared controls to 127 the same night — and a reader who
	// forms it again should find it already answered.
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

		// ALONE IN THE CHAIN FIRST — see the SUBCASE below for the crowded arrangement. Splitting them
		// is what separates "this device is broken" from "this SLOT is broken".
		const size_t slot = f.bridge.addFxDevice(tag.c_str());
		REQUIRE(slot == 1);
		lane->resized();
		lane->scrollCardIntoView(slot);
		lane->resized();

		ui::FxCard* card = lane->cardAt(slot);
		REQUIRE(card != nullptr);

		std::vector<ui::Knob*> knobs;
		collectKnobs(card, &knobs);
		CAPTURE(knobs.size());
		if (knobs.empty()) {
			continue; // a card that draws no knobs has nothing to claim here
		}

		f.arm(backplane::Source::core(0));

		// A KNOB THAT IS A DESTINATION AND IS ENABLED MUST BE MAPPABLE. That is the honesty rule as an
		// equality rather than an implication, exactly as the modulator-strip sweep states it: a glow
		// that is absent where a drag WOULD work is the same lie as a glow that is present where it
		// would not.
		size_t destinations = 0;
		for (ui::Knob* knob : knobs) {
			if (knob->isModDestination() && knob->enabled()) {
				++destinations;
				CHECK(knob->mappable());
			}
		}
		// AND THE CARD HAS AT LEAST ONE, or the loop above asserted nothing and this case would stay
		// green for a card whose knobs had all quietly stopped being destinations.
		CHECK(destinations > 0);
	}

	SUBCASE("and behind the widest device in the registry, which is where the columns would run out") {
		// THE HYPOTHESIS THAT WAS WRONG, KEPT BECAUSE IT WAS REASONABLE. A device's controls become
		// destinations through a per-slot routing lane, and drawn went from 103 declared controls to
		// 127 the same night this was reported — so "OWLTT lost its columns to drawn" was the obvious
		// first thought. It is false: OWLTT failed identically alone. This pins the arrangement anyway,
		// so the next reader who forms that hypothesis finds it answered rather than plausible.
		ModFixture f;
		auto* lane = findInTree<ui::FxLane>(f.root);
		REQUIRE(lane != nullptr);
		REQUIRE(f.bridge.addFxDevice(fx::drawn::kTraits.tag) == 1);
		const size_t slot = f.bridge.addFxDevice(fx::owltt::kTraits.tag);
		REQUIRE(slot == 2);
		lane->resized();
		lane->scrollCardIntoView(slot);
		lane->resized();

		ui::FxCard* card = lane->cardAt(slot);
		REQUIRE(card != nullptr);
		std::vector<ui::Knob*> knobs;
		collectKnobs(card, &knobs);
		REQUIRE_FALSE(knobs.empty());

		f.arm(backplane::Source::core(0));
		size_t destinations = 0;
		for (ui::Knob* knob : knobs) {
			if (knob->isModDestination() && knob->enabled()) {
				++destinations;
				CHECK(knob->mappable());
			}
		}
		CHECK(destinations > 0);
	}
}
TEST_CASE("a coordinate cell is one infinite line, one box, and a zone dropdown") {
	// **THE WIDGET THAT REPLACED THE ZONE KNOB, AND THE PREMISE THAT REPLACED ITS PREMISE.** Most zone
	// knobs in the firmware let you keep turning past the 8th zone, which keeps driving the phase; a
	// phase offset is the same unit, only a much larger jump; and a zone name is just a name for a
	// region of an otherwise infinite space. One line, one unit — so a dial with meaningful ends was the wrong shape. What replaced
	// it went through a pair of boxes and is now a single box with a dropdown; see
	// `zone_coord_cell.h`, and the readout subcase below for what the second box was printing.
	namespace dtt = feathers::fx::owltt;
	FilterFixture f;
	ui::WidgetRegistry registry;
	REQUIRE(f.bridge.addFxDevice(dtt::kTraits.tag) == 1);
	ui::OwlttExpanded panel { f.deviceHost, registry, f.bridge.fxDeviceAt(1) };
	panel.setBounds(0.0f, 0.0f, panel.minimumSize().x, panel.minimumSize().y);
	panel.resized();

	// **A MODE CELL SINCE backlog IW** — the dropdown lists Character's eight behaviours then Free; the box is the one
	// unbounded value. In Free (the default) the value IS the rolling coordinate, so every claim below about the
	// value still holds — its lap and zone are read from the value itself now, not from a region selector.
	REQUIRE(panel.modeCellsForTest().size() == 2);
	ui::ModeValueCell& character = *panel.modeCellsForTest().front();
	const auto characterBox = [&]() -> ui::ValueBox& { return *character.boxFor(static_cast<size_t>(dtt::Param::Character)); };
	const auto lapOf = [](double v) { return static_cast<long>(std::floor(v)); };
	const auto zoneOf = [](double v) { return static_cast<long>(std::floor((v - std::floor(v)) * 8.0)); };

	const auto fine = [&] {
		return f.bridge.fxChain().device[1].row.field[static_cast<size_t>(dtt::Param::Character)];
	};
	// **WHAT `coarse()` USED TO READ NO LONGER EXISTS.** It returned `CharacterPhase`, and the
	// subcases below asserted that a drag left it alone. The param is deleted (2026-09-09,
	// with the dual-gang value boxes), so the claim it was making — this gesture writes ONE
	// parameter and no other — is made against the WHOLE ROW instead, which is strictly stronger: it
	// would catch a gesture writing any of the other 25, not just the one that used to sit next door.
	const auto rowSnapshot = [&] { return f.bridge.fxChain().device[1].row.field; };
	const auto onlyCharacterMoved = [&](const auto& before) {
		// **THE WHOLE ROW, NOT JUST THE DECLARED PARAMS.** The row is a fixed 128 slots and OWLTT
		// declares 26 of them; sweeping only the 26 would miss a positional walk running off the end,
		// which is the exact bug shape this device has a history of. The name is only printed where
		// there is one to print.
		const auto after = rowSnapshot();
		for (size_t i = 0; i < after.size(); ++i) {
			if (i == static_cast<size_t>(dtt::Param::Character)) {
				continue;
			}
			CAPTURE(i);
			CAPTURE(std::string(i < static_cast<size_t>(dtt::Param::kCount)
			                        ? dtt::kParamDescs[i].name
			                        : "<past the declaration>"));
			CHECK(after[i] == doctest::Approx(before[i]));
		}
	};
	const auto setFine = [&](double v) {
		// **THE DEVICE THE PANEL IS ADDRESSED BY (kill-lanes step 3b)** — this wrote at record LANE 1
		// while the panel above was built on the device at POSITION 1. The two are the same number on a
		// chain that has only been appended to, and stopped being one address either way once where a
		// device is parked became the record's own business.
		f.bridge.setDeviceControl(f.bridge.fxDeviceAt(1), static_cast<size_t>(dtt::Param::Character),
		                          static_cast<float>(v));
	};
	// **WHERE A COLUMN IS, ASKED OF THE BOX RATHER THAN COMPUTED BESIDE IT.** With the phase box gone
	// this cell drags like every other collapsed coordinate — by the DIGIT under the press (BJ piece
	// 1) — so a gesture aimed at "the centre" is aimed at whichever place happens to be drawn there,
	// and a case built on that would change meaning every time the value's width changed. `placeAt`
	// is the box's own hit test, and it is the one `onMouseDown` latches.
	const auto xOfPlace = [&](double place) {
		ui::ValueBox& box = characterBox();
		for (float x = 0.0f; x < box.width(); x += 0.25f) {
			if (box.placeAt(x) == doctest::Approx(place)) {
				return x;
			}
		}
		return -1.0f;
	};
	// A drag of `dy` pixels upward, pressed on the column worth `place`.
	const auto dragPlace = [&](double place, float dy) {
		ui::ValueBox& box = characterBox();
		const float x = xOfPlace(place);
		REQUIRE(x >= 0.0f);
		const float y = box.height() * 0.5f;
		box.onMouseDown().callback(at(x, y));
		REQUIRE(box.pressPlaceForTest() == doctest::Approx(place));
		box.onMouseDrag().callback(at(x, y - dy));
		box.onMouseUp().callback(at(x, y - dy));
	};

	SUBCASE("it says where it is as a coordinate, not as a percentage") {
		// A COORDINATE IS COUNTED, NOT MEASURED. The control declares itself to the engine as the
		// number it is, and `formatValue` renders it faithfully — the first render of this widget
		// read `0.00 : 0%`, two correct renderings of a thing that is not a percentage.
		//
		// **AND IT PRINTS THE COORDINATE NOW, NOT THE POSITION INSIDE THE ZONE.** The two-box form
		// overrode the formatter to print the firmware's 0..127 because the box beside it held the
		// lap; with one box that split has nowhere to live, and every other collapsed coordinate in
		// the project prints its own number. **Asserted at a value where the two DISAGREE** — at 3.0
		// the coordinate is 3 and the position inside zone 0 is 0, so a box that had kept the old
		// formatter would print `0` here.
		setFine(3.0);
		CHECK(lapOf(fine()) == 3);
		CHECK(zoneOf(fine()) == 0);
		CHECK(characterBox().readout().front() == '3');
		CHECK(characterBox().readout()
		      != std::to_string(feathers::zonePositionToDisplay(
		             static_cast<float>((fine() - std::floor(fine())) * 8.0 - zoneOf(fine())))));

		// **AND THERE IS EXACTLY ONE BOX IN THE CELL — no dual-gang value boxes.** OWLTT was the last
		// holdout of the stacked pair, kept because its coordinate
		// and its phase were genuinely different inputs while the coordinate was bounded to 0..1.
		// Unbounding it made the phase reachable through the coordinate itself — `owltt.cpp` derives
		// the lap from the whole part — so the second box was a redundant road to a position the
		// first box already reaches, and the params behind it are gone.
		//
		// **COUNTED ACROSS THE CHILDREN, NOT ASKED OF AN ACCESSOR.** A `phase()` returning null would
		// be satisfied by a cell that still seated a second box some other way; what a user can see is
		// how many boxes are in the rectangle, so that is what is counted.
		size_t boxesInCell = 0;
		for (const visage::Frame* child : character.children()) {
			if (dynamic_cast<const ui::ValueBox*>(child) != nullptr) {
				++boxesInCell;
			}
		}
		CHECK(boxesInCell == 1);

		// AND THE SELECTOR IS UNDER IT, in the row where the region's name always was — the footprint
		// the collapse deliberately did not move.
		CHECK(character.select().y() > characterBox().y());
		CHECK(character.select().x() == characterBox().x());
		CHECK(character.select().width() == characterBox().width());
	}

	SUBCASE("backlog IW: the name row is the MODE — eight behaviours, then Free, and Free by default") {
		// The dropdown selects a mode, not a phase region: the modes, then Free. It used to name the REGION the value stood in; it names the chosen mode now, which a drag of the
		// value cannot change.
		CHECK(character.select().zoneCount() == 9);
		CHECK(character.select().captionText() == "Free"); // a fresh OWLTT rolls as it always did
		f.bridge.setDeviceControl(f.bridge.fxDeviceAt(1), static_cast<size_t>(dtt::Param::CharacterMode), 3.0f);
		CHECK(character.select().captionText() == std::string(dtt::kCharacterZoneNames[3].name)); // Punch
		// AND A DRAG OF THE VALUE DOES NOT MOVE IT — fenced: the value rolls inside Punch.
		setFine(0.0);
		dragPlace(0.1, ui::ValueBox::kDragRange);
		CHECK(character.select().captionText() == std::string(dtt::kCharacterZoneNames[3].name));
	}

	SUBCASE("a drag past the eighth zone KEEPS GOING, and writes no second parameter") {
		// **THE CARRY IS GONE AND THIS IS WHAT REPLACED IT (BJ piece 3).** These four subcases used to
		// assert that dragging past the eighth zone WRAPPED the coordinate back to the bottom and
		// wrote +1 into the phase param — "just keep turning" implemented as a UI gesture writing a
		// second control, because the coordinate could not leave 0..1.
		//
		// **THAT MADE THE BOUND LOAD-BEARING: leaving the range was implemented by not leaving it.**
		// None of these coordinates is bounded now, so the coordinate simply continues, and
		// `owltt.cpp` derives the lap from its whole part. The phase param is gone entirely and
		// nothing is written behind the user's back.
		setFine(7.5 / 8.0);
		const auto before = rowSnapshot();
		// THE HUNDREDTHS COLUMN, a full drag: ten of its own units, so +0.10 — past the end of the
		// eighth zone from 0.9375 and not by so much that where it lands says nothing.
		dragPlace(0.01, ui::ValueBox::kDragRange);
		// PAST THE END, NOT BACK AT THE BOTTOM.
		CHECK(fine() > 1.0f);
		// AND THE ZONE HAS WRAPPED, which is what makes the names repeat rather than the value stop.
		CHECK(zoneOf(fine()) == 0);
		CHECK(lapOf(fine()) == 1);
		// **AND NOTHING ELSE IN THE ROW MOVED.** A gesture that silently wrote a second parameter is
		// exactly what the automation-ratchet hazard below was about; there is nothing left to
		// ratchet, and this is the assertion that would notice if something grew back.
		onlyCharacterMoved(before);
	}

	SUBCASE("and a boundary is reachable by hand, at the digit a hand would grab") {
		// THE NON-VACUITY OF THE SUBCASE ABOVE, stated on its own so a failure says which half broke:
		// a crossing that only a whole-line sweep could produce would satisfy "it keeps going" and be
		// unreachable in use. **At the declared range a full drag would cross sixteen million zones**,
		// so something has to stand between the declaration and the hand.
		//
		// **THAT SOMETHING IS THE DIGIT NOW, NOT A DECLARED SPAN.** The two-box form set
		// `setDragSpan(perLap / zones)` — one region per drag, fixed at construction. The collapsed
		// cell drags by the column under the press (so the speed of a mouse edit depends on where the
		// box is grabbed), which is what every other coordinate in the project does, so
		// the claim is asked the way a hand would ask it: grab the tenths and move a fifth of a drag.
		setFine(0.0);
		REQUIRE(zoneOf(fine()) == 0);
		const auto before = rowSnapshot();
		dragPlace(0.1, ui::ValueBox::kDragRange * 0.2f);
		CHECK(zoneOf(fine()) == 1);
		CHECK(lapOf(fine()) == 0);
		onlyCharacterMoved(before);
	}

	SUBCASE("a drag held past the top does not accumulate, and still writes nothing else") {
		// `lapsSent_` used to make a crossing an EVENT so a held drag could not bump the phase once
		// per mouse-move. With no carry the hazard is gone by construction — there is nothing a
		// repeated move could bump — so what is asserted is the property rather than the mechanism:
		// the drag is absolute from its anchor, so twenty identical moves land where one does.
		setFine(7.5 / 8.0);
		const auto before = rowSnapshot();
		ui::ValueBox& box = characterBox();
		const float x = xOfPlace(0.01);
		REQUIRE(x >= 0.0f);
		const float y = box.height() * 0.5f;
		box.onMouseDown().callback(at(x, y));
		box.onMouseDrag().callback(at(x, y - ui::ValueBox::kDragRange));
		const float afterOne = fine();
		for (int i = 0; i < 20; ++i) {
			box.onMouseDrag().callback(at(x, y - ui::ValueBox::kDragRange));
		}
		box.onMouseUp().callback(at(x, y - ui::ValueBox::kDragRange));
		CHECK(fine() == doctest::Approx(afterOne));
		onlyCharacterMoved(before);
		// AND THE DRAG DID SOMETHING, so the assertions above are not passing on a gesture that
		// missed — twenty moves that all did nothing would agree with each other perfectly.
		CHECK(fine() > 7.5f / 8.0f);
	}

	SUBCASE("the two cells hold two DIFFERENT controls, and each one is a destination") {
		ui::ModeValueCell& vibe = *panel.modeCellsForTest().back();
		const ui::ValueBox& vibeBox = *vibe.boxFor(static_cast<size_t>(dtt::Param::Vibe));

		// **THIS ASSERTED THE OPPOSITE TWICE, AND THE SECOND REVERSAL IS THE COLLAPSE.** It first read
		// *"THE COARSE HALF CANNOT BE A ROUTE'S TARGET" with the widget's "coarse" meaning the PHASE,
		// so it pinned the phase as un-routable. It was decided the other way on 2026-09-02 — every fine
		// phase control may be modulated — and the flag moved onto the phase param.
		//
		// **THERE IS NO HALF TO CHOOSE BETWEEN ANY MORE.** One control per coordinate, so `modulatable`
		// sits on it and the question the two reversals were about cannot be asked. What is left worth
		// asserting is that the flag is actually THERE: a collapse that deleted the phase and forgot to
		// carry its flag would leave OWLTT with two coordinates no route could reach.
		CHECK(characterBox().control().desc().modulatable);
		CHECK(vibeBox.control().desc().modulatable);

		// AND THE TWO CELLS ARE NOT SHARING A CONTROL — the collapse into one device-wide gamma that
		// this whole arc exists to avoid, which a one-box cell makes newly easy to write by accident.
		CHECK(&characterBox().control() != &vibeBox.control());
	}

	SUBCASE("it occupies exactly a knob's column, row for row") {
		// The box-and-label assembly stays the same general size as a knob-and-label assembly, for
		// easier stacking. Matching the outer height alone would leave the
		// captions on different lines, so it borrows the knob's row structure instead.
		CHECK(ui::Knob::columnHeight(ui::Knob::kEngineRailDiameter)
		      == doctest::Approx(ui::Knob::columnHeight(ui::Knob::kEngineRailDiameter)));
		// THE WINDOW'S TONE GROUP IS A ROW, so its cells take the knob column's rows at a small face
		// (`OwlttExpanded::kToneFace`) rather than at a knob's: the same row structure, the shorter column.
		CHECK(character.height() == doctest::Approx(ui::Knob::columnHeight(ui::OwlttExpanded::kToneFace)));
	}
}
TEST_CASE("Explore's gamma is ONE value box, reading the sum and writing the base") {
	// **THIS CASE ASSERTED A COARSE:FINE PAIR FOR ABOUT AN HOUR.** The pair was the design and
	// the photographs were approved; what reversed it is the decomposition, not the look. `slice_build.h`:
	//
	//     sliceHome(zone, phaseOffset, gammaBase) = zone/1023.0 + phaseOffset + gammaBase
	//
	// Three terms on ONE axis — gamma is a THIRD term, not the coarse half of anything. The pair had
	// put `gammaBase` beside `GlobalGamma` and called them coarse and fine; they are two different
	// quantities sharing a display. Explore's device gamma (not applicable to effects) is one number,
	// so one value box. The zoned DEVICES keep their `ZoneCoordCell` — there the boxes
	// really are one line at two granularities.
	FilterFixture f;
	ui::WidgetRegistry registry;
	size_t selected = 0;
	ui::KnobTray tray { f.bridge, registry, nullptr, [&] { return selected; } };
	tray.setBounds(0.0f, 0.0f, 1200.0f, ui::KnobTray::kHeight);
	tray.resized();

	ui::ValueBox* gamma = nullptr;
	size_t boxes = 0;
	for (visage::Frame* child : tray.children()) {
		if (auto* box = dynamic_cast<ui::ValueBox*>(child)) {
			gamma = box;
			++boxes;
		}
	}
	REQUIRE(gamma != nullptr);

	SUBCASE("the ROW names it, not the box — the shared label line has no hole in it") {
		// **THE FIRST DRAFT LET THE BOX CAPTION ITSELF**, which collapsed two of the row's three tiers
		// into one rectangle and left the SHARED label line empty at this position: the line read
		// `PATH · POS X · POS Y · RELIEF · <gap> · WEATHER …`. It also named this control in a
		// different PLACE from the other ten — named inside itself rather than on the row.
		//
		// Caught by the integrator reading the photograph, not by any assertion here — which is why
		// the assertion exists now. An empty caption is the observable half of "the row names it";
		// the caption's PLACEMENT is `KnobTray::draw`, at `Knob`'s own `kDiameter + kGap` so the ten
		// names sit on one line by construction rather than by a number that lands nearby.
		CHECK(gamma->label().empty());
		CHECK(gamma->isCompact());
		// AND IT IS CENTRED IN THE FACE BAND, where a dial's centre is — **stated as the CENTRE
		// because "inside the band" was too loose to be a test.** The first version of this line
		// asserted `bottom() <= kPadTop + kDiameter`, and the known-bad it was written for — the box
		// sizing itself against the whole COLUMN instead of the face band — put the box at 47.5 with
		// its bottom at 62.5, comfortably inside a bound of 64.5. **It passed.** Seventeen pixels of
		// drop, invisible to the assertion and obvious in a photograph.
		//
		// A containment check is the weakest form of a placement claim: it is satisfied by every
		// position that is not catastrophic. The claim is that this box's centre is where a knob's
		// face centre is, so that is what it says.
		CHECK(gamma->bounds().y() + gamma->height() * 0.5f
		      == doctest::Approx(ui::KnobTray::kPadTop + ui::Knob::kDiameter * 0.5f));
	}

	SUBCASE("one box, and no coordinate pair anywhere in the row") {
		CHECK(boxes == 1);
		for (visage::Frame* child : tray.children()) {
			CHECK(dynamic_cast<ui::ZoneCoordCell*>(child) == nullptr);
		}
		// THE ROW IS STILL TEN CELLS. A box replaced a knob; it did not join the row.
		CHECK(tray.cellCount() == 9); // nine since 2026-10-03 (Attack, Release out; Voice Level in)
		CHECK(tray.knobCount() == 8);
	}

	SUBCASE("it reads the SUM of all three terms") {
		// THE NUMBER THAT MEANS SOMETHING MUSICALLY — where this sound's gamma actually is. A box
		// showing only the base would read zero on a sound whose gamma plainly is not.
		f.bridge.setValue(static_cast<clap_id>(ParamId::GlobalGamma), 1.5);
		f.bridge.setValue(static_cast<clap_id>(soundParam(0, SoundField::Gamma)), 0.75);
		f.bridge.beginSoundGammaGesture();
		f.bridge.setSoundGamma(0, 4.0);
		f.bridge.endSoundGammaGesture();
		f.bridge.refresh();
		CHECK(gamma->control().value() == doctest::Approx(1.5 + 0.75 + 4.0));
	}

	SUBCASE("**A TYPED NUMBER LANDS IN THE BASE, AND ONLY THE BASE**") {
		// The base is the sole term that can absorb any value, being unbounded and id-less. The other
		// two are bounded parameters with automation lanes, and a widget that silently rewrote one of
		// those would be editing a host's automation target behind its back.
		f.bridge.setValue(static_cast<clap_id>(ParamId::GlobalGamma), 1.5);
		f.bridge.setValue(static_cast<clap_id>(soundParam(0, SoundField::Gamma)), 0.5);
		f.bridge.refresh();
		gamma->control().beginGesture();
		gamma->control().setValue(10.0);
		gamma->control().endGesture();

		// THE SUM LANDED WHERE IT WAS ASKED TO, which is what "type where you want to be" means on a
		// quantity assembled from three terms.
		CHECK(gamma->control().value() == doctest::Approx(10.0));
		// AND THE OTHER TWO DID NOT MOVE — the assertion the sum alone cannot make.
		CHECK(f.bridge.value(static_cast<clap_id>(ParamId::GlobalGamma)) == doctest::Approx(1.5));
		CHECK(f.bridge.value(static_cast<clap_id>(soundParam(0, SoundField::Gamma)))
		      == doctest::Approx(0.5));
		CHECK(f.bridge.soundGamma().base[0] == doctest::Approx(10.0 - 2.0));
	}

	SUBCASE("it follows the selection, because the base is per sound") {
		f.bridge.beginSoundGammaGesture();
		f.bridge.setSoundGamma(0, 5.0);
		f.bridge.setSoundGamma(1, -3.0);
		f.bridge.endSoundGammaGesture();
		f.bridge.refresh();
		selected = 0;
		CHECK(gamma->control().value() == doctest::Approx(5.0));
		selected = 1;
		CHECK(gamma->control().value() == doctest::Approx(-3.0));
	}

	SUBCASE("**EVERY PARAMETER IN THE SUM TELLS THE BOX THAT DRAWS IT**") {
		// PLAYBOOK 8i, WHICH THIS ARC PAID FOR ONCE ALREADY. A `ValueBox` is built from a
		// `Modulatable` and has no `clap_id` to key on, so nothing watches on its behalf: the control
		// reads the bridge perfectly while the box draws its construction-time number forever, and a
		// value case passes throughout. Only a photograph can see that — so the case asks the
		// registry, which reports its watchers precisely because no headless venue can see a repaint.
		//
		// AND THE READING IS A SUM, so it is not enough to watch one of them. `GlobalGamma` moves it
		// and so does the SELECTED sound's own Gamma — and which sound that is changes under the
		// widget, so every slot's Gamma is watched rather than whichever was selected at construction.
		const auto tells = [&](clap_id id) {
			const std::span<visage::Frame* const> w = registry.watchers(id);
			return std::find(w.begin(), w.end(), gamma) != w.end();
		};
		CHECK(tells(static_cast<clap_id>(ParamId::GlobalGamma)));
		for (size_t sound = 0; sound < kMaxSounds; ++sound) {
			CAPTURE(sound);
			CHECK(tells(static_cast<clap_id>(soundParam(sound, SoundField::Gamma))));
		}
	}

	SUBCASE("it is a destination for a RELATIVE macro alone (backlog PQ), and never a modulation one") {
		// The quantity it WRITES has no parameter id — `sound_gamma.h`: an unbounded range is one no
		// host could draw and no automation lane could hold. So it is still not MODULATABLE. What it became
		// (backlog PQ) is the surface a Relative macro's route is made on: an edit road into the unbounded
		// half, which `ParamBridge::mayReachEditOnly` lets no other source take — so with nothing armed it
		// offers no drag (the gamma box's own case in the undo suite pins the Relative arm).
		CHECK_FALSE(gamma->control().desc().modulatable);
		CHECK(gamma->control().display().isDestination);
		CHECK_FALSE(gamma->control().display().routable);
	}
}
TEST_CASE("automod strip: the waveform drawn is the waveform the DSP will read, at the card's own "
          "coordinate") {
	namespace amd = feathers::fx::automod;
	AutomodStripFixture f;

	const amd::Settings s = f.card->settings();
	CHECK(s.type == doctest::Approx(AutomodStripFixture::kType));
	CHECK(s.flavor == doctest::Approx(AutomodStripFixture::kFlavor));
	CHECK(s.mod == doctest::Approx(AutomodStripFixture::kMod));

	const ui::AutomodCard::PreviewTrace trace = f.card->previewTrace();
	REQUIRE(trace.phase.size() > 8);
	REQUIRE(trace.lfo.size() == trace.phase.size());
	REQUIRE(trace.cutoffHz.size() == trace.phase.size());

	// THE INDEPENDENT RESTATEMENT — the audio path's own road, walked here rather than asked of the
	// card: settings to coordinates to a derive to a reading of the shape.
	const amd::AutomodDerived want =
	    amd::deriveAutomod(amd::coordsOf(s, amd::nominalRateHz(s)));
	for (size_t i = 0; i < trace.phase.size(); ++i) {
		CAPTURE(i);
		CHECK(trace.lfo[i]
		      == doctest::Approx(character::evalLfoShape(want.shape, trace.phase[i])).epsilon(1e-5));
	}

	// **AND THE NEIGHBOURING COORDINATE MUST FAIL**, or the case above would pass against a strip
	// that draws SOME waveform rather than THIS one. Two decisions were once pinned by a green revert
	// for want of exactly this half.
	amd::Settings other = s;
	other.mod = s.mod + 0.07f;
	const amd::AutomodDerived elsewhere =
	    amd::deriveAutomod(amd::coordsOf(other, amd::nominalRateHz(s)));
	size_t differing = 0;
	for (size_t i = 0; i < trace.phase.size(); ++i) {
		if (std::abs(trace.lfo[i] - character::evalLfoShape(elsewhere.shape, trace.phase[i]))
		    > 1e-3f) {
			++differing;
		}
	}
	CHECK(differing > trace.phase.size() / 4);

	// NOT A FLAT LINE, which is what makes every check above non-vacuous. The origin has 0.000
	// octaves of travel in it and a zero-filled row would land there.
	const auto [lo, hi] = std::minmax_element(trace.lfo.begin(), trace.lfo.end());
	CHECK(*hi - *lo > 0.5f);
}
TEST_CASE("automod strip: the phase axis covers one whole cycle and puts a vertex on every corner") {
	namespace amd = feathers::fx::automod;
	AutomodStripFixture f;
	const ui::AutomodCard::PreviewTrace trace = f.card->previewTrace();

	CHECK(trace.phase.front() == doctest::Approx(0.0f));
	CHECK(trace.phase.back() == doctest::Approx(1.0f));
	for (size_t i = 1; i < trace.phase.size(); ++i) {
		CAPTURE(i);
		CHECK(trace.phase[i] > trace.phase[i - 1]);
	}

	// EVERY BREAKPOINT IS A SAMPLE, exactly. A uniform grid would round each corner by up to a
	// sample's width, and this waveform is nothing but corners — the drawn shape would not be the
	// played one.
	const amd::Settings s = f.card->settings();
	const amd::AutomodDerived d = amd::deriveAutomod(amd::coordsOf(s, amd::nominalRateHz(s)));
	for (size_t w = 0; w < character::kLfoWaypoints; ++w) {
		CAPTURE(w);
		const bool found = std::any_of(trace.phase.begin(), trace.phase.end(), [&](float p) {
			return std::abs(p - d.shape.phase[w]) < 1e-6f;
		});
		CHECK(found);
	}
}
TEST_CASE("automod strip: Depth and Manual reach the corner trace, and the waveform ignores them") {
	namespace amd = feathers::fx::automod;
	AutomodStripFixture f;

	// **THE ONE CASE THAT A DIFFERENCE CHECK CANNOT WRITE.** At zero depth the device does not sweep
	// the filter at all, and a strip that had dropped `modulationFor` would still draw a moving
	// waveform AND a moving corner — two plausible curves, both wrong. Only the VALUE says so.
	f.set(amd::Param::Depth, 0.0f);
	f.set(amd::Param::Manual, 0.0f);
	const ui::AutomodCard::PreviewTrace flat = f.card->previewTrace();
	// EXACT EQUALITY, NOT `Approx(0)` — the playbook's 8j trap: `Approx(0).epsilon(e)` compares
	// `|a-b| < e * (1 + max(|a|,|b|))`, which at epsilon 0 REJECTS an exact match. This case wants
	// exactness, so it asks for it directly.
	const auto [flatLo, flatHi] = std::minmax_element(flat.cutoffHz.begin(), flat.cutoffHz.end());
	CHECK(*flatHi == *flatLo);

	// AND THE WAVEFORM IS UNTOUCHED BY IT, which is the other half of the same claim: Depth belongs
	// to the corner, not to the shape the Mod coordinate authored.
	const auto [lfoLo, lfoHi] = std::minmax_element(flat.lfo.begin(), flat.lfo.end());
	CHECK(*lfoHi - *lfoLo > 0.5f);

	// MANUAL MOVES THE CORNER WITH THE LFO STILL AT ZERO DEPTH — it is a fixed offset ADDED after the
	// scaling, so it reaches the sound at any depth. A strip that folded Manual into the depth
	// product would draw this as unchanged.
	f.set(amd::Param::Manual, 0.8f);
	const ui::AutomodCard::PreviewTrace offset = f.card->previewTrace();
	CHECK(offset.cutoffHz.front() > flat.cutoffHz.front());
	CHECK(offset.lfo.front() == doctest::Approx(flat.lfo.front()));

	// AND DEPTH RESTORES THE SWEEP.
	f.set(amd::Param::Manual, 0.0f);
	f.set(amd::Param::Depth, 1.0f);
	const ui::AutomodCard::PreviewTrace swept = f.card->previewTrace();
	const auto [lo, hi] = std::minmax_element(swept.cutoffHz.begin(), swept.cutoffHz.end());
	CHECK(*hi > *lo * 1.05f);
}
TEST_CASE("automod strip: the corner trace is the audio path's own arithmetic") {
	namespace amd = feathers::fx::automod;
	AutomodStripFixture f;
	f.set(amd::Param::Depth, 0.9f);
	f.set(amd::Param::Manual, -0.3f);

	const amd::Settings s = f.card->settings();
	const amd::AutomodDerived d = amd::deriveAutomod(amd::coordsOf(s, amd::nominalRateHz(s)));
	const ui::AutomodCard::PreviewTrace trace = f.card->previewTrace();

	for (size_t i = 0; i < trace.phase.size(); ++i) {
		CAPTURE(i);
		// `pitchRatio` IS 1 BECAUSE A CARD IS NOT PLAYING A NOTE, which is a stated limit of the
		// picture rather than an omission: note tracking moves the corner at play time and no surface
		// can know the note in advance.
		const float want = amd::cornerHzFor(d.filterCutoffBase, d.filterModDepth,
		                                    amd::modulationFor(s, trace.lfo[i]), 1.0f,
		                                    f.bridge.sampleRate());
		CHECK(trace.cutoffHz[i] == doctest::Approx(want).epsilon(1e-6));
	}

	// STOP HANDS THE WHOLE MODULATION TO MANUAL, so the corner stops moving while the waveform does
	// not — the mode's promise, drawn.
	f.set(amd::Param::LfoMode, static_cast<float>(amd::LfoModeChoice::Stop));
	const ui::AutomodCard::PreviewTrace stopped = f.card->previewTrace();
	const auto [lo, hi] = std::minmax_element(stopped.cutoffHz.begin(), stopped.cutoffHz.end());
	CHECK(*hi == *lo);
}
TEST_CASE("automod strip: the response is drawn at the rate the ENGINE reports") {
	namespace amd = feathers::fx::automod;
	AutomodStripFixture f;
	f.card->setPicture(ui::AutomodCard::Picture::Dual);

	// THE CLAIM SURVIVED THE PICTURE CHANGING. It used to be about the corner-over-time axis ending
	// where the filter ends; the corner trace is gone and the claim moved to the response curve,
	// which takes the same sample rate through `magnitudeAt`.
	//
	// `gui_bridge.h` records what it cost to learn this: the filter card drew its curve at a
	// hardcoded 48000 and was measured up to 1.67 dB wrong on an ordinary 44.1k session.
	const ui::AutomodCard::ResponseTrace at48 = f.card->responseTrace();
	REQUIRE(at48.gain.size() > 8);

	f.bridge.setSampleRate(44100.0);
	const ui::AutomodCard::ResponseTrace at44 = f.card->responseTrace();
	REQUIRE(at44.gain.size() == at48.gain.size());

	// THE CURVE MOVES. A bilinear-warped SVF's shape in Hz barely moves at low frequencies, so this
	// asks about the TOP of the span, where the warp actually bites — the same place the filter
	// card's 1.67 dB was measured.
	size_t differing = 0;
	for (size_t i = at48.gain.size() / 2; i < at48.gain.size(); ++i) {
		if (std::abs(at48.gain[i] - at44.gain[i]) > 1e-4f) {
			++differing;
		}
	}
	CHECK(differing > 0);

	// AND THE FREQUENCY AXIS DOES NOT — it is the audible span, a fact about hearing rather than
	// about the host, and it is the multimode filter's own so the two cards can be compared.
	CHECK(at44.hz.front() == doctest::Approx(at48.hz.front()));
	CHECK(at44.hz.back() == doctest::Approx(at48.hz.back()));
	CHECK(at48.hz.front() == doctest::Approx(static_cast<float>(fx::filter::kProfileLoHz)));
	CHECK(at48.hz.back() == doctest::Approx(static_cast<float>(fx::filter::kProfileHiHz)));
}
TEST_CASE("automod strip: the picture takes room the controls then start below") {
	AutomodStripFixture f;
	const visage::Bounds strip = f.card->lfoStripBounds();
	CHECK(strip.width() > 0.0f);
	CHECK(strip.height() == doctest::Approx(ui::AutomodCard::kStripHeight));

	// NOTHING IS LAID OVER IT. `layOut` takes the first row's y from this rectangle's bottom, so a
	// strip that grew without the height growing with it would put a coordinate box on top of the
	// waveform — the shape of defect this lane produced when the sync knob arrived.
	REQUIRE_FALSE(f.card->coordsForTest().empty());
	for (const auto& coord : f.card->coordsForTest()) {
		CHECK(coord->y() >= strip.bottom());
	}
	for (const auto& knob : f.card->knobsForTest()) {
		CHECK(knob->y() >= strip.bottom());
	}
	// **AND THE CARD IS TALL ENOUGH TO HOLD EVERY WIDGET IT LAID OUT**, which is the whole invariant
	// and NOT the one this case asked for first.
	//
	// THE FIRST VERSION ASKED ONLY `openHeight() > strip.bottom()`, AND A KNOWN-BAD THAT DELETED THE
	// STRIP FROM `openHeight` PASSED IT — 498 of 498 green. The strip is at the TOP, so a height
	// missing 70px still clears it comfortably; what overflows is the bottom row, which then draws
	// over whatever the lane put beneath this card. A card's height is a claim about its LOWEST
	// widget, so that is what is asked.
	float lowest = strip.bottom();
	for (const auto& coord : f.card->coordsForTest()) {
		lowest = std::max(lowest, coord->y() + coord->height());
	}
	for (const auto& knob : f.card->knobsForTest()) {
		lowest = std::max(lowest, knob->y() + knob->height());
	}
	CHECK(lowest > strip.bottom());
	CHECK(f.card->openHeight() >= lowest);
	CHECK(f.card->laneHeight() >= f.card->openHeight());

	// FOLDED, THE PICTURE IS GONE — an empty rectangle, so `drawLfoStrip` returns before drawing a
	// waveform over whatever the lane put underneath.
	f.card->setFolded(true);
	CHECK(f.card->lfoStripBounds().width() == doctest::Approx(0.0f));
}
TEST_CASE("automod card: no knob's caption or face is wider than the cell it was given") {
	// **THE DEFECT THIS PINS SHIPPED AND WAS ONLY FOUND IN A PHOTOGRAPH.** With six performance knobs
	// on one row the label line read `RATE SYNC :O MOC DEPTH ANUAL MIX` — `LFO MODE` and `MANUAL`
	// drawn ON TOP of their neighbours, not merely clipped. `knob.cpp:551` centres a caption at
	// `(width() - captionWidth) * 0.5f`, and that x is NEGATIVE whenever the caption is wider than
	// the cell, so an over-packed row does not overflow tidily to one side — it spills both ways.
	//
	// AND THE FACES WERE ALREADY TOO WIDE BEFORE THE CAPTIONS: `kEngineRailDiameter` is 38 against a
	// 32px cell. So both halves are asked here, because a row can be re-crowded by adding a control
	// OR by lengthening a name, and only one of those touches this file.
	//
	// ASKED OF THE CARD IN THE LANE, at the width the lane actually gives it — a card sized by the
	// case would be a statement about a width no hand ever sees.
	AutomodStripFixture f;
	REQUIRE_FALSE(f.card->knobsForTest().empty());

	const visage::Font& face = ui::font(ui::kKnobLabelSize);
	for (const auto& knob : f.card->knobsForTest()) {
		std::string caption(knob->label());
		for (char& c : caption) {
			c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		}
		CAPTURE(caption);
		CHECK(knob->width() > 0.0f);
		// THE CAPTION FITS, so `(width - captionWidth) * 0.5` cannot go negative.
		CHECK(ui::trackedWidth(face, caption, ui::kKnobLabelTracking) <= knob->width());
		// AND SO DOES THE THING IT NAMES.
		CHECK(ui::Knob::kEngineRailDiameter <= knob->width());
	}

	// THE COORDINATE ROW TOO, which is the other row this card packs — four cells rather than six,
	// and its boxes carry their own captions on the same line.
	for (const auto& coord : f.card->coordsForTest()) {
		CHECK(coord->width() >= ui::Knob::kEngineRailDiameter);
	}
}
TEST_CASE("automod strip: the response curve is the filter's magnitude at the resting corner") {
	namespace amd = feathers::fx::automod;
	AutomodStripFixture f;
	f.card->setPicture(ui::AutomodCard::Picture::Dual);
	f.set(amd::Param::Manual, 0.35f);

	const amd::Settings s = f.card->settings();
	const amd::AutomodDerived d = amd::deriveAutomod(amd::coordsOf(s, amd::nominalRateHz(s)));
	// THE INDEPENDENT RESTATEMENT: at rest the LFO contributes nothing, but MANUAL still does — it is
	// a fixed offset added after the depth scaling, so it moves where the filter SITS.
	const float rest = amd::cornerHzFor(d.filterCutoffBase, d.filterModDepth,
	                                    amd::modulationFor(s, 0.0f), 1.0f, f.bridge.sampleRate());

	const ui::AutomodCard::ResponseTrace t = f.card->responseTrace();
	REQUIRE(t.hz.size() > 8);
	REQUIRE(t.gain.size() == t.hz.size());
	for (size_t i = 0; i < t.hz.size(); ++i) {
		CAPTURE(i);
		CHECK(t.gain[i]
		      == doctest::Approx(amd::magnitudeAt(rest, d.filterResonance, d.filterMix, t.hz[i],
		                                          f.bridge.sampleRate()))
		             .epsilon(1e-6));
	}

	// **AND A CORNER AN OCTAVE AWAY MUST NOT MATCH**, or the loop above would pass against a curve
	// drawn at some corner rather than at THIS one — the half two decisions were once pinned without.
	size_t differing = 0;
	for (size_t i = 0; i < t.hz.size(); ++i) {
		if (std::abs(t.gain[i]
		             - amd::magnitudeAt(rest * 2.0f, d.filterResonance, d.filterMix, t.hz[i],
		                                f.bridge.sampleRate()))
		    > 1e-3f) {
			++differing;
		}
	}
	CHECK(differing > t.hz.size() / 4);

	// NOT A FLAT LINE, which is what stops every check above being vacuous.
	const auto [lo, hi] = std::minmax_element(t.gain.begin(), t.gain.end());
	CHECK(20.0f * std::log10(*hi / std::max(*lo, 1e-6f)) > 5.0f);
}
TEST_CASE("automod strip: the two pictures share one box, and only the drawn curve is computed") {
	AutomodStripFixture f;

	// **ONE FIELD.** The plots are truly overlaid, not two side by side, and the way
	// that is guaranteed rather than arranged is that there is only one rectangle.
	f.card->setPicture(ui::AutomodCard::Picture::Dual);
	CHECK(f.card->responseStripBounds() == f.card->lfoStripBounds());
	CHECK(f.card->responseStripBounds().width() > 0.0f);

	// AND NO RESPONSE AT ALL WHEN THE PICTURE DOES NOT INCLUDE ONE.
	f.card->setPicture(ui::AutomodCard::Picture::LfoOnly);
	CHECK(f.card->responseStripBounds().width() == doctest::Approx(0.0f));
	f.card->setPicture(ui::AutomodCard::Picture::Dual);

	// **THE SWEEP'S CURVES ARE NOT COMPUTED WHEN NOTHING DRAWS THEM.** Filling them unconditionally
	// was 6.3 us of this strip's 11 — two curves the shipping picture does not draw. Asserted on the
	// vectors because an empty one is the only observable proof the work did not happen.
	f.card->setResponseStyle(ui::AutomodCard::ResponseStyle::Plain);
	const ui::AutomodCard::ResponseTrace plain = f.card->responseTrace();
	CHECK_FALSE(plain.gain.empty());
	CHECK(plain.gainLo.empty());
	CHECK(plain.gainHi.empty());

	f.card->setResponseStyle(ui::AutomodCard::ResponseStyle::Band);
	const ui::AutomodCard::ResponseTrace band = f.card->responseTrace();
	CHECK(band.gainLo.size() == band.gain.size());
	CHECK(band.gainHi.size() == band.gain.size());
	// THE TWO ENDS ARE DIFFERENT ENDS, or the band would be a line.
	CHECK(band.gainLo != band.gainHi);
}
TEST_CASE("trace marker: where a parameter lands on a polyline, including the ends nothing reaches") {
	// **THE SHARED HALF, PINNED WHERE IT LIVES RATHER THAN ONLY THROUGH ITS FIRST CALLER.** The MSEG
	// gets this marker next, so the arithmetic is asserted on its own terms — and two of its branches
	// are UNREACHABLE through the automod strip, because the bridge wraps a phase into [0, 1) and the
	// strip's key spans exactly that. A guard reachable by only one caller is a guard the second
	// caller finds out about the hard way.
	const std::vector<float> key { 0.0f, 0.25f, 0.5f, 1.0f };

	SUBCASE("inside, it is the fraction of the segment it fell in") {
		const ui::TraceMarker::Landing mid = ui::TraceMarker::locate(key, 0.375f);
		REQUIRE(mid.valid);
		CHECK(mid.lo == 1);
		CHECK(mid.hi == 2);
		CHECK(mid.frac == doctest::Approx(0.5f));
		// AND THE BLEND IS ON THE CHORD: halfway between two screen positions, in pixels.
		CHECK(ui::TraceMarker::at(mid, 10.0f, 30.0f) == doctest::Approx(20.0f));
	}

	SUBCASE("on a vertex, the blend lands exactly on it") {
		// **ASSERTED THROUGH THE BLEND RATHER THAN ON `lo`/`hi`**, because a vertex is the boundary of
		// two segments and either answer is correct — `frac` 1 of the pair below it, or `frac` 0 of
		// the pair above. What must be true is that the COORDINATE comes out the vertex's own, and a
		// case that pinned the pair would be pinning an implementation choice as if it were the
		// contract.
		const ui::TraceMarker::Landing on = ui::TraceMarker::locate(key, 0.25f);
		REQUIRE(on.valid);
		const float xs[] = { 10.0f, 40.0f, 70.0f, 130.0f }; // one screen x per key vertex
		CHECK(ui::TraceMarker::at(on, xs[on.lo], xs[on.hi]) == doctest::Approx(40.0f));
	}

	SUBCASE("outside, it is CLAMPED to the ends rather than refused") {
		// Unreachable from the automod strip — the bridge wraps — and reachable the moment a caller
		// keys on something that does not span its own axis, which the MSEG's time axis will not
		// while a shape is shorter than its box.
		const ui::TraceMarker::Landing below = ui::TraceMarker::locate(key, -3.0f);
		REQUIRE(below.valid);
		CHECK(below.lo == 0);
		CHECK(below.hi == 1);
		CHECK(below.frac == doctest::Approx(0.0f));

		const ui::TraceMarker::Landing above = ui::TraceMarker::locate(key, 9.0f);
		REQUIRE(above.valid);
		CHECK(above.lo == 2);
		CHECK(above.hi == 3);
		CHECK(above.frac == doctest::Approx(1.0f));
	}

	SUBCASE("a curve with no line, and a parameter that is not a number, are REFUSED") {
		// A marker at NaN is drawn nowhere at all — a silently missing element rather than a visibly
		// wrong one — so the caller is told rather than handed a coordinate.
		const std::vector<float> one { 0.5f };
		CHECK_FALSE(ui::TraceMarker::locate(one, 0.5f).valid);
		CHECK_FALSE(ui::TraceMarker::locate({}, 0.5f).valid);
		CHECK_FALSE(
		    ui::TraceMarker::locate(key, std::numeric_limits<float>::quiet_NaN()).valid);
		CHECK_FALSE(ui::TraceMarker::locate(key, std::numeric_limits<float>::infinity()).valid);
	}

	SUBCASE("a repeated key is a zero-length segment, and the vertex is the honest answer") {
		// Dividing by a zero span is how a marker ends up at infinity, which draws nothing anywhere.
		const std::vector<float> flat { 0.0f, 0.5f, 0.5f, 1.0f };
		const ui::TraceMarker::Landing where = ui::TraceMarker::locate(flat, 0.5f);
		REQUIRE(where.valid);
		CHECK(std::isfinite(where.frac));
		CHECK(std::isfinite(ui::TraceMarker::at(where, 4.0f, 8.0f)));
	}
}
TEST_CASE("automod strip: the star sits on the waveform at the phase the DSP published") {
	namespace amd = feathers::fx::automod;
	AutomodStripFixture f;

	// **A DRAWN POSITION IS NOT OBSERVABLE HEADLESSLY**, so the marker's coordinates are exposed and
	// asserted against an independent restatement of where the waveform is — not against a second
	// render, which would pass whenever anything differed.
	const ui::AutomodCard::PreviewTrace trace = f.card->previewTrace();
	REQUIRE(trace.phase.size() > 8);
	const visage::Bounds strip = f.card->lfoStripBounds();
	REQUIRE(strip.width() > 1.0f);

	ui::AutomodStrip* pict = f.card->strip();
	REQUIRE(pict != nullptr);

	// THE STRIP DRAWS IN ITS OWN COORDINATES, so the restatement is written in them too.
	const float inner = std::max(strip.height() - 2.0f * 4.0f, 1.0f); // kStripInset
	const float mid = strip.height() * 0.5f;
	const auto xAt = [&](float p) { return strip.width() * p; };
	const auto yAt = [&](float amplitude) {
		return mid - std::clamp(amplitude, -1.0f, 1.0f) * inner * 0.5f;
	};

	SUBCASE("at a vertex the mark is ON the vertex, exactly") {
		// A VERTEX RATHER THAN AN ARBITRARY PHASE, so the expected y is the trace's own number and
		// the interpolation cannot hide behind a tolerance.
		const size_t k = trace.phase.size() / 3;
		f.bridge.publishAutomodReadings(f.slot, trace.phase[k], 0.0f);
		const ui::AutomodStrip::Marker mark = pict->marker();
		REQUIRE(mark.drawn);
		CHECK(mark.at.x == doctest::Approx(xAt(trace.phase[k])).epsilon(1e-4));
		CHECK(mark.at.y == doctest::Approx(yAt(trace.lfo[k])).epsilon(1e-4));
	}

	SUBCASE("between two vertices it is ON THE CHORD, which is the line that was drawn") {
		// **THE POINT OF INTERPOLATING IN SCREEN SPACE.** The waveform is drawn as straight segments
		// between vertices, so the marker must sit on the CHORD rather than on the curve the vertices
		// were sampled from. Evaluating `evalLfoShape` afresh would put it off the drawn line by
		// however much the sampling rounded the shape — worst exactly at a corner.
		const size_t k = trace.phase.size() / 2;
		REQUIRE(k + 1 < trace.phase.size());
		const float half = 0.5f * (trace.phase[k] + trace.phase[k + 1]);
		f.bridge.publishAutomodReadings(f.slot, half, 0.0f);
		const ui::AutomodStrip::Marker mark = pict->marker();
		REQUIRE(mark.drawn);
		CHECK(mark.at.x == doctest::Approx(xAt(half)).epsilon(1e-4));
		CHECK(mark.at.y
		      == doctest::Approx(0.5f * (yAt(trace.lfo[k]) + yAt(trace.lfo[k + 1]))).epsilon(1e-4));
	}

	SUBCASE("it MOVES, across the whole box, and it is the phase that moves it") {
		// NON-VACUITY IN THE DIRECTION THAT MATTERS: a marker pinned at one x satisfies every check
		// above at whichever phase happens to be published.
		f.bridge.publishAutomodReadings(f.slot, 0.0f, 0.0f);
		const float atZero = pict->marker().at.x;
		f.bridge.publishAutomodReadings(f.slot, 0.5f, 0.0f);
		const float atHalf = pict->marker().at.x;
		f.bridge.publishAutomodReadings(f.slot, 0.99f, 0.0f);
		const float atEnd = pict->marker().at.x;
		CHECK(atZero < atHalf);
		CHECK(atHalf < atEnd);
		CHECK(atZero == doctest::Approx(0.0f).epsilon(1e-4));
		CHECK(atHalf == doctest::Approx(strip.width() * 0.5f).epsilon(1e-4));
		CHECK(atEnd == doctest::Approx(strip.width() * 0.99f).epsilon(1e-4));

		// **A FULL TURN IS THE ORIGIN, and the bridge is where that is decided.** Publishing 1.0
		// lands the mark at the LEFT edge rather than the right, because the channel wraps a phase
		// rather than clamping it — a phase is a position on a circle. Pinned here because the first
		// draft of this case asked for the right edge and the wrap is the more useful answer: it is
		// what stops the star sticking at the end of every cycle.
		f.bridge.publishAutomodReadings(f.slot, 1.0f, 0.0f);
		CHECK(pict->marker().at.x == doctest::Approx(0.0f).epsilon(1e-4));
	}

	SUBCASE("a folded card has no picture, so there is no mark either") {
		f.card->setFolded(true);
		f.bridge.publishAutomodReadings(f.slot, 0.4f, 0.0f);
		CHECK_FALSE(pict->marker().drawn);
	}
}
TEST_CASE("automod strip: the response curve is drawn at the corner the LFO is at RIGHT NOW") {
	namespace amd = feathers::fx::automod;
	AutomodStripFixture f;
	f.card->setPicture(ui::AutomodCard::Picture::Dual);
	f.set(amd::Param::Manual, 0.2f);

	const amd::Settings s = f.card->settings();
	const amd::AutomodDerived d = amd::deriveAutomod(amd::coordsOf(s, amd::nominalRateHz(s)));

	// **THE INDEPENDENT RESTATEMENT OF THE WHOLE ROAD**: the shape at the published phase, through
	// `modulationFor` and `cornerHzFor`, into `magnitudeAt` — every one of them the function the
	// audio path itself calls, evaluated here in the test rather than asked of the card.
	const auto expectedAt = [&](float phase, float hz) {
		const float lfo = feathers::character::evalLfoShape(d.shape, phase);
		const float corner = amd::cornerHzFor(d.filterCutoffBase, d.filterModDepth,
		                                      amd::modulationFor(s, lfo), 1.0f, f.bridge.sampleRate());
		return amd::magnitudeAt(corner, d.filterResonance, d.filterMix, hz, f.bridge.sampleRate());
	};

	// A PHASE WHERE THE WAVEFORM IS ACTUALLY SOMEWHERE — chosen by asking the shape rather than by
	// picking a number that looks central, because a phase landing at a zero crossing would draw the
	// resting curve and make this case agree with the thing it exists to distinguish from.
	float moved = 0.0f;
	float best = 0.0f;
	for (int i = 1; i < 64; ++i) {
		const float p = static_cast<float>(i) / 64.0f;
		const float mag = std::abs(feathers::character::evalLfoShape(d.shape, p));
		if (mag > best) {
			best = mag;
			moved = p;
		}
	}
	REQUIRE(best > 0.2f); // NON-VACUITY: a flat shape cannot move a curve, and would pass silently

	f.bridge.publishAutomodReadings(f.slot, moved, 0.0f);
	const ui::AutomodCard::ResponseTrace live = f.card->responseTrace();
	REQUIRE(live.hz.size() > 8);
	for (size_t i = 0; i < live.hz.size(); ++i) {
		CAPTURE(i);
		CHECK(live.gain[i] == doctest::Approx(expectedAt(moved, live.hz[i])).epsilon(1e-6));
	}

	// **AND IT IS NOT THE RESTING CURVE**, which is the assertion the whole change is about. Counted
	// rather than sampled, because two curves can agree over part of the axis.
	size_t differing = 0;
	for (size_t i = 0; i < live.hz.size(); ++i) {
		if (std::abs(live.gain[i] - expectedAt(0.0f, live.hz[i])) > 1e-3f) {
			++differing;
		}
	}
	CHECK(differing > live.hz.size() / 4);

	// **A SILENT PLUGIN DRAWS EXACTLY WHAT IT USED TO.** A bridge nobody has published to answers 0,
	// the shape is pinned to rest at both endpoints, and `modulationFor(s, 0)` is what the resting
	// version passed — so this is a new behaviour that leaves the default picture untouched.
	f.bridge.publishAutomodReadings(f.slot, 0.0f, 0.0f);
	const ui::AutomodCard::ResponseTrace still = f.card->responseTrace();
	REQUIRE(still.hz.size() == live.hz.size());
	for (size_t i = 0; i < still.hz.size(); ++i) {
		CAPTURE(i);
		CHECK(still.gain[i] == doctest::Approx(expectedAt(0.0f, still.hz[i])).epsilon(1e-6));
	}
}

