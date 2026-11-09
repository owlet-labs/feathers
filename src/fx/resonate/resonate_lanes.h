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

#include <cstdint>

// RESONATE'S TWO FX LANES, AS AN EDITOR REACHES THEM (take the fx).
//
// DRONE FX runs on the sympathetic drone; WET FX on the wet (Level x the tone/filter blend) — and, while
// `Drone to Wet FX` is on, on the drone after DRONE FX too. Both before the dry/wet Mix. resonate.cpp has the
// signal flow, the latency and the body.
//
// FORWARD ONLY, for SKLITTER's reason (sklitter.h): `rack::InnerChain` lives above the fx library, and the
// holder's ops (resonate.cpp) are built in `feathers-fx-sklitter`, the target that sees both.

namespace feathers::rack {
class InnerChain;
}

namespace feathers::fx::resonate {

/// WHICH LANE — also its stable id in the saved body (`rack::BundleLane`), so never renumber.
enum class Lane : uint16_t { Drone = 0, Wet = 1 };

/// **THE LANE INSIDE THE INSTANCE A HOST FILED** (`DeviceHost::containerSurfaceOf`), or null. A contemporaneous
/// handle, never an identity: the converger replaces a container's instance when a body arrives (trap T9), so an
/// editor re-asks every tick, as SKLITTER's rails do.
[[nodiscard]] rack::InnerChain* laneChainOf(void* surface, Lane lane) noexcept;
[[nodiscard]] const rack::InnerChain* laneChainOf(const void* surface, Lane lane) noexcept;

/// TEST ONLY — the routing the instance last decoded (`Param::DroneToWetFx`).
[[nodiscard]] bool droneToWetFxForTest(const void* surface) noexcept;

} // namespace feathers::fx::resonate
