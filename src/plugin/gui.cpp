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

#include "plugin/gui.h"

#include "plugin/editor_prefs.h"
#include "plugin/gui_bridge.h"

#include <cstring>

#if defined(FEATHERS_GUI_VISAGE)

#include "plugin/ui/keyboard_claim.h"
#include "plugin/ui/knob_tray.h"
#include "plugin/ui/master_meter.h"
#include "plugin/ui/os_clipboard.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/explore_stage.h"
#include "plugin/ui/filter_card.h"
#include "plugin/ui/owltt_expanded.h"
#include "plugin/ui/expanded_canvas.h"
#include "plugin/ui/expanded_overlay.h"
#include "plugin/ui/surface_layers.h"
#include "plugin/ui/windowed_expanded.h"
#include "plugin/bridge_device_host.h"
#include "plugin/ui/expanded_views.h"
#include "rack/chain_expanded.h"
#include "plugin/ui/fx_lane.h"
#include "plugin/ui/harm_expanded.h"
#include "plugin/ui/header_bar.h"
#include "plugin/ui/engine_rail.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/mod_panel.h"
#include "plugin/ui/notify_tray.h"
#include "plugin/ui/mod_readout_layer.h"
#include "plugin/ui/perform_stage.h"
#include "plugin/ui/orientation_row.h"
#include "plugin/ui/param_row.h"
#include "plugin/ui/preset_browser.h"
#include "plugin/ui/stage_rail.h"
#include "plugin/ui/sound_stack.h"
#include "plugin/ui/theme.h"
#include "plugin/ui/tooltip_prefs.h"
#include "plugin/ui/chrome_hue_prefs.h"
#include "plugin/ui/window_controls_side_prefs.h"
#include "plugin/ui/emitters_front_prefs.h"
#include "plugin/ui/full_redraw.h"
#include "plugin/ui/settings_screen.h"
#include "plugin/ui/tuning_row.h"
#include "plugin/ui/wave_display.h"
#include "plugin/ui/text_field.h"
#include "plugin/ui/drum_panel.h"
#include "plugin/ui/spectrum_panel.h"
#include "plugin/ui/widget_registry.h"

#include "clap/stream.h"

#include <visage/app.h>
#include <visage/ui.h>
#include <visage/utils.h>

#if defined(__linux__)
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#endif

namespace feathers {

namespace {

/// The editor's logical size, which is the mock's canvas. The host is free to resize from here —
/// `guiCanResize` says so — but never below it: the layout is written in these units and has fixed
/// furniture in it (a header, an Engine rail, a knob tray), so a smaller logical canvas would crop
/// controls rather than tighten them.
constexpr uint32_t kDefaultWidth = Gui::kLogicalWidth;
constexpr uint32_t kDefaultHeight = Gui::kLogicalHeight;

/// THE NAMES THE PREFERENCES FILE USES, namespaced so that whatever the editor learns to remember
/// next joins as new keys rather than as a new file format. Written once here and read once here —
/// a key spelled twice is a preference that saves under one name and loads under another.
/// WHETHER AN EXPANDED VIEW OPENS IN ITS OWN WINDOW. **Default TRUE** — decided 2026-08-28.
///
/// IT IS A SWITCH RATHER THAN A MIGRATION, and the reason is not caution about our own code. CLAP has
/// no model for a secondary plugin-owned window: `is_floating` describes the MAIN editor, so this one
/// gets no `set_transient`, the host does not know it exists, and focus behaviour differs per host
/// and window manager (research doc §4). **There will be hosts where the window misbehaves and the
/// overlay is the answer there** — set this false and everything works as it did.
constexpr const char* kPopoutWindowKey = "popout.window";

constexpr const char* kEngineRailKey = "rail.inspector.collapsed"; // the stored pref keeps its old name: renaming it would drop a saved collapse state (QH)
constexpr const char* kModulatorRailKey = "rail.modulators.collapsed";
constexpr const char* kFxLaneKey = "rail.fx.collapsed";
/// THE STAGE'S COLLAPSE (backlog NF) — a rail like the others, so a desk preference like theirs.
constexpr const char* kStageRailKey = "rail.explore.collapsed";
/// The header's preset list filed by tag rather than by folder — a habit of the desk, not of a patch.
constexpr const char* kPresetsByTagKey = "presets.view.tags";
// THE CANVAS HAS NO DESK KEY: its fold and its partition travel in the PATCH (`rack::CanvasView`), by
// the decision of 2026-09-13, the way a card's fold does.

// **A CARD'S FOLD IS NO LONGER A DESK PREFERENCE — IT TRAVELS IN THE PATCH**. The key that stood here was `card.modulators.<slot>.minimised`, numbered by the card's POSITION
// because nothing else could serve: the head source's flat index died with the lanes, and a device's id
// could not be a desk key, since ids belong to a patch and two patches both hold a device 1. A position is
// not an identity either — a reorder moved a remembered fold onto whichever card now sat there — and that
// is the defect this decision settles. The fold lives with the device now (`ParamBridge::deviceFolded`,
// `rack_state.h`'s DeviceViews); the RAILS stay here, because a rail is a fact about a window.

} // namespace

#if defined(FEATHERS_GUI_VISAGE)

namespace {

using namespace feathers::ui;

/// What the X server says the display's resolution is, in dots per inch, or 0 when it cannot be
/// asked. The sanity check for visage's own derivation — see clampDerivedScale in gui.h.
///
/// GUARDED WITH THE SAME CONDITION AS ITS ONLY CALLER, which is the Apple branch of the scale
/// derivation below: macOS returns 1.0 outright and never asks the display anything, so on that
/// platform this is a function nobody calls — `-Wunused-function`, and macOS builds with -Werror.
/// The guard has to be `!__APPLE__` rather than `__linux__`, because the caller's `#else` covers
/// Windows too and the body already answers 0.0 there.
#if !defined(__APPLE__)
#endif // !__APPLE__

/// **A PRESET LOAD AS ONE UNDO STEP**.
/// The two patches are the WHOLE state as bytes — every parameter and every tagged block, the capture a
/// save makes — so an undo is a load of the patch that was playing, not a replay of whatever moved. A
/// whole-patch step sits cleanly in a linear stack: every step below it was recorded against the patch
/// it puts back, and every step above it against the one it took away.
class PresetLoadEdit final : public EditRecord {
public:
	struct Patch {
		PresetData bytes;
		std::string name;
		std::string category;
	};
	using Replay = std::function<void(const Patch&)>;
	PresetLoadEdit(Replay replay, Patch before, Patch after)
	    : replay_(std::move(replay)), before_(std::move(before)), after_(std::move(after)) {}
	void undo() const override { replay_(before_); }
	void redo() const override { replay_(after_); }
	const char* verb() const override { return "Preset load"; }

private:
	Replay replay_;
	Patch before_;
	Patch after_;
};

} // namespace

/// THE MODULATION STATE, HELD IN A BASE SO IT CANNOT BE OUTLIVED.
///
/// `Knob`, `RouteList`, `ModPanel` and `EnvCard` take themselves off `ModState`'s watcher list in
/// their OWN destructors, so every widget that watches it must die BEFORE it. As a plain member that
/// is a rule about declaration order, which is invisible at the point where it is broken and silent
/// when it breaks — MEASURED 2026-08-31, `mod` sat below `Engine rail`, `ModState` died first, and
/// ASan reported a heap-use-after-free in 252 of 517 widget cases while `ctest` stayed 72/72.
///
/// A BASE SUBOBJECT IS DESTROYED AFTER EVERY MEMBER, by [class.base.init]/13. So holding it here
/// makes the safe order the ONLY order: no member of `Impl`, present or future, can outlive `mod`,
/// and nobody has to remember where in a fifty-line member block to declare it. The bug is not
/// caught, it is unrepresentable.
struct ModStateHolder {
	ModStateHolder() = default;
	ui::ModState mod;
};

/// **THE WATCHER LISTS, IN A BASE — so no member can outlive them, whatever order anyone declares in.**
///
/// A BASE SUBOBJECT IS DESTROYED AFTER EVERY MEMBER. That is the whole mechanism, and it is the same
/// one `ModStateHolder` above uses for the same reason (§17.4 of the rack MVP doc records why that one
/// exists).
///
/// MEASURED, 2026-09-01: `stateWatchers` was declared at line 240 and `fxLane` at 197, so the LIST was
/// destroyed FIRST — while every card still held a live subscription token. `~FrameWatchers`'s assert
/// fired in 255 widget cases under ASan. Nothing corrupted, because `ui::Watch` finds the list's
/// sentinel emptied and skips; but the ordering was wrong and the assert was right to say so.
///
/// `expandedWindow`'s declaration twelve lines below the old site already argued this exact case —
/// *"put the member where the SAFE order is the default, rather than leaving it as a thing to
/// remember"* — for ONE member. This applies it to the thing they all point at.
struct WatcherHolder {
	/// **EVERY LIST A WIDGET CAN SUBSCRIBE TO LIVES HERE**, not just the two that were found first.
	///
	/// The registry, the selection, the stage mode and the history all own a watcher list, and all
	/// four were plain members — so each carried the same latent ordering bug as `stateWatchers` did,
	/// waiting for a widget declared before it. `HeaderBar` was the one that proved it: with the two
	/// `FrameWatchers` moved and nothing else, it was still on `StageMode`'s list at teardown.
	///
	/// Putting them in the base is not tidiness. It makes "declared in the wrong place" unrepresentable
	/// for this whole family, which is the same trade `ui::Watch` makes for "forgot to unwatch".
	WidgetRegistry registry;
	SoundSelection selection;
	StageMode mode;
	EditHistory history;

	/// WHAT THE AUDITION PREVIEW MOVES. Separate from the state list because a preview repaint is
	/// cheap and frequent, and the two populations barely overlap.
	ui::FrameWatchers previewWatchers;

	/// WHAT EDITOR STATE MOVES — the road a swallowed device's card repaints through, since its
	/// settings are not parameters and the registry cannot carry them.
	ui::FrameWatchers stateWatchers;
};

struct Gui::Impl : ModStateHolder, WatcherHolder, PluginWindow::Client {
	/// THE PARAMETER ROAD RECORDS INTO THIS EDITOR'S HISTORY (backlog JB) — see `ParamBridge::editHistory` —
	/// for exactly as long as the history exists. Taken back in the destructor's body, which runs before any
	/// member is destroyed, and only if it is still ours.
	Impl(GuiHost& h, ParamBridge& b) noexcept
	    : host(h), bridge(b) {
		bridge.setEditHistory(&history);
		ui::bindOsClipboard(); // copy and cut reach the system clipboard, and paste reads a clip of ours from it (OA)
	}
	~Impl() {
		ui::unbindOsClipboard();
		if (bridge.editHistory() == &history) {
			bridge.setEditHistory(nullptr);
		}
	}
	Impl(const Impl&) = delete;
	Impl& operator=(const Impl&) = delete;

	GuiHost& host;
	ParamBridge& bridge;

	/// WHICH WAY AN UNDO CHORD WALKS: false for undo (main modifier + Z), true for redo (main modifier +
	/// Shift + Z, or main modifier + Y); nullopt for any other key. Alt held is someone else's chord.
	static std::optional<bool> undoChordOf(const visage::KeyEvent& e) {
		if (!e.isMainModifier() || e.isAltDown()) {
			return std::nullopt;
		}
		if (e.keyCode() == visage::KeyCode::Z) {
			return e.isShiftDown();
		}
		if (e.keyCode() == visage::KeyCode::Y && !e.isShiftDown()) {
			return true;
		}
		return std::nullopt;
	}

	/// A TEXT ENTRY THAT IS EDITING AND HOLDS THE KEYBOARD, anywhere under `frame`. Its Ctrl+Z is the
	/// field's (visage's `TextEditor` keeps its own undo and never lets the chord up) or, for our
	/// `TextField`, which has none, the host's — never this editor's history.
	static bool textEntryFocusedUnder(visage::Frame* frame) {
		if (frame == nullptr || !frame->isVisible()) {
			return false;
		}
		// OUR FIELD BY ITS OWN WORD: it edits only while it holds the keyboard (a focus loss ends the edit),
		// so `editing()` is the answer even where no window event handler tracks the focus.
		if (const auto* field = dynamic_cast<const TextField*>(frame); field != nullptr && field->editing()) {
			return true;
		}
		// visage's `TextEditor` answers `receivesTextInput` whenever it exists, so only a FOCUSED one counts.
		if (frame->hasKeyboardFocus() && frame->receivesTextInput()) {
			return true;
		}
		for (visage::Frame* child : frame->children()) {
			if (textEntryFocusedUnder(child)) {
				return true;
			}
		}
		return false;
	}

	/// THE CHORD'S ACT: walk the history if it can be walked and nothing is typing. True only when it
	/// moved — the key is then ours; false hands it back to the host unhandled.
	/// `surface` is the window the key arrived in when it is not the editor's own (the popout), asked the same question.
	bool walkHistoryFromKey(bool redo, visage::Frame* surface = nullptr) {
		if (textEntryFocusedUnder(window.app()) || textEntryFocusedUnder(surface)) {
			return false;
		}
		if (redo ? !history.canRedo() : !history.canUndo()) {
			return false;
		}
		if (redo) {
			history.redo();
		}
		else {
			history.undo();
		}
		return true;
	}

	/// **ESC: CLOSE THE MOST RECENTLY OPENED THING, AND SAY WHETHER THERE WAS ONE** (backlog NN, 2026-10-18: close the most recently opened). The editor's one stack (`escape_stack.h`) holds every
	/// device window, browser level, the assign mode, the preset prompt and the settings screen in the order
	/// they opened; this closes its top. It used to be "leave assign mode", reached only when the overlay had
	/// declined the key — which is how a window opened BEFORE an arm got closed by the Esc meant for the arm.
	///
	/// LEAVING ASSIGN MODE STILL ROUTES NOTHING AND KEEPS THE FOCUSED SOURCE: its entry is `disarmTree`
	/// (`ArmEscape`), which clears flags through `arm(false)` and touches neither amounts nor focus.
	///
	/// CONSUMED ONLY IF IT DID SOMETHING. An Esc with nothing open is not this editor's key, and
	/// swallowing it would take it from whatever the host meant it for.
	bool escapeTop() { return surfaces != nullptr && surfaces->escapeStack().closeTop(); }

	/// THE POPOUT WINDOW ON THE SAME STACK (backlog NN) — it is an OS window of its own, but it is one more thing
	/// this editor opened. Called after each `expandedWindow->show`; its `onDismiss` takes the entry off.
	void popoutOpened() {
		if (surfaces == nullptr || expandedWindow == nullptr) {
			return;
		}
		popoutEscape = surfaces->escapeStack().push([this] {
			if (expandedWindow == nullptr || !expandedWindow->open()) {
				return false;
			}
			expandedWindow->dismiss();
			return true;
		});
	}

	/// THE SAME BRIDGE, SEEN AS THE DEVICE SEAM. Panels are converging onto `backplane::DeviceHost`
	/// so the RACK can open them too; `BridgeDeviceHost` is the adapter that lets Feathers keep
	/// handing them the parameter road underneath. `FxLane` already holds one for its cards — this is
	/// the same thing for the panels, which this file opens rather than the lane.
	///
	/// **AND FOR THE MODULATOR STRIP'S CARDS TOO — ONE HOST, BECAUSE A DEVICE IS ADDRESSED BY ITS ID**
	/// (kill-lanes step 2). A second host stood here for the strip's own slot space, because handing the
	/// strip this one made every modulator card read and write an unrelated device, silently, when both
	/// spaces were rows of floats. An id has one space, so there is nothing left to keep apart.
	BridgeDeviceHost deviceHost { bridge };

	/// THE EDITOR'S ONE UNDO HISTORY, declared here for the reason the readout below is: members are
	/// destroyed in reverse declaration order, so a history declared before every widget is destroyed
	/// AFTER them, and the surface that observes it is still alive to take itself back out. One
	/// history rather than one per surface — see edit_history.h; the clone on the cards is the verb
	/// that proved a per-widget stack was the wrong shape.

	/// HOW THIS EDITOR IS ARRANGED, from the last time anybody arranged it — see editor_prefs.h.
	/// Read once, in create(), and written the moment something changes. Nothing here is patch state
	/// and nothing here is ever read by the engine.
	EditorPrefs prefs;
	/// WRITES THE CHOSEN PRESET ROOT INTO `prefs` whichever browser chose it — the header's, or a device
	/// panel's, which has no prefs of its own (`preset_browser.h`). After `prefs`, so it goes first.
	std::unique_ptr<PresetRootWatch> presetRootWatch;
	/// WRITES THE TOOLTIP DETAIL INTO `prefs` whoever changes it (`tooltip_prefs.h`). After `prefs`.
	std::unique_ptr<TooltipDetailWatch> tooltipDetailWatch;
	/// WRITES THE CHROME HUE INTO `prefs` AND REPAINTS THIS EDITOR whoever moves it (`chrome_hue_prefs.h`,
	/// backlog LG). After `prefs`.
	std::unique_ptr<ChromeHueWatch> chromeHueWatch;
	/// The same for the ACCENT HUE, the second axis (`chrome_hue_prefs.h`). After `prefs`.
	std::unique_ptr<AccentHueWatch> accentHueWatch;
	/// WRITES THE WINDOW-CONTROLS SIDE INTO `prefs` whoever changes it (`window_controls_side_prefs.h`, backlog OD). After `prefs`.
	std::unique_ptr<WindowControlsSideWatch> windowControlsSideWatch;
	std::unique_ptr<EmittersFrontWatch> emittersFrontWatch; ///< arps sent to front of chain (`emitters_front.h`)
	std::unique_ptr<FullRedrawWatch> fullRedrawWatch; ///< repaint partial or every frame (`full_redraw.h`)

