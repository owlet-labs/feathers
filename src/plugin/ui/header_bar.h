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

#include "plugin/gui_bridge.h"
#include "plugin/ui/notify_tray.h"
#include "plugin/ui/preset_browser.h"
#include "plugin/ui/ui_state.h"
#include "plugin/ui/widget_registry.h"

#include <visage/ui.h>

#include <functional>
#include <string>

// The mock's header: wordmark and byline on the left, then the controls that belong to the whole
// plugin rather than to a surface.
//
// What is live and what is not follows the honesty rule, and the split is not arbitrary. The engine
// selector that was here moved to the top of the Engine rail (backlog QH). The
// Explore/Perform switch WAS drawn disabled while it chose between one surface and a plan; both
// surfaces exist now, so it chooses between them. It moves no parameter and stores nothing in the
// patch — see StageMode in ui_state.h — because which view you are looking at is not part of a
// sound.
//
// The mock also carries a UI scale control in the header, a minus/percentage/plus beside the mode
// switch. For a long time it was NOT drawn, because the editor's scale had one resolved source and a
// user-facing control would have been a fourth with no decision on what it did to the order. Backlog EJ
// answered that: the user's scale is one multiplier on top of the display's (`PluginWindow::uiScale`),
// stored in the editor prefs. So the control is drawn now, but only when the editor hands the header
// `uiScaleNow` — the header does not know the scale, it asks, and it steps it by telling.

namespace feathers::ui {

class HeaderBar : public visage::Frame {
public:
	HeaderBar(ParamBridge& bridge, WidgetRegistry& registry, StageMode& mode,
	          Notifier* notifier = nullptr);

	/// **OFF THE REGISTRY'S LIST BEFORE THE MEMORY GOES.** This widget subscribes in its constructor
	/// and must take itself off — the rule `Knob::~Knob` states for the modulation list, applied to the
	/// parameter one.
	///
	/// FOUND UNDER ASan, WHERE `~FrameWatchers`'s ASSERT IS LIVE. `NDEBUG` makes it compile to nothing
	/// in the ordinary build, so `ctest` was green throughout — the same blindness §17.3 of the rack
	/// MVP doc records about that same assert.
	///
	/// `WidgetRegistry::unwatch` sweeps every parameter rather than taking an id, deliberately: a
	/// frame being destroyed does not remember what it subscribed to, and asking it to would be the
	/// bookkeeping that produced the bug. So this is correct even for the constructor overload that
	/// never subscribed.
	~HeaderBar() override;

	void draw(visage::Canvas& canvas) override;

	/// The header's height in logical pixels, from the mock's own padding and type sizes.
	static constexpr float kHeight = 52.0f;

	/// Whether a point lands on either half of the Explore/Perform switch, and which. Public for the
	/// same reason hitsEngine is: a test that cannot ask this can only click and hope.
	bool hitsMode(visage::Point position) const { return modeBounds().contains(position); }
	StageMode::Surface surfaceAt(visage::Point position) const;

	/// Whether a point lands on the preset browser's prev, next, or name box, and which. Public for
	/// the same reason the other hits are: a test that cannot ask this can only click and hope.
	bool hitsPresetPrev(visage::Point position) const;
	bool hitsPresetNext(visage::Point position) const;
	bool hitsPresetName(visage::Point position) const;

	/// The preset browser, if one is set. The header does not own it — the editor does, and sets it
	/// here so the header can forward mouse events to it. Nullable because the tests build headers
	/// without one.
	PresetBrowser* presetBrowser() const { return preset_; }
	void setPresetBrowser(PresetBrowser* browser) { preset_ = browser; }

	//-- THE UI SCALE READOUT (backlog EJ, second road: a small readout and +/- buttons at the
	//-- header's right). The keyboard road needs the plugin window to
	//-- hold focus first, which a host only grants after a click inside it; this road is a click. The
	//-- header only ASKS and TELLS — the editor owns the scale and the file.
	/// Called with +1 / -1 when a button is clicked or the header is wheeled with Ctrl/Cmd held.
	std::function<void(int direction)> onUiScaleStep;
	/// What the readout shows: the scale in force, 1.0 = 100 %. Absent, the readout is not drawn.
	std::function<double()> uiScaleNow;
	/// The whole widget, and its two buttons — the minus at the left end, the plus at the right.
	visage::Bounds uiScaleBounds() const;
	visage::Bounds uiScaleMinusBounds() const;
	visage::Bounds uiScalePlusBounds() const;
	bool hitsUiScaleMinus(visage::Point position) const { return uiScaleMinusBounds().contains(position); }
	bool hitsUiScalePlus(visage::Point position) const { return uiScalePlusBounds().contains(position); }

	//-- THE LIBRA CONSTELLATION (centred on the title header, white stroke and fill). Owlet's mark, from `assets/branding/libra_constellation.svg`, embedded at build
	//-- time. It was decorative first; since backlog LG a click on it opens the settings screen (`onSettings`).
	//-- It sits in the gap between the preset browser and the UI scale readout,
	//-- and where a scale the host could not make room for squeezes that gap shut, it is not drawn.
	/// Where the mark is drawn, in the header's logical coordinates: centred in the strip both ways. EMPTY
	/// (zero size) when the header is too narrow for it to sit there clear of the preset browser and the
	/// right-hand controls — then it is not drawn.
	visage::Bounds logoBounds() const;
	/// **THE MARK OPENS THE SETTINGS SCREEN** (backlog LG: "we can also put an about/settings
	/// screen on a logo click"). A click on the drawn mark, or on the FEATHERS wordmark — the second because the
	/// mark is not drawn where a scale squeezes the header (`logoBounds` empty), and a screen that holds the
	/// scale must not become unreachable at the scales that hide its door. Absent, neither is a click target.
	std::function<void()> onSettings;
	/// The wordmark's box, the other door.
	visage::Bounds wordmarkBounds() const;
	bool hitsSettings(visage::Point position) const;
	/// The Explore/Perform switch's box, for a case that checks nothing is drawn over it.
	visage::Bounds modeBoundsForTest() const { return modeBounds(); }
	/// TEST AND TOOL ONLY: the switch's own road to a surface, without a click (widget-shot's `--explore`).
	void showSurfaceForTest(StageMode::Surface surface) { mode_.show(surface); }

private:
	/// **THE SUBSCRIPTIONS, HELD** — see `ui::Watch`. Declared last so they are released FIRST.
	std::vector<Watch> watches_;
	visage::Bounds modeBounds() const;

	/// The mark, rasterised at its NATIVE pixel size and kept until that size moves (a scale step or a
	/// resize). The buffer's identity is its content: visage's image atlas keys a raw image on its pointer
	/// and size, and the pixels here are a function of the size alone.
	void drawLogo(visage::Canvas& canvas);
	std::vector<uint32_t> logoPixels_;
	uint64_t logoSerial_ = 0; ///< `freshPixelSerial`, stamped when `logoPixels_` is rasterised
	int logoWidth_ = 0;
	int logoHeight_ = 0;

	WidgetRegistry& registry_;
	StageMode& mode_;
	PresetBrowser* preset_ = nullptr;
	Notifier* notifier_ = nullptr;
};

} // namespace feathers::ui
