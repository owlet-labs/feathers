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

#include "device/state_blocks.h"
#include "dsp/drum_voice.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// **THE DRUM DEVICE'S LIKES** ("more like this" on its terrain). A like is the PLACE the drum stood and the SEVEN FIELDS HEARD
// THERE, captured at the moment of the like and never re-derived (CLAUDE.md: captured state persists, so a scaler turned later cannot
// rewrite what was liked). Everything the likes are used for (the score, the warp, the suggestion) is derived from these and the
// device's own settings, and is never stored. The likes belong to the DEVICE, in a hand's block (`drum.likes.v1`), so they travel
// with it in a patch and in a Drum preset.

namespace feathers::fx::drumdevice {

/// ONE LIKE: the plane it was made on (Zone, Offset, Gamma, Rotate: what `fieldsOf` reads of the home), where on it (Travel X and Y),
/// the Kind it was made on (a drum switched to another Kind is not warped by the old Kind's likes), and what was heard.
struct DrumLike {
	double x = 0.0;
	double y = 0.0;
	uint16_t zone = 0;
	float offset = 0.0f;
	float gamma = 0.0f;
	float rotate = 0.0f;
	uint16_t kind = 0;
	std::array<float, 7> fields {}; ///< pitch, length, sweep, tone, noise, snap, metal, as `drum::Fields`
	friend bool operator==(const DrumLike&, const DrumLike&) = default;
};

inline constexpr size_t kMaxDrumLikes = 24;

/// THE BLOCK `drum.likes.v1`: a count, then per like x and y (a double each: four words), zone (one), offset, gamma and rotate (a float
/// each: two words), kind (one) and the seven fields (two each), all as 16-bit words (`device::BlockWriter`).
inline constexpr uint32_t kLikeWords = 4 + 4 + 1 + 2 + 2 + 2 + 1 + 14;
inline constexpr uint32_t kLikesBlockWords = 1 + static_cast<uint32_t>(kMaxDrumLikes) * kLikeWords;
inline constexpr size_t kLikesBlock = 0;

[[nodiscard]] std::array<float, 7> fieldsArray(const feathers::drum::Fields& f) noexcept;
[[nodiscard]] feathers::drum::Fields fieldsOfArray(const std::array<float, 7>& a) noexcept;

/// Writes `likes` (at most kMaxDrumLikes) into `out` (kLikesBlockWords long, zero padded).
void writeLikeBlock(const std::vector<DrumLike>& likes, float* out) noexcept;
/// Reads a block, or refuses it (a wrong length, a word that is not whole, a count past the cap, a kind or a field out of range, a
/// coordinate that is not finite): false, and `out` is untouched.
[[nodiscard]] bool readLikeBlock(const float* in, size_t n, std::vector<DrumLike>* out);

} // namespace feathers::fx::drumdevice
