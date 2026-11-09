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

#include "dsp/character_bank.h"

#include "dsp/halfband.h"
#include "character/phi_morph.hpp"
#include "character/phi_stair.hpp"
#include "dsp/slice_build.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace feathers {

namespace {

/// How much longer than the fade itself the guard has to run.
///
/// A voice can be holding up to Halfband4x::kMaxCarry finished samples when a flip lands — the
/// decimator only works in whole groups, so a voice rounds up and keeps the surplus — and it starts
/// fading that far behind. Taken from the decimator rather than written out, because it is the same
/// fact as the one decimate() states about its own block length, and a four repeated in two files is
/// a four that can be corrected in one of them.
constexpr int kCarrySlack = Halfband4x::kMaxCarry + 1;

/// One sound's morph table, built on the plane its own home gives it.
///
/// The basis is derived here rather than carried because the generator is the state of record: a basis
/// that travelled with the character would be one somebody could rotate in place, and a rotated-in-
/// place basis accumulates float error until a retraced breadcrumb lands near where it was rather than
/// on it. Deriving costs a handful of triangles and a dozen cos/sin against a build that already
/// spends hundreds of triangle evaluations, and it only runs when the character actually changes.
/// The plane comes from the rotate and the origin from the residue — sliceFrameFor rather than
/// sliceBasisFor, which is the difference between "which way does travel lead" and "and where does it
/// lead FROM". A frame built without the residue plays the pre-decision A10 character: the sound moves
/// when its plane is re-aimed. Empty residues make the two calls identical, which is why the migration
/// is free and why the blend suite has to check it rather than assume it.
owlet::dsp::PhiMorphWavetable oneMorph(const SliceOrientationPin& fallback, const SoundCoord& s, const PhaseWarp* warp = nullptr) {
	// The two things M is a function of: the turn this sound carries — its own pin, and the character's
	// only when it has none, see SoundCoord — and WHERE IT IS. `sliceHome` rather than a second
	// spelling of that sum, so the plane is derived at exactly the number the builder reads its
	// triangles at; two spellings would agree to the eye and differ in the last place.
	const SliceOrientationPin& pin = s.orientation.pinned ? s.orientation : fallback;
	const double home = sliceHome(s.zone, s.phaseOffset, s.gammaBase);
	// GRIT TRAVELS WITH THE SOUND, not with the blend. buildMorphBlend takes (tables, weights, blue)
	// and has no per-sound carrier, so scaling ampNoise after the blend would be Σw·field with one
	// knob over the sum — field(Σ w·coordinate) in the currency the house rule forbids. Applied here,
	// at each sound's own build, it stays Σ w·(this sound's scaled field).
	const AngleWarpScope likes(warp); // this sound's phase warp on its Morph sites (dsp/phase_warp.h)
	return buildMorphSliced(s.zone, s.phaseOffset, s.travelX, s.travelY,
	                        sliceFrameFor<kMorphTriangles>(rotateFor(pin), home, s.residue.morph),
	                        s.gammaBase, s.gritScale);
}

owlet::dsp::PhiStairParams oneStair(const SliceOrientationPin& fallback, const SoundCoord& s, const PhaseWarp* warp = nullptr) {
	const AngleWarpScope likes(warp); // this sound's phase warp on its Stair sites (dsp/phase_warp.h)
	const SliceOrientationPin& pin = s.orientation.pinned ? s.orientation : fallback;
	const double home = sliceHome(s.zone, s.phaseOffset, s.gammaBase);
	return buildStairSliced(s.zone, s.phaseOffset, s.travelX, s.travelY,
	                        sliceFrameFor<kStairTriangles>(rotateFor(pin), home, s.residue.stair),
	                        s.gammaBase);
}

owlet::dsp::PhiWeaveParams oneWeave(const SliceOrientationPin& fallback, const SoundCoord& s, const PhaseWarp* warp = nullptr) {
	const AngleWarpScope likes(warp); // this sound's phase warp on its Resonate sites (dsp/phase_warp.h)
	const SliceOrientationPin& pin = s.orientation.pinned ? s.orientation : fallback;
	const double home = sliceHome(s.zone, s.phaseOffset, s.gammaBase);
	return buildWeaveSliced(s.zone, s.phaseOffset, s.travelX, s.travelY,
	                        sliceFrameFor<kWeaveTriangles>(rotateFor(pin), home, s.residue.weave),
	                        s.gammaBase);
}

/// The same bits, compared as bits: a −0 is not a +0 and a NaN matches its own payload, so a hit is exactly the
/// input that built the entry.
template <typename T>
bool sameBits(const T& a, const T& b) {
	return std::memcmp(&a, &b, sizeof(T)) == 0;
}

/// `oneWeave(fallback, s)`, through `memo`'s entry for position `i`: built only when one of the inputs `oneWeave`
/// reads differs, bit for bit, from the ones the entry was built from.
const owlet::dsp::PhiWeaveParams& memoWeave(WeaveLawMemo& memo, size_t i, const SliceOrientationPin& fallback,
                                             const SoundCoord& s, const PhaseWarp* warp, uint64_t warpKey) {
	const SliceOrientationPin& pin = s.orientation.pinned ? s.orientation : fallback;
	WeaveLawMemo::Entry& e = memo.entries[i];
	if (e.valid && e.pinned == pin.pinned && sameBits(e.rotate, pin.generator.rotate) && e.zone == s.zone
	    && sameBits(e.phaseOffset, s.phaseOffset) && sameBits(e.gammaBase, s.gammaBase)
	    && sameBits(e.travelX, s.travelX) && sameBits(e.travelY, s.travelY)
	    && sameBits(e.residue.offset, s.residue.weave.offset) && e.warpKey == warpKey) {
		return e.laws;
	}
	e.laws = oneWeave(fallback, s, warp);
	e.warpKey = warpKey;
	e.valid = true;
	e.pinned = pin.pinned;
	e.rotate = pin.generator.rotate;
	e.zone = s.zone;
	e.phaseOffset = s.phaseOffset;
	e.gammaBase = s.gammaBase;
	e.travelX = s.travelX;
	e.travelY = s.travelY;
	e.residue = s.residue.weave;
	++memo.built;
	return e.laws;
}

/// One sound's additive spectrum, through its own frame exactly as the three above build theirs, with its
/// Smoothness knob on the Smoothness field and its six bias knobs on theirs (buildPartialSound).
/// ONE SOUND'S ADDITIVE FRAME: the plane its own home gives it (or its pin's), with its residue. The one derivation, read by
/// the Additive spectrum and the string's material alike, so the two cannot stand on different planes.
PartialBasis partialFrameOf(const SliceOrientationPin& fallback, const SoundCoord& s, double home) {
	const SliceOrientationPin& pin = s.orientation.pinned ? s.orientation : fallback;
	return sliceFrameFor<kPartialTriangles>(rotateFor(pin), home, s.residue.partials);
}

void onePartial(const SliceOrientationPin& fallback, const SoundCoord& s, partials::PartialSpectrum& out,
                const PhaseWarp* warp = nullptr) {
	const double home = sliceHome(s.zone, s.phaseOffset, s.gammaBase);
	buildPartialSound(partialFrameOf(fallback, s, home), home, s.travelX, s.travelY, out, s.smoothnessScale,
	                  s.partialBias, warp);
}

/// One sound's string material at its own coordinate, through the frame its pluck's tone is read through, scalers applied.
StringFields oneString(const SliceOrientationPin& fallback, const SoundCoord& s, const PhaseWarp* warp = nullptr) {
	const double home = sliceHome(s.zone, s.phaseOffset, s.gammaBase);
	return stringFieldsWithOverrides(readStringFields(partialFrameOf(fallback, s, home), home, s.travelX, s.travelY, warp),
	                                 s.stringBias);
}

/// One sound's drum fields at its own coordinate, through its Additive frame (the guests' host rows), scalers applied.
drum::Fields oneDrum(const SliceOrientationPin& fallback, const SoundCoord& s, const PhaseWarp* warp = nullptr) {
	const double home = sliceHome(s.zone, s.phaseOffset, s.gammaBase);
	return drumFieldsWithOverrides(drumFieldsWithOverrides(readDrumFields(partialFrameOf(fallback, s, home), home, s.travelX,
	                                                                     s.travelY, warp),
	                                                       s.drumBias),
	                               s.drumKitBias);
}

} // namespace

