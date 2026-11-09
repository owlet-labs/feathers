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

#include "plugin/ui/pixel_serial.h"
#include "plugin/ui/param_edit.h"
#include "plugin/ui/widget_registry.h"
#include "plugin/ui/menu_anchor.h"
#include "plugin/ui/header_bar.h"
#include "plugin/ui/libra_door.h"

#include "plugin/ui/theme.h"

#include <embedded/feathers_branding.h>
#include <visage/ui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace feathers::ui {

namespace {

/// The mock's header padding, `14px 20px`.
constexpr float kPadX = 20.0f;

/// The header's control height and gap (the mock's header `gap`). The ENGINE selector that used to set them lives at the
/// top of the Engine rail now (backlog QH); the header's right edge is the Explore/Perform switch.
constexpr float kEngineHeight = 26.0f;
constexpr float kEngineGap = 18.0f;

/// The Explore/Perform switch. Two halves of one box, as the mock has it.
constexpr float kModeWidth = 152.0f;
/// THE UI SCALE READOUT: a button either end of a percentage. Sized for "200%" in the control face
/// with room to breathe; the buttons are the mode switch's height, square.
constexpr float kScaleButton = 22.0f;
constexpr float kScaleReadout = 48.0f;
constexpr float kScaleWidth = kScaleButton * 2.0f + kScaleReadout;

/// THE LIBRA CONSTELLATION: 40 of the strip's 52, so six above and six below. Its width follows from the
/// asset's own viewBox, `800 135 109 143` — change one and change the other.
constexpr float kLogoHeight = 40.0f;
constexpr float kLogoViewHeight = 143.0f;
constexpr float kLogoAspect = 109.0f / kLogoViewHeight;
/// White, by design — not the bone of the type, which would read as one more word in the header.
/// RGB only: the alpha of each pixel is its coverage.
constexpr uint32_t kLogoRgb = 0x00ffffff;

/// THE MARK, RASTERISED HERE RATHER THAN BY visage's PATH FILL, and the reason is measured, not taste:
/// drawing the asset with `Canvas::svg` on this venue (xvfb, lavapipe) painted the stars in the RED
/// channel only and took the blue out of the header's bottom rule beside them — the path atlas's
/// red-only write state reaching draws that are not its own. Nothing else in Feathers fills a
/// `visage::Path`, so that road has never been exercised here, and a decoration is not where to open it.
/// visage still PARSES the asset (`Svg` flattens the curves into `Path::subPaths`); the pixels are ours,
/// drawn through `imageRaw`, the road the Explore terrain already takes.
///
/// Even-odd fill plus the asset's stroke (a band of half its width either side of every edge), sampled
/// 4×4 per native pixel. `width`×`height` are NATIVE pixels; the result is BGRA8 in memory (0xAARRGGBB).
std::vector<uint32_t> rasteriseLogo(int width, int height) {
	std::vector<uint32_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height), 0u);
	visage::Svg svg(resources::branding::libra_constellation_svg);
	svg.setDimensions(width, height, 1.0f);
	if (svg.drawable() == nullptr) {
		return pixels;
	}
	struct Edge {
		visage::Point a;
		visage::Point b;
	};
	std::vector<Edge> edges;
	float strokeWidth = 0.0f;
	const auto collect = [&](const auto& self, const visage::SvgDrawable& node) -> void {
		for (const visage::Path::SubPath& sub : node.path.subPaths()) {
			const size_t n = sub.points.size();
			for (size_t i = 0; n > 1 && i < n; ++i) {
				edges.push_back({ sub.points[i], sub.points[(i + 1) % n] });
			}
		}
		if (!node.path.subPaths().empty()) {
			strokeWidth = std::max(strokeWidth, node.state.stroke_width);
		}
		for (const auto& child : node.children) {
			self(self, *child);
		}
	};
	collect(collect, *svg.drawable());
	// The stroke is in the asset's units; the drawn height is `height` pixels for its 143.
	const float halfStroke = 0.5f * strokeWidth * static_cast<float>(height) / kLogoViewHeight;
	const float halfStrokeSq = halfStroke * halfStroke;

	constexpr int kSub = 4;
	for (int py = 0; py < height; ++py) {
		for (int px = 0; px < width; ++px) {
			int inside = 0;
			for (int sy = 0; sy < kSub; ++sy) {
				const float y = static_cast<float>(py) + (static_cast<float>(sy) + 0.5f) / kSub;
				for (int sx = 0; sx < kSub; ++sx) {
					const float x = static_cast<float>(px) + (static_cast<float>(sx) + 0.5f) / kSub;
					bool odd = false;
					bool onStroke = false;
					for (const Edge& e : edges) {
						if ((e.a.y <= y) != (e.b.y <= y) &&
						    x < e.a.x + (y - e.a.y) * (e.b.x - e.a.x) / (e.b.y - e.a.y)) {
							odd = !odd;
						}
						if (!onStroke && halfStroke > 0.0f) {
							const float dx = e.b.x - e.a.x;
							const float dy = e.b.y - e.a.y;
							const float len = dx * dx + dy * dy;
							const float t = len > 0.0f ? std::clamp(((x - e.a.x) * dx + (y - e.a.y) * dy) / len, 0.0f, 1.0f)
							                           : 0.0f;
							const float ex = e.a.x + t * dx - x;
							const float ey = e.a.y + t * dy - y;
							onStroke = ex * ex + ey * ey <= halfStrokeSq;
						}
					}
					inside += (odd || onStroke) ? 1 : 0;
				}
			}
			const uint32_t alpha = static_cast<uint32_t>(inside * 255 / (kSub * kSub));
			pixels[static_cast<size_t>(py) * static_cast<size_t>(width) + static_cast<size_t>(px)] =
			    (alpha << 24) | kLogoRgb;
		}
	}
	return pixels;
}

} // namespace

