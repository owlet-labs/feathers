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

#include "device/registry.h"
#include "device/state_blocks.h"
#include "mod/arp/arp_core.h"
#include "mod/arp/arp_types.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

// THE ARPS' SHARED CLOCK AND NOTE TABLE (terrain-arp design §2, build step 0). The classic Arp and the Pattern device
// differ only in WHICH notes a grid slot plays; everything else is here, lifted out of `arp.cpp` unchanged:
//
//   - the held chord (the keys under the fingers, and the chord Latch holds), folded from `processNotes`;
//   - the clock: the song's whenever the transport plays and the rate's timeline is known, a free clock otherwise
//     (unless Only with Transport holds it silent), re-seated on a scrub or a loop, every onset on its own sample;
//   - the SOUNDING table, which is what makes "note-offs are never lost" true: a note is in it until its off has been
//     WRITTEN, and an off that does not fit the output is retried on the next sample;
//   - the newest slots for a card's lane, and the `arp_play` block (Latch's chord, the free clock's phase).
//
// A device hands `emit` a PlaySlot: called once for each grid slot whose onset is due, with the emitter, through which
// it sounds notes (`sound`) and records what its card should draw (`noteRecent`). The emitter decides WHEN; the
// device decides WHAT.

namespace feathers::mod::arp {

/// WHAT THE CLOCK READS, decoded by the device from its own controls.
struct ClockSettings {
	bool enabled = true;
	bool latch = false;
	/// WHEN IT PLAYS (`RunMode`): Always (the free clock while stopped), Transport (silent while stopped), Note (the free clock
	/// from the first key after a full release, whatever the transport does).
	RunMode run = RunMode::Always;
	/// A musical division (beats are the timeline) rather than Hz (seconds are).
	bool synced = true;
	/// The synced step's length in beats, and the unsynced step's frequency.
	double stepBeats = 0.25;
	double stepHz = 1.0;
	StepGrid grid {};
	/// **THRU**: the keys in
	/// `thruKeys` (a bit per MIDI key) pass to the output untouched — ons, offs, chokes — and never trigger; every other
	/// key is a trigger. Off: every key is a trigger, as before.
	bool thru = false;
	std::array<uint64_t, 2> thruKeys {};
	/// WHAT A TRIGGER DOES TO THE CLOCK (design §7.5): Gate, the arps' way — plays while one is held.
	TrigMode trig = TrigMode::Gate;
	[[nodiscard]] bool passes(int key) const noexcept {
		return thru && key >= 0 && key < 128 && ((thruKeys[static_cast<size_t>(key) >> 6] >> (key & 63)) & 1u) != 0u;
	}
};

class StepEmitter {
public:
	/// THE MOST NOTES THAT CAN SOUND AT ONCE, whatever the limit a device sets (`setSoundingLimit`).
	static constexpr size_t kSoundingCapacity = 64;

	/// A device's answer to "slot g is due": sound its notes through `e`.
	using PlaySlot = void (*)(void* ctx, int64_t g, StepEmitter& e);
	/// **EVERY CALL, AFTER THE SLOT** (when the clock runs): `u` is now in steps and `sps` one step in samples. A device
	/// that writes between onsets — TB-Glide's bend (design §11.3) — writes with `pushEvent` and asks back with `wakeIn`.
	using Between = void (*)(void* ctx, double u, double sps, StepEmitter& e);

	void prepare(double sampleRate) noexcept;
	/// As if just switched on: nothing sounds and the clock re-seats. The chord is kept (keys can still be held).
	void reset() noexcept;
	void setSettings(const ClockSettings& c) noexcept { c_ = c; }
	/// **A NEW GRID UNDER WHAT SOUNDS** (Pattern's Cycle, design §7.6): `c`, and on the song clock — where a slot is the
	/// song position over the step — every sounding note, queued on and grid origin in steps is scaled by `factor` (the
	/// old step over the new), so each keeps its song position and a note sounding at a switch keeps its off. The free
	/// clock's steps run on unbroken across a step change (it re-origins), so there it is `setSettings`. Called from a
	/// PlaySlot; `wakeIn` (≥ 1) then keeps this call's remaining sums, in the old steps, from timing anything.
	void regrid(const ClockSettings& c, double factor) noexcept;
	/// **THE NEXT SLOT IS `g`** — a PlaySlot that moved the grid names where the clock carries on.
	void continueAt(int64_t g) noexcept {
		nextSlot_ = g;
		seated_ = true;
	}
	/// Whether the clock is the song's (else free, or none).
	[[nodiscard]] bool songClock() const noexcept { return clock_ == Clock::Song; }
	[[nodiscard]] const ClockSettings& settings() const noexcept { return c_; }
	/// How many notes may sound before the OLDEST is ended to make room (the classic arp's 8).
	void setSoundingLimit(size_t n) noexcept { limit_ = n < 1 ? 1 : (n > kSoundingCapacity ? kSoundingCapacity : n); }

