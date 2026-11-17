/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace feathers {

/// A KEY THE EDITOR PRESSES (a drum pad's preview button), on its way to the audio thread. A note, not a parameter: it is
/// never stored, never modulated, and reaches the synth by the road a host note takes.
struct UiNoteEvent {
	int16_t key = 0;
	float velocity = 0.0f; ///< 0..1, CLAP's
	bool on = false;
};

/// The note id a UI-originated note carries into the synth: INT32_MIN + 1. Host ids are zero and up, -1 is "none" (MIDI, the synth's
/// own lane notes), and INT32_MIN is the synth's `kNoHostNote`; this is none of them, so a preview can never be mistaken for, or end,
/// a host's note.
inline constexpr int32_t kUiNoteId = INT32_MIN + 1;

/// The note id a PAD STRIKE carries into a rack: INT32_MIN + 2, none of the above. Unlike a preview it is a note the rack routes and passes
/// downstream (so it is a rack's own, not a device's audition), and like a preview it never leaves the plugin for the host.
inline constexpr int32_t kPadNoteId = INT32_MIN + 2;

/// **A FIXED-CAPACITY LOCK-FREE SINGLE-PRODUCER SINGLE-CONSUMER RING** of editor notes: the editor's thread pushes, the audio thread
/// pops at the start of a block. Two plain indices and no allocation, lock or syscall on either side; a full ring DROPS the event and
/// counts it. (`UiParamQueue` has the same discipline for parameter moves; it is not generic, so this is its twin for notes.)
class UiNoteRing {
public:
	static constexpr size_t kCapacity = 64; ///< a power of two: the wrap is a mask

	bool push(const UiNoteEvent& event) noexcept {
		const size_t write = write_.load(std::memory_order_relaxed);
		const size_t read = read_.load(std::memory_order_acquire);
		if (write - read >= kCapacity) {
			dropped_.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		ring_[write & (kCapacity - 1)] = event;
		write_.store(write + 1, std::memory_order_release);
		return true;
	}
	bool pop(UiNoteEvent& out) noexcept {
		const size_t read = read_.load(std::memory_order_relaxed);
		if (write_.load(std::memory_order_acquire) == read) {
			return false;
		}
		out = ring_[read & (kCapacity - 1)];
		read_.store(read + 1, std::memory_order_release);
		return true;
	}
	[[nodiscard]] bool empty() const noexcept { return write_.load(std::memory_order_acquire) == read_.load(std::memory_order_acquire); }
	[[nodiscard]] uint32_t droppedCount() const noexcept { return dropped_.load(std::memory_order_relaxed); }

private:
	std::array<UiNoteEvent, kCapacity> ring_ {};
	std::atomic<size_t> write_ { 0 };
	std::atomic<size_t> read_ { 0 };
	std::atomic<uint32_t> dropped_ { 0 };
};

} // namespace feathers
