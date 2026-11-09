/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

// **A HAND'S BLOCK: THE ROAD ANY DEVICE CAN USE FOR USER-EDITED STATE TOO LARGE FOR THE ROW** (`StateBlockDesc::handEdited`,
// `StateOps::apply`, `DeviceHost::setBlock`, `AudioOps::pullHandBlocks`, `device::HandBlock`). Independent of Spectral, which
// is its first user: the fixture below is a device whose whole state is one eight-float block the hand writes.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "test_heap.h"
#include "doctest.h"
#include "plugin/device_registry.h"
#include "rack/rack_bridge.h"
#include "rack/rack_engine.h"
#include "rack/rack_state.h"
#include "persist_rig.h"
#include "portable_aligned_alloc.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <new>
#include <string>
#include <thread>
#include <vector>

using namespace feathers;

//------------------------------------------------------------------------------------------------//
// THE INSTRUMENT — a counter on every global allocation, armed only where a claim is made.          //
//------------------------------------------------------------------------------------------------//
namespace {
struct Counter {
	static inline std::atomic<bool> armed { false };
	static inline std::atomic<size_t> allocations { 0 };
};
} // namespace
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
void* operator new(size_t size) {
	if (Counter::armed.load(std::memory_order_relaxed)) {
		Counter::allocations.fetch_add(1, std::memory_order_relaxed);
	}
	void* p = feathers::test::heapAlloc(size == 0 ? 1 : size);
	if (p == nullptr) {
		throw std::bad_alloc();
	}
	return p;
}
void* operator new[](size_t size) { return ::operator new(size); }
void* operator new(size_t size, std::align_val_t align) {
	if (Counter::armed.load(std::memory_order_relaxed)) {
		Counter::allocations.fetch_add(1, std::memory_order_relaxed);
	}
	void* p = feathers::test::portableAlignedAlloc(size, static_cast<size_t>(align));
	if (p == nullptr) {
		throw std::bad_alloc();
	}
	return p;
}
void* operator new[](size_t size, std::align_val_t align) { return ::operator new(size, align); }
void operator delete(void* p) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, size_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, size_t) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, std::align_val_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, std::align_val_t) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { feathers::test::heapFree(p); }
#include "nothrow_new_forwarding.h"
#pragma GCC diagnostic pop

