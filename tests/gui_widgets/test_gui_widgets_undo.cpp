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

// UNDO FROM EVERYWHERE (backlog JB, 2026-10-11). Every road below changed the patch and recorded NOTHING into
// the editor's one history, so the Ctrl/Cmd+Z the editor has had since dff05099 walked back an OLDER edit and
// skipped it. Each case edits through the road a hand takes, asserts the step is ONE step, undoes and asserts
// the value BY VALUE, and redoes. A parameter's undo is also asserted to reach the HOST as a gesture — Begin,
// the value, End — because the DAW and its automation have to see an undo as an edit of the parameter.

#include "fixtures.h"

#include "fx/automod/automod_types.h"
#include "fx/eq/eq_types.h"
#include "fx/phaser/phaser_types.h"
#include "fx/shaper/shaper_types.h"
#include "plugin/ui/automod_pad.h"
#include "plugin/ui/eq_card.h"
#include "plugin/ui/eq_controls.h"
#include "plugin/ui/param_control.h"
#include "plugin/ui/param_edit.h"
#include "plugin/audition.h"
#include "plugin/ui/relative_macro.h"
#include "plugin/ui/gamma_control.h"
#include "plugin/ui/mod_state.h"
#include "plugin/ui/phaser_pad.h"
#include "plugin/ui/shaper_pad.h"
#include "plugin/ui/spectrum_panel.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/value_box.h"
#include "plugin/ui/windowed_expanded.h"

namespace {

clap_id idOf(ParamId id) {
	return static_cast<clap_id>(id);
}

/// **THE HOST HEARD IT AS ONE EDIT OF THE PARAMETER**: among what the editor posted since the last drain, a
/// GestureBegin on `id`, then a Value on `id` equal to `value`, then a GestureEnd on `id`, in that order.
bool hostSawGesture(const std::vector<UiParamEvent>& events, clap_id id, double value) {
	size_t i = 0;
	const auto next = [&](UiParamEvent::Kind kind, bool checkValue) {
		for (; i < events.size(); ++i) {
			const UiParamEvent& e = events[i];
			if (e.id == id && e.kind == kind && (!checkValue || e.value == value)) {
				++i;
				return true;
			}
		}
		return false;
	};
	return next(UiParamEvent::Kind::GestureBegin, false) && next(UiParamEvent::Kind::Value, true)
	       && next(UiParamEvent::Kind::GestureEnd, false);
}

/// A value of `id` other than the one it holds — the next step of a stepped one, or a quarter-range away.
double otherValueOf(const ParamBridge& bridge, ParamId id) {
	const ParamDesc& desc = descOf(id);
	const double now = bridge.value(idOf(id));
	if ((desc.flags & CLAP_PARAM_IS_STEPPED) != 0) {
		return now + 1.0 <= desc.max ? now + 1.0 : desc.min;
	}
	const double quarter = (desc.max - desc.min) * 0.25;
	return now + quarter <= desc.max ? now + quarter : now - quarter;
}

/// The undo and redo of one parameter step, asserted by value and at the host: undo puts `before` back and the
/// host hears a gesture carrying it, redo puts `after` back and the host hears that.
void undoRedoParam(ModFixture& f, ParamId id, double before, double after) {
	ui::EditHistory& history = *f.gui.history();
	(void)drain(f.bridge);
	REQUIRE(history.canUndo());
	history.undo();
	CHECK(f.bridge.value(idOf(id)) == before);
	CHECK(hostSawGesture(drain(f.bridge), idOf(id), before));
	REQUIRE(history.canRedo());
	history.redo();
	CHECK(f.bridge.value(idOf(id)) == after);
	CHECK(hostSawGesture(drain(f.bridge), idOf(id), after));
}

} // namespace

//------------------------------------------------------------------------------------------------//
// A1 — A HAND ON A FEATHERS PARAMETER                                                              //
//------------------------------------------------------------------------------------------------//

TEST_CASE("undo, A1: a value gesture on a Feathers parameter is ONE step, and undo reaches the host as a gesture") {
	ModFixture f;
	ui::EditHistory& history = *f.gui.history();
	// A CONTROL HANDED NO HISTORY OF ITS OWN — the editor's reaches it through the bridge, which is the point:
	// the sites that recorded nothing were controls nobody had handed one.
	ui::ParamControl control { f.bridge, &descOf(ParamId::VoiceLevel) };
	REQUIRE(control.recordsInto() == &history);
	const double before = f.bridge.value(idOf(ParamId::VoiceLevel));
	const double after = otherValueOf(f.bridge, ParamId::VoiceLevel);
	const size_t steps = history.length();

	control.beginGesture();
	control.setValue((before + after) * 0.5); // a drag passes through values on its way
	control.setValue(after);
	control.endGesture();
	CHECK(history.length() == steps + 1);
	CHECK(f.bridge.value(idOf(ParamId::VoiceLevel)) == after);
	undoRedoParam(f, ParamId::VoiceLevel, before, after);

	// A GESTURE THAT MOVED NOTHING RECORDS NOTHING.
	const size_t now = history.length();
	control.beginGesture();
	control.setValue(after);
	control.endGesture();
	CHECK(history.length() == now);
}