std::vector<uint32_t> rasteriseLibraMark(int width, int height) {
	static_assert(kLogoAspect == kLibraMarkAspect, "one viewBox, one aspect");
	return rasteriseLogo(width, height);
}

visage::Bounds HeaderBar::wordmarkBounds() const {
	// The same pen walk `draw` makes, so the box is the word as drawn.
	const visage::Font& wordmark = font(kWordmarkSize);
	float right = kPadX;
	for (const char* part : { "FEA", "TH", "ERS" }) {
		right += trackedWidth(wordmark, part, kWordmarkTracking) + kWordmarkTracking * static_cast<float>(wordmark.size());
	}
	return { kPadX, height() * 0.22f, right - kPadX, kWordmarkSize * 1.4f };
}

bool HeaderBar::hitsSettings(visage::Point position) const {
	if (!onSettings) {
		return false;
	}
	const visage::Bounds logo = logoBounds();
	return (logo.width() > 0.0f && logo.contains(position)) || wordmarkBounds().contains(position);
}

// THE TWO HEIGHTS, PINNED TOGETHER. PresetBrowser::kTopOffset centres the browser in a 52 px strip and
// cannot include this header to ask (header_bar.h includes preset_browser.h, not the other way round),
// so the number it assumes is checked here instead of trusted. Move HeaderBar::kHeight and this fails
// to compile rather than moving the browser's clicks off its box, which is the failure it already had.
static_assert(PresetBrowser::kTopOffset == (HeaderBar::kHeight - PresetBrowser::kHeight) * 0.5f,
              "PresetBrowser::kTopOffset must centre the browser in the header strip");

HeaderBar::~HeaderBar() {
	// BOTH LISTS. This watches the stage mode AND a host parameter, so leaving either behind is a
	// dangling pointer — and the two live in different objects, which is why one call cannot do it.
	mode_.unwatch(this);
	registry_.unwatch(this);
}

