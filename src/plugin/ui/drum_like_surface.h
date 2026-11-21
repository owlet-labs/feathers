/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "fx/drum/drum_likes.h"
#include "fx/drum/drum_types.h"
#include "plugin/ui/like_surface.h"

#include <array>
#include <memory>
#include <vector>

// **THE DRUM'S TERRAIN AS A LIKE SURFACE.** The score and the warp (like_surface.h) do not know what they are scoring, so the Drum's
// terrain takes them as the synth's does: a place is Travel X and Y, the fingerprint at a place is the seven fields heard there
// (`dd::fieldsOf`, the scalers in), and the non-position controls that also move it are the seven scalers. A field the Kind does not
// read is zero in every fingerprint, so it neither scores nor warps.
//
// THE SCALE: the likes' arithmetic is in the character terrain's units, where a x1 view spans `kLikeRegionUnit`. The Drum pad's x1
// view spans two units of travel (half a drag is one), so a place in travel is `kDrumWarpPerTravel` of the warp's own.

namespace feathers::ui {

namespace dd = feathers::fx::drumdevice;

inline constexpr double kDrumWarpPerTravel = kLikeRegionUnit / 2.0;

/// The warp's coordinates of a place in travel, and back.
[[nodiscard]] inline WarpPoint drumWarpOf(double x, double y) noexcept { return { x * kDrumWarpPerTravel, y * kDrumWarpPerTravel }; }
[[nodiscard]] inline WarpPoint drumTravelOf(WarpPoint p) noexcept { return { p.x / kDrumWarpPerTravel, p.y / kDrumWarpPerTravel }; }

/// THE FINGERPRINT OF SEVEN FIELDS, for the Kind that reads some of them: the read ones in the first seven dimensions, the rest zero.
[[nodiscard]] LikeVector drumFingerprint(const std::array<float, 7>& fields, feathers::drum::Kind kind) noexcept;

/// **THE LIKE OF THE DRUM WHERE IT STANDS**: the plane (Zone, Offset, Gamma, Rotate), the place (Travel X and Y), the Kind, and the fields
/// as heard now, scalers in. Captured: a scaler turned later does not rewrite it.
[[nodiscard]] dd::DrumLike likeOf(const dd::Settings& s);
/// Whether a like belongs to the terrain being drawn: the same Kind on the same plane. Another's is kept for when it is selected again.
[[nodiscard]] bool likeApplies(const dd::DrumLike& like, const dd::Settings& s) noexcept;

/// THE SURFACE: `base`'s plane, Kind and scalers, with Travel X and Y the place.
class DrumLikeSurface final : public LikeSurface {
public:
	explicit DrumLikeSurface(const dd::Settings& base) : base_(base) {}
	void fingerprintsAt(std::span<const WarpPoint> points, std::vector<LikeVector>& out) const override;
	[[nodiscard]] size_t controlCount() const override;
	[[nodiscard]] const char* controlName(size_t control) const override;
	void nudgedFingerprintsAt(size_t control, std::span<const WarpPoint> points, std::vector<LikeVector>& out) const override;

private:
	void fill(const dd::Settings& s, std::span<const WarpPoint> points, std::vector<LikeVector>& out) const;
	dd::Settings base_;
};

/// **WHAT THE LIKES MAKE OF THE DRUM'S TERRAIN** at this plane, Kind and scalers: the model, the warp and the likes that apply. Derived
/// (never stored), shared and immutable.
struct DrumLikeState {
	LikeDerivation derivation;
	std::vector<dd::DrumLike> applied;
	/// What it was derived from: the settings that decide a fingerprint (Kind, plane, scalers) and the likes.
	std::array<float, 13> key {};
	double strength = 0.0;
	/// A place of the view (travel) and the world place drawn there, through the lenses and the cartogram. The identity with no likes.
	[[nodiscard]] std::array<double, 2> worldAt(double viewX, double viewY) const noexcept;
	[[nodiscard]] std::array<double, 2> viewAt(double worldX, double worldY) const noexcept;
	/// How like a liked drum the drum at a place is: 1 at a like's own fields, falling toward 0.
	[[nodiscard]] double scoreAt(const dd::Settings& s, double x, double y) const;
};

/// WHAT DECIDES A DERIVATION, from the settings and the likes: `deriveDrumLikes` is rebuilt only when this moves.
[[nodiscard]] std::array<float, 13> drumLikeKey(const dd::Settings& s) noexcept;

/// The state for these likes at `s`, or null when none applies. A pure, deterministic function. `strength` is the lens's magnification
/// (kWarpStrength is the synth's).
[[nodiscard]] std::shared_ptr<const DrumLikeState> deriveDrumLikes(const dd::Settings& s, const std::vector<dd::DrumLike>& likes,
                                                                  double strength = kWarpStrength);

/// **MUTATE, BY THE LIKES**: the dice's roll is drawn on the warped view (a step of `reach` travel from where the star is DRAWN, along the
/// two unit rolls in [-1, 1]) and carried back through the inverse warp, so the roll's density picks up the warp's area factor and the
/// likes steer it with no rule of their own. With no state both maps are the identity and it is the plain sum, to the bit.
[[nodiscard]] std::array<double, 2> drumMutate(const DrumLikeState* state, double x, double y, double sideways, double up, double reach) noexcept;

} // namespace feathers::ui
