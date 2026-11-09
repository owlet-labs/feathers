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

// THE DELAY'S FEEDBACK FX LANE, AS AN EDITOR AND A TEST REACH IT.
//
// One `rack::InnerChain` inside the loop (`LoopInsert` in delay.h has where it sits and how it runs). **LAZY**
// (decision 9, backlog NS's pattern): a Delay holds no chain until something asks for one — an editor opening the rail,
// or a body that has devices in its lane — so a Delay that never uses its lane pays nothing for it.
//
// FORWARD ONLY, for SKLITTER's and Resonate's reason: `rack::InnerChain` lives above the fx library, and the holder's
// ops (`delay_ops.cpp`) are built in `feathers-fx-sklitter`, the target that sees both.

namespace feathers::rack {
class InnerChain;
}

namespace feathers::fx::delay {

class Delay;

/// THE LANE'S STABLE ID IN THE SAVED BODY (`rack::BundleLane`) — never renumber.
inline constexpr uint16_t kFeedbackLaneId = 0;

/// **THE LANE INSIDE THE INSTANCE A HOST FILED** (`DeviceHost::containerSurfaceOf`), or null — null too while it has
/// never been built. A contemporaneous handle, never an identity: the converger replaces a container's instance when
/// a body arrives (trap T9), so an editor re-asks every tick.
[[nodiscard]] rack::InnerChain* feedbackLaneOf(void* surface) noexcept;
[[nodiscard]] const rack::InnerChain* feedbackLaneOf(const void* surface) noexcept;

/// **BUILD THE LANE IF IT IS NOT BUILT, AND RETURN IT** — MESSAGE THREAD ONLY. Adopted with the catalogue the host
/// handed, given the gate and slots it handed, prepared at the rate it last prepared at, then published to the audio
/// thread, which picks it up at its next block. Idempotent. Null for a null surface.
rack::InnerChain* ensureFeedbackLane(void* surface);

/// TEST ONLY — the DSP inside an instance (a host's container surface, or storage a test built).
[[nodiscard]] const Delay& dspOf(const void* storage);

} // namespace feathers::fx::delay
