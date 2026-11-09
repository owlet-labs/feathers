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

// A minimal CLAP host that opens the editor and moves parameters from the ENGINE side, to see
// whether the editor notices without any mouse input.
//
//   build/guihost build/feathers.clap [output-directory]
//
// Originally the scratch harness written for the Tasks 1-2 spec review, vendored because it is the
// only place this can be tested at all.
//
// WHY THIS EXISTS RATHER THAN THE STANDALONE. clap-wrapper's StandaloneHost::param_request_flush()
// is an empty body (libs/clap-wrapper/src/detail/standalone/standalone_host.h:154), so a plugin
// asking the standalone for a parameter flush is asking nothing. Every path that depends on it — the
// editor moving a parameter while the plugin is inactive, and the engine handing values back — is
// silently dead there, and looks like the plugin doing nothing. The standalone is therefore not a
// venue for testing that seam, and the bug that motivated this tool survived precisely because the
// venue could not show it.
//
// HOW TO TRUST A RESULT FROM THIS. Run it against a known-bad binary as well and require the failure
// to reproduce. A pass on its own means very little: a blanked X display makes every capture return
// the same stale image, so a broken editor and a working one both report "no change", and a binary
// someone rebuilt in the meantime is no longer the control they think it is. Wake the display first
// with `xset -dpms s off dpms force on`.
//
// WHAT IT MISSED, AND WHY IT NOW OPENS THE EDITOR THE HARD WAY. This harness used to map its window
// before set_parent and never call show(), so it was a host that could not fail the way real hosts
// fail — and an editor that reported success only when it was already on screen sailed through here
// while never once opening in Bitwig. Both are fixed: the parent is mapped after set_parent, and
// show() is called and checked. For the rest of the protocol a real host performs, see
// tools/bitwighost.cpp.
//
// Needs the CLAP headers, libX11 and libdl to build, and ImageMagick's `import` on PATH to capture.

#include "host_common.h"

#include <clap/clap.h>

#include <X11/Xlib.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <algorithm>
#include <utility>
#include <dlfcn.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

//-----------------//
// The host's side //
//-----------------//

const clap_plugin* gPlugin = nullptr;
const clap_plugin_posix_fd_support* gFdExt = nullptr;
int gRegisteredFd = -1;
long gRequestFlushCalls = 0;
bool gFlushPending = false;

/// The whole point of the exercise: a host that actually answers request_flush. Setting the flag is
/// all that happens here — the main loop calls params.flush, because that is where a real host does
/// it and because calling into the plugin from inside its own callback would be a different test.
void hostRequestFlush(const clap_host*) {
	++gRequestFlushCalls;
	gFlushPending = true;
}
void hostRescan(const clap_host*, clap_param_rescan_flags) {}
void hostClear(const clap_host*, clap_id, clap_param_clear_flags) {}
const clap_host_params kHostParams = {hostRescan, hostClear, hostRequestFlush};

