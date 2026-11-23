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

// PIECE 18 OF 18 of the devices suite: its cases in their original order, with the helpers only they use. See devices_common.h.

#include "devices/devices_common.h"

// A PICTURE OF THE ROW, when FEATHERS_SLOT_SHOT_DIR names a directory: the composite on the card's ground at both scales.
TEST_CASE("The slot row is photographed at the card width at scale 1 and 1.75 when asked") {
	const char* dir = std::getenv("FEATHERS_SLOT_SHOT_DIR");
	if (dir == nullptr) {
		return;
	}
	for (const auto& [tag, type] : { std::pair<const char*, ero::DriveMode> { "", ero::DriveMode::Decimate },
	                                 std::pair<const char*, ero::DriveMode> { "-lopside", ero::DriveMode::Lopside } })
	for (const float scale : { 1.0f, 1.75f }) {
		SlotRow r;
		r.d.set(eroAt(ero::Param::DriveMode), static_cast<float>(type));
		struct Ground final : visage::Frame {
			void draw(visage::Canvas& canvas) override {
				canvas.setColor(ui::kSurface);
				canvas.fill(0.0f, 0.0f, static_cast<float>(width()), static_cast<float>(height()));
			}
		} ground;
		constexpr float kMargin = 8.0f;
		const float w = r.rowW + 2.0f * kMargin;
		const float h = r.rowH + 2.0f * kMargin;
		ground.addChild(r.outer.get());
		visage::ApplicationEditor editor;
		editor.setDpiScale(scale);
		editor.setBounds(0.0f, 0.0f, w, h);
		editor.addChild(&ground);
		ground.setBounds(0.0f, 0.0f, w, h);
		r.outer->setBounds(kMargin, kMargin, r.rowW, r.rowH);
		const int pw = static_cast<int>(std::ceil(w * scale));
		const int ph = static_cast<int>(std::ceil(h * scale));
		editor.setWindowless(pw, ph);
		if (!visage::Renderer::instance().supported()) {
			MESSAGE("SKIP no renderer: " << visage::Renderer::instance().errorMessage());
			ground.removeChild(r.outer.get());
			editor.removeChild(&ground);
			return;
		}
		editor.redrawAll();
		editor.drawWindow();
		editor.drawWindow();
		const visage::Screenshot& shot = editor.takeScreenshot();
		char name[64];
		std::snprintf(name, sizeof(name), "/slotrow2%s-%.2f.png", tag, static_cast<double>(scale));
		shot.save(std::string(dir) + name);
		MESSAGE("saved " << dir << name << " " << shot.width() << "x" << shot.height());
		ground.removeChild(r.outer.get());
		editor.removeChild(&ground);
	}
}

namespace {
//--------------------------------------------------------------------------------------------------//
// ETCH'S FACES: two slots and their route on one line.                                               //
//--------------------------------------------------------------------------------------------------//

visage::Point eroCentre(const visage::Bounds& b) {
	return { b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f };
}
} // namespace

