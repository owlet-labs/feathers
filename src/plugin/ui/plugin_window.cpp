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

#include "plugin/ui/plugin_window.h"

#include "plugin/ui/keyboard_claim.h"

#ifdef FEATHERS_DIAGNOSTIC
#include "plugin/event_log.h"
#include "plugin/ui/frame_timing.h"
#include "plugin/ui/settings_screen.h"

#include <chrono>
#include <string>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <cstring>
#include <vector>

#if defined(FEATHERS_GUI_VISAGE)
#include <visage/app.h>
#if defined(__linux__)
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#endif
#endif

// A PLUGIN'S WINDOW — see plugin_window.h for why this exists once rather than twice.
//
// EVERYTHING BELOW WAS MOVED FROM gui.cpp VERBATIM. The comments are the artefact here, not the calls
// they sit on: each one is a deadlock, a re-entrancy or a reparenting rule that was paid for against a
// real host. A transcription that kept the code and dropped the reasoning would be the same mistake as
// writing it twice, one step later.

namespace feathers::ui {

#if defined(FEATHERS_GUI_VISAGE)

namespace {

// NOT ON macOS AT ALL, because its only caller is not. The scale road returns `1.0` early under
// `__APPLE__` — visage's dpiScale there is the window's backingScaleFactor, not a request to draw
// larger — so a definition left visible on that platform is an unused function, and macOS builds
// with -Werror. Found by `clang-preflight.sh`, which is the only instrument here that looks.
//
// THE GUARD MUST MATCH THE CALLER'S, not merely be narrower: `#if defined(__linux__)` around the
// definition would drop it on Windows too, where the `#else` branch below DOES call it and wants
// the 0.0.
#if !defined(__APPLE__)

double xServerDpi() {
#if defined(__linux__)
	Display* display = XOpenDisplay(nullptr);
	if (display == nullptr) {
		return 0.0;
	}
	const int screen = DefaultScreen(display);
	const int pixels = DisplayWidth(display, screen);
	const int millimetres = DisplayWidthMM(display, screen);
	XCloseDisplay(display);
	if (pixels <= 0 || millimetres <= 0) {
		return 0.0;
	}
	constexpr double kMillimetresPerInch = 25.4;
	return static_cast<double>(pixels) * kMillimetresPerInch / static_cast<double>(millimetres);
#else
	return 0.0;
#endif
}

#endif // !__APPLE__

/// The space a window manager actually gives windows: the screen minus panels and docks, from
/// _NET_WORKAREA. The whole display is the wrong number to size against — asking for 1913x1080 on a
/// 1920x1080 screen produced a window whose bottom 69 pixels, the readout and the base of the knob
/// tray, were simply not on screen.
///
/// Falls back to nine tenths of the display when the property is absent, which some window managers
/// never set. That is the same idea with a cruder number, and better than the whole screen.
/// `PluginWindow::setUsableScreenForTest`'s screen, when a case has set one.
uint32_t gScreenForTestWidth = 0;
uint32_t gScreenForTestHeight = 0;

bool usableScreenArea(uint32_t* width, uint32_t* height) {
	if (gScreenForTestWidth > 0 && gScreenForTestHeight > 0) {
		*width = gScreenForTestWidth;
		*height = gScreenForTestHeight;
		return true;
	}
#if defined(__linux__)
	Display* display = XOpenDisplay(nullptr);
	if (display == nullptr) {
		return false;
	}
	bool found = false;
	if (const Atom atom = XInternAtom(display, "_NET_WORKAREA", True); atom != None) {
		Atom type = 0;
		int format = 0;
		unsigned long items = 0;
		unsigned long remaining = 0;
		unsigned char* data = nullptr;
		if (XGetWindowProperty(display, DefaultRootWindow(display), atom, 0, 4, False, XA_CARDINAL,
		                       &type, &format, &items, &remaining, &data) == Success &&
		    data != nullptr) {
			// Four cardinals per desktop, x/y/width/height; the first desktop is the one we are on
			// often enough, and no window manager reports a different size per desktop in practice.
			if (items >= 4 && format == 32) {
				const auto* area = reinterpret_cast<const unsigned long*>(data);
				*width = static_cast<uint32_t>(area[2]);
				*height = static_cast<uint32_t>(area[3]);
				found = *width > 0 && *height > 0;
			}
			XFree(data);
		}
	}
	if (!found) {
		const int screen = DefaultScreen(display);
		*width = static_cast<uint32_t>(DisplayWidth(display, screen) * 9 / 10);
		*height = static_cast<uint32_t>(DisplayHeight(display, screen) * 9 / 10);
		found = *width > 0 && *height > 0;
	}
	XCloseDisplay(display);
	return found;
#else
	(void)width;
	(void)height;
	return false;
#endif
}

#if defined(__linux__)
/// HEAR WHAT THE SERVER SAYS BACK ON ONE PRIVATE CONNECTION (backlog GQ).
///
/// Xlib reports a failed request by calling ONE process-wide error handler, and only when the connection reads the
/// error. An asynchronous request has no reply to wait for, so `XFlush` + `XCloseDisplay` sends it and tears the
/// connection down before the answer is read: no handler runs, not ours and not Xlib's fatal default. MEASURED
/// (GP, 2026-09-20), the same reparent against the same dead window: flushed, 0 errors seen; synced, 2.
///
/// BOTH HALVES OR NEITHER. `sync` makes the errors arrive, and the handler is what keeps an arriving error from being
/// Xlib's default, which prints and EXITS: the host's process, for a window that failed to move. The handler records
/// errors for THIS connection only and passes every other connection's on to whoever was installed before, so
/// visage's own connections, read by its timer thread in the same window of time, are not swallowed. Restored on the
/// way out. Main thread only, like every caller.
class XErrorTrap {
public:
	explicit XErrorTrap(Display* display) : display_(display) {
		gPrevious = XSetErrorHandler(&XErrorTrap::handle);
		gActive.store(this);
	}
	~XErrorTrap() {
		gActive.store(nullptr);
		XSetErrorHandler(gPrevious);
	}
	XErrorTrap(const XErrorTrap&) = delete;
	XErrorTrap& operator=(const XErrorTrap&) = delete;

	/// WAIT FOR THE SERVER TO ANSWER EVERYTHING SENT SO FAR, and say what it answered. `self` is the editor's own
	/// window, which is what decides whether a BadWindow means "we are gone" or "where we were going is gone".
	PluginWindow::XReport sync(::Window self) {
		XSync(display_, False);
		report_.selfGone = report_.errors > 0 && report_.code == BadWindow && report_.resource == self;
		return report_;
	}

private:
	static int handle(Display* display, XErrorEvent* error) {
		XErrorTrap* trap = gActive.load();
		if (trap != nullptr && display == trap->display_) {
			if (trap->report_.errors++ == 0) {
				trap->report_.code = error->error_code;
				trap->report_.request = error->request_code;
				trap->report_.resource = error->resourceid;
			}
			return 0;
		}
		return gPrevious != nullptr ? gPrevious(display, error) : 0;
	}

