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
#include "fx/phaser/phaser_response.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/tooltip.h"

#include <visage/ui.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

// THE PHASER/FLANGER'S PICTURE — the live magnitude response, left bright and right faint, with the sweep's reach.
//
// **WHAT IT DRAWS IS `fx::phaser::responseModelOf`**, the engine's own pure functions (`phaser_response.h` says what
// that includes and what it leaves out), at the instant the device last published: the LFO's phase, the follower,
// the barberpole's ramp and the tracked note come from its readings (`responseInputsOf`), one model per channel.
//
// **THE SETTINGS ARE THE MODULATED ONES** (`DeviceHost::modulatedFieldAt`), not the stored row: Position is THE
// destination an external modulator drives the sweep through, and a picture drawn from the stored value would
// stand still while the sound moves. Every host in this build overrides `modulatedFieldAt` with where modulation
// left each control this block; a host that does not answers the stored value, which is then the truth. The
// pad's folded travel is not a control and has no modulated form, so it is read stored.
//
// WHAT IS ON IT:
//   - THE CURVES: one point per pixel of the curve's width, so a notch is never stepped over by a coarse grid.
//   - THE SWEEP BAND: the range the sweep can reach — Position ± LFO Depth (LFO on) ± |Env Amount| (follower on),
//     clamped to ±1 — shaded, in hertz: the phaser's corner (`phaserCornerHz`) or the flanger's comb tooth spacing,
//     1/delay (`flangerDelaySamples`; under through-zero, 1/|relative delay|, which runs off the top at the crossing).
//     Under the barberpole the band carries stripes moved by the published ramp phase — never by a clock — and held
//     still under reduced motion.
//   - THE POSITION TICK at the foot, in the value colour, where the MODULATED Position puts the sweep's centre.
//   - THE CENTRE HANDLE, a line through the plot at Center (or 1/Delay); in the value colour while a note is tracked.
//   - THE FLANGER'S UNTRUSTED BAND: above `kFlangerTrustedFraction`·sr the model is the ideal comb, not the engine's
//     kernel, so that part of the curve is drawn dim under a veil.
//
// **THE AXIS IS THE CENTER CONTROL** — Center in the Phaser, Delay in the Flanger (1/delay is the hertz it draws at).
// A press places it under the pointer and a drag moves it, with one gesture; with a source armed the press is a ROUTE
// drag, `Knob`'s gesture, as the disperser's plot does. **WHILE A NOTE IS TRACKED THE AXIS TAKES NO HAND**: the note
// sets the centre, so a drag there would move a value nobody hears (the honesty rule).
//
// **IT DOES ZERO WORK AT REST.** `update()` recomputes only when the settings, the readings, the rate or the width
// moved since the last picture; a running LFO moves the readings, so it recomputes then and only then.

namespace feathers::ui {

class ModState;
class EditHistory;

/// THE DEVICE'S SETTINGS AS THE HOST HOLDS THEM — the params `modulated` (where modulation left them) or stored, and
/// the pad's folded travel past them, exactly as the ops table's `setFields` decodes a row.
[[nodiscard]] fx::phaser::Settings phaserSettingsAt(const backplane::DeviceHost& host, DeviceInstanceId id,
                                                    bool modulated);

/// **A NOTE IS BEING TRACKED** — the published `Reading::NoteHz` nonzero, NOT the Key switch: Key Track with no note
/// yet leaves the knob's own Hz or ms sounding (Task 6's review). The one statement the card's and window's Center/
/// Delay readouts ask (`phaserOffsetText`), so the two cannot disagree about what the knob means.
[[nodiscard]] bool phaserNoteTracked(const backplane::DeviceHost& host, DeviceInstanceId id) noexcept;

/// **CENTER OR DELAY AS THE OFFSET FROM A TRACKED NOTE**, printed — `fx::phaser::keyOffsetSemitonesOf` in semitones,
/// signed to a tenth ("+7.0 st"), and a value that rounds to nothing reads "0.0 st", never "-0.0 st". `knobLog2` is
/// the stored value. What the boxes print while `phaserNoteTracked`; the descriptor's readout cannot know about the
/// note, so the card and window seat this.
[[nodiscard]] std::string phaserOffsetText(fx::phaser::Mode mode, double knobLog2);

class PhaserPlot final : public visage::Frame, public TooltipSource {
public:
	/// @param large the window's size: axis labels and a caption.
	PhaserPlot(backplane::DeviceHost& host, DeviceInstanceId id, bool large);

