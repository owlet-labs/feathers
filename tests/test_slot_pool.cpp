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

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "hostslots/slot_pool.h"

#include <string>
#include <vector>

// THE HOST SLOT POOL (host-slots plan T1). Every case asserts WHICH slot, in WHICH state, naming WHICH
// control — never merely that something moved. The allocation order is the thing most worth getting wrong,
// so the cases that pin it fill the pool to the exact point where each rule takes over.

using namespace feathers;
using namespace feathers::hostslots;

namespace {

Target control(std::vector<DeviceInstanceId> path, std::string key) {
	Target t;
	t.path = std::move(path);
	t.key = key;
	t.tag = "dev";
	t.label = "Dev · " + key;
	return t;
}

/// A control on a device of its own, numbered so that no two collide with anything a case names by hand.
Target filler(int n) {
	return control({static_cast<DeviceInstanceId>(1000 + n)}, "k");
}

/// A label built from the path, as a caller naming devices along it would: "2/5/9 · time".
std::string pathLabel(const Target& t) {
	std::string s;
	for (size_t i = 0; i < t.path.size(); ++i)
		s += (i ? "/" : "") + std::to_string(t.path[i]);
	return s + " · " + t.key;
}

uint32_t countIn(const SlotPool& pool, SlotState st) {
	uint32_t n = 0;
	for (SlotIndex i = 0; i < pool.capacity(); ++i)
		n += pool.state(i) == st ? 1u : 0u;
	return n;
}

std::vector<SlotIndex> renamedList(SlotPool& pool) {
	std::vector<SlotIndex> out;
	const std::vector<bool> bits = pool.takeRenamed();
	for (SlotIndex i = 0; i < bits.size(); ++i)
		if (bits[i])
			out.push_back(i);
	return out;
}

} // namespace

TEST_CASE("an empty pool hands out slot 0 first") {
	SlotPool pool;
	CHECK(pool.capacity() == kHostSlotCapacity);
	CHECK(countIn(pool, SlotState::Empty) == kHostSlotCapacity);
	const uint64_t gen0 = pool.generation();

	const Target a = control({1}, "cutoff");
	const Allocation first = pool.touch(a);
	CHECK(first.slot == 0);
	CHECK(first.took == Allocation::Took::Empty);
	CHECK_FALSE(first.previous.has_value());
	CHECK_FALSE(first.announce);
	CHECK(pool.state(0) == SlotState::Recent);
	CHECK(pool.target(0) == a);
	CHECK(pool.slotOf(a) == 0);

	const Allocation second = pool.touch(control({1}, "res"));
	CHECK(second.slot == 1);
	CHECK(second.took == Allocation::Took::Empty);
	CHECK(pool.generation() > gen0);
	CHECK(renamedList(pool) == std::vector<SlotIndex>{0, 1});
	CHECK(renamedList(pool).empty());
	CHECK(countIn(pool, SlotState::Empty) == kHostSlotCapacity - 2);
}

TEST_CASE("touching a slotted control allocates nothing and bumps recency") {
	SlotPool pool;
	const Target a = control({1}, "cutoff");
	const Target b = control({2}, "mix");
	REQUIRE(pool.touch(a).slot == 0);
	REQUIRE(pool.touch(b).slot == 1);
	CHECK(pool.recency(0) < pool.recency(1));
	(void)pool.takeRenamed();
	const uint64_t gen = pool.generation();

	// The same control, with a different label and tag: identity is the path and the key.
	Target again = a;
	again.label = "Dev · cutoff";
	const Allocation re = pool.touch(again);
	CHECK(re.slot == 0);
	CHECK(re.took == Allocation::Took::Existing);
	CHECK_FALSE(re.previous.has_value());
	CHECK_FALSE(re.announce);
	CHECK(pool.recency(0) > pool.recency(1));
	CHECK(countIn(pool, SlotState::Empty) == kHostSlotCapacity - 2);
	CHECK(pool.slotOf(b) == 1);
	// Only the recent order moved, so no binding changed and nothing needs a rename.
	CHECK(pool.generation() == gen);
	CHECK(renamedList(pool).empty());

	// A new label on the same control IS a rename, and keeps the slot.
	Target renamed = a;
	renamed.label = "Filter · Cutoff";
	CHECK(pool.touch(renamed).slot == 0);
	CHECK(pool.target(0).label == "Filter · Cutoff");
	CHECK(renamedList(pool) == std::vector<SlotIndex>{0});
	CHECK(pool.generation() > gen);
}

TEST_CASE("a hand on a Default slot's control leaves it Default, and it is still taken over") {
	SlotPool pool;
	const std::vector<Candidate> cands{
		{control({1}, "cutoff"), 0, 0, 0},
		{control({1}, "res"), 0, 1, 0},
	};
	REQUIRE(pool.fillDefaults(cands) == 2);
	REQUIRE(pool.state(0) == SlotState::Default);
	const uint64_t gen = pool.generation();
	const uint64_t before = pool.recency(1);
	const Allocation a = pool.touch(cands[1].target);
	CHECK(a.slot == 1);
	CHECK(a.took == Allocation::Took::Existing);
	CHECK(pool.state(1) == SlotState::Default);
	CHECK(pool.recency(1) > before);
	CHECK(pool.generation() == gen);

	// Fill the rest with Recents, then need one more: the touched Default (the lowest priority) goes, silently.
	for (int i = 0; i + 2 < static_cast<int>(kHostSlotCapacity); ++i)
		REQUIRE(pool.touch(filler(i)).took == Allocation::Took::Empty);
	const Allocation take = pool.touch(control({9}, "new"));
	CHECK(take.slot == 1);
	CHECK(take.took == Allocation::Took::Default);
	CHECK_FALSE(take.announce);
	REQUIRE(take.previous.has_value());
	CHECK(take.previous->sameControl(cands[1].target));
}

