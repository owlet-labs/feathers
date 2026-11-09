/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "backplane/chunked_pool.h"
#include "backplane/endpoint.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace feathers::backplane {

/// **A STORE OF ROUTES THAT GROWS WITHOUT MOVING** (backlog OJ) — what a consumer snapshots into and an engine
/// keeps its handed-over routes in. Grown on the message thread by x1.5 in chunks of 16; never shrinks; an
/// element's address holds for the store's life, so the audio thread may be writing one while the message
/// thread adds room.
using RouteStage = ChunkedPool<Route, 16, 3, 2>;

/// **THE FIRST `count` ROUTES OF A STAGE, THROUGH ONE HELD DIRECTORY** — what a snapshot hands on in place of a
/// span (a stage is not contiguous). Valid while the stage lives; reaches what existed when it was taken.
struct RouteRun {
	RouteStage::ConstView view;
	size_t count = 0;

	[[nodiscard]] size_t size() const noexcept { return count; }
	[[nodiscard]] bool empty() const noexcept { return count == 0; }
	[[nodiscard]] const Route& operator[](size_t i) const noexcept { return view[i]; }

	class Iterator {
	public:
		Iterator(const RouteRun* run, size_t i) noexcept : run_(run), i_(i) {}
		const Route& operator*() const noexcept { return (*run_)[i_]; }
		Iterator& operator++() noexcept {
			++i_;
			return *this;
		}
		bool operator!=(const Iterator& o) const noexcept { return i_ != o.i_; }

	private:
		const RouteRun* run_;
		size_t i_;
	};
	[[nodiscard]] Iterator begin() const noexcept { return { this, 0 }; }
	[[nodiscard]] Iterator end() const noexcept { return { this, count }; }
};

/// **THE ROUTES AS A HOST PUBLISHES THEM — by identity, across the thread boundary.** One writer (the
/// message thread, from its record), any number of readers; a reader that must not see a torn list takes
/// a `snapshot`, whole or nothing.
///
/// **NOT WHAT A RESOLVE READS.** A block never sees an id: at a drain an engine compiles this into its own
/// own nodes' inputs (`RackEngine::rebuildInputs`), and resolves from those. So this list keeps no index and no
/// order — a route is where it was added — and the compile sorts.
///
/// THE SEQLOCK IS `PublishedChainState`'s (odd while written, Boehm's fences), for the
/// same reason: a copy taken across a publish would silently lose routes nobody touched.
///
/// **UNBOUNDED (backlog OJ): NO ROUTE IS DROPPED FOR ROOM.** The slots are a `ChunkedPool`, so `add` past the
/// capacity GROWS (x1.5, on the writer's thread) and nothing a reader is walking moves. And before the count
/// that needs the room is published, the GROWTH HOOK makes the consumer's stores as big (`setGrowthHook`): a
/// consumer that acquires a sequence therefore also sees room for every route in it. A consumer whose stores
/// were not grown (no hook) refuses the snapshot and counts it — never a truncated copy.
class RouteList {
public:
	/// ALLOCATES — construct where allocation is allowed. `capacity` is only where it starts.
	explicit RouteList(size_t capacity = 16) { slots_.grow(capacity); }

	RouteList(const RouteList&) = delete;
	RouteList& operator=(const RouteList&) = delete;

	/// **TOLD THE NEW CAPACITY, ON THE WRITER'S THREAD, EACH TIME THE LIST GROWS — BEFORE THE PUBLISH THAT NEEDED IT
	/// LANDS.** What the consumer's stores grow by. Also told the current capacity at once, so a hook set after
	/// routes exist starts level with them.
	using GrowthHook = void (*)(void* context, size_t capacity);
	void setGrowthHook(GrowthHook hook, void* context) {
		hook_ = hook;
		hookContext_ = context;
		if (hook_ != nullptr) {
			hook_(hookContext_, slots_.capacity());
		}
	}

	//--------------------------------------------------------------------------------------------//
	// THE WRITER — one thread. A publish is `begin`, `add`, `end`.                                  //
	//--------------------------------------------------------------------------------------------//

	void begin() noexcept {
		sequence_.store(sequence_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_release);
		count_.store(0, std::memory_order_relaxed);
		next_ = 0;
	}

	/// ONE ROUTE. **Zero is not a route** (`Graph::route`'s rule) and is skipped here rather than at every
	/// caller. False for anything not stored. ALLOCATES when the list is full (the writer's thread may).
	bool add(const Route& r) {
		if (r.amount == 0.0f) {
			return false;
		}
		if (next_ >= slots_.capacity()) {
			const size_t grown = slots_.grow(next_ + 1);
			++growths_;
			if (hook_ != nullptr) {
				hook_(hookContext_, grown); // the consumer's room, before `end` publishes the count
			}
		}
		Slot& s = slots_[next_++];
		s.sourceDevice.store(r.source.device, std::memory_order_relaxed);
		s.sourceIndex.store(r.source.index, std::memory_order_relaxed);
		s.destinationDevice.store(r.destination.device, std::memory_order_relaxed);
		s.destinationIndex.store(r.destination.index, std::memory_order_relaxed);
		s.amount.store(r.amount, std::memory_order_relaxed);
		s.transfer.store(static_cast<uint16_t>(r.transfer), std::memory_order_relaxed);
		return true;
	}