	void processNotes(const device::NoteEvent* in, size_t inCount, device::NoteEvent* out, size_t outCapacity,
	                  size_t* outCount) noexcept;
	size_t emit(const device::TransportClock& clock, uint64_t sampleNow, device::NoteEvent* out, size_t capacity,
	            uint32_t* samplesUntilNext, PlaySlot play, void* ctx, Between between = nullptr) noexcept;

	//-- WHAT A PLAYSLOT USES, during `emit` ------------------------------------------------------------------------//
	/// THE CHORD THE DEVICE PLAYS: the held one; or, where no trigger gates it (Transport, Restart) and none is held, C4 —
	/// the drone a pattern with no key plays over (in Drum, the keys only gate, so any key will do).
	/// **NOTES ALREADY SOUNDING THROUGH THE LANE BEFORE THIS EMITTER STOOD IN IT** (backlog QU): a host key that passed
	/// to the voices while the device was published but not yet installed. Seeded as Thru passes, so its off goes on to
	/// the voices (carrying the recorded id and channel where the host's off is a wildcard) instead of being swallowed.
	void seedPassed(const device::NoteEvent* ons, size_t count) noexcept {
		for (size_t i = 0; i < count && passedCount_ < kPassedCapacity; ++i) {
			passed_[passedCount_++] = Passed { ons[i].key, ons[i].channel, ons[i].noteId };
		}
	}
	[[nodiscard]] const HeldChord& chord() const noexcept {
		// NOTE (Run) gates on the keys held, not on the chord: a key passing Thru holds it too, and plays the idle chord.
		return chord_.empty() && (c_.trig != TrigMode::Gate || c_.run == RunMode::Note) ? idle_ : chord_;
	}
	/// The onset of slot `g` on this call's grid, in steps.
	[[nodiscard]] double onset(int64_t g) const noexcept { return c_.grid.onset(g); }
	/// One sample, in steps: the shortest a note may last.
	[[nodiscard]] double sampleSteps() const noexcept { return 1.0 / sps_; }
	/// **SOUND ONE NOTE** from `onU` to `offU` (in steps). A note already sounding on the same key is ended first — a
	/// note with no id is addressed by its key, so two overlapping on one key would be ended by whichever off came
	/// first — and at the limit the oldest is ended. False when the on did not fit (the later event is the one dropped).
	bool sound(int16_t key, int16_t channel, double velocity, double onU, double offU) noexcept;
	/// **A NOTE ALREADY SOUNDING ON `key` RUNS ON TO `offU`** instead of being struck again (a chord's held common tone,
	/// design §10.1). False when no such note sounds — the caller strikes it.
	bool extend(int16_t key, int16_t channel, double offU) noexcept;
	/// **SOUND A NOTE LATER IN THIS STEP** — at `onU`, a ratchet's later hits or a grace note's main note. Kept in a
	/// fixed queue and struck, by `sound`'s rules, on the call that reaches it. A jump, a change of clock or a switch-off
	/// forgets what is queued (nothing of it has sounded yet). False when the queue is full.
	bool schedule(int16_t key, int16_t channel, double velocity, double onU, double offU) noexcept;
	/// **WRITE ONE EVENT AS IT IS** — an expression for a sounding note. False when the output is full (the later event
	/// is the one dropped).
	bool pushEvent(const device::NoteEvent& e) noexcept { return push(e); }
	/// Be called again within `samples` (≥ 1), whatever else is due.
	void wakeIn(uint32_t samples) noexcept { wake_ = std::min(wake_, std::max<uint32_t>(samples, 1)); }
	/// Whether a note on `key` is sounding (its off not yet written).
	[[nodiscard]] bool sounding(int16_t key, int16_t channel) const noexcept;
	/// The card's lane: slot `slot` played `value` (key / 127).
	void noteRecent(uint32_t slot, float value) noexcept;

	//-- WHAT THE DEVICE PUBLISHES ----------------------------------------------------------------------------------//
	size_t recentEvents(device::RecentEvent* out, size_t capacity) const noexcept;
	void playhead(uint32_t* ordinal, float* phase) const noexcept;
	void publish() noexcept;
	uint64_t readPlay(float* out, size_t capacity) const noexcept { return play_.read(out, capacity); }
	bool restorePlay(const float* in, size_t n) noexcept;

