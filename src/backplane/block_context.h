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

#include "device/device.h"

// WHAT IS TRUE OF THIS BLOCK FOR THE WHOLE HOST — one object, one delivery path, however many
// containers there are.
//
// WHY IT EXISTS. There is no general per-block context in this codebase, and `setTransportClock`
// sits on `FxRunner` only because `setNoteContext` already did. That left ONE type with TWO delivery
// paths — `Synth` hands the same `rackClock()` to the rack and to the fx runner — and a third
// container would have made three. There was no good reason for the split — it followed the precedent
// that existed — and building this, then moving the fx lane onto it, beats having three. That is `device_row.h`'s rule arriving before the cost
// rather than after it: the convention that is wrong will be wrong silently, so it should exist once
// and be used twice rather than be discovered twice.
//
//------------------------------------------------------------------------------------------------//
// WHY NoteContext IS NOT IN HERE, WHICH IS THE ONLY REAL DECISION IN THIS FILE                     //
//------------------------------------------------------------------------------------------------//
//
// The obvious shape is `{ TransportClock clock; NoteContext note; }` — the two things a device is
// told about the world, in the one struct, delivered together. It is wrong, and the reason is one
// line of feathers-eb's own: **A NOTE IS PER VOICE; A SONG IS NOT.**
//
// The clock is host-wide and must be, and that is a measured constraint rather than a tidiness one:
// two slots reading different tempos is a bug that appears only when a tempo automation crosses a
// block boundary, which is why `FxRunner` holds ONE clock for the runner rather than one per
// instance. The note is the opposite — a per-voice runner holds one instance per voice slot
// precisely so that each can be told about a different note.
//
// Bundling them would give the note the clock's lifetime, and the failure would be quiet in the worst
// direction: every voice would see whichever note was written last, and a monophonic test would never
// notice. **Two facts that happen to arrive at the same moment are not one fact**, and a struct is
// the wrong place to record a coincidence of timing.
//
// So the note keeps travelling per instance, exactly as it does today, and this carries only what is
// genuinely true of the block for everyone in it. The condition for moving the fx lane onto this was
// that the context be per block — then the move is a small change; anything finer would not fit —
// which this satisfies by construction rather than by
// promise.
//
//------------------------------------------------------------------------------------------------//
// WHY IT IS A STRUCT AROUND ONE FIELD                                                              //
//------------------------------------------------------------------------------------------------//
//
// Because the growth is anticipated and named. `device::TransportClock` carries the tempo and
// deliberately not a POSITION, and the paragraph beside its declaration says what that costs: an LFO
// re-phasing on a bar line, a delay flushing at a loop point, a device that should sound the same
// wherever the playhead drops — none of those can be had from a tempo alone. When a position lands
// it lands in the clock, additively, and every container that takes a `BlockContext` gets it without
// a signature changing anywhere.
//
// WHAT IS DELIBERATELY ABSENT: the sample rate. It arrives through `AudioOps::prepare` and does not
// change per block, so carrying it here would be a second place for it to live and a second place for
// it to be stale. A field is earned by a reader that cannot get the fact anywhere else.

namespace feathers::backplane {

/// The per-block facts a host publishes to everything it contains.
struct BlockContext {
	/// The transport, or `unknown()`. `device::` and not `mod::` — the struct was moved out of
	/// `mod/modulator.h` precisely so the DEVICE vocabulary would not depend on the MODULATOR tier,
	/// and a `using` was left behind so the 41 existing references compile unchanged. A new caller
	/// should name the real one.
	///
	/// NOTE THAT VALIDITY IS INDEPENDENT OF WHETHER THE TRANSPORT IS PLAYING, and this is the
	/// inference a second host will make wrongly and never notice. `CLAP_TRANSPORT_HAS_TEMPO` and
	/// `CLAP_TRANSPORT_IS_PLAYING` are separate flags and a stopped host still reports its tempo —
	/// so "only while rolling" cannot be read out of `haveTempo()`, and a container that wants it
	/// must be given it as its own fact rather than deducing it from the tempo's absence.
	device::TransportClock clock {};

	friend bool operator==(const BlockContext&, const BlockContext&) = default;
};

/// Nothing known — what a container is handed before a host has said anything, and what a build with
/// no transport at all hands over for ever.
///
/// SPELLED AS A FUNCTION SO THE DEFAULT HAS A NAME. A caller writing `BlockContext {}` is saying the
/// same thing, but is not obviously saying it on purpose; the clock's own `unknown()` makes exactly
/// this distinction one level down, and the two should read alike.
constexpr BlockContext unknownBlock() noexcept {
	return BlockContext { device::TransportClock::unknown() };
}

} // namespace feathers::backplane
