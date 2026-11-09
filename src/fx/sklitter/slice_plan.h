/*
 * Copyright © 2024-2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * --- Additional terms under GNU GPL version 3 section 7 ---
 * This file requires preservation of the above copyright notice and author attribution
 * in all copies or substantial portions of this file.
 */

#pragma once

#include "character/phi_triangle.hpp"
#include "character/util.hpp"
#include "device/zone_param.h"
#include "fx/sklitter/decisions.h"
#include "fx/sklitter/hash_random.h"
#include "fx/sklitter/macro_push.h"
#include "fx/sklitter/sklitter_types.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace feathers::fx::sklitter {

//--------------------------------------------------------------------------------------------------//
// WHAT HAPPENS TO ONE SLICE. THIS IS WHERE SKLITTER'S CHARACTER LIVES.                                //
//--------------------------------------------------------------------------------------------------//
//
// Six of the eight modes share ONE slice-selection algorithm -- the reference's `processStutter`
// falls Repeat, Time, Grain, Pattern and Pitch all through to Shuffle with flags -- so this is one
// function and not six. The modes differ in how many slices the rate buys, and in what they do with
// the plan afterwards.
//
// **PORTED FAITHFULLY RATHER THAN RE-EXPRESSED, and that is a deliberate exception.** The choice was to
// re-express Scatter's STRUCTURE; this is not structure. Every threshold, every phi frequency and
// every duty cycle here is a tuning decision somebody made by ear, and there is no way to re-derive
// one from first principles -- a "cleaner" 0.3 is simply a different sound. The q31 arithmetic is
// gone (our controls arrive as floats) and nothing else moved.
//
// `test_fx_sklitter_modes.cpp` pins the plan as VALUES. That is the instrument the whole
// re-expression rests on: which slice plays, reversed, at what length, is not recoverable from a
// rendered buffer, because the buffer carries every quantity at once.

/// **THE SEND OVERRIDE — ONE BIPOLAR CONTROL THAT REPLACES A PROBABILITY RATHER THAN BIASING IT.**
///
/// At full strength the override knob raises the probability as high as 1.0, or ties it to
/// zero. Backlog EW says why the macro cannot do this job: `macroNorm * scale * K` is ADDED to
/// a triangle and then clamped, so **what a push can REACH depends on where the triangle currently
/// sits** — at a phi value of 0.2 no push in this device's range arrives at certainty, and at 0.9 a
/// downward push cannot arrive at silence either. A performer asking for ALWAYS or NEVER is not asking
/// for a bigger bias; they are asking for the phi value to stop being consulted. So this OVERRIDES.
///
/// **A LERP TOWARDS AN ENDPOINT, WHICH IS WHAT MAKES ALL THREE PROMISES HOLD AT ONCE:**
///   - `k = 0` is the IDENTITY, and exactly so rather than nearly — `p + 0 * (1 - p)` is `p` for every
///     float, no epsilon anywhere. A knob at rest leaves phi exactly as it was.
///   - `k = +1` returns exactly `1.0f` and `k = -1` exactly `0.0f`, for every `p`.
///   - monotonic in `k`, and continuous across zero because both branches agree there.
///
/// **THE TWO ENDPOINT CLAIMS ARE MEASURED, NOT ARGUED.** `p + (1.0f - p)` is a float sum of a float
/// difference and "it obviously comes to one" is exactly the kind of claim this repo has been wrong
/// about. Checked EXHAUSTIVELY over all 1,065,353,217 floats in [0, 1]: `k = +1` gives 1.0f for every
/// one of them, `k = -1` gives 0.0f, and `k = 0` returns its argument bit for bit.
///
/// **`k` IS CLAMPED HERE AND NOWHERE ELSE.** It is a modulation destination, so it can arrive outside
/// its declared range; clamping at the one place that reads it means there is one statement of the
/// rule rather than one per caller. `p` is not clamped — `planFor` has already done it, and a second
/// clamp would be a second road to the same rule, free to disagree the day one is edited.
[[nodiscard]] inline float overriddenProbability(float p, float k) {
	const float amount = std::clamp(k, -1.0f, 1.0f);
	return amount >= 0.0f ? p + amount * (1.0f - p) : p * (1.0f + amount);
}

/// THE PLAN FOR ONE SLICE — the reference's `GrainParams`, field for field.
struct SlicePlan {
	int32_t sliceOffset = 0;  ///< added to the slice selection, [0, numSlices)
	float lengthMult = 1.0f;  ///< one of `kLengthMultLevels`
	bool shouldSkip = false;  ///< jump to a non-adjacent slice?
	int32_t skipTarget = 0;   ///< [0,15]; the caller scales it to the slice count
	bool useDry = false;      ///< pass the input instead of the grain
	/// **THE MACRO'S OWN VOTE FOR DRY, which is a second and independent road to it.** The reference:
	/// `thresholdInfluence = triangleSimpleUnipolar(macroConfigNorm * kPhi, 0.5f)` and then
	/// `macroWantsDry = (macroNorm * thresholdInfluence > 0.5f)` — so Macro Cfg GATES how much of the
	/// macro's travel reaches the decision. Neither name appeared anywhere in this port.
	bool macroWantsDry = false;

	/// **WHERE MACRO CFG ACTUALLY LANDED, RAW: its position, its lap and gamma, summed and neither
	/// clamped nor wrapped.** It is a PHASE — every term `macroPushOf` derives from it multiplies it by a
	/// different phi constant and wraps the product — so bounding it would make gamma saturate. Double,
	/// because the fraction has to survive to the wrap; see `planFor`'s note where it is summed.
	///
	/// **PUBLISHED FOR THE TESTS, AND SINCE TASK 6 IT IS NOT WHAT DROVE THE PLAN.** It is the sum of
	/// whatever config the CALLER handed in, while the push the device hands in is blended from the four
	/// perform waypoints (`perform.h`) — two different quantities that happened to agree at a centred
	/// puck while there was still a knob to agree with.
	///
	/// **AND SINCE 2026-09-19 THE DEVICE HANDS IN NO CONFIG AT ALL**, so for a plan SKLITTER drew this is
	/// gamma alone (`sklitter.cpp`'s `kNoKnobConfig`). It stays because the REFERENCE/PORT overloads below
	/// still feed it a real pair, and the parity cases read it; a case that reads this field off a device
	/// plan is asking about a knob that no longer exists.
	double macroConfigNorm = 0.0;

	bool shouldReverse = false;
	bool shouldPitchUp = false; ///< up an octave, by 2x decimation
	bool shouldDelay = false;
	/// **THE PROBABILITY `shouldDelay` WAS DRAWN AGAINST, AFTER THE PUSH AND AFTER THE OVERRIDE.**
	///
	/// **IT IS PUBLISHED BECAUSE THE BOOLEAN CANNOT CARRY THE CLAIM.** `shouldDelay` is a hash draw, so
	/// a test that reads it is measuring a coin and needs a population to say anything — and at both
	/// ENDS of `delayProb` the override is invisible by construction: force-up at p = 1 returns 1, which
	/// is what no override returns, and force-down at p = 0 returns 0 likewise. A case built on the
	/// extremes passes with the whole feature deleted. CLAUDE.md's rule is the general form of this
	/// ("assert the value"); the seam is five lines and there is no cleverness with draws that recovers
	/// one probability from a stream of booleans.
	float delayProb = 0.0f;
	/// **DOES THIS GRAIN GO DOWN THE GRAIN LANE?** Backlog EW's per-grain routing decision, drawn from
	/// `HashSeed::GrainLaneDecision` against `delayProb` — the same probability the echo send uses, so
	/// `Send Override` steers both, and a different draw, so the two are independent events. See that
	/// seed for why.
	///
	/// **IT IS ONE FIELD ON THE PLAN BECAUSE THE PLAN IS THE ONE THING BOTH ROADS ALREADY BUILD.**
	/// `planFor` is called by `advanceSlice` (a slice at a time, the looper modes) AND by
	/// `grainPlanFor` (per voice, per wrap, in Grain) — so a decision taken HERE is taken once, in one
	/// place, and both roads read it. The alternative was a per-slice draw and a per-grain draw kept in
	/// agreement by hand, which is the shape this device has paid for five times over
	/// (`Sklitter::blendAt`'s header lists them).
	bool shouldSendToLane = false;
	float filterFreq = 0.5f;
	uint8_t delaySendBits = 0;  ///< 0=off, 1=25%, 2=50%, 3=100%
	uint8_t delayRatioBits = 0; ///< feeds `computeDelayTimeRatio`
	float envShape = 0.5f;      ///< 0 percussive, 0.5 hanning, 1 reversed
	float envDepth = 0.0f;      ///< 0 hard cut, 1 full envelope
	float panAmount = 0.0f;
	float stereoWidth = 0.0f; ///< [-1,1]

