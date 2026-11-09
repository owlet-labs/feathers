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

#include "plugin/ui/keyboard_claim.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <visage/ui.h>

// **THE FIRST PLACE IN THE PLUGIN A USER TYPES.** A macro's right-click menu offers "Rename…", which
// swaps the caption for this field; Enter keeps the name, Escape or a click elsewhere gives it back.
// Single line, one job, and deliberately small: no mouse selection, no clipboard, no undo.
//
// **WHY NOT `visage::TextEditor`, which ships in the library we already link.** Three reasons, and the
// first decides it on its own:
//
// - **THE CAP IS IN BYTES AND ITS CAP IS IN CHARACTERS.** A name is `device::kMacroNameMax` bytes of
//   UTF-8; `setMaxCharacters` counts UTF-32 code points, so "24" admits 96 bytes. Enforcing ours in a
//   subclass means policing every road into its text, and `insertTextAtCaret` — the one they share —
//   is not virtual: paste and dead-key composition reach it past any `textInput` override.
// - **IT DRAWS FROM A visage PALETTE.** Its colours are `VISAGE_THEME_DEFINE_COLOR` ids, and nothing in
//   this editor has a palette; `theme.h` is constants. Seating it would mean a palette for one widget.
// - **IT IS A SCROLLABLE, MULTI-LINE, CLIPBOARD-AND-UNDO EDITOR** carrying a scroll bar and a history
//   of 1000 states to rename a knob.
//
// **HOW IT IS FED, and the one thing about it that is not obvious.** visage's X11 layer delivers the
// TEXT of a keypress before its KEY, and hands over `XLookupString`'s bytes as they came — which are
// ISO LATIN-1, not UTF-8, so é arrives as the single byte 0xE9. And Return, Escape and Backspace
// arrive as text too, as "\r", "\x1b" and "\b". So `textInput` drops control characters and reads a
// byte sequence that is not UTF-8 as Latin-1; see the .cpp.
//
// **WHILE IT EDITS, THE KEYBOARD IS OURS AND NOTHING TYPED REACHES THE DAW** (otherwise Space
// starts Bitwig's transport mid-rename). Three roads carried a keystroke past the field, and each is
// closed separately: the X focus, which puts every key on our window wherever the pointer is — a
// `KeyboardClaim`, held for exactly the edit (keyboard_claim.h); the RELEASE of a key whose press was
// typed, which used to travel on to the host; and a key visage cannot name (`!`, `é`), which our fork
// of visage now keeps from the host while a text entry is active. At rest the field claims nothing and
// every key goes up as before.

namespace feathers::ui {

class TextField : public visage::Frame {
public:
	TextField();
	/// **DESTROYED MID-EDIT, IT CALLS NOBODY.** The owner is usually what is tearing it down, and a
	/// cancel delivered into a half-destroyed owner is the use-after-free this rule prevents.
	~TextField() override;

	/// Open an edit on `initial` — clamped to the name cap the way the name store clamps it — with ALL
	/// of it selected, so the first character typed replaces it. Takes the keyboard focus. Calling it
	/// during an edit abandons that edit WITHOUT a cancel: the caller is starting over on purpose.
	void begin(std::string initial);

	/// Enter: the name as typed. Called after the field has stopped editing.
	///
	/// **FROM INSIDE EITHER CALLBACK THE OWNER MAY HIDE THE FIELD, AND MUST NEVER DESTROY IT.** The
	/// field's own code finishing last is not enough, because visage's is still on the stack beneath
	/// it: a cancel on the HIDE path runs inside `Frame::setVisible`, which goes on to use `region_`,
	/// `redraw()` and `setDrawing()` after `visibilityChanged` returns, and a commit on Enter runs
	/// inside `CallbackList::callback`, which executes the frame's own `std::function` in place. So a
	/// field destroyed from either is a use-after-free, and the Enter half is one ASan cannot see. The
	/// owner (A7's MacroSlot) keeps ONE field for its own lifetime and only hides it. Hiding from inside
	/// is safe, including on the hide path: the edit has already ended, so the second hide finds
	/// nothing to cancel.
	void onCommit(std::function<void(std::string)> callback) { commit_ = std::move(callback); }
	/// Escape, a focus loss, or being hidden while editing — never a commit. The same rule as above.
	void onCancel(std::function<void()> callback) { cancel_ = std::move(callback); }

	[[nodiscard]] bool editing() const noexcept { return editing_; }
	/// The text as it stands, UTF-8, never longer than `device::kMacroNameMax` bytes.
	[[nodiscard]] const std::string& text() const noexcept { return text_; }

	/// TEST SEAM — where the caret is, as a byte offset that is always on a code-point boundary.
	[[nodiscard]] size_t caretForTest() const noexcept { return caret_; }
	/// TEST SEAM — how far the text is slid left, as of the last draw.
	[[nodiscard]] float scrollForTest() const noexcept { return scroll_; }

	void draw(visage::Canvas& canvas) override;
	bool keyPress(const visage::KeyEvent& key) override;
	/// A key whose press the field took is the field's on release too, even when that press ended the
	/// edit (Enter, Escape): the host was never shown the press and must not be shown half a keystroke.
	bool keyRelease(const visage::KeyEvent& key) override;
	bool receivesTextInput() override { return editing_; }
	void textInput(const std::string& text) override;
	void focusChanged(bool isFocused, bool wasClicked) override;
	void visibilityChanged() override;

private:
	void finish(bool commit);
	void eraseSelection();
	[[nodiscard]] bool hasSelection() const noexcept { return anchor_ != caret_; }

	std::string text_;
	/// Both are byte offsets into `text_` on code-point boundaries. The selection is the span between
	/// them; with no mouse selection and no shift-arrows, the only one that ever exists is begin's.
	size_t caret_ = 0;
	size_t anchor_ = 0;
	bool editing_ = false;
	/// How far the text is slid left so the caret stays in view once the name outgrows the field.
	/// Set in `draw`, which is where the caret's position in pixels becomes known.
	float scroll_ = 0.0f;
	std::function<void(std::string)> commit_;
	std::function<void()> cancel_;
	/// The keyboard, for exactly as long as `editing_`: taken in `begin`, let go in `finish` and in the
	/// destructor. A pointer only so `begin` can take the new claim before letting go of the old one.
	std::unique_ptr<KeyboardClaim> claim_;
	/// The keys pressed into the field and not yet released.
	std::vector<visage::KeyCode> held_;
};

} // namespace feathers::ui