	Display* display_;
	PluginWindow::XReport report_ {};
	static inline std::atomic<XErrorTrap*> gActive { nullptr };
	static inline XErrorHandler gPrevious = nullptr;
};
#endif

} // namespace

struct PluginWindow::Impl {
	explicit Impl(Client& c) : client(c) {}

	Client& client;
	std::unique_ptr<visage::ApplicationWindow> app;
	bool everShown = false;
	/// THE SERVER SAID OUR WINDOW IS GONE (backlog GP): a BadWindow naming it, on a detach or a reparent. `hasWindow`
	/// answers false from here until `setParent` rebuilds.
	bool windowGone = false;
	int rebuilds = 0;
	PluginWindow::XReport lastX {};
	double hostScale = 0.0;
	double visageScale = 0.0;
	uint32_t width = 0;
	uint32_t height = 0;
	/// WHETHER A HOST HAS SET A SIZE (`setSize`). Until one has, the size this window answers before it is embedded is
	/// the size it would like — see `PluginWindow::size`.
	bool sizedByHost = false;
	/// THE HEARTBEAT, AND IT IS THE SHELL'S BECAUSE A TIMER IS A WINDOW'S BUSINESS.
	///
	/// WHAT IT DOES IS NOT. A first draft of this extraction moved `startTicking` AND the editor's
	/// `driveReport`/`TickReport` in here together, because they arrived in the same method — the same
	/// "two facts that arrive at the same moment are not one fact" error the block context already
	/// caught once. The timer is generic; what happens on each beat is content, so it calls
	/// `Client::tick()` and learns nothing.
	struct Heartbeat : visage::EventTimer {
		explicit Heartbeat(Client& c) : client(c) {}
		void timerCallback() override {
#ifdef FEATHERS_DIAGNOSTIC
			const double start = nowMs();
			client.tick();
			frameTiming().ticked(nowMs() - start);
			logFrameTimingNow(false);
#else
			client.tick();
#endif
			// A click outside the editor ends a text entry's keyboard grab (keyboard_claim.h).
			pollKeyboardClaims();
		}
		Client& client;
	};
	Heartbeat heartbeat { client };

	/// IDEMPOTENT, and relied upon: `create` starts it on a reopen and `setParent` starts it again in
	/// a real host, which always follows create. Relying on that order would make this correct by
	/// CONVENTION rather than by construction.
	void startTicking() { heartbeat.startTimer(static_cast<int>(kTimerPeriodMs)); }
	void stopTicking() { heartbeat.stopTimer(); }

	static constexpr uint32_t kTimerPeriodMs = 16;

#ifdef FEATHERS_DIAGNOSTIC
	// THE FRAME TIMES (`frame_timing.h`), DIAGNOSTIC BUILDS ONLY: visage tells us every drawWindow's wait and draw, and
	// the heartbeat times the tick. A ship build hangs no callback and reads no clock.
	static double nowMs() {
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}
	static void instrument(visage::ApplicationWindow& window) {
		frameTiming().restartIntervals();
		window.setDrawTimedCallback([](long long waitUs, long long drawUs, bool presented) {
			const double end = nowMs();
			const double draw = static_cast<double>(drawUs) / 1000.0;
			frameTiming().drawn(end - draw, static_cast<double>(waitUs) / 1000.0, draw, presented);
		});
	}
	/// One event-log line (`events.log`) every `kLogPeriodMs` while an editor ticks, and one when it closes, so a
	/// sluggish session leaves its numbers behind.
	static constexpr double kLogPeriodMs = 30000.0;
	static void logFrameTimingNow(bool force) {
		static double last = nowMs();
		const double now = nowMs();
		if (!force && now - last < kLogPeriodMs) {
			return;
		}
		last = now;
		if (frameTiming().draw.stats().count == 0) {
			return;
		}
		eventlog::write("feathers-ui", "frames", std::string("gui-frames ") + frameTiming().summary() + "; " + SettingsScreen::sessionLine());
	}
#endif

	static double overrideScale() {
		const char* text = std::getenv("FEATHERS_UI_SCALE");
		if (text == nullptr) {
			return 0.0;
		}
		const double value = std::atof(text);
		return value > 0.0 ? value : 0.0;
	}

	double contentScale = 1.0;
	bool scaleApplied = false; ///< whether contentScale has ever been pushed to the window
	/// THE GLOBAL UI SCALE (backlog EJ): a multiplier on the display's scale, set by the editor from
	/// `EditorPrefs`. 1.0 is the design as drawn at the display's DPI.
	double uiScale = 1.0;

	/// THE PROMISE, in one expression: the largest scale at which the design canvas still fits inside
	/// a window this many device pixels across, and never larger than the display asked for. A logical
	/// canvas is native pixels divided by this, so the first two terms are exactly "the logical canvas
	/// is at least kLogicalWidth x kLogicalHeight" rearranged, and the third is what stops the content
	/// growing past the size the display wants once there is more room than the layout needs.
	///
	/// The lower clamp is the one thing here that could break the promise, at a window narrower than a
	/// quarter of the design canvas. Nothing may hand us one: minimumWidth/Height is the floor every
	/// entry point applies, and it is four times that.
	double contentScaleFor(int nativeWidth, int nativeHeight, double preferred) {
		// **THE WINDOW'S SIZE NO LONGER FEEDS THE SCALE** (backlog EJ: "window size ...
		// just enlarges the window but the widgets stay the same size"). This used to take the smallest of
		// fit-by-width, fit-by-height and the display's preference, so a window narrower than the design
		// shrank every widget to keep the whole canvas visible. Now the scale is the display's times the
		// user's, full stop: a bigger window is more room, a smaller one is the same widgets clipped at
		// the right and bottom — and `adjustSize` floors what a host may ask for at the design at this
		// scale, so a host that asks first cannot make one that small.
		(void)nativeWidth;
		(void)nativeHeight;
		return std::clamp(preferred, 0.25, 8.0);
	}

	/// THE SCALE THE CONTENT IS DRAWN AT: the display's scale times the global UI scale (backlog EJ).
	double preferredScale() const { return std::clamp(displayScale() * uiScale, 0.25, 8.0); }

	/// What the display asks for, before the user's own multiplier: the host's set_scale, then the
	/// FEATHERS_UI_SCALE override, then visage's derivation held down by the X server's DPI.
	double displayScale() const {
		if (hostScale > 0.0) {
			return hostScale;
		}
		if (const double override = overrideScale(); override > 0.0) {
			return override;
		}
		if (visageScale > 0.0) {
#if defined(__APPLE__)
			// visage's dpiScale on macOS is the window's backingScaleFactor — 2 on Retina — which
			// is how many device pixels a point occupies, not a request to draw the design canvas
			// twice as large. Using it as preferred made contentScaleFor raise the scale the
			// moment BOTH axes exceeded 1240x700, so the logical editor froze at the design size
			// while the window grew. A single-axis drag left the other term at 1 and looked fine;
			// dragging the other axis later, or both at once, did not. Host set_scale and
			// FEATHERS_UI_SCALE still win above. Standalone looked right because clap-wrapper
			// calls set_scale(1) and never hits this branch.
			return 1.0;
#else
			// The only source that gets second-guessed, and only ever downward. See clampDerivedScale.
			return clampDerivedScale(visageScale, xServerDpi());
#endif
		}
		return 1.0;
	}

