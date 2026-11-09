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

#pragma once

#include <cstdint>

namespace visage {
class Frame;
}

// **THE KEYBOARD, TAKEN FROM THE HOST WHILE SOMEBODY TYPES AND GIVEN BACK THE MOMENT THEY STOP.**
//
// In Bitwig on X11, 2026-09-26: while a text box was up (macro rename, tag edit, preset name), the
// DAW still intercepted keystrokes — space played/paused, Caps Lock played notes.
//
// WHY THE EDITOR DOES NOT NORMALLY HOLD THE KEYBOARD, and must not: it is an X child inside the host's
// window, the window manager keeps the focus on the host, and X hands a key to the window under the
// pointer only while that window is inside the focused window's subtree — otherwise to the focused
// window itself. visage forwards every key we do not use to our parent. That is how the DAW keeps its
// bindings while the editor is open, and losing the DAW's key bindings would be a deal-breaker.
// So the plugin never asks for the focus at rest.
//
// WHY A FIELD BEING TYPED INTO MUST: under that rule a keystroke reaches us only while the pointer is
// over the editor AND the host's focus is on the window we are embedded in. Move the mouse off the
// editor mid-name, or be embedded in a host window the window manager did not focus, and every key goes
// to the DAW — Space starts the transport. So while a field is editing, the X focus is put on our own
// window (`XSetInputFocus`), which makes X deliver every key to us wherever the pointer is.
//
// **THE RELEASE IS THE HALF THAT MATTERS.** A claim left standing is a DAW without its keyboard, which
// is worse than the bug. So a claim is an OBJECT, and its destructor gives the keyboard back: every
// exit — commit, cancel, click-away, hidden, the owner destroyed mid-edit — ends a lifetime, and the
// release rides on that rather than on each exit remembering to call something.
//
// GIVING IT BACK: to the window that held the focus when it was taken, and ONLY IF THE FOCUS IS STILL
// OURS. A user who clicked into the DAW while typing has already moved it, and yanking it back to where
// it was before the edit would be the plugin stealing focus. If the saved window has gone, the focus goes
// to our window's parent — the host's container — never to nothing.
//
// OTHER PLATFORMS: nothing, deliberately. macOS makes the embedded view the first responder when it is
// seated (visage's `setParentWindow`) and forwards an unused key up the responder chain; Win32 visage
// `SetFocus`es its HWND on every button press. In both a key already reaches a field that was clicked or
// right-clicked into. That is read from visage's code, not measured in a host.

namespace feathers::ui {

/// **THE SEAM.** What takes and gives back the keyboard. The platform's is the default; a case installs
/// its own (`ScopedKeyboardClaimBackend`) to count the pairs without a display.
class KeyboardClaimBackend {
public:
	virtual ~KeyboardClaimBackend() = default;
	/// Take the keyboard for the window `frame` is drawn in. Returns what `giveBack` needs, or 0 when
	/// nothing was taken (no window yet, a platform with nothing to do, the focus already ours).
	virtual uint64_t take(const visage::Frame& frame) = 0;
	/// Undo one `take` that returned `token`. Never called with 0.
	virtual void giveBack(uint64_t token) = 0;
};

/// Installs `backend` for its lifetime, restoring the one before. Test-only in practice.
class ScopedKeyboardClaimBackend {
public:
	explicit ScopedKeyboardClaimBackend(KeyboardClaimBackend& backend);
	~ScopedKeyboardClaimBackend();
	ScopedKeyboardClaimBackend(const ScopedKeyboardClaimBackend&) = delete;
	ScopedKeyboardClaimBackend& operator=(const ScopedKeyboardClaimBackend&) = delete;

private:
	KeyboardClaimBackend* previous_;
};

/// ONE CLAIM, FOR ITS LIFETIME. Held by whatever is being typed into, as a member it resets when the
/// edit ends. Not copyable and not movable: a claim that could be copied could be given back twice.
class KeyboardClaim {
public:
	explicit KeyboardClaim(const visage::Frame& frame);
	~KeyboardClaim();
	KeyboardClaim(const KeyboardClaim&) = delete;
	KeyboardClaim& operator=(const KeyboardClaim&) = delete;

private:
	/// The backend it was taken from, so a claim outliving a case's scoped backend still gives back to
	/// the one that took it.
	KeyboardClaimBackend* backend_;
	uint64_t token_;
};

/// **A CLICK OUTSIDE THE EDITOR LETS GO OF THE GRAB** (Linux; nothing elsewhere). While a claim stands the
/// keyboard is actively grabbed, and X tells us nothing about a click in another client's window — so a
/// user who clicks into the DAW mid-name would find the DAW deaf until Enter or Esc. Called once a frame by
/// each editor's tick: when a mouse button is down with the pointer outside a grabbing window, that grab
/// is released and the claim falls back to the focus alone (which the DAW's click has already moved).
/// Costs nothing while no grab stands.
void pollKeyboardClaims();

#if defined(__linux__)
/// Whether the claim on `window` currently holds an active keyboard grab. tools/key-probe reads it.
bool keyboardGrabbedForTest(unsigned long window);

/// **THE X11 MECHANISM ITSELF, by window id** — what the platform backend does once it has found the
/// frame's window, exposed so tools/key-probe can prove it against a real server. Returns 0 when
/// nothing was taken.
uint64_t takeX11Keyboard(unsigned long window);
void giveBackX11Keyboard(uint64_t token);
#endif

} // namespace feathers::ui
