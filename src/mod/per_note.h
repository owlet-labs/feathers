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

#include "device/registry.h"
#include "mod/modulator.h"
#include "mod/trig.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

// POLY OR MONO — ONE SHAPE PER NOTE, OR ONE FOR THE DEVICE, and every destination reads the one answer.
//
// **WHY A TABLE PER NOTE AT ALL** (backlog EF, then GA). The MSEG and the LFO ran ONE shape for the whole rack, so
// a second note restarted it for every voice still sounding. MEASURED before: a one-second 0 → 1 → 0
// tent in Once, note A released at 0.2 s and note B struck at 0.3 s, read A's voice at 0.4 s = 0.200 where its own
// timeline says 0.800 — B's rising gate had sent A's ringing voice back to the start.
//
// **WHY A SWITCH, AND WHY NOTHING ELSE** (then no dual-path logic). The first cut of this file kept BOTH a per-note table and a collapsed
// shape in every device, and let a per-voice destination read one while a shared destination read the other — so
// one device answered two different questions depending on where it was pointed. Now a device is EITHER poly OR
// mono (`Settings::poly`), and `PolyShape` gives every destination the same answer:
//
//   - Poly: each note runs its own shape under the device's own mode. A voice reads its note's. A destination
//     with no voice (a bus effect, the card's playhead) reads the MOST RECENTLY STRUCK note's — the simplest single
//     honest rule, and the one a player hears last.
//   - Mono: ONE shape, gated by the rack's "any note held" and restarted by notes as the mode says; everyone reads it.
//   - Free is mono whatever the switch says: it ignores notes, so a shape per note would have nothing to start it.
//
// SHARED BY THE DAHDSR, THE MSEG AND THE LFO, so the rules here (a keyboard's -1 id is a wildcard, a re-press under
// a held gate is a retrigger, the oldest finished note is reclaimed first, every event passes through) were paid
// for once. A device supplies a `Shape` with `prepare`, `reset`, `tick(settings, gate, retrigger, clock, rateMul)`,
// `value`, `idle` and `carrySmoothingFrom`, and `Settings` with `mode`, `poly`, `smoothing` and `smoothCarry`.

namespace feathers::mod {

template <typename Shape, size_t N = 16>
struct PerNoteTable {
	/// SIXTEEN by default, the note expressions' number: more than the voices Feathers plays, and a fixed table so
	/// nothing allocates after `construct`.
	static constexpr size_t kMaxNotes = N;

	struct Note {
		int32_t noteId = -1;
		int16_t channel = 0;
		int16_t key = 0;
		bool used = false; ///< holds a note: sounding, or releasing, or DONE (below) until the slot is needed
		bool gate = false; ///< on from the note's On to its Off
		/// RELEASED AND FINISHED: kept, answering its shape AT REST, until a note needs the slot (oldest done first).
		bool done = false;
		/// A RE-PRESS UNDER THIS NOTE'S HELD GATE, latched for the next tick — see `processNotes`.
		bool retrigger = false;
		/// THIS NOTE'S OWN TRIG EDGE, for a per-voice source (backlog MV, B) — see `PolyShape::tick`. Fresh with each note.
		TrigEdge trig;
		uint64_t serial = 0;
		Shape shape;
	};

	std::array<Note, N> notes {};
	uint64_t nextSerial = 1;

	/// The note expressions' match: -1 on either side is a wildcard, so a host that sends no note ids still finds a
	/// note by channel and key.
	[[nodiscard]] size_t indexOfNote(int32_t noteId, int16_t channel, int16_t key) const noexcept {
		for (size_t i = 0; i < N; ++i) {
			const Note& n = notes[i];
			if (!n.used) {
				continue;
			}
			const bool idMatches = noteId == -1 || n.noteId == -1 || n.noteId == noteId;
			if (idMatches && (channel == -1 || n.channel == channel) && (key == -1 || n.key == key)) {
				return i;
			}
		}
		return N;
	}

	/// THE MOST RECENTLY STRUCK NOTE WHOSE GATE IS STILL UP, other than `except`. nullptr when nothing is held.
	[[nodiscard]] const Note* newestGated(size_t except) const noexcept {
		const Note* best = nullptr;
		for (size_t i = 0; i < N; ++i) {
			const Note& n = notes[i];
			if (i == except || !n.used || n.done || !n.gate) {
				continue;
			}
			if (best == nullptr || n.serial > best->serial) {
				best = &n;
			}
		}
		return best;
	}

