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

#include "clap/clap.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace visage {
class ApplicationWindow;
}

// A PLUGIN'S WINDOW, AND THE THINGS THAT GO WRONG WITH ONE.
//
// EXTRACTED FROM `gui.cpp` RATHER THAN WRITTEN AGAIN, and the reason is the whole content of this
// header. A second plugin needs a window, and a first attempt at that chose to write a second shell
// on the grounds that the plumbing was only about thirty lines and that CLAP's and visage's APIs are
// EXTERNAL contracts — so two callers of them could not drift the way two copies of one of OUR
// conventions can.
//
// **THAT COUNTED API CALL SITES. What matters is the knowledge wrapped around them, and by that
// measure this is the densest code in the editor:**
//
//   - `show()` DESTROYS THE WINDOW AND ITS VULKAN SWAPCHAIN, and tearing that down joins Mesa's WSI
//     threads — one of which is parked in an untimed `poll(fd, POLLIN, -1)` waiting on the window
//     that was just destroyed. The join never returns, so the HOST'S MAIN THREAD wedges: the editor
//     never appears, saving the project hangs, and Bitwig reports the plugin host is not responding.
//     visage offers no way out — `Renderer::initialize` latches, nothing resets it, `bgfx::shutdown`
//     is called nowhere. **So the rule is: never destroy a window whose swapchain threads are alive.
//     One window, reparented between containers, torn down only at process exit.**
//   - `set_parent` IS RE-ENTRANT. Bitwig calls `get_size`, `adjust_size` and `set_size` from INSIDE
//     the `show()`, before `setParent` has returned. Nothing here may assume it holds the editor to
//     itself.
//   - DETACH REPARENTS TO THE ROOT, not merely unmaps. An unmapped child is still a child, and the
//     host destroying its container would take the editor's window with it — which is exactly the
//     destruction that deadlocks.
//
// **A SECOND COPY OF THAT WOULD NOT FAIL TO COMPILE. It would deadlock on somebody's machine, months
// later, against a host we cannot test** — which is `device_row.h`'s rule for its actual reason: the
// convention that is wrong will be wrong silently.
//
// WHAT STAYS WITH THE EDITOR: everything about what is IN the window. This class knows a size, a
// scale, a parent handle and a heartbeat, and nothing about what is drawn.

namespace feathers::ui {

/// Hold visage's own scale derivation to 1:1 when the X server's DPI says it cannot be right.
///
/// visage works its scale out from the physical size the monitor reports, and on Linux that number
/// is wrong often enough to be a running joke. This display reports 1920 pixels across 508mm, which
/// X reads as 96 DPI — the value it uses when it has nothing better to say — while visage's own
/// arithmetic lands on 168 and a scale of 1.75, opening the editor at 1856x1048 on an ordinary
/// unscaled 1080p screen. A display claiming BOTH "96 DPI" and "physically small enough to need
/// 1.75x" is contradicting itself, and between the two the desktop's own configured number is the
/// one to believe.
///
/// THE TRADE, which is real and was accepted deliberately: a genuine high-DPI laptop whose X server
/// still reports 96 gets a small editor and has to set FEATHERS_UI_SCALE. That is a worse day for
/// that user and a better one for everyone on an ordinary monitor, and the escape hatch is one
/// environment variable.
///
/// This clamps the DERIVATION ONLY. A host that calls clap_plugin_gui.set_scale, or a user who sets
/// FEATHERS_UI_SCALE, has said something explicit and is believed without argument.
inline double clampDerivedScale(double derived, double xServerDpi) noexcept {
	/// The DPI X reports when nobody has told it anything better.
	constexpr double kUninformativeDpi = 96.0;
	if (xServerDpi > 0.0 && xServerDpi <= kUninformativeDpi && derived > 1.0) {
		return 1.0;
	}
	return derived;
}

class PluginWindow {
public:
	/// WHAT A WINDOW NEEDS OF THE SURFACE INSIDE IT — four questions, none of them about drawing.
	///
	/// The size is asked in LOGICAL units, because that is what a surface knows about itself; turning
	/// it into the physical size a host wants is this class's job and involves the display, the
	/// host's scale and the space a window can actually have.
	class Client {
	public:
		virtual ~Client() = default;
		/// Place the contents. Called after a reparent and after any resize.
		virtual void layout() = 0;
		/// The heartbeat, at the editor's own rate.
		virtual void tick() = 0;
		virtual uint32_t logicalWidth() const = 0;
		virtual uint32_t logicalHeight() const = 0;