TEST_CASE("ETCH's card draws its slot row on one line, both slots and the route inside the card and apart, with Freq and Mix as boxes under it") {
	EroFaces e;
	auto& card = *e.d.card;
	e.mode(ero::Mode::Etch);
	REQUIRE(ui::EroderCard::openHeight() <= ui::kCardUnitHeight);
	std::vector<visage::Frame*> want = card.etchWidgetsForTest();
	want.push_back(card.drivePairForTest());
	want.push_back(card.emphasisForTest());
	want.push_back(card.resonanceForTest());
	const auto shown = eroVisible(&card, { card.plotForTest(), card.modeForTest() });
	CHECK(eroSameSet(shown, want));
	eroApart(shown, card.width(), ui::EroderCard::openHeight());

	ui::EtchSlots& slots = card.etchSlotsForTest();
	const float cell = static_cast<float>(card.drivePairForTest()->width()) * 0.5f;
	CHECK(slots.outer->width() == doctest::Approx(4.0f * cell));
	CHECK(slots.outer->bare());
	// ONE LINE: A, the route, B left to right, top-aligned, none over another, all inside the row and so inside the card.
	CHECK(slots.innerA->right() <= slots.route->x());
	CHECK(slots.route->right() <= slots.innerB->x());
	CHECK(slots.innerA->y() == slots.innerB->y());
	CHECK(slots.route->y() >= 0.0f);
	CHECK(slots.route->bottom() <= slots.outer->height());
	CHECK(slots.innerB->right() <= slots.outer->width());
	CHECK(slots.outer->x() + slots.outer->width() <= card.width());
	CHECK(slots.outer->bottom() <= ui::EroderCard::openHeight());
	// BOTH PAIRS ARE WHOLE.
	CHECK(slots.innerA->first().parent() == slots.innerA.get());
	CHECK(slots.innerB->second().parent() == slots.innerB.get());
	CHECK(slots.knobA->control()->desc().key == std::string_view("eroder_slot_a_amount"));
	CHECK(slots.knobB->control()->desc().key == std::string_view("eroder_slot_b_amount"));
	CHECK(slots.route->control().desc().key == std::string_view("eroder_slot_route"));
	for (ui::ModeValueCell* m : { slots.cellA.get(), slots.cellB.get() }) {
		CHECK(m->stacked());
		// THE CARD'S SLOT CELL: Shape and the Type's second parameter (Bal, the Rate in hertz, the Rate as a multiple); the slot's Mix is the WINDOW's knob.
		CHECK(m->followerParams().size() == 8);
		CHECK((m->pillFor(eroAt(ero::Param::SlotAKey)) != nullptr || m->pillFor(eroAt(ero::Param::SlotBKey)) != nullptr));
		CHECK(m->boxFor(eroAt(ero::Param::SlotAMix)) == nullptr);
		CHECK(m->boxFor(eroAt(ero::Param::SlotBMix)) == nullptr);
	}
	// FREQ AND MIX ARE BOXES IN THE SMALL ROW, with Track at its end, none over another.
	CHECK(card.freqBoxForTest()->control().desc().key == std::string_view("eroder_freq"));
	CHECK(card.mixBoxForTest()->control().desc().key == std::string_view("eroder_mix"));
	CHECK(card.freqBoxForTest()->isCompact());
	CHECK(card.freqBoxForTest()->y() >= slots.outer->bottom());
	CHECK(card.freqBoxForTest()->right() <= card.mixBoxForTest()->x());
	CHECK(card.mixBoxForTest()->right() <= card.etchWidgetsForTest()[3]->x());
	// ETCH USES NEITHER AMOUNT, SOURCE NOR GRIT: none is drawn.
	CHECK_FALSE(card.mixForTest()->isVisible());
	CHECK_FALSE(card.widthForTest()->isVisible());
	for (visage::Frame* f : card.dustWidgetsForTest()) {
		CHECK_FALSE(f->isVisible());
	}
	for (visage::Frame* f : card.rustWidgetsForTest()) {
		CHECK_FALSE(f->isVisible());
	}
	// THE ENGINE SELECTOR OFFERS FOUR, and the plot draws the marker, not a band.
	CHECK(card.plotForTest()->showsEtchForTest());
	CHECK_FALSE(card.plotForTest()->showsRustForTest());
}
TEST_CASE("ETCH's slot cells carry the Type's second parameter on the card and in the window, and the window has the slot's Mix as a knob between the Amount knob and the cell") {
	EroFaces e;
	e.mode(ero::Mode::Etch);
	ui::EtchSlots& card = e.d.card->etchSlotsForTest();
	ui::EtchSlots& panel = e.panel->etchSlotsForTest();
	const auto slotCells = [&] { return std::vector<ui::ModeValueCell*> { card.cellA.get(), card.cellB.get(), panel.cellA.get(), panel.cellB.get() }; };
	const std::array<ero::Param, 2> types { ero::Param::SlotAType, ero::Param::SlotBType };
	const std::array<std::array<ero::Param, 6>, 2> second {
		std::array<ero::Param, 6> { ero::Param::SlotABal, ero::Param::SlotARate, ero::Param::SlotARateMult, ero::Param::SlotAHard, ero::Param::SlotASharp, ero::Param::SlotABias },
		std::array<ero::Param, 6> { ero::Param::SlotBBal, ero::Param::SlotBRate, ero::Param::SlotBRateMult, ero::Param::SlotBHard, ero::Param::SlotBSharp, ero::Param::SlotBBias },
	};
	const std::array<ero::Param, 2> keys { ero::Param::SlotAKey, ero::Param::SlotBKey };
	std::vector<float> thirdY;
	for (int slot = 0; slot < 2; ++slot) {
		for (const bool track : { false, true }) {
			e.set(ero::Param::Track, track ? 1.0f : 0.0f);
			for (int type = 0; type < static_cast<int>(ero::DriveMode::kCount); ++type) {
				e.set(types[static_cast<size_t>(slot)], static_cast<float>(type));
				const auto dm = static_cast<ero::DriveMode>(type);
				const ero::SecondParam want = ero::driveSecondParam(dm);
				CAPTURE(slot);
				CAPTURE(track);
				CAPTURE(std::string(ero::kDriveModeNames[type]));
				CHECK(want != ero::SecondParam::None); // EVERY TYPE USES THE THIRD ROW
				for (ui::ModeValueCell* cell : { slot == 0 ? card.cellA.get() : card.cellB.get(), slot == 0 ? panel.cellA.get() : panel.cellB.get() }) {
					const auto& p = second[static_cast<size_t>(slot)];
					const auto shown = [&](size_t i) { return cell->boxFor(eroAt(p[i]))->isVisible(); };
					ui::KeySpreadRow* pill = cell->pillFor(eroAt(keys[static_cast<size_t>(slot)]));
					REQUIRE(pill != nullptr);
					CHECK(shown(0) == (want == ero::SecondParam::Bal));
					CHECK(shown(1) == (want == ero::SecondParam::Rate && !track));
					CHECK(shown(2) == (want == ero::SecondParam::Rate && track));
					CHECK(shown(3) == (want == ero::SecondParam::Hard));
					CHECK(shown(4) == (want == ero::SecondParam::Sharp));
					CHECK(shown(5) == (want == ero::SecondParam::Bias));
					CHECK(pill->isVisible() == (want == ero::SecondParam::Key));
					// ONE ROW, ONE PLACE: the shown third row is where the others would be, in every Type.
					float y = -1.0f;
					for (size_t i = 0; i < 6; ++i) {
						y = shown(i) ? cell->boxFor(eroAt(p[i]))->y() : y;
					}
					y = pill->isVisible() ? pill->y() : y;
					CHECK(y > 0.0f);
					if (cell == (slot == 0 ? card.cellA.get() : card.cellB.get())) {
						thirdY.push_back(y);
					}
				}
			}
		}
	}
	for (const float y : thirdY) {
		CHECK(y == thirdY.front());
	}
	e.set(ero::Param::Track, 0.0f);
	// ... AND THE CELL'S SIZE DOES NOT MOVE with the Type.
	e.set(ero::Param::SlotAType, static_cast<float>(ero::DriveMode::Soft));
	const float h = static_cast<float>(card.cellA->height());
	e.set(ero::Param::SlotAType, static_cast<float>(ero::DriveMode::PaunchS));
	CHECK(static_cast<float>(card.cellA->height()) == h);
	e.set(ero::Param::SlotAType, static_cast<float>(ero::DriveMode::Chonk));
	CHECK(static_cast<float>(card.cellA->height()) == h);
	(void)slotCells;
	// THE WINDOW: Amount knob, then the Mix knob, then the cell, left to right, for each slot; the Mix knob writes the slot's own Mix.
	for (int slot = 0; slot < 2; ++slot) {
		ui::SlotTriple* row = slot == 0 ? panel.rowA.get() : panel.rowB.get();
		ui::Knob* amount = slot == 0 ? panel.knobA.get() : panel.knobB.get();
		ui::Knob* mix = slot == 0 ? panel.mixKnobA.get() : panel.mixKnobB.get();
		ui::ModeValueCell* cell = slot == 0 ? panel.cellA.get() : panel.cellB.get();
		REQUIRE(row != nullptr);
		REQUIRE(mix != nullptr);
		CHECK(amount->right() <= mix->x());
		CHECK(mix->right() <= cell->x());
		CHECK(mix->control()->desc().key == std::string_view(slot == 0 ? "eroder_slot_a_mix" : "eroder_slot_b_mix"));
		CHECK(mix->isVisible());
	}
	// THE CARD HAS NO MIX KNOB, and no cell box for it.
	CHECK(card.mixKnobA == nullptr);
	CHECK(card.mixKnobB == nullptr);
}
TEST_CASE("ETCH's slot row writes the slots' own parameters: the route glyph, and each Type selector its own slot's Type") {
	EroFaces e;
	auto& card = *e.d.card;
	e.mode(ero::Mode::Etch);
	ui::EtchSlots& slots = card.etchSlotsForTest();
	ui::RouteSwitch& sw = *slots.route;
	CHECK(eroField(e.d, ero::Param::SlotRoute) == 0.0f);
	CHECK(sw.litChoice() == ui::RouteSwitch::Series);
	const visage::Bounds parallel = sw.glyphBounds(ui::RouteSwitch::Parallel);
	const visage::Bounds series = sw.glyphBounds(ui::RouteSwitch::Series);
	sw.onMouseDown().callback(at(eroCentre(parallel).x, eroCentre(parallel).y));
	sw.onMouseUp().callback(at(eroCentre(parallel).x, eroCentre(parallel).y));
	CHECK(eroField(e.d, ero::Param::SlotRoute) == 1.0f);
	CHECK(sw.litChoice() == ui::RouteSwitch::Parallel);
	sw.onMouseDown().callback(at(eroCentre(series).x, eroCentre(series).y));
	sw.onMouseUp().callback(at(eroCentre(series).x, eroCentre(series).y));
	CHECK(eroField(e.d, ero::Param::SlotRoute) == 0.0f);

	(void)eroField(e.d, ero::Param::SlotAType);
	const float bBefore = eroField(e.d, ero::Param::SlotBType);
	slots.cellA->select().selectZone(2);
	e.refresh();
	CHECK(eroField(e.d, ero::Param::SlotAType) == 2.0f);
	CHECK(eroField(e.d, ero::Param::SlotBType) == bBefore);
	slots.cellB->select().selectZone(4);
	e.refresh();
	CHECK(eroField(e.d, ero::Param::SlotAType) == 2.0f);
	CHECK(eroField(e.d, ero::Param::SlotBType) == 4.0f);
	// AND THE DRIVE'S OWN TYPE IS UNTOUCHED.
	CHECK(eroField(e.d, ero::Param::DriveMode) == static_cast<float>(ero::kParamDescs[eroAt(ero::Param::DriveMode)].def));
}
TEST_CASE("ETCH's slot row goes with the engine: leaving Etch hides it and restores the top row, and returning brings it back") {
	EroFaces e;
	auto& card = *e.d.card;
	ui::EtchSlots& slots = card.etchSlotsForTest();
	for (const ero::Mode away : { ero::Mode::Dust, ero::Mode::Rust, ero::Mode::Comb }) {
		CAPTURE(static_cast<int>(away));
		e.mode(ero::Mode::Etch);
		CHECK(slots.outer->isVisible());
		CHECK(card.freqBoxForTest()->isVisible());
		CHECK_FALSE(card.mixForTest()->isVisible());
		e.mode(away);
		CHECK_FALSE(slots.outer->isVisible());
		CHECK_FALSE(card.freqBoxForTest()->isVisible());
		CHECK_FALSE(card.mixBoxForTest()->isVisible());
		CHECK(card.mixForTest()->isVisible());
		CHECK(card.mixForTest()->y() == slots.outer->y());
		e.mode(ero::Mode::Etch);
		CHECK(slots.outer->isVisible());
		CHECK_FALSE(card.mixForTest()->isVisible());
	}
	// A NOISE SOURCE MEANS NOTHING TO ETCH: Width is the Bell's alone.
	e.set(ero::Param::Source, 1.0f);
	e.set(ero::Param::EmphAmount, 0.0f);
	CHECK_FALSE(e.panel->widthBoxForTest()->enabled());
	e.set(ero::Param::EmphAmount, 9.0f);
	e.set(ero::Param::EmphShape, 1.0f);
	CHECK(e.panel->widthBoxForTest()->enabled());
}
TEST_CASE("ETCH's window offers the slot row and every control Etch uses, nothing it does not, and nothing overlapping") {
	EroFaces e;
	auto& panel = *e.panel;
	e.mode(ero::Mode::Etch);
	const auto visible = [](visage::Frame* f) { return f != nullptr && f->isVisible(); };
	ui::EtchSlots& slots = panel.etchSlotsForTest();
	CHECK(visible(slots.stack.get()));
	// SLOT A OVER SLOT B, THE ROUTE SWITCH TO THE RIGHT OF BOTH, centred on them (the top-down order the switch's glyphs and the
	// topology picture's Parallel drawing show).
	REQUIRE(slots.rowA != nullptr);
	REQUIRE(slots.rowB != nullptr);
	CHECK(slots.rowA->parent() == slots.stack.get());
	CHECK(slots.rowB->parent() == slots.stack.get());
	CHECK(slots.route->parent() == slots.stack.get());
	CHECK(slots.rowA->bottom() <= slots.rowB->y());
	CHECK(slots.rowA->x() == slots.rowB->x());
	CHECK(slots.route->x() >= slots.rowA->right());
	CHECK(slots.route->x() >= slots.rowB->right());
	// EACH ROW IS THE AMOUNT KNOB, THE MIX KNOB AND THE TYPE'S CELL, left to right, with the slot's Mix a knob here and not in the cell.
	for (ui::SlotTriple* row : { slots.rowA.get(), slots.rowB.get() }) {
		CHECK(row->amount().right() <= row->mix().x());
		CHECK(row->mix().right() <= row->cell().x());
		CHECK(row->cell().right() <= static_cast<float>(row->width()) + 0.5f);
	}
	CHECK(slots.route->y() + slots.route->height() * 0.5f == doctest::Approx(slots.stack->height() * 0.5f).epsilon(0.1));
	CHECK(slots.stack->y() < slots.stack->bottom());
	CHECK(visible(panel.freqBoxForTest()));
	CHECK(visible(panel.widthBoxForTest()));
	CHECK(visible(panel.emphasisAmountForTest()));
	CHECK(visible(panel.smearStackForTest()));
	for (const ero::Param p : { ero::Param::Resonance, ero::Param::Follow, ero::Param::Mix, ero::Param::HpWet,
	                            ero::Param::Loop, ero::Param::LoopInterval, ero::Param::Input }) {
		CAPTURE(static_cast<int>(p));
		CHECK(visible(panel.knobForTest(p)));
	}
	for (const ero::Param p : { ero::Param::Amount, ero::Param::Source, ero::Param::Grit, ero::Param::Stereo, ero::Param::Step,
	                            ero::Param::Detune, ero::Param::ModFold, ero::Param::RustBias, ero::Param::RustRectify }) {
		CAPTURE(static_cast<int>(p));
		CHECK_FALSE(visible(panel.knobForTest(p)));
	}
	CHECK(visible(panel.drivePairForTest()));
	CHECK(visible(panel.emphasisStackForTest()));
	CHECK(visible(panel.loopPairForTest()));
	CHECK(visible(panel.shapeForTest()));
	for (visage::Frame* chip : { static_cast<visage::Frame*>(panel.restoreForTest()), static_cast<visage::Frame*>(panel.inLoopForTest()),
	                             static_cast<visage::Frame*>(panel.tapForTest()), static_cast<visage::Frame*>(panel.subThruForTest()) }) {
		CHECK(visible(chip));
	}
	std::vector<visage::Frame*> flat = eroVisible(&panel, { panel.plotForTest(), panel.modeForTest() });
	CHECK(std::find(flat.begin(), flat.end(), static_cast<visage::Frame*>(slots.stack.get())) != flat.end());
	eroApart(flat, panel.width(), panel.height());
	// THE SLOT ROW IS ABSENT FROM THE OTHER ENGINES' WINDOWS.
	e.mode(ero::Mode::Rust);
	CHECK_FALSE(visible(slots.stack.get()));
	CHECK(panel.plotForTest()->showsEtchForTest() == false);
	e.mode(ero::Mode::Etch);
	CHECK(visible(slots.stack.get()));
	CHECK(panel.plotForTest()->showsEtchForTest());
}
TEST_CASE("HARM's window: the harmonic box's popup is placed, centred and hangs off its box (moved from the card)") {
	// THE CARD NO LONGER HAS A HARMONIC ROW (its partials' controls live in the window), so this check stands on the WINDOW'S row,
	// which is the same widget with the same placement rules. The window is opened the way a hand opens it, through the lane.
	using menu_check::checkHangsOff;
	using menu_check::inTop;
	using menu_check::newestPopup;
	using menu_check::rowsOf;
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	auto* overlay = f.gui.expandedForTest();
	REQUIRE(lane != nullptr);
	REQUIRE(overlay != nullptr);
	namespace hrm = feathers::fx::harm;
	const size_t slot = f.bridge.addFxDevice(hrm::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	f.bridge.refresh();
	lane->resized();
	lane->scrollCardIntoView(slot);
	lane->resized();
	(void)f.gui.canvasForTest()->setCollapsed(true);
	lane->onExpandDevice(slot);
	REQUIRE(overlay->open());
	auto* panel = presentedAs<ui::HarmExpanded>(overlay);
	REQUIRE(panel != nullptr);
	ui::HarmonicRow* row = panel->harmonicRow();
	REQUIRE(row != nullptr);
	const DeviceInstanceId id = f.bridge.fxDeviceAt(slot);
	const int count = static_cast<int>(hrm::kHarmonicCount);
	for (const int want : { count / 2, 1, count - 2 }) {
		CAPTURE(want);
		f.deviceHost.setField(id, static_cast<size_t>(hrm::Param::Harmonic), static_cast<float>(want));
		row->showMenu();
		visage::PopupMenuFrame* popup = newestPopup(*row);
		REQUIRE(popup != nullptr);
		visage::PopupList* list = rowsOf(*popup);
		REQUIRE(list != nullptr);
		REQUIRE(list->numOptions() == count);
		const ui::MenuPlacement placed = ui::lastMenuPlacementForTest();
		CHECK(placed.current == want);
		CHECK(placed.scroll == doctest::Approx(list->yPosition()));
		checkMarkedOnly(*list, want);
		checkCentred(*list, want);
		// AND STILL HANGING OFF ITS BOX: the scroll moved the rows inside the list, not the list.
		checkHangsOff(placed, inTop(*row, row->boxBounds()));
		ui::dismissMenuOf(*row);
	}
	row->showMenu();
	checkHangsOff(ui::lastMenuPlacementForTest(), inTop(*row, row->boxBounds()));
	ui::dismissMenuOf(*row);
}

namespace {
//------------------------------------------------------------------------------------------------------------------------------//
// THE ERODER'S WINDOW IN SIGNAL ORDER: five captioned groups, and the topology picture with its switches in it.                  //
//------------------------------------------------------------------------------------------------------------------------------//

struct EroRect {
	float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
};
/// A FRAME'S BOUNDS IN THE WINDOW'S OWN COORDINATES, summed up its parents.
EroRect eroAbsolute(const visage::Frame* f, const visage::Frame* window) {
	EroRect r { 0.0f, 0.0f, static_cast<float>(f->width()), static_cast<float>(f->height()) };
	for (const visage::Frame* p = f; p != window && p != nullptr; p = p->parent()) {
		r.x += static_cast<float>(p->x());
		r.y += static_cast<float>(p->y());
	}
	return r;
}
bool eroInside(const EroRect& inner, const EroRect& outer) {
	const float eps = 0.01f;
	return inner.x >= outer.x - eps && inner.y >= outer.y - eps && inner.x + inner.w <= outer.x + outer.w + eps
	       && inner.y + inner.h <= outer.y + outer.h + eps;
}
} // namespace

TEST_CASE("ERODER's window groups its controls: Input, Tone, Drive over a fixed-height Engine over Feedback and Output, each control inside its own group, in every engine") {
	EroFaces e;
	auto& panel = *e.panel;
	using P = ero::Param;
	const char* const reading[] = { "Input", "Tone", "Drive", "Engine", "Feedback", "Output" };
	float engineHeight = -1.0f;
	MESSAGE("window minimum size " << panel.minimumSize().x << " x " << panel.minimumSize().y);
	for (const ero::Mode mode : { ero::Mode::Dust, ero::Mode::Rust, ero::Mode::Comb, ero::Mode::Etch }) {
		CAPTURE(static_cast<int>(mode));
		e.mode(mode);
		const bool comb = mode == ero::Mode::Comb;
		const bool etch = mode == ero::Mode::Etch;
		const auto& groups = panel.groupsForTest();
		REQUIRE(groups.size() == 6);
		for (size_t g = 0; g < 6; ++g) {
			CHECK(groups[g].caption == reading[g]);
			for (size_t other = 0; other < g; ++other) {
				const bool apart = groups[g].x >= groups[other].x + groups[other].w - 0.01f
				                   || groups[g].y >= groups[other].y + groups[other].h - 0.01f;
				CHECK(apart);
			}
		}
		// THE FIRST ROW IS ONE, THREE AND TWO CELLS; THE ENGINE ALONE BELOW; FEEDBACK BESIDE OUTPUT.
		CHECK(groups[0].y == groups[1].y);
		CHECK(groups[1].y == groups[2].y);
		CHECK(groups[4].y == groups[5].y);
		CHECK(groups[3].y >= groups[0].y + groups[0].h);
		CHECK(groups[4].y >= groups[3].y + groups[3].h);
		CHECK(groups[1].w > groups[2].w);
		CHECK(groups[2].w > groups[0].w);
		// THE ENGINE GROUP IS THE SAME HEIGHT IN EVERY ENGINE, so the window does not jump between them: two full knob cells.
		if (engineHeight < 0.0f) {
			engineHeight = groups[3].h;
		}
		CHECK(groups[3].h == doctest::Approx(engineHeight));
		CHECK(groups[3].h >= ui::EroderExpanded::groupHeight(2.0f * ui::Knob::columnHeight(ui::EroderExpanded::kFace)
		                                                   + ui::EroderExpanded::kRowGap) - 0.01f);
		const auto k = [&](P p) -> visage::Frame* { return panel.knobForTest(p); };
		std::vector<std::vector<visage::Frame*>> want(6);
		want[0] = { k(P::Input) };
		want[1] = { panel.filterForTest(), panel.emphasisStackForTest(), panel.smearStackForTest() };
		want[2] = { panel.drivePairForTest() };
		if (comb) {
			want[3] = { panel.toneForTest(), panel.characterForTest(), k(P::Heat), panel.blendForTest(), panel.gammaForTest() };
		}
		else if (etch) {
			want[3] = { panel.etchSlotsForTest().stack.get() };
		}
		else {
			want[3] = { k(P::Source), k(P::Grit), k(P::Stereo), k(P::Step), k(P::Detune), k(P::ModFold) };
			if (mode == ero::Mode::Dust) {
				want[3].push_back(k(P::Amount));
			}
			else {
				want[3].push_back(k(P::RustBias));
				want[3].push_back(k(P::RustRectify));
			}
		}
		want[4] = { panel.loopPairForTest() };
		if (!comb) {
			want[4].push_back(k(P::Resonance));
		}
		want[5] = { k(P::HpWet), k(P::Follow), k(P::Mix) };
		std::vector<visage::Frame*> listed;
		for (size_t g = 0; g < 6; ++g) {
			const EroRect box { groups[g].x, groups[g].y, groups[g].w, groups[g].h };
			for (visage::Frame* f : want[g]) {
				REQUIRE(f != nullptr);
				CAPTURE(g);
				CAPTURE(typeid(*f).name());
				CHECK(f->isVisible());
				CHECK(eroInside(eroAbsolute(f, &panel), box));
				// AND IN NO OTHER GROUP.
				for (size_t other = 0; other < 6; ++other) {
					if (other != g) {
						const EroRect o { groups[other].x, groups[other].y, groups[other].w, groups[other].h };
						CHECK_FALSE(eroInside(eroAbsolute(f, &panel), o));
					}
				}
				listed.push_back(f);
			}
		}
		// EVERY VISIBLE CHILD OF THE WINDOW IS IN THE TABLE, but the plot, the engine strip, the picture, and Track.
		const auto shown = eroVisible(&panel, { panel.plotForTest(), panel.subplotForTest(), panel.modeForTest(), panel.flowForTest(), panel.trackForTest() });
		CHECK(eroSameSet(shown, listed));
		// THE PICTURE IS BELOW THEM ALL, and the window is tall enough for it, with Track under the picture and outside its frame.
		CHECK(panel.flowForTest()->y() >= groups[5].y + groups[5].h);
		CHECK(panel.flowForTest()->y() - (groups[5].y + groups[5].h) < 12.0f);
		CHECK(panel.trackForTest()->isVisible());
		CHECK(panel.trackForTest()->y() >= panel.flowForTest()->y() + panel.flowForTest()->height());
		CHECK(panel.trackForTest()->y() + panel.trackForTest()->height() <= panel.height());
		const EroRect trackBox = eroAbsolute(panel.trackForTest(), &panel);
		CHECK_FALSE(eroInside(trackBox, { groups[3].x, groups[3].y, groups[3].w, groups[3].h }));
	}
	// TRACK IS ONE FIXED SPOT: it does not move when the engine does.
	e.mode(ero::Mode::Dust);
	const float trackX = panel.trackForTest()->x();
	const float trackY = panel.trackForTest()->y();
	for (const ero::Mode mode : { ero::Mode::Rust, ero::Mode::Etch, ero::Mode::Dust }) {
		e.mode(mode);
		CHECK(panel.trackForTest()->x() == trackX);
		CHECK(panel.trackForTest()->y() == trackY);
	}
}
TEST_CASE("ERODER's topology picture: the path in order with In Loop off and on, what is lit, where the returns leave, the Sub Thru road, Etch's slots") {
	EroFaces e;
	auto& panel = *e.panel;
	using P = ero::Param;
	ui::EroderFlow& flow = *panel.flowForTest();
	const auto labels = [&] { return flow.labelsForTest(); };
	const std::string sum = "\xE2\x8A\x95";
	const auto indexOf = [&](const std::string& label) {
		const auto l = labels();
		return static_cast<size_t>(std::find(l.begin(), l.end(), label) - l.begin());
	};
	const auto lit = [&](const std::string& label) { return flow.pathForTest().stages[indexOf(label)].lit; };
	e.mode(ero::Mode::Dust);
	// THE ORDER: the emphasis before the sum, and after it with In Loop.
	CHECK(labels() == std::vector<std::string>({ "in", "gain", "emph", sum, "smear", "dust", "drive", "dc", "restore", "hp", "mix" }));
	e.set(P::EmphInLoop, 1.0f);
	CHECK(labels() == std::vector<std::string>({ "in", "gain", sum, "emph", "smear", "dust", "drive", "dc", "restore", "hp", "mix" }));
	e.set(P::EmphInLoop, 0.0f);
	// WHAT IS LIT: the emphasis and its undoing at 0 dB are dim; a Smear, a Drive and an HP Wet are dim at their bottoms.
	CHECK_FALSE(lit("emph"));
	CHECK_FALSE(lit("restore"));
	CHECK_FALSE(lit("smear"));
	CHECK_FALSE(lit("drive"));
	CHECK_FALSE(lit("hp"));
	CHECK(lit("in"));
	CHECK(lit("dust"));
	CHECK(lit("mix"));
	e.set(P::EmphRestore, 0.0f);
	e.set(P::EmphAmount, 6.0f);
	CHECK(lit("emph"));
	CHECK_FALSE(lit("restore")); // the emphasis is not undone while Restore is off
	e.set(P::EmphRestore, 1.0f);
	CHECK(lit("restore"));
	e.set(P::Smear, 0.5f);
	e.set(P::Drive, 0.5f);
	e.set(P::HpWet, 8.0f);
	CHECK(lit("smear"));
	CHECK(lit("drive"));
	CHECK(lit("hp"));
	// THE RETURNS: both dim at rest; the inner leaves the engine (Clean) or the Drive (Driven); the outer the Drive.
	const auto& returns = flow.pathForTest().returns;
	REQUIRE(returns.size() == 2);
	CHECK_FALSE(returns[0].lit);
	CHECK_FALSE(returns[1].lit);
	CHECK(returns[0].to == indexOf(sum));
	CHECK(returns[1].to == indexOf(sum));
	e.set(P::Resonance, 0.4f);
	e.set(P::Loop, 0.4f);
	CHECK(flow.pathForTest().returns[0].lit);
	CHECK(flow.pathForTest().returns[1].lit);
	CHECK(lit(sum));
	CHECK(flow.pathForTest().returns[0].from == indexOf("dust")); // Tap Clean
	CHECK(flow.pathForTest().returns[1].from == indexOf("drive"));
	e.set(P::FbTap, 1.0f);
	CHECK(flow.pathForTest().returns[0].from == indexOf("drive")); // Tap Driven
	CHECK(flow.pathForTest().returns[1].from == indexOf("drive"));
	e.set(P::Loop, 0.0f);
	CHECK_FALSE(flow.pathForTest().returns[1].lit);
	CHECK(flow.pathForTest().returns[0].lit);
	// THE SUB THRU ROAD runs from the input to HP Wet, and is lit only while it gives something back.
	CHECK_FALSE(flow.pathForTest().bypass.lit);
	CHECK(flow.pathForTest().bypass.from == indexOf("in"));
	CHECK(flow.pathForTest().bypass.to == indexOf("hp"));
	e.set(P::SubThru, 1.0f);
	CHECK(flow.pathForTest().bypass.lit);
	e.set(P::HpWet, static_cast<float>(ero::kHpWetLog2Min));
	CHECK_FALSE(flow.pathForTest().bypass.lit);
	e.set(P::HpWet, 8.0f);
	// IN THE COMB it needs Blend, the window's own rule for the chip.
	e.mode(ero::Mode::Comb);
	CHECK(labels()[labels().size() - 6] == "comb"); // (the dc stage follows the drive)
	CHECK_FALSE(flow.pathForTest().bypass.lit);
	CHECK_FALSE(panel.subThruForTest()->enabled());
	e.set(P::Blend, 1.0f);
	CHECK(flow.pathForTest().bypass.lit);
	CHECK(panel.subThruForTest()->enabled());
	// ETCH: its two slots, side by side in Series and one over the other in Parallel.
	e.set(P::FbTap, 0.0f);
	e.mode(ero::Mode::Etch);
	CHECK(labels() == std::vector<std::string>({ "in", "gain", "emph", sum, "smear", "A", "B", "drive", "dc", "restore", "hp", "mix" }));
	CHECK_FALSE(flow.pathForTest().stages[indexOf("B")].beneath);
	CHECK(flow.stageXForTest(indexOf("B")) > flow.stageXForTest(indexOf("A")));
	CHECK_FALSE(lit("A"));
	e.set(P::SlotAAmount, 0.5f);
	CHECK(lit("A"));
	e.set(P::SlotRoute, 1.0f);
	CHECK(flow.pathForTest().stages[indexOf("B")].beneath);
	CHECK(flow.stageXForTest(indexOf("B")) == flow.stageXForTest(indexOf("A")));
	CHECK(flow.pathForTest().returns[0].from == indexOf("B"));

	// THE FOUR SWITCHES ARE IN THE PICTURE'S FRAME, and a press goes through the frame to them.
	CHECK(flow.ignoresMouseEvents());
	for (ui::SwitchChip* chip : { panel.inLoopForTest(), panel.tapForTest(), panel.restoreForTest(), panel.subThruForTest() }) {
		REQUIRE(chip != nullptr);
		CHECK(chip->parent() == &flow);
		CHECK(chip->isVisible());
		CHECK(eroInside(eroAbsolute(chip, &flow), { 0.0f, 0.0f, static_cast<float>(flow.width()), static_cast<float>(flow.height()) }));
	}
	// EACH SWITCH SITS BESIDE WHAT IT CHANGES, and nothing overlaps: not a stage's box, not another switch. Over every engine, the
	// Etch's two routes, both taps and both places for the emphasis.
	{
		using Chip = ui::EroderFlow::Chip;
		const auto overlap = [](const ui::EroderFlow::Rect& a, const ui::EroderFlow::Rect& b) {
			return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
		};
		const std::pair<Chip, ui::SwitchChip*> chips[] = { { Chip::InLoop, panel.inLoopForTest() }, { Chip::Tap, panel.tapForTest() },
			                                               { Chip::Restore, panel.restoreForTest() }, { Chip::SubThru, panel.subThruForTest() } };
		for (const ero::Mode mode : { ero::Mode::Dust, ero::Mode::Rust, ero::Mode::Comb, ero::Mode::Etch }) {
			for (const int variant : { 0, 1, 2, 3 }) {
				CAPTURE(static_cast<int>(mode));
				CAPTURE(variant);
				e.mode(mode);
				e.set(P::SlotRoute, (variant & 1) != 0 ? 1.0f : 0.0f);
				e.set(P::FbTap, (variant & 2) != 0 ? 1.0f : 0.0f);
				e.set(P::EmphInLoop, ((variant + 1) & 2) != 0 ? 1.0f : 0.0f);
				const float fw = static_cast<float>(flow.width());
				const float fh = static_cast<float>(flow.height());
				for (const auto& [chip, frame] : chips) {
					const ui::EroderFlow::Rect r = flow.chipRectForTest(chip);
					CHECK(frame->x() == doctest::Approx(r.x));
					CHECK(frame->y() == doctest::Approx(r.y));
					CHECK(r.x >= 0.0f);
					CHECK(r.y >= 0.0f);
					CHECK(r.x + r.w <= fw);
					CHECK(r.y + r.h <= fh);
					for (size_t i = 0; i < flow.pathForTest().stages.size(); ++i) {
						CAPTURE(flow.pathForTest().stages[i].label);
						CHECK_FALSE(overlap(r, flow.boxForTest(i)));
					}
					for (const auto& [other, otherFrame] : chips) {
						if (other != chip) {
							CHECK_FALSE(overlap(r, flow.chipRectForTest(other)));
						}
					}
					const ui::EroderFlow::Anchor a = flow.anchorForTest(chip);
					CHECK(std::hypot(r.x + r.w * 0.5f - a.x, r.y + r.h * 0.5f - a.y) <= ui::EroderFlow::kChipReach);
					// THE ANCHOR'S STAGE IS A REAL ONE, and the right kind.
					REQUIRE(a.stage < flow.pathForTest().stages.size());
				}
				const auto& stages = flow.pathForTest().stages;
				CHECK(stages[flow.anchorForTest(Chip::Restore).stage].label == "restore");
				CHECK(stages[flow.anchorForTest(Chip::InLoop).stage].label == "emph");
				CHECK(stages[flow.anchorForTest(Chip::SubThru).stage].label == "in");
				CHECK(flow.anchorForTest(Chip::Tap).stage == flow.pathForTest().returns[0].from);
			}
		}
		e.mode(ero::Mode::Etch);
		e.set(P::SlotRoute, 0.0f);
		e.set(P::FbTap, 0.0f);
		e.set(P::EmphInLoop, 0.0f);
	}
	CHECK(eroField(e.d, P::EmphInLoop) == 0.0f);
	panel.inLoopForTest()->onMouseDown().callback(at(4.0f, 4.0f));
	e.refresh();
	CHECK(eroField(e.d, P::EmphInLoop) == 1.0f);
	// AND THE PICTURE FOLLOWED: it repainted from the new row.
	CHECK(flow.labelsForTest()[2] == sum);
}
TEST_CASE("ERODER's topology picture draws opaque boxes: no line enters a stage's box, and a road under a chip is covered by the chip's backing") {
	EroFaces e;
	auto& panel = *e.panel;
	using P = ero::Param;
	ui::EroderFlow& flow = *panel.flowForTest();
	flow.setRecordingForTest(true);
	const std::string sum = "\xE2\x8A\x95";
	// THE INPUT DRIVE'S STAGE: right after the input, dim at 0 dB and lit otherwise; the Sub Thru road leaves the input BEFORE it.
	e.mode(ero::Mode::Dust);
	REQUIRE(flow.labelsForTest().size() > 2);
	CHECK(flow.labelsForTest()[0] == "in");
	CHECK(flow.labelsForTest()[1] == "gain");
	CHECK_FALSE(flow.pathForTest().stages[1].lit);
	e.set(P::Input, 6.0f);
	CHECK(flow.pathForTest().stages[1].lit);
	CHECK(flow.pathForTest().bypass.from == 0);
	CHECK(flow.boxForTest(0).x + flow.boxForTest(0).w <= flow.boxForTest(1).x);
	e.set(P::Input, 0.0f);

	size_t drawn = 0;
	for (const ero::Mode mode : { ero::Mode::Dust, ero::Mode::Rust, ero::Mode::Comb, ero::Mode::Etch }) {
		for (const int variant : { 0, 1, 2, 3 }) {
			CAPTURE(static_cast<int>(mode));
			CAPTURE(variant);
			e.mode(mode);
			e.set(P::SlotRoute, (variant & 1) != 0 ? 1.0f : 0.0f);
			e.set(P::FbTap, (variant & 2) != 0 ? 1.0f : 0.0f);
			e.set(P::EmphInLoop, ((variant + 1) & 2) != 0 ? 1.0f : 0.0f);
			e.set(P::Resonance, 0.4f);
			e.set(P::Loop, 0.4f);
			e.set(P::EmphAmount, 6.0f);
			e.set(P::HpWet, 8.0f);
			e.set(P::SubThru, 1.0f);
			e.set(P::Blend, 1.0f);
			visage::Canvas canvas;
			flow.draw(canvas);
			const auto& events = flow.drawnForTest();
			REQUIRE_FALSE(events.empty());
			for (size_t i = 0; i < flow.pathForTest().stages.size(); ++i) {
				const ui::EroderFlow::Rect b = flow.boxForTest(i);
				const bool circle = flow.pathForTest().stages[i].label == sum;
				const float cx = b.x + b.w * 0.5f;
				const float cy = b.y + b.h * 0.5f;
				for (const auto& ev : events) {
					if (ev.backing) {
						continue;
					}
					// A SEGMENT'S INTERIOR AGAINST THE BOX'S, sampled along it, the box shrunk by a pixel and a half.
					for (int t = 1; t < 20; ++t) {
						const float u = static_cast<float>(t) / 20.0f;
						const float x = ev.x0 + (ev.x1 - ev.x0) * u;
						const float y = ev.y0 + (ev.y1 - ev.y0) * u;
						const bool inside = circle ? std::hypot(x - cx, y - cy) < b.w * 0.5f - 1.5f
						                           : x > b.x + 1.5f && x < b.x + b.w - 1.5f && y > b.y + 1.5f && y < b.y + b.h - 1.5f;
						if (inside && !circle) {
							CAPTURE(flow.pathForTest().stages[i].label);
							CHECK_FALSE(inside);
						}
					}
				}
			}
			// A ROAD UNDER A CHIP IS COVERED: every chip's backing was put down after every segment that touches its plate.
			for (const ui::EroderFlow::Chip chip : { ui::EroderFlow::Chip::InLoop, ui::EroderFlow::Chip::Tap,
			                                        ui::EroderFlow::Chip::Restore, ui::EroderFlow::Chip::SubThru }) {
				const ui::EroderFlow::Rect r = flow.chipRectForTest(chip);
				size_t backing = events.size();
				for (size_t i = 0; i < events.size(); ++i) {
					if (events[i].backing && std::abs(events[i].x0 - r.x) < 0.01f && std::abs(events[i].y0 - r.y) < 0.01f) {
						backing = i;
					}
				}
				REQUIRE(backing < events.size());
				for (size_t i = 0; i < events.size(); ++i) {
					const auto& ev = events[i];
					if (ev.backing) {
						continue;
					}
					const bool touches = std::max(ev.x0, ev.x1) > r.x && std::min(ev.x0, ev.x1) < r.x + r.w
					                     && std::max(ev.y0, ev.y1) > r.y && std::min(ev.y0, ev.y1) < r.y + r.h;
					if (touches) {
						CHECK(i < backing);
					}
				}
			}
			drawn += events.size();
		}
	}
	CHECK(drawn > 400);
	flow.setRecordingForTest(false);
}
TEST_CASE("ERODER's Tone group: the Filter box holds Freq then Width, the Emphasis box Amount, Offset and Tilt/Bell, and no knob stands for them") {
	EroFaces e;
	using P = ero::Param;
	auto& panel = *e.panel;
	e.mode(ero::Mode::Dust);
	// NO KNOB FOR THE THREE THE FIELDS REPLACED, in any engine.
	for (const ero::Mode mode : { ero::Mode::Dust, ero::Mode::Rust, ero::Mode::Comb, ero::Mode::Etch }) {
		e.mode(mode);
		std::vector<ui::Knob*> knobs;
		collectKnobs(&panel, &knobs);
		for (const ui::Knob* knob : knobs) {
			const std::string_view key = knob->control()->desc().key;
			CHECK(key != std::string_view("eroder_freq"));
			CHECK(key != std::string_view("eroder_width"));
			CHECK(key != std::string_view("eroder_emph_amount"));
			CHECK(key != std::string_view("eroder_cutoff")); // the Comb's Filter field now
		}
		CHECK(panel.knobForTest(P::Freq) == nullptr);
		CHECK(panel.knobForTest(P::Width) == nullptr);
		CHECK(panel.knobForTest(P::EmphAmount) == nullptr);
		CHECK(panel.knobForTest(P::Cutoff) == nullptr);
	}
	e.mode(ero::Mode::Dust);
	const auto absolute = [&](const visage::Frame* f) { return eroAbsolute(f, &panel); };
	const auto& groups = panel.groupsForTest();
	const EroRect tone { groups[1].x, groups[1].y, groups[1].w, groups[1].h };
	REQUIRE(groups[1].caption == "Tone");
	for (visage::Frame* f : std::initializer_list<visage::Frame*> { panel.freqBoxForTest(), panel.widthBoxForTest(),
	                                                              panel.emphasisAmountForTest(), panel.emphasisOffsetForTest(),
	                                                              panel.shapeForTest() }) {
		CHECK(f->isVisible());
		CHECK(eroInside(absolute(f), tone));
	}
	// THE STACKS' ORDER, top to bottom, and each says which control it is.
	CHECK(std::string(panel.freqBoxForTest()->control().desc().key) == "eroder_freq");
	CHECK(std::string(panel.widthBoxForTest()->control().desc().key) == "eroder_width");
	CHECK(panel.freqBoxForTest()->y() < panel.widthBoxForTest()->y());
	CHECK(panel.filterForTest()->caption() == "Filter");
	CHECK(panel.emphasisStackForTest()->caption() == "Emphasis");
	CHECK(panel.emphasisAmountForTest()->y() < panel.emphasisOffsetForTest()->y());
	CHECK(panel.emphasisOffsetForTest()->y() < panel.shapeForTest()->y());
	CHECK(absolute(panel.filterForTest()).x < absolute(panel.emphasisStackForTest()).x);
	CHECK(absolute(panel.emphasisStackForTest()).x < absolute(panel.smearStackForTest()).x);
	CHECK(panel.smearStackForTest()->caption() == "Smear");

	// EACH FIELD'S OWN DRAG WRITES ITS OWN PARAMETER, by the amount its mapping says: the box's own control gives the value a
	// pixel-for-pixel restatement is compared with (the box's drag is the control's `setValue` of the dragged position).
	const auto dragUp = [&](ui::ValueBox* box, float pixels) {
		const float cx = static_cast<float>(box->width()) * 0.5f;
		const float cy = static_cast<float>(box->height()) * 0.5f;
		box->onMouseDown().callback(at(cx, cy));
		box->onMouseDrag().callback(at(cx, cy - pixels));
		box->onMouseUp().callback(at(cx, cy - pixels));
		e.refresh();
	};
	e.set(P::EmphAmount, 3.0f);
	e.set(P::Source, 1.0f);
	struct Field {
		ui::ValueBox* box;
		P param;
	};
	for (const Field& f : { Field { panel.freqBoxForTest(), P::Freq }, Field { panel.widthBoxForTest(), P::Width },
	                        Field { panel.emphasisAmountForTest(), P::EmphAmount }, Field { panel.emphasisOffsetForTest(), P::EmphOffset } }) {
		CAPTURE(ero::kParamDescs[eroAt(f.param)].key);
		REQUIRE(f.box->enabled());
		const float before = eroField(e.d, f.param);
		dragUp(f.box, 30.0f);
		CHECK(eroField(e.d, f.param) > before);
		const float after = eroField(e.d, f.param);
		// A DRAG DOWN BY THE SAME DISTANCE BRINGS IT BACK, within the mapping's rounding.
		dragUp(f.box, -30.0f);
		CHECK(eroField(e.d, f.param) < after);
	}
	// THE SELECTOR WRITES THE SHAPE.
	e.set(P::EmphShape, 0.0f);
	panel.shapeForTest()->onMouseDown().callback(at(panel.shapeForTest()->width() - 2.0f, panel.shapeForTest()->height() * 0.5f));
	e.refresh();
	CHECK(eroField(e.d, P::EmphShape) == 1.0f);

	// HONESTY, as the knobs had it: Width greys with no noise band and no Bell; the Comb has no Freq to show; Etch's Width is the Bell's.
	e.mode(ero::Mode::Dust);
	e.set(P::EmphAmount, 0.0f);
	e.set(P::Source, 0.0f);
	CHECK_FALSE(panel.widthBoxForTest()->enabled());
	e.set(P::Source, 0.5f);
	CHECK(panel.widthBoxForTest()->enabled());
	e.mode(ero::Mode::Comb);
	CHECK_FALSE(panel.freqBoxForTest()->isVisible());
	CHECK(panel.widthBoxForTest()->isVisible());
	CHECK_FALSE(panel.widthBoxForTest()->enabled());
	// THE COMB'S FILTER BOX: Cutoff takes Freq's top row, over Width.
	CHECK(panel.cutoffBoxForTest()->isVisible());
	CHECK(std::string(panel.cutoffBoxForTest()->control().desc().key) == "eroder_cutoff");
	CHECK(panel.cutoffBoxForTest()->enabled());
	CHECK(panel.cutoffBoxForTest()->y() == panel.emphasisAmountForTest()->y());
	CHECK(panel.widthBoxForTest()->y() > panel.cutoffBoxForTest()->y());
	{
		// THE FIELD WRITES CUTOFF AND NOTHING ELSE.
		const float cutoffBefore = eroField(e.d, P::Cutoff);
		const float widthBefore = eroField(e.d, P::Width);
		dragUp(panel.cutoffBoxForTest(), 30.0f);
		CHECK(eroField(e.d, P::Cutoff) > cutoffBefore);
		CHECK(eroField(e.d, P::Width) == widthBefore);
		dragUp(panel.cutoffBoxForTest(), -30.0f);
		CHECK(std::string(panel.cutoffBoxForTest()->tooltipAt({ 2.0f, 2.0f })) == ero::kParamDescs[eroAt(P::Cutoff)].tooltip);
	}
	// AND IN THE OTHER ENGINES IT IS NOT DRAWN.
	e.mode(ero::Mode::Dust);
	CHECK_FALSE(panel.cutoffBoxForTest()->isVisible());
	e.mode(ero::Mode::Comb);
	e.mode(ero::Mode::Etch);
	CHECK(panel.freqBoxForTest()->isVisible());
	CHECK_FALSE(panel.widthBoxForTest()->enabled());
	e.set(P::EmphAmount, 6.0f);
	e.set(P::EmphShape, 1.0f);
	CHECK(panel.widthBoxForTest()->enabled());
	// THE OFFSET AND THE SELECTOR GREY AT AMOUNT 0 (nothing to place), the Amount never.
	e.set(P::EmphAmount, 0.0f);
	CHECK(panel.emphasisAmountForTest()->enabled());
	CHECK_FALSE(panel.emphasisOffsetForTest()->enabled());
}
TEST_CASE("ERODER's Smear box: Amount, Stages and Reso, three rows inside the plate; the last two grey at Smear 0; the plot's handle writes the offset") {
	EroFaces e;
	using P = ero::Param;
	auto& panel = *e.panel;
	e.mode(ero::Mode::Dust);
	ui::FieldStack* stack = panel.smearStackForTest();
	REQUIRE(stack != nullptr);
	REQUIRE(stack->boxCount() == 3);
	const char* keys[] = { "eroder_smear", "eroder_smear_stages", "eroder_smear_reso" };
	for (size_t i = 0; i < 3; ++i) {
		CHECK(std::string(stack->box(i).control().desc().key) == keys[i]);
		CHECK(stack->box(i).isVisible());
	}
	ui::ValueBox& amount = stack->box(0);
	ui::ValueBox& stages = stack->box(1);
	ui::ValueBox& reso = stack->box(2);
	CHECK(amount.y() < stages.y());
	CHECK(stages.y() < reso.y());
	const float captionTop = ui::ZoneCoordCell::columnRowsFor(ui::EroderExpanded::kFace).caption;
	for (size_t i = 0; i < 3; ++i) {
		const ui::ValueBox& b = stack->box(i);
		CAPTURE(i);
		CHECK(b.x() >= 0.0f);
		CHECK(b.x() + b.width() <= stack->width());
		CHECK(b.y() + b.height() <= captionTop);
	}
	CHECK(stack->height() == panel.emphasisStackForTest()->height());
	CHECK(stack->caption() == "Smear");
	// HONESTY: Stages and Reso are nothing while Smear is at 0 (Amount never is).
	e.set(P::Smear, 0.0f);
	CHECK(amount.enabled());
	CHECK_FALSE(stages.enabled());
	CHECK_FALSE(reso.enabled());
	e.set(P::Smear, 0.3f);
	CHECK(stages.enabled());
	CHECK(reso.enabled());
	const auto dragUp = [&](ui::ValueBox& box, float pixels) {
		const float cx = static_cast<float>(box.width()) * 0.5f;
		const float cy = static_cast<float>(box.height()) * 0.5f;
		box.onMouseDown().callback(at(cx, cy));
		box.onMouseDrag().callback(at(cx, cy - pixels));
		box.onMouseUp().callback(at(cx, cy - pixels));
		e.refresh();
	};
	for (const P p : { P::SmearStages, P::SmearReso }) {
		ui::ValueBox& box = p == P::SmearStages ? stages : reso;
		CAPTURE(ero::kParamDescs[eroAt(p)].key);
		const float before = eroField(e.d, p);
		dragUp(box, 30.0f);
		CHECK(eroField(e.d, p) > before);
	}
	e.set(P::SmearStages, 8.0f);
	e.set(P::SmearReso, 8.0f);
	e.set(P::SmearOffset, 0.0f);

	// ---- THE PLOT'S SMEAR HANDLE ----
	ui::EroderPlot& plot = *panel.plotForTest();
	const auto press = [&](float x, float y) { plot.onMouseDown().callback(at(x, y)); };
	const auto drag = [&](float x, float y) { plot.onMouseDrag().callback(at(x, y)); };
	const auto release = [&](float x, float y) { plot.onMouseUp().callback(at(x, y)); };
	plot.update();
	// ON ITS OWN LINE NEAR THE TOP, and apart from the emphasis's handle.
	CHECK(plot.smearHandleYForTest() < plot.emphasisHandleYForTest());
	CHECK(plot.smearHzForTest() == doctest::Approx(plot.bandForTest().freqHz));
	// WHILE SMEAR IS AT 0 IT TAKES NO PRESS (greyed), and its tooltip says why.
	e.set(P::Smear, 0.0f);
	plot.update();
	CHECK_FALSE(plot.smearLiveForTest());
	{
		const float hx = plot.smearHandleXForTest();
		const float hy = plot.smearHandleYForTest();
		CHECK(std::string(plot.tooltipAt({ hx, hy })) == ero::kParamDescs[eroAt(P::SmearOffset)].tooltip);
		CHECK(std::string(plot.tooltipAt({ hx, hy })).find("Inactive while Smear is at 0") != std::string::npos);
		press(hx, hy);
		drag(hx + 60.0f, hy);
		release(hx + 60.0f, hy);
		e.refresh();
		CHECK(eroField(e.d, P::SmearOffset) == 0.0f);
	}
	// LIVE: a press with no movement writes nothing; a drag across writes the offset the plot's own pitch axis predicts.
	e.set(P::Smear, 0.5f);
	plot.update();
	CHECK(plot.smearLiveForTest());
	{
		const float hx = plot.smearHandleXForTest();
		const float hy = plot.smearHandleYForTest();
		press(hx, hy);
		release(hx, hy);
		e.refresh();
		CHECK(eroField(e.d, P::SmearOffset) == 0.0f);
		const float dx = 60.0f;
		press(hx, hy);
		drag(hx + dx, hy);
		release(hx + dx, hy);
		e.refresh();
		const double expected = 12.0 * std::log2(plot.xToHzForTest(hx + dx - ui::EroderPlot::kAmountDeadZone) / plot.xToHzForTest(hx));
		CAPTURE(expected);
		CHECK(expected > 1.0);
		CHECK(eroField(e.d, P::SmearOffset) == doctest::Approx(static_cast<float>(expected)).epsilon(1e-3));
		// ACROSS ONLY: the emphasis's offset and the Smear's amount are untouched.
		CHECK(eroField(e.d, P::EmphOffset) == 0.0f);
		CHECK(eroField(e.d, P::Smear) == 0.5f);
		// THE HANDLE FOLLOWS: centred at Freq times two to the offset over twelve.
		plot.update();
		const double want = plot.bandForTest().freqHz * std::exp2(static_cast<double>(eroField(e.d, P::SmearOffset)) / 12.0);
		CHECK(plot.smearHzForTest() == doctest::Approx(want).epsilon(1e-6));
		CHECK(plot.smearHandleXForTest() == doctest::Approx(plot.hzToXForTest(want)));
		CHECK(plot.smearHandleXForTest() > hx);
	}
	// THE PLOT DECLARES THE HANDLE'S CONTROL FOR THE GUARDS, and the window's tree reaches it.
	std::vector<ui::ControlFace::Offered> offered;
	plot.offeredControls(offered);
	CHECK(std::any_of(offered.begin(), offered.end(), [&](const ui::ControlFace::Offered& o) {
		return o.control != nullptr && std::string(o.control->desc().key) == "eroder_smear_offset";
	}));
	const auto addressed = panel.addressedParamsForTest();
	for (const P p : { P::Smear, P::SmearStages, P::SmearReso, P::SmearOffset }) {
		CAPTURE(static_cast<int>(p));
		CHECK(std::find(addressed.begin(), addressed.end(), p) != addressed.end());
	}
}
TEST_CASE("ERODER's Engine group is the same height in every engine, Etch's slot A sits over slot B with the route switch to the right of both, and Input is a knob of its own") {
	EroFaces e;
	using P = ero::Param;
	auto& panel = *e.panel;
	float height = -1.0f;
	float y = -1.0f;
	for (const ero::Mode mode : { ero::Mode::Dust, ero::Mode::Rust, ero::Mode::Comb, ero::Mode::Etch }) {
		e.mode(mode);
		const auto& g = panel.groupsForTest()[3];
		REQUIRE(g.caption == "Engine");
		if (height < 0.0f) {
			height = g.h;
			y = g.y;
		}
		CHECK(g.h == doctest::Approx(height));
		CHECK(g.y == doctest::Approx(y)); // and the groups under it do not move either
		CHECK(panel.groupsForTest()[4].y == doctest::Approx(g.y + g.h + ui::EroderExpanded::kGroupGap));
	}
	e.mode(ero::Mode::Etch);
	ui::EtchSlots& slots = panel.etchSlotsForTest();
	const EroRect a = eroAbsolute(slots.rowA.get(), &panel);
	const EroRect b = eroAbsolute(slots.rowB.get(), &panel);
	const EroRect r = eroAbsolute(slots.route.get(), &panel);
	CHECK(a.y + a.h <= b.y);
	CHECK(a.x == doctest::Approx(b.x));
	CHECK(r.x >= a.x + a.w);
	CHECK(r.x >= b.x + b.w);
	const auto& engine = panel.groupsForTest()[3];
	for (const EroRect& box : { a, b, r }) {
		CHECK(eroInside(box, { engine.x, engine.y, engine.w, engine.h }));
	}
	CHECK(slots.knobA->parent() == slots.rowA.get());
	CHECK(slots.knobB->parent() == slots.rowB.get());
	// A WRITES ITS PARAMETER FROM THE TOP ROW AND B FROM THE ONE BELOW.
	CHECK(std::string(slots.knobA->control()->desc().key) == "eroder_slot_a_amount");
	CHECK(std::string(slots.knobB->control()->desc().key) == "eroder_slot_b_amount");
	// INPUT: a knob of its own, in its own group, on the control it names.
	ui::Knob* input = panel.inputKnobForTest();
	REQUIRE(input != nullptr);
	CHECK(std::string(input->control()->desc().key) == "eroder_input");
	CHECK(input->isVisible());
	input->control()->beginGesture();
	input->control()->setValue(9.0);
	input->control()->endGesture();
	e.refresh();
	CHECK(eroField(e.d, P::Input) == doctest::Approx(9.0f));
	CHECK(std::string(input->tooltipAt({ 2.0f, 2.0f })) == ero::kParamDescs[eroAt(P::Input)].tooltip);
}
TEST_CASE("ERODER's picture is one split in every engine: the spectrum over a sub-plot of two panels, neither moving when the engine does") {
	EroFaces e;
	auto& panel = *e.panel;
	ui::EroderPlot& plot = *panel.plotForTest();
	ui::EroderSubplot& sub = *panel.subplotForTest();
	struct Box {
		float x, y, w, h;
	};
	const auto boxOf = [](const visage::Frame& f) { return Box { static_cast<float>(f.x()), static_cast<float>(f.y()), static_cast<float>(f.width()), static_cast<float>(f.height()) }; };
	Box plotBox {};
	Box subBox {};
	ui::EroderSubplot::Rect left {};
	ui::EroderSubplot::Rect right {};
	bool first = true;
	for (const ero::Mode mode : { ero::Mode::Dust, ero::Mode::Comb, ero::Mode::Rust, ero::Mode::Etch }) {
		CAPTURE(static_cast<int>(mode));
		e.mode(mode);
		const Box p = boxOf(plot);
		const Box b = boxOf(sub);
		CHECK(sub.isVisible());
		CHECK(plot.isVisible());
		// THE PLOT ABOVE THE SUB-PLOT, the split the window's own share says, and neither wider than the picture's column.
		CHECK(p.y + p.h <= b.y);
		CHECK(p.x == b.x);
		CHECK(p.w == b.w);
		const float total = p.h + ui::EroderExpanded::kPlotGap + b.h;
		CHECK(std::abs(p.h - (total - ui::EroderExpanded::kPlotGap) * ui::EroderExpanded::kPlotShare) <= 1.0f);
		CHECK(b.h / total > 0.40f);
		CHECK(b.h / total < 0.50f);
		if (first) {
			plotBox = p;
			subBox = b;
			left = sub.leftPanelForTest();
			right = sub.rightPanelForTest();
			first = false;
		}
		// IDENTICAL ACROSS THE FOUR ENGINES: the plot's bounds, the sub-plot's.
		CHECK(p.x == plotBox.x);
		CHECK(p.y == plotBox.y);
		CHECK(p.w == plotBox.w);
		CHECK(p.h == plotBox.h);
		CHECK(b.x == subBox.x);
		CHECK(b.y == subBox.y);
		CHECK(b.w == subBox.w);
		CHECK(b.h == subBox.h);
		// THE TWO PANELS, equal and side by side — except the Comb's bars, which take the whole width under the strip.
		const auto l = sub.leftPanelForTest();
		const auto r = sub.rightPanelForTest();
		if (mode == ero::Mode::Comb) {
			CHECK(l.w == doctest::Approx(b.w - 2.0f * ui::EroderSubplot::kInset));
			CHECK(r.y + r.h <= l.y);
			CHECK(sub.barsForTest().w == l.w);
			CHECK(sub.barsForTest().h <= 0.45f * (total));
			CHECK(plot.y() + plot.height() <= sub.y() + sub.barsForTest().y);
		}
		else {
			CHECK(l.w == doctest::Approx(r.w));
			CHECK(l.x + l.w < r.x);
			CHECK(l.y == r.y);
			CHECK(l.h == r.h);
			CHECK(l.x == doctest::Approx(left.x));
			CHECK(l.w == doctest::Approx(left.w));
			CHECK(r.x == doctest::Approx(right.x));
		}
		CHECK(l.x >= 0.0f);
		CHECK(r.x + r.w <= b.w);
		CHECK(r.y + r.h <= b.h);
		CHECK(l.y + l.h <= b.h);
	}
}
TEST_CASE("ERODER's feedback meters read the limiters' readings: the drawn fraction is the restated mapping, dim while the loop is off, and nothing repaints at rest") {
	DynCardFixture<ui::EroderCard> d(ero::kTraits.tag);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, ero::kDescriptor);
	auto* panel = dynamic_cast<ui::EroderExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->setBounds(0.0f, 0.0f, panel->minimumSize().x, panel->minimumSize().y);
	ui::EroderSubplot& sub = *panel->subplotForTest();
	using Which = ui::EroderSubplot::Which;
	(void)panel->updateContent();
	// AT REST (limiters at 1): no reduction, no level, and the meters are dim (no loop runs at the defaults).
	auto frame = eroFrame(1.0f, 1.0f, -60.0f, -60.0f);
	d.publish(frame);
	d.f.bridge.refresh();
	(void)panel->updateContent();
	CHECK(sub.meterForTest(Which::Inner).reductionDb == 0.0f);
	CHECK(sub.meterForTest(Which::Outer).reductionDb == 0.0f);
	CHECK(sub.meterForTest(Which::Inner).dim);
	CHECK(sub.meterForTest(Which::Outer).dim);
	CHECK(sub.meterFillWidthForTest(Which::Inner) == 0.0f);
	// A REDUCTION OF HALF A GAIN: 20 log10 0.5 decibels, the bar filled by that over the floor.
	frame[static_cast<size_t>(ero::Reading::LimiterInnerGain)] = 0.5f;
	frame[static_cast<size_t>(ero::Reading::LimiterOuterGain)] = 0.25f;
	frame[static_cast<size_t>(ero::Reading::LimiterInnerLevel)] = 0.7f;
	frame[static_cast<size_t>(ero::Reading::LimiterOuterLevel)] = 0.2f;
	d.set(eroAt(ero::Param::Resonance), 0.4f);
	d.set(eroAt(ero::Param::Loop), 0.5f);
	d.publish(frame);
	d.f.bridge.refresh();
	CHECK(panel->updateContent());
	const double innerDb = 20.0 * std::log10(0.5);
	const double outerDb = 20.0 * std::log10(0.25);
	CHECK(sub.meterForTest(Which::Inner).reductionDb == doctest::Approx(static_cast<float>(innerDb)).epsilon(1e-4));
	CHECK(sub.meterForTest(Which::Outer).reductionDb == doctest::Approx(static_cast<float>(outerDb)).epsilon(1e-4));
	CHECK(sub.meterForTest(Which::Inner).level == doctest::Approx(0.7f));
	CHECK(sub.meterForTest(Which::Outer).level == doctest::Approx(0.2f));
	CHECK_FALSE(sub.meterForTest(Which::Inner).dim);
	CHECK_FALSE(sub.meterForTest(Which::Outer).dim);
	const double floorDb = ui::EroderSubplot::kFloorDb;
	CHECK(sub.meterFillWidthForTest(Which::Inner) == doctest::Approx(sub.meterBarForTest(Which::Inner).w * (innerDb / floorDb)).epsilon(1e-4));
	CHECK(sub.meterFillWidthForTest(Which::Outer) == doctest::Approx(sub.meterBarForTest(Which::Outer).w * (outerDb / floorDb)).epsilon(1e-4));
	CHECK(sub.meterFillWidthForTest(Which::Outer) > sub.meterFillWidthForTest(Which::Inner));
	// THE SAME READINGS AGAIN ARE NOT NEWS, and the floor holds the bar to its track.
	CHECK_FALSE(panel->updateContent());
	CHECK(ui::EroderSubplot::fillFor(-60.0f) == 1.0f);
	CHECK(ui::EroderSubplot::fillFor(0.0f) == 0.0f);
	// A LOOP GONE QUIET DIMS ITS METER.
	d.set(eroAt(ero::Param::Loop), 0.0f);
	(void)panel->updateContent();
	CHECK(sub.meterForTest(Which::Outer).dim);
	CHECK_FALSE(sub.meterForTest(Which::Inner).dim);
}
TEST_CASE("ERODER's Etch sub-plot draws slot A's, slot B's and the Drive's transfer curves from the shaper's own function") {
	EroFaces e;
	using P = ero::Param;
	ui::EroderSubplot& sub = *e.panel->subplotForTest();
	using Curve = ui::EroderSubplot::Curve;
	e.mode(ero::Mode::Etch);
	e.set(P::SlotAAmount, 0.5f);
	e.set(P::SlotAType, static_cast<float>(ero::DriveMode::Tube));
	e.set(P::SlotAShape, 0.3f);
	e.set(P::SlotAMix, 1.0f);
	e.set(P::SlotBAmount, 0.0f);
	e.set(P::Drive, 0.0f);
	// THE RESTATEMENT: the gain the Amount sets, the make-up, and the curve itself.
	const double amount = 0.5;
	const double gain = std::exp2(ero::kDriveLog2Min + (ero::kDriveLog2Max - ero::kDriveLog2Min) * amount);
	const double norm = ero::driveMakeUp(ero::DriveMode::Tube, gain, 0.3f);
	for (const float x : { -0.5f, 0.1f, 0.8f }) {
		CAPTURE(x);
		const double want = norm * static_cast<double>(ero::shapeDrive(ero::DriveMode::Tube, x * static_cast<float>(gain), 0.3f, static_cast<float>(gain)));
		CHECK(sub.curveForTest(Curve::A, x) == doctest::Approx(static_cast<float>(want)).epsilon(1e-5));
	}
	CHECK_FALSE(sub.curveIsIdentityForTest(Curve::A));
	// NON-VACUITY: the curve is not the straight line there.
	CHECK(std::abs(sub.curveForTest(Curve::A, 0.8f) - 0.8f) > 0.01f);
	// A SLOT AT AMOUNT 0 AND A DRIVE AT ITS BOTTOM ARE THE IDENTITY LINE, drawn dim.
	CHECK(sub.curveIsIdentityForTest(Curve::B));
	CHECK(sub.curveIsIdentityForTest(Curve::Drive));
	for (const float x : { -0.5f, 0.1f, 0.8f }) {
		CHECK(sub.curveForTest(Curve::B, x) == x);
		CHECK(sub.curveForTest(Curve::Drive, x) == x);
	}
	// A TYPE THAT IS NOT A CURVE (Crush works in time) is the straight line too; the stage's Mix blends the rest.
	e.set(P::SlotBAmount, 0.6f);
	e.set(P::SlotBType, static_cast<float>(ero::DriveMode::Crush));
	CHECK(sub.curveIsIdentityForTest(Curve::B));
	e.set(P::SlotAMix, 0.5f);
	const float full = static_cast<float>(norm * static_cast<double>(ero::shapeDrive(ero::DriveMode::Tube, 0.8f * static_cast<float>(gain), 0.3f, static_cast<float>(gain))));
	CHECK(sub.curveForTest(Curve::A, 0.8f) == doctest::Approx(0.8f + 0.5f * (full - 0.8f)).epsilon(1e-5));
}
TEST_CASE("ERODER's Rust sub-plot draws the carrier the ring multiplies by: the sine, folded to one side by Rectify, Bias of the signal whole, Mod Fold through the engine's own fold") {
	EroFaces e;
	using P = ero::Param;
	ui::EroderSubplot& sub = *e.panel->subplotForTest();
	e.mode(ero::Mode::Rust);
	const auto sine = [](float phase) {
		return std::sin(2.0f * 3.14159265358979f * ui::EroderSubplot::kCarrierCycles * phase);
	};
	const float phases[] = { 0.0625f, 0.2f, 0.4375f, 0.7f };
	// SOURCE ALL SINE, BIAS 0, RECTIFY 0: the sine itself.
	e.set(P::Source, 0.0f);
	e.set(P::RustBias, 0.0f);
	e.set(P::RustRectify, 0.0f);
	e.set(P::ModFold, 0.0f);
	for (const float phase : phases) {
		CAPTURE(phase);
		CHECK(sub.carrierForTest(phase) == doctest::Approx(sine(phase)).epsilon(1e-5));
	}
	// NON-VACUITY: it is not flat.
	CHECK(std::abs(sub.carrierForTest(0.0625f) - sub.carrierForTest(0.2f)) > 0.1f);
	// RECTIFY 1: the sine folded to one side, |sine|.
	e.set(P::RustRectify, 1.0f);
	for (const float phase : phases) {
		CAPTURE(phase);
		CHECK(sub.carrierForTest(phase) == doctest::Approx(std::abs(sine(phase))).epsilon(1e-5));
	}
	// BIAS 0.5: half the signal let through whole, the multiplier moves to Bias + (1 - Bias) of the carrier.
	e.set(P::RustBias, 0.5f);
	for (const float phase : phases) {
		CHECK(sub.carrierForTest(phase) == doctest::Approx(0.5f + 0.5f * std::abs(sine(phase))).epsilon(1e-5));
	}
	// MOD FOLD: the engine's own triangle fold of the sine.
	e.set(P::RustBias, 0.0f);
	e.set(P::RustRectify, 0.0f);
	e.set(P::ModFold, 0.5f);
	for (const float phase : phases) {
		CHECK(sub.carrierForTest(phase) == doctest::Approx(ero::triFold(sine(phase), 1.0f + ero::kModFoldSpan * 0.5f)).epsilon(1e-5));
	}
	// A SOURCE ABOVE NOTHING MIXES IN THE FIXED NOISE TRACE: the same trace each time, and not the sine.
	e.set(P::ModFold, 0.0f);
	e.set(P::Source, 1.0f);
	const float a = sub.carrierForTest(0.3f);
	CHECK(a == sub.carrierForTest(0.3f));
	CHECK(std::abs(a - sine(0.3f)) > 1.0e-3f);
	CHECK(std::abs(a) <= 1.0f);
}
TEST_CASE("ERODER's Comb window draws the spectrum with the comb's own pitch, writes the emphasis from its handle, and keeps its bars' values") {
	EroFaces e;
	using P = ero::Param;
	auto& panel = *e.panel;
	e.mode(ero::Mode::Comb);
	ui::EroderPlot& plot = *panel.plotForTest();
	CHECK(plot.showsSpectrumForTest());
	CHECK_FALSE(plot.showsDustForTest()); // the band is not the comb's
	// THE COMB'S PITCH IS ITS DELAY'S RECIPROCAL, the engine's own `derive` restated.
	const ero::Phases phases = ero::phasesOf(eroField(e.d, P::Tone), eroField(e.d, P::Character), eroField(e.d, P::Gamma));
	const ero::Derived derived = ero::derive(phases, static_cast<double>(eroField(e.d, P::Cutoff)), static_cast<double>(eroField(e.d, P::Mix)),
	                                         ero::kReferenceNote, 48000.0);
	CHECK(plot.centreHzForTest() == doctest::Approx(1.0 / derived.baseDelaySeconds).epsilon(1e-6));
	CHECK(plot.emphasisHzForTest() == doctest::Approx(plot.centreHzForTest()).epsilon(1e-6));
	CHECK(plot.emphasisHandleXForTest() == doctest::Approx(plot.hzToXForTest(plot.centreHzForTest())));
	// THE BARS' VALUES ARE THE BANKS', two of them by value.
	const ero::ToneBank t = ero::evalToneBank(phases.tone);
	const ero::CharacterBank c = ero::evalCharacterBank(phases.character);
	CHECK(plot.barValues()[0] == std::max(static_cast<float>(ero::kBankFloor), t[0]));
	CHECK(plot.barValues()[6] == std::max(static_cast<float>(ero::kDepthFloor), c[0]));
	// THE EMPHASIS HANDLE WORKS HERE AS IN THE OTHER ENGINES: across writes the offset by the pitch axis, up and down the amount.
	e.set(P::EmphAmount, 6.0f);
	plot.update();
	const auto press = [&](float x, float y) { plot.onMouseDown().callback(at(x, y)); };
	const auto drag = [&](float x, float y) { plot.onMouseDrag().callback(at(x, y)); };
	const auto release = [&](float x, float y) { plot.onMouseUp().callback(at(x, y)); };
	{
		const float hx = plot.emphasisHandleXForTest();
		const float hy = plot.emphasisHandleYForTest();
		const float dx = 40.0f;
		press(hx, hy);
		drag(hx + dx, hy);
		release(hx + dx, hy);
		e.refresh();
		const double expected = 12.0 * std::log2(plot.xToHzForTest(hx + dx - ui::EroderPlot::kAmountDeadZone) / plot.xToHzForTest(hx));
		CHECK(expected > 1.0);
		CHECK(eroField(e.d, P::EmphOffset) == doctest::Approx(static_cast<float>(expected)).epsilon(1e-3));
		CHECK(eroField(e.d, P::EmphAmount) == 6.0f);
	}
	e.set(P::EmphOffset, 0.0f);
	plot.update();
	{
		const float hx = plot.emphasisHandleXForTest();
		const float hy = plot.emphasisHandleYForTest();
		const double dbPerPixel = ui::EroderPlot::kEmphasisDbPerQuarter / (0.25 * static_cast<double>(plot.plotFootForTest() - plot.plotTopForTest()));
		press(hx, hy);
		drag(hx, hy - 3.0f - 15.0f);
		release(hx, hy - 18.0f);
		e.refresh();
		CHECK(eroField(e.d, P::EmphAmount) == doctest::Approx(static_cast<float>(6.0 + 15.0 * dbPerPixel)).epsilon(1e-3));
		CHECK(eroField(e.d, P::EmphOffset) == 0.0f);
	}
	// THE CUTOFF HANDLE: on the shake filter's centre, and a drag across writes the Cutoff the engine's own mapping gives for that
	// frequency (the SVF's coefficient is the reference one times the note's ratio times two to the Cutoff).
	{
		e.set(P::Cutoff, 0.0f);
		plot.update();
		const ero::Derived now = ero::derive(phases, 0.0, static_cast<double>(eroField(e.d, P::Mix)), ero::kReferenceNote, 48000.0);
		CHECK(plot.cutoffHandleXForTest() == doctest::Approx(plot.hzToXForTest(now.svfHz)));
		const float hx = plot.cutoffHandleXForTest();
		const float hy = plot.cutoffHandleYForTest();
		const float dx = 50.0f;
		press(hx, hy);
		drag(hx + dx, hy);
		release(hx + dx, hy);
		e.refresh();
		const double expected = plot.cutoffForXForTest(hx + dx);
		CHECK(expected != 0.0);
		CHECK(eroField(e.d, P::Cutoff) == doctest::Approx(static_cast<float>(expected)).epsilon(1e-4));
		// THE ROUND TRIP THROUGH THE ENGINE: with that Cutoff the filter's centre is where the pointer was.
		const ero::Derived after = ero::derive(phases, static_cast<double>(eroField(e.d, P::Cutoff)),
		                                       static_cast<double>(eroField(e.d, P::Mix)), ero::kReferenceNote, 48000.0);
		CHECK(after.svfHz == doctest::Approx(plot.xToHzForTest(hx + dx)).epsilon(1e-3));
		// THE PLOT DECLARES IT, for the guards.
		std::vector<ui::ControlFace::Offered> offered;
		plot.offeredControls(offered);
		CHECK(std::any_of(offered.begin(), offered.end(), [](const ui::ControlFace::Offered& o) {
			return o.control != nullptr && std::string(o.control->desc().key) == "eroder_cutoff";
		}));
		e.set(P::Cutoff, 0.0f);
		plot.update();
	}
	// NOTHING ELSE ON THE PICTURE IS A CONTROL IN THE COMB: a press elsewhere writes neither Freq, Amount nor Width.
	const float freq = eroField(e.d, P::Freq);
	const float amount = eroField(e.d, P::Amount);
	const float width = eroField(e.d, P::Width);
	press(10.0f, plot.plotTopForTest() + 5.0f);
	drag(50.0f, plot.plotTopForTest() - 20.0f);
	release(50.0f, plot.plotTopForTest() - 20.0f);
	CHECK(eroField(e.d, P::Freq) == freq);
	CHECK(eroField(e.d, P::Amount) == amount);
	CHECK(eroField(e.d, P::Width) == width);
}
TEST_CASE("ERODER's greyable controls say in their tooltip when they are inactive and why, on the card and in the window") {
	EroFaces e;
	using P = ero::Param;
	struct Case {
		P param;
		const char* keyword;
	};
	// THE CONDITION EACH ONE NAMES: the control the greying turns on.
	const Case cases[] = { { P::EmphRestore, "Emphasis" }, { P::EmphInLoop, "Emphasis" }, { P::FbTap, "Reso" },
		                   { P::SubThru, "HP Wet" },       { P::LoopInterval, "Loop" },    { P::Width, "Source" },
		                   { P::EmphShape, "Emphasis" },   { P::Detune, "Source" },        { P::ModFold, "Source" },
		                   { P::DcBlock, "offset" } };
	for (const Case& c : cases) {
		const std::string declared = ero::kParamDescs[eroAt(c.param)].tooltip;
		CAPTURE(ero::kParamDescs[eroAt(c.param)].key);
		CHECK(declared.find("nactive") != std::string::npos);
		CHECK(declared.find(c.keyword) != std::string::npos);
		// THE BRIEF TOOLTIP (the first sentence) KEEPS THE CONDITION.
		CHECK(ui::briefTooltip(declared).find("nactive") != std::string::npos);
	}
	// THE WIDGETS ANSWER WITH THEIR CONTROL'S TEXT, on both faces.
	const auto chipSays = [&](ui::SwitchChip* chip, P p) {
		REQUIRE(chip != nullptr);
		CHECK(std::string(chip->tooltipAt({ 2.0f, 2.0f })).find("nactive") != std::string::npos);
		CHECK(std::string(chip->tooltipAt({ 2.0f, 2.0f })) == ero::kParamDescs[eroAt(p)].tooltip);
	};
	chipSays(e.panel->restoreForTest(), P::EmphRestore);
	chipSays(e.panel->inLoopForTest(), P::EmphInLoop);
	chipSays(e.panel->tapForTest(), P::FbTap);
	chipSays(e.panel->subThruForTest(), P::SubThru);
	chipSays(e.panel->dcForTest(), P::DcBlock);
	chipSays(e.d.card->restoreForTest(), P::EmphRestore);
	chipSays(e.d.card->tapForTest(), P::FbTap);
	for (ui::Knob* knob : { e.panel->knobForTest(P::LoopInterval), e.panel->knobForTest(P::Detune), e.panel->knobForTest(P::ModFold) }) {
		REQUIRE(knob != nullptr);
		CHECK(std::string(knob->tooltipAt({ 2.0f, 2.0f })).find("nactive") != std::string::npos);
	}
	CHECK(std::string(e.panel->widthBoxForTest()->tooltipAt({ 2.0f, 2.0f })).find("nactive") != std::string::npos);
	CHECK(std::string(e.d.card->widthForTest()->tooltipAt({ 2.0f, 2.0f })).find("nactive") != std::string::npos);
	CHECK(std::string(e.panel->shapeForTest()->tooltipAt({ 2.0f, 2.0f })).find("nactive") != std::string::npos);
}

