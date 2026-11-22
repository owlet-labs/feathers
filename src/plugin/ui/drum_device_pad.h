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
#include "plugin/ui/tooltip.h"

#include "fx/drum/drum_types.h"
#include "plugin/ui/drum_like_surface.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

// THE DRUM DEVICE'S TERRAIN PAD — the explore pad over Travel X and Travel Y, which are UNBOUNDED (the view follows the star, no edge).
// The terrain is how far the drum's seven fields at each place are from the fields where it stands, over the fields its Kind reads.

namespace feathers::ui {

/// WHAT THE DRUM'S TERRAIN IS A PICTURE OF: the device's whole row (its Zone, Offset, Gamma, Rotate and Kind decide the fields) and what the likes
/// made of it, the warp the picture is drawn through. Shared and immutable, so the key compares the pointer.
struct DrumPadContext {
	fx::drumdevice::Settings settings {};
	std::shared_ptr<const DrumLikeState> likes {};
	friend bool operator==(const DrumPadContext&, const DrumPadContext&) = default;
};

struct DrumDevicePadPolicy {
	static constexpr size_t kPhaseCount = 2;
	static constexpr size_t kTravelFirst = 0; ///< row field 0 is Travel X, 1 Travel Y
	using Phases = std::array<double, kPhaseCount>;
	using RowReader = std::function<float(size_t field)>;
	using Context = DrumPadContext;
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
	/// **THE LIKES' WARP**: the view's plane to the world's and back (plane units from the frame's origin), the identity with no likes.
	[[nodiscard]] static std::pair<double, double> worldFromView(const Context& c, const Phases& origin, double x, double y);
	[[nodiscard]] static std::pair<double, double> viewFromWorld(const Context& c, const Phases& origin, double x, double y);
	[[nodiscard]] static Key keyOf(const RowReader& row, double rotate, double zoom, const Phases& standing, const Context& context);
	static constexpr size_t kGrid = 17;
	static constexpr size_t kWarpedGrid = 49; ///< the grid of a view drawn through the likes' warp
	static constexpr std::chrono::milliseconds kKeyRebuildInterval { 100 };
	static constexpr bool kZoomControl = true;
};

extern template class ExplorePad<DrumDevicePadPolicy>;

class DrumDevicePad final : public ExplorePad<DrumDevicePadPolicy>, public TooltipSource {
public:
	using ExplorePad::ExplorePad;
	/// THE CONTROLS A DRAG WRITES, declared through ControlFace (a gesture on the plane): Travel X and Travel Y. Set once by the owner.
	void setTravelControls(device::Modulatable* x, device::Modulatable* y) {
		travelX_ = x;
		travelY_ = y;
	}
	/// THE LIKES OF THIS PLANE AND KIND, as the owner reads them (the marks, and the heart's answer).
	std::function<const std::vector<fx::drumdevice::DrumLike>&()> applied;
	void draw(visage::Canvas& canvas) override;
	/// Which applied like the star stands on, if any: one whose mark is within `kLikeTouchPixels` of the puck.
	[[nodiscard]] std::optional<size_t> likeUnderStar() const;
	/// TEST ONLY: how many like marks the last frame drew.
	[[nodiscard]] int likeMarksDrawn() const noexcept { return likeMarksDrawn_; }
	/// Where an applied like's mark is, in pixels.
	[[nodiscard]] visage::Point likePixel(const fx::drumdevice::DrumLike& like) const;
	static constexpr float kLikeTouchPixels = 6.0f;
	/// THE BOX DRAWN OVER THE PAD (Rotate's, pad-local; empty by default), which no like mark may hide under.
	void setInset(visage::Bounds inset) { inset_ = inset; }
	[[nodiscard]] visage::Bounds insetForTest() const noexcept { return inset_; }
	/// The rectangle a mark centred at `at` covers, and where the mark for a like at pixel `place` is drawn (moved out from under the inset).
	[[nodiscard]] visage::Bounds markRect(visage::Point at) const;
	[[nodiscard]] visage::Point markAt(visage::Point place) const;
	const char* tooltipAt(visage::Point) const override {
		return "The terrain the drum stands on: press or drag to place it. It has no edge: the view follows, and Travel X and Y keep going.";
	}
	void offeredControls(std::vector<Offered>& out) const override {
		if (travelX_ != nullptr) {
			out.push_back({ travelX_, Road::Gesture });
		}
		if (travelY_ != nullptr) {
			out.push_back({ travelY_, Road::Gesture });
		}
	}

private:
	int likeMarksDrawn_ = 0;
	visage::Bounds inset_ {};
	device::Modulatable* travelX_ = nullptr;
	device::Modulatable* travelY_ = nullptr;
};

} // namespace feathers::ui
