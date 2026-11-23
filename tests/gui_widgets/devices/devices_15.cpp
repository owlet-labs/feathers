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

// PIECE 15 OF 18 of the devices suite: its cases in their original order, with the helpers only they use. See devices_common.h.

#include "devices/devices_common.h"

TEST_CASE("NO item 2 follow-up: a chain's mute and solo chips glow for an armed source as the level's pill does, take a route by drag, and show a routed ring when not armed") {
	namespace pr = feathers::rack::parallel;
	MirroredRack m;
	ui::ChainStrip& strip = m.panel->stripForTest(0);
	ui::SwitchChip& mute = strip.muteChip();
	ui::SwitchChip& solo = strip.soloChip();
	CHECK(mute.showsModulation());
	CHECK(solo.showsModulation());
	CHECK_FALSE(mute.modMarks().glow); // nothing armed
	const backplane::Source source = backplane::Source::core(0);
	m.mod.focus(source);
	m.mod.arm(true);
	CHECK(mute.modMarks().glow);
	CHECK(solo.modMarks().glow);
	CHECK(mute.modMarks().reach == 0.0f);
	// A PRESS-DRAG ON THE ARMED CHIP ROUTES (the pill's power rule): the seed, then up by half the drag range.
	const uint8_t slot = m.r.rack().slotOf(0);
	mute.onMouseDown().callback(at(0.0f, 0.0f));
	CHECK(m.f.bridge.routeAmount(source, m.dest(slot, pr::SlotControl::Mute)) == doctest::Approx(ui::SwitchChip::kRouteSeedAmount));
	const visage::MouseEvent drag = at(0.0f, -ui::SwitchChip::kRouteDragRange * 0.5f);
	mute.onMouseDrag().callback(drag);
	mute.onMouseUp().callback(drag);
	CHECK(m.f.bridge.routeAmount(source, m.dest(slot, pr::SlotControl::Mute))
	      == doctest::Approx(ui::SwitchChip::kRouteSeedAmount + 0.5f));
	CHECK_FALSE(m.r.rack().muted(0)); // routing never pressed the switch
	CHECK(mute.modMarks().reach > 0.0f);
	// NOT ARMED: the ring says it is routed, filled while MODULATION has the switch on — the host's published modulated
	// value (no engine runs this fixture's lane, so it is whatever the host last published), never the hand's base.
	m.mod.arm(false);
	CHECK_FALSE(mute.modMarks().glow);
	CHECK(mute.modMarks().ring);
	CHECK(mute.modMarks().ringFilled == (mute.control().normalizedModulated() >= 0.5));
	CHECK_FALSE(solo.modMarks().ring); // nothing routed to the solo
}
TEST_CASE("NO item 2 follow-up: a chain's level exposes to the host through its slot param — a DAW write lands in the body at the service beat, and the knob, the save and the mirror follow") {
	namespace pr = feathers::rack::parallel;
	MirroredRack m;
	emptyPool(m.f.bridge);
	BridgeLookup lookup { m.f.bridge };
	hs::SlotHost host { m.f.bridge.slotPool(), lookup };
	ui::ChainControl& level = m.panel->stripForTest(0).levelControlForTest();
	CHECK(level.hostSlot().state == feathers::device::HostSlotStatus::State::Unslotted);
	REQUIRE(level.exposeToHost(nullptr));
	(void)visit(m.f.bridge, host);
	REQUIRE(level.hostSlot().state == feathers::device::HostSlotStatus::State::Pinned);
	const hs::SlotIndex slot = level.hostSlot().slot;
	const std::optional<hs::Target> target =
	    m.f.bridge.slotTargetOf(m.r.id, pr::slotParam(m.r.rack().slotOf(0), pr::SlotControl::Level));
	REQUIRE(target.has_value());
	CHECK(m.f.bridge.slotPool().slotOf(*target) == slot);

	// THE DAW WRITES 0.2: the slot owner's visit puts it in the record, and the holder's beat into the body.
	m.f.bridge.slotBus().onHostValue(slot, 0.2f);
	(void)visit(m.f.bridge, host);
	CHECK(m.f.deviceHost.fieldAt(m.r.id, pr::slotParam(m.r.rack().slotOf(0), pr::SlotControl::Level)) == 0.2f);
	(void)m.f.bridge.serviceOwnControls();
	CHECK(m.r.rack().level(0) == 0.2f);
	CHECK(level.value() == doctest::Approx(0.2));
	// AND A HAND'S EDIT AFTER IT IS THE HOST'S TO HEAR: the base, and so the bus, move with it.
	level.setValue(0.6);
	CHECK(m.f.deviceHost.fieldAt(m.r.id, pr::slotParam(m.r.rack().slotOf(0), pr::SlotControl::Level)) == 0.6f);
	(void)m.f.bridge.serviceOwnControls();
	CHECK(m.r.rack().level(0) == 0.6f); // nothing adopted back over the hand
}
TEST_CASE("DM: the Bus Utility card offers Swap as a button, Monitor as a dropdown, and the Mid and Side trims as knobs") {
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
		return f.deviceHost.fieldAt(f.bridge.fxDeviceAt(slot), static_cast<size_t>(p));
	};

	SUBCASE("Swap: a press where the button is toggles the field, both ways, and does not pick the card up") {
		const visage::Bounds b = card->swapBounds();
		const visage::Point at = card->positionInWindow()
		                         + visage::Point { b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f };
		REQUIRE(f.root->frameAtPoint(at) == static_cast<visage::Frame*>(card));
		REQUIRE(fieldOf(bu::Param::Swap) < 0.5f);
		dragThroughRoot(f.root, at, at);
		CHECK(fieldOf(bu::Param::Swap) >= 0.5f);
		CHECK_FALSE(card->cardDragging());
		dragThroughRoot(f.root, at, at);
		CHECK(fieldOf(bu::Param::Swap) < 0.5f);
		// ITS OWN CELL, clear of the polarity pair's.
		CHECK_FALSE(b.overlaps(card->polarityBounds(true)));
	}

	SUBCASE("Monitor: a dropdown of every rung, and a pick writes that one value") {
		ui::ValueBox* box = card->monitorForTest();
		REQUIRE(box != nullptr);
		CHECK(box->isVisible());
		CHECK(box->choiceMenuAvailable());
		const visage::PopupMenu menu = box->choiceMenu();
		REQUIRE(menu.options().size() == std::size(bu::kMonitorNames));
		menu.onSelection().callback(static_cast<int>(bu::Monitor::Side));
		CHECK(fieldOf(bu::Param::Monitor) == static_cast<float>(bu::Monitor::Side));
	}

	SUBCASE("every DM control is one the card reaches, and the trims are knobs on its third row") {
		const std::vector<bu::Param> reached = card->addressedParamsForTest();
		for (const bu::Param p : { bu::Param::MidTrim, bu::Param::SideTrim, bu::Param::Monitor, bu::Param::Swap }) {
			CAPTURE(static_cast<int>(p));
			CHECK(std::find(reached.begin(), reached.end(), p) != reached.end());
		}
		size_t trims = 0;
		for (const auto& knob : card->knobsForTest()) {
			const std::string name = knob->control()->desc().key;
			trims += (name == "bus_mid_trim" || name == "bus_side_trim") ? 1 : 0;
			// INSIDE THE CARD: the third row made the card taller, and a knob below its bottom edge is not offered.
			CHECK(knob->bounds().bottom() <= card->height());
		}
		CHECK(trims == 2);
	}
}
TEST_CASE("the Pattern card: a tab press focuses that pattern alone, its E and S chips are its Enable and Solo, and the lane is the device's own decode") {
	// TERRAIN-ARP DESIGN §15: a tab press EDITS (it moves the focus, never the selector); only the focused pattern's own
	// widgets are shown; the lane is `pattern::build` over `PatternDevice::placeOf` and `recipeFor`, as the device plays.
	namespace pt = feathers::mod::arp::pat;
	ModFixture f;
	const size_t slot = f.bridge.addRackDevice(pt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxRackSlots);
	f.bridge.refresh();
	f.panel->resized();
	auto* card = const_cast<ui::PatternCard*>(dynamic_cast<const ui::PatternCard*>(f.panel->cardFrameForTest(slot)));
	REQUIRE(card != nullptr);
	const DeviceInstanceId id = card->device();
	f.deviceHost.setField(id, pt::at(2, pt::Field::Zone), 333.0f);
	f.deviceHost.setField(id, pt::at(2, pt::Field::Mode), 2.0f); // Chords
	const float selectorBefore = f.deviceHost.fieldAt(id, pt::at(pt::Global::Selector));
	card->setFocus(2);
	CHECK(card->focus() == 2);
	CHECK(f.deviceHost.fieldAt(id, pt::at(pt::Global::Selector)) == selectorBefore); // edit only
	// THE LANE IS THE DEVICE'S DECODE OF PATTERN 3, over a held C4.
	(void)card->updateContent();
	std::vector<float> row(pt::kParamCount);
	for (size_t i = 0; i < row.size(); ++i) {
		row[i] = f.deviceHost.fieldAt(id, i);
	}
	const pt::Settings s = pt::settingsOf(row.data(), row.size());
	feathers::mod::arp::HeldChord held;
	held.press(60, 0, 0.8f, 1);
	std::array<bool, feathers::mod::arp::genes::kPinCount> on {};
	on = feathers::mod::arp::genes::kPinOnByDefault; // the card lane reads the hand's switches: the defaults here
	feathers::mod::arp::pattern::PatternCycle want;
	feathers::mod::arp::pattern::build(pt::PatternDevice::placeOf(s.patterns[2], feathers::SliceResidue<feathers::mod::arp::genes::kRowCount> {}),
	                                   pt::recipeFor(s, 2, on), held, want);
	const auto& got = card->lane().cycleForTest();
	REQUIRE(got.length == want.length);
	for (size_t k = 0; k < want.length; ++k) {
		CHECK(got.steps[k].kind == want.steps[k].kind);
		CHECK(got.steps[k].keys == want.steps[k].keys);
	}
	// EVERY PATTERN'S CONTROLS ARE ON THE CARD, the focused one's shown: the addressed set has all four blocks' six.
	const std::vector<size_t> addressed = card->addressedParamsForTest();
	for (size_t p = 0; p < pt::kPatterns; ++p) {
		for (const pt::Field fld : { pt::Field::Enable, pt::Field::Solo, pt::Field::Mode, pt::Field::Scale, pt::Field::Rate,
		                             pt::Field::Length }) {
			CHECK(std::count(addressed.begin(), addressed.end(), pt::at(p, fld)) == 1);
		}
	}
	REQUIRE(f.bridge.removeRackDevice(slot));
}
TEST_CASE("the Pattern device is offered by Feathers' modulator-lane picker, and a pick seats it with its own card") {
	// THE ARP'S LESSON (2026-10-30): the picker offers what calls itself a modulator or a note effect (`belongsInModulationRack`, the TAG, with
	// "note-effect" last), not what the browser files under a heading — so the offer is asked of the picker itself.
	namespace pt = feathers::mod::arp::pat;
	REQUIRE(std::string(pt::kTags[std::size(pt::kTags) - 1]) == "note-effect"); // a note effect since 2026-11-04
	ModFixture f;
	const std::vector<const feathers::device::Descriptor*> offered = f.panel->addableForTest();
	size_t index = offered.size();
	for (size_t i = 0; i < offered.size(); ++i) {
		if (feathers::device::sameTag(offered[i]->traits.tag, pt::kTraits.tag)) {
			index = i;
		}
	}
	REQUIRE_MESSAGE(index < offered.size(), "the picker does not offer the Pattern device");
	const size_t before = f.panel->deviceCount();
	const size_t slot = f.panel->addChoiceForTest(index);
	// AT THE FRONT while "Arps sent to front of chain" is on (its default) — it emits notes — at the end otherwise.
	REQUIRE(slot == (ui::emittersToFront() ? 0 : before));
	const DeviceInstanceId id = f.bridge.rackDeviceAt(slot);
	REQUIRE(id != kNoInstance);
	CHECK(dynamic_cast<const ui::PatternCard*>(f.panel->cardFrameForTest(slot)) != nullptr);
}
TEST_CASE("the Pattern window: its terrain is the pattern decoded at each place, painted by a pure palette, rebuilt only when an input moves; the puck moves Place X/Y; the cost is measured") {
	// TERRAIN-ARP DESIGN §14 AND STEP 10. Every claim by value: the cell's colour is `paletteOf(featuresOf(build at the
	// cell's place))`, restated here; a drag writes the focused pattern's own Place X/Y; a tab moves the focus.
	namespace pt = feathers::mod::arp::pat;
	namespace pn = feathers::mod::arp::pattern;
	namespace gn = feathers::mod::arp::genes;
	ModFixture f;
	const size_t slot = f.bridge.addRackDevice(pt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxRackSlots);
	f.bridge.refresh();
	const DeviceInstanceId id = f.bridge.rackDeviceAt(slot);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, id, pt::kDescriptor);
	auto* window = dynamic_cast<ui::PatternExpanded*>(view.get());
	REQUIRE(window != nullptr);
	window->setBounds(0.0f, 0.0f, 900.0f, 700.0f);
	f.deviceHost.setField(id, pt::at(1, pt::Field::Zone), 250.0f);
	f.deviceHost.setField(id, pt::at(1, pt::Field::Mode), 3.0f); // TB-Mono
	window->setFocus(1);
	ui::PatternTerrain& terrain = window->terrain();
	const size_t builds = terrain.buildsForTest();
	MESSAGE("one terrain build (" << ui::PatternTerrain::kWide << "x" << ui::PatternTerrain::kHigh
	                              << " cells, 16 steps each): " << terrain.lastBuildMsForTest() << " ms");
	CHECK(terrain.lastBuildMsForTest() < 2000.0);
	// A CELL BY VALUE: the restated decode at its place.
	std::vector<float> row(pt::kParamCount);
	for (size_t i = 0; i < row.size(); ++i) {
		row[i] = f.deviceHost.fieldAt(id, i);
	}
	pt::Settings s = pt::settingsOf(row.data(), row.size());
	for (const auto& [x, y] : { std::pair<size_t, size_t> { 0, 0 }, { 7, 13 }, { 31, 23 } }) {
		const std::array<double, 2> at = terrain.placeAt((x + 0.5) / ui::PatternTerrain::kWide, (y + 0.5) / ui::PatternTerrain::kHigh);
		pt::PatternSettings c = s.patterns[1];
		c.placeX = static_cast<float>(at[0]);
		c.placeY = static_cast<float>(at[1]);
		c.length = 16;
		pt::Settings cell = s;
		cell.patterns[1] = c;
		std::array<bool, gn::kPinCount> on {};
		on = gn::kPinOnByDefault; // the map decodes at the switches a fresh pattern has
		pn::Recipe r = pt::recipeFor(cell, 1, on);
		r.voice = false;
		feathers::mod::arp::HeldChord held;
		held.press(60, 0, 0.8f, 1);
		pn::PatternCycle cycle;
		pn::build(pt::PatternDevice::placeOf(c, feathers::SliceResidue<gn::kRowCount> {}), r, held, cycle);
		CAPTURE(x);
		CAPTURE(y);
		CHECK(terrain.cellForTest(x, y) == ui::paletteOf(gn::Mode::TbMono, ui::featuresOf(cycle, gn::Mode::TbMono)));
	}
	// NOT REBUILT WHEN NOTHING MOVED; REBUILT WHEN AN INPUT DID.
	CHECK_FALSE(terrain.poll());
	CHECK(terrain.buildsForTest() == builds);
	f.deviceHost.setField(id, pt::at(1, pt::Field::Gamma), 3.0f);
	CHECK(terrain.poll());
	// THE PUCK: a move writes pattern 2's Place X/Y, the place the map's point stands for.
	terrain.moveTo(0.75, 0.25);
	const std::array<double, 2> want = terrain.placeAt(0.75, 0.25);
	CHECK(f.deviceHost.fieldAt(id, pt::at(1, pt::Field::PlaceX)) == doctest::Approx(want[0]).epsilon(1e-5));
	CHECK(f.deviceHost.fieldAt(id, pt::at(1, pt::Field::PlaceY)) == doctest::Approx(want[1]).epsilon(1e-5));
	CHECK(f.deviceHost.fieldAt(id, pt::at(0, pt::Field::PlaceX)) == 0.0f); // pattern 1 untouched
	// THE PALETTE IS A FUNCTION OF THE FEATURES ALONE: the same features, the same colour; each family its own.
	const ui::PatternFeatures mid { 0.5f, 0.5f, 0.5f };
	CHECK(ui::paletteOf(gn::Mode::Melody, mid) == ui::paletteOf(gn::Mode::MelodyMono, mid));
	CHECK(ui::paletteOf(gn::Mode::Melody, mid) != ui::paletteOf(gn::Mode::Chords, mid));
	CHECK(ui::paletteOf(gn::Mode::Chords, mid) != ui::paletteOf(gn::Mode::TbMono, mid));
	REQUIRE(f.bridge.removeRackDevice(slot));
}
TEST_CASE("the Pattern window's heart: a like keeps the focused slot's place and what it played there, and the terrain draws its place brighter") {
	// BY DESIGN, likes are kept per pattern slot. By value: the block holds one like in slot 3 and none elsewhere; its
	// fingerprint scores 1 at the puck; the cell under the puck is drawn toward white from its base colour.
	namespace pt = feathers::mod::arp::pat;
	namespace lk = feathers::mod::arp::likes;
	ModFixture f;
	const size_t slot = f.bridge.addRackDevice(pt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxRackSlots);
	f.bridge.refresh();
	const DeviceInstanceId id = f.bridge.rackDeviceAt(slot);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, id, pt::kDescriptor);
	auto* window = dynamic_cast<ui::PatternExpanded*>(view.get());
	REQUIRE(window != nullptr);
	window->setBounds(0.0f, 0.0f, 900.0f, 700.0f);
	f.deviceHost.setField(id, pt::at(2, pt::Field::Zone), 600.0f);
	window->setFocus(2);
	ui::PatternTerrain& terrain = window->terrain();
	REQUIRE(terrain.like());
	std::vector<float> block(lk::kBlockWords);
	REQUIRE(f.deviceHost.blockAt(id, pt::kLikesBlock, block));
	lk::AllLikes all {};
	REQUIRE(lk::read(block.data(), block.size(), &all));
	CHECK(all[2].count == 1);
	CHECK(all[0].count + all[1].count + all[3].count == 0);
	CHECK(all[2].like[0].zone == 600.0f);
	CHECK(terrain.poll()); // the next frame rebuilds the map with the like (the heart itself does not wait for it)
	// THE PUCK'S PLACE SCORES 1, and its cell (the centre of the map) is brighter than its base.
	CHECK(terrain.scoreAt(0.5, 0.5) == doctest::Approx(1.0));
	const uint32_t base = terrain.baseForTest(ui::PatternTerrain::kWide / 2, ui::PatternTerrain::kHigh / 2);
	const uint32_t lit = terrain.cellForTest(ui::PatternTerrain::kWide / 2, ui::PatternTerrain::kHigh / 2);
	const auto sum = [](uint32_t c) { return ((c >> 16) & 0xFFu) + ((c >> 8) & 0xFFu) + (c & 0xFFu); };
	CHECK(sum(lit) > sum(base));
	REQUIRE(f.bridge.removeRackDevice(slot));
}
TEST_CASE("the Pattern card's selector knob is labelled Pattern and reads compactly — '1 · C Maj' under Pool, '2 · Dor' under Transpose") {
	namespace pt = feathers::mod::arp::pat;
	ModFixture f;
	const size_t slot = f.bridge.addRackDevice(pt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxRackSlots);
	f.bridge.refresh();
	f.panel->resized();
	auto* card = dynamic_cast<const ui::PatternCard*>(f.panel->cardFrameForTest(slot));
	REQUIRE(card != nullptr);
	const DeviceInstanceId id = card->device();
	const ui::Knob* knob = &card->selectorKnob();
	REQUIRE(knob->control() != nullptr);
	CHECK(std::string(knob->label()) == "Pattern");
	f.deviceHost.setField(id, pt::at(pt::Global::Keys), 1.0f); // Pool: the root is the box's
	CHECK(knob->control()->text() == "1 · C Maj");
	f.deviceHost.setField(id, pt::at(pt::Global::Keys), 0.0f); // Transpose: the key's
	f.deviceHost.setField(id, pt::at(1, pt::Field::Enable), 1.0f);
	f.deviceHost.setField(id, pt::at(1, pt::Field::Scale), 1.0f); // Dorian
	f.deviceHost.setField(id, pt::at(pt::Global::Selector), 0.9f);
	CHECK(knob->control()->text() == "2 · Dor");
	REQUIRE(f.bridge.removeRackDevice(slot));
}

