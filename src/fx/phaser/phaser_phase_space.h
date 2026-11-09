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

#include "character/phi_triangle.hpp"
#include "dsp/slice_basis.h"
#include "dsp/slice_build.h" // fieldWithOverride, kFieldNeutral — the pills' law

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

// **THE PHASER/FLANGER'S N-SPACE — THIRTY-FOUR phi phases, and the plane the pad travels in.**
//
// The house pattern, mirrored rather than reinvented: `shaper_phase_space.h` for the storage and the plane,
// `automod_phase_space.h` for `basisFor` / `planeHomeOf` / `phasesAt`. The decisions behind it, all of which hold
// here unchanged: travel is UNBOUNDED (stored folded per phase, so a float never loses its fraction); there are NO
// NAMED ZONES (Rotate makes the map from plane to phases variable, so a name would be false the moment it turned);
// Rotate turns the plane THROUGH the space and leaves the puck; a drag decodes into a vector on the plane that adds
// to the integrated trajectory.
//
// **WHAT IS DIFFERENT HERE IS THE ROLE: A CHARACTER LAYER, NOT THE DRIVER** (the design doc, "The explore layer").
// Automod's phases ARE its parameters; the shaper's are its curve. This device's literal controls always mean what
// they say, and every phase below is an OFFSET UNDER them that is EXACTLY ZERO AT HOME:
//
//     value_i = scale_i · (tri(home_i + travel_i) − tri(home_i))
//
// with tri the unipolar phi triangle (`triangleSimpleUnipolar`, duty 1: 0 → 1 → 0 over a cycle). At zero travel
// `home_i + 0.0` IS `home_i`, bit for bit, so the two triangle reads are the same float and their difference is
// exactly 0.0 — under ANY Rotate, because Rotate never reaches the derive (it aims the next gesture; see the shaper's
// "Rotate alone leaves the shaper's phases standing", and this device's own case). With zero travel the device is
// the textbook phaser/flanger.
//
// **THE REACH OF EACH OFFSET IS ITS SCALE, BOUNDED**: tri ∈ [0, 1], so value_i ∈ [−tri(home_i), 1 − tri(home_i)] ·
// scale_i ⊂ [−scale_i, +scale_i]. Which way a phase can go furthest depends on where its home sits on its triangle
// — that is the decorrelation, not an accident: the homes are φ-spread so no two dimensions have the same reach.
//
// THE PHASE IS `home_i + travel_i`, where `travel_i` is the stored, folded float — which is what `SliceBasis::
// phaseFor(i, home_i, tx, ty)` returns once the pad has folded its travel term for storage (`wrapForStorage`): the
// pad (Task 9, `ShaperPad`'s road) accumulates `origin + basis.x·tx + basis.y·ty` per phase and stores it folded;
// the derive adds it to the home. Folding changes the phase by a whole turn, which the triangle cannot see.
//
// ── THE INDEX TABLE, MODE BY MODE ────────────────────────────────────────────────────────────────────────────────
//
//   index    dimension               scale            PHASER                         FLANGER
//   0..11    per-stage spread        ±1.5 octaves     stage i's corner × 2^spread_i  0, 1, 2: tap k's delay × 2^spread_k
//                                                     (both sweeps: Normal and         under the barberpole; in Normal,
//                                                     barberpole, every voice)          0 = the L delay, 1 = the R delay,
//                                                     stages ≥ Stages: inert            and 2 is INERT (Normal runs one
//                                                                                       tap a channel). 3..11: INERT
//   12..23   per-stage LFO phase     ±0.5 cycle       Normal: stage i reads           12, 13, 14: tap k's sweep offset under
//            offset                                    lfoShape(φ + off_i).            the barberpole (as the phaser's);
//                                                     Barberpole: stage i's sweep     in Normal, 12 = L's LFO phase,
//                                                     + LfoDepth·(lfoShape(φ+off_i)   13 = R's, and 14 is INERT.
//                                                     − lfoShape(φ)), NEVER ψ.        15..23: INERT
//                                                     stages ≥ Stages: inert
//   24       LFO skew                0..1 (|·|)       the LFO's phase map bent        the same (Normal); the barberpole's
//   25       LFO squareness          0..1 (|·|)       toward a square (soft clip)     ramp ignores the LFO's shape, so
//   26       LFO triangle blend      0..1 (|·|)       mixed toward a triangle          under it only the tap offsets
//                                                     (Normal; under the barberpole   (12..14) read it
//                                                     only the per-stage offsets
//                                                     read it)
//   27       damping colour          ±2 octaves       the loop lowpass's corner (only while Damping is below its top:
//                                                     an OPEN loop stays open — the colour never closes it — and the
//                                                     colour FADES OUT over the knob's top octave, so the coloured
//                                                     corner meets the plain one where the lowpass leaves the loop:
//                                                     `colouredDampingCornerHz`)                              — both
//   28       drive asymmetry         ±0.5             a bias inside the saturator's tanh, its DC removed and the curve
//                                                     kept non-expansive (`saturate(v, drive, bias)`); it ramps in
//                                                     with Drive up to 0.1 — at Drive 0 there is nothing to bias.
//                                                     IT ALSO LOWERS THE SMALL-SIGNAL LOOP GAIN: the curve's slope at
//                                                     0 is 1 − tanh²(b) (0.79 at |b| = 0.5), so a biased loop rings
//                                                     shorter than the same Feedback unbiased. And the loop's own DC
//                                                     (an asymmetric curve rectifies) is taken out by a ~10 Hz
//                                                     blocker that runs only while the bias is nonzero        — both
//   29       stereo skew             ±0.5 octave      the R channel's corner (phaser) / delay (flanger)       — both
//
// ROUND 2 (more fields, every one exposed as a scaler pill) — four more, appended, so the thirty above keep their indices, homes and φ multipliers:
//
//   30       rate skew               ±0.25 octave     R's LFO runs at L's rate · 2^skew, so the stereo image ROLLS (R
//                                                     has its own phase accumulator and its own song lock at the
//                                                     skewed rate — the Free road's arrangement for a modulated Rate).
//                                                     Under the barberpole ψ_R's own phase turns at
//                                                     `barberCyclesPerSecond` of that skewed rate        — both
//   31       feedback tilt           ±0.3             PHASER, Normal: fb_L = fb + tilt/2, fb_R = fb − tilt/2.
//                                                     PHASER, barberpole: voice k's loop runs fb + tilt·(k − 1), both
//                                                     channels (each voice has its own loop there).
//                                                     FLANGER, both sweeps: the L/R reading — its taps share ONE loop a
//                                                     channel, so there is no voice loop to tilt.
//                                                     Added to the stored Feedback BEFORE the clamp and the ceiling
//                                                     (`loopFeedbackOf`)
//   32       notch pairing           ±1 octave        PHASER: stage 2j's corner × 2^(+p), stage 2j+1's × 2^(−p), on top
//                                                     of the spread and before the clamp (`stageSpreadOf`), so each
//                                                     pair parts or (with spread) closes. FLANGER: INERT
//   33       sweep curve             ±1 (c)           `s → sign(s)·|s|^(2^c)` on the SUMMED sweep, before its clamp
//                                                     (`sweepOf`'s `curveExp`): every primary sweep — each channel's,
//                                                     each Normal per-stage sweep, each barberpole voice's s_k, the
//                                                     flanger's per-sample reads. A per-stage or per-tap OFFSET added to
//                                                     an already-formed sweep (`offsetSweepOf`) is not curved again
//                                                                                                       — both
//
// The shape amounts are MAGNITUDES, |tri(phase) − tri(home)|, so each is 0 at home and 0..1 everywhere: a shape
// that is "sine at 0" has no negative side to go to.
//
// AN INDEX THAT IS INERT IN A MODE IS STILL READ BY THE DERIVE (`exploreOf` reads all thirty-four, mode-free), and the
// case "explore: every phase index is read by the derive exactly once" counts it. The pad is not a control, so an
// inert direction is not an honesty problem — it is a direction the plane leans in that this mode does not hear.
//
// ── THE GROUPS AND THEIR PILLS ───────────────────────────────────────────────────────────────────────────────────
//
// The thirty-four phases fall into TEN GROUPS — Spread (0..11), Stagger (12..23), Shape (24..26), Colour (27), Bias
// (28), Skew (29), Rate skew (30), Fb tilt (31), Pairing (32), Curve (33) — and each group has a FIELD-SCALER PILL in
// the window (a knob 0..1, default 0.5, and a power).
//
// **THE PILL IS THE HOUSE OVERRIDE LAW, `fieldWithOverride` (`dsp/slice_build.h`), ON THE FIELD** (chosen over the round-2 multiplier because it can drive to 0 or 1 regardless of the field). Each phase's field is
// f_i = tri(home_i + travel_i) ∈ [0, 1], and the pill stands IN FRONT of it, exactly as the shaper's extras and Explore's
// field-modifier knobs stand in front of theirs:
//
//     value_i = power_g ? scale_i · (fieldWithOverride(f_i, knob_g) − tri(home_i)) : 0
//
//   - KNOB 0.5 (`kFieldNeutral`, the default) IS THE FIELD UNTOUCHED, bit for bit: `f · (0.5f · 2.0f)` is f · 1.0f.
//     So at the default the derive is the round-2 one exactly, and zero travel is still exactly 0.0.
//   - KNOB 0 FORCES f TO 0 and KNOB 1 FORCES IT TO 1, whatever the travel — so a pill away from 0.5 moves the sound AT
//     HOME too: zero travel is the textbook device only with every pill at 0.5 (or off). Between, it is linear (a
//     multiply below 0.5, a lift toward 1 above it).
//   - POWER OFF IS NO EFFECT: the group's offsets are exactly +0.0, which is that group's travel at 0. (Unlike the
//     shaper, whose power off is its knob at 0 — there an off extra is a field forced to 0, here it is the textbook.)
//   - THE REACH IS THE FIELD'S: value_i ∈ scale_i · [−tri(home_i), 1 − tri(home_i)], the same bound travel alone has —
//     the round-2 multiplier's 200% (twice that) is gone with it.
//
// WHAT "FORCED TO 0 / 1" MEANS GROUP BY GROUP — the law is the same on f everywhere; what differs is what f feeds:
//   - Spread, Stagger, Colour, Bias, Skew, Rate skew, Fb tilt, Pairing, Curve: a straight `scale · (f − f_home)`, so
//     knob 1 is `scale · (1 − tri(home))` and knob 0 is `−scale · tri(home)` — the furthest each can go each way. The
//     Curve's value is c, and the sweep's exponent 2^c: knob 1 is the most linger-at-the-ends exponent the layer has,
//     knob 0 the most linger-in-the-middle one.
//   - Shape (24..26): the amounts are MAGNITUDES, |f − f_home|, so BOTH ends bend the LFO — knob 1 gives each amount
//     1 − tri(home), knob 0 gives tri(home). Neither end is "a sine": the sine is knob 0.5 at zero travel (or off).

