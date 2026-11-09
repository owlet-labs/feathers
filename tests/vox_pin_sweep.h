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
#include "dsp/osc_voice.h"

#include <array>
#include <cstddef>
#include <cstdint>

// THE VOWEL-FREE SWEEP, and the renders it is pinned by — shared between the live suite and the
// standalone capture program that produced the hashes.
//
// ONE FILE, TWO CONSUMERS, AND THAT IS THE WHOLE POINT. The pin's claim is that terrain with no
// vowel in it renders the same bits it rendered before vox existed. Evidence for that needs the
// SAME render driven at two different revisions of the tree, and two copies of "the same render"
// is a claim nobody can check — one of them drifts, quietly, and the pin goes on passing against
// a baseline it no longer corresponds to. So the spots, the schedules and the hash live here, and
// the capture program at e7f1690 compiled this exact text.
//
// NOTHING HERE MAY MENTION VOX. That is not squeamishness: e7f1690 is the last revision with no
// vox code in any shared file, and this header has to compile there. Everything it touches —
// Character, SoundCoord, CharacterBank, OscVoice, setCharacter, render, renderStereo — predates
// vox entirely. The old build never has to know what a vowel is.
//
// HOW THE SPOTS WERE FOUND, since a literal list cannot say it for itself: for each of six zones,
// four travels, two sound counts and four gamma bases, an offset scan at stride 0.0137 took the
// first position whose weighted species presence was exactly zero — each scan starting 977 steps
// further along than the last, because the species period is about six home units and home 0 sits
// in its deadzone, so scans all starting at zero would have returned forty-eight readings of the
// same place. Six more were asked for presence zero AND all three operator fields zero, to stand on
// decision V8's other render mechanism: quantize's duty is 0.85, so an unconstrained scan lands on an
// operated position forty-seven times in forty-eight.
//
// THE SPOTS ARE LITERALS, NOT A SEARCH. They were found by searching the species field, once, and
// then frozen. A pin whose positions are recomputed at test time is a statement about "wherever
// the field currently says there is no vowel", which moves the moment anybody retunes the field —
// and vox_bake.h says outright that those numbers are audition-tunable. Frozen, it is what a
// regression pin should be: named places, and the bits they have always rendered.

