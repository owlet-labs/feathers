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

#include "mod/noteexpr/noteexpr.h"

#include <algorithm>
#include <cmath>
#include <new>

namespace feathers::mod::noteexpr {

namespace {

NoteExpressions& asDevice(void* storage) noexcept {
	return *static_cast<NoteExpressions*>(storage);
}
const NoteExpressions& asDevice(const void* storage) noexcept {
	return *static_cast<const NoteExpressions*>(storage);
}

} // namespace

void NoteExpressions::prepare(double) noexcept {
	// NOTHING RATE-DEPENDENT. This device smooths nothing and runs no shape — it reports what the
	// host last said about a note it is still holding. The entry exists because `ModOps` requires it.
	heldCount_ = 0;
	nextSerial_ = 1;
}

void NoteExpressions::setFields(const float* fields, size_t count) noexcept {
	// THE ROW'S OWN ORDER, and absent fields keep the declared default — `device_row.h`'s rule that a
	// field past `fieldsPresent` is ABSENT rather than zero, which for `Enabled` is the difference
	// between a fresh device and a bypassed one.
	if (fields == nullptr) {
		return;
	}
	if (count > static_cast<size_t>(Param::Rule)) {
		const float raw = fields[static_cast<size_t>(Param::Rule)];
		const int index = static_cast<int>(std::lround(raw));
		if (index >= 0 && index < static_cast<int>(std::size(kRuleNames))) {
			rule_ = static_cast<Rule>(index);
		}
	}
	if (count > static_cast<size_t>(Param::Enabled)) {
		enabled_ = fields[static_cast<size_t>(Param::Enabled)] >= 0.5f;
	}
}

size_t NoteExpressions::indexOfNote(int32_t noteId, int16_t channel, int16_t key) const noexcept {
	for (size_t i = 0; i < heldCount_; ++i) {
		// **MATCHED THE WAY CLAP ADDRESSES, WHICH MEANS -1 IS A WILDCARD.** A host that sends
		// expressions by note id leaves channel and key at -1 and vice versa, so an equality on all
		// three would match nothing for most hosts. `Synth::noteExpression` leans on the voice
		// manager's own matcher for the same reason; this is the device's smaller version of it.
		const bool idMatches = noteId == -1 || held_[i].noteId == -1 || held_[i].noteId == noteId;
		const bool channelMatches = channel == -1 || held_[i].channel == channel;
		const bool keyMatches = key == -1 || held_[i].key == key;
		if (idMatches && channelMatches && keyMatches) {
			return i;
		}
	}
	return kMaxHeld;
}

size_t NoteExpressions::selectedIndex() const noexcept {
	if (heldCount_ == 0) {
		return kMaxHeld;
	}
	size_t best = 0;
	for (size_t i = 1; i < heldCount_; ++i) {
		switch (rule_) {
		case Rule::Last:
			// THE NEWEST HELD NOTE, which is the maximum serial by construction.
			if (serial_[i] > serial_[best]) {
				best = i;
			}
			break;
		case Rule::Lowest:
			if (held_[i].key < held_[best].key) {
				best = i;
			}
			break;
		case Rule::Highest:
			if (held_[i].key > held_[best].key) {
				best = i;
			}
			break;
		case Rule::Max:
			// **HANDLED PER EXPRESSION IN `output`, NOT HERE**, because Max is the one rule that does
			// not pick a NOTE — the loudest note for pressure may not be the loudest for brightness.
			// This arm keeps `best` at the first held note so `selectedNoteForTest` still answers
			// something honest, and `output` overrides it.
			break;
		}
	}
	return best;
}

int32_t NoteExpressions::selectedNoteForTest() const noexcept {
	const size_t at = selectedIndex();
	return at >= kMaxHeld ? -1 : held_[at].noteId;
}

float NoteExpressions::output(size_t expression) const noexcept {
	if (expression >= std::size(kOutputs)) {
		return 0.0f;
	}
	// **BYPASSED PUBLISHES THE NEUTRAL, NOT ZERO.** The enable square means "this device is not
	// contributing", and for pan that is 0.5 rather than 0 — the same argument `kNeutral` makes about
	// an untouched note, applied to a device the player has switched off.
	if (!enabled_ || heldCount_ == 0) {
		return kNeutral[expression];
	}
	if (rule_ == Rule::Max) {
		float best = held_[0].value[expression];
		for (size_t i = 1; i < heldCount_; ++i) {
			best = std::max(best, held_[i].value[expression]);
		}
		return best;
	}
	const size_t at = selectedIndex();
	return at >= kMaxHeld ? kNeutral[expression] : held_[at].value[expression];
}

float NoteExpressions::outputForNote(size_t expression, int32_t noteId, int16_t channel, int16_t key) const noexcept {
	if (expression >= std::size(kOutputs)) {
		return 0.0f;
	}
	if (!enabled_) {
		return kNeutral[expression];
	}
	// THE WHOLE NOTE, matched as the lane matches it: a keyboard's notes all wear id -1, and by id
	// alone every voice read the first held note's expressions.
	if (const size_t at = indexOfNote(noteId, channel, key); at < heldCount_) {
		return held_[at].value[expression];
	}
	// NOT HELD — the collapsed answer, not zero. A voice whose key has come up is still sounding, and
	// dropping its modulation to zero mid-release would be a click the player did not ask for.
	return output(expression);
}

void NoteExpressions::processNotes(const device::NoteEvent* in, size_t inCount,
                                   device::NoteEvent* out, size_t outCapacity,
                                   size_t* outCount) noexcept {
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
			if (heldCount_ < kMaxHeld) {
				Held entry {};
				entry.noteId = e.noteId;
				entry.channel = e.channel;
				entry.key = e.key;
				// **A FRESH NOTE IS UNEXPRESSED, at each expression's own neutral.** Zeroing would
				// make a new note read as hard-left pan and silent volume until the host said
				// otherwise — and a host only sends an expression when it CHANGES, so "until
				// otherwise" can be the whole note.
				entry.value = kNeutral;
				held_[heldCount_] = entry;
				serial_[heldCount_] = nextSerial_++;
				++heldCount_;
			}
			break;
		}
		case device::NoteEvent::Kind::Off:
		case device::NoteEvent::Kind::Choke: {
			// **REMOVED BY SWAPPING THE LAST ENTRY DOWN**, which is why the serial is a separate array
			// rather than the index: order in `held_` means nothing and the serial is what carries
			// recency across a removal. EVERY NOTE THE EVENT NAMES — a wildcard off lifts them all —
			// walked from the top, because each removal swaps the last note into the hole.
			for (size_t at = heldCount_; at-- > 0;) {
				const bool idMatches = e.noteId == -1 || held_[at].noteId == -1 || held_[at].noteId == e.noteId;
				if (!idMatches || (e.channel != -1 && held_[at].channel != e.channel)
				    || (e.key != -1 && held_[at].key != e.key)) {
					continue;
				}
				held_[at] = held_[heldCount_ - 1];
				serial_[at] = serial_[heldCount_ - 1];
				--heldCount_;
			}
			break;
		}
		case device::NoteEvent::Kind::Expression: {
			const size_t at = indexOfNote(e.noteId, e.channel, e.key);
			if (at < heldCount_ && e.expressionId >= 0
			    && static_cast<size_t>(e.expressionId) < std::size(kOutputs)
			    && std::isfinite(e.expressionValue)) {
				held_[at].value[static_cast<size_t>(e.expressionId)] =
				    static_cast<float>(e.expressionValue);
			}
			break;
		}
		}

		// **EVERY EVENT PASSES THROUGH, WHATEVER WE DID WITH IT.** `NoteOps` states the rule that
		// makes this matter: a dropped note-OFF is a stuck note. This device observes; it must not be
		// possible for it to swallow anything, including the events it did not understand.
		if (*outCount < outCapacity) {
			out[*outCount] = e;
			++(*outCount);
		}
	}
}

