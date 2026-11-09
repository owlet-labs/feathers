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

#include "dsp/character_bank.h"
#include "dsp/slice_build.h"
#include "dsp/constellation.h"
#include "plugin/audition_tuning.h"
#include "plugin/params.h"
#include "plugin/preview.h"
#include "plugin/sound_orientation.h"
#include "plugin/sound_residue.h"
#include "plugin/sound_gamma.h"
#include "plugin/sound_travel.h"
#include "plugin/sound_weights.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

// Where parameters become character coordinates.
//
// ONE mapping, in a header, because two things now need it and they must not be allowed to disagree:
// the synth, which plays the character, and the editor's wave display, which draws it. A picture of
// the sound derived from a second reading of the same parameters is a picture that can be subtly
// wrong for a whole release, and the wrongness would look like a design choice.
//
// A template over the reader rather than a function taking a ParamSet, because the two callers read
// from different places for good reasons: the synth reads ParamSet::resolved on the audio thread,
// and the editor reads the bridge's published snapshot on the main thread. Neither may borrow the
// other's source — see the threading note in params.h — but both may share the arithmetic.

namespace feathers {

/// Which oscillator the readings select, by the parameter table's own stepped rule. The editor was
/// briefly deriving this with a `>= 0.5` of its own, which agrees for two engines and would quietly
/// stop agreeing at three. The value-level rule lives in params.h now (oscTypeFromValue), where
/// consumers that hold a value rather than a reader — the two width controls' enablement — share it.
template <typename Read>
OscType oscTypeFromParams(Read&& read) {
	return oscTypeFromValue(read(ParamId::OscType));
}

/// Build the character coordinates from parameter readings. `read` takes a ParamId and returns the
/// value the ENGINE would run on, which for the editor means base plus host modulation.
/// NOTHING HERE IS DEFAULTED, and that is the point rather than an oversight. Every one of these
/// changes what comes out, so a call site that does not name one is a call site that has silently
/// chosen a value — and the three that mattered most, drift, the per-sound planes and now the banked
/// residue, were all added after the editor's calls were written. Defaults would have let those calls
/// keep compiling while drawing a patch nobody was playing, which is exactly what happened and what
/// 8f2e414 had to fix. Naming them makes the compiler list every site the next argument has to be
/// thought about at.
///
/// `velocity` is the smoothed puck velocity in units per second, and it is the ONE input here that is
/// not a function of the parameters — see PuckMotion for why it is measured on a fixed sample grid.
/// Zero is a parked puck, and a parked puck is the only value that changes nothing: every blue-shift
/// term below is this multiplied by something, so §2b.1's static purity holds by construction rather
/// than by a rule anybody has to remember.
///
/// `tuning` is the audition's handles — the orientation pin and the falloff A/B — and it is a
/// required argument rather than a defaulted one on purpose: the editor and the engine must be
/// auditioning the same thing, and a default would let one of them quietly forget to ask and draw a
/// picture of a sound nobody is playing. Normally every field is at the value that behaves as if none
/// of it were there. It arrives on the bridge's own channel, not as a parameter — see
/// audition_tuning.h, and the decision that these stay out of the CLAP parameter table.
/// The lookahead the motion terms are measured over.
///
/// A velocity in units per second becomes a DISTANCE by being given a time, and this is that time —
/// "where would the blend be if you kept going this long". The same order as the smoothing constant
/// (§2b's 50–100 ms), because a gesture and the window it is judged over should be the same size:
/// shorter and the effect tracks jitter, longer and it keeps reacting to a swipe you have finished.
inline constexpr double kBlueShiftLookaheadSeconds = 0.05;

/// What counts as a whole approach, in constellation units.
///
/// The default anchors sit about half a unit from the origin, so closing half a unit is arriving from
/// across the constellation — a full-strength approach. A MIXING CHOICE and meant for an ear, like
/// every other coefficient in this family.
inline constexpr double kBlueShiftApproachReference = 0.5;

/// **HOW FAR AN ADDITIVE EAR READS THE FIELDS FROM THE OTHER** — a multiple of the ear's anchor shift
/// (±(global Width · w_k)/2), added to each sound's travel so the ear reads the terrain at its own stand even at
/// Weather 0. A FLOOR, not an addition: Weather already moves the read by `weather` times the
/// shift, so the ear tops it up to this and no further, and at Weather knob ≥ √0.65 ≈ 0.81 the patch is what it
/// was. TUNED BY EAR; the measured shape it was picked from (default patch, Weather 0, side/mid at key 48):
/// 0 → −30.2 dB (only the A/B shares differed), 0.5 → −17.7, 0.6 → −13.7, **0.65 → −11.6**. At 0.65 a Width sweep
/// (20 steps) widens monotonically at the default puck, its worst dip over the 15-position sweep 0.09 dB (15.7 dB at 0);
/// at 0.7 every one of the 15 turned back. WHY IT TURNS BACK, AND WHAT IT IS NOT: not a ceiling of amplitude-only
/// stereo. Width drives TWO things here through one knob: the field offset (this constant times the shift) and the
/// anchor shift itself, whose change of A/B shares works against the field difference — and the response is not
/// monotone in the offset. Raising this constant moves the peak to a lower Width rather than capping it: the peak over
/// Width measured −10.0 dB at 1.0, −8.9 at 1.5, −7.2 at 6, and the same field offset is wider the smaller the anchor
/// shift that comes with it. So −6 to −10 dB at full Width is reachable by DECOUPLING the field offset from the anchor
/// shift; whether to is a design question left open.
/// **RE-TUNED 0.65 → 0.4 FOR PAL STAGE 1 (2026-10-19), BY MEASUREMENT, NOT BY EAR**: the terrain's new Rolloff spread and envelope
/// moved the turn-back below full Width (0.65 turned back from Width 0.6, 0.55 from 0.7, 0.5 and 0.45 from 0.85..0.9); 0.4 is the
/// largest of those tried that widens monotonically again, and reaches −12.6 dB side/mid at full Width (was −11.6). To be confirmed by ear.
inline constexpr double kPartialsEarTravel = 0.4;

/// How hard a fully reactive region pushes the overshoot at scale 1.
///
/// The same league as the character layer's own `kPhiMorphAmpOvershootBank` ceiling of 0.200, which is
/// the number this is generalising: blue shift should sound like the engine's existing excitation
/// aimed differently, not like a new and louder effect wearing its name.
inline constexpr float kBlueShiftOvershootMax = 0.25f;

/// THE BLUE SHIFT, written into a character that already knows where everything is.
///
/// Split out rather than inlined because it is the only part of the mapping that depends on MOTION,
/// and keeping it in one place is what makes "at rest none of this runs" something a reader can check
/// in one glance instead of tracing three multiplications.
///
/// The fields are read at the sound's own home — the same coordinate every other φ quantity in this
/// engine is read at, which is what §2b.1's periods say, since they are quoted in ZONES. So how
/// reactive a neighbourhood is travels with the sounds in it rather than with the pad.
inline void blueShiftInto(Character& c, std::span<const AnchorPoint> anchors, AnchorPoint velocity,
                          const AuditionTuning& tuning) {
	const size_t count = std::min<size_t>(c.count, anchors.size());

	// WHERE THE BLEND IS HEADING. The weights a lookahead ahead and a lookahead behind, from the same
	// function at the same exponent, so what comes out is the blend's own rate of change rather than a
	// model of it.
	//
	// CENTRED RATHER THAN FORWARD, and the difference is audible in principle: the weights are not
	// linear in position, so a forward difference answers a swipe and the same swipe reversed with
	// different magnitudes, and the overshoot would push harder one way round than the other for no
	// reason a player could hear a cause for. A centred difference is exactly antisymmetric under
	// reversing the gesture, which is the property a gesture and its undo should have. Found by a test
	// that expected the mirror image and got 0.196 against 0.237.
	const AnchorPoint ahead { c.puck.x + velocity.x * kBlueShiftLookaheadSeconds,
		                      c.puck.y + velocity.y * kBlueShiftLookaheadSeconds };
	const AnchorPoint behind { c.puck.x - velocity.x * kBlueShiftLookaheadSeconds,
		                       c.puck.y - velocity.y * kBlueShiftLookaheadSeconds };
	double aheadWeights[kMaxSounds] = {};
	double behindWeights[kMaxSounds] = {};
	blendWeights(anchors, ahead, { aheadWeights, count }, tuning.falloffPower);
	blendWeights(anchors, behind, { behindWeights, count }, tuning.falloffPower);

	// EVALUATE PER SOUND, INTERPOLATE THE VALUES — `Σ w·field(home)`, never `field(Σ w·home)`.
	//
	// Decided, and the reasoning is worth keeping because the two look equivalent and are not. The
	// wording is about values: readable at every point by simple interpolation. And it is the house
	// rule everywhere else in this engine — evaluate each sound, blend the results — for a reason that
	// bites here: an averaged COORDINATE can land in a region neither neighbour occupies, so two calm
	// sounds either side of a windy stripe would produce a phantom gale between them. Interpolating the
	// values is bounded by what the neighbours actually are, and standing on an anchor gives that
	// sound's field exactly under both, so nothing else moves.
	double overshootField = 0.0;

	for (size_t i = 0; i < count; ++i) {
		// One evaluation per sound, at that sound's own home, feeding both flavours: the overshoot's
		// weighted interpolation and this sound's own Doppler reactivity.
		//
		// THE BASE IS PART OF THAT HOME, and it was missing here until the default-dropping pass made
		// the compiler say so. A sound carried along its line by the Explore corner is somewhere else,
		// and these fields are declared a pure function of position (slice_build.h: read "at a home
		// coordinate, the same one every other φ quantity in this engine is read at") — so reading them
		// at the un-based home gave a carried sound the motion reactivity of the neighbourhood it left.
		// Exactly the shape of the plane bug the previous pass found in rotate_sound.h, and its sibling
		// one file over in orientation_row.cpp always passed the base. Bit-identical at base 0.
		const BlueShiftFields atSound = blueShiftFieldsAt(
		    sliceHome(c.sounds[i].zone, c.sounds[i].phaseOffset, c.sounds[i].gammaBase));
		overshootField += c.sounds[i].weight * static_cast<double>(atSound.overshoot);

		c.sounds[i].weightRate = 0.5 * (aheadWeights[i] - behindWeights[i]);

		// APPROACH RATE, per §2b: `dot(velocity, direction-to-sound)`. Given a time by the lookahead and
		// a size by the reference distance, so what comes out is "how much of a whole approach is this"
		// rather than a number in units per second that nothing downstream could scale.
		const double dx = anchors[i].x - c.puck.x;
		const double dy = anchors[i].y - c.puck.y;
		const double distance = std::sqrt(dx * dx + dy * dy);
		double approach = 0.0;
		if (distance > 0.0) {
			const double closing = (velocity.x * dx + velocity.y * dy) / distance;
			approach = closing * kBlueShiftLookaheadSeconds / kBlueShiftApproachReference;
		}
		// The Doppler field is this sound's own: it says how strongly THIS sound announces itself, which
		// is a property of the sound and travels with it. Per sound by construction, so the decision's
		// scheme needs nothing extra here — it is the overshoot, which is one number for the whole
		// blend, that had to be told which of the two it was.
		approach *= static_cast<double>(atSound.doppler) * tuning.blueDopplerScale;
		// Bounded so a fast swipe cannot invert a share or silence a sound outright — the plan asks for
		// brightening and dimming, and a weight that reached zero would be a sound disappearing.
		c.sounds[i].approach = std::clamp(approach, -0.85, 0.85);
	}

	c.blueOvershoot = static_cast<float>(overshootField * kBlueShiftOvershootMax
	                                     * tuning.blueOvershootScale);
}

/// WHICH LISTENER A CHARACTER IS BUILT FOR — terrain stereo's per-sound form (V13/V14c).
///
/// Head is the mono render and what every readout draws: no displacement at all, and the width
/// parameters are not even read, so the mono path costs what it always cost. Left and Right are the
/// two ears of a stereo build: each ear sees sound k's ANCHOR displaced by ±(global·w_k)/2 along the
/// disk's X — the ear axis, fixed and legible (V11) — so the weights, the weather travel, the
/// operator fields and the per-ear Doppler all inherit the displacement PER SOUND, which is the
/// point of the feature. An enum rather than an offset, because the displacement is no longer one
/// number: it is a SIDE, and the per-sound magnitudes belong to parameters this mapping already
/// reads. The values are the displacement's sign, and negation is exact in floating point, so the
/// two ears are exact mirrors by construction.
enum class EarSide : int8_t { Head = 0, Left = 1, Right = -1 };

template <typename Read>
Character characterFromParamsAtSide(Read&& read, OscType type, const AuditionTuning& tuning,
                                    AnchorPoint velocity, const SoundTravel& drift,
                                    const SoundOrientations& planes, const SoundResidue& residue,
                                    // THE DEBT IS PAID. This was defaulted only until its two readout
                                    // callers were wired, and they are: wave_display.cpp and
                                    // orientation_row.cpp both pass bridge_.soundGamma() now, as the synth
                                    // always did. So the default is gone, because what it bought was
                                    // convenience for callers that no longer exist and what it cost was a
                                    // silent wrong answer for any caller written next — a sound read at a
                                    // zero base is a NEIGHBOUR of the sound the engine is making, which is
                                    // G1, and G1 poisoned a round of listening judgments before anyone
                                    // noticed the picture was authoritative and wrong. Required means the
                                    // compiler names the next site instead of a reader having to spot it.
                                    const SoundGamma& gamma,
                                    // AND PREVIEW, REQUIRED FOR EXACTLY THE SAME REASON — see preview.h.
                                    // This one decides WHICH SOUNDS ARE IN THE CHARACTER AT ALL, so a site
                                    // that defaulted it would draw the whole blend while the engine played
                                    // one sound: the picture authoritative and wrong, which is G1 again
                                    // with a new cause. Both readers of this mapping pass what the bridge
                                    // holds, so the scope cannot draw a sound nobody is hearing.
                                    const Preview& preview,
                                    // WHICH EAR THIS CHARACTER IS FOR — see EarSide above. Terrain stereo
                                    // (§5.5, decisions V11/V13) is two of these characters, one per ear, and
                                    // since V13 the displacement lands on each sound's ANCHOR rather than
                                    // on the puck, scaled per sound: the weights, the weather travel, the
                                    // operator fields and the blue shift all see the displaced geometry, so
                                    // an ear is a full evaluation of the terrain as it stands FOR THAT EAR
                                    // rather than a pan law over the head's. Head touches nothing — not
                                    // even a `+ 0.0` — and skips the width reads entirely, so the mono
                                    // path is structurally today's. Head is what the editor's readouts
                                    // pass: the pictures draw the head-centre sound.
                                    // Required rather than defaulted, same rule as everything else here.
                                    // AS A NUMBER (voice stacking step 4c): an ear is ±1 and the head 0, and a stacked part
                                    // stands between, at its place in the fan doubled — see `characterFromParams` below.
                                    double side) {
	Character c;
	// V5 DELIBERATELY TAKES THE MORPH RENDER PATH: the flagship's operators are baked into the
	// table at build time (flagship-core plan), so the render side grows no case for it — V5 is Morph to
	// OscEngine, and the stair renderer stays frozen. (Resonate IS a third OscEngine, Weave: its voices
	// read a string rather than any table.) The flag is what routes a
	// morph-path build through the operator tier, derived HERE so the synth and the editor cannot
	// disagree about whether a patch is flagship.
	// RESONATE IS ITS OWN ENGINE, not a morph-path build: its voices scan the synth's string, so no table
	// the bank could build is what they play. Named here rather than falling through to Morph, which is
	// what a fourth OscType did before it had a case.
	// BASIC SHAPES IS ITS OWN ENGINE TOO: its voices read band-limited shape ladders, not a morph table. It
	// keeps V5's recipe for FM, Warp and Grit (the bank builds that from the same sounds), but `flagship`
	// stays false — that flag means "bake the operator grid", and Basic Shapes never does.
	// ADDITIVE IS ITS OWN ENGINE (2026-10-10): its voices render the blended SPECTRUM as a partial bank, so the bank
	// builds a PartialSpectrum and no table at all. `flagship` stays false for Basic Shapes' reason.
	c.engine = type == OscType::Stair         ? OscEngine::Stair
	           : type == OscType::Resonate    ? OscEngine::Weave
	           : type == OscType::BasicShapes ? OscEngine::Shapes
	           : type == OscType::Additive    ? OscEngine::Partials
	           : type == OscType::Pluck       ? OscEngine::String
	           : type == OscType::Drum        ? OscEngine::Drum
	                                          : OscEngine::Morph;
	c.flagship = type == OscType::V5;
	if (type == OscType::BasicShapes) {
		c.shapes.sine = static_cast<float>(read(ParamId::ShapesSine));
		c.shapes.triangle = static_cast<float>(read(ParamId::ShapesTriangle));
		c.shapes.saw = static_cast<float>(read(ParamId::ShapesSaw));
		c.shapes.square = static_cast<float>(read(ParamId::ShapesSquare));
	}
	// THE ADDITIVE SHAPE, read only on Additive for the shapes mix's reason: every other engine's Character keeps the
	// default and compares equal to what it was. By the table's own stepped rule, so a host value between two choices
	// lands where the menu says it does.
	static_assert(kAdditiveShapeNames.size() == static_cast<size_t>(partials::ShapeKind::kCount),
	              "the Additive Shape menu names every ShapeKind, in its order (test_params pins each label)");
	if (type == OscType::Additive) {
		c.additiveShape = static_cast<partials::ShapeKind>(
		    steppedIndex(read(ParamId::AdditiveShape), kAdditiveShapeNames.size()));
		// A-PH (design §8): the mode always; Sub and Ceiling only under Sub priority, where they act — so moving them under
		// Natural or Headroom rebuilds nothing (they are greyed there: the honesty rule).
		static_assert(kAdditivePhaseNames.size() == 3 && static_cast<size_t>(partials::PhaseMode::SubPriority) == 2,
		              "the Phases menu names every PhaseMode, in its order");
		c.phasing.mode = static_cast<partials::PhaseMode>(steppedIndex(read(ParamId::AdditivePhases), kAdditivePhaseNames.size()));
		if (c.phasing.mode == partials::PhaseMode::SubPriority) {
			c.phasing.subDb = static_cast<float>(read(ParamId::AdditiveSub));
			c.phasing.ceilingDb = static_cast<float>(read(ParamId::AdditiveCeiling));
		}
	}
	const bool additive = type == OscType::Additive;
	// THE WARP SHAPE, ENGINE-WIDE — derived here for `flagship`'s reason exactly: so the synth and the
	// editor cannot disagree about what a patch is. Clamped rather than cast blind, because the value
	// arrives from a host that may hand over anything within the declared range and a bad index would
	// read past `kWarpModeNames`.
	{
		const auto raw = static_cast<int>(std::lround(read(ParamId::WarpMode)));
		c.warpMode = static_cast<WarpMode>(
		    std::clamp(raw, 0, static_cast<int>(WarpMode::kCount) - 1));
	}
	c.orientation = tuning.orientation;
	c.v1Crossfade = tuning.v1Crossfade;

	// Nothing is clamped — offset and gamma are identity coordinates rather than amounts, and the
	// builders take any real value.
	//
	// Worth knowing while reading this: zone and phase offset were the SAME axis for the whole of v1.
	// Both builders opened with `phase = zone/1023 + phaseOffset`, so the four parameters below
	// addressed one coordinate — the finding that deferred Task 5. They are two coordinates of a plane
	// now, and the generator above is what decides whether that plane is open or still the old line.
	const double globalGamma = read(ParamId::GlobalGamma);
	c.xf = static_cast<float>(read(ParamId::Path));

	// THE ENABLED SOUNDS, IN SLOT ORDER, AND NOTHING ELSE. A switched-off sound is not a sound with a
	// weight of zero — it is absent, so it takes no place on the tour and no share of the blend. Slot
	// order is what makes the tour reproducible: the polyline walks the constellation in the order the
	// patch declares it, and an order that depended on where the puck happened to be would make Path
	// mean something different from one moment to the next.
	AnchorPoint anchors[kMaxSounds];
	/// Which slot each live sound came from. The live sounds are PACKED — a disabled slot takes no
	/// place — so an offset indexed by position rather than by slot would be somebody else's drift the
	/// moment a sound is switched off.
	size_t slotOf[kMaxSounds] = {};
	size_t count = 0;
	// PREVIEW TAKES ONE SOUND AND LEAVES THE SWITCHES ALONE. While it is on, the character is that
	// sound and nothing else — including when its own switch is off, which is the point: auditioning a
	// sound that is not in the patch is what you do before deciding to put it in one, and preview
	// writes nothing, so it stays out of the patch while you listen.
	// THE DRUM ENGINE'S KIT MODE (drum proposal §3.3): every sound is a pad, so all eight are in the character whatever their
	// switches and whatever is previewed — a kit with a pad switched out of the build would play that pad's note at a default
	// place. Each pad stands at its own place moved by the Walk, never blended (below).
	const bool drumKit = type == OscType::Drum
	                     && steppedIndex(read(ParamId::DrumMode), kDrumModeNames.size()) == static_cast<size_t>(DrumMode::Kit);
	c.drumKit = drumKit;
	const bool previewing = preview.active() && !drumKit;
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		const bool inCharacter =
		    drumKit      ? true
		    : previewing ? slot == preview.slot
		                 : steppedIndex(read(soundParam(slot, SoundField::Enable)), kEnableNames.size()) != 0;
		if (!inCharacter) {
			continue;
		}
		SoundCoord& sound = c.sounds[count];
		// THE WAY BACK OUT OF THE PACKING, carried rather than recovered. `slotOf` below already holds
		// it for this function's own use; putting it on the sound as well is what lets a reader of the
		// finished character address these the way a player does — by rail row — without owning a second
		// copy of the packing rule. See SoundCoord::slot.
		sound.slot = static_cast<uint8_t>(slot);
		// THE KIT-WIDE LAYER (2026-11-06): on a Drum Kit, Kit Zone moves every pad's zone (before the rounding, so it is moving
		// each pad's Zone by the same amount, held inside the zones that exist) and Kit Offset and Kit Gamma its phase offset.
		const double kitZone = drumKit ? read(ParamId::DrumKitZone) : 0.0;
		const double kitPhase = drumKit ? read(ParamId::DrumKitOffset) + read(ParamId::DrumKitGamma) : 0.0;
		sound.zone = static_cast<uint16_t>(std::lround(drumKit ? std::clamp(read(soundParam(slot, SoundField::Zone)) + kitZone, 0.0, 1023.0)
		                                                       : read(soundParam(slot, SoundField::Zone))));
		// Gamma is not a knob of its own in the character layer: the engine adds it to the phase offset,
		// which is the coordinate that walks a zone into quasi-periodic bank space. Global Gamma is the
		// macro offset over every sound, so it lands on all of them.
		sound.phaseOffset = static_cast<float>(read(soundParam(slot, SoundField::Offset))
		                                       + read(soundParam(slot, SoundField::Gamma)) + globalGamma + kitPhase);
		// AND GAMMA'S UNBOUNDED HALF, kept out of that float on purpose. The parameter above is the
		// vernier and stays bounded, automatable and exactly where it was; this is the coarse half,
		// and it reaches `home` as a double so it does not quantise the fine one. Zero here is the
		// engine as it was, to the bit.
		sound.gammaBase = gamma.base[slot];
		// THE SMOOTHNESS OVERRIDE (doc §11) — a per-sound knob that scales the flagship smoothness field.
		// Default 1.0 = full field authority, so the field is applied exactly as authored; 0.0 defeats
		// smoothing (plain morph). Stored on the SoundCoord so the DSP blend (buildFlagshipFor) and the
		// terrain path (buildFlagshipSliced) both see the scaled value.
		sound.smoothnessScale = static_cast<float>(read(smoothnessParam(slot)));
		// THE FM DEPTH KNOB (FM doc §9/§10) — the one quantity FM spends ids on. Zero is the default
		// and is the identity: the voice never touches its modulator and renders exactly what it
		// rendered before FM existed. Everything else FM needs is terrain, read from the fields at
		// this sound's own coordinate.
		sound.fmDepth = static_cast<float>(amountTaper(read(fmDepthParam(slot))));
		// THE FIVE FIELD MODIFIER KNOBS (2026-08-25). Read RAW, with no taper: the taper on fmDepth
		// above exists because FM depth is an amount whose bottom end wants resolution, while these
		// five are positions on a curve whose middle is the meaningful landmark — a taper would move
		// kFieldNeutral off the knob's centre detent and make "the field as authored" a place you have
		// to hunt for. fieldWithOverride does the shaping, once, where the field is read.
		sound.quantizeScale = static_cast<float>(read(quantizeParam(slot)));
		// WARP DEPTH — an amount, read straight through. No fieldWithOverride and no neutral: the
		// knob IS the value, because by design warp gets no phi field (dsp/warp.h).
		sound.warpDepth = static_cast<float>(read(warpParam(slot)));
		// THE ADDITIVE SHAPE AMOUNT — an amount, read straight through, and only on Additive (the bank reads it only on
		// a Partials build; left at 0 elsewhere so every other engine's SoundCoord compares equal to before).
		if (additive) {
			sound.shapeAmount = static_cast<float>(read(shapeAmountParam(slot)));
			// AND THE SIX BIAS KNOBS (design §4.1), read raw like the five field modifiers above — fieldWithOverride does
			// the shaping where the fields are read (partialFieldsWithOverrides) — and only on Additive, Shape Amount's
			// reason: everywhere else they stay neutral and the SoundCoord compares equal to before.
			sound.partialBias = { static_cast<float>(read(additiveRolloffParam(slot))),
				                  static_cast<float>(read(additiveFormantParam(slot))),
				                  static_cast<float>(read(additiveOddEvenParam(slot))),
				                  static_cast<float>(read(additiveCombParam(slot))),
				                  static_cast<float>(read(additiveDetailParam(slot))),
				                  static_cast<float>(read(additiveOffGridParam(slot))) };
		}
		// THE STRING'S FIVE FIELD SCALERS (KS design §4.1), raw, and only on Pluck: everywhere else they stay neutral and
		// the SoundCoord compares equal to before.
		if (type == OscType::Pluck) {
			sound.stringBias = { static_cast<float>(read(ksBrightnessParam(slot))), static_cast<float>(read(ksSustainParam(slot))),
				                 static_cast<float>(read(ksPickParam(slot))), static_cast<float>(read(ksNoiseParam(slot))),
				                 static_cast<float>(read(ksStretchParam(slot))) };
		}
		// THE DRUM'S SEVEN FIELD SCALERS (drum proposal §3.2), raw, and only on Drum.
		if (type == OscType::Drum) {
			sound.drumBias = { static_cast<float>(read(drumParam(slot, DrumControl::FieldPitch))),
				               static_cast<float>(read(drumParam(slot, DrumControl::FieldLength))),
				               static_cast<float>(read(drumParam(slot, DrumControl::FieldSweep))),
				               static_cast<float>(read(drumParam(slot, DrumControl::FieldTone))),
				               static_cast<float>(read(drumParam(slot, DrumControl::FieldNoise))),
				               static_cast<float>(read(drumParam(slot, DrumControl::FieldSnap))),
				               static_cast<float>(read(drumParam(slot, DrumControl::FieldMetal))) };
			// AND THE KIT-WIDE TRIMS on top (2026-11-06): the scaler's own law again, so neutral is the pad's fields to the bit.
			if (drumKit) {
				sound.drumKitBias = { static_cast<float>(read(ParamId::DrumKitPitch)), static_cast<float>(read(ParamId::DrumKitLength)),
					                  static_cast<float>(read(ParamId::DrumKitSweep)), static_cast<float>(read(ParamId::DrumKitTone)),
					                  static_cast<float>(read(ParamId::DrumKitNoise)), static_cast<float>(read(ParamId::DrumKitSnap)),
					                  static_cast<float>(read(ParamId::DrumKitMetal)) };
				// AND ITS LAYER'S OFFSET (stage (c)), only while the pad has a layer, so a layerless kit compares equal to before.
				if (std::lround(read(drumParam(slot, DrumControl::LayerKind))) > 0) {
					sound.drumLayer = true;
					sound.drumLayerX = read(drumParam(slot, DrumControl::LayerOffsetX));
					sound.drumLayerY = read(drumParam(slot, DrumControl::LayerOffsetY));
				}
			}
		}
		sound.syncScale = static_cast<float>(read(syncParam(slot)));
		sound.feedbackScale = static_cast<float>(read(feedbackParam(slot)));
		sound.pointScale = static_cast<float>(read(pointsParam(slot)));
		sound.gritScale = static_cast<float>(read(gritParam(slot)));
		anchors[count] = { read(soundParam(slot, SoundField::PlaceX)),
			               read(soundParam(slot, SoundField::PlaceY)) };
		slotOf[count] = slot;
		// The plane this sound plays on, decided here so the editor and the engine cannot disagree
		// about whose it is. Unpinned everywhere is the engine as it always was.
		sound.orientation = resolvedOrientation(planes, slot, tuning.orientation);
		// And what its rotations banked, so that turning a plane left the sound standing on it alone —
		// decided 2026-08-13. BY SLOT, like the drift and for the same reason (A7): the live
		// sounds below are packed, and a residue indexed by position would be somebody else's the moment
		// a sound was switched off.
		sound.residue = residue.banked[slot];
		count++;
	}
	c.count = static_cast<uint8_t>(count);

