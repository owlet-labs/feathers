/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#include "plugin/ui/comp_plot.h"

#include "plugin/ui/plot_grid.h"
#include "plugin/ui/theme.h"

namespace feathers::ui::comp_plot {

void paint(visage::Canvas& canvas, const visage::Bounds& box, const Curves& c, bool hasPeak, float inPeak) {
	if (box.width() <= 2.0f || box.height() <= 2.0f) {
		return;
	}
	canvas.setColor(withAlpha(kLine, 0.25f));
	canvas.roundedRectangle(box.x(), box.y(), box.width(), box.height(), 3.0f);
	const auto px = [&](float db) { return box.x() + (db - kFloorDb) / -kFloorDb * box.width(); };
	const auto py = [&](float db) {
		const float v = std::clamp(db, kFloorDb, 0.0f);
		return box.bottom() - (v - kFloorDb) / -kFloorDb * box.height();
	};
	// A LINE EVERY 12 dB, AND THE UNITY DIAGONAL: where the curve leaves it is where the device does something.
	canvas.setColor(withAlpha(kLine, plot::kFineAlpha));
	for (float db = kFloorDb + 12.0f; db < 0.0f; db += 12.0f) {
		canvas.segment(px(db), box.y() + 1.0f, px(db), box.bottom() - 1.0f, plot::kLineWidth, true);
		canvas.segment(box.x() + 1.0f, py(db), box.right() - 1.0f, py(db), plot::kLineWidth, true);
	}
	canvas.setColor(withAlpha(kMuted, 0.6f));
	canvas.segment(px(kFloorDb), py(kFloorDb), px(0.0f), py(0.0f), plot::kLineWidth, true);
	const auto curve = [&](auto&& f, float weight) {
		float lastX = px(kFloorDb);
		float lastY = py(f(kFloorDb));
		for (float db = kFloorDb + 1.0f; db <= 0.0f; db += 1.0f) {
			const float x = px(db);
			const float y = py(f(db));
			canvas.segment(lastX, lastY, x, y, weight, true);
			lastX = x;
			lastY = y;
		}
	};
	// THE HALVES, FAINT, while the topology has two (backlog QG): the first on the input, the second on its own input —
	// so the eye sees each half's slope and the net between them.
	if (c.paired()) {
		canvas.setColor(withAlpha(kMeterValue, 0.55f));
		curve([&](float db) { return c.firstDb(db); }, 1.0f);
		canvas.setColor(withAlpha(kMeterAccent, 0.45f));
		curve([&](float db) { return c.secondDb(db); }, 1.0f);
	}
	// THE NET CURVE, one segment a dB.
	canvas.setColor(kMeterAccent);
	curve([&](float db) { return c.netDb(db); }, 1.5f);
	// THE INPUT PEAK ON THE NET CURVE, while something is publishing: where the signal is sitting now.
	if (hasPeak && inPeak > 0.0f) {
		const float db = std::clamp(20.0f * std::log10(inPeak), kFloorDb, 0.0f);
		canvas.setColor(kMeterValue);
		constexpr float r = 2.5f;
		canvas.circle(px(db) - r, py(c.netDb(db)) - r, 2.0f * r);
	}
}

void paintMeter(visage::Canvas& canvas, const visage::Bounds& box, const Meter& m) {
	if (!m.has) {
		dyn_meter::drawNoSignal(canvas, box);
		return;
	}
	const float third = box.width() / 3.0f;
	const float barHeight = std::max(0.0f, box.height() - dyn_meter::kCaptionHeight - 2.0f);
	const auto barX = [&](int i) { return box.x() + third * (static_cast<float>(i) + 0.5f) - dyn_meter::kBarWidth * 0.5f; };
	// IN AND OUT RISE FROM THE BOTTOM; GR HANGS FROM THE TOP, because it is level taken away. The GR bar is the value
	// colour so it reads as the thing the device is DOING rather than as a third level.
	dyn_meter::drawBar(canvas, barX(0), box.y(), barHeight, m.in, false, kMeterAccent);
	dyn_meter::drawBar(canvas, barX(1), box.y(), barHeight, m.out, false, kMeterAccent);
	dyn_meter::drawBar(canvas, barX(2), box.y(), barHeight, m.gr, true, kMeterValue);
	static constexpr const char* kCaps[] = { "IN", "OUT", "GR" };
	for (size_t i = 0; i < 3; ++i) {
		dyn_meter::drawCaption(canvas, kCaps[i], box.x() + third * (static_cast<float>(i) + 0.5f), box.y() + barHeight + 2.0f, third);
	}
}

} // namespace feathers::ui::comp_plot
