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

#include "device/state_blocks.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace feathers {

/// **A LIKE ON THE TERRAIN** (backlog ON): "more like this". A like is about the SOUND, not the place,
/// so what it stores is the place AND what the sound was there at the moment of the like — its SPECTRUM (below; it was the
/// twelve harmonics of the 32-sample lens until the Hellinger fingerprint). Captured state (CLAUDE.md's persistence
/// rule): the envelope is stored rather than re-measured on load, because what the like is OF is what was heard when it
/// was made, and a later knob turn must not rewrite it. Everything the likes are used for — the score, its weights, the
/// warp — is DERIVED from these and never stored.
/// THE SPECTRUM A LIKE STORES: power in SIXTH-OCTAVE bins of harmonic number — bin b holds harmonics k
/// with 6 log2 k in [b, b+1), so harmonics 1..255 in 48 bins — measured from a fine cycle (ui::kLikeFineCycle samples).
/// A FIXED grid, so the bands a fingerprint is made of can change without re-measuring anything.
inline constexpr size_t kTerrainLikeFine = 48;
/// THE LARGEST FULL POINT a like can need: V5's, its morph, stair and vox rows (35 + 9 + 12 triangles, a phase and a
/// rate row each). slice_basis.h's counts; restated as a number here so this header stays free of the DSP.
inline constexpr size_t kTerrainLikePoint = 2 * (35 + 9 + 12);
/// AND THE PHASE WARP'S SITES (dsp/phase_warp.h kAngleSites: Morph 35, operator authors 10, Additive partials 340, Stair 5,
/// Resonate 22, vowel 12, the string's material 5),
/// restated as a number for the same reason; terrain.cpp asserts the two agree.
inline constexpr size_t kTerrainLikeAngles = 35 + 10 + 340 + 5 + 22 + 12 + 5 + 7;
struct TerrainLike {
	/// WHERE: the selected sound's place in the decoded plane (`SoundTravel::offset`), real coordinates — never the warped
	/// view's — and the home the plane was taken at (`sliceHome` of the sound's dials and gamma base), which is what says
	/// whether a like is on the plane being drawn.
	double x = 0.0;
	double y = 0.0;
	double home = 0.0;
	/// WHAT: the power of the cycle there, in the fine bins.
	std::array<float, kTerrainLikeFine> spectrum {};
	/// **THE FULL POINT** (2026-10-28: a place stored as plane (x, y) did not follow a rotate — the mark stayed put and
	/// what it named silently changed). The like's place in its engine's whole triangle space — every phase and rate row
	/// the engine reads (`ui::terrainPlacePoint`: V5 morph + stair + vox, Additive partials, ...), decoded on the plane it was
	/// made on. THE TRUTH OF WHERE THE LIKE IS: on any later plane its mark is this point's projection
	/// (`ui::terrainPlaceOn`), faded with its distance off that plane; `x`, `y` are kept beside it so the plane it was made
	/// on — undo, a generator restored — gives the place back to the bit.
	std::array<double, kTerrainLikePoint> point {};
	uint16_t pointCount = 0;
	/// **THE ANGLES AS HEARD** (P-MÖBIUS, `terrain.likes.v7`): every phase-warp site's wrapped angle at the
	/// like, in dsp/phase_warp.h's flat layout, captured on the plane it was made on — what the warp's fit reads, so a
	/// rotate or a knob cannot retarget it, and the newest like's pin is exact against the stance's own angles. Zero on an
	/// engine with no sites.
	std::array<float, kTerrainLikeAngles> angles {};
	/// WHICH TERRAIN: the engine (an OscType index) and the sound (its slot) the like was made on. Another engine's envelope means nothing here, so a like is applied and drawn only on its
	/// own engine and sound, and kept for when they are selected again.
	uint16_t engine = 0;
	uint16_t slot = 0;
	/// AND ON DRUM, THE KIND (2026-11-05, `terrain.likes.v8`; drum proposal §3.5): a pad's like is of the drum it was made on, so a
	/// pad switched to another kind is not warped by its old kind's likes. 0 on every other engine.
	uint16_t kind = 0;
	friend bool operator==(const TerrainLike&, const TerrainLike&) = default;
};

/// PER PATCH, never global: every like the patch holds, in the order they were made.
/// 64 since likes are kept per engine and sound, every engine's together.
inline constexpr size_t kMaxTerrainLikes = 64;
struct TerrainLikes {
	std::vector<TerrainLike> likes;
	friend bool operator==(const TerrainLikes&, const TerrainLikes&) = default;
};

