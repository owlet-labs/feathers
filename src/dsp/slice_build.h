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

#include "character/phi_morph.hpp"
#include "character/phi_stair.hpp"
#include "character/phi_vox.hpp"
#include "character/phi_weave.hpp"
#include "dsp/drum_voice.h"
#include "dsp/partial_spectrum.h"
#include "dsp/phase_warp.h"
#include "dsp/field_override.h"
#include "dsp/slice_basis.h"

#include <array>

// Building a character at a HOME plus a TRAVEL, rather than at a phase.
//
// The firmware's builders take one number — `zone/1023 + phaseOffset` — and hand it to every triangle
// in the bank. These take that same number as HOME, and then let the basis decide how far the travel
// carries each triangle from it: its own phase, and its own rate. That is the whole of the second
// axis; everything downstream of the bank evaluation is the character layer exactly as it is on
// hardware.
//
// At zero travel these ARE the firmware's builders, for every orientation — not only for a special
// one. Which is the rule: a rotation that changed the sound would be a second spelling of gamma.
//
// WHY THE ASSEMBLY IS COPIED HERE. The firmware submodule is not ours to modify (CLAUDE.md), and the
// entry point we need is a dozen lines INSIDE buildPhiMorphWavetable — after that it is waypoint
// accumulation, normalisation and shaping that we want unchanged. So the primitives are reused as-is
// (phi::evalTriangle, buildSegmentsFromWaypoints, and everything in phi_triangle.hpp is an inline
// header) and only the assembly around them is restated. The duplication is real and it can drift;
// what catches drift is the identity test — at the default basis these must be BIT-IDENTICAL to the
// firmware's own builders, so a firmware change that moves any float fails the suite immediately.
//
// Zero travel is not a special case in the code. Both of `phaseFor`'s products are exactly ±0.0, and
// adding either to a finite value is exact, so `home` comes back untouched and `rateFor` returns
// exactly 1.0f — which is the argument the firmware passes. Identical inputs to identical inline
// functions, so identical bits, without a branch that could rot.

