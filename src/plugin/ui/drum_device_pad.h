/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "plugin/ui/explore_pad.h"

#include "fx/drum/drum_types.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <functional>

// THE DRUM DEVICE'S TERRAIN PAD — the explore pad over Travel X and Travel Y, which are UNBOUNDED (the view follows the star, no edge).
// The terrain is how far the drum's seven fields at each place are from the fields where it stands, over the fields its Kind reads.

namespace feathers::ui {

struct DrumDevicePadPolicy {
	static constexpr size_t kPhaseCount = 2;
	static constexpr size_t kTravelFirst = 0; ///< row field 0 is Travel X, 1 Travel Y
	using Phases = std::array<double, kPhaseCount>;
	using RowReader = std::function<float(size_t field)>;
	/// What the terrain is a picture of: the device's whole row (its Zone, Offset, Gamma, Rotate and Kind decide the fields).
	using Context = fx::drumdevice::Settings;
	struct Key {
		double rotate = -1e9;
		double zoom = -1e9;
		Phases standing {};
		Context context {};
		bool operator==(const Key&) const = default;
	};
	[[nodiscard]] static SliceBasis<kPhaseCount> basisAt(const RowReader&, double, const Phases&);
	static void wrapForStorage(const Phases& accumulator, std::array<float, kPhaseCount>& stored);
	static void paint(ExploreMap& map, const Key& key, const ExplorePlane<kPhaseCount>& plane);
	[[nodiscard]] static Key keyOf(const RowReader& row, double rotate, double zoom, const Phases& standing, const Context& context);
	static constexpr size_t kGrid = 17;
	static constexpr std::chrono::milliseconds kKeyRebuildInterval { 100 };
	static constexpr bool kZoomControl = true;
};

extern template class ExplorePad<DrumDevicePadPolicy>;

class DrumDevicePad final : public ExplorePad<DrumDevicePadPolicy> {
public:
	using ExplorePad::ExplorePad;
};

} // namespace feathers::ui
