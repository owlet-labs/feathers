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

#include "plugin/ui/escape_stack.h"
#include "plugin/ui/surface_host.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <visage/ui.h>

// THE NON-WINDOWING HOST: focused things run in the SAME window, replacing the editor.
//
// A non-windowing mode runs focused things in the same window.
// This is that mode, and it is the first `SurfaceHost` built because it is the
// CHEAPEST rather than the most compromised: a takeover repaints inside bounds the editor already
// has, so there is no `requestResize`, no host negotiation, and none of the private-event-loop cost
// that a real top-level window carries (`gui.cpp:1330` — a popout gets its own `X11Connection` and
// `processPluginFdEvents` drains only one of them).
//
// IT IS A STACK AND NOT A FLAG, because takeovers NEST BY PUSHING where overlays nest by layering:
// a guest rack opens a browser, a device is picked, its expanded view opens — two levels deep, and
// Esc must pop ONE. One stack serves both kinds when the other arrives.
//
// THE BACK AFFORDANCE IS DRAWN HERE AND NOT BY THE CONTENT. Property 1 of the design is that the
// content never closes itself and never knows what closed it — so it cannot know whether it owes the
// player a `<` or a `x`, because it does not know which kind it got. The overlay's `x` and this
// bar's `<` are the same event arriving at the same `onDismiss`.
//
// WHAT IT DELIBERATELY IS NOT: a scrim. `expanded_overlay` needs one because the editor stays
// visible and reachable beneath it; here the editor is GONE, so the fill is opaque and this frame
// takes every press that its content does not. That difference IS the `SurfaceNeed` distinction,
// made out of pixels.

namespace feathers::ui {

class TakeoverStack : public visage::Frame, public SurfaceHost {
public:
	TakeoverStack();
	~TakeoverStack() override;

	void dismiss(SurfaceId id) override;

	/// POP ONE LEVEL — what Back and Escape both do. A no-op when nothing is up.
	void back();

	size_t depth() const noexcept { return entries_.size(); }
	bool empty() const noexcept { return entries_.empty(); }

	/// ESCAPE WITH NOTHING UP is offered onward rather than swallowed, exactly as
	/// `ExpandedOverlay::onEscapeWhenClosed` is: a surface that eats Esc for the life of the editor
	/// takes the key away from "leave modulation assign mode", which is the other thing that wants it.
	std::function<bool()> onEscapeWhenEmpty;

	/// **THE EDITOR'S ESC STACK** (backlog NN), as `ExpandedOverlay::setEscapeStack`: each level is an entry
	/// until it is popped by any road, and Esc here closes the stack's top — which is this stack's top level
	/// only when nothing of another kind opened after it.
	void setEscapeStack(std::function<EscapeStack*()> resolve) { escapeStack_ = std::move(resolve); }

	/// THE STACK CHANGED DEPTH — the owner may need to lay out again, and a host that hides its
	/// editor's other frames while a takeover is up learns here that it may show them.
	std::function<void()> onDepthChanged;

	void resized() override;
	void draw(visage::Canvas& canvas) override;

	/// WHAT THE BAR IS SHOWING. Read off the entry rather than a copy kept beside it, so a case
	/// cannot pass against a record of what was meant to be presented.
	const std::string& titleForTest() const;
	visage::Frame* topForTest() const;

	/// HOW MANY SURFACES HAVE BEEN TAKEN DOWN — a count produced BY the dismissal road, so a `back()`
	/// that quietly stops calling `onDismiss` cannot look like one that had nothing to tell.
	size_t dismissalsForTest() const noexcept { return dismissals_; }

	/// PICK THE BACK CHEVRON BY POINT, the road a click takes.
	bool pressAt(float x, float y);

	/// TAKE THE NEXT ID WITHOUT PRESENTING ANYTHING.
	///
	/// FOR A HOST THAT OWNS MORE THAN ONE LAYER. `SurfaceLayers` puts an overlay beside this stack, and
	/// two independent counters would hand out the same number twice — after which `dismiss(id)` has to
	/// GUESS which layer the caller meant, which is the identity confusion `SurfaceId` exists to
	/// prevent. One counter, borrowed, is cheaper than a tag on every id.
	SurfaceId reserveId() noexcept { return next_++; }

	static constexpr float kBarHeight = 28.0f;
	static constexpr float kBackWidth = 64.0f;

protected:
	SurfaceKind kindAvailable(SurfaceNeed need) const override;
	SurfaceId presentImpl(std::unique_ptr<visage::Frame> content, SurfaceNeed need, std::string title,
	                      std::function<void()> onDismiss) override;

private:
	struct Entry {
		SurfaceId id = kNoSurface;
		std::unique_ptr<visage::Frame> content;
		std::string title;
		std::function<void()> onDismiss;
		EscapeTicket escape;
	};
	std::function<EscapeStack*()> escapeStack_;

	/// POP UNTIL `depth` LEVELS REMAIN, top first, telling each as it goes.
	void popTo(size_t depth);

	/// DESTROY WHAT DISMISSAL RETIRED, at a moment when nothing is running inside it.
	void reapRetired();
	void layoutTop();

	std::vector<Entry> entries_;

	/// CONTENT THAT HAS BEEN TAKEN DOWN AND NOT YET DESTROYED.
	///
	/// **A SURFACE MUST NOT BE DESTROYED WHILE ITS OWN CALLBACK IS RUNNING.** The rack's browser is the
	/// case that proved it: `BrowserView::onPick` is a `std::function` OWNED BY the browser, and the
	/// guest's handler for it does `dismiss(browserId_); addDeviceByTag(tag);`. Destroying the content
	/// inside `dismiss` frees the lambda's `this` AND the `std::function`'s own storage, and then the
	/// next statement executes from freed memory. It showed as a crashed audio engine — in Bitwig a
	/// plugin's UI lives in the audio engine process, so a use-after-free on the UI thread reads as an
	/// audio crash and sends you looking at the wrong thread entirely.
	///
	/// **`ExpandedOverlay::retired_` ALREADY RECORDED THIS AND I DID NOT REUSE IT**, which is the part
	/// worth remembering: *"A FRAME MUST NOT BE DESTROYED FROM INSIDE ITS OWN TREE'S EVENT DISPATCH...
	/// deleting a child and re-routing events underneath that walk is how a SIGSEGV in an unrelated
	/// frame's handler happens, which is what was hit. So a dismissal RETIRES the content and the next
	/// `show` or `draw` destroys it."* The same sentence, one layer up, from the same kind of crash.
	///
	/// SO: `removeChild` happens immediately — the frame stops drawing and stops taking presses the
	/// instant it is dismissed — and the OBJECT outlives the callback that dismissed it, until the next
	/// `present`, `draw`, or this stack's destruction.
	std::vector<Entry> retired_;
	/// MONOTONIC, NEVER REUSED — see `SurfaceId`. Starts at 1 so `kNoSurface` can mean nothing.
	SurfaceId next_ = 1;
	size_t dismissals_ = 0;
};

} // namespace feathers::ui