TEST_CASE("a full pool takes the lowest-priority untouched Default before any Recent") {
	SlotPool pool;
	// Fill order is (rank, level, chainOrder): A#0, C#0 (level 1), A#1, B#1 — so slots 0..3 in that order.
	const std::vector<Candidate> cands{
		{control({2}, "b1"), 0, 1, 1},
		{control({1}, "a0"), 0, 0, 0},
		{control({3}, "c0"), 1, 0, 0},
		{control({1}, "a1"), 0, 1, 0},
	};
	REQUIRE(pool.fillDefaults(cands) == 4);
	REQUIRE(pool.target(0).key == "a0");
	REQUIRE(pool.target(1).key == "c0");
	REQUIRE(pool.target(2).key == "a1");
	REQUIRE(pool.target(3).key == "b1");
	for (int i = 0; i + 4 < static_cast<int>(kHostSlotCapacity); ++i)
		REQUIRE(pool.touch(filler(i)).slot == static_cast<SlotIndex>(4 + i));
	REQUIRE(countIn(pool, SlotState::Empty) == 0);
	REQUIRE(countIn(pool, SlotState::Recent) == kHostSlotCapacity - 4);

	// Four takeovers undo the fill exactly, last-filled first, each silent.
	const char* expected[] = {"b1", "a1", "c0", "a0"};
	const SlotIndex expectedSlot[] = {3, 2, 1, 0};
	for (int i = 0; i < 4; ++i) {
		const Allocation a = pool.touch(control({77}, "x" + std::to_string(i)));
		CHECK(a.slot == expectedSlot[i]);
		CHECK(a.took == Allocation::Took::Default);
		REQUIRE(a.previous.has_value());
		CHECK(a.previous->key == expected[i]);
		CHECK_FALSE(a.announce);
		CHECK(pool.state(a.slot) == SlotState::Recent);
		CHECK(pool.slotOf(control({77}, "x" + std::to_string(i))) == expectedSlot[i]);
	}
	CHECK(countIn(pool, SlotState::Default) == 0);

	// Only now, with no Default left, is a Recent recycled: the oldest, filler 0 on slot 4.
	const Allocation last = pool.touch(control({78}, "y"));
	CHECK(last.slot == 4);
	CHECK(last.took == Allocation::Took::Recent);
	CHECK(last.announce);
	REQUIRE(last.previous.has_value());
	CHECK(last.previous->sameControl(filler(0)));
}

TEST_CASE("Recent is recycled LRU only when full, and announced") {
	SlotPool pool;
	for (int i = 0; i + 1 < static_cast<int>(kHostSlotCapacity); ++i)
		REQUIRE(pool.touch(filler(i)).slot == static_cast<SlotIndex>(i));

	// One Empty left: it is taken, and no Recent is disturbed.
	const Allocation empty = pool.touch(control({1}, "a"));
	CHECK(empty.slot == kHostSlotCapacity - 1);
	CHECK(empty.took == Allocation::Took::Empty);
	CHECK_FALSE(empty.announce);
	CHECK(countIn(pool, SlotState::Recent) == kHostSlotCapacity);

	// Filler 0 is the oldest, but a touch moves it to the front, so filler 1 is now the least recent.
	REQUIRE(pool.touch(filler(0)).took == Allocation::Took::Existing);
	const Allocation recycled = pool.touch(control({1}, "b"));
	CHECK(recycled.slot == 1);
	CHECK(recycled.took == Allocation::Took::Recent);
	CHECK(recycled.announce);
	REQUIRE(recycled.previous.has_value());
	CHECK(recycled.previous->sameControl(filler(1)));
	CHECK(pool.slotOf(filler(1)) == kNoSlot);
	CHECK(pool.slotOf(filler(0)) == 0);
	CHECK(pool.slotOf(control({1}, "b")) == 1);

	// And the next one after that is filler 2.
	const Allocation next = pool.touch(control({1}, "c"));
	CHECK(next.slot == 2);
	CHECK(next.announce);
}

TEST_CASE("pinned and anchored are never taken; a pool full of anchors answers None") {
	SlotPool pool;
	// The OLDEST slot is pinned and the second oldest anchored, so an LRU that ignored states would take them.
	REQUIRE(pool.pin(control({1}, "pinned")).slot == 0);
	REQUIRE(pool.touch(control({1}, "anchored")).slot == 1);
	REQUIRE(pool.anchor(1));
	for (int i = 0; i + 2 < static_cast<int>(kHostSlotCapacity); ++i)
		REQUIRE(pool.touch(filler(i)).slot == static_cast<SlotIndex>(2 + i));
	CHECK(pool.state(0) == SlotState::Pinned);
	CHECK(pool.state(1) == SlotState::Anchored);

	const Allocation a = pool.touch(control({2}, "new"));
	CHECK(a.slot == 2);
	CHECK(a.took == Allocation::Took::Recent);
	CHECK(pool.slotOf(control({1}, "pinned")) == 0);
	CHECK(pool.slotOf(control({1}, "anchored")) == 1);

	// Now anchor or pin every slot: nothing can be taken.
	for (SlotIndex i = 2; i < kHostSlotCapacity; ++i) {
		if (i % 2 == 0)
			REQUIRE(pool.anchor(i));
		else
			REQUIRE(pool.pin(pool.target(i)).slot == i);
	}
	CHECK(countIn(pool, SlotState::Anchored) == kHostSlotCapacity / 2);
	CHECK(countIn(pool, SlotState::Pinned) == kHostSlotCapacity / 2);
	const PoolSnapshot before = pool.snapshot();
	const uint64_t gen = pool.generation();
	(void)pool.takeRenamed();

	const Allocation none = pool.touch(control({3}, "late"));
	CHECK(none.slot == kNoSlot);
	CHECK(none.took == Allocation::Took::None);
	CHECK_FALSE(none.previous.has_value());
	CHECK_FALSE(none.announce);
	CHECK(pool.pin(control({3}, "late")).slot == kNoSlot);
	CHECK(pool.slotOf(control({3}, "late")) == kNoSlot);
	CHECK(pool.snapshot() == before);
	CHECK(pool.generation() == gen);
	CHECK(renamedList(pool).empty());
}