	float gateRatio = 1.0f;
	int32_t subdivisions = 1; ///< ratchet: play the slice start N times
	int32_t repeatSlices = 1; ///< hold this grain for N slices
	int32_t grainLength = 1;  ///< combine N consecutive slices into one grain

	friend bool operator==(const SlicePlan&, const SlicePlan&) = default;
};

/// THE SECRET-KNOB OFFSETS. The reference's `ScatterPhaseOffsets`, less its precomputed macro scales,
/// which are `MacroPush` terms here.
struct PhaseOffsets {
	float zoneA = 0.0f;
	float zoneB = 0.0f;
	/// THE MACRO CONFIG COORDINATE'S LAP — the reference's `macroConfigPhaseOffset`, which is now the
	/// whole part of the control rather than a control of its own. Consumed at `* 0.1f`, as there.
	float macroConfig = 0.0f;
	float gamma = 0.0f;

	// **THE THREE THRESHOLD SCALES ARE GONE FROM HERE, AND THAT IS THE FIX.** They were read by
	// `planFor` and WRITTEN BY NOBODY — `Sklitter::advanceSlice` default-constructs this struct and
	// assigns only the four offsets and the bar — so the macro's influence on reverse, pitch and delay
	// probability was inert, and the `PlanCache` key omitted them into the bargain (latent only
	// because they were always zero). Found by the EH sweep, the same "declared and never written"
	// shape as `barIndex`, which sat here too.
	//
	// **NOT CARRIED HERE EITHER.** The reference precomputes them in the stutterer because that is where
	// its `macroConfigNorm` lives. Ours are three terms of `MacroPush`, built by `macroPushOf` from the
	// macro and its coordinate and handed to `planFor` whole, so no field of this struct has to be kept
	// in step with a value it is derived from — which is exactly how these came to be zero.

	int32_t barIndex = 0; ///< 0-3, for multi-bar evolution
	/// Whether the macro may push the delay probability — false in Repeat. See `offsetsFor`.
	bool macroPushesDelay = true;
	/// **THE SEND OVERRIDE, [-1, +1], APPLIED AFTER THE PUSH AND AFTER THE CLAMP.** `Param::SendOverride`.
	///
	/// **IT RIDES HERE RATHER THAN IN `MacroPush` BECAUSE IT IS NOT A PUSH.** Every field of `MacroPush`
	/// is a term that gets ADDED and then clamped, and blending several of them is meaningful — that is
	/// what the perform circle does. This one REPLACES, so it has no sum to join: two overrides blended
	/// half and half is not an override at all. `macroPushesDelay` is its neighbour for the same reason —
	/// a per-plan fact about the delay decision that is not itself a phase.
	///
	/// DEFAULT ZERO, which is the identity — see `overriddenProbability`. Nothing that does not ask for
	/// the override gets one, the parity road included.
	float sendOverride = 0.0f;
};

/// **THE DEAD-ZONE CACHE, AND IT IS A MEMBER NOW RATHER THAN A FUNCTION-LOCAL STATIC.**
///
/// The reference caches this in `static` locals inside `computeGrainParams`. On the hardware that is
/// sound: there is ONE Stutterer, so there is one caller. In a plugin there are as many SKLITTERs as
/// the user drags in, each on its own track and its own audio thread, all writing a twenty-two field
/// struct through the same pointer. The VALUES would agree -- the cache key is the whole input and
/// the function is pure -- so this is not a wrong-answer bug; it is a torn-write data race, which is
/// undefined behaviour and exactly what the sanitiser gate exists to find.
/// **AND THE KEY WAS SHORT BY THREE, WHICH IS A FIFTH DEFECT IN THE REFERENCE.** Its condition reads
/// `zoneAParam == cachedZoneA && zoneBParam == cachedZoneB && macroConfigParam == cachedMacroConfig
/// && macroParam == cachedMacro && ofs.gamma == cachedGamma && ofs.barIndex == cachedBarIndex` — the
/// four params, gamma and the bar. The three PHASE OFFSETS are not in it, and `phRawA`, `phRawB` and
/// `macroConfigOffset` all read them, so a plan computed at one offset is handed back at another.
///
/// **IT IS RARE ON THE HARDWARE AND ORDINARY HERE.** There the offsets move only by a secret
/// push-and-twist, so the stale answer needs a deliberate gesture inside both dead zones. In Feathers
/// they are the WHOLE PART of the three coordinates — turning Zone A from 0.5 to 1.5 leaves the
/// position at 0.5 and moves the lap, which under the reference's key is a control that does nothing.
///
/// **OURS KEYS ON THE MACRO'S PUSH, NOT THE MACRO.** The plan reads the macro only through its
/// `MacroPush`, so the push is the whole of that input — and once pushes are blended from several
/// waypoints there is no single macro to key on. **`macroConfig`, `gamma` and the config's lap stay in
/// the key ONLY because the plan publishes their sum in `macroConfigNorm`** — the push already carries
/// their effect on every decision — so a later tidy-up must not delete them as unread: a hit would
/// then hand back a plan whose published phase belongs to another config. (`gamma` is also read by the
/// zone phases and the timbral bank, so it would stay regardless.) An invalid entry is refused by
/// `valid`, so the default push is never mistaken for a hit.
struct PlanCache {
	SlicePlan plan {};
	float zoneA = -1.0f;
	float zoneB = -1.0f;
	float macroConfig = -1.0f;
	MacroPush push {};
	float gamma = -1.0f;
	float zoneALap = -1.0f;
	float zoneBLap = -1.0f;
	float macroConfigLap = -1.0f;
	int32_t barIndex = -1;
	bool macroPushesDelay = true;
	/// **IN THE KEY BECAUSE IT CHANGES THE ANSWER.** The dead-zone cache hands back a whole `SlicePlan`,
	/// `shouldDelay` and `delayProb` included, and the override moves both — so a plan drawn at one
	/// setting handed back at another is the reference's own short-key defect (see the note above)
	/// reappearing in the field added after it was fixed.
	float sendOverride = 0.0f;
	bool valid = false;
};

/// HOW MANY SLICES THE RATE BUYS, which is what Rate MEANS in a looper mode.
///
/// **IT IS A DIFFERENT QUESTION FROM BURST'S.** Burst's rate is a slice LENGTH -- a division of
/// the beat. Here it is a COUNT: how many pieces the captured bar is cut into. The reference reads
/// the same knob and maps it through a different ladder, and Pitch halves the result because a
/// pitched grain needs time to be heard as a pitch at all.
[[nodiscard]] inline int32_t sliceCountFor(Mode mode, float rate) {
	// The reference works in its own 0..50 UI units off a -64..+64 knob; ours is 0..1 over the same
	// span, so the thresholds are those divided by 50.
	const float ui = std::clamp(rate, 0.0f, 1.0f);
	int32_t slices = 32;
	if (ui < 4.0f / 50.0f) {
		slices = 1; // one bar
	}
	else if (ui < 9.0f / 50.0f) {
		slices = 2;
	}
	else if (ui < 16.0f / 50.0f) {
		slices = 4;
	}
	else if (ui < 22.0f / 50.0f) {
		slices = 8;
	}
	else if (ui < 28.0f / 50.0f) {
		slices = 16;
	}
	if (mode == Mode::Pitch && slices > 1) {
		slices /= 2;
	}
	return slices;
}

/// REPEAT'S SLICE LENGTH, which is a continuous curve rather than a ladder: a full bar down to a
/// ~6 ms floor, squared so the short end has the resolution.
[[nodiscard]] inline size_t repeatSliceLength(float rate, size_t playbackLength) {
	constexpr size_t kMinSlice = 256;
	const float normalized = std::clamp(1.0f - std::clamp(rate, 0.0f, 1.0f), 0.0f, 1.0f);
	const auto scaled = static_cast<size_t>(static_cast<double>(playbackLength) * normalized * normalized);
	return std::clamp(scaled, kMinSlice, playbackLength == 0 ? kMinSlice : playbackLength);
}

