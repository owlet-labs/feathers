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
#include "mod/tracker/tracker_types.h"

#include <array>
#include <cstddef>

// THE TRACKER'S STATE — a held-note stack and one smoothed output.
//
// **IT KEEPS THE STACK, NOT A PITCH.** The rule can change while notes are held, and a tracker that
// had already collapsed the chord to one number could not honour the change until the next note. That
// is the same mistake the host was making before the correction, one layer down: whoever throws the
// information away decides for everybody after them.

namespace feathers::mod::tracker {

/// **THE HELD KEYS, AND THE RULE THAT PICKS ONE OF THEM** — the tracker's own stack, lifted out so the
/// rack's host note (`RackEngine::updateNoteContext`) folds and picks by the SAME code rather than a
/// second copy that could drift from it (2026-10-04: monophonic keytrack on the bus follows one
/// chosen held note, by the Tracker's Last / Lowest / Highest).
///
/// ARRIVAL ORDER IS KEPT, because `Rule::Last` reads it. Bounded, so the audio thread never allocates:
/// a full stack drops its OLDEST key — the one furthest from being the note that sounds — to make room.
class HeldKeys {
public:
	static constexpr size_t kMax = 32;

	/// Fold one block's events: an On pushes its key (once — a repeated key is not a second entry), an
	/// Off or a Choke lifts it, and one whose key is negative is CLAP's wildcard and lifts every key.
	/// Anything else (an Expression) lifts nothing. Returns whether any On arrived.
	bool fold(const device::NoteEvent* in, size_t count) noexcept;

	[[nodiscard]] bool empty() const noexcept { return count_ == 0; }
	[[nodiscard]] size_t size() const noexcept { return count_; }
	/// THE KEY `rule` SELECTS. Meaningless while `empty()`; the caller keeps what it last read then.
	[[nodiscard]] int16_t pick(Rule rule) const noexcept;
	void clear() noexcept { count_ = 0; }

private:
	std::array<int16_t, kMax> held_ {};
	size_t count_ = 0;
};

class Tracker {
public:
	void prepare(double sampleRate) noexcept;
	void setFields(const float* fields, size_t count) noexcept;

	/// READ THE NOTES AND PASS THEM ON UNCHANGED — `NoteOps::processNotes`.
	void processNotes(const device::NoteEvent* in, size_t inCount, device::NoteEvent* out,
	                  size_t outCapacity, size_t* outCount) noexcept;

	/// ADVANCE THE GLIDE. Called once per modulator tick, like any other source.
	void tick() noexcept;

	/// **THE PITCH THE BUS DEVICES AFTER IT FOLLOW** (`NoteOps::busNote`) — the Rule's key, glided; false until a
	/// note has been heard, so the devices after it keep the bus's own note until then.
	bool busPitch(float* pitchSemitones) const noexcept;

	/// THE PUBLISHED SOURCE, 0..1 across the mapped key range.
	float output() const noexcept { return current_; }

	/// WHAT THE STACK SAYS RIGHT NOW, in semitones, or -1 when nothing is held. Exposed for a test
	/// rather than a picture: this is the value the whole device exists to compute, and asserting it
	/// through the rendered output would be asserting a claim about a value from a signal — which this
	/// repo has a rule about.
	float heldPitchForTest() const noexcept { return target_; }
	size_t heldCountForTest() const noexcept { return held_.size(); }

	/// **WHAT `setFields` DECODED, SO THE REGISTRY-WIDE ROUND-TRIP CASE CAN SEE THIS DEVICE AT ALL.**
	///
	/// Every other registered device has a `stateOf`/`rowOf` pair, and `test_device_ranges.cpp` walks
	/// the registry asking each one whether a value inside a control's DECLARED range survives its
	/// decode — the check that would have caught the shaper's gamma clamp, which read 0..1 while the
	/// declaration ran to +-2^24. This device decodes straight into its members and has no row codec,
	/// so without these four it would be the one hole in that sweep, and it is the device whose decode
	/// style is closest to the one that produced the bug: open-coded reads with clamps on two of them.
	///
	/// **FOUR AND NOT FIVE: `Param::Enabled` IS ABSENT BECAUSE `setFields` DOES NOT READ IT.** The
	/// host switches a modulator through `Traits::enableControl`; the device never sees it. A getter
	/// that returned something for it would be inventing a decode that does not happen.
	int ruleForTest() const noexcept { return static_cast<int>(rule_); }
	float rootNoteForTest() const noexcept { return rootNote_; }
	float spreadForTest() const noexcept { return spreadOctaves_; }
	float glideForTest() const noexcept { return glide_; }

	/// **THE GLIDE'S PER-TICK COEFFICIENT AT THE PREPARED RATE** — identical to the original formula at 48 kHz,
	/// and the same time constant at any other (a tick is a fixed 32 samples, so its length in seconds moves).
	float glideCoeff() const noexcept;

private:
	/// The rate the stored Glide number is calibrated at.
	static constexpr double kReferenceRate = 48000.0;
	void recomputeTarget() noexcept;

	/// THE KEYS HELD, IN THE ORDER THEY ARRIVED. Order is what `Rule::Last` reads, so a set would not
	/// do — and it is why a release removes by value rather than popping.
	HeldKeys held_;

	Rule rule_ = Rule::Last;
	/// THE NOTE THAT READS ZERO, and how many octaves either side reach full scale. See
	/// `Param::RootNote` on why 60 is chosen rather than read from a host key.
	float rootNote_ = 60.0f;
	float spreadOctaves_ = 2.0f;
	float glide_ = 0.0f;
	double sampleRate_ = 48000.0;

	/// -1 MEANS NOTHING IS HELD, and it is deliberately not 0: key 0 is a real note, and a tracker
	/// that reported silence as the bottom of the keyboard would slam every destination it drove. NOT SAVED WITH THE
	/// PATCH (backlog NM;): it is performance memory, the last note played, and a
	/// fresh instance holds nothing until its first note.
	float target_ = -1.0f;
	float current_ = 0.0f;
	/// THE BUS PITCH, glided in semitones — see `busPitch`. -1 until the first tick after a note.
	float pitch_ = -1.0f;
};

} // namespace feathers::mod::tracker
