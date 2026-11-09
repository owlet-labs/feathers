/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not,
 * see <https://www.gnu.org/licenses/>.
 */

// WHICH KEYS REACH THE HOST WHILE A NAME IS TYPED — through a real X server, by the server's own hand.
//
//   scripts/venue-xvfb.sh run -- build/key-probe        # exit 0 iff every expectation holds
//
//
// HOW. The shipped editor is assembled in-process the way alt-probe does it and embedded in an X11
// parent that THIS process owns, standing for the host's window. The parent selects KeyPress and
// KeyRelease, so it hears both kinds of key a host can get: the REAL ones X delivers to it (the focus
// is on it and the pointer is not over the editor), and the SENT ones visage's `passEventToParent`
// forwards (`send_event` set). Keys are pressed through XTEST, so X routes them by its own focus and
// pointer rules — which is the question — rather than by where a test posted them.
//
// WHAT IS ASSERTED, per phase:
//   rest, pointer on the editor   Space reaches the host, forwarded — the DAW keeps its bindings
//   renaming, pointer on it       nothing but a bare modifier reaches the host; the field holds the text
//   renaming, pointer OFF it      the same: the X focus is on our window, so the pointer does not matter
//   committed                     the focus is back on the host's window, and Space reaches it again
//   clicked away mid-edit         a focus the user moved elsewhere is NOT pulled back when the edit ends
//   a claimed window destroyed    X gives the focus to its parent; the give-back leaves it there
//
// WHAT IT CANNOT SAY. This is not Bitwig. The parent is ours and holds the focus because this process
// put it there; no window manager is running. A host that GRABS a key at its toplevel (a passive
// `XGrabKey`) takes it before any child sees it, whatever the focus — that is a host-side fact this
// cannot see, and the hand check in the report is what answers it.
//
// XTEST IS BANNED ON `:1` — that is the desktop display and the user's keyboard. Run this under venue-xvfb.sh.

#include "plugin/gui.h"
#include "plugin/gui_bridge.h"
#include "plugin/params.h"
#include "plugin/ui/keyboard_claim.h"
#include "plugin/ui/macro_slot.h"
#include "plugin/ui/preset_browser.h"
#include "plugin/ui/text_field.h"

#include <visage/ui.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <unistd.h>
#include <vector>

using namespace feathers;

namespace {

struct ProbeGuiHost : GuiHost {
	void guiRequestFlush() noexcept override {}
	void guiRequestResize(uint32_t, uint32_t) noexcept override {}
	/// Some bytes, so the preset prompt has a state to save (an empty one is "Nothing to save").
	void guiCaptureState(clap_ostream_t* s) noexcept override {
		static const char bytes[] = "key-probe";
		(void)s->write(s, bytes, sizeof(bytes));
	}
	bool guiLoadState(clap_istream_t*) noexcept override { return false; }
};

/// One key the host's window heard.
struct Heard {
	bool press;
	bool sent; ///< forwarded by visage, rather than delivered by X
	KeySym keysym;
};

ui::MacroSlot* firstVisibleSlot(visage::Frame* frame) {
	if (auto* slot = dynamic_cast<ui::MacroSlot*>(frame); slot != nullptr && slot->isVisible()) {
		return slot;
	}
	for (visage::Frame* child : frame->children()) {
		if (ui::MacroSlot* found = firstVisibleSlot(child)) {
			return found;
		}
	}
	return nullptr;
}

std::string describe(const std::vector<Heard>& heard) {
	std::string out;
	for (const Heard& h : heard) {
		const char* name = XKeysymToString(h.keysym);
		out += std::string(h.press ? "+" : "-") + (name != nullptr ? name : "?") + (h.sent ? "(sent) " : "(real) ");
	}
	return out.empty() ? "nothing" : out;
}

} // namespace