TEST_CASE("pin, unpin, anchor and release move between exactly the states the design names") {
	SlotPool pool;
	const Target a = control({1}, "a");
	REQUIRE(pool.touch(a).slot == 0);
	CHECK_FALSE(pool.unpin(0));      // not Pinned
	CHECK_FALSE(pool.release(0));    // not Anchored
	CHECK_FALSE(pool.anchor(5));     // Empty: nothing to protect
	CHECK(pool.state(5) == SlotState::Empty);
	CHECK_FALSE(pool.anchor(kNoSlot));
	// A target with no path or no key names nothing and gets nothing.
	CHECK(pool.touch(control({}, "k")).took == Allocation::Took::None);
	CHECK(pool.touch(control({1}, "")).took == Allocation::Took::None);
	CHECK(pool.state(1) == SlotState::Empty);

	CHECK(pool.pin(a).took == Allocation::Took::Existing);
	CHECK(pool.state(0) == SlotState::Pinned);
	CHECK(pool.anchor(0));
	CHECK(pool.state(0) == SlotState::Anchored);
	CHECK_FALSE(pool.unpin(0));      // an anchor outranks the pin
	CHECK(pool.release(0));
	CHECK(pool.state(0) == SlotState::Pinned); // back to what it was
	CHECK(pool.unpin(0));
	CHECK(pool.state(0) == SlotState::Recent);

	CHECK(pool.anchor(0));
	CHECK(pool.pin(a).slot == 0);    // pinning an anchored slot leaves it Anchored...
	CHECK(pool.state(0) == SlotState::Anchored);
	CHECK(pool.release(0));
	CHECK(pool.state(0) == SlotState::Pinned); // ...and a release lands on the pin
	CHECK(pool.slotOf(a) == 0);
}

TEST_CASE("defaults fill in tiers: every device's #1 before any #2, outer level first") {
	SlotPool pool;
	std::vector<Candidate> cands;
	// A long outer chain device D (level 0, chain 0) with 70 ranked controls, a second outer device B
	// (level 0, chain 1) with 3, and a nested device N (level 1, chain 0) with 3. Given in a scrambled order.
	// (D has capacity + 6 controls: 70 when the pool was 64; sized from the constant so the next growth holds.)
	for (int r = static_cast<int>(kHostSlotCapacity) + 5; r >= 0; --r)
		cands.push_back({control({10}, "d" + std::to_string(r)), 0, r, 0});
	for (int r = 2; r >= 0; --r)
		cands.push_back({control({7, 20}, "n" + std::to_string(r)), 1, r, 0});
	for (int r = 2; r >= 0; --r)
		cands.push_back({control({11}, "b" + std::to_string(r)), 0, r, 1});

	CHECK(pool.fillDefaults(cands) == kHostSlotCapacity);
	// Tier 0: D, B (outer, chain order), then N (nested). Tier 1 the same. Tier 2 the same. Then D alone.
	const std::vector<std::string> head{"d0", "b0", "n0", "d1", "b1", "n1", "d2", "b2", "n2", "d3", "d4"};
	for (SlotIndex i = 0; i < head.size(); ++i) {
		CHECK(pool.target(i).key == head[i]);
		CHECK(pool.state(i) == SlotState::Default);
	}
	// Slots 9..cap-1 are D's d3..d(cap-7); the rest did not fit, and the long chain did not crowd out B's or N's.
	CHECK(pool.target(kHostSlotCapacity - 1).key == "d" + std::to_string(kHostSlotCapacity - 7));
	CHECK(pool.slotOf(control({10}, "d" + std::to_string(kHostSlotCapacity - 6))) == kNoSlot);
	CHECK(pool.slotOf(control({7, 20}, "n0")) == 2);

	const PoolSnapshot snap = pool.snapshot();
	CHECK(snap.slots[2].level == 1);
	CHECK(snap.slots[2].rank == 0);
	CHECK(snap.slots[3].rank == 1);
}

TEST_CASE("defaults fill Empty only and never reshuffle") {
	SlotPool pool;
	const std::vector<Candidate> first{
		{control({1}, "a0"), 0, 0, 0},
		{control({1}, "a1"), 0, 1, 0},
	};
	REQUIRE(pool.fillDefaults(first) == 2);
	REQUIRE(pool.touch(control({5}, "hand")).slot == 2);
	REQUIRE(pool.pin(control({5}, "pin")).slot == 3);
	const PoolSnapshot before = pool.snapshot();

	// A device added later, OUTER and rank 0, so it outranks every Default already placed. It still gets
	// only Empty slots, and the old candidates offered again change nothing.
	std::vector<Candidate> later = first;
	later.push_back({control({2}, "z0"), 0, 0, 0});
	later.push_back({control({2}, "z1"), 0, 1, 0});
	later.push_back({control({5}, "hand"), 0, 0, 0}); // already slotted by a hand: skipped
	CHECK(pool.fillDefaults(later) == 2);
	for (const SlotRecord& r : before.slots) {
		CHECK(pool.state(r.index) == r.state);
		CHECK(pool.target(r.index) == r.target);
	}
	CHECK(pool.target(4).key == "z0");
	CHECK(pool.target(5).key == "z1");
	CHECK(pool.state(4) == SlotState::Default);
	CHECK(pool.state(2) == SlotState::Recent);
	CHECK(pool.state(3) == SlotState::Pinned);

	// With no Empty slot left, a fill places nothing and takes nothing.
	for (int i = 0; i + 6 < static_cast<int>(kHostSlotCapacity); ++i)
		REQUIRE(pool.touch(filler(i)).took == Allocation::Took::Empty);
	const PoolSnapshot full = pool.snapshot();
	const Candidate outranking{control({3}, "q0"), 0, 0, 0};
	CHECK(pool.fillDefaults(std::span(&outranking, 1)) == 0);
	CHECK(pool.snapshot() == full);
}

