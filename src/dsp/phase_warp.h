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

// THE LIKES AS A POST-WRAP PHASE WARP, P-MÖBIUS (backlog ON;
// shipped provisionally 2026-10-28). Every φ value a sound's character reads off its plane is tri(θ) for one wrapped angle
// θ (phi::evalTriangle).
// P-MÖBIUS warps each such SITE's angle on its own circle, θ' = W_i(θ), W_i the transport of the uniform circle onto
// w0·uniform + a wrapped-Cauchy mixture fitted to the likes' angles at that site — held as a LUT, applied in evalTriangle.
//
// THE SITES are numbered by KIND (the builder that reads them) and by their order within one call of it, which is the
// same at every place: the morph table (buildMorphSliced, 35), the V5 and Basic Shapes operator authors (flagshipAuthorsAt,
// 10), the Additive partials (buildPartialSound, 320 harmonics then 20 fields), the Stair table (buildStairSliced, 5), the
// Resonate string's laws (buildWeaveSliced, 22) and V5's vowel (buildVoxSliced, 12) — every builder of an engine's sound
// that reads a plane triangle through evalTriangle. NOT sites: the landscape reads (Stair's height and asymmetry patterns,
// Resonate's per-node lands), which wrap position·cycles + phase with their own arithmetic and no evalTriangle, and the
// vowel's PRESENCE field (vox_bake voxPresenceAt), which gates whether a vowel sounds rather than which. A builder opens
// its kind (`AngleKindScope`); the sound being built names its warp (`AngleWarpScope`, from its LikeWarp).
//
// THE AUDIO THREAD allocates nothing here: the scopes and the hook live on the stack and in a thread-local, and a warp is a
// fixed-size value the synth copies into storage of its own.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace feathers {

/// STRING (2026-10-31) is the Karplus-Strong engine's material, its five guest fields (readStringFields); its pluck's tone
/// reads the Partials sites, so a like on Pluck warps both.
/// DRUM (2026-11-05) is the drum engine's seven guest fields (readDrumFields), one set for all six kinds.
enum class AngleKind : uint8_t { Morph, Authors, Partials, Stair, Weave, Vox, String, Drum, kCount };
inline constexpr size_t kAngleKinds = static_cast<size_t>(AngleKind::kCount);
inline constexpr std::array<uint16_t, kAngleKinds> kAngleSitesOf { 35, 10, 340, 5, 22, 12, 5, 7 };
inline constexpr std::array<uint16_t, kAngleKinds> kAngleOffsetOf { 0, 35, 45, 385, 390, 412, 424, 429 };
inline constexpr size_t kAngleSites = 35 + 10 + 340 + 5 + 22 + 12 + 5 + 7;
inline constexpr int kPhaseWarpLut = 129;
/// THE PIN'S NEIGHBOURHOOD on a circle, in turns: the newest like's correction tapers over this.
inline constexpr double kPhaseWarpPinWidth = 0.02;
/// THE FIT'S HYPERPARAMETERS, named because the likes log records them (a sweep over logged sessions reads them back):
/// the uniform's weight k0 against strength·n likes (w0 = k0 / (k0 + s·n)), the most wrapped Cauchys BIC may choose, the
/// k-th nearest like that sets a component's width, the least width one like may give (in turns), and the EM's iterations.
/// The shipped values; `PhaseWarpParams` carries them into the fit so a sweep can vary each.
inline constexpr double kPhaseWarpPrior = 3.0;
inline constexpr int kPhaseWarpMaxComponents = 4;
inline constexpr int kPhaseWarpNeighbours = 2;
inline constexpr int kPhaseWarpIterations = 40;
inline constexpr double kPhaseWarpWidthFloor = 0.005;
struct PhaseWarpParams {
	double prior = kPhaseWarpPrior;
	int maxComponents = kPhaseWarpMaxComponents;
	int neighbours = kPhaseWarpNeighbours;
	double widthFloor = kPhaseWarpWidthFloor;
	int iterations = kPhaseWarpIterations;
	friend bool operator==(const PhaseWarpParams&, const PhaseWarpParams&) = default;
};

struct PhaseWarpSite {
	bool live = false;
	/// u at k / (kPhaseWarpLut - 1) -> the warped angle.
	std::array<float, kPhaseWarpLut> lut {};
	bool pinned = false;
	float pinAt = 0.0f;
	float pinTo = 0.0f;
	float pinShift = 0.0f;
};

