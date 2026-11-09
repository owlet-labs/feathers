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

#include "plugin/ui/delay_strip.h"

#include "fx/delay/delay_types.h"
#include "plugin/ui/dyn_meter.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace feathers::ui {

namespace dl = fx::delay;

namespace {

/// THE TICK'S STROKE.
constexpr float kTickWidth = 2.0f;

/// "250 ms", "1.50 s" — the live head, as a hand reads a delay time.
void formatSeconds(double seconds, char* out, size_t size) {
	if (seconds < 0.010) {
		std::snprintf(out, size, "%.1f ms", seconds * 1000.0);
	}
	else if (seconds < 1.0) {
		std::snprintf(out, size, "%.0f ms", seconds * 1000.0);
	}
	else {
		std::snprintf(out, size, "%.2f s", seconds);
	}
}

} // namespace

float DelayStrip::xAt(double seconds) const {
	const float usable = std::max(0.0f, bounds.width() - kInsetLeft - kInsetRight);
	const double t = span > 0.0 ? std::clamp(seconds / span, 0.0, 1.0) : 0.0;
	return bounds.x() + kInsetLeft + static_cast<float>(t) * usable;
}

double DelayStrip::secondsAt(float x) const {
	const float usable = std::max(0.0f, bounds.width() - kInsetLeft - kInsetRight);
	if (usable <= 0.0f || span <= 0.0) {
		return 0.0;
	}
	const double t = std::clamp(static_cast<double>((x - bounds.x() - kInsetLeft) / usable), 0.0, 1.0);
	return t * span;
}

float DelayStrip::tipY(int channel, float gain) const {
	// LEFT RISES ABOVE THE LINE, RIGHT HANGS BELOW IT, on the strip's dB scale (`stripHeightOf`); unity
	// reaches the half's edge less a margin, and a gain past unity (Feedback above 100%) stops there rather
	// than leaving the strip.
	const float mid = midY();
	const float reach = std::max(0.0f, bounds.height() * 0.5f - 4.0f);
	const float h = dl::stripHeightOf(gain) * reach;
	return channel == 0 ? mid - h : mid + h;
}

bool delayPicture(const backplane::DeviceHost& host, DeviceInstanceId id, dl::TapPicture* out) {
	const size_t count = host.readingCountAt(id);
	if (count < dl::kReadingCount) {
		return false;
	}
	std::array<float, dl::kReadingCount> values {};
	for (size_t i = 0; i < values.size(); ++i) {
		values[i] = host.readingAt(id, i);
	}
	return dl::decodeTapPicture(values.data(), values.size(), out);
}

double delayHandUnitSeconds(const backplane::DeviceHost& host, DeviceInstanceId id, const dl::TapPicture& p) {
	const double time = std::exp2(static_cast<double>(host.fieldAt(id, static_cast<size_t>(dl::Param::Time))));
	const double spread
	    = std::clamp(static_cast<double>(host.fieldAt(id, static_cast<size_t>(dl::Param::Spread))), 0.0, 1.0);
	return p.axisUnit * time * (1.0 + dl::kSpreadFraction * spread);
}

