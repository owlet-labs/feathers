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

#include "plugin/ui/resonate_meter.h"

#include "fx/resonate/resonate_types.h"
#include "plugin/ui/dyn_meter.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>

namespace feathers::ui {

namespace rs = fx::resonate;

ResonateMeter::ResonateMeter(const backplane::DeviceHost& host, DeviceInstanceId id, device::Modulatable& pluck)
    : host_(host), device_(id), pluck_(pluck) {
	// A SCALE CHANGE MOVES NOTHING LOGICAL — see the ring view's note.
	onDpiChange() += [this] { setBounds(bounds()); };
	// LAST — see `installTooltipHover`. A meter takes no press; the hover is its only road.
	installTooltipHover(*this, *this);
}

const char* ResonateMeter::tooltipAt(visage::Point) const {
	return tooltipOf(&pluck_);
}

bool ResonateMeter::hasReadings() const {
	return host_.readingCountAt(device_) >= rs::kReadingCount;
}

ResonateMeter::Fractions ResonateMeter::fractions() const {
	if (!hasReadings()) {
		return {};
	}
	Fractions f;
	f.level = dyn_meter::levelFraction(host_.readingAt(device_, rs::kReadInLevel));
	f.flashing = host_.readingAt(device_, rs::kReadPluckAge) < kFlashTicks;
	return f;
}

bool ResonateMeter::refresh() {
	const bool has = hasReadings();
	const Fractions f = fractions();
	if (drawnOnce_ && has == drawnHad_ && f == drawn_) {
		return false;
	}
	drawnOnce_ = true;
	drawnHad_ = has;
	drawn_ = f;
	redraw();
	return true;
}

void ResonateMeter::draw(visage::Canvas& canvas) {
	const visage::Bounds box { 0.0f, 0.0f, static_cast<float>(width()), static_cast<float>(height()) };
	if (box.width() <= 0.0f || box.height() <= 0.0f) {
		return;
	}
	if (!hasReadings()) {
		dyn_meter::drawNoSignal(canvas, box);
		return;
	}
	const Fractions f = fractions();
	// THE LEVEL BAR ON THE LEFT, THE PLUCK STRIP BESIDE IT — two quantities, two marks, one column.
	const float barX = 1.0f;
	const float barH = std::max(0.0f, box.height() - dyn_meter::kCaptionHeight - 2.0f);
	dyn_meter::drawBar(canvas, barX, 0.0f, barH, f.level, false, kMeterAccent);
	const float stripX = barX + dyn_meter::kBarWidth + 3.0f;
	canvas.setColor(kSurfaceRaised);
	canvas.roundedRectangle(stripX, 0.0f, kStripWidth, barH, 1.5f);
	// A PLUCK (a played note's or the Cycle's) LIGHTS THE STRIP.
	if (f.flashing) {
		canvas.setColor(kMeterValue);
		canvas.roundedRectangle(stripX, 0.0f, kStripWidth, barH, 1.5f);
	}
	dyn_meter::drawCaption(canvas, "IN", box.width() * 0.5f, barH + 2.0f, box.width());
}

} // namespace feathers::ui
