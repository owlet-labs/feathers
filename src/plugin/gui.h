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

#include "plugin/ui/plugin_window.h"

#include "clap/ext/gui.h"
#include "clap/stream.h"
#include "clap/id.h"

#include <cstdint>
#include <functional>
#include <string>
#include <memory>

// The editor: a visage window, GPU-rendered, embedded in the window the host hands us.
//
// Everything visage-shaped lives behind Impl, so the CLAP binding includes this header and no
// graphics one. That keeps the binding compiling in seconds rather than dragging bgfx through every
// rebuild, and it is what lets the plugin be built with no editor at all on a platform this does not
// cover yet.
//
// WHERE THE FRAMES COME FROM, which is the whole of the Linux integration. A plugin does not own the
// main thread — the host does — so something has to give visage main-thread time to draw on. visage's
// answer is to run a timer thread that pokes its own X11 connection, which turns "time to draw" into
// "an X event is readable"; the host then hands us that thread back through
// clap_host_posix_fd_support, and processEvents() drains the connection and draws inside it. So the
// render happens on the main thread, driven by a background thread that only ever writes an X
// message. Nothing here runs on, or blocks, the audio thread.
//
// A host with no posix-fd support gets the timer fallback instead: same processEvents(), called from
// clap_plugin_timer_support. A host with neither cannot show this editor, and create() says so by
// failing rather than handing back a window that never paints.

// FORWARD DECLARED, NEVER INCLUDED, and that is the rule above being kept rather than bent: the two
// accessors at the end of Gui hand back pointers to these, and a pointer to an incomplete type costs
// the CLAP binding nothing. Whoever dereferences one is including the graphics headers already.
namespace visage {
class Canvas;
class Frame;
}

namespace feathers::ui {
class WidgetRegistry;
class EditHistory;
class FrameWatchers;
}

namespace feathers::ui {
class ModState;
class SpectrumPanel;
class DrumPanel;
class SoundSelection;
class ExpandedCanvas;
class HeaderBar;
class SettingsScreen;
class ExpandedOverlay;
class SurfaceLayers;
class WindowedExpanded;
class FxLane;
class StageRail;
class ExploreStage;
class PresetBrowser;
class NotifyTray;
}

namespace feathers {

// `clampDerivedScale` MOVED TO plugin_window.h with the rest of the window's scale reasoning —
// it is about what a DISPLAY told us, not about what this editor draws.

class ParamBridge;

/// What the editor needs from the plugin, kept to an interface so the GUI can be exercised without a
/// plugin and the plugin does not have to expose its ParamSet.
class GuiHost {
public:
	virtual ~GuiHost() = default;

	/// **ONE BEAT OF THE EDITOR'S OWN HEARTBEAT, ON THE MESSAGE THREAD, ON EVERY PLATFORM.** Called
	/// from `PluginWindow`'s timer after the editor's own tick, so the plugin can do the once-per-
	/// main-thread-visit work that used to live only on the HOST'S callbacks — and on macOS those
	/// callbacks are not the road the editor lives by.
	///
	/// 2026-09-07, first Mac build: no effect worked besides the default filter (even another
	/// filter added later). The device built at activate worked; every device added afterwards was
	/// inert with its card live. An added device reached the synth ONLY through `pumpEditor`, which
	/// runs from `onPosixFd` (the X11 fd — Linux) or `onTimer` (the CLAP timer registered as a
	/// fallback in `guiSetParent`). Meanwhile the editor stays alive by a DIFFERENT road on every
	/// platform: the Heartbeat (`visage::EventTimer`) ticks the editor whether or not the host ever
	/// calls back. So on a host that answers timer support and does not deliver it — or never reaches
	/// the registering branch — the card turned on, the editor ticked, and the synth was never told.
	/// REPRODUCED ON LINUX in `test_clap_render` with a host whose timer registers and never fires.
	///
	/// Default no-op: six GuiHost implementations in tests and tools have no synth to provision.
	virtual void guiTick() noexcept {}