/// **CLASSIC'S RATE MAPPING — the longest division and how many halvings the control spans.** Here
/// rather than beside `resolveSliceFrames` so the readout (`rateDivisionFor`) and the engine read one
/// pair of numbers. `sklitter.cpp`'s "THE RATE MAPPING" note is where they are derived from the reference.
inline constexpr double kSlowestDivisionBeats = 1.0; ///< a quarter note
inline constexpr int kHalvings = 4;                  ///< 4th -> 8th -> 16th -> 32nd -> 64th

/// HOW MANY SLICES A LOOPER MODE CUTS `barFrames` INTO AT THIS RATE — Repeat by its curve, the rest by
/// the ladder. The resting picture (`publishReadings`) and the readout's fallback (`formatText`) both
/// call this, so the two cannot disagree; the press and `advanceSlice` still spell the same two lines
/// inline, left alone while another branch is in that code.
[[nodiscard]] inline int32_t loopSlicesFor(Mode mode, float rate, size_t barFrames) {
	if (mode == Mode::Repeat) {
		return static_cast<int32_t>(
		    std::max<size_t>(1, barFrames / std::max<size_t>(1, repeatSliceLength(rate, barFrames))));
	}
	return sliceCountFor(mode, rate);
}

/// **HOW MUCH OF THE CAPTURE ONE REPEAT READS, AT REST** — Repeat's face (`Reading::RepeatLength`). The
/// slot `scheduleFor` cuts for a one-slot grain: the bar over the count `loopSlicesFor` gives.
[[nodiscard]] inline size_t repeatReadFrames(float rate, size_t barFrames, int32_t numSlices) {
	(void)rate;
	return numSlices > 0 ? barFrames / static_cast<size_t>(numSlices) : barFrames;
}

/// **RATE AS A NOTE VALUE: THE N OF "1/N" — backlog GS** (rate doesnt show grain
/// subdivision). A whole note is `kBeatsPerBar` beats, so a length of `beats` is `1/(bar/beats)`.
///
/// **TWO QUESTIONS, ONE ANSWER SHAPE.** Burst asks for a LENGTH, continuous, from the
/// position PLAYING — `effectiveRatePosition()` during a quantized gesture, not the knob — so between
/// the five divisions N is not an integer. A looper mode asks for a COUNT, `slicesPerBar`, and its slice
/// is the bar over that count, so N is the count itself.
[[nodiscard]] inline double rateDivisionFor(Mode mode, double lengthPosition, int32_t slicesPerBar) {
	if (mode == Mode::Burst) {
		const double beats = kSlowestDivisionBeats * std::exp2(-kHalvings * std::clamp(lengthPosition, 0.0, 1.0));
		return static_cast<double>(kBeatsPerBar) / beats;
	}
	return static_cast<double>(std::max(slicesPerBar, 1));
}

// `Coordinate` and `splitCoordinate` live in `macro_push.h`, which the macro's push needs them for.

/// **THE OFFSETS A PLAN IS DRAWN WITH — ONE BUILDER, AND IT CARRIES THE MACRO'S PHASE PUSH, WHICH NO
/// CALLER DID.** The reference's caller, `stutterer.cpp`, before every `computeGrainParams`:
///
///     float macroZoneAPhase = macroNorm * staticTriangles.zoneAMacroInfluence * kMacroPhaseMax;  // 0.3
///     float macroZoneBPhase = macroNorm * staticTriangles.zoneBMacroInfluence * kMacroPhaseMax;
///     ScatterPhaseOffsets offsets{ zoneAPhaseOffset + macroZoneAPhase, zoneBPhaseOffset + macroZoneBPhase,
///                                  ..., isRepeat ? 0.0f : staticTriangles.delayScale, scatterBarIndex };
///
/// with the influences `triangleSimpleUnipolar(macroConfigNorm * kPhi050 / kPhi075, 0.5f)`. So the
/// Macro moves BOTH zones' phase — and any nonzero phase puts `computeGrainParams` into its full
/// evolution mode — which made it the reference's one-knob move across the whole plan. Ours only ever
/// nudged three probabilities. MEASURED by compiling the reference's `dsp/scatter.hpp` beside
/// `planFor` and diffing 153,664 plans over a zone x macro x bar x slice grid: with this push removed
/// from the reference's side the two are identical in every field; with it in, 71% of plans differ.
///
/// **THREE CALLERS BUILT THESE BY HAND** — the playing slice, the Grain voices and the published
/// picture — which is how a term goes missing from one of them. They all come here now.
///
/// **THE PUSH ARRIVES BUILT.** The two influences are `MacroPush::zoneAPush` and `zoneBPush` now,
/// computed by `macroPushOf` from the same position in the same product order, so a push blended from
/// several waypoints lands here exactly as one macro's did.
///
/// **`sendOverride` DEFAULTS TO THE IDENTITY, AND THAT IS WHAT KEEPS THE PARITY ROAD PARITY.** The
/// hardware has no such control, so a call that does not name one is asking for the reference's plan and
/// gets it to the bit — which is also why the twenty-one pinned values in `test_fx_sklitter_modes.cpp`
/// did not move when this argument appeared, checked rather than assumed.
///
/// **AND THE COUNT STAYS TWENTY-ONE THOUGH THE STRUCT IS NOW TWENTY-TWO FIELDS WIDE.** `delayProb` is
/// the new one and it is OURS: the reference computes the same quantity and keeps it in a local, so there
/// is no reference number for it to be pinned against. It is asserted directly instead, by the send
/// override's own cases.
[[nodiscard]] inline PhaseOffsets offsetsFor(const Coordinate& a, const Coordinate& b, const Coordinate& mc,
                                             const MacroPush& push, float gamma, int32_t barIndex,
                                             bool repeat, float sendOverride = 0.0f) {
	PhaseOffsets ofs;
	ofs.zoneA = a.lap + push.zoneAPush;
	ofs.zoneB = b.lap + push.zoneBPush;
	ofs.macroConfig = mc.lap;
	ofs.gamma = gamma;
	ofs.barIndex = barIndex;
	// *"No delay for Repeat"* — the macro's push on the delay probability is withheld there.
	ofs.macroPushesDelay = !repeat;
	// **AND THE OVERRIDE IS NOT WITHHELD THERE.** The line above is the reference's own *"no delay for
	// Repeat"*, and it is about the MACRO — Repeat still draws `shouldDelay` and still sends. A knob
	// whose whole point is that the player can say ALWAYS would be lying if the one mode where the
	// macro is muzzled silently ignored it too.
	ofs.sendOverride = sendOverride;
	return ofs;
}

/// **THE REFERENCE/PORT ROAD: the push of ONE macro and its config, which is the shape the REFERENCE has.**
///
/// **DO NOT DELETE THIS AS A DEAD KNOBS-ONLY OVERLOAD — IT IS THE PARITY ROAD, AND THAT IS WHY IT READS
/// LIKE A LEFTOVER.** SKLITTER itself has had no macro knob since 2026-09-19 (`Param::Macro` and
/// `Param::MacroConfig` are deleted; the circle's centre is a zero push), so nothing in `sklitter.cpp`
/// calls this and nothing will. What calls it is `test_fx_sklitter_modes.cpp`, which pins twenty-one plan
/// values against the reference's `computeGrainParams` — and the reference takes ONE (macro, config) pair,
/// because its Stutterer has exactly the knob we removed. **Re-expressing those cases in terms of a
/// waypoint blend would make them a test of our arithmetic against itself**; taking the pair is what lets
/// a pinned number mean "the reference does this".
///
/// The same argument keeps `SlicePlan::macroConfigNorm`, which this road is the only live producer of a
/// non-trivial value for.
[[nodiscard]] inline PhaseOffsets offsetsFor(const Coordinate& a, const Coordinate& b, const Coordinate& mc,
                                             float macro, float gamma, int32_t barIndex, bool repeat,
                                             float sendOverride = 0.0f) {
	return offsetsFor(a, b, mc, macroPushOf(macro, mc, gamma), gamma, barIndex, repeat, sendOverride);
}

