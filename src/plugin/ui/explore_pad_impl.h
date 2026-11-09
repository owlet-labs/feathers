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

// THE EXPLORE PAD'S MEMBER DEFINITIONS — included ONLY by a TU that explicitly instantiates a pad
// (`template class ExplorePad<SomePolicy>;`). Everyone else sees `explore_pad.h` and an `extern template`.
//
// MOVED FROM `shaper_pad.cpp` (phaser plan, Task 9a) with the shaper's four hard-codings replaced by policy
// calls and nothing else changed: every expression that touches the travel, the basis or the map is the shaper's
// own, in the shaper's association, which is what keeps the shaper bit-identical.

#include "plugin/ui/explore_pad.h"

#include "plugin/ui/celestial.h" // reducedMotionRequested — the release glide lands at once under it
#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>

namespace feathers::ui {

namespace explore_pad_detail {
inline constexpr float kPuckRadius = 6.0f;
} // namespace explore_pad_detail

template <class Policy>
ExplorePad<Policy>::ExplorePad(BlobWriter blob, RowReader readRow, device::Modulatable& rotate)
    : blob_(std::move(blob)), readRow_(std::move(readRow)), rotate_(rotate) {
	// THE FRAME THE PUCK STARTS IN. Not a constant of the device — see `reOrigin`, and `draw` for when it is asked
	// for again. No picture yet: the pad is 0 wide here, so `buildMap` waits for the first `draw` at a real size.
	reOrigin();

	// **THE PRESS PUTS THE STAR WHERE YOU PRESSED** — Explore's press, and the older rule that a click
	// anywhere also moves the puck: the sound at a point of the picture is what a
	// press there commits, because the map and the pointer share one scale and one frame.
	onMouseDown() = [this](const visage::MouseEvent& e) {
		// THE ZOOM BOX TAKES ITS OWN PRESSES (a policy that shows it): − and + step one notch, the readout swallows the
		// press, and none of them puts the star anywhere.
		if constexpr (zoomControlShown()) {
			if (zoomBounds().contains(e.position)) {
				if (zoomMinusBounds().contains(e.position)) {
					setZoom(zoom_ / kZoomStep);
				}
				else if (zoomPlusBounds().contains(e.position)) {
					setZoom(zoom_ * kZoomStep);
				}
				return;
			}
		}
		// A GLIDE STILL RUNNING LANDS FIRST, so this press is measured in a frame anchored on the sound.
		if (gliding_) {
			finishGlide();
		}
		held_ = true;
		if (onHoldBegan) {
			onHoldBegan();
		}
		dragged_ = false;
		pointer_ = e.position;
		edgeSteppedAt_ = std::chrono::steady_clock::now();
		if (width() <= 1.0f || height() <= 1.0f) {
			return;
		}
		placeStar();
		commit();
		redraw();
	};

	// **THE STAR IS UNDER THE HAND** — clamped just inside the pad, so a hand that has left it still has
	// its puck on the edge it crossed. What moves the VIEW is not this handler: it is `edgeScroll`, run
	// every frame while held, so a hand DWELLING at the edge keeps scrolling without having to wiggle.
	onMouseDrag() = [this](const visage::MouseEvent& e) {
		if (!held_ || width() <= 1.0f || height() <= 1.0f) {
			return;
		}
		dragged_ = true;
		pointer_ = e.position;
		placeStar();
		commit();
		redraw();
	};
	// **THE RELEASE GLIDES THE VIEW HOME TO THE STAR** — Explore's `glideToSelected`. Under reduced motion
	// it lands at once, which is also what a venue photographing the pad wants.
	onMouseUp() = [this](const visage::MouseEvent&) {
		if (!held_) {
			return;
		}
		held_ = false;
		if (onHoldEnded) {
			onHoldEnded();
		}
		if (reducedMotionRequested()) {
			finishGlide();
			return;
		}
		gliding_ = true;
		glideSteppedAt_ = std::chrono::steady_clock::now();
		redraw();
	};
}

template <class Policy>
void ExplorePad<Policy>::setZoom(double zoom) {
	const double want = std::clamp(zoom, kMinZoom, kMaxZoom);
	if (want == zoom_) {
		return;
	}
	zoom_ = want;
	// MID-GESTURE THE FRAME HOLDS and only the picture's scale changes; at rest the view re-anchors.
	if (held_ || gliding_) {
		buildMap();
	}
	else {
		reOrigin();
	}
	redraw();
}

template <class Policy>
double ExplorePad<Policy>::planePerPixel() const {
	// ONE SCALE, FROM THE SMALLER SIDE — see the note at `kMapMargin` for the anisotropy it replaced.
	const double half = std::max(std::min(width(), height()) * 0.5, 1.0);
	return kTravelPerHalfDrag / zoom_ / half;
}

template <class Policy>
std::pair<double, double> ExplorePad<Policy>::viewHalf() const {
	const double k = planePerPixel();
	return { std::max(width(), 2.0f) * 0.5 * k, std::max(height(), 2.0f) * 0.5 * k };
}

template <class Policy>
visage::Point ExplorePad<Policy>::toPixel(double x, double y) const {
	const double k = planePerPixel();
	return { static_cast<float>(width() * 0.5 + (x - viewX_) / k),
		     static_cast<float>(height() * 0.5 - (y - viewY_) / k) };
}

template <class Policy>
void ExplorePad<Policy>::placeStar() {
	const double inset = kStarInsetPixels;
	const double px = std::clamp(static_cast<double>(pointer_.x), inset, std::max(inset, width() - inset));
	const double py = std::clamp(static_cast<double>(pointer_.y), inset, std::max(inset, height() - inset));
	const double k = planePerPixel();
	starX_ = viewX_ + (px - width() * 0.5) * k;
	starY_ = viewY_ + (height() * 0.5 - py) * k; // UP IS MORE
}

template <class Policy>
std::pair<double, double> ExplorePad<Policy>::edgeVelocity() const {
	// **THE HIT BOX IS THE EDGE ITSELF** — it works in Explore because the hit box extends to the edges,
	// and a box inside them was hard to dwell on. The first draft panned on Explore's 15%
	// margin, so the terrain slid under a hand still well inside the box, and it moved only on mouse
	// EVENTS, so a hand held still at the edge stopped it. Now: a band `kEdgeBandPixels` wide INSIDE the
	// real edge and everything beyond it, ramping to full speed `kRampPixels` deeper.
	// The law itself is `edge_pan::velocity`, shared with the Pattern's map so the two cannot drift apart.
	return edge_pan::velocity(pointer_.x, pointer_.y, width(), height());
}

template <class Policy>
bool ExplorePad<Policy>::edgeScroll(double seconds) {
	// ONLY A DRAG SCROLLS: a still press near the edge is a click to hear that sound, not a pan.
	if (!held_ || !dragged_ || seconds <= 0.0 || width() <= 1.0f || height() <= 1.0f) {
		return false;
	}
	const auto [vx, vy] = edgeVelocity();
	if (vx == 0.0 && vy == 0.0) {
		return false;
	}
	const auto [hx, hy] = viewHalf();
	viewX_ += vx * seconds * 2.0 * hx;
	viewY_ += vy * seconds * 2.0 * hy;
	keepMapCovering();
	// THE PUCK STAYS UNDER THE HAND while the ground slides — so the sound walks with the view.
	placeStar();
	commit();
	return true;
}

template <class Policy>
void ExplorePad<Policy>::keepMapCovering() {
	// THE MAP COVERS `kMapMargin` OF THE VIEW; once the view has used most of that slack, build again
	// around where it now is. At `kPanViewsPerSecond` that is a rebuild every ~0.15 s of full-speed pan.
	const auto [hx, hy] = viewHalf();
	const double slackX = hx * (kMapMargin - 1.0) * 0.9;
	const double slackY = hy * (kMapMargin - 1.0) * 0.9;
	if (std::fabs(viewX_ - mapX_) > slackX || std::fabs(viewY_ - mapY_) > slackY) {
		buildMap();
	}
}

template <class Policy>
void ExplorePad<Policy>::commit() {
	// **ONLY THE PHASE HALF OF THE BASIS.** `SliceBasis<N>` covers 2N — N phase terms and N frequency
	// multipliers, the phi triangle's FM. Neither device that uses this pad evaluates a rate today (on the
	// shaper, FM would just multiply phase), so the FM half has nowhere to go here; the space is reserved for it.
	Phases travel {};
	for (size_t i = 0; i < kPhaseCount; ++i) {
		travel[i] = origin_[i] + basisX_[i] * starX_ + basisY_[i] * starY_;
	}
	std::array<float, kPhaseCount> stored {};
	Policy::wrapForStorage(travel, stored);
	for (size_t i = 0; i < kPhaseCount; ++i) {
		blob_(i, stored[i]);
	}
}

template <class Policy>
void ExplorePad<Policy>::finishGlide() {
	gliding_ = false;
	// THE FRAME RE-ORIGINS ON THE SOUND, which the committed travel already is — the same rebuild the
	// joystick's release made, now after the view has arrived rather than instead of it.
	reOrigin();
	redraw();
}

template <class Policy>
visage::Point ExplorePad<Policy>::puck() const {
	// THE STAR, WHERE THE VIEW SHOWS IT. At rest the frame is anchored on it, so it is the centre.
	return toPixel(starX_, starY_);
}

template <class Policy>
void ExplorePad<Policy>::reOrigin() {
	// THE STANDING TRAVEL, READ FRESH FROM THE ROW — a patch load or another surface may have moved it.
	for (size_t i = 0; i < kPhaseCount; ++i) {
		origin_[i] = static_cast<double>(readRow_(Policy::kTravelFirst + i));
	}
	// **THE PLANE IS A FUNCTION OF THE HOME, so the orientation is derived here and HELD for the frame.**
	// The joystick re-derived it on every event from wherever the field had got to, so the plane turned
	// under the hand as it moved and the picture — built at the press — stopped matching the drag. Where the
	// home comes from is the policy's: the shaper's moves with Colour and Gamma, the phaser's is a constant.
	const feathers::SliceBasis<kPhaseCount> basis = Policy::basisAt(readRow_, rotate_.value(), origin_);
	for (size_t i = 0; i < kPhaseCount; ++i) {
		basisX_[i] = static_cast<double>(basis.x[i]);
		basisY_[i] = static_cast<double>(basis.y[i]);
	}
	viewX_ = viewY_ = starX_ = starY_ = 0.0;
	buildMap();
}

template <class Policy>
void ExplorePad<Policy>::buildMap() {
	// **THE TERRAIN IS THE PLANE THE PUCK MOVES IN** — each cell the sound a press there would commit,
	// in the frame the press reads: the origin plus the basis carried to that cell. Built `kMapMargin`
	// wider than the view, around where the view is. WHAT the terrain says about a cell is the policy's.
	mapX_ = viewX_;
	mapY_ = viewY_;
	const auto [hx, hy] = viewHalf();
	const Plane plane { origin_, basisX_, basisY_, mapX_, mapY_, hx * kMapMargin, hy * kMapMargin };
	// NOTHING TO PAINT AT 2 PX OR LESS — the constructor's `reOrigin` runs at width 0, and a full terrain for a 2x2 view
	// is a build thrown away by the first `draw` at a real size (which `builtW_ == -1` makes immediate, see `draw`).
	if (width() <= 2.0f || height() <= 2.0f) {
		return;
	}
	// THE KEY FIRST, AND THE PAINTER READS ONLY IT — so the map is a picture of exactly what `built_` says it is,
	// whatever the row does during the build (see the header's note on the Task 9a review).
	const View key = Policy::keyOf(readRow_, rotate_.value(), zoom_, origin_);
	Policy::paint(map_, key, plane);
	built_ = key;
	builtW_ = static_cast<int>(std::lround(width()));
	builtH_ = static_cast<int>(std::lround(height()));
	++rebuilds_;
}

template <class Policy>
bool ExplorePad<Policy>::mouseWheel(const visage::MouseEvent& e) {
	// **WHAT ARRIVES HERE IS A NOTCH, NOT A GESTURE**, and `explore_stage.h` measured why: visage's
	// X11 backend reads scroll from core buttons 4-7 and converts them to exactly +/-1, never opening
	// XInput2 — so there are no high-resolution axes and no pinch. A touchpad's two-finger scroll
	// arrives because libinput emulates the legacy buttons for it. Handling the notch is the whole of
	// what this platform offers, and the same is true one surface over.
	const float delta = e.wheel_delta_y;
	if (delta == 0.0f) {
		return false;
	}
	setZoom(delta > 0.0f ? zoom_ * kZoomStep : zoom_ / kZoomStep);
	return true;
}

template <class Policy>
bool ExplorePad<Policy>::keyMoved() const {
	Phases standing {};
	for (size_t i = 0; i < kPhaseCount; ++i) {
		standing[i] = static_cast<double>(readRow_(Policy::kTravelFirst + i));
	}
	return !(Policy::keyOf(readRow_, rotate_.value(), zoom_, standing) == built_);
}

template <class Policy>
void ExplorePad<Policy>::draw(visage::Canvas& canvas) {
	using explore_pad_detail::kPuckRadius;
	// A VIEW THAT HAS MOVED UNDER THE PAD (a knob, a patch load, a zoom) re-anchors — but never while a
	// hand is on it or the view is gliding home, when the frame is the gesture's and must hold still.
	//
	// **A RESIZE IS TAKEN ONCE IT SETTLES** (the Task 9b review): a window dragged by its grip changes size every frame,
	// and a ~10 ms build per frame is the whole budget. So a new size is only NOTED the first frame it is seen, and the
	// map is rebuilt on a later frame that finds the size unchanged; meanwhile the old map is drawn scaled to the pad
	// (a frame or two of stretch, which the drag itself hides). A pad with no map yet (`builtW_ == -1`: nothing is built
	// at construction, at width 0) builds at once, so the first picture is never a stretched one.
	const int sizeW = static_cast<int>(std::lround(width()));
	const int sizeH = static_cast<int>(std::lround(height()));
	bool sizeSettling = false;
	if ((sizeW != builtW_ || sizeH != builtH_) && builtW_ > 2 && builtH_ > 2
	    && (sizeW != seenW_ || sizeH != seenH_)) {
		sizeSettling = true;
		redraw(); // come back next frame to see whether it held
	}
	seenW_ = sizeW;
	seenH_ = sizeH;
	if (!held_ && !gliding_ && !sizeSettling) {
		// THE PAD'S SIZE FIRST: the map's extent in plane units is the view's (`viewHalf`), so a map built at another
		// size is another region drawn stretched over this one. LOGICAL pixels, which a DPI change does not move — and
		// the map is drawn in logical units, so a scale change needs no rebuild. A size that has settled is never
		// throttled: the settle above already is its throttle.
		if (sizeW != builtW_ || sizeH != builtH_) {
			reOrigin();
		}
		else if (keyMoved()) {
			// **A MOVING KEY IS THROTTLED, IF THE POLICY ASKS** (`kKeyRebuildInterval`, the final review): a phaser
			// build is 11-17 ms and its key holds Center, Delay, Feedback, Mix and the rest, so a knob drag or a
			// DAW lane moving one rebuilt every frame. The first change after quiet rebuilds at once; one that comes
			// sooner than the interval after the last is deferred, and the frame keeps coming back (`redraw`) until
			// the interval has passed, when it rebuilds from the key AS IT THEN IS — so the last picture is always of
			// the final key, however the changes were spaced. The shaper names no interval and is unchanged.
			const auto interval = keyRebuildInterval();
			const auto now = clockNow();
			if (interval.count() == 0 || !keyBuiltAt_ || now - *keyBuiltAt_ >= interval) {
				reOrigin();
				keyBuiltAt_ = now;
			}
			else {
				redraw(); // owed: come back once the interval has passed
			}
		}
	}
	// THE EDGE SCROLL, on TIME while a hand is on the pad — so a hand that dwells at the edge keeps
	// panning without a mouse event to drive it. The step is capped so a stalled frame cannot fling.
	if (held_) {
		const auto now = std::chrono::steady_clock::now();
		const double dt = std::min(std::chrono::duration<double>(now - edgeSteppedAt_).count(), 0.05);
		edgeSteppedAt_ = now;
		if (edgeScroll(dt) || (dragged_ && edgeVelocity() != std::pair<double, double> { 0.0, 0.0 })) {
			redraw();
		}
	}
	// THE GLIDE, one step per frame on a time constant, until the view is on the star.
	if (gliding_) {
		const auto now = std::chrono::steady_clock::now();
		const double dt = std::chrono::duration<double>(now - glideSteppedAt_).count();
		glideSteppedAt_ = now;
		const double alpha = 1.0 - std::exp(-std::max(dt, 0.0) / kGlideSeconds);
		viewX_ += (starX_ - viewX_) * alpha;
		viewY_ += (starY_ - viewY_) * alpha;
		const auto [hx, hy] = viewHalf();
		if (std::fabs(starX_ - viewX_) < hx * 1e-3 && std::fabs(starY_ - viewY_) < hy * 1e-3) {
			finishGlide();
		}
		else {
			keepMapCovering();
			redraw();
		}
	}
	const float w = width();
	const float h = height();
	if (w <= 2.0f || h <= 2.0f) {
		return;
	}
	canvas.setColor(kVoid);
	canvas.rectangle(0.0f, 0.0f, w, h);
	// THE MAP, `kMapMargin` WIDER THAN THE VIEW AND CENTRED WHERE IT WAS BUILT — so a pan slides it under
	// the view instead of waiting for a rebuild. The frame clips what falls outside.
	const visage::Point centre = toPixel(mapX_, mapY_);
	const float mw = w * static_cast<float>(kMapMargin);
	const float mh = h * static_cast<float>(kMapMargin);
	map_.drawAt(canvas, centre.x - mw * 0.5f, centre.y - mh * 0.5f, mw, mh,
	            [](float a) { return withAlpha(kAccent, 0.06f + a * 0.30f); });

	const visage::Point p = puck();
	canvas.setColor(withAlpha(kInk, 0.35f));
	canvas.segment(0.0f, p.y, w, p.y, 1.0f, true);
	canvas.segment(p.x, 0.0f, p.x, h, 1.0f, true);
	canvas.setColor(kPointer);
	canvas.circle(p.x - kPuckRadius, p.y - kPuckRadius, kPuckRadius * 2.0f);
	canvas.setColor(kVoid);
	canvas.circle(p.x - kPuckRadius + 2.0f, p.y - kPuckRadius + 2.0f, (kPuckRadius - 2.0f) * 2.0f);
	canvas.setColor(withAlpha(kLine, 0.5f));
	canvas.rectangleBorder(0.0f, 0.0f, w, h, 1.0f);
	if constexpr (zoomControlShown()) {
		// THE HEADER'S UI-SCALE BOX, small: a bordered box, flat − and + at its ends, the percentage between.
		const visage::Bounds box = zoomBounds();
		const visage::Bounds minus = zoomMinusBounds();
		const visage::Bounds plus = zoomPlusBounds();
		const visage::Font& text = font(kKnobReadoutSize);
		canvas.setColor(kLine);
		canvas.fill(box.x(), box.y(), box.width(), box.height());
		canvas.setColor(kSurface);
		canvas.fill(box.x() + 1.0f, box.y() + 1.0f, box.width() - 2.0f, box.height() - 2.0f);
		canvas.setColor(kSurfaceRaised);
		canvas.fill(minus.x() + 1.0f, minus.y() + 1.0f, minus.width() - 1.0f, minus.height() - 2.0f);
		canvas.fill(plus.x(), plus.y() + 1.0f, plus.width() - 1.0f, plus.height() - 2.0f);
		canvas.setColor(kMuted);
		canvas.text("\xe2\x88\x92", text, visage::Font::kCenter, minus.x(), minus.y(), minus.width(), minus.height());
		canvas.text("+", text, visage::Font::kCenter, plus.x(), plus.y(), plus.width(), plus.height());
		canvas.setColor(kInk);
		canvas.text(zoomText(), text, visage::Font::kCenter, minus.right(), box.y(), plus.x() - minus.right(),
		            box.height());
	}
}

template <class Policy>
std::string ExplorePad<Policy>::zoomText() const {
	return std::to_string(static_cast<long>(std::lround(zoom_ * 100.0))) + "%";
}

} // namespace feathers::ui
