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

#include "character/phi_weave.hpp"
#include "dsp/basic_shapes.h"
#include "dsp/constellation.h"
#include "dsp/flagship.h"
#include "dsp/morph_renderer.h"
#include "dsp/partial_spectrum.h"
#include "dsp/phase_profile.h"
#include "dsp/slice_basis.h"
#include "dsp/stair_renderer.h"
#include "dsp/vox_bake.h"
#include "dsp/weave_models.h"
#include "dsp/weave_string.h"

#include <array>
#include <memory>
#include <span>
#include <cstdint>

#include "dsp/phase_warp.h"

// The prepared character tables, and the crossfade schedule every voice plays them against.
//
// One of these per synth, not per voice. A table build is waypoints and a 128-sample loudness
// measurement, and it depends on nothing a voice owns — not its pitch, not its phase, not its noise
// stream — so sixteen voices sharing a character were sixteen identical builds of it. The same
// sharing the firmware does across a unison spread, for the same reason.
//
// What stays per voice is everything with a phase in it: the accumulator, the noise stream, the
// decimator, and the position within a crossfade. Voices are born and die at arbitrary moments, so
// two of them can be at different points of the same fade, or one can be mid-fade while another has
// only ever known the new table.

namespace feathers {

/// Resonate is `Weave`: the physics keeps the firmware model's name (plugin/params.h OscType::Resonate). Its
/// voices read no prepared table — they scan the synth's ONE WeaveString — so a Weave character's
/// payload here is empty and its laws travel by buildWeaveFor below.
/// BASIC SHAPES is `Shapes`: its voices read the band-limited shape ladders (dsp/basic_shapes.h), not a morph
/// table and not a grid — see PreparedCharacter::shapes.
/// ADDITIVE will be `Partials` (2026-10-08; its OscType lands with its voice, plan Task 9): voices that sum up to
/// 320 partials from the blended PartialSpectrum the bank builds — see PreparedCharacter::partials. No grid and no
/// morph table.
/// THE KARPLUS-STRONG ENGINE is `String` (2026-10-31): each voice is its own
/// plucked string (dsp/ks_string.h). The bank builds the PLUCK'S TONE as Additive's spectrum at the same place
/// (PreparedCharacter::partials, by buildPartialsFor, the one reading of it) and the string's MATERIAL beside it
/// (PreparedCharacter::string). No grid and no morph table.
/// THE DRUM ENGINE is `Drum` (2026-11-05): each voice is one drum hit
/// (dsp/drum_voice.h). The bank builds each PAD'S seven fields (PreparedCharacter::drum): in Kit mode every sound at its own
/// place, unblended; in Pitched mode the live sounds' fields blended into pad 0. No grid and no table.
enum class OscEngine : uint8_t { Morph, Stair, Weave, Shapes, Partials, String, Drum };

/// One sound of the constellation, as the character layer sees it.
///
/// Zone and offset are the sound's HOME — the one coordinate the engine has always had. `travelX` and
/// `travelY` are how far that home has been carried across the plane its own generator aims, and they
/// are per sound: the puck is one place, but each sound measures from that place to its own anchor, so
/// a sound you are standing on has not travelled at all. At zero travel this is the engine as it
/// always was, bit for bit, whatever the orientation — which is the point of keeping home and travel
/// apart. Moving zone, offset or gamma moves the sound along the old line; moving travel goes
/// somewhere the old line never went.
///
/// WEATHER IS WHAT SCALES THE TRAVEL, and at zero it scales it away: every sound is exactly itself
/// and the blend is purely relative, which is the engine v1 shipped. The reading of why that
/// architecture was right in the first place is worth keeping — the waypoint-plus-excite crossfade
/// DECOUPLES local topography from performed feel, deliberately, rather than as a concession to what the
/// hardware could afford. Weather is the dial that couples them back together by choice.
///
/// `weight` is this sound's share of the blend, already normalised across the live sounds. It is
/// computed where the geometry is — the puck, the anchors and the falloff — because that is the only
/// place that knows what to do when the puck sits exactly on an anchor, and because the editor has to
/// draw the same blend the engine plays rather than a second reading of it.
struct SoundCoord {
	/// WHICH SLOT THIS LIVE SOUND CAME FROM. The array this sits in is PACKED — a switched-off slot
	/// takes no place — so a position here is not the row a player sees on the rail, and anything that
	/// wants to address a sound the way a player does needs the way back. The mapping computes this
	/// already, to index the per-slot drift and residue by; carrying it costs a byte and saves a second
	/// copy of the packing rule, which is the one thing the two readers of this layer must not each own.
	uint8_t slot{0};

	uint16_t zone{0};
	float phaseOffset{0.0f};
	/// GAMMA'S UNBOUNDED HALF, kept beside the offset rather than folded into it. The offset is a
	/// float and this is not: a base folded in would quantise at about eight zones per ULP by the
	/// time it reached 1e5, which is exactly where "unbounded" starts to mean something. Zero is the
	/// engine as it was, to the bit. See plugin/sound_gamma.h.
	double gammaBase{0.0};
	double travelX{0.0};
	double travelY{0.0};
	double weight{0.0};

	/// THE SMOOTHNESS OVERRIDE KNOB for this sound. Applied to the flagship smoothness field at
	/// field-read time (flagshipAuthorsAt → buildFlagshipFor), so the result feeds both the audio
	/// decode and the terrain feature measurement. NEUTRAL AT THE MIDDLE, not at the top: 0.0 defeats
	/// smoothing entirely (plain morph), kFieldNeutral is the field as-is, and above it the knob
	/// biases UP so a sound can be smooth on ground whose field says zero — which is most ground. See
	/// fieldWithOverride for why, params.h ASmoothness..HSmoothness, and the doc §11.
	float smoothnessScale{kFieldNeutral};

	/// THE FM DEPTH KNOB for this sound (FM doc §9/§10), 0..1 and DEFAULTING TO ZERO — which is the
	/// whole of FM's identity guarantee. At zero the voice never touches its modulator accumulator and
	/// renders exactly what it rendered before FM existed, so every patch saved before this is
	/// byte-identical and the terrain is unchanged. Scales the contribution the three FM terrain
	/// fields describe; it is the one quantity FM spends parameter ids on, and the only one a host can
	/// automate. See params.h AFmDepth..HFmDepth.
	float fmDepth{0.0f};