	/// THE CANVAS: the column outboard of the fx lane where an expanded view docks. A region of this
	/// layout, owned and placed here like a rail; the layers only route to it. DECLARED BEFORE THE
	/// WINDOW, because the window's constructor asks its Client for `logicalWidth()` and that reads
	/// the canvas's extent — MEASURED by ASan on the first Gui a case built: a read of the pointer
	/// before it was constructed. And before the layers, so it is destroyed after them: the layers
	/// unhook their dismissal road from it as they go.
	std::unique_ptr<ExpandedCanvas> canvas;
	/// THE STAGE'S COLLAPSE TOGGLE AND, CLOSED, ITS STRIP (backlog NF). DECLARED BEFORE THE WINDOW for the canvas's
	/// reason: `logicalWidth()` asks whether the stage is closed, and the window's constructor asks for that.
	std::unique_ptr<StageRail> stageRail;
	/// **THE SPECTRUM PANEL, WHILE OPEN** (spectrum_panel.h). Presented through `surfaces` as a device window is — or through the popout window when the
	/// preference asks — and OWNED BY ITS HOST: this is a handle, nulled by the host's dismissal road, whichever closed it
	/// (✕, Esc, the tab, leaving Additive). Declared BEFORE `surfaces` so it outlives the layers: their teardown can run the
	/// dismissal road, which writes only these three and the bridge's plot flag. Explore's tab caret is synced from it at the tick, never from the road.
	ui::SpectrumPanel* spectrumPanel = nullptr;
	SurfaceId spectrumSurface = kNoSurface;
	bool spectrumInWindow = false;
	void spectrumClosed() {
		spectrumPanel = nullptr;
		spectrumSurface = kNoSurface;
		spectrumInWindow = false;
		// AND THE ENGINE STOPS PUBLISHING ITS PLOT NOW: a host may retire the panel a frame later (the overlay reaps it on its
		// next tick), and the panel's destructor, which says the same, is then only the backstop.
		bridge.setPlotWanted(false);
	}
	/// THE TAB'S PRESS: open the spectrum panel, or close it when it is open (as a second press on a card closes its window).
	void toggleSpectrumPanel() {
		if (spectrumPanel != nullptr) {
			closeSpectrumPanel();
			return;
		}
		// ADDITIVE'S ALONE (design R9), at the open too — not only at the next tick's check — so a press while another engine is
		// playing (the tab is hidden then; a host or a case can still ask) opens nothing.
		if (surfaces == nullptr
		    || oscTypeFromValue(bridge.value(static_cast<clap_id>(ParamId::OscType))) != OscType::Additive) {
			return;
		}
		auto panel = std::make_unique<SpectrumPanel>(bridge, registry, selection, notify.get());
		// THE MODULATION STATE BEFORE IT IS SHOWN, as every device window is handed it (`openDeviceWindow`).
		panel->setModulation(&mod, &history);
		SpectrumPanel* raw = panel.get();
		// THE SAME TWO HOSTS AS A DEVICE WINDOW, CHOSEN THE SAME WAY: the in-editor overlay, or the popout window when the
		// preference asks for it.
		if (expandedWindow != nullptr && prefs.flag(kPopoutWindowKey, false)) {
			surfaces->dismissPresented();
			expandedWindow->show(std::move(panel));
			popoutOpened();
			spectrumPanel = raw;
			spectrumInWindow = true;
			return;
		}
		if (expandedWindow != nullptr) {
			expandedWindow->dismiss();
		}
		// ESC AND THE ✕ ARE THE HOST'S ROADS: its dismissal runs this while the panel is still alive (the overlay retires it a
		// tick later), so a gesture open on the plot is ended here, before the handle goes.
		const SurfaceId id = surfaces->present(std::move(panel), SurfaceNeed::SpansGesture, "Spectrum",
		                                       [this] {
			                                       if (spectrumPanel != nullptr) {
				                                       spectrumPanel->endGesture();
			                                       }
			                                       spectrumClosed();
		                                       });
		if (id != kNoSurface) {
			spectrumPanel = raw;
			spectrumSurface = id;
		}
	}
	/// CLOSE IT BY ITS HOST'S OWN ROAD, which runs `spectrumClosed` — the tab, leaving Additive, the editor closing. A gesture
	/// open on the plot is ended by that road (the overlay's dismissal callback, above) or, in the popout, by the panel's own
	/// destruction, which the window does at once.
	void closeSpectrumPanel() {
		if (spectrumInWindow && expandedWindow != nullptr) {
			expandedWindow->dismiss();
		}
		else if (surfaces != nullptr && spectrumSurface != kNoSurface) {
			surfaces->dismiss(spectrumSurface);
		}
		spectrumClosed(); // in case the host had already let it go
	}
	/// **THE DRUM KIT PANEL, WHILE OPEN** (drum proposal §6): the spectrum panel's arrangement exactly — presented through
	/// `surfaces` or the popout, OWNED BY ITS HOST, this a handle nulled by the host's dismissal road. Drum's alone.
	ui::DrumPanel* drumPanel = nullptr;
	SurfaceId drumSurface = kNoSurface;
	bool drumInWindow = false;
	void drumClosed() {
		drumPanel = nullptr;
		drumSurface = kNoSurface;
		drumInWindow = false;
	}
	void toggleDrumPanel() {
		if (drumPanel != nullptr) {
			closeDrumPanel();
			return;
		}
		if (surfaces == nullptr
		    || oscTypeFromValue(bridge.value(static_cast<clap_id>(ParamId::OscType))) != OscType::Drum) {
			return;
		}
		auto panel = std::make_unique<ui::DrumPanel>(bridge, registry, selection, notify.get());
		panel->setModulation(&mod, &history);
		ui::DrumPanel* raw = panel.get();
		if (expandedWindow != nullptr && prefs.flag(kPopoutWindowKey, false)) {
			surfaces->dismissPresented();
			expandedWindow->show(std::move(panel));
			popoutOpened();
			drumPanel = raw;
			drumInWindow = true;
			return;
		}
		if (expandedWindow != nullptr) {
			expandedWindow->dismiss();
		}
		const SurfaceId id = surfaces->present(std::move(panel), SurfaceNeed::SpansGesture, "Drum Kit", [this] { drumClosed(); });
		if (id != kNoSurface) {
			drumPanel = raw;
			drumSurface = id;
		}
	}
	void closeDrumPanel() {
		if (drumInWindow && expandedWindow != nullptr) {
			expandedWindow->dismiss();
		}
		else if (surfaces != nullptr && drumSurface != kNoSurface) {
			surfaces->dismiss(drumSurface);
		}
		drumClosed();
	}
	/// THE SURFACE HOST — the overlay and the takeover behind one seam, THE SAME ONE THE RACK HOLDS.
	/// Above every surface and below the notify tray, so a device's panel covers the editor and a
	/// notification still reaches the eye over it. Every expand in this editor goes through
	/// `surfaces->present`, and lands on the canvas or the overlay by the layers' one rule.
	std::unique_ptr<SurfaceLayers> surfaces;
	/// ASSIGN MODE ON THE LAYERS' ESC STACK (backlog NN) — after `surfaces`, so it comes off first.
	ArmEscape armEscape;
	/// THE SETTINGS SCREEN'S ENTRY on the same stack, while it shows.
	EscapeTicket settingsEscape;
	/// THE POPOUT WINDOW'S, while it is open (`popoutOpened`).
	EscapeTicket popoutEscape;

	/// THE WINDOW, AND EVERYTHING THAT GOES WRONG WITH ONE — see plugin/ui/plugin_window.h. It is
	/// declared after the members it calls back into, because `PluginWindow` takes `*this` as its
	/// Client and a Client must be alive before a window can ask it anything.
	PluginWindow window { *this };
	/// DECLARED BEFORE EVERY WIDGET, and the order is the point. Members are destroyed in reverse
	/// declaration order, so the readout outlives the header, Engine rail, tray and rows that all hold a
	/// raw Notifier* at it — even if someone reaches this struct without going through destroy()'s
	/// explicit sequence, or adds a widget there and forgets to add it here. destroy() still does it in
	/// order, because it must also run before the window goes; this makes the safe order the default
	/// rather than a thing to remember.
	std::unique_ptr<NotifyTray> notify;
	/// **"LOADED: <NAME>" — THE PRESET TOAST**. The tray's own widget at
	/// `NotifyTray::kToastTiming`, under the preset name in the header; deaf to the mouse and never
	/// focused. Its clock is `toastClock` when a case installs one.
	std::unique_ptr<NotifyTray> presetToast;
	/// THE AS-MODULATED READOUT OVER THE WHOLE EDITOR (backlog PU) — for the faces no card holds: the knob tray, the
	/// Engine rail, Explore. A card's or window's own layer (`FxCard`, `ExpandedView`) answers first when it holds the face.
	std::unique_ptr<ModReadoutLayer> modReadout;
	std::function<int64_t()> toastClock;
	/// WHILE AN UNDO OR REDO IS PUTTING A PATCH BACK, the load it makes is not itself a new step.
	bool replayingPreset = false;
	/// THE PATCH AS IT WAS just before the provider's load, waiting for the browser to say which entry
	/// that load was (`onSelected`) — the one place both the old bytes and the new name are known.
	struct PendingPresetLoad {
		PresetData before;
		std::string beforeName;
		std::string beforeCategory;
		PresetData after;
	};
	std::optional<PendingPresetLoad> pendingPresetLoad;
	/// The bridge's `presetIdentitySerial` the browser's face last agreed with (backlog QC): a host state load that
	/// moves it while the editor is open puts the face back on the name the patch carries, on the next tick.
	uint64_t seenPresetIdentity = 0;
	/// WHILE THE FACE IS PUT BACK FROM THE PATCH'S COPY: the browser's report of it is not a new name for the patch — a
	/// name this bank does not hold (another preset folder, a deleted file) must stay in the patch, not be erased by the miss.
	bool restoringPresetFace = false;

	/// PUT A WHOLE PATCH BACK (or forward again) for the history: the bytes through the same load a
	/// pick makes, the browser's face onto the entry it names (without loading it twice), and the toast.
	void replayPreset(const PresetData& bytes, const std::string& name, const std::string& category) {
		replayingPreset = true;
		const bool loaded = loadPresetBytes(bytes);
		replayingPreset = false;
		if (!loaded) {
			return;
		}
		if (presetBrowser != nullptr) {
			presetBrowser->markCurrent(presetBrowser->indexOf(name, category));
		}
		showPresetToast(name);
	}
	void showPresetToast(const std::string& name) {
		if (presetToast != nullptr && !name.empty()) {
			presetToast->say("Loaded: " + name);
		}
	}

	/// STEP THE GLOBAL UI SCALE (backlog EJ): apply it to the shell, persist it, and say the percentage.
	/// `direction` is `PluginWindow::uiScaleStepFor`'s: +1 up the ladder, -1 down, 0 back to 100 %.
	void stepUiScale(int direction) { setUiScale(PluginWindow::stepUiScale(window.uiScale(), direction)); }
	/// SET IT TO A RUNG — the settings screen's road, which offers every rung at once. The keys and the
	/// header's buttons step through here too, so there is one place a scale is applied and persisted.
	void setUiScale(double next) {
		window.setUiScale(next);
		prefs.setNumber(PluginWindow::kUiScaleKey, next);
		if (header != nullptr) {
			header->redraw(); // the readout shows the new percentage on the next frame
		}
		if (notify != nullptr) {
			notify->say("UI scale " + std::to_string(static_cast<int>(std::lround(next * 100.0))) + "%");
		}
	}
	/// SHOW THE SETTINGS SCREEN (backlog LG) over everything: re-added as the top frame's LAST child, so
	/// whatever was added since create (a panel, a prompt) is under it.
	void openSettings() {
		visage::ApplicationWindow* app = window.app();
		if (settings == nullptr || app == nullptr) {
			return;
		}
		if (settings->parent() != nullptr) {
			settings->parent()->removeChild(settings.get());
		}
		app->addChild(settings.get());
		settings->setBounds(0.0f, 0.0f, static_cast<float>(app->width()), static_cast<float>(app->height()));
		settings->setVisible(true);
		settings->redraw();
		// ON THE ESC STACK while it shows (backlog NN); opening it again while open brings it to the top.
		if (settingsEscape.held()) {
			settingsEscape.raise();
		}
		else if (surfaces != nullptr) {
			settingsEscape = surfaces->escapeStack().push([this] {
				if (settings == nullptr || !settings->isVisible()) {
					return false;
				}
				closeSettings();
				return true;
			});
		}
	}
	void closeSettings() {
		if (settings != nullptr) {
			settings->setVisible(false);
		}
		settingsEscape.release();
	}
	// THE PANELS THIS EDITOR PRESENTED ARE THE LAYERS' TO REMEMBER (several at once since 2026-09-14);
	// the one thing this editor does with them as a set — clearing them when the OS-window preference
	// takes the next open — asks the layers. The canvas and the layers themselves are declared above
	// the window; see there for why.
	std::unique_ptr<HeaderBar> header;
	/// THE SETTINGS SCREEN (backlog LG), opened from the header's Libra mark. Built once at create and kept;
	/// hidden rather than destroyed on close, because the close arrives from inside its own mouse handler.
	std::unique_ptr<SettingsScreen> settings;
	/// What the browser's entries mean (`pluginPresets()`). Declared BEFORE the browser so it outlives it.
	std::unique_ptr<FunctionPresetProvider> pluginPresets_;
	std::unique_ptr<PresetBrowser> presetBrowser;
	std::unique_ptr<EngineRail> engineRail;
	std::unique_ptr<ModPanel> modPanel;
	/// THE POST-FX LANE, against the right edge — the third rail, and the only one on that side. See
	/// fx_lane.h: the post-FX lane, and the card of its first tenant.
	std::unique_ptr<FxLane> fxLane;
	// The fx lane owns its cards; see Gui::create.
	std::unique_ptr<SoundStack> soundStack;
	std::unique_ptr<KnobTray> tray;
	std::unique_ptr<PerformStage> perform;
	/// THE TERRAIN OUTLIVES THE SURFACE THAT DRAWS IT, deliberately: it owns a worker thread, and
	/// declaring it before the stage means the stage is destroyed first and cannot be mid-read when the
	/// worker is joined. Declared here rather than owned by the stage for the same reason the notify
	/// readout is declared before its users — the safe order should be the default rather than a thing
	/// to remember.
	std::unique_ptr<TerrainMap> terrain;
	/// The warped ground, kept in front of the stage in the app's child list so it paints underneath it.
	std::unique_ptr<TerrainGround> ground;
	std::unique_ptr<ExploreStage> explore;
	std::unique_ptr<WaveDisplay> wave;
	/// THE MASTER METER, at the tray's far right (backlog DN). A readout on the tick, like the tray.
	std::unique_ptr<MasterMeter> meter;
	std::vector<std::unique_ptr<ParamRow>> rows;
	/// The slice's orientation, live, under FEATHERS_DEBUG_PANEL only — see orientation_row.h. Held
	/// beside the param rows because the panel lays them out as one grid.
	std::vector<std::unique_ptr<OrientationRow>> orientationRows;
	/// The audition's other handles, under the same switch and in the same strip — see tuning_row.h.
	std::vector<std::unique_ptr<TuningRow>> tuningRows;

	/// Who redraws when which parameter moves. Replaces the old positional lookup, where a row's index
	/// in the vector happened to be its parameter index — a coincidence that ends the moment two
	/// widgets watch one value, which the header's engine selector already does.
	/// Which sound the editor is talking about, and which surface is showing. Neither is a parameter —
	/// see ui_state.h — and both are held beside the registry because they are the same kind of thing:
	/// shared state with watchers.
	/// Who repaints when the PREVIEW moves. Its own set rather than the selection's, because they are
	/// different questions with different audiences: the selection is which sound is being edited (the
	/// rail, the chart), and this is which sound is audible (the rail, the blend circle, the scope).
	/// Neither is a parameter, so neither can go through the registry.

	/// THE NON-PARAMETER CHANNELS' REPAINT SIGNAL — travel, gamma, orientation, residue, hues,
	/// variations, preview, routes and the audition tuning. One list, because the signal is
	/// payload-free: everything on it re-derives what it draws from the bridge anyway, so telling it
	/// WHICH channel moved would buy nothing and cost a subscription matrix.
	///
	/// FINISHING A MECHANISM RATHER THAN INTRODUCING ONE. FrameWatchers was built for exactly this
	/// and had one user; the parameter half of the same job has been driven from tick() since it was
	/// written, and this is its other half arriving.

	/// THE SAME PANEL IN A WINDOW OF ITS OWN.
	/// Interchangeable with `expanded` above — same surface, same content, and the content cannot
	/// tell which one has it. See windowed_expanded.h.
	///
	/// **DECLARED AFTER `stateWatchers`, AND THAT IS THE WHOLE OF A BUG RATHER THAN A PREFERENCE.**
	/// Members are destroyed in REVERSE declaration order, and this one's destructor unregisters its
	/// content from that list. Declared before it, the list was already gone when the unregister ran
	/// — a use-after-free that presented as a SIGSEGV *after* the panel had opened successfully, so
	/// every trace through `show()` completed and the fault looked like it was somewhere else
	/// entirely.
	///
	/// The same idiom the terrain and the notify tray use here: put the member where the SAFE order
	/// is the default, rather than leaving it as a thing to remember.
	std::unique_ptr<WindowedExpanded> expandedWindow;

	uint32_t width = kDefaultWidth;
	uint32_t height = kDefaultHeight;

	/// THE ONE SOURCE OF SCALE TRUTH, and the order it is resolved in. A logical pixel is this many
	/// device pixels; everything the editor asks for or reports in native pixels goes through it.
	///
	///   1. THE HOST, through clap_plugin_gui.set_scale. It knows what the user actually set, so it
	///      wins whenever it speaks. Many hosts never do — Bitwig 5.1.5 has the call site but did not
	///      use it in any session we traced, its plugin host having been started not-dpi-aware.
	///   2. FEATHERS_UI_SCALE in the environment, for when the other two are wrong and somebody needs
	///      a way to say so without a rebuild.
	///   3. visage's own derivation, LAST because it is the least trustworthy of the three. It
	///      divides the mode's pixel width by a physical width the display reports and snaps the
	///      result to a multiple of 24: on this machine X reports 96 DPI and visage still lands on
	///      168, a scale of 1.75, for a display that is not scaled at all.
	///
	/// Before there is a window there is no source 3, so the fallback is 1.0 and the real value is
	/// resolved in setParent once visage has something to ask.
	double hostScale = 0.0; ///< 0 until the host says otherwise
	/// visage's derivation, captured the first time a window exists and never re-read. It has to be
	/// captured, because the way this editor applies a scale is by writing visage's own field — so
	/// reading that field back later returns our answer rather than its question, and "preferred"
	/// would collapse onto whatever was last applied.
	double visageScale = 0.0;




	/// The smallest window the editor accepts, and it does NOT depend on the scale. The layout needs
	/// kLogicalWidth x kLogicalHeight logical pixels; at a scale of 1 that is exactly this many
	/// device pixels, and a window this size is drawable on any display we care about. Making the
	/// floor scale-dependent instead was a mistake worth remembering: with the scale capped to fit
	/// the screen, "logical x scale" made the minimum window the entire display, so the editor could
	/// be made larger but never smaller.
	///
	/// WHAT MAKES A SCALE-FREE FLOOR SAFE is contentScaleFor, and nothing else: the floor is only the
	/// right number while the content scale is allowed to come down to meet it. The two are one
	/// mechanism — this says the window may be as small as the design canvas, and that says the
	/// content is drawn at whatever scale the window it actually has can hold.
	// The minimums are `PluginWindow::Client`'s below — they were already exactly this, which is
	// why the extraction did not have to invent them.