float PreparedCharacter::eval(const float* grid, double phase01) const {
	// Stair renders through the 128-slot grid, not the continuous evaluator: the original fixed-point Stair renderer never
	// evaluates continuously — it resamples onto that grid and interpolates between slots — and the
	// band-limiting that imposes is part of how the hardware sounds rather than a limitation to be
	// improved on. Morph is the opposite case, and stays continuous: the firmware renders it by direct
	// per-sample evaluation with no grid and no oversampling, so evaluating it at 4x is already
	// cleaner than the hardware it is copying.
	//
	// The flagship is both, by position (decision V8): an operated build plays its baked 256-slot
	// grid — the operators are IN the slots — and an identity-corner build is the continuous morph
	// path to the bit, which is what makes the q=0/s=0/k=0 anchor structural rather than measured.
	//
	// A VOWEL PUTS A CHARACTER ON THE GRID ROAD WHETHER OR NOT AN OPERATOR DID. The vox crossfade
	// happens before the operator chain (V22c), so even at an identity corner the cycle being played
	// is no longer the plain morph table — it is the blend, and the blend is what the voice baked.
	if (engine == OscEngine::Morph) {
		return readsGrid() ? evalFlagshipGrid(grid, phase01) : evalMorphTable(morph, phase01);
	}
	// A Weave side reads the synth's string and a Partials side sums its partials (the voice's own road, plan Task 9):
	// neither is a table, and the voice never asks. Silence if anything does.
	if (engine == OscEngine::Weave || engine == OscEngine::Partials || engine == OscEngine::String
	    || engine == OscEngine::Drum) {
		return 0.0f;
	}
	// A Shapes side reads its ladders at a mip only the voice knows; the voice calls evalShapesAt and never
	// this. Answering the brightest mip keeps a caller that has not thought about it audible rather than silent.
	if (engine == OscEngine::Shapes) {
		// AN OPERATED MIX IS V5'S GRID (backlog KV), read as V5 reads its own.
		return flagship.operated ? evalFlagshipGrid(grid, phase01) : evalShapesAt(0, phase01);
	}
	return evalStairGrid(grid, phase01);
}

const float* PreparedCharacter::gridFor(int stairMip) const {
	// AN OPERATED SHAPE MIX IS BAKED ON V5'S GRID (backlog KV); the ladders are read per voice.
	if (engine == OscEngine::Shapes && flagship.operated) {
		return flagshipMipSlots(flagship, stairMip);
	}
	// Neither the string, the shape ladders nor the partials is a grid the bank can hand out.
	if (engine == OscEngine::Weave || engine == OscEngine::Shapes || engine == OscEngine::Partials
	    || engine == OscEngine::String || engine == OscEngine::Drum) {
		return nullptr;
	}
	if (engine == OscEngine::Morph) {
		if (vox.audible) {
			// Not the bank's to give — see the header. The voice bakes this one for its own note.
			return nullptr;
		}
		if (flagship.operated) {
			return flagshipMipSlots(flagship, stairMip);
		}
	}
	return stairMipSlots(stair, stairMip);
}

namespace {
/// THE ENGINES THAT CARRY A MORPH TABLE'S MODIFIERS AND V5'S FM RECIPE: Morph (V5 among it) and Shapes, whose
/// build keeps both (decision C/E). One predicate for all six doors
/// below, so a seventh cannot be written asking only half of it.
bool carriesRecipe(OscEngine engine) {
	return engine == OscEngine::Morph || engine == OscEngine::Shapes;
}
} // namespace

float PreparedCharacter::phaseJitter() const {
	return carriesRecipe(engine) ? morph.phaseJitter : 0.0f;
}

float PreparedCharacter::fmDepth() const {
	return carriesRecipe(engine) ? recipe.fmDepth : 0.0f;
}

float PreparedCharacter::fmFrequencyRatio() const {
	return carriesRecipe(engine) ? recipe.fmFrequencyRatio() : 1.0f;
}

float PreparedCharacter::fmStereoWidth() const {
	return carriesRecipe(engine) ? recipe.fmStereoWidth : 0.0f;
}

float PreparedCharacter::fmIndexCyclesFor(double fundamentalHz) const {
	return carriesRecipe(engine) ? recipe.fmIndexCyclesFor(fundamentalHz) : 0.0f;
}

float PreparedCharacter::ampNoise() const {
	return carriesRecipe(engine) ? morph.ampNoise : 0.0f;
}

