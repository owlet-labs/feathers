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

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "card_head_check.h"
#include "menu_placement_check.h"
#include "device/device_groups.h"
#include "rack/device_browser.h"

#include "fx/owltt/owltt_types.h"
#include "fx/filter/filter_types.h"
#include "fx/shaper/shaper_types.h"
#include "mod/dahdsr/dahdsr_types.h"
#include "mod/mseg/mseg_types.h"
#include "mod/lfo/lfo_types.h"
#include "mod/arp/arp_types.h"
#include "plugin/ui/emitters_front.h"
#include "mod/arp/pattern_types.h"
#include "mod/stereo/stereo_types.h"
#include "mod/macros/macros_types.h"
#include "mod/modulator.h"
#include "plugin/ui/macro_menu.h"
#include "plugin/ui/macros_card.h"
#include "mod/math/math_types.h"
#include "plugin/ui/math_card.h"
#include "mod/twinfield/twinfield_types.h"
#include "plugin/ui/twinfield_card.h"
#include "plugin/ui/source_arm.h"
#include "plugin/device_registry.h"
#include "plugin/ui/filter_card.h"
#include "plugin/ui/mod_route_menu.h"
#include "plugin/ui/shaper_card.h"
#include "plugin/ui/sklitter_card.h" // backlog GM: the card that lost Mix and Density
#include "fx/sklitter/sklitter_types.h"
#include "plugin/ui/automod_card.h"
#include "plugin/ui/expanded_views.h"
#include "composite/composite_types.h"
#include "plugin/ui/knob.h"
#include "rack/chain_expanded.h"
#include "rack/chain_state.h"
#include "rack/converger.h"
#include "rack/rack_device.h"
#include "rack/rack_editor.h"
#include "rack/rack_bridge.h"
#include "rack/rack_engine.h"

#include "visage_app/application_editor.h" // the MACROS card's windowless camera

#include <algorithm>
#include <cmath>
#include <numbers>
#include <cstdlib>
#include <fstream>
#include <set>
#include <typeinfo>
#include <string>
#include <utility>
#include <vector>

namespace feathers::ui {
/// A DESCRIPTOR'S NAME FOR A CAPTURE, WITH THE NULL GUARDED — `device::SourceDesc::name` is a
/// `const char*` with no default initializer and production guards it in three places. A case that
/// walks the whole registry to report a failure should not be the thing that crashes on a device
/// someone added without one. (The widget suites have this in `gui_widgets/fixtures.h`; this file
/// does not include it.)
inline std::string nameOrUnnamed(const char* name) {
	return name != nullptr ? std::string(name) : std::string("(unnamed)");
}

/// A press at the origin — the venue does not care where, only that it happened.
inline visage::MouseEvent MouseEventForTest() {
	visage::MouseEvent e {};
	return e;
}
}

// THE RACK'S SURFACE, HEADLESS.
//
// A REPAINT CANNOT BE OBSERVED WITHOUT A WINDOW, which this repo learned the hard way, so nothing here
// claims anything about pixels. What it does claim is the part a windowed venue cannot check cheaply:
// that the surface is built from the card floor at all, that a card appears for a device and goes when
// the device does, and that a macro knob and the engine agree about a number.

using namespace feathers;

namespace {

/// THE EDITOR'S HEIGHT AS `RackGui` REPORTS IT: the lane's `kHeight` plus the canvas band above it, which
/// is room the editor asks the host for rather than room taken from the lane. Every case that sizes the
/// editor sizes it to this, so the lane keeps the height its cases were written against.
constexpr int kEditorHeight = rack::RackEditor::kHeight + static_cast<int>(ui::ExpandedCanvas::kExtent);

constexpr backplane::Axes kAxes {
	.coreSources = 8,
	.coreDestinations = 0,
	.deviceSlots = 8,
	.outputStride = 4,
};

struct Fixture {
	rack::RackEngine engine { kAxes, hostCatalogue() };
	/// THE EDITOR ADDRESSES A BRIDGE, NOT THE ENGINE (AS commit 1a). Today it forwards, so nothing
	/// here changes behaviour; what changes is that the fixture builds the editor the way the plugin
	/// does, through the seam, rather than reaching past it.
	rack::RackBridge bridge { engine };
	ui::ModState mod;
	ui::EditHistory history;
	rack::RackEditor editor { bridge, mod, history };

	/// **ADD AND HAND OVER, which is what a block does (AS 2).** The chain verbs are queued now, so a
	/// case that added a device and then asked the ENGINE about it would be asking before the handoff.
	/// Standing the drain in here is the honest spelling; the alternative — asserting against the
	/// bridge's optimistic mirror only — would delete the engine half most of these cases exist for.
	///
	/// **THE ANSWER IS THE DEVICE'S ID (kill-lanes step 2)** — `kNoInstance` for a refusal. It was the
	/// device's lane, and comparing an id against `kNoSlot` compiles and is always unequal.
	DeviceInstanceId add(const char* tag) {
		const DeviceInstanceId added = bridge.addDevice(tag);
		bridge.drain();
		return added;
	}
	void remove(DeviceInstanceId device) {
		bridge.removeDevice(device);
		bridge.drain();
	}
	/// THE DEVICE THE ENGINE RUNS AT `position` — the order the ear hears — named by the id the engine
	/// keeps it under, which is the record's (kill-lanes step 3a).
	DeviceInstanceId engineDeviceAt(size_t position) const {
		const size_t slot = engine.deviceAtPosition(position);
		return slot == rack::RackEngine::kNoSlot ? kNoInstance : engine.slotAtLane(slot).id;
	}
	/// AND THE OTHER WAY: THE SLOT THE ENGINE KEEPS `device` IN — its own choice (kill-lanes step 3a), for a
	/// case that asks the ENGINE. Every such case asks through here, so they are countable.
	size_t laneOf(DeviceInstanceId device) const { return engine.slotOf(device); }
	bool move(size_t from, size_t to) {
		const bool ok = bridge.moveDevice(from, to);
		bridge.drain();
		return ok;
	}
};

/// **A GENERIC CARD, BUILT DIRECTLY OVER A REAL DEVICE.**
///
/// Every REGISTERED device has a custom card now — the filter was the last one — so the editor no
/// longer produces a `GenericCard` for anything, and three cases that reached one through
/// the editor's cards lost their subject. `GenericCard` is not dead: it is the fallback for a descriptor
/// this build has never seen, which is exactly the case that cannot be staged through the registry.
///
/// So these cases construct it. What they are about — the grid's packing rule, its cap, its expand
/// verb, and whether a `DeviceControl` learns the panel's state — is a property of the class, and was
/// only ever being reached VIA a device that happened to lack a card.
std::unique_ptr<ui::GenericCard> genericOver(rack::RackBridge& engine, ui::ModState& mod,
                                             ui::EditHistory& history, DeviceInstanceId device,
                                             const device::Descriptor& desc) {
	auto card = std::make_unique<ui::GenericCard>(engine, device, desc, nullptr);
	card->setModulation(&mod, &history);
	// **THE RACK'S AXIS, SET BEFORE ANYTHING ASKS FOR AN EXTENT.** `laneExtent` answers about the FLOW
	// axis, which is the width horizontally and the height vertically — so a card left on the default
	// Vertical hands back a list HEIGHT to a caller sizing a width. It measured 2964 against a 720
	// window and read exactly like the uncapped bug this case was written for.
	card->setLaneAxis(ui::FxCard::LaneAxis::Horizontal);
	return card;
}

/// EVERY CARD THE EDITOR HOLDS — not only those the chain names, so a card that outlived its device
/// is counted rather than walked past.
size_t liveCards(const rack::RackEditor& e) { return e.cardCountForTest(); }

} // namespace

TEST_CASE("backlog LX: the rack canvas's fresh band and its double-click reset are one number, under the lane's cap") {
	Fixture f;
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	ui::ExpandedCanvas& canvas = f.editor.canvas();
	// RESTATED: the canvas's default band (480) under the cap, which is the lane's one-unit height (466) times kMaxShare.
	const float expected = std::min(480.0f, 466.0f * ui::ExpandedCanvas::kMaxShare);
	CHECK(canvas.openExtent() == doctest::Approx(expected));
	REQUIRE(canvas.setCollapsed(false));
	REQUIRE(canvas.setOpenExtent(300.0f));
	visage::MouseEvent twice;
	twice.position = { canvas.gripBounds().x() + 4.0f, canvas.gripBounds().y() + canvas.gripBounds().height() * 0.5f };
	twice.repeat_click_count = 1;
	canvas.onMouseDown().callback(twice);
	CHECK(canvas.openExtent() == doctest::Approx(expected));
}

TEST_CASE("the surface has one knob per macro and no cards to begin with") {
	Fixture f;
	REQUIRE(f.editor.macrosForTest().size() == kAxes.coreSources);
	REQUIRE(liveCards(f.editor) == 0);
}

TEST_CASE("backlog EG: a rack macro's knob reads through its polarity — the menu is on the knob, the verb is the bridge's, and the engine's cell follows") {
	// A context menu turns a macro into a bipolar knob that defaults to 0. The macro's stored value
	// stays 0..1; the knob, its readout and the published cell change.
	Fixture f;
	REQUIRE(f.editor.macrosForTest().size() == kAxes.coreSources);
	ui::Knob* knob = &f.editor.macrosForTest()[1]->knob();
	REQUIRE(knob != nullptr);
	CHECK(knob->hasMacroMenuForTest()); // the switch is on the knob
	CHECK_FALSE(f.bridge.macroBipolar(1));

	f.bridge.setMacro(1, 0.75f);
	f.bridge.setMacroBipolar(1, true);
	CHECK(f.bridge.macroBipolar(1));
	// A MACRO ARMED AT ANY KNOB DRAWS A REACH BOTH WAYS NOW — the host's answer, for core sources.
	CHECK(f.bridge.sourceIsBipolarNow(backplane::Source::core(1)));
	CHECK_FALSE(f.bridge.sourceIsBipolarNow(backplane::Source::core(0)));

	// THE ENGINE, AT THE DRAIN: the stored value as the hand holds it, and the mode beside it — what
	// the cell reads from the pair is the engine suite's to pin (test_rack_engine "backlog EG").
	f.bridge.drain();
	CHECK(f.engine.macro(1) == doctest::Approx(0.75f));
	CHECK(f.engine.macroBipolar(1));

	// AND BACK TO UNIPOLAR, the same stored value.
	f.bridge.setMacroBipolar(1, false);
	f.bridge.drain();
	CHECK_FALSE(f.engine.macroBipolar(1));
	CHECK(f.engine.macro(1) == doctest::Approx(0.75f));
	CHECK_FALSE(f.bridge.sourceIsBipolarNow(backplane::Source::core(1)));
}

TEST_CASE("a rack macro knob offers the macro menu, and a pick through it lands in the bridge") {
	Fixture f;
	REQUIRE(f.editor.macrosForTest().size() == kAxes.coreSources);
	for (size_t i = 0; i < f.editor.macrosForTest().size(); ++i) {
		CAPTURE(i);
		REQUIRE(f.editor.macrosForTest()[i]->knob().hasMacroMenuForTest());
	}
	ui::Knob* knob = &f.editor.macrosForTest()[1]->knob();
	ui::chooseMacroMenuEntry(knob->macroMenuForTest(), ui::kMacroMenuMomentary);
	CHECK(f.bridge.macroType(1) == device::MacroType::Momentary);
	CHECK(f.bridge.macroType(0) == device::MacroType::Knob);
	ui::chooseMacroMenuEntry(knob->macroMenuForTest(), ui::kMacroMenuBipolar);
	CHECK(f.bridge.macroBipolar(1));
	CHECK_FALSE(f.bridge.macroBipolar(0));
	// THE MENU READS IT BACK THROUGH THE SAME HOOKS — the X follows the bridge.
	CHECK(ui::macroMenuLabelsForTest(knob->macroMenuForTest())
	      == std::vector<std::string> { "     Knob", "     Latch", "X  Momentary", "", "     Unipolar  0..100%",
	                                    "X  Bipolar  -100..100%", "", "Rename…" });
}

TEST_CASE("a rack macro menu held across a row rebuild still writes the macro it was opened on") {
	// `setOuterDevice` rebuilds the row — every knob freed and made again — while a menu may be open
	// holding a copy of the old knob's hooks. The editor is the same one, so the pick is still good.
	Fixture f;
	const ui::MacroMenuHooks held = f.editor.macrosForTest()[2]->knob().macroMenuForTest();
	f.editor.setOuterDevice(nullptr, kNoInstance, nullptr, nullptr);
	REQUIRE(f.editor.macrosForTest().size() == kAxes.coreSources);
	ui::chooseMacroMenuEntry(held, ui::kMacroMenuLatch);
	ui::chooseMacroMenuEntry(held, ui::kMacroMenuBipolar);
	CHECK(f.bridge.macroType(2) == device::MacroType::Latch);
	CHECK(f.bridge.macroBipolar(2));
	CHECK(f.bridge.macroType(1) == device::MacroType::Knob);
	CHECK_FALSE(f.bridge.macroBipolar(1));
}

TEST_CASE("a rack macro menu left open across an editor rebuild writes nothing") {
	// visage parents the popup to the ROOT frame, and nested the root is Feathers' editor, which
	// outlives this one: `ChainExpanded::follow()` drops and rebuilds the RackEditor on a tick. A pick
	// made after that must reach no freed editor. Under plain ctest a freed read may not fault; the
	// ASan build of this binary is where this case earns its keep.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	rack::RackBridge bridge { engine };
	ui::ModState mod;
	ui::EditHistory history;
	auto editor = std::make_unique<rack::RackEditor>(bridge, mod, history);
	REQUIRE(editor->macrosForTest().size() > 1);
	const ui::MacroMenuHooks held = editor->macrosForTest()[1]->knob().macroMenuForTest();
	editor.reset();

	ui::chooseMacroMenuEntry(held, ui::kMacroMenuMomentary);
	ui::chooseMacroMenuEntry(held, ui::kMacroMenuBipolar);
	CHECK(bridge.macroType(1) == device::MacroType::Knob);
	CHECK_FALSE(bridge.macroBipolar(1));
	// THE READERS ANSWER THE DEFAULTS rather than reading through the dead editor.
	CHECK(held.readType() == device::MacroType::Knob);
	CHECK_FALSE(held.readBipolar());
}

namespace {

/// A LEFT PRESS AND RELEASE AT THE CHIP'S CENTRE, handed to its own callbacks — a click is not
/// deliverable headlessly, and the VALUE it writes is the claim.
void clickChip(ui::SwitchChip& chip) {
	visage::MouseEvent e {};
	e.position = { chip.width() * 0.5f, chip.height() * 0.5f };
	e.button_id = visage::kMouseButtonLeft;
	e.button_state = visage::kMouseButtonLeft;
	e.is_down = true;
	chip.onMouseDown().callback(e);
	e.is_down = false;
	chip.onMouseUp().callback(e);
}

} // namespace

TEST_CASE("a rack macro set to Latch shows a pad and the pad writes the macro") {
	Fixture f;
	REQUIRE(f.editor.macrosForTest().size() == kAxes.coreSources);
	f.editor.setBounds(0, 0, 1200, kEditorHeight); // a laid-out row, so the pad has a column
	ui::MacroSlot* slot = f.editor.macrosForTest()[3].get();
	REQUIRE(slot != nullptr);
	CHECK(slot->knob().isVisible());
	CHECK_FALSE(slot->chip().isVisible());

	ui::chooseMacroMenuEntry(slot->menuHooks(), ui::kMacroMenuLatch);
	CHECK(f.bridge.macroType(3) == device::MacroType::Latch);
	CHECK(f.bridge.macroType(2) == device::MacroType::Knob);
	// RE-SEATED AT THE PICK, as a pad in the knob's column.
	CHECK_FALSE(slot->knob().isVisible());
	CHECK(slot->chip().isVisible());
	CHECK(slot->chip().presentation() == ui::SwitchChip::Presentation::Pad);
	CHECK(slot->chip().behaviour() == ui::SwitchChip::Behaviour::Latch);
	CHECK(slot->chip().bounds() == slot->knob().bounds());
	CHECK(slot->chip().width() > 0.0f);

	// THE PAD WRITES THE RACK'S MACRO — through the forward channel, so the engine has it at the drain.
	f.bridge.drain();
	REQUIRE(f.engine.macro(3) == doctest::Approx(0.0f));
	clickChip(slot->chip());
	f.bridge.drain();
	CHECK(f.engine.macro(3) == doctest::Approx(1.0f));
	CHECK(f.engine.macro(2) == doctest::Approx(0.0f));
	CHECK(slot->chip().readout() == "ON");
	clickChip(slot->chip());
	f.bridge.drain();
	CHECK(f.engine.macro(3) == doctest::Approx(0.0f));
}

TEST_CASE("a row rebuilt under a held momentary pad lets the macro go") {
	// `setOuterDevice` frees every slot while the hand may be on a pad. The chip's destructor lets go
	// through the macro's view — which is why the views die AFTER the slots — so the macro falls back to
	// its minimum rather than sticking at max with nothing left to release it. The rack's own macro
	// has no gesture to count (`MacroControl::beginGesture` is empty: the binding carries undo), so the
	// VALUE is the claim, read off the engine after the drain. ASan's run is the lifetime half.
	Fixture f;
	f.editor.setBounds(0, 0, 1200, kEditorHeight);
	ui::MacroSlot* slot = f.editor.macrosForTest()[5].get();
	ui::chooseMacroMenuEntry(slot->menuHooks(), ui::kMacroMenuMomentary);
	REQUIRE(slot->chip().isVisible());
	REQUIRE(slot->chip().behaviour() == ui::SwitchChip::Behaviour::Momentary);

	visage::MouseEvent e {};
	e.position = { slot->chip().width() * 0.5f, slot->chip().height() * 0.5f };
	e.button_id = visage::kMouseButtonLeft;
	e.button_state = visage::kMouseButtonLeft;
	e.is_down = true;
	slot->chip().onMouseDown().callback(e);
	f.bridge.drain();
	// HELD: not vacuous — the macro IS at max before the rebuild.
	REQUIRE(f.engine.macro(5) == doctest::Approx(1.0f));

	f.editor.setOuterDevice(nullptr, kNoInstance, nullptr, nullptr);
	f.bridge.drain();
	CHECK(f.engine.macro(5) == doctest::Approx(0.0f));
	CHECK(f.bridge.macro(5) == doctest::Approx(0.0f));
	// THE REBUILT ROW IS A PAD AGAIN, off, and a fresh press works.
	ui::MacroSlot* rebuilt = f.editor.macrosForTest()[5].get();
	REQUIRE(rebuilt != nullptr);
	CHECK(rebuilt->chip().isVisible());
	CHECK(rebuilt->chip().readout() == "OFF");
	f.editor.setBounds(0, 0, 1200, kEditorHeight);
	clickChip(rebuilt->chip()); // a momentary click presses and lets go
	f.bridge.drain();
	CHECK(f.engine.macro(5) == doctest::Approx(0.0f));
}

TEST_CASE("a row rebuilt under an open rename field commits nothing") {
	// The field dies with its slot mid-edit. What it held is not a name anybody committed, so the macro
	// keeps its default — and nothing calls back into the freed slot on the way out (ASan's half).
	Fixture f;
	ui::MacroSlot* slot = f.editor.macrosForTest()[2].get();
	ui::chooseMacroMenuEntry(slot->menuHooks(), ui::kMacroMenuRename);
	REQUIRE(slot->renaming());
	slot->field().processTextInput("Ghost");
	REQUIRE(slot->field().text() != "Macro 3"); // typed into: there IS something that could be committed

	f.editor.setOuterDevice(nullptr, kNoInstance, nullptr, nullptr);
	CHECK(f.bridge.macroName(2) == "Macro 3");
	CHECK_FALSE(f.bridge.macroRenamed(2));
	ui::MacroSlot* rebuilt = f.editor.macrosForTest()[2].get();
	REQUIRE(rebuilt != nullptr);
	CHECK_FALSE(rebuilt->renaming());
	CHECK(rebuilt->caption() == "Macro 3");
	// AND THE NEW ROW STILL RENAMES, through its own field.
	ui::chooseMacroMenuEntry(rebuilt->menuHooks(), ui::kMacroMenuRename);
	REQUIRE(rebuilt->renaming());
	rebuilt->field().processTextInput("Real");
	rebuilt->field().processKeyPress(visage::KeyEvent { visage::KeyCode::Return, 0, true });
	CHECK(f.bridge.macroRenamed(2));
	CHECK(rebuilt->caption() == f.bridge.macroName(2));
}

TEST_CASE("rack rename commits to the bridge and the caption reads the name") {
	Fixture f;
	ui::MacroSlot* slot = f.editor.macrosForTest()[4].get();
	REQUIRE(slot != nullptr);
	// THE RACK'S OWN SHORT CAPTION IS THE FULL DEFAULT — what the row printed before it held slots.
	CHECK(slot->caption() == "Macro 5");
	CHECK(std::string(slot->knob().label()) == "Macro 5");

	// THE MENU'S RENAME OPENS THE SLOT'S FIELD, holding the name.
	ui::chooseMacroMenuEntry(slot->menuHooks(), ui::kMacroMenuRename);
	REQUIRE(slot->renaming());
	CHECK(slot->field().text() == "Macro 5");

	slot->field().processTextInput("Sweep");
	slot->field().processKeyPress(visage::KeyEvent { visage::KeyCode::Return, 0, true });
	CHECK_FALSE(slot->renaming());
	CHECK(f.bridge.macroName(4) == "Sweep");
	CHECK(f.bridge.macroRenamed(4));
	CHECK_FALSE(f.bridge.macroRenamed(3));
	CHECK(slot->caption() == "Sweep");
	CHECK(std::string(slot->knob().label()) == "Sweep");
	CHECK(slot->chip().label() == "Sweep");
	// AND THE ROUTE LIST'S HEAD NAMES THE SOURCE THE SAME WAY (`setSourceNamer`).
	CHECK(f.mod.sourceName(backplane::Source::core(4)) == "Sweep");

	// A NAME SET FROM ANYWHERE ELSE — the host, a script — is heard through the bridge's listener.
	f.bridge.setMacroName(4, "Tilt");
	CHECK(slot->caption() == "Tilt");
	// AND THE DEFAULT SPELLED OUT CLEARS IT, back to the short caption.
	f.bridge.setMacroName(4, "Macro 5");
	CHECK_FALSE(f.bridge.macroRenamed(4));
	CHECK(slot->caption() == "Macro 5");
}

TEST_CASE("a rack patch load relabels and retypes the slots") {
	Fixture f;
	rack::RackPatch patch;
	patch.present = true;
	patch.macro.assign(kAxes.coreSources, 0.0f);
	patch.macroType.assign(kAxes.coreSources, static_cast<uint8_t>(device::MacroType::Knob));
	patch.macroName.assign(kAxes.coreSources, std::string {});
	patch.macroType[1] = static_cast<uint8_t>(device::MacroType::Momentary);
	patch.macroType[6] = static_cast<uint8_t>(device::MacroType::Latch);
	patch.macroName[6] = "Freeze";
	f.editor.syncMacros(); // settle
	const size_t before = f.editor.macroResyncsForTest();

	REQUIRE(f.bridge.loadPatch(patch).routes.count == 0);
	// THE NAME AT ONCE, through the listener a load tells for each name it moves.
	CHECK(f.editor.macrosForTest()[6]->caption() == "Freeze");
	CHECK(f.editor.macrosForTest()[1]->caption() == "Macro 2");
	CHECK(f.editor.macrosForTest()[1]->knob().isVisible()); // not yet: its type is the beat's to catch

	// THE TYPES ON THE EDITOR'S BEAT — no listener reports a type. ONE re-seat, not two: macro 7's name
	// moved too, and the listener's `refresh()` re-reads the type with the name, so only macro 2 — a type
	// with no name beside it — waited for the beat.
	f.editor.syncMacros();
	CHECK(f.editor.macroResyncsForTest() == before + 1);
	const auto& slots = f.editor.macrosForTest();
	for (size_t i = 0; i < slots.size(); ++i) {
		CAPTURE(i);
		const bool pad = i == 1 || i == 6;
		CHECK(slots[i]->knob().isVisible() == !pad);
		CHECK(slots[i]->chip().isVisible() == pad);
	}
	CHECK(slots[1]->chip().behaviour() == ui::SwitchChip::Behaviour::Momentary);
	CHECK(slots[6]->chip().behaviour() == ui::SwitchChip::Behaviour::Latch);
	CHECK(slots[6]->chip().label() == "Freeze");
	// A STILL BEAT RE-SEATS NOTHING.
	f.editor.syncMacros();
	CHECK(f.editor.macroResyncsForTest() == before + 1);

	// AND A ROW REBUILT AFTER THE LOAD IS BUILT AS THE PATCH SAYS.
	f.editor.setOuterDevice(nullptr, kNoInstance, nullptr, nullptr);
	CHECK(f.editor.macrosForTest()[6]->caption() == "Freeze");
	CHECK(f.editor.macrosForTest()[6]->chip().isVisible());
	// AND THE REBUILT ROW STILL HEARS NAMES.
	f.bridge.setMacroName(6, "Thaw");
	CHECK(f.editor.macrosForTest()[6]->caption() == "Thaw");
}

TEST_CASE("the rack's namer reads a relabelled MACROS output, and leaves a one-output device alone") {
	// THE ROUTE LIST'S HEAD (`setSourceNamer`) names a multi-output device's source by the output as THIS
	// INSTANCE calls it — "Macros 1 · Wobble" — and a one-output device by its name and place alone.
	Fixture f;
	const DeviceInstanceId macros = f.add("MACROS");
	const DeviceInstanceId lfo = f.add("LFO");
	REQUIRE(macros != kNoInstance);
	REQUIRE(lfo != kNoInstance);
	const backplane::Source second = backplane::Source::output(macros, 1);
	CHECK(f.mod.sourceName(second) == "Macros 1 · Macro 2");
	f.bridge.setOutputLabel(macros, 1, "Wobble");
	CHECK(f.mod.sourceName(second) == "Macros 1 · Wobble");
	CHECK(f.mod.sourceName(backplane::Source::output(macros, 0)) == "Macros 1 · Macro 1");
	CHECK(f.mod.sourceName(backplane::Source::output(lfo, 0)) == "LFO 2");
}

TEST_CASE("a rename committed after the rack editor died writes nothing") {
	// The slot's rename verb is the editor's (`macroNaming`), holding `this`. A copy that outlives the
	// editor must find nothing to write into rather than `engine_` on a freed editor — under plain ctest
	// a freed read may not fault; the ASan build of this binary is where this case earns its keep.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	rack::RackBridge bridge { engine };
	ui::ModState mod;
	ui::EditHistory history;
	auto editor = std::make_unique<rack::RackEditor>(bridge, mod, history);
	REQUIRE(editor->macrosForTest().size() > 2);
	const ui::MacroSlot::Naming held = editor->macrosForTest()[2]->naming();
	REQUIRE(held.rename);
	editor.reset();

	held.rename("Ghost");
	CHECK_FALSE(bridge.macroRenamed(2));
	CHECK(bridge.macroName(2) == "Macro 3");
	// THE READERS ANSWER THE DEFAULTS rather than reading through the dead editor.
	CHECK(held.name() == "Macro 3");
	CHECK_FALSE(held.renamed());
}

TEST_CASE("a card appears for a device and goes when the device does") {
	Fixture f;
	const DeviceInstanceId slot = f.add(fx::filter::kTraits.tag);
	f.editor.syncCards();
	REQUIRE(liveCards(f.editor) == 1);
	REQUIRE(f.editor.cardOfDeviceForTest(slot) != nullptr);

	f.remove(slot);
	f.editor.syncCards();
	REQUIRE(liveCards(f.editor) == 0);
}

TEST_CASE("a device this build does not have gets a card that says \"device missing\", in its place") {
	// **KEPT, AND SHOWN AS "device missing".** The rack's record kept such a device
	// and saved it back out, and this editor drew NOTHING for it — a chain with a gap a hand could not
	// see, click or remove. A patch carrying one between two devices this build has:
	Fixture f;
	rack::RackPatch patch;
	patch.present = true;
	patch.macro.assign(kAxes.coreSources, 0.0f);
	const auto device = [](const char* tag) {
		rack::RackPatch::Device d;
		std::copy_n(tag, 4, d.tag.begin());
		return d;
	};
	patch.device.push_back(device(fx::filter::kTraits.tag));
	patch.device.push_back(device("zzz9"));
	patch.device.push_back(device(fx::owltt::kTraits.tag));
	REQUIRE(f.bridge.loadPatch(patch).routes.count == 0);
	f.bridge.drain();
	f.editor.syncCards();

	REQUIRE(f.bridge.chainLength() == 3);
	const DeviceInstanceId lane = f.bridge.deviceAtPosition(1);
	REQUIRE(lane != kNoInstance);
	// THE PREMISE: this build cannot name it — the record holds a tagless PLACEHOLDER for it (or nothing).
	const device::Descriptor* held = f.bridge.descriptorOf(lane);
	REQUIRE((held == nullptr || held->traits.tag == nullptr));
	const auto* missing = dynamic_cast<const ui::MissingDeviceCard*>(f.editor.cardOfDeviceForTest(lane));
	REQUIRE(missing != nullptr);
	CHECK(missing->name() == "zzz9");
	CHECK(liveCards(f.editor) == 3); // all three drawn — no gap

	// AND IT CAN BE TAKEN OUT, which is the one thing a hand most needs to do with it.
	f.remove(lane);
	f.editor.syncCards();
	CHECK(f.editor.cardOfDeviceForTest(lane) == nullptr);
	CHECK(liveCards(f.editor) == 2);
}

TEST_CASE("a modulator gets a card too, because a device is a device") {
	Fixture f;
	f.add(mod::dahdsr::kTraits.tag);
	f.editor.syncCards();
	REQUIRE(liveCards(f.editor) == 1);
}

TEST_CASE("syncCards is idempotent — calling it twice builds nothing the second time") {
	// A CARD REBUILT UNDER A LIVE PRESS is how the fx lane once reordered a chain on a click that
	// never moved. This surface may sync on a tick, so building only where the device CHANGED is a
	// requirement rather than an optimisation.
	Fixture f;
	f.add(fx::filter::kTraits.tag);
	f.editor.syncCards();
	const void* first = f.editor.cardsForTest()[0];
	f.editor.syncCards();
	f.editor.syncCards();
	REQUIRE(f.editor.cardsForTest()[0] == first);
}

TEST_CASE("a card in one slot is not disturbed by a device arriving in another") {
	Fixture f;
	f.add(fx::filter::kTraits.tag);
	f.editor.syncCards();
	const void* first = f.editor.cardsForTest()[0];

	f.add(mod::dahdsr::kTraits.tag);
	f.editor.syncCards();
	REQUIRE(liveCards(f.editor) == 2);
	REQUIRE(f.editor.cardsForTest()[0] == first);
}

TEST_CASE("a macro knob and the engine agree, in both directions") {
	Fixture f;
	f.engine.setMacro(2, 0.4f);
	// **THE ENGINE→EDITOR DIRECTION NOW GOES THROUGH A PUBLISH, which is what a block does.** Before
	// AS 1b the editor read the engine, so an engine-side write was visible instantly; it reads a
	// MIRROR now, and the mirror is filled once a block. Standing in for that block here is the honest
	// spelling — the alternative, setting the macro through the bridge, would quietly delete the
	// engine→editor half this case exists to assert.
	f.bridge.primeForTest();
	REQUIRE(f.editor.macrosForTest()[2] != nullptr);

	rack::MacroControl control(f.bridge, 2, "Macro 3");
	REQUIRE(control.value() == doctest::Approx(0.4));
	control.setValue(0.9);
	// **AND THE EDITOR→ENGINE DIRECTION CROSSES THE FORWARD CHANNEL NOW (AS 2).** The edit is queued;
	// the drain is what a block or a flush performs. Asserting without it would be asserting that the
	// handoff does not exist, which is the property this commit adds.
	f.bridge.drain();
	REQUIRE(f.engine.macro(2) == doctest::Approx(0.9f));
}

TEST_CASE("a macro is never a destination, so its knob can never glow or draw a ring") {
	// Nothing may be routed AT a macro — it is where routes come FROM. `ModDisplay::isDestination`
	// false is what makes every other field meaningless, and `Knob` gates its whole amount road on it.
	Fixture f;
	rack::MacroControl control(f.bridge, 0, "Macro 1");
	f.mod.arm(true);
	f.mod.focus(backplane::Source::core(0));
	const device::ModDisplay d = control.display();
	REQUIRE_FALSE(d.isDestination);
	REQUIRE_FALSE(d.routable);
	REQUIRE(d.modulated == doctest::Approx(d.base));
}

TEST_CASE("laying out an empty rack and a full one both terminate and place what exists") {
	Fixture f;
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.resized();

	for (size_t i = 0; i < kAxes.deviceSlots; ++i) {
		f.add(fx::filter::kTraits.tag);
	}
	f.editor.syncCards();
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.resized();
	REQUIRE(liveCards(f.editor) == kAxes.deviceSlots);

	// Every card got a real rectangle — a card at zero width is a card nobody can press.
	for (ui::FxCard* c : f.editor.cardsForTest()) {
		REQUIRE(c != nullptr);
		REQUIRE(c->width() > 0);
		REQUIRE(c->height() > 0);
	}
}

TEST_CASE("a macro arm arms its own source, and only one is armed at a time") {
	// "I dont see modulation arrows for the macros?" — there were none. Every other part of the
	// modulation surface was present (destinations declared, routes stored, rings drawn, amounts
	// resolved) and nothing could ARM a source, so none of it was reachable by hand.
	Fixture f;
	REQUIRE_FALSE(f.mod.armed());

	rack::SourceArm a(f.mod, backplane::Source::core(2));
	rack::SourceArm b(f.mod, backplane::Source::core(5));

	a.processMouseDown(feathers::ui::MouseEventForTest());
	REQUIRE(f.mod.armed());
	REQUIRE(f.mod.source() == backplane::Source::core(2));

	// ANOTHER ARROW MOVES THE FOCUS AND STAYS ARMED — the mock's sticky rule, so a hand can route
	// from one source to several controls without re-arming between them.
	b.processMouseDown(feathers::ui::MouseEventForTest());
	REQUIRE(f.mod.armed());
	REQUIRE(f.mod.source() == backplane::Source::core(5));

	// THE ARMED ONE DISARMS.
	b.processMouseDown(feathers::ui::MouseEventForTest());
	REQUIRE_FALSE(f.mod.armed());
}

TEST_CASE("a chain longer than the window can be scrolled, and an empty one cannot") {
	// "the full generic ui spills off the bottom and i cant scroll". MEASURED: OWLTT's generic card is
	// 842px in a 560px window.
	//
	// THE LANE RUNS LEFT-TO-RIGHT NOW, so "taller than the window" is "longer than the window" and the
	// overflow is along x. The scroll accessor is deliberately named for the FLOW axis rather than for
	// a direction, so this case reads the same either way — which is the point of `laneExtent`.
	Fixture f;
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.resized();
	REQUIRE(f.editor.maxScroll() == doctest::Approx(0.0f)); // nothing in it yet

	// A FULL RACK RATHER THAN THREE, so the overflow does not depend on arithmetic about one device's
	// card width — a case that is only just over the edge is one that a cell-size change turns red for
	// a reason that has nothing to do with scrolling.
	for (size_t i = 0; i < kAxes.deviceSlots; ++i) {
		f.add(fx::filter::kTraits.tag);
	}
	f.editor.syncCards();
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.resized();
	REQUIRE(f.editor.maxScroll() > 0.0f);

	f.editor.scrollBy(100.0f);
	REQUIRE(f.editor.scroll() == doctest::Approx(100.0f));

	// IT CANNOT GO PAST THE END, in either direction.
	f.editor.scrollBy(100000.0f);
	REQUIRE(f.editor.scroll() == doctest::Approx(f.editor.maxScroll()));
	f.editor.scrollBy(-100000.0f);
	REQUIRE(f.editor.scroll() == doctest::Approx(0.0f));
}

TEST_CASE("the macro row does not scroll away, and the chain does") {
	// It is the surface's fixed head: a player reaching for a macro should not have to find it first,
	// and the arrows are how a source is armed while looking at a control far along the chain.
	//
	// **THIS CASE ASSERTED THE WRONG AXIS AND WOULD HAVE PASSED VACUOUSLY.** It checked only that the
	// macro's `y` was unchanged, which was the whole story while the lane ran top-to-bottom. The lane
	// now runs left-to-right, so scrolling moves `x` and `y` is unchanged BY CONSTRUCTION — the macro
	// row could have slid off the side and this would still have been green.
	//
	// SO IT ASSERTS BOTH AXES, and then asserts that a CARD did move — the second assertion is what
	// proves the first is not vacuous, which is this repo's standing rule about difference checks: a
	// test that cannot tell "held still" from "nothing happened" is measuring neither.
	Fixture f;
	for (size_t i = 0; i < kAxes.deviceSlots; ++i) {
		f.add(fx::filter::kTraits.tag);
	}
	f.editor.syncCards();
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.resized();
	REQUIRE(f.editor.maxScroll() > 0.0f);
	REQUIRE_FALSE(f.editor.cardsForTest().empty());

	const float knobX = f.editor.macrosForTest()[0]->x();
	const float knobY = f.editor.macrosForTest()[0]->y();
	const float cardX = f.editor.cardsForTest()[0]->x();

	f.editor.scrollBy(200.0f);
	CHECK(f.editor.macrosForTest()[0]->x() == doctest::Approx(knobX));
	CHECK(f.editor.macrosForTest()[0]->y() == doctest::Approx(knobY));
	// AND THE SCROLL REACHED SOMETHING. Without this the case is satisfied by a scroll that does not work.
	CHECK(f.editor.cardsForTest()[0]->x() < cardX);
}

TEST_CASE("arming a macro makes a device control routable — the knob asks the CONTROL, not itself") {
	// "when i click arrows, knobs dont glow". `GenericCard::setModulation` set the state on the KNOBS
	// and never on the `DeviceControl`s, and `Knob::mappable` does not decide — it asks
	// `control_->display().routable`, which computes `armed` from the control's own `mod_`. The knob
	// knew it was armed and the thing it asks said nothing was.
	//
	// ONE FACT, TWO OBJECTS, AND ONLY ONE OF THEM TOLD.
	Fixture f;
	const DeviceInstanceId slot = f.add(fx::filter::kTraits.tag);
	REQUIRE(slot != kNoInstance);
	f.editor.syncCards();
	// **BUILT DIRECTLY, BECAUSE THE FILTER HAS A CUSTOM CARD NOW.** The line above used to say it had
	// none and warned that the day the filter gained one, this case would otherwise start testing a
	// different object silently. That day arrived — and the warning is why this is an edit rather
	// than a silent change of subject. The claim under test is `GenericCard`'s, so the card is the
	// thing constructed; the engine still supplies a real slot for it to read.
	std::unique_ptr<ui::GenericCard> owned =
	    genericOver(f.bridge, f.mod, f.history, slot, fx::filter::kDescriptor);
	ui::GenericCard* card = owned.get();
	REQUIRE(card != nullptr);

	size_t modulatable = fx::filter::kDescriptor.params.size();
	for (size_t i = 0; i < fx::filter::kDescriptor.params.size(); ++i) {
		if (fx::filter::kDescriptor.params[i].modulatable) {
			modulatable = i;
			break;
		}
	}
	REQUIRE(modulatable < fx::filter::kDescriptor.params.size());

	const device::Modulatable& control = card->controlForTest(modulatable);
	REQUIRE(control.display().isDestination);
	REQUIRE_FALSE(control.display().routable); // nothing armed yet

	f.mod.focus(backplane::Source::core(0));
	f.mod.arm(true);
	REQUIRE(control.display().armed);
	REQUIRE(control.display().routable); // and NOW it may be reached

	f.mod.arm(false);
	REQUIRE_FALSE(control.display().routable);
}

// ---------------------------------------------------------------------------------------------
// THE `(+)` OPENS FEATHERS' ADD MENU (backlog KP: the rack's browser did not scroll and was cut off at
// some UI scales, so the rack now uses Feathers' browser, by category). It was
// a full-window takeover list with no scroll; it is now the popup the fx lane opens, one submenu per
// category (`ui::addCategorisedDevices`), placed by `ui::showMenuAt` so it fits the window it opens in.
//
// WHAT THESE CLAIM IS THE WIRING: that a press opens THAT menu, that a pick reaches the engine through the
// one road that also reports a refusal, and that the menu fits the window at every rung of the scale.

namespace {
/// PRESS THE (+) THE WAY A HAND DOES, AND ANSWER THE MENU IT OPENED (or null).
visage::PopupMenuFrame* pressAdd(Fixture& f) {
	rack::AddDeviceRow* row = f.editor.addRowForTest();
	REQUIRE(row != nullptr);
	row->onMouseDown().callback(ui::MouseEventForTest());
	return menu_check::newestPopup(*row);
}
} // namespace

TEST_CASE("backlog KP: pressing the + opens Feathers' add menu, one submenu per category, and the lane does not "
          "move") {
	Fixture f;
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.resized();
	rack::AddDeviceRow* row = f.editor.addRowForTest();
	const float widthWas = row->width();

	visage::PopupMenuFrame* popup = pressAdd(f);
	REQUIRE(popup != nullptr);
	// NO TAKEOVER: the menu is the browser now.
	CHECK(f.editor.takeoverForTest().empty());
	CHECK(row->width() == widthWas);

	// THE TOP LEVEL IS THE CATEGORIES, in the order `device::groupByCategory` gives the rack's own offer —
	// restated here from the registry rather than read back from the menu.
	const std::vector<const device::Descriptor*> offered = rack::offeredDevices(kDevices);
	const std::vector<device::CategoryGroup> groups = device::groupByCategory(offered);
	REQUIRE(groups.size() > 1);
	visage::PopupList* list = menu_check::rowsOf(*popup);
	REQUIRE(list != nullptr);
	REQUIRE(list->numOptions() == static_cast<int>(groups.size()));
	size_t devices = 0;
	for (size_t g = 0; g < groups.size(); ++g) {
		const visage::PopupMenu& heading = list->option(static_cast<int>(g));
		CHECK(heading.name().toUtf8() == device::categoryName(groups[g].category));
		CHECK(heading.hasOptions());
		devices += heading.options().size();
	}
	// AND EVERY DEVICE THE RACK CAN RUN IS IN IT, once.
	CHECK(devices == offered.size());
}

TEST_CASE("backlog KP: picking in the add menu adds the device") {
	Fixture f;
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.resized();
	visage::PopupMenuFrame* popup = pressAdd(f);
	REQUIRE(popup != nullptr);
	REQUIRE(menu_check::pickRow(*popup, fx::filter::kTraits.name));
	f.bridge.drain();
	CHECK(liveCards(f.editor) == 1);
	REQUIRE(f.engine.chainLength() == 1);
	CHECK(device::sameTag(f.bridge.descriptorOf(f.bridge.deviceAtPosition(0))->traits.tag, fx::filter::kTraits.tag));
}

TEST_CASE("backlog KP: a rack past the size it was built with keeps its add menu live, and the pick lands") {
	// IT GREYED THE MENU AND SAID "The rack is full" here until unbounded devices (step 6): a rack is never full
	// now. So the menu past the axes' size is the ordinary one, a pick from it lands, and nothing is said.
	Fixture f;
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	for (size_t i = 0; i < kAxes.deviceSlots; ++i) {
		REQUIRE(f.add(fx::filter::kTraits.tag) != kNoInstance);
	}
	f.editor.syncCards();
	f.editor.resized();
	visage::PopupMenuFrame* popup = pressAdd(f);
	REQUIRE(popup != nullptr);
	CHECK(menu_check::pickRow(*popup, fx::filter::kTraits.name));
	CHECK(f.bridge.chainLength() == kAxes.deviceSlots + 1);
	CHECK(f.editor.addDeviceByTag(fx::filter::kTraits.tag) != kNoInstance);
	CHECK(f.bridge.chainLength() == kAxes.deviceSlots + 2);
	CHECK(f.editor.notifierTextForTest().empty());
}

TEST_CASE("backlog KP: \"Add before…\" opens the same add menu, and its pick lands before the card") {
	Fixture f;
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	const DeviceInstanceId first = f.add(fx::owltt::kTraits.tag);
	REQUIRE(first != kNoInstance);
	f.editor.syncCards();
	f.editor.resized();
	f.editor.showSlotMenu(first);
	ui::FxCard* card = f.editor.cardOfDeviceForTest(first);
	REQUIRE(card != nullptr);
	visage::PopupMenuFrame* slotMenu = menu_check::newestPopup(f.editor);
	REQUIRE(slotMenu != nullptr);
	REQUIRE(menu_check::pickRow(*slotMenu, "Add before…"));
	visage::PopupMenuFrame* addMenu = menu_check::newestPopup(*card);
	REQUIRE(addMenu != nullptr);
	REQUIRE(addMenu != slotMenu);
	REQUIRE(menu_check::pickRow(*addMenu, fx::filter::kTraits.name));
	f.bridge.drain();
	REQUIRE(f.engine.chainLength() == 2);
	CHECK(f.bridge.deviceAtPosition(1) == first);
	CHECK(device::sameTag(f.bridge.descriptorOf(f.bridge.deviceAtPosition(0))->traits.tag, fx::filter::kTraits.tag));
}

TEST_CASE("backlog KP: at every rung of the UI scale the add menu opens inside the window, against the +") {
	// A HOST THAT CANNOT GIVE THE SCALED SIZE leaves the editor its old pixels, which at scale `s` is
	// `1/s` of the logical room. The old takeover list was taller than the window at 1x and had no scroll;
	// the menu is placed by `showMenuAt` against whatever room there is.
	for (const double scale : { 1.0, 1.125, 1.25, 1.375, 1.5, 1.75, 2.0 }) {
		CAPTURE(scale);
		Fixture f;
		const int w = static_cast<int>(rack::RackEditor::kWidth / scale);
		const int h = static_cast<int>(kEditorHeight / scale);
		f.editor.setBounds(0, 0, w, h);
		f.editor.resized();
		rack::AddDeviceRow* row = f.editor.addRowForTest();
		REQUIRE(pressAdd(f) != nullptr);
		const ui::MenuPlacement placed = ui::lastMenuPlacementForTest();
		REQUIRE(placed.shown);
		const visage::Bounds at = f.editor.relativeBounds(row);
		menu_check::checkHangsOff(placed, { at.x(), at.y(), at.width(), rack::AddDeviceRow::kBarHeight });
	}
}

TEST_CASE("at every rung of the UI scale a rack card's slot menu opens inside the window, against its dots") {
	// THE SLOT MENU WAS THE ONE MENU IN EITHER EDITOR SHOWN BY visage DIRECTLY, past `showMenuAt` — so it
	// was placed by visage's flip alone and never hung off its dots by our rules.
	for (const double scale : { 1.0, 1.5, 2.0 }) {
		CAPTURE(scale);
		Fixture f;
		const int w = static_cast<int>(rack::RackEditor::kWidth / scale);
		const int h = static_cast<int>(kEditorHeight / scale);
		f.editor.setBounds(0, 0, w, h);
		const DeviceInstanceId first = f.add(fx::owltt::kTraits.tag);
		REQUIRE(first != kNoInstance);
		f.editor.syncCards();
		f.editor.resized();
		ui::FxCard* card = f.editor.cardOfDeviceForTest(first);
		REQUIRE(card != nullptr);
		f.editor.showSlotMenu(first);
		const visage::Bounds dots = card->slotMenuBounds();
		const visage::Bounds at = f.editor.relativeBounds(card);
		menu_check::checkHangsOff(ui::lastMenuPlacementForTest(),
		                          { at.x() + dots.x(), at.y() + dots.y(), dots.width(), dots.height() });
	}
}

TEST_CASE("the rack editor stops nesting where a patch stops reopening, on both of its add roads") {
	// **THE OTHER HALF OF THE NESTING BOUND, AND WITHOUT IT THE GUARANTEE IS ONLY HALF TRUE.** The
	// runtime limit is the depth at which a patch stops being saveable (about 20 deep), so that a
	// thing a hand can build always
	// reopens. `ChainColumn` has refused a container past `rack::kMaxUsefulDepth` since that decision; a
	// rack device added inside a rack chain went through THIS editor and was never asked how deep it
	// was, so the rack plugin could still build the patch the bound exists to make unreachable.
	//
	// **BOTH ROADS, BECAUSE THERE ARE TWO AND ONE GUARD.** A direct call and a pick from the add menu both
	// land in `addDeviceByTag`, so one check covers both. A case that drove only the call would pass with the
	// menu unguarded.
	Fixture f;
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.resized();

	// AT THE TOP OF THE TREE THERE IS ROOM, and a rack inside a rack is an ordinary thing to want.
	CHECK_FALSE(f.editor.nestingFull());
	REQUIRE(f.editor.addDeviceByTag(composite::kTraits.tag) != kNoInstance);
	// **DRAINED BEFORE ANYTHING IS COUNTED.** The chain verbs are queued (this fixture's own note), so
	// a count taken here without the handoff is a count taken before the add happened — which is how
	// the first draft of this case read `1 == 0` and blamed the guard.
	f.bridge.drain();

	// **WALKED DOWN TO THE WALL BY THE NEST, WHICH IS WHAT THE BOUND IS READ FROM** — the same state a
	// twenty-deep player is in, rather than a flag set to make the test pass.
	ui::DeviceNest deep;
	while (static_cast<size_t>(deep.depth) < rack::kMaxUsefulDepth) {
		deep = ui::DeviceNest::under(deep, 1);
	}
	f.editor.setNest(deep);
	CHECK(f.editor.nestingFull());

	// A CONTAINER IS REFUSED, AND SAID SO — the same sentence the chain column draws, from the same
	// constant, because the two surfaces must not bound a hand differently OR word it differently.
	const size_t before = f.engine.chainLength();
	CHECK(f.editor.addDeviceByTag(composite::kTraits.tag) == kNoInstance);
	CHECK(f.editor.notifierTextForTest() == rack::kNestingFullMessage);
	f.bridge.drain();
	CHECK(f.engine.chainLength() == before);

	// **AND AN ORDINARY EFFECT AT THE SAME DEPTH IS UNTOUCHED**, which is what makes this a NESTING
	// bound rather than a dead editor. Without this the case would pass with `addDeviceByTag` refusing
	// everything.
	CHECK(f.editor.addDeviceByTag(fx::filter::kTraits.tag) != kNoInstance);
	f.bridge.drain();
	CHECK(f.engine.chainLength() == before + 1);

	// **THE MENU ROAD LANDS IN THE SAME PLACE**, driven as a hand drives it rather than by calling the
	// shared function again — the whole point is that the presentation cannot get past the guard.
	visage::PopupMenuFrame* popup = pressAdd(f);
	REQUIRE(popup != nullptr);
	const size_t heldBefore = f.engine.chainLength();
	REQUIRE(menu_check::pickRow(*popup, composite::kTraits.name));
	f.bridge.drain();
	CHECK(f.editor.notifierTextForTest() == rack::kNestingFullMessage);
	CHECK(f.engine.chainLength() == heldBefore);

	// ONE LEVEL SHALLOWER AND IT GOES IN — the bound sits ON the edge rather than near it, and
	// `test_rack_nesting.cpp` is where that edge is pinned against the LOADER itself.
	ui::DeviceNest room;
	while (static_cast<size_t>(room.depth) + 1 < rack::kMaxUsefulDepth) {
		room = ui::DeviceNest::under(room, 1);
	}
	f.editor.setNest(room);
	CHECK_FALSE(f.editor.nestingFull());
	CHECK(f.editor.addDeviceByTag(composite::kTraits.tag) != kNoInstance);
}

// ---------------------------------------------------------------------------------------------
// THE HORIZONTAL LANE'S ONE HARD INVARIANT: a card gets the lane's height and no more, so every knob
// it draws has to FIT in that height. Vertically this could never fail — a card grew until its knobs
// fitted. Horizontally the height is fixed and the card grows sideways instead, which means a device
// with more knobs than fit in one column-height has nowhere to put the remainder.
//
// THIS IS THE MEASUREMENT THAT DECIDES WHETHER THE BOUNDED CARD IS NEEDED, rather than an argument
// that it is — AND IT ANSWERED THE OPPOSITE OF WHAT WAS PREDICTED, which is why it was made.
//
// THE PREDICTION WAS THAT KNOBS WOULD NOT FIT: OWLTT's generic card is 842px tall in a 560px window,
// so a lane that hands out a fixed height looked like it must clip. **It does not, and cannot.** The
// card grows along the FLOW axis, so a device with more knobs gets a WIDER card and every knob still
// lands inside it. This case passes for every device in the catalogue and always will.
//
// WHAT THE MEASUREMENT ACTUALLY FOUND, at a 720x560 window (lane height 430):
//
//     knobs   card width
//        11          296
//        12          296
//        13          388
//        15          388
//        21          572
//        28          664     <- one device is the whole window
//       127         2964     <- 4.1x the window, for ONE device
//
// SO THE BOUND IS NOT A CLIPPING BOUND, IT IS A USABILITY ONE, and that is a different design. A card
// that cannot clip has no error state to guard against; it just becomes unreadable, and no assertion
// about geometry will ever catch that. The cap has to be a chosen number with the remainder going to
// the expanded view — which makes the expanded view mandatory for a reason nobody can measure their
// way to.

TEST_CASE("in the horizontal lane every knob a card draws is inside that card") {
	size_t seatedFY = 0; // backlog FY: an add that fails is a device this sweep never met, so it is counted
	for (const device::Descriptor& d : kDevices) {
		Fixture f;
		if (f.add(d.traits.tag) == kNoInstance) {
			continue;
		}
		++seatedFY;
		f.editor.syncCards();
		f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
		f.editor.resized();

		const std::vector<ui::FxCard*> cards = f.editor.cardsForTest();
		REQUIRE_FALSE(cards.empty());
		const ui::GenericCard* card = nullptr;
		for (const auto& c : cards) {
			if (auto* g = dynamic_cast<const ui::GenericCard*>(c); g != nullptr) {
				card = g;
				break;
			}
		}
		// A DEVICE WITH A CUSTOM CARD IS NOT A FAILURE OF THIS CASE, it is a device this case has
		// nothing to say about: the invariant asserted below is the generic grid's packing rule.
		if (card == nullptr) {
			continue;
		}

		CAPTURE(d.traits.tag);
		CAPTURE(card->width());
		CAPTURE(card->height());

		for (const auto& knob : card->knobsForTest()) {
			if (knob == nullptr) {
				continue;
			}
			CAPTURE(knob->x());
			CAPTURE(knob->y());
			CAPTURE(knob->height());
			// **WITHIN THE CARD'S WIDTH, AND WITHIN ITS CONTENT — NOT WITHIN ITS HEIGHT.** A generic
			// card is one scrolling column now (by design), so a row below the fold sitting past
			// `height()` is what SCROLLED means,
			// not a layout fault. What must still hold is that nothing is laid out above the top, that
			// the column stays inside the card's width, and that every row is inside the CONTENT the
			// scroll can actually reach — a knob past `contentHeight()` would be unreachable by any
			// amount of scrolling, which is the real version of the bug this case was written for.
			CHECK(knob->y() + card->bodyScroll() >= 0);
			CHECK(knob->y() + knob->height() + card->bodyScroll() <= card->contentHeight());
			CHECK(knob->x() + knob->width() <= card->width());
		}
	}
	CHECK(seatedFY == feathers::kDeviceCount); // backlog FY: EVERY registered device was seated, not "the loop ran"
}

// ---------------------------------------------------------------------------------------------
// THE BOUNDED CARD. The measurement above says a horizontal card cannot clip — it grows sideways —
// so the cap is a chosen number and the card's duty is to SAY what it is not showing.

TEST_CASE("a card past the cap shows what fits, hides the rest, and says how many") {
	Fixture f;
	// THE BIGGEST DEVICE IN THE CATALOGUE, found by asking rather than by naming one: a case pinned to
	// a device tag goes quietly vacuous the day that device shrinks.
	// THE BIGGEST DEVICE THAT STILL GETS THE GENERIC GRID. The cap is `GenericCard`'s packing rule, so
	// a device with a custom card on the seam is outside this case entirely — and `drawn`, which used
	// to be the biggest, is now one of those. Asking `hasCustomCard` rather than naming a tag keeps
	// this case correct as more cards converge.
	const device::Descriptor* biggest = nullptr;
	for (const device::Descriptor& d : kDevices) {
		// **NO LONGER FILTERED ON `hasCustomCard`, because that now excludes EVERYTHING.** This skipped
		// devices with a custom card, on the reasoning that the editor would not give them the generic
		// grid — true until the filter converged and the set of card-less devices became empty. The
		// card is constructed directly below instead, so which device supplies the descriptor is a
		// question about param COUNT alone, which is what this case was ever choosing on.
		// **AND NOT A DEVICE WITH GATED CONTROLS (2026-10-30, the Pattern device).** `hiddenCount()` counts every
		// row the card is not showing, and a row its gate hides (the honesty rule) is one of those — so the biggest
		// gated device reads as "hiding" its gated rows, which is not the overflow this case is about. Every device
		// this case had chosen until then was ungated, so the filter keeps its subject.
		bool gated = false;
		for (const device::ParamDesc& p : d.params) {
			gated = gated || p.gateControl >= 0;
		}
		if (gated) {
			continue;
		}
		if (biggest == nullptr || d.params.size() > biggest->params.size()) {
			biggest = &d;
		}
	}
	REQUIRE(biggest != nullptr);
	const DeviceInstanceId slot = f.add(biggest->traits.tag);
	REQUIRE(slot != kNoInstance);

	// **CONSTRUCTED, NOT FOUND IN THE EDITOR.** Every registered device has a custom card now, so the
	// editor produces no `GenericCard` at all — see `genericOver`. The claim here is the generic
	// grid's own (the cap, the overflow, the head's `+N`), and it is a property of the class rather
	// than of any device that happens to lack a card.
	std::unique_ptr<ui::GenericCard> owned =
	    genericOver(f.bridge, f.mod, f.history, slot, *biggest);
	ui::GenericCard* card = owned.get();
	REQUIRE(card != nullptr);
	card->setBounds(0, 0, static_cast<int>(card->laneExtent(rack::RackEditor::kHeight)),
	                static_cast<int>(rack::RackEditor::kHeight));
	card->resized();
	CAPTURE(card->knobsForTest().size());
	CAPTURE(card->width());

	// IT FITS THE WINDOW NOW. Uncapped this device measured 2964px against a 720px window.
	CHECK(card->width() <= rack::RackEditor::kWidth);

	// **AND IT HIDES NOTHING, BECAUSE IT SCROLLS.** This required `hiddenCount() > 0` — the card
	// showed what fit and sent the rest to the expanded view, so "it says so rather than looking
	// complete while missing a hundred controls" was the guarantee that mattered. A one-column
	// scrolling list makes a stronger promise: every control is PLACED and reachable by scrolling, so
	// there is nothing to announce. The claim that replaces it is that the list is genuinely longer
	// than the card and the card knows it.
	CHECK(card->hiddenCount() == 0);
	CHECK(card->scrollable());
	CHECK(card->contentHeight() > card->height());

	// THE HIDDEN KNOBS ARE INVISIBLE, NOT MERELY UNPLACED — a knob that stops being laid out keeps the
	// bounds it last had and goes on drawing on top of one that IS placed.
	size_t visible = 0;
	for (const auto& knob : card->knobsForTest()) {
		if (knob != nullptr && knob->isVisible()) {
			++visible;
		}
	}
	CHECK(visible + card->hiddenCount() == card->knobsForTest().size());
}

TEST_CASE("a card that shows everything offers no expand verb and hides nothing") {
	Fixture f;
	// THE HONESTY RULE, at the card: a control is enabled iff it does something. A device whose
	// controls all fit has nothing behind an expand button, so there is no button.
	const device::Descriptor* smallest = nullptr;
	for (const device::Descriptor& d : kDevices) {
		// **NO LONGER FILTERED ON `hasCustomCard`, because that now excludes EVERYTHING.** This skipped
		// devices with a custom card, on the reasoning that the editor would not give them the generic
		// grid — true until the filter converged and the set of card-less devices became empty. The
		// card is constructed directly below instead, so which device supplies the descriptor is a
		// question about param COUNT alone, which is what this case was ever choosing on.
		if (smallest == nullptr || d.params.size() < smallest->params.size()) {
			smallest = &d;
		}
	}
	REQUIRE(smallest != nullptr);
	const DeviceInstanceId slot = f.add(smallest->traits.tag);
	REQUIRE(slot != kNoInstance);

	// **CONSTRUCTED, NOT FOUND IN THE EDITOR.** Every registered device has a custom card now, so the
	// editor produces no `GenericCard` at all — see `genericOver`. The claim here is the generic
	// grid's own (the cap, the overflow, the head's `+N`), and it is a property of the class rather
	// than of any device that happens to lack a card.
	std::unique_ptr<ui::GenericCard> owned =
	    genericOver(f.bridge, f.mod, f.history, slot, *smallest);
	ui::GenericCard* card = owned.get();
	REQUIRE(card != nullptr);
	card->setBounds(0, 0, static_cast<int>(card->laneExtent(rack::RackEditor::kHeight)),
	                static_cast<int>(rack::RackEditor::kHeight));
	card->resized();
	CAPTURE(card->knobsForTest().size());
	CHECK(card->hiddenCount() == 0);
	CHECK(card->expandBounds().width() == doctest::Approx(0.0f));
}

// ---------------------------------------------------------------------------------------------
// THE EXPAND VERB, END TO END — the first `SpansGesture` caller the surface host has ever had.
//
// UNTIL NOW `kindFor(SpansGesture)` ANSWERED `Inline` AND THAT WAS CORRECT: the rack owned only a
// takeover, and a takeover cannot serve a gesture that spans surfaces because it removes the editor
// the gesture's other end is on. The button existed and led nowhere. This is the road existing.

TEST_CASE("the rack's canvas sits above the macros; its partition and fold ride the patch and a load moves it on the beat") {
	// In the rack the canvas sits on top, above the macros, and its partition size and expanded state
	// persist with the patch.
	Fixture f;
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.resized();
	ui::ExpandedCanvas& canvas = f.editor.canvas();
	// FOLDED BY DEFAULT — the strip across the top, the macros just under it, exactly as before the canvas.
	REQUIRE(canvas.collapsed());
	CHECK(canvas.height() == doctest::Approx(34.0f));
	CHECK(f.editor.macrosForTest()[0]->y() < ui::ExpandedCanvas::kExtent);
	REQUIRE(canvas.setCollapsed(false));
	// THE BAND ACROSS THE TOP, the macros under it.
	CHECK(canvas.y() == doctest::Approx(0.0f));
	// AT ITS DEFAULT BAND: `kExtent` under the lane's cap (backlog LX).
	CHECK(canvas.height() == doctest::Approx(canvas.defaultExtent()));
	CHECK(canvas.width() == doctest::Approx(rack::RackEditor::kWidth));
	REQUIRE_FALSE(f.editor.macrosForTest().empty());
	CHECK(f.editor.macrosForTest()[0]->y() >= canvas.height());

	// THE HAND WRITES THE PATCH — the unfold above already did, then a fold, then a committed drag.
	CHECK(f.bridge.canvasView() == rack::CanvasView { canvas.defaultExtent(), false });
	REQUIRE(canvas.toggleCollapsed());
	CHECK(f.bridge.canvasView().collapsed);
	CHECK(f.editor.macrosForTest()[0]->y() < ui::ExpandedCanvas::kExtent); // the macros came up
	REQUIRE(canvas.toggleCollapsed());
	REQUIRE(canvas.setOpenExtent(300.0f));
	canvas.onExtentCommit();
	CHECK(f.bridge.canvasView() == rack::CanvasView { 300.0f, false });
	// AND IT SURVIVES THE PATCH ROAD.
	const rack::RackPatch saved = f.bridge.patch();
	CHECK(saved.canvas == rack::CanvasView { 300.0f, false });

	// THE PATCH MOVES THE HAND: a load's view reaches the frame on the beat.
	f.bridge.setCanvasView({ 200.0f, true });
	CHECK(f.editor.syncCanvas());
	CHECK(canvas.collapsed());
	CHECK(canvas.openExtent() == doctest::Approx(200.0f));
	CHECK_FALSE(f.editor.syncCanvas()); // nothing left to move
	f.bridge.setCanvasView({ 200.0f, false });
	CHECK(f.editor.syncCanvas());
	CHECK_FALSE(canvas.collapsed());
	CHECK(canvas.height() == doctest::Approx(200.0f));
	CHECK(f.editor.canvasExtent() == doctest::Approx(200.0f));

	// AND A BEAT DURING A PARTITION DRAG LEAVES THE HAND ALONE (the drag was jumpy and erratic — the beat
	// was re-applying the patch's extent under the hand). A top band grows as the hand moves DOWN.
	const auto at = [](float x, float y) {
		visage::MouseEvent e;
		e.position = { x, y };
		e.window_position = { x, y };
		return e;
	};
	const float gripY = canvas.gripBounds().y() + 2.0f;
	canvas.onMouseDown().callback(at(300.0f, gripY));
	REQUIRE(canvas.draggingExtentForTest());
	canvas.onMouseDrag().callback(at(300.0f, gripY + 60.0f));
	CHECK(canvas.openExtent() == doctest::Approx(260.0f));
	CHECK_FALSE(f.editor.syncCanvas()); // the patch still says 200 and the beat does not apply it
	CHECK(canvas.openExtent() == doctest::Approx(260.0f));
	canvas.onMouseUp().callback(at(300.0f, gripY + 60.0f));
	CHECK(f.bridge.canvasView().extent == doctest::Approx(260.0f));
	CHECK_FALSE(f.editor.syncCanvas());
}

TEST_CASE("the rack's canvas opens into the room its window already has — fitted when there is room, left alone when "
          "there is not") {
	// Reported 2026-09-17: "opening/closing the canvas reproportions the window. we should proportion/resize so
	// it fits inside the original window provided its large enough." The rack's half of the rule: RackGui
	// answers `windowRoomHeight` from the window; here the answer is set by hand, so the rule is asked without
	// a window. The growth the other half asks the host for is pinned in test_gui_windowed.
	Fixture f;
	ui::ExpandedCanvas& canvas = f.editor.canvas();
	REQUIRE(canvas.collapsed());
	const float saved = canvas.openExtent();
	REQUIRE(saved > 300.0f);

	SUBCASE("room under the saved band and over the minimum: the band takes the room") {
		f.editor.windowRoomHeight = [] { return static_cast<double>(rack::RackEditor::kHeight) + 300.0; };
		REQUIRE(canvas.setCollapsed(false));
		CHECK(canvas.openExtent() == doctest::Approx(300.0f));
	}
	SUBCASE("room over the saved band: the band keeps its size") {
		f.editor.windowRoomHeight = [saved] { return static_cast<double>(rack::RackEditor::kHeight) + saved + 400.0; };
		REQUIRE(canvas.setCollapsed(false));
		CHECK(canvas.openExtent() == doctest::Approx(saved));
	}
	SUBCASE("room under the minimum is not room: the band keeps its size and the window must grow") {
		f.editor.windowRoomHeight = [] { return static_cast<double>(rack::RackEditor::kHeight) + 60.0; };
		REQUIRE(canvas.setCollapsed(false));
		CHECK(canvas.openExtent() == doctest::Approx(saved));
	}
	SUBCASE("no window to ask: nothing is fitted") {
		REQUIRE(canvas.setCollapsed(false));
		CHECK(canvas.openExtent() == doctest::Approx(saved));
	}
}

TEST_CASE("the rack serves a spanning gesture with the canvas, and with the overlay once it is collapsed") {
	Fixture f;
	CHECK(f.editor.surfaces().kindFor(ui::SurfaceNeed::SelfContained) == ui::SurfaceKind::Takeover);
	// THE ANSWER THAT USED TO BE A REFUSAL, and since 2026-09-13 is the canvas while it is showing —
	// which a fresh editor is not (folded by default, 2026-09-14), so it is opened here.
	CHECK(f.editor.surfaces().kindFor(ui::SurfaceNeed::SpansGesture) == ui::SurfaceKind::Overlay);
	REQUIRE(f.editor.canvas().setCollapsed(false));
	REQUIRE(f.editor.canvas().showing());
	CHECK(f.editor.surfaces().kindFor(ui::SurfaceNeed::SpansGesture) == ui::SurfaceKind::Canvas);
	// COLLAPSED, THE OVERLAY — minimised, the existing popup behaviour is the right one.
	REQUIRE(f.editor.canvas().setCollapsed(true));
	CHECK(f.editor.surfaces().kindFor(ui::SurfaceNeed::SpansGesture) == ui::SurfaceKind::Overlay);
	// AND A TAKEOVER IS NEVER OFFERED FOR IT, whichever way the canvas is folded.
	REQUIRE(f.editor.canvas().setCollapsed(false));
	CHECK(f.editor.surfaces().kindFor(ui::SurfaceNeed::SpansGesture) != ui::SurfaceKind::Takeover);
}

TEST_CASE("in the rack, arming a source lights every destination knob on an open panel — the device's own panel and the generic grid, on the canvas and floating") {
	// The regression: modulation glow did not reach the expanded panel controls in the floating window
	// or the canvas. Feathers' side is covered in the widget suite; this is the rack's: both kinds of
	// panel the rack builds, on both hosts, with a macro armed and then a device output.
	Fixture f;
	const device::Descriptor* converged = nullptr;
	const device::Descriptor* generic = nullptr;
	for (const device::Descriptor& d : kDevices) {
		if (converged == nullptr && ui::hasCustomCard(d) && ui::hasExpandedView(d)) {
			converged = &d;
		}
		if (!ui::hasExpandedView(d) && (generic == nullptr || d.params.size() > generic->params.size())) {
			generic = &d;
		}
	}
	REQUIRE(converged != nullptr);
	REQUIRE(generic != nullptr);
	// A MODULATOR FIRST IN THE CHAIN, so its output is UPSTREAM of both panels' devices — the routing
	// rule (sources must be upstream of destinations) refuses a device output aimed at anything
	// before it, and a panel whose knobs stay dark for that reason is the rule working, not the glow
	// failing. (The first draft added it last, and read the rule as the defect.)
	DeviceInstanceId withOutput = kNoInstance;
	const device::Descriptor* modulator = nullptr;
	for (const device::Descriptor& d : kDevices) {
		// A MODULATOR, NOT MERELY A SOURCE — SKLITTER is an fx that also publishes, and is registered first.
		if (d.outputCount() > 0 && d.mod != nullptr && d.audio == nullptr) {
			modulator = &d;
			withOutput = f.add(d.traits.tag);
			break;
		}
	}
	REQUIRE(withOutput != kNoInstance);
	INFO(modulator->traits.tag); // which device this positional pick landed on, said out loud on a red
	REQUIRE(f.add(converged->traits.tag) != kNoInstance);
	REQUIRE(f.add(generic->traits.tag) != kNoInstance);
	f.editor.syncCards();
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.resized();
	REQUIRE(f.editor.cardsForTest().size() >= 3);

	std::function<void(visage::Frame*, std::vector<ui::Knob*>*)> collect = [&](visage::Frame* at, std::vector<ui::Knob*>* out) {
		if (auto* knob = dynamic_cast<ui::Knob*>(at)) {
			out->push_back(knob);
		}
		for (visage::Frame* child : at->children()) {
			collect(child, out);
		}
	};
	const auto litOf = [&](visage::Frame* panel, size_t* destinations, size_t* unwatched) {
		std::vector<ui::Knob*> knobs;
		collect(panel, &knobs);
		*destinations = 0;
		*unwatched = 0;
		size_t lit = 0;
		for (ui::Knob* knob : knobs) {
			if (!knob->isModDestination()) {
				continue;
			}
			++*destinations;
			lit += knob->haloLit() ? 1 : 0;
			*unwatched += f.mod.watches(knob) ? 0 : 1;
		}
		return lit;
	};
	const auto check = [&](bool onCanvas, backplane::Source source) {
		CAPTURE(onCanvas);
		(void)f.editor.canvas().setCollapsed(!onCanvas);
		(void)f.mod.arm(false);
		for (ui::FxCard* card : f.editor.cardsForTest()) {
			REQUIRE(card != nullptr);
			if (source.isDevice() && card->device() == source.device) {
				continue; // a device is not upstream of itself: its own panel is rightly dark
			}
			REQUIRE(card->onExpand);
			card->onExpand();
			ui::ExpandedPanel* panel = f.editor.overlayForTest().panelForTest(); // ONE overlay, band open or folded
			REQUIRE(panel != nullptr);
			size_t destinations = 0;
			size_t unwatched = 0;
			CHECK(litOf(panel, &destinations, &unwatched) == 0); // nothing armed yet
			REQUIRE(destinations > 0);
			(void)f.mod.arm(true);
			f.mod.focus(source);
			REQUIRE(f.mod.armed());
			// THE CARD'S OWN KNOBS LIGHT FOR THIS SOURCE — the control, so a panel that does not is the
			// panel's failure and not the source's.
			size_t cardDestinations = 0;
			size_t cardUnwatched = 0;
			const size_t cardLit = litOf(card, &cardDestinations, &cardUnwatched);
			CAPTURE(cardLit);
			CAPTURE(cardDestinations);
			CHECK(litOf(panel, &destinations, &unwatched) == destinations);
			CHECK(unwatched == 0);
			(void)f.mod.arm(false);
		}
		f.editor.overlayForTest().dismiss();
	};
	check(true, backplane::Source::core(0));
	check(false, backplane::Source::core(0));
	// A DEVICE OUTPUT AS THE SOURCE, from the head of the chain.
	check(true, backplane::Source::output(withOutput, 0));
	check(false, backplane::Source::output(withOutput, 0));
}

TEST_CASE("pressing a capped card's expand opens a panel holding every control") {
	Fixture f;
	// THE BIGGEST DEVICE THAT STILL GETS THE GENERIC GRID. The cap is `GenericCard`'s packing rule, so
	// a device with a custom card on the seam is outside this case entirely — and `drawn`, which used
	// to be the biggest, is now one of those. Asking `hasCustomCard` rather than naming a tag keeps
	// this case correct as more cards converge.
	// **WHATEVER CARD THE RACK BUILDS, and the biggest device gets one that scrolls.** This used to
	// insist on a `GenericCard` and skipped every device with a custom one; with the filter converged
	// that filter excludes everything and the case had no subject at all.
	//
	// THE GENERIC-NESS WAS INCIDENTAL. What is asserted below — a card that scrolls still offers its
	// panel, and the panel lands on the OVERLAY rather than the takeover — is true of any card in this
	// lane, and it is the editor's wiring that has to hold it up. So this asks the editor for its card
	// rather than reaching for one class of them, which is also the form that cannot go vacuous again.
	const device::Descriptor* biggest = nullptr;
	for (const device::Descriptor& d : kDevices) {
		if (biggest == nullptr || d.params.size() > biggest->params.size()) {
			biggest = &d;
		}
	}
	REQUIRE(biggest != nullptr);
	REQUIRE(f.add(biggest->traits.tag) != kNoInstance);
	f.editor.syncCards();
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.resized();

	ui::FxCard* card = nullptr;
	for (ui::FxCard* c : f.editor.cardsForTest()) {
		if (c != nullptr) {
			card = c;
			break;
		}
	}
	REQUIRE(card != nullptr);
	// AND IT REALLY OFFERS ONE. Without this the press below could land on a card with no expand verb
	// and the overlay assertion would be about nothing.
	REQUIRE(card->expandBounds().width() > 0.0f);
	REQUIRE(card->onExpand);
	// **THE PANEL IS OFFERED FOR ITS OWN SAKE, not because controls are missing.** This required
	// `hiddenCount() > 0` when the card dropped what would not fit; it scrolls now and drops nothing,
	// and the expanded view is still where a device's whole control set gets room to be read at once.
	REQUIRE_FALSE(f.editor.overlayForTest().open());
	REQUIRE(f.editor.canvas().setCollapsed(false)); // opened: a fresh canvas is folded

	card->onExpand();

	// THE OVERLAY, NOT THE TAKEOVER. If this were a takeover the editor would be gone and the macro
	// arrows with it, which is the whole reason the need is declared rather than the kind.
	CHECK(f.editor.overlayForTest().open());
	CHECK(f.editor.takeoverForTest().empty());

	// FOLDING THE CANVAS LEAVES THE PANEL UP (the band is more of the surface, not a
	// fence), and the same expand again CLOSES that device's panel rather than
	// twinning or replacing it.
	REQUIRE(f.editor.canvas().setCollapsed(true));
	CHECK(f.editor.overlayForTest().open());
	CHECK(f.editor.expandedIdForTest() != ui::kNoSurface);
	card->onExpand();
	CHECK(f.editor.overlayForTest().panelCount() == 0);
}

TEST_CASE("expand toggle, the rack editor: a card's expand mark closes its device's open panel, leaves another "
          "device's up, and reopens it where it was left") {
	// Clicking a card's window-expand while its window is already expanded closes it. Two devices with
	// panels — one custom and one generic is not required; any two with an
	// expand verb will do, and each is counted by its own device, read off the panel.
	Fixture f;
	std::vector<const device::Descriptor*> picked;
	for (const device::Descriptor& d : kDevices) {
		if (ui::hasExpandedView(d) && picked.size() < 2) {
			picked.push_back(&d);
		}
	}
	REQUIRE(picked.size() == 2);
	const DeviceInstanceId idA = f.add(picked[0]->traits.tag);
	const DeviceInstanceId idB = f.add(picked[1]->traits.tag);
	REQUIRE(idA != kNoInstance);
	REQUIRE(idB != kNoInstance);
	f.editor.syncCards();
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.resized();
	ui::FxCard* cardA = f.editor.cardOfDeviceForTest(idA);
	ui::FxCard* cardB = f.editor.cardOfDeviceForTest(idB);
	REQUIRE(cardA != nullptr);
	REQUIRE(cardB != nullptr);
	REQUIRE(cardA->onExpand);
	REQUIRE(cardB->onExpand);
	ui::ExpandedOverlay& overlay = f.editor.overlayForTest();
	const auto panelOf = [&overlay](DeviceInstanceId device) {
		for (const ui::ExpandedOverlay::PanelId id : overlay.panelIdsForTest()) {
			const ui::ExpandedView* content = overlay.contentForTest(id);
			if (content != nullptr && content->watchedDevice() == device) {
				return id;
			}
		}
		return ui::ExpandedOverlay::kNoPanel;
	};
	const auto panelsOf = [&overlay](DeviceInstanceId device) {
		size_t n = 0;
		for (const ui::ExpandedOverlay::PanelId id : overlay.panelIdsForTest()) {
			const ui::ExpandedView* content = overlay.contentForTest(id);
			n += content != nullptr && content->watchedDevice() == device ? 1 : 0;
		}
		return n;
	};

	// OPEN, CLOSE, OPEN.
	cardA->onExpand();
	CHECK(panelsOf(idA) == 1);
	cardA->onExpand();
	CHECK(panelsOf(idA) == 0);
	CHECK(overlay.lastDismissalForTest() == ui::ExpandedOverlay::Dismissal::ExpandToggle);
	CHECK(f.editor.expandedIdForTest() == ui::kNoSurface); // the editor's id was retired with it
	cardA->onExpand();
	CHECK(panelsOf(idA) == 1);

	// TWO UP: toggling A leaves B.
	cardB->onExpand();
	REQUIRE(panelsOf(idB) == 1);
	cardA->onExpand();
	CHECK(panelsOf(idA) == 0);
	CHECK(panelsOf(idB) == 1);
	cardB->onExpand();
	CHECK(panelsOf(idB) == 0);
	REQUIRE_FALSE(overlay.open());

	// THE PLACEMENT IS KEPT: moved, closed by the mark, reopened by it where it was left.
	cardA->onExpand();
	const ui::ExpandedOverlay::PanelId first = panelOf(idA);
	REQUIRE(first != ui::ExpandedOverlay::kNoPanel);
	const visage::Bounds opened = overlay.panelBounds(first);
	overlay.beginPanelDrag();
	overlay.dragPanelBy({ 31.0f, 17.0f });
	const visage::Bounds moved = overlay.panelBounds(first);
	REQUIRE((moved.x() != opened.x() || moved.y() != opened.y()));
	cardA->onExpand();
	REQUIRE(panelsOf(idA) == 0);
	cardA->onExpand();
	const ui::ExpandedOverlay::PanelId again = panelOf(idA);
	REQUIRE(again != ui::ExpandedOverlay::kNoPanel);
	CHECK(overlay.panelBounds(again).x() == doctest::Approx(moved.x()));
	CHECK(overlay.panelBounds(again).y() == doctest::Approx(moved.y()));
}

TEST_CASE("the rack: with the band open a panel arrives in it, drags down over the lane, and a fold pulls it back onto the editor") {
	// The band is an extension of the UI where windows can float, out of the way of the main surface;
	// when it is open, windows prefer to go there. The rack's band is at the TOP.
	Fixture f;
	const device::Descriptor* converged = nullptr;
	for (const device::Descriptor& d : kDevices) {
		if (ui::hasCustomCard(d) && ui::hasExpandedView(d)) {
			converged = &d;
			break;
		}
	}
	REQUIRE(converged != nullptr);
	REQUIRE(f.add(converged->traits.tag) != kNoInstance);
	f.editor.syncCards();
	REQUIRE_FALSE(f.editor.cardsForTest().empty());
	ui::FxCard* card = f.editor.cardsForTest()[0];
	REQUIRE(card != nullptr);
	REQUIRE(card->onExpand);
	REQUIRE(f.editor.canvas().setCollapsed(false));
	// THE HOST'S HALF: the window grows by the band, as the DAW is asked to.
	const auto hostLays = [&f] {
		f.editor.setBounds(0, 0, rack::RackEditor::kWidth,
		                   rack::RackEditor::kHeight + static_cast<int>(f.editor.canvasExtent()));
		f.editor.resized();
	};
	hostLays();
	ui::ExpandedOverlay& overlay = f.editor.overlayForTest();
	// THE BAND'S FLOOR IN THE OVERLAY'S PIXELS, restated from the two frames rather than asked of the
	// code under test.
	const ui::ExpandedCanvas& canvas = f.editor.canvas();
	const visage::Point offset = canvas.positionInWindow() - overlay.positionInWindow();
	const visage::Bounds floor = canvas.contentBounds();
	const visage::Bounds region { offset.x + floor.x(), offset.y + floor.y(), floor.width(), floor.height() };
	CHECK(region.bottom() <= f.editor.canvasExtent());

	card->onExpand();
	REQUIRE(overlay.open());
	const visage::Bounds arrived = overlay.panelBounds();
	CHECK(arrived.x() == doctest::Approx(region.x()));
	CHECK(arrived.y() == doctest::Approx(std::min(region.y(), overlay.height() - arrived.height())));

	// DOWN OVER THE LANE, as far as the editor's foot lets it go — a band's floor would have stopped it
	// at the band's foot.
	overlay.beginPanelDrag();
	overlay.dragPanelBy({ 0.0f, f.editor.canvasExtent() + 20.0f });
	const float expectedY = std::min(arrived.y() + f.editor.canvasExtent() + 20.0f, overlay.height() - arrived.height());
	CHECK(overlay.panelBounds().y() == doctest::Approx(expectedY));
	CHECK(overlay.panelBounds().bottom() > f.editor.canvasExtent() + 20.0f);
	CHECK(overlay.panelBounds().y() > arrived.y());
	// AND NEVER OFF THE EDITOR, however far it is thrown.
	overlay.beginPanelDrag();
	overlay.dragPanelBy({ 9000.0f, 9000.0f });
	CHECK(overlay.panelBounds().right() <= static_cast<float>(f.editor.width()) + 0.5f);
	CHECK(overlay.panelBounds().bottom() <= static_cast<float>(f.editor.height()) + 0.5f);

	// FOLD with the panel at the foot: the window shrinks by the band and the panel comes up with it.
	const float footBefore = overlay.panelBounds().bottom();
	REQUIRE(f.editor.canvas().setCollapsed(true));
	hostLays();
	REQUIRE(static_cast<float>(f.editor.height()) < footBefore); // the surface really did shrink under it
	CHECK(overlay.open());
	CHECK(overlay.panelBounds().bottom() == doctest::Approx(static_cast<float>(f.editor.height())));
	REQUIRE(overlay.panelForTest() != nullptr);
	CHECK(overlay.panelForTest()->y() == doctest::Approx(overlay.panelBounds().y()));
}

TEST_CASE("an overlay-mounted panel scrolls, because a panel is not a promise of room") {
	// The largest device is ~2280px of grid against a few hundred of panel. Without this the expanded
	// view repeats the bug it was opened to fix: showing part of a device while looking complete.
	auto content = std::make_unique<visage::Frame>();
	content->setBounds(0, 0, 400, 2000);
	ui::MountedFrame mount { std::move(content), "BIG", visage::Point { 400.0f, 300.0f } };
	mount.setBounds(0, 0, 400, 300);
	mount.resized();

	REQUIRE(mount.maxScroll() == doctest::Approx(1700.0f));
	mount.scrollBy(500.0f);
	CHECK(mount.scroll() == doctest::Approx(500.0f));
	CHECK(mount.contentForTest()->y() == -500);

	// IT CANNOT GO PAST EITHER END.
	mount.scrollBy(100000.0f);
	CHECK(mount.scroll() == doctest::Approx(mount.maxScroll()));
	mount.scrollBy(-100000.0f);
	CHECK(mount.scroll() == doctest::Approx(0.0f));
}

// ---------------------------------------------------------------------------------------------
// Menus (the browser) and the extended knob list popped up UNDER the effect cards.
//
// VISAGE HAS NO Z-ORDER: sibling order IS depth, so the only way to be in front is to be the LAST
// child. The notifier and the surface host are built in the constructor; a card is added by
// `syncCards` whenever a device arrives — so every card became a later sibling than both.
//
// THIS IS ASSERTED ON THE TREE RATHER THAN ON PIXELS, which is the only way it can be asserted at
// all: nothing in this repo renders the rack, and "the browser is behind a card" is not a value any
// buffer comparison would report.

TEST_CASE("the surface host stays in front of every card, however many arrive") {
	Fixture f;
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.resized();

	const auto indexOf = [&](const visage::Frame* frame) {
		return f.editor.indexOfChild(frame);
	};

	for (size_t i = 0; i < kAxes.deviceSlots; ++i) {
		REQUIRE(f.add(fx::filter::kTraits.tag) != kNoInstance);
		f.editor.syncCards();

		// AFTER EVERY SINGLE ADD, not just at the end: the defect appears the moment ONE card is added
		// after the layers, and a case that only checked the final state would pass on a fix that
		// re-ordered once at the end while every intermediate state was wrong.
		const int surfaces = indexOf(&f.editor.surfaceLayersForTest());
		CAPTURE(i);
		CAPTURE(surfaces);
		// THE NOTIFIER IS RAISED TOO AND IS ASSERTED TOO. It says "The rack is full" — a refusal drawn
		// behind the cards it is refusing to join is the honesty rule failing quietly, and half a fix
		// with no case on the other half is how the half without one rots.
		const int notifier = indexOf(&f.editor.notifyTrayForTest());
		for (ui::FxCard* card : f.editor.cardsForTest()) {
			if (card != nullptr) {
				CHECK(indexOf(card) < notifier);
				CHECK(indexOf(card) < surfaces);
			}
		}
		// AND THE SURFACE HOST IS ABOVE THE NOTIFIER: a takeover covers the whole editor, so a message
		// drawn over a browser would be about a rack the player cannot see.
		CHECK(notifier < surfaces);
	}
}

// ---------------------------------------------------------------------------------------------
// The rack's cards were not rendering the custom UIs
// the devices offer. The rack built `GenericCard` for every device, because every custom card took a
// `ParamBridge` and the rack has a `DeviceHost` and no bridge.

TEST_CASE("a device with a custom card gets it, and one without gets the generic grid") {
	// ASSERTED AGAINST THE FACTORY RATHER THAN A LIST OF TAGS, so this case stays true as cards
	// converge instead of needing an edit each time — and so it cannot pass by agreeing with itself.
	size_t custom = 0;
	size_t generic = 0;
	size_t seatedFY = 0; // backlog FY: an add that fails is a device this sweep never met, so it is counted
	for (const device::Descriptor& d : kDevices) {
		Fixture f;
		if (f.add(d.traits.tag) == kNoInstance) {
			continue;
		}
		++seatedFY;
		f.editor.syncCards();
		ui::FxCard* card = nullptr;
		for (ui::FxCard* c : f.editor.cardsForTest()) {
			if (c != nullptr) {
				card = c;
				break;
			}
		}
		CAPTURE(d.traits.tag);
		REQUIRE(card != nullptr);

		// **THE EXACT TYPE, NOT "IS A".** A `dynamic_cast<GenericCard*>` answers yes for a SUBCLASS,
		// and since 2026-09-15 there is one: `SklitterCard` derives from the generic card to reuse its
		// one-column, scrolling, mode-aware list and adds a band above it. That is a device getting
		// its OWN card by every meaning this case is about, and the cast called it generic.
		//
		// `typeid` rather than naming the subclass, because a list of exceptions is a list that goes
		// stale — the question is "is this card the generic grid ITSELF", and that is what exact type
		// equality asks.
		const bool isGeneric = typeid(*card) == typeid(ui::GenericCard);
		if (ui::hasCustomCard(d)) {
			// THE DEVICE'S OWN UI, which is the whole point: a knob grid where a plot belongs is the
			// report this case exists for.
			CHECK_FALSE(isGeneric);
			++custom;
		}
		else {
			CHECK(isGeneric);
			++generic;
		}
	}
	CHECK(seatedFY == feathers::kDeviceCount); // backlog FY: EVERY registered device was seated, not "the loop ran"

	// AND THE CUSTOM ARM RAN. Without this the case passes when the factory returns null for
	// everything — exactly the state it was written to end, so it is the one failure it must not hide.
	CHECK(custom > 0);

	// **THE GENERIC ARM HAS A SUBJECT AGAIN, AND IT IS THE TRACKER.** This has now been all three
	// numbers, and the history is the argument for asserting the current one rather than a bound:
	// `> 0` while devices were still un-converged, then `== 0` when the filter's conversion left every
	// registered device with a custom card, and `> 0` again from 2026-09-02 — the tracker is on the
	// generic card for now, with its own card to follow. The tracker is a registered device with no
	// bespoke RACK card, so the rack gives it the generic grid, which is the whole of what that
	// decision asked for.
	//
	// NOT LOOSENED TO A RANGE THAT WOULD HOLD EITHER WAY. A count that is allowed to be anything
	// stops reporting when the fallback silently stops being built, and that is the failure this arm
	// was written to catch. If a bespoke Tracker card is written later this goes back to zero, and
	// this line failing is the correct way to be told — at which point the stranger-descriptor check
	// below becomes the only road to the fallback again, as it was.
	CHECK(generic > 0);
	// A COPY OF A REAL DESCRIPTOR WITH A TAG NOBODY CLAIMS, rather than a hand-built one: the table is
	// keyed on the tag alone, and a descriptor assembled field by field here would go stale the next
	// time `Traits` gains a member (it just did — the compiler caught it).
	device::Descriptor stranger = fx::filter::kDescriptor;
	stranger.traits.tag = "zzzz";
	CHECK_FALSE(ui::hasCustomCard(stranger));
}

// ---------------------------------------------------------------------------------------------
// Popup views were unavailable. Two separate causes, and both are fixed here.

TEST_CASE("a device whose controls all fit still offers its panel when it declares one") {
	// THE BUG: `expandBounds` was gated on `hiddenCount() > 0` alone, so a card showing every control
	// drew no button — and the device's own panel, which holds controls that live NOWHERE else, was
	// unreachable. The gate is now overflow OR a declared expanded view.
	size_t declaring = 0;
	size_t fitting = 0;
	for (const device::Descriptor& d : kDevices) {
		if (!ui::hasExpandedView(d)) {
			continue;
		}
		Fixture f;
		if (f.add(d.traits.tag) == kNoInstance) {
			continue;
		}
		f.editor.syncCards();
		f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
		f.editor.resized();

		auto* card = dynamic_cast<ui::GenericCard*>(f.editor.cardsForTest()[0]);
		if (card == nullptr) {
			continue; // it has a custom card; its own head owns the verb
		}
		CAPTURE(d.traits.tag);
		CAPTURE(card->hiddenCount());
		// THE VERB IS OFFERED WHETHER OR NOT ANYTHING IS TRUNCATED.
		CHECK(card->expandBounds().width() > 0.0f);
		++declaring;
		if (card->hiddenCount() == 0) {
			++fitting;
		}
	}
	// **THE POPULATION IS EMPTY NOW, AND THAT IS RECORDED RATHER THAN ASSERTED AWAY.**
	//
	// This required `declaring > 0` so it could not pass by finding nothing to check — the right guard,
	// and it went red the moment `WaypointCard` entered the shared card table. Waypoint was the ONLY
	// device pairing a GENERIC card with a declared expanded view; with its own card it leaves this
	// sweep with nothing to sweep.
	//
	// KEPT, WITH THE CONDITION STATED, because the sweep becomes live again the day a device declares
	// an expanded view without a custom card — which is the next generic device anyone adds. Deleting
	// it would silently give that device up; weakening the guard to `>= 0` would keep a case that can
	// never fail. Saying "there are none, and here is why" is the honest third option.
	if (declaring == 0) {
		MESSAGE("no device currently pairs a generic card with a declared expanded view — Waypoint was "
		        "the last, and it has its own card now");
	}
	CHECK(fitting <= declaring);
	// **AND NO DEVICE IN THIS CATALOGUE CAN TELL THE TWO GATE RULES APART TODAY — MEASURED, AND SAID
	// HERE RATHER THAN ASSERTED AWAY.**
	//
	// The gate was `hiddenCount() > 0`; it is now that OR the device declaring an expanded view. Only
	// a device that declares one AND fits entirely distinguishes them, and there is none: the two
	// declaring devices that still get the generic grid both truncate — OWLTT hides 16, Waypoint
	// hides 9. So a known-bad restoring the old gate leaves this case GREEN, and that is a fact about
	// the catalogue rather than a weakness anyone can fix by writing a better assertion.
	//
	// THE FIX IS STILL RIGHT AND IS STILL NOT THE SYMPTOM SEEN. "popup views not available" was the
	// PANEL, not the button: OWLTT and Waypoint always had a button, and pressing it opened a generic
	// knob grid because the rack could not build their own. That is the case below, and it is the one
	// with a known-bad that bites.
	//
	// WHEN THIS BECOMES OBSERVABLE: the day a device with fewer than the cap's controls declares a
	// panel. Then `fitting` below is non-zero and this line should become an assertion.
	INFO("declaring devices on the generic grid that fit entirely: " << fitting);
}

TEST_CASE("expanding a converged device opens ITS panel, not the generic grid") {
	// THE OTHER HALF: the rack presented a vertical `GenericCard` for everything, because every custom
	// panel took a `ParamBridge` the rack has not got. `makeExpandedView` now takes the bridge as a
	// POINTER, and the rack passes none — so a converged panel is built and the rest decline.
	Fixture f;
	const device::Descriptor* converged = nullptr;
	for (const device::Descriptor& d : kDevices) {
		if (ui::hasCustomCard(d) && ui::hasExpandedView(d)) {
			converged = &d;
			break;
		}
	}
	REQUIRE(converged != nullptr);
	const DeviceInstanceId added = f.add(converged->traits.tag);
	REQUIRE(added != kNoInstance);
	f.editor.syncCards();
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.resized();

	ui::FxCard* card = f.editor.cardsForTest()[0];
	REQUIRE(card != nullptr);
	REQUIRE(card->onExpand);
	REQUIRE(f.editor.canvas().setCollapsed(false)); // opened: a fresh canvas is folded
	card->onExpand();

	REQUIRE(f.editor.overlayForTest().open());
	// IT IS THE DEVICE'S PANEL, NOT A KNOB GRID. Checking the type is the point: a canvas that
	// opened SOMETHING would pass a weaker assertion while showing exactly the wrong thing.
	const auto* mounted = dynamic_cast<const ui::MountedFrame*>(f.editor.overlayForTest().contentForTest());
	REQUIRE(mounted != nullptr);
	CHECK(dynamic_cast<ui::GenericCard*>(mounted->contentForTest()) == nullptr);
	CHECK(dynamic_cast<ui::ExpandedView*>(mounted->contentForTest()) != nullptr);
	// AND IT NAMES ITS INSTANCE — the key the canvas places and tints by (the defect: a second
	// instance of one kind replaced the first, untinted, because no panel said which device it was).
	CHECK(mounted->watchedDevice() == added);
}

// ---------------------------------------------------------------------------------------------
// Clicking the rack's arrow crashed.
//
// `Knob::setModulation` puts every knob on `ModState`'s watcher list, which holds RAW FRAME POINTERS.
// Destroying a card left them dangling, and `arm()` calls `watchers_.notify()`, which walks them. THE
// ARROW WAS NOT THE BUG — it was the first thing that read the list after a card had died.

TEST_CASE("a card that goes takes its watchers with it, so arming afterwards is safe") {
	Fixture f;
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);

	const DeviceInstanceId slot = f.add(fx::filter::kTraits.tag);
	REQUIRE(slot != kNoInstance);
	f.editor.syncCards();
	f.editor.resized();

	// THE CARD'S KNOBS ARE ON THE LIST — asserted, because if they were not this case would prove
	// nothing about a list it never populated.
	const size_t watchedWithCard = f.mod.watchers().framesForTest().size();
	CHECK(watchedWithCard > 0);

	f.remove(slot);
	f.editor.syncCards();

	// AND THEY LEFT WITH IT. Every frame still listed must be one that is still alive; the old card's
	// were freed by `reset()` above.
	CHECK(f.mod.watchers().framesForTest().size() < watchedWithCard);

	// THE PRESS THAT CRASHED. Under ASan this is the assertion that matters — a dangling pointer in
	// the list is a use-after-free here and nowhere else.
	f.mod.arm(true);
	f.mod.focus(backplane::Source::core(1));
	f.mod.arm(false);
}

TEST_CASE("replacing a device's card does not leave the old one being notified") {
	// THE SHAPE A HAND ACTUALLY MAKES: add, remove, add something else, then arm.
	Fixture f;
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);

	const size_t baseline = f.mod.watchers().framesForTest().size();
	REQUIRE(baseline > 0); // the editor's own knobs are on the list, so this is not a vacuous zero

	for (int round = 0; round < 4; ++round) {
		const DeviceInstanceId slot = f.add(fx::filter::kTraits.tag);
		REQUIRE(slot != kNoInstance);
		f.editor.syncCards();
		f.editor.resized();
		f.mod.arm(true);
		f.remove(slot);
		f.editor.syncCards();
		f.mod.arm(false);
	}

	// NOTHING STALE SURVIVED THE ROUNDS. With the forget missing this grew every round, and each
	// stale entry was a freed frame the next arm would walk.
	// **STABILITY IS THE INVARIANT, NOT A NUMBER.** The list holds the editor's permanent watchers —
	// eight macro knobs and eight arms — and a card's knobs while a card exists. Asserting the total
	// would mean encoding who registers, which is a fact about other files that will change; asserting
	// it does not GROW is the property the crash violated.
	//
	// MEASURED: with the forget missing this rose every round, and each stale entry was a freed frame
	// the next arm would walk.
	CHECK(f.mod.watchers().framesForTest().size() == baseline);
}

// ---------------------------------------------------------------------------------------------
// Clicking a knob in assign mode looked like it assigned at 100%, but dragging did not adjust
// the depth (dragging on the card did).
//
// `RackGui::tick` calls `syncCards` every beat. The z-order raise at the end of it used to
// `removeChild`/`addChild` unconditionally, and visage's window event handler CLEARS `mouse_down_frame_`
// for a frame that leaves the tree — so every beat threw away the frame a drag was being delivered to.
//
// THE ASYMMETRY IS THE PROOF AND THE REASON THIS IS ASSERTED AS A COUNT: a card is a direct child of
// the editor and was never re-added, so dragging worked there; a popup's knobs live under `surfaces_`,
// which was removed sixty times a second.

TEST_CASE("a beat with nothing new does not tear the top layers out of the tree") {
	Fixture f;
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.syncCards();
	f.editor.resized();

	const size_t afterFirst = f.editor.raisesForTest();

	// TEN QUIET BEATS. `syncCards` compares before building, so none of these changes anything — and
	// none of them may touch the hierarchy either.
	for (int beat = 0; beat < 10; ++beat) {
		f.editor.syncCards();
	}
	CHECK(f.editor.raisesForTest() == afterFirst);

	// AND THE ORDER IS STILL RIGHT, so "did not raise" is not "gave up": the surface host is last and
	// the notifier is directly beneath it.
	const int surfaces = f.editor.indexOfChild(&f.editor.surfaceLayersForTest());
	const int notifier = f.editor.indexOfChild(&f.editor.notifyTrayForTest());
	CHECK(surfaces == static_cast<int>(f.editor.children().size()) - 1);
	CHECK(notifier == surfaces - 1);
}

TEST_CASE("a card arriving DOES raise, so the idempotence is not simply never raising") {
	Fixture f;
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.syncCards();
	const size_t before = f.editor.raisesForTest();

	REQUIRE(f.add(fx::filter::kTraits.tag) != kNoInstance);
	f.editor.syncCards();

	// THE SECOND ASSERTION THE FIRST CASE NEEDS. Without this, "never raises" would satisfy it — and a
	// raise that never runs is the z-order bug coming back rather than the drag bug staying fixed.
	CHECK(f.editor.raisesForTest() > before);
	const int surfaces = f.editor.indexOfChild(&f.editor.surfaceLayersForTest());
	for (ui::FxCard* card : f.editor.cardsForTest()) {
		if (card != nullptr) {
			CHECK(f.editor.indexOfChild(card) < surfaces);
		}
	}
}

// ---------------------------------------------------------------------------------------------
// WHOEVER REGISTERS, UNREGISTERS — the rule that replaces "every owner must remember".
//
// `ModState`'s watcher list holds RAW FRAME POINTERS and `notify()` walks them. Until now a frame put
// ITSELF on the list and was taken off only if a distant owner called `forgetFrameTree` — five owners
// did and the rack did not, which showed as a crash on clicking an arm arrow. A widget now takes
// itself off in its own destructor, which makes the omission unrepresentable rather than guarded.

TEST_CASE("a knob that dies is off the watcher list, with no owner involved") {
	ui::ModState mod;
	ui::EditHistory history;
	rack::RackEngine engine { kAxes, hostCatalogue() };
	// OVER THE BRIDGE, THE RACK'S ONE HOST — the engine is no `DeviceHost` since kill-lanes step 2.
	rack::RackBridge bridge { engine };
	const DeviceInstanceId device = bridge.addDevice(fx::filter::kTraits.tag);
	REQUIRE(device != kNoInstance);

	const size_t before = mod.watchers().framesForTest().size();
	{
		ui::GenericCard card { bridge, device, fx::filter::kDescriptor, nullptr };
		card.setModulation(&mod, &history);
		// THE CARD'S KNOBS ARE ON THE LIST — asserted, so the check below is not about an empty one.
		CHECK(mod.watchers().framesForTest().size() > before);
	}
	// **NOBODY CALLED `forgetFrameTree`.** That is the whole point: the card went out of scope and its
	// knobs took themselves off. With the destructors missing this stays high and every entry is a
	// freed frame the next `arm()` would walk.
	CHECK(mod.watchers().framesForTest().size() == before);

	// AND NOTIFYING IS SAFE, which is the fault the rule exists to prevent rather than a proxy for it.
	mod.arm(true);
	mod.arm(false);
}

TEST_CASE("re-pointing a knob at another state leaves it on neither list twice") {
	// A knob handed a second state used to stay on the FIRST one's list for ever — registered with
	// something nobody would ever unregister it from. The same dangling pointer by a different door.
	ui::ModState first;
	ui::ModState second;
    ui::EditHistory history;
	rack::RackEngine engine { kAxes, hostCatalogue() };
	rack::RackBridge bridge { engine };
	const DeviceInstanceId device = bridge.addDevice(fx::filter::kTraits.tag);
	REQUIRE(device != kNoInstance);

	{
		ui::GenericCard card { bridge, device, fx::filter::kDescriptor, nullptr };
		card.setModulation(&first, &history);
		const size_t onFirst = first.watchers().framesForTest().size();
		REQUIRE(onFirst > 0);

		card.setModulation(&second, &history);
		CHECK(first.watchers().framesForTest().empty());
		CHECK(second.watchers().framesForTest().size() == onFirst);
	}
	CHECK(second.watchers().framesForTest().empty());
}

TEST_CASE("an open panel is on the tick road, and comes off it when dismissed") {
	// THE DEFECT THIS PINS IS AN ABSENCE. The rack never advanced anything: `RackGui::tick()` called
	// `syncCards` and `syncMacros` and nothing else, while Feathers drives four update roads out of
	// `Gui::Impl::tick()`. Both halves of what that costs were visible — a OWLTT analyser that moved only
	// while the mouse was over it (visage repainting on input, no clock behind it) and Automod curves
	// that never moved at all, because that card advances its strip INSIDE `update()`, so a repaint
	// alone redraws an unadvanced state.
	//
	// AND AN ABSENT ROAD IS INVISIBLE IN A RENDERED FRAME: a panel that never advances draws a
	// completely plausible still picture. So this reads the WIRING rather than comparing pixels.
	Fixture f;
	const device::Descriptor* animated = nullptr;
	for (const device::Descriptor& d : kDevices) {
		if (ui::hasExpandedView(d)) {
			animated = &d;
			break;
		}
	}
	REQUIRE(animated != nullptr);
	REQUIRE(f.add(animated->traits.tag) != kNoInstance);
	f.editor.syncCards();
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.resized();

	// NOTHING OPEN, NOTHING TO ADVANCE — and this half matters as much as the next one, because a
	// thunk left standing after dismissal points into content the overlay has retired.
	CHECK_FALSE(f.editor.panelTickWiredForTest());

	ui::FxCard* card = f.editor.cardsForTest()[0];
	REQUIRE(card != nullptr);
	REQUIRE(card->onExpand);
	REQUIRE(f.editor.canvas().setCollapsed(false)); // opened: a fresh canvas is folded
	card->onExpand();
	REQUIRE(f.editor.overlayForTest().open());
	CHECK(f.editor.panelTickWiredForTest());

	// AND `updateCards` REACHES IT. The count is produced BY the work — a card or panel that answers
	// true has asked for a frame — so calling it here also proves the walk does not trip over the
	// thunk it was just handed.
	const size_t told = f.editor.updateCards();
	CHECK(told <= f.editor.cardsForTest().size() + 1);

	f.editor.overlayForTest().dismiss();
	// CLEARED BY THE DISMISSAL ROAD, which is the same road that clears `expandedId_`. If this ever
	// goes true, the tick is calling into a frame the canvas has already let go of.
	CHECK_FALSE(f.editor.panelTickWiredForTest());
	CHECK(f.editor.updateCards() <= f.editor.cardsForTest().size());
}


// ---------------------------------------------------------------------------------------------
// A DEVICE'S OWN OUTPUT, AS SOMETHING A HAND CAN POINT.
//
// THE SYMPTOM: the modulator devices did not render, and their mod outputs could not be reached.
//
// **THE CAPABILITY EXISTED AT EVERY LAYER EXCEPT THE ONE A HAND TOUCHES.** `RackGui` sizes `ModState`
// to `axes().sources()`, which counts device outputs; `deviceSourceOf` names the index; `RouteTable`
// reads it; the engine ticks the modulator and `outputAt` returns its value. The rack built arms for
// the eight macros and for nothing else, so a modulator was a device you could add, tick, and not
// use. The class doing the arming was called `MacroArm` and took a bare source index — nothing in it
// was ever macro-specific, which is how a general mechanism gets used for one case and forgotten for
// the rest.
// ---------------------------------------------------------------------------------------------

TEST_CASE("a device that publishes an output gets an arm pointing at THAT output's source") {
	Fixture f;
	const device::Descriptor* modulator = nullptr;
	for (const device::Descriptor& d : kDevices) {
		if (d.outputCount() > 0) {
			modulator = &d;
			break;
		}
	}
	REQUIRE(modulator != nullptr);
	const DeviceInstanceId slot = f.add(modulator->traits.tag);
	REQUIRE(slot != kNoInstance);
	f.editor.syncCards();
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.resized();

	REQUIRE(f.editor.outputArmCountForTest() == modulator->outputCount());
	CHECK(f.editor.outputArmDeviceForTest(0) == slot);
	CHECK(f.editor.outputArmOutputForTest(0) == 0);

	// **THE ENDPOINT IS THE CLAIM.** An arm wired to the wrong source arms somebody else's output and
	// looks identical on screen, so pressing it and reading back WHICH source got focus is the only
	// assertion that distinguishes a working arm from a decorative one.
	visage::Frame* arm = f.editor.outputArmFrameForTest(0);
	REQUIRE(arm != nullptr);
	arm->onMouseDown().callback(ui::MouseEventForTest());
	CHECK(f.mod.armed());
	CHECK(f.mod.source() == backplane::Source::output(slot, 0));
	// AND IT IS NOT A MACRO'S SOURCE, which is the confusion the old build could not even express.
	CHECK(f.mod.source().isDevice());

	// STICKY AND EXCLUSIVE, the same rule the macro arms follow: pressing the armed arm disarms.
	arm->onMouseDown().callback(ui::MouseEventForTest());
	CHECK_FALSE(f.mod.armed());
}

TEST_CASE("a device with no outputs gets no arm, so nothing offers to route what does not exist") {
	Fixture f;
	const device::Descriptor* silent = nullptr;
	for (const device::Descriptor& d : kDevices) {
		if (d.outputCount() == 0 && d.audio != nullptr) {
			silent = &d;
			break;
		}
	}
	REQUIRE(silent != nullptr);
	REQUIRE(f.add(silent->traits.tag) != kNoInstance);
	f.editor.syncCards();
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.resized();

	CHECK(f.editor.outputArmCountForTest() == 0);
}

TEST_CASE("backlog FL: EVERY output of EVERY device the registry offers has an arm, and it is inside "
          "its own card — in the rack's lane, at the shortest lane the editor can be given") {
	// **THE RULE, ASKED OF THE WHOLE REGISTRY RATHER THAN OF TODAY'S DEVICES.** A design rule for
	// every modulator: all of its outputs are on the card.
	//
	// **THE CASES ABOVE COULD NOT SEE THIS AND SAY SO.** They take the FIRST device that publishes —
	// which is a one-output modulator — and count arms. A count is satisfied by seven arms drawn on
	// top of each other, or off the left edge of the card, or over the next card in the chain; this
	// lane did the second, because it laid one non-wrapping row: `armX -= armW + kPad` per arm, from
	// the card's right edge, with nothing that could ever start a second row.
	//
	// **AND IT ASKED FOR THE WRONG FRAME AS WELL.** `cardFrameOf` was called with neither the head
	// edge nor the row count, so a card whose head is its LEFT spine got a frame computed for a TOP
	// head — the strip laid across the spine.
	//
	// **AT TWO LANE HEIGHTS, THE SECOND SHORT.** A rack card's WIDTH is `laneExtent(cardHeight)`, so
	// the lane's height is the only lever this host has on how much room the chips have: a shorter
	// lane makes a narrower card, which is where a wrap that is nearly good enough stops being good
	// enough. The narrowest card this editor can produce is the one at the shortest usable lane.
	const int kShortEditor = 260;
	for (const int editorHeight : { kEditorHeight, kShortEditor }) {
		CAPTURE(editorHeight);
		size_t devicesChecked = 0;
		size_t outputsChecked = 0;
		size_t multiOutputDevices = 0;
		for (const device::Descriptor& d : kDevices) {
			if (d.outputCount() == 0) {
				continue; // an effect publishes nothing and pays nothing — the other half of the rule
			}
			CAPTURE(std::string(d.traits.tag)); // NUL-terminated, any length: a fixed 4 overruns "eq"
			// A FIXTURE PER DEVICE: a lane's layout is a function of what is in it, and a card built
			// into a crowded lane may be scrolled out of view and never laid out.
			Fixture f;
			const DeviceInstanceId device = f.add(d.traits.tag);
			REQUIRE(device != kNoInstance);
			f.editor.syncCards();
			f.editor.setBounds(0, 0, rack::RackEditor::kWidth, editorHeight);
			f.editor.resized();

			ui::FxCard* card = f.editor.cardOfDeviceForTest(device);
			REQUIRE(card != nullptr);
			const visage::Bounds box = card->bounds();
			REQUIRE(box.width() > 0.0f);
			REQUIRE_FALSE(card->folded()); // else the arms are hidden BY THE RULE and prove nothing

			float previousRight = -1.0f;
			float previousTop = -1.0f;
			for (size_t output = 0; output < d.outputCount(); ++output) {
				CAPTURE(output);
				CAPTURE(ui::nameOrUnnamed(d.outputs[output].name));
				// **THE ARM EXISTS AND IS SHOWN** — an arm placed and hidden is an output a hand cannot
				// reach, which is the same failure as one drawn off the card.
				if (((f.bridge.outputsShown(device) >> output) & 1u) == 0u) {
					CHECK_FALSE(f.editor.outputArmVisibleForTest(device, output)); // NOT ON THE CARD (`DeviceHost::outputsShown`)
					continue;
				}
				CHECK(f.editor.outputArmVisibleForTest(device, output));
				const visage::Bounds chip = f.editor.outputArmBoundsForTest(device, output);
				REQUIRE(chip.width() > 0.0f);
				// **INSIDE ITS CARD, ALL FOUR EDGES.** `visage::Bounds` has `contains(Point)` and not
				// `contains(Bounds)`, so the four inequalities are written out; half a pixel of slack
				// because the strip's rows are laid from a float top.
				CHECK(chip.x() >= box.x() - 0.5f);
				CHECK(chip.right() <= box.right() + 0.5f);
				CHECK(chip.y() >= box.y() - 0.5f);
				CHECK(chip.bottom() <= box.bottom() + 0.5f);
				// **AND NOT ON TOP OF THE CHIP BEFORE IT: either further RIGHT on the same row, or on a
				// row below it.** The walk lays them left to right in the order the device declares
				// them, which is what "further right" is the order OF. It laid them the other way until
				// 2026-09-20 — the mod outputs on the card were badly laid out — and this
				// assertion was the mirror of itself; the claim is unchanged.
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
		// **NOT VACUOUS, AND NOT VACUOUS IN THE WAY THAT MATTERS.** A registry of one-output devices
		// would satisfy every assertion above without ever exercising the wrap, so the case states
		// that it really did see a device with several outputs — which is the whole subject.
		CHECK(devicesChecked >= 3);
		CHECK(outputsChecked >= 7);
		CHECK(multiOutputDevices >= 1);
	}
}

TEST_CASE("an output that cannot fit inside its card is HIDDEN, not drawn outside it — and the "
          "count that fits rises as the editor grows") {
	// **THE DESIGN RULE.** All of a modulator's outputs are on the card, which does not say what
	// "on the card" means when there is no room. This does: a chip
	// that cannot be drawn inside its card is not drawn. **A hidden chip is a card that visibly cannot
	// show everything and a player who can resize; a chip over the neighbour is a lie about which
	// device publishes it.**
	//
	// **THIS LANE IS THE ONE THAT CAN REACH IT.** Its card height is `min(kCardUnitHeight, laneHeight)`
	// rather than what the content wants, and the editor is freely resizable — `guiCanResize` returns
	// true with no constraint of our own. `laneHeight` is the editor's height less the chrome, so the
	// strip is clamped below an editor height of about 218; the registry-wide case above runs at 260,
	// which is 42px ABOVE this path and could never see it.
	//
	// **WHAT WENT WRONG HERE BEFORE THE RULE, twice, and neither was visible in a green suite:** a
	// row past the clamped strip was pulled back onto the last one that fits, so two chips shared a
	// line — behaviour the no-overlap assertion above FORBIDS, so it could not occur in a green run and
	// was never validated; and at a strip of zero height or width every chip was laid at a negative x
	// AND SHOWN, which is the mainline defect this arc photographed, returning through another door.
	const device::Descriptor* publisher = nullptr;
	for (const device::Descriptor& d : kDevices) {
		if (d.outputCount() > 2) {
			publisher = &d; // the wrap only matters for a device with more chips than fit on a line
			break;
		}
	}
	REQUIRE(publisher != nullptr);
	CAPTURE(std::string(publisher->traits.tag, 4));
	const size_t declared = publisher->outputCount();

	// **A SWEEP RATHER THAN TWO CHOSEN HEIGHTS**, because what fits is a function of the card's width
	// as well as its height and the card's width is `laneExtent(cardHeight)` — so the heights at which
	// chips start to disappear are discovered here, not asserted from arithmetic done in a comment.
	std::vector<size_t> shownAt;
	// (THE SWEEP IS THE OLD ONE SHIFTED DOWN 94 px, the route-list band that used to sit above the lane: lane height is the same.)
	for (const int editorHeight : { 6, 36, 58, 80, 102, 124, 166, kEditorHeight }) {
		CAPTURE(editorHeight);
		Fixture f;
		const DeviceInstanceId device = f.add(publisher->traits.tag);
		REQUIRE(device != kNoInstance);
		f.editor.syncCards();
		f.editor.setBounds(0, 0, rack::RackEditor::kWidth, editorHeight);
		f.editor.resized();

		ui::FxCard* card = f.editor.cardOfDeviceForTest(device);
		REQUIRE(card != nullptr);
		const visage::Bounds box = card->bounds();

		size_t shown = 0;
		for (size_t output = 0; output < declared; ++output) {
			CAPTURE(output);
			CAPTURE(ui::nameOrUnnamed(publisher->outputs[output].name));
			if (!f.editor.outputArmVisibleForTest(device, output)) {
				continue; // hidden: the rule's answer, and the loop below says it is not ALL of them
			}
			++shown;
			// **EVERY SHOWN CHIP IS INSIDE ITS CARD, ALL FOUR EDGES.** This is the claim the rule
			// exists to keep true at every height rather than at the comfortable ones.
			const visage::Bounds chip = f.editor.outputArmBoundsForTest(device, output);
			REQUIRE(chip.width() > 0.0f);
			CHECK(chip.x() >= box.x() - 0.5f);
			CHECK(chip.right() <= box.right() + 0.5f);
			CHECK(chip.y() >= box.y() - 0.5f);
			CHECK(chip.bottom() <= box.bottom() + 0.5f);
		}
		shownAt.push_back(shown);
	}

	// **MORE ROOM NEVER SHOWS FEWER OUTPUTS.** Without this "hidden" could silently become "never
	// drawn" — a build that hid everything at every size would satisfy every assertion above, since
	// they are all about the chips that ARE shown.
	for (size_t i = 1; i < shownAt.size(); ++i) {
		CAPTURE(i);
		CAPTURE(shownAt[i - 1]);
		CAPTURE(shownAt[i]);
		CHECK(shownAt[i] >= shownAt[i - 1]);
	}
	// AND THE TWO ENDS ARE REAL: the smallest editor shows NONE of them (there is no strip at all),
	// and the largest shows EVERY one. A sweep whose ends were the same number would be describing a
	// build where the height changes nothing.
	CHECK(shownAt.front() == 0);
	CHECK(shownAt.back() == declared);
	// AND SOMEWHERE IN BETWEEN IT IS NEITHER — the PARTIAL state is the one the rule is about, and a
	// build that went straight from none to all would never exercise it.
	const bool partial = std::any_of(shownAt.begin(), shownAt.end(), [declared](size_t n) {
		return n > 0 && n < declared;
	});
	CHECK(partial);
}

TEST_CASE("removing a device takes its arm with it, rather than leaving one aimed at a dead slot") {
	// AN ARM HOLDS A SOURCE INDEX COMPUTED FROM ITS SLOT, so one that outlived a chain change would
	// arm the wrong source SILENTLY — the arm still draws, still lights, and points somewhere else.
	// That is why the arms are rebuilt wholesale rather than patched.
	Fixture f;
	const device::Descriptor* modulator = nullptr;
	for (const device::Descriptor& d : kDevices) {
		if (d.outputCount() > 0) {
			modulator = &d;
			break;
		}
	}
	REQUIRE(modulator != nullptr);
	const DeviceInstanceId slot = f.add(modulator->traits.tag);
	REQUIRE(slot != kNoInstance);
	f.editor.syncCards();
	REQUIRE(f.editor.outputArmCountForTest() == modulator->outputCount());

	f.remove(slot);
	f.editor.syncCards();
	CHECK(f.editor.outputArmCountForTest() == 0);
}

TEST_CASE("a modulator in the rack gets ITS OWN card, not the generic knob grid") {
	// THE SYMPTOM: the modulator devices did not render. `EnvCard` and `MsegCard` took a `ParamBridge&`
	// — Feathers' own road — so the rack could not build them whatever the card table said, and every
	// modulator fell through to `GenericCard`. That is the same gap as the earlier one where cards did
	// not render their native UI, which was fixed for the six FX cards and never for this tier.
	//
	// CHECKING THE TYPE IS THE POINT. A card that rendered SOMETHING would pass a weaker assertion
	// while showing exactly the wrong thing — a grid of knobs where an envelope belongs.
	Fixture f;
	const device::Descriptor* modulator = nullptr;
	for (const device::Descriptor& d : kDevices) {
		// **A MODULATOR, NOT MERELY A DEVICE WITH MOD OPS**, and the discriminator below is why it
		// matters: SKLITTER declares `mod` now (it is an fx that also publishes) and is registered
		// first, and its bespoke card DERIVES from `GenericCard` — so `dynamic_cast<GenericCard*>`
		// finds one and the case reads a correct card as the wrong card. The claim here is about the
		// modulator TIER, whose cards replace the grid rather than extending it.
		if (d.mod != nullptr && d.audio == nullptr && ui::hasCustomCard(d)) {
			modulator = &d;
			break;
		}
	}
	REQUIRE(modulator != nullptr);
	INFO(modulator->traits.tag);
	REQUIRE(f.add(modulator->traits.tag) != kNoInstance);
	f.editor.syncCards();
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.resized();

	ui::FxCard* card = f.editor.cardsForTest()[0];
	REQUIRE(card != nullptr);
	CHECK(dynamic_cast<ui::GenericCard*>(card) == nullptr);
}

TEST_CASE("the rack draws no playhead marker, because nothing there publishes one") {
	// **A PLAYHEAD IS A READING, NOT A SETTING**, so it did NOT follow the settings port: the rack has
	// no publisher for it. The honest outcome is a marker that is ABSENT, not one frozen at stage
	// zero — a stuck marker is a plausible picture of a stopped envelope, which is the worst kind of
	// wrong. `DeviceHost::playheadAt` declines by default and the card answers idle.
	Fixture f;
	uint32_t ordinal = 7;
	float phase = 0.5f;
	CHECK_FALSE(f.engine.playheadAt(0, ordinal, phase));
}



TEST_CASE("a folded card collapses to its spine, in every card the rack can hold") {
	// **THE SYMPTOM: the MSEG card did not collapse. AND IT DID NOT — `MsegCard::laneExtent` answered
	// `kModCardWidth` unconditionally, so folding flipped the state, redrew the head, and moved
	// nothing a lane could see.** The state was right and the geometry never followed it.
	//
	// SWEPT ACROSS EVERY CARD RATHER THAN DRIVEN ON ONE, which is the instrument that found the OWLTT
	// fold that never folded and the HARM row that stayed visible: ask every card the same question
	// and the one that does nothing names itself. A case driving MSEG alone would have missed those,
	// and a case driving DAHDSR alone would have missed MSEG.
	for (const device::Descriptor& d : kDevices) {
		Fixture f;
		const DeviceInstanceId slot = f.add(d.traits.tag);
		if (slot == kNoInstance) {
			continue;
		}
		f.editor.syncCards();
		f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
		f.editor.resized();
		ui::FxCard* card = f.editor.cardOfDeviceForTest(slot);
		REQUIRE(card != nullptr);
		CAPTURE(d.traits.tag);

		card->setLaneAxis(ui::FxCard::LaneAxis::Horizontal);
		const float open = card->laneExtent(ui::kCardUnitHeight);
		REQUIRE(open > ui::kCardHeadHeight); // else the card is already a spine and proves nothing

		card->setFolded(true);
		CHECK(card->folded());
		// **THE EXTENT IS THE CLAIM, NOT THE FLAG.** A card that reports the same width folded has not
		// collapsed however true its own state is — that is exactly the MSEG bug, and asserting
		// `folded()` alone would have passed straight through it.
		CHECK(card->laneExtent(ui::kCardUnitHeight) == doctest::Approx(ui::kCardHeadHeight));

		card->setFolded(false);
		CHECK(card->laneExtent(ui::kCardUnitHeight) == doctest::Approx(open));
	}
}

TEST_CASE("on a spine no card draws a child underneath its own header") {
	// The value boxes in Automod drew over the vertical header.
	//
	// **THE HEAD OWNS THE FIRST 24 PIXELS AND A BODY COORDINATE STARTS AFTER THEM.** `bodyLeft()` is
	// that number, and every card's layout is supposed to be spelled from it. Automod's was — in one
	// of its two placement loops. The other still read `kPad`, which is the card's origin, so the
	// coordinate row drew across the spine while the row directly above it did not.
	//
	// A CASE PER CARD WOULD HAVE MISSED IT AND DID: the sibling loop above the broken one was already
	// correct, so any assertion aimed at "automod lays out inside its body" had a fifty-fifty chance of
	// picking the right row. This asks the question of EVERY child of EVERY card instead, which is the
	// only form that does not depend on guessing which loop is wrong.
	//
	// It is deliberately about the HEAD rather than about clipping. `drawBody` clamps, so a child that
	// strays is invisible rather than overlapping — but a control the hand can see is not the same as a
	// control the hand can reach, and half a value box under a header is what a player actually gets.
	int checked = 0;
	size_t seatedFY = 0; // backlog FY: an add that fails is a device this sweep never met, so it is counted
	for (const device::Descriptor& d : kDevices) {
		Fixture f;
		if (f.add(d.traits.tag) == kNoInstance) {
			continue;
		}
		++seatedFY;
		f.editor.syncCards();
		f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
		f.editor.resized();

		for (ui::FxCard* c : f.editor.cardsForTest()) {
			ui::FxCard* card = c;
			if (card == nullptr) {
				continue;
			}
			// THE SPINE IS THE POINT. A top head puts `bodyLeft()` at zero and every assertion below
			// passes vacuously, so a run that somehow got a vertical lane would report success having
			// asked nothing.
			REQUIRE(card->headEdge() == ui::CardHeadEdge::Left);
			const float left = card->bodyLeft();
			REQUIRE(left > 0.0f);

			// AS A STRING: a bare `const char*` prints as `1` here, which names no device.
			const std::string tagName = d.traits.tag;
			CAPTURE(tagName);
			for (visage::Frame* child : card->children()) {
				if (child == nullptr || !child->isVisible()) {
					continue;
				}
				// THE HEAD'S OWN MARKS ARE NOT CHILDREN — they are painted by `drawCardFrame` and by
				// `FxCard::draw`, so everything in this list belongs to the body by construction.
				const std::string childName = child->name().empty() ? std::string("<unnamed>")
				                                                    : std::string(child->name());
				CAPTURE(childName);
				CAPTURE(child->x());
				CAPTURE(left);
				CHECK(static_cast<float>(child->x()) >= left);
				// AND NOT PUSHED OFF THE OTHER END BY THE FIX. Every correction above added `bodyLeft()`
				// to a coordinate; a cell width still derived from the CARD's width rather than the
				// body's would now overflow by exactly the head's 24px, and the left-edge assertion
				// cannot see that at all.
				CHECK(static_cast<float>(child->right()) <= static_cast<float>(card->width()) + 0.5f);
				++checked;
			}
		}
	}
	CHECK(seatedFY == feathers::kDeviceCount); // backlog FY: EVERY registered device was seated, not "the loop ran"
	// AND IT LOOKED AT SOMETHING. Every `continue` above is legitimate, so an empty sweep is a green
	// that asserted nothing — the shape this repo has been caught by more than once.
	CHECK(checked > 20);
}

TEST_CASE("every mark a spine draws can be pressed, and lands on its own verb") {
	// The symptom: the (...) menu did not work and was not drawn on the header, and the
	// expand-to-window/canvas box was missing from the vertical headers.
	//
	// **BOTH HALVES OF THAT REPORT ARE ONE RECTANGLE.** A card draws its menu at some coordinates and
	// hit-tests the same ones, so a geometry that did not transpose put the mark off the head AND the
	// press somewhere else — one defect that reads as two. The fix is that there is now exactly one
	// definition per mark, on `FxCard`; this case is what makes that claim checkable, because a
	// photograph shows the mark and cannot show the press.
	//
	// It asserts REACHABILITY, not appearance: the rectangle is inside the head, and pressing its
	// centre reaches the card rather than a sibling. What the verb then does is each verb's own case.
	int checked = 0;
	size_t seatedFY = 0; // backlog FY: an add that fails is a device this sweep never met, so it is counted
	for (const device::Descriptor& d : kDevices) {
		Fixture f;
		if (f.add(d.traits.tag) == kNoInstance) {
			continue;
		}
		++seatedFY;
		f.editor.syncCards();
		f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
		f.editor.resized();

		const std::string tagName = d.traits.tag;
		CAPTURE(tagName);

		for (ui::FxCard* c : f.editor.cardsForTest()) {
			ui::FxCard* card = c;
			if (card == nullptr) {
				continue;
			}
			REQUIRE(card->headEdge() == ui::CardHeadEdge::Left);

			const struct {
				const char* name;
				visage::Bounds rect;
			} marks[] = {
				{ "slot menu", card->slotMenuBounds() },
				{ "enable", card->enableBounds() },
				{ "expand", card->expandBounds() },
			};

			for (const auto& mark : marks) {
				if (mark.rect.width() <= 0.0f) {
					// ABSENT IS ALLOWED — a device with no enable field has no square, and one that
					// declares `hasExpanded = false` has no arrow. Three of the rack's own devices do
					// exactly that, which is why the absence noticed on AUTOMOD and MSEG is
					// correct rather than the bug it looks like.
					continue;
				}
				const std::string markName = mark.name;
				CAPTURE(markName);

				// **INSIDE THE SPINE.** The head is `kCardHeadHeight` WIDE here, not tall, and a mark
				// still spelled in top-head coordinates lands outside it — which is precisely what a
				// press then misses.
				CHECK(mark.rect.x() >= 0.0f);
				CHECK(mark.rect.right() <= ui::kCardHeadHeight);
				CHECK(mark.rect.y() >= 0.0f);
				CHECK(mark.rect.bottom() <= static_cast<float>(card->height()));

				// AND THE PRESS REACHES THIS CARD. Without this the case is about arithmetic; with it
				// it is about a hand.
				const visage::Point centre =
				    card->positionInWindow()
				    + visage::Point { mark.rect.x() + mark.rect.width() * 0.5f,
					                  mark.rect.y() + mark.rect.height() * 0.5f };
				CHECK(f.editor.frameAtPoint(centre) == card);
				++checked;
			}
		}
	}
	CHECK(seatedFY == feathers::kDeviceCount); // backlog FY: EVERY registered device was seated, not "the loop ran"
	CHECK(checked > 10);
}

TEST_CASE("add before, in the rack's editor: the verb lands the new device before THAT card, by its id, and the cards follow") {
	// THE RACK PLUGIN'S ENTRY FOR CR, asserted through the verb the menu ends in rather than the
	// popup (the removal case's precedent). The anchor is the card's DEVICE — which is what
	// `showSlotMenu` binds per card — and the record places the newcomer before wherever it stands.
	Fixture f;
	const DeviceInstanceId first = f.add(fx::filter::kTraits.tag);
	const DeviceInstanceId second = f.add(fx::owltt::kTraits.tag);
	REQUIRE(first != kNoInstance);
	REQUIRE(second != kNoInstance);
	f.editor.syncCards();
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.resized();
	REQUIRE(liveCards(f.editor) == 2);
	REQUIRE(f.bridge.deviceAtPosition(1) == second);
	REQUIRE(f.bridge.deviceAtPosition(0) != second);

	const DeviceInstanceId born = f.editor.insertDeviceBeforeForTest(second, fx::shaper::kTraits.tag);
	REQUIRE(born != kNoInstance);
	// THE VIEW AT ONCE, THEN THE ENGINE AFTER THE DRAIN: first, born, second.
	CHECK(f.bridge.deviceAtPosition(1) == born);
	CHECK(f.bridge.deviceAtPosition(2) == second);
	f.bridge.drain();
	CHECK(f.engine.chainLength() == 3);
	CHECK(f.engineDeviceAt(1) == born);
	CHECK(f.engineDeviceAt(2) == second);
	f.editor.syncCards();
	f.editor.resized();
	CHECK(liveCards(f.editor) == 3);

	SUBCASE("an anchor that is gone by the pick adds nothing and says so") {
		f.bridge.removeDevice(second);
		f.bridge.drain();
		const DeviceInstanceId none = f.editor.insertDeviceBeforeForTest(second, fx::shaper::kTraits.tag);
		CHECK(none == kNoInstance);
		CHECK(f.editor.notifierTextForTest().find("nothing added") != std::string::npos);
	}
}

TEST_CASE("Delete and Backspace remove the focused device, and nothing else does") {
	// Delete or Backspace with a device focused removes it, which makes the (...) remove verb
	// close to optional.
	//
	// THE POINT OF THE FOCUS HALF is that a key acts on a device the hand CHOSE. A road that deleted
	// "the last slot" or "slot 0" would pass every test about removal and be wrong about the only
	// thing that matters, so every assertion here names WHICH device went.
	// THROUGH THE SLOT VIEW, which is the seam both hosts read a chain's shape through — the engine's
	// own vector is private and asking for it would be this case reaching past the interface it is
	// nominally testing.
	const auto tagsOf = [](const rack::RackEngine& e) {
		std::vector<std::string> out;
		const backplane::SlotView slots = e.slots();
		for (size_t i = 0; i < slots.size(); ++i) {
			const device::Descriptor* d = slots.descriptorAtLane(i);
			out.emplace_back(d != nullptr ? d->traits.tag : "");
		}
		return out;
	};

	Fixture f;
	REQUIRE(f.add(fx::filter::kTraits.tag) != kNoInstance);
	REQUIRE(f.add(fx::owltt::kTraits.tag) != kNoInstance);
	REQUIRE(f.add(fx::shaper::kTraits.tag) != kNoInstance);
	f.editor.syncCards();
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.resized();
	REQUIRE(liveCards(f.editor) == 3);

	const auto press = [&](visage::KeyCode code) {
		const visage::KeyEvent e { code, 0, true };
		return f.editor.onKeyPress().callback(e);
	};

	SUBCASE("with nothing focused the key is passed on, not swallowed") {
		REQUIRE(f.editor.focusedDeviceForTest() == kNoInstance);
		// **FALSE, NOT MERELY HARMLESS.** A layer that returns true for a key it did not act on eats
		// that key for the editor's whole life — the failure `ExpandedOverlay`'s Esc handler documents
		// having had to learn. Returning false is what lets a second road ever claim Delete.
		CHECK_FALSE(press(visage::KeyCode::Delete));
		CHECK(liveCards(f.editor) == 3);
	}

	SUBCASE("the focused one goes, and it is the one the hand chose") {
		const std::vector<std::string> before = tagsOf(f.engine);
		// THE MIDDLE SLOT, deliberately: slot 0 and the last slot are both what a wrong implementation
		// would reach for, and either would pass a case that only counted the survivors.
		f.editor.focusDevice(f.bridge.deviceAtPosition(1));
		CHECK(press(visage::KeyCode::Delete));
		CHECK(liveCards(f.editor) == 2);
		// THE EDITOR'S OWN REMOVE IS A QUEUED VERB (AS 2) — the cards go at once because the bridge's
		// mirror is optimistic, and the ENGINE learns at the drain. Both halves are asserted: the count
		// above needs no drain, the tags below do.
		f.bridge.drain();
		const std::vector<std::string> after = tagsOf(f.engine);
		// **A RACK SLOT IS A FIXED POSITION AND NOTHING SLIDES.** The first draft of this case asserted
		// that the device below moved up, which is Feathers' fx chain and NOT this engine: routes are
		// keyed by slot index here, so compacting would silently re-point every route above the hole.
		// `removeDevice` clears in place, and the assertion that catches "removed the wrong index" is
		// therefore that the NEIGHBOURS DID NOT MOVE.
		REQUIRE(after.size() == before.size());
		CHECK(after[1].empty());
		CHECK(after[0] == before[0]);
		CHECK(after[2] == before[2]);
	}

	SUBCASE("Backspace is the same verb") {
		f.editor.focusDevice(f.bridge.deviceAtPosition(0));
		const std::vector<std::string> before = tagsOf(f.engine);
		CHECK(press(visage::KeyCode::Backspace));
		CHECK(liveCards(f.editor) == 2);
		f.bridge.drain(); // the queued verb, as above
		const std::vector<std::string> after = tagsOf(f.engine);
		CHECK(after[0].empty());
		CHECK(after[1] == before[1]);
	}

	SUBCASE("removing through the menu drops focus too, because it is the same verb") {
		// **THE ROAD THAT WAS MISSING IT.** `showSlotMenu` inlined the removal instead of calling
		// `removeSlot`, and the inlined copy was short by exactly the focus reset — so a device removed
		// from the ⋯ menu left `focusedSlot_` naming an empty slot. The commit that added `removeSlot`
		// asserted the two roads were one implementation; they were not, and nothing tested it.
		//
		// ASSERTED THROUGH THE VERB, NOT THE POPUP. `showSlotMenu` puts a real menu on the tree and
		// waits for a hand; `removeSlot` is what its selection now calls, and it is the shared road
		// whose behaviour this case is about.
		f.editor.focusDevice(f.bridge.deviceAtPosition(1));
		REQUIRE(f.editor.focusedDeviceForTest() == f.bridge.deviceAtPosition(1));
		f.editor.removeDevice(f.bridge.deviceAtPosition(1));
		CHECK(liveCards(f.editor) == 2);
		CHECK(f.editor.focusedDeviceForTest() == kNoInstance);
	}

	SUBCASE("focus does not survive the device") {
		f.editor.focusDevice(f.bridge.deviceAtPosition(2));
		REQUIRE(press(visage::KeyCode::Delete));
		// **OR THE NEXT DELETE TAKES A DEVICE NOBODY CHOSE.** A slot index kept across a removal names
		// whatever slid into that position, so a second press would remove a card the hand never
		// touched — silently, and with the same "it deleted something" evidence as a correct run.
		CHECK(f.editor.focusedDeviceForTest() == kNoInstance);
		CHECK_FALSE(press(visage::KeyCode::Delete));
		CHECK(liveCards(f.editor) == 2);
	}

	SUBCASE("a press on a card focuses it") {
		// THE ROAD THE HAND ACTUALLY TAKES. `focusSlot` above is the seam; this is the gesture, and
		// without it the whole feature is reachable only from a test.
		ui::FxCard* card = f.editor.cardsForTest()[2];
		REQUIRE(card != nullptr);
		visage::MouseEvent press {};
		press.position = { card->width() * 0.5f, ui::kCardHeadHeight * 0.5f };
		press.button_id = visage::kMouseButtonLeft;
		press.button_state = visage::kMouseButtonLeft;
		press.is_down = true;
		press.repeat_click_count = 1;
		card->onMouseDown().callback(press);
		CHECK(f.editor.focusedDeviceForTest() == f.bridge.deviceAtPosition(2));
		CHECK(card->headFocused());
	}
}

TEST_CASE("Backspace takes the card under the hand - even when the press landed on one of its controls") {
	// **THE SYMPTOM:** deleting a modulator with Backspace did not work properly while (...) -> remove
	// did: after a Backspace delete, its ghost modulation remained.
	//
	// **THE TWO ROADS ARE ONE FUNCTION AND NEITHER OMITS A LINE.** `showSlotMenu`'s selection and the
	// key handler both end in `removeSlot`. What differs is the ARGUMENT: the menu captures its lane
	// per card at bind time (`card->onSlotMenu = [this, i] { showSlotMenu(i); }`), the key reads
	// `focusedSlot_`, and `focusedSlot_` is written by exactly one thing — `FxCard::onCardPressed`,
	// fired from `noteCardPress`, which every card calls from its OWN `onMouseDown`.
	//
	// **AND MOST OF A CARD IS NOT THE CARD.** `visage::Frame::propagateMouseEvent` hands a press to an
	// ancestor only if that ancestor asked with `setReceiveChildMouseEvents(true)`, and NOTHING in
	// `src/` calls it. A modulator's card is largely its value boxes (`env_card.cpp:150`,
	// `mseg_card.cpp:150`) and a filter's is largely its knobs (`filter_card.cpp:139`), so a press
	// that a hand reads as "I picked this card" lands on a CHILD and the card never hears it — while
	// `WindowEventHandler::handleMouseDown` moves the KEYBOARD focus to the editor just the same,
	// because it walks to the nearest keystroke-accepting ANCESTOR. The key arrives, and acts on
	// whichever card's bare body was pressed last.
	//
	// **THE GHOST IS THE SURVIVOR'S ROUTES.** The device the hand aimed at is never removed, so
	// `clearDeviceSlotRoutes` never runs for its lane and it goes on modulating.
	//
	// **THE VALUE IS ASSERTED, NOT A PICTURE OR A COUNT.** The route cell is read by coordinate. "The
	// chain got shorter" passes whenever anything goes, which is exactly the case here — something
	// always goes, and it is the wrong thing.
	// **THREE DEVICES, AND THE THIRD IS WHAT MAKES THE GHOST READABLE.** The first draft routed the
	// modulator at a control on the device the stale focus removes — so the cell went to zero on the
	// BUGGY build too, cleared by the wrong device's own removal, and the ghost assertion passed while
	// the modulator sat there still modulating. The route has to land on a device that SURVIVES either
	// outcome, or the measurement answers a question nobody asked.
	Fixture f;
	const DeviceInstanceId fxLane = f.add(fx::filter::kTraits.tag);
	const DeviceInstanceId modLane = f.add(mod::dahdsr::kTraits.tag);
	const DeviceInstanceId targetLane = f.add(fx::shaper::kTraits.tag);
	REQUIRE(fxLane != kNoInstance);
	REQUIRE(modLane != kNoInstance);
	REQUIRE(targetLane != kNoInstance);
	f.editor.syncCards();
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.resized();
	REQUIRE(liveCards(f.editor) == 3);

	// THE MODULATOR'S OWN ROUTE — MADE BY ENDPOINT, the bridge's road, and READ BY COORDINATE, the
	// engine's. `deviceSourceOf` and `deviceDestinationOfOrdinal` are the arithmetic the engine's own
	// clear uses, restated here rather than read back from it, over the lanes the engine keeps the two
	// devices at.
	f.bridge.setRouteAmount(backplane::Source::output(modLane, 0),
	                        backplane::Destination::control(
	                            targetLane, fx::shaper::kDescriptor.controlOfNthModulatableBesideEnable(0)),
	                        0.5f);
	// BY IDENTITY, WHICH IS WHAT THE BRIDGE WAS ALREADY HANDED ABOVE — the engine's API speaks endpoints
	// since kill-lanes §2.4b, so the two spellings this used to hold (the bridge's, and the engine's flat
	// coordinates computed through `f.laneOf`) are one spelling.
	const backplane::Source source = backplane::Source::output(modLane, 0);
	const backplane::Destination destination =
	    backplane::Destination::control(targetLane, fx::shaper::kDescriptor.controlOfNthModulatableBesideEnable(0));
	f.bridge.drain();
	REQUIRE(f.engine.routeAmount(source, destination) == doctest::Approx(0.5f));
	// THE THREE LANES, TAKEN NOW WHILE ALL THREE ARE HELD — the engine is asked about them after the
	// removal, when the record no longer knows the one that went.
	const size_t fxAt = f.laneOf(fxLane);
	const size_t modAt = f.laneOf(modLane);
	const size_t targetAt = f.laneOf(targetLane);

	const auto press = [&](visage::KeyCode code) {
		const visage::KeyEvent e { code, 0, true };
		return f.editor.onKeyPress().callback(e);
	};

	// **THE PRESS GOES THROUGH THE TREE, NOT INTO A HANDLER.** The case that already exists above
	// calls `card->onMouseDown()` directly, which is why it could never see this — a test that
	// INVOKES a handler is not a test that the handler is REACHED (backlog CO).
	//
	// **BOTH HALVES OF `WindowEventHandler::handleMouseDown`, RESTATED FROM ITS SOURCE**
	// (`visage_app/window_event_handler.cpp:303-326`) — the frame under the point gets the press, and
	// the nearest keystroke-accepting ANCESTOR becomes the keyboard-focused frame and is told so.
	// Restating only the first half would be a venue that cannot express the outcome: the focus walk
	// IS the road under test, and a helper that skipped it would report a fix as broken and a break
	// as fixed. There is no window here to do it for us.
	const auto pressAtWindowPoint = [&](visage::Point where) {
		visage::Frame* target = f.editor.frameAtPoint(where);
		REQUIRE(target != nullptr);
		visage::Frame* focus = target;
		while (focus != nullptr && !focus->acceptsKeystrokes()) {
			focus = focus->parent();
		}
		if (focus != nullptr) {
			focus->processFocusChanged(true, true);
		}
		visage::MouseEvent e {};
		e.event_frame = target;
		e.window_position = where;
		e.position = where - target->positionInWindow();
		e.button_id = visage::kMouseButtonLeft;
		e.button_state = visage::kMouseButtonLeft;
		e.is_down = true;
		e.repeat_click_count = 1;
		target->processMouseDown(e);
		return target;
	};

	// A POINT INSIDE ONE OF THE MODULATOR CARD'S CHILD CONTROLS. Found by asking the card for its
	// children rather than by guessing a coordinate off the layout, so the case cannot rot into
	// pressing empty card when the DAHDSR's furniture moves.
	ui::FxCard* modCard = f.editor.cardOfDeviceForTest(modLane);
	REQUIRE(modCard != nullptr);
	visage::Frame* control = nullptr;
	for (visage::Frame* child : modCard->children()) {
		if (child->isVisible() && child->width() > 2.0f && child->height() > 2.0f) {
			control = child;
			break;
		}
	}
	REQUIRE(control != nullptr);
	const visage::Point onControl = control->positionInWindow()
	                                + visage::Point { control->width() * 0.5f, control->height() * 0.5f };

	SUBCASE("the key road") {
		// FOCUS SOMEWHERE ELSE FIRST, the way a session actually goes: the effect was selected, then
		// the hand moved to the modulator. Without this the stale index would happen to be right.
		f.editor.focusDevice(fxLane);
		REQUIRE(f.editor.focusedDeviceForTest() == fxLane);

		// **AND THE PRESS MUST REACH THE CHILD, or this case is about a body press and proves nothing.**
		CHECK(pressAtWindowPoint(onControl) == control);
		// THE MECHANISM THE FIX RESTS ON, PINNED: the first keystroke-accepting frame above that
		// control is the CARD. If a control ever accepts keystrokes itself, or the card stops, focus
		// stops arriving here and the line above goes on passing — so this is asserted rather than
		// assumed.
		visage::Frame* accepting = control->parent();
		while (accepting != nullptr && !accepting->acceptsKeystrokes()) {
			accepting = accepting->parent();
		}
		CHECK(accepting == static_cast<visage::Frame*>(modCard));

		// THE WHOLE BUG IN ONE ASSERTION: what a Backspace would act on, after a press on the card.
		CHECK(f.editor.focusedDeviceForTest() == modLane);

		REQUIRE(press(visage::KeyCode::Backspace));
		f.bridge.drain();
		// **THE MODULATOR IS GONE AND SO ARE ITS ROUTES.** The cell, by coordinate — not a count. It
		// reads 0.5 on the broken build: the device the hand aimed at was never removed, so
		// `clearDeviceSlotRoutes` never ran for its lane and it goes on modulating. That is the ghost.
		CHECK(f.engine.slots().descriptorAtLane(modAt) == nullptr);
		CHECK(f.engine.routeAmount(source, destination) == doctest::Approx(0.0f));
		// AND THE TWO THE HAND DID NOT CHOOSE SURVIVED, which is the half that catches "removed
		// everything" and "removed the focused one after all".
		CHECK(f.engine.slots().descriptorAtLane(fxAt) != nullptr);
		CHECK(f.engine.slots().descriptorAtLane(targetAt) != nullptr);
	}

	SUBCASE("the menu road - the twin that makes this a road difference") {
		// **GREEN BEFORE THE FIX AS WELL AS AFTER**, which is the whole point of it standing here: it
		// is what turns "removal leaves a ghost" into "the KEY road leaves a ghost". The menu never
		// reads `focusedSlot_`, so a stale focus cannot reach it.
		f.editor.focusDevice(fxLane);
		f.editor.removeDevice(modLane);
		f.bridge.drain();
		CHECK(f.engine.slots().descriptorAtLane(modAt) == nullptr);
		CHECK(f.engine.routeAmount(source, destination) == doctest::Approx(0.0f));
		CHECK(f.engine.slots().descriptorAtLane(fxAt) != nullptr);
		CHECK(f.engine.slots().descriptorAtLane(targetAt) != nullptr);
	}
}

TEST_CASE("a fresh rack reports a usable sample rate, because a UI draws before prepare") {
	// **ZERO IS NOT "UNKNOWN" TO ARITHMETIC, IT IS A DIVISION.** `RackEngine::sampleRate_` started at
	// 0.0 and `prepare` is what sets it — but an editor opens and draws BEFORE any host prepares
	// anything, and that number goes straight into `flt::gainProfile`.
	//
	// MEASURED, which is why this is a case and not a tidy-up: at a rate of 0 the profile is NaN at
	// every point. The filter card drew an EMPTY BOX in the rack — no crash, no log, just a blank
	// rectangle where a response belongs. It only surfaced when the filter got a real card instead of
	// a generic knob list; before that nothing in the rack asked for a rate.
	//
	// `GuiBridge` and `FxRunner` both default to 48000 already. This was the one road that did not.
	rack::RackEngine fresh { kAxes, hostCatalogue() };
	CHECK(fresh.sampleRate() > 0.0);

	// AND THE PROFILE IT PRODUCES IS REAL. The rate alone is a proxy; this is the thing that was
	// broken, so this is what the case asserts — a difference check against "not NaN" would pass on a
	// rate of 1 Hz, which is finite and equally useless.
	fx::filter::Settings s {};
	s.enabled = true;
	std::array<float, 16> freq {};
	std::array<float, 16> out {};
	for (size_t i = 0; i < freq.size(); ++i) {
		freq[i] = 40.0f * static_cast<float>(i + 1);
	}
	fx::filter::gainProfile(s, fresh.sampleRate(), freq.data(), out.data(), out.size());
	size_t finite = 0;
	for (const float v : out) {
		if (std::isfinite(v)) {
			++finite;
		}
	}
	CHECK(finite == out.size());
	// AND NOT FLAT-ZERO EITHER, which is the other way a "valid" profile says nothing.
	CHECK(*std::max_element(out.begin(), out.end()) > 0.1f);

	// THEN THE HOST'S RATE WINS. The default is a placeholder for one frame, not a value that sticks.
	fresh.prepare(96000.0, 4096);
	CHECK(fresh.sampleRate() == doctest::Approx(96000.0));
}

TEST_CASE("the filter card's painted head row clears the spine, which no sweep can see") {
	// **THE LIMIT OF `on a spine no card draws a child underneath its own header`, STATED AS A CASE.**
	// That sweep walks CHILD FRAMES, and it found the same defect in seven cards. It cannot see
	// geometry a card PAINTS: `FilterCard::arrowBounds(-1)` returned `kPad` — the card's origin —
	// while `typeMenuBounds` one line above it returned `bodyLeft() + kPad`, so on a spine the back
	// arrow was drawn and hit-tested underneath the header. A guard asymmetry inside one function,
	// invisible to the sweep and visible in a photograph.
	//
	// SO THE RECTANGLES ARE NAMED HERE. There is no generic way to enumerate what a card draws, which
	// means this kind of case is per-card and the sweep remains the thing that scales — worth writing
	// down so the next reader knows which one to reach for.
	Fixture f;
	const DeviceInstanceId slot = f.add(fx::filter::kTraits.tag);
	REQUIRE(slot != kNoInstance);
	f.editor.syncCards();
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.resized();

	auto* card = dynamic_cast<ui::FilterCard*>(f.editor.cardOfDeviceForTest(slot));
	// AND IT IS THE REAL CARD, not the generic grid. If the filter ever falls back again this case
	// should say so rather than quietly testing nothing.
	REQUIRE(card != nullptr);
	REQUIRE(card->headEdge() == ui::CardHeadEdge::Left);

	const float left = card->bodyLeft();
	REQUIRE(left > 0.0f);

	const struct {
		const char* name;
		visage::Bounds rect;
	} painted[] = {
		{ "back arrow", card->arrowBounds(-1) },
		{ "forward arrow", card->arrowBounds(1) },
		{ "type menu", card->typeMenuBounds() },
		{ "curve", card->curveBounds() },
	};

	for (const auto& part : painted) {
		const std::string partName = part.name;
		CAPTURE(partName);
		CAPTURE(part.rect.x());
		CAPTURE(left);
		REQUIRE(part.rect.width() > 0.0f);
		CHECK(part.rect.x() >= left);
		CHECK(part.rect.right() <= static_cast<float>(card->width()) + 0.5f);
	}
}

TEST_CASE("the rack reorders by drag, along x, and a drop where it already is moves nothing") {
	// Devices move by a mouse drag. The rack had NO reorder at all — no
	// engine verb and no gesture — while Feathers has had both for weeks. This is the lane's half.
	//
	// **ALONG X, WHICH IS THE ONLY THING THAT DIFFERS FROM THE FX LANE'S.** The gap arithmetic is
	// shared (`ui::gapAlong`, `ui::rowForGap`) rather than restated, because a second copy of an
	// off-by-one that took a dedicated header to get right is a second chance to get it wrong.
	Fixture f;
	const DeviceInstanceId filterDevice = f.add(fx::filter::kTraits.tag);
	const DeviceInstanceId owlttDevice = f.add(fx::owltt::kTraits.tag);
	const DeviceInstanceId shaperDevice = f.add(fx::shaper::kTraits.tag);
	REQUIRE(filterDevice != kNoInstance);
	REQUIRE(owlttDevice != kNoInstance);
	REQUIRE(shaperDevice != kNoInstance);
	f.editor.syncCards();
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.resized();

	// **BY POSITION, THROUGH THE CHAIN.** `slots()` is a view over LANES since step 2, and a drag
	// changes POSITIONS while leaving every lane exactly where it was — so this helper asking
	// `descriptorAtLane(slot)` would report that a drag did nothing at all. Which is what it did report,
	// and is the fifth spelling in this refactor whose meaning moved while its signature did not.
	const auto tagAt = [&](size_t position) {
		f.bridge.drain(); // as liveOrder: the engine is asked after the handoff (AS 2)
		const size_t lane = f.engine.deviceAtPosition(position);
		if (lane == rack::RackEngine::kNoSlot) {
			return std::string();
		}
		const device::Descriptor* d = f.engine.slots().descriptorAtLane(lane);
		return std::string(d != nullptr ? d->traits.tag : "");
	};

	// NOT VACUOUS: the three cards must actually be at different x, or every drop names the same gap
	// and the case passes without exercising the axis at all.
	REQUIRE(f.editor.cardsForTest()[0]->x() < f.editor.cardsForTest()[1]->x());
	REQUIRE(f.editor.cardsForTest()[1]->x() < f.editor.cardsForTest()[2]->x());

	SUBCASE("dragging the last card to the front reorders the chain") {
		// **THE LEFT HALF OF THE FIRST CARD, NOT ITS CENTRE.** `gapAlong` answers by the MIDPOINT — the
		// bumper rule `drop_gap.h` documents — so a drop exactly on card 0's centre means "AFTER card
		// 0" and lands the shaper in slot 1. My first draft aimed at the centre and read the resulting
		// correct behaviour as an off-by-one. Aim where the hand would aim to mean "before everything".
		const visage::Bounds first { static_cast<float>(f.editor.cardsForTest()[0]->x()),
			                         static_cast<float>(f.editor.cardsForTest()[0]->y()),
			                         static_cast<float>(f.editor.cardsForTest()[0]->width()),
			                         static_cast<float>(f.editor.cardsForTest()[0]->height()) };
		const visage::Point target { first.x() + first.width() * 0.25f,
			                         first.y() + first.height() * 0.5f };
		const ui::FxCard& dragged = *f.editor.cardsForTest()[2];
		const visage::Point inCard { target.x - static_cast<float>(dragged.x()),
			                         target.y - static_cast<float>(dragged.y()) };
		f.editor.cardDragForTest(shaperDevice, ui::FxCard::DragPhase::Begin, inCard);
		f.editor.cardDragForTest(shaperDevice, ui::FxCard::DragPhase::Move, inCard);
		// THE GAP IS NAMED BEFORE THE DROP, so a failure says whether the pointer was misread or the
		// move was — two different bugs that a bare "it did not reorder" cannot tell apart.
		CHECK(f.editor.dropGapForTest().has_value());
		f.editor.cardDragForTest(shaperDevice, ui::FxCard::DragPhase::End, inCard);
		// **THE DROP IS A QUEUED VERB (AS 2).** The cards land where the hand put them at once — the
		// bridge's order mirror is optimistic — and the ENGINE is reordered at the drain. `tagAt` asks
		// the engine, so it asks after the handoff.
		f.bridge.drain();

		CHECK(tagAt(0) == fx::shaper::kTraits.tag);
		CHECK(tagAt(1) == fx::filter::kTraits.tag);
		CHECK(tagAt(2) == fx::owltt::kTraits.tag);
	}

	SUBCASE("a drop on its own position changes nothing") {
		// `rowForGap` answers nullopt for BOTH gaps either side of the dragged card — see drop_gap.h.
		// A move-by-zero would still spend an undo step and still announce itself.
		const std::string before = tagAt(0) + tagAt(1) + tagAt(2);
		const ui::FxCard& dragged = *f.editor.cardsForTest()[1];
		const visage::Point inCard { static_cast<float>(dragged.width()) * 0.5f,
			                         static_cast<float>(dragged.height()) * 0.5f };
		f.editor.cardDragForTest(owlttDevice, ui::FxCard::DragPhase::Begin, inCard);
		f.editor.cardDragForTest(owlttDevice, ui::FxCard::DragPhase::Move, inCard);
		f.editor.cardDragForTest(owlttDevice, ui::FxCard::DragPhase::End, inCard);
		CHECK(tagAt(0) + tagAt(1) + tagAt(2) == before);
	}

	SUBCASE("a drop outside the lane cancels") {
		const std::string before = tagAt(0) + tagAt(1) + tagAt(2);
		const ui::FxCard& dragged = *f.editor.cardsForTest()[2];
		const visage::Point out { -static_cast<float>(dragged.x()) - 50.0f, 0.0f };
		f.editor.cardDragForTest(shaperDevice, ui::FxCard::DragPhase::Begin, { 0.0f, 0.0f });
		f.editor.cardDragForTest(shaperDevice, ui::FxCard::DragPhase::Move, out);
		CHECK_FALSE(f.editor.dropGapForTest().has_value());
		f.editor.cardDragForTest(shaperDevice, ui::FxCard::DragPhase::End, out);
		CHECK(tagAt(0) + tagAt(1) + tagAt(2) == before);
	}
}

TEST_CASE("backlog JA: the rack reorders by a hand's drag on a card HEAD, delivered through the editor's tree") {
	// **THE CASE ABOVE CALLS `cardDragForTest`, WHICH PROVES THE LANE ACTS ON A DRAG AND NOT THAT A HAND
	// CAN START ONE.** Backlog JA was exactly that gap one surface over: SKLITTER's column had every road
	// but the hook. So here the press is hit-tested from the editor and the drag and the release go to
	// the frame that took it, as `WindowEventHandler` delivers them.
	Fixture f;
	const DeviceInstanceId filterDevice = f.add(fx::filter::kTraits.tag);
	const DeviceInstanceId shaperDevice = f.add(fx::shaper::kTraits.tag);
	REQUIRE(filterDevice != kNoInstance);
	REQUIRE(shaperDevice != kNoInstance);
	f.editor.syncCards();
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.resized();

	ui::FxCard* card = f.editor.cardsForTest()[1];
	REQUIRE(card != nullptr);
	const visage::Point from = card->positionInWindow()
	                           + visage::Point { card->width() * 0.3f, ui::FxCard::kSlotHeadHeight * 0.5f };
	visage::Frame* down = f.editor.frameAtPoint(from);
	REQUIRE(down == card); // the head is the card's own, not a child's
	const ui::FxCard* firstCard = f.editor.cardsForTest()[0];
	const visage::Point to { firstCard->positionInWindow().x + firstCard->width() * 0.25f, from.y };
	const auto eventAt = [&](visage::Point where, bool isDown) {
		visage::MouseEvent e {};
		e.event_frame = down;
		e.window_position = where;
		e.position = where - down->positionInWindow();
		e.button_id = visage::kMouseButtonLeft;
		e.button_state = isDown ? visage::kMouseButtonLeft : visage::kMouseButtonNone;
		e.is_down = isDown;
		e.repeat_click_count = 1;
		return e;
	};
	down->processMouseDown(eventAt(from, true));
	down->processMouseDrag(eventAt(to, true));
	REQUIRE(f.editor.dropGapForTest().has_value());
	CHECK(*f.editor.dropGapForTest() == 0);
	down->processMouseUp(eventAt(to, false));
	f.bridge.drain();

	CHECK(f.bridge.positionOf(shaperDevice) == 0);
	CHECK(f.bridge.positionOf(filterDevice) == 1);
}

TEST_CASE("the drop bumper sits between the two cards the gap is between") {
	// **THE RACK COMPUTED THE GAP AND DREW NOTHING WITH IT.** The drag committed correctly and looked
	// like nothing was happening, which is the worst of both — a gesture that works and gives no sign
	// that it does. A player would conclude reorder was broken.
	//
	// THIS IS GEOMETRY, NOT PIXELS. Nothing here renders; what can be asserted is that the bar lands
	// in the space BETWEEN the right card and the wrong one, which is the whole content of "the
	// bumper is in the right place".
	Fixture f;
	REQUIRE(f.add(fx::filter::kTraits.tag) != kNoInstance);
	REQUIRE(f.add(fx::owltt::kTraits.tag) != kNoInstance);
	f.editor.syncCards();
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.resized();

	const ui::FxCard& first = *f.editor.cardsForTest()[0];
	const ui::FxCard& second = *f.editor.cardsForTest()[1];
	REQUIRE(first.x() < second.x());

	// GAP 0 — before everything. Left of the first card.
	const visage::Bounds before = f.editor.gapBoundsForTest(0);
	REQUIRE(before.width() > 0.0f);
	CHECK(before.height() > 0.0f);
	CHECK(before.right() <= static_cast<float>(first.x()));

	// GAP 1 — between them. Right of the first, left of the second, which is the assertion that
	// actually distinguishes a correct bumper from one drawn at a plausible-looking coordinate.
	const visage::Bounds between = f.editor.gapBoundsForTest(1);
	CHECK(between.x() >= static_cast<float>(first.x() + first.width()));
	CHECK(between.right() <= static_cast<float>(second.x()));

	// GAP 2 — after the last. Right of the second card.
	const visage::Bounds after = f.editor.gapBoundsForTest(2);
	CHECK(after.x() >= static_cast<float>(second.x() + second.width()));

	// AND A GAP THAT NAMES NOTHING DRAWS NOTHING, rather than a bar at some default coordinate.
	CHECK(f.editor.gapBoundsForTest(3).width() == doctest::Approx(0.0f));

	// IT SPANS THE LANE VERTICALLY — a bumper as tall as the cards it sits between, not a dot.
	CHECK(between.height() >= static_cast<float>(first.height()) * 0.9f);
}

TEST_CASE("a double-click folds a rack card ONCE, and a drag from it reorders instead") {
	// Two symptoms: modulator headers did not collapse, and items in the chain could not be
	// reordered. Both are this gesture, and the rack was where the first fix did NOT reach.
	//
	// **THE RACK FIRED BOTH FOLD ROADS.** It binds `onHeadFold` AND its cards keep `FxCard`'s
	// `onMouseUp`, so a double-click folded on the press and un-folded on the release — landing
	// exactly where it started. Feathers' two surfaces each happened to fire ONE road, which is why
	// they worked and this did not. The press road is retired; the release road, which separates fold
	// from drag on MOTION, is the only one left.
	Fixture f;
	REQUIRE(f.add(fx::filter::kTraits.tag) != kNoInstance);
	REQUIRE(f.add(fx::owltt::kTraits.tag) != kNoInstance);
	f.editor.syncCards();
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.resized();

	ui::FxCard* card = f.editor.cardsForTest()[0];
	REQUIRE(card != nullptr);
	// **THE RACK WEARS ITS HEAD AS A SPINE DOWN THE LEFT, and this point used to be written as though
	// it were a title bar across the top.** `RackEditor::syncCards` sets `LaneAxis::Horizontal`, so
	// `headEdge()` is `Left` — asserted here rather than assumed, because it is the platform's fact and
	// the whole point of the coordinate below.
	//
	// It did not matter while ANY double-click folded a card: `{ width/2, 12 }` is in the rack card's
	// BODY, and the case passed because the body folded too. The head-only design made the
	// difference visible, and this case was the third spelling of "the head" in a file whose own
	// history names that as the source of its duplication bugs. It now ASKS the card.
	REQUIRE(card->headEdge() == ui::CardHeadEdge::Left);
	const visage::Point onHead =
	    card->headEdge() == ui::CardHeadEdge::Left
	        ? visage::Point { ui::kCardHeadHeight * 0.5f, static_cast<float>(card->height()) * 0.5f }
	        : visage::Point { static_cast<float>(card->width()) * 0.5f, ui::kCardHeadHeight * 0.5f };

	const auto press = [&](int clicks) {
		visage::MouseEvent e {};
		e.position = onHead;
		e.button_id = visage::kMouseButtonLeft;
		e.button_state = visage::kMouseButtonLeft;
		e.is_down = true;
		e.repeat_click_count = clicks;
		card->onMouseDown().callback(e);
	};
	const auto release = [&] {
		visage::MouseEvent e {};
		e.position = onHead;
		e.button_id = visage::kMouseButtonLeft;
		e.is_down = false;
		e.repeat_click_count = 2;
		card->onMouseUp().callback(e);
	};

	SUBCASE("a single click leaves it open") {
		REQUIRE_FALSE(card->folded());
		press(1);
		release();
		// ONE CLICK IS NOT THE GESTURE. Folding here is what made a double-click toggle twice.
		CHECK_FALSE(card->folded());
	}

	SUBCASE("a double-click folds it, once") {
		REQUIRE_FALSE(card->folded());
		press(1);
		press(2);
		release();
		CHECK(card->folded());

		// AND AGAIN BRINGS IT BACK — the round trip, which is what a collapsed card with no road back
		// could not do. That was the second half of the symptom and it is the half a fold-only case
		// cannot see.
		press(1);
		press(2);
		release();
		CHECK_FALSE(card->folded());
	}
}

TEST_CASE("a drag still MOVES after devices have been removed and added, not only shows the bumper") {
	// The symptom: moving devices worked at first, but after a while (and after adding/removing some),
	// the bumper still showed and on mouse release the card did not move.
	//
	// **THE BUMPER AND THE MOVE COUNTED IN DIFFERENT SPACES.** `dropGapAt` and `gapBounds` count over
	// the LIVE cards, so the bumper is right whatever holes the rack has. The release then handed the
	// dragged card's SLOT index to `rowForGap` as if it were its position in that same live list —
	// and `rowForGap` refuses a `from` at or past the row count, so once a hole sat before the dragged
	// card the release answered nullopt and `moveDevice` was never called. Holes are permanent now
	// that a re-add goes to the END rather than filling one, which is why the bug arrived with churn.
	//
	// DRIVEN AS A HAND WOULD — press, move, release on the card — rather than through
	// `cardDragForTest`, and asserted on the ENGINE's order: a rendered bumper is exactly the thing
	// that stayed correct while the move did not.
	Fixture f;
	REQUIRE(f.add(fx::filter::kTraits.tag) != kNoInstance);
	REQUIRE(f.add(fx::owltt::kTraits.tag) != kNoInstance);
	REQUIRE(f.add(fx::shaper::kTraits.tag) != kNoInstance);
	f.editor.syncCards();
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.resized();

	// THE TAGS AS THE DESCRIPTORS SPELL THEM — read from the traits, not guessed.
	const std::string F = std::string(fx::filter::kTraits.tag) + ' ';
	const std::string D = std::string(fx::owltt::kTraits.tag) + ' ';
	const std::string S = std::string(fx::shaper::kTraits.tag) + ' ';
	const std::string E = std::string(mod::dahdsr::kTraits.tag) + ' ';
	// THE LIVE ORDER, slot indices dropped: what the chain sounds like, hole or no hole.
	// **THE CHAIN'S ORDER, WHICH IS WHAT "ORDER" MEANS.** This walked lanes and its own comment below
	// called that "LANE ORDER" — which was the same list before step 2 and is not now: a drag changes
	// positions and leaves every lane alone, so the lane walk reports that nothing moved. This case
	// exists to catch a drag that does not move anything, so reading it the old way would have made it
	// permanently green against the very bug it was written for.
	const auto liveOrder = [&] {
		// ASKS THE ENGINE, SO IT ASKS AFTER THE HANDOFF (AS 2): the editor's verbs are queued and
		// a block or a flush drains them. Standing that in here keeps every caller honest without
		// a drain scattered at each gesture.
		f.bridge.drain();
		std::string order;
		for (size_t position = 0; position < f.engine.chainLength(0); ++position) {
			const size_t lane = f.engine.deviceAtPosition(position);
			if (lane == rack::RackEngine::kNoSlot) {
				continue;
			}
			if (const device::Descriptor* d = f.engine.slots().descriptorAtLane(lane); d != nullptr) {
				order += d->traits.tag;
				order += ' ';
			}
		}
		return order;
	};
	// THE LIVE CARDS IN CHAIN ORDER — the list the bumper counts over, and the order the ear hears.
	// **IN CHAIN ORDER, BECAUSE A "LIVE ROW" IS WHAT THE EYE SEES.** The editor keys its cards by DEVICE
	// — a card belongs to a device — so the order comes from the ENGINE's chain, each position's device
	// asked for its card. Walking the editor's store would give the order devices were ADDED, which
	// agrees until something is reordered: exactly the state this case puts the rack into.
	const auto liveList = [&] {
		std::vector<std::pair<DeviceInstanceId, ui::FxCard*>> live;
		for (size_t position = 0; position < f.engine.chainLength(0); ++position) {
			const DeviceInstanceId device = f.engineDeviceAt(position);
			if (ui::FxCard* card = f.editor.cardOfDeviceForTest(device); card != nullptr) {
				live.emplace_back(device, card);
			}
		}
		return live;
	};
	// A REAL GESTURE ON THE CARD AT LIVE ROW `fromRow`, dropped at `fraction` across the card at live
	// row `targetRow` — 0.25 means "before it" and 0.75 "after it", by `gapAlong`'s midpoint rule.
	// Events are in the dragged card's own coordinates, which is what the host delivers and what the
	// lane converts.
	const auto drag = [&](size_t fromRow, size_t targetRow, float fraction) {
		const auto live = liveList();
		REQUIRE(fromRow < live.size());
		REQUIRE(targetRow < live.size());
		ui::FxCard& card = *live[fromRow].second;
		const ui::FxCard& target = *live[targetRow].second;
		const visage::Point start { static_cast<float>(card.width()) * 0.5f,
			                         ui::kCardHeadHeight * 0.5f };
		// PAST THE THRESHOLD IN Y, which is the axis `noteCardMove` measures, and at the target in x.
		const visage::Point end { static_cast<float>(target.x())
			                          + static_cast<float>(target.width()) * fraction
			                          - static_cast<float>(card.x()),
			                      start.y + 20.0f };
		visage::MouseEvent e {};
		e.button_id = visage::kMouseButtonLeft;
		e.button_state = visage::kMouseButtonLeft;
		e.repeat_click_count = 1;
		e.position = start;
		e.is_down = true;
		card.onMouseDown().callback(e);
		e.position = end;
		card.onMouseDrag().callback(e);
		// THE BUMPER IS UP — the half of the gesture that was seen working.
		CHECK(f.editor.dropGapForTest().has_value());
		e.is_down = false;
		card.onMouseUp().callback(e);
	};

	// CONTROL: a dense rack moves. Without this the churn assertion below cannot tell "the road is
	// broken" from "the road never worked".
	REQUIRE(liveOrder() == F + D + S);
	drag(2, 0, 0.25f);
	REQUIRE(liveOrder() == S + F + D);

	// **THE CHURN: remove one, add one, remove another, add another.**
	//
	// It used to assert the LANES the adds landed on — 3 then 4 — and describe the result as
	// "[ -, -, owltt, dahdsr, filter ]", because a lane was a position and an add went past the last
	// live one. Under step 2 an add takes the LOWEST FREE lane and goes to the END of the chain: the
	// holes are reused immediately and the lane numbers are no longer part of the story. What the case
	// is about survives untouched — a drag must still move something after devices have come and gone
	// — so the lane assertions become "it was placed at all" and the ORDER carries the claim.
	//
	// REMOVED BY DEVICE, ASKED BY POSITION. `removeDevice` takes the device's id, and after the drag above
	// positions have moved; naming a raw number here would remove whichever device happened to stand there.
	f.remove(f.engineDeviceAt(1));
	f.editor.syncCards();
	REQUIRE(f.add(mod::dahdsr::kTraits.tag) != kNoInstance);
	f.editor.syncCards();
	f.remove(f.engineDeviceAt(0));
	f.editor.syncCards();
	REQUIRE(f.add(fx::filter::kTraits.tag) != kNoInstance);
	f.editor.syncCards();
	f.editor.resized();
	REQUIRE(liveOrder() == D + E + F);
	REQUIRE(liveCards(f.editor) == 3);

	// THE NEWEST CARD, DRAGGED TO THE FRONT. Its lane and its live row are different numbers — the
	// pair that cannot both be handed to `rowForGap`, which is what this case was written for.
	drag(2, 0, 0.25f);
	CHECK(liveOrder() == F + D + E);

	// AND THE OTHER WAY: the front card to the end.
	drag(0, 2, 0.75f);
	CHECK(liveOrder() == D + E + F);

	// AND INTO THE MIDDLE, so the answer is not only ever an end: the last card between the first two.
	drag(2, 1, 0.25f);
	CHECK(liveOrder() == D + F + E);
}

TEST_CASE("every card the rack holds is in the chain and gets laid out") {
	// **THE INVARIANT STEP 2 MADE POSSIBLE TO BREAK.** `cards_` is indexed by LANE and the layout
	// walks POSITIONS, so the two are joined only by the graph agreeing with the card array. If a card
	// exists at a lane the chain does not name, nothing lays it out — it keeps whatever bounds it last
	// had and sits there invisible, intercepting presses at coordinates that mean nothing. That is the
	// shape of "invisible things hanging onto focus", and no case asserted the two agree.
	//
	// ASSERTED AS A SET EQUALITY rather than a count: the same number of cards and lanes can still be
	// the wrong cards at the wrong lanes.
	Fixture f;
	// SIZED, OR EVERY CARD IS ZERO-HIGH AND THE LAYOUT ASSERTION BELOW PASSES ON A FIXTURE ARTEFACT
	// RATHER THAN ON THE CODE. Caught by the first run of this case reporting height 0 for every lane.
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	const auto devicesInChain = [&] {
		// ASKS THE ENGINE, SO IT ASKS AFTER THE HANDOFF (AS 2): the editor's verbs are queued and
		// a block or a flush drains them. Standing that in here keeps every caller honest without
		// a drain scattered at each gesture.
		f.bridge.drain();
		std::set<DeviceInstanceId> devices;
		for (size_t p = 0; p < f.engine.chainLength(0); ++p) {
			if (const DeviceInstanceId device = f.engineDeviceAt(p); device != kNoInstance) {
				devices.insert(device);
			}
		}
		return devices;
	};
	// EVERY CARD THE EDITOR HOLDS, WHEREVER THE CHAIN PUTS IT — asked device by device of every device the
	// record has ever issued in this case, so a card for a device the chain does not name is counted too.
	const auto devicesWithCards = [&] {
		std::set<DeviceInstanceId> devices;
		for (DeviceInstanceId id = kFirstInstance; id < kFirstInstance + 64; ++id) {
			if (f.editor.cardOfDeviceForTest(id) != nullptr) {
				devices.insert(id);
			}
		}
		return devices;
	};
	const auto agree = [&](const char* whenText) {
		CAPTURE(std::string(whenText));
		CHECK(devicesWithCards() == devicesInChain());
	};

	REQUIRE(f.add(fx::filter::kTraits.tag) != kNoInstance);
	REQUIRE(f.add(fx::owltt::kTraits.tag) != kNoInstance);
	REQUIRE(f.add(fx::shaper::kTraits.tag) != kNoInstance);
	f.editor.syncCards();
	f.editor.resized();
	agree("three added");

	// THE SEQUENCE THE TROUBLE WAS SEEN IN: reorder, delete, add.
	REQUIRE(f.move(2, 0));
	f.editor.syncCards();
	f.editor.resized();
	agree("after a reorder");

	f.editor.removeDevice(f.engineDeviceAt(1));
	agree("after a delete");

	REQUIRE(f.add(mod::dahdsr::kTraits.tag) != kNoInstance);
	f.editor.syncCards();
	f.editor.resized();
	agree("after an add into the freed lane");

	// **AND EVERY CARD IS WHERE THE CHAIN SAYS, WHICH IS NOT THE SAME AS HAVING A SIZE.**
	//
	// A FIRST VERSION OF THIS ASSERTED `width() > 0` AND A KNOWN-BAD WALKED STRAIGHT THROUGH IT.
	// Making the layout skip the first position left the case GREEN, because a card that stops being
	// laid out KEEPS THE BOUNDS IT LAST HAD — which is the whole of the invisible-frame failure and
	// exactly what a size check cannot see. A stale card is not a zero-sized card; it is a correctly
	// sized card in the wrong place.
	//
	// SO THE ASSERTION IS ABOUT ORDER ON SCREEN: walking the chain, each card must start to the RIGHT
	// of the previous one's right edge. A card the layout skipped keeps a position from before the
	// reorder and breaks the run.
	float previousRight = -1e9f;
	size_t placed = 0;
	for (size_t position = 0; position < f.engine.chainLength(0); ++position) {
		const DeviceInstanceId device = f.engineDeviceAt(position);
		REQUIRE(device != kNoInstance);
		const ui::FxCard* card = f.editor.cardOfDeviceForTest(device);
		REQUIRE(card != nullptr);
		CAPTURE(position);
		CAPTURE(device);
		CHECK(card->width() > 0);
		CHECK(card->height() > 0);
		// THE RUN, WHICH IS THE PART A STALE CARD FAILS.
		CHECK(static_cast<float>(card->x()) > previousRight);
		previousRight = static_cast<float>(card->x() + card->width());
		++placed;
	}
	CHECK(placed == f.engine.chainLength(0));
}

TEST_CASE("EVERY rack card's enable square toggles the device, both ways, and the light follows") {
	// **THE CLASS, NOT THE INSTANCE.** The symptom: enable/disable was broken for all effects —
	// either they disabled and never came back, or did not disable at all. Three cards were wrong in
	// three different ways and each looked like its own bug:
	//
	//   * `OwlttCard` toggled the value and never called `redraw()`, so the DEVICE bypassed correctly
	//     and the square kept its last paint — "looks like its just the light";
	//   * `GenericCard` had no enable arm at all, so the square was dead — and since the tracker was
	//     registered on the generic card, that is a whole device's bypass;
	//   * `MsegCard` and `EnvCard` returned for any head press before reaching one (fixed separately).
	//
	// **`fx_card.h` PREDICTED THIS IN ITS OWN WORDS** while the base took over the drawing: a fifth
	// spelling of a switch is a fifth thing to keep in step. The geometry and the painting are the
	// base's; the PRESS is still each card's, ten times over, and three of the ten were wrong.
	//
	// SO THIS SWEEPS THE REGISTRY rather than naming cards. A device added tomorrow is covered without
	// anybody remembering to come back — which is the only kind of guard that survives this shape.
	for (const device::Descriptor& desc : feathers::kDevices) {
		if (desc.audio == nullptr && desc.mod == nullptr && desc.notes == nullptr) {
			continue; // not a rack device
		}
		if (desc.traits.enableControl < 0) {
			continue; // declares no bypass; the square is not drawn and there is nothing to press
		}
		CAPTURE(std::string(desc.traits.name));

		Fixture f;
		f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
		const DeviceInstanceId lane = f.add(desc.traits.tag);
		REQUIRE(lane != kNoInstance);
		f.editor.syncCards();
		f.editor.resized();
		auto* card = f.editor.cardOfDeviceForTest(lane);
		REQUIRE(card != nullptr);
		REQUIRE(card->hasEnable());

		const auto control = static_cast<size_t>(desc.traits.enableControl);
		const auto press = [&] {
			const visage::Bounds sw = card->enableBounds();
			REQUIRE(sw.width() > 0.0f);
			visage::MouseEvent e {};
			e.button_id = visage::kMouseButtonLeft;
			e.button_state = visage::kMouseButtonLeft;
			e.repeat_click_count = 1;
			e.is_down = true;
			e.position = { sw.x() + sw.width() * 0.5f, sw.y() + sw.height() * 0.5f };
			card->onMouseDown().callback(e);
			// **AND THE HANDOFF (AS 2).** The square's write is a queued edit now; the light follows at
			// once from the bridge's optimistic mirror, and the DEVICE changes at the drain. This case
			// asserts both, so it has to perform the block's part.
			f.bridge.drain();
		};

		// IT STARTS ON — `addDevice` switches a hand-added device on deliberately — so "it changed" is
		// a change away from on rather than a no-op that happens to read differently.
		REQUIRE(f.engine.fieldAt(f.laneOf(lane), control) >= 0.5f);
		REQUIRE(card->headEnabled());

		press();
		CHECK(f.engine.fieldAt(f.laneOf(lane), control) < 0.5f);
		// **AND THE LIGHT FOLLOWS THE DEVICE.** Asserting only the field passes on OWLTT's bug, where
		// the value moved and the square did not; asserting only the light passes on a card that
		// repaints a lie. The pair is what makes this the reported symptom.
		CHECK_FALSE(card->headEnabled());

		// **AND BACK, WHICH IS THE HALF THAT WAS BROKEN.** A toggle that only goes one way is a switch
		// with one position, and a case that pressed once would have passed on every card here.
		press();
		CHECK(f.engine.fieldAt(f.laneOf(lane), control) >= 0.5f);
		CHECK(card->headEnabled());
	}
}

TEST_CASE("every coordinate box in the rack can print its own widest readout") {
	// **THE SHAPER'S COLOUR BOX IS THE ONE PHOTOGRAPHED; IT IS NOT THE ONLY ONE.** Seven
	// surfaces hold a `ZoneCoordBox` and each lays its own row, so a fix verified on one card says
	// nothing about the rest — the same reasoning the spine case above gives for sweeping every
	// child of every card rather than writing a case per card.
	//
	// The widget now tightens its own chrome when the room is short, which every holder gets for
	// free; what does NOT travel is the row weighting, which each card spells itself. So this asks
	// the question of every device the browser offers, and a card that starts holding a coordinate
	// box tomorrow is asked without anybody remembering to add it.
	int checked = 0;
	size_t seatedFY = 0; // backlog FY: an add that fails is a device this sweep never met, so it is counted
	for (const device::Descriptor& d : kDevices) {
		Fixture f;
		if (f.add(d.traits.tag) == kNoInstance) {
			continue;
		}
		++seatedFY;
		f.editor.syncCards();
		f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
		f.editor.resized();

		const std::string tagName = d.traits.tag;
		CAPTURE(tagName);

		for (ui::FxCard* c : f.editor.cardsForTest()) {
			const ui::FxCard* card = c;
			if (card == nullptr) {
				continue;
			}
			// EVERY KIND THAT HOLDS ONE. A card whose coordinate boxes are unreachable from here is
			// simply not covered — which is why the count is asserted at the end rather than left to
			// be inferred from a green run.
			// **TWO WIDGET TYPES NOW, SO THE SWEEP COLLECTS THE TWO NUMBERS RATHER THAN THE POINTER.**
			// Automod's coordinates collapsed to one param each and its card seats `ZoneCoordCell`;
			// the shaper's pair is genuinely two quantities (measured — see `test_fx_shaper.cpp`) and
			// keeps `ZoneCoordBox`. Both answer `width()` and `minimumWidth()`, which is all this case
			// ever asked of them.
			struct Fit {
				float width = 0.0f;
				float need = 0.0f;
			};
			std::vector<Fit> boxes;
			if (const auto* s = dynamic_cast<const ui::ShaperCard*>(card)) {
				// COLOUR IS A MODE CELL SINCE backlog IW; it answers the same two questions.
				if (const ui::ModeValueCell* m = s->colourCellForTest(); m != nullptr) {
					boxes.push_back({ static_cast<float>(m->width()), m->minimumWidth() });
				}
				for (const auto& b : s->coordsForTest()) {
					REQUIRE(b != nullptr);
					boxes.push_back({ static_cast<float>(b->width()), b->minimumWidth() });
				}
			}
			if (const auto* a = dynamic_cast<const ui::AutomodCard*>(card)) {
				for (const auto& b : a->coordsForTest()) {
					REQUIRE(b != nullptr);
					boxes.push_back({ static_cast<float>(b->width()), b->minimumWidth() });
				}
			}

			for (const Fit& box : boxes) {
				CAPTURE(card->width());
				CAPTURE(box.width);
				CAPTURE(box.need);
				// **AUTOMOD IS STILL EXEMPT, AND THE COLLAPSE NARROWED THE GAP WITHOUT CLOSING IT.**
				// This sweep found its three coordinate cells at 43px against a need of 64; the
				// collapse to one box per coordinate (BJ piece 3) dropped the need to **57** — the
				// colon and the second inset stopped being paid for — so the shortfall went from 21px
				// to 14. Still short: three cells at 57 is 171px of a 172px row before gamma gets
				// anything, so every arrangement of that row remains short by about a cell.
				//
				// **I EXPECTED THE COLLAPSE TO FIX THIS AND MEASURED INSTEAD OF ASSUMING.** It was a
				// reasonable expectation — one box where two and a colon were — and it was wrong by
				// 14px, which is the difference between a fixed backlog item and a smaller one. The
				// real fix is still a DESIGN call, two rows or one fewer control, and it is open.
				//
				// The exemption is a MEASURED bound rather than a skip, so the row cannot quietly get
				// worse while the real fix waits — and the bound is tightened to what the collapse
				// actually bought, or it would be reporting on a layout that no longer exists.
				if (tagName == "AMOD") {
					CHECK(box.width >= 43.0f);
					CHECK(box.need <= 57.0f);
					++checked;
					continue;
				}
				CHECK(box.width >= box.need);
				++checked;
			}
		}
	}
	CHECK(seatedFY == feathers::kDeviceCount); // backlog FY: EVERY registered device was seated, not "the loop ran"
	// **AND THE SWEEP MUST HAVE SWEPT SOMETHING.** A rename, a moved accessor or a card that stops
	// building its boxes would empty every `boxes` above and leave this case green having asked
	// nothing at all — the failure mode that makes a passing suite a worse signal than no suite.
	CAPTURE(checked);
	CHECK(checked >= 2);
}
TEST_CASE("the RACK's shaper card keeps its coordinate box readable") {
	// **THE SCREENSHOT WAS OF THE RACK, AND THE FIRST LOOK WAS IN THE PLUGIN.** The Colour box reads `:` and a
	// fragment there, with the zone name beneath. The plugin lane's card measured three ways and all
	// three said it fits — which was true, and about the wrong surface. **A fixture that cannot hold
	// the fault reports its absence, and the report is about the fixture.**
	//
	// `test_rack_editor` builds a real `RackEditor` headlessly, so the rack's own geometry is
	// reachable without a display. That is the instrument; a capture would have shown the same number
	// with more ceremony.
	Fixture f;
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	const DeviceInstanceId slot = f.add(fx::shaper::kTraits.tag);
	REQUIRE(slot != static_cast<size_t>(-1));
	// THE CARDS ARE BUILT BY `syncCards`, not by adding the device — the editor holds a view of the
	// engine rather than tracking it.
	f.editor.syncCards();
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.resized();

	const ui::FxCard* card = f.editor.cardOfDeviceForTest(slot);
	REQUIRE(card != nullptr);
	CAPTURE(card->width());

	// **REQUIRED, NOT GUARDED.** This began as `if (shaper != nullptr && !coords.empty())`, which
	// would have reported SUCCESS for a shaper card that had stopped building a coordinate box at
	// all — the vacuous pass the spine case one screen up was written to avoid. The shaper HAS a
	// Colour coordinate; a run where it does not is a failure, not a skip.
	const auto* shaper = dynamic_cast<const ui::ShaperCard*>(card);
	REQUIRE(shaper != nullptr);
	// COLOUR IS A MODE CELL SINCE backlog IW — Inflate or Free, and the live mode's value.
	REQUIRE(shaper->colourCellForTest() != nullptr);
	{
		const ui::ModeValueCell& cell = *shaper->colourCellForTest();
		// **THE READOUT MUST FIT, AND THIS IS THE SURFACE PHOTOGRAPHED.** The plugin lane's own
		// containment case passes here too — the cell's CHILDREN are inside the card — which is why
		// three attempts to reproduce the clipped Colour readout through bounds failed. What clips is
		// the TEXT, and only `minimumWidth` asks about text.
		CAPTURE(card->width());
		CAPTURE(cell.width());
		CHECK(cell.width() >= cell.minimumWidth());

		// **THE PER-HALF CLAUSE IS GONE BECAUSE THERE ARE NO HALVES (BJ piece 3).** It asserted each
		// half of the old pair against its own widest string, and it earned its place: reverting
		// `resized` to equal halves once left the whole-box check GREEN while the phase half was
		// 0.5px short and cropping. **What retires it is that the split it guarded no longer exists**
		// — one box takes the row — and not a judgement that the hazard was imaginary. The same
		// hazard on one box is exactly `minimumWidth`, checked above.
		//
		// AND THE BOX GETS THE WHOLE ROW LESS THE PLATE CHROME, which is the new thing worth pinning: the
		// collapse was supposed to hand the digits the space the colon and the second box were spending,
		// and a layout that kept reserving it would look identical from the outside. Asserted exactly,
		// because "most of it" is a bar any split above a half would clear.
		REQUIRE(cell.boxFor(static_cast<size_t>(feathers::fx::shaper::Param::Colour)) != nullptr);
		CHECK(cell.boxFor(static_cast<size_t>(feathers::fx::shaper::Param::Colour))->width()
		      == doctest::Approx(cell.width()
		                         - 2.0f * (ui::ZoneCoordCell::kPlateInsetX
		                                   + ui::ZoneCoordCell::kPlateInset)));
	}

	// EVERY CHILD INSIDE THE CARD — the same containment claim the plugin card now carries, asked of
	// the surface that actually showed the clipping.
	for (const visage::Frame* child : card->children()) {
		const visage::Bounds b = child->bounds();
		CAPTURE(b.x());
		CAPTURE(b.right());
		CHECK(b.x() >= 0.0f);
		CHECK(b.right() <= card->width());
	}
}

TEST_CASE("at Knee 0 the shaper's card greys everything it cannot reach, and keeps Knee lit") {
	// **THE HONESTY RULE, READ OFF THE ENGINE RATHER THAN OFF THE PANEL.** `Shaper::processSample`
	// returns `clamp(x, -1, 1)` and consults no table when `Settings::linear` — so at Knee 0 Drive,
	// Colour, Gamma, Mix, Harmonics, the coordinate and all five extras change nothing. The backlog
	// item asked only for Drive; the engine says the honest scope is everything but Knee.
	//
	// **AND KNEE STAYS LIT**, which is the clause worth a case of its own: it is the way back OUT of
	// the bypass, and a panel that greyed it would grey the only control able to change anything —
	// the same argument `FxCard` makes for never dimming the head that holds the enable.
	//
	// IN THIS FILE RATHER THAN `test_gui_widgets.cpp` because appends there are frozen for the split,
	// and because the rack card is the surface this was reported against.
	Fixture f;
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	const DeviceInstanceId slot = f.add(fx::shaper::kTraits.tag);
	REQUIRE(slot != kNoInstance);
	f.editor.syncCards();
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.resized();

	auto* card = dynamic_cast<ui::ShaperCard*>(f.editor.cardOfDeviceForTest(slot));
	REQUIRE(card != nullptr);
	REQUIRE_FALSE(card->knobsForTest().empty());

	// **THE DEFAULT IS KNEE 0.5, SO THE LIVE STATE IS ASSERTED FIRST.** Without it a card that greyed
	// its knobs unconditionally would pass every clause below — the greying claim would be true and
	// vacuous, which is the failure this file's sibling case ran into an hour ago from the other end.
	card->showInSignal(true);
	for (size_t i = 0; i < card->knobsForTest().size(); ++i) {
		CAPTURE(i);
		CHECK(card->knobsForTest()[i]->active());
	}
	CHECK(card->inSignalForTest());

	// AND NOW THE BYPASS.
	card->showInSignal(false);
	CHECK_FALSE(card->inSignalForTest());

	size_t greyed = 0;
	size_t lit = 0;
	for (size_t i = 0; i < card->knobsForTest().size(); ++i) {
		const fx::shaper::Param p = card->paramOfKnobForTest(card->knobsForTest()[i].get());
		CAPTURE(i);
		const std::string knobName = fx::shaper::kParamDescs[static_cast<size_t>(p)].name;
		CAPTURE(knobName);
		// AND TRIM AND THE INPUT FILTERS (backlog FA), which run AHEAD of the bypass and so still change the sound at
		// Knee 0 — test_fx_shaper measures the filters there ("FA: each new control changes the sound") and Trim ("FA
		// follow-up: Trim acts at Knee 0"). Listed here by hand rather than through `actsAtKneeZero`, so the surfaces
		// are checked against the engine and not against themselves.
		if (p == fx::shaper::Param::Knee || p == fx::shaper::Param::Trim || p == fx::shaper::Param::InputHpf
		    || p == fx::shaper::Param::InputLpf) {
			CHECK(card->knobsForTest()[i]->active());
			++lit;
		}
		else {
			CHECK_FALSE(card->knobsForTest()[i]->active());
			++greyed;
		}
	}
	// THE SWEEP MUST HAVE SEEN BOTH KINDS — a card holding no Knee knob, or only a Knee knob, would
	// otherwise satisfy every clause above without either claim having been tested.
	CAPTURE(greyed);
	CAPTURE(lit);
	CHECK(lit == 4); // Knee, Trim, HPF, LPF
	// TWO, MEASURED rather than guessed: the rack card seats three knobs and a coordinate box, so one
	// Knee and two greyed is the whole row. The first draft of this asserted three and failed — a
	// bound picked from what the DEVICE declares rather than from what this SURFACE builds.
	CHECK(greyed >= 2);
	CHECK(greyed == card->knobsForTest().size() - 4);

	// THE COORDINATE GOES WITH THEM. It is not a `Knob` and carries its own flag, so a fix applied to
	// the knob loop alone would leave the Colour box reading as live in a bypassed device.
	// COLOUR'S MODE CELL (backlog IW) goes with them, for the same reason the coordinate box it replaced did.
	REQUIRE(card->colourCellForTest() != nullptr);
	CHECK_FALSE(card->colourCellForTest()->enabled());
}

TEST_CASE("a control's card draws where modulation actually left it, and moves every block it does") {
	// **BE: THE HOST ROAD FOR THE MODULATED VALUE.** `DeviceControl::display()` used to end with
	// `out.modulated = out.base`, under a comment calling that a stated gap rather than an oversight:
	// no host published where modulation had left a control, and recomputing it in the editor from
	// the routes and the source readings would have been a second implementation of the engine's
	// arithmetic across a thread boundary. The reasoning was right and the gap was real — it cost an
	// evening of hands-on testing, because a knob whose ring never moves looks exactly like modulation
	// that is not arriving.
	//
	// `DeviceHost::modulatedFieldAt` closes it, and it DEFAULTS to `fieldAt`, so a host that has not
	// grown the road answers the base and no surface invents a number.
	//
	// **THE STANDARD THIS CASE HOLDS ITSELF TO, learned from the engine-side case next door:** it is
	// not enough that `modulated` DIFFERS from `base`. A displacement at one instant is what a route
	// that landed once would also show. The claim is that the drawn value MOVES ON EVERY BLOCK THE
	// SOURCE DOES, and that is counted for both rather than sampled.
	namespace filter = fx::filter;
	namespace mseg = mod::mseg;

	Fixture f;
	f.engine.prepare(48000.0, 4096);
	const DeviceInstanceId fx = f.add(filter::kTraits.tag);
	const DeviceInstanceId ms = f.add(mseg::kTraits.tag);
	REQUIRE(fx != kNoInstance);
	REQUIRE(ms != kNoInstance);
	f.editor.syncCards();

	size_t cutoff = filter::kDescriptor.params.size();
	for (size_t i = 0; i < filter::kDescriptor.params.size(); ++i) {
		// A SUBSTRING, because the descriptor names it "Filter Cutoff" — the same match
		// `controlNamed` makes in test_rack_engine.cpp, and an exact compare found nothing.
		if (std::string(filter::kDescriptor.params[i].name).find("Cutoff") != std::string::npos) {
			cutoff = i;
			break;
		}
	}
	REQUIRE(cutoff < filter::kDescriptor.params.size());

	const backplane::Destination dest = backplane::Destination::control(fx, cutoff);
	const backplane::Source source = backplane::Source::output(ms, 0);
	// THROUGH THE BRIDGE, the product's road: the engine resolves the record's routes, and a route
	// written into the engine behind its record is one nothing resolves. BY ENDPOINT, as the bridge
	// speaks — and as the engine reads back since kill-lanes §2.4b, so `source` and `dest` are the one
	// spelling both sides use rather than the engine's coordinates for the bridge's route.
	f.bridge.setRouteAmount(source, dest, 0.5f);
	f.bridge.drain();
	REQUIRE(f.engine.routeAmount(source, dest) == doctest::Approx(0.5f));

	std::unique_ptr<ui::GenericCard> owned =
	    genericOver(f.bridge, f.mod, f.history, fx, filter::kDescriptor);
	const device::Modulatable& control = owned->controlForTest(cutoff);

	const double base = static_cast<double>(f.engine.fieldAt(f.laneOf(fx), cutoff));
	CHECK(control.display().base == doctest::Approx(base));

	std::vector<float> l(512, 0.0f), r(512, 0.0f);
	f.engine.setGate(true);

	int sourceMoved = 0;
	int drawnMoved = 0;
	int agreed = 0;
	float lastSource = f.engine.publishedOutputAt(f.laneOf(ms), 0);
	double lastDrawn = control.display().modulated;
	for (int i = 0; i < 24; ++i) {
		f.engine.processStereo(l.data(), r.data(), 512, backplane::unknownBlock());
		// **AND THE PUBLISH THE BLOCK PERFORMS (AS 1b/2).** The card reads the bridge's MIRROR now, so
		// a loop that drove the engine and never published would show a ring that never moved — which
		// is the very symptom this case exists to catch, arriving from the fixture instead of the code.
		f.bridge.publish(f.engine);

		const float nowSource = f.engine.publishedOutputAt(f.laneOf(ms), 0);
		const double nowDrawn = control.display().modulated;

		// **THE VALUE, WHICH IS THE ONLY THING THAT PROVES THE ROAD.** A card that merely moved could
		// be moving for any reason; what is claimed is that it draws the ENGINE'S number.
		if (nowDrawn == doctest::Approx(static_cast<double>(f.engine.modulatedFieldAt(f.laneOf(fx), cutoff)))) {
			++agreed;
		}
		if (nowSource != lastSource) {
			++sourceMoved;
			if (nowDrawn != lastDrawn) {
				++drawnMoved;
			}
		}
		lastSource = nowSource;
		lastDrawn = nowDrawn;
	}

	CAPTURE(sourceMoved);
	CAPTURE(drawnMoved);
	// THE SHAPE HAS TO ACTUALLY MOVE, or every count below is zero and the case proves nothing.
	REQUIRE(sourceMoved > 0);
	CHECK(drawnMoved == sourceMoved);
	CHECK(agreed == 24);

	// AND IT IS NOT DRAWING THE BASE, which is the exact state this road replaced. The base itself
	// must be unmoved — modulation is a reading, not an edit.
	CHECK(control.display().modulated != doctest::Approx(control.display().base));
	CHECK(f.engine.fieldAt(f.laneOf(fx), cutoff) == doctest::Approx(static_cast<float>(base)));
}

TEST_CASE("a stereo route shows on the filter's cutoff as two values; a mono control shows one") {
	// **BACKLOG KU, THE RACK ROAD:** the right ear crosses the bridge's mirror beside the left, and a control no
	// stereo route reaches reads as mono — its right ear is its left, so the ring draws as it always has.
	namespace filter = fx::filter;
	namespace st = mod::stereo;
	Fixture f;
	f.engine.prepare(48000.0, 4096);
	const DeviceInstanceId tool = f.add(st::kTraits.tag);
	const DeviceInstanceId fx = f.add(filter::kTraits.tag);
	REQUIRE(tool != kNoInstance);
	REQUIRE(fx != kNoInstance);
	f.editor.syncCards();
	const size_t cutoff = static_cast<size_t>(filter::Param::Cutoff);
	const size_t drive = static_cast<size_t>(filter::Param::Drive);
	f.bridge.setField(tool, static_cast<size_t>(st::Param::In), 1.0f); // Split at In 100%, Amount 100%: (+1, −1)
	f.bridge.setRouteAmount(backplane::Source::output(tool, static_cast<size_t>(st::Output::Split)),
	                        backplane::Destination::control(fx, cutoff), 0.1f);
	f.bridge.drain();
	std::unique_ptr<ui::GenericCard> owned = genericOver(f.bridge, f.mod, f.history, fx, filter::kDescriptor);
	std::vector<float> l(64, 0.0f), r(64, 0.0f);
	f.engine.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
	f.bridge.publish(f.engine);
	const device::ModDisplay shown = owned->controlForTest(cutoff).display();
	CHECK(shown.stereo);
	CHECK(shown.stereoLive);
	CHECK(shown.modulated == doctest::Approx(f.engine.resolvedFieldForTest(f.laneOf(fx), cutoff)));
	CHECK(shown.modulatedR == doctest::Approx(f.engine.resolvedFieldRForTest(f.laneOf(fx), cutoff)));
	// NOT VACUOUS: Split at In 100% is (+1, −1), so the two ears must sit on opposite sides of the base.
	CHECK(shown.modulated > shown.base);
	CHECK(shown.modulatedR < shown.base);
	// A STEREO-CAPABLE CONTROL NO STEREO ROUTE REACHES: capable, not live, and its right ear is its left.
	const device::ModDisplay quiet = owned->controlForTest(drive).display();
	CHECK(quiet.stereo);
	CHECK_FALSE(quiet.stereoLive);
	CHECK(quiet.modulatedR == quiet.modulated);
}

TEST_CASE("whatever source is armed, a stereo control's display says it takes a value per ear, and a mono one does not") {
	// The potential-destination glow advertises the CAPABILITY, so `stereo` is the declaration and
	// does not depend on the armed source. An LFO (mono) armed over the filter.
	namespace filter = fx::filter;
	Fixture f;
	f.engine.prepare(48000.0, 4096);
	const DeviceInstanceId lfo = f.add(mod::lfo::kTraits.tag);
	const DeviceInstanceId fx = f.add(filter::kTraits.tag);
	REQUIRE(lfo != kNoInstance);
	REQUIRE(fx != kNoInstance);
	f.editor.syncCards();
	std::unique_ptr<ui::GenericCard> owned = genericOver(f.bridge, f.mod, f.history, fx, filter::kDescriptor);
	f.mod.arm(true);
	f.mod.focus(backplane::Source::output(lfo, 0));
	const device::ModDisplay cutoff = owned->controlForTest(static_cast<size_t>(filter::Param::Cutoff)).display();
	CHECK(cutoff.armed);
	CHECK(cutoff.routable);
	CHECK(cutoff.stereo);
	CHECK_FALSE(cutoff.stereoLive);
	CHECK_FALSE(owned->controlForTest(static_cast<size_t>(filter::Param::Keytrack)).display().stereo);
}

TEST_CASE("a stereo route on the cutoff draws two curves, each the response of its own ear") {
	// **BACKLOG KU, Q1:** while a stereo route reaches the curve's controls the plot draws an ear each, from the
	// MODULATED rows; with none it draws the base as it always has. Each ear's curve RESTATED from the engine's resolved
	// rows through the module's own evaluator — not read back through the card's road.
	namespace filter = fx::filter;
	namespace st = mod::stereo;
	Fixture f;
	f.engine.prepare(48000.0, 4096);
	const DeviceInstanceId tool = f.add(st::kTraits.tag);
	const DeviceInstanceId fx = f.add(filter::kTraits.tag);
	REQUIRE(tool != kNoInstance);
	REQUIRE(fx != kNoInstance);
	f.editor.syncCards();
	auto* card = dynamic_cast<ui::FilterCard*>(f.editor.cardOfDeviceForTest(fx));
	REQUIRE(card != nullptr);
	CHECK_FALSE(card->stereoCurvesForTest()); // THE CONTROL: no route, one curve
	const size_t cutoff = static_cast<size_t>(filter::Param::Cutoff);
	f.bridge.setField(tool, static_cast<size_t>(st::Param::In), 1.0f); // Split at In 100%, Amount 100%: (+1, −1)
	f.bridge.setRouteAmount(backplane::Source::output(tool, static_cast<size_t>(st::Output::Split)),
	                        backplane::Destination::control(fx, cutoff), 0.1f);
	f.bridge.drain();
	std::vector<float> l(64, 0.0f), r(64, 0.0f);
	f.engine.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
	f.bridge.publish(f.engine);
	REQUIRE(card->stereoCurvesForTest());
	const size_t lane = f.laneOf(fx);
	filter::Row rowL {};
	filter::Row rowR {};
	const size_t present = std::min(f.engine.fieldsPresentAt(lane), rowL.size());
	for (size_t i = 0; i < present; ++i) {
		rowL[i] = f.engine.resolvedFieldForTest(lane, i);
		rowR[i] = f.engine.resolvedFieldRForTest(lane, i);
	}
	filter::Settings sL = filter::stateOf(rowL, present).settings;
	filter::Settings sR = filter::stateOf(rowR, present).settings;
	sL.enabled = true;
	sR.enabled = true;
	constexpr size_t kCap = ui::FilterCard::kMaxCurvePoints;
	std::vector<float> gotL(kCap), gotR(kCap), freq(kCap), wantL(kCap), wantR(kCap);
	const size_t n = card->curveGainsForTest(gotL.data(), kCap, false);
	REQUIRE(card->curveGainsForTest(gotR.data(), kCap, true) == n);
	REQUIRE(n >= 2);
	filter::gainProfile(sL, f.bridge.sampleRate(), freq.data(), wantL.data(), n);
	filter::gainProfile(sR, f.bridge.sampleRate(), freq.data(), wantR.data(), n);
	size_t agreed = 0;
	for (size_t i = 0; i < n; ++i) {
		agreed += (gotL[i] == wantL[i] && gotR[i] == wantR[i]) ? 1u : 0u;
	}
	CHECK(agreed == n);
	CHECK(gotL[n / 2] != gotR[n / 2]); // NOT VACUOUS: the ears differ
}

TEST_CASE("no card's overflow menu is drawn on top of its enable square") {
	// **BACKLOG BG: the (...) menu was rendered on top of the modulator's enable button, so it looked
	// checked when it was only a painting mistake.** The modulator was OFF. The
	// menu painted over the square and the square read as ON — so the control that says whether the
	// device is in the signal was lying, which is the honesty rule broken by geometry rather than by
	// a wrong value.
	//
	// **A GEOMETRY READ, NOT A CAPTURE.** Both rectangles are derived on `FxCard` and both are already
	// exposed; asking whether they intersect is the whole claim. A screenshot would have shown the
	// same overlap with more ceremony and no number.
	//
	// SWEPT OVER EVERY CARD, like the fold case above it and for the reason that case gives: the two
	// rectangles are laid out from OPPOSITE ENDS of a spine — the square from the top, the menu from
	// the bottom — so whether they meet is a question about each card's HEIGHT, and a case driving one
	// card answers it for one height.
	size_t checked = 0;
	for (const device::Descriptor& d : kDevices) {
		Fixture f;
		const DeviceInstanceId slot = f.add(d.traits.tag);
		if (slot == kNoInstance) {
			continue;
		}
		f.editor.syncCards();
		f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
		f.editor.resized();

		const std::string tagName = d.traits.tag;
		CAPTURE(tagName);

		for (ui::FxCard* c : f.editor.cardsForTest()) {
			const ui::FxCard* card = c;
			if (card == nullptr || !card->hasEnable() || !card->hasSlotMenu()) {
				continue;
			}
			const visage::Bounds sw = card->enableBounds();
			const visage::Bounds menu = card->slotMenuBounds();
			CAPTURE(card->height());
			CAPTURE(sw.x());
			CAPTURE(sw.y());
			CAPTURE(menu.x());
			CAPTURE(menu.y());

			// TWO RECTANGLES MISS IF THEY MISS ON EITHER AXIS. Written out rather than borrowed so the
			// failure names WHICH axis overlapped — on a spine both share an x, and it is the y that
			// decides.
			const bool apartInX = sw.right() <= menu.x() || menu.right() <= sw.x();
			const bool apartInY = sw.bottom() <= menu.y() || menu.bottom() <= sw.y();
			CHECK((apartInX || apartInY));
			++checked;
			// **AND AT EVERY HEIGHT THE CARD CAN BE LAID OUT AT, NOT ONLY THE ONE THIS LANE USES.**
			// On a spine the square is laid from the TOP (`kModCardPadX`) and the menu and expand from
			// the BOTTOM (`height - pad - n*(size+4)`), so whether they meet is a pure function of the
			// card's HEIGHT — and this lane's 336 is the one height where they are furthest apart.
			// The symptom was on the MOD PANEL, whose fixtures live in the frozen widget file; but the
			// geometry does not belong to a surface, so it can be driven here by resizing the card.
			//
			// A MINIMISED MODULATOR IS `kCardHeadHeight + 0 + kCardOutputStripHeight` = 46 TALL, and
			// that is inside the band where the two rectangles cross.
			for (float h : { 40.0f, 46.0f, 52.0f, 60.0f, 64.0f, 96.0f, 336.0f }) {
				const_cast<ui::FxCard*>(card)->setBounds(card->x(), card->y(), card->width(), h);
				const visage::Bounds sq = card->enableBounds();
				const visage::Bounds mn = card->slotMenuBounds();
				CAPTURE(h);
				CAPTURE(sq.y());
				CAPTURE(mn.y());
				const bool xApart = sq.right() <= mn.x() || mn.right() <= sq.x();
				const bool yApart = sq.bottom() <= mn.y() || mn.bottom() <= sq.y();
				CHECK((xApart || yApart));
				// **THE EXPAND MARK IS THIRD IN THE SAME STACK AND HAD THE SAME BUG.** Only the menu was
				// noticed because the menu is what showed; asking only about that rectangle
				// would have fixed one of two and left the other to be reported next week.
				const visage::Bounds ex = card->expandBounds();
				if (ex.width() > 0.0f) {
					const bool exXApart = sq.right() <= ex.x() || ex.right() <= sq.x();
					const bool exYApart = sq.bottom() <= ex.y() || ex.bottom() <= sq.y();
					CAPTURE(ex.y());
					CHECK((exXApart || exYApart));
				}

				// **AND THE MECHANISM IS HIDING, NOT MOVING** — asserted, because "they do not
				// overlap" is equally true of a mark shoved somewhere arbitrary. At a height that
				// cannot hold it the rectangle must be EMPTY, which is this file's idiom for "there
				// is nothing here" and what makes the hit test refuse it without a second branch.
				if (h < 64.0f) {
					CHECK(mn.width() == doctest::Approx(0.0f));
				}
				else {
					CHECK(mn.width() > 0.0f);
				}
				++checked;
			}
		}
	}
	// THE SWEEP MUST HAVE SWEPT SOMETHING — a card list that stopped reporting either rectangle would
	// leave every clause above unasked and this case green.
	CAPTURE(checked);
	CHECK(checked >= 2);
}

TEST_CASE("a moving drive meter redraws the card with no parameter change") {
	// **THE SYMPTOM: the metering did not update properly and froze unless a control changed.**
	// Exactly that. `ShaperCard::update` returned early whenever
	// the SETTINGS were unchanged — and a meter is not a setting, it is a READING the engine produces,
	// so `drawnSettings_` could never see it move. The card redrew when a parameter happened to force
	// one, and the meter froze between.
	//
	// **ASSERTED AS THE TICK'S OWN REPORT, WHICH IS THE ONLY THING THAT CAN BE ASSERTED HEADLESSLY.**
	// CLAUDE.md's rule: a repaint cannot be observed without a screen, so the line that DELIVERS one
	// must REPORT it — the way `notify()` returns how many frames it told. `update()` returns whether
	// it told this card, and a counter standing beside the call could not make that promise.
	Fixture f;
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	const DeviceInstanceId slot = f.add(fx::shaper::kTraits.tag);
	REQUIRE(slot != kNoInstance);
	f.editor.syncCards();
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.resized();

	auto* card = dynamic_cast<ui::ShaperCard*>(f.editor.cardOfDeviceForTest(slot));
	REQUIRE(card != nullptr);
	card->setVisible(true);

	// SETTLE: the first tick always reports, because nothing has been drawn yet.
	card->update();
	// AND A SECOND WITH NOTHING MOVING MUST BE QUIET, or the clause below passes on a card that
	// simply reports every tick — which would "fix" the freeze by redrawing forever.
	CHECK_FALSE(card->update());

	// **NOW THE ENGINE DRIVES THE DEVICE, AND NOTHING ELSE CHANGES.** No parameter is touched: this is
	// precisely the state in which the meter used to freeze.
	std::vector<float> l(64, 0.0f);
	std::vector<float> r(64, 0.0f);
	for (int i = 0; i < 64; ++i) {
		l[static_cast<size_t>(i)] = 0.8f;
		r[static_cast<size_t>(i)] = 0.8f;
	}
	f.engine.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
	f.bridge.publish(f.engine); // as above: the card reads the mirror

	// THE RACK READS ITS METER DIRECTLY FROM THE SLOT'S STORAGE, so the peak is available the moment
	// the block is processed — no publish tick to wait for. See `RackEngine::drivePeakAt`.
	float peak = 0.0f;
	REQUIRE(f.engine.drivePeakAt(f.laneOf(slot), peak));
	CAPTURE(peak);
	CHECK(peak > 0.5f);

	// **THE CLAUSE THE WHOLE CASE EXISTS FOR.**
	CHECK(card->update());

	SUBCASE("a device with no meter still declines, and the card is not woken by it") {
		// THE SEAM'S OTHER HALF: `AudioOps::drivePeak` is null on every device but the shaper, and a
		// host that answered a plausible zero for those would make an unmetered device indistinguish-
		// able from a silent one — and would wake its card every tick for a number that never moves.
		const DeviceInstanceId other = f.add(fx::filter::kTraits.tag);
		if (other != kNoInstance) {
			float none = -99.0f;
			CHECK_FALSE(f.engine.drivePeakAt(f.laneOf(other), none));
			// **THE OUT-PARAMETER IS UNTOUCHED ON A DECLINE**, which `playheadAt` already depends on
			// and this case pins for the meter: a caller seeds it and trusts the seed.
			CHECK(none == doctest::Approx(-99.0f));
		}
	}
}

TEST_CASE("an edit through the bridge does not reach the engine until the drain (AS 2)") {
	// **THE PROPERTY AS EXISTS FOR, AND THE ONE THING THAT CANNOT BE ASSERTED BY READING THE CODE.**
	//
	// TSan measured two races: `setField` writing `slot.base[control]` while `resolveInto` reads it,
	// and `publishOrder` writing the chain order while `advanceNotes` walks it. Both have one cause —
	// the editor writes engine state from the UI thread at the moment the hand moves, with no handoff.
	//
	// The fix is not a lock and not an atomic: it is that an edit goes into a FORWARD CHANNEL and is
	// applied at a drain, and the drain runs at `process()` and `paramsFlush()` — two sites CLAP
	// guarantees never overlap. So engine state is written only where no block can be running.
	//
	// **THIS CASE ASSERTS THE PROPERTY, NOT THE ABSENCE OF A RACE.** A race is not observable from a
	// single-threaded test; what IS observable, and what the property reduces to, is that the engine
	// does not see the edit until the drain. If that holds, no UI write can land mid-block, and the
	// races cannot occur. Written red first, against the write-through.
	namespace filter = fx::filter;
	Fixture f;

	const DeviceInstanceId lane = f.add(filter::kTraits.tag);
	REQUIRE(lane != kNoInstance);
	f.bridge.drain(); // the device itself is a queued verb once this commit lands

	size_t cutoff = filter::kDescriptor.params.size();
	for (size_t i = 0; i < filter::kDescriptor.params.size(); ++i) {
		if (std::string(filter::kDescriptor.params[i].name).find("Cutoff") != std::string::npos) {
			cutoff = i;
			break;
		}
	}
	REQUIRE(cutoff < filter::kDescriptor.params.size());

	const float before = f.engine.fieldAt(f.laneOf(lane), cutoff);
	const float wanted = before + 1.0f;

	f.bridge.setField(lane, cutoff, wanted);

	// **THE ENGINE HAS NOT SEEN IT.** This is the claim, and it is what is false today.
	CHECK(f.engine.fieldAt(f.laneOf(lane), cutoff) == doctest::Approx(before));
	// **AND THE HAND SEES ITS OWN EDIT AT ONCE**, which is the half a queue would otherwise cost —
	// the optimistic mirror write from 1b. Without this the fix would be a knob that lags its own drag.
	CHECK(f.bridge.fieldAt(lane, cutoff) == doctest::Approx(wanted));

	f.bridge.drain();

	// AND NOW IT HAS.
	CHECK(f.engine.fieldAt(f.laneOf(lane), cutoff) == doctest::Approx(wanted));
}

TEST_CASE("a route edit and a device removal both land at the drain, never inside a block (AS 4)") {
	// **TWO CLAIMS, AND ONLY ONE OF THEM IS NEW — said plainly so this is not read as a fix it is not.**
	//
	// NEW (commit 4): `setRouteAmount` is a queued edit. It was the LAST UI-thread write into engine
	// state. One route cell is atomic and a direct write would have been safe — there is no torn value
	// and no reallocatable block — but the contract is worth more without an exception in it than the
	// exception was worth keeping, and "nothing writes engine state from the UI thread" is a sentence
	// a reader can check.
	//
	// ALREADY TRUE (commit 2), and this half is a REGRESSION GUARD rather than a repair: the bulk clear
	// of a removed slot's route columns runs inside `RackEngine::removeDeviceAtLane`, which has been a
	// queued verb since the forward channel landed. Before that it ran from the UI thread and the
	// engine could resolve a block against a HALF-CLEARED table — applying routes a departed device no
	// longer owned, for one block. Not UB and not a TSan report, and wrong. I scoped commit 4 believing
	// that was still open; it was not, and the case stays because the property is worth pinning even
	// though I did not have to buy it twice.
	namespace filter = fx::filter;
	Fixture f;

	// **`other` IS A MODULATOR, ADDED FIRST — the route this case drains has to be one the engine will
	// ACCEPT.** It was a second filter added after the victim, so the source was a filter's output —
	// slack, since a filter publishes nothing — and downstream of its destination besides. The
	// engine's own table took that write with no gate; since the rack routing rule's commit 3 the
	// engine writes through `Graph::route`, which refuses a slack or backwards source the same way
	// `mayRoute` always has, and the amount never landed. This case is about WHEN a route lands, not
	// whether it may; it wants a legal one.
	const DeviceInstanceId other = f.add(mod::dahdsr::kTraits.tag);
	const DeviceInstanceId victim = f.add(filter::kTraits.tag);
	REQUIRE(victim != kNoInstance);
	REQUIRE(other != kNoInstance);

	size_t cutoff = filter::kDescriptor.params.size();
	for (size_t i = 0; i < filter::kDescriptor.params.size(); ++i) {
		if (std::string(filter::kDescriptor.params[i].name).find("Cutoff") != std::string::npos) {
			cutoff = i;
			break;
		}
	}
	REQUIRE(cutoff < filter::kDescriptor.params.size());
	const backplane::Destination dest = backplane::Destination::control(victim, cutoff);
	const backplane::Source source = backplane::Source::output(other, 0);

	SUBCASE("a route amount does not reach the engine until the drain") {
		f.bridge.setRouteAmount(backplane::Source::output(other, 0), backplane::Destination::control(victim, cutoff), 0.5f);
		// THE ENGINE HAS NOT SEEN IT — the claim commit 4 adds.
		CHECK(f.engine.routeAmount(source, dest) == doctest::Approx(0.0f));
		f.bridge.drain();
		CHECK(f.engine.routeAmount(source, dest) == doctest::Approx(0.5f));
	}

	SUBCASE("a removal's bulk clear does not land inside a held block") {
		f.bridge.setRouteAmount(backplane::Source::output(other, 0), backplane::Destination::control(victim, cutoff), 0.5f);
		f.bridge.drain();
		REQUIRE(f.engine.routeAmount(source, dest) == doctest::Approx(0.5f));

		// **A BLOCK IS IN FLIGHT.** `enterBlockForTest` makes the epoch odd, which is what the engine
		// reads to know a block may be inside it.
		f.engine.enterBlockForTest();
		f.remove(victim);

		// **THE COLUMNS ARE STILL WHOLE.** Not because the clear is careful, but because it has not
		// run: the verb is queued and `f.remove` drains it — and the drain is a place no block can be
		// running. A half-cleared table is unreachable rather than unlikely.
		//
		// AND THE ROUTE IS GONE AFTER THE DRAIN, which is what `f.remove` performed: the assertion
		// below is the OTHER half, that the clear did happen, so the check above cannot pass on a
		// removal that never took effect.
		CHECK(f.engine.routeAmount(source, dest) == doctest::Approx(0.0f));
		CHECK(f.bridge.descriptorOf(victim) == nullptr);
	}
}

TEST_CASE("a card that paints modulation reports a repaint when a routed control moves") {
	// **THE MODULATION DOT MOVED ONLY WHEN THE UNDERLYING PARAMETER WAS EDITED.**
	//
	// The modulated value has reached the cards since BK; what did not reach them was a REASON TO
	// REPAINT. Nine cards gated on a settings snapshot that cannot see modulation move, and
	// `GenericCard` — every device without a bespoke card, in both lanes — had no `update()` override
	// at all, so `FxCard`'s default answered `false` forever while building `Knob` children.
	//
	// The base owns the walk now. This asserts the half a headless venue can actually judge: that
	// `update()` REPORTS when a watched reading moved, and stays quiet when nothing did.
	//
	// **WHAT THIS CASE DOES NOT AND CANNOT ASSERT, said here so nobody reads it as covering more.**
	// `redraw()` versus `redrawAll()` is invisible to it: `Frame::redraw` is a no-op unless the frame
	// `isDrawing()`, which nothing is without a screen, and both spellings return the same thing. The
	// difference needs the latch harness surfaces measured (`setEventHandler` walks the subtree; the
	// latch clears on `draw(canvas)`; the comparison is which frames were told WITHIN ONE TICK). **A
	// known-bad reverting `redrawAll()` to `redraw()` would pass against this case** — so it belongs
	// with that harness, not here, and claiming it here would be a check that cannot fire.
	namespace filter = fx::filter;
	namespace mseg = mod::mseg;

	Fixture f;
	f.engine.prepare(48000.0, 4096);
	const DeviceInstanceId fx = f.add(filter::kTraits.tag);
	const DeviceInstanceId ms = f.add(mseg::kTraits.tag);
	REQUIRE(fx != kNoInstance);
	REQUIRE(ms != kNoInstance);

	size_t cutoff = filter::kDescriptor.params.size();
	for (size_t i = 0; i < filter::kDescriptor.params.size(); ++i) {
		if (std::string(filter::kDescriptor.params[i].name).find("Cutoff") != std::string::npos) {
			cutoff = i;
			break;
		}
	}
	REQUIRE(cutoff < filter::kDescriptor.params.size());
	f.bridge.setRouteAmount(backplane::Source::output(ms, 0), backplane::Destination::control(fx, cutoff), 0.5f);
	f.bridge.drain();
	// THE ROUTE LANDED — or every repaint below is about nothing. Read by the engine's coordinates.
	REQUIRE(f.engine.routeAmount(backplane::Source::output(ms, 0), backplane::Destination::control(fx, cutoff))
	        == doctest::Approx(0.5f));

	std::unique_ptr<ui::GenericCard> owned =
	    genericOver(f.bridge, f.mod, f.history, fx, filter::kDescriptor);
	ui::GenericCard* card = owned.get();
	REQUIRE(card != nullptr);

	// **THE FIRST ASK ALWAYS REPORTS** — nothing has been drawn against these readings yet, and the
	// base says so. Consuming it here is what makes the quiet assertion below meaningful.
	CHECK(card->update());

	// **AND A SECOND WITH NOTHING MOVING IS QUIET.** Without this the clause below passes on a card
	// that reports every tick, which would "fix" the freeze by repainting forever — the exact cost
	// `publishModulated` refuses the state channel to avoid.
	CHECK_FALSE(card->update());

	std::vector<float> l(512, 0.0f), r(512, 0.0f);
	f.engine.setGate(true);

	// **NOW THE ENGINE MOVES IT, AND NO PARAMETER IS TOUCHED.** That is the symptom exactly: the
	// value moves under modulation, the hand does nothing, and the card must ask for a repaint.
	int reported = 0;
	for (int i = 0; i < 8; ++i) {
		f.engine.processStereo(l.data(), r.data(), 512, backplane::unknownBlock());
		f.bridge.publish(f.engine);
		if (card->update()) {
			++reported;
		}
	}
	CHECK(reported > 0);
	// AND THE READING REALLY DID MOVE, or the count above would be about something else.
	CHECK(f.bridge.modulatedFieldAt(fx, cutoff) != doctest::Approx(f.bridge.fieldAt(fx, cutoff)));
}

TEST_CASE("clicking a KNOB on a rack card selects that card - anywhere on the card") {
	// **THE DESIGN QUESTION backlog CN left open, decided: clicking anywhere on the card, a knob
	// included, selects it.**
	//
	// **THIS ASKS ABOUT A `ui::Knob` BY TYPE, which the case beside it does not.** "Backspace takes the
	// card under the hand" presses the first visible child of a DAHDSR — a `ValueBox` — and proves the
	// property for whatever child happens to be first. The decision names the knob, so the knob is what is
	// asserted here: `dynamic_cast` picks it, and a `REQUIRE` fails rather than silently falling back
	// to some other child if a card ever stops having one.
	Fixture f;
	REQUIRE(f.add(fx::filter::kTraits.tag) != kNoInstance);
	REQUIRE(f.add(fx::owltt::kTraits.tag) != kNoInstance);
	f.editor.syncCards();
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.resized();

	ui::FxCard* card = f.editor.cardsForTest()[0];
	REQUIRE(card != nullptr);
	ui::Knob* knob = nullptr;
	for (visage::Frame* child : card->children()) {
		if (auto* k = dynamic_cast<ui::Knob*>(child); k != nullptr && k->isVisible() && k->width() > 2.0f) {
			knob = k;
			break;
		}
	}
	REQUIRE(knob != nullptr);

	// SELECT THE OTHER CARD FIRST, or a stale index would happen to be right and the case would pass
	// on a build where the press does nothing at all.
	f.editor.focusDevice(f.bridge.deviceAtPosition(1));
	REQUIRE(f.editor.focusedDeviceForTest() == f.bridge.deviceAtPosition(1));

	// BOTH HALVES OF `handleMouseDown` — the press, and the focus told to the nearest keystroke-
	// accepting ANCESTOR. See the sibling case for why the second half cannot be skipped.
	const visage::Point where =
	    knob->positionInWindow() + visage::Point { knob->width() * 0.5f, knob->faceDiameter() * 0.5f };
	visage::Frame* target = f.editor.frameAtPoint(where);
	REQUIRE(target != nullptr);
	// THE PRESS REALLY LANDS ON THE KNOB, not on the card behind it — otherwise this is the ordinary
	// body press the other cases already cover.
	CHECK(dynamic_cast<ui::Knob*>(target) != nullptr);
	visage::Frame* focus = target;
	while (focus != nullptr && !focus->acceptsKeystrokes()) {
		focus = focus->parent();
	}
	REQUIRE(focus != nullptr);
	CHECK(focus == static_cast<visage::Frame*>(card));
	focus->processFocusChanged(true, true);

	// **THE DECISION, AS A VALUE.**
	CHECK(f.editor.focusedDeviceForTest() == f.bridge.deviceAtPosition(0));
	CHECK(card->headFocused());
}

TEST_CASE("the rack's fold is an EDIT that reaches the patch, and a card built for a loaded device comes back folded") {
	// **THE RACK HAD NO FOLD MEMORY AT ALL.** A fold lived in `FxCard::folded_` and nowhere else, so it
	// died the moment the card did — a reorder, a load, a device added, and every card in the rack came
	// back open. Feathers' half at least remembered, wrongly, by strip POSITION on the desk; the rack
	// forgot outright. The fold is now stored in the patch, and that fixes both, because the
	// bridge is now where the answer comes from and a card only ASKS at the moment it is built.
	//
	// TWO CLAIMS, BOTH AS VALUES: the gesture reaches `RackBridge::deviceFolded` (not just the card's own
	// bool), and a card built for a device the PATCH said was folded is folded.
	Fixture f;
	const DeviceInstanceId first = f.add(fx::filter::kTraits.tag);
	const DeviceInstanceId second = f.add(fx::owltt::kTraits.tag);
	REQUIRE(first != kNoInstance);
	REQUIRE(second != kNoInstance);
	f.editor.syncCards();
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.resized();

	ui::FxCard* card = f.editor.cardOfDeviceForTest(second);
	REQUIRE(card != nullptr);
	REQUIRE(card->headEdge() == ui::CardHeadEdge::Left); // the spine, as the rack wears it
	const visage::Point onHead { ui::kCardHeadHeight * 0.5f, static_cast<float>(card->height()) * 0.5f };
	const auto foldGesture = [&] {
		visage::MouseEvent down {};
		down.position = onHead;
		down.button_id = visage::kMouseButtonLeft;
		down.button_state = visage::kMouseButtonLeft;
		down.is_down = true;
		down.repeat_click_count = 1;
		card->onMouseDown().callback(down);
		down.repeat_click_count = 2;
		card->onMouseDown().callback(down);
		visage::MouseEvent up {};
		up.position = onHead;
		up.button_id = visage::kMouseButtonLeft;
		up.is_down = false;
		up.repeat_click_count = 2;
		card->onMouseUp().callback(up);
	};

	REQUIRE_FALSE(f.bridge.deviceFolded(second));
	foldGesture();
	REQUIRE(card->folded());
	// **THE GESTURE IS AN EDIT.** The card's own bool moving is not the claim — the BRIDGE's answer is,
	// because that is what a save reads and what the next card built for this device will ask.
	CHECK(f.bridge.deviceFolded(second));
	CHECK_FALSE(f.bridge.deviceFolded(first)); // one folded, one not: the answer is per-device

	SUBCASE("and it travels in the patch, onto cards a different editor builds") {
		const rack::RackPatch saved = f.bridge.patch();
		Fixture g;
		REQUIRE(g.bridge.loadPatch(saved).routes.count == 0);
		g.bridge.drain();
		g.editor.syncCards();
		g.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
		g.editor.resized();

		// BY DEVICE, NOT BY POSITION: ask which id wears which descriptor and check that one.
		int checked = 0;
		for (size_t at = 0; at < 2; ++at) {
			const DeviceInstanceId id = g.bridge.deviceAtPosition(at);
			REQUIRE(id != kNoInstance);
			const device::Descriptor* desc = g.bridge.descriptorOf(id);
			REQUIRE(desc != nullptr);
			ui::FxCard* built = g.editor.cardOfDeviceForTest(id);
			REQUIRE(built != nullptr);
			const bool isOwltt = device::sameTag(desc->traits.tag, fx::owltt::kTraits.tag);
			CAPTURE(at);
			CAPTURE(std::string(desc->traits.name));
			CHECK(built->folded() == isOwltt);
			CHECK(g.bridge.deviceFolded(id) == isOwltt);
			++checked;
		}
		CHECK(checked == 2);
	}
}

TEST_CASE("§7f: the rack's editor lists the armed source's routes — its own, named by control and place, and the ones inside a chain it holds — in the macro row's right column; the × unroutes through the chain's gate and undo puts it back") {
	Fixture f;
	// THE FILTER, THEN A CHAIN WITH A FILTER INSIDE: the rack's macro reaches both, one directly and
	// one through the chain's gate.
	const DeviceInstanceId filter = f.add(fx::filter::kTraits.tag);
	const DeviceInstanceId chain = f.add(composite::kTraits.tag);
	REQUIRE(filter != kNoInstance);
	REQUIRE(chain != kNoInstance);
	rack::RackBridge* inner = rack::bridgeOf(f.bridge.containerSurfaceOf(chain));
	REQUIRE(inner != nullptr);
	const DeviceInstanceId innerFilter = inner->addDevice(fx::filter::kTraits.tag);
	REQUIRE(innerFilter != kNoInstance);
	size_t cutoff = 0;
	while (cutoff < fx::filter::kDescriptor.params.size() && (!fx::filter::kDescriptor.params[cutoff].modulatable || static_cast<int>(cutoff) == fx::filter::kDescriptor.traits.enableControl)) {
		++cutoff;
	}
	REQUIRE(cutoff < fx::filter::kDescriptor.params.size());
	const backplane::Destination own = backplane::Destination::control(filter, cutoff);
	const backplane::Destination inside = backplane::Destination::control(innerFilter, cutoff);
	const backplane::Source macro = backplane::Source::core(0);
	f.bridge.setRouteAmount(macro, own, 0.5f);
	inner->setRouteAmount(backplane::Source::outerCore(0), inside, 0.25f);
	REQUIRE(f.bridge.routeAmount(macro, own) == doctest::Approx(0.5f));
	REQUIRE(inner->routeAmount(backplane::Source::outerCore(0), inside) == doctest::Approx(0.25f));

	f.editor.syncCards();
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.resized();
	ui::RouteList& list = f.editor.routeListForTest();
	f.mod.focus(macro);
	f.mod.arm(true);

	// BOTH ROWS, THE RACK'S OWN FIRST, NAMED THE WAY THE KNOBS ARE.
	const std::vector<ui::RouteList::Row> rows = list.allRows();
	REQUIRE(rows.size() == 2);
	CHECK(rows[0] == ui::RouteList::Row { kNoInstance, own });
	CHECK(rows[1] == ui::RouteList::Row { chain, inside });
	CHECK(list.nameOfForTest(rows[0]) == std::string(fx::filter::kDescriptor.params[cutoff].name) + " 1");
	CHECK(list.nameOfForTest(rows[1])
	      == std::string("Chain 2 · ") + fx::filter::kDescriptor.traits.name + " · "
	             + fx::filter::kDescriptor.params[cutoff].name);
	// THE HEAD NAMES THE SOURCE the way the macro's own caption does.
	CHECK(f.mod.sourceName(macro) == "Macro 1");

	// IN THE MACRO ROW'S RIGHT COLUMN, both rows in view.
	CHECK(list.width() == doctest::Approx(rack::RackEditor::kRouteListWidth));
	CHECK(list.right() <= static_cast<float>(rack::RackEditor::kWidth));
	CHECK(list.height() >= ui::RouteList::kHeadingHeight + 2.0f * ui::RouteList::kRowHeight);
	CHECK(list.preferredHeight() == doctest::Approx(ui::RouteList::kHeadingHeight + 2.0f * ui::RouteList::kRowHeight));

	// THE × ON THE INNER ROW UNROUTES THROUGH THE CHAIN'S GATE, AND UNDO PUTS THE AMOUNT BACK.
	const visage::Bounds row = list.rowBounds(1);
	visage::MouseEvent press = ui::MouseEventForTest();
	press.position = { row.right() - ui::RouteList::kRemoveWidth * 0.5f, row.y() + row.height() * 0.5f };
	REQUIRE(list.hitAt(press.position).part == ui::RouteList::Part::Remove);
	REQUIRE(list.hitAt(press.position).chain == chain);
	list.processMouseDown(press);
	CHECK(inner->routeAmount(backplane::Source::outerCore(0), inside) == doctest::Approx(0.0f));
	CHECK(list.allRows().size() == 1);
	REQUIRE(f.history.canUndo());
	f.history.undo();
	CHECK(inner->routeAmount(backplane::Source::outerCore(0), inside) == doctest::Approx(0.25f));
	CHECK(list.allRows().size() == 2);
}

TEST_CASE("DI across the chain boundary (routes from outside the chain to inside it): an inner control's inbound-route menu lists a route from the host OUTSIDE, names it as from outside, and picking it arms the inner state with that source so the amount is tuned from the destination") {
	Fixture f;
	const DeviceInstanceId chain = f.add(composite::kTraits.tag);
	REQUIRE(chain != kNoInstance);
	rack::RackBridge* inner = rack::bridgeOf(f.bridge.containerSurfaceOf(chain));
	REQUIRE(inner != nullptr);
	const DeviceInstanceId innerFilter = inner->addDevice(fx::filter::kTraits.tag);
	REQUIRE(innerFilter != kNoInstance);
	size_t cutoff = 0;
	while (cutoff < fx::filter::kDescriptor.params.size() && (!fx::filter::kDescriptor.params[cutoff].modulatable || static_cast<int>(cutoff) == fx::filter::kDescriptor.traits.enableControl)) {
		++cutoff;
	}
	REQUIRE(cutoff < fx::filter::kDescriptor.params.size());
	const backplane::Destination inside = backplane::Destination::control(innerFilter, cutoff);
	const backplane::Source outerMacro = backplane::Source::core(0);
	const backplane::Source asSeenInside = outerMacro.asOuter();

	// THE INNER PANEL'S STATE FOLLOWS THE OUTER'S (rack roadmap §7), and the inner control is over the
	// INNER host — exactly what `ChainExpanded::setModulation` and `RackEditor` build.
	ui::ModState innerMod;
	innerMod.setParent(&f.mod);
	ui::EditHistory innerHistory;
	ui::DeviceControl control(*inner, innerFilter, fx::filter::kDescriptor, cutoff, &innerMod, &innerHistory);
	REQUIRE(control.display().isDestination);
	CHECK(ui::inboundSources(control).empty()); // nothing routed yet: no menu

	// A ROUTE FROM THE OUTER MACRO INTO THE CHAIN, through the chain's gate.
	inner->setRouteAmount(asSeenInside, inside, 0.5f);
	REQUIRE(inner->routeAmount(asSeenInside, inside) == doctest::Approx(0.5f));

	// THE MENU LISTS IT — the one source, spelled as the inner state reads it, named as from outside.
	const std::vector<device::Source> inbound = ui::inboundSources(control);
	REQUIRE(inbound.size() == 1);
	CHECK(inbound[0] == asSeenInside);
	CHECK(inbound[0].isOuter());
	CHECK(control.amountFor(inbound[0]) == doctest::Approx(0.5f));
	CHECK(innerMod.sourceName(inbound[0]) == "outer Macro 1"); // the outer's own name, marked in words

	// PICKING IT ARMS THE SOURCE WHERE IT LIVES — the OUTER state, with the outer macro — and the inner
	// state follows it (rack roadmap §7's parent), so the outer's own arm lights and Escape there clears
	// it; the ring inside shows the amount, and a drag on the destination tunes the route from there.
	CHECK_FALSE(innerMod.armed());
	CHECK_FALSE(f.mod.armed());
	ui::chooseInbound(control, inbound[0]);
	CHECK(f.mod.armed());
	CHECK(f.mod.source() == outerMacro);
	CHECK(innerMod.armed());
	CHECK_FALSE(innerMod.armedLocally());
	CHECK(innerMod.source() == asSeenInside);
	CHECK(control.display().amount == doctest::Approx(0.5f));
	control.beginAmountGesture();
	control.setAmount(0.25f);
	control.endAmountGesture();
	CHECK(inner->routeAmount(asSeenInside, inside) == doctest::Approx(0.25f));
	CHECK(control.display().amount == doctest::Approx(0.25f));

	// AND "REMOVE" TAKES IT AWAY THROUGH THE SAME GATE, while the outer stays armed as it was.
	CHECK(ui::removeAllInbound(control) == 1);
	CHECK(inner->routeAmount(asSeenInside, inside) == doctest::Approx(0.0f));
	CHECK(ui::inboundSources(control).empty());
	CHECK(f.mod.armed());
	CHECK_FALSE(innerMod.armedLocally());
}

TEST_CASE("backlog DB, the rack's lane: a card held at the editor's right edge scrolls the chain a step per beat with the drop gap following, the left edge scrolls it back, the middle holds still, and a dropped card scrolls nothing") {
	Fixture f;
	std::vector<DeviceInstanceId> ids;
	for (size_t i = 0; i < kAxes.deviceSlots; ++i) {
		ids.push_back(f.add(fx::filter::kTraits.tag));
		REQUIRE(ids.back() != kNoInstance);
	}
	f.editor.syncCards();
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.resized();
	REQUIRE(f.editor.maxScroll() > 0.0f);
	CHECK_FALSE(f.editor.tickDrag()); // nothing held

	const DeviceInstanceId held = ids[0];
	// THE CARD REPORTS IN ITS OWN COORDINATES; the case converts an editor x back through the card's
	// current place, which moves with the scroll and is re-read each time.
	const auto inCardFor = [&](float editorX) {
		return visage::Point { editorX - f.editor.cardBoundsForTest(held).x(), 10.0f };
	};
	f.editor.cardDragForTest(held, ui::FxCard::DragPhase::Begin, { 10.0f, 10.0f });
	f.editor.cardDragForTest(held, ui::FxCard::DragPhase::Move, inCardFor(rack::RackEditor::kWidth - 4.0f));
	REQUIRE(f.editor.dropGapForTest().has_value());
	const size_t gapBefore = *f.editor.dropGapForTest();

	CHECK(f.editor.tickDrag());
	CHECK(f.editor.scroll() == doctest::Approx(rack::RackEditor::kEdgeScrollStep));
	float last = f.editor.scroll();
	int ticks = 0;
	while (f.editor.tickDrag() && ticks < 10000) {
		CHECK(f.editor.scroll() > last);
		last = f.editor.scroll();
		++ticks;
	}
	CHECK(ticks > 0);
	CHECK(f.editor.scroll() == doctest::Approx(f.editor.maxScroll()));
	REQUIRE(f.editor.dropGapForTest().has_value());
	CHECK(*f.editor.dropGapForTest() > gapBefore);

	// BACK AT THE LEFT EDGE.
	f.editor.cardDragForTest(held, ui::FxCard::DragPhase::Move, inCardFor(4.0f));
	CHECK(f.editor.tickDrag());
	CHECK(f.editor.scroll() == doctest::Approx(f.editor.maxScroll() - rack::RackEditor::kEdgeScrollStep));

	// STILL IN THE MIDDLE.
	f.editor.cardDragForTest(held, ui::FxCard::DragPhase::Move, inCardFor(rack::RackEditor::kWidth * 0.5f));
	const float still = f.editor.scroll();
	CHECK_FALSE(f.editor.tickDrag());
	CHECK(f.editor.scroll() == doctest::Approx(still));

	// DROPPED: nothing more.
	f.editor.cardDragForTest(held, ui::FxCard::DragPhase::End, inCardFor(rack::RackEditor::kWidth * 0.5f));
	CHECK_FALSE(f.editor.dropGapForTest().has_value());
	CHECK_FALSE(f.editor.tickDrag());
}

TEST_CASE("a card that changes height re-places the rack's lane, rather than keeping its rectangle") {
	// **THE STANDALONE RACK NEVER INSTALLED THE HEIGHT HOOK, and that is the third responsibility
	// this family of hosts has been found missing.**
	//
	// `FxCard::onFoldChanged` exists because of a defect in Feathers' fx lane — collapsing an audio
	// effect hid its UI but left the card's size unchanged — where a fold
	// hid the controls and repainted the card AT THE SIZE IT ALREADY HAD, because `laneHeight()`
	// answered the new height immediately and nothing ever asked again. `FxLane` was fixed then.
	// `RackEditor` has the same `laneExtent`-summing stacking and never wired the trigger, so the
	// same defect has been sitting here — and the gated-control resize added 2026-09-21 drives the
	// SAME hook, so it would have done nothing in this host either.
	//
	// **ASKED OF THE PLACEMENT, NOT OF THE CALLBACK.** Checking that the lambda is installed would
	// pass against a lambda that does nothing; the claim is that the card below moves.
	Fixture f;
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.resized();

	// TWO DEVICES, because the claim is about what a height change does to the card BELOW it. With
	// one card there is nothing to move and the case would pass on an empty lane.
	REQUIRE(f.editor.addDeviceByTag(feathers::fx::delay::kTraits.tag) != feathers::kNoInstance);
	REQUIRE(f.editor.addDeviceByTag(feathers::fx::harm::kTraits.tag) != feathers::kNoInstance);
	f.editor.resized();

	std::vector<ui::FxCard*> cards = f.editor.cardsForTest();
	REQUIRE(cards.size() >= 2);
	REQUIRE(cards[0] != nullptr);
	REQUIRE(cards[1] != nullptr);

	// **THE RACK'S LANE RUNS HORIZONTALLY, so a fold changes a card's WIDTH and the card to its
	// RIGHT is the one that moves.** `rack_editor.cpp:1179` asks `card->laneExtent(cardHeight)` and
	// uses the answer as a width — the same call Feathers' vertical lane uses for a height. A first
	// draft of this case asserted the y of the card below and read `152 == 152` against a working
	// fix, because in this host there is no card below.
	const int nextBefore = cards[1]->x();
	const int topBefore = cards[0]->width();

	// FOLD THE TOP ONE. `setFolded` is the road a press takes, and it is what calls the hook.
	cards[0]->setFolded(true);

	// NON-VACUITY: the fold must actually have shortened the card, or "the one below moved" is a
	// claim about a height that did not change.
	CAPTURE(topBefore);
	CAPTURE(cards[0]->width());
	REQUIRE(cards[0]->laneExtent(static_cast<float>(cards[0]->height()))
	        < static_cast<float>(topBefore));

	// **THE CLAIM.** Without the hook the lane never re-places and this card sits where it was,
	// leaving a gap the width of the card that just collapsed.
	CHECK(cards[1]->x() < nextBefore);

	SUBCASE("and unfolding puts it back") {
		cards[0]->setFolded(false);
		CHECK(cards[1]->x() == nextBefore);
	}
}

TEST_CASE("THE HEAD SWEEP, in the rack editor: nothing in any card's spine overlaps anything else, open or folded") {
	// Text and widgets collided in the headers. The fx lanes'
	// half of this sweep is in `test_gui_widgets_devices.cpp`; this is the rack's, where every card
	// wears its head as a SPINE — the name rotated up the left edge, the switch and the source dot at
	// its top, the menu and expand stacked at its foot — and the question is the same one, asked by the
	// same lines (`test::checkHead`): the marks and the measured name inside the spine, disjoint.
	size_t swept = 0;
	for (const device::Descriptor& d : kDevices) {
		Fixture f;
		const DeviceInstanceId id = f.add(d.traits.tag);
		if (id == kNoInstance) {
			continue;
		}
		f.editor.syncCards();
		f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
		f.editor.resized();
		ui::FxCard* card = f.editor.cardOfDeviceForTest(id);
		REQUIRE(card != nullptr);
		const std::string tag = d.traits.tag;
		REQUIRE(card->headEdge() == ui::CardHeadEdge::Left);
		test::checkHead(*card, "rack editor " + tag + " open");
		card->setFolded(true);
		f.editor.resized();
		test::checkHead(*card, "rack editor " + tag + " folded");
		++swept;
	}
	CHECK(swept >= 10);
}

//------------------------------------------------------------------------------------------------//
// THE MACROS CARD IN THE RACK (macro-modulation B3)                                                //
//------------------------------------------------------------------------------------------------//

namespace {

namespace mac = mod::macros;

/// THE CARD THE EDITOR BUILT FOR A MACROS DEVICE — the cast is the claim that the rack gets the same card
/// Feathers does (`ui::makeCustomCard`), not the generic grid.
ui::MacrosCard* macrosCardOf(rack::RackEditor& editor, DeviceInstanceId device) {
	return dynamic_cast<ui::MacrosCard*>(editor.cardOfDeviceForTest(device));
}

} // namespace

TEST_CASE("MACROS in the rack: the editor builds its card, and bipolar on it flips the published output") {
	Fixture f;
	const DeviceInstanceId macros = f.add(mac::kTraits.tag);
	REQUIRE(macros != kNoInstance);
	f.editor.syncCards();
	ui::MacrosCard* card = macrosCardOf(f.editor, macros);
	REQUIRE(card != nullptr);
	f.engine.prepare(48000.0, 4096);

	card->view(1).setValue(0.8);
	ui::chooseMacroMenuEntry(card->slot(1).menuHooks(), ui::kMacroMenuBipolar);
	f.bridge.drain();
	CHECK(f.bridge.fieldAt(macros, mac::bipolarParam(1)) == doctest::Approx(1.0));
	std::vector<float> l(64, 0.0f), r(64, 0.0f);
	f.engine.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
	// THE ENGINE'S PUBLISHED OUTPUT, restated: 2 × 0.8 − 1.
	CHECK(f.engine.publishedOutputAt(f.laneOf(macros), 1) == doctest::Approx(0.6));
	// NOT VACUOUS, and only output 2: the others are unipolar at rest.
	CHECK(f.engine.publishedOutputAt(f.laneOf(macros), 1) != doctest::Approx(0.8));
	CHECK(f.engine.publishedOutputAt(f.laneOf(macros), 0) == doctest::Approx(0.0));
	// AS A GESTURE — the rack's own history holds the step.
	CHECK(f.history.canUndo());
}

TEST_CASE("an LFO routed into Macro 2 moves output 2 and the knob draws a ring") {
	Fixture f;
	const DeviceInstanceId macros = f.add(mac::kTraits.tag);
	const DeviceInstanceId lfo = f.add(mod::lfo::kTraits.tag);
	REQUIRE(macros != kNoInstance);
	REQUIRE(lfo != kNoInstance);
	f.editor.syncCards();
	ui::MacrosCard* card = macrosCardOf(f.editor, macros);
	REQUIRE(card != nullptr);
	f.engine.prepare(48000.0, 4096);

	// A FREE, FAST LFO, as `test_mod_macros`' own case sets it, so the output differs block to block.
	const auto lfoSet = [&](mod::lfo::Param p, double value) {
		const size_t c = static_cast<size_t>(p);
		f.bridge.setField(lfo, c, static_cast<float>(std::max(value, mod::lfo::kDescriptor.params[c].min)));
	};
	lfoSet(mod::lfo::Param::Mode, static_cast<double>(mod::Mode::Free));
	lfoSet(mod::lfo::Param::TimeScale, -4.0);
	lfoSet(mod::lfo::Param::Smoothing, -100.0);
	const float base = 0.5f;
	card->view(1).setValue(base);
	const backplane::Source from = backplane::Source::output(lfo, 0);
	const backplane::Destination to = backplane::Destination::control(macros, mac::macroParam(1));
	const float amount = 0.25f;
	f.bridge.setRouteAmount(from, to, amount);
	f.bridge.drain();
	REQUIRE(f.engine.routeAmount(from, to) == doctest::Approx(amount));

	const ui::Knob& knob = card->slot(1).knob();
	std::vector<float> l(64, 0.0f), r(64, 0.0f);
	size_t moved = 0;
	size_t ringed = 0;
	for (int b = 0; b < 16; ++b) {
		f.engine.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
		f.bridge.publish(f.engine);
		CAPTURE(b);
		const float source = f.engine.publishedOutputAt(f.laneOf(lfo), 0);
		const double expected = std::clamp(static_cast<double>(base) + amount * source, 0.0, 1.0);
		const float output = f.engine.publishedOutputAt(f.laneOf(macros), 1);
		CHECK(output == doctest::Approx(expected));
		if (std::abs(expected - base) > 1e-3) {
			++moved;
			// THE DOT SITS WHERE THE VALUE IS — the published output, in face units (unipolar, so the face IS
			// the value). The macro knob has no amount of its own, so there is no arc.
			const ui::Knob::ModRing ring = knob.modRingForTest();
			CHECK(ring.drawn);
			REQUIRE(ring.hasMark);
			CHECK(ring.mark == doctest::Approx(output).epsilon(1e-3));
			ringed += ring.drawn ? 1u : 0u;
		}
		// OUTPUT 1 IS NOT RIDDEN.
		CHECK(f.engine.publishedOutputAt(f.laneOf(macros), 0) == doctest::Approx(0.0));
	}
	// NOT VACUOUS: the LFO displaced the macro in most blocks.
	CHECK(moved >= 8);
	CHECK(ringed == moved);
}

TEST_CASE("the rack's output arms read the label") {
	Fixture f;
	const DeviceInstanceId macros = f.add(mac::kTraits.tag);
	REQUIRE(macros != kNoInstance);
	f.editor.syncCards();
	const auto armFor = [&](size_t output) -> const ui::SourceArm* {
		for (size_t i = 0; i < f.editor.outputArmCountForTest(); ++i) {
			if (f.editor.outputArmDeviceForTest(i) == macros && f.editor.outputArmOutputForTest(i) == output) {
				return dynamic_cast<const ui::SourceArm*>(f.editor.outputArmFrameForTest(i));
			}
		}
		return nullptr;
	};
	f.editor.setBounds(0, 0, 1200, kEditorHeight); // a laid-out lane, so the chips have a strip
	REQUIRE(armFor(2) != nullptr);
	CHECK(armFor(2)->label() == "Macro 3");
	CHECK(armFor(2)->width() == doctest::Approx(ui::SourceArm::widthFor("Macro 3")));
	f.bridge.setOutputLabel(macros, 2, "Filter sweep");
	REQUIRE(armFor(2) != nullptr);
	CHECK(armFor(2)->label() == "Filter sweep");
	// AS WIDE AS THE WORD, which the lane re-laid it at.
	CHECK(armFor(2)->width() == doctest::Approx(ui::SourceArm::widthFor("Filter sweep")));
	CHECK(armFor(1)->label() == "Macro 2");
	// AND THE CARD'S SLOT CARRIES THE SAME NAME — one label, two surfaces.
	CHECK(macrosCardOf(f.editor, macros)->slot(2).caption() == "Filter sweep");
}

TEST_CASE("a MACROS card popup left open across the rack editor's destruction writes nothing") {
	// A NESTED CHAIN'S EDITOR IS DROPPED AND REBUILT ON A TICK (`ChainExpanded::follow`), and the slot's
	// menu — parented to the root, holding a copy of the hooks — outlives it. The ASan build is where this
	// case earns its keep.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	rack::RackBridge bridge { engine };
	ui::ModState mod;
	ui::EditHistory history;
	auto editor = std::make_unique<rack::RackEditor>(bridge, mod, history);
	const DeviceInstanceId macros = bridge.addDevice(mac::kTraits.tag);
	bridge.drain();
	REQUIRE(macros != kNoInstance);
	editor->syncCards();
	ui::MacrosCard* card = macrosCardOf(*editor, macros);
	REQUIRE(card != nullptr);
	const ui::MacroMenuHooks held = card->slot(1).menuHooks();
	const ui::MacroSlot::Naming naming = card->slot(1).naming();
	editor.reset();

	ui::chooseMacroMenuEntry(held, ui::kMacroMenuMomentary);
	ui::chooseMacroMenuEntry(held, ui::kMacroMenuBipolar);
	naming.rename("Too late");
	CHECK(bridge.fieldAt(macros, mac::typeParam(1)) == doctest::Approx(0.0));
	CHECK(bridge.fieldAt(macros, mac::bipolarParam(1)) == doctest::Approx(0.0));
	CHECK_FALSE(bridge.outputRelabelled(macros, 1));
	CHECK(held.readType() == device::MacroType::Knob);
	CHECK_FALSE(held.readBipolar());
}

TEST_CASE("SHOT the MACROS card in the rack") {
	// NOT A CASE — A CAMERA, gated on FEATHERS_SHOT_DIR like `test_gui_widgets_lane`'s "SHOT the filter
	// card", so an ordinary run costs nothing. The rack editor has no tool of its own to photograph it
	// (`widget-shot` opens Feathers), so it is drawn here windowless into an application editor and the
	// frame written as raw RGBA, byte zero red. Run it under `scripts/venue-xvfb.sh` with
	// VISAGE_FULL_REDRAW=1 so the frame is one state. The state: one knob, one lit latch, one
	// momentary, one output relabelled with a 24-byte name.
	const char* dir = std::getenv("FEATHERS_SHOT_DIR");
	if (dir == nullptr) {
		return;
	}
	Fixture f;
	const DeviceInstanceId macros = f.add(mac::kTraits.tag);
	REQUIRE(macros != kNoInstance);
	f.bridge.setField(macros, mac::macroParam(0), 0.6f);
	f.bridge.setField(macros, mac::macroParam(1), 1.0f);
	f.bridge.setField(macros, mac::typeParam(1), 1.0f);
	f.bridge.setField(macros, mac::typeParam(2), 2.0f);
	f.bridge.setField(macros, mac::macroParam(3), 0.35f);
	f.bridge.setOutputLabel(macros, 3, "Twenty-four byte name!!!");
	f.bridge.drain();
	f.editor.syncCards();
	ui::MacrosCard* card = macrosCardOf(f.editor, macros);
	REQUIRE(card != nullptr);
	(void)card->update();

	const int w = rack::RackEditor::kWidth;
	const int h = kEditorHeight;
	visage::ApplicationEditor app;
	app.setBounds(0, 0, w, h);
	app.addChild(&f.editor);
	f.editor.setBounds(0, 0, w, h);
	app.setWindowless(w, h);
	if (!visage::Renderer::instance().supported()) {
		MESSAGE("SKIP no renderer: " << visage::Renderer::instance().errorMessage());
		app.removeChild(&f.editor);
		return;
	}
	app.redrawAll();
	app.drawWindow();
	app.drawWindow();
	app.drawWindow();
	const visage::Screenshot& shot = app.takeScreenshot();
	MESSAGE("shot " << shot.width() << "x" << shot.height());
	if (shot.width() > 0) {
		const std::string path = std::string(dir) + "/macros_rack.rgba";
		std::ofstream out(path, std::ios::binary);
		out.write(reinterpret_cast<const char*>(shot.data()),
		          static_cast<std::streamsize>(shot.width()) * shot.height() * 4);
		MESSAGE("wrote " << path << " (" << shot.width() << "x" << shot.height() << " RGBA)");
		const visage::Bounds at = f.editor.cardBoundsForTest(macros);
		MESSAGE("card at " << at.x() << "," << at.y() << " " << at.width() << "x" << at.height());
	}
	app.removeChild(&f.editor);
}

//------------------------------------------------------------------------------------------------//
// THE MATH CARD IN THE RACK — the compact layout                                                       //
//------------------------------------------------------------------------------------------------//

namespace {

namespace mth = mod::math;

/// THE CARD THE EDITOR BUILT FOR A MATH DEVICE — the cast is the claim that the rack gets the card Feathers
/// does (`ui::makeCustomCard`), not the generic grid.
ui::MathCard* mathCardOf(rack::RackEditor& editor, DeviceInstanceId device) {
	return dynamic_cast<ui::MathCard*>(editor.cardOfDeviceForTest(device));
}

} // namespace

TEST_CASE("MATH in the rack: the editor builds its compact card, and a pick on its operator changes the published output") {
	Fixture f;
	const DeviceInstanceId math = f.add(mth::kTraits.tag);
	REQUIRE(math != kNoInstance);
	f.editor.syncCards();
	f.editor.resized();
	ui::MathCard* card = mathCardOf(f.editor, math);
	REQUIRE(card != nullptr);
	f.engine.prepare(48000.0, 4096);

	// THE LAYOUT HOLDS BESIDE THE RACK'S SPINE TOO: A, the operator, B in order on one row.
	REQUIRE(card->width() > 0.0f);
	CHECK(card->a().x() + card->a().width() <= card->op().x());
	CHECK(card->op().x() + card->op().width() <= card->b().x());
	CHECK(card->a().x() >= card->bodyLeft());

	card->a().control()->setValue(0.5);
	card->b().control()->setValue(0.5);
	const auto publish = [&] {
		f.bridge.drain();
		std::vector<float> l(64, 0.0f), r(64, 0.0f);
		f.engine.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
		return f.engine.publishedOutputAt(f.laneOf(math), 0);
	};
	// × BY DEFAULT: 0.5 × 0.5, restated.
	CHECK(publish() == doctest::Approx(0.25));
	// "+" PICKED FROM THE MENU THE PRESS OPENS: 0.5 + 0.5, clamped to one.
	const visage::PopupMenu menu = card->guardedMenuFor(card->op());
	menu.onSelection().callback(static_cast<int>(mth::Op::Add));
	CHECK(f.bridge.fieldAt(math, static_cast<size_t>(mth::Param::Operator)) == doctest::Approx(1.0));
	CHECK(publish() == doctest::Approx(1.0));
	// AS A GESTURE — the rack's own history holds the step.
	CHECK(f.history.canUndo());
}

TEST_CASE("a MATH dropdown left open across the rack editor's destruction writes nothing") {
	// `MacrosCard`'s rack case, for this card's two dropdowns: the menu is parented to the root and holds its
	// pick until a click, and the editor — and with it the card — can go first. The bridge outlives both,
	// so a late pick that got through would be READ here as a moved field.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	rack::RackBridge bridge { engine };
	ui::ModState mod;
	ui::EditHistory history;
	auto editor = std::make_unique<rack::RackEditor>(bridge, mod, history);
	const DeviceInstanceId math = bridge.addDevice(mth::kTraits.tag);
	bridge.drain();
	REQUIRE(math != kNoInstance);
	editor->syncCards();
	ui::MathCard* card = mathCardOf(*editor, math);
	REQUIRE(card != nullptr);
	const visage::PopupMenu op = card->guardedMenuFor(card->op());
	const visage::PopupMenu transfer = card->transferMenu();
	editor.reset();

	op.onSelection().callback(static_cast<int>(mth::Op::Max));
	transfer.onSelection().callback(static_cast<int>(mth::Map::Exp));
	bridge.drain();
	CHECK(bridge.fieldAt(math, static_cast<size_t>(mth::Param::Operator)) == doctest::Approx(0.0));
	CHECK(bridge.fieldAt(math, static_cast<size_t>(mth::Param::Map)) == doctest::Approx(0.0));
}

TEST_CASE("SHOT the MATH card in the rack") {
	// NOT A CASE — A CAMERA, the MACROS shot's twin, gated on FEATHERS_SHOT_DIR. The state: A and B off
	// their rests in opposite directions, the operator on "+", Transfer on Log, Bipolar off, Amount at 70%.
	const char* dir = std::getenv("FEATHERS_SHOT_DIR");
	if (dir == nullptr) {
		return;
	}
	Fixture f;
	const DeviceInstanceId math = f.add(mth::kTraits.tag);
	REQUIRE(math != kNoInstance);
	f.bridge.setField(math, static_cast<size_t>(mth::Param::A), 0.6f);
	f.bridge.setField(math, static_cast<size_t>(mth::Param::B), -0.3f);
	f.bridge.setField(math, static_cast<size_t>(mth::Param::Operator), 1.0f);
	f.bridge.setField(math, static_cast<size_t>(mth::Param::Map), 1.0f);
	f.bridge.setField(math, static_cast<size_t>(mth::Param::Bipolar), 0.0f);
	f.bridge.setField(math, static_cast<size_t>(mth::Param::Amount), 0.7f);
	f.bridge.drain();
	f.editor.syncCards();
	ui::MathCard* card = mathCardOf(f.editor, math);
	REQUIRE(card != nullptr);
	(void)card->update();

	const int w = rack::RackEditor::kWidth;
	const int h = kEditorHeight;
	visage::ApplicationEditor app;
	app.setBounds(0, 0, w, h);
	app.addChild(&f.editor);
	f.editor.setBounds(0, 0, w, h);
	app.setWindowless(w, h);
	if (!visage::Renderer::instance().supported()) {
		MESSAGE("SKIP no renderer: " << visage::Renderer::instance().errorMessage());
		app.removeChild(&f.editor);
		return;
	}
	app.redrawAll();
	app.drawWindow();
	app.drawWindow();
	app.drawWindow();
	const visage::Screenshot& shot = app.takeScreenshot();
	MESSAGE("shot " << shot.width() << "x" << shot.height());
	if (shot.width() > 0) {
		const std::string path = std::string(dir) + "/math_rack.rgba";
		std::ofstream out(path, std::ios::binary);
		out.write(reinterpret_cast<const char*>(shot.data()),
		          static_cast<std::streamsize>(shot.width()) * shot.height() * 4);
		MESSAGE("wrote " << path << " (" << shot.width() << "x" << shot.height() << " RGBA)");
		const visage::Bounds at = f.editor.cardBoundsForTest(math);
		MESSAGE("card at " << at.x() << "," << at.y() << " " << at.width() << "x" << at.height());
	}
	app.removeChild(&f.editor);
}


//------------------------------------------------------------------------------------------------//
// BACKLOG JX — A NESTED EDITOR AND THE REAP THAT FREES ITS BRIDGE                                 //
//------------------------------------------------------------------------------------------------//
//
// `ChainExpanded::follow` builds a `RackEditor` over the bridge inside a chain device's instance, and
// drops it only at its NEXT follow — once a beat. The reap that frees a retired instance does not wait for
// that beat, on either host:
//
//   - FEATHERS: `Gui::Impl::tick` runs `tickOnce` (every panel's `updateContent`, so the follow) and THEN
//     `host.guiTick()` -> `provisionAndDrain` -> `Synth::provisionDevices` / `drainRetiredDevices` ->
//     `Converger::reap`, which frees what the drain parked and only then clears the handle. `pumpEditor`
//     runs `provisionAndDrain` a second time after `processEvents`. So every beat has a reap AFTER its
//     follow, and the X events and the draw before the next beat reach the editor in between. A lane
//     removal does not close the panel, and Feathers' carrier keeps the handle until that reap.
//   - THE RACK: `RackGui::tick` reaps first and follows after, but `RackBridge::commit` -> `provision`
//     reaps too, on EVERY edit, from whatever handler made it. A removal (the handle is erased at once) or
//     a body loaded over the chain (a new handle at once), then the drain, then any other edit before the
//     next beat, frees the instance under the open editor.
//
// Measured under ASan before the fix (build-asan in the jx-check worktree): a heap-use-after-free in the
// nested editor. The fix pins the instance while an editor is built over it (`DeviceHost::SurfacePin`,
// `ContainerBodies::pin`); the reap leaves a pinned instance PARKED and clears only its handle, so the next
// follow drops the editor and unpins, and the reap after that frees it.

namespace {

/// Every knob under `root`, depth first.
void knobsUnder(visage::Frame* root, std::vector<ui::Knob*>* out) {
	if (auto* knob = dynamic_cast<ui::Knob*>(root); knob != nullptr) {
		out->push_back(knob);
	}
	for (visage::Frame* child : root->children()) {
		knobsUnder(child, out);
	}
}

/// THE FIRST KNOB ON AN INNER DEVICE'S CARD THAT WRITES A CONTROL — what a hand turns in the panel.
ui::Knob* controlKnobOf(rack::RackEditor& editor, DeviceInstanceId device) {
	editor.syncCards();
	ui::FxCard* card = editor.cardOfDeviceForTest(device);
	REQUIRE(card != nullptr);
	std::vector<ui::Knob*> knobs;
	knobsUnder(card, &knobs);
	for (ui::Knob* k : knobs) {
		if (k->control() != nullptr) {
			return k;
		}
	}
	FAIL("no control knob on the card");
	return nullptr;
}

/// A HAND IN THE PANEL, AND THE BEAT THAT REPAINTS IT: a knob write through the inner bridge, then the
/// reads a repaint makes. Under ASan a freed bridge here is a report.
void touch(rack::RackEditor& editor, ui::Knob& knob) {
	knob.control()->setValue(0.25);
	editor.syncCards();
	editor.syncMacros();
	(void)editor.updateCards();
}

/// **FEATHERS' CARRIER, WITHOUT FEATHERS**: a host whose chain handles live in a `ContainerBodies` that
/// only its converger files and clears (`Synth` hands `ParamBridge::fxBodies()` to `LaneRack`, whose
/// `provision`/`converge`/`reap` are this converger's). Everything else forwards to a rack bridge, because
/// the nested editor's macro row reads the outer host and has to read something.
struct CarrierHost final : backplane::DeviceHost {
	rack::RackBridge& rest;
	ContainerBodies& bodies;
	CarrierHost(rack::RackBridge& r, ContainerBodies& b) : rest(r), bodies(b) {}
	const backplane::Axes& axes() const noexcept override { return rest.axes(); }
	backplane::DeviceView devices() const noexcept override { return rest.devices(); }
	float fieldAt(DeviceInstanceId d, size_t c) const override { return rest.fieldAt(d, c); }
	void setField(DeviceInstanceId d, size_t c, float v) override { rest.setField(d, c, v); }
	size_t fieldsPresentAt(DeviceInstanceId d) const override { return rest.fieldsPresentAt(d); }
	double sampleRate() const noexcept override { return rest.sampleRate(); }
	size_t readingCountAt(DeviceInstanceId d) const noexcept override { return rest.readingCountAt(d); }
	float readingAt(DeviceInstanceId d, size_t i) const noexcept override { return rest.readingAt(d, i); }
	float routeAmount(backplane::Source s, backplane::Destination d) const override { return rest.routeAmount(s, d); }
	void setRouteAmount(backplane::Source s, backplane::Destination d, float a) override { rest.setRouteAmount(s, d, a); }
	bool routedElsewhere(backplane::Destination d, backplane::Source except) const override {
		return rest.routedElsewhere(d, except);
	}
	void beginRouteGesture() override { rest.beginRouteGesture(); }
	void endRouteGesture() override { rest.endRouteGesture(); }
	void forEachRouteFrom(backplane::Source s, void* ctx, void (*f)(void*, backplane::Destination)) const override {
		rest.forEachRouteFrom(s, ctx, f);
	}
	std::string destinationName(backplane::Destination d) const override { return rest.destinationName(d); }
	void* containerSurfaceOf(DeviceInstanceId d) const override { return bodies.surfaceOf(d); }
	void pinContainerSurface(const void* s) override { bodies.pin(s); }
	void unpinContainerSurface(const void* s) override { bodies.unpin(s); }
};

} // namespace

TEST_CASE("backlog JX, Feathers' beat: a chain removed under its open panel stays alive until the follow "
          "after the reap drops the editor — the reap no longer frees it under the editor") {
	namespace cmp = composite;
	rack::registerChainExpandedView();
	Fixture f; // only its bridge, which the macro row reads
	rack::RackEngine lane { kAxes, hostCatalogue() };
	lane.prepare(48000.0, 4096);
	ContainerBodies bodies;
	rack::Converger converger { lane, nullptr };
	constexpr DeviceInstanceId kChain = 7;
	rack::ChainState want(kAxes.deviceSlots, 1);
	{
		rack::DeviceState row;
		row.tag = device::tagOf(cmp::kTraits.tag);
		row.instanceId = kChain;
		REQUIRE(want.append(0, row));
	}
	// PROVISION, THE DRAIN, THE REAP — the device runs and its handle is filed.
	(void)converger.provision(want, &bodies, 0);
	converger.converge(want, 0);
	converger.reap();
	void* const instance = bodies.surfaceOf(kChain);
	REQUIRE(instance != nullptr);
	REQUIRE(lane.retiredCountForTest() == 0);

	CarrierHost host { f.bridge, bodies };
	rack::ChainExpanded view { host, kChain, nullptr };
	view.setBounds(0, 0, rack::RackEditor::kWidth, rack::RackEditor::kHeight);
	REQUIRE(view.liveForTest());
	rack::RackEditor* editor = view.editorForTest();
	const DeviceInstanceId inner = editor->addDeviceByTag(fx::owltt::kTraits.tag);
	REQUIRE(inner != kNoInstance);
	view.bridgeForTest()->drain();
	ui::Knob* knob = controlKnobOf(*editor, inner);
	CHECK(bodies.pinned(instance)); // held while the editor is built over it

	// THE DEVICE LEAVES THE LANE (a removal, an undo, a load): provision, then the drain parks it.
	const rack::ChainState empty(kAxes.deviceSlots, 1);
	(void)converger.provision(empty, &bodies, 0);
	converger.converge(empty, 0);
	REQUIRE(lane.retiredCountForTest() == 1);
	// THE BEAT: `tickOnce` first — the follow finds the handle still filed and keeps the editor...
	(void)view.updateContent();
	REQUIRE(view.liveForTest());
	REQUIRE(view.editorForTest() == editor);
	// ...then `guiTick`'s reap. The handle is cleared ON TIME; the instance is NOT freed, because it is held.
	converger.reap();
	CHECK(bodies.surfaceOf(kChain) == nullptr);
	CHECK(lane.retiredCountForTest() == 1);

	// THE WINDOW BEFORE THE NEXT BEAT: a hand turns a knob in the panel, and the panel is repainted.
	touch(*editor, *knob);

	// THE NEXT BEAT: the follow drops the editor and lets go; the reap after it frees the instance.
	(void)view.updateContent();
	CHECK_FALSE(view.liveForTest());
	CHECK_FALSE(bodies.pinned(instance));
	converger.reap();
	CHECK(lane.retiredCountForTest() == 0);
}

TEST_CASE("backlog JX, the rack's commit: a chain removed under its open panel, then any other edit before "
          "the next beat, does not free the instance under the editor") {
	namespace cmp = composite;
	rack::registerChainExpandedView();
	Fixture f;
	const DeviceInstanceId chain = f.add(cmp::kTraits.tag);
	const DeviceInstanceId other = f.add(fx::owltt::kTraits.tag);
	REQUIRE(chain != kNoInstance);
	REQUIRE(other != kNoInstance);
	void* const instance = f.bridge.containerSurfaceOf(chain);
	REQUIRE(instance != nullptr);

	rack::ChainExpanded view { f.bridge, chain, nullptr };
	view.setBounds(0, 0, rack::RackEditor::kWidth, rack::RackEditor::kHeight);
	REQUIRE(view.liveForTest());
	rack::RackEditor* editor = view.editorForTest();
	const DeviceInstanceId inner = editor->addDeviceByTag(fx::owltt::kTraits.tag);
	REQUIRE(inner != kNoInstance);
	view.bridgeForTest()->drain();
	ui::Knob* knob = controlKnobOf(*editor, inner);
	REQUIRE(f.engine.retiredCountForTest() == 0);

	// REMOVED BY A ROAD THAT DOES NOT CLOSE THE PANEL (an undo, a load), and the drain parks it.
	f.bridge.removeDevice(chain);
	f.bridge.drain();
	REQUIRE(f.engine.retiredCountForTest() == 1);
	// ANOTHER EDIT BEFORE THE NEXT BEAT — its commit reaps.
	f.bridge.setField(other, 0, 0.5f);
	CHECK(f.engine.retiredCountForTest() == 1); // held, not freed
	REQUIRE(view.liveForTest());
	touch(*editor, *knob);

	// THE BEAT: the rack reaps (still held), then the panel follows and lets go; the next reap frees it.
	f.bridge.reapRetired();
	CHECK(f.engine.retiredCountForTest() == 1);
	(void)view.updateContent();
	CHECK_FALSE(view.liveForTest());
	f.bridge.reapRetired();
	CHECK(f.engine.retiredCountForTest() == 0);
}

TEST_CASE("backlog JX: the pin is counted and outlives a carrier's clear — two panels over one instance, "
          "and a patch load clearing the handles, keep it held until the last lets go") {
	ContainerBodies bodies;
	int storage = 0;
	bodies.attach(3, &storage);
	bodies.pin(&storage);
	bodies.pin(&storage);
	bodies.clear(); // a load empties the carrier; a panel still open still holds its instance
	CHECK(bodies.pinned(&storage));
	bodies.unpin(&storage);
	CHECK(bodies.pinned(&storage));
	bodies.unpin(&storage);
	CHECK_FALSE(bodies.pinned(&storage));
	bodies.unpin(&storage); // one too many is nothing
	CHECK_FALSE(bodies.pinned(&storage));
}

// ---------------------------------------------------------------------------------------------
// BACKLOG KR: in the rack, the Filter's type could not be changed by its dropdown or its
// ‹ › arrows, and editing poles no longer worked. Driven the way a hand drives it — a press through the
// editor's tree at the control's own rectangle, a row picked the way visage picks one — and judged by the
// VALUE the rack stores, never by what the card draws.

TEST_CASE("backlog KR: in the rack, the filter's arrows, its type menu and its poles menu all change the stored "
          "value") {
	Fixture f;
	const DeviceInstanceId filter = f.add(fx::filter::kTraits.tag);
	REQUIRE(filter != kNoInstance);
	f.editor.syncCards();
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.resized();
	auto* card = dynamic_cast<ui::FilterCard*>(f.editor.cardOfDeviceForTest(filter));
	REQUIRE(card != nullptr);

	const auto stored = [&](fx::filter::Param p) {
		f.bridge.drain();
		return f.bridge.fieldAt(filter, static_cast<size_t>(p));
	};
	// THE PRESS GOES THROUGH THE TREE (the same restatement as the canvas-focus case above), so a frame
	// lying over the control is part of what is tested.
	const auto press = [&](const visage::Bounds& local) {
		const visage::Point where = card->positionInWindow() + visage::Point { local.xCenter(), local.yCenter() };
		visage::Frame* target = f.editor.frameAtPoint(where);
		REQUIRE(target != nullptr);
		CHECK(target == card); // nothing lies over the head's hand-drawn controls
		visage::MouseEvent e {};
		e.event_frame = target;
		e.window_position = where;
		e.position = where - target->positionInWindow();
		e.button_id = visage::kMouseButtonLeft;
		e.button_state = visage::kMouseButtonLeft;
		e.is_down = true;
		e.repeat_click_count = 1;
		target->processMouseDown(e);
		e.is_down = false;
		e.button_state = visage::kMouseButtonNone;
		target->processMouseUp(e);
	};

	const float lowpass = static_cast<float>(fx::filter::Type::Lowpass);
	const float highpass = static_cast<float>(fx::filter::Type::Highpass);
	f.bridge.setField(filter, static_cast<size_t>(fx::filter::Param::Type), lowpass);
	REQUIRE(stored(fx::filter::Param::Type) == lowpass);

	SUBCASE("the next and previous arrows") {
		press(card->arrowBoundsForTest(1));
		CHECK(stored(fx::filter::Param::Type) == highpass);
		press(card->arrowBoundsForTest(-1));
		CHECK(stored(fx::filter::Param::Type) == lowpass);
	}
	SUBCASE("the type menu") {
		press(card->typeMenuBoundsForTest());
		visage::PopupMenuFrame* popup = menu_check::newestPopup(*card);
		CHECK(popup != nullptr); // CHECK, not REQUIRE: a fatal here would skip the poles subcase
		if (popup != nullptr) {
			CHECK(menu_check::pickRow(*popup, "Bandpass"));
		}
		CHECK(stored(fx::filter::Param::Type) == static_cast<float>(fx::filter::Type::Bandpass));
	}
	SUBCASE("the poles menu") {
		const float before = stored(fx::filter::Param::Slope);
		REQUIRE(before != static_cast<float>(fx::filter::Slope::db24));
		press(card->slopeBoxBoundsForTest());
		visage::PopupMenuFrame* popup = menu_check::newestPopup(*card);
		REQUIRE(popup != nullptr);
		REQUIRE(menu_check::pickRow(*popup, "24 dB"));
		CHECK(stored(fx::filter::Param::Slope) == static_cast<float>(fx::filter::Slope::db24));
	}
}

// ---------------------------------------------------------------------------------------------
// BACKLOG KQ: Smooth Shaper's controls clipped off the bottom of its card in the rack.
// The rack hands every card `min(kCardUnitHeight, lane)` of height and asks it for a WIDTH; a card that lays
// itself out against its vertical-lane height instead puts its last row below the card, where the card's
// clip cuts it off. Swept over every device with a hand-written card, because the defect is a card
// ignoring the height it was given and nothing about that is particular to one card.

TEST_CASE("backlog KQ: in the rack, every control a hand-written card shows is inside the card") {
	for (const device::Descriptor& d : kDevices) {
		Fixture f;
		const DeviceInstanceId added = f.add(d.traits.tag);
		if (added == kNoInstance) {
			continue;
		}
		f.editor.syncCards();
		f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
		f.editor.resized();
		ui::FxCard* card = f.editor.cardOfDeviceForTest(added);
		REQUIRE(card != nullptr);
		// THE GENERIC GRID SCROLLS (its own case, above); a row below its fold is what scrolled means.
		if (dynamic_cast<const ui::GenericCard*>(card) != nullptr) {
			continue;
		}
		const std::string tag = d.traits.tag;
		CAPTURE(tag);
		CAPTURE(card->height());
		for (const visage::Frame* child : card->children()) {
			if (!child->isVisible() || child->width() <= 0 || child->height() <= 0) {
				continue;
			}
			CAPTURE(child->y());
			CAPTURE(child->height());
			CHECK(child->y() >= 0);
			CHECK(child->y() + child->height() <= card->height());
		}
	}
}

TEST_CASE("OC: the standalone rack has its Macros column at the left — macros two across and four tall with their arms BESIDE them, the route list under them — and the lane starts after it with no band above") {
	Fixture f;
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	const DeviceInstanceId inner = f.add(fx::filter::kTraits.tag);
	REQUIRE(inner != kNoInstance);
	f.editor.syncCards();
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight); // laid out with the card
	const float top = f.editor.canvasExtent();
	REQUIRE(f.editor.macrosForTest().size() == 8);
	CHECK(f.editor.levelCountForTest() == 0); // no outer device: no Mix/Gain, no rule
	CHECK(f.editor.columnRuleForTest().width() == 0.0f);
	for (size_t i = 0; i < 8; ++i) {
		CAPTURE(i);
		const visage::Bounds b = f.editor.macroBoundsForTest(i);
		const visage::Bounds arm = f.editor.macroArmBoundsForTest(i);
		CHECK(b.x() == doctest::Approx(12.0f + rack::RackEditor::kMacroCellWidth * static_cast<float>(i % 2)));
		CHECK(b.width() == rack::RackEditor::kMacroKnobWidth);
		// THE ARM IS BESIDE ITS KNOB, level with the dial, inside the cell: not under it.
		CHECK(arm.x() >= b.right());
		CHECK(arm.right() <= 12.0f + rack::RackEditor::kMacroCellWidth * static_cast<float>(i % 2 + 1));
		CHECK(arm.y() >= b.y());
		CHECK(arm.bottom() <= b.bottom());
		if (i >= 2) {
			CHECK(b.y() >= f.editor.macroBoundsForTest(i - 2).bottom()); // four tall: row r under row r-1
			CHECK(f.editor.macroBoundsForTest(i - 2).x() == b.x());
		}
		if (i % 2 == 1) {
			CHECK(f.editor.macroBoundsForTest(i - 1).y() == b.y()); // two across share a row
		}
	}
	CHECK(f.editor.macroBoundsForTest(0).y() >= top);
	// THE ROUTE LIST IS UNDER THE LAST MACRO ROW, in the column, and inside the window: the window keeps its height.
	ui::RouteList& list = f.editor.routeListForTest();
	CHECK(list.x() == 12.0f);
	CHECK(list.width() == rack::RackEditor::kMacroColumnWidth);
	CHECK(list.y() >= f.editor.macroBoundsForTest(7).bottom());
	CHECK(list.bottom() <= static_cast<float>(kEditorHeight));
	CHECK(list.height() >= ui::RouteList::kHeadingHeight + ui::RouteList::kRowHeight);
	CHECK(rack::RackEditor::kHeight == 466);
	// THE LANE STARTS AFTER THE COLUMN AND AT THE TOP: no band above it any more.
	const visage::Bounds card = f.editor.cardBoundsForTest(inner);
	CHECK(card.x() >= rack::RackEditor::kLaneLeft);
	CHECK(card.y() == doctest::Approx(top + 12.0f));
	// THE COLUMN'S GROUND covers what scrolls under it, and it is drawn OVER the cards (sibling order), list included.
	CHECK(f.editor.columnBackdropForTest().right() > f.editor.macroBoundsForTest(1).right());
	ui::FxCard* placed = f.editor.cardOfDeviceForTest(inner);
	REQUIRE(placed != nullptr);
	const int cardAt = f.editor.indexOfChild(placed);
	CHECK(f.editor.indexOfChild(f.editor.macrosForTest()[0].get()) > cardAt);
	CHECK(f.editor.indexOfChild(f.editor.macrosForTest()[7].get()) > cardAt);
	CHECK(f.editor.indexOfChild(f.editor.columnBackdropFrameForTest()) > cardAt);
	CHECK(f.editor.indexOfChild(&list) > cardAt);
	// THE ARITHMETIC THE LAYOUT AND THE REPORT SHARE, against the default height, with and without the settings door.
	CHECK(rack::RackEditor::columnNeeded(false, true) + ui::RouteList::kHeadingHeight + ui::RouteList::kRowHeight
	      <= static_cast<float>(rack::RackEditor::kHeight));
	CHECK(f.editor.macroBoundsForTest(7).bottom() + 12.0f == doctest::Approx(top + rack::RackEditor::columnNeeded(false, false)).epsilon(0.002));
}

TEST_CASE("OC: the Chain window seats Mix and Gain over an H-line over the macros, then the route list, in the default height, and the lane starts after them") {
	namespace cmp = composite;
	rack::registerChainExpandedView();
	Fixture f;
	const DeviceInstanceId chain = f.add(cmp::kTraits.tag);
	REQUIRE(chain != kNoInstance);
	rack::ChainExpanded view { f.bridge, chain, nullptr };
	// THE WINDOW'S DEFAULT SIZE IS ITS MINIMUM, and it is still 466 tall: the route list gives way to the column, not the window.
	const visage::Point size = view.minimumSize();
	CHECK(size.y == static_cast<float>(rack::RackEditor::kHeight));
	view.setBounds(0, 0, size.x, size.y);
	REQUIRE(view.liveForTest());
	rack::RackEditor* editor = view.editorForTest();
	const DeviceInstanceId inner = editor->addDeviceByTag(fx::filter::kTraits.tag);
	REQUIRE(inner != kNoInstance);
	view.bridgeForTest()->drain();
	editor->syncCards();
	view.setBounds(0, 0, size.x, size.y);

	REQUIRE(editor->levelCountForTest() == 2);
	const visage::Bounds mix = editor->levelBoundsForTest(0);
	const visage::Bounds gain = editor->levelBoundsForTest(1);
	const visage::Bounds rule = editor->columnRuleForTest();
	CHECK(mix.y() == gain.y());
	CHECK(mix.right() <= gain.x());
	CHECK(rule.width() == rack::RackEditor::kMacroColumnWidth);
	CHECK(rule.y() >= mix.bottom()); // Mix and Gain above the line
	for (size_t i = 0; i < 8; ++i) {
		CAPTURE(i);
		const visage::Bounds b = editor->macroBoundsForTest(i);
		CHECK(b.y() >= rule.bottom()); // the macros below it
		CHECK(b.x() == (i % 2 == 0 ? mix.x() : gain.x())); // two across, under Mix and Gain
		if (i >= 2) {
			CHECK(b.y() >= editor->macroBoundsForTest(i - 2).bottom());
		}
		CHECK(editor->macroArmBoundsForTest(i).x() >= b.right()); // the arm beside the knob
	}
	// THE ROUTE LIST UNDER THE LAST MACRO ROW HOLDS AT LEAST ITS HEADING AND A ROW, inside the window, at 466.
	// (RELATIVE TO THE CANVAS BAND, which a nested editor has none of once it is presented, and this fixture presents nowhere.)
	const float top = editor->canvasExtent();
	ui::RouteList& list = editor->routeListForTest();
	CHECK(list.y() >= editor->macroBoundsForTest(7).bottom());
	CHECK(list.bottom() - top <= size.y);
	CHECK(list.height() >= ui::RouteList::kHeadingHeight + 3.0f * ui::RouteList::kRowHeight); // head and at least three rows at 466
	CHECK(editor->macroBoundsForTest(7).bottom() - top + 12.0f
	      == doctest::Approx(rack::RackEditor::columnNeeded(true, false) - 4.0f).epsilon(0.02));
	CHECK(editor->cardBoundsForTest(inner).x() >= rack::RackEditor::kLaneLeft);
	CHECK(size.x == static_cast<float>(rack::RackEditor::kWidth));
}

TEST_CASE("OC: the Macros column's route list scrolls by rows when its routes outnumber what fits, keeps its heading, and never scrolls when they fit") {
	Fixture f;
	const DeviceInstanceId filter = f.add(fx::filter::kTraits.tag);
	REQUIRE(filter != kNoInstance);
	const backplane::Source macro = backplane::Source::core(0);
	size_t routed = 0;
	for (size_t p = 0; p < fx::filter::kDescriptor.params.size(); ++p) {
		if (fx::filter::kDescriptor.params[p].modulatable) {
			f.bridge.setRouteAmount(macro, backplane::Destination::control(filter, p), 0.5f);
			++routed;
		}
	}
	f.editor.syncCards();
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.resized();
	ui::RouteList& list = f.editor.routeListForTest();
	f.mod.focus(macro);
	f.mod.arm(true);
	const std::vector<ui::RouteList::Row> rows = list.allRows();
	REQUIRE(rows.size() == routed);
	const size_t visible = list.visibleRows();
	REQUIRE(routed > 3);
	// SQUEEZE THE LIST to head + 3 rows: fewer than there are routes.
	list.setBounds(list.x(), list.y(), list.width(), ui::RouteList::kHeadingHeight + 3.0f * ui::RouteList::kRowHeight);
	CHECK(list.visibleRows() == 3);
	CHECK(list.firstRow() == 0);
	// THE WHEEL SCROLLS BY A ROW AND ANSWERS ONLY WHEN IT MOVED.
	visage::MouseEvent wheel = ui::MouseEventForTest();
	wheel.wheel_delta_y = -1.0f;
	CHECK(list.processMouseWheel(wheel));
	CHECK(list.firstRow() == 1);
	// THE FIRST VISIBLE ROW IS UNDER THE HEADING, and what is hit there is that route (row 1), not row 0.
	const visage::Bounds first = list.rowBounds(1);
	CHECK(first.y() == ui::RouteList::kHeadingHeight);
	const ui::RouteList::Hit hit = list.hitAt({ first.x() + 2.0f, first.y() + 2.0f });
	CHECK(hit.part == ui::RouteList::Part::Amount);
	CHECK(hit.destination == rows[1].destination);
	// A ROW SCROLLED OUT ABOVE OR BELOW IS NOT HIT.
	CHECK(list.hitAt({ 2.0f, list.rowBounds(0).y() + 2.0f }).part == ui::RouteList::Part::None);
	// SCROLLED TO THE END, it stops there and the wheel falls through (not answered).
	for (size_t i = 0; i < routed; ++i) {
		(void)list.processMouseWheel(wheel);
	}
	CHECK(list.firstRow() == routed - 3);
	CHECK_FALSE(list.processMouseWheel(wheel));
	wheel.wheel_delta_y = 1.0f;
	CHECK(list.processMouseWheel(wheel));
	CHECK(list.firstRow() == routed - 4);
	// A LIST TALL ENOUGH FOR EVERY ROW NEVER SCROLLS (Feathers' Engine rail places it at `preferredHeight`).
	list.setBounds(list.x(), list.y(), list.width(), list.preferredHeight());
	CHECK(list.visibleRows() >= routed);
	CHECK(list.firstRow() == 0);
	wheel.wheel_delta_y = -1.0f;
	CHECK_FALSE(list.processMouseWheel(wheel));
	(void)visible;
}

//------------------------------------------------------------------------------------------------//
// BACKLOG OA — MULTI-SELECT AND THE VERBS ON IT, THROUGH THE CHAIN EDITOR                          //
//------------------------------------------------------------------------------------------------//

namespace {

/// A PRESS ON A CARD AS VISAGE DELIVERS ONE: the focus road first (modifier-free, `focusChanged`), then the card's
/// own press with the modifiers, then the release. On the card's body, clear of every head control.
void pressCard(ui::FxCard& card, int modifiers) {
	card.onCardPressed();
	visage::MouseEvent e {};
	e.position = { static_cast<float>(card.width()) * 0.5f, static_cast<float>(card.height()) * 0.5f };
	e.modifiers = modifiers;
	e.repeat_click_count = 1;
	card.noteCardPress(e);
	card.noteCardRelease(e);
}

std::vector<DeviceInstanceId> flagged(rack::RackEditor& editor, const std::vector<DeviceInstanceId>& devices) {
	std::vector<DeviceInstanceId> out;
	for (const DeviceInstanceId d : devices) {
		if (const ui::FxCard* c = editor.cardOfDeviceForTest(d); c != nullptr && c->headFocused()) {
			out.push_back(d);
		}
	}
	return out;
}

} // namespace

TEST_CASE("OA: in the chain editor a card's own press selects — Shift a run from the anchor, Ctrl one more — every selected card draws the flag, a knob's press keeps the selection, and Esc clears it") {
	Fixture f;
	const DeviceInstanceId a = f.add(fx::filter::kTraits.tag);
	const DeviceInstanceId b = f.add(fx::filter::kTraits.tag);
	const DeviceInstanceId c = f.add(fx::filter::kTraits.tag);
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.syncCards();
	const std::vector<DeviceInstanceId> all { a, b, c };
	ui::FxCard* ca = f.editor.cardOfDeviceForTest(a);
	ui::FxCard* cb = f.editor.cardOfDeviceForTest(b);
	ui::FxCard* cc = f.editor.cardOfDeviceForTest(c);
	REQUIRE(ca != nullptr);
	REQUIRE(cb != nullptr);
	REQUIRE(cc != nullptr);

	pressCard(*ca, 0);
	CHECK(flagged(f.editor, all) == std::vector<DeviceInstanceId> { a });
	CHECK(f.editor.focusedDeviceForTest() == a);
	pressCard(*cc, visage::kModifierShift);
	CHECK(flagged(f.editor, all) == all);
	pressCard(*cb, visage::kModifierRegCtrl);
	CHECK(flagged(f.editor, all) == std::vector<DeviceInstanceId> { a, c });
	// A KNOB'S PRESS reaches the card only through the focus road, and keeps a selection that holds it.
	cc->onCardPressed();
	CHECK(flagged(f.editor, all) == std::vector<DeviceInstanceId> { a, c });
	// ONE ENTRY ON THE ESC STACK while two are selected; Esc leaves the anchor alone, selected.
	REQUIRE(f.editor.selectionForTest().escapeHeldForTest());
	CHECK(f.editor.escapeStack().closeTop());
	CHECK(flagged(f.editor, all).size() == 1);
	CHECK_FALSE(f.editor.selectionForTest().escapeHeldForTest());
	// A PLAIN PRESS ON A CARD OF A MULTI-SELECTION keeps it until the release proves it was not a drag.
	pressCard(*ca, 0);
	pressCard(*cc, visage::kModifierShift);
	REQUIRE(flagged(f.editor, all) == all);
	cb->onCardPressed();
	visage::MouseEvent e {};
	e.position = { static_cast<float>(cb->width()) * 0.5f, static_cast<float>(cb->height()) * 0.5f };
	e.repeat_click_count = 1;
	cb->noteCardPress(e);
	CHECK(flagged(f.editor, all) == all);
	cb->noteCardRelease(e);
	CHECK(flagged(f.editor, all) == std::vector<DeviceInstanceId> { b });
}

TEST_CASE("OA: in the chain editor the lasso from the bare lane selects every card it touches, and a click on the bare lane clears") {
	Fixture f;
	const DeviceInstanceId a = f.add(fx::filter::kTraits.tag);
	const DeviceInstanceId b = f.add(fx::filter::kTraits.tag);
	const DeviceInstanceId c = f.add(fx::filter::kTraits.tag);
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.syncCards();
	const std::vector<DeviceInstanceId> all { a, b, c };
	const visage::Bounds ba = f.editor.cardOfDeviceForTest(a)->bounds();
	const visage::Bounds bb = f.editor.cardOfDeviceForTest(b)->bounds();
	// FROM BELOW THE CARDS, ACROSS THE FIRST TWO.
	visage::MouseEvent down {};
	down.position = { ba.x() + 4.0f, ba.bottom() + 4.0f };
	f.editor.onMouseDown().callback(down);
	visage::MouseEvent drag = down;
	drag.position = { bb.x() + 4.0f, bb.y() + bb.height() * 0.5f };
	f.editor.onMouseDrag().callback(drag);
	f.editor.onMouseUp().callback(drag);
	CHECK(flagged(f.editor, all) == std::vector<DeviceInstanceId> { a, b });
	// A CLICK ON THE BARE LANE WITH NO DRAG selects nothing.
	f.editor.onMouseDown().callback(down);
	f.editor.onMouseUp().callback(down);
	CHECK(flagged(f.editor, all).empty());
}

TEST_CASE("OA: the chain editor's dots menu groups the selection into a Chain in place and selects it; Ctrl+D duplicates; each one undo step") {
	Fixture f;
	const DeviceInstanceId a = f.add(fx::filter::kTraits.tag);
	const DeviceInstanceId b = f.add(fx::filter::kTraits.tag);
	const DeviceInstanceId c = f.add(fx::filter::kTraits.tag);
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.syncCards();
	pressCard(*f.editor.cardOfDeviceForTest(a), 0);
	pressCard(*f.editor.cardOfDeviceForTest(b), visage::kModifierShift);
	const size_t steps = f.editor.historyForTest().length();

	REQUIRE(f.editor.slotVerbForTest(a, ui::FragmentVerb::GroupChain));
	f.bridge.drain();
	REQUIRE(f.bridge.chainLength() == 2);
	const DeviceInstanceId chain = f.bridge.deviceAtPosition(0);
	CHECK(device::sameTag(f.bridge.descriptorOf(chain)->traits.tag, composite::kTraits.tag));
	CHECK(f.bridge.deviceAtPosition(1) == c);
	CHECK(liveCards(f.editor) == 2);
	CHECK(f.editor.selectionForTest().selection().ids() == std::vector<DeviceInstanceId> { chain });
	CHECK(f.editor.historyForTest().length() == steps + 1);

	CHECK(f.editor.onKeyPress().callback(visage::KeyEvent { visage::KeyCode::D, visage::kModifierRegCtrl, true }));
	f.bridge.drain();
	CHECK(f.bridge.chainLength() == 3);
	CHECK(liveCards(f.editor) == 3);
	CHECK(f.editor.historyForTest().length() == steps + 2);

	f.editor.historyForTest().undo();
	f.editor.historyForTest().undo();
	f.bridge.drain();
	f.editor.syncCards();
	REQUIRE(f.bridge.chainLength() == 3);
	CHECK(f.bridge.deviceAtPosition(0) == a);
	CHECK(f.bridge.deviceAtPosition(1) == b);
	CHECK(f.bridge.deviceAtPosition(2) == c);
	CHECK(liveCards(f.editor) == 3);
}

TEST_CASE("a single card dragged in a plain chain is ONE undo step: the release reorders the chain, undo puts the order "
          "back and redo moves it again") {
	Fixture f;
	const DeviceInstanceId a = f.add(fx::filter::kTraits.tag);
	const DeviceInstanceId b = f.add(fx::filter::kTraits.tag);
	const DeviceInstanceId c = f.add(fx::filter::kTraits.tag);
	f.editor.setBounds(0, 0, 4000, kEditorHeight);
	f.editor.syncCards();
	const auto order = [&f] {
		std::vector<DeviceInstanceId> out;
		for (size_t p = 0; p < f.bridge.chainLength(); ++p) {
			out.push_back(f.bridge.deviceAtPosition(p));
		}
		return out;
	};
	REQUIRE(order() == std::vector<DeviceInstanceId> { a, b, c });
	const size_t steps = f.editor.historyForTest().length();

	// C, TAKEN BY HAND AND LET GO AT A'S LEFT EDGE: the first gap. Nothing selected, so it is the one-card road.
	const visage::Bounds from = f.editor.cardBoundsForTest(c);
	const visage::Bounds onto = f.editor.cardBoundsForTest(a);
	REQUIRE(from.width() > 0.0f);
	const visage::Point to { onto.x() + 2.0f - from.x(), onto.y() + onto.height() * 0.5f - from.y() };
	f.editor.cardDragForTest(c, ui::FxCard::DragPhase::Begin, { from.width() * 0.5f, from.height() * 0.5f });
	f.editor.cardDragForTest(c, ui::FxCard::DragPhase::Move, to);
	REQUIRE(f.editor.dropGapForTest().has_value());
	CHECK(*f.editor.dropGapForTest() == 0u);
	f.editor.cardDragForTest(c, ui::FxCard::DragPhase::End, to);
	f.bridge.drain();
	CHECK(order() == std::vector<DeviceInstanceId> { c, a, b });
	CHECK(f.engineDeviceAt(0) == c); // the engine runs it there too, not only the mirror
	CHECK(f.editor.historyForTest().length() == steps + 1);

	f.editor.historyForTest().undo();
	f.bridge.drain();
	f.editor.syncCards();
	CHECK(order() == std::vector<DeviceInstanceId> { a, b, c });
	CHECK(f.engineDeviceAt(0) == a);
	f.editor.historyForTest().redo();
	f.bridge.drain();
	CHECK(order() == std::vector<DeviceInstanceId> { c, a, b });
}

TEST_CASE("arps sent to front of chain: a picked arp lands first in the chain while on and last while off; an Add before "
          "keeps its place; a device that emits no notes appends either way") {
	// Read by value: the chain's device at each position.
	struct Reset {
		bool before = ui::emittersToFront();
		~Reset() { (void)ui::setEmittersToFront(before); }
	} reset;
	Fixture f;
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	const DeviceInstanceId filter = f.add(fx::filter::kTraits.tag);
	const DeviceInstanceId env = f.add(mod::dahdsr::kTraits.tag);
	REQUIRE(filter != kNoInstance);
	REQUIRE(env != kNoInstance);
	f.editor.syncCards();

	(void)ui::setEmittersToFront(true);
	const DeviceInstanceId front = f.editor.addDeviceByTag(mod::arp::kTraits.tag); // the (+)'s pick road
	f.bridge.drain();
	REQUIRE(front != kNoInstance);
	REQUIRE(f.bridge.chainLength() == 3);
	CHECK(f.bridge.deviceAtPosition(0) == front);
	CHECK(f.bridge.deviceAtPosition(1) == filter);
	CHECK(f.bridge.deviceAtPosition(2) == env);
	// NOT AN EMITTER: the setting does not move it.
	const DeviceInstanceId lfo = f.editor.addDeviceByTag(mod::lfo::kTraits.tag);
	f.bridge.drain();
	CHECK(f.bridge.deviceAtPosition(3) == lfo);
	// "ADD BEFORE…" NAMES ITS PLACE, and is honoured with the setting on.
	const DeviceInstanceId placed = f.editor.insertDeviceBeforeForTest(env, mod::arp::kTraits.tag);
	f.bridge.drain();
	REQUIRE(placed != kNoInstance);
	CHECK(f.bridge.deviceAtPosition(2) == placed);
	CHECK(f.bridge.deviceAtPosition(3) == env);

	(void)ui::setEmittersToFront(false);
	const DeviceInstanceId last = f.editor.addDeviceByTag(mod::arp::kTraits.tag);
	f.bridge.drain();
	REQUIRE(last != kNoInstance);
	CHECK(f.bridge.deviceAtPosition(f.bridge.chainLength() - 1) == last);
	CHECK(f.bridge.deviceAtPosition(0) == front);
}

TEST_CASE("arps sent to front of chain: the Pattern device, which emits notes too, lands first while on and last while off") {
	// The setting asks the CAPABILITY (`NoteOps::emitNotes`), so the Pattern device joins without a name check.
	REQUIRE(ui::emitsNotes(mod::arp::pat::kDescriptor));
	struct Reset {
		bool before = ui::emittersToFront();
		~Reset() { (void)ui::setEmittersToFront(before); }
	} reset;
	Fixture f;
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	const DeviceInstanceId filter = f.add(fx::filter::kTraits.tag);
	REQUIRE(filter != kNoInstance);
	f.editor.syncCards();
	(void)ui::setEmittersToFront(true);
	const DeviceInstanceId front = f.editor.addDeviceByTag(mod::arp::pat::kTraits.tag);
	f.bridge.drain();
	REQUIRE(front != kNoInstance);
	CHECK(f.bridge.deviceAtPosition(0) == front);
	CHECK(f.bridge.deviceAtPosition(1) == filter);
	(void)ui::setEmittersToFront(false);
	const DeviceInstanceId last = f.editor.addDeviceByTag(mod::arp::pat::kTraits.tag);
	f.bridge.drain();
	REQUIRE(last != kNoInstance);
	CHECK(f.bridge.deviceAtPosition(f.bridge.chainLength() - 1) == last);
}

TEST_CASE("backlog GM: SKLITTER's card in the rack shows exactly its live controls, each with room, at its FIRST layout and after a fold and unfold — in every mode, with no tick between") {
	// **WHAT WAS LOST WAS A VISIBILITY, AND THIS ASKS THE VISIBILITY BY VALUE.** Mix and Density were
	// absent until a resize; scrolling did not bring them back. So every row is asked the two things that
	// can hide it — is it visible, and has it room — against an independent restatement of what SHOULD be
	// live: the device's own gate chain (`device::relevantFor`, followed up `gateControl`), read off the
	// bridge. No `update()` is run between the steps, because a tick's re-layout is exactly the repair that
	// would cover a stale first layout (the venue caveat in GM's own entry).
	namespace sc = fx::sklitter;
	rack::RackEngine engine { kAxes, hostCatalogue() };
	rack::RackBridge bridge { engine };
	const DeviceInstanceId device = bridge.addDevice(sc::kTraits.tag);
	REQUIRE(device != kNoInstance);
	const device::Descriptor& desc = sc::kDescriptor;
	const auto modeIndex = static_cast<size_t>(sc::Param::Mode);
	const size_t modes = desc.params[modeIndex].names.size();
	REQUIRE(modes >= 2);

	const auto shouldShow = [&](size_t index) {
		for (size_t at = index;;) {
			const device::ParamDesc& p = desc.params[at];
			if (p.gateControl < 0) {
				return true;
			}
			const auto gate = static_cast<size_t>(p.gateControl);
			if (!device::relevantFor(p, bridge.fieldAt(device, gate))) {
				return false;
			}
			at = gate;
		}
	};
	const auto checkRows = [&](const ui::GenericCard& card, const char* when) {
		const std::string step = when;
		CAPTURE(step);
		const std::vector<visage::Frame*>& rows = card.rowsForTest();
		const std::vector<size_t>& params = card.paramIndicesForTest();
		REQUIRE(rows.size() == params.size());
		size_t shown = 0;
		for (size_t i = 0; i < rows.size(); ++i) {
			const std::string control = desc.params[params[i]].name;
			CAPTURE(control);
			const bool want = !card.folded() && shouldShow(params[i]);
			CHECK(rows[i]->isVisible() == want);
			if (want) {
				++shown;
				CHECK(rows[i]->width() > 0.0f);
				CHECK(rows[i]->height() > 0.0f);
				CHECK(rows[i]->x() >= 0.0f);
				CHECK(rows[i]->right() <= card.width() + 0.5f);
			}
		}
		return shown;
	};

	for (size_t mode = 0; mode < modes; ++mode) {
		const std::string modeName = desc.params[modeIndex].names[mode];
		CAPTURE(modeName);
		bridge.setField(device, modeIndex, static_cast<float>(mode));
		ui::SklitterCard card { bridge, device, desc, nullptr };
		card.setLaneAxis(ui::FxCard::LaneAxis::Horizontal);
		// THE RACK'S ORDER: the lane asks the extent at its cross, then places the card. First layout.
		const float cross = 466.0f;
		card.setBounds(0.0f, 0.0f, card.laneExtent(cross), cross);
		REQUIRE(card.width() > 0.0f);
		const size_t first = checkRows(card, "first layout");
		CHECK(first > 0);
		// MIX IS UNGATED: it is on the face in every mode, which is the control that went missing.
		// FOLDED AND OPENED AGAIN AT THE SAME BOUNDS — a host whose extent does not move with the fold
		// gives the card no `resized()`, so whatever the unfold itself shows is what is drawn.
		card.setFolded(true);
		checkRows(card, "folded");
		card.setFolded(false);
		CHECK(checkRows(card, "unfolded at the same bounds") == first);
	}

	// ONE CARD, THE MODE WALKED UNDER IT — so a row the mode gates away has had REAL bounds in an earlier
	// mode, and an unfold that showed it would show it over the row that closed up into its place.
	bridge.setField(device, modeIndex, 0.0f);
	ui::SklitterCard card { bridge, device, desc, nullptr };
	card.setLaneAxis(ui::FxCard::LaneAxis::Horizontal);
	card.setBounds(0.0f, 0.0f, card.laneExtent(466.0f), 466.0f);
	for (size_t mode = 0; mode < modes; ++mode) {
		const std::string modeName = desc.params[modeIndex].names[mode];
		CAPTURE(modeName);
		bridge.setField(device, modeIndex, static_cast<float>(mode));
		(void)card.update(); // the per-frame gate poll re-lays the rows for the new mode
		const size_t live = checkRows(card, "after the mode moved");
		card.setFolded(true);
		card.setFolded(false);
		CHECK(checkRows(card, "folded and unfolded after the mode moved") == live);
	}
}

TEST_CASE("backlog NE: the lane's horizontal scroll bar is hidden while nothing overflows, shows the visible span when the chain does, drags the chain both ways, follows the wheel and a held card's edge scroll, and sits in the pad under the lane at 1x and 1.75x") {
	Fixture f;
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.resized();
	rack::LaneScrollBar& bar = f.editor.laneScrollBarForTest();
	CHECK_FALSE(bar.isVisible()); // an empty chain: nothing to scroll, nothing drawn

	std::vector<DeviceInstanceId> ids;
	for (size_t i = 0; i < kAxes.deviceSlots; ++i) {
		ids.push_back(f.add(fx::filter::kTraits.tag));
	}
	f.editor.syncCards();
	f.editor.resized();
	const float maxScroll = f.editor.maxScroll();
	REQUIRE(maxScroll > 0.0f);
	REQUIRE(bar.isVisible());

	// THE SPAN: the thumb is the view's fraction of the whole chain, restated from the layout's own numbers.
	const float track = bar.width();
	const float view = track; // the bar spans the lane's view
	CHECK(bar.thumb().width == doctest::Approx(std::max(rack::LaneScrollBar::kMinThumb, track * view / (view + maxScroll))));
	CHECK(bar.thumb().x == doctest::Approx(0.0f));
	// IN THE PAD UNDER THE LANE: below every card, inside the editor.
	for (const DeviceInstanceId id : ids) {
		CHECK(bar.y() >= f.editor.cardBoundsForTest(id).bottom());
	}
	CHECK(bar.bottom() <= f.editor.height());

	// A DRAG ON THE THUMB carries the chain: half the travel is half the scroll, and back to the start.
	const float travel = track - bar.thumb().width;
	visage::MouseEvent e = ui::MouseEventForTest();
	e.position = { bar.thumb().width * 0.5f, 2.0f };
	bar.onMouseDown().callback(e);
	e.position.x += travel * 0.5f;
	bar.onMouseDrag().callback(e);
	CHECK(f.editor.scroll() == doctest::Approx(maxScroll * 0.5f).epsilon(1e-3));
	CHECK(bar.thumb().x == doctest::Approx(travel * 0.5f).epsilon(1e-3));
	e.position.x = bar.thumb().width * 0.5f;
	bar.onMouseDrag().callback(e);
	bar.onMouseUp().callback(e);
	CHECK(f.editor.scroll() == doctest::Approx(0.0f));
	// A PRESS ON THE TRACK at its end jumps there: the chain to its end.
	e.position.x = track - 1.0f;
	bar.onMouseDown().callback(e);
	bar.onMouseUp().callback(e);
	CHECK(f.editor.scroll() == doctest::Approx(maxScroll));
	CHECK(bar.thumb().x == doctest::Approx(travel));

	// THE WHEEL'S ROAD AND A HELD CARD'S EDGE SCROLL MOVE THE SAME SCROLL, and the thumb follows.
	f.editor.scrollBy(-maxScroll);
	CHECK(bar.thumb().x == doctest::Approx(0.0f));
	const DeviceInstanceId held = ids[0];
	f.editor.cardDragForTest(held, ui::FxCard::DragPhase::Begin, { 10.0f, 10.0f });
	f.editor.cardDragForTest(held, ui::FxCard::DragPhase::Move,
	                         { rack::RackEditor::kWidth - 4.0f - f.editor.cardBoundsForTest(held).x(), 10.0f });
	REQUIRE(f.editor.tickDrag());
	CHECK(f.editor.scroll() == doctest::Approx(rack::RackEditor::kEdgeScrollStep));
	CHECK(bar.thumb().x == doctest::Approx(travel * rack::RackEditor::kEdgeScrollStep / maxScroll).epsilon(1e-3));
	f.editor.cardDragForTest(held, ui::FxCard::DragPhase::End, { 10.0f, 10.0f });

	// AT 1.75x THE BAR IS RE-PLACED AT THE NEW SCALE: its NATIVE width is the logical width at that scale.
	f.editor.setDpiScale(1.75f);
	f.editor.resized();
	CHECK(bar.dpiScale() == 1.75f);
	CHECK(std::abs(static_cast<float>(bar.nativeWidth()) - 1.75f * bar.width()) <= 1.0f);

	// AND WITH THE CHAIN EMPTIED, HIDDEN AGAIN.
	for (const DeviceInstanceId id : ids) {
		f.remove(id);
	}
	f.editor.syncCards();
	f.editor.resized();
	CHECK_FALSE(bar.isVisible());
}

TEST_CASE("JB C10: the rack editor's add and remove are undo steps, and Ctrl+Z, Ctrl+Shift+Z and Ctrl+Y walk its history — the chain restored by value") {
	Fixture f;
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	f.editor.resized();
	const auto order = [&] {
		std::vector<DeviceInstanceId> out;
		for (size_t p = 0; p < f.bridge.chainLength(); ++p) {
			out.push_back(f.bridge.deviceAtPosition(p));
		}
		return out;
	};
	const auto key = [&](visage::KeyCode code, int mods) {
		return f.editor.onKeyPress().callback(visage::KeyEvent { code, mods, true });
	};
	const DeviceInstanceId a = f.editor.addDeviceByTag(fx::filter::kTraits.tag);
	f.bridge.drain();
	const DeviceInstanceId b = f.editor.addDeviceByTag(fx::filter::kTraits.tag);
	f.bridge.drain();
	REQUIRE((a != kNoInstance && b != kNoInstance));
	CHECK(f.history.length() == 2);
	f.editor.removeDevice(a);
	f.bridge.drain();
	CHECK(f.history.length() == 3);
	REQUIRE(order() == std::vector<DeviceInstanceId> { b });

	CHECK(key(visage::KeyCode::Z, visage::kModifierRegCtrl)); // the remove
	f.bridge.drain();
	CHECK(order() == (std::vector<DeviceInstanceId> { a, b }));
	CHECK(key(visage::KeyCode::Z, visage::kModifierRegCtrl)); // b's add
	f.bridge.drain();
	CHECK(order() == std::vector<DeviceInstanceId> { a });
	CHECK(key(visage::KeyCode::Z, visage::kModifierRegCtrl | visage::kModifierShift)); // redo b
	f.bridge.drain();
	CHECK(order() == (std::vector<DeviceInstanceId> { a, b }));
	CHECK(key(visage::KeyCode::Y, visage::kModifierRegCtrl)); // redo the remove
	f.bridge.drain();
	CHECK(order() == std::vector<DeviceInstanceId> { b });
	CHECK_FALSE(f.history.canRedo());
	CHECK_FALSE(key(visage::KeyCode::Z, 0)); // a plain Z is not the chord
}

TEST_CASE("backlog GM, the roads not yet tried: SKLITTER's card in a real rack editor keeps exactly its live controls, each with room, through a live mode change (no resize), a canvas open and close, and a scale change") {
	// GM's entry names what was not tried: a live gate change with nothing resizing the card, the canvas opening and
	// closing above the lane (which moves the lane's height), and a scale change. Each is asked the same two things the
	// first-layout case asks — visible iff live (the device's own gate chain), and with room — of a card the EDITOR
	// placed and re-placed, not one a case laid out by hand.
	namespace sc = fx::sklitter;
	Fixture f;
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	const DeviceInstanceId device = f.add(sc::kTraits.tag);
	REQUIRE(device != kNoInstance);
	f.editor.syncCards();
	f.editor.resized();
	auto* card = dynamic_cast<ui::GenericCard*>(f.editor.cardOfDeviceForTest(device));
	REQUIRE(card != nullptr);
	const device::Descriptor& desc = sc::kDescriptor;
	const auto modeIndex = static_cast<size_t>(sc::Param::Mode);
	const auto shouldShow = [&](size_t index) {
		for (size_t at = index;;) {
			const device::ParamDesc& p = desc.params[at];
			if (p.gateControl < 0) {
				return true;
			}
			const auto gate = static_cast<size_t>(p.gateControl);
			if (!device::relevantFor(p, f.bridge.fieldAt(device, gate))) {
				return false;
			}
			at = gate;
		}
	};
	const auto check = [&](const char* when) {
		const std::string step = when;
		CAPTURE(step);
		const std::vector<visage::Frame*>& rows = card->rowsForTest();
		const std::vector<size_t>& params = card->paramIndicesForTest();
		REQUIRE(rows.size() == params.size());
		size_t shown = 0;
		for (size_t i = 0; i < rows.size(); ++i) {
			const std::string control = desc.params[params[i]].name;
			CAPTURE(control);
			const bool want = !card->folded() && shouldShow(params[i]);
			CHECK(rows[i]->isVisible() == want);
			if (want) {
				++shown;
				CHECK(rows[i]->width() > 0.0f);
				CHECK(rows[i]->height() > 0.0f);
				CHECK(rows[i]->right() <= card->width() + 0.5f);
			}
		}
		CHECK(shown > 0);
	};
	check("first layout in the editor");
	// A LIVE MODE CHANGE, AS THE HOST'S AUTOMATION MAKES IT: the record moves, the next tick updates the card, and nothing
	// resizes it. Every mode, and back.
	const size_t modes = desc.params[modeIndex].names.size();
	for (size_t mode = 0; mode < modes; ++mode) {
		const std::string modeName = desc.params[modeIndex].names[mode];
		CAPTURE(modeName);
		f.bridge.setField(device, modeIndex, static_cast<float>(mode));
		f.bridge.drain();
		(void)card->update();
		check("live mode change");
	}
	// THE CANVAS OPENS ABOVE THE LANE (the lane's height moves) and closes again.
	(void)f.editor.canvas().setCollapsed(false);
	f.editor.resized();
	check("canvas open");
	(void)f.editor.canvas().setCollapsed(true);
	f.editor.resized();
	check("canvas closed");
	// AND A SCALE CHANGE (a scale moves nothing logical; CLAUDE.md's setBounds rule).
	f.editor.setDpiScale(1.75f);
	f.editor.resized();
	check("1.75x");
}

TEST_CASE("JB: the rack editor shows undo and redo beside its settings door — a press walks the history, the chain restored by value, and they repaint when the history moves") {
	Fixture f;
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	CHECK(f.editor.undoRedoForTest() == nullptr); // no door row, no buttons (a nested editor uses the keys)
	f.editor.setSettingsDoor([] {});
	rack::UndoRedoButtons* buttons = f.editor.undoRedoForTest();
	REQUIRE(buttons != nullptr);
	CHECK(buttons->isVisible());
	CHECK(buttons->width() == doctest::Approx(rack::UndoRedoButtons::kWidth));
	CHECK(buttons->right() <= rack::RackEditor::kLaneLeft); // in the column, not over the lane
	const size_t told = f.history.framesToldForTest();
	const DeviceInstanceId a = f.editor.addDeviceByTag(fx::filter::kTraits.tag);
	f.bridge.drain();
	REQUIRE(a != kNoInstance);
	CHECK(f.history.framesToldForTest() > told); // the buttons were told: they repaint lit

	visage::MouseEvent e = ui::MouseEventForTest();
	e.position = { buttons->undoBounds().x() + 4.0f, 4.0f };
	buttons->onMouseDown().callback(e);
	f.bridge.drain();
	CHECK(f.bridge.chainLength() == 0);
	CHECK(f.history.canRedo());
	e.position = { buttons->redoBounds().x() + 4.0f, 4.0f };
	buttons->onMouseDown().callback(e);
	f.bridge.drain();
	REQUIRE(f.bridge.chainLength() == 1);
	CHECK(f.bridge.deviceAtPosition(0) == a);
}

TEST_CASE("TFIELD in the rack: the card's 2x4 output block shows the values a route receives, block after block") {
	// THE BLOCK RECOMPUTES the eight outputs from the published orbit phases (playhead: A as the phase, B as the level)
	// and the MODULATED row, through the device's own `readLayer` and `blend`. The claim is a VALUE: what the card shows
	// is what the engine published, while the orbit moves them.
	namespace tf = mod::twinfield;
	Fixture f;
	const DeviceInstanceId field = f.add(tf::kTraits.tag);
	REQUIRE(field != kNoInstance);
	f.editor.syncCards();
	f.editor.resized();
	auto* card = dynamic_cast<ui::TwinFieldCard*>(f.editor.cardOfDeviceForTest(field));
	REQUIRE(card != nullptr);
	f.engine.prepare(48000.0, 4096);
	f.bridge.setField(field, tf::layerParam(0, tf::LayerParam::Sync), 1.0f); // 1 Hz, so the orbit visibly moves
	f.bridge.setField(field, tf::layerParam(1, tf::LayerParam::Sync), 1.0f);
	f.bridge.setField(field, tf::layerParam(1, tf::LayerParam::Rate), 1.0f); // B twice as fast: the two phases differ, so a swap shows
	f.bridge.setField(field, tf::at(tf::Param::Crossfade), 0.35f);
	std::vector<float> l(512, 0.0f), r(512, 0.0f);
	std::array<float, tf::kPoints> first {};
	for (int block = 0; block < 12; ++block) {
		CAPTURE(block);
		f.bridge.drain();
		f.engine.processStereo(l.data(), r.data(), 512, backplane::unknownBlock());
		f.bridge.publish(f.engine);
		(void)card->updateContent();
		std::array<float, tf::kPoints> shown {};
		REQUIRE(card->liveOutputs(shown));
		for (size_t k = 0; k < tf::kPoints; ++k) {
			CAPTURE(k);
			CHECK(std::abs(shown[k] - f.engine.publishedOutputAt(f.laneOf(field), k)) <= 1e-5f);
			CHECK(card->shownOutputsForTest()[k] == shown[k]);
		}
		if (block == 0) {
			first = shown;
		}
		if (block == 11) {
			CHECK(shown != first); // NOT VACUOUS: the orbit moved the outputs, and the block followed
			uint32_t ordinal = 0;
			float a = 0.0f;
			float b = 0.0f;
			REQUIRE(f.bridge.playheadAt(field, ordinal, a));
			REQUIRE(f.bridge.playheadLevelAt(field, b));
			CHECK(std::abs(a - b) > 0.01f); // NOT VACUOUS: the two published phases are not one number
		}
	}
}

TEST_CASE("TFIELD in the rack: the card's terrain draws the puck where the published orbit phase puts it, and a stopped orbit costs nothing") {
	// The orbit gives visual feedback. The puck rides its orbit around the pan
	// point at the phase the device PUBLISHES (the same channel the output cells read), and the eight points ride with
	// it. RESTATED here from the design's words: the pan point is the pad's centre, the orbit a circle `Depth` rings
	// wide, a ring 0.6 of a one-zone half-view over 2^zoom, Y up.
	namespace tf = mod::twinfield;
	Fixture f;
	const DeviceInstanceId field = f.add(tf::kTraits.tag);
	REQUIRE(field != kNoInstance);
	f.editor.syncCards();
	f.editor.resized();
	auto* card = dynamic_cast<ui::TwinFieldCard*>(f.editor.cardOfDeviceForTest(field));
	REQUIRE(card != nullptr);
	ui::TwinFieldPad& pad = card->pad();
	REQUIRE(pad.width() > 10.0f);
	f.engine.prepare(48000.0, 4096);
	const double depth = 0.8;
	const double zoom = 0.5;
	f.bridge.setField(field, tf::layerParam(0, tf::LayerParam::Sync), 1.0f); // 1 Hz
	f.bridge.setField(field, tf::layerParam(0, tf::LayerParam::Depth), static_cast<float>(depth));
	f.bridge.setField(field, tf::layerParam(0, tf::LayerParam::Zoom), static_cast<float>(zoom));
	std::vector<float> l(512, 0.0f), r(512, 0.0f);
	const auto block = [&] {
		f.bridge.drain();
		f.engine.processStereo(l.data(), r.data(), 512, backplane::unknownBlock());
		f.bridge.publish(f.engine);
		(void)card->updateContent();
	};
	visage::Point first {};
	for (int b = 0; b < 10; ++b) {
		CAPTURE(b);
		block();
		uint32_t ordinal = 0;
		float phase = 0.0f;
		REQUIRE(f.bridge.playheadAt(field, ordinal, phase));
		const double half = std::max(std::min(pad.width(), pad.height()) * 0.5, 1.0);
		const double ppp = 0.125 / std::exp2(zoom) / half;
		const double ring = 0.6 * 0.125; // Spread 0.6 zones (its default), on the TERRAIN: zoom is not in it (2026-11-08)
		CHECK(std::abs(pad.ringPixels() - ring / ppp) <= 0.01); // the DRAWN ring is that size through the zoom
		const double a = 2.0 * std::numbers::pi * static_cast<double>(phase);
		const double px = pad.width() * 0.5 + depth * ring * std::cos(a) / ppp;
		const double py = pad.height() * 0.5 - depth * ring * std::sin(a) / ppp;
		const visage::Point puck = pad.puckPixel();
		CHECK(std::abs(puck.x - px) <= 0.01);
		CHECK(std::abs(puck.y - py) <= 0.01);
		// THE EIGHT RIDE WITH IT: point k is the puck plus the ring at 45 degrees x k.
		for (size_t k = 0; k < tf::kPoints; ++k) {
			const double ak = std::numbers::pi / 4.0 * static_cast<double>(k);
			CHECK(std::abs(pad.pointPixel(k).x - (puck.x + ring * std::cos(ak) / ppp)) <= 0.01);
			CHECK(std::abs(pad.pointPixel(k).y - (puck.y - ring * std::sin(ak) / ppp)) <= 0.01);
		}
		if (b == 0) {
			first = puck;
		}
	}
	CHECK(std::abs(pad.puckPixel().x - first.x) + std::abs(pad.puckPixel().y - first.y) > 1.0f); // NOT VACUOUS: it moved

	// STOPPED (Once, no note: the orbit holds at its start): after the first block that settles it, a block asks the
	// card for nothing — no repaint is told and the terrain is not rebuilt.
	f.bridge.setField(field, tf::layerParam(0, tf::LayerParam::Mode), static_cast<float>(tf::Mode::Once));
	f.bridge.setField(field, tf::layerParam(1, tf::LayerParam::Mode), static_cast<float>(tf::Mode::Once));
	block();
	block();
	const size_t rebuilds = pad.rebuildsForTest();
	for (int b = 0; b < 5; ++b) {
		f.bridge.drain();
		f.engine.processStereo(l.data(), r.data(), 512, backplane::unknownBlock());
		f.bridge.publish(f.engine);
		CHECK_FALSE(card->updateContent());
	}
	CHECK(pad.rebuildsForTest() == rebuilds);
}

// ---- QN: THE MACROS COLUMN FOLDS (collapsible in the rack and in a chain) ----

TEST_CASE("QN: the Macros column folds to the rail's strip and back — the lane starts at the strip, the column's widgets hide, "
          "the hand's toggle says so, and a macro and its route keep working while it is folded") {
	Fixture f;
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	const DeviceInstanceId filter = f.add(fx::filter::kTraits.tag);
	REQUIRE(filter != kNoInstance);
	f.editor.syncCards();
	f.editor.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	ui::Rail& rail = f.editor.macrosRailForTest();
	std::vector<bool> told;
	f.editor.onMacrosCollapsedChanged = [&told](bool c) { told.push_back(c); };

	// OPEN: the lane after the column, the toggle a corner at the column's top right, nothing of the column hidden.
	REQUIRE_FALSE(f.editor.macrosCollapsed());
	CHECK(f.editor.laneLeft() == rack::RackEditor::kLaneLeft);
	const float openCardX = f.editor.cardBoundsForTest(filter).x();
	CHECK(openCardX >= rack::RackEditor::kLaneLeft);
	CHECK(rail.width() == doctest::Approx(rack::RackEditor::kMacrosToggleRoom));
	CHECK(rail.right() == doctest::Approx(rack::RackEditor::kLaneLeft - 6.0f));
	CHECK(f.editor.macrosForTest().at(0)->isVisible());
	CHECK(f.editor.routeListForTest().isVisible());

	// A ROUTE FROM MACRO 1 INTO THE FILTER, before the fold.
	const backplane::Source macro1 = backplane::Source::core(0);
	const backplane::Destination cutoff =
	    backplane::Destination::control(filter, fx::filter::kDescriptor.controlOfNthModulatableBesideEnable(0));
	f.bridge.setRouteAmount(macro1, cutoff, 0.5f);
	f.bridge.drain();
	REQUIRE(f.engine.routeAmount(macro1, cutoff) == doctest::Approx(0.5f));

	// THE HAND'S TOGGLE: a press on it folds the column, says so, and tells the owner.
	const visage::Bounds t = rail.toggleBounds();
	visage::MouseEvent press = ui::MouseEventForTest();
	press.position = { t.x() + t.width() * 0.5f, t.y() + t.height() * 0.5f };
	rail.processMouseDown(press);
	REQUIRE(f.editor.macrosCollapsed());
	CHECK(f.editor.notifierTextForTest() == "Macros minimised");
	CHECK(told == std::vector<bool> { true });
	// FOLDED: the strip is the rail's own width, the lane starts at it, and the column's widgets are hidden.
	CHECK(rail.width() == doctest::Approx(ui::Rail::kCollapsedWidth));
	CHECK(rail.x() == 0.0f);
	CHECK(f.editor.laneLeft() == doctest::Approx(ui::Rail::kCollapsedWidth + 6.0f));
	const float foldedCardX = f.editor.cardBoundsForTest(filter).x();
	CHECK(openCardX - foldedCardX == doctest::Approx(rack::RackEditor::kLaneLeft - (ui::Rail::kCollapsedWidth + 6.0f)));
	for (const auto& slot : f.editor.macrosForTest()) {
		CHECK_FALSE(slot->isVisible());
	}
	CHECK_FALSE(f.editor.routeListForTest().isVisible());

	// AND STILL WORKING: the route is the engine's, untouched by the fold; a macro the host moves reaches the engine.
	CHECK(f.engine.routeAmount(macro1, cutoff) == doctest::Approx(0.5f));
	f.bridge.setMacro(0, 0.75f);
	f.bridge.drain();
	CHECK(f.bridge.macro(0) == doctest::Approx(0.75f));
	CHECK(f.engine.macro(0) == doctest::Approx(0.75f));

	// THE OWNER'S RESTORE (`setMacrosCollapsed`) unfolds without a notice of its own; the widgets come back.
	f.editor.notifyTrayForTest().say("");
	REQUIRE(f.editor.setMacrosCollapsed(false));
	CHECK_FALSE(f.editor.macrosCollapsed());
	CHECK(f.editor.laneLeft() == rack::RackEditor::kLaneLeft);
	CHECK(f.editor.cardBoundsForTest(filter).x() == doctest::Approx(openCardX));
	CHECK(f.editor.macrosForTest().at(0)->isVisible());
	CHECK(f.editor.routeListForTest().isVisible());
	CHECK(told == std::vector<bool> { true, false });
	CHECK_FALSE(f.editor.setMacrosCollapsed(false)); // no change, no word
}

TEST_CASE("QN: a CHAIN device's Macros column, folded in its window, rides its body through the outer patch into a fresh rack, "
          "and a load under an open editor folds it on the beat without a word") {
	// THE CHAIN WINDOW'S EDITOR is a `RackEditor` over the chain's inner bridge (`rack::ChainExpanded`), and the chain's body
	// is that bridge's patch — the road Feathers' own chains block and the Rack plugin's state both take.
	std::string saved;
	{
		Fixture f;
		const DeviceInstanceId chain = f.add(composite::kTraits.tag);
		REQUIRE(chain != kNoInstance);
		rack::RackBridge* inner = rack::bridgeOf(f.bridge.containerSurfaceOf(chain));
		REQUIRE(inner != nullptr);
		ui::ModState mod;
		ui::EditHistory history;
		rack::RackEditor window(*inner, mod, history);
		window.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
		REQUIRE(window.setMacrosCollapsed(true));
		CHECK(inner->macrosCollapsed());
		const rack::RackPatch outer = f.bridge.patch();
		std::vector<uint8_t> bytes;
		clap_ostream out { &bytes, [](const clap_ostream* s, const void* data, uint64_t n) -> int64_t {
			                  auto* v = static_cast<std::vector<uint8_t>*>(s->ctx);
			                  v->insert(v->end(), static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + n);
			                  return static_cast<int64_t>(n);
		                  } };
		REQUIRE(rack::writeRack(&out, outer));
		saved.assign(bytes.begin(), bytes.end());
		CHECK(saved.find("\"macrosColumn\"") != std::string::npos);
	}
	Fixture g;
	rack::RackPatch patch;
	REQUIRE(rack::parseRackText(saved, &patch));
	(void)g.bridge.loadPatch(patch);
	g.bridge.drain();
	REQUIRE(g.engine.slotCount() >= 1);
	const DeviceInstanceId chain = g.engineDeviceAt(0);
	rack::RackBridge* inner = rack::bridgeOf(g.bridge.containerSurfaceOf(chain));
	REQUIRE(inner != nullptr);
	CHECK(inner->macrosCollapsed());
	ui::ModState mod;
	ui::EditHistory history;
	rack::RackEditor window(*inner, mod, history);
	window.setBounds(0, 0, rack::RackEditor::kWidth, kEditorHeight);
	CHECK(window.macrosCollapsed()); // opened folded, from its first frame
	CHECK(window.laneLeft() == doctest::Approx(ui::Rail::kCollapsedWidth + 6.0f));

	// A LOAD UNDER THE OPEN WINDOW: the bridge unfolds, and the next beat (`syncMacros`) follows it — silently.
	window.notifyTrayForTest().say("");
	inner->setMacrosCollapsed(false);
	window.syncMacros();
	CHECK_FALSE(window.macrosCollapsed());
	CHECK(window.laneLeft() == rack::RackEditor::kLaneLeft);
	CHECK(window.notifierTextForTest().empty());
}

TEST_CASE("TFIELD in the rack: the card draws the ring at its terrain size, so zooming in draws it larger and Spread sizes it") {
	// THE DECOUPLING ON THE GLASS (2026-11-08): the ring is Spread zones of terrain, drawn through the zoom's scale. One
	// octave of zoom in doubles it on the glass; doubling Spread doubles it too; the points sit on it, around the puck.
	namespace tf = mod::twinfield;
	Fixture f;
	const DeviceInstanceId field = f.add(tf::kTraits.tag);
	REQUIRE(field != kNoInstance);
	f.editor.syncCards();
	f.editor.resized();
	auto* card = dynamic_cast<ui::TwinFieldCard*>(f.editor.cardOfDeviceForTest(field));
	REQUIRE(card != nullptr);
	ui::TwinFieldPad& pad = card->pad();
	REQUIRE(pad.width() > 10.0f);
	const auto ringAt = [&](float zoom, float spread) {
		f.bridge.setField(field, tf::layerParam(0, tf::LayerParam::Zoom), zoom);
		f.bridge.setField(field, tf::layerParam(0, tf::LayerParam::Spread), spread);
		f.bridge.setField(field, tf::layerParam(0, tf::LayerParam::Depth), 0.0f);
		const double half = std::max(std::min(pad.width(), pad.height()) * 0.5, 1.0);
		const double want = spread * 0.125 / (0.125 / std::exp2(zoom) / half); // restated: terrain radius / plane-per-pixel
		CHECK(std::abs(pad.ringPixels() - want) <= 0.01);
		// THE POINTS ARE ON IT: point 0 sits one ring east of the puck (Depth 0, no phase: the pan point).
		CHECK(std::abs(pad.pointPixel(0).x - (pad.puckPixel().x + want)) <= 0.01);
		return pad.ringPixels();
	};
	const float base = ringAt(0.0f, 0.6f);
	CHECK(std::abs(ringAt(1.0f, 0.6f) - 2.0f * base) <= 0.01f);  // zoom in one octave: twice as large on the glass
	CHECK(std::abs(ringAt(0.0f, 1.2f) - 2.0f * base) <= 0.01f);  // twice the Spread: twice as large
	CHECK(std::abs(ringAt(-1.0f, 1.2f) - base) <= 0.01f);        // and the two cancel
}
