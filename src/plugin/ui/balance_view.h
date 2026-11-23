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
#include "device/modulatable.h"
#include "fx/balance/balance_types.h"
#include "plugin/ui/tooltip.h"

#include <visage/ui.h>

#include <array>
#include <string>
#include <vector>

// STEREO BALANCE'S PICTURE — the card's centrepiece and the window's, drawn at either size, with the controls ON it.
//
//   ┌─────────────────────────────────────────────┐
//   │ TEETH EVERY 200 Hz   (band curves overlay)  │   THE COMB, on the first spacing's harmonic axis, one curve per band in its colour
//   │ ─────────●─────────  spacing handle         │   (OWLTT's overlay, from the same `combResponse`); the handle IS Spacing, log scale
//   ├──────────────────┬──────────────────────────┤
//   │    ╲  M  ╱  GONIOMETER                      │   L against R turned 45 degrees: mono is the vertical, a band leaning left leans left, width is spread.
//   │  L  ╲ │ ╱  R   │ Between  L 2.2 > 0.0   +.9 │   One trace per band in its colour, added together so overlaps stay readable, over a faint grey GHOST of the whole
//   │ ─────╳─────    │ ░░░░|▓▓                    │   input (before HP Wet, correction and Widen). Beside it, each band's centre-zero
//   │                │ Odd ...                    │   meter (pale: measured; solid: after) and its correlation.
//   ├──────────────────┴──────────────────────────┤
//   │ STRENGTH ───●────  50%    WIDEN ──●──  0%   │   STRENGTH, WIDEN and PAN as slim handles on the plot's foot.
//   └─────────────────────────────────────────────┘
//
// It reads the device's readings (`balance::Reading`): the point pairs the audio thread keeps in a fixed ring, the correlations, the imbalances. The newest pairs are
// kept here with an age, so the traces fade (persistence) without the device keeping more than sixteen. `refresh()` reports whether anything drawn moved.
// The handles write through the controls they are given, as a knob does: a gesture, an undo step, a double-click for the default, and an Alt-less drag with a source
// armed makes a route, as every plot handle here does.

namespace feathers::ui {

/// WHAT THE PICTURE DRAWS, in one comparable struct (the points are kept apart: they move every tick).
struct BalanceModel {
	fx::balance::Settings settings {};
	bool hasReadings = false;
	std::array<float, 3> before {};
	std::array<float, 3> after {};
	std::array<float, 3> level {};
	std::array<float, 4> correlation {}; ///< the bands', then the whole output's
	float spacing1Hz = 0.0f;
	float noteHz = 0.0f;

	friend bool operator==(const BalanceModel&, const BalanceModel&) = default;

	/// THE BANDS' NAMES AS THE SPACINGS READ, honest about Key Track (`fx::balance::bandNames`).
	[[nodiscard]] std::array<std::string, 3> bandNames() const;
	[[nodiscard]] size_t bandCount() const { return settings.bands >= 3 ? 3 : 2; }
	/// WHICH OF THE COMB RESPONSE'S THREE CURVES BAND `b` IS (Between, Mid, High): a two-band split is Between and High.
	[[nodiscard]] size_t curveOf(size_t b) const { return bandCount() == 2 && b == 1 ? 2 : b; }
	/// THE SECOND SPACING OVER THE FIRST, from the knobs (the note cancels): the curves' shape.
	[[nodiscard]] double ratio21() const;
};

/// ONE POINT OF A TRACE, in the plot's own unit square (x to the right, y up, mono straight up), and how many refreshes old it is.
struct TracePoint {
	float x = 0.0f;
	float y = 0.0f;
	int age = 1 << 20;
};

class BalanceView final : public visage::Frame, public TooltipSource {
public:
	/// The four controls the handles write: they are the same controls the window's knobs write.
	BalanceView(backplane::DeviceHost& host, DeviceInstanceId device, device::Modulatable& spacing, device::Modulatable& strength, device::Modulatable& widen,
	            device::Modulatable& pan);

	/// The handles reach these four by a gesture, a side road beside their knobs.
	void offeredControls(std::vector<Offered>& out) const override;
	void draw(visage::Canvas& canvas) override;
	const char* tooltipAt(visage::Point local) const override;

	/// RE-READS THE DEVICE and ages and extends the traces; true when something drawn changed (the caller then redraws).
	bool refresh();
	[[nodiscard]] const BalanceModel& model() const { return model_; }

	static constexpr float kMeterSpanDb = 6.0f;
	static constexpr double kCurveSpan = 8.0;
	/// HOW MANY REFRESHES A POINT LIVES (persistence), and how many each trace keeps.
	static constexpr int kLife = 40;
	static constexpr size_t kTracePoints = 384;
	/// A BAND'S COLOUR: the OWLTT Comb's (green, lime, magenta); a two-band split uses the first and the last. The ghost is a neutral grey.
	[[nodiscard]] static uint32_t bandColour(size_t band, size_t bands) noexcept;
	[[nodiscard]] static uint32_t ghostColour() noexcept { return 0xff9aa0a8; }

	//-- geometry, in the view's own coordinates, shared by the drawing, the hit tests and the cases ------------//
	[[nodiscard]] visage::Bounds curveBounds() const;
	[[nodiscard]] visage::Bounds spacingRail() const;
	[[nodiscard]] visage::Bounds gonioBounds() const;
	[[nodiscard]] visage::Bounds rowBounds(size_t band) const;
	[[nodiscard]] visage::Bounds barBounds(size_t band) const;
	/// THE THREE HANDLES' RAILS: 0 Strength, 1 Widen, 2 Pan.
	[[nodiscard]] visage::Bounds sliderRail(size_t which) const;
	[[nodiscard]] float meterX(size_t band, float db) const;
	[[nodiscard]] std::vector<float> curve(size_t band, size_t points) const;
	/// THE TRACES' POINTS, one list per channel (the three bands, then the ghost), for a case that asks by value.
	[[nodiscard]] const std::vector<TracePoint>& trace(size_t channel) const { return traces_[channel]; }
	/// WHERE A HANDLE IS DRAWN (x of its centre) for the current value, and the value a pointer x means.
	[[nodiscard]] float handleX(size_t which) const;
	[[nodiscard]] double valueAt(size_t which, float x) const;

private:
	[[nodiscard]] device::Modulatable& controlOf(size_t which) const;
	[[nodiscard]] int handleAt(visage::Point p) const;
	void drawGonio(visage::Canvas& canvas);
	void drawHandles(visage::Canvas& canvas);
	void absorbPoints();

	backplane::DeviceHost& host_;
	DeviceInstanceId device_;
	/// 0 Strength, 1 Widen, 2 Pan, 3 Spacing.
	std::array<device::Modulatable*, 4> controls_ {};
	BalanceModel model_ {};
	bool primed_ = false;
	std::array<std::vector<TracePoint>, 4> traces_ {};
	std::array<size_t, 4> head_ {};
	int lastSeq_ = -1;
	/// THE PLOT'S SCALE: the peak amplitude it shows at the rim, followed slowly so a loud passage does not run off it.
	float scale_ = 0.25f;
	bool anyVisible_ = false;

	// THE DRAG
	int dragging_ = -1;
	bool gestureOpen_ = false;
	bool routeDragging_ = false;
	float routeAnchorY_ = 0.0f;
	float routeAnchorAmount_ = 0.0f;
	double grabOffset_ = 0.0;
};

} // namespace feathers::ui