	/// THE FIVE FIELD MODIFIER KNOBS for this sound (2026-08-25; params.h AQuantize..HGrit). Each scales the terrain field that authors its
	/// quantity, through fieldWithOverride — the same curve, the same neutral, and for the same reason
	/// smoothnessScale above has them: below the middle the knob attenuates the field toward nothing,
	/// above it the knob lifts the field toward full, so a player can ASK for quantize country rather
	/// than only find it.
	///
	/// ALL FIVE REST AT kFieldNeutral, which is the field as the terrain authored it. A SoundCoord
	/// that nobody has touched therefore renders exactly what it rendered before these existed —
	/// which is the property the defaults are FOR, and the reason the default is not 1.0: at 1.0 an
	/// unspecified sound would silently ask for every field lifted to full, the exact bug flagship.h
	/// records beside kFieldNeutral.
	///
	/// FOUR ARE FLAGSHIP FIELDS, applied at field-read time in buildFlagshipFor AND in the sliced
	/// entry prepareFlagshipRecipe — both, always. An effect applied at one of the two renders
	/// ATTENUATED rather than absent, and a difference check passes on it; CLAUDE.md's rule names this
	/// exact pair.
	float quantizeScale{kFieldNeutral};
	/// WARP DEPTH FOR THIS SOUND, 0..1 — an AMOUNT and not a bias, so it defaults to 0 (no warp) and
	/// not to kFieldNeutral. There is no φ field behind it, by design: it is just a
	/// modifier knob, as in Serum. See dsp/warp.h.
	float warpDepth{0.0f};
	float syncScale{kFieldNeutral};
	float feedbackScale{kFieldNeutral};
	/// POINTS COMPOSES WITH SMOOTHNESS RATHER THAN REPLACING IT, and the order is a decision:
	/// pointCountWithOverride(fieldWithOverride(field, pointScale), smoothnessScale). This knob
	/// modifies the FIELD — what K the terrain authors here — and the top of the Smoothness travel
	/// still walks whatever K results down toward two, which is what backlog §7's "a little stronger
	/// at maximum" bought. Both neutral is today exactly. See the plan doc §3.
	float pointScale{kFieldNeutral};
	/// GRIT IS THE ODD ONE: it scales the MORPH TABLE's ampNoise, applied in slice_build.cpp to the
	/// raw [0,1] triangle before the 0.25 factor, not to any FlagshipFields member. So it reaches the
	/// Morph engine and not only V5 — which is why its control is the one of the five that must not be
	/// greyed off-V5. Its twin in modifiers.h, phaseJitter, deliberately has no knob: it was named amp
	/// noise.
	float gritScale{kFieldNeutral};
	/// THE ADDITIVE SHAPE AMOUNT for this sound, 0..1 (design §4.1). An AMOUNT, Warp's kind, so it defaults to 0: at 0
	/// the sound is its terrain spectrum to the bit, at 1 it is the engine-wide `Character::additiveShape` exactly.
	/// Plain data the bank reads on a Partials build; NO PARAM FEEDS IT YET (its per-sound knob lands with the Explore
	/// controls, plan Phase B), so it rests at 0 and every other engine's SoundCoord compares equal to before.
	float shapeAmount{0.0f};
	/// THE ADDITIVE BIAS KNOBS for this sound (design §4.1): Rolloff, Formant, Odd/Even, Comb, Detail and Off-grid,
	/// each fieldWithOverride over its terrain field(s) on a Partials build (PartialBias names them). BIASES, so all six
	/// rest at kFieldNeutral, the terrain as authored. Read from the params only on Additive (character_params.h), so
	/// every other engine's SoundCoord compares equal to before.
	PartialBias partialBias{};
	/// THE STRING'S FIVE FIELD SCALERS for this sound (KS design §4.1): Brightness, Sustain, Pick, Noise and Stretch, each
	/// fieldWithOverride over its field on a String build. Neutral by default and read from the params only on the
	/// Karplus-Strong engine, so every other engine's SoundCoord compares equal to before.
	StringBias stringBias{};
	/// THE DRUM'S SEVEN FIELD SCALERS for this sound (drum proposal §3.2), read from the params only on the Drum engine, so
	/// every other engine's SoundCoord compares equal to before.
	DrumBias drumBias{};
	/// AND THE KIT-WIDE TRIMS (2026-11-06), the same law applied again on top of the pad's own: neutral except on a Drum Kit.
	DrumBias drumKitBias{};
	/// THE LAYER'S PLACE (drum proposal §12, stage (c)): its offset from this pad's travel, read only on a Drum Kit whose pad has
	/// a layer (zero everywhere else, so every other SoundCoord compares equal to before).
	double drumLayerX{0.0};
	double drumLayerY{0.0};
	bool drumLayer{false};

	/// THE BLUE SHIFT, and both of these are exactly zero unless the puck is moving AND the audition
	/// has turned a scale up. See §2b: the excitation gets a direction again, because a constellation
	/// has no target but it does have a velocity.
	///
	/// `approach` is how fast you are closing on this sound — `dot(velocity, direction to it)`, scaled
	/// by the Doppler reactivity field where you are standing. Positive approaching, negative receding.
	///
	/// `weightRate` is how much of this sound's share would arrive in the next moment if you kept
	/// going. It is what gives the amplitude overshoot a DIRECTION OF TRAVEL, which is the thing the
	/// two-way rule got for free and the N-way blend had to do without: pushing the waypoints further
	/// along the way they are already moving is singular nowhere, because the weights are smooth
	/// everywhere except on an anchor where the motion term is being multiplied by a vanishing excite
	/// anyway.
	double approach{0.0};
	double weightRate{0.0};

	/// THE PLANE THIS SOUND PLAYS ON, already resolved: its own pin if it has one, the audition's
	/// global pin if not, and empty when neither — in which case the builder derives it from this
	/// sound's home exactly as it always has. Resolved where the parameters are read rather than in
	/// the bank, so the editor and the engine cannot disagree about whose plane a sound is on.
	SliceOrientationPin orientation{};

	/// WHAT THIS SOUND'S ROTATIONS BANKED, so that re-aiming its plane left it standing where it was —
	/// the decision of 2026-08-13, which superseded A10. Empty for a sound that has never been rotated
	/// at a drift, and empty adds exactly nothing: this is the migration guarantee rather than a
	/// tolerance. Both engines' worth, because the oscillator type is a parameter and a rotation should
	/// not be forgotten by switching it. See plugin/sound_residue.h.
	PlaneResidue residue{};

