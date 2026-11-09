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

// A DEVICE WITH BOTH `.audio` AND `.notes` OVER ONE STORAGE — tests only.
//
// No registered device combines the two (the tracker is mod + notes), and WEAVE needs to: one shared
// string driven by real note events with their velocities, which `AudioOps::setNoteContext` (one pitch
// and a gate) cannot carry. This probe proves the road before WEAVE rides it. It passes both streams
// through untouched and WRITES DOWN, in the order they happened, every note event it was handed and
// every `processStereo` call — so a case can assert what arrived, at which offset, and before which
// audio, as values rather than as a difference between two renders.

#include "device/registry.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <new>

namespace feathers::test {

struct AudioNotesProbe {
	struct Entry {
		enum class What : uint8_t { Note, Audio };
		What what = What::Audio;
		device::NoteEvent::Kind kind = device::NoteEvent::Kind::On;
		uint32_t timeSamples = 0;
		int16_t key = 0;
		double velocity = 0.0;
		int frames = 0;
	};
	/// FIXED, so recording allocates nothing on the audio thread; a full log stops recording and says
	/// so through `dropped` rather than growing.
	std::array<Entry, 512> log {};
	size_t count = 0;
	size_t dropped = 0;
	/// Every `processNotes` call, including the empty ones a host makes at each block's start.
	size_t notesCalls = 0;
	/// THE LAST NOTE CONTEXT THE ENGINE HANDED IT (`setNoteContext`) — the held-stack summary, which is
	/// the engine's reading of the stream rather than the stream itself.
	device::NoteContext lastContext {};
	size_t contextCalls = 0;

	void record(const Entry& e) {
		if (count < log.size()) {
			log[count++] = e;
		}
		else {
			++dropped;
		}
	}
	void clear() {
		count = 0;
		dropped = 0;
		notesCalls = 0;
		contextCalls = 0;
	}
};

/// THE LIVE INSTANCE, set by `construct` and cleared by `destruct` — the probe runs on the bus only
/// (`perVoiceControl` is -1), so the engine builds one.
inline AudioNotesProbe* g_audioNotesProbe = nullptr;

inline constexpr device::AudioOps kAudioNotesProbeAudio {
	.stateBytes = sizeof(AudioNotesProbe),
	.stateAlign = alignof(AudioNotesProbe),
	.construct = [](void* s) { g_audioNotesProbe = new (s) AudioNotesProbe(); },
	.destruct =
	    [](void* s) {
		    auto* p = static_cast<AudioNotesProbe*>(s);
		    if (g_audioNotesProbe == p) {
			    g_audioNotesProbe = nullptr;
		    }
		    p->~AudioNotesProbe();
	    },
	.prepare = [](void*, double, int) {},
	.reset = [](void*) {},
	.setFields = [](void*, const float*, size_t) {},
	.setNoteContext =
	    [](void* s, const device::NoteContext& note) {
		    auto* p = static_cast<AudioNotesProbe*>(s);
		    p->lastContext = note;
		    ++p->contextCalls;
	    },
	.processStereo =
	    [](void* s, float*, float*, int frames) {
		    AudioNotesProbe::Entry e;
		    e.what = AudioNotesProbe::Entry::What::Audio;
		    e.frames = frames;
		    static_cast<AudioNotesProbe*>(s)->record(e);
	    },
};

/// THE SAME SIZE AND ALIGNMENT AS THE AUDIO OPS, because the engine builds ONE storage from the first
/// ops it finds and hands it to both (`RackEngine::provisionFor`).
inline constexpr device::NoteOps kAudioNotesProbeNotes {
	.stateBytes = sizeof(AudioNotesProbe),
	.stateAlign = alignof(AudioNotesProbe),
	.construct = kAudioNotesProbeAudio.construct,
	.destruct = kAudioNotesProbeAudio.destruct,
	.prepare = [](void*, double) {},
	.setFields = [](void*, const float*, size_t) {},
	.processNotes =
	    [](void* s, const device::NoteEvent* in, size_t inCount, device::NoteEvent* out, size_t outCapacity,
	       size_t* outCount) {
		    auto* p = static_cast<AudioNotesProbe*>(s);
		    ++p->notesCalls;
		    const size_t n = std::min(inCount, outCapacity);
		    for (size_t i = 0; i < n; ++i) {
			    AudioNotesProbe::Entry e;
			    e.what = AudioNotesProbe::Entry::What::Note;
			    e.kind = in[i].kind;
			    e.timeSamples = in[i].timeSamples;
			    e.key = in[i].key;
			    e.velocity = in[i].velocity;
			    p->record(e);
			    // PASS-THROUGH: in the synth what leaves the lane is what the voices play.
			    out[i] = in[i];
		    }
		    *outCount = n;
	    },
};

inline const device::Descriptor kAudioNotesProbe {
	.traits = { .tag = "ANPR", .name = "Audio Notes Probe", .hasCard = false, .hasExpanded = false },
	.params = {},
	.outputs = {},
	.audio = &kAudioNotesProbeAudio,
	.notes = &kAudioNotesProbeNotes,
};

} // namespace feathers::test
