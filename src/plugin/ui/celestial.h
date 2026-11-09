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

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <visage/ui.h>

// What both circular surfaces are made of.
//
// Perform and Explore are the same aperture seen twice: the same disk, the same star glyph, the same
// dashed vocabulary, and — this is the one that matters — the same random number generator. The
// mock's skies are procedural, so a second port of its PRNG would not be duplicated code so much as
// a SECOND SKY: same statistics, different stars, and no way to compare either against the spec.
// One port, here, and both surfaces are the mock's.

namespace feathers::ui {

/// The mock's own PRNG (`rng(seed)`), ported exactly.
///
/// `Math.imul` and `|0` are 32-bit wrapping multiply and add, which is what these unsigned
/// operations are; the division by 2^32 is the mock's. Verified against the mock's own output rather
/// than assumed — see the test, which pins the first values of seed 55 to the sky it draws.
class MockRandom {
public:
	explicit MockRandom(uint32_t seed) : state_(seed == 0 ? 1u : seed) {}

	float operator()() {
		state_ += 0x6D2B79F5u;
		uint32_t t = (state_ ^ (state_ >> 15)) * (1u | state_);
		t = (t + ((t ^ (t >> 7)) * (61u | t))) ^ t;
		return static_cast<float>(t ^ (t >> 14)) / 4294967296.0f;
	}

private:
	uint32_t state_;
};

/// The mock's `prefers-reduced-motion`, which a plugin window has no way to be asked about — so it is
/// an environment variable, read once when a surface is built.
///
/// SHARED BY BOTH SURFACES, and here rather than on one of them because it is one contract: no
/// ambient repaint while idle, and no motion trail. A second reading of the variable is a second
/// chance for one surface to keep moving after the other has stopped.
bool reducedMotionRequested();

/// One star of a fixed sky: a place in the unit disk and a brightness.
struct SkyStar {
	float x = 0.0f;
	float y = 0.0f;
	float magnitude = 0.0f;
};

/// The mock's `BSTARS`: the Perform circle's own sky, 110 stars on seed 55, uniform over the disk
/// (`sqrt(r)` is what makes it uniform rather than crowded at the centre).
///
/// Fixed IN THE CIRCLE rather than in identity space, which is the whole difference between the two
/// surfaces: this sky belongs to the instrument and turns where it is, while Explore's flies past
/// you and never repeats. Shared out of this header so a test can check the stars the surface
/// actually draws against the ones the mock draws, rather than against a second copy of the recipe.
const std::array<SkyStar, 110>& performSky();

/// The circle a surface is drawn in, and the arithmetic that gets in and out of it.
///
/// The mock draws both views into a square canvas with `C = s/2` and `R = s/2 - 8`, and quotes every
/// glyph size against that canvas being 520 across. Our stage frames are much wider than they are
/// tall, so the disk is the largest one that fits and is centred in what it is given. Shared because
/// two surfaces working out the same circle separately is two circles waiting to disagree.
class StageDisk {
public:
	StageDisk(float frameWidth, float frameHeight)
	    : centre_ { frameWidth * 0.5f, frameHeight * 0.5f },
	      side_(frameWidth < frameHeight ? frameWidth : frameHeight) {}

	/// A disk placed explicitly — for a surface whose chart must YIELD room to other controls
	/// (V16: Explore reserves the width column before sizing its square). The centre is wherever
	/// the caller keeps it — Explore keeps the frame's, so the circle stays concentric with
	/// Perform's across a surface switch — and the side is what the frame could afford.
	StageDisk(visage::Point centre, float side) : centre_(centre), side_(side) {}

	visage::Point centre() const { return centre_; }
	float radius() const { return side_ * 0.5f - kRimInset; }

	/// What to multiply the mock's glyph sizes by: it quotes them as `13*s/520`.
	float glyphScale() const { return side_ / kMockSize; }

	/// Unit-disk coordinates (x right, y down, centre at the origin) to this frame's pixels.
	visage::Point toPixels(float x, float y) const {
		const float r = radius();
		return { centre_.x + x * r, centre_.y + y * r };
	}

	/// And back. The result is in unit-disk coordinates, which is what the caller's own point type is
	/// made of — visage::Point is borrowed here as a pair of floats rather than as a position.
	visage::Point toNormalized(visage::Point pixels) const {
		const float r = radius();
		if (r <= 0.0f) {
			return { 0.0f, 0.0f };
		}
		return { (pixels.x - centre_.x) / r, (pixels.y - centre_.y) / r };
	}

	bool contains(visage::Point pixels) const;