/// THE PLAN FOR SLICE `sliceIndex`. A faithful port of the reference's `computeGrainParams`, with
/// q31 removed and the cache handed in.
///
/// **THE MACRO ARRIVES AS ITS PUSH** — the six terms this function used to compute inline from
/// `macro` and `macroConfigNorm`, built by `macroPushOf` in the same order and type. What stays here is
/// everything that must happen ONCE, after any blend of pushes: the dry vote's `> 0.5`, Repeat's
/// withholding of the delay push, the gate pull's zone-B guard, the ratchet branch the subdivision mix
/// lives in, and the probability clamps.
[[nodiscard]] inline SlicePlan planFor(float zoneA, float zoneB, float macroConfig, const MacroPush& push,
                                       int32_t sliceIndex, const PhaseOffsets& ofs, PlanCache* cache) {
	namespace phi = owlet::dsp::phi;
	using owlet::dsp::triangleSimpleUnipolar;

	constexpr int32_t kNumZones = 8;
	constexpr double kResolution = 1024.0; // matches the UI's scatter resolution
	constexpr float kStrideDeadzone = 0.3f;

	const float zoneBNorm = std::clamp(zoneB, 0.0f, 1.0f);
	const bool strideIsZero = zoneBNorm <= kStrideDeadzone;

	const float sliceWeight =
	    triangleSimpleUnipolar(phi::wrapPhase(static_cast<float>(sliceIndex) * phi::kPhiN050), 0.5f);
	const bool effectiveSliceIsZero = sliceWeight <= 0.1f;

	// THE DEAD ZONE: with no stride and no effective slice, the plan does not depend on `sliceIndex`
	// at all, so one answer serves every slice until a control moves.
	if (strideIsZero && effectiveSliceIsZero && cache != nullptr && cache->valid && cache->zoneA == zoneA
	    && cache->zoneB == zoneB && cache->macroConfig == macroConfig && cache->push == push
	    && cache->gamma == ofs.gamma && cache->zoneALap == ofs.zoneA && cache->zoneBLap == ofs.zoneB
	    && cache->macroConfigLap == ofs.macroConfig && cache->barIndex == ofs.barIndex
	    && cache->macroPushesDelay == ofs.macroPushesDelay && cache->sendOverride == ofs.sendOverride) {
		return cache->plan;
	}

	SlicePlan p;

	double phRawA = static_cast<double>(ofs.zoneA) + kResolution * static_cast<double>(ofs.gamma);
	const double phRawB = static_cast<double>(ofs.zoneB) + kResolution * static_cast<double>(ofs.gamma);

	if (ofs.barIndex != 0) {
		const float barBit0 = (ofs.barIndex & 1) ? 1.0f : 0.0f;
		const float barBit1 = (ofs.barIndex & 2) ? 1.0f : 0.0f;
		const float weight0 = triangleSimpleUnipolar(zoneBNorm * phi::kPhi050, 0.5f);
		const float weight1 = triangleSimpleUnipolar(zoneBNorm * phi::kPhi125, 0.5f);
		phRawA += static_cast<double>(barBit0 * weight0 + barBit1 * weight1) * kResolution * 0.25;
	}

	constexpr float kMaxStride = 8.0f;
	float stride = 0.0f;
	if (!strideIsZero) {
		stride = ((zoneBNorm - kStrideDeadzone) / (1.0f - kStrideDeadzone)) * kMaxStride;
	}
	const auto stridedSlice = static_cast<int32_t>(static_cast<float>(sliceIndex) * stride);

	// **THE CAST WAS UNDEFINED ONCE THE PHASE GREW, AND IT IS REACHABLE FROM A KNOB.** The reference
	// writes `static_cast<uint32_t>(phRawA * 65536.0f)`; `phRawA` is `zoneALap + 1024 * gamma`, so the
	// product passes 2^32 at a gamma of about 65535 — or at a Zone A lap of the same order — and a
	// float-to-unsigned conversion out of range is undefined behaviour, not a wrap. MEASURED: at
	// gamma 100 the product is already 6.7e9 against a 4.29e9 ceiling. On the hardware the offsets
	// move in tenths by a secret gesture and this is unreachable; here they are the whole part of two
	// unbounded coordinates and a hand can get there.
	//
	// REDUCED IN DOUBLE FIRST, which leaves every value the reference can actually produce
	// bit-identical — for `phRawA` under 65536 this is the same product and the same bits — and makes
	// the rest defined instead of lucky.
	const double phSeed = phRawA - std::floor(phRawA / 65536.0) * 65536.0;
	const uint32_t hashSeed =
	    static_cast<uint32_t>(stridedSlice) ^ static_cast<uint32_t>(phSeed * 65536.0) ^ 0x12345678u;
	const HashContext hashCtx { hashSeed };

	// **THE REFERENCE'S LINE, WITH THE FIRST TERM COMING FROM THE COORDINATE'S LAP.** It reads
	// `ofs.macroConfig * 0.1f + gamma`, where `ofs.macroConfig` was the push-and-twist offset. That
	// param is deleted and the coordinate carries it: `macroConfig` arrives here already split, its
	// fraction as the position inside the eight zones and its whole part as `ofs.macroConfig`. Same
	// arithmetic, one control. See `sklitter_types.h`.
	// **GAMMA NEITHER SATURATES NOR WRAPS. IT IS A PHASE OFFSET, AND THE PHI TRIANGLES ARE WHAT
	// WRAP IT — EACH AT ITS OWN PERIOD.** gamma is never supposed to saturate or wrap. if it
	// does likely is a bug. gamma drives phase offsets which drive phi triangles periodically.
	//
	// Both previous versions of this line were wrong, in opposite directions. The reference clamps
	// (`clamp(macroConfigNorm + macroConfigOffset, 0, 1)`), which MEASURED pins at a gamma of about
	// 0.7 against a Macro Cfg of 0.3 and freezes everything downstream. I then WRAPPED it, which is
	// worse in a way that is harder to hear: every reader of this sum multiplies it by a DIFFERENT phi
	// constant before wrapping — six of them, three `triangleSimpleUnipolar` (the dry vote at kPhi,
	// the gate pull at kPhi100, the ratchet mix at kPhi225) and three `triangleFloat` (reverse at
	// kPhi125, pitch at kPhi200, delay at kPhi075). Wrapping before the multiply forces all six to
	// share a period of 1 in gamma. Passing the phase RAW lets each wrap at its own period, and those
	// periods are incommensurate, so the triangles never come back into register and the evolution
	// does not repeat. That non-repetition is the entire point of a phi bank, and an early wrap is
	// exactly what destroys it.
	//
	// **SO IT IS CARRIED IN DOUBLE ALL THE WAY TO `wrapPhase`.** The triangles wrap their own input
	// with `floor` on a FLOAT, so a large phase loses its fraction before it is wrapped and
	// neighbouring gamma values collapse onto the same triangle output. That is saturation by another
	// name, arriving around 2^24. `PhiTriContext::gammaPhase` is a `double` in the reference with the
	// comment *"for precision at large values"*, which is this exact reasoning; the port had the value
	// but not the type.
	//
	// **ALL SIX READERS LIVE IN `macroPushOf` NOW, AND THAT IS WHERE THE DOUBLE MATTERS.** It forms
	// this same sum in the same order and type; `planFor` no longer reads it, and computes it here only
	// to publish it in `p.macroConfigNorm`.
	const double macroConfigOffset =
	    static_cast<double>(ofs.macroConfig) * 0.1 + static_cast<double>(ofs.gamma);
	const double macroConfigNorm = static_cast<double>(macroConfig) + macroConfigOffset;
	p.macroConfigNorm = macroConfigNorm;

	const int32_t effectiveSlice = effectiveSliceIsZero ? 0 : sliceIndex;
	const float slicePhase = phi::wrapPhase(static_cast<float>(effectiveSlice) * phi::kPhi);

	const float zoneANorm = std::clamp(zoneA, 0.0f, 1.0f);
	float effectiveZoneANorm = zoneANorm;
	if (phRawA != 0.0) {
		const float phaseContrib = phi::wrapPhase(phRawA * static_cast<double>(phi::kPhi075));
		effectiveZoneANorm = phi::wrapPhase(zoneANorm + phaseContrib);
	}
	const auto effectiveZoneA8 = static_cast<uint8_t>(effectiveZoneANorm * 127.0f);

	//---- ZONE A: what plays, from where, for how long ---------------------------------------------//
	const auto maxOffset = static_cast<uint8_t>(effectiveZoneANorm * 15.0f);
	p.sliceOffset = (maxOffset > 0) ? hashCtx.evalInt(HashSeed::SliceOffset, maxOffset + 1) : 0;

	const uint8_t lengthBits = (hash::derive(hashCtx.baseHash, HashSeed::LengthMult) >> 4) & 0x7;
	const auto minLengthIdx = static_cast<uint8_t>((1.0f - effectiveZoneANorm) * 7.0f);
	uint8_t lengthIdx = minLengthIdx + static_cast<uint8_t>((lengthBits * (8 - minLengthIdx)) >> 3);
	if (lengthIdx > 7) {
		lengthIdx = 7;
	}
	p.lengthMult = kLengthMultLevels[lengthIdx];

	p.shouldSkip = hashCtx.evalBool(HashSeed::SkipDecision, effectiveZoneANorm * 0.8f);
	p.skipTarget = hashCtx.evalInt(HashSeed::SkipTarget, 16);
	p.useDry = hashCtx.evalBool(HashSeed::DryMix, effectiveZoneANorm * 0.3f);
	p.macroWantsDry = push.dryThreshold > 0.5f;

	//---- ZONE B: what the slice sounds like -------------------------------------------------------//
	const float probPhase = phi::wrapPhase(zoneBNorm * phi::kPhi + phRawB * static_cast<double>(phi::kPhi125));
	float reverseProb = triangleSimpleUnipolar(probPhase, 0.5f);

	const float pitchPhase =
	    phi::wrapPhase(zoneBNorm * phi::kPhi150 + phRawB * static_cast<double>(phi::kPhi067));
	float pitchProb = triangleSimpleUnipolar(pitchPhase, 0.3f);

	const float delayPhase =
	    phi::wrapPhase(zoneBNorm * phi::kPhi075 + phRawB * static_cast<double>(phi::kPhi150));
	float delayProb = triangleSimpleUnipolar(delayPhase, 0.4f);

	// **THE PUSHES ARE BIPOLAR** — the reference's own `triangleFloat(macroConfigNorm * <phi>, 0.6f)`, now
	// inside `macroPushOf` — so the macro can push a probability DOWN as well as up. The reference skips
	// this whole block at a macro of 0.01 or less; that gate lives in the push now, which is zero there,
	// and clamping a triangle already in [0, 1] plus zero is the value it had. Unconditional so a blend
	// of pushes is clamped exactly once.
	reverseProb = std::clamp(reverseProb + push.dReverse, 0.0f, 1.0f);
	pitchProb = std::clamp(pitchProb + push.dPitch, 0.0f, 1.0f);
	delayProb = std::clamp(delayProb + (ofs.macroPushesDelay ? push.dDelay : 0.0f), 0.0f, 1.0f);

	// **THE OVERRIDE COMES AFTER THE PUSH AND AFTER THE CLAMP, WHICH IS THE WHOLE OF WHAT MAKES IT AN
	// OVERRIDE.** Joining the bias above would put it back inside `clamp(triangle + terms)`, where what
	// a term can reach depends on where the triangle sits — the exact arrangement backlog EW says a
	// dedicated control has to escape. Here it takes the finished probability, whatever the phi field
	// and the circle between them made of it, and lerps it to an endpoint. See `overriddenProbability`.
	//
	// **THE PROBABILITY IS EXACTLY 1.0 AT FULL PUSH; THE DRAW BELOW IS NOT QUITE ALWAYS, AND THAT IS
	// SAID RATHER THAN ROUNDED OFF.** `Context::toU32Threshold` maps a threshold of 1.0 to 0xFFFFFFFF
	// and `evalBoolFast` compares `derive(...) < threshold`, so the ONE hash value 0xFFFFFFFF still
	// declines — one slice in 2^32. The other end is exact: a threshold of 0 makes the comparison
	// `< 0`, which nothing satisfies, so "ties it to zero" is literally never. Left alone rather than
	// patched, because the asymmetry is the reference's threshold conversion and every other decision
	// on this device (reverse, pitch, skip, dry) is drawn through the same two lines — a `<=` there
	// would move all of them, which is a parity change wearing a one-character fix.
	delayProb = overriddenProbability(delayProb, ofs.sendOverride);
	p.delayProb = delayProb;

	p.shouldReverse = hashCtx.evalBool(HashSeed::ReverseDecision, reverseProb);
	p.shouldPitchUp = hashCtx.evalBool(HashSeed::PitchDecision, pitchProb);
	p.shouldDelay = hashCtx.evalBool(HashSeed::DelayDecision, delayProb);
	// **THE GRAIN LANE'S OWN DRAW, AT THE SAME PROBABILITY** (backlog EW). One line, here, is the whole
	// of "one decision mechanism serving both roads": every caller of `planFor` gets it, which is the
	// looper's `advanceSlice` and Grain's `grainPlanFor` alike.
	p.shouldSendToLane = hashCtx.evalBool(HashSeed::GrainLaneDecision, delayProb);
	p.delayRatioBits = static_cast<uint8_t>(hash::derive(hashCtx.baseHash, HashSeed::DelayRatio) & 0xF);

	if (phRawB != 0.0) {
		// FULL EVOLUTION: the discrete zones are ignored and every timbral param rides its own phi.
		const float pos = zoneBNorm;
		const float fmF = 1.0f + pos * (0.25f + 0.25f * phi::wrapPhase(phRawB * static_cast<double>(phi::kPhi067)));
		const float fmD = 1.0f + pos * (0.25f + 0.25f * phi::wrapPhase(phRawB * static_cast<double>(phi::kPhi125)));
		const float fmE = 1.0f + pos * (0.25f + 0.25f * phi::wrapPhase(phRawB * static_cast<double>(phi::kPhi200)));

		const float ph067 = phi::wrapPhase(phRawB * static_cast<double>(phi::kPhi067));
		const float ph125 = phi::wrapPhase(phRawB * static_cast<double>(phi::kPhi125));
		const float ph200 = phi::wrapPhase(phRawB * static_cast<double>(phi::kPhi200));

		p.filterFreq = triangleSimpleUnipolar(pos * phi::kPhi067 * fmF + ph067 + slicePhase + 0.250f, 0.7f);
		const float delayRaw =
		    triangleSimpleUnipolar(pos * phi::kPhi125 * fmD + ph125 + slicePhase + 0.500f, 0.6f);
		// **CLAMPED, AND THE REFERENCE IS WRONG HERE — THIS IS A BUG IN THE REFERENCE, NOT IN THE PORT.**
		//
		// The reference reads `static_cast<uint8_t>(delayRaw * 5.0f)` with the comment "[0,0.6]*5 =
		// [0,3]", which assumes `triangleSimpleUnipolar(x, 0.6f)` is bounded by its DUTY. It is not:
		// duty sets the WIDTH of the ramp, not its amplitude, and the function returns [0,1] for any
		// duty. So the product spans [0,5] and `delaySendBits` reaches 4 and 5 -- past the two bits
		// its own declaration promises ("2 bits: 0=off, 1=25%, 2=50%, 3=100%").
		//
		// THAT IS NOT MERELY A LOUDER SEND. `stutterer.cpp` computes `delaySendShift = 3 -
		// delaySendBits`, so a value of 4 or 5 makes the shift NEGATIVE -- undefined behaviour, not a
		// different sound. Parity with UB is not parity; it is inheriting a landmine. Clamped here,
		// and worth fixing upstream in the firmware too.
		//
		// FOUND BY SWEEPING THE EVOLUTION BRANCH, which the first draft of the mode tests never
		// entered. The two discrete paths below scale a [0,1] position by 3 and are correct.
		p.delaySendBits = static_cast<uint8_t>(std::min(delayRaw * 5.0f, 3.0f));
		p.envShape = triangleSimpleUnipolar(pos * phi::kPhi200 * fmE + ph200 + slicePhase + 0.750f, 0.8f);

		const float phDepth = phi::wrapPhase(phRawB * static_cast<double>(phi::kPhi050));
		const float fmDepth =
		    1.0f + pos * (0.25f + 0.25f * phi::wrapPhase(phRawB * static_cast<double>(phi::kPhi050)));
		p.envDepth = triangleSimpleUnipolar(pos * phi::kPhi050 * fmDepth + phDepth + slicePhase, 0.6f);

		const float phPan = phi::wrapPhase(phRawB * static_cast<double>(phi::kPhi125));
		p.panAmount = triangleSimpleUnipolar(pos * phi::kPhi125 + phPan + slicePhase, 0.25f);

		const float phStereo = phi::wrapPhase(phRawB * static_cast<double>(phi::kPhi175));
		const float stereoPhase = pos * phi::kPhi175 + phStereo + slicePhase;
		p.stereoWidth =
		    triangleSimpleUnipolar(stereoPhase, 0.15f) - triangleSimpleUnipolar(stereoPhase + 0.5f, 0.15f);

		const float phGate = phi::wrapPhase(phRawB * static_cast<double>(phi::kPhi150));
		const float gateRaw = triangleSimpleUnipolar(pos * phi::kPhi150 + phGate + slicePhase, 0.5f);
		p.gateRatio = 0.125f + (1.0f - gateRaw) * 0.875f;
	}
	else {
		// DISCRETE ZONES: the first four are one named effect each, the rest ride the timbral bank.
		const ZoneInfo zoneBInfo = computeZone(zoneBNorm, kNumZones);
		phi::PhiTriContext ctx { slicePhase, 1.0f, 1.0f, static_cast<double>(ofs.gamma) };
		constexpr int32_t kZoneBDiscreteZones = 4;

		if (zoneBInfo.index < kZoneBDiscreteZones) {
			switch (zoneBInfo.index) {
			case 0: // Flip
				p.shouldReverse = hashCtx.evalBool(HashSeed::ReverseDecision, zoneBInfo.position);
				break;
			case 1: // Filter
				p.filterFreq = zoneBInfo.position;
				break;
			case 2: // Echo
				p.delaySendBits = static_cast<uint8_t>(zoneBInfo.position * 3.0f);
				break;
			case 3: // Shape
			default:
				p.envShape = zoneBInfo.position;
				break;
			}
		}
		else {
			const auto timbral = ctx.evalBank(kTimbraBank, zoneBInfo.position);
			p.shouldReverse = hashCtx.evalBool(HashSeed::ReverseDecision, timbral[0]);
			p.filterFreq = timbral[1];
			p.delaySendBits = static_cast<uint8_t>(timbral[2] * 3.0f);
			p.envShape = timbral[3];
		}

		p.envDepth = triangleSimpleUnipolar(zoneBInfo.position * phi::kPhi050, 0.6f);
		p.panAmount = triangleSimpleUnipolar(zoneBInfo.position * phi::kPhi125, 0.25f);
		const float stereoPhase = zoneBInfo.position * phi::kPhi175;
		p.stereoWidth =
		    triangleSimpleUnipolar(stereoPhase, 0.15f) - triangleSimpleUnipolar(stereoPhase + 0.5f, 0.15f);
		if (zoneBInfo.position > 0.02f) {
			p.gateRatio = 1.0f - push.gatePull;
		}
	}

	//---- RATCHETS, REPEATS AND LONG GRAINS --------------------------------------------------------//
	if (effectiveZoneA8 < 2) {
		p.grainLength = 1;
		p.repeatSlices = 1;
		p.subdivisions = 1;
	}
	else {
		const auto longThresh = static_cast<uint8_t>(102 - ((effectiveZoneA8 * 76) >> 7));
		const uint8_t longMag = hashCtx.evalDutyU8(HashSeed::LongGrain, longThresh);
		p.grainLength = (longMag >= 16) ? 1 : (longMag >= 12) ? 8 : (longMag >= 6) ? 4 : 2;

		const auto repeatThresh = static_cast<uint8_t>((effectiveZoneA8 * 128) >> 7);
		const uint8_t repeatMag = hashCtx.evalDutyU8(HashSeed::RepeatSlice, repeatThresh);
		p.repeatSlices = (repeatMag >= 16) ? 1 : (repeatMag >= 11) ? 8 : (repeatMag >= 5) ? 4 : 2;

		if (p.repeatSlices > 1) {
			p.subdivisions = 1;
		}
		else {
			const auto binaryThresh = static_cast<uint8_t>((effectiveZoneA8 * 102) >> 7);
			const uint8_t binaryMag = hashCtx.evalDutyU8(HashSeed::BinarySubdiv, binaryThresh);
			const int32_t binarySub = (binaryMag >= 16) ? 1 : (binaryMag >= 11) ? 8 : (binaryMag >= 5) ? 4 : 2;

			const auto tripletThresh = static_cast<uint8_t>((effectiveZoneA8 * 51) >> 7);
			const uint8_t tripletMag = hashCtx.evalDutyU8(HashSeed::TripletSubdiv, tripletThresh);
			const int32_t tripletSub = (tripletMag >= 16) ? 1 : (tripletMag >= 8) ? 6 : 3;

			int32_t baseSub = 0;
			if (tripletSub > 1 && binarySub > 1) {
				baseSub = std::min<int32_t>(binarySub * tripletSub, 12);
			}
			else {
				baseSub = (tripletSub > 1) ? tripletSub : binarySub;
			}

			const float subdivMix = push.subdivMix;
			const int32_t targetSub = std::min<int32_t>(baseSub * 2, 12);
			p.subdivisions = 1 + static_cast<int32_t>(static_cast<float>(targetSub - 1) * subdivMix);
		}
	}

	if (strideIsZero && effectiveSliceIsZero && cache != nullptr) {
		cache->plan = p;
		cache->zoneA = zoneA;
		cache->zoneB = zoneB;
		cache->macroConfig = macroConfig;
		cache->push = push;
		cache->gamma = ofs.gamma;
		cache->zoneALap = ofs.zoneA;
		cache->zoneBLap = ofs.zoneB;
		cache->macroConfigLap = ofs.macroConfig;
		cache->barIndex = ofs.barIndex;
		cache->macroPushesDelay = ofs.macroPushesDelay;
		cache->sendOverride = ofs.sendOverride;
		cache->valid = true;
	}
	return p;
}