	/// Ask the host to call clap_plugin_params.flush, because the editor has moved something and the
	/// plugin may not be processing. Without it, a knob turned on a stopped transport would sit in the
	/// queue until the user pressed play. Main thread.
	virtual void guiRequestFlush() noexcept = 0;

	/// Ask the host to resize the window it gave us, because the editor's own contents changed size —
	/// a DPI scale change, or visage settling on a size that is not exactly what was requested. Without
	/// this the host's container and the editor inside it disagree, and the difference shows as a
	/// margin or a crop. Main thread.
	virtual void guiRequestResize(uint32_t width, uint32_t height) noexcept = 0;

	/// CAPTURE THE FULL PLUGIN STATE into the given CLAP output stream — parameters followed by every
	/// tagged state block, in the same order the CLAP state extension writes them. The preset browser
	/// calls this to save the current patch. Main thread.
	virtual void guiCaptureState(clap_ostream_t* stream) noexcept = 0;

	/// RESTORE THE FULL PLUGIN STATE from the given CLAP input stream — the same format
	/// guiCaptureState writes. The preset browser calls this to load a patch. Main thread.
	///
	/// TRUE ONLY IF THE PATCH ACTUALLY LOADED, and the return value is the whole reason this is not
	/// `void`. It was, and a preset that refused to load was then indistinguishable from one that
	/// loaded: the user picks a preset, the sound does not change, and nothing says why. Unreachable
	/// while every file on disk was written by the same build — and presets became FILES an hour
	/// before this comment, so "every file was written by the same build" stopped being something
	/// anyone can promise. ParamSet::load already refuses a stream whose version byte it does not
	/// know, staging into a local and committing nothing on a bad read; what was missing was any way
	/// for it to say so to the person who clicked.
	virtual bool guiLoadState(clap_istream_t* stream) noexcept = 0;
};

/// The plugin's editor, one per plugin instance, created and destroyed on the main thread.
///
/// The object exists for the plugin's whole life; `create()` and `destroy()` are the CLAP lifecycle
/// and may run more than once (a host reopening the editor), so nothing here assumes a single pass.
class Gui {
public:
	/// The bridge is the editor's only contact with the engine — see gui_bridge.h. It outlives the
	/// window, so opening and closing the editor loses no queued work.
	Gui(GuiHost& host, ParamBridge& bridge) noexcept;
	~Gui();

	Gui(const Gui&) = delete;
	Gui& operator=(const Gui&) = delete;

	/// Whether this build has an editor backend at all. False on platforms the GUI is not built for,
	/// which is what stops the plugin advertising an editor it cannot open.
	static bool isAvailable() noexcept;

	/// X11 embedded only. Floating windows and the mac/Windows APIs are later work.
	static bool isApiSupported(const char* api, bool isFloating) noexcept;

	bool create(const char* api, bool isFloating) noexcept;
	void destroy() noexcept;

	/// Embeds the window in the host's, which is also what makes it visible: visage has no separate
	/// show step for a plugin window.
	bool setParent(const clap_window* window) noexcept;

	/// Whether there is a native window to speak of. This is the honest answer to "did the embed
	/// work" — and, because the editor is visible from setParent() onward, also the answer to CLAP's
	/// show(). It is deliberately not a question about whether the window is on screen: that depends
	/// on the host's ancestors being mapped, which the host does on its own schedule.
	bool hasWindow() const noexcept;
	/// WHAT THE X SERVER ANSWERED the last embed or detach (backlog GQ), and how many times the window has been rebuilt
	/// because the host destroyed it under us (backlog GP): for the plugin's event log, and for a case to assert.
	const ui::PluginWindow::XReport& lastXReport() const noexcept;
	int windowRebuilds() const noexcept;

	/// The host's DPI scale, and the most trusted of the three sources the editor has for one — see
	/// the note on scale in gui.cpp. Acting on it may change the size the editor wants, in which case
	/// it asks the host for that size.
	bool setScale(double scale) noexcept;