		/// THE SMALLEST THE SURFACE MAY BE, in physical pixels. The window has no opinion about this —
		/// how small a layout can get before it stops working is a fact about the layout. Extracted as
		/// a Client question because `adjustSize` reached `Gui::Impl::minimumWidth()` directly, which
		/// is the one place the shell was still asking the content what it looked like.
		virtual uint32_t minimumWidth() const = 0;
		virtual uint32_t minimumHeight() const = 0;

		/// ASK THE HOST TO OPEN THE EDITOR AT A SIZE. The window works out WHAT size — that involves the
		/// display, the host's scale and the space a window can actually have — and the client asks,
		/// because the client is the one holding a host. A first draft took a `clap_host*` here instead
		/// and had the window call `guiRequestResize` itself; that put a CLAP interface into a class
		/// whose whole subject is visage and X11.
		virtual void requestResize(uint32_t width, uint32_t height) = 0;

		/// THE WINDOW WAS REBUILT, and these are the client's own handlers to hang on the new one (backlog GP). A host that
		/// destroys its container BEFORE guiDestroy takes our X window with it, and the next open builds a fresh
		/// `ApplicationWindow`. `PluginWindow` moves every child frame across itself, since a child is just a frame. What it
		/// cannot move is what the client set on the window (`onDraw`, `onKeyPress`, `onResize`, keystrokes), so the client
		/// installs them here, from the same function it installs them with at the first build. Two call sites, one
		/// installer, so a handler added at the build can never be missing after a rebuild.
		virtual void windowRebuilt(visage::ApplicationWindow& app) = 0;
	};

	/// WHAT THE X SERVER SAID BACK to the last request this window made on its own connection (backlog GQ).
	///
	/// `reparentTo` and `detachFromParent` each open a private connection, make asynchronous requests and close it.
	/// They used to `XFlush` and close, which sends the bytes and never waits, so every error the server answered with
	/// came back to a connection nobody was reading. A reparent onto a destroyed window reported success. They `XSync`
	/// now, under a scoped error handler that records the errors for that connection and passes everyone else's on.
	struct XReport {
		int errors = 0;
		/// The FIRST error, which is the one that explains the rest: X answers each later request on the same dead
		/// window with the same error.
		int code = 0;          ///< `BadWindow` is 3
		int request = 0;       ///< the major opcode: `ReparentWindow` is 7, `MapWindow` 8, `UnmapWindow` 10
		unsigned long resource = 0;
		/// WHETHER OUR OWN WINDOW IS GONE: a BadWindow naming the editor's window. Not the same as "a request failed",
		/// because a dead PARENT fails the same request with the same code and needs the opposite answer.
		bool selfGone = false;
	};
	/// THE REPORT AS ONE LOG FIELD: `x-errors=2 first=3/7/0x600010 self-gone`, the numbers being code/request/resource.
	static std::string describe(const XReport& report) {
		char text[96];
		std::snprintf(text, sizeof(text), "x-errors=%d first=%d/%d/0x%lx%s", report.errors, report.code, report.request,
		              report.resource, report.selfGone ? " self-gone" : "");
		return text;
	}
	/// The last report, for the caller to log and a case to assert. Empty until a private-connection call has run.
	[[nodiscard]] const XReport& lastXReport() const noexcept;
	/// HOW MANY TIMES THIS WINDOW HAS BEEN REBUILT because the host destroyed it under us (backlog GP).
	[[nodiscard]] int rebuilds() const noexcept;
	/// HOW MANY DEAD WINDOWS THIS PROCESS HAS ABANDONED (leaked on purpose, see `rebuild`), across every instance.
	/// Test-only: the bound is "exactly one per container-first close".
	[[nodiscard]] static size_t abandonedWindowsForTest() noexcept;
	/// Test-only: destroy every abandoned window. A test binary calls it once, after its last case and before static
	/// destruction, because the gate's -UNDEBUG build asserts in visage's InstanceCounter<Window> that none is alive (OL).
	/// Never called by the plugin: the leak is deliberate (see `rebuild`).
	static void releaseAbandonedForTest() noexcept;

