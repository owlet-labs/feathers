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

#include "plugin/ui/twinfield_pad.h"

#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace feathers::ui {

namespace {
namespace tf = mod::twinfield;
using LP = tf::LayerParam;
constexpr float kDotRadius = 4.5f;
constexpr float kPuckRadius = 6.0f;
} // namespace

TwinFieldPad::TwinFieldPad(std::array<LayerControls, tf::kLayers> layers) : layers_(layers) {
	// **A DRAG MOVES THE PUCK WITH THE HAND: right is +X, UP IS MORE** — AutoMod's and Explore's sign (`placeStar`:
	// `starX = view + (px - w/2) k`, `starY = view + (h/2 - py) k`). The first build grabbed the ground instead, which
	// panned the puck the opposite way from every other surface in the plugin.
	onMouseDown() = [this](const visage::MouseEvent& e) {
		held_ = true;
		if (!focused_) {
			focused_ = true; // A PRESS FOCUSES; the wheel never does
			redraw();
		}
		pressAt_ = e.position;
		pressPanX_ = field(LP::PanX);
		pressPanY_ = field(LP::PanY);
		layers_[layer_][static_cast<size_t>(LP::PanX)]->beginGesture();
		layers_[layer_][static_cast<size_t>(LP::PanY)]->beginGesture();
	};
	onMouseDrag() = [this](const visage::MouseEvent& e) {
		if (!held_) {
			return;
		}
		const double k = planePerPixel();
		const auto write = [this](LP p, double v) {
			device::Modulatable& c = *layers_[layer_][static_cast<size_t>(p)];
			c.setValue(std::clamp(v, c.desc().min, c.desc().max));
		};
		write(LP::PanX, pressPanX_ + static_cast<double>(e.position.x - pressAt_.x) * k);
		write(LP::PanY, pressPanY_ - static_cast<double>(e.position.y - pressAt_.y) * k);
		redraw();
	};
	onMouseUp() = [this](const visage::MouseEvent&) {
		if (!held_) {
			return;
		}
		held_ = false;
		layers_[layer_][static_cast<size_t>(LP::PanX)]->endGesture();
		layers_[layer_][static_cast<size_t>(LP::PanY)]->endGesture();
	};
	// THE HOVER ROAD, LAST (`tooltip.h` says why it must come last).
	installTooltipHover(*this, *this);
	// VISAGE'S FOCUS, so a click elsewhere tells us (`focusChanged`) and Esc reaches us. Every other key is declined.
	setAcceptsKeystrokes(true);
}

void TwinFieldPad::setLayer(size_t layer) {
	layer_ = std::min(layer, tf::kLayers - 1);
	redraw();
}

void TwinFieldPad::offeredControls(std::vector<Offered>& out) const {
	// THE GESTURE ROAD: pan by drag and zoom by wheel, for the SHOWN layer.
	out.push_back({ layers_[layer_][static_cast<size_t>(LP::PanX)], Road::Gesture });
	out.push_back({ layers_[layer_][static_cast<size_t>(LP::PanY)], Road::Gesture });
	out.push_back({ layers_[layer_][static_cast<size_t>(LP::Zoom)], Road::Gesture });
}

const char* TwinFieldPad::tooltipAt(visage::Point) const {
	return tooltipOf(layers_[layer_][static_cast<size_t>(LP::PanX)]);
}

void TwinFieldPad::blur() {
	if (focused_) {
		focused_ = false;
		redraw();
	}
}

void TwinFieldPad::focusChanged(bool isFocused, bool) {
	// GAINING visage's focus does not focus the pad (a window regaining focus restores it without a press); only a
	// press does. LOSING it — a click on anything else — always ends it.
	if (!isFocused) {
		blur();
	}
}

bool TwinFieldPad::keyPress(const visage::KeyEvent& e) {
	if (focused_ && e.keyCode() == visage::KeyCode::Escape) {
		blur();
		return true;
	}
	return false;
}

void TwinFieldPad::visibilityChanged() {
	if (!isVisible()) {
		blur();
	}
}