owlet::dsp::PhiMorphWavetable buildMorphFor(const Character& c) {
	const size_t n = std::min<size_t>(c.count, kMaxSounds);

	// THE PARITY REFERENCE, AND NOTHING ELSE TAKES IT — §2c. The verbatim v1 crossfade was the shipping
	// rule for two sounds until blue shift landed; it is now reachable only from a test or a debug
	// environment, kept so the bit-identity checks keep running against it rather than bit-rotting.
	//
	// What replaced it is one rule at every count. The seam §7.3 named — factors jumping from B's to a
	// weighted average, and the directional overshoot vanishing, at the moment a third sound is
	// switched on — does not exist under a uniform rule, because there is nothing to switch between.
	if (c.v1Crossfade && n == 2) {
		return buildMorphXfade(oneMorph(c.orientation, c.sounds[0], c.likes.of(c.sounds[0].slot)),
		                       oneMorph(c.orientation, c.sounds[1], c.likes.of(c.sounds[1].slot)), c.xf);
	}

	// N tables on the stack rather than a vector: this runs on the audio thread, where allocating is
	// forbidden, and eight of them is a few kilobytes against a build that is already the expensive
	// part. A count of zero falls through with empty spans and blends to a zeroed table — every sound
	// switched off is silence, which is the honest reading of an empty constellation.
	std::array<owlet::dsp::PhiMorphWavetable, kMaxSounds> tables{};
	std::array<double, kMaxSounds> weights{};
	std::array<double, kMaxSounds> approach{};
	std::array<double, kMaxSounds> weightRate{};
	for (size_t i = 0; i < n; ++i) {
		tables[i] = oneMorph(c.orientation, c.sounds[i], c.likes.of(c.sounds[i].slot));
		weights[i] = c.sounds[i].weight;
		approach[i] = c.sounds[i].approach;
		weightRate[i] = c.sounds[i].weightRate;
	}
	// All zero unless the puck is moving and the audition has a scale up, in which case the blend
	// takes its old path exactly — see BlueShift::active().
	const BlueShift blue { { approach.data(), n }, { weightRate.data(), n }, c.blueOvershoot };
	return buildMorphBlend({ tables.data(), n }, { weights.data(), n }, blue);
}

owlet::dsp::PhiStairParams buildStairFor(const Character& c) {
	const size_t n = std::min<size_t>(c.count, kMaxSounds);
	if (c.v1Crossfade && n == 2) {
		return buildStairXfade(oneStair(c.orientation, c.sounds[0], c.likes.of(c.sounds[0].slot)),
		                       oneStair(c.orientation, c.sounds[1], c.likes.of(c.sounds[1].slot)), c.xf);
	}

	std::array<owlet::dsp::PhiStairParams, kMaxSounds> params{};
	std::array<double, kMaxSounds> weights{};
	std::array<double, kMaxSounds> approach{};
	std::array<double, kMaxSounds> weightRate{};
	for (size_t i = 0; i < n; ++i) {
		params[i] = oneStair(c.orientation, c.sounds[i], c.likes.of(c.sounds[i].slot));
		weights[i] = c.sounds[i].weight;
		approach[i] = c.sounds[i].approach;
		weightRate[i] = c.sounds[i].weightRate;
	}
	// The same terms the morph side gets. They were computed for both engines all along — this is the
	// call that had been left out, so blue shift was a morph-only feature by omission rather than by
	// any decision.
	const BlueShift blue { { approach.data(), n }, { weightRate.data(), n }, c.blueOvershoot };
	return buildStairBlend({ params.data(), n }, { weights.data(), n }, blue);
}

namespace {

/// THE BLEND'S LEADERS AND ITS CROSSFADE, from the weights alone — `buildWeaveBlend`'s ranking and its cf, the one copy of
/// both, so the laws' blend and `weaveCrossfadeFor` (which needs no laws) cannot disagree by a bit. `shared` false: nothing
/// to share (one sound, or a sum that is not positive), and the crossfade is the default 0.
struct WeaveLead {
	bool shared = false;
	size_t first = 0;
	size_t second = 0;
	float crossfade = 0.0f;
};

WeaveLead weaveLeadOf(std::span<const double> weights) {
	WeaveLead lead;
	const size_t n = weights.size();
	if (n == 0) {
		return lead;
	}
	double sum = 0.0;
	for (size_t k = 0; k < n; ++k) {
		sum += weights[k];
	}
	if (n == 1 || !(sum > 0.0)) {
		return lead;
	}
	lead.shared = true;
	const auto share = [&](size_t k) { return weights[k] / sum; };
	// The three heaviest, the earlier index winning a tie so the choice is reproducible. The third is
	// "none" (weight 0) when there are only two.
	size_t first = 0;
	for (size_t k = 1; k < n; ++k) {
		if (weights[k] > weights[first]) {
			first = k;
		}
	}
	size_t second = first == 0 ? 1 : 0;
	for (size_t k = 0; k < n; ++k) {
		if (k != first && weights[k] > weights[second]) {
			second = k;
		}
	}
	double third = 0.0;
	for (size_t k = 0; k < n; ++k) {
		if (k != first && k != second) {
			third = std::max(third, share(k));
		}
	}
	// cf = (w2 - w3) / ((w1 - w3) + (w2 - w3)) — THE CONSTRUCTION, explained at its use in `buildWeaveBlend`.
	const double lead1 = share(first) - third;
	const double lead2 = share(second) - third;
	const double leads = lead1 + lead2;
	lead.first = first;
	lead.second = second;
	lead.crossfade = leads > 0.0 ? static_cast<float>(lead2 / leads) : 0.0f;
	return lead;
}

} // namespace

