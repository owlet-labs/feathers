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

#include "backplane/device_instance_id.h"
#include "device/registry.h"
#include "hostslots/slot_bus.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace feathers::rack {

//--------------------------------------------------------------------------------------------------//
// THE DESIRED STATE — what a chain's record says the engine should be running                      //
//--------------------------------------------------------------------------------------------------//
//
// Every host of the engine — Feathers' lane and strip,
// the standalone rack, a rack inside a rack — tells its engine WHAT TO BE, not what to do. The record
// (message thread) publishes one of these; the engine's drain converges to it (`Converger`). No verb
// is ever refused out of the editor's sight, a patch load is "here is the state", and an undo is the
// same — which is why the two products' forward channels are one channel now and not two.
//
// **AND IT IS EACH CHAIN'S ORDERED LIST OF DEVICES — NO LANES (kill-lanes step 3b).** It was a lane-indexed
// array of rows plus a path of lane numbers per chain: two structures that had to agree, and a lane that
// escaped into every reader that walked them. A chain IS its order, so the order is the storage. What a
// device is called, which device it is, and what it is set to travel together in one row, and WHERE it
// lives is nobody's business but the pool's (`RackEngine::slotOf`, `ChainRecord`'s own graph).
//
// A VALUE, AND DELIBERATELY DUMB: it knows nothing of routes, ranks or rules — those are the record's, and
// by the time a state is published they have been applied. SIZED AT CONSTRUCTION by the host's shape and
// never again, so a copy between two of the same shape (`assign`) allocates nothing and may run on the
// audio thread.

/// ONE DEVICE AS THE RECORD WANTS IT.
struct DeviceState {
	device::Tag tag {};
	/// WHICH DEVICE — the identity the engine keeps an instance under. A knob moving does not change
	/// it; a replacement does, and that is the only thing that rebuilds a device.
	DeviceInstanceId instanceId { kNoInstance };
	/// HOW MANY OF `field` THE RECORD SAYS — past it a control is at its declared default, extra state
	/// at zero (`device_row.h`'s rule, kept here because the engine is who applies it).
	size_t fieldsPresent { 0 };
	std::array<float, device::kMaxDeviceFields> field {};

	[[nodiscard]] bool live() const noexcept { return instanceId != kNoInstance; }
	friend bool operator==(const DeviceState&, const DeviceState&) = default;
};

/// **ONE HOST SLOT BOUND TO ONE DEVICE CONTROL** (host slots plan §1.2 "Engine", task T5). The record says
/// which slot drives which control; the engine reads the slot's value and modulation from the
/// `hostslots::SlotValueBus` it is handed (`RackEngine::setHostSlotBus`) and applies them before it
/// resolves (`RackEngine::applyHostSlots`). BY IDENTITY: `device` is an instance id, never a position,
/// so a reorder moves no binding (design §6).
struct SlotBinding {
	DeviceInstanceId device { kNoInstance };
	/// The control's index in its descriptor's `params`.
	uint16_t control { 0 };
	/// The host slot, 0..`kSlotBindingCapacity`-1 — the `SlotValueBus` cell it reads.
	uint16_t slot { 0 };
	bool live { false };
	friend bool operator==(const SlotBinding&, const SlotBinding&) = default;
};

/// ONE BINDING PER HOST SLOT AT MOST, so the table is the bus's size. Fixed, so a state copies it with no
/// allocation and the published state carries it under the same seqlock as the rows.
inline constexpr size_t kSlotBindingCapacity = hostslots::kSlotBusCapacity;

class ChainState {
public:
	ChainState() = default;

	/// ALLOCATES — construct where allocation is allowed. `capacity` is the most devices ONE chain may
	/// hold; `chains` how many a host keeps (Feathers: the strip and the fx lane; the rack: one).
	ChainState(size_t capacity, size_t chains)
	    : capacity_(capacity), row_(capacity * chains), length_(chains, 0), order_(capacity * chains + 1) {}

