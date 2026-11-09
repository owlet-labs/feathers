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

#include "plugin/params.h"
#include "plugin/ui/frame_watchers.h"
#include "plugin/ui/ui_state.h"

#include <visage/ui.h>

#include <array>
#include <span>
#include <vector>

// Which widgets care about which parameter.
//
// This replaces a coincidence. The debug panel builds one row per parameter in table order, so a
// row's position in the vector WAS its parameter index and the diff could redraw `rows[index]`. That
// held for exactly as long as the editor was a list of parameters in order — and the design language
// ends it immediately: the engine dropdown watches OscType from the header, the Engine rail shows the
// voice parameters somewhere else entirely, and a knob and its readout are two widgets watching one
// value. Position stops meaning anything the moment any of that is true.
//
// So widgets say what they are interested in, and the diff tells whoever asked. Main thread only,
// like everything else in the editor.

namespace feathers::ui {

class WidgetRegistry : public WatchList {
public:
	/// **THE SENTINEL DIES FIRST** — see `~FrameWatchers`. A token outliving this registry then finds a
	/// null and does nothing, rather than sweeping a destroyed vector of vectors.
	~WidgetRegistry() { *alive_ = nullptr; }

	/// Note that `frame` draws something derived from `id`. A widget may watch several parameters and
	/// a parameter may have several watchers; both are normal.
	///
	/// Unknown ids are ignored rather than asserted: the caller is UI code naming a parameter, and the
	/// bridge already refuses anything unaddressable, so the only way to get here with a bad id is a
	/// typo that should cost a dead control rather than a crash in a musician's session.
	/// **SUBSCRIBE, AND HOLD WHAT COMES BACK** — see `ui::Watch`. `[[nodiscard]]` is what makes
	/// forgetting unspellable; before it, four widgets subscribed here and never left.
	[[nodiscard]] Watch watchToken(clap_id id, visage::Frame* frame) {
		watch(id, frame);
		return Watch(alive_, frame);
	}

	void unwatchFrame(const visage::Frame* frame) noexcept override { unwatch(frame); }

	void watch(clap_id id, visage::Frame* frame) {
		const int index = paramIndexOf(id);
		if (index < 0 || frame == nullptr) {
			return;
		}
		std::vector<visage::Frame*>& watchers = watchers_[static_cast<size_t>(index)];
		// A widget registering twice for one parameter would be redrawn twice per change.
		for (const visage::Frame* existing : watchers) {
			if (existing == frame) {
				return;
			}
		}
		watchers.push_back(frame);
	}

	/// Redraw everything watching the parameter at this table index, and report HOW MANY it told.
	/// Indices come from ParamBridge::refresh(), which is why this takes a position rather than an id.
	///
	/// THE COUNT IS RETURNED SO THAT THE NOTIFICATION CAN BE OBSERVED AT ALL. visage's redraw() does
	/// nothing to a frame that is not being drawn, so no headless venue can watch a repaint happen —
	/// which left the one line that JOINS a parameter change to its widgets unpinned, and deleting it
	/// kept the whole suite green while breaking every control's response to automation. A count
	/// produced BY the notification cannot be had without it, which a separate counter beside the
	/// call could not promise.
	size_t notify(uint32_t index) const {
		if (index >= watchers_.size()) {
			return 0;
		}
		for (visage::Frame* frame : watchers_[index]) {
			frame->redraw();
		}
		return watchers_[index].size();
	}

	/// Who watches a parameter — which is also the only way to FIND a control without restating the
	/// layout that placed it.
	///
	/// A venue that checks event ROUTING has to ask "where is the Pos X knob" and then ask the tree
	/// "what would a click there reach", and those two questions must be answered by different code or
	/// the test is just the layout agreeing with itself. This is the first half; Frame's own
	/// frameAtPoint is the second.
	std::span<visage::Frame* const> watchers(clap_id id) const {
		const int index = paramIndexOf(id);
		if (index < 0) {
			return {};
		}
		return watchers_[static_cast<size_t>(index)];
	}

	/// FORGET ONE FRAME, wherever it appears. The exit this class did not have.
	///
	/// `FrameWatchers::unwatch` was written months ago with a comment explaining exactly why a
	/// subscriber that can die must be able to leave. THIS CLASS, WHICH HOLDS RAW `Frame*` IN THE SAME
	/// WAY AND CALLS `redraw()` ON THEM IN THE SAME WAY, never got one — so the lesson was learnt on
	/// one list and not applied to the other sitting beside it. See `forgetFrameTree` below.
	///
	/// It sweeps every parameter rather than taking an id, because the caller is a frame being
	/// destroyed and a frame does not remember what it subscribed to. Asking it to would be the same
	/// bookkeeping that produced the bug.
	void unwatch(const visage::Frame* frame) {
		if (frame == nullptr) {
			return;
		}
		for (std::vector<visage::Frame*>& watchers : watchers_) {
			for (size_t i = 0; i < watchers.size(); ++i) {
				if (watchers[i] == frame) {
					watchers.erase(watchers.begin() + static_cast<std::ptrdiff_t>(i));
					break; // watch() refuses duplicates, so there is at most one per parameter
				}
			}
		}
	}

	/// EVERY REGISTRATION, SUMMED ACROSS EVERY PARAMETER. Read by the case that requires each of the
	/// editor's watcher lists to return to its baseline across a card cycle: this list is per-param,
	/// so a total is the only shape comparable with the others.
	size_t totalWatchersForTest() const noexcept {
		size_t total = 0;
		for (const std::vector<visage::Frame*>& watchers : watchers_) {
			total += watchers.size();
		}
		return total;
	}

