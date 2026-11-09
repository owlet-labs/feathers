/*
 * Copyright © 2026 Owlet Records
 * SPDX-License-Identifier: MIT
 *
 * This file is part of the Feathers Device SDK, which is MIT-licensed (see LICENSE in the SDK's
 * root) so that devices of any licence, open or closed, may build against it. Feathers itself,
 * which vendors this SDK, is GPL-3.0-or-later.
 */

#pragma once

#include "device/instance_id.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

// **A DEVICE'S EXTRA STATE, AS THE DEVICE ITSELF HAS CHANGED IT, CARRIED BACK TO THE RECORD** (backlog MX).
//
// A patch is saved from the message thread's record, and the record is written by hands. A device that CHANGES ITS
// OWN STATE as it plays — the step sequencer's Turing machine rewriting its steps — would otherwise save the pattern a
// hand last drew rather than the one that was heard. So such a device publishes its whole extra state
// (`ModOps::publishExtraState`), the host copies it into a snapshot here on the audio thread, and the message thread
// copies the snapshot into the record whenever it moves and once more before every save.
//
// **THE WHOLE STATE, NOT A QUEUE OF CHANGES**: a queue of fixed size loses state when it overflows, and a snapshot
// cannot — however many changes land between two reads, the read takes the last. A SEQLOCK, single writer (the audio
// thread), any number of readers, no allocation and no lock: the writer makes the sequence odd, stores, makes it even;
// a reader that saw an odd sequence or a sequence that moved under it reads nothing this time.
//
// **A HAND OUTRANKS A STALE SNAPSHOT** — `ModOps::extraStateBasis` names one float of the extra state that only a hand
// changes. The message thread applies a snapshot only while the record's basis float equals the snapshot's: a snapshot
// taken before a hand's edit carries the old basis and is dropped, and the device, which reads the hand's edit at its
// next tick, publishes again from it.

namespace feathers::device {

/// THE FLOATS ONE SNAPSHOT HOLDS — a device's whole row at most (`kMaxDeviceFields`).
inline constexpr size_t kMaxExtraStateFloats = 128;

/// **A SERIAL NO TWO PUBLISHES SHARE**, across every device in the process — so a device rebuilt under the same id (a
/// patch load) can never publish a serial the message thread has already consumed from its predecessor. Lock-free.
inline uint64_t nextExtraStateSerial() noexcept {
	static std::atomic<uint64_t> serial { 0 };
	return serial.fetch_add(1, std::memory_order_relaxed) + 1;
}

/// **A FRESH BASIS** (`ModOps::extraStateBasis`): what a hand's edit and a load write, never what a device publishes.
/// A negative whole number below 2^24, so a float holds it exactly; distinct from every other the process issues until
/// the counter wraps 2^24.
inline float freshExtraStateBasis() noexcept {
	return -static_cast<float>((nextExtraStateSerial() & 0xFFFFFFu) + 1u);
}

class ExtraStateSnapshot {
public:
	/// THE AUDIO THREAD, after the device ticked: store `n` floats under `serial` for `device`, unless that is already
	/// what the snapshot holds. Returns whether it stored.
	bool publish(DeviceInstanceId device, uint64_t serial, const float* values, size_t n) noexcept {
		if (serial == 0 || values == nullptr) {
			return false;
		}
		if (serial_.load(std::memory_order_relaxed) == serial && device_.load(std::memory_order_relaxed) == device) {
			return false;
		}
		const uint32_t s = sequence_.load(std::memory_order_relaxed);
		sequence_.store(s + 1, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_release);
		const size_t count = n < kMaxExtraStateFloats ? n : kMaxExtraStateFloats;
		for (size_t i = 0; i < count; ++i) {
			values_[i].store(values[i], std::memory_order_relaxed);
		}
		count_.store(static_cast<uint32_t>(count), std::memory_order_relaxed);
		device_.store(device, std::memory_order_relaxed);
		serial_.store(serial, std::memory_order_relaxed);
		sequence_.store(s + 2, std::memory_order_release);
		return true;
	}

	/// What a read hands back: nothing published (`serial` 0), or one whole publish.
	struct Read {
		DeviceInstanceId device = kNoInstance;
		uint64_t serial = 0;
		size_t count = 0;
		std::array<float, kMaxExtraStateFloats> values {};
	};

	/// ANY THREAD. False when nothing whole could be read (never published, or a publish landed under every try).
	bool read(Read* out) const noexcept {
		for (int attempt = 0; attempt < 4; ++attempt) {
			const uint32_t before = sequence_.load(std::memory_order_acquire);
			if ((before & 1u) != 0) {
				continue;
			}
			out->serial = serial_.load(std::memory_order_relaxed);
			out->device = device_.load(std::memory_order_relaxed);
			out->count = count_.load(std::memory_order_relaxed);
			for (size_t i = 0; i < out->count && i < kMaxExtraStateFloats; ++i) {
				out->values[i] = values_[i].load(std::memory_order_relaxed);
			}
			std::atomic_thread_fence(std::memory_order_acquire);
			if (sequence_.load(std::memory_order_relaxed) == before) {
				return out->serial != 0;
			}
		}
		return false;
	}

private:
	std::atomic<uint32_t> sequence_ { 0 };
	std::atomic<uint64_t> serial_ { 0 };
	std::atomic<DeviceInstanceId> device_ { kNoInstance };
	std::atomic<uint32_t> count_ { 0 };
	std::array<std::atomic<float>, kMaxExtraStateFloats> values_ {};
};

} // namespace feathers::device