TEST_CASE("removal frees anchored slots too; undo (restoreUnder) brings back state and recency") {
	SlotPool pool;
	const Candidate def{control({5}, "def"), 0, 0, 0};
	REQUIRE(pool.fillDefaults(std::span(&def, 1)) == 1);           // slot 0 Default
	REQUIRE(pool.touch(control({5}, "recent")).slot == 1);         // slot 1 Recent
	REQUIRE(pool.pin(control({5}, "pinned")).slot == 2);           // slot 2 Pinned
	REQUIRE(pool.touch(control({5}, "anchored")).slot == 3);
	REQUIRE(pool.anchor(3));                                       // slot 3 Anchored
	REQUIRE(pool.touch(control({5, 9}, "nested")).slot == 4);      // slot 4 inside a rack at 5
	REQUIRE(pool.touch(control({6}, "other")).slot == 5);          // slot 5 another device
	REQUIRE(pool.touch(control({55}, "prefix")).slot == 6);        // 55 is not under 5
	const PoolSnapshot before = pool.snapshot();
	(void)pool.takeRenamed();
	const uint64_t gen = pool.generation();

	const DeviceInstanceId five[] = {5};
	CHECK(pool.freeUnder(five) == 5);
	for (SlotIndex i = 0; i < 5; ++i) {
		CHECK(pool.state(i) == SlotState::Empty);
		CHECK(pool.target(i).path.empty());
	}
	CHECK(pool.slotOf(control({5}, "anchored")) == kNoSlot);
	CHECK(pool.state(5) == SlotState::Recent);
	CHECK(pool.state(6) == SlotState::Recent);
	CHECK(renamedList(pool) == std::vector<SlotIndex>{0, 1, 2, 3, 4});
	CHECK(pool.generation() > gen);

	// An empty prefix frees nothing, and restoring under an unrelated prefix restores nothing.
	CHECK(pool.freeUnder(std::span<const DeviceInstanceId>{}) == 0);
	const DeviceInstanceId six[] = {6};
	CHECK(pool.restoreUnder(six) == 0);

	CHECK(pool.restoreUnder(five) == 5);
	CHECK(pool.snapshot() == before);
	CHECK(renamedList(pool) == std::vector<SlotIndex>{0, 1, 2, 3, 4});
	// The notes are spent: a second undo does nothing.
	CHECK(pool.restoreUnder(five) == 0);
}

TEST_CASE("free(n) frees one slot, keeps its note, and leaves every other slot and note alone") {
	SlotPool pool;
	REQUIRE(pool.pin(control({5}, "a")).slot == 0);             // slot 0 Pinned on device 5
	REQUIRE(pool.touch(control({5}, "b")).slot == 1);           // slot 1 Recent, same device
	REQUIRE(pool.touch(control({7}, "c")).slot == 2);           // slot 2 on device 7
	REQUIRE(pool.anchor(2));                                    // … Anchored
	const DeviceInstanceId seven[] = {7};
	REQUIRE(pool.freeUnder(seven) == 1);                        // an unrelated undo note on slot 2
	(void)pool.takeRenamed();
	const uint64_t gen = pool.generation();

	CHECK(pool.free(0));
	CHECK(pool.state(0) == SlotState::Empty);
	CHECK(pool.target(0).path.empty());
	CHECK(pool.slotOf(control({5}, "a")) == kNoSlot);
	// ONLY THAT ONE: its sibling on the same device is untouched.
	CHECK(pool.state(1) == SlotState::Recent);
	CHECK(pool.target(1) == control({5}, "b"));
	// Exactly one rename, and the generation moved.
	CHECK(renamedList(pool) == std::vector<SlotIndex>{0});
	CHECK(pool.generation() > gen);

	// An Empty slot, or one past the capacity, frees nothing and renames nothing.
	CHECK_FALSE(pool.free(0));
	CHECK_FALSE(pool.free(pool.capacity()));
	CHECK(renamedList(pool).empty());

	// THE NOTE IS KEPT: an undo over its path brings back the same slot in the same state, and the unrelated
	// note on slot 2 survived to be restored by its own undo.
	const DeviceInstanceId five[] = {5};
	CHECK(pool.restoreUnder(five) == 1);
	CHECK(pool.state(0) == SlotState::Pinned);
	CHECK(pool.target(0) == control({5}, "a"));
	CHECK(pool.restoreUnder(seven) == 1);
	CHECK(pool.state(2) == SlotState::Anchored);
	CHECK(pool.target(2) == control({7}, "c"));
}

TEST_CASE("a freed slot reused before undo is not restored") {
	SlotPool pool;
	const Target gone = control({5}, "anchored");
	REQUIRE(pool.touch(gone).slot == 0);
	REQUIRE(pool.anchor(0));
	for (int i = 0; i + 1 < static_cast<int>(kHostSlotCapacity); ++i)
		REQUIRE(pool.touch(filler(i)).slot == static_cast<SlotIndex>(1 + i));

	const DeviceInstanceId five[] = {5};
	REQUIRE(pool.freeUnder(five) == 1);
	// Slot 0 is the only Empty one, so it is taken — and it inherits whatever lane the host holds on it,
	// which is acceptable by design.
	const Target newcomer = control({8}, "new");
	const Allocation a = pool.touch(newcomer);
	CHECK(a.slot == 0);
	CHECK(a.took == Allocation::Took::Empty);

	CHECK(pool.restoreUnder(five) == 0);
	CHECK(pool.target(0) == newcomer);
	CHECK(pool.state(0) == SlotState::Recent);
	CHECK(pool.slotOf(gone) == kNoSlot);
}

TEST_CASE("a touch prefers an Empty slot with no note, so an undo survives it") {
	SlotPool pool;
	const Target gone = control({5}, "anchored");
	REQUIRE(pool.touch(gone).slot == 0);
	REQUIRE(pool.anchor(0));
	const DeviceInstanceId five[] = {5};
	REQUIRE(pool.freeUnder(five) == 1);

	// Slot 0 is the lowest Empty index, but it holds a note; slot 1 is taken instead.
	CHECK(pool.touch(control({8}, "new")).slot == 1);
	const Candidate c{control({9}, "def"), 0, 0, 0};
	CHECK(pool.fillDefaults(std::span(&c, 1)) == 1);
	CHECK(pool.slotOf(c.target) == 2);

	CHECK(pool.restoreUnder(five) == 1);
	CHECK(pool.slotOf(gone) == 0);
	CHECK(pool.state(0) == SlotState::Anchored);
}

