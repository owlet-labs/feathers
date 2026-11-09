/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "mod/per_note.h"
#include "mod/stepseq/stepseq_types.h"

#include <algorithm>
#include <array>
#include <limits>
#include <cstdint>

// THE SEQUENCER'S ENGINE: one running pattern (`Shape`), and the device around it, poly or mono (`Device`).
//
// **ONE STEP IS DECIDED WHEN IT IS ENTERED, AND THE NEXT ONE WITH IT.** Arriving at a step rolls its probability,
// lets the Turing machine rewrite it, and picks the step after — four draws from the device's dice, always four and in
// that order, so a setting that ignores a draw (Forward ignores the pick) never shifts the ones after it, and the same
// seed plays the same music whatever else changes. The NEXT step is known one step early because a Tie needs it: a
// step whose successor is a Tie holds its gate to the boundary.
//
// **AN ADVANCE OR A RESET LANDS ON THE TICK ITS TRIG CROSSED**, the convention's (`mod/trig.h`); a Reset beats an
// Advance on the same tick. Under Clock = Rate an Advance also restarts the step's own clock, so the next step is a
// whole step away.

namespace feathers::mod::stepseq {

/// THE DICE: splitmix64, 53 bits to a unit draw. Its own type so the act (`randomise`) and the device share it.
struct Dice {
	uint64_t state = 0;
	void seed(uint64_t s) noexcept { state = s; }
	[[nodiscard]] uint64_t next() noexcept {
		uint64_t z = (state += 0x9E3779B97F4A7C15ull);
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
		return z ^ (z >> 31);
	}
	/// [0, 1).
	[[nodiscard]] float unit() noexcept { return static_cast<float>(static_cast<double>(next() >> 11) * 0x1.0p-53); }
};

/// The seed the device's dice start from at each (re)start of a pattern.
///
/// **A NOTE'S ORDINAL IS FOLDED IN** (`Shape::setOrdinal`), so two Poly notes struck together under Random do not
/// share one dice order. Ordinal 0 is the seed itself, bit for bit — the one pattern under mono, and a note struck
/// alone — so only a note that sounds BESIDE another moves onto a course of its own.
[[nodiscard]] constexpr uint64_t playSeed(int seed, uint32_t ordinal = 0) noexcept {
	const uint64_t base = 0x5EC0'0000'0000'0000ull ^ static_cast<uint64_t>(static_cast<uint32_t>(seed));
	if (ordinal == 0) {
		return base;
	}
	uint64_t z = base + 0x9E3779B97F4A7C15ull * static_cast<uint64_t>(ordinal);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}

class Shape {
public:
	void prepare(double sampleRate);
	void reset();

	/// **WHICH NOTE THIS PATTERN IS, AMONG THOSE HELD WITH IT** (`playSeed`): set at the note's start, after `reset`, and
	/// read at every restart. DERIVED FROM THE NOTES, never from an address or a count of constructions — the lowest
	/// ordinal no other held note has (`Device::ordinalForNewNote`) — so the same notes played from a reset draw the
	/// same orders, with nothing to store. The mono pattern is 0 for ever.
	void setOrdinal(uint32_t ordinal) noexcept { ordinal_ = ordinal; }
	[[nodiscard]] uint32_t ordinal() const noexcept { return ordinal_; }

	/// `mod::PolyShape`'s contract. No smoothing to carry: each step carries its own slew.
	void carrySmoothingFrom(const Shape&) noexcept {}
	/// ADVANCE (the Trig) and RESET, taken at the next tick.
	void fire() noexcept { advancePending_ = true; }
	void fireReset() noexcept { resetPending_ = true; }
	/// THIS PATTERN'S OWN RESET EDGE, for a per-voice source — the Advance's is the note's (`PerNoteTable::Note::trig`).
	TrigEdge resetEdge;

	/// **THE TURING MACHINE'S REWRITES ARE THE PATTERN** (backlog MX: a reload plays exactly what was heard). They are
	/// kept across a restart — a restart starts the loop again, it does not undo what the loop has become — and carried
	/// to the record through `Device::publish`; dropped only when the record's own steps take over (`clearRewrites`).
	void clearRewrites() noexcept { mutated_ = 0; }
	/// How many rewrites so far: the device publishes when this moves.
	[[nodiscard]] uint32_t rewrites() const noexcept { return rewrites_; }
	/// The stored pattern with this machine's rewrites in place — what it plays, and what the record should hold.
	void applyRewrites(Pattern* p) const noexcept {
		for (size_t s = 0; s < kMaxSteps; ++s) {
			if ((mutated_ & (1u << s)) != 0) {
				p->step[s].value = register_[s];
			}
		}
	}
	/// Keep the rewrites over a reset that would otherwise forget them (`Device::reset`).
	void takeRewritesFrom(const Shape& other) noexcept {
		register_ = other.register_;
		mutated_ = other.mutated_;
	}