TEST_CASE("undo, A1: a knob dragged in the assembled editor is one step, and one Ctrl+Z puts it back") {
	ModFixture f;
	ui::EditHistory& history = *f.gui.history();
	ui::Knob* knob = f.knobFor(ParamId::VoiceLevel);
	REQUIRE(knob != nullptr);
	const double before = f.bridge.value(idOf(ParamId::VoiceLevel));
	const size_t steps = history.length();
	const visage::Point face = f.faceCentre(knob);
	dragThroughRoot(f.root, face, { face.x, face.y - 40.0f });
	const double after = f.bridge.value(idOf(ParamId::VoiceLevel));
	REQUIRE(after != before);
	CHECK(history.length() == steps + 1);

	REQUIRE(keyFromFocus(f.root, visage::KeyEvent { visage::KeyCode::Z, visage::kModifierRegCtrl, true }));
	CHECK(f.bridge.value(idOf(ParamId::VoiceLevel)) == before);
	REQUIRE(keyFromFocus(f.root, visage::KeyEvent { visage::KeyCode::Z, visage::kModifierRegCtrl | visage::kModifierShift, true }));
	CHECK(f.bridge.value(idOf(ParamId::VoiceLevel)) == after);
}

//------------------------------------------------------------------------------------------------//
// A2 — ONE-SHOT WRITES: A MENU PICK, A STEP, A SEGMENT, A SOUND'S SWITCH                           //
//------------------------------------------------------------------------------------------------//

TEST_CASE("undo, A2: the engine menu, Explore's four menus, a stepper, a segment and a sound's switch are a step each") {
	ModFixture f;
	ui::EditHistory& history = *f.gui.history();

	// Each road, its parameter, and the act; the value it lands on is read back, so the case asks the road and
	// not a number it chose.
	const auto road = [&](const char* name, ParamId id, const std::function<void()>& act) {
		CAPTURE(name);
		const double before = f.bridge.value(idOf(id));
		const size_t steps = history.length();
		act();
		const double after = f.bridge.value(idOf(id));
		REQUIRE(after != before);
		CHECK(history.length() == steps + 1);
		undoRedoParam(f, id, before, after);
	};
	const auto nextIndex = [&](ParamId id, size_t count) {
		return (static_cast<size_t>(std::lround(f.bridge.value(idOf(id)))) + 1) % count;
	};

	ui::EngineRail* header = findInTree<ui::EngineRail>(f.root);
	REQUIRE(header != nullptr);
	road("engine", ParamId::OscType,
	     [&] { header->selectEngine(static_cast<int>(nextIndex(ParamId::OscType, kOscTypeNames.size()))); });

	ui::ExploreStage* explore = findInTree<ui::ExploreStage>(f.root);
	REQUIRE(explore != nullptr);
	road("warp", ParamId::WarpMode, [&] {
		explore->chooseWarpMode(nextIndex(ParamId::WarpMode, static_cast<size_t>(feathers::WarpMode::kCount)));
	});
	road("bow", ParamId::ResonateBowMode,
	     [&] { explore->chooseBowMode(nextIndex(ParamId::ResonateBowMode, kResonateBowModeNames.size())); });
	road("additive shape", ParamId::AdditiveShape,
	     [&] { explore->chooseAdditiveShape(nextIndex(ParamId::AdditiveShape, kAdditiveShapeNames.size())); });
	road("resonate model", ParamId::ResonateModel,
	     [&] { explore->chooseModel(nextIndex(ParamId::ResonateModel, kResonateModelNames.size())); });

	ui::WidgetRegistry registry;
	ui::Stepper stepper { f.bridge, registry, descOf(ParamId::TransposeOct), 1.0 };
	road("stepper", ParamId::TransposeOct, [&] { stepper.nudge(1); });

	ui::SegmentedControl segments { f.bridge, registry, descOf(ParamId::VoiceMode),
		                            std::span<const char* const>(kVoiceModeNames) };
	road("segment", ParamId::VoiceMode,
	     [&] { segments.select(static_cast<int>(nextIndex(ParamId::VoiceMode, kVoiceModeNames.size()))); });

	ui::SoundStack* stack = findInTree<ui::SoundStack>(f.root);
	REQUIRE(stack != nullptr);
	const ParamId enable = static_cast<ParamId>(feathers::soundParam(0, feathers::SoundField::Enable));
	road("sound switch", enable, [&] {
		const visage::Bounds b = stack->switchBoundsForTest(0);
		const visage::Point p { b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f };
		stack->processMouseDown(at(p.x, p.y));
		stack->processMouseUp(at(p.x, p.y));
	});

	// A PICK OF WHAT IS ALREADY THERE IS NO STEP.
	const size_t steps = history.length();
	header->selectEngine(static_cast<int>(std::lround(f.bridge.value(idOf(ParamId::OscType)))));
	CHECK(history.length() == steps);
}

//------------------------------------------------------------------------------------------------//
// A3 — THE PERFORM PUCK                                                                            //
//------------------------------------------------------------------------------------------------//

