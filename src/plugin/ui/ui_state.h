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

#include "plugin/ui/widget_registry.h"
#include "plugin/params.h"
#include "plugin/ui/frame_watchers.h"

#include <visage/ui.h>

#include <cstddef>
#include <functional>

// The editor's own state: which sound it is talking about, and which surface is showing.
//
// NEITHER IS A PARAMETER, and neither must become one. Both are where the user's ATTENTION is, not
// something the engine reads or a patch stores — a session saved with sound C selected and Explore
// open plays exactly as one saved with A selected and Perform open. Making either a parameter would
// spend a permanent id on a highlight and put a UI focus into somebody's automation lane.
//
// Both are shared state all the same, because separate widgets have to agree about them: the stack
// shows which card is lit and Explore edits whatever that card names; the header switches surfaces
// and the stage draws whichever is chosen. So they live here, with the same watch/redraw idiom
// WidgetRegistry uses for parameters — a widget says it cares, and it is redrawn when the state moves.
//
// `FrameWatchers` USED TO LIVE HERE and now lives in `frame_watchers.h`, included above so every
// existing `#include "plugin/ui/ui_state.h"` still compiles. It moved because it is the half a
// device's card needs and these two are the half that needs `kMaxSounds` — see that file.

namespace feathers::ui {

class SoundSelection {
public:
	size_t slot() const noexcept { return slot_; }

	/// Out-of-range slots are ignored rather than clamped: a caller asking for a sound that does not
	/// exist has a bug, and silently selecting a neighbour would hide it behind a plausible highlight.
	void select(size_t slot) {
		if (slot >= kMaxSounds || slot == slot_) {
			return;
		}
		slot_ = slot;
		watchers_.notify();
		if (moved_) {
			moved_(slot_);
		}
	}

	/// Redraw `frame` whenever the selection moves.
	[[nodiscard]] Watch watch(visage::Frame* frame) { return watchers_.watch(frame); }

	/// AND ONE THING THAT IS NOT A REDRAW. Preview follows the selection, and the sound
	/// being previewed is held on the bridge where the audio thread can read it — so somebody has to
	/// carry the move across, from every road the selection can be moved down: a card, a clone, an
	/// undo, a crumb. This is that one place. A watcher could not do it: watchers redraw frames, and
	/// the thing that must change here is not a frame.
	///
	/// One callback rather than a list, set by the editor that owns this state, because there is one
	/// such consequence and a list would invite a second surface to grow behaviour behind everyone's
	/// back.
	void onMoved(std::function<void(size_t)> moved) { moved_ = std::move(moved); }

	/// HOW MANY FRAMES ARE ON THIS LIST. Read by the case that requires every watcher list in the
	/// editor to come back to its baseline across a card cycle.
	///
	/// IT EXISTS BECAUSE THIS LIST HAS NO EXIT. `forgetFrameTree` reaches the registry, the state
	/// channel and the modulation channel; it cannot reach this one. Every subscriber here is an
	/// editor-lifetime surface, which is a fact about who happens to have registered rather than a
	/// guarantee — and it is exactly the property the three shipped dangling-watcher bugs had, right
	/// up until a card became removable. A count that must return to where it started is what
	/// notices the day a dying frame joins.
	size_t watcherCountForTest() const noexcept { return watchers_.count(); }

	void clear() {
		watchers_.clear();
		moved_ = nullptr;
	}

private:
	size_t slot_ = 0;
	FrameWatchers watchers_;
	std::function<void(size_t)> moved_;
};

/// Which surface the stage is showing. The mock's `S.mode`, and the header's Explore/Perform switch
/// is what moves it.
class StageMode {
public:
	enum class Surface { Perform, Explore };

	Surface surface() const noexcept { return surface_; }
	bool exploring() const noexcept { return surface_ == Surface::Explore; }

	void show(Surface surface) {
		if (surface == surface_) {
			return;
		}
		surface_ = surface;
		watchers_.notify();
		if (changed_) {
			changed_();
		}
	}

	/// The editor's own reaction to a surface change — swap which stage is visible — rather than a
	/// frame redraw. The header is a watcher because it DRAWS the switch; the swap cannot wait for a
	/// tick, because on macOS a host that offers no timer never ticks and the circle would stay put
	/// while the switch lit up.
	void onChanged(std::function<void()> changed) { changed_ = std::move(changed); }

	[[nodiscard]] Watch watch(visage::Frame* frame) { return watchers_.watch(frame); }
	/// **THE EXIT THIS LIST DID NOT HAVE.** The note below says every subscriber here is an
	/// editor-lifetime surface, and that this is *"a fact about who happens to have registered rather
	/// than a guarantee"*. `HeaderBar` was that fact's only load-bearer and it never unsubscribed, so
	/// the guarantee rested on nobody ever destroying it early.
	///
	/// Now it can leave. `forgetFrameTree` still cannot reach this list — it knows the registry and the
	/// two channels — so a subscriber has to take itself off, which is what `~HeaderBar` does.
	void unwatch(const visage::Frame* frame) { watchers_.unwatch(frame); }

	/// HOW MANY FRAMES ARE ON THIS LIST. Read by the case that requires every watcher list in the
	/// editor to come back to its baseline across a card cycle.
	///
	/// IT EXISTS BECAUSE THIS LIST HAS NO EXIT. `forgetFrameTree` reaches the registry, the state
	/// channel and the modulation channel; it cannot reach this one. Every subscriber here is an
	/// editor-lifetime surface, which is a fact about who happens to have registered rather than a
	/// guarantee — and it is exactly the property the three shipped dangling-watcher bugs had, right
	/// up until a card became removable. A count that must return to where it started is what
	/// notices the day a dying frame joins.
	size_t watcherCountForTest() const noexcept { return watchers_.count(); }

	void clear() {
		watchers_.clear();
		changed_ = nullptr;
	}

private:
	Surface surface_ = Surface::Perform;
	FrameWatchers watchers_;
	std::function<void()> changed_;
};

} // namespace feathers::ui