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

// A replica of the CLAP editor-embedding protocol Bitwig Studio 5.1.5 performs, reconstructed from
// the observed behaviour of its plugin host and used to reproduce a bug our own guihost could not.
//
//   cmake -B build -DFEATHERS_BUILD_TOOLS=ON && ninja -C build bitwighost
//   build/bitwighost build/feathers.clap          # Bitwig's order: container mapped AFTER set_parent
//   build/bitwighost build/feathers.clap premap   # the easy order, for contrast
//
// WHY IT EXISTS. The editor opened in guihost and in the standalone, and never opened in Bitwig: the
// button did nothing, with no error anywhere. The difference turned out to be one line of ordering.
// A host that maps its container before set_parent makes the plugin's window IsViewable the moment
// it is reparented; Bitwig maps afterwards, so anything that decides "did the embed work" by asking
// X whether it is on screen answers no. Our setParent did exactly that. Running this in both modes
// is the check: a plugin that passes `premap` and fails the default has that bug.
//
// WHAT IT MODELS, and the numbers are Bitwig's, not ours:
//   1. every function pointer in clap_plugin_gui must be non-null or the plugin is treated as having
//      no GUI at all;
//   2. is_api_supported(x11, embedded) and (x11, floating) — a GUI exists if either is true;
//   3. get_preferred_api decides floating vs embedded, defaulting to !embeddedSupported;
//   4. create, then get_size, which must land within [10, 16000] px on both axes;
//   5. a container window, plus the host's own 1x1 helper child of it at (-1,-1);
//   6. set_parent, and then a poll of the container's children every 50ms for a 1.0s budget, waiting
//      for a SECOND child to appear — the plugin's. Miss that deadline and the editor never opens;
//   7. map, configure, read _XEMBED_INFO, send XEMBED_EMBEDDED_NOTIFY.
//
// It deliberately does NOT call show(), hide() or set_scale, because in the sessions we traced
// Bitwig called none of them. Modelling the host faithfully is the whole value here: a replica that
// exercises more than the original stops being evidence about the original. show() is covered by
// guihost instead. Bitwig does carry a set_scale call site, guarded by a change check against a
// cached scale, but it never fired for us — its plugin host was started not-dpi-aware.
//
// This is a model of one host at one version, not a specification. Where it disagrees with a real
// Bitwig, the real Bitwig is right and this file is stale.
//
// Needs the CLAP headers, libX11 and libdl. No GPU or window manager assumptions beyond an X display.

#include "host_common.h"

#include <clap/clap.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

/// Parameter events, the way a host streams automation. Mirrored from guihost deliberately: the
/// whole value of running the chaos recipe HERE is that only the EMBEDDING differs, so the
/// automation side has to be the same code doing the same thing.
struct EventList {
	std::vector<clap_event_param_value> events;
	clap_input_events in {};

	void push(clap_id id, double value) {
		clap_event_param_value event {};
		event.header.size = sizeof(event);
		event.header.time = 0;
		event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
		event.header.type = CLAP_EVENT_PARAM_VALUE;
		event.param_id = id;
		event.note_id = -1;
		event.port_index = -1;
		event.channel = -1;
		event.key = -1;
		event.value = value;
		events.push_back(event);
	}
};

uint32_t evSize(const clap_input_events* list) {
	return static_cast<uint32_t>(static_cast<EventList*>(list->ctx)->events.size());
}
const clap_event_header* evGet(const clap_input_events* list, uint32_t index) {
	return &static_cast<EventList*>(list->ctx)->events[index].header;
}
bool outPush(const clap_output_events*, const clap_event_header*) {
	return true;
}