TEST_CASE("undo, A3: a Perform puck drag is ONE step over every parameter it moved, and undo reaches the host") {
	StageFixture f;
	ui::EditHistory history;
	f.bridge.setEditHistory(&history); // the editor does this for its own; this fixture has no editor
	const clap_id x = idOf(ParamId::PosX);
	const clap_id y = idOf(ParamId::PosY);
	const double x0 = f.bridge.value(x);
	const double y0 = f.bridge.value(y);

	f.stage.processMouseDown(at(300.0f, 200.0f));
	f.stage.processMouseDrag(at(420.0f, 260.0f));
	f.stage.processMouseUp(at(420.0f, 260.0f));
	const double x1 = f.bridge.value(x);
	const double y1 = f.bridge.value(y);
	REQUIRE((x1 != x0 || y1 != y0));
	CHECK(history.length() == 1);

	(void)drain(f.bridge);
	history.undo();
	CHECK(f.bridge.value(x) == x0);
	CHECK(f.bridge.value(y) == y0);
	const std::vector<UiParamEvent> back = drain(f.bridge);
	CHECK((x1 == x0 || hostSawGesture(back, x, x0)));
	CHECK((y1 == y0 || hostSawGesture(back, y, y0)));
	history.redo();
	CHECK(f.bridge.value(x) == x1);
	CHECK(f.bridge.value(y) == y1);
	f.bridge.setEditHistory(nullptr);
}

//------------------------------------------------------------------------------------------------//
// B4 — THE SPECTRAL DRAWER'S STORED SWITCHES AND FLATTEN                                           //
//------------------------------------------------------------------------------------------------//

namespace {

void click(visage::Frame& frame) {
	const float x = std::max(1.0f, frame.width() * 0.5f);
	const float y = std::max(1.0f, frame.height() * 0.5f);
	frame.processMouseDown(at(x, y));
	frame.processMouseUp(at(x, y));
}

} // namespace

TEST_CASE("undo, B4: the Spectral master switch, a sound's Curve switch and Flatten are a step each, put back by value") {
	ModFixture f;
	ui::EditHistory& history = *f.gui.history();
	ui::SpectrumTab* tab = findInTree<ui::SpectrumTab>(f.root);
	REQUIRE(tab != nullptr);
	// ON ADDITIVE, the one engine the panel opens on, then opened as a hand opens it (Gui::Impl::toggleSpectrumPanel).
	ParamSet additive;
	additive.setBase(static_cast<clap_id>(ParamId::OscType), static_cast<double>(OscType::Additive));
	f.bridge.publish(additive);
	f.bridge.refresh();
	tab->clickLabelForTest();
	ui::SpectrumPanel* panel = f.gui.spectrumPanelForTest();
	REQUIRE(panel != nullptr);

	SUBCASE("the master switch") {
		tab->sync(true); // live, as on the Additive engine — the chip is greyed elsewhere and takes no press
		const bool before = f.bridge.spectralOn();
		const size_t steps = history.length();
		click(tab->masterForTest());
		REQUIRE(f.bridge.spectralOn() != before);
		CHECK(history.length() == steps + 1);
		history.undo();
		CHECK(f.bridge.spectralOn() == before);
		history.redo();
		CHECK(f.bridge.spectralOn() != before);
	}
	SUBCASE("a sound's curve switch, by slot") {
		constexpr size_t kSlot = 2;
		const bool before = f.bridge.spectralCurve(kSlot).on;
		const size_t steps = history.length();
		ui::SwitchChip& chip = panel->curveSwitchForTest(kSlot);
		click(chip);
		REQUIRE(f.bridge.spectralCurve(kSlot).on != before);
		CHECK(history.length() == steps + 1);
		history.undo();
		CHECK(f.bridge.spectralCurve(kSlot).on == before);
		history.redo();
		CHECK(f.bridge.spectralCurve(kSlot).on != before);
	}
	SUBCASE("Flatten") {
		const size_t slot = 0;
		f.bridge.setSpectralCurveBand(slot, 3, 0.25f); // a curve worth flattening, put there without a hand
		f.bridge.setSpectralCurveBand(slot, 7, 1.75f);
		const spectral::DrawnCurve before = f.bridge.spectralCurve(slot);
		const size_t steps = history.length();
		panel->flattenForTest();
		const spectral::DrawnCurve after = f.bridge.spectralCurve(slot);
		REQUIRE_FALSE(after == before);
		CHECK(after.gain[3] == 1.0f);
		CHECK(history.length() == steps + 1);
		history.undo();
		CHECK(f.bridge.spectralCurve(slot).gain[3] == 0.25f);
		CHECK(f.bridge.spectralCurve(slot).gain[7] == 1.75f);
		CHECK(f.bridge.spectralCurve(slot) == before);
		history.redo();
		CHECK(f.bridge.spectralCurve(slot) == after);
	}
}

//------------------------------------------------------------------------------------------------//
// B5 — A MACRO'S NAME, POLARITY AND TYPE; A MACROS DEVICE'S OUTPUT LABEL                           //
//------------------------------------------------------------------------------------------------//