namespace {
//------------------------------------------------------------------------------------------------------------------------------//
// A CHOICE BOX'S SECOND PRESS CLOSES THE MENU THE FIRST OPENED.                                                                  //
//------------------------------------------------------------------------------------------------------------------------------//

ui::ModeValueCell* eroModeCell(visage::Frame* root, const char* key) {
	ui::ModeValueCell* found = nullptr;
	const std::function<void(visage::Frame*)> walk = [&](visage::Frame* frame) {
		if (auto* cell = dynamic_cast<ui::ModeValueCell*>(frame);
		    cell != nullptr && std::string_view(cell->select().control().desc().key) == key) {
			found = cell;
		}
		for (visage::Frame* child : frame->children()) {
			walk(child);
		}
	};
	walk(root);
	return found;
}
} // namespace

TEST_CASE("Choice boxes: pressing the box that opened a menu closes it, in the real event order, for the Drive Type and an Etch slot Type") {
	EroFaces e;
	using P = ero::Param;
	for (const bool etch : { false, true }) {
		CAPTURE(etch);
		e.mode(etch ? ero::Mode::Etch : ero::Mode::Dust);
		ui::ModeValueCell* cell = eroModeCell(e.d.card, etch ? "eroder_slot_a_type" : "eroder_drive_mode");
		REQUIRE(cell != nullptr);
		ui::ZoneSelect& box = cell->select();
		visage::Frame* root = e.d.f.root;
		visage::Frame* top = box.topParentFrame();
		const visage::Point centre = box.positionInWindow() + visage::Point { box.width() * 0.5f, box.height() * 0.5f };
		REQUIRE(root->frameAtPoint(centre) == static_cast<visage::Frame*>(&box));
		const auto popup = [&]() -> visage::PopupMenuFrame* {
			visage::PopupMenuFrame* last = nullptr;
			for (visage::Frame* child : top->children()) {
				if (auto* p = dynamic_cast<visage::PopupMenuFrame*>(child); p != nullptr) {
					last = p;
				}
			}
			return last;
		};
		const P type = etch ? P::SlotAType : P::DriveMode;
		const float before = eroField(e.d, type);

		dragThroughRoot(root, centre, centre);
		CHECK(ui::menuOpenFor(box));
		visage::PopupMenuFrame* shown = popup();
		REQUIRE(shown != nullptr);

		// THE REAL ORDER: visage takes the focus off the popup (which cancels it) and THEN delivers the press.
		shown->processFocusChanged(false, true);
		dragThroughRoot(root, centre, centre);
		CHECK_FALSE(ui::menuOpenFor(box));
		CHECK_FALSE(ui::lastMenuPlacementForTest().shown);
		CHECK(popup() == shown); // no second menu
		CHECK(eroField(e.d, type) == before);

		// AND A THIRD PRESS OPENS IT AGAIN.
		dragThroughRoot(root, centre, centre);
		CHECK(ui::menuOpenFor(box));
		CHECK(ui::lastMenuPlacementForTest().shown);
		CHECK(popup() != shown);
		CHECK(eroField(e.d, type) == before);
	}
}

