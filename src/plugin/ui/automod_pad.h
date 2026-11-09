/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */
#pragma once

#include "device/modulatable.h"

#include <functional>
#include "fx/automod/automod_phase_space.h"
#include "fx/automod/automod_types.h"
#include "plugin/ui/field_activity_map.h"
#include "plugin/ui/automod_terrain.h"

#include "visage_ui/frame.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <utility>

namespace feathers::ui {

/// **AUTOMOD'S PLANE: A SQUARE CHART WITH TERRAIN, AND A JOYSTICK PUCK.**
///
/// DECISION 02, in order and each correcting the last:
///   - A repeat of the shaper-style square with terrain, not a new coordinate circle. So: a square,
///     and the terrain decoded from the field.
///   - The plane's axes are **Type across, Flavour up** — the two coordinates as peers.
///   - The origin of the plane, the current position, does not move; what moves is the direction from
///     it to every point on the xy map. The chart is anchored AT where you are.
///   - The origin **re-anchors on release**: the puck returns to centre. A joystick, not a marker.
///
/// **SO THE PUCK IS A DISPLACEMENT, NOT A POSITION**, and that is not a UI preference — it falls out
/// of the origin being the current position. There was never a position-versus-delta choice to make.
///
/// **WHAT THIS REPLACED AND WHY IT WAS WRONG.** The first version was a POLAR pad: one puck decoding
/// through radius and angle into three phase offsets, with a `Rotate` control invented to re-aim the
/// basis it projected onto. The design called for the shaper's square. The circle also made `Rotate`
/// mean something it cannot mean — automod had no travel coordinate, so a rotation could only turn the
/// position itself, and a rotation that changes the sound is redundant with gamma.
///
/// **THE SHAPER'S PAD IS THE REFERENCE, AND THIS ONE IS ITS SHAPE.** Automod's xy surface was brought
/// closer to the shaper's, which also fixed its bugs (the terrain not updating as the puck moved, among
/// others). The two pads were written from one draft and the shaper's was then corrected four
/// times on the installed build — the puck read off the travel, the terrain of the coordinate square
/// instead of the plane, the rebuild inside the gesture, the press anchored at the click — while this
/// one kept the draft. Every mechanism below now matches `ShaperPad`'s, and where the two differ the
/// difference is named at the site.
///
/// **AND THE GESTURE IS EXPLORE'S NOW, AS THE SHAPER'S IS (backlog IJ, 2026-09-23).** The shaper's
/// treatment, applied to the automod pad. So the joystick above — a delta per event, re-anchored, the plane re-derived from wherever the field had
/// got to, a picture frozen until release — is replaced by the shaper's pan, mechanism for mechanism.
/// What stays true of the decision: the chart is anchored AT where you are, at REST. What changed is only
/// what happens under a held hand. The one deliberate difference from `ShaperPad` is the reference
/// grid, chosen for this pad, which now slides with the ground instead of standing on the glass.
class AutomodPad final : public visage::Frame {
public:
	/// **THE PAD WRITES TRAVEL THROUGH THE PLANE, NOT TWO COORDINATES.**
	///
	/// It used to write `Type` and `Flavour` directly, which was right while the axes WERE those two
	/// and stopped being right when the plane started coming from the rotate generator: X is not Type
	/// any more, and a drag is a displacement in a 39-space that no pair of knobs can express.
	///
	/// @param blob     writes one float of the device's folded travel, by its offset in the blob
	/// @param readRow  reads the device's current row — home and the standing folded travel, which the
	///                 basis and the fold both need
	/// @param rotate   the generator the plane is derived from
	using BlobWriter = std::function<void(size_t index, float value)>;
	using RowReader = std::function<float(size_t field)>;
	AutomodPad(BlobWriter blob, RowReader readRow, device::Modulatable& rotate);