/// THE KEY the `likes` block stores them under, and its fixed length (tests/ledger/core.ledger): a count, then
/// kMaxTerrainLikes slots of x, y, home (doubles), its engine and sound, the 48 fine bins (floats), and the full point (its count,
/// then kTerrainLikePoint doubles), each by its bits in 16-bit words
/// (device::BlockWriter). Unused slots are zero.
/// `terrain.likes.v8` since the drum's seven fields joined the sites and a like stores its drum kind (v7: the string's material joined; v6: every engine's but Pluck's; v5: V5's, Morph's and Additive's only; v4: its full point; v3: its fine spectrum rather than twelve harmonic magnitudes; v2 added the engine and
/// sound; each change of layout retires the old key,
/// params.h kCoreRetiredKeys).
inline constexpr const char* kTerrainLikesKey = "terrain.likes.v8";
inline constexpr size_t kTerrainLikeWords = 3 * 4 + kTerrainLikeFine * 2 + 2 + 1 + 1 + kTerrainLikePoint * 4 + kTerrainLikeAngles * 2;
/// THE LONGEST the entry can be: every slot written. **A SAVE WRITES ONLY THE SLOTS IT USES (backlog OQ, 2026-10-30)**:
/// `terrainLikesFloatsFor(count)`. MEASURED: with the full table, ONE like took a patch from ~10 KB to ~370 KB (64
/// slots of 1,307 words, 4 bytes each, nearly all zero); a reader takes any whole number of slots up to this, so a
/// full-length entry from an earlier save still reads.
inline constexpr size_t kTerrainLikesFloats = 1 + kMaxTerrainLikes * kTerrainLikeWords;
[[nodiscard]] constexpr size_t terrainLikesFloatsFor(size_t count) noexcept {
	return 1 + (count < kMaxTerrainLikes ? count : kMaxTerrainLikes) * kTerrainLikeWords;
}

/// `n` floats at `out`: the count, then as many whole slots as `n` holds (`terrainLikesFloatsFor`), unused ones zero.
inline void writeTerrainLikes(const TerrainLikes& likes, float* out, size_t n = kTerrainLikesFloats) noexcept {
	device::BlockWriter w(out, n);
	const size_t slots = n > 0 ? std::min((n - 1) / kTerrainLikeWords, kMaxTerrainLikes) : 0;
	const size_t count = std::min(likes.likes.size(), slots);
	w.u16(static_cast<uint32_t>(count));
	for (size_t i = 0; i < slots; ++i) {
		const TerrainLike like = i < count ? likes.likes[i] : TerrainLike {};
		w.f64(like.x);
		w.f64(like.y);
		w.f64(like.home);
		w.u16(like.engine);
		w.u16(like.slot);
		w.u16(like.kind);
		for (const float p : like.spectrum) {
			w.f32(p);
		}
		w.u16(like.pointCount);
		for (const double v : like.point) {
			w.f64(v);
		}
		for (const float a : like.angles) {
			w.f32(a);
		}
	}
}

/// False for a block not this layout's, or one holding a count past the cap or a non-finite (or negative) value: the
/// caller keeps no likes rather than a part of them.
inline bool readTerrainLikes(const float* in, size_t n, TerrainLikes* out) {
	// ANY WHOLE NUMBER OF SLOTS UP TO THE CAP (backlog OQ: a save writes only those it uses); anything else is not this
	// layout.
	if (n < 1 || (n - 1) % kTerrainLikeWords != 0 || (n - 1) / kTerrainLikeWords > kMaxTerrainLikes) {
		return false;
	}
	const size_t slots = (n - 1) / kTerrainLikeWords;
	device::BlockReader r(in, n);
	const uint32_t count = r.u16();
	if (count > slots) {
		return false;
	}
	TerrainLikes read;
	for (size_t i = 0; i < slots; ++i) {
		TerrainLike like;
		like.x = r.f64();
		like.y = r.f64();
		like.home = r.f64();
		like.engine = static_cast<uint16_t>(r.u16());
		like.slot = static_cast<uint16_t>(r.u16());
		like.kind = static_cast<uint16_t>(r.u16());
		bool finite = std::isfinite(like.x) && std::isfinite(like.y) && std::isfinite(like.home);
		for (float& p : like.spectrum) {
			p = r.f32();
			finite = finite && std::isfinite(p) && p >= 0.0f;
		}
		like.pointCount = static_cast<uint16_t>(r.u16());
		finite = finite && like.pointCount <= kTerrainLikePoint;
		for (double& v : like.point) {
			v = r.f64();
			finite = finite && std::isfinite(v);
		}
		for (float& v : like.angles) {
			v = r.f32();
			finite = finite && std::isfinite(v) && v >= 0.0f && v < 1.0f;
		}
		if (i < count) {
			if (!finite) {
				return false;
			}
			read.likes.push_back(like);
		}
	}
	if (!r.ok() || count > kMaxTerrainLikes) {
		return false;
	}
	*out = std::move(read);
	return true;
}

} // namespace feathers