namespace {
/// THE SIX TYPE CELLS an Eroder shows in Etch: the Drive's, and slot A's and slot B's, each on the card and in the window.
struct TypeCell {
	std::string where;
	ui::ModeValueCell* cell;
	ero::Param type;
};
std::vector<TypeCell> typeCells(EroFaces& e) {
	ui::EtchSlots& card = e.d.card->etchSlotsForTest();
	ui::EtchSlots& panel = e.panel->etchSlotsForTest();
	return { { "card drive", e.d.card->driveCellForTest(), ero::Param::DriveMode },
		     { "window drive", e.panel->driveCellForTest(), ero::Param::DriveMode },
		     { "card slot A", card.cellA.get(), ero::Param::SlotAType },
		     { "card slot B", card.cellB.get(), ero::Param::SlotBType },
		     { "window slot A", panel.cellA.get(), ero::Param::SlotAType },
		     { "window slot B", panel.cellB.get(), ero::Param::SlotBType } };
}
} // namespace

TEST_CASE("The Type boxes of the Drive and both Etch slots keep their prev/next arrows: next and prev move the Type by one, stop at the ends, and the box still opens the menu") {
	EroFaces e;
	e.mode(ero::Mode::Etch);
	const int last = static_cast<int>(ero::DriveMode::kCount) - 1;
	// A TYPE WITHOUT A SECOND ROW (Soft, 0), ONE WITH (Chonk, Paunch-S...), AND THE ENDS.
	for (const TypeCell& tc : typeCells(e)) {
		ui::ZoneSelect& sel = tc.cell->select();
		for (const int at : { 0, 1, static_cast<int>(ero::DriveMode::Chonk), static_cast<int>(ero::DriveMode::PaunchOS), last - 1, last }) {
			CAPTURE(tc.where);
			CAPTURE(at);
			e.set(tc.type, static_cast<float>(at));
			REQUIRE(sel.arrowsShown());
			const visage::Bounds prev = sel.prevBounds();
			const visage::Bounds next = sel.nextBounds();
			const visage::Point prevAt { prev.x() + prev.width() * 0.5f, prev.y() + prev.height() * 0.5f };
			const visage::Point nextAt { next.x() + next.width() * 0.5f, next.y() + next.height() * 0.5f };
			const visage::Point midAt { sel.width() * 0.5f, sel.height() * 0.5f };
			CHECK(sel.verbAt(prevAt) == ui::ZoneSelect::Verb::Prev);
			CHECK(sel.verbAt(nextAt) == ui::ZoneSelect::Verb::Next);
			CHECK(sel.verbAt(midAt) == ui::ZoneSelect::Verb::Menu);
			// NEXT: ONE UP, AND STOPPED AT THE LAST (a Choice has no laps).
			CHECK(sel.pressAt(nextAt));
			e.refresh();
			CHECK(eroField(e.d, tc.type) == static_cast<float>(std::min(at + 1, last)));
			// PREV FROM WHERE THAT LEFT IT: back to `at`, or one down from the last.
			e.set(tc.type, static_cast<float>(at));
			CHECK(sel.pressAt(prevAt));
			e.refresh();
			CHECK(eroField(e.d, tc.type) == static_cast<float>(std::max(at - 1, 0)));
		}
	}
	// EVERY TYPE'S SECOND ROW IS DECIDED BY THE STEPPED-TO TYPE: stepping from Soft to Tube to ... reaches Chonk with its Bal row shown.
	e.set(ero::Param::DriveMode, 5.0f);
	ui::ZoneSelect& sel = e.d.card->driveCellForTest()->select();
	const visage::Bounds next = sel.nextBounds();
	CHECK(sel.pressAt({ next.x() + next.width() * 0.5f, next.height() * 0.5f }));
	e.refresh();
	CHECK(eroField(e.d, ero::Param::DriveMode) == 6.0f);
	CHECK(e.d.card->driveCellForTest()->boxFor(eroAt(ero::Param::DriveBal))->isVisible());
}
TEST_CASE("The Type boxes print a short face name at the cell's normal font; the menu, the stored label and the tooltip carry the full name") {
	EroFaces e;
	e.mode(ero::Mode::Etch);
	const int count = static_cast<int>(ero::DriveMode::kCount);
	// THE SHORT NAMES ARE UNIQUE, NON-EMPTY, AND NEVER LONGER THAN THE FULL NAME.
	std::set<std::string> seen;
	for (int i = 0; i < count; ++i) {
		const std::string face = ui::kDriveModeFaceNames[i];
		CAPTURE(face);
		CHECK_FALSE(face.empty());
		CHECK(face.size() <= std::string(ero::kDriveModeNames[i]).size());
		CHECK(seen.insert(face).second);
	}
	CHECK(static_cast<int>(seen.size()) == count);
	// THE STORED LABEL IS UNTOUCHED: the Choice declares the full names.
	const auto& desc = ero::kParamDescs[eroAt(ero::Param::DriveMode)];
	REQUIRE(desc.names.size() == static_cast<size_t>(count));
	for (int i = 0; i < count; ++i) {
		CHECK(std::string(desc.names[static_cast<size_t>(i)]) == ero::kDriveModeNames[i]);
	}
	for (const TypeCell& tc : typeCells(e)) {
		ui::ZoneSelect& sel = tc.cell->select();
		for (int i = 0; i < count; ++i) {
			CAPTURE(tc.where);
			CAPTURE(i);
			// THE MENU'S ITEM IS `captionOf`: the full name, with the "(w)" suffix.
			CHECK(sel.captionOf(static_cast<size_t>(i)) == ero::kDriveModeNames[i]);
			CHECK(sel.faceOf(static_cast<size_t>(i)) == ui::kDriveModeFaceNames[i]);
			e.set(tc.type, static_cast<float>(i));
			CHECK(sel.faceText() == ui::kDriveModeFaceNames[i]);
			CHECK(sel.captionText() == ero::kDriveModeNames[i]);
			// THE FACE DRAWS AT THE CELL'S NORMAL FONT, between the arrows, whole.
			CHECK(sel.arrowsShown());
			CHECK(sel.nameSize() >= ui::kKnobReadoutSize - 0.01f);
			// THE TOOLTIP SAYS THE FULL NAME WHERE THE FACE SAYS A SHORT ONE.
			const std::string tip = tc.cell->tooltipAt({ 2.0f, 2.0f });
			if (std::string(ui::kDriveModeFaceNames[i]) != ero::kDriveModeNames[i]) {
				CHECK(tip.find(ero::kDriveModeNames[i]) != std::string::npos);
			}
		}
	}
}
//------------------------------------------------------------------------------------------------------------------------------//
// THE EMPHASIS OFFSET ON THE WINDOW: the stacked cell beside the knob, and the plot's handle.                                    //
//------------------------------------------------------------------------------------------------------------------------------//

