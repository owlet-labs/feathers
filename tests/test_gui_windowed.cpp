/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

// THE EXPANDED VIEW IN A WINDOW OF ITS OWN — its own binary, and the separate PROCESS is the reason.
//
// These cases open a REAL top-level OS window. Measured on the branch this arrived from: the same
// case passes alone and SEGFAULTS when it runs after several hundred other cases in one process, so a
// second visage window is not safe in a process that has already built and torn down many editors.
// That is recorded rather than hidden — see tests/CMakeLists.txt — and this split DOES NOT FIX it.
// A DAW is exactly such a process, so the instability is a real open question about the feature and
// not an artefact of the suite.
//
// AND IT IS A SECOND REASON TO BE HERE RATHER THAN IN `test_gui_widgets.cpp`: every case in that file
// that opens an expanded view asks the OVERLAY what happened. It is the other host. Cases about which
// host took the panel do not belong among cases that assume the answer.

#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"

#include <visage_widgets/text_editor.h> // backlog MF (before X11, whose macros collide with its enums)
#include "plugin/editor_prefs.h"
#include "plugin/gui.h"
#include "plugin/gui_bridge.h"
#include "plugin/params.h"
#include "fx/filter/filter_types.h"
#include "plugin/bridge_device_host.h"
#include "plugin/ui/expanded_canvas.h"
#include "plugin/ui/expanded_overlay.h"
#include "plugin/ui/fx_lane.h"
#include "plugin/ui/mod_panel.h"
#include "plugin/ui/notify_tray.h"
#include "plugin/ui/keyboard_claim.h"
#include "plugin/ui/preset_browser.h"
#include "plugin/ui/text_field.h"
#include "plugin/ui/header_bar.h"
#include "plugin/ui/chrome_hue.h"
#include "plugin/ui/chrome_hue_prefs.h"
#include "plugin/ui/settings_screen.h"
#include "plugin/ui/theme.h"
#include "plugin/ui/tooltip.h"
#include "plugin/ui/ui_state.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/knob_tray.h"
#include "plugin/ui/perform_stage.h"
#include "plugin/ui/explore_stage.h"
#include "plugin/ui/stage_rail.h"
#include "plugin/ui/plugin_window.h"
#include "rack/rack_state.h"
#include "plugin/ui/mod_state.h"
// BEFORE Xlib, like everything above: `ZoneCoordBox::NameStyle` has a member Xlib's `None` macro
// would rewrite to `0L`.
#include "fx/shaper/shaper_types.h"
#include "plugin/ui/shaper_expanded.h"
#include "plugin/ui/shaper_pad.h"

#if defined(__linux__)
// THE WINDOW MANAGER'S OWN MESSAGE, so the close case can send what a title bar sends rather than
// call the road it should reach. Last, because Xlib's macros collide with anything that names True,
// False, Status or Bool.
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#endif
#include "plugin/ui/windowed_expanded.h"
#include "plugin/ui/param_edit.h"
#include "plugin/ui/owltt_expanded.h"
#include "menu_placement_check.h"

#include <visage_ui/popup_menu.h>

#include "fx/owltt/owltt_types.h"
#include "fx/harm/harm_types.h"

#include <visage_app/application_window.h>
#include <visage_app/window_event_handler.h> // backlog MF: the real hover road, driven

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>
#include <unistd.h>

using namespace feathers;

namespace {

/// A host that answers the editor and records nothing else — the same stub `test_gui_widgets` uses,
/// for the same reason: these cases care about which frame holds the panel, not about what a host
/// does with a flush.
struct QuietGuiHost : GuiHost {
	int flushes = 0;
	int resizes = 0;
	uint32_t lastResizeWidth = 0;
	uint32_t lastResizeHeight = 0;
	void guiRequestFlush() noexcept override { ++flushes; }
	void guiRequestResize(uint32_t w, uint32_t h) noexcept override {
		++resizes;
		lastResizeWidth = w;
		lastResizeHeight = h;
	}
	void guiCaptureState(clap_ostream_t*) noexcept override {}
	/// A host with no state refuses every load — unless a case says the load is to "succeed" for a
	/// photograph of what the editor does after one (the preset toast).
	bool acceptLoads = false;
	bool guiLoadState(clap_istream_t*) noexcept override { return acceptLoads; }
};

/// THE PREFERENCES FILE IS DELETED BEFORE EVERY CASE AND EVERY SUBCASE.
///
/// `popout.window` is a PERSISTED preference, so a case that sets it writes a file the next editor
/// reads — and doctest re-runs a whole case body once per subcase, each pass building a fresh editor
/// against that same file. Without this the second subcase below would start from whatever the first
/// left behind, which is the identical trap `test_gui_widgets`'s own `CleanDesk` was written for.
struct CleanDesk : doctest::IReporter {
	explicit CleanDesk(const doctest::ContextOptions&) {}

	void test_case_start(const doctest::TestCaseData&) override { clear(); }
	void test_case_reenter(const doctest::TestCaseData&) override { clear(); }

	static void clear() {
		std::error_code code;
		std::filesystem::remove(feathers::EditorPrefs::path(), code);
	}

	void report_query(const doctest::QueryData&) override {}
	void test_run_start() override {}
	void test_run_end(const doctest::TestRunStats&) override {}
	void test_case_end(const doctest::CurrentTestCaseStats&) override {}
	void test_case_exception(const doctest::TestCaseException&) override {}
	void subcase_start(const doctest::SubcaseSignature&) override {}
	void subcase_end() override {}
	void log_assert(const doctest::AssertData&) override {}
	void log_message(const doctest::MessageData&) override {}
	void test_case_skipped(const doctest::TestCaseData&) override {}
};

DOCTEST_REGISTER_REPORTER("clean-desk", 0, CleanDesk);

/// THE ASSEMBLED EDITOR, and the lane taken from the editor rather than found in the tree — the lane
/// is where `onExpandDevice` is installed, and that hook is the road a card's expand mark actually
/// takes. A seam invented for the test would prove the seam.
struct EditorFixture {
	QuietGuiHost host;
	ParamBridge bridge;
	Gui gui { host, bridge };
	visage::Frame* root = nullptr;
	ui::FxLane* lane = nullptr;

	EditorFixture() {
		bridge.refresh();
		REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));
		REQUIRE(gui.setSize(Gui::kLogicalWidth, Gui::kLogicalHeight));
		root = gui.root();
		REQUIRE(root != nullptr);
		REQUIRE(root->width() > 0.0f);
		lane = gui.fxLaneForTest();
		REQUIRE(lane != nullptr);
		REQUIRE(lane->onExpandDevice != nullptr);
	}

	/// A device in the fx chain, with the row published so `onExpandDevice` can see it.
	size_t addFx(const char* tag) {
		const size_t slot = bridge.addFxDevice(tag);
		REQUIRE(slot < kMaxFxSlots);
		bridge.refresh();
		lane->resized();
		return slot;
	}
};

} // namespace

int main(int argc, char** argv) {
#if defined(__linux__)
	// THE DRAW TIMER IS A THREAD, AND Xlib IS NOT THREAD-SAFE UNTIL ASKED. `startPluginDrawTimer`
	// runs a timer thread that posts a ClientMessage to the window while the main thread pumps the
	// same connection; without this, libxcb aborts the process with "Unknown sequence number while
	// processing queue". MEASURED: a case photographing a popout died on SIGABRT twelve assertions
	// in, before this line existed. `windowed-probe` opens with the same call for the same reason.
	XInitThreads();
#endif
	// THIS BINARY NO LONGER FORCES `VISAGE_FULL_REDRAW`, AND THE REASON IS A FIX RATHER THAN A
	// PREFERENCE. It used to: without the intervention a popout drew only what damage asked for, and
	// this binary's first photograph of one was a uniform 24,24,24 — a venue that could not express
	// its own finding. The cause was the expose road, not the damage road. `WindowX11::processEvent`
	// had no `Expose` branch at all, so the very first expose — the one X sends when a window is
	// mapped, covering the whole client area — was received and dropped, and nothing else ever asked
	// the editor to paint its ground. With that branch in place the window draws when X says it must,
	// full redraw or not, and all six cases here pass in the DEFAULT configuration.
	//
	// That matters beyond tidiness: the case below about an exposed rectangle is INVISIBLE under full
	// redraw, because the intervention repaints the damage before anything can photograph it.
	// Forcing the flag here would have made that case incapable of failing.
	// A DESK OF THIS BINARY'S OWN, before any case, for the same reason `test_gui_widgets` takes one:
	// `EditorPrefs` is XDG CONFIG, and without this the developer's real preferences decide what these
	// cases measure — and the cases, which set `popout.window`, would write to them.
	const std::filesystem::path desk =
	    std::filesystem::temp_directory_path()
	    / ("feathers-windowed-config-" + std::to_string(static_cast<long>(::getpid())));
	std::error_code code;
	std::filesystem::create_directories(desk, code);
	::setenv("XDG_CONFIG_HOME", desk.c_str(), 1);
	// AND THE PRESET LIBRARY, which is XDG DATA rather than config: the editor builds a preset browser
	// whether or not a case asks about one, and it must not read the developer's real bank.
	const std::filesystem::path library =
	    std::filesystem::temp_directory_path()
	    / ("feathers-windowed-data-" + std::to_string(static_cast<long>(::getpid())));
	std::filesystem::create_directories(library, code);
	::setenv("XDG_DATA_HOME", library.c_str(), 1);
	// The preset ROOT is ~/Documents/Owlet/Feathers/Presets since backlog FO phase 2, not XDG data.
	::setenv("FEATHERS_PRESET_ROOT", (library / "presets").c_str(), 1);
	// AND THE IMPULSE FOLDER (backlog JD): Featherverb's panel makes ~/Documents/Owlet/Feathers/IRs when its browser
	// first reads it, and a case that opens the panel without its own override made one in a real home (2026-09-24).
	::setenv("FEATHERS_IR_ROOT", (library / "irs").c_str(), 1);

	doctest::Context context;
	context.applyCommandLine(argc, argv);
	context.addFilter("reporters", "console,clean-desk");
	const int result = context.run();

	std::filesystem::remove_all(desk, code);
	std::filesystem::remove_all(library, code);
	return result;
}

TEST_CASE("the popout preference chooses WHICH host opens the panel — one at a time") {
	// The detached window is wired in behind a switch, ON by default.
	//
	// BOTH ROADS END IN A PANEL DRAWING THE SAME CONTENT, so "a panel appeared" cannot tell them
	// apart. The claim is about WHICH HOST took it, and that is a value — so it is asked of each host
	// directly rather than inferred from anything drawn.
	EditorFixture f;
	// A DEVICE THAT HAS AN EXPANDED VIEW, or `onExpandDevice` has nothing to open and both hosts
	// correctly stay shut — which would make every assertion below pass for the wrong reason.
	const size_t slot = f.addFx(fx::harm::kTraits.tag);

	ui::ExpandedOverlay* overlay = f.gui.expandedForTest();
	ui::WindowedExpanded* window = f.gui.expandedWindowForTest();
	REQUIRE(overlay != nullptr);
	// THE WINDOW HOST IS BUILT WHETHER OR NOT IT IS IN USE, so flipping the preference between opens
	// costs no construction.
	REQUIRE(window != nullptr);
	REQUIRE_FALSE(overlay->open());
	REQUIRE_FALSE(window->open());

	ui::ExpandedCanvas* canvas = f.gui.canvasForTest();
	REQUIRE(canvas != nullptr);

	SUBCASE("off, canvas open: the overlay takes it, and the window does not") {
		// A spanning present arrives in the band while it shows (2026-09-13), on the editor's one
		// overlay since 2026-10-03; folded — which is how a fresh editor starts (2026-09-14) — likewise.
		f.gui.setPopoutWindowForTest(false);
		REQUIRE(canvas->setCollapsed(false));
		f.lane->onExpandDevice(slot);
		CHECK(overlay->open());
		CHECK_FALSE(window->open());
	}

	SUBCASE("off, canvas collapsed: the overlay takes it") {
		f.gui.setPopoutWindowForTest(false);
		(void)canvas->setCollapsed(true);
		f.lane->onExpandDevice(slot);
		CHECK(overlay->open());
		CHECK_FALSE(window->open());
	}

	SUBCASE("on: the window takes it with no overlay open first") {
		// SEPARATES TWO CAUSES THAT THE TRANSITION SUBCASE BELOW CONFLATES: is it opening a second
		// ApplicationWindow at all, or is it opening one while the overlay already holds a panel?
		f.gui.setPopoutWindowForTest(true);
		f.lane->onExpandDevice(slot);
		CHECK(window->open());
		CHECK_FALSE(overlay->open());
	}

	SUBCASE("the two are never both open, because the preference can change between opens") {
		// A PANEL LEFT IN THE OTHER HOST IS A STATE NOTHING IN THE EDITOR CAN ADDRESS: no button
		// closes it, and the next open does not touch it. So opening one dismisses the other, and this
		// asserts the invariant rather than the mechanism.
		// A SECOND DEVICE FOR THE SECOND OPEN: the same device's mark pressed again now CLOSES its
		// window, which the subcases below pin.
		const size_t other = f.addFx(fx::owltt::kTraits.tag);
		f.gui.setPopoutWindowForTest(false);
		REQUIRE(canvas->setCollapsed(false));
		f.lane->onExpandDevice(slot);
		REQUIRE(overlay->open());

		f.gui.setPopoutWindowForTest(true);
		f.lane->onExpandDevice(other);
		CHECK(window->open()); // the window took the second one
		CHECK_FALSE(overlay->open()); // and the overlay was cleared by it opening
	}

	SUBCASE("expand toggle, the popout: the same device's mark closes its window, and opens it again") {
		// Pressing a card's window-expand while its window is already open closes it. The window can tell which device it shows (`showsDevice`), so it toggles
		// like the overlay; a different device's press still replaces it, one window at a time.
		const feathers::DeviceInstanceId id = f.bridge.fxDeviceAt(slot);
		const size_t other = f.addFx(fx::owltt::kTraits.tag);
		const feathers::DeviceInstanceId otherId = f.bridge.fxDeviceAt(other);
		f.gui.setPopoutWindowForTest(true);
		f.lane->onExpandDevice(slot);
		REQUIRE(window->open());
		CHECK(window->showsDevice(id, {}));
		f.lane->onExpandDevice(slot);
		CHECK_FALSE(window->open());
		f.lane->onExpandDevice(slot);
		CHECK(window->showsDevice(id, {}));
		f.lane->onExpandDevice(other);
		CHECK(window->showsDevice(otherId, {}));
		CHECK_FALSE(overlay->open());
	}

	SUBCASE("expand toggle, across a preference flip: the device's panel in the overlay is closed, not twinned") {
		f.gui.setPopoutWindowForTest(false);
		f.lane->onExpandDevice(slot);
		REQUIRE(overlay->open());
		f.gui.setPopoutWindowForTest(true);
		f.lane->onExpandDevice(slot);
		CHECK_FALSE(overlay->open());
		CHECK_FALSE(window->open());
	}
}

TEST_CASE("Feathers: with the band open a panel arrives in it, drags up over the synth, and a fold pulls it back onto the editor") {
	// The canvas is an extension of the UI where windows float over and out of the way of the main
	// synth, and while it is open windows still prefer to go there. Feathers' band is at the BOTTOM.
	EditorFixture f;
	const size_t slot = f.addFx(fx::harm::kTraits.tag);
	f.gui.setPopoutWindowForTest(false);
	ui::ExpandedCanvas* canvas = f.gui.canvasForTest();
	ui::ExpandedOverlay* overlay = f.gui.expandedForTest();
	REQUIRE(canvas != nullptr);
	REQUIRE(overlay != nullptr);
	REQUIRE(canvas->setCollapsed(false));
	REQUIRE(f.gui.setSize(Gui::kLogicalWidth, Gui::kLogicalHeight + static_cast<uint32_t>(canvas->extent())));
	// THE BAND'S FLOOR IN THE OVERLAY'S PIXELS, restated here from the two frames rather than asked of
	// the code under test.
	const visage::Point offset = canvas->positionInWindow() - overlay->positionInWindow();
	const visage::Bounds floor = canvas->contentBounds();
	const visage::Bounds region { offset.x + floor.x(), offset.y + floor.y(), floor.width(), floor.height() };
	REQUIRE(region.y() > 0.0f);

	f.lane->onExpandDevice(slot);
	REQUIRE(overlay->open());
	// AT THE FLOOR'S TOP-LEFT — or as near it as a panel taller than the floor can sit and stay whole.
	const visage::Bounds arrived = overlay->panelBounds();
	CHECK(arrived.x() == doctest::Approx(region.x()));
	CHECK(arrived.y() == doctest::Approx(std::min(region.y(), overlay->height() - arrived.height())));
	CHECK(arrived.y() > offset.y - arrived.height() * 0.5f); // mostly in the band, not over the synth

	// UP OVER THE SYNTH: wholly above the band.
	overlay->beginPanelDrag();
	overlay->dragPanelBy({ 0.0f, -(arrived.y() - 10.0f) });
	CHECK(overlay->panelBounds().y() == doctest::Approx(10.0f));
	CHECK(overlay->panelBounds().bottom() < offset.y);

	// DOWN TO THE FOOT, THEN FOLD: the editor loses the band and the panel comes up with it.
	overlay->beginPanelDrag();
	overlay->dragPanelBy({ 0.0f, 9000.0f });
	const float footBefore = overlay->panelBounds().bottom();
	CHECK(footBefore == doctest::Approx(overlay->height()));
	REQUIRE(canvas->setCollapsed(true));
	REQUIRE(f.gui.setSize(Gui::kLogicalWidth, Gui::kLogicalHeight));
	REQUIRE(overlay->height() < footBefore); // the surface really did shrink under it
	CHECK(overlay->open());
	CHECK(overlay->panelBounds().bottom() == doctest::Approx(overlay->height()));
	REQUIRE(overlay->panelForTest() != nullptr);
	CHECK(overlay->panelForTest()->y() == doctest::Approx(overlay->panelBounds().y()));
}

TEST_CASE("a popout cycle returns EVERY watcher list in the editor to where it started") {
	// THE DEFECT THIS BRANCH ARRIVED WITH, ASSERTED AS A VALUE.
	//
	// `WindowedExpanded::dismiss` cleaned the STATE list and nothing else. But `gui.cpp` calls
	// `ExpandedView::setModulation` on the content BEFORE handing it to a host, and
	// `Knob::setModulation` puts every modulatable knob on `ModState`'s OWN `FrameWatchers` — a
	// second list, joined by children the host never sees. So every knob in a windowed panel joined
	// that list and nothing took it off: a freed `Frame*` repainted the next time anything armed.
	//
	// IT IS THE SAME DEFECT TWICE ALREADY FIXED ELSEWHERE — `96d94be` for `FxLane`'s cards, which is
	// the crash hit while assigning an MSEG, and `25282de` for `ExpandedOverlay`. This host was written
	// before both landed.
	//
	// ASSERTED AS A COUNT AND NOT AS A CRASH, which is the stronger form and is the sibling case's
	// lesson taken whole: a dangling read is undefined rather than reliably fatal, and before the
	// overlay's fix the identical sequence LEAKED without segfaulting because nothing had reused the
	// memory yet. A case waiting for a crash would have passed.
	//
	// AND THE ENUMERATION IS THE EDITOR'S, NOT THIS CASE'S. `Gui::watcherCountsForTest` fills one
	// struct from all seven lists; a case that named them itself would go on passing the day an eighth
	// is added and the omission would be invisible from here. Only three of the seven have an exit at
	// all — see the struct's own comment — so this is a RETURN rather than a cleanup, and a future
	// panel that subscribes to the history or the selection reddens it with nobody having to think.
	EditorFixture f;
	f.gui.setPopoutWindowForTest(true);

	// HARM, because its panel is known to put knobs on the modulation list. A panel with no
	// modulatable control could not express this bug at all, and the REQUIREs below say so out loud
	// rather than trusting the choice.
	const size_t slot = f.addFx(fx::harm::kTraits.tag);
	ui::WindowedExpanded* window = f.gui.expandedWindowForTest();
	REQUIRE(window != nullptr);
	REQUIRE_FALSE(window->open());

	// SETTLE FIRST. The lane builds a card per pool slot up front, so a baseline taken before that
	// measures construction rather than leaking.
	f.lane->resized();
	const Gui::WatcherCounts base = f.gui.watcherCountsForTest();

	// THE LISTS ARE REALLY POPULATED, said before anything is opened. A struct of zeros satisfies
	// every equality below, and zeros are exactly what a torn-down editor leaves behind.
	REQUIRE(base.registry > 0);
	REQUIRE(base.state > 0);
	REQUIRE(base.preview > 0);
	REQUIRE(base.modulation > 0);
	REQUIRE(base.selection > 0);
	REQUIRE(base.mode > 0);
	// The history may legitimately be empty on this fixture — ExploreStage is its only subscriber and
	// it is not always built — so it is captured rather than required non-zero, and its return is
	// still asserted below.
	CAPTURE(base.history);

	f.lane->onExpandDevice(slot);
	REQUIRE(window->open()); // the WINDOW host took it, which is the road under test

	// THE PANEL REALLY JOINED BOTH LISTS THIS HOST CLEANS, and the modulation one is named on its own
	// because it is the leg the fix adds. Without this the whole case is arithmetic about numbers that
	// never moved: a host that registered nothing returns to baseline for free.
	const Gui::WatcherCounts mid = f.gui.watcherCountsForTest();
	REQUIRE(mid.state > base.state);
	REQUIRE(mid.modulation > base.modulation);

	window->dismiss();
	REQUIRE_FALSE(window->open());

	// AND EVERY LIST IS BACK, ONE CHECK EACH so a failure names WHICH list leaked rather than only
	// that something did.
	const Gui::WatcherCounts after = f.gui.watcherCountsForTest();
	CHECK(after.registry == base.registry);
	CHECK(after.state == base.state);
	CHECK(after.preview == base.preview);
	CHECK(after.modulation == base.modulation);
	CHECK(after.history == base.history);
	CHECK(after.selection == base.selection);
	CHECK(after.mode == base.mode);

	// AND IT SURVIVES A SECOND CYCLE. The overlay's first fix for this same bug was partial, and the
	// second open is what frees the previous content and reuses the memory.
	f.lane->onExpandDevice(slot);
	REQUIRE(window->open());
	window->dismiss();
	const Gui::WatcherCounts twice = f.gui.watcherCountsForTest();
	CHECK(twice.modulation == base.modulation);
	CHECK(twice.state == base.state);

	// AND NOTIFYING DOES NOT WALK A CORPSE. Survival is not much of an assertion on its own — the
	// counts above are the real ones — but this is the gesture that actually crashes.
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);
	CHECK(mod->watchers().notify() > 0);
}

#if defined(__linux__)
namespace {

/// IS THIS WINDOW ON THE SCREEN, ASKED OF THE X SERVER ON A CONNECTION OF ITS OWN. Not visage's
/// `isShowing()`, which is the window's own account of itself, and not `WindowedExpanded::open()`,
/// which is the panel's: the claim under test is what a hand would see. An error (the window already
/// destroyed) is swallowed and reads as "not on screen", which is what it is.
bool viewableOnServer(::Window handle) {
	Display* display = XOpenDisplay(nullptr);
	if (display == nullptr) {
		return false;
	}
	XErrorHandler previous = XSetErrorHandler([](Display*, XErrorEvent*) { return 0; });
	XWindowAttributes attributes {};
	const Status status = XGetWindowAttributes(display, handle, &attributes);
	XSync(display, False);
	XSetErrorHandler(previous);
	XCloseDisplay(display);
	return status != 0 && attributes.map_state == IsViewable;
}

} // namespace
#endif

