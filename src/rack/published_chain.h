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
#include <memory>
#include <vector>

namespace feathers::rack {

/// **A RECORD'S STATE, PUBLISHED ACROSS THE THREAD BOUNDARY — whole or not at all.** The message thread
/// writes (`publish`); the audio thread's drain takes a `snapshot` into a `ChainState` of the same shape
/// and converges to it. The same seqlock `backplane::RouteList` is (odd while a publish is written;
/// Boehm's reader and writer fences), for the same reason: a torn copy of a chain is not a torn number
/// but a device under another device's id, and the engine would build it.
///
/// A SNAPSHOT THAT SAW A PUBLISH LAND IS REFUSED, and the drain keeps converging to the state it had —
/// one block late, never wrong. `sequence` lets the drain skip the copy on every block nothing changed.
///
/// **`adopt` IS THE ONLY WAY IN, AND THAT IS THE FIX FOR A LOST ADD (2026-09-11).** A snapshot writes as
/// it reads, so a refused one has already overwritten part of its target. `snapshot` was public and said
/// "`out` not to be used" on a false — and BOTH of its callers snapshotted into the very state they ran
/// and ran it anyway: the rack's drain converged a torn state, and Feathers' lane synced from one. A torn
/// state can say a running device is not wanted; the converger removes it, the message thread's mirror
/// still says it was built, and nothing builds it again. MEASURED on the churn case under the sanitizer:
/// one run in twenty ended with the engine a device short and nothing pending. Now a snapshot lands in
/// STAGING and only a whole one is swapped in — `RouteList::snapshot`'s shape, which never had the bug.
class PublishedChainState {
public:
	/// ALLOCATES — construct where allocation is allowed, to the record's shape. `capacity` is the most
	/// devices ONE chain may hold.
	///
	/// `startSequence` CONTINUES ANOTHER'S COUNT — a published state replaced by a bigger one (unbounded devices
	/// step 5) must not restart at a number a drain has already taken, or the drain would skip its first publish.
	PublishedChainState(size_t capacity, size_t chains, uint32_t startSequence = 0)
	    : capacity_(capacity), chains_(chains), device_(std::make_unique<Device[]>(capacity * chains)),
	      length_(std::make_unique<std::atomic<uint32_t>[]>(chains)),
	      order_(std::make_unique<std::atomic<DeviceInstanceId>[]>(capacity * chains + 1)) {
		for (size_t c = 0; c < chains; ++c) {
			length_[c].store(0, std::memory_order_relaxed);
		}
		sequence_.store(startSequence & ~1u, std::memory_order_relaxed); // even: no publish in progress
	}

	PublishedChainState(const PublishedChainState&) = delete;
	PublishedChainState& operator=(const PublishedChainState&) = delete;

	[[nodiscard]] size_t capacity() const noexcept { return capacity_; }
	[[nodiscard]] size_t chainCount() const noexcept { return chains_; }