TEST_CASE("ERODER's emphasis offset: the cell's Offset and Width followers, and the plot's handle writes the offset across and the amount up and down") {
	EroFaces e;
	using P = ero::Param;
	auto& panel = *e.panel;
	e.mode(ero::Mode::Dust);
	e.set(P::EmphAmount, 6.0f);

	// ---- THE STACK: Amount, Offset and the shape selector, top to bottom; the Width is the filter box's, the Bell's only for a Bell ----
	ui::FieldStack* stack = panel.emphasisStackForTest();
	REQUIRE(stack != nullptr);
	REQUIRE(stack->boxCount() == 2);
	REQUIRE(stack->selector() != nullptr);
	ui::ValueBox* offsetBox = panel.emphasisOffsetForTest();
	ui::ValueBox* widthBox = panel.widthBoxForTest();
	CHECK(std::string(stack->box(0).control().desc().key) == "eroder_emph_amount");
	CHECK(std::string(offsetBox->control().desc().key) == "eroder_emph_offset");
	CHECK(std::string(stack->selector()->control().desc().key) == "eroder_emph_shape");
	CHECK(std::string(widthBox->control().desc().key) == "eroder_width");
	CHECK(stack->box(0).y() < stack->box(1).y());
	CHECK(stack->box(1).y() < stack->selector()->y());
	CHECK(widthBox->parent() == panel.filterForTest());
	CHECK(offsetBox->isVisible());
	CHECK(offsetBox->enabled());
	e.set(P::Source, 0.0f);
	CHECK_FALSE(widthBox->enabled()); // a Tilt has no width, and Source 0 has no noise band
	e.set(P::EmphShape, 1.0f);
	CHECK(widthBox->enabled());
	CHECK(offsetBox->isVisible());
	e.set(P::Source, 1.0f);
	// THE OFFSET BOX'S OWN DRAG WRITES THE PARAMETER.
	REQUIRE(eroField(e.d, P::EmphOffset) == 0.0f);
	const float cx = static_cast<float>(offsetBox->width()) * 0.5f;
	const float cy = static_cast<float>(offsetBox->height()) * 0.5f;
	offsetBox->onMouseDown().callback(at(cx, cy));
	offsetBox->onMouseDrag().callback(at(cx, cy - 30.0f));
	offsetBox->onMouseUp().callback(at(cx, cy - 30.0f));
	e.refresh();
	CHECK(eroField(e.d, P::EmphOffset) > 0.0f);
	e.set(P::EmphOffset, 0.0f);
	e.set(P::EmphShape, 0.0f);

	// ---- THE PLOT'S HANDLE ----
	ui::EroderPlot& plot = *panel.plotForTest();
	const auto press = [&](float x, float y) { plot.onMouseDown().callback(at(x, y)); };
	const auto drag = [&](float x, float y) { plot.onMouseDrag().callback(at(x, y)); };
	const auto release = [&](float x, float y) { plot.onMouseUp().callback(at(x, y)); };
	const float top = plot.plotTopForTest();
	const float foot = plot.plotFootForTest();
	const double dbPerPixel = ui::EroderPlot::kEmphasisDbPerQuarter / (0.25 * static_cast<double>(foot - top));

	// AT THE CENTRE LINE: the handle sits on the 0 dB line at Freq with no emphasis, so it can be picked up.
	e.set(P::EmphAmount, 0.0f);
	plot.update();
	CHECK(plot.emphasisHandleYForTest() == doctest::Approx(plot.emphasisYForTest(0.0)));
	CHECK(plot.emphasisHandleXForTest() == doctest::Approx(plot.hzToXForTest(plot.bandForTest().freqHz)));
	{
		const float hx = plot.emphasisHandleXForTest();
		const float hy = plot.emphasisHandleYForTest();
		press(hx, hy);
		drag(hx, hy - 3.0f - 20.0f); // up, past the dead zone
		release(hx, hy - 23.0f);
		e.refresh();
		CHECK(eroField(e.d, P::EmphAmount) == doctest::Approx(static_cast<float>(20.0 * dbPerPixel)).epsilon(1e-3));
		CHECK(eroField(e.d, P::EmphOffset) == 0.0f);
	}

	// A PRESS AND RELEASE WITH NO MOVEMENT WRITES NOTHING.
	e.set(P::EmphAmount, 6.0f);
	plot.update();
	{
		const float hx = plot.emphasisHandleXForTest();
		const float hy = plot.emphasisHandleYForTest();
		press(hx, hy);
		release(hx, hy);
		e.refresh();
		CHECK(eroField(e.d, P::EmphAmount) == 6.0f);
		CHECK(eroField(e.d, P::EmphOffset) == 0.0f);
	}

	// ACROSS: the offset the plot's own pitch axis predicts, and the amount is untouched.
	{
		const float hx = plot.emphasisHandleXForTest();
		const float hy = plot.emphasisHandleYForTest();
		const float dx = 60.0f;
		press(hx, hy);
		drag(hx + dx, hy);
		release(hx + dx, hy);
		e.refresh();
		const double expected = 12.0 * std::log2(plot.xToHzForTest(hx + dx - ui::EroderPlot::kAmountDeadZone) / plot.xToHzForTest(hx));
		CAPTURE(expected);
		CHECK(expected > 1.0);
		CHECK(eroField(e.d, P::EmphOffset) == doctest::Approx(static_cast<float>(expected)).epsilon(1e-3));
		CHECK(eroField(e.d, P::EmphAmount) == 6.0f);
		// THE OVERLAY MOVED WITH IT: tuned at Freq times two to the offset over twelve, and the handle follows.
		plot.update();
		const double want = plot.bandForTest().freqHz * std::exp2(static_cast<double>(eroField(e.d, P::EmphOffset)) / 12.0);
		CHECK(plot.emphasisHzForTest() == doctest::Approx(want).epsilon(1e-6));
		CHECK(plot.emphasisHandleXForTest() == doctest::Approx(plot.hzToXForTest(want)));
		CHECK(plot.emphasisHandleXForTest() > hx);
	}
	// A TILT'S CURVE IS CENTRED ON THE OFFSET FREQUENCY: at its tuning the response is 3 dB under its full lift, as at Freq before.
	{
		const double tuned = plot.emphasisHzForTest();
		const double atTuned = *plot.emphasisDbForTest(tuned);
		const double atFreq = *plot.emphasisDbForTest(plot.bandForTest().freqHz);
		CHECK(atTuned != doctest::Approx(atFreq).epsilon(1e-3));
	}

	// UP AND DOWN: the amount, and the offset is untouched.
	{
		const double offsetBefore = eroField(e.d, P::EmphOffset);
		const float hx = plot.emphasisHandleXForTest();
		const float hy = plot.emphasisHandleYForTest();
		press(hx, hy);
		drag(hx, hy + 3.0f + 18.0f); // down
		release(hx, hy + 21.0f);
		e.refresh();
		CHECK(eroField(e.d, P::EmphAmount) == doctest::Approx(static_cast<float>(6.0 - 18.0 * dbPerPixel)).epsilon(1e-3));
		CHECK(eroField(e.d, P::EmphOffset) == doctest::Approx(static_cast<float>(offsetBefore)));
	}

	// THE HANDLE IS THE WINDOW PLOT'S ALONE, and the plot declares its two controls for the guards.
	std::vector<ui::ControlFace::Offered> offered;
	plot.offeredControls(offered);
	const auto offers = [&](P p) {
		return std::any_of(offered.begin(), offered.end(), [&](const ui::ControlFace::Offered& o) {
			return o.control != nullptr && std::string(o.control->desc().key) == ero::kParamDescs[eroAt(p)].key;
		});
	};
	CHECK(offers(P::EmphOffset));
	CHECK(offers(P::EmphAmount));
	CHECK(std::find(panel.addressedParamsForTest().begin(), panel.addressedParamsForTest().end(), P::EmphOffset)
	      != panel.addressedParamsForTest().end());
}
TEST_CASE("OWLTT level meters: each sits to the right of its fader — the input reads the signal after Drive, the output the sum after Output Gain and before the master clip — and the master clip light is the output meter's") {
	namespace ow = feathers::fx::owltt;
	DynCardFixture<ui::OwlttCard> d(ow::kTraits.tag);
	ui::OwlttCard* card = d.card;
	ui::WidgetRegistry registry;
	ui::OwlttExpanded panel { d.f.deviceHost, registry, d.device };
	const visage::Point want = panel.minimumSize();
	panel.setBounds(0.0f, 0.0f, want.x, want.y);
	panel.resized();

	constexpr float kDriveDb = 6.0f;
	const auto settings = [&](bool clip) {
		ow::Settings s;
		s.mode = 1.0f;
		s.threshold = 0.5f;
		s.ratio = 0.0f;
		s.bandwidth = 0.5f;
		s.upDownSkew = 0.5f;
		s.attack = 0.25f;
		s.release = 0.25f;
		s.lowCrossover = 200.0f;
		s.highCrossover = 2000.0f;
		s.outputGain = 0.5f;
		s.blend = 1.0f;
		s.enabled = true;
		s.driveDb = kDriveDb;
		s.softClip = clip;
		s.clipOnly = 2; // the master clip only
		return s;
	};
	const auto sine = [](std::vector<float>& l, std::vector<float>& r, float amplitude, int block) {
		for (size_t i = 0; i < l.size(); ++i) {
			l[i] = r[i] = amplitude * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 1000.0 *
			                                                      static_cast<double>(block * 512 + static_cast<int>(i)) / 48000.0));
		}
	};
	// THE DEVICE UNDER TEST publishes through the bridge as the plugin does; THE REFERENCE is the same device with the master clip
	// OFF, whose rendered buffer peak is the pre-clip sum after Output Gain, computed from the buffer and not from the meter's tap.
	ow::Owltt dsp;
	dsp.prepare(48000.0f);
	dsp.setTarget(settings(true));
	dsp.setMeterCallback([&](size_t, const std::array<float, 3>& gr, const std::array<float, 3>& in,
	                         const std::array<float, 3>& out, bool clipping, const ow::Owltt::MeterExtras& extras) {
		d.f.bridge.publishOwlttMeters(d.slot, gr, in, out, clipping, extras.bandSaturating, extras.combinedGrDb,
		                              &extras.secondChannelGr, extras.noteHz, extras.inputPeak, extras.outputPeak);
	});
	ow::Owltt reference;
	reference.prepare(48000.0f);
	reference.setTarget(settings(false));
	struct Render {
		float handedIn = 0.0f;
		float clippedPeak = 0.0f;
		float preClipPeak = 0.0f;
	};
	const auto render = [&](float amplitude, int blocks) {
		Render r;
		std::vector<float> L(512), R(512), rl(512), rr(512);
		for (int b = 0; b < blocks; ++b) {
			sine(L, R, amplitude, b);
			rl = L;
			rr = R;
			r.handedIn = 0.0f;
			for (const float v : L) {
				r.handedIn = std::max(r.handedIn, std::fabs(v));
			}
			dsp.processStereo(L.data(), R.data(), 512);
			reference.processStereo(rl.data(), rr.data(), 512);
			r.clippedPeak = 0.0f;
			r.preClipPeak = 0.0f;
			for (size_t i = 0; i < L.size(); ++i) {
				r.clippedPeak = std::max(r.clippedPeak, std::max(std::fabs(L[i]), std::fabs(R[i])));
				r.preClipPeak = std::max(r.preClipPeak, std::max(std::fabs(rl[i]), std::fabs(rr[i])));
			}
		}
		return r;
	};

	for (ui::OwlttLevelMeter* in : { card->inputMeterForTest(), panel.inputMeterForTest() }) {
		REQUIRE(in != nullptr);
		CHECK_FALSE(in->hasLed()); // the input has no clip light
	}
	for (ui::OwlttLevelMeter* out : { card->outputMeterForTest(), panel.outputMeterForTest() }) {
		REQUIRE(out != nullptr);
		CHECK(out->hasLed());
	}
	const float driveGain = std::pow(10.0f, kDriveDb / 20.0f);

	// A QUIET SINE: the input meter reads input x Drive, the output the pre-clip sum, and nothing is near the clip.
	const Render quiet = render(0.1f, 6);
	REQUIRE(quiet.handedIn > 0.09f);
	for (auto [in, out] : { std::pair { card->inputMeterForTest(), card->outputMeterForTest() },
	                        std::pair { panel.inputMeterForTest(), panel.outputMeterForTest() } }) {
		CAPTURE(in == card->inputMeterForTest());
		(void)in->update();
		(void)out->update();
		CHECK(in->read().have);
		CHECK(in->read().peak == doctest::Approx(quiet.handedIn * driveGain).epsilon(1e-4));
		CHECK(out->read().peak == doctest::Approx(quiet.preClipPeak).epsilon(1e-4));
		CHECK(in->barFraction() == doctest::Approx(ui::dyn_meter::levelFraction(quiet.handedIn * driveGain)).epsilon(1e-4));
		CHECK(out->barFraction() == doctest::Approx(ui::dyn_meter::levelFraction(quiet.preClipPeak)).epsilon(1e-4));
		CHECK(out->ledState() == ui::ClipLedState::Normal);
	}
	CHECK(card->graph()->meters().bandSaturating == std::array<bool, 3> { false, false, false });

	// DRIVEN PAST 0 dBFS: the output meter reads ABOVE full scale while the rendered output is clipped below it, and its light is the
	// clip; the input's light is not. The bands' lights are the graph's.
	const Render hot = render(1.5f, 40);
	REQUIRE(hot.preClipPeak > 1.0f);
	CHECK(hot.clippedPeak < hot.preClipPeak * 0.95f); // the clipper removed some of what the meter reads
	for (auto [in, out] : { std::pair { card->inputMeterForTest(), card->outputMeterForTest() },
	                        std::pair { panel.inputMeterForTest(), panel.outputMeterForTest() } }) {
		CHECK(in->update());
		CHECK(out->update());
		CHECK(out->ledState() == ui::ClipLedState::Clip);
		CHECK(in->ledState() == ui::ClipLedState::Normal);
		CHECK(out->read().peak == doctest::Approx(hot.preClipPeak).epsilon(1e-4));
		CHECK(out->read().peak > 1.0f);
		CHECK(in->read().peak == doctest::Approx(hot.handedIn * driveGain).epsilon(1e-4));
	}
	const auto bands = card->graph()->meters().bandSaturating;
	CHECK((bands[0] || bands[1] || bands[2])); // the per-band lights still say which band drives it

	// WHERE THEY SIT: each to the RIGHT of its fader — Drive, input meter, plot, Output, output meter — as tall as the picture.
	const auto beside = [](const visage::Bounds& drive, const visage::Bounds& inMeter, const visage::Bounds& plot,
	                       const visage::Bounds& output, const visage::Bounds& outMeter) {
		CHECK(drive.right() <= inMeter.x());
		CHECK(inMeter.x() - drive.right() < 8.0f);
		CHECK(inMeter.right() <= plot.x());
		CHECK(plot.right() <= output.x());
		CHECK(output.right() <= outMeter.x());
		CHECK(outMeter.x() - output.right() < 8.0f);
		CHECK(inMeter.y() == doctest::Approx(plot.y()));
		CHECK(inMeter.height() == doctest::Approx(plot.height()));
		CHECK(outMeter.y() == doctest::Approx(plot.y()));
		CHECK(outMeter.height() == doctest::Approx(plot.height()));
	};
	beside(card->driveBounds(), card->inputMeterBounds(), card->crossoverBounds(), card->outputBounds(),
	       card->outputMeterBounds());
	beside(panel.driveBounds(), panel.inputMeterBounds(), panel.graphBounds(), panel.outputBounds(),
	       panel.outputMeterBounds());
	CHECK(card->outputMeterBounds().right() <= card->width());
}
TEST_CASE("Spectral's Match: the card holds the chip and the 3-stack cell, the window the same controls, and the stack greys with the chip; ratios read as ratios") {
	DynCardFixture<ui::SpectralCard> d(spx::kTraits.tag);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, spx::kDescriptor);
	auto* window = dynamic_cast<ui::SpectralExpanded*>(view.get());
	REQUIRE(window != nullptr);
	const spx::Param face[] = { spx::Param::MatchDownRatio, spx::Param::MatchUpRatio, spx::Param::MatchBandwidth };
	(void)d.card->updateContent();
	(void)window->updateContent();
	// WINDOW ⊇ CARD: every control of the card's Match section is in the window too, each its own widget.
	for (const spx::Param p : { spx::Param::MatchOn, spx::Param::MatchDownRatio, spx::Param::MatchUpRatio, spx::Param::MatchBandwidth }) {
		CAPTURE(static_cast<int>(p));
		CHECK(d.card->widgetForTest(p) != nullptr);
		CHECK(window->boxWidgetForTest(p) != nullptr);
	}
	// OFF: the stack's three pills are greyed on the card and in the window. ON: all live.
	const auto pillsActive = [&](bool on) {
		for (const spx::Param p : face) {
			auto* cardPill = dynamic_cast<ui::FaderPill*>(d.card->widgetForTest(p));
			auto* windowPill = dynamic_cast<ui::FaderPill*>(window->boxWidgetForTest(p));
			REQUIRE(cardPill != nullptr);
			REQUIRE(windowPill != nullptr);
			CHECK(cardPill->active() == on);
			CHECK(windowPill->active() == on);
		}
	};
	CHECK(d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(spx::Param::MatchOn)) < 0.5f);
	pillsActive(false);
	d.set(static_cast<size_t>(spx::Param::MatchOn), 1.0f);
	(void)d.card->updateContent();
	(void)window->updateContent();
	pillsActive(true);
	// THE RATIO TEXT, centred on 1:1 (the control's own text).
	const auto text = [&](double v) { return window->sourceForTest().deviceControl(spx::Param::MatchDownRatio).textAt(v); };
	CHECK(text(0.0) == "1:1");
	CHECK(text(0.75) == "4:1");
	CHECK(text(1.0) == "Inf:1");
	CHECK(text(-0.5) == "1:2");
	CHECK(text(0.9) == "10:1");
}
TEST_CASE("DISPERSER's face carries the slot's Mix: a box on the card and one in the window, both bound to disperser_mix, and a move on either is the same stored value") {
	DynCardFixture<ui::DisperserCard> d(dspr::kTraits.tag);
	const size_t mix = static_cast<size_t>(dspr::Param::Mix);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, dspr::kDescriptor);
	auto* panel = dynamic_cast<ui::DisperserExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->setBounds(0.0f, 0.0f, 700.0f, 420.0f);
	(void)panel->updateContent();
	ui::ValueBox* onCard = d.card->mixBoxForTest();
	ui::ValueBox* inWindow = panel->mixBoxForTest();
	REQUIRE(onCard != nullptr);
	REQUIRE(inWindow != nullptr);
	CHECK(onCard->isVisible());
	CHECK(inWindow->isVisible());
	CHECK(onCard->width() > 0.0f);
	CHECK(inWindow->width() > 0.0f);
	// WINDOW ⊇ CARD, by the control each answers for — read off the trees.
	const auto has = [&](const std::vector<dspr::Param>& v) {
		return std::find(v.begin(), v.end(), dspr::Param::Mix) != v.end();
	};
	CHECK(has(d.card->addressedParamsForTest()));
	CHECK(has(panel->addressedParamsForTest()));
	// THE WET GAIN STAYS THE PANEL'S ALONE.
	const std::vector<dspr::Param> onFace = d.card->addressedParamsForTest();
	CHECK(std::find(onFace.begin(), onFace.end(), dspr::Param::WetGain) == onFace.end());
	// THE DEFAULT IS ALL WET, AND A MOVE ON THE CARD'S BOX IS THE STORED VALUE, SEEN BY THE WINDOW'S.
	CHECK(d.f.deviceHost.fieldAt(d.device, mix) == doctest::Approx(1.0f).epsilon(1e-6));
	onCard->control().setValue(0.35);
	d.f.bridge.refresh();
	CHECK(d.f.deviceHost.fieldAt(d.device, mix) == doctest::Approx(0.35f).epsilon(1e-6));
	CHECK(inWindow->control().value() == doctest::Approx(0.35).epsilon(1e-6));
	inWindow->control().setValue(0.8);
	d.f.bridge.refresh();
	CHECK(onCard->control().value() == doctest::Approx(0.8).epsilon(1e-6));
	view.reset();
}
TEST_CASE("OWLTT's window: each band's OUT is a value box in the table beside IN, bound to that band's Level, and moves it alone") {
	namespace ow = feathers::fx::owltt;
	DynCardFixture<ui::OwlttCard> d(ow::kTraits.tag);
	ui::WidgetRegistry registry;
	ui::OwlttExpanded panel { d.f.deviceHost, registry, d.device };
	const visage::Point want = panel.minimumSize();
	panel.setBounds(0.0f, 0.0f, want.x, want.y);
	panel.resized();
	ui::OwlttGraph* graph = panel.graph();
	REQUIRE(graph != nullptr);
	const ow::Param levels[] = { ow::Param::LowLevel, ow::Param::MidLevel, ow::Param::HighLevel };
	const ow::Param inputs[] = { ow::Param::LowInput, ow::Param::MidInput, ow::Param::HighInput };
	CHECK(std::string(ui::OwlttGraph::tableCaption(0)) == "IN");
	CHECK(std::string(ui::OwlttGraph::tableCaption(1)) == "OUT");
	for (size_t band = 0; band < 3; ++band) {
		CAPTURE(band);
		ui::ValueBox* in = graph->tableBoxForTest(0, band);
		ui::ValueBox* out = graph->tableBoxForTest(1, band);
		REQUIRE(in != nullptr);
		REQUIRE(out != nullptr);
		CHECK(ui::OwlttGraph::tableParam(0, band) == inputs[band]);
		CHECK(ui::OwlttGraph::tableParam(1, band) == levels[band]);
		// THE SAME CONTROL TYPE, side by side, under the same band.
		CHECK(typeid(*in) == typeid(*out));
		CHECK(out->bounds().y() >= in->bounds().bottom());
		CHECK(out->bounds().x() == doctest::Approx(in->bounds().x()));

		const auto fields = [&] {
			const FxChain chain = d.f.bridge.fxChain();
			return fxDeviceAtPosition(chain, d.slot).row.field;
		};
		const auto before = fields();
		out->control().beginGesture();
		out->control().setValue(0.8);
		out->control().endGesture();
		const auto after = fields();
		size_t changed = 0;
		for (size_t i = 0; i < before.size(); ++i) {
			if (after[i] != before[i]) {
				++changed;
				CHECK(i == static_cast<size_t>(levels[band]));
			}
		}
		CHECK(changed == 1u);
		CHECK(after[static_cast<size_t>(levels[band])] == doctest::Approx(0.8f));
	}
}
TEST_CASE("OWLTT's window: the RATIO row reads each band's effective downward and upward ratio as N:1, live with the ratio") {
	namespace ow = feathers::fx::owltt;
	DynCardFixture<ui::OwlttCard> d(ow::kTraits.tag);
	ui::WidgetRegistry registry;
	ui::OwlttExpanded panel { d.f.deviceHost, registry, d.device };
	const visage::Point want = panel.minimumSize();
	panel.setBounds(0.0f, 0.0f, want.x, want.y);
	panel.resized();
	ui::OwlttGraph* graph = panel.graph();
	REQUIRE(graph != nullptr);
	CHECK(std::string(ui::OwlttGraph::tableCaption(ui::OwlttGraph::kRatioRow)) == "RATIO");
	CHECK(graph->tableBoxForTest(ui::OwlttGraph::kRatioRow, 0) == nullptr);

	ow::Owltt dsp;
	dsp.prepare(48000.0f);
	dsp.setMeterCallback([&](size_t, const std::array<float, 3>& gr, const std::array<float, 3>& in,
	                         const std::array<float, 3>& out, bool clipping, const ow::Owltt::MeterExtras& extras) {
		d.f.bridge.publishOwlttMeters(d.slot, gr, in, out, clipping, extras.bandSaturating, extras.combinedGrDb,
		                              &extras.secondChannelGr, extras.noteHz, extras.inputPeak, extras.outputPeak, &extras.ratios);
	});
	const auto run = [&](float lowRatio) {
		ow::Settings s = ow::Settings {};
		s.ratio = 0.5f; // neutral: displaces no band
		s.low.ratio = lowRatio;
		s.mid.ratio = 0.5f;
		s.high.ratio = 0.9f;
		dsp.setTarget(s);
		std::vector<float> l(512, 0.1f), r(512, 0.1f);
		for (int i = 0; i < 4; ++i) {
			dsp.processStereo(l.data(), r.data(), 512);
		}
		(void)graph->update();
	};
	// THE LAW RESTATED: down is 1/(1-k); up is 1/(1-k^3); N printed whole when whole, one decimal below ten.
	const auto text = [](double n) {
		char buf[16];
		if (std::fabs(n - std::round(n)) < 0.05 || n >= 9.95) {
			std::snprintf(buf, sizeof buf, "%.0f:1", n);
		}
		else {
			std::snprintf(buf, sizeof buf, "%.1f:1", n);
		}
		return std::string(buf);
	};
	const auto expect = [&](double k) { return std::string("\xE2\x86\x93") + text(1.0 / (1.0 - k)) + " \xE2\x86\x91" + text(1.0 / (1.0 - k * k * k)); };
	run(0.75f);
	CHECK(graph->ratioReadoutText(0) == expect(0.75)); // down 4:1, up 1.7:1
	CHECK(graph->ratioReadoutText(0) == "\xE2\x86\x93" "4:1 \xE2\x86\x91" "1.7:1");
	CHECK(graph->ratioReadoutText(1) == expect(0.5));  // down 2:1, up 1.1:1
	CHECK(graph->ratioReadoutText(2) == expect(0.9));  // down 10:1, up 3.7:1
	// A RATIO THAT MOVES (a routed source's effective value) MOVES THE TEXT, and only its band's.
	run(0.5f);
	CHECK(graph->ratioReadoutText(0) == expect(0.5));
	CHECK(graph->ratioReadoutText(0) != "\xE2\x86\x93" "4:1 \xE2\x86\x91" "1.7:1");
	CHECK(graph->ratioReadoutText(2) == expect(0.9));
}
TEST_CASE("OWLTT's window: a band's OUT reads in dB like IN, as 20 log10 of the gain the engine applies at that value, and types back through the inverse law") {
	namespace ow = feathers::fx::owltt;
	DynCardFixture<ui::OwlttCard> d(ow::kTraits.tag);
	ui::WidgetRegistry registry;
	ui::OwlttExpanded panel { d.f.deviceHost, registry, d.device };
	const visage::Point want = panel.minimumSize();
	panel.setBounds(0.0f, 0.0f, want.x, want.y);
	panel.resized();
	ui::OwlttGraph* graph = panel.graph();
	REQUIRE(graph != nullptr);
	// THE ENGINE'S LAW, RESTATED: the lower half of the knob is 0..unity, the upper half takes it to 10x.
	const auto gain = [](double g) { return g <= 0.5 ? g * 2.0 : 1.0 + (g - 0.5) * 18.0; };
	const auto text = [&](double g) {
		char buf[24];
		std::snprintf(buf, sizeof buf, "%+.1fdB", 20.0 * std::log10(gain(g)));
		return std::string(buf);
	};
	const ow::Param levels[] = { ow::Param::LowLevel, ow::Param::MidLevel, ow::Param::HighLevel };
	for (size_t band = 0; band < 3; ++band) {
		CAPTURE(band);
		ui::ValueBox* out = graph->tableBoxForTest(1, band);
		REQUIRE(out != nullptr);
		for (const double g : { 0.25, 0.4, 0.5, 0.65, 0.8, 1.0 }) {
			CAPTURE(g);
			d.set(static_cast<size_t>(levels[band]), static_cast<float>(g));
			CHECK(out->readout() == text(g));
			CHECK(out->control().text() == text(g));
		}
		// UNITY READS 0, AND THE TOP +20 dB; the floor reads -inf.
		d.set(static_cast<size_t>(levels[band]), 0.5f);
		CHECK(out->readout() == "+0.0dB");
		d.set(static_cast<size_t>(levels[band]), 0.8f);
		CHECK(out->readout() == "+16.1dB");
		d.set(static_cast<size_t>(levels[band]), 0.0f);
		CHECK(out->readout() == "-inf");
		CHECK(out->control().text() == "-inf");
		// TYPE-IN OPENS ON dB AT FULL PRECISION AND READS BACK TO THE EXACT STORED VALUE, through the inverse of the gain law.
		for (const double g : { 0.03, 0.25, 0.4, 0.5, 0.65, 0.8, 0.9375, 1.0 }) {
			CAPTURE(g);
			d.set(static_cast<size_t>(levels[band]), static_cast<float>(g));
			const double stored = out->control().value();
			const std::optional<std::string> opening = out->control().editText();
			REQUIRE(opening.has_value());
			CHECK(opening->find("dB") != std::string::npos);
			const std::optional<double> back = out->control().parseText(*opening, stored);
			REQUIRE(back.has_value());
			CHECK(static_cast<float>(*back) == static_cast<float>(stored));
		}
		// TYPING -6 dB SETS THE KNOB WHOSE GAIN IS -6 dB (0.5012, the lower half of the law: knob 0.2506); +10 dB IS 3.162 = 0.5 + 2.162/18.
		for (const char* typed : { "-6dB", "-6", "-6 dB" }) {
			const std::optional<double> k = out->control().parseText(typed, 0.5);
			REQUIRE(k.has_value());
			CHECK(*k == doctest::Approx(std::pow(10.0, -6.0 / 20.0) / 2.0).epsilon(1e-6));
		}
		const std::optional<double> hot = out->control().parseText("+10", 0.5);
		REQUIRE(hot.has_value());
		CHECK(*hot == doctest::Approx(0.5 + (std::pow(10.0, 0.5) - 1.0) / 18.0).epsilon(1e-6));
		const std::optional<double> over = out->control().parseText("+30dB", 0.5);
		REQUIRE(over.has_value());
		CHECK(*over == doctest::Approx(1.0)); // past the top of the knob: clamped
	}
}
TEST_CASE("QUAD LFO: the card's tabs are real presses; each tab shows its own set, bound to its own fields, and the others stay put") {
	namespace lfo = feathers::mod::lfo;
	FilterFixture f;
	const size_t slot = f.bridge.addRackDevice(lfo::kTraits.tag);
	REQUIRE(slot < kMaxRackSlots);
	f.bridge.refresh();
	f.panel->resized();
	auto* card = const_cast<ui::LfoCard*>(dynamic_cast<const ui::LfoCard*>(f.panel->cardFrameForTest(slot)));
	REQUIRE(card != nullptr);
	const DeviceInstanceId id = f.bridge.rackDeviceAt(slot);
	const auto field = [&](size_t tab, lfo::Param p) { return static_cast<double>(f.bridge.rackFieldAt(slot, lfo::tabParam(tab, p))); };
	const auto clickTab = [&](size_t tab) {
		const visage::Bounds b = card->tabBounds(tab);
		// THE LABEL SIDE, away from the On square: a press there selects.
		dragThroughRoot(f.root, card->positionInWindow() + visage::Point { b.right() - 6.0f, b.y() + b.height() * 0.5f },
		                card->positionInWindow() + visage::Point { b.right() - 6.0f, b.y() + b.height() * 0.5f });
	};
	// THE VISIBLE CHOICE BOX IS ALWAYS EXACTLY ONE SET'S.
	for (const size_t tab : { size_t { 2 }, size_t { 1 }, size_t { 3 }, size_t { 0 } }) {
		CAPTURE(tab);
		clickTab(tab);
		REQUIRE(card->selectedTab() == tab);
		CHECK(card->visibleSetWidgetsForTest() == 3); // two choice boxes and the pad of ONE set
		double before[lfo::kTabs];
		for (size_t t = 0; t < lfo::kTabs; ++t) {
			before[t] = field(t, lfo::Param::Mode);
		}
		ui::ValueBox* mode = card->choiceBoxForTest(1); // the selected tab's Mode box
		REQUIRE(mode != nullptr);
		REQUIRE(mode->isVisible());
		const double target = before[tab] == 3.0 ? 1.0 : 3.0;
		mode->control().setValue(target);
		for (size_t t = 0; t < lfo::kTabs; ++t) {
			CAPTURE(t);
			CHECK(field(t, lfo::Param::Mode) == (t == tab ? target : before[t]));
		}
	}
	// A PRESS ON A TAB'S ON SQUARE SWITCHES THAT TAB, and selects nothing.
	clickTab(0);
	REQUIRE(card->selectedTab() == 0);
	REQUIRE_FALSE(card->tabOn(2));
	const visage::Bounds on = card->tabOnBounds(2);
	const visage::Point centre = card->positionInWindow() + visage::Point { on.x() + on.width() * 0.5f, on.y() + on.height() * 0.5f };
	dragThroughRoot(f.root, centre, centre);
	CHECK(card->tabOn(2));
	CHECK(field(2, lfo::Param::Enabled) == 1.0);
	CHECK(card->selectedTab() == 0);
	// AND THE CARD NOW SHOWS TAB 3'S OUTPUT, which `cardHidden` kept off until it was on.
	(void)card->updateContent();
	CHECK((f.bridge.outputsShown(id) & (uint64_t { 1 } << 2)) != 0);
	CHECK((f.bridge.outputsShown(id) & (uint64_t { 1 } << 1)) == 0);
}
TEST_CASE("QUAD S&H: the card's tabs are real presses; each tab shows its own set, bound to its own fields, and the view field follows") {
	namespace sh = feathers::mod::samplehold;
	FilterFixture f;
	const size_t slot = f.bridge.addRackDevice(sh::kTraits.tag);
	REQUIRE(slot < kMaxRackSlots);
	f.bridge.refresh();
	f.panel->resized();
	auto* card = const_cast<ui::SampleHoldCard*>(dynamic_cast<const ui::SampleHoldCard*>(f.panel->cardFrameForTest(slot)));
	REQUIRE(card != nullptr);
	const DeviceInstanceId id = f.bridge.rackDeviceAt(slot);
	const auto field = [&](size_t tab, sh::Param p) { return static_cast<double>(f.bridge.rackFieldAt(slot, sh::tabParam(tab, p))); };
	const auto clickAt = [&](visage::Point inCard) {
		const visage::Point w = card->positionInWindow() + inCard;
		dragThroughRoot(f.root, w, w);
	};
	for (const size_t tab : { size_t { 2 }, size_t { 1 }, size_t { 3 }, size_t { 0 } }) {
		CAPTURE(tab);
		const visage::Bounds b = card->tabBounds(tab);
		clickAt({ b.right() - 6.0f, b.y() + b.height() * 0.5f });
		REQUIRE(card->selectedTab() == tab);
		// THE VIEW FIELD FOLLOWS, which is what makes the device publish this tab's playhead.
		CHECK(f.bridge.rackFieldAt(slot, sh::kViewTabField) == static_cast<float>(tab));
		// EXACTLY ONE SET'S WIDGETS ARE SHOWN (the live boxes, the three chips, the pad), and a Rate edit lands on THIS tab.
		CHECK(card->visibleSetWidgetsForTest() >= 4);
		double before[sh::kTabs];
		for (size_t t = 0; t < sh::kTabs; ++t) {
			before[t] = field(t, sh::Param::Probability);
		}
		const double target = before[tab] > 0.5 ? 0.25 : 0.75;
		card->box(sh::Param::Probability).control().setValue(target);
		for (size_t t = 0; t < sh::kTabs; ++t) {
			CAPTURE(t);
			CHECK(field(t, sh::Param::Probability) == doctest::Approx(t == tab ? target : before[t]));
		}
	}
	// THE SET COUNT IS CONSTANT: switching tabs shows one set whatever the tab.
	card->selectTab(1);
	const size_t one = card->visibleSetWidgetsForTest();
	card->selectTab(3);
	CHECK(card->visibleSetWidgetsForTest() == one);
	// THE ON SQUARE SWITCHES THAT TAB AND SELECTS NOTHING; the card then shows that tab's pair of outputs.
	card->selectTab(0);
	REQUIRE_FALSE(card->tabOn(2));
	const visage::Bounds on = card->tabOnBounds(2);
	clickAt({ on.x() + on.width() * 0.5f, on.y() + on.height() * 0.5f });
	CHECK(card->tabOn(2));
	CHECK(card->selectedTab() == 0);
	(void)card->updateContent();
	CHECK((f.bridge.outputsShown(id) & (uint64_t { 0b11 } << 4)) == (uint64_t { 0b11 } << 4));
	CHECK((f.bridge.outputsShown(id) & (uint64_t { 0b11 } << 2)) == 0);
}
TEST_CASE("PHOTO: the quad LFO and quad S&H cards, tab 1 and a switched-on tab 2 (writes only under FEATHERS_QUAD_SHOT_DIR)") {
	const char* dir = std::getenv("FEATHERS_QUAD_SHOT_DIR");
	if (dir == nullptr) {
		return;
	}
	const std::string out = dir;
	namespace lfo = feathers::mod::lfo;
	namespace sh = feathers::mod::samplehold;
	// THE WHOLE EDITOR, windowless, as the rack views' photographs are taken: the real strip with the real cards in it, children
	// and all.
	const auto shoot = [](FilterFixture& f, const std::string& path) {
		auto* editor = dynamic_cast<visage::ApplicationEditor*>(f.root);
		REQUIRE(editor != nullptr);
		editor->setWindowless(static_cast<int>(f.root->width()), static_cast<int>(f.root->height()));
		if (!visage::Renderer::instance().supported()) {
			MESSAGE("SKIP no renderer: " << visage::Renderer::instance().errorMessage());
			return;
		}
		f.root->redrawAll();
		editor->drawWindow();
		editor->drawWindow();
		const visage::Screenshot& shot = editor->takeScreenshot();
		REQUIRE(shot.width() > 0);
		shot.save(path.c_str());
	};
	{
		FilterFixture f;
		const size_t slot = f.bridge.addRackDevice(lfo::kTraits.tag);
		REQUIRE(slot < kMaxRackSlots);
		f.bridge.refresh();
		f.panel->resized();
		auto* card = const_cast<ui::LfoCard*>(dynamic_cast<const ui::LfoCard*>(f.panel->cardFrameForTest(slot)));
		REQUIRE(card != nullptr);
		shoot(f, out + "/lfo-tab1.png");
		f.bridge.setDeviceControl(f.bridge.rackDeviceAt(slot), lfo::tabParam(1, lfo::Param::Enabled), 1.0f);
		f.bridge.setDeviceControl(f.bridge.rackDeviceAt(slot), lfo::tabParam(1, lfo::Param::Waveform), static_cast<float>(lfo::Waveform::Square));
		f.bridge.refresh();
		card->selectTab(1);
		(void)card->updateContent();
		f.panel->resized();
		shoot(f, out + "/lfo-tab2.png");
	}
	{
		FilterFixture f;
		const size_t slot = f.bridge.addRackDevice(sh::kTraits.tag);
		REQUIRE(slot < kMaxRackSlots);
		f.bridge.refresh();
		f.panel->resized();
		auto* card = const_cast<ui::SampleHoldCard*>(dynamic_cast<const ui::SampleHoldCard*>(f.panel->cardFrameForTest(slot)));
		REQUIRE(card != nullptr);
		shoot(f, out + "/snh-tab1.png");
		f.bridge.setDeviceControl(f.bridge.rackDeviceAt(slot), sh::tabParam(1, sh::Param::Enabled), 1.0f);
		f.bridge.refresh();
		card->selectTab(1);
		(void)card->updateContent();
		f.panel->resized();
		shoot(f, out + "/snh-tab2.png");
	}
}