namespace {
/// THE PATTERN'S CONTROLS a frame offers while EFFECTIVELY visible, by descriptor identity (the tooltip guard's walk).
std::set<size_t> patternOffers(const visage::Frame* root) {
	namespace pt = feathers::mod::arp::pat;
	std::set<size_t> out;
	std::vector<ui::ControlFace::Offered> list;
	std::vector<const visage::Frame*> stack { root };
	while (!stack.empty()) {
		const visage::Frame* fr = stack.back();
		stack.pop_back();
		if (!fr->isVisible()) {
			continue;
		}
		if (fr != root) {
			if (const auto* face = dynamic_cast<const ui::ControlFace*>(fr)) {
				list.clear();
				face->offeredControls(list);
				for (const ui::ControlFace::Offered& o : list) {
					for (size_t i = 0; o.control != nullptr && i < pt::kDescriptor.params.size(); ++i) {
						if (&o.control->desc() == &pt::kDescriptor.params[i]) {
							out.insert(i);
						}
					}
				}
			}
		}
		for (const visage::Frame* c : fr->children()) {
			stack.push_back(c);
		}
	}
	return out;
}
} // namespace

TEST_CASE("the Pattern card and window show ONE TAB at a time: at each focus exactly that pattern's controls are drawn, none of the other three's (every tab's E and S on the strip)") {
	// All four tabs' controls were visible on the card whatever the tab; the face shows one tab at a time.
	// BY VALUE, at every focus, on both surfaces: the per-pattern controls drawn are
	// exactly the focused pattern's — on the card its Mode, Scale, Rate and Length; in the window every one of its controls
	// that is live under its gate (its pins as the pill band draws them: the ones its Mode reads) — and the E and S of
	// all four, which are the tab strip's.
	namespace pt = feathers::mod::arp::pat;
	namespace gn = feathers::mod::arp::genes;
	using Field = pt::Field;
	ModFixture f;
	const size_t slot = f.bridge.addRackDevice(pt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxRackSlots);
	f.bridge.refresh();
	f.panel->resized();
	auto* card = const_cast<ui::PatternCard*>(dynamic_cast<const ui::PatternCard*>(f.panel->cardFrameForTest(slot)));
	REQUIRE(card != nullptr);
	const DeviceInstanceId id = card->device();
	// THE PATTERNS DIFFER, so a gate that hides a field in one shows it in another: 2 is TB-Glide, 4 is Chords.
	f.deviceHost.setField(id, pt::at(1, Field::Mode), static_cast<float>(gn::Mode::TbGlide));
	f.deviceHost.setField(id, pt::at(3, Field::Mode), static_cast<float>(gn::Mode::Chords));
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, id, pt::kDescriptor);
	auto* window = dynamic_cast<ui::PatternExpanded*>(view.get());
	REQUIRE(window != nullptr);
	window->setBounds(0.0f, 0.0f, 900.0f, 1400.0f);
	const auto patternOf = [](size_t i) { return (i - pt::kGlobalCount) / pt::kFieldCount; };
	const auto fieldOf = [](size_t i) { return static_cast<Field>((i - pt::kGlobalCount) % pt::kFieldCount); };
	const auto isTabChip = [&](size_t i) { return fieldOf(i) == Field::Enable || fieldOf(i) == Field::Solo; };
	// THE GATE, restated from the descriptor and the row (a chain of one hop is all this device declares).
	const auto liveNow = [&](size_t i) {
		const auto& d = pt::kParamDescs[i];
		return d.gateControl < 0
		       || feathers::device::relevantFor(d, f.deviceHost.fieldAt(id, static_cast<size_t>(d.gateControl)));
	};
	for (size_t focus = 0; focus < pt::kPatterns; ++focus) {
		CAPTURE(focus);
		card->showPage(focus);
		window->showPage(focus);
		const std::set<size_t> onCard = patternOffers(card);
		const std::set<size_t> inWindow = patternOffers(window);
		std::set<size_t> cardWant;
		std::set<size_t> windowWant;
		const auto mode = static_cast<gn::Mode>(std::lround(f.deviceHost.fieldAt(id, pt::at(focus, Field::Mode))));
		for (size_t i = pt::kGlobalCount; i < pt::kParamCount; ++i) {
			if (isTabChip(i)) {
				cardWant.insert(i);
				windowWant.insert(i);
				continue;
			}
			// THE WINDOW'S TABS ALSO CARRY EACH TAB'S F (and, on a fill, Go): design §7.8. The card's do not.
			if (fieldOf(i) == Field::Fill || (fieldOf(i) == Field::FillTrigger && liveNow(i))) {
				windowWant.insert(i);
				continue;
			}
			if (patternOf(i) != focus) {
				continue;
			}
			// THE LANE HOLDS THE FOCUSED TAB'S START AND END MARKERS (End drawn only on a fill; held always).
			if (fieldOf(i) == Field::StartPhase || fieldOf(i) == Field::EndPhase) {
				windowWant.insert(i);
				continue;
			}
			const Field fld = fieldOf(i);
			if (fld == Field::Mode || fld == Field::Scale || fld == Field::Rate || fld == Field::Length) {
				cardWant.insert(i);
			}
			if (static_cast<size_t>(fld) >= static_cast<size_t>(Field::DrumNote0)) {
				if (liveNow(i)) {
					windowWant.insert(i); // DRUM'S LANE BANK, in Drum alone
				}
				continue;
			}
			if (static_cast<size_t>(fld) >= static_cast<size_t>(Field::Pin0)) {
				const auto pin = static_cast<gn::Pin>(static_cast<size_t>(fld) - static_cast<size_t>(Field::Pin0));
				if (gn::pinRead(mode, pin)) {
					windowWant.insert(i);
				}
				continue;
			}
			if (liveNow(i)) {
				windowWant.insert(i);
			}
		}
		std::set<size_t> cardPer;
		std::set<size_t> windowPer;
		for (const size_t i : onCard) {
			if (i >= pt::kGlobalCount) {
				cardPer.insert(i);
			}
		}
		for (const size_t i : inWindow) {
			if (i >= pt::kGlobalCount) {
				windowPer.insert(i);
			}
		}
		CHECK(cardPer == cardWant);
		CHECK(windowPer == windowWant);
		// AND THE WINDOW STILL HOLDS EVERYTHING THE CARD DOES, at this focus.
		CHECK(std::includes(inWindow.begin(), inWindow.end(), onCard.begin(), onCard.end()));
	}
	REQUIRE(f.bridge.removeRackDevice(slot));
}
TEST_CASE("Pattern's Cycle (design §7.6): on Cycle the selector is not drawn and the focused tab's Next and Repeats take its place on the card, every tab's cell is on the window's strip, Seed is always drawn, and the card still fits the chain unit") {
	namespace pt = feathers::mod::arp::pat;
	using Field = pt::Field;
	ModFixture f;
	const size_t slot = f.bridge.addRackDevice(pt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxRackSlots);
	f.bridge.refresh();
	f.panel->resized();
	auto* card = const_cast<ui::PatternCard*>(dynamic_cast<const ui::PatternCard*>(f.panel->cardFrameForTest(slot)));
	REQUIRE(card != nullptr);
	const DeviceInstanceId id = card->device();
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, id, pt::kDescriptor);
	auto* window = dynamic_cast<ui::PatternExpanded*>(view.get());
	REQUIRE(window != nullptr);
	window->setBounds(0.0f, 0.0f, 900.0f, 1400.0f);
	const size_t selector = pt::at(pt::Global::Selector);
	const size_t seed = pt::at(pt::Global::Seed);
	const size_t playMode = pt::at(pt::Global::PlayMode);
	const auto cells = [&](size_t p) { return std::set<size_t> { pt::at(p, Field::Repeats), pt::at(p, Field::Next) }; };
	const auto has = [](const std::set<size_t>& s, size_t i) { return s.count(i) != 0; };
	for (const int mode : { 0, 1, 0 }) {
		CAPTURE(mode);
		f.deviceHost.setField(id, playMode, static_cast<float>(mode));
		(void)card->updateContent();
		(void)window->updateContent();
		for (size_t focus = 0; focus < pt::kPatterns; ++focus) {
			CAPTURE(focus);
			card->showPage(focus);
			window->showPage(focus);
			const std::set<size_t> onCard = patternOffers(card);
			const std::set<size_t> inWindow = patternOffers(window);
			// PLAY MODE IS ON BOTH (the card's chip, the window's box).
			CHECK(has(onCard, playMode));
			CHECK(has(inWindow, playMode));
			// THE SELECTOR: drawn on Selector, not on Cycle (the honesty rule), on both.
			CHECK(has(onCard, selector) == (mode == 0));
			CHECK(has(inWindow, selector) == (mode == 0));
			for (size_t p = 0; p < pt::kPatterns; ++p) {
				CAPTURE(p);
				for (const size_t i : cells(p)) {
					// THE CARD: the focused tab's cell alone, on Cycle. THE WINDOW: every tab's, on Cycle.
					CHECK(has(onCard, i) == (mode == 1 && p == focus));
					CHECK(has(inWindow, i) == (mode == 1));
				}
			}
			CHECK(std::includes(inWindow.begin(), inWindow.end(), onCard.begin(), onCard.end()));
			checkFitsChainUnit(*card);
		}
	}
	// THE WINDOW'S CELLS ARE ON THEIR TABS: each inside its tab's column, under the number's row.
	f.deviceHost.setField(id, playMode, 1.0f);
	(void)window->updateContent();
	window->showPage(0);
	const ui::PatternTabs& tabs = window->tabsForTest();
	for (size_t p = 0; p < pt::kPatterns; ++p) {
		CAPTURE(p);
		const visage::Bounds tab = tabs.tabBounds(p);
		for (const ui::ValueBox* box : { &window->cycleRepeatsForTest(p), &window->cycleNextForTest(p) }) {
			const float x = box->x() - tabs.x();
			const float y = box->y() - tabs.y();
			CHECK(x >= tab.x());
			CHECK(x + box->width() <= tab.x() + tab.width());
			CHECK(y >= ui::PatternTabs::kNumberRow - 2.0f);
			CHECK(y + box->height() <= tabs.height());
		}
	}
	// SEED IS ALWAYS DRAWN in the window ("we should expose the random seed on pattern"): every feel,
	// either Play Mode — it rolls Humanize's dice and Cycle's Random Next.
	for (size_t feel = 0; feel < std::size(feathers::mod::arp::humanize::kModeNames); ++feel) {
		for (const float mode : { 0.0f, 1.0f }) {
			CAPTURE(feel);
			CAPTURE(mode);
			f.deviceHost.setField(id, pt::at(pt::Global::HumanizeMode), static_cast<float>(feel));
			f.deviceHost.setField(id, playMode, mode);
			(void)window->updateContent();
			CHECK(has(patternOffers(window), seed));
		}
	}
	REQUIRE(f.bridge.removeRackDevice(slot));
}
TEST_CASE("Pattern's per-tab Transpose (design §7.7): the window shows the focused tab's pair in its panel (hidden in Drum), the device's pair stays, and Copy-to copies the tab's pair") {
	namespace pt = feathers::mod::arp::pat;
	namespace gn = feathers::mod::arp::genes;
	using Field = pt::Field;
	ModFixture f;
	const size_t slot = f.bridge.addRackDevice(pt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxRackSlots);
	f.bridge.refresh();
	const DeviceInstanceId id = f.bridge.rackDeviceAt(slot);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, id, pt::kDescriptor);
	auto* window = dynamic_cast<ui::PatternExpanded*>(view.get());
	REQUIRE(window != nullptr);
	window->setBounds(0.0f, 0.0f, 900.0f, 1400.0f);
	const auto has = [](const std::set<size_t>& s, size_t i) { return s.count(i) != 0; };
	for (size_t focus = 0; focus < pt::kPatterns; ++focus) {
		CAPTURE(focus);
		window->showPage(focus);
		const std::set<size_t> shown = patternOffers(window);
		for (size_t p = 0; p < pt::kPatterns; ++p) {
			CHECK(has(shown, pt::at(p, Field::TransposeSteps)) == (p == focus));
			CHECK(has(shown, pt::at(p, Field::TransposeOctaves)) == (p == focus));
		}
		CHECK(has(shown, pt::at(pt::Global::TransposeSteps)));
		CHECK(has(shown, pt::at(pt::Global::TransposeOctaves)));
	}
	// DRUM HIDES THE TAB'S PAIR (its lanes are fixed notes); the device's pair stays while a pitched tab is in range.
	f.deviceHost.setField(id, pt::at(1, Field::Enable), 1.0f);
	f.deviceHost.setField(id, pt::at(1, Field::Mode), static_cast<float>(gn::Mode::Drum));
	window->showPage(1);
	(void)window->updateContent();
	{
		const std::set<size_t> shown = patternOffers(window);
		CHECK_FALSE(has(shown, pt::at(1, Field::TransposeSteps)));
		CHECK(has(shown, pt::at(pt::Global::TransposeSteps)));
	}
	// COPY TO 3: tab 1's pair travels with it.
	f.deviceHost.setField(id, pt::at(0, Field::TransposeSteps), -5.0f);
	f.deviceHost.setField(id, pt::at(0, Field::TransposeOctaves), 2.0f);
	ui::choosePatternTabMenu(f.deviceHost, id, 0, 2, nullptr);
	CHECK(f.deviceHost.fieldAt(id, pt::at(2, Field::TransposeSteps)) == -5.0f);
	CHECK(f.deviceHost.fieldAt(id, pt::at(2, Field::TransposeOctaves)) == 2.0f);
	CHECK(f.deviceHost.fieldAt(id, pt::at(3, Field::TransposeSteps)) == 0.0f); // and nowhere else
	REQUIRE(f.bridge.removeRackDevice(slot));
}
TEST_CASE("Pattern's phase markers and fill cells (design §7.8): the window's lane draws Start always and a fill's End; dragging a handle sets the field, kept in order; F, Go and Span sit on the tab") {
	namespace pt = feathers::mod::arp::pat;
	using Field = pt::Field;
	ModFixture f;
	const size_t slot = f.bridge.addRackDevice(pt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxRackSlots);
	f.bridge.refresh();
	const DeviceInstanceId id = f.bridge.rackDeviceAt(slot);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, id, pt::kDescriptor);
	auto* window = dynamic_cast<ui::PatternExpanded*>(view.get());
	REQUIRE(window != nullptr);
	ui::EditHistory history;
	window->setModulation(nullptr, &history);
	window->setBounds(0.0f, 0.0f, 900.0f, 1400.0f);
	(void)window->updateContent();
	ui::PatternLane& lane = window->lane();
	REQUIRE(lane.cycleForTest().length == 16);
	const auto has = [](const std::set<size_t>& s, size_t i) { return s.count(i) != 0; };
	// A NORMAL TAB: Start offered and drawn at 0; End not (the honesty rule: it only means something on a fill).
	CHECK(lane.startMarkForTest() == 0);
	CHECK_FALSE(lane.fillForTest());
	{
		std::vector<ui::ControlFace::Offered> offered;
		lane.offeredControls(offered);
		REQUIRE(offered.size() == 2); // Start and End (held; End is drawn only on a fill)
		CHECK(&offered[0].control->desc() == &pt::kDescriptor.params[pt::at(0, Field::StartPhase)]);
	}
	CHECK(lane.markAtForTest(lane.markX(0)) == 1);
	CHECK(lane.markAtForTest(lane.markX(16)) == 0); // no End on a normal tab
	// DRAG START from step 0 to step 4 by its handle: the field moves, one undo step.
	const float column = lane.xOfStep(1) - lane.xOfStep(0);
	lane.onMouseDown().callback(at(lane.markX(0), 3.0f));
	lane.onMouseDrag().callback(at(lane.markX(0) + 4.0f * column, 3.0f));
	lane.onMouseUp().callback(at(lane.markX(0) + 4.0f * column, 3.0f));
	CHECK(f.deviceHost.fieldAt(id, pt::at(0, Field::StartPhase)) == 4.0f);
	(void)lane.poll();
	CHECK(lane.startMarkForTest() == 4);
	CHECK(history.length() == 1);
	history.undo();
	CHECK(f.deviceHost.fieldAt(id, pt::at(0, Field::StartPhase)) == 0.0f);
	history.redo();
	// A FILL: F on the tab makes it one; End appears at the cycle's end; dragging End below Start stops one past it.
	{
		ui::SwitchChip& fill = window->fillChipForTest(0);
		fill.onMouseDown().callback(at(2.0f, 2.0f));
		fill.onMouseUp().callback(at(2.0f, 2.0f));
	}
	CHECK(f.deviceHost.fieldAt(id, pt::at(0, Field::Fill)) == 1.0f);
	(void)window->updateContent();
	CHECK(lane.fillForTest());
	CHECK(lane.endMarkForTest() == 16);
	CHECK(window->goChipForTest(0).isVisible());
	CHECK_FALSE(window->goChipForTest(1).isVisible()); // tab 2 is no fill
	lane.onMouseDown().callback(at(lane.markX(16), 3.0f));
	lane.onMouseDrag().callback(at(lane.markX(1), 3.0f)); // far below Start (4)
	lane.onMouseUp().callback(at(lane.markX(1), 3.0f));
	CHECK(f.deviceHost.fieldAt(id, pt::at(0, Field::EndPhase)) == 5.0f); // Start + 1
	(void)lane.poll();
	CHECK(lane.endMarkForTest() == 5);
	// GO is a momentary on the Fill Trigger: held high while pressed, back to 0 on release (a rise the device fires on).
	{
		ui::SwitchChip& go = window->goChipForTest(0);
		go.onMouseDown().callback(at(2.0f, 2.0f));
		CHECK(f.deviceHost.fieldAt(id, pt::at(0, Field::FillTrigger)) == 1.0f);
		go.onMouseUp().callback(at(2.0f, 2.0f));
		CHECK(f.deviceHost.fieldAt(id, pt::at(0, Field::FillTrigger)) == 0.0f);
	}
	// SPAN on the tab's cycle cell, on Cycle only.
	CHECK_FALSE(window->cycleSpanForTest(2).isVisible());
	f.deviceHost.setField(id, pt::at(pt::Global::PlayMode), 1.0f);
	(void)window->updateContent();
	const std::set<size_t> shown = patternOffers(window);
	for (size_t p = 0; p < pt::kPatterns; ++p) {
		CHECK(has(shown, pt::at(p, Field::Span)));
		CHECK(has(shown, pt::at(p, Field::Fill)));
	}
	REQUIRE(f.bridge.removeRackDevice(slot));
}