/// **ONE SITE, FOR A DEVICE THAT KEEPS ITS OWN** (the Pattern's genes, mod/arp/pattern_warp.h): the circle fit every
/// site of `buildPhaseWarp` runs (`angles` are the likes' wrapped angles at this site, oldest first), the newest like's
/// pin, and the read `PhaseWarp::at` makes of one site. Message thread for the fit and the pin (they allocate); the read
/// allocates nothing.
void fitPhaseWarpSite(std::span<const double> angles, double strength, const PhaseWarpParams& params, PhaseWarpSite& out);
/// The site's LUT alone at `theta` (no pin).
[[nodiscard]] double phaseWarpLutAt(const PhaseWarpSite& site, double theta) noexcept;
/// Pins the site's angle `at` to `heard`, its correction tapered over kPhaseWarpPinWidth.
void pinPhaseWarpSite(PhaseWarpSite& site, float at, float heard) noexcept;
[[nodiscard]] float phaseWarpSiteAt(const PhaseWarpSite& site, float theta) noexcept;

/// ONE SOUND'S WARP: every site's circle map, by its flat index (kAngleOffsetOf[kind] + index).
struct PhaseWarp {
	uint16_t likes = 0;
	std::array<PhaseWarpSite, kAngleSites> sites {};
	[[nodiscard]] bool identity() const noexcept { return likes == 0; }
	/// The warped angle at a site. Exact at the newest like's angle (as the float it is), its correction tapered around it.
	[[nodiscard]] float at(size_t site, float theta) const noexcept;
	[[nodiscard]] uint64_t contentKey() const noexcept;
};

/// THE FIT, message thread (allocates): from each like's angles (kAngleSites floats, in the flat layout, oldest first),
/// grown one like at a time with the newest pinned to what it was heard as under the warp before it — derived, so a reload
/// walking the same likes arrives at the same warp. `strength` weighs each like as that many against the uniform's k0 = 3;
/// `liveSites` says which sites the engine reads (the others stay the identity).
PhaseWarp buildPhaseWarp(std::span<const std::array<float, kAngleSites>> likes, std::span<const bool> liveSites, double strength,
                         const PhaseWarpParams& params = {});

/// THE SOUND BEING BUILT names its warp (null: none) for every builder it calls; nests, restoring the outer one.
class AngleWarpScope {
public:
	explicit AngleWarpScope(const PhaseWarp* warp) noexcept;
	~AngleWarpScope();
	AngleWarpScope(const AngleWarpScope&) = delete;
	AngleWarpScope& operator=(const AngleWarpScope&) = delete;

private:
	const PhaseWarp* previous_;
};

/// A BUILDER READS ITS KIND'S SITES between construction and destruction: the warp (if a sound named one) or a recorder
/// (if one is listening) is installed on phi::angleHook, its index counting from 0.
class AngleKindScope {
public:
	explicit AngleKindScope(AngleKind kind) noexcept;
	~AngleKindScope();
	AngleKindScope(const AngleKindScope&) = delete;
	AngleKindScope& operator=(const AngleKindScope&) = delete;

private:
	void* previousHook_;
	int previousKind_;
	uint16_t previousIndex_;
};

/// MESSAGE THREAD: while one lives, every kind scope RECORDS its sites' angles into `out` (flat layout), unwarped.
class AngleRecorder {
public:
	explicit AngleRecorder(std::array<float, kAngleSites>& out) noexcept;
	~AngleRecorder();
	AngleRecorder(const AngleRecorder&) = delete;
	AngleRecorder& operator=(const AngleRecorder&) = delete;
	/// How many sites each kind recorded (a check that the layout's counts are the builders').
	[[nodiscard]] std::array<uint16_t, kAngleKinds> counts() const noexcept;

private:
	std::array<float, kAngleSites>* previous_;
};

} // namespace feathers

namespace feathers {

/// **ONE SOUND'S WARP AS IT TRAVELS** — from the stage that derives it, through the bridge, to the synth and the scope. A
/// plain pointer the holder keeps valid: on the message thread `owner` holds it; the bridge's sets and the synth's tables
/// copy the warp into storage of their own (`assignLikeWarpInto`) and leave `owner` empty, so the audio thread never
/// touches a reference count. Compared by the warp's content key, so an equal warp re-derived is not a change. Null is no
/// likes: the identity.
struct LikeWarp {
	const PhaseWarp* warp = nullptr;
	uint64_t key = 0;
	std::shared_ptr<const PhaseWarp> owner;
	[[nodiscard]] bool identity() const noexcept { return warp == nullptr; }
	friend bool operator==(const LikeWarp& a, const LikeWarp& b) noexcept {
		return (a.warp == nullptr) == (b.warp == nullptr) && a.key == b.key;
	}
};

/// A COPY FOR STORAGE THAT OWNS ITS WARP: the warp's content copied into `store`, `dst` pointed at it, `dst.owner`
/// untouched. Allocates nothing.
inline void assignLikeWarpInto(LikeWarp& dst, const LikeWarp& src, PhaseWarp& store) noexcept {
	dst.key = src.key;
	if (src.warp != nullptr) {
		if (src.warp != &store) {
			store = *src.warp;
		}
		dst.warp = &store;
	}
	else {
		dst.warp = nullptr;
	}
}

} // namespace feathers