bool TwinFieldPad::mouseWheel(const visage::MouseEvent& e) {
	// **ON A CARD, A PLAIN WHEEL IS THE LANE'S** (otherwise the terrain takes over a scroll across the lane
	// as it passes under the pointer). No in-lane surface claims a plain wheel — AutoMod's and
	// Pattern's terrains zoom on it only in their WINDOWS, Explore on its own stage — so the card zooms on the main
	// modifier (Ctrl, Cmd on macOS) and hands every other wheel up to the lane by answering false.
	// AND ZOOM FOLLOWS FOCUS: a FOCUSED pad zooms on a plain wheel too. Ctrl/Cmd+wheel always works.
	if (!focused_ && !e.isMainModifier()) {
		return false;
	}
	const float delta = e.wheel_delta_y;
	if (delta == 0.0f) {
		return false;
	}
	device::Modulatable& zoom = *layers_[layer_][static_cast<size_t>(LP::Zoom)];
	zoom.beginGesture();
	zoom.setValue(std::clamp(zoom.value() + (delta > 0.0f ? kZoomStepOct : -kZoomStepOct), zoom.desc().min,
	                         zoom.desc().max));
	zoom.endGesture();
	redraw();
	return true;
}

double TwinFieldPad::field(LP p) const {
	return layers_[layer_][static_cast<size_t>(p)]->value();
}

double TwinFieldPad::planePerPixel() const {
	// AUTOMOD'S PAD SCALE: the half of the smaller side shows `kHalfViewPlane / zoom` of the plane.
	const double half = std::max(std::min(width(), height()) * 0.5, 1.0);
	return tf::kHalfViewPlane / std::exp2(field(LP::Zoom)) / half;
}

bool TwinFieldPad::livePhase(double* phase) const {
	return phaseReader_ && phaseReader_(layer_, phase);
}

visage::Point TwinFieldPad::puckPixel() const {
	// WHERE THE DEVICE'S PUCK IS NOW: the pan point (the view's centre) plus the orbit at the PUBLISHED phase,
	// through the device's own `orbitOffset`. With no phase published it is the pan point itself.
	double phase = 0.0;
	if (!livePhase(&phase)) {
		return { static_cast<float>(width() * 0.5), static_cast<float>(height() * 0.5) };
	}
	const auto o = tf::orbitOffset(field(LP::Depth), field(LP::Spread), phase);
	const double ppp = planePerPixel();
	return { static_cast<float>(width() * 0.5 + o[0] / ppp), static_cast<float>(height() * 0.5 - o[1] / ppp) };
}

float TwinFieldPad::ringPixels() const {
	return static_cast<float>(tf::ringRadius(field(LP::Spread)) / planePerPixel());
}

visage::Point TwinFieldPad::pointPixel(size_t k) const {
	// THE EIGHT RIDE WITH THE PUCK — they are read around it, so they are drawn around it.
	const visage::Point puck = puckPixel();
	const auto o = tf::pointOffset(k, field(LP::Spread));
	const double ppp = planePerPixel();
	return { static_cast<float>(puck.x + o[0] / ppp), static_cast<float>(puck.y - o[1] / ppp) };
}

TwinFieldPad::Key TwinFieldPad::keyNow() const {
	Key k;
	k.panX = field(LP::PanX);
	k.panY = field(LP::PanY);
	k.zoom = field(LP::Zoom);
	k.rotate = field(LP::Rotate);
	k.gamma = field(LP::Gamma);
	k.w = width();
	k.h = height();
	k.layer = layer_;
	return k;
}

void TwinFieldPad::rebuildIfMoved() {
	const Key now = keyNow();
	if (now == built_) {
		return;
	}
	// THE MAP IS THE VIEW: every input the terrain reads is in the key, so none can change unseen.
	const tf::Plane plane = tf::planeFor(now.rotate, now.gamma);
	const double ppp = planePerPixel();
	const double hx = std::max(now.w, 2.0f) * 0.5 * ppp;
	const double hy = std::max(now.h, 2.0f) * 0.5 * ppp;
	map_.build([&](float ux, float uy) {
		const double x = now.panX + (static_cast<double>(ux) * 2.0 - 1.0) * hx;
		const double y = now.panY + (static_cast<double>(uy) * 2.0 - 1.0) * hy;
		return tf::terrainAt(plane, x, y);
	});
	built_ = now;
	++rebuilds_;
}