WeaveCharacter buildWeaveBlend(std::span<const owlet::dsp::PhiWeaveParams> laws, std::span<const double> weights) {
	using owlet::dsp::PhiWeaveParams;
	WeaveCharacter out{};
	const size_t n = std::min(laws.size(), weights.size());
	if (n == 0) {
		return out;
	}
	// THE WEIGHTS AS SHARES: normalised by their sum, so the blend is convex whatever the caller hands
	// in (the constellation's are already a partition of one, and dividing by 1.0 is exact). Nothing to
	// share — every weight zero, or a sum that is not positive — plays the first sound as itself.
	double sum = 0.0;
	for (size_t k = 0; k < n; ++k) {
		sum += weights[k];
	}
	if (n == 1 || !(sum > 0.0)) {
		out.a = laws[0];
		out.b = laws[0];
		return out;
	}
	const auto share = [&](size_t k) { return weights[k] / sum; };

	// Every field, blended — in the same order for every sound, so a sound with all the weight comes
	// out as itself exactly (share 1 and the rest 0: x·1 + 0 + ... = x).
	PhiWeaveParams blend{};
	for (size_t k = 0; k < n; ++k) {
		const auto w = static_cast<float>(share(k));
		const PhiWeaveParams& p = laws[k];
		for (int i = 0; i < owlet::dsp::kPhiWeaveNumNodes; i++) {
			blend.stiffness[i] += w * p.stiffness[i];
			blend.coupling[i] += w * p.coupling[i];
			blend.damping[i] += w * p.damping[i];
			blend.bowBalance[i] += w * p.bowBalance[i];
			blend.home[i] += w * p.home[i];
		}
		blend.bowDepth += w * p.bowDepth;
		blend.bowRate += w * p.bowRate;
		blend.bowPos += w * p.bowPos;
		blend.bowSpread += w * p.bowSpread;
		blend.travelRate += w * p.travelRate;
		blend.pluckPos += w * p.pluckPos;
		blend.pluckWidth += w * p.pluckWidth;
		blend.pluckAmp += w * p.pluckAmp;
		blend.morphBowGain += w * p.morphBowGain;
		blend.outGain += w * p.outGain;
		blend.shimmerAlpha += w * p.shimmerAlpha;
	}

	// The three heaviest and the crossfade between the first two: `weaveLeadOf`, the one copy (`weaveCrossfadeFor` reads
	// it without building any laws).
	const WeaveLead lead = weaveLeadOf(weights.first(n));
	// THE CONSTRUCTION (approved 2026-09-25; the third-sound term decided 2026-09-26). Both sides carry the
	// SAME blended fields; they differ only in the bow mode, the one law a blend cannot average: A takes
	// the heaviest sound's, B the second heaviest's, and the string crossfades their forces by cf
	// (weave_string.cpp: (1 - cf) shapeA + cf shapeB), so a mode change is carried by the string's own
	// blend, never switched. cf is the second's lead over the third, as a share of the two leaders' leads:
	//
	//     cf = (w2 - w3) / ((w1 - w3) + (w2 - w3))
	//
	// TWO SOUNDS REDUCE TO THE FIRMWARE'S A/B. w3 = 0, so cf = w2 / (w1 + w2) = w1 at weights summing to
	// one. The effective laws (1 - cf) A + cf B are the blend, which is (1 - cf) s0 + cf s1, the firmware's
	// lerp between its two ends; and whichever sound is heavier sits on A, so the bow is
	// (1 - w1) shape0 + w1 shape1 as well.
	//
	// EVERY DOMINANCE SWAP IS SEAMLESS, and the w3 term is what makes that true past two sounds.
	// - The 1st and 2nd trading places: w1 = w2 there, so cf = 0.5 on both sides of the swap. A and B
	//   exchange modes at the only cf where the exchange changes nothing (0.5 shapeA + 0.5 shapeB is
	//   symmetric).
	// - The 2nd and 3rd trading places: w2 = w3 there, so cf = 0. B's mode changes while it carries no
	//   weight. (w2 / (w1 + w2) alone switched B's mode at cf of about 1/3 there: a step in the bow.)
	// The fields never move at either, being the blend on both sides. And cf's MOTION, which bows the
	// string (the morph-bow), is continuous across both.
	out.a = blend;
	out.b = blend;
	out.a.bowMode = laws[lead.first].bowMode;
	out.b.bowMode = laws[lead.second].bowMode;
	out.crossfade = lead.crossfade;
	return out;
}

WeaveCharacter buildWeaveFor(const Character& c, WeaveLawMemo* memo) {
	const size_t n = std::min<size_t>(c.count, kMaxSounds);
	const auto lawsOf = [&](size_t i) {
		const PhaseWarp* warp = c.likes.of(c.sounds[i].slot);
		return memo != nullptr ? memoWeave(*memo, i, c.orientation, c.sounds[i], warp, c.likes.keyOf(c.sounds[i].slot))
		                       : oneWeave(c.orientation, c.sounds[i], warp);
	};
	// THE PARITY REFERENCE, as buildStairFor takes it: two sounds are the firmware's two ends at `xf`.
	if (c.v1Crossfade && n == 2) {
		return { lawsOf(0), lawsOf(1), c.xf };
	}
	// Blue shift is not applied: its halves lean a RECIPE's brightness and push its shape, and the
	// ring's laws have neither — what moving does to this engine is the morph-bow the crossfade drives.
	std::array<owlet::dsp::PhiWeaveParams, kMaxSounds> laws{};
	std::array<double, kMaxSounds> weights{};
	for (size_t i = 0; i < n; ++i) {
		laws[i] = lawsOf(i);
		weights[i] = c.sounds[i].weight;
	}
	return buildWeaveBlend({ laws.data(), n }, { weights.data(), n });
}

float weaveCrossfadeFor(const Character& c) {
	// `buildWeaveFor(c).crossfade` without the laws: the parity reference's `xf`, else the blend's cf from the weights alone
	// (`weaveLeadOf`, which `buildWeaveBlend` reads too).
	const size_t n = std::min<size_t>(c.count, kMaxSounds);
	if (c.v1Crossfade && n == 2) {
		return c.xf;
	}
	std::array<double, kMaxSounds> weights{};
	for (size_t i = 0; i < n; ++i) {
		weights[i] = c.sounds[i].weight;
	}
	return weaveLeadOf({ weights.data(), n }).crossfade;
}

