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
#include "fx/disperser/disperser_response.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/tooltip.h"

#include <visage/ui.h>

#include <array>
#include <memory>
#include <vector>

// THE DISPERSER'S PICTURE — group delay against frequency, the curve a transient is smeared along.
//
// **WHAT IT DRAWS IS `fx::disperser::responseModelOf`** — the engine's own `layoutStages` and
// `allpassCoeffs`, through the whole routing, on the message thread. `disperser_response.h` says what that
// includes (every routing's blends, the emphasis gains) and what it leaves out (the echoes; the two slow
// stereo LFOs, drawn at their centre). The caption says the second half on the face.
//
// **THE FREQ HANDLE IS ON THE CURVE'S OWN AXIS.** A press anywhere on the plot places the centre under the
// pointer and a drag moves it — Freq when Key Track is off, Offset from the note when it is on (with no note
// yet, the Offset knob read as the absolute knob it is on the hardware). With a source armed the same press is
// a ROUTE drag, `Knob`'s gesture, and the reach is drawn along the axis — the plot is a control and keeps a
// control's whole modulation display.
//
// **THE HANDLE ALSO RIDES UP AND DOWN: ITS HEIGHT IS THE Q OFFSET** (in the window, x is the offset and a
// y-axis drag modifies Q). The middle of the plot is no offset, the top `kQOffsetOctaves` sharper,
// the bottom as many broader, one octave of Q to a sixth of the height. A drag moves it RELATIVELY, from where the
// press landed, and only once the pointer has left a `kQDeadZone` band, so a press that sets the centre and a
// sideways drag never touch Q. **ON THE CARD TOO, since backlog NH (the Q handle in the card's plot did
// not respond)** — the 2026-10-03 decision kept the card's handle on its foot and X only, and the card drew
// the same dot the window drags for Q, so a hand reached for it and nothing moved. Same road, same law, smaller
// plot; the "Q ±n oct" readout stays the window's.
// With a source armed the vertical drag stays the ROUTE's amount, as above; the Q offset's own routes are made on
// its box in the window.
//
// **THE DELAY AXIS DOES NOT REFIT UNDER A DRAG.** Group delay spans a millisecond to seconds, so the axis is
// logarithmic and its top snaps to a decade, moving only when the curve crosses that decade with room to
// spare (up past 95% of it, down under 40% of the one below) — a drag never makes the curve jump scale
// under the cursor.
//
// **IT REPAINTS ONLY WHEN WHAT IT DRAWS CHANGES**: `update()` compares the settings, the note and the arm
// state against what the last frame was drawn from.

namespace feathers::ui {

class ModState;
class EditHistory;

class DisperserPlot final : public visage::Frame, public TooltipSource {
public:
	/// @param large the panel's size: more curve points, axis labels, per-stage Q as tick height.
	DisperserPlot(backplane::DeviceHost& host, DeviceInstanceId id, bool large);

	void draw(visage::Canvas& canvas) override;
	const char* tooltipAt(visage::Point) const override;
	void offeredControls(std::vector<Offered>& out) const override;
	void setModulation(ModState* mod, EditHistory* history);

	/// RECOMPUTE IF AN INPUT CHANGED, and report whether it did — the card's `updateContent` contract. Asks
	/// for the frame itself when it did.
	bool update();