TEST_CASE("a popout closed from OUTSIDE survives it, and cleans the same lists") {
	// visage v1.0.0 CHANGED WHAT `ApplicationWindow::close()` MEANS, and this case was rewritten for
	// it rather than loosened. The old fork's `close()` ran `onCloseRequested()` and tore down INSIDE
	// the call; v1.0.0's is `window_->close()`, which on X11 POSTS a `WM_DELETE_WINDOW` ClientMessage
	// to the window itself (upstream's #100 close-veto fix) and on win32 posts `WM_CLOSE`. So a close
	// from outside is now a REQUEST that completes when the pump delivers it — the same road as the
	// window manager's ✕, which the next case drives by hand. On macOS `performClose:` still asks the
	// delegate inside the call. The contract this case pins is therefore the one every platform shares:
	// once the request has been delivered, the panel is dismissed, the dismissal was REPORTED, every
	// list is back, and nothing touched freed storage on the way.
	//
	// WHY RETIRE STILL MATTERS, which the asynchronous road does not make obsolete: the delivery is a
	// call INTO the window (`handleCloseRequested` from its own pump), and our handler is `dismiss()`.
	// Freeing the window there would free the object whose method is on the stack. `closeWindow`
	// retires it instead, and frees it at the next `show()` or in the destructor.
	//
	// AND IT ASSERTS THE LISTS AGAIN RATHER THAN ONLY SURVIVAL, because this is a SECOND ROAD into
	// the same teardown and a road that skipped the cleanup would still return here without crashing.
	EditorFixture f;
	f.gui.setPopoutWindowForTest(true);
	const size_t slot = f.addFx(fx::harm::kTraits.tag);

	ui::WindowedExpanded* window = f.gui.expandedWindowForTest();
	REQUIRE(window != nullptr);
	f.lane->resized();
	const Gui::WatcherCounts base = f.gui.watcherCountsForTest();
	REQUIRE(base.modulation > 0);

	bool told = false;
	window->onDismiss = [&told] { told = true; };

	f.lane->onExpandDevice(slot);
	REQUIRE(window->open());
	const Gui::WatcherCounts mid = f.gui.watcherCountsForTest();
	REQUIRE(mid.modulation > base.modulation);

	visage::ApplicationWindow* app = window->windowForTest();
	REQUIRE(app != nullptr);
	visage::Window* native = app->window();
	REQUIRE(native != nullptr);
#if defined(__linux__)
	const auto handle = reinterpret_cast<::Window>(native->nativeHandle());
	REQUIRE(handle != 0);
#endif
	app->close();

#if defined(__linux__)
	// ON X11 IT IS A REQUEST, AND THIS IS THE LINE THAT SAYS SO. Nothing has been pumped, so nothing
	// has been delivered: if visage ever goes back to closing inside the call, this goes red and the
	// retire-on-delivery reasoning above wants re-reading rather than a quiet green.
	CHECK(window->open());
	CHECK_FALSE(told);
#endif

	// DELIVERED BY THE POPOUT'S OWN PUMP — the venue note in the next case says why `Gui::processEvents`
	// cannot be the driver here. Bounded, and several rounds because the message goes through the X
	// server. On a platform whose close completed inside the call this loop does not run at all.
	for (int pump = 0; pump < 50 && window->open(); ++pump) {
		native->processPluginFdEvents();
	}

	CHECK_FALSE(window->open());
	// THE DISMISSAL WAS REPORTED, not merely performed: the lane has to stop drawing its expand mark
	// as pressed, and only `onDismiss` tells it.
	CHECK(told);

	const Gui::WatcherCounts after = f.gui.watcherCountsForTest();
	CHECK(after.state == base.state);
	CHECK(after.modulation == base.modulation);
	CHECK(after.registry == base.registry);

#if defined(__linux__)
	// AND IT IS OFF THE SCREEN, asked of the server. A panel that was dismissed while its window stayed
	// mapped is the shape v1.0.0's asynchronous `close()` would have left behind, if `closeWindow` had
	// kept calling it (see `dismiss` below for the road where that is the ONLY close).
	CHECK_FALSE(viewableOnServer(handle));
#endif
}

#if defined(__linux__)
TEST_CASE("a popout dismissed from inside leaves the screen at once, not on a later pump") {
	// THE DEFECT v1.0.0's ASYNCHRONOUS `close()` WOULD HAVE SHIPPED, and the reason `closeWindow` no
	// longer calls it. Esc, the overlay's ✕ and expanding another device all reach `dismiss()`, which
	// used to end in `window->close()` and retire the window. Under v1.0.0 that call only POSTS a
	// `WM_DELETE_WINDOW`; the panel is gone from our lists at once, but the X window stays mapped
	// until the pump delivers the message — and the delivery, arriving at a window whose panel is
	// already gone, closes nothing (the plugin pump does not hide on a close request; only the
	// standalone loop does). So the window would sit on screen, empty of anything we own, until the
	// next `show()` freed it.
	//
	// ASKED OF THE SERVER, WITH NO PUMP IN BETWEEN: "at once" is the claim.
	EditorFixture f;
	f.gui.setPopoutWindowForTest(true);
	const size_t slot = f.addFx(fx::harm::kTraits.tag);
	ui::WindowedExpanded* window = f.gui.expandedWindowForTest();
	REQUIRE(window != nullptr);
	f.lane->resized();

	f.lane->onExpandDevice(slot);
	REQUIRE(window->open());
	visage::ApplicationWindow* app = window->windowForTest();
	REQUIRE(app != nullptr);
	REQUIRE(app->window() != nullptr);
	const auto handle = reinterpret_cast<::Window>(app->window()->nativeHandle());
	// NON-VACUITY: the window really is on the screen before, so "not after" is a change.
	REQUIRE(viewableOnServer(handle));

	window->dismiss();
	REQUIRE_FALSE(window->open());
	CHECK_FALSE(viewableOnServer(handle));

	// AND THE NEXT PANEL STILL OPENS ONTO THE SCREEN: hiding the retired window must not have taken
	// anything a second window needs (the shared connection, the ground frame it was lent).
	f.lane->onExpandDevice(slot);
	REQUIRE(window->open());
	visage::ApplicationWindow* second = window->windowForTest();
	REQUIRE(second != nullptr);
	REQUIRE(second->window() != nullptr);
	for (int pump = 0; pump < 20; ++pump) {
		second->window()->processPluginFdEvents();
	}
	CHECK(window->open());
	CHECK(viewableOnServer(reinterpret_cast<::Window>(second->window()->nativeHandle())));
}
#endif

TEST_CASE("a popout opened over an open one cleans the first, and the second still draws") {
	// THE ROAD A HAND ACTUALLY TAKES to close a popout today, given the ✕ finding recorded above:
	// expand a different device. `show()` dismisses the sitting panel before taking the new one, so
	// this is the replacement road through the same teardown — and it is the one that FREES the
	// retired window, which the explicit-dismiss cases never reach.
	//
	// TWO DEVICES, NOT ONE TWICE. Re-expanding the same slot would let a host that never replaced
	// anything pass: the panel it ends holding would be indistinguishable from the one it started
	// with. Two devices means the second panel is a different object over a different row.
	EditorFixture f;
	f.gui.setPopoutWindowForTest(true);
	const size_t first = f.addFx(fx::harm::kTraits.tag);
	const size_t second = f.addFx(fx::owltt::kTraits.tag);
	REQUIRE(first != second);

	ui::WindowedExpanded* window = f.gui.expandedWindowForTest();
	REQUIRE(window != nullptr);
	f.lane->resized();
	const Gui::WatcherCounts base = f.gui.watcherCountsForTest();
	REQUIRE(base.modulation > 0);

	f.lane->onExpandDevice(first);
	REQUIRE(window->open());
	const Gui::WatcherCounts one = f.gui.watcherCountsForTest();
	REQUIRE(one.modulation > base.modulation);

	// STRAIGHT INTO THE SECOND, WITHOUT DISMISSING. This is `show()`'s own `dismiss()`.
	f.lane->onExpandDevice(second);
	REQUIRE(window->open());

	// THE FIRST PANEL'S KNOBS ARE GONE FROM THE LISTS AND THE SECOND PANEL'S ARE ON THEM. Asserted
	// against BASE rather than against `one`: the claim is that exactly one panel is registered, and
	// "the count changed" would be satisfied by two panels' worth minus nothing.
	const Gui::WatcherCounts two = f.gui.watcherCountsForTest();
	CHECK(two.state == one.state);
	CHECK(two.registry == base.registry);
	CHECK(two.preview == base.preview);
	CHECK(two.history == base.history);
	CHECK(two.selection == base.selection);
	CHECK(two.mode == base.mode);
	// The two devices' panels need not carry the same number of modulatable knobs, so the modulation
	// list is required to have come DOWN from the first panel's total and to still be above the
	// editor's baseline — a leak would leave it at or above `one.modulation`.
	CHECK(two.modulation > base.modulation);

	window->dismiss();
	CHECK_FALSE(window->open());
	const Gui::WatcherCounts after = f.gui.watcherCountsForTest();
	CHECK(after.state == base.state);
	CHECK(after.modulation == base.modulation);

	// AND NOTIFYING DOES NOT WALK A CORPSE — the gesture that crashes when a list holds a freed frame.
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);
	CHECK(mod->watchers().notify() > 0);
}

#if defined(__linux__)
TEST_CASE("the window manager's close reaches a popout inside a plugin") {
	// THE THIRD DEFECT THIS FEATURE ARRIVED WITH, AND THE ONE THAT NEEDED A CHANGE IN visage.
	// `show()` registers WM_DELETE_WINDOW on every window it maps, plugin windows included, so a
	// title-bar ✕ genuinely arrives as a ClientMessage. But the plugin pump routed it into
	// `WindowX11::processEvent`, whose only ClientMessage handling is the drag-and-drop atoms —
	// there was a `deleteMessage()` branch in the standalone `runEventLoop` and NOWHERE else. So the
	// message was received and dropped, and a popout's close button did nothing: the only way to
	// dismiss one was whatever the plugin's own UI offered.
	//
	// THIS CASE SENDS THE MESSAGE THE WINDOW MANAGER SENDS, from a connection of its own. Since
	// visage v1.0.0 `ApplicationWindow::close()` posts the same message from the window's OWN
	// connection (the case above), so the two now share a delivery; this one still pins that a
	// message from ANOTHER client — which is what a window manager is — is heard.
	//
	// IT NEEDS A DISPLAY, and it is honest about which half it cannot see: a real ✕ also depends on
	// the window manager decorating the window and on a real pointer, neither of which a headless
	// run has. What this pins is that the message, once it arrives, is heard.
	EditorFixture f;
	f.gui.setPopoutWindowForTest(true);
	const size_t slot = f.addFx(fx::harm::kTraits.tag);

	ui::WindowedExpanded* window = f.gui.expandedWindowForTest();
	REQUIRE(window != nullptr);
	f.lane->resized();
	const Gui::WatcherCounts base = f.gui.watcherCountsForTest();
	REQUIRE(base.modulation > 0);

	bool told = false;
	window->onDismiss = [&told] { told = true; };

	f.lane->onExpandDevice(slot);
	REQUIRE(window->open());
	// NON-VACUITY: the popout really took knobs onto the modulation list, so "the counts returned"
	// below is a claim about a teardown that had something to tear down.
	REQUIRE(f.gui.watcherCountsForTest().modulation > base.modulation);

	visage::ApplicationWindow* app = window->windowForTest();
	REQUIRE(app != nullptr);
	auto handle = reinterpret_cast<::Window>(app->window()->nativeHandle());
	REQUIRE(handle != 0);

	Display* display = XOpenDisplay(nullptr);
	REQUIRE(display != nullptr);
	XClientMessageEvent close {};
	close.type = ClientMessage;
	close.window = handle;
	close.message_type = XInternAtom(display, "WM_PROTOCOLS", False);
	close.format = 32;
	close.data.l[0] = static_cast<long>(XInternAtom(display, "WM_DELETE_WINDOW", False));
	close.data.l[1] = CurrentTime;
	REQUIRE(XSendEvent(display, handle, False, NoEventMask,
	                   reinterpret_cast<XEvent*>(&close)) != 0);
	XFlush(display);
	XCloseDisplay(display);

	// THE POPOUT'S OWN PUMP, AND THAT IS A STATEMENT ABOUT THIS VENUE RATHER THAN A SHORTCUT.
	// `Gui::processEvents` — what a host calls — returns at its first line here, because it needs
	// `impl_->app->window()` and this fixture's editor is created without one. MEASURED: a probe on
	// the line past that guard never fired across fifty calls. So the host's road cannot be driven
	// headlessly, and a case written through it would assert nothing while looking thorough.
	//
	// The display is process-global, so any window's pump drains every window's events — which is
	// why the delete branch sits in the dispatch loop and not in `handlePluginFdEvent`: the window
	// being closed may be the one pumping, and here it is. Several pumps because the message goes
	// through the X server; one is a race, not a measurement.
	for (int pump = 0; pump < 50 && window->open(); ++pump) {
		app->window()->processPluginFdEvents();
	}

	CHECK_FALSE(window->open());
	CHECK(told);

	const Gui::WatcherCounts after = f.gui.watcherCountsForTest();
	CHECK(after.state == base.state);
	CHECK(after.modulation == base.modulation);
	CHECK(after.registry == base.registry);
}
#endif

#if defined(__linux__)
namespace {

/// THE POPOUT'S OWN PIXELS, READ OFF THE X SERVER. `takeScreenshot` cannot be used here: it is
/// implemented for a WINDOWLESS layer only (`Layer::requestScreenshot` sets its flag under
/// `headless_render_` and otherwise hands the shot to bgfx asynchronously), and this window is
/// paired to a real swap chain. `import` is the same camera `guihost` uses, and PPM is asked for
/// rather than PNG so the bytes below are pixels rather than a compressed stream — a region of a
/// PNG is not addressable, and the claim here is about a rectangle.
struct Shot {
	int width = 0;
	int height = 0;
	std::vector<unsigned char> rgb;

	[[nodiscard]] bool valid() const {
		return width > 0 && height > 0 && rgb.size() == static_cast<size_t>(width) * height * 3;
	}
};

Shot capturePpm(::Window handle, const std::string& path) {
	Shot shot;
	char command[512];
	std::snprintf(command, sizeof(command), "import -window 0x%lx -depth 8 ppm:%s 2>/dev/null",
	              static_cast<unsigned long>(handle), path.c_str());
	if (std::system(command) != 0) {
		return shot;
	}
	FILE* file = std::fopen(path.c_str(), "rb");
	if (file == nullptr) {
		return shot;
	}
	char magic[3] = {};
	int maxval = 0;
	const bool header = std::fscanf(file, "%2s %d %d %d", magic, &shot.width, &shot.height, &maxval) == 4;
	if (!header || magic[0] != 'P' || magic[1] != '6' || maxval != 255) {
		std::fclose(file);
		shot.width = 0;
		shot.height = 0;
		return shot;
	}
	std::fgetc(file); // the single whitespace byte that ends the header
	shot.rgb.resize(static_cast<size_t>(shot.width) * shot.height * 3);
	if (std::fread(shot.rgb.data(), 1, shot.rgb.size(), file) != shot.rgb.size()) {
		shot.rgb.clear();
	}
	std::fclose(file);
	return shot;
}

/// A PHOTOGRAPH OF NOTHING, WHICH `import` REPORTS AS A SUCCESS.
///
/// MEASURED BY hunt-engine ON THIS DISPLAY, 2026-08-28: about one `import` capture in twelve comes
/// back ALL BLACK — a valid PNG, exit status 0, greyscale standard deviation exactly 0 against a
/// real capture's 0.068-0.073. Nothing downstream notices, and the guard people reach for makes it
/// WORSE rather than better: the frames-must-differ rule passes a blank frame with flying colours,
/// because a blank frame is emphatically different from its neighbours. **The hash rule catches a
/// window that is ASLEEP and hands back identical frames; it cannot catch one that hands back a
/// DIFFERENT frame which happens to be empty.** Opposite directions, and only one was guarded.
///
/// EXACT UNIFORMITY RATHER THAN A STANDARD-DEVIATION THRESHOLD, and it is the same judgement their
/// 0.001 bar makes: answer "uniform", never "boring". A real capture of this panel varies in the
/// second pixel; there is no threshold to tune and no ImageMagick call to make.
bool isUniform(const Shot& shot) {
	for (size_t at = 3; at < shot.rgb.size(); ++at) {
		if (shot.rgb[at] != shot.rgb[at % 3]) {
			return false;
		}
	}
	return true;
}

/// HOW MANY PIXELS OF ONE RECTANGLE DIFFER. A count rather than a bool, because "the rectangle came
/// back" and "the rectangle came back except for four pixels of antialiasing" are different answers
/// and only a number tells them apart.
int differingPixels(const Shot& a, const Shot& b, int x, int y, int w, int h) {
	if (!a.valid() || !b.valid() || a.width != b.width || a.height != b.height) {
		return -1;
	}
	int differing = 0;
	for (int row = std::max(0, y); row < std::min(a.height, y + h); ++row) {
		for (int col = std::max(0, x); col < std::min(a.width, x + w); ++col) {
			const size_t at = (static_cast<size_t>(row) * a.width + col) * 3;
			if (a.rgb[at] != b.rgb[at] || a.rgb[at + 1] != b.rgb[at + 1] || a.rgb[at + 2] != b.rgb[at + 2]) {
				++differing;
			}
		}
	}
	return differing;
}

/// HOW MANY PIXELS ARE THE OCCLUDER'S OWN COLOUR. Magenta is chosen because nothing in this
/// interface is remotely near it, so a count above zero cannot be a rounding difference or an
/// antialiased edge — it is another window's ink, still on our surface.
int magentaPixels(const Shot& shot) {
	int count = 0;
	for (size_t at = 0; at + 2 < shot.rgb.size(); at += 3) {
		if (shot.rgb[at] > 200 && shot.rgb[at + 1] < 80 && shot.rgb[at + 2] > 200) {
			++count;
		}
	}
	return count;
}

} // namespace

TEST_CASE("a popup menu closing in a popout leaves the panel as it found it") {
	// SEEN IN BITWIG: after the crossover-type dropdown in the OWLTT popout was opened, the panel
	// stayed on screen after it closed, clipped by the graph below it. That reads as pixels nobody
	// repainted — the menu's rectangle keeping the menu, with only the part the graph happens to
	// redraw cleared.
	//
	// THE CLAIM IS A VALUE, NOT A DIFFERENCE, and the difference is the second assertion rather than
	// the first: the menu's rectangle must hold EXACTLY the bytes it held before the menu existed.
	// "something changed after the menu closed" is satisfied by a menu that faded to half opacity.
	// The difference check is kept below it, on the same rectangle, to prove the equality is not
	// vacuous — a window that drew nothing at all would satisfy an equality on its own.
	EditorFixture f;
	f.gui.setPopoutWindowForTest(true);
	const size_t slot = f.addFx(fx::owltt::kTraits.tag);
	f.lane->onExpandDevice(slot);

	ui::WindowedExpanded* window = f.gui.expandedWindowForTest();
	REQUIRE(window != nullptr);
	REQUIRE(window->open());
	visage::ApplicationWindow* app = window->windowForTest();
	REQUIRE(app != nullptr);
	REQUIRE(app->window() != nullptr);
	const auto handle = reinterpret_cast<::Window>(app->window()->nativeHandle());
	REQUIRE(handle != 0);

	// THE POPOUT'S OWN PUMP, for the reason the close case above records: the editor in this fixture
	// has no window, so `Gui::processEvents` returns at its first line and cannot drive anything.
	const auto pump = [&](int milliseconds) {
		for (int slice = 0; slice < std::max(1, milliseconds / 4); ++slice) {
			app->window()->processPluginFdEvents();
			::usleep(4000);
		}
	};

	const std::string dir = std::filesystem::temp_directory_path().string();
	const std::string base = dir + "/feathers-popout-" + std::to_string(static_cast<long>(::getpid()));

	// SETTLE ON A WINDOW THAT HAS ACTUALLY DRAWN ITS PANEL AND THEN STOPPED MOVING, and prove both
	// rather than wait a guessed number of milliseconds. Two things were measured here and each one
	// silently invalidated the comparison below:
	//
	//   - after 1200 ms the window was still a uniform 24,24,24 — the ground visage paints before any
	//     content reaches it — so every assertion was being made against a baseline of nothing;
	//   - and the panel is NOT still when it first appears. OWLTT's band bars decay, so a baseline
	//     taken early differs from a photograph taken three seconds later in the graph and in every
	//     readout under it, for reasons that have nothing to do with a menu.
	//
	// So the loop below asks for a picture that is not blank AND that two consecutive photographs
	// agree on. That is the frames-must-differ rule run in reverse: stillness is the PRECONDITION
	// here, and it has to be established before a later difference can be read as a finding.
	//
	// IT USES THE RAW CAMERA DELIBERATELY, where everything below goes through `ink`: this loop is
	// already closed against a blank frame from both ends — a blank `before` fails `isUniform` and a
	// blank `again` fails the agreement — so a dropped capture costs one more turn of the loop and
	// nothing else.
	//
	// EVERY PHOTOGRAPH IN THIS CASE IS GUARDED AGAINST A BLANK, not only the baseline — the two
	// polling loops below both refuse a uniform frame and go round again, which is the same guard the
	// settle already applies here. See `isUniform`: roughly one `import` in twelve returns a valid,
	// all-black PNG with exit status 0, and the exposure runs toward a FALSE RED, in a case that is
	// in `ctest`. A retry is the whole fix; a blank capture is a dropped frame, not a state the
	// window is in.
	Shot before;
	Shot again;
	bool settled = false;
	for (int attempt = 0; attempt < 40 && !settled; ++attempt) {
		pump(500);
		before = capturePpm(handle, base + "-before.ppm");
		pump(500);
		again = capturePpm(handle, base + "-again.ppm");
		settled = before.valid() && again.valid() && !isUniform(before) && before.rgb == again.rgb;
	}
	REQUIRE(before.valid());
	REQUIRE_FALSE(isUniform(before));
	REQUIRE(settled);

	// THE MENU THE PANEL ITSELF OPENS, built here rather than reached by clicking the combo box:
	// `owltt_expanded.cpp` belongs to another session today, and the claim under test is visage's
	// teardown of a `PopupMenuFrame`, which every road into a menu converges on — `optionSelected`,
	// `mouseUpOutside` and `focusChanged` all end at `PopupMenuFrame::exit()`.
	// THE SOURCE IS THE PANEL, AND THE PARENT IS WHATEVER visage SAYS IT IS. `PopupMenuFrame::show`
	// parents itself on `source->topParentFrame()`, which for a window is the editor's own top-level
	// frame rather than the `ApplicationWindow` object — so the frame to watch is asked for rather
	// than assumed, or the count below would be taken on a frame the menu never joined.
	REQUIRE_FALSE(app->children().empty());
	visage::Frame* panel = app->children().front();
	visage::Frame* root = panel->topParentFrame();
	REQUIRE(root != nullptr);
	const size_t childrenBefore = root->children().size();
	visage::PopupMenu menu;
	for (int i = 0; i < 10; ++i) {
		menu.addOption(i, fx::owltt::kModeNames[i]);
	}
	menu.show(panel, { 40, 40 });
	REQUIRE(root->children().size() == childrenBefore + 1);
	visage::Frame* popup = root->children().back();
	REQUIRE(popup != nullptr);

	// WAIT FOR THE MENU TO REACH THE GLASS, POLLED — because the number of milliseconds it takes is
	// not a property of this plugin and cannot be guessed.
	//
	// MEASURED, with the quad census beside a wall clock: `menu.show()` returned at ...069269 and the
	// popup's own batch — `region (70,70)`, 5 rounded-rectangle quads and 65 text quads — was
	// SUBMITTED at ...069300. **Thirty-one milliseconds.** The editor is not slow. The camera is: the
	// first capture that contained the menu was at ...070902, **1,633 ms after the submit**.
	//
	// THAT LAG IS THE COMPOSITOR, and it is the same fact that shaped the expose case below. `:1` is
	// composited, GNOME Shell redirects TOP-LEVEL windows to offscreen pixmaps, and `import -window`
	// on a popout reads that pixmap rather than the glass — so what this camera shows is refreshed on
	// the compositor's schedule, not on ours. It is worse for a window that presents ONCE and then
	// goes quiet, which is exactly what a static panel does without full redraw.
	//
	// SO A FIXED `pump()` WAS ALWAYS A GUESS, and it decayed. 1,200 ms was enough when this case was
	// written and stopped being enough under a busier machine, which turned a premise into a
	// deterministic mainline failure — `REQUIRE(drewMenu > 1000)` reading `0 > 1000` — while the
	// product was doing its job in 31 ms. The bound below is generous and the REQUIRE still fires if
	// the menu genuinely never draws, so a real regression is still caught; what is gone is the
	// assumption that a constant can stand in for a condition.
	Shot withMenu;
	int drewMenu = 0;
	for (int attempt = 0; attempt < 60; ++attempt) {
		pump(100);
		withMenu = capturePpm(handle, base + "-menu.ppm");
		if (withMenu.valid() && !isUniform(withMenu)) {
			drewMenu = differingPixels(before, withMenu, 0, 0, before.width, before.height);
			if (drewMenu > 1000) {
				break;
			}
		}
	}
	REQUIRE(withMenu.valid());
	// NON-VACUITY, AND IT IS ALSO THE VENUE CHECK: the camera and the window can express a change.
	REQUIRE(drewMenu > 1000);

	// EVERY DISMISSAL'S TERMINUS. `exit()` sets `done_` and starts the one-shot timer whose callback
	// removes the frame from its parent; the timer is serviced by `EventManager::checkEventTimers`,
	// which the window's draw callback calls, so the pump below is what actually closes it.
	static_cast<visage::PopupMenuFrame*>(popup)->exit();
	pump(2000);
	CHECK(root->children().size() == childrenBefore);

	// AND SETTLE AGAIN BEFORE PHOTOGRAPHING THE RESULT, for the same reason and in the same shape as
	// the baseline. With a camera that can lag the editor by a second and a half, a single shot taken
	// a fixed time after the close could be a frame from BEFORE the menu was removed — which would
	// report the ghost this case exists to detect, on a build that does not have it. Two consecutive
	// photographs that agree is the cheapest proof the camera has caught up; whether they agree on
	// the RIGHT picture is the claim below, and that is left entirely to the assertion.
	Shot after;
	Shot afterAgain;
	bool afterSettled = false;
	for (int attempt = 0; attempt < 40 && !afterSettled; ++attempt) {
		after = capturePpm(handle, base + "-after.ppm");
		pump(300);
		afterAgain = capturePpm(handle, base + "-after2.ppm");
		afterSettled = after.valid() && afterAgain.valid() && !isUniform(after) &&
		               after.rgb == afterAgain.rgb;
	}
	REQUIRE(after.valid());
	REQUIRE(afterSettled);
	const int leftBehind = differingPixels(before, after, 0, 0, before.width, before.height);
	CHECK(leftBehind == 0);

	// KEPT WHEN THE CASE FAILS, because a count says how many pixels are wrong and only the picture
	// says which. `FEATHERS_POPOUT_KEEP` names a directory; without it the shots are deleted.
	if (std::getenv("FEATHERS_POPOUT_KEEP") == nullptr) {
		std::error_code code;
		for (const char* suffix : { "-before.ppm", "-again.ppm", "-menu.ppm", "-after.ppm",
		                            "-after2.ppm" }) {
			std::filesystem::remove(base + suffix, code);
		}
	}
	else {
		MESSAGE("shots kept at " << base << "-*.ppm");
	}
}

