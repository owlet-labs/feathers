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
#include "test_heap.h"
#include "doctest.h"
#include "portable_aligned_alloc.h"

#include "hostslots/slot_bus.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <thread>
#include <vector>

// THE HOST SLOTS' LOCK-FREE ROADS (host slots, task T2): the bus the host
// writes into on the audio thread, and the outbox the editor writes into for the host. The outbox
// cases drive `drain` with a fake host queue that records what it was offered and what it took, so
// every claim is about the EVENTS the host ends up holding, not about the debt table's insides.
//
// KNOWN-BADS RUN AGAINST THESE CASES (each through `scripts/run-cases.sh --each`, each restored after):
// - RETRY BEGIN (the plan's): a refused Begin owed and paid with the debts. Caught by `a dropped Begin
//   is not retried; an End is` — REQUIRE(host.got.size() == 2) reads 3 == 2.
// - A TAKE THAT IS NOT ONE EXCHANGE (load, yield, store 0): caught by the stress case, lostRounds 1.
// - THE LATEST VALUE RECORDED ONLY WHEN IT FITS THE RING: caught by the debt case, paying 0.04 / 0.54
//   against 0.2 / 0.7.
// - NO BEGIN-BEHIND-A-PARKED-END CHECK in `push`: caught by `an End parked after the fold does not
//   close the next gesture`.

namespace feathers::hostslots {

/// The seam `SlotOutbox` names as its friend. It does exactly what `push`'s overflow branch does for
/// an End, WITHOUT the ring-full test in front of it: the state left when the producer read a stale
/// head, found the ring full, and set the bit only after the consumer had published a new head and
/// folded. One thread cannot reach that interleaving through the public API, which is the point.
struct SlotOutboxTestAccess {
	template <size_t C, size_t R>
	static void parkEndAfterFold(SlotOutbox<C, R>& box, size_t slot) {
		box.overflowEnd_[slot / 64].fetch_or(uint64_t {1} << (slot % 64), std::memory_order_release);
	}
};

} // namespace feathers::hostslots

namespace {

using namespace feathers::hostslots;

//------------------------------------------------------------------------------------//
// THE ALLOCATION PROBE — the idiom from tests/test_backplane_graph.cpp, armed only      //
// around the calls under claim, because doctest itself allocates constantly.            //
//------------------------------------------------------------------------------------//

std::atomic<bool> gArmed {false};
std::atomic<int> gAllocations {0};

[[gnu::noinline]] void* countedAlloc(size_t bytes, size_t align) {
	if (gArmed.load(std::memory_order_relaxed)) {
		gAllocations.fetch_add(1, std::memory_order_relaxed);
	}
	// An aligned request may ask for less than a pointer: see portable_aligned_alloc.h.
	void* p = feathers::test::portableAlignedAlloc(bytes, align);
	if (p == nullptr) {
		throw std::bad_alloc {};
	}
	return p;
}

} // namespace

void* operator new(size_t bytes) { return countedAlloc(bytes, alignof(std::max_align_t)); }
void* operator new[](size_t bytes) { return countedAlloc(bytes, alignof(std::max_align_t)); }
void* operator new(size_t bytes, std::align_val_t align) { return countedAlloc(bytes, static_cast<size_t>(align)); }
void* operator new[](size_t bytes, std::align_val_t align) { return countedAlloc(bytes, static_cast<size_t>(align)); }
void operator delete(void* p) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, size_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, size_t) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, std::align_val_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, std::align_val_t) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { feathers::test::heapFree(p); }
#include "nothrow_new_forwarding.h"

namespace {

/// A fake host output queue: refuses while `full`, and records every event it accepts.
struct FakeHostQueue {
	struct Got {
		SlotEventKind kind;
		uint16_t slot;
		float value01;
	};
	bool full = false;
	/// Refuse exactly the next N offers, then fall back to `full`.
	int refuseNext = 0;
	int offered = 0;
	std::vector<Got> got;

