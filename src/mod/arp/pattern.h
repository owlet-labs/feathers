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

#include "mod/arp/arp_core.h"
#include "mod/arp/genes.h"
#include "mod/arp/theory.h"

#include <array>
#include <cstddef>
#include <cstdint>

// THE PATTERN BUILDER (terrain-arp design §5, §8–§11): a place in the gene space, a Mode, the scale boxes and the held
// keys become one cycle of steps — which keys each step strikes, how long, how loud, and whether it rests, ties,
// slides or ratchets. A pure function: the same inputs give the same pattern, which is what makes a scrub, a loop
// and a bounce play the same notes with nothing stored.
//
// The emitter (the device, build step 4) turns a `PatternStep` into note events on the step grid. This file never
// sees a sample.

namespace feathers::mod::arp::pattern {

using genes::Mode;

/// **DRUM'S EIGHT LANES** (design §11-ter, DECIDED 2026-10-31): each a note number and an on/off, per pattern, defaulting to
/// General MIDI's kit. The lane is its role (the kick keeps the downbeat, the snare and clap the backbeat, the hats
/// subdivide); the note is only where it lands.
enum class DrumLane : uint8_t { Kick, Snare, ClosedHat, OpenHat, Clap, LowTom, HighTom, Ride, kCount };
inline constexpr size_t kDrumLanes = static_cast<size_t>(DrumLane::kCount);
inline constexpr std::array<int16_t, kDrumLanes> kDrumDefaultNotes { 36, 38, 42, 46, 39, 45, 50, 51 };
inline constexpr const char* kDrumLaneNames[kDrumLanes] = { "Kick", "Snare", "Closed Hat", "Open Hat",
	                                                         "Clap", "Low Tom", "High Tom", "Ride" };
inline constexpr const char* kDrumLaneShort[kDrumLanes] = { "Kick", "Snare", "CH", "OH", "Clap", "LT", "HT", "Ride" };
/// MODULATION'S LANES, NAMED AS THEMSELVES, never the kit's.
inline constexpr const char* kModLaneNames[kDrumLanes] = { "Mod 1", "Mod 2", "Mod 3", "Mod 4", "Mod 5", "Mod 6", "Mod 7", "Mod 8" };
/// One pattern's lane map.
struct DrumLanes {
	std::array<int16_t, kDrumLanes> note = kDrumDefaultNotes;
	uint8_t on = 0xFF; ///< a bit per lane
	friend bool operator==(const DrumLanes&, const DrumLanes&) = default;
	[[nodiscard]] bool laneOn(size_t l) const noexcept { return ((on >> l) & 1u) != 0u; }
};

/// HOW THE HELD KEYS ARE READ (design §8, DECIDED).
enum class Keys : uint8_t { Transpose, Pool, kCount };
inline constexpr const char* kKeysNames[] = { "Transpose", "Pool" };

/// EVERYTHING A PATTERN IS BUILT FROM besides the place and the held keys.
struct Recipe {
	Mode mode = Mode::Melody;
	Keys keys = Keys::Transpose;
	int root = 0;       ///< the Root box, a pitch class (Pool's tonic; Transpose takes the key)
	size_t scale = 0;   ///< index into `theory::kScales`
	int degree = 0;     ///< the Degree box
	theory::ChordSize size = theory::ChordSize::Triad;
	int inversion = theory::kAuto;
	theory::Spread spread = theory::Spread::Close;
	size_t length = 16; ///< steps in the cycle, 1..kMaxSteps
	float gate = 0.5f;  ///< the card's Gate, a share of a step
	float velocity = 0.8f;
	bool fixedVelocity = false;
	float accent = 0.25f; ///< what an accent adds to the velocity
	bool pitchOnNotes = true; ///< TB: pitches advance on note steps only (design §11.4)
	/// Voice-lead the chords (design §10.2). False — the terrain's picture, which reads only degrees and tone counts —
	/// takes root position, the cheap road (MEASURED: voicing is nearly all of a Chords build).
	bool voice = true;
	genes::Pins pins {};
	/// **THE LIKES' WARP** (pattern_warp.h), or null: the plain decode. The caller keeps it alive across `build`.
	const genes::Warp* warp = nullptr;
	/// DRUM: which lanes play (the notes are the device's to sound; the build only decides the hits).
	DrumLanes drum {};
	/// PROGRESSION: how many beats one step lasts (the pattern's Time Base and Rate; 0 when the steps are in Hz, read as
	/// a sixteenth) — the bar a change per bar is counted in (design §11-quater; 4/4).
	double stepBeats = 0.25;
};

/// **APPROACH NOTES** (design §10-ter). The weights are the lead's design, TO BE CONFIRMED.
/// WHAT KIND of pickup: a scale step from the target, a chromatic half step, or the target itself sounded early (an
/// anticipation: it ties into the chord). None: not a pickup.
enum class ApproachKind : uint8_t { None, Scale, Chromatic, Anticipation };
/// WHICH VOICE of the new chord is approached: its top, its bass, or any one tone.
enum class ApproachVoice : uint8_t { Top, Bass, Any };
/// HOW MANY PICKUPS a change approached gets: one far more often than two, three rarely — cumulative shares of the
/// change's reading under the pin (1 below the first, 2 below the second, 3 above).
inline constexpr float kApproachOne = 0.75f;
inline constexpr float kApproachTwo = 0.95f;
/// WHICH KIND, cumulative: scale 50%, chromatic 30%, anticipation 20%.
inline constexpr float kApproachScale = 0.5f;
inline constexpr float kApproachChromatic = 0.8f;
/// WHICH VOICE, cumulative: top 50%, bass 25%, any 25%.
inline constexpr float kApproachTop = 0.5f;
inline constexpr float kApproachBass = 0.75f;
/// A pickup sounds this share of its step (detached into the change); an anticipation's last runs a hair into it.
inline constexpr float kApproachGate = 0.9f;
/// A pickup's velocity, as a share of the change's (it leads into the strike, not over it).
inline constexpr float kApproachVelocity = 0.85f;

/// ONE STEP OF THE CYCLE.
struct PatternStep {
	enum class Kind : uint8_t { Rest, Note, Tie };
	Kind kind = Kind::Rest;
	/// The keys a Note strikes, ascending: one in the line modes, the chord's tones in Chords. In TB with Pitch on Notes
	/// the key is resolved at play time from `pitchIndex` (`PatternCycle::keyAt`).
	std::array<int16_t, theory::kMaxTones> keys {};
	int count = 0;
	/// Chords: tones shared with the chord before that HOLD instead of re-striking (a held chord step).
	uint16_t holdMask = 0;
	float velocity = 0.8f;
	/// How long the note lasts, as a share of the step (a tie or a slide extends it into the next).
	float gate = 0.5f;
	bool accent = false;
	/// Overlaps the NEXT note (a 303 slide, a Melody Mono slur): the next On lands before this Off.
	bool slide = false;
	/// Hits in the step (1 = no ratchet); a Melody Mono grace note is `grace` instead.
	uint8_t ratchet = 1;
	bool grace = false;
	/// The grace note's key: one scale degree above the note (Melody Mono's Detail).
	int16_t graceKey = 60;
	/// The unquantised contour (0..1) and the chord's degree, for the modulation sources.
	float contour = 0.5f;
	int degree = 0;
	/// The octave (±12 or 0) the Octave gene put on this step — kept apart so Pitch on Notes can add it to the
	/// polymeter's key.
	int octave = 0;
	/// TB with Pitch on Notes: this note's place among the cycle's notes (0-based).
	int noteIndex = -1;
	/// **DRUM** (design §11-ter): a bit per lane for each kind of hit, each lane's velocity, and each lane's length in steps
	/// (the open hat's runs until the next closed hat chokes it). A lane never hits twice on one step: one bit.
	uint8_t drumHits = 0;
	uint8_t drumAccent = 0;
	uint8_t drumGhost = 0;
	uint8_t drumRoll = 0; ///< these lanes roll `ratchet` times
	std::array<float, kDrumLanes> drumVelocity {};
	std::array<float, kDrumLanes> drumLength {};
	/// **EDITED BY HAND** (design §15-bis): this step is the overlay's, not the terrain's — its key is `keys[0]` as set.
	bool edited = false;
	/// **PROGRESSION'S CHORD** (design §11-quater), on every step it sounds through: its root in semitones above the tonic
	/// (-1: none), its pitch classes above that root, and whether a NEW chord starts here (a re-strike of the same chord
	/// does not) — what the lane's numeral and the DAW's words are read from (`numeralOf`).
	int8_t chordRoot = -1;
	uint16_t chordMask = 0;
	bool chordStart = false;
	/// **A PICKUP** (the Approach pin, design §10-ter): a single note replacing a step before a chord change, resolving
	/// into `approachTarget`, a tone of the new chord, by its kind's interval. `approachIndex` counts back from the change
	/// (1 is the step just before it). Not a pickup: kind None.
	ApproachKind approach = ApproachKind::None;
	ApproachVoice approachVoice = ApproachVoice::Top;
	uint8_t approachIndex = 0;
	int16_t approachTarget = 0;
};

/// A WHOLE CYCLE.
struct PatternCycle {
	std::array<PatternStep, genes::kMaxSteps> steps {};
	size_t length = 0;
	/// TB with Pitch on Notes: the pitch sequence the notes walk, its length P, and the notes in one cycle n.
	std::array<int16_t, 16> pitchSeq {};
	int pitchLength = 0;
	int notesPerCycle = 0;
	/// THE KEY THE CYCLE WAS SPELLED FROM (the lowest held key under Transpose, the Root box's under Pool) — a hand edit's
	/// pitch is stored from it, so an edit made over the window's C4 decode is heard at whatever key is held.
	int rootKey = 60;
	/// Any held key at all (nothing plays without one: design §17, "silent with no keys").
	bool playable = false;

