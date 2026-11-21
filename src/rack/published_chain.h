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

#include "rack/chain_state.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <array>
#include <memory>
#include <vector>

namespace feathers::rack {
/// **A RECORD'S STATE, PUBLISHED ACROSS THE THREAD BOUNDARY — A TRIPLE BUFFER.** The message thread writes
/// (`publish`); the audio thread's drain takes the newest whole publish (`adopt`) at a point it owns, the top of
/// its block. Three `ChainState`s of the record's shape: the WRITER's (back), the one waiting to be taken
/// (middle), and the READER's (front). A publish fills the back state, then exchanges it with the middle one in
/// one atomic word that also says "new"; an adopt that sees "new" exchanges its front with the middle one and
/// trades contents with the caller's state. So:
///
///   - THE READER NEVER REFUSES, NEVER WAITS AND NEVER COPIES. This was a seqlock until 2026-11-21, and a read
///     that saw a publish land was refused, leaving the block on the old state. The editor republishes the whole
///     record on every knob it moves, so on the block a Pattern was first installed a hand on ANY control could
///     send that block's notes past it to the voices (test_note_drops "note drops (d)": 7 runs in 900 lost the
///     Pattern's steps, each exactly a run whose first block was refused).
///   - THE WRITER NEVER BLOCKS AND NEVER TOUCHES THE READER'S STATE. With two buffers it would have to wait for
///     the reader to let go of one; the third is what lets it always have one of its own to fill.
///   - TWO PUBLISHES BEFORE A TAKE: the newer one wins, the older is overwritten in place of a queue.
///   - A STATE IS NEVER TORN: the writer fills only the back state, which no reader can reach until the
///     exchange hands it over whole (release/acquire on the one word).
///
/// ONE READER. Its state (`front_`) is plain: the host calls the drain from one thread at a time (process,
/// or flush when not processing — never both at once), as it did when the drain wrote a staging copy.
///
/// **`adopt` IS THE ONLY WAY IN, AND IT TRADES WITH THE SAME `current` EVERY CALL.** The newest publish is
/// swapped INTO `current`, and the reader's front state then holds what `current` held, which is not read
/// again. An adopt with nothing new leaves `current` as it is: it already holds the newest this reader took.
class PublishedChainState {
public:
	/// ALLOCATES — construct where allocation is allowed, to the record's shape. `capacity` is the most
	/// devices ONE chain may hold.
	///
	/// `startSequence` CONTINUES ANOTHER'S COUNT — a published state replaced by a bigger one (unbounded devices
	/// step 5) must not restart at a number a drain has already taken, or the drain would skip its first publish.
	PublishedChainState(size_t capacity, size_t chains, uint32_t startSequence = 0)
	    : capacity_(capacity), chains_(chains),
	      state_ { ChainState(capacity, chains), ChainState(capacity, chains), ChainState(capacity, chains) } {
		sequence_.store(startSequence & ~1u, std::memory_order_relaxed); // even, as it always was
		taken_ = startSequence & ~1u;
	}

	PublishedChainState(const PublishedChainState&) = delete;
	PublishedChainState& operator=(const PublishedChainState&) = delete;

	[[nodiscard]] size_t capacity() const noexcept { return capacity_; }
	[[nodiscard]] size_t chainCount() const noexcept { return chains_; }

	/// ONE WRITER, the message thread. Refused (false, nothing written) for a state of another shape. No allocation:
	/// the back state is this shape already.
	bool publish(const ChainState& s) noexcept {
		if (s.capacity() != capacity_ || s.chainCount() != chains_) {
			return false;
		}
		const uint32_t next = sequence_.load(std::memory_order_relaxed) + 2u;
		(void)state_[back_].assign(s);
		stamp_[back_] = next;
		// THE HAND-OVER: everything written above is visible to the reader that takes this index (release), and the
		// state that was waiting — never the reader's — becomes the writer's to fill next.
		const uint32_t was = middle_.exchange(back_ | kNew, std::memory_order_acq_rel);
		back_ = was & kIndex;
		sequence_.store(next, std::memory_order_release);
		return true;
	}

