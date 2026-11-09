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

#include "dsp/flagship.h"
#include "dsp/slice_build.h"

#include <array>

// THE NOTE, BAKED — the half of vox that a position cannot answer.
//
// Every other engine here is position-pure: a place in the terrain names a waveform, and the same
// place names the same waveform whatever note is played. Vox cannot be, and the reason is the whole
// point of the engine rather than an inconvenience. A vowel is a pair of FORMANTS — resonances that
// sit at fixed frequencies in Hz because they are the shape of a throat, not a property of the note
// being sung. Sing "ah" an octave up and the formants stay where they are; that is what makes it
// still sound like "ah" rather than like a chipmunk. So the waveform of one cycle depends on how
// many formant periods fit inside that cycle — which is a fact about the NOTE.
//
// V22a's decision is the division of labour that follows: THE TERRAIN STAYS PURE AND THE VOICE BAKES
// THE NOTE. buildVoxSliced answers a position with frequencies in Hz and gains per pulse, knowing
// nothing about what is being played; this file turns that answer into one cycle of waveform for one
// voice's fundamental.
//
// THE BAKE DEPENDS ON A RATIO AND NOTHING ELSE, which is worth stating because it is stronger than
// the decision required. What enters below is formantHz/fundamentalHz — no sample rate, no absolute
// pitch, no block length. So the same note bakes the same cycle at 44.1k and at 96k, and a test can
// pin the whole engine without mentioning a sample rate. The firmware's own render walks integer
// phase per sample and therefore cannot say this; ours can because it bakes a cycle rather than a
// stream.
//
// WHAT PARITY MEANS HERE, stated plainly rather than implied (the C1 commit drew this boundary and
// this file sits on the same side of it): the RECIPE is the firmware's — raised-cosine pulses at the
// formant rate, gains carrying decay and polarity and balance, the pulse index sticking at the last
// slot, the cycle-end release that stops a truncated burst from clicking, the DC compensation. The
// ARITHMETIC is not: the firmware advances a uint32 phase once per sample and evaluates after the
// advance, so its output carries a one-sample offset and its quantisation; a baked cycle is the
// continuous limit of that walk. These two cannot be bit-identical and no test here pretends they
// are. What is pinned instead is every audible property the recipe promises.

namespace feathers {

/// Slots in a baked vox cycle. The flagship's number, deliberately: a vox cycle is blended into the
/// flagship's cycle grid before the operator chain (V22c), and two grids of different lengths would
/// need a resampling step whose only purpose would be to undo a choice made here.
inline constexpr int kVoxSlots = 256;

/// One cycle of vox, for one voice's note.
///
/// FIXED-SIZE AND FLAT, decision V10b: this lives on a voice, on the audio thread, and nothing here
/// may allocate after activate(). The trailing duplicate slot is the wrap guard the whole codebase's
/// grids carry, so an evaluator can read slot i and i+1 without a branch at the seam.
struct VoxCycle {
	/// False when this position has no vox in it at all — every gain zero, so the bake would be a
	/// flat line and the blend has nothing to contribute. A flag rather than a silent grid of zeros
	/// because the consumer can then skip the whole road, which is what keeps a vox-free patch
	/// costing exactly what it always did.
	bool voiced = false;
	std::array<float, kVoxSlots + 1> slots {};

