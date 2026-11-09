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
#include "mod/arp/humanize.h"
#include "mod/arp/step_emitter.h"

#include <cstddef>
#include <cstdint>

// THE ARP AT PLAY. Its clock, held chord, sounding table and saved block are the shared
// `StepEmitter` (step_emitter.h); what is the classic arp's own is which note a slot plays — the chord's pattern under
// Order and Octaves, entry `(g − Offset) mod length` — and its three modulation sources.
//
//   - `processNotes`, when the host's notes arrive: the emitter folds them into the held chord and passes nothing on
//     (off, it passes everything on). It NEVER emits a step here: a chord arrives one note per call.
//   - `emit`, at the start of every render chunk (`NoteOps::emitNotes`): the offs that are due, then the step that is
//     due, at this sample, and how many samples until the next of either.

namespace feathers::mod::arp {

class Arp {
public:
	/// HOW MANY NOTES CAN SOUND AT ONCE: a 200% gate overlaps two steps, and a jump or a tie can leave one more.
	static constexpr size_t kMaxSounding = 8;

	Arp() noexcept { em_.setSoundingLimit(kMaxSounding); }

	void prepare(double sampleRate) noexcept { em_.prepare(sampleRate); }
	/// As if just switched on: nothing sounds and the clock re-seats. The chord is kept (keys can still be held).
	void reset() noexcept { em_.reset(); }
	void setFields(const float* fields, size_t count) noexcept;

	void processNotes(const device::NoteEvent* in, size_t inCount, device::NoteEvent* out, size_t outCapacity,
	                  size_t* outCount) noexcept {
		em_.processNotes(in, inCount, out, outCapacity, outCount);
	}
	/// See `NoteOps::seedPassed` (backlog QU).
	void seedPassed(const device::NoteEvent* ons, size_t count) noexcept { em_.seedPassed(ons, count); }
	size_t emit(const device::TransportClock& clock, uint64_t sampleNow, device::NoteEvent* out, size_t capacity,
	            uint32_t* samplesUntilNext) noexcept {
		return em_.emit(clock, sampleNow, out, capacity, samplesUntilNext, &Arp::playSlot, this);
	}

	/// The modulation sources, after the newest step: Step (0–1 across the pattern), Pitch (key / 127), Velocity.
	[[nodiscard]] float outputAt(size_t output) const noexcept;
	/// Publishes what cannot be derived (`kBlocks`) for the host to harvest before a save.
	void tick() noexcept { em_.publish(); }
	/// **THE NEWEST STEPS, NEWEST FIRST** (`ModOps::recentEvents`), for the card's lane: each one's grid slot as its
	/// ordinal and its key / 127 as its value. Only an unbroken run of slots: a jump ends the list.
	size_t recentEvents(device::RecentEvent* out, size_t capacity) const noexcept {
		return em_.recentEvents(out, capacity);
	}
	/// **WHERE IT STANDS — the newest step's grid slot** (`ModOps::playhead`), `mod::Playhead::kIdle` before any. Not a
	/// formality: a host publishes a strip device's recent events beside its playhead (`Synth::playheadOf`), so a
	/// device with none would leave the card's step lane empty for ever.
	void playhead(uint32_t* ordinal, float* phase) const noexcept { em_.playhead(ordinal, phase); }

	uint64_t readPlay(float* out, size_t capacity) const noexcept { return em_.readPlay(out, capacity); }
	bool restorePlay(const float* in, size_t n) noexcept { return em_.restorePlay(in, n); }

	// TEST SEAMS — the values, not a render.
	[[nodiscard]] const Settings& settingsForTest() const noexcept { return s_; }
	[[nodiscard]] const HeldChord& chordForTest() const noexcept { return em_.chord(); }
	[[nodiscard]] size_t soundingForTest() const noexcept { return em_.soundingCount(); }
	/// The free clock's position in steps right now, or NaN when it is not running.
	[[nodiscard]] double freePhaseForTest() const noexcept { return em_.freePhase(); }
	/// The pattern the chord makes now, and its length.
	size_t patternForTest(PatternNote* out) const noexcept;
	/// The Feel choice's Humanize mode (SKLITTER's ten, in its order).
	[[nodiscard]] static humanize::Mode feelModeOf(size_t feel) noexcept;
	/// **THE FEEL AS HUMANIZE READS IT** — the mode, Feel Amount as the depth, Free Phase as Free's position (backlog QA),
	/// and Wobble (zone 2, design §12-ter). The engine and the card's lane both read it, so they cannot disagree.
	[[nodiscard]] static humanize::Feel feelOf(size_t feel, double amount, double freePhase, double wobble) noexcept {
		return { feelModeOf(feel), amount, 0, freePhase, wobble };
	}

private:
	/// THE CLASSIC ARP'S ANSWER TO "SLOT g IS DUE": one note of the chord's pattern.
	static void playSlot(void* self, int64_t g, StepEmitter& e) noexcept;
	/// THE FEEL'S GRID HOOK (`StepGrid::shift`).
	static double shiftOf(const void* self, int64_t g) noexcept;

	Settings s_ {};
	StepEmitter em_;

	float stepOut_ = 0.0f;
	float pitchOut_ = 0.0f;
	float velocityOut_ = 0.0f;
};

} // namespace feathers::mod::arp