	/// ONE WRITER. Refused (false, nothing written) for a state of another shape.
	///
	/// **EACH CHAIN'S DEVICES IN ORDER (kill-lanes step 3b)** — it published a lane-indexed array of rows
	/// and a path of lane numbers beside it, two structures that had to be torn together to be read
	/// together. One run per chain is one thing to tear, and the seqlock covers it as it always did.
	bool publish(const ChainState& s) noexcept {
		if (s.capacity() != capacity_ || s.chainCount() != chains_) {
			return false;
		}
		sequence_.store(sequence_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_release);
		for (size_t c = 0; c < chains_; ++c) {
			const std::span<const DeviceState> path = s.chain(c);
			for (size_t p = 0; p < path.size(); ++p) {
				const DeviceState& from = path[p];
				Device& to = device_[c * capacity_ + p];
				uint64_t tag = 0;
				static_assert(sizeof(tag) >= device::kTagBytes,
				              "the packed tag must hold the WHOLE tag: a narrower word truncates it "
				              "silently on the publish seam, and the device simply stops being found");
				std::memcpy(&tag, from.tag.data(), device::kTagBytes);
				to.tag.store(tag, std::memory_order_relaxed);
				to.instanceId.store(from.instanceId, std::memory_order_relaxed);
				to.fieldsPresent.store(static_cast<uint32_t>(from.fieldsPresent), std::memory_order_relaxed);
				for (size_t f = 0; f < from.field.size(); ++f) {
					to.field[f].store(from.field[f], std::memory_order_relaxed);
				}
			}
			length_[c].store(static_cast<uint32_t>(path.size()), std::memory_order_relaxed);
		}
		// AND THE EVALUATION ORDER THE GRAPH GAVE (kill-lanes 4c) — one id per node, under the same sequence.
		const std::span<const DeviceInstanceId> order = s.order();
		for (size_t i = 0; i < order.size(); ++i) {
			order_[i].store(order[i], std::memory_order_relaxed);
		}
		orderCount_.store(static_cast<uint32_t>(order.size()), std::memory_order_relaxed);
		// AND THE HOST SLOTS' BINDINGS (T5) — every entry, live or not, under the same sequence.
		const std::span<const SlotBinding> bindings = s.slotBindings();
		for (size_t n = 0; n < bindings.size(); ++n) {
			binding_[n].device.store(bindings[n].device, std::memory_order_relaxed);
			binding_[n].packed.store(packBinding(bindings[n]), std::memory_order_relaxed);
		}
		sequence_.store(sequence_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
		return true;
	}

	/// **BECOME THE PUBLISHED STATE, WHOLE OR NOT AT ALL.** A snapshot into `staging` (this shape), swapped
	/// into `current` only if it was whole; a false leaves `current` exactly as it was, so the reader runs on
	/// the state it had and asks again. `*sequence` is the publish adopted, written only on a true. No
	/// allocation and no copy past the snapshot's own: safe on the audio thread.
	[[nodiscard]] bool adopt(ChainState* current, ChainState* staging, uint32_t* sequence) const noexcept {
		uint32_t seen = 0;
		if (current == nullptr || !snapshot(staging, &seen)) {
			return false;
		}
		swap(*current, *staging);
		*sequence = seen;
		return true;
	}

	/// WHICH PUBLISH IS CURRENT — even when one is complete, odd while one is written, 0 before the first.
	[[nodiscard]] uint32_t sequence() const noexcept { return sequence_.load(std::memory_order_acquire); }

private:
	/// THE COPY, INTO `out` — which it has written into even when it returns false. PRIVATE for that reason:
	/// see the banner, and `adopt`.
	[[nodiscard]] bool snapshot(ChainState* out, uint32_t* sequence) const noexcept {
		if (out == nullptr || out->capacity() != capacity_ || out->chainCount() != chains_) {
			return false;
		}
		const uint32_t before = sequence_.load(std::memory_order_acquire);
		if ((before & 1u) != 0) {
			return false;
		}
		out->clear();
		bool placed = true;
		for (size_t c = 0; c < chains_; ++c) {
			const size_t length = std::min<size_t>(length_[c].load(std::memory_order_relaxed), capacity_);
			for (size_t p = 0; p < length; ++p) {
				const Device& from = device_[c * capacity_ + p];
				DeviceState to;
				const uint64_t tag = from.tag.load(std::memory_order_relaxed);
				std::memcpy(to.tag.data(), &tag, device::kTagBytes);
				to.instanceId = from.instanceId.load(std::memory_order_relaxed);
				to.fieldsPresent = from.fieldsPresent.load(std::memory_order_relaxed);
				for (size_t f = 0; f < to.field.size(); ++f) {
					to.field[f] = from.field[f].load(std::memory_order_relaxed);
				}
				// A TORN READ CAN NAME ONE DEVICE TWICE, which `append` refuses — and that refusal is exactly
				// the "not whole" this reports. The sequence check below catches it too; this catches it
				// without waiting for a writer to be caught in the act.
				placed = out->append(c, to) && placed;
			}
		}
		// THE ORDER, INTO THE STATE'S OWN STORAGE — no allocation here, this runs at the audio thread's drain.
		const size_t orderCount = std::min<size_t>(orderCount_.load(std::memory_order_relaxed), capacity_ * chains_ + 1);
		const std::span<DeviceInstanceId> ids = out->resizeOrder(orderCount);
		for (size_t i = 0; i < ids.size(); ++i) {
			ids[i] = order_[i].load(std::memory_order_relaxed);
		}
		// THE BINDINGS, whole — `clear()` above does not touch them, so every entry is written here. One the
		// state refuses (a torn read naming another slot) is written DEAD rather than left as it was.
		for (size_t n = 0; n < kSlotBindingCapacity; ++n) {
			const SlotBinding b = unpackBinding(binding_[n].device.load(std::memory_order_relaxed),
			                                    binding_[n].packed.load(std::memory_order_relaxed));
			if (!out->setSlotBinding(n, b)) {
				(void)out->setSlotBinding(n, SlotBinding {});
			}
		}
		std::atomic_thread_fence(std::memory_order_acquire);
		if (sequence_.load(std::memory_order_relaxed) != before || !placed) {
			return false;
		}
		*sequence = before;
		return true;
	}

	struct Device {
		/// **THE WHOLE TAG IN ONE ATOMIC WORD.** It was a uint32 while tags were four bytes, and widening
		/// the field without widening this truncated every longer tag on the way across -- "OWLTT"
		/// arrived as "OWLT", the catalogue missed it, and the device vanished from the chain with no
		/// error anywhere. Sixty-four bits is also the ceiling: `device::kTagBytes` is chosen so this
		/// stays one lock-free word.
		std::atomic<uint64_t> tag { 0 };
		std::atomic<DeviceInstanceId> instanceId { kNoInstance };
		std::atomic<uint32_t> fieldsPresent { 0 };
		std::array<std::atomic<float>, device::kMaxDeviceFields> field {};
	};

	/// ONE BINDING IN TWO LOCK-FREE WORDS: the device, and control | slot << 16 | live << 32.
	struct Binding {
		std::atomic<DeviceInstanceId> device { kNoInstance };
		std::atomic<uint64_t> packed { 0 };
	};
	static uint64_t packBinding(const SlotBinding& b) noexcept {
		return static_cast<uint64_t>(b.control) | static_cast<uint64_t>(b.slot) << 16
		       | static_cast<uint64_t>(b.live ? 1u : 0u) << 32;
	}
	static SlotBinding unpackBinding(DeviceInstanceId device, uint64_t packed) noexcept {
		return SlotBinding { .device = device,
		                     .control = static_cast<uint16_t>(packed & 0xFFFFu),
		                     .slot = static_cast<uint16_t>(packed >> 16 & 0xFFFFu),
		                     .live = (packed >> 32 & 1u) != 0 };
	}

	size_t capacity_;
	size_t chains_;
	std::array<Binding, kSlotBindingCapacity> binding_ {};
	std::unique_ptr<Device[]> device_;
	std::unique_ptr<std::atomic<uint32_t>[]> length_;
	std::unique_ptr<std::atomic<DeviceInstanceId>[]> order_;
	std::atomic<uint32_t> orderCount_ { 0 };
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