TEST_CASE("reordering changes nothing (targets are paths)") {
	SlotPool pool;
	const std::vector<Candidate> chain{
		{control({3}, "a0"), 0, 0, 0},
		{control({4}, "b0"), 0, 0, 1},
	};
	REQUIRE(pool.fillDefaults(chain) == 2);
	REQUIRE(pool.touch(control({4}, "b1")).slot == 2);
	REQUIRE(pool.anchor(2));
	const PoolSnapshot before = pool.snapshot();
	(void)pool.takeRenamed();
	const uint64_t gen = pool.generation();

	// The drag swaps the two devices: their chain order is all that moves, and ids stay.
	const std::vector<Candidate> swapped{
		{control({4}, "b0"), 0, 0, 0},
		{control({3}, "a0"), 0, 0, 1},
		{control({4}, "b1"), 0, 1, 0},
	};
	CHECK(pool.fillDefaults(swapped) == 0);
	CHECK(pool.snapshot() == before);
	CHECK(pool.generation() == gen);
	CHECK(renamedList(pool).empty());
	CHECK(pool.slotOf(control({3}, "a0")) == 0);
	CHECK(pool.slotOf(control({4}, "b0")) == 1);
	CHECK(pool.slotOf(control({4}, "b1")) == 2);
	CHECK(pool.state(2) == SlotState::Anchored);
}

TEST_CASE("rebase rewrites the path and keeps ids and states") {
	SlotPool pool;
	REQUIRE(pool.touch(control({5}, "mix")).slot == 0);
	REQUIRE(pool.anchor(0));
	REQUIRE(pool.pin(control({5, 9}, "time")).slot == 1);
	REQUIRE(pool.touch(control({6}, "other")).slot == 2);
	REQUIRE(pool.touch(control({5}, "gone")).slot == 3);
	const PoolSnapshot before = pool.snapshot();
	(void)pool.takeRenamed();
	const uint64_t gen = pool.generation();

	// Device 5 moves into a rack at 2. The caller names each target at its new path.
	const DeviceInstanceId from[] = {5};
	const DeviceInstanceId to[] = {2, 5};
	CHECK(pool.rebase(from, to, pathLabel) == std::optional<uint32_t>(3));
	// The renamed slots carry the NEW path-qualified label; the unmoved one keeps its own.
	CHECK(pool.target(0).label == "2/5 · mix");
	CHECK(pool.target(1).label == "2/5/9 · time");
	CHECK(pool.target(3).label == "2/5 · gone");
	CHECK(pool.target(2).label == "Dev · other");
	CHECK(pool.target(0).path == std::vector<DeviceInstanceId>{2, 5});
	CHECK(pool.target(1).path == std::vector<DeviceInstanceId>{2, 5, 9});
	CHECK(pool.target(3).path == std::vector<DeviceInstanceId>{2, 5});
	CHECK(pool.target(2).path == std::vector<DeviceInstanceId>{6});
	CHECK(pool.state(0) == SlotState::Anchored);
	CHECK(pool.state(1) == SlotState::Pinned);
	for (SlotIndex i = 0; i < 4; ++i) {
		CHECK(pool.recency(i) == before.slots[i].recency);
		CHECK(pool.target(i).key == before.slots[i].target.key);
	}
	CHECK(pool.slotOf(control({5}, "mix")) == kNoSlot);
	CHECK(pool.slotOf(control({2, 5}, "mix")) == 0);
	CHECK(renamedList(pool) == std::vector<SlotIndex>{0, 1, 3});
	CHECK(pool.generation() > gen);

	// A NOTE is rebased too: free the moved device, rebase it back, and the undo lands at the old path.
	const DeviceInstanceId inner[] = {2, 5};
	REQUIRE(pool.freeUnder(inner) == 3);
	CHECK(pool.rebase(inner, from, pathLabel) == std::optional<uint32_t>(0)); // no live slot moved...
	CHECK(pool.restoreUnder(from) == 3);                                       // ...but the notes did
	const PoolSnapshot back = pool.snapshot();
	REQUIRE(back.slots.size() == before.slots.size());
	for (size_t i = 0; i < back.slots.size(); ++i) {
		SlotRecord expected = before.slots[i];
		if (expected.target.path.front() == 5)
			expected.target.label = pathLabel(expected.target); // the note was relabelled on the way back
		CHECK(back.slots[i] == expected);
	}
	CHECK(pool.target(0).label == "5 · mix");
}

TEST_CASE("rebase refuses a collision and changes nothing") {
	SlotPool pool;
	// Device 5's Mix has slot 0 (anchored). Somebody already slotted Mix at the path it is moving TO.
	REQUIRE(pool.touch(control({5}, "mix")).slot == 0);
	REQUIRE(pool.anchor(0));
	REQUIRE(pool.touch(control({5}, "time")).slot == 1);
	REQUIRE(pool.touch(control({2, 5}, "mix")).slot == 2);
	const PoolSnapshot before = pool.snapshot();
	(void)pool.takeRenamed();
	const uint64_t gen = pool.generation();

	const DeviceInstanceId from[] = {5};
	const DeviceInstanceId to[] = {2, 5};
	CHECK_FALSE(pool.rebase(from, to, pathLabel).has_value());
	// Refused WHOLE: not even the non-colliding Time moved.
	CHECK(pool.snapshot() == before);
	CHECK(pool.target(1).path == std::vector<DeviceInstanceId>{5});
	CHECK(pool.generation() == gen);
	CHECK(renamedList(pool).empty());
	// And so the pool still saves a snapshot a fresh instance accepts, anchor and all.
	SlotPool fresh;
	CHECK(fresh.restore(pool.snapshot()));
	CHECK(fresh.state(0) == SlotState::Anchored);

	// Empty prefixes are refused too.
	CHECK_FALSE(pool.rebase(std::span<const DeviceInstanceId>{}, to, pathLabel).has_value());
	CHECK_FALSE(pool.rebase(from, std::span<const DeviceInstanceId>{}, pathLabel).has_value());
	CHECK(pool.snapshot() == before);
}

