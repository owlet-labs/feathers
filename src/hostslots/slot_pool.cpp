/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not,
 * see <https://www.gnu.org/licenses/>.
 */

#include "hostslots/slot_pool.h"

#include <algorithm>
#include <tuple>

namespace feathers::hostslots {

namespace {

bool isUnder(const std::vector<DeviceInstanceId>& path, std::span<const DeviceInstanceId> prefix) {
	return !prefix.empty() && prefix.size() <= path.size() && std::equal(prefix.begin(), prefix.end(), path.begin());
}

void replacePrefix(std::vector<DeviceInstanceId>& path, size_t oldLength, std::span<const DeviceInstanceId> newPrefix) {
	std::vector<DeviceInstanceId> out(newPrefix.begin(), newPrefix.end());
	out.insert(out.end(), path.begin() + static_cast<std::ptrdiff_t>(oldLength), path.end());
	path = std::move(out);
}

/// The fill order: every rank 0 before any rank 1, then outer level first, then chain order. The Default
/// taken first is the one LAST in this order, so taking is exactly the fill undone.
auto fillKey(int32_t rank, int32_t level, int32_t chainOrder) {
	return std::make_tuple(rank, level, chainOrder);
}

const Target kNoTarget{};

} // namespace

SlotPool::SlotPool(uint32_t capacity) {
	(void)grow(std::clamp(capacity, kHostSlotCapacity, kMaxHostSlotCapacity));
}

bool SlotPool::grow(uint32_t capacity) {
	if (capacity > kMaxHostSlotCapacity)
		return false;
	if (capacity <= slots_.size())
		return true;
	const auto first = static_cast<SlotIndex>(slots_.size());
	slots_.resize(capacity);
	renamed_.resize(capacity, false);
	for (SlotIndex i = first; i < capacity; ++i)
		slots_[i].rec.index = i;
	bump();
	return true;
}

SlotIndex SlotPool::slotOf(const Target& target) const {
	for (const Slot& s : slots_)
		if (s.rec.state != SlotState::Empty && s.rec.target.sameControl(target))
			return s.rec.index;
	return kNoSlot;
}

SlotState SlotPool::state(SlotIndex slot) const {
	return slot < slots_.size() ? slots_[slot].rec.state : SlotState::Empty;
}

const Target& SlotPool::target(SlotIndex slot) const {
	return slot < slots_.size() ? slots_[slot].rec.target : kNoTarget;
}

uint64_t SlotPool::recency(SlotIndex slot) const {
	return slot < slots_.size() ? slots_[slot].rec.recency : 0;
}

void SlotPool::markRenamed(SlotIndex slot) {
	renamed_[slot] = true;
}

// THE ALLOCATION ORDER (design §2). An Empty slot with no note is preferred to one that keeps a note of a
// removed device, so that an undo of the removal still finds its slots after a few unrelated touches. That
// refinement is ours: the plan says only "Empty, lowest index".
//
// **AND EVERY CLEAN SLOT BEFORE ANY HOST-MARKED ONE**: the whole order runs twice, over the
// ids the host has never touched and then over the marked ones, so a host lane left on an id drives a new
// control only when nothing else is left. A marked slot taken is always announced, whatever it was.
SlotIndex SlotPool::takeFor(Allocation& out) {
	for (const bool marked : {false, true}) {
		for (int pass = 0; pass < 2; ++pass)
			for (Slot& s : slots_)
				if (s.hostMarked == marked && s.rec.state == SlotState::Empty && s.note.has_value() == (pass == 1)) {
					out.took = Allocation::Took::Empty;
					out.announce = out.hostMarked = marked;
					return s.rec.index;
				}

		const Slot* lowest = nullptr;
		for (const Slot& s : slots_)
			if (s.hostMarked == marked && s.rec.state == SlotState::Default
			    && (lowest == nullptr
			        || fillKey(s.rec.rank, s.rec.level, s.rec.chainOrder)
			               >= fillKey(lowest->rec.rank, lowest->rec.level, lowest->rec.chainOrder)))
				lowest = &s;
		if (lowest != nullptr) {
			out.took = Allocation::Took::Default;
			out.previous = lowest->rec.target;
			out.announce = out.hostMarked = marked;
			return lowest->rec.index;
		}

		const Slot* oldest = nullptr;
		for (const Slot& s : slots_)
			if (s.hostMarked == marked && s.rec.state == SlotState::Recent
			    && (oldest == nullptr || s.rec.recency < oldest->rec.recency))
				oldest = &s;
		if (oldest != nullptr) {
			out.took = Allocation::Took::Recent;
			out.previous = oldest->rec.target;
			out.announce = true;
			out.hostMarked = marked;
			return oldest->rec.index;
		}
	}

	out.took = Allocation::Took::None;
	return kNoSlot;
}

Allocation SlotPool::touch(const Target& target) {
	Allocation out;
	// A target with no path or no key names no control, and `restore` would refuse to read it back.
	if (target.path.empty() || target.key.empty())
		return out;
	const SlotIndex existing = slotOf(target);
	if (existing != kNoSlot) {
		SlotRecord& rec = slots_[existing].rec;
		// Recency only. A Default stays Default: a hand touch outside recording leaves it "untouched" in the
		// design's sense (§2), and it is still taken over silently when the pool needs it.
		rec.recency = ++clock_;
		if (rec.target.label != target.label || rec.target.tag != target.tag) {
			rec.target.label = target.label;
			rec.target.tag = target.tag;
			markRenamed(existing);
			bump();
		}
		out.slot = existing;
		out.took = Allocation::Took::Existing;
		return out;
	}

	const SlotIndex slot = takeFor(out);
	if (slot == kNoSlot)
		return out;
	Slot& s = slots_[slot];
	s.note.reset();
	s.rec = SlotRecord{};
	s.rec.index = slot;
	s.rec.state = SlotState::Recent;
	s.rec.target = target;
	s.rec.recency = ++clock_;
	markRenamed(slot);
	bump();
	out.slot = slot;
	return out;
}

Allocation SlotPool::pin(const Target& target) {
	Allocation out = touch(target);
	if (out.slot == kNoSlot)
		return out;
	SlotRecord& rec = slots_[out.slot].rec;
	if (rec.state == SlotState::Anchored) {
		if (rec.beforeAnchor != SlotState::Pinned) {
			rec.beforeAnchor = SlotState::Pinned;
			bump();
		}
	} else if (rec.state != SlotState::Pinned) {
		rec.state = SlotState::Pinned;
		bump();
	}
	return out;
}

bool SlotPool::unpin(SlotIndex slot) {
	if (state(slot) != SlotState::Pinned)
		return false;
	slots_[slot].rec.state = SlotState::Recent;
	bump();
	return true;
}

bool SlotPool::anchor(SlotIndex slot) {
	const SlotState st = state(slot);
	if (st == SlotState::Empty)
		return false;
	if (st == SlotState::Anchored)
		return true;
	SlotRecord& rec = slots_[slot].rec;
	rec.beforeAnchor = st == SlotState::Pinned ? SlotState::Pinned : SlotState::Recent;
	rec.state = SlotState::Anchored;
	bump();
	return true;
}

bool SlotPool::release(SlotIndex slot) {
	if (state(slot) != SlotState::Anchored)
		return false;
	SlotRecord& rec = slots_[slot].rec;
	rec.state = rec.beforeAnchor == SlotState::Pinned ? SlotState::Pinned : SlotState::Recent;
	rec.beforeAnchor = SlotState::Recent;
	bump();
	return true;
}

bool SlotPool::markHost(SlotIndex slot) {
	if (slot >= slots_.size() || slots_[slot].hostMarked)
		return false;
	slots_[slot].hostMarked = true;
	bump();
	return true;
}

bool SlotPool::hostMarked(SlotIndex slot) const {
	return slot < slots_.size() && slots_[slot].hostMarked;
}

uint32_t SlotPool::fillDefaults(std::span<const Candidate> candidates) {
	std::vector<const Candidate*> order;
	order.reserve(candidates.size());
	for (const Candidate& c : candidates)
		order.push_back(&c);
	std::stable_sort(order.begin(), order.end(), [](const Candidate* a, const Candidate* b) {
		return fillKey(a->rank, a->level, a->chainOrder) < fillKey(b->rank, b->level, b->chainOrder);
	});

	uint32_t filled = 0;
	for (const Candidate* c : order) {
		if (c->target.path.empty() || c->target.key.empty() || slotOf(c->target) != kNoSlot)
			continue;
		// Empty slots only: the same preference as a touch (no note first, then lowest index), and never a
		// Default, a Recent, or anything else. AND NEVER A HOST-MARKED ONE: a default is taken silently, and a
		// marked id carries lanes a player must be told about — which only a hand's touch or pin can say.
		SlotIndex slot = kNoSlot;
		for (int pass = 0; pass < 2 && slot == kNoSlot; ++pass)
			for (const Slot& s : slots_)
				if (!s.hostMarked && s.rec.state == SlotState::Empty && s.note.has_value() == (pass == 1)) {
					slot = s.rec.index;
					break;
				}
		if (slot == kNoSlot)
			break;
		Slot& s = slots_[slot];
		s.note.reset();
		s.rec = SlotRecord{};
		s.rec.index = slot;
		s.rec.state = SlotState::Default;
		s.rec.target = c->target;
		s.rec.level = c->level;
		s.rec.rank = c->rank;
		s.rec.chainOrder = c->chainOrder;
		markRenamed(slot);
		++filled;
	}
	if (filled > 0)
		bump();
	return filled;
}

uint32_t SlotPool::freeUnder(std::span<const DeviceInstanceId> prefix) {
	uint32_t freed = 0;
	const uint64_t serial = ++freeSerial_;
	for (Slot& s : slots_) {
		if (s.rec.state == SlotState::Empty || !isUnder(s.rec.target.path, prefix))
			continue;
		s.note = s.rec;
		s.notedAt = serial;
		const SlotIndex index = s.rec.index;
		s.rec = SlotRecord{};
		s.rec.index = index;
		markRenamed(index);
		++freed;
	}
	if (freed > 0)
		bump();
	return freed;
}

bool SlotPool::free(SlotIndex slot) {
	if (slot >= slots_.size() || slots_[slot].rec.state == SlotState::Empty)
		return false;
	Slot& s = slots_[slot];
	s.note = s.rec;
	s.notedAt = ++freeSerial_;
	s.rec = SlotRecord{};
	s.rec.index = slot;
	markRenamed(slot);
	bump();
	return true;
}

uint32_t SlotPool::restoreUnder(std::span<const DeviceInstanceId> prefix) {
	std::vector<Slot*> noted;
	for (Slot& s : slots_)
		if (s.rec.state == SlotState::Empty && s.note && isUnder(s.note->target.path, prefix))
			noted.push_back(&s);
	// Latest free first, so that when two notes name one control the newer claims it and the older is spent.
	std::stable_sort(noted.begin(), noted.end(), [](const Slot* a, const Slot* b) { return a->notedAt > b->notedAt; });

	uint32_t restored = 0;
	for (Slot* sp : noted) {
		Slot& s = *sp;
		// The control came back with another slot in the meantime (a touch after the undo was begun): one
		// slot per control wins, and this note is spent.
		if (slotOf(s.note->target) != kNoSlot) {
			s.note.reset();
			continue;
		}
		s.rec = *s.note;
		s.note.reset();
		markRenamed(s.rec.index);
		++restored;
	}
	if (restored > 0)
		bump();
	return restored;
}

std::optional<uint32_t> SlotPool::rebase(std::span<const DeviceInstanceId> oldPrefix,
                                         std::span<const DeviceInstanceId> newPrefix, const LabelFor& labelFor) {
	if (oldPrefix.empty() || newPrefix.empty())
		return std::nullopt;

	// Check every collision BEFORE writing anything. Slots under the old prefix cannot collide with each
	// other (the same prefix swapped on each keeps distinct paths distinct), so the only collision is with a
	// slot that is NOT moving.
	for (const Slot& moving : slots_) {
		if (moving.rec.state == SlotState::Empty || !isUnder(moving.rec.target.path, oldPrefix))
			continue;
		Target moved = moving.rec.target;
		replacePrefix(moved.path, oldPrefix.size(), newPrefix);
		for (const Slot& other : slots_)
			if (other.rec.state != SlotState::Empty && !isUnder(other.rec.target.path, oldPrefix)
			    && other.rec.target.sameControl(moved))
				return std::nullopt;
	}

	uint32_t moved = 0;
	for (Slot& s : slots_) {
		if (s.rec.state != SlotState::Empty && isUnder(s.rec.target.path, oldPrefix)) {
			replacePrefix(s.rec.target.path, oldPrefix.size(), newPrefix);
			if (labelFor)
				s.rec.target.label = labelFor(s.rec.target);
			markRenamed(s.rec.index);
			++moved;
		}
		// A note that would collide is not refused: `restoreUnder` already gives up a note whose control
		// has a slot, so it can never produce a second one.
		if (s.note && isUnder(s.note->target.path, oldPrefix)) {
			replacePrefix(s.note->target.path, oldPrefix.size(), newPrefix);
			if (labelFor)
				s.note->target.label = labelFor(s.note->target);
		}
	}
	if (moved > 0)
		bump();
	return moved;
}

uint32_t SlotPool::retextUnder(std::span<const DeviceInstanceId> prefix) {
	uint32_t marked = 0;
	for (const Slot& s : slots_) {
		if (s.rec.state != SlotState::Empty && isUnder(s.rec.target.path, prefix)) {
			markRenamed(s.rec.index);
			++marked;
		}
	}
	return marked;
}

std::vector<bool> SlotPool::takeRenamed() {
	std::vector<bool> out(renamed_.size(), false);
	out.swap(renamed_);
	return out;
}

PoolSnapshot SlotPool::snapshot() const {
	PoolSnapshot snap;
	snap.capacity = capacity();
	snap.clock = clock_;
	for (const Slot& s : slots_) {
		if (s.rec.state != SlotState::Empty)
			snap.slots.push_back(s.rec);
		if (s.hostMarked)
			snap.hostMarked.push_back(s.rec.index);
	}
	return snap;
}

std::string SlotPool::whyInvalid(const PoolSnapshot& snap) {
	// An index is checked against the SNAPSHOT's capacity, not ours: a record past the capacity it was saved
	// with is a corrupt snapshot even when this pool happens to be big enough to hold it. The capacity is
	// checked FIRST, because `seen` is sized from it.
	if (snap.capacity > kMaxHostSlotCapacity)
		return "a capacity of " + std::to_string(snap.capacity) + ", past the most a pool may have ("
		       + std::to_string(kMaxHostSlotCapacity) + ")";
	const uint32_t savedCap = std::max(snap.capacity, kHostSlotCapacity);
	if (snap.clock == UINT64_MAX)
		return "a clock at its last value";
	std::vector<bool> seen(savedCap, false);
	for (size_t i = 0; i < snap.slots.size(); ++i) {
		const SlotRecord& r = snap.slots[i];
		const std::string slot = "slot " + std::to_string(r.index);
		if (r.index >= savedCap)
			return slot + " is past the capacity of " + std::to_string(savedCap);
		if (seen[r.index])
			return slot + " is listed twice";
		seen[r.index] = true;
		if (r.state == SlotState::Empty || static_cast<uint8_t>(r.state) > static_cast<uint8_t>(SlotState::Anchored))
			return slot + " has no usable state";
		if (r.target.path.empty() || r.target.key.empty())
			return slot + " names no control";
		// kNoInstance NAMES NO DEVICE (`instance_id.h`), so a path through it can never resolve.
		if (std::find(r.target.path.begin(), r.target.path.end(), kNoInstance) != r.target.path.end())
			return slot + " has a path through no device";
		if (r.recency > snap.clock)
			return slot + " was used after the pool's clock";
		if (r.beforeAnchor != SlotState::Pinned && r.beforeAnchor != SlotState::Recent)
			return slot + " would release to no usable state";
		for (size_t j = 0; j < i; ++j)
			if (snap.slots[j].target.sameControl(r.target))
				return slot + " names the control slot " + std::to_string(snap.slots[j].index) + " names";
	}
	// THE HOST MARKS: each an index inside the capacity, ascending, none twice.
	for (size_t i = 0; i < snap.hostMarked.size(); ++i) {
		const SlotIndex m = snap.hostMarked[i];
		if (m >= savedCap)
			return "host-marked slot " + std::to_string(m) + " is past the capacity of " + std::to_string(savedCap);
		if (i > 0 && m <= snap.hostMarked[i - 1])
			return "host-marked slot " + std::to_string(m) + " is out of order or listed twice";
	}
	return {};
}

bool SlotPool::restore(const PoolSnapshot& snap) {
	if (!whyInvalid(snap).empty())
		return false;
	const uint32_t savedCap = std::max(snap.capacity, kHostSlotCapacity);
	(void)grow(savedCap); // within the limit: `whyInvalid` refused past it
	for (Slot& s : slots_) {
		const SlotIndex index = s.rec.index;
		if (s.rec.state != SlotState::Empty)
			markRenamed(index);
		s.rec = SlotRecord{};
		s.rec.index = index;
		s.note.reset();
		s.hostMarked = false;
	}
	for (const SlotRecord& r : snap.slots) {
		slots_[r.index].rec = r;
		markRenamed(r.index);
	}
	for (const SlotIndex m : snap.hostMarked)
		slots_[m].hostMarked = true;
	clock_ = snap.clock;
	bump();
	return true;
}

} // namespace feathers::hostslots