void TwinFieldPad::draw(visage::Canvas& canvas) {
	const float w = width();
	const float h = height();
	if (w <= 2.0f || h <= 2.0f) {
		return;
	}
	rebuildIfMoved();
	canvas.setColor(kVoid);
	canvas.rectangle(0.0f, 0.0f, w, h);
	// AUTOMOD'S PALETTE, so the two terrains read as one.
	map_.draw(canvas, w, h, [](float a) { return withAlpha(kAccent, 0.06f + a * 0.30f); });

	const float cx = w * 0.5f;
	const float cy = h * 0.5f;
	// THE RING AT ITS TERRAIN SIZE (Spread), drawn through the zoom: zoom in and it is larger on the glass.
	const float ring = ringPixels();

	// THE ORBIT'S PATH: where the puck travels, `Depth` rings wide around the pan point, so the orbit is visible.
	// Static; the moving part is the puck and its ring below.
	const float orbit = ring * static_cast<float>(field(LP::Depth));
	if (orbit > 1.0f) {
		canvas.setColor(withAlpha(kInk, 0.30f));
		canvas.ring(cx - orbit, cy - orbit, orbit * 2.0f, 1.0f);
	}
	const visage::Point puck = puckPixel();
	// A SMALL MARK ON THE PAN POINT, so the orbit's centre reads once the puck has left it.
	canvas.setColor(withAlpha(kInk, 0.45f));
	canvas.circle(cx - 2.0f, cy - 2.0f, 4.0f);
	// THE RING THE POINTS SIT ON, faint, riding with the puck.
	canvas.setColor(withAlpha(kLine, 0.35f));
	canvas.ring(puck.x - ring, puck.y - ring, ring * 2.0f, 1.0f);

	// THE EIGHT POINTS, numbered: Mod 1 east, counter-clockwise.
	const visage::Font& label = font(kKnobReadoutSize);
	for (size_t k = 0; k < tf::kPoints; ++k) {
		const visage::Point p = pointPixel(k);
		canvas.setColor(k == 0 ? kPointer : kInk);
		canvas.circle(p.x - kDotRadius, p.y - kDotRadius, kDotRadius * 2.0f);
		const float dx = (p.x - puck.x);
		const float dy = (p.y - puck.y);
		const float len = std::max(std::sqrt(dx * dx + dy * dy), 1.0f);
		const float tx = p.x + dx / len * 11.0f;
		const float ty = p.y + dy / len * 11.0f;
		const std::string n = std::to_string(k + 1);
		canvas.setColor(withAlpha(kInk, 0.8f));
		canvas.text(n, label, visage::Font::kCenter, tx - 6.0f, ty - 6.0f, 12.0f, 12.0f);
	}

	// THE PUCK WHERE THE DEVICE HAS IT NOW, AutoMod's ring.
	canvas.setColor(kPointer);
	canvas.circle(puck.x - kPuckRadius, puck.y - kPuckRadius, kPuckRadius * 2.0f);
	canvas.setColor(kVoid);
	canvas.circle(puck.x - kPuckRadius + 2.0f, puck.y - kPuckRadius + 2.0f, (kPuckRadius - 2.0f) * 2.0f);
	// FOCUSED: a subtle accent outline says the plain wheel zooms here now.
	if (focused_) {
		canvas.setColor(withAlpha(kAccent, 0.65f));
		canvas.rectangleBorder(0.0f, 0.0f, w, h, 1.5f);
		return;
	}
	canvas.setColor(withAlpha(kLine, 0.5f));
	canvas.rectangleBorder(0.0f, 0.0f, w, h, 1.0f);
}

} // namespace feathers::ui
