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

#pragma once

#include <visage/ui.h>

#include <cassert>
#include <cstddef>
#include <span>
#include <memory>
#include <vector>

// WHO TO REDRAW WHEN A PIECE OF SHARED STATE MOVES — the redraw idiom, with nothing about what moved.
//
// SPLIT OUT OF ui_state.h SO THAT THE CARD LAYER CAN HAVE IT WITHOUT THE SYNTH. That file held three
// unrelated classes behind one include: this one, which is about frames and nothing else, and
// `SoundSelection` and `StageMode`, which are about Feathers' own surfaces. The single `#include
// "plugin/params.h"` the other two need was therefore paid by every widget that only wanted to be
// redrawn — measured with a link probe: it is one of four chokepoints putting the whole plugin
// parameter header, and the CLAP SDK behind it, into files that use neither.
//
// A DEVICE'S CARD IS THE READER THIS SPLIT IS FOR. A docked device has no clap ids and no sounds; it
// still has widgets that must repaint when something they watch changes. That is this class and only
// this class.

namespace feathers::ui {

/// **A LIST A FRAME CAN BE TAKEN OFF.** Implemented by `FrameWatchers` and by `WidgetRegistry`, which
/// are the two kinds of watcher list in this editor and had no common type until now.
///
/// It exists so ONE token type can release a subscription to either. The registry's `unwatch` sweeps
/// every parameter rather than taking an id — a frame being destroyed does not remember what it
/// subscribed to — so both roads really do have the same shape from a token's side.
class WatchList {
public:
	virtual void unwatchFrame(const visage::Frame* frame) noexcept = 0;

protected:
	~WatchList() = default;
	WatchList() = default;
	WatchList(const WatchList&) = delete;
	WatchList& operator=(const WatchList&) = delete;
};

/// **A SUBSCRIPTION THAT ENDS ITSELF. Holding one is the only way to make one.**
///
/// THE PROBLEM IT REPLACES, measured 2026-09-01: `watch()` returned void, so a widget subscribed in
/// its constructor and was TRUSTED to unsubscribe in its destructor. Four did not (`HeaderBar`,
/// `SegmentedControl`, `Stepper`, `KnobTray`), and fixing those four moved the widget suite's ASan
/// crash count from 113 to 108 — the leak was most of the editor, not four widgets. Hand-fixing a
/// tail that long is not a fix; making the mistake unspellable is.
///
/// **SAFE IN BOTH DESTRUCTION ORDERS, WHICH A NAIVE TOKEN IS NOT.** The obvious version holds a raw
/// `WatchList*` and dereferences it in its destructor — which turns "widget outlives list" from a
/// silent stale entry into a crash, trading one bug for a louder one. This holds a `weak_ptr` to a
/// sentinel the list owns, so a token that outlives its list does NOTHING rather than reaching into a
/// corpse. The list's own empty-at-destruction assert still catches the ordering mistake; the token
/// just refuses to be the thing that dies of it.
class Watch {
public:
	Watch() = default;
	Watch(std::weak_ptr<WatchList*> list, visage::Frame* frame) noexcept
	    : list_(std::move(list)), frame_(frame) {}

	Watch(const Watch&) = delete;
	Watch& operator=(const Watch&) = delete;

	Watch(Watch&& other) noexcept : list_(std::move(other.list_)), frame_(other.frame_) {
		other.frame_ = nullptr;
	}