namespace {
//--------------------------------------------------------------------------------------------------//
// THE ERODER'S HOVER TEXTS FOLLOW EACH DRIVE UNIT'S TYPE (`StageTips`, `fx/eroder/drive_tips.h`).     //
//--------------------------------------------------------------------------------------------------//

/// A HOVER MAY CARRY THE CONTROL'S FULL NAME OR THE TYPE'S AT ITS START ("Paunch-S (w) - ..."); what it ends with is the text under test.
bool hoverEndsWith(const std::string& hover, const std::string& tail) {
	return !tail.empty() && hover.size() >= tail.size() && hover.compare(hover.size() - tail.size(), tail.size(), tail) == 0;
}
/// ONE DRIVE UNIT'S FACES, card and window: its parameters, its Amount knobs and its cells (the Drive's, slot A's, slot B's).
struct TipUnit {
	const char* name;
	ui::StageParams p;
	ui::Knob* knobs[2];
	ui::ModeValueCell* cells[2];
};
std::vector<TipUnit> tipUnits(EroFaces& e) {
	ui::EtchSlots& card = e.d.card->etchSlotsForTest();
	ui::EtchSlots& panel = e.panel->etchSlotsForTest();
	return { { "Drive", ui::driveStageParams(), { e.d.card->driveForTest(), e.panel->driveForTest() }, { e.d.card->driveCellForTest(), e.panel->driveCellForTest() } },
		     { "Slot A", ui::slotStageParams(0), { card.knobA.get(), panel.knobA.get() }, { card.cellA.get(), panel.cellA.get() } },
		     { "Slot B", ui::slotStageParams(1), { card.knobB.get(), panel.knobB.get() }, { card.cellB.get(), panel.cellB.get() } } };
}
/// THE THREE HOVERS UNDER TEST for one cell and knob, against the table's row for `type` (and `keyed`).
void checkTipsFor(ui::Knob& knob, ui::ModeValueCell& cell, const ui::StageParams& p, ero::DriveMode type, bool keyed) {
	const ero::DriveTip& tip = ero::driveTip(type);
	const visage::Point at { 1.0f, 1.0f };
	const std::string amount = keyed && tip.amountKeyed != nullptr ? tip.amountKeyed : tip.amount;
	CHECK(hoverEndsWith(ui::hoverText(knob, at), amount));
	CHECK(hoverEndsWith(ui::hoverText(cell, at), tip.type));
	if (tip.shape != nullptr) {
		ui::ValueBox* shape = cell.boxFor(static_cast<size_t>(p.shape));
		REQUIRE(shape != nullptr);
		CHECK(hoverEndsWith(ui::hoverText(*shape, at), tip.shape));
	}
	else {
		// THE PAUNCH TYPES' ROW IS THEIR ANGLE, whose tooltip is its own (declared, without the list of Types).
		ui::ValueBox* angle = cell.boxFor(static_cast<size_t>(p.angle));
		REQUIRE(angle != nullptr);
		CHECK(hoverEndsWith(ui::hoverText(*angle, at), ero::withoutOnlyClause(angle->control().desc().tooltip)));
	}
}
} // namespace

