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
#include "dsp/basic_shapes.h"
#include "dsp/slice_build.h"
#include "dsp/warp.h"

// <numbers> EXPLICITLY, and this is the second time this family has paid for it. libstdc++ drags it
// in transitively so a Linux build never notices; libc++ does not, so `std::numbers::pi` in
// fmIndexCyclesFor below built here and broke the macOS leg outright. fm_carrier.h's own header
// comment warns about the SAME split for constexpr std::sin — "a portability bug that builds at home
// and fails in CI" — which is exactly what this was.
#include <array>
#include <numbers>

// THE FLAGSHIP'S OPERATOR TIER — three cycle operators whose depths are φ fields of position.
//
// The flagship (OscType::V5) builds the morph recipe through the existing slice decode, then puts
// the built cycle through three operators: hard sync, self-feedback, quantize. Their depths are not
// params and not stored per-sound values — they are φ triangles read at the sound's decoded
// position (deliberately not automatable for now: they are derived from the terrain), so WHERE a sound sits decides
// how operated it is, and the invariant every engine here lives by — sound is a pure function of
// position — holds for the operators by construction.
//
// WHERE THE FIELDS ARE READ (decision V7): at the morph frame's LEAD coordinate,
// `phaseFor(0, home, travelX, travelY)`. Plain home (the blue-shift spelling) was rejected because
// fields meant to be TERRAIN must move under travel and re-aim under rotate; a dedicated basis for
// the operator triangles was rejected because its rows would need their own residue, and residue is
// persisted state this tier may not grow. The lead coordinate gives all three properties at zero
// state cost: it is travel-coupled, it turns with the plane, and it is residue-protected — so
// rotate-at-drift preserves the standing sound's operators exactly, extending decision A26 to the
// operator tier for free. Precedent: the terrain macro reads its field at home + leadPhaseSlope ·
// travel, the same idiom (slice_basis.h, leadPhaseSlope — "the liveliest of the thirty-five").
// At rest the lead coordinate IS home, bit for bit, which is blue-shift parity.
//
// HOW THE OPERATED CYCLE RENDERS (decision V8): all three depths exactly zero — a real region, see
// the duties below — leaves the recipe unoperated and the voice on the untouched continuous morph
// path, so the identity corner coincides with Morph bit for bit BY CONSTRUCTION. Any depth nonzero
// bakes the operated cycle onto a 256-slot grid with two binomial mips, the stair render idiom at
// twice stair's resolution. Crossing a field-zero boundary therefore switches render mechanism;
// that seam is accepted on record, masked by the 2 ms rebuild crossfade like every character
// change, with an S3 audition item on the quantize-zero boundary at high pitch.