TEST_CASE("undo, B5: a macro's rename, polarity and type are a step each, and undo puts each back by value") {
	ModFixture f;
	ui::EditHistory& history = *f.gui.history();
	constexpr size_t kMacro = 1;
	ui::MacroSlot* slot = f.panel->macroSlotForTest(kMacro);
	REQUIRE(slot != nullptr);

	SUBCASE("rename") {
		const std::string before = f.bridge.macroName(kMacro);
		REQUIRE_FALSE(f.bridge.macroRenamed(kMacro));
		const size_t steps = history.length();
		slot->naming().rename("Wobble");
		CHECK(f.bridge.macroName(kMacro) == "Wobble");
		CHECK(history.length() == steps + 1);
		history.undo();
		CHECK(f.bridge.macroName(kMacro) == before);
		CHECK_FALSE(f.bridge.macroRenamed(kMacro));
		history.redo();
		CHECK(f.bridge.macroName(kMacro) == "Wobble");
	}
	SUBCASE("polarity") {
		const bool before = f.bridge.macroBipolar(kMacro);
		const size_t steps = history.length();
		slot->menuHooks().writeBipolar(!before);
		CHECK(f.bridge.macroBipolar(kMacro) == !before);
		CHECK(history.length() == steps + 1);
		history.undo();
		CHECK(f.bridge.macroBipolar(kMacro) == before);
		history.redo();
		CHECK(f.bridge.macroBipolar(kMacro) == !before);
	}
	SUBCASE("type") {
		const device::MacroType before = f.bridge.macroType(kMacro);
		REQUIRE(before == device::MacroType::Knob);
		const size_t steps = history.length();
		slot->menuHooks().writeType(device::MacroType::Latch);
		CHECK(f.bridge.macroType(kMacro) == device::MacroType::Latch);
		CHECK(history.length() == steps + 1);
		history.undo();
		CHECK(f.bridge.macroType(kMacro) == before);
		history.redo();
		CHECK(f.bridge.macroType(kMacro) == device::MacroType::Latch);
	}
	SUBCASE("a rename to the name it has is no step") {
		const size_t steps = history.length();
		slot->naming().rename(f.bridge.macroName(kMacro));
		CHECK(history.length() == steps);
	}
}

TEST_CASE("undo, B5: a MACROS device's output relabel is one step, by device and output") {
	namespace mac = feathers::mod::macros;
	ModFixture f;
	ui::EditHistory& history = *f.gui.history();
	const size_t rackSlot = f.bridge.addRackDevice(mac::kTraits.tag);
	REQUIRE(rackSlot < feathers::kMaxRackSlots);
	const DeviceInstanceId device = f.bridge.rackDeviceAt(rackSlot);
	f.bridge.refresh();
	f.panel->resized();
	auto* card = const_cast<ui::MacrosCard*>(dynamic_cast<const ui::MacrosCard*>(f.panel->cardFrameForTest(rackSlot)));
	REQUIRE(card != nullptr);
	const std::string before = f.bridge.outputLabel(device, 2);
	const size_t steps = history.length();
	card->slot(2).naming().rename("Filter sweep");
	CHECK(f.bridge.outputLabel(device, 2) == "Filter sweep");
	CHECK(history.length() == steps + 1);
	history.undo();
	CHECK(f.bridge.outputLabel(device, 2) == before);
	CHECK_FALSE(f.bridge.outputRelabelled(device, 2));
	history.redo();
	CHECK(f.bridge.outputLabel(device, 2) == "Filter sweep");
}

//------------------------------------------------------------------------------------------------//
// B6 — THE EQ'S PER-BAND CHANNEL                                                                   //
//------------------------------------------------------------------------------------------------//

TEST_CASE("undo, B6: an EQ band's Channel pick, on the card the lane builds, is one step put back by value") {
	namespace eq = feathers::fx::eq;
	FilterFixture f;
	ui::EditHistory& history = *f.gui.history();
	const size_t slot = f.bridge.addFxDevice(eq::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	f.lane->resized();
	auto* card = dynamic_cast<ui::EqCard*>(f.lane->cardAt(slot));
	REQUIRE(card != nullptr);
	ui::ValueBox* box = card->controlsForTest()->channelBoxForTest(1);
	REQUIRE(box != nullptr);
	device::Modulatable& channel = box->control();
	const DeviceInstanceId device = f.bridge.fxDeviceAt(slot);
	const size_t field = eq::extraIndex(eq::channelField(1));
	const float before = f.deviceHost.fieldAt(device, field);
	const size_t steps = history.length();

	channel.beginGesture();
	channel.setValue(2.0); // Side
	channel.endGesture();
	const float after = f.deviceHost.fieldAt(device, field);
	REQUIRE(after != before);
	CHECK(history.length() == steps + 1);
	history.undo();
	CHECK(f.deviceHost.fieldAt(device, field) == before);
	history.redo();
	CHECK(f.deviceHost.fieldAt(device, field) == after);
}

//------------------------------------------------------------------------------------------------//
// B7 — THE FOLDED-TRAVEL PADS: SHAPER, PHASER, AUTOMOD                                             //
//------------------------------------------------------------------------------------------------//

namespace {

/// A pad drag on `pad` is ONE step, and undo and redo put the whole travel `[first, first + count)` back by value.
void padDragIsOneStep(ModFixture& f, visage::Frame& pad, DeviceInstanceId device, size_t first, size_t count) {
	ui::EditHistory& history = *f.gui.history();
	const auto travel = [&] {
		std::vector<float> out(count);
		for (size_t i = 0; i < count; ++i) {
			out[i] = f.deviceHost.fieldAt(device, first + i);
		}
		return out;
	};
	REQUIRE(pad.width() > 1.0f);
	REQUIRE(pad.height() > 1.0f);
	const std::vector<float> before = travel();
	const size_t steps = history.length();
	pad.processMouseDown(at(pad.width() * 0.3f, pad.height() * 0.3f));
	pad.processMouseDrag(at(pad.width() * 0.6f, pad.height() * 0.55f));
	pad.processMouseDrag(at(pad.width() * 0.7f, pad.height() * 0.65f));
	pad.processMouseUp(at(pad.width() * 0.7f, pad.height() * 0.65f));
	const std::vector<float> after = travel();
	REQUIRE(after != before);
	CHECK(history.length() == steps + 1);
	history.undo();
	CHECK(travel() == before);
	history.redo();
	CHECK(travel() == after);
}

template <class Panel>
Panel* openPanel(ModFixture& f, ui::WidgetRegistry& registry, std::unique_ptr<ui::ExpandedView>& view,
                 DeviceInstanceId device, const device::Descriptor& desc) {
	view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, device, desc);
	auto* panel = dynamic_cast<Panel*>(view.get());
	REQUIRE(panel != nullptr);
	panel->setModulation(f.gui.modForTest(), f.gui.history());
	const visage::Point min = panel->minimumSize();
	panel->setBounds(0.0f, 0.0f, min.x, min.y);
	panel->resized();
	return panel;
}

} // namespace