	/// THE MOST RECENTLY STRUCK NOTE, held or not — what a destination with no voice reads under Poly. `except` is
	/// left out (N for none): a slot about to be handed to a new note is not where that note's output "was".
	[[nodiscard]] const Note* newest(size_t except = N) const noexcept {
		const Note* best = nullptr;
		for (size_t i = 0; i < N; ++i) {
			const Note& n = notes[i];
			if (i != except && n.used && (best == nullptr || n.serial > best->serial)) {
				best = &n;
			}
		}
		return best;
	}

	/// A slot for a new note: an unused one; else the oldest DONE one; else the oldest RELEASED one; else the oldest
	/// of all. Released before held, because a shape that never finishes on its own (an LFO's) stays un-done after
	/// its key is up, and reclaiming a held note's shape before it would take a sounding voice's modulation away.
	[[nodiscard]] size_t slotForNewNote() const noexcept {
		size_t best = N;
		int bestRank = 4;
		uint64_t bestSerial = 0;
		for (size_t i = 0; i < N; ++i) {
			const Note& n = notes[i];
			const int rank = !n.used ? 0 : n.done ? 1 : !n.gate ? 2 : 3;
			if (rank < bestRank || (rank == bestRank && n.serial < bestSerial)) {
				best = i;
				bestRank = rank;
				bestSerial = n.serial;
			}
		}
		return best;
	}

	void prepare(double sampleRate) {
		for (Note& n : notes) {
			n.shape.prepare(sampleRate);
		}
	}

	void reset() {
		for (Note& n : notes) {
			n.shape.reset();
			n.used = false;
			n.gate = false;
			n.done = false;
			n.retrigger = false;
			n.trig.reset();
		}
	}

	/// EVERY NOTE, ONE TICK, under the device's own settings. Each note is started by its own On (a rising gate
	/// always starts a shape) and a re-press under its held gate is a RETRIGGER, which the shape's own rule admits
	/// or refuses (backlog FJ). A done note is still ticked: done reclaims the slot first, it does not stop a wave
	/// (an LFO's goes on cycling under a voice still sounding it); an envelope that is done sits at rest.
	template <typename Settings>
	void tick(const Settings& settings, const device::ModTickContext& ctx) {
		for (Note& n : notes) {
			if (!n.used) {
				continue;
			}
			n.shape.tick(settings, n.gate, n.retrigger, ctx.clock, ctx.rateMul);
			n.retrigger = false;
			if (!n.gate && n.shape.idle()) {
				n.done = true;
			}
		}
	}