namespace feathers {

struct Character;

/// Forward-declared rather than included: the operator tier has no business knowing how a vowel is
/// baked, and vox_bake.h reads the rates below, so an include here would close a cycle. A pointer to
/// an incomplete type is all the seam needs.
struct VoxCycle;

/// The three operator rates, DERIVED rather than transcribed so their ratios are exact φ powers —
/// the same guarantee the blue-shift pair states, for the same reason: φ-to-four-places divided by
/// φ-to-four-places is not φ.
///
/// THE LONGEST-RANGE FIELDS IN THE ENGINE, deliberately. Operators are REGIONS — quantize country,
/// sync ridges, rare feedback passes — so their periods sit below everything already spinning:
/// the character banks run φ^-0.5…φ^3.75 (about 170–1300 zones per period), the blue-shift pair
/// φ^-0.75 and φ^-1.75 (~1470 and ~2370 zones). These land at φ^-2.25, φ^-2.75, φ^-3.25 — about
/// 3030, 3840 and 4880 zones — countries bigger than weather. The exponents are quarter-fractions,
/// so no rate coincides with any integer rung of the orientation ladder (φ^-1…φ^-7), and the
/// pairwise ratios are φ^0.5, φ^0.5 and φ^1 — all irrational, so no two fields ever realign with
/// each other, with the blue shift, or with the banks. This is also what the widest-stop finding
/// asked for: the banks have no long-range members, and now the engine does.
inline constexpr float kFlagshipQuantizeRate = 1.0f / owlet::dsp::phi::kPhi225;
inline constexpr float kFlagshipSyncRate = kFlagshipQuantizeRate / owlet::dsp::phi::kPhi050;
inline constexpr float kFlagshipFeedbackRate = kFlagshipSyncRate / owlet::dsp::phi::kPhi050;

/// DUTIES ARE HOW MUCH OF THE SPACE EACH OPERATOR TOUCHES — a unipolar triangle is exactly zero
/// outside its duty, so a duty is a fraction of the terrain, not a loudness.
///
///   - Quantize 0.85: wide, per the plan — quantize is the safe dimension — but deliberately not
///     1.0. The ~15% left untouched keeps pure morph a real PLACE in the flagship's terrain rather
///     than a corner case, and gives the identity anchor natural territory to stand on.
///   - Sync 0.50: moderate — ridges over about half the space.
///   - Feedback 0.40 with a 0.75 threshold on top (see kFlagshipFeedbackThreshold): the §5.3
///     grey-goo rule. Feedback is the dangerous one, so it is exactly zero across ~90% of the
///     terrain — duty × (1 − threshold) = 0.40 × 0.25 — and what remains is rare localized peaks.
///
/// The offsets spread the three peaks across the period, clear of each other and of the blue-shift
/// pair's 0.230/0.660, so no two fields crest in the same neighbourhood and none peaks at the
/// origin an audition starts from.
inline constexpr std::array<owlet::dsp::phi::PhiTriConfig, 3> kFlagshipFieldBank = { {
    { kFlagshipQuantizeRate, 0.85f, 0.150f, false },
    { kFlagshipSyncRate, 0.50f, 0.480f, false },
    { kFlagshipFeedbackRate, 0.40f, 0.820f, false },
} };

/// The feedback field keeps only the top quarter of its triangle: k = max(0, tri − 0.75) · 4, the
/// plan's own max(0, tri − threshold) spelling. Continuous at the threshold, zero below it.
inline constexpr float kFlagshipFeedbackThreshold = 0.75f;

/// MIXING CHOICES, reported rather than derived and meant to be tuned by ear at S3 — the same
/// status as every coefficient in this family.
///
/// Sync ratio spans [1, 3]: r = 1 + 2s. Ratio 3 is strong classic sync without leaving musical
/// territory; the plan asked for moderate.
/// Feedback depth tops out at 0.18 cycles of phase push — the league morphPhaseDistort's ±0.15
/// clamp already plays in, so the operator sounds like the engine's own phase machinery driven
/// harder, not a new effect wearing its name. Three iterations, fixed: bounded, deterministic,
/// and enough for the characteristic fold without runaway.
inline constexpr float kFlagshipSyncMaxRatio = 3.0f;
inline constexpr float kFlagshipFeedbackMax = 0.18f;
inline constexpr int kFlagshipFeedbackIterations = 3;

/// HOW MANY CONTROL POINTS A SMOOTHED SOURCE CAN CARRY — the cycle's endpoint plus every movable
/// waypoint the morph table has, and never fewer than two, which is the count that gives a sine.
inline constexpr int kFlagshipMaxSmoothPoints = owlet::dsp::kPhiMorphMovableWaypoints + 1;
inline constexpr int kFlagshipMinSmoothPoints = 2;

/// HOW MANY POINTS THE NORMALISATION IS MEASURED FROM, and there are TWO answers because the two
/// callers are asking for different things.
///
/// The true peak can fall between probe points, so a coarse probe leaves the real peak above 1 — and
/// the cost is paid once per RECIPE, which the terrain builds sixteen thousand times per map.
/// MEASURED, on a real 128x128 V5 build: 72.7 ms with no normalisation, 134.1 at 32 points, 161.2 at
/// 64, 306.1 at 128. And measured on the accuracy side, over a grid of 60 positions and knob
/// settings, the number of cycles left outside a healthy band: 45 with none, 8 at 32 points, 3 at 64,
/// and 0 at 128 with a worst peak of exactly 1.0000.
///
/// THE AUDIO TAKES 128, because the cost there is once per character build and the failure is
/// clipping — a cycle that peaks at 1.20 with the Level fader at 0 dB is over full scale for a reason
/// the musician cannot see or fix.
///
/// THE PICTURE TAKES 32, because of what the map actually reads. Of the five features a texel's
/// colour is made from, four — centroid, roughness, parity and fundamental — are RATIOS of the
/// magnitude spectrum and are invariant to both the DC removal and the gain. Only `energy` moves with
/// the gain, and it drives luminance. A gain a few percent out is a luminance a few percent out,
/// which no eye can see and which buys back more than half the build time. The map still normalises
/// rather than skipping it, because after this change every place really is peak 1.0 and a luminance
/// drawn from the un-normalised cycle would be a picture of a loudness the engine no longer has.
/// AND WHAT THE AUDIO PROBE COSTS THE AUDIO THREAD, because that is the next question and it should
/// not have to be re-derived. buildFlagshipFor runs inside CharacterBank::setCharacter, which the
/// synth calls from render() — so this probe is on the audio thread, and a puck drag can ask for a
/// build every block.
///
/// MEASURED: a full flagship build (recipe + bakeFlagshipTable's 256 slots x 3 mips) is 28.3 us with
/// the probe and 19.6 us without, so the probe is 8.7 us — 31% of a build, and 0.33% of one
/// 128-sample block at 48 kHz. Left alone at that price.
///
/// THE CHEAPER SHAPE, IF IT EVER MATTERS: the bake already computes 256 exact slots, so normalising
/// from those would be free AND more accurate than 128 probe points. It is not done because the
/// terrain and the scope call evalFlagship without ever baking, so they would need the probe anyway
/// — and two ways of computing one coefficient is the thing this file keeps paying to avoid.
/// THE AUDIO PROBE, 128 — AND A WARPED RECIPE OVERRIDES IT WITH 256. See `prepareFlagshipRecipe`.
///
/// 128 STAYS FOR EVERY UNWARPED RECIPE, and that is a deliberate refusal rather than an oversight.
/// Raising it globally to 256 makes the peak EXACT for every operated cycle — measured, and it also
/// closes a pre-existing defect (see below) — but it changes `r.gain` on sounds nobody asked to
/// change, and `test_vox_voice.cpp` pins those bits on purpose. **A warp feature may not re-level
/// the whole engine on its way in.** The finding is reported rather than acted on; the call.
///
/// **THE PRE-EXISTING DEFECT, MEASURED, so it is not lost.** Worst baked peak over 64 zones with NO
/// WARP AT ALL: **1.118 at 128 probe points, 3 of 64 zones over 1.05, and exactly 1.000 at 256.**
/// That is this file's own failure criterion — "a cycle that peaks at 1.20 with the Level fader at
/// 0 dB is over full scale for a reason the musician cannot see or fix" — happening today.
///
/// AND THE OLD ACCURACY TABLE HERE IS NOT REPRODUCIBLE. It recorded "0 outside the band at 128 with
/// a worst peak of exactly 1.0000" over 60 positions; over the 64 zones measured on 2026-08-28 the
/// worst is 1.118. The engine has grown a smooth source and a vowel since, so this is a claim that
/// aged rather than one that was wrong.
inline constexpr int kFlagshipNormaliseSamples = 128;

/// WHAT A WARPED RECIPE PROBES AT, and the number is not "denser" — it is **exact**.
///
/// It is `kFlagshipSlots`. An operated voice renders the BAKED GRID, 256 slots of it, so a probe at
/// 256 uniform phases samples the bake's own points and measures the peak of the signal that is
/// actually played. There is no interpolation error left to converge away, which is why 512 and 1024
/// measure identically.
///
/// **WARP IS WHAT FORCES IT.** A warp compresses read phase somewhere by construction — that is the
/// effect — so a probe uniform in OUTPUT phase becomes sparse in READ phase exactly where the curve
/// is steepest. MEASURED, worst baked peak over 64 zones at full depth, n=128 against n=256:
///
///     Off 1.118 -> 1.000    Bend - 1.659 -> 1.000    Asym + 1.616 -> 1.000    PWM 2.881 -> 1.000
///
/// PWM at 128 is 9 dB over full scale. And "raise it a bit" would not have worked: MEASURED
/// convergence at 128/192/256 on one position ran 0.534 / 0.733 / 0.580 — **non-monotone**, because
/// a sparse probe beats against the cycle's own structure. Matching the grid is what fixes it.
///
/// WHAT IT COSTS, MEASURED: a recipe build 21.5 -> 41.5 us, recipe + bake 57.2 -> 73.7 us, which is
/// 2.14% -> 2.76% of a 128-sample block at 48 kHz. Paid only where warp is on.
/// SPELLED AS THE LITERAL AND PINNED TO `kFlagshipSlots` BY A static_assert, because that constant is
/// declared 500 lines below this one and moving either would be a bigger edit than this is worth. The
/// assert is what stops the two drifting — the number's whole meaning is "the bake's grid".
inline constexpr int kFlagshipWarpedNormaliseSamples = 256;

inline constexpr int kFlagshipNormalisePictureSamples = 32;

/// Below this peak a cycle is left unscaled. See the note where it is used: dividing by nothing turns
/// the quietest ground in the engine into full scale.
inline constexpr double kFlagshipNormaliseFloor = 1.0e-6;

/// THE SMOOTH SOURCE'S TWO FIELDS, and the ladder they continue.
///
/// waypoint truncation shouldnt roughen the waveform... I specified we would renormalize the
/// time axis for the kept waypoints, and some kind of fix would address the wrap. I think you can
/// figure out how to get a sine waveform. That is this: keep K control points, renormalise them
/// evenly across the cycle, and read them through a WRAPPED CUBIC instead of the segment rule. At
/// K = 2 a wrapped Catmull-Rom through two points IS a sine to the measurement's resolution, which
/// is what the truncation arc was looking for and never found, because truncation was never the
/// mechanism — the interpolant was.
///
/// TWO KNOBS, NOT ONE, because they buy different things: K sets BRIGHTNESS (measured centroid
/// 0.0032 at K=2 rising to 0.1162 at K=8) and the interpolant sets ROUGHNESS (at K=3, a cubic reads
/// 0.0200/0.0036 against the segment rule's 0.0891/0.0475). One dial from "today's V5" to "a sine"
/// would have tied them together and thrown away three of the four corners.
///
/// SMOOTHNESS IS THE GATE AND POINT COUNT RIDES INSIDE IT. `pointCount` is read only where
/// smoothness is nonzero, so the identity corner grows exactly ONE condition rather than two — and
/// K stays free to vary independently within the feature's own territory, which is what makes the
/// two knobs separable at no cost to the anchor.
///
/// THE RATES CONTINUE THE QUARTER-FRACTION LADDER the operator tier and the species field are on —
/// φ^-2.25 quantize, φ^-2.75 sync, φ^-3.25 feedback, φ^-3.75 species — so these land at φ^-4.25 and
/// φ^-4.75, periods around 7900 and 10200 zones. No rate coincides with an integer rung of the
/// orientation ladder and every pairwise ratio is irrational, so nothing here ever realigns with
/// anything already spinning. Derived by division rather than transcribed, for the reason the
/// operator rates give: φ-to-four-places over φ-to-four-places is not φ.
///
/// STEPPED OFF THE FEEDBACK RATE RATHER THAN OFF SPECIES, which is the rung actually between them:
/// kVoxSpeciesRate lives in vox_bake.h, and vox_bake.h includes THIS header to read the operator
/// rates, so naming it here would close the include cycle the VoxCycle forward declaration exists to
/// keep open. φ^1.0 and φ^1.5 below the feedback rate are the same two numbers by a different route.
inline constexpr float kFlagshipSmoothRate = kFlagshipFeedbackRate / owlet::dsp::phi::kPhi100;
inline constexpr float kFlagshipPointRate = kFlagshipFeedbackRate / owlet::dsp::phi::kPhi150;

/// THE FM TRIPLE'S RATES, continuing the same ladder three rungs further down: φ^-5.25, φ^-5.75 and
/// φ^-6.25 against the point field's φ^-4.75. Longest periods in the engine, which is what §6 of the
/// FM doc asked for — ratio and denominator are the LONG-RANGE fields, so that a hand moving through
/// FM country hears the interval drift rather than shuffle.
///
/// Quarter-fractions again, so none coincides with an integer rung of the orientation ladder
/// (φ^-1…φ^-7), and the pairwise ratios within the triple are φ^0.5 — irrational, so the three never
/// realign with each other, with the operator fields above them, or with the blue shift.
inline constexpr float kFlagshipFmRatioRate = kFlagshipPointRate / owlet::dsp::phi::kPhi050;
inline constexpr float kFlagshipFmDenomRate = kFlagshipPointRate / owlet::dsp::phi::kPhi100;
inline constexpr float kFlagshipFmWidthRate = kFlagshipPointRate / owlet::dsp::phi::kPhi150;

/// THE THREE FM FIELDS, AT DUTY 1.0 — never zero, and for the point field's reason rather than by
/// oversight. A duty carves out territory where a field is OFF, and these are already gated: nothing
/// reads them unless a sound's FM Depth knob is nonzero, and that knob defaults to zero. Giving them
/// an off-region would only mean "FM country where the ratio is pinned" for no reason anyone could
/// hear as a place.
///
/// The offsets are clear of the operator bank's 0.150/0.480/0.820 and of the blue-shift pair's
/// 0.230/0.660, by the house rule: no two fields crest in the same neighbourhood.
inline constexpr owlet::dsp::phi::PhiTriConfig kFlagshipFmRatioField { kFlagshipFmRatioRate, 1.0f,
	                                                                    0.055f, false };
inline constexpr owlet::dsp::phi::PhiTriConfig kFlagshipFmDenomField { kFlagshipFmDenomRate, 1.0f,
	                                                                    0.360f, false };
inline constexpr owlet::dsp::phi::PhiTriConfig kFlagshipFmWidthField { kFlagshipFmWidthRate, 1.0f,
	                                                                    0.930f, false };

/// WHAT THE TWO INTERVAL FIELDS SPAN. The modulator runs at `carrier x ratio / denominator`, and both
/// ends are held at or above 1 BY CONSTRUCTION — a ratio of zero is a modulator at DC and a
/// denominator of zero is a division. Neither is reachable, which is the point of mapping through a
/// range rather than using the field directly.
///
/// 1..4 over 1..3 covers the set §2 measured — 1, 2, 1/2, 1/3, 2/3, 4/3 all live inside it — and both
/// ends are CONTINUOUS. §2 measured the discontinuity a ratio JUMP costs (2/3 steps 0.674 of full
/// scale in one sample at 48 kHz), and §9's answer is that a field is smooth by construction; a
/// denominator quantised to integers would have put that jump straight back.
inline constexpr float kFlagshipFmRatioMin = 1.0f;
inline constexpr float kFlagshipFmRatioMax = 4.0f;
inline constexpr float kFlagshipFmDenomMin = 1.0f;
inline constexpr float kFlagshipFmDenomMax = 3.0f;

/// THE DEEPEST THE DEPTH KNOB CAN PUSH, in CYCLES of carrier phase at knob 1.0 — the units §5's alias
/// rule is written in.
///
/// ONE CYCLE, MEASURED, not the 2.0 that §5's sweep reached. Two cycles is 12.6 RADIANS of index,
/// which is far past where FM is progressive: the Bessel terms oscillate and brightness stops
/// tracking the knob. Measured as the number of times brightness FALLS as the knob rises, across the
/// travel in eighths — max index 2.0 drops once, and 1.0 drops not at all. Below 1.0 the readings go
/// noisy again for the opposite reason, the changes being small enough to sit in the measurement's
/// own scatter. So 1.0 is the widest range over which this control still reads as one direction.
inline constexpr float kFlagshipFmMaxIndexCycles = 1.0f;

/// THE ALIAS CEILING, from §5: keep `fc·(1 + ratio·(2π·I + 1))` under about 70 kHz and the worst
/// alias sits below −80 dB; past ~96 kHz — the INTERNAL Nyquist, which is the binding constraint
/// rather than 24 kHz — it collapses within an octave of index. Enforced per voice rather than here,
/// because it needs the note's own fundamental; see fmIndexCyclesFor.
inline constexpr float kFlagshipFmAliasCeilingHz = 70000.0f;

/// THE RIGHT EAR'S MODULATOR POLARITY for a stereo width — §4's partial flip, and the one spelling of
/// it. (1 - 2w) runs +1 to -1 as w runs 0 to 1: at zero both ears are modulated identically and a
/// mono fold-down is bit-exact, at one the right ear is modulated by -I and every ODD sideband lands
/// 100% in the side channel while the carrier and every EVEN sideband land 100% in the mid (verified
/// to -200 dB).
///
/// IT IS NOT A LINEAR STEREO WIDTH IN THE EAR, and that is worth knowing before tuning the field: at
/// a large index a SMALL polarity difference already decorrelates the two ears almost completely,
/// because phase modulation is nonlinear in the modulator's amplitude. MEASURED at depth 1.0 and
/// w = 0.093 — a mid/side energy ratio of 0.976, which is very nearly a fully spread image from a
/// tenth of the control's travel. The useful range of the field is compressed toward zero.
/// HOW FAR FM PUSHES A READ PHASE, in cycles — the whole of the FM operation, and the ONE spelling of
/// it. The voice calls it per sample with its own accumulator; the terrain calls it with a nominal
/// one, so the picture and the sound cannot disagree about what the operator does.
///
/// It is PHASE modulation, which is what the voice's seam already did: `evalPhase +=
/// jitterOffsetCycles(...)` has been phase modulation since jitter existed.
float fmOffsetCycles(float indexCycles, double modulatorPhase, double polarity);

inline constexpr double fmRightPolarityFor(float stereoWidth) {
	return 1.0 - 2.0 * static_cast<double>(stereoWidth);
}

/// THE SMOOTHNESS FIELD. Duty 0.20 — the smallest territory of any field here except feedback's
/// post-threshold sliver, because this is the most transformative of them: it does not modify the
/// cycle, it replaces how the cycle is CONSTRUCTED. A duty that made smooth country ordinary would
/// make the segment rule the special case.
///
/// THE OFFSET IS NOT FITTED, AND THAT IS A DELIBERATE REFUSAL WORTH RECORDING. The identity corner
/// is where all the operator fields read zero, and a fourth field shrinks it wherever the two
/// overlap — so an offset can be CHOSEN to make smooth country avoid the corner, and measured over
/// the reachable window (home −8..+11) a band of offsets near 0.99 of a period retains 100% of it.
/// That was rejected on musical grounds: the corner is the CALM ground, a sine is the calmest sound
/// this engine can make, and buying the anchor by guaranteeing the smoothest waveform is never
/// available in the quietest place is paying in the wrong currency. So the offset follows the house
/// rule the other three follow — a peak clear of the others and clear of the origin an audition
/// starts from — and the corner pays what it pays. See the measurement beside `smoothness` in
/// FlagshipRecipe for the figure.
inline constexpr owlet::dsp::phi::PhiTriConfig kFlagshipSmoothField { kFlagshipSmoothRate, 0.20f,
	                                                                   0.40f / kFlagshipSmoothRate,
	                                                                   false };

/// THE POINT-COUNT FIELD, at duty 1.0 — never zero, and unlike every other field here that is
/// correct rather than careless. A duty carves out territory where a field is OFF, and this one is
/// already gated: it is read only where smoothness is nonzero, so it needs no off-region of its own,
/// and giving it one would only mean "smooth country where K is pinned at 8" for no reason anyone
/// could hear as a place. Inside smooth country K sweeps its whole range, which is what makes the
/// two knobs genuinely separable.
inline constexpr owlet::dsp::phi::PhiTriConfig kFlagshipPointField { kFlagshipPointRate, 1.0f,
	                                                                  0.10f / kFlagshipPointRate,
	                                                                  false };

/// The operator depths at one position — the three fields, read at the same coordinate.
struct FlagshipFields {
	float quantize = 0.0f; ///< q ∈ [0,1]: 0 leaves the cycle untouched, 1 is the full staircase.
	float sync = 0.0f;     ///< s ∈ [0,1]: sync ratio is 1 + 2s.
	float feedback = 0.0f; ///< k ∈ [0,1] after the threshold; depth is k · kFlagshipFeedbackMax.
	/// How much of the source is the smooth interpolant rather than the segment rule, 0..1.
	/// EXACTLY ZERO outside its duty, and that zero is an identity: see flagshipSourceAt.
	float smoothness = 0.0f;
	/// How many control points survive, continuous in [2, 8]. Meaningless while smoothness is zero.
	float pointCount = static_cast<float>(kFlagshipMaxSmoothPoints);

