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

#include "device/instance_id.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace feathers::rack {

/// **A RISE NO BLOCK HAS READ IS NOT UNDONE BY A FALL** (2026-11-07: Pattern's Go pressed and released between two
/// blocks was LOST). A hand's writes reach the engine as SNAPSHOTS of the record, one adopted per block, so a field that
/// goes up and comes back down between two drains is never seen up: a momentary press quicker than a block, or a toggle
/// clicked on and off as fast. Every device that acts on a rise (Go, a trigger, a gate) lost it.
///
/// So a write that takes a field from at or above the midpoint to below it, while the publish that carried the field's
/// rise is not yet adopted, is HELD here — the record keeps the field up — and LANDED by the bridge's message-thread
/// visit once the engine has adopted that publish (the drain hands the row to the block that follows it). A further
/// write to a held field replaces the held value. The publish SEQUENCE is the press's count: the fall waits for the
/// engine to have counted the rise. Ordinary writes (no rise in flight) pass untouched, so a drag pays nothing.
///
/// MESSAGE THREAD ONLY. It allocates, and a bridge's writes are all on that thread.
class HeldFalls {
public:
	static constexpr float kMidpoint = 0.5f;

	/// ONLY A MOMENTARY'S RELEASE ASKS (`DeviceHost::releaseField`): a preset, an undo or a load writes through `setField`,
	/// which records rises (`wrote`) and drops a held release (`drop`) but never holds.
	/// Asked BEFORE writing `value` over `before`: true means DO NOT WRITE — the write is held, to be handed back by
	/// `land`. `adopted` is the last publish the engine has adopted.
	[[nodiscard]] bool hold(DeviceInstanceId device, size_t control, float before, float value, uint32_t adopted) {
		for (Held& h : held_) {
			if (h.device == device && h.control == control) {
				h.value = value; // the latest word wins, once the rise is read
				return true;
			}
		}
		if (!(before >= kMidpoint && value < kMidpoint)) {
			return false;
		}
		for (const Rise& r : rises_) {
			if (r.device == device && r.control == control && !reached(adopted, r.sequence)) {
				held_.push_back({ device, control, value, r.sequence });
				return true;
			}
		}
		return false;
	}

	/// Told AFTER a write of `value` over `before` was published as `sequence`: a rise past the midpoint is remembered.
	void wrote(DeviceInstanceId device, size_t control, float before, float value, uint32_t sequence) {
		if (!(before < kMidpoint && value >= kMidpoint)) {
			return;
		}
		for (Rise& r : rises_) {
			if (r.device == device && r.control == control) {
				r.sequence = sequence;
				return;
			}
		}
		rises_.push_back({ device, control, sequence });
	}

	/// Every held write whose rise `adopted` has reached, handed to `write(device, control, value)` — which writes it as a
	/// hand would — and forgotten first, so that write passes. Rises already read are forgotten too.
	template <class Write>
	void land(uint32_t adopted, Write&& write) {
		std::vector<Held> due;
		for (size_t i = 0; i < held_.size();) {
			if (reached(adopted, held_[i].sequence)) {
				due.push_back(held_[i]);
				held_[i] = held_.back();
				held_.pop_back();
			}
			else {
				++i;
			}
		}
		std::erase_if(rises_, [&](const Rise& r) { return reached(adopted, r.sequence); });
		for (const Held& h : due) {
			write(h.device, h.control, h.value);
		}
	}

	/// A PLAIN WRITE TO THE FIELD (a preset, an undo, a load) supersedes a held release: it is dropped, not landed over it.
	void drop(DeviceInstanceId device, size_t control) {
		std::erase_if(held_, [&](const Held& h) { return h.device == device && h.control == control; });
	}
	[[nodiscard]] bool empty() const noexcept { return held_.empty(); }
	/// A device gone (or a whole record replaced): its rises and held writes go with it.
	void forget() {
		held_.clear();
		rises_.clear();
	}

private:
	struct Rise {
		DeviceInstanceId device;
		size_t control;
		uint32_t sequence;
	};
	struct Held {
		DeviceInstanceId device;
		size_t control;
		float value;
		uint32_t sequence;
	};
	/// Wrap-safe: `adopted` at or past `sequence`.
	[[nodiscard]] static bool reached(uint32_t adopted, uint32_t sequence) noexcept {
		return static_cast<int32_t>(adopted - sequence) >= 0;
	}
	std::vector<Rise> rises_;
	std::vector<Held> held_;
};

} // namespace feathers::rack