	/// Remember what visage worked out for itself, once, before anything overwrites it.
	///
	/// FROM THE FRAME WHEN THERE IS NO WINDOW, because the derivation exists before the window does:
	/// visage stamps its own scale on the editor the moment it is constructed, and that is the number
	/// the layout is being run at until something overrules it. Reading only the window meant the
	/// editor could not even see the scale it was drawing at until it was embedded.
	void captureVisageScale() {
		if (visageScale <= 0.0 && app != nullptr) {
			visageScale = app->window() != nullptr ? app->window()->dpiScale() : app->dpiScale();
		}
	}

	/// The size the editor would like to open at: its logical size at the display's scale, held down
	/// to what the screen can actually show.
	void preferredNativeSize(uint32_t* width, uint32_t* height) const {
		// THE DESIGN AT THE SCALE IN FORCE — and no smaller a SCALE for a small screen (backlog EJ): the
		// widgets' size is the user's setting, so what the screen cannot fit is clipped rather than
		// shrunk. The REQUEST is still held to the screen, because a window larger than the display is a
		// window the host cannot place; the scale is not.
		const double scale = preferredScale();
		double wantW = client.logicalWidth() * scale;
		double wantH = client.logicalHeight() * scale;
		if (app != nullptr && app->window() != nullptr && gScreenForTestWidth == 0) {
			// The display's room is in native pixels; the request is in host units (points on macOS).
			const visage::IPoint room = app->window()->maxWindowDimensions();
			if (room.x > 0 && room.y > 0) {
				wantW = std::min(wantW, static_cast<double>(room.x) / backingScale());
				wantH = std::min(wantH, static_cast<double>(room.y) / backingScale());
			}
		}
		// And the space windows actually get, which maxWindowDimensions does not know about: it
		// reports the display, panels and all.
		uint32_t usableWidth = 0;
		uint32_t usableHeight = 0;
		if (usableScreenArea(&usableWidth, &usableHeight)) {
			wantW = std::min(wantW, static_cast<double>(usableWidth));
			wantH = std::min(wantH, static_cast<double>(usableHeight));
		}
		// ROUNDED UP, as `adjustSize`'s floor is: a size rounded to nearest could land one row under that floor, and the
		// host's first drag would then snap it a row taller (1009 against 1010 at 1.375 x 734).
		*width = static_cast<uint32_t>(std::ceil(wantW));
		*height = static_cast<uint32_t>(std::ceil(wantH));
	}

	/// Ask the host to open the editor at the size it would like. THE ONLY PLACE this editor asks —
	/// see the note where onWindowContentsResized is deliberately not hooked up.
	void requestPreferredSize() {
		if (app == nullptr || app->window() == nullptr) {
			return;
		}
		uint32_t wantWidth = 0;
		uint32_t wantHeight = 0;
		preferredNativeSize(&wantWidth, &wantHeight);
		if (static_cast<uint32_t>(app->nativeWidth()) == wantWidth &&
		    static_cast<uint32_t>(app->nativeHeight()) == wantHeight) {
			return;
		}
		client.requestResize(wantWidth, wantHeight);
	}

	/// Set the content scale from the size the editor ACTUALLY is: the largest scale at which the
	/// logical canvas still fits inside it, and never larger than the display asked for.
	///
	/// This is where "never clip a control" is actually enforced. Whatever size the window is given
	/// — by the host, by a window manager, by a user dragging a corner — the content scales so that
	/// at least kLogicalWidth x kLogicalHeight logical pixels fit inside it. Above the preferred
	/// scale it stops growing and the fluid layout spreads into the extra room instead, because past
	/// that point the display is not asking for bigger pixels, there is simply more space.
	///
	/// WITH OR WITHOUT A WINDOW, and that is the correction. This used to return at the door unless
	/// there was a window to push the scale onto, which quietly made the promise above a promise
	/// about EMBEDDED editors only — while the editor is laid out from the moment it is built, at
	/// whatever scale visage stamped on the frame for itself. On this display that is 1.75, so the
	/// floor window measured 708.6x400 logical instead of 1240x700 and the Engine rail's Glide and Fine
	/// knobs were laid out at y=360..518 in a canvas 400 tall: not clipped by a scissor, simply put
	/// where no window reaches. A host that asks an editor its size before embedding it — which is
	/// the usual order, and CLAP's — was being answered out of that layout.
	///
	/// So the scale is a property of the EDITOR, resolved from the size it has, and the window is
	/// only how the answer is delivered when there is one.
	/// NATIVE PIXELS PER HOST UNIT. CLAP's Cocoa API "uses logical size": a macOS host speaks POINTS in
	/// get_size, set_size and adjust_size, while visage's frame and window sizes are native PIXELS, points
	/// times the backing scale. Everywhere a size crosses between the host and the frame it goes through
	/// this. 1 on X11, whose host sizes are the window's pixels.
	///
	/// MEASURED BY ITS ABSENCE (reported 2026-09-17, after the content-scale fix): "the background window
	/// becomes larger than the plugin leaving gray canvas ... it always stays larger than the painted part".
	/// Pixels were answered as points and points applied as pixels. That had been hidden while this file
	/// overwrote the window's backing scale with the UI scale, which made points and pixels the same number
	/// and drew a Retina display at 1x.
	double backingScale() const {
#if defined(__APPLE__)
		if (app != nullptr && app->window() != nullptr && app->window()->dpiScale() > 0.0f) {
			return app->window()->dpiScale();
		}
#endif
		return 1.0;
	}

	/// The scale the editor's frame ends up drawn at for a given content scale: the same number, except
	/// on macOS inside a window, where it rides on top of the window's backing scale (see the branch at
	/// the end of `syncContentScale`).
	float drawScaleFor(double scale) const {
#if defined(__APPLE__)
		if (app != nullptr && app->window() != nullptr) {
			return app->window()->dpiScale() * static_cast<float>(scale);
		}
#endif
		return static_cast<float>(scale);
	}