	/// THE FM TRIPLE — the interval the modulator runs at, and how much of it is a stereo flip. All
	/// three are terrain rather than controls (FM doc §9's settled gate: "every FM quantity is a
	/// terrain field, and one defeat knob scales the whole contribution"), so they cost no parameter
	/// ids and cannot be automated. Meaningless while a sound's FM Depth is zero, which is its
	/// default — FM is off by construction and every existing patch is byte-identical.
	float fmRatio = kFlagshipFmRatioMin;       ///< carrier:modulator numerator, [1, 4]
	float fmDenominator = kFlagshipFmDenomMin; ///< the subharmonic divisor, [1, 3]
	/// 0 = both ears modulated alike, so a mono fold-down is bit-exact; 1 = the full polarity flip,
	/// which §4 verified puts every ODD sideband 100% in the side channel and the carrier and every
	/// EVEN sideband 100% in the mid, to −200 dB. In between the side contribution scales linearly,
	/// which is what makes mono degrade gracefully instead of going hollow (a binary flip costs
	/// −4.83 dB and DELETES the odd sidebands at index 0.35).
	float fmStereoWidth = 0.0f;
};

/// The fields at a coordinate. freqMult 1.0, as the blue-shift read and for the same reason: the
/// rate half of the slice belongs to a sound's travel, and a field over the space is not
/// travelling. The COORDINATE is the caller's business — per decision V7 it is the morph frame's
/// lead coordinate, and flagshipAuthorsAt is the one place that derives it.
FlagshipFields flagshipFieldsAt(double coordinate);

/// THE SMOOTHNESS A KNOB AND A FIELD MAKE TOGETHER — the per-sound override (doc §11), and the ONE
/// spelling of it. Both the sliced entry and the blend read it, and a second spelling is how the
/// card and the engine came to disagree about spread.
///
/// IT IS NOT A MULTIPLIER, and that is the decision taken after finding the knob dead. A multiplier can
/// only take away what the field already put there, and MEASURED, the smoothness field is alive over
/// exactly ONE interval of the first eight offset steps of home — 4.6501 to 6.2002 — and is exactly
/// 0.000000 at the default coordinate. So on the ground a new patch starts on, and on four fifths of
/// the ground anywhere, the knob was multiplying zero: a control that does nothing wherever you
/// happen to be standing, which reads as broken rather than as out of range.
///
/// SO HALFWAY IS NEUTRAL AND THE TOP HALF BIASES UP. Below the middle it attenuates the field toward
/// nothing, exactly as the multiplier did; above the middle it lifts the result toward full
/// smoothness whatever the field says, so smooth country becomes somewhere you can ASK for rather
/// than only somewhere you can find. Continuous and monotonic through the middle, where it returns
/// the field untouched — which is why the parameter's default moved to 0.5 rather than staying at
/// 1.0: neutral is what a default should mean, and a patch saved before this existed loads at
/// neutral and sounds the same.
///
///   knob 0.0 -> 0            knob 0.5 -> field           knob 1.0 -> 1
// THE FIELD MODIFIER CURVE AND ITS NEUTRAL NOW LIVE IN dsp/slice_build.h, which this header
// includes — moved there 2026-08-25 because buildMorphSliced took a Grit scale and slice_build.h
// cannot include this file (the include runs the other way). Everything that used to get
// kFieldNeutral and fieldWithOverride from here still does, transitively.

/// HOW MANY CONTROL POINTS THE OVERRIDE KNOB LEAVES, given the field's own count.
///
/// Feedback (having heard the biased knob): "could maybe handle it being a little stronger at maximum."
/// At the top of the travel fieldWithOverride already returns 1.0, which is the whole of the
/// smooth source — there is no more crossfade to give. What is left is WHICH smooth curve, and that
/// is K: the cycle keeps two points at the sine end and every point the morph table authored at the
/// other. So the top half of the knob also walks K down toward two, and knob 1.0 is the smoothest
/// waveform this engine can make — a sine — rather than merely the smoothest interpolation of a
/// wavetable that may still have eight corners in it.
///
/// NEUTRAL AND BELOW LEAVE K ALONE, so the bottom half is exactly the old multiplier's behaviour and
/// a patch at neutral is the field as authored, which is what makes 0.5 an honest default.
/// THE POINTS KNOB, applied to the point-count FIELD — and it cannot be fieldWithOverride, which is
/// the whole reason this function exists.
///
/// `fields.pointCount` IS NOT IN [0,1]. It is a COUNT, mapped by flagshipAuthorsAt onto
/// [kFlagshipMinSmoothPoints, kFlagshipMaxSmoothPoints] — [2, 8]. fieldWithOverride clamps its field
/// argument to [0,1] before doing anything, so handing it a count of 7.09 yields 1.0, which
/// smoothSourceAt then clamps back up to 2: every smooth waveform in the engine collapses to a sine.
///
/// CAUGHT BY THE SUITE RATHER THAN BY READING, and it is worth recording which cases caught it,
/// because they are not the ones aimed at this knob. The vox-voice SMOOTH hashes went red — a bake
/// identity check nobody wrote with a point-count knob in mind — alongside this file's own
/// composition case. A [0,1] curve applied to a [2,8] quantity is exactly the "two units, one
/// function" shape that looks right at the call site and is wrong by a factor of seven.
///
/// So the bias happens in the field's OWN normalised domain: recover the triangle the count was
/// mapped from, bias THAT, and map back through the same endpoints. Neutral is the identity, and the
/// ends mean what the label says — knob 0 keeps two points (the sine end), knob 1 keeps every point
/// the morph table authored.
inline float pointFieldWithOverride(float field, float knob) {
	const auto lo = static_cast<float>(kFlagshipMinSmoothPoints);
	const auto hi = static_cast<float>(kFlagshipMaxSmoothPoints);
	const float unit = (field - lo) / (hi - lo);
	return lo + (hi - lo) * fieldWithOverride(unit, knob);
}

inline float pointCountWithOverride(float field, float knob) {
	if (knob <= kFieldNeutral) {
		return field;
	}
	const float toward = (knob - kFieldNeutral) * 2.0f;
	const auto floorPoints = static_cast<float>(kFlagshipMinSmoothPoints);
	return field + (floorPoints - field) * (toward > 1.0f ? 1.0f : toward);
}

/// EVERYTHING ONE SOUND'S KNOBS SAY ABOUT ITS FIELDS, in one carrier.
///
/// WHY A STRUCT AND NOT SEVEN TRAILING FLOATS. This started as two — `smoothnessScale` and
/// `fmDepth` — appended to a signature that already took seven positional arguments, and 2026-08-25
/// added five more. Seven same-typed defaulted floats in a row is a call site where transposing two
/// of them compiles, runs, and sounds subtly wrong; it is also a signature that invites an eighth.
/// Naming them at the call site is what makes the transposition unrepresentable, which the playbook
/// prefers to a comment asking people to be careful.
///
/// The defaults are the identity: six at kFieldNeutral (each field exactly as the terrain authored
/// it) and fmDepth at zero (FM off, never touching a modulator accumulator). So `{}` is the engine
/// as it was before any of these knobs existed, which is what every single-sound path and every
/// fixture that does not care wants.
struct FieldScales {
	float smoothness{kFieldNeutral};
	float quantize{kFieldNeutral};
	float sync{kFieldNeutral};
	float feedback{kFieldNeutral};
	/// Composes with `smoothness` rather than replacing its K walk — see SoundCoord::pointScale and
	/// the plan doc §3 for why the order is what it is.
	float points{kFieldNeutral};
	/// NOT A FLAGSHIP FIELD: this one reaches the morph table's ampNoise in slice_build.cpp, so it is
	/// carried past the flagship layer rather than consumed in it.
	float grit{kFieldNeutral};
	/// An AMOUNT, not a bias — hence a different default and a different curve. Kept here because it
	/// is the seventh thing a caller says about one sound's fields and splitting it out would leave
	/// two carriers for one idea.
	float fmDepth{0.0f};
	/// WARP DEPTH — an AMOUNT, like fmDepth and unlike the six above it: deliberately a plain modifier
	/// knob, as in Serum, rather than a phi field. So there is no field behind it and nothing to bias: the knob IS the value, and 0 means no warp.
	///
	/// THAT CHOICE IS WHY THE DEFAULT IS 0 AND NOT kFieldNeutral. `flagship.h` records the cost of
	/// getting that wrong once already — moving these semantics without moving the defaults turned
	/// every unspecified sound fully smooth. A bias defaults to 0.5 because 0.5 means "as authored";
	/// an amount with no author defaults to 0 because 0 means "none".
	float warpDepth{0.0f};
};

/// Everything one sound contributes to the operator tier: its depths, and the quantize grid the
/// STAIR MACHINERY authors at its position.
///
/// The step count and riser slope are the stair engine's own triangles — kPhiStairCount and
/// kPhiStairSlope — read through this sound's stair frame exactly as buildStairSliced reads them
/// (residue-protected, like everything else the frame carries). So at any place, the flagship's
/// staircase has the step count and slope the stair engine would have there: "which oscillator"
/// genuinely dissolves into a place. The zone-family special cases stay with the stair engine —
/// the flagship has no family, so the base formulas apply everywhere.
struct FlagshipAuthors {
	FlagshipFields fields;
	float stepCount = 2.0f;  ///< Stair's own 2 + tri·14, continuous in [2, 16].
	float riserSlope = 0.0f; ///< Stair's own tri²·0.9: fraction of a step spent ramping.
	/// WHERE THE FIELDS ABOVE WERE READ — decision V7's lead coordinate, carried rather than
	/// re-derived. Any other field over the terrain belongs at this same number (vox's species is
	/// the first to join), and handing it back is cheaper and safer than making each caller rebuild
	/// the frame to ask.
	double lead = 0.0;
};

FlagshipAuthors flagshipAuthorsAt(const MorphBasis& morphFrame, const StairBasis& stairFrame,
                                  double home, double travelX, double travelY);

/// WHERE A FIELD OVER THE TERRAIN IS READ (decision V7): the morph frame's lead row, travel and all.
///
/// Named rather than spelled at each site, and named the moment a SECOND field wanted it. Vox's
/// species field is read at exactly this place — it is terrain like the operator depths are terrain,
/// and it must move under travel and turn with the plane for the same reasons V7 gives — so there
/// are now two callers, which is precisely one more than it takes for two spellings to agree to the
/// eye and part in the last bit. A species field read a hair away from the operator fields would
/// put the vowel's edge somewhere the map does not draw it, and nobody would ever see why.
inline double flagshipLeadCoordinate(const MorphBasis& morphFrame, double home, double travelX,
                                     double travelY) {
	return morphFrame.phaseFor(0, home, travelX, travelY);
}

/// The most steps the author formula can ask for — stair's own ceiling, stated once.
inline constexpr int kFlagshipMaxSteps = 16;

/// A flagship character, ready to evaluate: the blended morph recipe plus the blended operator
/// author values. AUTHOR VALUES BLEND BY WEIGHT BEFORE ANY ROUNDING (decision V10a) — the step count
/// a three-sound blend quantizes on is the weighted count, never a vote among rounded ones.
struct FlagshipRecipe {
	owlet::dsp::PhiMorphWavetable morph {};

