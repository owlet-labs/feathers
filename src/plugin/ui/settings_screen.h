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

#include <visage/ui.h>

#include <cstdint>
#include <functional>
#include <span>
#include <string>

// THE SETTINGS SCREEN (backlog LG: "settings screen and a setting in there to tweak
// the color centers"; 2026-10-05: "we can also put an about/settings screen on a logo click").
//
// ONE SCREEN, TWO PARTS, over a scrim that covers the whole editor:
//
//     +----------------------------------------------------------------+
//     | FEATHERS  settings                                          ×  |
//     |----------------------------------------------------------------|
//     | UI SCALE                            | ABOUT                    |
//     | [100|113|125|138|150|175|200]       | Feathers 0.1.0           |
//     | TOOLTIPS                            | build 1a2b3c4d5e6f       |
//     | [ BRIEF | FULL ]                    | Owlet Records            |
//     |   ...                               | DEBUG OPTIONS            |
//     |                                     | REPAINT [PARTIAL|EVERY.] |
//     |                                     | (diagnostic: frame times)|
//     | CHROME HUE                    +120° |                          |
//     | [=========hue ramp====|=========]   | CREDITS                  |
//     | -180°          0            +180°   | (from THIRD-PARTY-       |
//     | ACCENT HUE                     +45° |  NOTICES.md, generated)  |
//     | [=======accent ramp=|===========]   |                          |
//     | -180°          0            +180°   |                          |
//     | [swatches: what each strip turns]   |                          |
//     +----------------------------------------------------------------+
//
// THE TWO HUES COMPOSE (`chrome_hue.h`): Chrome Hue turns everything chrome; Accent Hue turns the role
// family (accent, value, modulation) further, so the roles sit at chrome + accent. Each strip is painted
// in the accent its x would give, with the other strip's offset as it stands.
//
// EVERY CHOICE IS ON THE FACE BEFORE IT IS APPLIED (CLAUDE.md: a multivalue toggle is an antipattern):
// the scale's seven rungs and the tooltip detail's two are segments, and the hue strip is painted in the
// accent AT EACH OFFSET it would choose, so the strip is a picture of the choice.
//
// WHAT IT HOLDS IS PER-USER AND NEVER IN THE PATCH: every setting here lives in the editor prefs
// (`editor_prefs.h`). The screen owns none of it — the UI scale is asked for and told through the two
// functions below (the editor owns the window and the file); the tooltip detail and the chrome hue are
// process-global (`tooltip.h`, `chrome_hue.h`) and their watches write the prefs and repaint.
//
// Closed by the × or a click on the scrim outside the panel. Both editors hold one and show it from the
// Libra mark: Feathers' header, and the rack's macro row.

namespace feathers::ui {

class SettingsScreen : public visage::Frame {
public:
	SettingsScreen();

	void draw(visage::Canvas& canvas) override;
	void mouseDown(const visage::MouseEvent& e) override;
	void mouseDrag(const visage::MouseEvent& e) override;
	void mouseUp(const visage::MouseEvent& e) override;
	void mouseMove(const visage::MouseEvent& e) override;
	void mouseExit(const visage::MouseEvent& e) override;

	//-- THE EDITOR'S ROADS.
	/// The UI scale in force (1.0 = 100 %). Absent, the scale row is not drawn and takes no click.
	std::function<double()> uiScaleNow;
	/// Set the UI scale to a rung of the ladder; the editor applies, persists and says it.
	std::function<void(double scale)> onUiScale;
	/// The screen asks to be closed; the editor hides it.
	std::function<void()> onClose;
	/// Opens the About part's link (the one fixed URL, `kPlugUrl`). Defaults to the OS opener; a case swaps
	/// it for a recorder so a click is tested without a browser. Called with `kPlugUrl`.
	std::function<void(const char* url)> onOpenLink;

	/// The panel's size in logical pixels; it is centred in the frame, and shrinks to the frame less a
	/// margin when the editor is smaller than that.
	static constexpr float kPanelWidth = 600.0f;
	static constexpr float kPanelHeight = 440.0f;

