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

#include "plugin/ui/windowed_expanded.h"

#include "plugin/ui/mod_state.h"
#include "plugin/ui/widget_registry.h"

#include "plugin/ui/theme.h"

#include <algorithm>
#include <visage_app/application_window.h>
#include <visage_graphics/canvas.h>

namespace feathers::ui {

void PopoutGround::draw(visage::Canvas& canvas) {
	// THE EDITOR'S OWN GROUND COLOUR, so a detached panel and an overlaid one stand on the same
	// thing. Opaque by construction — every token is 0xff-alpha — which is the whole point: a
	// translucent ground would let what it covers show through at a few per cent and the artefact
	// would come back quieter rather than not at all.
	canvas.setColor(kVoid);
	canvas.fill(0.0f, 0.0f, width(), height());
}

WindowedExpanded::WindowedExpanded() = default;

WindowedExpanded::~WindowedExpanded() {
	// THE WINDOW GOES BEFORE THE CONTENT IT DRAWS. Destroying the content first would leave the
	// window's frame tree pointing at freed frames until its own destructor ran — and the window is
	// the thing an OS callback can still reach.
	closeWindow();
	// AND THE RETIRED ONE IS FREED HERE, WHICH IS THE OTHER SAFE POINT. Nothing is in flight in a
	// destructor: the only road into it is the editor's own teardown.
	releaseRetired();
}

visage::Frame* WindowedExpanded::root() const {
	return window_.get();
}

void WindowedExpanded::show(std::unique_ptr<ExpandedView> content) {
	if (content == nullptr) {
		return;
	}
	// ONE AT A TIME, and the old one goes through the same road as any other dismissal so a caller
	// watching `onDismiss` is told rather than having its content vanish.
	if (content_ != nullptr) {
		dismiss();
	}
	// AND THE PREVIOUS OCCUPANT IS DESTROYED HERE, not where it was closed — no close callback is in
	// flight at this point. See `retiredWindow_`.
	releaseRetired();
	content_ = std::move(content);

	const visage::Point wanted = content_->minimumSize();
	window_ = std::make_unique<visage::ApplicationWindow>();
	shownTitle_ = content_->headerTitle();
	window_->setTitle(shownTitle_);
	// THE GROUND FIRST, so the panel draws over it. See `PopoutGround` for why a detached window
	// needs one at all and why full redraw does not substitute for it.
	window_->addChild(&ground_);
	window_->addChild(content_.get());
	// THE EDITOR'S SHORTCUTS REACH THIS WINDOW TOO (backlog JB): the root is the terminus for a key the panel declined.
	window_->setAcceptsKeystrokes(true);
	window_->onKeyPress() = [this](const visage::KeyEvent& e) { return onKey && onKey(e); };

	// THE CONTENT FILLS THE WINDOW, AND RE-FILLS IT ON EVERY RESIZE. A window is resizable where an
	// overlay was not — property 3 in the overlay's header, arriving as the one thing that actually
	// differs. `minimumSize` is the only size fact the content states, so it is a floor here rather
	// than a fixed size.
	const auto fill = [this] {
		if (content_ != nullptr && window_ != nullptr) {
			// THE GROUND IS RESIZED WITH THE PANEL, not once at open. A ground that kept its first
			// size would leave the strip a window grew into unpainted — the same defect it exists to
			// prevent, in the one shape the first version of it would still have had.
			ground_.setBounds(0, 0, window_->width(), window_->height());
			content_->setBounds(0, 0, window_->width(), window_->height());
		}
	};
	window_->onWindowContentsResized() += fill;

	// **AND THE OTHER HOST HONOURS THE SAME ANNOUNCEMENT** — `ExpandedView::onMinimumSizeChanged`. A
	// content whose minimum grows while it is open (SKLITTER's rails) would otherwise be laid out
	// inside a window sized at OPEN time and draw its rails over its own grid.
	//
	// **GROW ONLY, NEVER SHRINK.** A window is resizable where an overlay is not, so its current size
	// may be one a hand chose; taking width back when a rail closes would undo that. The overlay makes
	// the same call by a different road (`slot.sized` is a floor under `panelSizeOf`).
	//
	// **NOT VERIFIED IN A REAL WINDOW (2026-09-19).** The popout host needs a display and a window
	// manager, and this repo's venues photograph the OVERLAY — `widget-shot` turns the popout off for
	// exactly that reason. The overlay's half of this is covered by a case; this half is reasoned.
	content_->onMinimumSizeChanged([this] {
		if (content_ == nullptr || window_ == nullptr) {
			return;
		}
		const visage::Point want = content_->minimumSize();
		const float haveW = static_cast<float>(window_->width());
		const float haveH = static_cast<float>(window_->height());
		if (want.x > haveW || want.y > haveH) {
			window_->setWindowDimensions(visage::Dimension(std::max(want.x, haveW)),
			                             visage::Dimension(std::max(want.y, haveH)));
		}
		if (content_ != nullptr) {
			content_->setBounds(0, 0, window_->width(), window_->height());
		}
	});

	// THE TITLE BAR'S CLOSE IS A DISMISSAL LIKE ANY OTHER — property 4. The content never learns what
	// closed it, which is exactly what let the same content be dismissed by Esc, by the overlay's ✕,
	// and now by a window manager, without changing.
	window_->onCloseRequested() += [this] {
		dismiss();
		return true;
	};

	// ON THE STATE CHANNEL IN ITS OWN RIGHT — property 5, and here it stops being a nicety. A
	// detached window is NOT a descendant of the editor, so nothing carries the editor's repaint to
	// it: without this the panel would draw once and then never follow a parameter again.
	if (watchers_ != nullptr) {
		contentWatch_ = watchers_->watch(content_.get());
	}

	window_->show(visage::Dimension(wanted.x), visage::Dimension(wanted.y));

	// AND ASK IT TO DRAW, WHICH NOTHING ELSE WILL. MEASURED IN A HOST: a popout mapped, froze the
	// pixels behind it, and never drew its own contents.
	//
	// visage starts a window's draw timer in the EMBEDDED constructor only — the editor's, which a
	// plugin owns and no loop of visage's draws. A top-level window relies on `runEventLoop`, and
	// that loop never runs inside a plugin. So this window sat between the two cases with nothing
	// asking it to draw at all.
	//
	// THE SECOND HALF IS IN `Gui::processEvents`, and both are needed. The timer posts its draw
	// request onto the connection this window was created on — `X11Connection::globalInstance()` —
	// while the editor was built with its own private connection and pumps only that. A request
	// nobody drains is the same as no request.
	//
	// visage v1.0.0 (2026-11-05): BOTH constructors now start the pacing thread, so the timer half is
	// visage's own and the fork's `startPluginDrawTimer` is gone. The draining half in
	// `Gui::processEvents` is still ours and still needed.
	if (window_->window() != nullptr) {
		// **AND ABOVE THE HOST, WHICH ON macOS IT WAS NOT.** DD, the first Mac build (2026-09-07):
		// expanded windows popped under active windows and were nearly impossible to find. This is a
		// plain top-level window with no relation to the editor's, and `WindowMac::show` orders it
		// front with `makeKeyAndOrderFront:` — which orders it front WITHIN ITS PROCESS. Bitwig hosts
		// plugins in its own plugin-host process, which is not the active application, and AppKit
		// does not raise a background application's window above the active one's: the panel opened
		// beneath Bitwig. X11's window manager raises a new top-level regardless of process, which is
		// why Linux never showed it.
		//
		// visage's own cross-platform API: NSFloatingWindowLevel on macOS (above every normal window of
		// every application, so the panel is on top of the DAW the moment it opens), HWND_TOPMOST on
		// win32 (the same class of pop-under for out-of-process hosts), and NO implementation on X11 —
		// the base no-op — so Linux is unchanged by construction. The better-behaved road is a CHILD
		// window of the editor's host window, which would follow the host's minimise and stay with its
		// editor rather than above everything; that needs new fork code in the .mm and the parent
		// handle plumbed from PluginWindow to here, none of it compilable on the Linux host that wrote
		// this. A floating panel is what most DAW plugin windows are; the trade-off (it stays above
		// OTHER applications too while open) is to be judged on the run that verifies DE.
		window_->window()->setAlwaysOnTop(true);
	}
	fill();
}

bool WindowedExpanded::updateContent() {
	if (content_ == nullptr) {
		return false;
	}
	if (window_ != nullptr) {
		if (std::string header = content_->headerTitle(); header != shownTitle_) {
			window_->setTitle(header);
			shownTitle_ = std::move(header);
		}
	}
	return content_->update();
}

void WindowedExpanded::dismiss() {
	if (content_ == nullptr) {
		return;
	}
	closeWindow();
	if (onDismiss) {
		onDismiss();
	}
}

void WindowedExpanded::closeWindow() {
	// OWNERSHIP MOVES OUT FIRST, AND THAT IS WHAT BREAKS A RECURSION RATHER THAN A FLAG HIDING ONE.
	//
	// `window_->close()` fires `onCloseRequested`, which this class registers to call `dismiss()` —
	// because the title bar's ✕ has to reach the same road as every other dismissal (property 4).
	// So closing from `dismiss()` re-entered: dismiss -> closeWindow -> close() -> dismiss, until the
	// stack overflowed. It presented as a SIGSEGV *after* the panel had opened successfully, with
	// every trace through `show()` complete, which is why it read as a fault in opening.
	//
	// Taking both pointers out before either is touched makes the re-entrant call find `content_`
	// null and return at `dismiss()`'s first line. The cycle cannot form, rather than being detected.
	std::unique_ptr<ExpandedView> content = std::move(content_);
	std::unique_ptr<visage::ApplicationWindow> window = std::move(window_);

	if (content != nullptr) {
		// OFF EVERY REDRAW LIST BEFORE IT IS FREED. A watcher list outlives the panel, and a stale
		// frame in it is a repaint into freed memory — the same discipline the overlay's dismissal
		// follows, and all three lists are named for the same reason its comment gives.
		//
		// THE STATE LIST IS NOT THE ONLY ONE, WHICH IS WHAT THIS CLASS SHIPPED WRONG. `gui.cpp` calls
		// `ExpandedView::setModulation` on the content before handing it here, so every modulatable
		// knob under it joins `ModState`'s OWN `FrameWatchers` — a second list, joined by children,
		// and the first version of this function cleaned only the first. That is the defect `96d94be`
		// fixed for `FxLane`'s cards and `25282de` for `ExpandedOverlay`, arriving a third time.
		//
		// THE REGISTRY `nullptr` IS A DECLARATION RATHER THAN AN OMISSION, and it is the overlay's
		// argument unchanged because it is the same content: an `ExpandedView` builds its knobs
		// through the `device::Modulatable` constructor, which deliberately does not register, so
		// there is nothing of ours on the parameter list to forget.
		forgetFrameTree(content.get(), nullptr, watchers_,
		                mod_ != nullptr ? &mod_->watchers() : nullptr);
	}
	if (window != nullptr) {
		// THE CLOSE ROAD TO US IS CUT FIRST. A retired window may still be sent a close — a second click
		// of the ✕, or the WM_DELETE_WINDOW visage v1.0.0's own `close()` posts — and its handler is
		// `dismiss()` on THIS object, which by then may hold a different panel. Clearing the list makes
		// a late delivery reach nothing by construction (CallbackList copies on write, so clearing it
		// from inside its own invocation is safe — it is the road the window manager's ✕ takes here).
		window->onCloseRequested().clear();
		// THE GROUND COMES OFF BEFORE THE WINDOW IS RETIRED. It is a MEMBER of this object and outlives
		// every window it is lent to, so leaving it parented to a window that is closed but not yet
		// freed would give it two owners for the length of a retirement — and `show()` would then add
		// it to a second parent while the first still listed it. `~Frame` would sort it out either
		// way; this makes the invariant "the ground has a parent exactly while a window is open"
		// true, instead of nearly true.
		window->removeChild(&ground_);
		// HIDDEN, NOT `close()`d, AND THAT IS visage v1.0.0's SEMANTICS CHANGE (2026-11-05).
		//
		// v1.0.0's `ApplicationWindow::close()` is `window_->close()`, which on X11 only POSTS a
		// WM_DELETE_WINDOW to the window itself (on win32, a WM_CLOSE): a REQUEST, delivered by a later
		// pump. Calling it here retired the panel at once and left its window MAPPED — and the
		// delivery, arriving at a window whose handler had nothing left to dismiss, closed nothing,
		// because the plugin pump does not hide on a close request (only the standalone loop does).
		// MEASURED on venue-xvfb: the X server still reported the window IsViewable after `dismiss()`,
		// and after a close from outside had been pumped through. A popout dismissed by Esc would have
		// sat on screen, empty of anything we own, until the next `show()` freed it.
		//
		// `hide()` on the NATIVE window unmaps it synchronously and asks nobody: `ApplicationWindow::
		// hide()` would consult `onCloseRequested` first, which is the road we are already on. The
		// window is still freed only at a safe point (below).
		if (window->window() != nullptr) {
			window->window()->hide();
		}
	}
	// RETIRED, NOT DESTROYED, AND THAT IS A SECOND BUG RATHER THAN TIDINESS.
	//
	// A close from OUTSIDE — the window manager's ✕, or anyone's `ApplicationWindow::close()` once the
	// pump delivers it — arrives as a call INTO the window: `handleCloseRequested`, from the window's
	// own event dispatch, into our handler, which is `dismiss()`, which arrives here. Freeing the
	// window on this line would free the object whose method is on the stack. Before v1.0.0 the same
	// was true one level up (`ApplicationWindow::close()` touched itself after our handler returned);
	// MEASURED then: a deterministic SIGSEGV, not a leak. The same holds for Esc, which is a key
	// event dispatched by the window being dismissed.
	//
	// THE PREVIOUS FIX ADDRESSED THE OTHER HALF AND ONLY THE OTHER HALF. Moving the pointers into
	// locals stops the RECURSION — the re-entrant `dismiss()` finds nothing to do — and does nothing
	// about the CALLER still being on the stack. Both are the same shape as
	// `ExpandedOverlay::retired_`, one layer down: there a frame must not be destroyed from inside
	// its tree's event dispatch, here a window must not be destroyed from inside its own dispatch.
	//
	// THE LISTS ARE ALREADY CLEAN BY NOW, which is what makes deferring the free safe rather than
	// merely later: a retired panel is on no watcher list, so nothing repaints it while it waits, and
	// its window is unmapped and deaf to closes.
	retiredWindow_ = std::move(window);
	retiredContent_ = std::move(content);
}

void WindowedExpanded::releaseRetired() {
	// THE WINDOW GOES BEFORE THE CONTENT IT DRAWS. Explicit, and not left to member order: the
	// window's frame tree points at the content, so releasing the content first would leave it
	// pointing at freed frames for the length of one destructor.
	retiredWindow_.reset();
	retiredContent_.reset();
}

} // namespace feathers::ui