void buildPartialsFor(const Character& c, partials::PartialSpectrum& out, PartialsScratch& scratch) {
	out = {};
	const size_t n = std::min<size_t>(c.count, kMaxSounds);
	if (n == 0) {
		// Every sound switched off: silence, the honest reading of an empty constellation.
		return;
	}
	// THE WEIGHTS AS SHARES, buildWeaveBlend's rule: the constellation's are already a partition of one (the
	// mapping normalises them, constellation.h), and dividing by their sum makes the blend convex whatever a
	// caller hands in. That matters more here than on any other engine, because the RATIOS blend too:
	// weights summing to w would scale every partial's frequency by w.
	//
	// SANITISED FIRST: a negative or NaN weight counts as 0, and a sum that is not finite and positive (an
	// infinite weight among them) is nothing to share, which plays the first sound. The shares below come from the
	// same sanitised weights, so they are always in [0, 1] and sum to 1.
	const auto clean = [](double w) { return w > 0.0 ? w : 0.0; }; // NaN fails `w > 0`
	double sum = 0.0;
	for (size_t i = 0; i < n; ++i) {
		sum += clean(c.sounds[i].weight);
	}
	const bool shared = std::isfinite(sum) && sum > 0.0;
	// ONE SHAPE PER BUILD: it is engine-wide, so it is built once, and only when some sound asks for it. Local to
	// this call on purpose: a "have I built it" carried across builds would keep the LAST build's shape.
	partials::PartialSpectrum& shape = scratch.shape;
	bool haveShape = false;
	// Blue shift is not applied, Resonate's reason: its halves lean a morph recipe's brightness and push its
	// waypoints, and a spectrum has neither. The v1 crossfade is not a reference here either: there is no
	// firmware Additive to be parity with.
	partials::PartialSpectrum& sound = scratch.sound; // overwritten whole by onePartial for every sound built
	scratch.soundsBuilt = 0;
	for (size_t i = 0; i < n; ++i) {
		const SoundCoord& s = c.sounds[i];
		const double w = shared ? clean(s.weight) / sum : (i == 0 ? 1.0 : 0.0);
		// A SOUND WITH NO SHARE IS NOT BUILT: it would add 0·x, and buildSound's output is finite, so that is +0 to
		// every member, exactly. Most of a constellation is far from the puck most of the time.
		if (w == 0.0) {
			continue;
		}
		// THE ORDER, per sound (plan Task 7): the fields at the sound's own coordinate through its own frame, the
		// Smoothness and bias knobs on their fields, buildSound (onePartial); then the Shape blend at this sound's
		// amount, renormalised (skipped entirely at 0, so a zero amount is the terrain to the bit); then the puck's
		// weight. Blending the shape in AFTER the weight would hand one sound's amount to every sound.
		onePartial(c.orientation, s, sound, c.likes.of(s.slot));
		++scratch.soundsBuilt;
		if (s.shapeAmount > 0.0f) {
			if (!haveShape) {
				partials::buildShapeSpectrum(c.additiveShape, shape);
				haveShape = true;
			}
			partials::blendTowardShape(sound, shape, s.shapeAmount);
		}
		partials::accumulate(out, sound, w);
	}
	// RENORMALISE THE BLEND (plan Task 7): each sound was normalised alone, but a blend is not — two disjoint
	// spectra dip −3 dB at 50/50, and the voice multiplies the blended envelope by the blended fine structure,
	// which adds cross terms. V5 normalises its BLENDED recipe for the same reason (flagshipNormalised).
	partials::normaliseRss(out);
}

void weaveRestCycle(WeaveModel model, const WeaveCharacter& w, float* out) {
	// The two sides at the crossfade, in the string's own form (cfInv * a + cf * b, weave_string.cpp).
	owlet::dsp::PhiWeaveParams laws = w.a;
	const float cf = std::clamp(w.crossfade, 0.0f, 1.0f);
	const float cfInv = 1.0f - cf;
	for (int i = 0; i < owlet::dsp::kPhiWeaveNumNodes; i++) {
		laws.stiffness[i] = cfInv * w.a.stiffness[i] + cf * w.b.stiffness[i];
		laws.coupling[i] = cfInv * w.a.coupling[i] + cf * w.b.coupling[i];
		laws.damping[i] = cfInv * w.a.damping[i] + cf * w.b.damping[i];
		laws.bowBalance[i] = cfInv * w.a.bowBalance[i] + cf * w.b.bowBalance[i];
		laws.home[i] = cfInv * w.a.home[i] + cf * w.b.home[i];
	}
	const float outGain = cfInv * w.a.outGain + cf * w.b.outGain;
	WeaveString::restScanPath(model, laws, out);

	// THE AGC SETTLED ON THAT PATH — restated from WeaveString::reset (its rule without the slew: 0.55 of
	// the 0.5 reference over the RMS, floored at 0.20; the reference over the peak, floored at 0.35; the
	// laws' output gain; the ±1 ceiling). test_resonate_engine pins this against a freshly reset
	// string's own table, so the restatement cannot drift from the string unnoticed.
	constexpr int kN = WeaveString::kNodes;
	float peak = 0.0f;
	float sumSq = 0.0f;
	for (int i = 0; i < kN; ++i) {
		peak = std::max(peak, std::abs(out[i]));
		sumSq += out[i] * out[i];
	}
	const float rms = std::sqrt(sumSq * (1.0f / static_cast<float>(kN)));
	const float scaleByRms = 0.55f * 0.5f / std::max(rms, 0.20f);
	const float scaleByPeak = 0.5f / std::max(peak, 0.35f);
	const float scale = outGain * std::min(scaleByRms, scaleByPeak);
	for (int i = 0; i < kN; ++i) {
		out[i] = std::clamp(out[i] * scale, -1.0f, 1.0f);
	}
}

void shapeWeaveLaws(const ResonateShaping& shaping, owlet::dsp::PhiWeaveParams& laws) {
	// The laws builder's own clamps (character/phi_weave.cpp), restated: a scaled law may reach them and
	// stop, never pass them. The shimmer coefficient has no clamp in the builder (it spans 0.07..0.30 by
	// construction); a one-pole coefficient is stable on (0, 1], and a quarter of the builder's floor is
	// the least it may take here.
	constexpr float kStiffMin = 0.016f;
	constexpr float kStiffMax = 0.12f;
	constexpr float kCoupleMin = 0.004f;
	constexpr float kCoupleMax = 0.45f;
	constexpr float kDampMin = 0.006f;
	constexpr float kDampMax = 0.09f;
	constexpr float kShimmerMin = 0.07f * 0.25f;
	constexpr float kShimmerMax = 1.0f;
	constexpr float kPluckMax = 2.0f; // WeaveString::pluck's own clamp on an amplitude
	for (int i = 0; i < owlet::dsp::kPhiWeaveNumNodes; i++) {
		if (shaping.stiffness != 1.0f) {
			laws.stiffness[i] = std::clamp(laws.stiffness[i] * shaping.stiffness, kStiffMin, kStiffMax);
		}
		if (shaping.coupling != 1.0f) {
			laws.coupling[i] = std::clamp(laws.coupling[i] * shaping.coupling, kCoupleMin, kCoupleMax);
		}
		if (shaping.decay != 1.0f && shaping.decay > 0.0f) {
			laws.damping[i] = std::clamp(laws.damping[i] / shaping.decay, kDampMin, kDampMax);
			// The builder's own energy-balance law on the new damping: 0.6 min(1, sqrt(d / 0.0255)).
			laws.bowBalance[i] = 0.60f * std::min(1.0f, std::sqrt(laws.damping[i] * (1.0f / 0.0255f)));
		}
	}
	if (shaping.shimmer != 1.0f) {
		laws.shimmerAlpha = std::clamp(laws.shimmerAlpha * shaping.shimmer, kShimmerMin, kShimmerMax);
	}
	if (shaping.pluck != 1.0f) {
		laws.pluckAmp = std::clamp(laws.pluckAmp * shaping.pluck, 0.0f, kPluckMax);
	}
	if (shaping.travel != 1.0f) {
		laws.travelRate *= shaping.travel;
	}
	if (shaping.bowMode > 0 && shaping.bowMode <= owlet::dsp::kWeaveBowModeCount) {
		laws.bowMode = static_cast<owlet::dsp::WeaveBow>(shaping.bowMode - 1);
	}
}

