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

// PIECE 9 OF 12 of the dahdsr suite: its cases in their original order, with the helpers only they use. See dahdsr_common.h.

#include "dahdsr/dahdsr_common.h"

TEST_CASE("additive: Shape Amount takes Warp's slot and the mode box is the Shape, every choice on its menu before one applies") {
	// DESIGN §4.1 ("one knob+a dropdown", Warp's pattern): the per-sound Shape Amount in the slot Warp
	// uses on V5, and the engine-wide Shape in Warp's mode-box slot under it. Warp is not drawn on Additive.
	ExploreFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Additive));
	f.set(ParamId::AEnable, 1.0);
	f.stage.update();
	using FlagshipKind = ExploreStage::FlagshipKind;
	Knob& amount = f.stage.shapeAmountKnobForTest(0);
	CHECK(amount.isVisible());
	CHECK(amount.enabled());
	CHECK(amount.bounds() == f.stage.flagshipKindBounds(FlagshipKind::Warp));
	CHECK_FALSE(f.stage.warpKnobForTest(0).isVisible());
	// The knob is sound A's Shape Amount: a write through it lands on 0x020C, and on no other sound's.
	f.bridge.drain([](const UiParamEvent&) {});
	amount.control()->beginGesture();
	amount.control()->setValue(0.75);
	amount.control()->endGesture();
	std::vector<UiParamEvent> wrote;
	f.bridge.drain([&](const UiParamEvent& e) { wrote.push_back(e); });
	REQUIRE(wrote.size() == 3);
	CHECK(wrote[1].id == static_cast<clap_id>(ParamId::AShapeAmount));
	CHECK(wrote[1].value == doctest::Approx(0.75));
	// A second sound selected shows ITS knob, bound to its own id.
	f.selection.select(1);
	f.set(ParamId::BEnable, 1.0);
	f.stage.update();
	CHECK_FALSE(f.stage.shapeAmountKnobForTest(0).isVisible());
	CHECK(f.stage.shapeAmountKnobForTest(1).isVisible());

	// THE BOX names the engine's Shape (default Saw), opens a menu of all four at a press, and a pick is one bracketed
	// edit of Additive Shape.
	CHECK(f.stage.additiveShapeLive());
	CHECK_FALSE(f.stage.warpModeLive());
	CHECK(std::string(f.stage.additiveShapeLabel()) == "Saw");
	f.set(ParamId::AdditiveShape, 0.0);
	f.stage.update();
	CHECK(std::string(f.stage.additiveShapeLabel()) == "Sine");
	const auto popupCount = [&] {
		int n = 0;
		for (visage::Frame* child : f.stage.children()) {
			n += dynamic_cast<visage::PopupMenuFrame*>(child) != nullptr ? 1 : 0;
		}
		return n;
	};
	const visage::Bounds box = f.stage.warpModeBounds();
	f.bridge.drain([](const UiParamEvent&) {});
	const int before = popupCount();
	f.stage.processMouseDown(at(box.x() + box.width() * 0.5f, box.y() + box.height() * 0.5f));
	f.stage.processMouseUp(at(box.x() + box.width() * 0.5f, box.y() + box.height() * 0.5f));
	CHECK(popupCount() == before + 1);
	CHECK_FALSE(f.bridge.hasPendingWork()); // opening the menu applied nothing
	f.stage.chooseAdditiveShape(3);
	std::vector<UiParamEvent> posted;
	f.bridge.drain([&](const UiParamEvent& e) { posted.push_back(e); });
	REQUIRE(posted.size() == 3);
	CHECK(posted[1].id == static_cast<clap_id>(ParamId::AdditiveShape));
	CHECK(posted[1].value == 3.0);
	visage::Canvas canvas;
	CHECK(f.stage.drawWarpModeBoxForTest(canvas));

	// AND ON V5 THE SAME SLOT IS WARP'S AGAIN: the Shape Amount is not drawn and the box is the warp mode.
	f.set(ParamId::OscType, static_cast<double>(OscType::V5));
	f.stage.update();
	CHECK_FALSE(f.stage.shapeAmountKnobForTest(1).isVisible());
	CHECK(f.stage.warpKnobForTest(1).isVisible());
	CHECK_FALSE(f.stage.additiveShapeLive());
	CHECK(f.stage.warpModeLive());
}
TEST_CASE("additive: the six bias knobs take FM Depth's slot and the right flank's five, each bound to its own sound's id") {
	// DESIGN §4.1: on Additive the left flank is Width, Shape Amount over the Shape box, Smoothness and ONE bias knob in
	// FM Depth's slot (Rolloff); the right flank is the other five, in V5's field-modifier slots. Every id written out.
	using FlagshipKind = ExploreStage::FlagshipKind;
	using FieldKind = ExploreStage::FieldKind;
	using BK = ExploreStage::AdditiveBiasKind;
	struct Want {
		BK kind;
		const char* label;
		visage::Bounds (*slot)(const ExploreStage&);
		ParamId a, b;
	};
	const Want kWant[] = {
		{ BK::Rolloff, "Rolloff", [](const ExploreStage& s) { return s.flagshipKindBounds(FlagshipKind::FmDepth); },
		  ParamId::ARolloff, ParamId::BRolloff },
		{ BK::Formant, "Formant", [](const ExploreStage& s) { return s.fieldKindBounds(FieldKind::Quantize); },
		  ParamId::AFormant, ParamId::BFormant },
		{ BK::OddEven, "Odd/Even", [](const ExploreStage& s) { return s.fieldKindBounds(FieldKind::Sync); },
		  ParamId::AOddEven, ParamId::BOddEven },
		{ BK::Comb, "Comb", [](const ExploreStage& s) { return s.fieldKindBounds(FieldKind::Feedback); },
		  ParamId::AComb, ParamId::BComb },
		{ BK::Detail, "Detail", [](const ExploreStage& s) { return s.fieldKindBounds(FieldKind::Points); },
		  ParamId::ADetail, ParamId::BDetail },
		{ BK::OffGrid, "Off-grid", [](const ExploreStage& s) { return s.fieldKindBounds(FieldKind::Grit); },
		  ParamId::AOffGrid, ParamId::BOffGrid },
	};
	CHECK(static_cast<uint32_t>(ParamId::ARolloff) == 0x0280u);
	CHECK(static_cast<uint32_t>(ParamId::HOffGrid) == 0x02F5u);
	ExploreFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Additive));
	f.set(ParamId::AEnable, 1.0);
	f.set(ParamId::BEnable, 1.0);
	f.stage.update();
	CHECK_FALSE(f.stage.fmDepthKnobForTest(0).isVisible());
	for (const Want& w : kWant) {
		CAPTURE(w.label);
		Knob& knob = f.stage.additiveBiasKnobForTest(w.kind, 0);
		CHECK(knob.isVisible());
		CHECK(knob.enabled());
		CHECK(std::string(knob.label()) == w.label);
		CHECK(knob.bounds() == w.slot(f.stage));
		CHECK(knob.bounds().y() >= 0.0f);
		CHECK(knob.bounds().bottom() <= f.stage.height());
		// Sound A's knob writes A's id, and nothing else.
		f.bridge.drain([](const UiParamEvent&) {});
		knob.control()->beginGesture();
		knob.control()->setValue(0.8);
		knob.control()->endGesture();
		std::vector<UiParamEvent> wrote;
		f.bridge.drain([&](const UiParamEvent& e) { wrote.push_back(e); });
		REQUIRE(wrote.size() == 3);
		CHECK(wrote[1].id == static_cast<clap_id>(w.a));
		CHECK(wrote[1].value == doctest::Approx(0.8));
		// The default is the neutral, the field as the terrain authored it.
		CHECK(feathers::findParam(static_cast<clap_id>(w.a))->def == feathers::kFieldNeutral);
	}
	// THE TERRAIN WINDOW CARRIES THE SELECTED SOUND'S BASE VALUES, by member.
	f.set(ParamId::ARolloff, 0.1);
	f.set(ParamId::AFormant, 0.2);
	f.set(ParamId::AOddEven, 0.3);
	f.set(ParamId::AComb, 0.4);
	f.set(ParamId::ADetail, 0.6);
	f.set(ParamId::AOffGrid, 0.7);
	f.stage.update();
	const TerrainWindow window = f.stage.askForTerrainForTest();
	CHECK(window.additive.bias.rolloff == doctest::Approx(0.1f));
	CHECK(window.additive.bias.formant == doctest::Approx(0.2f));
	CHECK(window.additive.bias.oddEven == doctest::Approx(0.3f));
	CHECK(window.additive.bias.comb == doctest::Approx(0.4f));
	CHECK(window.additive.bias.detail == doctest::Approx(0.6f));
	CHECK(window.additive.bias.offGrid == doctest::Approx(0.7f));
	// A second sound selected shows ITS six, bound to its own ids.
	f.selection.select(1);
	f.stage.update();
	for (const Want& w : kWant) {
		CAPTURE(w.label);
		CHECK_FALSE(f.stage.additiveBiasKnobForTest(w.kind, 0).isVisible());
		Knob& knob = f.stage.additiveBiasKnobForTest(w.kind, 1);
		REQUIRE(knob.isVisible());
		f.bridge.drain([](const UiParamEvent&) {});
		knob.control()->beginGesture();
		knob.control()->setValue(0.25);
		knob.control()->endGesture();
		std::vector<UiParamEvent> wrote;
		f.bridge.drain([&](const UiParamEvent& e) { wrote.push_back(e); });
		REQUIRE(wrote.size() == 3);
		CHECK(wrote[1].id == static_cast<clap_id>(w.b));
	}
	// AND ON V5 THE SLOTS ARE FM DEPTH'S AND THE FIELD MODIFIERS' AGAIN.
	f.set(ParamId::OscType, static_cast<double>(OscType::V5));
	f.stage.update();
	for (const Want& w : kWant) {
		CAPTURE(w.label);
		CHECK_FALSE(f.stage.additiveBiasKnobForTest(w.kind, 1).isVisible());
	}
	CHECK(f.stage.fmDepthKnobForTest(1).isVisible());
	CHECK(f.stage.fieldKnobForTest(FieldKind::Quantize, 1).isVisible());
}

namespace {
/// THE SURROUND AS DRAWN, one line per control: every visible knob on the stage (what it binds, its caption, whether it
/// is live with the selected sound playing and with it parked, and its rectangle), and each mode box that draws (which
/// choice it names, and its rectangle). Read off the widgets, never off the slot table, so it can pin the table.
std::vector<std::string> exploreSurroundAsDrawn(OscType engine, size_t sound, float width, float height) {
	std::vector<std::string> lines;
	std::vector<std::pair<Knob*, bool>> liveWhenPlaying;
	const char* const engineName = kOscTypeNames[static_cast<size_t>(engine)];
	for (const double enable : { 1.0, 0.0 }) {
		ExploreFixture f;
		f.stage.setBounds(0.0f, 0.0f, width, height);
		f.set(ParamId::OscType, static_cast<double>(engine));
		f.set(soundParam(sound, SoundField::Enable), enable);
		f.selection.select(sound);
		f.stage.update();
		REQUIRE(f.selection.slot() == sound);
		std::vector<std::string> here;
		for (visage::Frame* child : f.stage.children()) {
			auto* knob = dynamic_cast<Knob*>(child);
			if (knob == nullptr || !knob->isVisible()) {
				continue;
			}
			std::string binds = "travel";
			if (const auto* control = dynamic_cast<const ParamControl*>(knob->control())) {
				binds = findParam(control->id())->name;
			}
			const visage::Bounds b = knob->bounds();
			// THE HIT TEST: a press at the drawn knob's centre reaches that knob.
			const visage::Point centre { b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f };
			const std::string label = knob->label();
			CAPTURE(label);
			CHECK(f.stage.frameAtPoint(centre) == knob);
			// AND ITS CAPTION IS NOT CUT at this size ("SMOOTHN..." was) — the Explore surround is not a device card, so the
			// device-walking caption guard (`test_gui_widgets_tooltips`) does not reach it; every drawn knob is asked here.
			CHECK_FALSE(knob->captionCut());
			char line[256];
			std::snprintf(line, sizeof(line), "%s s%zu %gx%g knob '%s' = %s live%d [%.9g %.9g %.9g %.9g]", engineName,
			              sound, static_cast<double>(width), static_cast<double>(height), knob->label(), binds.c_str(),
			              knob->enabled() ? 1 : 0, static_cast<double>(b.x()), static_cast<double>(b.y()),
			              static_cast<double>(b.width()), static_cast<double>(b.height()));
			here.emplace_back(line);
		}
		visage::Canvas canvas;
		const auto box = [&](const char* side, const char* names, visage::Bounds b) {
			char line[256];
			std::snprintf(line, sizeof(line), "%s s%zu %gx%g box %s '%s' [%.9g %.9g %.9g %.9g]", engineName, sound,
			              static_cast<double>(width), static_cast<double>(height), side, names, static_cast<double>(b.x()),
			              static_cast<double>(b.y()), static_cast<double>(b.width()), static_cast<double>(b.height()));
			here.emplace_back(line);
		};
		if (f.stage.drawWarpModeBoxForTest(canvas)) {
			box("left", f.stage.bowModeLive() ? "bow mode" : f.stage.additiveShapeLive() ? "additive shape" : "warp mode",
			    f.stage.warpModeBounds());
		}
		if (f.stage.drawModelBoxForTest(canvas)) {
			box("right", f.stage.shapesPageLive() ? "shapes page" : "model", f.stage.modelBoxBounds());
		}
		std::sort(here.begin(), here.end());
		if (enable != 0.0) {
			lines = std::move(here);
			continue;
		}
		// PARKED: the same controls in the same places (the sound's switch greys, it never hides); only `live` moves.
		REQUIRE(here.size() == lines.size());
		for (size_t i = 0; i < here.size(); ++i) {
			std::string playing = lines[i];
			const size_t at = playing.find(" live");
			if (at != std::string::npos) {
				playing.replace(at, 6, std::string(" live") + playing[at + 5] + here[i][at + 5]);
			}
			lines[i] = playing;
		}
	}
	return lines;
}
} // namespace