	/// **RE-SUBSCRIBING TO THE SAME LIST WITH THE SAME FRAME MUST NOT UNSUBSCRIBE IT**, and the
	/// obvious `release(); take(other);` does exactly that.
	///
	/// MEASURED 2026-09-05, the whole of the defect in which an effect's assign glow did not appear
	/// right away. `Knob::setModulation` ends in `watch_ = mod_->watch(this)`, and C++ evaluates
	/// the RIGHT side first: `watch()` sees the frame already on the list, takes its duplicate branch
	/// and hands back a token WITHOUT pushing — so the list still holds exactly one entry for this
	/// frame. Then this operator released the old token, whose `frame_` is that same frame, and
	/// `unwatch` took the one entry off. **The knob ends up holding a token for a subscription that no
	/// longer exists.** In the assembled editor that was 6 of 93 knobs — every knob in the fx lane —
	/// and arming repainted the 87 and not the six a hand was about to point at, which is the report
	/// exactly: right in content, never delivered, until some later edit repainted the lane for its
	/// own reasons. Completing a mapping is such an edit (`DeviceControl::setAmount` ends in
	/// `mod_->touched()`), which is why the glow appeared at the moment it became useless.
	///
	/// **THAT THE LANE'S CARDS ARE TOLD MORE THAN ONCE IS A DEDUCTION FROM THE MEASUREMENT, NOT A
	/// READING OF THE CALL SITES.** Under the broken operator, one call leaves the frame ON (the old
	/// token is empty, so its release is a no-op) and any further call leaves it OFF. The lane's six
	/// measured OFF and the other 87 measured ON, so the lane's cards take at least two
	/// `setModulation` calls and the rest of the editor's knobs take one. `FxLane::setModulation`
	/// telling the cards that already exist and `ensureCard` telling each card as it is built are the
	/// two candidates in the file; which pair actually fires was not measured and does not change the
	/// fix.
	///
	/// **SAME FRAME AND SAME LIST, BOTH.** A token re-pointed at the same frame on a DIFFERENT list
	/// must still come off the old one, or the old list keeps a raw pointer nobody will remove — the
	/// dangling read this class exists to end. The weak_ptr sentinels are compared with
	/// `owner_before` in both directions, which is control-block identity without a lock and is
	/// noexcept, as this operator must be.
	///
	/// FIXED HERE RATHER THAN AT THE KNOB, on this file's own terms: a call site could release first
	/// and get it right, and the next one would not. Nothing that holds a `Watch` can now spell the
	/// mistake.
	Watch& operator=(Watch&& other) noexcept {
		if (this != &other) {
			const bool sameEntry = frame_ != nullptr && frame_ == other.frame_
			                       && !list_.owner_before(other.list_)
			                       && !other.list_.owner_before(list_);
			if (!sameEntry) {
				release();
			}
			list_ = std::move(other.list_);
			frame_ = other.frame_;
			other.frame_ = nullptr;
		}
		return *this;
	}

	~Watch() { release(); }