	/// Forget every registration. Called when the editor is torn down, because the frames these point
	/// at go with it.
	void clear() {
		for (std::vector<visage::Frame*>& watchers : watchers_) {
			watchers.clear();
		}
	}

private:
	/// THE SENTINEL TOKENS HOLD — see `FrameWatchers`'s for why it is emptied rather than trusted.
	std::shared_ptr<WatchList*> alive_ = std::make_shared<WatchList*>(this);

	std::array<std::vector<visage::Frame*>, kParamCount> watchers_;
};


/// TAKE A FRAME AND EVERYTHING UNDER IT OFF BOTH REDRAW LISTS, before it is destroyed.
///
/// BACKLOG 19, and the reason it is one function over both lists and the whole subtree rather than
/// three careful call sites. The crash was `ParamControl::setAmount` -> `ModState::touched()` ->
/// `FrameWatchers::notify()`, which calls `redraw()` on every entry; `FxLane::ensureCard` subscribes
/// each card it builds and then frees it on a later pass, and nothing in between removed it.
///
/// COUNTED BEFORE FIXING, because the shape of the fix follows from the count: THIRTY subscription
/// sites across the editor, and exactly ONE call to `unwatch` anywhere in `src/` -- the expanded
/// overlay's. Every other subscriber was written when every watcher outlived the editor, which was
/// true until cards became removable. So this is not one forgotten line; it is a mechanism whose
/// exit was optional, used once, in a codebase that grew three kinds of dying frame.
///
/// A DYING FRAME DOES NOT REMEMBER WHAT IT SUBSCRIBED TO, and it must not have to. A knob subscribes
/// itself in its constructor, `setModulation` subscribes it again to a different list, and its card
/// subscribes separately -- so a per-site unregister is per-site bookkeeping, which is the same
/// bookkeeping that produced the bug. Walking the tree needs no memory: whatever is under the frame
/// is what is about to be freed, by construction.
///
/// AND IT WALKS THE SUBTREE BECAUSE THE ROOT IS NOT THE PROBLEM. The overlay's existing fix unwatched
/// `content_` and stopped there, which is correct for the frame it names and misses every knob inside
/// it -- `Knob::setModulation` puts each one on the state list individually. A fix that handles the
/// object you were thinking about and not its children is how this bug survives being fixed.
///
/// CALL IT WHILE THE FRAME IS STILL ALIVE. The children are read from the frame, so this must run
/// before the destructor, not after -- at retirement rather than at the free.
///
/// THREE LISTS, NAMED SEPARATELY, AND EVERY CALLER MUST SAY SOMETHING ABOUT EACH — which is the
/// whole point of the third parameter existing rather than the caller making a second call.
///
/// IT USED TO TAKE TWO, AND THE SECOND HAD NO IDENTITY. `watchers` meant the STATE channel at
/// `fx_lane.cpp` and the MODULATION channel at `mod_panel.cpp` — the same slot, two different lists,
/// and nothing at either site to say which. So a caller that needed both had to make TWO CALLS, and
/// at every call site "this list does not apply here" and "I forgot this list" looked exactly alike.
///
/// THAT AMBIGUITY IS NOT HYPOTHETICAL; IT SHIPPED THREE TIMES, all in one week:
///   - `FxLane::ensureCard` cleaned the state list and not the modulation one. A deleted card left
///     its knobs on `ModState`'s list and the next `focus()` walked them — SIGSEGV (96d94be).
///   - `ExpandedOverlay::dismissBy` did the same, behind a comment asserting the knobs were on "the
///     SAME list" (25282de).
///   - and the same split produced a defect with no free at all: `FxControl::setAmount` wrote an
///     amount to a list nobody on the modulation channel was told about (683070d).
///
/// ONE ROAD, THREE NAMED DESTINATIONS. A caller that has no state channel now writes `nullptr` in the
/// state slot, which a reader can see and disagree with — where a missing second call is invisible.
/// The two sites that needed two calls collapse to one each.
///
/// Any of the three may be null: `ModPanel` genuinely is on no state channel, and an `ExpandedView`
/// genuinely subscribes to no parameter (its knobs are built through the `device::Modulatable`
/// constructor, which deliberately does not register — knob.cpp, controls.cpp). Those nulls are
/// declarations now rather than omissions.
inline void forgetFrameTree(const visage::Frame* root, WidgetRegistry* registry,
                            FrameWatchers* state, FrameWatchers* modulation) {
	if (root == nullptr) {
		return;
	}
	if (registry != nullptr) {
		registry->unwatch(root);
	}
	if (state != nullptr) {
		state->unwatch(root);
	}
	// AND THE MODULATION LIST, WHICH IS NOT THE STATE ONE. `Knob::setModulation` puts every knob on
	// `ModState`'s watchers so the glow follows arming; that list is joined by CHILDREN THE OWNER
	// NEVER SEES, which is why no owner can be trusted to remember it and why it is a parameter here.
	//
	// A CALLER MAY LEGITIMATELY PASS THE SAME LIST TWICE and it costs nothing: `watch()` refuses
	// duplicates, so `unwatch` has at most one entry to remove and the second call is a no-op.
	if (modulation != nullptr) {
		modulation->unwatch(root);
	}
	for (const visage::Frame* child : root->children()) {
		forgetFrameTree(child, registry, state, modulation);
	}
}

} // namespace feathers::ui