	float tick(const Settings& settings, bool gate, bool retrigger, TransportClock clock, float rateMul = 1.0f);

	/// **WHERE IT IS PLAYING, IN AND OUT OF THE PATCH** (backlog NM, `kPlayWords`). `load` is all-or-nothing.
	void save(device::BlockWriter& w) const noexcept;
	[[nodiscard]] bool load(device::BlockReader& r) noexcept;

	/// THE VALUE OUTPUT, in the output's terms (bipolar applied).
	[[nodiscard]] float value() const noexcept { return value_; }
	[[nodiscard]] float output(size_t index) const noexcept {
		return index == kValueOutput ? value_ : index == kGateOutput ? gate_ : index == kTriggerOutput ? trigger_ : 0.0f;
	}
	/// A sequencer never finishes on its own: a voice still sounding after its key keeps its steps.
	[[nodiscard]] bool idle() const noexcept { return false; }

	/// WHICH STEP IS PLAYING AND HOW FAR THROUGH IT — the card's playhead. Idle before the first tick.
	[[nodiscard]] mod::Playhead playhead() const noexcept {
		if (step_ < 0) {
			return {};
		}
		return { static_cast<uint32_t>(step_), phase_ };
	}

	/// TEST SEAMS: the step entered, whether it plays, the step after, and the Turing machine's current values.
	[[nodiscard]] int stepForTest() const noexcept { return step_; }
	[[nodiscard]] bool playsForTest() const noexcept { return plays_; }
	[[nodiscard]] int nextForTest() const noexcept { return next_; }
	[[nodiscard]] uint32_t arrivalsForTest() const noexcept { return arrivals_; }
	[[nodiscard]] uint32_t hitsForTest() const noexcept { return hits_; }
	[[nodiscard]] float targetForTest() const noexcept { return target_; }
	[[nodiscard]] float turingValueForTest(size_t s) const noexcept {
		return s < kMaxSteps && (mutated_ & (1u << s)) != 0 ? register_[s] : -1.0f;
	}

private:
	void restart(const Settings& s);
	void enter(const Settings& s);
	[[nodiscard]] int pick(const Settings& s, int from, float draw) noexcept;
	void hit() noexcept;

	double tickSeconds_ = static_cast<double>(kBlockSamples) / 48000.0;
	uint32_t ordinal_ = 0;
	Dice dice_ {};
	TransportLock lock_ {};
	bool songAnchored_ = false;
	double songWas_ = 0.0;
	int songUnit_ = 0;
	double lastWhole_ = 0.0;

	bool started_ = false;
	bool advancePending_ = false;
	bool resetPending_ = false;

	int step_ = -1;
	int next_ = 0;
	int bounce_ = 1; ///< ping-pong's heading
	bool plays_ = false;
	bool playedBefore_ = false;
	Gate gateKind_ = Gate::Off;
	bool nextTies_ = false;
	int ratchet_ = 1;
	int sub_ = 0;
	float slew_ = 0.0f;

	/// RATE CLOCK: how far into the step, 0..1. TRIG CLOCK: ticks since the step began, and the last step's length.
	double stepPos_ = 0.0;
	uint32_t ticksInStep_ = 0;
	uint32_t intervalTicks_ = 0;
	float phase_ = 0.0f;

	float from_ = 0.0f;   ///< where the slew starts (unipolar)
	float target_ = 0.0f; ///< the step's value, or the held one when it does not play
	float raw_ = 0.0f;    ///< unipolar value now
	float value_ = 0.0f;
	float gate_ = 0.0f;
	float trigger_ = 0.0f;
	bool hitThisTick_ = false;

	/// THE TURING MACHINE'S REWRITES: a step whose bit is set plays `register_` rather than its stored value. Kept across
	/// a restart; the record catches up through `Device::publish`.
	std::array<float, kMaxSteps> register_ {};
	uint32_t mutated_ = 0;

