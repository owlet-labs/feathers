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

// Open the editor, close it, open it again — the cycle a host performs every time a user shows and
// hides a plugin window, and the one thing none of our harnesses ever did.
//
//   build/cyclehost build/feathers.clap [cycles]
//
// WHY THIS EXISTS. An editor closed in Bitwig never came back, and Bitwig then hung
// saving plugin state. The plugin host's main thread was hard-blocked on a futex with the Vulkan
// swapchain threads still alive after the editor had been destroyed. Every harness we owned opened
// the editor exactly once and then let the process exit, so the entire close-then-reopen path — the
// one a user takes constantly — had never been executed anywhere.
//
// A HANG IS A RESULT HERE, not a wedged terminal. Every call the host makes into the plugin is
// wrapped in an alarm, so if one blocks, this says which call in which cycle and exits non-zero
// instead of joining the thing it was meant to diagnose.

#include "host_common.h"

#include <clap/clap.h>

#include <X11/Xlib.h>

#include <csignal>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <string>
#include <unistd.h>

namespace {

const clap_plugin* gPlugin = nullptr;
const clap_plugin_posix_fd_support* gFdExt = nullptr;
int gRegisteredFd = -1;
bool gFlushPending = false;

void hostRequestFlush(const clap_host*) { gFlushPending = true; }
void hostRescan(const clap_host*, clap_param_rescan_flags) {}
void hostClear(const clap_host*, clap_id, clap_param_clear_flags) {}
const clap_host_params kHostParams = {hostRescan, hostClear, hostRequestFlush};

bool fdRegister(const clap_host*, int fd, clap_posix_fd_flags_t) {
	gRegisteredFd = fd;
	return true;
}
bool fdModify(const clap_host*, int, clap_posix_fd_flags_t) { return true; }
bool fdUnregister(const clap_host*, int) {
	gRegisteredFd = -1;
	return true;
}
const clap_host_posix_fd_support kHostFd = {fdRegister, fdModify, fdUnregister};

void guiResizeHintsChanged(const clap_host*) {}
bool guiRequestResize(const clap_host*, uint32_t, uint32_t) { return true; }
bool guiRequestShow(const clap_host*) { return true; }
bool guiRequestHide(const clap_host*) { return true; }
void guiClosed(const clap_host*, bool) {}
const clap_host_gui kHostGui = {guiResizeHintsChanged, guiRequestResize, guiRequestShow,
                                guiRequestHide,        guiClosed};

const void* getExtension(const clap_host*, const char* id) {
	if (std::strcmp(id, CLAP_EXT_PARAMS) == 0) return &kHostParams;
	if (std::strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT) == 0) return &kHostFd;
	if (std::strcmp(id, CLAP_EXT_GUI) == 0) return &kHostGui;
	return nullptr;
}
void hostNoop(const clap_host*) {}
clap_host gHost = {CLAP_VERSION_INIT, nullptr,      "feathers cyclehost", "Owlet Records", "",
                   "1.0",             getExtension, hostNoop,             hostNoop,        hostNoop};

//------------------------------------------------------------------//
// The watchdog: a blocked call must name itself rather than wedge.  //
//------------------------------------------------------------------//

const char* gStep = "(nothing yet)";
int gCycle = 0;

void onAlarm(int) {
	// Deliberately write(2) and _exit: a hung process may hold locks that printf would want.
	char message[256];
	const int n = std::snprintf(message, sizeof(message),
	                            "\nHUNG: cycle %d blocked in %s — no return after the deadline.\n",
	                            gCycle, gStep);
	ssize_t ignored = write(2, message, static_cast<size_t>(n));
	(void)ignored;
	_exit(3);
}

struct Step {
	explicit Step(const char* what, unsigned seconds = 20) {
		gStep = what;
		alarm(seconds);
	}
	~Step() { alarm(0); }
};