namespace feathers {



using MorphBasis = SliceBasis<kMorphTriangles>;
using StairBasis = SliceBasis<kStairTriangles>;
using VoxBasis = SliceBasis<kVoxTriangles>;
using WeaveBasis = SliceBasis<kWeaveTriangles>;
using PartialBasis = SliceBasis<kPartialTriangles>;

/// Where the morph engine's triangles sit in the basis. Public because the audition tools and the
/// tests both need to name a triangle, and a magic 23 in two files is a bug waiting for its moment.
namespace morph_slot {
inline constexpr size_t kPhaseDelta = 0;   ///< 8, one per segment
inline constexpr size_t kAmplitude = 8;    ///< 7, one per movable waypoint
inline constexpr size_t kCurvature = 15;   ///< 8, one per segment
inline constexpr size_t kGain = 23;
inline constexpr size_t kEndpoint = 24;
inline constexpr size_t kAmpOvershoot = 25;
inline constexpr size_t kCurvBoost = 26;
inline constexpr size_t kPhaseDistort = 27;
inline constexpr size_t kSineBlend = 28;
inline constexpr size_t kOddSym = 29;
inline constexpr size_t kWindow = 30;
inline constexpr size_t kSlopeMatch = 31;
inline constexpr size_t kPhaseJitter = 32;
inline constexpr size_t kAmpNoise = 33;
inline constexpr size_t kAsymGain = 34;
} // namespace morph_slot

/// Where the stair engine's triangles sit. The two landscapes and the two spatial patterns are
/// single triangles read at many step positions, so one coefficient governs a whole landscape's
/// travel and rate — which is what makes the plane change how BUSY a staircase is, not just which
/// one it is.
namespace stair_slot {
inline constexpr size_t kCount = 0;
inline constexpr size_t kSlope = 1;
inline constexpr size_t kTilt = 2;
inline constexpr size_t kAsym = 3;
inline constexpr size_t kLandCycles = 4;
inline constexpr size_t kHeightLand = 5;
inline constexpr size_t kAsymLand = 6;
inline constexpr size_t kGlyph = 7;  ///< family 5's quantised levels
inline constexpr size_t kShard = 8;  ///< family 6's asymmetric jumps
} // namespace stair_slot

/// Where the vox engine's triangles sit. THIS ORDER IS THE RESIDUE ROW ORDER AND THE WIRE ORDER,
/// forever, the same way morph's and stair's are — it is the firmware header's own declaration
/// order, so the two lists read as one. Articulation holds slot 11 from birth (V22c): the slot, its
/// residue rows and its wire entries all exist NOW, so the counts never grow twice; only its
/// product waits for the motion arc.
namespace vox_slot {
inline constexpr size_t kF1Wander = 0;
inline constexpr size_t kF2Wander = 1;
inline constexpr size_t kPulses1 = 2;
inline constexpr size_t kPulses2 = 3;
inline constexpr size_t kDecay1 = 4;
inline constexpr size_t kDecay2 = 5;
inline constexpr size_t kBalance = 6;
inline constexpr size_t kPolarity1 = 7;
inline constexpr size_t kPolarity2 = 8;
inline constexpr size_t kShift = 9;
inline constexpr size_t kBreath = 10;
inline constexpr size_t kArticulation = 11;
} // namespace vox_slot

/// Where the Resonate engine's triangles sit — phi_weave.hpp's declaration order, which is also the
/// residue row order and the wire order, forever, as the other engines' are. A spatial landscape is
/// one triangle read at every node, so its slot's coefficients govern the whole landscape's travel
/// and rate, as stair's landscapes do.
namespace weave_slot {
inline constexpr size_t kStiffBase = 0;
inline constexpr size_t kCoupleBase = 1;
inline constexpr size_t kDampBase = 2;
inline constexpr size_t kStiffCycles = 3;
inline constexpr size_t kDampCycles = 4;
inline constexpr size_t kStiffLand1 = 5;
inline constexpr size_t kStiffLand2 = 6;
inline constexpr size_t kDampLand1 = 7;
inline constexpr size_t kDampLand2 = 8;
inline constexpr size_t kCoupleLand = 9;
inline constexpr size_t kHomeP2 = 10;
inline constexpr size_t kHomeP3 = 11;
inline constexpr size_t kHomeP5 = 12;
inline constexpr size_t kHomePh2 = 13;
inline constexpr size_t kHomePh3 = 14;
inline constexpr size_t kBowDepth = 15;
inline constexpr size_t kBowRate = 16;
inline constexpr size_t kBowPos = 17;
inline constexpr size_t kBowSpread = 18;
inline constexpr size_t kBowMode = 19;
inline constexpr size_t kTravel = 20;
inline constexpr size_t kPluckPos = 21;
inline constexpr size_t kPluckWidth = 22;
inline constexpr size_t kPluckAmp = 23;
inline constexpr size_t kShimmer = 24;
inline constexpr size_t kMorphBow = 25;
inline constexpr size_t kOutGain = 26;
} // namespace weave_slot

/// Where the additive engine's triangles sit (design 2026-10-08 §2) — also the residue row order and
/// the wire order, as the other engines' are. Rows 0-12 are the thirteen shape fields, one triangle
/// each; rows 13-16 are the four detail GROUPS, each read by every harmonic in its group at that
/// harmonic's own φ rate (partialDetailConfig), so one travel direction per group moves them all.
namespace partial_slot {
inline constexpr size_t kRolloff = 0;
inline constexpr size_t kFormant1Centre = 1, kFormant1Width = 2, kFormant1Gain = 3;
inline constexpr size_t kFormant2Centre = 4, kFormant2Width = 5, kFormant2Gain = 6;
inline constexpr size_t kOddEven = 7;
inline constexpr size_t kCombSpacing = 8, kCombDepth = 9;
inline constexpr size_t kDetail = 10;
inline constexpr size_t kOffGrid = 11;
inline constexpr size_t kSmoothness = 12; ///< Smoothness is kept (design §4.1): duty 0.20, as V5's kFlagshipSmoothField
inline constexpr size_t kDetailGroup = 13; ///< 4 rows: harmonics 1-2, 3-8, 9-32, 33 and up
/// The thirteen shape fields: every row below the detail groups.
inline constexpr size_t kFieldCount = kDetailGroup;
inline constexpr size_t kDetailGroups = 4;
} // namespace partial_slot
static_assert(partial_slot::kDetailGroup + partial_slot::kDetailGroups == kPartialTriangles);

/// The basis row harmonic `k` (1-based) reads its detail triangle through: its group's row. Groups
/// are harmonics 1-2, 3-8, 9-32 and 33 up — each four times as wide as the one below, so every group
/// is about two octaves (design §2).
constexpr size_t partialDetailRow(int k) {
	return partial_slot::kDetailGroup + (k <= 2 ? 0 : k <= 8 ? 1 : k <= 32 ? 2 : 3);
}

/// The thirteen shape fields' triangles, in partial_slot order (rows 0-12). Defined in slice_build.cpp,
/// because they sit on V5's band and Smoothness IS V5's field (dsp/flagship.h, which includes this header).
const std::array<owlet::dsp::phi::PhiTriConfig, partial_slot::kFieldCount>& partialFieldBank();

/// Harmonic `k`'s detail triangle (1-based, 1..partials::kMaxPartials): duty 1.0, bipolar, its rate
/// RISING with k as `partialDetailBaseRate() · φ^(0.5 · log2 k)` (design §2.2: the low harmonics drift
/// slowly under travel and the high ones sparkle), its phase offset frac(k·φ²) of its own period.
owlet::dsp::phi::PhiTriConfig partialDetailConfig(int k);
/// Harmonic 1's detail rate, the base the ladder above rises from.
float partialDetailBaseRate();

/// How many formants and pulse-table entries a vox character carries — the firmware's own numbers,
/// read from its header rather than restated, so a bank that grows there stops this build here.
inline constexpr size_t kVoxNumFormants = static_cast<size_t>(owlet::dsp::kPhiVoxNumFormants);
inline constexpr size_t kVoxMaxPulses = static_cast<size_t>(owlet::dsp::kPhiVoxMaxPulses);

/// One VOSIM formant, position-pure: everything about it EXCEPT the note.
///
/// The firmware stores a 44.1kHz-pinned phase increment and a note ratio here; this stores the
/// frequency in Hz and nothing else, because the increment is the VOICE's to derive — at its own
/// sample rate, from its own note — in the per-voice bake (V22a: the terrain stays pure, the voice
/// bakes the note). Tracking is fixed at 0 in this cut (V22b: pure vocal identity IS the species),
/// so the note-ratio machinery is deliberately not ported at all.
struct VoxFormant {
	/// The formant's centre frequency, clamped to the firmware's own 60..8000 Hz window.
	float frequencyHz = 0.0f;
	/// Signed per-pulse gains — decay, polarity alternation and balance already applied, the burst
	/// peak-normalised, zero-padded to the end: the silent gap until the cycle wraps.
	std::array<float, kVoxMaxPulses> pulseGain {};