	/// How many floats of folded travel a commit writes through `blob` — what an owner brackets as one undo step.
	static constexpr size_t kTravelCount = fx::automod::kFoldedTravelFloats;
	/// **THE HAND'S HOLD, FOR WHOEVER RECORDS IT** (backlog JB) — `ExplorePad`'s pair: `onHoldBegan` before the
	/// press writes anything, `onHoldEnded` on the release. Unset, nothing is told.
	std::function<void()> onHoldBegan;
	std::function<void()> onHoldEnded;

	void draw(visage::Canvas& canvas) override;

	/// **ZOOM ON THE WHEEL, WITH EXPLORE'S OWN LADDER** — the shaper's, constant for constant. The
	/// same gesture buys less ground the further in you are, so a hand can place the field precisely
	/// instead of only coarsely.
	bool mouseWheel(const visage::MouseEvent& e) override;
	static constexpr double kZoomStep = 1.1246826503806981; ///< Explore's: a quarter of the mock's 1.6 step
	static constexpr double kMinZoom = 0.25;
	static constexpr double kMaxZoom = 64.0;
	[[nodiscard]] double zoom() const noexcept { return zoom_; }
	void setZoom(double zoom);

	/// WHERE THE PUCK IS DRAWN, in this frame's pixels: the star, where the view shows it. At rest the
	/// frame is anchored on the sound, so it is the CENTRE.
	[[nodiscard]] visage::Point puck() const;

	/// **THE PAN — the seams, the shaper's own.** `starForTest` is where the sound is and `viewForTest`
	/// where the view is centred, both in PLANE units (the units `phasesAt` takes) from the frame's
	/// origin; at rest both are 0, because the frame re-origins on the sound once the view is home.
	[[nodiscard]] std::pair<double, double> starForTest() const noexcept { return { starX_, starY_ }; }
	[[nodiscard]] std::pair<double, double> viewForTest() const noexcept { return { viewX_, viewY_ }; }
	[[nodiscard]] bool glidingForTest() const noexcept { return gliding_; }
	/// LANDS A GLIDE AT ONCE — what a frame loop does over ~0.1 s, for a case with no frame loop.
	void finishGlideForTest() { finishGlide(); }
	/// ONE FRAME'S EDGE SCROLL, `seconds` long — what `draw` does every frame while a hand is on the pad.
	bool tickForTest(double seconds) { return edgeScroll(seconds); }
	/// Plane units per pixel, at the current zoom.
	[[nodiscard]] double planePerPixelForTest() const { return planePerPixel(); }
	/// **WHERE THE EDGE SCROLL STARTS, in pixels inside the pad's real edge** — and it runs on past the
	/// edge. The shaper's numbers, from its audition: it works in Explore because the hit box
	/// extends to the edges.
	static constexpr double kEdgeBandPixels = 24.0;
	static constexpr double kRampPixels = 48.0;       ///< how much deeper full speed is reached
	static constexpr double kPanViewsPerSecond = 1.2; ///< Explore's `kViewsPerSecond`
	static constexpr double kStarInsetPixels = 8.0;   ///< the star, held inside a pad the hand has left
	/// THE MAP IS BUILT THIS MUCH WIDER THAN THE VIEW — Explore's `kTerrainMargin` — so a pan reveals
	/// ground already drawn.
	static constexpr double kMapMargin = 1.35;

	static constexpr size_t kMapWide = 40;
	static constexpr size_t kMapHigh = 40;

	/// HOW MANY NAMED ZONES A HALF-DRAG CARRIES AT x1. The same "a gesture is worth a named region"
	/// scale `ZoneCoordBox` uses for its fine half, so a hand that has learned the boxes finds the pad
	/// moves at a rate it recognises. **In ZONES, not in coordinate units** — the coordinate spans
	/// 0..1 across eight of them, and the two are not interchangeable; reading this as coordinate
	/// units saturated Type on the first gesture. Divided by the zoom, as the shaper's is. A half-drag
	/// is half the pad's smaller side — which, the pad being square, is half of either.
	static constexpr float kZonesPerHalfDrag = 1.0f;