int main() {
	XInitThreads();
	Display* display = XOpenDisplay(nullptr);
	if (display == nullptr) {
		std::printf("XOpenDisplay failed — is DISPLAY set?\n");
		return 2;
	}
	int ignored[4] {};
	if (!XTestQueryExtension(display, &ignored[0], &ignored[1], &ignored[2], &ignored[3])) {
		std::printf("key-probe: NO XTEST on this display — nothing can be driven\n");
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
	const uint32_t width = Gui::kLogicalWidth;
	const uint32_t height = Gui::kLogicalHeight;
	gui.setSize(width, height);

	const int screen = DefaultScreen(display);
	const ::Window root = RootWindow(display, screen);
	const ::Window parent = XCreateSimpleWindow(display, root, 0, 0, width, height, 0, BlackPixel(display, screen),
	                                            BlackPixel(display, screen));
	XSelectInput(display, parent, KeyPressMask | KeyReleaseMask);
	// SOMEWHERE ELSE THE USER CAN CLICK — a second host window, for the click-away phase.
	const ::Window elsewhere = XCreateSimpleWindow(display, root, 0, 0, 16, 16, 0, BlackPixel(display, screen),
	                                               BlackPixel(display, screen));
	XFlush(display);

	clap_window window {};
	window.api = CLAP_WINDOW_API_X11;
	window.x11 = parent;
	if (!gui.setParent(&window)) {
		std::printf("gui.setParent failed\n");
		return 2;
	}
	XMapWindow(display, parent);
	XMapWindow(display, elsewhere);
	XRaiseWindow(display, parent);
	XSync(display, False);
	XSetInputFocus(display, parent, RevertToParent, CurrentTime);
	XFlush(display);

	std::vector<Heard> heard;
	const auto pump = [&](int milliseconds) {
		for (int slice = 0; slice < std::max(1, milliseconds / 4); ++slice) {
			gui.processEvents();
			while (XPending(display) > 0) {
				XEvent event;
				XNextEvent(display, &event);
				if (event.type == KeyPress || event.type == KeyRelease) {
					heard.push_back({ event.type == KeyPress, event.xkey.send_event != 0,
					                  XLookupKeysym(&event.xkey, 0) });
				}
			}
			usleep(4000);
		}
	};
	pump(1500);

	// OUR WINDOW — the editor's X child of the parent.
	::Window editorWindow = 0;
	{
		::Window r = 0;
		::Window p = 0;
		::Window* children = nullptr;
		unsigned int count = 0;
		XQueryTree(display, parent, &r, &p, &children, &count);
		if (count > 0) {
			editorWindow = children[0];
		}
		if (children != nullptr) {
			XFree(children);
		}
	}
	visage::Frame* tree = gui.root();
	ui::MacroSlot* slot = tree != nullptr ? firstVisibleSlot(tree) : nullptr;
	if (editorWindow == 0 || slot == nullptr) {
		std::printf("key-probe: no editor window (%lu) or no visible macro slot — nothing was driven\n",
		            static_cast<unsigned long>(editorWindow));
		return 2;
	}
	std::printf("editor:   window 0x%lx in host 0x%lx, %ux%u; renaming %s\n", static_cast<unsigned long>(editorWindow),
	            static_cast<unsigned long>(parent), width, height, slot->caption().c_str());

	const auto focus = [&] {
		::Window f = 0;
		int revert = 0;
		XGetInputFocus(display, &f, &revert);
		return f;
	};
	const auto key = [&](KeySym sym, bool down) {
		XTestFakeKeyEvent(display, XKeysymToKeycode(display, sym), down ? True : False, 0);
		XFlush(display);
		pump(40);
	};
	const auto tap = [&](KeySym sym) {
		key(sym, true);
		key(sym, false);
	};
	const auto pointerAt = [&](int x, int y) {
		XTestFakeMotionEvent(display, screen, x, y, 0);
		XFlush(display);
		pump(80);
	};
	const int onX = static_cast<int>(width / 2);
	const int onY = static_cast<int>(height / 2);
	const int screenW = DisplayWidth(display, screen);
	const int offX = static_cast<int>(width) + 8 < screenW ? static_cast<int>(width) + 8 : 4;
	const int offY = static_cast<int>(width) + 8 < screenW ? 4 : static_cast<int>(height) + 8;

	int failures = 0;
	const auto verdict = [&](const char* phase, bool ok, const std::string& detail) {
		std::printf("%-10s %s -> %s\n", phase, detail.c_str(), ok ? "ok" : "WRONG");
		if (!ok) {
			++failures;
		}
	};
	const auto onlyModifiers = [](const std::vector<Heard>& h) {
		return std::all_of(h.begin(), h.end(), [](const Heard& e) { return IsModifierKey(e.keysym); });
	};
	const auto spaceBothWays = [](const std::vector<Heard>& h, bool sent) {
		const auto has = [&](bool press) {
			return std::any_of(h.begin(), h.end(), [&](const Heard& e) {
				return e.keysym == XK_space && e.press == press && e.sent == sent;
			});
		};
		return has(true) && has(false);
	};

	// **NO AUTO-REPEAT.** A press is held for as long as the pumps between it and its release take, and
	// on a loaded host that ran past the server's repeat delay: one tap typed "!!!!!" and the verdict
	// was about the host's load, not the routing (4 of 4 runs on mainline, load 30-49). The server's
	// keyboard control is global, which is why this probe runs only on venue-xvfb's private display; it
	// is put back on the way out. Nothing between here and there returns early.
	XAutoRepeatOff(display);
	XSync(display, False);

	// REST, POINTER ON THE EDITOR: the host hears Space, forwarded.
	pointerAt(onX, onY);
	heard.clear();
	tap(XK_space);
	verdict("rest:", spaceBothWays(heard, true) && focus() == parent, describe(heard));

	// THE VENUE HOLDS STILL: a key held far past any repeat delay is still ONE press and ONE release, so
	// no later phase can be failed (or passed) by how long a loaded host took to pump.
	heard.clear();
	key(XK_space, true);
	pump(1200);
	key(XK_space, false);
	verdict("held:", heard.size() == 2 && spaceBothWays(heard, true), describe(heard));

	// RENAMING, POINTER ON THE EDITOR.
	slot->beginRename();
	pump(80);
	const bool focusTaken = focus() == editorWindow;
	heard.clear();
	tap(XK_space);
	key(XK_Shift_L, true);
	tap(XK_1); // "!" — a key visage cannot name
	key(XK_Shift_L, false);
	tap(XK_Caps_Lock);
	tap(XK_Caps_Lock); // on and off again, so the next letter is lower case
	tap(XK_a);
	verdict("renaming:", slot->renaming() && focusTaken && onlyModifiers(heard) && slot->field().text() == " !a",
	        describe(heard) + " text=\"" + slot->field().text() + "\" focus " + (focusTaken ? "ours" : "NOT ours"));

	// RENAMING, POINTER OFF THE EDITOR — X would give these to the host's window if the focus were there.
	pointerAt(offX, offY);
	heard.clear();
	tap(XK_space);
	tap(XK_b);
	verdict("off-ptr:", onlyModifiers(heard) && slot->field().text() == " !a b",
	        describe(heard) + " text=\"" + slot->field().text() + "\"");

	// COMMITTED: the focus goes back to the host, and Space reaches it — delivered by X now, since the
	// pointer is still off the editor.
	tap(XK_Return);
	const bool committed = !slot->renaming() && slot->caption() == " !a b";
	heard.clear();
	tap(XK_space);
	verdict("committed:", committed && focus() == parent && spaceBothWays(heard, false),
	        describe(heard) + " caption=\"" + slot->caption() + "\"");
	pointerAt(onX, onY);
	heard.clear();
	tap(XK_space);
	verdict("back-on:", spaceBothWays(heard, true), describe(heard));

	// CLICKED AWAY MID-EDIT: the user moves the focus to another window of the host's, then the edit ends.
	slot->beginRename();
	pump(80);
	XSetInputFocus(display, elsewhere, RevertToParent, CurrentTime);
	XSync(display, False);
	pump(40);
	// The focus is on a toplevel that does not hold the editor, so X gives this Escape to that window and
	// not to us; the edit is then ended the way a click-away ends it, by the field being put away.
	tap(XK_Escape);
	if (slot->renaming()) {
		slot->field().setVisible(false); // however it ends, it ends
		pump(40);
	}
	verdict("away:", !slot->renaming() && focus() == elsewhere,
	        std::string("focus ") + (focus() == elsewhere ? "left where the user put it" : "MOVED"));
	XSetInputFocus(display, parent, RevertToParent, CurrentTime);
	XSync(display, False);

	// A CLAIMED WINDOW DESTROYED: X reverts the focus to its parent; the give-back must not undo that.
	{
		const ::Window doomed = XCreateSimpleWindow(display, parent, 0, 0, 8, 8, 0, 0, 0);
		XMapWindow(display, doomed);
		XSync(display, False);
		const uint64_t token = ui::takeX11Keyboard(doomed);
		const bool took = focus() == doomed;
		XDestroyWindow(display, doomed);
		XSync(display, False);
		ui::giveBackX11Keyboard(token);
		verdict("destroyed:", took && focus() == parent,
		        std::string(took ? "taken" : "NOT taken") + ", then focus " + (focus() == parent ? "on the host" : "LOST"));
	}

	// **THE HOST TAKES THE FOCUS BACK** — what Bitwig's plugin window does (it holds the X focus itself and
	// forwards most keys, keeping Space: RustAudio/baseview#116; 2026-11-05: no capitals, no spaces,
	// keys reaching the DAW). The parent re-focuses itself after the claim; with the focus alone every key
	// below would be delivered to it, real. With the grab they still come to the field, Shift and all.
	pointerAt(onX, onY);
	slot->beginRename();
	pump(80);
	XSetInputFocus(display, parent, RevertToParent, CurrentTime);
	XSync(display, False);
	pump(40);
	// AND THE POINTER OFF THE EDITOR, so X itself would give every key to the host's window: what reaches the
	// field from here is what the grab brought, not what the pointer did.
	pointerAt(offX, offY);
	const bool slotGrabbed = ui::keyboardGrabbedForTest(editorWindow);
	heard.clear();
	key(XK_Shift_L, true);
	tap(XK_h);
	key(XK_Shift_L, false);
	tap(XK_i);
	tap(XK_space);
	verdict("stolen:", slotGrabbed && onlyModifiers(heard) && slot->field().text() == "Hi ",
	        describe(heard) + " text=\"" + slot->field().text() + "\" grab " + (slotGrabbed ? "held" : "NOT held"));
	tap(XK_Escape);
	pump(40);

	// **THE PRESET PROMPT, TYPED BY THE SERVER'S HAND WITH THE HOST HOLDING THE FOCUS** — "Hello World" with a
	// shifted H and W and a Space, exactly in the name field and nothing at the host.
	ui::PresetBrowser* browser = nullptr;
	{
		std::vector<visage::Frame*> stack { tree };
		while (!stack.empty() && browser == nullptr) {
			visage::Frame* at = stack.back();
			stack.pop_back();
			browser = dynamic_cast<ui::PresetBrowser*>(at);
			for (visage::Frame* child : at->children()) {
				stack.push_back(child);
			}
		}
	}
	if (browser == nullptr) {
		verdict("preset:", false, "no PresetBrowser in the editor");
	} else {
		browser->chooseFromMenu(ui::PresetBrowser::kMenuSave);
		pump(80);
		XSetInputFocus(display, parent, RevertToParent, CurrentTime);
		XSync(display, False);
		pump(40);
		const bool promptGrabbed = ui::keyboardGrabbedForTest(editorWindow);
		heard.clear();
		const auto shifted = [&](KeySym sym) {
			key(XK_Shift_L, true);
			tap(sym);
			key(XK_Shift_L, false);
		};
		shifted(XK_h);
		for (KeySym s : { XK_e, XK_l, XK_l, XK_o, XK_space }) {
			tap(s);
		}
		shifted(XK_w);
		for (KeySym s : { XK_o, XK_r, XK_l, XK_d }) {
			tap(s);
		}
		const std::string typed = browser->promptTextForTest();
		verdict("preset:", promptGrabbed && onlyModifiers(heard) && typed == "Hello World",
		        describe(heard) + " name=\"" + typed + "\" grab " + (promptGrabbed ? "held" : "NOT held"));

		// A KEY THE HOST FORWARDS WITHOUT SHIFT IN ITS STATE, while Shift is held: a sent event carries what its
		// sender wrote, and the server's own Shift is what the user is holding. The capital must survive.
		{
			key(XK_Shift_L, true);
			XEvent sent {};
			sent.xkey.type = KeyPress;
			sent.xkey.display = display;
			sent.xkey.window = editorWindow;
			sent.xkey.root = root;
			sent.xkey.same_screen = True;
			sent.xkey.keycode = XKeysymToKeycode(display, XK_k);
			sent.xkey.state = 0;
			XSendEvent(display, editorWindow, False, KeyPressMask, &sent);
			sent.xkey.type = KeyRelease;
			XSendEvent(display, editorWindow, False, KeyReleaseMask, &sent);
			XFlush(display);
			pump(40);
			key(XK_Shift_L, false);
			const std::string forwarded = browser->promptTextForTest();
			verdict("forwarded:", forwarded == "Hello WorldK", "name=\"" + forwarded + "\"");
		}

		// A KEY VISAGE CANNOT NAME — é, which a French or Finnish layout has on its own key. This server's US map
		// has none, so a spare keycode is given é for the phase (the private server only). Its text is typed and
		// the key is not forwarded after it: a typed letter must not also reach the DAW.
		{
			int minCode = 0;
			int maxCode = 0;
			XDisplayKeycodes(display, &minCode, &maxCode);
			int spare = 0;
			for (int code = maxCode; code >= minCode && spare == 0; --code) {
				int per = 0;
				KeySym* syms = XGetKeyboardMapping(display, static_cast<KeyCode>(code), 1, &per);
				bool empty = true;
				for (int i = 0; i < per; ++i) {
					empty = empty && syms[i] == NoSymbol;
				}
				XFree(syms);
				if (empty) {
					spare = code;
				}
			}
			KeySym accent[2] = { XK_eacute, XK_Eacute };
			XChangeKeyboardMapping(display, spare, 2, accent, 1);
			XSync(display, False);
			pump(80);
			heard.clear();
			XTestFakeKeyEvent(display, static_cast<unsigned int>(spare), True, 0);
			XFlush(display);
			pump(40);
			XTestFakeKeyEvent(display, static_cast<unsigned int>(spare), False, 0);
			XFlush(display);
			pump(40);
			KeySym none[2] = { NoSymbol, NoSymbol };
			XChangeKeyboardMapping(display, spare, 2, none, 1);
			XSync(display, False);
			const std::string accented = browser->promptTextForTest();
			verdict("unnamed:", spare != 0 && onlyModifiers(heard) && accented == "Hello WorldK\xC3\xA9",
			        describe(heard) + " name=\"" + accented + "\" keycode " + std::to_string(spare));
		}

		// ENTER WITH THE FOCUS ON THE PANEL, as a click on a tag chip leaves it: it saves, and reaches nobody else.
		if (visage::Frame* panel = browser->promptFrameForTest(); panel != nullptr) {
			panel->requestKeyboardFocus();
		}
		heard.clear();
		tap(XK_Return);
		pump(40);
		const bool saved = browser->prompt() == ui::PresetBrowser::Prompt::Closed;
		verdict("enter:", saved && onlyModifiers(heard) && !ui::keyboardGrabbedForTest(editorWindow),
		        describe(heard) + (saved ? std::string(" prompt closed") : " prompt STILL OPEN (" + std::to_string(static_cast<int>(browser->prompt())) + ", \"" + browser->notice() + "\", focus on panel " + std::to_string(browser->promptFrameForTest() != nullptr && browser->promptFrameForTest()->hasKeyboardFocus()) + ")"));

		// A CLICK IN ANOTHER WINDOW MID-NAME LETS GO OF THE GRAB, so the DAW is not left deaf until Enter or Esc.
		browser->chooseFromMenu(ui::PresetBrowser::kMenuSave);
		pump(80);
		const bool grabbedBefore = ui::keyboardGrabbedForTest(editorWindow);
		XRaiseWindow(display, elsewhere);
		XSync(display, False);
		pointerAt(4, 4);
		XTestFakeButtonEvent(display, 1, True, 0);
		XFlush(display);
		pump(80);
		XTestFakeButtonEvent(display, 1, False, 0);
		XFlush(display);
		pump(40);
		const bool released = !ui::keyboardGrabbedForTest(editorWindow);
		verdict("click-off:", grabbedBefore && released,
		        std::string("grab ") + (grabbedBefore ? "held" : "NOT held") + ", then "
		            + (released ? "let go" : "STILL HELD"));
		browser->cancelPrompt();
		pump(40);
	}

	std::printf("verdict:  %s\n", failures == 0 ? "no typed key reached the host, and the host kept its keys at rest"
	                                            : "a key went the wrong way");
	std::fflush(stdout);
	XAutoRepeatOn(display);
	XSync(display, False);
	gui.destroy();
	XDestroyWindow(display, elsewhere);
	XDestroyWindow(display, parent);
	XCloseDisplay(display);
	return failures == 0 ? 0 : 1;
}