	/// What the editor would really become if it were given this size: the request, raised to the
	/// smallest size the layout fits in. IDEMPOTENT, which is not decoration — a host asks this, then
	/// hands back the answer through setSize, and compares what getSize then reports. If any of those
	/// three disagree the two of them negotiate forever, which is what a user sees as a window that
	/// will not settle.
	bool adjustSize(uint32_t* width, uint32_t* height) const noexcept;

	bool setSize(uint32_t width, uint32_t height) noexcept;
	bool getSize(uint32_t* width, uint32_t* height) const noexcept;

	/// The editor's logical size: the mock's canvas, in the units the layout is written in. The
	/// window is this multiplied by the scale, and the layout never has less room than this.
	static constexpr uint32_t kLogicalWidth = 1240;
	static constexpr uint32_t kLogicalHeight = 700;

	/// The X11 connection descriptor to hand clap_host_posix_fd_support, or -1 when there is no window
	/// yet. Only meaningful between setParent() and destroy().
	int posixFd() const noexcept;

	/// Drain the window's pending events, drawing a frame if one is due. Main thread, from on_fd or —
	/// where the host has no posix-fd support — from on_timer. See the note at the top of this file.
	void processEvents() noexcept;

	/// How often a host driving this from a timer should call processEvents(). Ignored on the posix-fd
	/// path, where visage's own timer thread sets the frame rate.
	static constexpr uint32_t kTimerPeriodMs = 16;

	/// The assembled editor, for a venue that has to route an event the way a window would.
	///
	/// THE GAP THESE FILL, and it is the gap that left a bug report unanswerable: widgets are tested in
	/// isolation, the embed protocol is tested by bitwighost, and NOTHING tested that a click at a
	/// coordinate reaches the widget that owns it through the tree THIS file assembles. A control can
	/// be perfect on its own and unreachable in place — covered by a panel, outside its parent's
	/// bounds, behind something that answers the mouse first — and none of those is visible to a
	/// widget's own suite.
	///
	/// Reads of what already exists rather than a way to drive the editor: `root` is the frame a window
	/// would deliver an event to, and `widgets` is how a caller finds a control without restating the
	/// layout that placed it. Null before create() and after destroy().
	visage::Frame* root() noexcept;
	const ui::WidgetRegistry* widgets() const noexcept;

	/// The non-parameter channels' repaint list, for the same reason `widgets` is here: "this surface
	/// repaints when that state moves" cannot be asserted from a redraw, because visage's redraw() is
	/// a no-op on a frame that is not being drawn. What a headless test CAN check is that the surface
	/// is subscribed, and this is how it finds out.
	const ui::FrameWatchers* stateWatchers() const noexcept;

	/// EVERY WATCHER LIST THIS EDITOR OWNS, COUNTED IN ONE PLACE.
	///
	/// SEVEN LISTS, AND `forgetFrameTree` REACHES THREE. The registry, the state channel and
	/// `ModState`'s have an exit; `previewWatchers`, `EditHistory`'s, `SoundSelection`'s and
	/// `StageMode`'s do not, and are safe only because every subscriber they have today is an
	/// editor-lifetime surface. That is a fact about who happened to register, not a guarantee — the
	/// same property the three shipped dangling-watcher bugs had until a card became removable.
	///
	/// SO THE ENUMERATION LIVES HERE RATHER THAN IN THE CASE. A test that named the lists itself
	/// would go on passing the day an eighth is added, and the omission would be invisible; a struct
	/// the editor fills is at least one place to look, and the case asserts a return per field.
	struct WatcherCounts {
		size_t registry = 0;
		size_t state = 0;
		size_t preview = 0;
		size_t modulation = 0;
		size_t history = 0;
		size_t selection = 0;
		size_t mode = 0;
	};
	WatcherCounts watcherCountsForTest() const noexcept;