	float quantize = 0.0f;
	float syncRatio = 1.0f;
	float feedback = 0.0f;
	float stepCount = 2.0f;
	float riserSlope = 0.0f;

	/// WARP — Serum's phase-domain family, ported 2026-08-28. See dsp/warp.h.
	///
	/// THE MODE IS ENGINE-WIDE AND THE DEPTH IS PER SOUND, and the split is forced rather than
	/// chosen. Operator depths BLEND — `Σ w·override(field_k)`, a few lines below — because the
	/// flagship renders ONE cycle from the blended recipe. **An enum cannot be blended.** A per-sound
	/// mode would have to be resolved by dominant weight, and would then STEP the moment dominance
	/// changed under a moving puck — a discontinuity in the middle of a crossfade, which is exactly
	/// what the blend exists to avoid. So the shape is one choice for the engine, and how much of it
	/// each sound takes is per sound and blends like everything else here.
	WarpMode warpMode = WarpMode::Off;
	/// The blended knob, 0..1 as the parameter stores it. `warpRead` reads it through
	/// `warpAmount`, which is what makes the bipolar modes rest at 0.5 and the rest at 0.
	float warpDepth = 0.0f;

	/// HOW MUCH OF THE SOURCE IS THE SMOOTH INTERPOLANT, 0..1 — the crossfade between the segment
	/// rule and a wrapped cubic through the truncated, renormalised control points.
	///
	/// WHAT IT COSTS THE IDENTITY CORNER, measured rather than estimated, because this is the price
	/// of the feature and it should not be learned after the fact. Over the param-reachable window
	/// (home = zone/1023 + offset + gamma, so −8..+11) the corner — all three operator depths exactly
	/// zero — is 5.58% of terrain. NOT the ~15% this arc quoted for a long time: that figure is what
	/// quantize's 0.85 duty leaves ON ITS OWN, and sync and feedback cut into it independently, so
	/// the three together leave about 0.15 × 0.50 × 0.90. Adding this field makes the anchor "all
	/// FOUR zero" and, at duty 0.20 with the offset chosen by the house rule rather than fitted to
	/// the corner, retains between 53% and 100% of it depending where the offset lands, median 89%.
	/// The anchor stays STRUCTURAL either way — it is still an exact-zero region, not a measured
	/// near-miss — the territory it stands on is simply rarer.
	float smoothness = 0.0f;

