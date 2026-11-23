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
#include "fx/balance/balance_types.h"
#include "plugin/ui/tooltip.h"

#include <visage/ui.h>

#include <array>
#include <string>

// STEREO BALANCE'S PICTURE — the card and the window both draw it; the window just draws it bigger.
//
//   ┌────────────────────────────────────────────┐
//   │ teeth every 200 Hz                         │   THE COMB, on the first spacing's harmonic axis (1..8), one curve per band in
//   │  ╱╲  ╱╲  ╱╲  ╱╲   (the band curves overlay) │   its colour — OWLTT's overlay, from the same `combResponse`
//   ├────────────────────────────────────────────┤
//   │ Between  ░░░░░░░░░░|▓▓▓                L 3.1 → L 0.4 │   ONE METER PER BAND: centre-zero, left louder to the left, ±6 dB. The pale bar is
//   │ Odd      ▓▓▓▓|░░░░░░░                  R 1.2 → R 0.3 │   the imbalance the detector measures; the solid one what is left after the correction
//   │ Even                 |                      0.0 → 0.0 │
//   └────────────────────────────────────────────┘
//
// It reads the device's readings (`balance::Reading`) and its settings, offers no control, and reports through `refresh()` whether anything it draws moved.

namespace feathers::ui {

/// WHAT THE PICTURE DRAWS, in one comparable struct.
struct BalanceModel {
	fx::balance::Settings settings {};
	bool hasReadings = false;
	std::array<float, 3> before {};
	std::array<float, 3> after {};
	std::array<float, 3> level {};
	float spacing1Hz = 0.0f;
	float noteHz = 0.0f;

	friend bool operator==(const BalanceModel&, const BalanceModel&) = default;

	/// THE BANDS' NAMES AS THE SPACINGS READ (`owltt::combBandNames` for three, `combBandNames2` for two).
	[[nodiscard]] std::array<std::string, 3> bandNames() const;
	[[nodiscard]] size_t bandCount() const { return settings.bands >= 3 ? 3 : 2; }
	/// WHICH OF THE COMB RESPONSE'S THREE CURVES BAND `b` IS (Between, Mid, High): a two-band split is Between and High.
	[[nodiscard]] size_t curveOf(size_t b) const { return bandCount() == 2 && b == 1 ? 2 : b; }
	/// THE SECOND SPACING OVER THE FIRST, from the knobs (the note cancels): the curves' shape.
	[[nodiscard]] double ratio21() const;
};

class BalanceView final : public visage::Frame, public TooltipSource {
public:
	BalanceView(backplane::DeviceHost& host, DeviceInstanceId device);

	/// A picture: it offers no control.
	void offeredControls(std::vector<Offered>&) const override {}
	void draw(visage::Canvas& canvas) override;
	const char* tooltipAt(visage::Point local) const override;

	/// RE-READS THE DEVICE; true when something drawn changed (the caller then redraws).
	bool refresh();
	[[nodiscard]] const BalanceModel& model() const { return model_; }

	/// THE METER'S SPAN, dB each side of centre.
	static constexpr float kMeterSpanDb = 6.0f;
	static constexpr double kCurveSpan = 8.0;
	/// A BAND'S COLOUR: the OWLTT Comb's (green, lime, magenta); a two-band split uses the first and the last.
	[[nodiscard]] static uint32_t bandColour(size_t band, size_t bands) noexcept;
	/// THE PICTURE'S LAYOUT, in the view's own coordinates.
	[[nodiscard]] visage::Bounds plotBounds() const;
	[[nodiscard]] visage::Bounds rowBounds(size_t band) const;
	[[nodiscard]] visage::Bounds barBounds(size_t band) const;
	/// WHERE A DB VALUE SITS ON A BAND'S METER (x), clamped to its span.
	[[nodiscard]] float meterX(size_t band, float db) const;
	/// THE CURVE OF BAND `b` AT `points` places across harmonics 0..8.
	[[nodiscard]] std::vector<float> curve(size_t band, size_t points) const;

private:
	backplane::DeviceHost& host_;
	DeviceInstanceId device_;
	BalanceModel model_ {};
	bool primed_ = false;
};

} // namespace feathers::ui