TEST_CASE("backlog LJ: every engine's Explore surround binds the same parameter in the same rectangle as before the slot table") {
	// THE PIN, taken from mainline 069e4aec (before the slot table) by running this case with FEATHERS_LJ_PIN_WRITE=1:
	// per engine, at two stage sizes and two selected sounds, each drawn knob's binding, caption, liveness (sound on,
	// sound parked) and rectangle, and each drawn mode box's choice and rectangle. The slot table is a REFACTOR, so this
	// must not move by one entry or one pixel. A knob's binding is pinned by the parameter's NAME, not its id (ids are
	// free to move). `live10` = enabled with the sound on, greyed while it is parked.
	static const char* const kPinned[] = {
#include "explore_surround_pin.inc"
	};
	std::vector<std::string> actual;
	struct Config {
		size_t sound;
		float width;
		float height;
	};
	// The fixture's stage, and the design canvas with every rail open (backlog LH's 374x539), on a non-first sound.
	const Config kConfigs[] = { { 0, 866.0f, 534.0f }, { 2, 374.0f, 539.0f } };
	for (size_t e = 0; e < static_cast<size_t>(OscType::kCount); ++e) {
		for (const Config& c : kConfigs) {
			for (std::string& line : exploreSurroundAsDrawn(static_cast<OscType>(e), c.sound, c.width, c.height)) {
				actual.push_back(std::move(line));
			}
		}
	}
	if (std::getenv("FEATHERS_LJ_PIN_WRITE") != nullptr) {
		for (const std::string& line : actual) {
			std::printf("\"%s\",\n", line.c_str());
		}
	}
	CHECK(actual.size() == std::size(kPinned));
	for (size_t i = 0; i < std::min(actual.size(), std::size(kPinned)); ++i) {
		CAPTURE(i);
		CHECK(actual[i] == std::string(kPinned[i]));
	}
}
TEST_CASE("backlog LJ: a slot table can seat one engine's knob in another engine's gap, and it lays out, binds and hit-tests there") {
	// Nothing reflows, but a different engine's knob can be placed in a gap deliberately. The product does not do it
	// today; a table handed to the stage does, with no layout
	// code: Resonate's Speed in Morph's empty Width slot, V5's per-sound Smoothness in Morph's empty first right slot
	// (not its own slot), and Resonate's Model box under Morph's right flank. Morph's Grit stays where it was, and on
	// V5 the same table puts everything back where V5's row says.
	using Slot = ExploreStage::Slot;
	using C = ExploreStage::SurroundControl;
	using RK = ExploreStage::ResonateKnob;
	ExploreFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Morph));
	f.set(soundParam(1, SoundField::Enable), 1.0);
	f.selection.select(1);
	f.stage.update();

	ExploreStage::SlotTable table = f.stage.slotTable();
	ExploreStage::SlotRow& morph = table[static_cast<size_t>(OscType::Morph)];
	// They are gaps on Morph today, and these three are not Morph's.
	REQUIRE(morph[static_cast<size_t>(Slot::Left0)] == C::None);
	REQUIRE(morph[static_cast<size_t>(Slot::Right0)] == C::None);
	REQUIRE(morph[static_cast<size_t>(Slot::RightBox)] == C::None);
	REQUIRE_FALSE(f.stage.resonateKnobForTest(RK::Speed).isVisible());
	REQUIRE_FALSE(f.stage.smoothnessKnobForTest(1).isVisible());
	visage::Canvas canvas;
	REQUIRE_FALSE(f.stage.drawModelBoxForTest(canvas));
	morph[static_cast<size_t>(Slot::Left0)] = C::Speed;
	morph[static_cast<size_t>(Slot::Right0)] = C::Smoothness;
	morph[static_cast<size_t>(Slot::RightBox)] = C::Model;
	f.stage.setSlotTableForTest(table);

	const auto centreOf = [](visage::Bounds b) { return visage::Point { b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f }; };
	const auto boundId = [](Knob& knob) {
		const auto* control = dynamic_cast<const ParamControl*>(knob.control());
		REQUIRE(control != nullptr);
		return control->id();
	};
	const auto dragUpWrites = [&](Knob& knob, ParamId param) {
		const double before = f.bridge.value(static_cast<clap_id>(param));
		knob.processMouseDown(at(26.0f, 60.0f));
		knob.processMouseDrag(at(26.0f, 60.0f - Knob::kDragRange * 0.25f));
		knob.processMouseUp(at(26.0f, 60.0f - Knob::kDragRange * 0.25f));
		f.settleGesture();
		CHECK(f.bridge.value(static_cast<clap_id>(param)) > before);
	};

	// RESONATE'S SPEED, in Width's slot: laid out there, bound to its own parameter, reached by a press there, and live.
	Knob& speed = f.stage.resonateKnobForTest(RK::Speed);
	CHECK(speed.isVisible());
	CHECK(speed.enabled());
	CHECK(speed.bounds() == f.stage.slotBounds(Slot::Left0));
	CHECK(speed.bounds() == f.stage.flagshipKindBounds(ExploreStage::FlagshipKind::Width));
	CHECK(f.stage.resonateKnobBounds(RK::Speed) == speed.bounds());
	CHECK(boundId(speed) == static_cast<clap_id>(ParamId::ResonateSpeed));
	CHECK(f.stage.frameAtPoint(centreOf(speed.bounds())) == &speed);
	dragUpWrites(speed, ParamId::ResonateSpeed);

	// V5'S SMOOTHNESS, per sound, in the right flank's first slot: the SELECTED sound's knob, bound to sound B's id.
	Knob& smooth = f.stage.smoothnessKnobForTest(1);
	CHECK(smooth.isVisible());
	CHECK(smooth.enabled());
	CHECK_FALSE(f.stage.smoothnessKnobForTest(0).isVisible());
	CHECK(smooth.bounds() == f.stage.slotBounds(Slot::Right0));
	CHECK(smooth.bounds() == f.stage.fieldKindBounds(ExploreStage::FieldKind::Quantize));
	CHECK(boundId(smooth) == static_cast<clap_id>(smoothnessParam(1)));
	CHECK(f.stage.frameAtPoint(centreOf(smooth.bounds())) == &smooth);
	dragUpWrites(smooth, smoothnessParam(1));
	// The sound's own switch still greys a seated per-sound knob, and not the engine-wide one.
	f.set(soundParam(1, SoundField::Enable), 0.0);
	f.stage.update();
	CHECK(smooth.isVisible());
	CHECK_FALSE(smooth.enabled());
	CHECK(speed.enabled());
	f.set(soundParam(1, SoundField::Enable), 1.0);
	f.stage.update();

	// MORPH'S OWN GRIT is where it was.
	Knob& grit = f.stage.fieldKnobForTest(ExploreStage::FieldKind::Grit, 1);
	CHECK(grit.isVisible());
	CHECK(grit.bounds() == f.stage.slotBounds(Slot::Right4));
	CHECK(f.stage.frameAtPoint(centreOf(grit.bounds())) == &grit);

	// THE MODEL BOX under Morph's right flank: drawn, live, and a press there opens its menu.
	CHECK(f.stage.drawModelBoxForTest(canvas));
	CHECK(f.stage.modelBoxLive());
	CHECK_FALSE(f.stage.drawWarpModeBoxForTest(canvas)); // Morph's left box slot is still empty
	const auto popups = [&] {
		int n = 0;
		for (visage::Frame* child : f.stage.children()) {
			n += dynamic_cast<visage::PopupMenuFrame*>(child) != nullptr ? 1 : 0;
		}
		return n;
	};
	const int before = popups();
	const visage::Point box = centreOf(f.stage.slotBounds(Slot::RightBox));
	f.stage.processMouseDown(at(box.x, box.y));
	f.stage.processMouseUp(at(box.x, box.y));
	CHECK(popups() == before + 1);

	// AND ON V5 THE SAME TABLE IS V5'S ROW: Speed is gone, Width is back in its slot, Smoothness stands in its own.
	f.set(ParamId::OscType, static_cast<double>(OscType::V5));
	f.stage.update();
	CHECK_FALSE(speed.isVisible());
	CHECK_FALSE(speed.enabled());
	CHECK(f.stage.widthKnobForTest(1).isVisible());
	CHECK(f.stage.frameAtPoint(centreOf(f.stage.slotBounds(Slot::Left0))) == &f.stage.widthKnobForTest(1));
	CHECK(smooth.bounds() == f.stage.slotBounds(Slot::Left3));
	CHECK(f.stage.frameAtPoint(centreOf(f.stage.slotBounds(Slot::Right0)))
	      == &f.stage.fieldKnobForTest(ExploreStage::FieldKind::Quantize, 1));
}
TEST_CASE("backlog LJ: on every engine each slot holds at most one drawn control, and no drawn control stands outside a slot") {
	// The backlog's two invariants, asked of the widgets: every drawn flank control's rectangle is exactly one slot's,
	// and no slot is claimed twice. The polar pair is a corner, not a slot, and is set aside by identity.
	using Slot = ExploreStage::Slot;
	for (size_t e = 0; e < static_cast<size_t>(OscType::kCount); ++e) {
		const std::string engine = kOscTypeNames[e];
		CAPTURE(engine);
		ExploreFixture f;
		f.set(ParamId::OscType, static_cast<double>(e));
		f.set(ParamId::AEnable, 1.0);
		f.stage.update();
		std::array<int, ExploreStage::kSlotCount> claims {};
		const auto claim = [&](visage::Bounds b) {
			int matched = 0;
			for (size_t s = 0; s < ExploreStage::kSlotCount; ++s) {
				if (f.stage.slotBounds(static_cast<Slot>(s)) == b) {
					++claims[s];
					++matched;
				}
			}
			CHECK(matched == 1);
		};
		size_t drawn = 0;
		for (visage::Frame* child : f.stage.children()) {
			auto* knob = dynamic_cast<Knob*>(child);
			if (knob == nullptr || !knob->isVisible() || knob == &f.stage.radiusKnobForTest(0)
			    || knob == &f.stage.thetaKnobForTest(0)) {
				continue;
			}
			const std::string label = knob->label();
			CAPTURE(label);
			claim(knob->bounds());
			++drawn;
		}
		visage::Canvas canvas;
		if (f.stage.drawWarpModeBoxForTest(canvas)) {
			claim(f.stage.warpModeBounds());
			++drawn;
		}
		if (f.stage.drawModelBoxForTest(canvas)) {
			claim(f.stage.modelBoxBounds());
			++drawn;
		}
		for (size_t s = 0; s < ExploreStage::kSlotCount; ++s) {
			CAPTURE(s);
			CHECK(claims[s] <= 1);
		}
		// Not vacuous: every engine but Stair draws something on its flanks.
		CHECK((drawn > 0) == (static_cast<OscType>(e) != OscType::Stair));
	}
}
TEST_CASE("every OWLTT handle ring is inside the graph and grabs its own control") {
	// The defect: handles drawn below the band view that neither moved nor controlled anything.
	//
	// ONE OFFSET EXPLAINS BOTH HALVES OF THAT. `visage::Canvas::circle(x, y, width)` takes
	// the TOP-LEFT of the bounding box, which every other caller in this editor knows -- celestial,
	// value_box, harm_scope, mseg_card and perform_stage all write `circle(cx - r, cy - r, r * 2)`.
	// The OWLTT rings passed the CENTRE, so all twelve drew four pixels down and right of the thing
	// they sit on. A strip's ring sits at its VALUE, so a low strip put its centre near
	// `xover.bottom()` and the ring rendered BELOW the graph -- where `hitTestThreshold` refuses the
	// point before it looks at anything, because it opens with `xover.contains(pos)`. Drawn out of
	// the box it is grabbable in: mysterious, and inert, for the same reason.
	//
	// THE CASE ASKS WHERE THE RING IS AND THEN PRESSES THERE. A drawn position cannot be observed
	// headlessly, which is why `ringCentre` is exposed -- and this card has now shipped THREE bugs of
	// the form "drawn here, grabbable there", so the geometry being askable is the fix and this is
	// what makes it one.
	FilterFixture f;
	auto* lane = f.lane;
	REQUIRE(f.bridge.addFxDevice(fx::owltt::kTraits.tag) == 1);
	lane->resized();

	auto* card = dynamic_cast<ui::OwlttCard*>(lane->cardAt(1));
	REQUIRE(card != nullptr);
	f.bridge.setDeviceControl(f.bridge.fxDeviceAt(1), static_cast<size_t>(fx::owltt::Param::Enabled), 1.0f);

	const visage::Bounds box = card->graph()->plot();

	using Handle = ui::OwlttGraph::Handle;
	struct Probe { Handle handle; fx::owltt::Param param; const char* name; };
	// THE CONTROL EACH RING CLAIMS TO MOVE. The upper bar sizes the gap and the lower carries the
	// threshold -- the sign the DSP states, `thresholdUp = thresholdDown + 0.6 * bandwidth`.
	const std::array<Probe, 4> probes { {
	    { Handle::BandwidthBar, fx::owltt::Param::LowBandwidth, "upper bar" },
	    { Handle::ThresholdBar, fx::owltt::Param::LowThreshold, "lower bar" },
	    { Handle::RatioStrip, fx::owltt::Param::LowRatio, "ratio strip" },
	    { Handle::LevelStrip, fx::owltt::Param::LowLevel, "level strip" },
	} };

	for (const Probe& probe : probes) {
		CAPTURE(std::string(probe.name));
		const visage::Point centre = card->graph()->ringCentre(0, probe.handle);

		// INSIDE THE PICTURE, WHOLE. Not merely its centre: a ring whose centre clears the edge by
		// less than its radius is drawn half outside the thing it belongs to, and at the ends of a
		// strip's travel that is exactly where it lands. HARM paid for this one first
		// (harm_scope.cpp:324) -- a control unreachable at the end of its travel is unreachable
		// precisely where a hand goes to bring it back.
		CHECK(centre.x - 4.0f >= box.x());
		CHECK(centre.x + 4.0f <= box.right());
		CHECK(centre.y - 4.0f >= box.y());
		CHECK(centre.y + 4.0f <= box.bottom());

		// AND A PRESS THERE REACHES THE CONTROL THE RING IS FOR. Not "something moved" -- the named
		// control, because the four rings sit close together and a press landing on a neighbour looks
		// identical to a press landing right.
		const auto idx = static_cast<size_t>(probe.param);
		const float before = f.bridge.fxChain().device[1].row.field[idx];
		card->graph()->onMouseDown().callback(at(centre.x, centre.y));
		card->graph()->onMouseDrag().callback(at(centre.x, centre.y - 12.0f));
		card->graph()->onMouseUp().callback(at(centre.x, centre.y - 12.0f));
		CHECK(f.bridge.fxChain().device[1].row.field[idx] != before);
	}
}
TEST_CASE("OWLTT's panel alone reaches every control") {
	// THE STANDING DIRECTIVE, ASSERTED: the expanded view carries the COMPLETE device with every
	// control, so a player never needs to toggle between card and window to edit all of the params.
	//
	// THIS REPLACES A UNION AND IT IS A STRICTLY STRONGER CLAIM. The previous case took card ∪ panel
	// and required the pair to cover the device — which is satisfied by a control that lives ONLY on
	// the card, and a control that lives only on the card is precisely the toggling the directive
	// forbids. The panel alone is the invariant now.
	//
	// AND IT CAUGHT A CONTROL WITH NO SURFACE AT ALL: `Mode`, the crossover type, which could not be
	// selected anywhere. The card's reachability table had it recorded as
	// "drawn in the head, not a knob", which was true of the folded head's READOUT and not of any
	// control — a gap written down as a decision, which is how it survived being looked at.
	FilterFixture f;
	ui::WidgetRegistry registry;
	// BUILT AS THE EDITOR BUILDS IT, so the window's header knows its device: `Enabled` is the header's
	// chip since 2026-09-26 (`ExpandedView::deviceEnableControl`), and the window is body plus header.
	std::unique_ptr<ui::ExpandedView> built =
	    ui::makeExpandedView(f.deviceHost, &f.bridge, registry, f.bridge.fxDeviceAt(1), fx::owltt::kDescriptor);
	auto* panel = dynamic_cast<ui::OwlttExpanded*>(built.get());
	REQUIRE(panel != nullptr);

	std::set<size_t> reached;
	for (const fx::owltt::Param p : panel->addressedParamsForTest()) {
		reached.insert(static_cast<size_t>(p));
	}
	REQUIRE(built->deviceEnableControl() == static_cast<size_t>(fx::owltt::Param::Enabled));
	reached.insert(built->deviceEnableControl());

	// EVERY DECLARED CONTROL, WITH NO EXCEPTIONS LIST. A gap has to be argued for by adding a
	// surface, never by adding a line to a table.
	for (size_t p = 0; p < static_cast<size_t>(fx::owltt::Param::kCount); ++p) {
		CAPTURE(p);
		CAPTURE(std::string(fx::owltt::kParamDescs[p].name));
		CHECK(reached.count(p) == 1);
	}
}
TEST_CASE("OWLTT's panel fits the editor it opens over") {
	// THE OVERLAY CLAMPS ITS CONTENT RATHER THAN SCROLLING IT (`expanded_overlay.cpp:169`), so a
	// panel taller than the editor does not get a scrollbar — it gets its bottom groups silently cut
	// off. Twenty-four knobs in one column come to roughly 900px against a 700px editor, which is why
	// the groups declare a column.
	//
	// DRAWN WHERE IT CANNOT BE REACHED, one more time, and the reason this card has earned a geometry
	// case for every surface it owns.
	FilterFixture f;
	ui::WidgetRegistry registry;
	ui::OwlttExpanded panel { f.deviceHost, registry, f.bridge.fxDeviceAt(1) };

	const visage::Point want = panel.minimumSize();
	CAPTURE(want.x);
	CAPTURE(want.y);
	CHECK(want.y <= static_cast<float>(Gui::kLogicalHeight));
	CHECK(want.x <= static_cast<float>(Gui::kLogicalWidth));

	// AND EVERY KNOB INSIDE THAT EXTENT, which the height alone does not imply: a column budget that
	// was computed right and a layout that walked wrong would still clip.
	panel.setBounds(0.0f, 0.0f, want.x, want.y);
	panel.resized();
	// EVERY CELL, NOT EVERY KNOB. Two of the twelve are `ZoneCoordBox`es now, and a walk over
	// `knobs_` would have quietly stopped checking them — the case would still pass, on ten.
	const auto& knobs = panel.cellsForTest();
	// THE GLOBAL ROW'S NINE BOXES AND THE TWO TONE CELLS, every one inside the extent.
	REQUIRE(knobs.size() == ui::OwlttExpanded::globalParams().size() + 2);
	for (size_t i = 0; i < knobs.size(); ++i) {
		CAPTURE(i);
		CHECK(knobs[i]->bounds().bottom() <= want.y);
		CHECK(knobs[i]->bounds().right() <= want.x);
	}
	// AND THE PLOT, THE TWO FADERS AND THE TOP ROW's CONTROLS.
	for (const visage::Bounds& b : { panel.graphBounds(), panel.driveBounds(), panel.outputBounds(), panel.modeBounds(),
	                                 panel.clipBounds(), panel.lookaheadBounds(), panel.keyTrackBounds(),
	                                 panel.stereoBounds() }) {
		CHECK(b.bottom() <= want.y);
		CHECK(b.right() <= want.x);
		CHECK(b.x() >= 0.0f);
	}
}
TEST_CASE("OWLTT's crossover type can be chosen") {
	// The crossover type is chosen from a dropdown (it need not be modulatable).
	//
	// TEN TYPES, so a dropdown rather than a segmented strip, and the case drives `chooseMode` rather
	// than synthesising a platform menu — the menu is visage's and testing it would be testing
	// visage. What is ours is the mapping, and the mapping has an off-by-one waiting in it: the
	// parameter runs 1..10 and `kModeNames` runs 0..9.
	FilterFixture f;
	ui::WidgetRegistry registry;
	// THE DEVICE FIRST, THEN THE PANEL OVER IT — a panel is built over a device's ID, and there is no
	// id to name until the device exists. (It was built first when it took a lane, which a device then
	// arrived at.)
	REQUIRE(f.bridge.addFxDevice(fx::owltt::kTraits.tag) == 1);
	ui::OwlttExpanded panel { f.deviceHost, registry, f.bridge.fxDeviceAt(1) };

	const size_t idx = static_cast<size_t>(fx::owltt::Param::Mode);
	for (size_t i = 0; i < std::size(fx::owltt::kModeNames); ++i) {
		CAPTURE(i);
		panel.chooseMode(i);
		// THE VALUE THE NAME MEANS, restated independently: names are 0-based, the parameter is not.
		CHECK(f.bridge.fxChain().device[1].row.field[idx]
		      == doctest::Approx(static_cast<float>(i) + 1.0f));
		// AND THE READOUT AGREES WITH THE CHOICE, which is the half a value check alone would miss —
		// a picker that sets the right number and prints the neighbour is the same lie as a control
		// that does nothing.
		CHECK(panel.modeText() == std::string(fx::owltt::kModeNames[i]));
	}
}
TEST_CASE("the window's Active toggle says what it switches: the header chip, in the device's own title row") {
	// The defect: the active toggle had no label in the expanded view, which was confusing.
	//
	// A CONTROL WITH NO SUBJECT is the defect `filter_card.cpp` names in as many words. The card's
	// head escapes it because the device's name is printed beside the square there. OWLTT's panel
	// answered it once with an "Active" caption beside its own square; since 2026-09-26 the square is
	// the WINDOW HEADER'S chip (devices had been adding their own active chips to the window
	// inconsistently), and it escapes the defect the way the card does — its subject is the title beside it.
	FilterFixture f;
	// THE DEVICE FIRST. A panel built for a slot the chain does not hold writes into a row nothing
	// reads, so every value assertion below would compare two defaults and pass or fail for reasons
	// unrelated to the control. This case's first draft omitted it and reported `false != false`.
	REQUIRE(f.bridge.addFxDevice(fx::owltt::kTraits.tag) == 1);
	ui::WidgetRegistry registry;
	ui::ExpandedOverlay overlay;
	overlay.setBounds(0, 0, 1400, 900);
	overlay.show(ui::makeExpandedView(f.deviceHost, &f.bridge, registry, f.bridge.fxDeviceAt(1),
	                                  fx::owltt::kDescriptor));
	ui::ExpandedPanel* panel = overlay.panelForTest();
	REQUIRE(panel != nullptr);

	// THE SUBJECT IS THE DEVICE'S NAME, in the same row, to the chip's left.
	const visage::Bounds sw = panel->enableBounds();
	REQUIRE(sw.width() > 0.0f);
	CHECK(overlay.contentForTest()->title() == std::string(fx::owltt::kTraits.name));
	CHECK(sw.y() >= panel->titleBounds().y());
	CHECK(sw.bottom() <= panel->titleBounds().bottom());
	CHECK(sw.x() > ui::ExpandedOverlay::kPanelPad
	                   + ui::trackedWidth(ui::font(ui::kControlSize), fx::owltt::kTraits.name, ui::kControlTracking));

	// AND IT SWITCHES the device's own enable — the value, read from the row the engine reads.
	const size_t idx = static_cast<size_t>(fx::owltt::Param::Enabled);
	const float before = f.bridge.fxChain().device[1].row.field[idx];
	panel->onMouseDown().callback(at(sw.x() + sw.width() * 0.5f, sw.y() + sw.height() * 0.5f));
	CHECK(f.bridge.fxChain().device[1].row.field[idx] == (before >= 0.5f ? 0.0f : 1.0f));
}
TEST_CASE("a device that declares an expanded view can actually be opened") {
	// The defect: no expanded view was available.
	//
	// OWLTT declared `hasExpanded`, `makeExpandedView` had an arm for it, its panel was built and
	// tested -- and no button appeared, because `expanded_views.cpp` kept a SECOND list. `buildable`
	// named only HARM, so `hasExpandedView` answered false and `OwlttCard::expandBounds` returned an
	// empty rectangle. The panel was correct, complete, and unreachable.
	//
	// AND MY OWN CASE FOR THE SPLIT DID NOT CATCH IT, WHICH IS THE PART WORTH RECORDING. It
	// constructed `OwlttExpanded` DIRECTLY, so it proved the panel HELD the nine knobs and never that
	// a hand could get to the panel. Reachability asserted one hop short of the hand is the same
	// mistake as a control drawn where it cannot be pressed -- this card's recurring bug, committed
	// this time by the test rather than by the code.
	//
	// SO THIS ASKS THE TWO PUBLIC ANSWERS TO AGREE, for every device the registry declares: if a
	// device says it has an expanded view, the factory must produce one, and if the factory produces
	// one, the button must be offered. A device added to one list and not the other fails here.
	FilterFixture f;
	ui::WidgetRegistry registry;

	for (const device::Descriptor& desc : feathers::kDevices) {
		CAPTURE(std::string(desc.traits.name));
		const bool offered = ui::hasExpandedView(desc);
		auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, f.bridge.fxDeviceAt(0), desc, nullptr);
		CHECK(offered == (view != nullptr));
		// AND A DEVICE THAT DECLARES ONE GETS ONE. Without this the pair above is satisfied by a
		// build that offers nothing at all, which is exactly the state OWLTT was in.
		if (desc.traits.hasExpanded) {
			CHECK(view != nullptr);
		}
	}
}
TEST_CASE("OWLTT's card offers the way into its panel") {
	// THE OTHER END OF THE SAME HOP: the answer above is about the registry, this is about the head.
	// `expandBounds` empty is what was actually on screen -- no mark, nothing to press.
	FilterFixture f;
	auto* lane = f.lane;
	REQUIRE(f.bridge.addFxDevice(fx::owltt::kTraits.tag) == 1);
	lane->resized();

	auto* card = dynamic_cast<ui::OwlttCard*>(lane->cardAt(1));
	REQUIRE(card != nullptr);

	const visage::Bounds ex = card->expandBounds();
	CHECK(ex.width() > 0.0f);
	CHECK(ex.height() > 0.0f);
	// INSIDE THE HEAD and clear of the enable square it sits beside, so it is a button rather than a
	// mark overlapping another button.
	CHECK(ex.bottom() <= ui::OwlttCard::kHeadHeight);
	CHECK(ex.right() <= card->enableBounds().x());

	// AND PRESSING IT ASKS. The lane binds this to the slot; the card only knows a hand pressed it,
	// so what a case can pin here is that the hook fires at all.
	bool asked = false;
	card->onExpand = [&asked] { asked = true; };
	// THE CARD, NOT THE GRAPH. The expand mark is in the HEAD, and the head is the card's — the plot
	// is a sibling child frame that never sees these coordinates. Worth being explicit about, because
	// every other press in this file's OWLTT cases now goes to the graph and this one must not.
	card->onMouseDown().callback(at(ex.x() + ex.width() * 0.5f, ex.y() + ex.height() * 0.5f));
	CHECK(asked);
}
TEST_CASE("OWLTT's panel graph is the card's graph, larger") {
	// The pop-out has a larger version of the graph, and a wider layout to make room for it.
	//
	// ONE CLASS, TWO SIZES, which is the whole reason `OwlttGraph` was extracted from `OwlttCard`. The
	// alternative was a second copy of the geometry, the hit test, the drag machine and the drawing in
	// a file that produced TEN two-places-must-agree bugs in one week -- three of them of the exact
	// form "drawn here, grabbable there". A copy was never a candidate.
	FilterFixture f;
	auto* lane = f.lane;
	REQUIRE(f.bridge.addFxDevice(fx::owltt::kTraits.tag) == 1);
	lane->resized();
	auto* card = dynamic_cast<ui::OwlttCard*>(lane->cardAt(1));
	REQUIRE(card != nullptr);

	ui::WidgetRegistry registry;
	ui::OwlttExpanded panel { f.deviceHost, registry, f.bridge.fxDeviceAt(1) };
	const visage::Point want = panel.minimumSize();
	panel.setBounds(0.0f, 0.0f, want.x, want.y);
	panel.resized();

	REQUIRE(card->graph() != nullptr);
	REQUIRE(panel.graph() != nullptr);

	// LARGER IN BOTH DIRECTIONS. The height is what buys the aiming -- at 160 the two 16px bar
	// hit-bands take 20% of the plot, at 320 they take 10% -- and the width is the "wider layout".
	CHECK(panel.graph()->height() > card->graph()->height());
	CHECK(panel.graph()->width() > card->graph()->width());

	// AND IT IS A REAL CONTROL SURFACE THERE, not a picture of one. Same class, so the handles work;
	// asserted rather than assumed, because "we reused the class" is exactly the kind of claim that
	// is true of the type and false of the wiring.
	const size_t idx = static_cast<size_t>(fx::owltt::Param::LowLevel);
	const visage::Point ring = panel.graph()->ringCentre(0, ui::OwlttGraph::Handle::LevelStrip);
	const float before = f.bridge.fxChain().device[1].row.field[idx];
	panel.graph()->onMouseDown().callback(at(ring.x, ring.y));
	panel.graph()->onMouseDrag().callback(at(ring.x, ring.y - 20.0f));
	panel.graph()->onMouseUp().callback(at(ring.x, ring.y - 20.0f));
	CHECK(f.bridge.fxChain().device[1].row.field[idx] != before);
}
TEST_CASE("the panel offers no knob for anything the graph already holds") {
	// The pop-out had many controls duplicated between knobs and graph; the graph alone is enough.
	//
	// TWELVE KNOBS WENT -- low, mid and high threshold, bandwidth, ratio and level -- because every
	// one of them is a bar or a strip on the plot above. Two controls over one value is not
	// redundancy this codebase can afford: it is two places for a hand to form a habit and one of
	// them to later disagree, which is this file's most-paid-for shape.
	//
	// ASSERTED AS AN ABSENCE, which is the only way to state it. A case that checked the knob COUNT
	// would pass again the day somebody adds a different knob, and a case that named the twelve
	// would need editing whenever a band gains a control. This asks the panel for every knob it
	// builds and requires none of them to be something the graph addresses.
	FilterFixture f;
	ui::WidgetRegistry registry;
	ui::OwlttExpanded panel { f.deviceHost, registry, f.bridge.fxDeviceAt(1) };

	static constexpr fx::owltt::Param kOnTheGraph[] = {
		fx::owltt::Param::LowThreshold,  fx::owltt::Param::LowBandwidth,
		fx::owltt::Param::LowRatio,      fx::owltt::Param::LowLevel,
		fx::owltt::Param::MidThreshold,  fx::owltt::Param::MidBandwidth,
		fx::owltt::Param::MidRatio,      fx::owltt::Param::MidLevel,
		fx::owltt::Param::HighThreshold, fx::owltt::Param::HighBandwidth,
		fx::owltt::Param::HighRatio,     fx::owltt::Param::HighLevel,
	};
	std::set<size_t> onGraph;
	for (const fx::owltt::Param p : kOnTheGraph) {
		onGraph.insert(static_cast<size_t>(p));
	}

	// THE GLOBAL ROW'S BOXES: none of them is a control the plot addresses.
	for (const auto& box : panel.globalBoxesForTest()) {
		REQUIRE(box != nullptr);
		CAPTURE(box->label());
		CHECK(onGraph.count(static_cast<size_t>(panel.paramOfGlobalForTest(box.get()))) == 0);
	}

	// AND THE TWELVE ARE STILL REACHED, through the picture. Removing a duplicate must not remove a
	// control -- that is the difference between this and the six knobs that vanished off the card.
	std::set<size_t> reached;
	for (const fx::owltt::Param p : panel.addressedParamsForTest()) {
		reached.insert(static_cast<size_t>(p));
	}
	for (const size_t p : onGraph) {
		CAPTURE(p);
		CHECK(reached.count(p) == 1);
	}
}
TEST_CASE("a press through the real tree reaches OWLTT's graph") {
	// The defect: none of the bars or graph elements worked in either the card or the panel, while
	// every knob on both did.
	//
	// EVERY OWLTT CASE IN THIS FILE INVOKES THE HANDLER DIRECTLY. `card->graph()->onMouseDown()` calls
	// the callback; it does not ask the window which frame a press at that pixel would REACH. So the
	// whole suite stayed green through a change that moved the plot into a child frame, because the
	// suite was testing the handler and the bug is in the routing. One hop short of the hand -- the
	// same defect I wrote a case about this morning, committed again in the same file.
	FilterFixture f;
	auto* lane = f.lane;
	REQUIRE(f.bridge.addFxDevice(fx::owltt::kTraits.tag) == 1);
	lane->resized();
	// SCROLL TO IT, which is the lane's own stated contract: "every card reachable from some scroll
	// position" (fx_lane.h:219). If that holds, the plot being partly outside at scroll 0 is the lane
	// working as designed and the complaint is discoverability, not reachability.
	lane->setScroll(lane->maxScroll());
	lane->resized();
	{
		// THE ASSERTION THAT SEPARATES "the lane is full" FROM "the control is unreachable".
		//
		// MEASURED at scroll 0 with the filter card open above it: the plot spans window 488..648 and
		// the lane ends at 586, so its lower 62px sit OUTSIDE the lane -- and the footer's knob tray,
		// a later sibling covering those pixels, takes the press. At the default threshold the lower
		// bar lands at ~588, two pixels the wrong side of that line, which is why the bars could be seen
		// and not dragged.
		//
		// The lane's own contract is "every card reachable from some scroll position"
		// (`fx_lane.h:219`), and this pins it: scrolled to, the plot answers.
		auto* d = dynamic_cast<ui::OwlttCard*>(lane->cardAt(1));
		REQUIRE(d != nullptr);
		auto* g = d->graph();
		REQUIRE(g != nullptr);
		const visage::Point p =
		    g->positionInWindow() + g->ringCentre(0, ui::OwlttGraph::Handle::ThresholdBar);
		CHECK(f.root->frameAtPoint(p - f.root->positionInWindow())
		      == static_cast<visage::Frame*>(g));
	}

	// FOLD THE FILTER ABOVE IT so the OWLTT card sits entirely inside the lane. Without this the card
	// overflows its parent and the footer's knob tray -- a later sibling covering the same pixels --
	// wins the hit test, which is a REAL bug and a different one from the routing this case is about.
	if (auto* filter = dynamic_cast<ui::FilterCard*>(lane->cardAt(0))) {
		filter->setFolded(true);
		lane->resized();
	}
	auto* card = dynamic_cast<ui::OwlttCard*>(lane->cardAt(1));
	REQUIRE(card != nullptr);
	auto* graph = card->graph();
	REQUIRE(graph != nullptr);
	REQUIRE(graph->width() > 0.0f);

	// THE QUESTION THE HANDLER CANNOT ANSWER: what does the tree say is at this pixel? Asked of the
	// ROOT, at the graph's real place in the window, which is what a hand does.
	const visage::Point inGraph = graph->ringCentre(0, ui::OwlttGraph::Handle::ThresholdBar);
	const visage::Point inWindow = graph->positionInWindow() + inGraph;
	visage::Frame* landed = f.root->frameAtPoint(inWindow - f.root->positionInWindow());
	CHECK(landed == static_cast<visage::Frame*>(graph));

	// AND THE VALUE MOVES WHEN THE PRESS TRAVELS THAT ROAD rather than being handed to the callback.
	const size_t idx = static_cast<size_t>(fx::owltt::Param::LowThreshold);
	const float before = f.bridge.fxChain().device[1].row.field[idx];
	dragThroughRoot(f.root, inWindow, inWindow + visage::Point { 0.0f, -20.0f });
	CHECK(f.bridge.fxChain().device[1].row.field[idx] != before);
}
TEST_CASE("a press through the real tree reaches the panel's graph") {
	// The defect: none of the bars or graph elements worked in EITHER the card or the pop-out. The card's half
	// turned out to be the card overflowing its lane; the panel is an overlay and cannot have that
	// problem, so this asks the same question of the other surface.
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	REQUIRE(f.bridge.addFxDevice(fx::owltt::kTraits.tag) == 1);
	lane->resized();

	// OPEN IT THE WAY A HAND DOES -- through the lane's own hook, which is what the editor installs.
	REQUIRE(lane->onExpandDevice != nullptr);
	lane->onExpandDevice(1);

	auto* panel = findInTree<ui::OwlttExpanded>(f.root);
	REQUIRE(panel != nullptr);
	auto* graph = panel->graph();
	REQUIRE(graph != nullptr);
	REQUIRE(graph->width() > 0.0f);

	const visage::Point inGraph = graph->ringCentre(0, ui::OwlttGraph::Handle::ThresholdBar);
	const visage::Point inWindow = graph->positionInWindow() + inGraph;
	visage::Frame* landed = f.root->frameAtPoint(inWindow - f.root->positionInWindow());
	CHECK(landed == static_cast<visage::Frame*>(graph));

	const size_t idx = static_cast<size_t>(fx::owltt::Param::LowThreshold);
	const float before = f.bridge.fxChain().device[1].row.field[idx];
	dragThroughRoot(f.root, inWindow, inWindow + visage::Point { 0.0f, -20.0f });
	CHECK(f.bridge.fxChain().device[1].row.field[idx] != before);
}
TEST_CASE("the panel's crossovers drag on the graph") {
	// The crossovers are draggable on the panel's graph.
	//
	// A LANE THEY OWN OUTRIGHT, rather than a priority rule. A crossover line runs the full height of
	// the plot and so crosses every bar, every strip and the GR meter; ordering it against them would
	// leave one of them unreachable somewhere, which is this file's oldest lesson
	// (`harm_scope.cpp:400`) and one it has now paid for three times. Nothing else is drawn in the
	// lane, so there is no ordering question to get wrong -- and the bars are CONFINED below it
	// rather than merely outranked, because HARM recorded what the other choice costs.
	FilterFixture f;
	REQUIRE(f.bridge.addFxDevice(fx::owltt::kTraits.tag) == 1);
	ui::WidgetRegistry registry;
	ui::OwlttExpanded panel { f.deviceHost, registry, f.bridge.fxDeviceAt(1) };
	const visage::Point want = panel.minimumSize();
	panel.setBounds(0.0f, 0.0f, want.x, want.y);
	panel.resized();

	auto* graph = panel.graph();
	REQUIRE(graph != nullptr);
	REQUIRE(graph->crossoversDraggable());
	// THE CARD'S DOES NOT OFFER THEM, which is the other half of "on the panel graph".
	ui::OwlttGraph plain { f.deviceHost, f.bridge.fxDeviceAt(1) };
	CHECK_FALSE(plain.crossoversDraggable());

	const size_t low = static_cast<size_t>(fx::owltt::Param::LowCrossover);
	const float before = f.bridge.fxChain().device[1].row.field[low];

	// DRAG IT RIGHT along the axis. The handle is asked for rather than computed -- the same rule the
	// rings taught: a case that restates a layout is the same defect as code that does.
	const visage::Point handle = graph->crossoverHandle(0);
	const float toX = handle.x + 60.0f;
	graph->onMouseDown().callback(at(handle.x, handle.y));
	graph->onMouseDrag().callback(at(toX, handle.y));
	graph->onMouseUp().callback(at(toX, handle.y));

	const float after = f.bridge.fxChain().device[1].row.field[low];
	CAPTURE(before);
	CAPTURE(after);
	// HIGHER, because right is up the spectrum -- a direction check, not just a difference one.
	CHECK(after > before);

	// AND THE VALUE THE AXIS IMPLIES, restated independently: the split is a POSITION, so the pointer
	// maps back through the log sweep 20Hz..20kHz across the plot. This is what catches a drag that
	// moves the right control the wrong distance -- the partial effect a difference check is blind to.
	const visage::Bounds box = graph->plot();
	const float t = std::clamp((toX - box.x()) / box.width(), 0.0f, 1.0f);
	const float expected = 20.0f * std::pow(2.0f, t * std::log2(20000.0f / 20.0f));
	CHECK(after == doctest::Approx(std::clamp(expected, 20.0f, 2000.0f)).epsilon(0.02));

	// AND THE HANDLE FOLLOWED THE VALUE, which is what makes it a handle rather than a slider that
	// happens to sit there: `bandEdge` read forwards must land where the drag put it.
	CHECK(graph->crossoverHandle(0).x == doctest::Approx(toX).epsilon(0.05));
}
TEST_CASE("the crossover lane does not steal a bar's pixels") {
	// THE HAZARD A RESERVED LANE CREATES, and the reason HARM's scope is cited rather than copied:
	// its notch rail sits at the top of its plot and `harm_scope.cpp:324` records that a grip pushed
	// INTO the rail became unreachable at the end of its travel -- which is where a hand goes to
	// bring it back.
	//
	// So the bars are confined BELOW the lane rather than losing to it, and this drives a threshold
	// to the top of its sweep and requires the bar to still answer there.
	FilterFixture f;
	REQUIRE(f.bridge.addFxDevice(fx::owltt::kTraits.tag) == 1);
	ui::WidgetRegistry registry;
	ui::OwlttExpanded panel { f.deviceHost, registry, f.bridge.fxDeviceAt(1) };
	const visage::Point want = panel.minimumSize();
	panel.setBounds(0.0f, 0.0f, want.x, want.y);
	panel.resized();
	auto* graph = panel.graph();
	REQUIRE(graph != nullptr);

	f.bridge.setDeviceControl(f.bridge.fxDeviceAt(1), static_cast<size_t>(fx::owltt::Param::LowThreshold), 1.0f);

	const visage::Point bar = graph->ringCentre(0, ui::OwlttGraph::Handle::ThresholdBar);
	// CLEAR OF THE LANE at the very top of its travel.
	CHECK(bar.y >= graph->rail().bottom());

	// AND STILL GRABBABLE THERE. A press at the bar must reach the bar, not a crossover.
	const size_t idx = static_cast<size_t>(fx::owltt::Param::LowThreshold);
	const float before = f.bridge.fxChain().device[1].row.field[idx];
	graph->onMouseDown().callback(at(bar.x, bar.y));
	graph->onMouseDrag().callback(at(bar.x, bar.y + 25.0f));
	graph->onMouseUp().callback(at(bar.x, bar.y + 25.0f));
	CHECK(f.bridge.fxChain().device[1].row.field[idx] < before);
}
TEST_CASE("OWLTT's bars drag in the state the editor opens in") {
	// The defect: the bars still did not drag.
	//
	// EVERY OTHER CASE HERE CHEATS. They fold the filter card, or scroll the lane, or call the
	// handler directly -- each of which makes the plot reachable and none of which is what a hand
	// finds on opening the editor. This one takes the lane exactly as it comes up.
	FilterFixture f;
	auto* lane = f.lane;
	// ADDED THE WAY A HAND ADDS ONE — through the lane's own verb, not by writing the chain behind its
	// back. That is the difference this case turns on: `addDevice` scrolls the new card into view and
	// `bridge.addFxDevice` cannot, because the bridge has no idea a lane exists.
	REQUIRE(lane->addDevice(fx::owltt::kTraits.tag) == 1);

	auto* card = dynamic_cast<ui::OwlttCard*>(lane->cardAt(1));
	REQUIRE(card != nullptr);
	auto* graph = card->graph();
	REQUIRE(graph != nullptr);

	const float laneBottom = lane->positionInWindow().y + lane->height();
	const float plotTop = graph->positionInWindow().y;
	CAPTURE(plotTop);
	CAPTURE(plotTop + graph->height());
	CAPTURE(laneBottom);

	// THE WHOLE PLOT INSIDE THE LANE. Not most of it: a bar sits where its VALUE puts it, so any part
	// of the plot outside the lane is a range of values whose control cannot be touched -- and the
	// default threshold lands in exactly that range, which is what a hand kept hitting.
	CHECK(plotTop + graph->height() <= laneBottom);

	// AND THE DEFAULT BAR ANSWERS A PRESS ROUTED THROUGH THE TREE.
	const visage::Point p =
	    graph->positionInWindow() + graph->ringCentre(0, ui::OwlttGraph::Handle::ThresholdBar);
	CHECK(f.root->frameAtPoint(p - f.root->positionInWindow())
	      == static_cast<visage::Frame*>(graph));

	const size_t idx = static_cast<size_t>(fx::owltt::Param::LowThreshold);
	const float before = f.bridge.fxChain().device[1].row.field[idx];
	dragThroughRoot(f.root, p, p + visage::Point { 0.0f, -20.0f });
	CHECK(f.bridge.fxChain().device[1].row.field[idx] != before);
}
TEST_CASE("a state notify reaches a watcher's children, not just the watcher") {
	// The hypothesis: the graph was not redrawing. It was right, and it
	// is the whole of "bars still don't drag" -- the drag WORKED and the picture never repainted, so
	// the control was live and nothing on screen said so. The meters died the same way.
	//
	// `FxLane::ensureCard` subscribes THE CARD. `Frame::redraw()` marks ONE frame; `redrawAll()`
	// walks the subtree. While the plot was drawn by `OwlttCard::draw` that distinction cost nothing.
	// The moment it became a child frame, the notify reached the card, repainted its head and knobs,
	// and stopped one level above the only part of the card anybody was looking at.
	//
	// A REPAINT IS UNOBSERVABLE HEADLESSLY, so what is asserted is the COUNT the notification itself
	// reports -- a number produced BY the delivery, which a counter standing beside it could not
	// promise. That is why `notify()` counts FRAMES rather than watchers: counted as watchers, this
	// bug is invisible, because the number of watchers never changed.
	FilterFixture f;
	auto* lane = f.lane;
	REQUIRE(lane->addDevice(fx::owltt::kTraits.tag) == 1);

	auto* card = dynamic_cast<ui::OwlttCard*>(lane->cardAt(1));
	REQUIRE(card != nullptr);
	REQUIRE(card->graph() != nullptr);

	const ui::FrameWatchers* watchers = f.gui.stateWatchers();
	REQUIRE(watchers != nullptr);
	REQUIRE(watchers->watches(card)); // the card is the subscriber; the plot is not

	// THE PLOT IS TOLD ANYWAY, because it is under a frame that was. Asserted as "more frames than
	// watchers", which is exactly the difference a subtree walk makes and is false for `redraw()`.
	const size_t told = watchers->notify();
	CAPTURE(told);
	CAPTURE(watchers->count());
	CHECK(told > watchers->count());

	// AND SPECIFICALLY, the card's own subtree is more than one frame -- the head, three knobs and
	// the plot. Without this the check above passes on any watcher anywhere having a child.
	CHECK(ui::FrameWatchers::redrawTree(card) > 1);
}
TEST_CASE("the graph shows the threshold the device is using, not the one the band stores") {
	// The global threshold is linked to the graph's bars, as in the firmware: moving the threshold
	// knob changes the threshold shown in the graph, because the graph must match the thresholds the
	// device actually uses so that they can be set by eye.
	//
	// THE HONESTY RULE, APPLIED TO A PICTURE. `owltt.cpp` shifts every band by the global's distance
	// from neutral, so a bar drawn at the STORED per-band value is drawn at a number the compressor
	// does not use -- turning the global moved the sound and left the plot where it was. A graph you
	// cannot set by eye is lying about the thing it is a graph of.
	FilterFixture f;
	auto* lane = f.lane;
	REQUIRE(lane->addDevice(fx::owltt::kTraits.tag) == 1);
	auto* card = dynamic_cast<ui::OwlttCard*>(lane->cardAt(1));
	REQUIRE(card != nullptr);
	auto* graph = card->graph();
	REQUIRE(graph != nullptr);

	const float atNeutral = graph->ringCentre(0, ui::OwlttGraph::Handle::ThresholdBar).y;

	// MOVE THE GLOBAL ALONE. Not one per-band control is touched.
	f.bridge.setDeviceControl(f.bridge.fxDeviceAt(1), static_cast<size_t>(fx::owltt::Param::Threshold), 0.8f);
	const float raised = graph->ringCentre(0, ui::OwlttGraph::Handle::ThresholdBar).y;

	CAPTURE(atNeutral);
	CAPTURE(raised);
	// HIGHER ON SCREEN, because a higher threshold is nearer the top of the plot. A direction check,
	// not a difference one -- "it moved" would pass for a bar that moved the wrong way.
	CHECK(raised < atNeutral);

	// AND IT IS THE DEVICE'S OWN NUMBER, restated independently: effective = band + (global - 0.5),
	// clamped. This is the half a difference check cannot see -- a bar that responds to the global by
	// the wrong AMOUNT still moves.
	const fx::owltt::EffectiveBand eff = graph->effective(0);
	const float band =
	    f.bridge.fxChain().device[1].row.field[static_cast<size_t>(fx::owltt::Param::LowThreshold)];
	CHECK(eff.threshold == doctest::Approx(std::clamp(band + (0.8f - 0.5f), 0.0f, 1.0f)));
}
TEST_CASE("dragging a bar puts it under the pointer whatever the global is doing") {
	// THE OTHER HALF, AND THE ONE THAT MAKES "set them by eye" TRUE. If the picture shows effective
	// values but the drag writes the pointer's height straight into the band, the bar leaps away from
	// the hand by exactly the global's displacement the moment the global is off neutral.
	//
	// So the drag solves in EFFECTIVE terms and converts back at the last step -- the same inversion
	// discipline the bandwidth gap and the frequency axis each needed, and each cost a bug when it
	// was written forwards instead.
	FilterFixture f;
	auto* lane = f.lane;
	REQUIRE(lane->addDevice(fx::owltt::kTraits.tag) == 1);
	auto* card = dynamic_cast<ui::OwlttCard*>(lane->cardAt(1));
	REQUIRE(card != nullptr);
	auto* graph = card->graph();
	REQUIRE(graph != nullptr);

	// A GLOBAL WELL OFF NEUTRAL, which is the only condition under which the two roads differ.
	f.bridge.setDeviceControl(f.bridge.fxDeviceAt(1), static_cast<size_t>(fx::owltt::Param::Threshold), 0.75f);

	const visage::Point from = graph->ringCentre(0, ui::OwlttGraph::Handle::ThresholdBar);
	const float toY = from.y + 30.0f; // drag it down

	graph->onMouseDown().callback(at(from.x, from.y));
	graph->onMouseDrag().callback(at(from.x, toY));
	graph->onMouseUp().callback(at(from.x, toY));

	// THE BAR IS WHERE THE HAND LEFT IT. Asked of the card, which is the whole claim: the handle
	// follows the pointer rather than the pointer's value minus a shift nobody can see.
	const float landed = graph->ringCentre(0, ui::OwlttGraph::Handle::ThresholdBar).y;
	CAPTURE(from.y);
	CAPTURE(toY);
	CAPTURE(landed);
	CHECK(landed == doctest::Approx(toY).epsilon(0.05));
}
TEST_CASE("the panel asks for no more canvas than it fills") {
	// The defect: the graph was too big and the knobs were clustered together, with lots of blank
	// space in the pop-out canvas.
	//
	// MEASURED BEFORE IT WAS BELIEVED: the panel asked for 1240x660 -- the whole editor -- with a
	// 1212x320 plot and twelve knobs occupying 302px, leaving 938px of empty canvas beside them.
	//
	// AND THE BLANK SPACE WAS NOT A SPACING BUG. `bodyWidth` spanned four columns unconditionally
	// because the panel USED to have four groups, and three of them left when the duplicate per-band
	// knobs did. A number that was right when it was written and became wrong when something else
	// changed -- the same shape as the reachability table recording six knobs as absent, and the same
	// reason it survived being looked at: nothing connected the width to what was in it.
	//
	// SO THE ASSERTION IS THE RELATION, NOT THE NUMBER. A case pinning 460 would be the literal all
	// over again; this one requires the panel to ask for no more than its contents fill, whatever
	// they come to.
	FilterFixture f;
	ui::WidgetRegistry registry;
	ui::OwlttExpanded panel { f.deviceHost, registry, f.bridge.fxDeviceAt(1) };
	const visage::Point want = panel.minimumSize();
	panel.setBounds(0.0f, 0.0f, want.x, want.y);
	panel.resized();

	// THE KNOBS DECIDE THE WIDTH, AND THE GRAPH MUST BE EXCLUDED FROM THE QUESTION.
	//
	// THE FIRST DRAFT INCLUDED IT AND WAS VACUOUS -- caught by its own known-bad passing. The plot is
	// laid out as `width() - 2 * kPad`, so it STRETCHES to whatever the panel asks for: measured
	// against it, "no wasted width" is true by construction at any width, including the 1240 that
	// prompted this. A guard whose subject resizes itself to satisfy it is not a guard.
	//
	// So the rightmost thing that has a size of its OWN is what the extent has to be justified by.
	float rightmost = 0.0f;
	float lowest = panel.graphBounds().bottom();
	REQUIRE_FALSE(panel.cellsForTest().empty());
	for (const visage::Frame* cell : panel.cellsForTest()) {
		REQUIRE(cell != nullptr);
		rightmost = std::max(rightmost, cell->bounds().right());
		lowest = std::max(lowest, cell->bounds().bottom());
	}
	CAPTURE(want.x);
	CAPTURE(want.y);
	CAPTURE(rightmost);
	CAPTURE(lowest);

	// NOTHING BEYOND THE PADDING, in either direction. One pad of slack is the layout's own margin;
	// anything more is canvas the panel reserved and did not use.
	CHECK(want.x - rightmost <= ui::OwlttExpanded::kPad + 1.0f);
	CHECK(want.y - lowest <= ui::OwlttExpanded::kPad + 1.0f);

	// AND NOTHING OUTSIDE IT EITHER, which the first pair does not imply: a layout that overflowed
	// would also leave no slack.
	CHECK(rightmost <= want.x);
	CHECK(lowest <= want.y);

	// THE PICTURE IS STILL THE LARGER SURFACE. Cutting the graph back must not turn the panel into
	// the knob list it exists to avoid -- it is the reason a hand opened it.
	CHECK(panel.graphBounds().height() > Knob::columnHeight(ui::OwlttExpanded::kKnobDiameter));
	// THE DRIVE (backlog NB) stands at the plot's left and the OUTPUT at its right, each taking its width out of the picture's,
	// so the three (and the two level meters between them and the picture) together span the body.
	CHECK(panel.graphBounds().width()
	      >= want.x - 2.0f * ui::OwlttExpanded::kPad - 2.0f * (ui::OwlttExpanded::kDriveWidth + ui::OwlttExpanded::kDriveGap) - 2.0f * (ui::OwlttLevelMeter::kWidth + ui::OwlttExpanded::kLevelGap) - 0.5f);
	CHECK(panel.outputMeterBounds().right() - panel.driveBounds().x() >= want.x - 2.0f * ui::OwlttExpanded::kPad - 0.5f);
}
TEST_CASE("a bar and its handle move together when the global moves") {
	// The defect: moving the global threshold moved the graph's handles but not the bars they are
	// attached to.
	//
	// THREE PLACES COMPUTED A BAR'S HEIGHT AND ONE WAS MISSED. `ringCentre` and `hitTestThreshold`
	// were converted to effective values; `draw` was not. So the handle followed the global and the
	// line it sits on stayed put -- a picture disagreeing with itself, which is worse than either
	// half being wrong: it tells a hand the control is in two places.
	//
	// AND IT IS WHY `barYs` EXISTS RATHER THAN THE THIRD COPY BEING CORRECTED. Correcting it would
	// have left three copies of one fact, and the next conversion would have missed one of them.
	//
	// THE CASE ASKS THE DRAWING AND THE HANDLE THE SAME QUESTION, which is the only way to state
	// "attached to": a drawn line is not observable headlessly, so what is checked is that the
	// position the drawing uses and the position the handle uses are ONE number, at several values of
	// the global.
	FilterFixture f;
	auto* lane = f.lane;
	REQUIRE(lane->addDevice(fx::owltt::kTraits.tag) == 1);
	auto* card = dynamic_cast<ui::OwlttCard*>(lane->cardAt(1));
	REQUIRE(card != nullptr);
	auto* graph = card->graph();
	REQUIRE(graph != nullptr);

	float previous = -1.0f;
	bool everMoved = false;
	for (const float global : { 0.2f, 0.5f, 0.8f }) {
		CAPTURE(global);
		f.bridge.setDeviceControl(f.bridge.fxDeviceAt(1), static_cast<size_t>(fx::owltt::Param::Threshold), global);

		const ui::OwlttGraph::BarYs ys = graph->barYs(0);
		const visage::Point lowerRing = graph->ringCentre(0, ui::OwlttGraph::Handle::ThresholdBar);
		const visage::Point upperRing = graph->ringCentre(0, ui::OwlttGraph::Handle::BandwidthBar);

		// THE HANDLE IS ON THE LINE. `ringYInside` may hold a ring a radius inside the plot at the
		// ends of the sweep, so the comparison allows exactly that and no more -- otherwise this
		// would fail for the clamp rather than for the bug.
		CHECK(std::abs(lowerRing.y - ys.threshold) <= ui::OwlttGraph::kRingRadius + 0.01f);
		CHECK(std::abs(upperRing.y - ys.bandwidth) <= ui::OwlttGraph::kRingRadius + 0.01f);

		// AND NOT VACUOUS: the line really does move with the global, so the agreement above is
		// between two things that are both responding rather than two that are both stuck.
		if (previous >= 0.0f && std::abs(ys.threshold - previous) > 0.5f) {
			everMoved = true;
		}
		previous = ys.threshold;
	}
	CHECK(everMoved);
}
TEST_CASE("the graph answers a hover with the device's own words") {
	// The defect: the graph had no tooltips, and the crossover frequency was shown nowhere.
	//
	// `tooltipAt` SHIPPED WITH NO READER. The text was declared, the hit test reused, and nothing
	// ever called it -- a road built and never driven, which is the same shape as `travelToValue` in
	// the zone knob beside it. A field is earned by a reader on day one; backlog 16 is about exactly
	// this and it happened anyway.
	//
	// DRAWN BY THE GRAPH, NOT HANDED TO THE EDITOR'S TRAY, which is the whole point and is a design call:
	// "goal is to not couple to the feathers plugin so tooltips might still work when the effects are
	// standalone." A graph that asked a `Notifier` to show its text would know the Feathers window
	// exists. This one asks the DEVICE what a point means and draws the answer on itself.
	FilterFixture f;
	auto* lane = f.lane;
	REQUIRE(lane->addDevice(fx::owltt::kTraits.tag) == 1);
	auto* card = dynamic_cast<ui::OwlttCard*>(lane->cardAt(1));
	REQUIRE(card != nullptr);
	auto* graph = card->graph();
	REQUIRE(graph != nullptr);

	// EVERY HANDLE HAS WORDS, asked at the place the handle actually is rather than at a guessed
	// point -- the lesson this file has paid for three times.
	using Handle = ui::OwlttGraph::Handle;
	for (const Handle h : { Handle::BandwidthBar, Handle::ThresholdBar, Handle::RatioStrip,
		                    Handle::LevelStrip }) {
		const visage::Point at = graph->ringCentre(0, h);
		const char* text = graph->tooltipAt(at);
		CAPTURE(static_cast<int>(h));
		REQUIRE(text != nullptr);
		// AND THEY SAY SOMETHING. An empty string is a tooltip that renders as a blank box, which is
		// worse than none -- it claims there is an explanation and withholds it.
		CHECK(std::string(text).size() > 8);
	}

	// AND NOTHING OUTSIDE THE PLOT CLAIMS AN EXPLANATION.
	CHECK(graph->tooltipAt({ -5.0f, -5.0f }) == nullptr);
}
TEST_CASE("a crossover frequency is printed the way a player reads one") {
	// The crossover frequency was shown nowhere. A multiband compressor is DEFINED by where it
	// splits, and that number was on no surface: the knobs print a bare "%.2f" through
	// `device::Unit::Number` and the graph drew its two lines with no label at all.
	//
	// THE FORMATTING IS ASSERTED BECAUSE IT IS THE POINT. "200.00" is not a frequency a player reads;
	// it is a float that happens to be one.
	CHECK(ui::OwlttGraph::frequencyText(200.0f) == "200 Hz");
	CHECK(ui::OwlttGraph::frequencyText(2000.0f) == "2.0 kHz");
	CHECK(ui::OwlttGraph::frequencyText(20.0f) == "20 Hz");
	CHECK(ui::OwlttGraph::frequencyText(12500.0f) == "12.5 kHz");
	// THE BOUNDARY, because 999.5 rounding to "1000 Hz" rather than "1.0 kHz" is the kind of thing a
	// case is for.
	CHECK(ui::OwlttGraph::frequencyText(999.0f) == "999 Hz");
}
TEST_CASE("clicking a modulator while the OWLTT panel is open") {
	// The defect: a crash when clicking a background modulator while OWLTT's pop-out had focus.
	//
	// THE STACK WAS CLEAN AND NAMED THE GESTURE: `ModPanel::focusOrArm` +915, from the panel's own
	// mouse-down lambda, through `WindowX11::processEvent`. `focusOrArm` calls `mod_.focus()`, which
	// notifies every state watcher -- and since the subtree fix that walk now descends into each
	// watcher's CHILDREN. The open panel is a watcher.
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	REQUIRE(lane->addDevice(fx::owltt::kTraits.tag) == 1);

	// OPEN THE POPOUT THE WAY A HAND DOES.
	REQUIRE(lane->onExpandDevice != nullptr);
	lane->onExpandDevice(1);
	REQUIRE(findInTree<ui::OwlttExpanded>(f.root) != nullptr);

	// FIRST, THE GESTURES THE LOG SHOWS. `[FEATHERS-PLOT] press ... hit=1`, `hit=2`, `hit=13`,
	// `hit=14` in a 432x220 plot -- the two crossover handles and two level strips on the PANEL's
	// graph, dragged in the seconds before the crash. A crash after a drag is not a crash after a
	// click, and the difference is usually a gesture left open.
	auto* panel = findInTree<ui::OwlttExpanded>(f.root);
	REQUIRE(panel != nullptr);
	auto* pg = panel->graph();
	REQUIRE(pg != nullptr);
	{
		const visage::Point c = pg->crossoverHandle(0);
		pg->onMouseDown().callback(at(c.x, c.y));
		pg->onMouseDrag().callback(at(c.x + 20.0f, c.y));
		pg->onMouseUp().callback(at(c.x + 20.0f, c.y));
		const visage::Point lv = pg->ringCentre(2, ui::OwlttGraph::Handle::LevelStrip);
		pg->onMouseDown().callback(at(lv.x, lv.y));
		pg->onMouseDrag().callback(at(lv.x, lv.y - 10.0f));
		// NO MOUSE-UP, DELIBERATELY. A release that lands outside the plugin window never reaches the
		// frame, so a gesture can still be open when the next press arrives -- which is the state a
		// case that always releases can never be in.
	}

	// AND NOW CLICK A MODULATOR BEHIND IT, through the real tree.
	//
	// SOURCE 2, NOT 0. The focused source defaults to 0, so arming 0 and then asserting it is 0 is a
	// test that passes whether or not the click arrived -- which is exactly what the first draft of
	// this case did.
	// A SOURCE THE POPOUT DOES NOT COVER, and not 0: the window is as wide as its plot wants, so which arrows it hides depends
	// on its width. The first of sources 2, 1, 3 whose arrow the window leaves open is clicked.
	backplane::Source chosen = backplane::Source::core(2);
	visage::Point arrow {};
	for (const uint32_t k : { 2u, 1u, 3u }) {
		const backplane::Source candidate = backplane::Source::core(k);
		const visage::Point at = f.panel->positionInWindow()
		                         + visage::Point { f.panel->arrowBounds(candidate).x() + 8.0f,
			                                       f.panel->arrowBounds(candidate).y() + 8.0f };
		chosen = candidate;
		arrow = at;
		if (f.root->frameAtPoint(at - f.root->positionInWindow()) == static_cast<visage::Frame*>(f.panel)) {
			break;
		}
	}
	// THE CLICK REALLY REACHES THE PANEL with the popout open -- asserted, because "no crash" from a
	// click that landed on the overlay instead would be a case passing for the wrong reason.
	CHECK(f.root->frameAtPoint(arrow - f.root->positionInWindow())
	      == static_cast<visage::Frame*>(f.panel));
	dragThroughRoot(f.root, arrow, arrow);

	// GETTING HERE IS THE ASSERTION. A use-after-free in the notify walk takes the process down
	// rather than returning a wrong answer, so what a case can check is that the gesture completes --
	// and then that the state it was supposed to change actually changed, so a crash-free no-op does
	// not pass for a working one.
	CHECK(f.gui.modForTest() != nullptr);
	CHECK(f.gui.modForTest()->source() == chosen);

	// AND THE NOTIFY WALK ITSELF, driven directly, with the panel still open.
	const ui::FrameWatchers* watchers = f.gui.stateWatchers();
	REQUIRE(watchers != nullptr);
	CHECK(watchers->notify() > 0);
}
TEST_CASE("a swallowed device's control spends ONE undo step per gesture, and none for a gesture that moved nothing") {
	// BACKLOG 18. `FxControl::beginGesture`/`endGesture` were both `{}`, so Ctrl-Z reached nothing on
	// the filter card, HARM's card, HARM's expanded panel or the scope's grips. Not a surface problem
	// and no surface could fix it: every road that edits a swallowed control goes through this class.
	//
	// THE PARAMETER ROAD RECORDS TOO, SINCE BACKLOG JB (2026-10-11). `ParamControl`'s pair used to record
	// nothing, on the reasoning that a host parameter has a DAW behind it that owns undo — but the editor
	// walks its OWN history on Ctrl/Cmd+Z, so a knob drag was skipped by it. Both roads own their steps now.
	HarmFixture f;
	namespace hrm = feathers::fx::harm;
	ui::EditHistory history;
	feathers::ui::ModState mod;

	ui::FxControl level { f.bridge, f.device, hrm::kDescriptor,
		                  static_cast<size_t>(hrm::Param::Level) };
	level.setModulation(&mod, &history);
	level.setValue(0.7);
	f.bridge.refresh();
	const double before = level.value();
	REQUIRE(before == doctest::Approx(0.7));

	SUBCASE("one gesture through several values is ONE entry, and undo returns the value it started at") {
		// THE USER'S UNIT IS THE DRAG. Dragging a control through five intermediate values and hitting
		// Ctrl-Z returns to where the drag STARTED, rather than walking back through each landing —
		// the design call recorded in backlog 18 and the reason the bracket sits around the whole drag
		// rather than around each setValue.
		level.beginGesture();
		for (const double v : { 0.5, 0.4, 0.3, 0.25 }) {
			level.setValue(v);
			f.bridge.refresh();
		}
		level.endGesture();

		CHECK(history.length() == 1);
		REQUIRE(history.canUndo());

		// THE VALUE, ASSERTED AS A VALUE. A count alone would pass on a record that put back the wrong
		// number, and an intermediate landing is exactly the wrong number available here.
		history.undo();
		f.bridge.refresh();
		CHECK(level.value() == doctest::Approx(before));
		// NOT VACUOUS: it really was somewhere else before the undo, so the assertion above is not
		// satisfied by a gesture that never wrote.
		CHECK(std::abs(0.25 - before) > 0.1);

		// AND REDO WALKS FORWARD TO WHERE THE DRAG ENDED, not to an intermediate landing.
		history.redo();
		f.bridge.refresh();
		CHECK(level.value() == doctest::Approx(0.25));
	}

	SUBCASE("a gesture that changed nothing spends NO undo step") {
		// THE RULE THE AMOUNT ROAD ALREADY FOLLOWS, and the value box after it: a click that turns
		// nothing must not cost an undo. `Knob::commit` enforces the same thing one level up by opening
		// the bracket on the first value that MOVES rather than on the press, so this is belt and
		// braces — but the belt is the one being tested here, because a widget that brackets on press
		// would otherwise leave an empty step behind.
		level.beginGesture();
		level.endGesture();
		CHECK(history.length() == 0);
		CHECK_FALSE(history.canUndo());
	}

	SUBCASE("no history wired is not a crash, it is simply no step") {
		// A CARD BUILT WITHOUT A HISTORY IS A REAL CONFIGURATION — `setModulation(state, nullptr)`
		// appears in this very file — so the null road has to be walked rather than assumed.
		ui::FxControl bare { f.bridge, f.device, hrm::kDescriptor,
			                 static_cast<size_t>(hrm::Param::Level) };
		bare.beginGesture();
		bare.setValue(0.2);
		f.bridge.refresh();
		bare.endGesture();
		CHECK(bare.value() == doctest::Approx(0.2));
		CHECK(history.length() == 0);
	}
}
TEST_CASE("Esc leaves modulation assign mode, through the real editor") {
	// BACKLOG 20, the PERMANENT exit: Esc leaves mod mode. It pairs with the Alt bypass, because Alt
	// is the momentary escape and this is the permanent one.
	//
	// WHY IT WAS A TRAP AND NOT AN INCONVENIENCE: the only two callers of `mod.arm(...)` are
	// `ModPanel::focusOrArm` and `SourceGrid`, and both are a click on the SOURCE'S OWN control. So
	// the only way out was to find and click the exact thing you armed — a mode a hand can enter by
	// accident and could not leave without knowing how it got in.
	//
	// DRIVEN THROUGH THE EDITOR RATHER THAN THE LAYER, because the layer's own case can only prove
	// the callback is offered the key. That the callback DISARMS is a different claim and this is
	// where it is made.
	ModFixture f;
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);
	ui::ExpandedOverlay* overlay = f.gui.expandedForTest();
	REQUIRE(overlay != nullptr);
	REQUIRE_FALSE(overlay->open());

	const visage::KeyEvent escape { visage::KeyCode::Escape, 0, true };

	SUBCASE("armed, Esc, disarmed — and the key is reported consumed") {
		mod->focus(backplane::Source::core(0));
		mod->arm(true);
		REQUIRE(mod->armed());

		CHECK(overlay->onKeyPress().callback(escape));
		CHECK_FALSE(mod->armed());

		// AND THE FOCUS SURVIVES IT. Leaving the mode is not forgetting which source the hand was
		// pointing — a second arm should return to the same source rather than to whatever was first.
		CHECK(mod->source() == backplane::Source::core(0));
	}

	SUBCASE("Esc with nothing armed is NOT this editor's key") {
		// A MODE THAT IS OFF HAS NO EXIT TO SPEND. Swallowing Esc here would take it from whatever the
		// host meant it for, for the whole life of the editor — the same failure the overlay's own
		// case has guarded since it was written, arriving by a new road.
		REQUIRE_FALSE(mod->armed());
		CHECK_FALSE(overlay->onKeyPress().callback(escape));
		CHECK_FALSE(mod->armed());
	}
}
TEST_CASE("Esc leaves assign mode by the road a keystroke actually travels") {
	// THE CASE ABOVE CALLS THE HANDLER; THIS ONE ASKS WHETHER A KEY CAN REACH IT. On 2026-09-04 Esc
	// still did not exit modulation assign mode in the plugin — a feature that was written, tested and
	// shipped, because the handler sat on a frame the keystroke never visits.
	//
	// WHY IT COULD NOT FIRE. `WindowEventHandler::handleKeyDown` starts at the keyboard-focused frame
	// and walks up through PARENTS; `handleMouseDown` sets that focus to the first ancestor of the
	// clicked frame that `acceptsKeystrokes()`, and to NOTHING if there is none. `ExpandedOverlay` is
	// a SIBLING of the mod panel under the editor root, so arming a source could only ever put the
	// focus somewhere the overlay is not on the way up from — and before this change the editor root
	// accepted no keystrokes at all, so arming left the focus null and Esc was delivered nowhere.
	//
	// A HANDLER'S CALLBACK IS NOT A ROAD. That is the whole distinction this case exists for, and it
	// is the same shape as the render rules in CLAUDE.md from the other end: invoking the callback
	// answers "does the handler work", and the question was "does the key arrive".
	ModFixture f;
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);

	const visage::Point arrow = f.armPoint(backplane::Source::core(0));
	const visage::KeyEvent escape { visage::KeyCode::Escape, 0, true };

	SUBCASE("armed by a real click, Esc through the real routing, disarmed") {
		f.arm(backplane::Source::core(0));
		REQUIRE(mod->armed());
		REQUIRE(mod->source() == backplane::Source::core(0));

		// WHERE THE FOCUS IS AFTER THE ARMING CLICK — the press that put the editor in this mode is
		// the press that decides where a key goes next, so it is that press and no other that is
		// asked. A null here is the defect itself: no frame is focused, and `handleKeyDown` returns
		// before it visits anybody.
		visage::Frame* focused = keyboardFocusAfterPress(f.root, arrow);
		REQUIRE(focused != nullptr);

		CHECK(keyFromFocus(focused, escape));
		CHECK_FALSE(mod->armed());

		// AND THE FOCUSED SOURCE SURVIVES IT — leaving the mode is not forgetting which source the
		// hand was pointing.
		CHECK(mod->source() == backplane::Source::core(0));
	}

	SUBCASE("and it routes nothing") {
		// THE PROPERTY A CARELESS FIX BREAKS. An exit that left the mode by pretending the hand had
		// clicked something would pass every assertion above and write an amount the user did not ask
		// for. So the amount table is read before and after and compared as VALUES — not a count,
		// not a picture: `ModRoutes::amount` is the thing the engine reads.
		//
		// AND THE TABLE IS SEEDED FIRST, because two EMPTY arrays compare equal whatever the exit
		// did to them: an exit that CLEARED the routes would pass a vacuous version of this. With a
		// route already in it the comparison has both directions to fail in.
		f.bridge.beginRouteGesture();
		const backplane::Source seedSource = backplane::Source::core(1);
		const backplane::Destination seedDest = backplane::Destination::core(3);
		f.bridge.setRouteAmount(seedSource, seedDest, 0.5f);
		f.bridge.endRouteGesture();
		const std::vector<backplane::Route> before = routeListOf(f.bridge);
		REQUIRE(f.bridge.routeAmount(seedSource, seedDest) == doctest::Approx(0.5f));

		f.arm(backplane::Source::core(0));
		REQUIRE(mod->armed());
		visage::Frame* focused = keyboardFocusAfterPress(f.root, arrow);
		REQUIRE(focused != nullptr);
		CHECK(keyFromFocus(focused, escape));
		REQUIRE_FALSE(mod->armed());

		CHECK(routeListOf(f.bridge) == before);
		// THE SEEDED ROUTE SURVIVED — the clearing half, named rather than left to the list
		// comparison, so a failure says which way it went.
		CHECK(f.bridge.routeAmount(seedSource, seedDest) == doctest::Approx(0.5f));
		// AND THE ARMED SOURCE PICKED UP NOTHING — its whole row, because "routed nothing" is a claim
		// about every destination it could have reached and not only about the one a guess would name.
		size_t fromArmed = 0;
		f.bridge.forEachRoute([&](const backplane::Route& r) {
			if (r.source == backplane::Source::core(0)) {
				++fromArmed;
			}
		});
		CHECK(fromArmed == 0);
	}

	SUBCASE("Esc with nothing armed is still not this editor's key") {
		// A press SOMEWHERE, so the focus is whatever an ordinary click leaves — the mode is off, and
		// the key must travel the whole chain and be consumed by nobody. Swallowing it here would
		// take it from the host for the editor's whole life, by a road that did not exist when the
		// overlay's own case first guarded against exactly that.
		f.arm(backplane::Source::core(0));
		f.arm(backplane::Source::core(0)); // the same arrow again disarms
		REQUIRE_FALSE(mod->armed());

		visage::Frame* focused = keyboardFocusAfterPress(f.root, arrow);
		REQUIRE(focused != nullptr);
		CHECK_FALSE(keyFromFocus(focused, escape));
		CHECK_FALSE(mod->armed());
	}
}
TEST_CASE("backlog NN: Esc closes the most recently opened thing, whatever kind it is, through the real editor") {
	// Esc treats everything it can close equally and closes the most recently opened. The defect: with a window open
	// and a modulator assign armed "behind it", Esc closed the window, not the assign. The overlay took Esc first
	// whenever it held the focus, so the window beat the arm
	// whatever the order. Now one stack per editor, in the order things opened; each key below travels the road a
	// keystroke does — from the focus the last press left, up through the accepting ancestors.
	ModFixture f;
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);
	ui::ExpandedOverlay* overlay = f.gui.expandedForTest();
	REQUIRE(overlay != nullptr);
	REQUIRE_FALSE(overlay->open());
	REQUIRE_FALSE(mod->armed());

	const backplane::Source source = backplane::Source::core(0);
	const visage::KeyEvent escape { visage::KeyCode::Escape, 0, true };
	// A HAND'S ARM, BY THE API A PRESS ON THE ARROW REACHES — a panel opened first may sit over the arrow. The press
	// itself is used where nothing covers it (`f.arm`).
	const auto armByHand = [&](backplane::Source s) {
		mod->focus(s);
		mod->arm(true);
	};
	// THE FOCUS THE ARMING PRESS LEAVES — the root, since the arrow is under no other acceptor.
	visage::Frame* const focused = keyboardFocusAfterPress(f.root, f.armPoint(source));
	REQUIRE(focused != nullptr);

	SUBCASE("a window, then an arm: Esc disarms and the window stays; a second Esc closes the window") {
		overlay->show(makeStub({ 420.0f, 300.0f }, "Window"));
		REQUIRE(overlay->open());
		armByHand(source);
		REQUIRE(mod->armed());

		CHECK(keyFromFocus(focused, escape));
		CHECK_FALSE(mod->armed());
		CHECK(overlay->open());
		// AND FROM THE OVERLAY'S OWN DOOR TOO — the focus a press on the window leaves. Same stack, same order.
		CHECK(keyFromFocus(overlay, escape));
		CHECK_FALSE(overlay->open());
		CHECK(overlay->lastDismissalForTest() == ui::ExpandedOverlay::Dismissal::Escape);
		// LEAVING THE MODE IS NOT FORGETTING THE SOURCE.
		CHECK(mod->source() == source);
		// NOTHING LEFT: the key is the host's.
		CHECK_FALSE(keyFromFocus(focused, escape));
	}

	SUBCASE("an arm, then a window: Esc closes the window and the arm stays; a second Esc disarms") {
		f.arm(source);
		REQUIRE(mod->armed());
		overlay->show(makeStub({ 420.0f, 300.0f }, "Window"));
		REQUIRE(overlay->open());

		CHECK(keyFromFocus(focused, escape));
		CHECK_FALSE(overlay->open());
		CHECK(mod->armed());
		CHECK(keyFromFocus(focused, escape));
		CHECK_FALSE(mod->armed());
		CHECK_FALSE(keyFromFocus(focused, escape));
	}

	SUBCASE("re-arming another source after a window counts as opening the arm again") {
		f.arm(source);
		overlay->show(makeStub({ 420.0f, 300.0f }, "Window"));
		armByHand(backplane::Source::core(1));
		REQUIRE(mod->armed());
		CHECK(keyFromFocus(focused, escape));
		CHECK_FALSE(mod->armed());
		CHECK(overlay->open());
	}

	SUBCASE("an item closed by other means leaves the stack: the window's ✕, then Esc goes to the arm under it") {
		f.arm(source);
		overlay->show(makeStub({ 420.0f, 300.0f }, "Window"));
		overlay->dismiss(); // not Esc
		REQUIRE_FALSE(overlay->open());
		REQUIRE(mod->armed());
		CHECK(keyFromFocus(focused, escape));
		CHECK_FALSE(mod->armed());
		CHECK_FALSE(keyFromFocus(focused, escape));
	}

	SUBCASE("a menu over a window closes first, from the menu's own focus; the next Esc closes the window") {
		overlay->show(makeStub({ 420.0f, 300.0f }, "Window"));
		ui::ExpandedPanel* window = overlay->panelForTest();
		REQUIRE(window != nullptr);
		visage::PopupMenu menu;
		menu.addOption(1, "One");
		menu.addOption(2, "Two");
		int cancels = 0;
		menu.onCancel() = [&] { ++cancels; };
		REQUIRE(ui::showMenuAt(menu, *window, { 0.0f, 0.0f, 40.0f, 20.0f }));
		REQUIRE(ui::menuOpenFor(*window));
		visage::PopupMenuFrame* popup = nullptr;
		for (visage::Frame* child : window->topParentFrame()->children()) {
			if (auto* p = dynamic_cast<visage::PopupMenuFrame*>(child); p != nullptr) {
				popup = p;
			}
		}
		REQUIRE(popup != nullptr);

		// THE MENU HOLDS THE FOCUS, so the key starts there.
		CHECK(keyFromFocus(popup, escape));
		CHECK_FALSE(ui::menuOpenFor(*window));
		CHECK(cancels == 1);
		CHECK(overlay->open());
		CHECK(keyFromFocus(focused, escape));
		CHECK_FALSE(overlay->open());
	}

	SUBCASE("the settings screen opened over an arm closes first") {
		auto* header = findInTree<ui::HeaderBar>(f.root);
		REQUIRE(header != nullptr);
		REQUIRE(static_cast<bool>(header->onSettings));
		ui::SettingsScreen* settings = f.gui.settingsForTest();
		REQUIRE(settings != nullptr);
		f.arm(source);
		header->onSettings();
		REQUIRE(settings->isVisible());
		CHECK(keyFromFocus(focused, escape));
		CHECK_FALSE(settings->isVisible());
		CHECK(mod->armed());
		CHECK(keyFromFocus(focused, escape));
		CHECK_FALSE(mod->armed());
	}

	overlay->dismiss();
	mod->disarmTree();
}
TEST_CASE("the MSEG time base opens a menu rather than dragging") {
	// The time base is a dropdown. Twenty-seven divisions since dotted and triplet arrived, and
	// dragging a value box through them to reach "1/8." gets slower the more useful the list gets.
	//
	// A RANGE WANTS A DRAG; A VOCABULARY WANTS A MENU. That is the distinction the picker hook draws,
	// and it is why the hook is on `ValueBox` rather than a special case in this card: any stepped
	// `Unit::Choice` box is a list of names.
	ModFixture f;
	REQUIRE(f.bridge.addRackDevice(mod::mseg::kTraits.tag) < kMaxRackSlots);
	f.panel->resized();

	auto* card = findInTree<ui::MsegCard>(f.root);
	REQUIRE(card != nullptr);

	// THE TIME BASE BOX, FOUND BY WHAT IT CONTROLS rather than by its position in the row -- the
	// layout is a declared list and a case that hardcoded index 0 would pass until somebody reordered
	// it.
	const auto& layout = ui::MsegCard::layout();
	size_t timeBase = layout.size();
	for (size_t i = 0; i < layout.size(); ++i) {
		if (layout[i] == mod::mseg::Param::TimeBase) {
			timeBase = i;
		}
	}
	REQUIRE(timeBase < layout.size());

	// IT OPENS A MENU AND THE OTHERS DO NOT. Both halves: a card where every box opened a menu would
	// satisfy the first check and have broken the three controls that are genuinely ranges.
	//
	// **ASKED OF `choiceMenuAvailable`, NOT OF `hasPicker`, AND THE SWAP IS THE FIX IT PINS.** This
	// read `hasPicker()`, which was true here because `MsegCard` set one on this single box — so the
	// case was really asserting that a card had remembered to wire a widget. Automod's four choice
	// cells were identical in kind and nobody had wired them, and the case could not tell. The menu
	// is `ValueBox`'s own now, decided from the DESCRIPTOR, so this asks the property instead of the
	// wiring and reads the same on every card in the project: every value box that is a multivalue
	// selector is a dropdown, not 'drag over the box to select'.
	for (size_t i = 0; i < layout.size(); ++i) {
		const ui::ValueBox* box = card->boxForTest(i);
		REQUIRE(box != nullptr);
		CAPTURE(i);
		CHECK(box->choiceMenuAvailable() == (i == timeBase));
		// AND NO CARD WIRES IT BY HAND ANY MORE, which is what stops the two roads drifting apart —
		// a box with a picker set here would be taking a different list from the one every other
		// Choice box in the editor takes.
		CHECK_FALSE(box->hasPicker());
	}

	// **THE MARK, AND IT MOVES WITH THE VALUE** — the dropdown marks the selected time base with an 'X'.
	// Asked of `choiceLabelAt` rather than of a photograph, because a
	// `visage::PopupMenu` is built and shown in one breath and hands nothing back to read.
	{
		const ui::ValueBox* base = card->boxForTest(timeBase);
		REQUIRE(base != nullptr);
		const size_t entries = base->entryCount();
		REQUIRE(entries == device::kDivisionCount);

		const auto markedRow = [&] {
			size_t marked = entries;
			size_t marks = 0;
			for (size_t i = 0; i < entries; ++i) {
				if (base->choiceLabelAt(i).rfind("X", 0) == 0) {
					marked = i;
					++marks;
				}
			}
			// EXACTLY ONE, never merely at-least-one: a mark on every row would satisfy "the current
			// row is marked" and say nothing.
			CHECK(marks == 1);
			return marked;
		};

		for (const size_t pick : { size_t { 0 }, device::kDefaultDivision, entries - 1 }) {
			CAPTURE(pick);
			base->control().setValue(base->control().desc().min + static_cast<double>(pick));
			CHECK(markedRow() == pick);
			// AND THE NAME IS STILL THERE BEHIND THE MARK, so the row says which division it is
			// rather than only that it is the one you are on.
			CHECK(base->choiceLabelAt(pick).find(device::kDivisionNames[pick]) != std::string::npos);
		}

		// THE UNMARKED ROWS KEEP THE SAME LEADING CELL, which is what puts the names in a column
		// instead of stepping the current row one glyph to the right of its neighbours.
		base->control().setValue(base->control().desc().min);
		CHECK(base->choiceLabelAt(1).find(device::kDivisionNames[1]) != std::string::npos);
		CHECK(base->choiceLabelAt(1).rfind(" ", 0) == 0);
	}
}

