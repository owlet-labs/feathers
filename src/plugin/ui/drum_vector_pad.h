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

#include "dsp/character_bank.h"
#include "dsp/drum_voice.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <functional>

// THE DRUM LAYER'S TERRAIN PAD — the explore pad over a layer's two-number vector (kit stance redesign).
//
// A LAYER IS PLACED AT (THE KIT'S STANCE) + (ITS OWN VECTOR), and the vector is the two stored numbers OffsetX and OffsetY, so
// the phase space is two long: the row is (X, Y), the basis is the identity, and the travel a press commits IS the vector.
// What the terrain draws is how far the layer's sound at each place is from the sound where it stands: the root of the mean
// squared difference of the fields its kind reads, so dark is "the sound as it is" and bright is "a different drum".

namespace feathers::ui {

/// WHAT THE TERRAIN IS A PICTURE OF, which two floats of row cannot carry: the character the pad is built from, the pad it
/// is for and which layer. Refreshed by the pad editor between frames.
struct DrumVectorContext {
	Character character {};
	uint8_t slot = 0;
	uint8_t layer = 0;
	drum::Kind kind = drum::Kind::Membrane;
	friend bool operator==(const DrumVectorContext&, const DrumVectorContext&) = default;
};

struct DrumVectorPolicy {
	static constexpr size_t kPhaseCount = 2;
	static constexpr size_t kTravelFirst = 0; ///< row field 0 is OffsetX, 1 is OffsetY
	using Phases = std::array<double, kPhaseCount>;
	using RowReader = std::function<float(size_t field)>;
	using Context = DrumVectorContext;
	struct Key {
		double rotate = -1e9;
		double zoom = -1e9;
		Phases standing {};
		Context context {};
		bool operator==(const Key&) const = default;
	};
	/// THE IDENTITY: the plane's axes are the vector's.
	[[nodiscard]] static SliceBasis<kPhaseCount> basisAt(const RowReader&, double, const Phases&);
	/// THE VECTOR AS STORED: held to the parameter's own reach (kDrumVectorRange either side).
	static void wrapForStorage(const Phases& accumulator, std::array<float, kPhaseCount>& stored);
	/// THE STAR (AND THE VIEW) STAY WITHIN THE VECTOR'S REACH: the basis is the identity, so a plane position is the vector less where the frame stands.
	static void clampStar(const Phases& origin, const Phases&, const Phases&, double& x, double& y) {
		x = std::clamp(x, -kDrumVectorRange - origin[0], kDrumVectorRange - origin[0]);
		y = std::clamp(y, -kDrumVectorRange - origin[1], kDrumVectorRange - origin[1]);
	}
	static void paint(ExploreMap& map, const Key& key, const ExplorePlane<kPhaseCount>& plane);
	[[nodiscard]] static Key keyOf(const RowReader& row, double rotate, double zoom, const Phases& standing, const Context& context);
	/// The coarse grid's side; odd, so the centre cell is the sound where it stands.
	static constexpr size_t kGrid = 17;
	/// A build is cheap (a few hundred reads of seven triangles) but its key follows every parameter of the kit.
	static constexpr std::chrono::milliseconds kKeyRebuildInterval { 100 };
	static constexpr bool kZoomControl = true;
};

extern template class ExplorePad<DrumVectorPolicy>;

class DrumVectorPad final : public ExplorePad<DrumVectorPolicy> {
public:
	using ExplorePad::ExplorePad;
};

} // namespace feathers::ui