	// WHERE YOU ARE STANDING. The tour walks the constellation and Pos X/Y step off it, so the puck is
	// one place in the same plane the sounds are placed in. Pos X and Pos Y have been in the table
	// since v1 — bipolar, centre-detented, declared and dormant — reserved for exactly this.
	const std::span<const AnchorPoint> live { anchors, count };
	const AnchorPoint onTour = tourPoint(live, c.xf);
	// AND WHILE PREVIEWING YOU ARE STANDING ON THE SOUND. Not "the others are muted" — the puck is put
	// AT the anchor, which is what makes the travel term below exactly zero and leaves `home + drift`:
	// the sound at its own identity, which is what the Explore chart draws. Path and Pos X/Y are not
	// consulted, because where the performance is standing is the question preview exists to suspend.
	//
	// It runs through the SAME arithmetic as the ordinary case rather than short-circuiting to a
	// separate formula — one road, so the two cannot come to disagree about what standing on a sound
	// means.
	const AnchorPoint puck = previewing && count > 0
	                             ? anchors[0]
	                             : AnchorPoint { onTour.x + read(ParamId::PosX),
		                                         onTour.y + read(ParamId::PosY) };

	// PER-SOUND WIDTH DISPLACES THE ANCHORS, per ear, and only now — AFTER the tour and the puck are
	// taken from the undisplaced set (V13/V14c). The tour is where the player walks: the constellation
	// the patch declares and both ears share. Width is where the SOUNDS stand for each ear, so for ear
	// L sound k's anchor moves +(global·w_k)/2 along the disk's X, and for ear R the exact negation.
	//
	// THE UNIFORM CASE IS THE SHIPPED PUCK DISPLACEMENT, derived rather than asserted: everything
	// downstream of here reads the geometry RELATIVELY — the weights, the weather travel and the blue
	// shift all consume (puck − anchor) — so displacing every anchor by +d is displacing the puck by
	// −d, which is the shipped ear L at separation d·2 = global·w. That is why the tour must come from
	// the undisplaced anchors: a tour over displaced ones would carry the puck along with the sounds
	// and the relative displacement would cancel. The reduction case in tests/test_terrain_stereo.cpp
	// pins this equivalence bit for bit against a mono render at PosX ∓ (global·w)/2, on the binary
	// grid the rig keeps for exactly this comparison.
	//
	// AND ONLY OUTSIDE PREVIEW — V12a. Preview's own invariant is "puck at the anchor, travel exactly
	// drift, the sound at its own identity", and a displaced ear would colour the very thing preview
	// exists to isolate, so width suspends while previewing and the ordinary rebuild fades cover both
	// transitions. At Head the block is skipped whole: the width parameters are not read, so the mono
	// path is today's to the instruction.
	double earShift[kMaxSounds] = {}; // each sound's anchor displacement for this ear, for Additive's field read below
	if (side != 0.0 && !previewing) {
		const double globalWidth = read(ParamId::Width);
		for (size_t i = 0; i < count; ++i) {
			earShift[i] = side * 0.5 * globalWidth * read(soundParam(slotOf[i], SoundField::Width));
			anchors[i].x += earShift[i];
		}
	}