	friend bool operator==(const SoundCoord&, const SoundCoord&) = default;
};

/// Everything that decides what a voice sounds like, as opposed to what pitch it plays.
///
/// A CONSTELLATION OF SOUNDS AND A SHARE FOR EACH, which generalises the v1 "path": two sounds and a
/// crossfade between them was the case where the constellation is a segment. The character layer takes
/// nothing else — gamma and the rest of the param set resolve into these coordinates before they get
/// here.
///
/// `sounds[0 .. count)` are live and the rest are zero; a sound switched off in the patch is not here
/// at all, because "is this sound enabled" is a question about the patch and the geometry that
/// produced these weights has already answered it. `count` is small on purpose: it is what decides
/// which blend rule applies, and it is the honest count of what you can hear.
///
/// `xf` IS STILL PATH — it walks the tour, and the tour is where the puck sits before Pos X/Y step off
/// it. It no longer selects a blend rule: ONE RULE NOW APPLIES AT EVERY SOUND COUNT (§2c), so there is
/// no moment when switching a third sound on changes how the first two are mixed. The verbatim v1
/// crossfade survives as a parity reference behind `v1Crossfade`, which nothing but a test and a debug
/// environment sets.
///
/// Each sound's PLANE comes from its own home, so no two are born looking in the same direction —
/// per-sound orientation, as designed, and arriving for free rather than as a second mechanism.
/// **THE LIKES' PHASE WARPS A CHARACTER IS BUILT THROUGH (backlog ON, dsp/phase_warp.h)** — per sound slot, held by whoever
/// assembled the character (the synth, filtered to the engine being played; the scope; a test), and named by a SERIAL
/// rather than by the pointer: two characters compare equal through it when they carry the same warps, so a new set is a
/// rebuild and a re-sent one is not. The holder keeps the table alive while any character naming it may still be built
/// (the synth keeps the current and the previous). No table, or an empty entry, is the identity.
struct LikeWarpRef {
	const std::array<LikeWarp, kMaxSounds>* table = nullptr;
	uint64_t serial = 0;
	[[nodiscard]] const PhaseWarp* of(size_t slot) const noexcept {
		return table != nullptr && slot < kMaxSounds ? (*table)[slot].warp : nullptr;
	}
	/// The warp's content key (0: none) — what a memo keyed on a sound's inputs compares.
	[[nodiscard]] uint64_t keyOf(size_t slot) const noexcept {
		return of(slot) != nullptr ? (*table)[slot].key : 0;
	}
	friend bool operator==(const LikeWarpRef& a, const LikeWarpRef& b) noexcept { return a.serial == b.serial; }
};

struct Character {
	OscEngine engine{OscEngine::Morph};
	/// THE FLAGSHIP RIDES THE MORPH RENDER PATH, so `engine` alone cannot say a sound is V5 — and
	/// the bank has to know, because a flagship build applies the operator tier where a morph build
	/// must not. In-memory only: the patch stores the OscType index, and this is derived from it
	/// where the parameters are read. It participates in the defaulted operator== like everything
	/// else here, which is what makes toggling the engine dropdown a real rebuild rather than a
	/// request dropped as equal-to-live. Meaningless while `engine` is Stair.
	bool flagship{false};
	/// WHICH WARP SHAPE THE ENGINE IS BENDING ITS READ PHASE WITH — engine-wide, like `engine`
	/// itself, and for a reason the per-sound scales do not have: the flagship renders ONE cycle from
	/// the blended recipe and **an enum cannot be blended**. A per-sound mode would have to be
	/// resolved by dominant weight and would step mid-crossfade. The DEPTH is per sound and blends
	/// normally (`SoundCoord::warpDepth`).
	///
	/// It participates in the defaulted operator== like everything else here, which is what makes
	/// moving the dropdown a real rebuild rather than a request dropped as equal-to-live.
	WarpMode warpMode{WarpMode::Off};
	/// THE BASIC SHAPES MIX — engine-wide, the four knobs as the patch holds them ("the only
	/// net new knobs would be the basic shapes mixes"). Read only for BasicShapes and left at its default
	/// otherwise, so every other engine's Character compares equal to what it was before this existed.
	ShapesMix shapes{};
	/// THE ADDITIVE SHAPE (design §4.1) — engine-wide, the exact band-limited target each sound's `shapeAmount` blends
	/// toward. Plain data the bank reads on a Partials build; no param feeds it yet (plan Phase B), so it rests at its
	/// default and every other engine's Character compares equal to what it was before this existed.
	partials::ShapeKind additiveShape{partials::ShapeKind::Saw};
	/// THE ADDITIVE ENGINE'S PHASES (A-PH, design §8): Natural, Headroom or Sub priority, with Sub priority's Sub and Ceiling —
	/// engine-wide, read only on Additive (every other engine keeps the default and compares equal to what it was).
	partials::Phasing phasing{};
	std::array<SoundCoord, kMaxSounds> sounds{};
	/// The likes' phase warp each sound is built through (LikeWarpRef). Default: none.
	LikeWarpRef likes{};
	/// How many entries of `sounds` mean anything. Entries past it are value-initialised, so two
	/// characters with the same live sounds compare equal whatever was switched off to get there.
	uint8_t count{0};
	/// THE DRUM ENGINE'S KIT MODE (drum proposal §3.3): every one of `sounds` is a PAD, built at its own place and never
	/// blended. False everywhere else, so every other Character compares equal to before.
	bool drumKit{false};
	float xf{0.0f};
	/// Where the puck is, carried so that the thing which has to DIFFERENTIATE it does not have to
	/// re-read forty-eight parameters to find out. Nothing downstream of the blend reads it.
	AnchorPoint puck{};
	/// How hard the velocity-aimed overshoot pushes: the overshoot reactivity field where you are
	/// standing, times the audition's scale. Zero by default, and zero everywhere the field is.
	float blueOvershoot{0.0f};
	/// THE PARITY REFERENCE — take the verbatim v1 crossfade at two sounds instead of the uniform
	/// blend. False everywhere except a test and a debug environment; see audition_tuning.h.
	bool v1Crossfade{false};
	/// Normally empty: a sound's plane is derived from its home, so each one has its own and none of
	/// them carries a number anyone chose. Filled only by the experimental pin.
	SliceOrientationPin orientation{};