	friend bool operator==(const VoxCycle&, const VoxCycle&) = default;
};

/// Bake `params` for a voice playing `fundamentalHz`.
///
/// The only note-dependent quantity is each formant's frequency divided by the fundamental — how
/// many formant periods fit in a cycle — so that is all the note is used for. A fundamental at or
/// below zero is not a note: the bake yields an unvoiced cycle rather than a division.
///
/// WHY LOW NOTES SOUND MORE VOWEL-LIKE AND HIGH ONES LESS, which falls out of this and is not a
/// defect: at a low fundamental many formant periods fit inside one cycle, so the burst is long and
/// the resonance is well resolved. Play high enough that a formant period is longer than the whole
/// cycle and less than one pulse fits — the vowel flattens into the fundamental, exactly as a sung
/// note does when it climbs past its own formants.
void bakeVoxCycle(const VoxParams& params, double fundamentalHz, VoxCycle& out);

/// THE NOTE A PICTURE BAKES A VOWEL AT — C3, MIDI key 48. A KEY, not a frequency: callers put it
/// through pitchToHz, which is the tuning system's own answer and stays correct if the reference
/// ever moves.
///
/// It lives here, beside the paragraph above, because that paragraph is the whole reason it has to
/// exist. A vowel is the ONE thing the picture surfaces draw that is not a pure function of
/// position: the bake needs a fundamental and a picture has no note. Every other branch on those
/// surfaces meets that problem and stops, falling back to a continuous evaluator that is exact at
/// any pitch. Vox cannot, because the vowel-free cycle is not a coarser drawing of the voweled one
/// — it is a different waveform, and drawing it breaks the honesty rule outright.
///
/// C3 WAS DECIDED, and it is the firmware's own reference: phi_vox.cpp's `kReferenceHz = 130.81f`,
/// the frequency every stored noteRatio is a ratio AGAINST, so a picture drawn here is drawn at the
/// pitch the vowel recipes were authored at. E0 was proposed first and rejected on measurement: at
/// 20.6 Hz a 256-slot cycle carries about 1.55 slots per period of a 1600 Hz second formant, so the
/// upper formant would alias inside the very picture meant to show it.
///
/// BOTH PICTURE SURFACES SHARE IT, which is the point of putting it in one place: the scope and the
/// terrain drawing the same vowel at different pitches would be two surfaces disagreeing about the
/// same sound, which is the defect this whole arc has been closing rather than a new one to open.
inline constexpr double kVoxPictureKey = 48.0;

/// Does this recipe have a burst in it at all — any pulse of any formant carrying a gain?
///
/// THE WHOLE OF WHAT DECIDES `voiced` ONCE THERE IS A NOTE, which makes it position-pure: the bake
/// asks two questions, "is this a note" and "is there any gain", and only the first depends on the
/// voice. So the CHARACTER LAYER can settle the second and store the answer, and the voice's
/// decision to bake cannot disagree with the bank's decision about which road the sample loop is
/// on. Two readings of that would be a voice baking a grid the evaluator never looks at, or worse,
/// an evaluator reading a grid nobody baked.
bool voxParamsVoiced(const VoxParams& params);

/// The floor the log-space formant blend clamps to — the firmware's own low end of the 60..8000 Hz
/// window. A blend runs on log2(Hz), and a sound whose params were never built carries a frequency
/// of zero, whose logarithm would poison the whole sum with −inf rather than merely being wrong.
/// The blend already skips sounds contributing nothing to the identity, so this is a guard on the
/// arithmetic rather than a behaviour anything reaches.
inline constexpr float kVoxMinFormantHz = 60.0f;

/// Two slot loads and a lerp, the house evaluator's contract on a vox cycle.
float evalVoxCycle(const VoxCycle& cycle, double phase01);

/// HOW MUCH VOWEL IS IN THIS PLACE — the species field, V22b.
///
/// the decision was the shape rather than the number: species are terrain-derived, without hard
/// edges. So a species is not a mode you select and not a region with a border you
/// can hear yourself cross; it is a landscape like every other character here, and the engine grows
/// a throat gradually as you walk into it.
///
/// BELOW FEEDBACK'S RATE, and the constant says so in code rather than in a comment — it is derived
/// from kFlagshipFeedbackRate by the same φ^0.5 step that separates each operator field from the
/// next, so the ladder cannot drift apart in a later edit. That places its period around 6200 zones
/// against feedback's 4880: the longest-range field in the engine, which is what a SPECIES should
/// be. A vowel character that changed as fast as a quantize country would read as a modulation
/// effect; one that changes slower than everything else reads as terrain you travel through.
///
/// NO THRESHOLD, deliberately, and this is where it parts from the feedback field it sits under.
/// Feedback keeps only the top quarter of its triangle because feedback is dangerous and should be
/// rare — a max(0, tri − 0.75) cliff-with-a-continuous-join. Vox needs no such guard: a half-present
/// vowel is a perfectly good sound rather than a hazard, so the triangle's own shoulders ARE the
/// fade, and the presence you hear is the field's value unmodified. THE DUTY DOES THE RATIONING
/// INSTEAD, and at 0.25 three quarters of the terrain has no vowel in it at all — which keeps pure
/// flagship a real place rather than a corner, and gives the zero-presence identity a country
/// rather than a single point.
inline constexpr float kVoxSpeciesRate = kFlagshipFeedbackRate / owlet::dsp::phi::kPhi050;

/// WHERE THE ORIGIN SITS IN THAT LANDSCAPE, chosen rather than inherited — and written as the phase
/// it produces instead of as a decimal, because the two are only related through the rate and a
/// hand-written constant would stop meaning this the moment the rate moved.
///
/// The triangle's offset is added BEFORE the rate multiplies, so the phase at coordinate 0 is
/// exactly `offset * rate`; setting the offset to 0.75/rate puts the origin at phase 0.75 — three
/// quarters through the period, and well inside the deadzone. At the original duty of 0.5 that was
/// the middle of the silent half; at 0.25 the deadzone runs from phase 0.25 to 1.0 and the origin
/// sits comfortably within it, so the property survived the retune rather than being lucky. It is
/// what the identity pin leans on: THE FLAGSHIP'S HOME HAS NO THROAT IN IT. A vowel is somewhere you travel to, which is what "a species is terrain" means when
/// you have to pick a number; and an audition that started on a 0.62-strength vowel — which the
/// first spelling of this constant did, measured — would make the species the flagship's default
/// voice rather than one of its countries.
inline constexpr float kVoxSpeciesOffset = 0.75f / kVoxSpeciesRate;

/// The species landscape. DUTY 0.25 (superseding the 0.5 of V22b(d)): vox should cover well
/// under half the field. At 0.5 exactly half the terrain had some vowel in it, which made vox as
/// common as a sync ridge; at 0.25 it sits between sync (duty 0.50) and feedback (0.40 duty times a
/// 0.25 threshold, about 10% effective) — the right rarity for the thing that stops the instrument
/// sounding like an oscillator and starts it sounding like a throat.
///
/// What that buys, since a duty is a fraction of the terrain rather than a loudness: any vowel at
/// all 25.0%, presence above 0.5 12.5%, above 0.75 6.2%, mean presence 0.125. Vox country runs from
/// home 1.523 to 3.047 — the start is where it always was, the country is half as long.
inline constexpr owlet::dsp::phi::PhiTriConfig kVoxSpeciesField { kVoxSpeciesRate, 0.25f,
	                                                               kVoxSpeciesOffset, false };

/// How present the vowel is at `coordinate`, 0..1 — 0 being a place with no throat in it at all,
/// which is most of the terrain and is the country the identity pin stands on.
float voxPresenceAt(double coordinate);

/// THE VOX HALF OF A PREPARED CHARACTER — everything about the vowel that a position can answer.
///
/// The division V22a decided, made into a type: this is pitch-free and lives on the shared character
/// the way every other prepared value does, and the note-dependent bake that turns it into a cycle
/// belongs to the voice. Fixed-size, so it can sit inside PreparedCharacter (decision V10b).
struct VoxCharacter {
	VoxParams params {};
	/// How much of the cycle is vowel here, 0..1 — the species field at the sounds' coordinates,
	/// blended by weight. EXACTLY ZERO over most of the terrain, and that zero is the identity: a
	/// voice standing on it never bakes, never blends, and plays the bank's own grid unchanged.
	float presence = 0.0f;
	/// Whether the vox road is live at all: there is a vowel here AND it has a burst to make it
	/// with. The one flag both the bank's evaluator and the voice's bake decision read, so they
	/// cannot end up on different roads.
	bool audible = false;
};

/// The vowel a whole Character describes, blended across its sounds — the entry the bank builds
/// through, parallel to buildMorphFor and buildFlagshipFor.
///
/// PRESENCE IS READ AT DECISION V7'S LEAD COORDINATE, the same place the operator depths are read,
/// through flagshipLeadCoordinate rather than a second spelling of it. A species is terrain: it has
/// to move under travel and turn with the plane, which is what that coordinate is for.
///
/// PRESENCE AND IDENTITY BLEND BY DIFFERENT WEIGHTS, and the distinction is the whole of what this
/// function has to get right. PRESENCE DILUTES; IDENTITY DOES NOT.
///
///   - PRESENCE — how much vowel is here — blends by Σ w·field, the house field rule, never
///     field(Σ w·coordinate), and is NOT normalised. The weights are already a convex combination,
///     and presence is exactly the quantity that SHOULD fall off when a vowel-free neighbour joins.
///   - IDENTITY — what the vowel IS: the formant frequencies, the pulse gains, breath,
///     articulation — blends at w·presence, NORMALISED by the sum of those weights. buildVoxSliced
///     answers every position, including the three quarters of the terrain with no vowel in it,
///     where its formant numbers describe nothing at all. Weighting the recipe by w alone lets one
///     of those places drag a real vowel's formants halfway toward arbitrary values, so standing
///     between an "ah" and a plain morph sound gives neither an "ah" nor a fade — it gives a third,
///     arbitrary vowel.
///
/// AN EARLIER VERSION OF THIS COMMENT ARGUED FOR w ALONE, on the grounds that the species field has
/// the longest period in the engine so blended neighbours mostly agree about how much vowel is
/// where they are. That is true for sounds CLOSE TOGETHER and false in general, and the
/// constellation is where it fails: eight sounds sit wherever the patch put them, and with the duty
/// at 0.25 a constellation straddling the edge of vowel country is not a corner case — it is the
/// ground you travel when you go looking for the feature. The two rules AGREE EXACTLY wherever the
/// old argument holds, because equal presences cancel out of the normalisation; w·presence is a
/// strict generalisation rather than a competing design, and there is no territory where the old
/// rule is right and this one is wrong.
///
/// FREQUENCIES BLEND IN LOG SPACE — see the implementation for why a tract passes through the
/// geometric middle — and a single sound at weight 1.0 still comes back as its own recipe TO THE
/// BIT, which is the position-purity claim this shares with the rest of the layer and which the
/// log round trip is one careless spelling away from losing.
///
/// A VOWEL-FREE CHARACTER COSTS ONE TRIANGLE PER SOUND AND STOPS. The recipe build is twelve
/// triangle reads and a pulse table per sound, and there is no point paying it for a place with no
/// throat in it; presence is a weighted sum of non-negative values, so a zero total means every
/// sound read zero and there is nothing to blend.
VoxCharacter buildVoxFor(const Character& c);

} // namespace feathers