/// **THE REFERENCE/PORT ROAD: the push of ONE macro and its config**, the pair the reference's Stutterer
/// holds. **The config's LAP comes from `ofs.macroConfig`**, because that is where this function has
/// always read it: `macroConfig` arrives already split, so re-splitting it here would find lap zero and
/// drop the whole part. **THE DEVICE DOES NOT TAKE IT AND HAS NO PAIR TO OFFER** — Task 6 moved every
/// plan site onto `performPushOf`, and 2026-09-19 deleted the knobs themselves. The callers left are the
/// parity cases, for the reason the `offsetsFor` overload above gives at length.
///
/// **IT BUILDS THE PUSH ON EVERY CALL, CACHE HIT OR NOT** — `macroPushOf` runs before the dead-zone check
/// inside, so the hit path pays for six phi triangles it used to skip. That cost is a test's now: the
/// device builds ONE push per slice (per grain, per picture walk) and hands the same `const&` to both
/// `offsetsFor` and `planFor`, which is what `sklitter.cpp`'s **three** `targetPush()` sites do
/// (`sklitter.cpp:1044`, `:1813`, `:2205`).
///
/// **IT SAID FOUR, AND THE FOURTH IS A `planFor` CALL THAT THIS SENTENCE DOES NOT DESCRIBE.** `planFor`
/// has four call sites; the extra one is the ONE-AHEAD PEEK (`sklitter.cpp:1272`), which REUSES the push
/// the playing slice was built from and calls `planFor` alone — no `offsetsFor` beside it — so it is not
/// a site that "hands the same push to both". Counting `planFor` calls rather than push builders is how
/// four got here. **AND THE JOKE IS THAT `perform.h:183` DIAGNOSES EXACTLY THIS TWO HUNDRED LINES
/// EARLIER** — *"it said four until `targetPush()` existed, and the count is a maintenance hazard worth
/// naming"* — while this sentence went on saying four. A hazard named in one header is not fixed in the
/// next one; if you change the number of plan sites, the three places that count them are here,
/// `perform.h:183` and `sklitter.h:305`.
///
/// **THAT SHARING IS NOT ALL A SAVING, AND THE MEASURED TABLE LIVES WITH THE FUNCTION IT IS ABOUT** —
/// `perform.h`'s `performPushOf`, beside the bench that produced it (`tests/bench_perform_push.cpp`).
/// That builder calls `macroPushOf` FOUR times — it was five, and the fifth was the knob leg that went
/// with `Param::Macro` on 2026-09-19 — so the slice path, the Grain path and a still device's
/// per-block cost all went UP, and only the picture walk went down. Nothing allocates, so the
/// audio-thread rule is untouched, and nothing is cached: a cache keyed on any of it would be the ABA
/// trap CLAUDE.md spends a paragraph on.
[[nodiscard]] inline SlicePlan planFor(float zoneA, float zoneB, float macroConfig, float macro,
                                       int32_t sliceIndex, const PhaseOffsets& ofs, PlanCache* cache) {
	const MacroPush push = macroPushOf(macro, Coordinate { macroConfig, ofs.macroConfig }, ofs.gamma);
	return planFor(zoneA, zoneB, macroConfig, push, sliceIndex, ofs, cache);
}

