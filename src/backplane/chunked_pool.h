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

#include <atomic>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <vector>

// A POOL THAT GROWS WITHOUT MOVING ANYTHING — unbounded devices.
//
// **WHY CHUNKS.** Every per-device store the audio thread reaches was a fixed array, because a pool the
// audio thread reads cannot be reallocated under it, and `std::vector` growth MOVES its elements. A moved
// element is the bug this codebase has paid for most often: an address remembered on one side of a growth
// and read on the other ("an address is only a valid identity while the thing at it cannot be replaced",
// CLAUDE.md). So elements live in fixed chunks that are allocated once and never moved or freed while the
// pool lives, and growth only adds chunks. `&pool[i]` is the same address for the pool's whole life.
//
// **WHY A DIRECTORY, AND WHY OLD ONES ARE KEPT.** The chunks are reached through a directory of chunk
// pointers, and growing past it means a bigger directory, published with one release store. A reader on
// another thread may still be walking the OLD directory when that store lands, so the old one cannot be
// freed until no reader can hold it — which, for a reader on the audio thread, is a drain-and-epoch
// question this type would have to be told the answer to. It is not asked: an old directory is only chunk
// POINTERS (eight bytes per sixteen elements), every one of which is still valid in the new directory, so
// it is kept until the pool is destroyed. Doubling makes the kept ones sum to less than the live one; the
// route stores grow by 3/2, which keeps them under twice it.
//
// **A VIEW IS ONE DIRECTORY, HELD.** `view()` takes the directory once, so a hot loop indexes chunks with no
// atomic per element. It stays valid for the pool's life (no directory is freed) and reaches exactly the
// elements that existed when it was taken — every one initialised before that directory was published.
//
// **ONE WRITER OF THE SHAPE, ANY NUMBER OF READERS.** `grow` is the message thread's (it allocates);
// `at`/`capacity` may be called from any thread, and allocate nothing. What the ELEMENTS hold, and who may
// write them, is the element type's business — the pool only promises they stay where they are.
//
// **NO SHRINK**.

namespace feathers::backplane {

/// `kGrowNum / kGrowDen` is the least factor a growth multiplies the chunk count by: 2 for the device pools,
/// 3/2 for the route stores.
template <typename T, size_t kChunk = 16, size_t kGrowNum = 2, size_t kGrowDen = 1>
class ChunkedPool {
	static_assert(kChunk > 0, "a chunk holds at least one element");
	static_assert(kGrowNum > kGrowDen && kGrowDen > 0, "a growth grows");
	struct Directory;

public:
	ChunkedPool() = default;
	/// ALLOCATES — construct where allocation is allowed.
	explicit ChunkedPool(size_t capacity) { grow(capacity); }

	ChunkedPool(const ChunkedPool&) = delete;
	ChunkedPool& operator=(const ChunkedPool&) = delete;

	/// HOW MANY ELEMENTS ARE ADDRESSABLE. Any thread; acquire, so a reader that sees a capacity sees the
	/// chunks behind it.
	[[nodiscard]] size_t capacity() const noexcept {
		const Directory* d = current_.load(std::memory_order_acquire);
		return d != nullptr ? d->chunks.size() * kChunk : 0;
	}

	/// ELEMENT `i`, OR NULL PAST THE CAPACITY. Any thread; allocates nothing.
	[[nodiscard]] T* at(size_t i) noexcept {
		const Directory* d = current_.load(std::memory_order_acquire);
		if (d == nullptr || i / kChunk >= d->chunks.size()) {
			return nullptr;
		}
		return &d->chunks[i / kChunk]->item[i % kChunk];
	}
	[[nodiscard]] const T* at(size_t i) const noexcept { return const_cast<ChunkedPool*>(this)->at(i); }

	/// ELEMENT `i`, UNCHECKED — for a caller that has already bounded `i` by `size()`.
	[[nodiscard]] T& operator[](size_t i) noexcept { return *at(i); }
	[[nodiscard]] const T& operator[](size_t i) const noexcept { return *at(i); }
	/// `capacity()`, spelled the way the containers this replaces spelled it.
	[[nodiscard]] size_t size() const noexcept { return capacity(); }

	/// A WALK OVER THE ELEMENTS ADDRESSABLE WHEN IT BEGAN — `for (T& t : pool)`. A growth during the walk
	/// adds elements it does not visit and moves none it does.
	template <typename Pool, typename Ref>
	class Iterator {
	public:
		Iterator(Pool* pool, size_t i) noexcept : pool_(pool), i_(i) {}
		Ref operator*() const noexcept { return (*pool_)[i_]; }
		Iterator& operator++() noexcept {
			++i_;
			return *this;
		}
		bool operator!=(const Iterator& o) const noexcept { return i_ != o.i_; }