	friend bool operator==(const Character&, const Character&) = default;
};

/// The morph table this character describes — the one the voices play.
///
/// Shared between the bank and the editor's wave display so that the picture cannot be of a different
/// sound from the one being made. Two sounds go through buildMorphXfade at `xf`, which is v1 exactly;
/// anything else goes through the N-way recipe blend at the weights.
owlet::dsp::PhiMorphWavetable buildMorphFor(const Character& c);

/// The stair staircase this character describes, prepared for evaluation. Same rule as the morph
/// side: two sounds crossfade at `xf`, three or more blend at the weights.
owlet::dsp::PhiStairParams buildStairFor(const Character& c);

/// The laws the Resonate engine's ONE string plays under for this character: two sides and the
/// crossfade between them, which is the shape WeaveString takes (setLaws(a, b) + setCrossfade).
///
/// A CHARACTER IS N WEIGHTED SOUNDS, NOT TWO ENDS, so the two sides are built like this: every field
/// of both sides is the weight-blend of every sound's laws (convex, so the laws' clamps and the
/// k + 4c stability bound still hold), and the only thing the sides differ in is the bow MODE — a
/// discrete choice the blend cannot average. `a` takes the heaviest sound's mode, `b` the second
/// heaviest's, and `crossfade` = (w2 - w3) / ((w1 - w3) + (w2 - w3)), w3 being the third heaviest (0 for
/// two sounds), so the ring's own mode crossfade (forces blended, never switched) carries the change.
/// For two sounds that is exactly A = sound 0, B = sound 1 at the relative weight, because every other
/// field is linear. Every swap is seamless: the two heaviest trade at cf = 0.5 with the modes swapped,
/// and the second and third trade at cf = 0, where B's mode carries no weight. See buildWeaveBlend.
/// And because the crossfade moves with the puck and Path, MOVING bows the string — the firmware's
/// morph-bow, which it drove from its A/B knob.
struct WeaveCharacter {
	owlet::dsp::PhiWeaveParams a{};
	owlet::dsp::PhiWeaveParams b{};
	float crossfade{0.0f};
};

/// The weight blend above, over any number of sounds' laws. Public so a test can hand it laws of its
/// own. The weights are normalised by their sum; a sum that is not positive plays the first sound.
WeaveCharacter buildWeaveBlend(std::span<const owlet::dsp::PhiWeaveParams> laws, std::span<const double> weights);

/// ONE SOUND'S LAWS, REMEMBERED WITH EVERYTHING THEY WERE BUILT FROM (backlog LZ, 2026-10-10). The synth builds the
/// laws on every character update and on EVERY engine (a frozen string keeps following the puck, Synth::pushWeaveLaws),
/// and each sound's are 27 φ banks and a few hundred sines: MEASURED the largest cost a still 8-sound patch paid per
/// enabled sound, whatever its weight. A sound's laws are a pure function of its zone, offset, gamma, travel, effective
/// orientation and weave residue — not of its weight, not of the puck — so an entry whose inputs are the same BITS
/// hands back the laws those bits built, which are the laws a rebuild would build. Keyed on values, never on an
/// address (CLAUDE.md's ABA rule), and per position in the constellation, so a switched sound simply misses.
/// Fixed-size, a member of whoever owns it: no allocation.
struct WeaveLawMemo {
	struct Entry {
		bool valid = false;
		bool pinned = false; ///< the effective orientation, as `oneWeave` picks it
		float rotate = 0.0f;
		uint16_t zone = 0;
		float phaseOffset = 0.0f;
		double gammaBase = 0.0;
		double travelX = 0.0;
		double travelY = 0.0;
		SliceResidue<kWeaveTriangles> residue {};
		/// The likes' phase warp the laws were built through, by content (LikeWarpRef::keyOf; 0: none).
		uint64_t warpKey = 0;
		owlet::dsp::PhiWeaveParams laws {};
	};
	std::array<Entry, kMaxSounds> entries {};
	uint64_t built = 0; ///< laws actually built through this memo, for the case that says it is consulted
};

/// This character's laws, each sound's read through its own plane (buildWeaveSliced) exactly as
/// Stair's staircases are. Shared by the synth and the editor's pictures, for buildMorphFor's reason.
/// With a `memo`, a sound whose inputs are unchanged since the memo last saw that position is not rebuilt.
WeaveCharacter buildWeaveFor(const Character& c, WeaveLawMemo* memo = nullptr);

/// **`buildWeaveFor(c).crossfade`, WITHOUT BUILDING THE LAWS** (the idle-overhead pass, 2026-11-05): the crossfade is a
/// function of the weights alone, and a frozen string (any engine but Resonate) needs only it each tick
/// (`Synth::pushWeaveLaws`). The same arithmetic `buildWeaveBlend` runs, by construction: both read `weaveLeadOf`.
float weaveCrossfadeFor(const Character& c);

/// THE ADDITIVE BUILD'S WORKING SPECTRA — one sound's, and the engine-wide shape's, about 10 KB. NOT LOCALS, for one
/// reason: a build can run on the audio thread (CharacterBank::advance, between slices), which is the HOST's thread,
/// and a 10 KB call frame on a stack whose size we neither chose nor can query is a risk we have no need to take.
/// (It is not a rule against stack use in general: the other builds keep their small working state as locals.)
///
/// ONE PER BANK, AND DELIBERATELY NOT SHARED. Each bank owns one beside the tables it builds into and reuses it on
/// every build; nothing is carried from one build to the next (buildPartialsFor overwrites what it reads first, and
/// decides afresh each call whether the shape is built), and it is never touched concurrently, because a bank builds
/// one ear at a time, from one thread, inside build(), the rule its tables_ live by. A single buffer shared between
/// banks would save a few KB and buy a cross-object invariant (two banks never building at once) that nothing
/// states or checks: do not merge them.
struct PartialsScratch {
	partials::PartialSpectrum sound {};
	partials::PartialSpectrum shape {};
	/// How many sounds the LAST buildPartialsFor actually built (a sound with no share is skipped). Written BY the
	/// build, so a case can see the skip; nothing in the engine reads it.
	uint32_t soundsBuilt {0};
};

/// THE ADDITIVE ENGINE'S SPECTRUM for this character (design §1.2): each live sound's PartialSpectrum built at its
/// own coordinate through its own frame (the same `sliceFrameFor(rotateFor(pin), home, residue)` the other engines
/// take), its Smoothness knob on the Smoothness field, blended toward `additiveShape` by its Shape Amount and
/// renormalised; then Σ share·sound (shares = the sanitised weights over their sum; a sound with no share is not
/// built) and the blend renormalised to partials::kTargetRss. No sounds is silence. Shared by the bank and, later,
/// the editor's pictures, for buildMorphFor's reason.
/// `scratch` holds the per-sound working spectra (see PartialsScratch): the build writes it and nothing reads it after.
void buildPartialsFor(const Character& c, partials::PartialSpectrum& out, PartialsScratch& scratch);

/// THE STRING'S MATERIAL for this character (KS design §2.1): each live sound's five fields at its own coordinate through
/// its own Additive frame (the frame its pluck's tone is read through), its five scalers applied, then Σ share·field with
/// buildPartialsFor's shares. No sounds is the default material.
StringFields buildStringFieldsFor(const Character& c);

/// THE DRUM PADS' FIELDS (drum proposal §3): in Kit mode each live sound's seven at its own coordinate, scalers applied, at its
/// slot; in Pitched mode the live sounds' blended (buildStringFieldsFor's shares) into pad 0.
struct DrumCharacter {
	std::array<drum::Fields, kMaxSounds> pads{};
	/// EACH PAD'S LAYER'S FIELDS (stage (c)): at the pad's place plus its offset, through the pad's own scalers; default where a
	/// pad has no layer.
	std::array<drum::Fields, kMaxSounds> layers{};
	bool kit{false};
	friend bool operator==(const DrumCharacter&, const DrumCharacter&) = default;
};
DrumCharacter buildDrumFor(const Character& c);

/// WHAT A PARKED, UNBOWED STRING OF MODEL `model` READS UNDER THESE LAWS: its rest pose along the scan
/// path (WeaveString::restScanPath — the solve of k(x - home) = elastic force, NOT the raw home shape,
/// which coupling pulls away from), at the loudness the string's AGC settles to on it. WeaveString::kNodes
/// values, on the voices' own ±1 scale. The one picture of "what this coordinate selects" for the
/// wave display and the terrain alike; the live ring wanders around it — bowed, plucked, travelling.
/// The crossfade of `w` is applied to the fields the picture reads (the bow mode is not one of them).
void weaveRestCycle(WeaveModel model, const WeaveCharacter& w, float* out);

/// RESONATE'S OWN SHAPING : the engine-wide knobs that take the Explore disk's surround
/// on Resonate. Every default is the laws as authored, so a default-constructed one changes nothing.
struct ResonateShaping {
	float width = 0.0f;     ///< a second scan head, `width` x half the ring away (the synth's stereo; not a law)
	float bow = 1.0f;       ///< x the laws' bow depth (the string's Excitation::bowScale; not a law)
	int bowMode = 0;        ///< 0 = the laws' own; 1..5 = WeaveBow (index - 1) forced on both sides
	float shimmer = 1.0f;   ///< x the scan smoother's coefficient (shimmerAlpha)
	float stiffness = 1.0f; ///< x k, the springs home
	float coupling = 1.0f;  ///< x c, the neighbour springs
	float decay = 1.0f;     ///< how many times LONGER the ring rings: damping / decay
	float pluck = 1.0f;     ///< x the note-on pluck's amplitude
	float travel = 1.0f;    ///< x the ring's travel rate

