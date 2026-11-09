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

// DOES ALT REACH THE KNOB THROUGH A REAL X11 WINDOW — the one question the widget suite cannot ask.
//
//   scripts/venue-xvfb.sh run -- build/alt-probe        # exit 0 iff both roads are taken correctly
//
// BACKLOG CU.
// Every venue we own that reaches the widget tree hands the knob a MouseEvent it built itself, with
// `modifiers = kModifierAlt` written in by the test — so the four `isAltDown()` gates are proven
// against a bit the test set, never against a bit the X server set. The road between the two is
// visage's `WindowX11::modifierState()`: an `XQueryPointer` at press time, tested against `Mod1Mask`.
// Whether THAT sees a held Alt on a real display is what "we blamed Bitwig" is a guess about, and
// this is the instrument that turns the guess into a measurement.
//
// HOW. The shipped editor is assembled in-process the way windowed-probe does it, embedded in an X11
// parent the way a host embeds it, and a source is armed through `Gui::modForTest()`. Then the X
// server itself is asked to press Alt_L, move the pointer onto a destination knob's face, press
// button 1, drag, release, and lift Alt — through XTEST, which feeds the server's input pipeline so
// the modifier state `XQueryPointer` reports is the server's own, not a bit this process wrote. The
// same drag is then made without Alt. The verdict is read from the BRIDGE, not from a picture:
//
//   Alt held    -> the parameter's VALUE moved and the route cell did not   (the value road)
//   Alt lifted  -> the route cell moved and the value did not               (the route road)
//
// BOTH HALVES OR NOTHING. A build where Alt never arrives takes the route road twice, and a build
// where the gate is inverted takes the value road twice; each half alone is green on one of those.
//
// WHAT IT CANNOT SAY. This is not Bitwig: the parent window here belongs to this process, and the
// keyboard is XTEST. It measures the plugin's own road from the server to `Knob::mappable(altDown)`,
// which is the road Bitwig's plugin host also stands on — but a host that grabs Alt+Button1 before
// the child window sees it is a host-side fact this cannot see. What it can settle is whether the
// plugin does its part, which is the half that has been blamed without being measured.
//
// XTEST IS BANNED ON `:1` — that is the desktop display and the pointer belongs to whoever is at it. Run this under
// venue-xvfb.sh, whose Xvfb has the same modifier map (Alt_L, Alt_R on mod1 — MEASURED 2026-09-06)
// and the XTEST extension.

#include "plugin/gui.h"
#include "plugin/gui_bridge.h"
#include "plugin/params.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/mod_state.h"
#include "plugin/ui/widget_registry.h"

#include <visage/ui.h>

#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <unistd.h>

using namespace feathers;

namespace {

struct ProbeGuiHost : GuiHost {
	uint32_t requestedWidth = 0;
	uint32_t requestedHeight = 0;
	bool resizePending = false;

	void guiRequestFlush() noexcept override {}
	void guiRequestResize(uint32_t width, uint32_t height) noexcept override {
		requestedWidth = width;
		requestedHeight = height;
		resizePending = true;
	}
	void guiCaptureState(clap_ostream_t*) noexcept override {}
	bool guiLoadState(clap_istream_t*) noexcept override { return false; }
};

/// One drag on the knob's face, driven through the server. `alt` decides whether Alt_L is held
/// across the whole gesture, which is the state a hand is in when it uses the bypass — the press
/// is where `Knob` reads the modifier, but a hand does not lift Alt between press and drag either.
struct Drive {
	Display* display;
	int screen;
	KeyCode altKey;
	int rootX;
	int rootY;
	int travel;
	void (*pump)(int);
};

} // namespace

