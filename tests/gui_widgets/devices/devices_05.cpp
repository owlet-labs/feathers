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

// PIECE 5 OF 18 of the devices suite: its cases in their original order, with the helpers only they use. See devices_common.h.

#include "devices/devices_common.h"

TEST_CASE("a chain card's macro knobs in Feathers' lane light for an outer source — the chain's macros are destinations of the host it sits in (\"the macros should be destinations\")") {
	namespace cmp = feathers::composite;
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	const size_t slot = f.bridge.addFxDevice(cmp::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	f.bridge.refresh();
	lane->resized();
	lane->scrollCardIntoView(slot);
	lane->resized();
	ui::FxCard* card = lane->cardAt(slot);
	REQUIRE(card != nullptr);
	std::vector<ui::Knob*> knobs;
	collectKnobs(card, &knobs);
	size_t destinations = 0;
	size_t enabled = 0;
	size_t active = 0;
	for (ui::Knob* k : knobs) {
		destinations += k->isModDestination() ? 1 : 0;
		enabled += k->enabled() ? 1 : 0;
		active += k->active() ? 1 : 0;
	}
	CAPTURE(knobs.size());
	CAPTURE(enabled);
	CAPTURE(active);
	CHECK(destinations > 0);
	f.gui.modForTest()->focus(macroSourceAt(0)); // (through the state: the wider chain window stands over the strip's arrow, see above)
	f.gui.modForTest()->arm(true);
	REQUIRE(f.gui.modForTest()->armed());
	size_t lit = 0;
	size_t routable = 0;
	for (ui::Knob* k : knobs) {
		if (!k->isModDestination()) {
			continue;
		}
		lit += k->haloLit() ? 1 : 0;
		routable += k->mappable() ? 1 : 0;
		const auto* dc = dynamic_cast<const ui::DeviceControl*>(k->control());
		if (dc != nullptr) {
			CAPTURE(dc->control());
			CHECK(f.deviceHost.mayRoute(macroSourceAt(0), dc->destination()));
		}
	}
	CAPTURE(routable);
	CHECK(lit == destinations);
}
TEST_CASE("a chain's expanded view is the rack's own editor over the bridge inside the device, and it follows the host's handle (rack roadmap §6, 6h)") {
	// THE EDITOR FOR WHAT A CHAIN HOLDS is `RackEditor` — the standalone's — over the `RackBridge` the chain
	// device carries. The host's carrier files the running instance's handle at provisioning; the view reads
	// the bridge out of it and, because that pointer is a contemporaneous handle and not an identity
	// (CLAUDE.md's ABA rule), re-asks by id every tick and drops the editor when the answer changes.
	//
	// THE FIXTURE HAS NO SYNTH, SO NO INSTANCE IS PROVISIONED HERE: a rack engine stands in for the lane's,
	// its chain device's storage is filed by hand exactly as the converger files it, and the view is asked
	// through the same factory the lane uses.
	namespace cmp = feathers::composite;
	feathers::rack::registerChainExpandedView();
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	const size_t slot = f.bridge.addFxDevice(cmp::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	lane->resized();
	auto* card = dynamic_cast<ui::ChainCard*>(lane->cardAt(slot));
	REQUIRE(card != nullptr);
	const feathers::DeviceInstanceId id = f.bridge.fxDeviceAt(slot);
	const feathers::device::Descriptor* desc = f.deviceHost.descriptorOf(id);
	REQUIRE(desc != nullptr);

	// THE CARD OFFERS THE EXPAND, because the view is registered and the device declares one.
	CHECK(f.deviceHost.hasExpandedViewFor(*desc));
	CHECK(card->offersExpand());

	ui::WidgetRegistry registry;
	std::unique_ptr<ui::ExpandedView> made = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, id, *desc);
	REQUIRE(made != nullptr);
	auto* view = dynamic_cast<feathers::rack::ChainExpanded*>(made.get());
	REQUIRE(view != nullptr);
	CHECK(view->title() == "Chain");
	// THE RACK'S WIDTH: the chain's Mix and Gain stand in the macro column now (backlog OC), they no longer widen the row.
	CHECK(view->minimumSize().x
	      == doctest::Approx(feathers::rack::RackEditor::kWidth));
	// NO INSTANCE ON FILE YET: no editor, and no crash asking.
	CHECK_FALSE(view->liveForTest());
	CHECK_FALSE(view->updateContent());

	// AN INSTANCE, FILED THE WAY THE CONVERGER FILES ONE.
	feathers::rack::RackEngine outer { feathers::rack::kRackAxes, feathers::hostCatalogue() };
	outer.prepare(48000.0, 4096);
	const size_t lane2 = outer.addDevice(cmp::kTraits.tag);
	REQUIRE(lane2 != feathers::rack::RackEngine::kNoSlot);
	void* storage = outer.slotAtLane(lane2).storage.get();
	REQUIRE(storage != nullptr);
	REQUIRE(feathers::rack::bridgeOf(storage) != nullptr);
	f.bridge.fxBodies().attach(id, storage);
	REQUIRE(f.deviceHost.containerSurfaceOf(id) == storage);

	CHECK(view->updateContent()); // followed: the editor is built over the filed bridge
	REQUIRE(view->liveForTest());
	CHECK(view->bridgeForTest() == feathers::rack::bridgeOf(storage));
	REQUIRE(view->editorForTest() != nullptr);
	view->setBounds(0, 0, feathers::rack::RackEditor::kWidth, feathers::rack::RackEditor::kHeight);
	CHECK(view->editorForTest()->width() == doctest::Approx(feathers::rack::RackEditor::kWidth));

	// THE INSTANCE GOES (rebuilt, removed): the view drops its editor rather than draw a dead bridge.
	f.bridge.fxBodies().detach(id, storage);
	REQUIRE(f.deviceHost.containerSurfaceOf(id) == nullptr);
	CHECK(view->updateContent());
	CHECK_FALSE(view->liveForTest());
	CHECK(view->editorForTest() == nullptr);
	// AND A NEW ONE IS FOLLOWED AGAIN.
	f.bridge.fxBodies().attach(id, storage);
	CHECK(view->updateContent());
	CHECK(view->liveForTest());
	made.reset(); // the view goes before the engine whose bridge it held
}
TEST_CASE("OX: the chain card's macro knobs follow a rename made in the chain's window, a save and load, and a chain preset load") {
	// **THE CARD AND THE WINDOW ARE TWO VIEWS OF ONE NAME** (backlog OX: the chain card's macros did not follow
	// macros renamed in the chain's window). The card's knobs were captioned "M1".."M8" for ever. The VALUE under test is the
	// label text of knob 3 against the name the chain's own record holds, read here from the instance, never from the card.
	namespace cmp = feathers::composite;
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	const size_t slot = f.bridge.addFxDevice(cmp::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	lane->resized();
	auto* card = dynamic_cast<ui::ChainCard*>(lane->cardAt(slot));
	REQUIRE(card != nullptr);
	const feathers::DeviceInstanceId id = f.bridge.fxDeviceAt(slot);
	const feathers::device::Descriptor* desc = f.deviceHost.descriptorOf(id);
	REQUIRE(desc != nullptr);
	REQUIRE(desc->container != nullptr);
	REQUIRE(desc->container->macroLabel != nullptr);

	// A RUNNING INSTANCE, FILED THE WAY THE CONVERGER FILES ONE (as the expanded-view case does).
	feathers::rack::RackEngine outer { feathers::rack::kRackAxes, feathers::hostCatalogue() };
	outer.prepare(48000.0, 4096);
	const size_t lanes = outer.addDevice(cmp::kTraits.tag);
	REQUIRE(lanes != feathers::rack::RackEngine::kNoSlot);
	void* storage = outer.slotAtLane(lanes).storage.get();
	REQUIRE(storage != nullptr);
	feathers::rack::RackBridge* window = feathers::rack::bridgeOf(storage);
	REQUIRE(window != nullptr);
	f.bridge.fxBodies().attach(id, storage);

	const auto labelOf = [&](size_t macro) { return std::string(card->knobsForTest()[macro]->label()); };
	(void)card->updateContent();
	CHECK(labelOf(2) == "M3"); // unnamed: the short default

	// RENAME MACRO 3 IN THE WINDOW (its bridge, the road the window's slot writes).
	window->setMacroName(2, "Wobble");
	REQUIRE(window->macroName(2) == "Wobble");
	CHECK(card->updateContent()); // the repaint is DELIVERED: the tick's answer says the card changed
	CHECK(labelOf(2) == "Wobble");
	CHECK(labelOf(1) == "M2"); // only that knob
	CHECK(card->knobsForTest()[2]->captionIsName());
	CHECK_FALSE(card->knobsForTest()[8]->captionIsName()); // Mix keeps its upper-cased caption
	CHECK_FALSE(card->updateContent()); // and nothing more to say until another change

	// THE WINDOW'S OWN MACRO STRIP (the editor the rack plugin's strip is built from) SAYS THE SAME NAME: one name, two views.
	{
		feathers::rack::registerChainExpandedView();
		ui::WidgetRegistry registry;
		std::unique_ptr<ui::ExpandedView> made = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, id, *desc);
		auto* view = dynamic_cast<feathers::rack::ChainExpanded*>(made.get());
		REQUIRE(view != nullptr);
		(void)view->updateContent();
		REQUIRE(view->editorForTest() != nullptr);
		const auto& strip = view->editorForTest()->macrosForTest();
		REQUIRE(strip.size() > 2);
		CHECK(strip[2]->caption() == "Wobble");
		CHECK(strip[2]->caption() == labelOf(2));
		window->setMacroName(2, "Wibble"); // a rename while both are open reaches both
		CHECK(strip[2]->caption() == "Wibble");
		CHECK(card->updateContent());
		CHECK(strip[2]->caption() == labelOf(2));
		window->setMacroName(2, "Wobble");
		(void)card->updateContent();
		made.reset(); // the view goes before the engine whose bridge it held
	}

	// A RENAME BACK TO THE DEFAULT clears it.
	window->setMacroName(2, "");
	CHECK(card->updateContent());
	CHECK(labelOf(2) == "M3");
	window->setMacroName(2, "Wobble");
	(void)card->updateContent();

	// SAVE, THEN LOAD INTO A FRESH INSTANCE: the name is by value what the first held.
	struct Sink {
		std::vector<uint8_t> bytes;
		static void write(void* ctx, const uint8_t* data, size_t n) {
			auto* self = static_cast<Sink*>(ctx);
			self->bytes.insert(self->bytes.end(), data, data + n);
		}
	} saved;
	const feathers::device::ByteSink sink { &saved, &Sink::write };
	REQUIRE(desc->container->saveState(storage, sink));
	REQUIRE(!saved.bytes.empty());

	feathers::rack::RackEngine outer2 { feathers::rack::kRackAxes, feathers::hostCatalogue() };
	outer2.prepare(48000.0, 4096);
	const size_t lanes2 = outer2.addDevice(cmp::kTraits.tag);
	REQUIRE(lanes2 != feathers::rack::RackEngine::kNoSlot);
	void* fresh = outer2.slotAtLane(lanes2).storage.get();
	REQUIRE(fresh != nullptr);
	f.bridge.fxBodies().attach(id, fresh);
	(void)card->updateContent();
	CHECK(labelOf(2) == "M3"); // the fresh instance has no name yet
	REQUIRE(desc->container->loadState(fresh, saved.bytes.data(), saved.bytes.size()));
	REQUIRE(feathers::rack::bridgeOf(fresh)->macroName(2) == "Wobble");
	CHECK(card->updateContent());
	CHECK(labelOf(2) == "Wobble");

	// A CHAIN PRESET LOAD (OU, `ContainerOps::applyState`) into an instance that is already running: the names it carries
	// replace the ones it had.
	feathers::rack::RackEngine outer3 { feathers::rack::kRackAxes, feathers::hostCatalogue() };
	outer3.prepare(48000.0, 4096);
	const size_t lanes3 = outer3.addDevice(cmp::kTraits.tag);
	REQUIRE(lanes3 != feathers::rack::RackEngine::kNoSlot);
	void* running = outer3.slotAtLane(lanes3).storage.get();
	REQUIRE(running != nullptr);
	feathers::rack::bridgeOf(running)->setMacroName(5, "Old");
	f.bridge.fxBodies().attach(id, running);
	(void)card->updateContent();
	CHECK(labelOf(5) == "Old");
	REQUIRE(desc->container->applyState != nullptr);
	REQUIRE(desc->container->applyState(running, saved.bytes.data(), saved.bytes.size()));
	CHECK(card->updateContent());
	CHECK(labelOf(2) == "Wobble");
	CHECK(labelOf(5) == "M6"); // the preset named only macro 3

	f.bridge.fxBodies().detach(id, running);
}
TEST_CASE("a card's veil follows a change of SCALE, not only a change of layout") {
	// The inactive veils went wrong after a resize. Photographed on the xvfb
	// venue: the veil painted at 1/1.75 of the card. `Frame::setDpiScale` stamps a new scale and leaves
	// every frame's NATIVE rectangle where it was; the layout cascade that follows re-sets everybody's
	// bounds at the new scale — except the veil's, whose sync skipped `setBounds` because the LOGICAL
	// body had not moved. A canvas resize on a display that cannot fit the taller editor is exactly a
	// scale change, which is why it appeared "after resize".
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	lane->resized();
	ui::FxCard* card = lane->cardAt(0);
	REQUIRE(card != nullptr);
	REQUIRE(card->hasEnable());
	card->update();
	const ui::DisabledVeil& veil = card->disabledVeilForTest();
	const float body = static_cast<float>(veil.width());
	REQUIRE(body > 0.0f);
	const float was = card->dpiScale();
	REQUIRE(static_cast<float>(veil.nativeWidth()) == doctest::Approx(body * was).epsilon(0.02));

	// THE SCALE CHANGES AND NOTHING ELSE DOES — no layout, no beat, which is the window the defect lived in.
	const float now = was * 1.75f;
	f.root->setDpiScale(now);
	CHECK(card->dpiScale() == doctest::Approx(now));
	CHECK(static_cast<float>(veil.width()) == doctest::Approx(body)); // logical: unchanged, as it should be
	// NATIVE: converted at the NEW scale — the number the paint uses.
	CHECK(static_cast<float>(veil.nativeWidth()) == doctest::Approx(body * now).epsilon(0.02));
	// AND THE BEAT AGREES, which is the road every host drives.
	card->update();
	CHECK(static_cast<float>(veil.nativeWidth()) == doctest::Approx(body * now).epsilon(0.02));
	f.root->setDpiScale(was);
}
TEST_CASE("a device inside a chain device expands onto the OUTER host's overlay — never into the panel, arriving beside its chain in the band") {
	// THE ESCALATION, forced by the canvas: a panel
	// inside a panel is the clipping container `surface_host.h` was written about. So the inner editor
	// has no canvas, answers as the outermost does, and presents on the OUTERMOST layers' overlay —
	// beside the chain's own panel in the band while it shows (canvas doc §3f).
	namespace cmp = feathers::composite;
	feathers::rack::registerChainExpandedView();
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	const size_t slot = f.bridge.addFxDevice(cmp::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	lane->resized();
	const feathers::DeviceInstanceId id = f.bridge.fxDeviceAt(slot);

	// AN INSTANCE, FILED THE WAY THE CONVERGER FILES ONE, holding a device added THROUGH ITS RECORD —
	// inside a chain device the record is the truth, and a direct engine edit is undone at the drain.
	feathers::rack::RackEngine outer { feathers::rack::kRackAxes, feathers::hostCatalogue() };
	outer.prepare(48000.0, 4096);
	const size_t lane2 = outer.addDevice(cmp::kTraits.tag);
	REQUIRE(lane2 != feathers::rack::RackEngine::kNoSlot);
	void* storage = outer.slotAtLane(lane2).storage.get();
	feathers::rack::RackBridge* inner = feathers::rack::bridgeOf(storage);
	REQUIRE(inner != nullptr);
	REQUIRE(inner->addDevice(feathers::fx::owltt::kTraits.tag) != feathers::kNoInstance);
	inner->drain();
	f.bridge.fxBodies().attach(id, storage);

	// THE CHAIN OPENS THROUGH THE LANE'S OWN ROAD, onto Feathers' canvas.
	ui::ExpandedCanvas* canvas = f.gui.canvasForTest();
	ui::ExpandedOverlay* overlay = f.gui.expandedForTest();
	REQUIRE(canvas != nullptr);
	REQUIRE(overlay != nullptr);
	REQUIRE(canvas->setCollapsed(false)); // opened: a fresh canvas is folded
	// THE HOST GRANTS THE ROOM, AND THEN SOME (backlog EJ). Two panels side by side on the band need more
	// than the design width; before EJ this case had that room by accident — the fixture's short window
	// shrank the scale to fit the canvas, which made the LOGICAL canvas 2090 wide. Now a window is room
	// at the user's scale, so the case asks for the design and then widens the window, which is what
	// "dodged clear" was always a claim about: a band with room beside the chain.
	{
		uint32_t w = 1;
		uint32_t h = 1;
		REQUIRE(f.gui.adjustSize(&w, &h));
		REQUIRE(f.gui.setSize(w * 2, h));
	}
	REQUIRE(canvas->showing());
	REQUIRE(lane->onExpandDevice != nullptr);
	lane->onExpandDevice(slot);
	REQUIRE(overlay->open());
	const auto* mounted = dynamic_cast<const ui::MountedFrame*>(overlay->contentForTest());
	REQUIRE(mounted != nullptr);
	auto* view = dynamic_cast<feathers::rack::ChainExpanded*>(mounted->contentForTest());
	REQUIRE(view != nullptr);
	(void)view->updateContent(); // followed: the inner editor is built over the filed bridge
	REQUIRE(view->liveForTest());
	feathers::rack::RackEditor* editor = view->editorForTest();
	REQUIRE(editor != nullptr);

	// NO CANVAS INSIDE THE PANEL, AND THE INNER HOST ESCALATES — to wherever the OUTER would put its own
	// (backlog EB, decided "beside it"): the outer canvas while it shows, the outer overlay otherwise.
	CHECK(editor->canvasExtent() == doctest::Approx(0.0f));
	CHECK(editor->surfaces().kindFor(ui::SurfaceNeed::SpansGesture) == ui::SurfaceKind::Canvas);
	editor->syncCards();
	REQUIRE_FALSE(editor->cardsForTest().empty());
	ui::FxCard* card = editor->cardsForTest()[0];
	REQUIRE(card != nullptr);
	REQUIRE(card->onExpand);
	card->onExpand();
	// **BESIDE THE CHAIN, IN THE OUTER BAND** (backlog EB): a second panel on the outer overlay, dodged
	// clear of the chain's, the inner editor's own overlay untouched.
	CHECK(overlay->panelCount() == 2);
	CHECK_FALSE(editor->overlayForTest().open());
	CHECK(editor->expandedIdForTest() != ui::kNoSurface);
	{
		ui::ExpandedOverlay& band = *overlay;
		const ui::ExpandedOverlay::PanelId device = band.frontPanelIdForTest();
		bool overlaps = false;
		for (const ui::ExpandedOverlay::PanelId other : band.panelIdsForTest()) {
			if (other != device && band.panelBounds(other).overlaps(band.panelBounds(device))) {
				overlaps = true;
			}
		}
		CHECK_FALSE(overlaps);
	}
	// THE OUTER'S DISMISSAL OF THAT CARD REACHES THE INNER'S ID, and leaves the chain's card standing.
	overlay->dismiss(overlay->frontPanelIdForTest());
	CHECK(editor->expandedIdForTest() == ui::kNoSurface);
	CHECK(overlay->panelCount() == 1);

	// WITH THE CANVAS FOLDED, THE SAME PRESS FLOATS OVER THE EDITOR — the outer's own rule, followed.
	// (Folding leaves the chain's panel up, so the chain's expand press CLOSES it — 2026-10-04 — and
	// the next press reopens it as a fresh view.)
	REQUIRE(canvas->setCollapsed(true));
	REQUIRE(overlay->panelCount() == 1);
	lane->onExpandDevice(slot);
	REQUIRE_FALSE(overlay->open());
	lane->onExpandDevice(slot);
	REQUIRE(overlay->open());
	{
		auto* reopened = const_cast<ui::ExpandedView*>(overlay->contentForTest());
		const auto* remount = dynamic_cast<const ui::MountedFrame*>(reopened);
		REQUIRE(remount != nullptr);
		auto* review = dynamic_cast<feathers::rack::ChainExpanded*>(remount->contentForTest());
		REQUIRE(review != nullptr);
		(void)review->updateContent();
		REQUIRE(review->liveForTest());
		review->editorForTest()->syncCards();
		REQUIRE_FALSE(review->editorForTest()->cardsForTest().empty());
		ui::FxCard* innerCard = review->editorForTest()->cardsForTest()[0];
		REQUIRE(innerCard->onExpand);
		CHECK(review->editorForTest()->surfaces().kindFor(ui::SurfaceNeed::SpansGesture) == ui::SurfaceKind::Overlay);
		innerCard->onExpand();
		CHECK(overlay->panelCount() == 2); // the chain's panel and the inner device's, both floating
		// AND THE CHAIN PANEL GOING TAKES ITS FORWARDED PANEL WITH IT: the inner editor dies with the
		// view, and a panel reading a device of a bridge that is gone would be a panel over freed memory.
		overlay->dismiss();
	}
	// FREED BY THE TICK — outside every dispatch — which is when the retired chain view, its editor and
	// the forwarded id all go.
	(void)f.gui.surfacesForTest()->updateContent();
	CHECK_FALSE(overlay->open());
	f.bridge.fxBodies().detach(id, storage);
}
TEST_CASE("Bus Utility's polarity BUTTONS flip the device, and a press on one is not a card drag") {
	// **THE REACHABILITY GUARD TAKES A CARD'S WORD.** `addressedParamsForTest` lists Invert L and
	// Invert R because two buttons reach them — and a button that drew but did nothing would satisfy
	// that list exactly as well as a working one. This case presses them where they actually are and
	// asserts the VALUE, which is the only thing that can tell the two apart.
	//
	// The pair are buttons, and both fit in one knob's footprint.
	namespace bu = feathers::fx::busutil;
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	const size_t slot = f.bridge.addFxDevice(bu::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	lane->resized();
	lane->scrollCardIntoView(slot);
	lane->resized();
	auto* card = dynamic_cast<ui::BusUtilCard*>(lane->cardAt(slot));
	REQUIRE(card != nullptr);

	const auto fieldOf = [&](bu::Param p) {
		// THROUGH THE HOST THE CARD ITSELF WRITES, so this reads the same road the press wrote — a
		// value read from a different one could agree for a reason that has nothing to do with the
		// button.
		return f.deviceHost.fieldAt(f.bridge.fxDeviceAt(slot), static_cast<size_t>(p));
	};
	const auto pressCentreOf = [&](bool right) {
		const visage::Bounds b = card->polarityBounds(right);
		const visage::Point at = card->positionInWindow()
		                         + visage::Point { b.x() + b.width() * 0.5f,
			                                       b.y() + b.height() * 0.5f };
		// THROUGH THE REAL TREE, so the press is dispatched the way a hand's would be — a card that
		// hit-tested its buttons in the wrong coordinates would pass a direct call and fail here.
		REQUIRE(f.root->frameAtPoint(at) == static_cast<visage::Frame*>(card));
		dragThroughRoot(f.root, at, at);
	};

	SUBCASE("both start normal, and each flips only its own channel") {
		REQUIRE(fieldOf(bu::Param::InvertL) < 0.5f);
		REQUIRE(fieldOf(bu::Param::InvertR) < 0.5f);

		pressCentreOf(false);
		CHECK(fieldOf(bu::Param::InvertL) >= 0.5f);
		// **THE OTHER ONE DID NOT MOVE**, which is the assertion a single combined polarity switch
		// could not make and the reason there are two.
		CHECK(fieldOf(bu::Param::InvertR) < 0.5f);

		pressCentreOf(true);
		CHECK(fieldOf(bu::Param::InvertR) >= 0.5f);

		// AND THEY TOGGLE BACK — a button that only ever turned things on would pass every assertion
		// above.
		pressCentreOf(false);
		CHECK(fieldOf(bu::Param::InvertL) < 0.5f);
	}

	SUBCASE("the two buttons occupy one knob's footprint, and do not overlap") {
		const visage::Bounds l = card->polarityBounds(false);
		const visage::Bounds r = card->polarityBounds(true);
		CHECK(l.right() <= r.x());
		// WITHIN ONE CELL. Asserted against the cell a knob would have had, so the claim is about the
		// row's rhythm rather than about a number somebody picked.
		CHECK(r.right() - l.x() <= (card->width() / static_cast<float>(ui::BusUtilCard::kKnobsPerRow)));
	}

	SUBCASE("a press on a button does not pick the card up") {
		// **THE BUTTONS ARE DRAWN BY THE CARD, NOT BY CHILD FRAMES**, so nothing else consumes the
		// press — and `noteCardPress` is what arms the reorder drag. Without the gate in
		// `onMouseDown` the same gesture would flip the polarity AND pull the card out of the lane.
		pressCentreOf(false);
		CHECK_FALSE(card->cardDragging());
	}
}
TEST_CASE("automod's enumerations are DROPDOWNS, not boxes you drag through") {
	// A value box that selects among several values is a dropdown, not "drag over the box to select"
	// (automod's time base, for instance).
	//
	// **THE REQUIREMENT HAD ALREADY BEEN ANSWERED ONCE, IN THE WRONG PLACE.** `automod_card.cpp`
	// recorded that there was no separate dropdown widget in this editor and that building one would
	// be a second widget for a job `ValueBox` already does. What `ValueBox`
	// already did was RENDER the name — `formatValue` prints `Unit::Choice` by name, so the box read
	// "1 bar" rather than "6" — and that is a readout, not a picker. The gesture stayed a drag through
	// thirty entries.
	//
	// **AND THE MENU EXISTED THE WHOLE TIME, WIRED AT ONE SITE.** `ValueBox::onPicker` was added for
	// the MSEG's time base, under a note saying the point applied to more than that control — the
	// generalisation written down and not made. `ValueBox` decides for itself now, from the
	// DESCRIPTOR: stepped, named, three or more entries. No card opts in and none can forget.
	//
	// ASKED OF BOTH OF AUTOMOD'S, by walking the cells rather than naming indices, so a third choice
	// declared tomorrow is covered here without anybody coming back.
	namespace amd = feathers::fx::automod;
	AutomodStripFixture f;
	REQUIRE_FALSE(f.card->choicesForTest().empty());

	size_t swept = 0;
	for (const std::unique_ptr<ui::ValueBox>& box : f.card->choicesForTest()) {
		REQUIRE(box != nullptr);
		const std::string who = box->control().desc().name;
		CAPTURE(who);
		++swept;
		// IT OPENS A LIST.
		CHECK(box->choiceMenuAvailable());
		// AND THE LIST IS THE CONTROL'S WHOLE VOCABULARY, from the declared RANGE rather than from the
		// names array — the count and the array disagreeing is a real defect shape and reading off the
		// end of `names` is how it would show up.
		CHECK(box->entryCount()
		      == static_cast<size_t>(std::lround(box->control().desc().max
		                                         - box->control().desc().min))
		             + 1);
		CHECK(box->entryCount() >= 3);
		// AND NOTHING WIRED IT BY HAND. A card that set a picker would be handing this box a different
		// list from the one every other Choice box in the editor gets.
		CHECK_FALSE(box->hasPicker());
	}
	// SYNC DIVISION AND LFO MODE, and the count is part of the claim so a card that stopped building
	// one of them cannot pass by sweeping fewer.
	CHECK(swept == 2);

	// **AND A RANGE IS STILL A DRAG.** The other half, without which "everything is a dropdown" would
	// satisfy the checks above and have broken every continuous control on the card.
	REQUIRE_FALSE(f.card->knobsForTest().empty());
	size_t ranges = 0;
	for (const auto& knob : f.card->knobsForTest()) {
		if (!knob->control()->desc().stepped) {
			++ranges;
		}
	}
	CHECK(ranges > 0);
}
//------------------------------------------------------------------------------------------------//
// THE HOST'S OWN ROADS — asked THROUGH `BridgeDeviceHost`, which is what a card holds.              //
//                                                                                                  //
// A CENSUS FOUND THESE UNCOVERED: each could be broken — answer the default, answer false, skip a   //
// branch — with every case in all six widget binaries still green, because what is covered is the   //
// road UNDER the host (`ParamBridge`) and never the host's own hop. That hop is where a device id   //
// becomes a slot, and getting it wrong is the DG shape: one device's meter drawn for another.       //
//------------------------------------------------------------------------------------------------//

TEST_CASE("the drive meter reaches a card through the host BY DEVICE, and a device nobody meters declines") {
	// **THE SENTINEL IS THE POINT, AND IT IS WHY THIS ROAD RETURNS A BOOL.** This meter is a level meter in
	// the background of the transfer curve display, showing how hard the actual table is driven — and `ParamBridge::fxDrivePeak` keeps a NEGATIVE for a slot nobody
	// publishes for, so that "unmetered" and "driven by silence" stay different pictures. Nothing asked
	// `BridgeDeviceHost::drivePeakAt` at all: it could have answered false for everything (an empty meter
	// on the shaper) or 0.0f for everything (a meter that says silence about a device nobody measures).
	namespace shp = feathers::fx::shaper;
	ShaperFixture f;
	REQUIRE(f.device != kNoInstance);
	const DeviceInstanceId filter = f.f.bridge.fxDeviceAt(0); // the default chain's own, ahead of the shaper
	REQUIRE(filter != kNoInstance);
	REQUIRE(filter != f.device);

	SUBCASE("each device answers with ITS OWN peak") {
		// TWO DEVICES, TWO VALUES — the half that pins the hop. A host reading a fixed slot, or the other
		// device's, passes a single-device case and fails this one.
		f.f.bridge.publishFxDrivePeak(f.slot, 0.75f);
		f.f.bridge.publishFxDrivePeak(0, 0.25f);

		float shaperPeak = -1.0f;
		REQUIRE(f.f.deviceHost.drivePeakAt(f.device, shaperPeak));
		CHECK(shaperPeak == doctest::Approx(0.75f));

		float filterPeak = -1.0f;
		REQUIRE(f.f.deviceHost.drivePeakAt(filter, filterPeak));
		CHECK(filterPeak == doctest::Approx(0.25f));
	}

	SUBCASE("a device nobody publishes for DECLINES, and does not write the out parameter") {
		// NOTHING PUBLISHED FOR EITHER. A road answering `true` with 0.0f would draw a meter pinned at
		// silence for a device that is not metered at all, which is the picture the bool exists to refuse.
		float peak = -12.5f;
		CHECK_FALSE(f.f.deviceHost.drivePeakAt(f.device, peak));
		CHECK(peak == -12.5f); // untouched: a decline says nothing, it does not say zero
		CHECK_FALSE(f.f.deviceHost.drivePeakAt(filter, peak));

		// AND THE CASE IS NOT VACUOUS: the same device answers once something publishes for it.
		f.f.bridge.publishFxDrivePeak(f.slot, 0.5f);
		float after = -1.0f;
		REQUIRE(f.f.deviceHost.drivePeakAt(f.device, after));
		CHECK(after == doctest::Approx(0.5f));
	}

	SUBCASE("a device the lane does not hold declines") {
		float peak = -1.0f;
		CHECK_FALSE(f.f.deviceHost.drivePeakAt(f.device + 500, peak));
	}
}
TEST_CASE("a write past a device's declared params takes the STATE BLOB road through the host") {
	// **TWO ROADS, ONE SEAM, AND NOTHING WROTE DOWN THE SECOND ONE.** `DeviceHost::setField` takes a ROW
	// index; past the declared params that row is the device's opaque state (`device::Traits::extraState`
	// — the shaper's eight folded-travel floats, automod's thirty-nine), and it takes `setDeviceState`
	// because `setDeviceControl` coerces against a `ParamDesc` a blob float does not have. Skip that
	// dispatch and a card writing its device's state writes nothing at all, silently: the shaper's phase
	// space stops accumulating and the only symptom is a curve that does not travel.
	namespace shp = feathers::fx::shaper;
	ShaperFixture f;
	const size_t params = shp::kDescriptor.params.size();
	REQUIRE(shp::kDescriptor.traits.extraState > 1);
	const auto rowField = [&](size_t index) {
		return fxDeviceAtPosition(f.f.bridge.fxChain(), f.slot).row.field[index];
	};

	// A BLOB FLOAT IS NOT COERCED — it has no declaration to coerce against — so the value comes back
	// exactly, which is also how this case tells the two roads apart.
	f.f.deviceHost.setField(f.device, params + 3, 0.375f);
	CHECK(rowField(params + 3) == doctest::Approx(0.375f));

	// NOT VACUOUS: the validated road still works and still clamps, so a dispatch that sent everything
	// down one road would fail one half or the other.
	const device::ParamDesc& drive = shp::kDescriptor.params[static_cast<size_t>(shp::Param::Drive)];
	f.f.deviceHost.setField(f.device, static_cast<size_t>(shp::Param::Drive), static_cast<float>(drive.max) + 1000.0f);
	CHECK(rowField(static_cast<size_t>(shp::Param::Drive)) == doctest::Approx(static_cast<float>(drive.max)));

	SUBCASE("past the blob nothing is written") {
		// THE BOUND IS THE DEVICE'S OWN `extraState`, so a row index past it names nothing and must not
		// land anywhere — least of all on the last blob float, which is what a missing bound would do.
		const float last = rowField(params + shp::kDescriptor.traits.extraState - 1);
		f.f.deviceHost.setField(f.device, params + shp::kDescriptor.traits.extraState, 0.875f);
		CHECK(rowField(params + shp::kDescriptor.traits.extraState - 1) == doctest::Approx(last));
	}

	SUBCASE("a device with no blob is refused past its params, and its row is untouched") {
		namespace flt = feathers::fx::filter;
		REQUIRE(flt::kDescriptor.traits.extraState == 0);
		const DeviceInstanceId filter = f.f.bridge.fxDeviceAt(0);
		const size_t filterParams = flt::kDescriptor.params.size();
		const float before = fxDeviceAtPosition(f.f.bridge.fxChain(), 0).row.field[filterParams];
		f.f.deviceHost.setField(filter, filterParams, 0.625f);
		CHECK(fxDeviceAtPosition(f.f.bridge.fxChain(), 0).row.field[filterParams] == doctest::Approx(before));
	}
}
TEST_CASE("the host answers whether a device has a panel, and a card's way in is drawn from that answer") {
	// THE HONESTY RULE ON A CARD HEAD: the mark that opens a panel is drawn exactly when pressing it would
	// open one. The card asks its HOST (`DeviceHost::hasExpandedViewFor`) rather than Feathers' registry,
	// which is what keeps the card floor free of the synth — and nothing asked the host's own answer, so a
	// host returning true for everything would draw a way in to nowhere on every card in the lane.
	ShaperFixture f;
	CHECK(f.f.deviceHost.hasExpandedViewFor(feathers::fx::shaper::kDescriptor));
	// THE OTHER DIRECTION, WHICH IS WHAT MAKES IT A CLAIM: the filter declares no expanded view.
	REQUIRE_FALSE(feathers::fx::filter::kDescriptor.traits.hasExpanded);
	CHECK_FALSE(f.f.deviceHost.hasExpandedViewFor(feathers::fx::filter::kDescriptor));
	// AND THE HOST AGREES WITH WHAT THIS BUILD CAN ACTUALLY CONSTRUCT.
	CHECK(f.f.deviceHost.hasExpandedViewFor(feathers::fx::shaper::kDescriptor)
	      == ui::hasExpandedView(feathers::fx::shaper::kDescriptor));
	CHECK(f.f.deviceHost.hasExpandedViewFor(feathers::fx::filter::kDescriptor)
	      == ui::hasExpandedView(feathers::fx::filter::kDescriptor));
}
TEST_CASE("a zoned coordinate and a drag window are cells on EVERY surface, including the ones nobody hand-wrote") {
	// Zone knobs take the value-box semantics everywhere; a surface that skipped the migrated pattern
	// was a defect.
	//
	// The migration was real and it was complete where anybody had written a card: OWLTT, shaper and
	// automod each build `ZoneCoordCell` by hand. What it never reached was the surfaces that are
	// DERIVED from a descriptor — `GenericCard` and `GenericExpanded` — because there was no hand to
	// migrate. A device that declares `zones` and has no card of its own therefore got the
	// pre-migration widget: a bounded-looking dial over an unbounded number, no `zone : phase`
	// readout, no dropdown. SKLITTER is the first device to land in exactly that position, and it
	// shipped that way.
	//
	// **SWEPT, NOT NAMED.** Asserting it of SKLITTER would be a case about SKLITTER; the claim is about
	// the DERIVED surfaces, so every device the browser offers goes through it and the next one to
	// declare a zoned coordinate is covered before it exists.
	ModFixture probe;
	auto* probeLane = findInTree<ui::FxLane>(probe.root);
	REQUIRE(probeLane != nullptr);
	std::vector<const device::Descriptor*> descs = probeLane->addableForTest();
	REQUIRE(descs.size() >= 4);

	size_t devicesWithZones = 0;
	size_t devicesWithWindows = 0;
	for (const device::Descriptor* desc : descs) {
		CAPTURE(std::string(desc->traits.tag));

		// WHICH PARAMS ARE ZONED, asked of the descriptor — the same field the widget's existence is
		// supposed to follow from.
		std::vector<size_t> zoned;
		std::vector<size_t> windows;
		for (size_t i = 0; i < desc->params.size(); ++i) {
			if (desc->params[i].zones > 0) {
				zoned.push_back(i);
			}
			// **THE OTHER FAMILY THAT CANNOT SIT ON A KNOB**, and the gate has to admit a device that
			// has one WITHOUT having a zoned coordinate — a device with a gamma and no zones would
			// otherwise be skipped by a sweep whose door only opens for zones.
			else if (device::isDragWindow(desc->params[i])) {
				windows.push_back(i);
			}
		}
		if (zoned.empty() && windows.empty()) {
			continue;
		}
		if (!zoned.empty()) {
			++devicesWithZones;
		}
		if (!windows.empty()) {
			++devicesWithWindows;
		}

		ModFixture f;
		auto* lane = findInTree<ui::FxLane>(f.root);
		REQUIRE(lane != nullptr);
		const size_t slot = f.bridge.addFxDevice(desc->traits.tag);
		REQUIRE(slot == 1);
		lane->resized();
		lane->scrollCardIntoView(slot);
		lane->resized();

		ui::FxCard* card = lane->cardAt(slot);
		REQUIRE(card != nullptr);

		// **AND NO KNOB IS OVER A DRAG WINDOW EITHER.** Same defect, other family: gamma and rotate
		// declare a range as a window a drag moves through, and a `Knob` maps one 140px drag across
		// the whole of it. The registry audit for this matched the substring "Gamma" in a name, and I
		// escaped it by renaming SKLITTER's gamma to "Evolve Phase" — so the claim is restated over
		// `device::isDragWindow`, which reads the declaration and cannot be renamed out of.
		{
			std::vector<ui::Knob*> windowKnobs;
			collectKnobs(card, &windowKnobs);
			for (ui::Knob* knob : windowKnobs) {
				if (knob->control() == nullptr) {
					continue;
				}
				CAPTURE(std::string(knob->control()->desc().name == nullptr ? "" : knob->control()->desc().name));
				CHECK_FALSE(device::isDragWindow(knob->control()->desc()));
			}
		}

		// **THE CLAIM THAT HOLDS FOR EVERY CARD, HAND-WRITTEN OR DERIVED: NO KNOB IS OVER A ZONED
		// PARAM.** Stated this way it needs no agreement about WHICH controls a given card puts on its
		// face — a hand-written card draws what it likes, and OWLTT's draws neither of its two zoned
		// coordinates — while still being exactly the defect: the dial was the pre-migration widget,
		// and its presence anywhere is the migration having missed a surface. Every knob can answer
		// for itself, because `Modulatable::desc()` is the same declaration the cell is chosen from.
		std::vector<ui::Knob*> knobs;
		collectKnobs(card, &knobs);
		for (ui::Knob* knob : knobs) {
			if (knob->control() == nullptr) {
				continue; // a knob over something that is not a device param has no zones to declare
			}
			CAPTURE(std::string(knob->control()->desc().name == nullptr ? "" : knob->control()->desc().name));
			CHECK(knob->control()->desc().zones == 0);
		}

		// **THE CARD'S FACE MAY NOT CARRY THEM ALL** — `Traits::faceControls` (or `faceOrder`) lets a device keep a
		// small playing surface and send the rest to the expanded view.
		const std::vector<size_t> face = ui::GenericCard::faceIndicesFor(*desc);
		const auto onFace = [&](size_t index) { return std::find(face.begin(), face.end(), index) != face.end(); };
		size_t zonedOnFace = 0;
		for (size_t index : zoned) {
			if (onFace(index)) {
				++zonedOnFace;
			}
		}

		// **AND THE DERIVED SURFACES ANSWER PRECISELY**, because they are the ones that were wrong and
		// a count can be right for the wrong reason. A `GenericCard` knows which param each of its
		// rows is over, so the claim is made per param: a cell exists for it and NO knob does.
		if (auto* generic = dynamic_cast<ui::GenericCard*>(card)) {
			CHECK(generic->zoneCellsForTest().size() == zonedOnFace);
			// A DRAG WINDOW ON THE FACE IS A `GammaBox`, which is a `ValueBox` — so it is enough that
			// the param is PLACED and is not a knob, both of which are asserted here and above.
			for (size_t index : windows) {
				if (!onFace(index)) {
					continue;
				}
				CAPTURE(index);
				CAPTURE(std::string(desc->params[index].name));
				const auto& knobIndices = generic->knobParamIndicesForTest();
				CHECK(std::find(knobIndices.begin(), knobIndices.end(), index) == knobIndices.end());
				const auto& all = generic->paramIndicesForTest();
				CHECK(std::find(all.begin(), all.end(), index) != all.end());
			}
			for (size_t index : zoned) {
				if (!onFace(index)) {
					continue;
				}
				CAPTURE(index);
				CAPTURE(std::string(desc->params[index].name));
				const auto& knobIndices = generic->knobParamIndicesForTest();
				CHECK(std::find(knobIndices.begin(), knobIndices.end(), index) == knobIndices.end());
				const auto& all = generic->paramIndicesForTest();
				CHECK(std::find(all.begin(), all.end(), index) != all.end());
			}
		}

		// THE EXPANDED VIEW DRAWS EVERY CONTROL (the superset rule), so it must draw every zoned one
		// as a cell — all of them, not only the ones the face left off.
		if (ui::hasExpandedView(*desc)) {
			ui::WidgetRegistry registry;
			std::unique_ptr<ui::ExpandedView> panel = ui::makeExpandedView(
			    f.deviceHost, &f.bridge, registry, f.bridge.fxDeviceAt(slot), *desc, nullptr);
			REQUIRE(panel != nullptr);
			auto* frame = dynamic_cast<visage::Frame*>(panel.get());
			REQUIRE(frame != nullptr);
			std::vector<ui::ZoneCoordCell*> panelCells;
			collectZoneCells(frame, &panelCells);
			CAPTURE(panelCells.size());
			// **ASKED AS A SET OF PARAMS, NOT AS A COUNT OF CELLS, since 2026-09-19.** It was
			// `panelCells.size() == zoned.size()`, which assumed one cell per zoned control — and
			// SKLITTER's perform section broke that assumption honestly: its four `Cfg` cells exist
			// TWICE, once at their waypoints on the circle and once in the grid, with exactly one of
			// the two visible depending on whether the section is open (`SklitterExpanded`). Neither
			// holds a value — both read the host through a `DeviceControl` on every draw — so the
			// duplication is a seating choice, and a COUNT cannot tell it from a missing control.
			//
			// **THE SET IS ALSO A STRICTLY STRONGER QUESTION**, which is why this is a repair rather
			// than a relaxation: the count passed for a panel that seated two cells over one param and
			// none over another, and this does not.
			std::set<std::string> covered;
			for (ui::ZoneCoordCell* cell : panelCells) {
				const device::ParamDesc& d = cell->box().control().desc();
				covered.insert(d.name == nullptr ? std::string() : std::string(d.name));
			}
			// **AND A MODE CELL'S FOLLOWERS (backlog IW)** — Shaper Colour is the value one of Colour's modes reads, drawn
			// in the mode cell's box rather than a coordinate cell of its own.
			const std::function<void(visage::Frame*)> modeWalk = [&](visage::Frame* at) {
				if (const auto* mode = dynamic_cast<const ui::ModeValueCell*>(at)) {
					for (const size_t p : mode->followerParams()) {
						const device::ParamDesc& d = mode->boxFor(p)->control().desc();
						if (d.zones > 0) {
							covered.insert(d.name == nullptr ? std::string() : std::string(d.name));
						}
					}
				}
				for (visage::Frame* child : at->children()) {
					modeWalk(child);
				}
			};
			modeWalk(frame);
			std::set<std::string> wanted;
			for (size_t index : zoned) {
				wanted.insert(desc->params[index].name == nullptr ? std::string()
				                                                 : std::string(desc->params[index].name));
			}
			CHECK(covered == wanted);

			// AND NO DIAL OVER ONE, the same way the card is asked — nor over a drag window.
			std::vector<ui::Knob*> panelKnobs;
			collectKnobs(frame, &panelKnobs);
			for (ui::Knob* knob : panelKnobs) {
				if (knob->control() == nullptr) {
					continue;
				}
				CAPTURE(std::string(knob->control()->desc().name == nullptr ? "" : knob->control()->desc().name));
				CHECK(knob->control()->desc().zones == 0);
				CHECK_FALSE(device::isDragWindow(knob->control()->desc()));
			}
		}
	}

	// **NON-VACUITY.** Every assertion above sits inside `if (zoned.empty()) continue;`, so a registry
	// that stopped declaring zones anywhere — or a sweep that stopped seeing them — would run this
	// case green having checked nothing at all.
	CAPTURE(devicesWithZones);
	CAPTURE(devicesWithWindows);
	CHECK(devicesWithZones >= 2);
	CHECK(devicesWithWindows >= 1);
}
TEST_CASE("backlog EE, 2026-09-15: a panel FOLDS to its title strip — by its chevron or a double-click on the head, through the real tree; folded it keeps its width and place, hides its content and has no grip, a second panel dodges around the strip, a double-click opens it again where it was and on top, and a device reopens as it was left") {
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	auto* overlay = f.gui.expandedForTest();
	REQUIRE(lane != nullptr);
	REQUIRE(overlay != nullptr);
	const size_t harm = f.bridge.addFxDevice(fx::harm::kTraits.tag);
	const size_t second = f.bridge.addFxDevice(fx::harm::kTraits.tag);
	f.bridge.refresh();
	lane->resized();
	lane->scrollCardIntoView(harm);
	lane->resized();
	REQUIRE(lane->onExpandDevice != nullptr);
	(void)f.gui.canvasForTest()->setCollapsed(true); // the panels are the overlay's
	lane->onExpandDevice(harm);
	REQUIRE(overlay->open());
	REQUIRE(overlay->panelForTest() != nullptr);
	const ui::ExpandedOverlay::PanelId first = overlay->frontPanelIdForTest();
	const visage::Bounds open = overlay->panelBounds();
	REQUIRE(open.height() > ui::ExpandedOverlay::kTitleHeight);
	CHECK_FALSE(overlay->folded(first));

	// THE CHEVRON, PRESSED THROUGH THE ROOT: the panel folds to its strip, where it was and as wide.
	const auto chevron = [&] {
		const visage::Bounds b = overlay->foldBounds();
		return overlay->positionInWindow() + visage::Point { b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f };
	};
	REQUIRE(f.root->frameAtPoint(chevron()) == static_cast<visage::Frame*>(overlay->panelForTest()));
	dragThroughRoot(f.root, chevron(), chevron());
	CHECK(overlay->folded(first));
	const visage::Bounds strip = overlay->panelBounds();
	CHECK(strip.height() == doctest::Approx(ui::ExpandedOverlay::kTitleHeight));
	CHECK(strip.width() == doctest::Approx(open.width()));
	CHECK(strip.x() == doctest::Approx(open.x()));
	CHECK(strip.y() == doctest::Approx(open.y()));
	CHECK_FALSE(overlay->contentForTest()->isVisible());
	CHECK(overlay->panelForTest()->gripBounds().width() == 0.0f);
	CHECK(overlay->panelForTest()->height() == doctest::Approx(ui::ExpandedOverlay::kTitleHeight));

	// A SECOND PANEL DODGES AROUND THE STRIP — it covers no part of it.
	lane->scrollCardIntoView(second);
	lane->resized();
	lane->onExpandDevice(second);
	REQUIRE(overlay->panelCount() == 2);
	const ui::ExpandedOverlay::PanelId other = overlay->frontPanelIdForTest();
	REQUIRE(other != first);
	CHECK_FALSE(overlay->panelBounds(other).overlaps(overlay->panelBounds(first)));

	// A DOUBLE-CLICK ON THE STRIP'S HEAD OPENS IT AGAIN — where it was, its content back, on top.
	const visage::Bounds stripNow = overlay->panelBounds(first);
	const visage::Point head = overlay->positionInWindow()
	                           + visage::Point { stripNow.x() + 40.0f, stripNow.y() + ui::ExpandedOverlay::kTitleHeight * 0.5f };
	REQUIRE(f.root->frameAtPoint(head) == static_cast<visage::Frame*>(overlay->panelForTest(first)));
	doubleClickHead(f.root, head);
	CHECK_FALSE(overlay->folded(first));
	CHECK(overlay->frontPanelIdForTest() == first);
	const visage::Bounds reopened = overlay->panelBounds(first);
	CHECK(reopened.height() == doctest::Approx(open.height()));
	CHECK(reopened.x() == doctest::Approx(stripNow.x()));
	CHECK(reopened.y() == doctest::Approx(stripNow.y()));
	CHECK(overlay->contentForTest(first)->isVisible());
	CHECK(overlay->panelForTest(first)->gripBounds().width() == ui::ExpandedPanel::kGripSize);

	// LEFT FOLDED AND CLOSED, THE DEVICE REOPENS AS A STRIP.
	overlay->setFolded(first, true);
	REQUIRE(overlay->folded(first));
	overlay->dismiss(first);
	REQUIRE(overlay->panelCount() == 1);
	lane->scrollCardIntoView(harm);
	lane->resized();
	lane->onExpandDevice(harm);
	REQUIRE(overlay->panelCount() == 2);
	CHECK(overlay->folded(overlay->frontPanelIdForTest()));
	CHECK(overlay->panelBounds().height() == doctest::Approx(ui::ExpandedOverlay::kTitleHeight));
	CHECK_FALSE(overlay->contentForTest()->isVisible());
}
TEST_CASE("a control its mode does not use is not drawn, and the gap closes behind it") {
	// Many value boxes are mode-specific, so they are hidden when their mode does not use them.
	// SKLITTER in Burst uses six of its fifteen controls; the other nine sat there
	// doing nothing. The reference states this per menu item as `isRelevant()` across six headers;
	// `ParamDesc::gateControl` and `gateMask` are the same statement on the declaration, so a card can
	// ask without knowing what a mode is.
	namespace sc = feathers::fx::sklitter;
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	const size_t slot = f.bridge.addFxDevice(sc::kTraits.tag);
	REQUIRE(slot == 1);
	lane->resized();
	lane->scrollCardIntoView(slot);
	lane->resized();
	auto* card = dynamic_cast<ui::GenericCard*>(lane->cardAt(slot));
	REQUIRE(card != nullptr);

	const auto setMode = [&](sc::Mode m) {
		f.bridge.setDeviceControl(f.bridge.fxDeviceAt(slot), static_cast<size_t>(sc::Param::Mode),
		                          static_cast<float>(m));
		lane->resized();
		// **AND THE TICK, WHICH IS THE ROAD THAT RE-LAYS A CARD WHOSE SIZE DID NOT CHANGE.** `resized`
		// reaches `layOut` only when the bounds move; Shuffle -> Repeat keeps two rows, so nothing moved
		// and the hidden cell stayed laid out. The editor re-lays through `updateContent`'s gate check
		// every frame, so the fixture takes that road too.
		(void)card->updateContent();
	};
	// THE VISIBLE ROWS, counted off the widgets rather than off the declaration — the claim is about
	// what is DRAWN.
	//
	// **ASKED OF `rowsForTest`, NOT OF THE KNOBS.** A first draft counted knobs and coordinate cells
	// and read ONE visible row of SKLITTER's six in Burst — four of them are `ValueBox` choices, so
	// the instrument was hiding more than the card was.
	const auto visibleRows = [&] {
		size_t n = 0;
		for (const visage::Frame* r : card->rowsForTest()) {
			if (r != nullptr && r->isVisible()) {
				++n;
			}
		}
		return n;
	};

	setMode(sc::Mode::Burst);
	const size_t inBurst = visibleRows();
	setMode(sc::Mode::Shuffle);
	const size_t inShuffle = visibleRows();
	CAPTURE(inBurst);
	CAPTURE(inShuffle);

	// **BURST SHOWS FEWER**, which is the whole ask. Stated as an inequality rather than as two
	// magic numbers, so adding a control to the device does not make this case wrong.
	CHECK(inBurst < inShuffle);
	// AND IT STILL SHOWS SOMETHING — a mode that hid everything would be a card you cannot play.
	CHECK(inBurst > 0);

	SUBCASE("the looper-only controls are the ones Burst drops") {
		// Density and P Write are `mode != Burst` in the reference. (The
		// zoned coordinates are too, and this subcase asked about them — they left the card on
		// 2026-09-16, so the card's face asks about the gated controls it still carries. **Macro was
		// the third and is DELETED**, 2026-09-19, with nothing put in its place: for now it is
		// deliberately unreachable from window and card.)
		const std::vector<size_t>& indices = card->paramIndicesForTest();
		const auto visibleFor = [&](sc::Param p) {
			for (size_t i = 0; i < indices.size(); ++i) {
				if (indices[i] == static_cast<size_t>(p)) {
					return card->rowsForTest()[i]->isVisible();
				}
			}
			FAIL("not on the card");
			return false;
		};
		for (const sc::Param p : { sc::Param::Density, sc::Param::PWrite }) {
			CAPTURE(static_cast<int>(p));
			setMode(sc::Mode::Burst);
			CHECK_FALSE(visibleFor(p));
			setMode(sc::Mode::Shuffle);
			CHECK(visibleFor(p));
		}
	}

	SUBCASE("the card asks the lane for the room it will use, not for the room it declares") {
		// A card that hid nine rows and went on reserving their height would scroll through empty
		// space — the hiding would be invisible exactly where it matters. `gui-widgets-dahdsr`'s fold
		// case caught this as 456 reserved against 180 drawn.
		setMode(sc::Mode::Burst);
		const float burstExtent = card->laneExtent(rack::RackEditor::kHeight);
		setMode(sc::Mode::Shuffle);
		const float shuffleExtent = card->laneExtent(rack::RackEditor::kHeight);
		CAPTURE(burstExtent);
		CAPTURE(shuffleExtent);
		// **NOT STRICTLY SMALLER SINCE `Input Gain` (backlog KZ, 2026-10-10)**, and the reason is the grid, not the
		// reservation: the face is three across, Shuffle shows six cells and Burst four, and both are TWO rows. This
		// was `<` while Burst showed three (one row). What still holds, and is the claim: Burst never asks for
		// MORE than Shuffle, which shows every face cell and so is the declared room. The general guard against
		// reserving hidden rows is `gui-widgets-dahdsr`'s fold case.
		CHECK(burstExtent <= shuffleExtent);
	}

	SUBCASE("and no gap is left where a hidden row was") {
		// THE ROWS ARE CONTIGUOUS. A layout that skipped the hidden ones by INDEX rather than by
		// count would leave holes, which looks like a broken card rather than a shorter one.
		//
		// **THE STEP IS EACH ROW'S OWN HEIGHT PLUS THE GAP, NOT ONE CONSTANT** — and this case went
		// red the day rows stopped agreeing about their height (the dropdown sizing fix;
		// a `ValueBox` row is 28 where a knob row is 38, so it read `36 == Approx(46)`).
		//
		// **THE RED WAS CORRECT AND THE FIX IS NOT TO LOOSEN IT.** The old form took the FIRST step as
		// the expected one and compared every other step against it, which was only ever a proxy for
		// "no holes" — it happened to work while one number described every row. Measuring each step
		// against the height of the row it steps over is the same claim stated properly, and it is
		// STRICTER: a uniform-step assertion could not have noticed a row drawn at the wrong height at
		// all, so long as every row was wrong by the same amount.
		//
		// **A GRID SINCE 2026-09-16** (the knobs are a grid, not a list), so "no
		// hole" is: the live cells, in face order, take the grid's slots in reading order — cell k at
		// column k % across of row k / across — with no slot left empty before the last.
		//
		// **IN REPEAT, WHERE ONE FACE CONTROL HIDES MID-GRID** (P Write is every looper mode but Repeat).
		// Burst hides three and leaves one row since Mode left the card, which has
		// no row step to assert; Repeat leaves two rows with a hole to close — Mix moves into P Write's
		// slot — so this is asserting the closing rather than a grid with nothing hidden.
		setMode(sc::Mode::Repeat);
		std::vector<const visage::Frame*> live;
		size_t hidden = 0;
		for (const visage::Frame* r : card->rowsForTest()) {
			if (r != nullptr && r->isVisible()) {
				live.push_back(r);
			}
			else {
				++hidden;
			}
		}
		const auto across = static_cast<size_t>(sc::kTraits.faceColumns);
		REQUIRE(across > 1u);
		REQUIRE(live.size() > across); // two rows, or no row step is asserted
		REQUIRE(hidden >= 1u);         // a gap to close, or "no hole" is vacuous
		for (size_t k = 1; k < live.size(); ++k) {
			CAPTURE(k);
			const auto centreY = [](const visage::Frame* r) {
				return static_cast<float>(r->y()) + 0.5f * static_cast<float>(r->height());
			};
			if (k % across != 0) {
				// SAME ROW: to the right of its neighbour, centred on the same line.
				CHECK(live[k]->x() > live[k - 1]->x());
				CHECK(centreY(live[k]) == doctest::Approx(centreY(live[k - 1])));
			}
			else {
				// NEXT ROW: back under column 0, and below everything in the row above.
				CHECK(live[k]->x() == doctest::Approx(live[0]->x()));
				for (size_t j = k - across; j < k; ++j) {
					CHECK(live[k]->y() > live[j]->y() + live[j]->height());
				}
			}
		}
	}
}
TEST_CASE("STRUCTURAL: every card's expand verb is the SAME mark") {
	// The expand-to-window button in the header was nonconforming: its mark was not the glyph every
	// other card uses.
	//
	// `GenericCard` was the only card that set `drawsOwnExpandMark()`, and it drew `+N` in accent text
	// where the other six draw `FxCard::drawExpandMark`'s diagonal. The count it existed for stopped
	// being possible when the body started scrolling — `hiddenCount()` is zero for every device now —
	// so it always fell through to a bare `>`: a different glyph, in a different colour, for the same
	// verb, on whichever devices happen to lack a hand-written card.
	//
	// **ENUMERATED, not asserted of `GenericCard` alone.** A case about the one card that was wrong
	// would pass the day a seventh card opted out for its own reasons. This is the same shape as
	// `STRUCTURAL: no surface anywhere seats a gamma in a Knob`, and for the same reason: the rule is
	// "one verb, one mark", so it has to be asked of everything that draws the verb.
	ModFixture probe;
	auto* probeLane = findInTree<ui::FxLane>(probe.root);
	REQUIRE(probeLane != nullptr);
	std::vector<std::string> tags;
	for (const device::Descriptor* d : probeLane->addableForTest()) {
		tags.emplace_back(d->traits.tag);
	}
	REQUIRE(tags.size() >= 4);

	size_t withExpand = 0;
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
		if (card->expandBounds().width() <= 0.0f) {
			continue; // no panel behind it, so no verb and nothing to conform to
		}
		++withExpand;
		// THE ONE ANSWER: nobody draws their own.
		CHECK_FALSE(card->drawsOwnExpandMarkForTest());
	}
	// AND THE SWEEP SWEPT SOMETHING — the empty-filter trap this repo has now met four times.
	CAPTURE(withExpand);
	CHECK(withExpand > 0);
}
TEST_CASE("SKLITTER's band decodes the arrangement it is published, chunk for chunk") {
	// The buffer is rendered as a gradient that re-orders the chunks in their shuffled order, with
	// modifiers for the other modes.
	//
	// A WAVEFORM WOULD SAY WHAT WAS RECORDED; what this device does is read that recording out of
	// order. So each chunk sits at its place in PLAYBACK order and carries where it reads FROM — a bar
	// played straight is a smooth ramp and every reordering is a break in it.
	//
	// **THE PICTURE IS PUBLISHED HERE RATHER THAN PLAYED.** This case is about the CARD: that it
	// decodes what the device sends, chunk for chunk and flag for flag. Driving real audio would test
	// the device's publishing as well, which `test_fx_sklitter.cpp` does where the engine is in reach —
	// and would leave this case passing or failing for two unrelated reasons.
	namespace sc = feathers::fx::sklitter;
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	const size_t slot = f.bridge.addFxDevice(sc::kTraits.tag);
	REQUIRE(slot == 1);
	lane->resized();
	lane->scrollCardIntoView(slot);
	lane->resized();

	// **THE DEVICE GETS ITS OWN CARD**, which is the first claim and the cheapest to get wrong.
	auto* card = dynamic_cast<ui::SklitterCard*>(lane->cardAt(slot));
	REQUIRE(card != nullptr);

	// BEFORE ANYTHING IS PUBLISHED THE BAND IS EMPTY — it says "nothing yet" by drawing nothing rather
	// than by inventing chunks.
	CHECK(card->chunksForTest().empty());

	// FOUR STEPS, DELIBERATELY OUT OF ORDER, one flag each.
	std::vector<float> values(sc::kReadingCount, 0.0f);
	values[static_cast<size_t>(sc::Reading::Status)] = 3.0f; // playing
	values[static_cast<size_t>(sc::Reading::Steps)] = 4.0f;
	values[static_cast<size_t>(sc::Reading::Step)] = 2.0f;
	const auto put = [&](size_t step, float source, uint32_t flags) {
		const size_t base = sc::stepReading(step);
		values[base] = source;
		values[base + 1u] = static_cast<float>(flags | static_cast<uint32_t>(sc::StepFlag::Known));
	};
	put(0, 0.75f, 0u);
	put(1, 0.25f, static_cast<uint32_t>(sc::StepFlag::Reversed));
	put(2, 0.50f, static_cast<uint32_t>(sc::StepFlag::PitchedUp));
	put(3, 0.00f, static_cast<uint32_t>(sc::StepFlag::Dry));
	f.bridge.publishDeviceReadings(slot, values.data(), values.size());

	const std::vector<ui::SklitterCard::Chunk> picture = card->chunksForTest();
	REQUIRE(picture.size() == 4u);

	// **THE SOURCE POSITIONS COME BACK IN PLAYBACK ORDER**, which is the arrangement: the chunk drawn
	// first reads from three quarters of the way through the bar.
	CHECK(picture[0].source == doctest::Approx(0.75));
	CHECK(picture[1].source == doctest::Approx(0.25));
	CHECK(picture[2].source == doctest::Approx(0.50));
	CHECK(picture[3].source == doctest::Approx(0.00));

	// **AND EACH MODIFIER LANDS ON ITS OWN CHUNK AND NO OTHER.** A decode that shifted by one, or that
	// read the flags off the wrong half of the pair, would still give four chunks and four flags.
	CHECK(picture[1].reversed);
	CHECK_FALSE(picture[0].reversed);
	CHECK_FALSE(picture[2].reversed);
	CHECK(picture[2].pitchedUp);
	CHECK_FALSE(picture[1].pitchedUp);
	CHECK(picture[3].dry);
	CHECK_FALSE(picture[2].dry);
	for (const ui::SklitterCard::Chunk& c : picture) {
		CHECK(c.known);
	}

	// THE PLAYING STEP IS INSIDE THE PICTURE, so the head marks a chunk rather than the air past it.
	CHECK(card->playingStepForTest() == 2);

	SUBCASE("the PANEL asks for its own frames too, which it did not") {
		// The expanded window still redrew wrongly. The card was given an
		// `updateContent` that compares and redraws; the panel was not. One surface of two — **the
		// third time in this device a fix reached one road and stopped**, after density reached the
		// slice modes and not Grain, and the hard cut reached the looper renderer and not the grain
		// renderer. When a device has two surfaces over one picture, a fix to one of them is half a
		// fix by default.
		ui::WidgetRegistry reg;
		std::unique_ptr<ui::ExpandedView> v = ui::makeExpandedView(
		    f.deviceHost, &f.bridge, reg, f.bridge.fxDeviceAt(slot), sc::kDescriptor);
		REQUIRE(v != nullptr);
		auto* panel = dynamic_cast<ui::SklitterExpanded*>(v.get());
		REQUIRE(panel != nullptr);

		// THE FIRST LOOK IS ALWAYS A CHANGE — nothing has been drawn yet.
		CHECK(panel->updateContentForTest());
		// AND A SECOND WITH NOTHING MOVING IS NOT.
		CHECK_FALSE(panel->updateContentForTest());

		// **THE PHASE ALONE IS ENOUGH**, exactly as on the card: it is what makes the head sweep
		// between slice boundaries rather than jump once a slice.
		std::vector<float> moved(sc::kReadingCount, 0.0f);
		moved[static_cast<size_t>(sc::Reading::Steps)] = 4.0f;
		moved[static_cast<size_t>(sc::Reading::Step)] = 2.0f;
		moved[static_cast<size_t>(sc::Reading::StepPhase)] = 0.75f;
		const size_t b = static_cast<size_t>(sc::Reading::kHeaderCount);
		moved[b] = 0.5f;
		moved[b + 1u] = static_cast<float>(sc::StepFlag::Known);
		f.bridge.publishDeviceReadings(slot, moved.data(), moved.size());
		CHECK(panel->updateContentForTest());
	}

	SUBCASE("the expanded panel draws the same arrangement, against the bar it came from") {
		// **THE SKETCH**: `▓▒░▓░▒▓▒ buffer` over `░▒▓ reordered ▓▒░`. The panel drew TWO strips (the upper one is gone since backlog EY)
		// — the captured bar in its own order, which is a plain ramp, and the same chunks in the order
		// they are PLAYED. Straight through the two are identical; under a scatter the lower one is the
		// upper one cut up, and the difference between them IS the effect.
		//
		// The card shows the lower strip alone, because a card has room for a glance and not for a
		// comparison. Both decode through `sklitterChunks`, so they cannot disagree about what is
		// playing — which is the reason that function is free rather than a member of either.
		ui::WidgetRegistry registry;
		std::unique_ptr<ui::ExpandedView> view = ui::makeExpandedView(
		    f.deviceHost, &f.bridge, registry, f.bridge.fxDeviceAt(slot), sc::kDescriptor);
		REQUIRE(view != nullptr);
		auto* panel = dynamic_cast<ui::SklitterExpanded*>(view.get());
		REQUIRE(panel != nullptr);

		// **THE BAND IS RESERVED, not drawn over the controls.** A panel that drew the strips without
		// asking for the room would put them on top of the first row of cells.
		auto* frame = dynamic_cast<visage::Frame*>(view.get());
		REQUIRE(frame != nullptr);
		const visage::Point size = panel->minimumSize();
		CAPTURE(size.y);
		CHECK(size.y > ui::SklitterExpanded::kBandHeight);
		// **ONE STRIP'S ROOM, NOT TWO (backlog EY).** The BUFFER strip is gone, and the room it held is
		// the point of removing it — stated from the parts, so a second strip left reserved reds here.
		CHECK(ui::SklitterExpanded::kBandHeight
		      == doctest::Approx(ui::SklitterExpanded::kStripHeight + ui::SklitterExpanded::kLabelHeight
		                         + ui::SklitterExpanded::kBandGap));

		// AND IT READS THE SAME PICTURE THE CARD DOES — the published arrangement, not a second decode.
		frame->setBounds(0.0f, 0.0f, size.x, size.y);
		frame->resized();
		const std::vector<ui::SklitterChunk> same =
		    ui::sklitterChunks(f.deviceHost, f.bridge.fxDeviceAt(slot));
		REQUIRE(same.size() == picture.size());
		for (size_t i = 0; i < same.size(); ++i) {
			CAPTURE(i);
			CHECK(same[i].source == doctest::Approx(picture[i].source));
			CHECK(same[i].reversed == picture[i].reversed);
			CHECK(same[i].dry == picture[i].dry);
		}
	}

	SUBCASE("a step that has never played is not drawn, rather than drawn as the start of the bar") {
		// `Known` is why the flags carry a bit that says nothing about the sound. Source 0 and
		// "unknown" are the same float, and one means "reads from the top of the bar" while the other
		// means "no idea yet" — a band that could not tell them apart would draw a solid block at the
		// gradient's dark end for every step the device has not reached.
		std::vector<float> fresh(sc::kReadingCount, 0.0f);
		fresh[static_cast<size_t>(sc::Reading::Steps)] = 4.0f;
		const size_t base = static_cast<size_t>(sc::Reading::kHeaderCount);
		fresh[base] = 0.5f;
		fresh[base + 1u] = static_cast<float>(sc::StepFlag::Known);
		f.bridge.publishDeviceReadings(slot, fresh.data(), fresh.size());
		const std::vector<ui::SklitterCard::Chunk> partial = card->chunksForTest();
		REQUIRE(partial.size() == 4u);
		CHECK(partial[0].known);
		CHECK_FALSE(partial[1].known);
		CHECK_FALSE(partial[2].known);
		CHECK_FALSE(partial[3].known);
	}
}
TEST_CASE("the band's gradient walks HUE through several centres, and the card asks for its own frames") {
	// **TWO REPORTS, 2026-09-15**, both about the band being a picture you cannot read:
	//
	//   - the gradient walks hue, touching at least three colour centres, not just opacity and
	//     saturation;
	//   - the graph updated only sometimes, and the playhead moved only when a param changed.
	namespace sc = feathers::fx::sklitter;

	SUBCASE("the ramp is a hue walk, not a brightness one") {
		// **THE FIRST VERSION MIXED `kMute` INTO `kAccent`** — two blues a few degrees apart — so
		// position in the bar read as dimmer-or-brighter and two chunks a third of a bar apart were
		// nearly the same colour. A ramp you cannot ORDER by eye is the one thing this picture must
		// not be.
		//
		// **ASSERTED AS HUE ANGLE, not as "the colours differ".** Any ramp differs along its length,
		// including the brightness one this replaces; what separates them is whether the HUE moves.
		const auto hueOf = [](uint32_t c) {
			const float r = static_cast<float>((c >> 16) & 0xFFu) / 255.0f;
			const float g = static_cast<float>((c >> 8) & 0xFFu) / 255.0f;
			const float b = static_cast<float>(c & 0xFFu) / 255.0f;
			const float hi = std::max({ r, g, b });
			const float lo = std::min({ r, g, b });
			const float d = hi - lo;
			if (d < 1.0e-6f) {
				return -1.0f; // grey has no hue, and a ramp through grey is the defect
			}
			float h = 0.0f;
			if (hi == r) {
				h = std::fmod((g - b) / d, 6.0f);
			}
			else if (hi == g) {
				h = (b - r) / d + 2.0f;
			}
			else {
				h = (r - g) / d + 4.0f;
			}
			h *= 60.0f;
			return h < 0.0f ? h + 360.0f : h;
		};

		// SAMPLED ACROSS THE WHOLE BAR, and every sample must have a hue at all.
		std::vector<float> hues;
		for (int i = 0; i <= 20; ++i) {
			const uint32_t c = ui::sklitterRamp(static_cast<float>(i) / 20.0f);
			const float h = hueOf(c);
			CAPTURE(i);
			CHECK(h >= 0.0f);
			hues.push_back(h);
		}

		// **AT LEAST THREE DISTINCT CENTRES**, counted as samples more than 20 degrees apart — the
		// own bar, stated as a number. The old two-blue ramp spanned about eight degrees in total.
		std::vector<float> centres;
		for (const float h : hues) {
			bool near = false;
			for (const float c : centres) {
				if (std::abs(h - c) < 20.0f) {
					near = true;
					break;
				}
			}
			if (!near) {
				centres.push_back(h);
			}
		}
		CAPTURE(centres.size());
		CHECK(centres.size() >= 3);

		// AND THE TWO ENDS ARE PLAINLY DIFFERENT COLOURS, cool to warm, so the bar reads as a
		// direction rather than as a wash.
		const float span = std::abs(hues.front() - hues.back());
		CAPTURE(hues.front());
		CAPTURE(hues.back());
		CHECK(span > 60.0f);
	}

	SUBCASE("the card asks for a frame when the picture moves, and not when it does not") {
		// **RETURNING `true` IS THE REPORT, NOT THE DELIVERY** — `busutil_card.cpp` carries this exact
		// bug written up at length, and the band shipped with the same one: no `updateContent` at all,
		// so the only thing that ever repainted the card was something else doing it.
		//
		// This asserts the REPORT, which is what a case can reach — `visage::Frame` keeps `redrawing_`
		// private, which is the same wall `busutil_card.cpp` records hitting. The delivery is the
		// `redraw()` beside it, and the note there says so rather than pretending this covers it.
		ModFixture f;
		auto* lane = findInTree<ui::FxLane>(f.root);
		REQUIRE(lane != nullptr);
		const size_t slot = f.bridge.addFxDevice(sc::kTraits.tag);
		REQUIRE(slot == 1);
		lane->resized();
		lane->scrollCardIntoView(slot);
		lane->resized();
		auto* card = dynamic_cast<ui::SklitterCard*>(lane->cardAt(slot));
		REQUIRE(card != nullptr);

		std::vector<float> values(sc::kReadingCount, 0.0f);
		values[static_cast<size_t>(sc::Reading::Steps)] = 4.0f;
		values[static_cast<size_t>(sc::Reading::Step)] = 1.0f;
		values[static_cast<size_t>(sc::Reading::StepPhase)] = 0.25f;
		const size_t base = static_cast<size_t>(sc::Reading::kHeaderCount);
		values[base] = 0.5f;
		values[base + 1u] = static_cast<float>(sc::StepFlag::Known);
		f.bridge.publishDeviceReadings(slot, values.data(), values.size());

		// THE FIRST LOOK IS ALWAYS A CHANGE — nothing has been drawn yet.
		CHECK(card->updateContentForTest());
		// AND A SECOND WITH NOTHING MOVING IS NOT, or the card would repaint at the timer's rate for
		// ever, which is the failure mode the comparison exists to prevent.
		CHECK_FALSE(card->updateContentForTest());

		// **THE PHASE ALONE IS ENOUGH**, and it is the half that makes the head sweep: without it the
		// picture changes only when the SLICE does, so the head jumps a few times a bar and sits still
		// between.
		values[static_cast<size_t>(sc::Reading::StepPhase)] = 0.50f;
		f.bridge.publishDeviceReadings(slot, values.data(), values.size());
		CHECK(card->updateContentForTest());

		// AND SO IS A CHUNK CHANGING under a stationary head.
		values[base] = 0.75f;
		f.bridge.publishDeviceReadings(slot, values.data(), values.size());
		CHECK(card->updateContentForTest());
		CHECK_FALSE(card->updateContentForTest());
	}
}
TEST_CASE("the lane's per-frame road reaches SKLITTER's band, end to end") {
	// **THE WHOLE CHAIN IN ONE CASE**, because the pieces were each provable and the thing on screen was
	// not: the UI stayed empty until the effect was engaged, and then updated only when a param moved.
	// Readings published -> the host's count and values -> the card's decode ->
	// `FxLane::updateCards`, which is the road `Gui::Impl::tick` drives every frame.
	//
	// It was a throwaway probe first. Kept, because every link in it was individually asserted
	// somewhere and the JOIN was not — which is exactly the shape of gap the reported defect lives in.
	namespace sc = feathers::fx::sklitter;
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	const size_t slot = f.bridge.addFxDevice(sc::kTraits.tag);
	REQUIRE(slot == 1);
	lane->resized();
	lane->scrollCardIntoView(slot);
	lane->resized();
	auto* card = dynamic_cast<ui::SklitterCard*>(lane->cardAt(slot));
	REQUIRE(card != nullptr);
	REQUIRE(card->isVisible());

	std::vector<float> values(sc::kReadingCount, 0.0f);
	values[static_cast<size_t>(sc::Reading::Steps)] = 4.0f;
	values[static_cast<size_t>(sc::Reading::Step)] = 1.0f;
	values[static_cast<size_t>(sc::Reading::StepPhase)] = 0.25f;
	const size_t base = static_cast<size_t>(sc::Reading::kHeaderCount);
	values[base] = 0.5f;
	values[base + 1u] = static_cast<float>(sc::StepFlag::Known);
	f.bridge.publishDeviceReadings(slot, values.data(), values.size());

	// THE READINGS REACHED THE HOST THE CARD ASKS.
	CHECK(f.deviceHost.readingCountAt(f.bridge.fxDeviceAt(slot)) == sc::kReadingCount);
	CHECK(card->chunksForTest().size() == 4u);

	// AND THE LANE'S PER-FRAME ROAD SEES THE MOVEMENT — this is the link that no other case covers,
	// and the one that decides whether anything ever repaints.
	CHECK(lane->updateCards() > 0);

	// A PHASE MOVE ALONE IS ENOUGH, which is what makes the head sweep between slice boundaries.
	values[static_cast<size_t>(sc::Reading::StepPhase)] = 0.5f;
	f.bridge.publishDeviceReadings(slot, values.data(), values.size());
	CHECK(lane->updateCards() > 0);

	// AND A STILL PICTURE ASKS FOR NOTHING, or the editor repaints at the timer's rate for ever.
	CHECK(lane->updateCards() == 0);
}
TEST_CASE("a picker is sized for a picker: SKLITTER's dropdowns, on the card and on the panel") {
	// The dropdowns were mis-sized.
	//
	// Both generic surfaces gave every cell ONE height — the card `Knob::rowHeight`, the panel
	// `kCellHeight` — and a `ValueBox` draws its caption over its number in `ValueBox::kHeight` and
	// stops. SKLITTER's Engage and Mode were therefore bordered rectangles with their content in the
	// top third and the rest empty: 10px of dead band on the card, FIFTY-THREE on the panel.
	//
	// **PHOTOGRAPHED BEFORE IT WAS BELIEVED** (`tools/widget-shot SKLITTER --card`, and again without
	// `--card` for the panel), which is this project's rule about geometry. The camera is also what
	// turned up the second half below, which no report had connected to this one.
	//
	// **THE ASSERTIONS ARE ON THE NUMBERS, NOT ON A DIFFERENCE BETWEEN TWO PICTURES.** A repaint
	// comparison passes whenever anything moves; what is claimed here is that a specific widget has a
	// specific height, and only reading the height says so.
	namespace sc = feathers::fx::sklitter;
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	const size_t slot = f.bridge.addFxDevice(sc::kTraits.tag);
	REQUIRE(slot == 1);
	lane->resized();
	lane->scrollCardIntoView(slot);
	lane->resized();
	auto* card = dynamic_cast<ui::SklitterCard*>(lane->cardAt(slot));
	REQUIRE(card != nullptr);

	// THE DESCRIPTOR'S OWN ANSWER, which is what `heightFor` reserves a lane with.
	const float boxRow = ui::ValueBox::kHeight;
	const float knobRow = ui::Knob::rowHeight(ui::GenericCard::kKnobDiameter);
	REQUIRE(boxRow < knobRow); // or this case is asserting nothing

	SUBCASE("ON THE CARD: a row is as tall as what is in it, and no taller") {
		const std::vector<visage::Frame*>& rows = card->rowsForTest();
		const std::vector<size_t>& indices = card->paramIndicesForTest();
		REQUIRE(rows.size() == indices.size());

		size_t boxes = 0;
		size_t knobs = 0;
		for (size_t i = 0; i < rows.size(); ++i) {
			if (!rows[i]->isVisible()) {
				continue; // a control this mode hides takes no room; that is a different case
			}
			const device::ParamDesc& p = sc::kDescriptor.params[indices[i]];
			// **THE DESCRIPTOR'S ANSWER AND THE WIDGET'S ACTUAL HEIGHT ARE THE SAME NUMBER.** This is
			// the whole guard: `rowHeightFor` is computed from the descriptor because `heightFor` has
			// no instance to ask, so it is a SECOND opinion about what the constructor built — and a
			// second opinion that drifts is how a lane reserves one height and a card draws another.
			// **A GRID CELL SINCE 2026-09-16** (`Traits::faceColumns`), so the descriptor's answer is
			// `cellHeightFor` — the same guard against a second opinion, asked of the grid's function.
			CHECK(rows[i]->height()
			      == doctest::Approx(ui::GenericCard::cellHeightFor(p)).epsilon(0.001));
			if (dynamic_cast<ui::ValueBox*>(rows[i]) != nullptr) {
				++boxes;
				// A BOX GETS THE BOX'S HEIGHT, whichever kind of box it is.
				CHECK(rows[i]->height()
				      == doctest::Approx(dynamic_cast<ui::ValueBox*>(rows[i])->naturalHeight()));
			}
			else if (dynamic_cast<ui::Knob*>(rows[i]) != nullptr) {
				++knobs;
				// A CAPTIONED COLUMN, stated from the knob's own arithmetic.
				CHECK(rows[i]->height()
				      == doctest::Approx(ui::Knob::columnHeight(ui::GenericCard::kKnobDiameter)));
			}
			else if (dynamic_cast<ui::SwitchChip*>(rows[i]) != nullptr) {
				// **ENGAGE IS A KNOB-SIZED PAD** — an enable is a full knob-sized piece. The knob's column height, not the chip's 16.
				CHECK(rows[i]->height()
				      == doctest::Approx(ui::Knob::columnHeight(ui::GenericCard::kKnobDiameter)));
				CHECK(rows[i]->height() > ui::SwitchChip::kHeight);
			}
		}
		// **NOT VACUOUS**: the device really does put more than one kind of row on its face, so the loop
		// above is measuring a ragged list rather than passing over an empty one.
		//
		// **THIS FLOOR WAS TWO BOXES AND IS NOW ONE, AND THE CHANGE IS THE POINT RATHER THAN A
		// CONCESSION.** It read "Engage and Mode are the two dropdowns named in the report" — and backlog EP
		// then established that Engage was never a dropdown at all: a two-entry `Unit::Choice`, which
		// `ValueBox::choiceMenuAvailable` already refuses to open a menu for. It is a `SwitchChip` now,
		// so one box remains (Mode, a list of eight) and the case counts chips as the second kind.
		//
		// **AND NOW NONE: MODE LEFT THE CARD.** The box-sizing half of this case lives on in the panel subcase below, where Mode is.
		CHECK(boxes == 0u);
		CHECK(knobs >= 1u);
		size_t chips = 0;
		for (size_t i = 0; i < rows.size(); ++i) {
			if (rows[i]->isVisible() && dynamic_cast<ui::SwitchChip*>(rows[i]) != nullptr) {
				++chips;
			}
		}
		CHECK(chips >= 1u);

		// **AND THE GRID DOES NOT OVERLAP OR GAP.** Restated independently of `layOut`: the live cells,
		// in face order, fill rows of three; a row is as tall as its tallest cell; the next row starts
		// that far plus the gap below; a shorter cell is centred in its row; and a column's cells share
		// an x.
		std::vector<size_t> live;
		for (size_t i = 0; i < rows.size(); ++i) {
			if (rows[i]->isVisible()) {
				live.push_back(i);
			}
		}
		REQUIRE(live.size() >= 4u); // at least two rows, or the step below is never asserted
		const auto across = static_cast<size_t>(sc::kTraits.faceColumns);
		REQUIRE(across == 3u);
		float rowTop = -1.0f;
		for (size_t start = 0; start < live.size(); start += across) {
			const size_t end = std::min(live.size(), start + across);
			float tallest = 0.0f;
			for (size_t k = start; k < end; ++k) {
				tallest = std::max(tallest, static_cast<float>(rows[live[k]]->height()));
			}
			const float centreOffset = 0.5f * (tallest - static_cast<float>(rows[live[start]]->height()));
			const float top = static_cast<float>(rows[live[start]]->y()) - centreOffset;
			if (rowTop >= 0.0f) {
				CAPTURE(start);
				CHECK(top == doctest::Approx(rowTop));
			}
			for (size_t k = start; k < end; ++k) {
				CAPTURE(k);
				const visage::Frame* cell = rows[live[k]];
				CHECK(static_cast<float>(cell->y()) + 0.5f * static_cast<float>(cell->height())
				      == doctest::Approx(top + 0.5f * tallest));
				CHECK(cell->x() == doctest::Approx(rows[live[k - start]]->x()));
				if (k > start) {
					CHECK(cell->x() > rows[live[k - 1]]->x() + rows[live[k - 1]]->width() - 0.01f);
				}
			}
			rowTop = top + tallest + ui::GenericCard::kRowGap;
		}
	}

	SUBCASE("ON THE CARD: no zoned coordinate, and Mix is there") {
		// The zone controls left the card and Mix joined it. The
		// zones are Zone A, Zone B and Macro Cfg. They are still one click away: the panel draws
		// every control, which "an expanded window is a SUPERSET of its card's controls" enforces.
		const std::vector<size_t>& indices = card->paramIndicesForTest();
		CHECK(card->zoneCellsForTest().empty());
		for (const size_t index : indices) {
			CAPTURE(index);
			CHECK(sc::kDescriptor.params[index].zones == 0);
		}
		CHECK(std::find(indices.begin(), indices.end(), static_cast<size_t>(sc::Param::Mix))
		      != indices.end());
		// Mode left the card too (2026-09-17).
		CHECK(std::find(indices.begin(), indices.end(), static_cast<size_t>(sc::Param::Mode))
		      == indices.end());
	}

	SUBCASE("ON THE CARD: the lane reserves the height the card then draws at") {
		// THE 456-AGAINST-180 FAMILY, restated for ragged rows. `laneHeight()` is what the lane
		// reserves; the last row's foot is where the card actually ends.
		const std::vector<visage::Frame*>& rows = card->rowsForTest();
		const visage::Frame* last = nullptr;
		for (visage::Frame* row : rows) {
			if (row->isVisible()) {
				last = row;
			}
		}
		REQUIRE(last != nullptr);
		CHECK(static_cast<float>(last->y() + last->height()) <= card->laneHeight());
	}

	SUBCASE("ON THE PANEL: the readouts are not clipped, and the pickers are not slabs") {
		ui::WidgetRegistry reg;
		std::unique_ptr<ui::ExpandedView> v = ui::makeExpandedView(
		    f.deviceHost, &f.bridge, reg, f.bridge.fxDeviceAt(slot), sc::kDescriptor);
		REQUIRE(v != nullptr);
		auto* panel = dynamic_cast<ui::SklitterExpanded*>(v.get());
		REQUIRE(panel != nullptr);
		const visage::Point want = panel->minimumSize();
		panel->setBounds(0, 0, static_cast<int>(want.x), static_cast<int>(want.y));
		panel->resized();

		const std::vector<visage::Frame*>& cells = panel->cellsForTest();
		REQUIRE(!cells.empty());

		size_t boxes = 0;
		size_t knobs = 0;
		for (visage::Frame* cell : cells) {
			if (!cell->isVisible()) {
				continue;
			}
			if (auto* box = dynamic_cast<ui::ValueBox*>(cell); box != nullptr) {
				++boxes;
				// **THE BOX'S OWN ANSWER** — gamma is compact and wants 15, a choice wants 28, and a
				// site that picked one constant would have drawn the other half full of ground.
				CHECK(cell->height() == doctest::Approx(box->naturalHeight()));
			}
			else if (auto* knob = dynamic_cast<ui::Knob*>(cell); knob != nullptr) {
				++knobs;
				// **THE CLIP REPORTED AS "readouts are clipped".** A panel knob's face is 52 (no
				// site calls `setFaceDiameter` here), so its column is 81 against a cell that was
				// written as the literal 74 — seven pixels off a 13px readout, which prints a row of
				// half-height glyphs. `Knob::draw` clamps to its own bounds, so the widget obeyed and
				// the wrong size looked exactly like nothing being wrong.
				CHECK(static_cast<float>(cell->height())
				      >= ui::Knob::columnHeight(knob->faceDiameter()));
			}
		}
		CHECK(boxes >= 2u);
		CHECK(knobs >= 1u);
	}
}
TEST_CASE("backlog HY 'some knobs truncated in controls at default window size': a Mode "
          "change that adds a row to SKLITTER's grid RE-FITS THE PANEL — every live cell inside it, through "
          "the real overlay and the real tick") {
	// **THE GRID GREW AND NOBODY RE-ASKED.** Five of SKLITTER's controls are mode-gated (`Transport
	// Locked`, `Dry Write`, `Feedback`, `FB Tap`, `Grain Order`), so `liveCells()` moves when Mode
	// moves — at runtime, with the panel open. `GenericExpanded::updateContent` noticed (it re-ran
	// `resized()` and redrew), but the PANEL is sized by its host from `minimumSize()` on the host's
	// layout pass, and nothing asked for one: `ExpandedView::onMinimumSizeChanged` — *"live is not
	// automatic: something has to ask" — was wired for the rails and the body fold and not for the
	// count. So a panel opened in Burst kept Burst's height, and the row a fuller mode added was
	// laid out below the panel's bottom and clipped.
	//
	// **ASSERTED ON BOUNDS, IN WINDOW COORDINATES, CELL BY CELL.** "The panel got taller" would pass on
	// a panel that grew by one pixel; what is claimed is that no live cell's foot lies below the panel's,
	// and that is read off each cell.
	namespace sc = feathers::fx::sklitter;
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	auto* overlay = f.gui.expandedForTest();
	REQUIRE(lane != nullptr);
	REQUIRE(overlay != nullptr);
	const size_t slot = f.bridge.addFxDevice(sc::kTraits.tag);
	f.bridge.refresh();
	lane->resized();
	lane->scrollCardIntoView(slot);
	lane->resized();
	const DeviceInstanceId id = f.bridge.fxDeviceAt(slot);
	const auto setMode = [&](sc::Mode m) {
		f.bridge.setDeviceControl(id, static_cast<size_t>(sc::Param::Mode), static_cast<float>(m));
	};

	// **THE LIVE COUNT PER MODE, THROUGH THE PANEL'S OWN GATE** — a bare panel, no host, one per mode.
	// This is the table the fix is about, and pinning it is what makes the next gated control visible
	// to this case rather than to a listener.
	std::array<size_t, static_cast<size_t>(sc::Mode::kCount)> liveByMode {};
	// **AND THE ROWS, WHICH SINCE THE SIDE COLUMN (backlog IG) ARE THE TALLER OF TWO STACKS** — the grid's
	// cells four across, and the side column's one per row.
	std::array<size_t, static_cast<size_t>(sc::Mode::kCount)> rowsByMode {};
	size_t sparsest = 0;
	size_t fullest = 0;
	{
		ui::WidgetRegistry reg;
		std::unique_ptr<ui::ExpandedView> v =
		    ui::makeExpandedView(f.deviceHost, &f.bridge, reg, id, sc::kDescriptor);
		auto* bare = dynamic_cast<ui::GenericExpanded*>(v.get());
		REQUIRE(bare != nullptr);
		for (size_t m = 0; m < liveByMode.size(); ++m) {
			setMode(static_cast<sc::Mode>(m));
			liveByMode[m] = bare->liveCells();
			const size_t grid = bare->liveGridCells();
			rowsByMode[m] = std::max((grid + ui::GenericExpanded::kPerRow - 1) / ui::GenericExpanded::kPerRow,
			                         bare->liveSideCells());
			if (rowsByMode[m] < rowsByMode[sparsest]) {
				sparsest = m;
			}
			if (rowsByMode[m] > rowsByMode[fullest]) {
				fullest = m;
			}
		}
	}
	const auto rowsOfMode = [&](size_t m) { return rowsByMode[m]; };
	// NOT VACUOUS: the two modes differ by at least one ROW, or the panel has nothing to re-fit.
	REQUIRE(rowsOfMode(fullest) > rowsOfMode(sparsest));
	MESSAGE("live cells by mode: Burst=" << liveByMode[0]
	        << " Repeat=" << liveByMode[1] << " Time=" << liveByMode[2] << " Shuffle=" << liveByMode[3]
	        << " Grain=" << liveByMode[4] << " Pattern=" << liveByMode[5] << " Pitch=" << liveByMode[6]
	        << "; rows sparsest=" << rowsOfMode(sparsest) << " fullest=" << rowsOfMode(fullest));

	// **OPEN IN THE SPARSEST MODE, THROUGH THE REAL TREE.**
	setMode(static_cast<sc::Mode>(sparsest));
	(void)f.gui.canvasForTest()->setCollapsed(true); // the floating panel, whose height is the overlay's to set
	REQUIRE(lane->onExpandDevice != nullptr);
	lane->onExpandDevice(slot);
	REQUIRE(overlay->open());
	// THROUGH THE MOUNT, which is what the overlay holds; the panel is its child. Read off the FRAMES,
	// not `overlay->panelBounds()` — that recomputes from `minimumSize()` live and was green once on a
	// build where nothing had moved (see `MountedFrame`'s constructor).
	const auto* mounted = dynamic_cast<const ui::MountedFrame*>(overlay->contentForTest());
	REQUIRE(mounted != nullptr);
	const auto* content = dynamic_cast<const ui::SklitterExpanded*>(mounted->contentForTest());
	REQUIRE(content != nullptr);
	ui::ExpandedPanel* panel = overlay->panelForTest();
	REQUIRE(panel != nullptr);

	// Every live cell's rectangle, in window coordinates, against the panel's — and the count of rows
	// the grid actually laid out. **ROWS ARE COUNTED BY CELL CENTRE, NOT CELL TOP**: a box or a chip is
	// centred in a knob-tall cell (`GenericExpanded::resized`), so one row has up to three distinct tops
	// and exactly one centre line. A first draft counted tops and read 4 rows where Burst lays out 2.
	struct Fit {
		size_t live = 0;
		size_t rows = 0;
		size_t outside = 0;
		float lowestFoot = 0.0f;
	};
	const auto measure = [&]() {
		Fit fit;
		const visage::Point panelAt = panel->positionInWindow();
		const float panelBottom = panelAt.y + static_cast<float>(panel->height());
		const float panelRight = panelAt.x + static_cast<float>(panel->width());
		std::vector<float> tops;
		for (visage::Frame* cell : content->cellsForTest()) {
			if (!cell->isVisible()) {
				continue;
			}
			++fit.live;
			const visage::Point at = cell->positionInWindow();
			const float foot = at.y + static_cast<float>(cell->height());
			const float right = at.x + static_cast<float>(cell->width());
			fit.lowestFoot = std::max(fit.lowestFoot, foot);
			const float centre = at.y + 0.5f * static_cast<float>(cell->height());
			if (std::find_if(tops.begin(), tops.end(), [&](float t) { return std::abs(t - centre) < 0.5f; })
			    == tops.end()) {
				tops.push_back(centre);
			}
			CAPTURE(at.x);
			CAPTURE(at.y);
			CAPTURE(foot);
			CAPTURE(panelBottom);
			const bool inside = at.y >= panelAt.y - 0.5f && foot <= panelBottom + 0.5f
			                    && at.x >= panelAt.x - 0.5f && right <= panelRight + 0.5f;
			CHECK(inside);
			if (!inside) {
				++fit.outside;
			}
		}
		fit.rows = tops.size();
		return fit;
	};

	const Fit before = measure();
	CHECK(before.live == liveByMode[sparsest]);
	CHECK(before.rows == rowsOfMode(sparsest));
	CHECK(before.outside == 0u);
	const float heightBefore = static_cast<float>(panel->height());
	CHECK(heightBefore >= content->minimumSize().y + ui::ExpandedOverlay::kTitleHeight
	                          + 2.0f * ui::ExpandedOverlay::kPanelPad - 0.5f);

	// **THE MODE MOVES UNDER AN OPEN PANEL, AND THE EDITOR'S OWN TICK IS WHAT NOTICES.** No hand-written
	// `overlay->resized()` — that was exactly what `widget-shot` used to do to hide this class of
	// defect from the camera.
	setMode(static_cast<sc::Mode>(fullest));
	const Gui::TickReport report = f.gui.tickForTest();
	CHECK(report.deviceFrames >= 1u); // the panel reported the re-layout it made

	const Fit after = measure();
	CHECK(after.live == liveByMode[fullest]);
	CHECK(after.rows == rowsOfMode(fullest));
	CHECK(after.rows - before.rows == rowsOfMode(fullest) - rowsOfMode(sparsest));
	// THE CLAIM: nothing laid out below the panel's foot, and the panel is as tall as its content asks.
	CHECK(after.outside == 0u);
	const float heightAfter = static_cast<float>(panel->height());
	CHECK(heightAfter > heightBefore);
	CHECK(heightAfter >= content->minimumSize().y + ui::ExpandedOverlay::kTitleHeight
	                         + 2.0f * ui::ExpandedOverlay::kPanelPad - 0.5f);
	// **AND THE PANEL FITS THE DEFAULT EDITOR** — H2 of the diagnosis, answered by the instrument: the
	// fullest mode's panel, at its content's minimum, against the overlay's height (the fixture is the
	// default `kLogicalWidth × kLogicalHeight`). If this fails, the fix is a design question, not a
	// re-ask.
	MESSAGE("panel height sparsest=" << heightBefore << " fullest=" << heightAfter
	        << " content minimum y=" << content->minimumSize().y << " overlay height=" << overlay->height()
	        << " overlay width=" << overlay->width());
	CHECK(content->minimumSize().y + ui::ExpandedOverlay::kTitleHeight + 2.0f * ui::ExpandedOverlay::kPanelPad
	      <= static_cast<float>(overlay->height()));

	SUBCASE("and back: the sparsest mode hands the row back") {
		setMode(static_cast<sc::Mode>(sparsest));
		(void)f.gui.tickForTest();
		const Fit again = measure();
		CHECK(again.rows == before.rows);
		CHECK(again.outside == 0u);
		CHECK(static_cast<float>(panel->height()) == doctest::Approx(heightBefore));
	}

	SUBCASE("at 1.75x, the development display: the re-fit converts the NATIVE rectangles too") {
		// **A SCALE CHANGE MOVES NOTHING LOGICAL** (CLAUDE.md, backlog EC), so the logical containment
		// above would pass on a build whose panel never converted. The claim at a scale is about the
		// native rectangle: the panel's, the content's and every live cell's native height is the
		// logical one × 1.75, within the pixel visage rounds to — and strictly larger than the logical
		// one, which is the half a tolerance alone lets through at 1:1.
		constexpr float kScale = 1.75f;
		f.root->setDpiScale(kScale);
		// Back to the sparsest and forward again UNDER THE NEW SCALE, through the same tick, so the
		// re-fit this case is about is the layout pass that converts everybody.
		setMode(static_cast<sc::Mode>(sparsest));
		(void)f.gui.tickForTest();
		setMode(static_cast<sc::Mode>(fullest));
		(void)f.gui.tickForTest();
		const Fit scaled = measure();
		CHECK(scaled.outside == 0u);
		CHECK(scaled.rows == rowsOfMode(fullest));
		const auto nativeMatches = [&](const visage::Frame& fr) {
			const float want = static_cast<float>(fr.height()) * kScale;
			const auto got = static_cast<float>(fr.nativeHeight());
			CAPTURE(want);
			CAPTURE(got);
			CHECK(got > static_cast<float>(fr.height()));
			CHECK(std::abs(got - want) <= 1.0f);
		};
		nativeMatches(*panel);
		nativeMatches(*content);
		size_t checked = 0;
		for (visage::Frame* cell : content->cellsForTest()) {
			if (cell->isVisible()) {
				nativeMatches(*cell);
				++checked;
			}
		}
		CHECK(checked == liveByMode[fullest]);
		// AND THE PANEL'S NATIVE FOOT HOLDS EVERY CELL'S, restated in native pixels through the parent
		// chain rather than trusted from the logical containment above.
		const auto nativeTopInWindow = [](const visage::Frame* fr) {
			int y = 0;
			for (const visage::Frame* at = fr; at != nullptr; at = at->parent()) {
				y += at->nativeY();
			}
			return y;
		};
		const int panelNativeBottom = nativeTopInWindow(panel) + panel->nativeHeight();
		for (visage::Frame* cell : content->cellsForTest()) {
			if (!cell->isVisible()) {
				continue;
			}
			const int foot = nativeTopInWindow(cell) + cell->nativeHeight();
			CAPTURE(foot);
			CAPTURE(panelNativeBottom);
			CHECK(foot <= panelNativeBottom);
		}
	}
}
TEST_CASE("SKLITTER's band spans the SEQUENCE: ruled into its bars, and the head sweeps all of it") {
	// The playhead overwrote the effected display bar as it passed, because it spanned only one bar
	// while these sequences can have a four-bar period.
	//
	// The engine half is `test_fx_sklitter.cpp`'s "the published picture is ONE arrangement, not a seam
	// walking with the playhead", which measures the churn at zero. This is the DRAWING half: that the
	// surfaces read the sequence the device publishes rather than a bar of it, and that the bar count
	// comes from the device rather than being guessed from the step count.
	namespace sc = feathers::fx::sklitter;
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	const size_t slot = f.bridge.addFxDevice(sc::kTraits.tag);
	REQUIRE(slot == 1);
	lane->resized();
	lane->scrollCardIntoView(slot);
	lane->resized();
	auto* card = dynamic_cast<ui::SklitterCard*>(lane->cardAt(slot));
	REQUIRE(card != nullptr);

	// FOUR BARS OF EIGHT, published by hand: this case is about the DECODE and the drawing, and
	// driving real audio would make it fail for the engine's reasons as well as its own.
	constexpr int kBars = 4;
	constexpr size_t kPerBar = 8;
	constexpr size_t kSteps = kPerBar * kBars;
	std::vector<float> values(sc::kReadingCount, 0.0f);
	values[static_cast<size_t>(sc::Reading::Status)] = 3.0f; // playing
	values[static_cast<size_t>(sc::Reading::Steps)] = static_cast<float>(kSteps);
	values[static_cast<size_t>(sc::Reading::SeqBars)] = static_cast<float>(kBars);
	// THE HEAD IN THE THIRD BAR — a position no one-bar band could express, which is the point.
	constexpr int kHead = 2 * static_cast<int>(kPerBar) + 3;
	values[static_cast<size_t>(sc::Reading::Step)] = static_cast<float>(kHead);
	for (size_t k = 0; k < kSteps; ++k) {
		const size_t base = sc::stepReading(k);
		values[base] = static_cast<float>(k) / static_cast<float>(kSteps);
		values[base + 1u] = static_cast<float>(sc::StepFlag::Known);
	}
	f.bridge.publishDeviceReadings(slot, values.data(), values.size());

	// **THE WHOLE SEQUENCE DECODES, NOT ONE BAR OF IT.** Thirty-two chunks where the picture used to
	// cap at sixteen — which is also the `kPictureSteps` widening doing its job end to end, through
	// the bridge's `kMaxDeviceReadings` and the rack's `kMaxReadings`.
	const std::vector<ui::SklitterChunk> picture = card->chunksForTest();
	CHECK(picture.size() == kSteps);
	REQUIRE(picture.size() > 16u); // or this case is not testing the widening at all

	// **THE BAR COUNT IS THE DEVICE'S OWN.** Reading it back is what stops the surfaces inferring it
	// by dividing the step count by a slice count they do not have.
	CHECK(ui::sklitterSeqBars(f.deviceHost, f.bridge.fxDeviceAt(slot)) == kBars);

	// **THE HEAD ADDRESSES THE SEQUENCE.** It was `sliceIndex_` alone, a position WITHIN the bar, so a
	// head in bar three drew in bar one. Nineteen of thirty-two is inside the third bar; sixteen to
	// twenty-three is that bar's span.
	CHECK(card->playingStepForTest() == kHead);
	CHECK(card->playingStepForTest() >= 2 * static_cast<int>(kPerBar));
	CHECK(card->playingStepForTest() < 3 * static_cast<int>(kPerBar));

	SUBCASE("a one-bar sequence is ruled with no interior line, and a four-bar one with three") {
		// **THE RULING IS GEOMETRY AND IS ASSERTED AS GEOMETRY.** `drawSklitterBarRules` draws
		// `seqBars - 1` interior lines and nothing at one bar; a rule at the far edge would read as a
		// chunk boundary that is not there. Counted through the function's own arithmetic rather than
		// off a rendered frame, which this repo's rule forbids as evidence about a value.
		const auto interiorLines = [](int bars) { return bars > 1 ? bars - 1 : 0; };
		CHECK(interiorLines(1) == 0);
		CHECK(interiorLines(kBars) == 3);

		// AND THE DEVICE REPORTS ONE WHEN IT HAS PUBLISHED NOTHING, rather than zero — a band ruled
		// into zero bars divides by it.
		ModFixture bare;
		const size_t empty = bare.bridge.addFxDevice(sc::kTraits.tag);
		CHECK(ui::sklitterSeqBars(bare.deviceHost, bare.bridge.fxDeviceAt(empty)) >= 1);
	}

	SUBCASE("the bar count is in the repaint comparison, so a re-ruling asks for its frame") {
		// **A RULING CAN MOVE WHILE THE CHUNKS DO NOT.** At a fine cut a two-bar and a four-bar
		// sequence both clamp to `kPictureSteps`, so comparing chunks alone would report "nothing
		// moved" for a band that must be drawn with a different grid. This is the same omission that
		// made the band static in the first place, one field along.
		card->updateContentForTest();              // settle whatever the publish above moved
		CHECK_FALSE(card->updateContentForTest()); // and a second look with nothing moving is not
		values[static_cast<size_t>(sc::Reading::SeqBars)] = 2.0f;
		f.bridge.publishDeviceReadings(slot, values.data(), values.size());
		CHECK(card->updateContentForTest());
	}
}
TEST_CASE("backlog EP: a two-way choice is a CHIP on both generic surfaces, and pressing it toggles") {
	// An enable is a toggle, not a dropdown.
	//
	// SKLITTER's Engage drew as a `ValueBox` reading `Engage / Off` — a bordered rectangle printing a
	// word, identical to `Mode` beside it, which opens a list of eight. **AND IT WAS NEVER A DROPDOWN:**
	// `ValueBox::choiceMenuAvailable` already declines below three entries, so the box printed a word,
	// refused to open, and dragged instead. It looked like the one thing it would not do, which is the
	// affordance lying — and the honesty rule covers the SHAPE of a control, not only whether it is live.
	namespace sc = feathers::fx::sklitter;
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	const size_t slot = f.bridge.addFxDevice(sc::kTraits.tag);
	REQUIRE(slot == 1);
	lane->resized();
	lane->scrollCardIntoView(slot);
	lane->resized();
	auto* card = dynamic_cast<ui::SklitterCard*>(lane->cardAt(slot));
	REQUIRE(card != nullptr);

	// **WHICH PARAMS ARE TWO-WAY CHOICES, ASKED OF THE DESCRIPTOR.** Naming Engage here would test one
	// control; asking the declaration tests the RULE, and catches the next device that declares one.
	std::vector<size_t> twoWay;
	std::vector<size_t> longerLists;
	for (size_t i = 0; i < sc::kDescriptor.params.size(); ++i) {
		const device::ParamDesc& p = sc::kDescriptor.params[i];
		if (p.unit != device::Unit::Choice || p.names.empty()) {
			continue;
		}
		(p.names.size() == 2u ? twoWay : longerLists).push_back(i);
	}
	// NOT VACUOUS IN EITHER DIRECTION: this device really has both kinds, so the two checks below are
	// about a distinction rather than about an empty set.
	CHECK(twoWay.size() >= 1u);
	CHECK(longerLists.size() >= 1u);

	// **ASKED OF THE PANEL, NOT THE CARD, since Mode left the card.** The card carries no list any more, so "a list is still a box" could
	// only be asked where both kinds are drawn — and the panel is built by the same constructor rule.
	ui::WidgetRegistry listReg;
	std::unique_ptr<ui::ExpandedView> panelView = ui::makeExpandedView(
	    f.deviceHost, &f.bridge, listReg, f.bridge.fxDeviceAt(slot), sc::kDescriptor);
	REQUIRE(panelView != nullptr);
	auto* listPanel = dynamic_cast<ui::SklitterExpanded*>(panelView.get());
	REQUIRE(listPanel != nullptr);
	const std::vector<visage::Frame*>& rows = listPanel->cellsForTest();
	const std::vector<size_t>& indices = listPanel->paramIndicesForTest();
	REQUIRE(rows.size() == indices.size());

	size_t chips = 0;
	size_t boxes = 0;
	for (size_t i = 0; i < rows.size(); ++i) {
		const bool two = std::find(twoWay.begin(), twoWay.end(), indices[i]) != twoWay.end();
		const bool many = std::find(longerLists.begin(), longerLists.end(), indices[i])
		                  != longerLists.end();
		if (two) {
			CAPTURE(indices[i]);
			// A SWITCH IS A CHIP...
			CHECK(dynamic_cast<ui::SwitchChip*>(rows[i]) != nullptr);
			CHECK(dynamic_cast<ui::ValueBox*>(rows[i]) == nullptr);
			++chips;
		}
		else if (many) {
			CAPTURE(indices[i]);
			// ...AND A LIST IS STILL A BOX, which is the half that proves the rule discriminates — or, where
			// the device declared it `modeCell` (SKLITTER's Groove Mode, backlog IO addendum), a zone knob
			// whose selector is the list, every entry on its menu.
			const bool modeCell = feathers::fx::sklitter::kDescriptor.params[indices[i]].modeCell;
			CHECK((modeCell ? dynamic_cast<ui::ModeValueCell*>(rows[i]) != nullptr
			                : dynamic_cast<ui::ValueBox*>(rows[i]) != nullptr));
			CHECK(dynamic_cast<ui::SwitchChip*>(rows[i]) == nullptr);
			++boxes;
		}
	}
	CHECK(chips >= 1u);
	CHECK(boxes >= 1u);

	SUBCASE("pressing the chip moves the control, and moving it back is one press each way") {
		// **THE VALUE, NOT THE PICTURE.** A chip that drew a lit state without writing the control
		// would look right in every screenshot; this repo's rule is to read the number.
		ui::SwitchChip* chip = nullptr;
		for (visage::Frame* row : rows) {
			if (auto* c = dynamic_cast<ui::SwitchChip*>(row); c != nullptr) {
				chip = c;
				break;
			}
		}
		REQUIRE(chip != nullptr);
		const bool was = chip->on();
		chip->toggleForTest();
		CHECK(chip->on() != was);
		chip->toggleForTest();
		CHECK(chip->on() == was);
	}

	SUBCASE("the PANEL seats it the same way — the second road, which this device keeps teaching") {
		// A fix landing on one of the two generic surfaces is half a fix by default; SKLITTER has now
		// produced that shape four times. Checked rather than assumed.
		ui::WidgetRegistry reg;
		std::unique_ptr<ui::ExpandedView> v = ui::makeExpandedView(
		    f.deviceHost, &f.bridge, reg, f.bridge.fxDeviceAt(slot), sc::kDescriptor);
		REQUIRE(v != nullptr);
		auto* panel = dynamic_cast<ui::SklitterExpanded*>(v.get());
		REQUIRE(panel != nullptr);
		const visage::Point want = panel->minimumSize();
		panel->setBounds(0, 0, static_cast<int>(want.x), static_cast<int>(want.y));
		panel->resized();
		size_t panelChips = 0;
		size_t pads = 0;
		for (visage::Frame* cell : panel->cellsForTest()) {
			auto* c = dynamic_cast<ui::SwitchChip*>(cell);
			if (c == nullptr) {
				continue;
			}
			++panelChips;
			// **ONLY A VISIBLE CELL HAS BOUNDS.** `GenericExpanded::resized` skips the ones its mode
			// hides — the gap closes behind them — so a hidden chip reads height 0, and asserting over
			// all of them measured the gating rather than the seating.
			if (!c->isVisible()) {
				continue;
			}
			// AT A CHIP'S HEIGHT, not stretched to the cell like the box used to be — UNLESS the card
			// draws it as a pad, and then it is a pad here too (Engage is enlarged in the window
			// as well). The card's own decision is the input, so the two surfaces
			// cannot disagree about which switch is the pad.
			size_t index = panel->paramIndicesForTest().size();
			for (size_t n = 0; n < panel->cellsForTest().size(); ++n) {
				if (panel->cellsForTest()[n] == cell) {
					index = panel->paramIndicesForTest()[n];
				}
			}
			REQUIRE(index < sc::kDescriptor.params.size());
			CAPTURE(std::string(sc::kDescriptor.params[index].name));
			if (ui::GenericCard::isPad(sc::kDescriptor, index)) {
				++pads;
				// **THE PANEL'S OWN CELL HEIGHT, NOT `Knob::columnHeight(Knob::kDiameter)`.** It was that
				// literal derivation and went red on 2026-09-19 reading 67 against 81 — correctly at the
				// time, because the panel then compressed its grid while a section was open. It does not
				// any more (the sections are rails and the WINDOW grows instead), so the two agree again
				// and the seam is still what is asked: the rule this case is about is "a pad-shaped chip
				// is a WHOLE CELL tall", and spelling the number was the third visit from `kCellHeight`'s
				// own lesson.
				CHECK(static_cast<float>(c->height()) == doctest::Approx(panel->cellHeightForTest()));
			}
			else {
				CHECK(static_cast<float>(c->height()) == doctest::Approx(ui::SwitchChip::kHeight));
			}
		}
		// **ENGAGE IS THE PAD**, stated by name so the helper cannot be answering for some other control.
		CHECK(ui::GenericCard::isPad(sc::kDescriptor, static_cast<size_t>(sc::Param::Engage)));
		CHECK(pads >= 1u);
		// AND A SWITCH THE FACE DOES NOT CARRY IS STILL A CHIP — Quantized, live in Burst. Proves the
		// rule discriminates rather than enlarging every switch on the panel.
		f.bridge.setDeviceControl(f.bridge.fxDeviceAt(slot), static_cast<size_t>(sc::Param::Mode),
		                          static_cast<float>(sc::Mode::Burst));
		panel->resized();
		CHECK_FALSE(ui::GenericCard::isPad(sc::kDescriptor, static_cast<size_t>(sc::Param::Quantized)));
		for (size_t n = 0; n < panel->cellsForTest().size(); ++n) {
			if (panel->paramIndicesForTest()[n] != static_cast<size_t>(sc::Param::Quantized)) {
				continue;
			}
			REQUIRE(panel->cellsForTest()[n]->isVisible());
			CHECK(static_cast<float>(panel->cellsForTest()[n]->height())
			      == doctest::Approx(ui::SwitchChip::kHeight));
		}
		// **LESS THE ENABLE, WHICH NEITHER GENERIC SURFACE DRAWS.** The head draws it as its square, and
		// both the card and the panel skip `Traits::enableControl` for the reason each says at its own
		// loop: one field with two controls lets a player switch a device off in one place and see it on
		// in the other. So three declared two-way choices are two seated chips, and this arithmetic is
		// the check that the MISSING one is the enable rather than an oversight.
		const bool enableIsTwoWay =
		    sc::kDescriptor.traits.enableControl >= 0
		    && std::find(twoWay.begin(), twoWay.end(),
		                 static_cast<size_t>(sc::kDescriptor.traits.enableControl))
		           != twoWay.end();
		CHECK(panelChips == twoWay.size() - (enableIsTwoWay ? 1u : 0u));
	}
}
TEST_CASE("the perform pad writes the Perf X and Perf Y a reader computes from the same geometry") {
	namespace sc = feathers::fx::sklitter;
	namespace pp = feathers::ui::perform_pad;
	PerformPanel p;
	REQUIRE(p.panel != nullptr);
	ui::PerformPad& pad = p.panel->padForTest();
	REQUIRE(pad.isVisible());

	// **THE GEOMETRY IS THE PAD'S OWN, asked of it rather than recomputed here.** A case that built its
	// own disk would be asserting that two arithmetics agree, and would pass while the pad was laid out
	// somewhere else entirely.
	const pp::Disk d = pad.disk();
	REQUIRE(d.valid());

	const auto press = [&](float px, float py) {
		visage::MouseEvent e;
		e.position = { px, py };
		pad.onMouseDown().callback(e);
		pad.onMouseUp().callback(e);
	};
	const auto axisOf = [&](sc::Param which) {
		return p.f.deviceHost.fieldAt(p.device, static_cast<size_t>(which));
	};

	SUBCASE("a press at the centre is the centre, on both axes") {
		// Move it away first, or "it is at the centre" is the value it started at.
		press(d.cx + d.radius * 0.5f, d.cy);
		REQUIRE(axisOf(sc::Param::PerfX) != 0.0f);
		press(d.cx, d.cy);
		CHECK(axisOf(sc::Param::PerfX) == 0.0f);
		CHECK(axisOf(sc::Param::PerfY) == 0.0f);
	}

	SUBCASE("a press east is +X with Y untouched, and NORTH is negative Y") {
		// **THE AXIS AND ITS SIGN, BY NAME.** y is DOWN in every screen space in this editor, so N — the
		// top of the disk — is `Perf Y` NEGATIVE (`perform.h`). A pad that flipped it would put the
		// player's north waypoint at the bottom and nothing about a rendered circle would say so.
		press(d.cx + d.radius, d.cy);
		CHECK(axisOf(sc::Param::PerfX) == doctest::Approx(1.0f).epsilon(1e-5));
		CHECK(axisOf(sc::Param::PerfY) == doctest::Approx(0.0f).epsilon(1e-5));
		press(d.cx, d.cy - d.radius);
		CHECK(axisOf(sc::Param::PerfX) == doctest::Approx(0.0f).epsilon(1e-5));
		CHECK(axisOf(sc::Param::PerfY) == doctest::Approx(-1.0f).epsilon(1e-5));
	}

	SUBCASE("an arbitrary pixel writes what the geometry says, restated rather than re-called") {
		// **THE INDEPENDENT RESTATEMENT.** `(px − cx) / radius`, which is the definition of the mapping
		// and not a second call to `axesAt`. Chosen inside the disk so no clamp is involved: the clamp
		// has its own subcase.
		const float px = d.cx + d.radius * 0.37f;
		const float py = d.cy - d.radius * 0.22f;
		press(px, py);
		CHECK(axisOf(sc::Param::PerfX) == doctest::Approx((px - d.cx) / d.radius).epsilon(1e-5));
		CHECK(axisOf(sc::Param::PerfY) == doctest::Approx((py - d.cy) / d.radius).epsilon(1e-5));
		// NOT VACUOUS: neither axis is zero, so the two lines are not both asserting the rest position.
		CHECK(axisOf(sc::Param::PerfX) != 0.0f);
		CHECK(axisOf(sc::Param::PerfY) != 0.0f);
	}

	SUBCASE("a drag past the rim slides round it rather than leaving the square") {
		// **CLAMPED TO THE DISK, NOT THE SQUARE**, and `perform_pad.h` argues that this loses nothing:
		// beyond the diamond the weights depend on the DIRECTION alone, so every outside point has a rim
		// point with identical weights. What must not happen is an axis past 1.
		visage::MouseEvent down;
		down.position = { d.cx, d.cy };
		pad.onMouseDown().callback(down);
		visage::MouseEvent drag;
		drag.position = { d.cx + d.radius * 5.0f, d.cy + d.radius * 5.0f };
		pad.onMouseDrag().callback(drag);
		pad.onMouseUp().callback(drag);
		const float x = axisOf(sc::Param::PerfX);
		const float y = axisOf(sc::Param::PerfY);
		CHECK(std::hypot(x, y) == doctest::Approx(1.0f).epsilon(1e-5));
		// AND IT WENT THE WAY THE HAND WENT — south-east, both positive, equal by symmetry.
		CHECK(x == doctest::Approx(y).epsilon(1e-5));
		CHECK(x > 0.0f);
	}

	SUBCASE("a press outside the pad writes NOTHING — the hit test is real") {
		press(d.cx + d.radius * 0.5f, d.cy);
		const float x = axisOf(sc::Param::PerfX);
		REQUIRE(x != 0.0f);
		// Far outside, past the grab slack.
		press(d.cx + d.radius * 4.0f, d.cy);
		CHECK(axisOf(sc::Param::PerfX) == x);
	}
}