	/// How many control points survive, continuous in [2, 8]. Read only where smoothness is nonzero.
	/// FRACTIONAL ON PURPOSE: K is an integer and the waypoint-width trick the quantizer's stepCount
	/// uses does not transfer, because a control point appearing at zero width still moves its
	/// NEIGHBOURS' tangents in a cubic — so a whole-number K would pop as a sound drifts. The
	/// evaluator crossfades the floor and ceiling curves by the fraction instead; see smoothSourceAt.
	float pointCount = static_cast<float>(kFlagshipMaxSmoothPoints);

	/// THE FM TRIPLE AND THE DEPTH KNOB THAT GATES THEM (FM doc §9/§10).
	///
	/// The three fields are terrain — the interval the modulator runs at and how much of it is a
	/// stereo flip — and `fmDepth` is the one automatable quantity FM spends an id on, per sound,
	/// defaulting to zero. At zero the voice takes exactly the road it took before FM existed, which
	/// is the same identity guarantee the operator tier's zero corner already makes.
	///
	/// NOT AN INDEX IN CYCLES YET. The depth becomes one per VOICE, because §5's alias rule is
	/// written in terms of the note's own fundamental and the bank serves every note at once — see
	/// fmIndexCyclesFor.
	float fmRatio = kFlagshipFmRatioMin;
	float fmDenominator = kFlagshipFmDenomMin;
	float fmStereoWidth = 0.0f;
	float fmDepth = 0.0f;