TEST_CASE("undo, B7: a drag on the Shaper's, the Phaser's and the Automod's pad is ONE step over the whole travel") {
	FilterFixture f;
	ui::WidgetRegistry registry;
	std::unique_ptr<ui::ExpandedView> view;

	SUBCASE("Shaper") {
		namespace shp = feathers::fx::shaper;
		const DeviceInstanceId device = f.bridge.fxDeviceAt(f.bridge.addFxDevice(shp::kTraits.tag));
		auto* panel = openPanel<ui::ShaperExpanded>(f, registry, view, device, shp::kDescriptor);
		auto* pad = const_cast<ui::ShaperPad*>(panel->padForTest());
		REQUIRE(pad != nullptr);
		padDragIsOneStep(f, *pad, device, shp::kTravelFirst, ui::ShaperPad::kPhaseCount);
	}
	SUBCASE("Phaser") {
		namespace ph = feathers::fx::phaser;
		const DeviceInstanceId device = f.bridge.fxDeviceAt(f.bridge.addFxDevice(ph::kTraits.tag));
		auto* panel = openPanel<ui::PhaserExpanded>(f, registry, view, device, ph::kDescriptor);
		ui::PhaserPad* pad = panel->padForTest();
		REQUIRE(pad != nullptr);
		padDragIsOneStep(f, *pad, device, ph::kTravelFirst, ui::PhaserPad::kPhaseCount);
	}
	SUBCASE("Automod") {
		namespace amd = feathers::fx::automod;
		const DeviceInstanceId device = f.bridge.fxDeviceAt(f.bridge.addFxDevice(amd::kTraits.tag));
		auto* panel = openPanel<ui::AutomodExpanded>(f, registry, view, device, amd::kDescriptor);
		auto* pad = const_cast<ui::AutomodPad*>(panel->padForTest());
		REQUIRE(pad != nullptr);
		padDragIsOneStep(f, *pad, device, static_cast<size_t>(amd::Param::kCount), ui::AutomodPad::kTravelCount);
	}
}

//------------------------------------------------------------------------------------------------//
// THE CORNERS REPAINT WHEN A GROUP OPENS AND CLOSES                                                //
//------------------------------------------------------------------------------------------------//

TEST_CASE("undo: opening and closing a group tells the history's watchers, even a group that caught nothing") {
	ui::EditHistory history;
	visage::Frame corner;
	ui::Watch watch = history.watch(&corner);
	REQUIRE(history.framesToldForTest() == 0);

	// AN EMPTY GROUP: `canUndo` goes false as it opens and comes back as it closes, and nothing is recorded —
	// the road that told nobody, so a corner drawn lit before the press stayed lit while refusing.
	history.beginGroup("Perform");
	CHECK(history.framesToldForTest() == 1);
	history.endGroup();
	CHECK(history.framesToldForTest() == 2);
	CHECK(history.length() == 0);

	// A NESTED OPEN TELLS NOBODY AGAIN; only the outermost brackets do.
	history.beginGroup("outer");
	history.beginGroup("inner");
	CHECK(history.framesToldForTest() == 3);
	history.endGroup();
	CHECK(history.framesToldForTest() == 3);
	history.endGroup();
	CHECK(history.framesToldForTest() == 4);
}

//------------------------------------------------------------------------------------------------//
// BACKLOG PQ — A RELATIVE ("ENCODER") MACRO: A TURN IS AN EDIT OF EVERY BASE IT IS ROUTED TO       //
//------------------------------------------------------------------------------------------------//

