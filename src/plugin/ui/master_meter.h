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
#include "plugin/master_meter.h"
#include "plugin/ui/clip_led.h"

#include <cstdint>
#include <visage/ui.h>

namespace feathers::ui {

/// **THE MASTER METER** — backlog DN: the knob tray's height, at its far right; instantaneous but
/// coloured — orange at -6 LUFS, red at -3 LUFS; sample peak; transport play.
///
/// A column at the tray's far right, the tray's height. The 0 dBFS LED at the top: an INSTANTANEOUS core
/// (lit while the engine says a sample clipped within the last `kCoreHoldSeconds`) inside a glow that
/// DECAYS from the clip (`kGlowFallSeconds`), the two time constants the bus utility's card draws for
/// the same reason — a one-sample over at 60 fps is a light on for a quarter of a frame, and the glow is
/// what makes an over that happened a moment ago still readable. Under it the momentary loudness as a
/// bar over `kFloorLufs..0` and the number, coloured by where it stands: the ink below `kOrangeLufs`,
/// orange from there, red from `kRedLufs`. Sample peak, by design; a true-peak reading would be the
/// engine's to add and this column's to show.
///
/// A READOUT, NOT A CONTROL: it takes no mouse. It reads the bridge's published readings on the
/// editor's tick (`update`) and asks for a frame only when the picture moved.
class MasterMeter : public visage::Frame {
public:
	explicit MasterMeter(const ParamBridge& bridge) : bridge_(bridge) {}

	void draw(visage::Canvas& canvas) override;

	/// Read the published readings; redraw if they changed. Returns whether they did.
	bool update();

	static constexpr float kWidth = 44.0f;
	static constexpr float kFloorLufs = -40.0f;
	static constexpr float kOrangeLufs = -6.0f;
	static constexpr float kRedLufs = -3.0f;
	static constexpr float kCoreHoldSeconds = 0.05f;
	static constexpr float kGlowFallSeconds = 1.5f;
	static constexpr float kLedDiameter = 12.0f;
	static constexpr float kPadY = 8.0f;
	static constexpr float kReadoutHeight = 14.0f;

	enum class Zone { Ink, Orange, Red };
	[[nodiscard]] static Zone zoneFor(float lufs) noexcept;
	[[nodiscard]] static float fillFractionFor(float lufs) noexcept;
	/// THE LED'S STATE (backlog ML): Clip while the core is lit (an over within `kCoreHoldSeconds`) or the held raw
	/// peak is at full scale, Near while that peak is within 2 dB of it, Normal otherwise (`clip_led.h`).
	[[nodiscard]] static ClipLedState ledStateFor(const MasterMeterReadings& r) noexcept;

	[[nodiscard]] const MasterMeterReadings& readingsForTest() const noexcept { return readings_; }
	[[nodiscard]] Zone zoneForTest() const noexcept { return zoneFor(readings_.momentaryLufs); }
	[[nodiscard]] bool ledCoreLitForTest() const noexcept { return ledCoreLit(); }
	[[nodiscard]] float ledGlowForTest() const noexcept { return ledGlow(); }
	[[nodiscard]] ClipLedState ledStateForTest() const noexcept { return ledStateFor(readings_); }

private:
	[[nodiscard]] bool ledCoreLit() const noexcept;
	[[nodiscard]] float ledGlow() const noexcept;

	const ParamBridge& bridge_;
	MasterMeterReadings readings_ {};
};

} // namespace feathers::ui