TEST_CASE("restoreUnder restores the later of two notes on one control") {
	SlotPool pool;
	const Target x = control({5}, "x");
	REQUIRE(pool.pin(x).slot == 0);
	const DeviceInstanceId five[] = {5};
	REQUIRE(pool.freeUnder(five) == 1); // note A on slot 0: Pinned

	// The device comes back WITHOUT the undo, X is touched (slot 1, since slot 0 holds a note), anchored,
	// and the device is removed again: note B on slot 1, Anchored and later.
	REQUIRE(pool.touch(x).slot == 1);
	REQUIRE(pool.anchor(1));
	REQUIRE(pool.freeUnder(five) == 1);

	// Slot 0 comes first by index, so an undo that walked in index order would restore the OLDER state.
	CHECK(pool.restoreUnder(five) == 1);
	CHECK(pool.slotOf(x) == 1);
	CHECK(pool.state(1) == SlotState::Anchored);
	CHECK(pool.state(0) == SlotState::Empty);
	// The older note is spent, not waiting: a second undo restores nothing.
	CHECK(pool.restoreUnder(five) == 0);
}

TEST_CASE("snapshot/restore round-trips states, recency and anchors exactly") {
	constexpr uint32_t kGrown = kHostSlotCapacity + 16; // a grown pool: the capacity must round-trip too
	SlotPool pool(kGrown);
	const Candidate def{control({1}, "cutoff"), 0, 1, 2};
	REQUIRE(pool.fillDefaults(std::span(&def, 1)) == 1);
	REQUIRE(pool.touch(control({2}, "mix")).slot == 1);
	REQUIRE(pool.pin(control({2, 7}, "time")).slot == 2);
	REQUIRE(pool.touch(control({3}, "drive")).slot == 3);
	REQUIRE(pool.anchor(3));
	REQUIRE(pool.pin(control({4}, "size")).slot == 4);
	REQUIRE(pool.anchor(4)); // anchored over a pin
	REQUIRE(pool.touch(control({2}, "mix")).took == Allocation::Took::Existing);
	const PoolSnapshot snap = pool.snapshot();
	CHECK(snap.capacity == kGrown);
	CHECK(snap.slots.size() == 5);

	// A fresh instance, as a project reload makes one.
	SlotPool fresh;
	REQUIRE(fresh.restore(snap));
	CHECK(fresh.capacity() == kGrown);
	CHECK(fresh.snapshot() == snap);
	CHECK(fresh.state(0) == SlotState::Default);
	CHECK(fresh.state(3) == SlotState::Anchored);
	CHECK(fresh.state(4) == SlotState::Anchored);
	CHECK(fresh.recency(1) > fresh.recency(3));
	std::vector<SlotIndex> named;
	const std::vector<bool> bits = fresh.takeRenamed();
	for (SlotIndex i = 0; i < bits.size(); ++i)
		if (bits[i])
			named.push_back(i);
	CHECK(named == std::vector<SlotIndex>{0, 1, 2, 3, 4});

	// The anchors hold with no host event in between: 200 touches recycle everything else, never them.
	for (int i = 0; i < 200; ++i)
		(void)fresh.touch(filler(i));
	CHECK(fresh.slotOf(control({3}, "drive")) == 3);
	CHECK(fresh.slotOf(control({4}, "size")) == 4);
	CHECK(fresh.slotOf(control({2, 7}, "time")) == 2);
	CHECK(fresh.state(3) == SlotState::Anchored);
	// The clock survived too, so a restored recency still sorts against new touches.
	CHECK(fresh.recency(10) > snap.clock);
	// And the anchored-over-pin slot releases to Pinned, as it would have before the save.
	CHECK(fresh.release(4));
	CHECK(fresh.state(4) == SlotState::Pinned);
}

TEST_CASE("the capacity grows and never shrinks") {
	constexpr uint32_t kBig = kHostSlotCapacity + 6;
	SlotPool small(10);
	CHECK(small.capacity() == kHostSlotCapacity);
	SlotPool pool;
	pool.grow(kBig);
	CHECK(pool.capacity() == kBig);
	pool.grow(40);
	CHECK(pool.capacity() == kBig);

	// A snapshot from a default-sized pool restored into a bigger one leaves it at the bigger size.
	SlotPool from;
	REQUIRE(from.touch(control({1}, "a")).slot == 0);
	REQUIRE(pool.restore(from.snapshot()));
	CHECK(pool.capacity() == kBig);
	CHECK(pool.snapshot().capacity == kBig);
	CHECK(pool.slotOf(control({1}, "a")) == 0);
	CHECK(pool.takeRenamed().size() == kBig);

	// AND A POOL SAVED AT 64 (before 2026-09-26) LOADS AND GROWS to today's capacity, its slots where they were.
	PoolSnapshot old;
	old.capacity = 64;
	old.clock = 3;
	SlotRecord r;
	r.index = 63;
	r.state = SlotState::Anchored;
	r.target = control({1}, "old");
	r.recency = 3;
	old.slots = {r};
	SlotPool fresh;
	REQUIRE(fresh.restore(old));
	CHECK(fresh.capacity() == kHostSlotCapacity);
	CHECK(fresh.state(63) == SlotState::Anchored);
	CHECK(fresh.slotOf(control({1}, "old")) == 63);
}

