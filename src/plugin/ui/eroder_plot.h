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
#include "fx/eroder/eroder_types.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/tooltip.h"

#include <visage/ui.h>

#include <array>
#include <memory>
#include <optional>
#include <vector>

// THE ERODER'S PICTURE — one frame, two pictures, by the engine that is running.
//
// **DUST: THE SPECTRUM, AND THE BAND THAT SHAKES THE HEAD DRAWN ON IT.** Behind, what came in (grey) and what went
// out (the line; where it stands above the grey is what the shaking added), both the device's own analyser frame
// (`fx::eroder::Reading`). In front, the modulator's band: `deriveDust`'s centre and Q through the engine's own
// filter (two trapezoidal bandpasses in series, drawn with the same frequency warp), its peak at the height of the
// Amount. The sine half of Source is a stem at the centre.
//
// **THE BAND'S HANDLE IS THREE CONTROLS.** A press places the centre under the pointer and a drag moves it: Freq.
// The same drag up and down is the Amount, RELATIVE to where the press landed and only past a dead zone, so a
// sideways drag does not touch it. The wheel is the Width. With a source armed the press is a ROUTE drag on Freq,
// `Knob`'s gesture, and the reach is drawn along the foot. Each of the three also has a knob beside the plot, which
// is where its own routes are made.
//
// **COMB: ELEVEN BARS**, the eleven quantities Tone and Character turn (`evalToneBank`, `evalCharacterBank`, with
// the floors `derive` applies), in two groups; the window prints each one's real number under it (`derive`).
// Nothing on this picture is draggable: its two coordinates are the cells beside it.
//
// **THE WINDOW'S PLOT CARRIES THE READ HEAD UNDER IT**: where each side's head has been behind the write, the last
// few seconds, left over right, from the device's readings.
//
// **IT REPAINTS ONLY WHEN WHAT IT DRAWS CHANGES**: `update()` compares the controls, the note, the analyser's
// frame counter, the head and the arm state against what the last frame was drawn from. A device that is not
// processing publishes nothing new, so the picture rests.

namespace feathers::ui {

class ModState;
class EditHistory;

class EroderPlot final : public visage::Frame, public TooltipSource {
public:
	/// @param large the window's size: axis labels, the bars' numbers, the head strip.
	EroderPlot(backplane::DeviceHost& host, DeviceInstanceId id, bool large);

	void draw(visage::Canvas& canvas) override;
	const char* tooltipAt(visage::Point) const override;
	void offeredControls(std::vector<Offered>& out) const override;
	void setModulation(ModState* mod, EditHistory* history);

	/// RECOMPUTE IF AN INPUT CHANGED, and report whether it did — the card's `updateContent` contract.
	bool update();

	static constexpr double kMinHz = 20.0;
	static constexpr double kMaxHz = 20000.0;
	/// THE SPECTRUM'S WINDOW, dB of power density after the tilt: the top and the foot of the plot.
	static constexpr float kTopDb = -18.0f;
	static constexpr float kFootDb = -102.0f;
	/// PINK NOISE DRAWS FLAT: dB an octave added about 1 kHz.
	static constexpr float kTiltDbPerOctave = 3.0f;
	/// HOW FAR DOWN THE BAND'S SKIRT IS DRAWN before it meets the foot.
	static constexpr float kBandFloorDb = -36.0f;
	/// HOW FAR THE POINTER MOVES UP OR DOWN BEFORE A DRAG TOUCHES THE AMOUNT, in logical pixels.
	static constexpr float kAmountDeadZone = 3.0f;
	/// ONE WHEEL NOTCH OF WIDTH.
	static constexpr double kWidthPerNotch = 0.04;
	static constexpr size_t kBars = 11;
	static constexpr size_t kToneBars = 6;