	FakeHostQueue() { got.reserve(1024); }

	bool operator()(SlotEventKind kind, uint16_t slot, float value01) {
		++offered;
		if (refuseNext > 0) {
			--refuseNext;
			return false;
		}
		if (full) {
			return false;
		}
		got.push_back(Got {kind, slot, value01});
		return true;
	}

	int count(SlotEventKind kind, uint16_t slot) const {
		int n = 0;
		for (const Got& g : got) {
			n += (g.kind == kind && g.slot == slot) ? 1 : 0;
		}
		return n;
	}
};

/// The xorshift the round masks come from; both threads run their own copy from one seed, so the
/// reader knows each round's mask without being told it.
uint64_t nextMask(uint64_t& s) {
	s ^= s << 13;
	s ^= s >> 7;
	s ^= s << 17;
	return s;
}

/// A writer sets a pseudo-random set of slots per round, each ONCE, with the round number as the
/// value, while a reader takes concurrently. The writer does not start round r+1 until the reader has
/// taken every bit of round r, so each bit set is owed exactly one take: a lost bit hangs the round
/// (caught by the deadline), a duplicate shows as a bit taken twice, and a stray as a bit outside the
/// round's mask. The reader also reads the value AFTER taking the bit, which must already be the
/// round's — the release/acquire pairing on the bitset is what promises that.
template <size_t Capacity>
void stressSetTake(int rounds) {
	using Bus = SlotValueBus<Capacity>;
	using Bits = typename Bus::Bits;
	auto bus = std::make_unique<Bus>();

	std::atomic<int> roundTaken {-1};
	std::atomic<bool> abort {false};
	int duplicates = 0, strays = 0, staleValues = 0, lostRounds = 0;
	std::vector<int> expectedGen(Capacity, 0);

	auto maskFor = [](uint64_t& s) {
		Bits m;
		for (auto& w : m.words) {
			w = nextMask(s);
		}
		if constexpr (Capacity % 64 != 0) {
			m.words.back() &= (uint64_t {1} << (Capacity % 64)) - 1;
		}
		return m;
	};

	std::thread writer([&] {
		uint64_t s = 0x9e3779b97f4a7c15ull;
		for (int r = 0; r < rounds && !abort.load(); ++r) {
			const Bits m = maskFor(s);
			m.forEach([&](size_t slot) { bus->onHostValue(slot, static_cast<float>(r)); });
			while (roundTaken.load(std::memory_order_acquire) < r && !abort.load()) {
				std::this_thread::yield();
			}
		}
	});

	uint64_t s = 0x9e3779b97f4a7c15ull;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	for (int r = 0; r < rounds; ++r) {
		const Bits want = maskFor(s);
		want.forEach([&](size_t slot) { ++expectedGen[slot]; });
		Bits have;
		for (;;) {
			bool done = true;
			for (size_t w = 0; w < Bits::kWords; ++w) {
				done = done && have.words[w] == want.words[w];
			}
			if (done) {
				break;
			}
			if (std::chrono::steady_clock::now() > deadline) {
				++lostRounds;
				abort.store(true);
				break;
			}
			const Bits took = bus->takeHostWrote();
			took.forEach([&](size_t slot) {
				if (have.test(slot)) {
					++duplicates;
				}
				if (!want.test(slot)) {
					++strays;
				}
				if (bus->valueOf(slot) != static_cast<float>(r)) {
					++staleValues;
				}
				have.words[slot / 64] |= uint64_t {1} << (slot % 64);
			});
		}
		if (abort.load()) {
			break;
		}
		roundTaken.store(r, std::memory_order_release);
	}
	writer.join();

	CHECK(lostRounds == 0);
	CHECK(duplicates == 0);
	CHECK(strays == 0);
	CHECK(staleValues == 0);
	// Nothing left behind once the writer has stopped.
	CHECK(bus->takeHostWrote().count() == 0);
	// And every write moved its slot's generation exactly once.
	int genMismatch = 0;
	for (size_t slot = 0; slot < Capacity; ++slot) {
		genMismatch += bus->genOf(slot) == static_cast<uint32_t>(expectedGen[slot]) ? 0 : 1;
	}
	CHECK(genMismatch == 0);
	// Not vacuous: the masks did set bits in every word.
	CHECK(expectedGen[0] + expectedGen[Capacity - 1] > 0);
}

} // namespace

