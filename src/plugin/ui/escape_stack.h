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

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

// **ESC CLOSES THE MOST RECENTLY OPENED THING, WHATEVER KIND IT IS** (backlog NN: Esc treats everything it can
// close equally and closes the most recently opened; before this, with a window open and a modulator assign armed
// "behind it", Esc closed the window, not the assign).
//
// WHAT WAS THERE: a fixed priority made of WHERE the handlers sat. The overlay took Esc before the editor root
// whenever it held the keyboard focus, and the root's "leave assign mode" ran only for what the overlay
// declined — so which thing closed depended on which frame the hand last pressed, not on what it last opened.
//
// WHAT IT IS NOW: ONE STACK PER EDITOR, in the order things opened. Everything Esc can close puts an entry on it
// when it opens and takes it off when it closes, BY WHATEVER ROAD it closes (its ✕, a click, a pick, Esc). Every
// Esc handler in the editor asks the stack to close its top; an empty stack hands the key back to the host.
// Bringing a thing to the front (raising a window, arming a different source) counts as opening it again.
//
// WHO IS ON IT: device windows (the overlay's panels), the takeover's levels (browsers), modulation assign mode
// (ONE entry for the whole follow-tree — one hand, one arm), menus, the preset prompt and the settings screen.
// A TYPE-IN IS NOT, and needs not be: it holds the keyboard while it edits and ends on any focus loss, so
// nothing can open after it without ending it first — it is the top by construction, and its own Esc (cancel)
// runs before any handler here.

namespace feathers::ui {

/// An entry on the stack. Monotonic and never reused, so a stale ticket can only miss.
using EscapeId = uint64_t;
inline constexpr EscapeId kNoEscape = 0;

class EscapeStack;

/// **ONE ENTRY, FOR ITS LIFETIME.** Held by the thing that opened; resetting it (or destroying the owner) takes
/// the entry off, so every road that closes a thing — not only Esc — leaves the stack in step with the screen.
/// Safe to outlive the stack: it holds the stack weakly.
class EscapeTicket {
public:
	EscapeTicket() = default;
	~EscapeTicket() { release(); }
	EscapeTicket(EscapeTicket&& other) noexcept;
	EscapeTicket& operator=(EscapeTicket&& other) noexcept;
	EscapeTicket(const EscapeTicket&) = delete;
	EscapeTicket& operator=(const EscapeTicket&) = delete;

	/// Off the stack, if it is still on it.
	void release();
	/// To the top: the thing was brought to the front, which is opening it again.
	void raise();
	/// Still on a living stack.
	[[nodiscard]] bool held() const;
	/// ESC ON THE STACK THIS TICKET IS ON — `EscapeStack::closeTop` for a holder that must not keep the
	/// stack itself (a menu's popup, which can outlive the editor's layers). False if the stack is gone.
	bool closeTop();
	[[nodiscard]] EscapeId id() const noexcept { return id_; }

private:
	friend class EscapeStack;
	struct Core;
	EscapeTicket(std::weak_ptr<Core> core, EscapeId id) : core_(std::move(core)), id_(id) {}
	std::weak_ptr<Core> core_;
	EscapeId id_ = kNoEscape;
};

class EscapeStack {
public:
	EscapeStack();
	~EscapeStack();
	EscapeStack(const EscapeStack&) = delete;
	EscapeStack& operator=(const EscapeStack&) = delete;

	/// **OPEN SOMETHING.** `close` is what Esc does to it; it returns whether it closed (or stepped back)
	/// anything. A thing that is already gone returns false, and its entry is dropped and the next one asked —
	/// so an owner that vanished without saying so costs nothing but a skipped entry, never a swallowed key.
	/// A `close` that stepped back without closing (a confirmation returning to its prompt) returns true and
	/// keeps its entry: the entry leaves only when its ticket does.
	[[nodiscard]] EscapeTicket push(std::function<bool()> close);

	/// **WHAT ESC DOES.** Close the most recently opened thing still open. False when there was nothing to
	/// close — the key is then not ours, and goes back to the host.
	bool closeTop();

	[[nodiscard]] size_t size() const noexcept;
	[[nodiscard]] bool empty() const noexcept { return size() == 0; }
	/// The top entry, `kNoEscape` when empty — for a case reading the ORDER rather than inferring it.
	[[nodiscard]] EscapeId topForTest() const noexcept;

private:
	std::shared_ptr<EscapeTicket::Core> core_;
};

class ModState;

/// **MODULATION ASSIGN MODE AS ONE ENTRY.** One hand, one arm (`ModState::arm`), so the whole follow-tree is
/// one thing on the stack: a hand's arm anywhere in it opens (or raises) the entry, the tree going unarmed by
/// any road takes it off, and Esc on it disarms the whole tree. Held by the editor that owns the root state,
/// declared after the stack's owner so it goes first.
class ArmEscape {
public:
	ArmEscape() = default;
	~ArmEscape() { detach(); }
	ArmEscape(const ArmEscape&) = delete;
	ArmEscape& operator=(const ArmEscape&) = delete;

	/// `root` must be the top of its follow-tree: it is the root's observer that every state's arm tells.
	void attach(ModState& root, EscapeStack& stack);
	void detach();

private:
	ModState* mod_ = nullptr;
	EscapeStack* stack_ = nullptr;
	EscapeTicket ticket_;
};

} // namespace feathers::ui