//------------------------------------------------------------------------------------------------//
// THE TABLES                                                                                       //
//------------------------------------------------------------------------------------------------//

const device::NoteOps kNoteOps {
	.stateBytes = sizeof(NoteExpressions),
	.stateAlign = alignof(NoteExpressions),
	.construct = [](void* storage) { new (storage) NoteExpressions(); },
	.destruct = [](void* storage) { asDevice(storage).~NoteExpressions(); },
	.prepare = [](void* storage, double sampleRate) { asDevice(storage).prepare(sampleRate); },
	.setFields =
	    [](void* storage, const float* fields, size_t fieldsPresent) {
		    asDevice(storage).setFields(fields, fieldsPresent);
	    },
	.processNotes =
	    [](void* storage, const device::NoteEvent* in, size_t inCount, device::NoteEvent* out,
	       size_t outCapacity, size_t* outCount) {
		    asDevice(storage).processNotes(in, inCount, out, outCapacity, outCount);
	    },
};

const device::ModOps kModOps {
	.stateBytes = sizeof(NoteExpressions),
	.stateAlign = alignof(NoteExpressions),
	.construct = [](void* storage) { new (storage) NoteExpressions(); },
	.destruct = [](void* storage) { asDevice(storage).~NoteExpressions(); },
	.prepare = [](void* storage, double sampleRate) { asDevice(storage).prepare(sampleRate); },
	.reset = [](void*) {},
	.setFields =
	    [](void* storage, const float* fields, size_t fieldsPresent) {
		    asDevice(storage).setFields(fields, fieldsPresent);
	    },
	// **THE TICK LENGTH IS THE TRACKER'S, AND FOR A STRONGER REASON: THIS DEVICE DOES NOT TICK AT
	// ALL.** It holds no shape and smooths nothing — the value it publishes changed when the host
	// said so, not when time passed. The entry is required and the number only says how often a host
	// that wanted to call `tick` would.
	.blockSamples = 32,
	.tick = [](void*, const device::ModTickContext&) {},
	.outputAt =
	    [](const void* storage, size_t output) { return asDevice(storage).output(output); },
	.outputForNote =
	    [](const void* storage, size_t output, int32_t noteId, int16_t channel, int16_t key) {
		    return asDevice(storage).outputForNote(output, noteId, channel, key);
	    },
	.playhead = nullptr,
	// NO PLAYHEAD — the Tracker's own words apply unchanged: a playhead is a position within a SHAPE,
	// and this device has none.
};

} // namespace feathers::mod::noteexpr