namespace feathers::pin {

struct PinSpot {
	uint16_t zone;
	float offset;
	double gamma;
	double travelX;
	double travelY;
	int sounds;
};

inline constexpr PinSpot kSpots[] = {
	{ 120, 0.0f, 0.0, 0, 0, 1 },
	{ 120, 13.3849001f, 3.5, 0, 0, 2 },
	{ 120, 26.7698002f, 41.0, 0.35, -0.11, 1 },
	{ 120, 40.1547012f, 617.0, 0.35, -0.11, 2 },
	{ 120, 53.5396004f, 0.0, -0.8, 0.5, 1 },
	{ 120, 67.5547028f, 3.5, -0.8, 0.5, 2 },
	{ 120, 80.5286026f, 41.0, 2, 2, 1 },
	{ 120, 94.324501f, 617.0, 2, 2, 2 },
	{ 300, 107.8601f, 0.0, 0, 0, 1 },
	{ 300, 122.642403f, 3.5, 0, 0, 2 },
	{ 300, 133.848999f, 41.0, 0.35, -0.11, 1 },
	{ 300, 148.590195f, 617.0, 0.35, -0.11, 2 },
	{ 300, 160.687302f, 0.0, -0.8, 0.5, 1 },
	{ 300, 174.770905f, 3.5, -0.8, 0.5, 2 },
	{ 300, 187.881805f, 41.0, 2, 2, 1 },
	{ 300, 200.773499f, 617.0, 2, 2, 2 },
	{ 512, 214.158401f, 0.0, 0, 0, 1 },
	{ 512, 227.543304f, 3.5, 0, 0, 2 },
	{ 512, 243.394196f, 41.0, 0.35, -0.11, 1 },
	{ 512, 254.313095f, 617.0, 0.35, -0.11, 2 },
	{ 512, 267.697998f, 0.0, -0.8, 0.5, 1 },
	{ 512, 281.082886f, 3.5, -0.8, 0.5, 2 },
	{ 512, 294.467804f, 41.0, 2, 2, 1 },
	{ 512, 307.852692f, 617.0, 2, 2, 2 },
	{ 640, 321.23761f, 0.0, 0, 0, 1 },
	{ 640, 335.554108f, 3.5, 0, 0, 2 },
	{ 640, 348.007385f, 41.0, 0.35, -0.11, 1 },
	{ 640, 361.679993f, 617.0, 0.35, -0.11, 2 },
	{ 640, 376.078705f, 0.0, -0.8, 0.5, 1 },
	{ 640, 388.162109f, 3.5, -0.8, 0.5, 2 },
	{ 640, 401.546997f, 41.0, 2, 2, 1 },
	{ 640, 415.699097f, 617.0, 2, 2, 2 },
	{ 777, 430.316986f, 0.0, 0, 0, 1 },
	{ 777, 441.701691f, 3.5, 0, 0, 2 },
	{ 777, 455.771606f, 41.0, 0.35, -0.11, 1 },
	{ 777, 471.348511f, 617.0, 0.35, -0.11, 2 },
	{ 777, 481.856415f, 0.0, -0.8, 0.5, 1 },
	{ 777, 495.241302f, 3.5, -0.8, 0.5, 2 },
	{ 777, 508.62619f, 41.0, 2, 2, 1 },
	{ 777, 522.011108f, 617.0, 2, 2, 2 },
	{ 900, 535.395996f, 0.0, 0, 0, 1 },
	{ 900, 549.424805f, 3.5, 0, 0, 2 },
	{ 900, 562.165771f, 41.0, 0.35, -0.11, 1 },
	{ 900, 575.660278f, 617.0, 0.35, -0.11, 2 },
	{ 900, 589.332886f, 0.0, -0.8, 0.5, 1 },
	{ 900, 604.608398f, 3.5, -0.8, 0.5, 2 },
	{ 900, 615.705383f, 41.0, 2, 2, 1 },
	{ 900, 629.090271f, 617.0, 2, 2, 2 },
	{ 120, 5.21969986f, 0.0, 0, 0, 1 },
	{ 300, 1.53439999f, 3.5, 0, 0, 1 },
	{ 512, 2.31529999f, 41.0, 0, 0, 1 },
	{ 640, 9.37080002f, 617.0, 0, 0, 1 },
	{ 777, 4.57579994f, 0.0, 0, 0, 1 },
	{ 900, 0.958999991f, 3.5, 0, 0, 1 },
};

inline constexpr size_t kSpotCount = std::size(kSpots);

/// The character a spot names. A one-sound spot stands on its zone alone; a two-sound spot puts a
/// second sound 137 zones away at weight 0.4, so the blend road is in the sweep rather than beside
/// it.
inline Character characterOf(const PinSpot& s) {
	Character c;
	c.engine = OscEngine::Morph;
	c.flagship = true;
	c.count = static_cast<uint8_t>(s.sounds);
	c.sounds[0].zone = s.zone;
	c.sounds[0].phaseOffset = s.offset;
	c.sounds[0].gammaBase = s.gamma;
	c.sounds[0].travelX = s.travelX;
	c.sounds[0].travelY = s.travelY;
	c.sounds[0].weight = s.sounds == 1 ? 1.0 : 0.6;
	if (s.sounds > 1) {
		c.sounds[1] = c.sounds[0];
		c.sounds[1].zone = static_cast<uint16_t>((s.zone + 137) % 1024);
		c.sounds[1].weight = 0.4;
	}
	return c;
}

inline constexpr int kPinSamples = 512;
inline constexpr double kPinRate = 48000.0;

using PinBuffer = std::array<float, kPinSamples>;

/// FNV-1a over the raw bytes. A hash rather than stored buffers because the claim is bit-identity,
/// and a hash that matches IS bit-identity — no tolerance can be quietly introduced into one later,
/// which a float comparison is always one edit away from.
inline uint64_t pinHash(const void* data, size_t bytes) {
	const auto* p = static_cast<const unsigned char*>(data);
	uint64_t h = 1469598103934665603ull;
	for (size_t i = 0; i < bytes; ++i) {
		h ^= p[i];
		h *= 1099511628211ull;
	}
	return h;
}

/// A parked note, rendered in one call. THREE OF THESE PER SPOT, at notes chosen to land on all
/// three rungs of the mip ladder — the switch points are 700 and 1800 Hz, and a sweep that never
/// went above 1800 left flagshipMipSlots' dullest branch unpinned.
inline PinBuffer pinRenderMono(const Character& c, double noteHz) {
	CharacterBank bank;
	bank.setSampleRate(kPinRate);
	bank.setCharacter(c);
	OscVoice voice;
	voice.setCharacterBank(&bank);
	voice.setSampleRate(kPinRate);
	voice.setFrequency(noteHz);
	PinBuffer out {};
	voice.render(out.data(), kPinSamples);
	bank.advance(kPinSamples);
	return out;
}

/// A GLIDING note, rendered in three unequal calls. The glide road re-resolves the mip every
/// evaluation sample and crosses both switch points on the way — 40 to 76 semitones is 82 Hz to
/// 659 Hz at this glide length, and the unequal calls put chunk boundaries where a single call
/// would not.
inline PinBuffer pinRenderGlide(const Character& c) {
	CharacterBank bank;
	bank.setSampleRate(kPinRate);
	bank.setCharacter(c);
	OscVoice voice;
	voice.setCharacterBank(&bank);
	voice.setSampleRate(kPinRate);
	voice.setPitch(40.0);
	voice.setGlideSeconds(0.05);
	voice.glideToPitch(88.0);
	PinBuffer out {};
	int done = 0;
	for (const int take : { 37, 200, 275 }) {
		voice.render(out.data() + done, take);
		bank.advance(take);
		done += take;
	}
	return out;
}

/// TWO EARS, TWO CHARACTERS — the terrain-stereo road, which no earlier version of this sweep
/// touched at all. Hashes both channels together, so a build that collapsed one onto the other
/// would move the hash.
inline PinBuffer pinRenderStereo(const Character& left, const Character& right, double noteHz,
                                 PinBuffer& outR) {
	CharacterBank bank;
	bank.setSampleRate(kPinRate);
	bank.setCharacterStereo(left, right);
	OscVoice voice;
	voice.setCharacterBank(&bank);
	voice.setSampleRate(kPinRate);
	voice.setFrequency(noteHz);
	PinBuffer outL {};
	voice.renderStereo(outL.data(), outR.data(), kPinSamples);
	bank.advance(kPinSamples);
	return outL;
}

/// How many stereo pairs the sweep renders, and which spots pair up: spot i with spot i + half,
/// so the two ears are genuinely different country rather than neighbours.
inline constexpr size_t kStereoPairs = 6;

inline size_t stereoPartner(size_t i) {
	return (i + kSpotCount / 2) % kSpotCount;
}

/// THE PARTITION, FROZEN AS DELIBERATELY AS THE COORDINATES THEMSELVES.
///
/// The spots never move. What moved is the ENGINE underneath them: the smooth source (V5's fourth
/// field) replaces what the operator chain reads, so at the fourteen positions below the render is
/// no longer the one e7f1690 produced. That is the feature working, not the pin failing.
///
/// SO THE CLAIM SPLITS RATHER THAN THE HASHES BEING RE-CAPTURED. The forty spots outside smooth
/// country keep their e7f1690 numbers and keep testing the original sentence word for word — bits
/// rendered before vox existed. These fourteen move to their own recorded set with their own,
/// weaker claim: these positions render deterministically, pinned from the day the field landed.
/// Re-capturing all 228 was rejected because e7f1690 is the last revision with no vox code in any
/// shared file and the tree cannot go back — that evidence can be spent once and never rebuilt.
///
/// FORCING THE FIELD OFF FOR THE PIN WAS ALSO REJECTED, and for a reason worth keeping: it would
/// need a test-visible switch that WRITES to the character path. Every test seam in this codebase
/// observes — voicePhaseForTest, voiceFilterTargetForTest, mipForTest, tickForTest — and a door
/// that changes what the engine computes is permanent, reachable from anywhere, and the next field
/// would want one too.
///
/// THIS LIST IS A CLAIM ABOUT THE SMOOTHNESS FIELD, so a change to that field's rate, duty or
/// offset MUST fail here. That is the point of freezing it rather than deriving it: a derived
/// partition would silently re-select itself and the pin would quietly test something else. If this
/// list stops matching, the field moved — re-derive it deliberately and say so.
inline constexpr size_t kSmoothSpots[] = { 2, 10, 12, 16, 18, 23, 31, 32, 46, 48, 49, 50, 52, 53 };

inline bool spotIsSmooth(size_t spot) {
	for (const size_t s : kSmoothSpots) {
		if (s == spot) {
			return true;
		}
	}
	return false;
}

/// Which claim a HASH INDEX belongs to. The sweep is spot-major — four renders per spot, then two
/// per stereo pair — so this is the one place that mapping is written down.
///
/// PARTITIONED PER EAR, NOT PER PAIR, and that correction came from measurement rather than
/// reasoning. The obvious assumption is that a stereo pair cannot be split because one call renders
/// both characters, so a pair with either side in smooth country must move whole. That is FALSE and
/// the hashes say so: with pairs {0,27}…{5,32}, spots 2, 31 and 32 are in smooth country while
/// their partners 29, 4 and 5 are not — and those three partner EARS came back bit-identical to
/// e7f1690 while the ears beside them changed. The ears are independent renders that merely travel
/// together, so each belongs to the claim its OWN spot belongs to. Assuming otherwise would have
/// retired three hashes of genuine pre-vox evidence for nothing.
inline bool hashIsSmooth(size_t hash) {
	const size_t monoCount = kSpotCount * 4;
	if (hash < monoCount) {
		return spotIsSmooth(hash / 4);
	}
	const size_t relative = hash - monoCount;
	const size_t pair = relative / 2;
	const bool rightEar = (relative % 2) == 1;
	return spotIsSmooth(rightEar ? stereoPartner(pair) : pair);
}

/// FRESH SPOTS FOR THE SMOOTH ROAD'S OWN PIN, and the reason they exist is a coverage repair rather
/// than a wish for more hashes.
///
/// Partitioning the frozen 54 cost one dimension badly: five of the six spots with the OPERATOR TIER
/// AT REST sit in the low-home cluster the smoothness field falls across, so the pre-vox claim keeps
/// just one witness that a place with no operators renders the way it always did. That one is
/// irreplaceable and it stays exactly where it is — e7f1690 cannot be reached again, so its evidence
/// can be spent but never made.
///
/// THE CURRENT-BEHAVIOUR CLAIM HAS NO SUCH SCARCITY. Its hashes are recorded here and now, so the
/// answer to a thin dimension is simply to cover it properly: EIGHT fresh operator-corner spots,
/// against the six the whole file had before, every one of them inside smooth country and chosen to
/// SPAN it rather than to sit where the old ones happened to. All six zones, gamma at 0, 3.5, 41 and
/// 617, all three travel settings, both sound counts, smoothness from 0.60 to 0.94 and point counts
/// from 5.46 to 7.40. Vowel-free like every other spot in this file, so the vowel and the cubic are
/// never being asked about in the same breath.
///
/// NO SPOT HERE EVER PRETENDS TO BE PRE-VOX EVIDENCE, and that is the precedent this sets: the
/// frozen set never grows and never moves, and each field that lands after this brings its own
/// pinned set, its own recorded hashes and its own claim. The e7f1690 file becomes a shrinking,
/// honest historical record instead of something there is ever a reason to re-capture.
inline constexpr PinSpot kSmoothExtraSpots[] = {
	{ 120, 5.2196999f, 0.0, 0.00, 0.00, 1 },
	{ 300, 1.5481000f, 3.5, 0.00, 0.00, 2 },
	{ 512, 2.3701000f, 41.0, 0.35, -0.11, 1 },
	{ 640, 38.4558983f, 617.0, 0.35, -0.11, 2 },
	{ 777, 4.1374002f, 0.0, -0.80, 0.50, 2 },
	{ 900, 0.4932000f, 3.5, -0.80, 0.50, 1 },
	{ 300, 39.3875008f, 617.0, 0.00, 0.00, 1 },
	{ 512, 4.7950001f, 0.0, 0.35, -0.11, 2 },
};

inline constexpr size_t kSmoothExtraCount = std::size(kSmoothExtraSpots);

/// Four INTACT stereo pairs of their own, spot i with spot i + 4.
///
/// The 54-spot sweep's pairs are partitioned per EAR — measurement showed the two ears are
/// independent renders that merely travel together, so holding a plain ear to its e7f1690 number is
/// both possible and stricter than re-recording it. The cost is that no pair sits wholly in either
/// claim. These four give the smooth pin genuine whole pairs, so "two different characters render
/// two correct ears" is tested inside one table rather than across two.
inline constexpr size_t kSmoothExtraPairs = 4;

/// The smooth road's own traversal — the same four renders per spot and the same stereo call, so a
/// buffer here means exactly what a buffer there means.
template <typename Emit>
void smoothExtraSweep(Emit&& emit) {
	for (size_t i = 0; i < kSmoothExtraCount; ++i) {
		const Character c = characterOf(kSmoothExtraSpots[i]);
		emit(pinRenderMono(c, 55.0));
		emit(pinRenderMono(c, 1200.0));
		emit(pinRenderMono(c, 2600.0));
		emit(pinRenderGlide(c));
	}
	for (size_t p = 0; p < kSmoothExtraPairs; ++p) {
		PinBuffer right {};
		const PinBuffer left = pinRenderStereo(characterOf(kSmoothExtraSpots[p]),
		                                       characterOf(kSmoothExtraSpots[p + kSmoothExtraPairs]),
		                                       196.0, right);
		emit(left);
		emit(right);
	}
}

inline constexpr size_t kSmoothExtraHashCount = kSmoothExtraCount * 4 + kSmoothExtraPairs * 2;

/// EVERY BUFFER THE SWEEP PRODUCES, in order — the one traversal both consumers drive, so "the same
/// render in the same order" is a fact about this file rather than a promise about two of them.
///
/// It hands over BUFFERS rather than hashes so that the live suite can also look at what it got. A
/// hash cannot tell a correct render from silence, and silence hashes as reliably as anything else;
/// the capture program only needs the hash, and the suite needs to know the thing it hashed was a
/// waveform.
template <typename Emit>
void pinSweep(Emit&& emit) {
	for (size_t i = 0; i < kSpotCount; ++i) {
		const Character c = characterOf(kSpots[i]);
		emit(pinRenderMono(c, 55.0));
		emit(pinRenderMono(c, 1200.0));
		emit(pinRenderMono(c, 2600.0));
		emit(pinRenderGlide(c));
	}
	for (size_t p = 0; p < kStereoPairs; ++p) {
		PinBuffer right {};
		const PinBuffer left =
		    pinRenderStereo(characterOf(kSpots[p]), characterOf(kSpots[stereoPartner(p)]), 196.0,
		                    right);
		emit(left);
		emit(right);
	}
}

inline constexpr size_t kPinHashCount = kSpotCount * 4 + kStereoPairs * 2;

} // namespace feathers::pin
