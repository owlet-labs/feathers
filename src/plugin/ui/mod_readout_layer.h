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

#include "plugin/ui/mod_readout.h"

#include <visage_ui/frame.h>

#include <string>

namespace feathers::ui {

/// **THE BOX** (backlog PU; `mod_readout.h`). An on-top child covering its host — a card or a window, beside the
/// tooltip layer — ignoring the mouse. Each `advance()` it asks `LiveAmount` which route is being set, walks its
/// host's widgets for the face of that route's destination (`ControlFace` → a `RouteReadoutSubject` with the same
/// host and destination, drawn), and puts the readout beside it: ABOVE the face, below when there is no room,
/// clamped inside the host — never over the face. Nothing found, nothing drawn: a route set from the mod panel
/// shows its box in whichever card holds the destination.
///
/// **NO ANIMATION AT ALL**, so reduced motion has nothing to stop: the box appears and goes. What it does honour
/// (`FEATHERS_REDUCED_MOTION`) is TEXT CHURN: a moving source (an LFO) changes the reading every frame, and a
/// readout chasing a modulator re-rasterises its text every frame — the family the editor's atlas corruption was
/// traced to (`ModDisplay::base`). So a change made by the HAND (the amount) is taken at once, and one made only by
/// the source at most every `kSourceRefreshMs` (`kReducedSourceRefreshMs` under reduced motion).
class ModReadoutLayer final : public visage::Frame {
public:
	static constexpr double kSourceRefreshMs = 100.0;
	static constexpr double kReducedSourceRefreshMs = 1000.0;
	static constexpr float kPadX = 5.0f;
	static constexpr float kPadY = 3.0f;
	static constexpr float kGap = 4.0f;
	static constexpr float kMargin = 2.0f;

	ModReadoutLayer();
	~ModReadoutLayer() override;

	/// The host's hooks, once: cover on resize and DPI change (unconditionally — CLAUDE.md's setBounds/DPI rule).
	void watch(visage::Frame& host);
	void cover(const visage::Frame& host);

	/// Called from the host's per-frame update. Returns whether it asked for a repaint.
	bool advance();

	void draw(visage::Canvas& canvas) override;

	[[nodiscard]] bool shown() const noexcept { return shown_; }
	/// What the box says now (the cases read it): the value line, and the range line ("" when it does not sweep).
	[[nodiscard]] const std::string& valueLine() const noexcept { return valueLine_; }
	[[nodiscard]] const std::string& rangeLine() const noexcept { return rangeLine_; }
	/// The readout behind the lines, as last adopted.
	[[nodiscard]] const ModReadout& readout() const noexcept { return readout_; }
	/// The box and the face it sits beside, in the host's coordinates.
	[[nodiscard]] visage::Bounds box() const noexcept { return box_; }
	[[nodiscard]] visage::Bounds anchor() const noexcept { return anchor_; }

private:
	[[nodiscard]] RouteReadoutSubject* findFace(const LiveRoute& live, visage::Bounds& where) const;
	bool hide();
	void measure();
	void place();

	bool shown_ = false;
	ModReadout readout_ {};
	std::string valueLine_;
	std::string rangeLine_;
	float lastAmount_ = 0.0f;
	double adoptedAt_ = -1.0e300;
	LiveRoute lastLive_ {}; ///< the route the box was last about — found again at hide, to say it has gone
	visage::Point size_ {};
	visage::Bounds anchor_ {};
	visage::Bounds box_ {};
};

} // namespace feathers::ui