	/// THE KEY STEP k OF CYCLE `cycle` PLAYS — its own, or under Pitch on Notes the polymeter's
	/// `pitchSeq[(cycle·n + noteIndex) mod P]` (design §11.4), a closed form of the song position.
	[[nodiscard]] int keyAt(size_t k, int64_t cycle) const noexcept;
};

/// **BUILD ONE CYCLE.** `held` is the arp's chord (empty: nothing playable).
void build(const genes::Place& place, const Recipe& r, const HeldChord& held, PatternCycle& out) noexcept;

/// **THE ROOT UNDER KEYS TRANSPOSE** (backlog QR; design §7.7): the NEWEST press — the keys struck together, one block's
/// (`StepEmitter` groups them under one order) — and the lowest of those. A new key moves the pattern whether or not the
/// old one is still down or latched; letting the newest go falls back to the newest still held. 127 when none is held.
[[nodiscard]] int transposeRootOf(const HeldChord& held) noexcept;

/// **ONE STEP'S HAND EDIT** (scoped to the stance: a manual edit applies only within the stance it was made in): what
/// the step becomes, and the SIGNATURE of the terrain's step it was made over (its kind and key; in Drum, its hits) — so a
/// step the terrain no longer decodes the same (a modulated place, a new scale, a shorter cycle) plays the terrain's own.
struct StepEdit {
	bool on = false;
	PatternStep::Kind kind = PatternStep::Kind::Rest;
	/// THE PITCH, ONLY WHEN A HAND SET IT (`pitched`): semitones from the cycle's `rootKey`, never an absolute key — the
	/// window decodes over a held C4 and the device over whatever is held.
	bool pitched = false;
	int16_t key = 0;
	float velocity = 0.8f;
	float gate = 0.5f;
	bool accent = false;
	bool slide = false;
	uint8_t drumHits = 0;
	uint8_t drumAccent = 0;
	/// **A CELL'S OWN LEVEL** (Drum's velocity, Modulation's value:), lane by lane, where `cellSet` has its bit.
	std::array<float, kDrumLanes> cellLevel {};
	uint8_t cellSet = 0;
	/// THE SIGNATURE, in what the held keys do not move: the step's kind, its scale degree and its octave (in Drum, its hits).
	PatternStep::Kind origKind = PatternStep::Kind::Rest;
	int16_t origDegree = 0;
	int16_t origOctave = 0;
	uint8_t origHits = 0;
	friend bool operator==(const StepEdit&, const StepEdit&) = default;
};
/// The signature a step is matched by.
[[nodiscard]] bool editMatches(const StepEdit& e, const PatternStep& terrain, bool drum) noexcept;
/// Sets the edit's key to `key` as `cycle` (the decode it was made over) spells it.
void setEditKey(StepEdit& e, const PatternCycle& cycle, int key) noexcept;
/// The edit that leaves `terrain` as it is (on, signed with it) — what a hand's first touch of a step starts from.
[[nodiscard]] StepEdit editOf(const PatternStep& terrain, bool drum) noexcept;
/// **THE OVERLAY ON A CYCLE**: every step whose edit is on and whose terrain step still matches becomes the edit.
/// `levelsAreHits`: Modulation's, where a cell a hand set a level on is a hit (design §7.10).
void applyEdits(const std::array<StepEdit, genes::kMaxSteps>& edits, bool drum, PatternCycle& cycle, bool levelsAreHits = false) noexcept;

//-- PROGRESSION (design §11-quater) --------------------------------------------------------------------------------//

/// **THE FAMILIES, ACROSS THE TERRAIN** in the order a reading of `Row::WideMotif` walks them, neighbours alike — so a
/// puck move morphs one into the next a chord at a time (`progressionOf`). Each is scale degrees from the tonic, in the
/// pattern's own Scale: a minor scale makes the pop loop i–v–VI–iv, as it should.
enum class Family : uint8_t {
	PopLoop,     ///< I V vi IV
	Deceptive,   ///< I IV V vi
	Plagal,      ///< I vi IV I
	VampFour,    ///< i iv (a modal vamp)
	VampSeven,   ///< i ♭VII
	Descending,  ///< i ♭VII ♭VI V
	Blues,       ///< the 12-bar
	Turnaround,  ///< I vi ii V
	TwoFiveOne,  ///< ii V I I
	Circle,      ///< I IV vii iii vi ii V I (the circle of fifths)
	kCount,
};
inline constexpr size_t kFamilies = static_cast<size_t>(Family::kCount);
inline constexpr const char* kFamilyNames[kFamilies] = { "Pop loop", "Deceptive", "Plagal", "Vamp i-iv", "Vamp i-bVII",
	                                                     "Descending", "12-bar blues", "Turnaround", "ii-V-I", "Circle" };
/// The most chords a progression holds (the 12-bar blues, developed: no more than a cycle can carry anyway).
inline constexpr size_t kMaxChords = 24;
/// One family's degrees, `count` of them.
struct FamilyDegrees {
	std::array<int8_t, 12> degree {};
	size_t count = 0;
};
[[nodiscard]] FamilyDegrees familyDegrees(Family f) noexcept;
/// **WHERE THE TERRAIN SITS BETWEEN TWO FAMILIES**: the family `from`, the next one `to`, and how far across (0..1).
struct FamilyReading {
	Family from = Family::PopLoop;
	Family to = Family::PopLoop;
	float across = 0.0f;
};
[[nodiscard]] FamilyReading familyOf(const genes::Wide& w) noexcept;
/// **ONE CHORD OF A PROGRESSION**, before it is voiced: its degree in the scale, or — a borrowed chord or a secondary
/// dominant (Leap) — a chord stacked in another scale (`scale`, a `theory::kScales` index) on another tonic (`tonic`,
/// semitones above the pattern's), its size, and its root in semitones above the pattern's tonic.
struct ProgressionChord {
	int degree = 0;
	int scale = -1; ///< -1: the pattern's own scale and tonic
	int tonic = 0;
	theory::ChordSize size = theory::ChordSize::Triad;
	bool cadence = false; ///< placed by the Answer pin
};
/// **THE PROGRESSION AT A PLACE** (pins applied): its chords, how many, and how many steps each lasts. Pure.
struct Progression {
	std::array<ProgressionChord, kMaxChords> chord {};
	size_t count = 0;
	size_t block = 16; ///< steps a chord lasts (Density, counted in bars of `Recipe::stepBeats`)
	size_t stepsPerBeat = 4;
};
[[nodiscard]] Progression progressionOf(const genes::Place& place, const Recipe& r, const HeldChord& held) noexcept;
/// **THE ROMAN NUMERAL** of a progression step's chord — "ii7", "V", "bVII", "viio", "Isus4", "Vmaj7" — or "" for none.
void numeralOf(const PatternStep& step, char* out, size_t size) noexcept;

} // namespace feathers::mod::arp::pattern