	friend bool operator==(const ResonateShaping&, const ResonateShaping&) = default;
};

/// The shaping's laws applied to one side's laws, in place. Each law is scaled and then CLAMPED to the
/// range the laws' builder clamps it to (phi_weave.cpp: stiffness 0.016..0.12, coupling 0.004..0.45,
/// damping 0.006..0.09), so every stencil's stability bound (k + 4c < 4, weave_models.h) holds at every
/// setting; the bow balance is recomputed from the new damping by the builder's own law, so a lighter
/// damping cannot let the bow pump the string unbounded. A factor of exactly 1 touches nothing at all — not even the
/// clamp — so the default shaping is the laws to the bit. Width and Bow are not laws (see above).
void shapeWeaveLaws(const ResonateShaping& shaping, owlet::dsp::PhiWeaveParams& laws);

/// A character with all of its per-block work already done. Only one of the two payloads is live,
/// per `engine`; carrying both is what lets a fade run from a morph table to a stair one.
struct PreparedCharacter {
	OscEngine engine{OscEngine::Morph};
	owlet::dsp::PhiMorphWavetable morph{};
	StairTable stair{};
	/// The flagship's baked operator grid, live exactly when `engine` is Morph and the build was a
	/// V5 at an operated position. `operated` false is the identity corner: the voice renders the
	/// continuous morph table and never looks in here. Fixed-size members, like everything a voice
	/// reads — rule V10b, the no-allocation-post-activate rule.
	FlagshipTable flagship{};

	/// THE VOWEL AT THIS POSITION, and how much of it is heard — the position-pure half of vox
	/// (rule V22a). `audible` false is most of the terrain and is the identity: the sample loop
	/// takes the roads it took before vox existed, and no voice bakes anything.
	VoxCharacter vox{};

	/// THE OPERATOR RECIPE AS BUILT, kept for the one consumer that has to re-run it.
	///
	/// The bank bakes the flagship grid WITHOUT the vowel in it — vox-free is cheap, shared by every
	/// voice, and it is the identity path — so a voice standing in vowel country has to bake its own
	/// (the cycle depends on its note, which is the whole of V22a). To do that it needs the recipe
	/// the bank used, not just the grid the bank got: `held` is the chain sampled at the step
	/// centres, and a staircase built from the vowel-free cycle is not the staircase the blended one
	/// quantizes onto.
	///
	/// Live for a flagship build; meaningless otherwise, and read only when `vox.audible`.
	FlagshipRecipe recipe{};

	/// THE RECIPE'S SMOOTH SOURCE AT EVERY SLOT, read once here for every voice that bakes this table's vowel
	/// (FlagshipSlotSmooth, backlog PB). Live exactly when `vox.audible` and the recipe reads it; a voice hands it to
	/// its bake from the very table it is baking, so it is never a value from another build.
	FlagshipSlotSmooth smooth{};

	/// THE SHAPES BLEND, normalised — live exactly when `engine` is Shapes. A Shapes build also keeps V5's
	/// `recipe` (its FM triple and warp, rules C/E) and its `morph` (for ampNoise and phaseJitter), but never
	/// bakes `flagship`: the voice reads the band-limited ladders through evalShapes instead.
	ShapesWeights shapes{};

	/// THE ADDITIVE SPECTRUM, blended and renormalised — live exactly when `engine` is Partials (buildPartialsFor).
	/// Fixed-size like everything here, so it rides the bank's two-slot buffer and is identified by `buildSerial`
	/// below, never by its address. A slot rebuilt into another engine keeps these bits; only `engine` says
	/// whether they mean anything.
	partials::PartialSpectrum partials{};
	/// AND ITS PHASING (A-PH), copied from the Character on a Partials build (default on every other): the voice reads the mode
	/// at note-on and Sub priority's Sub and Ceiling on every block.
	partials::Phasing phasing{};
	/// THE STRING'S MATERIAL, blended — live exactly when `engine` is String, whose pluck's tone is `partials` above.
	StringFields string{};
	/// THE DRUM PADS' FIELDS — live exactly when `engine` is Drum.
	DrumCharacter drum{};