	void draw(visage::Canvas& canvas) override;
	void resized() override;
	const char* tooltipAt(visage::Point) const override;
	void offeredControls(std::vector<Offered>& out) const override;
	void setModulation(ModState* mod, EditHistory* history);

	/// RECOMPUTE IF AN INPUT CHANGED, and report whether the picture is out of date — the card's contract.
	bool update();

	//----------------------------------------------------------------------------------------------//
	// TEST SEAMS — the plotted VALUES.                                                               //
	//----------------------------------------------------------------------------------------------//
	[[nodiscard]] size_t pointCountForTest() const noexcept { return dbL_.size(); }
	[[nodiscard]] double hzAtForTest(size_t i) const noexcept { return hzAt(i); }
	[[nodiscard]] float dbAtForTest(size_t i, int channel) const noexcept { return channel == 0 ? dbL_[i] : dbR_[i]; }
	[[nodiscard]] const fx::phaser::ResponseModel& modelForTest(int channel) const noexcept {
		return channel == 0 ? modelL_ : modelR_;
	}
	/// HOW MANY TIMES THE CURVES HAVE BEEN COMPUTED — a count produced BY `recompute`, so "no work at rest" is asserted
	/// as a number rather than inferred from a picture.
	[[nodiscard]] int recomputesForTest() const noexcept { return recomputes_; }
	[[nodiscard]] float hzToXForTest(double hz) const noexcept { return hzToX(hz); }
	[[nodiscard]] double xToHzForTest(float x) const noexcept { return xToHz(x); }
	[[nodiscard]] float handleXForTest() const noexcept { return handleX(); }
	[[nodiscard]] float positionXForTest() const noexcept { return sweepToX(position_); }
	[[nodiscard]] std::pair<float, float> bandForTest() const noexcept { return { bandX0_, bandX1_ }; }
	/// THE STRIPES' OFFSET in pixels, from the barberpole's published ramp phase (0 under reduced motion).
	[[nodiscard]] float stripeOffsetForTest() const noexcept { return stripeOffset(); }
	/// THE CONTROL A DRAG ON THE AXIS WRITES — Center, or Delay in the Flanger.
	[[nodiscard]] size_t draggedControlForTest() const noexcept { return draggedControl(); }
	/// Whether a note is being tracked — `Reading::NoteHz` nonzero — so the centre knob is the offset from it.
	[[nodiscard]] bool trackingForTest() const noexcept { return tracking(); }
	/// THE HANDLE'S FEEDBACK RING, y, and the scale it is drawn on (+1 and −1).
	[[nodiscard]] float feedbackDotYForTest() const noexcept { return feedbackDotY(); }
	[[nodiscard]] float feedbackToYForTest(double feedback) const noexcept { return feedbackToY(feedback); }
	[[nodiscard]] double feedbackPerPixelForTest() const noexcept { return feedbackPerPixel(); }
	[[nodiscard]] double octavesPerPixelForTest() const noexcept { return octavesPerPixel(); }
	/// WHAT THE PLOT SAYS ON HOVER: both axes, in the mode's words (`tooltipAt`).
	static constexpr const char* kPhaserDragTip =
	    "Drag: Center across (the offset from the note while Key Track has one), Feedback up";
	static constexpr const char* kFlangerDragTip =
	    "Drag: Delay across, shorter to the right (the offset from the note while Key Track has one), Feedback up";