TEST_CASE("restore refuses a corrupt snapshot whole and leaves the pool as it was") {
	SlotPool pool;
	REQUIRE(pool.touch(control({1}, "keep")).slot == 0);
	REQUIRE(pool.anchor(0));
	const PoolSnapshot before = pool.snapshot();
	const uint64_t gen = pool.generation();

	SlotRecord good;
	good.index = 3;
	good.state = SlotState::Recent;
	good.target = control({2}, "x");
	good.recency = 1;
	PoolSnapshot base;
	base.clock = 5;
	base.slots = {good};

	auto refused = [&](PoolSnapshot s) {
		const bool ok = pool.restore(s);
		CHECK_FALSE(ok);
		CHECK(pool.snapshot() == before);
		CHECK(pool.generation() == gen);
	};
	{ PoolSnapshot s = base; s.slots[0].index = kHostSlotCapacity; refused(s); }   // out of range
	{ PoolSnapshot s = base; s.slots.push_back(good); s.slots[1].target.key = "y"; refused(s); } // index twice
	{ PoolSnapshot s = base; s.slots.push_back(good); s.slots[1].index = 4; refused(s); }        // control twice
	{ PoolSnapshot s = base; s.slots[0].state = SlotState::Empty; refused(s); }
	{ PoolSnapshot s = base; s.slots[0].target.path.clear(); refused(s); }
	{ PoolSnapshot s = base; s.slots[0].target.key.clear(); refused(s); }
	{ PoolSnapshot s = base; s.slots[0].recency = 6; refused(s); }                 // ahead of the clock
	{ PoolSnapshot s = base; s.slots[0].beforeAnchor = SlotState::Default; refused(s); }
	{ PoolSnapshot s = base; s.slots[0].state = static_cast<SlotState>(5); refused(s); } // not one of the five
	{ PoolSnapshot s = base; s.slots[0].state = static_cast<SlotState>(200); refused(s); }
	{ PoolSnapshot s = base; s.slots[0].beforeAnchor = static_cast<SlotState>(9); refused(s); }
	{ PoolSnapshot s = base; s.clock = UINT64_MAX; refused(s); }                   // the next touch would wrap

	// The good one is accepted and REPLACES the pool: the old anchor is gone, because the patch said so.
	REQUIRE(pool.restore(base));
	CHECK(pool.state(0) == SlotState::Empty);
	CHECK(pool.slotOf(control({2}, "x")) == 3);
	CHECK(pool.snapshot() == PoolSnapshot{kHostSlotCapacity, 5, {good}, {}});
}

TEST_CASE("the capacity has one ceiling: a pool refuses to grow past it, and 4096 is accepted where 4097 is refused") {
	// THE WRITER AND THE READER SHARE ONE LIMIT: a pool that could grow past what its own saved document may
	// name would save a member its reader drops whole, and lose every anchor with it.
	REQUIRE(kMaxHostSlotCapacity == 4096);
	SlotPool pool;
	CHECK_FALSE(pool.grow(kMaxHostSlotCapacity + 1));
	CHECK(pool.capacity() == kHostSlotCapacity); // refused, not clamped: nothing changed
	CHECK(pool.grow(kMaxHostSlotCapacity));
	CHECK(pool.capacity() == 4096);
	CHECK_FALSE(pool.grow(kMaxHostSlotCapacity + 1));
	CHECK(pool.capacity() == 4096);
	CHECK(pool.grow(100)); // smaller: not an error, and no shrink
	CHECK(pool.capacity() == 4096);
	// THE CONSTRUCTOR CLAMPS, so no pool starts past the limit either.
	CHECK(SlotPool(UINT32_MAX).capacity() == 4096);
	CHECK(SlotPool(4097).capacity() == 4096);

	// AND A SNAPSHOT: 4096 restores, 4097 is refused and says why. A slot at the last index proves the
	// accepted one is really 4096 wide.
	PoolSnapshot snap;
	snap.capacity = 4096;
	snap.clock = 1;
	SlotRecord r;
	r.index = 4095;
	r.state = SlotState::Recent;
	r.target = control({1}, "a");
	r.recency = 1;
	snap.slots = {r};
	CHECK(SlotPool::whyInvalid(snap).empty());
	SlotPool fresh;
	REQUIRE(fresh.restore(snap));
	CHECK(fresh.capacity() == 4096);
	CHECK(fresh.slotOf(control({1}, "a")) == 4095);
	CHECK(fresh.snapshot() == snap); // and it saves what it restored: the writer stays inside the reader

	snap.capacity = 4097;
	CHECK(SlotPool::whyInvalid(snap) == "a capacity of 4097, past the most a pool may have (4096)");
	SlotPool other;
	CHECK_FALSE(other.restore(snap));
	CHECK(other.capacity() == kHostSlotCapacity);
}

TEST_CASE("restore refuses a path through kNoInstance, and says so") {
	// kNoInstance NAMES NO DEVICE, so a slot on a path through it can never resolve, anywhere on the path.
	for (const std::vector<DeviceInstanceId>& path :
	     {std::vector<DeviceInstanceId> {kNoInstance}, std::vector<DeviceInstanceId> {kNoInstance, 3},
	      std::vector<DeviceInstanceId> {3, kNoInstance}}) {
		CAPTURE(path.size());
		PoolSnapshot snap;
		snap.clock = 1;
		SlotRecord r;
		r.index = 2;
		r.state = SlotState::Anchored;
		r.target = control(path, "a");
		snap.slots = {r};
		CHECK(SlotPool::whyInvalid(snap) == "slot 2 has a path through no device");
		SlotPool pool;
		CHECK_FALSE(pool.restore(snap));
		CHECK(pool.state(2) == SlotState::Empty);
	}
}

TEST_CASE("restore's boundaries: a recency AT the clock and an index one short of the capacity are the last accepted") {
	PoolSnapshot base;
	base.capacity = kHostSlotCapacity + 32; // above the floor, so the SAVED capacity is what is checked
	base.clock = 10;
	SlotRecord r;
	r.state = SlotState::Recent;
	r.target = control({1}, "a");
	base.slots = {r};

	// RECENCY: equal to the clock is the last touch the clock counted; one past it never happened.
	{
		PoolSnapshot s = base;
		s.slots[0].recency = 10;
		CHECK(SlotPool::whyInvalid(s).empty());
		s.slots[0].recency = 11;
		CHECK(SlotPool::whyInvalid(s) == "slot 0 was used after the pool's clock");
		SlotPool pool;
		CHECK_FALSE(pool.restore(s));
	}
	// INDEX: capacity - 1 is the last slot; the capacity itself is one past it. Checked against the SAVED
	// capacity (floor + 32), and at the floor (kHostSlotCapacity) for a snapshot that says less.
	{
		const uint32_t saved = kHostSlotCapacity + 32;
		const std::string cap = std::to_string(saved);
		PoolSnapshot s = base;
		s.slots[0].index = saved - 1;
		CHECK(SlotPool::whyInvalid(s).empty());
		s.slots[0].index = saved;
		CHECK(SlotPool::whyInvalid(s) == "slot " + cap + " is past the capacity of " + cap);
		SlotPool pool;
		CHECK_FALSE(pool.restore(s));
		s.capacity = 10; // read as the floor
		s.slots[0].index = kHostSlotCapacity - 1;
		CHECK(SlotPool::whyInvalid(s).empty());
		s.slots[0].index = kHostSlotCapacity;
		const std::string floor = std::to_string(kHostSlotCapacity);
		CHECK(SlotPool::whyInvalid(s) == "slot " + floor + " is past the capacity of " + floor);
	}
}