CharacterBank::CharacterBank() {
	setSampleRate(hostSampleRate_);
}

void CharacterBank::setSampleRate(double hostSampleRate) {
	// THE SHAPE LADDERS, built here (and so at construction) so no note ever pays for them on the audio thread.
	warmShapeTables();
	hostSampleRate_ = hostSampleRate;
	// At least one sample, so a fade is never a division by zero however absurd the rate.
	fadeLengthSamples_ =
	    std::max(1, static_cast<int>(std::lround(kCharacterFadeSeconds * hostSampleRate)));

	// A guard already running is protecting fades that were armed at the OLD length, and those still
	// take the same number of samples to drain: a voice counts its own fade in evaluation samples,
	// and changing the rate does not touch that count. So the guard is widened if the new length is
	// the longer one, and otherwise left exactly where it was.
	//
	// It is deliberately NOT rescaled down to the new length, and deliberately not cleared. Either
	// would retire the guard while a voice was still mixing the table it fades from, and a rebuild
	// into that slot is the one thing this exists to prevent. In practice a caller resets before
	// changing rate, which puts this out of reach; it is written to hold on its own terms rather than
	// on the caller's.
	if (fadeGuardLeft_ > 0) {
		fadeGuardLeft_ = std::max(fadeGuardLeft_, fadeLengthSamples_ + kCarrySlack);
	}
}

void CharacterBank::setCharacter(const Character& c) {
	request(c, nullptr);
}

void CharacterBank::setCharacterStereo(const Character& left, const Character& right) {
	request(left, &right);
}

void CharacterBank::request(const Character& left, const Character* right) {
	// A host that re-sends its whole parameter state every block asks for the character it already
	// has, every block. Rebuilding it would be waste; ARMING A FADE for it would be worse, because a
	// fade from a table to itself is inaudible but still holds the deferral gate shut, and a
	// character that then really did change would queue behind it forever.
	//
	// A mono request never matches a live stereo pair, whatever its character says: the second ear
	// is real prepared state, and collapsing back to one table is a rebuild like any other.
	const bool stereo = right != nullptr;
	if (haveTable_ && stereo == liveStereo_ && left == live_[0] && (!stereo || *right == live_[1])) {
		havePending_ = false;
		return;
	}
	if (fadeGuardLeft_ > 0) {
		pending_[0] = left;
		if (stereo) {
			pending_[1] = *right;
		}
		pendingStereo_ = stereo;
		havePending_ = true;
		return;
	}
	build(left, right);
}

StringFields buildStringFieldsFor(const Character& c) {
	const size_t n = std::min<size_t>(c.count, kMaxSounds);
	if (n == 0) {
		return {};
	}
	// buildPartialsFor's shares exactly (sanitised, over their sum; a bad sum plays the first sound), so the material
	// and the tone are blended by the same weights.
	const auto clean = [](double w) { return w > 0.0 ? w : 0.0; };
	double sum = 0.0;
	for (size_t i = 0; i < n; ++i) {
		sum += clean(c.sounds[i].weight);
	}
	const bool shared = std::isfinite(sum) && sum > 0.0;
	double acc[5] {};
	for (size_t i = 0; i < n; ++i) {
		const SoundCoord& s = c.sounds[i];
		const double w = shared ? clean(s.weight) / sum : (i == 0 ? 1.0 : 0.0);
		if (w == 0.0) {
			continue;
		}
		const StringFields f = oneString(c.orientation, s, c.likes.of(s.slot));
		acc[0] += w * f.brightness;
		acc[1] += w * f.sustain;
		acc[2] += w * f.pick;
		acc[3] += w * f.noise;
		acc[4] += w * f.stretch;
	}
	StringFields out;
	out.brightness = static_cast<float>(acc[0]);
	out.sustain = static_cast<float>(acc[1]);
	out.pick = static_cast<float>(acc[2]);
	out.noise = static_cast<float>(acc[3]);
	out.stretch = static_cast<float>(acc[4]);
	return out;
}

DrumCharacter buildDrumFor(const Character& c) {
	DrumCharacter out;
	out.kit = c.drumKit;
	const size_t n = std::min<size_t>(c.count, kMaxSounds);
	if (c.drumKit) {
		// EVERY PAD AT ITS OWN PLACE, NEVER BLENDED (drum proposal §3.3): pad N is sound N, addressed by its slot. A slot
		// with no live sound keeps the default fields.
		for (size_t i = 0; i < n; ++i) {
			const SoundCoord& s = c.sounds[i];
			out.pads[s.slot] = oneDrum(c.orientation, s, c.likes.of(s.slot));
			// ITS LAYER AT ITS OWN PLACE: the pad's, moved by the offset vector (so it follows the pad's drift, the Walk and every
			// modulation of them), the pad's scalers and the kit's trims applied as to the pad.
			if (s.drumLayer) {
				SoundCoord at = s;
				at.travelX += s.drumLayerX;
				at.travelY += s.drumLayerY;
				out.layers[s.slot] = oneDrum(c.orientation, at, c.likes.of(s.slot));
			}
		}
		return out;
	}
	if (n == 0) {
		return out;
	}
	// PITCHED: the live sounds blended field by field, buildPartialsFor's shares exactly (the house rule: blend the fields,
	// never the coordinates), into pad 0.
	const auto clean = [](double w) { return w > 0.0 ? w : 0.0; };
	double sum = 0.0;
	for (size_t i = 0; i < n; ++i) {
		sum += clean(c.sounds[i].weight);
	}
	const bool shared = std::isfinite(sum) && sum > 0.0;
	double acc[7] {};
	for (size_t i = 0; i < n; ++i) {
		const SoundCoord& s = c.sounds[i];
		const double w = shared ? clean(s.weight) / sum : (i == 0 ? 1.0 : 0.0);
		if (w == 0.0) {
			continue;
		}
		const drum::Fields f = oneDrum(c.orientation, s, c.likes.of(s.slot));
		acc[0] += w * f.pitch;
		acc[1] += w * f.length;
		acc[2] += w * f.sweep;
		acc[3] += w * f.tone;
		acc[4] += w * f.noise;
		acc[5] += w * f.snap;
		acc[6] += w * f.metal;
	}
	drum::Fields& p = out.pads[0];
	p.pitch = static_cast<float>(acc[0]);
	p.length = static_cast<float>(acc[1]);
	p.sweep = static_cast<float>(acc[2]);
	p.tone = static_cast<float>(acc[3]);
	p.noise = static_cast<float>(acc[4]);
	p.snap = static_cast<float>(acc[5]);
	p.metal = static_cast<float>(acc[6]);
	return out;
}

