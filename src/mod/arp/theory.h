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

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>

// THE PATTERN DEVICE'S MUSIC THEORY (terrain-arp design §9, §10) — scales, chords stacked in a scale, voicings, and
// voice leading. Pure functions of their inputs, with no allocation, so the emitter can call them on the audio thread
// and a case can hold them without a host. Everything is 12-TET (design §17: microtones need a per-note tuning an
// invented note cannot carry).

namespace feathers::mod::arp::theory {

//-------------------------------------------------------------------------------------------------//
// Scales                                                                                           //
//-------------------------------------------------------------------------------------------------//

/// ONE SCALE: its name and its pitch classes as a 12-bit mask above the root (bit 0, the root, always set).
struct ScaleDef {
	const char* name;
	uint16_t mask;
};

constexpr uint16_t maskOf(std::initializer_list<int> steps) {
	uint16_t m = 0;
	for (const int s : steps) {
		m = static_cast<uint16_t>(m | (1u << s));
	}
	return m;
}

/// **THE LIST, IN THE DROPDOWN'S ORDER** (design §9). Names that would duplicate a mask are folded into one entry's name
/// (Romanian Minor is Dorian ♯4; Bhairav is Double Harmonic), and a case asserts every mask is distinct, so the list
/// cannot offer the same scale twice under two names.
inline constexpr ScaleDef kScales[] = {
	{ "Major", maskOf({ 0, 2, 4, 5, 7, 9, 11 }) },
	{ "Dorian", maskOf({ 0, 2, 3, 5, 7, 9, 10 }) },
	{ "Phrygian", maskOf({ 0, 1, 3, 5, 7, 8, 10 }) },
	{ "Lydian", maskOf({ 0, 2, 4, 6, 7, 9, 11 }) },
	{ "Mixolydian", maskOf({ 0, 2, 4, 5, 7, 9, 10 }) },
	{ "Minor", maskOf({ 0, 2, 3, 5, 7, 8, 10 }) },
	{ "Locrian", maskOf({ 0, 1, 3, 5, 6, 8, 10 }) },
	{ "Harmonic Minor", maskOf({ 0, 2, 3, 5, 7, 8, 11 }) },
	{ "Melodic Minor", maskOf({ 0, 2, 3, 5, 7, 9, 11 }) },
	{ "Harmonic Major", maskOf({ 0, 2, 4, 5, 7, 8, 11 }) },
	{ "Dorian #4 (Romanian Minor)", maskOf({ 0, 2, 3, 6, 7, 9, 10 }) },
	{ "Phrygian Dominant", maskOf({ 0, 1, 4, 5, 7, 8, 10 }) },
	{ "Lydian Dominant", maskOf({ 0, 2, 4, 6, 7, 9, 10 }) },
	{ "Altered", maskOf({ 0, 1, 3, 4, 6, 8, 10 }) },
	{ "Lydian Augmented", maskOf({ 0, 2, 4, 6, 8, 9, 11 }) },
	{ "Locrian Natural 2", maskOf({ 0, 2, 3, 5, 6, 8, 10 }) },
	{ "Major Pentatonic", maskOf({ 0, 2, 4, 7, 9 }) },
	{ "Minor Pentatonic", maskOf({ 0, 3, 5, 7, 10 }) },
	{ "Egyptian", maskOf({ 0, 2, 5, 7, 10 }) },
	{ "Blues", maskOf({ 0, 3, 5, 6, 7, 10 }) },
	{ "Major Blues", maskOf({ 0, 2, 3, 4, 7, 9 }) },
	{ "Man Gong", maskOf({ 0, 3, 5, 8, 10 }) },
	{ "Hirajoshi", maskOf({ 0, 2, 3, 7, 8 }) },
	{ "In Sen", maskOf({ 0, 1, 5, 7, 10 }) },
	{ "Iwato", maskOf({ 0, 1, 5, 6, 10 }) },
	{ "Kumoi", maskOf({ 0, 2, 3, 7, 9 }) },
	{ "Yo", maskOf({ 0, 2, 5, 7, 9 }) },
	{ "Hungarian Minor", maskOf({ 0, 2, 3, 6, 7, 8, 11 }) },
	{ "Hungarian Major", maskOf({ 0, 3, 4, 6, 7, 9, 10 }) },
	{ "Double Harmonic (Bhairav)", maskOf({ 0, 1, 4, 5, 7, 8, 11 }) },
	{ "Neapolitan Major", maskOf({ 0, 1, 3, 5, 7, 9, 11 }) },
	{ "Neapolitan Minor", maskOf({ 0, 1, 3, 5, 7, 8, 11 }) },
	{ "Persian", maskOf({ 0, 1, 4, 5, 6, 8, 11 }) },
	{ "Enigmatic", maskOf({ 0, 1, 4, 6, 8, 10, 11 }) },
	{ "Spanish 8-Tone", maskOf({ 0, 1, 3, 4, 5, 6, 8, 10 }) },
	{ "Todi", maskOf({ 0, 1, 3, 6, 7, 8, 11 }) },
	{ "Marwa", maskOf({ 0, 1, 4, 6, 7, 9, 11 }) },
	{ "Purvi", maskOf({ 0, 1, 4, 6, 7, 8, 11 }) },
	{ "Whole Tone", maskOf({ 0, 2, 4, 6, 8, 10 }) },
	{ "Diminished (W-H)", maskOf({ 0, 2, 3, 5, 6, 8, 9, 11 }) },
	{ "Diminished (H-W)", maskOf({ 0, 1, 3, 4, 6, 7, 9, 10 }) },
	{ "Augmented", maskOf({ 0, 3, 4, 7, 8, 11 }) },
	{ "Prometheus", maskOf({ 0, 2, 4, 6, 9, 10 }) },
	{ "Bebop Dominant", maskOf({ 0, 2, 4, 5, 7, 9, 10, 11 }) },
	{ "Bebop Major", maskOf({ 0, 2, 4, 5, 7, 8, 9, 11 }) },
	{ "Chromatic", 0x0FFFu },
};
inline constexpr size_t kScaleCount = std::size(kScales);

/// A SCALE READY TO INDEX: its steps above the root, ascending.
struct Scale {
	std::array<int8_t, 12> steps {};
	int count = 0;
};
/// Scale `index` (clamped into the list).
[[nodiscard]] Scale scaleOf(size_t index) noexcept;
/// **THE KEY OF SCALE DEGREE `degree` ABOVE `rootKey`** — any integer degree; each `count` degrees is an octave.
[[nodiscard]] int keyOfDegree(const Scale& s, int rootKey, int degree) noexcept;
/// The degree whose key is the nearest at or below `key` (so `keyOfDegree(degreeAtOrBelow(k)) <= k`).
[[nodiscard]] int degreeAtOrBelow(const Scale& s, int rootKey, int key) noexcept;
/// **THE NEAREST KEY IN THE SCALE**, a tie snapping DOWN (design §8, Pool).
[[nodiscard]] int snapToScale(const Scale& s, int rootKey, int key) noexcept;

//-------------------------------------------------------------------------------------------------//
// Chords                                                                                           //
//-------------------------------------------------------------------------------------------------//

/// THE SIZE BOX (design §9), in its dropdown order.
enum class ChordSize : uint8_t { Single, Power, Triad, Sus2, Sus4, Sixth, Seventh, Add9, Ninth, Eleventh, Thirteenth, kCount };
inline constexpr const char* kChordSizeNames[] = { "Single", "Power", "Triad", "Sus2", "Sus4", "6th",
	                                               "7th",    "add9",  "9th",   "11th", "13th" };
static_assert(std::size(kChordSizeNames) == static_cast<size_t>(ChordSize::kCount));

/// THE SPREAD BOX (design §9): how a close voicing is opened. Drop 2 lowers the second-highest tone an octave, Drop 3
/// the third-highest, Drop 2+4 both the second and fourth; Open raises every other tone from the bottom (the 2nd, 4th,
/// ...) an octave; Wide is Open with the bass lowered an octave as well.
enum class Spread : uint8_t { Close, Drop2, Drop3, Drop24, Open, Wide, kCount };
inline constexpr const char* kSpreadNames[] = { "Close", "Drop 2", "Drop 3", "Drop 2+4", "Open", "Wide" };
static_assert(std::size(kSpreadNames) == static_cast<size_t>(Spread::kCount));

/// The most tones a chord has (a 13th).
inline constexpr size_t kMaxTones = 7;

/// **THE CHORD'S DEGREES ABOVE ITS OWN**, stacked in the SCALE (design §10.1): a triad is d, d+2, d+4, so a pentatonic
/// "triad" is the open, quartal-ish shape every-other-step gives. `thin` (0..2) drops the 5th, then the root as well,
/// and never the 3rd or the 7th (a chord of fewer than three tones is not thinned). Returns the count.
size_t chordDegrees(ChordSize size, int thin, int* out) noexcept;

/// A VOICING: keys, ascending.
struct Voicing {
	std::array<int16_t, 2 * kMaxTones> keys {};
	int count = 0;

