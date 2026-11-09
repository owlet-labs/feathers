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

#include "device/modulatable.h"
#include "fx/smooth/smooth_types.h"
#include "plugin/ui/tooltip.h"

#include <visage/ui.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>

// THE SMOOTH SHAPER'S PICTURE — the transfer curve and the harmonic bars, drawn by the card and the panel.
//
// **COMPUTED THROUGH THE SAME CORE THE AUDIO RUNS, NEVER A RESTATEMENT.** `fx::smooth::pictureOf` resolves the
// settings exactly as a block's setup does (`targetsOf` is shared with `SmoothShaper::beginBlock`) and
// `transferAt` is `shapeSample` itself. So the curve is what the shaper does to a level, and the bars are what
// it does to a full-scale sine — including the evens and rect post-processing Twist adds. What one static curve
// cannot show is said on it: the feedback loop, the 100 Hz highpass on the wet, and Mix.
//
// **THE GAIN STAGING IS THE POINT OF THE PICTURE**, since it matters a great deal, especially in the Chebyshev
// modes. The x axis is the INPUT in units of the reference's 0 dBFS, the level a full voice arrives at. The
// dashed verticals are the CEILING — where the zone's input saturates: x = 1 into the Chebyshev polynomial at an
// input of 0.5 / drive, Poly's clip at 1 / drive. In the FM zones nothing saturates, and the line is drawn
// dotted and called what it is: where the driven signal reaches 0 dBFS. On the bus the live input peak is a
// marker on the same axis, so where the signal sits against the ceiling is visible, not inferred.

namespace feathers::ui {

/// THE NUMBERS BOTH VIEWS DRAW, RECOMPUTED ONLY WHEN A SETTING MOVES.
struct SmoothPictureModel {
	static constexpr int kCurvePoints = 321;
	/// THE CURVE'S INPUT SPAN: −1..+1 of the reference's 0 dBFS, where a full voice arrives.
	static constexpr float kInputSpan = 1.0f;
	/// THE LEVEL THE BARS ARE MEASURED AT: a full voice, 1.0 = the reference's 0 dBFS.
	static constexpr float kHarmonicLevel = 1.0f;

	fx::smooth::Settings drawn {};
	bool have = false;
	fx::smooth::Picture picture {};
	std::array<float, kCurvePoints> curve {};
	std::array<float, fx::smooth::kHarmonicCount> harmonics {};
	/// THE CURVE'S OUTPUT SPAN: the larger of 1 (0 dBFS) and the curve's own peak, so a +13.8 dB Chebyshev wet
	/// is drawn whole and a quiet FM zone still reads against the 0 dBFS lines.
	float outputSpan = 1.0f;
	/// HOW MANY TIMES THE NUMBERS WERE COMPUTED — a case counts it to prove a still patch costs nothing.
	uint32_t computations = 0;

	/// Recompute if `now` differs from what was drawn. Returns whether anything changed.
	bool refresh(const fx::smooth::Settings& now);

	/// The input level at curve point `i`.
	[[nodiscard]] static float inputAt(int i) {
		return -kInputSpan + 2.0f * kInputSpan * static_cast<float>(i) / static_cast<float>(kCurvePoints - 1);
	}
};

/// THE TRANSFER CURVE, WITH DRIVE LIVING ON IT: the ceiling line is the Drive control, dragged along the input
/// axis. Pulling the ceiling toward the signal IS more drive — the gain staging as the gesture.
class SmoothCurveView final : public visage::Frame, public TooltipSource {
public:
	SmoothCurveView(const SmoothPictureModel& model, device::Modulatable& drive);
	/// The ceiling drags Drive: a side road (Drive has its own fader).
	void offeredControls(std::vector<Offered>& out) const override { out.push_back({ &drive_, Road::Gesture }); }

	void draw(visage::Canvas& canvas) override;
	const char* tooltipAt(visage::Point local) const override;

	/// THE LIVE INPUT PEAK, or none — the owner reads the device's readings and hands them here.
	void setInputPeak(bool has, float peak);
	/// Print the numbers along the axes (the panel's large plot) or not (the card's).
	void setAnnotated(bool on) { annotated_ = on; }

	//-- geometry, shared by the drawing, the hit test and the cases ---------------------------------//
	[[nodiscard]] visage::Bounds plotBounds() const;
	/// Where input level `x` (1.0 = the reference's 0 dBFS) lands horizontally, and back.
	[[nodiscard]] float xOfInput(float input) const;
	[[nodiscard]] float inputOfX(float x) const;
	/// Where output level `y` lands vertically.
	[[nodiscard]] float yOfOutput(float output) const;
	/// THE CEILING'S x ON THE POSITIVE SIDE, clamped to the plot (off-scale ceilings sit on the edge).
	[[nodiscard]] float ceilingX() const;
	/// Is the ceiling beyond the plotted input span (a low drive in 3579, below unity in Poly)?
	[[nodiscard]] bool ceilingOffScale() const;
	/// Does `local` grab the Drive handle — either ceiling line, or its diamond on the axis?
	[[nodiscard]] bool grabsDrive(visage::Point local) const;

	static constexpr float kHandleSize = 9.0f;
	static constexpr float kGrabSlop = 6.0f;

	[[nodiscard]] bool draggingForTest() const noexcept { return dragging_; }

private:
	void dragTo(float x);

	const SmoothPictureModel& model_;
	device::Modulatable& drive_;
	bool hasPeak_ = false;
	float peak_ = 0.0f;
	bool annotated_ = false;
	bool dragging_ = false;
	bool routeDragging_ = false;
	bool gestureOpen_ = false; ///< the value drag's gesture opens on the first MOVE, never on the press
	float grabX_ = 0.0f;
	float grabCeiling_ = 0.0f;
	float grabSide_ = 1.0f;
	float routeAnchorY_ = 0.0f;
	float routeAnchorAmount_ = 0.0f;
	bool hovering_ = false;
	visage::Point hover_ {};
};

/// THE HARMONIC BARS: H1..H9 of a full-scale sine through the shaper, in dB of the reference's 0 dBFS.
class SmoothHarmonicsView final : public visage::Frame, public TooltipSource {
public:
	explicit SmoothHarmonicsView(const SmoothPictureModel& model);
	/// A picture: it offers no control.
	void offeredControls(std::vector<Offered>&) const override {}

	void draw(visage::Canvas& canvas) override;
	const char* tooltipAt(visage::Point local) const override;

	/// THE BARS' dB SPAN: −60 dB to +12 dB of the reference's 0 dBFS (a Chebyshev wet runs to +8 dB).
	static constexpr float kFloorDb = -60.0f;
	static constexpr float kTopDb = 12.0f;
	[[nodiscard]] static float fractionOfLevel(float level);

	[[nodiscard]] visage::Bounds barBounds(int harmonic) const;
	[[nodiscard]] visage::Bounds barsArea() const;

private:
	const SmoothPictureModel& model_;
};

} // namespace feathers::ui

namespace feathers::ui {

class ZoneCoordCell;

/// **HARMONIC AND TWIST, ONE CONTROL ON THE CARD AND IN THE WINDOW** (the window adopts the card's interface: the
/// field modifiers are the pills, so the dropdowns are no longer needed). A coordinate cell
/// with no zone names, so no dropdown: the value box, the lap's sigil, the caption. Twist is the sweep alone now,
/// and no lap of it is a named zone, so nothing is left to name.
[[nodiscard]] std::unique_ptr<ZoneCoordCell> makeSmoothCoordCell(device::Modulatable& coordinate,
                                                                 fx::smooth::Param param, std::string label,
                                                                 float faceDiameter);

} // namespace feathers::ui
