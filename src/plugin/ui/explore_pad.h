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

#include "dsp/slice_basis.h"
#include "plugin/ui/edge_pan.h"
#include "plugin/ui/field_activity_map.h"
#include "plugin/ui/fx_control.h"

#include <visage/ui.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

// THE EXPLORE PAD — the shaper's pad with the shaper taken out of it (phaser plan, Task 9a).
//
// WHAT IS SHARED. The puck, unbounded travel, the drag decoded onto a plane through an N-dim phi phase space,
// Rotate re-origining the frame, zoom on the wheel, Explore's pan (edge scroll on time, the release glide), and a
// terrain cached behind it all. None of that names a device: it reads the row by index and writes the travel by
// index, and it always did (`BlobWriter`, `RowReader`).
//
// WHAT VARIES, AND IS THEREFORE THE POLICY. Four things were hard-coded to the shaper, and each is now a member of
// `Policy`, a struct of statics:
//
//   - `kPhaseCount` and `kTravelFirst` — how many phases the space has (shaper 18, phaser 30) and where in the row
//     the standing travel starts;
//   - `basisAt(row, rotate, origin)` — the HOME FROM THE ROW, and the plane taken there. The shaper's home moves with
//     Colour and Gamma; the phaser's is a constant. Returns `SliceBasis<kPhaseCount>`;
//   - `wrapForStorage(accumulator, stored)` — the double accumulator reduced to the floats a patch stores;
//   - `Key` and `keyOf(row, rotate, zoom, standing)` — WHAT THE MAP IS BUILT FROM. The class of bug the shaper's own
//     `View` comment names — *a cache key must name every input its value reads* — is why the key lives with the
//     painter that reads those inputs, not here;
//   - `paint(map, key, plane)` — THE TERRAIN: fills the backdrop map for a region of the plane, reading its inputs
//     ONLY FROM THE KEY. `Plane` hands it the frame (`travelAt(ux, uy)`, the travel a press at that unit coordinate
//     would commit), so a painter may sample every cell or a coarse grid of its own;
//   - OPTIONALLY `kKeyRebuildInterval` (a `std::chrono::milliseconds`) — THE THROTTLE ON A MOVING KEY. A painter whose
//     build costs a real fraction of a frame (the phaser's: ~25 ms) names one, and while the key keeps changing
//     (a knob drag, DAW automation) the pad rebuilds at most once per interval and once more after the key settles.
//     A policy that names none rebuilds on every frame that finds the key moved, which is what the shaper has always
//     done — so the shaper's behaviour, at rest and in motion, is exactly what it was.
//
// **THE KEY IS TAKEN FIRST AND THE PAINTER READS NOTHING ELSE** (the Task 9a review). The first draft painted from the
// row and then took the key from the row a second time, so an automation move landing during a ~10 ms build left a
// map of the OLD values under a key naming the NEW ones — stale, and recorded as fresh, so nothing would rebuild it.
// One read, two uses: the map and the key cannot disagree. And the pad's own SIZE is part of what the map was built
// for (`builtW_`/`builtH_`): the first build once ran at construction, at width 0, and without it the map stayed a 2x2
// view's stretched across the real pad until something else changed. No build runs at 2 px or less now (`buildMap`),
// so the first is the first `draw` at a real size.
//
// A TEMPLATE, NOT A RUNTIME POLICY OBJECT, and the reason is the shaper's bit identity: the shaper's arithmetic is
// the template's arithmetic, expression for expression, with the policy calls resolved at compile time — no
// `std::function` per cell, no allocation in a build, no indirection anybody has to prove inert. The member
// definitions live in `explore_pad_impl.h`, which only the TUs that explicitly instantiate a pad include
// (`shaper_pad.cpp`, `phaser_pad.cpp`), so the template costs a reader of this header nothing.
//
// **AUTOMOD'S PAD IS A SIBLING THAT WAS NOT FOLDED IN**, on purpose: it carries a separate home member, a basis
// applied through `SliceBasis::travelTerm`, zone bands and a map-centre seam, over a travel array sized by
// `kFoldedTravelFloats` rather than its phase count. Folding it in would be a behaviour change dressed as a
// refactor, and it was not this task's to make. It is the obvious third policy when somebody owns that change.
//
// LIFETIME, UNCHANGED FROM THE SHAPER'S. The pad owns its writer and reader by value and holds Rotate by reference,
// exactly as `ShaperPad` did; the policy holds nothing, so it cannot extend or shorten what any callback captures.