bool fdRegister(const clap_host*, int fd, clap_posix_fd_flags_t) {
	gRegisteredFd = fd;
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

void guiResizeHintsChanged(const clap_host*) {}

/// A size the editor has asked to become. Recorded here and applied from the main loop, because
/// that is what a host does — resizing from inside the plugin's own callback would be a different
/// thing entirely. Answering "true" and then doing nothing, which this used to do, is NOT what a
/// host does either: an editor that has to be resized before it can lay itself out correctly looks
/// broken here and fine everywhere else.
uint32_t gRequestedWidth = 0;
uint32_t gRequestedHeight = 0;
bool gResizePending = false;

bool guiRequestResize(const clap_host*, uint32_t width, uint32_t height) {
	gRequestedWidth = width;
	gRequestedHeight = height;
	gResizePending = true;
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
	if (std::strcmp(id, CLAP_EXT_GUI) == 0) {
		return &kHostGui;
	}
	return nullptr;
}
void hostNoop(const clap_host*) {}

clap_host gHost = {CLAP_VERSION_INIT, nullptr,  "feathers guihost", "Owlet Records", "",
                   "1.0",             getExtension, hostNoop,       hostNoop,        hostNoop};

//----------------//
// Event plumbing //
//----------------//

struct EventList {
	std::vector<clap_event_param_value> events;
	clap_input_events in{};

	void push(clap_id id, double value) {
		clap_event_param_value event{};
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

//----------//
// Captures //
//----------//

/// The editor's own window, not the one we handed the plugin. The plugin reparents its window into
/// ours, and capturing the parent can pick up the wrong thing.
Window captureTarget(Display* display, Window parent) {
	Window root = 0;
	Window par = 0;
	Window* kids = nullptr;
	unsigned int count = 0;
	Window target = parent;
	if (XQueryTree(display, parent, &root, &par, &kids, &count) != 0 && count > 0) {
		target = kids[0];
	}
	if (kids != nullptr) {
		XFree(kids);
	}
	return target;
}

// The blank-frame check lives in host_common.h — all three hosts photograph and all three were
// exposed, so it is shared rather than copied. See there for the measurement behind the threshold.
bool capture(Display* display, Window parent, const std::string& path) {
	char command[512];
	std::snprintf(command, sizeof(command), "import -window 0x%lx %s 2>/dev/null",
	              captureTarget(display, parent), path.c_str());
	if (std::system(command) != 0) {
		return false;
	}
	if (feathers::toolhost::captureIsUniform(path)) {
		std::printf("capture:  %s IS A SINGLE FLAT COLOUR — the window was not ready, and a blank"
		            " frame passes every other check this tool makes\n",
		            path.c_str());
		return false;
	}
	return true;
}

std::vector<unsigned char> readFile(const std::string& path) {
	std::vector<unsigned char> bytes;
	FILE* file = std::fopen(path.c_str(), "rb");
	if (file == nullptr) {
		return bytes;
	}
	unsigned char buffer[4096];
	size_t got = 0;
	while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
		bytes.insert(bytes.end(), buffer, buffer + got);
	}
	std::fclose(file);
	return bytes;
}

} // namespace

int main(int argc, char** argv) {
	// BEFORE ANYTHING LOADS — see `toolhost::isolateUserConfig`. A test host must not write
	// the files the installed plugin reads.
	feathers::toolhost::isolateUserConfig("guihost");
	if (argc < 2) {
		std::printf("usage: %s <path-to.clap> [output-directory]"
		            " [--set \"<param name>=<value>\"]..."
		            " [--device <tag> [--dset \"<control name>=<value>\"]...]..."
		            " [--size <W>x<H>] [--shot <name>] [--state <saved state file>] [--play <seconds>]\n",
		            argv[0]);
		return 2;
	}

	// --set PUTS THE EDITOR IN A STATE SO IT CAN BE PHOTOGRAPHED THERE, which is the gap this tool
	// had: it could prove the editor NOTICES the engine, and it could hold the window open for a
	// hand, but it could not be told "show me the filter card with a bell selected" and take the
	// picture itself. Five arcs have now turned up faults that only a camera could see, two of them
	// overlaps that every geometry case passed, so a state nobody can photograph is a state nobody
	// checks.
	//
	// BY NAME, NOT BY ID, which is this file's own rule and worth keeping at the door: it deliberately
	// knows nothing about the plugin's headers, and a hard-coded id that silently moves is a run that
	// proves nothing about the parameter it claims to be moving. A name that no longer exists is
	// reported and refused rather than quietly setting nothing.
	//
	// With any --set given, the scripted engine-to-editor round trip below is SKIPPED: that section
	// moves Path, Relief and Level to make a difference visible, which would fight the state being
	// asked for and photograph something nobody requested.
	std::vector<std::pair<std::string, double>> wanted;
	// --device PLACES A DEVICE IN THE FX LANE AND --dset SETS ONE OF ITS CONTROLS, which is the half
	// --set cannot reach: a device's controls are SWALLOWED rather than CLAP parameters, so no amount
	// of clap_plugin_params gets at one. Until this, no device surface had ever been rendered by any
	// machine -- six cards ship and every one of them had been seen by human eyes alone.
	//
	// --dset ATTACHES TO THE MOST RECENT --device, so a lane of two devices reads left to right and
	// needs no slot arithmetic at the door. A --dset with no --device before it is refused rather
	// than guessed at.
	std::vector<feathers::toolhost::SeedRequest> seeds;
	// --list-devices PRINTS THE PLACEABLE TAGS AND EXITS, one per line, so a script can drive one
	// run per device without anybody typing a list. It is deliberately not a photographing mode:
	// one run photographs one state, and a mode that quietly took six pictures would be a different
	// tool wearing this one's name.
	bool listDevices = false;
	/// THE SIZE `--size` ASKED FOR, or zero meaning "whatever the editor's own `get_size` says".
	uint32_t openWidth = 0;
	uint32_t openHeight = 0;
	std::string shotName = "state";
	std::string stateFile;
	/// `--play <seconds>`: SOUND IN THE PICTURE. This host never activated the plugin, so every meter, ring and scope
	/// said "no signal". With it, the plugin is activated and processed for that long, block by block on this thread
	/// between the editor's pumps, holding a chord (A2 E3 A3 C#4, struck again every two seconds), with the same chord
	/// at any audio input; it is left active, still sounding, for the capture. Test-only: a real host would process on
	/// its own audio thread.
	double playSeconds = 0.0;
	std::vector<std::string> positional;
	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		if (arg == "--set" && i + 1 < argc) {
			const std::string pair = argv[++i];
			const size_t eq = pair.find('=');
			if (eq == std::string::npos) {
				std::printf("--set wants \"<param name>=<value>\", got \"%s\"\n", pair.c_str());
				return 2;
			}
			wanted.emplace_back(pair.substr(0, eq), std::atof(pair.c_str() + eq + 1));
			continue;
		}
		// **OPEN THE EDITOR AT A STATED SIZE — which is a thing a host does, and the only way to
		// photograph what an editor looks like when it is SMALL.**
		//
		// The rack's `guiCanResize` returns true with no constraint of our own, so a player can make
		// the window short; its lane then grants each card `min(kCardUnitHeight, laneHeight)` and a
		// card can end up too short to hold the rows of output chips its device publishes. That is a
		// real state with a rule attached — an output that cannot be drawn inside its card is hidden
		// rather than drawn outside it — and no capture could reach it, because this host opened at
		// whatever `get_size` answered and then only ever shrank by 40px. A geometry case can assert
		// the rule; only a picture can show what it looks like.
		if (arg == "--size" && i + 1 < argc) {
			const std::string spec = argv[++i];
			const size_t x = spec.find('x');
			if (x == std::string::npos) {
				std::printf("--size wants WxH, got \"%s\"\n", spec.c_str());
				return 2;
			}
			// **PARSED STRICTLY, BECAUSE `atoi` ACCEPTS `-5x10` AND HANDS BACK 4294967291.** It has no
			// way to report a refusal and a negative width becomes an enormous unsigned one, which is
			// a window request nobody meant and an X error a long way from the typo that caused it.
			// `strtoul` with its end pointer is the version that can say no.
			const auto number = [](const char* from, const char* end, uint32_t* into) {
				if (*from == '-' || *from == '+') {
					return false; // a size is a magnitude; a sign here is always a typo
				}
				char* stop = nullptr;
				const unsigned long value = std::strtoul(from, &stop, 10);
				return stop == end && stop != from && value > 0 && value <= 16384
				       && (*into = static_cast<uint32_t>(value), true);
			};
			const char* spec_c = spec.c_str();
			if (!number(spec_c, spec_c + x, &openWidth)
			    || !number(spec_c + x + 1, spec_c + spec.size(), &openHeight)) {
				std::printf("--size wants two positive numbers as WxH (max 16384), got \"%s\"\n",
				            spec.c_str());
				return 2;
			}
			continue;
		}
		if (arg == "--list-devices") {
			listDevices = true;
			continue;
		}
		if (arg == "--device" && i + 1 < argc) {
			seeds.push_back(feathers::toolhost::SeedRequest{argv[++i], {}});
			continue;
		}
		if (arg == "--dset" && i + 1 < argc) {
			const std::string pair = argv[++i];
			const size_t eq = pair.find('=');
			if (eq == std::string::npos) {
				std::printf("--dset wants \"<control name>=<value>\", got \"%s\"\n", pair.c_str());
				return 2;
			}
			if (seeds.empty()) {
				std::printf("--dset \"%s\" has no --device before it to apply to\n", pair.c_str());
				return 2;
			}
			seeds.back().controls.emplace_back(pair.substr(0, eq), std::atof(pair.c_str() + eq + 1));
			continue;
		}
		if (arg == "--shot" && i + 1 < argc) {
			shotName = argv[++i];
			continue;
		}
		// --state LOADS A SAVED STATE FILE (the plugin's own `state.load`) before the editor opens: the one way to photograph a
		// state that is patch data and no parameter, such as a folded Macros column (backlog QN).
		if (arg == "--state" && i + 1 < argc) {
			stateFile = argv[++i];
			continue;
		}
		if (arg == "--play" && i + 1 < argc) {
			playSeconds = std::max(0.0, std::atof(argv[++i]));
			continue;
		}
		// AN UNRECOGNISED --flag IS AN ERROR, NOT A DIRECTORY. This fell through to `positional`,
		// so `--out somewhere` made "--out" the output directory, every `import` wrote into a path
		// that does not exist, and the run ended "INCONCLUSIVE - no captures. Is `import` on PATH?"
		// -- pointing at the one thing that was not wrong. A typo has to say it was a typo.
		if (arg.rfind("--", 0) == 0) {
			std::printf("unknown option \"%s\"\n", arg.c_str());
			return 2;
		}
		positional.push_back(arg);
	}
	// Same failure from the other side: a stray word silently became the output directory.
	if (positional.size() > 2) {
		std::printf("expected at most <path-to.clap> and an output directory, got %zu positionals"
		            " (\"%s\" was unexpected)\n",
		            positional.size(), positional[2].c_str());
		return 2;
	}
	if (positional.empty()) {
		std::printf("no plugin path given\n");
		return 2;
	}
	const std::string pluginPath = positional[0];
	const std::string outDir = positional.size() > 1 ? positional[1] : ".";
	const std::string beforePath = outDir + "/guihost_before.png";
	const std::string afterPath = outDir + "/guihost_after.png";
	const std::string resizedPath = outDir + "/guihost_resized.png";

	XInitThreads();

	// THE LOAD DANCE IS SHARED — see tools/host_common.h, which keeps this host's granular reporting
	// and gives the other two the same.
	const feathers::toolhost::Loaded loaded = feathers::toolhost::loadPlugin(pluginPath.c_str(), &gHost);
	if (!loaded) {
		return 1;
	}
	std::printf("plugin:   %s\n", loaded.descriptor->id);
	const clap_plugin_entry* entry = loaded.entry;
	gPlugin = loaded.plugin;

	auto* gui = static_cast<const clap_plugin_gui*>(gPlugin->get_extension(gPlugin, CLAP_EXT_GUI));
	auto* params =
	    static_cast<const clap_plugin_params*>(gPlugin->get_extension(gPlugin, CLAP_EXT_PARAMS));
	gFdExt = static_cast<const clap_plugin_posix_fd_support*>(
	    gPlugin->get_extension(gPlugin, CLAP_EXT_POSIX_FD_SUPPORT));
	if (gui == nullptr || params == nullptr) {
		std::printf("plugin has no gui or params extension — nothing to test\n");
		return 1;
	}

	if (listDevices) {
		const std::vector<std::string> tags = feathers::toolhost::availableDevices(gPlugin);
		if (tags.empty()) {
			std::printf("plugin offers no placeable devices (or no %s)\n",
			            CLAP_EXT_FEATHERS_TEST_CHAIN);
			return 3;
		}
		for (const std::string& tag : tags) {
			std::printf("%s\n", tag.c_str());
		}
		return 0;
	}

	// SEEDED BEFORE THE EDITOR IS CREATED, so the editor lays itself out with the device already in
	// the lane rather than having to notice one arriving. A refusal is fatal: photographing a lane
	// that is not what was asked for is the exact failure this seam exists to prevent, and it is
	// indistinguishable from success in the picture.
	if (!feathers::toolhost::seedFxChain(gPlugin, seeds)) {
		std::printf("seeding failed — refusing to photograph\n");
		return 3;
	}
	if (!stateFile.empty()) {
		std::ifstream file(stateFile, std::ios::binary);
		std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		const auto* state = static_cast<const clap_plugin_state*>(gPlugin->get_extension(gPlugin, CLAP_EXT_STATE));
		struct Reader {
			const std::vector<char>* bytes;
			size_t at = 0;
		} reader { &bytes };
		clap_istream in { &reader, [](const clap_istream* s, void* buffer, uint64_t size) -> int64_t {
			                 auto* r = static_cast<Reader*>(s->ctx);
			                 const size_t take = std::min<size_t>(static_cast<size_t>(size), r->bytes->size() - r->at);
			                 std::memcpy(buffer, r->bytes->data() + r->at, take);
			                 r->at += take;
			                 return static_cast<int64_t>(take);
		                 } };
		if (bytes.empty() || state == nullptr || !state->load(gPlugin, &in)) {
			std::printf("--state %s: not loaded — refusing to photograph\n", stateFile.c_str());
			return 3;
		}
		std::printf("state:    %s (%zu bytes)\n", stateFile.c_str(), bytes.size());
	}

	if (!gui->create(gPlugin, CLAP_WINDOW_API_X11, false)) {
		std::printf("gui->create failed (no X11 backend, or the host offered no way to drive it)\n");
		return 1;
	}

	Display* display = XOpenDisplay(nullptr);
	if (display == nullptr) {
		std::printf("XOpenDisplay failed — is DISPLAY set?\n");
		return 1;
	}

	uint32_t width = 0;
	uint32_t height = 0;
	gui->get_size(gPlugin, &width, &height);
	// The parent is created at the size the EDITOR asked for, never at `--size`. A `--size` request is
	// applied after `show`, by resizing this window — see the block there, which carries the
	// measurement (backlog FU). An earlier comment stood here claiming the opposite about `set_size`;
	// it outlived the code it described, at the exact spot the bug it misdescribed lived.
	const int screen = DefaultScreen(display);
	Window parent = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0, width, height, 0,
	                                    BlackPixel(display, screen), BlackPixel(display, screen));
	XFlush(display);

	// The parent stays UNMAPPED across set_parent, because that is what real hosts do and this
	// harness spent a release cycle not knowing it. Mapping first makes the plugin's window
	// IsViewable the instant it is reparented, which quietly excuses a plugin that decides whether
	// the embed worked by asking X whether it is on screen. Bitwig maps its container afterwards,
	// so that plugin fails there and passes here — which is exactly what happened, and the editor
	// never opened in Bitwig while every check we owned was green.
	clap_window window{};
	window.api = CLAP_WINDOW_API_X11;
	window.x11 = parent;
	if (!gui->set_parent(gPlugin, &window)) {
		std::printf("gui->set_parent failed\n");
		// Torn down rather than left to the process exit: this is the path the known-bad control takes,
		// and an editor whose timer thread is still running while statics unwind turns a clean verdict
		// into a core dump that reads like a second, unrelated failure.
		gui->destroy(gPlugin);
		return 1;
	}

	// Only now, the way a host does it.
	XMapWindow(display, parent);
	// Raised and focused so the capture reads live pixels rather than whatever was stacked above.
	XRaiseWindow(display, parent);
	XSetInputFocus(display, parent, RevertToParent, CurrentTime);
	XFlush(display);

	// The documented CLAP sequence ends here. Bitwig never calls it, which is precisely why this
	// harness should: nobody else is checking.
	if (!gui->show(gPlugin)) {
		std::printf("gui->show failed\n");
		return 1;
	}

	// Honour a resize the editor asked for, the way a host does. An editor that needs a different
	// size than the one it was opened at — because it has just worked out the display's scale, say —
	// cannot lay itself out correctly until somebody acts on that, and a harness that always answers
	// "yes" and never resizes is a harness where such an editor looks broken for reasons of our own
	// making.
	if (gResizePending) {
		gResizePending = false;
		std::printf("editor:   asked for %ux%u, honouring it\n", gRequestedWidth, gRequestedHeight);
		width = gRequestedWidth;
		height = gRequestedHeight;
		XResizeWindow(display, parent, width, height);
		XSync(display, False);
		gui->set_size(gPlugin, width, height);
	}
	// **`--size` IS APPLIED HERE, AFTER `show`, AND BY RESIZING THE X WINDOW — NOT BY `set_size`
	// ALONE.** MEASURED: `set_size` before the window exists RETURNS TRUE AND DOES NOTHING; the editor
	// went on laying itself out at 720x594 while the X window was 720x196, so the capture was the
	// top-left corner of a larger picture. Every measurement taken off such a frame is wrong in the
	// most convincing way available — a card whose foot is simply out of frame reads exactly like a
	// card that is not drawing its foot. It cost a confident wrong reading before it was caught, by
	// reading `get_size` back instead of trusting the return value — **backlog FU**, which also lists
	// what any capture taken before 2026-09-19 with a size flag is actually a picture of.
	//
	// **AND EVEN WHEN THE SIZE TAKES, THE RACK EDITOR CLIPS RATHER THAN RE-LAYS (backlog FT)**: it
	// reports the short size and keeps its design height, so a small-window capture of THAT editor is
	// still a crop. Read `get_size` to know the editor's size; that is not the same as knowing it
	// re-laid out.
	//
	// **THE X RESIZE IS WHAT THE EDITOR ACTUALLY OBSERVES** (the harness's own late resize below has
	// always worked this way), and `set_size` is sent with it because the CLAP contract says a host
	// that resizes tells the plugin. Both, then read back, then report what is really in force.
	// Applied below, once `pump` exists — the editor has to be given its thread back before it can
	// act on the configure, and nothing may be measured until it has.
	std::printf("editor:   open in window 0x%lx at %ux%u, plugin fd = %d\n", parent, width, height,
	            gRegisteredFd);

	clap_input_events emptyIn{};
	emptyIn.ctx = nullptr;
	emptyIn.size = [](const clap_input_events*) -> uint32_t { return 0; };
	emptyIn.get = [](const clap_input_events*, uint32_t) -> const clap_event_header* {
		return nullptr;
	};
	clap_output_events sink{};
	sink.ctx = nullptr;
	sink.try_push = outPush;

	// What a host's main loop does: give the plugin its thread back, and answer request_flush.
	auto pump = [&](int milliseconds) {
		for (int i = 0; i < milliseconds / 5; ++i) {
			if (gRegisteredFd >= 0 && gFdExt != nullptr) {
				gFdExt->on_fd(gPlugin, gRegisteredFd, CLAP_POSIX_FD_READ);
			}
			if (gFlushPending) {
				gFlushPending = false;
				params->flush(gPlugin, &emptyIn, &sink);
			}
			while (XPending(display) > 0) {
				XEvent event;
				XNextEvent(display, &event);
			}
			usleep(5000);
		}
	};

	if (openWidth != 0 && openHeight != 0) {
		XResizeWindow(display, parent, openWidth, openHeight);
		XSync(display, False);
		const bool took = gui->set_size(gPlugin, openWidth, openHeight);
		pump(600); // let the editor see the configure and lay itself out before anything is measured
		gui->get_size(gPlugin, &width, &height);
		std::printf("editor:   --size %ux%u %s, laying out at %ux%u\n", openWidth, openHeight,
		            took ? "accepted" : "REFUSED", width, height);
		if (width != openWidth || height != openHeight) {
			std::printf("editor:   THE EDITOR IS NOT THE SIZE ASKED FOR — anything measured off this"
			            " capture is about a %ux%u editor cropped to a %ux%u window\n",
			            width, height, openWidth, openHeight);
		}
	}

	pump(1500);

	if (playSeconds > 0.0) {
		constexpr double kRate = 48000.0;
		constexpr uint32_t kBlock = 256;
		if (!gPlugin->activate(gPlugin, kRate, 1, kBlock) || !gPlugin->start_processing(gPlugin)) {
			std::printf("--play: the plugin would not activate — refusing to photograph\n");
			return 3;
		}
		const auto* ports = static_cast<const clap_plugin_audio_ports*>(gPlugin->get_extension(gPlugin, CLAP_EXT_AUDIO_PORTS));
		// ONE STEREO BUFFER PAIR PER PORT, each port's channel count as it declares it.
		struct Port {
			std::vector<std::vector<float>> channels;
			std::vector<float*> pointers;
			clap_audio_buffer buffer {};
		};
		const auto makePorts = [&](bool input) {
			std::vector<Port> made(ports != nullptr ? ports->count(gPlugin, input) : 0);
			for (uint32_t p = 0; p < made.size(); ++p) {
				clap_audio_port_info info {};
				(void)ports->get(gPlugin, p, input, &info);
				made[p].channels.assign(std::max<uint32_t>(info.channel_count, 1), std::vector<float>(kBlock, 0.0f));
			}
			for (Port& port : made) {
				for (std::vector<float>& c : port.channels) {
					port.pointers.push_back(c.data());
				}
				port.buffer.data32 = port.pointers.data();
				port.buffer.channel_count = static_cast<uint32_t>(port.pointers.size());
			}
			return made;
		};
		std::vector<Port> ins = makePorts(true);
		std::vector<Port> outs = makePorts(false);
		std::vector<clap_audio_buffer> inBuffers;
		std::vector<clap_audio_buffer> outBuffers;
		for (Port& port : ins) {
			inBuffers.push_back(port.buffer);
		}
		for (Port& port : outs) {
			outBuffers.push_back(port.buffer);
		}
		struct Notes {
			std::vector<clap_event_note> events;
		} notes;
		clap_input_events noteIn {};
		noteIn.ctx = &notes;
		noteIn.size = [](const clap_input_events* l) -> uint32_t {
			return static_cast<uint32_t>(static_cast<Notes*>(l->ctx)->events.size());
		};
		noteIn.get = [](const clap_input_events* l, uint32_t i) -> const clap_event_header* {
			return &static_cast<Notes*>(l->ctx)->events[i].header;
		};
		const int16_t chord[] = { 45, 52, 57, 61 };
		const double freqs[] = { 110.0, 164.81, 220.0, 277.18 };
		double phase[4] = {};
		const auto note = [&](uint16_t type, int16_t key) {
			clap_event_note e {};
			e.header.size = sizeof(e);
			e.header.time = 0;
			e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
			e.header.type = type;
			e.note_id = -1;
			e.port_index = 0;
			e.channel = 0;
			e.key = key;
			e.velocity = 0.8;
			notes.events.push_back(e);
		};
		const auto blocks = static_cast<long>(playSeconds * kRate / kBlock);
		const long strike = static_cast<long>(2.0 * kRate / kBlock);
		int64_t steady = 0;
		for (long b = 0; b < blocks; ++b) {
			notes.events.clear();
			if (b % strike == 0) {
				if (b > 0) {
					for (const int16_t k : chord) {
						note(CLAP_EVENT_NOTE_OFF, k);
					}
				}
				for (const int16_t k : chord) {
					note(CLAP_EVENT_NOTE_ON, k);
				}
			}
			for (Port& port : ins) {
				for (uint32_t i = 0; i < kBlock; ++i) {
					double sum = 0.0;
					for (int v = 0; v < 4; ++v) {
						phase[v] += freqs[v] / kRate;
						phase[v] -= std::floor(phase[v]);
						sum += 2.0 * phase[v] - 1.0;
					}
					for (std::vector<float>& c : port.channels) {
						c[i] = static_cast<float>(0.15 * sum);
					}
				}
			}
			clap_process process {};
			process.steady_time = steady;
			process.frames_count = kBlock;
			process.transport = nullptr;
			process.audio_inputs = inBuffers.empty() ? nullptr : inBuffers.data();
			process.audio_inputs_count = static_cast<uint32_t>(inBuffers.size());
			process.audio_outputs = outBuffers.empty() ? nullptr : outBuffers.data();
			process.audio_outputs_count = static_cast<uint32_t>(outBuffers.size());
			process.in_events = &noteIn;
			process.out_events = &sink;
			(void)gPlugin->process(gPlugin, &process);
			steady += kBlock;
			// THE EDITOR'S THREAD BACK EVERY FEW BLOCKS, at no more than real time, so its meters and scopes see the
			// signal as it goes rather than only where it ended. NO FLUSH while processing: the events go in `process`.
			if (b % 4 == 3) {
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
		gFlushPending = false;
		std::printf("play:     %.2f s processed (%ld blocks of %u), %zu input and %zu output port(s), chord held\n",
		            static_cast<double>(steady) / kRate, blocks, kBlock, ins.size(), outs.size());
	}

	bool holdOnly = false;
	double readBack = 0.0;
	// THE REQUESTED STATE, POSTED AND PHOTOGRAPHED, and then nothing else — see the note at --set.
	if (!wanted.empty() || !seeds.empty()) {
		EventList asked;
		int found = 0;
		for (const auto& [name, value] : wanted) {
			clap_id id = CLAP_INVALID_ID;
			const uint32_t count = params->count(gPlugin);
			for (uint32_t i = 0; i < count; ++i) {
				clap_param_info info{};
				if (params->get_info(gPlugin, i, &info) && name == info.name) {
					id = info.id;
					break;
				}
			}
			if (id == CLAP_INVALID_ID) {
				std::printf("no parameter named \"%s\" — refusing rather than setting nothing\n",
				            name.c_str());
				return 1;
			}
			std::printf("set:      %-20s (0x%04X) -> %g\n", name.c_str(), id, value);
			asked.push(id, value);
			found++;
		}
		asked.in.ctx = &asked;
		asked.in.size = evSize;
		asked.in.get = evGet;
		clap_output_events askedOut{};
		askedOut.ctx = nullptr;
		askedOut.try_push = outPush;
		params->flush(gPlugin, &asked.in, &askedOut);

		// Long enough for the editor's own snapshot to reach the glass — the same wait the scripted
		// section uses before it believes what it sees.
		pump(1500);
		const std::string shotPath = outDir + "/guihost_" + shotName + ".png";
		capture(display, parent, shotPath);
		std::printf("\nRESULT:   %d parameter(s) posted, %zu device(s) seeded, editor photographed\n",
		            found, seeds.size());
		std::printf("          capture: %s\n", shotPath.c_str());
		// FALLS THROUGH TO THE HOLD BELOW rather than exiting, when one is asked for. A state posted
		// through --set and a state reached by CLICKING are not alternatives: folding a card is not a
		// parameter and never will be, so photographing a folded card that also has a bell selected
		// needs both doors open at once. Without a hold, this is where the run ends.
		if (std::getenv("FEATHERS_GUIHOST_HOLD") == nullptr) {
			gui->destroy(gPlugin);
			return 0;
		}
		std::printf("          holding open for a driver; window is 0x%lx\n", parent);
		holdOnly = true;
	}

	if (!holdOnly) {
	capture(display, parent, beforePath);

	// ENGINE SIDE: move three parameters through params.flush, exactly as host automation or a preset
	// load would while the plugin is inactive. No mouse input at all.
	EventList list;
	list.push(0x0030, 1.0); // Path    -> maximum
	list.push(0x0040, 1.0); // Relief  -> maximum
	list.push(0x0052, 0.0); // Level   -> minimum, so one row moves the other way
	list.in.ctx = &list;
	list.in.size = evSize;
	list.in.get = evGet;
	clap_output_events out{};
	out.ctx = nullptr;
	out.try_push = outPush;
	params->flush(gPlugin, &list.in, &out);

	pump(1500);
	capture(display, parent, afterPath);

	params->get_value(gPlugin, 0x0030, &readBack);

	// A resize is the one thing that dirties the whole editor, so it separates "the editor never
	// looks at the engine" from "the editor looks but does not repaint".
	XResizeWindow(display, parent, width - 40, height - 40);
	XFlush(display);
	pump(1500);
	capture(display, parent, resizedPath);
	} // !holdOnly — a --set run has already taken its picture and wants the window left alone

	// HOLD OPEN, so somebody can drive the editor while it is up.
	//
	// Everything above this line is a scripted round trip that finishes in a few seconds, which is the
	// right shape for the seam it was built to test and the wrong shape for anything involving a HAND.
	// A drag storm against a window that has already exited is not a negative result, it is no result —
	// and two of them were run before this existed, which is the reason it does now.
	//
	// FEATHERS_GUIHOST_HOLD=<seconds> keeps pumping after the captures, so an external driver
	// (xdotool, a human) has a live editor to work on. The window id is printed above; the pump is the
	// same one the scripted section uses, so the plugin sees exactly the host it always sees.
	// FEATHERS_GUIHOST_CHAOS=<seconds> streams AUTOMATION the way a host streams it, which is the one
	// state this harness could not reach before and the one reported from: with weather enabled and
	// position automated, the explore view's breadcrumbs went out of the circle and line and
	// polygon glitches appeared.
	//
	// Weather up, then Path, Pos X and Pos Y on incommensurate LFOs pushed through params.flush every
	// frame-ish slice — so the star and every trail leg are dragged across the view and out of it,
	// continuously, with the geometry never repeating. Captures periodically so a glitch that lasts one
	// frame is still on disk afterwards.
	//
	// The ids are DISCOVERED BY NAME rather than written as constants: this file deliberately knows
	// nothing about the plugin's headers, and a hard-coded id that silently moves is a run that proves
	// nothing about the parameter it claims to be moving.
	if (const char* chaosFor = std::getenv("FEATHERS_GUIHOST_CHAOS")) {
		const int seconds = std::atoi(chaosFor);
		clap_id pathId = CLAP_INVALID_ID;
		clap_id posXId = CLAP_INVALID_ID;
		clap_id posYId = CLAP_INVALID_ID;
		clap_id weatherId = CLAP_INVALID_ID;
		// GLOBAL GAMMA IS CHURNED TOO, and it is a different kind of load from the other four.
		// Path/Pos X/Pos Y move the geometry; Global Gamma is a macro over every sound's character, so
		// changing it re-renders every card's readouts — the text and the numbers, not just the
		// positions. That is the maximum-turnover case for whatever the cards allocate per frame, and
		// it is the state the screencast of the remaining glitch was taken in.
		//
		// OPTIONAL, unlike the other four: a run without it is still the old chaos recipe and still
		// worth having, so a missing id downgrades the run rather than skipping it.
		//
		// RUN AND SEEN TO WORK, 30 s on :1 against the Perform surface: the header reports
		// "+ Global Gamma churn" rather than the not-found downgrade, 30 captures were written, and
		// with VISAGE_TRACE_BATCH on there was not one SHORT, UNWALKED, DROPPED, OVERFLOW or SPLIT
		// line in the whole run. It was committed UNVERIFIED because the display was asleep at the
		// time — see scripts/venue-preflight.sh, which exists so that never reads as a wedged venue
		// again.
		//
		// WHAT THAT RUN DOES NOT SETTLE, so nobody reads it as an acquittal: it churns GLOBAL gamma
		// only, and the frames the glitch was captured in show the PER-SOUND gammas driven to the
		// rail as well (A Gamma -4.00 beside Global Gamma -3.66). It also ran in guihost at one
		// window size while the glitch was reported from Bitwig at another, and this project has
		// already had one bug that was venue- and size-dependent.
		clap_id gammaId = CLAP_INVALID_ID;

		// AND THE PER-SOUND GAMMAS, because the frames the glitch was captured in show those at the
		// rail too — A Gamma -4.00 sitting beside Global Gamma -3.66 — and a run that moved only the
		// macro was reproducing half the state it was aiming at. Eight of them, each on its own
		// incommensurate rate, so the cards never rebuild in step with one another: a rail where all
		// eight readouts change together is one repaint, and a rail where they change at eight
		// different moments is eight, which is the damage pattern that multiplies pieces.
		clap_id soundGammaIds[8];
		for (clap_id& id : soundGammaIds) {
			id = CLAP_INVALID_ID;
		}
		const uint32_t count = params->count(gPlugin);
		for (uint32_t i = 0; i < count; ++i) {
			clap_param_info info{};
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
		if (seconds > 0 && pathId != CLAP_INVALID_ID && posXId != CLAP_INVALID_ID
		    && posYId != CLAP_INVALID_ID && weatherId != CLAP_INVALID_ID) {
			int soundGammas = 0;
			for (const clap_id id : soundGammaIds) {
				soundGammas += id != CLAP_INVALID_ID ? 1 : 0;
			}
			// The header says WHAT WAS ACTUALLY CHURNED rather than what was intended, because a run
			// that silently moved fewer parameters than it meant to is the shape of every invalid
			// storm this project has recorded.
			std::printf("chaos:    %d s of automated position at Weather 0.6%s, %d per-sound gamma(s)"
			            " — window 0x%lx\n",
			            seconds,
			            gammaId == CLAP_INVALID_ID ? " (no Global Gamma found — not churning it)"
			                                       : " + Global Gamma churn",
			            soundGammas, static_cast<unsigned long>(captureTarget(display, parent)));
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
					// Fast and full-range (-4..4), so the displayed number changes every slice rather
					// than settling — a readout that reads the same twice does not rebuild, and a card
					// that does not rebuild is not the load this is here to apply.
					moving.push(gammaId, 4.0 * std::sin(t * 2.0 * 3.14159265358979 * 1.7));
				}
				for (int sound = 0; sound < 8; ++sound) {
					if (soundGammaIds[sound] == CLAP_INVALID_ID) {
						continue;
					}
					// Rates that share no small common multiple, so the eight cards rebuild at eight
					// different moments rather than together.
					const double rate = 0.7 + 0.37 * sound;
					moving.push(soundGammaIds[sound],
					            4.0 * std::sin(t * 2.0 * 3.14159265358979 * rate + sound));
				}
				moving.in.ctx = &moving;
				moving.in.size = evSize;
				moving.in.get = evGet;
				clap_output_events discard{};
				discard.ctx = nullptr;
				discard.try_push = outPush;
				params->flush(gPlugin, &moving.in, &discard);
				pump(16);

				if (slice % 60 == 0) {
					char path[512] = {};
					std::snprintf(path, sizeof(path), "%s/chaos_%03d.png", outDir.c_str(), slice / 60);
					capture(display, parent, path);
				}
			}
			std::printf("chaos:    done, %d captures in %s\n", slices / 60, outDir.c_str());
			std::fflush(stdout);
		}
		else {
			std::printf("chaos:    SKIPPED — could not find Path/Pos X/Pos Y/Weather by name\n");
		}
	}

	if (const char* held = std::getenv("FEATHERS_GUIHOST_HOLD")) {
		const int seconds = std::atoi(held);
		if (seconds > 0) {
			std::printf("holding:  %d s with the editor live — window 0x%lx\n", seconds,
			            static_cast<unsigned long>(captureTarget(display, parent)));
			std::fflush(stdout);
			pump(seconds * 1000);
			std::printf("holding:  done\n");
			std::fflush(stdout);
		}
	}

	if (holdOnly) {
		// The verdict below reads the scripted round trip's three captures, which a --set run never
		// took. It has already printed its own result; anything more here would be a report about
		// files that were deliberately not written.
		gui->destroy(gPlugin);
		if (playSeconds > 0.0) { // --play left it active and processing, as a host mid-song would
			gPlugin->stop_processing(gPlugin);
			gPlugin->deactivate(gPlugin);
		}
		gPlugin->destroy(gPlugin);
		entry->deinit();
		return 0;
	}

	const std::vector<unsigned char> before = readFile(beforePath);
	const std::vector<unsigned char> after = readFile(afterPath);
	const std::vector<unsigned char> resized = readFile(resizedPath);

	std::printf("engine:   Path reads %.3f after the flush\n", readBack);
	std::printf("flushes:  host answered %ld request_flush call(s)\n", gRequestFlushCalls);

	if (before.empty() || after.empty()) {
		std::printf("\nRESULT:   INCONCLUSIVE — no captures. Is `import` on PATH?\n");
		gui->destroy(gPlugin);
		if (playSeconds > 0.0) { // --play left it active and processing, as a host mid-song would
			gPlugin->stop_processing(gPlugin);
			gPlugin->deactivate(gPlugin);
		}
		gPlugin->destroy(gPlugin);
		entry->deinit();
		return 2;
	}

	const bool updated = before != after;
	std::printf("\nRESULT:   engine->editor %s without a resize\n",
	            updated ? "UPDATED" : "DID NOT UPDATE");
	if (!updated) {
		std::printf("          the editor %s once resized, so it %s\n",
		            before != resized ? "did update" : "still did not update",
		            before != resized ? "is not looking at the engine between redraws"
		                              : "may not be drawing at all — check the display is awake");
	}
	std::printf("          captures: %s %s\n", beforePath.c_str(), afterPath.c_str());
	std::printf("          a pass means nothing until this same run FAILS on a known-bad build.\n");

	gui->destroy(gPlugin);
	if (playSeconds > 0.0) { // --play left it active and processing, as a host mid-song would
		gPlugin->stop_processing(gPlugin);
		gPlugin->deactivate(gPlugin);
	}
	gPlugin->destroy(gPlugin);
	entry->deinit();
	return updated ? 0 : 1;
}