	/// **THE NEWEST PUBLISH, INTO `current`** — O(1): one atomic exchange and a trade of buffers. True with `*sequence`
	/// the publish `current` holds; false (nothing touched) only for a `current` of another shape, which waits for its
	/// own growth. Never refused for a publish in flight: that one is the back state, and is taken next time.
	[[nodiscard]] bool adopt(ChainState* current, [[maybe_unused]] ChainState* staging, uint32_t* sequence) const noexcept {
		if (current == nullptr || current->capacity() != capacity_ || current->chainCount() != chains_) {
			return false;
		}
		if ((middle_.load(std::memory_order_relaxed) & kNew) != 0) {
			const uint32_t was = middle_.exchange(front_, std::memory_order_acq_rel);
			front_ = was & kIndex;
			swap(*current, state_[front_]);
			taken_ = stamp_[front_];
		}
		*sequence = taken_;
		return true;
	}

	/// WHETHER A PUBLISH IS WAITING THAT THE READER HAS NOT TAKEN — the drain's "anything new?", one load.
	[[nodiscard]] bool fresh() const noexcept { return (middle_.load(std::memory_order_acquire) & kNew) != 0; }

	/// WHICH PUBLISH IS NEWEST — even, 0 before the first. For the message thread (what a provision builds for).
	[[nodiscard]] uint32_t sequence() const noexcept { return sequence_.load(std::memory_order_acquire); }

private:
	static constexpr uint32_t kIndex = 3u;
	static constexpr uint32_t kNew = 4u;

	size_t capacity_;
	size_t chains_;
	/// THE THREE STATES. A slot's buffers move between slots and the caller's state by `swap`, never by copy on the reader.
	mutable std::array<ChainState, 3> state_;
	/// The publish each slot holds (written with the slot, before its hand-over).
	mutable std::array<uint32_t, 3> stamp_ {};
	/// WRITER-OWNED: the slot the next publish fills.
	uint32_t back_ = 2;
	/// THE SHARED WORD: the waiting slot's index, and `kNew` while the reader has not taken it.
	mutable std::atomic<uint32_t> middle_ { 1 };
	/// READER-OWNED: the slot it took last, and that publish.
	mutable uint32_t front_ = 0;
	mutable uint32_t taken_ = 0;
	std::atomic<uint32_t> sequence_ { 0 };
};


/// **A PUBLISHED STATE THAT CAN BE OUTGROWN** (unbounded devices step 5). A `PublishedChainState` has the shape
/// of the record it was built for, and a record that grows publishes a state of a bigger shape, which that
/// one refuses. So a publish of a new shape builds a new published state — on the MESSAGE thread, which may
/// allocate — continuing the sequence, publishes into it, and only then points readers at it.
///
/// **THE OLD ONES ARE KEPT UNTIL THIS IS DESTROYED.** The drain may be reading the old one when the pointer
/// moves, and nothing here can know when it has stopped; every growth at least doubles the record, so what
/// is kept sums to less than the live one. A drain that reads the new one before its own buffers have grown
/// is refused by `adopt`'s shape check and asks again next block — one block late, never wrong.
class PublishedChains {
public:
	/// ALLOCATES — construct where allocation is allowed.
	PublishedChains(size_t capacity, size_t chains) {
		all_.push_back(std::make_unique<PublishedChainState>(capacity, chains));
		current_.store(all_.back().get(), std::memory_order_release);
	}
	PublishedChains(const PublishedChains&) = delete;
	PublishedChains& operator=(const PublishedChains&) = delete;

	/// ONE WRITER, THE MESSAGE THREAD. A state of the current shape is published as before; one of another
	/// shape gets a published state of its own.
	bool publish(const ChainState& s) {
		PublishedChainState* now = all_.back().get();
		if (s.capacity() == now->capacity() && s.chainCount() == now->chainCount()) {
			return now->publish(s);
		}
		all_.push_back(std::make_unique<PublishedChainState>(s.capacity(), s.chainCount(), now->sequence()));
		PublishedChainState* next = all_.back().get();
		const bool ok = next->publish(s);
		current_.store(next, std::memory_order_release);
		return ok;
	}

	/// What readers read — any thread; one acquire.
	[[nodiscard]] const PublishedChainState& current() const noexcept {
		return *current_.load(std::memory_order_acquire);
	}
	[[nodiscard]] uint32_t sequence() const noexcept { return current().sequence(); }
	[[nodiscard]] size_t capacity() const noexcept { return current().capacity(); }
	[[nodiscard]] size_t publishedShapesForTest() const noexcept { return all_.size(); }

private:
	std::vector<std::unique_ptr<PublishedChainState>> all_;
	std::atomic<PublishedChainState*> current_ { nullptr };
};

} // namespace feathers::rack