namespace feathers::ui {

/// THE TERRAIN'S CELLS — one size for every explore pad. Outside the template so a policy, which the template
/// is still incomplete without, can name it in its own declarations.
using ExploreMap = FieldActivityMap<48, 32>;

/// **THE REGION OF THE PLANE A TERRAIN PAINTER FILLS** — the frame the press reads, carried to the map's extent.
/// `travelAt(ux, uy)` is the travel a press at unit coordinate (ux, uy) of the MAP would commit: x rightward,
/// y upward, `FieldActivityMap::build`'s convention. The arithmetic is the shaper's `buildMap`, unchanged.
template <size_t N>
struct ExplorePlane {
	const std::array<double, N>& origin;
	const std::array<double, N>& basisX;
	const std::array<double, N>& basisY;
	double centreX; ///< where the map is centred, in plane units from the frame's origin
	double centreY;
	double halfX; ///< the map's half-extents, in plane units
	double halfY;
	[[nodiscard]] std::array<float, N> travelAt(float ux, float uy) const {
		const double px = centreX + (static_cast<double>(ux) * 2.0 - 1.0) * halfX;
		const double py = centreY + (static_cast<double>(uy) * 2.0 - 1.0) * halfY;
		std::array<float, N> travel {};
		for (size_t i = 0; i < N; ++i) {
			travel[i] = static_cast<float>(origin[i] + basisX[i] * px + basisY[i] * py);
		}
		return travel;
	}
};

template <class Policy>
class ExplorePad : public visage::Frame {
public:
	static constexpr size_t kPhaseCount = Policy::kPhaseCount;
	using Phases = std::array<double, kPhaseCount>;
	using View = typename Policy::Key;

	/// @param blob   writes one float of the device's folded travel, by its index in the blob
	/// @param row    reads a field of the device's row, by index — the blob and the coordinates
	/// @param rotate the plane's turn. It moves the BASIS, never the sound.
	using BlobWriter = std::function<void(size_t index, float value)>;
	using RowReader = std::function<float(size_t field)>;
	static_assert(std::is_same_v<typename Policy::RowReader, RowReader>,
	              "the policy's row reader is the pad's: `basisAt` and `keyOf` are handed the pad's own");
	ExplorePad(BlobWriter blob, RowReader readRow, device::Modulatable& rotate);

	/// **THE HAND'S HOLD, FOR WHOEVER RECORDS IT** (backlog JB). The pad writes its travel through `blob` on
	/// the press, every drag and every edge-scroll frame; the owner brackets them into ONE undo step —
	/// `onHoldBegan` before the press writes anything, `onHoldEnded` on the release. Unset, nothing is told.
	std::function<void()> onHoldBegan;
	std::function<void()> onHoldEnded;

	void draw(visage::Canvas& canvas) override;

	/// **ZOOM ON THE WHEEL, WITH EXPLORE'S OWN LADDER.** Same step, same ceiling, and the same reason
	/// the two axes share a scale: the basis's phase coefficients are of order one, so a unit of
	/// travel moves each phase about as far as a unit of home does, and a chart whose axes were scaled
	/// differently would draw a distortion that is not in the sound.
	bool mouseWheel(const visage::MouseEvent& e) override;

	/// One notch of scroll. Explore's `kZoomStep`, not a second opinion about it.
	static constexpr double kZoomStep = 1.1246826503806981; ///< Explore's: a quarter of the mock's 1.6 step
	static constexpr double kMinZoom = 0.25;
	static constexpr double kMaxZoom = 64.0;

