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

#include "device/device.h"
#include "device/transfer.h"

// **ONE ROUTE'S ARITHMETIC, NAMED, SO THE ENGINE AND THE EDITOR'S AS-MODULATED READOUT CALL THE SAME TWO
// FUNCTIONS** (backlog PU). The rack engine's resolve (`RackEngine::resolveEar`,
// `resolveWindowInto`) sums `routeTerm` over a control's inputs and lands the sum with `resolveRouted`;
// the readout that floats beside a destination while a route's amount is set (`ui/mod_readout.h`) does the
// same with the amount under the hand. A readout that restated `amount × source × span` would be a second
// implementation across a thread boundary, the shape `DeviceHost::modulatedFieldAt` was written to refuse.

namespace feathers::device {

/// **WHAT ONE ROUTE ADDS**, in the destination's own units: `amount × f(source) × span`, in double, in that
/// order — the order the engine has always multiplied in, so a lane moved between hosts resolves to the bit.
/// `span` is the destination's `routeSpanOf`.
[[nodiscard]] inline double routeTerm(float amount, Transfer transfer, float source, double span) noexcept {
	return static_cast<double>(amount) * static_cast<double>(shapeTransfer(transfer, source)) * span;
}

/// **THE VALUE A DEVICE RECEIVES, FROM ITS BASE AND EVERYTHING THAT REACHES IT** — `withRouteOffset` (the
/// stepped rounding and the range, unless `offsetsMayLeaveRange`), but only when something actually reached
/// it: an unrouted control is its base, exactly, which is the engine's rule.
[[nodiscard]] inline double resolveRouted(const ParamDesc& p, double base, double offset) noexcept {
	return offset != 0.0 ? withRouteOffset(p, base, offset) : base;
}

} // namespace feathers::device
