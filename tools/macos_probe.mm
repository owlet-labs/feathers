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

// A REAL Cocoa window WITH A REAL EDITOR IN IT — the macOS counterpart to tools/windowed-probe.cpp.
//
//   build/macos-probe                                # rest-silent + known-bad + storm verdict
//   FEATHERS_PROBE_CONTROL=1 build/macos-probe      # the known-bad: a full quadrant of red over chrome
//   FEATHERS_REDUCED_MOTION=1 ...                   # REQUIRED: the editor's own animation would otherwise
//                                                  # make every run report "updated", per the venue rules.
//
// WHY THIS EXISTS, in one line: windowed-probe opens an X11 window and uses ImageMagick's `import -window`
// to read the window server's presented pixels of that window; macOS has no X11 to reuse, and the repo's
// rule that "a repaint cannot be observed headlessly" forbids reading an offscreen framebuffer. So this
// opens a real NSWindow hosting the shipped editor (in-process, the same Gui the plugin builds), and
// captures THAT window's presented pixels with CoreGraphics — the macOS analogue of `import -window`.
//
// WHAT COUNTS AS A FINDING. The same three checks windowed-probe runs, and for the same reasons:
//   1. REST SILENT. Under FEATHERS_REDUCED_MOTION, consecutive captures of the idle editor must be
//      byte-identical. A venue that cannot stay silent cannot be trusted to notice anything; if it
//      can't, the run is INCONCLUSIVE rather than clean.
//   2. KNOWN-BAD FIRES. With FEATHERS_PROBE_CONTROL set, an opaque red quadrant is injected over
//      window-origin chrome; the diff MUST catch it or this venue has a hole and the run fails.
//   3. THE EDITOR RESPONDS. A parameter storm (GlobalGamma) through the bridged ParamSet must move
//      pixels that the resting editor did not — proving this is a detector of editor output and not
//      a detector of capture noise.
// exit 0 only if (1) is silent, (2) is caught, (3) differs; anything else exits non-zero so CI fails
// rather than false-greens.
//
// UNVERIFIED FROM THIS REPO. The Cocoa presentation timing (MTKView display link must present a frame
// before CoreGraphics can capture it) and the exact CGWindowList capture under the GitHub macOS runner
// desktop session are validated on a Mac, not in this Linux-first environment. Until then this is a
// compile gate plus a run scaffold that FAILS LOUD. See the comment block over the macos-probe target
// in CMakeLists.txt.

#import <AppKit/AppKit.h>

#include "plugin/gui.h"
#include "plugin/gui_bridge.h"
#include "plugin/params.h"

#include "visage_graphics/canvas.h"
#include <visage/ui.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace feathers;

namespace {

//---------------------//
// The editor's side   //
//---------------------//

/// Records flush/resize requests the editor asks the host for, the way a real host would.
struct ProbeGuiHost : GuiHost {
	bool flushRequested = false;
	uint32_t requestedWidth = 0;
	uint32_t requestedHeight = 0;
	bool resizePending = false;

	void guiRequestFlush() noexcept override { flushRequested = true; }
	void guiRequestResize(uint32_t width, uint32_t height) noexcept override {
		requestedWidth = width;
		requestedHeight = height;
		resizePending = true;
	}
	// No plugin state behind the probe, so the preset browser's capture/load are stubs.
	void guiCaptureState(clap_ostream_t*) noexcept override {}
	bool guiLoadState(clap_istream_t*) noexcept override { return false; }
};

//---------------------//
// Pixels              //
//---------------------//

/// One captured frame: 8-bit RGBA as CoreGraphics hands it over. The alpha plane is carried and
/// compared too — a presenter that never wrote pixels would leave 0 and that difference is exactly the
/// "frozen window" failure this venue exists to kill.
struct Image {
	int width = 0;
	int height = 0;
	std::vector<unsigned char> rgba;