TEST_CASE("a rectangle of a popout trashed by another window is repainted") {
	// THE DAY AFTER THE DROPDOWN REPORT: stray pixels appeared even when no popup had been opened.
	// So the torn band across the editor needs no second window and no menu.
	// This is the road that was never handled at all.
	//
	// `kEventMask` has always contained `ExposureMask` and `XSelectInput` has always been given it,
	// so X has always been sending Expose events to a plugin's editor. The ONLY handler was inside
	// `WindowX11::runEventLoop` — the standalone loop, which a plugin never runs — and
	// `processEvent`'s switch had eleven cases, none of them `Expose`. The event was received and
	// discarded. Put that beside `createWindow`'s `background_pixmap = None`, which tells X to paint
	// nothing into an exposed region, and the arithmetic is grim: nothing clears the area and nothing
	// asks for it to be repainted, so it keeps the other window's pixels for the life of the editor.
	//
	// A CHILD WINDOW IS THE INSTRUMENT, and the choice is not arbitrary. This display is composited
	// (GNOME Shell owns `_NET_WM_CM_S0`), and a compositor redirects TOP-LEVEL windows to offscreen
	// pixmaps — so covering the editor with another top-level generates no expose and is not even
	// visible to `import`. MEASURED: in the top-level and sibling arms the occluder registered ZERO
	// magenta pixels while it was up, which makes any verdict from them worthless. A child window is
	// not separately redirected, so destroying one is an expose X is obliged to send, compositor or
	// not — and its occluder photographs at exactly its own area.
	//
	// THE CASE CANNOT FAIL UNDER `VISAGE_FULL_REDRAW`, which is why this binary stopped forcing it.
	// That intervention invalidates every layer every frame, so the next present overwrites the
	// damage before anything can photograph it: the same experiment comes back 0 px in every arm with
	// the flag on and 37440 px with it off. The flag is a stopgap for a different fault and it must
	// not be what is keeping this one off the screen.
	EditorFixture f;
	f.gui.setPopoutWindowForTest(true);
	const size_t slot = f.addFx(fx::owltt::kTraits.tag);
	f.lane->onExpandDevice(slot);

	ui::WindowedExpanded* window = f.gui.expandedWindowForTest();
	REQUIRE(window != nullptr);
	REQUIRE(window->open());
	visage::ApplicationWindow* app = window->windowForTest();
	REQUIRE(app != nullptr);
	REQUIRE(app->window() != nullptr);
	const auto handle = reinterpret_cast<::Window>(app->window()->nativeHandle());
	REQUIRE(handle != 0);

	const auto pump = [&](int milliseconds) {
		for (int slice = 0; slice < std::max(1, milliseconds / 4); ++slice) {
			app->window()->processPluginFdEvents();
			::usleep(4000);
		}
	};

	const std::string dir = std::filesystem::temp_directory_path().string();
	const std::string base = dir + "/feathers-expose-" + std::to_string(static_cast<long>(::getpid()));
	const auto ink = [&](const std::string& path) {
		Shot shot;
		for (int attempt = 0; attempt < 8; ++attempt) {
			shot = capturePpm(handle, path);
			if (shot.valid() && !isUniform(shot)) {
				return shot;
			}
			pump(120);
		}
		return shot;
	};

	// The same settle the case above uses, and for the same two measured reasons.
	Shot before;
	Shot again;
	bool settled = false;
	for (int attempt = 0; attempt < 40 && !settled; ++attempt) {
		pump(500);
		before = capturePpm(handle, base + "-before.ppm");
		pump(500);
		again = capturePpm(handle, base + "-again.ppm");
		settled = before.valid() && again.valid() && !isUniform(before) && before.rgb == again.rgb;
	}
	REQUIRE(before.valid());
	REQUIRE(settled);
	// NOTHING IN THIS PANEL IS MAGENTA, which is what makes the count below mean "somebody else's ink"
	// rather than "some ink".
	REQUIRE(magentaPixels(before) == 0);

	Display* display = XOpenDisplay(nullptr);
	REQUIRE(display != nullptr);
	const int bandWidth = static_cast<int>(before.width * 0.78);
	const int bandHeight = 40;
	XSetWindowAttributes attributes {};
	attributes.background_pixel = 0xFF00FF;
	attributes.border_pixel = 0xFF00FF;
	// OVERRIDE-REDIRECT so no window manager moves, decorates or reparents it — this has to land
	// exactly where it is put, over a band at the top of the panel, which is where the reported artefact is.
	attributes.override_redirect = True;
	const ::Window occluder = XCreateWindow(display, handle, 0, 10, static_cast<unsigned>(bandWidth),
	                                        static_cast<unsigned>(bandHeight), 0, CopyFromParent,
	                                        InputOutput, CopyFromParent,
	                                        CWBackPixel | CWBorderPixel | CWOverrideRedirect,
	                                        &attributes);
	REQUIRE(occluder != 0);
	XMapRaised(display, occluder);
	XFlush(display);
	XSync(display, False);
	pump(800);

	// THE VENUE CHECK AND THE NON-VACUITY CHECK ARE THE SAME SHOT. If the occluder is not visible to
	// this camera then the editor's pixels were never trashed, and a clean verdict below would be the
	// camera's silence rather than the editor's repair. That is exactly what the top-level arm of the
	// hand experiment did: zero magenta while it was up.
	const Shot covered = capturePpm(handle, base + "-covered.ppm");
	REQUIRE(covered.valid());
	const int coveredMagenta = magentaPixels(covered);
	REQUIRE(coveredMagenta > bandWidth * bandHeight / 2);

	XDestroyWindow(display, occluder);
	XFlush(display);
	XSync(display, False);
	XCloseDisplay(display);
	pump(2000); // ~120 frames at 60 Hz, ample for one damaged rectangle

	const Shot after = ink(base + "-after.ppm");
	REQUIRE(after.valid());
	// THE VALUE: not one pixel of the other window's ink survives, and the band holds exactly the
	// bytes it held before. The magenta count is the sharper of the two — it names WHOSE pixels are
	// wrong, where a difference count would also fire on a panel that had merely moved.
	CHECK(magentaPixels(after) == 0);
	CHECK(differingPixels(before, after, 0, 0, before.width, before.height) == 0);

	if (std::getenv("FEATHERS_POPOUT_KEEP") == nullptr) {
		std::error_code code;
		for (const char* suffix : { "-before.ppm", "-again.ppm", "-covered.ppm", "-after.ppm" }) {
			std::filesystem::remove(base + suffix, code);
		}
	}
	else {
		MESSAGE("shots kept at " << base << "-*.ppm");
	}
}
#endif

#if defined(__linux__)
namespace {

visage::MouseEvent pressAt(float x, float y) {
	visage::MouseEvent event;
	event.relative_position = {3.0f, -2.0f};
	event.position = {x, y};
	event.button_id = visage::kMouseButtonLeft;
	event.button_state = visage::kMouseButtonLeft;
	return event;
}

/// A rectangle of a popout, in the PHOTOGRAPH's pixels, from a frame rectangle in logical ones.
struct PixelRect {
	int x = 0, y = 0, w = 0, h = 0;
};

PixelRect pixelsOf(const visage::Bounds& logical, float scale) {
	return {static_cast<int>(logical.x() * scale), static_cast<int>(logical.y() * scale),
	        static_cast<int>(logical.width() * scale), static_cast<int>(logical.height() * scale)};
}

} // namespace

// THE SAME QUESTION ASKED OF THE SHIPPED PLUGIN WITH A REAL POINTER — the venue recipe, kept here
// because the script lived in a session scratchpad and this case is the only durable place for it.
// It ran 2026-09-02 under `scripts/venue-xvfb.sh run -- <script> <outdir>` and gave: popout plot
// 1945 px moved on the pad write, 1106 px on the knob drag; the editor's CARD plot 1310 px and
// 580 px for the same two gestures. A real-pointer TEST_CASE was tried and removed: it passed
// alone and, in one doctest process, went to 0 px after the expose case above — the venue's X
// history, not the shaper. This is what that case could not be:
//
//   FEATHERS_GUIHOST_HOLD=120 build/guihost build/feathers.clap "$OUT" --device SHPR --shot seeded
//       > "$OUT/guihost.log" 2>&1 &          (continues the line above)
//   until grep -q "holding open for a driver" "$OUT/guihost.log"; do sleep 0.2; done
//   EDITOR=$(grep -o "window is 0x[0-9a-f]*" "$OUT/guihost.log" | head -1 | awk '{print $3}')
//   xdotool mousemove --window "$EDITOR" 1178 458 click 1      # the Shaper card's expand button
//   sleep 1.5; POP=$(xdotool search --name "^Shaper$" | tail -1)
//   xdotool windowmove "$POP" 0 0     # CLEAR OF THE CARD: an obscured rectangle photographs BLACK
//                                     # on Xvfb, and a first run read that as "the card did not update"
//   sleep 1.5; import -window "$POP" -depth 8 ppm:"$OUT/pop-before.ppm"
//   import -window "$EDITOR" -depth 8 ppm:"$OUT/ed-before.ppm"
//   xdotool mousemove --window "$POP" 35 329; xdotool mousedown 1          # pad: (14,44) 420x300
//   for s in 1 2 3 4 5 6 7 8 9 10; do
//     xdotool mousemove --window "$POP" $((35 + 189*s/10)) $((329 - 135*s/10)); sleep 0.04; done
//   xdotool mouseup 1; sleep 1
//   import -window "$POP" -depth 8 ppm:"$OUT/pop-after-pad.ppm"
//   import -window "$EDITOR" -depth 8 ppm:"$OUT/ed-after-pad.ppm"
//   xdotool mousemove --window "$POP" 67 395; xdotool mousedown 1          # Knee knob, cell 0
//   for s in 1 2 3 4 5 6 7 8; do xdotool mousemove --window "$POP" 67 $((395 - 8*s)); sleep 0.04; done
//   xdotool mouseup 1; sleep 1
//   import -window "$POP" -depth 8 ppm:"$OUT/pop-after-knob.ppm"
//   import -window "$EDITOR" -depth 8 ppm:"$OUT/ed-after-knob.ppm"
//
//   regions (scale 1): popout plot 300x190+454+44, popout pad 420x300+14+44,
//                      editor card plot 220x120+1003+468 — compare with
//   compare -metric AE <(convert A -crop R +repage png:-) <(convert B -crop R +repage png:-) null:
//   and md5sum the .ppm files: identical before/again is the settle, differing after is the change.
// INTERMITTENT ON UNCHANGED CODE — MEASURED by surfaces 2026-09-02: the same tree passed a full ctest,
// failed the next full ctest (padChanged == 0, plotChanged == 0, plotChangedByKnob == 0, all three at
// once), passed by name, failed under --each (1 of 8), and passed a third full ctest after an unrelated
// edit. Three assertions going to zero together is the signature of frames coming back IDENTICAL —
// the windowed-capture hazard CLAUDE.md names: a run where the window did not repaint reports "nothing
// changed" and is indistinguishable from a real regression. The headless case beside it in
// test_gui_widgets ("a Shaper change from any of four sources reaches BOTH plots — counted and read
// back") is the PROOF; this one is the PICTURE, and a picture that is sometimes blank must not turn the
// suite red. `may_fail` keeps it running and reporting without failing ctest; a real regression is
// still caught by the counted case. Remove the decorator when the venue's repaint is made reliable.
TEST_CASE("the Shaper popout's transfer curve follows the pad and the knobs — photographed"
          * doctest::may_fail()) {
	// THE BUG: the table display did not always update with puck/knob changes in the
	// window view. The headless case in gui-widgets proves the panel is TOLD; this is the picture:
	// a real popout, pumped the way `Gui::processEvents` pumps it (the tick, then the window's own
	// connection), and the plot's rectangle photographed around a pad write and a knob drag.
	EditorFixture f;
	f.gui.setPopoutWindowForTest(true);
	const size_t slot = f.addFx(fx::shaper::kTraits.tag);
	f.lane->onExpandDevice(slot);

	ui::WindowedExpanded* window = f.gui.expandedWindowForTest();
	REQUIRE(window != nullptr);
	REQUIRE(window->open());
	visage::ApplicationWindow* app = window->windowForTest();
	REQUIRE(app != nullptr);
	REQUIRE(app->window() != nullptr);
	const auto handle = reinterpret_cast<::Window>(app->window()->nativeHandle());
	REQUIRE(handle != 0);

	ui::ShaperExpanded* panel = nullptr;
	for (visage::Frame* child : app->children()) {
		if (auto* found = dynamic_cast<ui::ShaperExpanded*>(child)) {
			panel = found;
		}
	}
	REQUIRE(panel != nullptr);
	auto* pad = const_cast<ui::ShaperPad*>(panel->padForTest());
	REQUIRE(pad != nullptr);

	// THE HOST'S ORDER: `processEvents` ticks the editor FIRST and pumps the popout's connection
	// AFTER, so a write made inside the pump is noticed by the tick one visit later.
	const auto pump = [&](int milliseconds) {
		for (int slice = 0; slice < std::max(1, milliseconds / 4); ++slice) {
			f.gui.tickForTest();
			app->window()->processPluginFdEvents();
			::usleep(4000);
		}
	};

	const std::string dir = std::filesystem::temp_directory_path().string();
	const std::string base = dir + "/feathers-shaper-plot-" + std::to_string(static_cast<long>(::getpid()));

	Shot before;
	Shot again;
	bool settled = false;
	for (int attempt = 0; attempt < 40 && !settled; ++attempt) {
		pump(500);
		before = capturePpm(handle, base + "-before.ppm");
		pump(500);
		again = capturePpm(handle, base + "-again.ppm");
		settled = before.valid() && again.valid() && !isUniform(before) && before.rgb == again.rgb;
	}
	REQUIRE(before.valid());
	REQUIRE(settled);

	const float scale = app->window()->dpiScale();
	const PixelRect plot = pixelsOf(panel->plotBounds(), scale);
	const PixelRect padRect = pixelsOf(panel->padBounds(), scale);
	MESSAGE("popout " << before.width << "x" << before.height << " scale " << scale << " plot "
	                  << plot.x << "," << plot.y << " " << plot.w << "x" << plot.h);

	// A PAD WRITE, from the bottom-left corner to the middle — colour and gamma both move, so the
	// curve `deriveCurve` gives cannot stay where it was.
	pad->onMouseDown().callback(pressAt(pad->width() * 0.05f, pad->height() * 0.95f));
	pad->onMouseDrag().callback(pressAt(pad->width() * 0.5f, pad->height() * 0.5f));
	pad->onMouseUp().callback(pressAt(pad->width() * 0.5f, pad->height() * 0.5f));
	pump(600);
	const Shot afterPad = capturePpm(handle, base + "-after-pad.ppm");
	REQUIRE(afterPad.valid());
	REQUIRE_FALSE(isUniform(afterPad));

	// THE PUCK MOVED — the venue can express a change — AND THE PLOT MOVED WITH IT.
	const int padChanged = differingPixels(before, afterPad, padRect.x, padRect.y, padRect.w, padRect.h);
	const int plotChanged = differingPixels(before, afterPad, plot.x, plot.y, plot.w, plot.h);
	MESSAGE("pad write: pad px changed " << padChanged << ", plot px changed " << plotChanged);
	CHECK(padChanged > 0);
	CHECK(plotChanged > 0);

	// A KNOB DRAG in the same window: Knee, which reaches the curve through `deriveCurve` directly.
	ui::Knob* knee = nullptr;
	for (const auto& knob : panel->knobsForTest()) {
		if (panel->paramOfKnobForTest(knob.get()) == fx::shaper::Param::Knee) {
			knee = knob.get();
		}
	}
	REQUIRE(knee != nullptr);
	knee->onMouseDown().callback(pressAt(knee->width() * 0.5f, knee->height() * 0.5f));
	knee->onMouseDrag().callback(pressAt(knee->width() * 0.5f, knee->height() * 0.5f - 60.0f));
	knee->onMouseUp().callback(pressAt(knee->width() * 0.5f, knee->height() * 0.5f - 60.0f));
	pump(600);
	const Shot afterKnob = capturePpm(handle, base + "-after-knob.ppm");
	REQUIRE(afterKnob.valid());
	REQUIRE_FALSE(isUniform(afterKnob));
	const int plotChangedByKnob = differingPixels(afterPad, afterKnob, plot.x, plot.y, plot.w, plot.h);
	MESSAGE("knob drag: plot px changed " << plotChangedByKnob);
	CHECK(plotChangedByKnob > 0);

	if (std::getenv("FEATHERS_POPOUT_KEEP") == nullptr) {
		std::error_code code;
		for (const char* suffix : { "-before.ppm", "-again.ppm", "-after-pad.ppm", "-after-knob.ppm" }) {
			std::filesystem::remove(base + suffix, code);
		}
	}
	else {
		MESSAGE("shots kept at " << base << "-*.ppm");
	}
	window->dismiss();
}

#endif

// ---------------------------------------------------------------------------------------------
// THE CANVAS IN FEATHERS. A column outboard of the lane,
// added to the editor's logical width rather than taken from the stage, folding like a rail.
// ---------------------------------------------------------------------------------------------

TEST_CASE("the canvas is a band under the tray; folding it keeps the stage's size and changes the size the host is told") {
	// ON A SCREEN THAT HOLDS IT (backlog LX, 2026-10-10: the floor is held to the usable screen, and the private
	// display's is 1728x972, shorter than the editor with its canvas open).
	struct ScreenStub {
		ScreenStub() { ui::PluginWindow::setUsableScreenForTest(8192, 8192); }
		~ScreenStub() { ui::PluginWindow::setUsableScreenForTest(0, 0); }
	} screen;
	EditorFixture f;
	ui::ExpandedCanvas* canvas = f.gui.canvasForTest();
	REQUIRE(canvas != nullptr);
	// FOLDED BY DEFAULT (a minimised canvas is the original behaviour), opened here for the geometry.
	REQUIRE(canvas->collapsed());
	REQUIRE(canvas->setCollapsed(false));
	// AND THE HOST GRANTING THE OPEN SIZE, so every measurement below is at a size the host gave.
	REQUIRE(f.gui.setSize(Gui::kLogicalWidth,
	                      Gui::kLogicalHeight + static_cast<uint32_t>(ui::ExpandedCanvas::kExtent)));
	// ALONG THE BOTTOM, THE FULL WIDTH, WITH THE TRAY DIRECTLY ABOVE IT — in Feathers it goes at the bottom,
	// below Attack/Release and the rest.
	CHECK(canvas->y() + canvas->height() == doctest::Approx(f.root->height()));
	CHECK(canvas->height() == doctest::Approx(ui::ExpandedCanvas::kExtent));
	CHECK(canvas->x() == doctest::Approx(0.0f));
	CHECK(canvas->width() == doctest::Approx(f.root->width()));
	CHECK(f.lane->y() + f.lane->height() == doctest::Approx(canvas->y() - ui::KnobTray::kHeight));

	const visage::Frame* stage = nullptr;
	for (visage::Frame* child : f.root->children()) {
		if (dynamic_cast<ui::PerformStage*>(child) != nullptr) {
			stage = child;
		}
	}
	REQUIRE(stage != nullptr);
	const float stageW = stage->width();

	// THE FOLD: a strip's width, and the size the host is now told is the design canvas plus the strip.
	// THE ASK ITSELF CANNOT BE COUNTED HERE: `requestPreferredSize` speaks only through a native
	// window, and this fixture's editor has none (the case below simulates the host granting it).
	const float stageH = stage->height();
	REQUIRE(canvas->toggleCollapsed());
	CHECK(canvas->height() == doctest::Approx(34.0f));
	uint32_t w = 1, h = 1;
	REQUIRE(f.gui.adjustSize(&w, &h));
	CHECK(w == Gui::kLogicalWidth);
	CHECK(h == Gui::kLogicalHeight + 34u);
	// WITHOUT THE HOST'S ANSWER the window is as tall as it was and the layout spreads into the room —
	// PluginWindow's fluid rule, and the honest picture in a host that ignores the ask.
	CHECK(canvas->y() + canvas->height() == doctest::Approx(f.root->height()));
	// WITH IT, the stage is exactly the size it was — the canvas was room the editor asked for, so
	// giving it back takes nothing from the disk.
	REQUIRE(f.gui.setSize(Gui::kLogicalWidth, Gui::kLogicalHeight + 34u));
	CHECK(stage->width() == doctest::Approx(stageW));
	CHECK(stage->height() == doctest::Approx(stageH));
	CHECK(f.lane->y() + f.lane->height() == doctest::Approx(canvas->y() - ui::KnobTray::kHeight));

	// AND BACK, the same facts the other way.
	REQUIRE(canvas->toggleCollapsed());
	CHECK(canvas->height() == doctest::Approx(ui::ExpandedCanvas::kExtent));
	w = 1;
	h = 1;
	REQUIRE(f.gui.adjustSize(&w, &h));
	CHECK(h == Gui::kLogicalHeight + static_cast<uint32_t>(ui::ExpandedCanvas::kExtent));
	REQUIRE(f.gui.setSize(w, h));
	CHECK(stage->width() == doctest::Approx(stageW));
	CHECK(stage->height() == doctest::Approx(stageH));
	CHECK(canvas->y() + canvas->height() == doctest::Approx(f.root->height()));
}

// ---------------------------------------------------------------------------------------------
// BACKLOG EJ — THE GLOBAL UI SCALE: a global scaling setting, apart from window size, which only
// enlarges the window while the widgets stay the same size. The preference lives in
// `EditorPrefs` (this binary's main points XDG_CONFIG_HOME at a scratch desk, so these cases write a
// real file and clean it up); the shell reads it before the window exists.
// ---------------------------------------------------------------------------------------------

namespace {

/// Write the UI scale preference the way the editor does, then forget the object — the fixture's own
/// `EditorPrefs` reads the file, which is the road a second instance takes.
void writeUiScalePref(double scale) {
	feathers::EditorPrefs prefs;
	prefs.load();
	prefs.setNumber(ui::PluginWindow::kUiScaleKey, scale);
}

void clearUiScalePref() {
	std::error_code code;
	std::filesystem::remove(feathers::EditorPrefs::path(), code);
}

} // namespace

