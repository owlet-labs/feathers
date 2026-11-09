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

#include "mod/tracker/tracker.h"

#include <algorithm>
#include <cmath>

namespace feathers::mod::tracker {

void Tracker::prepare(double sampleRate) noexcept {
	sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
}

void Tracker::setFields(const float* fields, size_t count) noexcept {
	const auto at = [&](Param p, float fallback) {
		const size_t i = static_cast<size_t>(p);
		return i < count && fields != nullptr ? fields[i] : fallback;
	};
	const int rule = static_cast<int>(std::lround(at(Param::Rule, 0.0f)));
	rule_ = static_cast<Rule>(std::clamp(rule, 0, 2));
	rootNote_ = at(Param::RootNote, 60.0f);
	spreadOctaves_ = at(Param::Spread, 2.0f);
	glide_ = std::clamp(at(Param::Glide, 0.0f), 0.0f, 1.0f);
	// THE RULE MAY HAVE CHANGED UNDER HELD NOTES, so the target is recomputed from the STACK rather
	// than left until the next note arrives. This is why the stack is kept and a pitch is not.
	recomputeTarget();
}

bool HeldKeys::fold(const device::NoteEvent* in, size_t count) noexcept {
	bool anyOn = false;
	for (size_t i = 0; i < count; ++i) {
		const device::NoteEvent& e = in[i];
		if (e.kind == device::NoteEvent::Kind::On) {
			anyOn = true;
			// A REPEATED KEY IS NOT A SECOND ENTRY. A host that sends two ons for one key — or a
			// chain with two sources — would otherwise need two offs to release it, and the second
			// never comes.
			bool already = false;
			for (size_t h = 0; h < count_; ++h) {
				already = already || held_[h] == e.key;
			}
			if (already) {
				continue;
			}
			// **A FULL STACK DROPS ITS OLDEST KEY**, the one furthest from being the note that sounds. The
			// tracker used to refuse the new key instead, which under `Rule::Last` ignored the note the hand
			// had just played; the rack's own stack had always dropped the oldest.
			if (count_ == kMax) {
				std::copy(held_.begin() + 1, held_.end(), held_.begin());
				--count_;
			}
			held_[count_++] = e.key;
		}
		else if (e.kind == device::NoteEvent::Kind::Off || e.kind == device::NoteEvent::Kind::Choke) {
			// ONLY AN OFF OR A CHOKE LIFTS A KEY (backlog EH, shape 6). This was a bare `else` over every
			// kind that is not On, and the vocabulary carries Expression too: a pressure message reaching
			// the lane stripped its key from the stack, so under Lowest or Highest the pitch jumped to
			// another chord note, and when it was the only note the target went stale.
			//
			// **A NEGATIVE KEY IS CLAP'S WILDCARD** (`events.h`: "-1 if unspecified or wildcard") and lifts
			// them all — a host's choke-everything would otherwise leave every key held.
			if (e.key < 0) {
				count_ = 0;
				continue;
			}
			// REMOVED BY VALUE, NOT POPPED. `Rule::Last` reads arrival order, so releasing the middle
			// of a chord must leave the rest in the order they arrived — a swap-with-back would make
			// the oldest note suddenly the newest.
			size_t write = 0;
			for (size_t h = 0; h < count_; ++h) {
				if (held_[h] != e.key) {
					held_[write++] = held_[h];
				}
			}
			count_ = write;
		}
	}
	return anyOn;
}

int16_t HeldKeys::pick(Rule rule) const noexcept {
	if (count_ == 0) {
		return 0;
	}
	int16_t key = held_[count_ - 1];
	if (rule == Rule::Lowest) {
		for (size_t h = 0; h < count_; ++h) {
			key = std::min(key, held_[h]);
		}
	}
	else if (rule == Rule::Highest) {
		for (size_t h = 0; h < count_; ++h) {
			key = std::max(key, held_[h]);
		}
	}
	return key;
}

void Tracker::processNotes(const device::NoteEvent* in, size_t inCount, device::NoteEvent* out,
                           size_t outCapacity, size_t* outCount) noexcept {
	(void)held_.fold(in, inCount);
	recomputeTarget();

	// AND THE STREAM PASSES THROUGH UNCHANGED. A device whose whole job is to observe has no business
	// consuming — see `NoteOps::processNotes`.
	const size_t n = std::min(inCount, outCapacity);
	for (size_t i = 0; i < n; ++i) {
		out[i] = in[i];
	}
	if (outCount != nullptr) {
		*outCount = n;
	}
}

void Tracker::recomputeTarget() noexcept {
	if (held_.empty()) {
		// NOTHING HELD LEAVES THE TARGET WHERE IT WAS, rather than resetting it. A tracker that
		// snapped to a default on release would make every note-off a downward sweep of whatever it
		// drives, which is the opposite of what a glide is for.
		return;
	}
	target_ = static_cast<float>(held_.pick(rule_));
}

bool Tracker::busPitch(float* pitchSemitones) const noexcept {
	if (target_ < 0.0f || pitchSemitones == nullptr) {
		return false; // nothing played yet: the devices after it keep the bus's own note
	}
	// GLIDE ZERO IS THE RULE'S KEY, this piece — not the last tick's, which a note struck after the tick would miss.
	*pitchSemitones = glide_ <= 0.0f || pitch_ < 0.0f ? target_ : pitch_;
	return true;
}

void Tracker::tick() noexcept {
	if (target_ < 0.0f) {
		return; // nothing has ever been played
	}
	// THE PITCH IT HANDS THE BUS (`busPitch`), glided by the same one-pole as the output below, in SEMITONES —
	// so a keytracked corner after this Tracker slides between notes over the time its output does. The first
	// note arrives where it is played: there is nothing to slide from.
	const float coeff = std::clamp(1.0f - std::pow(glide_, 0.25f), 0.0f, 1.0f);
	pitch_ = glide_ <= 0.0f || pitch_ < 0.0f ? target_ : pitch_ + (target_ - pitch_) * coeff;
	// **A DISTANCE FROM THE ROOT, IN UNITS OF `Spread` OCTAVES.** The root reads 0, the root plus
	// `Spread` octaves reads +1, and the root minus `Spread` octaves reads −1 — so a note below the
	// one the player named modulates the other way, which is the whole point of the design.
	const float span = spreadOctaves_ * 12.0f;
	// A ZERO SPREAD WOULD DIVIDE, so it reports the root rather than a NaN that would travel into
	// every routed destination. The declared minimum is one octave, so this is a guard against a row
	// that arrived from somewhere else rather than against a setting a player can make.
	const float mapped =
	    span < 1e-6f ? 0.0f : std::clamp((target_ - rootNote_) / span, -1.0f, 1.0f);
	if (glide_ <= 0.0f) {
		current_ = mapped;
		return;
	}
	// A ONE-POLE TOWARD THE TARGET. The coefficient is per TICK rather than per sample because that is
	// what the host calls, and `blockSamples` is what makes a tick's duration knowable at all.
	current_ += (mapped - current_) * coeff;
}

} // namespace feathers::mod::tracker