TEST_CASE("hostWrote bits survive concurrent set/take") {
	SUBCASE("at the shipping capacity") { stressSetTake<kSlotBusCapacity>(20000); }
	// The capacity is growable by recompile; a pool past 64 spans words, and the last one partial.
	SUBCASE("at a capacity past one word") { stressSetTake<130>(20000); }

	SUBCASE("recTouched is its own set, taken apart from hostWrote") {
		SlotValueBus<> bus;
		bus.onRecTouched(7);
		bus.onHostValue(9, 0.25f);
		const auto rec = bus.takeRecTouched();
		const auto host = bus.takeHostWrote();
		CHECK(rec.count() == 1);
		CHECK(rec.test(7));
		CHECK(host.count() == 1);
		CHECK(host.test(9));
		CHECK(bus.takeRecTouched().count() == 0);
	}

	SUBCASE("seed moves the generation and anchors nothing") {
		SlotValueBus<> bus;
		const uint32_t g = bus.seed(4, 12.5f);
		CHECK(g == 1);
		CHECK(bus.genOf(4) == 1);
		CHECK(bus.valueOf(4) == 12.5f);
		CHECK(bus.takeHostWrote().count() == 0);
		bus.onHostMod(4, 0.0f);
		CHECK(bus.takeHostWrote().count() == 0); // a modulator at rest anchors nothing
		bus.onHostMod(4, -0.25f);
		CHECK(bus.modOf(4) == -0.25f);
		CHECK(bus.genOf(4) == 1); // modulation is not a write
		// BUT A NON-ZERO AMOUNT ANCHORS, AS A HOST WRITE DOES: the bit is set.
		const auto bits = bus.takeHostWrote();
		CHECK(bits.count() == 1);
		CHECK(bits.test(4));
		// AND A WRITE THAT REACHED NO CONTROL marks, on its own bit, and anchors nothing.
		bus.onHostTouched(9);
		CHECK(bus.takeHostWrote().count() == 0);
		const auto marked = bus.takeHostMarked();
		CHECK(marked.count() == 1);
		CHECK(marked.test(9));
		CHECK(bus.takeHostMarked().count() == 0);
	}
}

TEST_CASE("a full out-queue owes one value per slot, re-read fresh") {
	// An eight-event ring, so a long drag outruns it: the first eight moves enter the ring and the
	// host refuses them; the other thirty-two never reach the ring at all. The value each slot HAS
	// at the end therefore was never offered to the host, which is what makes "fresh" testable: a
	// debt that remembered the value it failed with would pay 0.04 / 0.54, never the latest.
	SlotOutbox<kSlotBusCapacity, 8> box;
	FakeHostQueue host;
	auto a = [](int i) { return 0.01f * static_cast<float>(i); };
	auto b = [](int i) { return 0.5f + 0.01f * static_cast<float>(i); };

	host.full = true;
	int entered = 0;
	for (int i = 1; i <= 20; ++i) {
		entered += box.value(3, a(i)) ? 1 : 0;
		entered += box.value(5, b(i)) ? 1 : 0;
	}
	REQUIRE(entered == 8);
	box.drain(host);
	CHECK(host.got.empty());
	// Eight ring events offered, then ONE debt payment per slot — not twenty.
	CHECK(host.offered == 8 + 2);
	CHECK(box.owesValue(3));
	CHECK(box.owesValue(5));
	CHECK_FALSE(box.owesValue(4));

	// Still full: the debt is offered again, once per slot per drain.
	host.offered = 0;
	box.drain(host);
	CHECK(host.offered == 2);

	// The host opens up. Each slot gets exactly one value, and it is the one it has NOW.
	host.full = false;
	box.drain(host);
	REQUIRE(host.got.size() == 2);
	CHECK(host.count(SlotEventKind::Value, 3) == 1);
	CHECK(host.count(SlotEventKind::Value, 5) == 1);
	for (const auto& g : host.got) {
		CHECK(g.kind == SlotEventKind::Value);
		CHECK(g.value01 == (g.slot == 3 ? a(20) : b(20)));
	}
	CHECK_FALSE(box.owesValue(3));
	CHECK_FALSE(box.owesValue(5));

	// Paid means paid: an idle drain sends nothing.
	host.got.clear();
	box.drain(host);
	CHECK(host.got.empty());

	SUBCASE("a value the host takes clears an older debt for its slot") {
		host.full = true;
		box.value(8, 0.1f);
		box.drain(host);
		REQUIRE(box.owesValue(8));
		host.full = false;
		box.value(8, 0.9f);
		box.drain(host);
		CHECK(host.count(SlotEventKind::Value, 8) == 1);
		CHECK(host.got.back().value01 == 0.9f);
		CHECK_FALSE(box.owesValue(8));
	}
}