TEST_CASE("backlog EJ: the UI scale ladder steps 100 % to 200 % and back, and snaps an off-ladder value to the "
          "next rung") {
	using ui::PluginWindow;
	CHECK(PluginWindow::stepUiScale(1.0, +1) == doctest::Approx(1.125));
	CHECK(PluginWindow::stepUiScale(1.125, +1) == doctest::Approx(1.25));
	CHECK(PluginWindow::stepUiScale(1.75, +1) == doctest::Approx(2.0));
	CHECK(PluginWindow::stepUiScale(2.0, +1) == doctest::Approx(2.0)); // the top holds
	CHECK(PluginWindow::stepUiScale(1.25, -1) == doctest::Approx(1.125));
	CHECK(PluginWindow::stepUiScale(1.0, -1) == doctest::Approx(1.0)); // the floor holds
	CHECK(PluginWindow::stepUiScale(1.3, +1) == doctest::Approx(1.375)); // off the ladder: next rung up
	CHECK(PluginWindow::stepUiScale(1.3, -1) == doctest::Approx(1.25));  // and down
	CHECK(PluginWindow::stepUiScale(1.75, 0) == doctest::Approx(1.0));   // reset
	// The keys: = and + up, - down, 0 reset, anything else is not a scale key.
	CHECK(PluginWindow::uiScaleStepFor(static_cast<int>(visage::KeyCode::Equals)) == 1);
	CHECK(PluginWindow::uiScaleStepFor(static_cast<int>(visage::KeyCode::KPPlus)) == 1);
	CHECK(PluginWindow::uiScaleStepFor(static_cast<int>(visage::KeyCode::Minus)) == -1);
	CHECK(PluginWindow::uiScaleStepFor(static_cast<int>(visage::KeyCode::Number0)) == 0);
	CHECK(PluginWindow::uiScaleStepFor(static_cast<int>(visage::KeyCode::A)) == 2);
}

TEST_CASE("backlog EJ: the UI scale is a global preference the editor comes up at — a multiplier on the display's "
          "scale, with the size floor and the request following it") {
	// ON A SCREEN THAT HOLDS IT (backlog LX, 2026-10-10: the floor is held to the usable screen, and the private
	// display's is 1728x972, shorter than the editor with its canvas open).
	struct ScreenStub {
		ScreenStub() { ui::PluginWindow::setUsableScreenForTest(8192, 8192); }
		~ScreenStub() { ui::PluginWindow::setUsableScreenForTest(0, 0); }
	} screen;
	clearUiScalePref();
	double base = 0.0;
	{
		EditorFixture plain;
		base = plain.gui.contentScaleForTest();
		REQUIRE(base > 0.0);
		CHECK(plain.gui.uiScaleForTest() == doctest::Approx(1.0));
	}

	writeUiScalePref(1.5);
	EditorFixture f;
	CHECK(f.gui.uiScaleForTest() == doctest::Approx(1.5));
	CHECK(f.gui.contentScaleForTest() == doctest::Approx(1.5 * base));

	// THE FLOOR A HOST IS TOLD IS THE DESIGN AT THIS SCALE, in its own pixels — rounded up.
	uint32_t w = 1;
	uint32_t h = 1;
	REQUIRE(f.gui.adjustSize(&w, &h));
	CHECK(w == static_cast<uint32_t>(std::ceil(Gui::kLogicalWidth * 1.5 * base)));
	// The height is the design plus the canvas as it stands (folded, a fresh editor: its strip).
	REQUIRE(f.gui.canvasForTest() != nullptr);
	const double logicalH = Gui::kLogicalHeight + static_cast<double>(f.gui.canvasForTest()->height());
	CHECK(h == static_cast<uint32_t>(std::ceil(logicalH * 1.5 * base)));

	// AND A LAYOUT TALLER THAN THE WINDOW DOES NOT SHRINK THE WIDGETS. Opening the canvas grows the
	// logical height by its extent while the window stays the size the fixture gave it; before EJ the
	// content scale followed that ratio down so the whole canvas fitted (a 700-pixel window drew a
	// 1180-row layout at 0.59). Now the scale is the setting, and what does not fit is clipped until
	// the host grants the room. This is the road the old fit took — `preferredSizeChanged` re-derives
	// the scale — so it is the road that proves the fit is gone.
	REQUIRE(f.gui.canvasForTest()->setCollapsed(false));
	CHECK(f.gui.contentScaleForTest() == doctest::Approx(1.5 * base));
	clearUiScalePref();
}

TEST_CASE("backlog EJ: Ctrl/Cmd with = steps the UI scale up, - down and 0 back, says the percentage, and writes "
          "the preference other editors read") {
	clearUiScalePref();
	EditorFixture f;
	REQUIRE(f.gui.uiScaleForTest() == doctest::Approx(1.0));
	const double base = f.gui.contentScaleForTest();

	// A BARE KEY IS NOT THE GESTURE.
	CHECK_FALSE(f.root->onKeyPress().callback(visage::KeyEvent { visage::KeyCode::Equals, 0, true }));
	CHECK(f.gui.uiScaleForTest() == doctest::Approx(1.0));

	CHECK(f.root->onKeyPress().callback(visage::KeyEvent { visage::KeyCode::Equals, visage::kModifierRegCtrl, true }));
	CHECK(f.gui.uiScaleForTest() == doctest::Approx(1.125));
	CHECK(f.gui.contentScaleForTest() == doctest::Approx(1.125 * base));
	CHECK(f.gui.notifyTextForTest().find("UI scale 113%") != std::string::npos);

	// PERSISTED, read back through a fresh object the way another instance would.
	{
		feathers::EditorPrefs again;
		again.load();
		CHECK(again.number(ui::PluginWindow::kUiScaleKey, 0.0) == doctest::Approx(1.125));
	}

	CHECK(f.root->onKeyPress().callback(visage::KeyEvent { visage::KeyCode::Equals, visage::kModifierRegCtrl, true }));
	CHECK(f.gui.uiScaleForTest() == doctest::Approx(1.25));
	CHECK(f.root->onKeyPress().callback(visage::KeyEvent { visage::KeyCode::Minus, visage::kModifierRegCtrl, true }));
	CHECK(f.gui.uiScaleForTest() == doctest::Approx(1.125));
	CHECK(f.root->onKeyPress().callback(visage::KeyEvent { visage::KeyCode::Number0, visage::kModifierRegCtrl, true }));
	CHECK(f.gui.uiScaleForTest() == doctest::Approx(1.0));
	CHECK(f.gui.contentScaleForTest() == doctest::Approx(base));
	clearUiScalePref();
}

TEST_CASE("backlog EJ: the header's plus and minus take the same road as the keys — the scale moves, the "
          "readout says so, and the preference is written") {
	clearUiScalePref();
	EditorFixture f;
	REQUIRE(f.gui.uiScaleForTest() == doctest::Approx(1.0));
	const double base = f.gui.contentScaleForTest();
	ui::HeaderBar* header = f.gui.headerForTest();
	REQUIRE(header != nullptr);
	// THE EDITOR WIRED THE ROAD — a header without it draws no readout at all.
	REQUIRE(static_cast<bool>(header->uiScaleNow));
	CHECK(header->uiScaleNow() == doctest::Approx(1.0));

	// THE BUTTONS ARE READ FRESH BEFORE EVERY CLICK, because a scale step moves them: this fixture's
	// host never answers the resize request, so the window keeps its native size and the header's
	// LOGICAL width shrinks by the step (1240 → 1102 at 112.5 %) — the whole right-hand cluster slides
	// left. A user clicks where the button is drawn, which is this. (A DAW that grants the request
	// leaves the logical geometry alone instead; either way the fresh read is right.)
	const auto click = [header](visage::Bounds (ui::HeaderBar::*button)() const) {
		const visage::Bounds box = (header->*button)();
		visage::MouseEvent e;
		e.position = { box.x() + box.width() * 0.5f, box.y() + box.height() * 0.5f };
		header->onMouseDown().callback(e);
	};
	const visage::Bounds plusBefore = header->uiScalePlusBounds();

	click(&ui::HeaderBar::uiScalePlusBounds);
	CHECK(f.gui.uiScaleForTest() == doctest::Approx(1.125));
	CHECK(header->uiScaleNow() == doctest::Approx(1.125));
	CHECK(f.gui.contentScaleForTest() == doctest::Approx(1.125 * base));
	// The move itself, pinned so the comment above stays true: the button is elsewhere after the step.
	CHECK(header->uiScalePlusBounds().x() < plusBefore.x());
	CHECK(f.gui.notifyTextForTest().find("UI scale 113%") != std::string::npos);
	{
		feathers::EditorPrefs again;
		again.load();
		CHECK(again.number(ui::PluginWindow::kUiScaleKey, 0.0) == doctest::Approx(1.125));
	}
	click(&ui::HeaderBar::uiScalePlusBounds);
	CHECK(f.gui.uiScaleForTest() == doctest::Approx(1.25));
	click(&ui::HeaderBar::uiScaleMinusBounds);
	click(&ui::HeaderBar::uiScaleMinusBounds);
	CHECK(f.gui.uiScaleForTest() == doctest::Approx(1.0));
	// AND THE FLOOR: minus at 100 % stays at 100 %, the ladder's bottom rung.
	click(&ui::HeaderBar::uiScaleMinusBounds);
	CHECK(f.gui.uiScaleForTest() == doctest::Approx(1.0));
	CHECK(f.gui.contentScaleForTest() == doctest::Approx(base));
	clearUiScalePref();
}

// ---------------------------------------------------------------------------------------------
// BACKLOG LG — THE SETTINGS SCREEN AND THE CHROME HUE. The screen opens from the header's Libra mark (or
// its wordmark), every setting on it is a desk preference in `EditorPrefs` — this binary's main points
// XDG_CONFIG_HOME at a scratch desk — and the hue is read back after the editor that set it is gone.
// ---------------------------------------------------------------------------------------------

namespace {

void clearPrefsFile() {
	std::error_code code;
	std::filesystem::remove(feathers::EditorPrefs::path(), code);
}

/// A press at the middle of `box`, in the screen's own coordinates.
void pressSettings(ui::SettingsScreen& screen, visage::Bounds box, int clicks = 1) {
	visage::MouseEvent e;
	e.position = { box.x() + box.width() * 0.5f, box.y() + box.height() * 0.5f };
	e.repeat_click_count = clicks;
	screen.mouseDown(e);
	screen.mouseUp(e);
}

} // namespace

TEST_CASE("backlog LG: the Libra mark opens the settings screen, whose hue strip turns the chrome live and writes the "
          "desk, and the next editor comes up at that hue") {
	struct Reset {
		~Reset() {
			(void)ui::setChromeHueOffset(0.0);
			(void)ui::setAccentHueOffset(0.0);
			(void)ui::setTooltipDetail(ui::TooltipDetail::Full);
			clearPrefsFile();
		}
	} reset;
	clearPrefsFile();
	REQUIRE(ui::chromeHueOffset() == 0.0);
	{
		EditorFixture f;
		ui::HeaderBar* header = f.gui.headerForTest();
		ui::SettingsScreen* screen = f.gui.settingsForTest();
		REQUIRE(header != nullptr);
		REQUIRE(screen != nullptr);
		REQUIRE(static_cast<bool>(header->onSettings));
		CHECK_FALSE(screen->isVisible());

		// THE DOOR: the mark where it is drawn, the wordmark otherwise.
		const visage::Bounds logo = header->logoBounds();
		const visage::Bounds door = logo.width() > 0.0f ? logo : header->wordmarkBounds();
		visage::MouseEvent press;
		press.position = { door.x() + door.width() * 0.5f, door.y() + door.height() * 0.5f };
		REQUIRE(header->hitsSettings(press.position));
		header->onMouseDown().callback(press);
		CHECK(screen->isVisible());
		// OVER EVERYTHING: the top frame's last child, and the whole editor.
		REQUIRE(f.root->children().back() == screen);
		CHECK(screen->width() == doctest::Approx(f.root->width()));
		CHECK(screen->height() == doctest::Approx(f.root->height()));
		// AND THE WORDMARK IS A DOOR TOO, at every width (the mark hides when the header is squeezed).
		const visage::Bounds word = header->wordmarkBounds();
		CHECK(header->hitsSettings({ word.x() + 4.0f, word.y() + word.height() * 0.5f }));

		// THE HUE STRIP, pressed at +120°.
		pressSettings(*screen, { screen->hueX(120.0) - 0.5f, screen->hueStripBounds().y(), 1.0f,
		                          screen->hueStripBounds().height() });
		CHECK(ui::chromeHueOffset() == doctest::Approx(120.0).epsilon(0.01));
		const double chosen = ui::chromeHueOffset();
		CHECK(ui::kAccent == ui::rotateHue(feathers::tokens::kAccent, chosen));
		CHECK(ui::kLine == ui::rotateHue(feathers::tokens::kLine, chosen));
		{
			feathers::EditorPrefs again;
			again.load();
			CHECK(again.number(ui::kChromeHuePrefKey, 0.0) == chosen);
		}
		// A DOUBLE CLICK ON THE STRIP GOES HOME; RESET DOES TOO.
		pressSettings(*screen, screen->hueStripBounds(), 2);
		CHECK(ui::chromeHueOffset() == 0.0);
		pressSettings(*screen, { screen->hueX(-90.0) - 0.5f, screen->hueStripBounds().y(), 1.0f,
		                          screen->hueStripBounds().height() });
		CHECK(ui::chromeHueOffset() == doctest::Approx(-90.0).epsilon(0.02));
		pressSettings(*screen, screen->hueResetBounds());
		CHECK(ui::chromeHueOffset() == 0.0);
		pressSettings(*screen, { screen->hueX(120.0) - 0.5f, screen->hueStripBounds().y(), 1.0f,
		                          screen->hueStripBounds().height() });
		REQUIRE(ui::chromeHueOffset() == doctest::Approx(chosen));

		// THE ACCENT HUE STRIP, pressed at +45°: the roles turn by chrome + accent, the greys by chrome alone,
		// and the editor's own watch writes the desk.
		pressSettings(*screen, { screen->accentX(45.0) - 0.5f, screen->accentStripBounds().y(), 1.0f,
		                          screen->accentStripBounds().height() });
		CHECK(ui::accentHueOffset() == doctest::Approx(45.0).epsilon(0.03));
		const double accent = ui::accentHueOffset();
		CHECK(ui::chromeHueOffset() == chosen);
		CHECK(ui::kAccent == ui::rotateHue(feathers::tokens::kAccent, chosen + accent));
		CHECK(ui::kLine == ui::rotateHue(feathers::tokens::kLine, chosen));
		{
			feathers::EditorPrefs again;
			again.load();
			CHECK(again.number(ui::kAccentHuePrefKey, 0.0) == accent);
		}

		// THE OTHER TWO SETTINGS: the tooltip detail, and a UI scale rung — every rung on the face.
		pressSettings(*screen, screen->tooltipSegmentBounds(0));
		CHECK(ui::tooltipDetail() == ui::TooltipDetail::Brief);
		pressSettings(*screen, screen->tooltipSegmentBounds(1));
		CHECK(ui::tooltipDetail() == ui::TooltipDetail::Full);
		REQUIRE(ui::SettingsScreen::scaleLadder().size() == ui::PluginWindow::kUiScaleLadder.size());
		pressSettings(*screen, screen->scaleSegmentBounds(2));
		CHECK(f.gui.uiScaleForTest() == doctest::Approx(1.25));
		{
			feathers::EditorPrefs again;
			again.load();
			CHECK(again.number(ui::PluginWindow::kUiScaleKey, 0.0) == doctest::Approx(1.25));
		}
		pressSettings(*screen, screen->scaleSegmentBounds(0));
		CHECK(f.gui.uiScaleForTest() == doctest::Approx(1.0));

		// THE ABOUT PART says the version and the stamp the event log writes.
		CHECK(ui::SettingsScreen::versionLine().find("Feathers ") == 0);
		CHECK(ui::SettingsScreen::buildLine().size() > std::string("build ").size());

		// CLOSED BY THE ×, and by a press on the scrim outside the panel.
		pressSettings(*screen, screen->closeBounds());
		CHECK_FALSE(screen->isVisible());
		header->onMouseDown().callback(press);
		REQUIRE(screen->isVisible());
		pressSettings(*screen, { 1.0f, 1.0f, 2.0f, 2.0f });
		CHECK_FALSE(screen->isVisible());
	}
	// THE EDITOR IS GONE AND SO IS ITS WATCH: the process goes back to 0 with the desk still saying the hue.
	(void)ui::setChromeHueOffset(0.0);
	(void)ui::setAccentHueOffset(0.0);
	{
		feathers::EditorPrefs again;
		again.load();
		REQUIRE(again.number(ui::kChromeHuePrefKey, 0.0) != 0.0);
		REQUIRE(again.number(ui::kAccentHuePrefKey, 0.0) != 0.0);
	}
	// THE NEXT EDITOR COMES UP AT THE DESK'S HUE.
	EditorFixture next;
	CHECK(ui::chromeHueOffset() != 0.0);
	CHECK(ui::accentHueOffset() != 0.0);
	CHECK(ui::kAccent == ui::rotateHue(feathers::tokens::kAccent, ui::chromeHueOffset() + ui::accentHueOffset()));
	CHECK(ui::kAccent != feathers::tokens::kAccent);
}

TEST_CASE("the canvas's partition and fold travel in the PATCH, and a load moves the frame on the beat") {
	// The partition size and expanded state persist with the patch — not the desk,
	// which is where the first draft kept the fold. A fold or a moved partition is an edit; a patch that
	// arrives under an open editor is applied by the tick, the road every other patch fact takes.
	EditorFixture f;
	ui::ExpandedCanvas* canvas = f.gui.canvasForTest();
	REQUIRE(canvas != nullptr);
	// A FRESH PATCH HAS NO VIEW, and the frame is folded by default.
	CHECK(f.bridge.canvasView() == rack::CanvasView {});
	REQUIRE(canvas->collapsed());

	// THE HAND WRITES THE PATCH: an unfold, a fold, and a partition drag committed.
	REQUIRE(canvas->toggleCollapsed());
	CHECK(f.bridge.canvasView() == rack::CanvasView { ui::ExpandedCanvas::kExtent, false });
	REQUIRE(canvas->toggleCollapsed());
	CHECK(f.bridge.canvasView().collapsed);
	REQUIRE(canvas->toggleCollapsed());
	REQUIRE(canvas->setOpenExtent(320.0f));
	canvas->onExtentCommit();
	CHECK(f.bridge.canvasView().extent == doctest::Approx(320.0f));
	CHECK_FALSE(f.bridge.canvasView().collapsed);
	CHECK(canvas->height() == doctest::Approx(320.0f));

	// THE PATCH MOVES THE HAND: a view set on the bridge (what a load does) reaches the frame on the
	// next beat, partition and fold both.
	f.bridge.setCanvasView({ 240.0f, true });
	(void)f.gui.tickForTest();
	CHECK(canvas->collapsed());
	CHECK(canvas->openExtent() == doctest::Approx(240.0f));
	CHECK(canvas->height() == doctest::Approx(34.0f));
	f.bridge.setCanvasView({ 240.0f, false });
	(void)f.gui.tickForTest();
	CHECK_FALSE(canvas->collapsed());
	CHECK(canvas->height() == doctest::Approx(240.0f));
	// AND A PATCH THAT NEVER SET AN EXTENT LEAVES THE FRAME'S — zero is "unset", not "nothing".
	f.bridge.setCanvasView({ 0.0f, false });
	(void)f.gui.tickForTest();
	CHECK(canvas->openExtent() == doctest::Approx(240.0f));
}

#if defined(__linux__)
TEST_CASE("opening the canvas fits it inside a window that already has room, and only grows a window that has not — "
          "closing it never shrinks the window") {
	// Reported 2026-09-17: "opening/closing the canvas reproportions the window. we should proportion/resize
	// so it fits inside the original window provided its large enough." Every open and close used to ask the
	// host for exactly the editor's preferred size, growing or shrinking the window each time.
	//
	// IN A REAL X PARENT, because a resize request speaks only through a native window: the fixture's editor
	// alone has none and asks nothing (see the band case above).
	EditorFixture f;
	Display* display = XOpenDisplay(nullptr);
	REQUIRE(display != nullptr);
	const int screen = DefaultScreen(display);
	uint32_t w = 0, h = 0;
	REQUIRE(f.gui.getSize(&w, &h));
	::Window parent = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0, w, h, 0,
	                                      BlackPixel(display, screen), BlackPixel(display, screen));
	XFlush(display);
	clap_window window {};
	window.api = CLAP_WINDOW_API_X11;
	window.x11 = parent;
	REQUIRE(f.gui.setParent(&window));
	ui::ExpandedCanvas* canvas = f.gui.canvasForTest();
	REQUIRE(canvas != nullptr);
	REQUIRE(canvas->collapsed());
	const double scale = f.gui.contentScaleForTest();
	REQUIRE(scale > 0.0);
	const auto hostRows = [scale](double logical) { return static_cast<uint32_t>(std::lround(logical * scale)); };
	const uint32_t width = hostRows(Gui::kLogicalWidth);

	SUBCASE("a window with room: the band takes the room, the host is asked for nothing either way") {
		constexpr double kRoom = 300.0; // under the saved 480, over the 120 minimum
		REQUIRE(f.gui.setSize(width, hostRows(Gui::kLogicalHeight + kRoom)));
		const int before = f.host.resizes;
		REQUIRE(canvas->setCollapsed(false));
		CHECK(f.host.resizes == before);
		// THE BAND IS THE ROOM, and the editor is exactly as tall as its window.
		CHECK(canvas->openExtent() == doctest::Approx(kRoom));
		CHECK(canvas->y() + canvas->height() == doctest::Approx(f.root->height()));
		uint32_t gw = 0, gh = 0;
		REQUIRE(f.gui.getSize(&gw, &gh));
		CHECK(gh == hostRows(Gui::kLogicalHeight + kRoom));
		// CLOSING KEEPS THE WINDOW: no ask, and the instrument takes the room back.
		REQUIRE(canvas->setCollapsed(true));
		CHECK(f.host.resizes == before);
		REQUIRE(f.gui.getSize(&gw, &gh));
		CHECK(gh == hostRows(Gui::kLogicalHeight + kRoom));
	}

	SUBCASE("a window with more room than the saved band: the band keeps its size, nothing is asked") {
		REQUIRE(f.gui.setSize(width, hostRows(Gui::kLogicalHeight + 900.0)));
		const int before = f.host.resizes;
		REQUIRE(canvas->setCollapsed(false));
		CHECK(f.host.resizes == before);
		CHECK(canvas->openExtent() == doctest::Approx(ui::ExpandedCanvas::kExtent));
	}

	SUBCASE("a window with no room: the host is asked once, for exactly the height the band needs, width kept") {
		// A BAND THAT FITS ON THIS SCREEN, because a request is also held to the usable screen area (on the
		// private display 1080 less its panels) — the saved 480 would be clipped to that, which is the
		// screen rule and not the one under test.
		constexpr float kBand = 200.0f;
		REQUIRE(canvas->setOpenExtent(kBand));
		REQUIRE(f.gui.setSize(width, hostRows(Gui::kLogicalHeight)));
		const int before = f.host.resizes;
		REQUIRE(canvas->setCollapsed(false));
		CHECK(f.host.resizes == before + 1);
		CHECK(canvas->openExtent() == doctest::Approx(kBand)); // not shrunk to the room there was
		CHECK(f.host.lastResizeHeight == hostRows(Gui::kLogicalHeight + kBand));
		CHECK(f.host.lastResizeWidth == width);
	}

	SUBCASE("room under the band's minimum is not room: the window grows rather than the band vanishing") {
		constexpr float kBand = 200.0f;
		REQUIRE(canvas->setOpenExtent(kBand));
		REQUIRE(f.gui.setSize(width, hostRows(Gui::kLogicalHeight + 60.0)));
		const int before = f.host.resizes;
		REQUIRE(canvas->setCollapsed(false));
		CHECK(f.host.resizes == before + 1);
		CHECK(canvas->openExtent() == doctest::Approx(kBand));
		CHECK(f.host.lastResizeHeight == hostRows(Gui::kLogicalHeight + kBand));
	}
	XDestroyWindow(display, parent);
	XCloseDisplay(display);
}
#endif // defined(__linux__)