//--------------------------------------------------------------------------------------------------//
// WHICH SLICE PLAYS, WHERE IT STARTS AND HOW LONG IT LASTS.                                          //
//--------------------------------------------------------------------------------------------------//
//
// The plan above says what to DO to a slice; this says WHICH slice and where to find it. It is pure
// arithmetic over the plan and the slice index, which is what makes it the layer worth pinning by
// value: "slice 6 plays reversed for 0.75 of its length starting at frame 12000" is a claim a
// rendered buffer cannot answer, because the buffer carries every quantity at once.

/// THE EIGHT ORDERINGS Pattern mode picks between, from Zone A. The reference names each in a
/// comment and those names are the contract -- they are transcribable, so the tests transcribe them.
enum class SlicePattern : uint8_t {
	Sequential = 0, ///< 0,1,2,3...
	Weave,          ///< 0,N-1,1,N-2,...
	Skip,           ///< evens then odds: 0,2,4,6,1,3,5,7
	Mirror,         ///< forward then back: 0,1,2,3,3,2,1,0
	Pairs,          ///< swap adjacent: 1,0,3,2,5,4,7,6
	Reverse,        ///< N-1,N-2,...,0
	Thirds,         ///< interleave by three: 0,3,6,1,4,7,2,5
	Spiral,         ///< middle outward: 3,4,2,5,1,6,0,7
	kCount
};