HeaderBar::HeaderBar(ParamBridge& /*bridge*/, WidgetRegistry& registry, StageMode& mode,
                     Notifier* notifier)
    : registry_(registry), mode_(mode), notifier_(notifier) {
	// The switch draws which surface is showing, so it has to redraw when anything else changes it.
	watches_.push_back(mode.watch(this));

	// CTRL/CMD + WHEEL ANYWHERE ON THE HEADER STEPS THE UI SCALE (backlog EJ): a wheel over the plugin's
	// window reaches the plugin whatever the host does with its own chords. Unmodified wheels are not
	// consumed — the header has nothing to scroll and must not eat a gesture meant for what is under it.
	onMouseWheel() = [this](const visage::MouseEvent& e) {
		if (!(e.isCtrlDown() || e.isCmdDown()) || !onUiScaleStep || !uiScaleNow) {
			return false;
		}
		const float delta = e.wheel_delta_y;
		if (delta == 0.0f) {
			return false;
		}
		onUiScaleStep(delta > 0.0f ? +1 : -1);
		return true;
	};
	onMouseDown() = [this](const visage::MouseEvent& e) {
		// THE SETTINGS SCREEN'S DOOR (backlog LG): the mark, or the wordmark. Neither overlaps a control.
		if (hitsSettings(e.position)) {
			onSettings();
			return;
		}
		// THE UI SCALE BUTTONS FIRST — they sit left of the mode switch and neither box overlaps, but a
		// widget that is not drawn (no `uiScaleNow`) must not swallow a click either.
		if (uiScaleNow && onUiScaleStep) {
			if (uiScaleMinusBounds().contains(e.position)) {
				onUiScaleStep(-1);
				return;
			}
			if (uiScalePlusBounds().contains(e.position)) {
				onUiScaleStep(+1);
				return;
			}
		}
		// `position` is the point within this frame; `relativePosition()` is mouse movement since the
		// last event, which hit-tested the selector against a delta. See the note in param_row.cpp.
		if (modeBounds().contains(e.position)) {
			const StageMode::Surface wanted = surfaceAt(e.position);
			if (wanted != mode_.surface()) {
				mode_.show(wanted);
				if (notifier_ != nullptr) {
					notifier_->say(wanted == StageMode::Surface::Explore ? "Explore" : "Perform");
				}
			}
		}
	// THE PRESET BROWSER IS NOT FORWARDED TO ANY MORE, and could not usefully have been. It is a
	// sibling frame added to the top frame AFTER this one, so it is on top, and Frame::frameAtPoint
	// stops at the first child that contains the point and does not ignore the mouse — which is the
	// browser. Every click on its box was already being delivered there and swallowed, because it had
	// no handlers of its own; this branch could only ever have fired for points its box does NOT
	// cover. It now handles its own mouse in its own coordinates, which also retires the two-axis
	// translation that lived here and was wrong in one of them.
	};
}

visage::Bounds HeaderBar::uiScaleBounds() const {
	// LEFT OF THE MODE SWITCH, one header gap away, the switch's height: the right side reads
	// [ - 125% + ] [ EXPLORE | PERFORM ] from the middle outward.
	const visage::Bounds mode = modeBounds();
	return {mode.x() - kEngineGap - kScaleWidth, mode.y(), kScaleWidth, mode.height()};
}

visage::Bounds HeaderBar::uiScaleMinusBounds() const {
	const visage::Bounds box = uiScaleBounds();
	return {box.x(), box.y(), kScaleButton, box.height()};
}

visage::Bounds HeaderBar::uiScalePlusBounds() const {
	const visage::Bounds box = uiScaleBounds();
	return {box.right() - kScaleButton, box.y(), kScaleButton, box.height()};
}

visage::Bounds HeaderBar::modeBounds() const {
	const float x = width() - kPadX - kModeWidth;
	return {x, (height() - kEngineHeight) * 0.5f, kModeWidth, kEngineHeight};
}