	//-- GEOMETRY, public so a case can click what the hand would click rather than guess.
	visage::Bounds panelBounds() const;
	visage::Bounds closeBounds() const;
	/// The UI scale's rungs, one segment each, in ladder order.
	visage::Bounds scaleSegmentBounds(size_t rung) const;
	/// Brief (0) and Full (1).
	visage::Bounds tooltipSegmentBounds(int index) const;
	/// Left (0) and Right (1): which end of a window's header holds its controls (backlog OD).
	visage::Bounds windowSideSegmentBounds(int index) const;
	/// On (0) and Off (1): arps sent to front of chain (`emitters_front.h`).
	visage::Bounds arpsFrontSegmentBounds(int index) const;
	/// DEBUG OPTIONS (compiled under FEATHERS_DEBUG_OPTIONS), in the About column: where its header sits, and REPAINT's
	/// Partial (0) and Every frame (1) — whether every frame repaints the whole window (`full_redraw.h`).
	float debugOptionsTop() const;
	visage::Bounds repaintSegmentBounds(int index) const;
	/// The hue strip: −180° at its left edge, +180° at its right.
	/// The SoundCloud line under "Owlet Records" in the About part (backlog: the shameless plug).
	visage::Bounds plugLinkBounds() const;
	visage::Bounds hueStripBounds() const;
	visage::Bounds hueResetBounds() const;
	/// The offset a point on the strip names, whole degrees, held to the range.
	double hueAt(float x) const;
	/// Where an offset sits on the strip.
	float hueX(double degrees) const;
	/// THE ACCENT HUE's strip, below the chrome hue's, with the same range and mapping.
	visage::Bounds accentStripBounds() const;
	visage::Bounds accentResetBounds() const;
	double accentAt(float x) const;
	float accentX(double degrees) const;

	/// The ladder the scale row offers — `PluginWindow::kUiScaleLadder`.
	static std::span<const double> scaleLadder();

	/// THE REPAINT ROW's explanation, drawn under its segments (the screen has no tooltips).
	static constexpr const char* kRepaintHint =
	    "Repaint the whole window every frame: costs GPU, avoids stale regions; try it if the UI shows leftovers";

	/// The words the About part says, so a case can read them rather than a picture of them.
	static std::string versionLine();
	static std::string buildLine();
	/// THE EDITOR'S FRAME TIMES (`frame_timing.h`) in a FEATHERS_DIAGNOSTIC build; empty, and not drawn, otherwise.
	static std::string frameTimingLine();
	/// WHAT GROWS OVER A SESSION, IF ANYTHING DOES: editor opens, running visage timers, live preference watches, the
	/// tracked-text cache's fill and its misses while full. Compiled in every build; DRAWN only in a diagnostic one.
	static std::string sessionLine();
	/// The process's live preference watches (chrome and accent hue, tooltip detail, window side, arps to front,
	/// repaint): each open editor holds one of each, so the sum is a multiple of the editors open — never more.
	static size_t prefWatchCount();

private:
	void drawSettings(visage::Canvas& canvas, visage::Bounds column);
	void drawAbout(visage::Canvas& canvas, visage::Bounds column);
	void drawSegments(visage::Canvas& canvas, std::span<const std::string> labels, int selected,
	                  const std::function<visage::Bounds(size_t)>& boundsOf);
	visage::Bounds settingsColumn() const;
	visage::Bounds aboutColumn() const;

	void drawHueStrip(visage::Canvas& canvas, const char* caption, visage::Bounds strip, visage::Bounds reset,
	                  double offset, const std::function<uint32_t(double)>& colourAt);

	/// Redraws the About column's frame times twice a second while shown — started only in a diagnostic build.
	visage::EventTimer frameTimingTimer_;

	enum class Dragging { None, Chrome, Accent };
	Dragging dragging_ = Dragging::None;
	bool linkHovered_ = false;
};

} // namespace feathers::ui
