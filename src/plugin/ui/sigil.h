/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */
#pragma once

#include "device/modulatable.h"

#include <visage/ui.h>

#include <array>
#include <cstdint>

namespace feathers::ui {

/// **A SIGIL: WHERE IN THE FIELD A FREE COORDINATE IS, AT A GLANCE** (backlog IW: a display that
/// decodes coarse phase into a small random glyph — a generated sigil, a little bigger than one character).
///
/// A FREE-ONLY control — one whose zones were labels over a continuum rather than modes — lost its dropdown under
/// IW, and this takes the room. It reads the coordinate's COARSE phase, the whole lap it is in, and draws a small
/// mark for it: a few strokes between the dots of a 3x3 grid, chosen by hashing the lap. So each lap has its own
/// stable face, and a hand that has been somewhere before recognises it; the fine position is the number beside it.
///
/// **A MARK, NOT A NAME.** It names nothing and promises no sound — which is the whole point of IW: a label over a
/// continuum read as a claim about the sound, and this makes no claim beyond "this lap, not that one".
class Sigil final : public visage::Frame {
public:
	/// @param perLap  how much coordinate one lap is — the ZoneSelect's `kCoordinatePerLap` for every zoned control
	Sigil(device::Modulatable& control, double perLap);

	void draw(visage::Canvas& canvas) override;
	void setEnabled(bool on);

	/// THE 3x3 GRID'S TWENTY EDGES: six across, six down, eight diagonal. `strokesFor` answers a subset.
	static constexpr int kEdges = 20;
	/// "A BIT BIGGER THAN ONE CHARACTER": the readout row's height, against a 9 px readout face.
	static constexpr float kSide = 13.0f;

	/// WHICH EDGES A LAP DRAWS, as a bitmask over `kEdges` — four to six of them, a pure function of the lap, so a
	/// lap's sigil is the same on every surface and every run.
	[[nodiscard]] static uint32_t strokesFor(int64_t lap);
	/// The two grid points (0..8, row-major) edge `e` joins.
	[[nodiscard]] static std::array<int, 2> endpointsOf(int e);

	/// The lap the control is in now: `floor(value / perLap)`.
	[[nodiscard]] int64_t lap() const;

private:
	device::Modulatable& control_;
	double perLap_;
	bool enabled_ = true;
};

} // namespace feathers::ui