	//----------------------------------------------------------------------------------------------//
	// TEST SEAMS — the plotted VALUES, so a case asserts the picture's numbers.                      //
	//----------------------------------------------------------------------------------------------//
	[[nodiscard]] const fx::disperser::ResponseModel& modelForTest() const noexcept { return model_; }
	/// The delay axis: its top and bottom in ms.
	[[nodiscard]] double axisTopMsForTest() const noexcept { return axisTopMs_; }
	[[nodiscard]] double axisBottomMsForTest() const noexcept { return axisTopMs_ / kAxisSpan; }
	/// The plotted curve: `hzAt(i)` and the L/R delay at it, in ms.
	[[nodiscard]] size_t pointCountForTest() const noexcept { return delayL_.size(); }
	[[nodiscard]] double hzAtForTest(size_t i) const noexcept { return hzAt(i); }
	[[nodiscard]] double delayMsForTest(size_t i, int channel) const noexcept {
		return channel == 0 ? delayL_[i] : delayR_[i];
	}
	/// Where the handle sits, in the frame's x.
	[[nodiscard]] float markerXForTest() const noexcept { return hzToX(model_.markerHz); }
	[[nodiscard]] float hzToXForTest(double hz) const noexcept { return hzToX(hz); }
	[[nodiscard]] double xToHzForTest(float x) const noexcept { return xToHz(x); }
	/// THE CONTROL A DRAG ON THE AXIS WRITES — Freq, or Offset when Key Track is on.
	[[nodiscard]] size_t draggedControlForTest() const noexcept { return draggedControl(); }
	[[nodiscard]] int redrawsForTest() const noexcept { return recomputes_; }
	/// THE Q HANDLE: where it sits, and the height an offset maps to — the inverse of the drag.
	[[nodiscard]] float qHandleYForTest() const noexcept { return qOffsetToY(static_cast<double>(drawnSettings_.qOffsetOctaves)); }
	[[nodiscard]] float qOffsetToYForTest(double octaves) const noexcept { return qOffsetToY(octaves); }
	[[nodiscard]] float pixelsPerQOctaveForTest() const noexcept { return pixelsPerQOctave(); }

	static constexpr double kMinHz = 20.0;
	static constexpr double kMaxHz = 20000.0;
	/// THREE DECADES OF DELAY under the snapped top.
	static constexpr double kAxisSpan = 1000.0;
	/// HOW FAR THE POINTER MOVES UP OR DOWN BEFORE A DRAG TOUCHES THE Q OFFSET, in logical pixels.
	static constexpr float kQDeadZone = 3.0f;

private:
	[[nodiscard]] DeviceControl& control(size_t index) const;
	[[nodiscard]] size_t draggedControl() const noexcept;
	[[nodiscard]] fx::disperser::Settings settingsNow() const;
	[[nodiscard]] double hzAt(size_t i) const noexcept;
	[[nodiscard]] float hzToX(double hz) const noexcept;
	[[nodiscard]] double xToHz(float x) const noexcept;
	[[nodiscard]] float msToY(double ms) const noexcept;
	/// THE Q OFFSET'S HEIGHT: the plot's middle at 0, its top and bottom at `±kQOffsetOctaves`.
	[[nodiscard]] float qOffsetToY(double octaves) const noexcept;
	[[nodiscard]] float pixelsPerQOctave() const noexcept;
	[[nodiscard]] float plotTop() const noexcept;
	[[nodiscard]] float plotFoot() const noexcept;
	/// THE VALUE A PRESS AT `x` WRITES into the dragged control.
	[[nodiscard]] double valueForX(float x) const noexcept;
	void recompute(const fx::disperser::Settings& s, bool haveNote, double note, const float* gains);
	void snapAxis();

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	bool large_ = false;
	mutable std::vector<std::unique_ptr<DeviceControl>> controls_;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;

	fx::disperser::ResponseModel model_ {};
	std::vector<double> delayL_;
	std::vector<double> delayR_;
	double axisTopMs_ = 10.0;

	// WHAT THE PICTURE ON SCREEN WAS COMPUTED FROM — `update()`'s comparison.
	bool computed_ = false;
	fx::disperser::Settings drawnSettings_ {};
	bool drawnHeard_ = false;
	double drawnNote_ = 0.0;
	double drawnRate_ = 0.0;
	bool drawnHasGains_ = false;
	std::array<float, fx::disperser::kMaxStages> drawnGains_ {};
	bool drawnArmed_ = false;
	float drawnAmount_ = 0.0f;
	int recomputes_ = 0;

	struct Drag {
		bool active = false;
		bool route = false;
		float startY = 0.0f;
		double startQ = 0.0; ///< the Q offset at the press — the vertical drag is relative to it
		bool qGesture = false; ///< the pointer has left the dead zone and the Q offset is being dragged
		float anchor = 0.0f;
		size_t control = 0;
	} drag_ {};
};

} // namespace feathers::ui
