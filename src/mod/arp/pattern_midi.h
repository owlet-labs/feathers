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

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// **ONE PATTERN AS A MIDI FILE** (design §7.9), to drag into a DAW. The notes are not re-derived: a fresh `PatternDevice` is handed the patch's row
// and blocks, made to play that one pattern from the song's start at the given tempo, and what it EMITS is written —
// so ties, chords, ratchets, grace notes, Humanize, the hand's edits and Transpose come out exactly as heard.
//
// A CONSERVATIVE STANDARD MIDI FILE: format 0, one track, 960 ticks a quarter, a tempo and a 4/4 time signature, a
// bar-aligned loop of the pattern's cycle (the fewest whole cycles that fill whole bars, at most 8 bars; padded with
// silence to the next bar when no count of cycles lands on one). A note still sounding at the loop's end ends there.
//
// EVERY MODE ON ONE CHANNEL (channel 1). TB-GLIDE'S GLIDE IS STANDARD CHANNEL PITCH BEND (backlog QW: the MPE export
// opened many tracks in a DAW): it holds one note at a time, so the channel's bend is the note's; the bend range is
// set once at the start by RPN 0, sized to the clip's widest glide (`midiBendRangeFor`: 12, 24 or 48), and the bend is
// put back to centre before each new note. No MPE zone, no member channels. Message thread: it allocates.

namespace feathers::mod::arp::pat {

/// THE PATCH, as the editor reads it: the row (every field), and the hand's, the edits' and the play blocks (empty: none).
struct MidiSource {
	std::vector<float> row;
	std::vector<float> hand;
	std::vector<float> edits;
	std::vector<float> play;
};

inline constexpr uint16_t kMidiTicksPerQuarter = 960;
/// THE WIDEST BEND RANGE the file states (RPN 0), in semitones: a glide wider is clamped there.
inline constexpr int kMidiBendRange = 48;
/// **THE BEND RANGE FOR A CLIP** whose widest glide is `widest` semitones: 12, 24 or 48, the first that holds it.
[[nodiscard]] constexpr int midiBendRangeFor(double widest) noexcept {
	return widest <= 12.0 ? 12 : (widest <= 24.0 ? 24 : kMidiBendRange);
}
/// The most bars the loop runs to.
inline constexpr int kMidiMaxBars = 8;

/// **THE LOOP'S LENGTH IN BEATS** for a cycle `cycleBeats` long: the fewest whole cycles that fill whole 4/4 bars, at
/// most `kMidiMaxBars`; else the cycle (or the bars it reaches, capped) padded to the next bar.
[[nodiscard]] double midiLoopBeats(double cycleBeats) noexcept;

/// **PATTERN `pattern` OF THIS PATCH AS A .mid FILE'S BYTES**, played at `bpm` over its held (latched) chord, C4 if
/// none is latched.
[[nodiscard]] std::vector<uint8_t> patternMidi(const MidiSource& source, size_t pattern, double bpm);

/// The file's name: "Pattern 2.mid".
[[nodiscard]] std::string patternMidiName(size_t pattern);

} // namespace feathers::mod::arp::pat