	[[nodiscard]] size_t capacity() const noexcept { return capacity_; }
	[[nodiscard]] size_t chainCount() const noexcept { return length_.size(); }
	[[nodiscard]] size_t length(size_t chain) const noexcept {
		return chain < length_.size() ? length_[chain] : 0;
	}

	/// **ONE CHAIN'S DEVICES, IN THE ORDER THE BLOCK WALKS THEM.** Empty for a chain out of range.
	[[nodiscard]] std::span<const DeviceState> chain(size_t c) const noexcept {
		if (c >= chainCount()) {
			return {};
		}
		return std::span<const DeviceState>(row_).subspan(c * capacity_, length_[c]);
	}

	/// The device at `position` of `chain`, or a dead row (`live()` false) for a position nothing holds.
	[[nodiscard]] const DeviceState& at(size_t chain, size_t position) const noexcept {
		static const DeviceState kNone {};
		return position < length(chain) ? row_[chain * capacity_ + position] : kNone;
	}
	/// The same, to WRITE — for the record, which is the only thing that edits a state in place.
	[[nodiscard]] DeviceState* mutableAt(size_t chain, size_t position) noexcept {
		return position < length(chain) ? &row_[chain * capacity_ + position] : nullptr;
	}

	/// **WHERE DEVICE `id` STANDS**, or false. The one lookup every reader needs and none of them should
	/// write for itself.
	[[nodiscard]] bool find(DeviceInstanceId id, size_t* inChain, size_t* position) const noexcept {
		if (id == kNoInstance) {
			return false;
		}
		for (size_t c = 0; c < chainCount(); ++c) {
			const std::span<const DeviceState> path = chain(c);
			for (size_t p = 0; p < path.size(); ++p) {
				if (path[p].instanceId == id) {
					if (inChain != nullptr) {
						*inChain = c;
					}
					if (position != nullptr) {
						*position = p;
					}
					return true;
				}
			}
		}
		return false;
	}
	[[nodiscard]] bool holds(DeviceInstanceId id) const noexcept { return find(id, nullptr, nullptr); }

	//--------------------------------------------------------------------------------------------//
	// THE EVALUATION ORDER — the graph's, carried with the chains (kill-lanes 4c)                   //
	//--------------------------------------------------------------------------------------------//
	//
	// **THE ENGINE RUNS ITS DEVICES IN THE ORDER THE GRAPH GIVES, AND THE GRAPH IS NOT ON THE AUDIO
	// THREAD** — so the record writes `Graph::order()` here, one id per live node in topological order,
	// and the converger follows it (`Converger::converge`'s order pass). Until this the converger ran THE CHAINS IN INDEX ORDER, which agreed
	// with the graph for every shape a hand could build only because nothing across two chains had
	// been routed backwards — a fact about the shapes, not a rule (kill-lanes plan §2.5b).
	//
	// **EMPTY MEANS "NO GRAPH SPOKE"**, and the converger then falls back to the chains in index order:
	// that is a state built by hand or by the by-value fixture road (`stateOfChains`), where chain
	// index is all the order there is. A record never publishes an empty one while it holds a device.
	//
	// The host's own node is in it (`Graph::kCoreInstance`); a reader that holds no such device skips
	// it, as it skips any id it does not run. Sized for every node the chains can hold plus that one.
	[[nodiscard]] std::span<const DeviceInstanceId> order() const noexcept {
		return std::span<const DeviceInstanceId>(order_).first(orderCount_);
	}

	/// REPLACE THE ORDER WHOLE. False (and the order emptied) when it does not fit, which a graph sized
	/// from the same capacities cannot produce.
	bool setOrder(std::span<const DeviceInstanceId> ids) noexcept {
		if (ids.size() > order_.size()) {
			orderCount_ = 0;
			return false;
		}
		std::copy(ids.begin(), ids.end(), order_.begin());
		orderCount_ = ids.size();
		return true;
	}