namespace feathers::fx::phaser::phase_space {

namespace phi = owlet::dsp::phi;

/// THE DEVICE'S N — MEASURED from the derive (`exploreOf`): 12 + 12 + 3 + 1 + 1 + 1, and round 2's 1 + 1 + 1 + 1. The
/// extra state's width is a stored fact (`Traits::extraState`), keyed, so a change is not a format break.
inline constexpr size_t kSpreadCount = 12;
inline constexpr size_t kLfoPhaseCount = 12;
inline constexpr size_t kLfoShapeCount = 3;
inline constexpr size_t kPhaseCount = kSpreadCount + kLfoPhaseCount + kLfoShapeCount + 3 + 4;
static_assert(kPhaseCount == 34);

/// THE GROUPS' FIRST INDICES. Per stage, the two twelves are indexed by stage.
inline constexpr size_t kSpreadFirst = 0;     ///< per-stage corner spread, 12
inline constexpr size_t kLfoPhaseFirst = 12;  ///< per-stage LFO phase offset, 12
inline constexpr size_t kLfoShapeFirst = 24;  ///< the LFO's shape: skew, squareness, triangle blend
inline constexpr size_t kDampingPhase = 27;   ///< the loop's damping colour
inline constexpr size_t kDrivePhase = 28;     ///< the saturator's asymmetry
inline constexpr size_t kStereoPhase = 29;    ///< the stereo skew
inline constexpr size_t kRateSkewPhase = 30;  ///< R's LFO rate against L's
inline constexpr size_t kFbTiltPhase = 31;    ///< the feedback tilt across channels (or barberpole voices)
inline constexpr size_t kPairingPhase = 32;   ///< the phaser's notch pairing
inline constexpr size_t kCurvePhase = 33;     ///< the sweep curve
static_assert(kLfoPhaseFirst == kSpreadFirst + kSpreadCount && kLfoShapeFirst == kLfoPhaseFirst + kLfoPhaseCount
                  && kDampingPhase == kLfoShapeFirst + kLfoShapeCount && kDrivePhase == kDampingPhase + 1
                  && kStereoPhase == kDrivePhase + 1 && kRateSkewPhase == kStereoPhase + 1
                  && kFbTiltPhase == kRateSkewPhase + 1 && kPairingPhase == kFbTiltPhase + 1
                  && kCurvePhase == kPairingPhase + 1 && kCurvePhase + 1 == kPhaseCount,
              "the groups tile the space: 12 + 12 + 3 + 1 + 1 + 1 + 1 + 1 + 1 + 1 == kPhaseCount");

/// THE SCALES — what one whole triangle of departure is worth on each dimension (the table above).
inline constexpr double kSpreadOctaves = 1.5;
inline constexpr double kLfoPhaseCycles = 0.5;
inline constexpr double kShapeScale = 1.0;
inline constexpr double kDampingOctaves = 2.0;
inline constexpr double kDriveBias = 0.5;
inline constexpr double kStereoOctaves = 0.5;
inline constexpr double kRateSkewOctaves = 0.25;
inline constexpr double kFbTilt = 0.3;
inline constexpr double kPairingOctaves = 1.0;
inline constexpr double kCurveScale = 1.0;

/// **THE TEN GROUPS, ONE PILL EACH** (the header's "groups and their pills"). Declaration order is the pills' order
/// in the window and the order of `Param::XSpread …`'s pairs.
enum class Group : size_t { Spread, Stagger, Shape, Colour, Bias, Skew, RateSkew, FbTilt, Pairing, Curve, kCount };
inline constexpr size_t kGroupCount = static_cast<size_t>(Group::kCount);
/// THE GROUP AN INDEX BELONGS TO — the index table, grouped.
[[nodiscard]] constexpr Group groupOf(size_t index) noexcept {
	if (index < kLfoPhaseFirst) {
		return Group::Spread;
	}
	if (index < kLfoShapeFirst) {
		return Group::Stagger;
	}
	if (index < kDampingPhase) {
		return Group::Shape;
	}
	switch (index) {
	case kDampingPhase: return Group::Colour;
	case kDrivePhase: return Group::Bias;
	case kStereoPhase: return Group::Skew;
	case kRateSkewPhase: return Group::RateSkew;
	case kFbTiltPhase: return Group::FbTilt;
	case kPairingPhase: return Group::Pairing;
	default: return Group::Curve;
	}
}
/// **THE TEN PILLS AS THE DERIVE HEARS THEM** — each group's knob (0..1, `kFieldNeutral` the field untouched) and its
/// power. The defaults, every knob 0.5 and every power on, are the layer exactly as Task 7 built it.
struct Pills {
	std::array<float, kGroupCount> knob = [] {
		std::array<float, kGroupCount> k {};
		k.fill(feathers::kFieldNeutral);
		return k;
	}();
	std::array<bool, kGroupCount> on = [] {
		std::array<bool, kGroupCount> o {};
		o.fill(true);
		return o;
	}();
	friend bool operator==(const Pills&, const Pills&) = default;
};
/// A STORED KNOB AS THE LAW READS IT: a non-finite one is the neutral 0.5, so a hand-built `Settings` cannot put a NaN
/// into the derive (`fieldWithOverride` clamps a finite knob to [0, 1] itself; its NaN would pass both clamps).
[[nodiscard]] inline float pillKnobOf(float stored) noexcept {
	return std::isfinite(stored) ? stored : feathers::kFieldNeutral;
}

using Phases = std::array<double, kPhaseCount>;
using Travel = std::array<float, kPhaseCount>;

/// **THE φ MULTIPLIERS: φ^((i+1)/4), one per dimension, in index order.** The shaper reads eight named φ powers
/// (kPhi225 … kPhi100); thirty-four need a ladder, and quarter powers of φ are the ladder those names are rungs of
/// (kPhi025, kPhi050, kPhi075, kPhi100 …). Built by repeated multiplication so it is constexpr and the same on every
/// machine. ORDER IS A STORED FACT: the travel blob is one float per entry BY INDEX.
inline constexpr double kGolden = 1.6180339887498949;
inline constexpr double kPhiQuarter = 1.1278384855616823; // φ^(1/4)
constexpr std::array<double, kPhaseCount> makePhi() {
	std::array<double, kPhaseCount> out {};
	double m = 1.0;
	for (size_t i = 0; i < kPhaseCount; ++i) {
		m *= kPhiQuarter;
		out[i] = m;
	}
	return out;
}
inline constexpr std::array<double, kPhaseCount> kPhi = makePhi();

/// **EACH DIMENSION'S CONSTANT OFFSET: frac(i·φ⁻¹)**, the golden (Weyl) sequence — the shaper's `kOffset` job
/// (spreading the triangles around the cycle instead of stacking them), for thirty-four, and the same fraction
/// `slice_basis.h`'s `planeEntry` spreads its coordinates by.
constexpr std::array<double, kPhaseCount> makeOffset() {
	std::array<double, kPhaseCount> out {};
	for (size_t i = 0; i < kPhaseCount; ++i) {
		double v = static_cast<double>(i) * (kGolden - 1.0);
		while (v >= 1.0) {
			v -= 1.0;
		}
		out[i] = v;
	}
	return out;
}
inline constexpr std::array<double, kPhaseCount> kOffset = makeOffset();

/// THE HOME'S FIXED "GAMMA". The shaper's home is `colour·φ·mult + gamma·φ + offset`; this device has no knob that
/// moves its home (the literal controls are its home), so the home is that expression at gamma 1, colour 0.
inline constexpr double kHomeGamma = 1.0;

/// **WHERE EACH PHASE SITS BEFORE ANY TRAVEL**: `kHomeGamma · kPhi[i] + kOffset[i]`, UNWRAPPED (the shaper's
/// reason: the triangle wraps at the point of use, so travel can accumulate past a turn without folding under it).
/// A pure constant — no control moves it — so the plane a pad travels in depends on Rotate alone.
constexpr Phases makeHome() {
	Phases out {};
	for (size_t i = 0; i < kPhaseCount; ++i) {
		out[i] = kHomeGamma * kPhi[i] + kOffset[i];
	}
	return out;
}
inline constexpr Phases kHome = makeHome();
[[nodiscard]] inline const Phases& homeFor() noexcept { return kHome; }

/// THE SCALAR THE PLANE IS ANCHORED AT — the mean of the phases, automod's and the shaper's reduction. `home` is the
/// home plus whatever travel the pad stands on (`ShaperPad::reOrigin`'s arrangement).
[[nodiscard]] inline double planeHomeOf(const Phases& home) noexcept {
	double sum = 0.0;
	for (const double v : home) {
		sum += v;
	}
	return sum / static_cast<double>(kPhaseCount);
}

using Basis = SliceBasis<kPhaseCount>;

/// **THE PLANE, FROM ROTATE AND THE HOME — `sliceBasisFor`, EXPLORE'S OWN**, automod's `basisFor` signature exactly:
/// `home` is the ONE scalar the plane is anchored at, `planeHomeOf(...)`. Rotate stays non-modulatable: a moving
/// basis would make one gesture mean something different from block to block.
[[nodiscard]] inline Basis basisFor(double rotate, double home) {
	return sliceBasisFor<kPhaseCount>(rotate, home);
}

/// WHERE EVERY TRIANGLE IS EVALUATED for a position (tx, ty) on a plane — `phaseFor`, so at zero travel this IS
/// `home`, bit for bit, whatever the basis (automod's `phasesAt`).
[[nodiscard]] inline Phases phasesAt(const Phases& home, const Basis& basis, double travelX, double travelY) {
	Phases out {};
	for (size_t i = 0; i < kPhaseCount; ++i) {
		out[i] = basis.phaseFor(i, home[i], travelX, travelY);
	}
	return out;
}

/// **THE ACCUMULATOR, REDUCED FOR STORAGE — modulo ONE**, exactly the shaper's `wrapForStorage`: the travel is added
/// to a phase, so a whole turn is one for every entry. Reduced at all because a float with a large integer part has
/// no fraction left. Non-finite becomes zero (it would reappear every time the patch loads).
inline void wrapForStorage(const Phases& accumulator, Travel& stored) {
	for (size_t i = 0; i < kPhaseCount; ++i) {
		const double v = std::isfinite(accumulator[i]) ? accumulator[i] : 0.0;
		stored[i] = static_cast<float>(v - std::floor(v));
	}
}

/// THE PHI TRIANGLE the derive reads: unipolar, duty 1, wrapped in double first (`phi::wrapPhase`) so an unbounded
/// phase keeps its fraction.
[[nodiscard]] inline float fieldAt(double phase) noexcept {
	return owlet::dsp::triangleSimpleUnipolar(phi::wrapPhase(phase), 1.0f);
}
[[nodiscard]] inline double triangleAt(double phase) noexcept { return static_cast<double>(fieldAt(phase)); }

/// **ONE FIELD PER PHASE**, f_i = tri(home_i + travel_i) ∈ [0, 1], as the float the triangle returns — what the pills'
/// `fieldWithOverride` takes. Kept by the engine between travel changes, so a pill's move re-derives from these
/// without a triangle read.
using Fields = std::array<float, kPhaseCount>;

/// EVERY PHASE'S FIELD AT HOME, tri(home_i) — the reference each offset is measured from. `kHome` is a constant, so this
/// is too; built once at load (the triangle is not constexpr), never on the audio thread.
inline const Fields kHomeField = [] {
	Fields f {};
	for (size_t i = 0; i < kPhaseCount; ++i) {
		f[i] = fieldAt(kHome[i]);
	}
	return f;
}();

/// **THE LFO'S SHAPE, BENT** — three amounts, each 0..1, and all three 0 is the sine exactly (`lfoShape` in
/// `phaser_types.h` takes the home road then, the same expression as before the layer existed).
struct LfoBend {
	double skew = 0.0;     ///< the phase map bent: the peak moves earlier
	double square = 0.0;   ///< a soft clip toward a square
	double triangle = 0.0; ///< mixed toward a triangle of the same phase
	[[nodiscard]] bool atHome() const noexcept { return skew == 0.0 && square == 0.0 && triangle == 0.0; }
	friend bool operator==(const LfoBend&, const LfoBend&) = default;
};

/// **EVERYTHING THE LAYER DERIVES, in the units the engine uses** — every member 0.0 at home.
struct Explore {
	std::array<double, kSpreadCount> spreadOctaves {}; ///< 0..11
	std::array<double, kLfoPhaseCount> lfoPhase {};    ///< 12..23, cycles
	LfoBend bend {};                                   ///< 24..26
	double dampingOctaves = 0.0;                       ///< 27
	double driveBias = 0.0;                            ///< 28, in the saturator's tanh argument
	double stereoOctaves = 0.0;                        ///< 29, on the R channel
	double rateSkewOctaves = 0.0;                      ///< 30, R's LFO rate against L's, octaves
	double feedbackTilt = 0.0;                         ///< 31, added to the stored Feedback per channel or voice
	double pairingOctaves = 0.0;                       ///< 32, stage 2j up and 2j+1 down by this
	double sweepCurve = 0.0;                           ///< 33, c: the sweep's exponent is 2^c
	friend bool operator==(const Explore&, const Explore&) = default;
};

/// **THE FIELDS — reads every phase's travel EXACTLY ONCE**, mode-free (a mode that does not hear an index still reads
/// it; the table above says which). `reads`, when given, counts each index's reads: the case "every phase index is read
/// by the derive exactly once" walks it, which is what MEASURES `kPhaseCount` rather than asserting it. At zero travel
/// `home_i + 0.0` is `home_i`, so f_i is `kHomeField[i]` bit for bit.
[[nodiscard]] inline Fields fieldsOf(const Travel& travel, std::array<int, kPhaseCount>* reads = nullptr) noexcept {
	Fields f {};
	for (size_t i = 0; i < kPhaseCount; ++i) {
		if (reads != nullptr) {
			++(*reads)[i];
		}
		const float t = travel[i];
		f[i] = fieldAt(kHome[i] + (std::isfinite(t) ? static_cast<double>(t) : 0.0));
	}
	return f;
}

/// ONE DIMENSION'S OFFSET, THE PILL IN FRONT OF ITS FIELD: `scale · (fieldWithOverride(f, knob) − tri(home))`, and
/// exactly +0.0 with the power off. THE ONE `fieldWithOverride` — its own comment says why there is no second copy.
/// At knob 0.5 it returns f itself, so this is the round-2 `scale · (tri(home + travel) − tri(home))` bit for bit.
[[nodiscard]] inline double offsetOf(size_t i, float field, float knob, bool on, double scale) noexcept {
	if (!on) {
		return 0.0;
	}
	const double forced = static_cast<double>(feathers::fieldWithOverride(field, pillKnobOf(knob)));
	return scale * (forced - static_cast<double>(kHomeField[i]));
}

/// **THE DERIVE: the fields, each through its group's pill, in the engine's units.** Every member is +0.0 at zero travel
/// with every pill at 0.5 (or off); see the header for what a knob away from 0.5 does there.
[[nodiscard]] inline Explore exploreOfFields(const Fields& f, const Pills& pills) noexcept {
	const auto at = [&](size_t i, double scale) {
		const auto g = static_cast<size_t>(groupOf(i));
		return offsetOf(i, f[i], pills.knob[g], pills.on[g], scale);
	};
	Explore e;
	for (size_t s = 0; s < kSpreadCount; ++s) {
		e.spreadOctaves[s] = at(kSpreadFirst + s, kSpreadOctaves);
	}
	for (size_t s = 0; s < kLfoPhaseCount; ++s) {
		e.lfoPhase[s] = at(kLfoPhaseFirst + s, kLfoPhaseCycles);
	}
	// THE SHAPE AMOUNTS ARE MAGNITUDES: 0 at home, 0..1 anywhere (a sine has no negative skew to go to). So both ends of
	// the Shape pill bend the LFO: knob 1 is 1 − tri(home) per amount, knob 0 is tri(home).
	e.bend.skew = std::fabs(at(kLfoShapeFirst + 0, kShapeScale));
	e.bend.square = std::fabs(at(kLfoShapeFirst + 1, kShapeScale));
	e.bend.triangle = std::fabs(at(kLfoShapeFirst + 2, kShapeScale));
	e.dampingOctaves = at(kDampingPhase, kDampingOctaves);
	e.driveBias = at(kDrivePhase, kDriveBias);
	e.stereoOctaves = at(kStereoPhase, kStereoOctaves);
	e.rateSkewOctaves = at(kRateSkewPhase, kRateSkewOctaves);
	e.feedbackTilt = at(kFbTiltPhase, kFbTilt);
	e.pairingOctaves = at(kPairingPhase, kPairingOctaves);
	e.sweepCurve = at(kCurvePhase, kCurveScale);
	return e;
}
/// THE DERIVE FROM TRAVEL, WITH THE PILLS — `exploreOfFields(fieldsOf(travel), pills)`. Every index read exactly once.
[[nodiscard]] inline Explore exploreOf(const Travel& travel, const Pills& pills,
                                       std::array<int, kPhaseCount>* reads = nullptr) noexcept {
	return exploreOfFields(fieldsOf(travel, reads), pills);
}
/// THE DERIVE AT THE DEFAULT PILLS (every knob 0.5, every power on) — the layer as Task 7 built it.
[[nodiscard]] inline Explore exploreOf(const Travel& travel, std::array<int, kPhaseCount>* reads = nullptr) noexcept {
	return exploreOfFields(fieldsOf(travel, reads), Pills {});
}

} // namespace feathers::fx::phaser::phase_space
