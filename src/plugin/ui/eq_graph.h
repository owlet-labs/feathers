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

// THE EQ'S PLOT — ONE class, drawn on the card and in the panel
// (OwlttGraph's shape): the grid, the BASE response (the bands' stored values) with a handle per band on it, the
// HEARD response (after the globals, modulation and Key Track) drawn over it wherever the two part (backlog MP), a
// rail of markers for the bands that are off, the tilt's pivot diamond, and the crosshair readout.
//
// THE SAME `eq::` FUNCTIONS THE DSP RUNS draw the curve (R5: one magnitude function), so what is drawn is
// what is heard, to the design library's own arithmetic.

#include "backplane/device_host.h"
#include "fx/eq/eq_types.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/control_face.h"
#include "plugin/ui/eq_plot.h"

#include <array>
#include <functional>
#include <string>
#include <visage/ui.h>

namespace feathers::ui {

class EqGraph final : public visage::Frame, public ControlFace {
public:
	/// See `ControlFace` — what this surface offers, so the window-superset guard can see it.
	void offeredControls(std::vector<Offered>& out) const override;
	/// `control(i)` is the owner's adapter for control `i` — the SAME `DeviceControl` its knobs hold, so a
	/// drag here and a knob there are one gesture history.
	using ControlFn = std::function<DeviceControl&(size_t)>;

	EqGraph(backplane::DeviceHost& host, DeviceInstanceId id, ControlFn control, bool compact);
	EqGraph(backplane::DeviceHost&, size_t, ControlFn, bool) = delete;

	void draw(visage::Canvas& canvas) override;

	/// Redraw if anything drawn moved; true if it redrew.
	bool update();

	/// Called after the selected band changes (the owner re-lays its strip).
	std::function<void(int)> onSelected;

	/// The selected band, from the extra-state block.
	[[nodiscard]] int selected() const;
	void select(int band);

	/// The dB range the plot spans: the panel's saved zoom, or ±24 on the card.
	[[nodiscard]] float rangeDb() const;
	[[nodiscard]] eq_plot::Geometry geometry() const;

	/// What the plot draws, computed as the DSP computes it. TWO OF THEM (backlog MP: the base curve stays
	/// still, and the effective curve, which does move, is shown separately without handles): the
	/// BASE, the bands' stored values with Offset, Amount and Tilt at identity, carries the handles and the hit tests, so
	/// a band stays where the hand can take it whatever host automation or modulation does to a global; the LIVE one,
	/// from where modulation left every control plus Key Track's shift, is the response heard, drawn over the base
	/// quieter and with no handles wherever it departs from it — which a non-default Offset, Amount or Tilt does even at
	/// rest: base against heard, honestly two curves (shape B, by design).
	struct View {
		std::array<fx::eq::EffectiveBand, fx::eq::kBandCount> bands {};
		std::array<fx::eq::BandDesign, fx::eq::kBandCount> designs {};
		fx::eq::TiltDesign tilt {};
		fx::eq::Globals globals {};
		std::array<double, fx::eq::kBandCount> unclampedHz {};
		/// Each band's channel, and the VIEW (step 6): which path the curve and the analyser show.
		std::array<fx::eq::Channel, fx::eq::kBandCount> channel {};
		fx::eq::Channel viewChannel = fx::eq::Channel::Stereo;
		double sampleRate = 48000.0;
	};
	[[nodiscard]] View view() const;     ///< the LIVE response: what is heard
	[[nodiscard]] View baseView() const; ///< the BASE: the stored values, where the handles sit
	/// Whether two views draw the same response — when they do, only the base curve is drawn.
	[[nodiscard]] static bool sameResponse(const View& a, const View& b);

	/// THE VIEW'S BANDS (step 6, doc §7). A Stereo band acts on every path; a Mid or Side band only on its
	/// own. So the Mid path is the Stereo bands and the Mid bands, the Side path likewise, and the Stereo
	/// view — left and right as such — the Stereo bands alone. A band outside the view is drawn dimmed and
	/// left out of the curve.
	[[nodiscard]] static bool inPath(fx::eq::Channel band, fx::eq::Channel path) noexcept {
		return band == fx::eq::Channel::Stereo || band == path;
	}
	/// The response of one path in dB at `hz` — the curve, and the crosshair's number.
	[[nodiscard]] static double pathDb(const View& v, fx::eq::Channel path, double hz);
	/// Whether any Mid or Side band is acting — the Stereo view then also draws the two paths faintly.
	[[nodiscard]] static bool anyMidSide(const View& v);
	/// The view, from the extra-state block.
	[[nodiscard]] fx::eq::Channel viewChannel() const;
	/// KEY TRACK'S SHIFT AS THE DEVICE APPLIES IT (backlog MQ): octaves, log2(f0 / C3), read from its readings; 0
	/// with Key Track off or nothing published yet.
	[[nodiscard]] double keyOctaves() const;