	uint32_t arrivals_ = 0;
	uint32_t hits_ = 0;
	uint32_t rewrites_ = 0;
};

/// THE DEVICE: one pattern, or one per note (`mod::PolyShape`), plus the Reset's edges. `PolyShape` watches the
/// Advance (`Settings::kTrigControl`); this watches the Reset the same way, before it, so a Reset lands first.
struct Device {
	using Poly = mod::PolyShape<Shape, Settings>;
	Poly poly;
	TrigEdge resetEdge;
	uint32_t resetFires = 0;
	/// **RANDOMISE ON A TRIG** (backlog NT): its edge and count, and the pattern a fire rolled, played in place of the
	/// record's until the record has caught up with it (the harvest of `publish`) or a hand's edit or a load moves the basis.
	std::array<TrigEdge, kLaneCount> randomEdges {};
	std::array<uint32_t, kLaneCount> randomFires {};
	bool haveRolled = false;
	Pattern rolled {};

	/// **THE BASIS THE PATTERN WAS LAST TAKEN FROM** (`Pattern::basis`): NaN until the first tick, so the first tick
	/// takes it. When the record's moves — a hand's edit, an undo, a load — every rewrite is dropped and the record's
	/// steps play; when only its steps move (the record catching up with this device's own publish), nothing is.
	float takenBasis = std::numeric_limits<float>::quiet_NaN();
	/// The one machine's rewrite count at the last publish, and the serial that publish was given.
	uint32_t publishedRewrites = 0;
	uint64_t serial = 0;
	/// WHERE THE ONE PATTERN IS PLAYING AS LAST PUBLISHED, for the patch (`kStateOps`; backlog NM): after every tick.
	device::BlockSnapshot<kPlayWords> playSnapshot;
	void publishPlay() noexcept;

	void prepare(double sampleRate) { poly.prepare(sampleRate); }
	/// A RESET RESTARTS EVERY PATTERN AND KEEPS THE ONE MACHINE'S REWRITES — the loop it has become is the pattern,
	/// whether or not the record has caught up yet. A per-note machine's rewrites end with its note, as always.
	void reset() {
		Shape kept;
		kept.takeRewritesFrom(poly.mono);
		poly.reset();
		poly.mono.takeRewritesFrom(kept);
		resetEdge.reset();
		for (auto& n : poly.notes.notes) {
			n.shape.resetEdge.reset();
		}
		for (TrigEdge& e : randomEdges) {
			e.reset();
		}
	}
	void tick(const device::ModTickContext& ctx);
	/// **A NEW NOTE'S ORDINAL**: the lowest no other HELD note (used, gated, not done) carries, `starting` itself left
	/// out — so notes struck together count 0, 1, 2 and a note struck alone is 0, the mono pattern. A function of the
	/// note stream since the last reset, so a replay reproduces it.
	[[nodiscard]] uint32_t ordinalForNewNote(const Shape& starting) const noexcept;
	/// **ONE FIRE OF A LANE'S RANDOMISE**: `randomise` of `lane` over what the record holds (the pattern with the one machine's rewrites in
	/// place), by the stored lane, Rand Amt and Seed — the press's arguments — under the basis it was taken from, which
	/// the device never moves. A press writes a fresh basis and so drops every rewrite; a fire drops them itself.
	/// Published with a new serial, so the record catches up and no undo step is made. Nothing when nothing moves.
	void roll(Lane lane) noexcept;

	/// `ModOps::publishExtraState`: the whole extra state — the record's pattern with the one machine's rewrites in
	/// place, under the basis it was taken from — and its serial, or 0 before the first rewrite.
	uint64_t publish(float* extra, size_t capacity) const noexcept;

	[[nodiscard]] float outputAt(size_t output) const noexcept { return scaled(output, poly.sharedShape().output(output)); }
	[[nodiscard]] float outputForNote(size_t output, int32_t noteId, int16_t channel, int16_t key) const noexcept;

	/// **THE AMOUNT SCALES THE VALUE OUTPUT ONLY** (backlog NT): Gate and Trigger leave as they are, a pulse to 1 that a Trig
	/// fires on at 0.5. At unity it is the output itself, to the bit.
	[[nodiscard]] float scaled(size_t output, float v) const noexcept {
		return output == kValueOutput ? v * std::clamp(poly.settings.amount, -1.0f, 1.0f) : v;
	}

	/// THE SEVEN FIRE COUNTS IN ONE WORD — the fields are `kFire*` in stepseq_types.h, where the card reads them.
	[[nodiscard]] uint32_t fireWord() const noexcept {
		uint32_t word = ((poly.trigFires & kFireAdvanceMask) << kFireAdvanceShift)
		                | ((resetFires & kFireResetMask) << kFireResetShift);
		for (size_t l = 0; l < kLaneCount; ++l) {
			word |= (randomFires[l] & kFireRandomMask) << fireRandomShift(static_cast<Lane>(l));
		}
		return word;
	}
};

} // namespace feathers::mod::stepseq