	/// THE COUNT LAST, then the sequence even again, RELEASE — what a snapshot's acquire pairs with.
	void end() noexcept {
		count_.store(next_, std::memory_order_relaxed);
		sequence_.store(sequence_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
	}

	//--------------------------------------------------------------------------------------------//
	// THE READERS                                                                                  //
	//--------------------------------------------------------------------------------------------//

	/// EVERY ROUTE: `f(const Route&)`. On the WRITER's thread this cannot straddle a publish; anywhere else
	/// it may see fewer routes than either publish had — take a `snapshot` there.
	template <typename F>
	void forEach(F&& f) const {
		const auto slots = slots_.view();
		const size_t live = std::min(count_.load(std::memory_order_relaxed), slots.capacity());
		for (size_t i = 0; i < live; ++i) {
			f(read(slots[i]));
		}
	}

	/// WHY A SNAPSHOT TOOK NOTHING.
	enum class Refused : uint8_t { None, Torn, NoRoom };

	/// **THE WHOLE LIST, OR NOTHING.** Copies every route into `out` and answers true with the publish it
	/// is a copy of — or false, having written nothing a caller may use: a publish in progress or landed
	/// meanwhile (`Torn` — keep what you had and ask again), or `out` smaller than the list (`NoRoom` — the
	/// consumer's stores were not grown; it must say so, not truncate).
	[[nodiscard]] bool snapshot(RouteStage& out, size_t* count, uint32_t* sequence, Refused* why = nullptr) const noexcept {
		const auto refuse = [why](Refused r) {
			if (why != nullptr) {
				*why = r;
			}
			return false;
		};
		const uint32_t before = sequence_.load(std::memory_order_acquire);
		if ((before & 1u) != 0) {
			return refuse(Refused::Torn);
		}
		// THE DIRECTORIES AFTER THE ACQUIRE: a sequence whose count needs the room was published after the room.
		const auto slots = slots_.view();
		const auto into = out.view();
		const size_t live = count_.load(std::memory_order_relaxed);
		if (live > slots.capacity()) {
			return refuse(Refused::Torn); // a count from a publish newer than the directory just read
		}
		if (live > into.capacity()) {
			return refuse(Refused::NoRoom);
		}
		for (size_t i = 0; i < live; ++i) {
			into[i] = read(slots[i]);
		}
		std::atomic_thread_fence(std::memory_order_acquire);
		if (sequence_.load(std::memory_order_relaxed) != before) {
			return refuse(Refused::Torn);
		}
		*count = live;
		*sequence = before;
		if (why != nullptr) {
			*why = Refused::None;
		}
		return true;
	}

	/// HOW FAR `s` REACHES `d`, or 0 — a walk, for the writer's thread and a case.
	[[nodiscard]] float amountOf(Source s, Destination d) const noexcept {
		float found = 0.0f;
		forEach([&](const Route& r) {
			if (r.source == s && r.destination == d) {
				found = r.amount;
			}
		});
		return found;
	}

	/// WHICH PUBLISH THIS IS — even when complete, odd while written, zero before the first.
	[[nodiscard]] uint32_t sequence() const noexcept { return sequence_.load(std::memory_order_acquire); }
	[[nodiscard]] size_t count() const noexcept { return count_.load(std::memory_order_relaxed); }
	/// WHERE THE LIST STANDS — not a bound: `add` grows past it.
	[[nodiscard]] size_t capacity() const noexcept { return slots_.capacity(); }
	/// How many times the list has grown — a case counts it against log1.5 of the routes.
	[[nodiscard]] size_t growthsForTest() const noexcept { return growths_; }

private:
	/// ONE PUBLISHED ROUTE, ATOMIC PER WORD — a whole 4-byte store is never seen half-done.
	struct Slot {
		std::atomic<uint32_t> sourceDevice { 0 };
		std::atomic<uint32_t> sourceIndex { 0 };
		std::atomic<uint32_t> destinationDevice { 0 };
		std::atomic<uint32_t> destinationIndex { 0 };
		std::atomic<float> amount { 0.0f };
		/// THE CURVE'S WHOLE CODE — kind and coefficient step (`device/transfer.h`), so 16 bits.
		std::atomic<uint16_t> transfer { 0 };
	};

	static Route read(const Slot& s) noexcept {
		return Route { Source { s.sourceDevice.load(std::memory_order_relaxed), s.sourceIndex.load(std::memory_order_relaxed) },
			           Destination { s.destinationDevice.load(std::memory_order_relaxed),
				                     s.destinationIndex.load(std::memory_order_relaxed) },
			           s.amount.load(std::memory_order_relaxed),
			           static_cast<Transfer>(s.transfer.load(std::memory_order_relaxed)) };
	}

	ChunkedPool<Slot, 16, 3, 2> slots_;
	std::atomic<size_t> count_ { 0 };
	std::atomic<uint32_t> sequence_ { 0 };
	GrowthHook hook_ = nullptr;
	void* hookContext_ = nullptr;
	// THE WRITER'S OWN STATE between `begin` and `end`.
	size_t next_ = 0;
	size_t growths_ = 0;
};

} // namespace feathers::backplane