visage::Bounds HeaderBar::logoBounds() const {
	const float w = kLogoHeight * kLogoAspect;
	const visage::Bounds centred { (width() - w) * 0.5f, (height() - kLogoHeight) * 0.5f, w, kLogoHeight };
	// ONLY WHERE IT FITS. The editor's logical width falls below its design width when a host cannot give it
	// the room a UI scale asks for (MEASURED on xvfb's 1920×1080: 175 % lays out 987 wide), and the right-hand
	// cluster slides left with it. Centred and clear of the preset browser on one side and the first control
	// of the cluster on the other, one header gap each — or not drawn at all: it is a decoration, and moving
	// it off centre would make it a different one.
	// THE WHOLE BAR, the variant chips included (backlog PP).
	const float clearLeft = PresetBrowser::kLeftOffset + PresetBrowser::barWidthFor(PresetBrowser::kHeaderStyle) + kEngineGap;
	const float clearRight = (uiScaleNow ? uiScaleBounds().x() : modeBounds().x()) - kEngineGap;
	if (centred.x() < clearLeft || centred.right() > clearRight) {
		return { centred.x(), centred.y(), 0.0f, 0.0f };
	}
	return centred;
}

void HeaderBar::drawLogo(visage::Canvas& canvas) {
	const visage::Bounds box = logoBounds();
	const float scale = canvas.dpiScale();
	if (scale <= 0.0f || box.width() <= 0.0f) {
		return;
	}
	// ONE IMAGE PIXEL PER NATIVE PIXEL, on the native grid: the box is snapped to whole native pixels so
	// the image is placed, not resampled. The snap moves the mark by at most half a native pixel.
	const int w = std::max(1, static_cast<int>(std::lround(box.width() * scale)));
	const int h = std::max(1, static_cast<int>(std::lround(box.height() * scale)));
	const float x = std::round(box.x() * scale) / scale;
	const float y = std::round(box.y() * scale) / scale;
	if (w != logoWidth_ || h != logoHeight_) {
		logoPixels_ = rasteriseLogo(w, h);
		logoSerial_ = freshPixelSerial();
		logoWidth_ = w;
		logoHeight_ = h;
	}
	canvas.setColor(0xffffffffu);
	canvas.imageRaw(reinterpret_cast<const unsigned char*>(logoPixels_.data()), w, h, visage::ImageFormat::BGRA8, x,
	                y, static_cast<float>(w) / scale, static_cast<float>(h) / scale, logoSerial_);
}

StageMode::Surface HeaderBar::surfaceAt(visage::Point position) const {
	// The box splits down the middle, which is where the two captions divide. Left is Explore, exactly
	// as the labels read.
	const visage::Bounds box = modeBounds();
	return position.x < box.x() + box.width() * 0.5f ? StageMode::Surface::Explore
	                                                 : StageMode::Surface::Perform;
}

bool HeaderBar::hitsPresetPrev(visage::Point position) const {
	return preset_ != nullptr && preset_->hitsPrev(position);
}

bool HeaderBar::hitsPresetNext(visage::Point position) const {
	return preset_ != nullptr && preset_->hitsNext(position);
}

bool HeaderBar::hitsPresetName(visage::Point position) const {
	return preset_ != nullptr && preset_->hitsName(position);
}

