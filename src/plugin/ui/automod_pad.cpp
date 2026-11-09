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
#include "plugin/ui/automod_pad.h"

#include "plugin/ui/theme.h"
#include "plugin/ui/celestial.h" // reducedMotionRequested — the release glide lands at once under it
#include "fx/automod/automod_derived.h"

#include <algorithm>
#include <cmath>

namespace feathers::ui {

namespace {
namespace ter = automod_terrain;
namespace amd = fx::automod;
namespace ps = amd::phase_space;

/// THE PUCK'S RADIUS — the shaper's. A ring rather than a disc, so the map underneath it stays
/// readable: the position and the place it sits in are both facts the player is reading at once.
constexpr float kPuckRadius = 6.0f;
/// THE REFERENCE GRID: this many squares across the view at any zoom — see `draw`.
constexpr double kReferenceLines = 8.0;
} // namespace

AutomodPad::AutomodPad(BlobWriter blob, RowReader readRow, device::Modulatable& rotate)
    : blob_(std::move(blob)), readRow_(std::move(readRow)), rotate_(rotate) {
	// THE FIRST PICTURE, of the plane the puck starts in. Not a constant of the device — see
	// `reOrigin`, and `viewMoved` for when it is asked for again.
	reOrigin();

	// **THE PRESS PUTS THE STAR WHERE YOU PRESSED** — Explore's press: clicking somewhere also moves the
	// puck, because the sound at a point of the picture is what a press there commits,
	// because the map and the pointer share one scale and one frame. A press on the centre commits the
	// standing travel unchanged, which is the honest answer rather than a special case.
	onMouseDown() = [this](const visage::MouseEvent& e) {
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
	// its puck on the edge it crossed. What moves the VIEW is `edgeScroll`, run every frame while held,
	// so a hand DWELLING at the edge keeps panning without having to wiggle.
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

void AutomodPad::setZoom(double zoom) {
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

bool AutomodPad::mouseWheel(const visage::MouseEvent& e) {
	// A NOTCH, NOT A GESTURE — `explore_stage.h` measured why: visage's X11 backend reads scroll from
	// core buttons 4-7 and converts them to exactly +/-1. Handling the notch is the whole of what this
	// platform offers, and the same is true one surface over on the shaper.
	const float delta = e.wheel_delta_y;
	if (delta == 0.0f) {
		return false;
	}
	setZoom(delta > 0.0f ? zoom_ * kZoomStep : zoom_ / kZoomStep);
	return true;
}

double AutomodPad::planePerPixel() const {
	// **A HALF-DRAG CARRIES `kZonesPerHalfDrag` ZONES, DIVIDED BY THE ZOOM** — the joystick's scale,
	// kept, so a hand that knew the pad finds the same ground under the same distance.
	const double half = std::max(std::min(width(), height()) * 0.5, 1.0);
	const double carry = static_cast<double>(kZonesPerHalfDrag) / static_cast<double>(ter::kZonesShown) / zoom_;
	return carry / half;
}

std::pair<double, double> AutomodPad::viewHalf() const {
	const double k = planePerPixel();
	return { std::max(width(), 2.0f) * 0.5 * k, std::max(height(), 2.0f) * 0.5 * k };
}

visage::Point AutomodPad::toPixel(double x, double y) const {
	const double k = planePerPixel();
	return { static_cast<float>(width() * 0.5 + (x - viewX_) / k),
		     static_cast<float>(height() * 0.5 - (y - viewY_) / k) };
}

void AutomodPad::placeStar() {
	const double inset = kStarInsetPixels;
	const double px = std::clamp(static_cast<double>(pointer_.x), inset, std::max(inset, width() - inset));
	const double py = std::clamp(static_cast<double>(pointer_.y), inset, std::max(inset, height() - inset));
	const double k = planePerPixel();
	starX_ = viewX_ + (px - width() * 0.5) * k;
	starY_ = viewY_ + (height() * 0.5 - py) * k; // UP IS MORE
}

std::pair<double, double> AutomodPad::edgeVelocity() const {
	// **THE HIT BOX IS THE EDGE ITSELF** — the shaper's, from its audition: a band `kEdgeBandPixels` wide
	// INSIDE the real edge and everything beyond it, ramping to full speed `kRampPixels` deeper.
	const auto rate = [](double depth) {
		return depth <= 0.0 ? 0.0 : kPanViewsPerSecond * std::min(1.0, depth / kRampPixels);
	};
	const double x = pointer_.x;
	const double y = pointer_.y;
	const double w = width();
	const double h = height();
	const double vx = rate(x - (w - kEdgeBandPixels)) - rate(kEdgeBandPixels - x);
	const double vy = rate(kEdgeBandPixels - y) - rate(y - (h - kEdgeBandPixels)); // UP IS MORE
	return { vx, vy };
}

bool AutomodPad::edgeScroll(double seconds) {
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

void AutomodPad::keepMapCovering() {
	// THE MAP COVERS `kMapMargin` OF THE VIEW; once the view has used most of that slack, build again
	// around where it now is.
	const auto [hx, hy] = viewHalf();
	const double slackX = hx * (kMapMargin - 1.0) * 0.9;
	const double slackY = hy * (kMapMargin - 1.0) * 0.9;
	if (std::fabs(viewX_ - mapX_) > slackX || std::fabs(viewY_ - mapY_) > slackY) {
		buildMap();
	}
}

void AutomodPad::commit() {
	// **THE STAR, ABSOLUTE IN THE FRAME — not a delta.** The joystick added a per-event delta to an
	// accumulator and re-derived the basis from where that had got to, so the plane turned under the
	// hand. Here the frame's standing travel and basis are held for the gesture and the star's position
	// in it is written whole, so the same point of the picture always commits the same sound.
	// **ACCUMULATED IN DOUBLE, STORED WRAPPED** — `wrapForStorage` wraps each coordinate to ITS OWN
	// period before the float conversion; see `automod_derived.h` for why a whole lap is not a period.
	std::array<double, amd::kFoldedTravelFloats> travel {};
	for (size_t i = 0; i < amd::kFoldedTravelFloats; ++i) {
		travel[i] = origin_[i] + basis_.travelTerm(i, starX_, starY_);
	}
	std::array<float, amd::kFoldedTravelFloats> stored {};
	amd::wrapForStorage(travel, stored);
	for (size_t i = 0; i < amd::kFoldedTravelFloats; ++i) {
		blob_(i, stored[i]);
	}
}

void AutomodPad::finishGlide() {
	gliding_ = false;
	// THE FRAME RE-ORIGINS ON THE SOUND, which the committed travel already is.
	reOrigin();
	redraw();
}

visage::Point AutomodPad::puck() const {
	// THE STAR, WHERE THE VIEW SHOWS IT. At rest the frame is anchored on it, so it is the centre.
	return toPixel(starX_, starY_);
}

AutomodPad::View AutomodPad::viewNow() const {
	const auto field = [this](amd::Param p) {
		return static_cast<double>(readRow_(static_cast<size_t>(p)));
	};
	View now {};
	now.type = field(amd::Param::Type);
	now.flavour = field(amd::Param::Flavor);
	now.mod = field(amd::Param::Mod);
	now.gamma = field(amd::Param::Gamma);
	now.rotate = rotate_.value();
	now.zoom = zoom_;
	for (size_t i = 0; i < ps::kTriangles; ++i) {
		now.standing[i] = static_cast<double>(readRow_(amd::kFoldedTravelFirst + i));
	}
	return now;
}

bool AutomodPad::viewMoved() const {
	return !(viewNow() == built_);
}

void AutomodPad::reOrigin() {
	// **THE PLANE IS A FUNCTION OF THE HOME, so the orientation is derived here and HELD for the frame.**
	// The joystick re-derived it on every event, so the plane turned under the hand as it moved.
	// Everything is read fresh from the row — a patch load or another surface may have moved it.
	const View now = viewNow();
	home_ = ps::homeFor(now.type, now.flavour, now.mod);
	ps::pushAlong(home_, ps::kGammaAxis, now.gamma);
	for (size_t i = 0; i < ps::kTriangles; ++i) {
		origin_[i] = now.standing[i];
		home_[i] += origin_[i];
	}
	basis_ = ps::basisFor(now.rotate, ps::planeHomeOf(home_));
	viewX_ = viewY_ = starX_ = starY_ = 0.0;
	buildMap();
}

void AutomodPad::buildMap() {
	// **THE TERRAIN IS THE PLANE THE PUCK MOVES IN** — each cell the sound a press there would commit, in
	// the frame the press reads. Built `kMapMargin` wider than the view, around where the view is.
	mapX_ = viewX_;
	mapY_ = viewY_;
	const auto [hx, hy] = viewHalf();
	const double ex = hx * kMapMargin;
	const double ey = hy * kMapMargin;

	// THE DERIVE'S OTHER INPUTS, held nominal — the terms `automod_terrain` sums do not read the rate
	// or the three coordinates once the phases are given, so neither is a terrain input.
	amd::AutomodCoords coords {};
	coords.lfoHz = 1.0f;

	map_.build([&](float ux, float uy) {
		const double px = mapX_ + (static_cast<double>(ux) * 2.0 - 1.0) * ex;
		const double py = mapY_ + (static_cast<double>(uy) * 2.0 - 1.0) * ey;
		return ter::activityOf(amd::deriveAutomodAt(coords, ps::phasesAt(home_, basis_, px, py)));
	});

	// THE KEY IS THE FRAME'S — the home and standing travel it was anchored on — plus the zoom now.
	View key = viewNow();
	for (size_t i = 0; i < ps::kTriangles; ++i) {
		key.standing[i] = origin_[i];
	}
	built_ = key;
	++rebuilds_;
}

void AutomodPad::draw(visage::Canvas& canvas) {
	// A VIEW THAT HAS MOVED UNDER THE PAD (a knob, a patch load, a modulated coordinate) re-anchors — but
	// never while a hand is on it or the view is gliding home, when the frame is the gesture's and must
	// hold still. During a gesture the standing travel moves on every event, which is exactly why.
	if (!held_ && !gliding_ && viewMoved()) {
		reOrigin();
	}
	// THE EDGE SCROLL, on TIME while a hand is on the pad. The step is capped so a stalled frame cannot fling.
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

	// THE FIELD, as a coarse brightness map, `kMapMargin` wider than the view and centred where it was
	// built — so a pan slides it under the view instead of waiting for a rebuild. `automod_terrain.h`
	// records why the normaliser is twice the field's mean rather than its maximum.
	const visage::Point centre = toPixel(mapX_, mapY_);
	const float mw = w * static_cast<float>(kMapMargin);
	const float mh = h * static_cast<float>(kMapMargin);
	map_.drawAt(canvas, centre.x - mw * 0.5f, centre.y - mh * 0.5f, mw, mh,
	            [](float a) { return withAlpha(kAccent, 0.06f + a * 0.30f); });

	// **NO ZONE GRID, BY DECISION:** the zone partitions leave phase space and are chosen separately,
	// from a dropdown with prev/next buttons for quick exploration. A partition drawn
	// on a rotating plane is a line through country it does not describe.
	//
	// A LIGHT REFERENCE GRID INSTEAD, meaning nothing but scale — decided for this pad, and the one drawing
	// the shaper's does not have. **IT IS PINNED TO THE GROUND, NOT THE GLASS**: its lines sit at fixed
	// plane positions, so a pan slides them with the terrain and they say how far you have come. At rest
	// the view is on the frame's origin, so a line runs through the centre, as it always did.
	{
		const double k = planePerPixel();
		const double step = std::min(w, h) * k / kReferenceLines;
		const auto [hx, hy] = viewHalf();
		canvas.setColor(withAlpha(kLine, 0.14f));
		for (double gx = std::ceil((viewX_ - hx) / step) * step; gx <= viewX_ + hx; gx += step) {
			const float x = toPixel(gx, 0.0).x;
			canvas.segment(x, 0.0f, x, h, 1.0f, true);
		}
		for (double gy = std::ceil((viewY_ - hy) / step) * step; gy <= viewY_ + hy; gy += step) {
			const float y = toPixel(0.0, gy).y;
			canvas.segment(0.0f, y, w, y, 1.0f, true);
		}
	}

	// THE PUCK, AND ITS CROSSHAIR — the shaper's: a ring so the map under it stays readable, and the
	// crosshair through the PUCK, so a held gesture reads as where the sound is.
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
}

} // namespace feathers::ui
