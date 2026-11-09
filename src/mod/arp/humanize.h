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

#include <cstddef>
#include <cstdint>
#include <iterator>

// HUMANIZE (terrain-arp design §12): a mode and an amount, like SKLITTER's groove — the feel is a Choice, so an amount can
// never roll into another feel. Every value here is a pure function of (the feel, the Seed, the grid slot, the step's
// length): never a running generator, so a scrub, a loop or a bounce lands every note where it did (CLAUDE.md:
// determinism first). Jitter is keyed on the song's slot, so it does not loop with the pattern; Swing and Groove follow
// the bar, through SKLITTER's own warp (`fx/sklitter/groove.h`).

namespace feathers::mod::arp::humanize {

/// THE FEELS. **The first ten are SKLITTER's groove modes, in its order** (`fx::sklitter::GrooveMode`: Straight, Swing
/// 4/8/16, Groove 4/8/16, Free 4/8/16) and run through its own warp (`groove.h`), shared and not forked; the classic Arp's Feel list is exactly those ten. The rest are the Pattern device's own.
enum class Mode : uint8_t {
	Straight,
	Swing4,
	Swing8,
	Swing16,
	Groove4,
	Groove8,
	Groove16,
	Free4,
	Free8,
	Free16,
	Tight,
	Loose,
	Drunk,
	Push,
	Lazy,
	StrumUp,
	StrumDown,
	Flam,
	kCount
};
inline constexpr const char* kModeNames[] = { "Straight", "Swing 4",  "Swing 8",    "Swing 16", "Groove 4", "Groove 8",
	                                          "Groove 16", "Free 4",  "Free 8",     "Free 16",  "Tight",    "Loose",
	                                          "Drunk",     "Push",    "Lazy",       "Strum Up", "Strum Down", "Flam" };
static_assert(std::size(kModeNames) == static_cast<size_t>(Mode::kCount));
/// "Free" WAS THE ONLY FREE FEEL (Free 16) until SKLITTER's ten replaced the list (2026-10-30): a stored "Free" reads as it.
inline constexpr device::ChoiceAlias kFeelAliases[] = { { "Free", "Free 16" } };
/// THE GROOVE FEELS — the first `kGrooveFeels` modes, SKLITTER's own.
inline constexpr size_t kGrooveFeels = 10;
[[nodiscard]] constexpr bool isGroove(Mode m) { return static_cast<size_t>(m) < kGrooveFeels; }

/// THE MODES WHOSE AMOUNT DOES SOMETHING (all but Straight) and the ones that roll the Seed's dice — the gates.
inline constexpr uint32_t kAmountModes = ((1u << static_cast<unsigned>(Mode::kCount)) - 1u) & ~1u;
/// THE FREE FEELS (a position: `Feel::freePhase`) and the feels whose `amount` is a depth or a size (all but Straight and Free).
inline constexpr uint32_t kFreeModes = (1u << static_cast<unsigned>(Mode::Free4)) | (1u << static_cast<unsigned>(Mode::Free8))
                                       | (1u << static_cast<unsigned>(Mode::Free16));
inline constexpr uint32_t kDepthModes = kAmountModes & ~kFreeModes;
/// FREE PHASE (backlog QA, both arps): its declared range — not a bound (SKLITTER's `kCoordMax`; `offsetsMayLeaveRange`
/// says so).
inline constexpr double kFreePhaseReach = 8388608.0;
inline constexpr uint32_t kSeedModes = (1u << static_cast<unsigned>(Mode::Tight)) | (1u << static_cast<unsigned>(Mode::Loose))
                                       | (1u << static_cast<unsigned>(Mode::Drunk)) | (1u << static_cast<unsigned>(Mode::Push))
                                       | (1u << static_cast<unsigned>(Mode::Lazy));

/// **NO SLOT MOVES FURTHER THAN THIS, in steps** — under half a step, so two neighbours can never meet and onsets keep their order (the grid's
/// `firstAtOrAfter` stays exact: `StepGrid::shift`). SKLITTER's swing reaches half a step at 75%; here it stops at 74.5%.
inline constexpr double kMaxShiftSteps = 0.49;

/// THE FEEL'S CONSTANTS, each a draft for the ear (design §12).
inline constexpr double kTightMs = 8.0;
inline constexpr double kLooseMs = 25.0;
inline constexpr double kDrunkMs = 30.0;
inline constexpr double kPushMs = 20.0;
inline constexpr double kPushJitterMs = 4.0;
inline constexpr double kFlamMs = 30.0;
inline constexpr double kTightVelocity = 0.06;
inline constexpr double kLooseVelocity = 0.20;
inline constexpr double kDrunkVelocity = 0.15;
/// A strum spreads a chord over at most this share of a step.
inline constexpr double kStrumMaxShare = 0.25;
/// Each later tone of a strum is this much softer.
inline constexpr double kStrumVelocityStep = 0.05;
/// A flam's grace note is this loud against the note.
inline constexpr double kFlamVelocity = 0.6;

struct Feel {
	Mode mode = Mode::Straight;
	double amount = 0.0;
	int seed = 0;
	/// **FREE'S POSITION ALONG ITS WALK** (backlog QA; SKLITTER's `free_phase`, backlog IO): unbounded, read by the Free feels in
	/// place of `amount`, which stays the other feels' depth.
	double freePhase = 0.0;
	/// **WOBBLE** (zone 2, backlog QA; terrain-arp design §12-ter): a second unbounded position, read by EVERY feel, that
	/// walks orthogonal phi fields — timing, velocity, length. 0 changes nothing.
	double wobble = 0.0;
	friend bool operator==(const Feel&, const Feel&) = default;
};

/// **WOBBLE — THE SECOND GROOVE ZONE** (design §12-ter). Zone 1 (`fx/sklitter/groove.h`) moves timing alone, along waves at
/// `φ^(1/j)`. Wobble walks the same kind of wave — a duty-cycled triangle per step of the bar, the duty by metrical level,
/// 0 at phase 0 — once per FIELD, each field at its own rates `φ^(1/(j + c_f))`, so no field's rates ever line up with
/// zone 1's or another field's (`1/(j + c_f) = 1/k` has no integer solution, and φ to a nonzero rational power is
/// irrational). A pure function of (phase, the step's place in the bar): no state, no seed.
///
/// **EACH FIELD IS ONE ENTRY**, so a later field (a skip, a burst, an octave jump, a strum spread, a slide chance: if asked for)
/// is one more `Field`, one more `kFieldOffset`, and one reader — `wave(Field, j, phase)` and `sign(Field, j)` serve
/// them all.
namespace wobble {
/// THE FIELDS. The first four shape every note; the four EXTRAS are EVENTS: a step
/// takes one where its triangle stands over the half (`fires`), so the share of the walk an extra occupies is half its duty.
enum class Field : uint8_t { Timing, Velocity, Length, Run, Burst, Octave, Strum, Slide, kCount };
/// THE FIELDS' RATE OFFSETS `c_f`: each non-integer, and no two differing by an integer.
inline constexpr double kFieldOffset[] = { 0.5, 1.0 / 3.0, 0.25, 0.2, 1.0 / 6.0, 1.0 / 7.0, 1.0 / 9.0, 0.4 };
/// THE EXTRAS' DUTY — the share of the walk each step's triangle is up (fixed, not metrical: an extra is an event, not a lean).
/// A step fires on half of it: a burst 8% of the walk, a strum 10%, an octave jump 15%, a slide 25% (drafts for the ear).
inline constexpr double kExtraDuty[] = { 0.0, 0.0, 0.0, 0.0, 0.16, 0.3, 0.2, 0.5 };
static_assert(std::size(kExtraDuty) == static_cast<size_t>(Field::kCount));
/// A BURST'S HITS (a stutter: 3 or 4 in the step, by its sign) and a strum's spread at most, as a share of the step.
inline constexpr int kBurstFew = 3;
inline constexpr int kBurstMany = 4;
inline constexpr double kStrumSpread = 0.3;
static_assert(std::size(kFieldOffset) == static_cast<size_t>(Field::kCount));
/// THE DEPTHS (drafts for the ear: "generous but musical"). Timing: a step's own push or drag, plus a phrase-long rush or
/// drag; the sum is held inside ±`kMaxShift` (and every slot inside humanize's ±0.49, so no onset crosses its neighbour).
inline constexpr double kPushDrag = 0.32;
inline constexpr double kRun = 0.18;
inline constexpr double kMaxShift = 0.45;
/// Velocity: a step dips by up to this share or rings up to `kRing` over its level (the downbeat never dips).
inline constexpr double kDip = 0.55;
inline constexpr double kRing = 0.25;
/// Length: a step is clipped by up to this share of its gate (never lengthened; slides and ties are never clipped).
inline constexpr double kClip = 0.7;
/// The run's reach grows from 0 over the first quarter unit of phase, so a small phase is a small wobble.
inline constexpr double kRunRamp = 4.0;

/// The duty-cycled triangle for field `f` at step `j` (0..1), 0 at phase 0 and mirrored below it.
[[nodiscard]] double wave(Field f, int32_t j, double phase) noexcept;
/// The field's direction at step `j`, ±1: which steps push and which drag, which dip and which ring.
[[nodiscard]] double sign(Field f, int32_t j) noexcept;
/// **THE RUN**: a slow curve over the bar, a rush (early) into a drag (late), at a phrase phase that walks with the phase.
[[nodiscard]] double run(int32_t j, int32_t grid, double phase) noexcept;
/// THE THREE READINGS at step `j` of a `grid`-step bar: a shift in steps, a velocity factor, a length factor.
[[nodiscard]] double shiftSteps(double phase, int32_t j, int32_t grid) noexcept;
[[nodiscard]] double velocity(double phase, int32_t j) noexcept;
[[nodiscard]] double length(double phase, int32_t j) noexcept;
/// SLOT g'S PLACE IN ITS BAR, and the bar's steps, for a step `stepBeats` long (0: Hz, a bar read as sixteen steps).
[[nodiscard]] int32_t gridOf(double stepBeats) noexcept;
[[nodiscard]] int32_t stepInBar(int64_t g, int32_t grid) noexcept;
/// **DOES EXTRA `f` FIRE AT STEP `j`** — its fixed-duty triangle over the half. Never at phase 0.
[[nodiscard]] bool fires(Field f, int32_t j, double phase) noexcept;
/// The extra's triangle at step `j` (0..1), its duty `kExtraDuty[f]`.
[[nodiscard]] double extraWave(Field f, int32_t j, double phase) noexcept;
} // namespace wobble

/// **WHERE SLOT g LANDS, in steps from its straight onset** (late positive), within ±`kMaxShiftSteps`. `stepSeconds` is
/// one step's length; `stepBeats` its length in beats, or 0 when the rate is in Hz (Swing and Groove then read a bar as
/// sixteen steps).
[[nodiscard]] double shiftSteps(const Feel& feel, int64_t g, double stepSeconds, double stepBeats) noexcept;
/// The velocity multiplier for slot g (a step `stepBeats` long; 0 under Hz): the feel's jitter, times Wobble's.
[[nodiscard]] double velocityScale(const Feel& feel, int64_t g, double stepBeats) noexcept;
/// **THE LENGTH MULTIPLIER FOR SLOT g** — Wobble's (≤ 1; 1 at Wobble 0). A caller does not apply it to a slide or a tie.
[[nodiscard]] double lengthScale(const Feel& feel, int64_t g, double stepBeats) noexcept;
/// **A WOBBLE EXTRA AT SLOT g** (design §12-ter): whether it fires (never at Wobble 0), and its direction there (±1).
[[nodiscard]] bool wobbleFires(const Feel& feel, wobble::Field f, int64_t g, double stepBeats) noexcept;
[[nodiscard]] double wobbleSign(wobble::Field f, int64_t g, double stepBeats) noexcept;
/// **A STRUM**: tone `j` of `n` (0 the first struck) lands this share of a step late; 0 outside the strums.
[[nodiscard]] double strumShare(const Feel& feel, int j, int n) noexcept;
/// **A FLAM'S LEAD, in steps**: the slot's onset moves this much EARLY for the grace note, and the note itself stays on
/// the grid. 0 outside Flam.
[[nodiscard]] double flamSteps(const Feel& feel, double stepSeconds) noexcept;

} // namespace feathers::mod::arp::humanize