	/// THE NOTE LANE'S EVENTS INTO THE TABLE, every one passing through. `startShape(shape, newestHeld)` prepares a
	/// FRESH note's shape — reset it, or take up `newestHeld` (non-null only when the device asked for it by
	/// passing `legato`) — and is the one place a device says anything about a note's beginning.
	///
	/// **`carrySmoothing`: SMOOTH THROUGH RETRIGGER** (backlog MM). MEASURED before it: under mono the
	/// one shape's smoother already ran through a retrigger (LFO 0.411 → 0.406 at 100 ms, a tick's glide), but under
	/// Poly every new note's shape began its smoother at ZERO — a fresh `reset()` — so the voice it landed on read 0
	/// from its first sample whatever the last note had stood at (0.411 → 0.000). With this on, a fresh shape's
	/// smoother takes up the MOST RECENTLY STRUCK other note's output (what the card shows, and in a played line the
	/// note before), and the new note glides from there. A legato take-up already carries everything, so it is left
	/// alone. The caller passes false when smoothing is off: with nothing to glide, carrying would only hand the voice
	/// the old note's value until the next tick — the blip backlog FF removed.
	template <typename StartShape>
	void processNotes(const device::NoteEvent* in, size_t inCount, device::NoteEvent* out, size_t outCapacity,
	                  size_t* outCount, bool legato, bool carrySmoothing, StartShape&& startShape) noexcept {
		if (outCount != nullptr) {
			*outCount = 0;
		}
		if (in == nullptr || out == nullptr || outCount == nullptr) {
			return;
		}
		for (size_t i = 0; i < inCount; ++i) {
			const device::NoteEvent& e = in[i];
			switch (e.kind) {
			case device::NoteEvent::Kind::On: {
				size_t at = indexOfNote(e.noteId, e.channel, e.key);
				const bool known = at < N;
				if (!known) {
					at = slotForNewNote();
				}
				Note& n = notes[at];
				if (known && n.used && !n.done && n.gate) {
					// **STRUCK AGAIN UNDER ITS OWN HELD GATE** — a keyboard's re-press of a key still held, whose -1
					// id finds the same note (backlog FG). There is no rising gate to start it, so it is a RETRIGGER,
					// and the shape's own rule decides.
					n.retrigger = true;
				}
				else if (!known || !n.used || n.done) {
					// A NEW SHAPE: fresh, or — under Legato — the one already held (backlog FG). `!known` includes a
					// RECLAIMED slot, which still holds another note's shape.
					const Note* held = legato ? newestGated(at) : nullptr;
					startShape(n.shape, held);
					if (carrySmoothing && held == nullptr) {
						if (const Note* from = newest(at); from != nullptr) {
							n.shape.carrySmoothingFrom(from->shape);
						}
					}
					n.retrigger = false;
					n.trig.reset();
				}
				// AND A KEY STRUCK AGAIN WHILE ITS LAST NOTE IS STILL RELEASING keeps its shape: the gate rising
				// under it is a start, and the shape's own rule decides where that start begins (the DAHDSR's
				// re-attack from where it stands).
				n.noteId = e.noteId;
				n.channel = e.channel;
				n.key = e.key;
				n.used = true;
				n.gate = true;
				n.done = false;
				n.serial = nextSerial++;
				break;
			}
			case device::NoteEvent::Kind::Off:
			case device::NoteEvent::Kind::Choke: {
				// EVERY NOTE THE EVENT NAMES, not the first: a wildcard off (channel or key -1, the all-notes-off
				// shape) lifts every note it matches, and one left gated would hold a voice open with nothing to
				// release it.
				for (Note& n : notes) {
					if (!n.used || n.done) {
						continue;
					}
					const bool idMatches = e.noteId == -1 || n.noteId == -1 || n.noteId == e.noteId;
					if (!idMatches || (e.channel != -1 && n.channel != e.channel) || (e.key != -1 && n.key != e.key)) {
						continue;
					}
					n.gate = false; // the release runs from here; `tick` marks it done when it ends
					if (e.kind == device::NoteEvent::Kind::Choke) {
						n.shape.reset();
						n.done = true;
					}
				}
				break;
			}
			case device::NoteEvent::Kind::Expression:
				break;
			}
			// **EVERY EVENT PASSES THROUGH** — the note lane's rule; this device only listens.
			if (*outCount < outCapacity) {
				out[(*outCount)++] = e;
			}
		}
	}
};

/// **ONE DEVICE'S SHAPE, POLY OR MONO** — the whole of the decision, in one place, so the three devices cannot
/// answer it three ways. See the top of this file.
template <typename Shape, typename Settings, size_t N = 16>
struct PolyShape {
	/// THE DEVICE'S ONE SHAPE, gated by the rack's "any note held" — what every destination reads under mono.
	///
	/// **UNDER POLY IT IS STILL TICKED, AS THE ANSWER FOR A NOTE THE TABLE NEVER SAW**, and only that. A device
	/// instance built while a note is held — the first note into a freshly loaded patch, or an envelope added (or
	/// deleted and replaced) under a held key — never heard that note's On, so it has no entry for it. MEASURED: with
	/// nothing to fall back on, such a voice read 0 for its whole life (`test_synth`, "both modulator kinds tick to
	/// the values the pre-runner engine produced", every reading 0). The one shape gated by the rack is the best
	/// answer for a note nobody told the device about; it is not a second meaning for notes the device did see.
	Shape mono;
	PerNoteTable<Shape, N> notes;
	Settings settings {};

	/// POLY, unless the mode is Free: a Free shape ignores notes, so there is nothing per note to start it. NOR LFO RETRIG
	/// WITH "NOTES FIRE TRIG" OFF (backlog NU): with no note restarting it, it is Free (less the song lock), and a fresh
	/// shape per note would only be a note starting it by another road.
	[[nodiscard]] bool poly() const noexcept {
		return settings.poly && polyRuns(settings.mode, notesFireTrig(settings));
	}

	void prepare(double sampleRate) {
		mono.prepare(sampleRate);
		notes.prepare(sampleRate);
	}