/// ZONE A PICKS THE PATTERN, in eight steps across one lap.
[[nodiscard]] inline SlicePattern patternFor(float zoneA) {
	const auto idx = static_cast<int32_t>(std::clamp(zoneA, 0.0f, 1.0f) * 8.0f);
	return static_cast<SlicePattern>(std::clamp(idx, 0, 7));
}

/// WHERE SLICE `index` GOES UNDER `pattern`. Ported arm for arm from the reference's switch.
[[nodiscard]] inline int32_t reorderFor(SlicePattern pattern, int32_t index, int32_t numSlices) {
	if (numSlices <= 1) {
		return 0;
	}
	const int32_t n = numSlices;
	const int32_t half = n / 2;
	int32_t i = index;
	switch (pattern) {
	case SlicePattern::Weave:
		i = (i & 1) ? (n - 1 - i / 2) : (i / 2);
		break;
	case SlicePattern::Skip:
		i = (i < half) ? (i * 2) : ((i - half) * 2 + 1);
		break;
	case SlicePattern::Mirror:
		i = (i >= half) ? (n - 1 - i) : i;
		break;
	case SlicePattern::Pairs:
		i ^= 1;
		break;
	case SlicePattern::Reverse:
		i = n - 1 - i;
		break;
	case SlicePattern::Thirds: {
		const int32_t third = (n + 2) / 3;
		i = (i % third) * 3 + (i / third);
		if (i >= n) {
			i = n - 1;
		}
		break;
	}
	case SlicePattern::Spiral: {
		const int32_t offset = (i + 1) / 2;
		int32_t spiral = (i & 1) ? (half + offset) : (half - offset);
		spiral = std::clamp(spiral, 0, n - 1);
		i = spiral;
		break;
	}
	case SlicePattern::Sequential:
	case SlicePattern::kCount:
	default:
		break;
	}
	return std::clamp(i, 0, n - 1);
}

/// WHAT ACTUALLY PLAYS FOR ONE STEP OF THE BAR.
struct SliceSchedule {
	/// **WHICH SLICE OF THE CAPTURED BAR — and its only reader is the test suite, deliberately.**
	///
	/// The render uses `startOffset`, derived from the same local two lines below, so this field
	/// changes no sound. The EH sweep flags it as a dead declaration and the answer is that it is a
	/// SEAM: three cases assert which slice a schedule chose, and `startOffset / baseSliceLength` is
	/// the implementation's own arithmetic — a test recomputing it would be restating the code it is
	/// checking rather than checking it. Kept, and said out loud so the next sweep does not have to
	/// ask twice.
	int32_t targetSlice = 0;
	int32_t grainLength = 1;   ///< how many consecutive slices it spans
	size_t sliceLength = 0;    ///< frames, including the bar's remainder on the last grain
	size_t startOffset = 0;    ///< frames into the captured bar
	friend bool operator==(const SliceSchedule&, const SliceSchedule&) = default;
};

/// THE SCHEDULE FOR STEP `sliceIndex` OF A BAR CUT INTO `numSlices`.
/// @param playbackLength ONE BAR, which is the grid: `numSlices` divides THIS.
/// @param bufferLength   the captured loop, which is the REACH: an offset may land anywhere in it.
///                       Zero means "the same as the bar", which is what the device did before the
///                       buffer became a setting and is what every grid-only caller wants.
///
/// **THE TWO ARE THE SAME NUMBER ONLY AT A ONE-BAR BUFFER, AND THAT IS THE WHOLE POINT.** The choice was
/// slices-per-BAR over slices-per-buffer, so Rate keeps meaning what it means and a longer buffer
/// gives the offsets more history to reach into rather than stretching the rhythm. Passing one number
/// for both is what the device did when it could only ever capture a bar.
///
/// **THE DEFAULT IS A REAL ANSWER, NOT A SILENCER.** CLAUDE.md records a default parameter quietly
/// muting every call site so that new logic was correct and reached by nothing; the guard against
/// that here is that the ONE caller which matters — `Sklitter::advanceSlice` — passes the buffer
/// explicitly, and the buffer's own case passes a length different from the bar. The other call sites
/// are grid tests, for which "the reach is the bar" is the truth rather than a convenience.
[[nodiscard]] inline SliceSchedule scheduleFor(Mode mode, float zoneA, int32_t sliceIndex,
                                               int32_t numSlices, size_t playbackLength,
                                               const SlicePlan& plan, size_t bufferLength = 0,
                                               size_t minSliceFrames = 256) {
	SliceSchedule s;
	if (numSlices <= 0 || playbackLength == 0) {
		return s;
	}
	if (bufferLength == 0) {
		bufferLength = playbackLength;
	}

	// TIME MODE STRETCHES by holding each source slice for `repeatSlices` steps -- 1111,2222,3333.
	int32_t base = (mode == Mode::Time && plan.repeatSlices > 1) ? sliceIndex / plan.repeatSlices : sliceIndex;

	// PATTERN AND PITCH REORDER; the others play the bar in order.
	if ((mode == Mode::Pattern || mode == Mode::Pitch) && numSlices > 1) {
		base = reorderFor(patternFor(zoneA), base, numSlices);
	}
	base = ((base % numSlices) + numSlices) % numSlices;

	// **HOW MANY SLICES THE BUFFER HOLDS, AT THE BAR'S GRID.** One at a one-bar buffer (identical to
	// `numSlices`), four times that at a four-bar one, and a QUARTER of it at a quarter-bar buffer —
	// which is why this is clamped at one rather than trusted: the two sub-bar rungs of the ladder
	// (`1/4 1/2 1 2 4`) can divide to zero at a coarse rate, and a modulo by zero is not a rounding
	// error.
	const size_t baseSliceLength = playbackLength / static_cast<size_t>(numSlices);
	// **THE RATIO, NOT `bufferLength / baseSliceLength`.** The first draft divided by the slice length
	// and it does NOT return `numSlices` when the buffer is the bar: `baseSliceLength` is a TRUNCATED
	// division, so at a bar of 10 over 4 slices it is 2, and 10/2 is FIVE. That would have moved the
	// arrangement at the default setting — a change to how the device sounds, arriving as a rounding
	// artefact inside a feature that is supposed to be inert until you turn it on.
	//
	// Scaling the count instead is exact at the default by construction: `llround(numSlices * 1.0)`
	// is `numSlices`, for every bar length and every rate. Clamped at one because the two sub-bar
	// rungs of the ladder (`1/4 1/2 1 2 4`) scale it below one at a coarse rate, and a modulo by
	// zero is not a rounding error.
	const auto bufferSlices = static_cast<int32_t>(
	    std::max<long long>(1, std::llround(static_cast<double>(numSlices)
	                                        * static_cast<double>(bufferLength)
	                                        / static_cast<double>(playbackLength))));

	// THE PLAN'S OFFSET, THEN ITS SKIP. A skip REPLACES the target rather than nudging it -- that is
	// what makes it a jump to somewhere non-adjacent rather than a bigger offset.
	//
	// **THE TARGET RANGES OVER THE BUFFER, THE PLAYBACK POSITION OVER THE BAR.** `base` is where we
	// ARE, which is a position on the bar's grid; `target` is where we READ FROM, which is anywhere in
	// the captured loop. Wrapping the target by `numSlices` is what confined a four-bar buffer to its
	// first bar, and at a one-bar buffer the two moduli are the same number so nothing moves.
	const int32_t offsetSlices = (plan.sliceOffset * bufferSlices) >> 4;
	int32_t target = (base + offsetSlices) % bufferSlices;
	if (plan.shouldSkip) {
		target = ((plan.skipTarget * bufferSlices) >> 4) % bufferSlices;
	}

	// A LONG GRAIN CANNOT RUN PAST THE END OF THE BAR, IN EITHER DIRECTION: not past the steps that
	// remain, and not past the slices that remain in the buffer.
	const int32_t remainingSteps = numSlices - sliceIndex;
	const int32_t remainingSlices = bufferSlices - target;
	s.grainLength = std::max(1, std::min({ plan.grainLength, remainingSteps, remainingSlices }));

	s.sliceLength = baseSliceLength * static_cast<size_t>(s.grainLength);

	// **THE LAST GRAIN OF THE BAR SWALLOWS THE REMAINDER, so the bar does not rush.** Integer division
	// loses up to numSlices-1 frames per bar; handing them to the final grain is what keeps the loop
	// the length the transport thinks it is.
	if (sliceIndex + s.grainLength >= numSlices) {
		s.sliceLength += playbackLength - baseSliceLength * static_cast<size_t>(numSlices);
	}
	// **THE SHORTEST A SLICE MAY BE, IN THE HOST'S FRAMES.** The literal 256 was the reference's count
	// at 44.1 kHz — 5.8 ms — so at 96 kHz the floor was half the duration it was meant to be. Passed in
	// because this is a free function and only the device knows the rate; the default preserves the
	// reference's number for the grid callers, whose subject is the arithmetic and not the floor.
	s.sliceLength = std::max<size_t>(s.sliceLength, minSliceFrames);

	s.targetSlice = target;
	s.startOffset = static_cast<size_t>(target) * baseSliceLength;

	// **REPEAT LOOPS ONE SPAN FROM THE END OF THE CAPTURE — the reference's, ported 2026-09-23 (backlog IX,
	// A1).** Until this, Repeat fell through to the lines above and read the bar STRAIGHT in N
	// pieces, which is Shuffle: MEASURED 0/24000/48000/72000 at rate 0.5, every bar. The reference
	// (`stutterer.cpp`, "Slice offset computation: Repeat=continuous"):
	//
	//     availableRange   = playbackLength - currentSliceLength;
	//     offsetAmount     = (sliceOffset * availableRange) >> 4;     // or skipTarget, on a skip
	//     sliceStartOffset = playbackLength - currentSliceLength - offsetAmount;
	//
	// **TWO DELIBERATE DIFFERENCES, BOTH "A1".** The span is the SLOT this grid cuts — `bar / N`,
	// the last one carrying the remainder — rather than the raw curve length the reference loops, so the
	// transport lock and the groove still tile the bar (the count is the curve's either way). And its reach
	// is the BUFFER, as every other mode's offset is since `Param::BufferLength`; at a one-bar buffer that is
	// the reference's `playbackLength`. A Repeat slot is never a long grain (the reference's
	// `effectiveGrainLength = 1`).
	if (mode == Mode::Repeat) {
		s.grainLength = 1;
		s.sliceLength = std::max<size_t>(
		    baseSliceLength + (sliceIndex + 1 >= numSlices ? playbackLength - baseSliceLength * static_cast<size_t>(numSlices) : 0),
		    minSliceFrames);
		const size_t span = std::min(s.sliceLength, bufferLength);
		const size_t available = bufferLength - span;
		const auto pick = static_cast<size_t>(plan.shouldSkip ? plan.skipTarget : plan.sliceOffset);
		const size_t offset = (pick * available) >> 4;
		s.startOffset = bufferLength - span - std::min(offset, bufferLength - span);
		s.targetSlice = baseSliceLength > 0 ? static_cast<int32_t>(s.startOffset / baseSliceLength) : 0;
	}
	return s;
}