	/// THE MODULATOR'S FREQUENCY RATIO against the carrier — the two interval fields as the one
	/// number the voice's accumulator needs. Written here rather than at the call site because the
	/// denominator's floor is what makes it safe, and a second spelling could forget that.
	float fmFrequencyRatio() const {
		return fmRatio / (fmDenominator < kFlagshipFmDenomMin ? kFlagshipFmDenomMin : fmDenominator);
	}

	/// HOW DEEP THIS RECIPE MAY PUSH A NOTE AT `fundamentalHz`, in cycles of carrier phase, with §5's
	/// alias rule applied.
	///
	/// The rule is `fc·(1 + ratio·(2π·I + 1)) <= 70 kHz`, solved for I. It is enforced per voice and
	/// not in the recipe because it needs the NOTE, and the bank builds one recipe for every note at
	/// once — the same reason vox bakes per voice. Below the ceiling the knob has its whole travel;
	/// above it the index is pulled back rather than the note being refused, so a hand sweeping up
	/// the keyboard hears FM thin out instead of hearing it alias.
	float fmIndexCyclesFor(double fundamentalHz) const {
		const float wanted = fmDepth * kFlagshipFmMaxIndexCycles;
		if (wanted <= 0.0f || fundamentalHz <= 0.0) {
			return 0.0f;
		}
		const double ratio = static_cast<double>(fmFrequencyRatio());
		const double headroom = static_cast<double>(kFlagshipFmAliasCeilingHz) / fundamentalHz - 1.0;
		const double allowed = (headroom / ratio - 1.0) / (2.0 * std::numbers::pi);
		if (allowed <= 0.0) {
			return 0.0f;
		}
		return wanted < static_cast<float>(allowed) ? wanted : static_cast<float>(allowed);
	}

	/// WHAT MAKES THE HEADER'S PROMISE TRUE — "normalised so full scale is ±1.0", which it was not.
	///
	/// MEASURED before this existed, over a grid of positions and knob settings: 45 of 60 sampled
	/// cycles were outside a healthy band. The bare morph table is well behaved — DC within ±0.15,
	/// peak 0.58 to 1.00, always bipolar — and QUANTIZE is what breaks it. `held[i]` samples the chain
	/// at step CENTRES, so with few steps and a waveform whose centres cluster on one side the
	/// staircase inherits a large offset and can collapse in level: at home 1.0 (quantize 0.91) the
	/// cycle went from DC +0.020 to DC +0.402 and stopped crossing zero at all, and at home 7.0
	/// (quantize 0.98) the peak fell from 1.000 to 0.260. The smooth cubic can overshoot the other way,
	/// to 2.21.
	///
	/// A DC OFFSET IS NOT A SOUND. It costs headroom, it moves a speaker cone nowhere useful, and it
	/// makes every downstream stage — the filter's drive especially — operate around the wrong point.
	/// So it is removed, and what is left is scaled to reach ±1.
	///
	/// PER RECIPE, NOT PER SAMPLE, because a single evaluation cannot know its own cycle's peak. Both
	/// numbers are measured once in prepareFlagshipRecipe and applied as one subtract and one multiply
	/// in evalFlagship, which is why this costs the sample loop almost nothing.
	float dcOffset = 0.0f;
	float gain = 1.0f;

	/// False exactly when all three depths are zero AND no smoothness — the identity corner, where
	/// evalFlagship IS evalMorphTable and the voice stays on the continuous morph path (decision V8).
	/// SMOOTHNESS BELONGS IN THIS FLAG rather than in a fourth disjunct at every call site: `operated`
	/// already means "not the plain morph path", which is exactly what a replaced source makes true,
	/// so the bake, the grid road and PreparedCharacter::readsGrid all follow with nothing to change.
	bool operated = false;

	/// The control points the smooth source reads, in cycle order and already truncated to what this
	/// position authored — the endpoint's amplitude followed by each movable waypoint's, stopping
	/// where the builder's own waypoint ordering stops. Extracted once in prepareFlagshipRecipe
	/// rather than per read, because it is a walk with a branch in it and the source is read 256
	/// times per bake. Zero when smoothness is zero: nothing extracts what nothing will read.
	std::array<float, kFlagshipMaxSmoothPoints> smoothAmp {};
	int smoothPoints = 0;
	/// **A SHAPE MIX'S CONTROL POINTS (backlog KV).** The shapes have no waypoints, so on Basic Shapes the smooth source
	/// is the wrapped cubic through the mix itself sampled at K EVEN points — `smoothShapes[K][j]` is the mix at j / K —
	/// one row per count, because the count crossfades floor and ceiling and each must sample its own spacing. Live
	/// exactly when `smoothFromShapes`; prepared from the source in `prepareFlagshipRecipe`.
	bool smoothFromShapes = false;
	std::array<std::array<float, kFlagshipMaxSmoothPoints>, kFlagshipMaxSmoothPoints + 1> smoothShapes {};

