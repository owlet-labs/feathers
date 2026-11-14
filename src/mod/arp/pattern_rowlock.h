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

#include "mod/arp/genes.h"
#include "mod/arp/pattern.h"

#include <array>
#include <cstddef>
#include <cstdint>

// **LOCK ROW**: a row of a pattern's grid cut loose from the terrain. The decode (`pattern::build`) is a pure function of
// the place; a locked row's steps are CAPTURED from it at the moment of locking and laid back over every later decode
// (`applyRowLocks`), after the build and BEFORE the hand's edits and everything the device does at play time (Transpose,
// Wobble, Humanize, swing, the note range): those still act on a locked row, exactly as on any other.
//
// WHAT A ROW IS, per mode:
//   Drum, Modulation   one lane (id 0..7): its hit, accent, ghost and roll on every step, its velocity (Modulation: its
//                      level) and length. The step's ratchet count is the step's, shared by the lanes: a locked rolling
//                      lane brings its own count and the step keeps the larger.
//   every pitched mode ALL THE NOTES ON ONE PITCH (id: semitones from the cycle's root key, so the held key still moves
//                      it): which steps strike that pitch, and the tie that continues it. A step's velocity, length,
//                      accent, slide, ratchet and grace are the step's, so where the pitch sounds the step takes them
//                      from the capture; where it does not, the step keeps the terrain's own. In Chords and Progression
//                      a step holds other tones too: those follow the terrain; in the single-note modes the locked
//                      note takes the whole step. A locked note plays its captured pitch
//                      (TB with Pitch on Notes: it leaves the polymeter's walk).

namespace feathers::mod::arp::pattern {

inline constexpr size_t kMaxLockedRows = 8;

/// ONE STEP OF A LOCKED ROW.
struct LockedCell {
	bool on = false;     ///< the row sounds here (a lane's hit; a note on the pitch)
	bool tie = false;    ///< pitched: a tie continuing the row's note
	bool accent = false;
	bool ghost = false;
	bool roll = false;
	bool slide = false;
	bool grace = false;
	bool held = false;   ///< pitched: the tone is held from the chord before (not struck again)
	uint8_t ratchet = 1;
	int16_t graceKey = 0; ///< semitones from the cycle's root key
	float velocity = 0.0f;
	float length = 0.0f;  ///< a lane's length in steps; a note's gate
	friend bool operator==(const LockedCell&, const LockedCell&) = default;
};

/// ONE LOCKED ROW: its identity, how many steps the cycle had when it was captured (beyond them the row has no content and
/// follows the decode), and its steps.
struct LockedRow {
	bool used = false;
	int16_t id = 0;
	uint16_t length = 0;
	std::array<LockedCell, genes::kMaxSteps> cell {};
	friend bool operator==(const LockedRow&, const LockedRow&) = default;
};

/// A PATTERN'S LOCKED ROWS, and the Mode they were captured under: under any other Mode they are kept but inert.
struct RowLocks {
	genes::Mode mode = genes::Mode::Melody;
	std::array<LockedRow, kMaxLockedRows> row {};
	[[nodiscard]] bool any() const noexcept {
		for (const LockedRow& r : row) {
			if (r.used) {
				return true;
			}
		}
		return false;
	}
	/// The slot holding row `id`, or kMaxLockedRows.
	[[nodiscard]] size_t find(int id) const noexcept {
		for (size_t i = 0; i < kMaxLockedRows; ++i) {
			if (row[i].used && row[i].id == id) {
				return i;
			}
		}
		return kMaxLockedRows;
	}
	friend bool operator==(const RowLocks&, const RowLocks&) = default;
};

/// The row `id` of `cycle` as the decode holds it now. `mode` says what a row is (a lane, or a pitch).
[[nodiscard]] LockedRow captureRow(const PatternCycle& cycle, genes::Mode mode, int id) noexcept;
/// The row id of a pitch: its key above the cycle's root.
[[nodiscard]] inline int rowIdOfKey(const PatternCycle& cycle, int key) noexcept { return key - cycle.rootKey; }
/// Locks row `id` of `locks` at the cycle's decode (a lock made under another Mode replaces the stored set). False when
/// the set is full.
bool lockRow(RowLocks& locks, const PatternCycle& cycle, genes::Mode mode, int id) noexcept;
/// Releases row `id`; the row follows the decode again. False when it was not locked.
bool unlockRow(RowLocks& locks, int id) noexcept;
/// **THE OVERLAY**: every locked row laid over `cycle`, if the locks were made under `mode`. Allocation-free.
void applyRowLocks(const RowLocks& locks, genes::Mode mode, PatternCycle& cycle) noexcept;

} // namespace feathers::mod::arp::pattern