	/// WHERE A BAND'S HANDLE SITS — asked by the drawing and the hit test both (OWLTT's lesson: one
	/// number, not two that must agree). Asked of the BASE view (MP, which replaced R13's "on the heard curve"):
	/// its STORED frequency, and its stored gain for a gain band (before Offset and Amount), or the 0 dB line for a
	/// cut or notch. An off band's marker sits on the rail under the plot.
	[[nodiscard]] visage::Point handleAt(const View& v, int band) const;
	[[nodiscard]] visage::Bounds plotBounds() const;
	[[nodiscard]] visage::Bounds railBounds() const;
	[[nodiscard]] visage::Point pivotAt(const View& v) const;
	[[nodiscard]] int bandAt(visage::Point p, const View& v) const;

	/// The crosshair's text at a point: "A3 +0c / 440.0 Hz   EQ +3.2 dB   pre -24 post -21 dB" (a tap
	/// that is off is left out).
	[[nodiscard]] std::string crosshairText(visage::Point p) const;

	/// THE ANALYSER AS DRAWN (doc §4.2–4.3): the latest whole frame the device published, smoothed with
	/// the chosen release (attack is instant), and the post tap's held peaks. dB, before the view's tilt.
	struct AnalyserView {
		std::array<float, fx::eq::kAnalyserBins> pre {};
		std::array<float, fx::eq::kAnalyserBins> post {};
		std::array<float, fx::eq::kAnalyserBins> peak {};
		int framesTaken = 0;
		int framesDropped = 0;
		int framesOtherView = 0;
	};
	[[nodiscard]] const AnalyserView& analyserForTest() const { return analyser_; }
	/// The analyser settings, read from the extra-state block.
	[[nodiscard]] bool preOn() const;
	[[nodiscard]] bool postOn() const;
	[[nodiscard]] float analyserTiltDb() const;
	/// The analyser's level at `hz` as drawn — interpolated between bins, with the view's tilt.
	[[nodiscard]] float analyserDbAt(const std::array<float, fx::eq::kAnalyserBins>& bins, double hz) const;
	/// The analyser's own axis: 0 dBFS at the top of the plot, -90 at the bottom.
	static constexpr float kAnalyserRangeDb = 90.0f;
	/// The time between two `update` calls, the editor's tick (`Gui::kTimerPeriodMs`).
	static constexpr float kTickSeconds = 0.016f;

	static constexpr float kHandleRadius = 5.5f;
	static constexpr float kGrabRadius = 11.0f;
	static constexpr float kRailHeight = 12.0f;
	/// Pixels of vertical drag per octave of Q, for the cuts and the notch (whose y is not a gain).
	static constexpr float kPxPerQOctave = 40.0f;
	/// Shift's fine factor, the house one (`Knob::kFineFactor`).
	static constexpr float kFineFactor = 0.2f;

private:
	[[nodiscard]] float field(size_t i) const;
	[[nodiscard]] float modulated(size_t i) const;
	void drawGrid(visage::Canvas& canvas, const eq_plot::Geometry& g);
	[[nodiscard]] View viewOf(bool live) const;
	void drawCurve(visage::Canvas& canvas, const View& v, const eq_plot::Geometry& g, bool live);
	void drawHandles(visage::Canvas& canvas, const View& v);
	void drawCrosshair(visage::Canvas& canvas, const eq_plot::Geometry& g);
	void showTypeMenu(int band, visage::Point at);
	void beginDrag(int band, visage::Point p, bool fine);
	void dragTo(visage::Point p, bool fine);
	void endDrag();
	void showRangeMenu(visage::Point at);
	void showAnalyserMenu(visage::Point at);
	void showViewMenu(visage::Point at);
	[[nodiscard]] visage::Bounds viewChipBounds() const;
	[[nodiscard]] visage::Bounds analyserChipBounds() const;
	[[nodiscard]] int extraChoice(fx::eq::Extra e, int count) const;
	void setExtra(fx::eq::Extra e, float value);
	void drawAnalyser(visage::Canvas& canvas, const eq_plot::Geometry& g);
	/// Take a new frame if one whole one is there; step the smoothing. True if anything drawn moved.
	bool advanceAnalyser();

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	ControlFn control_;
	bool compact_ = false;

	struct Drag {
		bool active = false;
		bool pivot = false;
		int band = -1;
		bool yIsGain = true;
		bool fine = false;
		visage::Point anchor {};
		double anchorFreqLog2 = 0.0;
		double anchorY = 0.0; // gain dB or log2 Q at the anchor
		visage::Point grabOffset {};
	};
	Drag drag_ {};

	bool hovering_ = false;
	visage::Point hover_ {};
	std::vector<float> drawnFields_;
	std::vector<float> scratchFields_; ///< `update`'s working row, kept so a tick at rest allocates nothing

	AnalyserView analyser_ {};
	std::array<float, fx::eq::kAnalyserBins> rawPre_ {};
	std::array<float, fx::eq::kAnalyserBins> rawPost_ {};
	std::array<float, fx::eq::kAnalyserBins> peakAge_ {};
	float lastFrame_ = -1.0f;
	int shownView_ = 0;
};

} // namespace feathers::ui