	static constexpr int kColumns = 3;

	/// What is left for a surface once the furniture has taken its share: the header off the top, the
	/// knob tray off the bottom, the Engine rail rail off the left. THE ONE PLACE that rectangle is
	/// worked out — the Perform surface sits in it, Explore will sit in it, and the debug rows fill it
	/// when they are built, and three copies of this arithmetic is three chances for them to disagree
	/// about where the stage is.
	/// How wide a strip the audition controls take out of the stage, or zero when there are none.
	///
	/// TAKEN OUT OF THE STAGE RATHER THAN LAID OVER IT. The debug rows used to be drawn on top of the
	/// Perform surface, which was fine while they were scaffolding and useless the moment the surface
	/// became the thing being auditioned  You cannot judge a constellation
	/// you cannot see, and you cannot drag a puck through a slider.
	float auditionStripWidth() const {
		if (orientationRows.empty() && tuningRows.empty()) {
			return 0.0f;
		}
		// Wide enough for a label and a value on one line, and no wider: every pixel here is one the
		// circle does not get.
		return std::min(240.0f, static_cast<float>(window.app()->width()) * 0.22f);
	}

	/// Where the rails end and the surface begins. The Engine rail's rail, then the modulator strip, then
	/// the sound stack's — the mock's own column order (`main{grid-template-columns: auto auto 206px
	/// minmax(0,1fr) auto}`), with the panels it does not have left out.
	///
	/// EVERY RAIL ANSWERS FOR ITS OWN WIDTH, and this adds them up. That is the whole of the column
	/// arithmetic, and it is deliberately the ONLY place it happens: a rail that closes gives its
	/// width to the stage here, stageBounds() reads it, and ExploreStage's flank columns are a
	/// function of the stage frame they are handed — so one number moves and three geometries follow
	/// with nothing to keep in step by hand.
	///
	/// THE AUTO-HIDE THAT USED TO LIVE HERE IS GONE, and the measurement is why. It asked whether the
	/// strip cost the disk anything and took the strip away when it did, which was a rule written for
	/// a canvas that no longer exists: it was reachable only because the content scale went unresolved
	/// until the editor was embedded, so the floor window laid out at 708x400 logical. With that fixed
	/// (see syncContentScale) the floor IS the design canvas, where three rails leave a 630x534 stage
	/// and the disk at its full 259 — measured — and both terms of the old test grow with the canvas,
	/// so no reachable window could ever fail it again. A rule that cannot fire is a layout state
	/// nobody can reach and a way for the panel to vanish if the arithmetic ever drifts.
	///
	/// What replaces it is the mock's own control and The
	/// strip is always in its column, open at kWidth or closed at Rail::kCollapsedWidth, and the only
	/// thing that moves it is a hand on its toggle. So a resize can no longer take a panel away, and
	/// "the user's choice survives a resize" needs no guard: nothing but the user ever writes it.
	float railsWidth() const {
		return (engineRail != nullptr ? engineRail->railWidth() : 0.0f)
		       + (modPanel != nullptr ? modPanel->railWidth() : 0.0f)
		       + (soundStack != nullptr ? SoundStack::kWidth : 0.0f);
	}

	/// THE STAGE NOW YIELDS ON BOTH SIDES. The left rails are a running edge from x = 0; this is the
	/// one column that grows inward from the right, so it is its own term rather than another
	/// addend in railsWidth() — anything that added it there would put the stage's LEFT edge under
	/// the lane.
	float fxLaneWidth() const { return fxLane != nullptr ? fxLane->railWidth() : 0.0f; }

	/// The stage closed to its strip (backlog NF).
	bool stageCollapsed() const { return stageRail != nullptr && stageRail->collapsed(); }

	/// **THE EDITOR'S WIDTH WITH THE STAGE CLOSED: EVERY OTHER COLUMN, AND THE STRIP** (backlog NF). Decided 2026-10-18,
	/// between giving the room to the lane and giving it back: cards are unit size and never stretch, and the
	/// synth just shrinks. So nothing widens to take the stage's room; the editor asks to be this wide instead, the
	/// way the canvas band asks for more height.
	float collapsedStageEditorWidth() const {
		return railsWidth() + Rail::kCollapsedWidth + auditionStripWidth() + fxLaneWidth();
	}

	/// **WHAT CLOSING THE STAGE GIVES BACK, OR OPENING IT TAKES: the stage's open width less the strip.** The open
	/// width is the surfaces' own — they keep their last open rectangle while closed (see `layout`) — and a stage that
	/// has never been laid out open (an editor that came up closed) is the one the design canvas would give it.
	float stageRoom() const {
		float open = explore != nullptr ? explore->width() : 0.0f;
		if (open <= 0.0f) {
			open = static_cast<float>(kDefaultWidth) - (collapsedStageEditorWidth() - Rail::kCollapsedWidth);
		}
		return std::max(0.0f, open - Rail::kCollapsedWidth);
	}

	/// A RAIL OPENED OR CLOSED. With the stage open, its room goes to or comes from the stage, the one column that
	/// flexes — so a layout is the whole of it. With the stage closed nothing flexes (backlog NF), so the editor
	/// itself is that much narrower or wider, and the host is asked for the difference.
	void railWidthChanged(const Rail& rail) {
		if (!stageCollapsed()) {
			layout();
			return;
		}
		const double room = rail.expandedWidth() - Rail::kCollapsedWidth;
		window.widthChangedBy(rail.collapsed() ? -room : room);
	}

	/// THE CANVAS'S BAND, under the tray. It is ADDED TO THE LOGICAL HEIGHT rather than taken from
	/// the stage (`logicalHeight()` below), so the stage keeps every pixel it has today and the
	/// canvas is room the host was asked for.
	float canvasExtent() const { return canvas != nullptr ? canvas->extent() : 0.0f; }

	/// THE CANVAS AS THE PATCH HAS IT, applied to the frame when they differ — the road a load takes.
	/// The frame's callbacks write the same value back, which the bridge ignores as no edit. An extent
	/// of zero is a patch that never set one: the frame keeps its default.
	void syncCanvasFromPatch() {
		// NOT WHILE A HAND IS ON THE PARTITION. The patch holds the extent from the last release; applying
		// it on every beat mid-drag snapped the partition back under the hand — the erratic, hard-to-shrink
		// drag that was hit. The release writes the patch, and the next beat finds nothing to move.
		if (canvas == nullptr || canvas->draggingExtent()) {
			return;
		}
		const rack::CanvasView view = bridge.canvasView();
		// AN EXTENT OF ZERO IS A PATCH THAT NEVER SET THE CANVAS: the frame keeps its default for both
		// the partition and the fold, rather than a zero-initialised "open" unfolding every fresh patch.
		if (view.extent <= 0.0f) {
			return;
		}
		bool changed = false;
		if (canvas->setOpenExtent(view.extent)) {
			changed = true;
		}
		if (view.collapsed != canvas->collapsed()) {
			canvas->setCollapsed(view.collapsed); // tells the host through onCollapsedChanged
			return;
		}
		if (changed) {
			window.preferredSizeChanged();
		}
	}
	/// WHAT THE FRAME SHOWS, INTO THE PATCH — one edit when it differs, none when it does not.
	void writeCanvasView() {
		if (canvas != nullptr) {
			bridge.setCanvasView({ canvas->openExtent(), canvas->collapsed() });
		}
	}

	visage::Bounds stageBounds() const {
		const float w = static_cast<float>(window.app()->width());
		const float h = static_cast<float>(window.app()->height());
		const float top = HeaderBar::kHeight;
		const float bottom = h - canvasExtent() - KnobTray::kHeight;
		// CLOSED, THE STAGE IS ITS STRIP (backlog NF) and the lane has the rest (`fxLaneWidth`).
		const float stageW = stageCollapsed() ? Rail::kCollapsedWidth
		                                      : w - railsWidth() - auditionStripWidth() - fxLaneWidth();
		return onSeparator({ railsWidth(), top, stageW, bottom - top });
	}

	/// **THE FOOTER'S SEPARATOR, IN NATIVE ROWS: its first row, and how many** (backlog LI). One logical
	/// pixel, snapped whole — the first row is where a frame whose top is `trayTop` would start, so the
	/// separator and the frames resting on it agree about the seam at every scale.
	static int separatorRow(float trayTop, float scale) { return static_cast<int>(std::round(trayTop * scale)); }
	static int separatorRows(float scale) { return std::max(1, static_cast<int>(std::round(scale))); }

	/// **A FRAME STANDING ON THE SEPARATOR ENDS ON ITS FIRST NATIVE ROW, NOT HALF A ROW INTO IT** (backlog
	/// LI). visage rounds a
	/// frame's native top and native height SEPARATELY (`Frame::setBounds`), so a frame from the header to
	/// `trayTop` can end at `round(top*s) + round(h*s)`, one row past `round(trayTop*s)` — MEASURED at UI
	/// scale 1.125, 1.375 and 1.5 — and children composite over the ground, so the stage's last row
	/// replaced the separator's first across its whole width: the terrain, under the circle. Here the
	/// logical rectangle is chosen so both of visage's roundings land on whole rows that end where the
	/// separator starts.
	visage::Bounds onSeparator(visage::Bounds b) const {
		const float scale = window.app()->dpiScale();
		if (!(scale > 0.0f)) {
			return b;
		}
		const float nativeTop = std::round(b.y() * scale);
		const float nativeFoot = static_cast<float>(separatorRow(b.bottom(), scale));
		return { b.x(), nativeTop / scale, b.width(), std::max(0.0f, nativeFoot - nativeTop) / scale };
	}

	//--------------------------------------------------------------------------------------------//
	// PluginWindow::Client — what the window asks of the surface inside it.                        //
	//--------------------------------------------------------------------------------------------//
	//
	// SIX ANSWERS AND NOT ONE OF THEM NEW. Each forwards to something this editor already had, which
	// is the property that made the window extractable at all: the shell was never asking the content
	// anything the content did not already know, it was simply reaching in to take it.

	/// THE DESIGN CANVAS PLUS THE EXPANDED-VIEW CANVAS. The second is room this editor asks the host
	/// for when it opens, and gives back when it collapses; a host that refuses still gets a correct
	/// editor, because `PluginWindow` scales the whole logical canvas to whatever it was given.
	/// AND NARROWER BY THE STAGE'S ROOM WHILE THE STAGE IS CLOSED (backlog NF) — the open size, the floor `adjustSize`
	/// holds a host to, and so a host that drags an edge can take the editor down to it.
	uint32_t logicalWidth() const override {
		return stageCollapsed() ? static_cast<uint32_t>(std::ceil(collapsedStageEditorWidth())) : kDefaultWidth;
	}
	uint32_t logicalHeight() const override {
		return kDefaultHeight + static_cast<uint32_t>(canvasExtent());
	}
	uint32_t minimumWidth() const override { return logicalWidth(); }
	uint32_t minimumHeight() const override { return logicalHeight(); }
	void requestResize(uint32_t w, uint32_t h) override { host.guiRequestResize(w, h); }

	/// THE HEARTBEAT'S PAYLOAD, which stays here because it is this editor's. `PluginWindow` owns the
	/// timer and knows only that it must call something; what a beat DOES — the parameter diff, the
	/// state channel, the accumulated report a case can ask about — is content.
	void tick() override {
		const Gui::TickReport report = this->tickOnce();
		driveReport.paramFrames += report.paramFrames;
		driveReport.stateFrames += report.stateFrames;
		driveReport.macroSlots += report.macroSlots;
		// AND THE PLUGIN'S OWN BEAT, after the editor's: the device the gesture just added is
		// provisioned from HERE on a platform whose host never calls back. See GuiHost::guiTick.
		host.guiTick();
	}

	/// THE HANDLERS THIS EDITOR HANGS ON ITS WINDOW, in one place because there are two windows to hang them on: the
	/// first build, and the one `PluginWindow` builds when the host destroyed ours (backlog GP, `windowRebuilt`).
	void installWindowHandlers(visage::ApplicationWindow& app) {
		// **ESC LEAVES MODULATION ASSIGN MODE, AND IT HAS TO BE THE ROOT THAT HEARS IT.**
		// Asked 2026-08-25 and again 2026-09-04 (Esc should exit modulation assign mode) —
		// asked a second
		// time about a feature that had been written, tested and shipped, because the handler was on a
		// frame the keystroke never visits.
		//
		// WHAT WAS WRONG, AND IT IS A ROUTING FACT RATHER THAN A LOGIC ONE.
		// `WindowEventHandler::handleKeyDown` begins at the keyboard-focused frame and walks up through
		// PARENTS, visiting only ancestors that `acceptsKeystrokes()`. A SIBLING IS NEVER VISITED. The
		// exit lived on `ExpandedOverlay`, which is a sibling of the mod panel and the Engine rail under
		// this root, so it could be reached only while the overlay itself held the focus — which it does
		// only while a panel is OPEN, and the fallback it guarded is by definition the closed case.
		//
		// AND IT WAS WORSE THAN UNREACHABLE: `handleMouseDown` sets the focus to the first
		// keystroke-accepting ANCESTOR of the clicked frame and to NOTHING when there is none. Nothing in
		// this editor's spine accepted keystrokes, so clicking a source's arrow — the gesture that ENTERS
		// the mode — left the focus null and `handleKeyDown` returned before visiting anybody. MEASURED:
		// the case below required a non-null focus after a real click on the real arrow and read
		// `nullptr != nullptr`. Esc was not going to the wrong frame; it was going nowhere at all.
		//
		// THE ROOT IS THE ONLY FRAME THAT CANNOT BE MISSED, because every frame in the editor is a
		// descendant of it, so whatever the hand last pressed the walk ends here. That is what makes it
		// the right home for a key about a MODE rather than about a widget: both doors into assign mode —
		// the strip's `SourceArm` and the Engine rail's `SourceGrid` — are under this root, and neither is
		// under any of the frames that already take keys.
		//
		// IT IS THE TERMINUS, NOT A COMPETITOR. The editor's other keystroke acceptors sit NEARER the hand
		// and are visited first: `FxLane` (Delete/Backspace on a focused device) and `ExpandedOverlay`
		// (Esc while a panel is open). Nearest-the-hand still wins; this catches only what they declined.
		// `TakeoverStack`, the third handler in the tree, is the RACK's surface and is not under this
		// root at all.
		//
		// A KEY NOBODY WANTS IS NOT SWALLOWED — `false` for any other key, and `false` for Esc with
		// nothing open, so a host's own Escape survives. See `Impl::escapeTop` (backlog NN).
		app.setAcceptsKeystrokes(true);
		app.onKeyPress() = [this](const visage::KeyEvent& e) {
			// **UNDO AND REDO ON THE EDITOR'S ONE HISTORY**: Ctrl+Z, and Ctrl+Shift+Z or
			// Ctrl+Y — Cmd on macOS, which is what `isMainModifier` means there. The same history the Explore
			// corners walk and a preset load records into, so Ctrl+Z after a preset puts the patch back.
			// **TAKEN ONLY WHEN WE ACT.** Nothing to undo, or a text entry editing, and the key goes back
			// unhandled — up to the host, so the DAW's own undo still works through a plugin with focus.
			if (const std::optional<bool> redo = Impl::undoChordOf(e); redo.has_value()) {
				return walkHistoryFromKey(*redo);
			}
			// THE UI SCALE, ON THE KEYS A BROWSER USES (backlog EJ): Ctrl/Cmd with = or + steps up, - steps
			// down, 0 resets. A global setting, so it is written to `EditorPrefs` as it changes and every
			// editor that opens afterwards — this plugin's or the rack's — comes up at it.
			if (e.isCtrlDown() || e.isCmdDown()) {
				const int direction = PluginWindow::uiScaleStepFor(static_cast<int>(e.keyCode()));
				if (direction != 2) {
					stepUiScale(direction);
					return true;
				}
			}
			if (e.keyCode() != visage::KeyCode::Escape) {
				return false;
			}
			return escapeTop();
		};
	
		// visage vetoes its own close unless someone says otherwise: ApplicationWindow::close() runs its
		// teardown only `if (window_ && onCloseRequested().callback())`, and an EMPTY CallbackList returns
		// a default-constructed bool — false. We never set this, so every app.close() we have ever made
		// has been a silent no-op, and the teardown below has been running in the opposite order to the
		// one its own comment describes.
		app.onCloseRequested() = [] { return true; };
		app.onDraw() = [this](visage::Canvas& canvas) { drawBackground(canvas); };
		// `+=`, NOT `=`. CallbackList::operator= is `set` — it CLEARS the list — and the editor's onResize
		// arrives carrying two callbacks visage installed itself: the Frame default that runs the virtual
		// resized() (frame.h's `on_resize_ { [this] { resized(); } }`) and the ApplicationEditor ctor's
		// re-derivation `top_level_->setNativeBounds(nativeLocalBounds())`. Assigning over them deleted
		// both, so a size that landed on the editor directly (ApplicationWindow::setNativeWindowDimensions,
		// the host-setSize road) laid THIS frame out and left top_level_'s native rectangle at the old
		// size until the platform event leg caught up — and on macOS, where backing scale and content
		// scale each move the native size, that window is where the explore terrain drew tiled at any
		// size or scale but the birth one. Appending keeps visage's own geometry dance and runs the
		// layout after it, on the re-derived rectangle.
		app.onResize() += [this] { layout(); };
	}

	void windowRebuilt(visage::ApplicationWindow& app) override { installWindowHandlers(app); }

