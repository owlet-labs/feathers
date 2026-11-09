/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "plugin/ui/expanded_overlay.h"

#include <visage_ui/frame.h>

#include <functional>
#include <memory>
#include <string>

namespace visage {
class ApplicationWindow;
}

// A DETACHED WINDOW FOR AN EXPANDED VIEW — the thing the overlay was built to become.
//
// THE DAY THE OVERLAY LANDED, a full popup was the stated next step. This is that,
// and it is a HOST rather than a rewrite: it takes the same `ExpandedView` the overlay takes, and
// the content does not know which one has it. `expanded_overlay.h` lists the five properties kept to
// make that true; all five still hold, which is why this file is short.
//
// THE SAME SURFACE AS `ExpandedOverlay`, deliberately: `show`, `dismiss`, `open`, `setStateWatchers`,
// `onDismiss`. The two are interchangeable from the editor's side, which is what lets the overlay
// stay as the default and the fallback.
//
// WHY THE OVERLAY IS NOT BEING REPLACED. CLAP has no model for a secondary plugin-owned window:
// `is_floating` describes the MAIN editor, so there is no `set_transient` for this one, the host does
// not know it exists, and focus behaviour differs per host and window manager. **There will be hosts where this misbehaves
// and the overlay is the answer there**, so this is a switch and not a migration.
//
// TWO THINGS THAT HAD TO BE MEASURED BEFORE ANY OF THIS COULD WORK, both in that document:
//   1. `WindowX11::processPluginFdEvents` drained the process-global X11 queue and DROPPED events
//      belonging to other windows — a second window drew once and went deaf. Fixed in the visage
//      fork (`7f63ac0`), with a case and a known-bad.
//   2. `Canvas::pairToWindow` asserts `BGFX_CAPS_SWAP_CHAIN`, which bgfx's GL backend historically
//      does not advertise. **Probed on this machine: YES.** One machine, one driver — evidence the
//      road exists, not that it is paved everywhere. The assert means an unsupported backend fails
//      loudly rather than drawing nothing.

namespace feathers::ui {

class FrameWatchers;
class ModState;

/// THE GROUND A DETACHED PANEL STANDS ON, AND NOTHING ELSE DRAWS IT.
///
/// visage carries pixels forward: `Layer::clearInvalidRectAreas` paints over the rectangles a frame
/// marked dirty and over nothing else, and there is no `setViewClear` in the fork — so any pixel no
/// shape covers keeps whatever was last written there, for the life of the window. In the EDITOR that
/// is invisible, because `gui.cpp` paints a full-window ground under everything ("just the ground the
/// surfaces are drawn on"), and the overlay host puts the same `ExpandedView` on top of it. A
/// detached window has no such ground, and `visage::Frame::draw` is a no-op by default.
///
/// MEASURED, not reasoned: photographed at rest, a popout is pure (0,0,0) at every pixel its panel
/// does not itself paint — rows above the graph, below the knob rows, and both margins. That is not
/// a colour anything chose; it is memory nobody wrote. Draw one transient thing there — a popup menu
/// — and it is permanent. A screenshot from Bitwig showed exactly that: the crossover dropdown still on
/// screen after it closed, cut off where the graph below it redraws over its own opaque rectangle.
///
/// FULL REDRAW CANNOT REACH IT, which is why this is a frame and not a damage fix.
/// `FEATHERS_FULL_REDRAW` invalidates every layer every frame, so every SHAPE is re-submitted — and
/// there was no shape over the ground to re-submit. Verified: the artefact reproduces with the flag
/// on, and a window resize (which destroys and recreates the layer texture) is what clears it.
///
/// FIRST CHILD, so everything else draws over it: visage draws a region's sub-regions in the order
/// they were added.
class PopoutGround : public visage::Frame {
public:
	PopoutGround() { setIgnoresMouseEvents(true, false); }
	void draw(visage::Canvas& canvas) override;
};

class WindowedExpanded {
public:
	WindowedExpanded();
	~WindowedExpanded();
	WindowedExpanded(const WindowedExpanded&) = delete;
	WindowedExpanded& operator=(const WindowedExpanded&) = delete;

	/// OPEN `content` IN ITS OWN WINDOW, replacing whatever was open. One at a time, matching the
	/// overlay — and here the reason is the same rather than merely consistent: two panels for one
	/// device is a state nothing in the editor can address.
	void show(std::unique_ptr<ExpandedView> content);

	/// CLOSE IT. Safe to call when nothing is open, and it invokes `onDismiss` exactly as the
	/// overlay's does, so the editor's teardown does not learn which host it had.
	void dismiss();

	[[nodiscard]] bool open() const noexcept { return content_ != nullptr; }
	/// IS THE OPEN WINDOW THIS DEVICE'S — matched on device and chain, as the overlay matches, so the
	/// expand mark can close the window it opened rather than replace it with a twin.
	[[nodiscard]] bool showsDevice(DeviceInstanceId device, DeviceNest nest) const {
		return content_ != nullptr && device != kNoInstance && content_->watchedDevice() == device &&
		       content_->nest() == nest;
	}

	/// TICK THE OPEN PANEL, and say whether it wants the screen. Both hosts carry this because an
	/// `ExpandedView` lives in one or the other and never in both, and a road installed on only one
	/// of them is a panel whose live readings die depending on a preference.
	/// The content's tick, and its HEADER TEXT followed in the window's title (ExpandedView::headerTitle) as the overlay
	/// follows it in its title strip.
	bool updateContent();