	// THE SHARES ARE GEOMETRY, so they are computed where the geometry is and travel with the character
	// rather than being recomputed by whoever needs them. Every sound count reads them now — §2c made
	// the blend rule uniform, and only parity mode still goes by Path alone — and
	// a build that computed them only when it was about to use them would be a build where the editor
	// could draw a blend the engine was not playing.
	double weights[kMaxSounds] = {};
	blendWeights(live, puck, { weights, count }, tuning.falloffPower);

	// TRAVEL IS WHAT WEATHER LETS THROUGH. At zero every product below is exactly ±0.0, so each sound
	// is precisely its home and the blend is the relative one v1 shipped — bit for bit, not nearly.
	// Turned up, where you are standing starts to colour what you hear.
	// AND DRIFT IS ADDED, NOT SCALED. Weather says how much of the terrain the PUCK's position lets
	// through; a sound's own drift is part of what that sound is, so a patch at Weather 0 can still
	// have drifted sounds — which it could not if the offset went through the same multiplier. Two
	// coordinates that answer different questions, added because they are both distances from home.
	//
	// At the default offset of exactly 0.0 this is `x + 0.0`, which is exact for every finite x, so a
	// patch that has never drifted builds the character it always built. That is the migration
	// guarantee and it is checked rather than asserted — see the zero-offset case in the blend suite.
	// TAPERED, for the reason amountTaper states: measured with the puck held far from a sound's
	// anchor, Weather covered 98% of its total distance by knob 0.125. Zero is still exactly zero, so
	// the migration guarantee two paragraphs up is untouched.
	const double weather = amountTaper(read(ParamId::Weather));
	for (size_t i = 0; i < count; ++i) {
		c.sounds[i].travelX = weather * (puck.x - anchors[i].x) + drift.offset[slotOf[i]].x;
		c.sounds[i].travelY = weather * (puck.y - anchors[i].y) + drift.offset[slotOf[i]].y;
		c.sounds[i].weight = weights[i];
	}
	// A KIT PAD STANDS AT ITS OWN PLACE MOVED BY THE WALK (drum proposal D3): the puck is not where a kit is played from, so
	// Weather's puck term does not reach it; the Walk moves every pad together, and its own drift is still its own. At Walk 0
	// each pad is exactly home + drift.
	if (drumKit) {
		const double walkX = read(ParamId::DrumWalkX);
		const double walkY = read(ParamId::DrumWalkY);
		for (size_t i = 0; i < count; ++i) {
			c.sounds[i].travelX = walkX + drift.offset[slotOf[i]].x;
			c.sounds[i].travelY = walkY + drift.offset[slotOf[i]].y;
		}
	}
	// AND ON ADDITIVE EACH EAR READS THE FIELDS AT ITS OWN STAND, at any Weather (2026-10-11, option (b) of the
	// stereo diagnosis). The anchor shift above moves only the blend while Weather is 0 — travel is 0 in both ears —
	// so an Additive ear differed from the other by its A/B shares alone, capped at the two sounds' own difference
	// (−18.4 dB side/mid on the default patch) and NARROWING as the shift grew, the shares tending to 50/50 in both
	// ears. This adds the ear's shift to each sound's travel, puck-relative as Weather's own term is, so every field and
	// detail triangle is read at the ear's coordinate — topped up to kPartialsEarTravel times the shift, counting what
	// Weather's own term above already moved it. Amplitude only: the voice still shares one rotator per partial
	// across the ears (design §1.4), so the mono sum cannot cancel. At Width 0 the shift is exactly 0 and the travel
	// is the mono one to the bit; Head (side 0) never reaches it; no other engine reads it. The field offset rides on
	// the anchor shift (one knob, both effects), which is what bounds the width at the shipped constant — see
	// kPartialsEarTravel for the measurement and the open question of decoupling them.
	if (type == OscType::Additive) {
		const double topUp = std::max(0.0, kPartialsEarTravel - weather);
		for (size_t i = 0; i < count; ++i) {
			c.sounds[i].travelX -= topUp * earShift[i];
		}
	}
	c.puck = puck;

