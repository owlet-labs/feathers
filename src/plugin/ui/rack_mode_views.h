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
#include "plugin/ui/device_control.h"
#include "plugin/ui/tooltip.h"
#include "rack/parallel_rack.h"

#include <visage/ui.h>

#include <array>
#include <vector>

// THE RACK'S MODE VIEWS (rack stage 2, step 4): each mode composed
// around something to look at, with the controls ON it (the no-knob-list rule).
//
// - `RackSpectrumStrip`: §7a's thin strip ABOVE the chains in Bands, Mid/Side and Left/Right — the rack's input (PRE, a
//   line) and output (POST, filled) from the EQ's lifted analyser, on a log-frequency axis. In Bands each band's span
//   is tinted and each corner is a vertical line that is its drag handle. It asks the rack for readings while it is
//   shown (`Rack::wantReadings`), so the analyser costs nothing with no view open.
// - `RackPositionBar`: Select and Blend — one segment per chain and a cursor at the position; in Blend each segment is
//   filled to its weight. Dragging it writes `rack_position`.
//
// BOTH WRITE THROUGH `DeviceControl`s THE OWNER HOLDS, one gesture per drag (one undo step, the host's gesture with it),
// and declare those controls as offered on their face, so the window ⊇ card guard sees them.

namespace feathers::ui {

class RackSpectrumStrip final : public visage::Frame, public TooltipSource {
public:
	/// `cuts` are the rack's Cut 1..4 controls and `mode`/`bands` its Mode and Bands, all owned by the caller.
	RackSpectrumStrip(backplane::DeviceHost& host, DeviceInstanceId rack, std::array<DeviceControl*, 4> cuts,
	                  DeviceControl& mode, DeviceControl& bands);

	void draw(visage::Canvas& canvas) override;
	const char* tooltipAt(visage::Point) const override;
	void offeredControls(std::vector<Offered>& out) const override;

	/// RE-READ THE READINGS AND ASK FOR MORE (the lease), while shown; true when anything drawn changed.
	bool refresh();

	/// THE FREQUENCY → x MAP, 20 Hz at the left edge and 20 kHz at the right, even in octaves.
	[[nodiscard]] float xOfHz(double hz) const noexcept;
	[[nodiscard]] double hzOfX(float x) const noexcept;
	/// How many corners are drawn now (bands − 1 in Bands, 0 elsewhere), and where corner k's line stands.
	[[nodiscard]] int cornerCount() const;
	[[nodiscard]] float cornerXForTest(int k) const;
	/// A HAND DRAGS corner k to `x` and lets go — the strip's own road (one gesture).
	void dragCornerForTest(int k, float x);
	[[nodiscard]] float postDbForTest(size_t bin) const noexcept { return post_[bin]; }

	static constexpr float kCardHeight = 30.0f;
	static constexpr float kWindowHeight = 44.0f;
	/// How close to a corner's line a press takes it, in pixels.
	static constexpr float kGrab = 5.0f;
	/// The level range drawn, in dB (top and bottom of the strip).
	static constexpr float kTopDb = 0.0f;
	static constexpr float kBottomDb = -90.0f;

private:
	[[nodiscard]] bool inBands() const;
	/// Corner k in Hz as the ENGINE runs it (`trackedCorners`), from the cuts the record holds and the note it tracks.
	[[nodiscard]] double cornerHz(int k) const;
	/// The note Key Track places the corners on (the rack's `NoteHz` reading), 0 when it is not tracking.
	[[nodiscard]] double noteHz() const;
	void beginCorner(int k);
	void moveCorner(int k, float x);
	void endCorner();

	backplane::DeviceHost& host_;
	DeviceInstanceId rack_;
	std::array<DeviceControl*, 4> cuts_;
	DeviceControl& mode_;
	DeviceControl& bands_;
	std::array<float, dsp::kAnalyserBins> pre_ {};
	std::array<float, dsp::kAnalyserBins> post_ {};
	float frame_ = -1.0f;
	int dragging_ = -1;
	std::vector<int> touched_;
};

class RackPositionBar final : public visage::Frame, public TooltipSource {
public:
	RackPositionBar(backplane::DeviceHost& host, DeviceInstanceId rack, DeviceControl& position, DeviceControl& mode);

	void draw(visage::Canvas& canvas) override;
	const char* tooltipAt(visage::Point) const override { return tooltipOf(&position_); }
	void offeredControls(std::vector<Offered>& out) const override { out.push_back({ &position_, Road::Face }); }

	/// The chain count it last drew, re-read; true when anything drawn changed.
	bool refresh();
	/// Where the cursor stands, and segment k's fill (its weight in the mode, 0..1) — what is drawn, as values.
	[[nodiscard]] float cursorXForTest() const;
	[[nodiscard]] float fillForTest(size_t k) const;
	/// A HAND PRESSES at `x` and drags to `toX` (one gesture).
	void dragForTest(float x, float toX);

	static constexpr float kHeight = 18.0f;

private:
	void setFromX(float x);
	backplane::DeviceHost& host_;
	DeviceInstanceId rack_;
	DeviceControl& position_;
	DeviceControl& mode_;
	size_t chains_ = 0;
	double drawnPosition_ = -1.0;
	double drawnMode_ = -1.0;
	bool dragging_ = false;
};

} // namespace feathers::ui