	void syncContentScale() {
		if (app == nullptr) {
			return;
		}
		captureVisageScale();
		const double scale = contentScaleFor(app->nativeWidth(), app->nativeHeight(), preferredScale());
		// WHAT IS ACTUALLY IN FORCE, and not merely what we last decided. The frame arrives carrying
		// visage's own derivation, and gets it stamped on again every time it is attached to a window
		// — ApplicationEditor::addToWindow pushes the WINDOW's scale down the tree — which is exactly
		// the number we may be here to overrule. When our answer and our last answer agree — a
		// 1240x700 window wanting a scale of 1, which is the common case — skipping the push on the
		// grounds that nothing changed leaves visage's 1.75 in place and the editor drawn at almost
		// twice the size it asked for. So the guard reads the scale off the frame that is being laid
		// out, and a scale we decided but that something has since overwritten is not applied.
		const bool inForce = app->dpiScale() == drawScaleFor(contentScale);
		if (scaleApplied && scale == contentScale && inForce) {
			return;
		}
		scaleApplied = true;
		contentScale = scale;
		if (app->window() == nullptr) {
			// No window to route the change through, so it goes straight onto the frame: the scale
			// first, then the same native size again, which is what turns it into logical bounds and
			// runs the layout through onResize. Native and not logical, because the device size is the
			// thing that did not change here.
			const visage::IBounds native = { app->nativeX(), app->nativeY(), app->nativeWidth(),
				                             app->nativeHeight() };
			app->setDpiScale(static_cast<float>(contentScale));
			app->setNativeBounds(native);
			return;
		}
		// Setting the scale on the window changes nothing by itself: visage's top-level frame only
		// re-reads it while being laid out, and Frame::setBounds skips the layout when the bounds work
		// out unchanged — which is precisely this case, because the bounds only change once the new
		// scale has been read. Setting it and hoping was the first attempt and left the editor drawing
		// at 1.75 with a scale of 1 on the books.
		//
		// So the layout is driven through the same entry point the X11 backend uses on a
		// ConfigureNotify, at a size one pixel out and then at the real one. The first call is what
		// makes the bounds genuinely differ so the scale is picked up; the second puts the size back.
		// A host that honours our resize request would have supplied that change for free, but plenty
		// of hosts ignore request_resize, and an editor may not depend on being resized to be correct.
#if defined(__APPLE__)
		// ON macOS THE WINDOW'S DPI SCALE IS NOT OURS TO SET. It is the backing scale factor, the
		// platform code uses it for points-to-pixels geometry, and it resets it whenever the view
		// changes window or display — so the UI scale written into it here was overwritten and the
		// editor drew at the backing scale in a window sized for the UI scale (reported 2026-09-17: "on
		// osx, scale is scaling the window but not drawing larger"). The UI scale rides on top as the
		// window's CONTENT scale; the editor, its canvas and its mouse coordinates read the product.
		app->window()->setContentScale(static_cast<float>(contentScale));
#else
		app->window()->setDpiScale(static_cast<float>(contentScale));
#endif
		const int nativeW = app->nativeWidth();
		const int nativeH = app->nativeHeight();
		app->window()->handleResized(nativeW, nativeH + 1);
		app->window()->handleResized(nativeW, nativeH);
	}

	/// Move the editor into a host container it has not been in before.
	///
	/// Plain X, deliberately: XReparentWindow moves an existing window between parents without
	/// touching the renderer, the swapchain or any of visage's state, which is the entire point — the
	/// alternative is show(), and show() destroys the window and deadlocks on Mesa's WSI threads.
	bool reparentTo(void* parentHandle) {
#if defined(__linux__)
		const ::Window self = nativeWindow();
		const auto parent = reinterpret_cast<::Window>(parentHandle);
		if (self == 0 || parent == 0) {
			return false;
		}
		Display* display = XOpenDisplay(nullptr);
		if (display == nullptr) {
			return false;
		}
		// SYNCED, NOT FLUSHED (backlog GQ): see `XErrorTrap`. The answer is what `setParent` reports.
		{
			XErrorTrap trap(display);
			XReparentWindow(display, self, parent, 0, 0);
			XMapWindow(display, self);
			lastX = trap.sync(self);
		}
		XCloseDisplay(display);
		windowGone = windowGone || lastX.selfGone;
		return lastX.errors == 0;
#elif defined(__APPLE__)
		// THE COCOA TWIN, routed through visage rather than raw Cocoa — this file is C++, and the
		// NSView moves are the backend's to make. reparent() re-homes the MTKView into the host's
		// new container without touching the renderer or the swapchain, which is the entire point
		// of doing it this way rather than through show() — see the note in create(). Before this
		// branch existed the #else below answered false, so every open after the first failed
		// silently: the wrapper ignores the answer, the stale view stayed in the previous
		// container, and the reopened editor was a blank canvas.
		if (app == nullptr || app->window() == nullptr) {
			return false;
		}
		app->window()->reparent(parentHandle);
		return true;
#else
		(void)parentHandle;
		return false;
#endif
	}

	/// THE WINDOWS `rebuild` HAS ABANDONED, process-wide: a heap vector that is never freed, so not even process exit
	/// runs their teardown.
	static std::vector<std::unique_ptr<visage::ApplicationWindow>>* abandonedWindows() {
		static auto* windows = new std::vector<std::unique_ptr<visage::ApplicationWindow>>();
		return windows;
	}

	/// A NEW WINDOW, BECAUSE THE HOST DESTROYED OURS (backlog GP) — AND THE OLD ONE IS NEVER DESTROYED.
	///
	/// A host that destroys its container before guiDestroy takes our X window with it: X destroys a window's children
	/// with it, and `detachFromParent` comes too late. Re-mapping is not available, because there is nothing left to
	/// map. So the next open builds a fresh `ApplicationWindow` and the editor's frames move into it.
	///
	/// **THE DEAD ONE IS ABANDONED, NOT DESTROYED, and that is the whole safety argument.** Tearing down a Vulkan
	/// swapchain AFTER its window has gone is exactly the order that deadlocked the host's main thread on Mesa's WSI
	/// thread (visage fork 018cd86: "flush the frame buffer's destroy while its window still exists"). Here the window
	/// is gone before we know it, so that order is unavoidable for this canvas: `show()` on it would re-pair it,
	/// `pairToWindow` destroys the frame buffer, and so would its destructor. A fresh `ApplicationWindow` has its own
	/// canvas and nothing to tear down, which is why a second plugin instance's first `show()` has always been safe.
	/// The cost is one window's objects leaked per such close, for the life of the process, deliberately: a heap
	/// vector that is never freed, so not even process exit runs the teardown.
	///
	/// What the leaked one still does: its visage timer thread posts a draw message to the dead id every frame, which
	/// the server refuses on a connection nobody reads. It never draws, because no message reaches a window that does
	/// not exist. Stopping that thread needs the fork; it is not worth a fork change for a close order no host has been
	/// shown to take.
	void rebuild(void* parentHandle) {
		// **THE LEAK, ACCEPTED:** abandon the dead window, one per container-first close. The
		// reason is the WSI teardown order: a swapchain destroyed after its window is gone joins a Mesa WSI thread that
		// is waiting on that window, and never returns (visage fork 018cd86). Bounded to exactly one per event by
		// `test_rack_clap`'s "backlog GP: the abandoned windows are exactly one per container-first close …".
		auto* abandoned = abandonedWindows();
		auto fresh = std::make_unique<visage::ApplicationWindow>();
		// The size the dead one had, which is the size the host was last told.
		fresh->setWindowDimensions(visage::Dimension::nativePixels(static_cast<float>(app->nativeWidth())),
		                           visage::Dimension::nativePixels(static_cast<float>(app->nativeHeight())));
		// EVERY CHILD, IN ORDER (the order is the paint order), with its own visibility: `addChild`'s `make_visible`
		// only ever HIDES, so `true` leaves a hidden panel hidden.
		const std::vector<visage::Frame*> children = app->children();
		for (visage::Frame* child : children) {
			app->removeChild(child);
			fresh->addChild(child, true);
		}
		abandoned->push_back(std::move(app));
		app = std::move(fresh);
#ifdef FEATHERS_DIAGNOSTIC
		instrument(*app);
#endif
		client.windowRebuilt(*app);
		// The new frame arrives at visage's own scale, as the first one did.
		scaleApplied = false;
		app->show(parentHandle);
		windowGone = false;
		++rebuilds;
	}

