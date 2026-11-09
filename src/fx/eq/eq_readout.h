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

// THE EQ'S MUSICAL READOUT: every frequency the EQ shows reads as a
// note, its cents, and Hz — "A2 +5c / 220.6 Hz".
//
// THE HOUSE NAMING, BY DESIGN: the reference's, where MIDI 60 is C3 — the same as
// `Unit::Note` in value_box.cpp, so one plugin names one pitch one way. 220 Hz is A2 here, not A3.
// A = 440 Hz; there is no global tuning reference to follow.

#include <cmath>
#include <cstdio>
#include <string>

namespace feathers::fx::eq {

struct NoteReading {
	/// The nearest equal-tempered MIDI note.
	long midi = 69;
	/// Signed distance from it, rounded, in [-50, +50].
	long cents = 0;
};

[[nodiscard]] inline NoteReading noteOf(double hz) noexcept {
	const double semis = 69.0 + 12.0 * std::log2(hz / 440.0);
	NoteReading r;
	r.midi = std::lround(semis);
	r.cents = std::lround((semis - static_cast<double>(r.midi)) * 100.0);
	return r;
}

/// The reference's naming, arithmetic included: `octave = floor(n / 12) - 2` puts 60 at C3.
[[nodiscard]] inline std::string noteName(long midi) {
	static const char* const kLetters[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
	const long within = ((midi % 12) + 12) % 12;
	const long octave = static_cast<long>(std::floor(static_cast<double>(midi) / 12.0)) - 2;
	// Sized for any `long`, which is what -Wformat-truncation checks against.
	char out[32] = {};
	std::snprintf(out, sizeof(out), "%s%ld", kLetters[within], octave);
	return out;
}

/// Hz with the precision following the magnitude: "86.3 Hz", "220.6 Hz", "1.77 kHz", "12.4 kHz".
[[nodiscard]] inline std::string hertzText(double hz) {
	char out[24] = {};
	if (hz >= 10000.0) {
		std::snprintf(out, sizeof(out), "%.1f kHz", hz / 1000.0);
	}
	else if (hz >= 1000.0) {
		std::snprintf(out, sizeof(out), "%.2f kHz", hz / 1000.0);
	}
	else {
		std::snprintf(out, sizeof(out), "%.1f Hz", hz);
	}
	return out;
}

/// "A2 +5c / 220.6 Hz". The cents are always signed, so "+0c" says "on the note" rather than leaving
/// the reader to wonder whether a sign was dropped.
[[nodiscard]] inline std::string frequencyReadout(double hz) {
	const NoteReading n = noteOf(hz);
	char cents[32] = {};
	std::snprintf(cents, sizeof(cents), "%+ldc", n.cents);
	return noteName(n.midi) + " " + cents + " / " + hertzText(hz);
}

} // namespace feathers::fx::eq
