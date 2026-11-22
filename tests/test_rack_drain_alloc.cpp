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

// THE RACK'S DRAIN UNDER THE AUDIO-THREAD GUARD. `RackBridge::drain` takes each publish of the chain record through the
// same `PublishedChainState::adopt` the synth's `adoptChains` uses, but only the synth was held to "no allocation, no
// free on the audio thread" (test_note_drops "(d)"). Here a second thread publishes chain records while this one drains
// block after block, and the drain is counted: zero allocations and zero frees.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "fx/filter/filter_types.h"
#include "plugin/device_registry.h"
#include "rack/rack_axes.h"
#include "rack/rack_bridge.h"
#include "rack/rack_engine.h"

#include <atomic>
#include <cstdint>
#include <new>
#include <thread>
#include <vector>

#include "test_heap.h"

using namespace feathers;

namespace {
thread_local bool tInDrain = false;
std::atomic<uint64_t> gDrainAllocations { 0 };
std::atomic<uint64_t> gDrainFrees { 0 };
void countedFree(void* p) noexcept {
	if (tInDrain && p != nullptr) {
		gDrainFrees.fetch_add(1, std::memory_order_relaxed);
	}
	feathers::test::heapFree(p);
}
} // namespace

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
void* operator new(std::size_t size) {
	if (tInDrain) {
		gDrainAllocations.fetch_add(1, std::memory_order_relaxed);
	}
	if (void* p = feathers::test::heapAlloc(size == 0 ? 1 : size)) {
		return p;
	}
	throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
	return ::operator new(size);
}
void* operator new(std::size_t size, std::align_val_t al) {
	if (tInDrain) {
		gDrainAllocations.fetch_add(1, std::memory_order_relaxed);
	}
	const std::size_t a = static_cast<std::size_t>(al);
	const std::size_t rounded = (size + a - 1) / a * a;
	if (void* p = feathers::test::heapAlignedAlloc(a, rounded == 0 ? a : rounded)) {
		return p;
	}
	throw std::bad_alloc();
}
void* operator new[](std::size_t size, std::align_val_t al) {
	return ::operator new(size, al);
}
void operator delete(void* p) noexcept { countedFree(p); }
void operator delete[](void* p) noexcept { countedFree(p); }
void operator delete(void* p, std::size_t) noexcept { countedFree(p); }
void operator delete[](void* p, std::size_t) noexcept { countedFree(p); }
void operator delete(void* p, std::align_val_t) noexcept { countedFree(p); }
void operator delete[](void* p, std::align_val_t) noexcept { countedFree(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { countedFree(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { countedFree(p); }
#pragma GCC diagnostic pop

TEST_CASE("a rack's drain adopts chain records published from another thread with no allocation and no free") {
	rack::RackEngine engine { rack::kRackAxes, hostCatalogue() };
	engine.prepare(48000.0, 512);
	rack::RackBridge bridge { engine };
	constexpr size_t kDevices = 6;
	for (size_t i = 0; i < kDevices; ++i) {
		REQUIRE(bridge.addDevice(fx::filter::kTraits.tag) != kNoInstance);
	}
	bridge.drain(); // the first adoption builds the instances, outside the guard
	bridge.drain();

	// THE MESSAGE THREAD: every publish reorders the chain (a whole new state to adopt), until told to stop.
	std::atomic<bool> stop { false };
	std::atomic<uint64_t> publishes { 0 };
	std::thread writer([&] {
		for (size_t i = 0; !stop.load(std::memory_order_relaxed); ++i) {
			(void)bridge.moveDevice(i % kDevices, (i * 5 + 1) % kDevices);
			publishes.fetch_add(1, std::memory_order_relaxed);
			std::this_thread::yield();
		}
	});
	uint64_t blocks = 0;
	uint64_t seenPublishes = 0;
	uint64_t adopted = 0;
	uint32_t lastConverges = 0;
	(void)lastConverges;
	while (blocks < 20000 || adopted < 200) {
		const uint64_t before = publishes.load(std::memory_order_relaxed);
		tInDrain = true;
		bridge.drain();
		tInDrain = false;
		++blocks;
		const uint64_t after = publishes.load(std::memory_order_relaxed);
		adopted += after != before ? 1 : 0;
		seenPublishes = after;
		if (blocks > 2000000) {
			break;
		}
	}
	stop.store(true, std::memory_order_relaxed);
	writer.join();
	CHECK(gDrainAllocations.load() == 0);
	CHECK(gDrainFrees.load() == 0);
	// NON-VACUITY: the writer really published while the drains ran.
	CHECK(seenPublishes > 100);
	CHECK(adopted > 100);
}