TEST_CASE("a dropped Begin is not retried; an End is") {
	SlotOutbox<> box;
	FakeHostQueue host;

	// One whole gesture on slot 2 while the host is full.
	host.full = true;
	box.begin(2);
	box.value(2, 0.4f);
	box.end(2);
	box.drain(host);
	CHECK(host.got.empty());
	CHECK(box.owesEnd(2));

	host.full = false;
	box.drain(host);
	// The value and the end reach the host, in that order; the begin never does.
	REQUIRE(host.got.size() == 2);
	CHECK(host.got[0].kind == SlotEventKind::Value);
	CHECK(host.got[0].value01 == 0.4f);
	CHECK(host.got[1].kind == SlotEventKind::End);
	CHECK(host.got[1].slot == 2);
	CHECK(host.count(SlotEventKind::Begin, 2) == 0);
	CHECK_FALSE(box.owesEnd(2));

	// And no later drain resurrects it.
	host.got.clear();
	box.drain(host);
	box.drain(host);
	CHECK(host.got.empty());

	SUBCASE("an End owed from the last gesture goes BEFORE the next Begin") {
		host.full = true;
		box.begin(6);
		box.end(6);
		box.drain(host);
		REQUIRE(box.owesEnd(6));
		host.full = false;
		box.begin(6);
		box.value(6, 0.3f);
		box.drain(host);
		REQUIRE(host.got.size() == 3);
		CHECK(host.got[0].kind == SlotEventKind::End);
		CHECK(host.got[1].kind == SlotEventKind::Begin);
		CHECK(host.got[2].kind == SlotEventKind::Value);
		CHECK_FALSE(box.owesEnd(6)); // the new gesture is still open, as it should be
	}

	SUBCASE("a full RING drops a Begin and still delivers the End") {
		SlotOutbox<kSlotBusCapacity, 2> small;
		FakeHostQueue h;
		CHECK(small.value(1, 0.1f));
		CHECK(small.value(1, 0.2f));
		CHECK_FALSE(small.begin(1));
		CHECK_FALSE(small.end(1));
		small.drain(h);
		CHECK(h.count(SlotEventKind::Begin, 1) == 0);
		CHECK(h.count(SlotEventKind::End, 1) == 1);
		CHECK(h.got.back().kind == SlotEventKind::End);
	}
}