	/// THE ORDER'S STORAGE, SIZED TO `count` AND HANDED BACK TO BE WRITTEN — for the two writers that must
	/// not allocate (the record, whose source is a span of `Step`s rather than ids; the published state's
	/// snapshot, on the audio thread). Clamped to what fits.
	[[nodiscard]] std::span<DeviceInstanceId> resizeOrder(size_t count) noexcept {
		orderCount_ = std::min(count, order_.size());
		return std::span<DeviceInstanceId>(order_).first(orderCount_);
	}

	//--------------------------------------------------------------------------------------------//
	// THE HOST SLOTS' BINDINGS (host slots plan §1.2, T5)                                            //
	//--------------------------------------------------------------------------------------------//
	//
	// **A FIXED TABLE OF `kSlotBindingCapacity`, carried with the chains** — so it reaches the engine the
	// way every other fact of the record does: published whole under `PublishedChainState`'s seqlock,
	// adopted at a drain, handed to the engine by the converger (`RackEngine::setHostSlotBindings`). An
	// entry whose `live` is false binds nothing. Written by the main thread's slot service (T6).
	//
	// **`clear()` LEAVES THEM ALONE**: the record empties its chains on every structural edit and puts the
	// rows back (`ChainRecord::syncOrder`), and a binding is not a row — clearing it there would unbind
	// every slot on every drag. `clearSlotBindings` is the explicit way.
	//
	// **THE TABLE IS INDEXED BY SLOT: entry `n` is host slot `n`'s binding, or dead.** So one slot drives at
	// most one control by construction — two live entries naming one slot cannot be written, and the engine's
	// per-slot `lastGen` has exactly one reader per slot.
	[[nodiscard]] std::span<const SlotBinding> slotBindings() const noexcept { return bindings_; }
	/// ENTRY `n` BECOMES `b`. False (nothing written) for `n` out of range, or for a live binding whose
	/// `slot` is not `n`.
	bool setSlotBinding(size_t n, const SlotBinding& b) noexcept {
		if (n >= bindings_.size() || (b.live && b.slot != n)) {
			return false;
		}
		bindings_[n] = b;
		return true;
	}
	void clearSlotBindings() noexcept { bindings_.fill(SlotBinding {}); }
	/// EVERY BINDING TO DEVICE `id` DROPPED — a removed device drives nothing. Returns how many.
	size_t dropSlotBindingsOf(DeviceInstanceId id) noexcept {
		size_t dropped = 0;
		for (SlotBinding& b : bindings_) {
			if (b.live && b.device == id) {
				b = SlotBinding {};
				++dropped;
			}
		}
		return dropped;
	}

	/// PUT `row` AT THE END OF `chain`. Refused — false, nothing written — for a chain out of range, a
	/// full chain, a row under no id, or an id this state already holds: one device, in one place.
	bool append(size_t chain, const DeviceState& row) noexcept {
		if (chain >= chainCount() || length_[chain] >= capacity_ || !row.live() || holds(row.instanceId)) {
			return false;
		}
		row_[chain * capacity_ + length_[chain]++] = row;
		return true;
	}

	/// PUT `row` AT `position`, the devices at and after it moving up one — what an insert into a chain is.
	bool insert(size_t chain, size_t position, const DeviceState& row) noexcept {
		if (chain >= chainCount() || position > length(chain) || length_[chain] >= capacity_ || !row.live()
		    || holds(row.instanceId)) {
			return false;
		}
		DeviceState* const first = &row_[chain * capacity_];
		std::move_backward(first + position, first + length_[chain], first + length_[chain] + 1);
		first[position] = row;
		++length_[chain];
		return true;
	}

	/// TAKE THE DEVICE AT `position` OUT, the rest closing up.
	bool erase(size_t chain, size_t position) noexcept {
		if (position >= length(chain)) {
			return false;
		}
		DeviceState* const first = &row_[chain * capacity_];
		std::move(first + position + 1, first + length_[chain], first + position);
		first[--length_[chain]] = DeviceState {};
		return true;
	}