	/// THE MODULATION STATE THE WHOLE EDITOR SHARES. Exposed so a case can hand it to a surface built
	/// OUTSIDE the tree and then assert that arming in the editor reaches it — which is the claim the
	/// expanded panel rests on and the one that has to survive the panel becoming a window. Arming is
	/// state, not geometry; this accessor is what lets a test say so without a second window.
	ui::ModState* modForTest() noexcept;
	/// THE SPECTRUM PANEL WHILE IT IS OPEN, or null — the handle the editor keeps (it is owned by its host), so a case can
	/// open it by the tab, find it, and see it gone after Esc, ✕, the tab again or an engine change.
	ui::SpectrumPanel* spectrumPanelForTest() noexcept;
	/// TEST ONLY: the drum kit panel while it is open (the Drum tab on Explore opens it).
	ui::DrumPanel* drumPanelForTest() noexcept;
	/// THE SELECTED SOUND, which the panel follows — a case moves it as a hand on the sound stack would.
	ui::SoundSelection* selectionForTest() noexcept;

	/// THE EXPANDED LAYER, because it owns the editor's ONLY keyboard road and therefore the only
	/// place Esc can be injected. A case that wants to prove "Esc leaves assign mode" has to drive
	/// the key through the thing that receives it; without this it could only prove the callback
	/// fires, which is a claim about wiring rather than about the mode.
	ui::ExpandedOverlay* expandedForTest() noexcept;
	/// THE CANVAS AND THE LAYERS THAT ROUTE TO IT, so a case can read WHERE a present landed — a
	/// value, per the rule that "a panel appeared" cannot tell two hosts apart.
	ui::ExpandedCanvas* canvasForTest() noexcept;
	/// THE GLOBAL UI SCALE IN FORCE (backlog EJ): the user's multiplier, and the content scale every widget
	/// is drawn at (display x multiplier). Neither is recoverable from the tree's logical bounds.
	double uiScaleForTest() const noexcept;
	double contentScaleForTest() const noexcept;
	/// THE WINDOW'S OWN GROUND, painted into `canvas` (backlog LI): the void and the footer's separator, the
	/// layer every frame composites over. A case that composites the stage over it at a scale can read the
	/// seam's pixels; nothing else draws the separator, so nothing else can stand in for this.
	void drawBackgroundForTest(visage::Canvas& canvas) const;
	/// THE HEADER, so a case can click its scale buttons and prove the click reaches the same road the
	/// keys take — the value assertion is `uiScaleForTest`, this is only how the click gets in.
	ui::HeaderBar* headerForTest() noexcept;
	/// THE SETTINGS SCREEN (backlog LG), so a case can open it through the header and click what a hand would.
	ui::SettingsScreen* settingsForTest() noexcept;
	ui::SurfaceLayers* surfacesForTest() noexcept;

	/// THE WINDOW HOST, and the preference that chooses between the two.
	///
	/// A CASE HAS TO BE ABLE TO ASK WHICH ONE OPENED, because both roads end in a panel that draws the
	/// same content — so "a panel appeared" cannot distinguish them. The claim is about WHICH HOST
	/// took it, and that is a value.
	ui::WindowedExpanded* expandedWindowForTest() noexcept;
	void setPopoutWindowForTest(bool on) noexcept;
	/// THE LANE, so a case can drive `onExpandDevice` — the road the card's expand mark actually
	/// takes — instead of a seam invented for the test.
	ui::FxLane* fxLaneForTest() noexcept;
	/// THE STAGE'S COLLAPSE AND THE SURFACE IT HIDES (backlog NF), so a case can press the toggle a hand presses and
	/// read what the closed stage did and did not do — its geometry, its paints, its terrain's builds.
	ui::StageRail* stageRailForTest() noexcept;
	ui::ExploreStage* exploreForTest() noexcept;

