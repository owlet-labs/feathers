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

#include "plugin/editor_prefs.h"
#include "plugin/ui/container_presets.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/emitters_front.h"
#include "plugin/ui/full_redraw.h"
#include "plugin/ui/escape_stack.h"
#include "plugin/ui/mod_state.h"
#include "plugin/ui/plugin_window.h"
#include "plugin/ui/rail.h"
#include "rack/rack_editor.h"
#include "rack/rack_state.h"

#include <algorithm>
#include <functional>
#include <memory>

// THE RACK'S EDITOR — a `PluginWindow` with a `RackEditor` in it, and almost nothing else.
//
// IT IS SHORT BECAUSE THE WINDOW WAS EXTRACTED FIRST. Everything that goes wrong with a plugin window
// — the Mesa WSI deadlock, the re-entrant `set_parent`, reparent-to-root — is in `PluginWindow` and
// was paid for once, against a real host, by Feathers. This file answers the six `Client` questions
// and holds the contents.
//
// **THAT IS THE ONLY REASON THIS IS SAFE TO WRITE AT ALL.** A second window shell would have been a
// second copy of that knowledge, and its failure mode is not a compile error — it is a host wedging
// on somebody's machine months later.

namespace feathers::ui {
class PresetRootWatch;
class TooltipDetailWatch;
class ChromeHueWatch;
class AccentHueWatch;
class WindowControlsSideWatch;
class SettingsScreen;
}

namespace feathers::rack {

class RackBridge;

class RackGui final : public ui::PluginWindow::Client {
public:
	explicit RackGui(RackBridge& bridge);
	~RackGui() override;

	static bool isAvailable() noexcept { return ui::PluginWindow::isAvailable(); }
	static bool isApiSupported(const char* api, bool isFloating) noexcept {
		return ui::PluginWindow::isApiSupported(api, isFloating);
	}

	bool create(const char* api, bool isFloating) noexcept;
	/// WHAT THE EDITOR'S ADD MENU OFFERS (`RackEditor::offerFilter`), handed to each editor this builds — the Note Rack's narrowing.
	std::function<bool(const device::Descriptor&)> offerFilter;
	void destroy() noexcept;
	bool setParent(const clap_window* window) noexcept;
	bool hasWindow() const noexcept { return window_.hasWindow(); }
	/// What the X server answered the last embed or detach (backlog GQ), and the rebuilds a host's early container
	/// destroy has cost (backlog GP) — see `PluginWindow`.
	const ui::PluginWindow::XReport& lastXReport() const noexcept { return window_.lastXReport(); }
	int windowRebuilds() const noexcept { return window_.rebuilds(); }
	bool setScale(double scale) noexcept { return window_.setScale(scale); }
	bool adjustSize(uint32_t* w, uint32_t* h) const noexcept { return window_.adjustSize(w, h); }
	bool setSize(uint32_t w, uint32_t h) noexcept { return window_.setSize(w, h); }
	bool getSize(uint32_t* w, uint32_t* h) const noexcept;

	/// THE HOST HAS TO PUMP THIS EDITOR OR IT NEVER PAINTS — see PluginWindow::processEvents.
	int posixFd() const noexcept { return window_.posixFd(); }
	void processEvents() noexcept { window_.processEvents(); }

	/// HOW THE EDITOR ASKS ITS HOST FOR A SIZE. Installed by the plugin, because only the plugin holds
	/// a `clap_host`. Absent is fine and means the editor never asks — which is what a venue gets.
	void setResizeRequester(std::function<void(uint32_t, uint32_t)> request) {
		requestResize_ = std::move(request);
	}

	/// THE ASSEMBLED SURFACE, for a venue. `Gui::root()` is the same seam and exists for the same
	/// reason: a test that reached into the window for the editor would be finding a frame rather than
	/// the one this class actually built.
	RackEditor* editorForTest() noexcept { return editor_.get(); }
	/// The rack plugin's own preset browser, or null with no editor (backlog QC's cases read its face).
	ui::PresetBrowser* presetBrowserForTest() noexcept { return presets_.browser.get(); }
	/// The editor's modulation state, which outlives every close with the editor (backlog KS).
	ui::ModState& modForTest() noexcept { return mod_; }
	/// The global UI scale in force (backlog EJ) — the same preference Feathers' editor reads.
	double uiScaleForTest() const noexcept { return window_.uiScale(); }
	double contentScaleForTest() const noexcept { return window_.contentScaleForTest(); }
	/// The window the editor is IN, for a case asking whether a frame reached the live window (backlog GP).
	const visage::ApplicationWindow* appForTest() const noexcept { return window_.app(); }
	/// THE SETTINGS SCREEN (backlog LG), opened from the Libra mark at the head of the macro row.
	ui::SettingsScreen* settingsForTest() noexcept { return settings_.get(); }

	/// SAY A LOAD'S REFUSALS, if there is a surface to say them on.
	///
	/// QUEUED WHEN THERE IS NOT. A patch is loaded long before an editor exists — that is the usual
	/// order, not an edge case — so a refusal that could only be said to an open editor would be a
	/// refusal nobody ever hears. It is held and said on the next open.
	///
	/// **THE WHOLE LOAD REPORT SINCE BACKLOG JF** (`LoadReport`), of which the refused routes are one kind;
	/// one tray line, `loadReportLine`'s.
	void sayLoadReport(const LoadReport& report);