	[[nodiscard]] double zoom() const noexcept { return zoom_; }
	void setZoom(double zoom);

	/// **A VISIBLE ZOOM: "− 100% +" IN THE PAD'S TOP-LEFT CORNER, ON THE ROTATE BOX'S ROW** (phaser round 2: the terrain
	/// gets a zoom) — the header's UI-scale control in miniature (`HeaderBar::uiScaleBounds`'s idiom:
	/// a bordered box, flat − and + at its ends, the percentage between), each click ONE `kZoomStep`, clamped as the
	/// wheel is. **OPT-IN PER POLICY** (`Policy::kZoomControl`): a policy that does not name it — the shaper's — draws
	/// and handles nothing new, so its pad is the pad it was (`if constexpr`, not a runtime flag).
	[[nodiscard]] static constexpr bool zoomControlShown() noexcept {
		if constexpr (requires { Policy::kZoomControl; }) {
			return Policy::kZoomControl;
		}
		else {
			return false;
		}
	}
	static constexpr float kZoomInset = 6.0f;
	static constexpr float kZoomButton = 18.0f;
	static constexpr float kZoomReadout = 42.0f;
	static constexpr float kZoomHeight = 16.0f;
	[[nodiscard]] visage::Bounds zoomBounds() const {
		return { kZoomInset, kZoomInset, 2.0f * kZoomButton + kZoomReadout, kZoomHeight };
	}
	[[nodiscard]] visage::Bounds zoomMinusBounds() const {
		const visage::Bounds b = zoomBounds();
		return { b.x(), b.y(), kZoomButton, b.height() };
	}
	[[nodiscard]] visage::Bounds zoomPlusBounds() const {
		const visage::Bounds b = zoomBounds();
		return { b.right() - kZoomButton, b.y(), kZoomButton, b.height() };
	}
	/// THE READOUT: the zoom as a percentage, the header's form ("100%", "112%", "25%").
	[[nodiscard]] std::string zoomText() const;

	/// **HOW MUCH TRAVEL A FULL DRAG COMMITS AT x1.** Divided by the zoom, which is what zoom is FOR on
	/// a joystick: the same gesture buys less ground, so a hand can place the field precisely instead
	/// of only coarsely. At x64 a full drag is a hundredth of what it is at x1.
	static constexpr double kTravelPerHalfDrag = 1.0;

	/// WHERE THE PUCK IS. At rest it is the centre — the chart's origin IS the current position, so
	/// there is never any standing travel for a turn of the plane to move.
	[[nodiscard]] visage::Point puck() const;

	/// **THE PAN, AS EXPLORE DOES IT (backlog IJ)** — the seams. `starForTest` is where the sound is and
	/// `viewForTest` where the view is centred, both in PLANE units from the frame's origin; at rest both
	/// are 0, because the frame re-origins on the sound once the view has glided home.
	[[nodiscard]] std::pair<double, double> starForTest() const noexcept { return { starX_, starY_ }; }
	[[nodiscard]] std::pair<double, double> viewForTest() const noexcept { return { viewX_, viewY_ }; }
	[[nodiscard]] bool glidingForTest() const noexcept { return gliding_; }
	/// LANDS A GLIDE AT ONCE — what a frame loop does over ~0.1 s, for a case with no frame loop.
	void finishGlideForTest() { finishGlide(); }
	/// ONE FRAME'S EDGE SCROLL, `seconds` long — what `draw` does every frame while a hand is on the pad.
	/// Returns whether the view moved. The pointer is wherever the last press or drag left it.
	bool tickForTest(double seconds) { return edgeScroll(seconds); }
	/// **WHERE THE EDGE SCROLL STARTS, in pixels inside the pad's real edge** — and it runs on out past the
	/// edge, which is the point: the hit box is the edge and everything beyond it.
	static constexpr double kEdgeBandPixels = edge_pan::kEdgeBandPixels;
	/// How much deeper than the band's start the scroll reaches full speed.
	static constexpr double kRampPixels = edge_pan::kRampPixels;
	/// Full speed, in views per second — Explore's `kViewsPerSecond`.
	static constexpr double kPanViewsPerSecond = edge_pan::kPanViewsPerSecond;
	/// How far inside the pad the star is held while the hand is beyond it: the puck stays whole.
	static constexpr double kStarInsetPixels = edge_pan::kStarInsetPixels;