namespace {

constexpr size_t kN = 8;

/// THE FIXTURE DEVICE: the hand's eight floats, pulled each block. It counts the blocks it saw and the ones that were MIXED
/// (not all one value) — the stress case's claim is that the second never moves.
struct Holder {
	device::HandBlock<kN> cell;
	device::HandBlockCopy<kN> copy;
	std::array<float, kN> scratch {};
	std::atomic<uint32_t> blocks { 0 };
	std::atomic<uint32_t> mixed { 0 };
	std::atomic<uint32_t> pulls { 0 };
	bool checkWhole = false;
};
Holder& holder(void* s) { return *static_cast<Holder*>(s); }
const Holder& holder(const void* s) { return *static_cast<const Holder*>(s); }

uint64_t readHolder(const void* s, size_t index, float* out, size_t capacity) {
	return index == 0 ? holder(s).cell.read(out, capacity) : 0;
}
bool restoreHolder(void* s, size_t index, const float* in, size_t n) {
	if (index != 0 || n != kN) {
		return false;
	}
	holder(s).cell.write(in, n);
	(void)holder(s).copy.pull(holder(s).cell, holder(s).scratch);
	return true;
}
bool applyHolder(void* s, size_t index, const float* in, size_t n) {
	if (index != 0 || n != kN) {
		return false;
	}
	holder(s).cell.write(in, n);
	return true;
}

const device::AudioOps kHolderAudio {
	.stateBytes = sizeof(Holder),
	.stateAlign = alignof(Holder),
	.construct = [](void* s) { new (s) Holder(); },
	.destruct = [](void* s) { holder(s).~Holder(); },
	.prepare = [](void*, double, int) {},
	.reset = [](void*) {},
	.setFields = [](void*, const float*, size_t) {},
	.processStereo = [](void* s, float* left, float*, int frames) {
		// WHAT THE DEVICE HEARS: the first float of its pulled copy, written into the signal.
		for (int i = 0; i < frames; ++i) {
			left[i] = holder(s).copy.values[0];
		}
		holder(s).blocks.fetch_add(1, std::memory_order_relaxed);
	},
	.processMono = [](void*, float*, int) {},
	.pullHandBlocks = [](void* s, const void* source) {
		Holder& self = holder(s);
		self.pulls.fetch_add(1, std::memory_order_relaxed);
		(void)self.copy.pull(holder(source).cell, self.scratch);
		if (self.checkWhole) {
			for (const float v : self.copy.values) {
				if (v != self.copy.values[0]) {
					self.mixed.fetch_add(1, std::memory_order_relaxed);
					break;
				}
			}
		}
	},
};
constexpr device::StateBlockDesc kBlocks[] = { { .key = "hand", .floats = kN, .handEdited = true } };
constexpr device::StateOps kState { .blocks = kBlocks, .read = &readHolder, .restore = &restoreHolder, .apply = &applyHolder };
constexpr device::Descriptor kHolder {
	.traits = { .tag = "THND", .name = "Hand holder", .hasCard = false, .hasExpanded = false },
	.params = {},
	.outputs = {},
	.audio = &kHolderAudio,
	.state = &kState,
	.category = device::Category::FilterEq,
};
const device::Descriptor* findWithHolder(void*, const char* tag) {
	return device::sameTag(tag, kHolder.traits.tag) ? &kHolder : deviceForTag(tag);
}
device::Catalogue catalogue() { return device::Catalogue { nullptr, &findWithHolder }; }

std::array<float, kN> pattern(float seed) {
	std::array<float, kN> a {};
	for (size_t i = 0; i < kN; ++i) {
		a[i] = seed + 0.125f * static_cast<float>(i) - 3.0f * std::sin(seed + static_cast<float>(i));
	}
	return a;
}

std::array<float, kN> blockOf(persist::Rig& rig) {
	std::array<float, kN> out {};
	(void)rig.bridge->blockAt(rig.id, 0, out); // nothing written yet: flat, as an editor draws it
	return out;
}

} // namespace

TEST_CASE("hand blocks: the fixture's keys pass the registry's key rule") {
	CHECK(deviceKeysValid(kHolder));
	CHECK(retiredKeysUnused(kHolder));
}

TEST_CASE("hand blocks: a block the hand set is saved and loaded into a FRESH rack, value by value, and the audio thread hears it") {
	persist::Rig run { catalogue() };
	run.seat(kHolder.traits.tag);
	run.block();
	const std::array<float, kN> drawn = pattern(1.0f);
	REQUIRE(run.bridge->setBlock(run.id, 0, drawn));
	run.block(2); // the audio thread pulls it
	CHECK(run.as<Holder>().copy.values == drawn);
	CHECK(run.l[0] == drawn[0]);
	CHECK(blockOf(run) == drawn);
	// THE HOST'S STORE HOLDS IT TOO, not only the running instance: a rebuild of the instance (a restart, a latency change) is
	// restored from the store, and a harvest between saves does not refresh a hand's block — so a stale store would undo the hand.
	const std::vector<ContainerBodies::Block>* stored = run.bridge->blockStoreOf(run.id)->blocksOf(run.id);
	REQUIRE(stored != nullptr);
	REQUIRE(stored->size() == 1);
	CHECK((*stored)[0].floats == std::vector<float>(drawn.begin(), drawn.end()));
	const std::string saved = run.save();

	persist::Rig back { catalogue() };
	back.load(saved);
	back.block(2);
	// THE FRESH INSTANCE WAS BUILT HOLDING IT: by value, the instance's own copy and what the editor reads.
	CHECK(back.as<Holder>().copy.values == drawn);
	CHECK(blockOf(back) == drawn);
	CHECK(back.l[0] == drawn[0]);
	// NOT VACUOUS: a patch saved before the hand drew anything does not carry it.
	persist::Rig blank { catalogue() };
	blank.seat(kHolder.traits.tag);
	blank.block();
	persist::Rig blankBack { catalogue() };
	blankBack.load(blank.save());
	blankBack.block(2);
	CHECK(blankBack.as<Holder>().copy.values != drawn);
	// A WRONG-LENGTH OR UNKNOWN WRITE is refused and changes nothing.
	CHECK_FALSE(run.bridge->setBlock(run.id, 0, std::span<const float>(drawn.data(), kN - 1)));
	CHECK_FALSE(run.bridge->setBlock(run.id, 1, drawn));
	CHECK(blockOf(run) == drawn);
}