namespace {

using RelType = feathers::device::MacroType;

backplane::Destination coreParam(ParamId id) {
	return backplane::Destination::core(static_cast<size_t>(modDestinationIndexOf(id)));
}

/// THE PATCH BLOCKS, IN MEMORY — the bytes `saveSoundGamma`/`saveMacroModes` write, read back by their loaders.
struct MemStream {
	std::vector<uint8_t> bytes;
	size_t at = 0;
	clap_ostream out() {
		return { this, [](const clap_ostream* s, const void* buffer, uint64_t size) -> int64_t {
			        auto* m = static_cast<MemStream*>(s->ctx);
			        const auto* p = static_cast<const uint8_t*>(buffer);
			        m->bytes.insert(m->bytes.end(), p, p + size);
			        return static_cast<int64_t>(size);
		        } };
	}
	clap_istream in() {
		return { this, [](const clap_istream* s, void* buffer, uint64_t size) -> int64_t {
			        auto* m = static_cast<MemStream*>(s->ctx);
			        const size_t n = std::min<size_t>(static_cast<size_t>(size), m->bytes.size() - m->at);
			        std::memcpy(buffer, m->bytes.data() + m->at, n);
			        m->at += n;
			        return static_cast<int64_t>(n);
		        } };
	}
};

} // namespace

TEST_CASE("relative macro: N turns of +d move Gamma's base by N.d.depth past the vernier's ceiling, and the vernier stays put") {
	ModFixture f;
	const ParamId gamma = soundParam(0, SoundField::Gamma);
	const ParamDesc& g = descOf(gamma);
	const double span = g.max - g.min; // the span the engine scales a route into Gamma by
	const float depth = 0.5f;
	// A SOUND'S IDENTITY IS NO ABSOLUTE MACRO'S TO MODULATE (A32): the route is refused until the macro is Relative.
	CHECK_FALSE(f.bridge.mayRoute(backplane::Source::core(0), coreParam(gamma)));
	f.bridge.setRouteAmount(backplane::Source::core(0), coreParam(gamma), depth);
	CHECK(f.bridge.routeAmount(backplane::Source::core(0), coreParam(gamma)) == 0.0f);
	f.bridge.setMacroType(0, RelType::Relative);
	CHECK(f.bridge.mayRoute(backplane::Source::core(0), coreParam(gamma)));
	f.bridge.setRouteAmount(backplane::Source::core(0), coreParam(gamma), depth);
	const double vernier = f.bridge.value(idOf(gamma));
	const double base0 = f.bridge.soundGamma().base[0];

	ui::RelativeMacroGesture turn { f.bridge, f.deviceHost };
	turn.begin(0);
	REQUIRE(turn.targetCount() == 1);
	constexpr int kTurns = 40;
	constexpr double kStep = 0.25;
	for (int i = 0; i < kTurns; ++i) {
		turn.turn(kStep);
	}
	turn.end(f.gui.history());

	// EXACT: every term is a power of two, so the sum is too — 40 x 0.25 x 0.5 x 8 = 40.
	const double expected = base0 + kTurns * kStep * static_cast<double>(depth) * span;
	CHECK(f.bridge.soundGamma().base[0] == expected);
	CHECK(expected + vernier > g.max); // past where the vernier, or an absolute route, could ever carry it
	CHECK(f.bridge.value(idOf(gamma)) == vernier);
}

TEST_CASE("relative macro: the header slot turns into an endless encoder, and one drag on it is one undo step") {
	ModFixture f;
	ui::EditHistory& history = *f.gui.history();
	ui::MacroSlot* slot = f.panel->macroSlotForTest(0);
	REQUIRE(slot != nullptr);
	// THE MENU OFFERS IT ON A HEADER MACRO, and picking it is the type.
	const std::vector<std::string> labels = ui::macroMenuLabelsForTest(slot->menuHooks());
	CHECK(std::find(labels.begin(), labels.end(), "     Relative (encoder)") != labels.end());
	ui::chooseMacroMenuEntry(slot->menuHooks(), ui::kMacroMenuRelative);
	REQUIRE(f.bridge.macroType(0) == RelType::Relative);
	CHECK(slot->encoder().isVisible());
	CHECK_FALSE(slot->knob().isVisible());
	CHECK_FALSE(slot->chip().isVisible());
	// AND ITS MENU NO LONGER OFFERS A POLARITY, which would switch nothing.
	const std::vector<std::string> relLabels = ui::macroMenuLabelsForTest(slot->menuHooks());
	CHECK(std::none_of(relLabels.begin(), relLabels.end(),
	                   [](const std::string& l) { return l.find("polar") != std::string::npos; }));

	const ParamId gamma = soundParam(0, SoundField::Gamma);
	const double span = descOf(gamma).max - descOf(gamma).min;
	const float depth = 0.5f;
	f.bridge.setRouteAmount(backplane::Source::core(0), coreParam(gamma), depth);
	const double base0 = f.bridge.soundGamma().base[0];
	const size_t steps = history.length();

	// ONE UNIT UP: `kDragRange` pixels, in two moves, is a full unit of increment.
	ui::EncoderFace& face = slot->encoder();
	const float x = face.width() * 0.5f;
	const float y = 200.0f;
	face.processMouseDown(at(x, y));
	face.processMouseDrag(at(x, y - ui::EncoderFace::kDragRange * 0.5f));
	face.processMouseDrag(at(x, y - ui::EncoderFace::kDragRange));
	face.processMouseUp(at(x, y - ui::EncoderFace::kDragRange));
	const double after = base0 + 1.0 * static_cast<double>(depth) * span;
	CHECK(f.bridge.soundGamma().base[0] == doctest::Approx(after).epsilon(1e-9));
	CHECK(history.length() == steps + 1);
	history.undo();
	CHECK(f.bridge.soundGamma().base[0] == base0);
	history.redo();
	CHECK(f.bridge.soundGamma().base[0] == doctest::Approx(after).epsilon(1e-9));

	// AND BACK TO A KNOB: the knob is on show again.
	ui::chooseMacroMenuEntry(slot->menuHooks(), ui::kMacroMenuKnob);
	CHECK(slot->knob().isVisible());
	CHECK_FALSE(slot->encoder().isVisible());
}