	friend bool operator==(const Voicing& a, const Voicing& b) noexcept {
		if (a.count != b.count) {
			return false;
		}
		for (int i = 0; i < a.count; ++i) {
			if (a.keys[static_cast<size_t>(i)] != b.keys[static_cast<size_t>(i)]) {
				return false;
			}
		}
		return true;
	}
};

/// **THE CHORD ON DEGREE `degree` OF THE SCALE**, inversion `inversion` (its lowest `inversion` tones raised an octave,
/// modulo the tone count), opened by `spread`, then moved by `octaves` octaves. Ascending.
[[nodiscard]] Voicing voiceChord(const Scale& s, int rootKey, int degree, ChordSize size, int thin, int inversion,
                                 Spread spread, int octaves) noexcept;

/// **HOW FAR A VOICING MOVES FROM THE ONE BEFORE** (design §10.2): with as many tones, the sum of |a_j − b_j| over the
/// two ascending lists (which is the least total motion any pairing gives — a case checks it against every
/// permutation); otherwise each tone's distance to the nearest tone of the other, both ways. 0 against an empty one.
[[nodiscard]] int leadCost(const Voicing& a, const Voicing& prev) noexcept;

/// A SET OF PITCH CLASSES, bit i for class i.
using PitchClasses = uint16_t;
[[nodiscard]] PitchClasses pitchClassesOf(const Voicing& v) noexcept;
[[nodiscard]] int countIn(PitchClasses set, PitchClasses of) noexcept;

/// WHERE A VOICING MAY SIT: every key within [low, high].
struct Window {
	int low = 36;
	int high = 96;
};

/// ONE CHORD STEP TO VOICE: its degree, size and thinning; a fixed inversion, or `kAuto` to voice-lead.
struct ChordSpec {
	int degree = 0;
	ChordSize size = ChordSize::Triad;
	int thin = 0;
	int inversion = 0;
	Spread spread = Spread::Close;
	/// Take the second-cheapest candidate instead of the cheapest (the Voicing gene, design §10.2).
	bool second = false;
	/// **A CHORD FROM OUTSIDE THE SCALE** (Progression's borrowed chords and secondary dominants, design §11-quater):
	/// stacked in `kScales[scale]` on a tonic `tonic` semitones above `rootKey`, its `degree` counted there. -1: the scale.
	int scale = -1;
	int tonic = 0;
	/// **VOICE LEADING, 0..1** (the Voice Leading pin). Every candidate is scored by `L · leadCost(c, prev) + (1 − L) · leadCost(c, centre)`, the
	/// centre being the window's centre as one key — the reference a chord with nothing before it is measured against. At 1
	/// the score is today's least motion EXACTLY (the second term is 0·x = 0); at 0 every chord is the one it would be
	/// standing alone (block chords near the centre, whatever came before); between, a linear blend, so as L rises the
	/// winner's motion from the chord before can only fall (a parametric minimum is monotone in its weight).
	float leading = 1.0f;
};
inline constexpr int kAuto = -1;

/// THE HELD-NOTE PULL (design §10.3): how many semitones of motion one held pitch class inside a voicing is worth. Large
/// enough that a voicing holding a held note wins unless it must leave the window.
inline constexpr int kHeldWeight = 24;

/// **THE DEGREE A CHORD STEP PLAYS, PULLED TOWARD THE HELD NOTES** (design §10.3): among `degree − 1 .. degree + 1`, the
/// one whose chord (in the scale) holds the most of `held`; a tie keeps `degree` itself, then the lower. With fewer than
/// two held pitch classes, `degree` unchanged.
[[nodiscard]] int pullDegree(const Scale& s, int rootKey, const ChordSpec& spec, PitchClasses held) noexcept;

/// **ONE STEP VOICED AGAINST THE ONE BEFORE** (design §10.2): every inversion (or the fixed one) at every octave
/// placement inside the window; the least `leadCost(c, prev) − kHeldWeight · |classes(c) ∩ held|` wins, a tie going to
/// the lower voicing (smaller key sum) and then the lower inversion. With no candidate inside the window, the placement
/// nearest its centre. `prev` empty: measured against the window's centre as one key.
[[nodiscard]] Voicing voiceStep(const Scale& s, int rootKey, const ChordSpec& spec, const Window& w,
                                const Voicing& prev, PitchClasses held) noexcept;

/// **A WHOLE CYCLE, VOICED UNTIL IT WRAPS** (design §10.2), so the voicing at a step never depends on where playback began:
/// pass 1 leads from an empty reference; each later pass leads again from the LAST chord of the one before, until a pass
/// ends where it began (at most `kMaxPasses`), so the wrap is voice-led too. `specs[i]` null means "no chord at step i"
/// (its `out[i]` is empty, and the chain skips
/// it). At most `kMaxCycle` steps.
inline constexpr size_t kMaxCycle = 64;
/// MEASURED (test_arp_theory, 2000 random cycles): 1996 settle within 6 passes, all 2000 within 8.
inline constexpr int kMaxPasses = 8;
void voiceCycle(const Scale& s, int rootKey, const ChordSpec* const* specs, size_t length, const Window& w,
                PitchClasses held, Voicing* out) noexcept;

} // namespace feathers::mod::arp::theory
