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
#include "mod/noteexpr/noteexpr_types.h"

#include <array>
#include <cstddef>

// THE NOTE-EXPRESSION MODULATOR'S STATE — a held-note stack with each note's expressions on it.
//
// **IT KEEPS EVERY HELD NOTE, NOT THE COLLAPSED ANSWER**, which is the Tracker's own lesson stated in
// its header: *"whoever throws the information away decides for everybody after them."* The Rule can
// change while notes are held, and a device that had already reduced the chord to one number could
// not honour the change until the next note-on.
//
// **AND IT IS WHAT MAKES THE PER-VOICE STEP CHEAP LATER.** The per-note values are already here,
// addressed by note id; publishing them per voice is a matter of asking for a different one, not of
// finding them.

namespace feathers::mod::noteexpr {

class NoteExpressions {
public:
	void prepare(double sampleRate) noexcept;
	void setFields(const float* fields, size_t count) noexcept;

	/// WATCH THE NOTES AND PASS THEM ON UNCHANGED — `NoteOps::processNotes`.
	///
	/// **A PASSTHROUGH ON PURPOSE.** This device's output is an observation; it has no reason to
	/// rewrite the stream, and one that did would be inventing the mapping `kTraits`'s header
	/// declines to invent. A device that wants to change an expression can — what leaves the lane is
	/// what the voice feels — and this is not that device.
	void processNotes(const device::NoteEvent* in, size_t inCount, device::NoteEvent* out,
	                  size_t outCapacity, size_t* outCount) noexcept;

	/// **WHAT ONE NOTE IS CARRYING ON `expression`** — `ModOps::outputForNote`, and the per-voice road.
	///
	/// BY NOTE ID BECAUSE THIS LAYER HAS NO VOICES. The host knows which voice is playing which note;
	/// this device knows what each note is expressing. Neither needs the other's vocabulary.
	///
	/// A note this device is not holding gets the COLLAPSED answer, so a voice in its release tail
	/// reads the rule's value rather than dropping to zero the instant the key comes up.
	float outputForNote(size_t expression, int32_t noteId, int16_t channel, int16_t key) const noexcept;

	/// WHAT THE SELECTED NOTE IS CARRYING ON `expression`, or 0 when nothing is held.
	///
	/// `expression` is a CLAP expression id and is also the output index — see `kOutputs`.
	float output(size_t expression) const noexcept;

	/// TEST SEAM. How many notes the stack is holding.
	///
	/// Here for the reason the Tracker's `heldCountForTest` is: the stack is the thing this device
	/// exists to keep, and asserting it through a published value would be asserting a claim about a
	/// structure from one number that structure produced.
	size_t heldCountForTest() const noexcept { return heldCount_; }

	/// TEST SEAM. Which note id the current Rule selects, or -1 when nothing is held.
	int32_t selectedNoteForTest() const noexcept;

	/// TEST SEAMS FOR THE DECODED CONTROLS — what `test_device_ranges.cpp` reads a row back through.
	///
	/// This device has no `Row`/`Settings` codec pair: `setFields` decodes straight into members, the
	/// Tracker's arrangement and for the same reason (two controls, neither of them a payload). The
	/// range sweep needs a way back from the members to a row, and these are it.
	int ruleForTest() const noexcept { return static_cast<int>(rule_); }
	bool enabledForTest() const noexcept { return enabled_; }

private:
	/// ONE HELD NOTE AND EVERYTHING IT IS EXPRESSING.
	struct Held {
		int32_t noteId = -1;
		int16_t channel = 0;
		int16_t key = 0;
		/// **INDEXED BY CLAP EXPRESSION ID**, which is also the output index. Starts at each
		/// expression's own neutral rather than at zero — see `kNeutral`.
		std::array<float, std::size(kOutputs)> value {};
	};

	/// **WHERE AN UNEXPRESSED NOTE READS, PER EXPRESSION, AND IT IS NOT ALWAYS ZERO.**
	///
	/// CLAP's own defaults: VOLUME is a gain with 1 as unity, PAN is 0..1 with 0.5 centred, and the
	/// rest rest at 0. A note nobody has touched must publish the value that means "untouched" — a
	/// pan source sitting at 0 would read as hard left the moment a patch routed it, which is a device
	/// inventing a gesture the player did not make.
	static constexpr std::array<float, std::size(kOutputs)> kNeutral { 1.0f, 0.5f, 0.0f,
		                                                              0.0f, 0.0f, 0.0f, 0.0f };

	size_t indexOfNote(int32_t noteId, int16_t channel, int16_t key) const noexcept;
	size_t selectedIndex() const noexcept;

	/// THE STACK. Sized to the voice pool's polyphony rather than to the note lane's capacity: what
	/// can be HELD is bounded by what can sound, and a chord larger than the synth can play has
	/// already been stolen down by the time it matters.
	static constexpr size_t kMaxHeld = 16;
	std::array<Held, kMaxHeld> held_ {};
	size_t heldCount_ = 0;
	/// Climbs forever, so the newest held note is the maximum — the Tracker's argument for the same
	/// field: a "previous note" scalar gets the three-note case wrong and passes every two-note test.
	std::array<uint64_t, kMaxHeld> serial_ {};
	uint64_t nextSerial_ = 1;

	Rule rule_ = Rule::Last;
	bool enabled_ = true;
};

} // namespace feathers::mod::noteexpr