	private:
		Pool* pool_;
		size_t i_;
	};
	Iterator<ChunkedPool, T&> begin() noexcept { return { this, 0 }; }
	Iterator<ChunkedPool, T&> end() noexcept { return { this, capacity() }; }
	Iterator<const ChunkedPool, const T&> begin() const noexcept { return { this, 0 }; }
	Iterator<const ChunkedPool, const T&> end() const noexcept { return { this, capacity() }; }

	/// MESSAGE THREAD — THE ONE WRITER OF THE SHAPE. Makes at least `atLeast` elements addressable, rounding
	/// up to whole chunks and at least by the growth factor, so a run of single adds costs a logarithmic number of
	/// directories. Elements that already existed keep their addresses and their values; new ones are
	/// value-initialised. Returns the capacity it left.
	size_t grow(size_t atLeast) {
		return grow(atLeast, [](T&, size_t) {});
	}

	/// THE SAME, AND EACH NEW ELEMENT IS HANDED TO `init(element, index)` BEFORE THE DIRECTORY THAT REACHES IT
	/// IS PUBLISHED — so a reader on another thread can never see one half-prepared (a slot whose per-voice
	/// rows are not sized yet, a hop that does not know its own index).
	template <typename Init>
	size_t grow(size_t atLeast, Init&& init) {
		const size_t have = capacity();
		if (atLeast <= have) {
			return have;
		}
		const size_t chunksHad = have / kChunk;
		size_t chunksWant = (atLeast + kChunk - 1) / kChunk;
		if (const size_t factor = (chunksHad * kGrowNum + kGrowDen - 1) / kGrowDen; chunksWant < factor) {
			chunksWant = factor;
		}
		auto next = std::make_unique<Directory>();
		next->chunks.reserve(chunksWant);
		if (const Directory* d = current_.load(std::memory_order_relaxed); d != nullptr) {
			next->chunks.assign(d->chunks.begin(), d->chunks.end());
		}
		while (next->chunks.size() < chunksWant) {
			owned_.push_back(std::make_unique<Chunk>());
			Chunk& fresh = *owned_.back();
			for (size_t k = 0; k < kChunk; ++k) {
				init(fresh.item[k], next->chunks.size() * kChunk + k);
			}
			next->chunks.push_back(&fresh);
		}
		current_.store(next.get(), std::memory_order_release);
		directories_.push_back(std::move(next));
		return chunksWant * kChunk;
	}

	/// ONE DIRECTORY, HELD — see the header. Any thread; one acquire, then plain loads.
	template <typename E>
	class ViewOf {
	public:
		ViewOf() = default;
		[[nodiscard]] size_t capacity() const noexcept { return dir_ != nullptr ? dir_->chunks.size() * kChunk : 0; }
		[[nodiscard]] size_t size() const noexcept { return capacity(); }
		/// ELEMENT `i`, OR NULL PAST WHAT THIS VIEW REACHES.
		[[nodiscard]] E* at(size_t i) const noexcept {
			return dir_ != nullptr && i / kChunk < dir_->chunks.size() ? &dir_->chunks[i / kChunk]->item[i % kChunk] : nullptr;
		}
		/// UNCHECKED, for a caller that has bounded `i` by `capacity()`.
		[[nodiscard]] E& operator[](size_t i) const noexcept { return dir_->chunks[i / kChunk]->item[i % kChunk]; }
		/// A WRITABLE VIEW READS AS A READ-ONLY ONE — the same directory.
		operator ViewOf<const E>() const noexcept
		    requires(!std::is_const_v<E>)
		{
			return ViewOf<const E>(dir_);
		}

	private:
		friend class ChunkedPool;
		template <typename>
		friend class ViewOf;
		explicit ViewOf(const Directory* dir) noexcept : dir_(dir) {}
		const Directory* dir_ = nullptr;
	};
	using View = ViewOf<T>;
	using ConstView = ViewOf<const T>;
	[[nodiscard]] View view() noexcept { return View(current_.load(std::memory_order_acquire)); }
	[[nodiscard]] ConstView view() const noexcept { return ConstView(current_.load(std::memory_order_acquire)); }

	/// HOW MANY DIRECTORIES THIS POOL HAS PUBLISHED — for a case that wants to see the doubling.
	[[nodiscard]] size_t directoryCountForTest() const noexcept { return directories_.size(); }

private:
	struct Chunk {
		T item[kChunk] {};
	};
	struct Directory {
		std::vector<Chunk*> chunks;
	};

	std::atomic<const Directory*> current_ { nullptr };
	/// Message thread only. The chunks, which never move; and every directory ever published, kept for the
	/// reason in the header.
	std::vector<std::unique_ptr<Chunk>> owned_;
	std::vector<std::unique_ptr<Directory>> directories_;
};

} // namespace feathers::backplane
