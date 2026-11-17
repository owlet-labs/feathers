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

#include "backplane/device_host.h"
#include "plugin/ui/clip_led.h"
#include "plugin/ui/dyn_meter.h"
#include "plugin/ui/theme.h"

#include <visage/ui.h>

#include <algorithm>
#include <cstddef>

// OWLTT'S INPUT AND OUTPUT LEVEL METERS — the small bar of the dynamics cards (`dyn_meter.h`), one at each side of the
// plot, beside the Drive and the Output faders.
//
// THE INPUT is the device's own peak of what it was handed (reading 18, linear, held and falling); THE OUTPUT is the
// engine's measure of what came out (`DeviceHost::outputPeakAt`, the courtesy meter's), so it is the number the next
// device receives. The OUTPUT METER CARRIES THE MASTER CLIP: a small LED at its top, lit in the clip colour when the
// device's held clip flag (reading 9) or the peak at full scale says so, and in the near colour within 2 dB of it
// (`clip_led.h`). The per-band LEDs stay on the plot.
//
// NO DATA DRAWS A BARE TRACK, never a bar at zero: a host that meters nothing and a silent signal must not look alike.

namespace feathers::ui {

class OwlttLevelMeter final : public visage::Frame {
public:
	enum class Side { Input, Output };
	/// The device's reading indices this meter reads (`Owltt::kReadingCount` is 19).
	static constexpr size_t kClipReading = 9;
	static constexpr size_t kInputPeakReading = 18;
	static constexpr size_t kReadingsNeeded = 19;

	static constexpr float kWidth = 11.0f;
	static constexpr float kLedSize = 6.0f;
	static constexpr float kLedGap = 4.0f;

	struct Reading {
		bool have = false;
		float peak = 0.0f; ///< linear
		ClipLedState state = ClipLedState::Normal;
		friend bool operator==(const Reading&, const Reading&) = default;
	};

	OwlttLevelMeter(backplane::DeviceHost& host, DeviceInstanceId device, Side side)
	    : host_(host), device_(device), side_(side) {
		setIgnoresMouseEvents(true, false);
	}

	[[nodiscard]] Side side() const noexcept { return side_; }

	/// WHAT THE METER SHOULD SHOW NOW, read off the host.
	[[nodiscard]] Reading read() const {
		Reading r;
		if (side_ == Side::Input) {
			if (host_.readingCountAt(device_) >= kReadingsNeeded) {
				r.have = true;
				r.peak = std::max(0.0f, host_.readingAt(device_, kInputPeakReading));
			}
			return r;
		}
		float peak = 0.0f;
		if (host_.outputPeakAt(device_, peak)) {
			r.have = true;
			r.peak = std::max(0.0f, peak);
		}
		const bool held = host_.readingCountAt(device_) > kClipReading && host_.readingAt(device_, kClipReading) != 0.0f;
		r.state = clipLedStateFor(r.have ? r.peak : 0.0f, held);
		return r;
	}

	/// THE FRAME'S SHARE OF THE TICK: true, and a redraw, only when the reading moved by a pixel's worth.
	bool update() {
		const Reading now = read();
		if (now == drawn_) {
			return false;
		}
		drawn_ = now;
		redraw();
		return true;
	}
	[[nodiscard]] const Reading& drawnForTest() const noexcept { return drawn_; }
	/// THE BAR'S FRACTION and the LED's state as drawn — the values the picture is made from.
	[[nodiscard]] float barFraction() const { return drawn_.have ? dyn_meter::levelFraction(drawn_.peak) : 0.0f; }
	[[nodiscard]] ClipLedState ledState() const noexcept { return drawn_.state; }
	[[nodiscard]] bool hasLed() const noexcept { return side_ == Side::Output; }
	/// THE BAR'S RECTANGLE in this frame's own coordinates: under the LED on the output side, full height on the input.
	[[nodiscard]] visage::Bounds barBounds() const {
		const float top = hasLed() ? kLedSize + kLedGap : 0.0f;
		return { (width() - dyn_meter::kBarWidth) * 0.5f, top, dyn_meter::kBarWidth, std::max(0.0f, height() - top) };
	}

	void draw(visage::Canvas& canvas) override {
		const visage::Bounds bar = barBounds();
		dyn_meter::drawBar(canvas, bar.x(), bar.y(), bar.height(), barFraction(), false,
		                   drawn_.state == ClipLedState::Normal ? kMeterAccent : kMeterNear);
		if (!hasLed()) {
			return;
		}
		const float x = (width() - kLedSize) * 0.5f;
		if (drawn_.state == ClipLedState::Normal) {
			canvas.setColor(kMuted);
			canvas.ring(x, 0.0f, kLedSize, 1.0f);
		}
		else {
			canvas.setColor(drawn_.state == ClipLedState::Clip ? kMeterClip : kMeterNear);
			canvas.circle(x, 0.0f, kLedSize);
		}
	}

private:
	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	Side side_;
	Reading drawn_ {};
};

} // namespace feathers::ui