// THE TWO REPRO PHOTOGRAPHS READ THE X SERVER (XOpenDisplay, XGetImage), SO THEY ARE LINUX-ONLY like the
// file's other X11 blocks. They were added outside a guard and stopped the macOS CI build (run 35169434704:
// "unknown type name 'Display'").
#if defined(__linux__)
TEST_CASE("REPRO: a bypassed card's veil, photographed before and after the canvas resizes the editor") {
	// THE BUG: the inactive veils were wrong after a resize. Photographed rather
	// than reasoned about: the editor in a real X window, a filter bypassed, the canvas folded and the
	// window resized as a host would, and the shots kept under the scratchpad for a pair of eyes.
	const char* keep = std::getenv("FEATHERS_VEIL_REPRO");
	if (keep == nullptr) {
		return;
	}
	EditorFixture f;
	Display* display = XOpenDisplay(nullptr);
	REQUIRE(display != nullptr);
	const int screen = DefaultScreen(display);
	uint32_t w = 0, h = 0;
	REQUIRE(f.gui.getSize(&w, &h));
	::Window parent = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0, w, h, 0,
	                                      BlackPixel(display, screen), BlackPixel(display, screen));
	XFlush(display);
	clap_window window {};
	window.api = CLAP_WINDOW_API_X11;
	window.x11 = parent;
	REQUIRE(f.gui.setParent(&window));
	XMapWindow(display, parent);
	XRaiseWindow(display, parent);
	XFlush(display);
	const auto pump = [&](int milliseconds) {
		for (int slice = 0; slice < std::max(1, milliseconds / 4); ++slice) {
			f.gui.processEvents();
			::usleep(4000);
		}
	};

	const size_t slot = f.addFx(fx::filter::kTraits.tag);
	BridgeDeviceHost host { f.bridge };
	const DeviceInstanceId id = f.bridge.fxDeviceAt(slot);
	host.setField(id, static_cast<size_t>(fx::filter::kTraits.enableControl), 0.0f);
	f.bridge.refresh();
	pump(1200);
	const std::string base = std::string(keep) + "/veil";
	const Shot before = capturePpm(parent, base + "-1-open.ppm");
	REQUIRE(before.valid());
	MESSAGE("open: " << before.width << "x" << before.height);

	// FOLD, AND THE HOST GRANTS THE FOLDED SIZE.
	REQUIRE(f.gui.canvasForTest()->toggleCollapsed());
	uint32_t w2 = 1, h2 = 1;
	REQUIRE(f.gui.adjustSize(&w2, &h2));
	XResizeWindow(display, parent, w2, h2);
	XFlush(display);
	REQUIRE(f.gui.setSize(w2, h2));
	pump(1200);
	const Shot folded = capturePpm(parent, base + "-2-folded.ppm");
	REQUIRE(folded.valid());
	MESSAGE("folded: " << folded.width << "x" << folded.height);

	// AND BACK.
	REQUIRE(f.gui.canvasForTest()->toggleCollapsed());
	uint32_t w3 = 1, h3 = 1;
	REQUIRE(f.gui.adjustSize(&w3, &h3));
	XResizeWindow(display, parent, w3, h3);
	XFlush(display);
	REQUIRE(f.gui.setSize(w3, h3));
	pump(1200);
	const Shot reopened = capturePpm(parent, base + "-3-reopened.ppm");
	REQUIRE(reopened.valid());
	MESSAGE("reopened: " << reopened.width << "x" << reopened.height);
	XCloseDisplay(display);
}

TEST_CASE("PHOTO: the canvas as more of the surface — a panel arrives in the band, floats over the synth, and a fold pulls it on") {
	// Env-gated like the veil repro: FEATHERS_CANVAS_SURFACE_REPRO
	// names a directory, and the three frames land there. Run under FEATHERS_REDUCED_MOTION=1 so the
	// sky stands still and two frames differ only by what the case did.
	const char* keep = std::getenv("FEATHERS_CANVAS_SURFACE_REPRO");
	if (keep == nullptr) {
		return;
	}
	EditorFixture f;
	f.gui.setPopoutWindowForTest(false);
	Display* display = XOpenDisplay(nullptr);
	REQUIRE(display != nullptr);
	const int screen = DefaultScreen(display);
	uint32_t w = 0, h = 0;
	REQUIRE(f.gui.getSize(&w, &h));
	::Window parent = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0, w, h, 0,
	                                      BlackPixel(display, screen), BlackPixel(display, screen));
	XFlush(display);
	clap_window window {};
	window.api = CLAP_WINDOW_API_X11;
	window.x11 = parent;
	REQUIRE(f.gui.setParent(&window));
	XMapWindow(display, parent);
	XRaiseWindow(display, parent);
	XFlush(display);
	const auto pump = [&](int milliseconds) {
		for (int slice = 0; slice < std::max(1, milliseconds / 4); ++slice) {
			f.gui.processEvents();
			::usleep(4000);
		}
	};
	const auto grant = [&] {
		uint32_t gw = 1, gh = 1;
		REQUIRE(f.gui.adjustSize(&gw, &gh));
		XResizeWindow(display, parent, gw, gh);
		XFlush(display);
		REQUIRE(f.gui.setSize(gw, gh));
	};

	const size_t slot = f.addFx(fx::harm::kTraits.tag);
	ui::ExpandedCanvas* canvas = f.gui.canvasForTest();
	ui::ExpandedOverlay* overlay = f.gui.expandedForTest();
	REQUIRE(canvas != nullptr);
	REQUIRE(overlay != nullptr);
	REQUIRE(canvas->setCollapsed(false));
	grant();
	f.lane->onExpandDevice(slot);
	REQUIRE(overlay->open());
	pump(1200);
	const std::string base = std::string(keep) + "/canvas-surface";
	const Shot arrived = capturePpm(parent, base + "-1-arrived-in-band.ppm");
	REQUIRE(arrived.valid());
	MESSAGE("arrived at " << overlay->panelBounds().x() << "," << overlay->panelBounds().y());

	// OVER THE SYNTH: dragged up out of the band, to the top of the editor.
	overlay->beginPanelDrag();
	overlay->dragPanelBy({ 0.0f, -9000.0f });
	pump(1200);
	const Shot over = capturePpm(parent, base + "-2-over-synth.ppm");
	REQUIRE(over.valid());
	MESSAGE("over the synth at " << overlay->panelBounds().x() << "," << overlay->panelBounds().y());

	// BACK TO THE FOOT OF THE BAND, THEN FOLD: the panel comes up with the editor's new foot.
	overlay->beginPanelDrag();
	overlay->dragPanelBy({ 0.0f, 9000.0f });
	REQUIRE(canvas->toggleCollapsed());
	grant();
	pump(1200);
	const Shot folded = capturePpm(parent, base + "-3-folded-pulled-on.ppm");
	REQUIRE(folded.valid());
	CHECK(overlay->open());
	MESSAGE("folded: panel at " << overlay->panelBounds().y() << " of " << overlay->height());
	XCloseDisplay(display);
}

TEST_CASE("PHOTO: the window header's one enable chip, the selected card in its window's colour, and a folded card's expand chip") {
	// Env-gated like the photographs above: FEATHERS_WINDOW_ENABLE_REPRO names a
	// directory. Run under FEATHERS_REDUCED_MOTION=1 so frames differ only by what the case did.
	const char* keep = std::getenv("FEATHERS_WINDOW_ENABLE_REPRO");
	if (keep == nullptr) {
		return;
	}
	EditorFixture f;
	f.gui.setPopoutWindowForTest(false);
	Display* display = XOpenDisplay(nullptr);
	REQUIRE(display != nullptr);
	const int screen = DefaultScreen(display);
	uint32_t w = 0, h = 0;
	REQUIRE(f.gui.getSize(&w, &h));
	::Window parent = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0, w, h, 0,
	                                      BlackPixel(display, screen), BlackPixel(display, screen));
	XFlush(display);
	clap_window window {};
	window.api = CLAP_WINDOW_API_X11;
	window.x11 = parent;
	REQUIRE(f.gui.setParent(&window));
	XMapWindow(display, parent);
	XRaiseWindow(display, parent);
	XFlush(display);
	const auto pump = [&](int milliseconds) {
		for (int slice = 0; slice < std::max(1, milliseconds / 4); ++slice) {
			f.gui.processEvents();
			::usleep(4000);
		}
	};

	const size_t owltt = f.addFx(fx::owltt::kTraits.tag);
	const size_t harm = f.addFx(fx::harm::kTraits.tag);
	ui::ExpandedOverlay* overlay = f.gui.expandedForTest();
	REQUIRE(overlay != nullptr);
	f.lane->onExpandDevice(owltt);
	f.lane->onExpandDevice(harm);
	REQUIRE(overlay->panelCount() == 2);
	ui::FxCard* card = f.lane->cardAt(owltt);
	REQUIRE(card != nullptr);
	// BOTH WINDOWS MOVED OFF THE FX LANE, as a hand would, so the cards they belong to are in the frame:
	// HARM (in front) to the middle, then OWLTT to the left edge.
	overlay->beginPanelDrag();
	overlay->dragPanelBy({ -380.0f, 0.0f });
	REQUIRE(card->onHeadPressed != nullptr);
	card->onHeadPressed(); // OWLTT to the front, to be dragged
	overlay->beginPanelDrag();
	overlay->dragPanelBy({ -9000.0f, 0.0f });
	f.lane->cardAt(harm)->onHeadPressed(); // and HARM back in front, the state the next frame changes
	REQUIRE(overlay->contentForTest()->watchedDevice() == f.bridge.fxDeviceAt(harm));
	pump(1200);
	const std::string base = std::string(keep) + "/window-enable";
	const Shot opened = capturePpm(parent, base + "-1-two-windows.ppm");
	REQUIRE(opened.valid());

	// THE OWLTT CARD SELECTED, AND ITS HEAD PRESSED: its window comes forward wearing the card's colour.
	REQUIRE(card->onCardPressed != nullptr);
	card->onCardPressed();
	card->onHeadPressed();
	REQUIRE(overlay->contentForTest()->watchedDevice() == f.bridge.fxDeviceAt(owltt));
	CHECK(card->frameStyle().outlineColor == overlay->panelForTest()->edgeColor());
	pump(1200);
	const Shot selected = capturePpm(parent, base + "-2-selected-owltt-front.ppm");
	REQUIRE(selected.valid());

	// THE HEADER CHIP PRESSED: OWLTT off, on the window's chip and the card's square alike.
	ui::ExpandedPanel* panel = overlay->panelForTest();
	const visage::Bounds sw = panel->enableBounds();
	visage::MouseEvent e;
	e.position = { sw.x() + sw.width() * 0.5f, sw.y() + sw.height() * 0.5f };
	e.repeat_click_count = 1;
	panel->onMouseDown().callback(e);
	panel->onMouseUp().callback(e);
	CHECK_FALSE(card->headEnabled());
	pump(1200);
	const Shot off = capturePpm(parent, base + "-3-chip-off.ppm");
	REQUIRE(off.valid());

	// THE CARD FOLDED: its expand chip is still in the head.
	card->setFolded(true);
	f.lane->resized();
	CHECK(card->expandBounds().width() > 0.0f);
	pump(1200);
	const Shot folded = capturePpm(parent, base + "-4-folded-card-expand.ppm");
	REQUIRE(folded.valid());
	MESSAGE("card at " << card->positionInWindow().x << "," << card->positionInWindow().y << " size "
	                   << card->width() << "x" << card->height() << "; expand "
	                   << card->expandBounds().x() << "," << card->expandBounds().y());

	// THE MODULATOR STRIP: the windows closed, the first modulator selected — its outline in its own
	// instance's window colour, not the old accent.
	overlay->dismiss();
	ui::ModPanel* strip = nullptr;
	const auto find = [&](visage::Frame* frame, auto& self) -> void {
		if (strip == nullptr) {
			strip = dynamic_cast<ui::ModPanel*>(frame);
		}
		for (visage::Frame* child : frame->children()) {
			self(child, self);
		}
	};
	find(f.root, find);
	REQUIRE(strip != nullptr);
	auto* modCard = dynamic_cast<ui::FxCard*>(const_cast<visage::Frame*>(strip->cardFrameForTest(0)));
	REQUIRE(modCard != nullptr);
	modCard->onCardPressed();
	CHECK(modCard->frameStyle().outlineColor == ui::instanceEdgeColor(ui::instanceTintFor(modCard->device())));
	pump(1200);
	const Shot modSelected = capturePpm(parent, base + "-5-modulator-selected.ppm");
	REQUIRE(modSelected.valid());
	XCloseDisplay(display);
}

TEST_CASE("PHOTO: the preset toast — \"Loaded: <name>\" at full strength, half way through its fade, and gone") {
	// Env-gated: FEATHERS_PRESET_TOAST_REPRO names a directory. The toast's clock is
	// held by the case, so each frame is the toast at a stated instant rather than whenever the camera
	// caught up.
	const char* keep = std::getenv("FEATHERS_PRESET_TOAST_REPRO");
	if (keep == nullptr) {
		return;
	}
	EditorFixture f;
	f.host.acceptLoads = true;
	int64_t now = 10'000;
	f.gui.setPresetToastClockForTest([&now] { return now; });
	Display* display = XOpenDisplay(nullptr);
	REQUIRE(display != nullptr);
	const int screen = DefaultScreen(display);
	uint32_t w = 0, h = 0;
	REQUIRE(f.gui.getSize(&w, &h));
	::Window parent = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0, w, h, 0,
	                                      BlackPixel(display, screen), BlackPixel(display, screen));
	XFlush(display);
	clap_window window {};
	window.api = CLAP_WINDOW_API_X11;
	window.x11 = parent;
	REQUIRE(f.gui.setParent(&window));
	XMapWindow(display, parent);
	XRaiseWindow(display, parent);
	XFlush(display);
	const auto pump = [&](int milliseconds) {
		for (int slice = 0; slice < std::max(1, milliseconds / 4); ++slice) {
			f.gui.processEvents();
			::usleep(4000);
		}
	};
	ui::PresetBrowser* browser = f.gui.presetBrowserForTest();
	ui::NotifyTray* toast = f.gui.presetToastForTest();
	REQUIRE(browser != nullptr);
	REQUIRE(toast != nullptr);
	pump(1200);
	const std::string base = std::string(keep) + "/preset-toast";
	const Shot quiet = capturePpm(parent, base + "-0-before.ppm");
	REQUIRE(quiet.valid());
	REQUIRE(browser->selectIndex(browser->indexOf("Init")));
	CHECK(toast->text() == "Loaded: Init");
	pump(1200);
	const Shot full = capturePpm(parent, base + "-1-loaded-full.ppm");
	REQUIRE(full.valid());
	now += 400; // half way through the fade
	CHECK(toast->opacity() == doctest::Approx(0.5f).epsilon(0.001));
	pump(1200);
	const Shot half = capturePpm(parent, base + "-2-half-faded.ppm");
	REQUIRE(half.valid());
	now += 400;
	CHECK_FALSE(toast->visible());
	pump(1200);
	const Shot gone = capturePpm(parent, base + "-3-gone.ppm");
	REQUIRE(gone.valid());
	XCloseDisplay(display);
}

TEST_CASE("REPRO: the explore terrain across a host RESIZE DRAG, photographed and measured for tiling") {
	// THE BUG: changing the window size made the Explore terrain render with a tiled effect — narrowed
	// the same day to a resize only (not the plugin scale), only sometimes, staying broken until the next
	// resize (repaints did not fix it), with the terrain repeated. The windowed venue is the only road here that
	// hosts the real frame tree — the ground frame and its bubble warp exist only in the assembled
	// editor (gui.cpp builds the ground as an earlier sibling of the stage; a headless ExploreFixture
	// has none and paints the picture unbent, which is not the picture the bug lives in).
	//
	// THE SHAPE OF THE REPRO IS A DRAG, NOT A STEP. A hand on a window edge produces dozens of
	// host-granted sizes, one per frame, each going through setNativeWindowDimensions ->
	// setNativeWindowSize -> handleResized on the editor road and drawableSizeWillChange on the
	// platform road. Forty granted steps at one frame each is that stream.
	//
	// THE MEASUREMENT IS PERIODIC SELF-SIMILARITY. "Repeated terrain" means the chart's own rows
	// recur at a period: for some vertical offset dy, row y and row y+dy agree over most of the
	// chart. A correct chart is an organic field — near rows correlate (smoothness) and far rows do
	// not; a tiled one has a sharp match at the tile period. The detector walks dy from a floor that
	// smoothness cannot reach to half the region and reports the best match and where it sits; the
	// numbers land next to the photographs for a pair of eyes.
	const char* keep = std::getenv("FEATHERS_TERRAIN_REPRO");
	if (keep == nullptr) {
		return;
	}
	EditorFixture f;
	Display* display = XOpenDisplay(nullptr);
	REQUIRE(display != nullptr);
	const int screen = DefaultScreen(display);
	uint32_t w0 = 0, h0 = 0;
	REQUIRE(f.gui.getSize(&w0, &h0));
	::Window parent = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0, w0, h0, 0,
	                                      BlackPixel(display, screen), BlackPixel(display, screen));
	XFlush(display);
	clap_window window {};
	window.api = CLAP_WINDOW_API_X11;
	window.x11 = parent;
	REQUIRE(f.gui.setParent(&window));
	XMapWindow(display, parent);
	XRaiseWindow(display, parent);
	XFlush(display);
	const auto pump = [&](int milliseconds) {
		for (int slice = 0; slice < std::max(1, milliseconds / 4); ++slice) {
			f.gui.processEvents();
			::usleep(4000);
		}
	};

	// THE EXPLORE SURFACE. The switch splits at its middle and the LEFT half names Explore
	// (header_bar.cpp surfaceAt) — a click there is the same road the hand takes. The point is found
	// by ASKING, not by assuming the switch's geometry: scan the header for the first point that both
	// lands on the switch and names Explore, which survives any re-layout of the right cluster.
	ui::HeaderBar* header = f.gui.headerForTest();
	REQUIRE(header != nullptr);
	visage::Point explorePoint { -1.0f, -1.0f };
	for (float x = header->width() - 1.0f; x > 0.0f; x -= 2.0f) {
		for (float y = 2.0f; y < header->height() - 2.0f; y += 4.0f) {
			const visage::Point at { x, y };
			if (header->hitsMode(at) && header->surfaceAt(at) == ui::StageMode::Surface::Explore) {
				explorePoint = at;
				break;
			}
		}
		if (explorePoint.x >= 0.0f) {
			break;
		}
	}
	REQUIRE(explorePoint.x >= 0.0f);
	visage::MouseEvent explorePress;
	explorePress.position = explorePoint;
	header->onMouseDown().callback(explorePress);
	pump(300);

	// SETTLE THE MAP: the builder is coalesced and finishes each window it is given, so a generous
	// pump after the switch lets the first builds land. A capture taken mid-build would measure the
	// carry, which is a different question (and a designed one).
	pump(2500);

	// THE DETECTOR. The match fraction at each vertical shift, reported as the profile's top scores
	// AND its local peaks: smoothness dominates at small shifts and decays monotonically, a tile
	// shows as a SHARP LOCAL PEAK at its period on top of that decay, and only a profile tells those
	// apart. Pairs are sampled every 2 px and matched on a max-channel difference of 6 — venue noise
	// is a couple of counts (venue-xvfb.sh's own numbers) and terrain detail is tens.
	const auto tileProfile = [](const Shot& shot, int x, int y, int w, int h) {
		std::vector<std::pair<int, float>> profile;
		const int maxDy = std::max(24, h / 2);
		for (int dy = 12; dy <= maxDy; ++dy) {
			long matched = 0;
			long total = 0;
			for (int row = y; row + dy < y + h; row += 2) {
				for (int col = x; col < x + w; col += 2) {
					const size_t top = (static_cast<size_t>(row) * shot.width + col) * 3;
					const size_t low = (static_cast<size_t>(row + dy) * shot.width + col) * 3;
					const int dr = std::abs(shot.rgb[top] - shot.rgb[low]);
					const int dg = std::abs(shot.rgb[top + 1] - shot.rgb[low + 1]);
					const int db = std::abs(shot.rgb[top + 2] - shot.rgb[low + 2]);
					matched += (std::max(dr, std::max(dg, db)) <= 6) ? 1 : 0;
					++total;
				}
			}
			profile.emplace_back(dy, total > 0 ? static_cast<float>(matched) / static_cast<float>(total)
			                                   : 0.0f);
		}
		std::vector<std::pair<int, float>> top = profile;
		std::sort(top.begin(), top.end(),
		          [](const auto& a, const auto& b) { return b.second < a.second; });
		if (top.size() > 3) {
			top.resize(3);
		}
		// A LOCAL PEAK well past the smoothness floor: better than BOTH neighbours by a real margin.
		std::vector<std::pair<int, float>> peaks;
		for (size_t at = 1; at + 1 < profile.size(); ++at) {
			if (profile[at].first >= 24 && profile[at].second > profile[at - 1].second + 0.02f
			    && profile[at].second > profile[at + 1].second + 0.02f) {
				peaks.push_back(profile[at]);
			}
		}
		std::sort(peaks.begin(), peaks.end(),
		          [](const auto& a, const auto& b) { return b.second < a.second; });
		if (peaks.size() > 3) {
			peaks.resize(3);
		}
		return std::make_pair(top, peaks);
	};
	// THE CHART REGION: the disk is inscribed in the stage band, which is the editor minus the header
	// above and the tray band below. A central window of these fractions is inside the disk at every
	// aspect this case visits, and keeps out of the lanes' text.
	const auto region = [](int w, int h, int* x, int* y, int* rw, int* rh) {
		*x = static_cast<int>(w * 0.24);
		*y = static_cast<int>(h * 0.18);
		*rw = static_cast<int>(w * 0.52);
		*rh = static_cast<int>(h * 0.58);
	};

	const std::string base = std::string(keep) + "/terrain";
	const auto report = [&](const char* label, const Shot& shot, int x, int y, int w, int h) {
		const auto [top, peaks] = tileProfile(shot, x, y, w, h);
		std::string tops;
		for (const auto& [dy, score] : top) {
			tops += " dy " + std::to_string(dy) + "=" + std::to_string(score).substr(0, 5);
		}
		std::string pks;
		for (const auto& [dy, score] : peaks) {
			pks += " dy " + std::to_string(dy) + "=" + std::to_string(score).substr(0, 5);
		}
		MESSAGE(label << " " << shot.width << "x" << shot.height << " top:" << tops
		              << "  peaks:" << (pks.empty() ? " none" : pks));
	};

	const Shot atDefault = capturePpm(parent, base + "-1-default.ppm");
	REQUIRE(atDefault.valid());
	REQUIRE_FALSE(isUniform(atDefault));
	int rx = 0, ry = 0, rw = 0, rh = 0;
	region(atDefault.width, atDefault.height, &rx, &ry, &rw, &rh);
	report("default", atDefault, rx, ry, rw, rh);

	// THE DRAG OUT — forty granted sizes, one per frame. The span arithmetic is SIGNED: toW < fromW on
	// the way back, and unsigned underflow there was handing the host a four-billion-pixel window.
	// The position walks at = 0..steps for BOTH directions — from and to already encode the
	// direction, and a reversed index made the return drag walk back OUT to the big size.
	const int steps = 40;
	const auto drag = [&](uint32_t fromW, uint32_t fromH, uint32_t toW, uint32_t toH) {
		for (int step = 0; step <= steps; ++step) {
			const long spanW = static_cast<long>(toW) - static_cast<long>(fromW);
			const long spanH = static_cast<long>(toH) - static_cast<long>(fromH);
			const uint32_t w = static_cast<uint32_t>(static_cast<long>(fromW) + spanW * step / steps);
			const uint32_t h = static_cast<uint32_t>(static_cast<long>(fromH) + spanH * step / steps);
			XResizeWindow(display, parent, w, h);
			XFlush(display);
			REQUIRE(f.gui.setSize(w, h));
			pump(16);
		}
	};
	drag(w0, h0, w0 + 400, h0 + 400);
	pump(900);
	const Shot dragged = capturePpm(parent, base + "-2-dragged-out.ppm");
	REQUIRE(dragged.valid());
	REQUIRE_FALSE(isUniform(dragged));
	region(dragged.width, dragged.height, &rx, &ry, &rw, &rh);
	report("dragged-out", dragged, rx, ry, rw, rh);

	// AND BACK — the road reported to heal it.
	drag(w0 + 400, h0 + 400, w0, h0);
	pump(900);
	const Shot back = capturePpm(parent, base + "-3-back.ppm");
	REQUIRE(back.valid());
	REQUIRE_FALSE(isUniform(back));
	region(back.width, back.height, &rx, &ry, &rw, &rh);
	report("back", back, rx, ry, rw, rh);

	// THE ROUND TRIP IS THE DETERMINISM CHECK, and on this venue it is exact: the render at the birth
	// size after a drag is the render it started with. A tiled state that survives the round trip
	// would show here as a score off the baseline. EACH shot's region is recomputed for its OWN
	// dimensions — a stale region from a bigger capture reads past the smaller one's buffer.
	{
		int bx = 0, by = 0, bw = 0, bh = 0;
		region(back.width, back.height, &bx, &by, &bw, &bh);
		int dx = 0, dy0 = 0, dw = 0, dh = 0;
		region(atDefault.width, atDefault.height, &dx, &dy0, &dw, &dh);
		const auto [topBack, peaksBack] = tileProfile(back, bx, by, bw, bh);
		const auto [topDefault, peaksDefault] = tileProfile(atDefault, dx, dy0, dw, dh);
		REQUIRE_FALSE(topBack.empty());
		REQUIRE_FALSE(topDefault.empty());
		MESSAGE("determinism: back top score " << topBack.front().second << " vs default "
		                                       << topDefault.front().second);
	}

	// **THE macOS INGREDIENT THIS VENUE CAN REACH: THE SCALE CHANGES MID-DRAG.** On macOS
	// syncContentScale re-derives the content scale FROM the window size — every drag step is a
	// resize AND a scale change at once, which is literally "changing window size and scaling". So
	// drag again with the UI scale stepped between sizes: up a rung, a few sizes, up a rung, drag
	// back, down rungs. If the tiled state needs the two roads interleaved, this is where it shows.
	{
		ui::HeaderBar* header = f.gui.headerForTest();
		const auto scaleStep = [&](visage::Bounds (ui::HeaderBar::*button)() const) {
			const visage::Bounds box = (header->*button)();
			visage::MouseEvent e;
			e.position = { box.x() + box.width() * 0.5f, box.y() + box.height() * 0.5f };
			header->onMouseDown().callback(e);
			pump(40);
		};
		for (int step = 1; step <= steps; ++step) {
			const uint32_t w = w0 + static_cast<uint32_t>((static_cast<long>(300) * step) / steps);
			const uint32_t h = h0 + static_cast<uint32_t>((static_cast<long>(300) * step) / steps);
			XResizeWindow(display, parent, w, h);
			XFlush(display);
			REQUIRE(f.gui.setSize(w, h));
			pump(16);
			if (step % 10 == 0 && f.gui.uiScaleForTest() < 1.5) {
				scaleStep(&ui::HeaderBar::uiScalePlusBounds);
			}
		}
		pump(900);
		const Shot scaled = capturePpm(parent, base + "-4-scaled-drag.ppm");
		REQUIRE(scaled.valid());
		REQUIRE_FALSE(isUniform(scaled));
		region(scaled.width, scaled.height, &rx, &ry, &rw, &rh);
		report("scaled-drag", scaled, rx, ry, rw, rh);
		const double scaleNow = f.gui.uiScaleForTest();
		MESSAGE("scale after interleaved drag: " << scaleNow);

		// AND HOME: sizes back, scale back down the ladder.
		for (int step = 1; step <= steps; ++step) {
			const uint32_t w = w0 + 300
			                       - static_cast<uint32_t>((static_cast<long>(300) * step) / steps);
			const uint32_t h = h0 + 300
			                       - static_cast<uint32_t>((static_cast<long>(300) * step) / steps);
			XResizeWindow(display, parent, w, h);
			XFlush(display);
			REQUIRE(f.gui.setSize(w, h));
			pump(16);
			if (step % 10 == 0 && f.gui.uiScaleForTest() > 1.0) {
				scaleStep(&ui::HeaderBar::uiScaleMinusBounds);
			}
		}
		pump(900);
		const Shot home = capturePpm(parent, base + "-5-scaled-home.ppm");
		REQUIRE(home.valid());
		region(home.width, home.height, &rx, &ry, &rw, &rh);
		report("scaled-home", home, rx, ry, rw, rh);
	}

	XCloseDisplay(display);
}