TEST_CASE("relative macro: Theta wraps round, a bounded parameter clamps, and turning back leaves the end at once") {
	ModFixture f;
	f.bridge.setMacroType(0, RelType::Relative);
	const backplane::Destination theta = backplane::Destination::core(travelDestinationIndex(0, TravelAxis::Theta));
	const double thetaSpan = device::routeSpanOf(internalDestinationDesc(travelInternalIndex(0, TravelAxis::Theta)));
	const ParamDesc& level = descOf(ParamId::VoiceLevel);
	const double levelSpan = level.max - level.min;
	f.bridge.setRouteAmount(backplane::Source::core(0), theta, 0.5f);
	f.bridge.setRouteAmount(backplane::Source::core(0), coreParam(ParamId::VoiceLevel), 1.0f);
	const double theta0 = f.bridge.soundTravel().orbit[0].theta;

	ui::RelativeMacroGesture turn { f.bridge, f.deviceHost };
	turn.begin(0);
	REQUIRE(turn.targetCount() == 2);
	turn.turn(1.5); // Theta: 0.5 x 1.5 turns = three quarters of a turn round; Level: well past its top
	// INDEPENDENTLY: the angle three quarters of a turn on is a quarter turn back, inside one turn.
	const double wound = theta0 + 0.5 * 1.5 * thetaSpan;
	CHECK(f.bridge.soundTravel().orbit[0].theta == doctest::Approx(std::remainder(wound, 2.0 * 3.14159265358979323846)));
	CHECK(std::abs(f.bridge.soundTravel().orbit[0].theta) <= 3.14159265358979323846);
	CHECK(f.bridge.value(idOf(ParamId::VoiceLevel)) == level.max);
	// THE CLAMP IS PER INCREMENT: a tenth back moves a tenth off the top, not a tenth back from 2.5 tops up.
	turn.turn(-0.1);
	CHECK(f.bridge.value(idOf(ParamId::VoiceLevel)) == doctest::Approx(level.max - 0.1 * levelSpan));
	turn.end(f.gui.history());
}

TEST_CASE("radius-mod-jump: a relative macro turns Radius along its face, the same face a route moves along") {
	// 2026-11-07, option A: a route into Radius moves ALONG THE KNOB'S FACE. A relative macro is the other road an
	// amount reaches Radius by, so it steps the same face: amount x turn of FACE, from where the knob sits, held at the
	// face's ends. Restated independently: face(r) = ln(1 + r / 2 (e^8.4 - 1)) / 8.4.
	// KNOWN-BAD: `RelativeMacroGesture::begin` holding the radius itself in `running` (no `radiusFace`) — the radius
	// moves by 0.1 travel units rather than a tenth of the face, and the first check goes red.
	const auto faceOf = [](double r) { return std::log1p(r / 2.0 * std::expm1(8.4)) / 8.4; };
	const auto radiusAt = [](double p) { return 2.0 * std::expm1(8.4 * p) / std::expm1(8.4); };
	ModFixture f;
	f.bridge.setMacroType(0, RelType::Relative);
	const backplane::Destination radius = backplane::Destination::core(travelDestinationIndex(0, TravelAxis::Radius));
	f.bridge.setRouteAmount(backplane::Source::core(0), radius, 0.1f);
	f.bridge.setSoundOrbit(0, { 0.0296, 0.0 });
	const double face0 = faceOf(0.0296);

	ui::RelativeMacroGesture turn { f.bridge, f.deviceHost };
	turn.begin(0);
	REQUIRE(turn.targetCount() == 1);
	turn.turn(1.0); // a tenth of the face up
	CHECK(f.bridge.soundTravel().orbit[0].radius == doctest::Approx(radiusAt(face0 + 0.1)).epsilon(1e-6));
	turn.turn(-2.0); // two tenths down from there
	CHECK(f.bridge.soundTravel().orbit[0].radius == doctest::Approx(radiusAt(face0 - 0.1)).epsilon(1e-6));
	turn.turn(-20.0); // far past the bottom: the face's end, a real zero
	CHECK(f.bridge.soundTravel().orbit[0].radius == 0.0);
	turn.turn(0.5); // and back off the end at once
	CHECK(f.bridge.soundTravel().orbit[0].radius == doctest::Approx(radiusAt(0.05)).epsilon(1e-6));
	turn.turn(40.0); // the top: the whole plane
	CHECK(f.bridge.soundTravel().orbit[0].radius == kTravelRadiusMax);
	turn.end(f.gui.history());
}