namespace {
/// THE OS SAVE DIALOG AS A CASE SEES IT: what was asked, and the path the case answers with.
class FakeSave final : public ui::FileDialogBackend {
public:
	class Session final : public ui::FileDialogBackend::Session {
	public:
		void cancel() override {}
	};
	std::unique_ptr<ui::FileDialogBackend::Session> start(const ui::FileDialogOptions& options,
	                                                     std::shared_ptr<ui::FileDialogChannel> channel) override {
		last = options;
		this->channel = std::move(channel);
		return std::make_unique<Session>();
	}
	ui::FileDialogOptions last;
	std::shared_ptr<ui::FileDialogChannel> channel;
};
std::vector<uint8_t> bytesOf(const std::filesystem::path& path) {
	std::ifstream in(path, std::ios::binary);
	return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
} // namespace

TEST_CASE("Pattern's MIDI (design §7.9): each tab's grip is a drag source that writes the pattern's .mid at the press and offers its path; Export MIDI… writes the same bytes where the dialog says") {
	namespace pt = feathers::mod::arp::pat;
	const std::filesystem::path scratch = std::filesystem::temp_directory_path() / "feathers-midi-grip-case";
	std::filesystem::remove_all(scratch);
	const char* before = std::getenv("XDG_CACHE_HOME");
	const std::string kept = before != nullptr ? before : "";
	::setenv("XDG_CACHE_HOME", scratch.c_str(), 1);
	ModFixture f;
	const size_t slot = f.bridge.addRackDevice(pt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxRackSlots);
	f.bridge.refresh();
	const DeviceInstanceId id = f.bridge.rackDeviceAt(slot);
	f.deviceHost.setField(id, pt::at(1, pt::Field::Enable), 1.0f);
	f.deviceHost.setField(id, pt::at(1, pt::Field::Mode), static_cast<float>(feathers::mod::arp::genes::Mode::Chords));
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, id, pt::kDescriptor);
	auto* window = dynamic_cast<ui::PatternExpanded*>(view.get());
	REQUIRE(window != nullptr);
	window->setBounds(0.0f, 0.0f, 900.0f, 1400.0f);
	const std::vector<uint8_t> want = pt::patternMidi(ui::midiSourceOf(f.deviceHost, id), 1, ui::midiTempoOf(f.deviceHost));
	REQUIRE(want.size() > 40);
	// THE GRIP: a drag source; the press writes the file into the cache folder and hands visage its path.
	ui::MidiGrip& grip = window->midiGripForTest(1);
	CHECK(grip.isVisible());
	CHECK(grip.isDragDropSource());
	const std::string path = grip.startDragDropSource();
	CHECK(std::filesystem::path(path) == scratch / "feathers" / "midi" / "Pattern 2.mid");
	CHECK(bytesOf(path) == want);
	// EXPORT MIDI…: a save dialog for a .mid named after the tab, and the same bytes where it says.
	FakeSave dialogs;
	ui::ScopedFileDialogBackend useFake(dialogs);
	REQUIRE(window->exportMidi(1));
	CHECK(dialogs.last.mode == ui::FileDialogOptions::Mode::SaveFile);
	CHECK(dialogs.last.defaultName == "Pattern 2.mid");
	const std::filesystem::path chosen = scratch / "chosen" / "my loop.mid";
	std::filesystem::create_directories(chosen.parent_path());
	dialogs.channel->complete({ ui::FileDialogResult::Status::Chosen, chosen, {} });
	CHECK(window->exportDialogForTest().poll());
	CHECK(bytesOf(chosen) == want);
	REQUIRE(f.bridge.removeRackDevice(slot));
	if (before != nullptr) {
		::setenv("XDG_CACHE_HOME", kept.c_str(), 1);
	}
	else {
		::unsetenv("XDG_CACHE_HOME");
	}
	std::filesystem::remove_all(scratch);
}
TEST_CASE("Pattern's MIDI grip writes into the platform's cache: ~/Library/Caches/Feathers/MIDI on macOS (XDG ignored), XDG or ~/.cache elsewhere") {
	// ASKED OF THE PURE RULE, so the macOS answer is checked on any host. macOS normally has no XDG_CACHE_HOME, and its
	// per-user cache is ~/Library/Caches; an XDG value set there is not the platform's and is not followed.
	using ui::MidiGrip;
	CHECK(MidiGrip::directoryFor(nullptr, "/Users/a", true) == std::filesystem::path("/Users/a/Library/Caches/Feathers/MIDI"));
	CHECK(MidiGrip::directoryFor("/x/cache", "/Users/a", true) == std::filesystem::path("/Users/a/Library/Caches/Feathers/MIDI"));
	CHECK(MidiGrip::directoryFor("/x/cache", "/home/a", false) == std::filesystem::path("/x/cache/feathers/midi"));
	CHECK(MidiGrip::directoryFor(nullptr, "/home/a", false) == std::filesystem::path("/home/a/.cache/feathers/midi"));
	CHECK(MidiGrip::directoryFor("", "/home/a", false) == std::filesystem::path("/home/a/.cache/feathers/midi"));
	// NO HOME: the temp dir, on either.
	std::error_code ec;
	const std::filesystem::path temp = std::filesystem::temp_directory_path(ec) / "feathers" / "midi";
	CHECK(MidiGrip::directoryFor(nullptr, nullptr, true) == temp);
	CHECK(MidiGrip::directoryFor(nullptr, "", false) == temp);
#if !defined(__APPLE__)
	// AND THIS HOST'S `directory()` IS THE RULE'S non-macOS answer for its own environment.
	CHECK(MidiGrip::directory() == MidiGrip::directoryFor(std::getenv("XDG_CACHE_HOME"), std::getenv("HOME"), false));
#endif
}
TEST_CASE("KW: the window header's enable chip is a modulation destination — armed, a press routes the armed source into it (and again unroutes), and the chip shows the MODULATED switch") {
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	auto* overlay = f.gui.expandedForTest();
	REQUIRE(lane != nullptr);
	REQUIRE(overlay != nullptr);
	const size_t slot = f.bridge.addFxDevice(fx::harm::kTraits.tag);
	f.bridge.refresh();
	lane->resized();
	lane->scrollCardIntoView(slot);
	lane->resized();
	(void)f.gui.canvasForTest()->setCollapsed(true);
	lane->onExpandDevice(slot);
	REQUIRE(overlay->open());
	ui::ExpandedPanel* chrome = overlay->panelForTest();
	REQUIRE(chrome != nullptr);
	const visage::Bounds sw = chrome->enableBounds();
	REQUIRE(sw.width() > 0.0f);
	const auto enable = static_cast<size_t>(fx::harm::kTraits.enableControl);
	REQUIRE(fx::harm::kDescriptor.params[enable].modulatable);
	const DeviceInstanceId device = f.bridge.fxDeviceAt(slot);
	const backplane::Destination dest = backplane::Destination::control(device, enable);
	const auto press = [&] {
		chrome->onMouseDown().callback(at(sw.x() + sw.width() * 0.5f, sw.y() + sw.height() * 0.5f));
		chrome->onMouseUp().callback(at(sw.x() + sw.width() * 0.5f, sw.y() + sw.height() * 0.5f));
	};

	// NOT ARMED: a press is the toggle, as it always was.
	REQUIRE(f.deviceHost.fieldAt(device, enable) == 1.0f);
	press();
	CHECK(f.deviceHost.fieldAt(device, enable) == 0.0f);
	press();
	REQUIRE(f.deviceHost.fieldAt(device, enable) == 1.0f);

	// ARMED: the press routes macro 0 into the enable at full amount, and the switch's own value stays where it was.
	f.arm(backplane::Source::core(0));
	(void)chrome->syncEnable();
	CHECK(chrome->enableMarksForTest().glow);
	press();
	CHECK(f.bridge.routeAmount(backplane::Source::core(0), dest) == 1.0f);
	CHECK(f.deviceHost.fieldAt(device, enable) == 1.0f);
	// AND AGAIN: unrouted.
	press();
	CHECK(f.bridge.routeAmount(backplane::Source::core(0), dest) == 0.0f);

	// THE CHIP SAYS WHAT THE DEVICE IS: the hand's switch on, the modulated reading off, and the chip shows off.
	feathers::FxChain movedFx = f.bridge.fxChain();
	movedFx.device[slot].row.field[enable] = 0.0f;
	f.bridge.publishModulated(f.bridge.modRack(), movedFx, feathers::RackPlayheads {}, feathers::mod::TransportClock::unknown());
	REQUIRE(f.deviceHost.modulatedFieldAt(device, enable) == 0.0f);
	(void)chrome->syncEnable();
	CHECK_FALSE(chrome->enableShownForTest());
	CHECK(f.deviceHost.fieldAt(device, enable) == 1.0f);
}
TEST_CASE("Rotate keeps the pattern under the puck: a turn banks its difference into the hand's block, once, so the place decodes as before") {
	// FEATHERS' RULE (A10 superseded 2026-08-13), on the Pattern terrain: turning the plane re-aims where travel GOES from
	// here; the pattern standing at the puck stays. By value: every row's phase at the place, before and after the turn.
	namespace pt = feathers::mod::arp::pat;
	namespace gn = feathers::mod::arp::genes;
	ModFixture f;
	const size_t slot = f.bridge.addRackDevice(pt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxRackSlots);
	f.bridge.refresh();
	f.panel->resized();
	const DeviceInstanceId id = f.bridge.rackDeviceAt(slot);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, id, pt::kDescriptor);
	auto* window = dynamic_cast<ui::PatternExpanded*>(view.get());
	REQUIRE(window != nullptr);
	window->setBounds(0.0f, 0.0f, 900.0f, 700.0f);
	f.deviceHost.setField(id, pt::at(2, pt::Field::Zone), 333.0f);
	f.deviceHost.setField(id, pt::at(2, pt::Field::PlaceX), 0.7f);
	f.deviceHost.setField(id, pt::at(2, pt::Field::PlaceY), -0.4f);
	(void)window->updateContent(); // the watch sees the pattern as it stands
	// AND SO DOES THE CARD, before the turn: a watch of its own would fold the turn a second time when it next looks.
	auto* card = const_cast<ui::PatternCard*>(dynamic_cast<const ui::PatternCard*>(f.panel->cardFrameForTest(slot)));
	REQUIRE(card != nullptr);
	(void)card->updateContent();
	const auto settingsNow = [&] {
		std::vector<float> row(pt::kParamCount);
		for (size_t i = 0; i < row.size(); ++i) {
			row[i] = f.deviceHost.fieldAt(id, i);
		}
		return pt::settingsOf(row.data(), row.size());
	};
	const auto handNow = [&] {
		std::vector<float> block(pt::kHandWords);
		return f.deviceHost.blockAt(id, 1, block) ? pt::handStateOf(block.data(), block.size()) : pt::HandState {};
	};
	const gn::Place before = pt::PatternDevice::placeOf(settingsNow().patterns[2], handNow()[2].residue);
	f.deviceHost.setField(id, pt::at(2, pt::Field::Rotate), 0.37f);
	CHECK(window->updateContent());
	// THE CARD SHARES THE WATCH: it must NOT fold the same turn a second time.
	(void)card->updateContent();
	const gn::Place after = pt::PatternDevice::placeOf(settingsNow().patterns[2], handNow()[2].residue);
	for (size_t i = 0; i < gn::kRowCount; ++i) {
		CAPTURE(i);
		CHECK(after.phase[i] == doctest::Approx(before.phase[i]).epsilon(1e-9));
		CHECK(after.rate[i] == doctest::Approx(before.rate[i]).epsilon(1e-6));
	}
	// AND THE TURN IS REAL: away from the puck, the plane moved.
	pt::PatternSettings moved = settingsNow().patterns[2];
	moved.placeX += 0.5f;
	pt::PatternSettings movedUnturned = moved;
	movedUnturned.rotate = 0.0f;
	const gn::Place turned = pt::PatternDevice::placeOf(moved, handNow()[2].residue);
	const gn::Place straight = pt::PatternDevice::placeOf(movedUnturned, feathers::SliceResidue<gn::kRowCount> {});
	bool differs = false;
	for (size_t i = 0; i < gn::kRowCount; ++i) {
		differs = differs || std::abs(turned.phase[i] - straight.phase[i]) > 1e-6;
	}
	CHECK(differs);
	REQUIRE(f.bridge.removeRackDevice(slot));
}
TEST_CASE("the Pattern window's pins are FaderPills: the amount is the pin's field, the switch is the hand block's bit, and only the pins the Mode reads are drawn") {
	namespace pt = feathers::mod::arp::pat;
	namespace gn = feathers::mod::arp::genes;
	ModFixture f;
	const size_t slot = f.bridge.addRackDevice(pt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxRackSlots);
	f.bridge.refresh();
	f.panel->resized();
	const DeviceInstanceId id = f.bridge.rackDeviceAt(slot);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, id, pt::kDescriptor);
	auto* window = dynamic_cast<ui::PatternExpanded*>(view.get());
	REQUIRE(window != nullptr);
	window->setBounds(0.0f, 0.0f, 900.0f, 700.0f);
	(void)window->updateContent();
	const auto handNow = [&] {
		std::vector<float> block(pt::kHandWords);
		return f.deviceHost.blockAt(id, 1, block) ? pt::handStateOf(block.data(), block.size()) : pt::HandState {};
	};
	REQUIRE(window->pills().size() == gn::kPinCount);
	const auto pillOf = [&](gn::Pin p) -> ui::FaderPill& { return *window->pills()[static_cast<size_t>(p)]; };
	// THE MODE DECIDES WHAT IS DRAWN: Glide is a TB pin, so Melody hides it and TB-Glide shows it.
	f.deviceHost.setField(id, pt::at(0, pt::Field::Mode), static_cast<float>(gn::Mode::Melody));
	(void)window->updateContent();
	for (size_t i = 0; i < gn::kPinCount; ++i) {
		CAPTURE(gn::kPinNames[i]);
		CHECK(window->pills()[i]->isVisible() == gn::pinRead(gn::Mode::Melody, static_cast<gn::Pin>(i)));
	}
	CHECK_FALSE(pillOf(gn::Pin::Glide).isVisible());
	f.deviceHost.setField(id, pt::at(0, pt::Field::Mode), static_cast<float>(gn::Mode::TbGlide));
	(void)window->updateContent();
	CHECK(pillOf(gn::Pin::Glide).isVisible());
	// THE AMOUNT IS THE PIN'S OWN FIELD, by identity.
	auto& leap = pillOf(gn::Pin::Leap);
	CHECK(&leap.amountForTest().desc() == &pt::kDescriptor.params[pt::pinAt(0, gn::Pin::Leap)]);
	// THE SWITCH: a press on the glyph turns the slot's bit off in the hand's block, and nothing else moves.
	REQUIRE(leap.powerForTest() != nullptr);
	CHECK(handNow()[0].pinOn[static_cast<size_t>(gn::Pin::Leap)]);
	const visage::Bounds g = leap.powerGlyphForTest();
	leap.onMouseDown().callback(at(g.x() + g.width() * 0.5f, g.y() + g.height() * 0.5f));
	leap.onMouseUp().callback(at(g.x() + g.width() * 0.5f, g.y() + g.height() * 0.5f));
	const pt::HandState after = handNow();
	CHECK_FALSE(after[0].pinOn[static_cast<size_t>(gn::Pin::Leap)]);
	CHECK(leap.powerForTest()->value() == 0.0);
	for (size_t s = 0; s < pt::kPatterns; ++s) {
		for (size_t p = 0; p < gn::kPinCount; ++p) {
			CAPTURE(s);
			CAPTURE(p);
			CHECK(after[s].pinOn[p] == (gn::kPinOnByDefault[p] && (s != 0 || p != static_cast<size_t>(gn::Pin::Leap))));
		}
	}
	// FOCUS FOLLOWS THE TAB: pattern 3's pills read pattern 3's fields.
	window->setFocus(3);
	CHECK(&pillOf(gn::Pin::Leap).amountForTest().desc() == &pt::kDescriptor.params[pt::pinAt(3, gn::Pin::Leap)]);
	CHECK(pillOf(gn::Pin::Leap).powerForTest()->value() == 1.0);
	REQUIRE(f.bridge.removeRackDevice(slot));
}
TEST_CASE("the Pattern's pin pills draw as the engine reads them: lit when the pin is on and its Mode reads it, dim only when switched off, hidden when the Mode does not read it") {
	// The field scaler sliders all looked inactive even when they were not. BY VALUE against the
	// engine: a pin is ON when the hand block's bit is set (`recipeFor` hands it to the decode as `pins.on`), and the
	// pill's drawn state (`poweredForTest`) must say exactly that — across a toggle and a Mode change.
	namespace pt = feathers::mod::arp::pat;
	namespace gn = feathers::mod::arp::genes;
	ModFixture f;
	const size_t slot = f.bridge.addRackDevice(pt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxRackSlots);
	f.bridge.refresh();
	const DeviceInstanceId id = f.bridge.rackDeviceAt(slot);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, id, pt::kDescriptor);
	auto* window = dynamic_cast<ui::PatternExpanded*>(view.get());
	REQUIRE(window != nullptr);
	const visage::Point min = window->minimumSize();
	window->setBounds(0.0f, 0.0f, min.x, min.y);
	const auto engineOn = [&](size_t pin) {
		std::vector<float> block(pt::kHandWords);
		const pt::HandState hand = f.deviceHost.blockAt(id, 1, block) ? pt::handStateOf(block.data(), block.size()) : pt::HandState {};
		std::vector<float> row(pt::kParamCount);
		for (size_t i = 0; i < row.size(); ++i) {
			row[i] = f.deviceHost.fieldAt(id, i);
		}
		const feathers::mod::arp::pattern::Recipe r = pt::recipeFor(pt::settingsOf(row.data(), row.size()), 0, hand[0].pinOn);
		return r.pins.on[pin];
	};
	const auto checkAll = [&](gn::Mode mode) {
		for (size_t i = 0; i < gn::kPinCount; ++i) {
			CAPTURE(gn::kPinNames[i]);
			const ui::FaderPill& pill = *window->pills()[i];
			const bool read = gn::pinRead(mode, static_cast<gn::Pin>(i));
			CHECK(pill.isVisible() == read); // HIDDEN, not dimmed, where the Mode does not read it
			if (read) {
				CHECK(pill.poweredForTest() == engineOn(i));
			}
		}
	};
	// AT REST: every pin on in the engine, every pill lit — but RATCHET, opt in ("opt in, field
	// scaler"): drawn in Melody, its switch Off in the engine and dim on the face.
	checkAll(gn::Mode::Melody);
	CHECK(window->pills()[static_cast<size_t>(gn::Pin::Leap)]->poweredForTest());
	CHECK(window->pills()[static_cast<size_t>(gn::Pin::Ratchet)]->isVisible());
	CHECK_FALSE(window->pills()[static_cast<size_t>(gn::Pin::Ratchet)]->poweredForTest());
	CHECK_FALSE(engineOn(static_cast<size_t>(gn::Pin::Ratchet)));
	CHECK_FALSE(window->pills()[static_cast<size_t>(gn::Pin::Detail)]->isVisible()); // Detail no longer ratchets Melody
	CHECK_FALSE(window->pills()[static_cast<size_t>(gn::Pin::Approach)]->isVisible()); // a chord Mode's pin (Chords, Progression)
	CHECK_FALSE(engineOn(static_cast<size_t>(gn::Pin::Approach)));                       // and opt in
	// A TOGGLE: the Leap pill's switch off — off in the engine, dim on the face; and back.
	ui::FaderPill& leap = *window->pills()[static_cast<size_t>(gn::Pin::Leap)];
	const visage::Bounds g = leap.powerGlyphForTest();
	const auto press = [&] {
		leap.onMouseDown().callback(at(g.x() + g.width() * 0.5f, g.y() + g.height() * 0.5f));
		leap.onMouseUp().callback(at(g.x() + g.width() * 0.5f, g.y() + g.height() * 0.5f));
	};
	press();
	CHECK_FALSE(engineOn(static_cast<size_t>(gn::Pin::Leap)));
	checkAll(gn::Mode::Melody);
	press();
	CHECK(engineOn(static_cast<size_t>(gn::Pin::Leap)));
	checkAll(gn::Mode::Melody);
	// A MODE CHANGE: TB reads Glide and not Length; Drum reads neither Range nor Length.
	for (const gn::Mode mode : { gn::Mode::TbGlide, gn::Mode::Drum }) {
		f.deviceHost.setField(id, pt::at(0, pt::Field::Mode), static_cast<float>(mode));
		(void)window->updateContent();
		checkAll(mode);
	}
	REQUIRE(f.bridge.removeRackDevice(slot));
}
TEST_CASE("the Pattern tabs' menu is Drawn's: Copy to 3 makes slot 3 decode exactly slot 1 (its controls, its banked turns, its pin switches, its edits; not its Enable or Solo or likes), Copy to all others covers every slot, and each undoes as one step") {
	namespace pt = feathers::mod::arp::pat;
	namespace pn = feathers::mod::arp::pattern;
	namespace gn = feathers::mod::arp::genes;
	using Field = pt::Field;
	ModFixture f;
	const size_t slot = f.bridge.addRackDevice(pt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxRackSlots);
	f.bridge.refresh();
	const DeviceInstanceId id = f.bridge.rackDeviceAt(slot);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, id, pt::kDescriptor);
	auto* window = dynamic_cast<ui::PatternExpanded*>(view.get());
	REQUIRE(window != nullptr);
	ui::EditHistory history;
	window->setModulation(nullptr, &history);
	const visage::Point min = window->minimumSize();
	window->setBounds(0.0f, 0.0f, min.x, min.y);
	// THE MENU, AS DRAWN'S READS: Reset, then Copy to each other slot, then Copy to all others.
	const auto options = ui::patternTabMenuOptions(0);
	REQUIRE(options.size() == 5);
	CHECK(options[0].second == "Reset to default");
	CHECK(options[1].second == "Copy to 2");
	CHECK(options[3].second == "Copy to 4");
	CHECK(options[4].second == "Copy to all others");
	// SLOT 1, MADE ITS OWN: controls, a Rotate fold, a pin switched off, a hand edit; slot 3's Enable on, Solo off.
	f.deviceHost.setField(id, pt::at(0, Field::Mode), static_cast<float>(gn::Mode::Chords));
	f.deviceHost.setField(id, pt::at(0, Field::Scale), 4.0f);
	f.deviceHost.setField(id, pt::at(0, Field::Zone), 512.0f);
	f.deviceHost.setField(id, pt::at(0, Field::PlaceX), 0.6f);
	f.deviceHost.setField(id, pt::pinAt(0, gn::Pin::Leap), 0.8f);
	f.deviceHost.setField(id, pt::drumNoteAt(0, 2), 44.0f);
	f.deviceHost.setField(id, pt::at(0, Field::Enable), 1.0f);
	f.deviceHost.setField(id, pt::at(2, Field::Enable), 0.0f);
	(void)window->updateContent();
	f.deviceHost.setField(id, pt::at(0, Field::Rotate), 0.45f);
	(void)window->updateContent(); // the fold banks the turn into slot 1's residue
	window->showPage(0);
	ui::FaderPill& hold = *window->pills()[static_cast<size_t>(gn::Pin::Hold)];
	const visage::Bounds g = hold.powerGlyphForTest();
	hold.onMouseDown().callback(at(g.x() + g.width() * 0.5f, g.y() + g.height() * 0.5f));
	hold.onMouseUp().callback(at(g.x() + g.width() * 0.5f, g.y() + g.height() * 0.5f));
	ui::PatternLane& lane = window->lane();
	(void)lane.poll();
	size_t on = 0;
	while (on < lane.cycleForTest().length && lane.cycleForTest().steps[on].kind != pn::PatternStep::Kind::Note) {
		++on;
	}
	REQUIRE(on < lane.cycleForTest().length);
	lane.onMouseDown().callback(doubleAt(lane.xOfStep(on) + 2.0f, lane.yOfKey(lane.cycleForTest().keyAt(on, 0)) + 1.0f));
	lane.onMouseUp().callback(doubleAt(lane.xOfStep(on) + 2.0f, lane.yOfKey(lane.cycleForTest().keyAt(on, 0)) + 1.0f));
	const auto handNow = [&] {
		std::vector<float> b(pt::kHandWords);
		return f.deviceHost.blockAt(id, 1, b) ? pt::handStateOf(b.data(), b.size()) : pt::HandState {};
	};
	const auto editsNow = [&] {
		std::vector<float> b(pt::kEditsWords);
		return f.deviceHost.blockAt(id, pt::kEditsBlock, b) ? pt::editStateOf(b.data(), b.size()) : pt::EditState {};
	};
	REQUIRE(editsNow()[0].any());
	REQUIRE_FALSE(handNow()[0].pinOn[static_cast<size_t>(gn::Pin::Hold)]);
	REQUIRE(handNow()[0].residue != pt::HandSlot {}.residue);
	// THE DECODE A SLOT PLAYS, by the device's own road: its settings, its banked turns, its pin switches, its edits.
	const auto decode = [&](size_t p) {
		std::vector<float> row(pt::kParamCount);
		for (size_t i = 0; i < row.size(); ++i) {
			row[i] = f.deviceHost.fieldAt(id, i);
		}
		const pt::Settings s = pt::settingsOf(row.data(), row.size());
		const pt::HandState hand = handNow();
		feathers::mod::arp::HeldChord held;
		held.press(55, 0, 0.8f, 1);
		pn::PatternCycle c;
		pn::build(pt::PatternDevice::placeOf(s.patterns[p], hand[p].residue), pt::recipeFor(s, p, hand[p].pinOn), held, c);
		const pt::EditState e = editsNow();
		if (e[p].mode == s.patterns[p].mode) {
			pn::applyEdits(e[p].steps, false, c);
		}
		return c;
	};
	const auto same = [&](size_t a, size_t b) {
		CAPTURE(a);
		CAPTURE(b);
		for (size_t k = 0; k < pt::kFieldCount; ++k) {
			const auto fld = static_cast<Field>(k);
			if (fld == Field::Enable || fld == Field::Solo) {
				continue;
			}
			CAPTURE(k);
			CHECK(f.deviceHost.fieldAt(id, pt::at(b, fld)) == f.deviceHost.fieldAt(id, pt::at(a, fld)));
		}
		CHECK(handNow()[b].residue == handNow()[a].residue);
		CHECK(handNow()[b].pinOn == handNow()[a].pinOn);
		CHECK(handNow()[b].zoom == handNow()[a].zoom);
		CHECK(editsNow()[b] == editsNow()[a]);
		const pn::PatternCycle ca = decode(a);
		const pn::PatternCycle cb = decode(b);
		REQUIRE(ca.length == cb.length);
		for (size_t k = 0; k < ca.length; ++k) {
			CAPTURE(k);
			CHECK(cb.steps[k].kind == ca.steps[k].kind);
			CHECK(cb.steps[k].keys == ca.steps[k].keys);
			CHECK(cb.steps[k].velocity == ca.steps[k].velocity);
			CHECK(cb.steps[k].edited == ca.steps[k].edited);
		}
	};
	const pn::PatternCycle slot3Before = decode(2);
	// COPY TO 3.
	ui::choosePatternTabMenu(f.deviceHost, id, 0, 2, &history);
	(void)window->updateContent(); // the watch must not fold the copied Rotate a second time
	same(0, 2);
	CHECK(f.deviceHost.fieldAt(id, pt::at(2, Field::Enable)) == 0.0f); // its own Enable, kept
	// UNDO: slot 3 as it was, whole, in one step.
	history.undo();
	(void)window->updateContent();
	const pn::PatternCycle slot3Undone = decode(2);
	for (size_t k = 0; k < slot3Before.length; ++k) {
		CHECK(slot3Undone.steps[k].kind == slot3Before.steps[k].kind);
		CHECK(slot3Undone.steps[k].keys == slot3Before.steps[k].keys);
	}
	CHECK_FALSE(editsNow()[2].any());
	// COPY TO ALL OTHERS: every slot slot 1.
	ui::choosePatternTabMenu(f.deviceHost, id, 0, ui::kPatternMenuCopyAll, &history);
	(void)window->updateContent();
	for (size_t p = 1; p < pt::kPatterns; ++p) {
		same(0, p);
	}
	// RESET: slot 2 to the defaults, its overlay gone.
	ui::choosePatternTabMenu(f.deviceHost, id, 1, ui::kPatternMenuReset, &history);
	CHECK(f.deviceHost.fieldAt(id, pt::at(1, Field::Mode)) == static_cast<float>(pt::kParamDescs[pt::at(1, Field::Mode)].def));
	CHECK_FALSE(editsNow()[1].any());
	REQUIRE(f.bridge.removeRackDevice(slot));
}
TEST_CASE("the Pattern map pans at the edge as Explore's pad does: a drag held at an edge walks the place that way on time, by the shared law, under Rotate and Zoom; a drag inside does not pan") {
	namespace pt = feathers::mod::arp::pat;
	ModFixture f;
	const size_t slot = f.bridge.addRackDevice(pt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxRackSlots);
	f.bridge.refresh();
	const DeviceInstanceId id = f.bridge.rackDeviceAt(slot);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, id, pt::kDescriptor);
	auto* window = dynamic_cast<ui::PatternExpanded*>(view.get());
	REQUIRE(window != nullptr);
	window->setBounds(0.0f, 0.0f, 900.0f, 1000.0f);
	ui::PatternTerrain& terrain = window->terrain();
	const float w = terrain.width();
	const float h = terrain.height();
	REQUIRE(w > 100.0f);
	const auto placeNow = [&] {
		return std::array<double, 2> { f.deviceHost.fieldAt(id, pt::at(0, pt::Field::PlaceX)),
			                            f.deviceHost.fieldAt(id, pt::at(0, pt::Field::PlaceY)) };
	};
	for (const auto& [rotate, zoom] : { std::pair { 0.0f, 1.0 }, std::pair { 0.7f, 2.0 } }) {
		CAPTURE(rotate);
		CAPTURE(zoom);
		f.deviceHost.setField(id, pt::at(0, pt::Field::Rotate), rotate);
		(void)window->updateContent();
		terrain.setZoom(zoom);
		(void)terrain.poll();
		// INSIDE: a drag that never reaches the band does not pan, however long it is held.
		terrain.onMouseDown().callback(at(w * 0.5f, h * 0.5f));
		terrain.onMouseDrag().callback(at(w * 0.6f, h * 0.4f));
		const auto inside = terrain.viewCentreForTest();
		CHECK_FALSE(terrain.edgeScroll(0.05));
		CHECK(terrain.viewCentreForTest() == inside);
		// AT THE RIGHT EDGE (2 px in: 22 px into the band): the view scrolls by the law's velocity × time × one view,
		// and the place, under the hand, walks right with it.
		const float ex = w - 2.0f;
		const float ey = h * 0.5f;
		terrain.onMouseDrag().callback(at(ex, ey));
		const auto [vx, vy] = ui::edge_pan::velocity(ex, ey, w, h);
		REQUIRE(vx > 0.0);
		CHECK(vy == 0.0);
		double lastX = placeNow()[0];
		for (int tick = 0; tick < 5; ++tick) {
			const auto before = terrain.viewCentreForTest();
			REQUIRE(terrain.edgeScroll(0.05));
			const auto after = terrain.viewCentreForTest();
			CHECK(after[0] - before[0] == doctest::Approx(vx * 0.05 * (2.0 * ui::PatternTerrain::kHalfSpan / zoom)));
			CHECK(after[1] == before[1]);
			// THE PLACE IS THE ONE UNDER THE (INSET-CLAMPED) HAND in the moved view.
			const double inset = ui::edge_pan::kStarInsetPixels;
			const std::array<double, 2> want = terrain.placeAt((std::min<double>(ex, w - inset)) / w, 1.0 - ey / h);
			CHECK(placeNow()[0] == doctest::Approx(want[0]).epsilon(1e-5));
			CHECK(placeNow()[0] > lastX);
			lastX = placeNow()[0];
		}
		// THE TOP EDGE, past it: Y walks up (UP IS MORE).
		terrain.onMouseDrag().callback(at(w * 0.5f, -10.0f));
		const double lastY = placeNow()[1];
		REQUIRE(terrain.edgeScroll(0.05));
		CHECK(placeNow()[1] > lastY);
		terrain.onMouseUp().callback(at(w * 0.5f, -10.0f));
		// LET GO, no pan.
		CHECK_FALSE(terrain.edgeScroll(0.05));
	}
	REQUIRE(f.bridge.removeRackDevice(slot));
}
TEST_CASE("the Pattern map has no fences: a drag reaches every edge of the map at any place, Rotate, Zone and Zoom, and an edge pan keeps walking") {
	// Sometimes the whole terrain was not reachable: invisible fences stopped the puck. Through the real window on the real bridge: a press at each edge must put the place there.
	namespace pt = feathers::mod::arp::pat;
	using Field = pt::Field;
	ModFixture f;
	const size_t slot = f.bridge.addRackDevice(pt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxRackSlots);
	f.bridge.refresh();
	const DeviceInstanceId id = f.bridge.rackDeviceAt(slot);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, id, pt::kDescriptor);
	auto* window = dynamic_cast<ui::PatternExpanded*>(view.get());
	REQUIRE(window != nullptr);
	const visage::Point min = window->minimumSize();
	window->setBounds(0.0f, 0.0f, min.x, min.y);
	ui::PatternTerrain& terrain = window->terrain();
	const float w = terrain.width();
	const float h = terrain.height();
	const auto place = [&] {
		return std::array<double, 2> { f.deviceHost.fieldAt(id, pt::at(0, Field::PlaceX)),
			                            f.deviceHost.fieldAt(id, pt::at(0, Field::PlaceY)) };
	};
	struct Setting {
		float x, y, rotate, zone;
		double zoom;
	};
	for (const Setting s : { Setting { 0.0f, 0.0f, 0.0f, 0.0f, 1.0 }, Setting { 3.5f, -3.5f, 0.0f, 0.0f, 1.0 },
	                         Setting { 3.0f, 2.0f, 1.3f, 700.0f, 0.125 }, Setting { -2.0f, 3.9f, -0.6f, 90.0f, 4.0 } }) {
		CAPTURE(s.x);
		CAPTURE(s.y);
		CAPTURE(s.zoom);
		f.deviceHost.setField(id, pt::at(0, Field::PlaceX), s.x);
		f.deviceHost.setField(id, pt::at(0, Field::PlaceY), s.y);
		f.deviceHost.setField(id, pt::at(0, Field::Rotate), s.rotate);
		f.deviceHost.setField(id, pt::at(0, Field::Zone), s.zone);
		(void)window->updateContent();
		terrain.setZoom(s.zoom);
		(void)terrain.poll();
		const auto centre = terrain.viewCentreForTest();
		const double half = ui::PatternTerrain::kHalfSpan / s.zoom;
		const double inset = ui::edge_pan::kStarInsetPixels;
		// EVERY EDGE: the place under the (inset-clamped) hand, wherever that is on the plane.
		for (const auto& [px, py] : { std::pair { w, h * 0.5f }, std::pair { 0.0f, h * 0.5f }, std::pair { w * 0.5f, 0.0f },
		                              std::pair { w * 0.5f, h } }) {
			terrain.onMouseDown().callback(at(w * 0.5f, h * 0.5f));
			terrain.onMouseDrag().callback(at(px, py));
			terrain.onMouseUp().callback(at(px, py));
			const double ux = std::clamp<double>(px, inset, w - inset) / w;
			const double uy = 1.0 - std::clamp<double>(py, inset, h - inset) / h;
			CHECK(place()[0] == doctest::Approx(centre[0] + (2.0 * ux - 1.0) * half).epsilon(1e-5));
			CHECK(place()[1] == doctest::Approx(centre[1] + (2.0 * uy - 1.0) * half).epsilon(1e-5));
		}
	}
	// AND AN EDGE PAN WALKS ON: two seconds held at the right edge from x = 3.9 goes well past it.
	f.deviceHost.setField(id, pt::at(0, Field::PlaceX), 3.9f);
	(void)window->updateContent();
	terrain.setZoom(1.0);
	(void)terrain.poll();
	terrain.onMouseDown().callback(at(w * 0.5f, h * 0.5f));
	terrain.onMouseDrag().callback(at(w - 1.0f, h * 0.5f));
	for (int i = 0; i < 40; ++i) {
		(void)terrain.edgeScroll(0.05);
	}
	terrain.onMouseUp().callback(at(w - 1.0f, h * 0.5f));
	CHECK(place()[0] > 5.0);
	REQUIRE(f.bridge.removeRackDevice(slot));
}

