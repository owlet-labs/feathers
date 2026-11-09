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

#include "mod/arp/pattern_cycle.h"

#include <algorithm>
#include <cmath>

namespace feathers::mod::arp::pat {

namespace {

/// SPLITMIX64 — Random's dice, a pure function of Seed and the boundary's index (never the wall clock).
uint64_t mix(uint64_t x) noexcept {
	x += 0x9E3779B97F4A7C15ull;
	x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
	x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
	return x ^ (x >> 31);
}

} // namespace

double cycleTolerance(double t) noexcept {
	return 1e-9 * std::max(1.0, std::abs(t));
}

bool CycleSegment::contains(double t) const noexcept {
	const double near = t + cycleTolerance(t);
	return near >= start && near < end;
}

int64_t firstSlotAtOrAfter(double t, double unit) noexcept {
	const double x = t / unit;
	return static_cast<int64_t>(std::ceil(x - cycleTolerance(x)));
}

size_t cycleNext(const CycleConfig& c, size_t from, int64_t index) noexcept {
	if (c.count == 0) {
		return kPatterns;
	}
	size_t pos = c.count;
	for (size_t i = 0; i < c.count; ++i) {
		if (c.order[i] == from) {
			pos = i;
		}
	}
	if (pos == c.count) {
		return c.order[0]; // not in range (a configuration cannot make this, but a walk must not stall on it)
	}
	switch (c.next[from]) {
	case NextAction::Next: return c.order[(pos + 1) % c.count];
	case NextAction::Previous: return c.order[(pos + c.count - 1) % c.count];
	case NextAction::Loop: return from;
	case NextAction::Stop: return kPatterns;
	case NextAction::Random: {
		if (c.count < 2) {
			return from;
		}
		// ONE OF THE OTHERS, evenly: the dice name a place among the count − 1 that are not `from`.
		const uint64_t roll = mix(mix(static_cast<uint64_t>(static_cast<int64_t>(c.seed)) ^ kRandomNextSalt) ^ static_cast<uint64_t>(index));
		size_t k = static_cast<size_t>(roll % static_cast<uint64_t>(c.count - 1));
		if (k >= pos) {
			++k;
		}
		return c.order[k];
	}
	default: return from;
	}
}

/// THE MOST FILLS A RANDOM WALK PASSES IN A ROW before it calls the run a dead end — (1/3)^64 for the likeliest case.
constexpr size_t kFillPassLimit = 64;

bool CycleWalk::endsNever(size_t pattern) const noexcept {
	// A SEGMENT THAT CAN ONLY BE FOLLOWED BY ITSELF never ends: its steps run on unbroken, so one segment is all of it.
	const NextAction a = c_.next[pattern];
	return a == NextAction::Loop || (c_.count == 1 && a != NextAction::Stop);
}

void CycleWalk::finish(CycleSegment& s) const noexcept {
	const size_t p = s.pattern;
	if (endsNever(p)) {
		s.end = CycleSegment::kNever;
		s.nextPattern = p;
		s.nextIndex = s.index + 1;
		return;
	}
	// THE WALK AFTER THIS SEGMENT, past any FILLS: the first fill reached merges over this segment's tail; a fill straight
	// after it is passed through (its Next still steers the walk). A walk that only finds fills (a fill whose Next loops
	// back to fills) ends after this segment rather than spinning.
	int64_t index = s.index + 1;
	size_t q = cycleNext(c_, p, index);
	size_t passed = 0;
	while (q < kPatterns && c_.fill[q]) {
		if (s.fill == kPatterns) {
			s.fill = q;
		}
		// A FILL'S RANDOM NEXT MAY LAND ON ANOTHER FILL BY CHANCE, a few times running: that is a walk, not a dead end, so only a
		// run no dice plausibly make ends it (a fill whose Next only reaches fills, Next or Loop, stops after one lap as before).
		if (++passed > (c_.next[q] == NextAction::Random ? kFillPassLimit : c_.count)) {
			q = kPatterns;
			break;
		}
		++index;
		q = cycleNext(c_, q, index);
	}
	s.nextPattern = q;
	s.nextIndex = index;
	// ITS END: Repeats × the cycle on its own grid (Auto), or Repeats × the Span's slot from where it began (bars).
	s.end = c_.spanSlot[p] > 0.0 ? s.base + static_cast<double>(c_.repeats[p]) * c_.spanSlot[p]
	                             : static_cast<double>(s.first + c_.span[p]) * c_.unit[p];
}

CycleSegment CycleWalk::first() const noexcept {
	CycleSegment s;
	// THE FIRST PATTERN IN RANGE THAT IS NOT A FILL (a fill first in the order has nothing before it to merge into).
	for (size_t i = 0; i < c_.count && s.pattern >= kPatterns; ++i) {
		if (!c_.fill[c_.order[i]]) {
			s.pattern = c_.order[i];
		}
	}
	if (s.pattern >= kPatterns) {
		return s; // nothing in range: silent everywhere
	}
	s.first = 0;
	s.base = 0.0;
	finish(s);
	return s;
}

CycleSegment CycleWalk::after(const CycleSegment& s) const noexcept {
	if (s.pattern >= kPatterns || !(s.end < CycleSegment::kNever)) {
		return s;
	}
	CycleSegment n;
	n.index = s.nextIndex;
	n.start = s.end;
	n.pattern = s.nextPattern;
	if (n.pattern >= kPatterns) {
		return n; // STOP: silent from here on
	}
	n.first = firstSlotAtOrAfter(s.end, c_.unit[n.pattern]);
	n.base = s.end;
	finish(n);
	return n;
}

void CycleWalk::configure(const CycleConfig& c) noexcept {
	if (configured_ && c == c_) {
		return;
	}
	c_ = c;
	for (size_t p = 0; p < kPatterns; ++p) {
		if (!(c_.unit[p] > 0.0) || !std::isfinite(c_.unit[p])) {
			c_.unit[p] = 1.0;
		}
		c_.span[p] = std::max<int64_t>(1, c_.span[p]);
		if (!(c_.spanSlot[p] > 0.0) || !std::isfinite(c_.spanSlot[p])) {
			c_.spanSlot[p] = 0.0;
		}
		c_.repeats[p] = std::max(1, c_.repeats[p]);
	}
	configured_ = true;
	markCount_ = 0;
	stride_ = 1;
	haveCursor_ = false;
	steps_ = 0;
}

void CycleWalk::mark(const CycleSegment& s) noexcept {
	const auto expected = [&] { return s.index % stride_ == 0 && s.index / stride_ == static_cast<int64_t>(markCount_); };
	if (!expected()) {
		return;
	}
	if (markCount_ == kMarks) {
		// FULL: keep every other checkpoint and double the stride, so the cache spans twice as far in the same room.
		for (size_t i = 0; i < kMarks / 2; ++i) {
			marks_[i] = marks_[2 * i];
		}
		markCount_ = kMarks / 2;
		stride_ *= 2;
		if (!expected()) {
			return;
		}
	}
	marks_[markCount_++] = s;
}

CycleSegment CycleWalk::advance(const CycleSegment& s) noexcept {
	const CycleSegment n = after(s);
	++steps_;
	mark(n);
	return n;
}

CycleSegment CycleWalk::at(double t) noexcept {
	if (!configured_ || !std::isfinite(t)) {
		return first();
	}
	if (haveCursor_ && cursor_.contains(t)) {
		return cursor_;
	}
	// PLAYING ON: the next segment or two.
	if (haveCursor_ && t + cycleTolerance(t) >= cursor_.end) {
		CycleSegment s = cursor_;
		for (int i = 0; i < 4 && !s.contains(t); ++i) {
			s = advance(s);
		}
		if (s.contains(t)) {
			cursor_ = s;
			return s;
		}
	}
	// A LOCATE: from the last checkpoint at or before `t`.
	if (markCount_ == 0) {
		marks_[0] = first();
		markCount_ = 1;
	}
	size_t lo = 0;
	size_t hi = markCount_;
	while (hi - lo > 1) {
		const size_t mid = (lo + hi) / 2;
		if (marks_[mid].start <= t + cycleTolerance(t)) {
			lo = mid;
		}
		else {
			hi = mid;
		}
	}
	CycleSegment s = marks_[lo];
	while (!s.contains(t)) {
		s = advance(s);
	}
	cursor_ = s;
	haveCursor_ = true;
	return s;
}

} // namespace feathers::mod::arp::pat