TEST_CASE("an End parked after the fold does not close the next gesture") {
	// The race, driven step by step from one thread. Gesture 1 opens and moves; its End overflows the
	// ring but lands as a bit only after a drain has already folded; the hand grabs the control again
	// at once (gesture 2). Unfixed, the next drain sends Begin(g2) and THEN pays g1's End — so the host
	// closes g2 at its first event and every later move of the drag lands outside any grouping.
	SlotOutbox<kSlotBusCapacity, 4> box;
	FakeHostQueue host;

	box.begin(1);
	box.value(1, 0.2f);
	box.drain(host);
	REQUIRE(host.got.size() == 2);
	host.got.clear();

	SlotOutboxTestAccess::parkEndAfterFold(box, 1); // End(g1), late
	const bool g2Began = box.begin(1);
	box.drain(host);
	box.value(1, 0.5f); // g2's move
	box.end(1);         // g2's own End
	box.drain(host);
	box.drain(host);

	size_t moved = host.got.size();
	for (size_t i = 0; i < host.got.size(); ++i) {
		if (host.got[i].kind == SlotEventKind::Value && host.got[i].value01 == 0.5f) {
			moved = i;
			break;
		}
	}
	REQUIRE(moved < host.got.size());
	// No End between any Begin and g2's move: nothing closed g2 early.
	int closedEarly = 0;
	for (size_t i = 0; i < moved; ++i) {
		if (host.got[i].kind != SlotEventKind::Begin) {
			continue;
		}
		for (size_t j = i + 1; j < moved; ++j) {
			closedEarly += host.got[j].kind == SlotEventKind::End ? 1 : 0;
		}
	}
	CHECK(closedEarly == 0);
	// g1's End still reached the host, before g2's move; and g2's own End after it, exactly once.
	int endsBefore = 0, endsAfter = 0;
	for (size_t i = 0; i < host.got.size(); ++i) {
		if (host.got[i].kind == SlotEventKind::End) {
			(i < moved ? endsBefore : endsAfter) += 1;
		}
	}
	CHECK(endsBefore == 1);
	CHECK(endsAfter == 1);
	// How the fix gets there: the Begin waiting behind a parked End is dropped.
	CHECK_FALSE(g2Began);
	CHECK_FALSE(box.owesEnd(1));
	CHECK_FALSE(box.owesValue(1));
}

TEST_CASE("onHostValue allocates nothing") {
	auto bus = std::make_unique<SlotValueBus<>>();
	auto box = std::make_unique<SlotOutbox<>>();
	FakeHostQueue host; // its vector is reserved up front
	host.full = true;
	box->value(1, 0.5f);
	box->end(1);

	gAllocations.store(0);
	gArmed.store(true);
	// Everything the audio thread calls, many times over.
	uint32_t gens = 0;
	for (int i = 0; i < 1000; ++i) {
		bus->onHostValue(static_cast<size_t>(i) % kSlotBusCapacity, static_cast<float>(i));
		bus->onHostMod(static_cast<size_t>(i) % kSlotBusCapacity, 0.001f * static_cast<float>(i));
		bus->onRecTouched(static_cast<size_t>(i) % kSlotBusCapacity);
		gens += bus->genOf(static_cast<size_t>(i) % kSlotBusCapacity);
	}
	box->drain(host);
	host.full = false;
	box->drain(host);
	gArmed.store(false);
	CHECK(gAllocations.load() == 0);

	// Not vacuous: the calls did their work, and the probe does count when armed.
	CHECK(bus->takeHostWrote().count() == kSlotBusCapacity);
	// 1000 writes round-robin over the capacity: slot 0 gets ceil(1000 / capacity) of them.
	CHECK(bus->genOf(0) == static_cast<uint32_t>((1000 + kSlotBusCapacity - 1) / kSlotBusCapacity));
	CHECK(gens > 0);
	CHECK(host.count(SlotEventKind::End, 1) == 1);
	gArmed.store(true);
	// A volatile POINTER, so the allocation cannot be elided ([expr.new]/10; CLAUDE.md, known-bads).
	int* volatile probe = new int(7);
	gArmed.store(false);
	CHECK(gAllocations.load() == 1);
	delete probe;
}

// ---------------------------------------------------------------------------------------------------
// THE PACKED CELL (T6b review): `{value, gen}` in one lock-free word, so a reader never pairs a value
// with another write's generation, and `seedIf` loses to any write since the caller looked.
// ---------------------------------------------------------------------------------------------------