namespace {
/// The first frame under `root` that offers `desc`'s control `param` (`ui::ControlFace`), shown or not.
const visage::Frame* frameOffering(const visage::Frame* root, const feathers::device::Descriptor& desc, size_t param) {
	std::vector<ui::ControlFace::Offered> list;
	std::vector<const visage::Frame*> stack { root };
	while (!stack.empty()) {
		const visage::Frame* fr = stack.back();
		stack.pop_back();
		if (const auto* face = dynamic_cast<const ui::ControlFace*>(fr); face != nullptr && fr != root) {
			list.clear();
			face->offeredControls(list);
			for (const ui::ControlFace::Offered& o : list) {
				if (o.control != nullptr && &o.control->desc() == &desc.params[param]) {
					return fr;
				}
			}
		}
		for (const visage::Frame* c : fr->children()) {
			stack.push_back(c);
		}
	}
	return nullptr;
}
} // namespace

TEST_CASE("the Pattern window is composed: the terrain's own controls lie in it, switches are chips, the rest a slider bank, Humanize SKLITTER's zone cell") {
	// Rotate and the other terrain controls sit close to the terrain, even in it as in the other effects; no single
	// toggle holds a whole cell, and knobs that can be are compact slider banks; Swing/Humanize is a zone knob. By value,
	// at every focus.
	namespace pt = feathers::mod::arp::pat;
	using Field = pt::Field;
	ModFixture f;
	const size_t slot = f.bridge.addRackDevice(pt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxRackSlots);
	f.bridge.refresh();
	const DeviceInstanceId id = f.bridge.rackDeviceAt(slot);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, id, pt::kDescriptor);
	auto* window = dynamic_cast<ui::PatternExpanded*>(view.get());
	REQUIRE(window != nullptr);
	const visage::Point min = window->minimumSize();
	window->setBounds(0.0f, 0.0f, min.x, min.y);
	f.deviceHost.setField(id, pt::at(pt::Global::HumanizeMode), 3.0f); // Swing 16: an amount to show
	(void)window->updateContent();
	for (size_t focus = 0; focus < pt::kPatterns; ++focus) {
		CAPTURE(focus);
		window->showPage(focus);
		const visage::Bounds map = window->terrain().bounds();
		for (const Field fld : { Field::Rotate, Field::Zone, Field::ZoneOffset, Field::Gamma }) {
			CAPTURE(static_cast<int>(fld));
			const visage::Frame* w = frameOffering(window, pt::kDescriptor, pt::at(focus, fld));
			REQUIRE(w != nullptr);
			CHECK(w->isVisible());
			const visage::Bounds b = w->bounds();
			CHECK(b.x() >= map.x());
			CHECK(b.right() <= map.right());
			CHECK(b.y() >= map.y());
			CHECK(b.bottom() <= map.bottom());
		}
		CHECK(dynamic_cast<const ui::GammaBox*>(frameOffering(window, pt::kDescriptor, pt::at(focus, Field::Rotate))) != nullptr);
		CHECK(dynamic_cast<const ui::GammaBox*>(frameOffering(window, pt::kDescriptor, pt::at(focus, Field::Gamma))) != nullptr);
	}
	// SWITCHES ARE CHIPS; CONTINUOUS CONTROLS A SLIDER BANK (the selector excepted: it reads better as a knob).
	window->showPage(0);
	for (size_t i = 0; i < pt::kParamCount; ++i) {
		const auto& d = pt::kParamDescs[i];
		const bool global = i < pt::kGlobalCount;
		if (!global && (i - pt::kGlobalCount) / pt::kFieldCount != 0) {
			continue;
		}
		const visage::Frame* w = frameOffering(window, pt::kDescriptor, i);
		if (w == nullptr) {
			continue; // the enable (the header's) and Place X/Y (the puck)
		}
		CAPTURE(d.key);
		const bool offOn = d.unit == feathers::device::Unit::Choice && d.names.size() == 2
		                   && std::string(d.names[0]) == "Off" && std::string(d.names[1]) == "On";
		if (offOn) {
			CHECK(dynamic_cast<const ui::SwitchChip*>(w) != nullptr);
		}
		CHECK((dynamic_cast<const ui::Knob*>(w) == nullptr || i == pt::at(pt::Global::Selector)));
	}
	for (const Field fld : { Field::Rate, Field::Length, Field::Gate, Field::Accent }) {
		CHECK(dynamic_cast<const ui::FaderPill*>(frameOffering(window, pt::kDescriptor, pt::at(0, fld))) != nullptr);
	}
	CHECK(dynamic_cast<const ui::FaderPill*>(frameOffering(window, pt::kDescriptor, pt::at(pt::Global::Velocity))) != nullptr);
	// HUMANIZE: SKLITTER's cell — its ZoneSelect is the mode, its box the Amount, shown only where the feel has one.
	const auto* cell = dynamic_cast<const ui::ModeValueCell*>(frameOffering(window, pt::kDescriptor, pt::at(pt::Global::HumanizeMode)));
	REQUIRE(cell != nullptr);
	CHECK(cell->activeParam() == static_cast<long>(pt::at(pt::Global::HumanizeAmount)));
	f.deviceHost.setField(id, pt::at(pt::Global::HumanizeMode), 0.0f); // Straight: no amount
	(void)window->updateContent();
	CHECK(cell->activeParam() == -1);
	REQUIRE(f.bridge.removeRackDevice(slot));
}
TEST_CASE("the classic Arp's Feel is SKLITTER's zone cell on the card and in the window: the feel on a ZoneSelect, its Amount the box, only where the feel has one") {
	namespace ap = feathers::mod::arp;
	ModFixture f;
	const size_t slot = f.bridge.addRackDevice(ap::kTraits.tag);
	REQUIRE(slot < kMaxRackSlots);
	f.bridge.refresh();
	f.panel->resized();
	auto* card = const_cast<ui::ArpCard*>(dynamic_cast<const ui::ArpCard*>(f.panel->cardFrameForTest(slot)));
	REQUIRE(card != nullptr);
	const DeviceInstanceId id = card->device();
	ui::ModeValueCell& feel = card->feel();
	CHECK(&feel.select().control().desc() == &ap::kDescriptor.params[ap::at(ap::Param::Feel)]);
	f.deviceHost.setField(id, ap::at(ap::Param::Feel), 0.0f); // Straight
	(void)card->updateContent();
	CHECK(feel.activeParam() == -1);
	f.deviceHost.setField(id, ap::at(ap::Param::Feel), 3.0f); // Swing 16
	(void)card->updateContent();
	CHECK(feel.activeParam() == static_cast<long>(ap::at(ap::Param::FeelAmount)));
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, id, ap::kDescriptor);
	REQUIRE(view != nullptr);
	view->setBounds(0.0f, 0.0f, 900.0f, 700.0f);
	(void)view->update();
	CHECK(dynamic_cast<const ui::ModeValueCell*>(frameOffering(view.get(), ap::kDescriptor, ap::at(ap::Param::Feel))) != nullptr);
	REQUIRE(f.bridge.removeRackDevice(slot));
}
TEST_CASE("the Pattern in Drum: card and window hide the pitch boxes, the window shows the lane bank, and the lane is the drum grid") {
	namespace pt = feathers::mod::arp::pat;
	namespace gn = feathers::mod::arp::genes;
	using Field = pt::Field;
	ModFixture f;
	const size_t slot = f.bridge.addRackDevice(pt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxRackSlots);
	f.bridge.refresh();
	f.panel->resized();
	auto* card = const_cast<ui::PatternCard*>(dynamic_cast<const ui::PatternCard*>(f.panel->cardFrameForTest(slot)));
	REQUIRE(card != nullptr);
	const DeviceInstanceId id = card->device();
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, id, pt::kDescriptor);
	auto* window = dynamic_cast<ui::PatternExpanded*>(view.get());
	REQUIRE(window != nullptr);
	const visage::Point min = window->minimumSize();
	window->setBounds(0.0f, 0.0f, min.x, min.y + 200.0f);
	const auto shownOn = [&](const visage::Frame* root, size_t param) {
		const visage::Frame* w = frameOffering(root, pt::kDescriptor, param);
		for (const visage::Frame* p = w; p != nullptr && p != root; p = p->parent()) {
			if (!p->isVisible()) {
				return false;
			}
		}
		return w != nullptr;
	};
	CHECK(shownOn(card, pt::at(0, Field::Scale)));
	CHECK(shownOn(window, pt::at(0, Field::Scale)));
	f.deviceHost.setField(id, pt::at(0, Field::Mode), static_cast<float>(gn::Mode::Drum));
	(void)card->updateContent();
	(void)window->updateContent();
	for (const Field fld : { Field::Scale, Field::Degree, Field::Root }) {
		CAPTURE(static_cast<int>(fld));
		CHECK_FALSE(shownOn(card, pt::at(0, fld)));
		CHECK_FALSE(shownOn(window, pt::at(0, fld)));
	}
	CHECK(window->lane().drumForTest());
	// THE LANE BANK: eight chips and eight note boxes, over the lane map's block.
	size_t chips = 0;
	std::vector<const visage::Frame*> stack { window };
	while (!stack.empty()) {
		const visage::Frame* fr = stack.back();
		stack.pop_back();
		if (const auto* chip = dynamic_cast<const ui::SwitchChip*>(fr); chip != nullptr && chip->isVisible()
		    && std::string(chip->control().desc().key).find(".drum.") != std::string::npos) {
			++chips;
		}
		for (const visage::Frame* c : fr->children()) {
			stack.push_back(c);
		}
	}
	CHECK(chips == feathers::mod::arp::pattern::kDrumLanes);
	f.deviceHost.setField(id, pt::at(0, Field::Mode), static_cast<float>(gn::Mode::Melody));
	(void)window->updateContent();
	CHECK_FALSE(window->lane().drumForTest());
	REQUIRE(f.bridge.removeRackDevice(slot));
}