/// CAPTURES THE PLUGIN'S OWN WINDOW, and the caller must pass that rather than the container.
///
/// The container holds two children: the host's 1x1 helper at (-1,-1), which exists because Bitwig
/// creates one, and the plugin's. Photographing the helper would capture a single pixel, every
/// frame would compare equal, and the run would look clean while having looked at nothing — the
/// exact failure mode tools/drag-storm.sh already records three instances of. `main` has already
/// resolved the right one into `pluginWindow`; use that.
bool capture(Window window, const std::string& path) {
	if (window == 0) {
		return false;
	}
	char command[512];
	std::snprintf(command, sizeof(command), "import -window 0x%lx %s 2>/dev/null",
	              static_cast<unsigned long>(window), path.c_str());
	if (std::system(command) != 0) {
		return false;
	}
	// The comment above already says a wrong-window capture "would compare equal, and the run would
	// look clean while having looked at nothing". A BLANK capture is the same failure with the
	// opposite symptom -- it compares UNequal to everything -- so the equality check cannot see it.
	if (feathers::toolhost::captureIsUniform(path)) {
		std::printf("capture: %s is a single flat colour — the window was not ready\n", path.c_str());
		return false;
	}
	return true;
}

const clap_plugin* gPlugin = nullptr;
int gRegisteredFd = -1;
clap_id gTimerId = CLAP_INVALID_ID;
bool gTimerRegistered = false;

void hostRequestFlush(const clap_host*) {}
void hostRescan(const clap_host*, clap_param_rescan_flags) {}
void hostClear(const clap_host*, clap_id, clap_param_clear_flags) {}
const clap_host_params kHostParams = {hostRescan, hostClear, hostRequestFlush};

bool fdRegister(const clap_host*, int fd, clap_posix_fd_flags_t) {
	gRegisteredFd = fd;
	std::printf("  host: register_fd(%d)\n", fd);
	return true;
}
bool fdModify(const clap_host*, int, clap_posix_fd_flags_t) {
	return true;
}
bool fdUnregister(const clap_host*, int) {
	gRegisteredFd = -1;
	return true;
}
const clap_host_posix_fd_support kHostFd = {fdRegister, fdModify, fdUnregister};

bool timerRegister(const clap_host*, uint32_t periodMs, clap_id* id) {
	*id = 1;
	gTimerId = 1;
	gTimerRegistered = true;
	std::printf("  host: register_timer(%u ms)\n", periodMs);
	return true;
}
bool timerUnregister(const clap_host*, clap_id) {
	gTimerRegistered = false;
	return true;
}
const clap_host_timer_support kHostTimer = {timerRegister, timerUnregister};

void guiResizeHintsChanged(const clap_host*) {}

/// What the plugin has asked the window to become, if anything. Recorded rather than acted on here:
/// a host applies a resize request from its own loop, not from inside the plugin's callback, and
/// calling back into the plugin from here would model a bug rather than a host.
uint32_t gRequestedWidth = 0;
uint32_t gRequestedHeight = 0;
int gResizeRequests = 0;

bool guiRequestResize(const clap_host*, uint32_t width, uint32_t height) {
	gRequestedWidth = width;
	gRequestedHeight = height;
	++gResizeRequests;
	return true;
}
bool guiRequestShow(const clap_host*) {
	return true;
}
bool guiRequestHide(const clap_host*) {
	return true;
}
void guiClosed(const clap_host*, bool) {}
const clap_host_gui kHostGui = {guiResizeHintsChanged, guiRequestResize, guiRequestShow,
                                guiRequestHide,        guiClosed};

const void* getExtension(const clap_host*, const char* id) {
	if (std::strcmp(id, CLAP_EXT_PARAMS) == 0) {
		return &kHostParams;
	}
	if (std::strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT) == 0) {
		return &kHostFd;
	}
	if (std::strcmp(id, CLAP_EXT_TIMER_SUPPORT) == 0) {
		return &kHostTimer;
	}
	if (std::strcmp(id, CLAP_EXT_GUI) == 0) {
		return &kHostGui;
	}
	return nullptr;
}
void hostNoop(const clap_host*) {}

clap_host gHost = {CLAP_VERSION_INIT, nullptr,      "Bitwig Studio", "Bitwig", "",
                   "5.1.5",           getExtension, hostNoop,        hostNoop, hostNoop};

int countChildren(Display* d, Window parent, std::vector<Window>& out) {
	Window root = 0;
	Window par = 0;
	Window* kids = nullptr;
	unsigned int n = 0;
	out.clear();
	if (XQueryTree(d, parent, &root, &par, &kids, &n) == 0) {
		return -1;
	}
	for (unsigned int i = 0; i < n; ++i) {
		out.push_back(kids[i]);
	}
	if (kids != nullptr) {
		XFree(kids);
	}
	return static_cast<int>(n);
}

} // namespace