	/// **WHAT THE MAP WAS BUILT FOR — THE CACHE KEY, NAMING EVERY INPUT THE TERRAIN READS.** The
	/// shaper's rule, learned there the hard way: a key that omits an input lets that input change
	/// the terrain's INPUT without ever asking for a rebuild. The terrain reads the three coordinates
	/// with their phases, the gamma, the rotate, the zoom and the standing travel the frame is anchored
	/// on, and nothing else; the key is exactly that list, and sits beside the function that builds it.
	struct View {
		double type = -1e9;
		double flavour = -1e9;
		double mod = -1e9;
		double gamma = -1e9;
		double rotate = -1e9;
		double zoom = -1e9;
		fx::automod::phase_space::Phases standing {};
		bool operator==(const View&) const = default;
	};
	[[nodiscard]] const View& builtForTest() const noexcept { return built_; }

	[[nodiscard]] const std::array<float, kMapWide * kMapHigh>& mapForTest() const noexcept {
		return map_.cells();
	}
	/// Where the map was centred when built, in plane units — a cell's point is this plus its offset.
	[[nodiscard]] std::pair<double, double> mapCentreForTest() const noexcept { return { mapX_, mapY_ }; }

	/// **HOW MANY TIMES THE TERRAIN HAS BEEN BUILT.** A COUNT, not a comparison of pictures: the claim
	/// under test is "no rebuild happened", and a rebuild that lands on the same cells is invisible to
	/// a difference check while costing exactly as much. Produced BY `buildMap`, so it cannot report
	/// a build that did not run.
	[[nodiscard]] size_t rebuildsForTest() const noexcept { return rebuilds_; }

private:
	static constexpr double kGlideSeconds = 0.08; ///< the release glide's time constant

	/// Plane units per pixel — one scale for both axes, from the smaller side, divided by the zoom.
	[[nodiscard]] double planePerPixel() const;
	[[nodiscard]] std::pair<double, double> viewHalf() const;
	[[nodiscard]] visage::Point toPixel(double x, double y) const;

	/// RE-ANCHOR THE FRAME ON THE SOUND: read the home and the standing travel, derive the basis there,
	/// put the view, the star and the map at the origin. At rest, on a zoom or knob change, on landing.
	void reOrigin();
	/// BUILD THE MAP AROUND THE VIEW, IN THE CURRENT FRAME — mid-gesture, when the view leaves the margin.
	void buildMap();
	void placeStar();
	[[nodiscard]] std::pair<double, double> edgeVelocity() const;
	bool edgeScroll(double seconds);
	void keepMapCovering();
	/// THE STAR'S TRAVEL, written: the frame's standing travel plus the basis carried to the star.
	void commit();
	void finishGlide();
	[[nodiscard]] bool viewMoved() const;
	[[nodiscard]] View viewNow() const;

	View built_ {};
	size_t rebuilds_ = 0;

	BlobWriter blob_;
	RowReader readRow_;
	device::Modulatable& rotate_;

	/// **THE FRAME — fixed from one rest to the next**, so the map, the press and the drag all read the
	/// plane the same way. `origin_` is the standing travel in DOUBLE (by design: the accumulator is
	/// double and the stored form float, and `wrapForStorage` keeps the float from carrying a large
	/// integer part — see `automod_derived.h`); `home_` is where the plane is taken, `basis_` its
	/// orientation.
	std::array<double, fx::automod::kFoldedTravelFloats> origin_ {};
	fx::automod::phase_space::Phases home_ {};
	fx::automod::phase_space::Basis basis_ {};
	double viewX_ = 0.0;
	double viewY_ = 0.0;
	double starX_ = 0.0;
	double starY_ = 0.0;
	double mapX_ = 0.0;
	double mapY_ = 0.0;
	bool held_ = false;
	bool dragged_ = false;
	bool gliding_ = false;
	std::chrono::steady_clock::time_point glideSteppedAt_ {};
	std::chrono::steady_clock::time_point edgeSteppedAt_ {};
	visage::Point pointer_ {};
	double zoom_ = 1.0;
	FieldActivityMap<kMapWide, kMapHigh> map_;
};

} // namespace feathers::ui
