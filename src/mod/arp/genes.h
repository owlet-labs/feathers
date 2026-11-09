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
#include <iterator>

// THE PATTERN DEVICE'S GENE SPACE (terrain-arp design §3–§6) — the row bank a place in the terrain is read through.
//
// ONE BANK FOR EVERY MODE. Each row is a phi triangle with a phase row and a rate row, as every terrain in the house is
// (`SliceBasis`: N phase rows and N rate rows). The Mode decides what a row MEANS (design §5); the bank does not change
// with it, so switching a pattern's Mode re-reads the same place.
//
// TWO KINDS OF ROW. A PER-STEP row reads step k at `tri(phase + k·(stride + rate / L))`: each step walks its own triangle
// at an irrational stride, so the steps differ without being noise. A PATTERN-WIDE row reads `tri(phase)` once.
//
// **THE RATE ROW IS DIVIDED BY L, AND THAT IS A MEASURED RULE (design §6).** A rate move of d moves step k by k·d, so
// unscaled a long pattern's tail re-rolls at a move its head barely notices (MODELLED: 70% of a 64-step pattern's
// pitches for d = 0.001, against 2.9% for a phase move). Divided by L, the last step moves at most as far as a phase
// move moves every step. test_arp_genes holds the bound.
//
// DETERMINISTIC BY CONSTRUCTION: every value here is a pure function of the place, the pins and the step. Nothing is
// drawn and nothing is stored.

