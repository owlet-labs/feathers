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

#include "plugin/ui/settings_screen.h"

#include "plugin/event_log.h"
#include "plugin/ui/chrome_hue.h"
#include "plugin/ui/emitters_front.h"
#include "plugin/ui/frame_timing.h"
#include "plugin/ui/full_redraw.h"
#include "plugin/ui/open_url.h"
#include "plugin/ui/plugin_window.h"
#include "plugin/ui/theme.h"
#include "plugin/ui/tooltip.h"
#include "plugin/ui/window_controls_side.h"

#include <feathers/credits.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#ifndef FEATHERS_VERSION
#define FEATHERS_VERSION "dev"
#endif

namespace feathers::ui {

namespace {

constexpr float kMargin = 12.0f;     ///< the least room left round the panel when the editor is small
constexpr float kHeadHeight = 40.0f;
constexpr float kPad = 18.0f;
constexpr float kCaptionHeight = 14.0f;
constexpr float kRowHeight = 22.0f;
constexpr float kRowGap = 14.0f;
constexpr const char* kPlugText = "soundcloud.com/greatowlmusic";
constexpr float kStripHeight = 18.0f;
constexpr float kSettingsShare = 0.58f; ///< the settings column's share of the panel's width
constexpr float kTooltipSegmentWidth = 64.0f;
constexpr float kRepaintSegmentWidth = 92.0f; ///< "EVERY FRAME" needs more than a 64 px segment
constexpr float kResetWidth = 48.0f;
constexpr float kSwatch = 12.0f;

std::string signedDegrees(double degrees) {
	const long whole = std::lround(degrees);
	return (whole > 0 ? "+" : "") + std::to_string(whole) + "\xc2\xb0";
}

/// `text` broken into lines no wider than `width` in `face`, at spaces.
std::vector<std::string> wrapped(const std::string& text, const visage::Font& face, float width) {
	std::vector<std::string> lines;
	std::string line;
	size_t at = 0;
	while (at < text.size()) {
		size_t end = text.find(' ', at);
		if (end == std::string::npos) {
			end = text.size();
		}
		const std::string word = text.substr(at, end - at);
		const std::string candidate = line.empty() ? word : line + " " + word;
		if (!line.empty() && textWidth(candidate, face) > width) {
			lines.push_back(line);
			line = word;
		}
		else {
			line = candidate;
		}
		at = end + 1;
	}
	if (!line.empty()) {
		lines.push_back(line);
	}
	return lines;
}

} // namespace

SettingsScreen::SettingsScreen() {
	onOpenLink = [](const char* url) { (void)openInBrowser(url); };
	frameTimingTimer_.onTimerCallback() = [this] {
		if (isVisible()) {
			redraw();
		} else {
			frameTimingTimer_.stopTimer();
		}
	};
}

size_t SettingsScreen::prefWatchCount() {
	return chromeHueWatchCount() + accentHueWatchCount() + tooltipDetailWatchCount()
	       + window_controls_detail::watches().size() + emitters_front_detail::watches().size()
	       + full_redraw_detail::watches().size();
}

std::string SettingsScreen::sessionLine() {
	char text[256];
	std::snprintf(text, sizeof text,
	              "session: opens %llu, timers %zu, watches %zu, text cache %zu/%zu (measures %llu, evictions %llu)",
	              static_cast<unsigned long long>(frameTiming().editorOpens), visage::EventManager::instance().timerCount(),
	              prefWatchCount(), trackedTextCacheSize(), trackedTextCacheCapacity(),
	              static_cast<unsigned long long>(trackedTextMeasures()),
	              static_cast<unsigned long long>(trackedTextEvictions()));
	return text;
}

std::string SettingsScreen::frameTimingLine() {
#ifdef FEATHERS_DIAGNOSTIC
	return frameTiming().summary() + "; " + sessionLine();
#else
	return {};
#endif
}

visage::Bounds SettingsScreen::plugLinkBounds() const {
	const visage::Bounds column = aboutColumn();
	const float y = column.y() + kCaptionHeight + 2.0f + 45.0f;
	const float w = std::min(column.width(), trackedWidth(font(kReadoutSize), kPlugText, 0.0f) + 2.0f);
	return { column.x(), y, w, 15.0f };
}

void SettingsScreen::mouseMove(const visage::MouseEvent& e) {
	const bool over = isVisible() && plugLinkBounds().contains(e.position);
	if (over != linkHovered_) {
		linkHovered_ = over;
		setCursorStyle(over ? visage::MouseCursor::Pointing : visage::MouseCursor::Arrow);
		redraw();
	}
}

void SettingsScreen::mouseExit(const visage::MouseEvent&) {
	if (linkHovered_) {
		linkHovered_ = false;
		setCursorStyle(visage::MouseCursor::Arrow);
		redraw();
	}
}

std::span<const double> SettingsScreen::scaleLadder() {
	return PluginWindow::kUiScaleLadder;
}

std::string SettingsScreen::versionLine() {
	return std::string("Feathers ") + FEATHERS_VERSION;
}

std::string SettingsScreen::buildLine() {
	return std::string("build ") + eventlog::buildSha();
}

visage::Bounds SettingsScreen::panelBounds() const {
	const float w = std::max(0.0f, std::min(kPanelWidth, width() - 2.0f * kMargin));
	const float h = std::max(0.0f, std::min(kPanelHeight, height() - 2.0f * kMargin));
	return { std::round((width() - w) * 0.5f), std::round((height() - h) * 0.5f), w, h };
}

visage::Bounds SettingsScreen::closeBounds() const {
	const visage::Bounds panel = panelBounds();
	// THE × FOLLOWS THE WINDOW-CONTROLS SIDE like every window's (backlog OD).
	const float x = windowControlsSide() == WindowControlsSide::Left ? panel.x() : panel.right() - kHeadHeight;
	return { x, panel.y(), kHeadHeight, kHeadHeight };
}

visage::Bounds SettingsScreen::settingsColumn() const {
	const visage::Bounds panel = panelBounds();
	const float w = panel.width() * kSettingsShare;
	return { panel.x() + kPad, panel.y() + kHeadHeight + kPad, w - 2.0f * kPad,
		     panel.height() - kHeadHeight - 2.0f * kPad };
}

visage::Bounds SettingsScreen::aboutColumn() const {
	const visage::Bounds panel = panelBounds();
	const float x = panel.x() + panel.width() * kSettingsShare;
	return { x + kPad, panel.y() + kHeadHeight + kPad, panel.right() - x - 2.0f * kPad,
		     panel.height() - kHeadHeight - 2.0f * kPad };
}

visage::Bounds SettingsScreen::scaleSegmentBounds(size_t rung) const {
	const visage::Bounds column = settingsColumn();
	const float segment = column.width() / static_cast<float>(scaleLadder().size());
	return { column.x() + segment * static_cast<float>(rung), column.y() + kCaptionHeight, segment, kRowHeight };
}

visage::Bounds SettingsScreen::tooltipSegmentBounds(int index) const {
	const visage::Bounds column = settingsColumn();
	const float top = column.y() + 2.0f * (kCaptionHeight) + kRowHeight + kRowGap;
	return { column.x() + kTooltipSegmentWidth * static_cast<float>(index), top, kTooltipSegmentWidth, kRowHeight };
}

visage::Bounds SettingsScreen::windowSideSegmentBounds(int index) const {
	const visage::Bounds column = settingsColumn();
	const float top = column.y() + 3.0f * kCaptionHeight + 2.0f * kRowHeight + 2.0f * kRowGap;
	return { column.x() + kTooltipSegmentWidth * static_cast<float>(index), top, kTooltipSegmentWidth, kRowHeight };
}

visage::Bounds SettingsScreen::arpsFrontSegmentBounds(int index) const {
	const visage::Bounds column = settingsColumn();
	const float top = column.y() + 4.0f * kCaptionHeight + 3.0f * kRowHeight + 3.0f * kRowGap;
	return { column.x() + kTooltipSegmentWidth * static_cast<float>(index), top, kTooltipSegmentWidth, kRowHeight };
}

float SettingsScreen::debugOptionsTop() const {
	// Under the About part's four lines (version, build, label, the plug link: 15 px each, after a caption) and a gap:
	// see drawAbout.
	const visage::Bounds column = aboutColumn();
	return column.y() + kCaptionHeight + 2.0f + 4.0f * 15.0f + kRowGap;
}

visage::Bounds SettingsScreen::repaintSegmentBounds(int index) const {
	const visage::Bounds column = aboutColumn();
	const float top = debugOptionsTop() + 2.0f * kCaptionHeight + 4.0f;
	const float width = std::min(kRepaintSegmentWidth, column.width() * 0.5f);
	return { column.x() + width * static_cast<float>(index), top, width, kRowHeight };
}

visage::Bounds SettingsScreen::hueStripBounds() const {
	const visage::Bounds column = settingsColumn();
	const float top = column.y() + 5.0f * kCaptionHeight + 4.0f * kRowHeight + 4.0f * kRowGap;
	return { column.x(), top, column.width() - kResetWidth - 8.0f, kStripHeight };
}

visage::Bounds SettingsScreen::hueResetBounds() const {
	const visage::Bounds strip = hueStripBounds();
	return { strip.right() + 8.0f, strip.y(), kResetWidth, kStripHeight };
}

double SettingsScreen::hueAt(float x) const {
	const visage::Bounds strip = hueStripBounds();
	if (strip.width() <= 0.0f) {
		return 0.0;
	}
	const double t = std::clamp((x - strip.x()) / strip.width(), 0.0f, 1.0f);
	return clampChromeHue(std::round(kChromeHueMin + t * (kChromeHueMax - kChromeHueMin)));
}

float SettingsScreen::hueX(double degrees) const {
	const visage::Bounds strip = hueStripBounds();
	const double t = (clampChromeHue(degrees) - kChromeHueMin) / (kChromeHueMax - kChromeHueMin);
	return strip.x() + static_cast<float>(t) * strip.width();
}

visage::Bounds SettingsScreen::accentStripBounds() const {
	const visage::Bounds hue = hueStripBounds();
	// Below the chrome strip's end labels (12 px under a 2 px gap), a gap, and its own caption.
	return { hue.x(), hue.bottom() + 20.0f + kCaptionHeight, hue.width(), kStripHeight };
}

visage::Bounds SettingsScreen::accentResetBounds() const {
	const visage::Bounds strip = accentStripBounds();
	return { strip.right() + 8.0f, strip.y(), kResetWidth, kStripHeight };
}

double SettingsScreen::accentAt(float x) const {
	const visage::Bounds strip = accentStripBounds();
	if (strip.width() <= 0.0f) {
		return 0.0;
	}
	const double t = std::clamp((x - strip.x()) / strip.width(), 0.0f, 1.0f);
	return clampAccentHue(std::round(kAccentHueMin + t * (kAccentHueMax - kAccentHueMin)));
}

float SettingsScreen::accentX(double degrees) const {
	const visage::Bounds strip = accentStripBounds();
	const double t = (clampAccentHue(degrees) - kAccentHueMin) / (kAccentHueMax - kAccentHueMin);
	return strip.x() + static_cast<float>(t) * strip.width();
}

void SettingsScreen::mouseDown(const visage::MouseEvent& e) {
	const visage::Point p = e.position;
	if (closeBounds().contains(p) || !panelBounds().contains(p)) {
		if (onClose) {
			onClose();
		}
		return;
	}
	if (plugLinkBounds().contains(p)) {
		if (onOpenLink) {
			onOpenLink(kPlugUrl);
		}
		return;
	}
	if (uiScaleNow && onUiScale) {
		const std::span<const double> ladder = scaleLadder();
		for (size_t i = 0; i < ladder.size(); ++i) {
			if (scaleSegmentBounds(i).contains(p)) {
				onUiScale(ladder[i]);
				redraw();
				return;
			}
		}
	}
	for (int i = 0; i < 2; ++i) {
		if (tooltipSegmentBounds(i).contains(p)) {
			(void)setTooltipDetail(i == 0 ? TooltipDetail::Brief : TooltipDetail::Full);
			redraw();
			return;
		}
	}
	for (int i = 0; i < 2; ++i) {
		if (windowSideSegmentBounds(i).contains(p)) {
			(void)setWindowControlsSide(i == 0 ? WindowControlsSide::Left : WindowControlsSide::Right);
			redraw();
			return;
		}
	}
	for (int i = 0; i < 2; ++i) {
		if (arpsFrontSegmentBounds(i).contains(p)) {
			(void)setEmittersToFront(i == 0);
			redraw();
			return;
		}
	}
#ifdef FEATHERS_DEBUG_OPTIONS
	for (int i = 0; i < 2; ++i) {
		if (repaintSegmentBounds(i).contains(p)) {
			(void)setFullRedraw(i == 1);
			redraw();
			return;
		}
	}
#endif
	if (hueResetBounds().contains(p)) {
		(void)setChromeHueOffset(0.0);
		redraw();
		return;
	}
	if (hueStripBounds().contains(p)) {
		// A DOUBLE CLICK ON THE STRIP IS THE OTHER WAY HOME, as on every knob in this editor.
		(void)setChromeHueOffset(e.repeatClickCount() >= 2 ? 0.0 : hueAt(p.x));
		dragging_ = e.repeatClickCount() < 2 ? Dragging::Chrome : Dragging::None;
		redraw();
		return;
	}
	if (accentResetBounds().contains(p)) {
		(void)setAccentHueOffset(0.0);
		redraw();
		return;
	}
	if (accentStripBounds().contains(p)) {
		(void)setAccentHueOffset(e.repeatClickCount() >= 2 ? 0.0 : accentAt(p.x));
		dragging_ = e.repeatClickCount() < 2 ? Dragging::Accent : Dragging::None;
		redraw();
	}
}

void SettingsScreen::mouseDrag(const visage::MouseEvent& e) {
	if (dragging_ == Dragging::Chrome) {
		(void)setChromeHueOffset(hueAt(e.position.x));
		redraw();
	}
	else if (dragging_ == Dragging::Accent) {
		(void)setAccentHueOffset(accentAt(e.position.x));
		redraw();
	}
}

void SettingsScreen::mouseUp(const visage::MouseEvent&) { dragging_ = Dragging::None; }

void SettingsScreen::drawSegments(visage::Canvas& canvas, std::span<const std::string> labels, int selected,
                                  const std::function<visage::Bounds(size_t)>& boundsOf) {
	if (labels.empty()) {
		return;
	}
	const visage::Bounds first = boundsOf(0);
	const visage::Bounds last = boundsOf(labels.size() - 1);
	// The header's mode switch's idiom: a line-coloured box, the surface inside it, the chosen segment
	// raised with bone type and the rest muted.
	canvas.setColor(kLine);
	canvas.fill(first.x(), first.y(), last.right() - first.x(), first.height());
	canvas.setColor(kSurface);
	canvas.fill(first.x() + 1.0f, first.y() + 1.0f, last.right() - first.x() - 2.0f, first.height() - 2.0f);
	const visage::Font& face = font(kControlSize);
	for (size_t i = 0; i < labels.size(); ++i) {
		const visage::Bounds box = boundsOf(i);
		const bool on = static_cast<int>(i) == selected;
		if (on) {
			canvas.setColor(kSurfaceRaised);
			canvas.fill(box.x() + 1.0f, box.y() + 1.0f, box.width() - 2.0f, box.height() - 2.0f);
		}
		canvas.setColor(on ? kInk : kMuted);
		canvas.text(labels[i], face, visage::Font::kCenter, box.x(), box.y(), box.width(), box.height());
	}
}

void SettingsScreen::drawSettings(visage::Canvas& canvas, visage::Bounds column) {
	const visage::Font& caption = font(kBylineSize);
	const auto captionAt = [&](const char* text, float y) {
		canvas.setColor(kMuted);
		drawTracked(canvas, text, caption, kBylineTracking, column.x(), y, kCaptionHeight);
	};

	// UI SCALE — the ladder's rungs, all on the face.
	if (uiScaleNow) {
		captionAt("UI SCALE", column.y());
		const std::span<const double> ladder = scaleLadder();
		std::vector<std::string> labels;
		int selected = -1;
		const double now = uiScaleNow();
		for (size_t i = 0; i < ladder.size(); ++i) {
			labels.push_back(std::to_string(static_cast<int>(std::lround(ladder[i] * 100.0))) + "%");
			if (std::abs(ladder[i] - now) < 1e-6) {
				selected = static_cast<int>(i);
			}
		}
		drawSegments(canvas, labels, selected, [this](size_t i) { return scaleSegmentBounds(i); });
	}

	// TOOLTIPS — Brief | Full.
	const visage::Bounds brief = tooltipSegmentBounds(0);
	captionAt("TOOLTIPS", brief.y() - kCaptionHeight);
	const std::array<std::string, 2> details { "BRIEF", "FULL" };
	drawSegments(canvas, details, tooltipDetail() == TooltipDetail::Brief ? 0 : 1,
	             [this](size_t i) { return tooltipSegmentBounds(static_cast<int>(i)); });

	// WINDOW CONTROLS — Left | Right: which end of a window's header holds the close, fold and enable.
	const visage::Bounds leftSide = windowSideSegmentBounds(0);
	captionAt("WINDOW CONTROLS", leftSide.y() - kCaptionHeight);
	const std::array<std::string, 2> sides { "LEFT", "RIGHT" };
	drawSegments(canvas, sides, windowControlsSide() == WindowControlsSide::Left ? 0 : 1,
	             [this](size_t i) { return windowSideSegmentBounds(static_cast<int>(i)); });

	// ARPS SENT TO FRONT OF CHAIN — On | Off: where a picked arp lands in a lane or chain.
	const visage::Bounds arpsOn = arpsFrontSegmentBounds(0);
	captionAt("ARPS SENT TO FRONT OF CHAIN", arpsOn.y() - kCaptionHeight);
	const std::array<std::string, 2> arpsFront { "ON", "OFF" };
	drawSegments(canvas, arpsFront, emittersToFront() ? 0 : 1,
	             [this](size_t i) { return arpsFrontSegmentBounds(static_cast<int>(i)); });

	// CHROME HUE and ACCENT HUE — each strip is painted in the accent AT the offset each x would choose
	// (with the other strip's offset as it stands), so it is a picture of the choice before it is made.
	const double chrome = chromeHueOffset();
	const double accent = accentHueOffset();
	drawHueStrip(canvas, "CHROME HUE", hueStripBounds(), hueResetBounds(), chrome,
	             [this, accent](double x) { return rotateHue(kChromeBase.accent, hueAt(static_cast<float>(x)) + accent); });
	drawHueStrip(canvas, "ACCENT HUE", accentStripBounds(), accentResetBounds(), accent,
	             [this, chrome](double x) { return rotateHue(kChromeBase.accent, chrome + accentAt(static_cast<float>(x))); });

	// WHAT EACH STRIP TURNS, as swatches in the colours in force: the greys and the eight window hues turn
	// with Chrome Hue alone; the three roles turn with Chrome Hue AND Accent Hue. The editor behind the scrim
	// shows it in place; this shows it all in one row.
	const visage::Font& small = font(kKnobLabelSize);
	const float swatchTop = accentStripBounds().bottom() + 20.0f;
	captionAt("WHAT EACH TURNS", swatchTop);
	const float y = swatchTop + kCaptionHeight;
	const float labelY = y + kSwatch + 3.0f;
	const auto row = [&](float x, std::span<const uint32_t> colours) {
		for (uint32_t colour : colours) {
			canvas.setColor(colour);
			canvas.fill(x, y, kSwatch, kSwatch);
			x += kSwatch + 2.0f;
		}
		return x;
	};
	const std::array<uint32_t, 7> greys { kVoid, kSurface, kSurfaceRaised, kLine, kDisabled, kMuted, kInk };
	const std::array<uint32_t, 3> roles { kAccent, kValue, kModulation };
	float x = column.x();
	const float chromeStart = x;
	x = row(x, greys);
	x = row(x + 4.0f, kInstanceTints);
	const float chromeEnd = x;
	x += 10.0f;
	const float rolesStart = x;
	x = row(x, roles);
	canvas.setColor(kMuted);
	canvas.text("CHROME", small, visage::Font::kLeft, chromeStart, labelY, chromeEnd - chromeStart, 12.0f);
	canvas.text("CHROME + ACCENT", small, visage::Font::kLeft, rolesStart, labelY, column.right() - rolesStart, 12.0f);
	canvas.setColor(kDisabled);
	canvas.text("Sound colours, meters, the stereo ring and the terrain keep theirs.", small, visage::Font::kLeft,
	            column.x(), labelY + 14.0f, column.width(), 12.0f);
}

void SettingsScreen::drawHueStrip(visage::Canvas& canvas, const char* captionText, visage::Bounds strip,
                                  visage::Bounds reset, double offset, const std::function<uint32_t(double)>& colourAt) {
	canvas.setColor(kMuted);
	drawTracked(canvas, captionText, font(kBylineSize), kBylineTracking, strip.x(), strip.y() - kCaptionHeight,
	            kCaptionHeight);
	canvas.setColor(kInk);
	canvas.text(signedDegrees(offset), font(kReadoutSize), visage::Font::kRight, strip.x(),
	            strip.y() - kCaptionHeight, strip.width(), kCaptionHeight);
	const int steps = std::max(1, static_cast<int>(strip.width() / 3.0f));
	const float step = strip.width() / static_cast<float>(steps);
	for (int i = 0; i < steps; ++i) {
		const float x = strip.x() + step * static_cast<float>(i);
		canvas.setColor(colourAt(x + step * 0.5f));
		canvas.fill(x, strip.y(), step + 0.5f, strip.height());
	}
	// THE THUMB: a bone bar with a void edge, so it reads on every hue the strip holds. Both strips share
	// one range and one mapping, so the offset's x is the same arithmetic for either.
	const double t = (std::clamp(offset, kChromeHueMin, kChromeHueMax) - kChromeHueMin) / (kChromeHueMax - kChromeHueMin);
	const float thumb = strip.x() + static_cast<float>(t) * strip.width();
	canvas.setColor(kVoid);
	canvas.fill(thumb - 2.5f, strip.y() - 3.0f, 5.0f, strip.height() + 6.0f);
	canvas.setColor(kInk);
	canvas.fill(thumb - 1.0f, strip.y() - 2.0f, 2.0f, strip.height() + 4.0f);
	// The ends and the middle, named.
	canvas.setColor(kDisabled);
	const visage::Font& small = font(kKnobLabelSize);
	const float below = strip.bottom() + 2.0f;
	canvas.text("-180\xc2\xb0", small, visage::Font::kLeft, strip.x(), below, 40.0f, 12.0f);
	canvas.text("0", small, visage::Font::kCenter, strip.x() + strip.width() * 0.5f - 20.0f, below, 40.0f, 12.0f);
	canvas.text("+180\xc2\xb0", small, visage::Font::kRight, strip.right() - 40.0f, below, 40.0f, 12.0f);
	// RESET, a flat button in the segments' idiom.
	canvas.setColor(kLine);
	canvas.fill(reset.x(), reset.y(), reset.width(), reset.height());
	canvas.setColor(offset == 0.0 ? kSurface : kSurfaceRaised);
	canvas.fill(reset.x() + 1.0f, reset.y() + 1.0f, reset.width() - 2.0f, reset.height() - 2.0f);
	canvas.setColor(offset == 0.0 ? kDisabled : kInk);
	canvas.text("RESET", font(kKnobLabelSize), visage::Font::kCenter, reset.x(), reset.y(), reset.width(),
	            reset.height());
}

void SettingsScreen::drawAbout(visage::Canvas& canvas, visage::Bounds column) {
	const visage::Font& caption = font(kBylineSize);
	canvas.setColor(kMuted);
	drawTracked(canvas, "ABOUT", caption, kBylineTracking, column.x(), column.y(), kCaptionHeight);
	const visage::Font& body = font(kReadoutSize);
	float y = column.y() + kCaptionHeight + 2.0f;
	canvas.setColor(kInk);
	canvas.text(versionLine(), body, visage::Font::kLeft, column.x(), y, column.width(), 15.0f);
	y += 15.0f;
	canvas.setColor(kMuted);
	canvas.text(buildLine(), body, visage::Font::kLeft, column.x(), y, column.width(), 15.0f);
	y += 15.0f;
	canvas.text("Owlet Records", body, visage::Font::kLeft, column.x(), y, column.width(), 15.0f);
	y += 15.0f;
	// THE PLUG: accent, underlined and lit on hover; a click opens the one fixed URL (mouseDown).
	const visage::Bounds link = plugLinkBounds();
	canvas.setColor(linkHovered_ ? kInk : kAccent);
	canvas.text(kPlugText, body, visage::Font::kLeft, link.x(), link.y(), link.width() + 4.0f, link.height());
	if (linkHovered_) {
		canvas.fill(link.x(), link.bottom() - 1.0f, link.width(), 1.0f);
	}
	y += 15.0f + kRowGap;

#ifdef FEATHERS_DEBUG_OPTIONS
	// DEBUG OPTIONS (its own section, so a release can drop the whole of it in one place —
	// `FEATHERS_DEBUG_OPTIONS`). REPAINT, Partial | Every frame: the canvas's full-redraw switch (`full_redraw.h`),
	// with what it costs written under it, since this screen has no tooltips. Then, in a diagnostic build, the
	// editor's frame times and session counters (`frame_timing.h`).
	{
		y = debugOptionsTop();
		canvas.setColor(kMuted);
		drawTracked(canvas, "DEBUG OPTIONS", caption, kBylineTracking, column.x(), y, kCaptionHeight);
		const visage::Bounds partial = repaintSegmentBounds(0);
		canvas.setColor(kMuted);
		drawTracked(canvas, "REPAINT", caption, kBylineTracking, column.x(), partial.y() - kCaptionHeight,
		            kCaptionHeight);
		const std::array<std::string, 2> repaint { "PARTIAL", "EVERY FRAME" };
		drawSegments(canvas, repaint, fullRedraw() ? 1 : 0,
		             [this](size_t i) { return repaintSegmentBounds(static_cast<int>(i)); });
		const visage::Font& hintFace = font(kKnobLabelSize);
		y = partial.bottom() + 3.0f;
		canvas.setColor(kDisabled);
		for (const std::string& line : wrapped(kRepaintHint, hintFace, column.width())) {
			canvas.text(line, hintFace, visage::Font::kLeft, column.x(), y, column.width(), 11.0f);
			y += 11.0f;
		}
		y += 6.0f;
		if (const std::string frames = frameTimingLine(); !frames.empty()) {
			if (!frameTimingTimer_.isRunning()) {
				frameTimingTimer_.startTimer(500);
			}
			canvas.setColor(kMuted);
			drawTracked(canvas, "FRAME TIMES", caption, kBylineTracking, column.x(), y, kCaptionHeight);
			y += kCaptionHeight + 2.0f;
			const visage::Font& small = font(kKnobLabelSize + 1.0f);
			canvas.setColor(kInk);
			for (const std::string& line : wrapped(frames, small, column.width())) {
				canvas.text(line, small, visage::Font::kLeft, column.x(), y, column.width(), 11.0f);
				y += 11.0f;
			}
		}
		y += kRowGap;
	}
#endif

	canvas.setColor(kMuted);
	drawTracked(canvas, "CREDITS", caption, kBylineTracking, column.x(), y, kCaptionHeight);
	y += kCaptionHeight + 2.0f;
	// FROM THIRD-PARTY-NOTICES.md AT BUILD TIME (cmake/GenerateCredits.cmake), so this cannot drift from
	// what ships. Clipped at the panel's foot rather than shrunk: the notices file is the full text.
	const visage::Font& small = font(kKnobLabelSize + 1.0f);
	constexpr float kCreditLine = 11.0f;
	canvas.setColor(kDisabled);
	for (const char* credit : credits::kRequired) {
		for (const std::string& line : wrapped(credit, small, column.width())) {
			if (y + kCreditLine > column.bottom()) {
				return;
			}
			canvas.text(line, small, visage::Font::kLeft, column.x(), y, column.width(), kCreditLine);
			y += kCreditLine;
		}
		y += 3.0f;
	}
	if (y + kCreditLine <= column.bottom()) {
		canvas.text("Full licences: THIRD-PARTY-NOTICES.md", small, visage::Font::kLeft, column.x(), y,
		            column.width(), kCreditLine);
	}
}

void SettingsScreen::draw(visage::Canvas& canvas) {
	// THE SCRIM: the editor stays visible behind, dimmed, so a hue being dragged is seen in place.
	canvas.setColor(withAlpha(kVoid, 0.55f));
	canvas.fill(0.0f, 0.0f, width(), height());

	const visage::Bounds panel = panelBounds();
	if (panel.width() <= 0.0f || panel.height() <= 0.0f) {
		return;
	}
	canvas.setColor(kLine);
	canvas.roundedRectangle(panel.x(), panel.y(), panel.width(), panel.height(), kCardCorner);
	canvas.setColor(kSurface);
	canvas.roundedRectangle(panel.x() + 1.0f, panel.y() + 1.0f, panel.width() - 2.0f, panel.height() - 2.0f,
	                        kCardCorner - 1.0f);

	// THE HEAD: the wordmark as the header sets it, "SETTINGS" after it, the × at the right.
	const visage::Font& wordmark = font(kWordmarkSize);
	// (the × owns the left end when the desk puts window controls there, and the wordmark follows it)
	float pen = panel.x() + kPad + (windowControlsSide() == WindowControlsSide::Left ? kHeadHeight - kPad * 0.5f : 0.0f);
	const float wordTop = panel.y() + (kHeadHeight - kWordmarkSize * 1.4f) * 0.5f;
	const std::string parts[3] = { "FEA", "TH", "ERS" };
	const uint32_t colours[3] = { kInk, kAccent, kInk };
	for (int i = 0; i < 3; ++i) {
		canvas.setColor(colours[i]);
		drawTracked(canvas, parts[i], wordmark, kWordmarkTracking, pen, wordTop, kWordmarkSize * 1.4f);
		pen += trackedWidth(wordmark, parts[i], kWordmarkTracking) +
		       kWordmarkTracking * static_cast<float>(wordmark.size());
	}
	canvas.setColor(kMuted);
	drawTracked(canvas, "SETTINGS", font(kBylineSize), kBylineTracking, pen + 10.0f, panel.y(), kHeadHeight);
	const visage::Bounds close = closeBounds();
	canvas.setColor(kMuted);
	canvas.text("\xc3\x97", font(16.0f), visage::Font::kCenter, close.x(), close.y(), close.width(),
	            close.height());
	canvas.setColor(kLine);
	canvas.fill(panel.x() + 1.0f, panel.y() + kHeadHeight, panel.width() - 2.0f, 1.0f);
	// The rule between the two parts.
	const float split = panel.x() + panel.width() * kSettingsShare;
	canvas.fill(split, panel.y() + kHeadHeight + kPad, 1.0f, panel.height() - kHeadHeight - 2.0f * kPad);

	drawSettings(canvas, settingsColumn());
	drawAbout(canvas, aboutColumn());
}

} // namespace feathers::ui