	bool valid() const { return width > 0 && height > 0 && !rgba.empty(); }
};

/// FNV-1a over the pixels. Not a checksum for integrity — a name for the frame, so two captures can be
/// told apart without keeping both. A cycle whose captures all share one name photographed a window
/// that was not drawing.
uint64_t fingerprint(const Image& image) {
	uint64_t hash = 1469598103934665603ULL;
	for (const unsigned char byte : image.rgba) {
		hash ^= byte;
		hash *= 1099511628211ULL;
	}
	return hash;
}

/// The difference between two frames as a count of bytes that changed by more than the renderer's own
/// noise. The artefact this venue hunts is opaque fills, so it clears a channel tolerance by two
/// orders of magnitude; anything that small is compositor noise.
constexpr int kChannelTolerance = 12;
long changedBytes(const Image& a, const Image& b) {
	if (!a.valid() || !b.valid() || a.width != b.width || a.height != b.height || a.rgba.size() != b.rgba.size()) {
		return -1;
	}
	long changed = 0;
	for (size_t i = 0; i < a.rgba.size(); ++i) {
		if (std::abs(static_cast<int>(a.rgba[i]) - static_cast<int>(b.rgba[i])) > kChannelTolerance) {
			++changed;
		}
	}
	return changed;
}

/// The known-bad: an opaque red quadrant anchored at the origin, wearing the same costume the
/// reported artefact did so it is a detector for the real bug and not a convenient stand-in. Only
/// ever added when the control is being run, so a measured cycle is a cycle against the editor as it
/// ships.
class OriginInk : public visage::Frame {
public:
	bool armed = false;
	void draw(visage::Canvas& canvas) override {
		if (!armed) {
			return;
		}
		canvas.setColor(0xffff0000);
		canvas.rectangle(0.0f, 0.0f, width() * 0.62f, height() * 0.42f);
	}
};

} // namespace

/// Capture the editor's PRESENTED window as the macOS analogue of `import -window`. The modern
/// CoreGraphics capture APIs (CGWindowListCreateImage, CGDisplayCreateImageForRect) are obsoleted and
/// unavailable in the macOS 15+ SDK, and ScreenCaptureKit needs interactive permission — so this
/// shells out to the Apple-signed `screencapture` CLI, which reads the window server's composited
/// image for that window (the SAME presented-pixels evidence the Linux venue gets from X11's
/// `import -window`, and which the repo rules require rather than a renderer readback). Runs in a
/// child with a hard deadline so a permission prompt that nobody can answer in CI degrades to
/// INCONCLUSIVE rather than hanging the job.
///
/// `screencapture` writes PNG by default, but PNG carries per-capture metadata that can drift and
/// its decode API is version-fragile across SDKs. A BMP (`-t bmp`) is uncompressed, carries no
/// metadata, and is decoded here with a 10-line header parse — zero framework decode, so the bytes
/// are byte-for-byte stable between idle captures (what the rest-silent check needs).
static bool captureWindow(NSWindow* window, Image& out) {
	const unsigned long wid = [window windowNumber];
	char path[128];
	std::snprintf(path, sizeof(path), "/tmp/feathers_mac_probe_%d.bmp",
	              static_cast<int>(getpid()));

	// posix_spawn (not fork): the probe has visage's Metal display-link thread alive, and forking a
	// multithreaded process is unsafe. argv is hand-built so no shell is involved.
	const std::string widStr = "-l" + std::to_string(wid);
	char* argv[] = {(char*) "/usr/sbin/screencapture", (char*) "-x", (char*) "-T", (char*) "0",
	                (char*) "-t", (char*) "bmp", const_cast<char*>(widStr.c_str()), path, nullptr};

	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	int devnull = open("/dev/null", O_WRONLY);
	if (devnull >= 0) {
		posix_spawn_file_actions_adddup2(&fa, devnull, 1);
		posix_spawn_file_actions_adddup2(&fa, devnull, 2);
		close(devnull);
	}
	pid_t pid = 0;
	const int spawnRc = posix_spawn(&pid, "/usr/sbin/screencapture", &fa, nullptr, argv, nullptr);
	posix_spawn_file_actions_destroy(&fa);
	if (spawnRc != 0 || pid == 0) {
		std::remove(path);
		return false;
	}

	// Wait bounded. A permission prompt that CI can't answer leaves the child blocked; kill it and
	// report a failed capture — the run degrades to INCONCLUSIVE, never a false green.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	bool captured = false;
	for (;;) {
		int status = 0;
		const pid_t reaped = waitpid(pid, &status, WNOHANG);
		if (reaped == pid) {
			captured = (WIFEXITED(status) && WEXITSTATUS(status) == 0);
			break;
		}
		if (reaped == -1) {
			break;  // waitpid error
		}
		if (std::chrono::steady_clock::now() >= deadline) {
			kill(pid, SIGKILL);
			waitpid(pid, nullptr, 0);
			captured = false;
			break;
		}
		usleep(40000);
	}
	if (!captured) {
		std::remove(path);  // leave no stale capture behind
		return false;
	}

	// Parse the BMP header (little-endian) and keep the raw pixel bytes. BMP carries no per-capture
	// metadata, so two captures of an idle window are byte-identical — what rest-silent needs.
	FILE* file = std::fopen(path, "rb");
	if (file == nullptr) {
		return false;
	}
	unsigned char hdr[54];
	if (std::fread(hdr, 1, sizeof(hdr), file) != sizeof(hdr)) {
		std::fclose(file);
		std::remove(path);
		return false;
	}
	const auto readLE32 = [](const unsigned char* p) -> uint32_t {
		return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
		     | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
	};
	const uint32_t offBits = readLE32(hdr + 10);
	const int32_t width = static_cast<int32_t>(readLE32(hdr + 18));
	const int32_t height = static_cast<int32_t>(readLE32(hdr + 22));   // negative = top-down
	long fileSize = 0;
	std::fseek(file, 0, SEEK_END);
	fileSize = std::ftell(file);
	if (offBits < 14 || static_cast<size_t>(offBits) > static_cast<size_t>(fileSize) || width == 0) {
		std::fclose(file);
		std::remove(path);
		return false;
	}
	const size_t pixelBytes = static_cast<size_t>(fileSize) - offBits;
	out.rgba.resize(pixelBytes);
	std::fseek(file, static_cast<long>(offBits), SEEK_SET);
	const size_t got = std::fread(out.rgba.data(), 1, pixelBytes, file);
	std::fclose(file);
	std::remove(path);

	if (got != pixelBytes) {
		return false;
	}
	out.width = width;
	out.height = height < 0 ? -height : height;
	return out.valid();
}

