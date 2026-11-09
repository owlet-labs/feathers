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

#include "mod/arp/pattern_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

// **CYCLE'S WALK** — which pattern plays at a position, DERIVED from the
// position: the walk starts at the origin (the song's start, or the free clock's) on the first pattern in range, each
// SEGMENT is one pattern's Repeats × Length steps on its own grid, and its Next names the following one. A segment's first
// step is the first of its pattern's grid slots at or after the boundary, so a switch always lands on a step. Nothing in it
// is history: a locate, a loop or a scrub asks `at` and gets what the run from the origin played there.
//
// IT IS A WALK, NOT A CLOSED FORM, because Random makes the sequence aperiodic. It is cached so a locate is cheap: the
// segment playing (a forward step is O(1)), and up to `kMarks` checkpoints spaced `stride` segments apart, the stride
// doubling when they fill — so a locate walks at most one stride from the nearest checkpoint, and the cache is rebuilt only
// when the configuration changes. A segment that can only be followed by itself (Loop, or one pattern in range) never ends,
// so a looping walk costs nothing however far it is played. Audio thread: no allocation, bounded state.

namespace feathers::mod::arp::pat {

/// WHAT THE WALK IS MADE OF, decoded from the row (and the clock's units).
struct CycleConfig {
	/// The patterns in range (`inRange`), in tab order, and how many.
	std::array<uint8_t, kPatterns> order {};
	size_t count = 0;
	/// Each pattern's segment, in its own steps: Repeats × its cycle length.
	std::array<int64_t, kPatterns> span {};
	std::array<NextAction, kPatterns> next {};
	/// One step of each pattern, in the walk's unit (beats, seconds, or 1 on the free clock: slots).
	std::array<double, kPatterns> unit {};
	/// **SPAN** (design §7.8): one slot's length in walk units (whole bars), 0 for Auto; a segment is Repeats such slots.
	std::array<double, kPatterns> spanSlot {};
	std::array<int, kPatterns> repeats {};
	/// **FILLS** (design §7.8): a fill tab takes no segment; it merges over the tail of the segment before it. False for
	/// every tab when every tab in range is a fill (nothing to merge into: they play as patterns).
	std::array<bool, kPatterns> fill {};
	/// Random's dice.
	int seed = 0;
	friend bool operator==(const CycleConfig&, const CycleConfig&) = default;
};

struct CycleSegment {
	static constexpr double kNever = std::numeric_limits<double>::infinity();
	/// 0 for the first from the origin.
	int64_t index = 0;
	/// The pattern playing; `kPatterns` once a Stop has ended the walk (or nothing is in range).
	size_t pattern = kPatterns;
	/// Its first step, as a slot of its own grid (step k of the pattern plays at slot `first + k`, wrapping by length).
	int64_t first = 0;
	/// Where it starts and ends, in the walk's unit (the first starts at −∞; one that never ends, at +∞).
	double start = -kNever;
	double end = kNever;
	/// THE FILL MERGED OVER THIS SEGMENT'S TAIL (`kPatterns`: none), and the walk after it: the pattern the next segment
	/// plays and that segment's index (a fill counts one step of the walk, so Random's dice move past it).
	size_t fill = kPatterns;
	size_t nextPattern = kPatterns;
	int64_t nextIndex = 1;
	/// Where Span's slots are counted from (the boundary it began at; 0 for the first).
	double base = 0.0;
	[[nodiscard]] bool contains(double t) const noexcept;
};

/// **RANDOM NEXT'S OWN STREAM OF SEED** — Seed hashed with this salt, so it shares nothing with Humanize's dice (which
/// read Seed as they always have): re-rolling one never shifts the other.
inline constexpr uint64_t kRandomNextSalt = 0xC1C1E5EEDull;
/// THE PATTERN AFTER `from` at boundary `index` (the index of the segment it starts): `kPatterns` for Stop.
[[nodiscard]] size_t cycleNext(const CycleConfig& c, size_t from, int64_t index) noexcept;
/// The first step a grid of `unit` has at or after `t` (a step within rounding of `t` is at it).
[[nodiscard]] int64_t firstSlotAtOrAfter(double t, double unit) noexcept;
/// The tolerance two walk positions are equal within.
[[nodiscard]] double cycleTolerance(double t) noexcept;

class CycleWalk {
public:
	static constexpr size_t kMarks = 128;

	/// Sets what the walk is made of; the cache is kept only if nothing moved.
	void configure(const CycleConfig& c) noexcept;
	[[nodiscard]] const CycleConfig& config() const noexcept { return c_; }
	[[nodiscard]] bool configured() const noexcept { return configured_; }
	void clear() noexcept { configured_ = false; }
	/// THE SEGMENT PLAYING AT `t`.
	[[nodiscard]] CycleSegment at(double t) noexcept;
	/// The first segment, and the one after `s` — pure functions of the configuration.
	[[nodiscard]] CycleSegment first() const noexcept;
	[[nodiscard]] CycleSegment after(const CycleSegment& s) const noexcept;
	// TEST SEAMS: how many segments the cache has computed since it was configured, and its checkpoint stride.
	[[nodiscard]] uint64_t stepsForTest() const noexcept { return steps_; }
	[[nodiscard]] int64_t strideForTest() const noexcept { return stride_; }

private:
	[[nodiscard]] bool endsNever(size_t pattern) const noexcept;
	/// The fill merged over `s`'s tail and the walk after it (`s.fill`, `s.nextPattern`, `s.nextIndex`), and its end.
	void finish(CycleSegment& s) const noexcept;
	CycleSegment advance(const CycleSegment& s) noexcept;
	void mark(const CycleSegment& s) noexcept;

	CycleConfig c_ {};
	bool configured_ = false;
	std::array<CycleSegment, kMarks> marks_ {};
	size_t markCount_ = 0;
	int64_t stride_ = 1;
	CycleSegment cursor_ {};
	bool haveCursor_ = false;
	uint64_t steps_ = 0;
};

} // namespace feathers::mod::arp::pat
