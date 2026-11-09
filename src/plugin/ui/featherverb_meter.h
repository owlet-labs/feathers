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

#pragma once

#include "backplane/device_host.h"
#include "fx/featherverb/featherverb_types.h"
#include "plugin/ui/dyn_meter.h"
#include "plugin/ui/theme.h"

#include <array>

// FEATHERVERB'S METER — IN, WET, GR (backlog LA: an input level meter and input gain; and LD: meter the
// wet and the limiter's gain reduction too). The dynamics cards' column (`dyn_meter.h`), drawn the same
// way on the card and in the window, BESIDE THE INPUT KNOB, so the hand that sets the gain sees what it does:
// - IN: what enters the reverb, after Input — the level Input is set by.
// - WET: the wet after its 0 dBFS limiter. Several kinds run the wet 10-16 dB over a -1 dBFS input, so an input that
//   reads fine can still drive the limiter hard; IN alone cannot show that.
// - GR: what the limiter takes, hanging from the top.

namespace feathers::ui::featherverb_meter {

namespace fv = fx::featherverb;

struct Fractions {
	bool has = false; ///< false until the device has published: "no signal", not bars at zero
	float in = 0.0f;
	float wet = 0.0f;
	float gr = 0.0f;
	friend bool operator==(const Fractions&, const Fractions&) = default;
};

/// THE READINGS AS BAR FRACTIONS (`dyn_meter`'s mappings): IN and WET in dB from -60 to 0 dBFS, GR over 24 dB.
[[nodiscard]] inline Fractions read(const backplane::DeviceHost& host, DeviceInstanceId device) {
	if (host.readingCountAt(device) < static_cast<size_t>(fv::Reading::kCount)) {
		return {};
	}
	const auto at = [&](fv::Reading r) { return host.readingAt(device, static_cast<size_t>(r)); };
	return { true, dyn_meter::levelFraction(at(fv::Reading::InPeak)), dyn_meter::levelFraction(at(fv::Reading::WetPeak)),
		     dyn_meter::grFraction(at(fv::Reading::LimiterGr)) };
}

/// THE COLUMN IN `box`: three bars, captioned underneath, as COMP's (`CompCard::drawMeter`).
inline void draw(visage::Canvas& canvas, visage::Bounds box, const Fractions& f) {
	if (box.width() <= 0 || box.height() <= 0) {
		return;
	}
	if (!f.has) {
		dyn_meter::drawNoSignal(canvas, box);
		return;
	}
	const float third = box.width() / 3.0f;
	const float barHeight = std::max(0.0f, box.height() - dyn_meter::kCaptionHeight - 2.0f);
	const auto centre = [&](int i) { return box.x() + third * (static_cast<float>(i) + 0.5f); };
	dyn_meter::drawBar(canvas, centre(0) - dyn_meter::kBarWidth * 0.5f, box.y(), barHeight, f.in, false, kMeterAccent);
	dyn_meter::drawBar(canvas, centre(1) - dyn_meter::kBarWidth * 0.5f, box.y(), barHeight, f.wet, false, kMeterAccent);
	dyn_meter::drawBar(canvas, centre(2) - dyn_meter::kBarWidth * 0.5f, box.y(), barHeight, f.gr, true, kMeterValue);
	static constexpr std::array<const char*, 3> kCaps { "IN", "WET", "GR" };
	for (size_t i = 0; i < kCaps.size(); ++i) {
		dyn_meter::drawCaption(canvas, kCaps[i], centre(static_cast<int>(i)), box.y() + barHeight + 2.0f, third);
	}
}

/// HOW WIDE THE COLUMN WANTS TO BE: three bars with room between them, as COMP's.
inline constexpr float kWidth = 72.0f;

} // namespace feathers::ui::featherverb_meter