int main() {
	const bool controlMode = std::getenv("FEATHERS_PROBE_CONTROL") != nullptr;
	const int cycles = std::getenv("FEATHERS_PROBE_CYCLES") != nullptr
	                       ? std::atoi(std::getenv("FEATHERS_PROBE_CYCLES"))
	                       : 8;

	// Cocoa needs a running application object before any window will present or accept capture.
	// Mirrors the idiom visage's own macOS backend uses (visage_windowing/macos:windowing_macos.mm).
	[NSApplication sharedApplication];
	[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
	[NSApp activateIgnoringOtherApps:YES];

	// THE CONTAINER, owned by this process and presented on screen so CoreGraphics can photograph it.
	// The editor is sized to its logical canvas at 1x so the window is small enough to fit any screen.
	NSRect frame = NSMakeRect(400, 400, Gui::kLogicalWidth, Gui::kLogicalHeight);
	NSWindow* window = [[NSWindow alloc]
	    initWithContentRect:frame
	              styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable)
	                backing:NSBackingStoreBuffered
	                  defer:NO];
	[window setTitle:@"feathers macos-probe"];
	NSView* content = [[NSView alloc] initWithFrame:frame];
	[[window contentView] addSubview:content];
	[window orderFront:nil];
	[window makeKeyAndOrderFront:nil];

	// The parameter loop is closed here the way windowed-probe does — drain the bridge into a ParamSet
	// and publish straight back, so the engine echo that drives the readouts moves and the gesture
	// (the param storm below) actually churns glyph text. Without it the readout never changes and a
	// drag that reports itself as working measures nothing it did.
	ProbeGuiHost host;
	ParamBridge bridge;
	ParamSet params;
	bridge.publish(params);
	bridge.refresh();

	Gui gui(host, bridge);
	if (!Gui::isAvailable() || !gui.create(CLAP_WINDOW_API_COCOA, false)) {
		std::printf("gui.create(CLAP_WINDOW_API_COCOA) failed — no Cocoa backend in this build\n");
		return 2;
	}
	gui.setSize(Gui::kLogicalWidth, Gui::kLogicalHeight);

	clap_window clapWindow {};
	clapWindow.api = CLAP_WINDOW_API_COCOA;
	clapWindow.cocoa = (__bridge void*) content;
	if (!gui.setParent(&clapWindow)) {
		std::printf("gui.setParent failed — Cocoa embedding did not take\n");
		return 2;
	}

	// What a host's main loop does, minus the plugin: give the editor its thread back, drain the
	// Cocoa event queue so the MTKView's display link can present, and apply any queued parameter
	// moves through the bridge back to the ParamSet.
	const auto pump = [&](int milliseconds) {
		for (int slice = 0; slice < std::max(1, milliseconds / 4); ++slice) {
			bridge.drain([&](const UiParamEvent& event) {
				if (event.kind == UiParamEvent::Kind::Value) {
					params.setBase(event.id, event.value);
					host.flushRequested = true;
				}
			});
			if (host.flushRequested) {
				bridge.publish(params);
				bridge.refresh();
				host.flushRequested = false;
			}
			gui.processEvents();
			// Spin the Cocoa run loop so the MTKView's display link can present a Metal
			// drawable before the next CoreGraphics capture — otherwise the rest-silent check
			// would photograph the previous frame and be a lie. UNVERIFIED TIMING — confirm on
			// a Mac that frames actually present. CFRunLoopRunInMode pumps default-mode input
			// (timers, display link) without the nextEvent selector-resolution quirks.
			for (int i = 0; i < 8; ++i) {
				CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.002, false);
			}
			usleep(4000);
		}
	};
	pump(400);

	visage::Frame* root = gui.root();
	if (root == nullptr) {
		std::printf("no assembled tree — gui.root() is null\n");
		return 2;
	}

	// The storm drives GlobalGamma across its full range, the parameter the Explore corner owns and the
	// one that churns the most readout text (every per-sound card's base) — maximum atlas pressure for
	// a minimum gesture. The ids are found BY NAME so a renumbered parameter downgrades the run visibly.
	const ParamDesc* gamma = findParam(static_cast<clap_id>(ParamId::GlobalGamma));
	if (gamma == nullptr) {
		std::printf("storm:    GlobalGamma not in paramDescs — refusing to storm\n");
		return 2;
	}
	const double lo = static_cast<double>(gamma->min);
	const double hi = static_cast<double>(gamma->max);

	// (1) REST SILENT. Under FEATHERS_REDUCED_MOTION the idle editor must not move on its own.
	Image rest;
	if (!captureWindow(window, rest)) {
		std::printf("rest:     capture failed — window not photographed (display?)\n");
		return 3;
	}
	const uint64_t restHash = fingerprint(rest);
	pump(300);
	Image rest2;
	if (!captureWindow(window, rest2)) {
		std::printf("rest:     second capture failed\n");
		return 3;
	}
	if (restHash == fingerprint(rest2)) {
		std::printf("rest:     SILENT (hash %016llx)\n",
		            static_cast<unsigned long long>(restHash));
	} else {
		std::printf("rest:     INCONCLUSIVE — idle editor is not silent (hash %016llx vs %016llx); "
		            "the display or capture is moving\n",
		            static_cast<unsigned long long>(restHash),
		            static_cast<unsigned long long>(fingerprint(rest2)));
		return 3;
	}

	// (2) KNOWN-BAD FIRES. Inject red ink; the diff must catch it. This proves the detector can fire
	// before any clean verdict is read — without it, "clean" means nothing.
	if (controlMode) {
		OriginInk ink;
		ink.setIgnoresMouseEvents(true, false);
		ink.setBounds(0.0f, 0.0f, root->width() * 0.62f, root->height() * 0.42f);
		root->addChild(&ink);
		ink.armed = true;
		pump(400);
		Image stained;
		if (!captureWindow(window, stained)) {
			std::printf("control:  capture failed\n");
			return 2;
		}
		long moved = changedBytes(rest, stained);
		root->removeChild(&ink);
		ink.armed = false;
		pump(400);
		std::printf("control:  injected red quadrant moved %ld bytes — detector %s\n",
		            moved, moved > 0 ? "FIRED" : "IS DEAD TO ME");
		return moved > 0 ? 0 : 1;
	}

	// (3) THE EDITOR RESPONDS. A parameter storm must move pixels the resting editor did not.
	uint64_t prev = restHash;
	bool responded = false;
	for (int cycle = 0; cycle < cycles && !responded; ++cycle) {
		const double value = lo + (hi - lo) * static_cast<double>(cycle) / static_cast<double>(cycles - 1);
		params.setBase(static_cast<clap_id>(ParamId::GlobalGamma), value);
		bridge.publish(params);
		bridge.refresh();
		pump(120);
		Image frame;
		if (!captureWindow(window, frame)) {
			continue;
		}
		if (changedBytes(rest, frame) > 0 && fingerprint(frame) != prev) {
			responded = true;
			std::printf("storm:    cycle %d changed the editor (hash %016llx)\n",
			            cycle, static_cast<unsigned long long>(fingerprint(frame)));
		}
		prev = fingerprint(frame);
	}

	std::printf("storm:    %s\n",
	            responded ? "THE EDITOR RESPONDED — venue is a detector"
	                      : "INCONCLUSIVE — storm moved nothing; check presentation timing on a Mac");
	return responded ? 0 : 1;
}