TEST_CASE("ERODER's hover texts follow each Drive unit's Type — the Drive and both Etch slots, on the card and in the window, set through the host") {
	EroFaces e;
	e.mode(ero::Mode::Etch);
	const std::vector<TipUnit> units = tipUnits(e);
	const visage::Point at { 1.0f, 1.0f };
	for (const TipUnit& u : units) {
		CAPTURE(std::string(u.name));
		for (const ero::DriveMode type : { ero::DriveMode::Tube, ero::DriveMode::Fold, ero::DriveMode::PaunchS, ero::DriveMode::Decimate, ero::DriveMode::Lean,
		                                   ero::DriveMode::Chonk, ero::DriveMode::Slew, ero::DriveMode::Soft }) {
			CAPTURE(std::string(ero::kDriveModeNames[static_cast<size_t>(type)]));
			e.set(static_cast<ero::Param>(u.p.type), static_cast<float>(type));
			for (size_t i = 0; i < 2; ++i) {
				CAPTURE(i);
				checkTipsFor(*u.knobs[i], *u.cells[i], u.p, type, false);
			}
		}
		// THE PINS: a table that is wrong everywhere would still agree with itself.
		e.set(u.p.type, static_cast<float>(ero::DriveMode::Decimate));
		for (size_t i = 0; i < 2; ++i) {
			CHECK(ui::hoverText(*u.knobs[i], at).find("How long each sample is held") != std::string::npos);
			ui::ValueBox* shape = u.cells[i]->boxFor(static_cast<size_t>(u.p.shape));
			CHECK(ui::hoverText(*shape, at).find("straight line") != std::string::npos);
			CHECK(ui::hoverText(*u.cells[i], at).find("nothing is rounded") != std::string::npos);
		}
		e.set(u.p.type, static_cast<float>(ero::DriveMode::Tube));
		for (size_t i = 0; i < 2; ++i) {
			CHECK(ui::hoverText(*u.knobs[i], at).find("an eighth at 0% to sixteen times at 100%") != std::string::npos);
			CHECK(ui::hoverText(*u.cells[i]->boxFor(static_cast<size_t>(u.p.shape)), at).find("8 times lower") != std::string::npos);
		}
		e.set(u.p.type, static_cast<float>(ero::DriveMode::PaunchS));
		for (size_t i = 0; i < 2; ++i) {
			CHECK(ui::hoverText(*u.knobs[i], at).find("rectified mid") != std::string::npos);
		}
	}
}
TEST_CASE("ERODER's hover texts update when the Type moves by any road, and the Amount's follows the Key switch") {
	EroFaces e;
	e.mode(ero::Mode::Etch);
	const std::vector<TipUnit> units = tipUnits(e);
	const visage::Point at { 1.0f, 1.0f };
	for (const TipUnit& u : units) {
		CAPTURE(std::string(u.name));
		// THE HOST ROAD (automation, a patch load) AND THE CONTROL'S OWN ROAD (a click, an arrow, a menu pick) are both a write to the field;
		// the second is made through the cell's own control with a gesture, as a hand does.
		e.set(u.p.type, static_cast<float>(ero::DriveMode::Fold));
		checkTipsFor(*u.knobs[0], *u.cells[0], u.p, ero::DriveMode::Fold, false);
		ui::ZoneSelect& select = u.cells[0]->select();
		select.selectZone(static_cast<size_t>(ero::DriveMode::Crush));
		e.refresh();
		for (size_t i = 0; i < 2; ++i) {
			checkTipsFor(*u.knobs[i], *u.cells[i], u.p, ero::DriveMode::Crush, false);
		}
		CHECK(ui::hoverText(*u.knobs[0], at).find("about 2 ms") != std::string::npos);
		// KEY ON: the Amount is a multiple of the note; Key off: back to the time. A Type without a Key does not read the switch.
		e.set(u.p.key, 1.0f);
		for (size_t i = 0; i < 2; ++i) {
			checkTipsFor(*u.knobs[i], *u.cells[i], u.p, ero::DriveMode::Crush, true);
			CHECK(ui::hoverText(*u.knobs[i], at).find("multiple of the played note's frequency") != std::string::npos);
		}
		e.set(u.p.key, 0.0f);
		checkTipsFor(*u.knobs[0], *u.cells[0], u.p, ero::DriveMode::Crush, false);
		e.set(u.p.key, 1.0f);
		e.set(u.p.type, static_cast<float>(ero::DriveMode::Tube));
		for (size_t i = 0; i < 2; ++i) {
			checkTipsFor(*u.knobs[i], *u.cells[i], u.p, ero::DriveMode::Tube, true);
		}
		e.set(u.p.key, 0.0f);
		// THE THIRD ROW: its text is the declared one without the list of Types, and Lean's Rate says what it turns.
		e.set(u.p.type, static_cast<float>(ero::DriveMode::Soft));
		for (size_t i = 0; i < 2; ++i) {
			ui::ValueBox* hard = u.cells[i]->boxFor(static_cast<size_t>(u.p.hard));
			CHECK(hoverEndsWith(ui::hoverText(*hard, at), ero::withoutOnlyClause(hard->control().desc().tooltip)));
			CHECK(ui::hoverText(*hard, at).find(" only:") == std::string::npos);
			CHECK(ui::hoverText(*hard, at).find("knee") != std::string::npos);
		}
		e.set(u.p.type, static_cast<float>(ero::DriveMode::Lean));
		for (size_t i = 0; i < 2; ++i) {
			ui::ValueBox* rate = u.cells[i]->boxFor(static_cast<size_t>(u.p.rate));
			CHECK(hoverEndsWith(ui::hoverText(*rate, at), ero::kLeanRateTip));
		}
		e.set(u.p.type, static_cast<float>(ero::DriveMode::PaunchOS));
		for (size_t i = 0; i < 2; ++i) {
			ui::ValueBox* rate = u.cells[i]->boxFor(static_cast<size_t>(u.p.rate));
			CHECK(hoverEndsWith(ui::hoverText(*rate, at), ero::withoutOnlyClause(rate->control().desc().tooltip)));
		}
	}
}
TEST_CASE("ERODER's hover texts are right on a freshly opened card and window whose state already has a non-default Type") {
	DynCardFixture<ui::EroderCard> d(ero::kTraits.tag);
	const ero::DriveMode types[] = { ero::DriveMode::Decimate, ero::DriveMode::PaunchOH, ero::DriveMode::Lopside };
	const ui::StageParams params[] = { ui::driveStageParams(), ui::slotStageParams(0), ui::slotStageParams(1) };
	for (size_t u = 0; u < 3; ++u) {
		d.set(eroAt(params[u].type), static_cast<float>(types[u]));
	}
	// A NEW CARD AND A NEW WINDOW, built after the state was set and never given a tick.
	ui::WidgetRegistry registry;
	auto card = std::make_unique<ui::EroderCard>(d.f.deviceHost, registry, d.device);
	ui::WidgetRegistry registry2;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry2, d.device, ero::kDescriptor);
	auto* panel = dynamic_cast<ui::EroderExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	ui::Knob* cardKnobs[] = { card->driveForTest(), card->etchSlotsForTest().knobA.get(), card->etchSlotsForTest().knobB.get() };
	ui::ModeValueCell* cardCells[] = { card->driveCellForTest(), card->etchSlotsForTest().cellA.get(), card->etchSlotsForTest().cellB.get() };
	ui::Knob* panelKnobs[] = { panel->driveForTest(), panel->etchSlotsForTest().knobA.get(), panel->etchSlotsForTest().knobB.get() };
	ui::ModeValueCell* panelCells[] = { panel->driveCellForTest(), panel->etchSlotsForTest().cellA.get(), panel->etchSlotsForTest().cellB.get() };
	for (size_t u = 0; u < 3; ++u) {
		CAPTURE(u);
		checkTipsFor(*cardKnobs[u], *cardCells[u], params[u], types[u], false);
		checkTipsFor(*panelKnobs[u], *panelCells[u], params[u], types[u], false);
	}
}
TEST_CASE("ERODER's per-Type hover table: every Type has an Amount and a Type text, a Shape text or the Angle's, and no text carries an \"only:\" clause or names the hardware") {
	REQUIRE(sizeof ero::kDriveTips / sizeof ero::kDriveTips[0] == static_cast<size_t>(ero::DriveMode::kCount));
	for (size_t i = 0; i < static_cast<size_t>(ero::DriveMode::kCount); ++i) {
		const auto type = static_cast<ero::DriveMode>(i);
		CAPTURE(std::string(ero::kDriveModeNames[i]));
		const ero::DriveTip& tip = ero::driveTip(type);
		REQUIRE(tip.type != nullptr);
		REQUIRE(tip.amount != nullptr);
		CHECK(std::string(tip.type).size() > 20);
		CHECK(std::string(tip.amount).size() > 20);
		// A SHAPE TEXT, OR THE ANGLE: the four Paunch Types alone have none (their row is another control); a Key's text only where there is a Key.
		CHECK((tip.shape == nullptr) == (ero::driveIsPaunchLaw(type) || ero::driveIsPaunchOdd(type)));
		CHECK((tip.amountKeyed != nullptr) == ero::driveHasKey(type));
		for (const char* text : { tip.type, tip.amount, tip.amountKeyed, tip.shape }) {
			if (text == nullptr) {
				continue;
			}
			const std::string s(text);
			CHECK(s.find(" only:") == std::string::npos);
			// THE NAME THE HARDWARE GOES BY, assembled as `test_device_tooltips` does.
			std::string lower;
			for (const char c : s) {
				lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
			}
			CHECK(lower.find(std::string("del") + "uge") == std::string::npos);
		}
	}
	CHECK(std::string(ero::kLeanRateTip).find(" only:") == std::string::npos);
	CHECK(std::string(ero::kLeanRateMultTip).find(" only:") == std::string::npos);
	// THE CLAUSE STRIPPER: a leading list goes and the first letter is raised; text without one is as it was.
	CHECK(ero::withoutOnlyClause("Soft and Tube only: the knee. Shown only while its Type is Soft") == "The knee. Shown only while its Type is Soft");
	CHECK(ero::withoutOnlyClause("What Drive pushes into") == "What Drive pushes into");
}

