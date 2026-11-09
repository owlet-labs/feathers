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

#include "mod/follower/follower_core.h"
#include "mod/follower/follower_types.h"

#include <cstddef>
#include <cstdint>

// THE FOLLOWER'S STATE — one per instance: the bus one, or one per voice under Poly in Audio. Its audio road listens to
// the signal passing (`hear`, Audio only) and its mod road follows its Input (Control only) and publishes (`tick`).
// See `follower_types.h`.

namespace feathers::mod::follower {

class Follower {
public:
	void prepare(double sampleRate) noexcept;
	void reset() noexcept;
	/// THE RESOLVED ROW, decoded and handed to the core.
	void setFields(const float* fields, size_t count) noexcept;
	/// THE AUDIO PASSING, followed per sample in Audio; nothing in Control. `right` null is the mono road: one ear
	/// stands for both.
	void hear(const float* left, const float* right, int frames) noexcept;
	/// ONE MODULATION TICK of `samples`: in Control the core is fed the Input for those samples; then a held fire
	/// becomes the tick's pulse.
	void tick(size_t samples) noexcept;

	[[nodiscard]] float outputAt(size_t n) const noexcept;
	/// THE LEVEL THE CARD DRAWS, 0..1 (`ModOps::playheadLevel`, whose telemetry carries no negative): the Env, or
	/// under Signed the Env mapped −1..1 → 0..1, so 0.5 is zero.
	[[nodiscard]] float drawnLevel() const noexcept;
	/// HOW MANY TIMES THE TRIG OUTPUT HAS PULSED — a count that only grows, so a card polling at its own rate sees
	/// every pulse (`ModOps::trigFires`; for this device it counts the OUTPUT, since it has no Trig input).
	[[nodiscard]] uint32_t fires() const noexcept { return fires_; }

	/// THE ROW AS DECODED — stored units, clamped to the declared ranges (`settingsOf`).
	[[nodiscard]] const Settings& settings() const noexcept { return settings_; }
	[[nodiscard]] const Core& core() const noexcept { return core_; }

private:
	Core core_;
	Settings settings_ {};
	uint32_t fires_ = 0;
};

} // namespace feathers::mod::follower