	//----------------------------------------------------------------------------------------------//
	// TEST SEAMS — the plotted VALUES, so a case asserts the picture's numbers.                      //
	//----------------------------------------------------------------------------------------------//
	[[nodiscard]] bool showsDustForTest() const noexcept { return dust_; }
	[[nodiscard]] bool showsRustForTest() const noexcept { return rust_; }
	[[nodiscard]] bool showsEtchForTest() const noexcept { return etch_; }
	/// How far up the plot Rust's band stands.
	static constexpr float kRustBandShare = 0.7f;
	[[nodiscard]] const fx::eroder::DustDerived& bandForTest() const noexcept { return band_; }
	/// The band's level at `hz`, dB below its peak: the engine's filter restated.
	[[nodiscard]] double bandDbForTest(double hz) const noexcept { return bandDb(hz); }
	[[nodiscard]] float handleXForTest() const noexcept { return hzToX(band_.freqHz); }
	[[nodiscard]] float edgeXForTest() const noexcept { return edgeX(); }
	[[nodiscard]] float edgeYForTest() const noexcept { return edgeY(); }
	[[nodiscard]] double edgeHzForTest() const noexcept { return edgeHz(); }
	/// How far from the edge handle a press still takes it, and the share of the plot's width that is all of Width.
	static constexpr float kEdgeReach = 7.0f;
	static constexpr float kWidthDragShare = 0.3f;
	static constexpr double kEdgeDb = -12.0;
	[[nodiscard]] float handleYForTest() const noexcept { return handleY(); }
	[[nodiscard]] float hzToXForTest(double hz) const noexcept { return hzToX(hz); }
	[[nodiscard]] double xToHzForTest(float x) const noexcept { return xToHz(x); }
	/// THE BAND'S AXIS: its foot (no travel) and its top (the most), in the frame's y.
	[[nodiscard]] float plotFootForTest() const noexcept { return plotFoot(); }
	[[nodiscard]] float plotTopForTest() const noexcept { return plotTop(); }
	[[nodiscard]] float amountToYForTest(double log2Seconds) const noexcept { return amountToY(log2Seconds); }
	[[nodiscard]] const std::array<float, kBars>& barsForTest() const noexcept { return bars_; }
	/// THE COMB'S ELEVEN BARS AND ITS DERIVED QUANTITIES, for the window's sub-plot, which draws them (`EroderSubplot`).
	[[nodiscard]] const std::array<float, kBars>& barValues() const noexcept { return bars_; }
	[[nodiscard]] const fx::eroder::Derived& combDerived() const noexcept { return comb_; }
	/// WHETHER THE PICTURE IS A SPECTRUM: every engine's in the window, all but the Comb's on the card.
	[[nodiscard]] bool showsSpectrumForTest() const noexcept { return spectrum_; }
	/// **WHERE THE COMB'S ENGINE STANDS ON THE AXIS**: its delay is a period, so it rings at 1/delay (the emphasis pivots there too).
	[[nodiscard]] double centreHzForTest() const noexcept { return centreHz(); }
	/// **THE COMB'S CUTOFF HANDLE** (the window's Comb plot): on the shake filter's centre (`Derived::svfHz`). `cutoffForX` inverts the engine's own
	/// mapping exactly where it is not clamped: the SVF's coefficient is `2 sin(pi hz / 44100)`, which is the reference coefficient times the
	/// note's ratio times 2^cutoff.
	[[nodiscard]] float cutoffHandleXForTest() const noexcept { return cutoffHandleX(); }
	[[nodiscard]] float cutoffHandleYForTest() const noexcept { return cutoffHandleY(); }
	[[nodiscard]] double cutoffForXForTest(float x) const noexcept { return cutoffForX(x); }
	[[nodiscard]] bool hasSpectrumForTest() const noexcept { return haveSpectrum_; }
	[[nodiscard]] float preDbForTest(size_t bin) const noexcept { return pre_[bin]; }
	[[nodiscard]] float postDbForTest(size_t bin) const noexcept { return post_[bin]; }
	[[nodiscard]] int recomputesForTest() const noexcept { return recomputes_; }
	/// **THE EMPHASIS CURVE'S dB AT `hz`** as drawn, or nothing where none is drawn (Emphasis at 0, or the Comb's bars,
	/// which have no frequency axis).
	[[nodiscard]] std::optional<double> emphasisDbForTest(double hz) const noexcept { return emphasisDb(hz); }
	/// How far up and down the plot the curve reaches: this many dB is a quarter of the plot's height from its middle.
	static constexpr double kEmphasisDbPerQuarter = 9.0;
	[[nodiscard]] float emphasisYForTest(double db) const noexcept { return emphasisY(db); }
	/// **THE EMPHASIS'S HANDLE** (the window's plot): its x is the emphasis's tuning on the plot's pitch axis, its y the
	/// curve's height there (the centre line at 0 dB). A drag moves it: across writes the offset, up and down the amount.
	[[nodiscard]] float emphasisHandleXForTest() const noexcept { return emphasisHandleX(); }
	[[nodiscard]] float emphasisHandleYForTest() const noexcept { return emphasisHandleY(); }
	[[nodiscard]] double emphasisHzForTest() const noexcept { return emphasisHz(); }

private:
	using Bins = std::array<float, feathers::dsp::kAnalyserBins>;