TEST_CASE("REPRO: the arm halo on a lane knob and on an expanded panel's knob, photographed unarmed and armed") {
	// THE BUG: the modulation glow did not reach the expanded panel's controls, in the floating
	// window or the canvas. Every headless predicate says lit, so this asks the glass:
	// the editor in a real X window, harm's panel floating, a macro armed, and the colour INSIDE each
	// knob's face — where the halo's radial gradient shows through — sampled before and after.
	const char* keep = std::getenv("FEATHERS_GLOW_REPRO");
	if (keep == nullptr) {
		return;
	}
	EditorFixture f;
	Display* display = XOpenDisplay(nullptr);
	REQUIRE(display != nullptr);
	const int screen = DefaultScreen(display);
	uint32_t w = 0, h = 0;
	REQUIRE(f.gui.getSize(&w, &h));
	::Window parent = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0, w, h, 0,
	                                      BlackPixel(display, screen), BlackPixel(display, screen));
	XFlush(display);
	clap_window window {};
	window.api = CLAP_WINDOW_API_X11;
	window.x11 = parent;
	REQUIRE(f.gui.setParent(&window));
	XMapWindow(display, parent);
	XRaiseWindow(display, parent);
	XFlush(display);
	const auto pump = [&](int milliseconds) {
		for (int slice = 0; slice < std::max(1, milliseconds / 4); ++slice) {
			f.gui.processEvents();
			::usleep(4000);
		}
	};

	const size_t slot = f.addFx(fx::harm::kTraits.tag);
	f.lane->scrollCardIntoView(slot);
	f.lane->resized();
	pump(600);
	ui::ExpandedOverlay* overlay = f.gui.expandedForTest();
	REQUIRE(overlay != nullptr);
	(void)f.gui.canvasForTest()->setCollapsed(true);
	f.lane->onExpandDevice(slot);
	REQUIRE(overlay->open());
	pump(900);

	std::function<void(visage::Frame*, std::vector<ui::Knob*>*)> collect = [&](visage::Frame* at, std::vector<ui::Knob*>* out) {
		if (auto* knob = dynamic_cast<ui::Knob*>(at); knob != nullptr && knob->isModDestination() && knob->width() > 0.0f) {
			out->push_back(knob);
		}
		for (visage::Frame* child : at->children()) {
			collect(child, out);
		}
	};
	std::vector<ui::Knob*> laneKnobs;
	std::vector<ui::Knob*> panelKnobs;
	collect(f.lane, &laneKnobs);
	collect(overlay->panelForTest(), &panelKnobs);
	REQUIRE_FALSE(laneKnobs.empty());
	REQUIRE_FALSE(panelKnobs.empty());
	ui::Knob* laneKnob = laneKnobs.front();
	ui::Knob* panelKnob = panelKnobs.front();

	const std::string base = std::string(keep) + "/glow";
	const Shot unarmed = capturePpm(parent, base + "-1-unarmed.ppm");
	REQUIRE(unarmed.valid());
	const float scale = static_cast<float>(unarmed.width) / static_cast<float>(f.root->width());
	// MEAN COLOUR INSIDE THE FACE, radius 0.3 of it, where the halo's gradient shows through.
	const auto faceMean = [&](const Shot& shot, ui::Knob* knob, float* r, float* g, float* b) {
		const visage::Point c = knob->positionInWindow()
		                        + visage::Point { knob->width() * 0.5f, knob->faceDiameter() * 0.5f };
		const float radius = knob->faceDiameter() * 0.3f * scale;
		const int cx = static_cast<int>(c.x * scale);
		const int cy = static_cast<int>(c.y * scale);
		double sr = 0, sg = 0, sb = 0;
		int n = 0;
		for (int y = cy - static_cast<int>(radius); y <= cy + static_cast<int>(radius); ++y) {
			for (int x = cx - static_cast<int>(radius); x <= cx + static_cast<int>(radius); ++x) {
				if (x < 0 || y < 0 || x >= shot.width || y >= shot.height) {
					continue;
				}
				const float dx = static_cast<float>(x - cx);
				const float dy = static_cast<float>(y - cy);
				if (dx * dx + dy * dy > radius * radius) {
					continue;
				}
				const size_t at = (static_cast<size_t>(y) * shot.width + static_cast<size_t>(x)) * 3;
				sr += shot.rgb[at];
				sg += shot.rgb[at + 1];
				sb += shot.rgb[at + 2];
				++n;
			}
		}
		*r = n > 0 ? static_cast<float>(sr / n) : -1.0f;
		*g = n > 0 ? static_cast<float>(sg / n) : -1.0f;
		*b = n > 0 ? static_cast<float>(sb / n) : -1.0f;
	};
	float lr0, lg0, lb0, pr0, pg0, pb0;
	faceMean(unarmed, laneKnob, &lr0, &lg0, &lb0);
	faceMean(unarmed, panelKnob, &pr0, &pg0, &pb0);
	MESSAGE("unarmed lane " << lr0 << "," << lg0 << "," << lb0 << " panel " << pr0 << "," << pg0 << "," << pb0
	                        << " lit " << laneKnob->haloLit() << "/" << panelKnob->haloLit());

	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);
	(void)mod->arm(true);
	mod->focus(backplane::Source::core(0));
	pump(900);
	const Shot armed = capturePpm(parent, base + "-2-armed.ppm");
	REQUIRE(armed.valid());
	float lr1, lg1, lb1, pr1, pg1, pb1;
	faceMean(armed, laneKnob, &lr1, &lg1, &lb1);
	faceMean(armed, panelKnob, &pr1, &pg1, &pb1);
	MESSAGE("armed   lane " << lr1 << "," << lg1 << "," << lb1 << " panel " << pr1 << "," << pg1 << "," << pb1
	                        << " lit " << laneKnob->haloLit() << "/" << panelKnob->haloLit()
	                        << " scale " << scale << " watched " << mod->watches(laneKnob) << "/" << mod->watches(panelKnob));
	XCloseDisplay(display);
}
TEST_CASE("PHOTO: backlog LH — Explore's corners and buttons with every rail open, at the design size and at a narrow stage") {
	// At some scales Rotate and Theta overlapped the mutate and undo/redo buttons. The geometry case
	// (test_gui_widgets_explore, "backlog LH: ...") is the claim; this is the look, for a pair of eyes. Off unless
	// FEATHERS_LH_PHOTO names a directory.
	const char* keep = std::getenv("FEATHERS_LH_PHOTO");
	if (keep == nullptr) {
		return;
	}
	EditorFixture f;
	Display* display = XOpenDisplay(nullptr);
	REQUIRE(display != nullptr);
	const int screen = DefaultScreen(display);
	uint32_t w0 = 0, h0 = 0;
	REQUIRE(f.gui.getSize(&w0, &h0));
	::Window parent = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0, w0, h0, 0,
	                                      BlackPixel(display, screen), BlackPixel(display, screen));
	XFlush(display);
	clap_window window {};
	window.api = CLAP_WINDOW_API_X11;
	window.x11 = parent;
	REQUIRE(f.gui.setParent(&window));
	XMapWindow(display, parent);
	XRaiseWindow(display, parent);
	XFlush(display);
	const auto pump = [&](int milliseconds) {
		for (int slice = 0; slice < std::max(1, milliseconds / 4); ++slice) {
			f.gui.processEvents();
			::usleep(4000);
		}
	};
	ui::HeaderBar* header = f.gui.headerForTest();
	REQUIRE(header != nullptr);
	visage::Point explorePoint { -1.0f, -1.0f };
	for (float x = header->width() - 1.0f; x > 0.0f && explorePoint.x < 0.0f; x -= 2.0f) {
		for (float y = 2.0f; y < header->height() - 2.0f; y += 4.0f) {
			const visage::Point at { x, y };
			if (header->hitsMode(at) && header->surfaceAt(at) == ui::StageMode::Surface::Explore) {
				explorePoint = at;
				break;
			}
		}
	}
	REQUIRE(explorePoint.x >= 0.0f);
	visage::MouseEvent explorePress;
	explorePress.position = explorePoint;
	header->onMouseDown().callback(explorePress);
	// EVERY RAIL OPEN — the narrowest stage the design canvas has.
	for (visage::Frame* child : f.root->children()) {
		if (auto* rail = dynamic_cast<ui::Rail*>(child); rail != nullptr) {
			(void)rail->setCollapsed(false);
		}
	}
	pump(1500);
	const visage::Frame* stage = nullptr;
	for (visage::Frame* child : f.root->children()) {
		if (child->isVisible() && dynamic_cast<ui::ExploreStage*>(child) != nullptr) {
			stage = child;
		}
	}
	REQUIRE(stage != nullptr);
	MESSAGE("design size, every rail open: stage " << stage->width() << "x" << stage->height());
	const Shot open = capturePpm(parent, std::string(keep) + "/lh-1-rails-open.ppm");
	REQUIRE(open.valid());
	XDestroyWindow(display, parent);
	XCloseDisplay(display);
}

#endif // defined(__linux__) — the two REPRO photographs

TEST_CASE("a beat during a partition drag leaves the hand alone — the patch's extent is applied only once the hand lets go") {
	// THE BUG: expanding worked, but compressing was jumpy and erratic and hard to size down. The tick was re-applying the patch's extent — the last release's —
	// under the hand on every beat. It worked once because a fresh patch holds no extent.
	EditorFixture f;
	ui::ExpandedCanvas* canvas = f.gui.canvasForTest();
	REQUIRE(canvas != nullptr);
	REQUIRE(canvas->setCollapsed(false)); // the grip is only there while the band is open
	REQUIRE(canvas->setOpenExtent(400.0f));
	canvas->onExtentCommit();
	REQUIRE(f.bridge.canvasView().extent == doctest::Approx(400.0f));

	const auto at = [](float x, float y, float wx, float wy) {
		visage::MouseEvent e;
		e.position = { x, y };
		e.window_position = { wx, wy };
		return e;
	};
	const visage::Point origin = canvas->positionInWindow();
	canvas->onMouseDown().callback(at(300.0f, 2.0f, origin.x + 300.0f, origin.y + 2.0f));
	REQUIRE(canvas->draggingExtentForTest());
	// THE HAND MOVES DOWN 150: the band shrinks. BY HOW MUCH is the host's arithmetic — the partition
	// goes under the pointer physically and the instrument keeps its 700 rows (`partitionExtent`) — so
	// the number is not restated here; what is asserted is that it shrank, that the beat leaves it, and
	// that the release writes what the frame shows.
	canvas->onMouseDrag().callback(at(300.0f, 152.0f, origin.x + 300.0f, origin.y + 152.0f));
	const float dragged = canvas->openExtent();
	CHECK(dragged < 400.0f);
	CHECK(dragged >= ui::ExpandedCanvas::kMinExtent);
	// THE BEAT, MID-DRAG: the patch still says 400, and the frame must NOT snap back to it.
	(void)f.gui.tickForTest();
	(void)f.gui.tickForTest();
	CHECK(canvas->openExtent() == doctest::Approx(dragged));
	CHECK(f.bridge.canvasView().extent == doctest::Approx(400.0f)); // not written until release
	// THE RELEASE writes it, and the next beat finds nothing to move.
	canvas->onMouseUp().callback(at(300.0f, 152.0f, origin.x + 300.0f, origin.y + 152.0f));
	CHECK(f.bridge.canvasView().extent == doctest::Approx(dragged));
	(void)f.gui.tickForTest();
	CHECK(canvas->openExtent() == doctest::Approx(dragged));
}

//--------------------------------------------------------------------------------------------------//
// A DROPDOWN HANGS OFF ITS BOX, IN EVERY HOST A PANEL CAN BE IN (backlog GG's placement sweep)       //
//--------------------------------------------------------------------------------------------------//

namespace {

template <typename T>
T* findIn(visage::Frame* root) {
	if (root == nullptr) {
		return nullptr;
	}
	if (T* hit = dynamic_cast<T*>(root)) {
		return hit;
	}
	for (visage::Frame* child : root->children()) {
		if (T* hit = findIn<T>(child)) {
			return hit;
		}
	}
	return nullptr;
}

/// OPEN OWLTT'S MODE MENU in whatever host holds its panel, and check it hangs off the Mode box.
void checkOwlttModeMenu(visage::Frame* host) {
	ui::OwlttExpanded* panel = findIn<ui::OwlttExpanded>(host);
	REQUIRE(panel != nullptr);
	REQUIRE(panel->width() > 0.0f);
	panel->showModeMenu();
	menu_check::checkHangsOff(ui::lastMenuPlacementForTest(), menu_check::inTop(*panel, panel->modeBounds()));
}

} // namespace

TEST_CASE("GG: OWLTT's Mode menu hangs off its box in the overlay, the canvas and a popped-out window, at 100, "
          "150 and 175 %") {
	// THE BUG: in some places (OWLTT among them) a dropdown menu did not appear next to the box it
	// belongs to. The panel has THREE hosts and the popout — the default —
	// is a window the size of the panel, which is where visage's own flip threw the menu to the top.
	for (const double scale : { 1.0, 1.5, 1.75 }) {
		CAPTURE(scale);
		clearUiScalePref();
		if (scale != 1.0) {
			writeUiScalePref(scale);
		}
		{
			EditorFixture f;
			REQUIRE(f.gui.uiScaleForTest() == doctest::Approx(scale));
			const size_t slot = f.addFx(fx::owltt::kTraits.tag);
			ui::ExpandedCanvas* canvas = f.gui.canvasForTest();
			REQUIRE(canvas != nullptr);

			SUBCASE("the overlay") {
				f.gui.setPopoutWindowForTest(false);
				(void)canvas->setCollapsed(true);
				f.lane->onExpandDevice(slot);
				REQUIRE(f.gui.expandedForTest()->open());
				checkOwlttModeMenu(f.root);
			}
			SUBCASE("the canvas, docked at the bottom of the editor") {
				f.gui.setPopoutWindowForTest(false);
				REQUIRE(canvas->setCollapsed(false));
				f.lane->onExpandDevice(slot);
				REQUIRE(f.gui.expandedForTest()->open());
				checkOwlttModeMenu(f.root);
			}
			SUBCASE("a popped-out window") {
				f.gui.setPopoutWindowForTest(true);
				f.lane->onExpandDevice(slot);
				ui::WindowedExpanded* window = f.gui.expandedWindowForTest();
				REQUIRE(window != nullptr);
				REQUIRE(window->open());
				checkOwlttModeMenu(window->windowForTest());
			}
		}
	}
	clearUiScalePref();
}

#if defined(__linux__)
namespace {
/// BACKLOG KS: A POINTER THROUGH THE X SERVER into the editor's own window — press, motion and release go
/// through visage's X11 backend and its event handler, which is where input state that outlives a close sits.
/// through the X server and visage's X11 backend, which is where state that outlives a close would sit.
struct EditorPointer {
	Display* display = nullptr;
	::Window target = 0;

	void send(int type, int x, int y, unsigned int button, unsigned int state) {
		XEvent e {};
		if (type == MotionNotify) {
			e.xmotion.type = MotionNotify;
			e.xmotion.display = display;
			e.xmotion.window = target;
			e.xmotion.x = x;
			e.xmotion.y = y;
			e.xmotion.state = state;
			e.xmotion.same_screen = True;
			XSendEvent(display, target, False, PointerMotionMask, &e);
		}
		else {
			e.xbutton.type = type;
			e.xbutton.display = display;
			e.xbutton.window = target;
			e.xbutton.x = x;
			e.xbutton.y = y;
			e.xbutton.button = button;
			e.xbutton.state = state;
			e.xbutton.same_screen = True;
			XSendEvent(display, target, False, type == ButtonPress ? ButtonPressMask : ButtonReleaseMask, &e);
		}
		XFlush(display);
	}
};

void pumpEditor(Gui& gui, int rounds = 20) {
	for (int i = 0; i < rounds; ++i) {
		gui.processEvents();
		usleep(2000);
	}
}



/// AN EDITOR IN A REAL X PARENT THAT CAN BE CLOSED AND OPENED AGAIN THE WAY A HOST DOES IT: destroy, the
/// container gone, create, a new container. The editor object is the same one throughout — that is
/// `PluginWindow`'s rule, and it is why anything held by the hand at the close is still held at the open.
struct ReopenRig {
	EditorFixture f;
	Display* display = XOpenDisplay(nullptr);
	::Window parent = 0;
	const clap_id fine = static_cast<clap_id>(ParamId::FineTune);

	ReopenRig() {
		REQUIRE(display != nullptr);
		open();
	}
	~ReopenRig() {
		f.gui.destroy();
		if (parent != 0) {
			XDestroyWindow(display, parent);
		}
		XCloseDisplay(display);
	}
	void open() {
		const int screen = DefaultScreen(display);
		uint32_t w = 0, h = 0;
		REQUIRE(f.gui.getSize(&w, &h));
		parent = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0, w, h, 0,
		                             BlackPixel(display, screen), BlackPixel(display, screen));
		XMapWindow(display, parent);
		// XSync, NOT XFlush: the editor reparents through visage's OWN connection, and a flush only puts the
		// create in this connection's socket, and the server may read the other client first. MEASURED: the reopen
		// case below failed `setParent` once, at its 19th cycle, in a loaded ctest run (load ~30), and passed five
		// times alone; this ordering is the likely cause, not a proven one, so the X report rides on a failure.
		XSync(display, False);
		clap_window window {};
		window.api = CLAP_WINDOW_API_X11;
		window.x11 = parent;
		const bool parented = f.gui.setParent(&window);
		const std::string xReport = ui::PluginWindow::describe(f.gui.lastXReport());
		CAPTURE(xReport);
		REQUIRE(parented);
		pumpEditor(f.gui);
	}
	void close() {
		f.gui.destroy();
		XDestroyWindow(display, parent);
		parent = 0;
		XFlush(display);
	}
	void reopen() {
		REQUIRE(f.gui.create(CLAP_WINDOW_API_X11, false));
		open();
	}
	EditorPointer pointer() {
		return EditorPointer { display, reinterpret_cast<::Window>(
		                                    static_cast<visage::ApplicationWindow*>(f.root)->window()->nativeHandle()) };
	}
	/// The fine-tune knob's centre in the window's native pixels — what an X event carries.
	visage::IPoint knobCentre() {
		const auto watchers = f.gui.widgets()->watchers(fine);
		REQUIRE_FALSE(watchers.empty());
		const visage::Frame* k = watchers.front();
		const visage::Point c = k->positionInWindow() + visage::Point(k->width() / 2, k->height() / 2);
		const double scale = f.root->dpiScale();
		return { static_cast<int>(c.x * scale), static_cast<int>(c.y * scale) };
	}
	void press(visage::IPoint at) {
		EditorPointer p = pointer();
		p.send(MotionNotify, at.x, at.y, 0, 0);
		p.send(ButtonPress, at.x, at.y, Button1, 0);
		pumpEditor(f.gui, 5);
	}
	void moveHeld(visage::IPoint at) {
		pointer().send(MotionNotify, at.x, at.y, 0, Button1Mask);
		pumpEditor(f.gui, 2);
	}
	/// Press the knob, pull it up by 60 pixels, let go: how far the PARAMETER moved.
	double dragKnobUp() {
		const visage::IPoint at = knobCentre();
		const double before = f.bridge.value(fine);
		press(at);
		for (int i = 1; i <= 10; ++i) {
			moveHeld({ at.x, at.y - 6 * i });
		}
		pointer().send(ButtonRelease, at.x, at.y - 60, Button1, Button1Mask);
		pumpEditor(f.gui, 5);
		return f.bridge.value(fine) - before;
	}
};

struct ClaimCount : ui::KeyboardClaimBackend {
	int takes = 0;
	int givebacks = 0;
	uint64_t take(const visage::Frame&) override { return static_cast<uint64_t>(++takes); }
	void giveBack(uint64_t) override { ++givebacks; }
	[[nodiscard]] int outstanding() const noexcept { return takes - givebacks; }
};

} // namespace