TEST_CASE("hand blocks: undo and redo put the block back exactly, whole, through the same write") {
	persist::Rig run { catalogue() };
	run.seat(kHolder.traits.tag);
	run.block();
	// THE EDITOR'S UNDO IS ANOTHER WRITE OF THE OLD FLOATS (`ui::CurveEdit`): a stack of (before, after) pairs.
	struct Step {
		std::array<float, kN> before, after;
	};
	std::vector<Step> done, undone;
	const auto edit = [&](const std::array<float, kN>& after) {
		const std::array<float, kN> before = blockOf(run);
		REQUIRE(run.bridge->setBlock(run.id, 0, after));
		done.push_back({ before, after });
		undone.clear();
		run.block();
	};
	const auto undo = [&] {
		Step s = done.back();
		done.pop_back();
		REQUIRE(run.bridge->setBlock(run.id, 0, s.before));
		undone.push_back(s);
		run.block();
	};
	const auto redo = [&] {
		Step s = undone.back();
		undone.pop_back();
		REQUIRE(run.bridge->setBlock(run.id, 0, s.after));
		done.push_back(s);
		run.block();
	};
	const std::array<float, kN> flat {};
	const auto a = pattern(2.0f);
	const auto b = pattern(5.5f);
	edit(a);
	edit(b);
	CHECK(blockOf(run) == b);
	CHECK(run.as<Holder>().copy.values == b);
	undo();
	CHECK(blockOf(run) == a);
	CHECK(run.as<Holder>().copy.values == a);
	undo();
	CHECK(blockOf(run) == flat);
	CHECK(run.as<Holder>().copy.values == flat);
	redo();
	redo();
	CHECK(blockOf(run) == b);
	CHECK(run.as<Holder>().copy.values == b);
	// AND A SAVE AFTER AN UNDO WRITES THE UNDONE VALUE, not the last one drawn.
	undo();
	persist::Rig back { catalogue() };
	back.load(run.save());
	back.block(2);
	CHECK(back.as<Holder>().copy.values == a);
}

TEST_CASE("hand blocks: a write while a render thread reads is all the old value or all the new, never mixed") {
	persist::Rig run { catalogue() };
	run.seat(kHolder.traits.tag);
	run.block();
	const_cast<Holder&>(run.as<Holder>()).checkWhole = true;
	// EVERY WRITE IS EIGHT EQUAL FLOATS: a read that sees two values is a torn one.
	const auto flatOf = [](float v) {
		std::array<float, kN> a {};
		a.fill(v);
		return a;
	};
	std::atomic<bool> stop { false };
	std::atomic<uint32_t> ran { 0 };
	std::array<float, 64> l {}, r {};
	std::thread render([&] {
		while (!stop.load(std::memory_order_acquire)) {
			// THE ENGINE'S OWN BLOCK, no drain: what the audio thread does between the message thread's writes.
			run.engine.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
			ran.fetch_add(1, std::memory_order_relaxed);
		}
	});
	for (int i = 1; i <= 20000; ++i) {
		REQUIRE(run.bridge->setBlock(run.id, 0, flatOf(static_cast<float>(i))));
		if ((i & 63) == 0) {
			std::this_thread::yield();
		}
	}
	// LET THE RENDER THREAD SEE THE LAST WRITES, then stop it.
	const uint32_t at = ran.load();
	while (ran.load() < at + 50) {
		std::this_thread::yield();
	}
	stop.store(true, std::memory_order_release);
	render.join();
	const Holder& h = run.as<Holder>();
	MESSAGE("render blocks " << h.blocks.load() << ", pulls " << h.pulls.load() << ", mixed " << h.mixed.load());
	CHECK(h.pulls.load() > 100);
	CHECK(h.mixed.load() == 0);
	CHECK(h.copy.values == flatOf(20000.0f)); // and it ended on the last write
}