namespace feathers::mod::arp::genes {

/// THE ROWS. Per-step rows first, then the pattern-wide ones; ORDER IS THE BANK'S CONTRACT (a stored residue is indexed
/// by it), so append only.
enum class Row : uint8_t {
	// per step
	ContourSlow, ///< the melodic arc (Melody), the degree walk (Chords), the riff offset (TB)
	ContourFast, ///< leaps, mixed in by Wide::leap
	Octave,      ///< whether and which way a step jumps an octave
	Rest,
	Hold,        ///< ties, slurs, held chords
	Length,      ///< the gate's variation
	Accent,
	Slide,
	Velocity,
	Repeat,      ///< whether a step copies the motif's step before
	Answer,      ///< how a step of the second half answers the first
	Detail,      ///< grace notes, ghosts, extensions — and which steps ratchet (the Ratchet pin's row too: see `pinRow`)
	Voicing,     ///< the Chords voicing bias (take the second-cheapest candidate)
	// pattern-wide, under a pin
	WideMotion,
	WideLeap,
	WideRange,
	WideDensity,
	WideLength,
	WideHold,
	WideAccent,
	WideGlide,
	WideOctave,
	WideRepeat,
	WideAnswer,
	WideDetail,
	// pattern-wide, no pin
	WideDirection,
	WideRegister,
	WideMotif,
	WideSize,
	WidePitchLength,
	WidePhrase,
	kCount,
};
inline constexpr size_t kRowCount = static_cast<size_t>(Row::kCount);
inline constexpr size_t kStepRowCount = static_cast<size_t>(Row::WideMotion);
[[nodiscard]] constexpr size_t at(Row r) { return static_cast<size_t>(r); }

/// ONE ROW'S CONSTANTS: its φ multiplier (how far a home scalar moves it, design §3), its triangle's duty, a fixed phase
/// that spreads the rows round the cycle, and — per-step rows — the stride step k walks at.
struct RowDef {
	double phi;
	float duty;
	double offset;
	double stride;
};
/// **THE BANK.** φ multipliers are powers of φ in the house's quarter steps; strides are frac(nφ) (the golden sequence:
/// no two clustered), except the slow contour's, which is small so neighbouring steps sit near each other (an ARC) and
/// the fast contour's, which is large so neighbours jump. Duty 1 everywhere a row is read against a threshold: a full
/// triangle sampled at an irrational stride is uniform on [0, 1], so a density d marks a share d of the steps.
inline constexpr RowDef kRows[kRowCount] = {
	{ 1.1271566, 1.0f, 0.000, 0.0901699 }, // ContourSlow: frac(5φ), an arc over ~11 steps
	{ 1.4352958, 1.0f, 0.167, 0.3819660 }, // ContourFast: 2 − φ, a jump each step
	{ 1.8257419, 1.0f, 0.333, 0.7082039 }, // Octave: frac(6φ), and so on down
	{ 2.0581710, 1.0f, 0.500, 0.2360680 }, // Rest
	{ 2.3197171, 1.0f, 0.667, 0.8541020 }, // Hold
	{ 2.6180340, 1.0f, 0.833, 0.4721360 }, // Length
	{ 2.9603110, 1.0f, 0.250, 0.3262379 }, // Accent
	{ 3.3302077, 1.0f, 0.583, 0.9442719 }, // Slide
	{ 3.7515562, 1.0f, 0.917, 0.5623059 }, // Velocity
	{ 4.2360680, 1.0f, 0.125, 0.1803399 }, // Repeat
	{ 4.7742114, 1.0f, 0.375, 0.7983739 }, // Answer
	{ 5.3884156, 1.0f, 0.625, 0.4164079 }, // Detail
	{ 6.0409418, 1.0f, 0.875, 0.0344419 }, // Voicing
	{ 0.6180340, 1.0f, 0.050, 0.0 },       // WideMotion
	{ 0.7861513, 1.0f, 0.150, 0.0 },       // WideLeap
	{ 0.8872984, 1.0f, 0.250, 0.0 },       // WideRange
	{ 1.1746627, 1.0f, 0.350, 0.0 },       // WideDensity
	{ 1.2720196, 1.0f, 0.450, 0.0 },       // WideLength
	{ 1.3871872, 1.0f, 0.550, 0.0 },       // WideHold
	{ 1.6180340, 1.0f, 0.650, 0.0 },       // WideAccent
	{ 2.0581710, 1.0f, 0.750, 0.0 },       // WideGlide
	{ 2.6180340, 1.0f, 0.850, 0.0 },       // WideOctave
	{ 3.3302077, 1.0f, 0.950, 0.0 },       // WideRepeat
	{ 4.2360680, 1.0f, 0.100, 0.0 },       // WideAnswer
	{ 5.3884156, 1.0f, 0.200, 0.0 },       // WideDetail
	{ 0.6180340, 1.0f, 0.300, 0.0 },       // WideDirection
	{ 0.8872984, 1.0f, 0.400, 0.0 },       // WideRegister
	{ 1.2720196, 1.0f, 0.500, 0.0 },       // WideMotif
	{ 1.6180340, 1.0f, 0.600, 0.0 },       // WideSize
	{ 2.0581710, 1.0f, 0.700, 0.0 },       // WidePitchLength
	{ 2.6180340, 1.0f, 0.800, 0.0 },       // WidePhrase
};

/// **A PLACE IN THE BANK**: each row's phase and rate, as the terrain decodes them (home + travel; build step 4 seats
/// Feathers' own `SliceBasis` on this). Unwrapped: the triangle wraps at the point of use.
struct Place {
	std::array<double, kRowCount> phase {};
	std::array<double, kRowCount> rate {};
};

/// THE FIFTEEN PIN SLOTS (design §4), in the window's order. Each keeps one ROLE across the Modes.
///
/// **RATCHET IS ITS OWN PIN, AND OFF BY DEFAULT** (they were Detail's ratchets, two steps in sixteen at the default, which doubled up notes in melody mode; opt-in by design, a field scaler).
/// It was a share of Detail; Detail keeps its other roles (grace notes, ghosts, extensions).
///
/// **VOICE LEADING** (a provisional design, to be confirmed): how far a chord is led from the one before (`theory::ChordSpec::leading`) — 0 block chords, each
/// voiced as if alone; 1 the strict least motion every chord had before it existed. ON AT 1 BY DEFAULT, so nothing moved.
///
/// **APPROACH**: how often a chord change is approached by PICKUP notes that resolve into a
/// tone of the new chord (`pattern.cpp`'s `addApproaches`). OFF BY DEFAULT, so nothing moved.
enum class Pin : uint8_t {
	Motion, Leap, Range, Density, Length, Hold, Accent, Glide, Octave, Repeat, Answer, Detail, Ratchet, VoiceLeading,
	Approach, kCount
};
inline constexpr size_t kPinCount = static_cast<size_t>(Pin::kCount);
inline constexpr const char* kPinNames[kPinCount] = { "Motion", "Leap",   "Range",  "Density", "Length",  "Hold",
	                                                  "Accent", "Glide",  "Octave", "Repeat",  "Answer",  "Detail",
	                                                  "Ratchet", "Voice Leading", "Approach" };
/// **THE PATTERN-WIDE ROW EACH PIN SCALES**: `WideMotion` onward, in the pins' order — except the two that share a row
/// with a pin no Mode reads beside them (`pinRead`), which couples nothing that sounds together and leaves the bank, the
/// hand's residue, the likes' points and the warp's sites as they were. RATCHET reads `WideDetail`, the row the ratchets
/// always read, so a ratchet switched on falls on exactly the steps, with exactly the hits, it did inside Detail. VOICE
/// LEADING reads `WideGlide`: Glide is the TB modes' alone, Voice Leading Chords' and Progression's. APPROACH reads
/// `WidePitchLength`, the TB polymeter's pitch length, which no chord Mode reads.
[[nodiscard]] constexpr Row pinRow(Pin p) {
	if (p == Pin::Ratchet) {
		return Row::WideDetail;
	}
	if (p == Pin::VoiceLeading) {
		return Row::WideGlide;
	}
	if (p == Pin::Approach) {
		return Row::WidePitchLength;
	}
	return static_cast<Row>(at(Row::WideMotion) + static_cast<size_t>(p));
}
/// **EACH PIN'S SWITCH, AS A FRESH PATTERN HAS IT**: on, but Ratchet and Approach (opt in) — Off is their neutral.
inline constexpr std::array<bool, kPinCount> kPinOnByDefault { true, true, true, true, true, true, true,
	                                                           true, true, true, true, true, false, true, false };
/// **EACH PIN'S AMOUNT, AS A FRESH PATTERN HAS IT**: 0.5 (the field's own), but Voice Leading at 1 — the strict leading
/// every chord had before the pin existed, whatever the field says.
inline constexpr std::array<float, kPinCount> kPinAmountByDefault { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f,
	                                                                0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 1.0f, 0.5f };

/// **THE PINS**: an amount and a switch each (a `FaderPill`). The house law (`fieldWithOverride`): amount 0 is the
/// neutral, 0.5 the field's own value bit for bit, 1 the most. Off is the neutral whatever the amount says — and every
/// pin's neutral here is 0 (no rests, no ties, no leaps, the narrowest range, no ratchets, no leading...), so Off and 0
/// are the same sound and the switch only remembers an amount (design §7.2).
struct Pins {
	std::array<float, kPinCount> amount = kPinAmountByDefault;
	std::array<bool, kPinCount> on = kPinOnByDefault;
};

/// THE MODES (DECIDED 2026-10-30, design §5; DRUM appended 2026-10-31, gate only, 8 lanes — design §11-ter;
/// PROGRESSION appended 2026-11-04, the other modes not driven yet — design §11-quater; MODULATION appended 2026-11-06, design §7.10), in the dropdown's order.
enum class Mode : uint8_t { Melody, MelodyMono, Chords, TbMono, TbGlide, Drum, Progression, Modulation, kCount };
inline constexpr const char* kModeNames[] = { "Melody", "Melody Mono", "Chords", "TB-Mono", "TB-Glide", "Drum", "Progression",
	                                            "Modulation" };
static_assert(std::size(kModeNames) == static_cast<size_t>(Mode::kCount));
[[nodiscard]] constexpr bool isTb(Mode m) { return m == Mode::TbMono || m == Mode::TbGlide; }
[[nodiscard]] constexpr bool isDrum(Mode m) { return m == Mode::Drum; }
[[nodiscard]] constexpr bool isProgression(Mode m) { return m == Mode::Progression; }
/// **MODULATION** (design §7.10): plays no notes; the Drum decode's eight lanes, read as levels, are its outputs.
[[nodiscard]] constexpr bool isModulation(Mode m) { return m == Mode::Modulation; }
/// THE MODES THE DRUM DECODE BUILDS: Drum, and Modulation, which reads the same eight lanes as levels.
[[nodiscard]] constexpr bool decodesDrum(Mode m) { return isDrum(m) || isModulation(m); }

/// **WHICH PINS A MODE READS** (design §5's table) — the honesty rule's source: a pin a Mode does not read is not drawn.
/// Glide is the TB modes' alone (a slide); TB gates are half a step, so TB reads no Length; and nothing answers in TB.
[[nodiscard]] constexpr bool pinRead(Mode m, Pin p) {
	// RATCHET AND DETAIL SPLIT THE OLD DETAIL BY MODE, and no Mode reads both (`pinRow` leans on that). Ratchet: the modes
	// that strike ratchets — Melody, Chords, TB-Mono (TB-Glide bends ONE held note and strikes no hit after its first;
	// Melody Mono's detail is its grace note; Drum's rolls ride Octave; Progression's detail is its extensions). Detail:
	// what is left to it — Melody Mono's grace notes, Drum's ghosts, Progression's extensions.
	if (p == Pin::Ratchet) {
		return m == Mode::Melody || m == Mode::Chords || m == Mode::TbMono;
	}
	if (p == Pin::Detail) {
		return m == Mode::MelodyMono || decodesDrum(m) || isProgression(m);
	}
	// VOICE LEADING: the modes whose chords are voiced as a cycle (`theory::voiceCycle`) — Chords and Progression. It shares
	// Glide's row (`pinRow`), and Glide is the TB modes' alone.
	if (p == Pin::VoiceLeading) {
		return m == Mode::Chords || isProgression(m);
	}
	// APPROACH: pickups into a chord change — Chords and Progression first.
	if (p == Pin::Approach) {
		return m == Mode::Chords || isProgression(m);
	}
	if (isDrum(m)) {
		// DRUM READS NINE (design §11-ter): no note length, no pitch range, no slide.
		return p != Pin::Glide && p != Pin::Length && p != Pin::Range;
	}
	if (isModulation(m)) {
		// MODULATION READS EIGHT, Drum's nine but Octave: Octave rolls, and a roll would be several values in one step.
		return p != Pin::Glide && p != Pin::Length && p != Pin::Range && p != Pin::Octave;
	}
	if (isProgression(m)) {
		// PROGRESSION READS EIGHT, the progression's way (design §11-quater): Density the changes a bar, Motion how far the
		// root moves, Leap the borrowed chords and secondary dominants, Detail the extensions, Answer the phrase-end
		// cadence, Repeat loop or develop, Hold sustained or re-struck, Accent the pushes.
		return p != Pin::Glide && p != Pin::Length && p != Pin::Range && p != Pin::Octave;
	}
	switch (p) {
	case Pin::Glide: return isTb(m);
	case Pin::Length:
	case Pin::Answer: return !isTb(m);
	default: return true;
	}
}

/// THE PATTERN-WIDE VALUES, 0..1, pins applied to the thirteen (Ratchet and Detail read one row: `pinRow`).
struct Wide {
	std::array<float, kPinCount> pinned {};
	float direction = 0.0f;
	float registerCentre = 0.0f;
	float motif = 0.0f;
	float size = 0.0f;
	float pitchLength = 0.0f;
	float phrase = 0.0f;