	explicit PluginWindow(Client& client);
	~PluginWindow();

	PluginWindow(const PluginWindow&) = delete;
	PluginWindow& operator=(const PluginWindow&) = delete;

	static bool isAvailable() noexcept;
	static bool isApiSupported(const char* api, bool isFloating) noexcept;

	/// BUILD THE WINDOW, ONCE, AND KEEP IT FOR THE LIFE OF THE PROCESS.
	///
	/// A second call on an existing window restarts the heartbeat and returns — it does NOT rebuild.
	/// See the header: rebuilding is the deadlock.
	///
	/// Returns false when the api is not one we embed into. The caller adds its contents to `app()`
	/// after this returns true and only on the first call, which `builtThisCall` reports.
	bool create(const char* api, bool isFloating, bool* builtThisCall) noexcept;

	void destroy() noexcept;

	/// EMBED IN THE HOST'S CONTAINER. False when the embed failed, and that includes an X error the server answered it
	/// with (`lastXReport`). If the host destroyed our window before guiDestroy (backlog GP), the window is rebuilt here
	/// and embedded fresh: see `windowRebuilt`.
	bool setParent(const clap_window* window) noexcept;
	/// WHETHER THERE IS A WINDOW THAT EXISTS. False once the server has told us ours was destroyed, until a
	/// `setParent` rebuilds it. It used to answer true about a dead window, which is how every layer of GP reported
	/// success.
	bool hasWindow() const noexcept;

	bool setScale(double scale) noexcept;
	bool adjustSize(uint32_t* width, uint32_t* height) const noexcept;
	void size(uint32_t* width, uint32_t* height) const noexcept;
	bool setSize(uint32_t width, uint32_t height) noexcept;

	/// The window itself, for a caller adding its contents to it. Null before `create`.
	visage::ApplicationWindow* app() noexcept;
	const visage::ApplicationWindow* app() const noexcept;

	/// SETTLE THE SCALE AND LAY THE CONTENTS OUT — what `create` and `setParent` both need after the
	/// window exists or moves. It is one call rather than two because the order matters: the scale
	/// decides how big the contents think they are.
	void syncScaleAndLayout();

	/// THE CLIENT'S LOGICAL SIZE CHANGED — a canvas opened or closed, a partition released — so the
	/// scale is re-derived, the client laid out, and the host asked to GROW the window only if the
	/// contents no longer fit in it. Never to shrink it, and never to reproportion a window that
	/// already holds them (2026-09-17: "opening/closing the canvas reproportions the window. we should
	/// proportion/resize so it fits inside the original window provided its large enough"). A window
	/// that is too short grows to exactly the contents' height; its width is left alone unless that is
	/// short too. The first open and a UI-scale change still ask for the preferred size outright
	/// (`requestPreferredSize`): those are the moments the window is being sized, not merely used.
	void preferredSizeChanged();

	/// **THE CLIENT GAVE UP OR TOOK BACK `logicalDelta` OF WIDTH, AND THE WINDOW FOLLOWS IT EXACTLY** (backlog NF: the
	/// stage closing to its strip, and the synth simply shrinks). Unlike `preferredSizeChanged` this
	/// shrinks as readily as it grows, because the room is the window's own and not a fit: the host is asked for its
	/// current width plus the delta at the scale in force, the height left as it is, and no narrower than the floor
	/// `adjustSize` holds it to. Laid out either way; with no window there is no host to ask.
	void widthChangedBy(double logicalDelta);

	/// HOW MANY LOGICAL ROWS THE WINDOW HOLDS at the scale in force — its height in host units over the
	/// preferred scale. What a canvas opening asks so it can take the room that exists rather than ask
	/// for more. Zero when there is no window.
	double logicalRoomHeight() const noexcept;