	/// WHICH BUILD THIS IS — a serial the bank stamps on every prepared table, never reused.
	///
	/// IT EXISTS BECAUSE THE ADDRESS IS NOT AN IDENTITY. The bank double-buffers into tables_[2][2]
	/// and flips on every build, so only two addresses per ear ever exist and a third build lands
	/// back on the first's. Anything that CACHES a derived result against a PreparedCharacter must
	/// key on this rather than on the pointer, or it will hold a value from two rebuilds ago and
	/// compare equal — a classic ABA, and it shipped once: a voice kept playing a vowel baked for a
	/// character the puck had long left, so the sound stopped evolving under performance until the
	/// next note retriggered the voice.
	///
	/// Contents and serial move together by construction, which is the property the pointer never
	/// had. Zero means "no build", so a default-constructed key can never match a real table.
	uint64_t buildSerial{0};

	/// WHETHER THE SAMPLE LOOP READS A GRID HERE, rather than evaluating continuously.
	///
	/// ONE PREDICATE, TWO CALLERS, and it exists because they had drifted apart: `eval` asked
	/// "operated or a vowel" while `gridFor` asked "operated" alone. That was safe only by accident
	/// — the voice short-circuits before ever asking gridFor for a voweled character — and the trap
	/// it left is the worst kind: a future caller writing entirely reasonable code would be handed
	/// the stair grid for a morph character and have eval read it as a flagship grid. A silent wrong
	/// sound, reachable without doing anything wrong.
	bool readsGrid() const {
		return engine != OscEngine::Morph || flagship.operated || vox.audible;
	}

	/// One evaluation at `phase01`. `grid` arrives already chosen by gridFor — which mip plays
	/// depends only on the fundamental, which is block state, so resolving it here would be
	/// re-deciding a settled question once per sample. Ignored on the continuous morph path.
	float eval(const float* grid, double phase01) const;

	/// A SHAPES side's evaluation: the blend at the voice's own mip (the mip is a fact about the note and its
	/// FM index, so the voice resolves it — see OscVoice::shapesMip_), read through this build's warp.
	/// Inline, because it is the voice's per-sample read.
	float evalShapesAt(int mip, double phase01) const {
		return evalShapesWarped(shapes, mip, phase01, recipe.warpMode, recipe.warpDepth);
	}

	/// The grid this table plays through at `stairMip`, or the one it would if it needed one: the
	/// flagship's when its operator grid is live, stair's otherwise. The voice asks per prepared
	/// table, because a fade can be running from a stair table into a flagship one and each side
	/// reads its own. The mip level is stairMipForFrequency's for both — a statement about the
	/// note, not the engine.
	///
	/// NULL WHEN `vox.audible`, because there is genuinely no answer: the grid a vowel plays through
	/// is the VOICE's own bake — the cycle depends on the note, and the bank serves every note at
	/// once. Returning null rather than the nearest plausible grid is the house preference for a
	/// loud failure over a quiet wrong one (Engine rail.cpp makes the same trade for the same reason):
	/// a caller that has not thought about whose grid this is finds out immediately, instead of
	/// rendering a stair waveform through a morph character and sounding merely odd. The voice never
	/// asks — gridForSide tests `vox.audible` first, on the same fact `readsGrid` is built from.
	const float* gridFor(int stairMip) const;

	/// The per-sample modifier depths, zero for a stair table whose engine has none. Read through
	/// these rather than off `morph` directly: a slot rebuilt from morph to stair keeps its old
	/// wavetable bits, and only `engine` says whether they mean anything.
	float phaseJitter() const;
	float ampNoise() const;

	/// THE FM TERMS A VOICE NEEDS, through the same guarded door and for the same reason: a slot
	/// rebuilt from morph to stair keeps its old bits, and only `engine` says whether they mean
	/// anything. FM is a flagship feature, so a stair table answers "off" and the voice's whole FM
	/// road costs it one compare.
	///
	/// `fmDepth` ZERO IS THE IDENTITY and is the default, so a voice standing anywhere the knob has
	/// not been turned renders exactly what it rendered before FM existed.
	float fmDepth() const;
	/// The modulator's frequency as a multiple of the carrier's — the two interval fields resolved.
	float fmFrequencyRatio() const;
	/// How much of the modulation is a stereo polarity flip: 0 is mono-safe, 1 is the full §4 split.
	float fmStereoWidth() const;
	/// The modulation index in CYCLES of carrier phase for a note at `fundamentalHz`, with §5's alias
	/// rule already applied. Per note, because the rule is written in terms of the fundamental.
	float fmIndexCyclesFor(double fundamentalHz) const;
};

/// The crossfade a character change is masked by, as a duration rather than a block count.
///
/// A rebuild genuinely POPS — the pattern families quantise a threshold in step index, so a height
/// can flip sign between adjacent phase offsets, and a wrap-riser origin can switch. Interpolating
/// the params does not smooth those, because the pop is in the params; fading between the two
/// rendered waveforms is what masks them, and it is the same job the firmware's smoothedCrossfade
/// does on hardware.
///
/// A fixed duration (2 ms until backlog PB (c), below), and deliberately NOT "one block". Block length
/// is the host's business: with CLAP event splitting a block can be a single sample, and a
/// handful-of-samples fade is not a pop-masker. It also has to be a constant for the render to be
/// reproducible at all — a fade whose length came from the block would make every sample after a
/// character change a function of how the host chose to chop its buffer.
///
/// **AND IT IS THE REBUILD RATE: ONE BUILD PER 256 SAMPLES AT 48 kHz, INTERPOLATED** (backlog PB option (c),
/// approved 2026-10-29). A build arms a guard of one fade (plus kCarrySlack) and a request inside it waits for its end,
/// so the fade's length is also the shortest interval between two builds. At 2 ms a moving character (Book of Dreams'
/// Free LFO) rebuilt about every 100 samples, and EVERY voice rebaked its vowel at each one — 99% of a core at sixteen
/// notes. At 256 samples a continuous motion rebuilds 2.6 times less often, and because the fade is as long as the
/// interval, consecutive bakes join end to end: each one fades in over exactly the time until the next lands, so the
/// voice is always on a straight line between two bakes, never parked on one and then ramped (which is what a 2 ms
/// fade behind a 256-sample guard does: a plateau and a step every interval — a zipper on a fast sweep). The cost is a
/// lag of one interval (5.3 ms) behind the request, and a single change from rest fades over 5.3 ms instead of 2.
/// A first build has nothing to fade from and lands at once, as before, so a note's first table is never waited for.
/// Seconds rather than samples so the sound is the same at every host rate. MEASURED (bench_patch, Book of Dreams, 16
/// notes, load 5-5.6, medians of 5 alternated rounds): held 3518 us at 2 ms, 2465 at 256 samples, 2002 at 1024; p99
/// 4052, 2757, 2762. 256 is the default: 1024 buys 13% more but a 21 ms lag, and a 21 ms smear on every discrete change.
inline constexpr double kCharacterFadeSeconds = 256.0 / 48000.0;

/// **WHAT A VOICE READS FROM A BANK, AND NOTHING ELSE** (voice stacking step 4c). `OscVoice` asks four things of the
/// bank it plays: whether it has a table, which generation it is on, the current table by ear, and how long a fade
/// is. Both kinds of bank answer them — the synth's `CharacterBank` and a stacked part's mono `StandBank` — so a part
/// can move between the two. Virtual, because a voice asks only when it syncs (once per render), never per sample.
class TableSource {
public:
	virtual ~TableSource() = default;
	virtual bool hasTable() const = 0;
	virtual uint32_t generation() const = 0;
	virtual const PreparedCharacter& current(int ear) const = 0;
	virtual int fadeLengthSamples() const = 0;
};

class CharacterBank final : public TableSource {
public:
	CharacterBank();