	// AND NO MOTION TERMS WHILE PREVIEWING. The blue shift is what the blend does as you MOVE THROUGH
	// it — an overshoot aimed along the puck's velocity, a Doppler on the rate of approach — and a
	// preview has suspended the puck at the sound. Leaving it in would let a gesture somewhere else on
	// the circle colour a sound you are holding still to listen to, which is the lie this feature
	// exists to stop. Standing on one sound at rest is what preview means, and at rest these terms are
	// zero anyway; this makes it true while the puck is being dragged as well.
	if (!previewing && count > 0 && tuning.blueShiftActive()
	    && (velocity.x != 0.0 || velocity.y != 0.0)) {
		blueShiftInto(c, { anchors, count }, velocity, tuning);
	}
	return c;
}

/// THE ACTIVATION WEIGHTS, UNPACKED — a character's shares addressed by slot rather than by position.
///
/// A READING, NOT A SECOND EVALUATION, and that is the whole design of it. Every semantic this has to
/// honour is already decided above and decided once: a switched-off sound is absent because the loop
/// never gave it a place; a previewed sound is the only live one because preview replaced the
/// membership test outright; the shares are normalised because blendWeights normalised them. So this
/// walks what was built rather than asking the parameters anything, and there is no arrangement of
/// switches under which it can disagree with the sound being played.
///
/// The mask comes from the packing, not from a count: `set` lights the bit for every slot that took a
/// place, so a live sound sitting at a share of exactly zero is LIT AT ZERO rather than dark. That
/// distinction is the readout's whole reason for having a mask — see sound_weights.h.
inline SoundWeights soundWeightsOf(const Character& c) {
	SoundWeights out;
	const size_t count = std::min<size_t>(c.count, kMaxSounds);
	for (size_t i = 0; i < count; ++i) {
		out.set(c.sounds[i].slot, c.sounds[i].weight);
	}
	return out;
}