TEST_CASE("a bus cell is one {value, gen}: load pairs them, and seedIf loses to a write since the look") {
	static_assert(std::atomic<uint64_t>::is_always_lock_free);
	SlotValueBus<> bus;
	using Snap = SlotValueBus<>::Snapshot;
	CHECK(bus.load(2) == Snap {0.0f, 0});

	CHECK(bus.seed(2, 3.5f) == 1u);
	CHECK(bus.load(2) == Snap {3.5f, 1});
	// genOf/valueOf are the two halves of the same word.
	CHECK(bus.genOf(2) == 1u);
	CHECK(bus.valueOf(2) == 3.5f);

	// A SEED AGAINST WHAT IS THERE lands, one generation on.
	const Snap seen = bus.load(2);
	CHECK(bus.seedIf(2, seen, 7.25f) == 2u);
	CHECK(bus.load(2) == Snap {7.25f, 2});

	// AGAINST A STALE LOOK it writes nothing: the host wrote since.
	const Snap stale = bus.load(2);
	bus.onHostValue(2, -1.0f);
	CHECK(bus.seedIf(2, stale, 99.0f) == 0u);
	CHECK(bus.load(2) == Snap {-1.0f, 3});
	// And against a value that matches but a generation that does not — the whole pair is the key.
	CHECK(bus.seedIf(2, Snap {-1.0f, 2}, 99.0f) == 0u);
	CHECK(bus.load(2) == Snap {-1.0f, 3});
	// A SEED NEVER ANCHORS; the host's write did, once.
	CHECK(bus.takeHostWrote().count() == 1);
	// Out of range: nothing, and no crash.
	CHECK(bus.seedIf(kSlotBusCapacity, Snap {}, 1.0f) == 0u);
	CHECK(bus.load(kSlotBusCapacity) == Snap {});
	// The neighbour is untouched.
	CHECK(bus.load(3) == Snap {0.0f, 0});
}

