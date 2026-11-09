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

#include "plugin/ui/keyboard_claim.h"

#include <visage/app.h>

#if defined(__linux__)
#include <X11/Xlib.h>

#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <vector>
#endif

namespace feathers::ui {

namespace {

#if defined(__linux__)

/// **ONE RECORD PER WINDOW, COUNTED.** Two claims on one window at once are real — `TextField::begin`
/// during an edit, a prompt open over a rename — and they end in either order. Uncounted, the second
/// take would find the focus already ours and save OUR window as the one to give back to, or the first
/// release would hand the keyboard to the DAW while the second field was still being typed into. So
/// the first take saves where the focus was, and only the last release puts it back.
struct Held {
	int count = 0;
	/// Where the focus was before the first take — a window, `PointerRoot`, or `None`.
	::Window previous = None;
	/// False when the focus was already ours before anyone claimed it: nothing was taken, so nothing
	/// is given back.
	bool restore = false;
	/// The keyboard is ACTIVELY GRABBED for the window (visage's `grabKeyboardForWindow`), so every key
	/// comes to us whoever holds the focus. Cleared when the grab is let go early (`pollKeyboardClaims`).
	bool grabbed = false;
};

/// Message thread only, as every frame is.
std::map<::Window, Held>& held() {
	static std::map<::Window, Held> table;
	return table;
}

/// **GRABS KEPT UNTIL THE KEYS ARE UP.** The key that ends an edit — Enter, Esc — is pressed while the
/// grab stands and released after the claim is given back; let go at once, its RELEASE goes to whoever has
/// the focus: a key-up the host never saw pressed (measured in tools/key-probe, `-Return(real)`). So a grab
/// whose claim ends with a key down stays until `pollKeyboardClaims` sees every key up.
std::set<::Window>& lingering() {
	static std::set<::Window> windows;
	return windows;
}

bool anyKeyDown(Display* display) {
	char keys[32] {};
	XQueryKeymap(display, keys);
	return std::any_of(std::begin(keys), std::end(keys), [](char k) { return k != 0; });
}

/// Plain X on a connection of our own, the way `plugin_window.cpp` reparents: visage's connection is
/// its event loop's, and nothing here needs its events.
struct OwnDisplay {
	Display* display = XOpenDisplay(nullptr);
	~OwnDisplay() {
		if (display != nullptr) {
			XCloseDisplay(display);
		}
	}
	OwnDisplay() = default;
	OwnDisplay(const OwnDisplay&) = delete;
	OwnDisplay& operator=(const OwnDisplay&) = delete;
};

/// A window that still exists and is on screen — the only kind `XSetInputFocus` accepts. A window that
/// has gone raises BadWindow, which visage's process-wide handler logs and survives.
bool viewable(Display* display, ::Window window) {
	XWindowAttributes attributes {};
	return XGetWindowAttributes(display, window, &attributes) != 0 && attributes.map_state == IsViewable;
}

::Window parentOf(Display* display, ::Window window) {
	::Window root = 0;
	::Window parent = 0;
	::Window* children = nullptr;
	unsigned int count = 0;
	if (XQueryTree(display, window, &root, &parent, &children, &count) == 0) {
		return None;
	}
	if (children != nullptr) {
		XFree(children);
	}
	return parent == root ? None : parent;
}

/// **THE NEAREST EDITOR UP THE TREE, NOT THE TOP OF IT.** visage parents an `ApplicationEditor` INSIDE a
/// `TopLevelFrame` of its own, so the root of every real editor's tree is that frame, which is not an
/// editor and cannot be asked for its window. MEASURED in tools/key-probe: a walk to the root, as
/// `nativeWindowOf` makes, answers null for a field in the assembled editor, and the claim took nothing.
///
/// **AND A FRAME HUNG IN THE TOP FRAME ITSELF, BESIDE THE EDITOR, FINDS IT THERE.** The preset prompt is
/// added to `topParentFrame()` — the `TopLevelFrame` — so no editor is ever its ancestor: the editor is its
/// SIBLING. The walk above found nothing for it and every preset prompt since it moved there claimed
/// NOTHING (measured 2026-11-05 in tools/key-probe: no grab, no focus), which is why preset names in Bitwig
/// got only what the host chose to forward — no Space, no capitals — while a macro rename worked.
void* windowOf(const visage::Frame& frame) {
	const auto handleOf = [](const visage::ApplicationEditor& editor) -> void* {
		return editor.window() != nullptr ? editor.window()->nativeHandle() : nullptr;
	};
	const visage::Frame* root = &frame;
	for (const visage::Frame* at = &frame; at != nullptr; at = at->parent()) {
		if (const auto* editor = dynamic_cast<const visage::ApplicationEditor*>(at); editor != nullptr) {
			return handleOf(*editor);
		}
		root = at;
	}
	for (const visage::Frame* child : root->children()) {
		if (const auto* editor = dynamic_cast<const visage::ApplicationEditor*>(child); editor != nullptr) {
			return handleOf(*editor);
		}
	}
	return nullptr;
}

#endif

class PlatformKeyboardClaimBackend final : public KeyboardClaimBackend {
public:
	uint64_t take(const visage::Frame& frame) override {
#if defined(__linux__)
		return takeX11Keyboard(reinterpret_cast<unsigned long>(windowOf(frame)));
#else
		(void)frame;
		return 0;
#endif
	}
	void giveBack(uint64_t token) override {
#if defined(__linux__)
		giveBackX11Keyboard(token);
#else
		(void)token;
#endif
	}
};

KeyboardClaimBackend& platformBackend() {
	static PlatformKeyboardClaimBackend backend;
	return backend;
}

KeyboardClaimBackend* scopedBackend = nullptr;

KeyboardClaimBackend& currentBackend() {
	return scopedBackend != nullptr ? *scopedBackend : platformBackend();
}

} // namespace

#if defined(__linux__)

uint64_t takeX11Keyboard(unsigned long window) {
	const auto self = static_cast<::Window>(window);
	if (self == None) {
		return 0;
	}
	// A grab still lingering from the last edit belongs to this one now: the poll must not let it go.
	lingering().erase(self);
	auto found = held().find(self);
	if (found != held().end()) {
		++found->second.count;
		return self;
	}
	OwnDisplay own;
	if (own.display == nullptr || !viewable(own.display, self)) {
		return 0;
	}
	::Window focus = None;
	int revert = 0;
	XGetInputFocus(own.display, &focus, &revert);
	Held record;
	record.count = 1;
	record.previous = focus;
	record.restore = focus != self;
	if (record.restore) {
		// RevertToParent: if this window is unmapped or destroyed while it holds the focus — the editor
		// closed mid-name — X itself hands the keyboard to the host's container.
		XSetInputFocus(own.display, self, RevertToParent, CurrentTime);
		XSync(own.display, False);
	}
	// **AND THE KEYBOARD GRABBED, BECAUSE THE FOCUS ALONE DOES NOT HOLD IN BITWIG** (with the focus
	// alone, capitals and spaces never reached a preset name and keys still reached the DAW).
	// Bitwig's plugin window takes the X focus itself and forwards most keys to the plugin, keeping Space
	// and others (RustAudio/baseview#116), so a focus put on our window is taken straight back and our
	// keys are whatever the host chose to pass on. An active grab is the one thing a focus change cannot
	// undo: X delivers every key to the grab window until it is let go. It must be the window's OWN
	// connection that grabs (the grabbing client is the one that receives), hence visage's call. X ends a
	// grab itself if the window is unmapped or destroyed. Refused (another client's grab, not viewable)
	// leaves the focus claim as the only one, as before.
	record.grabbed = visage::grabKeyboardForWindow(reinterpret_cast<void*>(self));
	held()[self] = record;
	return self;
}

void giveBackX11Keyboard(uint64_t token) {
	const auto self = static_cast<::Window>(token);
	auto found = held().find(self);
	if (found == held().end()) {
		return;
	}
	if (--found->second.count > 0) {
		return;
	}
	const Held record = found->second;
	held().erase(found);
	if (record.grabbed) {
		const OwnDisplay keys;
		if (keys.display != nullptr && anyKeyDown(keys.display)) {
			lingering().insert(self); // let go by `pollKeyboardClaims` once every key is up
		} else {
			visage::releaseKeyboardForWindow(reinterpret_cast<void*>(self));
		}
	}
	if (!record.restore) {
		return;
	}
	OwnDisplay own;
	if (own.display == nullptr) {
		return;
	}
	::Window focus = None;
	int revert = 0;
	XGetInputFocus(own.display, &focus, &revert);
	// **ONLY IF IT IS STILL OURS.** A click into the DAW mid-edit moved it already; putting it back where
	// it was before the edit would be the plugin stealing focus from the window the user just chose.
	if (focus != self) {
		return;
	}
	::Window to = record.previous;
	// NEVER TO NOTHING: `None` would leave every key undelivered, and a saved window that has gone
	// cannot take it. The host's container can.
	if (to == None || (to != PointerRoot && !viewable(own.display, to))) {
		to = parentOf(own.display, self);
	}
	if (to == None) {
		to = PointerRoot;
	}
	XSetInputFocus(own.display, to, RevertToParent, CurrentTime);
	XSync(own.display, False);
}

bool keyboardGrabbedForTest(unsigned long window) {
	const auto found = held().find(static_cast<::Window>(window));
	return found != held().end() && found->second.grabbed;
}

#endif

void pollKeyboardClaims() {
#if defined(__linux__)
	// A connection of our own kept only while some grab stands: one `XQueryPointer` a frame then, and
	// nothing at all at rest.
	static Display* poll = nullptr;
	const bool any = !lingering().empty()
	                 || std::any_of(held().begin(), held().end(), [](const auto& h) { return h.second.grabbed; });
	if (!any) {
		if (poll != nullptr) {
			XCloseDisplay(poll);
			poll = nullptr;
		}
		return;
	}
	if (poll == nullptr && (poll = XOpenDisplay(nullptr)) == nullptr) {
		return;
	}
	if (!lingering().empty() && !anyKeyDown(poll)) {
		for (const ::Window window : lingering()) {
			visage::releaseKeyboardForWindow(reinterpret_cast<void*>(window));
		}
		lingering().clear();
	}
	const ::Window root = DefaultRootWindow(poll);
	::Window rootReturn = 0;
	::Window child = 0;
	int px = 0;
	int py = 0;
	int wx = 0;
	int wy = 0;
	unsigned int mask = 0;
	if (XQueryPointer(poll, root, &rootReturn, &child, &px, &py, &wx, &wy, &mask) == 0
	    || (mask & (Button1Mask | Button2Mask | Button3Mask)) == 0) {
		return;
	}
	std::vector<::Window> released;
	for (auto& [window, record] : held()) {
		if (!record.grabbed) {
			continue;
		}
		// INSIDE MEANS OUR WINDOW IS THE ONE UNDER THE POINTER, not that the point is within our rectangle:
		// a DAW window stacked over the editor covers the same pixels. So the pointer's own top-level (the
		// root's child it is in) must be ours, and then our window must contain the point.
		::Window top = window;
		for (::Window up = parentOf(poll, top); up != None; up = parentOf(poll, top)) {
			top = up;
		}
		XWindowAttributes attributes {};
		int x = 0;
		int y = 0;
		::Window ignored = 0;
		const bool known = XGetWindowAttributes(poll, window, &attributes) != 0
		                   && XTranslateCoordinates(poll, window, root, 0, 0, &x, &y, &ignored) != 0;
		const bool inside = known && child == top && px >= x && py >= y && px < x + attributes.width
		                    && py < y + attributes.height;
		if (!inside) {
			record.grabbed = false;
			released.push_back(window);
		}
	}
	// AFTER THE WALK: the release tells the window it lost the focus when the click put the focus elsewhere,
	// which ends the edit — whose claim then gives back, changing `held()` under any loop still walking it.
	for (const ::Window window : released) {
		visage::releaseKeyboardForWindow(reinterpret_cast<void*>(window), true);
	}
#endif
}

ScopedKeyboardClaimBackend::ScopedKeyboardClaimBackend(KeyboardClaimBackend& backend) : previous_(scopedBackend) {
	scopedBackend = &backend;
}

ScopedKeyboardClaimBackend::~ScopedKeyboardClaimBackend() { scopedBackend = previous_; }

KeyboardClaim::KeyboardClaim(const visage::Frame& frame)
    : backend_(&currentBackend()), token_(backend_->take(frame)) {}

KeyboardClaim::~KeyboardClaim() {
	if (token_ != 0) {
		backend_->giveBack(token_);
	}
}

} // namespace feathers::ui