/// THE TWO EARS' MEAN — what a single readout can honestly say about a blend that is two blends.
///
/// Terrain stereo evaluates the character once per ear with every anchor displaced by that sound's own
/// width, so a wide patch has no head character to read: there are two sets of shares and the middle
/// is not one of them. Averaging is what a meter about a stereo image can honestly be, and at width 0
/// the ears are identical, so this is exactly the mono reading rather than nearly it.
///
/// MEMBERSHIP IS NOT AN EAR'S BUSINESS. Which sounds are live is decided before any displacement — by
/// the switches and by preview — so the two masks are equal by construction and this takes their
/// union rather than choosing: a disagreement would be a bug upstream, and the union is the reading
/// that would still show the player every sound that is sounding.
inline SoundWeights meanSoundWeights(const SoundWeights& left, const SoundWeights& right) {
	SoundWeights mean;
	mean.live = static_cast<uint8_t>(left.live | right.live);
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		mean.weight[slot] = 0.5 * (left.weight[slot] + right.weight[slot]);
	}
	return mean;
}


/// **THE MAPPING BY EAR** — `characterFromParamsAtSide` at −1, 0 or +1. Every reader but a stacked part's stand asks
/// by ear; the side is the displacement's sign, so Left and Right are exact mirrors and Head reads no width at all.
template <typename Read>
Character characterFromParams(Read&& read, OscType type, const AuditionTuning& tuning, AnchorPoint velocity,
                              const SoundTravel& drift, const SoundOrientations& planes, const SoundResidue& residue,
                              const SoundGamma& gamma, const Preview& preview, EarSide ear) {
	return characterFromParamsAtSide(std::forward<Read>(read), type, tuning, velocity, drift, planes, residue, gamma,
	                                 preview, static_cast<double>(static_cast<int8_t>(ear)));
}
} // namespace feathers