int main(int argc, char** argv) {
	// BEFORE ANYTHING LOADS — see `toolhost::isolateUserConfig`. A test host must not write
	// the files the installed plugin reads.
	feathers::toolhost::isolateUserConfig("bitwighost");
	if (argc < 2) {
		std::printf("usage: bitwighost <plugin.clap> [premap] [cycles]\n");
		return 2;
	}
	// Before any other Xlib call, and before the plugin opens its own connection: visage runs a timer
	// thread that pokes X, and without this Xlib's sequence tracking trips over itself.
	XInitThreads();

	// THE LOAD DANCE IS SHARED — see tools/host_common.h. This host had folded its four steps into
	// two reports AND dereferenced the factory without checking it: `factory->get_plugin_descriptor`
	// on a null factory is a crash where the shared loader says "plugin offers no plugin factory".
	// The exit code stays 2, which is this host's own contract with drag-storm.sh.
	const feathers::toolhost::Loaded loaded = feathers::toolhost::loadPlugin(argv[1], &gHost);
	if (!loaded) {
		return 2;
	}
	gPlugin = loaded.plugin;
	std::printf("plugin: %s\n\n", loaded.descriptor->id);

	auto* gui = static_cast<const clap_plugin_gui*>(gPlugin->get_extension(gPlugin, CLAP_EXT_GUI));
	if (gui == nullptr) {
		std::printf("STEP 1 FAIL: no clap.gui extension\n");
		return 1;
	}

	// STEP 1: every pointer must be non-null, exactly as Bitwig checks.
	struct {
		const char* name;
		const void* ptr;
	} slots[] = {
	    {"is_api_supported", (const void*)gui->is_api_supported},
	    {"get_preferred_api", (const void*)gui->get_preferred_api},
	    {"create", (const void*)gui->create},
	    {"destroy", (const void*)gui->destroy},
	    {"set_scale", (const void*)gui->set_scale},
	    {"get_size", (const void*)gui->get_size},
	    {"can_resize", (const void*)gui->can_resize},
	    {"get_resize_hints", (const void*)gui->get_resize_hints},
	    {"adjust_size", (const void*)gui->adjust_size},
	    {"set_size", (const void*)gui->set_size},
	    {"suggest_title", (const void*)gui->suggest_title},
	    {"show", (const void*)gui->show},
	    {"hide", (const void*)gui->hide},
	};
	for (const auto& s : slots) {
		if (s.ptr == nullptr) {
			std::printf("STEP 1 FAIL: clap_plugin_gui.%s is null -> Bitwig reports NO GUI\n", s.name);
			return 1;
		}
	}
	std::printf("STEP 1 ok: all clap_plugin_gui pointers non-null\n");

	// STEP 2/3
	const bool embedded = gui->is_api_supported(gPlugin, CLAP_WINDOW_API_X11, false);
	const bool floating = gui->is_api_supported(gPlugin, CLAP_WINDOW_API_X11, true);
	std::printf("STEP 2 ok: embedded=%d floating=%d -> hasGui=%d\n", embedded, floating,
	            embedded || floating);
	const char* prefApi = nullptr;
	bool prefFloating = false;
	const bool gotPref = gui->get_preferred_api(gPlugin, &prefApi, &prefFloating);
	const bool openFloating = gotPref ? prefFloating : !embedded;
	std::printf("STEP 3 ok: get_preferred_api -> %d (%s, floating=%d); opening %s\n", gotPref,
	            prefApi ? prefApi : "(null)", prefFloating, openFloating ? "FLOATING" : "EMBEDDED");
	if (openFloating) {
		std::printf("(this replica only covers the embedded path)\n");
		return 1;
	}

	// CYCLES: the whole embed, performed again, because a host does this every time the user shows and
	// hides the window — and because the ordering below is the one that made a teardown deadlock bite
	// in Bitwig while a simpler harness sailed through it. Anything after the first round is a REOPEN.
	const int cycles = argc > 3 ? std::atoi(argv[3]) : (argc > 2 && std::strcmp(argv[2], "premap") != 0
	                                                        ? std::atoi(argv[2])
	                                                        : 1);
	bool settledAll = true;
	for (int cycle = 1; cycle <= (cycles > 0 ? cycles : 1); ++cycle) {
	if (cycle > 1) {
		std::printf("\n===== REOPEN %d =====\n", cycle);
	}

	// STEP 4: create comes FIRST in the embedded branch (vtbl+0x2a8 at 1ee6db), then get_size
	// (vtbl+0x2c0 at 1ee705), then the container window, then set_parent (vtbl+0x2d8 at 1ee74d).
	if (!gui->create(gPlugin, CLAP_WINDOW_API_X11, false)) {
		std::printf("STEP 4 FAIL: gui->create returned false (Bitwig logs nothing here and the "
		            "button does nothing)\n");
		return 1;
	}
	std::printf("STEP 4 ok: gui->create\n");

	uint32_t w = 0;
	uint32_t h = 0;
	if (!gui->get_size(gPlugin, &w, &h)) {
		std::printf("STEP 5 FAIL: 'Could not get initial size of plugin before opening window'\n");
		return 1;
	}
	// Bitwig rejects anything outside [10, 16000] in either axis (bounds checked at 2060ba).
	if (w < 10 || w > 16000 || h < 10 || h > 16000) {
		std::printf("STEP 5 FAIL: size %ux%u is outside Bitwig's accepted range [10,16000] -> "
		            "'Could not get initial size of plugin before opening window'\n",
		            w, h);
		return 1;
	}
	std::printf("STEP 5 ok: initial size %ux%u (within Bitwig's [10,16000] bounds)\n", w, h);

	// STEP 6: container window plus Bitwig's own 1x1 helper child.
	Display* d = XOpenDisplay(nullptr);
	if (d == nullptr) {
		std::printf("no display\n");
		return 2;
	}
	Window top = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, w, h, 0, 0, 0x202020);
	Window container = XCreateSimpleWindow(d, top, 0, 0, w, h, 0, 0, 0x202020);
	Window helper = XCreateSimpleWindow(d, container, -1, -1, 1, 1, 0, 0, 0);
	// Bitwig maps the container only AFTER set_parent (xcb_map_window at 1ee845, set_parent at
	// 1ee7bc). Pass "premap" to map it first, which is what our own guihost does and what hides the
	// difference. With the container unmapped, a plugin window inside it is IsUnviewable, not
	// IsViewable, however correctly it was created.
	const bool premap = argc > 2 && std::strcmp(argv[2], "premap") == 0;
	if (premap) {
		XMapWindow(d, container);
		XMapWindow(d, top);
	}
	XSync(d, False);
	std::printf("(container mapped %s set_parent)\n", premap ? "BEFORE" : "AFTER");
	std::printf("STEP 6 ok: container=0x%lx helper=0x%lx\n", container, helper);

	clap_window win{};
	win.api = CLAP_WINDOW_API_X11;
	win.x11 = container;
	const bool parented = gui->set_parent(gPlugin, &win);
	std::printf("STEP 6: gui->set_parent -> %d\n", parented);
	if (!parented) {
		std::printf("STEP 6 FAIL: set_parent returned false; Bitwig abandons the open here and "
		            "calls gui->destroy(). The button does nothing.\n");
		gui->destroy(gPlugin);
		return 1;
	}
	if (!premap) {
		XMapWindow(d, container);
		XMapWindow(d, top);
		XSync(d, False);
	}

	// STEP 7: the poll Bitwig performs, 50ms apart, 1.0s budget.
	std::vector<Window> kids;
	Window pluginWindow = 0;
	double elapsed = 0.0;
	bool first = true;
	while (true) {
		if (!first) {
			usleep(50000);
			elapsed += 0.05;
			if (elapsed >= 1.0) {
				std::printf("STEP 7 FAIL after %.2fs: 'Plugin did not create it's window inside the "
				            "plugin window'\n",
				            elapsed);
				return 1;
			}
		}
		first = false;
		XSync(d, False);
		const int n = countChildren(d, container, kids);
		if (n < 0) {
			continue;
		}
		if (n == 1) {
			continue; // only our helper so far
		}
		pluginWindow = kids[0] == helper ? kids[1] : kids[0];
		break;
	}
	std::printf("STEP 7 ok after %.2fs: plugin window 0x%lx is a child of the container\n", elapsed,
	            pluginWindow);

	// STEP 8: XEmbed handshake.
	XMapWindow(d, pluginWindow);
	XSync(d, False);
	Atom embedInfo = XInternAtom(d, "_XEMBED_INFO", False);
	Atom actualType = 0;
	int actualFormat = 0;
	unsigned long nItems = 0;
	unsigned long bytesAfter = 0;
	unsigned char* prop = nullptr;
	const int got = XGetWindowProperty(d, pluginWindow, embedInfo, 0, 2, False, AnyPropertyType,
	                                   &actualType, &actualFormat, &nItems, &bytesAfter, &prop);
	std::printf("STEP 8: _XEMBED_INFO present=%d nItems=%lu\n", got == Success && prop != nullptr,
	            nItems);
	if (prop != nullptr) {
		XFree(prop);
	}

	// Give the editor some frames, driving it the way the host would.
	auto* fdExt = static_cast<const clap_plugin_posix_fd_support*>(
	    gPlugin->get_extension(gPlugin, CLAP_EXT_POSIX_FD_SUPPORT));
	auto* timerExt = static_cast<const clap_plugin_timer_support*>(
	    gPlugin->get_extension(gPlugin, CLAP_EXT_TIMER_SUPPORT));
	auto pump = [&](int frames) {
		for (int i = 0; i < frames; ++i) {
			if (gRegisteredFd >= 0 && fdExt != nullptr) {
				fdExt->on_fd(gPlugin, gRegisteredFd, CLAP_POSIX_FD_READ);
			}
			if (gTimerRegistered && timerExt != nullptr) {
				timerExt->on_timer(gPlugin, gTimerId);
			}
			usleep(8000);
		}
	};
	pump(60);

	// STEP 9: a resize, which is a NEGOTIATION and not an instruction.
	//
	// A host drags a window edge and asks the plugin what that size would really be — adjust_size —
	// then gives it that size and expects get_size to agree. Two properties have to hold or the two
	// of them never stop talking:
	//
	//   adjust_size(x) must be IDEMPOTENT: adjust_size(adjust_size(x)) == adjust_size(x). Otherwise
	//   every round of the conversation produces a new answer.
	//
	//   set_size(adjust_size(x)) must leave get_size() == adjust_size(x). Otherwise the host writes
	//   a size, reads back a different one, and writes again.
	//
	// And a resize the HOST asked for must not come back as a resize the PLUGIN asks for. A plugin
	// that answers every size change with clap_host_gui.request_resize is holding one end of a loop;
	// the host holds the other.
	std::printf("\n--- STEP 9: resize negotiation ---\n");
	bool settled = true;
	const struct {
		uint32_t w;
		uint32_t h;
	} drags[] = {{1400, 820}, {1000, 560}, {1600, 900}};

	for (const auto& drag : drags) {
		uint32_t adjusted_w = drag.w;
		uint32_t adjusted_h = drag.h;
		gui->adjust_size(gPlugin, &adjusted_w, &adjusted_h);

		uint32_t twice_w = adjusted_w;
		uint32_t twice_h = adjusted_h;
		gui->adjust_size(gPlugin, &twice_w, &twice_h);
		const bool idempotent = twice_w == adjusted_w && twice_h == adjusted_h;

		std::printf("drag to %ux%u -> adjust_size says %ux%u%s\n", drag.w, drag.h, adjusted_w,
		            adjusted_h, idempotent ? "" : "   [NOT IDEMPOTENT]");
		if (adjusted_w > drag.w || adjusted_h > drag.h) {
			std::printf("   (raised to the plugin's minimum, which is a legitimate answer)\n");
		}
		if (adjusted_w < drag.w || adjusted_h < drag.h) {
			std::printf("   [REFUSED] the plugin answers a dragged size with a SMALLER one, so the "
			            "window can never follow the pointer\n");
			settled = false;
		}

		gResizeRequests = 0;
		gui->set_size(gPlugin, adjusted_w, adjusted_h);
		XResizeWindow(d, container, adjusted_w, adjusted_h);
		XResizeWindow(d, pluginWindow, adjusted_w, adjusted_h);
		XSync(d, False);
		pump(12);

		uint32_t got_w = 0;
		uint32_t got_h = 0;
		gui->get_size(gPlugin, &got_w, &got_h);
		const bool agreed = got_w == adjusted_w && got_h == adjusted_h;
		std::printf("   set_size(%u,%u) then get_size -> %ux%u%s\n", adjusted_w, adjusted_h, got_w,
		            got_h, agreed ? "" : "   [DISAGREES WITH WHAT WAS SET]");
		if (!idempotent || !agreed) {
			settled = false;
		}

		// Now behave like a host: honour what the plugin asked for, and see whether that ends the
		// conversation or restarts it. A settled editor asks for nothing here.
		int rounds = 0;
		while (gResizeRequests > 0 && rounds < 8) {
			const uint32_t wantW = gRequestedWidth;
			const uint32_t wantH = gRequestedHeight;
			std::printf("   round %d: plugin asked the host for %ux%u (%d request(s))\n", rounds + 1,
			            wantW, wantH, gResizeRequests);
			gResizeRequests = 0;
			gui->set_size(gPlugin, wantW, wantH);
			XResizeWindow(d, container, wantW, wantH);
			XResizeWindow(d, pluginWindow, wantW, wantH);
			XSync(d, False);
			pump(12);
			++rounds;
		}
		if (rounds >= 8) {
			std::printf("   [DID NOT SETTLE] still asking after %d rounds — this is the oscillation\n",
			            rounds);
			settled = false;
		}
		else if (rounds > 0) {
			std::printf("   settled after %d round(s)\n", rounds);
		}
	}

	// STEP 10: a size the plugin did NOT choose. Step 9 could not move the window at all, because
	// adjust_size answered with the size it already was — so nothing ever changed and the feedback
	// edge never got a chance to fire. A host reaches this path whenever a size arrives that the
	// plugin did not pick: restoring a saved window size with a project, a host that resizes first
	// and asks later, or a user dragging an edge in a host that honours the drag.
	// Which of the two ways a size can change is the one that makes the plugin talk back? The window
	// manager moving the X window, or the host calling set_size? The guard belongs on whichever it
	// is, so they are tried separately rather than together.
	std::printf("\n--- STEP 9b: the X window resized WITHOUT set_size ---\n");
	gResizeRequests = 0;
	XResizeWindow(d, container, 1320, 760);
	XResizeWindow(d, pluginWindow, 1320, 760);
	XSync(d, False);
	pump(20);
	std::printf("WM-style resize to 1320x760 -> plugin asked for %ux%u (%d request(s))\n",
	            gRequestedWidth, gRequestedHeight, gResizeRequests);
	uint32_t after_w = 0;
	uint32_t after_h = 0;
	gui->get_size(gPlugin, &after_w, &after_h);
	std::printf("   get_size now reports %ux%u\n", after_w, after_h);

	std::printf("\n--- STEP 10: a size the plugin did not choose ---\n");
	gResizeRequests = 0;
	gui->set_size(gPlugin, 1400, 820);
	XResizeWindow(d, container, 1400, 820);
	XResizeWindow(d, pluginWindow, 1400, 820);
	XSync(d, False);
	pump(12);
	std::printf("host set 1400x820; plugin then asked for %ux%u (%d request(s))\n", gRequestedWidth,
	            gRequestedHeight, gResizeRequests);

	int rounds = 0;
	while (gResizeRequests > 0 && rounds < 8) {
		const uint32_t wantW = gRequestedWidth;
		const uint32_t wantH = gRequestedHeight;
		gResizeRequests = 0;
		gui->set_size(gPlugin, wantW, wantH);
		XResizeWindow(d, container, wantW, wantH);
		XResizeWindow(d, pluginWindow, wantW, wantH);
		XSync(d, False);
		pump(12);
		++rounds;
		std::printf("   round %d: honoured %ux%u, plugin now asks for %ux%u (%d request(s))\n", rounds,
		            wantW, wantH, gRequestedWidth, gRequestedHeight, gResizeRequests);
	}
	if (rounds >= 8) {
		std::printf("   [DID NOT SETTLE] the plugin answers every host resize with another request\n");
		settled = false;
	}

	// THE CHAOS RECIPE, IN THE EMBEDDED VENUE — the one condition every real sighting of the
	// remaining glitch has shared and no clean run has had.
	//
	// guihost runs the same automation and comes back clean: 1800 windowless frames and 45 s windowed
	// at 1856x1048 with all nine gamma parameters churning, without one SHORT, UNWALKED or DROPPED
	// line. The recipe is not the remaining variable. The EMBEDDING is: this file's whole reason to
	// exist is that a bug once lived in the difference between how Bitwig parents an editor and how
	// guihost opens one, and it was invisible everywhere else.
	//
	// So the loop below is guihost's, deliberately unchanged in every respect but the venue — same
	// parameters found the same way by name, same rates, same 60 Hz slicing, same per-second capture.
	// A difference in the result is then a difference in the embedding, which is the only thing this
	// run is asking about.
	if (const char* chaosFor = std::getenv("FEATHERS_BITWIGHOST_CHAOS")) {
		const int seconds = std::atoi(chaosFor);
		auto* params = static_cast<const clap_plugin_params*>(
		    gPlugin->get_extension(gPlugin, CLAP_EXT_PARAMS));
		const std::string outDir = std::getenv("FEATHERS_CHAOS_OUT") != nullptr
		                               ? std::getenv("FEATHERS_CHAOS_OUT")
		                               : ".";

		clap_id pathId = CLAP_INVALID_ID;
		clap_id posXId = CLAP_INVALID_ID;
		clap_id posYId = CLAP_INVALID_ID;
		clap_id weatherId = CLAP_INVALID_ID;
		clap_id gammaId = CLAP_INVALID_ID;
		clap_id soundGammaIds[8];
		for (clap_id& id : soundGammaIds) {
			id = CLAP_INVALID_ID;
		}

		// BY NAME, never by constant, for the reason guihost states: a hard-coded id that silently
		// moves is a run that proves nothing about the parameter it claims to be moving.
		const uint32_t count = params != nullptr ? params->count(gPlugin) : 0;
		for (uint32_t i = 0; i < count; ++i) {
			clap_param_info info {};
			if (!params->get_info(gPlugin, i, &info)) {
				continue;
			}
			const std::string name = info.name;
			if (name == "Path") {
				pathId = info.id;
			}
			else if (name == "Pos X") {
				posXId = info.id;
			}
			else if (name == "Pos Y") {
				posYId = info.id;
			}
			else if (name == "Weather") {
				weatherId = info.id;
			}
			else if (name == "Global Gamma") {
				gammaId = info.id;
			}
			else if (name.size() == 7 && name.compare(1, 6, " Gamma") == 0 && name[0] >= 'A'
			         && name[0] <= 'H') {
				soundGammaIds[name[0] - 'A'] = info.id;
			}
		}

		// The child the embed protocol above already resolved — not the container, and not the 1x1
		// helper that shares it.
		const Window editor = pluginWindow;
		if (seconds > 0 && params != nullptr && pathId != CLAP_INVALID_ID
		    && posXId != CLAP_INVALID_ID && posYId != CLAP_INVALID_ID
		    && weatherId != CLAP_INVALID_ID && editor != 0) {
			int soundGammas = 0;
			for (const clap_id id : soundGammaIds) {
				soundGammas += id != CLAP_INVALID_ID ? 1 : 0;
			}
			std::printf("chaos:    %d s EMBEDDED at Weather 0.6%s, %d per-sound gamma(s)"
			            " — editor 0x%lx in container 0x%lx\n",
			            seconds,
			            gammaId == CLAP_INVALID_ID ? " (no Global Gamma found — not churning it)"
			                                       : " + Global Gamma churn",
			            soundGammas, static_cast<unsigned long>(editor),
			            static_cast<unsigned long>(container));
			std::fflush(stdout);

			const int slices = seconds * 60;
			for (int slice = 0; slice < slices; ++slice) {
				const double t = static_cast<double>(slice) / 60.0;
				EventList moving;
				moving.push(weatherId, 0.6);
				moving.push(pathId, 0.5 + 0.5 * std::sin(t * 2.0 * 3.14159265358979 * 0.31));
				moving.push(posXId, std::sin(t * 2.0 * 3.14159265358979 * 0.53));
				moving.push(posYId, std::cos(t * 2.0 * 3.14159265358979 * 0.83));
				if (gammaId != CLAP_INVALID_ID) {
					moving.push(gammaId, 4.0 * std::sin(t * 2.0 * 3.14159265358979 * 1.7));
				}
				for (int sound = 0; sound < 8; ++sound) {
					if (soundGammaIds[sound] == CLAP_INVALID_ID) {
						continue;
					}
					const double rate = 0.7 + 0.37 * sound;
					moving.push(soundGammaIds[sound],
					            4.0 * std::sin(t * 2.0 * 3.14159265358979 * rate + sound));
				}
				moving.in.ctx = &moving;
				moving.in.size = evSize;
				moving.in.get = evGet;
				clap_output_events discard {};
				discard.ctx = nullptr;
				discard.try_push = outPush;
				params->flush(gPlugin, &moving.in, &discard);
				pump(2); // the pump's frame is 8 ms, so two of them is one 60 Hz slice

				if (slice % 60 == 0) {
					char path[512] = {};
					std::snprintf(path, sizeof(path), "%s/embed_%03d.png", outDir.c_str(), slice / 60);
					capture(editor, path);
				}
			}
			std::printf("chaos:    done, %d captures in %s\n", slices / 60, outDir.c_str());
			std::fflush(stdout);
		}
		else {
			// SAYS WHICH HALF FAILED, because "skipped" on its own has sent this project chasing the
			// wrong thing before.
			std::printf("chaos:    SKIPPED — params %s, ids %s, editor window %s\n",
			            params != nullptr ? "found" : "MISSING",
			            pathId != CLAP_INVALID_ID && posXId != CLAP_INVALID_ID
			                    && posYId != CLAP_INVALID_ID && weatherId != CLAP_INVALID_ID
			                ? "found"
			                : "MISSING",
			            editor != 0 ? "found" : "MISSING");
			std::fflush(stdout);
		}
	}

	// HOLD OPEN, so the editor can be driven while it is embedded THE WAY BITWIG EMBEDS IT.
	//
	// guihost has the same switch, and the two are not interchangeable for this: guihost opens the
	// editor its own way, and the mid-drag corruption we are chasing has only ever been seen inside
	// Bitwig. If the embedding is what makes the difference, only this venue can show it.
	//
	// The container is printed rather than the plugin's own window: a driver wants the window it can
	// aim at, and the plugin's child is inside this one.
	if (const char* held = std::getenv("FEATHERS_BITWIGHOST_HOLD")) {
		const int seconds = std::atoi(held);
		if (seconds > 0) {
			std::printf("holding:  %d s embedded — container 0x%lx\n", seconds,
			            static_cast<unsigned long>(container));
			std::fflush(stdout);
			pump(seconds * 125); // the pump's frame is 8 ms
			std::printf("holding:  done\n");
			std::fflush(stdout);
		}
	}

	std::printf("\nRESULT: the embed protocol Bitwig performs SUCCEEDED against this plugin.\n");
	std::printf("RESIZE: %s\n", settled ? "the negotiation settles"
	                                     : "THE NEGOTIATION DOES NOT SETTLE — see the steps above");
	gui->destroy(gPlugin);
	// The container goes the moment the host is done with it, exactly as Bitwig's does — which is why
	// the plugin must have taken its window out of it by the time destroy returns.
	XDestroyWindow(d, helper);
	XDestroyWindow(d, container);
	XDestroyWindow(d, top);
	XSync(d, False);
	XCloseDisplay(d);
	settledAll = settledAll && settled;
	}
	return settledAll ? 0 : 1;
}
