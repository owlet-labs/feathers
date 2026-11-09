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

#include "mod/arp/genes.h"
#include "mod/arp/pattern.h"

#include <array>
#include <cstddef>
#include <cstdint>

// LIKES ON THE PATTERN TERRAIN (backlog ON, "more like this"). By design they are keyed by
// just the pattern slot — each of the four patterns keeps its own likes.
//
// A LIKE IS A PLACE AND WHAT WAS HEARD THERE: the pattern's terrain place and the FINGERPRINT of the cycle it was
// playing, captured at the moment of the like and never re-derived (CLAUDE.md: captured state persists; a later knob
// turn must not rewrite what was liked). The fingerprint is the spectrum's musical analogue: rhythm, contour, register,
// articulation, chords and the Mode family, each block normalised, compared block by block (Hellinger), as Feathers'
// likes compare spectra.

namespace feathers::mod::arp::likes {

/// THE FINGERPRINT'S BLOCKS, in order.
inline constexpr size_t kRhythm = 16;
inline constexpr size_t kContour = 12;
inline constexpr size_t kRegister = 6;
inline constexpr size_t kArticulation = 6;
inline constexpr size_t kChords = 4;
inline constexpr size_t kModeBins = 4;
inline constexpr size_t kFingerprint = kRhythm + kContour + kRegister + kArticulation + kChords + kModeBins;
static_assert(kFingerprint == 48, "the spectrum's 48 bins, by design");
using Fingerprint = std::array<float, kFingerprint>;

/// THE MODE FAMILY a fingerprint's last block names: Melody, Melody Mono, Chords, TB (TB-Mono and TB-Glide are one).
[[nodiscard]] size_t familyOf(genes::Mode mode) noexcept;

/// **THE FINGERPRINT OF A CYCLE** (design §14-bis): rhythm — onsets folded onto 16 positions, weighted by velocity;
/// contour — the interval from each note to the next, −6..+5 scale-ish semitone bins (octave+ in the ends); register —
/// the notes' height above the held root in half-octaves; articulation — note, tie, rest, accent, slide and ratchet
/// shares; chords — chord share, mean tones, degree movement, voicing span; the Mode family, one-hot. Each block sums
/// to 1 (or is all zero when it has nothing to say). A pure function of the cycle.
[[nodiscard]] Fingerprint fingerprintOf(const pattern::PatternCycle& cycle, genes::Mode mode, int rootKey) noexcept;

/// **HOW FAR APART TWO FINGERPRINTS ARE**: Hellinger per block, weighted (rhythm 0.35, contour 0.25, register 0.15,
/// articulation 0.15, chords 0.10 — by ear); ∞ (a large number) across Mode families, which are not compared.
[[nodiscard]] double distance(const Fingerprint& a, const Fingerprint& b) noexcept;
inline constexpr double kAcrossFamilies = 1e9;

/// ONE LIKE: where (the pattern's place controls) and what was heard.
struct Like {
	float zone = 0.0f;
	float zoneOffset = 0.0f;
	float gamma = 0.0f;
	float placeX = 0.0f;
	float placeY = 0.0f;
	float radius = 0.0f;
	float theta = 0.0f;
	float rotate = 0.0f;
	Fingerprint heard {};
	/// **THE FULL POINT** (Feathers' rule, 2026-10-28: a place stored as plane (x, y) did not follow a rotate): every
	/// gene's phase, then every rate's offset from 1, as `PatternDevice::placeOf` decoded the place — what the pattern
	/// heard there IS, so a mark can be projected onto whatever plane is drawn now (`pat::markOn`).
	std::array<double, 2 * genes::kRowCount> point {};
};

inline constexpr size_t kPatterns = 4;
inline constexpr size_t kMaxLikes = 16;
struct SlotLikes {
	std::array<Like, kMaxLikes> like {};
	size_t count = 0;
};
using AllLikes = std::array<SlotLikes, kPatterns>;

/// **MORE LIKE THIS**: `Σ e^{−d²/σ²}` over a slot's likes, at most 1 — how much a fingerprint resembles what was liked.
inline constexpr double kScoreSigma = 0.35;
[[nodiscard]] double score(const SlotLikes& likes, const Fingerprint& f) noexcept;

/// **THE BLOCK `pattern.likes.v3`** (v1, without the point, and v2, its point in floats, are retired): per slot a count, then each like's eight place floats, 48 fingerprint floats and
/// its full point (2 × 31 doubles, each as two floats: the float nearest and what it missed by, so a phase keeps its
/// fraction however far out the home is), all as 16-bit words (`device::BlockWriter`'s f32: two words each).
inline constexpr uint32_t kLikeWords = (8 + static_cast<uint32_t>(kFingerprint) + 4 * static_cast<uint32_t>(genes::kRowCount)) * 2;
inline constexpr uint32_t kSlotWords = 1 + static_cast<uint32_t>(kMaxLikes) * kLikeWords;
inline constexpr uint32_t kBlockWords = kSlotWords * static_cast<uint32_t>(kPatterns);
void write(const AllLikes& likes, float* out) noexcept;
[[nodiscard]] bool read(const float* in, size_t n, AllLikes* out) noexcept;

} // namespace feathers::mod::arp::likes