TEST_CASE("the open editor re-measures no text on a steady frame, before and after a session's worth of other "
          "strings") {
	// Grabbing a new control was slow while recently grabbed controls responded faster. The
	// tracked-text cache kept its first 64 strings and re-measured every other one on EVERY draw, so the editor's per-frame
	// text work grew with how much the session had shown. BY COUNT, through the real window: a whole-tree repaint of a
	// settled editor measures nothing; after thousands of other strings (a long session), the first repaint re-measures at
	// most what was evicted and the next measures nothing again.
	ReopenRig rig;
	auto* app = static_cast<visage::ApplicationWindow*>(rig.f.root);
	const auto repaint = [&] {
		app->redrawAll();
		pumpEditor(rig.f.gui);
		return ui::trackedTextMeasures();
	};
	(void)repaint();
	(void)repaint();
	const uint64_t settled = repaint();
	CHECK(repaint() == settled); // a steady frame measures nothing
	// THE VENUE CAN EXPRESS IT: a cold cache makes the very same repaint measure the editor's captions.
	ui::clearFontCache();
	const uint64_t cold = ui::trackedTextMeasures();
	const uint64_t afterCold = repaint();
	REQUIRE(afterCold > cold);
	const uint64_t editorStrings = afterCold - cold;
	MESSAGE("the editor's tracked strings on one repaint: " << editorStrings << " (cap " << ui::trackedTextCacheCapacity() << ")");
	CHECK(repaint() == afterCold);
	// A LONG SESSION: more distinct strings than the cache holds, none of them the editor's — and shown FIRST, from a
	// cold cache, so the editor's own strings arrive at a full cache (the order the old cache failed in: it kept its
	// first 64 and re-measured everything after them on every draw).
	ui::clearFontCache();
	const visage::Font& face = ui::font(ui::kControlSize);
	for (size_t i = 0; i < ui::trackedTextCacheCapacity() + 500; ++i) {
		(void)ui::trackedWidth(face, "elsewhere " + std::to_string(i), 0.0f);
	}
	const uint64_t afterSession = ui::trackedTextMeasures();
	const uint64_t refill = repaint() - afterSession;
	CHECK(refill <= editorStrings); // the evicted come back once, never more than the editor shows
	const uint64_t steady = ui::trackedTextMeasures();
	CHECK(repaint() == steady); // and the next frame measures nothing, however much the session has seen
}

TEST_CASE("grabbing controls measures each string once, through more distinct grabs than the old cache held strings") {
	// Seen on macOS: grabbing a control not grabbed recently responded slowly, recently grabbed ones faster. THROUGH
	// THE REAL GESTURE ROAD: X button and motion events on every visible knob of the open
	// editor (with five more cards in the lane), each in four drags of different lengths and each drag twice — more
	// distinct grabs than the old cache's 64 strings. A grab's readouts are NOT the same strings twice (MEASURED: which
	// intermediate values reach a frame depends on how motion lands in the pump), so the claim is not "a repeat measures
	// nothing" but the cache's own contract: with nothing evicted, NO STRING IS EVER MEASURED TWICE (the audit counts
	// re-measures), and a steady repaint measures nothing. The old cache (its first 64 strings, then every later one
	// re-measured on every draw) fails both once full.
	ReopenRig rig;
	for (const char* tag : { "DELAY", "flt1", "OWLTT", "SKLITTER", "flt1" }) {
		(void)rig.f.addFx(tag);
	}
	auto* app = static_cast<visage::ApplicationWindow*>(rig.f.root);
	ui::clearFontCache();
	ui::setTrackedTextAudit(true);
	const uint64_t measuredBefore = ui::trackedTextMeasures();
	app->redrawAll();
	pumpEditor(rig.f.gui);
	std::vector<ui::Knob*> knobs;
	std::function<void(visage::Frame*)> collect = [&](visage::Frame* at) {
		if (auto* knob = dynamic_cast<ui::Knob*>(at); knob != nullptr && knob->isVisible() && knob->width() > 0.0f
		    && knob->control() != nullptr) {
			knobs.push_back(knob);
		}
		for (visage::Frame* child : at->children()) {
			if (child->isVisible()) {
				collect(child);
			}
		}
	};
	collect(rig.f.root);
	const double scale = rig.f.root->dpiScale();
	const auto grab = [&](ui::Knob* k, int step) {
		const visage::Point c = k->positionInWindow() + visage::Point(k->width() / 2, k->height() / 2);
		const visage::IPoint at { static_cast<int>(c.x * scale), static_cast<int>(c.y * scale) };
		// NOT A DOUBLE CLICK: a press on the same spot within the double-click time counts as a repeat click, which
		// resets the knob (MEASURED: 'Voice Level reset'). A middle-button click at the window's corner first makes
		// visage count this press as a first click (`windowing.cpp`: a repeat needs the same button in the same place).
		rig.pointer().send(ButtonPress, 1, 1, Button2, 0);
		rig.pointer().send(ButtonRelease, 1, 1, Button2, Button2Mask);
		pumpEditor(rig.f.gui, 2);
		rig.press(at);
		for (int i = 1; i <= 4; ++i) {
			rig.moveHeld({ at.x, at.y - step * i });
		}
		rig.pointer().send(ButtonRelease, at.x, at.y - 4 * step, Button1, Button1Mask);
		pumpEditor(rig.f.gui, 5);
	};
	size_t distinctGrabs = 0;
	for (ui::Knob* k : knobs) {
		for (const int step : { 6, 9, 13, 20 }) {
			const double start = k->control()->value();
			grab(k, step);
			if (k->control()->value() == start) {
				continue; // not a grab this knob turns for (covered, another road, a pinned value)
			}
			k->control()->setValue(start);
			pumpEditor(rig.f.gui, 5);
			grab(k, step); // the same control again: a "recently grabbed" one
			k->control()->setValue(start);
			pumpEditor(rig.f.gui, 5);
			++distinctGrabs;
		}
	}
	MESSAGE("knobs " << knobs.size() << ", distinct grabs " << distinctGrabs << ", strings measured "
	                 << ui::trackedTextMeasures() << ", cache " << ui::trackedTextCacheSize() << ", re-measures "
	                 << ui::trackedTextRemeasures());
	CHECK(ui::trackedTextRemeasures() == 0); // every string measured once, however often it was drawn
	CHECK(ui::trackedTextEvictions() == 0);
	REQUIRE(distinctGrabs > 64);                                  // more distinct grabs than the old cache held strings
	REQUIRE(ui::trackedTextMeasures() - measuredBefore > 64);     // and more distinct strings shown
	app->redrawAll();
	pumpEditor(rig.f.gui);
	const uint64_t settled = ui::trackedTextMeasures();
	app->redrawAll();
	pumpEditor(rig.f.gui);
	CHECK(ui::trackedTextMeasures() == settled); // and a steady frame measures nothing
	ui::setTrackedTextAudit(false);
}

TEST_CASE("twenty closes and reopens leave the editor's per-frame work where it was: timers, preference watches, and "
          "the frames one parameter move tells") {
	// The UI on macOS grew sluggish over a session and was faster after a restart. If anything an
	// editor hangs on the process at open (a visage timer, a preference watch, a parameter watcher) were not taken down
	// at close, every reopen would multiply the work each tick and each notify does. BY VALUE: the counts after each of
	// twenty reopens are the counts after the first open, exactly.
	ReopenRig rig;
	const auto settle = [&] {
		for (int i = 0; i < 4; ++i) {
			(void)rig.f.gui.tickForTest();
		}
	};
	// One parameter move, delivered: how many frames the tick told.
	double fineValue = 0.0;
	const auto framesOneMoveTells = [&] {
		settle();
		fineValue = fineValue == 0.0 ? 0.25 : 0.0;
		rig.f.bridge.setValue(rig.fine, fineValue);
		return rig.f.gui.tickForTest().paramFrames;
	};
	const size_t timers = visage::EventManager::instance().timerCount();
	const size_t watches = ui::SettingsScreen::prefWatchCount();
	const size_t watchers = rig.f.gui.widgets()->watchers(rig.fine).size();
	const size_t told = framesOneMoveTells();
	REQUIRE(timers > 0);   // the heartbeat runs: the count can express a leak
	REQUIRE(watches >= 6); // one of each preference watch: chrome, accent, tooltip, side, arps, repaint
	REQUIRE(told > 0);     // a move reaches the screen: the count can express a doubling
	for (int cycle = 1; cycle <= 20; ++cycle) {
		CAPTURE(cycle);
		rig.close();
		CHECK(visage::EventManager::instance().timerCount() < timers); // the closed editor's heartbeat stopped
		rig.reopen();
		CHECK(visage::EventManager::instance().timerCount() == timers);
		CHECK(ui::SettingsScreen::prefWatchCount() == watches);
		CHECK(rig.f.gui.widgets()->watchers(rig.fine).size() == watchers);
		CHECK(framesOneMoveTells() == told);
	}
}

TEST_CASE("backlog KS: closing the editor ends assign mode, so the reopened editor's knobs turn rather than route") {
	// After closing and reopening the UI with macro assign mode active, the entire UI was frozen and
	// unresponsive. The editor is built once and kept
	// (PluginWindow), so its ModState is too — and a knob dragged while armed edits a route's amount,
	// never its value. MEASURED before the fix: the reopened editor answered armed, and the drag moved the
	// parameter by exactly 0.
	ReopenRig rig;
	REQUIRE(rig.dragKnobUp() != 0.0); // the venue can express a turning knob before anything is armed
	ui::ModState* mod = rig.f.gui.modForTest();
	REQUIRE(mod != nullptr);

	SUBCASE("the editor's own arm") {
		mod->arm(true);
	}
	SUBCASE("a following state's LOCAL arm — a nested chain's, which `arm(false)` on the root would leave on") {
		ui::ModState inner;
		inner.setParent(mod);
		inner.arm(true);
		REQUIRE(inner.armedLocally());
		REQUIRE_FALSE(mod->armedLocally());
		rig.close();
		CHECK_FALSE(inner.armedLocally());
		inner.setParent(nullptr);
		rig.reopen();
		CHECK_FALSE(mod->armed());
		return;
	}
	REQUIRE(mod->armed());
	rig.close();
	CHECK_FALSE(mod->armed()); // THE VALUE: the close itself ended it
	rig.reopen();
	CHECK_FALSE(mod->armed());
	CHECK(rig.dragKnobUp() != 0.0); // and the symptom: a knob turns again
}

TEST_CASE("backlog KS: closing the editor mid-drag lets go of the knob — no host gesture stays open, and the next press "
          "is an ordinary one") {
	// A host closing the editor while a button is held never delivers the release to it: the window is
	// detached first. The press stayed visage's `mouse_down_frame_` and the knob's gesture stayed open —
	// a host told "the user is still touching this parameter" for as long as the editor stayed shut.
	ReopenRig rig;
	const visage::IPoint at = rig.knobCentre();
	rig.press(at);
	for (int i = 1; i <= 5; ++i) {
		rig.moveHeld({ at.x, at.y - 6 * i });
	}
	REQUIRE(rig.f.bridge.openGestureCountForTest() == 1); // the drag really is in progress
	rig.close();
	CHECK(rig.f.bridge.openGestureCountForTest() == 0); // THE VALUE: the close let go
	rig.reopen();
	CHECK(rig.dragKnobUp() != 0.0);
	CHECK(rig.f.bridge.openGestureCountForTest() == 0);
}

TEST_CASE("backlog KS: closing the editor mid-edit cancels the field and gives the keyboard back to the host") {
	// The field ends on focus loss, and a close never told it — so the X keyboard claim outlived the editor
	// (`keyboard_claim.h`: "a claim left standing is a DAW without its keyboard").
	ClaimCount claims;
	const ui::ScopedKeyboardClaimBackend scoped(claims);
	ReopenRig rig;
	ui::TextField field;
	field.setBounds(10, 10, 120, 18);
	rig.f.root->addChild(&field);
	field.begin("Macro");
	REQUIRE(field.editing());
	REQUIRE(claims.outstanding() == 1);
	rig.close();
	CHECK_FALSE(field.editing());
	CHECK(claims.outstanding() == 0);
	rig.reopen();
	CHECK_FALSE(field.editing());
	rig.f.root->removeChild(&field);
}

TEST_CASE("backlog KS: closing the editor closes an open preset prompt and gives its keyboard back — and only this "
          "editor's") {
	// The save/edit prompt does not end on a focus loss (its fields are visage's own), so the window being
	// told it lost focus left it open with its KeyboardClaim standing: the host's keyboard held by a window
	// that had left the screen.
	ClaimCount claims;
	const ui::ScopedKeyboardClaimBackend scoped(claims);
	ReopenRig rig;
	ui::PresetBrowser* browser = rig.f.gui.presetBrowserForTest();
	REQUIRE(browser != nullptr);
	// IN THIS EDITOR'S TREE, or "closed because it is in the editor" would be passing for another reason.
	REQUIRE(browser->parent() == rig.f.root);
	// AND A BROWSER THAT IS NOT — another editor's — whose prompt the close must leave alone.
	ui::PresetBrowser elsewhere(std::vector<std::string> { "Init" }, std::vector<ui::PresetData> { ui::PresetData { { 1 } } },
	                            "KS", ui::PresetBrowser::kHeaderStyle);
	elsewhere.setBounds(0.0f, 0.0f, elsewhere.totalWidth(), ui::PresetBrowser::kHeight);

	browser->chooseFromMenu(ui::PresetBrowser::kMenuSave); // the edit road opens the same prompt (openPrompt)
	elsewhere.chooseFromMenu(ui::PresetBrowser::kMenuSave);
	REQUIRE(browser->prompt() != ui::PresetBrowser::Prompt::Closed);
	REQUIRE(elsewhere.prompt() != ui::PresetBrowser::Prompt::Closed);
	REQUIRE(claims.outstanding() == 2);

	rig.close();
	CHECK(browser->prompt() == ui::PresetBrowser::Prompt::Closed); // THE VALUE
	CHECK(claims.outstanding() == 1); // this editor's claim given back; the other's still held
	CHECK(elsewhere.prompt() != ui::PresetBrowser::Prompt::Closed);
	rig.reopen();
	CHECK(browser->prompt() == ui::PresetBrowser::Prompt::Closed);
	elsewhere.cancelPrompt();
	CHECK(claims.outstanding() == 0);
}
#endif

#if defined(__linux__)
TEST_CASE("backlog GP: after the host destroys its container first, the REBUILT Feathers window still hears Esc at its "
          "root — the editor's own handlers are re-hung on the new window") {
	// The rebuild (`PluginWindow::rebuild`) moves every child frame into a fresh window; what was set ON the window —
	// the root's key handler, its background, its layout on a resize — is the client's to install again
	// (`Client::windowRebuilt`). A rebuild that forgot would hand the host a window whose Esc, undo and scale keys go
	// nowhere. Asked of the NEW root, armed by the mode Esc exists to leave.
	EditorFixture f;
	Display* display = XOpenDisplay(nullptr);
	REQUIRE(display != nullptr);
	const int screen = DefaultScreen(display);
	const auto embed = [&] {
		uint32_t w = 0, h = 0;
		REQUIRE(f.gui.getSize(&w, &h));
		const ::Window parent = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0, w, h, 0,
		                                            BlackPixel(display, screen), BlackPixel(display, screen));
		XSync(display, False);
		clap_window window {};
		window.api = CLAP_WINDOW_API_X11;
		window.x11 = parent;
		REQUIRE(f.gui.setParent(&window));
		return parent;
	};
	const ::Window first = embed();
	const visage::Frame* firstRoot = f.gui.root();
	// THE CONTAINER FIRST, then guiDestroy, then the reopen.
	XDestroyWindow(display, first);
	XSync(display, False);
	f.gui.destroy();
	REQUIRE(f.gui.create(CLAP_WINDOW_API_X11, false));
	const ::Window second = embed();
	REQUIRE(f.gui.windowRebuilds() == 1);
	visage::Frame* root = f.gui.root();
	REQUIRE(root != nullptr);
	CHECK(root != firstRoot);

	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);
	mod->focus(backplane::Source::core(0));
	mod->arm(true);
	REQUIRE(mod->armed());
	const visage::KeyEvent escape { visage::KeyCode::Escape, 0, true };
	CHECK(root->acceptsKeystrokes());
	CHECK(root->onKeyPress().callback(escape));
	CHECK_FALSE(mod->armed());

	f.gui.destroy();
	XDestroyWindow(display, second);
	XCloseDisplay(display);
}

namespace {

/// THE FRAME HOLDING THE KEYBOARD under `frame`, or null — where `handleKeyDown` starts the next key.
visage::Frame* focusedUnder(visage::Frame* frame) {
	if (frame == nullptr) {
		return nullptr;
	}
	if (frame->hasKeyboardFocus()) {
		return frame;
	}
	for (visage::Frame* child : frame->children()) {
		if (visage::Frame* found = focusedUnder(child); found != nullptr) {
			return found;
		}
	}
	return nullptr;
}


/// THE WALK `WindowEventHandler::handleKeyDown` MAKES: the focus, then up through every ancestor that accepts keys.
bool keyFromFocusUp(visage::Frame* focused, const visage::KeyEvent& e) {
	visage::Frame* frame = focused;
	bool used = false;
	while (!used && frame != nullptr) {
		used = frame->processKeyPress(e);
		frame = frame->parent();
		while (frame != nullptr && !frame->acceptsKeystrokes()) {
			frame = frame->parent();
		}
	}
	return used;
}
} // namespace

TEST_CASE("backlog NN: Esc cancels the preset prompt over an arm, and hands the keyboard back so the NEXT Esc disarms") {
	// The prompt's field holds the keyboard and its Esc cancels; the field then leaves the tree. Unless the focus is
	// handed back, the next Esc starts nowhere and goes to the host, past the arm opened before the prompt. Embedded
	// in a real X window, because the keyboard focus is the window event handler's and a bare editor has none.
	EditorFixture f;
	Display* display = XOpenDisplay(nullptr);
	REQUIRE(display != nullptr);
	const int screen = DefaultScreen(display);
	uint32_t w = 0, h = 0;
	REQUIRE(f.gui.getSize(&w, &h));
	const ::Window parent = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0, w, h, 0,
	                                            BlackPixel(display, screen), BlackPixel(display, screen));
	XSync(display, False);
	clap_window window {};
	window.api = CLAP_WINDOW_API_X11;
	window.x11 = parent;
	REQUIRE(f.gui.setParent(&window));

	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);
	ui::PresetBrowser* browser = f.gui.presetBrowserForTest();
	REQUIRE(browser != nullptr);
	mod->focus(backplane::Source::core(0));
	mod->arm(true);
	REQUIRE(mod->armed());
	browser->beginSave();
	REQUIRE(browser->prompt() != ui::PresetBrowser::Prompt::Closed);
	visage::Frame* const top = f.gui.root()->topParentFrame();
	visage::Frame* field = focusedUnder(top);
	REQUIRE(field != nullptr); // the prompt took the keyboard
	REQUIRE(field == browser->nameFieldForTest());

	const visage::KeyEvent escape { visage::KeyCode::Escape, 0, true };
	CHECK(keyFromFocusUp(field, escape));
	CHECK(browser->prompt() == ui::PresetBrowser::Prompt::Closed);
	CHECK(mod->armed());
	visage::Frame* now = focusedUnder(top);
	REQUIRE(now != nullptr);
	CHECK(keyFromFocusUp(now, escape));
	CHECK_FALSE(mod->armed());

	f.gui.destroy();
	XDestroyWindow(display, parent);
	XCloseDisplay(display);
}
#endif

TEST_CASE("backlog JB: Ctrl+Z pressed in the popout walks the editor's one history, and is refused untaken at its end") {
	// THE POPOUT IS ITS OWN OS WINDOW, so a key pressed there never reaches the editor's root — the chords were
	// the editor's only. The window's root is now the terminus for a key its panel declined, and hands it on.
	EditorFixture f;
	const size_t slot = f.addFx(fx::harm::kTraits.tag);
	ui::WindowedExpanded* window = f.gui.expandedWindowForTest();
	REQUIRE(window != nullptr);
	f.gui.setPopoutWindowForTest(true);
	f.lane->onExpandDevice(slot);
	REQUIRE(window->open());
	visage::Frame* popout = window->root();
	REQUIRE(popout != nullptr);
	REQUIRE(popout != f.root);

	ui::EditHistory& history = *f.gui.history();
	const clap_id id = static_cast<clap_id>(ParamId::VoiceLevel);
	const double before = f.bridge.value(id);
	const double after = before > 0.5 ? before - 0.25 : before + 0.25;
	REQUIRE(ui::writeParamAsEdit(f.bridge, id, after));
	REQUIRE(history.canUndo());

	const auto key = [&](int modifiers) {
		return popout->processKeyPress(visage::KeyEvent { visage::KeyCode::Z, modifiers, true });
	};
	CHECK(key(visage::kModifierRegCtrl));
	CHECK(f.bridge.value(id) == before);
	CHECK(key(visage::kModifierRegCtrl | visage::kModifierShift));
	CHECK(f.bridge.value(id) == after);
	// AT THE TIP THERE IS NOTHING TO REDO, so the chord goes back untaken; a plain Z is not undo.
	CHECK_FALSE(key(visage::kModifierRegCtrl | visage::kModifierShift));
	CHECK_FALSE(key(0));
	window->dismiss();
}

TEST_CASE("backlog NN: the popout window is on the editor's Esc stack — Esc in it closes the newest thing, the window or an arm") {
	// The popout is its own OS window, so its keys never reach the editor's root; its root hands Esc to the editor's
	// one stack, on which the popout is an entry from show to dismissal.
	EditorFixture f;
	const size_t slot = f.addFx(fx::harm::kTraits.tag);
	ui::WindowedExpanded* window = f.gui.expandedWindowForTest();
	REQUIRE(window != nullptr);
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);
	f.gui.setPopoutWindowForTest(true);
	const visage::KeyEvent escape { visage::KeyCode::Escape, 0, true };

	SUBCASE("an arm, then the popout: Esc in the popout closes it; Esc at the editor then disarms") {
		mod->focus(backplane::Source::core(0));
		mod->arm(true);
		f.lane->onExpandDevice(slot);
		REQUIRE(window->open());
		visage::Frame* popout = window->root();
		REQUIRE(popout != nullptr);
		CHECK(popout->processKeyPress(escape));
		CHECK_FALSE(window->open());
		CHECK(mod->armed());
		CHECK(f.root->onKeyPress().callback(escape));
		CHECK_FALSE(mod->armed());
	}

	SUBCASE("the popout, then an arm: Esc at the editor disarms, the popout stays; again, it closes") {
		f.lane->onExpandDevice(slot);
		REQUIRE(window->open());
		mod->focus(backplane::Source::core(0));
		mod->arm(true);
		CHECK(f.root->onKeyPress().callback(escape));
		CHECK_FALSE(mod->armed());
		CHECK(window->open());
		CHECK(f.root->onKeyPress().callback(escape));
		CHECK_FALSE(window->open());
		CHECK_FALSE(f.root->onKeyPress().callback(escape));
	}
	window->dismiss();
	mod->disarmTree();
}

// ---------------------------------------------------------------------------------------------
// BACKLOG NF — THE EXPLORE PANE COLLAPSES, JUST LIKE THE FX LANE. The stage is a `Rail` now
// (`ui::StageRail`): the same toggle, chevrons, strip and upright label as the lane, the same desk memory.
// What it adds is a stage that is HIDDEN while closed, so a closed Explore costs nothing, and a lane that
// takes the room — the flexible column passes from the stage to the lane.
// ---------------------------------------------------------------------------------------------

namespace {

/// Click the header's half for `surface`, the road a hand takes to it.
void showSurfaceByHeader(Gui& gui, ui::StageMode::Surface surface) {
	ui::HeaderBar* header = gui.headerForTest();
	REQUIRE(header != nullptr);
	for (float x = header->width() - 1.0f; x > 0.0f; x -= 2.0f) {
		for (float y = 2.0f; y < header->height() - 2.0f; y += 4.0f) {
			const visage::Point at { x, y };
			if (header->hitsMode(at) && header->surfaceAt(at) == surface) {
				visage::MouseEvent press;
				press.position = at;
				header->onMouseDown().callback(press);
				return;
			}
		}
	}
	FAIL("the header has no half for that surface");
}

/// A press in the middle of the stage's toggle, through the rail's own handler.
void pressStageToggle(ui::StageRail& rail) {
	const visage::Bounds box = rail.toggleBounds();
	visage::MouseEvent press;
	press.position = { box.x() + box.width() * 0.5f, box.y() + box.height() * 0.5f };
	rail.onMouseDown().callback(press);
}

const visage::Frame* terrainGroundOf(const visage::Frame& root) {
	for (const visage::Frame* child : root.children()) {
		if (dynamic_cast<const ui::TerrainGround*>(child) != nullptr) {
			return child;
		}
	}
	return nullptr;
}

} // namespace