	void layout() override {
		if (window.app() == nullptr) {
			return;
		}
		const float w = static_cast<float>(window.app()->width());
		const float h = static_cast<float>(window.app()->height());

		// The knob tray is the floor the rest stands on: it takes its height off the bottom first, and
		// everything else is laid out in what is left.
		// AND THE CANVAS TAKES ITS HEIGHT OFF THE BOTTOM BEFORE THE TRAY DOES.
		const float trayTop = h - canvasExtent() - KnobTray::kHeight;

		if (header != nullptr) {
			header->setBounds(0.0f, 0.0f, w, HeaderBar::kHeight);
		}
		// The preset browser sits in the header, to the right of the FEATHERS wordmark and byline. It is
		// a sibling of the header (not a child of it) — the header forwards mouse events to it. Its
		// vertical center matches the header's; its left edge is just past the wordmark's tracked width.
		if (presetBrowser != nullptr) {
			// FROM THE BROWSER'S OWN CONSTANTS, both of them. The header forwards clicks by subtracting
			// the same two numbers, so a layout that placed the frame from a copy of them is a layout
			// that can drift out from under its own hit tests — which it had.
			const float browserW = PresetBrowser::barWidthFor(PresetBrowser::kHeaderStyle); // the variant chips with it
			presetBrowser->setBounds(PresetBrowser::kLeftOffset, PresetBrowser::kTopOffset, browserW,
			                         PresetBrowser::kHeight);
			// THE TOAST HANGS JUST UNDER THE HEADER, beneath the name it reports — where the eye already is
			// after a step of the arrows. Wide enough for a long name; its box sizes itself to the text.
			if (presetToast != nullptr) {
				constexpr float kToastRoom = 420.0f;
				constexpr float kToastDrop = 6.0f;
				presetToast->setBounds(PresetBrowser::kLeftOffset, HeaderBar::kHeight + kToastDrop,
				                       std::min(kToastRoom, w - PresetBrowser::kLeftOffset), NotifyTray::kHeight);
			}
		}
		// The footer, left to right: the wave scope, then the knob row in what is left. The mock's
		// `footer{padding:12px 20px 16px; gap:20px}` and `.knobs{flex:1}` — the tray's own kPadX IS
		// that gap, so the row starts one padding after the scope ends and needs no extra inset.
		const float waveRight = KnobTray::kPadX + WaveDisplay::kWidth;
		if (wave != nullptr) {
			wave->setBounds(KnobTray::kPadX, trayTop + (KnobTray::kHeight - WaveDisplay::kHeight) * 0.5f,
			                WaveDisplay::kWidth, WaveDisplay::kHeight);
		}
		if (tray != nullptr) {
			// Starting after the scope is also what stops the notify readout covering the Path knob's
			// value: the readout belongs over the scope's empty lower left, which is where the mock puts
			// it, and it could not go there while a knob was underneath.
			const float trayLeft = wave != nullptr ? waveRight : 0.0f;
			// THE MASTER METER TAKES THE FAR RIGHT (backlog DN), the tray's height, one tray padding in
			// from the edge; the knob row shares the rest. The tray's own kPadX is the gap between them.
			const float meterW = meter != nullptr ? MasterMeter::kWidth + KnobTray::kPadX : 0.0f;
			tray->setBounds(trayLeft, trayTop, std::max(0.0f, w - trayLeft - meterW), KnobTray::kHeight);
		}
		if (meter != nullptr) {
			meter->setBounds(w - KnobTray::kPadX - MasterMeter::kWidth, trayTop, MasterMeter::kWidth,
			                 KnobTray::kHeight);
		}
		if (surfaces != nullptr) {
			// THE WHOLE EDITOR, because a panel floats over the whole editor. It is the one frame here
			// whose rectangle is not a region of the layout: a layer that covered less than everything
			// would leave a corner of the editor a panel could not be dragged into.
			surfaces->setBounds(0.0f, 0.0f, w, h);
		}
		if (canvas != nullptr) {
			// THE BAND ALONG THE BOTTOM, under the knob tray. The tray keeps its height and sits on top of it.
			canvas->setBounds(0.0f, h - canvasExtent(), w, canvasExtent());
		}
		if (modReadout != nullptr) {
			modReadout->setBounds(0.0f, 0.0f, w, h); // the whole editor: the box is placed inside it
		}
		if (notify != nullptr) {
			// Bottom left of the INSTRUMENT, over whatever is there — the mock's `position:absolute;
			// left:18px; bottom:12px`, measured from the tray's foot rather than the window's so the
			// canvas underneath does not get a readout about a knob drawn over it. Wide enough for a long
			// reading and no wider; the box inside sizes itself to the text.
			constexpr float kNotifyRoom = 420.0f;
			notify->setBounds(NotifyTray::kInsetX,
			                  h - canvasExtent() - NotifyTray::kInsetY - NotifyTray::kHeight,
			                  std::min(kNotifyRoom, w - 2.0f * NotifyTray::kInsetX), NotifyTray::kHeight);
		}
		// THE SETTINGS SCREEN covers the whole editor (its scrim), open or not, so a scale step taken from
		// it re-lays it with everything else.
		if (settings != nullptr) {
			settings->setBounds(0.0f, 0.0f, w, h);
		}
		// EVERY RAIL AT WHICHEVER WIDTH IT IS CURRENTLY WEARING, and each one's column starts where the
		// last one ended. Written as a running edge rather than as three sums of constants, so a rail
		// that opens or closes moves everything outboard of it and nothing else.
		float railEdge = 0.0f;
		if (engineRail != nullptr) {
			engineRail->setBounds(onSeparator({ railEdge, HeaderBar::kHeight, engineRail->railWidth(),
			                                   trayTop - HeaderBar::kHeight }));
			railEdge += engineRail->railWidth();
		}
		// ALWAYS IN ITS COLUMN, at whichever of its two widths it is currently wearing. There is no
		// setVisible here any more and that absence is the feature: the strip cannot leave, so there is
		// no state where a control is gone with nothing on screen to say where it went.
		if (modPanel != nullptr) {
			modPanel->setBounds(onSeparator({ railEdge, HeaderBar::kHeight, modPanel->railWidth(),
			                                  trayTop - HeaderBar::kHeight }));
			railEdge += modPanel->railWidth();
		}
		if (soundStack != nullptr) {
			soundStack->setBounds(onSeparator({ railEdge, HeaderBar::kHeight, SoundStack::kWidth,
			                                    trayTop - HeaderBar::kHeight }));
		}
		// THE LANE COMES IN FROM THE OTHER END, which is why it is placed from the frame's right edge
		// rather than from the running left one. Same two heights as the other rails, so the three of
		// them are one band across the editor with the stage in the middle of it.
		if (fxLane != nullptr) {
			fxLane->setBounds(onSeparator({ w - fxLaneWidth(), HeaderBar::kHeight, fxLaneWidth(),
			                                trayTop - HeaderBar::kHeight }));
		}
		const visage::Bounds stage = stageBounds();
		// THE STAGE'S TOGGLE (backlog NF): open, the corner of the stage a left rail keeps its toggle in; closed,
		// the strip itself.
		if (stageRail != nullptr) {
			stageRail->setBounds(stageCollapsed() ? stage
			                                      : visage::Bounds { stage.right() - StageRail::kOpenWidth, stage.y(),
			                                                         StageRail::kOpenWidth, StageRail::kOpenHeight });
		}
		// BOTH SURFACES GET THE SAME RECTANGLE, and only one of them is visible. They are two views of
		// one instrument in one aperture, so a surface laid out anywhere else would move the circle when
		// the mode changed.
		//
		// NOT WHILE THE STAGE IS CLOSED (backlog NF): they are hidden, and a surface squeezed into a 34px strip
		// would ask the terrain for a map of that shape — work for nobody. They keep the last open rectangle,
		// and the layout that opens the stage sets it again, at whatever scale is in force by then (`setBounds`
		// converts the native rectangle even when the logical one is unchanged — CLAUDE.md's scale rule).
		if (!stageCollapsed()) {
			if (perform != nullptr) {
				perform->setBounds(stage);
			}
			if (explore != nullptr) {
				explore->setBounds(stage);
			}
			// THE SAME RECTANGLE, so the two frames share coordinates and the ground needs no geometry of
			// its own — everything it paints is drawn in the stage's numbers.
			if (ground != nullptr) {
				ground->setBounds(stage);
			}
		}
		// THE AUDITION STRIP, in the column stageBounds() has already reserved for it — beside the
		// stage, never on top of it. Only what an audition actually needs; the full parameter list is a
		// separate view (below) because seventy rows is a smoke test, not an instrument.
		if (!orientationRows.empty() || !tuningRows.empty()) {
			const float stripX = stage.right();
			const float stripW = auditionStripWidth();
			// Sized to the number of lines actually being drawn, plus the two gaps between the three
			// families, so a strip that grew a family does not run off the bottom of the stage.
			const size_t lines = orientationRows.size() + tuningRows.size() + 1;
			const float rowH =
			    std::min(46.0f, stage.height() / static_cast<float>(std::max<size_t>(lines + 1, 12)));
			const float padX = stripW * 0.06f;
			// One running index over both kinds, so a handle added to either list finds its own line
			// instead of landing on top of somebody else's.
			size_t line = 0;
			const auto place = [&](visage::Frame& frame) {
				frame.setBounds(stripX + padX, stage.y() + 12.0f + static_cast<float>(line) * rowH,
				                stripW - 2.0f * padX, rowH - 8.0f);
				++line;
			};
			for (const auto& row : orientationRows) {
				place(*row);
			}
			// A gap between the two families, because they answer different questions and a wall of
			// identical rows reads as one control with nine handles.
			line += 1;
			for (const auto& row : tuningRows) {
				place(*row);
			}
		}

		// EVERY parameter as a bare row, over the stage, only when asked for by name. It is the
		// cheapest param-surface smoke test there is and it is unusable as an instrument, which is why
		// it is now its own view rather than the price of having any debug control at all.
		if (rows.empty()) {
			return;
		}
		const int perColumn = (static_cast<int>(rows.size()) + kColumns - 1) / kColumns;
		const float cellW = stage.width() / static_cast<float>(kColumns);
		const float cellH = stage.height() / static_cast<float>(perColumn);
		const float padX = cellW * 0.05f;
		const float padY = cellH * 0.12f;

		for (size_t i = 0; i < rows.size(); ++i) {
			const int column = static_cast<int>(i) / perColumn;
			const int row = static_cast<int>(i) % perColumn;
			rows[i]->setBounds(stage.x() + static_cast<float>(column) * cellW + padX,
			                   stage.y() + static_cast<float>(row) * cellH + padY,
			                   cellW - 2.0f * padX, cellH - 2.0f * padY);
		}
	}

	/// Whether the window has ever been shown. The first set_parent builds it; every later one moves
	/// the window we already have.

	/// The editor's own X window, or 0 before it exists. visage hands out the native handle, which on
	/// this backend is the window id. macOS has no X11 handle — the Cocoa embed goes through
	/// setParent's show(void* NSView) instead of a reparent — so this is a no-op there and is only
	/// ever called from the X11-guarded reparentTo/detachFromParent below.
#if defined(__linux__)
#else
	void* nativeWindow() const { return nullptr; }
#endif



	/// Once a frame, on the main thread: take the engine's latest snapshot, redraw whatever was
	/// watching what moved, and nudge the host for a flush if the editor has posted anything nobody
	/// has collected.
	Gui::TickReport tickOnce() {
		Gui::TickReport report;
		// THE ONE PLACE THAT CAN PROMISE NOTHING IS WALKING THE TREE. `processEvents` calls this
		// BEFORE `processPluginFdEvents` drains a single X event, so no dispatch is in flight and no
		// draw is underway — which is exactly what freeing a retired card requires and what the lane
		// could not check for itself. See `FxLane::releaseRetiredCards`.
		if (fxLane != nullptr) {
			fxLane->releaseRetiredCards();
			// AND A CARD HELD AT THE LANE'S EDGE SCROLLS IT (backlog DB) — this tick is the only clock a
			// still pointer has.
			(void)fxLane->tickDrag();
		}
		// **AND THE MODULATOR STRIP'S, WHICH IS THE SECOND LANE THAT RETIRES CARDS.** It freed them
		// inside its own `ensureCard` until the cache key learned about lanes; a reorder now rebuilds
		// there too, so it needs the same promise from the same place. A rule fixed in one file is
		// not a rule followed in the others — `FxLane::ensureCard` says exactly that about the bug
		// this is.
		if (modPanel != nullptr) {
			modPanel->releaseRetiredCards();
		}
		for (const uint32_t index : bridge.refresh()) {
			report.paramFrames += registry.notify(index);
		}
		// THE SAME JOB FOR THE STATE THAT IS NOT A PARAMETER, and BATCHED HERE rather than fired from
		// the setters — which is the whole design and not a convenience.
		//
		// A synchronous notification would arrive MID-WRITE. `ExploreStage::nudgeSelectedGamma` writes
		// the base and THEN advances `centredOnGamma_` and `scrubbedGamma_`, because the hand moved
		// rather than the world; a handler run from inside `setSoundGamma` would see `gammaSum()`
		// already moved and neither compensation applied, and would compute a view that exists at no
		// point in the gesture. That is harmless today only because notify() does nothing but mark
		// frames stale — which is a trap armed for the first handler that does real work, and the
		// kind that is found by a bug report rather than by a test.
		//
		// (It named `scrubTo` until backlog GY retired that verb with the axis it served. The hazard
		// is unchanged — it belongs to writing state and compensating for it in two steps, not to the
		// particular caller — but a comment naming a function that no longer exists is a claim a
		// reader cannot check, so it names the one that does.)
		//
		// Once a frame also coalesces: a drag moves several of these channels tens of times a second
		// and the screen is redrawn once regardless.
		if (bridge.takeRepaintDirty()) {
			// **A LOAD REPLACES THE CHAINS UNDER BOTH LANES, AND A REDRAW DOES NOT BUILD A CARD.**
			// 2026-09-26: after loading a preset, effects did not populate into view until the fx pane
			// was opened and closed — the preset browser's load and the host's state load both end in
			// `ParamBridge::restoreChains`, which raises this flag and nothing else, and the notify below
			// only repaints what is watched. Each lane compares its cards to its chain and lays itself out
			// when they differ, BEFORE the notify, so the cards it builds are drawn on this frame.
			if (fxLane != nullptr) {
				(void)fxLane->syncCards();
			}
			if (modPanel != nullptr) {
				(void)modPanel->syncCards();
			}
			report.stateFrames += stateWatchers.notify();
			// **AND A MACRO WHOSE TYPE MOVED UNDER ITS SLOT IS RE-SEATED** — a patch restore sets this
			// flag and tells nobody which type moved, and a redraw alone would repaint a knob that should
			// now be a chip. Here, batched, for the reason above: never from inside the restore.
			if (modPanel != nullptr) {
				report.macroSlots += modPanel->syncMacroSlots();
			}
		}
		// **AND A HOST'S STATE LOAD THAT RENAMED THE PATCH UNDER AN OPEN EDITOR** (backlog QC): the face follows the name
		// the patch now carries. A browser change moves the serial too, but `onCurrentChanged` records it as seen.
		if (presetBrowser != nullptr && bridge.presetIdentitySerial() != seenPresetIdentity) {
			seenPresetIdentity = bridge.presetIdentitySerial();
			const PresetIdentity& id = bridge.presetIdentity();
			restoringPresetFace = true;
			if (id.empty()) {
				presetBrowser->markCurrent(-1);
			}
			else {
				(void)presetBrowser->restoreCurrent(id.name, id.category);
			}
			restoringPresetFace = false;
		}
		// AND THE DEVICES' OWN LIVE READINGS, which do NOT go through the flag above.
		//
		// A meter arrives every audio block. The state channel redraws the whole subtree of every
		// watcher — PerformStage, ExploreStage, FxLane, SoundStack, WaveDisplay and any open panel,
		// measured at 108 frames for one meter update — and that is right for the gestures and patch
		// loads it was built for and wrong at block rate. `publishModulated` already refuses the
		// flag for exactly this reason. So each device surface is asked for itself, tells only what
		// it draws, and asks for nothing when its reading has not moved.
		//
		// THE CARD AND THE PANEL ARE BOTH ASKED, and they are separate widgets over the same slot:
		// an open OWLTT panel builds its own graph, so a road that updated only the lane would leave
		// the panel's meters dead while the card's moved.
		if (fxLane != nullptr) {
			report.deviceFrames += fxLane->updateCards();
		}
		// AND THE MOD RACK'S CARDS, which had no such road until a playhead needed one. Same contract,
		// same count, same reason it is here rather than on the state channel: a modulator publishes
		// every block, and the flag's blast radius is the whole watched tree.
		if (modPanel != nullptr) {
			report.deviceFrames += modPanel->updateCards();
			// AND THE MACRO ROW'S TOOLTIP DELAY, uncounted: it is not a device (the Engine rail's, below).
			(void)modPanel->updateTooltips();
		}
		// AND THE ENGINE RAIL'S TOOLTIP DELAY (voice stacking step 5), uncounted: it is not a device.
		if (engineRail != nullptr) {
			(void)engineRail->updateTooltips();
			// AND THE ROUTE LIST'S HEIGHT (backlog LV), uncounted for the same reason: a list laid out before its routes
			// were made kept one row's height and clipped the rest.
			(void)engineRail->updateLayout();
		}
		// AND THE AS-MODULATED READOUT (backlog PU), uncounted: a box, not a device.
		if (modReadout != nullptr) {
			(void)modReadout->advance();
		}
		if (surfaces != nullptr && surfaces->updateContent()) {
			++report.deviceFrames;
		}
		// THE EMPTY CANVAS'S SKY, uncounted: it is not a device, and the meter cases count devices.
		if (canvas != nullptr) {
			(void)canvas->animateBackdrop();
		}
		// AND THE CANVAS FOLLOWS THE PATCH, so a load moves the partition and the fold as saved.
		syncCanvasFromPatch();
		if (expandedWindow != nullptr && expandedWindow->updateContent()) {
			++report.deviceFrames;
		}
		// A ROUTE THE RULES REFUSED ON LOAD, SAID OUT LOUD. Drained here rather than fired from the
		// loader because a patch can be loaded with no editor open, and because this is the frame on
		// which an editor first exists to say anything at all.
		//
		// WORDED HERE AND COUNTED THERE, which is the split that keeps the serialization layer free of
		// the editor's vocabulary: the loader reports INDICES, and the only place that knows a source
		// is called "Sound A weight" is the panel that draws that name.
		//
		// **AND SINCE BACKLOG JF IT IS THE WHOLE LOAD REPORT** (`rack::LoadReport`): unknown devices, values a
		// newer build wrote, new controls at their defaults — one line, the most serious in full and the rest
		// counted (`rack::loadReportLine`). The refused routes are still named here, in this editor's words.
		if (notify != nullptr) {
			const rack::LoadReport report = bridge.takeLoadReport();
			const RouteRefusals& refused = report.routes;
			std::string message;
			if (refused.count > 0) {
				// **THE REFUSAL NAMES ENDPOINTS (kill-lanes step 3b), SO THERE IS NOTHING TO TRANSLATE HERE.**
				// `rack::RouteRefusals` reported COORDINATES and this file turned the source's half back into
				// a device by indexing the published state at that lane — which stopped being an address the
				// moment a chain became its ORDER. The loader hands over the `Source` and the `Destination`
				// the patch named, and both go to the two naming functions every other surface asks, so the
				// tray, the route list and the Engine rail cannot disagree about what a route was.
				//
				// AN END THAT NAMED A DEVICE THIS PATCH NO LONGER HAS is the record not holding its id — the
				// same question the old "one past the axis" sentinel asked, in the vocabulary that replaced
				// it. BOTH ends are tested, because either can be the missing one, and naming it honestly
				// beats printing whatever sits at index zero or a "?".
				//
				// **AND IT IS ANSWERABLE ONLY BECAUSE THE ENDS ARE WHAT THE FILE ASKED FOR, NOT WHAT THE
				// LOOKUP FOUND** (`rack_state.h`'s banner, decided 2026-09-12): a route that named a device
				// this patch no longer has arrives NAMING THAT DEVICE, so the record can be asked about it.
				// A coordinate threw the identity away at the loader, which is why this could only ever
				// report that something was missing and never which thing.
				//
				// THE ONE END THAT IS NOT A DEVICE AND NAMES NOTHING is `nobody()` — an end the file wrote
				// that cannot be spelled as an endpoint at all (a host kind the codec refuses, a device
				// written under `kNoInstance`). It is a host index past any host's own table, so it is not
				// a real source or parameter and must not be handed to the naming functions.
				//
				// A HOST END CANNOT OTHERWISE GO MISSING — Feathers' macros, sound weights and parameters
				// are this build's own table, fixed when it was compiled — and `modDestinationName`
				// already answers "Route" for an index it does not recognise.
				constexpr const char* kGone = "a device that is no longer here";
				const bool sourceGone = refused.firstSource == backplane::Source::nobody()
				                        || (refused.firstSource.isDevice()
				                            && bridge.descriptorOf(refused.firstSource.device) == nullptr);
				const bool destinationGone = refused.firstDestination == backplane::Destination::nobody()
				                             || (refused.firstDestination.isDevice()
				                                 && bridge.descriptorOf(refused.firstDestination.device) == nullptr);
				if (sourceGone && destinationGone) {
					message += kGone;
				}
				else {
					message += sourceGone ? std::string(kGone) : ui::modSourceName(bridge, refused.firstSource);
					message += " -> ";
					message += destinationGone ? std::string(kGone)
					                           : ui::modDestinationName(bridge, refused.firstDestination);
				}
			}
			if (const std::string line = rack::loadReportLine(report, message); !line.empty()) {
				notify->say(line);
			}
			// A HOST SLOT A HAND'S TOUCH RECYCLED (host slots T9, design §2): the last resort is said. The bridge
			// holds it until no hand is down, so the knob's own readout cannot write over it.
			if (const std::string slot = bridge.takeSlotNotice(); !slot.empty()) {
				notify->say(slot);
			}
		}
		// Batched to once a frame rather than once per event: a drag posts tens of values a second and
		// the host only needs telling that there is something waiting.
		if (bridge.hasPendingWork()) {
			host.guiRequestFlush();
		}
		// The two things that move on a clock rather than on an event, so they are the two that need a
		// frame to notice time has passed. Both decide for themselves whether the screen is out of
		// date: the readout only while it is fading, the stage every frame it is animating and never
		// under reduced motion.
		if (notify != nullptr) {
			notify->update();
		}
		if (presetToast != nullptr) {
			presetToast->update();
		}
		// The tray's master-scale knob follows the published engine (V17 + the V12c honesty idiom):
		// its enablement is patch state, so it needs a frame to notice the engine moved — same
		// contract as the stages' updates, and guarded the same way.
		if (tray != nullptr) {
			tray->update();
		}
		// THE MASTER METER READS WHAT THE LAST PROCESS PUBLISHED (backlog DN) — a frame only if it moved.
		if (meter != nullptr) {
			(void)meter->update();
		}
		// A DRUM KIT'S CARDS PULSE WHEN THEIR PADS ARE STRUCK, from the published hits.
		if (soundStack != nullptr) {
			(void)soundStack->updatePulses(static_cast<int64_t>(visage::time::milliseconds()));
		}
		// Reconciled here rather than through a callback out of the header: the switch is shared state
		// with watchers, so the one thing that has to happen when it moves is a check, not a signal.
		if (surfaceStale()) {
			showSurface();
		}
		if (perform != nullptr && perform->isVisible()) {
			perform->update();
		}
		if (explore != nullptr && explore->isVisible()) {
			explore->update();
		}
		// THE SPECTRUM PANEL IS ADDITIVE'S ALONE (design R9): leaving the engine closes it, by its host's road — the content
		// never closes itself. And the tab's caret follows whether it is open, whichever road closed it.
		if (spectrumPanel != nullptr
		    && oscTypeFromValue(bridge.value(static_cast<clap_id>(ParamId::OscType))) != OscType::Additive) {
			closeSpectrumPanel();
		}
		// THE DRUM KIT PANEL IS DRUM'S ALONE: leaving the engine closes it by its host's road, and the tab follows.
		if (drumPanel != nullptr
		    && oscTypeFromValue(bridge.value(static_cast<clap_id>(ParamId::OscType))) != OscType::Drum) {
			closeDrumPanel();
		}
		if (explore != nullptr) {
			explore->setDrumOpen(drumPanel != nullptr);
		}
		if (explore != nullptr) {
			explore->setSpectrumOpen(spectrumPanel != nullptr);
		}
		// THE BYPASS, POLLED — ONE READ OF THE LIVE MODIFIERS PER FRAME, feeding every armed glow at
		// once. It was: the only road a held Alt had to the draw path was the stamp
		// on the next mouse event, so the pointer had to move over a knob before any halo went out,
		// and only that knob's own event carried it. This asks the platform directly —
		// `Window::modifierState()`, XQueryPointer on X11, GetKeyState on Windows, [NSEvent
		// modifierFlags] on macOS (our fork patch, plan §5.5) — so Alt going down with the pointer
		// still, or parked off every knob, is seen on the next frame and every watcher is told in
		// the same notify. The knob's motion feed still exists and is now a redundant fast path.
		//
		// COUNTED BY THE NOTIFICATION, like the two roads above: a repaint is unobservable headlessly,
		// so the line that delivers one reports how many frames it told. The probe is the test seam —
		// a headless Gui has no window to ask — and with neither, the flag is left alone rather than
		// forced false, so a case that fed it by motion is not silently un-fed by a tick.
		//
		// AND ONLY WHERE THE PLATFORM CAN ANSWER (`Window::hasModifierState`). A backend without a live read returns
		// zero, which reads exactly like "Alt is up": polled anyway, it cleared the bypass every frame, so the motion
		// feed set it on each move and the next frame took it away. The glow stayed lit while Alt held and the press
		// (which reads its own event) still bypassed. That was macOS after the visage v1 port dropped the override.
		// Where the platform cannot answer, the event feed is the only witness and the poll stays out of its way.
		{
			int modifiers = -1; // negative: nothing read
			if (modifierProbe != nullptr) {
				modifiers = modifierProbe();
			}
			else if (window.app() != nullptr && window.app()->window() != nullptr
			         && window.app()->window()->hasModifierState()) {
				modifiers = window.app()->window()->modifierState();
			}
			if (modifiers >= 0) {
				report.bypassFrames += mod.setBypass((modifiers & visage::kModifierAlt) != 0);
			}
		}
		return report;
	}

