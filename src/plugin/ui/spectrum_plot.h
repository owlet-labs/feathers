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

#include "dsp/spectral_shaper.h"
#include "plugin/gui_bridge.h"
#include "plugin/params.h"
#include "plugin/spectral_plot.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/spectrum_axes.h"
#include "plugin/ui/spectrum_source.h"
#include "plugin/ui/tooltip.h"

#include <array>
#include <memory>
#include <optional>
#include <visage/ui.h>

// THE SPECTRUM PANEL'S PLOT.
//
// WHAT IT DRAWS IS WHAT IS HEARD: the engine's frame (spectral_plot.h, published while the panel is open) — the newest
// voice's partials, or with no note the next note's at the last pitch — as the terrain's bars (dark) and the result's
// (the value colour; the two ears apart, in the stereo pair's tints, when they differ). That is the BLEND of the sounds'
// drawers (Synth::drawerFor); the handles and the drawn curve are the SELECTED sound's, the controls the row edits.
//
// THE AXES: frequency in Hz on a log scale, the note's harmonic numbers along the top; level in dB, the frame's loudest
// terrain partial at 0. UNITS FOLLOW WHAT A CONTROL TRACKS: the comb's spacing and the curve are
// harmonic, so they ride the note; the filter's cutoff reads in harmonics while Key Track follows the note and in Hz
// while it does not; the crossover is Hz. TILT IS DRAWN AS THE ENGINE APPLIES IT, COMPENSATED (its level held): its line is
// spectral::gains() of a drawer holding only the selected sound's Tilt, over the frame's terrain — the shaper's own
// arithmetic, called, not restated — and its grip rides the line's right end. Compensation removes the pivot, so it has
// no pivot handle — only the grip, whose height sets the amount.
//
// THE HONESTY RULE ON THE PLOT: a handle is drawn, and grabbed, only while its operator is on, the master switch is on
// and the panel acts for the sound; a handle axis is live only while its control changes sound (the filter's height is
// Resonance, which Notch ignores, so under Notch the corner moves across and not up).
//
// THE CURVE IS DRAWN with the Draw tool (the panel's chip, live while the sound and the master are; a stroke on an off
// curve switches it on, in the same undo step, as the sketch does): one band per harmonic, the
// stroke interpolated across the bands a fast hand skips, and the bottom of the range SNAPPING TO AN EXACT ZERO GAIN —
// silence — so a band can be drawn out entirely (−36 dB is not silence). A stroke is one undo step (CurveEdit).