	/// Leave the host's container before it is destroyed under us, and stop being visible.
	///
	/// Reparented to the ROOT rather than merely unmapped: an unmapped child is still a child, and
	/// the host destroying its container would take the editor's window with it — which is precisely
	/// the destruction that deadlocks. Unmapped at the root, it is out of everyone's way and still
	/// ours.
	void detachFromParent() {
#if defined(__linux__)
		const ::Window self = nativeWindow();
		if (self == 0) {
			return;
		}
		Display* display = XOpenDisplay(nullptr);
		if (display == nullptr) {
			return;
		}
		// SYNCED, as `reparentTo` is. A detach that fails because our window is already gone is how a host that
		// destroyed its container BEFORE guiDestroy is first noticed (backlog GP), and it is the only notice there is.
		{
			XErrorTrap trap(display);
			XUnmapWindow(display, self);
			XReparentWindow(display, self, DefaultRootWindow(display), 0, 0);
			lastX = trap.sync(self);
		}
		XCloseDisplay(display);
		windowGone = windowGone || lastX.selfGone;
#elif defined(__APPLE__)
		// THE COCOA TWIN of the same rule: our view leaves the host's container so the container
		// can die without taking anything of ours with it. Before this branch existed the X11 body
		// had no macOS counterpart at all, so the MTKView stayed a subview of a container the host
		// was free to destroy — exactly the situation the comment above says must not happen.
		// The view itself stays built (the built-once rule, see create()) and is ours, ready for
		// the next open to re-home it.
		if (app == nullptr || app->window() == nullptr) {
			return;
		}
		app->window()->detachFromParent();
#endif
	}


#if defined(__linux__)
	// X11 only: the callers above sit inside the same guard, and ::Window is Xlib's type — the
	// second macOS CI run (34095702319) stopped here when this stood outside it.
	::Window nativeWindow() const {
		if (app == nullptr || app->window() == nullptr) {
			return 0;
		}
		return reinterpret_cast<::Window>(app->window()->nativeHandle());
	}
#endif
};

PluginWindow::PluginWindow(Client& client) : impl_(std::make_unique<Impl>(client)) {
	impl_->width = client.logicalWidth();
	impl_->height = client.logicalHeight();
}

PluginWindow::~PluginWindow() = default;

visage::ApplicationWindow* PluginWindow::app() noexcept { return impl_->app.get(); }
const visage::ApplicationWindow* PluginWindow::app() const noexcept { return impl_->app.get(); }

void PluginWindow::size(uint32_t* width, uint32_t* height) const noexcept {
	if (width == nullptr || height == nullptr) {
		return;
	}
	// Only once there is a window to measure. An ApplicationWindow that has not been shown yet answers
	// out of the display rather than out of itself — it reported the whole screen here — and a host
	// calling get_size before set_parent, which is the usual order, would size its container to that.
	if (impl_->app != nullptr && impl_->app->window() != nullptr) {
		// What the window actually is, which is not always what was last asked for: the host may have
		// resized it, and CLAP expects the answer to track that.
		// In HOST units — points on macOS — see `backingScale`.
		const double backing = impl_->backingScale();
		*width = static_cast<uint32_t>(std::lround(impl_->app->nativeWidth() / backing));
		*height = static_cast<uint32_t>(std::lround(impl_->app->nativeHeight() / backing));
		return;
	}
	// **BEFORE THE EMBED, THE SIZE IT WOULD LIKE — SCALED, AND HELD TO THE SCREEN** (backlog LX). Bitwig
	// asks this before `set_parent`, makes its container that size, and ignores the request `setParent` makes. This
	// answered the LOGICAL design unscaled, so at a UI scale of 1.75 the window opened at 1240x700 while `adjustSize`
	// floors at the design times the scale, and the first drag snapped it to 2170x1285 — past a 1920x1080 screen.
	// Answering the preferred size makes the open size, the floor and the request one number. A host that has set a
	// size is answered that size.
	if (!impl_->sizedByHost) {
		impl_->preferredNativeSize(width, height);
		return;
	}
	*width = impl_->width;
	*height = impl_->height;
}
void PluginWindow::syncScaleAndLayout() {
	impl_->syncContentScale();
	impl_->client.layout();
}

void PluginWindow::preferredSizeChanged() {
	impl_->syncContentScale();
	impl_->client.layout();
	// GROW ONLY, AND ONLY WHAT IS SHORT. Both sizes in host units, like everything the host sees.
	if (impl_->app == nullptr || impl_->app->window() == nullptr) {
		return;
	}
	uint32_t wantWidth = 0;
	uint32_t wantHeight = 0;
	impl_->preferredNativeSize(&wantWidth, &wantHeight);
	uint32_t haveWidth = 0;
	uint32_t haveHeight = 0;
	size(&haveWidth, &haveHeight);
	if (haveWidth >= wantWidth && haveHeight >= wantHeight) {
		return;
	}
	impl_->client.requestResize(std::max(haveWidth, wantWidth), std::max(haveHeight, wantHeight));
}

void PluginWindow::widthChangedBy(double logicalDelta) {
	impl_->syncContentScale();
	impl_->client.layout();
	if (impl_->app == nullptr || impl_->app->window() == nullptr) {
		return;
	}
	uint32_t haveWidth = 0;
	uint32_t haveHeight = 0;
	size(&haveWidth, &haveHeight);
	// In host units, like `haveWidth` — see `contentScale`.
	const double wanted = static_cast<double>(haveWidth) + logicalDelta * impl_->contentScale;
	uint32_t wantWidth = static_cast<uint32_t>(std::max(0L, std::lround(wanted)));
	uint32_t wantHeight = haveHeight;
	(void)adjustSize(&wantWidth, &wantHeight);
	if (wantWidth != haveWidth) {
		impl_->client.requestResize(wantWidth, haveHeight);
	}
}

double PluginWindow::logicalRoomHeight() const noexcept {
	if (impl_->app == nullptr || impl_->app->window() == nullptr) {
		return 0.0;
	}
	const double scale = impl_->preferredScale();
	return scale > 0.0 ? static_cast<double>(nativeHeight()) / scale : 0.0;
}

double PluginWindow::preferredScale() const noexcept {
	return impl_->app != nullptr ? impl_->preferredScale() : 0.0;
}

int PluginWindow::nativeHeight() const noexcept {
	return impl_->app != nullptr
	           ? static_cast<int>(std::lround(impl_->app->nativeHeight() / impl_->backingScale()))
	           : 0;
}

int PluginWindow::nativeWidth() const noexcept {
	return impl_->app != nullptr
	           ? static_cast<int>(std::lround(impl_->app->nativeWidth() / impl_->backingScale()))
	           : 0;
}

double PluginWindow::contentScale() const noexcept {
	return impl_->contentScale;
}

int PluginWindow::posixFd() const noexcept {
	if (impl_->app == nullptr || impl_->app->window() == nullptr) {
		return -1;
	}
#if defined(__linux__)
	return impl_->app->window()->posixFd();
#else
	return -1;
#endif
}

void PluginWindow::processEvents() noexcept {
	if (impl_->app == nullptr || impl_->app->window() == nullptr) {
		return;
	}
	// BEFORE THE PUMP, NOT INSIDE A DRAW CALLBACK — visage only invokes a frame's draw for frames it
	// has marked stale, so anything hung off the top-level frame's onDraw runs once and then never
	// again.
#ifdef FEATHERS_DIAGNOSTIC
	const double start = Impl::nowMs();
	impl_->client.tick();
	frameTiming().ticked(Impl::nowMs() - start);
	Impl::logFrameTimingNow(false);
#else
	impl_->client.tick();
#endif
	pollKeyboardClaims();
	impl_->app->window()->processPluginFdEvents();
}

void PluginWindow::startTicking() { impl_->startTicking(); }
void PluginWindow::stopTicking() { impl_->stopTicking(); }