namespace {
//--------------------------------------------------------------------------------------------------//
// CLIP'S HOVER TEXTS FOLLOW ITS MODE (`ClipCard::applyTips`, `fx/clip/clip_tips.h`).                   //
// Clip has no expanded window (`Traits::hasExpanded` is false): the card is its one face.             //
//--------------------------------------------------------------------------------------------------//

/// THE THREE HOVERS UNDER TEST on a Clip card, against the table's row for `mode`: the Mode box, the Drive (its Amount) and, where the mode has one, the Shape.
void checkClipTips(ui::ClipCard& card, int mode) {
	namespace cl = feathers::fx::clip;
	const cl::ClipTip& tip = cl::modeTip(mode);
	const visage::Point at { 1.0f, 1.0f };
	auto* drive = dynamic_cast<ui::Knob*>(card.widgetForTest(cl::Param::Drive));
	auto* shape = dynamic_cast<ui::Knob*>(card.widgetForTest(cl::Param::Shape));
	REQUIRE(drive != nullptr);
	REQUIRE(shape != nullptr);
	CHECK(hoverEndsWith(ui::hoverText(*card.modeForTest(), at), tip.type));
	CHECK(hoverEndsWith(ui::hoverText(*drive, at), tip.amount));
	if (tip.shape != nullptr) {
		CHECK(hoverEndsWith(ui::hoverText(*shape, at), tip.shape));
	}
	else {
		// A MODE WITH NO SHAPE KNOB leaves the declared text.
		CHECK(hoverEndsWith(ui::hoverText(*shape, at), cl::kParamDescs[static_cast<size_t>(cl::Param::Shape)].tooltip));
	}
}
} // namespace

TEST_CASE("CLIP's hover texts follow its Type — the Mode box, the Drive and the Shape, set through the host") {
	namespace cl = feathers::fx::clip;
	DynCardFixture<ui::ClipCard> d(cl::kTraits.tag);
	const visage::Point at { 1.0f, 1.0f };
	for (const cl::Mode mode : { cl::Mode::Soft, cl::Mode::Fold, cl::Mode::Tube, cl::Mode::Jag, cl::Mode::Chonk, cl::Mode::PaunchS, cl::Mode::PaunchOS, cl::Mode::Slew, cl::Mode::Lopside }) {
		CAPTURE(std::string(cl::kModeNames[static_cast<size_t>(mode)]));
		d.set(static_cast<size_t>(cl::Param::Mode), static_cast<float>(mode));
		(void)d.card->updateContent();
		checkClipTips(*d.card, static_cast<int>(mode));
	}
	// THE PINS: a table that is wrong everywhere would still agree with itself.
	auto* drive = dynamic_cast<ui::Knob*>(d.card->widgetForTest(cl::Param::Drive));
	auto* shape = dynamic_cast<ui::Knob*>(d.card->widgetForTest(cl::Param::Shape));
	auto* rate = dynamic_cast<ui::Knob*>(d.card->widgetForTest(cl::Param::Rate));
	d.set(static_cast<size_t>(cl::Param::Mode), static_cast<float>(cl::Mode::Slew));
	(void)d.card->updateContent();
	CHECK(ui::hoverText(*drive, at).find("20 kHz at -24 dB down to 50 Hz at +24 dB") != std::string::npos);
	CHECK(ui::hoverText(*shape, at).find("one-pole lowpass") != std::string::npos);
	d.set(static_cast<size_t>(cl::Param::Mode), static_cast<float>(cl::Mode::Tube));
	(void)d.card->updateContent();
	CHECK(ui::hoverText(*drive, at).find("from -24 dB to +24 dB") != std::string::npos);
	CHECK(ui::hoverText(*shape, at).find("8 times lower") != std::string::npos);
	CHECK(ui::hoverText(*drive, at).find("sixteen times") == std::string::npos); // the Eroder's Amount is not Clip's Drive
	d.set(static_cast<size_t>(cl::Param::Mode), static_cast<float>(cl::Mode::PaunchS));
	(void)d.card->updateContent();
	CHECK(ui::hoverText(*drive, at).find("four times the mid's level at +24 dB") != std::string::npos);
	CHECK(ui::hoverText(*rate, at).find("Angle holds still") != std::string::npos);
	CHECK(ui::hoverText(*rate, at).find("Lean") == std::string::npos);
	// THE MODES' OWN CONTROLS DROP THEIR LEADING "X only:" CLAUSE.
	d.set(static_cast<size_t>(cl::Param::Mode), static_cast<float>(cl::Mode::Tube));
	(void)d.card->updateContent();
	auto* hard = dynamic_cast<ui::Knob*>(d.card->widgetForTest(cl::Param::Hard));
	REQUIRE(hard != nullptr);
	CHECK(ui::hoverText(*hard, at).find(" only:") == std::string::npos);
	CHECK(hoverEndsWith(ui::hoverText(*hard, at), feathers::fx::eroder::withoutOnlyClause(hard->control()->desc().tooltip)));
}
TEST_CASE("CLIP's hover texts update when the Type moves by the Mode box's own selection") {
	namespace cl = feathers::fx::clip;
	DynCardFixture<ui::ClipCard> d(cl::kTraits.tag);
	const visage::Point at { 1.0f, 1.0f };
	d.set(static_cast<size_t>(cl::Param::Mode), static_cast<float>(cl::Mode::Fold));
	(void)d.card->updateContent();
	checkClipTips(*d.card, static_cast<int>(cl::Mode::Fold));
	// THE MENU'S PICK IS THE ROAD A HAND TAKES: it writes the field, and the card's next tick follows it.
	for (const cl::Mode to : { cl::Mode::Lean, cl::Mode::PaunchOH, cl::Mode::Lopside, cl::Mode::Soft }) {
		CAPTURE(std::string(cl::kModeNames[static_cast<size_t>(to)]));
		d.card->modeForTest()->choiceMenu().onSelection().callback(static_cast<int>(to));
		d.f.bridge.refresh();
		(void)d.card->updateContent();
		checkClipTips(*d.card, static_cast<int>(to));
	}
	auto* drive = dynamic_cast<ui::Knob*>(d.card->widgetForTest(cl::Param::Drive));
	CHECK(ui::hoverText(*drive, at).find("soft shoulder") == std::string::npos); // (the Mode box says that; the Drive says what it feeds)
	CHECK(hoverEndsWith(ui::hoverText(*drive, at), cl::modeTip(static_cast<int>(cl::Mode::Soft)).amount));
}
TEST_CASE("CLIP's hover texts are right on a freshly opened card whose state already has a non-default Type") {
	namespace cl = feathers::fx::clip;
	DynCardFixture<ui::ClipCard> d(cl::kTraits.tag);
	d.set(static_cast<size_t>(cl::Param::Mode), static_cast<float>(cl::Mode::Chonk));
	// A NEW CARD, built after the state was set and never given a tick or a layout.
	ui::WidgetRegistry registry;
	auto card = std::make_unique<ui::ClipCard>(d.f.deviceHost, registry, d.device);
	checkClipTips(*card, static_cast<int>(cl::Mode::Chonk));
	const visage::Point at { 1.0f, 1.0f };
	CHECK(ui::hoverText(*dynamic_cast<ui::Knob*>(card->widgetForTest(cl::Param::Shape)), at).find("turned over on the right side only") != std::string::npos);
}
TEST_CASE("CLIP's per-Type hover table: every Type has texts, and no hover in any Type's cell carries \" only:\" or names the hardware") {
	namespace cl = feathers::fx::clip;
	REQUIRE(sizeof cl::kModeTips / sizeof cl::kModeTips[0] == static_cast<size_t>(cl::Mode::kCount));
	DynCardFixture<ui::ClipCard> d(cl::kTraits.tag);
	const visage::Point at { 1.0f, 1.0f };
	const auto clean = [](const std::string& s) {
		std::string lower;
		for (const char c : s) {
			lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
		}
		// THE NAME THE HARDWARE GOES BY, assembled as `test_device_tooltips` does.
		return s.find(" only:") == std::string::npos && lower.find(std::string("del") + "uge") == std::string::npos;
	};
	size_t seen = 0;
	for (int mode = 0; mode < static_cast<int>(cl::Mode::kCount); ++mode) {
		CAPTURE(std::string(cl::kModeNames[mode]));
		const cl::ClipTip& tip = cl::modeTip(mode);
		REQUIRE(tip.type != nullptr);
		REQUIRE(tip.amount != nullptr);
		CHECK(std::string(tip.type).size() > 20);
		CHECK(std::string(tip.amount).size() > 20);
		// A SHAPE TEXT EXACTLY WHERE THE CARD SHOWS A SHAPE KNOB.
		CHECK((tip.shape != nullptr) == (((cl::kShapeKnobModes >> static_cast<uint32_t>(mode)) & 1u) != 0u));
		d.set(static_cast<size_t>(cl::Param::Mode), static_cast<float>(mode));
		(void)d.card->updateContent();
		for (const cl::Param p : d.card->addressedParamsForTest()) {
			// (THE ENABLE SQUARE IS DRAWN IN THE HEAD, and Bits' segmented control has no one tooltip: neither is a source here.)
			auto* source = dynamic_cast<ui::TooltipSource*>(d.card->widgetForTest(p));
			if (source == nullptr) {
				continue;
			}
			++seen;
			CAPTURE(std::string(cl::kParamDescs[static_cast<size_t>(p)].name));
			CHECK(clean(ui::hoverText(*source, at)));
		}
	}
	CHECK(seen > 15 * 10);
}

