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

#include <cstddef>
#include <cstdint>

// ONE CONSUMER OF A RACK'S STRIKE MASK, FOR EVERY SURFACE THAT SHOWS IT.
//
// The engine's strike mask clears on a look (`Rack::takeStrikeMask`), so two surfaces of one rack (its card and its window, both open) that
// each took it would split the strikes between them: a flash could land on only one. The surfaces' pulses (`RackStrikePulse`) subscribe
// here under the RACK'S IDENTITY — the serial it was built with (`Rack::serial`), never the object's address, which a slot may reuse, and never a host's, since the card and the window may hold different ones — and
// whichever of them looks first in a tick (the hub's round) takes the mask once and fans it out to the rest. Message thread only.

namespace feathers::ui {

class RackStrikePulse;

struct RackStrikeKey {
	/// The rack's own serial (`Rack::serial`), stamped where it is built: 0 is no rack.
	uint64_t serial = 0;
	[[nodiscard]] bool valid() const noexcept { return serial != 0; }
	friend bool operator==(const RackStrikeKey&, const RackStrikeKey&) = default;
};

class RackStrikeHub {
public:
	/// A ROUND IS THIS LONG, in seconds: the surfaces of one editor tick poll within microseconds of each other, and the next tick is a frame later.
	static constexpr double kRoundSeconds = 0.008;

	/// Subscribe `pulse` to the hub of `key` (made on the first), and take it off again. A hub with no subscriber is gone.
	static RackStrikeHub* attach(const RackStrikeKey& key, RackStrikePulse* pulse);
	static void detach(const RackStrikeKey& key, RackStrikePulse* pulse);
	/// TEST SEAM: how many hubs stand, and how many pulses one has.
	[[nodiscard]] static size_t hubCountForTest() noexcept;
	[[nodiscard]] static size_t subscribersForTest(const RackStrikeKey& key) noexcept;

	/// Whether a look by `who` at `now` opens a new round (and, if so, opens it): another surface's look within the round is the same tick's;
	/// the surface that took the last one looking again is the next tick's, however soon.
	[[nodiscard]] bool openRound(double now, const RackStrikePulse* who) noexcept {
		if (who != lastTaker_ && now - lastTake_ < kRoundSeconds && now >= lastTake_) {
			return false;
		}
		lastTake_ = now;
		lastTaker_ = who;
		return true;
	}
	[[nodiscard]] RackStrikePulse* const* begin() const noexcept;
	[[nodiscard]] RackStrikePulse* const* end() const noexcept;

private:
	friend struct RackStrikeRegistry;
	RackStrikeKey key_ {};
	double lastTake_ = -1.0e9;
	const RackStrikePulse* lastTaker_ = nullptr;
	static constexpr size_t kMaxSubscribers = 8;
	RackStrikePulse* subs_[kMaxSubscribers] {};
	size_t count_ = 0;
};

} // namespace feathers::ui