	[[nodiscard]] float of(Pin p) const noexcept { return pinned[static_cast<size_t>(p)]; }
};

/// THE LONGEST CYCLE.
inline constexpr size_t kMaxSteps = 64;

/// ONE STEP'S RAW READINGS, 0..1, one per per-step row (indexed by `Row`).
using StepRaw = std::array<float, kStepRowCount>;

/// **THE LIKES' WARP** (pattern_warp.h): a circle map per read SITE — each pattern-wide row, and each per-step row at
/// each step — bent toward the angles the likes were heard at. Null everywhere is the plain decode, to the bit.
struct Warp;
/// **A SITE'S NUMBER**: the pattern-wide rows first (`Row::WideMotion` onward), then per-step row i at step k.
inline constexpr size_t kWideRowCount = kRowCount - kStepRowCount;
inline constexpr size_t kStepSitesFrom = kWideRowCount;
[[nodiscard]] constexpr size_t wideSite(Row r) { return at(r) - kStepRowCount; }
[[nodiscard]] constexpr size_t stepSite(size_t row, size_t k) { return kStepSitesFrom + k * kStepRowCount + row; }

/// **THE TRIANGLE A ROW IS READ THROUGH** — unipolar, with the row's duty, at an unwrapped phase.
[[nodiscard]] float readRow(Row r, double phase) noexcept;
/// THE ANGLE IT IS READ AT: the phase plus the row's offset, wrapped to [0, 1).
[[nodiscard]] double angleOf(Row r, double phase) noexcept;
/// The same read with `site` warped (null: `readRow`).
[[nodiscard]] float readRow(Row r, double phase, const Warp* warp, size_t site) noexcept;
/// **PATTERN-WIDE READINGS**, pins applied.
[[nodiscard]] Wide wideOf(const Place& place, const Pins& pins, const Warp* warp = nullptr) noexcept;
/// **STEP k OF AN L-STEP CYCLE**: every per-step row at `phase + k·(stride + rate / L)`.
[[nodiscard]] StepRaw stepOf(const Place& place, size_t k, size_t length, const Warp* warp = nullptr) noexcept;
/// THE PHASE step k of an L-step cycle reads row i at (unwrapped).
[[nodiscard]] double stepPhase(const Place& place, size_t row, size_t k, size_t length) noexcept;

} // namespace feathers::mod::arp::genes