/// This process's CPU time, in milliseconds. utime + stime from /proc, which is the only number that
/// settles "does a closed editor still cost anything".
double cpuMs() {
	FILE* f = std::fopen("/proc/self/stat", "r");
	if (f == nullptr) {
		return 0.0;
	}
	char buffer[2048];
	const size_t n = std::fread(buffer, 1, sizeof(buffer) - 1, f);
	std::fclose(f);
	buffer[n] = '\0';
	// Field 14 and 15, after the comm field which may contain spaces inside parentheses.
	const char* p = std::strrchr(buffer, ')');
	if (p == nullptr) {
		return 0.0;
	}
	long utime = 0;
	long stime = 0;
	int field = 3;
	for (const char* c = p + 2; *c != '\0'; ++c) {
		if (*c == ' ') {
			++field;
			if (field == 14) utime = std::strtol(c + 1, nullptr, 10);
			if (field == 15) { stime = std::strtol(c + 1, nullptr, 10); break; }
		}
	}
	const double tick = static_cast<double>(sysconf(_SC_CLK_TCK));
	return (utime + stime) * 1000.0 / tick;
}

double nowMs() {
	timespec ts{};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

/// Give the plugin its thread back, the way a host's main loop does.
void pump(Display* display, int milliseconds) {
	for (int i = 0; i < milliseconds / 5; ++i) {
		if (gRegisteredFd >= 0 && gFdExt != nullptr) {
			gFdExt->on_fd(gPlugin, gRegisteredFd, CLAP_POSIX_FD_READ);
		}
		while (XPending(display) > 0) {
			XEvent event;
			XNextEvent(display, &event);
		}
		usleep(5000);
	}
}

} // namespace