	// TEST SEAMS — the values, not a render.
	[[nodiscard]] size_t soundingCount() const noexcept { return soundingCount_; }
	[[nodiscard]] double freePhase() const noexcept { return freeRunning_ ? lastU_ : std::nan(""); }

private:
	enum class Clock : uint8_t { None, Song, Free };
	struct Sounding {
		int16_t key = 60;
		int16_t channel = 0;
		double onU = 0.0;
		double offU = 0.0;
	};

	bool push(const device::NoteEvent& e) noexcept;
	bool endSounding(size_t i) noexcept;
	void endAll() noexcept;

	ClockSettings c_ {};
	double sampleRate_ = 48000.0;
	size_t limit_ = 8;

	/// THE KEYS UNDER THE FINGERS, and the chord the arp plays — the same set unless Latch holds a chord after them.
	HeldChord physical_;
	HeldChord chord_;
	uint32_t pressCount_ = 0;
	/// A PRESS GROUP IS OPEN: the keys pressed since the last `emit` share `pressCount_` (backlog QR).
	bool pressGroupOpen_ = false;
	bool latchWas_ = false;
	bool enabledWas_ = true;

	Clock clock_ = Clock::None;
	bool seated_ = false;
	int64_t nextSlot_ = 0;
	double lastU_ = 0.0;
	/// This call's step length in samples (what `sampleSteps` reads).
	double sps_ = 1.0;
	/// THE FREE CLOCK: where it started (in steps) and at which sample. `freeRestore_` is a saved phase waiting for the
	/// free clock's next start, so a reload carries on from where it was.
	bool freeRunning_ = false;
	double freeOriginU_ = 0.0;
	uint64_t freeStartSample_ = 0;
	bool haveFreeRestore_ = false;
	double freeRestore_ = 0.0;
	/// THE FREE CLOCK'S STEP LENGTH LAST CALL, in samples: a Rate move re-origins the free clock where it stands
	/// instead of jumping it.
	double freeSps_ = 0.0;

	std::array<Sounding, kSoundingCapacity> sounding_ {};
	size_t soundingCount_ = 0;

	/// THE QUEUED ONS (`schedule`): a ratchet's later hits and a grace note's main note.
	struct Pending {
		int16_t key = 60;
		int16_t channel = 0;
		double velocity = 1.0;
		double onU = 0.0;
		double offU = 0.0;
	};
	static constexpr size_t kPendingCapacity = 32;
	std::array<Pending, kPendingCapacity> pending_ {};
	size_t pendingCount_ = 0;

	/// This call's sample (what `regrid` re-origins the free clock at).
	uint64_t callSample_ = 0;
	/// The soonest a `Between` asked to be called back, this call.
	uint32_t wake_ = 0xFFFFFFFFu;
	/// The output buffer of the current `emit`.
	device::NoteEvent* out_ = nullptr;
	size_t outCapacity_ = 0;
	size_t outCount_ = 0;

	std::array<device::RecentEvent, device::kRecentEvents> recent_ {};
	size_t recentCount_ = 0;
	uint32_t lastSlot_ = 0;

	device::BlockSnapshot<kPlayWords> play_;

	/// THE CHORD A PATTERN WITH NO TRIGGER HELD PLAYS OVER (`chord`): C4.
	HeldChord idle_ = [] {
		HeldChord c;
		c.press(60, 0, 0.8f, 1);
		return c;
	}();
	/// THE THRU NOTES SOUNDING DOWNSTREAM (passed ons whose off has not come): an off or a keyed choke for one passes
	/// whatever Thru says by then, so a note passed is never left without its end.
	struct Passed {
		int16_t key = 0;
		int16_t channel = 0;
		/// The on's note id (−1: none), so an off sent with a wildcard id goes on carrying it (backlog QU).
		int32_t noteId = -1;
	};
	static constexpr size_t kPassedCapacity = 64;
	std::array<Passed, kPassedCapacity> passed_ {};
	size_t passedCount_ = 0;
	/// Takes the passed note an off or a keyed choke names — CLAP's wildcards (channel −1, note id −1) match any — and
	/// returns it through `found`; false when none is passed.
	bool takePassed(const device::NoteEvent& e, Passed* found) noexcept;
	/// **RESTART** (Trig): a trigger's on asked for step 0 here; the next `emit` (the same sample, a host calls it right
	/// after the notes) moves the grid's origin under it.
	bool restartPending_ = false;
	/// NOTE (Run): the first key after a full release asked for step 0; the next `emit` re-seats there, as a Restart does.
	bool noteRestart_ = false;
	/// The grid's origin, in steps of the running clock: slot g's onset is `grid.onset(g) + originU_`.
	double originU_ = 0.0;
};

} // namespace feathers::mod::arp
