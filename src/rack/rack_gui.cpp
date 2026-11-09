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

#include "rack/rack_gui.h"

#include "rack/chain_expanded.h"

#include "rack/rack_bridge.h"
#include "plugin/editor_prefs.h"
#include "plugin/ui/preset_browser.h"
#include "plugin/ui/tooltip_prefs.h"
#include "plugin/ui/chrome_hue_prefs.h"
#include "plugin/ui/window_controls_side_prefs.h"
#include "plugin/ui/emitters_front_prefs.h"
#include "plugin/ui/settings_screen.h"

#include "plugin/ui/theme.h"
#include "rack/rack_engine.h"


#include <cmath>
#include <string>

#if defined(FEATHERS_GUI_VISAGE)
#include <visage/app.h>
#endif

namespace feathers::rack {

RackGui::RackGui(RackBridge& bridge) : bridge_(bridge) {}

RackGui::~RackGui() = default;

bool RackGui::create(const char* api, bool isFloating) noexcept {
	// THE CHAIN DEVICE'S EXPANDED VIEW, filed before any card can ask for it (a hook, not an include).
	registerChainExpandedView();
	// THE GLOBAL UI SCALE, BEFORE THE WINDOW EXISTS (backlog EJ) — the same key Feathers' editor reads,
	// so the two plugins and every instance come up at one size. See Gui::create for the ordering.
	prefs_.load();
	window_.setUiScale(prefs_.number(ui::PluginWindow::kUiScaleKey, 1.0));
	// THE CHOSEN PRESET ROOT, as Feathers' editor reads and keeps it — see Gui::create.
	ui::restorePresetRoot(prefs_);
	if (presetRootWatch_ == nullptr) {
		presetRootWatch_ = ui::keepPresetRootIn(prefs_);
	}
	// THE TOOLTIP DETAIL, likewise.
	ui::restoreTooltipDetail(prefs_);
	if (tooltipDetailWatch_ == nullptr) {
		tooltipDetailWatch_ = ui::keepTooltipDetailIn(prefs_);
	}
	// THE CHROME HUE (backlog LG), likewise, and a repaint of the whole window when it moves.
	// THE WINDOW-CONTROLS SIDE (backlog OD), likewise.
	ui::restoreWindowControlsSide(prefs_);
	if (windowControlsSideWatch_ == nullptr) {
		windowControlsSideWatch_ = ui::keepWindowControlsSideIn(prefs_);
	}
	// ARPS SENT TO FRONT OF CHAIN, likewise.
	ui::restoreEmittersToFront(prefs_);
	if (emittersFrontWatch_ == nullptr) {
		emittersFrontWatch_ = ui::keepEmittersToFrontIn(prefs_);
	}
	// REPAINT, PARTIAL OR EVERY FRAME , likewise.
	ui::restoreFullRedraw(prefs_);
	if (fullRedrawWatch_ == nullptr) {
		fullRedrawWatch_ = ui::keepFullRedrawIn(prefs_, [this] {
#if defined(FEATHERS_GUI_VISAGE)
			if (visage::ApplicationWindow* top = window_.app(); top != nullptr) {
				top->redrawAll();
			}
#endif
		});
	}
	ui::restoreChromeHue(prefs_);
	if (chromeHueWatch_ == nullptr) {
		chromeHueWatch_ = ui::keepChromeHueIn(prefs_, [this] {
#if defined(FEATHERS_GUI_VISAGE)
			if (visage::ApplicationWindow* top = window_.app(); top != nullptr) {
				top->redrawAll();
			}
#endif
		});
	}
	ui::restoreAccentHue(prefs_);
	if (accentHueWatch_ == nullptr) {
		accentHueWatch_ = ui::keepAccentHueIn(prefs_, [this] {
#if defined(FEATHERS_GUI_VISAGE)
			if (visage::ApplicationWindow* top = window_.app(); top != nullptr) {
				top->redrawAll();
			}
#endif
		});
	}
	bool built = false;
	if (!window_.create(api, isFloating, &built)) {
		return false;
	}
	if (!built) {
		return true; // a reopen: the window and its contents are already there
	}
#if defined(FEATHERS_GUI_VISAGE)
	visage::ApplicationWindow* app = window_.app();
	if (app == nullptr) {
		return false;
	}
	// THE ONE THING THE EXTRACTION'S OWN BUG WAS ABOUT: build the contents into the window the shell
	// just made, and keep them. `Gui::create` dropped its window because the assignment that kept it
	// was the last line after the contents; the shell owns that now and this only has to hold the
	// editor.
	// THE TOP-LEVEL FRAME NEEDS A DRAW FUNCTION OR NOTHING DRAWS AT ALL — not the window, and not its
	// children either. `Gui::create` has always done this (`app->onDraw() = ... drawBackground`) and
	// the rack did not, which is the whole of why its editor came up as a black rectangle: `draw()`
	// was CALLED on the editor, at the right size, visible, with a parent — and every canvas operation
	// went nowhere.
	//
	// FOUND BY INSTRUMENTING RATHER THAN BY READING, after four wrong guesses: the diagnostics said
	// resized ran with 8 knobs, draw ran at 720x560, and knob 0 sat at 38,12 34x86 and visible. Every
	// fact about the tree was right, and the capture was one flat colour — 0,0,0, not even the
	// surface token's 7,10,18. A frame can be correct in every particular and still be attached to a
	// window that never paints.
	installWindowHandlers(*app);

	editor_ = std::make_unique<RackEditor>(bridge_, mod_, history_);
	editor_->offerFilter = offerFilter;
	// ASSIGN MODE ON THE EDITOR'S ESC STACK (backlog NN) — the rack's whole follow-tree, nested chains included.
	armEscape_.attach(mod_, editor_->escapeStack());
	// THE CANVAS OPENING OR CLOSING IS THE ONE MOMENT THIS EDITOR'S PREFERRED SIZE CHANGES — so it
	// is the one moment, besides a change of scale, that the host is asked for a size.
	editor_->onCanvasExtentChanged = [this] { window_.preferredSizeChanged(); };
	editor_->windowRoomHeight = [this] { return window_.logicalRoomHeight(); };
	// A DRAG STEP PUTS THE PARTITION UNDER THE POINTER AND KEEPS THE LANE WHOLE: the band is across the
	// TOP, so the canvas's physical share is the pointer's distance from the top; the lane below keeps
	// its design height and the scale pays (`ExpandedCanvas::partitionExtent`). This stops it from happening.
	editor_->onCanvasDrag = [this](visage::Point pointer) {
		const int nativeH = window_.nativeHeight();
		const double preferred = window_.preferredScale();
		if (nativeH <= 0 || preferred <= 0.0 || window_.app() == nullptr) {
			return;
		}
		// In host units, like nativeH — see PluginWindow::contentScale.
		const float canvasNative = pointer.y * static_cast<float>(window_.contentScale());
		// CAPPED AT THE LANE'S OWN HEIGHT (`kMaxShare`): the canvas is never more than the rack beside it.
		(void)editor_->canvas().setOpenExtent(ui::ExpandedCanvas::partitionExtent(
		    static_cast<float>(RackEditor::kHeight), static_cast<float>(nativeH), canvasNative,
		    static_cast<float>(preferred), ui::ExpandedCanvas::kMinExtent, editor_->canvas().maxExtent()));
		window_.syncScaleAndLayout();
	};
	app->addChild(editor_.get());
	// THE SETTINGS SCREEN (backlog LG): built now and hidden, opened by the Libra mark the macro row makes
	// room for. It covers the whole window — the canvas band included — when it is open.
	settings_ = std::make_unique<ui::SettingsScreen>();
	settings_->uiScaleNow = [this] { return window_.uiScale(); };
	settings_->onUiScale = [this](double scale) { setUiScale(scale); };
	settings_->onClose = [this] {
		if (settings_ != nullptr) {
			settings_->setVisible(false);
		}
		settingsEscape_.release();
	};
	app->addChild(settings_.get(), false);
	editor_->setSettingsDoor([this] { openSettings(); });
	// THE PLUGIN'S PRESETS (backlog OU), beside the door: a chain preset is this plugin's whole state. After a load the
	// cards follow the record at once, as after a state load.
	presets_ = ui::makeTopPresets(bridge_, &editor_->notifier(), [this] { syncCards(); });
	editor_->setPresetBrowser(presets_.browser.get());
	// ANYTHING A LOAD REFUSED BEFORE THERE WAS A SURFACE gets said now.
	if (!pending_.empty()) {
		const LoadReport held = pending_;
		pending_ = LoadReport {};
		sayLoadReport(held);
	}
	window_.syncScaleAndLayout();
	window_.startTicking();
#endif
	return true;
}

#if defined(FEATHERS_GUI_VISAGE)
void RackGui::installWindowHandlers(visage::ApplicationWindow& app) {
	app.onDraw() = [](visage::Canvas& canvas) {
		canvas.setColor(ui::kSurface);
		canvas.fill(0, 0, canvas.width(), canvas.height());
	};
	// THE UI SCALE ON THE KEYS A BROWSER USES — Ctrl/Cmd with = / + up, - down, 0 reset — the same
	// gesture as Feathers' editor, persisted through the same preference (backlog EJ).
	//
	// **AND ESC LEAVES MODULATION ASSIGN MODE** (backlog NI) — Feathers' root rule (`Gui::Impl::escapeTop`), here. NEITHER HANDLER COULD FIRE BEFORE THIS:
	// the window accepted no keystrokes, so `handleKeyDown`'s walk up from the focus skipped it, and a key
	// `RackEditor` declined (it takes Delete and Backspace only) went nowhere. Accepting keys at the root is what
	// makes it the terminus every press ends at; a key nobody here wants still goes back to the host.
	//
	// THE WHOLE FOLLOW-TREE, NOT ONLY THIS STATE: an arm inside a nested chain is the hand's one arm too
	// (`ModState::arm`), and it is the one a player would be stuck in. Esc with nothing armed is not ours.
	app.setAcceptsKeystrokes(true);
	app.onKeyPress() = [this](const visage::KeyEvent& e) {
		if (e.isCtrlDown() || e.isCmdDown()) {
			const int direction = ui::PluginWindow::uiScaleStepFor(static_cast<int>(e.keyCode()));
			if (direction == 2) {
				return false;
			}
			setUiScale(ui::PluginWindow::stepUiScale(window_.uiScale(), direction));
			return true;
		}
		if (e.keyCode() != visage::KeyCode::Escape) {
			return false;
		}
		// **THE MOST RECENTLY OPENED THING, WHATEVER KIND IT IS** (backlog NN) — the arm is one entry on the
		// editor's Esc stack beside its windows, browsers, prompt and settings, in the order they opened.
		return editor_ != nullptr && editor_->escapeStack().closeTop();
	};
}

void RackGui::setUiScale(double next) {
	window_.setUiScale(next);
	prefs_.setNumber(ui::PluginWindow::kUiScaleKey, next);
	if (editor_ != nullptr) {
		editor_->notifier().say("UI scale " + std::to_string(static_cast<int>(std::lround(next * 100.0))) + "%");
	}
}

void RackGui::openSettings() {
	visage::ApplicationWindow* app = window_.app();
	if (settings_ == nullptr || app == nullptr) {
		return;
	}
	// LIFTED OVER EVERYTHING each time it opens — whatever was added since create is under it.
	if (settings_->parent() != nullptr) {
		settings_->parent()->removeChild(settings_.get());
	}
	app->addChild(settings_.get());
	settings_->setBounds(0.0f, 0.0f, static_cast<float>(app->width()), static_cast<float>(app->height()));
	settings_->setVisible(true);
	settings_->redraw();
	// ON THE ESC STACK while it shows (backlog NN); opening it again while open brings it to the top.
	if (settingsEscape_.held()) {
		settingsEscape_.raise();
	}
	else if (editor_ != nullptr) {
		settingsEscape_ = editor_->escapeStack().push([this] {
			if (settings_ == nullptr || !settings_->isVisible() || !settings_->onClose) {
				return false;
			}
			settings_->onClose();
			return true;
		});
	}
}
#else
void RackGui::installWindowHandlers(visage::ApplicationWindow&) {}
#endif

void RackGui::destroy() noexcept {
	// NOTHING OF THE WINDOW IS TORN DOWN — see PluginWindow. The editor stays built too, because it is
	// the window's child and a reopen reuses both. WHICH IS WHY ASSIGN MODE IS ENDED HERE (backlog KS): a
	// state that is never rebuilt would otherwise reopen armed. See `ModState::disarmTree`.
	mod_.disarmTree();
	// AND NO PRESET PROMPT LEFT HOLDING THE KEYBOARD — see `PresetBrowser::closePromptsUnder`.
	if (const visage::Frame* top = window_.app(); top != nullptr) {
		(void)ui::PresetBrowser::closePromptsUnder(*top);
	}
	window_.destroy();
}

bool RackGui::setParent(const clap_window* window) noexcept {
	return window_.setParent(window);
}

bool RackGui::getSize(uint32_t* w, uint32_t* h) const noexcept {
	if (w == nullptr || h == nullptr) {
		return false;
	}
	window_.size(w, h);
	return true;
}

void RackGui::sayLoadReport(const LoadReport& report) {
	if (report.empty()) {
		return;
	}
	if (editor_ == nullptr) {
		// HELD, NOT DROPPED — see the header. The editor is usually built after the patch.
		pending_ = report;
		return;
	}
	const RouteRefusals& refused = report.routes;
	// THE FIRST IN FULL AND THE REST COUNTED, which is what `RouteRefusals` is shaped for: the tray is
	// one line and a later message replaces an earlier one, so N of them would show only the last.
	//
	// **THE FIRST REFUSAL'S DESTINATION, NAMED FROM THE ENDPOINT** (kill-lanes step 3b). It read a
	// COORDINATE and decoded a pool slot out of it; a refusal names a DEVICE now, so the record is simply
	// asked where that device stands — and a device it does not hold is an honest sentence rather than a
	// number one past an axis standing in for one.
	//
	// **THE ENDPOINT IS WHAT THE PATCH ASKED FOR, NOT WHAT THE LOAD FOUND** (`RouteRefusals`), so this says
	// what the route NAMED — which is why the wording is "first named" and not "first at". Where the device
	// is here, that is also where it stands; where it is not, the route named something this rack does not
	// hold, and saying so is the whole message.
	//
	// **THE POSITION THE HAND SEES, AND THE DEVICE'S OWN CONTROL INDEX.** The old line said the pool slot,
	// which was never where the card was; and it said the MODULATABLE ORDINAL, which counts only the
	// controls that can be routed to and so skips past the ones a reader is counting on the card. Both are
	// now the numbers on screen.
	std::string where;
	if (refused.count > 0) {
		where = "first named something this rack does not have";
		if (refused.firstDestination.isDevice()) {
			const size_t at = bridge_.positionOf(refused.firstDestination.device);
			where = at != RackBridge::kNoSlot ? "first named device " + std::to_string(at + 1) + ", control "
			                                        + std::to_string(refused.firstDestination.index + 1)
			                                  : "first named a device this rack does not hold";
		}
	}
	editor_->notifier().say(loadReportLine(report, where));
	pending_ = LoadReport {};
}

void RackGui::syncCards() {
	if (editor_ != nullptr) {
		editor_->syncCards();
	}
}

void RackGui::layout() {
#if defined(FEATHERS_GUI_VISAGE)
	visage::ApplicationWindow* app = window_.app();
	if (app == nullptr || editor_ == nullptr) {
		return;
	}
	editor_->setBounds(0, 0, app->width(), app->height());
	if (settings_ != nullptr) {
		settings_->setBounds(0, 0, app->width(), app->height());
	}
#endif
}

void RackGui::tick() {
	// **THE MESSAGE THREAD'S SERVICE, ON THE BEAT** (backlog CF): the drain parks what it removes and
	// never frees; this is where the parked devices are destroyed, and where a lane the engine could not
	// take is built again. There is no "an edit did not land" to say any more: the record IS the edit,
	// and the engine keeps converging to it.
	bridge_.reapRetired();
	// AND WHAT THAT SERVICE DROPPED INSIDE A CHAIN IS SAID (rack roadmap §7, hygiene): an outer source
	// removed or moved downstream takes its inner routes with it, and the tray says so.
	sayLoadReport(bridge_.takeLoadReport());
	// AND A HOST SLOT A HAND'S TOUCH RECYCLED (host slots T9, design §2): the last resort is said, once no hand
	// is down (the bridge holds it until then).
	if (editor_ != nullptr) {
		if (const std::string slot = bridge_.takeSlotNotice(); !slot.empty()) {
			editor_->notifier().say(slot);
		}
	}
	// THE CHAIN CAN CHANGE FROM OUTSIDE THE EDITOR — a patch load, or a host driving the test
	// extension — so the card stack is reconciled on the beat rather than only when this surface is
	// the one that changed it. `syncCards` compares before building, so a beat with nothing new costs
	// a walk over the slots and no allocation.
	syncCards();
	if (editor_ != nullptr) {
		editor_->syncMacros();
		editor_->syncCanvas();
		// ADVANCE WHAT MOVES ON ITS OWN. **THE RACK HAD NO SUCH LINE**, and that is the whole of what
		// Reported: an OWLTT analyser that animated only while the mouse was over it — visage
		// repainting on input, with no clock behind it — and Automod curves that never moved at all,
		// because that card's strip advances IN `update()`, so a repaint alone redraws an unadvanced
		// state. Feathers drives four of these out of `Gui::Impl::tick()`; the rack drove none.
		editor_->updateCards();
		// AND A CARD HELD AT THE LANE'S EDGE SCROLLS IT (backlog DB) — this beat is the only clock a still
		// pointer has.
		(void)editor_->tickDrag();
	}
}

void RackGui::requestResize(uint32_t width, uint32_t height) {
	if (requestResize_) {
		requestResize_(width, height);
	}
}

} // namespace feathers::rack