	friend bool operator==(const VoxFormant&, const VoxFormant&) = default;
};

/// The vox character at one position — the whole recipe, fixed-size, pitch-free.
struct VoxParams {
	std::array<VoxFormant, kVoxNumFormants> formant {};
	/// Noise mixed into the pulse envelope, 0..0.3 — voiced-gated aspiration (the C5 consumer).
	float breath = 0.0f;
	/// The consonant-burst gain crossfade MOTION will scale, 0.5..4.0. Evaluated from birth,
	/// consumed by nothing until the motion arc lands (V22c).
	float articulation = 0.0f;

	friend bool operator==(const VoxParams&, const VoxParams&) = default;
};

/// HOW REACTIVE THIS PART OF THE SPACE IS TO MOTION — the blue shift's two depths, as φ fields.
///
/// potentially both are availabe orthogonally like the other excite params via phi triangles?
/// readable at every point by simple interpolation? These complete that idiom rather than extending
/// it: the existing excite depths (`kPhiMorphAmpOvershootBank`, `kPhiMorphCurvBoostBank`,
/// `kPhiMorphPhaseDistortBank`) are already φ triangles read at a position, and these two join them.
///
/// ONE TRIANGLE EACH, AT INCOMMENSURATE RATES, so the two vary ORTHOGONALLY: a region can be reactive
/// to overshoot without being reactive to Doppler, and the two maps drift out of step forever rather
/// than repeating together. Read at a home coordinate, the same one every other φ quantity in this
/// engine is read at — which is what the plan's own periods say, since they are quoted in ZONES.
///
/// "Readable at every point by simple interpolation" is satisfied by the triangles' own continuity:
/// they are piecewise-linear in position by construction, so every point already has a value and there
/// is nothing to interpolate between. No grid, no cache.
///
/// THE FIELD IS POSITION-PURE AND THE EFFECT IS MOTION-GATED. Nothing here knows about velocity; the
/// caller multiplies by a velocity term that is exactly zero at rest, so a parked patch is a pure
/// function of position however reactive the field says the neighbourhood is.
struct BlueShiftFields {
	/// How hard the velocity-aimed overshoot pushes here. Unipolar: a reactivity is an amount.
	float overshoot = 0.0f;
	/// How strongly approaching and receding sounds part company here. Unipolar, same reason.
	float doppler = 0.0f;
};

BlueShiftFields blueShiftFieldsAt(double position);

/// A sound's home, computed exactly as the builders compute it.
///
/// Exists so that the orientation is derived from the SAME number the character is built at. Two
/// spellings of `zone/1023 + offset` would agree to the eye and disagree in the last place, and a
/// plane derived from a home a hair away from the real one is a bug nobody would ever see.
/// GAMMA'S UNBOUNDED HALF ARRIVES HERE AND NOWHERE EARLIER, which is the whole reason it is a
/// separate argument rather than folded into `phaseOffset` by the caller. The offset is a float; a
/// base folded into it would quantise at about eight zones per ULP by the time the base reached 1e5,
/// so the coarse control would eat the fine one exactly where it started to be useful. Added after
/// the float, in double, the road stays finer than a cent to a base of 1e10. See sound_gamma.h.
///
/// REQUIRED, NOT DEFAULTED, and it was defaulted for exactly as long as it took to wire the callers.
/// A default here reads a sound at a zero base, which is right for a patch that has never carried one
/// and wrong the moment somebody does — and wrong SILENTLY, describing a neighbour of the sound being
/// heard. That is G1, which cost a round of listening judgments, and rotate_sound.h's fix is the
/// pattern: make it required and let the compiler name every site rather than trusting a reading of
/// the tree. The migration guarantee survives unchanged, because it was always arithmetic and never a
/// default — `+ 0.0` on a finite double is exact, so a site that means zero says 0.0 and builds the
/// character it always built, to the bit.
inline double sliceHome(uint16_t zone, float phaseOffset, double gammaBase) {
	return static_cast<double>(zone) / 1023.0 + static_cast<double>(phaseOffset) + gammaBase;
}

/// THE ROTATE A SOUND PLAYS AT: the turn it has been given, or none at all.
///
/// UNPINNED IS ZERO, and that is the born-open rule spelled as arithmetic. A sound that has never
/// been turned sits at rotate 0, where M is already an open plane — a DIFFERENT open plane at every
/// home, because home is M's other argument. "Born" and "turned by nothing" are therefore the same
/// state, which is what lets the corner's readout be the turn itself rather than an offset from a
/// number nobody chose.
///
/// THE ALTERNATIVE, REJECTED WITH ITS REASONS ON RECORD: deriving a born rotate from a φ field of
/// home would fold home into the turn, and then two homes one φ⁻¹ period apart would be born on
/// exactly the same plane, a pinned rotate would carry no home dependence at all — every row of a
/// rotated sound's map sharing one basis — and the first click of the encoder would move the readout
/// from a φ triangle of home rather than from zero. That field (`phiOrientationFor`) is deleted; see
/// sliceCoefficient in slice_basis.h for what folding home into the turn would have cost.
inline double rotateFor(const SliceOrientationPin& pin) {
	return pin.pinned ? sliceRotateOf(pin.generator) : 0.0;
}

/// The morph character at `zone`/`phaseOffset`, carried by `travelX`/`travelY` across the plane.
///
/// Home is taken in the firmware's own units so that a call site does not have to convert and cannot
/// convert differently. Travel is Pos X and Pos Y, bipolar and centred on zero — and AT ZERO TRAVEL
/// this returns exactly what `owlet::dsp::buildPhiMorphWavetable(zone, phaseOffset)` returns, to the
/// bit, whatever the basis.
/// `gritScale` is the per-sound GRIT knob (params.h AGrit..HGrit, 2026-08-25), applied through
/// fieldWithOverride to the amp-noise triangle. Neutral leaves the table bit-identical to what this
/// returned before the knob existed, which is what keeps the sentence above true.
owlet::dsp::PhiMorphWavetable buildMorphSliced(uint16_t zone, float phaseOffset, double travelX,
                                               double travelY, const MorphBasis& basis,
                                               double gammaBase, float gritScale = kFieldNeutral);

/// The stair character at a home and a travel. Bit-identical to
/// `owlet::dsp::buildPhiStairParams(zone, phaseOffset)` at zero travel, whatever the basis.
///
/// The pattern FAMILY still comes from `zone >> 7` alone, deliberately: it is a discrete choice of
/// eight, and interpolating it is not a thing the plane can do. Travel changes the staircase within
/// its family, and crossing a family boundary is the same step change it has always been.
owlet::dsp::PhiStairParams buildStairSliced(uint16_t zone, float phaseOffset, double travelX,
                                             double travelY, const StairBasis& basis,
                                             double gammaBase);

/// The vox character at a home and a travel — the twelve φ banks read through the frame, assembled
/// exactly as the firmware's buildPhiVoxParams assembles them (fractional pulse counts, peak
/// normalisation, polarity alternation, breath squared-and-scaled), verbatim where the arithmetic
/// is theirs.
///
/// THE VOWEL ANCHORS FOLLOW THE ZONE KNOB ALONE, deliberately and firmware-identically (V22b): the
/// firmware interpolates its anchor tables on the raw zone, not on the φ phase, so gamma and travel
/// move the WANDERS around a standing anchor rather than the anchor itself — the same shape as
/// stair keeping its pattern family from `zone >> 7`. Zone stays the vowel selector; the plane
/// carries everything φ.
///
/// THE PARITY BOUNDARY, said rather than pretended around: the twelve bank configs are the firmware
/// header's OWN constants (no copies — a retune there flows straight through), but the anchors and
/// the Hz clamp live in the firmware TU's private namespace, and compiling that TU on the host
/// would drag in its render layer and the argon dependency. Those few numbers are restated here,
/// and the identity test compares against a reference restated from the same source — it catches
/// every drift of OURS, and a silent change to the firmware's private constants is outside its
/// net, unlike morph's and stair's true link-level parity.
VoxParams buildVoxSliced(uint16_t zone, float phaseOffset, double travelX, double travelY,
                         const VoxBasis& basis, double gammaBase);

/// The Resonate engine's laws at a home and a travel — the twenty-seven φ banks read through the
/// frame and assembled exactly as `owlet::dsp::buildPhiWeaveLaws` assembles them. AT ZERO TRAVEL
/// AND ZERO RESIDUE THIS IS `buildPhiWeaveLaws(sliceHome(zone, phaseOffset, gammaBase))` TO THE BIT,
/// whatever the basis (tested across the phase line), which is what makes it the same road Stair's
/// staircases take rather than a second mapping.
owlet::dsp::PhiWeaveParams buildWeaveSliced(uint16_t zone, float phaseOffset, double travelX, double travelY,
                                             const WeaveBasis& basis, double gammaBase);

/// The additive engine's thirteen shape fields at a home and a travel, each read through its own row:
/// `phi::evalTriangle(frame.phaseFor(row, home, tx, ty), frame.rateFor(row, tx, ty), partialFieldBank()[row])`.
/// Raw triangle values in [0, 1]; buildSound clamps and applies Smoothness.
partials::PartialFields readPartialFields(const PartialBasis& frame, double home, double travelX, double travelY);

/// The 320 detail triangles at a home and a travel: `detail[k - 1]` is harmonic k's, read through its
/// GROUP's row (partialDetailRow) at its own config (partialDetailConfig), in [-1, 1].
void readPartialDetail(const PartialBasis& frame, double home, double travelX, double travelY,
                       std::array<float, partials::kMaxPartials>& detail);

/// THE ADDITIVE ENGINE'S SIX PER-SOUND BIAS KNOBS (design §4.1; per sound), one per
/// sound A-H, each the house `fieldWithOverride` over the terrain field(s) it names: knob 0 -> field 0,
/// kFieldNeutral -> the field as the terrain authored it, 1 -> 1. All six rest at kFieldNeutral, so an untouched
/// sound is the terrain to the bit.
///
/// EACH KNOB BIASES THE FIELD WHOSE 0 IS "NOTHING" — the quantity, not where it sits — which is the set
/// Smoothness pulls toward the smooth spectrum (`partials::smoothed`), plus Rolloff, which always has a value:
///   rolloff -> PartialFields::rolloff (0 = p 0.5, the brightest; 1 = p 2.5, the steepest)
///   formant -> BOTH formant GAINS (formant1Gain, formant2Gain): how much formant, wherever the terrain puts the
///              peaks. Not the centres or widths, which have no "nothing" for a knob's 0 to mean, and not one
///              formant, because the knob is named for the effect and the terrain authors two peaks of it.
///   oddEven -> oddEven (one-sided: 0 = neutral, 1 = evens removed)
///   comb    -> combDepth (the spacing has no "nothing" either)
///   detail  -> detail
///   offGrid -> offGrid
struct PartialBias {
	float rolloff{kFieldNeutral};
	float formant{kFieldNeutral};
	float oddEven{kFieldNeutral};
	float comb{kFieldNeutral};
	float detail{kFieldNeutral};
	float offGrid{kFieldNeutral};
	friend bool operator==(const PartialBias&, const PartialBias&) = default;
};

/// One sound's additive spectrum at its own coordinate, through its own plane (design §2): the fields
/// and the detail triangles above, handed to `partials::buildSound`. At zero travel and zero residue
/// every row reads at `home` with rate 1, whatever the orientation (phaseFor's guarantee), so the
/// spectrum is the plane-free one bit for bit (tested).
///
/// `smoothnessScale` is the sound's Smoothness knob (SoundCoord::smoothnessScale, ASmoothness..HSmoothness),
/// applied to the Smoothness FIELD through partialFieldsWithOverrides before buildSound — V5's knob on V5's
/// field, the same curve. Its default, kFieldNeutral, is the field as authored, to the bit.
/// `bias` is the sound's six knobs above (SoundCoord::partialBias), applied with Smoothness in
/// partialFieldsWithOverrides. Its default is neutral, the terrain to the bit.
void buildPartialSound(const PartialBasis& frame, double home, double travelX, double travelY,
                       partials::PartialSpectrum& out, float smoothnessScale = kFieldNeutral,
                       const PartialBias& bias = {}, const PhaseWarp* warp = nullptr);

/// THE PER-SOUND KNOBS OVER THE ADDITIVE FIELDS, applied to the RAW fields (readPartialFields) before buildSound
/// clamps and smooths them: Smoothness, `fieldWithOverride(raw.smoothness, smoothnessScale)` — knob 0 defeats the
/// field, kFieldNeutral leaves it, 1 makes the sound the smooth one whatever the terrain says (design §4.1) — and
/// the six bias knobs, each `fieldWithOverride(raw.<its field>, bias.<knob>)` (PartialBias names the fields).
/// BEFORE THE SMOOTHNESS PULL, which buildSound applies to these fields after clamping: so Smoothness 1 is the
/// smooth spectrum whatever the bias asks for, as §4.1 promises. At kFieldNeutral every knob returns its field
/// exactly (`f · 1.0f` for a field in [0, 1]; one outside it is clamped here as buildSound would clamp it), so a
/// neutral sound's spectrum is today's to the bit. Every other field passes through untouched. Public so a case
/// can assert the FIELD, by value.
partials::PartialFields partialFieldsWithOverrides(const partials::PartialFields& raw, float smoothnessScale,
                                                   const PartialBias& bias = {});

/// THE KARPLUS-STRONG ENGINE'S MATERIAL: five fields, each in [0, 1],
/// read as GUESTS of the Additive rows (PAL stage 1's road) — a triangle of its own at a host row's placement — so the
/// string adds no basis row, no residue value and no stance member, and its pluck's tone and its material travel the
/// same frame. Zero is "nothing" for Noise and Stretch, which rest there (gated); the other three always have a value.
///   brightness -> the loop FIR's c: 0 dark (c 0.25) to 1 bright (c 0)
///   sustain    -> the T60's multiplier around Decay, 0.25x to 4x (log)
///   pick       -> the pick position, 0.04 to 0.40 of the string
///   noise      -> the noise's share of the pluck
///   stretch    -> the dispersion coefficient, 0 to -0.9 (sparse: most of the map is harmonic)
struct StringFields {
	float brightness{0.5f};
	float sustain{0.5f};
	float pick{0.25f};
	float noise{0.0f};
	float stretch{0.0f};
	friend bool operator==(const StringFields&, const StringFields&) = default;
};

/// THE STRING'S FIVE PER-SOUND FIELD SCALERS, one per sound A-H, each `fieldWithOverride` over the field it names:
/// knob 0 -> 0, kFieldNeutral -> the terrain's field, 1 -> 1. All neutral by default, so an untouched sound is the
/// terrain to the bit.
struct StringBias {
	float brightness{kFieldNeutral};
	float sustain{kFieldNeutral};
	float pick{kFieldNeutral};
	float noise{kFieldNeutral};
	float stretch{kFieldNeutral};
	friend bool operator==(const StringBias&, const StringBias&) = default;
};

/// The five fields at a home and a travel, raw, each its own triangle read at its host row's placement. They are the String
/// kind's five phase-warp sites (dsp/phase_warp.h), read under `warp` (null: none), the sound's likes.
StringFields readStringFields(const PartialBasis& frame, double home, double travelX, double travelY,
                              const PhaseWarp* warp = nullptr);
/// The five scalers over the raw fields. At kFieldNeutral each returns its field exactly.
StringFields stringFieldsWithOverrides(const StringFields& raw, const StringBias& bias);

/// **THE DRUM ENGINE'S SEVEN FIELDS**: Pitch, Length, Sweep, Tone, Noise, Snap
/// and Metal, each in [0, 1], read as GUESTS of the Additive rows (the string's road): a triangle of its own at a host row's
/// placement, so the drum adds no basis row, no residue value and no stance member. One bank for all six kinds: a place means
/// one physical thing to a kick and to a tom (drum::Fields).
///
/// THE PER-SOUND SCALERS, one per field, each `fieldWithOverride` over it; neutral by default, so an untouched pad is the terrain
/// to the bit.
struct DrumBias {
	float pitch{kFieldNeutral};
	float length{kFieldNeutral};
	float sweep{kFieldNeutral};
	float tone{kFieldNeutral};
	float noise{kFieldNeutral};
	float snap{kFieldNeutral};
	float metal{kFieldNeutral};
	friend bool operator==(const DrumBias&, const DrumBias&) = default;
};
/// The seven fields at a home and a travel, raw.
drum::Fields readDrumFields(const PartialBasis& frame, double home, double travelX, double travelY,
                            const PhaseWarp* warp = nullptr);
/// The seven scalers over the raw fields. At kFieldNeutral each returns its field exactly.
drum::Fields drumFieldsWithOverrides(const drum::Fields& raw, const DrumBias& bias);

} // namespace feathers