	/// Show whichever surface the header's switch names, and only that one. A hidden surface costs
	/// nothing: it is not drawn, it is not ticked, and the terrain's worker sleeps because nothing asks
	/// it for a window.
	///
	/// AND NEITHER WHILE THE STAGE IS CLOSED (backlog NF) — the same "hidden costs nothing" bought for both,
	/// which is the whole of the claim that a closed Explore has no hidden cost. The strip names the surface
	/// it would open onto.
	void showSurface() {
		const bool exploring = mode.exploring();
		const bool open = !stageCollapsed();
		if (perform != nullptr) {
			perform->setVisible(open && !exploring);
		}
		if (explore != nullptr) {
			explore->setVisible(open && exploring);
		}
		if (ground != nullptr) {
			ground->setVisible(open && exploring);
		}
		if (stageRail != nullptr) {
			stageRail->setRailLabel(exploring ? StageRail::kExploreLabel : StageRail::kPerformLabel);
		}
		if (window.app() != nullptr) {
			window.app()->redrawAll();
		}
	}

	/// Whether the surfaces' visibility is not what `showSurface` would make it.
	bool surfaceStale() const {
		const bool open = !stageCollapsed();
		return (perform != nullptr && perform->isVisible() != (open && !mode.exploring()))
		       || (explore != nullptr && explore->isVisible() != (open && mode.exploring()));
	}

	//----------------------------------------------//
	// Preset capture / restore, to an in-memory buffer
	//----------------------------------------------//

	/// A clap_ostream over a growing byte vector — what capturePreset writes to. The bytes outlive
	/// the call: the resulting PresetData owns them.
	struct MemOStream {
		std::vector<uint8_t>* out = nullptr;
		static int64_t CLAP_ABI write(const clap_ostream_t* self, const void* buffer, uint64_t size) {
			auto* impl = static_cast<MemOStream*>(self->ctx);
			if (impl == nullptr || impl->out == nullptr || size == 0) {
				return -1;
			}
			const auto* bytes = static_cast<const uint8_t*>(buffer);
			impl->out->insert(impl->out->end(), bytes, bytes + size);
			return static_cast<int64_t>(size);
		}
	};

	/// A clap_istream over a read-only byte span — what loadPreset reads from.
	struct MemIStream {
		std::span<const uint8_t> in;
		size_t pos = 0;
		static int64_t CLAP_ABI read(const clap_istream_t* self, void* buffer, uint64_t size) {
			auto* impl = static_cast<MemIStream*>(self->ctx);
			if (impl == nullptr || impl->pos >= impl->in.size()) {
				return 0; // end of stream
			}
			const size_t available = impl->in.size() - impl->pos;
			const size_t toCopy = static_cast<size_t>(std::min<uint64_t>(size, available));
			std::memcpy(buffer, impl->in.data() + impl->pos, toCopy);
			impl->pos += toCopy;
			return static_cast<int64_t>(toCopy);
		}
	};

	/// Capture the full current plugin state into a PresetData. The same walk the CLAP stateSave
	/// runs — parameters, then every tagged block in order — but to a memory buffer.
	PresetData captureCurrent() const {
		PresetData result;
		MemOStream writer{};
		writer.out = &result.bytes;
		clap_ostream_t stream{};
		stream.ctx = &writer;
		stream.write = &MemOStream::write;
		host.guiCaptureState(&stream);
		return result;
	}

	/// **THE WHOLE PLUGIN AS A PRESET — this editor's `PresetProvider`.** The browser resolves the entry
	/// (factory first, then user, from its own banks); this is only what the bytes mean.
	PresetProvider& pluginPresets() {
		if (pluginPresets_ == nullptr) {
			pluginPresets_ = std::make_unique<FunctionPresetProvider>(
			    [this](PresetData& out) {
				    out = captureCurrent();
				    return !out.bytes.empty();
			    },
			    [this](const PresetData& data) {
				    if (replayingPreset) {
					    return loadPresetBytes(data);
				    }
				    // THE WAY BACK, captured BEFORE the load: the whole patch — every parameter and every
				    // tagged block — through the same capture a save makes. Recorded as an undo step once
				    // the browser names the entry (`onSelected`), so one undo puts this exact patch back.
				    PendingPresetLoad pending { captureCurrent(), {}, {}, data };
				    if (presetBrowser != nullptr && presetBrowser->currentIndex() >= 0) {
					    pending.beforeName = presetBrowser->currentName();
					    pending.beforeCategory =
					        presetBrowser->entry(static_cast<size_t>(presetBrowser->currentIndex())).category;
				    }
				    if (!loadPresetBytes(data)) {
					    return false;
				    }
				    pendingPresetLoad = std::move(pending);
				    return true;
			    });
		}
		return *pluginPresets_;
	}

	/// Load one preset's bytes into the live plugin.
	bool loadPresetBytes(const PresetData& preset) {
		const PresetData* data = &preset;
		MemIStream reader{};
		reader.in = std::span<const uint8_t>(data->bytes);
		clap_istream_t stream{};
		stream.ctx = &reader;
		stream.read = &MemIStream::read;
		// AND IF IT REFUSES, SAY SO. A preset written by a build whose state version this one does not
		// know is refused at the first byte — which is the honest outcome and exactly what we want,
		// because the alternative is walking the tagged blocks at the wrong offsets. What is not
		// acceptable is doing that silently: the player clicked a name, the sound did not change, and
		// the editor said nothing. The notify tray is the same channel a refused modulation route
		// already speaks on.
		if (!host.guiLoadState(&stream)) {
			if (notify != nullptr) {
				notify->say("Preset did not load — saved by a different version");
			}
			return false;
		}
		return true;
	}

	/// THE FACTORY BANK, WHICH IS ONE ENTRY AND HONEST ABOUT IT.
	///
	/// It was five — "Init", "Warm Pad", "Pluck Lead", "Drift Choir", "Stair Bass" — and all five were
	/// the SAME capture of the current state, taken five times in a loop. Five names, one sound: a
	/// list of promises the plugin could not keep, which is the honesty rule's failure case written as
	/// a menu. Selecting "Warm Pad" gave you Init and nothing said so.
	///
	/// One entry is what can be said truthfully today. It is the plugin's own default state, captured
	/// at build, so "Init" means what it says. AUTHORED FACTORY CONTENT IS A MATTER OF EAR, not something
	/// this function can invent — when there are real sounds to ship, they belong here as real bytes
	/// with real names.
	std::vector<PresetData> loadFactoryPresets() { return { captureCurrent() }; }

	void drawBackground(visage::Canvas& canvas) const {
		const float w = static_cast<float>(window.app()->width());
		const float h = static_cast<float>(window.app()->height());

		// Just the ground the surfaces are drawn on. The header carries the wordmark and the build
		// stamp; nothing else belongs to the window itself.
		canvas.setColor(kVoid);
		canvas.fill(0.0f, 0.0f, w, h);

		// The footer's `border-top:1px solid var(--line)` from the mock, across the WHOLE width. It
		// lives here rather than in the knob tray because it separates the footer from the surface
		// above it, and the footer is wider than the row of knobs — the tray starts after the wave
		// scope, so a line drawn to the tray's own width left the leftmost third unseparated.
		// WHOLE NATIVE ROWS, from the row the frames above it end on (`onSeparator`, backlog LI): a fill at
		// a fractional row blends its first and last rows with the void, so the line's weight wandered with
		// the scale.
		canvas.setColor(kLine);
		const float scale = canvas.dpiScale() > 0.0f ? canvas.dpiScale() : 1.0f;
		canvas.fill(0.0f, static_cast<float>(separatorRow(h - canvasExtent() - KnobTray::kHeight, scale)) / scale, w,
		            static_cast<float>(separatorRows(scale)) / scale);
	}


	/// WHAT THE HEARTBEAT HAS DELIVERED, accumulated so that a case can ask. Not diagnostics: it is
	/// the only way to show the tick ran without a test or a host calling it, and every number in it
	/// is made by notify() rather than counted beside it. See Gui::takeDriveReportForTest.
	Gui::TickReport driveReport;

	/// TEST SEAM for the bypass poll in tickOnce: when set, it stands in for the window's
	/// `modifierState()`, because a headless Gui has no window to ask and a case cannot press a
	/// real Alt. See Gui::setModifierProbeForTest.
	int (*modifierProbe)() = nullptr;

	/// DECLARED LAST, which is what makes it destroyed FIRST: its callback reaches every widget in
	/// this struct, so it has to stop being callable before any of them are gone. The same reasoning
	/// the notify readout's declaration order carries, from the other end of the list.