	/// THE CHAIN CHANGED — rebuild the card stack. Called by the plugin when a device is added or
	/// removed, because the editor cannot see the engine's verbs from here.
	void syncCards();

	// ui::PluginWindow::Client
	void layout() override;
	void tick() override;
	/// THE LANE'S HEIGHT PLUS THE CANVAS'S — the canvas is a band above the macros, room the editor
	/// asks the host for, not room taken from the lane. Before
	/// the editor exists the canvas is open, which is its default, so the first size a host is told
	/// already has it.
	uint32_t logicalWidth() const override { return RackEditor::kWidth; }
	uint32_t logicalHeight() const override { return RackEditor::kHeight + canvasExtent(); }
	uint32_t minimumWidth() const override { return RackEditor::kWidth; }
	uint32_t minimumHeight() const override { return RackEditor::kHeight + canvasExtent(); }
	uint32_t canvasExtent() const noexcept {
		if (editor_ != nullptr) {
			return static_cast<uint32_t>(editor_->canvasExtent());
		}
		// BEFORE THE EDITOR EXISTS, the canvas is as a fresh one is: folded (the strip) by default.
		// Open, it is the band a fresh editor's canvas opens at: `kExtent` under the lane's cap (`defaultExtent`).
		return static_cast<uint32_t>(ui::ExpandedCanvas::kDefaultCollapsed
		                                 ? ui::Rail::kCollapsedWidth
		                                 : std::min(ui::ExpandedCanvas::kExtent,
		                                            static_cast<float>(RackEditor::kHeight) * ui::ExpandedCanvas::kMaxShare));
	}
	void requestResize(uint32_t width, uint32_t height) override;
	/// THE WINDOW WAS REBUILT (backlog GP): the same handlers `create` hangs on the first one.
	void windowRebuilt(visage::ApplicationWindow& app) override { installWindowHandlers(app); }

private:
	/// THE HANDLERS THE RACK HANGS ON ITS WINDOW — the background and the UI-scale keys — at the first build and on a
	/// rebuilt window alike.
	void installWindowHandlers(visage::ApplicationWindow& app);

	/// **THE EDITOR'S HOST, OWNED BY THE PLUGIN.** In 1a this class owned the bridge, because a
	/// forwarder needs nothing more. The mirrors moved ownership up: they are published from
	/// `process`, and this class does not exist between editor closes. So the plugin owns it and hands
	/// it here — Feathers' shape exactly, where `ParamBridge` outlives every editor.
	///
	/// **AND THE RACK'S UI NOW NAMES THE ENGINE NOWHERE AT ALL** (AS 1a's guarantee, widened).
	RackBridge& bridge_;
	ui::ModState mod_;
	ui::EditHistory history_;
	/// THE PLUGIN'S PRESET BROWSER (backlog OU), lent to the editor — declared BEFORE it, so the editor goes first.
	ui::ContainerPresets presets_;
	std::unique_ptr<RackEditor> editor_;
	/// ASSIGN MODE ON THE EDITOR'S ESC STACK (backlog NN). After `editor_`, whose layers hold the stack, so it
	/// comes off first.
	ui::ArmEscape armEscape_;
	/// THE SETTINGS SCREEN'S ENTRY on the same stack, while it shows.
	ui::EscapeTicket settingsEscape_;
	ui::PluginWindow window_ { *this };
	/// The desk's preferences — read for the global UI scale at create, written when a key steps it.
	EditorPrefs prefs_;
	/// Keeps the chosen preset root in `prefs_` — a device panel in this rack may be where it was chosen,
	/// and a panel has no prefs (`preset_browser.h`). After `prefs_`, so it goes first.
	std::unique_ptr<ui::PresetRootWatch> presetRootWatch_;
	/// Keeps the tooltip detail in `prefs_` (`tooltip_prefs.h`). After `prefs_`, so it goes first.
	std::unique_ptr<ui::TooltipDetailWatch> tooltipDetailWatch_;
	/// Keeps the chrome hue in `prefs_` and repaints this editor when it moves (`chrome_hue_prefs.h`, backlog LG).
	std::unique_ptr<ui::ChromeHueWatch> chromeHueWatch_;
	/// The same for the ACCENT HUE, the second axis.
	std::unique_ptr<ui::AccentHueWatch> accentHueWatch_;
	/// Keeps the window-controls side in `prefs_` (`window_controls_side_prefs.h`, backlog OD).
	std::unique_ptr<ui::WindowControlsSideWatch> windowControlsSideWatch_;
	std::unique_ptr<ui::EmittersFrontWatch> emittersFrontWatch_; ///< arps sent to front of chain
	std::unique_ptr<ui::FullRedrawWatch> fullRedrawWatch_; ///< repaint partial or every frame (`full_redraw.h`)
	/// THE SETTINGS SCREEN, built with the editor and hidden until the door opens it. Hidden rather than
	/// destroyed on close, because the close arrives from inside its own mouse handler.
	std::unique_ptr<ui::SettingsScreen> settings_;
	/// Apply, persist and say a UI scale — the keys' road and the settings screen's, one place.
	void setUiScale(double next);
	void openSettings();
	std::function<void(uint32_t, uint32_t)> requestResize_;
	LoadReport pending_ {};
};

} // namespace feathers::rack