	/// The mock's `R = s/2 - 8`, and the canvas its glyph sizes are quoted against.
	static constexpr float kRimInset = 8.0f;
	static constexpr float kMockSize = 520.0f;

private:
	visage::Point centre_;
	float side_ = 0.0f;
};

/// THE BUBBLE: the chart's flat plane read as the near face of a sphere, seen from far enough away
/// that the projection is orthographic (backlog FE: the terrain is a flat sheet with a hemispherical
/// bubble in it, and the goal is to make it look like a planet).
///
/// ONE RADIAL LAW, AND EVERYTHING ELSE FOLLOWS FROM IT. Wrap the flat plane onto the hemisphere so
/// that distance from the middle is preserved AS ARC — walking a zone north on the plane walks a zone
/// north over the surface, whichever zone it is — and then look at the ball. A surface point an angle
/// θ from the pole is drawn at radius sin θ, and its distance from the pole along the surface is θ.
/// Normalising both to the rim (θ = π/2 is the limb, and the edge of the world):
///
///     glass = sin(world · π/2)          world = (2/π) · asin(glass)
///
/// WHAT THAT COSTS, and it was decided on rather than discovered: the gain varies across the disc.
/// d(glass)/d(world) is π/2 in the middle and ZERO at the limb, so the same hand movement covers less
/// world near the rim than at the pole, and a pixel near the rim is worth more world than a pixel in
/// the middle. Weighed against the alternatives, the decision was to accept it and slow the
/// drag down (which is `kViewsPerSecond`, and a different mechanism — the cap is about what the
/// TERRAIN can answer for, not about this).
///
/// WHAT IT DOES NOT COST: any world. The map is onto [0,1] from [0,1], so the disc shows exactly the
/// country it showed flat, redistributed — the middle magnified, the rim squeezed into a thin annulus,
/// which is what foreshortening IS. Nothing new comes into view and nothing leaves, so pan, zoom and
/// every window the terrain builds are untouched by it.
///
/// AND PAST THE RIM IT IS THE IDENTITY, which is the model taken literally — a bubble IN a sheet, so
/// outside the bubble the sheet is flat. The two meet at the rim, where sin(π/2) = 1, continuous in
/// value though not in gain: inside, the gain falls to nothing at the limb; outside, it is the flat
/// plane's own. Nothing is DRAWN out there — the disc is the whole viewport and the ground's shader
/// samples only inside it — but a pointer can go there, and that is how you travel a long way.
/// Saturating instead (the first draft) is correct for the ground and quietly wrong for the hand: it
/// made a drag past the rim ask for the rim and nothing further. See `glassRadius` for the cost.
class Bubble {
public:
	/// Quarter turn: the limb's angle from the pole, and the constant both directions are scaled by.
	static constexpr double kQuarterTurn = 1.5707963267948966;

	/// Flat radius (in disc radii, 0 at the middle and 1 at the rim) to the radius it is DRAWN at.
	static double glassRadius(double world);

	/// And back: a radius on the glass to the flat radius standing under it.
	static double worldRadius(double glass);

	/// A point in the flat chart's pixels to where the bubble draws it. Both directions go through the
	/// disk's own normalisation, so the map is about the circle and not about the frame.
	static visage::Point toGlass(const StageDisk& disk, visage::Point flat);