TEST_CASE("hand blocks: the audio thread's pull allocates nothing and a write that lands under it makes it keep its last good copy") {
	persist::Rig run { catalogue() };
	run.seat(kHolder.traits.tag);
	run.block();
	REQUIRE(run.bridge->setBlock(run.id, 0, pattern(3.0f)));
	run.block();
	const std::array<float, kN> held = run.as<Holder>().copy.values;
	// NO ALLOCATION ON THE AUDIO SIDE: processStereo (which pulls) under the counter, with writes landing around it.
	std::array<float, 64> l {}, r {};
	Counter::allocations.store(0);
	Counter::armed.store(true);
	for (int i = 0; i < 200; ++i) {
		run.engine.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
	}
	Counter::armed.store(false);
	CHECK(Counter::allocations.load() == 0);
	CHECK(run.as<Holder>().copy.values == held);
	// A TORN READ (a write in flight: the sequence odd) CHANGES NOTHING and is not retried in place.
	device::HandBlock<kN> cell;
	device::HandBlockCopy<kN> copy;
	std::array<float, kN> scratch {};
	const auto first = pattern(7.0f);
	cell.write(first.data(), kN);
	CHECK(copy.pull(cell, scratch));
	CHECK(copy.values == first);
	CHECK_FALSE(copy.pull(cell, scratch)); // nothing moved: not news
	const auto second = pattern(9.0f);
	cell.write(second.data(), kN);
	CHECK(copy.pull(cell, scratch));
	CHECK(copy.values == second);
	// A SERIAL OF 0 (never written) is nothing to take.
	device::HandBlock<kN> never;
	device::HandBlockCopy<kN> fresh;
	CHECK_FALSE(fresh.pull(never, scratch));
	CHECK(fresh.serial == 0);
}

TEST_CASE("hand blocks: a pull with nothing new copies nothing — the serial it holds is checked before any word is read") {
	// THE IDLE-OVERHEAD PASS (2026-11-05): `HandBlockCopy::pull` asked `tryRead` for the whole block word by word on every
	// call and threw the copy away when the serial was the one it held — a Pattern did that at every render chunk. It now
	// asks `tryReadNewer`, which answers the held serial without touching `out`. Asserted on `out` itself: a sentinel the
	// block never holds survives every pull that has nothing new, and is overwritten by the one that has.
	device::HandBlock<kN> cell;
	device::HandBlockCopy<kN> copy;
	std::array<float, kN> scratch {};
	scratch.fill(-123.0f);
	// NEVER WRITTEN: serial 0 is the one a fresh copy holds, so nothing is read.
	CHECK_FALSE(copy.pull(cell, scratch));
	CHECK(scratch[0] == -123.0f);
	CHECK(cell.tryReadNewer(scratch.data(), kN, 0) == 0);
	CHECK(scratch[0] == -123.0f);
	const auto first = pattern(7.0f);
	cell.write(first.data(), kN);
	CHECK(copy.pull(cell, scratch));
	CHECK(copy.values == first);
	CHECK(scratch == first); // NOT VACUOUS: a pull with something new does read through `scratch`
	scratch.fill(-123.0f);
	for (int i = 0; i < 4; ++i) {
		CHECK_FALSE(copy.pull(cell, scratch));
	}
	CHECK(scratch[0] == -123.0f); // nothing new: not one word read
	CHECK(copy.values == first);
	// A SERIAL THE CALLER DOES NOT HOLD is `tryRead` exactly: the block and its serial.
	std::array<float, kN> out {};
	CHECK(cell.tryReadNewer(out.data(), kN, copy.serial + 1) == copy.serial);
	CHECK(out == first);
	const auto second = pattern(9.0f);
	cell.write(second.data(), kN);
	CHECK(copy.pull(cell, scratch));
	CHECK(copy.values == second);
}
