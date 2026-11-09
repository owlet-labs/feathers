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

#include "mod/rtrig/rtrig_types.h"

#include <cstdint>

// ONE STREAM OF RANDOM EVENTS AS THREE MODULATION OUTPUTS — what a note runs under Poly, and what the device runs under
// global. The device (`rtrig.cpp`) owns the choice of which streams exist and when they restart; this is one of them.

namespace feathers::mod::rtrig {

/// WHAT ONE TICK OF A STREAM IS ASKED WITH — resolved once per device tick, shared by every stream it ticks.
struct TickSettings {
	dsp::RandomEventSettings events {};
	/// THE DECAY OUTPUT'S FACTOR PER TICK (`decayFactor`).
	double decay = 0.0;
	/// STABLE: a re-press under a held key restarts the stream from `stableSeed`.
	bool stable = true;
	uint64_t stableSeed = 0;
};

class Shape {
public:
	void prepare(double sampleRate) noexcept;
	/// THE OUTPUTS TO REST and the count to zero; the stream is left where it is (the device reseeds it).
	void reset() noexcept;
	/// THE STREAM STARTS AGAIN FROM `seed` — a Stable note, or a Stable restart under global.
	void start(uint64_t seed) noexcept;
	/// THE STREAM TURNS ONTO ANOTHER COURSE WITHOUT RESTARTING — a Free note's start (`dsp::RandomEvents::mix`).
	void mix(uint64_t k) noexcept { events_.mix(k); }
	/// **ONE HIT, FIRED BY THE TRIG** (backlog MV), landing in the next tick: sized by the same law and polarity as the
	/// stream's own, drawn from a stream of its own so a fire never moves the generator's hits (NOISE parity holds).
	void fire() noexcept { firePending_ = true; }

	/// **THE STREAM, IN AND OUT OF THE PATCH** (backlog NM, `kStreamWords`). `load` is all-or-nothing.
	void save(device::BlockWriter& w) const noexcept;
	[[nodiscard]] bool load(device::BlockReader& r) noexcept;

	/// ONE TICK: `mod::kBlockSamples` samples of the generator, then the three outputs. `PerNoteTable`'s signature.
	void tick(const TickSettings& s, bool gate, bool retrigger, TransportClock clock, float rateMul) noexcept;

	/// A NOTE'S STREAM NEVER ENDS ON ITS OWN, so a released note is done at once: its slot is the first reclaimed, and
	/// until then it goes on ticking for the voice still sounding it.
	[[nodiscard]] bool idle() const noexcept { return true; }
	/// NOTHING TO CARRY: the outputs are events, not a glide (`PerNoteTable::processNotes` asks only with smoothing).
	void carrySmoothingFrom(const Shape&) noexcept {}

	[[nodiscard]] float output(Output o) const noexcept;
	/// HOW MANY HITS THIS STREAM HAS MADE since its last reset — what the card counts new hits by.
	[[nodiscard]] uint32_t hits() const noexcept { return hits_; }
	/// THE SAMPLE, within the last tick, its newest hit landed on; -1 when no hit landed in it.
	[[nodiscard]] int lastHitOffset() const noexcept { return lastOffset_; }
	/// HOW MANY HITS LANDED IN THE LAST TICK.
	[[nodiscard]] int hitsLastTick() const noexcept { return hitsLastTick_; }
	[[nodiscard]] float size() const noexcept { return size_; }
	[[nodiscard]] float decayValue() const noexcept { return decay_.value(); }
	/// **THE NEWEST HITS, NEWEST FIRST** (`ModOps::recentEvents`): each one's place in `hits` and its size — so a card
	/// that looks once a frame draws every hit a frame held, not only the newest.
	size_t recentEvents(device::RecentEvent* out, size_t capacity) const noexcept;

private:
	void record(float size) noexcept;

	dsp::RandomEvents events_ {};
	bool trig_ = false;
	float size_ = 0.0f;
	HitDecay decay_ {};
	uint32_t hits_ = 0;
	int lastOffset_ = -1;
	int hitsLastTick_ = 0;
	bool firePending_ = false;
	FireStream fire_ {};
	/// THE LAST `kRecentEvents` HITS, at `hits % kRecentEvents` — a display's, not the patch's: never saved.
	std::array<device::RecentEvent, device::kRecentEvents> recent_ {};
};

/// TEST ONLY — the stream a destination with no voice reads (the newest note's under Poly, else the global one), and
/// the stream a note reads (null for a note the device does not hold, or under global).
[[nodiscard]] const Shape& sharedShapeForTest(const void* storage);
[[nodiscard]] const Shape* noteShapeForTest(const void* storage, int32_t noteId, int16_t channel, int16_t key);

} // namespace feathers::mod::rtrig
