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

#include "device/device.h"
#include "mod/arp/arp_types.h"

#include <array>
#include <cstddef>
#include <cstdint>

// THE ARP'S REUSABLE CORE — the clock and the pattern, with no notes in flight. A later kind of arp keeps `StepGrid` and `HeldChord` and brings its own way of choosing a note. Pure
// functions of their inputs, so a case holds them without a host.

namespace feathers::mod::arp {

/// ONE HELD KEY: what was pressed, how hard, and when relative to the other keys (for Played).
struct HeldNote {
	int16_t key = 60;
	int16_t channel = 0;
	float velocity = 1.0f;
	uint32_t order = 0;
	/// THIS KEY'S ON WENT STRAIGHT TO THE VOICES (the arp was off), so its off must follow it there.
	bool passed = false;

	friend bool operator==(const HeldNote&, const HeldNote&) = default;
};

/// THE HELD KEYS, in press order, at most `kMaxHeld` (a sixteenth key pushes out the oldest). A key is a key whatever
/// channel it came on: a second press of a held key replaces it.
class HeldChord {
public:
	void press(int16_t key, int16_t channel, float velocity, uint32_t order) noexcept;
	/// False when the key was not held.
	bool release(int16_t key) noexcept;
	void clear() noexcept { count_ = 0; }
	[[nodiscard]] bool empty() const noexcept { return count_ == 0; }
	[[nodiscard]] size_t size() const noexcept { return count_; }
	[[nodiscard]] const HeldNote& operator[](size_t i) const noexcept { return notes_[i]; }
	[[nodiscard]] HeldNote& operator[](size_t i) noexcept { return notes_[i]; }
	[[nodiscard]] HeldNote* find(int16_t key) noexcept;

	friend bool operator==(const HeldChord& a, const HeldChord& b) noexcept {
		if (a.count_ != b.count_) {
			return false;
		}
		for (size_t i = 0; i < a.count_; ++i) {
			if (!(a.notes_[i] == b.notes_[i])) {
				return false;
			}
		}
		return true;
	}

private:
	std::array<HeldNote, kMaxHeld> notes_ {};
	size_t count_ = 0;
};

/// ONE ENTRY OF A PATTERN.
struct PatternNote {
	int16_t key = 60;
	int16_t channel = 0;
	float velocity = 1.0f;
};

/// **THE PATTERN THE CHORD MAKES UNDER ONE ORDER**, written into `out` (at most `kMaxPattern`); returns its length.
/// Under Random it is the Up pattern, and `noteAt` draws from it. A key pushed past 127 by its octave is left out.
size_t buildPattern(const HeldChord& chord, Order order, int octaves, OctaveMode mode, PatternNote* out) noexcept;

/// **THE ENTRY PATTERN STEP `step` PLAYS** — `pattern[step mod length]`, or under Random an entry drawn from the
/// dice of (`seed`, `step`), so the same step always draws the same note. `length` must be > 0.
[[nodiscard]] size_t entryAt(Order order, int64_t step, size_t length, int seed) noexcept;

/// The dice Random draws from: a hash of the seed and the step, never a running generator, so nothing is stored.
[[nodiscard]] uint64_t stepDice(int seed, int64_t step) noexcept;

/// **THE GRID, IN STEPS.** Slot g's onset is at `g + nudge + (g odd ? swing / 2 : 0)`. Swing works on the GRID slot,
/// not on the pattern step, so a whole-step Offset never moves the swing off the beat.
struct StepGrid {
	double nudge = 0.0;
	double swing = 0.0;
	/// **A PER-SLOT TIMING OFFSET, in steps, or null for none** — the Pattern device's Humanize (terrain-arp design §12),
	/// a pure function of the slot so a scrub or a bounce lands every onset where it did. `firstAtOrAfter` assumes no
	/// onset sits a whole step from its slot, so a hook keeps `swing / 2 + |shift|` under one (Humanize: ≤ 0.45).
	double (*shift)(const void* ctx, int64_t g) = nullptr;
	const void* shiftCtx = nullptr;

	[[nodiscard]] double onset(int64_t g) const noexcept;
	/// The first slot whose onset is at or after `u` (within `tolerance` steps: an onset that close counts as now).
	[[nodiscard]] int64_t firstAtOrAfter(double u, double tolerance) const noexcept;
};

/// **HOW FAST THE STEPS GO**, as the step's length in beats (synced) or its frequency in Hz (Hz), from Time Base and
/// Rate: the step seq's `stepHz`, split so a synced step can be measured against the song's beats.
[[nodiscard]] double stepBeats(const Settings& s) noexcept;
[[nodiscard]] double stepHz(const Settings& s) noexcept;
/// THE SHORTEST STEP the arp plays, in seconds — 1 ms, so a Rate dragged to its bottom cannot ask for several steps
/// inside one sample.
inline constexpr double kMinStepSeconds = 0.001;

} // namespace feathers::mod::arp