	/// FEEDBACK'S DRAG: the full −1..+1 over this many logical pixels, card and window alike, and the vertical dead
	/// zone a hand passes before Feedback is touched at all.
	static constexpr float kFeedbackDragSpan = 200.0f;
	static constexpr float kFeedbackDeadZone = 3.0f;
	static constexpr double kMinHz = 20.0;
	static constexpr double kMaxHz = 20000.0;
	static constexpr float kTopDb = 12.0f;
	static constexpr float kBottomDb = -36.0f;

private:
	[[nodiscard]] DeviceControl& control(size_t index) const;
	[[nodiscard]] size_t draggedControl() const noexcept;
	[[nodiscard]] bool tracking() const noexcept;
	[[nodiscard]] double hzAt(size_t i) const noexcept;
	[[nodiscard]] float inset() const noexcept;
	[[nodiscard]] float hzToX(double hz) const noexcept;
	[[nodiscard]] double xToHz(float x) const noexcept;
	[[nodiscard]] float dbToY(float db) const noexcept;
	/// WHERE A SWEEP VALUE `s` DRAWS — the phaser's corner, or the flanger's 1/delay — at the drawn settings.
	[[nodiscard]] float sweepToX(double s) const noexcept;
	[[nodiscard]] float handleX() const noexcept;
	[[nodiscard]] float stripeOffset() const noexcept;
	/// THE VALUE A DRAG NOW AT `x` WRITES into the dragged control — RELATIVE to the press: the hand's travel in the
	/// axis's own octaves, added to the value the press found (Delay's subtracted: a longer delay is lower). Relative
	/// because while a note is tracked the handle sits on the note's centre, not on the knob's, and an absolute map
	/// would jump the knob to wherever the press landed.
	[[nodiscard]] double valueForDrag(float x) const noexcept;
	/// THE AXIS'S OCTAVES PER LOGICAL PIXEL.
	[[nodiscard]] double octavesPerPixel() const noexcept;
	/// FEEDBACK A DRAG NOW AT `y` WRITES — relative to the press, up is more positive, the full ±1 across the
	/// handle's scale (`feedbackTop`..`feedbackFoot`).
	[[nodiscard]] double feedbackForDrag(float y) const noexcept;
	[[nodiscard]] double feedbackPerPixel() const noexcept;
	/// THE HANDLE'S VERTICAL SCALE: +1 at the top, −1 at the foot, 0 in the middle.
	[[nodiscard]] float feedbackTop() const noexcept;
	[[nodiscard]] float feedbackFoot() const noexcept;
	[[nodiscard]] float feedbackToY(double feedback) const noexcept;
	/// Where the handle's Feedback ring draws, at the drawn settings.
	[[nodiscard]] float feedbackDotY() const noexcept;
	[[nodiscard]] size_t wantedPoints() const noexcept;
	void recompute();

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	bool large_ = false;
	mutable std::vector<std::unique_ptr<DeviceControl>> controls_;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;

	fx::phaser::ResponseModel modelL_ {};
	fx::phaser::ResponseModel modelR_ {};
	std::vector<float> dbL_;
	std::vector<float> dbR_;
	/// THE SPAN EACH PIXEL COVERS, left channel: the lowest and highest dB of `kSubSamples` points across the pixel.
	/// A flanger's teeth crowd past one per pixel up the log axis, and one point per pixel then ALIASES them into a
	/// noise the sound does not have; the span is drawn as a bar under the line, so a crowded comb reads as a band.
	std::vector<float> loL_;
	std::vector<float> hiL_;
	float bandX0_ = 0.0f;
	float bandX1_ = 0.0f;
	double position_ = 0.0;

	// WHAT THE PICTURE ON SCREEN WAS COMPUTED FROM — `update()`'s comparison.
	bool computed_ = false;
	fx::phaser::Settings drawnSettings_ {};
	std::array<float, fx::phaser::kReadingCount> drawnReadings_ {};
	double drawnRate_ = 0.0;
	size_t drawnPoints_ = 0;
	bool drawnArmed_ = false;
	float drawnAmount_ = 0.0f;
	int recomputes_ = 0;

	struct Drag {
		bool active = false;
		bool route = false;
		float startX = 0.0f;
		float startY = 0.0f;
		float anchor = 0.0f;
		double startValue = 0.0;    ///< the dragged control's stored value at the press (or the last re-anchor)
		double startFeedback = 0.0; ///< Feedback's, likewise
		bool fine = false;          ///< Shift, as the drag is now anchored
		bool xOpen = false;         ///< the centre control's gesture is open (its first real change came)
		bool yOpen = false;         ///< Feedback's is (the hand passed the dead zone)
		size_t control = 0;
	} drag_ {};
};

} // namespace feathers::ui
