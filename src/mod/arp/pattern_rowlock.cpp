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

#include "mod/arp/pattern_rowlock.h"

#include <algorithm>

namespace feathers::mod::arp::pattern {

namespace {

constexpr size_t kMaxTones = theory::kMaxTones;

/// The key tone `i` of step `k` plays — tone 0 as the cycle spells it (the polymeter included), the rest as stored.
int toneKey(const PatternCycle& c, size_t k, int i) noexcept {
	return i == 0 ? c.keyAt(k, 0) : c.steps[k].keys[static_cast<size_t>(i)];
}

/// The tone of step `k` that sounds the pitch `id` above the root, or -1.
int toneAt(const PatternCycle& c, size_t k, int id) noexcept {
	const PatternStep& st = c.steps[k];
	if (st.kind != PatternStep::Kind::Note) {
		return -1;
	}
	for (int i = 0; i < st.count; ++i) {
		if (toneKey(c, k, i) - c.rootKey == id) {
			return i;
		}
	}
	return -1;
}

void removeTone(PatternStep& st, int i) noexcept {
	for (int j = i; j + 1 < st.count; ++j) {
		st.keys[static_cast<size_t>(j)] = st.keys[static_cast<size_t>(j) + 1];
	}
	--st.count;
	const uint32_t low = (1u << i) - 1u;
	st.holdMask = static_cast<uint16_t>((st.holdMask & low) | ((static_cast<uint32_t>(st.holdMask) >> (i + 1)) << i));
}

void toRest(PatternStep& st) noexcept {
	st.kind = PatternStep::Kind::Rest;
	st.count = 0;
	st.holdMask = 0;
	st.accent = false;
	st.slide = false;
	st.grace = false;
	st.ratchet = 1;
}

/// Puts `key` among the step's tones, ascending (the highest goes when the step is full); `held` says it sustains from the
/// chord before. A tone already there is left as it is.
void insertTone(PatternStep& st, int key, bool held) noexcept {
	for (int i = 0; i < st.count; ++i) {
		if (st.keys[static_cast<size_t>(i)] == key) {
			return;
		}
	}
	if (st.count >= static_cast<int>(kMaxTones)) {
		--st.count;
	}
	int pos = 0;
	while (pos < st.count && st.keys[static_cast<size_t>(pos)] < key) {
		++pos;
	}
	for (int j = st.count; j > pos; --j) {
		st.keys[static_cast<size_t>(j)] = st.keys[static_cast<size_t>(j) - 1];
	}
	st.keys[static_cast<size_t>(pos)] = static_cast<int16_t>(key);
	++st.count;
	const uint32_t low = (1u << pos) - 1u;
	st.holdMask = static_cast<uint16_t>((st.holdMask & low) | ((static_cast<uint32_t>(st.holdMask) & ~low) << 1)
	                                    | (held ? (1u << pos) : 0u));
}

} // namespace

LockedRow captureRow(const PatternCycle& cycle, genes::Mode mode, int id) noexcept {
	LockedRow row;
	row.used = true;
	row.id = static_cast<int16_t>(id);
	row.length = static_cast<uint16_t>(std::min(cycle.length, genes::kMaxSteps));
	const size_t n = row.length;
	if (genes::decodesDrum(mode)) {
		const auto l = static_cast<size_t>(std::clamp(id, 0, static_cast<int>(kDrumLanes) - 1));
		const auto bit = static_cast<uint8_t>(1u << l);
		for (size_t k = 0; k < n; ++k) {
			const PatternStep& st = cycle.steps[k];
			LockedCell& c = row.cell[k];
			c.on = (st.drumHits & bit) != 0u;
			if (!c.on) {
				continue;
			}
			c.accent = (st.drumAccent & bit) != 0u;
			c.ghost = (st.drumGhost & bit) != 0u;
			c.roll = (st.drumRoll & bit) != 0u;
			c.ratchet = st.ratchet;
			c.velocity = st.drumVelocity[l];
			c.length = st.drumLength[l];
		}
		return row;
	}
	bool chain = false;
	for (size_t k = 0; k < n; ++k) {
		const PatternStep& st = cycle.steps[k];
		LockedCell& c = row.cell[k];
		if (st.kind == PatternStep::Kind::Note) {
			chain = toneAt(cycle, k, id) >= 0;
			if (chain) {
				c.on = true;
				c.accent = st.accent;
				c.slide = st.slide;
				c.grace = st.grace;
				c.held = toneAt(cycle, k, id) >= 0 && ((st.holdMask >> toneAt(cycle, k, id)) & 1u) != 0u;
				c.ratchet = st.ratchet;
				c.graceKey = static_cast<int16_t>(st.graceKey - cycle.rootKey);
				c.velocity = st.velocity;
				c.length = st.gate;
			}
		}
		else if (st.kind == PatternStep::Kind::Tie) {
			c.tie = chain;
		}
		else {
			chain = false;
		}
	}
	return row;
}

bool lockRow(RowLocks& locks, const PatternCycle& cycle, genes::Mode mode, int id) noexcept {
	if (locks.mode != mode) {
		locks = RowLocks {};
		locks.mode = mode;
	}
	if (locks.find(id) < kMaxLockedRows) {
		return true;
	}
	for (LockedRow& r : locks.row) {
		if (!r.used) {
			r = captureRow(cycle, mode, id);
			return true;
		}
	}
	return false;
}

bool unlockRow(RowLocks& locks, int id) noexcept {
	const size_t i = locks.find(id);
	if (i >= kMaxLockedRows) {
		return false;
	}
	locks.row[i] = LockedRow {};
	return true;
}

namespace {

void applyLane(const LockedRow& row, PatternCycle& cycle) noexcept {
	const auto l = static_cast<size_t>(std::clamp<int>(row.id, 0, static_cast<int>(kDrumLanes) - 1));
	const auto bit = static_cast<uint8_t>(1u << l);
	const size_t n = std::min<size_t>(row.length, cycle.length);
	for (size_t k = 0; k < n; ++k) {
		PatternStep& st = cycle.steps[k];
		const LockedCell& c = row.cell[k];
		const auto put = [bit](uint8_t& mask, bool v) { mask = static_cast<uint8_t>(v ? (mask | bit) : (mask & ~bit)); };
		put(st.drumHits, c.on);
		put(st.drumAccent, c.on && c.accent);
		put(st.drumGhost, c.on && c.ghost);
		put(st.drumRoll, c.on && c.roll);
		st.drumVelocity[l] = c.on ? c.velocity : 0.0f;
		st.drumLength[l] = c.on ? c.length : 0.0f;
		if (c.on && c.roll) {
			st.ratchet = std::max(st.ratchet, c.ratchet);
		}
		st.accent = st.drumAccent != 0;
		st.kind = st.drumHits != 0 ? PatternStep::Kind::Note : PatternStep::Kind::Rest;
	}
}

void applyPitch(const LockedRow& row, PatternCycle& cycle, bool chordal) noexcept {
	const size_t n = std::min<size_t>(row.length, cycle.length);
	const int id = row.id;
	// WHAT THE TERRAIN HOLDS ON THE ROW, read before anything moves: the tone on the pitch, and the ties that continue it.
	std::array<int8_t, genes::kMaxSteps> owner {};
	std::array<bool, genes::kMaxSteps> tie {};
	bool chain = false;
	for (size_t k = 0; k < n; ++k) {
		const PatternStep& st = cycle.steps[k];
		owner[k] = -1;
		tie[k] = false;
		if (st.kind == PatternStep::Kind::Note) {
			const int i = toneAt(cycle, k, id);
			owner[k] = static_cast<int8_t>(i);
			chain = i >= 0;
		}
		else if (st.kind == PatternStep::Kind::Tie) {
			tie[k] = chain;
		}
		else {
			chain = false;
		}
	}
	// TAKE THE TERRAIN'S NOTES OFF THE ROW (and the ties of a note left with nothing).
	bool dead = false;
	for (size_t k = 0; k < n; ++k) {
		PatternStep& st = cycle.steps[k];
		if (owner[k] >= 0) {
			removeTone(st, owner[k]);
			dead = st.count == 0;
			if (dead) {
				toRest(st);
			}
		}
		else if (tie[k]) {
			if (dead) {
				toRest(st);
			}
		}
		else {
			dead = false;
		}
	}
	// LAY THE CAPTURED ROW ON.
	const int rowKey = std::clamp(cycle.rootKey + id, 0, 127);
	for (size_t k = 0; k < n; ++k) {
		PatternStep& st = cycle.steps[k];
		const LockedCell& c = row.cell[k];
		if (c.on) {
			if (st.kind != PatternStep::Kind::Note) {
				st.kind = PatternStep::Kind::Note;
				st.count = 0;
				st.holdMask = 0;
			}
			else if (!chordal) {
				st.count = 0; // ONE NOTE A STEP: the locked note takes the step, whatever the terrain put there now
				st.holdMask = 0;
			}
			insertTone(st, rowKey, c.held);
			st.velocity = c.velocity;
			st.gate = c.length;
			st.accent = c.accent;
			st.slide = c.slide;
			st.grace = c.grace;
			st.ratchet = std::max<uint8_t>(1, c.ratchet);
			st.graceKey = static_cast<int16_t>(std::clamp(cycle.rootKey + c.graceKey, 0, 127));
			st.noteIndex = -1; // its own pitch: out of the polymeter's walk
		}
		else if (c.tie && st.kind != PatternStep::Kind::Note) {
			st.kind = PatternStep::Kind::Tie;
			st.count = 0;
			st.holdMask = 0;
		}
	}
}

} // namespace

void applyRowLocks(const RowLocks& locks, genes::Mode mode, PatternCycle& cycle) noexcept {
	if (locks.mode != mode) {
		return;
	}
	const bool drum = genes::decodesDrum(mode);
	for (const LockedRow& row : locks.row) {
		if (!row.used) {
			continue;
		}
		if (drum) {
			applyLane(row, cycle);
		}
		else {
			applyPitch(row, cycle, mode == genes::Mode::Chords || mode == genes::Mode::Progression);
		}
	}
}

} // namespace feathers::mod::arp::pattern
