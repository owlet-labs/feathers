/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "backplane/device_host.h"
#include "fx/eroder/eroder_types.h"
#include "plugin/ui/eroder_plot.h"
#include "plugin/ui/tooltip.h"

#include <visage/ui.h>

#include <array>
#include <vector>

// THE ERODER WINDOW'S SUB-PLOT — the strip under the spectrum, one fixed height in every engine.
//
//   ┌ READ HEAD / TRANSFER / TONE …──────────┬ FEEDBACK ─────────────────────────┐
//   │ the engine's own picture               │ reso  ▕████░░░░░░░▏  -6.2 dB      │
//   │                                        │       ▕▌░░░░░░░░░░▏ level         │
//   │                                        │ loop  ▕░░░░░░░░░░░▏   0 dB        │
//   └────────────────────────────────────────┴───────────────────────────────────┘
//
// **LEFT PANEL, BY ENGINE.** Dust and Rust: the read head (where each side's head has been behind the write, left over right, the
// last seconds). Etch: the transfer curves of slot A and slot B (and the Drive, dimmer), input across and output up, from
// `shapeDrive` at the current Type, Shape, Amount and Mix. Comb: the two bar banks (Tone and Character) with each quantity's number,
// across BOTH panels' width — eleven labelled bars do not fit in half — with the FEEDBACK meters in a slim strip above them.
// **RIGHT PANEL, EVERY ENGINE: FEEDBACK.** Two meters, "reso" and "loop": each loop's limiter gain reduction in decibels (nothing at
// rest, the whole bar at `kFloorDb`) and, thin under it, the level the loop's return reached against its limiter's threshold. A
// meter is dim while its loop is not running. The values are the device's readings (`fx::eroder::Reading`); nothing here is computed
// from audio.
//
// **IT REPAINTS ONLY WHEN WHAT IT DRAWS CHANGES**: `update()` compares the row and the readings with the last drawn.

namespace feathers::ui {

class EroderSubplot final : public visage::Frame, public TooltipSource {
public:
	EroderSubplot(backplane::DeviceHost& host, DeviceInstanceId id, const EroderPlot& plot);

	void draw(visage::Canvas& canvas) override;
	void resized() override { layout(); }
	const char* tooltipAt(visage::Point) const override;
	/// IT OFFERS NO CONTROL: it only reads.
	void offeredControls(std::vector<Offered>&) const override {}
	/// RECOMPUTE IF AN INPUT CHANGED, and report whether it did.
	bool update();

	/// The meters' scale: a gain reduction of this many decibels fills the bar.
	static constexpr float kFloorDb = -24.0f;
	static constexpr size_t kHeadHistory = 180;
	/// THE SPLIT: a panel's caption band, and the edge room.
	static constexpr float kInset = 8.0f;
	static constexpr float kCaptionHeight = 14.0f;
	static constexpr float kCombStripHeight = 40.0f;
	/// THE CURVES: how many points a transfer curve is drawn from.
	static constexpr size_t kCurvePoints = 65;

	struct Rect {
		float x = 0.0f;
		float y = 0.0f;
		float w = 0.0f;
		float h = 0.0f;
	};
	/// WHAT A METER READS: the limiter's gain reduction, decibels (0 at rest, down to `kFloorDb`); the return's level against the
	/// limiter's threshold (0..1); and whether the loop is not running.
	struct Meter {
		float reductionDb = 0.0f;
		float level = 0.0f;
		bool dim = false;
		bool operator==(const Meter&) const = default;
	};
	enum class Which { Inner, Outer };
	/// The bar's fill, 0..1, for a reduction in decibels: the restated mapping the drawing uses.
	[[nodiscard]] static float fillFor(float reductionDb) noexcept;

	// TEST SEAMS ---------------------------------------------------------------------------------------------------
	/// The two panels' rectangles, in this frame. In the Comb the left one is the bars, over the whole width.
	[[nodiscard]] Rect leftPanelForTest() const noexcept { return left_; }
	[[nodiscard]] Rect rightPanelForTest() const noexcept { return right_; }
	/// The Comb's bars' region (empty elsewhere), and the feedback meters' region in it.
	[[nodiscard]] Rect barsForTest() const noexcept { return bars_; }
	[[nodiscard]] Meter meterForTest(Which which) const noexcept { return which == Which::Inner ? inner_ : outer_; }
	/// The meter's bar rectangle and its fill's width as drawn.
	[[nodiscard]] Rect meterBarForTest(Which which) const noexcept { return which == Which::Inner ? innerBar_ : outerBar_; }
	[[nodiscard]] float meterFillWidthForTest(Which which) const noexcept;
	[[nodiscard]] size_t headSamplesForTest() const noexcept { return headCount_; }
	[[nodiscard]] fx::eroder::Mode modeForTest() const noexcept { return settings_.mode; }
	[[nodiscard]] int recomputesForTest() const noexcept { return recomputes_; }
	/// Etch's curves: the output for an input, for slot A, slot B and the Drive, as drawn (the points are these at `x`).
	enum class Curve { A, B, Drive };
	[[nodiscard]] float curveForTest(Curve curve, float input) const noexcept;
	/// Whether a curve is the identity line (a slot at Amount 0, a Type that is not a curve) and so drawn dim.
	[[nodiscard]] bool curveIsIdentityForTest(Curve curve) const noexcept;
	[[nodiscard]] Rect curveAreaForTest() const noexcept { return curveArea_; }
	/// RUST'S CARRIER: the multiplier the ring applies to the signal, over \`kCarrierCycles\` cycles of the sine, at a phase 0..1 across
	/// the panel — Source's mix of the sine (folded by Mod Fold) and a fixed noise trace, Rectify folding it to one side, Bias
	/// letting the signal through whole. The sine is the engine's own \`triFold\`; the noise is one seeded trace, not animated.
	static constexpr float kCarrierCycles = 2.0f;
	static constexpr size_t kCarrierPoints = 161;
	[[nodiscard]] float carrierForTest(float phase) const noexcept { return carrierAt(phase); }

private:
	void layout();
	void drawHead(visage::Canvas& canvas);
	void drawCurves(visage::Canvas& canvas);
	void drawCarrier(visage::Canvas& canvas);
	[[nodiscard]] float carrierAt(float phase) const noexcept;
	void drawBars(visage::Canvas& canvas);
	void drawFeedback(visage::Canvas& canvas, const Rect& panel, bool strip);
	void drawMeter(visage::Canvas& canvas, const char* label, const Meter& meter, const Rect& bar, bool strip);

	struct Head {
		float nearL = 0.0f;
		float farL = 0.0f;
		float nearR = 0.0f;
		float farR = 0.0f;
		bool operator==(const Head&) const = default;
	};
	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	const EroderPlot& plot_;
	bool computed_ = false;
	fx::eroder::Row row_ {};
	fx::eroder::Settings settings_ {};
	double rate_ = 48000.0;
	Rect left_ {};
	Rect right_ {};
	Rect bars_ {};
	Rect curveArea_ {};
	Rect innerBar_ {};
	Rect outerBar_ {};
	Meter inner_ {};
	Meter outer_ {};
	std::array<Head, kHeadHistory> head_ {};
	size_t headNext_ = 0;
	size_t headCount_ = 0;
	Head lastHead_ {};
	std::array<float, EroderPlot::kBars> lastBars_ {};
	/// THE NOISE TRACE the carrier shows at a Source above nothing: seeded, smoothed, fixed.
	std::array<float, kCarrierPoints> noise_ {};
	int recomputes_ = 0;
};

} // namespace feathers::ui