namespace {
namespace lane_check {
/// EVERY STEP BUT `k` STRUCK THE SAME, before and after.
void othersSame(const std::map<int64_t, std::vector<Strike>>& a, const std::map<int64_t, std::vector<Strike>>& b, int64_t k) {
	for (int64_t s = 0; s < 16; ++s) {
		if (s == k || s == k + 1) {
			continue; // the step, and the one after it (a slide or a long gate moves the note before its off)
		}
		CAPTURE(s);
		const auto ia = a.find(s);
		const auto ib = b.find(s);
		REQUIRE((ia == a.end()) == (ib == b.end()));
		if (ia == a.end()) {
			continue;
		}
		REQUIRE(ia->second.size() == ib->second.size());
		for (size_t i = 0; i < ia->second.size(); ++i) {
			CHECK(ia->second[i].key == ib->second[i].key);
			CHECK(ia->second[i].velocity == ib->second[i].velocity);
		}
	}
}
} // namespace lane_check
} // namespace

TEST_CASE("the Pattern window's lane: a click SELECTS and edits nothing; a double-click adds a note and deletes it again; no modifier click edits; each is one undo step") {
	// No more modifier-click note editing: a single click selects, a double click adds or deletes.
	using namespace lane_check;
	using Kind = pn::PatternStep::Kind;
	Window w;
	ui::PatternLane& lane = w.lane();
	REQUIRE(lane.cycleForTest().length == 16);
	const pn::PatternCycle terrain = lane.terrainForTest();
	size_t on = 99;
	size_t off = 99;
	for (size_t k = 0; k < 16; ++k) {
		if (terrain.steps[k].kind == Kind::Note && on == 99) {
			on = k;
		}
		if (terrain.steps[k].kind == Kind::Rest && off == 99) {
			off = k;
		}
	}
	REQUIRE(on < 16);
	REQUIRE(off < 16);
	const std::vector<float> clean = w.edits();
	const size_t notes = w.notes();
	const auto press = [&](visage::MouseEvent e) {
		e.repeat_click_count = std::max(e.repeat_click_count, 1); // A SINGLE CLICK COUNTS 1 (visage's Window::handleMouseDown)
		lane.onMouseDown().callback(e);
		lane.onMouseUp().callback(e);
	};
	const float noteY = lane.yOfKey(terrain.keyAt(on, 0)) + 1.0f;
	// A CLICK ON A NOTE: selected, and nothing written.
	press(at(lane.xOfStep(on) + 2.0f, noteY));
	CHECK(lane.selectedStep() == static_cast<int>(on));
	CHECK(w.edits() == clean);
	CHECK(w.notes() == notes);
	CHECK(w.window->inspector().labelForTest() == "Step " + std::to_string(on + 1));
	// A CLICK ON A REST: selected, and nothing written.
	press(at(lane.xOfStep(off) + 2.0f, lane.height() * 0.5f));
	CHECK(lane.selectedStep() == static_cast<int>(off));
	CHECK(w.edits() == clean);
	// NO MODIFIER CLICK EDITS ANY MORE — Shift (was accent), Alt (was slide), Ctrl/Cmd (was tie): each only selects.
	for (const visage::MouseEvent& e : { fineAt(lane.xOfStep(on) + 2.0f, noteY), optAt(lane.xOfStep(on) + 2.0f, noteY) }) {
		press(e);
		CHECK(lane.selectedStep() == static_cast<int>(on));
		CHECK(w.edits() == clean);
	}
	{
		visage::MouseEvent e = at(lane.xOfStep(on) + 2.0f, noteY);
		e.modifiers = visage::kModifierCmd | visage::kModifierRegCtrl;
		press(e);
		CHECK(w.edits() == clean);
	}
	CHECK(w.history.length() == 0);
	// A DOUBLE-CLICK ON THE REST ADDS A NOTE, at the key under the hand, in the scale — one undo step.
	const float y = lane.height() * 0.5f;
	press(doubleAt(lane.xOfStep(off) + 2.0f, y));
	CHECK(lane.cycleForTest().steps[off].kind == Kind::Note);
	CHECK(lane.cycleForTest().steps[off].keys[0] == lane.keyAt(y));
	CHECK(w.notes() == notes + 1);
	CHECK(lane.selectedStep() == static_cast<int>(off));
	CHECK(w.history.length() == 1);
	// AND A DOUBLE-CLICK ON IT DELETES IT.
	press(doubleAt(lane.xOfStep(off) + 2.0f, lane.yOfKey(lane.cycleForTest().keyAt(off, 0)) + 1.0f));
	CHECK(lane.cycleForTest().steps[off].kind == Kind::Rest);
	CHECK(w.notes() == notes);
	CHECK(w.history.length() == 2);
	// UNDO AND REDO, a gesture each.
	w.history.undo();
	(void)lane.poll();
	CHECK(lane.cycleForTest().steps[off].kind == Kind::Note);
	w.history.undo();
	(void)lane.poll();
	CHECK(lane.cycleForTest().steps[off].kind == Kind::Rest);
	CHECK(w.notes() == notes);
	w.history.redo();
	(void)lane.poll();
	CHECK(lane.cycleForTest().steps[off].kind == Kind::Note);
	// THE SELECTION IS UI STATE: the edits block holds no trace of it, and a focus move drops it.
	w.window->setFocus(1);
	CHECK(lane.selectedStep() == -1);
	CHECK(w.window->inspector().labelForTest().empty());
	// THE TOOLTIP NAMES THE NEW GESTURES.
	w.window->setFocus(0);
	CHECK(std::string(lane.tooltipAt({ lane.width() * 0.5f, lane.height() * 0.5f })) == ui::PatternLane::kPitchedGestures);
	CHECK(std::string(ui::PatternLane::kPitchedGestures).find("Shift") == std::string::npos);
	CHECK(std::string(ui::PatternLane::kPitchedGestures).find("double-click") != std::string::npos);
}
TEST_CASE("the Pattern window's lane: a click on a note's BODY selects that note — no note is added over it ('selecting a note body currently overlays a note at the same pitch')") {
	using namespace lane_check;
	using Kind = pn::PatternStep::Kind;
	Window w;
	ui::PatternLane& lane = w.lane();
	// A NOTE THAT RUNS ON THROUGH THE NEXT STEPS: step k's gate made 3 steps long (an edit), the next two rests.
	const pn::PatternCycle terrain = lane.terrainForTest();
	size_t k = 99;
	for (size_t s = 0; s + 3 < 16; ++s) {
		if (terrain.steps[s].kind == Kind::Note) {
			k = s;
			break;
		}
	}
	REQUIRE(k < 16);
	lane.select(static_cast<int>(k));
	for (size_t s = k + 1; s <= k + 2; ++s) {
		lane.select(static_cast<int>(s));
		lane.editSelected([](pn::StepEdit& ed) { ed.kind = Kind::Rest; });
	}
	lane.select(static_cast<int>(k));
	lane.editSelected([](pn::StepEdit& ed) { ed.gate = 2.9f; });
	REQUIRE(lane.cycleForTest().steps[k].gate == doctest::Approx(2.9f));
	REQUIRE(lane.cycleForTest().steps[k + 2].kind == Kind::Rest);
	const size_t notes = w.notes();
	const std::vector<float> before = w.edits();
	// THE BODY: two steps on, on the note's own row — over a step that is itself a rest.
	const int key = lane.cycleForTest().keyAt(k, 0);
	const float bx = lane.xOfStep(k + 2) + 2.0f;
	const float by = lane.yOfKey(key) + 1.0f;
	REQUIRE(lane.stepAt(bx) == k + 2);
	CHECK(lane.noteAt({ bx, by }) == static_cast<int>(k));
	lane.select(-1);
	lane.onMouseDown().callback(once(at(bx, by)));
	lane.onMouseUp().callback(once(at(bx, by)));
	CHECK(lane.selectedStep() == static_cast<int>(k)); // THE NOTE, not the step under the hand
	CHECK(w.notes() == notes);                         // one note before, one after
	CHECK(lane.cycleForTest().steps[k + 2].kind == Kind::Rest);
	CHECK(w.edits() == before);
	// AND A DOUBLE-CLICK THERE DELETES THAT NOTE, never adds one beside it.
	lane.onMouseDown().callback(doubleAt(bx, by));
	lane.onMouseUp().callback(doubleAt(bx, by));
	CHECK(lane.cycleForTest().steps[k].kind == Kind::Rest);
	CHECK(lane.cycleForTest().steps[k + 2].kind == Kind::Rest);
	CHECK(w.notes() == notes - 1);
}
TEST_CASE("the Pattern window's step inspector: each setting changes exactly that step's played note — Note, Pitch, Velocity, Gate, Accent, Slide, Tie — through a fresh device fed the patch's row and edits; one undo step each") {
	using namespace lane_check;
	using Kind = pn::PatternStep::Kind;
	using What = ui::StepControl::What;
	Window w;
	ui::PatternLane& lane = w.lane();
	ui::StepInspector& inspector = w.window->inspector();
	pn::PatternCycle cycle;
	const auto base = play(w.f.deviceHost, w.id, &cycle);
	size_t k = 99;
	size_t rest = 99;
	for (size_t s = 1; s + 2 < 16; ++s) {
		if (cycle.steps[s].kind == Kind::Note && cycle.steps[s].count == 1 && k == 99 && base.count(static_cast<int64_t>(s)) != 0) {
			k = s;
		}
		if (cycle.steps[s].kind == Kind::Rest && rest == 99) {
			rest = s;
		}
	}
	REQUIRE(k < 16);
	REQUIRE(rest < 16);
	REQUIRE(base.at(static_cast<int64_t>(k)).size() == 1);
	const Strike was = base.at(static_cast<int64_t>(k))[0];
	// NOTHING SELECTED: the inspector shows only its hint.
	lane.select(-1);
	for (size_t i = 0; i < static_cast<size_t>(What::kCount); ++i) {
		CHECK_FALSE(inspector.widgetForTest(static_cast<What>(i))->isVisible());
	}
	const auto set = [&](size_t step, What what, double value) {
		lane.select(static_cast<int>(step));
		ui::StepControl& c = inspector.controlForTest(what);
		c.beginGesture();
		c.setValue(value);
		c.endGesture();
	};
	const auto undoAll = [&] {
		while (w.history.canUndo()) {
			w.history.undo();
		}
		(void)lane.poll();
	};
	// A NOTE STEP SELECTED: Note, Tie, Pitch, Velocity, Gate, Accent, Slide are shown; the drum switches are not.
	lane.select(static_cast<int>(k));
	for (const What shown : { What::Note, What::Tie, What::Pitch, What::Velocity, What::Gate, What::Accent, What::Slide }) {
		CHECK(inspector.widgetForTest(shown)->isVisible());
	}
	CHECK_FALSE(inspector.widgetForTest(What::Hit)->isVisible());
	SUBCASE("Pitch: the step's key, snapped in the scale") {
		const int want = lane.snapKey(was.key + 2, was.key);
		REQUIRE(want != was.key);
		set(k, What::Pitch, want);
		CHECK(w.history.length() == 1);
		const auto now = play(w.f.deviceHost, w.id);
		REQUIRE(now.at(static_cast<int64_t>(k)).size() == 1);
		CHECK(now.at(static_cast<int64_t>(k))[0].key == want);
		othersSame(base, now, static_cast<int64_t>(k));
	}
	SUBCASE("Velocity") {
		set(k, What::Velocity, 0.3);
		const auto now = play(w.f.deviceHost, w.id);
		CHECK(now.at(static_cast<int64_t>(k))[0].velocity == doctest::Approx(0.3));
		CHECK(now.at(static_cast<int64_t>(k))[0].key == was.key);
		othersSame(base, now, static_cast<int64_t>(k));
	}
	SUBCASE("Gate: the note lasts that many steps") {
		set(k, What::Gate, 0.75);
		const auto now = play(w.f.deviceHost, w.id);
		CHECK(now.at(static_cast<int64_t>(k))[0].length == 4500);
		othersSame(base, now, static_cast<int64_t>(k));
	}
	SUBCASE("Accent: the device's step is accented") {
		const bool before = cycle.steps[k].accent;
		set(k, What::Accent, before ? 0.0 : 1.0);
		pn::PatternCycle now;
		(void)play(w.f.deviceHost, w.id, &now);
		CHECK(now.steps[k].accent != before);
		for (size_t s = 0; s < 16; ++s) {
			if (s != k) {
				CHECK(now.steps[s].accent == cycle.steps[s].accent);
			}
		}
	}
	SUBCASE("Slide: the note's off moves 2 ms past its gate (`kSlideOverlapSeconds`), so a Legato synth overlaps it") {
		set(k, What::Slide, 0.0);
		set(k, What::Gate, 0.5);
		const auto plain = play(w.f.deviceHost, w.id);
		REQUIRE(plain.at(static_cast<int64_t>(k))[0].length == 3000);
		set(k, What::Slide, 1.0);
		pn::PatternCycle now;
		const auto heard = play(w.f.deviceHost, w.id, &now);
		CHECK(now.steps[k].slide);
		CHECK(heard.at(static_cast<int64_t>(k))[0].length == 3000 + 96);
	}
	SUBCASE("Note off, then Tie: the step strikes nothing; Note on a rest strikes there") {
		set(k, What::Note, 0.0);
		auto now = play(w.f.deviceHost, w.id);
		CHECK(now.count(static_cast<int64_t>(k)) == 0);
		othersSame(base, now, static_cast<int64_t>(k));
		set(k, What::Tie, 1.0);
		pn::PatternCycle tied;
		now = play(w.f.deviceHost, w.id, &tied);
		CHECK(tied.steps[k].kind == Kind::Tie);
		CHECK(now.count(static_cast<int64_t>(k)) == 0);
		undoAll();
		set(rest, What::Note, 1.0);
		now = play(w.f.deviceHost, w.id);
		REQUIRE(now.count(static_cast<int64_t>(rest)) == 1);
		CHECK(now.at(static_cast<int64_t>(rest))[0].key == lane.defaultKeyAt(rest));
		othersSame(base, now, static_cast<int64_t>(rest));
	}
	// EACH GESTURE WAS ONE UNDO STEP, and undoing them all gives the terrain's own again.
	undoAll();
	const auto back = play(w.f.deviceHost, w.id);
	othersSame(base, back, -10);
}
TEST_CASE("Pattern's Degree and Thru hover text follow the tab's Mode: the chord degree in Chords, the line's transposition in the line modes, Thru's Auto by Drum") {
	namespace pt = feathers::mod::arp::pat;
	namespace gn = feathers::mod::arp::genes;
	using Field = pt::Field;
	ModFixture f;
	const size_t slot = f.bridge.addRackDevice(pt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxRackSlots);
	f.bridge.refresh();
	f.panel->resized();
	auto* card = const_cast<ui::PatternCard*>(dynamic_cast<const ui::PatternCard*>(f.panel->cardFrameForTest(slot)));
	REQUIRE(card != nullptr);
	const DeviceInstanceId id = card->device();
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, id, pt::kDescriptor);
	auto* window = dynamic_cast<ui::PatternExpanded*>(view.get());
	REQUIRE(window != nullptr);
	const auto tipOf = [&](Field fld) { return std::string(ui::tooltipOf(&window->controlForTest(pt::at(0, fld)))); };
	const auto setMode = [&](gn::Mode m) {
		f.deviceHost.setField(id, pt::at(0, Field::Mode), static_cast<float>(m));
		(void)window->updateContent();
	};
	setMode(gn::Mode::Chords);
	CHECK(tipOf(Field::Degree) == pt::kDegreeTipChords);
	setMode(gn::Mode::TbGlide);
	CHECK(tipOf(Field::Degree) == pt::kDegreeTipLine);
	// NOT VACUOUS: the two modes name different things.
	CHECK(pt::kDegreeTipChords != std::string(pt::kDegreeTipLine));
	setMode(gn::Mode::Drum);
	// THRU'S AUTO RESOLVES BY MODE: on in Drum, off in a note mode.
	CHECK(tipOf(Field::Thru) == pt::kThruTipAutoDrum);
	setMode(gn::Mode::Melody);
	CHECK(tipOf(Field::Thru) == pt::kThruTipAutoOther);
	// AN EXPLICIT Thru says what it does, and keeps the declared text (no Auto to resolve).
	f.deviceHost.setField(id, pt::at(0, Field::Thru), static_cast<float>(pt::Thru::On));
	(void)window->updateContent();
	CHECK(tipOf(Field::Thru) != pt::kThruTipAutoOther);
}
TEST_CASE("the Pattern window's drum grid: a click selects a cell and edits nothing; a double-click toggles that lane's hit; the inspector's Hit and Accent set that cell") {
	using namespace lane_check;
	using What = ui::StepControl::What;
	namespace gn = feathers::mod::arp::genes;
	Window w(200.0f);
	w.f.deviceHost.setField(w.id, pt::at(0, pt::Field::Mode), static_cast<float>(gn::Mode::Drum));
	(void)w.window->updateContent();
	ui::PatternLane& lane = w.lane();
	REQUIRE(lane.drumForTest());
	const auto ride = static_cast<size_t>(pn::DrumLane::Ride);
	const auto bit = static_cast<uint8_t>(1u << ride);
	const uint8_t was = lane.cycleForTest().steps[5].drumHits;
	const float x = lane.xOfStep(5) + 2.0f;
	const float y = lane.yOfLane(ride) + 2.0f;
	REQUIRE(lane.laneAt(y) == ride);
	const std::vector<float> clean = w.edits();
	lane.onMouseDown().callback(once(at(x, y)));
	lane.onMouseUp().callback(once(at(x, y)));
	CHECK(lane.selectedStep() == 5);
	CHECK(lane.selectedLane() == ride);
	CHECK(w.edits() == clean);
	CHECK(lane.cycleForTest().steps[5].drumHits == was);
	CHECK(w.window->inspector().labelForTest() == "Step 6 · Ride");
	CHECK(w.window->inspector().widgetForTest(What::Hit)->isVisible());
	CHECK_FALSE(w.window->inspector().widgetForTest(What::Pitch)->isVisible());
	// A SHIFT-CLICK NO LONGER ACCENTS.
	lane.onMouseDown().callback(once(fineAt(x, y)));
	lane.onMouseUp().callback(once(fineAt(x, y)));
	CHECK(w.edits() == clean);
	// A DOUBLE-CLICK TOGGLES THE HIT.
	lane.onMouseDown().callback(doubleAt(x, y));
	lane.onMouseUp().callback(doubleAt(x, y));
	CHECK(lane.cycleForTest().steps[5].drumHits == static_cast<uint8_t>(was ^ bit));
	CHECK(lane.cycleForTest().steps[5].edited);
	// THE INSPECTOR: Hit on, then Accent on — that cell alone, and the device plays the lane's note there.
	ui::StepControl& hit = w.window->inspector().controlForTest(What::Hit);
	hit.beginGesture();
	hit.setValue(1.0);
	hit.endGesture();
	ui::StepControl& accent = w.window->inspector().controlForTest(What::HitAccent);
	accent.beginGesture();
	accent.setValue(1.0);
	accent.endGesture();
	pn::PatternCycle cycle;
	const auto heard = play(w.f.deviceHost, w.id, &cycle);
	CHECK((cycle.steps[5].drumHits & bit) != 0);
	CHECK((cycle.steps[5].drumAccent & bit) != 0);
	const int rideNote = static_cast<int>(std::lround(w.f.deviceHost.fieldAt(w.id, pt::drumNoteAt(0, ride))));
	REQUIRE(heard.count(5) == 1);
	bool struck = false;
	for (const Strike& s : heard.at(5)) {
		struck = struck || s.key == rideNote;
	}
	CHECK(struck);
	CHECK(std::string(lane.tooltipAt({ lane.width() * 0.5f, lane.height() * 0.5f })) == ui::PatternLane::kDrumGestures);
}
TEST_CASE("Pattern's Solo is a radio: a press solos that tab alone and focuses it; Ctrl/Cmd adds or removes; pressing the only solo clears it; one undo step; a DAW write of one Solo field still works") {
	// Solo did not release the previously soloed tab; and soloing a tab focuses it.
	namespace pt = feathers::mod::arp::pat;
	ModFixture f;
	const size_t slot = f.bridge.addRackDevice(pt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxRackSlots);
	f.bridge.refresh();
	f.panel->resized();
	auto* card = const_cast<ui::PatternCard*>(dynamic_cast<const ui::PatternCard*>(f.panel->cardFrameForTest(slot)));
	REQUIRE(card != nullptr);
	const DeviceInstanceId id = card->device();
	ui::EditHistory history;
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, id, pt::kDescriptor);
	auto* window = dynamic_cast<ui::PatternExpanded*>(view.get());
	REQUIRE(window != nullptr);
	window->setModulation(nullptr, &history);
	card->setModulation(nullptr, &history);
	window->setBounds(0.0f, 0.0f, 900.0f, 1400.0f);
	const auto solos = [&] {
		std::array<bool, pt::kPatterns> s {};
		for (size_t p = 0; p < pt::kPatterns; ++p) {
			s[p] = f.deviceHost.fieldAt(id, pt::at(p, pt::Field::Solo)) >= 0.5f;
		}
		return s;
	};
	using S = std::array<bool, pt::kPatterns>;
	const auto chipOf = [&](visage::Frame* root, size_t p) -> ui::SwitchChip* {
		std::vector<visage::Frame*> stack { root };
		while (!stack.empty()) {
			visage::Frame* fr = stack.back();
			stack.pop_back();
			for (visage::Frame* ch : fr->children()) {
				stack.push_back(ch);
			}
			auto* c = dynamic_cast<ui::SwitchChip*>(fr);
			if (c == nullptr) {
				continue;
			}
			if (const auto* road = dynamic_cast<const ui::DeviceControl*>(&c->control());
			    road != nullptr && road->control() == pt::at(p, pt::Field::Solo)) {
				return c;
			}
		}
		return nullptr;
	};
	const auto pressChip = [](ui::SwitchChip* c, bool cmd) {
		REQUIRE(c != nullptr);
		visage::MouseEvent e = at(2.0f, 2.0f);
		if (cmd) {
			e.modifiers = visage::kModifierCmd | visage::kModifierRegCtrl;
		}
		c->onMouseDown().callback(e);
		c->onMouseUp().callback(e);
	};
	// ON THE CARD: S of tab 2 — tab 2 alone, and the card focuses it.
	pressChip(chipOf(card, 1), false);
	CHECK(solos() == S { false, true, false, false });
	CHECK(card->focus() == 1);
	// S of tab 3: the radio moves — exactly one soloed.
	pressChip(chipOf(card, 2), false);
	CHECK(solos() == S { false, false, true, false });
	CHECK(card->focus() == 2);
	// IN THE WINDOW: Ctrl/Cmd-S of tab 1 adds it, and focuses it (the window was on tab 3, so the focus MOVES).
	window->setFocus(2);
	REQUIRE(window->focus() == 2);
	pressChip(chipOf(window, 0), true);
	CHECK(solos() == S { true, false, true, false });
	CHECK(window->focus() == 0);
	// Ctrl/Cmd-S again takes it out — un-soloing does not move the focus.
	window->setFocus(3);
	pressChip(chipOf(window, 0), true);
	CHECK(solos() == S { false, false, true, false });
	CHECK(window->focus() == 3);
	// S ON THE ONLY SOLO CLEARS IT.
	pressChip(chipOf(window, 2), false);
	CHECK(solos() == S { false, false, false, false });
	// A PLAIN S WHILE SEVERAL ARE SOLOED: that one alone.
	pressChip(chipOf(card, 0), true);
	pressChip(chipOf(card, 1), true);
	pressChip(chipOf(card, 3), false);
	CHECK(solos() == S { false, false, false, true });
	// ONE UNDO STEP A PRESS: the radio's move undone in one, every field it wrote.
	history.undo();
	CHECK(solos() == S { true, true, false, false });
	history.redo();
	CHECK(solos() == S { false, false, false, true });
	// A DAW WRITE OF ONE SOLO FIELD is that field alone, as before (several soloed is still a state the device plays).
	f.deviceHost.setField(id, pt::at(0, pt::Field::Solo), 1.0f);
	CHECK(solos() == S { true, false, false, true });
	REQUIRE(f.bridge.removeRackDevice(slot));
}
TEST_CASE("SHOT Pattern's Cycle cells: the window on Cycle and on Selector, and the card in the chain (FEATHERS_SHOT_DIR)") {
	// NOT AN ASSERTION — A CAMERA for design §7.6: the whole editor with the Pattern window open, on Cycle (every tab's
	// Repeats and Next on its tab) and on Selector (none, the selector back), the window's and the tab strip's rectangles
	// printed so a close-up can be cut. Run under `scripts/venue-xvfb.sh` with FEATHERS_REDUCED_MOTION=1.
	const char* dir = std::getenv("FEATHERS_SHOT_DIR");
	if (dir == nullptr) {
		return;
	}
	namespace pt = feathers::mod::arp::pat;
	using Field = pt::Field;
	ModFixture f;
	auto* root = dynamic_cast<visage::ApplicationEditor*>(f.root);
	REQUIRE(root != nullptr);
	root->setWindowless(static_cast<int>(f.root->width()), static_cast<int>(f.root->height()));
	const size_t slot = f.bridge.addRackDevice(pt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxRackSlots);
	f.bridge.refresh();
	f.panel->resized();
	auto* card = const_cast<ui::PatternCard*>(dynamic_cast<const ui::PatternCard*>(f.panel->cardFrameForTest(slot)));
	REQUIRE(card != nullptr);
	const DeviceInstanceId id = card->device();
	// FOUR TABS, FOUR CELLS: 1 Next ×2, 2 Random ×1, 3 Loop ×4, 4 Stop ×3 (switched off).
	const std::array<std::pair<int, pt::NextAction>, pt::kPatterns> cells { { { 2, pt::NextAction::Next },
		                                                                       { 1, pt::NextAction::Random },
		                                                                       { 4, pt::NextAction::Loop },
		                                                                       { 3, pt::NextAction::Stop } } };
	for (size_t p = 0; p < pt::kPatterns; ++p) {
		f.deviceHost.setField(id, pt::at(p, Field::Enable), p < 3 ? 1.0f : 0.0f);
		f.deviceHost.setField(id, pt::at(p, Field::Repeats), static_cast<float>(cells[p].first));
		f.deviceHost.setField(id, pt::at(p, Field::Next), static_cast<float>(cells[p].second));
	}
	REQUIRE(f.panel->onExpandDevice != nullptr);
	f.panel->onExpandDevice(id);
	auto* window = findInTree<ui::PatternExpanded>(f.root);
	REQUIRE(window != nullptr);
	const auto shoot = [&](const char* name) {
		for (int i = 0; i < 8; ++i) {
			(void)window->updateContent();
			(void)card->updateContent();
			root->redrawAll();
			root->drawWindow();
		}
		const visage::Screenshot& shot = root->takeScreenshot();
		if (shot.width() == 0) {
			MESSAGE("SKIP no screenshot");
			return;
		}
		const std::string path = std::string(dir) + "/" + name + ".png";
		shot.save(path);
		const visage::Point w = window->positionInWindow();
		const ui::PatternTabs& tabs = window->tabsForTest();
		const visage::Point t = tabs.positionInWindow();
		const visage::Point c = card->positionInWindow();
		MESSAGE(name << " scale " << root->dpiScale() << " shot " << shot.width() << "x" << shot.height() << " window "
		             << w.x << "," << w.y << " " << window->width() << "x" << window->height() << " strip " << t.x << ","
		             << t.y << " " << tabs.width() << "x" << tabs.height() << " card " << c.x << "," << c.y << " "
		             << card->width() << "x" << card->height() << " -> " << path);
	};
	f.deviceHost.setField(id, pt::at(pt::Global::PlayMode), 1.0f);
	window->showPage(0);
	card->showPage(1);
	shoot("cycle_window");
	f.deviceHost.setField(id, pt::at(pt::Global::PlayMode), 0.0f);
	shoot("selector_window");
	// THE STEP INSPECTOR: a step selected in Melody, then a cell in Drum.
	f.deviceHost.setField(id, pt::at(0, pt::Field::Zone), 420.0f);
	f.deviceHost.setField(id, pt::pinAt(0, feathers::mod::arp::genes::Pin::Detail), 0.0f);
	window->showPage(0);
	(void)window->updateContent();
	{
		ui::PatternLane& lane = window->lane();
		int k = -1;
		for (size_t s = 0; s < lane.cycleForTest().length && k < 0; ++s) {
			if (lane.cycleForTest().steps[s].kind == feathers::mod::arp::pattern::PatternStep::Kind::Note && s >= 2) {
				k = static_cast<int>(s);
			}
		}
		lane.select(k);
	}
	shoot("inspector_melody");
	f.deviceHost.setField(id, pt::at(0, pt::Field::Mode), static_cast<float>(feathers::mod::arp::genes::Mode::Drum));
	(void)window->updateContent();
	window->lane().select(4, static_cast<size_t>(feathers::mod::arp::pattern::DrumLane::Snare));
	shoot("inspector_drum");
	// THE FILL'S MARKERS: tab 1 a fill with its window [4, 12), Start and End on the lane.
	f.deviceHost.setField(id, pt::at(0, pt::Field::Mode), static_cast<float>(feathers::mod::arp::genes::Mode::Melody));
	f.deviceHost.setField(id, pt::at(0, pt::Field::Fill), 1.0f);
	f.deviceHost.setField(id, pt::at(0, pt::Field::StartPhase), 4.0f);
	f.deviceHost.setField(id, pt::at(0, pt::Field::EndPhase), 12.0f);
	f.deviceHost.setField(id, pt::at(pt::Global::PlayMode), 1.0f);
	f.deviceHost.setField(id, pt::at(0, pt::Field::Span), 1.0f);
	window->showPage(0);
	window->lane().select(-1);
	shoot("fill_markers");
}
TEST_CASE("the Pattern map marks the focused slot's likes: a heart at the puck draws a mark there, by the stored point's projection, and Rotate keeps it under the puck") {
	namespace pt = feathers::mod::arp::pat;
	ModFixture f;
	const size_t slot = f.bridge.addRackDevice(pt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxRackSlots);
	f.bridge.refresh();
	const DeviceInstanceId id = f.bridge.rackDeviceAt(slot);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, id, pt::kDescriptor);
	auto* window = dynamic_cast<ui::PatternExpanded*>(view.get());
	REQUIRE(window != nullptr);
	window->setBounds(0.0f, 0.0f, 900.0f, 700.0f);
	window->setFocus(1);
	ui::PatternTerrain& terrain = window->terrain();
	visage::Canvas canvas;
	terrain.draw(canvas);
	CHECK(terrain.likeMarksDrawnForTest() == 0);
	f.deviceHost.setField(id, pt::at(1, pt::Field::Zone), 512.0f);
	f.deviceHost.setField(id, pt::at(1, pt::Field::PlaceX), 0.1f);
	f.deviceHost.setField(id, pt::at(1, pt::Field::PlaceY), 0.05f);
	(void)window->updateContent();
	REQUIRE(terrain.like());
	std::vector<feathers::mod::arp::pat::PlaneMark> marks = terrain.likeMarks();
	REQUIRE(marks.size() == 1);
	CHECK(marks[0].x == doctest::Approx(0.1).epsilon(1e-4));
	CHECK(marks[0].y == doctest::Approx(0.05).epsilon(1e-4));
	(void)terrain.poll();
	terrain.draw(canvas);
	CHECK(terrain.likeMarksDrawnForTest() == 1);
	// ANOTHER SLOT'S LIKES ARE NOT THIS MAP'S.
	window->setFocus(0);
	terrain.draw(canvas);
	CHECK(terrain.likeMarksDrawnForTest() == 0);
	window->setFocus(1);
	// A TURN, folded by the window: the pattern under the puck stays, and so does its mark.
	f.deviceHost.setField(id, pt::at(1, pt::Field::Rotate), 0.8f);
	(void)window->updateContent();
	marks = terrain.likeMarks();
	REQUIRE(marks.size() == 1);
	CHECK(marks[0].x == doctest::Approx(0.1).epsilon(1e-4));
	CHECK(marks[0].y == doctest::Approx(0.05).epsilon(1e-4));
	// AND THE PUCK MOVED AWAY LEAVES THE MARK WHERE THE LIKE IS.
	f.deviceHost.setField(id, pt::at(1, pt::Field::PlaceX), -0.2f);
	marks = terrain.likeMarks();
	CHECK(marks[0].x == doctest::Approx(0.1).epsilon(1e-4));
	REQUIRE(f.bridge.removeRackDevice(slot));
}
TEST_CASE("the Pattern window's like log: the first like in a slot writes the slot's set before it, then each like is one line with its place, fingerprint and point") {
	namespace pt = feathers::mod::arp::pat;
	const std::filesystem::path dir = std::filesystem::temp_directory_path()
	                                  / ("feathers-pattern-likes-" + std::to_string(static_cast<long>(::getpid())));
	std::filesystem::remove_all(dir);
	REQUIRE(setenv(ui::likeslog::kEnvVar, dir.c_str(), 1) == 0);
	ModFixture f;
	const size_t slot = f.bridge.addRackDevice(pt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxRackSlots);
	f.bridge.refresh();
	const DeviceInstanceId id = f.bridge.rackDeviceAt(slot);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, id, pt::kDescriptor);
	auto* window = dynamic_cast<ui::PatternExpanded*>(view.get());
	REQUIRE(window != nullptr);
	window->setBounds(0.0f, 0.0f, 900.0f, 700.0f);
	window->setFocus(2);
	f.deviceHost.setField(id, pt::at(2, pt::Field::PlaceX), 0.25f);
	REQUIRE(window->terrain().like());
	f.deviceHost.setField(id, pt::at(2, pt::Field::PlaceX), -0.5f);
	REQUIRE(window->terrain().like());
	REQUIRE(setenv(ui::likeslog::kEnvVar, "", 1) == 0);
	const std::string lastLike = ui::patternlog::likeJson(window->terrain().likesNow().like[1]);
	REQUIRE(f.bridge.removeRackDevice(slot));
	std::vector<std::string> lines;
	for (const auto& entry : std::filesystem::directory_iterator(dir)) {
		CHECK(entry.path().filename().string().ends_with(".pattern.jsonl"));
		std::ifstream in(entry.path());
		for (std::string line; std::getline(in, line);) {
			lines.push_back(line);
		}
	}
	std::filesystem::remove_all(dir);
	REQUIRE(lines.size() == 3);
	const std::string slotTag = "\"device\":\"PAT\",\"instance\":" + std::to_string(id) + ",\"slot\":2,";
	CHECK(lines[0].find(slotTag + "\"event\":\"snapshot\",\"likes\":[]") != std::string::npos);
	CHECK(lines[1].find(slotTag + "\"event\":\"like\",\"index\":0,\"dropped\":false") != std::string::npos);
	CHECK(lines[1].find("\"placeX\":0.25,") != std::string::npos);
	CHECK(lines[2].find("\"index\":1,") != std::string::npos);
	CHECK(lines[2].find("\"placeX\":-0.5,") != std::string::npos);
	// BY VALUE: the line's like is the block's like, to the float.
	CHECK(lines[2].find(lastLike) != std::string::npos);
	for (const std::string& line : lines) {
		CHECK(line.front() == '{');
		CHECK(line.back() == '}');
	}
}
/// THIS THREAD'S CPU time in ms: what the heart COSTS the UI thread whatever the host is doing (backlog PL: a wall-clock bound measured
/// the scheduler at load 57 and read 8.7 ms against 5). The fit it must not run is ~1.7 s of CPU.
static double patternThreadCpuMs() {
	timespec t {};
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
	return static_cast<double>(t.tv_sec) * 1e3 + static_cast<double>(t.tv_nsec) * 1e-6;
}
TEST_CASE("the Pattern window's heart returns at once with the slot full of likes: the likes' warp fits in the background, never on the UI thread") {
	namespace pt = feathers::mod::arp::pat;
	ModFixture f;
	const size_t slot = f.bridge.addRackDevice(pt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxRackSlots);
	f.bridge.refresh();
	const DeviceInstanceId id = f.bridge.rackDeviceAt(slot);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, id, pt::kDescriptor);
	auto* window = dynamic_cast<ui::PatternExpanded*>(view.get());
	REQUIRE(window != nullptr);
	window->setBounds(0.0f, 0.0f, 900.0f, 700.0f);
	f.deviceHost.setField(id, pt::at(pt::Global::LikesWarp), 1.0f);
	ui::PatternTerrain& terrain = window->terrain();
	double worst = 0.0;
	for (int i = 0; i < 16; ++i) {
		f.deviceHost.setField(id, pt::at(0, pt::Field::PlaceX), -0.8f + 0.1f * static_cast<float>(i));
		const double t0 = patternThreadCpuMs();
		REQUIRE(terrain.like());
		const double ms = patternThreadCpuMs() - t0;
		worst = std::max(worst, ms);
	}
	MESSAGE("the slowest of 16 hearts cost the UI thread " << worst << " ms of CPU (a 16-like fit is ~1.7 s)");
	CHECK(worst < 5.0);
	CHECK(terrain.likesNow().count == 16);
	REQUIRE(f.bridge.removeRackDevice(slot)); // and the fit still running is stopped with its device
}
TEST_CASE("NQ: a card dragged out of one Rack chain and dropped on another chain's column moves there — same id, its fields, ONE undo step in the host's history, undone and redone by value") {
	namespace pr = feathers::rack::parallel;
	MirroredRack m;
	m.panel->addChainForTest();
	(void)m.panel->update();
	REQUIRE(m.r.rack().chainCount() == 2);
	rack::RackBridge& a = *m.r.rack().chainAt(0)->bridge();
	rack::RackBridge& b = *m.r.rack().chainAt(1)->bridge();
	const DeviceInstanceId filter = a.addDevice(fx::filter::kTraits.tag);
	REQUIRE(filter != kNoInstance);
	// A CONTINUOUS CONTROL, by its declaration (a stepped one would round 0.3 to a step, and index 0 is the Enable).
	const feathers::device::Descriptor& fd = fx::filter::kDescriptor;
	size_t c = 0;
	while (c < fd.params.size() && (fd.params[c].stepped || c == static_cast<size_t>(fd.traits.enableControl))) {
		++c;
	}
	REQUIRE(c < fd.params.size());
	const float want = static_cast<float>(fd.params[c].min + 0.3 * (fd.params[c].max - fd.params[c].min));
	a.setField(filter, c, want);
	const float field = a.fieldAt(filter, c);
	REQUIRE(field == doctest::Approx(want));
	for (size_t i = 0; i < 2; ++i) {
		(void)m.panel->railForTest(i).setCollapsed(false);
	}
	m.panel->setBounds(0.0f, 0.0f, 1400.0f, 800.0f);
	(void)m.panel->update();
	ui::ChainColumn* from = m.panel->railForTest(0).columnForTest();
	ui::ChainColumn* to = m.panel->railForTest(1).columnForTest();
	REQUIRE((from != nullptr && to != nullptr));
	REQUIRE(to->width() > 0.0f);
	const size_t steps = m.history.length();

	// THE DRAG: picked up on chain 0's card, released over chain 1's column.
	const visage::Bounds card = from->cardBoundsForTest(filter);
	REQUIRE(card.width() > 0.0f);
	const visage::Point over = to->positionInWindow() + visage::Point { to->width() * 0.5f, 8.0f };
	const visage::Point inCard = over - from->positionInWindow() - visage::Point { card.x(), card.y() };
	from->cardDragForTest(filter, ui::FxCard::DragPhase::Begin, { 4.0f, 4.0f });
	from->cardDragForTest(filter, ui::FxCard::DragPhase::Move, inCard);
	CHECK_FALSE(from->dropGap().has_value()); // outside its own column
	from->cardDragForTest(filter, ui::FxCard::DragPhase::End, inCard);

	CHECK(a.positionOf(filter) == rack::RackBridge::kNoSlot);
	REQUIRE(b.positionOf(filter) != rack::RackBridge::kNoSlot); // the same id, in the other chain
	CHECK(b.fieldAt(filter, c) == doctest::Approx(field));
	CHECK(m.history.length() == steps + 1);
	m.history.undo();
	CHECK(b.positionOf(filter) == rack::RackBridge::kNoSlot);
	REQUIRE(a.positionOf(filter) != rack::RackBridge::kNoSlot);
	CHECK(a.fieldAt(filter, c) == doctest::Approx(field));
	m.history.redo();
	CHECK(b.positionOf(filter) != rack::RackBridge::kNoSlot);
	CHECK(a.positionOf(filter) == rack::RackBridge::kNoSlot);
	// A RELEASE OVER NOTHING STAYS THE CANCEL.
	ui::ChainColumn* now = m.panel->railForTest(1).columnForTest();
	REQUIRE(now != nullptr);
	(void)m.panel->update();
	const visage::Bounds there = now->cardBoundsForTest(filter);
	const visage::Point nowhere { -500.0f - there.x(), -500.0f - there.y() };
	now->cardDragForTest(filter, ui::FxCard::DragPhase::Begin, { 4.0f, 4.0f });
	now->cardDragForTest(filter, ui::FxCard::DragPhase::Move, nowhere);
	now->cardDragForTest(filter, ui::FxCard::DragPhase::End, nowhere);
	CHECK(b.positionOf(filter) != rack::RackBridge::kNoSlot);
	CHECK(m.history.length() == steps + 1);
}
TEST_CASE("the Pattern lane highlights the tonic's rows: every key in view at the cycle's root in any octave — the held C4 under Transpose, Root under Pool — on card and window; none in Drum") {
	// The tonic's row is highlighted too. The expectation is restated, not read back: the root is
	// the held C4 under Transpose and 48 + Root under Pool, and a key is in view when its row lies inside the lane.
	namespace pt = feathers::mod::arp::pat;
	namespace gn = feathers::mod::arp::genes;
	ModFixture f;
	const size_t slot = f.bridge.addRackDevice(pt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxRackSlots);
	f.bridge.refresh();
	f.panel->resized();
	auto* card = const_cast<ui::PatternCard*>(dynamic_cast<const ui::PatternCard*>(f.panel->cardFrameForTest(slot)));
	REQUIRE(card != nullptr);
	const DeviceInstanceId id = card->device();
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, id, pt::kDescriptor);
	auto* window = dynamic_cast<ui::PatternExpanded*>(view.get());
	REQUIRE(window != nullptr);
	const visage::Point min = window->minimumSize();
	window->setBounds(0.0f, 0.0f, min.x, min.y);
	f.deviceHost.setField(id, pt::at(0, pt::Field::Zone), 420.0f);
	const auto expected = [](const ui::PatternLane& lane, int root) {
		const auto h = static_cast<float>(lane.height());
		const float barHeight = std::max(2.0f, (h - 6.0f) / 16.0f);
		std::vector<int> keys;
		for (int key = 0; key < 128; ++key) {
			const float y = lane.yOfKey(key);
			if (((key - root) % 12 + 12) % 12 == 0 && y >= 3.0f - 0.01f && y <= h - 3.0f - barHeight + 0.01f) {
				keys.push_back(key);
			}
		}
		return keys;
	};
	struct Case {
		float keys;
		float root;
		int rootKey;
	};
	for (const Case c : { Case { 0.0f, 0.0f, 60 }, Case { 0.0f, 7.0f, 60 }, Case { 1.0f, 0.0f, 48 }, Case { 1.0f, 5.0f, 53 },
	                      Case { 1.0f, 11.0f, 59 } }) {
		CAPTURE(c.keys);
		CAPTURE(c.root);
		f.deviceHost.setField(id, pt::at(pt::Global::Keys), c.keys);
		f.deviceHost.setField(id, pt::at(0, pt::Field::Root), c.root);
		(void)card->updateContent();
		(void)window->updateContent();
		for (const ui::PatternLane* lane : std::array<const ui::PatternLane*, 2> { &card->lane(), &window->lane() }) {
			REQUIRE(lane->height() > 0);
			const std::vector<int> want = expected(*lane, c.rootKey);
			CHECK_FALSE(want.empty());
			CHECK(lane->tonicKeys() == want);
		}
	}
	// DRUM: rows are lanes, so no tonic.
	f.deviceHost.setField(id, pt::at(0, pt::Field::Mode), static_cast<float>(gn::Mode::Drum));
	(void)card->updateContent();
	(void)window->updateContent();
	CHECK(card->lane().tonicKeys().empty());
	CHECK(window->lane().tonicKeys().empty());
}
TEST_CASE("Feathers' modulation-rack picker offers the modulators AND the note effects (Arp, Pattern), filed under two headings") {
	// A "note" category keeps Pattern and Arp from riding along with the modulators; both modulators and note
	// effects are in scope for Feathers' modulation rack. The offer is the device's own
	// statement (a "modulator" or "note-effect" tag, `belongsInModulationRack`); the heading is its category.
	ModFixture f;
	std::set<std::string> tags;
	for (const feathers::device::Descriptor* d : f.panel->addableForTest()) {
		tags.insert(d->traits.tag);
		CHECK((d->category == feathers::device::Category::Modulators || d->category == feathers::device::Category::Notes));
	}
	CHECK(tags.count(feathers::mod::arp::kTraits.tag) == 1);
	CHECK(tags.count(feathers::mod::arp::pat::kTraits.tag) == 1);
	CHECK(tags.count(feathers::mod::lfo::kTraits.tag) == 1);
	CHECK(tags.count(feathers::mod::dahdsr::kTraits.tag) == 1);
	const visage::PopupMenu menu = f.panel->addMenuForTest();
	const std::vector<std::string> headings = headingsOf(menu);
	CHECK(std::find(headings.begin(), headings.end(), "Modulators") != headings.end());
	CHECK(std::find(headings.begin(), headings.end(), "Note FX") != headings.end());
}
TEST_CASE("the Pattern window's lane names its gestures on hover — and every gesture it names does what it says: a pitch "
          "drag, a drag from a bar's end for length; Drum names its own; the card's lane, a view, names none; Rotate keeps "
          "the edits and a Mode change clears them") {
	// The pattern lane has a gesture tooltip, because how to modify notes was not discoverable. Select and
	// double-click (pitched and Drum) are driven by the cases above (2026-11-05); this one drives the drags.
	namespace pt = feathers::mod::arp::pat;
	namespace pn = feathers::mod::arp::pattern;
	namespace gn = feathers::mod::arp::genes;
	using Field = pt::Field;
	ModFixture f;
	const size_t slot = f.bridge.addRackDevice(pt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxRackSlots);
	f.bridge.refresh();
	f.panel->resized();
	const DeviceInstanceId id = f.bridge.rackDeviceAt(slot);
	// THE CARD'S LANE IS A VIEW: no gesture, so no tooltip.
	auto* card = const_cast<ui::PatternCard*>(dynamic_cast<const ui::PatternCard*>(f.panel->cardFrameForTest(slot)));
	REQUIRE(card != nullptr);
	CHECK(card->lane().tooltipAt({ 10.0f, 10.0f }) == nullptr);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, id, pt::kDescriptor);
	auto* window = dynamic_cast<ui::PatternExpanded*>(view.get());
	REQUIRE(window != nullptr);
	ui::EditHistory history;
	window->setModulation(nullptr, &history);
	const visage::Point min = window->minimumSize();
	window->setBounds(0.0f, 0.0f, min.x, min.y);
	f.deviceHost.setField(id, pt::at(0, Field::Zone), 420.0f);
	(void)window->updateContent();
	ui::PatternLane& lane = window->lane();
	REQUIRE(lane.cycleForTest().length == 16);

	// THE TEXT: the pitched gestures, and NOTHING the lane does not do (no orbit, Radius or Theta: they are gone).
	const std::string text = lane.tooltipAt({ lane.width() * 0.5f, lane.height() * 0.5f });
	CHECK(text == ui::PatternLane::kPitchedGestures);
	for (const char* named : { "click a step to select it", "step inspector", "double-click to add a note", "delete the note",
	                           "drag a note up or down", "right end", "Rotate keeps them", "the Mode changes" }) {
		CAPTURE(named);
		CHECK(text.find(named) != std::string::npos);
	}
	for (const char* absent : { "orbit", "Radius", "Theta", "right-click", "Shift", "Alt", "Ctrl", "wheel" }) {
		CAPTURE(absent);
		CHECK(text.find(absent) == std::string::npos);
	}

	const pn::PatternCycle terrain = lane.terrainForTest();
	size_t note = 99;
	for (size_t k = 0; k < 16 && note == 99; ++k) {
		if (terrain.steps[k].kind == pn::PatternStep::Kind::Note && k + 1 < 16) {
			note = k;
		}
	}
	REQUIRE(note < 16);
	const float column = lane.xOfStep(1) - lane.xOfStep(0);
	const auto withMods = [](visage::MouseEvent e, int mods) {
		e.modifiers = mods;
		return e;
	};
	const auto gesture = [&](visage::MouseEvent down, visage::MouseEvent drag, visage::MouseEvent up) {
		lane.onMouseDown().callback(down);
		lane.onMouseDrag().callback(drag);
		lane.onMouseUp().callback(up);
	};
	const float x = lane.xOfStep(note) + 2.0f;
	// DRAG UP OR DOWN: the pitch, snapped to the scale.
	const float yFrom = lane.yOfKey(terrain.keyAt(note, 0)) + 1.0f;
	const float yTo = std::max(4.0f, yFrom - 30.0f);
	gesture(at(x, yFrom), at(x, yTo), at(x, yTo));
	CHECK(lane.cycleForTest().steps[note].keys[0] == lane.keyAt(yTo));
	CHECK(lane.cycleForTest().steps[note].keys[0] != terrain.steps[note].keys[0]);
	CHECK(lane.selectedStep() == static_cast<int>(note)); // the press selected the note it dragged
	CHECK(history.length() == 1);                          // and the drag is one undo step
	// A DRAG FROM A NOTE'S RIGHT END: its length.
	const pn::PatternStep& st = lane.cycleForTest().steps[note];
	const float end = lane.xOfStep(note) + st.gate * column;
	const float gateWas = st.gate;
	const float yNow = lane.yOfKey(lane.cycleForTest().keyAt(note, 0)) + 1.0f;
	gesture(at(end, yNow), at(end + column, yNow), at(end + column, yNow));
	CHECK(lane.cycleForTest().steps[note].gate > gateWas + 0.5f);
	(void)withMods;
	// WHEN EDITS CLEAR, as the line says: ROTATE KEEPS THEM; A MODE CHANGE CLEARS THEM.
	std::vector<float> block(pt::kEditsWords);
	const auto edited = [&] {
		REQUIRE(f.deviceHost.blockAt(id, pt::kEditsBlock, block));
		return pt::editStateOf(block.data(), block.size())[0].any();
	};
	REQUIRE(edited());
	f.deviceHost.setField(id, pt::at(0, Field::Rotate), 0.37f);
	(void)window->updateContent();
	CHECK(edited());
	f.deviceHost.setField(id, pt::at(0, Field::Mode), static_cast<float>(gn::Mode::Drum));
	(void)window->updateContent();
	CHECK_FALSE(edited());
	// AND DRUM NAMES ITS OWN.
	REQUIRE(lane.drumForTest());
	CHECK(std::string(lane.tooltipAt({ lane.width() * 0.5f, lane.height() * 0.5f })) == ui::PatternLane::kDrumGestures);
	REQUIRE(f.bridge.removeRackDevice(slot));
}
TEST_CASE("delay-love S2: the Delay's window — FEEDBACK FX is a PHANTOM rail until its (+) builds the lane, then the "
          "rail edits that lane") {
	namespace dl = feathers::fx::delay;
	DelayRig rig;
	REQUIRE(rig.surface() != nullptr);
	ui::WidgetRegistry registry;
	ui::DelayExpanded panel(rig.bridge, registry, rig.id);
	const visage::Point min = panel.minimumSize();
	panel.setBounds(0.0f, 0.0f, min.x, min.y);

	ui::FxLaneRail& rail = panel.laneRailForTest();
	CHECK(std::string(rail.spec().label) == ui::DelayExpanded::kLaneLabel);
	// OPEN BY DEFAULT, ON THE RIGHT, AND A PHANTOM: no lane exists, and opening the window did not make one (decision 9).
	CHECK_FALSE(rail.collapsed());
	CHECK(rail.x() > panel.planeForTest().right());
	CHECK(rail.phantom());
	CHECK_FALSE(rail.liveForTest());
	CHECK(dl::feedbackLaneOf(rig.surface()) == nullptr);
	(void)panel.updateContent();
	CHECK(dl::feedbackLaneOf(rig.surface()) == nullptr);

	// THE (+) BUILDS IT — and the rail, re-asked on the next tick, edits exactly that lane.
	const visage::Bounds add = rail.phantomAddBounds();
	rail.onMouseDown().callback(at(add.x() + add.width() * 0.5f, add.y() + add.height() * 0.5f));
	feathers::rack::InnerChain* lane = dl::feedbackLaneOf(rig.surface());
	REQUIRE(lane != nullptr);
	(void)panel.updateContent();
	CHECK_FALSE(rail.phantom());
	REQUIRE(rail.liveForTest());
	CHECK(rail.bridgeForTest() == lane->bridge());
	REQUIRE(rail.bridgeForTest()->addDevice(feathers::fx::busutil::kTraits.tag) != kNoInstance);
	CHECK(lane->bridge()->chainLength() == 1);
}
TEST_CASE("delay-love S2: the Delay's window offers every control, each where its gate says, by Mode") {
	namespace dl = feathers::fx::delay;
	DelayRig rig;
	ui::WidgetRegistry registry;
	ui::DelayExpanded panel(rig.bridge, registry, rig.id);
	const visage::Point min = panel.minimumSize();
	panel.setBounds(0.0f, 0.0f, min.x, min.y);
	// EVERY CONTROL BUT THE ENABLE HAS A WIDGET HERE, read off the tree.
	const std::vector<dl::Param> offered = panel.addressedParamsForTest();
	for (size_t i = 1; i < static_cast<size_t>(dl::Param::kCount); ++i) {
		CAPTURE(i);
		CHECK(std::find(offered.begin(), offered.end(), static_cast<dl::Param>(i)) != offered.end());
	}
	// THE MODE BAND SHOWS THE LIVE MODE'S FOUR AND NO OTHER'S.
	const auto mode = static_cast<size_t>(dl::Param::Mode);
	for (const auto& [m, shown, hidden] : { std::tuple { dl::Mode::Granular, dl::Param::Density, dl::Param::Wow },
	                                        std::tuple { dl::Mode::Tape, dl::Param::Wow, dl::Param::Taps },
	                                        std::tuple { dl::Mode::MultiTap, dl::Param::TapPattern, dl::Param::Spray } }) {
		CAPTURE(static_cast<int>(m));
		rig.bridge.setField(rig.id, mode, static_cast<float>(static_cast<int>(m)));
		(void)panel.updateContent();
		CHECK(panel.widgetForTest(shown)->isVisible());
		CHECK_FALSE(panel.widgetForTest(hidden)->isVisible());
		// THE BAND'S WIDGET IS UNDER THE PLOT AND INSIDE THE FACE, never under the rail.
		CHECK(panel.widgetForTest(shown)->y() > panel.planeForTest().bottom());
		CHECK(panel.widgetForTest(shown)->right() <= panel.laneRailForTest().x());
	}
}
TEST_CASE("delay-love S2: the card's FB FX chip counts the lane's devices and opens the window") {
	namespace dl = feathers::fx::delay;
	DelayRig rig;
	ui::DelayCard card(rig.bridge, rig.id);
	card.setBounds(0.0f, 0.0f, 240.0f, 400.0f);
	(void)card.updateContent();
	CHECK(card.laneChipTextForTest() == ui::DelayCard::kLaneChipEmpty);
	visage::Frame& chip = card.laneChipForTest();
	CHECK(chip.isVisible());
	// INSIDE THE STRIP, bottom right.
	const visage::Bounds strip = card.stripBoundsForTest();
	CHECK(chip.right() <= strip.right());
	CHECK(chip.bottom() <= strip.bottom());
	CHECK(chip.x() > strip.x() + strip.width() * 0.5f);

	bool opened = false;
	card.onExpand = [&] { opened = true; };
	chip.mouseDown(at(2.0f, 2.0f));
	CHECK(opened);

	// TWO DEVICES IN THE LANE: the chip says so on the next tick.
	feathers::rack::InnerChain* lane = dl::ensureFeedbackLane(rig.surface());
	REQUIRE(lane != nullptr);
	REQUIRE(lane->bridge()->addDevice(feathers::fx::busutil::kTraits.tag) != kNoInstance);
	REQUIRE(lane->bridge()->addDevice(feathers::fx::busutil::kTraits.tag) != kNoInstance);
	CHECK(card.updateContent());
	CHECK(card.laneChipTextForTest() == std::string(ui::DelayCard::kLaneChipPrefix) + "2");
}
TEST_CASE("delay-love S3: each handle on the time plane writes exactly its control, by the plane's own mapping") {
	namespace dl = feathers::fx::delay;
	DelayRig rig;
	runDelayRig(rig, 4800);
	ui::WidgetRegistry registry;
	ui::DelayExpanded panel(rig.bridge, registry, rig.id);
	const visage::Point min = panel.minimumSize();
	panel.setBounds(0.0f, 0.0f, min.x + 200.0f, min.y + 100.0f);
	(void)panel.updateContent();
	ui::DelayPlane& plane = panel.planeForTest();
	using H = ui::DelayPlane::Handle;
	dl::TapPicture p;
	REQUIRE(ui::delayPicture(rig.bridge, rig.id, &p));
	const ui::DelayStrip s = plane.strip();
	const double unit = p.axisUnit * std::exp2(static_cast<double>(rig.bridge.fieldAt(rig.id, static_cast<size_t>(dl::Param::Time))));
	const auto field = [&](dl::Param q) { return static_cast<double>(rig.bridge.fieldAt(rig.id, static_cast<size_t>(q))); };

	// TIME: the handle sits ON the first left repeat; dragged to 1.5x the tap, Time moves by log2(1.5).
	visage::Point at {};
	REQUIRE(plane.handleAt(H::Time, &at));
	CHECK(at.x == doctest::Approx(s.xAt(unit)));
	CHECK(plane.handleNear(at) == H::Time);
	const double time0 = field(dl::Param::Time);
	plane.dragForTest(at, { s.xAt(1.5 * unit), at.y });
	CHECK(field(dl::Param::Time) == doctest::Approx(time0 + std::log2(1.5)).epsilon(2e-3));
	rig.bridge.setField(rig.id, static_cast<size_t>(dl::Param::Time), static_cast<float>(time0));

	// FEEDBACK: the handle on the second repeat's tip; dragged to the height of 0.6 of the first, Feedback is 0.6.
	REQUIRE(plane.handleAt(H::Feedback, &at));
	CHECK(at.x == doctest::Approx(s.xAt(2.0 * unit)));
	plane.dragForTest(at, { at.x, s.tipY(0, p.wetGain * 0.6f) });
	CHECK(field(dl::Param::Feedback) == doctest::Approx(0.6).epsilon(2e-3));

	// TONE: up 60 px is +0.5 from where it was.
	REQUIRE(plane.handleAt(H::Tone, &at));
	const double tone0 = field(dl::Param::Tone);
	plane.dragForTest(at, { at.x, at.y - 0.5f * ui::DelayPlane::kTonePixels });
	CHECK(field(dl::Param::Tone) == doctest::Approx(tone0 + 0.5).epsilon(1e-6));

	// SPREAD: dragged to 1.1x the left tap, Spread is 0.4 (the right tap is the left times 1 + 0.25 Spread).
	REQUIRE(plane.handleAt(H::Spread, &at));
	plane.dragForTest(at, { s.xAt(unit * (1.0 + dl::kSpreadFraction * 0.4)), at.y });
	CHECK(field(dl::Param::Spread) == doctest::Approx(0.4).epsilon(2e-3));

	// NOT MULTI-TAP, SO NO TAPS AND NO PUCK — their gate is shut, so they are not drawn and not taken.
	CHECK_FALSE(plane.handleAt(H::Taps, nullptr));
	CHECK_FALSE(plane.handleAt(H::Cluster, nullptr));

	// MULTI-TAP: two taps more for two steps left; the puck moves Pattern and Levels inside their zones.
	rig.bridge.setField(rig.id, static_cast<size_t>(dl::Param::Mode), static_cast<float>(static_cast<int>(dl::Mode::MultiTap)));
	runDelayRig(rig, 4800);
	(void)panel.updateContent();
	REQUIRE(ui::delayPicture(rig.bridge, rig.id, &p));
	REQUIRE(plane.handleAt(H::Taps, &at));
	const double taps0 = field(dl::Param::Taps);
	plane.dragForTest(at, { at.x - 2.0f * ui::DelayPlane::kTapsPixels, at.y });
	CHECK(field(dl::Param::Taps) == taps0 + 2.0);
	const dl::TapZone pattern0 = dl::decodeTapCoord(static_cast<float>(field(dl::Param::TapPattern)));
	const dl::TapZone levels0 = dl::decodeTapCoord(static_cast<float>(field(dl::Param::TapLevels)));
	REQUIRE(plane.handleAt(H::Cluster, &at));
	const ui::DelayStrip s2 = plane.strip();
	const float span = s2.bounds.height() * 0.5f - 12.0f;
	plane.dragForTest(at, { s2.xAt(unit * (0.25 + 0.5 * 0.8)), s2.midY() - 0.2f * span });
	const dl::TapZone pattern1 = dl::decodeTapCoord(static_cast<float>(field(dl::Param::TapPattern)));
	const dl::TapZone levels1 = dl::decodeTapCoord(static_cast<float>(field(dl::Param::TapLevels)));
	CHECK(pattern1.zone == pattern0.zone);
	CHECK(levels1.zone == levels0.zone);
	CHECK(pattern1.position == doctest::Approx(0.8).epsilon(5e-3));
	CHECK(levels1.position == doctest::Approx(0.7).epsilon(5e-3));
	// THE PLANE DECLARES WHAT IT WRITES, so the superset guard can see it (a Gesture road).
	std::vector<ui::ControlFace::Offered> offered;
	plane.offeredControls(offered);
	CHECK(offered.size() == 7);
}