	/// The quantize staircase, precomputed: how many steps this cycle quantizes on, and the value
	/// held on each — the sync+feedback chain sampled at each step's centre. Precomputed because
	/// every evaluation inside a step needs its held value and its neighbour's, and re-deriving
	/// them per sample would put the whole chain behind every riser.
	int steps = 0;
	std::array<float, kFlagshipMaxSteps> held {};
};

/// WHAT THE OPERATOR CHAIN READS — the one seam decision V22c lands on.
///
/// The chain does not read a morph table; it reads a SOURCE WAVEFORM, and until vox arrived the
/// morph table was the only thing that could author one. V22c put the vox crossfade here, BEFORE
/// the operators.
///
/// WHY THAT IS RIGHT, stated on its real justification rather than the one it is tempting to give.
/// The tempting one — "the operators are nonlinear, so the order matters" — is mostly FALSE and a
/// careful reader would take it apart. There is exactly ONE nonlinearity in the chain: the feedback
/// iteration, y ← C(p + depth·y), which evaluates the source at a phase depending on its own
/// output. Sync is a pure phase remap. The quantizer reads nothing whatever from the source — its
/// step index, its riser and its count are functions of phase and of authored values alone — and
/// the final combine is affine. So wherever feedback is zero, which is about 90% of the terrain BY
/// DESIGN, blending before the chain EQUALS crossfading two separately-operated outputs to float
/// rounding. Measured: 3.7e-08 at a quantize-0.65, feedback-0 position.
///
/// The three reasons that do hold:
///   - ONE DECODE (V21c), taken literally: the operators bake on the final cycle whatever authored
///     it, including a half-vox one. A seam here is a seam in one place rather than a rule about
///     which engine gets operated.
///   - THE FEEDBACK COUNTRY, where the two genuinely differ and blending after would be a
///     different sound — rare, but it is real terrain rather than a corner.
///   - COST AND MACHINERY: blending after needs a SECOND bake and a crossfade between two grids,
///     per voice, per block. Blending before needs neither.
///
/// A NULL `vox` IS THE ROAD THAT WAS ALWAYS THERE, and deliberately not "a blend at weight zero":
/// the read then IS evalMorphTable, the same call with the same argument, so every position with
/// no vowel in it — half the terrain, by the species duty — renders the bits it always rendered
/// with nothing new in the arithmetic to round differently.
struct FlagshipSource {
	const VoxCycle* vox = nullptr;
	/// How much of the cycle is vowel, 0..1. Meaningless while `vox` is null.
	float presence = 0.0f;
	/// **BASIC SHAPES' SOURCE (backlog KV)**: the normalised shape mix, read at `shapesMip`, IN PLACE OF the morph
	/// table. The operators then work on the shapes exactly as they work on V5's cycle. Null is the morph table.
	const ShapesWeights* shapes = nullptr;
	int shapesMip = 0;
};

/// Finish a recipe whose depth and author fields are already set: derive `operated`, the step
/// count's integer ceiling, and the held values. Split out from the builders so a test can force a
/// depth — the known-bad controls the plan requires — and still evaluate through the real path.
///
/// THE SOURCE HAS TO BE HERE AS WELL AS AT EVALUATION, which is easy to miss: `held` is the
/// sync+feedback chain sampled at each step's centre, so a recipe prepared against the morph table
/// alone and then evaluated with a vowel in it would quantize onto a staircase built from a
/// waveform that is not the one being played.
void prepareFlagshipRecipe(FlagshipRecipe& r, const FlagshipSource& source = {},
                           int normaliseSamples = kFlagshipNormaliseSamples);

/// The flagship character of one sound at a home and a travel — the single-sound entry the terrain
/// THE WARP MODE IS A PARAMETER AND NOT A `FieldScales` MEMBER, deliberately. That bundle is
/// "everything ONE SOUND says about its fields", and the mode is engine-wide — putting it there
/// would be the second copy of a fact that has exactly one home, and would invite a caller to set it
/// per sound where it cannot mean anything. Trailing and defaulted, so every existing caller is
/// unchanged and reads as unwarped, which is what they are.
///
/// draws through, exactly parallel to buildMorphSliced. The frames are the sound's own, derived
/// where the caller derives everything else about it; the authors are read through them, so this
/// and the blend entry below produce bit-identical operator values for the same sound — which is
/// the position-purity claim, and the suite holds it.
FlagshipRecipe buildFlagshipSliced(uint16_t zone, float phaseOffset, double travelX, double travelY,
                                   const MorphBasis& morphFrame, const StairBasis& stairFrame,
                                   double gammaBase, FieldScales scales = {},
                                   int normaliseSamples = kFlagshipNormaliseSamples,
                                   WarpMode warpMode = WarpMode::Off, const PhaseWarp* warp = nullptr,
                                   bool operatorFieldsOff = false);

/// The flagship character a whole Character describes — the entry the bank and the wave display
/// share, so what is drawn and what is played cannot be two readings of the same patch. The morph
/// half is buildMorphFor exactly; the operator authors are evaluated per sound at each sound's own
/// coordinate and blended by weight — Σ w·field(coordinate), never field(Σ w·coordinate), the
/// house rule blueShiftInto states the reasons for.
FlagshipRecipe buildFlagshipFor(const Character& c);

/// Evaluate the operated cycle at `phase01`, normalised so full scale is ±1.0.
///
/// THE ONE DECODE. The voice's baked grid, the wave display's curve and the terrain's feature
/// cycles are all this function — the grid is it sampled at the slot phases, the pictures are it
/// sampled at theirs — so the picture and the sound cannot disagree about what an operator did.
/// Unoperated recipes are evalMorphTable exactly, to the bit.
///
/// The operators apply in order sync → feedback → quantize (decision V9): feedback folds the SYNCED
/// cycle, so feedback peaks interact with sync ridges instead of being resampled away, and the
/// final shape is the staircase — which is what keeps the q=1 endpoint testable as piecewise-flat.
float evalFlagship(const FlagshipRecipe& r, double phase01, const FlagshipSource& source = {});

/// THE NORMALISATION APPLIED, and the only spelling of it — evalFlagship's last act, exposed so that
/// anything comparing against the recipe's RAW numbers can compare like with like.
///
/// Two cases needed it the moment normalisation landed, and both are claims worth keeping rather than
/// artefacts: "the identity corner is Morph to the bit" reads evalMorphTable directly, and "q = 1 is
/// piecewise-flat" reads `held[]` directly. Neither claim changed — the corner is still the morph
/// path and the staircase is still flat — but the left side of each comparison now carries a gain and
/// an offset the right side did not, and a case that patched around that by widening a tolerance
/// would have stopped testing what it says it tests.
float flagshipNormalised(const FlagshipRecipe& r, double raw);

/// The baked grid the voice plays: the operated cycle on 256 slots plus two binomial mips, the
/// stair render idiom at twice stair's resolution (morph is the brighter engine; 128 harmonics
/// against stair's 64). The last entry repeats the first, exactly as StairTable's grids carry
/// their guard, so the sample loop's idx+1 needs no wrap test.
inline constexpr int kFlagshipSlots = 256;
static_assert(kFlagshipWarpedNormaliseSamples == kFlagshipSlots,
              "a warped recipe's probe must sample the bake's own grid — that is what makes its peak "
              "exact rather than merely closer");

struct FlagshipTable {
	/// False for an identity-corner build: the voice then never reads these grids and renders the
	/// continuous morph table instead. A FIXED-SIZE MEMBER, never heap (decision V10b) — this sits
	/// inside PreparedCharacter on the audio thread.
	bool operated = false;
	float slots[kFlagshipSlots + 1] {};
	float mip1[kFlagshipSlots + 1] {};
	float mip2[kFlagshipSlots + 1] {};

