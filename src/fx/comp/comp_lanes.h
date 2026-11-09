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

#include <cstdint>

// COMP'S ENCODE/DECODE LANE, AS AN EDITOR AND A TEST REACH IT (backlog QG) — the Delay's `delay_lanes.h`, for the same
// component (`rack::LazyLane`). One `rack::InnerChain` between the Compander's halves; LAZY, so a COMP that never opens
// it pays nothing. It runs ONLY in Compander mode: in Compress and Limit its devices are kept, and saved, but not run
// (their latency still pads the output, so a Mode flip never moves the device's).
//
// FORWARD ONLY: `rack::InnerChain` lives above the fx library, and the holder (`comp_ops.cpp`) is built in
// `feathers-fx-sklitter`, the target that sees both.

namespace feathers::rack {
class InnerChain;
}

namespace feathers::fx::comp {

struct CompDsp;

/// THE LANE'S STABLE ID IN THE SAVED BODY (`rack::BundleLane`) — never renumber.
inline constexpr uint16_t kCompanderLaneId = 0;

/// **THE LANE INSIDE THE INSTANCE A HOST FILED** (`DeviceHost::containerSurfaceOf`), or null — null too while it has never
/// been built. A contemporaneous handle, never an identity (trap T9): an editor re-asks every tick.
[[nodiscard]] rack::InnerChain* companderLaneOf(void* surface) noexcept;
[[nodiscard]] const rack::InnerChain* companderLaneOf(const void* surface) noexcept;
/// **BUILD THE LANE IF IT IS NOT BUILT, AND RETURN IT** — MESSAGE THREAD ONLY. Null for a null surface.
rack::InnerChain* ensureCompanderLane(void* surface);
/// TEST ONLY — the DSP inside an instance.
[[nodiscard]] const CompDsp& dspOf(const void* storage);

} // namespace feathers::fx::comp
