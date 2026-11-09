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

#include "plugin/ui/expanded_canvas.h"

#include "plugin/ui/celestial.h"
#include "plugin/ui/mod_state.h"
#include "plugin/ui/rail.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>

namespace feathers::ui {

//--------------------------------------------------------------------------------------------------//
// The starfield.                                                                                   //
//--------------------------------------------------------------------------------------------------//

void StarfieldBackdrop::draw(visage::Canvas& canvas, visage::Bounds area, double phase) {
	if (area.width() <= 0.0f || area.height() <= 0.0f) {
		return;
	}
	const float p = static_cast<float>(phase);
	// THE PERFORM SKY'S LUMINANCE TERM, PINNED. There it is a field sample under the star; here there
	// is no field, and 0.5 is the middle of the range it takes.
	constexpr float kLuminance = 0.5f;
	for (const SkyStar& star : performSky()) {
		// THE UNIT DISK SPREAD OVER THE RECTANGLE — the same 110 places, stretched rather than
		// re-rolled, so the canvas and the perform surface share a sky rather than each having one.
		const float x = area.x() + (star.x + 1.0f) * 0.5f * area.width();
		const float y = area.y() + (star.y + 1.0f) * 0.5f * area.height();
		const float twinkle = 0.18f * std::sin(p * 1.2f + star.x * 9.0f + star.y * 7.0f);
		const float alpha = star.magnitude * (0.14f + 0.55f * kLuminance + twinkle);
		const float size = (star.magnitude > 0.75f ? 1.4f : 0.9f) * (1.0f + kLuminance * 1.1f);
		canvas.setColor(withAlpha(kInk, std::clamp(alpha, 0.0f, 1.0f)));
		canvas.fill(x, y, size, size);
	}
}

//--------------------------------------------------------------------------------------------------//
// The canvas.                                                                                      //
//--------------------------------------------------------------------------------------------------//

ExpandedCanvas::ExpandedCanvas(Edge edge) : edge_(edge), reducedMotion_(reducedMotionRequested()) {
	backdrop_ = std::make_unique<StarfieldBackdrop>();
	// THIS FRAME TAKES ITS OWN PRESSES, unlike the overlay's layer: it sits in a band the layout
	// reserved for it, so nothing of the editor is beneath it to reach. A panel floating over the band
	// is on the layers above this frame and takes its own presses first.
	setIgnoresMouseEvents(false, true);
	onMouseDown() = [this](const visage::MouseEvent& e) {
		// THE GRIP FIRST: it is the inboard edge's strip, where the toggle row also begins.
		if (!collapsed_ && gripBounds().contains(e.position)) {
			// A DOUBLE-CLICK RESETS THE PARTITION — the road back from a size a hand cannot undo by
			// dragging (see `kMaxShare`). Committed at once, like a release.
			if (e.repeatClickCount() >= 1) {
				draggingExtent_ = false;
				if (setOpenExtent(defaultExtent()) && onExtentCommit) {
					onExtentCommit();
				}
				return;
			}
			draggingExtent_ = true;
			// WINDOW PIXELS, because this frame is about to move under the hand — the panel's rule.
			dragOrigin_ = e.window_position;
			dragStartExtent_ = openExtent_;
			return;
		}
		if (toggleBounds().contains(e.position)) {
			toggleCollapsed();
		}
	};
	onMouseDrag() = [this](const visage::MouseEvent& e) {
		if (!draggingExtent_) {
			return;
		}
		// TOWARDS THE CHAIN GROWS THE CANVAS: up for a bottom band, down for a top band, left for a
		// right column — the partition follows the hand.
		const visage::Point delta = e.window_position - dragOrigin_;
		float moved = 0.0f;
		switch (edge_) {
		case Edge::Right: moved = -delta.x; break;
		case Edge::Bottom: moved = -delta.y; break;
		case Edge::Top: moved = delta.y; break;
		}
		// THE HAND'S LOGICAL DELTA FIRST — the whole answer for a host without a window (a case) — then
		// the host, which may put the partition under the pointer physically instead.
		const bool movedAtAll = setOpenExtent(dragStartExtent_ + moved);
		if (onExtentDrag) {
			onExtentDrag(e.window_position);
		}
		else if (movedAtAll) {
			redraw();
		}
	};
	onMouseUp() = [this](const visage::MouseEvent&) {
		if (!draggingExtent_) {
			return;
		}
		draggingExtent_ = false;
		if (onExtentCommit) {
			onExtentCommit();
		}
	};
	// NO KEYS: Escape is the overlay's, which takes the focus when a panel opens, and a panel is no
	// longer this band's to close.
}

ExpandedCanvas::~ExpandedCanvas() = default;

void ExpandedCanvas::setBackdrop(std::unique_ptr<CanvasBackdrop> backdrop) {
	backdrop_ = std::move(backdrop);
	redraw();
}

float ExpandedCanvas::extent() const noexcept {
	if (!isVisible()) {
		return 0.0f;
	}
	return collapsed_ ? Rail::kCollapsedWidth : openExtent_;
}

bool ExpandedCanvas::setOpenExtent(float extent) {
	const float clamped = std::clamp(extent, kMinExtent, maxExtent_);
	if (clamped == openExtent_) {
		return false;
	}
	openExtent_ = clamped;
	redraw();
	return true;
}

float ExpandedCanvas::partitionExtent(float base, float nativeSpan, float canvasNative, float preferredScale,
                                      float minExtent, float maxExtent) {
	if (nativeSpan <= 0.0f || preferredScale <= 0.0f || base <= 0.0f) {
		return minExtent;
	}
	const float canvas = std::clamp(canvasNative, 0.0f, nativeSpan - 1.0f);
	// SCALE-LIMITED: the instrument's side is `nativeSpan - canvas` device pixels for `base` rows, so the
	// scale is that quotient, and the canvas's rows are its pixels at that scale.
	const float limitedScale = (nativeSpan - canvas) / base;
	float extent = 0.0f;
	if (limitedScale <= preferredScale) {
		extent = canvas / limitedScale; // == base * canvas / (nativeSpan - canvas)
	}
	else {
		// FREE: the window has room at the preferred scale. The pointer's rows at that scale, and never
		// so many that the other side has fewer than `base`.
		extent = std::min(canvas / preferredScale, nativeSpan / preferredScale - base);
	}
	return std::clamp(std::round(extent), minExtent, maxExtent);
}

visage::Bounds ExpandedCanvas::gripBounds() const {
	switch (edge_) {
	case Edge::Right: return { 0.0f, 0.0f, kGripSize, height() };
	case Edge::Bottom: return { 0.0f, 0.0f, width(), kGripSize };
	case Edge::Top: return { 0.0f, std::max(0.0f, height() - kGripSize), width(), kGripSize };
	}
	return {};
}

bool ExpandedCanvas::setCollapsed(bool collapsed) {
	if (collapsed == collapsed_) {
		return false;
	}
	collapsed_ = collapsed;
	if (onCollapsedChanged) {
		onCollapsedChanged();
	}
	redraw();
	return true;
}

bool ExpandedCanvas::animateBackdrop() {
	if (!showing() || backdrop_ == nullptr || !backdrop_->animates() || reducedMotion_) {
		return false;
	}
	// ONE STEP PER TICK, and a frame asked for by the step — the notify-count rule: the count is
	// produced BY the work. The sky moves behind an open card too: the card floats on it.
	phase_ += 1.0 / 60.0;
	redraw();
	return true;
}

//--------------------------------------------------------------------------------------------------//
// Geometry.                                                                                        //
//--------------------------------------------------------------------------------------------------//

visage::Bounds ExpandedCanvas::toggleBounds() const {
	// THE INBOARD CORNER, both states — the rail's rule. A right-hand column's inboard edge is its
	// left; a band's toggle sits at its left end. Either way that is (pad, pad) here.
	const float inset = collapsed_ ? Rail::kCollapsedPadX : Rail::kCollapsedPadX * 2.0f;
	float y = Rail::kPadTop * 0.5f;
	if (collapsed_ && edge_ != Edge::Right) {
		// CENTRED IN THE BAND: a closed band is one strip tall, with nothing under the toggle.
		y = (Rail::kCollapsedWidth - Rail::kToggleSize) * 0.5f;
	}
	return { inset, y, Rail::kToggleSize, Rail::kToggleSize };
}

visage::Bounds ExpandedCanvas::contentBounds() const {
	if (collapsed_) {
		return { 0.0f, 0.0f, 0.0f, 0.0f };
	}
	// UNDER THE TOGGLE ROW, ABOVE THE GRIP if the grip is at the foot: the floor a panel may occupy.
	const float foot = edge_ == Edge::Top ? kGripSize : 0.0f;
	return { kPad, kTitleHeight, std::max(0.0f, width() - 2.0f * kPad),
		     std::max(0.0f, height() - kTitleHeight - kPad - foot) };
}

void ExpandedCanvas::resized() {
	// NO CHILDREN: the panels float on the layers above, and read the floor when they arrive.
	redraw();
}

//--------------------------------------------------------------------------------------------------//
// Drawing.                                                                                         //
//--------------------------------------------------------------------------------------------------//

void ExpandedCanvas::draw(visage::Canvas& canvas) {
	canvas.setColor(kSurface);
	canvas.fill(0, 0, width(), height());
	// THE RULE ALONG THE INBOARD EDGE, so the canvas reads as a region beside the chain and not as
	// more of it.
	canvas.setColor(withAlpha(kLine, 0.9f));
	switch (edge_) {
	case Edge::Right: canvas.fill(0.0f, 0.0f, 1.0f, height()); break;
	case Edge::Bottom: canvas.fill(0.0f, 0.0f, width(), 1.0f); break;
	case Edge::Top: canvas.fill(0.0f, height() - 1.0f, width(), 1.0f); break;
	}
	// THE GRIP'S MARK: a short bar at the middle of the inboard edge, the sign every desktop reads as
	// "this partition moves". Brighter while it is being dragged.
	if (!collapsed_) {
		const visage::Bounds grip = gripBounds();
		canvas.setColor(withAlpha(kMuted, draggingExtent_ ? 0.9f : 0.5f));
		if (edge_ == Edge::Right) {
			canvas.fill(grip.x() + 2.0f, grip.y() + grip.height() * 0.5f - 18.0f, 2.0f, 36.0f);
		}
		else {
			canvas.fill(grip.x() + grip.width() * 0.5f - 18.0f, grip.y() + 2.0f, 36.0f, 2.0f);
		}
	}

	drawStrip(canvas);
	if (collapsed_) {
		return;
	}
	// THE SKY, UNDER WHATEVER FLOATS: a panel is on the layers above and draws over it.
	if (backdrop_ != nullptr) {
		backdrop_->draw(canvas, contentBounds(), phase_);
	}
}

void ExpandedCanvas::drawStrip(visage::Canvas& canvas) {
	drawChevrons(canvas);
	if (!collapsed_) {
		return;
	}
	// THE LABEL, which is the whole of what a closed canvas says. Upright down a column, along a
	// band — the rail's own size, because a strip has one word and the whole strip to say it in.
	canvas.setColor(kMuted);
	const visage::Bounds toggle = toggleBounds();
	if (edge_ == Edge::Right) {
		const float top = toggle.bottom() + Rail::kPadTop;
		canvas.text("CANVAS", font(Rail::kCollapsedLabelSize), visage::Font::kRight, 0.0f, top, width(),
		            height() - top, visage::Direction::Left);
	}
	else {
		const float left = toggle.right() + Rail::kPadTop;
		canvas.text("CANVAS", font(Rail::kCollapsedLabelSize), visage::Font::kLeft, left, 0.0f,
		            width() - left, height());
	}
}

void ExpandedCanvas::drawChevrons(visage::Canvas& canvas) const {
	// THE RAIL'S DOUBLE CHEVRON, pointing the way the canvas will travel: outboard to close it,
	// inboard to bring it back. Right edge: outboard is +x. Bottom edge: +y. Top edge: -y.
	const visage::Bounds toggle = toggleBounds();
	const bool outboard = !collapsed_;
	const float cx = toggle.x() + toggle.width() * 0.5f;
	const float cy = toggle.y() + toggle.height() * 0.5f;
	const float half = toggle.height() * 0.22f;
	const float step = toggle.width() * 0.20f;
	const float outSign = edge_ == Edge::Top ? -1.0f : 1.0f;
	const float dir = (outboard ? 1.0f : -1.0f) * outSign;
	canvas.setColor(kMuted);
	for (int chevron = 0; chevron < 2; ++chevron) {
		const float along = (static_cast<float>(chevron) - 0.5f) * step * dir + step * 0.5f * dir;
		if (edge_ == Edge::Right) {
			const float tipX = cx + along;
			const float tailX = tipX - step * dir;
			canvas.segment(tipX, cy, tailX, cy - half, Rail::kChevronWeight, true);
			canvas.segment(tipX, cy, tailX, cy + half, Rail::kChevronWeight, true);
		}
		else {
			const float tipY = cy + along;
			const float tailY = tipY - step * dir;
			canvas.segment(cx, tipY, cx - half, tailY, Rail::kChevronWeight, true);
			canvas.segment(cx, tipY, cx + half, tailY, Rail::kChevronWeight, true);
		}
	}
}

} // namespace feathers::ui
