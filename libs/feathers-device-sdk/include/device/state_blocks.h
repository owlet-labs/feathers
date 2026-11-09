/*
 * Copyright © 2026 Owlet Records
 * SPDX-License-Identifier: MIT
 *
 * This file is part of the Feathers Device SDK, which is MIT-licensed (see LICENSE in the SDK's
 * root) so that devices of any licence, open or closed, may build against it. Feathers itself,
 * which vendors this SDK, is GPL-3.0-or-later.
 */

#pragma once

#include "device/extra_state.h"

#include <array>
#include <atomic>
#include <cassert>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// **STATE A DEVICE ARRIVED AT ITSELF, IN KEYED BLOCKS, CARRIED TO THE PATCH AND BACK** (backlog NM step 1).
//
// CLAUDE.md, "RANDOM AND CAPTURED STATE PERSISTS WITH THE PATCH": a value reached by chance or captured at a moment is
// saved and restored. The row's extra state (`Traits::extraState`, `ModOps::publishExtraState`, backlog MX) is the road
// for state a HAND writes and the device may rewrite; it shares the 128-float row with the controls and has one key.
// This is the road for state only the DEVICE writes — a held sample, a stream's position, a frozen envelope:
//
// - **SEVERAL BLOCKS PER DEVICE, EACH UNDER ITS OWN KEY** (`StateBlockDesc::key`, in the ledger as `block`), each SIZED
//   BY NEED — not the row, so a block may be larger than the row (Spectral's frozen envelope is about 260 floats).
// - **THE DEVICE OWNS A SNAPSHOT PER BLOCK** (`BlockSnapshot<N>`, inside its own storage, so it is allocated when the
//   instance is built on the message thread and never again). The AUDIO THREAD publishes into it after it changes the
//   state; ANY thread reads it (`StateOps::read`) under its seqlock — never the device's working fields, which the audio
//   thread is writing (backlog NM prerequisite 5).
// - **HARVESTED BEFORE A SAVE**, from the running instance (`rack::patchOfRecord`, `rack::patchOf`). Nothing republishes
//   the chains when a block moves, so a block that moves every tick costs nothing between saves (prerequisite 4). A
//   block that something on the message thread wants current between saves says `harvestOnTick`, and the bridges'
//   harvest copies it into the host's store whenever its serial moves.
// - **RESTORED INTO A FRESH INSTANCE, BEFORE IT REACHES THE AUDIO THREAD.** A load stores the blocks beside the device's
//   body (`ContainerBodies::storeBlocks`), which moves the device's stored generation; the converger therefore builds
//   the device again, hands the new instance its blocks (`StateOps::restore`, message thread, nothing running it yet)
//   and posts it, and the drain swaps it in. No audio-thread allocation and no copy on the audio thread: the instance
//   that arrives already holds what was saved (prerequisite 7's "only once" is then true by construction — a hand's
//   edit never restores anything, because only a load stores blocks).
//
// **AUDIO BUFFERS DO NOT TRAVEL HERE** (audio is not persisted unless that is added deliberately): SKLITTER's
// loop and a Delay self-loop are not blocks. And **what can be DERIVED is not stored** — a value that is a function of
// the seed and the song position is reproduced by construction, not saved.
//
// **EXACT INTEGERS IN FLOATS STOP AT 2^24** (prerequisite 6), and a patch stores floats as decimal text: so a block is
// written in 16-bit WORDS (`BlockWriter`), each an integer a float holds exactly and the text round-trips exactly. A
// 64-bit RNG state is four of them; a double is four; a float is two, by its bits.