	/// **THE SUBSCRIPTIONS THIS STRUCT MAKES ON BEHALF OF ITS OWN WIDGETS** — see `ui::Watch`.
	///
	/// The stages are watched BY the editor rather than watching for themselves, so the token has
	/// nowhere else to live. Declared after both `FrameWatchers` members and so released before them,
	/// which is the ordering this file already argues for twice above and now gets by construction
	/// rather than by everyone remembering.
	std::vector<Watch> watches_;
};

Gui::Gui(GuiHost& host, ParamBridge& bridge) noexcept
    : impl_(std::make_unique<Impl>(host, bridge)) {}

Gui::~Gui() {
	destroy();
}

bool Gui::isAvailable() noexcept {
	return PluginWindow::isAvailable();
}

bool Gui::isApiSupported(const char* api, bool isFloating) noexcept {
	return PluginWindow::isApiSupported(api, isFloating);
}

bool Gui::create(const char* api, bool isFloating) noexcept {
	// THE CHAIN DEVICE'S EXPANDED VIEW — the rack's editor, from the rack module, filed here because this
	// library cannot include it (rack roadmap §6). Before any card asks; idempotent on a reopen.
	rack::registerChainExpandedView();
	// THE WINDOW IS THE SHELL'S NOW — see plugin/ui/plugin_window.h. Everything this function used to
	// say about Mesa's WSI threads, about never destroying a window whose swapchain is alive, and about
	// a reopen reusing the one window, moved there WITH ITS REASONING. What is left here is what this
	// editor puts inside it.
	// THE GLOBAL UI SCALE, BEFORE THE WINDOW EXISTS (backlog EJ): the first scale decision is made as the
	// window is created, so the preference has to be on the shell by then — a value applied afterwards
	// would open the editor at one size and jump. Shared with the rack's editor through the same key.
	impl_->prefs.load();
	impl_->window.setUiScale(impl_->prefs.number(PluginWindow::kUiScaleKey, 1.0));
	// THE PRESET ROOT A PERSON CHOSE, BEFORE ANY BROWSER READS A BANK — and kept from here on, whichever
	// browser in the process changes it.
	restorePresetRoot(impl_->prefs);
	if (impl_->presetRootWatch == nullptr) {
		impl_->presetRootWatch = keepPresetRootIn(impl_->prefs);
	}
	// THE TOOLTIP DETAIL (Brief | Full), the same two halves — read now, kept from here on.
	restoreTooltipDetail(impl_->prefs);
	if (impl_->tooltipDetailWatch == nullptr) {
		impl_->tooltipDetailWatch = keepTooltipDetailIn(impl_->prefs);
	}
	// THE CHROME HUE (backlog LG), the same two halves, plus a repaint: every frame reads the palette as it
	// draws, so one redraw of the whole tree is the whole of "live".
	// THE WINDOW-CONTROLS SIDE (backlog OD): the stored side, else the platform's norm; kept from here on.
	restoreWindowControlsSide(impl_->prefs);
	if (impl_->windowControlsSideWatch == nullptr) {
		impl_->windowControlsSideWatch = keepWindowControlsSideIn(impl_->prefs);
	}
	// ARPS SENT TO FRONT OF CHAIN, likewise: the stored choice, else ON.
	restoreEmittersToFront(impl_->prefs);
	if (impl_->emittersFrontWatch == nullptr) {
		impl_->emittersFrontWatch = keepEmittersToFrontIn(impl_->prefs);
	}
	// REPAINT, PARTIAL OR EVERY FRAME , likewise: the stored choice, else the build's default; a change
	// repaints the whole tree so a switch to partial starts from a whole, current frame.
	restoreFullRedraw(impl_->prefs);
	if (impl_->fullRedrawWatch == nullptr) {
		impl_->fullRedrawWatch = keepFullRedrawIn(impl_->prefs, [impl = impl_.get()] {
			if (visage::ApplicationWindow* top = impl->window.app(); top != nullptr) {
				top->redrawAll();
			}
		});
	}
	restoreChromeHue(impl_->prefs);
	if (impl_->chromeHueWatch == nullptr) {
		impl_->chromeHueWatch = keepChromeHueIn(impl_->prefs, [impl = impl_.get()] {
			if (visage::ApplicationWindow* top = impl->window.app(); top != nullptr) {
				top->redrawAll();
			}
		});
	}
	restoreAccentHue(impl_->prefs);
	if (impl_->accentHueWatch == nullptr) {
		impl_->accentHueWatch = keepAccentHueIn(impl_->prefs, [impl = impl_.get()] {
			if (visage::ApplicationWindow* top = impl->window.app(); top != nullptr) {
				top->redrawAll();
			}
		});
	}
	bool built = false;
	if (!impl_->window.create(api, isFloating, &built)) {
		return false;
	}
	if (!built) {
		return true; // a reopen: the window and its contents are already there
	}
	visage::ApplicationWindow* app = impl_->window.app();
	if (app == nullptr) {
		return false;
	}

	auto notify = std::make_unique<NotifyTray>();
	// Over everything, and deaf: it is a readout, not a control, and a click meant for the knob
	// underneath must reach the knob. The mock says the same thing with `pointer-events:none`.
	notify->setIgnoresMouseEvents(true, false);
	// THE PRESET TOAST, the same widget at its own timing and just as deaf; it never takes the keyboard.
	impl_->presetToast = std::make_unique<NotifyTray>(
	    [impl = impl_.get()] {
		    return impl->toastClock ? impl->toastClock() : static_cast<int64_t>(visage::time::milliseconds());
	    },
	    NotifyTray::kToastTiming);
	impl_->presetToast->setIgnoresMouseEvents(true, false);

	auto header = std::make_unique<HeaderBar>(impl_->bridge, impl_->registry, impl_->mode, notify.get());
	app->addChild(header.get());
	// THE UI SCALE READOUT'S TWO ROADS (backlog EJ): the header asks the shell what the scale is and
	// tells the editor to step it; the editor persists it and says the percentage, as the keys do.
	header->uiScaleNow = [this] { return impl_->window.uiScale(); };
	header->onUiScaleStep = [this](int direction) { impl_->stepUiScale(direction); };
	// THE SETTINGS SCREEN'S DOOR (backlog LG): the Libra mark, or the wordmark.
	header->onSettings = [this] { impl_->openSettings(); };
	impl_->header = std::move(header);

	// The preset browser, to the right of the FEATHERS wordmark. Seeded with the factory bank — "Init"
	// plus a handful of starter sounds — and an empty user bank the user saves into. The browser owns
	// the *selection*; the editor owns the bytes behind each preset through the onCapture/onSelected
	// callbacks. The header gets a pointer so it can forward mouse events to the steppers and menu.
	auto presetBrowser = std::make_unique<PresetBrowser>(
	    impl_->registry, std::vector<std::string>{"Init"}, impl_->loadFactoryPresets());
	// AND THE USER'S OWN BANK, off disk. Read here rather than lazily so that the browser's list is
	// right the first time it is drawn — see preset_store.h.
	presetBrowser->loadBankFromDisk();
	app->addChild(presetBrowser.get());
	impl_->presetBrowser = std::move(presetBrowser);
	impl_->header->setPresetBrowser(impl_->presetBrowser.get());
	impl_->presetBrowser->setProvider(&impl_->pluginPresets());
	// WHERE IMPORT… AND EXPORT… SAY WHAT HAPPENED — the tray a refused preset load already speaks on.
	impl_->presetBrowser->setNotifier(notify.get());
	// HOW THE LIST WAS LAST FILED, from the prefs `create()` has already read; a hand's change is written
	// the moment it is made, like the rails'.
	impl_->presetBrowser->setView(impl_->prefs.flag(kPresetsByTagKey) ? PresetBrowser::View::Tags
	                                                                  : PresetBrowser::View::Folders);
	// A LOAD THE BROWSER MADE — a pick, a step of the arrows, an import — is one undo step and a toast.
	// `onSelected` is told only after the provider took the bytes, and names the entry it now shows.
	impl_->presetBrowser->onSelected([this](int index) {
		PresetBrowser* browser = impl_->presetBrowser.get();
		if (browser == nullptr || index < 0 || static_cast<size_t>(index) >= browser->count()) {
			impl_->pendingPresetLoad.reset();
			return;
		}
		const PresetBrowser::Entry& now = browser->entry(static_cast<size_t>(index));
		if (impl_->pendingPresetLoad.has_value()) {
			Impl::PendingPresetLoad pending = std::move(*impl_->pendingPresetLoad);
			impl_->pendingPresetLoad.reset();
			impl_->history.record(std::make_unique<PresetLoadEdit>(
			    [impl = impl_.get()](const PresetLoadEdit::Patch& patch) {
				    impl->replayPreset(patch.bytes, patch.name, patch.category);
			    },
			    PresetLoadEdit::Patch { std::move(pending.before), std::move(pending.beforeName),
			                            std::move(pending.beforeCategory) },
			    PresetLoadEdit::Patch { std::move(pending.after), now.name, now.category }));
		}
		impl_->showPresetToast(now.name);
	});
	// **WHICH PRESET THE SOUND CAME FROM IS THE PATCH'S** (backlog QC: an engine restart used to forget which preset it had
	// loaded and show init). The face is put back on the patch's copy now — an editor rebuilt over a live instance, or an
	// instance the host just loaded — and every change the browser makes goes into that copy, which the `preset` block saves.
	impl_->restoringPresetFace = true;
	(void)impl_->presetBrowser->restoreCurrent(impl_->bridge.presetIdentity().name, impl_->bridge.presetIdentity().category);
	impl_->restoringPresetFace = false;
	impl_->seenPresetIdentity = impl_->bridge.presetIdentitySerial();
	impl_->presetBrowser->onCurrentChanged = [this](const std::string& name, const std::string& category) {
		if (impl_->restoringPresetFace) {
			return;
		}
		impl_->bridge.setPresetIdentity(PresetIdentity { name, category });
		impl_->seenPresetIdentity = impl_->bridge.presetIdentitySerial();
	};
	impl_->presetBrowser->onViewChanged = [this](PresetBrowser::View view) {
		impl_->prefs.setFlag(kPresetsByTagKey, view == PresetBrowser::View::Tags);
	};

	auto engineRail = std::make_unique<EngineRail>(impl_->bridge, impl_->registry, notify.get());
	app->addChild(engineRail.get());
	impl_->engineRail = std::move(engineRail);
	// Same wiring as the modulator strip's below: the rail says its width moved and the editor takes
	// the column sum again. See the note there for why a rail never sets its own bounds.
	impl_->engineRail->onRailWidthChanged([this] {
		impl_->prefs.setFlag(kEngineRailKey, impl_->engineRail->collapsed());
		impl_->railWidthChanged(*impl_->engineRail);
	});

	// PREVIEW FOLLOWS THE SELECTION, and this is the single place that carries it. The sound being
	// listened to lives on the bridge, where the audio thread can read it; the selection moves down
	// half a dozen roads (a card, a clone, an undo, a crumb, a switch coming on), and every one of them
	// has to move the preview with it or a latched preview would sit on the sound you have stopped
	// working on. Only while a preview is actually on: with none, this does nothing at all.
	impl_->selection.onMoved([this](size_t slot) {
		if (impl_->bridge.preview().active()) {
			impl_->bridge.setPreview(slot);
		}
	});

	// THE MODULATOR STRIP, between the Engine rail and the sound rail — the mock's own column order.
	auto modPanel = std::make_unique<ModPanel>(impl_->bridge, impl_->deviceHost, impl_->registry, impl_->mod,
	                                           impl_->history, notify.get());
	app->addChild(modPanel.get());
	impl_->modPanel = std::move(modPanel);
	// A RAIL THAT CLOSES CHANGES WHERE EVERY COLUMN IS, and only this side knows that. The panel says
	// its width moved; the editor takes the sum again and lays the whole row out — the same entry
	// point a resize goes through, so a collapse and a drag cannot end up with different arithmetic.
	impl_->modPanel->onRailWidthChanged([this] {
		impl_->prefs.setFlag(kModulatorRailKey, impl_->modPanel->collapsed());
		impl_->railWidthChanged(*impl_->modPanel);
	});
	// A FOLD CHANGES NO COLUMN, so unlike the rail it asks for no layout — the strip re-places its own
	// cards. AND THE EDITOR NO LONGER WRITES IT DOWN EITHER: the fold is the bridge's, stored against the
	// device and saved with the patch (`ParamBridge::setDeviceFolded`), so the panel's own setter has
	// already recorded it by the time this runs. Nothing is subscribed here today; the hook stays because
	// it is what a notifier or a future desk-side reaction would use.

	// THE POST-FX LANE, on the other side of the stage. Added here rather than beside the left rails
	// because its column is measured from the opposite edge; wired exactly like them, which is the
	// whole claim the third heir was built to test.
	// THE LANE BUILDS ITS OWN CARDS NOW, so it is handed everything a card needs rather than a card.
	// `stateWatchers` goes with it for the reason the subscription below used to exist here and no
	// longer can: the lane rebuilds a card whenever a slot changes device, and a subscription made
	// out here would be made once, for the card that happened to exist at startup.
	auto fxLane = std::make_unique<FxLane>(impl_->bridge, impl_->registry, &impl_->mod,
	                                       &impl_->history, impl_->notify.get(),
	                                       &impl_->stateWatchers);
	app->addChild(fxLane.get());
	impl_->fxLane = std::move(fxLane);
	impl_->fxLane->onRailWidthChanged([this] {
		impl_->prefs.setFlag(kFxLaneKey, impl_->fxLane->collapsed());
		impl_->railWidthChanged(*impl_->fxLane);
	});
	// AN ARMED SOURCE THE STRIP DOES NOT HOLD IS THE LANE'S — a modulator in the fx lane (2026-10-04).
	impl_->modPanel->onArmedElsewhere = [this](backplane::Source source) {
		if (source.isDevice()) {
			(void)impl_->fxLane->focusDevice(source.device);
		}
	};

	// THE CARD IS THE LANE'S, and so is its subscription to the state road. Both used to be made here
	// — a FilterCard built, docked, and registered with stateWatchers in three separate statements —
	// which was correct for exactly as long as the lane could hold one device that never changed.
	//
	// THE SUBSCRIPTION IS THE HALF THAT COULD NOT STAY. A card built out here is registered once; a
	// lane that rebuilds a card when its slot changes device would hand back a card nothing was
	// watching, and a card that has quietly stopped repainting is a failure no headless suite can
	// see. So the registration moved to the one place that knows a card has just been built.

	auto soundStack = std::make_unique<SoundStack>(impl_->bridge, impl_->registry, impl_->selection,
	                                              impl_->history, impl_->previewWatchers,
	                                              notify.get());
	app->addChild(soundStack.get());
	impl_->soundStack = std::move(soundStack);
	// THE FIRST OF THE TWO WRONG SURFACES. The rail cards watch seven parameters per slot and then
	// draw from six channels that are not parameters at all — the hues, the variation rows and the
	// ride-along readout's travel, gamma, orientation and residue. So a card showed a placement that
	// had moved only when something else happened to invalidate it.
	impl_->watches_.push_back(impl_->stateWatchers.watch(impl_->soundStack.get()));

	// THE SELECTION, FOR THE ONE CONTROL IN THIS ROW THAT IS PER-SOUND. Gamma's coarse half is
	// `SoundGamma::base[slot]`, so the tray has to know which sound is being edited — and it must ASK
	// rather than be told once, because the selection moves under it. See `GammaBaseControl`.
	auto tray = std::make_unique<KnobTray>(impl_->bridge, impl_->registry, notify.get(),
	                                       [this] { return impl_->selection.slot(); });
	app->addChild(tray.get());
	impl_->tray = std::move(tray);

	// THE DESTINATIONS LIVE IN THREE PLACES — four in the footer row, Glide and Fine in the Engine rail,
	// and the filter's five on its card in the fx lane — so all three are handed the panel's state. A
	// knob that never gets it is exactly as it was before this feature existed, which is what keeps
	// every other editor in this repo unchanged; it is also what made the filter card's knobs
	// unarmable for as long as they went untold.
	impl_->tray->setModulation(&impl_->mod, &impl_->history);
	impl_->engineRail->setModulation(&impl_->mod, &impl_->history);
	impl_->fxLane->setModulation(&impl_->mod, &impl_->history);

	auto perform = std::make_unique<PerformStage>(impl_->bridge, impl_->registry, notify.get());
	app->addChild(perform.get());
	// The blend circle dims what a latched preview has silenced, so it repaints when that moves.
	impl_->watches_.push_back(impl_->previewWatchers.watch(perform.get()));
	// And it reads the HUES, which are a channel rather than a parameter. Not the weights: those are
	// published from the audio thread and are deliberately outside this signal, so a subscription
	// buys nothing for them.
	impl_->watches_.push_back(impl_->stateWatchers.watch(perform.get()));
	impl_->perform = std::move(perform);

	impl_->terrain = std::make_unique<TerrainMap>();
	// THE GROUND FIRST, AND THAT ORDER IS THE WHOLE POINT (backlog FE, the bubble): visage paints a
	// frame's children after the frame itself, so the only way to put the warped terrain UNDER the
	// stage's grid and stars is to make it an earlier sibling. See TerrainGround's header.
	auto ground = std::make_unique<TerrainGround>();
	app->addChild(ground.get());
	auto explore = std::make_unique<ExploreStage>(impl_->bridge, impl_->registry, impl_->selection,
	                                              impl_->history, *impl_->terrain, notify.get());
	app->addChild(explore.get());
	ground->setStage(explore.get());
	explore->setGround(ground.get());
	impl_->ground = std::move(ground);
	// EXPLORE SUBSCRIBES TO EVERYTHING IT READS, INCLUDING THE CHANNELS IT WRITES ITSELF — which
	// looks like it should double-count a scrub and does not. Its centredOnGamma_/scrubbedGamma_
	// compensations run at WRITE time, inside the gesture, and the view they correct is a const
	// getter evaluated at DRAW time; this notification does nothing but mark the frame stale. So a
	// scrub still redraws exactly once with the compensations already applied, and gamma moved by
	// any OTHER road — the vernier knob, GlobalGamma, a preset load, an undo, host automation — now
	// repaints it too, which is the half that was missing.
	impl_->watches_.push_back(impl_->stateWatchers.watch(explore.get()));
	impl_->explore = std::move(explore);
	// AND THE EXPLORE STAGE, which was missing from the setModulation block above — its twenty-four
	// per-sound Width, FM Depth and Smoothness knobs had no ModState at all, so they never lit in mod
	// mode and could not be pointed at. Every surface that carries a Knob belongs on that list.
	//
	// HERE RATHER THAN BESIDE THE OTHERS, because the others are moved into impl_ before that block
	// runs and this one is not — putting it up there dereferenced a null unique_ptr and took every
	// case that builds an editor with it. The wiring has to follow the ownership, not the topic.
	impl_->explore->setModulation(&impl_->mod, &impl_->history);
	// THE SPECTRUM TAB ASKS THIS EDITOR FOR THE PANEL — Explore holds none (Impl::toggleSpectrumPanel).
	impl_->explore->onSpectrumTab = [this] { impl_->toggleSpectrumPanel(); };
	impl_->explore->onDrumTab = [this] { impl_->toggleDrumPanel(); };
	// THE STAGE'S COLLAPSE (backlog NF), AFTER BOTH SURFACES so its corner paints over them and takes the press.
	// Wired exactly like the other rails — the desk remembers it, the editor re-derives the columns — plus the
	// surfaces, which it does not parent (see StageRail's header).
	auto stageRail = std::make_unique<StageRail>();
	app->addChild(stageRail.get());
	impl_->stageRail = std::move(stageRail);
	impl_->stageRail->onRailWidthChanged([this] {
		impl_->prefs.setFlag(kStageRailKey, impl_->stageRail->collapsed());
		impl_->showSurface();
		// THE SYNTH JUST SHRINKS: closing gives the stage's room back to the host and opening asks
		// for it again — nothing in the editor widens to take it, and the cards keep their unit width.
		const double room = impl_->stageRoom();
		impl_->window.widthChangedBy(impl_->stageRail->collapsed() ? -room : room);
	});
	// WHICH SURFACE AN EDITOR OPENS ON, for a capture and for a stress run. X11 synthesis is dead here
	// (see the plan), so without this the Explore surface could never be photographed or exercised for
	// LIFETIME — and lifetime is exactly what its worker thread makes worth exercising. Same channel
	// as FEATHERS_REDUCED_MOTION and FEATHERS_SHIMMER_AT, and it stores nothing: the switch in the
	// header owns the mode from the first click onward.
	if (const char* surface = std::getenv("FEATHERS_SURFACE")) {
		impl_->mode.show(std::string(surface) == "explore" ? StageMode::Surface::Explore
		                                                   : StageMode::Surface::Perform);
	}
	impl_->mode.onChanged([this] { impl_->showSurface(); });
	impl_->showSurface();

	auto wave = std::make_unique<WaveDisplay>(impl_->bridge, impl_->registry);
	// A readout, not a control: the click that lands on it belongs to whatever is underneath, and
	// nothing is, which is the point of putting the notify line over it.
	wave->setIgnoresMouseEvents(true, false);
	app->addChild(wave.get());
	// The scope draws the character the engine is building, and a preview changes which sounds are in
	// it — so it repaints for the same reason the circle does.
	impl_->watches_.push_back(impl_->previewWatchers.watch(wave.get()));
	// THE SECOND. The wave display derives the character it draws from travel, gamma, per-sound
	// orientation, residue and the audition tuning, and tints it from the hues — none of which is a
	// parameter, and none of which it was told about. Its own refresh() is a change-detector and
	// stays one: it still rebuilds only when the character has really moved. What it lacked was any
	// reason to be asked.
	impl_->watches_.push_back(impl_->stateWatchers.watch(wave.get()));
	impl_->wave = std::move(wave);

	// THE MASTER METER (backlog DN): a readout at the tray's far right, deaf like the scope, fed on the
	// tick from what the plugin published.
	auto meter = std::make_unique<MasterMeter>(impl_->bridge);
	meter->setIgnoresMouseEvents(true, false);
	app->addChild(meter.get());
	impl_->meter = std::move(meter);

#if defined(FEATHERS_DEBUG_PANEL)
	// The rotate pin, first in the grid so it is where a hand goes looking for it. ONE SLIDER: the
	// orientation is one number under M — the aperture and extra-angle rows the panel used to offer
	// were dead knobs, showing authority the decode does not read. A live sweep is what makes the
	// audition question answerable in one session rather than across five restarts of the host — see
	// orientation_row.h.
	{
		auto row = std::make_unique<OrientationRow>(impl_->bridge, [this] {
			// The readout follows the orientation, since nothing else would tell it: it redraws on
			// parameter moves, and this is deliberately not a parameter.
			if (impl_->wave != nullptr) {
				impl_->wave->redraw();
			}
		});
		app->addChild(row.get());
		impl_->orientationRows.push_back(std::move(row));
	}

	// THE FALLOFF A/B, §7.1d, and it ships in this build rather than an earlier one because an earlier
	// one could not hear it: with two sounds Path rules the blend and the exponent reaches nothing. A
	// control that does nothing in the build it ships in teaches its user that it does nothing.
	{
		auto row = std::make_unique<TuningRow>(
		    "FALLOFF",
		    std::vector<TuningRow::Step> { { kFalloffPowerA, "1 (NEAR)" }, { kFalloffPowerB, "2 (LOCAL)" } },
		    [this] { return impl_->bridge.tuning().falloffPower; },
		    [this](double value) { impl_->bridge.setFalloffPower(value); },
		    [this] {
			    if (impl_->wave != nullptr) {
				    impl_->wave->redraw();
			    }
		    });
		app->addChild(row.get());
		impl_->tuningRows.push_back(std::move(row));
	}

	// THE EIGHT ENABLES ARE GONE FROM HERE, and what removed them is the sound stack. They existed for
	// one reason — "a sound you cannot switch on is a sound the surface can never show you" — and the
	// designed rail now carries that switch on every card, in the shipping build rather than behind a
	// compile flag. Kept in the history rather than in the strip: a debug row whose job the real
	// surface has taken over is a second control for one value, and the next person to add a sound
	// would have two places to look.

	// THE BLUE SHIFT'S TWO SCALES, §2b.1 — global scales over φ fields rather than flat depths, so an
	// ear tunes the magnitude and φ authors the spatial variation. Both start at x1, the chosen shipping
	// default: blue shift is LIVE, because §2c's uniform rule is gated on there being directional
	// character to give back. OFF is the reference to compare against, and it is the first step on each
	// row for exactly that reason.
	//
	// Two rows rather than one because the plan calls the flavours compoundable, and a single control
	// over both would make "is it the overshoot or the Doppler I am hearing" unanswerable — which is
	// the only question this build exists to ask.
	{
		const std::vector<TuningRow::Step> steps { { kBlueShiftScaleOff, "OFF" },
			                                       { kBlueShiftScaleOn, "x1" },
			                                       { kBlueShiftScaleMore, "x2" } };
		auto redrawScope = [this] {
			if (impl_->wave != nullptr) {
				impl_->wave->redraw();
			}
		};
		auto overshoot = std::make_unique<TuningRow>(
		    "BLUE OVERSHOOT", steps, [this] { return impl_->bridge.tuning().blueOvershootScale; },
		    [this](double value) {
			    impl_->bridge.setBlueShiftScales(value, impl_->bridge.tuning().blueDopplerScale);
		    },
		    redrawScope);
		app->addChild(overshoot.get());
		impl_->tuningRows.push_back(std::move(overshoot));

		auto doppler = std::make_unique<TuningRow>(
		    "BLUE DOPPLER", steps, [this] { return impl_->bridge.tuning().blueDopplerScale; },
		    [this](double value) {
			    impl_->bridge.setBlueShiftScales(impl_->bridge.tuning().blueOvershootScale, value);
		    },
		    redrawScope);
		app->addChild(doppler.get());
		impl_->tuningRows.push_back(std::move(doppler));
	}

	// Every parameter as a bare row, which is what the editor drew before it had a design. Kept
	// because it is the cheapest param-surface smoke test there is, and now behind a SECOND switch:
	// it covers the stage, and the stage is the thing being auditioned. Seventy rows over the Perform
	// circle is a smoke test wearing an instrument's clothes.
	if (std::getenv("FEATHERS_DEBUG_ALL_PARAMS") != nullptr) {
	for (const ParamDesc& desc : paramDescs()) {
		auto row = std::make_unique<ParamRow>(impl_->bridge, impl_->registry, desc);
		app->addChild(row.get());
		impl_->rows.push_back(std::move(row));
	}
	}
#endif

	// THE EXPANDED-VIEW LAYER, added after every surface it must cover and before the notify tray,
	// which stays on top of everything. It is invisible and deaf until a device is opened into it —
	// a full-window frame that answered presses while empty would swallow the whole editor.
	{
		// THE CANVAS FIRST, a sibling of the rails and under the layers: it is a region of the layout
		// the overlay must be able to float over.
		auto canvas = std::make_unique<ExpandedCanvas>(ExpandedCanvas::Edge::Bottom);
		// NEVER MORE THAN THE INSTRUMENT ABOVE IT — see `ExpandedCanvas::kMaxShare`.
		canvas->setMaxExtent(static_cast<float>(kDefaultHeight) * ExpandedCanvas::kMaxShare);
		app->addChild(canvas.get());
		impl_->canvas = std::move(canvas);
		auto layers = std::make_unique<SurfaceLayers>();
		app->addChild(layers.get());
		impl_->surfaces = std::move(layers);
		impl_->surfaces->setCanvas(impl_->canvas.get());
	}
	// THE PANEL RIDES THE STATE CHANNEL ITSELF. A swallowed device's controls are not parameters, so
	// this is the only road that repaints them — and handing the layer the watchers rather than
	// having the panel walk up to find them is re-parenting property 5.
	impl_->surfaces->setStateWatchers(&impl_->stateWatchers);
	// THE SECOND LIST. Its knobs join `ModState`'s watchers on their own; without this the overlay
	// cannot take them off again. See `setModulationState`. ONE CALL FOR EVERY LAYER, canvas included.
	impl_->surfaces->setModulationState(&impl_->mod);
	// THE CANVAS'S FOLD AND ITS PARTITION ARE PATCH STATE AND A CHANGE OF PREFERRED SIZE, both at once: the bridge takes
	// the view, and the window re-derives its scale and asks the host for the height this editor now
	// wants. A drag step only re-lays out at the window it has; the ask waits for the release.
	impl_->canvas->onCollapsedChanged = [this] {
		// OPENING INTO A WINDOW THAT ALREADY HAS ROOM TAKES THAT ROOM rather than asking for more — see
		// `ExpandedCanvas::fitOpenExtentTo` and `PluginWindow::preferredSizeChanged`, which now only grows.
		if (!impl_->canvas->collapsed()) {
			(void)impl_->canvas->fitOpenExtentTo(
			    static_cast<float>(impl_->window.logicalRoomHeight()) - static_cast<float>(kDefaultHeight));
		}
		impl_->writeCanvasView();
		impl_->window.preferredSizeChanged();
	};
	// A DRAG STEP PUTS THE PARTITION UNDER THE POINTER AND KEEPS THE INSTRUMENT WHOLE. The canvas has
	// already moved its extent by the hand's logical delta; with a window to measure against, the
	// extent is recomputed so the canvas's physical edge is where the pointer is and the instrument
	// above it keeps its design height, the SCALE paying the difference (`partitionExtent`). Then the
	// scale is re-derived and the editor laid out — every step, so nothing is ever squeezed and the
	// hand sees the balance it is choosing. Without a window (a case), the logical delta stands.
	impl_->canvas->onExtentDrag = [this](visage::Point pointer) {
		const int nativeH = impl_->window.nativeHeight();
		const double preferred = impl_->window.preferredScale();
		if (nativeH > 0 && preferred > 0.0 && impl_->window.app() != nullptr) {
			// In host units, like nativeH — see PluginWindow::contentScale.
			const float pointerNative = pointer.y * static_cast<float>(impl_->window.contentScale());
			const float canvasNative = static_cast<float>(nativeH) - pointerNative;
			// CAPPED AT THE INSTRUMENT'S OWN HEIGHT (`kMaxShare`): the canvas is never more than the
			// synth beside it, so the scale never falls past a half and the grip stays a handle.
			(void)impl_->canvas->setOpenExtent(ExpandedCanvas::partitionExtent(
			    static_cast<float>(kDefaultHeight), static_cast<float>(nativeH), canvasNative,
			    static_cast<float>(preferred), ExpandedCanvas::kMinExtent, impl_->canvas->maxExtent()));
		}
		impl_->window.syncScaleAndLayout();
	};
	impl_->canvas->onExtentCommit = [this] {
		impl_->writeCanvasView();
		impl_->window.preferredSizeChanged();
	};

	// THE WINDOW HOST, BUILT WHETHER OR NOT IT IS THE ONE IN USE, so flipping the preference between
	// opens costs no construction and no null check anywhere but the call site. It is on the SAME
	// state channel as the overlay: a detached window is not a descendant of the editor, so nothing
	// else would carry a parameter change to it.
	impl_->expandedWindow = std::make_unique<WindowedExpanded>();
	impl_->expandedWindow->setStateWatchers(&impl_->stateWatchers);
	// AND THE SECOND LIST, EXACTLY AS THE OVERLAY ABOVE. `onExpandDevice` calls
	// `view->setModulation(&impl_->mod, &impl_->history)` on the content BEFORE handing it to either
	// host, so every modulatable knob under it joins `ModState`'s watchers whichever host takes it.
	// Only the host can take them off again, so a host without this pointer leaks a freed frame onto
	// that list every time a window closes — the crash `96d94be` and `25282de` each fixed one door
	// over.
	impl_->expandedWindow->setModulationState(&impl_->mod);
	// **THE UNDO CHORDS IN THE POPOUT TOO** (backlog JB): the popout is its own OS window, so a key pressed there
	// never reaches this editor's root. Same chord, same history, same refusal while a field is typing.
	impl_->expandedWindow->onKey = [this](const visage::KeyEvent& e) {
		// AND ESC, ON THE EDITOR'S ONE STACK (backlog NN): the newest thing the editor opened, the popout included.
		if (e.keyCode() == visage::KeyCode::Escape) {
			return impl_->escapeTop();
		}
		const std::optional<bool> redo = Impl::undoChordOf(e);
		return redo.has_value() && impl_->walkHistoryFromKey(*redo, impl_->expandedWindow->root());
	};
	impl_->expandedWindow->onDismiss = [this] {
		impl_->popoutEscape.release();
		// THE SPECTRUM PANEL, IF IT WAS THE ONE IN THE WINDOW: its handle goes with it (Impl::spectrumClosed).
		if (impl_->spectrumInWindow) {
			impl_->spectrumClosed();
		}
		if (impl_->fxLane != nullptr) {
			impl_->fxLane->redraw();
		}
	};
	// ESC LEAVES MODULATION ASSIGN MODE — the
	// PERMANENT exit beside Alt's momentary one, since both doors into the mode are a click on the source's
	// own control. **SINCE BACKLOG NN THE MODE IS ONE ENTRY ON THE EDITOR'S ESC STACK**: opened by any hand's arm in the follow-tree, raised by a re-arm, taken
	// off by any disarm. So Esc takes the arm off when it came after an open window, and the window when it
	// came after the arm — the overlay's old "nearest the hand" door (`onEscapeWhenClosed`) is no longer
	// wired: every Esc handler in this editor asks the one stack.
	impl_->armEscape.attach(impl_->mod, impl_->surfaces->escapeStack());
	// THE FX LANE'S SELECTION OF TWO OR MORE IS AN ENTRY ON THE SAME STACK (backlog OA, D1): Esc clears it.
	if (impl_->fxLane != nullptr) {
		impl_->fxLane->setEscapeStack(&impl_->surfaces->escapeStack());
	}
	// AND THE MODULATOR STRIP'S (backlog PN), the same rule over the same stack.
	if (impl_->modPanel != nullptr) {
		impl_->modPanel->setEscapeStack(&impl_->surfaces->escapeStack());
	}
	// THE OVERLAY'S `onDismiss` IS THE LAYERS' — it is how they retire the id — so what this editor
	// wants told on a dismissal travels with each `present` instead (see `onExpandDevice`).
	// A PRESS ON A CARD'S HEAD FOCUSES THAT DEVICE'S WINDOW IF ONE IS OPEN — the
	// overlay's front panel. The popout-window road (`kPopoutWindowKey`) is an OS window this editor
	// cannot order (the reason it is off by default), so it is not asked.
	impl_->fxLane->onFocusDeviceWindow = [this](DeviceInstanceId device, DeviceNest nest) {
		return impl_->surfaces != nullptr && impl_->surfaces->focusPanelFor(device, nest);
	};
	// **ONE OPENER FOR EVERY DEVICE A HAND CAN EXPAND** — the fx lane's cards and the modulator strip's
	// Addressed by id and
	// descriptor, so neither caller can hand it a position.
	const auto openDeviceWindow = [this](DeviceInstanceId device, const device::Descriptor* desc) {
		// **A PRESS WHILE THIS DEVICE'S WINDOW IS UP CLOSES IT**. Both hosts are
		// asked whatever the preference says, because it can be flipped while a panel is open. This
		// editor is the outermost host, so its devices are in the top chain: the default nest, which is
		// what the view built below would carry.
		const bool closedPanel = impl_->surfaces->dismissPanelFor(device, DeviceNest {});
		const bool closedWindow = impl_->expandedWindow != nullptr &&
		                          impl_->expandedWindow->showsDevice(device, DeviceNest {});
		if (closedWindow) {
			impl_->expandedWindow->dismiss();
		}
		if (closedPanel || closedWindow) {
			return;
		}
		// BOTH ROADS: converged panels take the seam, the rest still need the bridge, and one table
		// decides which. The rack passes nullptr for the bridge and gets nullptr back for those.
		auto view = makeExpandedView(impl_->deviceHost, &impl_->bridge, impl_->registry, device, *desc,
		                             impl_->notify.get());
		if (view == nullptr) {
			return; // the device declares no expanded view, or this build cannot construct it
		}
		// THE CONTENT IS HANDED THE MODULATION STATE THE SAME WAY A CARD IS, before it is shown: a
		// knob that has not been told never lights while mapping is armed, which is the failure
		// FilterCard's own knobs shipped with once.
		// ONE CALL, THROUGH THE BASE. This was two `dynamic_cast`s — HARM, then OWLTT after HARM
		// shipped the bug — and the note beside them said the next device should make it virtual
		// instead of adding a third. **By then WAYPOINT had been missed entirely**: its panel was
		// built, shown and never handed the state, so every control on it reported itself a
		// destination and none was armable. A list of casts is a list somebody has to remember to
		// add to, and this one had already been forgotten once while the comment warning about it
		// was on screen.
		view->setModulation(&impl_->mod, &impl_->history);
		// ONE PANEL, TWO POSSIBLE HOSTS, AND THE CONTENT KNOWS NEITHER. `kPopoutWindowKey` picks;
		// everything above this line is identical either way, which is the whole point of the five
		// properties expanded_overlay.h kept.
		// THE LAYER IS THE DEFAULT AGAIN, ON EVERY PLATFORM — 2026-09-07, first Mac build: the
		// separate window popped under the DAW and stayed under it after `setAlwaysOnTop`, and the
		// decision was to go back to the window inside the plugin window, but floating instead of fixed
		// location. The OS-window road is kept and still selected by this key: a prefs line
		// `popout.window=true` brings it back (it worked on Linux; it is the Mac that cannot order it).
		if (impl_->expandedWindow != nullptr && impl_->prefs.flag(kPopoutWindowKey, false)) {
			// CLOSE THE OTHER ONE FIRST. The switch can be flipped between opens, and two panels for
			// one device is a state nothing in the editor can address.
			impl_->surfaces->dismissPresented();
			// A DEVICE WINDOW REPLACES THE SPECTRUM PANEL in the one popout window, so its handle goes first.
			if (impl_->spectrumInWindow) {
				impl_->spectrumClosed();
			}
			impl_->expandedWindow->show(std::move(view));
			impl_->popoutOpened();
			return;
		}
		if (impl_->expandedWindow != nullptr) {
			impl_->expandedWindow->dismiss();
		}
		// THROUGH THE SEAM THE RACK USES — canvas or overlay is the layers' call, not this editor's.
		const std::string title = desc->traits.name;
		(void)impl_->surfaces->present(std::move(view), SurfaceNeed::SpansGesture, title, [this] {
			// THE LANE REDRAWS BECAUSE ITS EXPAND BUTTON MAY DRAW DIFFERENTLY once nothing is open.
			// It does not today; the road exists so that the day it does, the button is not a
			// control whose state only changes on the next unrelated repaint.
			if (impl_->fxLane != nullptr) {
				impl_->fxLane->redraw();
			}
		});
	};
	impl_->fxLane->onExpandDevice = [this, openDeviceWindow](size_t slot) {
		// THE DEVICE THE SLOT ACTUALLY HOLDS, from the tag in the row and through the registry — the
		// same road the lane takes to choose a card, so the panel and the card cannot open different
		// devices from one press.
		const FxChain chain = impl_->bridge.fxChain();
		if (slot >= chain.count || slot >= chain.device.size()) {
			return;
		}
		const device::Descriptor* desc = deviceForTag(fxDeviceAtPosition(chain, slot).tag().data());
		if (desc == nullptr) {
			return;
		}
		// **THE BOUNDARY, CROSSED HERE TOO — `slot` IS A POSITION AND A PANEL IS ADDRESSED BY ITS DEVICE.**
		//
		// `FxLane::ensureCard` crosses exactly this boundary for the CARD and says so in its own
		// comment, building its card with the device's id (a lane before kill-lanes step 2). The EXPAND
		// road, one press away, once passed the position straight through — so every control in a
		// floating panel addressed whichever device held the lane NUMBERED like that panel's position,
		// and read and wrote it.
		//
		// The symptom: the window controls on harm affected owltt's settings.
		// Both halves were this line — the panel's writes landing on
		// another device, and its readouts showing that device's values. Every chain that has only
		// ever been appended to had lane == position, which is why no fixture saw it. An id is a
		// different KIND of number from a position, and `makeExpandedView` refuses a `size_t`.
		const DeviceInstanceId device = impl_->bridge.fxDeviceAt(slot);
		openDeviceWindow(device, desc);
	};
	// **THE SITE CHIP** (backlog NR): the device gets its PRE/POST chains on first use (filed, and built at the next
	// visit's provision), and their panel opens by the same road a device window does — keyed by the site, so it is a
	// panel of its own beside the device's.
	impl_->fxLane->setOnExpandSite([this, openDeviceWindow](size_t slot) {
		const FxChain chain = impl_->bridge.fxChain();
		if (slot >= chain.count || slot >= chain.device.size()) {
			return;
		}
		const DeviceInstanceId device = impl_->bridge.fxDeviceAt(slot);
		if (!impl_->deviceHost.openSite(device)) {
			return;
		}
		openDeviceWindow(siteKeyOf(device), &siteViewDescriptor());
	});
	if (impl_->modPanel != nullptr) {
		// THE STRIP'S OWN TWO ROADS, the lane's twins: its cards are faces of devices by id already.
		impl_->modPanel->onExpandDevice = [this, openDeviceWindow](DeviceInstanceId device) {
			if (const device::Descriptor* desc = impl_->deviceHost.descriptorOf(device); desc != nullptr) {
				openDeviceWindow(device, desc);
			}
		};
		impl_->modPanel->onFocusDeviceWindow = [this](DeviceInstanceId device, DeviceNest nest) {
			return impl_->surfaces != nullptr && impl_->surfaces->focusPanelFor(device, nest);
		};
	}

	// Added after every surface so it draws on top of them.
	app->addChild(notify.get());
	impl_->modReadout = std::make_unique<ModReadoutLayer>();
	app->addChild(impl_->modReadout.get()); // the as-modulated readout (backlog PU), over every surface
	app->addChild(impl_->presetToast.get()); // last: over everything, the notify readout included
	// THE SETTINGS SCREEN, built now and hidden; `openSettings` lifts it over everything each time it opens.
	impl_->settings = std::make_unique<SettingsScreen>();
	impl_->settings->uiScaleNow = [this] { return impl_->window.uiScale(); };
	impl_->settings->onUiScale = [this](double scale) { impl_->setUiScale(scale); };
	impl_->settings->onClose = [this] { impl_->closeSettings(); };
	app->addChild(impl_->settings.get(), false);
	impl_->notify = std::move(notify);

	// THE WINDOW'S OWN HANDLERS — Esc and the undo and scale keys at the root, the background, the layout on a resize.
	// In `Impl::installWindowHandlers`, because a rebuilt window (backlog GP) needs the same ones.
	impl_->installWindowHandlers(*app);
	// NOTHING is hung off onWindowContentsResized, and that absence is the fix for a window that
	// would not stop moving. It fires on EVERY size change whatever caused it — the host calling
	// set_size, or the window manager dragging the frame — so answering it with
	// clap_host_gui.request_resize meant every resize the host performed came straight back as a
	// resize the plugin demanded. The host holds one end of that loop and we were holding the other.
	// A guard around set_size would not have been enough either: a window-manager resize arrives
	// nowhere near set_size and echoes just the same.
	//
	// request_resize is for the one thing it means — the editor's own contents now need a different
	// size — and this editor knows exactly one such moment, which is a change of scale. It asks
	// there, in setParent and setScale, and nowhere else.

	// HOW IT WAS LEFT, BEFORE THE FIRST LAYOUT — so the editor is built in the arrangement it will be
	// shown in rather than built open and corrected. The restore goes through setCollapsed, the same
	// door a hand goes through, which is what keeps the panels' hiding and the columns' arithmetic
	// from having a second path they only take on startup. Writing back is harmless here: setFlag
	// stores what is already stored and does not touch the disk.
	impl_->prefs.load();
	if (impl_->engineRail != nullptr) {
		impl_->engineRail->setCollapsed(impl_->prefs.flag(kEngineRailKey));
	}
	if (impl_->fxLane != nullptr) {
		// CLOSED UNTIL SOMEBODY OPENS IT, and this is the one rail whose DEFAULT is not "open" —
		// measured rather than preferred. At the design canvas the left rails take 610 of 1240, so an
		// open 256px lane leaves the stage 374 wide against 534 tall: StageDisk takes min(width,
		// height), so the circle stops being bound by the height and drops from 259 to 179. That is
		//30% of the instrument spent on a chain with nothing in it.
		//
		// Closed, the lane costs exactly nothing — 596 of stage against 534 of height, so the disk is
		// still the height's and the editor is pixel-for-pixel the one that shipped yesterday. The
		// user who wants the lane opens it and keeps it (see the key above); everyone else gets a
		// labelled 34px strip that says where the effects will live.
		//
		// FLIPPED, 2026-08-18: THE DEVICE ARRIVED. The lane holds the multimode filter — a real card
		// with a real curve over real parameters — so the 30% of the instrument that the open lane
		// costs is now buying something, which is exactly the condition the note above set for this
		// word. A user who does not want it still closes it, and the preference above still remembers
		// that they did; only the answer for somebody who has never touched it has changed.
		impl_->fxLane->setCollapsed(impl_->prefs.flag(kFxLaneKey, false));
	}
	// THE STAGE AFTER THE RAILS (backlog NF), so the width a closed stage asks for is taken over the rails as the desk
	// has them — the same door a hand goes through.
	if (impl_->stageRail != nullptr) {
		impl_->stageRail->setCollapsed(impl_->prefs.flag(kStageRailKey, false));
	}
	// THE CANVAS COMES FROM THE PATCH, not the desk — applied BEFORE the first layout, so the size the
	// host is first told is the one the editor will be shown at, and again on every beat (`tick`), so
	// a patch loaded under an open editor moves the partition and folds the band as it was saved.
	impl_->syncCanvasFromPatch();
	if (impl_->modPanel != nullptr) {
		// THE RAIL, AND ONLY THE RAIL. The folds used to be restored here, card by card, out of the desk
		// file; they come with the patch now and the panel reads them from the bridge as it draws
		// (`ModPanel::cardMinimised`), so an editor opened over a loaded patch shows them without being
		// told — and an editor opened over a patch loaded LATER shows them too, which this loop could not.
		impl_->modPanel->setCollapsed(impl_->prefs.flag(kModulatorRailKey));
	}
	// BEFORE THE FIRST LAYOUT, because there is a scale on this frame already and it is not ours:
	// visage stamps its own derivation on a new editor, and every layout run until something
	// overrules it is run at that scale. A host is entitled to ask an editor its size, and to resize
	// it, before it is ever embedded, and this is what makes those answers come out of the same rule
	// the embedded editor uses.
	impl_->window.syncScaleAndLayout();

	// THE HEARTBEAT, from the moment the editor exists. It only actually fires while some visage
	// window is painting — EventManager is checked from Window::drawCallback — so a built but
	// unparented editor costs nothing. setParent starts it again, because this function returns early
	// on every reopen after the first.
	impl_->window.startTicking();
	return true;
}

void Gui::destroy() noexcept {
	// NO ORPHANED PREVIEW, and it happens here rather than in the window — a latched preview is a
	// listening mode whose only control is on a card in THIS editor, so cleaning it up is content
	// teardown. An editor that closed with it on would leave the plugin playing one sound with nothing
	// on screen to say why or turn it off.
	impl_->bridge.clearPreview();
	// THE SPECTRUM PANEL CLOSES WITH THE EDITOR, by its host's road, so the engine stops publishing a plot nobody draws.
	impl_->closeSpectrumPanel();
	// NO ASSIGN MODE ACROSS A CLOSE, for the same reason (backlog KS): it is a gesture at THIS editor,
	// and the editor reopens as the same object. See `ModState::disarmTree`.
	impl_->mod.disarmTree();
	// AND NO PRESET PROMPT, which would keep the keyboard from the host while the editor is shut.
	if (const visage::Frame* top = impl_->window.app(); top != nullptr) {
		(void)ui::PresetBrowser::closePromptsUnder(*top);
	}
	impl_->window.destroy();
}

bool Gui::setParent(const clap_window* window) noexcept {
	return impl_->window.setParent(window);
}

bool Gui::hasWindow() const noexcept {
	return impl_->window.hasWindow();
}

const ui::PluginWindow::XReport& Gui::lastXReport() const noexcept {
	return impl_->window.lastXReport();
}

int Gui::windowRebuilds() const noexcept {
	return impl_->window.rebuilds();
}

bool Gui::setScale(double scale) noexcept {
	return impl_->window.setScale(scale);
}

bool Gui::adjustSize(uint32_t* width, uint32_t* height) const noexcept {
	return impl_->window.adjustSize(width, height);
}

bool Gui::setSize(uint32_t width, uint32_t height) noexcept {
	return impl_->window.setSize(width, height);
}

bool Gui::getSize(uint32_t* width, uint32_t* height) const noexcept {
	impl_->window.size(width, height);
	return true;
}

int Gui::posixFd() const noexcept {
	if (impl_->window.app() == nullptr || impl_->window.app()->window() == nullptr) {
		return -1;
	}
	// X11 ONLY, because only the X11 backend has a connection to pump. The base Window::posixFd
	// answers 0, which on every other backend is not "no fd to poll" but STDIN — and guiSetParent
	// would have the host polling the plugin's standard input: forever-hot on a /dev/null, dead
	// on a closed pipe, and wrong on both. "No fd" is spelled -1.
#if defined(__linux__)
	return impl_->window.app()->window()->posixFd();
#else
	return -1;
#endif
}

void Gui::processEvents() noexcept {
	if (impl_->window.app() == nullptr || impl_->window.app()->window() == nullptr) {
		return;
	}
	// Before the pump, not inside a draw callback. visage only invokes a frame's draw for frames it
	// has marked stale, so anything hung off the top-level frame's onDraw runs once and then never
	// again — which is where this used to be, and it made the engine-to-editor half silently dead
	// after the first frame: automation and preset loads did not appear until something else happened
	// to dirty the window, such as a resize. Here it runs on every callback the host makes, and the
	// rows the diff names are the ones that get marked stale for the pump below to redraw.
	impl_->tickOnce();
	ui::pollKeyboardClaims(); // a click outside the editor ends a text entry's keyboard grab
	impl_->window.app()->window()->processPluginFdEvents();
	// AND THE POPOUT'S OWN CONNECTION, WHICH THE LINE ABOVE CANNOT REACH.
	//
	// The editor is built with a PRIVATE `X11Connection` (visage's embedded constructor makes one per
	// plugin window); a top-level window is built on `X11Connection::globalInstance()`. Two
	// connections, and `processPluginFdEvents` drains the one its window was made on — so every event
	// belonging to a detached panel, its draw requests and the window manager's close among them, sat
	// on a queue nobody read.
	//
	// MEASURED IN A HOST: the popout never drew and its ✕ did nothing. Both are this.
	if (impl_->expandedWindow != nullptr) {
		if (visage::ApplicationWindow* popout = impl_->expandedWindow->windowForTest()) {
			if (popout->window() != nullptr) {
				popout->window()->processPluginFdEvents();
			}
		}
	}
}

double Gui::uiScaleForTest() const noexcept {
	return impl_->window.uiScale();
}

double Gui::contentScaleForTest() const noexcept {
	return impl_->window.contentScaleForTest();
}

void Gui::drawBackgroundForTest(visage::Canvas& canvas) const {
	impl_->drawBackground(canvas);
}

visage::Frame* Gui::root() noexcept {
	// The ApplicationWindow IS the top frame — the one a window delivers an event to, and the one
	// everything else is a child of. No window is needed to answer this, which is the point: the tree
	// exists as soon as create() has built it and layout() has placed it.
	return impl_->window.app();
}

const ui::WidgetRegistry* Gui::widgets() const noexcept {
	return impl_->window.app() == nullptr ? nullptr : &impl_->registry;
}

const ui::FrameWatchers* Gui::stateWatchers() const noexcept {
	return impl_->window.app() == nullptr ? nullptr : &impl_->stateWatchers;
}

Gui::WatcherCounts Gui::watcherCountsForTest() const noexcept {
	WatcherCounts out;
	if (impl_ == nullptr || impl_->window.app() == nullptr) {
		return out;
	}
	out.registry = impl_->registry.totalWatchersForTest();
	out.state = impl_->stateWatchers.count();
	out.preview = impl_->previewWatchers.count();
	out.modulation = impl_->mod.watcherCountForTest();
	out.history = impl_->history.watcherCountForTest();
	out.selection = impl_->selection.watcherCountForTest();
	out.mode = impl_->mode.watcherCountForTest();
	return out;
}

ui::FxLane* Gui::fxLaneForTest() noexcept {
	return impl_ == nullptr ? nullptr : impl_->fxLane.get();
}

ui::StageRail* Gui::stageRailForTest() noexcept {
	return impl_ == nullptr ? nullptr : impl_->stageRail.get();
}

ui::ExploreStage* Gui::exploreForTest() noexcept {
	return impl_ == nullptr ? nullptr : impl_->explore.get();
}

ui::WindowedExpanded* Gui::expandedWindowForTest() noexcept {
	return impl_ == nullptr ? nullptr : impl_->expandedWindow.get();
}

void Gui::setPopoutWindowForTest(bool on) noexcept {
	if (impl_ != nullptr) {
		impl_->prefs.setFlag(kPopoutWindowKey, on);
	}
}

ui::ExpandedOverlay* Gui::expandedForTest() noexcept {
	return impl_ == nullptr || impl_->surfaces == nullptr ? nullptr : &impl_->surfaces->overlay();
}

ui::ExpandedCanvas* Gui::canvasForTest() noexcept {
	return impl_ == nullptr ? nullptr : impl_->canvas.get();
}

ui::HeaderBar* Gui::headerForTest() noexcept {
	return impl_ == nullptr ? nullptr : impl_->header.get();
}

ui::SettingsScreen* Gui::settingsForTest() noexcept {
	return impl_ == nullptr ? nullptr : impl_->settings.get();
}

ui::SurfaceLayers* Gui::surfacesForTest() noexcept {
	return impl_ == nullptr ? nullptr : impl_->surfaces.get();
}

ui::DrumPanel* Gui::drumPanelForTest() noexcept {
	return impl_ != nullptr ? impl_->drumPanel : nullptr;
}

ui::SpectrumPanel* Gui::spectrumPanelForTest() noexcept {
	return impl_ != nullptr ? impl_->spectrumPanel : nullptr;
}

ui::SoundSelection* Gui::selectionForTest() noexcept {
	return impl_ != nullptr ? &impl_->selection : nullptr;
}

ui::ModState* Gui::modForTest() noexcept {
	return impl_ != nullptr ? &impl_->mod : nullptr;
}

Gui::TickReport Gui::tickForTest() noexcept {
	if (impl_ == nullptr || impl_->window.app() == nullptr) {
		return {};
	}
	return impl_->tickOnce();
}

ui::PresetBrowser* Gui::presetBrowserForTest() noexcept {
	return impl_ == nullptr ? nullptr : impl_->presetBrowser.get();
}

ui::NotifyTray* Gui::presetToastForTest() noexcept {
	return impl_ == nullptr ? nullptr : impl_->presetToast.get();
}
void Gui::setPresetToastClockForTest(std::function<int64_t()> clock) {
	if (impl_ != nullptr) {
		impl_->toastClock = std::move(clock);
	}
}
std::string Gui::notifyTextForTest() const noexcept {
	if (impl_ == nullptr || impl_->notify == nullptr) {
		return {};
	}
	return impl_->notify->text();
}

Gui::TickReport Gui::takeDriveReportForTest() noexcept {
	if (impl_ == nullptr) {
		return {};
	}
	const Gui::TickReport report = impl_->driveReport;
	impl_->driveReport = {};
	return report;
}

void Gui::setModifierProbeForTest(int (*probe)()) noexcept {
	if (impl_ != nullptr) {
		impl_->modifierProbe = probe;
	}
}

ui::EditHistory* Gui::history() noexcept {
	return impl_ == nullptr ? nullptr : &impl_->history;
}

#else // no editor backend on this platform

struct Gui::Impl {};

Gui::Gui(GuiHost&, ParamBridge&) noexcept {}
Gui::~Gui() = default;

bool Gui::isAvailable() noexcept {
	return false;
}
bool Gui::isApiSupported(const char*, bool) noexcept {
	return false;
}
bool Gui::create(const char*, bool) noexcept {
	return false;
}
void Gui::destroy() noexcept {}
bool Gui::setParent(const clap_window*) noexcept {
	return false;
}
bool Gui::hasWindow() const noexcept {
	return false;
}
const ui::PluginWindow::XReport& Gui::lastXReport() const noexcept {
	static const ui::PluginWindow::XReport none {};
	return none;
}
int Gui::windowRebuilds() const noexcept {
	return 0;
}
bool Gui::setScale(double) noexcept {
	return false;
}
bool Gui::adjustSize(uint32_t* width, uint32_t* height) const noexcept {
	if (width == nullptr || height == nullptr) {
		return false;
	}
	*width = std::max(*width, kDefaultWidth);
	*height = std::max(*height, kDefaultHeight);
	return true;
}
bool Gui::setSize(uint32_t, uint32_t) noexcept {
	return false;
}
bool Gui::getSize(uint32_t* width, uint32_t* height) const noexcept {
	if (width == nullptr || height == nullptr) {
		return false;
	}
	*width = kDefaultWidth;
	*height = kDefaultHeight;
	return true;
}
int Gui::posixFd() const noexcept {
	return -1;
}
void Gui::processEvents() noexcept {}
double Gui::uiScaleForTest() const noexcept {
	return 1.0;
}
double Gui::contentScaleForTest() const noexcept {
	return 1.0;
}
void Gui::drawBackgroundForTest(visage::Canvas&) const {}
ui::HeaderBar* Gui::headerForTest() noexcept {
	return nullptr;
}
ui::SettingsScreen* Gui::settingsForTest() noexcept {
	return nullptr;
}
visage::Frame* Gui::root() noexcept {
	return nullptr;
}
const ui::WidgetRegistry* Gui::widgets() const noexcept {
	return nullptr;
}
const ui::FrameWatchers* Gui::stateWatchers() const noexcept {
	return nullptr;
}
Gui::WatcherCounts Gui::watcherCountsForTest() const noexcept {
	return {};
}
Gui::TickReport Gui::tickForTest() noexcept {
	return {};
}
Gui::TickReport Gui::takeDriveReportForTest() noexcept {
	return {};
}
void Gui::setModifierProbeForTest(int (*)()) noexcept {}
ui::PresetBrowser* Gui::presetBrowserForTest() noexcept {
	return nullptr;
}
ui::NotifyTray* Gui::presetToastForTest() noexcept {
	return nullptr;
}
void Gui::setPresetToastClockForTest(std::function<int64_t()>) {}
std::string Gui::notifyTextForTest() const noexcept {
	return {};
}
ui::EditHistory* Gui::history() noexcept {
	return nullptr;
}

#endif

} // namespace feathers