int main() {
	XInitThreads();
	Display* display = XOpenDisplay(nullptr);
	if (display == nullptr) {
		std::printf("XOpenDisplay failed — is DISPLAY set?\n");
		return 2;
	}
	int xtestEvent = 0;
	int xtestError = 0;
	int xtestMajor = 0;
	int xtestMinor = 0;
	if (!XTestQueryExtension(display, &xtestEvent, &xtestError, &xtestMajor, &xtestMinor)) {
		std::printf("alt-probe: NO XTEST on this display — nothing can be driven\n");
		return 2;
	}
	const KeyCode altKey = XKeysymToKeycode(display, XK_Alt_L);
	if (altKey == 0) {
		std::printf("alt-probe: Alt_L has no keycode on this display — nothing can be driven\n");
		return 2;
	}

	ProbeGuiHost host;
	ParamBridge bridge;
	ParamSet params;
	bridge.publish(params);
	bridge.refresh();

	Gui gui(host, bridge);
	if (!Gui::isAvailable() || !gui.create(CLAP_WINDOW_API_X11, false)) {
		std::printf("gui.create failed — no X11 backend in this build\n");
		return 2;
	}
	uint32_t width = Gui::kLogicalWidth;
	uint32_t height = Gui::kLogicalHeight;
	gui.setSize(width, height);

	const int screen = DefaultScreen(display);
	Window parent = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0, width, height, 0,
	                                    BlackPixel(display, screen), BlackPixel(display, screen));
	XStoreName(display, parent, "feathers alt-probe");
	XFlush(display);

	clap_window window {};
	window.api = CLAP_WINDOW_API_X11;
	window.x11 = parent;
	if (!gui.setParent(&window)) {
		std::printf("gui.setParent failed\n");
		return 2;
	}
	XMapWindow(display, parent);
	XRaiseWindow(display, parent);
	XSetInputFocus(display, parent, RevertToParent, CurrentTime);
	XFlush(display);

	// THE PARAMETER LOOP, CLOSED — the knob writes into the bridge's queue and reads the engine's
	// echo back, so a probe that never drained would show a value that never moved whatever the
	// knob did. Same loop windowed-probe runs, minus its storm.
	const auto pump = [&](int milliseconds) {
		for (int slice = 0; slice < std::max(1, milliseconds / 4); ++slice) {
			gui.processEvents();
			bool moved = false;
			bridge.drain([&](const UiParamEvent& event) {
				if (event.kind == UiParamEvent::Kind::Value) {
					params.setBase(event.id, event.value);
					moved = true;
				}
			});
			if (moved) {
				bridge.publish(params);
				bridge.refresh();
			}
			while (XPending(display) > 0) {
				XEvent event;
				XNextEvent(display, &event);
			}
			usleep(4000);
		}
	};

	// Settle the size the way windowed-probe does: read back what the server actually granted.
	pump(400);
	for (int round = 0; round < 4 && host.resizePending; ++round) {
		host.resizePending = false;
		const uint32_t askedWidth = std::min<uint32_t>(host.requestedWidth,
		                                              static_cast<uint32_t>(DisplayWidth(display, screen)));
		const uint32_t askedHeight = std::min<uint32_t>(host.requestedHeight,
		                                               static_cast<uint32_t>(DisplayHeight(display, screen)));
		XResizeWindow(display, parent, askedWidth, askedHeight);
		XSync(display, False);
		Window ignoredRoot = 0;
		int ignoredX = 0;
		int ignoredY = 0;
		unsigned int actualWidth = askedWidth;
		unsigned int actualHeight = askedHeight;
		unsigned int ignoredBorder = 0;
		unsigned int ignoredDepth = 0;
		XGetGeometry(display, parent, &ignoredRoot, &ignoredX, &ignoredY, &actualWidth, &actualHeight,
		             &ignoredBorder, &ignoredDepth);
		width = actualWidth;
		height = actualHeight;
		gui.setSize(width, height);
		pump(400);
	}
	XRaiseWindow(display, parent);
	XSetInputFocus(display, parent, RevertToParent, CurrentTime);
	XFlush(display);
	pump(1200);

	visage::Frame* root = gui.root();
	const ui::WidgetRegistry* widgets = gui.widgets();
	ui::ModState* mod = gui.modForTest();
	if (root == nullptr || widgets == nullptr || mod == nullptr) {
		std::printf("no assembled tree — gui.root()/widgets()/modForTest() are null\n");
		return 2;
	}

	// A DESTINATION KNOB, FOUND RATHER THAN ASSUMED: the first core destination the registry has a
	// visible Knob for. Printed, so a run that drove the wrong control says which one it drove.
	ui::Knob* knob = nullptr;
	clap_id targetId = 0;
	for (const ParamId id : kCoreModDestinations) {
		for (visage::Frame* frame : widgets->watchers(static_cast<clap_id>(id))) {
			auto* candidate = dynamic_cast<ui::Knob*>(frame);
			if (candidate != nullptr && candidate->isVisible() && candidate->isModDestination() &&
			    candidate->enabled()) {
				knob = candidate;
				targetId = static_cast<clap_id>(id);
				break;
			}
		}
		if (knob != nullptr) {
			break;
		}
	}
	if (knob == nullptr) {
		std::printf("alt-probe: NO VISIBLE DESTINATION KNOB in the tree — nothing was driven\n");
		return 2;
	}
	const ParamDesc* desc = findParam(targetId);
	const int dest = modDestinationIndexOf(static_cast<ParamId>(targetId));
	if (desc == nullptr || dest < 0) {
		std::printf("alt-probe: knob's parameter %u is not a destination — nothing was driven\n", targetId);
		return 2;
	}

	// ARMED, AND SHOWN TO BE: `mappable()` with no modifier is the gate a plain press takes, and a
	// run where it reads false has armed nothing and would report "Alt bypassed" for a knob that had
	// nothing to bypass. The first source the gate accepts is the one used.
	size_t source = kMacroCount + kWeightSourceCount;
	for (size_t s = 0; s < kMacroCount + kWeightSourceCount; ++s) {
		mod->focus(backplane::Source::core(s));
		mod->arm(true);
		pump(100);
		if (knob->mappable()) {
			source = s;
			break;
		}
		mod->arm(false);
	}
	if (source >= kMacroCount + kWeightSourceCount) {
		std::printf("alt-probe: NO SOURCE MAKES %s mappable — nothing was driven\n", desc->name);
		return 2;
	}
	std::printf("armed:    source %zu -> %s (dest %d); mappable() = %d, mappable(alt) = %d\n", source,
	            desc->name, dest, knob->mappable() ? 1 : 0, knob->mappable(true) ? 1 : 0);

	// WHERE THE FACE IS, IN THE SERVER'S PIXELS. The tree is logical and the window is scaled, and
	// the parent may not sit at the root's origin, so the point is translated twice: logical to
	// pixel by the root frame's own dpi scale, and window to root by the server.
	const float scale = root->dpiScale();
	const visage::Point face = knob->positionInWindow()
	                           + visage::Point { knob->width() * 0.5f, knob->faceDiameter() * 0.5f };
	int parentRootX = 0;
	int parentRootY = 0;
	Window ignoredChild = 0;
	XTranslateCoordinates(display, parent, RootWindow(display, screen), 0, 0, &parentRootX, &parentRootY,
	                      &ignoredChild);
	const int faceX = parentRootX + static_cast<int>(std::lround(face.x * scale));
	const int faceY = parentRootY + static_cast<int>(std::lround(face.y * scale));
	const int travel = static_cast<int>(std::lround(60.0f * scale));
	std::printf("face:     logical (%.0f,%.0f) scale %.3f -> root (%d,%d); drag %dpx up\n", face.x, face.y,
	            static_cast<double>(scale), faceX, faceY, travel);

	// THE GESTURE, THROUGH THE SERVER. Motion first, and a pump between every step, because visage
	// reads the modifier by QUERYING the server at the moment it handles the press — an Alt that
	// arrives after the press is handled is an Alt the press never saw, whatever order it was sent.
	const auto drive = [&](bool alt) {
		if (alt) {
			XTestFakeKeyEvent(display, altKey, True, 0);
			XFlush(display);
			pump(60);
		}
		XTestFakeMotionEvent(display, screen, faceX, faceY, 0);
		XFlush(display);
		pump(60);
		XTestFakeButtonEvent(display, 1, True, 0);
		XFlush(display);
		pump(60);
		for (int step = 1; step <= 6; ++step) {
			XTestFakeMotionEvent(display, screen, faceX, faceY - travel * step / 6, 0);
			XFlush(display);
			pump(40);
		}
		XTestFakeButtonEvent(display, 1, False, 0);
		XFlush(display);
		pump(60);
		if (alt) {
			XTestFakeKeyEvent(display, altKey, False, 0);
			XFlush(display);
			pump(60);
		}
		// Park the pointer off the knob so the next gesture's press is not a double-click on it.
		XTestFakeMotionEvent(display, screen, parentRootX + 2, parentRootY + 2, 0);
		XFlush(display);
		pump(500);
	};

	int failures = 0;

	// THE POLLED ROAD, HOVER-FREE: Alt goes down with the pointer PARKED OFF EVERY KNOB and no mouse
	// event of any kind, and the next ticks must put the halo out on their own — this is the line
	// The report is about the glow redraw seeming to be gated on hovering over a knob. The
	// motion road cannot pass it, because nothing moves.
	{
		XTestFakeMotionEvent(display, screen, parentRootX + 2, parentRootY + 2, 0);
		XFlush(display);
		pump(300);
		XTestFakeKeyEvent(display, altKey, True, 0);
		XFlush(display);
		pump(120);
		const bool bypassHeld = mod->bypassed();
		const bool haloHeld = knob->haloLit();
		XTestFakeKeyEvent(display, altKey, False, 0);
		XFlush(display);
		pump(120);
		const bool bypassLifted = mod->bypassed();
		const bool haloLifted = knob->haloLit();
		const bool ok = bypassHeld && !haloHeld && !bypassLifted && haloLifted;
		std::printf("parked:   alt held: bypassed %d halo %d; alt lifted: bypassed %d halo %d -> %s\n",
		            bypassHeld ? 1 : 0, haloHeld ? 1 : 0, bypassLifted ? 1 : 0, haloLifted ? 1 : 0,
		            ok ? "ok" : "WRONG");
		if (!ok) {
			++failures;
		}
	}

	// WHAT THE POLL COSTS. The tick's read is one XQueryPointer round trip on visage's own connection;
	// this times the same request on this process's connection to the same server, which is the
	// number that matters and the one nobody should estimate. Printed, not asserted.
	{
		constexpr int kSamples = 1000;
		::Window rootRet = 0;
		::Window childRet = 0;
		int rx = 0;
		int ry = 0;
		int wx = 0;
		int wy = 0;
		unsigned int mask = 0;
		struct timespec t0 {};
		struct timespec t1 {};
		clock_gettime(CLOCK_MONOTONIC, &t0);
		for (int i = 0; i < kSamples; ++i) {
			XQueryPointer(display, parent, &rootRet, &childRet, &rx, &ry, &wx, &wy, &mask);
		}
		clock_gettime(CLOCK_MONOTONIC, &t1);
		const double us = (static_cast<double>(t1.tv_sec - t0.tv_sec) * 1e6 +
		                   static_cast<double>(t1.tv_nsec - t0.tv_nsec) / 1e3) /
		                  kSamples;
		std::printf("cost:     XQueryPointer %.1f us per call over %d calls (the tick's poll is one)\n", us,
		            kSamples);
	}

	// THE GLOW'S HALF, THROUGH THE SERVER. The halo reads `ModState::bypassed`, which only the hand's
	// motion feeds — so Alt is held, the pointer is moved onto the face with no press, and the flag
	// and the predicate the draw path asks are read. Then Alt is lifted and the pointer nudged, and
	// both must come back. A knob that glowed while Alt was held is the honesty-rule half of CU.
	{
		XTestFakeKeyEvent(display, altKey, True, 0);
		XFlush(display);
		pump(60);
		XTestFakeMotionEvent(display, screen, faceX, faceY, 0);
		XFlush(display);
		pump(120);
		const bool bypassHeld = mod->bypassed();
		const bool haloHeld = knob->haloLit();
		XTestFakeKeyEvent(display, altKey, False, 0);
		XFlush(display);
		pump(60);
		XTestFakeMotionEvent(display, screen, faceX + 1, faceY, 0);
		XFlush(display);
		pump(120);
		const bool bypassLifted = mod->bypassed();
		const bool haloLifted = knob->haloLit();
		const bool ok = bypassHeld && !haloHeld && !bypassLifted && haloLifted;
		std::printf("hover:    alt held: bypassed %d halo %d; alt lifted: bypassed %d halo %d -> %s\n",
		            bypassHeld ? 1 : 0, haloHeld ? 1 : 0, bypassLifted ? 1 : 0, haloLifted ? 1 : 0,
		            ok ? "ok" : "WRONG");
		if (!ok) {
			++failures;
		}
		XTestFakeMotionEvent(display, screen, parentRootX + 2, parentRootY + 2, 0);
		XFlush(display);
		pump(300);
	}

	const auto report = [&](const char* which, bool valueMoved, bool routeMoved, bool wantValue) {
		const bool ok = valueMoved == wantValue && routeMoved == !wantValue;
		std::printf("%-9s value %s, route %s -> %s (%s road expected)\n", which,
		            valueMoved ? "MOVED" : "still", routeMoved ? "MOVED" : "still", ok ? "ok" : "WRONG",
		            wantValue ? "value" : "route");
		if (!ok) {
			++failures;
		}
	};
	// THE ROUTE UNDER TEST, BY ENDPOINT (kill-lanes step 2): the macro, into the parameter.
	const backplane::Source src = backplane::Source::core(source);
	const backplane::Destination into = backplane::Destination::core(static_cast<size_t>(dest));

	// ALT HELD: the value road.
	{
		const double valueBefore = bridge.value(targetId);
		const float amountBefore = bridge.routeAmount(src, into);
		drive(true);
		const double valueAfter = bridge.value(targetId);
		const float amountAfter = bridge.routeAmount(src, into);
		std::printf("alt:      value %.4f -> %.4f, route[%zu][%d] %.3f -> %.3f\n", valueBefore, valueAfter,
		            source, dest, static_cast<double>(amountBefore), static_cast<double>(amountAfter));
		report("alt:", valueAfter != valueBefore, amountAfter != amountBefore, true);
	}

	// ALT LIFTED: the route road — and the half that proves the first was not a knob with no gate.
	{
		const double valueBefore = bridge.value(targetId);
		const float amountBefore = bridge.routeAmount(src, into);
		drive(false);
		const double valueAfter = bridge.value(targetId);
		const float amountAfter = bridge.routeAmount(src, into);
		std::printf("plain:    value %.4f -> %.4f, route[%zu][%d] %.3f -> %.3f\n", valueBefore, valueAfter,
		            source, dest, static_cast<double>(amountBefore), static_cast<double>(amountAfter));
		report("plain:", valueAfter != valueBefore, amountAfter != amountBefore, false);
	}

	std::printf("verdict:  %s\n", failures == 0 ? "both roads taken as the modifier says" :
	                                              "the modifier did not decide the road");
	std::fflush(stdout);
	gui.destroy();
	XDestroyWindow(display, parent);
	XCloseDisplay(display);
	return failures == 0 ? 0 : 1;
}
