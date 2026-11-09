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

#include "fx/conv/ir_ref.h"
#include "mod/mseg/mseg_geometry.h"

#include <visage/ui.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace feathers::fx::featherverb {
class Featherverb;
}

namespace feathers::ui {

//--------------------------------------------------------------------------------------------------//
// THE IMPULSE, DRAWN AND SHAPED — the heart of Featherverb's conv drawer (backlog JD).                //
//--------------------------------------------------------------------------------------------------//
//
// Markers in the impulse view choose the begin and end points at which to truncate, and an
// MSEG-style envelope draws over the active part of the impulse, reusing parts of the MSEG code.
//
// - **THE WAVEFORM** is the WHOLE source (`Featherverb::irWhole`), its peaks per pixel, cached against the
//   device's `sourceSerial` (never the vector's address). What the markers leave out is dimmed.
// - **THE MARKERS**: begin and end, full-height lines with a grip at the top, dragged along x.
// - **THE ENVELOPE** is an MSEG shape (`mod::mseg::Settings`) drawn over the span BETWEEN the markers,
//   and edited by the MSEG's own geometry — `handleAt`, `withHandleMoved`, `withWaypointAdded`/`Removed`,
//   `curveTargetAt`/`withCurvature` — the same gestures as the MSEG card: drag a breakpoint, double-click
//   to add or remove one, Opt-drag to bend.
// - **A GESTURE PREVIEWS AND THE RELEASE COMMITS.** Each change rebuilds the IR's spectra off the audio
//   thread (tens of milliseconds for a long one), so a drag draws its shape live and hands it to the device
//   once, on mouse-up — not once per pixel.
// - **DRAG A FILE IN**: WAV, AIFF or FLAC, onto the view (`dropFiles`).
//
// It reaches the device through `Surface`, asked afresh on every use: the instance behind a panel is
// replaced whole by a patch load, so a pointer kept from an earlier call could be one that is gone.
class IrView final : public visage::Frame {
public:
	using Surface = std::function<fx::featherverb::Featherverb*()>;
	/// Called after an edit the device took — the body moved, so the host is told (backlog JV).
	using Edited = std::function<void()>;
	explicit IrView(Surface surface, Edited edited = {});

	void draw(visage::Canvas& canvas) override;

	bool receivesDragDropFiles() override { return true; }
	std::string dragDropFileExtensionRegex() override { return R"(.*\.([wW][aA][vV]|[aA][iI][fF][fF]?|[fF][lL][aA][cC]))"; }
	void dropFiles(const std::vector<std::string>& paths) override;

	/// WHETHER THE SCREEN IS BEHIND THE DEVICE — a new source, a new shape (a patch load, an undo elsewhere).
	/// Asked by the panel's per-tick `updateContent`; redraws and says so.
	bool refresh();

	/// What the last drop said, empty on success: shown in the drawer's strip.
	[[nodiscard]] const std::string& lastError() const noexcept { return error_; }
	/// Forget it (a file loaded by another road: the browser).
	void clearError() { error_.clear(); }
	/// Say something in its place (the folder browser: a folder where nothing loads).
	void setError(std::string error) {
		error_ = std::move(error);
		redraw();
	}

	//---------------------------------------------------------------------------------------------//
	// TEST SEAMS — the geometry is arithmetic, so a case can drive it without a window.             //
	//---------------------------------------------------------------------------------------------//
	/// The span's box, in this frame's coordinates, that the envelope is drawn in.
	[[nodiscard]] visage::Bounds spanBoundsForTest() const { return spanBounds(); }
	/// The shape a gesture in progress would commit.
	[[nodiscard]] const fx::conv::IrShape& previewForTest() const noexcept { return preview_; }
	/// The peaks as last cached: one (min, max) per column, and the serial they were made from.
	[[nodiscard]] uint64_t cachedSerialForTest() const noexcept { return cachedSerial_; }

private:
	enum class Drag { None, Begin, End, Handle, Curve };

	[[nodiscard]] visage::Bounds waveBounds() const;
	[[nodiscard]] visage::Bounds spanBounds() const;
	[[nodiscard]] float xOfFraction(float f) const;
	[[nodiscard]] float fractionOfX(float x) const;
	/// A point in the span's unit box: x across the span, y 0 at the bottom, 1 at the top.
	[[nodiscard]] std::pair<float, float> unitOf(visage::Point p) const;
	[[nodiscard]] visage::Point pointOf(float ux, float uy) const;
	[[nodiscard]] mod::mseg::Settings envelopeOf(const fx::conv::IrShape& shape) const;
	[[nodiscard]] fx::conv::IrShape withEnvelope(fx::conv::IrShape shape, const mod::mseg::Settings& env) const;
	void cachePeaks(const fx::featherverb::Featherverb& verb);
	void commit();

	Surface surface_;
	Edited edited_;
	fx::conv::IrShape preview_;
	fx::conv::IrShape before_;
	bool previewing_ = false;
	Drag drag_ = Drag::None;
	int handle_ = -1;
	mod::mseg::CurveTarget curve_;
	float curveAnchorY_ = 0.0f;

	std::vector<std::pair<float, float>> peaks_;
	uint64_t cachedSerial_ = 0;
	float cachedWidth_ = 0.0f;
	fx::conv::IrShape shownShape_;
	std::string error_;
};

} // namespace feathers::ui