// ---------------------------------------------------------------------------------------------------
// THE HOST MARK: an id the host has written or modulated keeps the host's lanes whatever
// the plugin does, so it is reused only after every clean slot, and its taking is announced.
// ---------------------------------------------------------------------------------------------------

TEST_CASE("a released slot the host touched is reused only after every clean slot") {
	SlotPool pool;
	// SLOT 0: the host wrote it (marked, anchored), then it was released — Recent, and the OLDEST Recent, so a
	// plain LRU would take it first. SLOT 1: marked, then its device went — Empty, and the lowest Empty, so a
	// plain Empty-first would take it first.
	REQUIRE(pool.touch(control({1}, "a")).slot == 0);
	REQUIRE(pool.touch(control({1}, "b")).slot == 1);
	CHECK(pool.markHost(0));
	CHECK(pool.markHost(1));
	CHECK_FALSE(pool.markHost(1)); // once: the transition is what reports
	REQUIRE(pool.anchor(0));
	REQUIRE(pool.release(0));
	REQUIRE(pool.free(1));
	// THE MARK OUTLIVES release, free and unpin: it is the id's, not the mapping's.
	CHECK(pool.state(0) == SlotState::Recent);
	CHECK(pool.hostMarked(0));
	CHECK(pool.state(1) == SlotState::Empty);
	CHECK(pool.hostMarked(1));

	// EVERY CLEAN EMPTY FIRST: slots 2..cap-1, none announced.
	for (int i = 0; i + 2 < static_cast<int>(kHostSlotCapacity); ++i) {
		const Allocation a = pool.touch(filler(i));
		REQUIRE(a.slot == static_cast<SlotIndex>(2 + i));
		REQUIRE_FALSE(a.hostMarked);
	}
	// THEN THE CLEAN RECENT BY LRU — filler 0 at slot 2 — before either marked slot, though slot 0 is older
	// and slot 1 is Empty.
	const Allocation clean = pool.touch(control({2}, "x"));
	CHECK(clean.slot == 2);
	CHECK(clean.took == Allocation::Took::Recent);
	CHECK(clean.announce);
	CHECK_FALSE(clean.hostMarked);

	// PIN EVERY CLEAN SLOT: now only the marked ones are left, the Empty before the Recent.
	for (SlotIndex i = 2; i < kHostSlotCapacity; ++i)
		REQUIRE(pool.pin(pool.target(i)).slot == i);
	const Allocation markedEmpty = pool.touch(control({2}, "y"));
	CHECK(markedEmpty.slot == 1);
	CHECK(markedEmpty.took == Allocation::Took::Empty);
	CHECK(markedEmpty.hostMarked);
	CHECK(markedEmpty.announce);
	CHECK(pool.hostMarked(1)); // taken, still marked: the lanes are still there
	const Allocation markedRecent = pool.touch(control({2}, "z"));
	CHECK(markedRecent.slot == 0);
	CHECK(markedRecent.took == Allocation::Took::Recent);
	CHECK(markedRecent.hostMarked);
	REQUIRE(markedRecent.previous.has_value());
	CHECK(markedRecent.previous->sameControl(control({1}, "a")));
	// AND A FURTHER TOUCH recycles the older of the two marked Recents ("y", slot 1), still announced as marked.
	const Allocation again = pool.touch(control({2}, "w"));
	CHECK(again.slot == 1);
	CHECK(again.hostMarked);
}

TEST_CASE("defaults never fill a host-marked slot") {
	SlotPool pool;
	REQUIRE(pool.markHost(0));
	const std::vector<Candidate> cands{{control({1}, "a0"), 0, 0, 0}};
	REQUIRE(pool.fillDefaults(cands) == 1);
	CHECK(pool.state(0) == SlotState::Empty);
	CHECK(pool.slotOf(control({1}, "a0")) == 1);
}

TEST_CASE("the host mark survives snapshot and restore, and a bad one is refused") {
	SlotPool pool;
	REQUIRE(pool.touch(control({1}, "a")).slot == 0);
	REQUIRE(pool.markHost(0));
	REQUIRE(pool.markHost(7)); // an Empty slot's mark is saved too
	const PoolSnapshot snap = pool.snapshot();
	CHECK(snap.hostMarked == std::vector<SlotIndex>{0, 7});
	SlotPool fresh;
	REQUIRE(fresh.restore(snap));
	CHECK(fresh.hostMarked(0));
	CHECK(fresh.hostMarked(7));
	CHECK_FALSE(fresh.hostMarked(1));
	CHECK(fresh.snapshot() == snap);
	// A restore REPLACES the marks: one without them clears them.
	REQUIRE(fresh.restore(PoolSnapshot {}));
	CHECK_FALSE(fresh.hostMarked(0));

	PoolSnapshot bad = snap;
	bad.hostMarked = {7, 0};
	CHECK(SlotPool::whyInvalid(bad) == "host-marked slot 0 is out of order or listed twice");
	bad.hostMarked = {kHostSlotCapacity};
	CHECK(SlotPool::whyInvalid(bad)
	      == "host-marked slot " + std::to_string(kHostSlotCapacity) + " is past the capacity of "
	             + std::to_string(kHostSlotCapacity));
}