int main(int argc, char** argv) {
	// BEFORE ANYTHING LOADS — see `toolhost::isolateUserConfig`. A test host must not write
	// the files the installed plugin reads.
	feathers::toolhost::isolateUserConfig("cyclehost");
	if (argc < 2) {
		std::printf("usage: %s <path-to.clap> [cycles] [capture-directory]\n", argv[0]);
		return 2;
	}
	const std::string pluginPath = argv[1];
	const int cycles = argc > 2 ? std::atoi(argv[2]) : 3;
	const char* captureDir = argc > 3 ? argv[3] : nullptr;

	XInitThreads();
	std::signal(SIGALRM, onAlarm);
#if defined(__linux__) && defined(PR_SET_PTRACER)
	// Let a debugger attach without sudo. Yama's ptrace_scope defaults to 1 on this distribution,
	// which lets only a direct parent attach — so `eu-stack -p` on this process from another terminal
	// answers "Operation not permitted", and a wedged run is exactly when its stack is worth having.
	// A harness whose whole purpose is reproducing a hang should be inspectable while hung.
	prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
#endif

	// THE LOAD DANCE IS SHARED — see tools/host_common.h. This host had folded four of its steps
	// into two reports; the shared one keeps guihost's granular form, so a refusal here now says
	// WHICH step refused.
	const feathers::toolhost::Loaded loaded = feathers::toolhost::loadPlugin(pluginPath.c_str(), &gHost);
	if (!loaded) {
		return 1;
	}
	const clap_plugin_entry* entry = loaded.entry;
	gPlugin = loaded.plugin;
	const auto* gui = static_cast<const clap_plugin_gui*>(gPlugin->get_extension(gPlugin, CLAP_EXT_GUI));
	gFdExt = static_cast<const clap_plugin_posix_fd_support*>(
	    gPlugin->get_extension(gPlugin, CLAP_EXT_POSIX_FD_SUPPORT));
	if (gui == nullptr) {
		std::printf("plugin has no gui extension\n");
		return 1;
	}

	Display* display = XOpenDisplay(nullptr);
	if (display == nullptr) {
		std::printf("XOpenDisplay failed — is DISPLAY set?\n");
		return 1;
	}
	const int screen = DefaultScreen(display);

	std::printf("plugin:   %s\n", loaded.descriptor->id);
	std::printf("cycles:   %d open/close rounds\n\n", cycles);

	// FEATHERS_CYCLE_RECREATE_PLUGIN: throw the whole plugin instance away between rounds, which is
	// what removing a device and adding it again does. A fix that only survives editor cycles would
	// still wedge here.
	const bool recreatePlugin = std::getenv("FEATHERS_CYCLE_RECREATE_PLUGIN") != nullptr;
	const char* idleText = std::getenv("FEATHERS_CYCLE_IDLE_MS");
	const int idleMs = idleText != nullptr ? std::atoi(idleText) : 0;
	const bool destroyParentFirst = std::getenv("FEATHERS_CYCLE_DESTROY_PARENT_FIRST") != nullptr;
	int emptyRounds = 0;

	for (int cycle = 1; cycle <= cycles; ++cycle) {
		gCycle = cycle;
		const double started = nowMs();
		if (recreatePlugin && cycle > 1) {
			Step step("plugin->destroy (recreate mode)");
			gPlugin->destroy(gPlugin);
			gPlugin = feathers::toolhost::createAnother(loaded, &gHost);
			if (gPlugin == nullptr) {
				std::printf("cycle %d: could not re-create the plugin instance\n", cycle);
				return 1;
			}
			gui = static_cast<const clap_plugin_gui*>(gPlugin->get_extension(gPlugin, CLAP_EXT_GUI));
			gFdExt = static_cast<const clap_plugin_posix_fd_support*>(
			    gPlugin->get_extension(gPlugin, CLAP_EXT_POSIX_FD_SUPPORT));
		}

		{
			Step step("gui->create");
			if (!gui->create(gPlugin, CLAP_WINDOW_API_X11, false)) {
				std::printf("cycle %d: gui->create REFUSED\n", cycle);
				return 1;
			}
		}

		uint32_t width = 0;
		uint32_t height = 0;
		{
			Step step("gui->get_size");
			gui->get_size(gPlugin, &width, &height);
		}

		Window parent = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0, width, height,
		                                    0, BlackPixel(display, screen), BlackPixel(display, screen));
		XFlush(display);

		// FEATHERS_CYCLE_SKIP_FIRST_SHOW: open the first editor WITHOUT ever parenting or showing it,
		// so the renderer is never initialised in that round. If the second round then works, the
		// trigger is the renderer coming up — not anything our create/destroy pair leaves behind.
		const bool skipShow = cycle == 1 && std::getenv("FEATHERS_CYCLE_SKIP_FIRST_SHOW") != nullptr;
		if (skipShow) {
			std::printf("cycle %d: created only — no set_parent, no show, so no renderer\n", cycle);
			{
				Step step("gui->destroy (unshown)");
				gui->destroy(gPlugin);
			}
			XDestroyWindow(display, parent);
			XFlush(display);
			continue;
		}

		clap_window window{};
		window.api = CLAP_WINDOW_API_X11;
		window.x11 = parent;
		{
			Step step("gui->set_parent");
			if (!gui->set_parent(gPlugin, &window)) {
				std::printf("cycle %d: gui->set_parent REFUSED — the editor would not embed\n", cycle);
				return 1;
			}
		}

		XMapWindow(display, parent);
		XRaiseWindow(display, parent);
		XFlush(display);

		{
			Step step("gui->show");
			if (!gui->show(gPlugin)) {
				std::printf("cycle %d: gui->show REFUSED\n", cycle);
				return 1;
			}
		}

		{
			Step step("pump", 30);
			pump(display, 900);
		}

		// On the last round, photograph it. A window that exists but draws nothing is the other half of
		// "the reopen does not work", and only pixels can tell those apart — run tools/halo_profile.py
		// over this capture to check the editor is not merely present but rendering its own contents.
		if (cycle == cycles && captureDir != nullptr) {
			Step step("capture", 30);
			Window target = parent;
			Window r = 0, p2 = 0, *ch = nullptr;
			unsigned int n = 0;
			if (XQueryTree(display, parent, &r, &p2, &ch, &n) != 0 && n > 0) target = ch[0];
			if (ch != nullptr) XFree(ch);
			char command[512];
			std::snprintf(command, sizeof(command), "import -window 0x%lx %s/cyclehost_last.png 2>/dev/null",
			              static_cast<unsigned long>(target), captureDir);
			const std::string shotPath = std::string(captureDir) + "/cyclehost_last.png";
			if (std::system(command) != 0) {
				std::printf("          (capture failed — is `import` on PATH?)\n");
			} else if (feathers::toolhost::captureIsUniform(shotPath)) {
				// This host's whole question is whether a REOPENED editor draws. A blank frame is
				// what "it did not draw" looks like, so scoring one as a capture would answer this
				// tool's question wrongly in the reassuring direction.
				std::printf("          (capture is a single flat colour — the reopened editor drew"
				            " nothing, or the window was not ready)\n");
			}
		}

		// Did the editor actually put a window inside ours? A create that returns true and produces no
		// window is exactly the shape of "reopen never appears".
		Window root = 0;
		Window par = 0;
		Window* kids = nullptr;
		unsigned int count = 0;
		XQueryTree(display, parent, &root, &par, &kids, &count);
		if (kids != nullptr) XFree(kids);

		// FEATHERS_CYCLE_DESTROY_PARENT_FIRST: the host destroys its container BEFORE guiDestroy (backlog GP). X
		// destroys a window's children with it, so the editor's own window dies with the container; the default
		// order below is the one that makes `detachFromParent` sufficient, and it is the only order this tool
		// drove until GP — which is why it could never find GP.
		if (destroyParentFirst) {
			XDestroyWindow(display, parent);
			XFlush(display);
			{
				Step step("pump after the container died", 30);
				pump(display, 300);
			}
			{
				Step step("gui->destroy (container already gone)");
				gui->destroy(gPlugin);
			}
		} else {
			{
				Step step("gui->destroy");
				gui->destroy(gPlugin);
			}
			XDestroyWindow(display, parent);
			XFlush(display);
		}
		{
			Step step("pump after destroy", 30);
			pump(display, 300);
		}

		// WHAT DOES A CLOSED EDITOR COST? The window still exists while closed — that is the fix — so
		// this measures whether it is also still burning CPU. The host stops driving us at guiDestroy
		// (the fd and timer are unregistered there), so the answer should be "nothing".
		if (idleMs > 0) {
			const double cpuBefore = cpuMs();
			const double wallBefore = nowMs();
			pump(display, idleMs);
			const double cpuIdle = cpuMs() - cpuBefore;
			const double wallIdle = nowMs() - wallBefore;
			std::printf("          closed for %.0f ms: %.0f ms of CPU (%.1f%% of one core)\n", wallIdle,
			            cpuIdle, wallIdle > 0 ? 100.0 * cpuIdle / wallIdle : 0.0);
		}

		std::printf("cycle %d: ok — editor windows inside the parent: %u, round trip %.0f ms\n", cycle,
		            count, nowMs() - started);
		if (count == 0) {
			std::printf("          ^ THE EDITOR CREATED NO WINDOW. This is the reopen failure.\n");
			++emptyRounds;
		}
	}

	std::printf("\nRESULT:   %d open/close cycles completed without blocking; %d left the container empty.\n",
	            cycles, emptyRounds);
	{
		Step step("plugin->destroy");
		gPlugin->destroy(gPlugin);
	}
	{
		Step step("entry->deinit");
		entry->deinit();
	}
	std::printf("          teardown returned too.\n");
	// AN EMPTY CONTAINER IS A FAILURE, not a line to be read: a round the host was told embedded, with nothing in it.
	return emptyRounds == 0 ? 0 : 4;
}