namespace feathers::ui {

class ParamControl;

/// **ONE UNDO STEP OVER A SOUND'S DRAWN CURVE**: a stroke on the plot, or Flatten. The curve is stored state, not a
/// param, so the host's undo never sees it; this is the editor's.
class CurveEdit final : public EditRecord {
public:
	CurveEdit(ParamBridge& bridge, size_t slot, const spectral::DrawnCurve& before, const spectral::DrawnCurve& after,
	          const char* verb)
	    : CurveEdit(BridgeSpectrumSource(bridge, nullptr).curveWriter(slot), slot, before, after, verb) {}
	/// Over any source's writer (SpectrumSource::curveWriter), which outlives the editor that recorded the edit.
	CurveEdit(std::function<void(const spectral::DrawnCurve&)> write, size_t slot, const spectral::DrawnCurve& before,
	          const spectral::DrawnCurve& after, const char* verb)
	    : write_(std::move(write)), slot_(slot), before_(before), after_(after), verb_(verb) {}
	void undo() const override { write_(before_); }
	void redo() const override { write_(after_); }
	const char* verb() const override { return verb_; }
	std::optional<size_t> slot() const override { return slot_; }

private:
	std::function<void(const spectral::DrawnCurve&)> write_;
	size_t slot_;
	spectral::DrawnCurve before_;
	spectral::DrawnCurve after_;
	const char* verb_;
};

class SpectrumPlot final : public visage::Frame, public TooltipSource {
public:
	/// A HANDLE ANSWERS FOR THE CONTROL IT MOVES ACROSS (or up, for Tilt's grip): that control's own declaration.
	const char* tooltipAt(visage::Point local) const override;
	/// The plot's controls are the operator row's and the card's widgets too; it declares none of its own.
	void offeredControls(std::vector<Offered>&) const override {}
	/// THE AXES, in one place.
	static constexpr double kMinHz = spectrum_axes::kMinHz;
	static constexpr double kMaxHz = spectrum_axes::kMaxHz;
	static constexpr double kTopDb = spectrum_axes::kTopDb;
	static constexpr double kBottomDb = spectrum_axes::kBottomDb;
	/// THE CURVE'S FLOOR: a stroke at or below this level writes a gain of EXACTLY 0 (lead's condition, D2).
	static constexpr double kCurveFloorDb = -36.0;
	static constexpr float kPadLeft = 30.0f;
	static constexpr float kPadRight = 8.0f;
	static constexpr float kPadTop = 16.0f;
	static constexpr float kPadBottom = 14.0f;
	/// How near a press must land to grab a handle, in logical pixels.
	static constexpr float kGrab = 10.0f;
	/// THE HANDLES' HEIGHTS, the sketch's: the filter corner at −6 dB plus 14 dB of Resonance; the comb tooth at its depth
	/// times −30 dB; Formant Shift's arrows at +4 dB, their anchors a fixed 1 kHz linked and half or twice the crossover
	/// unlinked; the crossover's grip near the floor. (Tilt's grip is on its compensated line: tiltLine.)
	static constexpr double kFilterBaseDb = -6.0;
	static constexpr double kFilterSpanDb = 14.0;
	static constexpr double kCombSpanDb = 30.0;
	static constexpr double kShiftDb = 4.0;
	static constexpr double kShiftAnchorHz = 1000.0;
	static constexpr double kCrossoverDb = -42.0;
	/// The vowel at Depth 0 sits at kVowelBaseDb, at Depth 1 kVowelSpanDb above; Shift's marker at kShiftMarkerDb; the Partials
	/// cut's grip at kCutGripDb, near the floor.
	static constexpr double kVowelBaseDb = -40.0;
	static constexpr double kVowelSpanDb = 40.0;
	static constexpr double kShiftMarkerDb = 8.0;
	static constexpr double kCutGripDb = -44.0;
	/// The pitch the axes are read at before the engine has published a frame.
	static constexpr double kRestF0 = 110.0;
	/// Narrower than this the plot is COMPACT (a device card's): only the handle in the hand is labelled.
	static constexpr float kCompactWidth = 400.0f;

	/// THE LATER FIVE'S HANDLES (spectral-ops, 2026-10-12): the vowel (across = Vowel along PhiVox's eight anchors, which
	/// is a walk and not a frequency, so it is laid out evenly across the plot; up = Depth), Shift's marker (the fundamental
	/// moved by the shift, dragged across in Hz) and the Partials cut (a line between the last partial kept and the first cut,
	/// dragged across in harmonics). Damp and Stretch have none.
	/// F2: Filter2 is the second filter's corner (a Bell's centre), drawn in its own colour; the SELECTED filter's handle
	/// (SpectrumSource::selectedFilter) is drawn in front and grabbed first, and grabbing either selects its tab.
	enum class Handle : uint8_t { Filter, Comb, ShiftLow, ShiftHigh, Crossover, Tilt, Vowel, ShiftMarker, PartialsCut, Filter2, kCount };
	/// A BELL'S HANDLE HEIGHT: kFilterBaseDb at 0 dB, kBellDbScale dB of plot per dB of Gain (±24 dB spans −24..+12 on the axis).
	static constexpr double kBellDbScale = 0.75;

	explicit SpectrumPlot(ParamBridge& bridge);
	/// Over any source (spectrum_source.h): the drawer's through the bridge, or a device's. `source` outlives the plot.
	explicit SpectrumPlot(SpectrumSource& source);
	~SpectrumPlot() override;

	void draw(visage::Canvas& canvas) override;
	void setHistory(EditHistory* history) { history_ = history; }
	/// ONCE A FRAME (the panel's sync): take a new engine frame when one has been published, read the selected sound's
	/// handle settings, and repaint only when either moved. `live`: the panel acts for the sound (Additive, the sound in the constellation). `draw`:
	/// the Draw tool is on. Answers whether it repainted.
	bool sync(size_t selected, bool live, bool draw);