	/// Also sizes the fade, so it must be called before the first render at a new rate.
	void setSampleRate(double hostSampleRate);

	/// Ask for `c` to become the live character.
	///
	/// Takes effect immediately if nothing is fading; otherwise it is remembered and applied when the
	/// fade in flight has drained. THAT DEFERRAL IS THE WHOLE SAFETY ARGUMENT for the double buffer:
	/// a fading voice is reading both slots, and the only spare slot to build into is the one it is
	/// fading FROM. Before fades could span blocks, a fade always finished inside the render call
	/// that armed it and the case could not arise; now that they span blocks, it is prevented here
	/// instead. Rebuilding the same character is free — a request equal to what is already live, or
	/// to what is already pending, is dropped rather than re-armed, so a host sending its parameter
	/// state every block does not put the synth into a permanent crossfade.
	void setCharacter(const Character& c);

	/// Ask for TWO characters, one per ear — terrain stereo (rule V11), each ear a full
	/// evaluation of the terrain at its own displaced puck. Same contract as setCharacter in every
	/// other respect: immediate when nothing is fading, deferred behind the guard otherwise, a
	/// request equal to the live pair dropped. The ears share one generation, one fade and one
	/// guard — they are asked for together, so they land together; two banks would be two guards
	/// free to disagree about when a deferred build lands, which is a seam the host's block size
	/// could reach.
	void setCharacterStereo(const Character& left, const Character& right);

	/// Report `hostSamples` of rendering, which is what retires a fade and lets a deferred build in.
	/// Call it once per rendered slice, after the voices have run.
	///
	/// MUST NOT be given more samples than samplesUntilPendingBuild() allows. This is the only thing
	/// that moves the guard, so a deferred build lands wherever a caller happens to cross zero — and
	/// a caller that crossed it mid-piece would land the build at the end of a piece whose length the
	/// HOST chose, making the same automation render differently at different block sizes.
	void advance(int hostSamples);

	/// How many host samples may be rendered before a deferred rebuild has to land.
	///
	/// The character layer's equivalent of "when is the next event": a renderer asks, renders no
	/// further than the answer, and a held character change then takes effect at the same sample
	/// whatever the block size. Very large when nothing is waiting, which is the ordinary case.
	///
	/// The rule lives here, with the guard it is about, rather than in the renderer — the renderer
	/// only has to honour a number, exactly as it already honours the next event's timestamp.
	int samplesUntilPendingBuild() const;

	/// False until the first setCharacter: there is nothing to render before that, and voices asked
	/// to render anyway write silence rather than reading an unbuilt table.
	bool hasTable() const override { return haveTable_; }

	/// Bumped by every flip. A voice compares this against the generation it last synchronised to;
	/// they differ exactly when it needs to swap its own pointers and arm its own fade.
	uint32_t generation() const override { return generation_; }

	const PreparedCharacter& current() const {
		prepare(current_, 0);
		return tables_[current_][0];
	}

	/// The table the current one replaced, which a voice armed on this generation is fading FROM.
	/// Identical to current() before the second build, which is why a first fade never happens.
	const PreparedCharacter& previous() const {
		prepare(1 - current_, 0);
		return tables_[1 - current_][0];
	}

	/// The tables BY EAR, 0 left and 1 right. A mono build answers with ear 0 for both — the two
	/// pointers are then EQUAL, which is how a voice can tell "the ears are the same table" without
	/// consulting a width anywhere: the fast path is pointer identity, structural rather than a
	/// number compared against zero. That rule holds through every fade combination — a voice fading
	/// out of a mono build into a stereo one reads previous(1) == previous(0), the old mono table,
	/// and one fading the other way reads current(1) == current(0), the new one.
	const PreparedCharacter& current(int ear) const override {
		const int e = slotStereo_[current_] ? ear : 0;
		prepare(current_, e);
		return tables_[current_][e];
	}
	const PreparedCharacter& previous(int ear) const {
		const int e = slotStereo_[1 - current_] ? ear : 0;
		prepare(1 - current_, e);
		return tables_[1 - current_][e];
	}

	/// True while any voice may need the two-ear render: the live build is stereo, or a stereo build
	/// is still inside the guard window some voice could be fading out of. False is the promise that
	/// every ear pointer in every voice is ear 0's table, so the mono render path — today's path,
	/// untouched — is exact, not approximate.
	bool stereoActive() const {
		return stereoWindow() || fmNeedsTwoEars();
	}

	/// stereoActive less its FM half: the BUILDS' stereo window alone, which is bookkeeping and makes no table. The
	/// synth asks this one while no voice is sounding (when the answer cannot change a sample) so that a character
	/// moved over silence is never built just to be asked whether it flips its FM.
	bool stereoWindow() const {
		return slotStereo_[current_] || (fadeGuardLeft_ > 0 && slotStereo_[1 - current_]);
	}

	/// WHETHER FM ALONE REQUIRES THE TWO-EAR PATH, which it can even at width zero.
	///
	/// FM doc §2 named this as the third thing with no clause for a second accumulator: the stereo
	/// path is gated on the CHARACTER BUILD being stereo, and a polarity flip needs two ears
	/// independent of width. So V11a's "width 0 renders bit-exact as today" becomes "width 0 AND FM
	/// off", and this is the "and FM off".
	///
	/// IT ASKS ABOUT THE FLIP, NOT ABOUT FM. A sound with FM at stereo width zero modulates both ears
	/// identically, so the mono path renders it exactly and there is no reason to pay for two — which
	/// matters, because the fields put SOME ratio and SOME width everywhere and a test on depth alone
	/// would push every FM patch onto the stereo path whether or not it had a stereo image to make.
	bool fmNeedsTwoEars() const {
		const auto flips = [](const PreparedCharacter& c) {
			return c.fmDepth() > 0.0f && c.fmStereoWidth() > 0.0f;
		};
		return flips(current())
		       || (fadeGuardLeft_ > 0 && flips(previous()));
	}