	/// And back: a point on the glass to the flat chart pixel under it. This is the one hit-testing
	/// wants — a pointer lands on the glass, and what it MEANS is a place on the plane.
	static visage::Point toFlat(const StageDisk& disk, visage::Point glass);
};

/// The mock's `drawStarGlyph`: a halo, a cross glint, a white core and a coloured heart. A sound is a
/// star on both surfaces — the same glyph, so the same object reads as the same thing in both.
///
/// `haloScale` SCALES THE HALO ALONE, and defaults to the identity so the two stages that draw a
/// SOUND are untouched. It exists because automod's strip wanted a glow whose RADIUS carries a
/// number — and scaling `size` would have grown the arms, the core and the heart with it, which
/// reads as "the star got bigger" rather than "the glow got wider". Two different statements, and
/// only one of them was asked for.
void starGlyph(visage::Canvas& canvas, float x, float y, float size, uint32_t hue, float alpha,
               float haloScale = 1.0f);

/// The mock's `setLineDash([2,5])`, walked along a straight line. Returns how many dashes it drew, so
/// a refusal is a number a case can read rather than an absence it has to infer.
///
/// THREE WAYS TO ASK FOR SOMETHING THAT IS NOT A DASHED LINE, and each is refused for its own reason.
/// Non-positive `on`/`off` never advances, which is an endless loop. A zero-length line has no
/// direction to walk. And a NON-FINITE endpoint is the one that had no guard until a reported fan of
/// stray lines sent somebody looking — measured, because the three misbehave differently:
///
///   - a NaN endpoint draws NOTHING today. Every comparison against NaN is false, so the loop's own
///     condition ends it before the first dash. It cannot be the source of a fan, and saying so is
///     what keeps this guard honestly labelled.
///   - an INFINITE endpoint HANGS THE EDITOR. The length is infinite, so the loop never reaches its
///     end, and the unit vector is `inf/inf` — so every iteration submits NaN coordinates to the
///     renderer, forever, on the one thread the editor has. That is what this guard is really for.
///   - a FINITE BUT WRONG endpoint — a zeroed `from`, say — draws a perfectly well-formed dashed line
///     from the wrong place, which no finite check can catch. If a fan of dashes anchored at the
///     origin is ever photographed again, THAT is its shape, and the search belongs at the caller.
int dashedLine(visage::Canvas& canvas, visage::Point from, visage::Point to, float on, float off,
               float thickness);

/// A dashed ring, which visage has no notion of: `setLineDash` is a canvas idea. Arcs with gaps
/// between them, which is what a dash pattern is once it has been wrapped around a circle.
void dashedRing(visage::Canvas& canvas, float centreX, float centreY, float radius, float thickness,
                int dashes);

/// The part of a line that lies inside the disk, or nothing at all.
///
/// A surface that places its marks from IDENTITY distances has no bound on where a mark lands: at a
/// tight zoom the view spans a fraction of a zone, so a point a little way off in identity is
/// THOUSANDS of pixels off on screen, and a line drawn to it crosses the whole editor rather than
/// marking a chart. Clipping before submitting is what keeps the geometry the size of the picture it
/// belongs to — a scissor alone would still make the renderer carry a ten-thousand-pixel quad, and
/// would still count it against a batch that has a hard ceiling.
///
/// Both endpoints outside is not the same as nothing to draw: a line between two points on opposite
/// sides passes straight through, which is exactly the leg a trail leaving the view draws. Non-finite
/// endpoints answer "nothing", because a coordinate that is not a number cannot be said to be inside
/// anything, and it is the one input whose treatment in the renderer is undefined.
std::optional<std::pair<visage::Point, visage::Point>>
clipSegmentToDisk(const StageDisk& disk, visage::Point from, visage::Point to);

/// The part of a RING that lies inside the disk, as an arc — or nothing at all.
///
/// The ripples had this first, worked out where they were drawn, and the puck needs exactly the same
/// answer: visage has no clip path, so a circle crossing the rim has to be reduced to the span of it
/// that is still inside. Circle-circle intersection — with the ring's centre `d` from the middle of
/// the disk, the arc still inside spans ±acos((d² + r² − R²) / 2dr) about the direction pointing back
/// at the centre.
///
/// Hoisted rather than copied when the puck's marks came to be clipped too,
/// because two implementations of one piece of geometry is how two surfaces come to disagree about
/// where their own rim is.
struct RingArc {
	float middle = 0.0f; ///< the middle of the visible span, in radians
	float half = 0.0f;   ///< and how far it reaches either side
	bool whole = false;  ///< the ring is entirely inside, so draw it as a ring
};
std::optional<RingArc> clipRingToDisk(const StageDisk& disk, visage::Point centre, float radius);

/// Where the ink of one frame went: the box every mark fell inside.
///
/// READABLE BECAUSE THE SYMPTOM IS OUTSIDE THE WIDGET. Overlay marks are placed from distances the
/// view does not bound — an identity on Explore, the puck's residual on Perform — so they can land
/// arbitrarily far from the disk, and both surfaces have drawn them there. A count cannot see that
/// and a screenshot cannot assert it, so the drawing records where its ink went, at the site that
/// submits it: a bound checked somewhere other than where it is enforced is a bound nobody is
/// enforcing.
///
/// Explore had this first and still owns the strictest use of it. It lives here because Perform's
/// puck now answers the same question, and a second copy would be a second definition of "inside".
struct InkBox {
	float left = 0.0f;
	float top = 0.0f;
	float right = 0.0f;
	float bottom = 0.0f;
	bool any = false;

	void add(visage::Point at, float reach);
	void add(visage::Point from, visage::Point to, float reach);
	void add(float x, float y, float width, float height);

	/// Whether this box is inside `other`, to a tolerance — nothing to draw is inside anything.
	bool within(const InkBox& other, float slack = 0.0f) const;
};

/// The box a disk's marks are allowed to fall inside: its own square. The aperture is a circle and
/// the clipping is to the circle, so this is the weaker of the two statements — but it is the one a
/// rectangle can hold, and it is the statement the bugs broke.
InkBox diskBox(const StageDisk& disk);

} // namespace feathers::ui