	/// End the subscription early. Idempotent, and the same road the destructor takes.
	void release() noexcept {
		if (frame_ == nullptr) {
			return;
		}
		// THE LOCK IS THE WHOLE SAFETY PROPERTY: an expired sentinel means the list is gone, and there
		// is nothing to take ourselves off.
		if (const std::shared_ptr<WatchList*> alive = list_.lock(); alive && *alive != nullptr) {
			(*alive)->unwatchFrame(frame_);
		}
		frame_ = nullptr;
		list_.reset();
	}

private:
	std::weak_ptr<WatchList*> list_;
	visage::Frame* frame_ = nullptr;
};


/// Who to redraw when a piece of editor state moves. The registry's mechanism without its parameter
/// index, so the two kinds of shared state work the same way from a widget's side.
class FrameWatchers : public WatchList {
public:
	/// EMPTY AT DESTRUCTION IS THE INVARIANT, AND IT IS CHECKED RATHER THAN HOPED FOR.
	///
	/// Every watcher takes itself off in its own destructor, so a list with entries left on it means
	/// a watcher is going to OUTLIVE this object and call `unwatch` into freed memory. That is a
	/// member-order bug in whoever owns both, and it is INVISIBLE to an ordinary test run: reading a
	/// freed `std::vector` buffer does not fault without a sanitiser.
	///
	/// MEASURED, 2026-08-31: `Gui::Impl` declared `mod` BELOW `Engine rail`, so `ModState` died first
	/// and the Engine rail's knobs unwatched into a corpse. ASan reported a heap-use-after-free in 252
	/// of 517 widget cases. `ctest` was 72/72 through the whole thing and nothing in this repo runs a
	/// sanitiser, so the only reason it was caught is that somebody went looking with one.
	///
	/// WHAT THIS ASSERT IS AND IS NOT, SAID EXACTLY, BECAUSE THE FIRST VERSION OF THIS COMMENT
	/// OVERSOLD IT AND THAT IS THE SAME MISTAKE AGAIN. **It is INERT in this repo's ordinary build**
	/// — `NDEBUG` is defined in 595 places in `build.ninja`, so `assert` compiles to nothing and
	/// `ctest` will never fire it. It earns its place only in a debug or sanitiser build.
	///
	/// THE ACTUAL GUARANTEE IS STRUCTURAL AND LIVES ELSEWHERE: `Gui::Impl` holds its `ModState` in a
	/// `ModStateHolder` BASE, and a base subobject is destroyed after every member, so no member can
	/// outlive it whatever order somebody declares things in. `RackGui` gets the same property from
	/// `mod_` being declared above `editor_`. This assert is a backstop for a third owner written
	/// later that does neither — not the thing keeping the two existing ones honest.
	~FrameWatchers() {
		// **THE SENTINEL DIES FIRST, DELIBERATELY.** Any token still holding a weak reference now sees
		// a null and skips its unwatch — which is what stops a widget outliving its list from reaching
		// into freed memory. The assert below still fires, because that ordering is still a bug; this
		// just makes it a diagnosable one rather than a corruption.
		*alive_ = nullptr;
		assert(frames_.empty() &&
		       "a FrameWatchers died with watchers still on it — whoever owns both declared this "
		       "ABOVE a widget that watches it, so the widget will unwatch into freed memory; see "
		       "the ordering note in this header");
	}

	FrameWatchers() = default;

	FrameWatchers(const FrameWatchers&) = delete;
	FrameWatchers& operator=(const FrameWatchers&) = delete;

	/// **SUBSCRIBE, AND HOLD WHAT COMES BACK.** `[[nodiscard]]` is the enforcement: a call site that
	/// drops the token no longer compiles, so "forgot to unwatch" is not a mistake this codebase can
	/// express any more. Storing it in a member is the whole discipline.
	[[nodiscard]] Watch watch(visage::Frame* frame) {
		if (frame == nullptr) {
			return {};
		}
		for (const visage::Frame* existing : frames_) {
			if (existing == frame) {
				// ALREADY ON: hand back a token that releases it, so the second holder ending is not
				// silently a no-op for the first. Duplicate subscription stays impossible either way.
				//
				// **AND THIS BRANCH IS HALF OF A BUG THAT SHIPPED — see `Watch::operator=`.** The
				// single most common holder of one of these tokens is a MEMBER being re-assigned by
				// the same widget (`watch_ = list.watch(this)`), and this branch hands that widget a
				// token for an entry it already has. Releasing the old member then took that entry
				// off, so subscribing twice unsubscribed. Fixed in the operator, where the two tokens
				// can be compared; nothing is wrong with returning a live token from here.
				return Watch(alive_, frame);
			}
		}
		frames_.push_back(frame);
		return Watch(alive_, frame);
	}

	void unwatchFrame(const visage::Frame* frame) noexcept override { unwatch(frame); }

	/// Redraw every watcher, and report how many. The count exists for the same reason
	/// WidgetRegistry::notify returns one: a repaint is unobservable headlessly, so the only way to
	/// pin that the signal was actually delivered is to have the delivery say so.
	size_t notify() const {
		size_t told = 0;
		for (visage::Frame* frame : frames_) {
			told += redrawTree(frame);
		}
		return told;
	}

