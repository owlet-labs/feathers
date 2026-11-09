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

#include "plugin/ui/takeover_stack.h"

#include "plugin/ui/theme.h"

#include <utility>
#include <vector>

namespace feathers::ui {

namespace {

/// **DESTROY A PARK LIST BY SWAPPING IT EMPTY FIRST — backlog HL.** The same helper as
/// `expanded_overlay.cpp`'s, for the same construction, and the comment is repeated rather than
/// shared because the fault it prevents is a property of THIS vector: libstdc++'s `clear()` destroys
/// `[_M_start, _M_finish)` with `_M_finish` still at the old end, so a `push_back` reached from
/// inside one of those destructors reallocates and frees the buffer the destroy loop is walking.
///
/// **IN `ExpandedOverlay` THAT IS A CRASH THAT WAS HIT; HERE IT IS HARDENED AND UNPROVEN, and the reason
/// it is unproven is worth writing down because it will expire.** This stack has the identical shape
/// — `dismissTo` parks into `retired_`, `reapRetired` and `~TakeoverStack` empty it — and its own
/// header records that it inherited `retired_` from `ExpandedOverlay` a beat late. What it does NOT
/// have today is content that owns a host: the only surface anywhere that asks for
/// `SurfaceNeed::SelfContained` is `RackEditor`'s device browser (`rack_editor.cpp`, one call site),
/// and a `BrowserView` presents nothing, so no destructor of anything this stack holds can re-enter
/// it. That is a fact about today's callers, not about this construction — the day a takeover holds
/// a panel-opening device the road is the overlay's exactly. So: swapped, not cleared, and no case
/// here claims to demonstrate it.
template <typename T>
void reapParked(std::vector<T>& parked) {
	std::vector<T> going;
	going.swap(parked);
}

} // namespace

TakeoverStack::TakeoverStack() {
	setVisible(false);

	// IT TAKES EVERY PRESS THAT ITS CONTENT DOES NOT, which is the exact opposite of what
	// `ExpandedOverlay` had to do — and the opposite for the same reason. There the editor stayed
	// reachable beneath, so a layer covering it in order to dim it would have swallowed the press
	// that arms a source. Here the editor is not beneath: it is replaced. A press landing on it
	// through this frame would reach a control the player cannot see.
	setIgnoresMouseEvents(false, true);

	onMouseDown() = [this](const visage::MouseEvent& e) {
		pressAt(e.position.x, e.position.y);
	};

	// AND IT TAKES THE WHEEL. `RackEditor::onMouseWheel` scrolls the card stack, and a wheel over a
	// child bubbles up to it — so without this, scrolling over a browser would move a rack the player
	// cannot see, and they would come back to a surface that had wandered.
	onMouseWheel() = [this](const visage::MouseEvent&) {
		return !entries_.empty();
	};

	setAcceptsKeystrokes(true);
	onKeyPress() = [this](const visage::KeyEvent& e) {
		if (e.keyCode() != visage::KeyCode::Escape) {
			return false;
		}
		// WITH THE EDITOR'S STACK, ITS TOP — one level of this stack when that is what opened last (NN).
		if (EscapeStack* stack = escapeStack_ ? escapeStack_() : nullptr; stack != nullptr) {
			return stack->closeTop() || (onEscapeWhenEmpty != nullptr && onEscapeWhenEmpty());
		}
		// ESC POPS ONE LEVEL, NOT ALL OF THEM. A stack is the reason: two levels deep, the player
		// means "back" by it, and "close everything" is a thing no other back-stack does.
		if (!entries_.empty()) {
			back();
			return true;
		}
		return onEscapeWhenEmpty != nullptr && onEscapeWhenEmpty();
	};
}

void TakeoverStack::reapRetired() {
	// CLEARED IN ONE GO RATHER THAN ONE PER CALL — but NOT by `clear()`, and the claim that stood here
	// is the one `ExpandedOverlay` made and was wrong about: *"a retired entry's destructor cannot
	// retire anything else: it is already out of the tree and its callback has already run."* Out of
	// the tree is not out of reach. A retired entry can OWN a host — that is exactly what a
	// `ChainColumn` inside a retired panel is one layer up — and that host's destructor dismisses what
	// it presented, which can land back here. See `reapParked`, and backlog HL.
	reapParked(retired_);
}

TakeoverStack::~TakeoverStack() {
	// THE CHILDREN GO BEFORE THE VECTOR THAT OWNS THEM. `entries_` destroying its `unique_ptr`s
	// while this frame still lists them as children would leave a parent pointing at freed frames
	// for the rest of `~Frame`.
	//
	// AND NOTHING IS TOLD. `onDismiss` means "the player closed your surface"; an editor being torn
	// down is not that, and a guest already being destroyed is the worst possible moment to call
	// back into it.
	for (Entry& entry : entries_) {
		removeChild(entry.content.get());
	}
	// SWAPPED EMPTY, NOT CLEARED, for the reason in `reapParked`: an entry's destructor reaching a
	// host it owns can re-enter this stack, and "this frame is being destroyed" does not stop it — it
	// is precisely the moment those destructors run.
	reapParked(entries_);
	// AND ANYTHING STILL PARKED, including anything the line above parked on its way out.
	reapParked(retired_);
}

SurfaceKind TakeoverStack::kindAvailable(SurfaceNeed need) const {
	// ONE POLICY, HONESTLY STATED: this host does takeovers and nothing else. It does not answer
	// `Takeover` for `SpansGesture` — but note that it would not matter if it tried, because
	// `SurfaceHost::kindFor` corrects that above every implementor. This says the true thing anyway,
	// so that the day a subclass adds an overlay the two answers do not have to be untangled.
	if (need == SurfaceNeed::SpansGesture) {
		return SurfaceKind::Inline;
	}
	return SurfaceKind::Takeover;
}

SurfaceId TakeoverStack::presentImpl(std::unique_ptr<visage::Frame> content, SurfaceNeed,
                                     std::string title, std::function<void()> onDismiss) {
	// A SAFE MOMENT: nothing dismissed earlier can still be running, because a present is not reached
	// from inside a dismissal's callback — and if it ever were, the reap would be one frame later
	// rather than never.
	reapRetired();

	// THE ONE BELOW STOPS DRAWING. A takeover REPLACES; leaving the previous level visible would
	// paint two surfaces through each other, and the lower one would still be taking presses.
	if (!entries_.empty()) {
		entries_.back().content->setVisible(false);
	}

	const SurfaceId id = next_++;
	Entry entry;
	entry.id = id;
	entry.content = std::move(content);
	entry.title = std::move(title);
	entry.onDismiss = std::move(onDismiss);
	if (EscapeStack* stack = escapeStack_ ? escapeStack_() : nullptr; stack != nullptr) {
		entry.escape = stack->push([this, id] {
			for (const Entry& open : entries_) {
				if (open.id == id) {
					dismiss(id);
					return true;
				}
			}
			return false;
		});
	}

	addChild(entry.content.get());
	entry.content->setVisible(true);
	entries_.push_back(std::move(entry));

	setVisible(true);
	layoutTop();
	requestKeyboardFocus();
	redraw();
	if (onDepthChanged) {
		onDepthChanged();
	}
	return id;
}

void TakeoverStack::dismiss(SurfaceId id) {
	if (id == kNoSurface) {
		return;
	}
	for (size_t i = 0; i < entries_.size(); ++i) {
		if (entries_[i].id != id) {
			continue;
		}
		// EVERYTHING ABOVE IT GOES TOO. A level was pushed from inside the one being dismissed, so
		// it cannot outlive it — and leaving it would show a surface whose owner is gone.
		popTo(i);
		return;
	}
	// AN UNKNOWN ID IS SILENCE, not an error. A guest that kept an id across a dismissal it did not
	// cause — the player pressed Back — must be safe rather than lucky, and ids are never reused so
	// this cannot mean "some other surface".
}

void TakeoverStack::back() {
	if (entries_.empty()) {
		return;
	}
	popTo(entries_.size() - 1);
}

void TakeoverStack::popTo(size_t depth) {
	if (entries_.size() <= depth) {
		return;
	}

	// EVERYTHING THAT IS GOING COMES OUT FIRST, AND IS TOLD AFTERWARDS.
	//
	// **THE OBVIOUS SHAPE — pop one, tell it, look again — IS WRONG, AND ITS OWN CASE CAUGHT IT.**
	// `onDismiss` may present: a browser whose pick opens the thing it picked does exactly that. With
	// the loop still reading `entries_.size()` as its condition, the level the callback had just
	// pushed became the next thing the loop popped, so a surface was taken down by the act of opening
	// it. A guard on the new size cannot fix that either — it cannot tell a level that survived from
	// one that arrived.
	//
	// TAKING THEM OUT FIRST MAKES IT STRUCTURAL: the loop below walks a list that nothing can grow,
	// so a callback that presents is pushing onto a stack this pop has already finished reading.
	std::vector<Entry> going;
	while (entries_.size() > depth) {
		going.push_back(std::move(entries_.back()));
		entries_.pop_back();
	}

	// TOP-DOWN, which is the order a hand sees: the thing in front goes first.
	for (Entry& entry : going) {
		// REMOVED FROM THE TREE NOW, DESTROYED LATER. The frame stops drawing and stops taking presses
		// immediately, which is everything a dismissal owes the player; the object has to outlive the
		// callback, which may be the content's own — see `retired_`.
		removeChild(entry.content.get());
		entry.escape.release();
		++dismissals_;
		if (entry.onDismiss) {
			entry.onDismiss();
		}
	}

	// AND THEY ARE PARKED, NOT DROPPED. `going` destructing here is exactly the use-after-free this
	// stack shipped: it runs before `dismiss` returns into a caller that may BE the content.
	for (Entry& entry : going) {
		retired_.push_back(std::move(entry));
	}

	if (entries_.empty()) {
		setVisible(false);
	}
	else {
		entries_.back().content->setVisible(true);
		layoutTop();
	}
	redraw();
	if (onDepthChanged) {
		onDepthChanged();
	}
}

bool TakeoverStack::pressAt(float x, float y) {
	if (entries_.empty() || y >= kBarHeight) {
		// BELOW THE BAR IS THE CONTENT'S, and a press that reaches this frame there landed on space
		// the content did not claim. It is SWALLOWED rather than passed on — there is nothing behind
		// a takeover to pass it to, and a miss must not close, because Back is the only exit and a
		// full-window surface is mostly empty space.
		return false;
	}
	if (x < kBackWidth) {
		back();
		return true;
	}
	return false;
}

void TakeoverStack::resized() {
	layoutTop();
}

void TakeoverStack::layoutTop() {
	if (entries_.empty()) {
		return;
	}
	const int h = height() - static_cast<int>(kBarHeight);
	entries_.back().content->setBounds(0, static_cast<int>(kBarHeight), width(), h > 0 ? h : 0);
}

void TakeoverStack::draw(visage::Canvas& canvas) {
	// THE OTHER SAFE MOMENT, and the one that catches a stack left non-empty: a draw is never reached
	// from inside a mouse or key handler, so anything retired by the press that caused this repaint has
	// certainly returned. `ExpandedOverlay` reaps on `show` and `draw` for the same two reasons.
	reapRetired();

	if (entries_.empty()) {
		return;
	}
	const float w = static_cast<float>(width());

	// OPAQUE, AND THAT IS THE WHOLE DIFFERENCE FROM AN OVERLAY. A scrim is translucent because the
	// editor beneath must stay visible to be armed; this covers it because it has replaced it.
	canvas.setColor(kSurface);
	canvas.fill(0.0f, 0.0f, w, static_cast<float>(height()));

	canvas.setColor(kSurfaceRaised);
	canvas.rectangle(0.0f, 0.0f, w, kBarHeight);
	canvas.setColor(kLine);
	canvas.rectangle(0.0f, kBarHeight - 1.0f, w, 1.0f);

	canvas.setColor(kAccent);
	canvas.text("< BACK", font(10.0f), visage::Font::kLeft, 12, 0, static_cast<int>(kBackWidth),
	            static_cast<int>(kBarHeight));

	canvas.setColor(kInk);
	canvas.text(entries_.back().title.c_str(), font(11.0f), visage::Font::kCenter,
	            static_cast<int>(kBackWidth), 0, static_cast<int>(w - 2.0f * kBackWidth),
	            static_cast<int>(kBarHeight));
}

const std::string& TakeoverStack::titleForTest() const {
	static const std::string kEmpty;
	return entries_.empty() ? kEmpty : entries_.back().title;
}

visage::Frame* TakeoverStack::topForTest() const {
	return entries_.empty() ? nullptr : entries_.back().content.get();
}

} // namespace feathers::ui