void paintDelayStrip(visage::Canvas& canvas, const DelayStrip& strip, const dl::TapPicture* picture, double unit) {
	const visage::Bounds b = strip.bounds;
	if (b.width() <= DelayStrip::kInsetLeft + DelayStrip::kInsetRight) {
		return;
	}
	// THE BED, drawn whatever the device is doing, so the strip is a place rather than something that comes
	// and goes (SKLITTER's band).
	canvas.setColor(kSurfaceRaised);
	canvas.roundedRectangle(b.x(), b.y(), b.width(), b.height(), 3.0f);
	const float mid = strip.midY();
	if (picture == nullptr) {
		dyn_meter::drawNoSignal(canvas, b);
		return;
	}
	const dl::TapPicture& p = *picture;
	const double span = strip.span;

	// THE RULES: every multiple of the tap the hand asked for. A modulated or wobbling head draws its ticks
	// OFF these, which is how the strip shows the movement against something that stands still.
	canvas.setColor(withAlpha(kLine, 0.55f));
	if (unit > 0.0) {
		for (int n = 1; n < 64 && unit * n <= span; ++n) {
			canvas.fill(strip.xAt(unit * n) - 0.5f, b.y() + 3.0f, 1.0f, b.height() - 6.0f);
		}
	}
	canvas.setColor(kLine);
	canvas.fill(b.x() + DelayStrip::kInsetLeft, mid - 0.5f, b.width() - DelayStrip::kInsetLeft - DelayStrip::kInsetRight,
	            1.0f);

	// WHICH SIDE IS WHICH, in the margin the inset leaves.
	canvas.setColor(kDisabled);
	canvas.text("L", font(kKnobLabelSize), visage::Font::kLeft, b.x() + 2.0f, b.y() + 1.0f, 10.0f, 10.0f);
	canvas.text("R", font(kKnobLabelSize), visage::Font::kLeft, b.x() + 2.0f, b.y() + b.height() - 11.0f, 10.0f, 10.0f);

	const std::vector<dl::StripTick> ticks = dl::stripTicks(p, span);
	const auto colourOf = [](const dl::StripTick& t) {
		// THE DRY PATH IS GREY; AN ARRIVAL ON ITS OWN SIDE IS AMBER; ONE THE LOOP CROSSED IS BONE.
		if (t.pass < 0) {
			return kMuted;
		}
		return t.origin == t.channel ? kValue : kInk;
	};
	// THE TAIL FADES IN COLOUR AS WELL AS IN HEIGHT, so a long run of repeats at Feedback near unity —
	// which barely shrink — still reads as moving away.
	const auto fadeOf = [](const dl::StripTick& t) {
		return t.pass < 0 ? 1.0f : 0.35f + 0.65f * std::pow(0.8f, static_cast<float>(t.pass));
	};

	// GRANULAR: EACH PASS'S WINDOW AS A WHISKER AT ITS TICK'S TIP — where its grains can land (REVERSE: the
	// chunk the arrival plays back in, with an arrowhead pointing back along it). A whisker
	// rather than a filled area: the windows widen with every pass and overlap, and filled they stacked into
	// one muddy block (photographed, first draft).
	for (const dl::StripTick& t : ticks) {
		if (t.early <= 0.0 && t.late <= 0.0) {
			continue;
		}
		const float x0 = strip.xAt(t.seconds - t.early);
		const float x1 = strip.xAt(t.seconds + t.late);
		const float tip = strip.tipY(t.channel, t.gain);
		canvas.setColor(withAlpha(colourOf(t), 0.5f * fadeOf(t)));
		canvas.fill(x0, tip - 0.5f, std::max(1.0f, x1 - x0), 1.0f);
		if (p.layout == dl::TapLayout::Reverse) {
			canvas.triangleLeft(x0, tip - 3.0f, 3.0f);
		}
	}
	// AND THE GRAINS IN FLIGHT, as dots at the level each is heard at.
	for (const dl::StripGrain& g : dl::stripGrains(p)) {
		if (g.seconds > span || !(g.weight > dl::kStripGainFloor)) {
			continue;
		}
		const float tip = strip.tipY(g.channel, g.weight);
		canvas.setColor(withAlpha(kInk, 0.85f));
		canvas.fill(strip.xAt(g.seconds) - 1.5f, tip - 1.5f, 3.0f, 3.0f);
	}

	for (const dl::StripTick& t : ticks) {
		const float tip = strip.tipY(t.channel, t.gain);
		canvas.setColor(withAlpha(colourOf(t), fadeOf(t)));
		canvas.fill(strip.xAt(t.seconds) - kTickWidth * 0.5f, std::min(tip, mid), kTickWidth,
		            std::max(1.0f, std::abs(tip - mid)));
	}

	// THE LIVE HEAD, top right: the left channel's tap as the device is reading it right now.
	char label[24];
	formatSeconds(p.head[0], label, sizeof(label));
	canvas.setColor(kMuted);
	canvas.text(label, font(kKnobReadoutSize), visage::Font::kRight, b.x(), b.y() + 1.0f, b.width() - 4.0f, 11.0f);
}

} // namespace feathers::ui
