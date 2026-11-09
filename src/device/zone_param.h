/*
 * Copyright © 2024-2025 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <algorithm>
#include <cstdint>

// Zone-based parameter computation, ported from the firmware's zone_param.hpp.
//
// THE PORT: the firmware represents knob values as q31_t (0 to ONE_Q31). Feathers uses float (0.0 to
// 1.0). The zone math is identical — divide the 0..1 range into NUM_ZONES equal zones, report which
// zone a value falls in and where within that zone it sits — just without the q31 scaling.
//
// OWLTT uses this for its CHARACTER and VIBE knobs: each is divided into 8 zones, and the zone index
// and position within the zone determine the compressor's tonal character. The header lives in
// src/device/ (not src/fx/owltt/) so it can be reused by other devices — a zone knob usable by more
// than OWLTT cannot include fx/owltt.

namespace feathers {

/// Result of zone computation: which zone a value falls in, and where within that zone.
struct ZoneInfo {
	int32_t index;    ///< Zone index (0 to numZones-1)
	float position;   ///< Position within zone (0.0 to 1.0)
	float zoneStart;  ///< Start of current zone in 0..1
	float zoneWidth;  ///< Width of one zone in 0..1
};

/// Compute zone width for a given number of zones (always 1/numZones in float).
[[nodiscard]] constexpr float computeZoneWidth(int32_t numZones) {
	return 1.0f / static_cast<float>(numZones);
}

/// Compute zone info from a float value in 0..1.
[[nodiscard]] inline ZoneInfo computeZone(float value, int32_t numZones) {
	const float zoneWidth = computeZoneWidth(numZones);
	const float clamped = std::clamp(value, 0.0f, 1.0f);

	// Zone index: which of the numZones bands does this value fall in.
	int32_t index = std::min(static_cast<int32_t>(clamped / zoneWidth), numZones - 1);
	const float zoneStart = static_cast<float>(index) * zoneWidth;

	// Position within the zone: 0..1, clamped to handle the edge case at value == 1.0.
	const float offset = std::min(clamped - zoneStart, zoneWidth - 1e-6f);
	const float position = offset / zoneWidth;

	return {index, position, zoneStart, zoneWidth};
}

/// Check if value is in or past a specific zone.
[[nodiscard]] constexpr bool isInZoneOrLater(float value, int32_t zoneIndex, int32_t numZones) {
	return value >= (computeZoneWidth(numZones) * static_cast<float>(zoneIndex));
}

/// Get the start position of a zone in 0..1.
[[nodiscard]] constexpr float getZoneStart(int32_t zoneIndex, int32_t numZones) {
	return computeZoneWidth(numZones) * static_cast<float>(zoneIndex);
}

/// Convert zone position (0.0-1.0) to display value (0-127).
[[nodiscard]] constexpr int32_t zonePositionToDisplay(float position) {
	return static_cast<int32_t>(position * 127.0f);
}

/// Zone-based parameter: a float value in 0..1 with zone semantics. Computes zone index and position
/// for a value, used by OWLTT's CHARACTER and VIBE knobs.
template <int32_t NUM_ZONES = 8>
struct ZoneBasedParam {
	float value = 0.0f;

	static constexpr int32_t kNumZones = NUM_ZONES;
	static constexpr float kZoneWidth = computeZoneWidth(NUM_ZONES);

	[[nodiscard]] ZoneInfo getZoneInfo() const { return computeZone(value, NUM_ZONES); }
	[[nodiscard]] int32_t getZoneIndex() const { return getZoneInfo().index; }
	[[nodiscard]] float getPosInZone() const { return getZoneInfo().position; }
	[[nodiscard]] float getGlobalPos() const { return value; }
	/// QUALIFIED, BECAUSE THE MEMBER SHADOWS THE FREE FUNCTION IT MEANS TO CALL. Unqualified, the
	/// name resolves to THIS one-argument member and the three-argument call fails to compile.
	///
	/// A METHOD THAT COULD NEVER COMPILE, SHIPPED, because a template member is only instantiated
	/// when something calls it and nothing ever did. The move of this header into `src/device/` is
	/// what surfaced it -- not because moving broke it, but because the test that came with the move
	/// was the first code to instantiate the template. Dead code in a template is not merely unused;
	/// it is UNCHECKED.
	[[nodiscard]] bool isInZoneOrLater(int32_t zoneIndex) const {
		return feathers::isInZoneOrLater(value, zoneIndex, NUM_ZONES);
	}

	[[nodiscard]] operator float() const { return value; }
	ZoneBasedParam& operator=(float v) {
		value = v;
		return *this;
	}
};

} // namespace feathers