void CharacterBank::buildOne(const Character& c, PreparedCharacter& next, PartialsScratch& scratch) {
	next.engine = c.engine;
	next.phasing = {}; // a slot rebuilt into another engine keeps no stale phasing (the Partials branch below sets it)
	next.smooth.live = false; // read only where a vowel is baked from this build (the flagship branch below fills it)
	// ONE PLANE PER SOUND, derived from that sound's own home unless a pin says otherwise — and the
	// build itself is the free function above, shared with the editor's wave display so that what is
	// drawn and what is played cannot be two readings of the same character.
	if (c.engine == OscEngine::Morph) {
		if (c.flagship) {
			// The flagship: the same morph blend, then the operator tier baked over it. The recipe's
			// morph table lands in `morph` so the identity corner — operated false — renders exactly
			// the build a plain morph character would have made, and the fade machinery always has a
			// morph table to mix whichever way this build came out.
			const FlagshipRecipe recipe = buildFlagshipFor(c);
			next.morph = recipe.morph;
			// THE BANK'S GRID STAYS VOX-FREE, deliberately: it is shared by every voice at every
			// pitch and a vox cycle is a fact about ONE note (V22a), so there is no such thing as
			// the bank's version of it. What the bank can do is settle the position-pure half — the
			// vowel and how present it is — and keep the recipe, so a voice in vowel country has
			// everything it needs to bake for its own note and nothing else has to change.
			next.vox = buildVoxFor(c);
			// AND THE NOTE-FREE HALF OF EVERY VOICE'S BAKE (backlog PB): the smooth source at each slot, read here once
			// for the voices that will bake this vowel.
			// AND THE BANK'S OWN GRID ONLY WHERE A VOICE CAN READ IT (backlog PB). A build with an audible
			// vowel is never read through the bank — `gridFor` answers null and every voice plays its own bake — and a
			// PreparedCharacter never changes after its build, so that build's grid is unread for its whole life. The
			// moment the vowel goes, that is a NEW build, with `vox.audible` false, and it bakes its grid here before any
			// voice can sync to it. The grid's bits in a skipped build are whatever the slot last held, and nothing reads
			// them: `gridFor` is the one door and it asks `vox.audible` first. `operated` is still set, for `readsGrid`.
			if (next.vox.audible) {
				fillFlagshipSlotSmooth(recipe, next.smooth);
				next.flagship.operated = recipe.operated;
			}
			else {
				bakeFlagshipTable(recipe, next.flagship);
			}
			next.recipe = recipe;
		}
		else {
			next.morph = buildMorphFor(c);
			// The slots are reused — a stale operated flag from a previous flagship build in this slot
			// would send a plain morph voice into the wrong grid.
			next.flagship.operated = false;
			next.vox = {};
			// AND THE RECIPE (backlog EH, shape 6): the four FM accessors gate on the ENGINE alone, so a
			// plain morph build in a slot that last held a flagship kept that flagship's FM depth, ratio
			// and width and rendered them — FM leaking onto a sound that declares none.
			next.recipe = {};
		}
	}
	else if (c.engine == OscEngine::Shapes) {
		// V5'S RECIPE, NOT ITS GRID (decision C/E): the recipe is where the FM triple, the warp and the morph table's
		// modifiers live, all built per sound and blended by the puck exactly as on V5 — so FM, Warp and Grit
		// are V5's to the bit. The operator grid is never baked: the voice reads the band-limited shape ladders.
		FlagshipRecipe recipe = buildFlagshipFor(c);
		next.morph = recipe.morph;
		next.vox = {};
		next.shapes = normaliseShapes(c.shapes);
		// THE OPERATOR TIER (backlog KV): where a Quantize, Sync, Feedback or Smoothness knob is above neutral, the mix
		// is operated as V5's cycle is and baked on V5's grid, which the voice then plays (gridFor). Otherwise nothing is
		// baked and the voice reads the ladders, exactly as before.
		if (shapesOperated(recipe)) {
			FlagshipSource source;
			source.shapes = &next.shapes;
			source.shapesMip = kShapesBakeMip;
			prepareAndBakeFlagshipLadder(recipe, source, next.flagship.slots, next.flagship.mip1, next.flagship.mip2);
			next.flagship.operated = true;
		}
		else {
			next.flagship.operated = false;
		}
		next.recipe = recipe;
	}
	else if (c.engine == OscEngine::Partials) {
		// THE BLENDED SPECTRUM, every sound through its own plane (buildPartialsFor). No grid, no morph table, no
		// recipe: the flags and payloads that would let a stale slot leak into the voice are cleared, as the
		// branches around this one clear theirs.
		buildPartialsFor(c, next.partials, scratch);
		next.phasing = c.phasing;
		next.flagship.operated = false;
		next.vox = {};
		next.recipe = {};
	}
	else if (c.engine == OscEngine::String) {
		// THE PLUCK'S TONE IS ADDITIVE'S SPECTRUM AT THE SAME PLACE (KS design §2.2), built by the one reading of it; the
		// string's material beside it. A voice reads both at note-on and the material on every block.
		buildPartialsFor(c, next.partials, scratch);
		next.string = buildStringFieldsFor(c);
		next.flagship.operated = false;
		next.vox = {};
		next.recipe = {};
	}
	else if (c.engine == OscEngine::Drum) {
		// THE PADS' SEVEN FIELDS (drum proposal §3): seven triangle reads a pad, nothing baked. A voice reads its pad's on every
		// control block, so a moving place or Walk moves a ringing hit.
		next.drum = buildDrumFor(c);
		next.flagship.operated = false;
		next.vox = {};
		next.recipe = {};
	}
	else if (c.engine == OscEngine::Weave) {
		// NOTHING TO BUILD: the voices scan the synth's string, whose laws the synth sets from buildWeaveFor.
		// The build still happens, so an engine switch INTO Resonate flips the bank and the voices fade
		// from the old table to the string rather than stepping.
		next.flagship.operated = false;
		next.vox = {};
		next.recipe = {};
	}
	else {
		next.stair = buildStairTable(buildStairFor(c));
		next.flagship.operated = false;
		next.vox = {};
	}
}