TEST_CASE("relative macro: one undo reverts one gesture across every base it moved, and the host hears the parameter's") {
	ModFixture f;
	ui::EditHistory& history = *f.gui.history();
	f.bridge.setMacroType(0, RelType::Relative);
	const ParamId gamma = soundParam(0, SoundField::Gamma);
	f.bridge.setRouteAmount(backplane::Source::core(0), coreParam(gamma), 1.0f);
	f.bridge.setRouteAmount(backplane::Source::core(0), coreParam(ParamId::VoiceLevel), 0.25f);
	f.bridge.beginGesture(idOf(ParamId::VoiceLevel));
	f.bridge.setValue(idOf(ParamId::VoiceLevel), 0.25);
	f.bridge.endGesture(idOf(ParamId::VoiceLevel));
	const double g0 = f.bridge.soundGamma().base[0];
	const double l0 = f.bridge.value(idOf(ParamId::VoiceLevel));
	const size_t steps = history.length();

	ui::RelativeMacroGesture turn { f.bridge, f.deviceHost };
	turn.begin(0);
	turn.turn(0.5);
	turn.end(&history);
	const double g1 = f.bridge.soundGamma().base[0];
	const double l1 = f.bridge.value(idOf(ParamId::VoiceLevel));
	turn.begin(0);
	turn.turn(0.25);
	turn.end(&history);
	const double g2 = f.bridge.soundGamma().base[0];
	REQUIRE(g1 != g0);
	REQUIRE(l1 != l0);
	REQUIRE(g2 != g1);
	CHECK(history.length() == steps + 2);

	(void)drain(f.bridge);
	history.undo(); // the second turn, and only it
	CHECK(f.bridge.soundGamma().base[0] == g1);
	CHECK(f.bridge.value(idOf(ParamId::VoiceLevel)) == l1);
	history.undo(); // the first, both bases at once
	CHECK(f.bridge.soundGamma().base[0] == g0);
	CHECK(f.bridge.value(idOf(ParamId::VoiceLevel)) == l0);
	CHECK(hostSawGesture(drain(f.bridge), idOf(ParamId::VoiceLevel), l0));
	history.redo();
	CHECK(f.bridge.soundGamma().base[0] == g1);
	CHECK(f.bridge.value(idOf(ParamId::VoiceLevel)) == l1);
}

TEST_CASE("relative macro: the accumulated base and the type survive the patch blocks into a fresh bridge") {
	ModFixture f;
	f.bridge.setMacroType(2, RelType::Relative);
	const ParamId gamma = soundParam(1, SoundField::Gamma);
	f.bridge.setRouteAmount(backplane::Source::core(2), coreParam(gamma), 0.75f);
	ui::RelativeMacroGesture turn { f.bridge, f.deviceHost };
	turn.begin(2);
	for (int i = 0; i < 9; ++i) {
		turn.turn(1.0);
	}
	turn.end(f.gui.history());
	const double carried = f.bridge.soundGamma().base[1];
	REQUIRE(carried > descOf(gamma).max);

	// THE BLOCKS THE PLUGIN WRITES FOR THESE (feathers_clap.cpp's state save: `saveSoundGamma(bridge.soundGamma())`,
	// `saveMacroModes(bridge.macroModes())`), read back by their loaders into a bridge that never saw the turn.
	MemStream stream;
	clap_ostream out = stream.out();
	REQUIRE(saveSoundGamma(&out, f.bridge.soundGamma()));
	REQUIRE(saveMacroModes(&out, f.bridge.macroModes()));
	clap_istream in = stream.in();
	SoundGamma readGamma;
	MacroModes readModes;
	bool present = false;
	REQUIRE(loadSoundGamma(&in, &readGamma, &present));
	REQUIRE(present);
	REQUIRE(loadMacroModes(&in, &readModes, &present));
	REQUIRE(present);
	ParamBridge fresh;
	fresh.restoreSoundGamma(readGamma);
	fresh.restoreMacroModes(readModes);
	CHECK(fresh.soundGamma().base[1] == carried);
	CHECK(fresh.macroType(2) == RelType::Relative);
	CHECK(fresh.macroType(0) == RelType::Knob);
	CHECK(fresh.macroRelativeMask() == (1u << 2));
}

TEST_CASE("relative macro: the tray's Gamma box is a destination for a Relative macro only, and a depth set on it is one step") {
	ModFixture f;
	ui::EditHistory& history = *f.gui.history();
	ui::ModState mod;
	ui::GammaControl gamma { f.bridge, [] { return size_t { 0 }; } };
	gamma.setModulation(&mod, &history);
	mod.focus(device::Source::core(1));
	(void)mod.arm(true);

	// AN ABSOLUTE MACRO ARMED: a destination, but not one it may reach — no glow, and a drag sets nothing.
	CHECK(gamma.display().isDestination);
	CHECK_FALSE(gamma.display().routable);
	gamma.beginAmountGesture();
	gamma.setAmount(0.4f);
	gamma.endAmountGesture();
	CHECK(f.bridge.routeAmount(backplane::Source::core(1), gamma.destination()) == 0.0f);

	// RELATIVE: it glows, and the depth lands as one undo step.
	f.bridge.setMacroType(1, RelType::Relative);
	CHECK(gamma.display().routable);
	const size_t steps = history.length();
	gamma.beginAmountGesture();
	gamma.setAmount(0.4f);
	gamma.endAmountGesture();
	CHECK(f.bridge.routeAmount(backplane::Source::core(1), gamma.destination()) == 0.4f);
	CHECK(history.length() == steps + 1);
	history.undo();
	CHECK(f.bridge.routeAmount(backplane::Source::core(1), gamma.destination()) == 0.0f);
	(void)mod.arm(false);
}