	/// EVERY CHAIN EMPTIED. The rows past a length are not read, so they are left as they are.
	void clear() noexcept {
		std::fill(length_.begin(), length_.end(), 0);
		orderCount_ = 0;
	}

	/// BECOME `other`, WHICH MUST HAVE THIS SHAPE — no allocation, so it may run on the audio thread.
	/// Refused (false, nothing written) for a different shape.
	bool assign(const ChainState& other) noexcept {
		if (other.capacity_ != capacity_ || other.chainCount() != chainCount()) {
			return false;
		}
		std::copy(other.row_.begin(), other.row_.end(), row_.begin());
		std::copy(other.length_.begin(), other.length_.end(), length_.begin());
		std::copy(other.order_.begin(), other.order_.end(), order_.begin());
		orderCount_ = other.orderCount_;
		bindings_ = other.bindings_;
		return true;
	}

	/// TRADE CONTENTS — buffers only: no allocation and no copy, so a drain may take a whole snapshot out of
	/// its staging with it (`PublishedChainState::adopt`).
	/// **INTO A STATE GROWN BIGGER, WITHOUT ALLOCATING** (unbounded devices). `assign` refuses another shape
	/// because a drain copies between two of the same; a GROWTH copies once, from the state it is retiring
	/// into the bigger one replacing it, and the rows have to move to the new stride. Refused (false,
	/// nothing written) for a state this one cannot hold: fewer chains, or less room per chain.
	bool assignGrown(const ChainState& other) noexcept {
		if (other.chainCount() != chainCount() || other.capacity_ > capacity_
		    || other.order_.size() > order_.size()) {
			return false;
		}
		for (size_t c = 0; c < chainCount(); ++c) {
			const std::span<const DeviceState> path = other.chain(c);
			std::copy(path.begin(), path.end(), row_.begin() + static_cast<std::ptrdiff_t>(c * capacity_));
			length_[c] = path.size();
		}
		std::copy(other.order_.begin(), other.order_.end(), order_.begin());
		orderCount_ = other.orderCount_;
		bindings_ = other.bindings_;
		return true;
	}

	friend void swap(ChainState& a, ChainState& b) noexcept {
		std::swap(a.capacity_, b.capacity_);
		a.row_.swap(b.row_);
		a.length_.swap(b.length_);
		a.order_.swap(b.order_);
		std::swap(a.orderCount_, b.orderCount_);
		// A FIXED ARRAY, SO THIS ONE IS A COPY — 64 small PODs, no allocation.
		std::swap(a.bindings_, b.bindings_);
	}

	/// THE SAME STATE — the same devices in the same order in every chain. Storage past a chain's end says
	/// nothing and is not compared.
	friend bool operator==(const ChainState& a, const ChainState& b) noexcept {
		if (a.capacity_ != b.capacity_ || a.chainCount() != b.chainCount()) {
			return false;
		}
		for (size_t c = 0; c < a.chainCount(); ++c) {
			const std::span<const DeviceState> pa = a.chain(c);
			const std::span<const DeviceState> pb = b.chain(c);
			if (!std::equal(pa.begin(), pa.end(), pb.begin(), pb.end())) {
				return false;
			}
		}
		// AND THE ORDER: two states that run their devices differently are different states.
		const std::span<const DeviceInstanceId> oa = a.order();
		const std::span<const DeviceInstanceId> ob = b.order();
		// AND THE BINDINGS: a state that drives a control from a host slot is not one that does not.
		return std::equal(oa.begin(), oa.end(), ob.begin(), ob.end()) && a.bindings_ == b.bindings_;
	}

private:
	size_t capacity_ = 0;
	/// `chains` rows of `capacity_` entries; row `c`'s first `length_[c]` are its devices, in order.
	std::vector<DeviceState> row_;
	std::vector<size_t> length_;
	std::vector<DeviceInstanceId> order_;
	size_t orderCount_ = 0;
	std::array<SlotBinding, kSlotBindingCapacity> bindings_ {};
};

} // namespace feathers::rack
