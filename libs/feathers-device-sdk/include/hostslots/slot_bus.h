/*
 * Copyright © 2026 Owlet Records
 * SPDX-License-Identifier: MIT
 *
 * This file is part of the Feathers Device SDK, which is MIT-licensed (see LICENSE in the SDK's
 * root) so that devices of any licence, open or closed, may build against it. Feathers itself,
 * which vendors this SDK, is GPL-3.0-or-later.
 */

#pragma once

// THE HOST SLOTS' TWO LOCK-FREE ROADS (task T2).
//
// A host slot is a CLAP param mapped at run time to a device control. Its values cross threads in
// both directions, and neither direction may take a lock or allocate on the audio thread:
//
// - HOST -> ENGINE. Automation and host modulation arrive in `process` on the audio thread, and an
//   offline bounce has no main-thread cadence at all, so the engine reads them straight from
//   `SlotValueBus`. The main thread reads the same cells later to mirror the value into the record,
//   and takes the `hostWrote` bits to ANCHOR the slot (design §5: a slot the host wrote is never
//   recycled, because its lane still holds that id).
// - EDITOR -> HOST. A hand on a slotted control sends begin/set/end from the main thread; the host
//   only takes output events inside `process` or `flush`. `SlotOutbox` is the SPSC ring between the
//   two, plus the DEBT table that the core params already keep (`feathers_clap.cpp`, `ParamDebt`) for
//   when the host's output queue is full.
//
// Header-only and CLAP-free: the CLAP layer (`slot_host`, T6a; the drain is T6b's) hands `SlotOutbox::drain` a functor that
// tries `clap_output_events::try_push` and says whether it landed.
//
// THE CAPACITY IS A TEMPLATE ARGUMENT, growable by recompile. `kHostSlotCapacity` lives in
// `slot_pool.h` (T1), which this header must not depend on; `kSlotBusCapacity` is its twin, and
// whoever first includes both static_asserts they agree. The bitsets are arrays of 64-bit words sized
// by the capacity, so a pool that grows past 64 needs no change here.

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace feathers::hostslots {

// `kHostSlotCapacity`'s twin. `slot_host.h`, where `slot_pool.h` and this header first meet, static_asserts
// that the two agree.
inline constexpr size_t kSlotBusCapacity = 256;
/// One bit per slot, as TAKEN from a bus: a plain value, owned by whoever took it.
template <size_t Capacity>
struct SlotBits {
	static constexpr size_t kWords = (Capacity + 63) / 64;
	std::array<uint64_t, kWords> words {};

	bool test(size_t slot) const noexcept {
		return slot < Capacity && (words[slot / 64] >> (slot % 64) & 1u) != 0;
	}
	bool any() const noexcept {
		for (uint64_t w : words) {
			if (w != 0) {
				return true;
			}
		}
		return false;
	}
	size_t count() const noexcept {
		size_t n = 0;
		for (uint64_t w : words) {
			n += static_cast<size_t>(std::popcount(w));
		}
		return n;
	}
	/// Calls `f(slot)` for every set bit, lowest slot first.
	template <class F>
	void forEach(F&& f) const {
		for (size_t w = 0; w < kWords; ++w) {
			for (uint64_t bits = words[w]; bits != 0; bits &= bits - 1) {
				f(w * 64 + static_cast<size_t>(std::countr_zero(bits)));
			}
		}
	}
};

/// The host's side of every slot: the value it last wrote, the modulation it last sent, and a
/// generation that moves on every write, so a reader can tell "moved" from "same number again".
///
/// WRITERS. `onHostValue`/`onHostMod` are the audio thread's (a `CLAP_EVENT_PARAM_VALUE` / `_MOD` on a
/// slot id). `seed` is the main thread's, for a value that did NOT come from the host — a patch load or
/// an undo that the record mirror (§1.2) pushes back into the bus so the engine re-applies it after an
/// adopt. Both write `{value, gen + 1}` as one CAS, so neither loses the other's bump, and `seed`
/// against `onHostValue` on one slot is LAST WINS for the pair — the same answer a host and a hand racing
/// on a core param get. `seedIf` is the seed that LOSES to any write since the caller looked.
///
/// **ONE CELL IS ONE ATOMIC: `{value, gen}` PACKED INTO A LOCK-FREE 64-BIT WORD** (T6b review). The value
/// and the generation were two atomics, and a `seed` racing an `onHostValue` could leave the bus holding
/// the HOST's value under the generation the SEED returned — so the main thread believed the record
/// matched the bus while the engine applied something else, and the knob and the engine disagreed. Packed,
/// every write is a CAS on the pair and every reader takes one consistent snapshot (`load`): a value is
/// never seen under another write's generation.
template <size_t Capacity = kSlotBusCapacity>
class SlotValueBus {
public:
	static constexpr size_t kCapacity = Capacity;
	using Bits = SlotBits<Capacity>;

	/// One cell as read in ONE load: the value and the generation that wrote it.
	struct Snapshot {
		float value = 0.0f;
		uint32_t gen = 0;
		friend bool operator==(const Snapshot&, const Snapshot&) = default;
	};

	/// Audio thread. The host wrote `value` (DEVICE units — the CLAP layer denormalises) to `slot`.
	/// Sets the `hostWrote` bit, which is what anchors the slot on the main thread.
	void onHostValue(size_t slot, float value) noexcept {
		if (slot >= Capacity) {
			return;
		}
		(void)write(cells_[slot].packed, value);
		// AFTER the value and the generation: a main thread that takes this bit must find both.
		hostWrote_[slot / 64].fetch_or(bitOf(slot), std::memory_order_release);
	}

	/// Audio thread. A global `CLAP_EVENT_PARAM_MOD` on `slot`: an offset in NORMALISED units that the
	/// engine scales by the control's span (§1.2). No generation: the engine reads the offset every block,
	/// so there is no "moved" to detect.
	///
	/// **A NON-ZERO AMOUNT ANCHORS, AS A HOST WRITE DOES** (a modulation-only
	/// slot must not be left unanchored). A host modulator stays on the slot's id whatever the plugin does, so a slot it drives
	/// must not be recycled under it: the `hostWrote` bit is set, which the main thread folds into an anchor
	/// and into the slot's host mark. A zero amount is the modulator at rest (or removed) and sets nothing.
	void onHostMod(size_t slot, float mod) noexcept {
		if (slot >= Capacity) {
			return;
		}
		cells_[slot].mod.store(mod, std::memory_order_relaxed);
		if (mod != 0.0f) {
			hostWrote_[slot / 64].fetch_or(bitOf(slot), std::memory_order_release);
		}
	}

	/// Any thread. **THE HOST SENT SOMETHING ON `slot`'S ID THAT REACHED NO CONTROL** — a write on a slot that
	/// drives nothing (Empty, or mapped since the last visit). Nothing is written and nothing anchors, but the
	/// id is MARKED at the next visit (`SlotPool::markHost`): a lane is there, and the pool reuses a marked id
	/// last.
	void onHostTouched(size_t slot) noexcept {
		if (slot >= Capacity) {
			return;
		}
		hostMarked_[slot / 64].fetch_or(bitOf(slot), std::memory_order_release);
	}

	/// Any thread. A hand touched `slot` while the host was recording (design §5), which anchors it
	/// exactly as a host write does. Kept apart from `hostWrote` so the main thread can say which.
	void onRecTouched(size_t slot) noexcept {
		if (slot >= Capacity) {
			return;
		}
		recTouched_[slot / 64].fetch_or(bitOf(slot), std::memory_order_release);
	}

	/// Main thread. A value that did not come from the host (a load, an undo) goes into the bus so the
	/// engine picks it up. Moves the generation and sets NO bit: it must not anchor anything. Returns
	/// the generation it produced, which the record mirror takes as already committed — otherwise it
	/// would read its own seed as a host write to commit.
	uint32_t seed(size_t slot, float value) noexcept {
		if (slot >= Capacity) {
			return 0;
		}
		return write(cells_[slot].packed, value);
	}

	/// **MAIN THREAD. SEED ONLY IF THE CELL IS STILL EXACTLY `expected`** — one CAS from `expected` to
	/// `{value, expected.gen + 1}`. Returns the new generation, or 0 (nothing written) when anything wrote
	/// the cell since `expected` was loaded: the host's write then stands, and the caller's record copy is
	/// the stale one. This is what lets a road that rewrote the record seed the bus without ever putting
	/// the record's copy over a host write the mirror has not committed yet.
	uint32_t seedIf(size_t slot, Snapshot expected, float value) noexcept {
		if (slot >= Capacity) {
			return 0;
		}
		uint64_t want = pack(expected);
		const Snapshot next {value, nextGen(expected.gen)};
		return cells_[slot].packed.compare_exchange_strong(want, pack(next), std::memory_order_acq_rel,
		                                                   std::memory_order_acquire)
		           ? next.gen
		           : 0;
	}

	/// ANY THREAD. The cell's value and generation, taken in ONE load — the only read that may pair them.
	Snapshot load(size_t slot) const noexcept {
		return slot < Capacity ? unpack(cells_[slot].packed.load(std::memory_order_acquire)) : Snapshot {};
	}

	/// Main thread. Every slot the host wrote since the last take, each exactly once.
	Bits takeHostWrote() noexcept { return take(hostWrote_); }
	/// Main thread. Every slot touched while recording since the last take, each exactly once.
	Bits takeRecTouched() noexcept { return take(recTouched_); }
	/// Main thread. Every slot `onHostTouched` named since the last take, each exactly once.
	Bits takeHostMarked() noexcept { return take(hostMarked_); }

	/// Each half of `load`, for a reader that needs only one. `genOf` then `valueOf` is TWO loads and may
	/// pair a value with another write's generation; a reader that needs both takes `load`.
	uint32_t genOf(size_t slot) const noexcept { return load(slot).gen; }
	float valueOf(size_t slot) const noexcept { return load(slot).value; }
	float modOf(size_t slot) const noexcept {
		return slot < Capacity ? cells_[slot].mod.load(std::memory_order_relaxed) : 0.0f;
	}

private:
	static constexpr size_t kWords = Bits::kWords;

	struct Cell {
		/// `{value, gen}` — the value's bits low, the generation high (`pack`).
		std::atomic<uint64_t> packed {0};
		std::atomic<float> mod {0.0f};
	};

	static uint64_t pack(Snapshot s) noexcept {
		return static_cast<uint64_t>(s.gen) << 32 | std::bit_cast<uint32_t>(s.value);
	}
	static Snapshot unpack(uint64_t w) noexcept {
		return Snapshot {std::bit_cast<float>(static_cast<uint32_t>(w)), static_cast<uint32_t>(w >> 32)};
	}
	/// The generation after `g`. It skips 0 on the wrap: 0 means "never written or seeded" to the engine,
	/// which never applies it.
	static uint32_t nextGen(uint32_t g) noexcept { return g + 1 == 0 ? 1 : g + 1; }

	/// `{value, gen + 1}` over whatever the cell holds, as one CAS loop. Returns the generation written.
	static uint32_t write(std::atomic<uint64_t>& cell, float value) noexcept {
		uint64_t seen = cell.load(std::memory_order_relaxed);
		Snapshot next;
		do {
			next = Snapshot {value, nextGen(unpack(seen).gen)};
		} while (!cell.compare_exchange_weak(seen, pack(next), std::memory_order_acq_rel, std::memory_order_relaxed));
		return next.gen;
	}

	static constexpr uint64_t bitOf(size_t slot) noexcept { return uint64_t {1} << (slot % 64); }

	/// One exchange per word, so a bit set during the take lands in THIS take or the next one and
	/// never in neither: fetch_or and exchange on one atomic are totally ordered.
	static Bits take(std::array<std::atomic<uint64_t>, kWords>& from) noexcept {
		Bits out;
		for (size_t w = 0; w < kWords; ++w) {
			out.words[w] = from[w].exchange(0, std::memory_order_acq_rel);
		}
		return out;
	}

	std::array<Cell, Capacity> cells_ {};
	std::array<std::atomic<uint64_t>, kWords> hostWrote_ {};
	std::array<std::atomic<uint64_t>, kWords> recTouched_ {};
	std::array<std::atomic<uint64_t>, kWords> hostMarked_ {};

	static_assert(std::atomic<float>::is_always_lock_free);
	static_assert(std::atomic<uint64_t>::is_always_lock_free,
	              "a bus cell is one {value, gen} word; it must be lock-free or the audio thread may block");
	static_assert(sizeof(float) == sizeof(uint32_t));
};

/// What the editor tells the host about a slot, in the order a host expects: a gesture begin, values,
/// a gesture end.
enum class SlotEventKind : uint8_t { Begin, Value, End };

struct SlotOutboxTestAccess;

/// The editor-to-host road for slots: the main thread produces, `process`/`flush` consumes.
///
/// TWO PLACES AN EVENT CAN FAIL TO FIT, and both follow the core params' rules
/// (`feathers_clap.cpp`, `consumeEditorEvents` / `ParamDebt`):
///
/// 1. THE HOST'S output queue refuses it at `drain`. A Value is then OWED, once per slot however many
///    were refused, and re-read FRESH when paid, so a debt paid after several more moves carries the
///    current value rather than the one that failed. A refused End is owed too: it is safe alone (a
///    host that never saw the begin ignores it) and it is the only thing that closes an undo grouping
///    that did open. A refused Begin is NOT retried: re-sent later it would open a grouping around
///    edits that are not the gesture's, whose own end has already gone by.
/// 2. THIS RING is full at `push` (the audio thread has not drained for a while: a stalled host, a
///    suspended plugin). The same three rules apply one step earlier — a Value and an End become debts
///    that the next drain pays, a Begin is dropped — so the ring's size bounds latency, never
///    correctness.
///
/// The debt table itself belongs to the CONSUMER alone; the producer reaches it only through the
/// overflow bits, which the consumer folds in at the next drain.
///
/// ACCEPTED, and the same as the core params: a Value owed from one gesture may be paid inside the
/// NEXT gesture on that slot. It carries the slot's current value, so the host ends up holding the
/// right number; what it costs is that the host may file one value under the later gesture.
template <size_t Capacity = kSlotBusCapacity, size_t RingSize = 256>
class SlotOutbox {
	/// The tests' way to set up a state only a cross-thread interleaving otherwise produces (the
	/// parked-End race in `push`). Defined in `tests/test_slot_bus.cpp`; nothing else names it.
	friend struct SlotOutboxTestAccess;

public:
	static_assert(RingSize >= 2 && (RingSize & (RingSize - 1)) == 0, "RingSize must be a power of two");
	static_assert(Capacity <= 0xffff, "a slot index travels as uint16_t");

	static constexpr size_t kCapacity = Capacity;

	struct Event {
		SlotEventKind kind = SlotEventKind::Value;
		uint16_t slot = 0;
		float value01 = 0.0f;
	};

	//----------------------------------------------------------------------------------------//
	// PRODUCER (main thread)                                                                 //
	//----------------------------------------------------------------------------------------//

	/// Queue one event. True if it entered the ring; false if the ring was full, in which case a Value
	/// or an End is owed (and will still reach the host) and a Begin is gone. A Begin is also gone,
	/// ring or no ring, while its slot's End is still parked in an overflow bit (see below).
	bool push(SlotEventKind kind, size_t slot, float value01 = 0.0f) noexcept {
		if (slot >= Capacity) {
			return false;
		}
		if (kind == SlotEventKind::Value) {
			// BEFORE the ring: a debt is paid from here, so it must never be older than the ring.
			latest01_[slot].store(value01, std::memory_order_relaxed);
		}
		// A BEGIN WAITS BEHIND ITS SLOT'S PARKED END, or is dropped. The race it closes: this thread
		// reads a stale head, finds the ring full and parks End(g1) as an overflow bit, but the bit
		// lands AFTER a drain has already published the new head and folded — so the ring has room,
		// Begin(g2) goes in, and the NEXT drain sends Begin(g2), then folds the old End and pays it at
		// once, closing g2 at its first event. Only this thread sets the bit, so reading it 0 means
		// the consumer's exchange has already run: `owed_` holds that End before any drain can take
		// this Begin, and the Begin arm below sends the End first. Read 1, the fold has not happened,
		// and the Begin is dropped — an outcome a Begin already has (the class comment). Moving the
		// fold ahead of the ring in `drain` instead would be wrong: an older Value still in the ring
		// would then clear a newer, overflowed Value debt.
		if (kind == SlotEventKind::Begin
			&& (overflowEnd_[slot / 64].load(std::memory_order_acquire) & bitOf(slot)) != 0) {
			return false;
		}
		const size_t tail = tail_.load(std::memory_order_relaxed);
		if (tail - head_.load(std::memory_order_acquire) >= RingSize) {
			switch (kind) {
			case SlotEventKind::Value:
				overflowValue_[slot / 64].fetch_or(bitOf(slot), std::memory_order_release);
				break;
			case SlotEventKind::End:
				overflowEnd_[slot / 64].fetch_or(bitOf(slot), std::memory_order_release);
				break;
			case SlotEventKind::Begin:
				break;
			}
			return false;
		}
		ring_[tail & (RingSize - 1)] = Event {kind, static_cast<uint16_t>(slot), value01};
		tail_.store(tail + 1, std::memory_order_release);
		return true;
	}

	bool begin(size_t slot) noexcept { return push(SlotEventKind::Begin, slot); }
	bool value(size_t slot, float value01) noexcept { return push(SlotEventKind::Value, slot, value01); }
	bool end(size_t slot) noexcept { return push(SlotEventKind::End, slot); }

	//----------------------------------------------------------------------------------------//
	// CONSUMER (audio thread in process, or the flush thread — one at a time, as CLAP has it) //
	//----------------------------------------------------------------------------------------//

	/// Tell the host everything queued, then pay what it refused earlier. `emit(kind, slot, value01)`
	/// tries to push one event to the host and returns true IFF the host now has it. noexcept iff `emit`
	/// is; allocates nothing of its own.
	///
	/// Queued events FIRST, debts after, as `consumeEditorEvents` has it: what a user's hand is doing
	/// right now is not held behind a host that refused something a second ago.
	///
	/// NOT RE-ENTRANT, and not safe from two threads at once: `head_` and `owed_` are single-consumer
	/// state. That rests on CLAP never running `process` and `params.flush` concurrently for one
	/// instance, which the spec guarantees; a caller that could break it needs its own exclusion.
	template <class Emit>
	void drain(Emit&& emit) noexcept(noexcept(emit(SlotEventKind::Value, uint16_t {}, 0.0f))) {
		const size_t tail = tail_.load(std::memory_order_acquire);
		size_t head = head_.load(std::memory_order_relaxed);
		for (; head != tail; ++head) {
			const Event e = ring_[head & (RingSize - 1)];
			switch (e.kind) {
			case SlotEventKind::Value:
				if (emit(SlotEventKind::Value, e.slot, e.value01)) {
					// Current with the host now, so an older unpaid value for this slot is moot.
					clear(e.slot, kOweValue);
				}
				else {
					owe(e.slot, kOweValue);
				}
				break;
			case SlotEventKind::Begin:
				// An End still owed from this slot's LAST gesture must reach the host before this Begin
				// does, or paying it below would close the NEW gesture's grouping at its first event.
				// If it will not go, neither will the Begin (the queue is full), and a Begin is not
				// retried — so the gesture loses its grouping, the End stays owed, and nothing closes
				// early.
				if ((owed_[e.slot] & kOweEnd) != 0) {
					if (!emit(SlotEventKind::End, e.slot, 0.0f)) {
						break;
					}
					clear(e.slot, kOweEnd);
				}
				// Not retried, whatever it returns (see the class comment).
				emit(SlotEventKind::Begin, e.slot, 0.0f);
				break;
			case SlotEventKind::End:
				if (emit(SlotEventKind::End, e.slot, 0.0f)) {
					clear(e.slot, kOweEnd);
				}
				else {
					owe(e.slot, kOweEnd);
				}
				break;
			}
		}
		head_.store(head, std::memory_order_release);

		foldOverflow(overflowValue_, kOweValue);
		foldOverflow(overflowEnd_, kOweEnd);
		payDebts(emit);
	}

	/// Consumer-side reads of the debt table, for tests and diagnostics.
	bool owesValue(size_t slot) const noexcept { return slot < Capacity && (owed_[slot] & kOweValue) != 0; }
	bool owesEnd(size_t slot) const noexcept { return slot < Capacity && (owed_[slot] & kOweEnd) != 0; }

private:
	static constexpr size_t kWords = (Capacity + 63) / 64;

	/// What the host has not been told yet about one slot. One flag per kind, so a drag that overflows
	/// the host's queue for a second records the slot once rather than hundreds of times.
	static constexpr uint8_t kOweValue = 1u << 0;
	static constexpr uint8_t kOweEnd = 1u << 1;

	static constexpr uint64_t bitOf(size_t slot) noexcept { return uint64_t {1} << (slot % 64); }

	void owe(size_t slot, uint8_t what) noexcept { owed_[slot] |= what; }
	void clear(size_t slot, uint8_t what) noexcept { owed_[slot] &= static_cast<uint8_t>(~what); }

	void foldOverflow(std::array<std::atomic<uint64_t>, kWords>& from, uint8_t what) noexcept {
		for (size_t w = 0; w < kWords; ++w) {
			for (uint64_t bits = from[w].exchange(0, std::memory_order_acq_rel); bits != 0; bits &= bits - 1) {
				owe(w * 64 + static_cast<size_t>(std::countr_zero(bits)), what);
			}
		}
	}

	/// Re-send what the host refused, in the order a host expects (a value before its gesture end),
	/// and forget whatever it accepts. A value is read FRESH from `latest01_`, never replayed. There
	/// is no Begin arm, because nothing ever owes a Begin.
	template <class Emit>
	void payDebts(Emit& emit) {
		for (size_t slot = 0; slot < Capacity; ++slot) {
			const uint8_t owed = owed_[slot];
			if (owed == 0) {
				continue;
			}
			const auto s = static_cast<uint16_t>(slot);
			if ((owed & kOweValue) != 0
				&& emit(SlotEventKind::Value, s, latest01_[slot].load(std::memory_order_relaxed))) {
				clear(slot, kOweValue);
			}
			if ((owed & kOweEnd) != 0 && emit(SlotEventKind::End, s, 0.0f)) {
				clear(slot, kOweEnd);
			}
		}
	}

	// Producer-written, consumer-read.
	std::array<Event, RingSize> ring_ {};
	alignas(64) std::atomic<size_t> tail_ {0};
	alignas(64) std::atomic<size_t> head_ {0};
	std::array<std::atomic<float>, Capacity> latest01_ {};
	std::array<std::atomic<uint64_t>, kWords> overflowValue_ {};
	std::array<std::atomic<uint64_t>, kWords> overflowEnd_ {};

	// Consumer-only.
	std::array<uint8_t, Capacity> owed_ {};
};

} // namespace feathers::hostslots