	/// THE STATE CHANNEL, because a detached window is NOT a descendant of the editor and cannot ride
	/// its repaint. This is property 5 from the overlay's header arriving as code: modulation reaches
	/// the panel because the content is on the notify road IN ITS OWN RIGHT, not because it happens
	/// to be parented under something that repaints.
	void setStateWatchers(FrameWatchers* watchers) { watchers_ = watchers; }

	/// THE MODULATION LIST IS A SECOND LIST, AND THIS HOST HAD NO POINTER TO IT EITHER.
	///
	/// `setStateWatchers` above is the state road, and it is the only one the first version of this
	/// class cleaned. `Knob::setModulation` separately puts every modulatable knob on `ModState`'s own
	/// `FrameWatchers`, so the glow follows arming — a DIFFERENT list, joined by children this class
	/// never sees, and nothing here could take them off again. `gui.cpp` calls
	/// `ExpandedView::setModulation` on the content BEFORE handing it to this host, so every knob in a
	/// windowed panel joined that list and stayed on it after the window closed: a freed `Frame*`
	/// repainted the next time anything armed.
	///
	/// THAT IS THE SAME DEFECT TWICE ALREADY FIXED ELSEWHERE — `96d94be` for `FxLane`'s cards, the
	/// crash on assigning an MSEG, and `25282de` for `ExpandedOverlay`. This host was written
	/// before both. It takes the STATE rather than a bare list for the same reason the overlay does:
	/// the pointer the editor has to give is `ModState*`, and the list is reached from it.
	void setModulationState(ModState* mod) { mod_ = mod; }

	std::function<void()> onDismiss;

	/// **A KEY PRESSED IN THIS WINDOW THAT NOTHING IN IT TOOK** — handed to the editor, which owns the shortcuts
	/// (backlog JB: Ctrl/Cmd+Z here walks the same history as in the editor). True if it was used; unset, the
	/// key goes back to the platform.
	std::function<bool(const visage::KeyEvent&)> onKey;
	/// The window's root frame while one is up, else null — what a key handler asks "is a field typing here?" of.
	[[nodiscard]] visage::Frame* root() const;

	/// TEST ONLY — the window, so a case can drive the close road and read the title without opening
	/// a real one on a developer's desktop.
	[[nodiscard]] visage::ApplicationWindow* windowForTest() const { return window_.get(); }

private:
	void closeWindow();
	void releaseRetired();

	std::unique_ptr<visage::ApplicationWindow> window_;
	std::unique_ptr<ExpandedView> content_;
	/// The title the window was last given, so a header that has not moved costs no call.
	std::string shownTitle_;

	/// **THE CONTENT'S SUBSCRIPTION, BESIDE THE CONTENT.** `content_` is REPLACED whenever a different
	/// panel is shown, so a flat list of tokens would hold the previous panel on the watcher list after
	/// it was destroyed — see `FxLane::cardWatches_` for the same shape and the bug it hid. Assigning
	/// a new token releases the old one at exactly the moment the old panel goes.
	Watch contentWatch_;

	/// ONE PER HOST RATHER THAN ONE PER WINDOW, and it outlives every window this object opens: it is
	/// added to each new window in `show()` and goes out with that window's frame tree. A member
	/// rather than an owned child of the window because the retired-window road frees a window later
	/// than it frees what it drew, and a ground that a retired window still points at must not be a
	/// dangling one.
	PopoutGround ground_;

	/// THE WINDOW THAT HAS BEEN CLOSED AND NOT YET DESTROYED, AND ITS CONTENT WITH IT.
	///
	/// A WINDOW MUST NOT BE DESTROYED FROM INSIDE ITS OWN CLOSE CALLBACK, and this class reaches that
	/// callback by two roads. `visage::ApplicationWindow::close()` reads
	/// `if (window_ && onCloseRequested().callback()) { removeFromWindow(); window_ = nullptr; }` —
	/// so it invokes our handler and then GOES ON TOUCHING ITSELF. Our handler calls `dismiss()`, and
	/// `dismiss()` used to free the `ApplicationWindow` outright; the two lines after the callback
	/// then ran on freed storage. MEASURED: a deterministic SIGSEGV, 12 assertions into the case
	/// below, on `close()` called from outside.
	///
	/// visage v1.0.0 (2026-11-05) MOVED THE HAZARD, IT DID NOT REMOVE IT: `close()` now only posts a
	/// close request, and the delivery is `handleCloseRequested` from the window's OWN pump into our
	/// handler — still a call into the window being dismissed. So a retired window is HIDDEN at once
	/// (v1's `close()` would leave it mapped until its message came round, then close nothing) and its
	/// close handler is cleared, and it is still freed only at a safe point. See `closeWindow`.
	///
	/// SO A DISMISSAL RETIRES THE WINDOW AND THE NEXT `show()` OR THE DESTRUCTOR FREES IT, by which
	/// time no close is in flight. It is `ExpandedOverlay::retired_` exactly — there the frame must
	/// not be destroyed from inside the frame tree's event dispatch, here the window must not be
	/// destroyed from inside the window's own close. Same rule, one layer down.
	///
	/// AND THE CONTENT IS RETIRED WITH IT AND FREED AFTER IT, because the window draws the content:
	/// releasing the content first would leave the window's frame tree pointing at freed frames for
	/// the length of one destructor. The watcher lists are cleaned at RETIREMENT rather than at
	/// release, so nothing repaints a retired panel in between.
	std::unique_ptr<visage::ApplicationWindow> retiredWindow_;
	std::unique_ptr<ExpandedView> retiredContent_;

	FrameWatchers* watchers_ = nullptr;
	ModState* mod_ = nullptr;
};

} // namespace feathers::ui