	/// Length of the fade a generation change arms, in host-rate samples. Never zero.
	int fadeLengthSamples() const override { return fadeLengthSamples_; }

	/// TEST ONLY. Clear the per-sample modifier depths on both prepared tables.
	///
	/// Every claim about what the modifiers DO is a claim about a difference, and the other side of
	/// that difference — this exact table with jitter and grit at zero — is not otherwise reachable,
	/// because the depths are zone-derived and no public call sets them. Zeroing the fields is the
	/// counterfactual itself rather than a simulation of it: the sample loop reads them and takes its
	/// off branches, so what a test subtracts is the real path minus the real path. The next
	/// setCharacter() rebuilds the depths. Nothing outside tests may call it.
	void clearModifiersForTest();

	/// TEST ONLY. How many recorded builds have been made into tables so far (a build is made when first read).
	uint64_t tablesMadeForTest() const { return tablesMade_; }
	/// How many tables have been built (prepared) so far — the spike recorder's "a rebake ran this block" (backlog SR).
	uint64_t tablesMade() const noexcept { return tablesMade_; }

	/// The CHARACTER the current table is (or will be, once read) built from, by ear — a question about the build that
	/// makes no table, for a caller that only needs to know what kind of build it is (the synth's phase request).
	const Character& currentCharacter(int ear) const {
		return (*slotCharacter_)[static_cast<size_t>(current_)][slotStereo_[current_] ? static_cast<size_t>(ear) : 0u];
	}

	/// One character into one prepared slot — the body every build shares, whichever ear or bank it is for. The build
	/// serial is stamped by the caller, which owns the counter.
	/// `scratch` is the calling bank's own PartialsScratch (read on a Partials build only).
	static void buildOne(const Character& c, PreparedCharacter& next, PartialsScratch& scratch);

private:
	/// The two doors above funnelled into one: dedup against the live request, defer behind the
	/// guard, build otherwise. A null `right` is a mono request.
	void request(const Character& left, const Character* right);

	/// Build into the spare slot and make it current. The expensive half of a request, split out
	/// because a deferred one runs it from advance() instead. Builds ear 0 always and ear 1 only for
	/// a stereo request — a mono build never pays for the second ear, which is what makes width 0
	/// cost what today costs.
	void build(const Character& left, const Character* right);

	/// [double-buffer slot][ear]. The second ear of a slot is dead storage unless slotStereo_ says
	/// that slot's build filled it — the accessors above never hand it out otherwise.
	/// MUTABLE because a const accessor is where a recorded build is made (`prepare`), on the audio thread like every
	/// build: the contents a reader sees are fixed by the record, so making them on first read changes no observable.
	mutable PreparedCharacter tables_[2][2];
	/// THE RECORD a slot's table is made from, per ear, and whether it has been made yet (see `build`).
	/// On the heap, made with the bank (never on the audio thread): four characters are 56 KB, and a bank lives inside
	/// every Synth, which tests build several of on the stack.
	using SlotRecords = std::array<std::array<Character, 2>, 2>;
	std::unique_ptr<SlotRecords> slotCharacter_ = std::make_unique<SlotRecords>();
	mutable bool unprepared_[2][2] {};
	/// How many recorded builds have been made into tables: the count a case reads to see that nothing was made.
	mutable uint64_t tablesMade_{0};
	/// Make slot `slot`'s table for `ear` from its record, if it has not been made since its last build.
	void prepare(int slot, int ear) const;
	bool slotStereo_[2] {false, false};
	int current_{0};
	bool haveTable_{false};

	Character live_[2] {};
	bool liveStereo_{false};
	Character pending_[2] {};
	bool pendingStereo_{false};
	bool havePending_{false};

	double hostSampleRate_{48000.0};
	int fadeLengthSamples_{96};

	/// Host samples until every voice armed on the last flip has certainly finished fading. Counts
	/// the fade plus one decimator chunk: a voice serves up to three samples out of its carry before
	/// it generates anything, so its fade can start that far behind the flip.
	int fadeGuardLeft_{0};

	uint32_t generation_{0};

	/// Stamped onto every prepared table and never reused — see PreparedCharacter::buildSerial for
	/// what goes wrong without it. Starts at 1 so that zero means "never built".
	uint64_t buildSerial_{0};
	/// The additive build's working spectra (PartialsScratch says why they are a member and why one is enough).
	mutable PartialsScratch partialsScratch_ {};
};


/// **ONE STAND OF A STACK: A MONO BANK** (voice stacking step 4c; the doc's §5, step 4c). The same contract as
/// `CharacterBank` for one ear — a double-buffered table, the deferral behind the fade guard, the dedupe against the
/// live request, a build serial — at half its size (~34.5 KB against 69 KB, measured), because a stand is one place
/// and never two ears. The synth holds a fixed pool of these, keyed by stand.
class StandBank final : public TableSource {
public:
	StandBank();

	void setSampleRate(double hostSampleRate);
	/// `CharacterBank::setCharacter`'s contract: at once when no fade is guarded, deferred behind the guard
	/// otherwise, a request equal to the live one dropped.
	void setCharacter(const Character& c);
	/// `CharacterBank::advance`'s contract.
	void advance(int hostSamples);
	/// `CharacterBank::samplesUntilPendingBuild`'s contract.
	int samplesUntilPendingBuild() const;

	bool hasTable() const override { return haveTable_; }
	uint32_t generation() const override { return generation_; }
	/// A mono bank answers every ear with its one table — the pointer equality a voice reads as "one table".
	const PreparedCharacter& current(int ear) const override {
		(void)ear;
		return tables_[current_];
	}
	const PreparedCharacter& current() const { return tables_[current_]; }
	int fadeLengthSamples() const override { return fadeLengthSamples_; }
	/// Whether the live table was built for exactly `c` — what the synth asks before seating a part here.
	bool holds(const Character& c) const { return haveTable_ && live_ == c; }

private:
	void build(const Character& c);

	PreparedCharacter tables_[2];
	int current_{0};
	bool haveTable_{false};
	Character live_ {};
	Character pending_ {};
	bool havePending_{false};
	double hostSampleRate_{48000.0};
	int fadeLengthSamples_{96};
	int fadeGuardLeft_{0};
	uint32_t generation_{0};
	uint64_t buildSerial_{0};
	/// The additive build's working spectra (PartialsScratch says why they are a member and why one is enough).
	PartialsScratch partialsScratch_ {};
};

} // namespace feathers