void HeaderBar::draw(visage::Canvas& canvas) {
	const float w = width();
	const float h = height();

	canvas.setColor(kVoid);
	canvas.fill(0.0f, 0.0f, w, h);

	// The mock's `border-bottom:1px solid var(--line)`, which is the only thing separating the header
	// from the surface below it.
	canvas.setColor(kLine);
	canvas.fill(0.0f, h - 1.0f, w, 1.0f);

	drawLogo(canvas);

	// FEATHERS, with "TH" in accent — the mock spells it `Fea<span>th</span>ers`, and the highlighted
	// pair is the wordmark's one piece of colour.
	const visage::Font& wordmark = font(kWordmarkSize);
	const float wordTop = h * 0.22f;
	const float wordHeight = kWordmarkSize * 1.4f;
	float pen = kPadX;
	const std::string parts[3] = {"FEA", "TH", "ERS"};
	const uint32_t colours[3] = {kInk, kAccent, kInk};
	for (int i = 0; i < 3; ++i) {
		canvas.setColor(colours[i]);
		drawTracked(canvas, parts[i], wordmark, kWordmarkTracking, pen, wordTop, wordHeight);
		pen += trackedWidth(wordmark, parts[i], kWordmarkTracking) +
		       kWordmarkTracking * static_cast<float>(wordmark.size());
	}

	// The byline names what Feathers is; the build stamp lives on the settings screen's About column.
	const visage::Font& byline = font(kBylineSize);
	canvas.setColor(kMuted);
	drawTracked(canvas, "PROCEDURAL SYNTHESIZER", byline, kBylineTracking,
	            kPadX, h * 0.60f, kBylineSize * 1.6f);

	// The Explore/Perform switch, live: both surfaces exist. The chosen half is a raised panel with the
	// bone type over it and the other is the void with muted type, which is the mock's `.on` segment.
	const visage::Bounds mode = modeBounds();
	const bool exploring = mode_.exploring();
	canvas.setColor(kLine);
	canvas.fill(mode.x(), mode.y(), mode.width(), mode.height());
	canvas.setColor(kSurface);
	canvas.fill(mode.x() + 1.0f, mode.y() + 1.0f, mode.width() - 2.0f, mode.height() - 2.0f);
	const float half = (mode.width() - 2.0f) * 0.5f;
	canvas.setColor(kSurfaceRaised);
	canvas.fill(mode.x() + 1.0f + (exploring ? 0.0f : half), mode.y() + 1.0f, half,
	            mode.height() - 2.0f);
	const visage::Font& control = font(kControlSize);
	canvas.setColor(exploring ? kInk : kMuted);
	drawTracked(canvas, "EXPLORE", control, kControlTracking, mode.x() + 12.0f, mode.y(),
	            mode.height());
	canvas.setColor(exploring ? kMuted : kInk);
	drawTracked(canvas, "PERFORM", control, kControlTracking, mode.x() + mode.width() * 0.52f,
	            mode.y(), mode.height());

	// THE UI SCALE READOUT, drawn only when the editor has told the header what the scale is — a
	// bordered box in the mode switch's idiom, a minus at the left end, the percentage, a plus at the
	// right. The buttons are flat text so the box reads as one control rather than three.
	if (uiScaleNow) {
		const visage::Bounds box = uiScaleBounds();
		canvas.setColor(kLine);
		canvas.fill(box.x(), box.y(), box.width(), box.height());
		canvas.setColor(kSurface);
		canvas.fill(box.x() + 1.0f, box.y() + 1.0f, box.width() - 2.0f, box.height() - 2.0f);
		const visage::Bounds minus = uiScaleMinusBounds();
		const visage::Bounds plus = uiScalePlusBounds();
		canvas.setColor(kSurfaceRaised);
		canvas.fill(minus.x() + 1.0f, minus.y() + 1.0f, minus.width() - 1.0f, minus.height() - 2.0f);
		canvas.fill(plus.x(), plus.y() + 1.0f, plus.width() - 1.0f, plus.height() - 2.0f);
		canvas.setColor(kMuted);
		canvas.text("\xe2\x88\x92", control, visage::Font::kCenter, minus.x(), minus.y(), minus.width(),
		            minus.height());
		canvas.text("+", control, visage::Font::kCenter, plus.x(), plus.y(), plus.width(), plus.height());
		const int percent = static_cast<int>(std::lround(uiScaleNow() * 100.0));
		canvas.setColor(kInk);
		canvas.text(std::to_string(percent) + "%", control, visage::Font::kCenter, minus.right(), box.y(),
		            plus.x() - minus.right(), box.height());
	}
}

} // namespace feathers::ui