TEST_CASE("backlog NF: the Explore pane collapses like the fx lane — a 34px EXPLORE strip, the surface hidden, the "
          "editor narrower by the stage's room and the cards their own width, and the desk remembers it") {
	// Cards are unit size and never stretch; the synth shrinks instead. So the claim is a SIZE: the
	// editor's own width falls by the stage's room (its open width less the strip), which is what the host is told —
	// `getSize` (the open size) and `adjustSize` (the floor a dragging host is held to) — and the lane does not move
	// off its unit width to fill anything.
	{
		EditorFixture f;
		showSurfaceByHeader(f.gui, ui::StageMode::Surface::Explore);
		ui::StageRail* rail = f.gui.stageRailForTest();
		ui::ExploreStage* explore = f.gui.exploreForTest();
		REQUIRE(rail != nullptr);
		REQUIRE(explore != nullptr);
		const visage::Frame* ground = terrainGroundOf(*f.root);
		REQUIRE(ground != nullptr);
		REQUIRE(!rail->collapsed());
		REQUIRE(!f.lane->collapsed());
		REQUIRE(explore->isVisible());
		const double scale = f.gui.contentScaleForTest();
		const visage::Bounds openStage = explore->bounds();
		uint32_t w = 0, h = 0;
		REQUIRE(f.gui.getSize(&w, &h));
		const uint32_t openW = w;
		const uint32_t openH = h;
		REQUIRE(openW == static_cast<uint32_t>(std::ceil(Gui::kLogicalWidth * scale)));

		// OPEN: the toggle sits in the stage's top-right corner, inside the stage — the corner a left rail
		// keeps its toggle in — and the lane is its own width beside the stage.
		CHECK(rail->bounds().right() == doctest::Approx(openStage.right()));
		CHECK(rail->y() == doctest::Approx(openStage.y()));
		CHECK(rail->width() == doctest::Approx(ui::StageRail::kOpenWidth));
		CHECK(rail->height() == doctest::Approx(ui::StageRail::kOpenHeight));
		CHECK(rail->toggleBounds().width() == doctest::Approx(ui::Rail::kToggleSize));
		CHECK(f.lane->width() == doctest::Approx(ui::FxLane::kWidth));
		CHECK(f.lane->x() == doctest::Approx(openStage.right()));

		// CLOSED, BY THE TOGGLE A HAND PRESSES.
		pressStageToggle(*rail);
		REQUIRE(rail->collapsed());
		CHECK(rail->labelForTest() == "EXPLORE");
		CHECK(rail->x() == doctest::Approx(openStage.x()));
		CHECK(rail->y() == doctest::Approx(openStage.y()));
		CHECK(rail->width() == doctest::Approx(ui::Rail::kCollapsedWidth));
		CHECK(rail->height() == doctest::Approx(openStage.height()));
		// THE SIZE THE HOST IS TOLD: the floor `adjustSize` holds a host to is the design width less the stage's room,
		// at the scale in force — so a host can take the editor down to it (a windowless editor has no host to ASK; the
		// windowed case below counts the request itself). The height is not touched.
		const double room = openStage.width() - ui::Rail::kCollapsedWidth;
		const uint32_t closedW = static_cast<uint32_t>(std::ceil((Gui::kLogicalWidth - room) * scale));
		INFO("stage " << openStage.width() << " wide, room " << room << ", closed width " << closedW);
		w = 1;
		h = 1;
		REQUIRE(f.gui.adjustSize(&w, &h));
		CHECK(w == closedW);
		CHECK(h == openH);
		// THE HOST GRANTS IT: the lane is its own width at the window's right edge, straight after the strip — no
		// gap, no stretch.
		REQUIRE(f.gui.setSize(closedW, openH));
		CHECK(f.lane->width() == doctest::Approx(ui::FxLane::kWidth));
		CHECK(f.lane->x() == doctest::Approx(rail->bounds().right()).epsilon(0.002));
		CHECK(f.lane->bounds().right() == doctest::Approx(f.root->width()));
		CHECK(rail->x() == doctest::Approx(openStage.x()));
		// AND THE SURFACE AND ITS GROUND ARE GONE, which is what makes a closed Explore cost nothing.
		CHECK(!explore->isVisible());
		CHECK(!ground->isVisible());
		// A tick does not bring them back (the surface reconciler knows about the collapse).
		(void)f.gui.tickForTest();
		CHECK(!explore->isVisible());
		CHECK(!ground->isVisible());
		{
			feathers::EditorPrefs desk;
			desk.load();
			CHECK(desk.flag("rail.explore.collapsed", false));
		}
		// THE STRIP NAMES THE SURFACE IT HOLDS: PERFORM once the switch says so, and Perform stays hidden too.
		showSurfaceByHeader(f.gui, ui::StageMode::Surface::Perform);
		(void)f.gui.tickForTest();
		CHECK(rail->labelForTest() == "PERFORM");
		CHECK(rail->collapsed());
		for (const visage::Frame* child : f.root->children()) {
			if (dynamic_cast<const ui::PerformStage*>(child) != nullptr) {
				CHECK(!child->isVisible());
			}
		}
		CHECK(!explore->isVisible());
		showSurfaceByHeader(f.gui, ui::StageMode::Surface::Explore);
		(void)f.gui.tickForTest();
		CHECK(rail->labelForTest() == "EXPLORE");
		CHECK(!explore->isVisible());

		// OPEN AGAIN: the design width is what the host is told, and granted, the stage is the rectangle it had.
		pressStageToggle(*rail);
		REQUIRE(!rail->collapsed());
		w = 1;
		h = 1;
		REQUIRE(f.gui.adjustSize(&w, &h)); // the floor: the width the editor asks to be
		CHECK(w == openW);
		REQUIRE(f.gui.setSize(openW, openH));
		CHECK(explore->isVisible());
		CHECK(ground->isVisible());
		CHECK(explore->width() == doctest::Approx(openStage.width()));
		CHECK(f.lane->width() == doctest::Approx(ui::FxLane::kWidth));
		pressStageToggle(*rail); // closed again, for the desk below
	}
	// THE DESK: the next editor comes up closed, and the size it asks the host for is the closed one.
	{
		EditorFixture f;
		showSurfaceByHeader(f.gui, ui::StageMode::Surface::Explore);
		ui::StageRail* rail = f.gui.stageRailForTest();
		REQUIRE(rail != nullptr);
		CHECK(rail->collapsed());
		CHECK(!f.gui.exploreForTest()->isVisible());
		uint32_t w = 0, h = 0;
		w = 1;
		h = 1;
		REQUIRE(f.gui.adjustSize(&w, &h)); // the floor: the width the editor asks to be
		CHECK(w < static_cast<uint32_t>(std::ceil(Gui::kLogicalWidth * f.gui.contentScaleForTest())));
		pressStageToggle(*rail);
		feathers::EditorPrefs desk;
		desk.load();
		CHECK(!desk.flag("rail.explore.collapsed", true));
	}
}

TEST_CASE("backlog NF: with the stage closed a rail's toggle moves the editor's width, not anybody else's — closing "
          "the lane too makes the editor narrower still, and nothing reopens on its own") {
	EditorFixture f;
	showSurfaceByHeader(f.gui, ui::StageMode::Surface::Explore);
	ui::StageRail* rail = f.gui.stageRailForTest();
	REQUIRE(rail != nullptr);
	const double scale = f.gui.contentScaleForTest();
	pressStageToggle(*rail);
	REQUIRE(rail->collapsed());
	uint32_t w = 0, h = 0;
	w = 1;
	h = 1;
	REQUIRE(f.gui.adjustSize(&w, &h)); // the floor: the width the editor asks to be
	const uint32_t closedW = w;
	const visage::Bounds laneToggle = f.lane->toggleBounds();
	REQUIRE(f.lane->pressCollapse(
	    { laneToggle.x() + laneToggle.width() * 0.5f, laneToggle.y() + laneToggle.height() * 0.5f }));
	CHECK(f.lane->collapsed());
	CHECK(rail->collapsed());
	w = 1;
	h = 1;
	REQUIRE(f.gui.adjustSize(&w, &h)); // the floor: the width the editor asks to be
	const double laneRoom = ui::FxLane::kWidth - ui::Rail::kCollapsedWidth;
	CHECK(std::abs(static_cast<double>(w) - (static_cast<double>(closedW) - laneRoom * scale)) <= 1.0);
	// GRANTED, the two strips sit side by side at the window's right edge.
	REQUIRE(f.gui.setSize(w, h));
	CHECK(f.lane->width() == doctest::Approx(ui::Rail::kCollapsedWidth));
	CHECK(f.lane->x() == doctest::Approx(rail->bounds().right()).epsilon(0.002));
	CHECK(f.lane->bounds().right() == doctest::Approx(f.root->width()));
}

TEST_CASE("backlog NF: a stage opened after a UI-scale change while it was closed is laid out at the NEW scale — "
          "its native width is its logical width at the scale in force") {
	clearUiScalePref();
	EditorFixture f;
	showSurfaceByHeader(f.gui, ui::StageMode::Surface::Explore);
	ui::StageRail* rail = f.gui.stageRailForTest();
	ui::ExploreStage* explore = f.gui.exploreForTest();
	REQUIRE(rail != nullptr);
	REQUIRE(explore != nullptr);
	pressStageToggle(*rail);
	REQUIRE(rail->collapsed());
	const int nativeBefore = explore->nativeWidth();

	// THE SCALE MOVES WHILE THE STAGE IS CLOSED — the header's plus, the road a hand takes.
	ui::HeaderBar* header = f.gui.headerForTest();
	REQUIRE(header != nullptr);
	const visage::Bounds plus = header->uiScalePlusBounds();
	visage::MouseEvent press;
	press.position = { plus.x() + plus.width() * 0.5f, plus.y() + plus.height() * 0.5f };
	header->onMouseDown().callback(press);
	REQUIRE(f.gui.uiScaleForTest() == doctest::Approx(1.125));
	const double scale = f.gui.contentScaleForTest();
	REQUIRE(explore->dpiScale() == doctest::Approx(scale));
	// AND THE HOST GRANTS THE CLOSED SIZE AT THE NEW SCALE, as a DAW does.
	uint32_t grantW = 0, grantH = 0;
	REQUIRE(f.gui.getSize(&grantW, &grantH));
	REQUIRE(f.gui.setSize(grantW, grantH));

	pressStageToggle(*rail);
	REQUIRE(!rail->collapsed());
	REQUIRE(explore->isVisible());
	const visage::Frame* ground = terrainGroundOf(*f.root);
	REQUIRE(ground != nullptr);
	// THE NATIVE RECTANGLE IS THE LOGICAL ONE AT THE SCALE IN FORCE, for the surface and the ground under it.
	const auto nativeOf = [scale](const visage::Frame& frame) {
		return static_cast<int>(std::round(frame.right() * scale)) - static_cast<int>(std::round(frame.x() * scale));
	};
	INFO("logical " << explore->width() << " at scale " << scale << ", native before " << nativeBefore);
	CHECK(explore->width() > 0.0f);
	CHECK(std::abs(explore->nativeWidth() - nativeOf(*explore)) <= 1);
	CHECK(std::abs(ground->nativeWidth() - nativeOf(*ground)) <= 1);
	CHECK(std::abs(explore->nativeHeight()
	               - (static_cast<int>(std::round(explore->bottom() * scale))
	                  - static_cast<int>(std::round(explore->y() * scale))))
	      <= 1);
	clearUiScalePref();
}

#if defined(__linux__)
TEST_CASE("backlog NF: a closed Explore costs nothing in a real window — no stage paint, no ground paint, no terrain "
          "request or build while closed, however the selection moves; opening it paints and builds again") {
	// ON THE PRIVATE DISPLAY (ctest's gui-windowed venue). A repaint is not observable headlessly, so the claim
	// is counted where the paints happen. FEATHERS_NF_PHOTO=<dir> also keeps the photographs: the fx lane's own
	// collapse (the spec), the stage open, closed, and open again.
	const char* keep = std::getenv("FEATHERS_NF_PHOTO");
	EditorFixture f;
	Display* display = XOpenDisplay(nullptr);
	REQUIRE(display != nullptr);
	const int screen = DefaultScreen(display);
	uint32_t w0 = 0, h0 = 0;
	REQUIRE(f.gui.getSize(&w0, &h0));
	::Window parent = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0, w0, h0, 0,
	                                      BlackPixel(display, screen), BlackPixel(display, screen));
	XFlush(display);
	clap_window window {};
	window.api = CLAP_WINDOW_API_X11;
	window.x11 = parent;
	REQUIRE(f.gui.setParent(&window));
	XMapWindow(display, parent);
	XRaiseWindow(display, parent);
	XFlush(display);
	// **FRAMES, COUNTED; AND WAITS, ENDED BY WHAT THEY WAIT FOR** (backlog PL, 2026-10-30). This pumped fixed
	// "milliseconds" that were really 1,825 slices of `processEvents`, each a full lavapipe frame: 52 s at load 39,
	// and past run-cases' 120 s per-case timeout inside the integrator's `ctest -j6` at load 40-49 — which is the
	// gui-windowed red of batch 51 (239.75 s: one case killed at 120 s). Every wait that was for something to
	// HAPPEN now pumps until it has (`pumpUntil`, whose cap is hang protection and is followed by a REQUIRE on the
	// condition), and the one interval that asserts NOTHING happens is a count of frames, which no load can change.
	const auto pumpFrames = [&](int frames) {
		for (int slice = 0; slice < frames; ++slice) {
			f.gui.processEvents();
			::usleep(4000);
		}
	};
	const auto pumpUntil = [&](const auto& condition) {
		for (int slice = 0; slice < 5000 && !condition(); ++slice) {
			f.gui.processEvents();
			::usleep(4000);
		}
		return condition();
	};
	// THE TERRAIN AT REST: the map in hand is whole and the builder has not started another across a few frames —
	// so a count taken after this is not racing a build the PREVIOUS state asked for.
	const auto terrainQuiet = [&](const ui::TerrainMap& map) {
		if (map.snapshot()->rows < ui::TerrainMap::kResolution) {
			return false;
		}
		const uint64_t was = map.builds();
		pumpFrames(10);
		return map.builds() == was && map.snapshot()->rows >= ui::TerrainMap::kResolution;
	};
	const auto photo = [&](const char* name) {
		if (keep != nullptr) {
			REQUIRE(capturePpm(parent, std::string(keep) + "/" + name + ".ppm").valid());
		}
	};
	showSurfaceByHeader(f.gui, ui::StageMode::Surface::Explore);
	ui::StageRail* rail = f.gui.stageRailForTest();
	ui::ExploreStage* explore = f.gui.exploreForTest();
	REQUIRE(rail != nullptr);
	REQUIRE(explore != nullptr);
	const ui::TerrainMap& terrain = explore->terrainForTest();
	REQUIRE(pumpUntil([&] { return explore->stagePaintsForTest() > 0 && explore->groundPaintsForTest() > 0; }));

	// THE SPEC, PHOTOGRAPHED: the fx lane open, then closed by its own toggle, then open again.
	photo("nf-0-lane-open");
	REQUIRE(f.lane->setCollapsed(true));
	pumpFrames(25);
	photo("nf-1-lane-closed");
	const int paintsLaneClosed = explore->stagePaintsForTest();
	REQUIRE(f.lane->setCollapsed(false));
	REQUIRE(pumpUntil([&] { return explore->stagePaintsForTest() > paintsLaneClosed; }));
	photo("nf-2-stage-open");
	// NOT VACUOUS: an open stage in this venue paints.
	const int paintsOpen = explore->stagePaintsForTest();
	REQUIRE(paintsOpen > 0);
	REQUIRE(explore->groundPaintsForTest() > 0);

	// THE SYNTH JUST SHRINKS: closing asks the host for the window's width less the stage's room, height untouched.
	const double roomPx = (explore->width() - ui::Rail::kCollapsedWidth) * f.gui.contentScaleForTest();
	const int resizesBefore = f.host.resizes;
	pressStageToggle(*rail);
	REQUIRE(rail->collapsed());
	REQUIRE(f.host.resizes == resizesBefore + 1);
	INFO("asked for " << f.host.lastResizeWidth << "x" << f.host.lastResizeHeight << " from " << w0 << "x" << h0
	                  << ", room " << roomPx);
	CHECK(std::abs(static_cast<double>(f.host.lastResizeWidth) - (static_cast<double>(w0) - roomPx)) <= 1.0);
	CHECK(f.host.lastResizeHeight == h0);
	const uint32_t closedW = f.host.lastResizeWidth;
	// GRANTED, as a host does: the lane keeps its unit width at the right edge.
	XResizeWindow(display, parent, closedW, h0);
	REQUIRE(f.gui.setSize(closedW, h0));
	CHECK(f.lane->width() == doctest::Approx(ui::FxLane::kWidth));
	pumpFrames(25);
	REQUIRE(pumpUntil([&] { return terrainQuiet(terrain); }));
	photo("nf-3-stage-closed");
	const int stagePaints = explore->stagePaintsForTest();
	const int groundPaints = explore->groundPaintsForTest();
	const uint64_t requests = terrain.requests();
	const uint64_t builds = terrain.builds();
	// WHAT WOULD MAKE AN OPEN STAGE WORK: a new selection is a new chart (`ExploreStage::draw` re-centres on it).
	ui::SoundSelection* selection = f.gui.selectionForTest();
	REQUIRE(selection != nullptr);
	const size_t before = selection->slot();
	selection->select(before == 0 ? 1 : 0);
	REQUIRE(selection->slot() != before);
	f.root->redrawAll();
	// A HUNDRED FRAMES PROCESSED WHILE CLOSED, after a new selection and a redraw of everything: a count of frames,
	// so "nothing painted" means the same at any load (a closed stage that painted would do it on the first).
	pumpFrames(100);
	INFO("closed: stage paints " << explore->stagePaintsForTest() - stagePaints << ", ground paints "
	                             << explore->groundPaintsForTest() - groundPaints << ", requests "
	                             << terrain.requests() - requests << ", builds " << terrain.builds() - builds);
	CHECK(explore->stagePaintsForTest() == stagePaints);
	CHECK(explore->groundPaintsForTest() == groundPaints);
	CHECK(terrain.requests() == requests);
	CHECK(terrain.builds() == builds);

	// OPEN AGAIN: it paints, and it asks for the new selection's map — so the silence above was the collapse.
	pressStageToggle(*rail);
	REQUIRE(!rail->collapsed());
	// AND OPENING ASKS FOR THE ROOM BACK: the width it had.
	REQUIRE(f.host.resizes == resizesBefore + 2);
	CHECK(std::abs(static_cast<int>(f.host.lastResizeWidth) - static_cast<int>(w0)) <= 1);
	XResizeWindow(display, parent, f.host.lastResizeWidth, h0);
	REQUIRE(f.gui.setSize(f.host.lastResizeWidth, h0));
	// UNTIL IT HAS PAINTED, ASKED AND BUILT AGAIN — a wait for the claim itself, so its REQUIRE is the verdict.
	REQUIRE(pumpUntil([&] {
		return explore->stagePaintsForTest() > stagePaints && explore->groundPaintsForTest() > groundPaints
		       && terrain.requests() > requests && terrain.builds() > builds;
	}));
	photo("nf-4-stage-reopened");
	CHECK(explore->stagePaintsForTest() > stagePaints);
	CHECK(explore->groundPaintsForTest() > groundPaints);
	CHECK(terrain.requests() > requests);
	CHECK(terrain.builds() > builds);
	XDestroyWindow(display, parent);
	XCloseDisplay(display);
}
#endif // defined(__linux__)

#if defined(__linux__) // the case drives the X11 window and reads XOpenDisplay — Linux only, like the cases above
TEST_CASE("backlog MF: a text field that leaves the pointer gives the cursor back — removed, destroyed, or scrolled away under the wheel — through the window's own event road") {
	// THE BUG: the text-selection cursor often appeared while using the plugin with no text being
	// selected. visage's TextEditor sets the I-beam on enter and the arrow on exit, and on X11 the cursor is
	// one setting on the whole window. Two roads in `WindowEventHandler` forgot a hovered frame WITHOUT an exit: a
	// frame leaving the tree (`giveUpFocus`) and the wheel's re-hover. The preset prompt was patched in our layer;
	// these are the general fixes in the fork, so ANY TextEditor or Button is covered.
	//
	// DRIVEN THROUGH THE WINDOW'S OWN HANDLER, not a restatement of it: the case reaches the editor's real
	// `WindowEventHandler` and calls the same entries the X11 loop calls. The cursor is read by value from the
	// frame handler's `set_cursor_style`, which every `Frame::setCursorStyle` goes through.
	EditorFixture f;
	Display* display = XOpenDisplay(nullptr);
	REQUIRE(display != nullptr);
	const int screen = DefaultScreen(display);
	uint32_t w0 = 0, h0 = 0;
	REQUIRE(f.gui.getSize(&w0, &h0));
	::Window parent = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0, w0, h0, 0,
	                                      BlackPixel(display, screen), BlackPixel(display, screen));
	XFlush(display);
	clap_window window {};
	window.api = CLAP_WINDOW_API_X11;
	window.x11 = parent;
	REQUIRE(f.gui.setParent(&window));
	auto* editor = dynamic_cast<visage::ApplicationEditor*>(f.root);
	REQUIRE(editor != nullptr);
	REQUIRE(editor->window() != nullptr);
	auto* handler = dynamic_cast<visage::WindowEventHandler*>(editor->window()->eventHandler());
	REQUIRE(handler != nullptr);

	// THE CURSOR, RECORDED. Restored on the way out so the fixture's teardown uses the real one.
	visage::FrameEventHandler* frameHandler = f.root->eventHandler();
	REQUIRE(frameHandler != nullptr);
	const auto realSetCursor = frameHandler->set_cursor_style;
	visage::MouseCursor last = visage::MouseCursor::Arrow;
	int sets = 0;
	frameHandler->set_cursor_style = [&](visage::MouseCursor c) {
		last = c;
		++sets;
	};
	struct Restore {
		visage::FrameEventHandler* h;
		std::function<void(visage::MouseCursor)> real;
		~Restore() { h->set_cursor_style = real; }
	} restore { frameHandler, realSetCursor };

	// A FIELD ON TOP OF EVERYTHING, at a place the pointer can be put.
	const visage::Bounds at { 40.0f, 40.0f, 160.0f, 24.0f };
	const visage::Point inside { at.x() + 20.0f, at.y() + 12.0f };
	const visage::IPoint native = handler->convertToNative(inside);
	const auto hover = [&](visage::TextEditor& field) {
		// Off the field first, so the move onto it is a hover CHANGE whatever was hovered before.
		handler->handleMouseMove(1, 1, 0, 0);
		handler->handleMouseMove(native.x, native.y, 0, 0);
		REQUIRE(f.root->frameAtPoint(inside) == &field);
		REQUIRE(last == visage::MouseCursor::IBeam); // not vacuous: the field DID take the cursor
	};
	const auto placed = [&](visage::TextEditor& field) {
		field.setOnTop(true);
		f.root->addChild(&field);
		field.setBounds(at);
	};

	SUBCASE("removed from the tree under a still pointer") {
		visage::TextEditor field("mf-probe");
		placed(field);
		hover(field);
		f.root->removeChild(&field);
		CHECK(last == visage::MouseCursor::Arrow);
	}
	SUBCASE("destroyed under a still pointer") {
		auto field = std::make_unique<visage::TextEditor>("mf-probe");
		placed(*field);
		hover(*field);
		field.reset();
		CHECK(last == visage::MouseCursor::Arrow);
	}
	SUBCASE("moved out from under the pointer, and the wheel is the next event") {
		visage::TextEditor field("mf-probe");
		placed(field);
		hover(field);
		field.setBounds(at.x(), at.y() + 300.0f, at.width(), at.height());
		REQUIRE(f.root->frameAtPoint(inside) != &field);
		handler->handleMouseWheel(0.0f, -1.0f, 0.0f, -1.0f, native.x, native.y, 0, 0, false);
		CHECK(last == visage::MouseCursor::Arrow);
		// AND THE WHEEL'S NEW HOVER IS A REAL ONE: moving back onto the field is an enter again.
		field.setBounds(at);
		handler->handleMouseWheel(0.0f, -1.0f, 0.0f, -1.0f, native.x, native.y, 0, 0, false);
		CHECK(last == visage::MouseCursor::IBeam);
		f.root->removeChild(&field);
		CHECK(last == visage::MouseCursor::Arrow);
	}
	CHECK(sets > 0);
	XDestroyWindow(display, parent);
	XCloseDisplay(display);
}
#endif // defined(__linux__)
