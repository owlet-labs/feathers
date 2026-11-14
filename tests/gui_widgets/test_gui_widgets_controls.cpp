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

// THE EDITOR'S CONTROLS — knobs, the notify tray, the derivation clamp, the perform stage and its sky, the wave display, the sound stack, the order of the list, and the terrain. Lines 1–5819 of the one file, moved verbatim 2026-09-03.

#include <cxxabi.h>

#include "fixtures.h"

#include "plugin/ui/zone_select.h"
#include "plugin/ui/sigil.h"
#include "plugin/ui/settings_screen.h"

#include <bit>
#include <set>
#include "plugin/ui/macro_menu.h"
#include "plugin/ui/macro_polarity.h"
#include "plugin/ui/knob_tray.h"
#include "plugin/ui/master_meter.h"
#include "plugin/ui/value_box.h"
#include "plugin/ui/fader_pill.h"
#include "plugin/ui/route_list.h"
#include "fx/filter/filter_types.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/text_field.h"
#include "plugin/ui/macro_slot.h"
#include "plugin/ui/menu_anchor.h"
#include "../menu_placement_check.h"
#include "plugin/macro_modes.h"
#include "fx/automod/automod_types.h"
#include "fx/shaper/shaper_types.h"
#include "fx/eroder/eroder_types.h"
#include "fx/sklitter/sklitter_types.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/edit_history.h"
#include "plugin/synth.h"
#include "plugin/audition.h"



TEST_CASE("dragging a parameter row posts a bracketed gesture") {
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	WidgetRegistry registry;
	ParamRow row(bridge, registry, descOf(ParamId::Path));
	row.setBounds(0.0f, 0.0f, 200.0f, 40.0f);
	bridge.refresh();

	// Press at the middle, drag to three quarters, release — the shape of a real drag.
	row.processMouseDown(at(100.0f, 20.0f));
	row.processMouseDrag(at(150.0f, 20.0f));
	row.processMouseUp(at(150.0f, 20.0f));

	// Four, not three: the press itself sets a value before the drag moves it again. That is the
	// intended grammar — a click jumps the parameter to where it landed rather than waiting for a
	// drag — and it is worth pinning, because it is the difference between a control that responds
	// to a click and one that only responds to a drag.
	const std::vector<UiParamEvent> posted = drain(bridge);
	REQUIRE(posted.size() == 4);
	CHECK(posted[0].kind == UiParamEvent::Kind::GestureBegin);
	CHECK(posted[1].kind == UiParamEvent::Kind::Value);
	CHECK(posted[2].kind == UiParamEvent::Kind::Value);
	CHECK(posted[3].kind == UiParamEvent::Kind::GestureEnd);

	const ParamDesc& path = descOf(ParamId::Path);
	CHECK(posted[0].id == static_cast<clap_id>(ParamId::Path));
	// Half way along the row is half way along the range, and three quarters is three quarters.
	CHECK(posted[1].value == doctest::Approx(path.min + (path.max - path.min) * 0.5));
	CHECK(posted[2].value == doctest::Approx(path.min + (path.max - path.min) * 0.75));
}

TEST_CASE("a drag off either end of a row stays inside the parameter's range") {
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	WidgetRegistry registry;
	ParamRow row(bridge, registry, descOf(ParamId::Path));
	row.setBounds(0.0f, 0.0f, 200.0f, 40.0f);

	row.processMouseDown(at(-500.0f, 20.0f));
	row.processMouseDrag(at(9999.0f, 20.0f));
	row.processMouseUp(at(9999.0f, 20.0f));

	const ParamDesc& path = descOf(ParamId::Path);
	for (const UiParamEvent& event : drain(bridge)) {
		if (event.kind == UiParamEvent::Kind::Value) {
			CHECK(event.value >= path.min);
			CHECK(event.value <= path.max);
		}
	}
}

TEST_CASE("QH: the Engine rail's engine dropdown sets OscType, and the header no longer has an engine selector") {
	ParamBridge bridge;
	feathers::BridgeDeviceHost deviceHost { bridge };
	WidgetRegistry registry;
	NotifyTray tray;
	StageMode mode;
	EngineRail rail(bridge, registry, &tray);
	rail.setBounds(0.0f, 0.0f, EngineRail::kWidth, 700.0f);
	bridge.refresh();

	// IT IS THE RAIL'S FIRST CONTROL, over OscType, and its press opens the descriptor's list: all eight engines are on the
	// face of the menu before one is applied.
	ValueBox* box = rail.engineBoxForTest();
	REQUIRE(box != nullptr);
	CHECK(box->isVisible());
	CHECK(box->choiceMenuAvailable());
	CHECK(box->y() < 40.0f);
	CHECK(box->readout() == feathers::kOscTypeNames[static_cast<size_t>(std::lround(bridge.value(
	                            static_cast<clap_id>(ParamId::OscType))))]);

	// A PICK FROM THAT MENU, through the menu's own selection callback, writes ONE bracketed edit of OscType.
	visage::PopupMenu menu = box->choiceMenu();
	menu.onSelection().callback(3);
	const std::vector<UiParamEvent> posted = drain(bridge);
	REQUIRE(posted.size() == 3);
	CHECK(posted[0].kind == UiParamEvent::Kind::GestureBegin);
	CHECK(posted[1].kind == UiParamEvent::Kind::Value);
	CHECK(posted[1].id == static_cast<clap_id>(ParamId::OscType));
	CHECK(posted[1].value == doctest::Approx(3.0));
	CHECK(posted[2].kind == UiParamEvent::Kind::GestureEnd);

	CHECK(rail.labelForTest() == "ENGINE");

	// AND THE HEADER HAS NO ENGINE SELECTOR: a click at the right-hand end lands on no engine, only the mode switch.
	HeaderBar header(bridge, registry, mode);
	header.setBounds(0.0f, 0.0f, 708.0f, HeaderBar::kHeight);
	CHECK(header.modeBoundsForTest().right() == doctest::Approx(708.0f - 20.0f));
	(void)drain(bridge);
	header.onMouseDown().callback(at(700.0f, HeaderBar::kHeight * 0.5f));
	CHECK_FALSE(bridge.hasPendingWork());
}

TEST_CASE("backlog EJ: the header's UI scale readout — minus and plus step by one, Ctrl/Cmd-wheel steps by sign, "
          "a bare wheel and a header without the road swallow nothing") {
	ParamBridge bridge;
	feathers::BridgeDeviceHost deviceHost { bridge };
	WidgetRegistry registry;
	StageMode mode;
	HeaderBar header(bridge, registry, mode);
	header.setBounds(0.0f, 0.0f, 708.0f, HeaderBar::kHeight);
	bridge.refresh();

	// WITHOUT THE ROAD THERE IS NO WIDGET: the box is laid out (geometry is not conditional) but a click
	// where it would be reaches nothing and is not eaten — the header then behaves as it always did.
	const visage::Bounds box = header.uiScaleBounds();
	const visage::Point minus = { header.uiScaleMinusBounds().x() + 4.0f, box.y() + box.height() * 0.5f };
	const visage::Point plus = { header.uiScalePlusBounds().right() - 4.0f, box.y() + box.height() * 0.5f };
	const visage::Point readout = { box.x() + box.width() * 0.5f, box.y() + box.height() * 0.5f };
	CHECK(header.hitsUiScaleMinus(minus));
	CHECK(header.hitsUiScalePlus(plus));
	CHECK_FALSE(header.hitsUiScaleMinus(readout));
	CHECK_FALSE(header.hitsUiScalePlus(readout));
	// It sits LEFT of the mode switch and RIGHT of the wordmark, overlapping neither control.
	CHECK(box.right() < 708.0f);
	CHECK_FALSE(header.hitsMode(plus));

	std::vector<int> steps;
	header.onMouseDown().callback(at(minus.x, minus.y));
	header.onMouseDown().callback(at(plus.x, plus.y));
	CHECK(steps.empty());

	// WITH THE ROAD: each button is one step in its direction, the readout between them is not a button.
	header.uiScaleNow = [] { return 1.25; };
	header.onUiScaleStep = [&steps](int direction) { steps.push_back(direction); };
	header.onMouseDown().callback(at(minus.x, minus.y));
	header.onMouseDown().callback(at(plus.x, plus.y));
	header.onMouseDown().callback(at(plus.x, plus.y));
	header.onMouseDown().callback(at(readout.x, readout.y));
	REQUIRE(steps.size() == 3);
	CHECK(steps[0] == -1);
	CHECK(steps[1] == +1);
	CHECK(steps[2] == +1);

	// THE WHEEL IS THE KEYBOARD'S CHORD ON THE MOUSE: Ctrl/Cmd held, up is bigger. A bare wheel is not
	// ours — it is left for whoever scrolls — and a zero delta is nothing.
	steps.clear();
	visage::MouseEvent wheel = at(readout.x, readout.y);
	wheel.wheel_delta_y = 1.0f;
	wheel.precise_wheel_delta_y = 1.0f;
	CHECK_FALSE(header.onMouseWheel().callback(wheel));
	CHECK(steps.empty());
	wheel.modifiers = visage::kModifierRegCtrl;
	CHECK(header.onMouseWheel().callback(wheel));
	wheel.wheel_delta_y = -3.0f;
	wheel.precise_wheel_delta_y = -3.0f;
	CHECK(header.onMouseWheel().callback(wheel));
	wheel.wheel_delta_y = 0.0f;
	wheel.precise_wheel_delta_y = 0.0f;
	CHECK_FALSE(header.onMouseWheel().callback(wheel));
	wheel.modifiers = visage::kModifierCmd;
	wheel.wheel_delta_y = 0.5f;
	CHECK(header.onMouseWheel().callback(wheel));
	REQUIRE(steps.size() == 3);
	CHECK(steps[0] == +1);
	CHECK(steps[1] == -1);
	CHECK(steps[2] == +1);
}

TEST_CASE("the libra constellation sits centred in the header, inside it, clear of every header control and "
          "a door to the settings screen, at the minimum width and a wide one, at every rung of the UI scale "
          "ladder; squeezed narrower than that, it is centred and clear or not drawn") {
	// THE REAL EDITOR, not a header on its own: the preset browser is a SIBLING frame the editor lays over
	// the header, and only the editor wires the scale readout, so a bare header could not see either.
	ModFixture f;
	ui::HeaderBar* header = f.gui.headerForTest();
	ui::PresetBrowser* browser = f.gui.presetBrowserForTest();
	REQUIRE(header != nullptr);
	REQUIRE(browser != nullptr);
	REQUIRE(static_cast<bool>(header->uiScaleNow)); // the readout is drawn, so its box is a control

	const auto clickAt = [header](visage::Point p) {
		visage::MouseEvent e;
		e.position = p;
		header->onMouseDown().callback(e);
	};
	const auto centreOf = [](visage::Bounds b) {
		return visage::Point { b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f };
	};
	// From the bottom rung, whatever an earlier case in this process left in the prefs.
	for (int i = 0; i < 16 && f.gui.uiScaleForTest() > 1.0 + 1e-9; ++i) {
		clickAt(centreOf(header->uiScaleMinusBounds()));
	}
	REQUIRE(f.gui.uiScaleForTest() == doctest::Approx(1.0));

	int hidden = 0;
	const auto checkHere = [&](const std::string& where, bool mustShow) {
		CAPTURE(where);
		const double ui = f.gui.uiScaleForTest();
		CAPTURE(ui);
		const float w = header->width();
		const float h = header->height();
		CAPTURE(w);
		if (mustShow) {
			REQUIRE(w >= static_cast<float>(Gui::kLogicalWidth) - 1.0f); // the host granted the floor or more
		}
		const visage::Bounds logo = header->logoBounds();
		if (!mustShow && logo.width() <= 0.0f) {
			++hidden; // no room, so not drawn — and an empty box overlaps nothing
			return;
		}
		REQUIRE(logo.width() > 0.0f);
		REQUIRE(logo.height() > 0.0f);

		// CENTRED, in the units the claim is about: the NATIVE pixels the mark is drawn at, to a pixel or two.
		const float s = header->dpiScale();
		CHECK(s == doctest::Approx(f.gui.contentScaleForTest()));
		const float nativeCentreX = (logo.x() + logo.width() * 0.5f) * s;
		const float nativeCentreY = (logo.y() + logo.height() * 0.5f) * s;
		CHECK(std::abs(nativeCentreX - static_cast<float>(header->nativeWidth()) * 0.5f) <= 2.0f);
		CHECK(std::abs(nativeCentreY - static_cast<float>(header->nativeHeight()) * 0.5f) <= 2.0f);

		// INSIDE the strip, and above its bottom rule.
		CHECK(logo.x() >= 0.0f);
		CHECK(logo.right() <= w);
		CHECK(logo.y() >= 0.0f);
		CHECK(logo.bottom() <= h - 1.0f);

		// CLEAR OF EVERY CONTROL the header holds — its own three, and the browser laid over it. The
		// browser is placed in its parent's coordinates, so it is taken to the header's through the window.
		CHECK_FALSE(logo.overlaps(header->modeBoundsForTest()));
		CHECK_FALSE(logo.overlaps(header->uiScaleBounds()));
		const visage::Point offset = browser->positionInWindow() - header->positionInWindow();
		const visage::Bounds browserBox { offset.x, offset.y, browser->width(), browser->height() };
		CHECK_FALSE(logo.overlaps(browserBox));

		// A DOOR AND NOTHING ELSE (backlog LG — it was deaf until the settings screen hung off it): the frame
		// under the mark is the header itself (nothing laid over it), a click there lands on none of the
		// header's other hit boxes, so it moves no engine and no scale, and it OPENS THE SETTINGS SCREEN. The
		// screen is closed again so the next rung's checks see the header, not the scrim.
		const visage::Point mid = centreOf(logo);
		CHECK(f.root->frameAtPoint(header->positionInWindow() + mid) == header);
		CHECK(header->hitsSettings(mid));
		CHECK_FALSE(header->hitsMode(mid));
		CHECK_FALSE(header->hitsUiScaleMinus(mid));
		CHECK_FALSE(header->hitsUiScalePlus(mid));
		const double engine = f.bridge.value(static_cast<clap_id>(ParamId::OscType));
		ui::SettingsScreen* settings = f.gui.settingsForTest();
		REQUIRE(settings != nullptr);
		REQUIRE_FALSE(settings->isVisible());
		clickAt(mid);
		CHECK(settings->isVisible());
		CHECK(f.bridge.value(static_cast<clap_id>(ParamId::OscType)) == engine);
		CHECK(f.gui.uiScaleForTest() == doctest::Approx(ui));
		REQUIRE(static_cast<bool>(settings->onClose));
		settings->onClose();
		REQUIRE_FALSE(settings->isVisible());
	};

	int rungs = 0;
	for (int guard = 0; guard < 16; ++guard) { // bounded, so a known-bad that steps the scale cannot spin
		// The minimum the editor will accept at this scale, which is what a host that honours the ask gives.
		f.grantPreferredSize();
		checkHere("minimum width", true);
		const double cs = f.gui.contentScaleForTest();
		REQUIRE(f.gui.setSize(static_cast<uint32_t>(1600.0 * cs), static_cast<uint32_t>(Gui::kLogicalHeight * cs)));
		checkHere("1600 wide", true);
		++rungs;
		const double now = f.gui.uiScaleForTest();
		if (ui::PluginWindow::stepUiScale(now, +1) <= now + 1e-9) {
			break;
		}
		clickAt(centreOf(header->uiScalePlusBounds()));
		REQUIRE(f.gui.uiScaleForTest() > now);
	}
	// EVERY RUNG, counted against the ladder itself rather than a copy of its length.
	int ladder = 1;
	for (double s = 1.0; ui::PluginWindow::stepUiScale(s, +1) > s + 1e-9; s = ui::PluginWindow::stepUiScale(s, +1)) {
		++ladder;
	}
	CHECK(rungs == ladder);
	CHECK(rungs >= 2);

	for (int i = 0; i < 16 && f.gui.uiScaleForTest() > 1.0 + 1e-9; ++i) {
		clickAt(centreOf(header->uiScaleMinusBounds()));
	}
	REQUIRE(f.gui.uiScaleForTest() == doctest::Approx(1.0));

	// A HOST THAT CANNOT GIVE THE ROOM: the window keeps its 100 % size while the scale climbs, so the
	// logical width falls under the design width (1240 -> 620 at 200 %; xvfb's 1920x1080 screen lays 175 % out
	// 987 wide) and the right-hand cluster slides left over the middle. Every rung again, nothing granted.
	f.grantPreferredSize();
	int squeezedRungs = 0;
	for (int guard = 0; guard < 16; ++guard) {
		checkHere("squeezed: the 100 % window at this scale", false);
		++squeezedRungs;
		const double now = f.gui.uiScaleForTest();
		if (ui::PluginWindow::stepUiScale(now, +1) <= now + 1e-9) {
			break;
		}
		clickAt(centreOf(header->uiScalePlusBounds()));
		REQUIRE(f.gui.uiScaleForTest() > now);
	}
	CHECK(squeezedRungs == ladder);
	CHECK(header->width() < static_cast<float>(Gui::kLogicalWidth) * 0.6f); // it really was squeezed
	// NOT VACUOUS: the squeeze reached widths with no room, so the "not drawn" road was taken.
	CHECK(hidden > 0);

	// Back to 100 %, so the next case in this process starts where a fresh editor does.
	for (int i = 0; i < 16 && f.gui.uiScaleForTest() > 1.0 + 1e-9; ++i) {
		clickAt(centreOf(header->uiScaleMinusBounds()));
	}
}

TEST_CASE("the Engine rail's engine dropdown refuses an entry the parameter table does not have") {
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	WidgetRegistry registry;
	EngineRail rail(bridge, registry);
	rail.setBounds(0.0f, 0.0f, EngineRail::kWidth, 700.0f);

	// Nothing the menu offers carries these (the mock's "PhiWeave · soon" entry, which did carry -1, is
	// Resonate now and real); if one ever arrived anyway it must not write a value the engine has no
	// oscillator for.
	rail.selectEngine(-1);
	rail.selectEngine(99);
	CHECK_FALSE(bridge.hasPendingWork());
}

TEST_CASE("Engine rail: the engine menu offers Resonate and choosing it sets OscType 3") {
	ParamBridge bridge;
	feathers::BridgeDeviceHost deviceHost { bridge };
	WidgetRegistry registry;
	NotifyTray tray;
	EngineRail rail(bridge, registry, &tray);
	rail.setBounds(0.0f, 0.0f, EngineRail::kWidth, 700.0f);
	bridge.refresh();

	// The fourth engine is a real choice now, the menu's last entry, and it is written as one edit.
	REQUIRE(feathers::kOscTypeNames.size() == 8);
	CHECK(std::string(feathers::kOscTypeNames[4]) == "Basic Shapes");
	CHECK(std::string(feathers::kOscTypeNames[5]) == "Additive");
	CHECK(std::string(feathers::kOscTypeNames[6]) == "Pluck");
	CHECK(std::string(feathers::kOscTypeNames[7]) == "Drum");
	// The φ engines wear the prefix on the selector; Basic Shapes, Additive and Pluck do not.
	CHECK(EngineRail::engineDisplayName(2) == "PhiV5");
	CHECK(EngineRail::engineDisplayName(4) == "Basic Shapes");
	CHECK(EngineRail::engineDisplayName(5) == "Additive");
	CHECK(EngineRail::engineDisplayName(7) == "Drum"); // nor Drum (2026-11-05)
	CHECK(EngineRail::engineDisplayName(6) == "Pluck");
	CHECK(std::string(feathers::kOscTypeNames[3]) == "Resonate");
	rail.selectEngine(3);
	const std::vector<UiParamEvent> posted = drain(bridge);
	REQUIRE(posted.size() == 3);
	CHECK(posted[1].kind == UiParamEvent::Kind::Value);
	CHECK(posted[1].id == static_cast<clap_id>(ParamId::OscType));
	CHECK(posted[1].value == 3.0);
	CHECK(tray.text().find("PhiResonate") != std::string::npos);
}

TEST_CASE("a widget redraws when the engine moves it, through the registry") {
	// The header watches OscType from outside the parameter rows, which is the case the positional
	// lookup could not express. Here the registry is asked what cares about the index that moved.
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	WidgetRegistry registry;
	EngineRail rail(bridge, registry);
	rail.setBounds(0.0f, 0.0f, EngineRail::kWidth, 700.0f);
	ParamRow row(bridge, registry, descOf(ParamId::Path));
	row.setBounds(0.0f, 0.0f, 200.0f, 40.0f);
	bridge.refresh();

	ParamSet params;
	params.setBase(static_cast<clap_id>(ParamId::OscType), 1.0);
	bridge.publish(params);

	const std::span<const uint32_t> changed = bridge.refresh();
	REQUIRE(changed.size() == 1);
	CHECK(changed[0] == static_cast<uint32_t>(paramIndexOf(static_cast<clap_id>(ParamId::OscType))));
	// Nothing to assert about pixels without a window; what matters is that notify() reaches a
	// watcher registered from somewhere other than a row, and does not walk off the end.
	registry.notify(changed[0]);
	registry.notify(9999);
}

TEST_CASE("an edit made while the plugin is inactive survives the flush seam") {
	// The direction the whole request_flush arrangement exists for, and the one that had never been
	// exercised anywhere: the plugin is not processing, so nothing drains the queue until the host
	// answers a flush request. The stand-in for the binding is the same sequence paramsFlush runs —
	// drain the queue into ParamSet — and the assertion is that the value arrives and that the editor
	// had told the host there was something waiting.
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	WidgetRegistry registry;
	ParamRow row(bridge, registry, descOf(ParamId::AZone));
	row.setBounds(0.0f, 0.0f, 200.0f, 40.0f);
	bridge.refresh();

	ParamSet params;
	const clap_id zone = static_cast<clap_id>(ParamId::AZone);
	const double before = params.base(zone);

	row.processMouseDown(at(200.0f, 20.0f)); // hard right: the maximum
	row.processMouseUp(at(200.0f, 20.0f));

	// Nothing has reached the engine yet — there is no audio thread to take it.
	CHECK(params.base(zone) == doctest::Approx(before));
	// ...but the editor knows it is owed a flush, which is what makes it ask the host for one.
	CHECK(bridge.hasPendingWork());

	bridge.drain([&](const UiParamEvent& event) {
		if (event.kind == UiParamEvent::Kind::Value) {
			params.setBase(event.id, event.value);
		}
	});

	CHECK(params.base(zone) == doctest::Approx(descOf(ParamId::AZone).max));
	CHECK_FALSE(bridge.hasPendingWork());

	// And the round trip closes: what the engine now holds is what the editor shows.
	bridge.publish(params);
	bridge.refresh();
	CHECK(bridge.value(zone) == doctest::Approx(params.base(zone)));
}

TEST_CASE("backlog DL: the two trims that are decibels READ as decibels — the Shaper's input trim and automod's follower "
          "trim print a signed dB value, not a bare number") {
	// Both rows carried `Unit::Number` under a comment apologising that no dB unit existed, so a knob
	// labelled "Trim" read "+6.00" and the tooltip carried the unit. Bus Utility brought `Unit::Decibel`
	// (backlog DK) and these were its other two users, held back only because the migration was not
	// what that device needed. THE STORED FLOAT IS UNCHANGED — a unit is how a row is READ — so no state
	// version moves; `test_device_ranges` pins that the fingerprint does not see units. This pins the
	// reading through `formatValue`, the one road every value box takes.
	namespace shp = feathers::fx::shaper;
	namespace am = feathers::fx::automod;
	const device::ParamDesc& trim = shp::kParamDescs[static_cast<size_t>(shp::Param::Trim)];
	const device::ParamDesc& env = am::kParamDescs[static_cast<size_t>(am::Param::EnvTrim)];
	CHECK(std::string(trim.name) == "Shaper Trim");
	CHECK(std::string(env.name) == "Env");
	CHECK(trim.unit == device::Unit::Decibel);
	CHECK(env.unit == device::Unit::Decibel);

	// SIGNED, ONE DECIMAL, THE UNIT ON THE NUMBER — `Unit::Number` printed "+6.00" and "-18.00".
	CHECK(ui::formatValue(trim, 6.0) == "+6.0dB");
	CHECK(ui::formatValue(trim, 0.0) == "+0.0dB");
	CHECK(ui::formatValue(trim, -24.0) == "-24.0dB");
	CHECK(ui::formatValue(env, -18.0) == "-18.0dB");
	CHECK(ui::formatValue(env, 0.0) == "+0.0dB");
	// AND THE RANGES AND DEFAULTS ARE WHERE THEY WERE — the change is the reading, nothing else.
	CHECK(trim.min == doctest::Approx(-24.0));
	CHECK(trim.max == doctest::Approx(24.0));
	CHECK(trim.def == doctest::Approx(0.0));
	CHECK(env.min == doctest::Approx(-48.0));
	CHECK(env.max == doctest::Approx(0.0));
	CHECK(env.def == doctest::Approx(-18.0));
}

TEST_CASE("a segmented control picks an option and refuses a non-option") {
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	WidgetRegistry registry;
	SegmentedControl seg(bridge, registry, descOf(ParamId::VoiceMode), kVoiceModeNames);
	seg.setBounds(0.0f, 0.0f, 150.0f, 24.0f);
	bridge.refresh();

	// Three options across 150 logical pixels: 0..50 is Poly, 50..100 Mono, 100..150 Legato.
	CHECK(seg.segmentAt({25.0f, 12.0f}) == 0);
	CHECK(seg.segmentAt({75.0f, 12.0f}) == 1);
	CHECK(seg.segmentAt({125.0f, 12.0f}) == 2);
	CHECK(seg.segmentAt({-5.0f, 12.0f}) == -1);
	CHECK(seg.segmentAt({25.0f, 99.0f}) == -1);

	seg.processMouseDown(at(75.0f, 12.0f));
	const std::vector<UiParamEvent> posted = drain(bridge);
	REQUIRE(posted.size() == 3);
	CHECK(posted[1].id == static_cast<clap_id>(ParamId::VoiceMode));
	CHECK(posted[1].value == doctest::Approx(1.0));

	// Choosing what is already chosen posts nothing: an undo step that changes no sound is noise.
	seg.processMouseDown(at(75.0f, 12.0f));
	CHECK_FALSE(bridge.hasPendingWork());
}

TEST_CASE("a disabled segmented control ignores the mouse") {
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	WidgetRegistry registry;
	SegmentedControl seg(bridge, registry, descOf(ParamId::NotePriority), kNotePriorityNames);
	seg.setBounds(0.0f, 0.0f, 150.0f, 24.0f);
	bridge.refresh();

	seg.setEnabled(false);
	seg.processMouseDown(at(125.0f, 12.0f));
	CHECK_FALSE(bridge.hasPendingWork());

	// And takes it again when it means something.
	seg.setEnabled(true);
	seg.processMouseDown(at(125.0f, 12.0f));
	CHECK(bridge.hasPendingWork());
}

TEST_CASE("a stepper moves by one and stops at the ends") {
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	WidgetRegistry registry;
	const ParamDesc& count = descOf(ParamId::VoiceCount);
	Stepper stepper(bridge, registry, count);
	stepper.setBounds(0.0f, 0.0f, 100.0f, 24.0f);
	bridge.refresh();

	CHECK(stepper.buttonAt({5.0f, 12.0f}) == -1);
	CHECK(stepper.buttonAt({50.0f, 12.0f}) == 0);
	CHECK(stepper.buttonAt({95.0f, 12.0f}) == 1);

	const double before = bridge.value(static_cast<clap_id>(ParamId::VoiceCount));
	stepper.processMouseDown(at(95.0f, 12.0f));
	std::vector<UiParamEvent> posted = drain(bridge);
	REQUIRE(posted.size() == 3);
	CHECK(posted[1].value == doctest::Approx(before + 1.0));

	// The label between the buttons is not a button.
	stepper.processMouseDown(at(50.0f, 12.0f));
	CHECK_FALSE(bridge.hasPendingWork());

	// Walk to the top and try to go past it. The parameter's own maximum is the wall, and pushing at
	// the wall posts nothing rather than spending a gesture on no change.
	for (int i = 0; i < 40; ++i) {
		stepper.processMouseDown(at(95.0f, 12.0f));
		drain(bridge);
	}
	CHECK(bridge.value(static_cast<clap_id>(ParamId::VoiceCount)) == doctest::Approx(count.max));
	stepper.processMouseDown(at(95.0f, 12.0f));
	CHECK_FALSE(bridge.hasPendingWork());
}

TEST_CASE("the Engine rail greys out what the voice mode makes meaningless") {
	// The mock's behaviour, and it is honest rather than cosmetic: a voice count means nothing to a
	// monophonic synth, and a note priority means nothing when no note ever has to lose.
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	WidgetRegistry registry;
	EngineRail engineRail(bridge, registry);
	engineRail.setBounds(0.0f, 0.0f, EngineRail::kWidth, 400.0f);
	bridge.refresh();

	ParamSet params;
	const clap_id mode = static_cast<clap_id>(ParamId::VoiceMode);

	params.setBase(mode, static_cast<double>(VoiceMode::Poly));
	bridge.publish(params);
	bridge.refresh();
	engineRail.refreshEnablement();
	// Nothing to assert about pixels here; what is asserted is that the panel followed the parameter
	// rather than only its own clicks — it is driven from the published value either way.
	CHECK(bridge.value(mode) == doctest::Approx(static_cast<double>(VoiceMode::Poly)));

	params.setBase(mode, static_cast<double>(VoiceMode::Legato));
	bridge.publish(params);
	bridge.refresh();
	engineRail.refreshEnablement();
	CHECK(bridge.value(mode) == doctest::Approx(static_cast<double>(VoiceMode::Legato)));
}

TEST_CASE("the voice count stepper stops at the stack's CPU guard, and says what it is held to") {
	// Backlog KE: a stack of N sounds at most ⌊B / N⌋ notes (voices steal sooner). The counts above that are the
	// same sound, so by the honesty rule the stepper's plus stops there, and a setting above it reads as the
	// setting AND the limit — "16 (plays 5)" — rather than pretending either one away.
	ParamBridge bridge;
	feathers::BridgeDeviceHost deviceHost { bridge };
	WidgetRegistry registry;
	EngineRail engineRail(bridge, registry);
	engineRail.setBounds(0.0f, 0.0f, EngineRail::kWidth, 400.0f);
	bridge.refresh();

	const auto watchers = registry.watchers(static_cast<clap_id>(ParamId::VoiceCount));
	REQUIRE_FALSE(watchers.empty());
	auto* stepper = static_cast<ui::Stepper*>(watchers.front());
	const clap_id count = static_cast<clap_id>(ParamId::VoiceCount);

	// The limit restated from the rule, not asked of `stackVoiceLimit`: B / 8 whole notes.
	const int limit = feathers::kOscillatorBudget / 8;
	ParamSet params;
	params.setBase(static_cast<clap_id>(ParamId::StackCount), 8.0);
	params.setBase(count, static_cast<double>(limit));
	bridge.publish(params);
	bridge.refresh();
	engineRail.refreshEnablement();
	CHECK(stepper->ceiling() == doctest::Approx(static_cast<double>(limit)));

	// AT THE CEILING, THE PLUS CHANGES NOTHING — and posts nothing, so no host undo step for no sound.
	// (`value` shows a posted move at once, so it is read straight after the press, before any refresh.)
	const bool pendingBefore = bridge.hasPendingWork();
	stepper->nudge(1);
	CHECK(bridge.value(count) == doctest::Approx(static_cast<double>(limit)));
	CHECK(bridge.hasPendingWork() == pendingBefore);
	// The minus still works: fewer notes is a real change.
	stepper->nudge(-1);
	CHECK(bridge.value(count) == doctest::Approx(static_cast<double>(limit - 1)));

	// A SETTING ABOVE THE CEILING reads as both, with an arrow between.
	params.setBase(count, 16.0);
	bridge.publish(params);
	bridge.refresh();
	engineRail.refreshEnablement();
	const std::string reading = stepper->reading();
	CAPTURE(reading);
	CHECK(reading.find("16") != std::string::npos);
	CHECK(reading.find("(plays " + std::to_string(limit) + ")") != std::string::npos);
	// HELD ABOVE THE CAP, the plus does nothing and the minus steps from what is HEARD: 16 held at the limit,
	// pressed down, is limit − 1 — not 15, which would sound exactly like 16 (voice-stacking doc §7, assumption 8).
	stepper->nudge(1);
	CHECK(bridge.value(count) == doctest::Approx(16.0));
	stepper->nudge(-1);
	CHECK(bridge.value(count) == doctest::Approx(static_cast<double>(limit - 1)));
	params.setBase(count, 16.0); // back to the held setting for what follows
	bridge.publish(params);
	bridge.refresh();
	engineRail.refreshEnablement();

	// A STACK OF ONE HOLDS NOTHING: the ceiling is the whole range, and the reading is the plain count.
	params.setBase(static_cast<clap_id>(ParamId::StackCount), 1.0);
	bridge.publish(params);
	bridge.refresh();
	engineRail.refreshEnablement();
	CHECK(stepper->ceiling() == doctest::Approx(static_cast<double>(feathers::kMaxVoices)));
	CHECK(stepper->reading().find("plays") == std::string::npos);

	// AND STACK DETUNE IS LIVE EXACTLY WHEN THERE IS A FAN: at Stack 1 the one oscillator stands at fan position 0,
	// where the detune multiplies nothing, so its knob greys; at 2 it moves two parts apart, so it does not.
	ui::Knob* detune = nullptr;
	for (visage::Frame* w : registry.watchers(static_cast<clap_id>(ParamId::StackDetune))) {
		if (auto* knob = dynamic_cast<ui::Knob*>(w); knob != nullptr) {
			detune = knob;
		}
	}
	REQUIRE(detune != nullptr);
	CHECK_FALSE(detune->enabled());
	params.setBase(static_cast<clap_id>(ParamId::StackCount), 2.0);
	bridge.publish(params);
	bridge.refresh();
	engineRail.refreshEnablement();
	CHECK(detune->enabled());
}

TEST_CASE("the osc-mode row is live in every voice mode, and selects all three of its options") {
	// V21d: "enabling phase catchup is an Engine rail option: near voice retrig options", widened by
	// V24 into the three-way it is now. Unlike the voice count (meaningless in mono) and glide
	// (ignored in poly), what a note-on does to the phase matters in EVERY voice mode — poly steals
	// retrigger too — so the row never greys. And it is a real control: selecting a segment posts
	// the param, bracketed.
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	WidgetRegistry registry;
	EngineRail engineRail(bridge, registry);
	engineRail.setBounds(0.0f, 0.0f, EngineRail::kWidth, 400.0f);
	bridge.refresh();

	const auto watchers = registry.watchers(static_cast<clap_id>(ParamId::OscMode));
	REQUIRE_FALSE(watchers.empty());
	auto* row = static_cast<ui::SegmentedControl*>(watchers.front());

	ParamSet params;
	for (const VoiceMode mode : { VoiceMode::Poly, VoiceMode::Mono, VoiceMode::Legato }) {
		CAPTURE(static_cast<int>(mode));
		params.setBase(static_cast<clap_id>(ParamId::VoiceMode), static_cast<double>(mode));
		bridge.publish(params);
		bridge.refresh();
		engineRail.refreshEnablement();
		CHECK(row->enabled());
	}

	// The default is OURS (V21d's lean, carried through the widening): Phase Catchup, the last of
	// the three, NOT the zero index a widened enum would land on by accident.
	CHECK(bridge.value(static_cast<clap_id>(ParamId::OscMode))
	      == doctest::Approx(static_cast<double>(OscMode::Catchup)));

	// EVERY option is reachable and posts its own value — a three-segment row that could only ever
	// send two would pass a spot check of one.
	for (const OscMode mode : { OscMode::Digi, OscMode::Analog, OscMode::Catchup }) {
		CAPTURE(static_cast<int>(mode));
		row->select(static_cast<int>(mode));
		bool sawValue = false;
		bridge.drain([&](const UiParamEvent& event) {
			if (event.id == static_cast<clap_id>(ParamId::OscMode)
			    && event.kind == UiParamEvent::Kind::Value) {
				sawValue = event.value == static_cast<double>(mode);
			}
		});
		CHECK(sawValue);
	}
}

//-------//
// Knobs //
//-------//

// The knob's whole interaction is a vertical drag, so these aim at the two things a drag can get
// wrong and a screenshot cannot show: where the movement is measured FROM, and what it is measured
// AGAINST. Both were live bugs in this editor within the last day.

TEST_CASE("dragging a knob upward raises its parameter across the mock's travel") {
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	WidgetRegistry registry;
	const ParamDesc& path = descOf(ParamId::Path);
	auto knob_ptr = paramKnob(bridge, registry, &path, "Path");
	ui::Knob& knob = *knob_ptr;
	knob.setBounds(0.0f, 0.0f, 52.0f, Knob::kHeight);
	bridge.refresh();

	// Path starts at its minimum, so up is the only way it can go.
	knob.processMouseDown(at(26.0f, 26.0f));
	knob.processMouseDrag(at(26.0f, 26.0f - Knob::kDragRange * 0.5f));
	knob.processMouseUp(at(26.0f, 26.0f - Knob::kDragRange * 0.5f));

	const std::vector<UiParamEvent> posted = drain(bridge);
	REQUIRE(posted.size() == 3);
	CHECK(posted[0].kind == UiParamEvent::Kind::GestureBegin);
	CHECK(posted[1].kind == UiParamEvent::Kind::Value);
	CHECK(posted[2].kind == UiParamEvent::Kind::GestureEnd);
	// Half the travel is half the range. The press itself posts nothing, because a press that has
	// not moved is not an edit — unlike the row, where a click IS a jump to where it landed.
	CHECK(posted[1].value == doctest::Approx(path.min + (path.max - path.min) * 0.5));
}

TEST_CASE("a knob drag measures from the press, not from the previous event") {
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	WidgetRegistry registry;
	const ParamDesc& path = descOf(ParamId::Path);
	auto knob_ptr = paramKnob(bridge, registry, &path, "Path");
	ui::Knob& knob = *knob_ptr;
	knob.setBounds(0.0f, 0.0f, 52.0f, Knob::kHeight);
	bridge.refresh();

	// Out and back. If the drag accumulated per event instead of anchoring on the press, returning
	// to the press point would leave the value somewhere else entirely.
	knob.processMouseDown(at(26.0f, 60.0f));
	knob.processMouseDrag(at(26.0f, 20.0f));
	drain(bridge);
	// It really did move — otherwise the return trip below proves nothing.
	REQUIRE(bridge.value(static_cast<clap_id>(ParamId::Path)) > path.min);

	knob.processMouseDrag(at(26.0f, 40.0f));
	knob.processMouseDrag(at(26.0f, 60.0f));
	knob.processMouseUp(at(26.0f, 60.0f));
	drain(bridge);

	CHECK(bridge.value(static_cast<clap_id>(ParamId::Path)) == doctest::Approx(path.min));
}

TEST_CASE("a knob drag stops at both ends of its parameter's range") {
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	WidgetRegistry registry;
	const ParamDesc& path = descOf(ParamId::Path);
	auto knob_ptr = paramKnob(bridge, registry, &path, "Path");
	ui::Knob& knob = *knob_ptr;
	knob.setBounds(0.0f, 0.0f, 52.0f, Knob::kHeight);
	bridge.refresh();

	knob.processMouseDown(at(26.0f, 500.0f));
	knob.processMouseDrag(at(26.0f, -500.0f));
	knob.processMouseUp(at(26.0f, -500.0f));
	drain(bridge);
	CHECK(bridge.value(static_cast<clap_id>(ParamId::Path)) == doctest::Approx(path.max));

	knob.processMouseDown(at(26.0f, -500.0f));
	knob.processMouseDrag(at(26.0f, 500.0f));
	knob.processMouseUp(at(26.0f, 500.0f));
	drain(bridge);
	CHECK(bridge.value(static_cast<clap_id>(ParamId::Path)) == doctest::Approx(path.min));
}

TEST_CASE("the fine modifier makes the same movement smaller") {
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	WidgetRegistry registry;
	const ParamDesc& path = descOf(ParamId::Path);
	auto knob_ptr = paramKnob(bridge, registry, &path, "Path");
	ui::Knob& knob = *knob_ptr;
	knob.setBounds(0.0f, 0.0f, 52.0f, Knob::kHeight);
	bridge.refresh();

	knob.processMouseDown(fineAt(26.0f, 26.0f));
	knob.processMouseDrag(fineAt(26.0f, 26.0f - Knob::kDragRange * 0.5f));
	knob.processMouseUp(fineAt(26.0f, 26.0f - Knob::kDragRange * 0.5f));
	drain(bridge);

	const double span = path.max - path.min;
	CHECK(bridge.value(static_cast<clap_id>(ParamId::Path)) ==
	      doctest::Approx(path.min + span * 0.5 * Knob::kFineFactor));
}

TEST_CASE("backlog EG: a macro seen through its polarity — the view re-ranges the knob, its readout and its default while the parameter underneath keeps its shape") {
	// THE ADAPTER BOTH HOSTS BUILD THEIR MACRO KNOBS ON, asserted by value against the arithmetic it
	// declares (`toBipolar`, `toStored`), in both modes, with the mode flipped underneath it.
	ParamBridge bridge;
	const ParamDesc& macro = descOf(ParamId::Macro1);
	ParamControl inner(bridge, &macro);
	bool bipolar = false;
	BipolarMacro view(inner, [&bipolar] { return bipolar; });
	// THE VALUE ARRIVES FROM THE ENGINE, as it does in the plugin: a control reads what the bridge
	// last published, not what a hand posted toward the engine.
	{
		ParamSet params;
		params.setBase(static_cast<clap_id>(ParamId::Macro1), 0.75);
		bridge.publish(params);
		bridge.refresh();
	}

	// UNIPOLAR IS THE IDENTITY: the view is the control.
	CHECK(view.desc().min == doctest::Approx(0.0));
	CHECK(view.desc().max == doctest::Approx(1.0));
	CHECK(view.desc().def == doctest::Approx(macro.def));
	CHECK_FALSE(view.desc().centred);
	CHECK(view.value() == doctest::Approx(0.75));
	CHECK(view.text() == inner.text());
	CHECK(view.restNormalized() == doctest::Approx(0.0));
	CHECK(view.normalized() == doctest::Approx(0.75));

	// BIPOLAR RE-RANGES THE VIEW — -1..1, rest and default at the centre — and READS 2v - 1.
	bipolar = true;
	CHECK(view.desc().min == doctest::Approx(-1.0));
	CHECK(view.desc().max == doctest::Approx(1.0));
	CHECK(view.desc().def == doctest::Approx(0.0));
	CHECK(view.desc().centred);
	CHECK(std::string(view.desc().name) == std::string(macro.name));
	CHECK(view.value() == doctest::Approx(0.5));
	CHECK(view.normalized() == doctest::Approx(0.75)); // the same place on the sweep
	CHECK(view.restNormalized() == doctest::Approx(0.5)); // drawn from the middle now
	CHECK(view.text() == "+50%");
	CHECK(view.display().base == doctest::Approx(0.5));

	// A WRITE THROUGH THE VIEW LANDS ON THE PARAMETER AS 0..1: -0.5 seen is 0.25 stored.
	view.setValue(-0.5);
	drain(bridge);
	CHECK(bridge.value(static_cast<clap_id>(ParamId::Macro1)) == doctest::Approx(0.25));
	CHECK(view.value() == doctest::Approx(-0.5));
	CHECK(view.text() == "-50%");
	// AND CLAMPED IN THE VIEW'S RANGE before the map, so an overshoot lands at the edge it means.
	view.setValue(3.0);
	drain(bridge);
	CHECK(bridge.value(static_cast<clap_id>(ParamId::Macro1)) == doctest::Approx(1.0));

	// THE DEFAULT A DOUBLE-CLICK RETURNS TO IS THE CENTRE — "bipolar knobs which default to 0" — which
	// is the parameter's half way, and the shared reset verb lands there through the view.
	CHECK(view.resetToDefault());
	drain(bridge);
	CHECK(bridge.value(static_cast<clap_id>(ParamId::Macro1)) == doctest::Approx(0.5));
	CHECK(view.value() == doctest::Approx(0.0));
	CHECK(view.text() == "+0%");

	// FLIPPED BACK, THE SAME STORED VALUE READS AS ITSELF AGAIN.
	bipolar = false;
	CHECK(view.value() == doctest::Approx(0.5));
	CHECK(view.desc().def == doctest::Approx(macro.def));
}

TEST_CASE("backlog EG: the strip's macro knobs carry the polarity menu, and the switch is the bridge's verb") {
	ModFixture f;
	for (size_t i = 0; i < kMacroCount; ++i) {
		CAPTURE(i);
		ui::Knob* knob = f.panel->macroKnobForTest(i);
		REQUIRE(knob != nullptr);
		CHECK(knob->hasMacroMenuForTest());
	}
	// A KNOB THAT IS NOT A MACRO OFFERS NO SUCH MENU: the press goes on to its ordinary road.
	ui::Knob* path = f.knobFor(ParamId::Path);
	REQUIRE(path != nullptr);
	CHECK_FALSE(path->hasMacroMenuForTest());
	CHECK_FALSE(path->showMacroMenu());

	// THE VERB: what picking "Bipolar" in the menu does, called as a case can call it.
	(void)f.bridge.takeStateDirty();
	f.bridge.setMacroBipolar(2, true);
	CHECK(f.bridge.macroBipolar(2));
	CHECK(f.bridge.takeStateDirty());
	// DECLARED AND LIVE, BOTH — a macro's polarity is its mode, so the two readers a surface has agree.
	CHECK(sourceIsBipolar(f.bridge, backplane::Source::core(2)));
	CHECK(sourceIsBipolarNow(f.bridge, backplane::Source::core(2)));
	CHECK_FALSE(sourceIsBipolar(f.bridge, backplane::Source::core(1)));
	CHECK_FALSE(sourceIsBipolarNow(f.bridge, backplane::Source::core(1)));
}

namespace {

/// A MACRO MENU OVER PLAIN STATE, recording what each verb was handed — so a case asserts the value a
/// pick delivered and how many times, not merely that something moved.
struct MenuProbe {
	bool bipolar = false;
	device::MacroType type = device::MacroType::Knob;
	std::vector<bool> bipolarWrites;
	std::vector<device::MacroType> typeWrites;
	int renames = 0;

	ui::MacroMenuHooks hooks() {
		return {
			.readBipolar = [this] { return bipolar; },
			.writeBipolar = [this](bool b) { bipolarWrites.push_back(b); },
			.readType = [this] { return type; },
			.writeType = [this](device::MacroType t) { typeWrites.push_back(t); },
			.beginRename = [this] { ++renames; },
		};
	}
};

} // namespace

TEST_CASE("macro menu lists all three types and both polarities with the current one marked") {
	MenuProbe probe;
	probe.type = device::MacroType::Latch;
	probe.bipolar = true;
	CHECK(ui::macroMenuLabelsForTest(probe.hooks())
	      == std::vector<std::string> { "     Knob", "X  Latch", "     Momentary", "", "     Unipolar  0..100%",
	                                    "X  Bipolar  -100..100%", "", "Rename…" });

	probe.type = device::MacroType::Knob;
	probe.bipolar = false;
	CHECK(ui::macroMenuLabelsForTest(probe.hooks())
	      == std::vector<std::string> { "X  Knob", "     Latch", "     Momentary", "", "X  Unipolar  0..100%",
	                                    "     Bipolar  -100..100%", "", "Rename…" });

	// READ AT THE OPEN, NOT CACHED: the list is built from the hooks each time.
	probe.type = device::MacroType::Momentary;
	CHECK(ui::macroMenuLabelsForTest(probe.hooks())[2] == "X  Momentary");

	// AN INCOMPLETE SET IS NOT A MACRO: nothing listed, nothing shown, nothing written.
	ui::MacroMenuHooks partial = probe.hooks();
	partial.beginRename = nullptr;
	CHECK(ui::macroMenuLabelsForTest(partial).empty());
	visage::Frame anchor;
	CHECK_FALSE(ui::showMacroMenu(anchor, partial));
	ui::chooseMacroMenuEntry(partial, ui::kMacroMenuLatch);
	CHECK(probe.typeWrites.empty());
}

TEST_CASE("showMacroMenu opens on complete hooks and refuses incomplete ones") {
	MenuProbe probe;
	visage::Frame root;
	visage::Frame anchor;
	root.setBounds(0.0f, 0.0f, 400.0f, 300.0f);
	root.addChild(&anchor);
	anchor.setBounds(10.0f, 10.0f, 52.0f, 52.0f);

	// REFUSED: nothing is added under the root, so the press can go on to its ordinary road.
	ui::MacroMenuHooks partial = probe.hooks();
	partial.writeType = nullptr;
	CHECK_FALSE(ui::showMacroMenu(anchor, partial));
	CHECK(root.children().size() == 1);

	// OPENED: visage parents the popup to the TOP frame, so the root gains exactly one child.
	CHECK(ui::showMacroMenu(anchor, probe.hooks()));
	CHECK(root.children().size() == 2);
	// OPENING WRITES NOTHING — a menu is a question, not a pick.
	CHECK(probe.typeWrites.empty());
	CHECK(probe.bipolarWrites.empty());
	CHECK(probe.renames == 0);
}

TEST_CASE("choosing Latch writes the type") {
	MenuProbe probe;
	ui::chooseMacroMenuEntry(probe.hooks(), ui::kMacroMenuLatch);
	CHECK(probe.typeWrites == std::vector<device::MacroType> { device::MacroType::Latch });
	CHECK(probe.bipolarWrites.empty());
	CHECK(probe.renames == 0);

	// EACH TYPE ITS OWN ENTRY, so the other two are not the same verb under another name.
	ui::chooseMacroMenuEntry(probe.hooks(), ui::kMacroMenuMomentary);
	ui::chooseMacroMenuEntry(probe.hooks(), ui::kMacroMenuKnob);
	CHECK(probe.typeWrites
	      == std::vector<device::MacroType> { device::MacroType::Latch, device::MacroType::Momentary,
	                                          device::MacroType::Knob });

	// A DISMISSED MENU (-1) AND AN ID PAST THE LAST ENTRY DO NOTHING.
	ui::chooseMacroMenuEntry(probe.hooks(), -1);
	ui::chooseMacroMenuEntry(probe.hooks(), ui::kMacroMenuRename + 1);
	CHECK(probe.typeWrites.size() == 3);
	CHECK(probe.bipolarWrites.empty());
	CHECK(probe.renames == 0);
}

TEST_CASE("choosing Bipolar writes the polarity") {
	MenuProbe probe;
	ui::chooseMacroMenuEntry(probe.hooks(), ui::kMacroMenuBipolar);
	CHECK(probe.bipolarWrites == std::vector<bool> { true });
	ui::chooseMacroMenuEntry(probe.hooks(), ui::kMacroMenuUnipolar);
	CHECK(probe.bipolarWrites == std::vector<bool> { true, false });
	CHECK(probe.typeWrites.empty());
	CHECK(probe.renames == 0);
}

TEST_CASE("choosing Rename begins a rename") {
	MenuProbe probe;
	ui::chooseMacroMenuEntry(probe.hooks(), ui::kMacroMenuRename);
	CHECK(probe.renames == 1);
	CHECK(probe.typeWrites.empty());
	CHECK(probe.bipolarWrites.empty());
}

TEST_CASE("a macro knob in the header offers the macro menu") {
	ModFixture f;
	for (size_t i = 0; i < kMacroCount; ++i) {
		CAPTURE(i);
		ui::Knob* knob = f.panel->macroKnobForTest(i);
		REQUIRE(knob != nullptr);
		REQUIRE(knob->hasMacroMenuForTest());
		// THE KNOB'S OWN HOOKS READ THIS MACRO: a fresh bridge's Knob, unipolar.
		CHECK(ui::macroMenuLabelsForTest(knob->macroMenuForTest())[0] == "X  Knob");
	}
	// A PICK THROUGH THE KNOB'S HOOKS LANDS IN THE BRIDGE, on that macro and no other.
	ui::Knob* third = f.panel->macroKnobForTest(2);
	(void)f.bridge.takeStateDirty();
	ui::chooseMacroMenuEntry(third->macroMenuForTest(), ui::kMacroMenuLatch);
	CHECK(f.bridge.macroType(2) == device::MacroType::Latch);
	CHECK(f.bridge.macroType(1) == device::MacroType::Knob);
	CHECK(f.bridge.takeStateDirty());
	ui::chooseMacroMenuEntry(third->macroMenuForTest(), ui::kMacroMenuBipolar);
	CHECK(f.bridge.macroBipolar(2));
	CHECK_FALSE(f.bridge.macroBipolar(1));
	// AND THE MENU NOW SAYS SO — read back through the bridge, not remembered.
	CHECK(ui::macroMenuLabelsForTest(third->macroMenuForTest())
	      == std::vector<std::string> { "     Knob", "X  Latch", "     Momentary", "     Relative (encoder)", "", "     Unipolar  0..100%",
	                                    "X  Bipolar  -100..100%", "", "Rename…" });
	// RENAME OPENS THE SLOT'S FIELD (A7) — and opening it writes nothing: the name moves on Enter.
	ui::chooseMacroMenuEntry(third->macroMenuForTest(), ui::kMacroMenuRename);
	REQUIRE(f.panel->macroSlotForTest(2) != nullptr);
	CHECK(f.panel->macroSlotForTest(2)->renaming());
	CHECK_FALSE(f.bridge.macroRenamed(2));
	CHECK(f.bridge.macroType(2) == device::MacroType::Latch);
}

TEST_CASE("double-clicking a knob restores its parameter's default") {
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	WidgetRegistry registry;
	const ParamDesc& level = descOf(ParamId::VoiceLevel);
	auto knob_ptr = paramKnob(bridge, registry, &level, "Level");
	ui::Knob& knob = *knob_ptr;
	knob.setBounds(0.0f, 0.0f, 52.0f, Knob::kHeight);
	bridge.refresh();

	// Move it off the default first, or the reset would have nothing to undo — TOWARDS THE FAR END,
	// whichever end that is: Level's default is its floor since backlog EF (the default patch's
	// envelope carries it), and a drag downward from the floor goes nowhere.
	const float away = level.def < 0.5 * (level.min + level.max) ? -Knob::kDragRange * 0.5f
	                                                            : Knob::kDragRange * 0.5f;
	knob.processMouseDown(at(26.0f, 26.0f));
	knob.processMouseDrag(at(26.0f, 26.0f + away));
	knob.processMouseUp(at(26.0f, 26.0f + away));
	drain(bridge);
	REQUIRE(bridge.value(static_cast<clap_id>(ParamId::VoiceLevel)) != doctest::Approx(level.def));

	knob.processMouseDown(doubleClickAt(26.0f, 26.0f));
	const std::vector<UiParamEvent> posted = drain(bridge);
	REQUIRE(posted.size() == 3);
	CHECK(posted[0].kind == UiParamEvent::Kind::GestureBegin);
	CHECK(posted[1].value == doctest::Approx(level.def));
	CHECK(posted[2].kind == UiParamEvent::Kind::GestureEnd);

	// Resetting what is already default posts nothing.
	knob.processMouseDown(doubleClickAt(26.0f, 26.0f));
	CHECK_FALSE(bridge.hasPendingWork());
}

TEST_CASE("a dormant knob answers nothing") {
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	WidgetRegistry registry;
	const ParamDesc& relief = descOf(ParamId::Relief);

	// A parameter the engine does not read yet: it has a value, and refuses the mouse.
	auto dormant_ptr = paramKnob(bridge, registry, &relief, "Relief");
	ui::Knob& dormant = *dormant_ptr;
	dormant.setBounds(0.0f, 0.0f, 52.0f, Knob::kHeight);
	bridge.refresh();
	dormant.setEnabled(false);

	dormant.processMouseDown(at(26.0f, 26.0f));
	dormant.processMouseDrag(at(26.0f, 26.0f - Knob::kDragRange * 0.5f));
	dormant.processMouseUp(at(26.0f, 26.0f - Knob::kDragRange * 0.5f));
	CHECK_FALSE(bridge.hasPendingWork());

	dormant.processMouseDown(doubleClickAt(26.0f, 26.0f));
	CHECK_FALSE(bridge.hasPendingWork());

	// The same gesture on the same knob, once it means something, does post — otherwise the checks
	// above would pass on a knob that simply does nothing at all.
	dormant.setEnabled(true);
	dormant.processMouseDown(at(26.0f, 26.0f));
	dormant.processMouseDrag(at(26.0f, 26.0f - Knob::kDragRange * 0.5f));
	dormant.processMouseUp(at(26.0f, 26.0f - Knob::kDragRange * 0.5f));
	CHECK(bridge.hasPendingWork());
	drain(bridge);

	// And a knob with no parameter at all — the mock's Weather — has nothing to answer with.
	auto unbound_ptr = paramKnob(bridge, registry, nullptr, "Weather");
	ui::Knob& unbound = *unbound_ptr;
	unbound.setBounds(0.0f, 0.0f, 52.0f, Knob::kHeight);
	unbound.processMouseDown(at(26.0f, 26.0f));
	unbound.processMouseDrag(at(26.0f, 26.0f - Knob::kDragRange * 0.5f));
	unbound.processMouseUp(at(26.0f, 26.0f - Knob::kDragRange * 0.5f));
	CHECK_FALSE(bridge.hasPendingWork());
	CHECK(unbound.normalized() == doctest::Approx(0.0f));
}

TEST_CASE("the tray's master-scale knob follows the engine") {
	// V17 put the GLOBAL Width in the tray as its tenth knob; V12c says what it does on a frozen
	// engine: nothing, visibly. This is the dormant-knob discipline applied to an enablement that
	// is PATCH STATE rather than a build flag — the same drag refused on Stair and answered on V5,
	// through the one shared derivation (oscTypeFromValue), with the flip arriving via update()
	// exactly as the editor's tick delivers it. The V5 half is the control: without it the refusal
	// above would pass on a knob that simply answers nothing.
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	WidgetRegistry registry;
	ParamSet params;
	params.setBase(static_cast<clap_id>(ParamId::OscType), static_cast<double>(OscType::Stair));
	bridge.publish(params);
	bridge.refresh();

	KnobTray tray(bridge, registry, nullptr);
	tray.setBounds(0.0f, 0.0f, 700.0f, KnobTray::kHeight);
	// TEN SINCE V17, asked of the tray rather than restated here. A case that wrote the number down
	// would go on passing while disagreeing with the row it is testing.
	// NINE KNOBS, NOT TEN — gamma is a coordinate box now and `knobCount()` says what it says on the
	// tin. The ROW is still ten cells wide, which is the fact a layout cares about, so both are
	// checked: a count that quietly dropped a cell and a count that quietly reclassified one look
	// identical from either number alone.
	CHECK(tray.knobCount() == 8); // NINE CELLS since 2026-10-03: Attack and Release out, Voice Level in
	CHECK(tray.cellCount() == 9);

	// The width knob is the one the registry holds for its id — found the way the assembled-editor
	// cases find every knob, rather than by an index into a row whose order could change.
	const auto watchers = registry.watchers(static_cast<clap_id>(ParamId::Width));
	REQUIRE_FALSE(watchers.empty());
	visage::Frame* knob = watchers.front();

	// DOWNWARD, away from the clamp: the master scale ships at its maximum (V13b's default flip),
	// so an upward drag would post nothing even on a live knob and the refusal below would prove
	// the wrong thing.
	knob->processMouseDown(at(26.0f, 20.0f));
	knob->processMouseDrag(at(26.0f, 60.0f));
	knob->processMouseUp(at(26.0f, 60.0f));
	CHECK_FALSE(bridge.hasPendingWork());

	// The engine moves to the flagship — a host can do this — and the next tick's update() flips
	// the knob live: the same gesture posts.
	params.setBase(static_cast<clap_id>(ParamId::OscType), static_cast<double>(OscType::V5));
	bridge.publish(params);
	bridge.refresh();
	tray.update();

	knob->processMouseDown(at(26.0f, 20.0f));
	knob->processMouseDrag(at(26.0f, 60.0f));
	knob->processMouseUp(at(26.0f, 60.0f));
	CHECK(bridge.hasPendingWork());
	drain(bridge);

	// EVERY ENGINE, from the synth's stereo predicate as read (Synth's anyEnabledSoundWide arm): two ears are built
	// for V5, Additive and Basic Shapes (backlog KV), and for nothing else.
	auto* width = dynamic_cast<Knob*>(knob);
	REQUIRE(width != nullptr);
	for (const auto& [engine, live] : { std::pair { OscType::Morph, false }, std::pair { OscType::Stair, false },
	                                    std::pair { OscType::V5, true }, std::pair { OscType::Resonate, false },
	                                    std::pair { OscType::BasicShapes, true },
	                                    std::pair { OscType::Additive, true } }) {
		CAPTURE(static_cast<int>(engine));
		params.setBase(static_cast<clap_id>(ParamId::OscType), static_cast<double>(engine));
		bridge.publish(params);
		bridge.refresh();
		tray.update();
		CHECK(width->enabled() == live);
	}
}

TEST_CASE("a knob reads the published value rather than its own drag") {
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	WidgetRegistry registry;
	const ParamDesc& path = descOf(ParamId::Path);
	auto knob_ptr = paramKnob(bridge, registry, &path, "Path");
	ui::Knob& knob = *knob_ptr;
	knob.setBounds(0.0f, 0.0f, 52.0f, Knob::kHeight);
	bridge.refresh();

	// What the engine says, not what the mouse last did: automation and preset loads move the arc.
	ParamSet params;
	params.setBase(static_cast<clap_id>(ParamId::Path), path.min + (path.max - path.min) * 0.25);
	bridge.publish(params);
	bridge.refresh();
	CHECK(knob.normalized() == doctest::Approx(0.25f));
}

//-------------//
// Notify tray //
//-------------//

// The tray is a clock with a string attached, so it is tested as one: the fade is the behaviour,
// and waiting 1.4 real seconds per assertion would be a poor way to find out it works.

TEST_CASE("the notify tray holds a message, then fades it out") {
	int64_t now = 1000;
	NotifyTray tray([&now] { return now; });
	tray.setBounds(0.0f, 0.0f, 200.0f, NotifyTray::kHeight);

	// Nothing said, nothing shown — an editor that has just opened is not announcing anything.
	CHECK_FALSE(tray.visible());

	tray.say("Path 42.0 %");
	CHECK(tray.text() == "Path 42.0 %");
	CHECK(tray.opacity() == doctest::Approx(1.0f));

	// Fully lit for the whole hold.
	now += NotifyTray::kHoldMs - 1;
	CHECK(tray.opacity() == doctest::Approx(1.0f));

	// Then it fades, rather than vanishing between one frame and the next.
	now += 1 + NotifyTray::kFadeMs / 2;
	const float midFade = tray.opacity();
	CHECK(midFade < 1.0f);
	CHECK(midFade > 0.0f);

	now += NotifyTray::kFadeMs;
	CHECK(tray.opacity() == doctest::Approx(0.0f));
	CHECK_FALSE(tray.visible());
}

TEST_CASE("a new message restarts the tray rather than queueing behind the old one") {
	int64_t now = 1000;
	NotifyTray tray([&now] { return now; });
	tray.setBounds(0.0f, 0.0f, 200.0f, NotifyTray::kHeight);

	tray.say("Path 42.0 %");
	now += NotifyTray::kHoldMs - 10;
	tray.say("Gamma 0.35");

	// The second message is the one on screen, and it gets its own full hold: a drag posts a message
	// a frame and the readout must not start fading in the middle of one.
	CHECK(tray.text() == "Gamma 0.35");
	now += NotifyTray::kHoldMs - 1;
	CHECK(tray.opacity() == doctest::Approx(1.0f));
}

TEST_CASE("the notify tray asks to be redrawn only when it has changed") {
	int64_t now = 1000;
	NotifyTray tray([&now] { return now; });
	tray.setBounds(0.0f, 0.0f, 200.0f, NotifyTray::kHeight);

	// Idle and empty: nothing to redraw, however many frames go by. Without this the editor would
	// repaint this frame forever for the sake of a line of text that is not there.
	CHECK_FALSE(tray.update());
	now += 10000;
	CHECK_FALSE(tray.update());

	tray.say("Path 42.0 %");
	CHECK(tray.update()); // it appeared

	now += 10;
	CHECK_FALSE(tray.update()); // still held, still identical

	now += NotifyTray::kHoldMs;
	CHECK(tray.update()); // fading now, so the screen is out of date

	now += NotifyTray::kFadeMs * 4;
	CHECK(tray.update()); // gone
	CHECK_FALSE(tray.update()); // and stays gone without further repaints
}

TEST_CASE("a new message while held asks to be redrawn") {
	// The case the test above cannot see, and the bug it hid. Every message it holds is the same
	// message, so "held, so no repaint" and "the readout never updates during a drag" look identical
	// from inside it. A drag says something new every frame at an opacity that never leaves 1.0, so
	// the tray must ask for a repaint on the TEXT as well as on the clock.
	int64_t now = 1000;
	NotifyTray tray([&now] { return now; });
	tray.setBounds(0.0f, 0.0f, 200.0f, NotifyTray::kHeight);

	tray.say("Path 42.0 %");
	CHECK(tray.update());

	now += 16;
	tray.say("Path 43.5 %");
	CHECK(tray.opacity() == doctest::Approx(1.0f)); // mid-hold: the clock has nothing to report
	CHECK(tray.update());
	CHECK(tray.text() == "Path 43.5 %");

	// And having painted it, it settles again — one repaint per reading, not one per frame.
	now += 16;
	CHECK_FALSE(tray.update());

	// The same reading re-said is not a new reading. A knob that announces on every mouse event
	// while the value has not moved must not cost a frame.
	tray.say("Path 43.5 %");
	CHECK_FALSE(tray.update());
}

TEST_CASE("the Engine rail greys glide out in poly, where the engine ignores it") {
	// Not cosmetic, and not a guess about the mock: Synth::glideSecondsNow() returns 0 in Poly, so a
	// glide time set there changes no sound at all. The honesty rule says the control must say so.
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	WidgetRegistry registry;
	EngineRail engineRail(bridge, registry);
	engineRail.setBounds(0.0f, 0.0f, EngineRail::kWidth, 460.0f);
	bridge.refresh();

	ParamSet params;
	const clap_id mode = static_cast<clap_id>(ParamId::VoiceMode);

	params.setBase(mode, static_cast<double>(VoiceMode::Poly));
	bridge.publish(params);
	bridge.refresh();
	engineRail.refreshEnablement();
	CHECK_FALSE(engineRail.glideEnabled());

	params.setBase(mode, static_cast<double>(VoiceMode::Mono));
	bridge.publish(params);
	bridge.refresh();
	engineRail.refreshEnablement();
	CHECK(engineRail.glideEnabled());

	// Fine tune is in the pitch sum unconditionally, so it is live in every mode.
	CHECK(engineRail.fineEnabled());
	params.setBase(mode, static_cast<double>(VoiceMode::Poly));
	bridge.publish(params);
	bridge.refresh();
	engineRail.refreshEnablement();
	CHECK(engineRail.fineEnabled());
}

TEST_CASE("the Engine rail's engine dropdown says what it switched to") {
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	WidgetRegistry registry;
	NotifyTray tray;
	EngineRail rail(bridge, registry, &tray);
	rail.setBounds(0.0f, 0.0f, EngineRail::kWidth, 700.0f);
	bridge.refresh();

	rail.selectEngine(1);
	CHECK(tray.visible());
	CHECK(tray.text().find("PhiStair") != std::string::npos);
}

TEST_CASE("a vertical segmented control divides its height, not its width") {
	// The mock stacks the Engine rail's segments — `.insp-sec .seg { flex-direction: column }` — while
	// leaving the header's mode switch in a row, so the orientation is a property of the control and
	// the hit test has to follow it. A vertical control that still divided by width would put every
	// click in the first segment.
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	WidgetRegistry registry;
	SegmentedControl seg(bridge, registry, descOf(ParamId::VoiceMode), kVoiceModeNames);
	seg.setVertical(true);
	seg.setBounds(0.0f, 0.0f, 144.0f, 72.0f); // three options, 24 logical pixels each

	CHECK(seg.segmentAt({72.0f, 4.0f}) == 0);
	CHECK(seg.segmentAt({72.0f, 36.0f}) == 1);
	CHECK(seg.segmentAt({72.0f, 68.0f}) == 2);
	// Outside is still outside, in the axis that is now the long one and the one that is not.
	CHECK(seg.segmentAt({72.0f, -1.0f}) == -1);
	CHECK(seg.segmentAt({72.0f, 73.0f}) == -1);
	CHECK(seg.segmentAt({-1.0f, 36.0f}) == -1);
}

//----------------------//
// The derivation clamp //
//----------------------//

TEST_CASE("visage's derived scale is only believed when the X server does not contradict it") {
	// The case that motivated it: this display reports 1920px across 508mm, which is 96 DPI, while
	// visage derives 1.75 from the same panel and opens the editor at 1856x1048 on an ordinary
	// 1080p screen.
	CHECK(clampDerivedScale(1.75, 96.0) == doctest::Approx(1.0));
	CHECK(clampDerivedScale(1.75, 72.0) == doctest::Approx(1.0));

	// A display that says it really is dense keeps its scale.
	CHECK(clampDerivedScale(1.75, 168.0) == doctest::Approx(1.75));
	CHECK(clampDerivedScale(2.0, 192.0) == doctest::Approx(2.0));

	// Nothing is ever scaled UP by the clamp, and a display that cannot be asked is left alone —
	// answering 0 means "no opinion", not "96".
	CHECK(clampDerivedScale(0.5, 96.0) == doctest::Approx(0.5));
	CHECK(clampDerivedScale(1.75, 0.0) == doctest::Approx(1.75));
}

TEST_CASE("content scale must not rise just because both axes grew past the design canvas") {
	// contentScaleFor is min(native/design, preferred). With preferred=2 (a Retina backing
	// scale, wrongly treated as a zoom request) a window that grew on BOTH axes — or on one
	// and then the other — has both ratios > 1, so the min becomes > 1 and the logical canvas
	// snaps back to 1240x700 while the window keeps growing. Preferred=1 keeps the scale at
	// the floor and the layout spreads into the extra room, which is the single-axis behaviour.
	constexpr double kW = 1240.0;
	constexpr double kH = 700.0;
	const double grownW = 1800.0 / kW;
	const double grownH = 1000.0 / kH;
	CHECK(grownW > 1.0);
	CHECK(grownH > 1.0);
	CHECK(std::min({grownW, grownH, 2.0}) > 1.0);
	CHECK(std::min({grownW, grownH, 1.0}) == doctest::Approx(1.0));
	CHECK(std::min({grownW, kH / kH, 2.0}) == doctest::Approx(1.0));
}

//---------------//
// Perform stage //
//---------------//

// The stage is geometry over one parameter, so it is tested as geometry over one parameter: where
// the puck IS for a given Path, what a drag posts, and what it must never post. No window, no GPU —
// the visuals are checked by rendering them, which no assertion here pretends to do.


TEST_CASE("the puck is where Path says, and is not stored anywhere else") {
	StageFixture f;
	const BlendPoint a = f.placeOf(0);
	const BlendPoint b = f.placeOf(1);

	// Path is the tour parameter: 0 is the first sound, 1 is the last, and the engine's blend agrees —
	// Character::xf IS this value, which is what makes the picture honest rather than decorative. The
	// anchors come from the PATCH now, not from constants in this file, which is the whole of the fix.
	f.setPath(0.0);
	CHECK(f.stage.puck().x == doctest::Approx(a.x));
	CHECK(f.stage.puck().y == doctest::Approx(a.y));

	f.setPath(1.0);
	CHECK(f.stage.puck().x == doctest::Approx(b.x));
	CHECK(f.stage.puck().y == doctest::Approx(b.y));

	f.setPath(0.5);
	CHECK(f.stage.puck().x == doctest::Approx((a.x + b.x) * 0.5f));
	CHECK(f.stage.puck().y == doctest::Approx((a.y + b.y) * 0.5f));

	// AND POS X/Y MOVE IT OFF THE TOUR, which they could not do while they reached nothing. A puck
	// pinned to the line would now be showing a position the engine is not standing at.
	f.set(ParamId::PosX, 0.2);
	f.set(ParamId::PosY, -0.1);
	CHECK(f.stage.puck().x == doctest::Approx((a.x + b.x) * 0.5f + 0.2f));
	CHECK(f.stage.puck().y == doctest::Approx((a.y + b.y) * 0.5f - 0.1f));
}

TEST_CASE("the stage draws the constellation the patch has") {
	// THE BUG THIS REPLACES: placements() returned two hard-coded points, so a sound switched on was
	// inaudible to the eye — the surface drew two stars whatever the patch said, and an audition of a
	// three-sound blend would have been conducted on a picture of a two-sound one.
	StageFixture f;
	REQUIRE(f.stage.stars().size() == 2); // A and B, the default patch

	f.set(feathers::soundParam(2, feathers::SoundField::Enable), 1.0);
	REQUIRE(f.stage.stars().size() == 3);
	CHECK(f.stage.stars()[2].label == 'C');
	CHECK(f.stage.stars()[2].at.x == doctest::Approx(f.placeOf(2).x));

	// Switching one OFF removes it, and the survivors keep their own identities rather than sliding
	// down into the gap — a star that changed colour because a different sound was disabled would be
	// the surface renaming somebody's sound.
	f.set(feathers::soundParam(0, feathers::SoundField::Enable), 0.0);
	const std::vector<PerformStage::Star> live = f.stage.stars();
	REQUIRE(live.size() == 2);
	CHECK(live[0].label == 'B');
	CHECK(live[1].label == 'C');
	CHECK(live[0].slot == 1);
	CHECK(live[1].slot == 2);

	// And a moved placement moves the star, which is what makes the arrangement worth looking at.
	f.set(feathers::soundParam(2, feathers::SoundField::PlaceX), -0.9);
	CHECK(f.stage.stars()[1].at.x == doctest::Approx(-0.9f));
}

TEST_CASE("the tour walks every live sound, in slot order") {
	// The route on screen has to be the route Path walks, or the dashed line is decoration. Three
	// sounds: Path 0.5 lands exactly on the middle one, because the engine's tour gives each leg an
	// equal share of the knob rather than an equal share of the distance.
	StageFixture f;
	f.set(feathers::soundParam(2, feathers::SoundField::Enable), 1.0);
	f.set(feathers::soundParam(0, feathers::SoundField::PlaceX), -0.8);
	f.set(feathers::soundParam(0, feathers::SoundField::PlaceY), 0.0);
	f.set(feathers::soundParam(1, feathers::SoundField::PlaceX), 0.0);
	f.set(feathers::soundParam(1, feathers::SoundField::PlaceY), 0.5);
	f.set(feathers::soundParam(2, feathers::SoundField::PlaceX), 0.8);
	f.set(feathers::soundParam(2, feathers::SoundField::PlaceY), 0.0);

	f.setPath(0.5);
	CHECK(f.stage.puck().x == doctest::Approx(0.0f));
	CHECK(f.stage.puck().y == doctest::Approx(0.5f)); // ON sound B, not halfway across the chord

	f.setPath(0.25);
	CHECK(f.stage.puck().x == doctest::Approx(-0.4f)); // halfway along the FIRST leg
	CHECK(f.stage.puck().y == doctest::Approx(0.25f));
}

TEST_CASE("dragging the puck moves Path and the residual it is standing off by") {
	// THIS TEST USED TO ASSERT THE OPPOSITE, and the inversion is the engine's doing rather than a
	// change of taste. v1 dropped the mock's Pos X/Y residual because those parameters reached nothing
	// — a puck sliding off the line would have shown a mixture nobody was hearing. Phase 2 reads them:
	// they move the puck, which moves the weights and the travel. So keeping the drag on the line is
	// now the dishonest choice, and the hand going one way while the sound goes another is the failure
	// this replaces.
	StageFixture f;
	f.setPath(0.0);
	// The two sounds placed DIAGONALLY, so the perpendicular offset below has a component on both
	// axes. With the table's default anchors the tour is horizontal, the residual is purely vertical,
	// and Pos X correctly does not move at all — which makes it a fine patch and a poor fixture: the
	// case would be asserting a coincidence of the defaults rather than the decomposition.
	f.set(feathers::soundParam(0, feathers::SoundField::PlaceX), -0.5);
	f.set(feathers::soundParam(0, feathers::SoundField::PlaceY), -0.4);
	f.set(feathers::soundParam(1, feathers::SoundField::PlaceX), 0.5);
	f.set(feathers::soundParam(1, feathers::SoundField::PlaceY), 0.4);

	// Straight out from the middle of the tour, at right angles to it. The projection back onto the
	// tour is exactly the middle whatever the offset is, so Path's value is not a number to look up —
	// the assertion is about the decomposition rather than about arithmetic.
	const BlendPoint a = f.placeOf(0);
	const BlendPoint b = f.placeOf(1);
	const BlendPoint middle { (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f };
	const float alongX = b.x - a.x;
	const float alongY = b.y - a.y;
	const float length = std::hypot(alongX, alongY);
	const BlendPoint offTour { middle.x - alongY / length * 0.3f, middle.y + alongX / length * 0.3f };

	const visage::Point pixels = f.stage.toPixels(offTour);
	f.stage.processMouseDown(at(pixels.x, pixels.y));
	f.stage.processMouseUp(at(pixels.x, pixels.y));

	// Three parameters moved, each bracketed by its own gesture — a host handed an unmatched begin
	// holds an undo step open forever, so the pairing is the part worth checking rather than the count.
	const std::vector<UiParamEvent> posted = drain(f.bridge);
	std::map<clap_id, int> begins;
	std::map<clap_id, int> ends;
	std::map<clap_id, double> values;
	for (const UiParamEvent& event : posted) {
		if (event.kind == UiParamEvent::Kind::GestureBegin) {
			begins[event.id]++;
		}
		else if (event.kind == UiParamEvent::Kind::GestureEnd) {
			ends[event.id]++;
		}
		else {
			values[event.id] = event.value;
		}
	}
	for (const ParamId id : { ParamId::Path, ParamId::PosX, ParamId::PosY }) {
		CAPTURE(static_cast<uint32_t>(id));
		CHECK(begins[static_cast<clap_id>(id)] == 1);
		CHECK(ends[static_cast<clap_id>(id)] == 1);
	}

	CHECK(values[static_cast<clap_id>(ParamId::Path)] == doctest::Approx(0.5));
	// AND THE RESIDUAL IS THE REST OF THE WAY, which is what makes the puck land under the hand: the
	// offset was perpendicular to the tour and 0.3 long, so that is what Pos X/Y have to carry.
	const double residualX = values[static_cast<clap_id>(ParamId::PosX)];
	const double residualY = values[static_cast<clap_id>(ParamId::PosY)];
	CHECK(residualX == doctest::Approx(static_cast<double>(offTour.x - middle.x)));
	CHECK(residualY == doctest::Approx(static_cast<double>(offTour.y - middle.y)));
	CHECK(std::hypot(residualX, residualY) == doctest::Approx(0.3).epsilon(1e-4));

	// AND THE PUCK ENDS UP UNDER THE HAND, which is the claim all of the above is in service of. If
	// the decomposition were wrong in a way that still posted three plausible numbers, this is what
	// would notice.
	f.set(ParamId::PosX, residualX);
	f.set(ParamId::PosY, residualY);
	f.setPath(values[static_cast<clap_id>(ParamId::Path)]);
	CHECK(f.stage.puck().x == doctest::Approx(offTour.x));
	CHECK(f.stage.puck().y == doctest::Approx(offTour.y));
}

TEST_CASE("the puck stops at the sounds rather than running past them") {
	StageFixture f;
	f.setPath(0.5);

	// Along the tour, past B. The tour ends at the sound: Path is 0..1 and there is nothing beyond
	// the last anchor to walk to.
	const BlendPoint a = f.placeOf(0);
	const BlendPoint b = f.placeOf(1);
	const float alongX = b.x - a.x;
	const float alongY = b.y - a.y;
	const float length = std::hypot(alongX, alongY);
	const BlendPoint beyond { b.x + alongX / length * 0.3f, b.y + alongY / length * 0.3f };
	const visage::Point pixels = f.stage.toPixels(beyond);
	f.stage.processMouseDown(at(pixels.x, pixels.y));
	f.stage.processMouseUp(at(pixels.x, pixels.y));

	const std::vector<UiParamEvent> posted = drain(f.bridge);
	REQUIRE(posted.size() >= 2);
	CHECK(posted[1].value == doctest::Approx(1.0));
}

TEST_CASE("a drag along the tour tracks it, and a press that moves nothing posts nothing") {
	StageFixture f;
	f.setPath(0.0);

	const visage::Point start = f.stage.toPixels(f.placeOf(0));
	const visage::Point middle = f.stage.toPixels(f.stage.pathPos(0.5));
	const visage::Point end = f.stage.toPixels(f.placeOf(1));

	// Pressing exactly where the puck already is: the value does not move, so no gesture opens and
	// the host records no edit at all.
	f.stage.processMouseDown(at(start.x, start.y));
	CHECK(drain(f.bridge).empty());

	f.stage.processMouseDrag(at(middle.x, middle.y));
	f.stage.processMouseDrag(at(end.x, end.y));
	f.stage.processMouseUp(at(end.x, end.y));

	const std::vector<UiParamEvent> posted = drain(f.bridge);
	REQUIRE(posted.size() == 4);
	CHECK(posted[0].kind == UiParamEvent::Kind::GestureBegin);
	CHECK(posted[1].value == doctest::Approx(0.5));
	CHECK(posted[2].value == doctest::Approx(1.0));
	CHECK(posted[3].kind == UiParamEvent::Kind::GestureEnd);

	// And it said so, in the readout, in the units the parameter prints.
	CHECK(f.tray.visible());
	CHECK(f.tray.text().find("Path") != std::string::npos);
}

TEST_CASE("the circle is the control, and a drag may leave it") {
	StageFixture f;
	f.setPath(0.5);

	// The stage frame is twice as wide as the circle in it. A press in that dead space is not a press
	// on a control, and must not move the sound — which is a thing to check rather than assume,
	// because the frame is what receives the event.
	const visage::Point centre = f.stage.diskCentre();
	CHECK(f.stage.hitsDisk(centre));
	CHECK_FALSE(f.stage.hitsDisk({ centre.x + f.stage.diskRadius() + 40.0f, centre.y }));

	f.stage.processMouseDown(at(centre.x + f.stage.diskRadius() + 40.0f, centre.y));
	f.stage.processMouseUp(at(centre.x + f.stage.diskRadius() + 40.0f, centre.y));
	CHECK(drain(f.bridge).empty());

	// But a drag that STARTED on the circle keeps tracking when the hand runs off it, and whatever it
	// tracks to is still a value the parameter can hold.
	f.stage.processMouseDown(at(centre.x, centre.y));
	f.stage.processMouseDrag(at(9000.0f, 9000.0f));
	f.stage.processMouseDrag(at(-4000.0f, -4000.0f));
	f.stage.processMouseUp(at(-4000.0f, -4000.0f));

	const std::vector<UiParamEvent> posted = drain(f.bridge);
	CHECK_FALSE(posted.empty());
	// A range check, which is all this is: it says the clamp holds off the edge of the circle. Against
	// EACH PARAMETER'S OWN declared range rather than against 0..1 — a drag now posts the bipolar Pos
	// X/Y beside Path, and hard-coding one range here would be this test asserting the parameter table
	// it does not own. It caught a real thing when the residual landed: an unclamped drag to the corner
	// of the window posted 32.
	for (const UiParamEvent& event : posted) {
		if (event.kind != UiParamEvent::Kind::Value) {
			continue;
		}
		const ParamDesc* desc = findParam(event.id);
		REQUIRE(desc != nullptr);
		CAPTURE(std::string(desc->name));
		CHECK(event.value >= desc->min);
		CHECK(event.value <= desc->max);
	}
}

TEST_CASE("host modulation puts the reticle and its anchor in two places") {
	StageFixture f;

	// Nothing modulating: one state, and no anchor ring to draw.
	f.setPath(0.25);
	CHECK_FALSE(f.stage.showsModulation());
	CHECK(f.stage.puck().x == doctest::Approx(f.stage.puckBase().x));

	// The host pushes Path up: the puck rides the modulated reading — which is what the engine is
	// running on — and the base keeps the anchor where the hand left it.
	f.setPath(0.25, 0.75);
	CHECK(f.stage.showsModulation());
	CHECK(f.stage.puck().x == doctest::Approx(f.stage.pathPos(0.75).x));
	CHECK(f.stage.puckBase().x == doctest::Approx(f.stage.pathPos(0.25).x));
}

TEST_CASE("the stage animates on its own, and stops dead under reduced motion") {
	{
		StageFixture f;
		REQUIRE_FALSE(f.stage.reducedMotion());

		// Ambient motion is always on while the editor is visible: the sky turns with nobody touching
		// anything, so every frame is a frame the screen is out of date.
		CHECK(f.stage.update());
		f.now += 16;
		CHECK(f.stage.update());
		CHECK(f.stage.shimmer() > 0.0);

		// A frame the host lost — a stalled callback, a window being dragged — advances the world by
		// the mock's 50ms clamp rather than by the whole gap.
		const double before = f.stage.shimmer();
		f.now += 5000;
		f.stage.update();
		CHECK(f.stage.shimmer() - before == doctest::Approx(0.05));
	}

	{
		const ScopedEnv reduced("FEATHERS_REDUCED_MOTION", "1");
		StageFixture f;
		REQUIRE(f.stage.reducedMotion());

		// The mock's contract exactly: idle repaints stop, and the clock they were driving stops with
		// them. Everything else on this surface still moves, because everything else only moves when
		// the user or the host moves it.
		CHECK_FALSE(f.stage.update());
		f.now += 16;
		CHECK_FALSE(f.stage.update());
		CHECK(f.stage.shimmer() == doctest::Approx(0.0));
	}
}

TEST_CASE("the modulation trail grows while the host drives Path, and not otherwise") {
	{
		StageFixture f;
		f.setPath(0.2, 0.2);
		for (int frame = 0; frame < 10; ++frame) {
			f.now += 16;
			f.stage.update();
		}
		// A hand-driven puck leaves no trail: the trail is what modulation looks like.
		CHECK(f.stage.trailLength() == 0);

		f.setPath(0.2, 0.6);
		for (int frame = 0; frame < 10; ++frame) {
			f.now += 16;
			f.stage.update();
		}
		CHECK(f.stage.trailLength() == 10);

		// It is bounded — the mock's 46 points — however long the modulation runs.
		for (int frame = 0; frame < 200; ++frame) {
			f.now += 16;
			f.stage.update();
		}
		CHECK(f.stage.trailLength() == PerformStage::kTrailLength);

		// And it retracts a point at a time when the modulation stops, rather than blinking out.
		f.setPath(0.2, 0.2);
		f.now += 16;
		f.stage.update();
		CHECK(f.stage.trailLength() == PerformStage::kTrailLength - 1);
	}

	{
		const ScopedEnv reduced("FEATHERS_REDUCED_MOTION", "1");
		StageFixture f;
		f.setPath(0.2, 0.6);
		for (int frame = 0; frame < 10; ++frame) {
			f.now += 16;
			f.stage.update();
		}
		CHECK(f.stage.trailLength() == 0);
	}
}

TEST_CASE("the blend circle is the largest disk the stage can hold") {
	StageFixture f;

	// The mock's `R = s/2 - 8` over the shorter side, centred: a stage twice as wide as it is tall
	// still holds a circle, not an ellipse.
	CHECK(f.stage.diskRadius() == doctest::Approx(560.0f * 0.5f - PerformStage::kRimInset));
	CHECK(f.stage.diskCentre().x == doctest::Approx(536.0f));
	CHECK(f.stage.diskCentre().y == doctest::Approx(280.0f));

	// Blend coordinates and pixels are inverses of each other, which every hit test depends on.
	const BlendPoint p { 0.37f, -0.61f };
	const BlendPoint back = f.stage.fromPixels(f.stage.toPixels(p));
	CHECK(back.x == doctest::Approx(p.x));
	CHECK(back.y == doctest::Approx(p.y));

	// The rim is where the mock puts it, and the centre of the frame is the centre of the disk.
	CHECK(f.stage.toPixels({ 0.0f, 0.0f }).x == doctest::Approx(f.stage.diskCentre().x));
	CHECK(f.stage.toPixels({ 1.0f, 0.0f }).x
	      == doctest::Approx(f.stage.diskCentre().x + f.stage.diskRadius()));
}

TEST_CASE("the wake is left on the shimmer clock, and cleared when it expires") {
	StageFixture f;
	f.setPath(0.0);

	// Frames of 16ms, because that is what the surface gets and because the ambient clock advances by
	// at most 50ms per frame however long a frame took — so "wait 100ms" in one step is 50ms of
	// shimmer, not 100. Warm past the first 140ms first: the mock leaves no wake in them, and this
	// test is about the rule after that.
	for (int frame = 0; frame < 12; ++frame) {
		f.now += 16;
		f.stage.update();
	}
	CHECK(f.stage.rippleCount() == 0); // nothing has moved yet

	f.setPath(0.5);
	f.now += 16;
	f.stage.update();
	CHECK(f.stage.rippleCount() == 1);

	// Again straight away: the spacing is what stops a drag drawing a solid disc of rings.
	f.setPath(0.9);
	f.now += 16;
	f.stage.update();
	CHECK(f.stage.rippleCount() == 1);

	// Once the spacing has passed, the next move leaves its own.
	for (int frame = 0; frame < 10; ++frame) {
		f.now += 16;
		f.stage.update();
	}
	f.setPath(0.2);
	f.now += 16;
	f.stage.update();
	CHECK(f.stage.rippleCount() == 2);

	// And they go when they have lived their 1.4 seconds, rather than lingering as state nothing
	// draws and rippleCount() would still report.
	for (int frame = 0; frame < 120; ++frame) {
		f.now += 16;
		f.stage.update();
	}
	CHECK(f.stage.rippleCount() == 0);
}

//----------------------------//
// The mock's own random sky  //
//----------------------------//

TEST_CASE("the shared PRNG is the mock's, and so is the sky it builds") {
	// FIXTURE FROM THE PLATFORM, not from us: these numbers were produced by running the mock's own
	// `rng` and `BSTARS` in node, lifted out of feathers-ui-mock.html verbatim. A port that agrees
	// with our reading of the algorithm and disagrees with the algorithm would pass a test written
	// the other way round — which is the rule this project learned twice.
	MockRandom random(55);
	const double expected[6] = { 0.446262851, 0.873613231, 0.707322523,
		                         0.749052782, 0.062248004, 0.221056100 };
	for (const double value : expected) {
		CHECK(random() == doctest::Approx(value).epsilon(1e-6));
	}

	// And the sky those numbers become. Checked on the stars the surface actually draws, so this
	// fails if the derivation drifts as well as if the generator does.
	const std::array<SkyStar, 110>& stars = performSky();
	CHECK(stars.size() == 110);
	CHECK(stars[0].x == doctest::Approx(-0.855443149).epsilon(1e-6));
	CHECK(stars[0].y == doctest::Approx(0.300332663).epsilon(1e-6));
	CHECK(stars[0].magnitude == doctest::Approx(0.795125766).epsilon(1e-6));
	CHECK(stars[109].x == doctest::Approx(-0.439560884).epsilon(1e-6));
	CHECK(stars[109].y == doctest::Approx(-0.724934369).epsilon(1e-6));
	CHECK(stars[109].magnitude == doctest::Approx(0.516983889).epsilon(1e-6));

	// The sky's closest star to the centre, which is why the `||.01` zero-substitute in drawSky is
	// inert for this table and would not be for another.
	float closest = 1.0f;
	for (const SkyStar& star : stars) {
		closest = std::min(closest, std::hypot(star.x, star.y));
	}
	CHECK(closest == doctest::Approx(0.089523702).epsilon(1e-5));
}

TEST_CASE("the shared disk is the mock's circle wherever it is put") {
	// `C = s/2`, `R = s/2 - 8` over the SHORTER side, and the mock's glyph sizes scale by s/520.
	const StageDisk wide(1072.0f, 534.0f);
	CHECK(wide.radius() == doctest::Approx(534.0f * 0.5f - 8.0f));
	CHECK(wide.centre().x == doctest::Approx(536.0f));
	CHECK(wide.glyphScale() == doctest::Approx(534.0f / 520.0f));

	const StageDisk tall(300.0f, 900.0f);
	CHECK(tall.radius() == doctest::Approx(300.0f * 0.5f - 8.0f));

	// A frame too small for the rim inset has no circle to speak of, and must not answer with a
	// negative radius that would turn every glyph inside out.
	const StageDisk tiny(6.0f, 6.0f);
	CHECK(tiny.radius() <= 0.0f);
	CHECK_FALSE(tiny.contains({ 3.0f, 3.0f }));
	const visage::Point nowhere = tiny.toNormalized({ 3.0f, 3.0f });
	CHECK(nowhere.x == doctest::Approx(0.0f));
	CHECK(nowhere.y == doctest::Approx(0.0f));
}

TEST_CASE("the puck's marks stay inside the aperture, however far out it stands") {
	// SUPERSEDES A30, WHICH LET THE MARKS SPILL. A30 measured the spill and judged it honest — the puck
	// outside the disk is showing a residual pushed past the edge of the blend space — but the decision
	// is to clip, for tidiness. The mock clips everything to the circle, the
	// sky and the ripples already do, and a reticle drawn over the sound cards reads as a bug however
	// true it is.
	//
	// KNOWN-BAD IS THE SHIPPED DRAW: every mark went to the canvas at wherever the arithmetic put it.
	StageFixture f;
	visage::Canvas canvas;

	// Stand the puck well outside: a tour point out at a sound, plus the full Pos X/Y residual. This
	// is reachable by hand and by automation — the same state chaotic automation runs put it in.
	f.setPath(1.0);
	f.set(ParamId::PosX, 1.0);
	f.set(ParamId::PosY, 1.0);

	const StageDisk d = f.stage.disk();
	const BlendPoint puck = f.stage.puck();
	const visage::Point at = d.toPixels(puck.x, puck.y);
	INFO("puck at " << puck.x << "," << puck.y << " = pixels " << at.x << "," << at.y
	                << ", disk centre " << d.centre().x << "," << d.centre().y << " r " << d.radius());
	// The case is about a puck that is genuinely outside. If the clamps ever bring it home this
	// REQUIRE fails loudly rather than the case passing against a puck that never left.
	REQUIRE_FALSE(d.contains(at));

	f.stage.draw(canvas);
	// SOMETHING was drawn — a clip that swallowed the cursor whole would pass an "inside" check by
	// drawing nothing at all, which is the way this assertion fails without failing.
	CHECK(f.stage.puckInk().any);
	// ...and all of it is inside the aperture.
	CHECK(f.stage.puckInk().within(f.stage.diskSquare(), PerformStage::kGlyphBleed));
}

TEST_CASE("a puck inside the disk still draws its whole reticle") {
	// THE OVER-CLIPPING HALF, which is the mistake the first Explore clip made: a clip that drops any
	// shape it cannot fully contain quietly eats the picture. With the puck parked in the middle,
	// every mark is far from the rim and the ink must reach the reticle's full extent.
	StageFixture f;
	visage::Canvas canvas;
	f.setPath(0.5);
	f.set(ParamId::PosX, 0.0);
	f.set(ParamId::PosY, 0.0);

	const StageDisk d = f.stage.disk();
	const BlendPoint puck = f.stage.puck();
	const visage::Point at = d.toPixels(puck.x, puck.y);
	REQUIRE(d.contains(at));

	f.stage.draw(canvas);
	const InkBox ink = f.stage.puckInk();
	REQUIRE(ink.any);
	CHECK(ink.within(f.stage.diskSquare(), PerformStage::kGlyphBleed));
	// The reticle's ring is 13 units at the mock's scale and its ticks reach three quarters of a tick
	// beyond it, so the ink spans at least the ring either side of the puck. Quoted from the mock's
	// numbers rather than from the drawing code, so a shrunk reticle fails here instead of agreeing
	// with itself.
	const float ring = 13.0f * d.glyphScale();
	CHECK(ink.left <= at.x - ring + 1.0f);
	CHECK(ink.right >= at.x + ring - 1.0f);
	CHECK(ink.top <= at.y - ring + 1.0f);
	CHECK(ink.bottom >= at.y + ring - 1.0f);
}

TEST_CASE("a puck that has left entirely is marked at the rim rather than lost") {
	// Clipping alone would let the cursor VANISH — which is precisely A30's objection to clipping, and
	// the half of A30 the reversal did not touch: it removed the spill, not the hand. So when
	// nothing of the reticle is inside, a dim tick sits on the rim in the puck's direction, the same
	// vocabulary A27 gave Explore for a thing that is out there.
	StageFixture f;
	visage::Canvas canvas;
	f.setPath(1.0);
	f.set(ParamId::PosX, 1.0);
	f.set(ParamId::PosY, 1.0);

	const StageDisk d = f.stage.disk();
	const visage::Point at = d.toPixels(f.stage.puck().x, f.stage.puck().y);
	// Far enough out that not even the reticle's ring reaches back in, or this case is testing the
	// clip rather than the tick.
	const float ring = 13.0f * d.glyphScale();
	REQUIRE(std::hypot(at.x - d.centre().x, at.y - d.centre().y) > d.radius() + ring);

	f.stage.draw(canvas);
	const InkBox ink = f.stage.puckInk();
	REQUIRE(ink.any);
	// The mark is ON the rim, in the puck's own direction: the same side, and against the edge rather
	// than in the middle of the disk.
	const float markX = (ink.left + ink.right) * 0.5f;
	const float markY = (ink.top + ink.bottom) * 0.5f;
	const float reach = std::hypot(markX - d.centre().x, markY - d.centre().y);
	INFO("rim mark at " << markX << "," << markY << ", reach " << reach << " of " << d.radius());
	CHECK(reach > d.radius() * 0.8f);
	CHECK(reach <= d.radius());
	// Same direction as the puck, to a degree or so.
	const float wanted = std::atan2(at.y - d.centre().y, at.x - d.centre().x);
	const float drawn = std::atan2(markY - d.centre().y, markX - d.centre().x);
	CHECK(std::abs(wanted - drawn) < 0.02f);
}

TEST_CASE("a clipped ring keeps every piece of an arc that straddles the wrap") {
	// THE TRAP INSIDE THE ARC CLIP, pinned on its own because it is invisible in a picture: the
	// visible span and the arc being drawn are both spans around a circle, so their intersection can
	// be TWO pieces, and the piece written a turn away is the one a naive intersection drops. It would
	// go wrong only at certain angles, which is the kind of bug that reads as a rendering glitch.
	const StageDisk d(400.0f, 400.0f);
	const visage::Point centre = d.centre();

	// A ring centred exactly on the middle of the disk and bigger than it: nothing of it is inside.
	CHECK_FALSE(clipRingToDisk(d, centre, d.radius() + 10.0f).has_value());
	// One entirely inside answers "whole" rather than an arc that happens to cover everything.
	const std::optional<RingArc> inside = clipRingToDisk(d, centre, d.radius() * 0.5f);
	REQUIRE(inside.has_value());
	CHECK(inside->whole);
	// And one that crosses: the visible span points back at the middle of the disk, which is the half
	// of it that is still over the aperture.
	const visage::Point off { centre.x + d.radius() * 0.9f, centre.y };
	const std::optional<RingArc> crossing = clipRingToDisk(d, off, d.radius() * 0.5f);
	REQUIRE(crossing.has_value());
	CHECK_FALSE(crossing->whole);
	CHECK(std::abs(crossing->middle) > 3.0f); // pointing back toward −x, i.e. ±π
	CHECK(crossing->half > 0.0f);
	CHECK(crossing->half < 3.14159265f);
	// Non-finite geometry is nothing to draw rather than an arc nobody can place.
	const float notANumber = std::numeric_limits<float>::quiet_NaN();
	CHECK_FALSE(clipRingToDisk(d, { notANumber, notANumber }, 10.0f).has_value());
	CHECK_FALSE(clipRingToDisk(d, centre, -1.0f).has_value());
}

//--------------//
// Wave display //
//--------------//

// The scope draws one cycle of the character the engine is running on. Not an oscilloscope — see the
// header — so there is no ring buffer to test and nothing on the audio thread to get wrong. What
// there is to get wrong is the identity: which zone, which offset, which crossfade. These check that
// against the engine's own builders, with the parameter composition spelled out here rather than
// borrowed from the code under test.


TEST_CASE("the wave display draws the engine's own table, at the engine's own coordinates") {
	WaveFixture f;

	// A character with every ingredient different, so a mapping that dropped one — or added gamma to
	// the wrong sound — cannot pass by coincidence.
	f.set(ParamId::OscType, 0.0); // morph
	f.set(ParamId::AZone, 23.0);
	f.set(ParamId::AOffset, 0.4);
	f.set(ParamId::AGamma, 0.1);
	f.set(ParamId::BZone, 512.0);
	f.set(ParamId::BOffset, 0.7);
	f.set(ParamId::BGamma, -0.3);
	f.set(ParamId::GlobalGamma, 0.05);
	f.set(ParamId::Path, 0.25);
	f.publish();
	CHECK(f.wave.refresh());

	// The composition, written out here on purpose: phase offset is offset + per-sound gamma + global
	// gamma, and zone rounds. If characterFromParams ever disagrees with this, this test is the one
	// that should fail rather than the picture quietly going wrong.
	//
	// Blended rather than crossfaded, because ONE RULE now applies at every sound count (§2c). The
	// weights are Path's, which at two anchors on the tour and falloff 1 is what Path always was — so
	// the composition being checked is unchanged and only the rule that consumes it has moved.
	const owlet::dsp::PhiMorphWavetable tables[] = {
		owlet::dsp::buildPhiMorphWavetable(23, 0.4f + 0.1f + 0.05f),
		owlet::dsp::buildPhiMorphWavetable(512, 0.7f - 0.3f + 0.05f)
	};
	const double weights[] = { 0.75, 0.25 };
	const auto expected = buildMorphBlend({ tables, 2 }, { weights, 2 });

	const std::span<const float> curve = f.wave.curve();
	// At least two samples per logical pixel, and never below the mock's own 601.
	REQUIRE(curve.size() >= WaveDisplay::kMinPoints);
	CHECK(curve.size() >= static_cast<size_t>(WaveDisplay::kWidth * WaveDisplay::kPointsPerPixel));
	for (size_t i = 0; i < curve.size(); ++i) {
		const double phase = static_cast<double>(i) / static_cast<double>(curve.size() - 1);
		const float engineValue = std::clamp(evalMorphTable(expected, phase), -WaveDisplay::kClamp,
		                                     WaveDisplay::kClamp);
		CHECK(curve[i] == doctest::Approx(engineValue).epsilon(1e-6));
	}

	// And it is a waveform rather than a flat line, which is what makes the loop above worth running.
	const auto [low, high] = std::minmax_element(curve.begin(), curve.end());
	CHECK(*high - *low > 0.5f);
}

TEST_CASE("resonate: the wave display draws the string's rest pose, read as the scan reads it") {
	// A VALUE, against an independent source: a freshly reset WeaveString under the same laws holds the
	// parked scan in its table, so the picture must be that table, interpolated linearly and wrapped as
	// the scan reads it. One sound on its own anchor, so the laws are the firmware builder's at its home.
	WaveFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Resonate));
	f.set(ParamId::AZone, 17.0);
	f.set(ParamId::BEnable, 0.0);
	f.set(ParamId::Path, 0.0);
	f.publish();
	f.wave.refresh();

	const auto parked = [](feathers::WeaveModel model) {
		const owlet::dsp::PhiWeaveParams laws = owlet::dsp::buildPhiWeaveLaws(17.0 / 1023.0);
		feathers::WeaveString string;
		string.prepare(48000.0);
		string.setModel(model);
		string.setLaws(laws, laws);
		string.reset();
		std::vector<float> table(feathers::WeaveString::kNodes);
		for (int i = 0; i < feathers::WeaveString::kNodes; ++i) {
			table[static_cast<size_t>(i)] = string.tableForTest(0, i);
		}
		return table;
	};
	const auto check = [&](const std::vector<float>& table) {
		const std::span<const float> curve = f.wave.curve();
		REQUIRE(curve.size() >= WaveDisplay::kMinPoints);
		constexpr int kN = feathers::WeaveString::kNodes;
		for (size_t i = 0; i < curve.size(); i += 7) {
			const double at = static_cast<double>(i) / static_cast<double>(curve.size() - 1) * kN;
			const int index = std::min(kN - 1, static_cast<int>(at));
			const auto frac = static_cast<float>(at - index);
			const float from = table[static_cast<size_t>(index)];
			const float to = table[static_cast<size_t>((index + 1) % kN)];
			CAPTURE(i);
			CHECK(std::abs(curve[i] - (from + (to - from) * frac)) <= 1e-6f);
		}
	};
	const std::vector<float> ring = parked(feathers::WeaveModel::Ring);
	check(ring);

	// NOT THE HOME SHAPE, which is what a first reading of "the shape the coordinate selects" draws:
	// coupling pulls the rest pose away from home, and at this coordinate by a visible amount.
	const owlet::dsp::PhiWeaveParams laws = owlet::dsp::buildPhiWeaveLaws(17.0 / 1023.0);
	float homePeak = 0.0f;
	float restPeak = 0.0f;
	for (int i = 0; i < feathers::WeaveString::kNodes; ++i) {
		homePeak = std::max(homePeak, std::abs(laws.home[i]));
		restPeak = std::max(restPeak, std::abs(ring[static_cast<size_t>(i)]));
	}
	float shapeGap = 0.0f;
	for (int i = 0; i < feathers::WeaveString::kNodes; ++i) {
		shapeGap = std::max(shapeGap,
		                    std::abs(laws.home[i] / homePeak - ring[static_cast<size_t>(i)] / restPeak));
	}
	MESSAGE("rest pose vs home shape, peak-normalised: max gap " << shapeGap);
	CHECK(shapeGap > 0.05f);

	// And the model is part of the picture: Membrane redraws, as its own parked scan.
	f.set(ParamId::ResonateModel, 3.0);
	f.publish();
	CHECK(f.wave.refresh());
	check(parked(feathers::WeaveModel::Membrane));
}

TEST_CASE("resonate: the scope draws the rest pose of the SHAPED laws") {
	// Resonate's Stiffness and Coupling move the pose the string rests in, so the scope's picture must be
	// the shaped laws' pose: a reset string handed the laws shaped the same way holds it in its table.
	WaveFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Resonate));
	f.set(ParamId::AZone, 17.0);
	f.set(ParamId::BEnable, 0.0);
	f.set(ParamId::Path, 0.0);
	f.set(ParamId::ResonateStiffness, -2.0); // x0.25: slack
	f.set(ParamId::ResonateCoupling, 2.0);   // x4: strongly coupled
	f.publish();
	f.wave.refresh();

	owlet::dsp::PhiWeaveParams laws = owlet::dsp::buildPhiWeaveLaws(17.0 / 1023.0);
	feathers::ResonateShaping s;
	s.stiffness = 0.25f;
	s.coupling = 4.0f;
	feathers::shapeWeaveLaws(s, laws);
	feathers::WeaveString string;
	string.prepare(48000.0);
	string.setLaws(laws, laws);
	string.reset();
	constexpr int kN = feathers::WeaveString::kNodes;
	const std::span<const float> curve = f.wave.curve();
	float worst = 0.0f;
	for (size_t i = 0; i < curve.size(); i += 7) {
		const double at = static_cast<double>(i) / static_cast<double>(curve.size() - 1) * kN;
		const int index = std::min(kN - 1, static_cast<int>(at));
		const auto frac = static_cast<float>(at - index);
		const float from = string.tableForTest(0, index);
		const float to = string.tableForTest(0, (index + 1) % kN);
		worst = std::max(worst, std::abs(curve[i] - (from + (to - from) * frac)));
	}
	CHECK(worst <= 1e-6f);
	const std::vector<float> shaped(curve.begin(), curve.end()); // a copy: the span is the display's own buffer

	// And it is not the unshaped pose, so the check above is not passing on a picture that ignored them.
	f.set(ParamId::ResonateStiffness, 0.0);
	f.set(ParamId::ResonateCoupling, 0.0);
	f.publish();
	CHECK(f.wave.refresh());
	float moved = 0.0f;
	const std::span<const float> plain = f.wave.curve();
	for (size_t i = 0; i < plain.size(); ++i) {
		moved = std::max(moved, std::abs(plain[i] - shaped[i]));
	}
	MESSAGE("scope: shaped against unshaped pose, max difference " << moved);
	CHECK(moved > 0.01f);
}

TEST_CASE("the wave display follows the crossfade and the engine choice") {
	WaveFixture f;
	f.set(ParamId::AZone, 23.0);
	f.set(ParamId::BZone, 700.0);
	f.set(ParamId::Path, 0.0);
	f.publish();
	f.wave.refresh();
	const std::vector<float> atA(f.wave.curve().begin(), f.wave.curve().end());

	// All the way to B is a different sound, so a different picture.
	f.set(ParamId::Path, 1.0);
	f.publish();
	CHECK(f.wave.refresh());
	const std::vector<float> atB(f.wave.curve().begin(), f.wave.curve().end());
	CHECK(atA != atB);

	// The stair engine is a different instrument, and the scope says so — its cycle is steps, so it
	// holds values far longer than the morph curve does.
	f.set(ParamId::OscType, 1.0);
	f.publish();
	CHECK(f.wave.refresh());
	const std::vector<float> stair(f.wave.curve().begin(), f.wave.curve().end());
	CHECK(stair != atB);

	size_t held = 0;
	for (size_t i = 1; i < stair.size(); ++i) {
		if (std::abs(stair[i] - stair[i - 1]) < 1.0e-6f) {
			++held;
		}
	}
	CHECK(held > stair.size() / 4);
}

TEST_CASE("the wave display draws the vowel the voices are singing") {
	// THE DEFECT THIS CLOSES: the scope drew a V5 sound's morph-and-operators cycle with no vox in
	// it, so a sound with a throat was pictured as a waveform nobody was playing. V22c puts the vox
	// crossfade at the operator chain's SOURCE read, so in vowel country the blend IS the cycle, and
	// a picture that skips it is not a coarser drawing of the sound — it is a different waveform.
	WaveFixture f;
	f.set(ParamId::OscType, 2.0); // V5, the only engine with a vox seam
	f.set(ParamId::AZone, 0.0);
	// A CHOSEN SPOT, NOT A STUMBLED-ON ONE. Home is zone/1023 + offset + gamma, and vowel country at
	// zone 0 runs about home 1.70..3.10 — the species field has the longest period in the engine, so
	// there is no vowel anywhere in home 0..1 and an offset under 1.0 would have tested nothing. This
	// one maximises presence × quantize: the vowel is strongly present AND the quantizer is awake,
	// which is what makes the staircase re-derivation below load-bearing rather than a no-op. Both
	// facts are asserted rather than trusted, so a species-field change fails here loudly instead of
	// quietly turning this into a second test of the vowel-free path.
	f.set(ParamId::AOffset, 1.945);
	f.set(ParamId::Path, 0.0);
	f.publish();
	REQUIRE(f.wave.refresh());

	const Character c = f.wave.character();
	REQUIRE(c.flagship);
	const VoxCharacter vox = buildVoxFor(c);
	REQUIRE(vox.audible);
	REQUIRE(vox.presence > 0.4f);

	// THE REFERENCE, BUILT THE VOICE'S WAY — three statements, and the middle one is the whole point.
	// FlagshipSource is a DEFAULTED parameter on both entries, so a two-statement version compiles
	// clean and is wrong: `held` is the sync+feedback chain sampled at the step centres, and
	// buildFlagshipFor sampled it on the vowel-free cycle. The final combine drags the output back
	// toward those held values, so the vowel comes out attenuated by (1 − quantize) and vanishes
	// entirely at quantize 1. Plausible picture, half-applied blend, nothing failing — which is why
	// this case asserts the VALUE rather than that two renders differ.
	VoxCycle cycle;
	bakeVoxCycle(vox.params, pitchToHz(WaveDisplay::kVoxRenderKey), cycle);
	REQUIRE(cycle.voiced);
	const FlagshipSource source { &cycle, vox.presence };
	FlagshipRecipe voiced = buildFlagshipFor(c);
	REQUIRE(voiced.operated);
	REQUIRE(voiced.quantize > 0.2f); // the staircase is live, so `held` is load-bearing here
	prepareFlagshipRecipe(voiced, source);

	// And the vowel-free build, which is what the picture WAS and must no longer be.
	const FlagshipRecipe dry = buildFlagshipFor(c);

	const std::span<const float> curve = f.wave.curve();
	REQUIRE(curve.size() > 1);
	double worstFromDry = 0.0;
	for (size_t i = 0; i < curve.size(); ++i) {
		const double phase = static_cast<double>(i) / static_cast<double>(curve.size() - 1);
		const float want = std::clamp(evalFlagship(voiced, phase, source), -WaveDisplay::kClamp,
		                              WaveDisplay::kClamp);
		CHECK(curve[i] == doctest::Approx(want).epsilon(1e-6));
		const float voxFree =
		    std::clamp(evalFlagship(dry, phase), -WaveDisplay::kClamp, WaveDisplay::kClamp);
		worstFromDry = std::max(worstFromDry, std::abs(static_cast<double>(curve[i])
		                                              - static_cast<double>(voxFree)));
	}
	// AND THE VOWEL IS VISIBLE, not merely present in the arithmetic. This is the half that failed
	// before the fix, when the curve WAS `dry` and this difference was exactly zero.
	CHECK(worstFromDry > 0.05);

	// THE RENDER PITCH IS C3 AND IT IS DERIVED, not transcribed: the firmware's phi_vox.cpp pivots
	// every stored noteRatio on kReferenceHz = 130.81f, and pitchToHz(48) is the tuning system's own
	// exact answer for that note — 0.037 cents from the rounded literal, which is what deriving buys.
	CHECK(pitchToHz(WaveDisplay::kVoxRenderKey) == doctest::Approx(130.81).epsilon(1e-4));
}

TEST_CASE("the wave display leaves vowel-free terrain exactly as it drew it") {
	// THE OTHER HALF OF THE CLAIM, and the one that keeps the fix from being a change to every V5
	// picture: most of the terrain has no vowel in it, and there the source is null — not a blend at
	// weight zero — so evalFlagship takes the road it always took and the curve is the bits it always
	// was. Without this, "the scope now draws vox" would be indistinguishable from "the scope now
	// draws everything slightly differently".
	WaveFixture f;
	f.set(ParamId::OscType, 2.0);
	f.set(ParamId::AZone, 0.0);
	f.set(ParamId::AOffset, 0.4); // home well under 1: no vowel reaches here
	f.set(ParamId::Path, 0.0);
	f.publish();
	REQUIRE(f.wave.refresh());

	const Character c = f.wave.character();
	REQUIRE_FALSE(buildVoxFor(c).audible);

	const FlagshipRecipe recipe = buildFlagshipFor(c);
	const std::span<const float> curve = f.wave.curve();
	REQUIRE(curve.size() > 1);
	for (size_t i = 0; i < curve.size(); ++i) {
		const double phase = static_cast<double>(i) / static_cast<double>(curve.size() - 1);
		// Bit-exact, not Approx: the claim is that no new float code runs here at all.
		CHECK(curve[i]
		      == std::clamp(evalFlagship(recipe, phase), -WaveDisplay::kClamp, WaveDisplay::kClamp));
	}
}

TEST_CASE("the wave display rebuilds only when the character moves") {
	WaveFixture f;
	f.set(ParamId::AZone, 40.0);
	f.publish();
	CHECK(f.wave.refresh()); // first build

	// Nothing moved: no rebuild. The scope is on the same every-frame path as everything else in the
	// footer, and rebuilding a cycle per frame for a still parameter is work nobody asked for.
	CHECK_FALSE(f.wave.refresh());

	// A parameter it does not draw moves nothing either.
	f.set(ParamId::Level, 0.5);
	f.publish();
	CHECK_FALSE(f.wave.refresh());

	// Zone rounds to an integer on the way into the character, so a change too small to change the
	// table is correctly ignored — and one that crosses the boundary is not.
	f.set(ParamId::AZone, 40.2);
	f.publish();
	CHECK_FALSE(f.wave.refresh());
	f.set(ParamId::AZone, 40.7);
	f.publish();
	CHECK(f.wave.refresh());
}

TEST_CASE("the wave display shows what is played, modulation included") {
	WaveFixture f;
	f.set(ParamId::AZone, 10.0);
	f.set(ParamId::BZone, 900.0);
	f.set(ParamId::Path, 0.0);
	f.publish();
	f.wave.refresh();
	CHECK(f.wave.character().xf == doctest::Approx(0.0f));

	// The host modulates Path without touching its base value. The engine runs on base + modulation,
	// so the picture has to as well — a scope showing the un-modulated sound would be drawing
	// something nobody is hearing.
	f.params.setModOffset(static_cast<clap_id>(ParamId::Path), 0.8);
	f.publish();
	CHECK(f.wave.refresh());
	CHECK(f.wave.character().xf == doctest::Approx(0.8f));
}

TEST_CASE("the rotate slider pins the turn") {
	// The debug panel's audition handle. Untested, it is exactly the kind of control that looks alive
	// — it draws, it moves under the mouse — and writes nowhere, which would be discovered as "the
	// rotation does not seem to do anything" in the middle of a listening session.
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	int redraws = 0;
	OrientationRow rotate(bridge, [&redraws] { redraws++; });
	rotate.setBounds(0.0f, 0.0f, 200.0f, 24.0f);

	CHECK_FALSE(bridge.orientation().pinned); // nothing is held until a hand touches it
	CHECK(rotate.value() == 0.0f);            // and 0 is the turn every unturned sound plays at

	// The turn reads from the centre, because zero is the meaningful place for one rather than an end
	// of its travel — and pinning AT the centre pins rotate 0, which is exactly where every unturned
	// sound already is, so the capture cannot jump the sound.
	rotate.processMouseDown(at(100.0f, 12.0f));
	CHECK(bridge.orientation().pinned);
	CHECK(bridge.orientation().generator.rotate == 0.0f);
	CHECK(redraws == 1);

	rotate.processMouseDrag(at(200.0f, 12.0f));
	CHECK(bridge.orientation().generator.rotate > 3.0f);
	rotate.processMouseDrag(at(0.0f, 12.0f));
	CHECK(bridge.orientation().generator.rotate < -3.0f);
	CHECK(redraws == 3);

	// THE PIN IS ONE NUMBER, and since the shrink the type says so: a SliceGenerator holds the rotate
	// and nothing else, so "nothing rides along" no longer needs asserting field by field.
}

TEST_CASE("the scope reports the excitation of the blend it is drawing") {
	// The readout is a claim about the engine, so it has to change rule when the engine does. With two
	// sounds it prints the crossfade's 4t(1−t); with three it prints the spread of the weights, which
	// is what the build is actually applying. A scope that kept printing the fade's number with a
	// constellation on screen would be describing a performance nobody is giving.
	//
	// AT TWO SOUNDS IT IS THE BLEND'S NUMBER TOO, since §2c made the rule uniform — and the fixture has
	// to be a point where the two formulas DISAGREE or it proves nothing. Path 0.5 with the puck on the
	// tour and falloff 1 is the one place they agree exactly, which is where this test used to sit: it
	// passed on a build printing the superseded formula. Pos X off the tour is what separates them.
	WaveFixture f;
	f.set(ParamId::Path, 0.5);
	f.set(ParamId::PosX, 0.3);
	f.set(ParamId::PosY, -0.2);
	f.publish();
	REQUIRE(f.wave.refresh());
	REQUIRE(f.wave.character().count == 2);
	{
		const Character drawn = f.wave.character();
		const double pair[] = { drawn.sounds[0].weight, drawn.sounds[1].weight };
		const float blended = blendExcite({ pair, 2 });
		const float faded = morphExcite(drawn.xf);
		// Stated rather than assumed: if these ever stop disagreeing here, the case has stopped being
		// a control and is passing on a coincidence again.
		REQUIRE(std::abs(blended - faded) > 0.05f);
		CHECK(f.wave.excite() == doctest::Approx(blended));
	}

	f.set(feathers::soundParam(2, feathers::SoundField::Enable), 1.0);
	f.publish();
	REQUIRE(f.wave.refresh());
	REQUIRE(f.wave.character().count == 3);
	// And with a third sound it still follows the WEIGHTS rather than Path, which has not moved. The
	// comparison is against the weights the scope is drawing, not against a constant: an earlier
	// version checked "not equal to morphExcite(0.5)" and went red the day the blend legitimately
	// reached full spread — a test failing because the engine was right.
	const Character drawn = f.wave.character();
	double weights[8] = {};
	for (int i = 0; i < 3; ++i) {
		weights[i] = drawn.sounds[i].weight;
	}
	CHECK(f.wave.excite() == doctest::Approx(blendExcite({ weights, 3 })));
}

TEST_CASE("the falloff row picks the side of the A/B under the pointer") {
	// The other audition handle, and the one whose whole purpose is a comparison made twice in a
	// minute. Same failure mode as the orientation sliders — a control that draws and moves and writes
	// nowhere would be discovered as "the falloff does not seem to do anything" mid-session — and it
	// has a second one of its own: a stepped control that reads its own copy instead of the bridge
	// would keep showing A after the environment set B.
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	int redraws = 0;
	TuningRow falloff(
	    "FALLOFF",
	    std::vector<TuningRow::Step> { { kFalloffPowerA, "1 (NEAR)" }, { kFalloffPowerB, "2 (LOCAL)" } },
	    [&bridge] { return bridge.tuning().falloffPower; },
	    [&bridge](double value) { bridge.setFalloffPower(value); }, [&redraws] { redraws++; });
	falloff.setBounds(0.0f, 0.0f, 200.0f, 24.0f);

	REQUIRE(bridge.tuning().falloffPower == kFalloffPowerA);

	falloff.processMouseDown(at(150.0f, 12.0f)); // the right half is the B side
	CHECK(bridge.tuning().falloffPower == kFalloffPowerB);
	CHECK(redraws == 1);

	falloff.processMouseDown(at(50.0f, 12.0f)); // and back, which is what makes it an A/B
	CHECK(bridge.tuning().falloffPower == kFalloffPowerA);
	CHECK(redraws == 2);

	// The far right edge picks the last step rather than reading one past the end.
	falloff.processMouseDown(at(200.0f, 12.0f));
	CHECK(bridge.tuning().falloffPower == kFalloffPowerB);
}

TEST_CASE("an unpinned rotate row reads zero however far the sound is carried") {
	// The row's reading is the TURN, not the plane. An unturned sound plays at rotate 0 wherever it
	// stands — home enters the decode as M's other argument, never as a turn — so carrying the sound a
	// long way along its line must NOT move this reading. The predecessor of this case asserted the
	// opposite (the row derived a φ-orientation from home), and that number stopped reaching the
	// decode when M landed: a reading that moved with gamma here would be showing authority it does
	// not have.
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	OrientationRow rotate(bridge, [] {});
	rotate.setBounds(0.0f, 0.0f, 200.0f, 24.0f);
	REQUIRE(rotate.value() == 0.0f);

	bridge.beginSoundGammaGesture();
	bridge.setSoundGamma(0, 51.5);
	bridge.endSoundGammaGesture();

	CHECK(rotate.value() == 0.0f);

	// And a PINNED turn is equally indifferent to the base: the pin is a turn, not a place.
	rotate.processMouseDown(at(150.0f, 12.0f));
	const float pinnedAt = rotate.value();
	REQUIRE(pinnedAt > 0.0f);
	bridge.beginSoundGammaGesture();
	bridge.setSoundGamma(0, -12.25);
	bridge.endSoundGammaGesture();
	CHECK(rotate.value() == pinnedAt);
}

TEST_CASE("right-clicking the rotate slider gives the sounds their own turns back") {
	// The comparison the design asks for — are the sounds' own turns good ones? — needs both
	// directions in one session. A pin you can only escape by restarting the host is a pin nobody
	// A/Bs.
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	int redraws = 0;
	OrientationRow rotate(bridge, [&redraws] { redraws++; });
	rotate.setBounds(0.0f, 0.0f, 200.0f, 24.0f);

	rotate.processMouseDown(at(150.0f, 12.0f));
	REQUIRE(bridge.orientation().pinned);

	visage::MouseEvent release = at(150.0f, 12.0f);
	release.button_id = visage::kMouseButtonRight;
	rotate.processMouseDown(release);
	CHECK_FALSE(bridge.orientation().pinned);
	CHECK(redraws == 2); // the readout has to follow, or it draws a turn nothing is playing

	// And a right-drag does not smear a value on the way out.
	rotate.processMouseDrag(release);
	CHECK_FALSE(bridge.orientation().pinned);
}

TEST_CASE("a star can be picked up and put somewhere else") {
	// ARRANGING THE CONSTELLATION BY HAND, which is what makes the audition's question askable: "does
	// moving between three sounds sound like moving between three sounds" is not answerable if the
	// three sounds can only be moved from the host's generic panel.
	StageFixture f;
	f.set(feathers::soundParam(2, feathers::SoundField::Enable), 1.0);

	const clap_id placeX = static_cast<clap_id>(feathers::soundParam(2, feathers::SoundField::PlaceX));
	const clap_id placeY = static_cast<clap_id>(feathers::soundParam(2, feathers::SoundField::PlaceY));

	// Press ON sound C, then drag it somewhere else in the disk.
	const visage::Point onStar = f.stage.toPixels(f.placeOf(2));
	const BlendPoint target { -0.25f, 0.35f };
	const visage::Point to = f.stage.toPixels(target);
	f.stage.processMouseDown(at(onStar.x, onStar.y));
	f.stage.processMouseDrag(at(to.x, to.y));
	f.stage.processMouseUp(at(to.x, to.y));

	std::map<clap_id, double> values;
	std::map<clap_id, int> begins;
	std::map<clap_id, int> ends;
	for (const UiParamEvent& event : drain(f.bridge)) {
		if (event.kind == UiParamEvent::Kind::Value) {
			values[event.id] = event.value;
		}
		else if (event.kind == UiParamEvent::Kind::GestureBegin) {
			begins[event.id]++;
		}
		else {
			ends[event.id]++;
		}
	}
	CHECK(values[placeX] == doctest::Approx(target.x));
	CHECK(values[placeY] == doctest::Approx(target.y));
	CHECK(begins[placeX] == 1);
	CHECK(ends[placeX] == 1);

	// AND THE PUCK WAS NOT MOVED BY IT. Grabbing a star is a different gesture from grabbing the puck,
	// and a press that did both would rearrange the constellation and the position you are hearing it
	// from in one motion, with no way to tell which changed the sound.
	CHECK(values.find(static_cast<clap_id>(ParamId::Path)) == values.end());
	CHECK(values.find(static_cast<clap_id>(ParamId::PosX)) == values.end());
}

/// IS THIS PARAMETER A SOUND'S PLACEMENT — by its id, over the enumerators, not by its name (backlog EH,
/// shape 8): two cases below asserted "no placement was posted" by `find("Place")` on the name, which a
/// rename would turn into a check that passes on every event including the ones it exists to forbid.
bool isPlacementParam(clap_id id) {
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		if (id == static_cast<clap_id>(soundParam(slot, SoundField::PlaceX))
		    || id == static_cast<clap_id>(soundParam(slot, SoundField::PlaceY))) {
			return true;
		}
	}
	return false;
}

TEST_CASE("the puck keeps the presses the stars do not want") {
	// The other half of the same grammar: a press in open space is the puck's, and a drag that started
	// on the puck stays the puck's even when it passes over a star. Deciding again mid-drag would let
	// a star steal a gesture aimed somewhere else, which is the kind of thing that only shows up as
	// "the sound jumped while I was moving".
	StageFixture f;
	f.setPath(0.5);

	const BlendPoint open { 0.0f, -0.6f }; // inside the disk, well away from both default anchors
	const visage::Point from = f.stage.toPixels(open);
	f.stage.processMouseDown(at(from.x, from.y));
	// Straight across sound B's position and out the other side.
	const visage::Point overStar = f.stage.toPixels(f.placeOf(1));
	f.stage.processMouseDrag(at(overStar.x, overStar.y));
	f.stage.processMouseUp(at(overStar.x, overStar.y));

	for (const UiParamEvent& event : drain(f.bridge)) {
		if (event.kind != UiParamEvent::Kind::Value) {
			continue;
		}
		// Nothing this gesture posted may be a placement: the hand grabbed the puck.
		const ParamDesc* desc = findParam(event.id);
		REQUIRE(desc != nullptr);
		CAPTURE(std::string(desc->name));
		CHECK_FALSE(isPlacementParam(event.id)); // by id, so a rename cannot make this vacuous
	}
}

TEST_CASE("where the puck sits on a sound, the press is the puck's") {
	// The ambiguity is real: park Path at either end of the tour and the puck is exactly on a star. The
	// two readings fail differently, which is what decides it — guessing "star" moves somebody's sound
	// when they meant to move where they are standing, and does it silently. Guessing "puck" means a
	// star under the puck has to be uncovered before it can be grabbed, which is a thing you can see
	// and fix by moving.
	StageFixture f;
	f.setPath(0.0); // the puck is now exactly on sound A

	const visage::Point onBoth = f.stage.toPixels(f.placeOf(0));
	f.stage.processMouseDown(at(onBoth.x, onBoth.y));
	const visage::Point away = f.stage.toPixels(BlendPoint { 0.1f, 0.4f });
	f.stage.processMouseDrag(at(away.x, away.y));
	f.stage.processMouseUp(at(away.x, away.y));

	for (const UiParamEvent& event : drain(f.bridge)) {
		if (event.kind != UiParamEvent::Kind::Value) {
			continue;
		}
		const ParamDesc* desc = findParam(event.id);
		REQUIRE(desc != nullptr);
		CAPTURE(std::string(desc->name));
		CHECK_FALSE(isPlacementParam(event.id)); // by id, so a rename cannot make this vacuous
	}
	// Sound A did not move, which is the whole point.
	CHECK(f.stage.stars()[0].at.x == doctest::Approx(f.placeOf(0).x));
}

TEST_CASE("in parity mode the scope prints the crossfade's excitation again") {
	// The other side of the same rule: parity mode really does run the v1 crossfade at two sounds, so
	// its 4t(1−t) is what the engine is applying and what the readout owes. Without this case the fix
	// above could be "always blendExcite", which would be wrong in exactly one mode and would look
	// right everywhere a test happened to point.
	WaveFixture f;
	f.set(ParamId::Path, 0.5);
	f.set(ParamId::PosX, 0.3);
	f.set(ParamId::PosY, -0.2);
	f.publish();
	AuditionTuning parity;
	parity.v1Crossfade = true;
	f.bridge.setV1Crossfade(true);
	REQUIRE(f.wave.refresh());
	REQUIRE(f.wave.character().count == 2);
	REQUIRE(f.wave.character().v1Crossfade);
	CHECK(f.wave.excite() == doctest::Approx(morphExcite(f.wave.character().xf)));
}

//--------------//
// Sound stack  //
//--------------//

// The rail is a list over the parameters that say what a sound IS, plus the one switch that decides
// whether it is in the constellation at all. So it is tested as that: what a coordinate hits, what a
// click posts, what the scroll contract holds, and — the one that matters for Explore — that the
// selection follows the card rather than the other way round.


TEST_CASE("the rail shows the sounds somebody made, and room to make another") {
	// Clone was confusing with all eight sounds pre-seeded, so slots stay empty until they are
	// consumed. The mock never showed eight seeded sounds — it
	// starts with three and has a "+ New sound" button — and this is the rail catching up to it.
	StackFixture f;
	// A fresh patch: A and B are in the constellation, the other six are slots nobody has used.
	CHECK(f.stack.visibleCount() == 2);
	CHECK(f.stack.visibleSlot(0) == 0);
	CHECK(f.stack.visibleSlot(1) == 1);
	for (size_t slot = 2; slot < kMaxSounds; ++slot) {
		CAPTURE(slot);
		CHECK(f.stack.isEmpty(slot));
	}

	// AN AUTHORED WIDTH CONSUMES THE SLOT (V14a — placement-family, the same argument as a moved
	// placement): a slot somebody has widened is a slot somebody has used, so it appears in the rail
	// and a clone will not land on it. Back at its default it is derivation-empty again.
	f.set(soundParam(7, SoundField::Width), 0.25);
	f.settle();
	CHECK_FALSE(f.stack.isEmpty(7));
	CHECK(f.stack.visibleCount() == 3);
	f.set(soundParam(7, SoundField::Width), 0.0);
	f.settle();
	CHECK(f.stack.isEmpty(7));
	CHECK(f.stack.visibleCount() == 2);

	// A PARKED SOUND IS NOT EMPTY — the distinction the whole change turns on. Authoring an identity
	// consumes the slot; switching it off does not un-consume it, so it stays on screen and a clone
	// will not land on it.
	f.consume(4, 700.0);
	CHECK_FALSE(f.stack.isEmpty(4));
	CHECK(f.stack.visibleCount() == 3);
	CHECK(f.stack.visibleSlot(2) == 4);
	CHECK_FALSE(f.read(ParamId::EEnable) == 1.0);

	// AND THE CARDS CLOSE UP. E is the third card even though it is the fifth slot — there is no gap
	// where the empty ones would have been, which is the whole visual point.
	const visage::Bounds second = f.stack.cardBoundsForTest(1);
	const visage::Bounds third = f.stack.cardBoundsForTest(4);
	CHECK(third.y() > second.y());
	CHECK(third.y() - second.y() == doctest::Approx(f.stack.cardBoundsForTest(1).y()
	                                                - f.stack.cardBoundsForTest(0).y()));
	// A press on that card reaches the sound it draws, not the slot its position would suggest.
	CHECK(f.stack.hitAt(f.cardPoint(4)).slot == 4);
}

TEST_CASE("a clone lands where nothing was, so it can never replace a sound somebody made") {
	// The other half of the empty-until-consumed design. With every slot pre-seeded there was never an empty one, so
	// every clone landed on a seeded sound and said "cloned over" — which reads as destroying
	// something because it is. Now a clone spends a slot nothing was authored in, and the sounds
	// between that slot and the source shift along to put the copy beside its original.
	StackFixture f;
	f.set(ParamId::AZone, 256.0);
	// Park a sound in C. It is on screen, it is out of the constellation, and it must be safe.
	f.consume(2, 815.0);
	const SlotState parked = readSlotState(f.bridge, 2);

	REQUIRE(f.stack.cloneTarget().has_value());
	CHECK(*f.stack.cloneTarget() == 3); // D — C holds something
	f.stack.onMouseDown().callback(at(f.clonePoint(0).x, f.clonePoint(0).y));
	f.settle();
	CHECK(f.tray.text() == "Sound A cloned to B");
	// THE PARKED SOUND SURVIVES, which is the claim — it has moved down one with the rest of the
	// splice, and nothing about it has changed.
	CHECK(readSlotState(f.bridge, 3) == parked);

	// Fill the rest, and the refusal is about slots being USED rather than switched on.
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		f.consume(slot, 100.0 + static_cast<double>(slot));
	}
	CHECK_FALSE(f.stack.cloneTarget().has_value());
	f.stack.onMouseDown().callback(at(f.clonePoint(0).x, f.clonePoint(0).y));
	CHECK(f.tray.text() == "Max 8 sounds — nowhere to clone to");
}

TEST_CASE("the new-sound row mints a sound in an empty slot, at the emptiest part of the circle") {
	// The mock's "+ New sound" (its addBtn), which is how a sound comes into being when there is no
	// original to copy — and the thing that makes "empty until consumed" a state you can leave.
	StackFixture f;
	REQUIRE(f.stack.visibleCount() == 2);
	REQUIRE(f.stack.newSoundTarget().has_value());
	CHECK(*f.stack.newSoundTarget() == 2);

	const visage::Point add = f.newSoundPoint();
	REQUIRE(f.stack.hitAt(add).part == SoundStack::Part::NewSound);
	f.stack.onMouseDown().callback(at(add.x, add.y));
	f.settle();

	// It exists, it is in the constellation, and it is what you are working on.
	CHECK(f.stack.visibleCount() == 3);
	CHECK_FALSE(f.stack.isEmpty(2));
	CHECK(f.read(ParamId::CEnable) == 1.0);
	CHECK(f.selection.slot() == 2);
	// A ROLLED IDENTITY rather than a sound born at the origin — the mock mints one the same way, and
	// a new sound at zone 0 would be a new sound nobody asked to hear.
	CHECK(f.read(ParamId::CZone) != doctest::Approx(0.0));
	// AND A PLACE OF ITS OWN: farther from both existing sounds than they are from each other, which
	// is what "claims the emptiest territory" has to mean to be worth doing.
	const AnchorPoint a { f.read(ParamId::APlaceX), f.read(ParamId::APlaceY) };
	const AnchorPoint b { f.read(ParamId::BPlaceX), f.read(ParamId::BPlaceY) };
	const AnchorPoint c { f.read(ParamId::CPlaceX), f.read(ParamId::CPlaceY) };
	const auto gap = [](const AnchorPoint& p, const AnchorPoint& q) {
		return std::hypot(p.x - q.x, p.y - q.y);
	};
	CHECK(std::min(gap(c, a), gap(c, b)) > 0.0);
	CHECK(gap(c, a) > 0.3);
	CHECK(gap(c, b) > 0.3);

	// One undo step, and the slot goes back to being empty — the rail stops drawing it.
	REQUIRE(f.history.canUndo());
	f.history.undo();
	f.settle();
	CHECK(f.stack.isEmpty(2));
	CHECK(f.stack.visibleCount() == 2);

}

TEST_CASE("the new-sound row exists only while it can do something") {
	// A REFINEMENT AFTER THE FIRST BUILD: with eight sounds present the row is hidden completely, not
	// dimmed. So the rail shows exactly what exists — the sounds, and a way to make one
	// while there is room — and the ceiling is communicated by the row being GONE rather than by a
	// control sitting there looking broken.
	StackFixture f;
	REQUIRE(f.stack.visibleCount() == 2);
	const float roomForOne = f.stack.contentHeight();
	REQUIRE(f.stack.hitAt(f.newSoundPoint()).part == SoundStack::Part::NewSound);

	// Fill every slot.
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		f.consume(slot, 200.0 + static_cast<double>(slot));
	}
	REQUIRE(f.stack.visibleCount() == kMaxSounds);
	REQUIRE_FALSE(f.stack.newSoundTarget().has_value());

	// GONE, not dimmed: it answers no press, and it takes no height either — a hidden row that still
	// reserved its space would leave a gap that means nothing.
	CHECK(f.stack.hitAt(f.newSoundPoint()).part != SoundStack::Part::NewSound);
	const float atCeiling = f.stack.contentHeight();
	float cards = 0.0f;
	for (size_t row = 0; row < kMaxSounds; ++row) {
		cards += f.stack.slotHeightForTest(f.stack.visibleSlot(row));
	}
	CHECK(atCeiling == doctest::Approx(SoundStack::kPadTop + SoundStack::kHeadingHeight
	                                   + SoundStack::kRowGap + cards
	                                   + static_cast<float>(kMaxSounds) * SoundStack::kRowGap
	                                   + SoundStack::kPadTop));

	// AND THE CEILING IS SAID SOMEWHERE, because with the row hidden the clone's refusal is the only
	// place it is put into words.
	f.stack.onMouseDown().callback(at(f.clonePoint(0).x, f.clonePoint(0).y));
	CHECK(f.tray.text() == "Max 8 sounds — nowhere to clone to");

	// FREE ONE AND IT COMES BACK. Undo is the road back to an empty slot, so the row has to return the
	// moment there is somewhere for it to put a sound.
	//
	// THE DEFAULT IS ASKED OF THE TABLE, not assumed to be zero — this case first wrote 0.0 here and
	// went red on correct code, because every slot is seeded with its OWN zone (H's is 960) and a slot
	// at somebody else's default is a slot somebody authored. Emptiness reads the table; so does this.
	const ParamDesc* hZone = findParam(static_cast<clap_id>(soundParam(7, SoundField::Zone)));
	REQUIRE(hZone != nullptr);
	f.set(static_cast<ParamId>(soundParam(7, SoundField::Zone)), hZone->def);
	REQUIRE(f.stack.isEmpty(7));
	CHECK(f.stack.newSoundTarget().has_value());
	CHECK(f.stack.contentHeight() > atCeiling - SoundStack::kSlotHeight);
	CHECK(f.stack.hitAt(f.newSoundPoint()).part == SoundStack::Part::NewSound);
	CHECK(f.stack.contentHeight() < roomForOne + 8.0f * SoundStack::kSlotHeight);
}

TEST_CASE("delete frees the slot back to empty, and one undo puts the whole sound back") {
	// THE DECISION, made after hitting the wall the empty-until-consumed rail built: switching a
	// sound off leaves it consumed — that is what protects a parked sound from a clone — so without
	// this verb a patch could reach the ceiling and never make room again except by undo. The ✕ is
	// the way back, and DELETED IS NEVER-USED: the slot after a delete is bit-for-bit the slot nobody
	// ever touched, so everything derived from emptiness comes back by derivation, not by handling.
	StackFixture f;
	// Give A all five kinds of state the record carries, so the undo has everything to prove: an
	// authored identity (the fixture's default), a moved placement, an authored width (V14a — the
	// fifth kind — a RIDE-ALONG since V18, reset and restored with its family), a stored stance,
	// and the switch.
	f.set(ParamId::APlaceX, 0.31);
	f.set(ParamId::APlaceY, -0.22);
	f.set(soundParam(0, SoundField::Width), 0.4);
	f.stack.storeVariationAt(0);
	f.settle();
	const SlotState before = readSlotState(f.bridge, 0);
	REQUIRE(before.rideAlong.width == 0.4);
	REQUIRE_FALSE(f.stack.isEmpty(0));
	REQUIRE_FALSE(before.rideAlong.stances.empty());

	// And make it the one being both edited and heard, so the two follow-behaviours are exercised by
	// the same press that destroys their subject.
	f.selection.select(0);
	f.stack.togglePreview(0);
	REQUIRE(f.stack.previewing(0));

	// The press lands on the ✕ — asked of the widget, and it is the RIGHTMOST verb on the line: the
	// one control that destroys does not sit between two that are safe.
	const visage::Point press = f.deletePoint(0);
	REQUIRE(f.stack.hitAt(press).part == SoundStack::Part::Delete);
	CHECK(f.stack.deleteBoundsForTest(0).x() > f.stack.switchBoundsForTest(0).right());
	f.stack.onMouseDown().callback(at(press.x, press.y));
	f.settle();

	// Gone in every sense the rail has: empty, off screen, no longer heard, no longer selected.
	CHECK(f.stack.isEmpty(0));
	CHECK(f.stack.visibleCount() == 1);
	CHECK_FALSE(f.stack.anyPreviewing());
	CHECK(f.selection.slot() == 1);
	CHECK(f.tray.text() == "Deleted A");

	// ONE undo step restores the WHOLE SlotState — identity, switch, stances, placement. Anything
	// less is the copied-less-than-a-sound bug this record type exists to make impossible.
	REQUIRE(f.history.canUndo());
	f.history.undo();
	f.settle();
	CHECK(readSlotState(f.bridge, 0) == before);
	CHECK_FALSE(f.stack.isEmpty(0));
	CHECK(f.stack.visibleCount() == 2);
}

TEST_CASE("delete at the ceiling brings the new-sound row back") {
	// The wall itself, walked: fill all eight, watch the + row go, delete one, watch it return —
	// by the same derivation that hid it, since a deleted slot IS an empty one.
	StackFixture f;
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		f.consume(slot, 200.0 + static_cast<double>(slot));
	}
	REQUIRE_FALSE(f.stack.newSoundTarget().has_value());
	REQUIRE(f.stack.hitAt(f.newSoundPoint()).part != SoundStack::Part::NewSound);

	const visage::Point press = f.deletePoint(3);
	REQUIRE(f.stack.hitAt(press).part == SoundStack::Part::Delete);
	f.stack.onMouseDown().callback(at(press.x, press.y));
	f.settle();

	CHECK(f.stack.isEmpty(3));
	REQUIRE(f.stack.newSoundTarget().has_value());
	CHECK(*f.stack.newSoundTarget() == 3);
	CHECK(f.stack.hitAt(f.newSoundPoint()).part == SoundStack::Part::NewSound);
}

TEST_CASE("delete is the one verb allowed to touch a consumed slot — a clone still cannot") {
	// The protection the whole empty-until-consumed design turns on, asserted with the new verb in
	// the room: a parked sound is safe from every verb except an explicit press on its own ✕.
	StackFixture f;
	// Park B: AUTHORED, then switched off. Both halves matter — this case's first draft parked a
	// fresh B by switching it off alone and went red on correct code, because a slot at its defaults
	// with its switch off IS empty (deleted and never-used are one state, and so are
	// switched-off-untouched and never-used). Parked means somebody made a sound here first.
	f.consume(1, 700.0);
	f.set(ParamId::BEnable, 0.0);
	REQUIRE_FALSE(f.stack.isEmpty(1));

	// A clone of A spends the first genuinely empty slot and pushes the parked sound down into it,
	// rather than landing on top of it.
	const SlotState parked = readSlotState(f.bridge, 1);
	f.stack.onMouseDown().callback(at(f.clonePoint(0).x, f.clonePoint(0).y));
	f.settle();
	CHECK_FALSE(f.stack.isEmpty(2));
	// The parked sound untouched: exactly as somebody left it, one slot further down.
	CHECK(readSlotState(f.bridge, 2) == parked);
	CHECK(f.read(ParamId::CZone) == doctest::Approx(700.0));
	CHECK_FALSE(f.read(ParamId::CEnable) == 1.0);

	// And the ✕ on its own card is what frees it — the explicit press, nothing else.
	const visage::Point press = f.deletePoint(2);
	REQUIRE(f.stack.hitAt(press).part == SoundStack::Part::Delete);
	f.stack.onMouseDown().callback(at(press.x, press.y));
	f.settle();
	CHECK(f.stack.isEmpty(2));
}

TEST_CASE("a card click selects its sound and nothing else") {
	// RESTATED, NOT RELAXED, for the rail that draws consumed slots only: the claim is about what a
	// click on a CARD does, so the cards it clicks have to exist. Two sounds are authored to make them
	// exist; every assertion below is the original one.
	StackFixture f;
	f.consume(2, 300.0);
	f.consume(5, 700.0);
	REQUIRE(f.selection.slot() == 0);

	f.stack.onMouseDown().callback(at(f.cardPoint(2).x, f.cardPoint(2).y));
	CHECK(f.selection.slot() == 2);
	// Selecting is not editing: a click that only moves the highlight must post nothing, or every
	// glance at a sound would land in the host's undo history.
	CHECK(drain(f.bridge).empty());

	f.stack.onMouseDown().callback(at(f.cardPoint(5).x, f.cardPoint(5).y));
	CHECK(f.selection.slot() == 5);
}

TEST_CASE("the preview button listens to one sound, selects it, and writes nothing at all") {
	// #42: a preview button/mode selects an individual sound even when the puck is off every
	// individual sound. The three claims a preview has to make good on are here together, because
	// they only mean anything together: it aims the engine at ONE sound, it points the editor at that
	// same sound, and it changes NOTHING about the patch.
	StackFixture f;
	f.bridge.refresh();
	// Drain whatever the fixture's own setup left in the queue, so what is counted below is this
	// gesture's doing and not the fixture's.
	f.bridge.drain([](const UiParamEvent&) {});
	(void)f.bridge.takeStateDirty();

	REQUIRE_FALSE(f.stack.anyPreviewing());
	REQUIRE(f.selection.slot() == 0);

	// D — A SOUND SOMEBODY PARKED: authored, and switched OFF. That is the interesting case and it is
	// now the only way a switched-off sound is on screen at all: preview ignores the switch, so a sound
	// that is out of the constellation can still be listened to. (Before slots stayed empty until
	// consumed, every slot was drawn and this case used D untouched; the claim is unchanged, the state
	// it is made in is the one a hand can actually reach.)
	const size_t slot = 3;
	f.consume(slot, 512.0);
	f.bridge.drain([](const UiParamEvent&) {});
	REQUIRE(f.read(ParamId::DEnable) == 0.0);

	const visage::Point button = f.previewPoint(slot);
	REQUIRE(f.stack.hitAt(button).part == SoundStack::Part::Preview);
	REQUIRE(f.stack.hitAt(button).slot == slot);
	f.stack.onMouseDown().callback(at(button.x, button.y));

	// THE ENGINE IS AIMED AT IT — read off the bridge, which is where the audio thread reads it.
	CHECK(f.bridge.preview().active());
	CHECK(f.bridge.preview().slot == slot);
	CHECK(f.stack.previewing(slot));
	// AND THE EDITOR IS POINTED AT IT, because listening to a sound is asking to work on it.
	CHECK(f.selection.slot() == slot);

	// AND NOTHING WAS WRITTEN. Not "no parameter I thought to check" — nothing reached the queue at
	// all, which is the road every parameter write in this editor takes, and the state was not marked
	// dirty either. The switch is still off: it was never the point, and preview did not touch it.
	int posted = 0;
	f.bridge.drain([&posted](const UiParamEvent&) { ++posted; });
	CHECK(posted == 0);
	CHECK_FALSE(f.bridge.takeStateDirty());
	CHECK(f.read(ParamId::DEnable) == 0.0);

	// Pressing it again gives the blend back.
	f.stack.onMouseDown().callback(at(button.x, button.y));
	CHECK_FALSE(f.bridge.preview().active());
	CHECK_FALSE(f.stack.anyPreviewing());
	int postedOff = 0;
	f.bridge.drain([&postedOff](const UiParamEvent&) { ++postedOff; });
	CHECK(postedOff == 0);
}

TEST_CASE("a latched preview follows the sound being edited") {
	// The design: latched, and it follows the selection. That makes preview a MODE with one piece of
	// state rather than a per-card sticky, and it is the behaviour the feature exists for — you move
	// from sound to sound editing them, and what you hear has to move with you rather than being left
	// on the one you started at.
	//
	// The carrying is done by the editor (gui.cpp hangs it off SoundSelection::onMoved), so the wiring
	// is stated here as the editor states it, and the case drives the real cards on both ends.
	StackFixture f;
	f.selection.onMoved([&f](size_t slot) {
		if (f.bridge.preview().active()) {
			f.bridge.setPreview(slot);
		}
	});

	const visage::Point button = f.previewPoint(0);
	f.stack.onMouseDown().callback(at(button.x, button.y));
	REQUIRE(f.bridge.preview().slot == 0);

	// Click another CARD — not its preview button — the way a hand moves on to the next sound.
	f.stack.onMouseDown().callback(at(f.cardPoint(2).x, f.cardPoint(2).y));
	REQUIRE(f.selection.slot() == 2);
	CHECK(f.bridge.preview().active());
	CHECK(f.bridge.preview().slot == 2);

	// And with no preview latched, moving the selection starts nothing: the mode is off and stays off.
	f.stack.onMouseDown().callback(at(f.previewPoint(2).x, f.previewPoint(2).y));
	REQUIRE_FALSE(f.bridge.preview().active());
	f.stack.onMouseDown().callback(at(f.cardPoint(1).x, f.cardPoint(1).y));
	CHECK(f.selection.slot() == 1);
	CHECK_FALSE(f.bridge.preview().active());
}

TEST_CASE("the blend circle stops claiming you can hear the sounds a preview has silenced") {
	// THE TRUTHFULNESS HALF, and it is not decoration. The circle is a picture of the blend; a preview
	// suspends the blend; so a Perform surface drawing eight equal stars while one sound plays is the
	// surface most confidently lying about what is coming out of the plugin.
	//
	// Asserted on what the DRAW READS — heardAlpha is the number drawSounds and drawTour multiply
	// their alphas by — rather than on a picture of it, which is ui-10's law from the flicker arc:
	// a measurement of the artefact is not a measurement of the frame.
	StageFixture f;
	f.set(ParamId::AEnable, 1.0);
	f.set(ParamId::BEnable, 1.0);
	REQUIRE(f.stage.heardAlpha(0) == 1.0f);
	REQUIRE(f.stage.heardAlpha(1) == 1.0f);
	REQUIRE_FALSE(f.stage.unpatchedPreviewStar().has_value());

	f.bridge.setPreview(1);
	CHECK(f.stage.heardAlpha(1) == 1.0f);
	CHECK(f.stage.heardAlpha(0) == PerformStage::kUnheardAlpha);
	CHECK(f.stage.heardAlpha(0) < 1.0f);
	// B is in the patch, so it is already in the sky and needs no second star.
	CHECK_FALSE(f.stage.unpatchedPreviewStar().has_value());

	// A SOUND THAT IS NOT IN THE PATCH still has to appear, or the circle would show a sky of stars
	// none of which you can hear. It is drawn at its own placement, under its own letter.
	f.bridge.setPreview(4);
	REQUIRE(f.stage.unpatchedPreviewStar().has_value());
	CHECK(f.stage.unpatchedPreviewStar()->slot == 4);
	CHECK(f.stage.unpatchedPreviewStar()->label == 'E');
	CHECK(f.stage.heardAlpha(0) == PerformStage::kUnheardAlpha);
	CHECK(f.stage.heardAlpha(1) == PerformStage::kUnheardAlpha);

	// And with the preview off, every sound in the patch is heard again and nothing extra is drawn.
	f.bridge.clearPreview();
	CHECK(f.stage.heardAlpha(0) == 1.0f);
	CHECK(f.stage.heardAlpha(1) == 1.0f);
	CHECK_FALSE(f.stage.unpatchedPreviewStar().has_value());
}

TEST_CASE("clone copies the whole sound in beside its source and switches it on") {
	// Cloning was missing from the tree view. A4 left the mock's ⧉ out because
	// eight fixed slots have nothing to create or destroy — true of the SLOTS, false of the SOUNDS.
	StackFixture f;
	// Give A an identity worth copying, on every axis a sound has: the two that are parameters, the
	// drift that is state, and the plane and residue a rotation banks. A clone that copied fewer of
	// these than an undo restores would be a second opinion about what a sound is — which is the bug
	// this project has already had twice (A5, A26).
	f.set(ParamId::AZone, 640.0);
	f.set(ParamId::AOffset, 0.37);
	f.set(ParamId::AGamma, -1.25);
	f.bridge.beginSoundTravelGesture();
	f.bridge.setSoundTravel(0, { 0.021, -0.013 });
	f.bridge.endSoundTravelGesture();
	f.bridge.beginSoundGammaGesture();
	f.bridge.setSoundGamma(0, 61.25);
	f.bridge.endSoundGammaGesture();
	SliceOrientationPin pinned;
	pinned.pinned = true;
	pinned.generator.rotate = 0.44f;
	f.bridge.beginSoundOrientationGesture();
	f.bridge.setSoundOrientation(0, pinned);
	f.bridge.endSoundOrientationGesture();
	PlaneResidue banked;
	banked.morph.offset[0] = 0.017;
	f.bridge.setSoundResidue(0, banked);
	f.bridge.endSoundOrientationGesture();

	// C is the first slot nothing has been authored in — the table is born with A and B on — so C is
	// the slot this act SPENDS. Where the copy APPEARS is B, immediately after its source, and B's own
	// sound moves down into C. Both asked rather than assumed, because the button has to be able to say
	// the first before it is pressed.
	REQUIRE(f.stack.cloneTarget().has_value());
	CHECK(*f.stack.cloneTarget() == 2);
	REQUIRE(f.stack.cloneDestination(0).has_value());
	CHECK(*f.stack.cloneDestination(0) == 1);

	const SoundIdentity source = readIdentity(f.bridge, 0);
	f.stack.clone(0);
	// Drain what the editor posted back, the way the plugin's binding does, so the copy is visible.
	f.bridge.drain([&](const UiParamEvent& event) {
		if (event.kind == UiParamEvent::Kind::Value) {
			f.params.setBase(event.id, event.value);
		}
	});
	f.bridge.publish(f.params);
	f.bridge.refresh();

	// THE WHOLE IDENTITY ARRIVED — compared as one object, so a field added to SoundIdentity later and
	// forgotten by the VERB fails rather than passing quietly.
	CHECK(readIdentity(f.bridge, 1) == source);
	// AND THE STATE ITSELF SAYS SO, which is a different claim and the one that survives a reader that
	// lies. `readIdentity` is used by the verb and by the line above it, so a field the READER drops
	// reads as zero on both sides and the comparison passes — measured, not supposed: dropping the
	// base from readIdentity leaves this case green and fails two others. Each channel is therefore
	// also checked where it actually lives.
	CHECK(f.bridge.soundGamma().base[1] == doctest::Approx(61.25));
	CHECK(f.bridge.soundTravel().offset[1].x == doctest::Approx(0.021));
	CHECK(f.bridge.soundOrientations().pin[1].pinned);
	CHECK(f.bridge.soundResidue().banked[1].morph.offset[0] == doctest::Approx(0.017));
	// AND IT IS AUDIBLE: a clone you cannot hear is not a clone.
	CHECK(steppedIndex(f.read(static_cast<ParamId>(soundParam(1, SoundField::Enable))),
	                   kEnableNames.size())
	      != 0);
	// The clone is what you are now working on, the same rule the switch already follows.
	CHECK(f.selection.slot() == 1);
	// The SOURCE is untouched — a copy that moved the original would be a rename.
	CHECK(readIdentity(f.bridge, 0) == source);
}

TEST_CASE("the active stance is the record you are editing, and a recall switches records") {
	// THE SECOND LEVEL OF THE TREE under its two design decisions. V19: editing a sound MUTATES its active
	// stance (the active row is the live record, by derivation), there is no step-off, the light is
	// an active-stance indicator, preserving is the clone-stance verb FIRST, and a recall switches
	// which record you occupy. V20: what the record HOLDS is the EXPLORE BOX — everything in
	// the explore box, but not the tray or anything outside it — so this case walks the model on the
	// ZONE axis, the exploration itself, and pins that PLACE stays untouched throughout: "place
	// leaves the stance", a variant switch no longer moves the star.
	StackFixture f;
	f.set(ParamId::AZone, 100.0);
	f.set(ParamId::APlaceX, -0.5);

	// A sound that has stored nothing still draws ONE row: the implicit active record its live
	// state holds — the row this rail has always drawn, and under V19 also the record any edit is
	// mutating.
	REQUIRE(f.stack.variationRows(0) == 1);
	CHECK(f.stack.variationValuesAt(0, 0).zone == doctest::Approx(100.0));
	CHECK(f.stack.activeStance(0, 0));

	// Preserve it. The mock's "clone variation": the original banks what it holds, the COPY becomes
	// the record your next edits mutate.
	f.stack.onMouseDown().callback(at(f.variationStorePoint(0, 0).x, f.variationStorePoint(0, 0).y));
	f.settle();
	REQUIRE(f.stack.variationRows(0) == 2);
	// STORING MOVES NOTHING, and the light moved to the copy — one active record, not a comparison.
	CHECK(f.read(ParamId::AZone) == doctest::Approx(100.0));
	CHECK_FALSE(f.stack.activeStance(0, 0));
	CHECK(f.stack.activeStance(0, 1));

	// Explore somewhere else — a scrub, or automation, or the host's own panel. THE ACTIVE ROW
	// MOVES WITH YOU: editing the sound is editing its active stance, so the row reads the live
	// state ("the active one simply updates as you play") while the banked original stands.
	f.set(ParamId::AZone, 800.0);
	CHECK(f.stack.variationValuesAt(0, 1).zone == doctest::Approx(800.0));
	CHECK(f.stack.variationValuesAt(0, 0).zone == doctest::Approx(100.0));
	// And the light did not blink: there is no stepped-off state for it to be honest about.
	CHECK(f.stack.activeStance(0, 1));

	// Preserve this one too, so the sound knows two explorations and is editing a third.
	f.stack.onMouseDown().callback(at(f.variationStorePoint(0, 1).x, f.variationStorePoint(0, 1).y));
	f.settle();
	REQUIRE(f.stack.variationRows(0) == 3);
	CHECK(f.stack.variationValuesAt(0, 2).zone == doctest::Approx(800.0));

	// Wander again — the third record wears it, the first two stand banked.
	f.set(ParamId::AZone, 400.0);
	CHECK(f.stack.variationValuesAt(0, 2).zone == doctest::Approx(400.0));
	CHECK(f.stack.variationValuesAt(0, 1).zone == doctest::Approx(800.0));

	// SWITCH RECORDS. The outgoing stance BANKS the exploration the sound stood at — hand edits and
	// automation alike — and the incoming one's stored box comes into the live state. THE PLACE
	// DOES NOT MOVE (V20): the star stands where Perform put it, whatever record you occupy.
	f.stack.onMouseDown().callback(at(f.variationPoint(0, 0).x, f.variationPoint(0, 0).y));
	f.settle();
	CHECK(f.read(ParamId::AZone) == doctest::Approx(100.0));
	CHECK(f.stack.activeStance(0, 0));
	// The banked record remembers the wander — that is the write-through, observed in storage.
	CHECK(f.stack.variationValuesAt(0, 2).zone == doctest::Approx(400.0));
	CHECK(f.read(ParamId::APlaceX) == doctest::Approx(-0.5));

	// One undo step per recall, and it puts the exploration back where it was.
	REQUIRE(f.history.canUndo());
	f.history.undo();
	f.settle();
	CHECK(f.read(ParamId::AZone) == doctest::Approx(400.0));
	CHECK(f.stack.activeStance(0, 2));
	CHECK(f.read(ParamId::APlaceX) == doctest::Approx(-0.5));
}

TEST_CASE("a sound is never unplaced, and forgetting a stance leaves the sound where it is") {
	// The mock says it outright: "a sound is never unplaced — the last variation can't be removed".
	StackFixture f;
	f.set(ParamId::BPlaceX, 0.5);
	f.set(ParamId::BPlaceY, 0.0);
	f.stack.onMouseDown().callback(at(f.variationStorePoint(1, 0).x, f.variationStorePoint(1, 0).y));
	f.settle();
	REQUIRE(f.stack.variationRows(1) == 2);

	// Forgetting one leaves the sound standing exactly where it was: you forgot a place you had been,
	// not a place you are.
	f.stack.onMouseDown().callback(at(f.variationForgetPoint(1, 1).x, f.variationForgetPoint(1, 1).y));
	f.settle();
	CHECK(f.stack.variationRows(1) == 1);
	CHECK(f.read(ParamId::BPlaceX) == doctest::Approx(0.5));
	CHECK(f.tray.text() == "Sound B forgot a place");

	// And the last one stays, out loud rather than silently.
	f.stack.onMouseDown().callback(at(f.variationForgetPoint(1, 0).x, f.variationForgetPoint(1, 0).y));
	f.settle();
	CHECK(f.stack.variationRows(1) == 1);
	CHECK(f.tray.text() == "A sound is never unplaced — this is its last place");

	// Undo brings back the stance that was forgotten, which is the whole reason the verb is recorded.
	REQUIRE(f.history.canUndo());
	f.history.undo();
	f.settle();
	CHECK(f.stack.variationRows(1) == 2);
}

TEST_CASE("a cloned sound remembers the explorations its original knew") {
	// ASKED FOR CLONING "FOR SOUNDS AND SUBSOUNDS", and this is the subsound half: a copy that
	// arrived remembering none of what its original knows would be the shallow version of the verb
	// — A5's forgotten drift, one level up. Since V20 what a stance knows is the EXPLORE BOX, so
	// the memories walked here are zone moves, and the clone's PLACE stays its own (A34, and
	// "place leaves the stance"): the pair is two stars, never one on top of the other.
	StackFixture f;
	f.set(ParamId::AZone, 512.0);
	f.stack.onMouseDown().callback(at(f.variationStorePoint(0, 0).x, f.variationStorePoint(0, 0).y));
	f.settle();
	f.set(ParamId::AZone, 217.0);
	f.stack.onMouseDown().callback(at(f.variationStorePoint(0, 1).x, f.variationStorePoint(0, 1).y));
	f.settle();
	REQUIRE(f.stack.variationRows(0) == 3);

	f.stack.onMouseDown().callback(at(f.clonePoint(0).x, f.clonePoint(0).y));
	f.settle();

	// The copy knows the same explorations. It lands in B — beside its source — and B's sound moves
	// to C. THE BANKED RECORDS MATCH; the ACTIVE row of each sound is its own live state (V19 — the
	// active stance is the live record), and the clone's live state was copied from the source
	// (V18), so even the active rows agree here — while the PLACES differ, which is A34 doing its
	// job.
	REQUIRE(f.stack.variationRows(1) == 3);
	for (size_t index = 0; index < 3; ++index) {
		CAPTURE(index);
		CHECK(f.stack.variationValuesAt(1, index) == f.stack.variationValuesAt(0, index));
	}
	CHECK(f.bridge.soundVariations().sound[1] == f.bridge.soundVariations().sound[0]);
	CHECK(f.read(ParamId::BPlaceX) != doctest::Approx(f.read(ParamId::APlaceX)));

	// And one undo takes the whole clone back, stances included — every slot the splice touched.
	f.history.undo();
	f.settle();
	CHECK(f.stack.variationRows(1) == 1);
	CHECK(f.stack.isEmpty(2));
}

TEST_CASE("the rail makes room for the rows a sound grows") {
	// The geometry stopped being a multiplication when slots grew rows: a sound that remembers four
	// places is taller than one that remembers none, and the sounds below it move down rather than
	// being drawn over. This is the claim the whole second level rests on visually.
	StackFixture f;
	const float before = f.stack.contentHeight();
	const visage::Bounds cardBelow = f.stack.variationBoundsForTest(1, 0);

	f.stack.onMouseDown().callback(at(f.variationStorePoint(0, 0).x, f.variationStorePoint(0, 0).y));
	f.settle();
	REQUIRE(f.stack.variationRows(0) == 2);

	CHECK(f.stack.contentHeight() == doctest::Approx(before + SoundStack::kPlaceHeight));
	// The sound below moved down by exactly one row, and its row still answers a press.
	const visage::Bounds movedBelow = f.stack.variationBoundsForTest(1, 0);
	CHECK(movedBelow.y() == doctest::Approx(cardBelow.y() + SoundStack::kPlaceHeight));
	const SoundStack::Hit hit = f.stack.hitAt({ movedBelow.x() + 30.0f,
		                                        movedBelow.y() + movedBelow.height() * 0.5f });
	CHECK(hit.part == SoundStack::Part::Variation);
	CHECK(hit.slot == 1);
	// And the new row of the sound above answers as its own row rather than as somebody else's.
	const SoundStack::Hit second = f.stack.hitAt(f.variationPoint(0, 1));
	CHECK(second.part == SoundStack::Part::Variation);
	CHECK(second.slot == 0);
	CHECK(second.index == 1);
}

TEST_CASE("a clone is one undo step, and it puts back the sound it landed on") {
	// THE GAP THIS CLOSES, and it is the reason the history left the Explore surface. A clone writes a
	// whole sound into a slot and switches it on; the history lived in another widget with no road to
	// this one; so a clone was the single edit in this editor that could not be taken back. Worse, the
	// slot it lands in is the first one switched OFF, and a switched-off slot may be a sound somebody
	// PARKED there — destroyed silently, with nothing to walk back to.
	StackFixture f;
	// Park a sound in C and switch it off, which is exactly what a hand does with a sound it is not
	// using but is not finished with either.
	f.set(ParamId::CZone, 815.0);
	f.set(ParamId::COffset, -0.21);
	f.set(ParamId::CEnable, 0.0);
	f.set(ParamId::AZone, 128.0);
	const SlotState parked = readSlotState(f.bridge, 2);
	REQUIRE_FALSE(parked.enabled);
	// RESTATED TWICE. It was written when a clone landed in the first switched-OFF slot, so it landed
	// ON the parked sound and the claim was that undo put it back; empty-until-consumed made the clone
	// skip it; and the splice now MOVES it, one slot down, whole. The original worry is answered better
	// than by an undo either way: the parked sound is never written over, asserted below.
	REQUIRE(f.stack.cloneTarget().has_value());
	REQUIRE(*f.stack.cloneTarget() == 3);
	REQUIRE(f.history.length() == 0);

	f.stack.onMouseDown().callback(at(f.clonePoint(0).x, f.clonePoint(0).y));
	f.settle();

	// The clone happened: B sounds like A and is in the patch.
	REQUIRE(readIdentity(f.bridge, 1) == readIdentity(f.bridge, 0));
	REQUIRE(f.read(ParamId::BEnable) == 1.0);
	// AND THE PARKED SOUND IS UNTOUCHED — the thing the confusion was really about. A clone can no
	// longer replace a sound somebody made, so it says "to" and means it.
	CHECK(readSlotState(f.bridge, 3) == parked);
	CHECK(f.tray.text() == "Sound A cloned to B");

	// ONE STEP, not four — the identity's three parameters, the switch and the state channels are one
	// act because a hand made one gesture.
	REQUIRE(f.history.length() == 1);

	f.history.undo();
	f.settle();
	// The spent slot goes back to being empty, which is what "putting it back" means: the rail stops
	// drawing it, and the sound that was never there is not there.
	CHECK(f.read(ParamId::DEnable) == 0.0);
	CHECK(f.stack.isEmpty(3));
	// And the parked sound is back in the slot it was parked in, through all of it.
	CHECK(readSlotState(f.bridge, 2) == parked);
	CHECK(f.read(ParamId::CZone) == doctest::Approx(815.0));

	// And forward again.
	f.history.redo();
	f.settle();
	CHECK(readIdentity(f.bridge, 1) == readIdentity(f.bridge, 0));
	CHECK(f.read(ParamId::BEnable) == 1.0);
}

TEST_CASE("a clone copies the sound as AUTHORED, not as a modulator is currently bending it") {
	// A CONSTRAINT FROM THE MODULATION PANEL'S DESIGN, made a case here before there is a modulator to
	// break it. `readIdentity` reads value() — the BASE, what the patch and automation own — and never
	// modulated(). The two agree today for the reason that makes this worth pinning now: nothing
	// modulates a sound's identity yet, so the wrong reading would be invisible until the mod panel
	// lands, and then permanent — a clone taken while a modulator happened to be at +0.3 would bake
	// that +0.3 into a new sound's identity forever, and no undo would explain where it came from.
	//
	// A REAL MODULATION OFFSET, not a simulated one: the sound parameters carry
	// CLAP_PARAM_IS_MODULATABLE, so the offset goes through the same lane a host's would.
	StackFixture f;
	f.set(ParamId::AZone, 300.0);
	f.set(ParamId::AOffset, 0.5);

	f.params.setModOffset(static_cast<clap_id>(ParamId::AZone), 200.0);
	f.params.setModOffset(static_cast<clap_id>(ParamId::AOffset), 0.25);
	f.bridge.publish(f.params);
	f.bridge.refresh();
	// The bend is really there, or the case below would pass on a modulation that never arrived.
	REQUIRE(f.bridge.modulated(static_cast<clap_id>(ParamId::AZone)) == doctest::Approx(500.0));
	REQUIRE(f.bridge.value(static_cast<clap_id>(ParamId::AZone)) == doctest::Approx(300.0));

	f.stack.onMouseDown().callback(at(f.clonePoint(0).x, f.clonePoint(0).y));
	f.settle();

	// THE COPY IS THE AUTHORED SOUND. 300, not 500. It lands in B, beside its source.
	CHECK(f.read(ParamId::BZone) == doctest::Approx(300.0));
	CHECK(f.read(ParamId::BOffset) == doctest::Approx(0.5));
	CHECK(readIdentity(f.bridge, 1) == readIdentity(f.bridge, 0));
}

TEST_CASE("a clone into a slot nobody has touched says so plainly") {
	// The other half of the sentence above: an empty slot is not something you overwrote, and telling
	// a player they replaced a sound they never made would send them looking for what they lost.
	StackFixture f;
	f.set(ParamId::AZone, 300.0);
	f.stack.onMouseDown().callback(at(f.clonePoint(0).x, f.clonePoint(0).y));
	f.settle();
	CHECK(f.tray.text() == "Sound A cloned to B");
	CHECK(f.history.length() == 1);
}

TEST_CASE("a clone keeps its own place in the circle rather than standing on its source") {
	// THE ONE DESIGN CHOICE INSIDE THE VERB, and the line this whole session's arc is drawn along:
	// PlaceX/PlaceY are where a sound is PLAYED FROM, not what it IS. So they do not travel with a
	// clone. Two stars on one anchor would also be a zero-length leg of the tour and two glyphs nobody
	// could tell apart, which is the practical half of the same answer.
	StackFixture f;
	f.set(ParamId::APlaceX, -0.42);
	f.set(ParamId::APlaceY, 0.31);
	// THE PLACE THE SPENT SLOT FREED, which is C's: the copy appears in B, and the ring position that
	// this act actually released is the one belonging to the empty slot it consumed. Quoting the
	// DESTINATION's old place instead would stand the copy on top of the sound that moved down into it.
	const double targetX = f.read(static_cast<ParamId>(soundParam(2, SoundField::PlaceX)));
	const double targetY = f.read(static_cast<ParamId>(soundParam(2, SoundField::PlaceY)));

	f.stack.clone(0);
	f.bridge.drain([&](const UiParamEvent& event) {
		if (event.kind == UiParamEvent::Kind::Value) {
			f.params.setBase(event.id, event.value);
		}
	});
	f.bridge.publish(f.params);
	f.bridge.refresh();

	CHECK(f.read(static_cast<ParamId>(soundParam(1, SoundField::PlaceX)))
	      == doctest::Approx(targetX));
	CHECK(f.read(static_cast<ParamId>(soundParam(1, SoundField::PlaceY)))
	      == doctest::Approx(targetY));
	// And the two really are in different places, which is what makes the pair usable at all.
	CHECK(f.read(ParamId::APlaceX) != doctest::Approx(targetX));
}

TEST_CASE("the ride-alongs follow the clone: the family from the source, the place from the spent slot") {
	// V18: under cloning, the "ride along" sound params (now including width, later more) follow
	// the clone and the stance. This REGOLDS the V14a-era claim that a
	// clone stands at its own slot's width — the copy is the same SOUND, so its character travels;
	// only the PLACE keeps the spent-slot rule (the case above). Compared as ONE RideAlong object,
	// so a value member added to the family later and forgotten by the verb fails here rather than
	// passing quietly — the stage-3 birth constraint, applied.
	StackFixture f;
	f.set(static_cast<ParamId>(soundParam(0, SoundField::Width)), 0.55);
	f.stack.storeVariationAt(0);
	f.settle();
	const RideAlong source = readSlotState(f.bridge, 0).rideAlong;
	REQUIRE(source.width == 0.55);
	REQUIRE_FALSE(source.stances.empty());

	f.stack.clone(0);
	f.settle();

	// The whole family arrived — the one-object compare.
	CHECK(readSlotState(f.bridge, 1).rideAlong == source);
	// AND THE PARAM SAYS SO WHERE IT LIVES, the claim that survives a reader that lies — and the
	// KNOWN-BAD: under the overturned behaviour the copy took the SPENT slot's width, which is the
	// table default of 0, so this line is exactly the one the old code fails.
	CHECK(f.read(static_cast<ParamId>(soundParam(1, SoundField::Width))) == doctest::Approx(0.55));
	// And the source kept its own — copied, not moved.
	CHECK(f.read(static_cast<ParamId>(soundParam(0, SoundField::Width))) == doctest::Approx(0.55));
}

TEST_CASE("clones get independent copies — they aren't linked, in either direction") {
	// V18: clones get INDEPENDENT copies; they are not linked. True today by
	// construction — every value moves through the param road, no slot references another — and
	// pinned EXPLICITLY so no future shared-storage or copy-on-write shortcut can silently link
	// them: mutate the SOURCE on every family channel and the clone must not move; then mutate the
	// CLONE and the source must not move.
	StackFixture f;
	f.set(static_cast<ParamId>(soundParam(0, SoundField::Width)), 0.4);
	f.stack.storeVariationAt(0);
	f.settle();
	f.stack.clone(0);
	f.settle();
	const SlotState cloneBorn = readSlotState(f.bridge, 1);

	// Mutate the source: its width, its stance list, and an identity scrub.
	f.set(static_cast<ParamId>(soundParam(0, SoundField::Width)), 0.9);
	f.stack.storeVariationAt(0);
	f.set(static_cast<ParamId>(soundParam(0, SoundField::Zone)), 512.0);
	f.settle();
	CHECK(readSlotState(f.bridge, 1) == cloneBorn);

	// And the other direction: the clone moves, the source stands.
	const SlotState sourceNow = readSlotState(f.bridge, 0);
	f.set(static_cast<ParamId>(soundParam(1, SoundField::Width)), 0.1);
	f.stack.storeVariationAt(1);
	f.set(static_cast<ParamId>(soundParam(1, SoundField::Zone)), 77.0);
	f.settle();
	CHECK(readSlotState(f.bridge, 0) == sourceNow);
}

TEST_CASE("the active stance carries the width you are playing, and a switch banks it") {
	// REGOLDED TWICE, each time by a design decision. V18 put the ride-alongs INTO the stance; V19 then made
	// the stance write-through — the two-places invariant became STRUCTURAL, one record: while a
	// stance is active there is no second copy to drift, because the live parameters ARE its
	// contents. So turning the width knob mutates the active stance (as a stance is meant
	// to be mutated), the banked sibling stands, a switch banks the outgoing record and
	// restores the incoming one whole, and the undo un-widens exactly as it un-moves.
	StackFixture f;
	f.set(static_cast<ParamId>(soundParam(0, SoundField::Width)), 0.6);
	f.stack.storeVariationAt(0); // banks the original at 0.6; the copy is now the record edits hit
	f.settle();
	const auto width0 = [&] {
		return f.read(static_cast<ParamId>(soundParam(0, SoundField::Width)));
	};

	// Turn the knob: the ACTIVE stance wears it, the light stays, the banked original stands.
	f.set(static_cast<ParamId>(soundParam(0, SoundField::Width)), 0.2);
	CHECK(f.stack.activeStance(0, 1));
	CHECK(f.stack.variationValuesAt(0, 1).width == doctest::Approx(0.2));
	CHECK(f.stack.variationValuesAt(0, 0).width == doctest::Approx(0.6));

	// Switch to the original: the outgoing record banks 0.2, the original's 0.6 comes back whole.
	f.stack.activateVariation(0, 0);
	f.settle();
	CHECK(width0() == doctest::Approx(0.6));
	CHECK(f.stack.activeStance(0, 0));
	CHECK(f.stack.variationValuesAt(0, 1).width == doctest::Approx(0.2));

	// And one undo of the switch un-widens exactly as it un-moves — banked contents included.
	f.history.undo();
	f.settle();
	CHECK(width0() == doctest::Approx(0.2));
	CHECK(f.stack.activeStance(0, 1));
	CHECK(f.stack.variationValuesAt(0, 0).width == doctest::Approx(0.6));
}

TEST_CASE("a recall restores the whole box, and a switch banks the whole wander") {
	// THE REVIEW'S MUTATION FINDING, closed at the road that plays. The verb-level pins above
	// exercise zone and width, so writeRideAlong survived four mutations green: losing its RESIDUE
	// write — the turn-restored-without-its-bank A26 forbids — its PLANE write, its DRIFT write,
	// and OFFSET+GAMMA-BASE together. The wire round trips pin STORAGE of every member; nothing
	// pinned the recall and bank ROADS beyond two channels. This case does both at the ONE-OBJECT
	// level with every member non-default, so a dropped channel — today's seven or any future
	// member, which joins RideAlongValues and therefore joins these equalities automatically —
	// reds here first. That is V20's standing membership rule given teeth where it plays: a
	// control whose value stances don't RESTORE is a defect, not merely one they don't store.
	StackFixture f;
	// Every member of the box non-default, through the channels a hand or host actually drives.
	f.set(static_cast<ParamId>(soundParam(0, SoundField::Width)), 0.4);
	f.set(ParamId::AZone, 512.0);
	f.set(ParamId::AOffset, 0.3);
	f.bridge.beginSoundGammaGesture();
	f.bridge.setSoundGamma(0, 61.25);
	f.bridge.endSoundGammaGesture();
	f.bridge.beginSoundTravelGesture();
	f.bridge.setSoundTravel(0, { 0.02, -0.01 });
	f.bridge.endSoundTravelGesture();
	f.bridge.beginSoundOrientationGesture();
	f.bridge.setSoundOrientation(0, { true, { 0.44f } });
	PlaneResidue turned {};
	turned.morph.offset[0] = 0.017;
	turned.stair.offset[3] = -0.005;
	f.bridge.setSoundResidue(0, turned);
	f.bridge.endSoundOrientationGesture();

	const RideAlongValues stored = f.stack.liveRideAlong(0);
	f.stack.storeVariationAt(0); // banks the exploration at row 0; the copy is the record edits hit
	f.settle();
	REQUIRE(f.stack.activeStance(0, 1));

	// Wander on EVERY axis, so a channel the bank drops cannot hide behind an unchanged value.
	f.set(static_cast<ParamId>(soundParam(0, SoundField::Width)), 0.15);
	f.set(ParamId::AZone, 800.0);
	f.set(ParamId::AOffset, 0.7);
	f.bridge.beginSoundGammaGesture();
	f.bridge.setSoundGamma(0, -3.5);
	f.bridge.endSoundGammaGesture();
	f.bridge.beginSoundTravelGesture();
	f.bridge.setSoundTravel(0, { -0.3, 0.22 });
	f.bridge.endSoundTravelGesture();
	f.bridge.beginSoundOrientationGesture();
	f.bridge.setSoundOrientation(0, { true, { -1.2f } });
	PlaneResidue rewound {};
	rewound.morph.offset[7] = -0.4;
	f.bridge.setSoundResidue(0, rewound);
	f.bridge.endSoundOrientationGesture();
	const RideAlongValues wandered = f.stack.liveRideAlong(0);
	REQUIRE_FALSE(wandered == stored); // or the equalities below could not discriminate

	// The switch: the recall restores the WHOLE stored box, one object...
	f.stack.activateVariation(0, 0);
	f.settle();
	CHECK(f.stack.liveRideAlong(0) == stored);
	// ...and the outgoing record banked the WHOLE wander, one object.
	CHECK(f.stack.variationValuesAt(0, 1) == wandered);
	// And back again, so both roads discriminate in both directions.
	f.stack.activateVariation(0, 1);
	f.settle();
	CHECK(f.stack.liveRideAlong(0) == wandered);
	CHECK(f.stack.variationValuesAt(0, 0) == stored);
}

TEST_CASE("a press on a stored variation row recalls, through the real mouse gesture") {
	// FROM A LIVE REGRESSION on an installed binary (the sibling branch): a variation-row click
	// stopped recalling, prime suspect the drag-to-reorder arming swallowing the row press — and
	// the hole was both trees': nothing drove a row CLICK through the real mouse events anywhere.
	// This is the whole gesture a mouse actually makes — down on the row, a jitter smaller than the
	// reorder threshold, up — asserting the recall under the V19 model: the act switched, the
	// outgoing record banked what it was mirroring, the stored place reached the parameters, and
	// the jitter stayed a click rather than arming a drag (arming is the CARD body's alone, so a
	// routing change that let it eat row presses fails here first).
	StackFixture f;
	f.set(ParamId::AZone, 300.0);
	f.stack.storeVariationAt(0); // banks zone 300; the copy (row 1) is the record edits mutate
	f.set(ParamId::AZone, 800.0);
	f.settle();
	REQUIRE(f.stack.variationRows(0) == 2);
	REQUIRE(f.stack.activeStance(0, 1));

	const visage::Point row = f.variationPoint(0, 0);
	REQUIRE(f.stack.hitAt(row).part == SoundStack::Part::Variation);
	f.stack.onMouseDown().callback(at(row.x, row.y));
	static_assert(SoundStack::kDragThreshold > 2.0f, "the jitter below must stay under the threshold");
	f.stack.onMouseDrag().callback(at(row.x, row.y + 2.0f));
	f.stack.onMouseUp().callback(at(row.x, row.y + 2.0f));
	f.settle();

	// The recall happened: the pressed row is the active record and its exploration is live.
	CHECK(f.stack.activeStance(0, 0));
	CHECK(f.zoneOf(0) == doctest::Approx(300.0));
	// The outgoing record banked the stance it was mirroring.
	CHECK(f.stack.variationValuesAt(0, 1).zone == doctest::Approx(800.0));
	// And the jitter reordered nothing: the rows are intact and no drag armed.
	CHECK_FALSE(f.stack.dragSlot().has_value());
	CHECK(f.stack.variationRows(0) == 2);
}

TEST_CASE("forgetting the record you occupy lands you in the neighbour's") {
	// V19's one forced consequence at the forget verb: there is no stepped-off state to remain in,
	// so deleting the ACTIVE record cannot leave the sound standing in it — the neighbour the verb
	// makes active RECALLS, exactly as clicking its row would (the mock's model can only do the
	// same: delete `act` and the view is the new act's). Forgetting a place you HAD BEEN still
	// moves nothing — the sibling case pins that on identical stances; this one makes the records
	// differ so the two rules are told apart.
	StackFixture f;
	f.set(ParamId::AZone, 200.0);
	f.stack.storeVariationAt(0); // banks the original at zone 200; the copy is active
	f.set(ParamId::AZone, 800.0); // the active record wears the exploration
	f.settle();

	// Forget the banked sibling: nothing moves.
	f.stack.forgetVariationAt(0, 0);
	f.settle();
	REQUIRE(f.stack.variationRows(0) == 1);
	CHECK(f.read(ParamId::AZone) == doctest::Approx(800.0));

	// Rebuild the pair and forget the ACTIVE one: the sound lands in the banked record.
	f.stack.storeVariationAt(0); // banks 800; copy active
	f.set(ParamId::AZone, 350.0);
	f.settle();
	f.stack.forgetVariationAt(0, 1);
	f.settle();
	CHECK(f.read(ParamId::AZone) == doctest::Approx(800.0));
	CHECK(f.stack.activeStance(0, 0));

	// And the undo restores both the record and the standing.
	f.history.undo();
	f.settle();
	CHECK(f.read(ParamId::AZone) == doctest::Approx(350.0));
}

TEST_CASE("editing a fresh sound stores nothing — the implicit record is the parameters") {
	// THE EMPTINESS PICK, on the record (the sister session's constraint, resolved deliberately):
	// under write-through a sound that carried an always-present birth stance would make "has
	// stances" stop discriminating fresh from authored. Chosen: the birth record is IMPLICIT — the
	// live parameters ARE the active record while the list is empty, and NO ordinary edit
	// materialises a stored stance; only the explicit clone-stance verb does. So fresh sounds store
	// nothing (delete's bit-for-bit never-used holds trivially), the wire stays sparse, emptiness
	// keeps deriving from was-authored exactly as the pinned cases state it, and the mock's
	// always-one-row is presentation — the accessors' empty-list fallbacks. Clone-stance-first
	// still preserves: the first store banks the implicit record as a real one and hands you the
	// copy.
	StackFixture f;
	f.set(ParamId::AZone, 412.0);
	f.set(soundParam(0, SoundField::Width), 0.7);
	f.settle();
	CHECK(f.bridge.soundVariations().sound[0].empty());
	// The one drawn row is the implicit active record, wearing the edits.
	REQUIRE(f.stack.variationRows(0) == 1);
	CHECK(f.stack.activeStance(0, 0));
	CHECK(f.stack.variationValuesAt(0, 0).zone == doctest::Approx(412.0));
	CHECK(f.stack.variationValuesAt(0, 0).width == doctest::Approx(0.7));
	// And an untouched slot stays derivation-empty, edits elsewhere notwithstanding.
	CHECK(f.stack.isEmpty(2));
}

TEST_CASE("a switch carries the whole box, both directions — one object, every member") {
	// THE REVIEW'S MUTATION FINDING, closed: writeRideAlong survived dropping its RESIDUE write
	// (the A26 violation V20's own text names), its PLANE write, its DRIFT write, and OFFSET plus
	// GAMMA-BASE together — four mutations, all green — because the verb-level cases exercised only
	// zone and width while the wire fixtures pin storage, not the road that plays. This case closes
	// all four at once: EVERY member non-default in the stored record, a wander that moves EVERY
	// member, and the recall and the bank each asserted as ONE RideAlongValues object — so a member
	// added to the family, or a write dropped from the road, fails here by construction. That is
	// the standing membership rule given teeth where the sound is made.
	StackFixture f;
	f.set(ParamId::AZone, 512.0);
	f.set(ParamId::AOffset, 0.37);
	f.set(soundParam(0, SoundField::Width), 0.55);
	f.bridge.beginSoundGammaGesture();
	f.bridge.setSoundGamma(0, 61.25);
	f.bridge.endSoundGammaGesture();
	f.bridge.beginSoundTravelGesture();
	f.bridge.setSoundTravel(0, { 0.021, -0.013 });
	f.bridge.endSoundTravelGesture();
	SliceOrientationPin pinned;
	pinned.pinned = true;
	pinned.generator.rotate = 0.44f;
	f.bridge.beginSoundOrientationGesture();
	f.bridge.setSoundOrientation(0, pinned);
	f.bridge.endSoundOrientationGesture();
	PlaneResidue banked;
	banked.morph.offset[0] = 0.017;
	banked.stair.offset[2] = -0.004;
	f.bridge.setSoundResidue(0, banked);
	f.settle();
	const RideAlongValues explored = f.stack.liveRideAlong(0);
	// The setup really is non-default on every member, or the one-object compares below have
	// nothing to bite on.
	REQUIRE_FALSE(explored == RideAlongValues {});
	REQUIRE(explored.residue.morph.offset[0] == doctest::Approx(0.017));
	REQUIRE(explored.plane.pinned);

	f.stack.storeVariationAt(0); // banks the exploration; the copy is active
	f.settle();

	// Wander on EVERY axis the box carries.
	f.set(ParamId::AZone, 100.0);
	f.set(ParamId::AOffset, 0.9);
	f.set(soundParam(0, SoundField::Width), 0.1);
	f.bridge.beginSoundGammaGesture();
	f.bridge.setSoundGamma(0, -5.0);
	f.bridge.endSoundGammaGesture();
	f.bridge.beginSoundTravelGesture();
	f.bridge.setSoundTravel(0, { -0.4, 0.2 });
	f.bridge.endSoundTravelGesture();
	SliceOrientationPin turned;
	turned.pinned = true;
	turned.generator.rotate = -0.9f;
	f.bridge.beginSoundOrientationGesture();
	f.bridge.setSoundOrientation(0, turned);
	f.bridge.endSoundOrientationGesture();
	PlaneResidue rebanked;
	rebanked.morph.offset[5] = 0.11;
	f.bridge.setSoundResidue(0, rebanked);
	f.settle();
	const RideAlongValues wandered = f.stack.liveRideAlong(0);
	REQUIRE_FALSE(wandered == explored);

	// SWITCH to the stored record: the LIVE state is the stored exploration, one object — every
	// write road in writeRideAlong exercised and asserted at once.
	f.stack.activateVariation(0, 0);
	f.settle();
	CHECK(f.stack.liveRideAlong(0) == explored);
	// And the outgoing record BANKED the wander whole — the other direction of the same claim.
	CHECK(f.stack.variationValuesAt(0, 1) == wandered);

	// Switch back: the wander returns as one object.
	f.stack.activateVariation(0, 1);
	f.settle();
	CHECK(f.stack.liveRideAlong(0) == wandered);
	CHECK(f.stack.variationValuesAt(0, 0) == explored);
}

TEST_CASE("stances are independent records — editing the active one touches no sibling") {
	// V18's addendum ("as do variation clones") REGOLDED for V19: independence no longer means
	// "storage never follows the live values" — the ACTIVE record is exactly the one that does —
	// it means editing THROUGH the active stance mutates that record alone: every banked sibling
	// stands, within a sound and through a sound clone carrying its list.
	StackFixture f;
	f.set(static_cast<ParamId>(soundParam(0, SoundField::Width)), 0.6);
	f.stack.storeVariationAt(0); // banks the original at 0.6; the copy is active
	f.set(static_cast<ParamId>(soundParam(0, SoundField::Width)), 0.3); // mutates the copy only
	f.settle();
	CHECK(f.stack.variationValuesAt(0, 0).width == doctest::Approx(0.6));
	f.stack.storeVariationAt(0); // banks the copy at 0.3; a third record is active
	f.set(static_cast<ParamId>(soundParam(0, SoundField::Width)), 0.95); // mutates the third only
	f.settle();
	CHECK(f.stack.variationValuesAt(0, 0).width == doctest::Approx(0.6));
	CHECK(f.stack.variationValuesAt(0, 1).width == doctest::Approx(0.3));
	const auto width0 = [&] {
		return f.read(static_cast<ParamId>(soundParam(0, SoundField::Width)));
	};

	// Each switch restores its record's own number and banks the outgoing one.
	f.stack.activateVariation(0, 1);
	f.settle();
	CHECK(width0() == doctest::Approx(0.3));
	CHECK(f.stack.variationValuesAt(0, 2).width == doctest::Approx(0.95));

	// THROUGH A SOUND CLONE: the carried list switches with the clone's own stored values, and the
	// switch touches the clone's parameters only — the source's live width stands.
	f.stack.clone(0);
	f.settle();
	const double sourceWidth = width0();
	f.stack.activateVariation(1, 2);
	f.settle();
	CHECK(f.read(static_cast<ParamId>(soundParam(1, SoundField::Width))) == doctest::Approx(0.95));
	CHECK(width0() == doctest::Approx(sourceWidth));
	// And forgetting a NON-ACTIVE stance on the source moves neither the source's sound nor the
	// clone's list — independence at the stance granularity, through the sound granularity.
	const SoundVariations::List cloneList = f.bridge.soundVariations().sound[1];
	f.stack.forgetVariationAt(0, 2);
	f.settle();
	CHECK(width0() == doctest::Approx(sourceWidth));
	CHECK(f.bridge.soundVariations().sound[1] == cloneList);
}

TEST_CASE("the clone button is where the mock puts it, and says when it cannot work") {
	// The press has to reach it — the geometry half, which is what "a control nothing tests the
	// routing of" cost this project a release once.
	StackFixture f;
	const visage::Point on = f.clonePoint(1);
	const SoundStack::Hit hit = f.stack.hitAt(on);
	CHECK(hit.part == SoundStack::Part::Clone);
	CHECK(hit.slot == 1);
	// And it must not have eaten the switch beside it or the card under it.
	CHECK(f.stack.hitAt(f.switchPoint(1)).part == SoundStack::Part::Enable);
	CHECK(f.stack.hitAt(f.cardPoint(1)).part == SoundStack::Part::Card);

	// With every sound switched on there is nowhere for a copy to go. The verb says so rather than
	// doing nothing quietly, and it changes no sound while saying it.
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		f.set(static_cast<ParamId>(soundParam(slot, SoundField::Enable)), 1.0);
	}
	CHECK_FALSE(f.stack.cloneTarget().has_value());
	const SoundIdentity before = readIdentity(f.bridge, 3);
	f.stack.onMouseDown().callback(at(on.x, on.y));
	CHECK(drain(f.bridge).empty());
	CHECK(readIdentity(f.bridge, 3) == before);
	CHECK(f.tray.text() == "Max 8 sounds — nowhere to clone to");
}

TEST_CASE("the switch toggles Enable as one bracketed edit, and the card does not") {
	StackFixture f;
	const clap_id enableC = static_cast<clap_id>(soundParam(2, SoundField::Enable));
	// C holds a sound and is switched off — a parked sound, which is the only way a switched-off card
	// is on screen now that empty slots are not drawn. The claim is unchanged: the switch posts one
	// bracketed edit and the card posts none.
	f.consume(2, 640.0);
	f.bridge.drain([](const UiParamEvent&) {});
	REQUIRE(steppedIndex(f.read(static_cast<ParamId>(enableC)), kEnableNames.size()) == 0);

	const visage::Point on = f.switchPoint(2);
	f.stack.onMouseDown().callback(at(on.x, on.y));
	const std::vector<UiParamEvent> events = drain(f.bridge);
	REQUIRE(events.size() == 3);
	CHECK(events[0].kind == UiParamEvent::Kind::GestureBegin);
	CHECK(events[0].id == enableC);
	CHECK(events[1].kind == UiParamEvent::Kind::Value);
	CHECK(events[1].value == 1.0);
	CHECK(events[2].kind == UiParamEvent::Kind::GestureEnd);
	CHECK(events[2].id == enableC);
	// Switching a sound on is also how you start working on it.
	CHECK(f.selection.slot() == 2);
}

TEST_CASE("the switch wins the press it lands on") {
	StackFixture f;
	f.consume(3, 480.0); // a card has to be on screen to be pressed
	// The pill is 18x10 in a card that answers every other pixel, so the grab area is padded. What
	// this pins is that the padding did not swallow the card: a press at the card's own text still
	// selects rather than switching.
	CHECK(f.stack.hitAt(f.switchPoint(3)).part == SoundStack::Part::Enable);
	CHECK(f.stack.hitAt(f.switchPoint(3)).slot == 3);
	CHECK(f.stack.hitAt(f.cardPoint(3)).part == SoundStack::Part::Card);
	CHECK(f.stack.hitAt(f.cardPoint(3)).slot == 3);
}

TEST_CASE("the switch reads the parameter rather than a state of its own") {
	StackFixture f;
	const ParamId enableD = soundParam(3, SoundField::Enable);
	f.set(enableD, 1.0);
	// Turned on by automation, so the switch must now turn it OFF — a widget holding its own idea of
	// the state would post 1.0 again and the sound would never leave the constellation.
	const visage::Point pill = f.switchPoint(3);
	f.stack.onMouseDown().callback(at(pill.x, pill.y));
	const std::vector<UiParamEvent> events = drain(f.bridge);
	REQUIRE(events.size() == 3);
	CHECK(events[1].value == 0.0);
}

TEST_CASE("the rail scrolls rather than growing, and stops at both ends") {
	StackFixture f;
	// Eight cards do not have to overflow at every size, so the contract is stated as one: whatever
	// the rail's height, the scroll never leaves content unreachable and never scrolls past the end.
	CHECK(f.stack.maxScroll() == doctest::Approx(std::max(
	          0.0f, f.stack.contentHeight() - StackFixture::kRailHeight)));

	f.stack.setScroll(-50.0f);
	CHECK(f.stack.scroll() == doctest::Approx(0.0f));
	f.stack.setScroll(10000.0f);
	CHECK(f.stack.scroll() == doctest::Approx(f.stack.maxScroll()));

}

TEST_CASE("a short rail scrolls, and a hit test at the bottom is still a real card") {
	StackFixture f;
	// EIGHT SOUNDS, because the rail draws consumed slots only and a list that cannot fit is what this
	// case is about. That used to be the default state — every slot drawn whether or not it held
	// anything — and it is now a patch somebody filled, which is the state a hand reaches this
	// complaint in anyway.
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		f.consume(slot, 100.0 + 50.0 * static_cast<double>(slot));
	}
	// Deliberately shorter than the content: the defect was a window growing taller instead of
	// scrolling, so the case worth pinning is the one where the list cannot fit.
	f.stack.setBounds(0.0f, 0.0f, SoundStack::kWidth, 200.0f);
	REQUIRE(f.stack.maxScroll() > 0.0f);

	// A hit test follows the scroll, which is the half a fixed layout gets wrong: the card under a
	// coordinate changes when the list moves under it. cardPoint() subtracts the scroll itself, so it
	// is asked for the UNSCROLLED point on purpose here — that coordinate must now name a later card.
	const visage::Point top { SoundStack::kPadX + 30.0f,
		                      SoundStack::kPadTop + SoundStack::kHeadingHeight + SoundStack::kRowGap
		                          + SoundStack::kCardHeight * 0.5f };
	REQUIRE(f.stack.hitAt(top).slot == 0);
	f.stack.setScroll(SoundStack::kSlotHeight + SoundStack::kRowGap);
	CHECK(f.stack.hitAt(top).slot == 1);

	f.stack.setScroll(f.stack.maxScroll());
	// The last card is reachable once scrolled to the end — which is what "nothing is unreachable"
	// means in practice, and what a list that grew its window instead would never have had to prove.
	bool foundLast = false;
	for (float y = 0.0f; y < 200.0f; y += 2.0f) {
		if (f.stack.hitAt({ SoundStack::kPadX + 30.0f, y }).slot == kMaxSounds - 1) {
			foundLast = true;
			break;
		}
	}
	CHECK(foundLast);
}

TEST_CASE("a card scrolled off the top keeps drawing until the last of its stance rows is gone") {
	// THE CULL MEASURED THE CARD AND ONE ROW, which is the right number only for a sound that remembers
	// nothing. A sound with four stances is 97px tall and its card is 38 of them, so a scroll that took
	// the card off the top dropped a slot with a third of it still on screen — rows vanishing a card at
	// a time while the wheel was turning, which is a list that looks broken exactly while somebody is
	// watching it move.
	StackFixture f;
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		f.consume(slot, 100.0 + 50.0 * static_cast<double>(slot));
	}
	SoundVariations variations = f.bridge.soundVariations();
	for (int i = 0; i < 4; ++i) {
		storeVariation(variations, 0, { 0.1 * i });
	}
	f.bridge.beginSoundVariationsGesture();
	f.bridge.setSoundVariations(0, variations.sound[0]);
	f.bridge.endSoundVariationsGesture();
	f.settle();
	// Asked of the widget rather than counted here: storing into an empty list seeds the live
	// placement as well, so how many rows four presses make is the rail's arithmetic, not this test's.
	const size_t rows = f.stack.variationRows(0);
	REQUIRE(rows >= 4);

	// Scrolled far enough that A's card is off the top and its last stance row is not.
	f.stack.setBounds(0.0f, 0.0f, SoundStack::kWidth, 200.0f);
	f.stack.setScroll(SoundStack::kPadTop + SoundStack::kHeadingHeight + SoundStack::kRowGap
	                  + SoundStack::kCardHeight + SoundStack::kRowGap + 2.0f * SoundStack::kPlaceHeight);
	const visage::Bounds card = f.stack.cardBoundsForTest(0);
	const visage::Bounds lastRow = f.stack.variationBoundsForTest(0, rows - 1);
	REQUIRE(card.bottom() < 0.0f);
	REQUIRE(lastRow.bottom() > 0.0f);

	CHECK(f.stack.slotOnScreenForTest(0));
}

TEST_CASE("a click outside every card hits nothing") {
	StackFixture f;
	CHECK(f.stack.hitAt({ -5.0f, 40.0f }).part == SoundStack::Part::None);
	CHECK(f.stack.hitAt({ SoundStack::kWidth * 0.5f, 4.0f }).part == SoundStack::Part::None);
}

//-------------------------//
// The order of the list   //
//-------------------------//

TEST_CASE("a clone lands beside the sound it came from, not at the end of the list") {
	// THE DECISION. A clone used to go to the first empty slot, which on a list of three sounds put
	// the copy fourth — as far from its original as the rail can manage. Slot order IS list order IS
	// the Path's tour order, so that was not a cosmetic distance: it put the copy somewhere else in
	// the sweep entirely.
	StackFixture f;
	f.consume(0, 300.0);
	f.consume(1, 500.0);
	f.consume(2, 700.0);
	REQUIRE(f.stack.visibleCount() == 3);
	REQUIRE(f.stack.cloneDestination(0).has_value());
	CHECK(*f.stack.cloneDestination(0) == 1);

	f.press(f.clonePoint(0));
	f.settle();

	// THE COPY IS SECOND, and everything that was below it has moved down one. B and C keep their
	// sounds and change their letters, which is what a letter is: the name of a slot.
	CHECK(f.stack.visibleCount() == 4);
	CHECK(f.zoneOf(0) == doctest::Approx(300.0));
	CHECK(f.zoneOf(1) == doctest::Approx(300.0));
	CHECK(f.zoneOf(2) == doctest::Approx(500.0));
	CHECK(f.zoneOf(3) == doctest::Approx(700.0));
	CHECK(f.read(ParamId::BEnable) == 1.0);
	CHECK(f.selection.slot() == 1);
	CHECK(f.tray.text() == "Sound A cloned to B");
}

TEST_CASE("a clone with room only above its source still lands beside it") {
	// THE OTHER DIRECTION, and it is not symmetric: with no empty slot below, the SOURCE moves up one
	// and the copy takes the slot it vacated. The copy still sits immediately after its original —
	// which is the invariant — but the original's own letter changes, so the tray has to say the new
	// one rather than the one the button was pressed on.
	StackFixture f;
	// B..H hold sounds; A is the only empty slot, and it is above every one of them.
	for (size_t slot = 1; slot < kMaxSounds; ++slot) {
		f.consume(slot, 100.0 * static_cast<double>(slot));
	}
	// A has to be genuinely empty, and a fresh patch has it switched on — so free it the way the rail
	// does, with the verb that exists for exactly this.
	f.stack.deleteSound(0);
	f.settle();
	REQUIRE(f.stack.isEmpty(0));
	REQUIRE(f.stack.visibleCount() == 7);
	REQUIRE(f.stack.cloneDestination(4).has_value());
	CHECK(*f.stack.cloneDestination(4) == 4);

	f.press(f.clonePoint(4));
	f.settle();

	// Slots 1..4 each took the sound above them, so E's sound is now in D — and the copy is in E,
	// immediately after it. F, G and H never moved.
	CHECK(f.stack.visibleCount() == 8);
	CHECK(f.zoneOf(0) == doctest::Approx(100.0));
	CHECK(f.zoneOf(1) == doctest::Approx(200.0));
	CHECK(f.zoneOf(2) == doctest::Approx(300.0));
	CHECK(f.zoneOf(3) == doctest::Approx(400.0));
	CHECK(f.zoneOf(4) == doctest::Approx(400.0));
	CHECK(f.zoneOf(5) == doctest::Approx(500.0));
	CHECK(f.zoneOf(7) == doctest::Approx(700.0));
	CHECK(f.tray.text() == "Sound D cloned to E");
}

TEST_CASE("a parked sound is carried along by a splice, whole, rather than written over") {
	// THE DISTINCTION THE WHOLE RAIL TURNS ON. A parked sound — authored, then switched off — is not
	// empty, so a clone may not land on it; but it is in the list, so a splice past it must MOVE it,
	// and move all of it. A shift that carried the identity and left the placement would teleport a
	// sound nobody touched.
	StackFixture f;
	f.consume(0, 300.0);
	f.consume(1, 500.0);
	f.consume(2, 815.0);
	f.set(ParamId::CEnable, 0.0);
	f.set(ParamId::CPlaceX, 0.42);
	REQUIRE_FALSE(f.stack.isEmpty(2));
	const SlotState parked = readSlotState(f.bridge, 2);
	REQUIRE_FALSE(parked.enabled);

	f.press(f.clonePoint(0));
	f.settle();

	// The parked sound is one slot further down and otherwise untouched — still off, still standing
	// where it stood, still remembering what it remembered.
	CHECK(readSlotState(f.bridge, 3) == parked);
	CHECK(f.zoneOf(1) == doctest::Approx(300.0));
	CHECK(f.zoneOf(2) == doctest::Approx(500.0));
}

TEST_CASE("a full rail refuses a clone and names the limit rather than moving anything") {
	StackFixture f;
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		f.consume(slot, 100.0 + static_cast<double>(slot));
	}
	REQUIRE_FALSE(f.stack.cloneTarget().has_value());
	CHECK_FALSE(f.stack.cloneDestination(0).has_value());
	const std::array<SlotState, kMaxSounds> before = f.everySlot();

	f.press(f.clonePoint(0));
	f.settle();
	CHECK(f.tray.text() == "Max 8 sounds — nowhere to clone to");
	CHECK(f.everySlot() == before);
	CHECK(f.history.length() == 0);
}

TEST_CASE("one undo puts back every slot a clone-splice touched") {
	// ONE STEP FOR THE WHOLE ACT. A splice rewrites a run of slots, and a history that walked it back
	// one slot at a time would walk through orders the list was never in.
	StackFixture f;
	f.consume(0, 300.0);
	f.consume(1, 500.0);
	f.consume(2, 700.0);
	f.consume(3, 900.0);
	const std::array<SlotState, kMaxSounds> before = f.everySlot();

	f.press(f.clonePoint(0));
	f.settle();
	const std::array<SlotState, kMaxSounds> after = f.everySlot();
	REQUIRE_FALSE(after == before);
	CHECK(f.history.length() == 1);

	f.history.undo();
	f.settle();
	CHECK(f.everySlot() == before);
	f.history.redo();
	f.settle();
	CHECK(f.everySlot() == after);
}

TEST_CASE("dragging a card onto another row moves it there, and the rest close up behind it") {
	// MOCK PARITY ITEM 15, and the one affordance whose absence changed what the instrument could be
	// played into: `tourPoint` walks the live anchors in slot order, so this list's order is the order
	// the Path knob sweeps.
	StackFixture f;
	f.consume(0, 300.0);
	f.consume(1, 500.0);
	f.consume(2, 700.0);
	f.consume(3, 900.0);

	f.dragCard(0, 2);

	// MOVE-TO-INDEX, NOT SWAP — the mock's own `reorderSound()` splices. A swap would have left C
	// first; what happened is that A went third and B and C each moved up one.
	CHECK(f.zoneOf(0) == doctest::Approx(500.0));
	CHECK(f.zoneOf(1) == doctest::Approx(700.0));
	CHECK(f.zoneOf(2) == doctest::Approx(300.0));
	CHECK(f.zoneOf(3) == doctest::Approx(900.0));
	CHECK(f.stack.visibleCount() == 4);
	// The selection follows the SOUND, so the card that is lit is still the one that was picked up.
	CHECK(f.selection.slot() == 2);
	CHECK(f.tray.text() == "A moved to 3rd");
}

TEST_CASE("a drag upwards splices the other way") {
	StackFixture f;
	f.consume(0, 300.0);
	f.consume(1, 500.0);
	f.consume(2, 700.0);
	f.consume(3, 900.0);

	f.dragCard(3, 1);

	CHECK(f.zoneOf(0) == doctest::Approx(300.0));
	CHECK(f.zoneOf(1) == doctest::Approx(900.0));
	CHECK(f.zoneOf(2) == doctest::Approx(500.0));
	CHECK(f.zoneOf(3) == doctest::Approx(700.0));
	CHECK(f.tray.text() == "D moved to 2nd");
}

TEST_CASE("a press that does not travel is a click, and every verb on the card still answers one") {
	// THE HALF A DRAG GESTURE CAN COST YOU. The reorder arms from the card body, so a hand that presses
	// and lets go must still select and nothing else — and the three verbs on the line, which the hit
	// test answers before the card, must be untouched by any of it.
	StackFixture f;
	f.consume(0, 300.0);
	f.consume(1, 500.0);
	const std::array<SlotState, kMaxSounds> before = f.everySlot();

	// A press, a twitch under the threshold, a release.
	const visage::Point card = f.cardPoint(0);
	f.press(card);
	f.dragTo({ card.x, card.y + SoundStack::kDragThreshold * 0.5f });
	f.release({ card.x, card.y + SoundStack::kDragThreshold * 0.5f });
	f.settle();
	CHECK(f.everySlot() == before);
	CHECK(f.selection.slot() == 0);
	CHECK_FALSE(f.stack.dragSlot().has_value());

	// And the verbs. Each is pressed and released where a hand would leave it; none of them may come
	// back as a reorder.
	// THE STANCE ROWS, which this case did not cover and should have — it was reported "it seems I cant
	// select a variant?" against a build where the cards had just learned to be dragged, and nothing
	// here could have answered the question. A press on a row is a click like any other: it recalls, and it
	// does not arm a reorder.
	f.press(f.variationStorePoint(0, 0));
	f.release(f.variationStorePoint(0, 0));
	f.settle();
	REQUIRE(f.stack.variationRows(0) == 2);
	// The stance as the ROW states it, not a literal — the case's first draft guessed a number and
	// said so. Since V20 the record is the explore box, so the recalled axis is ZONE and the place
	// must NOT move ("place leaves the stance").
	const double stanceZone = f.stack.variationValuesAt(0, 0).zone;
	f.set(ParamId::AZone, 999.0);
	REQUIRE_FALSE(f.stack.activeStance(0, 0)); // V19 port: the light names the active record
	const double placeBefore = f.read(ParamId::APlaceX);

	f.press(f.variationPoint(0, 0));
	f.release(f.variationPoint(0, 0));
	f.settle();
	CHECK(f.zoneOf(0) == doctest::Approx(stanceZone));
	CHECK(f.stack.activeStance(0, 0));
	CHECK_FALSE(f.stack.dragSlot().has_value());
	CHECK(f.read(ParamId::APlaceX) == doctest::Approx(placeBefore)); // the star did not move
	// Nothing moved in the list: a row press is not a card press, so it may not arm the gesture the
	// card body arms — slot 1's sound is still where the setup put it.
	CHECK(f.zoneOf(1) == doctest::Approx(500.0));

	// And the row's own ✕ still forgets rather than recalling or reordering — forgetting the
	// banked sibling moves nothing, so the recalled zone stands.
	f.press(f.variationForgetPoint(0, 1));
	f.release(f.variationForgetPoint(0, 1));
	f.settle();
	CHECK(f.stack.variationRows(0) == 1);
	CHECK(f.zoneOf(0) == doctest::Approx(stanceZone));

	f.press(f.switchPoint(1));
	f.release(f.switchPoint(1));
	f.settle();
	CHECK(f.read(ParamId::BEnable) == 0.0);

	f.press(f.previewPoint(0));
	f.release(f.previewPoint(0));
	CHECK(f.stack.previewing(0));

	f.press(f.clonePoint(0));
	f.release(f.clonePoint(0));
	f.settle();
	CHECK(f.stack.visibleCount() == 3);
	CHECK(f.zoneOf(1) == doctest::Approx(300.0));
}

TEST_CASE("the GAP a drag would drop into is the one the rail is holding, and leaving cancels it") {
	StackFixture f;
	f.consume(0, 300.0);
	f.consume(1, 500.0);
	f.consume(2, 700.0);
	const std::array<SlotState, kMaxSounds> before = f.everySlot();

	f.press(f.cardPoint(0));
	CHECK_FALSE(f.stack.dragSlot().has_value()); // a press is not yet a drag
	f.dragTo(f.cardPoint(2));
	REQUIRE(f.stack.dragSlot().has_value());
	CHECK(*f.stack.dragSlot() == 0);
	// A GAP, NOT A ROW — this asserted slot 2 while the rail highlighted the CARD a drop would land
	// on, which is the thing it was reported.
	//
	// AND THE NUMBER IS 2 RATHER THAN 3, WHICH I GOT WRONG FIRST AND MEASURED SECOND. `cardPoint` is
	// the centre of the CARD, while a row is the whole SLOT — taller, because a slot carries its
	// stance rows — so the card's centre sits ABOVE the row's midpoint and means "before this row".
	// Gap 2 is between rows 1 and 2. The card-centre point that used to mean "onto row 2" now means
	// "above row 2", and that shift is exactly the fix rather than a side effect of it.
	REQUIRE(f.stack.dropGap().has_value());
	CHECK(*f.stack.dropGap() == 2);

	// OUT OF THE RAIL IS THE CANCEL — Escape would need keyboard focus, and nothing in this editor has
	// ever asked a host for it. A release with no target moves nothing.
	f.dragTo({ -20.0f, 40.0f });
	CHECK_FALSE(f.stack.dropGap().has_value());
	f.release({ -20.0f, 40.0f });
	f.settle();
	CHECK(f.everySlot() == before);
	CHECK(f.history.length() == 0);
}

TEST_CASE("one undo puts back every slot a reorder moved") {
	StackFixture f;
	for (size_t slot = 0; slot < 5; ++slot) {
		f.consume(slot, 100.0 * static_cast<double>(slot + 1));
	}
	const std::array<SlotState, kMaxSounds> before = f.everySlot();

	f.dragCard(4, 0);
	const std::array<SlotState, kMaxSounds> after = f.everySlot();
	REQUIRE_FALSE(after == before);
	CHECK(f.history.length() == 1);

	f.history.undo();
	f.settle();
	CHECK(f.everySlot() == before);
	f.history.redo();
	f.settle();
	CHECK(f.everySlot() == after);
}

TEST_CASE("a reorder past an empty slot leaves the visible order a plain splice") {
	// EMPTY SLOTS SHIFT TOO, and that is what makes this simple: an empty slot moved is still empty and
	// still hidden, so the splice does not have to know where the gaps are.
	StackFixture f;
	f.consume(0, 300.0);
	f.consume(1, 500.0);
	f.consume(3, 900.0);
	REQUIRE(f.stack.visibleCount() == 3);
	REQUIRE(f.stack.isEmpty(2));

	// The third sound in the list to the front of it.
	f.dragCard(3, 0);

	CHECK(f.stack.visibleCount() == 3);
	CHECK(f.stack.visibleSlot(0) == 0);
	CHECK(f.zoneOf(0) == doctest::Approx(900.0));
	CHECK(f.zoneOf(1) == doctest::Approx(300.0));
	CHECK(f.zoneOf(f.stack.visibleSlot(2)) == doctest::Approx(500.0));
}

TEST_CASE("a drag reorders by the card actually under the pointer, scrolled or not") {
	// THE ONE INTERACTION THE NEW GESTURE INHERITED RATHER THAN WROTE. Every card position in this rail
	// is a running sum MINUS the scroll, so a drop-row computation measured from the top of the
	// CONTENT rather than the top of the FRAME would agree with the cards at rest and disagree with
	// them by exactly the scroll offset the moment the list moved — a drag that lands two rows from
	// where the hand let go, and only on a list long enough to scroll.
	StackFixture f;
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		f.consume(slot, 100.0 + 50.0 * static_cast<double>(slot));
	}
	// A rail too short for its list, scrolled off the top: the state the offset code exists for.
	f.stack.setBounds(0.0f, 0.0f, SoundStack::kWidth, 200.0f);
	f.stack.setScroll(2.0f * (SoundStack::kSlotHeight + SoundStack::kRowGap));
	REQUIRE(f.stack.scroll() > 0.0f);

	// Asked of the widget, never restated here — a case that recomputes a widget's geometry is a case
	// that can disagree with it, which is how a test comes to press empty space and report a bug.
	// Scrolled by two slots, a 200px rail shows the third, fourth and fifth cards — so those are the
	// ones a hand can reach, and the case drags between two of them rather than between two indices it
	// assumed were on screen. Its first draft aimed at a card the scroll had pushed off the bottom and
	// said so, which is the setup check earning its place.
	const visage::Point from = f.cardPoint(2);
	const visage::Point to = f.cardPoint(4);
	REQUIRE(f.stack.hitAt(from).slot == 2);
	REQUIRE(f.stack.hitAt(to).slot == 4);

	f.press(from);
	f.dragTo(to);
	// THE CLAIM: the gap the rail says it would drop into is the gap the pointer is actually over,
	// SCROLL INCLUDED — which is what this case is really about.
	//
	// GAP 4, NOT 5, AND THE REASON IS MEASURED RATHER THAN ASSUMED. `cardPoint` is the centre of the
	// CARD (38px) while a row is the whole SLOT (55px here — slot 4 carries a stance row), so the
	// card's centre sits ABOVE the row's midpoint and means "before this row". Under the old
	// slot-shaped answer the same point meant "onto row 4"; that difference IS the fix it was asked
	// for, and this is where it is visible.
	REQUIRE(f.stack.dropGap().has_value());
	CHECK(*f.stack.dropGap() == 4);
	f.release(to);
	f.settle();

	// AND THE ORDER THAT GAP PRODUCES: the dragged sound lands ABOVE what was row 4, so it ends at
	// position 3 and row 4 never moves.
	CHECK(f.zoneOf(2) == doctest::Approx(250.0)); // what was in 3, shifted up
	CHECK(f.zoneOf(3) == doctest::Approx(200.0)); // the sound that was dragged
	CHECK(f.zoneOf(4) == doctest::Approx(300.0)); // untouched — the drop was ABOVE it
	CHECK(f.zoneOf(1) == doctest::Approx(150.0)); // above the move, untouched
	CHECK(f.zoneOf(5) == doctest::Approx(350.0)); // below it, untouched
	CHECK(f.selection.slot() == 3);
}

TEST_CASE("a reordered sound keeps the colour it was wearing") {
	// Reordering keeps each sound's original colour. The colour was a
	// function of the SLOT, so a splice repainted whatever it moved — and the mock has never done that.
	// Its `hueOf(i)` reads a hue stored ON THE SOUND (mock :507) and `reorderSound` splices the array,
	// so colour travels; the palette's own comment says why: a sound's colour has to name the
	// sound. A name that changes when a row moves is not a name.
	StackFixture f;
	f.consume(0, 300.0);
	f.consume(1, 500.0);
	f.consume(2, 700.0);
	f.consume(3, 900.0);
	const uint32_t wasFirst = f.stack.seedHueForTest(0);
	const uint32_t wasThird = f.stack.seedHueForTest(2);
	REQUIRE(wasFirst != wasThird);

	f.dragCard(0, 2);

	// The sound that was first is now third, and it is wearing what it wore.
	REQUIRE(f.zoneOf(2) == doctest::Approx(300.0));
	CHECK(f.stack.seedHueForTest(2) == wasFirst);
	// And the one it displaced kept its own rather than inheriting the row's.
	REQUIRE(f.zoneOf(1) == doctest::Approx(700.0));
	CHECK(f.stack.seedHueForTest(1) == wasThird);

	// Undo puts the colours back with the order.
	f.history.undo();
	f.settle();
	CHECK(f.stack.seedHueForTest(0) == wasFirst);
	CHECK(f.stack.seedHueForTest(2) == wasThird);
}

TEST_CASE("the scope's ramp is painted in the colours the first two sounds are wearing") {
	// THE LAST SURFACE STILL PAINTING BY SLOT. The scope's gradient was written `kSeed1 -> kSeed2` —
	// literally "sound A to sound B" — which said something true only while a colour WAS the slot.
	// With hues riding the sound, row 0 is not necessarily wearing the first colour any more, and a
	// ramp that still starts there disagrees with the rail directly above it: two sounds, named two
	// different ways, on one screen. The same argument as the rail's own case a few lines up — "a
	// sound's colour has to name the sound" — applied to the surface that was left out of it.
	StackFixture f;
	WaveDisplay wave { f.bridge, f.registry };
	wave.setBounds(0.0f, 0.0f, WaveDisplay::kWidth, WaveDisplay::kHeight);
	f.consume(0, 300.0);
	f.consume(1, 500.0);
	f.consume(2, 700.0);

	// THE RAIL IS ASKED RATHER THAN A PALETTE ENTRY WRITTEN OUT, at both ends and in both states. What
	// is being checked is that two surfaces agree about one sound, so the only way to check it is to
	// put the same question to both — a literal colour here would pass a scope that had quietly grown
	// a palette of its own.
	REQUIRE(wave.seedHueForTest(0) == f.stack.seedHueForTest(0));
	REQUIRE(wave.seedHueForTest(1) == f.stack.seedHueForTest(1));

	const uint32_t wasFirst = f.stack.seedHueForTest(0);
	f.dragCard(0, 2);

	// The splice put a different sound in each of the top two rows, and each brought its colour with
	// it — so this is the state where a ramp keyed to the slot and one keyed to the sound come apart.
	REQUIRE(f.zoneOf(0) == doctest::Approx(500.0));
	REQUIRE(f.stack.seedHueForTest(0) != wasFirst);

	CHECK(wave.seedHueForTest(0) == f.stack.seedHueForTest(0));
	CHECK(wave.seedHueForTest(1) == f.stack.seedHueForTest(1));
}

TEST_CASE("a clone repaints nothing it moved out of its way") {
	// THE WORSE HALF, found while investigating the reorder colour and not yet seen in use: a
	// clone splices, so every sound between the source and the spent slot shifts one place — and with
	// colour keyed to the slot, ONE clone repainted the whole tail of the list. Four sounds changing
	// colour because you copied the first is the same defect as the reorder's, with a bigger blast
	// radius.
	StackFixture f;
	f.consume(0, 300.0);
	f.consume(1, 500.0);
	f.consume(2, 700.0);
	f.consume(3, 900.0);
	std::array<uint32_t, 4> before {};
	for (size_t slot = 0; slot < 4; ++slot) {
		before[slot] = f.stack.seedHueForTest(slot);
	}

	f.press(f.clonePoint(0));
	f.settle();

	// The three sounds the splice pushed down are wearing what they wore, at their new rows.
	CHECK(f.stack.seedHueForTest(0) == before[0]);
	CHECK(f.stack.seedHueForTest(2) == before[1]);
	CHECK(f.stack.seedHueForTest(3) == before[2]);
	CHECK(f.stack.seedHueForTest(4) == before[3]);
	// AND THE COPY WEARS ITS OWN, not its source's — the mock's answer (forkSound pushes a fresh hue),
	// and the same separability argument that already keeps a clone off its source's anchor: two
	// identical dots would assert a link between two sounds that are independent from the moment the
	// copy exists.
	const uint32_t copy = f.stack.seedHueForTest(1);
	CHECK(copy != before[0]);
	for (size_t slot = 0; slot < 4; ++slot) {
		CAPTURE(slot);
		CHECK(copy != before[slot]);
	}
}

TEST_CASE("a new sound takes the lowest colour nobody is wearing") {
	// THE ASSIGNMENT RULE, and a correction of the mock rather than a copy of it: its `forkSound`
	// pushes `hue: S.sounds.length`, which collides the moment a delete has left a gap (delete the
	// second of three, add one, and the newcomer wears a colour already on screen) and survives only
	// because the read modulos. A list of eight with a delete verb needs the lowest palette index
	// nobody is wearing, which is well defined with gaps and cannot collide.
	//
	// ASKED IN A LIST THAT HAS BEEN REORDERED, which is the only state where the rule says anything.
	// While a colour is the slot, the colour a freed slot gives back IS that slot's, and the case
	// passes on arithmetic rather than on the rule. Permute first and the two come apart.
	StackFixture f;
	f.consume(0, 300.0);
	f.consume(1, 500.0);
	f.consume(2, 700.0);
	const uint32_t third = f.stack.seedHueForTest(2);

	f.dragCard(0, 2);
	// The middle row now holds what was the third sound, wearing the third colour.
	REQUIRE(f.zoneOf(1) == doctest::Approx(700.0));
	REQUIRE(f.stack.seedHueForTest(1) == third);

	// Free it. That colour is nobody's now, and it is the lowest nobody is wearing.
	f.stack.deleteSound(1);
	f.settle();
	REQUIRE(f.stack.isEmpty(1));

	// So the next sound made takes it back — not the colour its ROW would once have given it.
	f.press(f.newSoundPoint());
	f.settle();
	REQUIRE_FALSE(f.stack.isEmpty(1));
	CHECK(f.stack.seedHueForTest(1) == third);

	// And every sound on screen wears a different colour, which is the property all of this is for —
	// asserted over the whole list rather than at the one slot that just changed.
	std::set<uint32_t> worn;
	for (size_t row = 0; row < f.stack.visibleCount(); ++row) {
		worn.insert(f.stack.seedHueForTest(f.stack.visibleSlot(row)));
	}
	CHECK(worn.size() == f.stack.visibleCount());
}

TEST_CASE("width rides a splice with its sound, and a clone takes its source's") {
	// REGOLDED FOR V18, deliberately — under cloning, the "ride along" sound params (now including
	// width, later more) follow the clone and the stance. This
	// case's previous claim was the V14a-era rule: the copy stood at the SPENT slot's width, the
	// way it stands at the spent slot's place. Width is a RIDE-ALONG now — it follows the SOUND —
	// so a shifted sound still carries its width with it (that half never changed: the sound
	// moves), and the COPY takes its SOURCE's, not the freed slot's. Place alone keeps the
	// spent-slot rule.
	StackFixture f;
	f.consume(0, 300.0);
	f.consume(1, 500.0);
	f.set(soundParam(0, SoundField::Width), 0.31);
	f.set(soundParam(1, SoundField::Width), 0.72);
	// The slot this act will spend. Its width is the parameter table's own, and it has to differ
	// from the two authored ones so the claims below can tell the three roads apart — it is also
	// exactly what the OVERTURNED behaviour handed the copy, which makes the copy's check its own
	// known-bad.
	const double spentWidth = f.read(static_cast<ParamId>(soundParam(2, SoundField::Width)));
	REQUIRE(spentWidth != doctest::Approx(0.72));
	REQUIRE(spentWidth != doctest::Approx(0.31));

	f.press(f.clonePoint(0));
	f.settle();

	const auto widthOf = [&](size_t slot) {
		return f.read(static_cast<ParamId>(soundParam(slot, SoundField::Width)));
	};
	// B's sound moved down into C and took its width with it — the ride-along following its sound.
	CHECK(widthOf(2) == doctest::Approx(0.72));
	// The source is untouched, and the copy stands at ITS SOURCE'S width (V18). The overturned
	// behaviour produced spentWidth here, so this line is the regold's own known-bad.
	CHECK(widthOf(0) == doctest::Approx(0.31));
	CHECK(widthOf(1) == doctest::Approx(0.31));
	CHECK(widthOf(1) != doctest::Approx(spentWidth));

	// AND A REORDER CARRIES IT TOO, because a reorder moves an existing sound between slots rather
	// than making a new one: what it stands at goes with it.
	f.dragCard(2, 0);
	CHECK(widthOf(0) == doctest::Approx(0.72));

	// One undo puts every field of every slot it touched back, width among them — asserted as whole
	// slot states, so a field added to SlotState after this is covered without editing the case.
	const std::array<SlotState, kMaxSounds> after = f.everySlot();
	f.history.undo();
	f.settle();
	CHECK(widthOf(0) == doctest::Approx(0.31));
	f.history.redo();
	f.settle();
	CHECK(f.everySlot() == after);
}

TEST_CASE("a parked sound can be dragged, and arrives still parked") {
	StackFixture f;
	f.consume(0, 300.0);
	f.consume(1, 500.0);
	f.consume(2, 815.0);
	f.set(ParamId::CEnable, 0.0);
	f.set(ParamId::CPlaceY, -0.37);
	const SlotState parked = readSlotState(f.bridge, 2);
	REQUIRE_FALSE(parked.enabled);

	f.dragCard(2, 0);

	CHECK(readSlotState(f.bridge, 0) == parked);
	CHECK(f.zoneOf(1) == doctest::Approx(300.0));
	CHECK(f.zoneOf(2) == doctest::Approx(500.0));
	CHECK(f.tray.text() == "C moved to 1st");
}


//---------//
// Terrain //
//---------//

// The Explore surface's ground, and the design rule it exists under: THE MAP IS A PURE FUNCTION OF
// POSITION (§4a). Every case below is a statement about that — the same place under two windows, the
// same place at two zooms, the same place with something else selected — because a map that is not
// learnable is a map that does nothing, and the only way it stops being learnable is by quietly
// fitting itself to the view.


TEST_CASE("the two dials reach three units of home between them, which is what a zoom has to fit") {
	// WHAT SURVIVES OF THE DECOMPOSITION, AND WHY THREE CASES BECAME ONE. Zone and Offset used to be
	// the chart's own coordinates: a scrub decomposed the pointer's home into them, clamping at the
	// ends, and three cases here pinned that rule — the round trip, the offset anchoring that keeps
	// PhiStair's family from changing under a decomposition, and the clamp at either end.
	//
	// the one-value decision retired the rule and `homeToCoord` with it. A scrub moves the unbounded
	// base; the two dials are bounded knobs that sum on top and the chart never writes them. So:
	//   - the round trip is gone with the function,
	//   - the family claim is stronger elsewhere, stated where it now lives ("how home is spelled is a
	//     gauge": morph is byte-identical across two spellings of the same homes and stair is not),
	//   - and the clamp is gone entirely, which is the point of the whole change.
	//
	// WHAT IS LEFT IS THE RANGE ITSELF, which is still real and still load-bearing: it is what the
	// widest zoom is sized to (minZoom), and it is the whole reach a host has over this axis through
	// automation now that the chart's own motion lives in patch state. Read from the parameter table
	// rather than restated, so a table edit moves this rather than silently disagreeing with it.
	CHECK(homeCeiling(0.0) - homeFloor(0.0) == doctest::Approx(1023.0 / 1023.0 + 2.0));
	CHECK(homeFloor(0.0) == doctest::Approx(0.0));
	// It still moves with gamma — the dials reach three units either side of wherever the base has
	// carried the sound, which is what makes the vernier a vernier.
	CHECK(homeFloor(7.5) == doctest::Approx(7.5));
	CHECK(homeCeiling(7.5) - homeFloor(7.5) == doctest::Approx(homeCeiling(0.0) - homeFloor(0.0)));
}

TEST_CASE("the map applies nothing the position does not") {
	// THE DESIGN RULE'S OWN CONTROL: the map is a pure function of position, so every texel must be
	// exactly what a bare call at that texel's coordinates returns. Any pass over the built rows — a
	// normalisation, a palette fitted to the window, a contrast stretch — breaks this the moment it is
	// added, and NOTHING ELSE we have could see it: such a map still looks like a map.
	// THE REFERENCE HAS AN OPINION OF ITS OWN. This used to build its expected texel from
	// `window.pin` — a value read back out of the very object under test — so it verified the map was
	// consistent with whatever plane it had been handed and said nothing about that plane being right.
	// The pin is a LITERAL here now, written by the test and passed in, so the two sides can disagree.
	SliceOrientationPin reference;
	reference.pinned = true;
	reference.generator.rotate = 1.1f;

	TerrainMap map;
	TerrainWindow window = windowAt(0.5, 64.0 / 1023.0, 2.0);
	window.pin = reference;
	map.request(window);
	REQUIRE(settle(map, window));
	// ONE SNAPSHOT FOR THE WHOLE CASE, exactly as a frame takes one: asking twice can hand back two
	// different maps, and comparing one map's texels against another's coordinates would be the very
	// thing this case exists to forbid.
	const std::shared_ptr<const TerrainSnapshot> built = map.snapshot();
	REQUIRE(built->window == window);

	// THE REFERENCE IS WHAT A SCRUB TO THAT ROW WOULD WRITE, which since the one-value decision is the
	// sound's own two dials with the whole of the rest of home in that row's base. It used to decompose
	// each row's home into its own zone and offset; a reference that went on doing that would be
	// describing places this chart can no longer take you to.
	const auto zone = static_cast<uint16_t>(std::lround(window.zoneBaseline));
	const auto phaseOffset = static_cast<float>(window.offsetBaseline);
	for (const int row : { 0, 17, 64, 127 }) {
		for (const int column : { 0, 41, 96, 127 }) {
			// **THE RESTATEMENT FOLLOWS THE AXES** (backlog GY). It used to read the row as a HOME and
			// decompose it into this row's own base — which is what the builder did while the vertical
			// axis WAS home. Both axes are travel now and the home is the window's, one for the whole
			// map, so the reference computes what the builder computes: one base, two displacements.
			const double travelY = TerrainMap::travelYAtRow(window, row);
			const double travelX = TerrainMap::travelXAtColumn(window, column);
			const TerrainFeatures features = terrainFeaturesAt(OscType::Morph, reference, zone,
			                                                   phaseOffset, travelX, travelY,
			                                                   kNoPlaneResidue, window.gammaBase);
			const int family = window.engine == OscType::Stair ? stairFamilyAt(zone) : -1;
			CHECK(built->texel(column, row)
			      == terrainColour(features, travelX, travelY, family, colourMapFor(window.engine)));
		}
	}
}

TEST_CASE("panning does not repaint the place it moved past") {
	// The same rule seen from the player's side, and exactly rather than nearly: the second window is the
	// first shifted by a whole number of rows, so the two share texel positions to the bit. A place
	// that changed colour because it moved to the middle of the screen would be a map nobody can learn.
	TerrainMap map;
	const double span = 64.0 / 1023.0;
	constexpr int kShift = 16;

	TerrainWindow first = windowAt(0.5, span, 2.0);
	map.request(first);
	REQUIRE(settle(map, first));
	const std::shared_ptr<const TerrainSnapshot> a = map.snapshot();
	std::vector<uint32_t> before;
	for (int row = kShift; row < TerrainMap::kResolution; ++row) {
		before.push_back(a->texel(TerrainMap::kResolution / 2, row));
	}

	TerrainWindow second = first;
	second.travelYCentre = first.travelYCentre - static_cast<double>(kShift) * span / TerrainMap::kResolution;
	map.request(second);
	REQUIRE(settle(map, second));
	const std::shared_ptr<const TerrainSnapshot> b = map.snapshot();
	// Row r of the first window is row r - kShift of the second: the same home, the same travel.
	for (int row = kShift; row < TerrainMap::kResolution; ++row) {
		CHECK(b->texel(TerrainMap::kResolution / 2, row - kShift)
		      == before[static_cast<size_t>(row - kShift)]);
	}
}

TEST_CASE("the features are a measurement of the waveform, not of the parameters") {
	// Energy, centroid and roughness are read off one cycle of the table the engine would build, so
	// they are bounded the way a measurement is and they MOVE when the character moves. A map whose
	// three channels never varied would be a picture of nothing, and it would still be a pure function
	// of position.
	SliceOrientationPin pin;
	std::vector<TerrainFeatures> seen;
	for (int zone = 0; zone < 1024; zone += 64) {
		const TerrainFeatures f =
		    terrainFeaturesAt(OscType::Morph, pin, static_cast<uint16_t>(zone), 0.0f, 0.0, 0.0);
		CHECK(f.energy >= 0.0f);
		CHECK(f.centroid >= 0.0f);
		CHECK(f.centroid <= 1.0f);
		CHECK(f.roughness >= 0.0f);
		CHECK(f.roughness <= 1.0f);
		seen.push_back(f);
	}
	const auto varies = [&](float TerrainFeatures::*field) {
		float low = seen.front().*field;
		float high = low;
		for (const TerrainFeatures& f : seen) {
			low = std::min(low, f.*field);
			high = std::max(high, f.*field);
		}
		return high - low;
	};
	CHECK(varies(&TerrainFeatures::energy) > 0.02f);
	CHECK(varies(&TerrainFeatures::centroid) > 0.02f);
	CHECK(varies(&TerrainFeatures::roughness) > 0.02f);
}

TEST_CASE("travel reaches characters home cannot") {
	// The second axis has to be REAL or the surface must not draw it. At zero travel the character is
	// the firmware's, bit for bit, whatever the plane; move sideways and it has to become something
	// else, or the horizontal axis is decoration.
	SliceOrientationPin pin;
	const TerrainFeatures home = terrainFeaturesAt(OscType::Morph, pin, 512, 0.0f, 0.0, 0.0);
	const TerrainFeatures away = terrainFeaturesAt(OscType::Morph, pin, 512, 0.0f, 0.8, 0.0);
	const float moved = std::abs(home.energy - away.energy) + std::abs(home.centroid - away.centroid)
	                    + std::abs(home.roughness - away.roughness);
	CHECK(moved > 0.01f);
}

TEST_CASE("the terrain colours the vowel, not the waveform underneath it") {
	// THE SECOND HALF OF THE PICTURE DEFECT. The scope was fixed first; this surface kept drawing the
	// vox-free cycle, so the same place had two different colours depending which surface you asked.
	//
	// The decision that got here is worth stating because it was reversed once: a 32-sample,
	// 12-harmonic lens cannot RESOLVE a vowel, and that was taken to mean it should not draw one.
	// Measured, that is the wrong question — including the vowel makes this reduction 3.5x closer to
	// the truth about its own twelve harmonics than omitting it, across 18 vowel regions in 6 zones,
	// every region improved. The fold is real and smaller than the lie it replaced.
	SliceOrientationPin pin;
	constexpr uint16_t kZone = 0;
	constexpr float kOffset = 1.945f; // vowel country at zone 0 — see the scope's case for why
	const double home = sliceHome(kZone, kOffset, 0.0);
	const TerrainFrames frames = terrainFramesAt(pin, home, kNoPlaneResidue);

	// The position has to be worth testing: a vowel present AND the quantizer awake, so the staircase
	// re-derivation below is load-bearing rather than decorative.
	const float presence = voxPresenceAt(flagshipLeadCoordinate(frames.morph, home, 0.0, 0.0));
	REQUIRE(presence > 0.4f);
	const VoxParams params = buildVoxSliced(kZone, kOffset, 0.0, 0.0, frames.vox, 0.0);
	REQUIRE(voxParamsVoiced(params));
	VoxCycle vowel;
	bakeVoxCycle(params, pitchToHz(kVoxPictureKey), vowel);
	REQUIRE(vowel.voiced);

	// THE REFERENCE, restated here rather than borrowed — three statements, and the prepare is the one
	// that would go silently wrong: `held` is sampled on whatever source the build was given, so a
	// recipe prepared without the vowel quantizes the blend onto a staircase built from a waveform
	// nobody plays, attenuating it by (1 − quantize).
	const FlagshipSource source { &vowel, presence };
	FlagshipRecipe wet = buildFlagshipSliced(kZone, kOffset, 0.0, 0.0, frames.morph, frames.stair, 0.0,
	                        FieldScales {}, kFlagshipNormalisePictureSamples);
	REQUIRE(wet.quantize > 0.2f);
	// THE PICTURE'S PROBE COUNT, because this is a restatement of what the TERRAIN does and the
	// terrain normalises coarsely on purpose (see kFlagshipNormalisePictureSamples). Preparing at the
	// audio default here would compare two different gains and fail on `energy` alone — which is
	// exactly how this case failed when the split landed, centroid and roughness passing because they
	// are ratios and cannot see a gain at all.
	prepareFlagshipRecipe(wet, source, kFlagshipNormalisePictureSamples);

	std::array<float, kTerrainCycleSamples> wetCycle {};
	std::array<float, kTerrainCycleSamples> dryCycle {};
	const FlagshipRecipe dry =
	    buildFlagshipSliced(kZone, kOffset, 0.0, 0.0, frames.morph, frames.stair, 0.0,
	                        FieldScales {}, kFlagshipNormalisePictureSamples);
	for (int i = 0; i < kTerrainCycleSamples; ++i) {
		const double phase = static_cast<double>(i) / static_cast<double>(kTerrainCycleSamples);
		wetCycle[static_cast<size_t>(i)] = evalFlagship(wet, phase, source);
		dryCycle[static_cast<size_t>(i)] = evalFlagship(dry, phase);
	}
	const TerrainFeatures want = terrainFeaturesOfCycle(wetCycle);
	const TerrainFeatures voxFree = terrainFeaturesOfCycle(dryCycle);

	// ASSERT THE VALUE, not that two colours differ — a mutation that drops the source from the
	// PREPARE alone leaves some vowel in the picture, so a difference check passes and the bug ships.
	// That has now caught something twice in this arc.
	const TerrainFeatures got =
	    terrainFeaturesAt(OscType::V5, pin, kZone, kOffset, 0.0, 0.0, kNoPlaneResidue, 0.0);
	CHECK(got.energy == doctest::Approx(want.energy).epsilon(1e-6));
	CHECK(got.centroid == doctest::Approx(want.centroid).epsilon(1e-6));
	CHECK(got.roughness == doctest::Approx(want.roughness).epsilon(1e-6));

	// And the vowel really does move the colour, which is what makes the checks above worth running.
	CHECK(std::abs(want.centroid - voxFree.centroid) > 0.01f);
}

TEST_CASE("the terrain leaves vowel-free places exactly the colour they were") {
	// The other half, and the one that keeps this from being a change to every V5 texel: outside
	// vowel country no source is built, so the branch is the one it always was — bit for bit, which
	// is the only way to tell "the terrain now draws vox" from "the terrain now draws differently".
	SliceOrientationPin pin;
	constexpr uint16_t kZone = 0;
	constexpr float kOffset = 0.4f; // home well under 1: no vowel reaches here
	const double home = sliceHome(kZone, kOffset, 0.0);
	const TerrainFrames frames = terrainFramesAt(pin, home, kNoPlaneResidue);
	REQUIRE(voxPresenceAt(flagshipLeadCoordinate(frames.morph, home, 0.0, 0.0)) == 0.0f);

	const FlagshipRecipe recipe =
	    buildFlagshipSliced(kZone, kOffset, 0.0, 0.0, frames.morph, frames.stair, 0.0,
	                        FieldScales {}, kFlagshipNormalisePictureSamples);
	std::array<float, kTerrainCycleSamples> cycle {};
	for (int i = 0; i < kTerrainCycleSamples; ++i) {
		cycle[static_cast<size_t>(i)] =
		    evalFlagship(recipe, static_cast<double>(i) / static_cast<double>(kTerrainCycleSamples));
	}
	const TerrainFeatures want = terrainFeaturesOfCycle(cycle);
	const TerrainFeatures got =
	    terrainFeaturesAt(OscType::V5, pin, kZone, kOffset, 0.0, 0.0, kNoPlaneResidue, 0.0);
	CHECK(got.energy == want.energy);
	CHECK(got.centroid == want.centroid);
	CHECK(got.roughness == want.roughness);
}

TEST_CASE("a terrain build is affordable off the editor's thread") {
	// Not a benchmark for its own sake: the whole design rests on a build being cheap enough to do on
	// a background thread while the editor keeps its frame rate, and "0.40 µs a table" was measured
	// for the BUILD alone. This measures the build plus the analysis plus the colour, which is what
	// the surface actually pays. The bound is deliberately loose — it is a guard against an order of
	// magnitude, not a target.
	TerrainMap map;
	const auto start = std::chrono::steady_clock::now();
	const TerrainWindow window = windowAt(0.5, 64.0 / 1023.0, 2.0);
	map.request(window);
	REQUIRE(settle(map, window));
	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
	                         std::chrono::steady_clock::now() - start)
	                         .count();
	MESSAGE("terrain build: " << elapsed << " ms for " << TerrainMap::kResolution << "x"
	                          << TerrainMap::kResolution << " texels");
	CHECK(elapsed < 4000);

	// AND THE ENGINE THAT ACTUALLY COSTS SOMETHING, which this case did not cover until the vowel
	// arrived. A V5 texel in vowel country bakes a 256-slot vox cycle and re-derives the staircase
	// against it, and neither is free — where the map above is Morph, which pays none of it. Timed
	// over a row span that straddles vowel country so this is the worst realistic case rather than
	// empty terrain, and reported per texel so it can be read against the 0.40 µs the build costs.
	constexpr int kRows = 64;
	constexpr int kCols = 64;
	SliceOrientationPin pin;
	int voweledRows = 0;
	const auto v5Start = std::chrono::steady_clock::now();
	for (int y = 0; y < kRows; ++y) {
		const double gamma = 1.5 + static_cast<double>(y) * (2.0 / kRows);
		const double rowHome = sliceHome(0, 0.0f, gamma);
		const TerrainFrames frames = terrainFramesAt(pin, rowHome, kNoPlaneResidue);
		if (voxPresenceAt(flagshipLeadCoordinate(frames.morph, rowHome, 0.0, 0.0)) > 0.0f) {
			++voweledRows;
		}
		for (int x = 0; x < kCols; ++x) {
			(void)terrainFeaturesOn(OscType::V5, frames, static_cast<uint16_t>(x * 8), 0.0f,
			                        static_cast<double>(x) / kCols, 0.0, gamma);
		}
	}
	const auto v5Micros = std::chrono::duration_cast<std::chrono::microseconds>(
	                          std::chrono::steady_clock::now() - v5Start)
	                          .count();
	MESSAGE("V5 texels with vowels: " << v5Micros << " us for " << (kRows * kCols) << " ("
	                                  << (static_cast<double>(v5Micros) / (kRows * kCols))
	                                  << " us each, " << voweledRows << "/" << kRows
	                                  << " rows voweled)");
	// Two orders of magnitude of headroom against a 60 Hz frame, off-thread. A guard, not a target.
	CHECK(v5Micros < 4000000);
}

TEST_CASE("a map is abandoned when the view moves, rather than finished for nobody") {
	TerrainMap map;
	map.request(windowAt(0.5, 64.0 / 1023.0, 2.0));
	const TerrainWindow next = windowAt(0.9, 8.0 / 1023.0, 0.25);
	map.request(next);
	REQUIRE(settle(map, next));
	// Whatever it was doing, what it finished is what was last asked for.
	CHECK(map.snapshot()->window == next);
	CHECK(map.snapshot()->rows == TerrainMap::kResolution);
}

TEST_CASE("a chart row and a chart position name each other, both ways") {
	// The inverses carrying is built on. Cheap, and worth writing down because an off-by-half here
	// would move every carried texel by half a texel and look like softness rather than like a bug.
	TerrainWindow window = windowAt(0.37, 64.0 / 1023.0, 2.0);
	for (int row = 0; row < TerrainMap::kResolution; ++row) {
		CHECK(TerrainMap::rowAtTravelY(window, TerrainMap::travelYAtRow(window, row))
		      == doctest::Approx(row).epsilon(1e-12));
	}
	for (int column = 0; column < TerrainMap::kResolution; ++column) {
		CHECK(TerrainMap::columnAtTravelX(window, TerrainMap::travelXAtColumn(window, column))
		      == doctest::Approx(column).epsilon(1e-12));
	}
}

TEST_CASE("the world does not end: a place far outside the old band still draws a real picture") {
	// The world is meant to be infinite, yet zooming all the way out and panning up ran into a wall of
	// nothing. The ENGINE ALREADY AGREED: the character is a pure function of
	// position, defined everywhere on the plane. The band bounds what zone and offset can POINT AT, not
	// what exists. The builder used to clamp and paint alpha zero.
	//
	// **THE REFERENCE HALF OF THIS CASE IS GONE, AND SAYING WHY IS THE POINT** (backlog GY). It used to
	// draw one place by two routes — once by a view that could not address it (zone and offset pinned,
	// the rest carried in the gamma base) and once by a view whose gamma had been carried out to meet
	// it — and require the two pictures to agree. That worked because home was a SUM into which gamma
	// and the dials both fed, so two different splits named one place.
	//
	// Under the current design they no longer do. Gamma is a content input the decode CONSUMES, travel is a
	// displacement in the plane the decode produces, and they enter through different doors: changing
	// the gamma base is changing which plane you are on, not walking to the same spot by another road.
	// Two windows differing in `gammaBase` are two different places, correctly, so the old comparison
	// would now assert something false.
	//
	// SO THE CLAIM IS SPLIT IN TWO, and both halves are kept:
	//   1. INFINITY, which is the actual defect — far outside the old band, real content is drawn
	//      rather than nothing;
	//   2. ONE PLACE, ONE PICTURE, which is what the two-route comparison was really for — the same
	//      travel drawn by two windows that differ only in where they are CENTRED must agree, because a
	//      pan may not change what is at a point.
	const double span = 64.0 / 1023.0;
	const double farY = homeCeiling(0.0) + 4.0; // well past anything zone and offset could ever reach

	TerrainMap unreachable;
	TerrainWindow beyond = windowAt(farY, span, 2.0);
	beyond.gammaBase = 0.0;
	unreachable.request(beyond);
	REQUIRE(settle(unreachable, beyond));
	REQUIRE(unreachable.snapshot()->rows == TerrainMap::kResolution);

	// (1) NOTHING IS DRAWN AS NOTHING OUT HERE ANY MORE — the design, in one assertion.
	//
	// KNOWN-BAD: clamp the row's travel back into the old band and these go to 0, which is the wall of
	// nothing a user panned into.
	const std::shared_ptr<const TerrainSnapshot> a = unreachable.snapshot();
	for (const int row : { 0, TerrainMap::kResolution / 2, TerrainMap::kResolution - 1 }) {
		INFO("row " << row << " is travel-Y " << TerrainMap::travelYAtRow(a->window, row));
		REQUIRE(a->texel(64, row) != 0u);
	}

	// (2) AND THE SAME PLACE SEEN FROM A DIFFERENT VIEW IS THE SAME PICTURE. A second window centred
	// half a span further up overlaps the first; where they overlap, the texels must match row for row.
	// This is the surviving form of "one place, two routes", and it is a stronger statement than the
	// old one in the way that matters — it is about the coordinate the chart actually pans in.
	TerrainMap shifted;
	TerrainWindow moved = windowAt(farY + span * 0.5, span, 2.0);
	moved.gammaBase = 0.0;
	shifted.request(moved);
	REQUIRE(settle(shifted, moved));
	REQUIRE(shifted.snapshot()->rows == TerrainMap::kResolution);
	const std::shared_ptr<const TerrainSnapshot> b = shifted.snapshot();

	int compared = 0;
	int agreed = 0;
	for (int row = 0; row < TerrainMap::kResolution; ++row) {
		const double y = TerrainMap::travelYAtRow(a->window, row);
		const double rowInB = TerrainMap::rowAtTravelY(b->window, y);
		const long nearest = std::lround(rowInB);
		// Only rows the second window actually contains, and only where the two lattices land on the
		// same place rather than on neighbours — comparing neighbours would measure the gradient.
		if (nearest < 0 || nearest >= TerrainMap::kResolution
		    || std::abs(rowInB - static_cast<double>(nearest)) > 1e-6) {
			continue;
		}
		++compared;
		if (a->texel(64, row) == b->texel(64, static_cast<int>(nearest))) {
			++agreed;
		}
	}
	// THE PROBE HAS TO HAVE LOOKED AT SOMETHING, or "they all agreed" is a statement about an empty
	// set — which is exactly how the version of this case that preceded it would have passed.
	INFO("overlapping rows compared: " << compared << ", agreed: " << agreed);
	REQUIRE(compared >= TerrainMap::kResolution / 4);
	CHECK(agreed == compared);
}

TEST_CASE("a carried texel is the one the builder would have computed there") {
	// THE CLAIM CARRYING RESTS ON: the map is a pure function of position, so a texel of the old map
	// is still true at the place it was built for. If that is right, then across a pan the carried
	// texels and a from-scratch build of the new window agree EXACTLY, and the re-centre snap has
	// nothing left to redraw. The reference is the builder itself, computed independently — not a
	// second copy of carry()'s own arithmetic asking itself whether it agrees.
	//
	// The pan is a whole number of rows, which is the case in which the claim is exact: the two grids
	// land on the same homes. Off-grid, carrying is the nearest texel and is replaced within one build
	// anyway, so exactness there is not claimed and is not tested for.
	const double span = 64.0 / 1023.0;
	const int panRows = 32;
	const TerrainWindow first = windowAt(0.5, span, 2.0);
	TerrainWindow second = first;
	second.travelYCentre += span * static_cast<double>(panRows) / TerrainMap::kResolution;

	TerrainMap builder;
	builder.request(first);
	REQUIRE(settle(builder, first));
	const TerrainSnapshot before = *builder.snapshot();
	builder.request(second);
	REQUIRE(settle(builder, second));
	const TerrainSnapshot after = *builder.snapshot();
	REQUIRE(before.rows == TerrainMap::kResolution);
	REQUIRE(after.rows == TerrainMap::kResolution);

	TerrainSnapshot carried;
	carried.window = second;
	carried.texels.assign(static_cast<size_t>(TerrainMap::kResolution) * TerrainMap::kResolution, 0u);
	REQUIRE(TerrainMap::carry(carried, before));
	CHECK(carried.carried);

	// Every row the old map reaches: identical to what the builder produced for the new window.
	size_t compared = 0;
	for (int row = panRows; row < TerrainMap::kResolution; ++row) {
		for (int column = 0; column < TerrainMap::kResolution; ++column) {
			REQUIRE(carried.texel(column, row) == after.texel(column, row));
			++compared;
		}
	}
	CHECK(compared == static_cast<size_t>(TerrainMap::kResolution - panRows) * TerrainMap::kResolution);

	// And the leading edge is NOT invented. The rows the old map never covered stay empty, which is
	// what makes a pan a strip filling in rather than a smear of the nearest colour.
	for (int row = 0; row < panRows; ++row) {
		for (int column = 0; column < TerrainMap::kResolution; ++column) {
			REQUIRE(carried.texel(column, row) == 0u);
		}
	}

	// THE CONTROL, in the case rather than beside it: can these assertions tell a positional carry
	// from the obvious wrong one? The obvious wrong one is a straight buffer copy — index to index,
	// ignoring that the window moved — so require that the straight copy would have FAILED the
	// assertion above. Without this the 12,288 REQUIREs are consistent with carry() having done
	// nothing clever at all, and a pan is exactly the case where doing nothing looks plausible.
	size_t indexCopyWouldMatch = 0;
	for (int row = panRows; row < TerrainMap::kResolution; ++row) {
		for (int column = 0; column < TerrainMap::kResolution; ++column) {
			indexCopyWouldMatch += before.texel(column, row) == after.texel(column, row) ? 1 : 0;
		}
	}
	const size_t overlap =
	    static_cast<size_t>(TerrainMap::kResolution - panRows) * TerrainMap::kResolution;
	MESSAGE("an index-to-index copy would have matched " << indexCopyWouldMatch << " of " << overlap
	                                                     << " texels");
	CHECK(indexCopyWouldMatch < overlap / 2);

	// AND UNDER A ZOOM, which is the other thing a gesture does to a window: the Zoom corner and the
	// wheel both change the SPAN mid-drag, and then position and index disagree everywhere. The two
	// grids share no home at all there, so exactness against a from-scratch build is not the claim —
	// a carried texel is the NEAREST one and is replaced within a build. What must be true is that it
	// is the nearest one BY POSITION, because that is the difference between a blurred seed refined
	// live and garbage placed confidently.
	//
	// THE REFERENCE IS BRUTE FORCE, deliberately: it searches every source row for the smallest
	// difference in home rather than inverting the mapping, so it shares no arithmetic with the
	// rowAtTravelY/columnAtTravelX that carry() uses. A scale error, an off-by-half or an index-shift
	// would all survive a check written with the same inverse and all fail this one.
	//
	// 2.5x AND NOT 2x OR 4x, and the reason is worth a line because the first draft of this case went
	// red on correct code. At an integer zoom every zoomed row lands EXACTLY halfway between two
	// source rows; `lround` rounds half away from zero and a first-wins search takes the lower index,
	// so the two disagree on every row about a choice that is genuinely arbitrary. At 2.5x the
	// fractional parts cycle through a quarter and three quarters and never land on a half, so
	// "nearest" has one answer and the comparison is about placement rather than about tie-breaking.
	TerrainWindow zoomed = first;
	zoomed.travelYSpan = span * 2.5;
	zoomed.travelXSpan = first.travelXSpan * 2.5;
	TerrainSnapshot intoZoom;
	intoZoom.window = zoomed;
	intoZoom.texels.assign(static_cast<size_t>(TerrainMap::kResolution) * TerrainMap::kResolution, 0u);
	REQUIRE(TerrainMap::carry(intoZoom, before));

	const auto nearestRow = [&](double home) {
		int best = 0;
		double bestGap = std::numeric_limits<double>::max();
		// Wider than the map on both sides, so "the nearest row is off the end" is representable and
		// is not silently rounded to an edge row.
		for (int row = -TerrainMap::kResolution; row < 2 * TerrainMap::kResolution; ++row) {
			const double gap = std::abs(TerrainMap::travelYAtRow(first, row) - home);
			if (gap < bestGap) {
				bestGap = gap;
				best = row;
			}
		}
		return best;
	};
	const auto nearestColumn = [&](double travel) {
		int best = 0;
		double bestGap = std::numeric_limits<double>::max();
		for (int column = -TerrainMap::kResolution; column < 2 * TerrainMap::kResolution; ++column) {
			const double gap = std::abs(TerrainMap::travelXAtColumn(first, column) - travel);
			if (gap < bestGap) {
				bestGap = gap;
				best = column;
			}
		}
		return best;
	};

	int carriedRows = 0;
	int emptyRows = 0;
	for (int row = 0; row < TerrainMap::kResolution; ++row) {
		const int source = nearestRow(TerrainMap::travelYAtRow(zoomed, row));
		if (source < 0 || source >= before.rows) {
			// The old map does not reach here, so nothing may have been invented.
			for (int column = 0; column < TerrainMap::kResolution; ++column) {
				REQUIRE(intoZoom.texel(column, row) == 0u);
			}
			++emptyRows;
			continue;
		}
		for (int column = 0; column < TerrainMap::kResolution; ++column) {
			const int sourceColumn = nearestColumn(TerrainMap::travelXAtColumn(zoomed, column));
			const uint32_t expected = sourceColumn >= 0 && sourceColumn < TerrainMap::kResolution
			                              ? before.texel(sourceColumn, source)
			                              : 0u;
			REQUIRE(intoZoom.texel(column, row) == expected);
		}
		++carriedRows;
	}
	// Both halves have to be non-trivial or the loop above proves nothing: a quarter of a 4x zoomed-out
	// view is the old window, so roughly a quarter carries and the rest stays empty.
	CHECK(carriedRows >= 40);
	CHECK(emptyRows >= 40);
	MESSAGE("zoomed 2.5x: " << carriedRows << " rows carried by position, " << emptyRows
	                        << " left empty");
}

TEST_CASE("a rebuild does not blank the chart, and nowhere stays nowhere") {
	// THE FLICKER, AS A NUMBER. Explore asks for a window every frame it draws, so a held gesture is a
	// request per frame; the builder coalesces to one map per completion and used to publish each of
	// them starting from an empty buffer. Measured over a 120-frame gesture before this changed: 104
	// frames in 120 drew an incomplete map and the average frame was 43% VOID, sawtoothing from 16
	// rows to 128 and back about thirty times a second. That is what "the map flickers" was.
	//
	// The assertion is about what a FRAME DRAWS, not about how much has been built — the rebuild is
	// meant to keep happening, and `rows` still sawtooths exactly as it did. What may not happen is
	// the chart going blank while it does.
	const double span = 64.0 / 1023.0;
	const size_t all = static_cast<size_t>(TerrainMap::kResolution) * TerrainMap::kResolution;
	const auto voidFraction = [](const TerrainSnapshot& snapshot) {
		size_t transparent = 0;
		for (const uint32_t texel : snapshot.texels) {
			transparent += (texel >> 24) == 0u ? 1 : 0;
		}
		return static_cast<double>(transparent) / static_cast<double>(all);
	};
	struct Published {
		TerrainWindow window;
		int rows = 0;
		double voidFraction = 0.0;
	};
	std::mutex publishedLock;
	std::vector<Published> published;
	TerrainMap map;

	// **EVERY PUBLISH, NOT A SAMPLE OF THEM** (`observePublishesForTest`). The claim is about what each
	// rebuild hands the chart, and a once-a-frame look sees whichever publish the host's speed put in
	// front of it — see the verdict at the bottom for what that cost.
	//
	// A SUMMARY PER PUBLISH, NOT THE SNAPSHOT: holding every one would keep a few hundred maps alive.
	// The storage is declared BEFORE the map, so the worker that writes it is joined before it goes.
	map.observePublishesForTest([&](const std::shared_ptr<const TerrainSnapshot>& snapshot) {
		const Published one { snapshot->window, snapshot->rows, voidFraction(*snapshot) };
		std::lock_guard<std::mutex> lock(publishedLock);
		published.push_back(one);
	});
	const auto publishedSoFar = [&] {
		std::lock_guard<std::mutex> lock(publishedLock);
		return published;
	};

	// WARMED UP FIRST, so the cold start is not a special case inside the measurement. Before any map
	// exists there is nothing to carry and an empty chart is the honest picture; the flicker was never
	// about the first map, it was about all the ones after it.
	const TerrainWindow opening = windowAt(0.5, span, 2.0);
	map.request(opening);
	REQUIRE(settle(map, opening));
	const size_t warmUpPublishes = publishedSoFar().size();
	REQUIRE(warmUpPublishes > 0);

	double worst = 0.0;
	double total = 0.0;
	int frames = 0;
	int snaps = 0;
	double centre = 0.5;
	bool sawIncomplete = false;
	TerrainWindow last = opening;
	const uint64_t buildsBefore = map.builds();
	for (int frame = 1; frame <= 120; ++frame) {
		// Gamma creeping under a hand, which is the gesture that moves the window every frame — AND
		// the view snapping after it, which is what the surface does when the star leaves the chart.
		// Both halves matter: the creep alone overlaps perfectly and would never exercise the leading
		// edge a re-centre exposes, so a case built on it would pass while the snap still flashed.
		const double gamma = 0.002 * static_cast<double>(frame);
		const double home = 0.5 + gamma;
		// The surface's own rule: the view holds while the star is on the chart and snaps onto it when
		// it leaves, so the jump is half a span and half the new view is terrain that has never been
		// computed. Modelled rather than assumed, because a creep with the view held overlaps perfectly
		// and a case built on that would pass while every snap still flashed.
		if (std::abs(home - centre) > span * 0.5) {
			centre = home;
			++snaps;
		}
		TerrainWindow window = windowAt(centre, span, 2.0);
		window.gammaBase = gamma;
		map.request(window);
		last = window;
		const std::shared_ptr<const TerrainSnapshot> snapshot = map.snapshot();
		const double fraction = voidFraction(*snapshot);
		worst = std::max(worst, fraction);
		total += fraction;
		++frames;
		sawIncomplete = sawIncomplete || snapshot->rows < TerrainMap::kResolution;
		std::this_thread::sleep_for(std::chrono::milliseconds(16));
	}
	REQUIRE(frames == 120);
	// **THIS GUARD USED TO BE `REQUIRE(sawIncomplete)` AND IT IS NOW UNSATISFIABLE — ON PURPOSE.**
	//
	// It asked that the gesture catch the builder mid-map, because a run where every frame saw a
	// finished map would pass the void bound while proving nothing. That was the right guard while a
	// build recomputed the ~90% of its texels it had just carried exactly: a drag then left the map
	// part built on essentially every frame (measured 2026-09-19, backlog GJ — row counts cycling
	// `16 48 80 128 32 64 96 0 ...` and 100 of 100 pan frames incomplete).
	//
	// `TerrainSnapshot::computed` ended that: an exactly carried texel is not re-earned, so a pan's
	// builds finish inside a frame and NO frame of this gesture is incomplete. The old guard would now
	// fail for the best possible reason, and deleting it to go green would be relaxing a guard to
	// accommodate a change — so it is REPLACED by one that still does its job.
	//
	// The job is "prove the builder was actually exercised", and builds() answers it without depending
	// on the builder being slow.
	INFO("builds over the gesture: " << (map.builds() - buildsBefore));
	REQUIRE(map.builds() > buildsBefore);
	// **REPORTED, NOT ASSERTED, AND THE DIFFERENCE IS THE POINT.** A first version of this said
	// CHECK_FALSE(sawIncomplete) — "the builder now keeps up" — and it was red at load 29 on this very
	// box while being green on a quiet one. That is a claim about a CLOCK wearing the clothes of a
	// claim about the code, which is the trap `vox-voice` and `flagship` already pay for here. How
	// much of a map is ready when a frame happens to look depends on what else the machine is doing.
	//
	// The load-independent half of the same claim — that a pan does not recompute what it exactly
	// carried — is asserted in "a pan computes only the ground it has not seen before", against
	// `texelsComputed()`, which counts WORK rather than time.
	MESSAGE("frames that saw a part-built map: " << (sawIncomplete ? "some" : "none"));
	// And the gesture has to have actually snapped, or it measured the easy half of the problem.
	REQUIRE(snaps > 0);
	MESSAGE("re-centre snaps in the gesture: " << snaps);
	// **REPORTED, NOT ASSERTED — THE FRAME MEANS WERE A CLOCK** (backlog HF). This case used to assert
	// a mean below 10% and a worst below 60% over these 120 frames, and both are ratios of BUILDER time
	// to SLEEP time: a snap leaves half the view to be computed, and the frames that see it void are
	// however many 16 ms sleeps the build takes — a build that is CPU and a sleep that is not. MEASURED
	// 2026-09-23 on one binary: 1.6% on a quiet box, 19% at load 43 with 6 builds in 120 frames, and
	// the ASan sightings (14-30%) are the same arithmetic. And its WORST could not tell the bug from
	// the load: when a build outlasts two snaps, the map it replaces does not overlap the new window
	// AT ALL, so there is honestly nothing to carry and the chart shows its first 16 rows — 87.5% void,
	// which is also exactly what the flicker drew. Kept as a report because it is what a player would
	// see on this host; the claim is asserted below, per publish, where the host cannot move it.
	MESSAGE("void a frame drew: mean " << (100.0 * total / frames) << "%, worst " << (100.0 * worst)
	                                   << "%, over " << frames << " frames");

	// AND THE TWO REBUILDS EVERY HOST SEES, so the per-publish check below is never vacuous. How the
	// gesture's requests coalesce is the host's business: a slow one may build only windows that do not
	// overlap what they replace, which makes the carry bound trivially true. These start from a SETTLED
	// map, so the map each replaces is known — a gamma creep (the whole view carries) and a snap (half
	// of it does). Gamma moves in both, as in the gesture, so the wider prefetch is another world and
	// is not carried: the only thing that can fill these views is the map they replace.
	REQUIRE(settle(map, last));
	TerrainWindow creep = last;
	creep.gammaBase += 0.002;
	map.request(creep);
	REQUIRE(settle(map, creep));
	TerrainWindow snap = windowAt(creep.travelYCentre + span * 0.5, span, 2.0);
	snap.gammaBase = creep.gammaBase + 0.002;
	map.request(snap);
	REQUIRE(settle(map, snap));

	// **THE CLAIM, BY VALUE, FOR EVERY PUBLISH OF EVERY REBUILD.** A rebuild starts where a publish's
	// window differs from the one before it, and the map it replaced is that earlier publish — which
	// must be FINISHED, because a build runs to completion before the next begins. Against it, how much
	// of the new window is new ground is geometry: the centres are some number of texels apart on each
	// axis, and the old map covers the rest. Restated here from the windows alone, not through `carry`.
	//
	//   A REBUILD DOES NOT BLANK THE CHART: no publish is more void than the ground the old map did not
	//   cover, plus whatever the old map itself left void. A rebuild from an empty buffer — the flicker —
	//   first publishes 16 computed rows, 87.5% void, against a bound of a few percent on a creep.
	//
	//   NOWHERE STAYS NOWHERE: new ground is void until a row of THIS build reaches it. Nothing may be
	//   invented there, so a publish is at least that void, less the rows it has computed.
	//
	// SLACK OF ONE ROW AND ONE COLUMN, because a texel on the boundary is placed by nearest neighbour
	// and either answer is the right one.
	const std::vector<Published> publishes = publishedSoFar();
	REQUIRE(publishes.size() > warmUpPublishes);
	const double slack = 2.0 / static_cast<double>(TerrainMap::kResolution);
	int rebuilds = 0;
	int carriedWhole = 0;
	int carriedPart = 0;
	int checked = 0;
	// The warm-up's last publish: the map the gesture's first rebuild replaces.
	size_t replaced = warmUpPublishes - 1;
	double newGround = 0.0;
	for (size_t at = warmUpPublishes; at < publishes.size(); ++at) {
		const Published& now = publishes[at];
		const Published& before = publishes[at - 1];
		if (!(now.window == before.window)) {
			INFO("publish " << at << " starts a rebuild replacing a map of " << before.rows << " rows");
			REQUIRE(before.rows == TerrainMap::kResolution);
			replaced = at - 1;
			const double cellY = now.window.travelYSpan / static_cast<double>(TerrainMap::kResolution);
			const double cellX = now.window.travelXSpan / static_cast<double>(TerrainMap::kResolution);
			const double shiftRows = std::abs(now.window.travelYCentre - before.window.travelYCentre) / cellY;
			const double shiftColumns =
			    std::abs(now.window.travelXCentre - before.window.travelXCentre) / cellX;
			const double coveredY = std::max(0.0, 1.0 - shiftRows / TerrainMap::kResolution);
			const double coveredX = std::max(0.0, 1.0 - shiftColumns / TerrainMap::kResolution);
			newGround = 1.0 - coveredY * coveredX;
			++rebuilds;
			carriedWhole += newGround < slack ? 1 : 0;
			carriedPart += newGround > 0.3 && newGround < 0.7 ? 1 : 0;
		}
		// Same span on both sides throughout this gesture, which the geometry above assumes.
		REQUIRE(now.window.travelYSpan == publishes[replaced].window.travelYSpan);
		REQUIRE(now.window.travelXSpan == publishes[replaced].window.travelXSpan);
		const double replacedVoid = publishes[replaced].voidFraction;
		const double fraction = now.voidFraction;
		const double built = static_cast<double>(now.rows) / static_cast<double>(TerrainMap::kResolution);
		INFO("publish " << at << ": " << now.rows << " rows computed, " << (100.0 * fraction)
		                << "% void, " << (100.0 * newGround) << "% new ground, replaced map "
		                << (100.0 * replacedVoid) << "% void");
		CHECK(fraction <= newGround + replacedVoid + slack);
		CHECK(fraction >= newGround - built - slack);
		++checked;
	}
	MESSAGE("publishes checked: " << checked << " over " << rebuilds << " rebuilds (" << carriedWhole
	                              << " carrying the whole view, " << carriedPart << " carrying part)");
	// NOT VACUOUS: the two settled rebuilds above guarantee one of each on any host.
	CHECK(carriedWhole >= 1);
	CHECK(carriedPart >= 1);
}

TEST_CASE("the gesture's published maps, written out for looking at") {
	// THE INSTRUMENT THAT PHOTOGRAPHED THE FLICKER, kept because the acceptance evidence for the fix
	// above is a picture and the next person has to be able to remake it. Off unless asked:
	//
	//   FEATHERS_DUMP_DIR=/some/dir ./tests/test_gui_widgets -tc="the gesture's published maps*"
	//   # then, for a sequence in one image:
	//   convert frame_03{0,1,2,3,4,5,6,7,8,9}.ppm -scale 200% +append strip.png
	//
	// WHY NOT THE WINDOWED PROBE, which is the venue for anything on screen. Its camera is an `import`
	// subprocess costing about 100 ms a frame and a terrain build is 31 ms, so every picture it can
	// take lands on a FINISHED map: the chart was blank in most of the frames the editor actually drew
	// and photographed perfectly clean, measured, zero void pixels inside the disk. Slowing the
	// builder to bring the transient inside the camera's reach was tried and is a trap — at a 150 ms
	// dwell the map never completes in EITHER arm, so both arms photograph an empty chart and the
	// instrument reports no difference where there is a large one. This writes what the surface
	// uploads, at the rate it is published, and needs no camera at all.
	//
	// PPM because it needs no library, and a map is 128x128.
	const char* out = std::getenv("FEATHERS_DUMP_DIR");
	if (out == nullptr) {
		return;
	}
	// WHICH ENGINE AND WHICH GESTURE, because the two engines fail differently and one of them is the
	// honest worst case. Under PhiMorph a gamma nudge lands on the same characters — home is the whole
	// coordinate — so the carried seed IS the answer and the picture barely moves. Under PhiStair the
	// pattern family is `zone >> 7` and a nudge really does slide the family boundaries, so the seed
	// is position-correct but a moment out of date and the new rows visibly sweep down over it. That
	// sweep is what the fix promises for rotation too, and it is the thing worth showing.
	const char* engineName = std::getenv("FEATHERS_DUMP_ENGINE");
	const OscType engine =
	    engineName != nullptr && std::strcmp(engineName, "stair") == 0 ? OscType::Stair : OscType::Morph;
	const bool rotating = std::getenv("FEATHERS_DUMP_ROTATE") != nullptr;

	TerrainMap map;
	const double span = 64.0 / 1023.0;
	TerrainWindow opening = windowAt(0.5, span, 2.0);
	opening.engine = engine;
	map.request(opening);
	REQUIRE(settle(map, opening));

	double centre = 0.5;
	int written = 0;
	std::shared_ptr<const TerrainSnapshot> last;
	for (int frame = 1; frame <= 90; ++frame) {
		// The same gesture the case above measures: gamma creeping, the view snapping after it.
		TerrainWindow window = windowAt(centre, span, 2.0);
		window.engine = engine;
		if (rotating) {
			// ROTATION: the sound's plane turns, which changes the character at EVERY position and so
			// makes nothing carryable. The view holds still, exactly as it does under the rotate corner.
			window.pin.pinned = true;
			window.pin.generator.rotate = 0.30f + 0.004f * static_cast<float>(frame);
		}
		else {
			// GAMMA, THE WAY THE SURFACE ACTUALLY DRIVES IT: the view follows gamma one for one, because
			// the reachable band is `[gammaSum, gammaSum + 3]` and a view that stayed put while gamma
			// moved would leave the world (see viewCentre()). The first version of this modelled the view
			// SNAPPING after the star instead, which is a gesture nobody makes — the corner holds
			// `grabbed_`, so update()'s re-centre never runs — and it is why the instrument measured 1.2%
			// void where a real hand got 100%.
			const double gamma = 0.002 * static_cast<double>(frame);
			centre = 0.5 + gamma;
			window.travelYCentre = centre;
			window.gammaBase = gamma;
		}
		map.request(window);
		const std::shared_ptr<const TerrainSnapshot> snapshot = map.snapshot();
		if (snapshot != last) {
			last = snapshot;
			char path[512];
			std::snprintf(path, sizeof(path), "%s/frame_%03d.ppm", out, written++);
			std::FILE* file = std::fopen(path, "wb");
			REQUIRE(file != nullptr);
			std::fprintf(file, "P6\n%d %d\n255\n", TerrainMap::kResolution, TerrainMap::kResolution);
			for (int row = 0; row < TerrainMap::kResolution; ++row) {
				for (int column = 0; column < TerrainMap::kResolution; ++column) {
					const uint32_t texel = snapshot->texel(column, row);
					// A transparent texel is drawn as the surface's own void ground rather than as a
					// debug colour, so the picture is what a frame would have shown.
					const bool nothing = (texel >> 24) == 0u;
					const unsigned char rgb[3] = {
						static_cast<unsigned char>(nothing ? 2 : ((texel >> 16) & 0xff)),
						static_cast<unsigned char>(nothing ? 3 : ((texel >> 8) & 0xff)),
						static_cast<unsigned char>(nothing ? 8 : (texel & 0xff)),
					};
					std::fwrite(rgb, 1, 3, file);
				}
			}
			std::fclose(file);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(16));
	}
	MESSAGE("distinct published maps written: " << written);
}

// **THE GAMMA-SWEEP CASE MOVED TO THE EXPLORE SUITE** (backlog GY, 2026-09-20).
//
// It lived here and drove `TerrainMap` directly, and its whole mechanism was the gamma-follow: the
// view moved with gamma, so `carry()` matched on `home - gammaSum` — the chart's addressable
// coordinate — and the case's known-bad was "match on absolute home instead". Under the current design
// the view does not follow gamma, `carry()` matches on the displacement itself, and that known-bad
// names a line that no longer exists.
//
// THE GUARANTEE IT PROTECTED IS STILL REAL AND HAS A NEW OWNER. Gamma is now in the content key, so a
// sweep driven straight at the map SHOULD rebuild — that is correctness, and the inverted case
// "gamma moves the whole plane" asserts it. What must not happen is the CHART blanking while a hand
// turns the corner, and that is prevented by `ExploreStage::heldGammaBase_` holding the map still for
// the gesture. A stage guarantee needs a stage fixture, which this suite does not have.
//
// Moved rather than deleted: see "the Gamma corner holds the map still while it turns" in
// test_gui_widgets_explore.cpp.
TEST_CASE("how home is spelled is a gauge: morph draws the same map, stair only changes family") {
	// THE REGRESSION GUARD FOR THE NEW DECOMPOSITION, and the strongest one available: it is what
	// stands in for "compare against the shipped build", which no in-repo case can do directly.
	//
	// The builder used to give each ROW its own zone and offset (pinning them at the band edge and
	// putting the overflow in that row's base); it now gives every row the SOUND's two dials and puts
	// the whole of the rest in the base. Both spellings preserve `zone/1023 + phaseOffset + gammaBase`
	// exactly — that was (A)'s guarantee and it is this one's — so if the picture depends on nothing but
	// that sum, the change cannot have moved a single morph pixel. This case asserts precisely that
	// independence, over two spellings further apart than any two builders would ever choose.
	//
	// AND IT NAMES THE ONE THING THAT IS NOT A GAUGE. `family = min(7, zone >> 7)` reads the raw dial
	// — here and on the hardware alike (phi_stair.cpp:74-76) — so under PhiStair the spelling is
	// audible, which is exactly why the chart may no longer choose it. That is a deliberate
	// decision: the dial picks the family, the chart travels within it.
	const double span = 64.0 / 1023.0;
	const double centre = 1.4;

	for (const OscType engine : { OscType::Morph, OscType::Stair, OscType::Resonate }) {
		// **THE SUM IS WHAT HAS TO MATCH, AND SAYING SO IS NOW THE CASE'S JOB** (backlog GY).
		//
		// This used to set the two dials differently and leave the base alone, and the maps came out
		// identical — because the VERTICAL AXIS supplied each row's home and the builder subtracted the
		// dials back out of it, so the dials cancelled and only the row's position survived. Under the
		// current design the vertical axis is travel-Y and the home is `zone/1023 + offset + gammaBase`, so two
		// windows with different dials and the same base are two DIFFERENT homes. The old setup was
		// testing the cancellation, not the invariant.
		//
		// So the spellings are made to sum equal on purpose. `highDial` carries the dials and
		// `lowDial` carries the same amount in its base; both address the identical place, and the
		// claim — that a decomposition is a spelling and not a position — is the one that survives.
		const double spelled = 900.0 / 1023.0 + 1.5;
		TerrainWindow lowDial = windowAt(centre, span, span);
		lowDial.engine = engine;
		lowDial.zoneBaseline = 0.0;
		lowDial.offsetBaseline = 0.0;
		lowDial.gammaBase = spelled;
		TerrainWindow highDial = lowDial;
		highDial.zoneBaseline = 900.0;
		highDial.offsetBaseline = 1.5;
		highDial.gammaBase = 0.0;

		TerrainMap a;
		a.request(lowDial);
		REQUIRE(settle(a, lowDial));
		TerrainMap b;
		b.request(highDial);
		REQUIRE(settle(b, highDial));
		const std::shared_ptr<const TerrainSnapshot> low = a.snapshot();
		const std::shared_ptr<const TerrainSnapshot> high = b.snapshot();
		REQUIRE(low->rows == TerrainMap::kResolution);
		REQUIRE(high->rows == TerrainMap::kResolution);
		REQUIRE(low->texel(64, 64) != 0u);

		size_t same = 0;
		size_t total = 0;
		for (int row = 0; row < TerrainMap::kResolution; ++row) {
			for (int column = 0; column < TerrainMap::kResolution; ++column) {
				same += low->texel(column, row) == high->texel(column, row) ? 1 : 0;
				++total;
			}
		}
		INFO("engine " << static_cast<int>(engine) << ": " << same << " of " << total
		               << " texels identical across two spellings of the same homes");
		if (engine != OscType::Stair) {
			// BYTE FOR BYTE, EVERYWHERE. The spelling is invisible, so this change moved no pixel. Resonate's
			// laws read the home alone (no family), so it is a gauge exactly as Morph is.
			CHECK(same == total);
		}
		else {
			// And under stair it is visible — family 0 against family 7 — which is by design, not a
			// bug. Asserted rather than merely expected, so that a future engine change which quietly
			// made stair spelling-independent would be noticed here rather than believed.
			CHECK(same < total);
		}
	}

	// AND THE FAMILY IS ONE FAMILY ALL THE WAY DOWN, which is the visible half of the design: a scrub
	// moves the base and cannot change `zone >> 7`, so a map striped by family would be advertising
	// landscapes this chart cannot reach. Checked by building a view a whole band tall — the span that
	// used to cross every one of the eight families — and asking the reference what family each row is.
	TerrainWindow tall = windowAt(1.5, 3.0, span);
	tall.engine = OscType::Stair;
	tall.zoneBaseline = 384.0; // family 3, and nothing on the chart may move it
	tall.offsetBaseline = 0.0;
	TerrainMap map;
	map.request(tall);
	REQUIRE(settleRefined(map, tall));
	const std::shared_ptr<const TerrainSnapshot> built = map.snapshot();
	REQUIRE(built->rows == TerrainMap::kResolution);
	const auto zone = static_cast<uint16_t>(std::lround(tall.zoneBaseline));
	REQUIRE(stairFamilyAt(zone) == 3);
	// THE REFERENCE MEANS OVER THE CELL, because the builder does. A view this wide gives each texel a
	// 24-zone cell against stair's finest feature of 34.5, so the schedule asks for more than one
	// sample and a point-sampled reference would be comparing against a picture nobody draws.
	const double cellHome = tall.travelYSpan / static_cast<double>(TerrainMap::kResolution);
	const double cellTravel = tall.travelXSpan / static_cast<double>(TerrainMap::kResolution);
	const int samples = terrainSamplesFor(OscType::Stair, cellHome * 1023.0);
	INFO("cell " << cellHome * 1023.0 << " zones, " << samples << " samples a texel");
	// One, under the cap — so this reference is the point sample, and the loop below is written to
	// mean over whatever the schedule says so that it keeps working if the cap is ever raised.
	REQUIRE(samples >= 1);
	for (const int row : { 0, 32, 64, 96, 127 }) {
		// backlog GY: the row is travel-Y and the home is the window's, one for the whole map.
		const double travelY = TerrainMap::travelYAtRow(tall, row);
		// The column's own travel, not zero: the centre of a 128-wide grid falls between two texels, so
		// a reference built at travel zero is a reference for a place half a texel to the left.
		const double travelX = TerrainMap::travelXAtColumn(tall, 64);
		// ONLY THE CHARACTER IS MEANED NOW. The macro used to be averaged over the cell beside it; the
		// country left the draw with that decision, and stair's family is constant across a map under
		// the one-family rule, so there is nothing else in a texel that a cell can summarise.
		TerrainFeatures mean {};
		for (int sample = 0; sample < samples; ++sample) {
			const TerrainSampleOffset at = terrainSampleOffset(sample, samples);
			// backlog GY: the sample offsets straddle the cell on BOTH travel axes now.
			const double sampleTravelY = travelY + at.home * cellHome;
			const double sampleTravelX = travelX + at.travel * cellTravel;
			const TerrainFeatures one =
			    // backlog GY: the base is the WINDOW's, passed straight through. It used to be derived
			    // by subtracting the dials out of the row's home, because the row supplied the home.
			    terrainFeaturesAt(OscType::Stair, tall.pin, zone, 0.0f, sampleTravelX, sampleTravelY,
			                      kNoPlaneResidue, tall.gammaBase);
			mean.energy += one.energy;
			mean.centroid += one.centroid;
			mean.roughness += one.roughness;
			// Every feature the colour reads — this fixture mirrors the builder's cell mean, and a
			// mirror that carries a subset reports a difference the builder does not have. Both this and
			// the builder itself were caught by the reconstruction case on the first run with the second
			// colour axis in.
			mean.parity += one.parity;
			mean.fundamental += one.fundamental;
		}
		const auto scale = 1.0f / static_cast<float>(samples);
		mean.energy *= scale;
		mean.centroid *= scale;
		mean.roughness *= scale;
		mean.parity *= scale;
		mean.fundamental *= scale;
		INFO("row " << row << " at travelY " << travelY << ", travelX " << travelX);
		CHECK(built->texel(64, row) == terrainColour(mean, travelX, travelY, 3, colourMapFor(OscType::Stair)));
	}
}

TEST_CASE("a texel is the mean of its own cell, and at every rung shipped so far that is one point") {
	// Span averaging, and averaging the samples within an xy volume rather than along a row — this is
	// both of those, and the second half is the one that makes the
	// change shippable.
	//
	// THE LOAD-BEARING PROPERTY IS THE CHEAP ONE: at every zoom this surface has ever had, a texel is
	// still ONE sample, so nothing anyone is looking at today costs a microsecond more or moves a
	// pixel. That is not a saving, it is the difference between a change nobody notices and a change
	// that makes the whole ladder slower to serve the two rungs at the end of it.
	SUBCASE("K=1 wherever a cell is smaller than the feature it would average") {
		// Morph's finest feature is 169 zones, stair's 34.5 — measured, see the wavelength case in the
		// slice-build suite. Half of that is the most a cell can cover and still be sure of what is in
		// it, and today's whole ladder sits far inside that for morph.
		for (double zoom = 1.0; zoom <= ExploreStage::kMaxZoom; zoom *= 2.0) {
			const double cell = ExploreStage::kBaseZones / zoom / 128.0;
			INFO("zoom x" << zoom << ": cell " << cell << " zones");
			CHECK(terrainSamplesFor(OscType::Morph, cell) == 1);
			CHECK(terrainSamplesFor(OscType::Stair, cell) == 1);
		}
		// And at the widest stop this surface shipped with — 3069 zones, 24 a texel — morph needs nothing
		// while STAIR ALREADY CANNOT RESOLVE what a texel covers. That is not a problem this change
		// introduced; it is an old one it found: stair's glyph pattern has a Nyquist span of 2208 zones
		// and we shipped 3069. Under the cap nothing is DONE about it — the raw picture is preferred —
		// so the count is one either way and what this pins is that the finding is still on the record.
		const double shipped = 3069.0 / 128.0;
		CHECK(terrainSamplesFor(OscType::Morph, shipped) == 1);
		CHECK(terrainSamplesFor(OscType::Stair, shipped) == 1);
		CHECK(shipped > 34.5 * 0.5); // the cell really is past what stair can resolve
	}

	SUBCASE("the trigger is the measurement, the cap is the decision, and the decision says one") {
		// TWO THINGS OF DIFFERENT KINDS, and keeping them apart is the whole point of this case. WHERE a
		// cell stops being able to resolve what is inside it is a MEASUREMENT — half the finest feature,
		// 169 zones on morph and 34.5 on stair — and moving it would be a bug. HOW MUCH IS DONE ABOUT IT
		// is a TASTE, and it is a matter of taste.
		//
		// THE CHAIN, because it is a chain of taste and the record should read as one: seventeen samples
		// shipped, where the measured error stops falling → rejected, the settle was too slow to sit and
		// watch → two, a third of the benefit for one extra sweep → ONE. So it switched to one. It looked
		// at the smoothed picture and the rough one side by side and kept choosing the rough one.
		//
		// SO EVERY SPAN NOW TAKES ONE SAMPLE, and this case asserts exactly that — while still asserting
		// that the TRIGGER underneath it computes, because the measurement it encodes is true whatever
		// the taste is and raising the cap must not require rediscovering it.
		CHECK(terrainSamplesFor(OscType::Morph, 169.0 * 0.5 - 1.0) == 1);
		CHECK(terrainSamplesFor(OscType::Morph, 169.0 * 0.5 + 1.0) == 1);
		CHECK(terrainSamplesFor(OscType::Stair, 34.5 * 0.5 - 1.0) == 1);
		CHECK(terrainSamplesFor(OscType::Stair, 34.5 * 0.5 + 1.0) == 1);
		CHECK(terrainSamplesFor(OscType::Stair, 100000.0) == 1);
		CHECK(terrainSamplesFor(OscType::Morph, 100000.0) == 1);
		// Including the widest stop this surface has, which is the view the decision was made looking at.
		CHECK(terrainSamplesFor(OscType::Morph, ExploreStage::kWidestSpanZones / 128.0) == 1);
		CHECK(terrainSamplesFor(OscType::Stair, ExploreStage::kWidestSpanZones / 128.0) == 1);
		// Degenerate inputs answer rather than divide: a zero-span window is a window nobody drew.
		CHECK(terrainSamplesFor(OscType::Morph, 0.0) == 1);
		CHECK(terrainSamplesFor(OscType::Morph, -5.0) == 1);
		// WHAT THIS ONE STILL CATCHES, which is why it is a restatement rather than a deletion: the
		// schedule disobeying its own constant. It was a defect when a wide stop took one sample; it is
		// the decision now, and the case fires either way if the number and the behaviour part company.
	}

	SUBCASE("one sample is the centre, and a pair straddles the banding instead of riding it") {
		// A ONE-SAMPLE BUILD TAKES THE CENTRE — the property that keeps every shipped zoom
		// bit-identical, asserted on the function rather than inferred from the schedule.
		const TerrainSampleOffset alone = terrainSampleOffset(0, 1);
		CHECK(alone.home == doctest::Approx(0.0));
		CHECK(alone.travel == doctest::Approx(0.0));

		// AND A PAIR CROSSES THE GRAIN OF THE PICTURE. Phase is `home + x·travel`, so a place keeps its
		// character along a line where home rises with travel — the diagonal banding the wide-stop
		// photographs show. Two samples ALONG that diagonal would sit on one band and average nothing;
		// across it they straddle two. That direction is the whole reason a pair is worth twice the work.
		const TerrainSampleOffset a = terrainSampleOffset(0, 2);
		const TerrainSampleOffset b = terrainSampleOffset(1, 2);
		INFO("pair at (" << a.home << ", " << a.travel << ") and (" << b.home << ", " << b.travel
		                 << ")");
		CHECK(a.home == doctest::Approx(-b.home));     // symmetric about the centre, so the mean of
		CHECK(a.travel == doctest::Approx(-b.travel)); // the two is unbiased over the cell
		CHECK(a.home != doctest::Approx(0.0));         // and it is a pair, not the centre twice over

		// AND IT IS THE DIRECTION THAT ACTUALLY AVERAGES, asserted by MEASUREMENT rather than by the
		// geometry argument that got it backwards. The two samples of the shipped pair must land on
		// materially more different character than the mirrored pair does — that is what "across the
		// banding" means, and the first version of this pattern failed it while looking perfectly
		// reasonable in a comment (four percent quieter where a pair should give thirty).
		//
		// AND THAT CLAIM IS NOW RETIRED, BY MEASUREMENT, WHICH IS THE HONEST OUTCOME RATHER THAN A
		// WEAKENING. The pair's ORIENTATION was justified by a dominant grain: under the decode this was
		// first measured on, every triangle's iso-line lay at exactly −45°, so there was one band
		// direction everywhere and a sample pair genuinely wanted to cross it. M leaves no such band. The
		// structure tensor on the drawn field, at the two spans this surface actually uses, reads
		// coherence 0.004 at the widest stop and 0.014 at the band — against 0.98 for the six-angle
		// decode and 0.44 for the spread that shipped this morning. There is no grain left to straddle,
		// and the diagonal it was asked about in #29 is gone rather than reduced.
		//
		// AND THE REPLACEMENT I REACHED FOR FIRST DOES NOT HOLD EITHER, which is the more interesting
		// finding and the reason this is a retirement rather than a substitution. My second draft asserted
		// that the pair must SPREAD — that two samples separated across the cell see materially more of it
		// than two nearly on top of each other. Measured, at this span: a pair pulled in to a TENTH of its
		// separation still sees 5.28 against the full pair's 5.70, ninety-three percent of it. At the
		// widest stop the character decorrelates well inside a tenth of a texel, so no two-point pattern
		// is measuring a mean of anything here — which is exactly what #27's Nyquist rider said in other
		// words, and exactly why it was set to sample-once and left this machinery dormant.
		//
		// SO NO COMPARATIVE ASSERTION IS MADE, and the numbers are logged instead. What this case still
		// guards is the pattern's GEOMETRY — one sample is the centre, a pair is symmetric about it and
		// unbiased, and neither leaves its own texel — which are the claims that remain true and are
		// checked above and below. The direction-of-averaging claim is left here in its dead form on
		// purpose, with the measurements that killed it, so nobody re-derives it from the old comment and
		// then wonders why the field disagrees.
		SliceOrientationPin unpinned;
		const double cell = ExploreStage::kWidestSpanZones / 128.0 / 1023.0; // one texel, in home
		const auto apart = [&](double home, uint16_t zone, const TerrainSampleOffset& one,
		                       const TerrainSampleOffset& two) {
			const auto at = [&](const TerrainSampleOffset& o) {
				return terrainFeaturesAt(OscType::Morph, unpinned, zone, 0.0f, o.travel * cell, 0.0,
				                         kNoPlaneResidue,
				                         home + o.home * cell - static_cast<double>(zone) / 1023.0);
			};
			const TerrainFeatures p = at(one);
			const TerrainFeatures q = at(two);
			return std::abs(p.energy - q.energy) + std::abs(p.centroid - q.centroid)
			       + std::abs(p.roughness - q.roughness);
		};
		double shipped = 0.0;
		double mirrored = 0.0;
		int sites = 0;
		int shippedWins = 0;
		for (const double home : { 2.0, 6.0, 11.5, 19.25, 31.0 }) {
			for (const uint16_t zone : { uint16_t { 128 }, uint16_t { 512 }, uint16_t { 900 } }) {
				const double one = apart(home, zone, a, b);
				const double other = apart(home, zone, { a.home, -a.travel }, { b.home, -b.travel });
				shipped += one;
				mirrored += other;
				shippedWins += one > other ? 1 : 0;
				++sites;
			}
		}
		// The huddled pair — the same pattern pulled in to a tenth of its separation — measured and
		// reported rather than asserted on, for the reason above.
		double huddled = 0.0;
		for (const double home : { 2.0, 6.0, 11.5, 19.25, 31.0 }) {
			for (const uint16_t zone : { uint16_t { 128 }, uint16_t { 512 }, uint16_t { 900 } }) {
				huddled += apart(home, zone, { a.home * 0.1, a.travel * 0.1 },
				                 { b.home * 0.1, b.travel * 0.1 });
			}
		}
		INFO("across " << sites << " places the shipped pair differs by " << shipped
		               << " in total, the mirrored one by " << mirrored << " and a huddled pair by "
		               << huddled << "; the shipped pair is the wider one at " << shippedWins
		               << " of them");

		// Neither leaves its own texel: a sample that wandered into the neighbour would be averaging the
		// wrong cell and smearing the map by half a texel.
		for (const int count : { 1, 2 }) {
			for (int i = 0; i < count; ++i) {
				const TerrainSampleOffset at = terrainSampleOffset(i, count);
				INFO("count " << count << " sample " << i);
				CHECK(at.home >= -0.5);
				CHECK(at.home <= 0.5);
				CHECK(at.travel >= -0.5);
				CHECK(at.travel <= 0.5);
			}
		}
	}

	SUBCASE("the map is the point sample, everywhere, including where a cell cannot resolve") {
		// UNDER THE DECISION THIS IS THE CLAIM. The builder's averaging arithmetic is intact and dormant;
		// what ships is one sample in the middle of every texel at every span, which is what this checks
		// against an independently written reference. It replaces "the mean is the mean" — the same
		// assertion at the count the cap now allows — and it is the case that would go red if the
		// averaging path ever started running again without the constant being raised.
		const double span = ExploreStage::kWidestSpanZones / 1023.0;
		TerrainWindow wide = windowAt(6.0, span, span);
		wide.zoneBaseline = 512.0;
		TerrainMap map;
		map.request(wide);
		REQUIRE(settle(map, wide));
		const std::shared_ptr<const TerrainSnapshot> built = map.snapshot();
		REQUIRE(built->rows == TerrainMap::kResolution);
		CHECK(built->samples == 1);
		const auto zone = static_cast<uint16_t>(std::lround(wide.zoneBaseline));
		for (const int row : { 0, 40, 90, 127 }) {
			for (const int column : { 3, 64, 120 }) {
				// backlog GY: the row is travel-Y and the home is the window's, one for the whole map.
		const double travelY = TerrainMap::travelYAtRow(wide, row);
				const double travelX = TerrainMap::travelXAtColumn(wide, column);
				const TerrainFeatures one =
				    terrainFeaturesAt(OscType::Morph, wide.pin, zone, 0.0f, travelX, travelY,
				                      kNoPlaneResidue, wide.gammaBase);
				INFO("row " << row << " column " << column);
				CHECK(built->texel(column, row)
				      == terrainColour(one, travelX, travelY, -1, colourMapFor(OscType::Morph)));
			}
		}
	}

	SUBCASE("there is no refinement pass at all — the first map is the final one") {
		// THE SETTLE IS GONE, which is the visible half of the decision and the thing to verify rather than
		// assume: at a cap of one there is no second pass to run, so the map that arrives is the map, and
		// the publish path does no extra work after it. The settle's visible crawl was rejected; this
		// is the assertion that it cannot come back quietly.
		const double span = ExploreStage::kWidestSpanZones / 1023.0;
		TerrainWindow wide = windowAt(6.0, span, span);
		wide.zoneBaseline = 512.0;
		TerrainMap map;
		map.request(wide);
		REQUIRE(settle(map, wide));
		const TerrainSnapshot first = *map.snapshot();
		CHECK(first.samples == 1);
		const uint64_t builtOnce = map.builds();

		// Nothing further arrives, and nothing further is built: watched for long enough that a
		// seventeen-sample refinement (which took 600ms when it existed) could not have hidden in it.
		std::this_thread::sleep_for(std::chrono::milliseconds(800));
		const TerrainSnapshot later = *map.snapshot();
		CHECK(map.builds() == builtOnce);
		CHECK(later.samples == 1);
		size_t same = 0;
		for (size_t i = 0; i < first.texels.size(); ++i) {
			same += first.texels[i] == later.texels[i] ? 1 : 0;
		}
		INFO(same << " of " << first.texels.size() << " texels unchanged after 800ms at rest");
		CHECK(same == first.texels.size());
	}

	SUBCASE("two builds of one window are byte-identical, because nothing here is random") {
		// The pattern is stateless and deterministic on purpose: a random one would trade a visible
		// artefact for an invisible one, and byte-identical rebuilds are load-bearing in this suite —
		// the gauge case, the carry cases and A35 all rest on them.
		const double span = 12288.0 / 1023.0;
		TerrainWindow wide = windowAt(6.0, span, span);
		wide.zoneBaseline = 512.0;
		TerrainMap first;
		first.request(wide);
		REQUIRE(settleRefined(first, wide));
		const TerrainSnapshot a = *first.snapshot();
		TerrainMap second;
		second.request(wide);
		REQUIRE(settleRefined(second, wide));
		const TerrainSnapshot b = *second.snapshot();
		REQUIRE(a.rows == TerrainMap::kResolution);
		REQUIRE(b.rows == TerrainMap::kResolution);
		size_t same = 0;
		for (size_t i = 0; i < a.texels.size(); ++i) {
			same += a.texels[i] == b.texels[i] ? 1 : 0;
		}
		CHECK(same == a.texels.size());
	}
}

TEST_CASE("gamma moves the whole plane, so it changes every texel and the map IS rebuilt for it") {
	// THE THIRD LIFE OF THIS GUARD, and each life had a different reason — worth recording, because the
	// subject ("what does gamma do to the map") has outlived two answers.
	//
	// (1) It began as the CLAMP's obligation: rows outside the band were painted as nothing, carrying
	//     arrived holding the previous map, so a row that had just moved out of the band would keep the
	//     colour it had and the chart would end in terrain instead of at a boundary. The band moves with
	//     gamma, so gamma was what this case moved. (A first draft held gamma still and was VACUOUS —
	//     with the band still, "outside the band" is the same positions in both windows and there is
	//     nothing to carry there.)
	// (2) the world was made infinite and the clamp went: past the band the row's overflow went into
	//     that row's own base, so the obligation became "carrying must never leave a texel that
	//     disagrees with a from-scratch build".
	// (3) the decision then was ONE VALUE: a scrub moves the unbounded base and the two dials stay put, so
	//     every row is built at the sound's own zone and offset with the rest in that row's base — an
	//     arithmetic that does not mention gamma at all. So the map is now a function of the view and
	//     the dials, and GAMMA CANNOT CHANGE A SINGLE TEXEL.
	//
	// THAT IS WHY THIS CASE EXISTS IN THIS FORM: `TerrainWindow::operator==` is written out by hand to
	// leave gamma out of the content key, which inverts the lesson that a defaulted comparison is the
	// safe shape. The exclusion is only true while the builder ignores gamma, and a comment cannot
	// enforce that. This can. If anyone teaches the builder to read gammaSum or gammaBase again, the
	// first half of this fails; if the key silently starts including them again, the second half does.
	const double span = 64.0 / 1023.0;

	// THE SAME VIEW OF THE SAME SOUND, at two gammas a hand could never bridge in one gesture. Built in
	// two separate maps because one map would (correctly) refuse to rebuild for the second request,
	// which is the very thing the second half of this case is about.
	TerrainWindow here = windowAt(1.4, span, 2.0);
	here.zoneBaseline = 384.0;
	here.offsetBaseline = 0.25;
	here.gammaBase = 0.0;
	TerrainWindow faraway = here;
	faraway.gammaBase = 137.5;

	TerrainMap first;
	first.request(here);
	REQUIRE(settle(first, here));
	TerrainMap second;
	second.request(faraway);
	REQUIRE(settle(second, faraway));
	const std::shared_ptr<const TerrainSnapshot> a = first.snapshot();
	const std::shared_ptr<const TerrainSnapshot> b = second.snapshot();
	REQUIRE(a->rows == TerrainMap::kResolution);
	REQUIRE(b->rows == TerrainMap::kResolution);

	// **THE FOURTH LIFE, AND IT IS THE INVERSE OF THE THIRD** (backlog GY,). The case
	// above predicted its own end: *"If anyone teaches the builder to read gammaSum or gammaBase
	// again, the first half of this fails; if the key silently starts including them again, the second
	// half does." Both halves fired, and not by accident — the design is that the chart is a window
	// on the 2-space `rotate` decodes, so gamma stopped being the vertical axis and became the one
	// place the whole plane is taken at. Move it and every texel is somewhere else.
	//
	// THE GUARD IS KEPT AND POINTED THE OTHER WAY. Deleting it would retire the only thing watching
	// the agreement between what the builder reads and what the key carries — an agreement that still
	// needs watching, because under-keying it is the failure that leaves the wrong map on screen and
	// makes the Gamma corner look dead.
	size_t compared = 0;
	size_t differing = 0;
	for (int row = 0; row < TerrainMap::kResolution; ++row) {
		for (int column = 0; column < TerrainMap::kResolution; ++column) {
			if (a->texel(column, row) != b->texel(column, row)) {
				++differing;
			}
			++compared;
		}
	}
	// NOT "SOME MOVED" — nearly all of them. A handful could coincide by luck at 8-bit depth; a map
	// that mostly agreed would mean gamma was reaching the decode and barely doing anything there,
	// which is the half-wired state this refactor exists to end.
	INFO("texels that changed across a gamma of 137.5: " << differing << " of " << compared);
	CHECK(differing > compared * 9 / 10);
	// And the map is real, so this is not two blank sheets agreeing.
	REQUIRE(a->texel(64, 64) != 0u);

	// AND A GAMMA THE PICTURE DEPENDS ON DOES ASK FOR A REBUILD — the other half, inverted with the
	// first. The old worry was a flicker manufactured out of nothing: with gamma in the key, every
	// frame of a VERTICAL DRAG would have asked for a map identical to the one on screen. That worry
	// is retired by the same decision that inverted the first half — a vertical drag no longer writes
	// gamma at all, it writes travel-Y, and the only thing that moves gamma now is the Gamma corner.
	// So keying it costs nothing per gesture and buys a map that is actually of where the sound is.
	const uint64_t builtSoFar = first.builds();
	TerrainWindow nudged = here;
	nudged.gammaBase = 6.25;
	first.request(nudged);
	REQUIRE(settle(first, nudged));
	INFO("builds across a gamma nudge: " << (first.builds() - builtSoFar));
	CHECK(first.builds() > builtSoFar);
	CHECK(first.snapshot()->rows == TerrainMap::kResolution);

	// While a change the picture DOES depend on still asks for one, or the line above would be passing
	// because requests stopped working.
	TerrainWindow moved = here;
	moved.zoneBaseline = 900.0;
	first.request(moved);
	REQUIRE(settle(first, moved));
	CHECK(first.builds() > builtSoFar);
	MESSAGE("texels compared across a gamma of 137.5: " << compared);
}

TEST_CASE("the map is neither a white sheet nor a black one") {
	// THE CALIBRATION'S OWN CONTROL, and it caught the first version: with the gains guessed rather
	// than measured, luminance saturated everywhere and the terrain photographed as a pale wash with a
	// texel grid in it. Every channel's MEDIAN has to sit inside its travel, or the map has resolution
	// on paper and none on screen. The bounds are loose — this guards against a channel collapsing,
	// not against a taste.
	SliceOrientationPin pin;
	// AND RESONATE (2026-09-26, whose explore topography read dark and dull): its channels read its features
	// quantile-matched onto the shared world (channelFeaturesFor), so its medians sit inside the same bars.
	for (const OscType engine : { OscType::Morph, OscType::Stair, OscType::Resonate }) {
		// THE CHANNELS THE MAP ACTUALLY DRAWS, through terrainChannels — not a second copy of the gains
		// written out here. The first version of this case did write them out, and a known-bad build that
		// put the ember gain back to the value which had ALREADY drawn a broken map stayed green: the
		// fixture was asserting agreement with itself.
		std::vector<TerrainChannels> channels;
		for (int zone = 0; zone < 1024; zone += 7) {
			for (int step = -8; step <= 8; ++step) {
				channels.push_back(terrainChannels(
				    terrainFeaturesAt(engine, pin, static_cast<uint16_t>(zone), 0.0f, step * 0.005, 0.0), engine));
			}
		}
		const auto at = [&](float TerrainChannels::*field, double p) {
			std::vector<float> v;
			v.reserve(channels.size());
			for (const TerrainChannels& c : channels) {
				v.push_back(c.*field);
			}
			std::sort(v.begin(), v.end());
			return v[static_cast<size_t>(p * (v.size() - 1))];
		};
		CHECK(at(&TerrainChannels::luminance, 0.5) > 0.15f);
		CHECK(at(&TerrainChannels::luminance, 0.5) < 0.85f);
		CHECK(at(&TerrainChannels::hue, 0.5) > 0.15f);
		CHECK(at(&TerrainChannels::hue, 0.5) < 0.85f);
		CHECK(at(&TerrainChannels::grain, 0.5) > 0.10f);
		CHECK(at(&TerrainChannels::grain, 0.5) < 0.90f);

		// AND THE RIDGE IS A HIGHLIGHT, NOT THE GROUND. This one a photograph caught rather than a
		// number: the ember gain had been set against the RAW roughness, kRoughGain then scaled roughness
		// by 2.6 and the ember squares it, so the term ran about twenty times deep and PhiStair — whose
		// roughness runs higher — drew as an almost entirely warm map. Warm is the MODULATOR land's
		// colour in this design language; sound land is cool with gold ridges.
		CHECK(at(&TerrainChannels::ember, 0.5) < 0.15f);
		CHECK(at(&TerrainChannels::ember, 0.95) < 0.60f);
	}
}

TEST_CASE("a value box takes its drag speed from the digit the press landed on") {
	// **BACKLOG BJ.** The coarse:fine pair is collapsed into ONE box by design, and the speed of a mouse
	// edit depends on where the box is grabbed. Grab the hundreds column
	// and a drag moves hundreds; grab the units and it moves units.
	//
	// **THIS IS WHAT REPLACES A PAIR OF CONTROLS**, so the case asserts the PLACE — the number the
	// drag is scaled by — rather than a resulting value. A value folds the place together with the
	// travel and the range, and would pass for the wrong place at a compensating distance.
	namespace dtt = feathers::fx::owltt;
	ModFixture f;
	const size_t slot = f.bridge.addFxDevice(dtt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);

	// AN UNBOUNDED COORDINATE, which is what the collapse leaves behind — a declared range that is a
	// window, never a distance a hand crosses.
	ui::DeviceControl control { f.deviceHost, f.bridge.fxDeviceAt(slot), dtt::kDescriptor,
		                        static_cast<size_t>(dtt::Param::Character), nullptr, nullptr };
	ui::ValueBox box { control, "" };
	box.setCompact(true);
	// **A FRACTIONAL READOUT, AND THAT CHOICE IS THE CASE'S TEETH.** An all-digit readout cannot see
	// the two mistakes this function exists to avoid: with no decimal point, counting places from the
	// END of the string is arithmetically identical to counting from the point, and with only tabular
	// digits, dividing the text into equal columns lands on the same glyphs as measuring each advance.
	// MEASURED — both mutations ran green against a "1234" fixture. A point and a fraction make both
	// of them wrong, so the readout carries them.
	box.setReadoutFormatter([](double v) {
		char out[32] = {};
		std::snprintf(out, sizeof(out), "%.2f", v);
		return std::string(out);
	});
	box.setBounds(0, 0, 120, 20);

	control.setValue(1234.0);
	REQUIRE(box.readout() == "1234.00");

	const ui::ValueBox::ReadoutBox rb = box.readoutBox();
	const std::u32string wide = U"1234.00";
	const float total = ui::font(ui::kKnobReadoutSize).stringWidth(wide);
	const float left = rb.x + (rb.width - total) * 0.5f;

	// **EACH COLUMN, BY VALUE, MEASURED AT ITS OWN ADVANCE** rather than at an assumed equal width —
	// the same prefix-difference the widget uses, so the case cannot pass by a coincidence of a
	// monospaced font.
	const auto centreOfGlyph = [&](size_t index) {
		const float before = index == 0 ? 0.0f
		                                : ui::font(ui::kKnobReadoutSize)
		                                      .stringWidth(wide.c_str(), static_cast<int>(index));
		const float upTo =
		    ui::font(ui::kKnobReadoutSize).stringWidth(wide.c_str(), static_cast<int>(index + 1));
		return left + (before + upTo) * 0.5f;
	};

	// Six columns must give six DIFFERENT places, in order — a mapping that returned one place
	// everywhere would satisfy any single-column assertion.
	CHECK(box.placeAt(centreOfGlyph(0)) == doctest::Approx(1000.0));
	CHECK(box.placeAt(centreOfGlyph(1)) == doctest::Approx(100.0));
	CHECK(box.placeAt(centreOfGlyph(2)) == doctest::Approx(10.0));
	CHECK(box.placeAt(centreOfGlyph(3)) == doctest::Approx(1.0));

	// **THE PLACE IS MEASURED FROM THE POINT, NOT FROM THE END OF THE STRING.** The digit before the
	// point is the units column whatever follows it; counting trailing characters would call it the
	// thousands here, which is the whole reason the function looks for a point at all.
	CHECK(box.placeAt(centreOfGlyph(5)) == doctest::Approx(0.1));
	CHECK(box.placeAt(centreOfGlyph(6)) == doctest::Approx(0.01));

	// **THE POINT ITSELF IS NOT A PLACE.** It is also the narrowest glyph in the readout, so this is
	// the assertion that holds the hit test to the font's real advances: split into equal columns,
	// this x falls on the tenths digit instead and the case reads 0.1 for a press on punctuation.
	CHECK(box.placeAt(centreOfGlyph(4)) == 0.0);

	// **OFF THE NUMBER IS A REFUSAL, NOT A PLACE.** The caller falls back to the declared span;
	// answering for a press on empty padding would make the granularity depend on where the text
	// happened to be centred.
	CHECK(box.placeAt(left - 10.0f) == 0.0);
	CHECK(box.placeAt(left + total + 10.0f) == 0.0);

	// **AND THE COLUMN BOUNDARIES ARE WHERE THE GLYPHS ARE.** The centres above are satisfied by any
	// hit test roughly the right shape; the EDGES are what hold it to the font's own advances.
	// MEASURED in this font at `kKnobReadoutSize`: a digit advances 5px and the point 2, so "1234.00"
	// is 32px with the point spanning 20..22 — while seven equal columns of 4.571px put 19.5 on the
	// point and 22.5 on it as well. Sampling one pixel inside each glyph is therefore the only
	// sampling that can tell the two apart, and a hit test that disagrees with the drawing by half a
	// glyph is a box whose granularity is not the digit the eye aimed at.
	const auto insideGlyph = [&](size_t index, float fromLeftEdge) {
		const float before = index == 0 ? 0.0f
		                                : ui::font(ui::kKnobReadoutSize)
		                                      .stringWidth(wide.c_str(), static_cast<int>(index));
		return left + before + fromLeftEdge;
	};
	CHECK(box.placeAt(insideGlyph(0, 0.5f)) == doctest::Approx(1000.0));
	CHECK(box.placeAt(insideGlyph(3, 4.5f)) == doctest::Approx(1.0));  // last pixel before the point
	CHECK(box.placeAt(insideGlyph(5, 0.5f)) == doctest::Approx(0.1));  // first pixel after it
	CHECK(box.placeAt(insideGlyph(6, 4.5f)) == doctest::Approx(0.01)); // last pixel of the readout
}

TEST_CASE("the column a value-box drag grabbed is the column it moves") {
	// **THE OTHER HALF OF BJ's FIRST PIECE.** The case above asserts the MAPPING from an x to a place;
	// this one asserts the place is what the drag is scaled by. Both are needed and neither implies
	// the other: `placeAt` could be perfect while `dragTo` ignored it, which is a road with a correct
	// signpost and no traffic — the shape `mod_route_menu.h` and the arm-glow bug were both about.
	//
	// **AND IT ASSERTS THE RATIO, NOT ONLY THE TWO VALUES.** 1000:1 between the thousands column and
	// the units is the claim; either value alone is satisfied by a compensating error in the travel,
	// the range, or `kDragRange`.
	namespace dtt = feathers::fx::owltt;
	ModFixture f;
	const size_t slot = f.bridge.addFxDevice(dtt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);

	ui::DeviceControl control { f.deviceHost, f.bridge.fxDeviceAt(slot), dtt::kDescriptor,
		                        static_cast<size_t>(dtt::Param::Character), nullptr, nullptr };
	ui::ValueBox box { control, "" };
	box.setCompact(true);
	box.setReadoutFormatter([](double v) {
		char out[32] = {};
		std::snprintf(out, sizeof(out), "%.2f", v);
		return std::string(out);
	});
	box.setBounds(0, 0, 120, 20);

	const std::u32string wide = U"1234.00";
	const ui::ValueBox::ReadoutBox rb = box.readoutBox();
	const float total = ui::font(ui::kKnobReadoutSize).stringWidth(wide);
	const float left = rb.x + (rb.width - total) * 0.5f;
	const auto centreOfGlyph = [&](size_t index) {
		const float before = index == 0 ? 0.0f
		                                : ui::font(ui::kKnobReadoutSize)
		                                      .stringWidth(wide.c_str(), static_cast<int>(index));
		const float upTo =
		    ui::font(ui::kKnobReadoutSize).stringWidth(wide.c_str(), static_cast<int>(index + 1));
		return left + (before + upTo) * 0.5f;
	};

	// A FULL DRAG UPWARD, which is the unit the reach is declared in: `kUnitsPerPlaceDrag` of the
	// grabbed digit's own place per `kDragRange` of travel.
	const auto dragFrom = [&](size_t column) {
		control.setValue(1234.0);
		REQUIRE(box.readout() == "1234.00");
		const float x = centreOfGlyph(column);
		box.processMouseDown(at(x, 10.0f));
		box.processMouseDrag(at(x, 10.0f - ui::ValueBox::kDragRange));
		const double moved = control.value() - 1234.0;
		box.processMouseUp(at(x, 10.0f - ui::ValueBox::kDragRange));
		return moved;
	};

	const double thousands = dragFrom(0);
	const double units = dragFrom(3);

	CHECK(thousands == doctest::Approx(10000.0));
	CHECK(units == doctest::Approx(10.0));
	CHECK(thousands / units == doctest::Approx(1000.0));

	// **AND A PRESS THAT IS NOT ON A DIGIT FALLS BACK RATHER THAN FREEZING.** Zero reach would make a
	// drag started on the decimal point move nothing at all, which reads as a dead control; the box
	// keeps its declared behaviour there instead.
	control.setValue(1234.0);
	const float onPoint = centreOfGlyph(4);
	REQUIRE(box.placeAt(onPoint) == 0.0);
	box.processMouseDown(at(onPoint, 10.0f));
	box.processMouseDrag(at(onPoint, 10.0f - ui::ValueBox::kDragRange));
	const double fallback = control.value() - 1234.0;
	box.processMouseUp(at(onPoint, 10.0f - ui::ValueBox::kDragRange));
	CHECK(fallback != 0.0);
	CHECK(fallback != doctest::Approx(10000.0));
	CHECK(fallback != doctest::Approx(10.0));
}

TEST_CASE("hardening: a choice menu's pick runs only if its box is still in the window") {
	// THE SITE CAPTURES `this`: a card rebuilt (a state or preset load) while its menu is open leaves a pick that
	// would run on a dead box. The value is the observable, not a crash: with the box merely DETACHED (still alive)
	// the pick would write the control, so the unguarded road is red without a sanitiser; the destroyed box is the
	// same road, which a sanitiser run proves is not a use after free.
	namespace dtt = feathers::fx::owltt;
	ModFixture f;
	const size_t slot = f.bridge.addFxDevice(dtt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	ui::DeviceControl mode { f.deviceHost, f.bridge.fxDeviceAt(slot), dtt::kDescriptor,
		                      static_cast<size_t>(dtt::Param::Mode), nullptr, nullptr };
	const int row = 6; // "LR2 Full", as the readout case above names it
	const std::string label = ui::markedChoiceLabel("LR2 Full", false);

	visage::Frame root;
	root.setBounds(0.0f, 0.0f, 400.0f, 300.0f);
	auto box = std::make_unique<ui::ValueBox>(mode, "");
	box->setBounds(20.0f, 20.0f, 120.0f, 20.0f);
	root.addChild(box.get());
	mode.setValue(0.0);
	const double start = mode.value(); // the control's own floor, whatever it is
	REQUIRE(start != static_cast<double>(row));
	REQUIRE(box->showChoiceMenu());
	visage::PopupMenuFrame* popup = menu_check::newestPopup(*box);
	REQUIRE(popup != nullptr);

	SUBCASE("the box in the window: the pick writes the control (the road works, so the others mean something)") {
		REQUIRE(menu_check::pickRow(*popup, label));
		CHECK(mode.value() == static_cast<double>(row));
		root.removeChild(box.get());
	}

	SUBCASE("the box left the window but is not destroyed: the pick runs nothing") {
		root.removeChild(box.get());
		REQUIRE(menu_check::pickRow(*popup, label));
		CHECK(mode.value() == start);
	}

	SUBCASE("the box destroyed: the pick runs nothing") {
		root.removeChild(box.get());
		box.reset();
		REQUIRE(menu_check::pickRow(*popup, label));
		CHECK(mode.value() == start);
	}
}

TEST_CASE("digit dragging declines where the readout is not the value") {
	// **THE HONESTY GATE — see `ValueBox::readoutTracksValue`.** Most readouts in this editor are a
	// translation, not a rendering: a Percent box prints 42 for 0.42, a Choice box prints a word, a
	// Fader prints "-inf" for zero. A digit column in any of those names nothing the drag can move
	// by, and taking it at face value would move a 0..1 control by ten.
	//
	// **THE CASE THIS EXISTS FOR IS THE ORIGIN, and it is why one sample is not enough.** A Percent
	// box sitting at 0 prints "0%", which agrees with its value exactly — by coincidence, because
	// every scaled readout agrees at zero. Only the second point separates them.
	namespace dtt = feathers::fx::owltt;
	ModFixture f;
	const size_t slot = f.bridge.addFxDevice(dtt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);

	const auto centreOf = [](const ui::ValueBox& b, const std::u32string& wide, size_t index) {
		const ui::ValueBox::ReadoutBox rb = b.readoutBox();
		const float total = ui::font(ui::kKnobReadoutSize).stringWidth(wide);
		const float left = rb.x + (rb.width - total) * 0.5f;
		const float before = index == 0 ? 0.0f
		                                : ui::font(ui::kKnobReadoutSize)
		                                      .stringWidth(wide.c_str(), static_cast<int>(index));
		const float upTo =
		    ui::font(ui::kKnobReadoutSize).stringWidth(wide.c_str(), static_cast<int>(index + 1));
		return left + (before + upTo) * 0.5f;
	};

	SUBCASE("a percentage prints a hundred times its value, so no column is a place") {
		ui::DeviceControl pct { f.deviceHost, f.bridge.fxDeviceAt(slot), dtt::kDescriptor,
			                     static_cast<size_t>(dtt::Param::Threshold), nullptr, nullptr };
		ui::ValueBox box { pct, "" };
		box.setCompact(true);
		box.setBounds(0, 0, 120, 20);

		pct.setValue(0.42);
		REQUIRE(box.readout() == "42%");
		CHECK(box.placeAt(centreOf(box, U"42%", 0)) == 0.0);
		CHECK(box.placeAt(centreOf(box, U"42%", 1)) == 0.0);

		// **AT THE ORIGIN THE OFFSET AGREES AND THE SLOPE DOES NOT.** "0%" parses to 0 and the value
		// IS 0; a one-sample check would hand this box the units column and a drag would then run its
		// entire 0..1 range ten times over.
		pct.setValue(0.0);
		REQUIRE(box.readout() == "0%");
		CHECK(box.placeAt(centreOf(box, U"0%", 0)) == 0.0);
		// The gate is what refuses, so state it directly too — the hit test found a digit here.
		CHECK_FALSE(box.readoutTracksValue(1.0, 1.0));
	}

	SUBCASE("a choice prints a name, and a name has no columns at all") {
		ui::DeviceControl mode { f.deviceHost, f.bridge.fxDeviceAt(slot), dtt::kDescriptor,
			                      static_cast<size_t>(dtt::Param::Mode), nullptr, nullptr };
		ui::ValueBox box { mode, "" };
		box.setCompact(true);
		box.setBounds(0, 0, 120, 20);
		mode.setValue(6.0);
		REQUIRE(box.readout() == "LR2 Full");
		// No digit under any x, so this is the hit test refusing rather than the gate — assert the
		// gate separately, since a readout that parsed nothing must not read as the number zero.
		CHECK(box.placeAt(centreOf(box, U"LR2 Full", 4)) == 0.0);
		CHECK_FALSE(box.readoutTracksValue(1.0, 1.0));
	}

	SUBCASE("and a box that DOES render its own number keeps the road") {
		// THE CONTROL, and it is what makes the two refusals above mean something. Without it a gate
		// that returned false unconditionally would satisfy every assertion in this case.
		ui::DeviceControl phase { f.deviceHost, f.bridge.fxDeviceAt(slot), dtt::kDescriptor,
			                       static_cast<size_t>(dtt::Param::Character), nullptr, nullptr };
		ui::ValueBox box { phase, "" };
		box.setCompact(true);
		box.setBounds(0, 0, 120, 20);
		phase.setValue(1234.0);
		REQUIRE(box.readout() == "1234.00");
		CHECK(box.readoutTracksValue(1.0, 0.01));
		CHECK(box.placeAt(centreOf(box, U"1234.00", 0)) == doctest::Approx(1000.0));
	}
}

// ============================================================================================== //
// THE ZONE SELECTOR — backlog BJ, piece 2 of 4                                                   //
// ============================================================================================== //

namespace {

/// EIGHT REGIONS WITH TWO OF THEM DELIBERATELY BLANK, which is OWLTT's real declaration rather than a
/// tidy invention: its character zones 1 and 2 carry `{"", ""}` because there is *"no case in the
/// DSP's switch" there. A fixture that named all eight would have made `captionOf`'s fallback
/// untestable, and the fallback is the half a user sees when they step into one.
std::vector<ui::ZoneSelect::ZoneName> owlttCharacterZones() {
	std::vector<ui::ZoneSelect::ZoneName> out;
	for (const feathers::fx::owltt::ZoneName& z : feathers::fx::owltt::kCharacterZoneNames) {
		out.push_back({ z.name, z.description });
	}
	return out;
}

} // namespace

TEST_CASE("the zone selector wraps the named regions instead of stopping at the last one") {
	// **THE COLLAPSED COORDINATE IS UNBOUNDED, AND THAT IS WHY THE OLD DERIVATION CANNOT SERVE IT.**
	// `computeZone` in `device/zone_param.h` CLAMPS its input to 0..1 — correct while it read a
	// bounded half whose overflow carried into a separate phase, and wrong the moment one control
	// holds the whole line: every value past the first lap would report the last zone forever, and
	// every value below zero would report the first.
	namespace dtt = feathers::fx::owltt;
	ModFixture f;
	const size_t slot = f.bridge.addFxDevice(dtt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);

	ui::DeviceControl coord { f.deviceHost, f.bridge.fxDeviceAt(slot), dtt::kDescriptor,
		                       static_cast<size_t>(dtt::Param::Character), nullptr, nullptr };
	ui::ZoneSelect select { coord, owlttCharacterZones() };
	REQUIRE(select.zoneCount() == 8);
	// ONE LAP OVER EIGHT REGIONS. Derived from the two numbers rather than declared as a third, so a
	// device that changes its zone count cannot leave a stale width behind.
	REQUIRE(select.perZone() == doctest::Approx(0.125));

	struct Probe {
		double value;
		size_t zone;
		long lap;
	};
	// THE FIRST LAP, THEN PAST IT, THEN BELOW ZERO — the three regions of the line, and only the
	// first is one the clamping derivation gets right.
	const Probe probes[] = {
		{ 0.0, 0, 0 },      // the origin
		{ 0.5, 4, 0 },      // mid-lap
		{ 0.875, 7, 0 },    // the last named region of lap 0
		{ 1.0, 0, 1 },      // **one step past it: the FIRST region again, one lap up**
		{ 3.3125, 2, 3 },   // far out — 26.5 zones from the origin
		{ -0.125, 7, -1 },  // exactly one zone below the origin
		{ -0.0625, 7, -1 }, // **half a zone below it, which FLOOR puts in 7 and truncation puts in 0**
	};
	for (const Probe& p : probes) {
		CAPTURE(p.value);
		coord.setValue(p.value);
		CHECK(select.zoneIndex() == p.zone);
		CHECK(select.lap() == p.lap);
	}

	// **AND THE CAPTION FOLLOWS THE WHOLE VALUE.** The live bug BJ recorded was a name derived from
	// half of it; here there is only one half.
	coord.setValue(1.0);
	CHECK(select.zoneName() == "Width");
	coord.setValue(0.875);
	CHECK(select.zoneName() == "OWLTT"); // zone 7, the last of the set
	coord.setValue(0.375);
	CHECK(select.zoneName() == "Punch"); // zone 3, the first named one after the two blanks
}

TEST_CASE("prev and next cross a lap; picking a zone from the list does not") {
	// **THE TWO GESTURES ARE DELIBERATELY DIFFERENT AND THIS IS THE CASE THAT SAYS SO.** A step is
	// one region in the direction you asked, and crossing the end of the named set is exactly the
	// firmware's *"just keep turning past the 8th zone which keeps driving the phase". A pick is a
	// jump WITHIN the current lap, because the lap is a separate quantity the box beside this widget
	// displays — a pick that also moved it would be two edits from one gesture.
	namespace dtt = feathers::fx::owltt;
	ModFixture f;
	const size_t slot = f.bridge.addFxDevice(dtt::kTraits.tag);
	ui::DeviceControl coord { f.deviceHost, f.bridge.fxDeviceAt(slot), dtt::kDescriptor,
		                       static_cast<size_t>(dtt::Param::Character), nullptr, nullptr };
	ui::ZoneSelect select { coord, owlttCharacterZones() };

	// STANDING IN THE LAST REGION OF LAP 0, a fifth of the way into it.
	coord.setValue(0.9);
	REQUIRE(select.zoneIndex() == 7);
	REQUIRE(select.lap() == 0);

	select.stepZone(1);
	CHECK(select.zoneIndex() == 0);
	CHECK(select.lap() == 1); // **the step carried**
	CHECK(coord.value() == doctest::Approx(1.025));

	select.stepZone(-1);
	CHECK(select.zoneIndex() == 7);
	CHECK(select.lap() == 0); // and back down again
	CHECK(coord.value() == doctest::Approx(0.9));

	// THE PICK, FROM THE SAME PLACE, TO THE SAME NAME THE STEP REACHED — and it lands one lap lower.
	// That is the whole distinction: same destination name, different place on the line.
	select.selectZone(0);
	CHECK(select.zoneIndex() == 0);
	CHECK(select.lap() == 0); // **the pick did NOT carry**
	CHECK(coord.value() == doctest::Approx(0.025));

	// A PICK OF WHERE WE ALREADY ARE MOVES NOTHING, rather than re-seating the value on the zone
	// boundary and quietly discarding the position inside it.
	const double before = coord.value();
	select.selectZone(0);
	CHECK(coord.value() == before);

	// AND AN INDEX OFF THE END IS REFUSED, not clamped onto the last region — a menu can only offer
	// what exists, but `selectZone` is public and a case is cheaper than an out-of-range read.
	select.selectZone(99);
	CHECK(coord.value() == before);
}

TEST_CASE("a zone change keeps where you were standing inside the zone") {
	// **THE PART THAT MAKES THE DROPDOWN USABLE RATHER THAN DESTRUCTIVE.** Somebody two-thirds of the
	// way into "Punch" who picks "Air" wants two-thirds of the way into "Air", not the boundary. It
	// falls out of moving by whole multiples of one zone — which is worth an assertion precisely
	// BECAUSE it is implicit: an implementation that computed the destination from the zone's start
	// would satisfy every index assertion in this file and silently throw the offset away.
	namespace dtt = feathers::fx::owltt;
	ModFixture f;
	const size_t slot = f.bridge.addFxDevice(dtt::kTraits.tag);
	ui::DeviceControl coord { f.deviceHost, f.bridge.fxDeviceAt(slot), dtt::kDescriptor,
		                       static_cast<size_t>(dtt::Param::Character), nullptr, nullptr };
	ui::ZoneSelect select { coord, owlttCharacterZones() };

	coord.setValue(0.9); // zone 7, one fifth in
	REQUIRE(select.positionInZone() == doctest::Approx(0.2));

	select.selectZone(2);
	CHECK(select.zoneIndex() == 2);
	CHECK(select.positionInZone() == doctest::Approx(0.2));
	// THE VALUE ITSELF, computed independently: zone 2 starts at 0.25 and a fifth of 0.125 is 0.025.
	CHECK(coord.value() == doctest::Approx(0.275));

	select.stepZone(3);
	CHECK(select.zoneIndex() == 5);
	CHECK(select.positionInZone() == doctest::Approx(0.2));
}

TEST_CASE("every x across a zone selector answers exactly one verb") {
	// **THE GUARD `FxCard`'s HEAD ALREADY CARRIES, on a widget with three targets in one row.** Two
	// verbs sharing a pixel is a control that does the wrong thing occasionally rather than never,
	// which is the hardest kind to be told about.
	namespace dtt = feathers::fx::owltt;
	ModFixture f;
	const size_t slot = f.bridge.addFxDevice(dtt::kTraits.tag);
	ui::DeviceControl coord { f.deviceHost, f.bridge.fxDeviceAt(slot), dtt::kDescriptor,
		                       static_cast<size_t>(dtt::Param::Character), nullptr, nullptr };
	ui::ZoneSelect select { coord, owlttCharacterZones() };
	select.setBounds(0, 0, 90, 18);

	int prev = 0;
	int next = 0;
	int menu = 0;
	for (int x = 0; x < 90; ++x) {
		const visage::Point at { static_cast<float>(x) + 0.5f, 9.0f };
		int hits = 0;
		hits += select.prevBounds().contains(at) ? 1 : 0;
		hits += select.nextBounds().contains(at) ? 1 : 0;
		hits += select.captionBounds().contains(at) ? 1 : 0;
		CAPTURE(x);
		CHECK(hits == 1); // covered, and covered once
		switch (select.verbAt(at)) {
		case ui::ZoneSelect::Verb::Prev: prev++; break;
		case ui::ZoneSelect::Verb::Next: next++; break;
		case ui::ZoneSelect::Verb::Menu: menu++; break;
		case ui::ZoneSelect::Verb::None: break;
		}
	}
	// **ALL THREE ARE REACHABLE, which is what stops the sweep above passing on a degenerate layout.**
	// One cell swallowing the row satisfies "exactly one" at every x just as well as three do.
	CHECK(prev == 14);
	CHECK(next == 14);
	CHECK(menu == 62);
	CHECK(prev + next + menu == 90);

	// AND THE PRESS ROAD AGREES WITH THE HIT TEST, rather than being a second reading of the same
	// geometry — a press on the right arrow steps forward.
	coord.setValue(0.0);
	CHECK(select.pressAt({ 85.0f, 9.0f }));
	CHECK(select.zoneIndex() == 1);
	CHECK(select.pressAt({ 5.0f, 9.0f }));
	CHECK(select.zoneIndex() == 0);
}

TEST_CASE("an unnamed region is captioned by its number rather than left blank") {
	// A REGION WITH NOTHING TO BE CALLED IS A REAL STATE, not missing data. `ZoneCoordBox::regionText`
	// settled what to draw there and the rule carries: a caption that vanished would read as a broken
	// widget, and it would read IDENTICALLY in two different regions, which is the ambiguity that
	// widget's own comment warns about.
	//
	// THE TABLE IS HAND-MADE. OWLTT's character zones 1 and 2 were the standing example of blanks
	// ("no case in the DSP's switch") until the parity sweep of 2026-09-16 found the DSP had both cases
	// and named them Timing and Skew — so no shipped device has a blank zone now, and the rule is
	// tested over a table that has one on purpose rather than over a device that happens to.
	namespace dtt = feathers::fx::owltt;
	ModFixture f;
	const size_t slot = f.bridge.addFxDevice(dtt::kTraits.tag);
	ui::DeviceControl coord { f.deviceHost, f.bridge.fxDeviceAt(slot), dtt::kDescriptor,
		                       static_cast<size_t>(dtt::Param::Character), nullptr, nullptr };
	std::vector<ui::ZoneSelect::ZoneName> names = owlttCharacterZones();
	REQUIRE(names.size() == 8);
	names[1] = { "", "" };
	names[2] = { "", "" };
	ui::ZoneSelect select { coord, names };

	coord.setValue(0.125); // zone 1 — blank in THIS table
	REQUIRE(select.zoneIndex() == 1);
	CHECK(select.zoneName().empty()); // the table's answer, unchanged
	CHECK(select.captionText() == "zone 1"); // what the WIDGET says
	coord.setValue(0.25);
	CHECK(select.captionText() == "zone 2");

	// A NAMED ONE IS UNTOUCHED BY THE FALLBACK, which is what keeps the two assertions above from
	// being satisfied by a widget that numbers everything.
	coord.setValue(0.0);
	CHECK(select.captionText() == "Width");

	// **AND A LINE WITH NO VOCABULARY AT ALL SAYS NOTHING.** Explore's gamma is such a line — a
	// continuum with no named regions over it — and numbering it would claim a structure the quantity
	// does not have.
	ui::ZoneSelect bare { coord, {} };
	CHECK(bare.captionText().empty());
	CHECK(bare.verbAt({ 5.0f, 5.0f }) == ui::ZoneSelect::Verb::None);
}

TEST_CASE("a disabled zone selector offers no verb and performs none") {
	// GREYED WITH ITS CARD, and it must REFUSE rather than merely look refused — the honesty rule is
	// about what a control does. It also answers `None` from the hit test rather than reporting a
	// verb it will decline, so the picture and the behaviour cannot disagree.
	namespace dtt = feathers::fx::owltt;
	ModFixture f;
	const size_t slot = f.bridge.addFxDevice(dtt::kTraits.tag);
	ui::DeviceControl coord { f.deviceHost, f.bridge.fxDeviceAt(slot), dtt::kDescriptor,
		                       static_cast<size_t>(dtt::Param::Character), nullptr, nullptr };
	ui::ZoneSelect select { coord, owlttCharacterZones() };
	select.setBounds(0, 0, 90, 18);
	coord.setValue(0.0);

	select.setEnabled(false);
	CHECK(select.verbAt({ 85.0f, 9.0f }) == ui::ZoneSelect::Verb::None);
	CHECK_FALSE(select.pressAt({ 85.0f, 9.0f }));
	CHECK(coord.value() == doctest::Approx(0.0));

	select.setEnabled(true);
	CHECK(select.pressAt({ 85.0f, 9.0f }));
	CHECK(coord.value() == doctest::Approx(0.125));
}

TEST_CASE("a missing device's card paints its tag in the head and \"device missing\" in the body") {
	// **RENDERED, NOT INFERRED FROM ITS TYPE** (CLAUDE.md: render geometry before believing it). The cases
	// on the three hosts assert which card they built and what it is called; this asserts the card
	// actually PAINTS both things, and that a folded one paints no body.
	constexpr int kW = 200;
	const int kH = static_cast<int>(ui::MissingDeviceCard::height());
	const auto render = [&](ui::MissingDeviceCard& card) {
		card.setBounds(0, 0, kW, kH);
		visage::Canvas canvas;
		markedWindowless(canvas, kW, kH);
		canvas.setColor(0xff000000);
		canvas.fill(0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH));
		visage::Region region;
		region.setBounds(0, 0, kW, kH);
		canvas.addRegion(&region);
		canvas.beginRegion(&region);
		card.draw(canvas);
		canvas.endRegion();
		// MARKED: a capture of anything but this frame stops here and names the venue (backlog IT).
		return takeMarkedCapture(canvas, kW, kH);
	};
	// INK IS WHAT IS BRIGHTER THAN THE CARD'S OWN GROUND, read from inside the body where neither text
	// nor border reaches (its right end, past any glyph of a fourteen-character message).
	const auto inkCounter = [&](const visage::Screenshot& shot) {
		const auto peakAt = [&](int x, int y) {
			return std::max({ static_cast<int>(shot.sample(x, y).hexRed()),
			                  static_cast<int>(shot.sample(x, y).hexGreen()),
			                  static_cast<int>(shot.sample(x, y).hexBlue()) });
		};
		const int ground = peakAt(kW - 20, kH - 8);
		return [peakAt, ground](int x0, int y0, int x1, int y1) {
			int count = 0;
			for (int y = y0; y < y1; ++y) {
				for (int x = x0; x < x1; ++x) {
					count += peakAt(x, y) > ground + 40 ? 1 : 0;
				}
			}
			return count;
		};
	};
	const int head = static_cast<int>(kCardHeadHeight);
	constexpr int kTextRight = 120; // a four-character tag and "device missing" both end well before this

	ui::MissingDeviceCard open { "zzz9" };
	const visage::Screenshot& shot = render(open);
	REQUIRE(shot.width() == kW);
	REQUIRE_MESSAGE(shot.sample(kW / 2, head + 4).hexRed() + shot.sample(kW / 2, head + 4).hexGreen() > 0,
	                "the card painted nothing: the capture's marker proved the frame, so this is the card, "
	                "not the venue (backlog IT)");
	const auto inkIn = inkCounter(shot);
	const int headInk = inkIn(20, 3, kTextRight, head - 3);
	const int bodyInk = inkIn(4, head + 2, kTextRight, kH - 2);
	CAPTURE(headInk);
	CAPTURE(bodyInk);
	CHECK(headInk > 0); // the tag
	CHECK(bodyInk > 0); // "device missing"

	ui::MissingDeviceCard folded { "zzz9" };
	folded.setFolded(true);
	const visage::Screenshot& foldedShot = render(folded);
	const auto foldedInk = inkCounter(foldedShot);
	CHECK(foldedInk(4, head + 2, kTextRight, kH - 2) == 0); // folded: the head and nothing else
}

TEST_CASE("a missing device's card frames the rectangle it was GIVEN, not the one its class names") {
	// **`MissingDeviceCard::height()` IS A STATIC THAT SHADOWS `visage::Frame::height()`**, and `draw`
	// began as a copy of `FxCard::draw` — so the unqualified `height()` in its frame rectangle
	// resolved to the class's 52, not to the frame's. `width()` is not shadowed, so the one expression
	// mixed a live width with a frozen height.
	//
	// **THE CASE ABOVE CANNOT SEE THIS AND IT IS WORTH SAYING WHY**: it sizes the card at exactly
	// `MissingDeviceCard::height()`, so the two heights coincide and every assertion it makes is true
	// of either. Both of the strip hosts grant `laneHeight()`, which for this card IS that static, so
	// they cannot see it either. **The rack is the host that can**: it grants `kCardUnitHeight` and
	// asks the card only for its width, which is 216x336 with a 52px frame painted into it.
	//
	// **SO THE CARD IS GIVEN A HEIGHT THAT IS NOT ITS CLASS'S, WHICH IS THE WHOLE SUBJECT.**
	constexpr int kW = 200;
	const int kStatic = static_cast<int>(ui::MissingDeviceCard::height());
	const int kGranted = kStatic + 120;
	REQUIRE(kGranted > kStatic); // else the case is the one above, wearing a different name

	ui::MissingDeviceCard card { "zzz9" };
	card.setBounds(0, 0, kW, kGranted);
	visage::Canvas canvas;
	markedWindowless(canvas, kW, kGranted);
	// **PURE BLACK BEHIND IT, so "the card painted here" and "nothing painted here" are different
	// numbers rather than two shades of the same intention.** The card's own ground is not black.
	canvas.setColor(0xff000000);
	canvas.fill(0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kGranted));
	visage::Region region;
	region.setBounds(0, 0, kW, kGranted);
	canvas.addRegion(&region);
	canvas.beginRegion(&region);
	card.draw(canvas);
	canvas.endRegion();
	// MARKED: a capture of anything but this frame stops here and names the venue (backlog IT).
	const visage::Screenshot shot = takeMarkedCapture(canvas, kW, kGranted);
	REQUIRE(shot.width() == kW);

	const auto luma = [&shot](int x, int y) {
		return static_cast<int>(shot.sample(x, y).hexRed())
		       + static_cast<int>(shot.sample(x, y).hexGreen())
		       + static_cast<int>(shot.sample(x, y).hexBlue());
	};
	// **THE VALUE: the card's own ground, sampled where the card certainly is.** Clear of the head, of
	// the message and of the border, and REQUIREd to be distinguishable from the black behind it —
	// which is what makes the assertion below capable of failing.
	const int ground = luma(kW - 12, kStatic - 12);
	REQUIRE(ground > 12);
	REQUIRE_MESSAGE(ground > 0, "the card painted nothing: the capture's marker proved the frame, so this "
	                            "is the card, not the venue (backlog IT)");

	// **AND IT IS STILL THE CARD'S GROUND NEAR THE BOTTOM OF WHAT IT WAS GIVEN.** Read as the VALUE,
	// against the ground measured above: a frame painted at 52 leaves pure black here.
	const int deep = luma(kW - 12, kGranted - 6);
	CAPTURE(ground);
	CAPTURE(deep);
	CHECK(deep == ground);
}

TEST_CASE("a canvas whose gradient atlas grows taller still paints every colour it was given") {
	// **BACKLOG IU, AT THE ONLY ROAD A WIDGET HAS INTO IT.** visage's gradient atlas is 512 wide and
	// grows in HEIGHT alone once it holds more full-width gradients than its 64 rows. `resize` used to
	// recreate the texture only when BOTH dimensions changed, so a taller atlas kept the old texture:
	// re-uploads past row 64 landed outside it and every colour sampled a row normalised by the new
	// height. A widget cannot choose its gradients' rows, but it can have more than 64 of them.
	//
	// So: 80 swatches, each a REPEATING one-colour gradient (repeat forces full resolution, so each
	// takes a whole row), and every swatch's centre read back as its OWN colour — a value, not "the
	// swatches differ". KNOWN-BAD: `||` put back to `&&` in `GradientAtlas::resize`.
	constexpr int kCols = 10;
	constexpr int kRows = 8;
	constexpr int kCell = 10;
	constexpr int kW = kCols * kCell;
	constexpr int kH = kRows * kCell;
	const auto colourOf = [](int i) {
		return visage::Color(0xff000000u | (static_cast<uint32_t>(0x20 + i * 2) << 16) |
		                     (static_cast<uint32_t>(0xe0 - i * 2) << 8) | static_cast<uint32_t>(0x40 + (i * 37) % 0x80));
	};

	// **NOT A MARKED CAPTURE, deliberately.** The marker's colour comes through the same atlas as the
	// swatches, so under this fault it reads wrong too and the guard would blame the venue for what is
	// the atlas's own defect. MEASURED on the known-bad: "marker row did not come back", 0 of 100.
	// The swatch census below names the failure instead.
	visage::Canvas canvas;
	canvas.setWindowless(kW, kH);
	canvas.setColor(0xff000000);
	canvas.fill(0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH));
	for (int i = 0; i < kCols * kRows; ++i) {
		// A SUBMIT AFTER THE FIRST ROW, SO THE TEXTURE EXISTS BEFORE THE ATLAS GROWS. Gradients are packed
		// as they are drawn and the texture is made lazily at submit, so drawing all 80 first creates it
		// at its final height and never takes the road under test — measured: the known-bad stayed green.
		if (i == kCols) {
			canvas.submit();
		}
		visage::Gradient gradient(colourOf(i), colourOf(i));
		gradient.setRepeat(true);
		REQUIRE(gradient.resolution() == visage::Gradient::kMaxGradientResolution); // a row each
		canvas.setColor(visage::Brush::horizontal(gradient));
		canvas.fill(static_cast<float>((i % kCols) * kCell), static_cast<float>((i / kCols) * kCell),
		            static_cast<float>(kCell), static_cast<float>(kCell));
	}
	canvas.submit();
	// THREE TIMES: the windowless renderer can hand back the frame before the one just submitted.
	canvas.takeScreenshot();
	canvas.takeScreenshot();
	const visage::Screenshot& shot = canvas.takeScreenshot();
	REQUIRE(shot.width() == kW);

	int wrong = 0;
	for (int i = 0; i < kCols * kRows; ++i) {
		const visage::Color want = colourOf(i);
		const visage::Color got = shot.sample((i % kCols) * kCell + kCell / 2, (i / kCols) * kCell + kCell / 2);
		const auto off = [](int a, int b) { return std::abs(a - b) > 2; };
		if (off(got.hexRed(), want.hexRed()) || off(got.hexGreen(), want.hexGreen())
		    || off(got.hexBlue(), want.hexBlue())) {
			++wrong;
			if (wrong <= 3) {
				MESSAGE("swatch " << i << " reads " << got.hexRed() << "," << got.hexGreen() << "," << got.hexBlue()
				                  << " for " << want.hexRed() << "," << want.hexGreen() << "," << want.hexBlue());
			}
		}
	}
	CHECK(wrong == 0);
}

TEST_CASE("a zone selector draws a caption and two chevrons, each in its own cell") {
	// **RENDER GEOMETRY BEFORE BELIEVING IT.** The bounds sweep above proves the three cells PARTITION
	// the row; it says nothing about whether anything is drawn in them. A widget whose caption is
	// centred on the whole frame instead of on its cell, or whose chevrons are outside their cells,
	// satisfies every assertion in this file and looks wrong the moment it is seated.
	//
	// **AND IT ASSERTS INK PER CELL, NOT INK ANYWHERE.** "Something was drawn" passes for a caption
	// painted over the arrows — the difference-check trap this repo has paid for four times.
	namespace dtt = feathers::fx::owltt;
	ModFixture f;
	const size_t slot = f.bridge.addFxDevice(dtt::kTraits.tag);
	ui::DeviceControl coord { f.deviceHost, f.bridge.fxDeviceAt(slot), dtt::kDescriptor,
		                       static_cast<size_t>(dtt::Param::Character), nullptr, nullptr };
	ui::ZoneSelect select { coord, owlttCharacterZones() };
	constexpr int kW = 90;
	constexpr int kH = 18;
	select.setBounds(0, 0, kW, kH);
	coord.setValue(0.0); // "Width" — a named region, so the caption has glyphs

	visage::Canvas canvas;
	markedWindowless(canvas, kW, kH);
	canvas.setColor(0xff000000);
	canvas.fill(0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH));
	visage::Region region;
	region.setBounds(0, 0, kW, kH);
	canvas.addRegion(&region);
	canvas.beginRegion(&region);
	select.draw(canvas);
	canvas.endRegion();
	// MARKED: a capture of anything but this frame stops here and names the venue (backlog IT).
	const visage::Screenshot shot = takeMarkedCapture(canvas, kW, kH);
	REQUIRE(shot.width() == kW);

	// THE GROUND IS `kSurface`, so "ink" is anything brighter than it. Measured rather than guessed:
	// the surface's own brightest channel is what the empty corners report.
	const auto peakAt = [&](int x, int y) {
		return std::max({ static_cast<int>(shot.sample(x, y).hexRed()),
		                  static_cast<int>(shot.sample(x, y).hexGreen()),
		                  static_cast<int>(shot.sample(x, y).hexBlue()) });
	};
	int ground = 0;
	for (int x = 0; x < kW; ++x) {
		ground = std::max(ground, peakAt(x, 1)); // the top row: chrome and text both avoid it
	}
	// **A DEAD CAPTURE AND A DEAD WIDGET LOOK IDENTICAL, SO SEPARATE THEM BEFORE ASSERTING.** They
	// are separated now by the marker `takeMarkedCapture` checks (backlog IT): a capture that is not
	// this frame stops there with the venue's name on it. So a ground of zero past that point is the
	// widget failing to fill its ground, which it does unconditionally — and this says so rather than
	// blaming the venue, which is what it said while the two could not be told apart.
	REQUIRE_MESSAGE(ground > 0, "the selector painted nothing: the capture's marker proved the frame, so "
	                            "this is the widget, not the venue (backlog IT)");
	const int floorLevel = ground + 20;

	const auto inkIn = [&](const visage::Bounds& b) {
		int count = 0;
		for (int y = static_cast<int>(b.y()); y < static_cast<int>(b.bottom()); ++y) {
			for (int x = static_cast<int>(b.x()); x < static_cast<int>(b.right()); ++x) {
				count += peakAt(x, y) > floorLevel ? 1 : 0;
			}
		}
		return count;
	};

	const int prevInk = inkIn(select.prevBounds());
	const int nextInk = inkIn(select.nextBounds());
	const int capInk = inkIn(select.captionBounds());
	CAPTURE(ground);
	CAPTURE(prevInk);
	CAPTURE(nextInk);
	CAPTURE(capInk);
	CHECK(prevInk > 0);  // the left chevron is drawn
	CHECK(nextInk > 0);  // and the right one
	CHECK(capInk > 0);   // and the name between them
	// THE TWO CHEVRONS ARE THE SAME MARK MIRRORED, so a large asymmetry means one of them is being
	// clipped by its cell rather than sitting inside it.
	CHECK(std::abs(prevInk - nextInk) <= std::max(prevInk, nextInk) / 2);
	// AND THE CAPTION IS THE BIGGEST THING ON THE ROW — five glyphs against two small strokes. This
	// is what fails if the text is centred on the frame rather than on its cell, because half of it
	// then lands under the arrows.
	CHECK(capInk > prevInk);

	// **A DUMP TO LOOK AT**, because every assertion above is a proxy for the only question that
	// matters, which is whether it reads as a control. Written under the scratch dir when the
	// environment names one, skipped silently otherwise so a normal run writes nothing.
	if (const char* dir = std::getenv("FEATHERS_SHOT_DIR"); dir != nullptr) {
		std::string path = std::string(dir) + "/zone_select.ppm";
		if (std::FILE* out = std::fopen(path.c_str(), "wb"); out != nullptr) {
			std::fprintf(out, "P6\n%d %d\n255\n", kW, kH);
			for (int y = 0; y < kH; ++y) {
				for (int x = 0; x < kW; ++x) {
					const unsigned char rgb[3] = {
						static_cast<unsigned char>(shot.sample(x, y).hexRed()),
						static_cast<unsigned char>(shot.sample(x, y).hexGreen()),
						static_cast<unsigned char>(shot.sample(x, y).hexBlue())
					};
					std::fwrite(rgb, 1, 3, out);
				}
			}
			std::fclose(out);
		}
	}
}

namespace {
/// A READABLE TYPE NAME. `typeid::name` is the mangled form on this toolchain, and "1" is not a
/// finding anybody can act on.
std::string demangled(const char* mangled) {
	int status = 0;
	char* out = abi::__cxa_demangle(mangled, nullptr, nullptr, &status);
	std::string name = (status == 0 && out != nullptr) ? out : mangled;
	std::free(out);
	return name;
}
} // namespace

TEST_CASE("every value box in the assembled editor is on the mod state's watcher list") {
	// Value boxes did not glow in assign mode: they got a border, but only after being clicked.
	// The box read the right state all along — `draw` computes `isDestination &&
	// armed` exactly as `Knob` does — and nothing ever TOLD it when that state moved, so it kept
	// painting the last frame until some other edit repainted it. A click is such an edit.
	//
	// **THE SAME REPORT ONE WIDGET OVER, AND THE KNOB HALF IS ALREADY IN THE RECORD.**
	// `frame_watchers.h` carries it: the effect knobs did not glow at once on assign, on
	// 2026-09-05, where a `Watch` move-assignment silently unsubscribed the six knobs a hand was about
	// to point at — *"right in content, never delivered". That was fixed. This was never wired at all:
	// `Knob::setModulation` ends in `watch_ = mod_->watch(this)` and `ValueBox` had no watch.
	//
	// **ASKED OF THE ASSEMBLED EDITOR, NOT OF ONE BOX ON A BENCH.** A hand-built box needs a state and
	// a canvas the editor would have given it, and the first draft of this case did exactly that and
	// died in teardown with every assertion green — a lifetime tangle that says nothing about the
	// product. The real question is whether the boxes a user actually touches are subscribed, and the
	// lane suite already asks its surfaces the same way for the same reason.
	//
	// **AND THE SUBSCRIPTION IS THE WITNESS BECAUSE A REPAINT IS NOT ONE.** CLAUDE.md: `redraw()` does
	// nothing to a frame nobody is drawing and `Frame` exposes no staleness.
	// **A DEVICE IS ADDED, AND WITHOUT ONE THIS CASE ASKED THE WRONG POPULATION.**
	//
	// MEASURED: in an EMPTY editor the only destination boxes are the envelope card's seven — and
	// `env_card.cpp:167` subscribes those BY HAND, one at a time. So the first draft of this case
	// passed with the whole fix removed, twice, against two known-bads. It was measuring the one
	// surface that never had the bug.
	//
	// **THAT HAND-WIRING IS ALSO THE SHAPE OF THE DEFECT.** One card remembered to subscribe its boxes
	// and every other surface forgot, which is exactly what `mod_route_menu.h` was written to stop —
	// *"seven `ValueBox` construction sites, each free to forget it". A device card's boxes are the
	// ones a hand meets in assign mode, so a device is what this case must have in it.
	FilterFixture f;
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);
	// **AUTOMOD, BECAUSE ITS CARD IS ONE THAT ACTUALLY HAS BOXES.** OWLTT's card seats only knobs — its
	// boxes live in the expanded panel — so a case built on it was still asking the env card's
	// question. Automod's card carries three `ZoneCoordCell`s and a gamma box, over device controls
	// that are real destinations.
	REQUIRE(f.bridge.addFxDevice(feathers::fx::automod::kTraits.tag) < feathers::kMaxFxSlots);
	f.lane->resized();

	size_t boxes = 0;
	size_t subscribed = 0;
	std::function<void(visage::Frame*)> walk = [&](visage::Frame* frame) {
		for (visage::Frame* child : frame->children()) {
			if (auto* box = dynamic_cast<ui::ValueBox*>(child)) {
				// **ONLY BOXES THAT CAN LIGHT, and the exclusion is a fact rather than a convenience.**
				// Explore's gamma box is over a `GammaControl`, whose `display()` says *"NEVER A
				// DESTINATION. Not a policy this class enforces — a fact about a quantity with no
				// parameter id." A box that can never light has nothing to be repainted FOR, and
				// subscribing it would wake it on every arm to draw the same frame.
				if (!box->control().display().isDestination) {
					continue;
				}
				++boxes;
				// THE CONTROL'S TYPE, because the label is often empty and the ANSWER to a failure here
				// is always "which control could not find its state" — see `RoutedControl`.
				// The control is taken into a reference FIRST: `typeid` of a call expression evaluates the call,
				// and clang's -Wpotentially-evaluated-expression flagged it (macOS CI run 35169434704).
				const auto& control = box->control();
				INFO("box '" << box->label() << "' over " << demangled(typeid(control).name()));
				CHECK(mod->watches(box));
				subscribed += mod->watches(box) ? 1 : 0;
			}
			walk(child);
		}
	};
	walk(f.root);

	// **THE SWEEP MUST HAVE SWEPT SOMETHING.** A walk that found no boxes would pass by checking
	// nothing, which is how a tree walk usually fails — the editor's DAHDSR card alone carries seven.
	// MEASURED at 7 destination boxes in a fresh editor — 29 boxes in the tree, of which most are over
	// controls that are not destinations in an empty chain. The floor is set well below so this does
	// not become a change detector for the editor's furniture.
	CAPTURE(boxes);
	CHECK(boxes >= 4);
	CHECK(subscribed == boxes);
}

TEST_CASE("backlog DN: the master meter stands at the knob tray's far right at the tray's height, reads what the bridge published on the editor's tick, turns orange from -6 LUFS and red from -3, and its 0 dBFS LED has an instantaneous core and a decaying glow") {
	ModFixture f;
	auto* meter = findInTree<ui::MasterMeter>(f.root);
	auto* tray = findInTree<ui::KnobTray>(f.root);
	REQUIRE(meter != nullptr);
	REQUIRE(tray != nullptr);

	// THE PLACE: the tray's height, one tray padding in from the editor's right edge, the tray's frame
	// ending where the meter's begins — the tray's own right padding is the gap between the last knob
	// and the meter, the same padding that is the gap between the scope and the first knob.
	CHECK(meter->height() == doctest::Approx(ui::KnobTray::kHeight));
	CHECK(meter->width() == doctest::Approx(ui::MasterMeter::kWidth));
	CHECK(meter->right() == doctest::Approx(f.root->width() - ui::KnobTray::kPadX));
	CHECK(meter->y() == doctest::Approx(tray->y()));
	CHECK(tray->right() <= meter->x() + 0.5f);
	CHECK(tray->right() >= meter->x() - 0.5f);

	// AT REST: silence, nothing lit.
	CHECK(meter->zoneForTest() == ui::MasterMeter::Zone::Ink);
	CHECK_FALSE(meter->ledCoreLitForTest());
	CHECK(meter->ledGlowForTest() == 0.0f);

	// WHAT THE BRIDGE PUBLISHED REACHES IT ON THE TICK, and the zones sit where it was put them.
	const auto publish = [&](float lufs, float clipAge) {
		f.bridge.publishMasterMeter({ lufs, 0.5f, clipAge });
		(void)f.gui.tickForTest();
	};
	publish(-12.0f, fx::busutil::kClipAgeMax);
	CHECK(meter->readingsForTest().momentaryLufs == -12.0f);
	CHECK(meter->zoneForTest() == ui::MasterMeter::Zone::Ink);
	publish(-6.0f, fx::busutil::kClipAgeMax);
	CHECK(meter->zoneForTest() == ui::MasterMeter::Zone::Orange);
	publish(-4.0f, fx::busutil::kClipAgeMax);
	CHECK(meter->zoneForTest() == ui::MasterMeter::Zone::Orange);
	publish(-3.0f, fx::busutil::kClipAgeMax);
	CHECK(meter->zoneForTest() == ui::MasterMeter::Zone::Red);
	publish(-0.5f, fx::busutil::kClipAgeMax);
	CHECK(meter->zoneForTest() == ui::MasterMeter::Zone::Red);
	CHECK(ui::MasterMeter::fillFractionFor(ui::MasterMeter::kFloorLufs) == 0.0f);
	CHECK(ui::MasterMeter::fillFractionFor(0.0f) == 1.0f);
	CHECK(ui::MasterMeter::fillFractionFor(-20.0f) == doctest::Approx(0.5f));

	// THE LED: a clip just now lights the core inside a full glow; seconds later the core is out and the
	// glow is fading; long after, nothing.
	publish(-12.0f, 0.01f);
	CHECK(meter->ledCoreLitForTest());
	CHECK(meter->ledGlowForTest() > 0.95f);
	publish(-12.0f, 2.0f);
	CHECK_FALSE(meter->ledCoreLitForTest());
	CHECK(meter->ledGlowForTest() > 0.0f);
	CHECK(meter->ledGlowForTest() < 0.5f);
	publish(-12.0f, fx::busutil::kClipAgeMax);
	CHECK(meter->ledGlowForTest() == 0.0f);

	// AND A TICK WITH NOTHING NEW ASKS FOR NO FRAME.
	CHECK_FALSE(meter->update());
}

TEST_CASE("backlog ML: the master's 0 dBFS LED has three states by the published peak — normal under -2 dBFS, near from -2 to just under 0, clip at 0 and over") {
	// The clip LEDs, the master's especially, take a distinct colour from -2 to 0 dBFS, so that almost clipping is
	// easy to tell from clipping. Asserted as the STATE the LED draws, read off the published raw peak on
	// the editor's tick; no over latched (the clip age at its maximum), so the peak alone decides.
	ModFixture f;
	auto* meter = findInTree<ui::MasterMeter>(f.root);
	REQUIRE(meter != nullptr);
	const auto stateAt = [&](double dbfs, float clipAge) {
		const float peak = static_cast<float>(std::pow(10.0, dbfs / 20.0));
		f.bridge.publishMasterMeter({ -12.0f, peak, clipAge });
		(void)f.gui.tickForTest();
		REQUIRE(meter->readingsForTest().rawPeak == peak);
		return meter->ledStateForTest();
	};
	const float none = fx::busutil::kClipAgeMax;
	CHECK(stateAt(-2.1, none) == ui::ClipLedState::Normal);
	CHECK(stateAt(-1.0, none) == ui::ClipLedState::Near);
	CHECK(stateAt(-0.01, none) == ui::ClipLedState::Near);
	// 0 dBFS IS A CLIP: the engine's over is `>= 1.0`, and a hard clamp's output sits exactly there (`clip_led.h`).
	CHECK(stateAt(0.0, none) == ui::ClipLedState::Clip);
	CHECK(stateAt(0.1, none) == ui::ClipLedState::Clip);
	// AN OVER STILL HELD keeps the LED a clip while the held peak has already fallen back under -2 dBFS.
	CHECK(stateAt(-20.0, 0.01f) == ui::ClipLedState::Clip);
	CHECK(stateAt(-20.0, none) == ui::ClipLedState::Normal);
	// THE BOUNDARY ITSELF: -2 dBFS is near, by the constant the classifier reads.
	CHECK(ui::clipLedStateFor(ui::kNearClipLinear, false) == ui::ClipLedState::Near);
	CHECK(ui::kNearClipLinear == doctest::Approx(std::pow(10.0, -2.0 / 20.0)).epsilon(1e-7));
	// THE NEAR COLOUR IS ITS OWN: neither meter colour nor the over's red, and fixed (data, not chrome).
	CHECK(ui::kMeterNear != ui::kMeterValue);
	CHECK(ui::kMeterNear != ui::kMeterAccent);
	CHECK(ui::kMeterNear != 0xffD9483Bu);
}

TEST_CASE("a pan computes only the ground it has not seen before") {
	// **THE DRAG-PAN FLICKER, PINNED AS WORK DONE RATHER THAN AS A CLOCK** (backlog GJ, 2026-09-19:
	// the map flickered only while moving, and then only on a drag pan).
	//
	// WHAT WAS WRONG: a build threw away the ~90% of its texels it had just carried EXACTLY and
	// recomputed them. Measured through `FEATHERS_TRACE_TERRAIN_REUSE` — 14,720 of 16,384 reusable,
	// lattice slip 0.0000 — so each build took ten times the work it needed, a drag restarted it every
	// frame, and the chart was drawn part-built on essentially every frame of every pan.
	//
	// WHY THIS IS THE RIGHT SHAPE FOR THE CASE. The tempting assertion is "no frame sees an incomplete
	// map", and it is a lie detector for the host's load rather than for this code: it passes on a
	// quiet box and fails at load 29 with nothing changed. `texelsComputed()` counts WORK, and the
	// skip is decided by `TerrainSnapshot::computed` rather than by how much time was available — so
	// this reads the same on an idle machine and a hammered one.
	TerrainMap map;
	constexpr double span = 64.0 / 1023.0;

	// A FIRST MAP, PAID FOR IN FULL. Nothing to carry from, so every texel is computed and the count
	// below is the honest price of a map — which is also the reference the pan is measured against.
	const TerrainWindow first = windowAt(0.5, span, 2.0);
	map.request(first);
	REQUIRE(settle(map, first));
	const uint64_t afterFirst = map.texelsComputed();
	INFO("texels computed for a cold map: " << afterFirst);
	REQUIRE(afterFirst >= static_cast<uint64_t>(TerrainMap::kResolution) * TerrainMap::kResolution);

	// NOW PAN BY EXACTLY EIGHT CELLS. A whole number of cells is what `anchoredTravelY` produces anyway,
	// and stating it here rather than relying on the snap keeps the case honest about what it is
	// asking: two windows whose grids coincide, differing only in where they are centred.
	const double cell = span / static_cast<double>(TerrainMap::kResolution);
	const TerrainWindow panned = windowAt(0.5 + 8.0 * cell, span, 2.0);
	map.request(panned);
	REQUIRE(settle(map, panned));
	const uint64_t panCost = map.texelsComputed() - afterFirst;

	// EIGHT ROWS OF NEW GROUND, NOT ONE HUNDRED AND TWENTY-EIGHT. The exposed strip is 8 rows of 128
	// columns; the bound is set well above that and far below a full map, so it cannot be satisfied by
	// a build that recomputed everything and cannot fail because the strip's edge rounds by a row.
	const uint64_t full = static_cast<uint64_t>(TerrainMap::kResolution) * TerrainMap::kResolution;
	INFO("texels computed for an 8-cell pan: " << panCost << " against " << full << " for a full map");
	CHECK(panCost > 0);
	CHECK(panCost < full / 4);

	// NOT VACUOUS, AND THIS IS THE HALF THAT WOULD ROT QUIETLY: a build that computed NOTHING would
	// satisfy the bound above and would mean the map had stopped being rebuilt at all. The strip has
	// to have actually been paid for, so the floor is the strip itself.
	CHECK(panCost >= 8u * TerrainMap::kResolution);

	// AND A CHANGE THAT IS NOT A PAN STILL PAYS IN FULL. The skip is gated on the two windows agreeing
	// on every field but their centres; a zoom does not, so nothing may be inherited and the whole map
	// is earned again. Without this the case would pass just as well against a build that reused
	// texels it had no right to — which is the one way this optimisation can be actively wrong, and it
	// would show as terrain that refuses to change when the world does.
	const uint64_t beforeZoom = map.texelsComputed();
	const TerrainWindow zoomed = windowAt(0.5 + 8.0 * cell, span * 0.5, 2.0);
	map.request(zoomed);
	REQUIRE(settle(map, zoomed));
	const uint64_t zoomCost = map.texelsComputed() - beforeZoom;
	INFO("texels computed for a zoom: " << zoomCost << " against " << full << " for a full map");
	CHECK(zoomCost >= full);
}

TEST_CASE("a second carry that finds nothing to do does not un-say that the map is full") {
	// **THE ONE-FRAME BLANK CIRCLE** (on the build that fixed the part-built map, the entire circle
	// sometimes disappeared for about one frame, again only on an aggressive drag pan).
	//
	// `buildMap` carries twice — from the map being replaced, then from the prefetched one with
	// `onlyEmpty`, which fills only what the first left. `carry` used to END with
	// `into.carried = carried`, a plain assignment of ITS OWN local result, so the second call
	// overwrote the first's answer. Fill the buffer completely on the first pass and the second finds
	// nothing, returns false, and says the buffer is empty.
	//
	// WHY THAT BLANKS THE CHART RATHER THAN DEGRADING IT: `seeded` is `carry(prefetch) || seeded`, so
	// the snapshot is still published, carrying `rows == 0` and `carried == false`. Every draw gate on
	// the surface asks `rows > 0 || carried`, so that frame draws no terrain at all.
	//
	// AND THE FIX MADE IT MORE LIKELY, WHICH IS WHY IT SURFACED WHEN IT DID. The better the first
	// carry fills the map, the less the prefetch has to do — and `TerrainSnapshot::computed` made
	// pans carry almost everything.
	constexpr double span = 64.0 / 1023.0;
	TerrainMap map;
	const TerrainWindow window = windowAt(0.5, span, 2.0);
	map.request(window);
	REQUIRE(settle(map, window));
	const std::shared_ptr<const TerrainSnapshot> full = map.snapshot();
	REQUIRE(full->rows == TerrainMap::kResolution);

	// A FRESH BUFFER FILLED BY THE FIRST CARRY, exactly as `buildMap` does it.
	TerrainSnapshot building;
	building.window = window;
	building.texels.assign(static_cast<size_t>(TerrainMap::kResolution) * TerrainMap::kResolution, 0u);
	building.features.assign(static_cast<size_t>(TerrainMap::kResolution) * TerrainMap::kResolution,
	                         TerrainFeatures {});
	building.computed.assign(static_cast<size_t>(TerrainMap::kResolution) * TerrainMap::kResolution, 0u);
	REQUIRE(TerrainMap::carry(building, *full));
	REQUIRE(building.carried);

	// NOW THE SECOND CARRY, WITH NOTHING LEFT FOR IT. Same map, `onlyEmpty` — every position is
	// already filled, so it copies nothing and returns false. That return is CORRECT and is not what
	// is under test; what is under test is that it does not retract the first carry's answer.
	const bool second = TerrainMap::carry(building, *full, true);
	CHECK_FALSE(second);
	CHECK(building.carried);

	// AND THE CONSEQUENCE, ASSERTED WHERE THE SURFACE READS IT rather than on the flag alone — a flag
	// is only interesting because three draw gates ask it this exact question. This is the condition
	// that was false for one frame and took the whole circle with it.
	INFO("rows " << building.rows << ", carried " << building.carried);
	CHECK((building.rows > 0 || building.carried));

	// NOT VACUOUS: a buffer nothing has carried into must still answer NO, or the assertion above
	// passes for a map that really is empty and the case certifies nothing.
	TerrainSnapshot untouched;
	untouched.window = window;
	untouched.texels.assign(static_cast<size_t>(TerrainMap::kResolution) * TerrainMap::kResolution, 0u);
	CHECK_FALSE((untouched.rows > 0 || untouched.carried));
}

TEST_CASE("IW: a sigil is four to six strokes on a 3x3 grid, the same mark for a lap every time, and a different mark for every lap near home") {
	// A display that decodes coarse phase into a small random glyph — a generated sigil, a little bigger
	// than one character. A PURE FUNCTION of the lap, asserted as values.
	std::set<uint32_t> seen;
	for (int64_t lap = -64; lap <= 64; ++lap) {
		CAPTURE(lap);
		const uint32_t mask = ui::Sigil::strokesFor(lap);
		CHECK(mask < (1u << ui::Sigil::kEdges)); // only the grid's twenty edges
		const int strokes = std::popcount(mask);
		CHECK(strokes >= 4);
		CHECK(strokes <= 6);
		CHECK(ui::Sigil::strokesFor(lap) == mask); // stable
		seen.insert(mask);
	}
	// DISTINCT ACROSS 129 LAPS — a hand that has been two laps apart must not see one mark.
	CHECK(seen.size() == 129);
	// NEIGHBOURS DIFFER BY MORE THAN ONE STROKE, so a step of one lap reads as a new mark, not a tweak.
	for (int64_t lap = -64; lap < 64; ++lap) {
		CAPTURE(lap);
		CHECK(std::popcount(ui::Sigil::strokesFor(lap) ^ ui::Sigil::strokesFor(lap + 1)) >= 2);
	}
	// EVERY EDGE JOINS TWO DISTINCT GRID POINTS THAT ARE NEIGHBOURS (a step of at most one across and one down).
	for (int e = 0; e < ui::Sigil::kEdges; ++e) {
		const auto [a, b] = ui::Sigil::endpointsOf(e);
		CAPTURE(e);
		CHECK(a != b);
		CHECK(std::abs(a % 3 - b % 3) <= 1);
		CHECK(std::abs(a / 3 - b / 3) <= 1);
	}
	// "A BIT BIGGER THAN ONE CHARACTER": the readout row's height, above the 9 px readout face.
	CHECK(ui::Sigil::kSide > ui::kKnobReadoutSize);
}

namespace {

/// **A CHIP'S CONTROL, COUNTED.** The range is deliberately NOT 0..1 — -2..3 — so a chip that wrote a
/// hard-coded 0 or 1 instead of the declared ends is caught by the value, not by luck.
struct ChipSpy final : device::Modulatable {
	static constexpr const char* kNames[] = { "Off", "On" };
	device::ParamDesc d { "macro", "Macro", -2.0, 3.0, -2.0, true, device::Unit::Choice, kNames };
	double v = -2.0;
	int begins = 0, ends = 0, writes = 0;
	const device::ParamDesc& desc() const override { return d; }
	double value() const override { return v; }
	void setValue(double value) override {
		++writes;
		v = value;
	}
	device::ModDisplay display() const override { return {}; }
	void setAmount(float) override {}
	void beginGesture() override { ++begins; }
	void endGesture() override { ++ends; }
	std::string text() const override { return {}; }
};

visage::MouseEvent rightAt(float x, float y) {
	visage::MouseEvent event = at(x, y);
	event.button_id = visage::kMouseButtonRight;
	event.button_state = visage::kMouseButtonRight;
	return event;
}

} // namespace

TEST_CASE("momentary chip: press writes max, release writes min, one gesture") {
	ChipSpy spy;
	ui::SwitchChip chip { spy, "Hold" };
	chip.setBounds(0, 0, 60, ui::SwitchChip::kHeight);
	chip.setBehaviour(ui::SwitchChip::Behaviour::Momentary);

	chip.onMouseDown().callback(at(30.0f, 8.0f));
	CHECK(spy.v == 3.0);
	CHECK(chip.on());
	CHECK(spy.begins == 1);
	CHECK(spy.ends == 0); // HELD: the gesture is open for as long as the hand is down

	chip.onMouseUp().callback(at(30.0f, 8.0f));
	CHECK(spy.v == -2.0);
	CHECK_FALSE(chip.on());
	CHECK(spy.begins == 1);
	CHECK(spy.ends == 1);

	// A RELEASE WITH NO PRESS BEHIND IT does nothing — no write, no stray end.
	const int writes = spy.writes;
	chip.onMouseUp().callback(at(30.0f, 8.0f));
	CHECK(spy.writes == writes);
	CHECK(spy.begins == 1);
	CHECK(spy.ends == 1);
}

TEST_CASE("momentary chip: leaving while held does not release") {
	// A HARDWARE MOMENTARY: sliding off the button keeps it down until the hand lets go.
	ChipSpy spy;
	ui::SwitchChip chip { spy, "Hold" };
	chip.setBounds(0, 0, 60, ui::SwitchChip::kHeight);
	chip.setBehaviour(ui::SwitchChip::Behaviour::Momentary);

	chip.onMouseDown().callback(at(30.0f, 8.0f));
	REQUIRE(spy.v == 3.0);
	chip.onMouseDrag().callback(at(200.0f, 90.0f));
	chip.onMouseExit().callback(at(200.0f, 90.0f));
	CHECK(spy.v == 3.0);
	CHECK(spy.ends == 0);

	chip.onMouseUp().callback(at(200.0f, 90.0f));
	CHECK(spy.v == -2.0);
	CHECK(spy.begins == 1);
	CHECK(spy.ends == 1);
}

TEST_CASE("latch chip toggles") {
	ChipSpy spy;
	ui::SwitchChip chip { spy, "Latch" };
	chip.setBounds(0, 0, 60, ui::SwitchChip::kHeight);
	// THE DEFAULT IS THE LATCH — every chip seated before momentary existed is one.
	chip.onMouseDown().callback(at(30.0f, 8.0f));
	chip.onMouseUp().callback(at(30.0f, 8.0f));
	CHECK(spy.v == 3.0);
	CHECK(spy.begins == 1);
	CHECK(spy.ends == 1);
	chip.onMouseDown().callback(at(30.0f, 8.0f));
	chip.onMouseUp().callback(at(30.0f, 8.0f));
	CHECK(spy.v == -2.0);
	CHECK(spy.begins == 2);
	CHECK(spy.ends == 2);
}

TEST_CASE("right-click opens the popup and does not toggle") {
	for (const auto behaviour : { ui::SwitchChip::Behaviour::Latch, ui::SwitchChip::Behaviour::Momentary }) {
		CAPTURE(static_cast<int>(behaviour));
		ChipSpy spy;
		ui::SwitchChip chip { spy, "Macro" };
		chip.setBounds(0, 0, 60, ui::SwitchChip::kHeight);
		chip.setBehaviour(behaviour);
		int popups = 0;
		chip.setPopup([&popups] { ++popups; });
		chip.onMouseDown().callback(rightAt(30.0f, 8.0f));
		chip.onMouseUp().callback(rightAt(30.0f, 8.0f));
		CHECK(popups == 1);
		CHECK(spy.v == -2.0);
		CHECK(spy.writes == 0);
		CHECK(spy.begins == 0);
		CHECK(spy.ends == 0);
	}
}

TEST_CASE("right-click with no popup does nothing") {
	for (const auto behaviour : { ui::SwitchChip::Behaviour::Latch, ui::SwitchChip::Behaviour::Momentary }) {
		CAPTURE(static_cast<int>(behaviour));
		ChipSpy spy;
		ui::SwitchChip chip { spy, "Macro" };
		chip.setBounds(0, 0, 60, ui::SwitchChip::kHeight);
		chip.setBehaviour(behaviour);
		chip.onMouseDown().callback(rightAt(30.0f, 8.0f));
		chip.onMouseUp().callback(rightAt(30.0f, 8.0f));
		CHECK(spy.v == -2.0);
		CHECK(spy.writes == 0);
		CHECK(spy.begins == 0);
		CHECK(spy.ends == 0);
	}
}

TEST_CASE("momentary chip: hidden while held releases") {
	// A SLOT HIDES THE CHIP WHEN THE MACRO'S TYPE CHANGES — possibly under the hand. The macro must not
	// be left at max behind a chip nobody can see, and the host's gesture must close.
	ChipSpy spy;
	ui::SwitchChip chip { spy, "Hold" };
	chip.setBounds(0, 0, 60, ui::SwitchChip::kHeight);
	chip.setBehaviour(ui::SwitchChip::Behaviour::Momentary);
	chip.onMouseDown().callback(at(30.0f, 8.0f));
	REQUIRE(spy.v == 3.0);
	REQUIRE(spy.ends == 0);

	chip.setVisible(false);
	CHECK(spy.v == -2.0);
	CHECK(spy.begins == 1);
	CHECK(spy.ends == 1);

	// The release that follows finds nothing held — no second write, no second end.
	const int writes = spy.writes;
	chip.onMouseUp().callback(at(30.0f, 8.0f));
	CHECK(spy.writes == writes);
	CHECK(spy.ends == 1);
}

TEST_CASE("momentary chip: destroyed while held releases") {
	// visage's `~Frame` drops the pointer capture WITHOUT a mouse-up, so the chip must let go itself.
	ChipSpy spy;
	{
		ui::SwitchChip chip { spy, "Hold" };
		chip.setBounds(0, 0, 60, ui::SwitchChip::kHeight);
		chip.setBehaviour(ui::SwitchChip::Behaviour::Momentary);
		chip.onMouseDown().callback(at(30.0f, 8.0f));
		REQUIRE(spy.v == 3.0);
		REQUIRE(spy.ends == 0);
	}
	CHECK(spy.v == -2.0);
	CHECK(spy.begins == 1);
	CHECK(spy.ends == 1);
}

TEST_CASE("momentary chip: one edit per gesture, and none when it returns to where it started") {
	for (const bool fromMax : { true, false }) {
		CAPTURE(fromMax);
		ChipSpy spy;
		spy.v = fromMax ? 3.0 : -2.0;
		ui::SwitchChip chip { spy, "Hold" };
		chip.setBounds(0, 0, 60, ui::SwitchChip::kHeight);
		chip.setBehaviour(ui::SwitchChip::Behaviour::Momentary);
		int edits = 0;
		double before = 0.0, after = 0.0;
		chip.onEdited([&](double b, double a) {
			++edits;
			before = b;
			after = a;
		});
		chip.onMouseDown().callback(at(30.0f, 8.0f));
		CHECK(edits == 0); // nothing is reported while held
		chip.onMouseUp().callback(at(30.0f, 8.0f));
		if (fromMax) {
			// FOUND AT MAX (a host left it there), released to min: the patch changed, once.
			CHECK(edits == 1);
			CHECK(before == 3.0);
			CHECK(after == -2.0);
		}
		else {
			CHECK(edits == 0); // from rest back to rest is a performance, not an edit
		}
	}
}

// ------------------------------------------------------------------------------------------------
// THE TEXT FIELD — the first place in the plugin a user types. Every event is handed to the frame the
// way `WindowEventHandler` hands it, and in X11's order: `windowing_x11.cpp` delivers the TEXT of a
// keypress before the KEY, and for Return, Escape and Backspace that text is "\r", "\x1b" and "\b".
// So a case that presses Enter first feeds "\r" as text, which is the road a field that inserted
// control characters would have broken.

namespace {

struct FieldSpy {
	int commits = 0;
	int cancels = 0;
	std::string committed;

	void attach(ui::TextField& field) {
		field.onCommit([this](std::string text) {
			++commits;
			committed = std::move(text);
		});
		field.onCancel([this] { ++cancels; });
	}
};

void press(ui::TextField& field, visage::KeyCode code, int modifiers = 0) {
	field.processKeyPress(visage::KeyEvent { code, modifiers, true });
}

/// One keystroke as X11 delivers it: the text, then the key.
void typeKey(ui::TextField& field, const std::string& text, visage::KeyCode code) {
	field.processTextInput(text);
	press(field, code);
}

void typeText(ui::TextField& field, const std::string& text) {
	field.processTextInput(text);
}

} // namespace

TEST_CASE("text field commits on enter") {
	ui::TextField field;
	field.setBounds(0, 0, 80, 16);
	FieldSpy spy;
	spy.attach(field);

	field.begin("Macro 1");
	REQUIRE(field.editing());
	CHECK(field.text() == "Macro 1");

	// BEGIN SELECTS ALL, so the first character REPLACES the name rather than landing after it.
	typeText(field, "L");
	typeText(field, "f");
	typeText(field, "o");
	CHECK(field.text() == "Lfo");
	CHECK(spy.commits == 0);

	typeKey(field, "\r", visage::KeyCode::Return);
	CHECK(spy.commits == 1);
	CHECK(spy.cancels == 0);
	CHECK(spy.committed == "Lfo"); // exactly the typed text: no "\r" slipped in ahead of the key
	CHECK_FALSE(field.editing());

	// A SECOND ENTER after the edit ended reports nothing.
	typeKey(field, "\r", visage::KeyCode::Return);
	CHECK(spy.commits == 1);
}

TEST_CASE("text field cancels on escape") {
	ui::TextField field;
	field.setBounds(0, 0, 80, 16);
	FieldSpy spy;
	spy.attach(field);

	field.begin("Macro 1");
	typeText(field, "x");
	typeKey(field, "\x1b", visage::KeyCode::Escape);
	CHECK(spy.cancels == 1);
	CHECK(spy.commits == 0);
	CHECK_FALSE(field.editing());

	// Nothing after the cancel is an edit: Enter does not commit what was abandoned.
	typeKey(field, "\r", visage::KeyCode::Return);
	CHECK(spy.commits == 0);
	CHECK(spy.cancels == 1);
}

TEST_CASE("text field caps at the name limit") {
	ui::TextField field;
	field.setBounds(0, 0, 80, 16);
	FieldSpy spy;
	spy.attach(field);

	field.begin("");
	for (int i = 0; i < 30; ++i) {
		typeText(field, "a");
	}
	CHECK(field.text() == std::string(device::kMacroNameMax, 'a'));
	CHECK(field.text().size() == 24);

	// A TWO-BYTE CHARACTER AT 23 BYTES does not fit and is refused WHOLE — half of it would be a name
	// that is not UTF-8. A one-byte character after it still fits.
	field.begin(std::string(23, 'a'));
	press(field, visage::KeyCode::End); // collapse the select-all to a caret at the end
	typeText(field, "\xC3\xA9");        // é
	CHECK(field.text() == std::string(23, 'a'));
	typeText(field, "b");
	CHECK(field.text() == std::string(23, 'a') + "b");

	typeKey(field, "\r", visage::KeyCode::Return);
	CHECK(spy.committed.size() == 24);

	// AN OVER-CAP STARTING NAME is clamped the way the name store clamps it, on a code-point boundary.
	field.begin(std::string(23, 'a') + "\xC3\xA9");
	CHECK(field.text() == std::string(23, 'a'));
}

TEST_CASE("backspace removes one code point") {
	ui::TextField field;
	field.setBounds(0, 0, 80, 16);
	FieldSpy spy;
	spy.attach(field);

	field.begin("");
	typeText(field, "a");
	typeText(field, "\xC3\xA9"); // é, two bytes
	REQUIRE(field.text() == "a\xC3\xA9");
	typeKey(field, "\b", visage::KeyCode::Backspace);
	CHECK(field.text() == "a");
	typeKey(field, "\b", visage::KeyCode::Backspace);
	CHECK(field.text().empty());
	typeKey(field, "\b", visage::KeyCode::Backspace); // at the start: nothing to remove, nothing breaks
	CHECK(field.text().empty());

	// BACKSPACE OVER THE SELECT-ALL clears it whole.
	field.begin("Macro 1");
	typeKey(field, "\b", visage::KeyCode::Backspace);
	CHECK(field.text().empty());
	CHECK(spy.commits == 0);
	CHECK(spy.cancels == 0);
}

TEST_CASE("left and right step over a whole code point, and home and end reach the ends") {
	ui::TextField field;
	field.setBounds(0, 0, 80, 16);
	field.begin("a\xC3\xA9"
	            "b"); // a é b
	press(field, visage::KeyCode::End);
	CHECK(field.caretForTest() == 4);
	press(field, visage::KeyCode::Left);
	CHECK(field.caretForTest() == 3);
	press(field, visage::KeyCode::Left); // over é: two bytes in one step
	CHECK(field.caretForTest() == 1);
	typeText(field, "x");
	CHECK(field.text() == "ax\xC3\xA9"
	                      "b");
	press(field, visage::KeyCode::Right); // over é again
	CHECK(field.caretForTest() == 4);
	press(field, visage::KeyCode::Home);
	CHECK(field.caretForTest() == 0);
	typeText(field, "y");
	CHECK(field.text() == "yax\xC3\xA9"
	                      "b");

	// LEFT FROM A SELECT-ALL lands at the start, RIGHT at the end, and neither deletes anything.
	field.begin("abc");
	press(field, visage::KeyCode::Left);
	CHECK(field.caretForTest() == 0);
	CHECK(field.text() == "abc");
	field.begin("abc");
	press(field, visage::KeyCode::Right);
	CHECK(field.caretForTest() == 3);
	CHECK(field.text() == "abc");
}

TEST_CASE("text field swallows the keys it edits with and passes shortcuts on") {
	// KEYS TRAVEL UPWARD from the focus until one is used, and the chain column deletes a device on
	// Backspace. A field that did not claim its own Backspace would delete the device under the name.
	ui::TextField field;
	field.setBounds(0, 0, 80, 16);
	field.begin("abc");
	CHECK(field.processKeyPress(visage::KeyEvent { visage::KeyCode::Backspace, 0, true }));
	CHECK(field.processKeyPress(visage::KeyEvent { visage::KeyCode::Delete, 0, true }));
	CHECK(field.processKeyPress(visage::KeyEvent { visage::KeyCode::A, 0, true }));
	CHECK(field.processKeyPress(visage::KeyEvent { visage::KeyCode::A, visage::kModifierShift, true }));
	// A MAIN-MODIFIER CHORD is the host's (save, undo) — the field has no use for it.
	CHECK_FALSE(field.processKeyPress(visage::KeyEvent { visage::KeyCode::S, visage::kModifierRegCtrl, true }));

	// And a field that is not editing claims nothing.
	FieldSpy spy;
	spy.attach(field);
	typeKey(field, "\x1b", visage::KeyCode::Escape);
	REQUIRE_FALSE(field.editing());
	CHECK_FALSE(field.processKeyPress(visage::KeyEvent { visage::KeyCode::Backspace, 0, true }));
	CHECK_FALSE(field.receivesTextInput());
}

TEST_CASE("text field reads X11's Latin-1 bytes as the characters they name") {
	// `XLookupString` returns ISO Latin-1, and visage passes its bytes on as they came: é arrives as
	// the single byte 0xE9, which is not UTF-8. Stored as it came, the name would not be UTF-8 either.
	ui::TextField field;
	field.setBounds(0, 0, 80, 16);
	field.begin("");
	typeText(field, "\xE9");
	CHECK(field.text() == "\xC3\xA9");
	typeText(field, "\xC3\xA9"); // real UTF-8 is taken as it is
	CHECK(field.text() == "\xC3\xA9\xC3\xA9");
}

TEST_CASE("focus loss cancels") {
	ui::TextField field;
	field.setBounds(0, 0, 80, 16);
	FieldSpy spy;
	spy.attach(field);

	field.begin("Macro 1");
	typeText(field, "Lfo");
	field.processFocusChanged(false, true); // a click somewhere else
	CHECK(spy.cancels == 1);
	CHECK(spy.commits == 0); // NOT a commit: clicking away is not agreeing to the half-typed name
	CHECK_FALSE(field.editing());

	// A focus loss with no edit open reports nothing.
	field.processFocusChanged(false, false);
	CHECK(spy.cancels == 1);
}

TEST_CASE("text field hidden while editing cancels") {
	// A caption swapped for this field has to come back. Hidden without a word, the owner would wait
	// for an answer that never comes.
	ui::TextField field;
	field.setBounds(0, 0, 80, 16);
	FieldSpy spy;
	spy.attach(field);
	field.begin("Macro 1");
	field.setVisible(false);
	CHECK(spy.cancels == 1);
	CHECK(spy.commits == 0);
	CHECK_FALSE(field.editing());
}

TEST_CASE("text field destroyed while editing calls nobody") {
	FieldSpy spy;
	{
		auto field = std::make_unique<ui::TextField>();
		field->setBounds(0, 0, 80, 16);
		spy.attach(*field);
		field->begin("Macro 1");
		typeText(*field, "x");
		field.reset();
	}
	CHECK(spy.commits == 0);
	CHECK(spy.cancels == 0);
}

// ------------------------------------------------------------------------------------------------
// WHILE A FIELD EDITS, NOTHING TYPED REACHES THE DAW (the DAW had kept intercepting keystrokes:
// space toggled play/pause, caps lock played notes). visage's X11 layer sends a key to the host
// exactly when the walk from the focus comes back false — `keyFromFocus` for a press, `keyUpFromFocus`
// for a release — so "reaches the DAW" is read off that answer. At rest the answer must stay false: the
// editor passing keys on is what keeps the DAW's bindings, and losing them would make the plugin unusable
// in a session.

TEST_CASE("text field: while it edits, Space is typed and neither its press nor its release reaches the "
          "host; at rest both do") {
	visage::Frame host; // stands for the rest of the editor, which uses no Space
	host.setBounds(0, 0, 200, 40);
	ui::TextField field;
	host.addChild(&field);
	field.setBounds(0, 0, 80, 16);
	const visage::KeyEvent spaceDown { visage::KeyCode::Space, 0, true };
	const visage::KeyEvent spaceUp { visage::KeyCode::Space, 0, false };
	const visage::KeyEvent capsDown { visage::KeyCode::CapsLock, 0, true };
	const visage::KeyEvent capsUp { visage::KeyCode::CapsLock, 0, false };

	// AT REST, THE HOST'S — the press and the release both go up.
	CHECK_FALSE(keyFromFocus(&field, spaceDown));
	CHECK_FALSE(keyUpFromFocus(&field, spaceUp));

	field.begin("ab");
	// A keystroke as X11 delivers it: the text, then the key, then the release.
	field.processTextInput(" ");
	CHECK(keyFromFocus(&field, spaceDown));
	CHECK(keyUpFromFocus(&field, spaceUp));
	field.processTextInput("x");
	CHECK(field.text() == " x"); // the space was TYPED, replacing the selected name
	CHECK(keyFromFocus(&field, capsDown));
	CHECK(keyUpFromFocus(&field, capsUp));
	// A release whose press came before the edit is still the field's while it edits.
	CHECK(keyUpFromFocus(&field, visage::KeyEvent { visage::KeyCode::A, 0, false }));
	// A CHORD IS THE HOST'S, press and release.
	CHECK_FALSE(keyFromFocus(&field, visage::KeyEvent { visage::KeyCode::S, visage::kModifierRegCtrl, true }));
	CHECK_FALSE(keyUpFromFocus(&field, visage::KeyEvent { visage::KeyCode::S, visage::kModifierRegCtrl, false }));

	// ENTER ENDS THE EDIT ON ITS PRESS, and its release — half of a keystroke the host never saw the
	// start of — is the field's too.
	field.processTextInput("\r");
	CHECK(keyFromFocus(&field, visage::KeyEvent { visage::KeyCode::Return, 0, true }));
	REQUIRE_FALSE(field.editing());
	CHECK(keyUpFromFocus(&field, visage::KeyEvent { visage::KeyCode::Return, 0, false }));

	// AND THE HOST HAS ITS KEYS BACK.
	CHECK_FALSE(keyFromFocus(&field, spaceDown));
	CHECK_FALSE(keyUpFromFocus(&field, spaceUp));
	CHECK_FALSE(keyFromFocus(&field, capsDown));
	CHECK_FALSE(keyUpFromFocus(&field, capsUp));
	CHECK(field.text() == " x");
}

TEST_CASE("text field takes the keyboard for exactly the edit: given back on commit, escape, click-away, "
          "hide, begin-over-begin and destruction") {
	ClaimCounter claims;
	const ui::ScopedKeyboardClaimBackend scoped(claims);
	ui::TextField field;
	field.setBounds(0, 0, 80, 16);
	CHECK(claims.takes == 0); // AT REST, NOTHING IS TAKEN

	const auto check = [&](int takes, int outstanding) {
		CHECK(claims.takes == takes);
		CHECK(claims.outstanding() == outstanding);
	};

	field.begin("a");
	check(1, 1);
	typeKey(field, "\r", visage::KeyCode::Return); // commit
	check(1, 0);

	field.begin("a");
	check(2, 1);
	typeKey(field, "\x1b", visage::KeyCode::Escape); // cancel
	check(2, 0);

	field.begin("a");
	field.processFocusChanged(false, true); // a click somewhere else
	check(3, 0);

	field.begin("a");
	field.setVisible(false); // hidden mid-edit
	check(4, 0);
	field.setVisible(true);

	field.begin("a");
	field.begin("b"); // over an open edit: the new claim, then the old one goes
	check(6, 1);
	typeKey(field, "\r", visage::KeyCode::Return);
	check(6, 0);

	{
		auto doomed = std::make_unique<ui::TextField>();
		doomed->setBounds(0, 0, 80, 16);
		doomed->begin("gone");
		check(7, 1);
		doomed.reset(); // DESTROYED MID-EDIT — the exit no callback marks
	}
	check(7, 0);
}

TEST_CASE("an owner that hides the field from inside onCommit or onCancel leaves it idle") {
	// **THE OWNER HIDES; IT NEVER DESTROYS.** visage's own frames are beneath these callbacks on the
	// stack (`CallbackList::callback` runs the frame's function in place; `setVisible` goes on using
	// the frame after `visibilityChanged`), so hiding is the only thing an owner may do from inside —
	// and the case pins that hiding from inside reports nothing twice.
	for (const bool viaEnter : { true, false }) {
		CAPTURE(viaEnter);
		ui::TextField field;
		field.setBounds(0, 0, 80, 16);
		int commits = 0;
		int cancels = 0;
		std::string committed;
		field.onCommit([&](std::string text) {
			++commits;
			committed = std::move(text);
			field.setVisible(false);
		});
		field.onCancel([&] {
			++cancels;
			field.setVisible(false);
		});
		field.begin("Macro 1");
		typeText(field, "ok");
		press(field, viaEnter ? visage::KeyCode::Return : visage::KeyCode::Escape);
		CHECK(commits == (viaEnter ? 1 : 0));
		CHECK(cancels == (viaEnter ? 0 : 1));
		if (viaEnter) {
			CHECK(committed == "ok");
		}
		CHECK_FALSE(field.editing());
		CHECK_FALSE(field.isVisible());
		CHECK_FALSE(field.receivesTextInput());
		// IDLE: nothing that follows is an edit.
		typeText(field, "x");
		press(field, visage::KeyCode::Return);
		field.processFocusChanged(false, false);
		CHECK(field.text() == "ok"); // the typing after the edit ended changed nothing
		CHECK(commits == (viaEnter ? 1 : 0));
		CHECK(cancels == (viaEnter ? 0 : 1));
	}
}

TEST_CASE("a field hidden mid-edit whose owner hides it again from onCancel cancels once") {
	// **THE HIDE PATH, RE-ENTERED.** `setVisible(false)` → `visibilityChanged` → cancel → the owner
	// calls `setVisible(false)` from inside, while the outer `setVisible` is still on the stack and
	// will go on to touch the frame when it returns. The sanitizer gate reads this one: the frame must
	// still be alive for the outer call, and the inner one must find no edit left to cancel.
	ui::TextField field;
	field.setBounds(0, 0, 80, 16);
	int cancels = 0;
	int commits = 0;
	field.onCommit([&](std::string) { ++commits; });
	field.onCancel([&] {
		++cancels;
		field.setVisible(false);
		field.setVisible(true); // an owner that restores then re-hides is still only one cancel
		field.setVisible(false);
	});
	field.begin("Macro 1");
	field.setVisible(false);
	CHECK(cancels == 1);
	CHECK(commits == 0);
	CHECK_FALSE(field.editing());
	CHECK_FALSE(field.isVisible());

	// And the field is reusable afterwards: shown, begun, committed.
	field.setVisible(true);
	field.begin("Macro 1");
	typeText(field, "again");
	press(field, visage::KeyCode::Return);
	CHECK(commits == 1);
	CHECK(cancels == 1);
}

TEST_CASE("an overlong or surrogate spelling is not stored as it came") {
	// **VALID-LOOKING LEAD, ILLEGAL SECOND BYTE.** E0 80 80 is an overlong NUL and ED A0 80 a UTF-16
	// surrogate — both have a lead byte that can start a real three-byte character, so a validator that
	// read the lead alone would store them. They are not UTF-8, so the Latin-1 reading takes them: each
	// byte becomes the character it names, and the name stays valid.
	ui::TextField field;
	field.setBounds(0, 0, 80, 16);

	field.begin("");
	typeText(field, "\xE0\x80\x80");
	CHECK(field.text() != "\xE0\x80\x80");
	CHECK(field.text() == "\xC3\xA0\xC2\x80\xC2\x80"); // à U+0080 U+0080

	field.begin("");
	typeText(field, "\xED\xA0\x80");
	CHECK(field.text() != "\xED\xA0\x80");
	CHECK(field.text() == "\xC3\xAD\xC2\xA0\xC2\x80"); // í U+00A0 U+0080

	// F0 80 80 80 is an overlong four-byte form.
	field.begin("");
	typeText(field, "\xF0\x80\x80\x80");
	CHECK(field.text() == "\xC3\xB0\xC2\x80\xC2\x80\xC2\x80");

	// **PAST U+10FFFF, TWO WAYS.** F4 90 80 80 has the last legal lead with a second byte one past its
	// range (U+110000); F5 is a lead that can only ever begin a code point past the end. Neither is
	// UTF-8, so both are read as Latin-1, byte by byte.
	field.begin("");
	typeText(field, "\xF4\x90\x80\x80");
	CHECK(field.text() != "\xF4\x90\x80\x80");
	CHECK(field.text() == "\xC3\xB4\xC2\x90\xC2\x80\xC2\x80"); // ô U+0090 U+0080 U+0080
	field.begin("");
	typeText(field, "\xF5\x80\x80\x80");
	CHECK(field.text() != "\xF5\x80\x80\x80");
	CHECK(field.text() == "\xC3\xB5\xC2\x80\xC2\x80\xC2\x80"); // õ U+0080 U+0080 U+0080
	// AND THE LAST LEGAL CODE POINT, U+10FFFF (F4 8F BF BF), is taken as it is.
	field.begin("");
	typeText(field, "\xF4\x8F\xBF\xBF");
	CHECK(field.text() == "\xF4\x8F\xBF\xBF");

	// The legal neighbours still pass through untouched: U+0800 (E0 A0 80) and U+D7FF (ED 9F BF).
	field.begin("");
	typeText(field, "\xE0\xA0\x80");
	typeText(field, "\xED\x9F\xBF");
	CHECK(field.text() == "\xE0\xA0\x80\xED\x9F\xBF");
}

namespace {

/// Render a field into a marked windowless capture of its own size, and hand the capture back.
visage::Screenshot captureField(ui::TextField& field, int w, int h) {
	visage::Canvas canvas;
	markedWindowless(canvas, w, h);
	canvas.setColor(0xff000000);
	canvas.fill(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
	visage::Region region;
	region.setBounds(0, 0, w, h);
	canvas.addRegion(&region);
	canvas.beginRegion(&region);
	field.draw(canvas);
	canvas.endRegion();
	return takeMarkedCapture(canvas, w, h);
}

int peakOf(const visage::Screenshot& shot, int x, int y) {
	return std::max({ static_cast<int>(shot.sample(x, y).hexRed()),
	                  static_cast<int>(shot.sample(x, y).hexGreen()),
	                  static_cast<int>(shot.sample(x, y).hexBlue()) });
}

/// Bright pixels in columns [x0, x1) over the interior rows, clear of the border and its corners.
int inkIn(const visage::Screenshot& shot, int x0, int x1, int h, int ground) {
	int count = 0;
	for (int y = 3; y < h - 3; ++y) {
		for (int x = x0; x < x1; ++x) {
			count += peakOf(shot, x, y) > ground + 40 ? 1 : 0;
		}
	}
	return count;
}

} // namespace

TEST_CASE("a text field draws its text from the left edge and nothing past it") {
	// **RENDER GEOMETRY BEFORE BELIEVING IT.** Every case above is about the string; none of them says
	// a glyph reaches the screen, or that it lands where the caption it replaces would be read.
	constexpr int kW = 80;
	constexpr int kH = 16;
	ui::TextField field;
	field.setBounds(0, 0, kW, kH);

	// **THE CARET ALONE, FIRST**, so "the text drew" is measured against what an EMPTY field paints —
	// a field that drew only its caret must not pass for one that drew two glyphs.
	field.begin("");
	const visage::Screenshot caretOnly = captureField(field, kW, kH);
	REQUIRE(caretOnly.width() == kW);
	const int caretGround = peakOf(caretOnly, kW - 6, kH / 2);
	const int caretInk = inkIn(caretOnly, 2, 24, kH, caretGround);

	field.begin("ab");
	field.processKeyPress(visage::KeyEvent { visage::KeyCode::End, 0, true }); // a caret, no selection
	const visage::Screenshot shot = captureField(field, kW, kH);
	REQUIRE(shot.width() == kW);
	// Ink is anything well above the field's own ground, read from a column the text cannot reach.
	const int ground = peakOf(shot, kW - 6, kH / 2);
	const int textInk = inkIn(shot, 2, 24, kH, ground);
	const int emptyInk = inkIn(shot, 32, kW - 3, kH, ground);
	CAPTURE(ground);
	CAPTURE(caretInk);
	CAPTURE(textInk);
	CAPTURE(emptyInk);
	CHECK(caretInk > 0);                // the caret is drawn and is visible to this instrument
	CHECK(textInk >= caretInk + 20);    // and two glyphs are a good deal more than the caret alone
	CHECK(textInk >= 3 * caretInk);
	CHECK(emptyInk == 0);               // and nothing where there is no text
}

TEST_CASE("a name longer than the field slides so the caret stays in view") {
	constexpr int kW = 40;
	constexpr int kH = 16;
	ui::TextField field;
	field.setBounds(0, 0, kW, kH);
	field.begin(std::string(24, 'M'));
	field.processKeyPress(visage::KeyEvent { visage::KeyCode::End, 0, true });
	const visage::Screenshot shot = captureField(field, kW, kH);
	REQUIRE(shot.width() == kW);
	// Twenty-four capitals at the caption size are far wider than 40 px, so the end is only in view
	// if the text moved left.
	CHECK(field.scrollForTest() > 0.0f);

	// **AND WHAT SLID OFF IS CUT AT THE PAD**, not painted over it: columns 1 and 2 — inside the border,
	// left of the text's clip — carry no glyph ink even though a glyph straddles them.
	// THE GROUND, read from row 1. Row 1 is INSIDE the text's clip — the clip is `trimClampBounds(kPadX,
	// 1, ...)`, so only row 0, the border, is outside it vertically — and it is ground because no glyph
	// at the caption size reaches that high in a 16px field, not because anything cut it.
	const int ground = peakOf(shot, kW / 2, 1);
	CAPTURE(ground);
	CHECK(inkIn(shot, 1, 3, kH, ground) == 0);
	CHECK(inkIn(shot, kW - 3, kW - 1, kH, ground) == 0);
	CHECK(inkIn(shot, 3, kW - 3, kH, ground) > 0); // while the interior is full of text

	field.processKeyPress(visage::KeyEvent { visage::KeyCode::Home, 0, true });
	(void)captureField(field, kW, kH);
	CHECK(field.scrollForTest() == 0.0f); // and back to the start when the caret is
}

// ------------------------------------------------------------------------------------------------
// THE MACRO SLOT (A7) — one macro as a knob, a latch or a momentary, renamed in place. The header row
// cases go through `ModFixture`, so the bridge, the view and the menu are the ones the editor builds;
// the lifetime cases stand a slot up on its own, where hiding and destroying it is the case's to do.

namespace {

/// Press and release the chip where a hand would, on its face.
void clickChip(ui::SwitchChip& chip) {
	const float x = chip.width() * 0.5f;
	const float y = chip.height() * 0.5f;
	chip.onMouseDown().callback(at(x, y));
	chip.onMouseUp().callback(at(x, y));
}

double macroValue(ParamBridge& bridge, size_t macro) {
	return bridge.value(static_cast<clap_id>(macroParam(macro)));
}

/// A SLOT OVER PLAIN STATE — the type, the name and the rename verb are fields the case owns, so a
/// case can hide or destroy the slot with nothing else in the way.
struct SlotProbe {
	device::MacroType type = device::MacroType::Knob;
	std::string name = "Macro 1";
	bool renamed = false;
	int renames = 0;

	ui::MacroSlot::Naming naming() {
		return {
			.name = [this] { return name; },
			.renamed = [this] { return renamed; },
			.rename =
			    [this](const std::string& text) {
				    ++renames;
				    name = text;
				    renamed = true;
			    },
		};
	}
	ui::MacroMenuHooks hooks() {
		return {
			.readBipolar = [] { return false; },
			.writeBipolar = [](bool) {},
			.readType = [this] { return type; },
			.writeType = [this](device::MacroType t) { type = t; },
			.beginRename = [] {},
		};
	}
};

std::unique_ptr<ui::MacroSlot> probeSlot(device::Modulatable& control, SlotProbe& probe) {
	auto slot = std::make_unique<ui::MacroSlot>(control, "M1", probe.naming(), probe.hooks());
	slot->setBounds(0.0f, 0.0f, 54.0f, ui::MacroSlot::columnHeight(36.0f));
	slot->setFaceDiameter(36.0f);
	return slot;
}

} // namespace

TEST_CASE("a macro set to Latch shows a chip and the chip writes the macro param") {
	ModFixture f;
	ui::MacroSlot* slot = f.panel->macroSlotForTest(0);
	REQUIRE(slot != nullptr);
	// A FRESH MACRO IS A KNOB: the knob on show, the chip built and hidden.
	CHECK(slot->knob().isVisible());
	CHECK_FALSE(slot->chip().isVisible());

	ui::chooseMacroMenuEntry(slot->menuHooks(), ui::kMacroMenuLatch);
	CHECK(f.bridge.macroType(0) == device::MacroType::Latch);
	// RE-SEATED AT THE PICK, not a tick later.
	CHECK_FALSE(slot->knob().isVisible());
	CHECK(slot->chip().isVisible());
	CHECK(slot->chip().behaviour() == ui::SwitchChip::Behaviour::Latch);
	// **A PAD IN THE FACE**: the chip takes the knob's whole column, and its pad is a
	// square of the face's side centred where the knob's face is centred — half the width across, half
	// the diameter down (`Knob::draw`).
	CHECK(slot->chip().presentation() == ui::SwitchChip::Presentation::Pad);
	CHECK(slot->chip().bounds() == slot->knob().bounds());
	const float face = slot->knob().faceDiameter();
	REQUIRE(face == doctest::Approx(ui::ModPanel::kMacroDiameter));
	const visage::Bounds pad = slot->chip().padBounds();
	CHECK(pad.width() == doctest::Approx(face));
	CHECK(pad.height() == doctest::Approx(face));
	CHECK(pad.x() + pad.width() * 0.5f == doctest::Approx(slot->width() * 0.5f));
	CHECK(pad.y() + pad.height() * 0.5f == doctest::Approx(face * 0.5f));
	// NO TEXT ON THE PAD; the caption and the reading are under it, and the reading says OFF at rest.
	CHECK(slot->chip().label() == "M1");
	CHECK(slot->chip().readout() == "OFF");
	// AND THE TYPE SWITCH WROTE NOTHING to the parameter.
	CHECK(drain(f.bridge).empty());
	// A PRESS ANYWHERE IN THE COLUMN FINDS THE PAD'S CHIP, as one anywhere finds a knob.
	CHECK(slot->frameAtPoint({ pad.x() + pad.width() * 0.5f, pad.y() + pad.height() * 0.5f }) == &slot->chip());
	CHECK(slot->frameAtPoint({ slot->width() * 0.5f, slot->height() - 4.0f }) == &slot->chip());

	const ParamDesc& macro = descOf(ParamId::Macro1);
	REQUIRE(macroValue(f.bridge, 0) < (macro.min + macro.max) * 0.5);
	clickChip(slot->chip());
	const std::vector<UiParamEvent> events = drain(f.bridge);
	CHECK(macroValue(f.bridge, 0) == doctest::Approx(macro.max));
	CHECK(slot->chip().readout() == "ON");
	// ONE GESTURE AROUND ONE VALUE, on this macro's parameter.
	REQUIRE(events.size() == 3);
	CHECK(events[0].kind == UiParamEvent::Kind::GestureBegin);
	CHECK(events[1].kind == UiParamEvent::Kind::Value);
	CHECK(events[1].id == static_cast<clap_id>(macroParam(0)));
	CHECK(events[1].value == doctest::Approx(macro.max));
	CHECK(events[2].kind == UiParamEvent::Kind::GestureEnd);

	// PRESSED AGAIN, THE LATCH LETS GO.
	clickChip(slot->chip());
	(void)drain(f.bridge);
	CHECK(macroValue(f.bridge, 0) == doctest::Approx(macro.min));
}

TEST_CASE("a bipolar latch publishes -1/+1") {
	// **THE ENGINE'S NUMBER, NOT THE WIDGET'S.** A latch on a bipolar macro writes the VIEW's ends,
	// which the view maps to the parameter's 0 and 1; what a route reads is the core source cell, which
	// `ParamSet::publishCoreSources` fills with `2v - 1` for a macro whose bit is set. So the case hands
	// the parameter the chip left and the mask the bridge holds to a real ParamSet — the two lines
	// `publishEditorLanes` and the engine's block run — and reads the cell.
	ModFixture f;
	ui::MacroSlot* slot = f.panel->macroSlotForTest(1);
	REQUIRE(slot != nullptr);
	ui::chooseMacroMenuEntry(slot->menuHooks(), ui::kMacroMenuBipolar);
	ui::chooseMacroMenuEntry(slot->menuHooks(), ui::kMacroMenuLatch);
	REQUIRE(f.bridge.macroBipolar(1));
	REQUIRE(slot->chip().isVisible());

	const auto published = [&f] {
		ParamSet params;
		params.setBase(static_cast<clap_id>(macroParam(1)), macroValue(f.bridge, 1));
		params.setMacroBipolarMask(f.bridge.macroBipolarMask());
		params.publishCoreSources();
		return params.coreSourceCells()[1];
	};

	// AT REST, UNLIT: the parameter at 0, which bipolar publishes as -1 — the latch's off end.
	REQUIRE_FALSE(slot->chip().on());
	CHECK(published() == doctest::Approx(-1.0f));

	clickChip(slot->chip());
	(void)drain(f.bridge);
	CHECK(macroValue(f.bridge, 1) == doctest::Approx(1.0)); // the parameter keeps its 0..1 shape
	CHECK(published() == doctest::Approx(1.0f));

	clickChip(slot->chip());
	(void)drain(f.bridge);
	CHECK(macroValue(f.bridge, 1) == doctest::Approx(0.0));
	CHECK(published() == doctest::Approx(-1.0f));
	// NEVER THE MIDDLE: a latch has two positions, and neither is the bipolar rest.
}

TEST_CASE("momentary macro: press max, release min") {
	ModFixture f;
	ui::MacroSlot* slot = f.panel->macroSlotForTest(3);
	REQUIRE(slot != nullptr);
	ui::chooseMacroMenuEntry(slot->menuHooks(), ui::kMacroMenuMomentary);
	REQUIRE(slot->chip().isVisible());
	CHECK(slot->chip().behaviour() == ui::SwitchChip::Behaviour::Momentary);
	const ParamDesc& macro = descOf(ParamId::Macro4);
	const float x = slot->chip().width() * 0.5f;

	slot->chip().onMouseDown().callback(at(x, 8.0f));
	(void)drain(f.bridge);
	CHECK(macroValue(f.bridge, 3) == doctest::Approx(macro.max));
	slot->chip().onMouseUp().callback(at(x, 8.0f));
	(void)drain(f.bridge);
	CHECK(macroValue(f.bridge, 3) == doctest::Approx(macro.min));
}

TEST_CASE("switching back to Knob keeps the value") {
	ModFixture f;
	ui::MacroSlot* slot = f.panel->macroSlotForTest(0);
	REQUIRE(slot != nullptr);
	ui::chooseMacroMenuEntry(slot->menuHooks(), ui::kMacroMenuLatch);
	clickChip(slot->chip());
	(void)drain(f.bridge);
	REQUIRE(macroValue(f.bridge, 0) == doctest::Approx(1.0));

	ui::chooseMacroMenuEntry(slot->menuHooks(), ui::kMacroMenuKnob);
	CHECK(slot->knob().isVisible());
	CHECK_FALSE(slot->chip().isVisible());
	// NOTHING WRITTEN by the switch, and the knob reads the value the latch left.
	CHECK(drain(f.bridge).empty());
	CHECK(macroValue(f.bridge, 0) == doctest::Approx(1.0));
	CHECK(slot->knob().control()->value() == doctest::Approx(1.0));
}

TEST_CASE("rename commits to the bridge and the caption reads the name") {
	ModFixture f;
	ui::MacroSlot* slot = f.panel->macroSlotForTest(2);
	REQUIRE(slot != nullptr);
	CHECK(slot->caption() == "M3");
	CHECK(std::string(slot->knob().label()) == "M3");

	ui::chooseMacroMenuEntry(slot->menuHooks(), ui::kMacroMenuRename);
	REQUIRE(slot->renaming());
	CHECK(slot->field().isVisible());
	CHECK(slot->field().text() == "Macro 3"); // the name, not the short caption
	// OVER THE CAPTION LINE, inside the column.
	CHECK(slot->field().y() > slot->knob().faceDiameter() - 1.0f);
	CHECK(slot->field().y() + slot->field().height() <= slot->height());

	typeText(slot->field(), "Wobble");
	typeKey(slot->field(), "\r", visage::KeyCode::Return);
	CHECK(f.bridge.macroName(2) == "Wobble");
	CHECK(f.bridge.macroRenamed(2));
	CHECK_FALSE(f.bridge.macroRenamed(1));
	CHECK_FALSE(slot->renaming());
	CHECK_FALSE(slot->field().isVisible());
	CHECK(slot->caption() == "Wobble");
	CHECK(std::string(slot->knob().label()) == "Wobble");
	CHECK(slot->chip().label() == "Wobble");

	// AND A PAD CARRIES THE NAME TOO, under it; the field opens over the same caption line as a knob's.
	const float knobFieldY = slot->field().y();
	ui::chooseMacroMenuEntry(slot->menuHooks(), ui::kMacroMenuLatch);
	CHECK(slot->chip().caption() == "Wobble");
	ui::chooseMacroMenuEntry(slot->menuHooks(), ui::kMacroMenuRename);
	CHECK(slot->field().text() == "Wobble");
	CHECK(slot->field().y() == doctest::Approx(knobFieldY));
	CHECK(slot->field().y() > slot->chip().padBounds().bottom() - 1.0f);
	// CLEARED BACK TO THE DEFAULT, the short caption returns.
	typeKey(slot->field(), "\b", visage::KeyCode::Backspace);
	typeKey(slot->field(), "\r", visage::KeyCode::Return);
	CHECK_FALSE(f.bridge.macroRenamed(2));
	CHECK(slot->caption() == "M3");
	CHECK(slot->chip().label() == "M3");
}

TEST_CASE("escape leaves the name unchanged") {
	ModFixture f;
	ui::MacroSlot* slot = f.panel->macroSlotForTest(1);
	REQUIRE(slot != nullptr);
	(void)f.bridge.takeStateDirty();
	slot->beginRename();
	REQUIRE(slot->renaming());
	typeText(slot->field(), "Nope");
	typeKey(slot->field(), "\x1b", visage::KeyCode::Escape);
	CHECK_FALSE(slot->renaming());
	CHECK_FALSE(slot->field().isVisible());
	CHECK_FALSE(f.bridge.macroRenamed(1));
	CHECK(f.bridge.macroName(1) == "Macro 2");
	CHECK(slot->caption() == "M2");
	CHECK_FALSE(f.bridge.takeStateDirty()); // not an edit
}

TEST_CASE("a patch restore relabels the slot") {
	ModFixture f;
	ui::MacroSlot* slot = f.panel->macroSlotForTest(1);
	REQUIRE(slot != nullptr);
	REQUIRE(slot->knob().isVisible());

	MacroModes modes;
	modes.name[1] = "Lead";
	modes.type[1] = device::MacroType::Momentary;
	f.bridge.restoreMacroModes(modes);
	// THE NAME AT ONCE — the bridge's rename listener.
	CHECK(slot->caption() == "Lead");
	CHECK(slot->chip().label() == "Lead");
	// THE TYPE ON THE EDITOR'S NEXT TICK — no listener reports a type, and the restore set the repaint
	// flag the tick drains.
	(void)f.gui.tickForTest();
	CHECK_FALSE(slot->knob().isVisible());
	CHECK(slot->chip().isVisible());
	CHECK(slot->chip().behaviour() == ui::SwitchChip::Behaviour::Momentary);
	// THE OTHER MACROS ARE WHAT THEY WERE.
	CHECK(f.panel->macroSlotForTest(0)->knob().isVisible());
	CHECK(f.panel->macroSlotForTest(0)->caption() == "M1");

	// **A RESTORE THAT MOVES A TYPE AND NO NAME** — the one the rename listener never hears, so the
	// tick is the only road. (The restore above changed the name too, and the listener's full refresh
	// re-seated the control on its own: a known-bad deleting the tick's sync stayed green against it.)
	modes.type[1] = device::MacroType::Latch;
	f.bridge.restoreMacroModes(modes);
	CHECK(slot->chip().behaviour() == ui::SwitchChip::Behaviour::Momentary); // nothing has told it yet
	// THE TICK REPORTS WHAT IT RE-SEATED: one slot, this one — and a tick with nothing moved reports none.
	CHECK(f.gui.tickForTest().macroSlots == 1);
	CHECK(slot->chip().behaviour() == ui::SwitchChip::Behaviour::Latch);
	CHECK(f.gui.tickForTest().macroSlots == 0);
	CHECK(slot->type() == device::MacroType::Latch);
	modes.type[1] = device::MacroType::Knob;
	f.bridge.restoreMacroModes(modes);
	(void)f.gui.tickForTest();
	CHECK(slot->knob().isVisible());
	CHECK_FALSE(slot->chip().isVisible());
	CHECK(slot->caption() == "Lead");

	// AND BACK: a restore of the defaults puts the knob and the short caption back.
	f.bridge.restoreMacroModes(MacroModes {});
	(void)f.gui.tickForTest();
	CHECK(slot->caption() == "M2");
	CHECK(slot->knob().isVisible());
	CHECK_FALSE(slot->chip().isVisible());
}

TEST_CASE("hiding the slot mid-rename cancels") {
	ChipSpy spy;
	SlotProbe probe;
	auto slot = probeSlot(spy, probe);
	slot->beginRename();
	REQUIRE(slot->renaming());
	typeText(slot->field(), "Half");
	slot->setVisible(false);
	CHECK_FALSE(slot->renaming());
	CHECK_FALSE(slot->field().isVisible());
	CHECK(probe.renames == 0);
	CHECK(probe.name == "Macro 1");
	// A hidden slot opens no edit at all.
	slot->beginRename();
	CHECK_FALSE(slot->renaming());
	// Shown again, it renames as ever.
	slot->setVisible(true);
	slot->beginRename();
	typeText(slot->field(), "Back");
	typeKey(slot->field(), "\r", visage::KeyCode::Return);
	CHECK(probe.renames == 1);
	CHECK(slot->caption() == "Back");
}

TEST_CASE("destroying the slot mid-rename is clean") {
	// THE SANITIZER GATE READS THIS ONE: the field dies with an edit open inside a slot that is dying,
	// and nothing may call back into either.
	ChipSpy spy;
	SlotProbe probe;
	auto slot = probeSlot(spy, probe);
	// A MENU OPENED BEFORE THE SLOT WENT holds a copy of its hooks; keep one to pick from afterwards.
	const ui::MacroMenuHooks survivor = slot->menuHooks();
	slot->beginRename();
	typeText(slot->field(), "Gone");
	slot.reset();
	CHECK(probe.renames == 0);
	CHECK(probe.name == "Macro 1");
	// A PICK FROM THE ORPHANED MENU reaches the owner's verb and no slot.
	ui::chooseMacroMenuEntry(survivor, ui::kMacroMenuRename);
	ui::chooseMacroMenuEntry(survivor, ui::kMacroMenuLatch);
	CHECK(probe.type == device::MacroType::Latch);
	CHECK(spy.writes == 0);
}

TEST_CASE("a macro slot destroyed mid-rename gives the keyboard back") {
	// THE OWNER GOING, NOT THE FIELD ALONE — the panel closed under an open rename (the popup-crash
	// lesson: destruction mid-edit is where a claim would leak and leave the DAW deaf).
	ClaimCounter claims;
	const ui::ScopedKeyboardClaimBackend scoped(claims);
	ChipSpy spy;
	SlotProbe probe;
	auto slot = probeSlot(spy, probe);
	slot->beginRename();
	REQUIRE(slot->renaming());
	CHECK(claims.outstanding() == 1);
	typeText(slot->field(), "Half");
	slot.reset();
	CHECK(claims.takes == 1);
	CHECK(claims.outstanding() == 0);
}

TEST_CASE("hiding the slot while a momentary is held releases it") {
	// **THE CHIP IS NOT TOLD WHEN ITS PARENT HIDES** — visage tells only the frame whose own flag moved
	// — so the slot lets go for it. A macro held at max behind a column nobody can see is a stuck note.
	ChipSpy spy;
	SlotProbe probe;
	probe.type = device::MacroType::Momentary;
	auto slot = probeSlot(spy, probe);
	REQUIRE(slot->chip().isVisible());
	slot->chip().onMouseDown().callback(at(20.0f, 8.0f));
	REQUIRE(spy.v == 3.0);
	REQUIRE(spy.begins == 1);

	slot->setVisible(false);
	CHECK(spy.v == -2.0);
	CHECK(spy.ends == 1);
	// AND THE RELEASE THAT FOLLOWS finds nothing held: no second write, no second end.
	const int writes = spy.writes;
	slot->chip().onMouseUp().callback(at(20.0f, 8.0f));
	CHECK(spy.writes == writes);
	CHECK(spy.ends == 1);
}

TEST_CASE("a momentary held when its type switches lets go, and a latch does not move") {
	ChipSpy spy;
	SlotProbe probe;
	probe.type = device::MacroType::Momentary;
	auto slot = probeSlot(spy, probe);
	slot->chip().onMouseDown().callback(at(20.0f, 8.0f));
	REQUIRE(spy.v == 3.0);
	// TO KNOB: the chip is hidden, and hiding it lets go.
	ui::chooseMacroMenuEntry(slot->menuHooks(), ui::kMacroMenuKnob);
	CHECK(spy.v == -2.0);
	CHECK(spy.ends == 1);

	// A LATCH LEFT ON stays on through a switch to Knob and back: a type switch writes nothing.
	ui::chooseMacroMenuEntry(slot->menuHooks(), ui::kMacroMenuLatch);
	clickChip(slot->chip());
	REQUIRE(spy.v == 3.0);
	const int writes = spy.writes;
	ui::chooseMacroMenuEntry(slot->menuHooks(), ui::kMacroMenuKnob);
	ui::chooseMacroMenuEntry(slot->menuHooks(), ui::kMacroMenuMomentary);
	CHECK(spy.writes == writes);
	CHECK(spy.v == 3.0);
}

TEST_CASE("a caption wider than its column ends in an ellipsis inside the column") {
	// **CUT, SAID, AND KEPT INSIDE THE COLUMN** — a 24-byte macro name in the header's 54px column. It
	// used to be drawn left-aligned and clipped at the column's edge, which kept the start of the word
	// but read as a whole short word; the cut now ends in "..." and the fitted text is what is drawn.
	const std::string longName = "Resonance Sweep Amount 1";
	REQUIRE(longName.size() == 24);
	const visage::Font& face = ui::font(ui::kKnobLabelSize);
	constexpr float kColumn = 54.0f;
	REQUIRE(ui::trackedWidth(face, ui::upper(longName.c_str()), ui::kKnobLabelTracking) > kColumn);

	const std::string fitted = ui::Knob::columnCaption(longName.c_str(), kColumn);
	CAPTURE(fitted);
	REQUIRE(fitted.size() > 3);
	CHECK(fitted.substr(fitted.size() - 3) == "...");
	CHECK(fitted.rfind("RES", 0) == 0); // the START of the name is what is kept
	// FITTED TO THE COLUMN LESS A MARGIN EACH SIDE, so two cut neighbours never touch. The margin is
	// pinned by value as well as by name: a margin quietly set to zero would pass the bound below.
	CHECK(ui::Knob::kCaptionMargin == 3.0f);
	constexpr float kRoom = kColumn - 2.0f * ui::Knob::kCaptionMargin;
	CHECK(ui::trackedWidth(face, fitted, ui::kKnobLabelTracking) <= kRoom);
	// THE LONGEST START THAT FITS: one more character would not.
	const std::string upperName = ui::upper(longName.c_str());
	const size_t kept = fitted.size() - 3;
	CHECK(ui::trackedWidth(face, upperName.substr(0, kept + 1) + "...", ui::kKnobLabelTracking) > kRoom);
	// A CAPTION THAT FITS IS LEFT ALONE — no dots on "M1".
	CHECK(ui::Knob::columnCaption("M1", kColumn) == "M1");

	// AND A SLOT'S CAPTION IS A NAME (backlog OT), fitted by `fitName` as typed: the knob and the pad agree.
	ChipSpy spy;
	SlotProbe probe;
	probe.name = longName;
	probe.renamed = true;
	auto slot = probeSlot(spy, probe);
	slot->refresh();
	REQUIRE(slot->caption() == longName);
	REQUIRE(slot->knob().width() == doctest::Approx(kColumn));
	const ui::FittedName name = ui::Knob::columnName(longName.c_str(), kColumn);
	CHECK(slot->knob().drawnCaption() == name.text);
	CHECK(slot->chip().drawnCaption() == name.text);
	CHECK(slot->knob().drawnCaptionSize() == name.size);
	CHECK(slot->chip().drawnCaptionSize() == name.size);
	// A short name in the same column prints whole, AS TYPED and at the default size.
	probe.name = "Wob";
	slot->refresh();
	CHECK(slot->knob().drawnCaption() == "Wob");
	CHECK(slot->chip().drawnCaption() == "Wob");
	CHECK(slot->knob().drawnCaptionSize() == ui::kKnobLabelSize);
}

TEST_CASE("OT: a macro name shows as typed, shrinks to fit down to a floor, and is cut only below it") {
	// **THE VALUE OF THE FIT, NOT A RENDER DIFF** (backlog OT). One helper, `fitName`, behind every place a macro name is
	// drawn: the size it picks, the width that size gives, and where it gives up.
	constexpr float kRoom = 48.0f;
	constexpr float kDefault = ui::kKnobLabelSize;
	constexpr float kTrack = ui::kKnobLabelTracking;
	const auto widthAt = [&](const std::string& text, float size) {
		return ui::trackedWidth(ui::font(size), text, kTrack);
	};
	REQUIRE(ui::kNameFitFloorSize < kDefault);

	// A SHORT NAME keeps the default size, uncut, with its capitals as typed.
	const ui::FittedName shortName = ui::fitName("Wobble", kRoom, kDefault, ui::kNameFitFloorSize, kTrack);
	CHECK(shortName.text == "Wobble");
	CHECK(shortName.size == kDefault);
	CHECK_FALSE(shortName.elided);

	// A NAME THAT CLIPS AT THE DEFAULT BUT FITS BELOW IT: found by walking the sizes with the same measurement, then
	// asserted — the chosen size is below the default, at or above the floor, the LARGEST that fits, and the text whole.
	std::string mid;
	for (const char* word : { "Filter", "Cutoff", "Sweep", "Amount", "Depth", "Rate" }) {
		mid += mid.empty() ? word : std::string(" ") + word;
		if (widthAt(mid, kDefault) > kRoom && widthAt(mid, ui::kNameFitFloorSize) <= kRoom) {
			break;
		}
	}
	REQUIRE(widthAt(mid, kDefault) > kRoom);
	REQUIRE(widthAt(mid, ui::kNameFitFloorSize) <= kRoom);
	const ui::FittedName shrunk = ui::fitName(mid, kRoom, kDefault, ui::kNameFitFloorSize, kTrack);
	CAPTURE(mid);
	CAPTURE(shrunk.size);
	CHECK(shrunk.text == mid);
	CHECK_FALSE(shrunk.elided);
	CHECK(shrunk.size < kDefault);
	CHECK(shrunk.size >= ui::kNameFitFloorSize);
	CHECK(widthAt(shrunk.text, shrunk.size) <= kRoom);
	// THE LARGEST SIZE THAT FITS: one step bigger would clip.
	CHECK(widthAt(mid, shrunk.size + 1.0f) > kRoom);

	// A NAME PAST THE FLOOR STILL CLIPS, AT THE FLOOR SIZE, with the dots, inside the room.
	const std::string longName = "Resonance Sweep Amount 1";
	REQUIRE(widthAt(longName, ui::kNameFitFloorSize) > kRoom);
	const ui::FittedName cut = ui::fitName(longName, kRoom, kDefault, ui::kNameFitFloorSize, kTrack);
	CHECK(cut.size == ui::kNameFitFloorSize);
	CHECK(cut.elided);
	REQUIRE(cut.text.size() > 3);
	CHECK(cut.text.substr(cut.text.size() - 3) == "...");
	CHECK(widthAt(cut.text, cut.size) <= kRoom);
}

namespace {

/// WHETHER `text` IS WELL-FORMED UTF-8 as far as a cut could break it: every lead byte legal and
/// followed by exactly its count of continuation bytes. Written out here rather than borrowed from `decodeUtf8`, because that is the code
/// under test's own reader and a test cannot use the thing it checks to check it.
bool validUtf8(const std::string& text) {
	size_t i = 0;
	while (i < text.size()) {
		const auto lead = static_cast<unsigned char>(text[i]);
		size_t extra = 0;
		if (lead < 0x80) {
			extra = 0;
		}
		else if (lead >= 0xC2 && lead <= 0xDF) {
			extra = 1;
		}
		else if (lead >= 0xE0 && lead <= 0xEF) {
			extra = 2;
		}
		else if (lead >= 0xF0 && lead <= 0xF4) {
			extra = 3;
		}
		else {
			return false;
		}
		if (i + extra >= text.size()) { // the character runs off the end
			return false;
		}
		for (size_t k = 1; k <= extra; ++k) {
			if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) {
				return false;
			}
		}
		i += extra + 1;
	}
	return true;
}

} // namespace

TEST_CASE("upper() capitalises ASCII and leaves every other byte alone") {
	// **A UTF-8 NAME CANNOT BE CORRUPTED BY ITS CAPTION.** `upper` used `std::toupper` a byte at a time,
	// which answers from the locale; under a Latin-1 one 0xE0-0xFE are small letters, and those are the
	// lead bytes of every three- and four-byte character.
	CHECK(ui::upper("é mix") == "\xC3\xA9 MIX");
	// "é" ALONE DOES NOT CATCH THAT — its bytes are C3 A9, neither in the Latin-1 small-letter range, and
	// a Latin-1 known-bad passed the line above. The whole high half below is what goes red.
	CHECK(ui::upper("abc XYZ 0-9 [z]") == "ABC XYZ 0-9 [Z]");
	CHECK(ui::upper(nullptr).empty());
	// EVERY NON-ASCII BYTE, round-tripped untouched — the whole high half, in one string.
	std::string high;
	for (int b = 0x80; b <= 0xFF; ++b) {
		high.push_back(static_cast<char>(b));
	}
	CHECK(ui::upper(high.c_str()) == high);
}

TEST_CASE("a caption of multi-byte characters is cut on a code-point boundary") {
	// **THE CUT LANDS BETWEEN CHARACTERS, NEVER INSIDE ONE.** Every character here is two bytes (É À Ö
	// are U+00C9 U+00C0 U+00D6), so a cut counted in bytes would leave half a character before the dots
	// and the result would not be UTF-8 at all.
	std::string name;
	for (int i = 0; i < 8; ++i) {
		name += "\xC3\x89\xC3\x80\xC3\x96"; // ÉÀÖ
	}
	constexpr float kColumn = 54.0f;
	const visage::Font& face = ui::font(ui::kKnobLabelSize);
	REQUIRE(ui::trackedWidth(face, name, ui::kKnobLabelTracking) > kColumn); // it needs cutting

	const std::string fitted = ui::Knob::columnCaption(name.c_str(), kColumn);
	CAPTURE(fitted);
	REQUIRE(fitted.size() > 3);
	CHECK(fitted.substr(fitted.size() - 3) == "...");
	CHECK(validUtf8(fitted));
	const std::string kept = fitted.substr(0, fitted.size() - 3);
	CHECK(!kept.empty());
	CHECK(kept.size() % 2 == 0); // whole two-byte characters only
	CHECK(name.rfind(kept, 0) == 0); // a prefix of the original
	CHECK(ui::trackedWidth(face, fitted, ui::kKnobLabelTracking) <= kColumn - 2.0f * ui::Knob::kCaptionMargin);
	// NOT VACUOUS: the checker refuses a string cut inside a character.
	CHECK_FALSE(validUtf8(name.substr(0, 3)));
}

//------------------------------------------------------------------------------------------------//
// A MENU IS ALWAYS INSIDE ITS WINDOW, AND EVERY ROW OF IT CAN BE REACHED                            //
//------------------------------------------------------------------------------------------------//
//
// A context menu that ran off the bottom of the window was clipped and could not be reached.
// The GG sweep in the devices binary asks every SITE to hang its menu off its box; this
// asks the PLACEMENT itself, by value, across the anchors and sizes no one site exercises: a box at
// the top, middle, foot and foot-right of the window; a box laid out PAST the window's foot (content
// taller than the room it was given, which a small window at a high UI scale is); menus short, long
// and taller than the window; and a SUBMENU, which visage places on its own, after the hover pause,
// where `showMenuAt` never sees it.

namespace {

constexpr float kMenuBoxW = 90.0f;
constexpr float kMenuBoxH = 20.0f;

visage::PopupMenu menuOfRows(int rows) {
	visage::PopupMenu menu;
	for (int i = 0; i < rows; ++i) {
		menu.addOption(i, "Row " + std::to_string(i));
	}
	return menu;
}

/// A WINDOW, AND A BOX IN IT A MENU OPENS FROM — a plain root frame is what `showMenuAt` measures
/// against (the popup takes its top frame's bounds), so no renderer is needed for the geometry.
struct MenuWindow {
	visage::Frame root;
	visage::Frame box;
	MenuWindow(float w, float h) {
		root.setBounds(0.0f, 0.0f, w, h);
		root.addChild(&box);
	}
	~MenuWindow() {
		ui::dismissMenuOf(box);
		root.removeChild(&box);
	}
	visage::PopupMenuFrame* open(visage::PopupMenu menu, float x, float y) {
		box.setBounds(x, y, kMenuBoxW, kMenuBoxH);
		REQUIRE(ui::showMenuAt(std::move(menu), box, { 0.0f, 0.0f, kMenuBoxW, kMenuBoxH }));
		return menu_check::newestPopup(box);
	}
};

void checkInsideWindow(const visage::Frame& list, float w, float h) {
	CAPTURE(list.x());
	CAPTURE(list.y());
	CAPTURE(list.width());
	CAPTURE(list.height());
	CAPTURE(w);
	CAPTURE(h);
	CHECK(list.x() >= -0.5f);
	CHECK(list.y() >= -0.5f);
	CHECK(list.right() <= w + 0.5f);
	CHECK(list.bottom() <= h + 0.5f);
}

/// WHEEL `list` AS FAR DOWN AS IT GOES — the wheel's own road, which stops at visage's scroll limit.
void wheelToFoot(visage::PopupList& list) {
	visage::MouseEvent e;
	e.event_frame = &list;
	e.position = { list.width() * 0.5f, list.height() * 0.5f };
	e.wheel_momentum = true;
	e.precise_wheel_delta_y = -1.0e5f;
	(void)list.mouseWheel(e);
}

/// THE LAST ROW CAN BE REACHED: wheeled to its foot, the list shows the last row's foot.
void checkLastRowReachable(visage::PopupList& list) {
	wheelToFoot(list);
	const int last = list.numOptions() - 1;
	const float rowFoot = static_cast<float>(list.yForIndex(last + 1)) - list.yPosition();
	CAPTURE(list.renderHeight());
	CAPTURE(list.height());
	CAPTURE(list.yPosition());
	CAPTURE(rowFoot);
	CHECK(rowFoot <= list.height() + 0.5f);
	list.setYPosition(0.0f);
}

visage::PopupList* listOf(visage::PopupMenuFrame& popup, size_t depth) {
	return depth < popup.children().size() ? dynamic_cast<visage::PopupList*>(popup.children()[depth]) : nullptr;
}

} // namespace

TEST_CASE("a menu opened from any anchor, at any length, lies inside its window with its last row reachable") {
	struct Size {
		const char* name;
		float w;
		float h;
	};
	// THE EDITOR'S OWN FLOOR (1240x734 logical, canvas strip folded), and a small one — a popped-out
	// panel, or the logical room a window gets at a high UI scale.
	const Size sizes[] = { { "editor", 1240.0f, 734.0f }, { "small", 420.0f, 300.0f } };
	for (const Size& s : sizes) {
		struct Anchor {
			const char* name;
			float x;
			float y;
		};
		const Anchor anchors[] = {
			{ "top", 20.0f, 10.0f },
			{ "middle", s.w * 0.5f, s.h * 0.5f },
			{ "bottom", 20.0f, s.h - kMenuBoxH - 4.0f },
			{ "bottom-right", s.w - kMenuBoxW - 4.0f, s.h - kMenuBoxH - 4.0f },
			// LAID OUT PAST THE FOOT: the box's own frame below the window's last row.
			{ "past the foot", 20.0f, s.h + 12.0f },
			{ "past the foot, right", s.w - kMenuBoxW * 0.5f, s.h + 12.0f },
		};
		const int lengths[] = { 3, 20, 80 };
		for (const Anchor& a : anchors) {
			for (const int rows : lengths) {
				const std::string window = s.name;
				CAPTURE(window);
				const std::string anchor = a.name;
				CAPTURE(anchor);
				CAPTURE(rows);
				MenuWindow win(s.w, s.h);
				visage::PopupMenuFrame* popup = win.open(menuOfRows(rows), a.x, a.y);
				REQUIRE(popup != nullptr);
				visage::PopupList* list = listOf(*popup, 0);
				REQUIRE(list != nullptr);
				REQUIRE(list->numOptions() == rows);
				checkInsideWindow(*list, s.w, s.h);
				checkLastRowReachable(*list);
				// THE PLACEMENT REPORT IS THE LIST: a case below reads it, so it must not be a different claim.
				const ui::MenuPlacement placed = ui::lastMenuPlacementForTest();
				CHECK(placed.list.y() == doctest::Approx(list->y()));
				CHECK(placed.list.bottom() == doctest::Approx(list->bottom()));
			}
		}
	}
}

TEST_CASE("a menu with no room below its box at the window's foot opens ABOVE it, flush") {
	// The ordinary foot case, by value: a 20-row menu (448 px) under a box 30 px from the foot of the
	// editor goes UP, its foot on the box's head and its left edge on the box's.
	MenuWindow win(1240.0f, 734.0f);
	const float boxY = 734.0f - kMenuBoxH - 10.0f;
	visage::PopupMenuFrame* popup = win.open(menuOfRows(20), 300.0f, boxY);
	REQUIRE(popup != nullptr);
	visage::PopupList* list = listOf(*popup, 0);
	REQUIRE(list != nullptr);
	CHECK(list->bottom() == doctest::Approx(boxY));
	CHECK(list->x() == doctest::Approx(300.0f));
	CHECK(list->height() == doctest::Approx(list->renderHeight()));
}

TEST_CASE("a submenu lies inside its window with its last row reachable, at every depth and from a scrolled list") {
	struct Size {
		const char* name;
		float w;
		float h;
	};
	const Size sizes[] = { { "editor", 1240.0f, 734.0f }, { "small", 420.0f, 300.0f } };
	for (const Size& s : sizes) {
		const int subLengths[] = { 4, 20, 80 };
		for (const int subRows : subLengths) {
			const float anchorsY[] = { 10.0f, s.h - kMenuBoxH - 4.0f };
			const float anchorsX[] = { 20.0f, s.w - kMenuBoxW - 4.0f };
			for (const float ay : anchorsY) {
				for (const float ax : anchorsX) {
					const std::string window = s.name;
					CAPTURE(window);
					CAPTURE(subRows);
					CAPTURE(ax);
					CAPTURE(ay);
					// TEN HEADINGS, each a submenu of `subRows`, the LAST also holding a nested one.
					visage::PopupMenu menu;
					for (int h = 0; h < 10; ++h) {
						visage::PopupMenu sub("Heading " + std::to_string(h));
						for (int i = 0; i < subRows; ++i) {
							sub.addOption(h * 1000 + i, "Item " + std::to_string(i));
						}
						if (h == 9) {
							sub.addSubMenu(menuOfRows(subRows));
						}
						menu.addSubMenu(std::move(sub));
					}
					MenuWindow win(s.w, s.h);
					visage::PopupMenuFrame* popup = win.open(std::move(menu), ax, ay);
					REQUIRE(popup != nullptr);
					visage::PopupList* top = listOf(*popup, 0);
					REQUIRE(top != nullptr);
					checkInsideWindow(*top, s.w, s.h);
					for (const int heading : { 0, 9 }) {
						CAPTURE(heading);
						// visage's own road: the row's y in its list's rows, handed on by the list's selection.
						popup->subMenuSelected(top->option(heading), top->yForIndex(heading), top);
						visage::PopupList* sub = listOf(*popup, 1);
						REQUIRE(sub != nullptr);
						REQUIRE(sub->isVisible());
						checkInsideWindow(*sub, s.w, s.h);
						checkLastRowReachable(*sub);
					}
					// A LEVEL DEEPER: the nested menu at the foot of heading 9.
					visage::PopupList* sub = listOf(*popup, 1);
					popup->subMenuSelected(sub->option(subRows), sub->yForIndex(subRows), sub);
					visage::PopupList* nested = listOf(*popup, 2);
					REQUIRE(nested != nullptr);
					REQUIRE(nested->isVisible());
					checkInsideWindow(*nested, s.w, s.h);
					checkLastRowReachable(*nested);
				}
			}
		}
	}
}

TEST_CASE("a submenu opened from a SCROLLED list hangs off its row as drawn, not as it would sit unscrolled") {
	// The small window: the top list is taller than the window and scrolls; heading 30 is opened after
	// scrolling it into view. Its submenu's head belongs on the row's head ON SCREEN.
	MenuWindow win(420.0f, 300.0f);
	visage::PopupMenu menu;
	for (int h = 0; h < 40; ++h) {
		visage::PopupMenu sub("Heading " + std::to_string(h));
		sub.addOption(h * 10, "Only");
		menu.addSubMenu(std::move(sub));
	}
	visage::PopupMenuFrame* popup = win.open(std::move(menu), 20.0f, 10.0f);
	REQUIRE(popup != nullptr);
	visage::PopupList* top = listOf(*popup, 0);
	REQUIRE(top != nullptr);
	wheelToFoot(*top);
	REQUIRE(top->yPosition() > 0.0f);
	const int heading = 35;
	const float rowOnScreen = top->y() + static_cast<float>(top->yForIndex(heading)) - top->yPosition();
	REQUIRE(rowOnScreen >= top->y());
	REQUIRE(rowOnScreen < top->bottom());
	popup->subMenuSelected(top->option(heading), top->yForIndex(heading), top);
	visage::PopupList* sub = listOf(*popup, 1);
	REQUIRE(sub != nullptr);
	CHECK(sub->y() == doctest::Approx(rowOnScreen));
	checkInsideWindow(*sub, 420.0f, 300.0f);
}

TEST_CASE("SHOT a long menu and its long submenu, opened at a small window's foot") {
	// NOT A CASE — A CAMERA, gated on FEATHERS_SHOT_DIR like the other SHOT cases, so an ordinary run
	// costs nothing. Run it under `scripts/venue-xvfb.sh` (VISAGE_FULL_REDRAW=1 is the venue's). A
	// 420x300 window — a popped-out panel — with a box 24 px from its foot opening ten headings, the
	// last one hovered open on a 40-row submenu. Upstream's submenu sat at y = 300 - 888 with its first
	// 27 rows above the window and no scroll; written as `menu_foot.png`.
	const char* dir = std::getenv("FEATHERS_SHOT_DIR");
	if (dir == nullptr) {
		return;
	}
	constexpr int kW = 420;
	constexpr int kH = 300;
	visage::ApplicationEditor editor;
	editor.setBounds(0.0f, 0.0f, kW, kH);
	editor.setWindowless(kW, kH);
	if (!visage::Renderer::instance().supported()) {
		MESSAGE("SKIP no renderer: " << visage::Renderer::instance().errorMessage());
		return;
	}
	editor.onDraw() = [&editor](visage::Canvas& canvas) {
		canvas.setColor(0xff101418);
		canvas.fill(0.0f, 0.0f, editor.width(), editor.height());
	};
	visage::Frame box;
	box.onDraw() = [&box](visage::Canvas& canvas) {
		canvas.setColor(0xff4070c0);
		canvas.fill(0.0f, 0.0f, box.width(), box.height());
	};
	editor.addChild(&box);
	box.setBounds(40.0f, kH - kMenuBoxH - 4.0f, kMenuBoxW, kMenuBoxH);
	visage::PopupMenu menu;
	for (int h = 0; h < 10; ++h) {
		visage::PopupMenu sub("Heading " + std::to_string(h));
		for (int i = 0; i < 40; ++i) {
			sub.addOption(h * 100 + i, "Item " + std::to_string(i));
		}
		menu.addSubMenu(std::move(sub));
	}
	REQUIRE(ui::showMenuAt(std::move(menu), box, { 0.0f, 0.0f, kMenuBoxW, kMenuBoxH }));
	visage::PopupMenuFrame* popup = menu_check::newestPopup(box);
	REQUIRE(popup != nullptr);
	visage::PopupList* top = listOf(*popup, 0);
	REQUIRE(top != nullptr);
	popup->subMenuSelected(top->option(9), top->yForIndex(9), top);
	visage::PopupList* sub = listOf(*popup, 1);
	REQUIRE(sub != nullptr);
	MESSAGE("top list " << top->x() << "," << top->y() << " " << top->width() << "x" << top->height());
	MESSAGE("submenu " << sub->x() << "," << sub->y() << " " << sub->width() << "x" << sub->height());
	// FULLY FADED IN — the popup's opacity animates on its draws.
	for (int i = 0; i < 60; ++i) {
		editor.redrawAll();
		editor.drawWindow();
	}
	const visage::Screenshot& shot = editor.takeScreenshot();
	REQUIRE(shot.width() == kW);
	const std::string path = std::string(dir) + "/menu_foot.png";
	shot.save(path);
	MESSAGE("wrote " << path);
	ui::dismissMenuOf(box);
	editor.removeChild(&box);
}

TEST_CASE("additive: the Engine rail draws Phase Width, Osc Mode, and Phase as a stereo destination there") {
	// BACKLOG OZ (2026-10-29) overturned half of LY's decision (2026-10-10): the partial bank's right ear now reads its own
	// rotators while the ears' phases differ (PartialBank::setEarPhase), so Phase Width's mirror and a stereo route's ear
	// difference into Phase are heard on Additive as on V5, and both are drawn there. AND OSC MODE IS DRAWN (backlog LY,
	// phase drift in detune): the bank starts each note at the accumulator's phase and follows it.
	ParamBridge bridge;
	feathers::BridgeDeviceHost deviceHost { bridge };
	WidgetRegistry registry;
	EngineRail engineRail(bridge, registry);
	engineRail.setBounds(0.0f, 0.0f, EngineRail::kWidth, 700.0f);
	bridge.refresh();
	ParamSet params;
	for (const OscType engine : { OscType::V5, OscType::Additive, OscType::V5 }) {
		CAPTURE(static_cast<int>(engine));
		params.setBase(static_cast<clap_id>(ParamId::OscType), static_cast<double>(engine));
		bridge.publish(params);
		bridge.refresh();
		engineRail.refreshEnablement();
		CHECK(engineRail.phaseWidthForTest()->isVisible());
		CHECK(engineRail.oscModeForTest()->isVisible());
		CHECK(engineRail.phaseForTest()->isVisible());
		CHECK(engineRail.phaseForTest()->enabled());
		CHECK(engineRail.phaseForTest()->control()->display().stereo);
	}
}

TEST_CASE("additive: the wave display draws the blended spectrum summed as one cycle, at Newman's flat phases") {
	// A VALUE: the curve is Σ_{k<N} envAtRatio_k·fine_k·sin(2π·ratio_k·φ + π·k²/N) over the spectrum the bank builds for
	// the character the display drew, N the partials the curve's sampling resolves (a quarter of its points), φ = i/(n−1).
	// Restated with a sine per partial against the recurrence the display uses. Not the stair grid, not silence.
	WaveFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Additive));
	f.set(ParamId::AZone, 17.0);
	f.set(ParamId::BEnable, 0.0);
	f.set(ParamId::Path, 0.0);
	f.publish();
	f.wave.refresh();
	REQUIRE(f.wave.character().engine == feathers::OscEngine::Partials);
	feathers::PreparedCharacter p;
	feathers::PartialsScratch scratch;
	feathers::CharacterBank::buildOne(f.wave.character(), p, scratch);
	const feathers::partials::PartialSpectrum& s = p.partials;
	const std::span<const float> curve = f.wave.curve();
	REQUIRE(curve.size() >= WaveDisplay::kMinPoints);
	const int n = std::min(static_cast<int>(curve.size() / 4), feathers::partials::kMaxPartials);
	CHECK(WaveDisplay::partialsDrawnAt(curve.size()) == n);
	double peak = 0.0;
	for (size_t i = 0; i < curve.size(); i += 5) {
		const double phase = static_cast<double>(i) / static_cast<double>(curve.size() - 1);
		double want = 0.0;
		for (int k = 0; k < n; ++k) {
			want += static_cast<double>(s.envAtRatio[k]) * static_cast<double>(s.fine[k])
			        * std::sin(2.0 * std::numbers::pi * s.ratio[k] * phase + std::numbers::pi * k * k / n);
		}
		peak = std::max(peak, std::abs(want));
		CAPTURE(i);
		CHECK(std::abs(static_cast<double>(curve[i]) - want) <= 1e-5);
	}
	CHECK(peak > 0.05); // not silence
	// NOT STAIR'S GRID: the same character read as the last branch would read it draws something else.
	feathers::Character asStair = f.wave.character();
	asStair.engine = feathers::OscEngine::Stair;
	const feathers::StairTable stair = feathers::buildStairTable(feathers::buildStairFor(asStair));
	double apart = 0.0;
	for (size_t i = 0; i < curve.size(); i += 5) {
		const double phase = static_cast<double>(i) / static_cast<double>(curve.size() - 1);
		apart = std::max(apart, std::abs(static_cast<double>(curve[i]) - feathers::evalStairTable(stair, phase)));
	}
	CHECK(apart > 0.05);
}

//---------------------------------------------------------------------------------------------------//
// TYPE-IN — backlog LS: "no double click resets, triple click types."             //
//---------------------------------------------------------------------------------------------------//

namespace {

namespace ero = feathers::fx::eroder;

/// ERODER's Mix — a Percent control, 0..1, default 50 % — on a knob or a box seated in the editor's own
/// tree, over a `DeviceControl` given the editor's history and panel state, which is how every device card
/// builds one. In the tree because the field takes the keyboard focus, as the macro rename's does.
struct TypeInFixture {
	ModFixture f;
	DeviceInstanceId device = kNoInstance;
	std::unique_ptr<ui::DeviceControl> control;
	ui::EditHistory* history = nullptr;

	TypeInFixture() {
		const size_t slot = f.bridge.addFxDevice(ero::kTraits.tag);
		REQUIRE(slot < feathers::kMaxFxSlots);
		device = f.bridge.fxDeviceAt(slot);
		history = f.gui.history();
		REQUIRE(history != nullptr);
		control = std::make_unique<ui::DeviceControl>(f.deviceHost, device, ero::kDescriptor,
		                                             static_cast<size_t>(ero::Param::Mix), f.gui.modForTest(), history);
	}
	/// A value put there without a gesture: where the hand found it, not an edit of its own.
	void place(double v) {
		control->setValue(v);
		REQUIRE(static_cast<float>(control->value()) == static_cast<float>(v));
	}
	[[nodiscard]] float value() const { return static_cast<float>(control->value()); }
};

/// The three presses of a triple-click at `p`, each released — visage's own counting, 1 then 2 then 3.
void tripleClick(visage::Frame& frame, visage::Point p) {
	frame.processMouseDown(at(p.x, p.y));
	frame.processMouseUp(at(p.x, p.y));
	frame.processMouseDown(doubleClickAt(p.x, p.y));
	frame.processMouseUp(at(p.x, p.y));
	frame.processMouseDown(tripleClickAt(p.x, p.y));
	frame.processMouseUp(at(p.x, p.y));
}

void typeKey(ui::TextField& field, visage::KeyCode key) { field.processKeyPress(visage::KeyEvent { key, 0, true }); }

} // namespace

TEST_CASE("type-in: a triple-click on a knob opens a field on the value from before the reset, and Enter is ONE undo step") {
	TypeInFixture t;
	ui::Knob knob { *t.control, "Mix" };
	t.f.root->addChild(&knob);
	knob.setBounds(0.0f, 0.0f, 52.0f, ui::Knob::kHeight);
	const visage::Point face { 26.0f, knob.faceDiameter() * 0.5f };
	t.place(0.3);
	const size_t steps = t.history->length();

	// THE DOUBLE-CLICK STILL RESETS, and records its step, on the way to the third click.
	knob.processMouseDown(at(face.x, face.y));
	knob.processMouseUp(at(face.x, face.y));
	knob.processMouseDown(doubleClickAt(face.x, face.y));
	knob.processMouseUp(at(face.x, face.y));
	CHECK(t.value() == 0.5f);
	CHECK(t.history->length() == steps + 1);
	CHECK(knob.typeInField() == nullptr); // a double-click opens nothing

	knob.processMouseDown(tripleClickAt(face.x, face.y));
	ui::TextField* field = knob.typeInField();
	REQUIRE(field != nullptr);
	CHECK(field->editing());
	CHECK(field->isVisible());
	// ON THE PRE-RESET VALUE, in the unit the knob shows, and ALL OF IT SELECTED: the first key replaces it.
	CHECK(field->text() == "30%");
	CHECK(field->caretForTest() == field->text().size());
	field->processTextInput("7");
	CHECK(field->text() == "7");
	field->processTextInput("2");
	typeKey(*field, visage::KeyCode::Return);

	CHECK_FALSE(field->isVisible());
	CHECK(t.value() == 0.72f);
	// THE RESET FOLDED INTO THE TYPED STEP: one entry for the whole triple-click...
	CHECK(t.history->length() == steps + 1);
	// ...so one undo goes back to where the hand started, past the default.
	t.history->undo();
	CHECK(t.value() == 0.3f);
	t.history->redo();
	CHECK(t.value() == 0.72f);
	t.f.root->removeChild(&knob);
}

TEST_CASE("type-in: a triple-click then Escape changes NOTHING — the pre-reset value is back and no step is left") {
	TypeInFixture t;
	ui::Knob knob { *t.control, "Mix" };
	t.f.root->addChild(&knob);
	knob.setBounds(0.0f, 0.0f, 52.0f, ui::Knob::kHeight);
	const visage::Point face { 26.0f, knob.faceDiameter() * 0.5f };
	t.place(0.3);
	const size_t steps = t.history->length();
	const bool couldRedo = t.history->canRedo();

	const auto openField = [&] {
		tripleClick(knob, face);
		REQUIRE(knob.typeInField() != nullptr);
		REQUIRE(knob.typeInField()->editing());
		CHECK(t.value() == 0.5f); // the reset ran on the way
	};
	const auto unchanged = [&](const char* how) {
		INFO(how);
		CHECK_FALSE(knob.typeInField()->isVisible());
		CHECK(t.value() == 0.3f);
		CHECK(t.history->length() == steps);
		CHECK(t.history->canRedo() == couldRedo);
	};

	SUBCASE("Escape") {
		openField();
		knob.typeInField()->processTextInput("12");
		typeKey(*knob.typeInField(), visage::KeyCode::Escape);
		unchanged("Escape");
	}
	SUBCASE("a click elsewhere: on the knob's face, which is the house's click-away") {
		openField();
		knob.typeInField()->processTextInput("12");
		knob.processMouseDown(at(face.x, face.y));
		knob.processMouseUp(at(face.x, face.y));
		unchanged("a press on the face");
	}
	SUBCASE("the focus going elsewhere, as the macro rename's does") {
		openField();
		knob.typeInField()->processTextInput("12");
		knob.typeInField()->focusChanged(false, true);
		unchanged("focus lost");
	}
	SUBCASE("an unchanged Enter") {
		openField();
		typeKey(*knob.typeInField(), visage::KeyCode::Return);
		unchanged("Enter on the opening text");
	}
	SUBCASE("a non-number") {
		openField();
		knob.typeInField()->processTextInput("abc");
		typeKey(*knob.typeInField(), visage::KeyCode::Return);
		unchanged("Enter on junk");
	}
	SUBCASE("the same value spelled another way") {
		openField();
		knob.typeInField()->processTextInput("30.0 %");
		typeKey(*knob.typeInField(), visage::KeyCode::Return);
		unchanged("Enter on the same value");
	}
	t.f.root->removeChild(&knob);
}

TEST_CASE("type-in: a typed value is clamped to the control's own range and read in the unit it shows") {
	TypeInFixture t;
	ui::Knob knob { *t.control, "Mix" };
	t.f.root->addChild(&knob);
	knob.setBounds(0.0f, 0.0f, 52.0f, ui::Knob::kHeight);
	const visage::Point face { 26.0f, knob.faceDiameter() * 0.5f };
	const auto type = [&](const char* text) {
		t.place(0.3);
		tripleClick(knob, face);
		REQUIRE(knob.typeInField() != nullptr);
		knob.typeInField()->processTextInput(text);
		typeKey(*knob.typeInField(), visage::KeyCode::Return);
		return t.value();
	};
	CHECK(type("150") == 1.0f);
	CHECK(type("-20") == 0.0f);
	CHECK(type("12.5%") == 0.125f);
	// A PERCENT, NEVER A GUESSED FRACTION — absolute bound (Approx's scale would pass anything this small).
	CHECK(std::abs(type("0.5") - 0.005f) < 1e-7f);
	t.f.root->removeChild(&knob);
}

TEST_CASE("type-in: in mapping mode the triple-click types the ROUTE AMOUNT as a percent") {
	TypeInFixture t;
	ui::ModState* mod = t.f.gui.modForTest();
	REQUIRE(mod != nullptr);
	const backplane::Source source = backplane::Source::core(0);
	t.f.arm(source);
	REQUIRE(mod->armed());
	ui::Knob knob { *t.control, "Mix" };
	knob.setModulation(mod, t.history);
	t.f.root->addChild(&knob);
	knob.setBounds(0.0f, 0.0f, 52.0f, ui::Knob::kHeight);
	REQUIRE(knob.mappable());
	const visage::Point face { 26.0f, knob.faceDiameter() * 0.5f };

	// A ROUTE AT 40 %, made the way a hand makes one: an amount gesture.
	t.control->beginAmountGesture();
	t.control->setAmount(0.4f);
	t.control->endAmountGesture();
	REQUIRE(t.control->display().amount == 0.4f);
	const size_t steps = t.history->length();
	const float base = t.value();

	SUBCASE("Enter: the amount, as one undo step back to the route") {
		tripleClick(knob, face);
		ui::TextField* field = knob.typeInField();
		REQUIRE(field != nullptr);
		REQUIRE(field->editing());
		CHECK(field->text() == "40"); // the amount from before the double-click's unroute
		field->processTextInput("-25%");
		typeKey(*field, visage::KeyCode::Return);
		CHECK(t.control->display().amount == -0.25f);
		CHECK(t.value() == base); // the VALUE was never the thing under the hand
		CHECK(t.history->length() == steps + 1);
		t.history->undo();
		CHECK(t.control->display().amount == 0.4f);
	}
	SUBCASE("Escape: the route is back and no step is left") {
		tripleClick(knob, face);
		REQUIRE(knob.typeInField() != nullptr);
		CHECK(t.control->display().amount == 0.0f); // the double-click unrouted on the way
		knob.typeInField()->processTextInput("90");
		typeKey(*knob.typeInField(), visage::KeyCode::Escape);
		CHECK(t.control->display().amount == 0.4f);
		CHECK(t.history->length() == steps);
	}
	SUBCASE("beyond the amount's range is clamped to it") {
		tripleClick(knob, face);
		REQUIRE(knob.typeInField() != nullptr);
		knob.typeInField()->processTextInput("900");
		typeKey(*knob.typeInField(), visage::KeyCode::Return);
		CHECK(t.control->display().amount == device::kMaxAmount);
	}
	t.f.root->removeChild(&knob);
}

TEST_CASE("type-in: a value box types the same way — the field over its reading, one step, Escape changes nothing") {
	TypeInFixture t;
	ui::ValueBox box { *t.control, "Mix" };
	t.f.root->addChild(&box);
	box.setBounds(0.0f, 0.0f, 60.0f, ui::ValueBox::kHeight);
	const visage::Point on { 30.0f, ui::ValueBox::kHeight * 0.5f };
	t.place(0.3);
	const size_t steps = t.history->length();

	SUBCASE("Enter") {
		tripleClick(box, on);
		ui::TextField* field = box.typeInField();
		REQUIRE(field != nullptr);
		REQUIRE(field->editing());
		CHECK(field->text() == "30%");
		field->processTextInput("64");
		typeKey(*field, visage::KeyCode::Return);
		CHECK(t.value() == 0.64f);
		// THE BOX'S RESET NOW RECORDS A STEP (it used to bracket the host's gesture alone and record none,
		// so a double-click on a box could not be undone), and the typed value folds into it.
		CHECK(t.history->length() == steps + 1);
		t.history->undo();
		CHECK(t.value() == 0.3f);
	}
	SUBCASE("Escape") {
		tripleClick(box, on);
		REQUIRE(box.typeInField() != nullptr);
		box.typeInField()->processTextInput("64");
		typeKey(*box.typeInField(), visage::KeyCode::Escape);
		CHECK(t.value() == 0.3f);
		CHECK(t.history->length() == steps);
	}
	SUBCASE("the double-click alone is an undoable reset") {
		box.processMouseDown(at(on.x, on.y));
		box.processMouseUp(at(on.x, on.y));
		box.processMouseDown(doubleClickAt(on.x, on.y));
		box.processMouseUp(at(on.x, on.y));
		CHECK(t.value() == 0.5f);
		CHECK(t.history->length() == steps + 1);
		t.history->undo();
		CHECK(t.value() == 0.3f);
	}
	t.f.root->removeChild(&box);
}

TEST_CASE("type-in: a core parameter's knob types through the host's own text, and reads it back") {
	// THE HOST ROAD (`ParamControl`): the knob prints what the DAW prints ("-6.0 dB"), so it types through
	// the same table — `ParamSet::textToValue` — rather than through a device unit it does not have.
	ParamBridge bridge;
	feathers::BridgeDeviceHost deviceHost { bridge };
	WidgetRegistry registry;
	const ParamDesc& level = descOf(ParamId::VoiceLevel);
	auto knob_ptr = paramKnob(bridge, registry, &level, "Level");
	ui::Knob& knob = *knob_ptr;
	knob.setBounds(0.0f, 0.0f, 52.0f, Knob::kHeight);
	bridge.refresh();
	bridge.setValue(static_cast<clap_id>(ParamId::VoiceLevel), 0.25);
	REQUIRE(bridge.value(static_cast<clap_id>(ParamId::VoiceLevel)) == 0.25);
	drain(bridge);
	char shown[64] = {};
	REQUIRE(valueToText(static_cast<clap_id>(ParamId::VoiceLevel), 0.25, shown, sizeof shown));

	tripleClick(knob, { 26.0f, 26.0f });
	ui::TextField* field = knob.typeInField();
	REQUIRE(field != nullptr);
	CHECK(field->text() == shown);
	drain(bridge);
	field->processTextInput("-6 dB");
	typeKey(*field, visage::KeyCode::Return);
	const std::vector<UiParamEvent> posted = drain(bridge);
	// ONE HOST GESTURE CARRYING THE TYPED VALUE: Gain is a linear factor, so -6 dB is 10^(-6/20).
	REQUIRE(posted.size() == 3);
	CHECK(posted[0].kind == UiParamEvent::Kind::GestureBegin);
	CHECK(std::abs(posted[1].value - std::pow(10.0, -6.0 / 20.0)) < 1e-9);
	CHECK(posted[2].kind == UiParamEvent::Kind::GestureEnd);
}

TEST_CASE("type-in: the field sits over the control's reading at 1x and 1.75x, its native size following the scale") {
	// "A SCALE CHANGE MOVES NOTHING LOGICAL" (CLAUDE.md): the claim is NATIVE, because the logical rectangle
	// cannot change. Asked of a knob and a value box seated on ERODER's card in the fx lane, as the editor
	// seats them, both opened at 1x and then rescaled, and opened again at 1.75x.
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	const size_t slot = f.bridge.addFxDevice(ero::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	lane->setCollapsed(false);
	lane->resized();
	ui::FxCard* card = lane->cardAt(slot);
	REQUIRE(card != nullptr);
	std::vector<ui::Knob*> knobs;
	collectKnobs(card, &knobs);
	ui::Knob* mix = nullptr;
	for (ui::Knob* k : knobs) {
		if (k->control() != nullptr && std::string(k->control()->desc().key) == "eroder_mix") {
			mix = k;
		}
	}
	REQUIRE(mix != nullptr);
	REQUIRE(mix->width() > 0.0f);

	const auto check = [&](float scale, const char* when) {
		INFO(when);
		ui::TextField* field = mix->typeInField();
		REQUIRE(field != nullptr);
		const visage::Bounds want = mix->typeInBounds();
		// OVER THE READING, inside the knob, and the width of its column.
		CHECK(field->x() == want.x());
		CHECK(field->y() == want.y());
		CHECK(field->width() == want.width());
		CHECK(field->height() == want.height());
		CHECK(want.right() <= mix->width());
		CHECK(want.bottom() <= mix->height());
		CHECK(want.width() > 0.0f);
		CHECK(field->dpiScale() == scale);
		CHECK(std::abs(static_cast<float>(field->nativeWidth()) - scale * want.width()) <= 1.0f);
		CHECK(std::abs(static_cast<float>(field->nativeHeight()) - scale * want.height()) <= 1.0f);
	};
	const visage::Point face { mix->width() * 0.5f, mix->faceDiameter() * 0.5f };
	tripleClick(*mix, face);
	check(1.0f, "opened at 1x");
	lane->setDpiScale(1.75f);
	lane->resized();
	check(1.75f, "open, then the editor rescaled to 1.75x");
	typeKey(*mix->typeInField(), visage::KeyCode::Escape);
	tripleClick(*mix, face);
	check(1.75f, "opened again at 1.75x");
	typeKey(*mix->typeInField(), visage::KeyCode::Escape);
}

TEST_CASE("type-in: a value box's field sits over its reading at 1x and 1.75x") {
	TypeInFixture t;
	ui::ValueBox box { *t.control, "Mix" };
	t.f.root->addChild(&box);
	box.setBounds(0.0f, 0.0f, 60.0f, ui::ValueBox::kHeight);
	tripleClick(box, { 30.0f, ui::ValueBox::kHeight * 0.5f });
	ui::TextField* field = box.typeInField();
	REQUIRE(field != nullptr);
	const visage::Bounds want = box.typeInBounds();
	CHECK(field->x() == want.x());
	CHECK(field->y() == want.y());
	CHECK(field->width() == want.width());
	CHECK(field->height() == want.height());
	// THE CAPTION STAYS ABOVE IT, to say what is being typed.
	CHECK(want.y() >= ui::ValueBox::kLabelHeight);
	CHECK(want.bottom() <= box.height());
	t.f.root->setDpiScale(1.75f);
	box.setBounds(0.0f, 0.0f, 60.0f, ui::ValueBox::kHeight);
	CHECK(field->dpiScale() == 1.75f);
	CHECK(std::abs(static_cast<float>(field->nativeWidth()) - 1.75f * want.width()) <= 1.0f);
	CHECK(std::abs(static_cast<float>(field->nativeHeight()) - 1.75f * want.height()) <= 1.0f);
	typeKey(*field, visage::KeyCode::Escape);
	t.f.root->setDpiScale(1.0f);
	t.f.root->removeChild(&box);
}

TEST_CASE("type-in: a control that refuses opens no field, and its double-click still resets") {
	// SKLITTER's Rate prints a note length or a slice count through its device's own `TextOps` ("1/16"),
	// which no unit parser reads — so the field refuses rather than take "16" as a position of 16.
	namespace sk = feathers::fx::sklitter;
	ModFixture f;
	const size_t slot = f.bridge.addFxDevice(sk::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	ui::DeviceControl rate { f.deviceHost, f.bridge.fxDeviceAt(slot), sk::kDescriptor,
		                     static_cast<size_t>(sk::Param::Rate), nullptr, f.gui.history() };
	REQUIRE(sk::kDescriptor.text != nullptr);
	ui::Knob knob { rate, "Rate" };
	f.root->addChild(&knob);
	knob.setBounds(0.0f, 0.0f, 52.0f, ui::Knob::kHeight);
	const double def = rate.desc().def;
	const double away = def < 0.5 * (rate.desc().min + rate.desc().max) ? rate.desc().max : rate.desc().min;
	rate.setValue(away);
	REQUIRE(rate.value() != def);
	CHECK_FALSE(rate.editText().has_value());

	tripleClick(knob, { 26.0f, knob.faceDiameter() * 0.5f });
	CHECK(rate.value() == def);
	CHECK(knob.typeInRefused());
	CHECK((knob.typeInField() == nullptr || !knob.typeInField()->isVisible()));
	f.root->removeChild(&knob);
}

TEST_CASE("retractLast takes the tip back undone and leaves no redo tail; it refuses a tip that has moved") {
	struct Probe final : ui::EditRecord {
		double* cell;
		double before;
		double after;
		Probe(double* c, double b, double a) : cell(c), before(b), after(a) {}
		void undo() const override { *cell = before; }
		void redo() const override { *cell = after; }
		const char* verb() const override { return "Probe"; }
	};
	ui::EditHistory history;
	double cell = 1.0;
	cell = 2.0;
	history.record(std::make_unique<Probe>(&cell, 1.0, 2.0));
	cell = 3.0;
	history.record(std::make_unique<Probe>(&cell, 2.0, 3.0));
	const uint64_t tip = history.lastSerial();
	REQUIRE(tip != 0);

	CHECK_FALSE(history.retractLast(tip + 1)); // not the tip
	CHECK(history.length() == 2);
	CHECK(history.retractLast(tip));
	CHECK(cell == 2.0);
	CHECK(history.length() == 1);
	CHECK_FALSE(history.canRedo()); // gone, not merely undone
	CHECK_FALSE(history.retractLast(tip)); // and only once

	// A TIP WALKED BACK IS NOT A TIP: an undone entry is in the redo tail, and retracting it would be
	// silently deleting the redo.
	const uint64_t first = history.lastSerial();
	history.undo();
	CHECK_FALSE(history.retractLast(first));
	CHECK(history.canRedo());

	// NOT INSIDE AN OPEN GROUP — the act in progress owns the stack.
	history.redo();
	history.beginGroup("G");
	CHECK_FALSE(history.retractLast(history.lastSerial()));
	history.endGroup();
}

TEST_CASE("SHOT explore's flanks on Additive, with its six bias knobs, and on V5") {
	// NOT A CASE — A CAMERA, gated on FEATHERS_SHOT_DIR like the other SHOT cases. Run under `scripts/venue-xvfb.sh`
	// (VISAGE_FULL_REDRAW=1). Writes explore_additive.png and explore_v5.png: the same stage on the two engines, so the
	// bias knobs (Rolloff in FM Depth's slot, the other five on the right) can be seen to sit where V5's knobs sit.
	const char* dir = std::getenv("FEATHERS_SHOT_DIR");
	if (dir == nullptr) {
		return;
	}
	ExploreFixture f;
	const int w = static_cast<int>(f.stage.width());
	const int h = static_cast<int>(f.stage.height());
	visage::ApplicationEditor editor;
	editor.setBounds(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
	editor.setWindowless(w, h);
	if (!visage::Renderer::instance().supported()) {
		MESSAGE("SKIP no renderer: " << visage::Renderer::instance().errorMessage());
		return;
	}
	editor.onDraw() = [&editor](visage::Canvas& canvas) {
		canvas.setColor(0xff101418);
		canvas.fill(0.0f, 0.0f, editor.width(), editor.height());
	};
	editor.addChild(&f.stage);
	f.stage.setBounds(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
	for (const auto& [engine, name] : { std::pair { OscType::Additive, "explore_additive" },
	                                    std::pair { OscType::V5, "explore_v5" } }) {
		f.set(ParamId::OscType, static_cast<double>(engine));
		f.set(ParamId::AEnable, 1.0);
		f.stage.update();
		for (int i = 0; i < 20; ++i) {
			f.stage.update();
			editor.redrawAll();
			editor.drawWindow();
		}
		const visage::Screenshot& shot = editor.takeScreenshot();
		REQUIRE(shot.width() == w);
		const std::string path = std::string(dir) + "/" + name + ".png";
		shot.save(path);
		MESSAGE("wrote " << path);
	}
	editor.removeChild(&f.stage);
}

TEST_CASE("a reordered sound takes its per-sound knobs with it: Smoothness, FM Depth, Warp, Shape Amount and the six bias knobs") {
	// THE KNOBS ARE THE SOUND'S, not the slot's (V18's rule for the ride-along family, and the "per-sound settings
	// follow a cloned sound", 2026-10-10). A reorder splices SlotState along the lane; a per-sound knob that SlotState
	// does not carry stays in its slot and lands on whichever sound moved in. Asserted by VALUE at the slots the sounds
	// end up in.
	StackFixture f;
	f.consume(0, 300.0);
	f.consume(1, 500.0);
	f.consume(2, 700.0);
	f.set(feathers::smoothnessParam(0), 0.9);
	f.set(feathers::fmDepthParam(0), 0.7);
	f.set(feathers::warpParam(0), 0.6);
	f.set(feathers::shapeAmountParam(0), 0.8);
	f.set(feathers::additiveRolloffParam(0), 0.15);
	f.set(feathers::additiveOffGridParam(1), 0.95);
	f.settle();

	f.dragCard(0, 2); // the first sound to the end of the list
	f.settle();
	REQUIRE(f.zoneOf(2) == doctest::Approx(300.0)); // the sound that was in slot 0 is in slot 2 now
	REQUIRE(f.zoneOf(0) == doctest::Approx(500.0)); // and slot 1's is in slot 0
	CHECK(f.read(feathers::smoothnessParam(2)) == doctest::Approx(0.9));
	CHECK(f.read(feathers::fmDepthParam(2)) == doctest::Approx(0.7));
	CHECK(f.read(feathers::warpParam(2)) == doctest::Approx(0.6));
	CHECK(f.read(feathers::shapeAmountParam(2)) == doctest::Approx(0.8));
	CHECK(f.read(feathers::additiveRolloffParam(2)) == doctest::Approx(0.15));
	CHECK(f.read(feathers::additiveOffGridParam(0)) == doctest::Approx(0.95));
	// AND NOTHING WAS LEFT BEHIND: slot 0's sound now has the knobs its own sound had (defaults, but Off-grid).
	CHECK(f.read(feathers::smoothnessParam(0))
	      == doctest::Approx(feathers::findParam(static_cast<clap_id>(feathers::smoothnessParam(0)))->def));
	CHECK(f.read(feathers::additiveRolloffParam(0))
	      == doctest::Approx(feathers::findParam(static_cast<clap_id>(feathers::additiveRolloffParam(0)))->def));
}

TEST_CASE("a cloned sound takes independent copies of its per-sound knobs, a delete resets them, and one undo puts each back") {
	StackFixture f;
	f.consume(0, 300.0);
	for (size_t k = 0; k < feathers::kPerSoundKnobCount; ++k) {
		const feathers::ParamDesc* desc = feathers::findParam(static_cast<clap_id>(feathers::perSoundKnobParam(0, k)));
		REQUIRE(desc != nullptr);
		f.set(feathers::perSoundKnobParam(0, k), desc->min + 0.3 * (desc->max - desc->min));
	}
	f.settle();
	const auto source = readSlotState(f.bridge, 0).knobs;
	REQUIRE(source != feathers::ui::untouchedKnobs(0)); // not vacuous: every knob moved off its default

	// CLONE: the copy lands in slot 1 with the source's knobs, every one.
	f.stack.clone(0);
	f.settle();
	CHECK(readSlotState(f.bridge, 1).knobs == source);
	// INDEPENDENT: a knob moved on the copy leaves the source's alone.
	f.set(feathers::smoothnessParam(1), 0.05);
	f.settle();
	CHECK(f.read(feathers::smoothnessParam(0)) == doctest::Approx(source[1]));
	// ONE UNDO (of the knob move, then of the clone) puts slot 1 back as it was before the clone.
	const auto empty = feathers::ui::untouchedKnobs(1);
	f.history.undo();
	f.settle();
	f.history.undo();
	f.settle();
	CHECK(readSlotState(f.bridge, 1).knobs == empty);
	f.history.redo();
	f.settle();
	CHECK(readSlotState(f.bridge, 1).knobs == source);

	// DELETE resets a sound's knobs to the table's, and its undo restores them.
	f.stack.deleteSound(0);
	f.settle();
	CHECK(readSlotState(f.bridge, 0).knobs == feathers::ui::untouchedKnobs(0));
	f.history.undo();
	f.settle();
	CHECK(readSlotState(f.bridge, 0).knobs == source);
}

namespace {
/// A like with every member a like stores set to values of its own, so a copy that dropped or mixed any of them differs.
feathers::TerrainLike likeFor(uint16_t engine, uint16_t slot, double seed) {
	feathers::TerrainLike like;
	like.x = 0.1 * seed;
	like.y = -0.07 * seed;
	like.home = 0.3 + seed;
	like.engine = engine;
	like.slot = slot;
	for (size_t b = 0; b < like.spectrum.size(); ++b) {
		like.spectrum[b] = static_cast<float>(seed + 0.01 * static_cast<double>(b));
	}
	like.pointCount = 7;
	for (size_t p = 0; p < like.pointCount; ++p) {
		like.point[p] = seed * 0.5 + static_cast<double>(p);
	}
	for (size_t a = 0; a < like.angles.size(); ++a) {
		like.angles[a] = static_cast<float>(std::fmod(seed * 0.13 + 0.001 * static_cast<double>(a), 1.0));
	}
	return like;
}

std::vector<feathers::TerrainLike> likesOn(const feathers::ParamBridge& bridge, size_t slot) {
	std::vector<feathers::TerrainLike> out;
	for (const feathers::TerrainLike& like : bridge.terrainLikes().likes) {
		if (like.slot == slot) {
			out.push_back(like);
		}
	}
	return out;
}

/// The likes as the plugin's own save writes them and its own load reads them back (audition.h).
struct LikesStream {
	std::vector<uint8_t> bytes;
	size_t at = 0;
	static int64_t CLAP_ABI writeFn(const clap_ostream* stream, const void* buffer, uint64_t size) {
		auto* self = static_cast<LikesStream*>(stream->ctx);
		const auto* in = static_cast<const uint8_t*>(buffer);
		self->bytes.insert(self->bytes.end(), in, in + size);
		return static_cast<int64_t>(size);
	}
	static int64_t CLAP_ABI readFn(const clap_istream* stream, void* buffer, uint64_t size) {
		auto* self = static_cast<LikesStream*>(stream->ctx);
		const size_t n = std::min<size_t>(size, self->bytes.size() - self->at);
		std::memcpy(buffer, self->bytes.data() + self->at, n);
		self->at += n;
		return static_cast<int64_t>(n);
	}
};
} // namespace

TEST_CASE("PC: a cloned sound takes independent copies of its likes on every engine, its neighbours' likes move with them, one undo puts them back, and the copies survive a save into a fresh instance") {
	StackFixture f;
	f.consume(0, 300.0);
	f.consume(1, 500.0);
	f.consume(2, 700.0);
	f.settle();
	// THE SOURCE'S LIKES ON TWO ENGINES (a clone keeps the sound, and the sound has likes on each), its neighbours' one each.
	const std::vector<feathers::TerrainLike> source { likeFor(0, 0, 1.0), likeFor(3, 0, 2.0), likeFor(0, 0, 3.0) };
	const feathers::TerrainLike onB = likeFor(0, 1, 4.0);
	const feathers::TerrainLike onC = likeFor(3, 2, 5.0);
	feathers::TerrainLikes made;
	made.likes = { source[0], onB, source[1], onC, source[2] };
	f.bridge.setTerrainLikes(made);
	REQUIRE(likesOn(f.bridge, 0).size() == 3);

	// CLONE A: the copy lands in slot 1, B moves to 2 and C to 3.
	f.stack.clone(0);
	f.settle();
	REQUIRE(f.zoneOf(1) == doctest::Approx(300.0));
	REQUIRE(f.zoneOf(2) == doctest::Approx(500.0));
	// THE SOURCE KEEPS ITS LIKES, value for value.
	CHECK(likesOn(f.bridge, 0) == source);
	// THE COPY HAS THE SAME LIKES BY VALUE — place, spectrum, full point, angles, engine — in the order they were made,
	// and only the sound they are keyed to differs.
	std::vector<feathers::TerrainLike> expected = source;
	for (feathers::TerrainLike& like : expected) {
		like.slot = 1;
	}
	CHECK(likesOn(f.bridge, 1) == expected);
	// THE SOUNDS THE SPLICE MOVED TOOK THEIR OWN LIKES WITH THEM, and nothing else arrived on them.
	feathers::TerrainLike movedB = onB;
	movedB.slot = 2;
	feathers::TerrainLike movedC = onC;
	movedC.slot = 3;
	CHECK(likesOn(f.bridge, 2) == std::vector<feathers::TerrainLike> { movedB });
	CHECK(likesOn(f.bridge, 3) == std::vector<feathers::TerrainLike> { movedC });
	const feathers::TerrainLikes cloned = f.bridge.terrainLikes();
	CHECK(cloned.likes.size() == made.likes.size() + source.size());

	// SAVE, THEN LOAD INTO A FRESH INSTANCE: the copies are patch state like any like.
	{
		LikesStream stream;
		const clap_ostream out { &stream, LikesStream::writeFn };
		REQUIRE(feathers::saveTerrainLikes(&out, f.bridge.terrainLikes()));
		const clap_istream in { &stream, LikesStream::readFn };
		feathers::TerrainLikes loaded;
		bool present = false;
		REQUIRE(feathers::loadTerrainLikes(&in, &loaded, &present));
		REQUIRE(present);
		feathers::ParamBridge fresh;
		fresh.restoreTerrainLikes(loaded);
		CHECK(fresh.terrainLikes() == cloned);
		CHECK(likesOn(fresh, 1) == expected);
		CHECK(likesOn(fresh, 0) == source);
	}

	// INDEPENDENT: unliking on the copy leaves the source's alone, and the other way round.
	{
		feathers::TerrainLikes edited = cloned;
		const auto first = std::find_if(edited.likes.begin(), edited.likes.end(),
		                                [](const feathers::TerrainLike& like) { return like.slot == 1; });
		REQUIRE(first != edited.likes.end());
		edited.likes.erase(first);
		f.bridge.setTerrainLikes(edited);
		CHECK(likesOn(f.bridge, 0) == source);
		CHECK(likesOn(f.bridge, 1).size() == source.size() - 1);
		edited = cloned;
		edited.likes.erase(edited.likes.begin()); // the source's first
		f.bridge.setTerrainLikes(edited);
		CHECK(likesOn(f.bridge, 1) == expected);
		CHECK(likesOn(f.bridge, 0).size() == source.size() - 1);
		f.bridge.setTerrainLikes(cloned);
	}

	// ONE UNDO PUTS THE LIKES BACK WITH THE SLOTS, and redo takes them forward again.
	f.history.undo();
	f.settle();
	CHECK(f.bridge.terrainLikes() == made);
	f.history.redo();
	f.settle();
	CHECK(f.bridge.terrainLikes() == cloned);
}

TEST_CASE("PC: a reordered sound carries its likes, and the sounds the splice shifts carry theirs, undoable in one step") {
	StackFixture f;
	f.consume(0, 300.0);
	f.consume(1, 500.0);
	f.consume(2, 700.0);
	f.settle();
	const feathers::TerrainLike onA0 = likeFor(0, 0, 1.0);
	const feathers::TerrainLike onA1 = likeFor(3, 0, 2.0);
	const feathers::TerrainLike onB = likeFor(0, 1, 3.0);
	const feathers::TerrainLike onC = likeFor(3, 2, 4.0);
	feathers::TerrainLikes made;
	made.likes = { onA0, onB, onA1, onC };
	f.bridge.setTerrainLikes(made);

	// A TO THIRD: B and C move up one, A lands in slot 2.
	f.stack.reorder(0, 2);
	f.settle();
	REQUIRE(f.zoneOf(0) == doctest::Approx(500.0));
	REQUIRE(f.zoneOf(2) == doctest::Approx(300.0));
	const auto at = [](feathers::TerrainLike like, uint16_t slot) {
		like.slot = slot;
		return like;
	};
	CHECK(likesOn(f.bridge, 2) == std::vector<feathers::TerrainLike> { at(onA0, 2), at(onA1, 2) });
	CHECK(likesOn(f.bridge, 0) == std::vector<feathers::TerrainLike> { at(onB, 0) });
	CHECK(likesOn(f.bridge, 1) == std::vector<feathers::TerrainLike> { at(onC, 1) });
	CHECK(f.bridge.terrainLikes().likes.size() == made.likes.size());
	const feathers::TerrainLikes moved = f.bridge.terrainLikes();

	CHECK(f.history.length() == 1);
	f.history.undo();
	f.settle();
	CHECK(f.bridge.terrainLikes() == made);
	f.history.redo();
	f.settle();
	CHECK(f.bridge.terrainLikes() == moved);
}

TEST_CASE("PC: a deleted sound's likes go with it on every engine, other sounds keep theirs, and one undo puts the sound and its likes back") {
	StackFixture f;
	f.consume(0, 300.0);
	f.consume(1, 500.0);
	f.settle();
	const feathers::TerrainLike onA = likeFor(0, 0, 1.0);
	const feathers::TerrainLike onB0 = likeFor(0, 1, 2.0);
	const feathers::TerrainLike onB1 = likeFor(3, 1, 3.0);
	feathers::TerrainLikes made;
	made.likes = { onB0, onA, onB1 };
	f.bridge.setTerrainLikes(made);
	const SlotState slotBefore = readSlotState(f.bridge, 1);

	f.stack.deleteSound(1);
	f.settle();
	CHECK(likesOn(f.bridge, 1).empty());
	CHECK(f.bridge.terrainLikes().likes == std::vector<feathers::TerrainLike> { onA });

	// ONE STEP for the sound and its likes.
	CHECK(f.history.length() == 1);
	f.history.undo();
	f.settle();
	CHECK(f.bridge.terrainLikes() == made);
	CHECK(readSlotState(f.bridge, 1) == slotBefore);
	f.history.redo();
	f.settle();
	CHECK(f.bridge.terrainLikes().likes == std::vector<feathers::TerrainLike> { onA });

	// A NEW SOUND MADE IN THE FREED SLOT STARTS WITH NO LIKES.
	f.consume(1, 900.0);
	f.settle();
	CHECK(likesOn(f.bridge, 1).empty());
}

TEST_CASE("a cloned sound takes its spectral drawer — every control and its drawn curve — and one undo puts the slot back") {
	// PER-SOUND SETTINGS FOLLOW A CLONED SOUND, the drawer included: its nineteen controls (switches,
	// Mode and Link among them, which no destination run holds) and its drawn curve, which is stored state.
	StackFixture f;
	f.consume(0, 300.0);
	for (size_t c = 0; c < feathers::kSpectralControlCount; ++c) {
		const ParamId id = feathers::spectralParam(0, static_cast<feathers::SpectralControl>(c));
		const feathers::ParamDesc* desc = feathers::findParam(static_cast<clap_id>(id));
		REQUIRE(desc != nullptr);
		f.set(id, desc->max); // every switch on, every control at its top: all off their defaults
	}
	f.bridge.setSpectralCurveOn(0, true);
	f.bridge.setSpectralCurveBand(0, 11, 0.4f);
	f.settle();
	const feathers::spectral::DrawnCurve curve = f.bridge.spectralCurve(0);

	f.stack.clone(0);
	f.settle();
	for (size_t c = 0; c < feathers::kSpectralControlCount; ++c) {
		const auto control = static_cast<feathers::SpectralControl>(c);
		CAPTURE(c);
		CHECK(f.read(feathers::spectralParam(1, control)) == doctest::Approx(f.read(feathers::spectralParam(0, control))));
		CHECK(f.read(feathers::spectralParam(1, control))
		      == doctest::Approx(feathers::findParam(static_cast<clap_id>(feathers::spectralParam(1, control)))->max));
	}
	CHECK(f.bridge.spectralCurve(1) == curve);
	// INDEPENDENT: the copy's curve drawn again leaves the source's alone.
	f.bridge.setSpectralCurveBand(1, 11, 0.9f);
	CHECK(f.bridge.spectralCurve(0) == curve);
	f.bridge.setSpectralCurveBand(1, 11, 0.4f);

	// ONE UNDO puts the slot back as it was: every control at its default, the curve flat and off.
	f.history.undo();
	f.settle();
	for (size_t c = 0; c < feathers::kSpectralControlCount; ++c) {
		const auto control = static_cast<feathers::SpectralControl>(c);
		CAPTURE(c);
		CHECK(f.read(feathers::spectralParam(1, control))
		      == doctest::Approx(feathers::findParam(static_cast<clap_id>(feathers::spectralParam(1, control)))->def));
	}
	CHECK(f.bridge.spectralCurve(1) == feathers::spectral::DrawnCurve {});
	f.history.redo();
	f.settle();
	CHECK(f.bridge.spectralCurve(1) == curve);
}

TEST_CASE("a reordered sound carries its spectral drawer — its stepped controls and its drawn curve — and a delete resets them, undoably") {
	// THE CLONE CASE'S CLAIM FOR THE OTHER TWO VERBS: the drawer's switches, Mode and Link (which no destination run holds)
	// and its drawn curve (stored state) move with a reordered sound and are reset with a deleted one.
	StackFixture f;
	f.consume(0, 300.0);
	f.consume(1, 500.0);
	f.consume(2, 700.0);
	for (const feathers::SpectralControl c : feathers::kSpectralSteppedControls) {
		const ParamId id = feathers::spectralParam(0, c);
		f.set(id, feathers::findParam(static_cast<clap_id>(id))->max); // every switch on, Mode Notch, Link at its top
	}
	f.set(feathers::spectralParam(0, feathers::SpectralControl::FormantShiftLink), 0.0); // Link's default is on: move it
	f.bridge.setSpectralCurveOn(0, true);
	f.bridge.setSpectralCurveBand(0, 9, 0.35f);
	f.settle();
	const feathers::spectral::DrawnCurve curve = f.bridge.spectralCurve(0);
	std::array<double, feathers::kSpectralSteppedControls.size()> stepped {};
	for (size_t i = 0; i < stepped.size(); ++i) {
		stepped[i] = f.read(feathers::spectralParam(0, feathers::kSpectralSteppedControls[i]));
	}

	f.dragCard(0, 2); // the first sound to the end of the list
	f.settle();
	REQUIRE(f.zoneOf(2) == doctest::Approx(300.0));
	for (size_t i = 0; i < stepped.size(); ++i) {
		CAPTURE(i);
		CHECK(f.read(feathers::spectralParam(2, feathers::kSpectralSteppedControls[i])) == doctest::Approx(stepped[i]));
		// AND NOTHING LEFT BEHIND: slot 0 now holds slot 1's sound, with a fresh sound's drawer.
		const ParamId there = feathers::spectralParam(0, feathers::kSpectralSteppedControls[i]);
		CHECK(f.read(there) == doctest::Approx(feathers::findParam(static_cast<clap_id>(there))->def));
	}
	CHECK(f.bridge.spectralCurve(2) == curve);
	CHECK(f.bridge.spectralCurve(0) == feathers::spectral::DrawnCurve {});

	// DELETE resets them; its undo restores them.
	f.stack.deleteSound(2);
	f.settle();
	for (const feathers::SpectralControl c : feathers::kSpectralSteppedControls) {
		const ParamId id = feathers::spectralParam(2, c);
		CHECK(f.read(id) == doctest::Approx(feathers::findParam(static_cast<clap_id>(id))->def));
	}
	CHECK(f.bridge.spectralCurve(2) == feathers::spectral::DrawnCurve {});
	f.history.undo();
	f.settle();
	for (size_t i = 0; i < stepped.size(); ++i) {
		CHECK(f.read(feathers::spectralParam(2, feathers::kSpectralSteppedControls[i])) == doctest::Approx(stepped[i]));
	}
	CHECK(f.bridge.spectralCurve(2) == curve);
}

// ---- STANCES CARRY THE SOUND'S KNOBS AND ITS SPECTRAL DRAWER (V18, V20) ------------------------------
namespace {
/// Every knob of `slot` at `share` of its travel (a switch or choice rounds to an index), so a stance that dropped one would
/// be caught by a value that moved.
void setEveryKnob(StackFixture& f, size_t slot, double share) {
	for (size_t k = 0; k < feathers::kPerSoundKnobCount; ++k) {
		const ParamId id = feathers::perSoundKnobParam(slot, k);
		const feathers::ParamDesc* desc = feathers::findParam(static_cast<clap_id>(id));
		REQUIRE(desc != nullptr);
		f.set(id, desc->min + share * (desc->max - desc->min));
	}
}
} // namespace

TEST_CASE("a stance carries the sound's knobs and its spectral drawer: a recall puts every knob and the drawn curve back") {
	StackFixture f;
	f.consume(0, 300.0);
	setEveryKnob(f, 0, 0.3);
	f.bridge.setSpectralCurveOn(0, true);
	f.bridge.setSpectralCurveBand(0, 4, 0.6f);
	f.settle();
	const RideAlongValues stored = f.stack.liveRideAlong(0);
	REQUIRE(stored.knobs != feathers::ui::untouchedKnobs(0)); // not vacuous
	f.stack.storeVariationAt(0); // banks the exploration at row 0; the copy is the record edits hit
	f.settle();
	REQUIRE(f.stack.activeStance(0, 1));

	// WANDER every knob and the curve.
	setEveryKnob(f, 0, 0.8);
	f.bridge.setSpectralCurveBand(0, 4, 1.7f);
	f.bridge.setSpectralCurveOn(0, false);
	f.settle();
	const RideAlongValues wandered = f.stack.liveRideAlong(0);
	REQUIRE(wandered.knobs != stored.knobs);
	REQUIRE_FALSE(wandered.curve == stored.curve);

	// THE RECALL puts every knob back, the drawer's nineteen among them, and the curve, by value.
	f.stack.activateVariation(0, 0);
	f.settle();
	for (size_t k = 0; k < feathers::kPerSoundKnobCount; ++k) {
		CAPTURE(k);
		CHECK(f.read(feathers::perSoundKnobParam(0, k)) == doctest::Approx(stored.knobs[k]));
	}
	CHECK(f.bridge.spectralCurve(0) == stored.curve);
	CHECK(f.stack.liveRideAlong(0) == stored);
	// AND THE OUTGOING RECORD banked the wander, knobs and curve: back again restores it.
	CHECK(f.stack.variationValuesAt(0, 1) == wandered);
	f.stack.activateVariation(0, 1);
	f.settle();
	CHECK(f.bridge.spectralCurve(0) == wandered.curve);
	CHECK(f.stack.liveRideAlong(0) == wandered);
}

TEST_CASE("two stances of one sound with different drawers switch the drawer the voice is handed") {
	// THE ENGINE'S SIDE OF A RECALL: the stance writes the params, and the Additive voice's drawer follows — asserted on the
	// ShaperParams the synth hands the voice, not on a render.
	StackFixture f;
	f.consume(0, 300.0);
	f.set(ParamId::OscType, static_cast<double>(OscType::Additive));
	f.set(ParamId::VoiceLevel, 1.0);
	f.set(ParamId::Level, 1.0);
	f.set(feathers::spectralParam(0, feathers::SpectralControl::FilterOn), 1.0);
	f.set(feathers::spectralParam(0, feathers::SpectralControl::FilterCutoff), std::log2(500.0));
	f.settle();
	f.stack.storeVariationAt(0);
	f.settle();
	f.set(feathers::spectralParam(0, feathers::SpectralControl::FilterCutoff), std::log2(3000.0));
	f.set(feathers::spectralParam(0, feathers::SpectralControl::CombOn), 1.0);
	f.settle();

	feathers::Synth synth { f.params };
	synth.activate(48000.0, 4096);
	std::vector<float> l(256), r(256);
	const auto drawerNow = [&] {
		synth.setSpectralState(f.bridge.spectralState());
		for (int b = 0; b < 8; ++b) {
			synth.render(l.data(), r.data(), 256);
		}
		return synth.voiceStackOscForTest(synth.newestVoiceSlotForTest(), 0).drawerForTest(0);
	};
	synth.render(l.data(), r.data(), 256);
	synth.noteOn(0, 48, 1, 1.0);
	const feathers::spectral::ShaperParams onStance1 = drawerNow();
	CHECK(onStance1.filter.cutoffHz == doctest::Approx(3000.0).epsilon(1e-5));
	CHECK(onStance1.comb.on);

	f.stack.activateVariation(0, 0);
	f.settle();
	const feathers::spectral::ShaperParams onStance0 = drawerNow();
	CHECK(onStance0.filter.cutoffHz == doctest::Approx(500.0).epsilon(1e-5));
	CHECK_FALSE(onStance0.comb.on);

	f.stack.activateVariation(0, 1);
	f.settle();
	CHECK(drawerNow() == onStance1);
}

TEST_CASE("an empty variation list leaves today's behaviour: the knobs and the drawer are the parameters, untouched by any stance") {
	// A SOUND THAT HAS STORED NOTHING has the one implicit stance its parameters hold: its knobs and curve read from the
	// live state, and nothing writes them.
	StackFixture f;
	f.consume(0, 300.0);
	setEveryKnob(f, 0, 0.4);
	f.bridge.setSpectralCurveOn(0, true);
	f.settle();
	REQUIRE(f.bridge.soundVariations().sound[0].empty());
	const RideAlongValues live = f.stack.variationValuesAt(0, 0);
	for (size_t k = 0; k < feathers::kPerSoundKnobCount; ++k) {
		CHECK(live.knobs[k] == f.read(feathers::perSoundKnobParam(0, k)));
	}
	CHECK(live.curve == f.bridge.spectralCurve(0));
	CHECK(f.stack.variationRows(0) == 1);
	// AND ANOTHER SOUND'S EDIT moves nothing here.
	f.consume(1, 500.0);
	setEveryKnob(f, 1, 0.9);
	f.settle();
	CHECK(f.stack.variationValuesAt(0, 0) == live);
}

TEST_CASE("a stance's curve is validated on its way to the engine: a recall pins a bad band, and the clone and undo road maps NaN to unity") {
	// THE WIRE CASTS ANY DOUBLE (setRideAlongWireValue), so a corrupt record can hold any band. ParamBridge::setSpectralCurve —
	// the road a recall, a clone and an undo all take — applies the loader's rule (stances review, 2026-10-10).
	StackFixture f;
	f.consume(0, 300.0);
	f.settle();
	feathers::SoundVariations::List list;
	list.count = 2;
	list.active = 1;
	list.values[0] = f.stack.liveRideAlong(0);
	list.values[0].curve.on = true;
	list.values[0].curve.gain[0] = -1.0f;
	list.values[0].curve.gain[1] = 9.0f;
	list.values[1] = f.stack.liveRideAlong(0);
	f.bridge.beginSoundVariationsGesture();
	f.bridge.setSoundVariations(0, list);
	f.bridge.endSoundVariationsGesture();
	f.stack.activateVariation(0, 0);
	f.settle();
	CHECK(f.bridge.spectralCurve(0).on);
	CHECK(f.bridge.spectralCurve(0).gain[0] == 0.0f);                             // −1: the floor
	CHECK(f.bridge.spectralCurve(0).gain[1] == feathers::kSpectralCurveGainMax); // 9: the shared bound
	CHECK(f.bridge.spectralCurve(0).gain[2] == 1.0f);                             // the rest as stored

	// A NaN IN A STANCE: the stance is not recallable at all (sound_variations.h: refuse, never half-apply), so the curve the
	// sound holds stands — and the bridge's own road, which a clone or an undo takes, maps a NaN band to unity.
	list.active = 0;
	list.values[1].curve.on = true;
	list.values[1].curve.gain[5] = std::numeric_limits<float>::quiet_NaN();
	f.bridge.beginSoundVariationsGesture();
	f.bridge.setSoundVariations(0, list);
	f.bridge.endSoundVariationsGesture();
	const feathers::spectral::DrawnCurve before = f.bridge.spectralCurve(0);
	f.stack.activateVariation(0, 1);
	f.settle();
	CHECK(f.bridge.spectralCurve(0) == before);
	feathers::spectral::DrawnCurve nan;
	nan.on = true;
	nan.gain[5] = std::numeric_limits<float>::quiet_NaN();
	f.bridge.setSpectralCurve(0, nan);
	CHECK(f.bridge.spectralCurve(0).gain[5] == 1.0f);
}

TEST_CASE("visage v1: every glyph our font carries measures its settled width on its first lookup, so a label "
          "is laid out at its real width on the first draw") {
	// THE QUESTION glyph-heal-check's red raised on visage v1.0.0 (2026-11-05), asked as values in a
	// suite that runs. That tool walks U+0021..U+0320 and found U+0300..U+0320 measuring ZERO on their
	// first lookup. MEASURED with FreeType directly: the Lato we embed has NO glyph for any of them, and
	// v1.0.0 sends a missing glyph to a fontconfig system FALLBACK font (`fallbackTypeFace`), where a
	// combining mark's advance is genuinely zero — the old pin drew Lato's .notdef box at 9 px instead.
	// So that red was the fallback road, not a glyph handed out before it had a home. THIS case pins the
	// half that matters for layout: every codepoint Lato carries (printable ASCII and U+00A0..U+00FF —
	// measured: Lato has all of them and none at zero advance) measures non-zero on its first lookup,
	// the same on a second, and the same after the atlas has been churned. A deferral like tier 2's
	// would read zero first and non-zero later, which is a label measured short on its first draw.
	//
	// A SIZE NOTHING ELSE ASKS FOR (37 px), so the first lookup really is the first one in this binary.
	const visage::Font& cold = ui::font(37.0f);
	std::u32string carried;
	for (char32_t c = 0x21; c < 0x7f; ++c) {
		carried.push_back(c);
	}
	for (char32_t c = 0xa0; c < 0x100; ++c) {
		carried.push_back(c);
	}

	std::vector<float> first;
	first.reserve(carried.size());
	int zeros = 0;
	for (char32_t c : carried) {
		const float w = cold.stringWidth(std::u32string(1, c));
		if (w <= 0.0f) {
			zeros++;
			MESSAGE("U+" << std::hex << static_cast<unsigned>(c) << " measured " << w << " on its first lookup");
		}
		first.push_back(w);
	}
	CHECK(zeros == 0);

	// AND A LABEL MEASURED COLD IS THE SUM OF ITS SETTLED GLYPHS: the claim a first draw rests on.
	const float wholeCold = ui::font(37.0f).stringWidth(carried);
	// Churn the atlas (another block, the same size), then measure every glyph again.
	std::u32string churn;
	for (char32_t c = 0x100; c < 0x180; ++c) {
		churn.push_back(c);
	}
	(void)cold.stringWidth(churn);
	int moved = 0;
	float sum = 0.0f;
	for (size_t i = 0; i < carried.size(); ++i) {
		const float settled = cold.stringWidth(std::u32string(1, carried[i]));
		sum += settled;
		if (settled != first[i]) {
			moved++;
		}
	}
	CHECK(moved == 0);
	CHECK(wholeCold == doctest::Approx(sum).epsilon(1e-5));
	// NON-VACUITY: the widths are real ones, not a font that measured nothing everywhere.
	CHECK(sum > 100.0f * 37.0f * 0.3f);
}

TEST_CASE("voice Engine rail on drums (drum proposal §15): a Kit hides the voice mode, count, priority and glide; Pitched draws them; stack, pitch and bend stay") {
	ParamBridge bridge;
	feathers::BridgeDeviceHost deviceHost { bridge };
	WidgetRegistry registry;
	EngineRail engineRail(bridge, registry);
	engineRail.setBounds(0.0f, 0.0f, EngineRail::kWidth, 700.0f);
	ParamSet params;
	const auto set = [&](ParamId id, double v) {
		params.setBase(static_cast<clap_id>(id), v);
		bridge.publish(params);
		bridge.refresh();
		engineRail.refreshEnablement();
	};
	set(ParamId::VoiceMode, static_cast<double>(VoiceMode::Mono)); // so Glide would be live off a kit
	set(ParamId::OscType, static_cast<double>(OscType::Drum));
	struct Row {
		const char* name;
		visage::Frame* frame;
		bool kit;
		bool pitched;
	};
	const Row rows[] = {
		{ "voice mode", engineRail.voiceModeForTest(), false, true },
		{ "voice count", engineRail.voiceCountForTest(), false, true },
		{ "note priority", engineRail.notePriorityForTest(), false, true },
		{ "glide", engineRail.glideForTest(), false, true },
		{ "transpose (pitch page)", engineRail.transposeOctForTest(), true, true },
		{ "bend range", engineRail.bendRangeForTest(), true, true },
		{ "stack", engineRail.stackCountForTest(), true, true },
		{ "phase", engineRail.phaseForTest(), false, false },
		{ "osc mode", engineRail.oscModeForTest(), false, false },
	};
	for (const double mode : { 1.0, 0.0 }) {
		set(ParamId::DrumMode, mode);
		for (const Row& r : rows) {
			CAPTURE(r.name);
			CAPTURE(mode);
			CHECK(r.frame->isVisible() == (mode == 1.0 ? r.kit : r.pitched));
		}
	}
	// AND OFF DRUM, everything a Mono V5 draws comes back.
	set(ParamId::OscType, static_cast<double>(OscType::V5));
	CHECK(engineRail.voiceModeForTest()->isVisible());
	CHECK(engineRail.glideForTest()->isVisible());
}

// ============================================================================================== //
// ZONE DRAG FEEL — backlog QB                                                                     //
// ============================================================================================== //
//
// The zone field's multi-rate drag felt too slow even on the large digits, then fast on a second grab. The
// goal: a fast top speed, a precise slow speed, and acceleration between them.

namespace {

namespace qb {

namespace accel = feathers::ui::drag_accel;

/// A CLOCK THE CASE TURNS BY HAND, installed for the case's lifetime (main.cpp stops it again at the next case).
struct HandClock {
	double now = 1000.0;
	HandClock() {
		accel::setClockForTest([this] { return now; });
	}
	~HandClock() { accel::setClockForTest([] { return 0.0; }); }
};

/// OWLTT's own Character names, in the selector's shape.
std::vector<ui::ZoneCoordCell::ZoneName> characterNames() {
	std::vector<ui::ZoneCoordCell::ZoneName> out;
	for (const auto& n : feathers::fx::owltt::kCharacterZoneNames) {
		out.push_back({ n.name, n.description });
	}
	return out;
}

/// OWLTT's Character — a zoned, unbounded coordinate — in the cell every zoned surface seats it in.
struct CharacterCell {
	ModFixture f;
	size_t slot = f.bridge.addFxDevice(feathers::fx::owltt::kTraits.tag);
	ui::DeviceControl control { f.deviceHost, f.bridge.fxDeviceAt(slot), feathers::fx::owltt::kDescriptor,
		                        static_cast<size_t>(feathers::fx::owltt::Param::Character), nullptr, nullptr };
	ui::ZoneCoordCell cell { control, "Character", characterNames() };
	CharacterCell() { cell.setBounds(0, 0, 90, 110); }
	ui::ValueBox& box() { return cell.box(); }

	/// WHERE THE COLUMN WORTH `place` IS, asked of the box's own hit test.
	float xOf(double place) {
		for (float x = 0.0f; x < box().width(); x += 0.25f) {
			if (box().placeAt(x) == doctest::Approx(place)) {
				return x;
			}
		}
		return -1.0f;
	}

	/// ONE GESTURE: press on the column worth `place`, `events` moves of `dy` px upward, each `dtMs` apart on
	/// `clock`, release. Answers how far the value moved.
	double drag(HandClock& clock, double start, double place, int events, float dy, double dtMs,
	             bool shift = false) {
		control.setValue(start);
		const float x = xOf(place);
		REQUIRE(x >= 0.0f);
		float y = 50.0f;
		visage::MouseEvent press = at(x, y);
		if (shift) {
			press.modifiers |= visage::kModifierShift;
		}
		box().processMouseDown(press);
		REQUIRE(box().pressPlaceForTest() == doctest::Approx(place));
		for (int i = 0; i < events; ++i) {
			clock.now += dtMs;
			y -= dy;
			visage::MouseEvent move = at(x, y);
			move.modifiers = press.modifiers;
			box().processMouseDrag(move);
		}
		box().processMouseUp(at(x, y));
		return control.value() - start;
	}
};

/// THE DIGIT'S OWN RATE, value per pixel: `kUnitsPerPlaceDrag` of the place across `kDragRange`.
double baseRate(double place) {
	return place * ui::ValueBox::kUnitsPerPlaceDrag / static_cast<double>(ui::ValueBox::kDragRange);
}

/// **AN INDEPENDENT RESTATEMENT OF THE INTENDED ACCELERATION** — pooled travel per window of at least
/// `kWindowMs`, an exponential average over `kSmoothingMs`, each event's travel at the gain of the speed known
/// once that event is counted. Written from the header's prose, not called through `Tracker`.
double restatedTravel(int events, double dy, double dtMs) {
	double speed = 0.0;
	double windowTravel = 0.0;
	double windowElapsed = 0.0;
	double travel = 0.0;
	for (int i = 0; i < events; ++i) {
		windowTravel += dy;
		windowElapsed += dtMs;
		if (windowElapsed >= accel::kWindowMs) {
			speed += (windowTravel / windowElapsed - speed) * (1.0 - std::exp(-windowElapsed / accel::kSmoothingMs));
			windowTravel = 0.0;
			windowElapsed = 0.0;
		}
		travel += dy * accel::gain(speed);
	}
	return travel;
}

} // namespace qb

} // namespace

TEST_CASE("zone drag: the acceleration curve is 1 when slow, its maximum when fast, and never falls") {
	namespace accel = feathers::ui::drag_accel;
	CHECK(accel::gain(0.0) == 1.0);
	CHECK(accel::gain(accel::kSlowSpeed) == 1.0);
	CHECK(accel::gain(accel::kFastSpeed) == doctest::Approx(accel::kMaxGain));
	CHECK(accel::gain(accel::kFastSpeed * 10.0) == doctest::Approx(accel::kMaxGain));
	// STRICTLY ABOVE 1 the moment the hand is past slow, and strictly below the maximum short of fast.
	const double mid = (accel::kSlowSpeed + accel::kFastSpeed) * 0.5;
	CHECK(accel::gain(mid) > 1.0);
	CHECK(accel::gain(mid) < accel::kMaxGain);
	CHECK(accel::gain(mid) == doctest::Approx(1.0 + (accel::kMaxGain - 1.0) * 0.5)); // smoothstep's midpoint
	double previous = 0.0;
	for (double speed = 0.0; speed <= accel::kFastSpeed * 2.0; speed += 0.001) {
		const double g = accel::gain(speed);
		CHECK_MESSAGE(g >= previous, "the curve fell at " << speed << " px/ms");
		previous = g;
	}
}

TEST_CASE("zone drag: a slow drag moves at exactly the grabbed digit's own rate") {
	qb::HandClock clock;
	qb::CharacterCell c;
	REQUIRE(c.box().accelerated());
	// ONE PIXEL EVERY 16 ms — 0.0625 px/ms, well under `kSlowSpeed` — for 70 px, on the tenths of "1.00".
	const double moved = c.drag(clock, 1.0, 0.1, 70, 1.0f, 16.0);
	CHECK(moved == doctest::Approx(70.0 * qb::baseRate(0.1)).epsilon(1e-9));
	// AND THE UNITS, so the claim is per digit and not about one column.
	const double units = c.drag(clock, 1.0, 1.0, 70, 1.0f, 16.0);
	CHECK(units == doctest::Approx(70.0 * qb::baseRate(1.0)).epsilon(1e-9));
}

TEST_CASE("zone drag: a fast drag moves by the acceleration curve, above the digit's own rate") {
	qb::HandClock clock;
	qb::CharacterCell c;
	// TWENTY PIXELS EVERY 8 ms — 2.5 px/ms, a flick — for 140 px of pointer travel, on the tenths.
	constexpr int kEvents = 7;
	constexpr float kDy = 20.0f;
	constexpr double kDt = 8.0;
	const double moved = c.drag(clock, 1.0, 0.1, kEvents, kDy, kDt);
	const double plain = kEvents * kDy * qb::baseRate(0.1);
	// THE VALUE, from the restated curve...
	CHECK(moved == doctest::Approx(qb::restatedTravel(kEvents, kDy, kDt) * qb::baseRate(0.1)).epsilon(1e-6));
	// ...and the comparison that proves the first assertion is not vacuous: it is faster than the digit alone,
	// and no faster than the maximum allows.
	CHECK(moved > 2.0 * plain);
	CHECK(moved <= feathers::ui::drag_accel::kMaxGain * plain + 1e-9);
	// A LONG FLICK REACHES THE MAXIMUM: the last pixels of a long fast drag go at `kMaxGain` times the digit.
	const double longFlick = c.drag(clock, 1.0, 0.1, 60, kDy, kDt);
	const double shorter = c.drag(clock, 1.0, 0.1, 59, kDy, kDt);
	CHECK((longFlick - shorter) / (kDy * qb::baseRate(0.1)) == doctest::Approx(feathers::ui::drag_accel::kMaxGain).epsilon(0.01));
}

TEST_CASE("zone drag: Shift is the precise override, and never accelerates") {
	qb::HandClock clock;
	qb::CharacterCell c;
	const double moved = c.drag(clock, 1.0, 0.1, 7, 20.0f, 8.0, true);
	CHECK(moved == doctest::Approx(140.0 * qb::baseRate(0.1) * ui::ValueBox::kFineFactor).epsilon(1e-6));
}

TEST_CASE("zone drag: two identical gestures move the coordinate by the same amount") {
	// **THE FIRST-GRAB / SECOND-GRAB REPORT, ASKED BY VALUE.** A fast gesture then a slow one, then the same pair
	// again: each gesture must answer exactly what its twin did, so no speed, digit or anchor crosses from one
	// gesture to the next.
	qb::HandClock clock;
	qb::CharacterCell c;
	const double fastA = c.drag(clock, 1.0, 0.1, 7, 20.0f, 8.0);
	const double slowA = c.drag(clock, 1.0, 0.1, 70, 1.0f, 16.0);
	const double fastB = c.drag(clock, 1.0, 0.1, 7, 20.0f, 8.0);
	const double slowB = c.drag(clock, 1.0, 0.1, 70, 1.0f, 16.0);
	CHECK(fastA == fastB);
	CHECK(slowA == slowB);
	// A SLOW GESTURE STRAIGHT AFTER A FAST ONE IS SLOW — the fast one's speed did not survive the release.
	CHECK(slowA == doctest::Approx(70.0 * qb::baseRate(0.1)).epsilon(1e-9));
}

TEST_CASE("zone drag: a grab aimed at the big digit moves at its rate even a pixel beside the glyph") {
	// **THE FIRST-GRAB-SLOW / SECOND-GRAB-FAST BUG, BY VALUE.** No state crosses gestures (the case above); what
	// differed between two such grabs was ONE PIXEL. A digit is ~5 px wide and most of a zone box is padding, and a
	// press on padding used to latch the FINEST column — so a grab that landed just left of the units digit of "1.50"
	// crawled at a hundredth per tenth of a drag, and the next grab, landing on the glyph, went 100× faster.
	// Two gestures, identical but for that pixel, must move the coordinate by the same amount.
	qb::HandClock clock; // stopped, so neither gesture accelerates — this is about the digit, not the speed
	clock.now = 0.0;
	qb::CharacterCell c;
	c.control.setValue(1.5);
	REQUIRE(c.box().readout() == "1.50");
	// THE UNITS GLYPH'S LEFT EDGE, asked of the box's own hit test.
	float glyphLeft = -1.0f;
	for (float x = 0.0f; x < c.box().width(); x += 0.25f) {
		if (c.box().placeAt(x) == doctest::Approx(1.0)) {
			glyphLeft = x;
			break;
		}
	}
	REQUIRE(glyphLeft > 1.0f);
	REQUIRE(c.box().placeAt(glyphLeft - 1.0f) == 0.0); // genuinely beside the glyph, on padding
	REQUIRE(c.box().nearestDigitPress());

	const auto dragAt = [&](float x) {
		c.control.setValue(1.5);
		c.box().processMouseDown(at(x, 50.0f));
		for (int i = 1; i <= 35; ++i) {
			c.box().processMouseDrag(at(x, 50.0f - static_cast<float>(i)));
		}
		c.box().processMouseUp(at(x, 15.0f));
		return c.control.value() - 1.5;
	};
	const double onGlyph = dragAt(glyphLeft + 1.0f);
	const double beside = dragAt(glyphLeft - 1.0f);
	CHECK(onGlyph == doctest::Approx(35.0 * qb::baseRate(1.0)));
	CHECK(beside == doctest::Approx(onGlyph));
	// AND FAR LEFT OF THE NUMBER IS STILL ITS LEADING DIGIT; FAR RIGHT ITS LAST — the box's left is coarse, its
	// right fine, which is what a hand aiming at a side of a short number means.
	CHECK(c.box().pressPlaceAt(0.5f) == doctest::Approx(1.0));
	CHECK(c.box().pressPlaceAt(c.box().width() - 0.5f) == doctest::Approx(0.01));
}

TEST_CASE("momentary chip: a hold whose release never arrived lets go at the first move with no button down (Pattern's Go stuck on,)") {
	// A RELEASE CAN BE LOST: the host or the window manager takes the pointer between the press and the release (a DAW
	// stealing focus on a click, a modal opening), and visage never delivers the mouse-up. The chip then holds max for ever
	// — Go lit, its trigger never re-armed. A plain move (no button down) is proof the hand already let go.
	ChipSpy spy;
	ui::SwitchChip chip { spy, "Go" };
	chip.setBounds(0, 0, 60, ui::SwitchChip::kHeight);
	chip.setBehaviour(ui::SwitchChip::Behaviour::Momentary);
	chip.onMouseDown().callback(at(30.0f, 8.0f));
	REQUIRE(chip.on());
	// A DRAG (the button still down) keeps it held.
	visage::MouseEvent drag = at(32.0f, 8.0f);
	drag.button_state = visage::kMouseButtonLeft;
	chip.onMouseMove().callback(drag);
	CHECK(chip.on());
	CHECK(spy.ends == 0);
	// A MOVE WITH NOTHING DOWN: the release that was lost.
	visage::MouseEvent hover = at(34.0f, 8.0f);
	hover.button_state = 0;
	chip.onMouseMove().callback(hover);
	CHECK_FALSE(chip.on());
	CHECK(spy.v == -2.0);
	CHECK(spy.ends == 1);
}

namespace {

namespace fine {

/// A SHIFT-HELD EVENT: `at` with the modifier the hand's Shift key sets.
visage::MouseEvent shifted(float x, float y) {
	visage::MouseEvent e = at(x, y);
	e.modifiers |= visage::kModifierShift;
	return e;
}

/// THE FILTER'S RESONANCE, a 0..1 percent control, over a real device on a real bridge — the one quantity all four
/// widgets are asked about, so the four numbers below are the same arithmetic restated four times.
struct Resonance {
	ModFixture f;
	DeviceInstanceId device = kNoInstance;
	std::unique_ptr<ui::DeviceControl> control;
	Resonance() {
		const size_t slot = f.bridge.addFxDevice(fx::filter::kTraits.tag);
		REQUIRE(slot < feathers::kMaxFxSlots);
		device = f.bridge.fxDeviceAt(slot);
		control = std::make_unique<ui::DeviceControl>(f.deviceHost, device, fx::filter::kDescriptor,
		                                               static_cast<size_t>(fx::filter::Param::Resonance));
		control->setValue(0.2);
	}
};

constexpr float kTravel = 28.0f;

} // namespace fine

} // namespace

TEST_CASE("fine adjust is one rule on every drag: Shift mid-drag re-anchors, and the travel after it is worth kFineAdjust — Knob") {
	// **THE SAME EXPERIMENT ON EACH OF THE FOUR DRAGGABLES** (Knob, ValueBox, FaderPill, a route's amount): drag N px
	// coarse, press Shift, drag N px more. The value must (1) not jump at the modifier change — an event at the same point
	// with Shift newly down leaves it where it was — and (2) then move by `kFineAdjust` of what N px was worth.
	// Expected values are restated from the range and the pixel rate, not read off the widget.
	fine::Resonance r;
	ui::Knob knob(*r.control, "Res", nullptr);
	knob.setBounds(0.0f, 0.0f, 52.0f, ui::Knob::kHeight);
	const float x = 26.0f;
	const float y0 = 40.0f;
	const double perPixel = 1.0 / static_cast<double>(ui::Knob::kDragRange); // range is 0..1
	knob.processMouseDown(at(x, y0));
	knob.processMouseDrag(at(x, y0 - fine::kTravel));
	const double coarse = 0.2 + static_cast<double>(fine::kTravel) * perPixel;
	CHECK(r.control->value() == doctest::Approx(coarse).epsilon(1e-6));
	knob.processMouseDrag(fine::shifted(x, y0 - fine::kTravel)); // the key goes down, the hand has not moved
	CHECK(r.control->value() == doctest::Approx(coarse).epsilon(1e-6)); // no jump
	knob.processMouseDrag(fine::shifted(x, y0 - 2.0f * fine::kTravel));
	CHECK(r.control->value() == doctest::Approx(coarse + static_cast<double>(fine::kTravel) * perPixel * ui::kFineAdjust).epsilon(1e-6));
	knob.processMouseUp(fine::shifted(x, y0 - 2.0f * fine::kTravel));
}

TEST_CASE("fine adjust is one rule on every drag: Shift mid-drag re-anchors, and the travel after it is worth kFineAdjust — ValueBox") {
	fine::Resonance r;
	ui::ValueBox box(*r.control, "Res");
	box.setDragSpan(1.0); // one whole range per kDragRange px, so the arithmetic is the knob's
	box.setBounds(0.0f, 0.0f, 80.0f, ui::ValueBox::kHeight);
	const float x = 40.0f;
	const float y0 = 60.0f;
	const double perPixel = 1.0 / static_cast<double>(ui::ValueBox::kDragRange);
	box.processMouseDown(at(x, y0));
	box.processMouseDrag(at(x, y0 - fine::kTravel));
	const double coarse = 0.2 + static_cast<double>(fine::kTravel) * perPixel;
	CHECK(r.control->value() == doctest::Approx(coarse).epsilon(1e-6));
	box.processMouseDrag(fine::shifted(x, y0 - fine::kTravel));
	CHECK(r.control->value() == doctest::Approx(coarse).epsilon(1e-6)); // no jump
	box.processMouseDrag(fine::shifted(x, y0 - 2.0f * fine::kTravel));
	CHECK(r.control->value() == doctest::Approx(coarse + static_cast<double>(fine::kTravel) * perPixel * ui::kFineAdjust).epsilon(1e-6));
	// AND LETTING SHIFT GO RE-ANCHORS BACK to the coarse rate, from where the value is now.
	const double fineValue = r.control->value();
	box.processMouseDrag(at(x, y0 - 2.0f * fine::kTravel));
	CHECK(r.control->value() == doctest::Approx(fineValue).epsilon(1e-6));
	box.processMouseDrag(at(x, y0 - 3.0f * fine::kTravel));
	CHECK(r.control->value() == doctest::Approx(fineValue + static_cast<double>(fine::kTravel) * perPixel).epsilon(1e-6));
	box.processMouseUp(at(x, y0 - 3.0f * fine::kTravel));
}

TEST_CASE("fine adjust is one rule on every drag: Shift mid-drag re-anchors, and the travel after it is worth kFineAdjust — FaderPill") {
	fine::Resonance r;
	ui::FaderPill pill(*r.control, "Res");
	pill.setBounds(0.0f, 0.0f, 160.0f, 18.0f);
	const float y = 9.0f;
	const float x0 = 20.0f;
	const double perPixel = 1.0 / static_cast<double>(ui::FaderPill::kSpan); // range is 0..1
	pill.processMouseDown(at(x0, y));
	pill.processMouseDrag(at(x0 + fine::kTravel, y));
	const double coarse = 0.2 + static_cast<double>(fine::kTravel) * perPixel;
	CHECK(r.control->value() == doctest::Approx(coarse).epsilon(1e-6));
	pill.processMouseDrag(fine::shifted(x0 + fine::kTravel, y));
	CHECK(r.control->value() == doctest::Approx(coarse).epsilon(1e-6)); // no jump
	pill.processMouseDrag(fine::shifted(x0 + 2.0f * fine::kTravel, y));
	CHECK(r.control->value() == doctest::Approx(coarse + static_cast<double>(fine::kTravel) * perPixel * ui::kFineAdjust).epsilon(1e-6));
	pill.processMouseUp(fine::shifted(x0 + 2.0f * fine::kTravel, y));
}

TEST_CASE("fine adjust is one rule on every drag: Shift mid-drag re-anchors, and the travel after it is worth kFineAdjust — a route's amount") {
	ModFixture f;
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);
	f.arm(backplane::Source::core(0));
	REQUIRE(mod->armed());
	ui::Knob* path = f.knobFor(ParamId::Path);
	REQUIRE(path != nullptr);
	const int destination = modDestinationIndexOf(ParamId::Path);
	REQUIRE(destination >= 0);
	const backplane::Destination dest = backplane::Destination::core(static_cast<size_t>(destination));
	const visage::Point face = f.faceCentre(path);
	dragThroughRoot(f.root, face, { face.x, face.y - 35.0f });
	const auto amountNow = [&] { return f.bridge.routeAmount(backplane::Source::core(0), dest); };
	const float made = amountNow();
	REQUIRE(made > 0.0f);
	auto* list = findInTree<ui::RouteList>(f.root);
	REQUIRE(list != nullptr);
	f.root->resized();
	// A POINT ON THE ROW'S AMOUNT, asked of the list.
	const auto rows = list->rows();
	size_t index = rows.size();
	for (size_t i = 0; i < rows.size(); ++i) {
		if (rows[i] == dest) {
			index = i;
		}
	}
	REQUIRE(index < rows.size());
	const visage::Bounds row = list->rowBounds(index);
	const float y0 = row.y() + row.height() * 0.5f;
	float x = -1.0f;
	for (float px = row.x() + 1.0f; px < row.right(); px += 1.0f) {
		const ui::RouteList::Hit hit = list->hitAt({ px, y0 });
		if (hit.part == ui::RouteList::Part::Amount && hit.destination == dest) {
			x = px;
			break;
		}
	}
	REQUIRE(x >= 0.0f);
	const double perPixel = 1.0 / static_cast<double>(ui::RouteList::kDragRange);
	// SMALL TRAVELS: the amount is clamped, so stay well inside it.
	const float n = 8.0f;
	list->processMouseDown(at(x, y0));
	list->processMouseDrag(at(x, y0 - n));
	const double coarse = static_cast<double>(made) + static_cast<double>(n) * perPixel;
	CHECK(static_cast<double>(amountNow()) == doctest::Approx(coarse).epsilon(1e-4));
	list->processMouseDrag(fine::shifted(x, y0 - n));
	CHECK(static_cast<double>(amountNow()) == doctest::Approx(coarse).epsilon(1e-4)); // no jump
	list->processMouseDrag(fine::shifted(x, y0 - 2.0f * n));
	CHECK(static_cast<double>(amountNow())
	      == doctest::Approx(coarse + static_cast<double>(n) * perPixel * ui::kFineAdjust).epsilon(1e-4));
	list->processMouseUp(fine::shifted(x, y0 - 2.0f * n));
}