	using Map = ExploreMap;
	static constexpr size_t kMapWide = Map::kWide;
	static constexpr size_t kMapHigh = Map::kHigh;
	using Plane = ExplorePlane<kPhaseCount>;

	[[nodiscard]] const std::array<float, kMapWide * kMapHigh>& mapForTest() const noexcept {
		return map_.cells();
	}

	/// **HOW MANY TIMES THE TERRAIN HAS BEEN BUILT.** A COUNT, not a comparison of pictures: the claim
	/// under test is "no rebuild happened", and a rebuild that lands on the same cells is invisible to
	/// a difference check while costing exactly as much. Produced BY `buildMap`, so it cannot report
	/// a build that did not run.
	[[nodiscard]] size_t rebuildsForTest() const noexcept { return rebuilds_; }
	/// WHAT THE MAP WAS BUILT FOR — the policy's key, read by value.
	[[nodiscard]] const View& builtForTest() const noexcept { return built_; }
	/// THE PAD'S SIZE, in whole logical pixels, when the map was last built — the half of "what it was built for"
	/// the policy's key does not hold.
	[[nodiscard]] std::pair<int, int> builtSizeForTest() const noexcept { return { builtW_, builtH_ }; }
	/// **THE THROTTLE'S CLOCK, PINNED** — from this call on, the key-rebuild throttle reads `now` instead of the steady
	/// clock, so a case can move a key every "frame" at a chosen spacing. Only the throttle reads it: the edge scroll and
	/// the glide keep the real clock, which their own seams (`tickForTest`, `finishGlideForTest`) already stand in for.
	void setClockForTest(std::chrono::steady_clock::time_point now) noexcept { clockForTest_ = now; }
	/// The policy's throttle — zero for a policy that names none (see the header's note on `kKeyRebuildInterval`).
	[[nodiscard]] static constexpr std::chrono::milliseconds keyRebuildInterval() noexcept {
		if constexpr (requires { Policy::kKeyRebuildInterval; }) {
			return Policy::kKeyRebuildInterval;
		}
		else {
			return std::chrono::milliseconds { 0 };
		}
	}

private:
	//-- THE PAN, AS EXPLORE DOES IT (backlog IJ, 2026-09-23) -------------------------------------//
	//
	// The same pan drag as Explore. The pad was a JOYSTICK: every drag event committed a delta,
	// re-anchored, and re-derived the plane's orientation from where the field had got to, while the
	// picture stood still until release and then jumped under the puck. Now it is Explore's gesture:
	//   - the press puts the sound (the STAR) where you pressed, on a map that stays put;
	//   - the star stays UNDER THE HAND, clamped `kStarInsetPixels` inside the pad;
	//   - while a DRAGGING hand is within `kEdgeBandPixels` of an edge, or past it, the view scrolls on
	//     TIME, every frame, at up to `kPanViewsPerSecond` — so dwelling at the edge keeps going. The
	//     first draft moved the view only on drag EVENTS and began 15% inside (Explore's margin, read
	//     off its code), which made the hit box narrow, not at the actual edge, and hard to dwell on;
	//   - the map is built `kMapMargin` wider than the view and drawn at an offset, so a pan reveals
	//     ground already drawn and a rebuild is owed only when the view leaves that margin;
	//   - on release the view GLIDES home to the star, and then the frame re-origins on the sound —
	//     which is where the old release rebuild already put it.
	// One scale for the pointer and the map: `planePerPixel`, from the SMALLER side. The joystick
	// measured the pointer that way but stretched the map across the whole width, so on the 420x300 pad a
	// press 150 px right of centre landed the sound drawn 210 px right — the picture lied horizontally.