	// ---- the mappings, public so a case restates nothing --------------------------------------------------------
	[[nodiscard]] float hzToX(double hz) const;
	[[nodiscard]] double xToHz(float x) const;
	[[nodiscard]] float dbToY(double db) const;
	[[nodiscard]] double yToDb(float y) const;
	/// The vowel's position (0..7) at plot x, and back: PhiVox's anchors walked evenly across the plot (a walk, not Hz).
	[[nodiscard]] float vowelToX(double position) const;
	[[nodiscard]] double xToVowel(float x) const;
	/// The curve gain a stroke at height `y` writes: 0 at or below the floor, else the level's amplitude, at most
	/// kSpectralCurveGainMax.
	[[nodiscard]] float curveGainAtY(float y) const;
	/// The note the axes and the harmonic handles are read at: the frame's, or kRestF0 before one.
	[[nodiscard]] double f0() const;

	// ---- test seams ----------------------------------------------------------------------------------------------
	[[nodiscard]] const SpectralPlotFrame& frameForTest() const { return frame_; }
	[[nodiscard]] uint32_t serialForTest() const { return shownSerial_; }
	[[nodiscard]] int redrawsForTest() const { return redraws_; }
	/// Where a handle is drawn, or nothing when it is not (its operator off, the master off, the panel dead).
	[[nodiscard]] std::optional<visage::Point> handleForTest(Handle h) const;
	/// Whether a drag on `h` moves its control up and down as well as across.
	[[nodiscard]] bool verticalForTest(Handle h) const { return vertical(h); }
	/// The text drawn beside a handle (its value, in the units its control tracks).
	[[nodiscard]] std::string labelForTest(Handle h) const;
	/// WHICH NOTE THE PLOT SHOWS: the playing note's name, or "reference · <note>" when no note plays (the next note's, at
	/// the last pitch); empty before a frame.
	[[nodiscard]] std::string noteLabel() const;
	/// THE SELECTED SOUND'S TILT LINE at `dbPerOctave`, as the engine applies it over the frame's terrain: each partial's
	/// compensated factor from spectral::gains() with Tilt the only operator. Answers how many partials it wrote.
	int tiltLine(double dbPerOctave, float* out) const;
	/// CLOSE THE GESTURE IN FLIGHT, if any, as a release does (the stroke recorded, a handle's host gestures ended) — the
	/// panel calls it when the selected sound changes under a hand.
	void endGesture() { release(); }
	void pressForTest(visage::Point p) { press(p); }
	void dragForTest(visage::Point p) { drag(p); }
	void releaseForTest() { release(); }

private:
	/// The selected sound's settings the handles and the curve are drawn from — `sync`'s comparison.
	struct View {
		size_t slot = 0;
		bool live = false;
		bool master = false;
		bool draw = false;
		std::array<double, static_cast<size_t>(SpectralControl::kCount)> value {};
		spectral::DrawnCurve curve {};
		friend bool operator==(const View&, const View&) = default;
	};
	[[nodiscard]] View viewNow(size_t slot, bool live, bool draw) const;
	[[nodiscard]] double value(SpectralControl c) const { return view_.value[static_cast<size_t>(c)]; }
	[[nodiscard]] bool on(SpectralControl c) const;
	[[nodiscard]] float plotLeft() const { return kPadLeft; }
	[[nodiscard]] float plotRight() const { return width() - kPadRight; }
	[[nodiscard]] float plotTop() const { return kPadTop; }
	[[nodiscard]] float plotBottom() const { return height() - kPadBottom; }
	/// The tracked cutoff in Hz at the frame's note (spectral_shaper.cpp's trackedCutoff, restated over the param), of filter
	/// `filter` (0 or 1).
	[[nodiscard]] double trackedCutoffHz(int filter = 0) const;
	/// Whether filter `filter` is a Bell (its handle's height is then its Gain).
	[[nodiscard]] bool bell(int filter) const;
	/// The handles in the order they are drawn and offered to a press: the selected filter's LAST, so it paints in front.
	[[nodiscard]] std::array<Handle, static_cast<size_t>(Handle::kCount)> handleOrder() const;
	/// Formant Shift's two anchors: where a zero shift would put each arrow.
	[[nodiscard]] double shiftAnchorHz(bool high) const;
	/// WHERE THE PARTIALS CUT STANDS for `count`: between the last partial kept and the first cut, at the MOVED ratios — the
	/// selected sound's Count, Stretch and Shift through the voice's own pitchPartials, over a harmonic spectrum (the handle is
	/// the selected sound's, not the frame's blend). Nothing when no partial is kept.
	[[nodiscard]] std::optional<double> cutHz(double count) const;
	[[nodiscard]] std::optional<visage::Point> handleAt(Handle h) const;
	[[nodiscard]] std::string label(Handle h) const;
	/// The band a stroke at `x` paints (harmonic b + 1 nearest the pointer), or nothing below the fundamental.
	[[nodiscard]] std::optional<size_t> bandAt(float x) const;
	[[nodiscard]] bool canDraw() const;
	/// Whether a drag on `h` moves its control up and down as well as across (the Filter not under Notch, the Comb, Tilt).
	[[nodiscard]] bool vertical(Handle h) const;
	/// The height a gain draws at: its level in dB, the floor at the bottom (unclamped above: a caller clamps to draw).
	[[nodiscard]] float lineY(float gain) const;
	/// Where a Tilt line computed by tiltLine ends: its highest partial on the axis, or nothing with fewer than two there (a
	/// line of one point has no slope for the grip to set).
	[[nodiscard]] std::optional<visage::Point> tiltEnd(const float* line, int count, bool clamped) const;
	/// Where Tilt's grip sits for `dbPerOctave`: the right end of its line (the highest partial on the axis).
	/// `clamped`: held inside the plot, as it is drawn; unclamped, the line end's true height (a drag above the plot reaches past
	/// the top of the axis).
	[[nodiscard]] std::optional<visage::Point> tiltGrip(double dbPerOctave, bool clamped = true) const;
	/// Write `value` through an axis's control, opening its gesture first if this is the first value that moves it.
	void writeAxis(std::unique_ptr<device::Modulatable>& control, SpectralControl c, double value);