TEST_CASE("seed and onHostValue never tear") {
	// THE HOST writes ODD values and the MAIN THREAD seeds EVEN ones, into one slot, at once; a third thread
	// samples `load`. The seeder logs which generation each of its seeds got. Afterwards every sample must
	// pair a generation with the value written under it: a seeded generation with that seed's (even) value,
	// any other generation with an odd one. Two atomics, as the cell was, let a seed return generation g while
	// the host's value sat under it — exactly the pairing checked here and at each seed.
	//
	// LIVENESS IS SIGNALLED, NOT HOPED FOR. Each writer makes at least kWrites free-running writes (the
	// contention the tear check needs), then KEEPS writing until the reader says it has recorded enough
	// samples AND at least one of each writer's values — or a generous cap/deadline runs out, which the
	// checks below then report by naming the starved writer. At load ~30 the free-running run alone left
	// one writer unsampled ~1 time in 5 (8/40 measured 2026-09-26), a red about the scheduler, not the bus.
	constexpr size_t kSlot = 5;
	constexpr uint32_t kWrites = 200000;      // each side, free-running, always
	constexpr uint32_t kMaxWrites = 2000000;  // each side, at most; every value below 2^24, so exact as a float
	constexpr size_t kMinSamples = 2000;
	constexpr size_t kSampleCap = 400000;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	auto bus = std::make_unique<SlotValueBus<>>();
	std::vector<float> seededAt(2 * kMaxWrites + 2, -1.0f); // by generation; -1 = not a seed
	std::vector<SlotValueBus<>::Snapshot> samples;
	samples.reserve(kSampleCap);
	std::atomic<bool> go {false};
	std::atomic<bool> readerSatisfied {false};
	std::atomic<int> writersDone {0};
	int seedTears = 0;
	uint32_t hostWrites = 0;
	uint32_t seedWrites = 0;
	// Past the free-running minimum, a writer goes on only while the reader still lacks something.
	auto keepWriting = [&](uint32_t k) {
		if (k < kWrites) {
			return true;
		}
		if (readerSatisfied.load() || k >= kMaxWrites) {
			return false;
		}
		return (k % 1024 != 0) || std::chrono::steady_clock::now() < deadline;
	};

	std::thread host([&] {
		while (!go.load()) {
			std::this_thread::yield();
		}
		uint32_t k = 0;
		for (; keepWriting(k); ++k) {
			bus->onHostValue(kSlot, static_cast<float>(2 * k + 1));
		}
		hostWrites = k;
		writersDone.fetch_add(1);
	});
	std::thread reader([&] {
		while (!go.load()) {
			std::this_thread::yield();
		}
		// By VALUE parity (host odd, seed even); the generation-keyed classification below is the verdict,
		// and it agrees with parity exactly when nothing tore.
		bool sawOdd = false;
		bool sawEven = false;
		while (writersDone.load() < 2) {
			const SlotValueBus<>::Snapshot s = bus->load(kSlot);
			const bool odd = static_cast<uint32_t>(s.value) % 2u == 1u;
			const bool newKind = s.gen != 0 && (odd ? !sawOdd : !sawEven);
			// The last two places are kept for a writer not yet seen, so a full buffer cannot hide one.
			if (samples.size() + 2 < kSampleCap || (newKind && samples.size() < kSampleCap)) {
				samples.push_back(s);
				if (s.gen != 0) {
					(odd ? sawOdd : sawEven) = true;
				}
			}
			if (!readerSatisfied.load(std::memory_order_relaxed) && sawOdd && sawEven &&
			    samples.size() >= kMinSamples) {
				readerSatisfied.store(true);
			}
		}
	});
	go.store(true);
	uint32_t k = 0;
	for (; keepWriting(k); ++k) {
		const float v = static_cast<float>(2 * k + 2);
		const uint32_t g = bus->seed(kSlot, v);
		if (g < seededAt.size()) {
			seededAt[g] = v;
		}
		// THE PROPERTY THE RECORD MIRROR RELIES ON: still at my generation means still my value.
		const SlotValueBus<>::Snapshot now = bus->load(kSlot);
		if (now.gen == g && now.value != v) {
			++seedTears;
		}
	}
	seedWrites = k;
	writersDone.fetch_add(1);
	host.join();
	reader.join();

	int tears = 0;
	int seededSeen = 0;
	int hostSeen = 0;
	for (const SlotValueBus<>::Snapshot& s : samples) {
		if (s.gen == 0) {
			continue;
		}
		const bool seededGen = s.gen < seededAt.size() && seededAt[s.gen] >= 0.0f;
		const bool odd = static_cast<uint32_t>(s.value) % 2u == 1u;
		if (seededGen) {
			tears += s.value == seededAt[s.gen] ? 0 : 1;
			++seededSeen;
		}
		else {
			tears += odd ? 0 : 1;
			++hostSeen;
		}
	}
	CHECK(seedTears == 0);
	CHECK(tears == 0);
	// EVERY WRITE MOVED THE GENERATION EXACTLY ONCE — no bump lost to the other writer.
	CHECK(hostWrites >= kWrites);
	CHECK(seedWrites >= kWrites);
	CHECK(bus->genOf(kSlot) == hostWrites + seedWrites);
	// Not vacuous: the reader saw both writers' values while they raced. With the hand-off above these go
	// red only if a writer was starved for the whole cap/deadline — and the message says which.
	CHECK_MESSAGE(samples.size() > 1000, "reader recorded only ", samples.size(), " samples");
	CHECK_MESSAGE(seededSeen > 0, "SEED writer starved: reader never sampled a seed in ", seedWrites,
	              " seeds and ", hostWrites, " host writes");
	CHECK_MESSAGE(hostSeen > 0, "HOST writer starved: reader never sampled a host value in ", hostWrites,
	              " host writes and ", seedWrites, " seeds");
}