	/// THE SCALE THIS EDITOR WOULD DRAW AT GIVEN ROOM — the host's, an override's, or visage's
	/// derivation, in that order — and the window's native height. What a partition drag needs to put
	/// the canvas's edge under the pointer while the other side keeps its design size
	/// (`ExpandedCanvas::partitionExtent`). Zero when there is no window.
	///
	/// ALL IN HOST UNITS: the window's size as the host measures it (points on macOS, pixels on X11), and
	/// `contentScale` the host units per logical unit the editor is laid out at. A pointer's logical
	/// position times `contentScale` is therefore on the same axis as `nativeHeight`, on every platform —
	/// which the frame's own dpi scale is not on macOS, where it also carries the Retina backing factor.
	double preferredScale() const noexcept;
	int nativeHeight() const noexcept;
	int nativeWidth() const noexcept;
	double contentScale() const noexcept;

	//-- THE GLOBAL UI SCALE (backlog EJ: "a global scaling setting. apart from window
	//-- size, which just enlarges the window but the widgets stay the same size"). ------------------//
	/// A multiplier on the display's scale, 1.0 = the design as drawn at the display's DPI. Applied to
	/// every scale decision this window makes; the window's own size does NOT feed the scale any more,
	/// so a bigger window is more room and a smaller one is the same widgets, clipped. Persisted by the
	/// editors in `EditorPrefs` under `kUiScaleKey`, so both plugins and every instance share it.
	void setUiScale(double scale) noexcept;
	[[nodiscard]] double uiScale() const noexcept;
	/// The ladder a hand steps through: 100 % to 200 % in the steps a face stays legible at. `direction`
	/// +1 steps up, -1 down, 0 resets; a value off the ladder steps to the nearest rung in that direction.
	[[nodiscard]] static double stepUiScale(double current, int direction) noexcept;
	/// The rungs themselves, for a face that offers all of them at once (the settings screen, backlog LG).
	static constexpr std::array<double, 7> kUiScaleLadder { 1.0, 1.125, 1.25, 1.375, 1.5, 1.75, 2.0 };
	/// Which way a key steps the scale, by its `visage::KeyCode` value: `=`, `+` and keypad plus are +1;
	/// `-` and keypad minus are -1; `0` and keypad 0 are 0 (reset); anything else is 2, "not a scale key".
	/// The modifier test is the caller's, so a bare key never reaches this.
	[[nodiscard]] static int uiScaleStepFor(int keyCode) noexcept;
	static constexpr const char* kUiScaleKey = "ui.scale";
	/// The scale actually in force on the content — the number every widget is drawn at. Test-only.
	[[nodiscard]] double contentScaleForTest() const noexcept;
	/// TEST ONLY — THE USABLE SCREEN AREA, in host units, in place of the one X reports (`_NET_WORKAREA`, else nine
	/// tenths of the display) and the display's own room. Every size this window clamps to the screen reads it: the
	/// size it answers before it is embedded, the size it asks for, and `adjustSize`'s floor. Process-wide, like the
	/// screen; 0 x 0 puts the real one back.
	static void setUsableScreenForTest(uint32_t width, uint32_t height) noexcept;

	/// THE HOST'S POLLING HANDLE, or -1 for none. X11 only — the base `Window::posixFd` answers 0,
	/// which on every other backend is not "nothing to poll" but STDIN.
	int posixFd() const noexcept;

	/// DRAIN THE WINDOW'S OWN EVENT QUEUE. A plugin window is not pumped by anybody unless its host is
	/// told to, so this is what `onPosixFd` and `onTimer` call.
	///
	/// **WITHOUT IT THE EDITOR NEVER PAINTS.** Not "paints late" — never: the rack came up as a black
	/// rectangle with `draw()` being called at the right size on a visible frame with a parent, and
	/// every canvas operation going nowhere, because nothing was draining the queue those operations
	/// end up on. It was missing because the plugin implemented neither posix-fd nor timer support,
	/// which the shell cannot do for it.
	void processEvents() noexcept;

	void startTicking();
	void stopTicking();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace feathers::ui