	[[nodiscard]] DeviceControl& control(fx::eroder::Param p) const;
	[[nodiscard]] float inset() const noexcept;
	[[nodiscard]] float plotTop() const noexcept;
	[[nodiscard]] float plotFoot() const noexcept;
	[[nodiscard]] float hzToX(double hz) const noexcept;
	[[nodiscard]] double xToHz(float x) const noexcept;
	[[nodiscard]] float dbToY(float db) const noexcept;
	[[nodiscard]] float amountToY(double log2Seconds) const noexcept;
	[[nodiscard]] double bandDb(double hz) const noexcept;
	/// THE EMPHASIS-IN'S RESPONSE, from the sections the engine builds (`EmphasisPair`, `shaper::tiltSection`), pivoting or
	/// centred on the engine's Freq; the Bell's width is Width's octaves.
	[[nodiscard]] std::optional<double> emphasisDb(double hz) const noexcept;
	[[nodiscard]] float emphasisY(double db) const noexcept;
	/// WHERE THE EMPHASIS IS TUNED: Freq and the offset (`SharedSection::emphasisHz`).
	[[nodiscard]] double emphasisHz() const noexcept;
	[[nodiscard]] float emphasisHandleX() const noexcept;
	[[nodiscard]] float emphasisHandleY() const noexcept;
	void dragEmphasis(visage::Point at);
	/// WHERE THE BAND IS `kEdgeDb` DOWN ABOVE ITS CENTRE, and the handle drawn there.
	[[nodiscard]] float handleY() const noexcept;
	[[nodiscard]] double edgeHz() const noexcept;
	[[nodiscard]] float edgeX() const noexcept;
	[[nodiscard]] float edgeY() const noexcept;
	[[nodiscard]] double pitchRatio() const noexcept;
	/// THE VALUE A PRESS AT `x` WRITES INTO FREQ: the pointer's hertz with the note's ratio taken back out.
	[[nodiscard]] double freqValueForX(float x) const noexcept;
	void drawDust(visage::Canvas& canvas);
	void drawComb(visage::Canvas& canvas);
	void drawCombMarks(visage::Canvas& canvas, float top, float foot);
	[[nodiscard]] float cutoffHandleX() const noexcept;
	[[nodiscard]] float cutoffHandleY() const noexcept;
	[[nodiscard]] double cutoffForX(float x) const noexcept;
	/// THE ENGINE'S OWN FREQUENCY: Freq (after tracking) in Dust, Rust and Etch, the comb's pitch in the Comb.
	[[nodiscard]] double centreHz() const noexcept;

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	bool large_ = false;
	mutable std::vector<std::unique_ptr<DeviceControl>> controls_;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;

	// WHAT THE PICTURE ON SCREEN WAS COMPUTED FROM — `update()`'s comparison.
	bool computed_ = false;
	fx::eroder::Row row_ {};
	fx::eroder::Settings settings_ {};
	bool dust_ = true;
	/// THE PICTURE IS THE SPECTRUM: not the Comb's bars. The window's Comb draws it too, with the comb's own marks.
	bool spectrum_ = true;
	bool rust_ = false;
	bool etch_ = false;
	double pitch_ = fx::eroder::kReferenceNote;
	double rate_ = 48000.0;
	fx::eroder::DustDerived band_ {};
	fx::eroder::Derived comb_ {};
	std::array<float, kBars> bars_ {};
	bool haveSpectrum_ = false;
	float frame_ = -1.0f;
	Bins pre_ {};
	Bins post_ {};
	bool armed_ = false;
	float routeAmount_ = 0.0f;
	bool routeBipolar_ = false;
	int recomputes_ = 0;

	struct Drag {
		bool active = false;
		bool route = false;
		bool amountGesture = false;
		float startY = 0.0f;
		double startAmount = 0.0;
		float anchor = 0.0f;
		bool width = false;
		/// THE EMPHASIS'S HANDLE: its two gestures are begun by the first movement past the dead zone on their own axis.
		bool emph = false;
		/// THE COMB'S CUTOFF HANDLE: a drag across places the shake filter's centre under the pointer.
		bool cutoff = false;
		bool offsetGesture = false;
		bool emphAmountGesture = false;
		double startOffset = 0.0;
		float startX = 0.0f;
		double startWidth = 0.0;
	} drag_ {};
};

} // namespace feathers::ui