	/// REDRAW A WATCHER AND EVERYTHING UNDER IT, and report how many frames that was.
	///
	/// `Frame::redraw()` MARKS ONE FRAME. `redrawAll()` walks the subtree, and the difference is a
	/// whole class of bug: a widget that subscribes here and then grows a child which draws from the
	/// same state gets repainted while its child does not.
	///
	/// FOUND FROM THE SYMPTOM — the graph might not be redrawing — after two wrong fixes. OWLTT's plot moved out of `OwlttCard::draw` into a child frame, and the lane
	/// subscribes THE CARD. So a bar drag set the value, the bridge marked state dirty, the notify
	/// redrew the card's head and knobs, and the picture never repainted: the control worked and
	/// nothing on screen said so. The meters died the same way and for the same reason.
	///
	/// THE EVIDENCE THAT IT WAS ALREADY BITING: `env_card.cpp:153-155` and `mseg_card.cpp:126-128`
	/// each subscribe the card AND every value box inside it, one at a time. That is this bug, worked
	/// around per widget by whoever hit it first, and it is why those lines look like belt and
	/// braces rather than what they are.
	///
	/// AND THE COUNT IS OF FRAMES, NOT OF WATCHERS, which is what makes the fix observable at all: a
	/// repaint cannot be seen headlessly, so the only honest evidence is a number produced BY the
	/// notification. A count of watchers would be identical before and after this change.
	static size_t redrawTree(visage::Frame* frame) {
		if (frame == nullptr) {
			return 0;
		}
		frame->redraw();
		size_t told = 1;
		for (visage::Frame* child : frame->children()) {
			told += redrawTree(child);
		}
		return told;
	}

	/// FORGET ONE WATCHER. It exists because a surface can now OUTLIVE ITS SUBSCRIPTION THE WRONG WAY
	/// ROUND: every watcher used to live as long as the editor, and the expanded-view overlay
	/// destroys its content on dismissal. A list holding a freed frame is a use-after-free the next
	/// time anything moves — `notify` calls `redraw()` on every entry — so a subscriber that can die
	/// must be able to leave.
	void unwatch(const visage::Frame* frame) {
		for (size_t i = 0; i < frames_.size(); ++i) {
			if (frames_[i] == frame) {
				frames_.erase(frames_.begin() + static_cast<std::ptrdiff_t>(i));
				return;
			}
		}
	}

	/// Whether `frame` is on the list. Exists for the tests: "this surface repaints when that state
	/// moves" is otherwise unassertable without a window, because visage's redraw() is a no-op on a
	/// frame that is not being drawn — so what a headless test can check is the SUBSCRIPTION, and
	/// this is what lets it.
	bool watches(const visage::Frame* frame) const {
		for (const visage::Frame* existing : frames_) {
			if (existing == frame) {
				return true;
			}
		}
		return false;
	}

	/// How many are listening. The same number `notify` returns, available without notifying — so a
	/// case can state a subscription's effect on the count before and after rather than inferring it.
	size_t count() const noexcept { return frames_.size(); }

	/// THE LIST ITSELF, for the one case that must ask the same question `notify` answers by a
	/// different route — see `expectedStateFrames`. A count derived from this cannot go stale the way
	/// the literal it replaced did.
	std::span<visage::Frame* const> framesForTest() const { return frames_; }

	/// Forget every watcher. Called from the editor's teardown, because the frames go with it.
	void clear() { frames_.clear(); }

private:
	std::vector<visage::Frame*> frames_;

	/// **THE SENTINEL EVERY TOKEN HOLDS A `weak_ptr` TO.** It carries a pointer to this list and is
	/// EMPTIED in the destructor, so a token that outlives its list finds a null and does nothing.
	/// One allocation per list, none per token.
	std::shared_ptr<WatchList*> alive_ = std::make_shared<WatchList*>(this);
};

} // namespace feathers::ui