	void press(visage::Point p);
	void drag(visage::Point p);
	void release();
	/// Paint the curve from the last stroke point to `p`, every band between them at the height the line crosses it.
	void stroke(visage::Point p);
	/// Write the dragged handle's control(s) from the pointer.
	void moveHandle(visage::Point p);

	void drawGrid(visage::Canvas& canvas);
	void drawBars(visage::Canvas& canvas);
	void drawOverlay(visage::Canvas& canvas);
	void drawCurve(visage::Canvas& canvas);
	void drawHandles(visage::Canvas& canvas);

	/// THE SOURCE: owned when built over the bridge (the drawer's constructor), borrowed otherwise.
	// THE OWNER of the bridge source this plot builds for itself; it is read only to bind `source_` and exists to keep
	// that source alive, which newer clang reports as an unused private field.
	[[maybe_unused]] std::unique_ptr<SpectrumSource> ownedSource_;
	SpectrumSource& source_;
	EditHistory* history_ = nullptr;
	SpectralPlotFrame frame_ {};
	uint32_t shownSerial_ = 0;
	/// The newest serial published before the plot was built (a fresh panel every open): frames at or below it are not
	/// taken (sync).
	uint32_t staleThrough_ = 0;
	View view_ {};
	bool primed_ = false;
	int redraws_ = 0;

	/// THE GESTURE IN FLIGHT: a handle's (the controls each axis writes, each opened for the host's undo on the first
	/// value that MOVES it, as a knob opens its own — a bare click opens nothing) or a stroke's (the curve before it, for
	/// CurveEdit, and the last point painted).
	struct Gesture {
		enum class Kind : uint8_t { None, Handle, Stroke } kind = Kind::None;
		Handle handle = Handle::Filter;
		size_t slot = 0;
		SpectralControl xControl = SpectralControl::kCount; ///< kCount: the axis writes nothing
		SpectralControl yControl = SpectralControl::kCount;
		std::unique_ptr<device::Modulatable> x;
		std::unique_ptr<device::Modulatable> y;
		spectral::DrawnCurve before {};
		visage::Point last {};
		/// From the press to the handle's centre, kept for the drag, so an off-centre press does not make the handle jump.
		visage::Point offset {};
	} gesture_ {};
};

} // namespace feathers::ui