	/// MEMBER-WISE, so a test asserting bake identity compares the bake and not the three padding
	/// bytes after `operated` — which memcmp reads, and whose value is indeterminate on every
	/// compiler (equal by accident on g++ 14/x86-64, different on Apple clang/arm64, where the
	/// same shape in `AutomodDerived` was the one red of the first green macOS build, 2026-09-07).
	/// Differs from a bit comparison only at the sign of zero and at NaN, where a NaN slot FAILS
	/// here and would have passed memcmp.
	friend bool operator==(const FlagshipTable&, const FlagshipTable&) = default;
};

struct FlagshipSlotSmooth; // below: the smooth source a bank reads once and its voices share

/// Bake `r` for the voice. An unoperated recipe just lowers the flag — the grids are not read.
void bakeFlagshipTable(const FlagshipRecipe& r, FlagshipTable& out);

/// ONE MIP, BAKED ON ITS OWN — what a caller that already knows which level it will play needs.
///
/// The bank bakes all three because it is shared: sixteen voices at sixteen pitches read one table,
/// so it cannot know which level any of them wants. A VOICE knows — stairMipForFrequency has
/// already settled it from that voice's own note — and a voice that has to bake for itself (the
/// vox road, where the cycle depends on the note) would be paying for two grids nobody reads. So
/// the base is evaluated once and the binomial pass applied `mipLevel` times, which is the same
/// arithmetic in the same order bakeFlagshipTable performs; the two agree to the bit, and a case
/// holds them to it.
///
/// `out` is 257 entries, wrap guard included. Levels outside 0..2 clamp, as flagshipMipSlots does.
void bakeFlagshipGridAtMip(const FlagshipRecipe& r, const FlagshipSource& source, int mipLevel,
                           float* out);

/// ALL THREE RUNGS IN ONE PASS OVER THE CHAIN — the base evaluated ONCE, then the binomial applied
/// successively, which is exactly what bakeFlagshipTable does and produces bit-identical grids.
///
/// IT EXISTS BECAUSE CALLING bakeFlagshipGridAtMip THREE TIMES COSTS THREE TIMES THE EVALUATIONS
/// for the same answer: each call rebuilds the base from scratch before filtering it, so a
/// three-rung bake ran the operator chain 768 times instead of 256. MEASURED on a sixteen-voice
/// drag through vowel country: 41.8% of a core before, and the chain evaluation is nearly all of
/// it. The two binomial passes are about two thousand flops against tens of thousands for the base,
/// so the ladder is very nearly the price of one rung.
///
/// Each output is 257 entries, wrap guard included.
void bakeFlagshipLadder(const FlagshipRecipe& r, const FlagshipSource& source, float* base,
                        float* mip1, float* mip2);

/// `prepareFlagshipRecipe(r, source)` THEN `bakeFlagshipLadder(r, source, ...)`, to the bit, evaluating the chain
/// once per slot rather than once per slot AND once per normalisation probe (backlog LZ, 2026-10-10).
///
/// The pair evaluated the cycle twice at every probe phase: the probe measured the raw cycle at i / 128 (i / 256
/// warped), and the ladder evaluated it again at the same phases, normalised. Every probe phase is a slot phase, so
/// this evaluates the 256 raw slots once, measures the probe off them and normalises them into the ladder. It is the
/// vowel bake's road, where every voice bakes at its own note on every rebuild: MEASURED, the bake was over half of a
/// moving 8-sound V5 patch at sixteen voices; COUNTED, the probe was 128 of each bake's 384 evaluations (256 of 512
/// warped). `test_flagship` holds it to the pair across recipes, and `test_synth`'s LZ case holds a moving
/// constellation's render to its pre-change hash. Only the audio probe (the default `normaliseSamples`) has this
/// road; the pictures' coarser probes stay on the pair.
void prepareAndBakeFlagshipLadder(FlagshipRecipe& r, const FlagshipSource& source, float* base, float* mip1,
                                  float* mip2, const FlagshipSlotSmooth* shared = nullptr);

/// **THE SMOOTH SOURCE AT EVERY SLOT, READ ONCE PER BUILD AND SHARED BY EVERY VOICE THAT BAKES IT** (backlog PB,
/// 2026-10-29). The vowel bake is per voice because the vowel's cycle depends on the note, but smoothness is applied
/// LAST in the chain and reads only the recipe's control points (`smoothSourceAt`), never the source: the same 256
/// numbers for every note. MEASURED in Book of Dreams (16 notes, its LFO moving the character every build): about a
/// sixth of every vowel bake. So the bank reads them once, where it builds the table, and a voice baking that table
/// reads them from it.
///
/// IDENTIFIED BY WHAT HOLDS IT, NEVER BY AN ADDRESS: it is a member of the PreparedCharacter whose recipe it was read
/// from, so it rides that table's slot and build serial and a voice reads it off the very table it is baking (the
/// ABA rule). `live` false is "not read": an unoperated or unsmoothed recipe, or a build with no vowel, whose voices
/// never bake. A fixed-size member, never heap (decision V10b).
struct FlagshipSlotSmooth {
	bool live = false;
	std::array<double, kFlagshipSlots> at {};
};

/// Fill `out` from a PREPARED recipe: `smoothSourceAt(r, j / kFlagshipSlots)` for every slot, exactly the read
/// `evalFlagship` makes there, or `live` false where the chain would not read it.
void fillFlagshipSlotSmooth(const FlagshipRecipe& r, FlagshipSlotSmooth& out);

/// TEST ONLY. The wrapped Catmull-Rom the smooth source is built from (`smoothCurveAt`), so a case can hold its
/// division-free index to the modulo it replaced.
double flagshipSmoothCurveForTest(const float* amp, int n, double p);
static_assert(kFlagshipSlots % kFlagshipNormaliseSamples == 0,
              "the fused bake takes the audio probe off the slot grid, so the probe's phases must be slot phases");

/// The 257-entry grid for a mip level; anything outside 0..2 clamps into range. Which mip plays is
/// the same statement about the note stair makes — stairMipForFrequency decides for both.
const float* flagshipMipSlots(const FlagshipTable& table, int mipLevel);

/// Two slot loads and a lerp — evalStairGrid's contract on the flagship's wider grid.
float evalFlagshipGrid(const float* slots, double phase01);

//------------------------------------------------------------------------------------------------//
// BASIC SHAPES' OPERATOR TIER (backlog KV)                                                         //
//------------------------------------------------------------------------------------------------//
//
// Quantize, Sync, Feedback, Smoothness and Points on the shape mix: V5's operator chain with the mix as its source.
//
// THE TERRAIN'S OPERATOR FIELDS DO NOT ACT ON BASIC SHAPES; ONLY ITS KNOBS DO (to confirm). On V5 the knobs are
// biases on fields the terrain authors (neutral 0.5 = the field as authored), and the quantize field alone is live on
// 85% of the terrain, so taking the fields over would turn a default Basic Shapes patch into a staircase. Here each
// amount is `fieldWithOverride(0, knob)`: at the knob's neutral default it is 0 (the engine as it was, to the bit), and
// the top half lifts it to full; the bottom half has nothing to take away. The STRUCTURE the terrain authors (the step
// count, the riser, the point count) is kept, as it is on V5.
//
// THE OPERATED CYCLE IS BAKED ON V5'S GRID, at the shapes mip whose partials the grid holds, and the voice plays it
// down V5's grid road (its binomial mips, its band limit). The operators make corners at every step and sync reset,
// and no mip of the source can band-limit what they add afterwards; per-mip operated bakes would cost eleven bakes per
// build for a band limit the operators break anyway (to confirm). Warp alone stays on the band-limited ladder road.

/// THE SHAPES MIP THE GRID IS BAKED FROM: 128 harmonics, the most 256 slots hold.
inline constexpr int kShapesBakeMip = 3;
static_assert((kShapesTopHarmonics >> kShapesBakeMip) * 2 == kFlagshipSlots);

/// Whether the shape mix is OPERATED: any of the four depths live. Warp is NOT in it — warp alone keeps the ladder road.
[[nodiscard]] inline bool shapesOperated(const FlagshipRecipe& r) {
	return r.quantize > 0.0f || r.syncRatio > 1.0f || r.feedback > 0.0f || r.smoothness > 0.0f;
}

/// Re-prepare a Basic Shapes recipe against its mix (held steps, smooth points, normalisation), when operated; an
/// unoperated recipe is left exactly as it was built.
void prepareShapesRecipe(FlagshipRecipe& r, const ShapesWeights& weights, int mip,
                         int normaliseSamples = kFlagshipNormaliseSamples);

/// THE ONE DECODE of a Basic Shapes cycle: the operated chain when operated (a recipe `prepareShapesRecipe` prepared at
/// the same mip), else the ladder read through the warp — what the voice, the wave display and the terrain each read.
[[nodiscard]] float evalShapesCycle(const FlagshipRecipe& r, const ShapesWeights& weights, int mip, double phase01);

} // namespace feathers