namespace feathers::mod::tracker {

namespace {
Tracker& asTracker(void* storage) noexcept { return *static_cast<Tracker*>(storage); }
const Tracker& asTracker(const void* storage) noexcept {
	return *static_cast<const Tracker*>(storage);
}
} // namespace

// **TWO OPS TABLES OVER ONE OBJECT, WHICH IS THE POINT OF THE DEVICE.**
//
// This is that device. It declares
// `NoteOps` because it reads notes and `ModOps` because it publishes a source, and the two share one
// storage — `Descriptor` holds the pointers independently for precisely this reason, and a `lane` enum
// could not have expressed it.
//
// THE SIZES MUST AGREE AND DO, because they describe the same class. `RackEngine` allocates once from
// whichever pointer it reaches first.
const device::NoteOps kNoteOps {
	.stateBytes = sizeof(Tracker),
	.stateAlign = alignof(Tracker),
	.construct = [](void* storage) { new (storage) Tracker(); },
	.destruct = [](void* storage) { asTracker(storage).~Tracker(); },
	.prepare = [](void* storage, double sampleRate) { asTracker(storage).prepare(sampleRate); },
	.setFields =
	    [](void* storage, const float* fields, size_t fieldsPresent) {
		    asTracker(storage).setFields(fields, fieldsPresent);
	    },
	.processNotes =
	    [](void* storage, const device::NoteEvent* in, size_t inCount, device::NoteEvent* out,
	       size_t outCapacity, size_t* outCount) {
		    asTracker(storage).processNotes(in, inCount, out, outCapacity, outCount);
	    },
	// **THE BUS NOTE FOR THE DEVICES AFTER IT** — its Rule's key, through its Glide. Not its Root
	// or Spread: those map the modulation output, and a pitch is not a modulation amount.
	.busNote = [](const void* storage, float* pitch) { return asTracker(storage).busPitch(pitch); },
};

const device::ModOps kModOps {
	.stateBytes = sizeof(Tracker),
	.stateAlign = alignof(Tracker),
	.construct = [](void* storage) { new (storage) Tracker(); },
	.destruct = [](void* storage) { asTracker(storage).~Tracker(); },
	.prepare = [](void* storage, double sampleRate) { asTracker(storage).prepare(sampleRate); },
	.reset = [](void*) {},
	.setFields =
	    [](void* storage, const float* fields, size_t fieldsPresent) {
		    asTracker(storage).setFields(fields, fieldsPresent);
	    },
	// ONE TICK PER SAMPLE-BLOCK OF 32, matching the other modulators: the glide is the only thing that
	// moves between notes, and it does not need audio rate to be smooth.
	.blockSamples = 32,
	.tick = [](void* storage, const device::ModTickContext&) { asTracker(storage).tick(); },
	.outputAt =
	    [](const void* storage, size_t output) {
		    return output == 0 ? asTracker(storage).output() : 0.0f;
	    },
	// **NULL, AND THAT IS A STATEMENT ABOUT THIS DEVICE RATHER THAN AN OMISSION.** A playhead is a
	// position within a SHAPE — a stage or a segment — and a tracker has none: it holds a stack of
	// held notes and reports the one its rule selects. There is nothing running to be part-way
	// through, so it declines and a card draws no marker for it.
	//
	// The compiler is what asked: `-Wmissing-field-initializers` refused this table the moment
	// `ModOps` grew the entry, which is why a device cannot forget to answer the question.
	.playhead = nullptr,
};

} // namespace feathers::mod::tracker