namespace feathers::device {

/// ONE KEYED BLOCK A DEVICE DECLARES.
struct StateBlockDesc {
	/// `[a-z0-9_.]`, distinct from every control's key and from `Traits::extraStateKey`. A key names a MEANING: a block
	/// whose layout changes takes a NEW key (and the old one goes to `Traits::retiredKeys`), so an older patch's block
	/// is dropped rather than read wrong.
	const char* key = nullptr;
	/// Its length in floats — what `read` writes and `restore` takes. A stored block of any other length is refused.
	uint32_t floats = 0;
	/// Also copied into the host's store whenever it moves, not only before a save. False is the default and the right
	/// answer for state that moves every tick.
	bool harvestOnTick = false;
	/// **A HAND'S BLOCK** (`StateOps::apply`): the state is written by the USER on the message thread (a drawn curve), not by
	/// the audio thread, so it travels the other way — `DeviceHost::setBlock` writes it into the running instance
	/// (`apply`, a seqlock'd `HandBlock`), the host's store and the undo history together, and the audio thread PULLS it
	/// (`AudioOps::pullHandBlocks`). It saves and restores like any block.
	bool handEdited = false;
};

/// **HOW A DEVICE'S BLOCKS ARE READ AND RESTORED**, or null on the `Descriptor` for a device with none. Independent of
/// `audio`, `mod` and `notes`, so an effect and a modulator take one road.
struct StateOps {
	std::span<const StateBlockDesc> blocks {};
	/// ANY THREAD: block `index` as the device last published it, into `out` (`blocks[index].floats`, at most
	/// `capacity`), and the serial it was published under — 0 when the device has published nothing yet (a fresh
	/// instance, which then has nothing of its own to say: the host keeps what it stored).
	uint64_t (*read)(const void* storage, size_t index, float* out, size_t capacity) = nullptr;
	/// MESSAGE THREAD, on an instance nothing is running yet (built for a load, or rebuilt for a restart): take block
	/// `index` from `in` (exactly `blocks[index].floats`). False refuses it — malformed, or from a layout this build does
	/// not read — and the device plays as freshly built.
	bool (*restore)(void* storage, size_t index, const float* in, size_t n) = nullptr;
	/// **MESSAGE THREAD ONLY, ON A RUNNING INSTANCE, FOR A `handEdited` BLOCK**: take the hand's new value for block
	/// `index` (`blocks[index].floats` of them) into the instance's `HandBlock`, whole, without stopping anything. THE ONE
	/// WRITER IS THE MESSAGE THREAD (asserted in `HandBlock::write`); the audio thread reads it by `AudioOps::pullHandBlocks`.
	/// False refuses it (a bad length or index, or a block that is not a hand's). `read` then answers with the hand's
	/// latest, so a save and the editor see it.
	bool (*apply)(void* storage, size_t index, const float* in, size_t n) = nullptr;
};

/// THE INDEX OF THE BLOCK `key` NAMES, or `blocks.size()` (0 for no ops) for none.
inline size_t blockIndexOf(const StateOps* ops, std::string_view key) noexcept {
	if (ops == nullptr) {
		return 0;
	}
	for (size_t i = 0; i < ops->blocks.size(); ++i) {
		if (ops->blocks[i].key != nullptr && key == ops->blocks[i].key) {
			return i;
		}
	}
	return ops->blocks.size();
}

/// **ONE BLOCK'S SNAPSHOT**, inside the device's storage. One writer (the audio thread), any number of readers, no lock
/// and no allocation: `ExtraStateSnapshot`'s seqlock, sized at compile time by the block.
template <size_t N> class BlockSnapshot {
public:
	/// AUDIO THREAD (or the message thread before the instance is posted): store `n` floats under a fresh serial.
	void publish(const float* values, size_t n) noexcept {
		const uint32_t s = sequence_.load(std::memory_order_relaxed);
		sequence_.store(s + 1, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_release);
		for (size_t i = 0; i < N; ++i) {
			values_[i].store(i < n ? values[i] : 0.0f, std::memory_order_relaxed);
		}
		serial_.store(nextExtraStateSerial(), std::memory_order_relaxed);
		sequence_.store(s + 2, std::memory_order_release);
	}
	/// ANY THREAD: the last publish into `out` and its serial; 0 when nothing whole was published (never, or a publish
	/// landed under every try).
	uint64_t read(float* out, size_t capacity) const noexcept {
		for (int attempt = 0; attempt < 4; ++attempt) {
			const uint32_t before = sequence_.load(std::memory_order_acquire);
			if ((before & 1u) != 0) {
				continue;
			}
			const uint64_t serial = serial_.load(std::memory_order_relaxed);
			for (size_t i = 0; i < N && i < capacity; ++i) {
				out[i] = values_[i].load(std::memory_order_relaxed);
			}
			std::atomic_thread_fence(std::memory_order_acquire);
			if (sequence_.load(std::memory_order_relaxed) == before) {
				return serial;
			}
		}
		return 0;
	}
	/// **ONE ATTEMPT, NO RETRY** (the audio thread's read of a hand's block, `HandBlock`): 0 when a publish is in flight.
	uint64_t tryRead(float* out, size_t capacity) const noexcept {
		const uint32_t before = sequence_.load(std::memory_order_acquire);
		if ((before & 1u) != 0) {
			return 0;
		}
		const uint64_t serial = serial_.load(std::memory_order_relaxed);
		for (size_t i = 0; i < N && i < capacity; ++i) {
			out[i] = values_[i].load(std::memory_order_relaxed);
		}
		std::atomic_thread_fence(std::memory_order_acquire);
		return sequence_.load(std::memory_order_relaxed) == before ? serial : 0;
	}
	/// **`tryRead`, WITHOUT THE COPY WHEN THE SERIAL IS ONE THE CALLER ALREADY HOLDS** (the idle-overhead pass):
	/// `known` back, `out` untouched. A serial is never reissued (`nextExtraStateSerial`), so the values under `known` are the
	/// ones the caller copied when it took it; a write in flight that has not stored its serial yet is read next time, as a
	/// torn `tryRead` would leave it. Otherwise exactly `tryRead`. MEASURED: a Pattern pulled its two hand blocks (3,600
	/// words) word by word at every render chunk, about 35 times a 1024-frame block, to learn that nothing had moved.
	uint64_t tryReadNewer(float* out, size_t capacity, uint64_t known) const noexcept {
		const uint32_t before = sequence_.load(std::memory_order_acquire);
		if ((before & 1u) != 0) {
			return 0;
		}
		// `known` 0 included: nothing ever written reads as serial 0, which `tryRead` would hand back after copying the zeros.
		if (serial_.load(std::memory_order_relaxed) == known) {
			return known;
		}
		return tryRead(out, capacity);
	}

private:
	std::atomic<uint32_t> sequence_ { 0 };
	std::atomic<uint64_t> serial_ { 0 };
	std::array<std::atomic<float>, N> values_ {};
};

/// **A HAND'S BLOCK, INSIDE THE DEVICE'S STORAGE** — `BlockSnapshot`'s mirror: where that one is written by the audio thread
/// and read by anyone, this one is written by the MESSAGE THREAD (the one writer, asserted) and READ BY THE AUDIO THREAD,
/// which must never wait for it. The seqlock and its word-by-word relaxed atomic copy are `BlockSnapshot`'s own, so there is
/// no plain `memcpy` under it (a data race by the standard); the sequence counter carries the acquire/release.
template <size_t N> class HandBlock {
public:
	/// MESSAGE THREAD, the one writer: store `n` floats whole under a fresh serial. No allocation. A second writer in at
	/// once is a bug and trips the assert.
	void write(const float* values, size_t n) noexcept {
		const bool busy = writing_.exchange(true, std::memory_order_acquire);
		assert(!busy && "HandBlock has one writer: the message thread");
		(void)busy;
		cell_.publish(values, n);
		writing_.store(false, std::memory_order_release);
	}
	/// ANY NON-AUDIO THREAD: the latest whole write and its serial (0 when nothing was ever written). Retries a few times.
	uint64_t read(float* out, size_t capacity) const noexcept { return cell_.read(out, capacity); }
	/// **THE AUDIO THREAD'S READ: ONE ATTEMPT, NEVER A SPIN.** The serial and the block when the read was whole, 0 when a write
	/// was in flight (or none ever landed): the caller keeps its last good copy and asks again next block.
	uint64_t tryRead(float* out, size_t capacity) const noexcept { return cell_.tryRead(out, capacity); }
	/// `tryRead`, answering `known` without copying when that is still the serial (`BlockSnapshot::tryReadNewer`).
	uint64_t tryReadNewer(float* out, size_t capacity, uint64_t known) const noexcept {
		return cell_.tryReadNewer(out, capacity, known);
	}

private:
	BlockSnapshot<N> cell_;
	std::atomic<bool> writing_ { false };
};

/// **THE AUDIO SIDE'S COPY OF A HAND'S BLOCK, kept by whoever runs the device**: the last whole value pulled and the serial it
/// came under. `pull` is cheap when nothing moved (the sequence and the serial, two atomic loads, and no copy: `tryReadNewer`),
/// bounded, allocation-free, and on a torn read changes nothing.
template <size_t N> struct HandBlockCopy {
	std::array<float, N> values {};
	uint64_t serial = 0;
	/// True when `values` moved. `scratch` is the caller's N floats (it keeps this struct's good copy untouched on a torn read).
	bool pull(const HandBlock<N>& source, std::array<float, N>& scratch) noexcept {
		const uint64_t s = source.tryReadNewer(scratch.data(), N, serial);
		if (s == 0 || s == serial) {
			return false;
		}
		values = scratch;
		serial = s;
		return true;
	}
};

/// **A BLOCK WRITTEN IN 16-BIT WORDS** — see the banner. Writes past the end are dropped and `ok()` says so.
class BlockWriter {
public:
	BlockWriter(float* out, size_t capacity) noexcept : out_(out), capacity_(capacity) {}
	void u16(uint32_t v) noexcept {
		if (n_ < capacity_) {
			out_[n_] = static_cast<float>(v & 0xFFFFu);
		}
		++n_;
	}
	void u32(uint32_t v) noexcept {
		u16(v);
		u16(v >> 16);
	}
	void u64(uint64_t v) noexcept {
		u32(static_cast<uint32_t>(v));
		u32(static_cast<uint32_t>(v >> 32));
	}
	void f32(float v) noexcept { u32(std::bit_cast<uint32_t>(v)); }
	void f64(double v) noexcept { u64(std::bit_cast<uint64_t>(v)); }
	void flag(bool v) noexcept { u16(v ? 1u : 0u); }
	[[nodiscard]] size_t size() const noexcept { return n_; }
	[[nodiscard]] bool ok() const noexcept { return n_ <= capacity_; }

private:
	float* out_;
	size_t capacity_;
	size_t n_ = 0;
};

/// **THE SAME WORDS, READ BACK.** A word that is not a whole number in [0, 65535] — a hand-edited or damaged file —
/// fails the read (`ok()`), and the caller refuses the block.
class BlockReader {
public:
	BlockReader(const float* in, size_t n) noexcept : in_(in), n_(n) {}
	uint32_t u16() noexcept {
		if (at_ >= n_) {
			bad_ = true;
			return 0;
		}
		const float f = in_[at_++];
		if (!(f >= 0.0f && f <= 65535.0f) || static_cast<float>(static_cast<uint32_t>(f)) != f) {
			bad_ = true;
			return 0;
		}
		return static_cast<uint32_t>(f);
	}
	uint32_t u32() noexcept {
		const uint32_t lo = u16();
		return lo | (u16() << 16);
	}
	uint64_t u64() noexcept {
		const uint64_t lo = u32();
		return lo | (static_cast<uint64_t>(u32()) << 32);
	}
	float f32() noexcept { return std::bit_cast<float>(u32()); }
	double f64() noexcept { return std::bit_cast<double>(u64()); }
	bool flag() noexcept { return u16() != 0; }
	/// Every word whole, and exactly `n` of them read.
	[[nodiscard]] bool ok() const noexcept { return !bad_ && at_ == n_; }

private:
	const float* in_;
	size_t n_;
	size_t at_ = 0;
	bool bad_ = false;
};

/// **A RANDOM-EVENT STREAM'S POSITION** (`dsp::RandomEvents::Position`, or anything with its five fields) in
/// `kEventWords` words — three devices persist one, so it is written one way. A template so this header names no dsp.
inline constexpr size_t kEventWords = 4 + 4 * 3 + 1;
template <class Position> void putEvents(BlockWriter& w, const Position& p) noexcept {
	w.u64(p.state);
	w.f64(p.interval);
	w.f64(p.untilEvent);
	w.f64(p.lastOffset);
	w.flag(p.primed);
}
template <class Position> Position takeEvents(BlockReader& r) noexcept {
	Position p {};
	p.state = r.u64();
	p.interval = r.f64();
	p.untilEvent = r.f64();
	p.lastOffset = r.f64();
	p.primed = r.flag();
	return p;
}

} // namespace feathers::device