	/// Start and stop the heartbeat with the window, because a detached editor has nothing to paint
	/// and EventManager is a process-wide singleton — a second instance's frame loop would otherwise
	/// keep ticking an editor that is not on screen.

bool PluginWindow::isAvailable() noexcept {
#if defined(__linux__) || defined(__APPLE__)
	return true;
#else
	// WINDOWS HAS NO EMBED YET. visage is built there because the widget library compiles against it, but this file
	// speaks X11 and Cocoa only: no CLAP_WINDOW_API_WIN32 attach, reparent or detach. So the gui extension is absent
	// (the promise feathers_clap.cpp makes for a platform with no backend) rather than offered and broken.
	return false;
#endif
}

bool PluginWindow::isApiSupported(const char* api, bool isFloating) noexcept {
	// Embedded X11 on Linux, embedded Cocoa on macOS — and nothing else on either, which is what
	// the comment below has always claimed and the code now matches. Accepting X11 on macOS let a
	// test drive create() through the (since-removed, Linux-only) extension gate into an editor
	// claiming an embedding no macOS host can provide. Floating is a window the plugin owns and
	// places, which the plan defers along with the Windows backend.
	return !isFloating && api != nullptr
#if defined(__APPLE__)
	       && std::strcmp(api, CLAP_WINDOW_API_COCOA) == 0;
#elif defined(__linux__)
	       && std::strcmp(api, CLAP_WINDOW_API_X11) == 0;
#else
	       && false; // no Windows embed yet: see isAvailable()
#endif
}

void PluginWindow::destroy() noexcept {
	if (impl_ == nullptr) {
		return;
	}
	// THE CONTENT'S OWN TEARDOWN — a latched preview, say — happens in the caller's destroy(), before
	// this is reached. It is deliberately NOT here: what must be cleaned up when an editor closes is a
	// fact about that editor.
	if (impl_->app == nullptr) {
		return;
	}
#ifdef FEATHERS_DIAGNOSTIC
	Impl::logFrameTimingNow(true); // the session's last window of frame times, as the editor closes
#endif
	// NOTHING IS TORN DOWN HERE, and that is the fix rather than an oversight. Destroying the window
	// deadlocks the calling thread on a Mesa WSI thread that is waiting on the window being destroyed
	// — see the note in create(). So the editor is detached from the host's container and hidden, and
	// it stays built, ready for the next open.
	//
	// The one thing that MUST happen is leaving the host's container: the host is free to destroy it
	// the moment this returns, and a child window left inside would be destroyed with it — which is
	// exactly the situation the deadlock is made of.
	// The heartbeat stops with the window. EventManager is process-wide, so a second instance's frame
	// loop would go on calling this one's timer after it left the screen.
	impl_->stopTicking();
	// **AND THE HAND LETS GO OF EVERYTHING IT HELD** (backlog KS). The window is kept, so visage's own
	// input state in it is kept too: a press whose release never came (the host closed the editor
	// mid-drag) is still the frame every motion goes to and still holds a host gesture open; a field being
	// typed into is still editing and still holds the X keyboard (`KeyboardClaim`); a popup menu is still
	// up. Leaving the screen IS losing the focus, so the window is told so — visage's one road that ends
	// a press with a release, tells the focused frame it lost focus (a field cancels and gives the
	// keyboard back; a popup menu closes, read from `PopupMenuFrame::focusChanged` and not cased) and ends
	// the hover — rather than each of those learning about a close.
	if (impl_->app->window() != nullptr) {
		impl_->app->window()->handleFocusLost();
	}
	impl_->detachFromParent();
}

bool PluginWindow::setParent(const clap_window* window) noexcept {
	if (window == nullptr || !isApiSupported(window->api, false) || impl_->app == nullptr) {
		return false;
	}
	// CONSTRAINT, observed rather than documented: this call is RE-ENTRANT. Bitwig calls get_size,
	// adjust_size and set_size on the plugin from inside the show() below, before this function has
	// returned — it reacts to the reparent while we are still mid-embed. Nothing here may assume it
	// holds the editor to itself, and anything added to this function has to survive being asked its
	// size, and being resized, halfway through.
	//
	// `ptr` and `x11` are the same union member read two ways; visage's X11 backend casts the pointer
	// back to the window id. This is the spelling its own CLAP example uses.
	//
	// ONLY THE FIRST TIME. show() destroys and rebuilds the window, which deadlocks the caller on a
	// Mesa WSI thread — see the note in create(). Afterwards the window we already have is moved into
	// the host's new container with plain X, which touches neither the renderer nor the swapchain.
	if (!impl_->everShown) {
		// THE WINDOW IS MADE THE SIZE `size` TOLD THE HOST, which is the container the host made for it — unless the host
		// has set one, which `setSize` already applied.
		if (!impl_->sizedByHost) {
			uint32_t w = 0;
			uint32_t h = 0;
			impl_->preferredNativeSize(&w, &h);
			impl_->app->setWindowDimensions(visage::Dimension::nativePixels(static_cast<float>(w)),
			                                visage::Dimension::nativePixels(static_cast<float>(h)));
		}
		impl_->app->show(window->ptr);
		impl_->everShown = true;
	}
	else {
		// A MOVE, unless the server has told us our window is gone, at a detach or by refusing this reparent. Then it
		// is a rebuild (backlog GP). A reparent that fails for any OTHER reason (a parent that does not exist) is a
		// failed embed, and says so (backlog GQ).
		if (!impl_->windowGone && !impl_->reparentTo(window->ptr) && !impl_->windowGone) {
			return false;
		}
		if (impl_->windowGone) {
			impl_->rebuild(window->ptr);
		}
	}

	// Only now is there a window to ask about the display, so this is where the scale is settled for
	// the first time. If the editor wants a different size than the host guessed, this is also the
	// one moment it is entitled to say so.
	impl_->syncContentScale();
	impl_->requestPreferredSize();

	impl_->client.layout();

	// EVERYTHING REPAINTS, not just what has changed. The chrome — the header, the Engine rail rail —
	// redraws only when one of its parameters moves, which for a static patch is never; so anything
	// that ever lands on those pixels stays there for the life of the editor. A screenshot showed
	// exactly that: a dashed diagonal from the editor's origin across the Engine rail, over opaque panel
	// chrome, sitting in a region nothing was going to repaint.
	//
	// I could not reproduce it and I cannot attribute it — the geometry matches no draw call we make
	// (see the plan) — but the PROPERTY that lets any such artefact persist is ours, and it costs one
	// full repaint per open to remove. After moving a window between containers we cannot know what
	// was on those pixels or whether the compositor kept them, so we repaint all of it.
	impl_->app->redrawAll();

	// AND THE HEARTBEAT STARTS, now that there is a window whose frame loop can carry it. See
	// TickTimer: this is what makes the engine->editor road independent of the host offering a timer
	// or an fd, which two shipping paths do not.
	impl_->startTicking();

	// What we can answer: visage gave us a native window. What we must NOT answer with is visage's
	// isShowing(), which asks X for the window's map_state and demands IsViewable — true only once
	// the window AND EVERY ANCESTOR is mapped. The ancestors belong to the host, which is free to map
	// its container after this call returns, and Bitwig does exactly that. So isShowing() was false
	// on every open, this returned false, and the host abandoned the open and destroyed the editor
	// without a word: we were reporting the host's "not yet" as our own "failed".
	return hasWindow();
}

bool PluginWindow::hasWindow() const noexcept {
	return impl_->app != nullptr && impl_->app->window() != nullptr && !impl_->windowGone;
}

const PluginWindow::XReport& PluginWindow::lastXReport() const noexcept {
	return impl_->lastX;
}

int PluginWindow::rebuilds() const noexcept {
	return impl_->rebuilds;
}

size_t PluginWindow::abandonedWindowsForTest() noexcept {
	return Impl::abandonedWindows()->size();
}

void PluginWindow::releaseAbandonedForTest() noexcept {
	Impl::abandonedWindows()->clear();
}

void PluginWindow::setUiScale(double scale) noexcept {
	// Held to the ladder's ends: below 1 the design's smallest type is under the legible floor, and above
	// 2 no display this editor targets has the room. A non-finite value is ignored, not applied.
	if (!std::isfinite(scale)) {
		return;
	}
	const double held = std::clamp(scale, 1.0, 2.0);
	if (held == impl_->uiScale) {
		return;
	}
	impl_->uiScale = held;
	// The same two steps a host's set_scale takes: re-derive the content scale and push it through the
	// layout, then ask the host for the size the design now needs.
	impl_->syncContentScale();
	impl_->requestPreferredSize();
}

double PluginWindow::uiScale() const noexcept {
	return impl_->uiScale;
}

double PluginWindow::stepUiScale(double current, int direction) noexcept {
	constexpr const auto& kLadder = kUiScaleLadder;
	constexpr int kRungs = static_cast<int>(kUiScaleLadder.size());
	if (direction == 0) {
		return kLadder[0];
	}
	const double now = std::isfinite(current) ? current : 1.0;
	if (direction > 0) {
		for (int i = 0; i < kRungs; ++i) {
			if (kLadder[i] > now + 1e-9) {
				return kLadder[i];
			}
		}
		return kLadder[kRungs - 1];
	}
	for (int i = kRungs - 1; i >= 0; --i) {
		if (kLadder[i] < now - 1e-9) {
			return kLadder[i];
		}
	}
	return kLadder[0];
}

double PluginWindow::contentScaleForTest() const noexcept {
	return impl_->contentScale;
}

void PluginWindow::setUsableScreenForTest(uint32_t width, uint32_t height) noexcept {
	gScreenForTestWidth = width;
	gScreenForTestHeight = height;
}

int PluginWindow::uiScaleStepFor(int keyCode) noexcept {
	switch (static_cast<visage::KeyCode>(keyCode)) {
	case visage::KeyCode::Equals:
	case visage::KeyCode::KPPlus:
		return 1;
	case visage::KeyCode::Minus:
	case visage::KeyCode::KPMinus:
		return -1;
	case visage::KeyCode::Number0:
	case visage::KeyCode::KP0:
		return 0;
	default:
		return 2;
	}
}

bool PluginWindow::setScale(double scale) noexcept {
	if (!(scale > 0.0)) {
		return false;
	}
	// The most trusted source, so it is simply taken — see the note on scale in Impl. Acting on it
	// can change the size the editor needs, and that is one of the two moments this editor is
	// entitled to ask the host for a resize.
	impl_->hostScale = scale;
	impl_->syncContentScale();
	impl_->requestPreferredSize();
	return true;
}

bool PluginWindow::adjustSize(uint32_t* width, uint32_t* height) const noexcept {
	if (width == nullptr || height == nullptr) {
		return false;
	}
	// Raised to the minimum and nothing else. Idempotent because a maximum against a fixed floor is
	// idempotent: adjusting an already-adjusted size returns it unchanged, which is the property a
	// host relies on when it asks, applies, and asks again. What this must NEVER do is what the
	// clap-helpers default did on our behalf — answer with the size we already are, whatever was
	// asked. That told every host that dragged an edge to put the window back where it started.
	// THE FLOOR IS THE DESIGN AT THE SCALE IN FORCE, in the host's pixels (backlog EJ). The minimum the
	// client states is LOGICAL; a host speaks native, and since the scale no longer shrinks to fit, a
	// window below design x scale would clip. Rounded up, so the floor never lands a pixel short.
	const double scale = impl_->app != nullptr ? impl_->preferredScale() : 1.0;
	const auto floorOf = [scale](uint32_t logical) {
		return static_cast<uint32_t>(std::ceil(static_cast<double>(logical) * scale));
	};
	// AND THE FLOOR HELD TO THE USABLE SCREEN (backlog LX), as the open size and the request already
	// are: a design larger than the screen at this scale clips rather than putting the window past the display. A drag
	// LARGER than the screen is the host's to allow and is not refused here.
	uint32_t floorW = floorOf(impl_->client.minimumWidth());
	uint32_t floorH = floorOf(impl_->client.minimumHeight());
	uint32_t screenW = 0;
	uint32_t screenH = 0;
	if (usableScreenArea(&screenW, &screenH)) {
		floorW = std::min(floorW, screenW);
		floorH = std::min(floorH, screenH);
	}
	*width = std::max(*width, floorW);
	*height = std::max(*height, floorH);
	return true;
}

bool PluginWindow::setSize(uint32_t width, uint32_t height) noexcept {
	if (width == 0 || height == 0) {
		return false;
	}
	// Through the same floor adjustSize applies, so the three answers cannot contradict each other.
	// A host that follows the contract has already adjusted and nothing happens here; a host that
	// has not still gets an editor whose reported size is one it can actually draw at.
	adjustSize(&width, &height);
	impl_->width = width;
	impl_->height = height;
	impl_->sizedByHost = true;
	if (impl_->app != nullptr) {
		// Host units in, native pixels to the frame — see `backingScale`.
		const double backing = impl_->backingScale();
		impl_->app->setNativeWindowDimensions(static_cast<int>(std::lround(width * backing)),
		                                      static_cast<int>(std::lround(height * backing)));
		// The window changed size, so the content scale may need to change with it — that is what
		// keeps the logical canvas at or above the size the layout needs. No resize is requested
		// here: the host set this size, and answering it with a request of our own is the loop.
		impl_->syncContentScale();
		impl_->client.layout();
	}
	return true;
}

bool PluginWindow::create(const char* api, bool isFloating, bool* builtThisCall) noexcept {
	*builtThisCall = false;
	if (!isApiSupported(api, isFloating)) {
		return false;
	}
	// THE EDITOR IS BUILT ONCE AND KEPT FOR THE LIFE OF THE PROCESS, and every reopen reuses it. This
	// is not a cache: it is the only way to survive a second open at all.
	//
	// visage's ApplicationWindow::show() begins with removeFromWindow() and then builds a NEW window,
	// so a second open destroys the first window and its Vulkan swapchain. Tearing that swapchain down
	// joins Mesa's WSI threads, and one of them is parked in an untimed `poll(fd, POLLIN, -1)` waiting
	// on the window that has just been destroyed. The join never returns, so the HOST'S MAIN THREAD
	// wedges: the editor never appears, saving the project hangs, and Bitwig eventually reports that
	// the plugin host is not responding. Traced to those exact syscalls; see the plan.
	//
	// visage offers no way out of it — Renderer::initialize latches `initialized_`, nothing resets it,
	// and bgfx::shutdown is called nowhere — so the rule here is simply never to destroy a window
	// whose swapchain threads are alive. One window, reparented between hosts' containers as they come
	// and go, and torn down only at process exit where the accepted residual crash already lives.
	if (impl_->app != nullptr) {
		// A REOPEN, and the heartbeat has to come back with it. destroy() stops the timer, and this
		// early return means the one at the end of this function never runs a second time — so
		// without this line the editor's own tick would be alive on the first open and dead on every
		// one after, which is the same class of bug as the surface left off the setModulation list.
		//
		// setParent starts it too, and in a real host that always follows create — but relying on
		// that makes this correct by CONVENTION rather than by construction, and startTimer is
		// idempotent, so there is nothing to buy by leaving it out.
		impl_->startTicking();
#ifdef FEATHERS_DIAGNOSTIC
		++frameTiming().editorOpens;
		Impl::instrument(*impl_->app);
#endif
		return true; // built nothing: *builtThisCall stays false
	}

#ifdef FEATHERS_DIAGNOSTIC
	++frameTiming().editorOpens;
#endif
	auto app = std::make_unique<visage::ApplicationWindow>();
	// Native, not logical, pixels — CLAP's window sizes are physical on X11 and Win32, and this is the
	// same number that getSize() reports and setSize() receives. Asking in logical pixels instead
	// multiplies by the display's scale on the way in and not on the way out: on a 1.75x desktop the
	// editor opened 2170x1225, overflowing a 1920x1080 screen, while still reporting 1240x700.
	//
	// visage still scales the CONTENTS by the display scale, so the layout inside is in logical units
	// and text is rasterised at full resolution. What is deliberately not handled yet is a sensible
	// DEFAULT on a high-DPI display, where 1240x700 physical is a small window: that wants the host's
	// own scale via clap_plugin_gui.set_scale, and belongs with the design work rather than here.
	app->setWindowDimensions(visage::Dimension::nativePixels(static_cast<float>(impl_->width)),
	                         visage::Dimension::nativePixels(static_cast<float>(impl_->height)));

	// AND THE WINDOW BECOMES THE MEMBER, which is what makes `app()` answer.
	//
	// IN gui.cpp THIS ASSIGNMENT WAS THE LAST LINE OF create(), AFTER the contents were built — so the
	// slice that moved here ended before it and the shell built a window it then dropped. `app()`
	// answered null, `Gui::create` refused, and every windowed case failed at
	// `REQUIRE(gui.create(...))`. Caught by the gui-widgets venue on the first run after the swap,
	// which is exactly the thing that venue is for: a shell that compiles is not a shell that opens.
	impl_->app = std::move(app);
#ifdef FEATHERS_DIAGNOSTIC
	Impl::instrument(*impl_->app);
#endif

	impl_->startTicking();
	*builtThisCall = true;
	return true;
}

#else

struct PluginWindow::Impl {
	explicit Impl(Client& c) : client(c) {}
	Client& client;
};

PluginWindow::PluginWindow(Client& client) : impl_(std::make_unique<Impl>(client)) {}
PluginWindow::~PluginWindow() = default;
bool PluginWindow::isAvailable() noexcept { return false; }
bool PluginWindow::isApiSupported(const char*, bool) noexcept { return false; }
bool PluginWindow::create(const char*, bool, bool*) noexcept { return false; }
void PluginWindow::destroy() noexcept {}
bool PluginWindow::setParent(const clap_window*) noexcept { return false; }
bool PluginWindow::hasWindow() const noexcept { return false; }
const PluginWindow::XReport& PluginWindow::lastXReport() const noexcept {
	static const XReport none {};
	return none;
}
int PluginWindow::rebuilds() const noexcept { return 0; }
size_t PluginWindow::abandonedWindowsForTest() noexcept { return 0; }
void PluginWindow::releaseAbandonedForTest() noexcept {}
bool PluginWindow::setScale(double) noexcept { return false; }
bool PluginWindow::adjustSize(uint32_t*, uint32_t*) const noexcept { return false; }
void PluginWindow::size(uint32_t*, uint32_t*) const noexcept {}
bool PluginWindow::setSize(uint32_t, uint32_t) noexcept { return false; }
visage::ApplicationWindow* PluginWindow::app() noexcept { return nullptr; }
const visage::ApplicationWindow* PluginWindow::app() const noexcept { return nullptr; }
void PluginWindow::syncScaleAndLayout() {}
void PluginWindow::preferredSizeChanged() {}
void PluginWindow::widthChangedBy(double) {}
double PluginWindow::logicalRoomHeight() const noexcept { return 0.0; }
double PluginWindow::preferredScale() const noexcept { return 0.0; }
int PluginWindow::nativeHeight() const noexcept { return 0; }
int PluginWindow::nativeWidth() const noexcept { return 0; }
double PluginWindow::contentScale() const noexcept { return 0.0; }
int PluginWindow::posixFd() const noexcept { return -1; }
void PluginWindow::processEvents() noexcept {}
void PluginWindow::startTicking() {}
void PluginWindow::stopTicking() {}
void PluginWindow::setUsableScreenForTest(uint32_t, uint32_t) noexcept {}

#endif

} // namespace feathers::ui