	/// **THE TRIG'S EDGE (backlog MV)** — the device's detector, watching `Settings::trig` as the rack resolved it this
	/// tick for the whole rack (`mod/trig.h`). It fires the one shape, and every note the host cannot resolve for (see
	/// `tick`). Each note also carries its own (`PerNoteTable::Note::trig`) for a per-voice source.
	TrigEdge trig;
	/// HOW MANY TIMES THE TRIG HAS FIRED, the device's edge and every note's — a count rather than a flag, so a reader
	/// that looks less often than the device ticks (a card at 60 Hz against 750) still sees that a one-tick fire
	/// happened.
	uint32_t trigFires = 0;

	void reset() {
		mono.reset();
		notes.reset();
		trig.reset();
	}

	/// ONE TICK. The notes' gates come from the note lane; the one shape's from the rack's "any note held" and its
	/// retrigger. The notes are ticked only under Poly — under mono nothing reads them. The Trig is read FIRST, so a
	/// fire lands on the tick whose row first crossed the fire level.
	///
	/// **UNDER POLY A NOTE FIRES ON ITS OWN VOICE'S TRIG** (backlog MV, B). When the host resolves the Trig for a note (`ModTickContext::perNote`: a per-voice source
	/// reaches it and a voice holds the note), that note's own edge watches its own value and fires its shape alone.
	/// When it cannot — a source with one value for every voice, a rack with no voices, a note no voice holds — the note
	/// follows the device's edge, so a macro or an LFO fires every sounding note at once. A finished note is left
	/// alone: nothing reads it but the slot reclaimer. Each shape's own rule decides what a fire does (`Shape::fire`).
	void tick(const device::ModTickContext& ctx) {
		const bool fired = trig.step(settings.trig);
		if (fired) {
			++trigFires;
			mono.fire();
		}
		if (poly()) {
			const device::ModTickContext::PerNoteResolver* r = ctx.perNote;
			for (auto& n : notes.notes) {
				if (!n.used || n.done) {
					continue;
				}
				float own = 0.0f;
				const bool resolved = r != nullptr && r->resolve != nullptr
				                      && r->resolve(r->context, Settings::kTrigControl, n.noteId, n.channel, n.key, &own);
				if (resolved ? n.trig.step(own) : fired) {
					n.shape.fire();
					trigFires += resolved ? 1u : 0u;
				}
			}
		}
		mono.tick(settings, ctx.gate, ctx.retrigger, ctx.clock, ctx.rateMul);
		if (poly()) {
			notes.tick(settings, ctx);
		}
	}

	/// THE SHAPE A DESTINATION WITH NO VOICE READS — the mono shape, or under Poly the most recently struck note's.
	[[nodiscard]] const Shape& sharedShape() const noexcept {
		if (poly()) {
			if (const auto* n = notes.newest(); n != nullptr) {
				return n->shape;
			}
		}
		return mono;
	}

	/// WHAT ONE VOICE READS — its note's own shape under Poly (a note the table never saw reads the one shape, see
	/// `mono`), and the one shape under mono. THE WHOLE NOTE, not the id alone: a keyboard's notes all wear id -1, and by id
	/// alone every voice read the first entry.
	[[nodiscard]] float valueForNote(int32_t noteId, int16_t channel, int16_t key) const noexcept {
		return shapeForNote(noteId, channel, key).value();
	}
	/// THE SHAPE `valueForNote` READS — for a device whose shape has more to say than its value (the LFO's right ear).
	[[nodiscard]] const Shape& shapeForNote(int32_t noteId, int16_t channel, int16_t key) const noexcept {
		if (poly()) {
			const size_t at = notes.indexOfNote(noteId, channel, key);
			if (at < N) {
				return notes.notes[at].shape;
			}
		}
		return mono;
	}

	/// SMOOTH THROUGH RETRIGGER only when there is smoothing to carry — see `PerNoteTable::processNotes`.
	[[nodiscard]] bool carrySmoothing() const noexcept {
		return settings.smoothCarry && smoothingTau(settings.smoothing) > 0.0;
	}

	template <typename StartShape>
	void processNotes(const device::NoteEvent* in, size_t inCount, device::NoteEvent* out, size_t outCapacity,
	                  size_t* outCount, bool legato, StartShape&& startShape) noexcept {
		notes.processNotes(in, inCount, out, outCapacity, outCount, legato, carrySmoothing(),
		                   std::forward<StartShape>(startShape));
	}
};

} // namespace feathers::mod