	/// WHAT ONE FRAME OF EDITOR BOOKKEEPING ACTUALLY DELIVERED — how many watching frames each of the
	/// two roads told. Returned by the tick and ignored by its real caller; it exists so a test can
	/// see that the join happened, which a repaint cannot say for itself.
	struct TickReport {
		size_t paramFrames = 0;
		size_t stateFrames = 0;
		/// How many DEVICE surfaces asked for the screen off their own live engine readings — the
		/// road a block-rate publisher takes instead of the state channel. Counted BY the update
		/// that made the request, for the reason the two above are.
		size_t deviceFrames = 0;
		/// How many frames the BYPASS flag told this tick — the polled Alt/Option road, counted by
		/// `ModState::setBypass`'s own return. Zero on every frame the modifier did not change.
		size_t bypassFrames = 0;
		/// How many header macros were RE-SEATED as another control this tick — a type that moved under
		/// the row without the row's own menu moving it, which is a patch restore. Counted by
		/// `ModPanel::syncMacroSlots`'s own return, beside `stateFrames` on the same repaint flag.
		size_t macroSlots = 0;
	};

	/// TEST ONLY. Run one frame's worth of editor bookkeeping — the body processEvents() runs before
	/// it pumps the window.
	///
	/// It exists because processEvents() returns early without a window, so a headless venue cannot
	/// reach the tick at all, and the tick is where the engine-to-editor half of the editor lives. A
	/// mutation that stopped it draining the repaint bit survived both of the other checks on that
	/// feature — the bit can rise and the surfaces can be subscribed while nothing ever connects
	/// them. What this pins is the tick's own contract; the window pump is not part of that claim
	/// and is not run here.
	TickReport tickForTest() noexcept;

	/// TEST ONLY. What the editor's OWN heartbeat has delivered since this was last asked; reading it
	/// clears the total.
	///
	/// It is the same count tickForTest() returns and it is made in the same place — inside
	/// registry.notify() and stateWatchers.notify(), BY the notification. The difference is who called
	/// the tick: this total can only be non-zero if something other than a test or a host callback
	/// drove one, which is the whole claim. Delete the heartbeat and no arrangement of visage's frame
	/// loop can make this number move.
	TickReport takeDriveReportForTest() noexcept;

	/// TEST ONLY. Stand in for the window's live modifier read in the tick's bypass poll — a
	/// function returning a visage `kModifier*` mask. A headless Gui has no window to ask and a case
	/// cannot press a real Alt, so this is how a case says "Alt is down, with no mouse event" and
	/// then asks the tick what it told. Null (the default) restores the window read. A NEGATIVE return
	/// stands in for a platform that cannot report its modifiers (`Window::hasModifierState()` false):
	/// the poll then leaves the event-fed bypass alone.
	void setModifierProbeForTest(int (*probe)()) noexcept;

	/// TEST ONLY. The preset browser, so a case can ask the frame tree "what would a click here
	/// reach" and compare the answer against the widget itself. The routing question cannot be
	/// answered from the widget alone — that is the whole point of asking it.
	ui::PresetBrowser* presetBrowserForTest() noexcept;

	/// TEST ONLY. What the notify tray is currently saying, empty if nothing.
	///
	/// The tray is how this editor tells a player something went wrong, so "did it say anything" is a
	/// claim a case has to be able to make. Reading the pixels cannot: the text is rasterised into an
	/// atlas and the tray fades, so a capture answers "is something drawn" and never "what".
	std::string notifyTextForTest() const noexcept;
	/// THE PRESET TOAST ("Loaded: <name>") and a clock for it, so a case can walk its 100 ms hold and its
	/// fade without waiting. Null outside an editor.
	ui::NotifyTray* presetToastForTest() noexcept;
	void setPresetToastClockForTest(std::function<int64_t()> clock);

	/// The editor's one undo history, for a case that has just made an edit through a real widget and
	/// wants to walk it back the way the corner buttons do. Same tier as the two above: a read of what
	/// exists, not a second way to drive the editor.
	ui::EditHistory* history() noexcept;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace feathers