	static constexpr double kMapMargin = 1.35;    ///< Explore's `kTerrainMargin`
	static constexpr double kGlideSeconds = 0.08; ///< the release glide's time constant

	/// Plane units per pixel — ONE scale for both axes, from the smaller side, divided by the zoom.
	[[nodiscard]] double planePerPixel() const;
	/// The view's half-extents in plane units.
	[[nodiscard]] std::pair<double, double> viewHalf() const;
	[[nodiscard]] visage::Point toPixel(double x, double y) const;

	/// RE-ANCHOR THE FRAME ON THE SOUND: read the standing travel, derive the basis there, and put the
	/// view, the star and the map at the origin. At rest, on a zoom or knob change, and when a glide lands.
	void reOrigin();
	/// BUILD THE MAP AROUND THE VIEW, IN THE CURRENT FRAME — mid-gesture, when the view leaves the margin.
	void buildMap();
	/// THE STAR UNDER THE (CLAMPED) POINTER, in the current view.
	void placeStar();
	/// Views per second along each axis for where the pointer is — zero outside the edge band.
	[[nodiscard]] std::pair<double, double> edgeVelocity() const;
	/// THE EDGE SCROLL, one step of `seconds`: moves the view, keeps the star under the hand, commits.
	bool edgeScroll(double seconds);
	/// Rebuild the map if the view has used most of the margin it was built with.
	void keepMapCovering();
	/// THE STAR'S TRAVEL, written: the frame's origin plus the basis carried to the star.
	void commit();
	void finishGlide();
	/// Whether the policy's key no longer names what the map was built for (the size is `draw`'s own check).
	[[nodiscard]] bool keyMoved() const;
	[[nodiscard]] std::chrono::steady_clock::time_point clockNow() const {
		return clockForTest_ ? *clockForTest_ : std::chrono::steady_clock::now();
	}

	View built_ {};
	/// -1 until the first build at a real size — none is made at construction, when the pad is 0 wide.
	int builtW_ = -1;
	int builtH_ = -1;
	/// WHEN `draw` LAST REBUILT FOR A MOVED KEY — the throttle's reference. Unset until the first: a key change after
	/// quiet always rebuilds at once, so only a key that KEEPS moving is ever deferred.
	std::optional<std::chrono::steady_clock::time_point> keyBuiltAt_ {};
	std::optional<std::chrono::steady_clock::time_point> clockForTest_ {};
	/// THE SIZE THE LAST `draw` SAW — a resize is rebuilt once a frame finds it unchanged (see `draw`).
	int seenW_ = -1;
	int seenH_ = -1;
	size_t rebuilds_ = 0;

	BlobWriter blob_;
	RowReader readRow_;
	device::Modulatable& rotate_;

	/// THE FRAME — fixed from one rest to the next, so the map, the press and the drag all read the plane
	/// the same way. `origin_` is the standing travel it is anchored on, in double; `basis_` its orientation
	/// (the PHASE half of the policy's `SliceBasis` — see `commit`).
	Phases origin_ {};
	Phases basisX_ {};
	Phases basisY_ {};
	double viewX_ = 0.0;
	double viewY_ = 0.0;
	double starX_ = 0.0;
	double starY_ = 0.0;
	double mapX_ = 0.0;
	double mapY_ = 0.0;
	bool held_ = false;
	bool gliding_ = false;
	std::chrono::steady_clock::time_point glideSteppedAt_ {};
	std::chrono::steady_clock::time_point edgeSteppedAt_ {};
	visage::Point pointer_ {};
	bool dragged_ = false;
	double zoom_ = 1.0;
	Map map_;
};

} // namespace feathers::ui