void CharacterBank::prepare(int slot, int ear) const {
	if (!unprepared_[slot][ear]) {
		return;
	}
	unprepared_[slot][ear] = false;
	const uint64_t serial = tables_[slot][ear].buildSerial;
	buildOne((*slotCharacter_)[static_cast<size_t>(slot)][static_cast<size_t>(ear)], tables_[slot][ear], partialsScratch_);
	tables_[slot][ear].buildSerial = serial; // buildOne leaves it, but the identity is the record's, said here
	++tablesMade_;
}

void CharacterBank::build(const Character& left, const Character* right) {
	const int slot = 1 - current_;
	const bool stereo = right != nullptr;
	// THE BUILD IS A RECORD, AND THE TABLE IS MADE WHEN SOMETHING READS IT (backlog PB): every
	// accessor below calls `prepare` first. A character moved with no voice sounding (Pos X automated over silence)
	// used to bake a table per interval that nothing read; now the bookkeeping — the flip, the generation, the serial,
	// the guard, the stereo window — runs exactly as before, and the table is built from the character recorded here
	// the first time a voice (or anything else) asks for it. buildOne is a function of the character alone, so a table
	// built late is the table that would have been built on time, bit for bit; its serial is stamped here, so it
	// identifies the build, not the moment it was made.
	(*slotCharacter_)[static_cast<size_t>(slot)][0] = left;
	unprepared_[slot][0] = true;
	// THE SERIAL IS STAMPED HERE, where the counter lives, and per EAR rather than per build: the
	// two ears are two prepared tables and a cache keyed on one must not match the other.
	tables_[slot][0].buildSerial = ++buildSerial_;
	if (stereo) {
		// The second ear only when asked for: a mono build — every patch at width 0, every frozen
		// engine — pays exactly what it always paid, and its slot's stale second ear is dead storage
		// the accessors never hand out.
		(*slotCharacter_)[static_cast<size_t>(slot)][1] = *right;
		unprepared_[slot][1] = true;
		tables_[slot][1].buildSerial = ++buildSerial_;
	}
	else {
		unprepared_[slot][1] = false; // dead storage: the accessors answer ear 0 for a mono slot
	}
	slotStereo_[slot] = stereo;

	current_ = 1 - current_;
	live_[0] = left;
	if (stereo) {
		live_[1] = *right;
	}
	liveStereo_ = stereo;
	havePending_ = false;
	generation_++;

	if (!haveTable_) {
		// Nothing to fade from on the first build, and no guard to hold: the second character can
		// land in the very next sample.
		haveTable_ = true;
		return;
	}
	fadeGuardLeft_ = fadeLengthSamples_ + kCarrySlack;
}

int CharacterBank::samplesUntilPendingBuild() const {
	if (!havePending_) {
		return std::numeric_limits<int>::max();
	}
	// A request is only ever held while the guard is running — setCharacter builds on the spot
	// otherwise — so this is positive every time it is asked. The floor is there so that a caller
	// honouring the answer can always render at least one sample, rather than spinning on a zero it
	// should not be able to see.
	return std::max(1, fadeGuardLeft_);
}

void CharacterBank::advance(int hostSamples) {
	if (fadeGuardLeft_ > 0) {
		fadeGuardLeft_ = std::max(0, fadeGuardLeft_ - hostSamples);
	}
	if (fadeGuardLeft_ == 0 && havePending_) {
		// Deliberately built here, on the audio thread, between one slice and the next. That is the
		// same context every other build runs in — never inside the sample loop, and never off-thread
		// where it would need synchronising against the voices reading the slots.
		build(pending_[0], pendingStereo_ ? &pending_[1] : nullptr);
	}
}

void CharacterBank::clearModifiersForTest() {
	// A table not yet made would be made later WITH its modifiers, so every recorded build is made first.
	for (int slot = 0; slot < 2; ++slot) {
		prepare(slot, 0);
		prepare(slot, 1);
	}
	for (auto& slot : tables_) {
		for (PreparedCharacter& p : slot) {
			p.morph.phaseJitter = 0.0f;
			p.morph.ampNoise = 0.0f;
		}
	}
}

//-----------//
// StandBank //
//-----------//

StandBank::StandBank() {
	setSampleRate(hostSampleRate_);
}

void StandBank::setSampleRate(double hostSampleRate) {
	warmShapeTables();
	// `CharacterBank::setSampleRate`'s rule, and its reasons: at least a sample, and a running guard widened, never
	// shortened.
	hostSampleRate_ = hostSampleRate;
	fadeLengthSamples_ = std::max(1, static_cast<int>(std::lround(kCharacterFadeSeconds * hostSampleRate)));
	if (fadeGuardLeft_ > 0) {
		fadeGuardLeft_ = std::max(fadeGuardLeft_, fadeLengthSamples_ + kCarrySlack);
	}
}

void StandBank::setCharacter(const Character& c) {
	// `CharacterBank::request` for one ear: the live character is dropped, a guarded one deferred.
	if (haveTable_ && c == live_) {
		havePending_ = false;
		return;
	}
	if (fadeGuardLeft_ > 0) {
		pending_ = c;
		havePending_ = true;
		return;
	}
	build(c);
}

void StandBank::build(const Character& c) {
	PreparedCharacter& next = tables_[1 - current_];
	CharacterBank::buildOne(c, next, partialsScratch_);
	next.buildSerial = ++buildSerial_;
	current_ = 1 - current_;
	live_ = c;
	havePending_ = false;
	generation_++;
	if (!haveTable_) {
		haveTable_ = true;
		return;
	}
	fadeGuardLeft_ = fadeLengthSamples_ + kCarrySlack;
}

int StandBank::samplesUntilPendingBuild() const {
	return havePending_ ? std::max(1, fadeGuardLeft_) : std::numeric_limits<int>::max();
}

void StandBank::advance(int hostSamples) {
	if (fadeGuardLeft_ > 0) {
		fadeGuardLeft_ = std::max(0, fadeGuardLeft_ - hostSamples);
	}
	if (fadeGuardLeft_ == 0 && havePending_) {
		build(pending_);
	}
}

} // namespace feathers
