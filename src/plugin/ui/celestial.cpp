/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 *
 * --- Additional terms under GNU GPL version 3 section 7 ---
 * This file requires preservation of the above copyright notice and author attribution
 * in all copies or substantial portions of this file.
 */

#include "plugin/ui/celestial.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <string>

#include "plugin/ui/theme.h"

#include <visage/graphics.h>

#include <cmath>

namespace feathers::ui {

bool reducedMotionRequested() {
	// Present and not "0" is on. A plugin window cannot ask the platform this — the mock reads a CSS
	// media query it has no equivalent of here — so the environment is the only channel, and it is the
	// same one a user already has for FEATHERS_UI_SCALE.
	const char* text = std::getenv("FEATHERS_REDUCED_MOTION");
	return text != nullptr && text[0] != '\0' && std::string(text) != "0";
}


namespace {

constexpr float kTau = 6.28318530717958647692f;

} // namespace

const std::array<SkyStar, 110>& performSky() {
	static const std::array<SkyStar, 110> stars = [] {
		std::array<SkyStar, 110> built {};
		MockRandom random(55);
		for (SkyStar& star : built) {
			const float angle = random() * kTau;
			const float radius = std::sqrt(random()) * 0.97f;
			star.x = std::cos(angle) * radius;
			star.y = std::sin(angle) * radius;
			star.magnitude = 0.3f + random() * 0.7f;
		}
		return built;
	}();
	return stars;
}

bool StageDisk::contains(visage::Point pixels) const {
	const visage::Point c = centre();
	return std::hypot(pixels.x - c.x, pixels.y - c.y) <= radius();
}

double Bubble::glassRadius(double world) {
	// PAST THE RIM THE SHEET IS FLAT AGAIN, and that is the original model of this surface
	// taken literally: *"terrain is a flat sheet with a hemispherical bubble in it"*. The bubble is a
	// lens over the disc; outside it there is no lens, so the map is the identity and the two meet at
	// the rim, where sin(π/2) = 1.
	//
	// THE FIRST DRAFT SATURATED HERE, AND IT COST REACH. Clamping every radius past 1 onto the rim is
	// right for the GROUND — a sphere does not show you its far side, and `sin` turning back on itself
	// would paint the next country over the near face, mirrored — but the same function also answers
	// what a POINTER MEANS, and a pointer dragged outside the disc is how you travel a long way. With
	// the clamp, dragging out past the rim asked for the rim and nothing further, so a pan that used
	// to fly crawled along at whatever the edge-follow granted. It was felt within minutes of the
	// install: click-to-pan did not go far enough.
	//
	// The ground keeps its clamp, because the ground is drawn by the shader and the shader only ever
	// samples inside the disc. Nothing is drawn out here; the disc is the whole viewport.
	if (!(world > 0.0)) {
		return 0.0;
	}
	if (world >= 1.0) {
		return world;
	}
	return std::sin(world * kQuarterTurn);
}

double Bubble::worldRadius(double glass) {
	if (!(glass > 0.0)) {
		return 0.0;
	}
	if (glass >= 1.0) {
		return glass;
	}
	return std::asin(glass) / kQuarterTurn;
}

namespace {

/// The shared half of both directions: rescale a point's distance from the disk's middle by `map`,
/// keeping its bearing. The middle is its own image under any radial map and has no bearing to keep,
/// so it is answered directly rather than divided by zero.
visage::Point remapRadius(const StageDisk& disk, visage::Point pixels, double (*map)(double)) {
	const visage::Point unit = disk.toNormalized(pixels);
	const double r = std::hypot(static_cast<double>(unit.x), static_cast<double>(unit.y));
	if (r <= 1e-9) {
		return disk.toPixels(0.0f, 0.0f);
	}
	const double scale = map(r) / r;
	return disk.toPixels(static_cast<float>(unit.x * scale), static_cast<float>(unit.y * scale));
}

}  // namespace

visage::Point Bubble::toGlass(const StageDisk& disk, visage::Point flat) {
	return remapRadius(disk, flat, &Bubble::glassRadius);
}

visage::Point Bubble::toFlat(const StageDisk& disk, visage::Point glass) {
	return remapRadius(disk, glass, &Bubble::worldRadius);
}

void starGlyph(visage::Canvas& canvas, float x, float y, float size, uint32_t hue, float alpha,
               float haloScale) {
	// THE HALO ALONE TAKES THE SCALE — see the header. Floored at zero rather than clamped to 1,
	// because a caller saying "no glow" is a legitimate thing to say and a negative radius is not.
	const float halo = size * 3.0f * std::max(haloScale, 0.0f);
	canvas.setColor(visage::Brush::radial(visage::Gradient(visage::Color(withAlpha(hue, alpha * 0.32f)),
	                                                       visage::Color(withAlpha(hue, 0.0f))),
	                                      { x, y }, halo));
	canvas.circle(x - halo, y - halo, halo * 2.0f);

	canvas.setColor(withAlpha(hue, alpha * 0.85f));
	const float arm = size * 1.9f;
	canvas.segment(x - arm, y, x + arm, y, 1.0f, false);
	canvas.segment(x, y - arm, x, y + arm, 1.0f, false);

	const float core = size * 0.55f;
	canvas.setColor(withAlpha(kPointer, alpha));
	canvas.circle(x - core, y - core, core * 2.0f);
	const float heart = size * 0.34f;
	canvas.setColor(withAlpha(hue, alpha));
	canvas.circle(x - heart, y - heart, heart * 2.0f);
}

int dashedLine(visage::Canvas& canvas, visage::Point from, visage::Point to, float on, float off,
               float thickness) {
	// A dash pattern that does not advance is an endless loop, and a caller computing `on` and `off`
	// from a scale can reach zero without meaning to. Refusing is better than hanging the editor's
	// one thread, and better than silently drawing a solid line that hides the mistake.
	if (on <= 0.0f || on + off <= 0.0f) {
		return 0;
	}
	// AND THE SAME COURTESY FOR THE COORDINATES, which this function did not extend until now while
	// `clipSegmentToDisk` twenty lines below always has. The asymmetry was the whole finding: one of
	// the two straight-line primitives on this surface checked its inputs and the other did not.
	//
	// It is a HANG this closes, not a misdrawn line — see the header. An infinite endpoint gives an
	// infinite length, so the loop below never ends, and its unit vector is inf/inf, so every one of
	// those endless iterations hands the renderer a NaN. A NaN endpoint, by contrast, already drew
	// nothing at all, because a loop condition against NaN is false on the first test.
	if (!std::isfinite(from.x) || !std::isfinite(from.y) || !std::isfinite(to.x)
	    || !std::isfinite(to.y)) {
		return 0;
	}
	const float dx = to.x - from.x;
	const float dy = to.y - from.y;
	const float length = std::hypot(dx, dy);
	if (length <= 0.0f) {
		return 0;
	}
	const float ux = dx / length;
	const float uy = dy / length;
	int drawn = 0;
	for (float travelled = 0.0f; travelled < length; travelled += on + off) {
		const float end = std::min(length, travelled + on);
		canvas.segment(from.x + ux * travelled, from.y + uy * travelled, from.x + ux * end,
		               from.y + uy * end, thickness, false);
		++drawn;
	}
	return drawn;
}

std::optional<std::pair<visage::Point, visage::Point>>
clipSegmentToDisk(const StageDisk& disk, visage::Point from, visage::Point to) {
	if (!std::isfinite(from.x) || !std::isfinite(from.y) || !std::isfinite(to.x)
	    || !std::isfinite(to.y)) {
		return std::nullopt;
	}
	const visage::Point centre = disk.centre();
	const float radius = disk.radius();
	if (radius <= 0.0f) {
		return std::nullopt;
	}

	// Where the line meets the circle: the two roots of |from + t·(to − from) − centre|² = r², kept
	// only where they overlap the segment's own 0..1. In doubles because these are the inputs it
	// exists for — at ×64 an endpoint is a coordinate of order 10⁵, and `b² − 4ac` in floats loses
	// the answer to cancellation for exactly the geometry that most needs clipping.
	const double dx = static_cast<double>(to.x) - static_cast<double>(from.x);
	const double dy = static_cast<double>(to.y) - static_cast<double>(from.y);
	const double fx = static_cast<double>(from.x) - static_cast<double>(centre.x);
	const double fy = static_cast<double>(from.y) - static_cast<double>(centre.y);
	const double a = dx * dx + dy * dy;
	if (a <= 0.0) {
		// A line of no length is a point, and a point is either in the disk or it is not.
		return disk.contains(from) ? std::optional { std::pair { from, to } } : std::nullopt;
	}
	const double b = 2.0 * (fx * dx + fy * dy);
	const double c = fx * fx + fy * fy - static_cast<double>(radius) * static_cast<double>(radius);
	const double discriminant = b * b - 4.0 * a * c;
	if (discriminant < 0.0) {
		return std::nullopt;
	}
	const double root = std::sqrt(discriminant);
	const double enters = std::max(0.0, (-b - root) / (2.0 * a));
	const double leaves = std::min(1.0, (-b + root) / (2.0 * a));
	if (leaves <= enters) {
		// The line passes the disk without entering it, or touches it at a single point.
		return std::nullopt;
	}
	const auto at = [&](double t) {
		return visage::Point { static_cast<float>(static_cast<double>(from.x) + dx * t),
			                   static_cast<float>(static_cast<double>(from.y) + dy * t) };
	};
	return std::pair { at(enters), at(leaves) };
}

std::optional<RingArc> clipRingToDisk(const StageDisk& disk, visage::Point centre, float radius) {
	const float aperture = disk.radius();
	if (!std::isfinite(centre.x) || !std::isfinite(centre.y) || !std::isfinite(radius)
	    || radius <= 0.0f || aperture <= 0.0f) {
		return std::nullopt;
	}
	const visage::Point middle = disk.centre();
	const float d = std::hypot(centre.x - middle.x, centre.y - middle.y);
	if (d + radius <= aperture) {
		return RingArc { 0.0f, 0.0f, true };
	}
	// The aperture swallowed by the ring, or the two concentric: nothing of it is visible, and the
	// arithmetic below would be dividing by a zero distance.
	if (radius >= d + aperture || d <= 0.0f) {
		return std::nullopt;
	}
	const float cosine = (d * d + radius * radius - aperture * aperture) / (2.0f * d * radius);
	if (cosine >= 1.0f) {
		return std::nullopt;
	}
	return RingArc { std::atan2(middle.y - centre.y, middle.x - centre.x),
		             std::acos(std::max(-1.0f, cosine)), false };
}

void InkBox::add(visage::Point at, float reach) {
	add(at.x - reach, at.y - reach, reach * 2.0f, reach * 2.0f);
}

void InkBox::add(visage::Point from, visage::Point to, float reach) {
	add(from, reach);
	add(to, reach);
}

void InkBox::add(float x, float y, float width, float height) {
	if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) || !std::isfinite(height)) {
		// A mark nobody can place is recorded as the widest possible box rather than skipped: an ink
		// record that quietly forgets the one mark whose coordinates went wrong is a check that passes
		// hardest exactly when it matters.
		left = top = -std::numeric_limits<float>::infinity();
		right = bottom = std::numeric_limits<float>::infinity();
		any = true;
		return;
	}
	if (!any) {
		left = x;
		top = y;
		right = x + width;
		bottom = y + height;
		any = true;
		return;
	}
	left = std::min(left, x);
	top = std::min(top, y);
	right = std::max(right, x + width);
	bottom = std::max(bottom, y + height);
}

bool InkBox::within(const InkBox& other, float slack) const {
	if (!any) {
		return true;
	}
	if (!other.any) {
		return false;
	}
	return left >= other.left - slack && top >= other.top - slack && right <= other.right + slack
	       && bottom <= other.bottom + slack;
}

InkBox diskBox(const StageDisk& disk) {
	InkBox box;
	box.add(disk.centre().x - disk.radius(), disk.centre().y - disk.radius(), disk.radius() * 2.0f,
	        disk.radius() * 2.0f);
	return box;
}

void dashedRing(visage::Canvas& canvas, float centreX, float centreY, float radius, float thickness,
                int dashes) {
	if (dashes <= 0 || radius <= 0.0f) {
		return;
	}
	const float box = radius * 2.0f;
	const float step = kTau / static_cast<float>(dashes);
	for (int i = 0; i < dashes; ++i) {
		canvas.arc(centreX - radius, centreY - radius, box, thickness, static_cast<float>(i) * step,
		           step * 0.25f, false);
	}
}

} // namespace feathers::ui
