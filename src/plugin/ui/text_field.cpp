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

#include "plugin/ui/text_field.h"

#include "device/macro_type.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <string_view>

namespace feathers::ui {

namespace {

/// The inset from the field's edge to the text, either side.
constexpr float kPadX = 3.0f;
/// The caret is one pixel wide.
constexpr float kCaretWidth = 1.0f;

[[nodiscard]] bool isContinuation(unsigned char byte) {
	return (byte & 0xC0u) == 0x80u;
}

/// How many bytes the code point led by `lead` takes, or 0 when `lead` cannot start one.
[[nodiscard]] size_t sequenceLength(unsigned char lead) {
	if (lead < 0x80u) {
		return 1;
	}
	if ((lead & 0xE0u) == 0xC0u) {
		return lead >= 0xC2u ? 2 : 0; // C0 and C1 only ever start an overlong spelling
	}
	if ((lead & 0xF0u) == 0xE0u) {
		return 3;
	}
	if ((lead & 0xF8u) == 0xF0u) {
		return lead <= 0xF4u ? 4 : 0; // past F4 is past U+10FFFF
	}
	return 0;
}

/// **THE SECOND BYTE IS WHERE THE REMAINING ILLEGAL SPELLINGS LIVE**, so a lead byte alone cannot
/// rule them out: E0 80..9F is an overlong three-byte form, ED A0..BF a UTF-16 surrogate, F0 80..8F
/// an overlong four-byte form, and F4 90..BF past U+10FFFF. Every other lead accepts any continuation.
[[nodiscard]] bool secondByteAllowed(unsigned char lead, unsigned char second) {
	switch (lead) {
		case 0xE0u: return second >= 0xA0u && second <= 0xBFu;
		case 0xEDu: return second >= 0x80u && second <= 0x9Fu;
		case 0xF0u: return second >= 0x90u && second <= 0xBFu;
		case 0xF4u: return second >= 0x80u && second <= 0x8Fu;
		default: return isContinuation(second);
	}
}

[[nodiscard]] bool validUtf8(std::string_view text) {
	for (size_t i = 0; i < text.size();) {
		const auto lead = static_cast<unsigned char>(text[i]);
		const size_t length = sequenceLength(lead);
		if (length == 0 || i + length > text.size()) {
			return false;
		}
		if (length > 1 && !secondByteAllowed(lead, static_cast<unsigned char>(text[i + 1]))) {
			return false;
		}
		for (size_t k = 2; k < length; ++k) {
			if (!isContinuation(static_cast<unsigned char>(text[i + k]))) {
				return false;
			}
		}
		i += length;
	}
	return true;
}

/// **WHAT ARRIVES IS NOT ALWAYS UTF-8.** `windowing_x11.cpp` hands on `XLookupString`'s buffer, which
/// Xlib documents as ISO Latin-1: é is the single byte 0xE9. Latin-1 is the first 256 code points, so
/// each byte IS its code point and the transcoding is exact. Input that is already valid UTF-8 — any
/// other platform, or an X input method — is taken as it is; the two cannot be confused in practice,
/// because a run of Latin-1 accented letters is almost never a valid UTF-8 sequence.
[[nodiscard]] std::string asUtf8(std::string_view text) {
	if (validUtf8(text)) {
		return std::string(text);
	}
	std::string out;
	for (const char c : text) {
		const auto byte = static_cast<unsigned char>(c);
		if (byte < 0x80u) {
			out.push_back(c);
		}
		else {
			out.push_back(static_cast<char>(0xC0u | (byte >> 6)));
			out.push_back(static_cast<char>(0x80u | (byte & 0x3Fu)));
		}
	}
	return out;
}

[[nodiscard]] size_t previousBoundary(const std::string& text, size_t at) {
	if (at == 0) {
		return 0;
	}
	--at;
	while (at > 0 && isContinuation(static_cast<unsigned char>(text[at]))) {
		--at;
	}
	return at;
}

[[nodiscard]] size_t nextBoundary(const std::string& text, size_t at) {
	if (at >= text.size()) {
		return text.size();
	}
	++at;
	while (at < text.size() && isContinuation(static_cast<unsigned char>(text[at]))) {
		++at;
	}
	return at;
}

/// A glyph's advance, measured one code point at a time — `drawTracked`'s own arithmetic.
///
/// **NOT THROUGH `trackedWidth`, on purpose.** Its cache keeps the first 64 strings it is asked about
/// for the life of the editor, and every keystroke here is a string nobody will draw again — a name
/// typed out would spend a third of that cache on its own drafts, and every caption after it would
/// fall to the uncached path.
[[nodiscard]] float advanceOf(const visage::Font& face, std::string_view glyph) {
	const visage::String decoded(std::string { glyph });
	return decoded.length() > 0 ? face.stringWidth(decoded.c_str(), 1) : 0.0f;
}

} // namespace

TextField::TextField() {
	// So the window hands the focus here on a click, and so a key travelling up from a child stops.
	setAcceptsKeystrokes(true);
}

TextField::~TextField() {
	// Nothing is called from here: see the header. Dropping the callbacks first means that nothing
	// visage does to this frame on the way out — a focus or visibility notice from `~Frame` — can
	// reach one either.
	editing_ = false;
	commit_ = nullptr;
	cancel_ = nullptr;
	// **DESTROYED MID-EDIT, THE KEYBOARD STILL GOES BACK** — the one exit no callback marks, and the one
	// that would leave the DAW deaf until something else moved the focus.
	claim_.reset();
}

void TextField::begin(std::string initial) {
	// **THE FOCUS FIRST, THEN THE EDIT.** When this frame already holds the focus, visage's
	// `setKeyboardFocus` tells it "lost" and then "gained" — and a loss is a cancel. Asked for while
	// `editing_` is still false, that loss finds nothing to cancel.
	editing_ = false;
	requestKeyboardFocus();
	// THE NEW CLAIM BEFORE THE OLD ONE GOES, so a begin during an edit never hands the keyboard to the
	// host for the instant between them.
	claim_ = std::make_unique<KeyboardClaim>(*this);
	text_ = device::clampMacroName(initial);
	anchor_ = 0;
	caret_ = text_.size();
	scroll_ = 0.0f;
	editing_ = true;
	redraw();
}

void TextField::eraseSelection() {
	const size_t from = std::min(anchor_, caret_);
	const size_t to = std::max(anchor_, caret_);
	text_.erase(from, to - from);
	caret_ = from;
	anchor_ = from;
}

void TextField::textInput(const std::string& text) {
	if (!editing_) {
		return;
	}
	const std::string utf8 = asUtf8(text);

	// Code point by code point, so a character that would cross the cap is refused WHOLE — half of
	// one would leave a name that is not UTF-8. The first refusal ends the input: an input method's
	// composed string is kept as a prefix, never as a prefix with a hole in it.
	bool changed = false;
	for (size_t i = 0; i < utf8.size();) {
		const size_t length = sequenceLength(static_cast<unsigned char>(utf8[i]));
		const std::string_view glyph(utf8.data() + i, length);
		i += length;
		// CONTROL CHARACTERS ARE KEYS, NOT TEXT. X11 sends Return, Escape and Backspace here as "\r",
		// "\x1b" and "\b" before it sends the key; the key is what acts on them.
		if (length == 1 && (static_cast<unsigned char>(glyph[0]) < 0x20u || glyph[0] == 0x7F)) {
			continue;
		}
		if (hasSelection()) {
			eraseSelection();
			changed = true;
		}
		if (text_.size() + length > device::kMacroNameMax) {
			break;
		}
		text_.insert(caret_, glyph);
		caret_ += length;
		anchor_ = caret_;
		changed = true;
	}
	if (changed) {
		redraw();
	}
}

bool TextField::keyPress(const visage::KeyEvent& key) {
	if (!editing_) {
		return false;
	}
	// **A MAIN-MODIFIER CHORD IS THE HOST'S** — save, undo, the DAW's own shortcuts. The field has no
	// clipboard and no undo, so it has no claim on one, and swallowing it would take the key from
	// somebody who does.
	if (key.isMainModifier() || key.isAltDown()) {
		return false;
	}
	if (std::find(held_.begin(), held_.end(), key.keyCode()) == held_.end()) {
		held_.push_back(key.keyCode());
	}
	switch (key.keyCode()) {
		case visage::KeyCode::Return:
		case visage::KeyCode::KPEnter:
			finish(true);
			return true;
		case visage::KeyCode::Escape:
			finish(false);
			return true;
		case visage::KeyCode::Backspace:
			if (hasSelection()) {
				eraseSelection();
			}
			else if (caret_ > 0) {
				const size_t from = previousBoundary(text_, caret_);
				text_.erase(from, caret_ - from);
				caret_ = from;
				anchor_ = from;
			}
			break;
		case visage::KeyCode::Left:
			caret_ = hasSelection() ? std::min(anchor_, caret_) : previousBoundary(text_, caret_);
			anchor_ = caret_;
			break;
		case visage::KeyCode::Right:
			caret_ = hasSelection() ? std::max(anchor_, caret_) : nextBoundary(text_, caret_);
			anchor_ = caret_;
			break;
		case visage::KeyCode::Home:
			caret_ = 0;
			anchor_ = 0;
			break;
		case visage::KeyCode::End:
			caret_ = text_.size();
			anchor_ = caret_;
			break;
		default:
			break;
	}
	redraw();
	// **EVERY UNMODIFIED KEY IS CLAIMED**, including the ones that do nothing here. Keys travel up from
	// the focus until one is used, and the chain column deletes a device on Backspace and Delete: a
	// field that let a key it does not use travel would act on whatever sits under the name.
	return true;
}

bool TextField::keyRelease(const visage::KeyEvent& key) {
	const auto found = std::find(held_.begin(), held_.end(), key.keyCode());
	if (found != held_.end()) {
		held_.erase(found);
		return true;
	}
	// A release with no press here (the press went elsewhere, or came before the edit) is claimed only
	// while editing, by the rule `keyPress` uses: a chord is the host's.
	return editing_ && !key.isMainModifier() && !key.isAltDown();
}

void TextField::focusChanged(bool isFocused, bool /*wasClicked*/) {
	// Clicking elsewhere is not agreeing to a half-typed name.
	if (!isFocused && editing_) {
		finish(false);
	}
}

void TextField::visibilityChanged() {
	// HIDDEN MID-EDIT, the caption this field stands in for has to come back, and only a cancel tells
	// the owner so.
	if (!isVisible() && editing_) {
		finish(false);
	}
}

void TextField::finish(bool commit) {
	editing_ = false;
	anchor_ = caret_;
	// THE KEYBOARD GOES BACK BEFORE ANYBODY IS TOLD, so an owner acting on the result acts in a world
	// where the host already has its keys.
	claim_.reset();
	redraw();
	// **THE CALL IS THE LAST THING THIS FRAME DOES**, so an owner that HIDES the field from inside it
	// finds the edit already over. It is made on a COPY only so an owner that re-points `onCommit` or
	// `onCancel` from inside is not replacing the function that is running. None of this makes
	// DESTROYING the field from inside safe — visage's own frames are beneath this one on the stack;
	// see the header.
	if (commit) {
		if (commit_) {
			auto callback = commit_;
			callback(text_);
		}
	}
	else if (cancel_) {
		auto callback = cancel_;
		callback();
	}
}

void TextField::draw(visage::Canvas& canvas) {
	const float w = static_cast<float>(width());
	const float h = static_cast<float>(height());
	// The chip's own frame — `switch_chip.cpp`'s radius and line — lit, because a field being typed
	// into is the one thing on the surface with the keyboard.
	canvas.setColor(kVoid);
	canvas.roundedRectangle(0.0f, 0.0f, w, h, 3.0f);
	canvas.setColor(editing_ ? kAccent : withAlpha(kLine, 0.8f));
	canvas.roundedRectangleBorder(0.0f, 0.0f, w, h, 3.0f, 1.0f);

	// **THE CAPTION'S FACE AND TRACKING, NOT ITS CAPITALS.** A knob prints `upper(label)`, but the name
	// is stored as typed: a field that capitalised would hide the very thing being chosen.
	const visage::Font& face = font(kKnobLabelSize);
	const float spacing = kKnobLabelTracking * static_cast<float>(face.size());

	// Pen positions at every code-point boundary, so the caret and the selection land between glyphs.
	float caretX = 0.0f;
	float anchorX = 0.0f;
	float pen = 0.0f;
	for (size_t i = 0;; i = nextBoundary(text_, i)) {
		if (i == caret_) {
			caretX = pen;
		}
		if (i == anchor_) {
			anchorX = pen;
		}
		if (i >= text_.size()) {
			break;
		}
		const size_t next = nextBoundary(text_, i);
		pen += advanceOf(face, std::string_view(text_).substr(i, next - i)) + spacing;
	}

	// SLIDE, DON'T SHRINK: once the name outgrows the field, the text moves so the caret stays in view.
	// Less the caret's own pixel, so a caret at the very end sits inside the clip below, not on its edge.
	const float room = std::max(1.0f, w - 2.0f * kPadX - kCaretWidth);
	if (caretX - scroll_ > room) {
		scroll_ = caretX - room;
	}
	if (caretX < scroll_) {
		scroll_ = caretX;
	}
	const float originX = kPadX - scroll_;

	// **CLIPPED TO THE INNER RECT**, so a glyph slid half out of view is cut at the pad rather than
	// painted over it and over the border. `explore_stage.cpp` clips its chart the same way.
	canvas.saveState();
	canvas.trimClampBounds(kPadX, 1.0f, std::max(0.0f, w - 2.0f * kPadX), std::max(0.0f, h - 2.0f));

	if (editing_ && hasSelection()) {
		const float from = std::min(anchorX, caretX);
		const float to = std::max(anchorX, caretX);
		canvas.setColor(withAlpha(kValue, 0.35f));
		canvas.rectangle(originX + from, 2.0f, to - from, h - 4.0f);
	}

	canvas.setColor(kInk);
	pen = originX;
	for (size_t i = 0; i < text_.size();) {
		const size_t next = nextBoundary(text_, i);
		const std::string glyph = text_.substr(i, next - i);
		const float advance = advanceOf(face, glyph);
		canvas.text(glyph, face, visage::Font::kLeft, pen, 0.0f, advance, h);
		pen += advance + spacing;
		i = next;
	}

	if (editing_ && !hasSelection()) {
		canvas.setColor(kInk);
		canvas.rectangle(originX + caretX, 3.0f, kCaretWidth, h - 6.0f);
	}
	canvas.restoreState();
}

} // namespace feathers::ui