//--------------------------------------------------------------------------------------------------//
// THE HOLD AND TIME'S OVERRIDE — WHICH PLAN A STEP PLAYS, NOT ONLY WHICH PLAN ITS INDEX DRAWS.        //
//--------------------------------------------------------------------------------------------------//
//
// Ported 2026-09-23 (after backlog IX found both missing). Until then `repeatSlices` was
// computed by `planFor` and read by exactly one line — Time's stretch divisor in `scheduleFor` — and
// Time drew its long grain and its hold from the same Zone A hash as Shuffle.

/// **THE MODES THAT HOLD A PLAN.** The reference's `!isRepeat` inside the slice setup Repeat, Time,
/// Grain, Pattern, Pitch and Shuffle all fall into (`stutterer.cpp`, the `case ScatterMode::Shuffle`
/// block). Repeat plans at its own counter and never holds; Grain has its own renderer here
/// (`playGrain`), so this port stops at the four modes `advanceSlice` cuts the bar for.
[[nodiscard]] constexpr bool holdsPlans(Mode mode) {
	return mode == Mode::Shuffle || mode == Mode::Time || mode == Mode::Pattern || mode == Mode::Pitch;
}

/// **TIME'S ZONES ARE ITS COMBINE AND ITS HOLD, READ STRAIGHT OFF THE KNOBS** — the reference's
/// `stutterer.cpp:703-712`, applied to a FRESH plan only (inside the `else` of the hold), before the plan
/// is cached:
///
///     combine = 1 + (int32_t)(zoneANorm * (scatterNumSlices - 1));   grainLength  = clamp(combine, 1, N)
///     repeat  = 1 + (int32_t)(zoneBNorm * (scatterNumSlices - 1));   repeatSlices = clamp(repeat, 1, N)
///
/// It replaces the Zone A hash's long grain and repeat draw and touches nothing else, so the ratchet
/// `planFor` drew stays — the reference computes `subdivisions` inside `computeGrainParams`, from the
/// HASHED `repeatSlices`, before this runs. **The positions are the coordinates' fractions**, the 0..1
/// part the eight zones divide, as `patternFor` reads Zone A; the lap is a phase and has no count in it.
/// One consequence, stated: a fraction is below 1.0, so the truncation tops out at N − 1 where the
/// reference's knob at its end stop reaches N — one step, at the very end of travel, where the next
/// increment rolls onto the following lap.
inline void applyTimeOverride(SlicePlan& plan, float zoneA, float zoneB, int32_t numSlices) {
	const int32_t n = std::max<int32_t>(numSlices, 1);
	const int32_t combine = 1 + static_cast<int32_t>(zoneA * static_cast<float>(n - 1));
	plan.grainLength = std::clamp<int32_t>(combine, 1, n);
	const int32_t repeat = 1 + static_cast<int32_t>(zoneB * static_cast<float>(n - 1));
	plan.repeatSlices = std::clamp<int32_t>(repeat, 1, n);
}

/// **THE REFERENCE'S `scatterCachedGrain` + `scatterRepeatCounter`.** A fresh plan is cached with
/// `repeatSlices − 1` slots left to run; each later slot plays the cached plan and counts one down.
/// The bar's end cancels it (`nextSliceIndex >= scatterNumSlices` → counter 0, *"compute fresh params for
/// new bar"*), so a hold never crosses a bar and the plan a step plays is a function of the bar's walk
/// from its first step — which is what lets the band and the peek restate it.
struct PlanHold {
	SlicePlan plan {};
	int32_t left = 0;
};

/// ONE SLOT'S PLAN UNDER THE HOLD — the reference's order exactly: a hold with slots left plays the cached
/// plan and counts down WITHOUT drawing; otherwise `fresh()` draws, Time's override is applied, and the
/// result is cached with `repeatSlices − 1` to run. `fresh` is a callable so the held road costs nothing,
/// as the reference's comment says of it (*"reuse cached grain (skip ~2200 cycles)"*).
template <typename Fresh>
[[nodiscard]] SlicePlan planUnderHold(PlanHold& hold, Mode mode, float zoneA, float zoneB, int32_t numSlices,
                                      Fresh&& fresh) {
	if (!holdsPlans(mode)) {
		return fresh();
	}
	if (hold.left > 0) {
		--hold.left;
		return hold.plan;
	}
	SlicePlan p = fresh();
	if (mode == Mode::Time) {
		applyTimeOverride(p, zoneA, zoneB, numSlices);
	}
	hold.plan = p;
	hold.left = std::max<int32_t>(p.repeatSlices, 1) - 1;
	return p;
}

} // namespace feathers::fx::sklitter
