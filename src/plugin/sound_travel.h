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

#include "dsp/constellation.h"

#include <algorithm>
#include <array>
#include <cmath>

// WHERE EACH SOUND HAS DRIFTED TO — the state of record for stage 3's second verb.
//
// DRIFT IS IDENTITY-LEVEL, by design (assumption A5). A sound carries its own offset across the slice
// plane, stored with the patch and added into the decode beside the travel the puck contributes. So
// drifting a sound changes what that sound IS, everywhere and for as long as the patch lasts, while
// moving the puck changes only where you are standing to listen to it. The two are added rather than
// mixed because they answer different questions and a player has to be able to undo one without
// touching the other.
//
// NOT A PARAMETER, and not for the reason the audition handles are not. These are permanent patch
// state — they have to survive a save, they belong in the stream — but there are sixteen of them and
// they are scrubbed by a gesture rather than automated, so spending sixteen ids on them now would be
// spending them before the Explore surface has told us what shape it wants. They travel in the tagged
// trailing block beside the orientation, which is the same argument that put the orientation there.
//
// DEFAULT EXACTLY ZERO, and the zero is load-bearing: a patch that has never drifted must render what
// it rendered before this existed, to the bit. That is the migration control, not a hope — see the
// zero-offset case in tests/test_character_blend.cpp.

namespace feathers {

/// **A SOUND'S OWN POLAR OFFSET FROM WHERE IT WAS PUT — the Explore chart's Radius and Theta corners.**
///
/// INDEPENDENT OF `SoundTravel::offset`, by design. The star is where the sound was PUT; this is how far from there it
/// is PLAYED and in which direction. So dragging the star never moves these, turning these never
/// moves the star, and a ramp into Theta orbits the sound AROUND its own place — not around the
/// plane's origin, where a sound that had never been moved had radius zero and could not orbit at all.
///
/// DEFAULT EXACTLY ZERO, load-bearing for the reason `offset`'s zero is: see `pulledPlace`.
struct PolarOffset {
	double radius = 0.0; ///< travel units, never negative
	double theta = 0.0;  ///< radians; stored in (-pi, pi], but only ever consumed by cos/sin

	friend bool operator==(const PolarOffset&, const PolarOffset&) = default;
};

struct SoundTravel {
	/// Per slot, in the same units Pos X/Y are: the constellation plane, ±1 across the disk.
	std::array<AnchorPoint, kMaxSounds> offset{};
	/// Per slot, the polar offset added to `offset` to give where the sound is played.
	std::array<PolarOffset, kMaxSounds> orbit{};

	friend bool operator==(const SoundTravel&, const SoundTravel&) = default;
};

/// One sound drifted further, cumulatively — the second verb, in the shape rotate has.
///
/// PURE, AND ON THE STATE OF RECORD, for the same reason rotateBy is: the encoder and the drag both
/// send increments, and a stored offset that accumulates them can be rewound by restoring the number
/// rather than by subtracting the increments back off. `delta` is in the plane's own travel
/// coordinates, which is what "along the current orientation" means — the basis decides where those
/// coordinates lead, so drifting by (1, 0) goes wherever the plane currently aims its first axis, and
/// re-aiming the plane afterwards changes where the sound has ended up.
///
/// A slot past the end is ignored rather than clamped: there is no sound there to drift, and clamping
/// would silently drift a different one.
inline SoundTravel driftBy(const SoundTravel& from, size_t slot, AnchorPoint delta) {
	SoundTravel drifted = from;
	if (slot < kMaxSounds) {
		drifted.offset[slot] = { from.offset[slot].x + delta.x, from.offset[slot].y + delta.y };
	}
	return drifted;
}

/// WHAT MODULATION IS ADDING TO EACH SOUND'S PLACE, in the polar form it is added in: Radius in travel
/// units, Theta in radians and UNWRAPPED (see `pulledPlace`). Zero everywhere when nothing is routed,
/// which is also what a bridge nobody has published to reads — so "no engine" and "no modulation"
/// draw the same thing, the authored place.
struct TravelPulls {
	std::array<double, kMaxSounds> radius{};
	std::array<double, kMaxSounds> theta{};

	friend bool operator==(const TravelPulls&, const TravelPulls&) = default;
};

/// **HOW FAR AN ORBIT CAN REACH: THE WHOLE PLANE, TWO TRAVEL UNITS** (backlog PO). The constellation plane is ±1 across, so a sound standing at one edge and played this far
/// from its place reaches the other edge. It is the top of the Radius FACE (`kTravelRadiusMax`), so the knob and a
/// full-depth route from its zero both reach it.
inline constexpr double kTravelRadiusReach = 2.0;

/// **THE FAR END OF THE RADIUS FACE**, in travel units — the knob's top and the ceiling of every route into it
/// (radius-mod-jump, 2026-11-07: routes move along the face, so the face's range IS the route's range, and the
/// whole plane has to be on it for a full swing to reach it). It was 1, with routes reaching 2 off the face's end.
inline constexpr double kTravelRadiusMax = kTravelRadiusReach;

/// **HOW CURVED THE RADIUS FACE IS.** `radius = max * (e^(k p) - 1) / (e^k - 1)` for a face position p in 0..1:
/// a real zero at the bottom, the plane's whole reach at the top. At k = 8.4 the MIDDLE of the throw is 0.0296 —
/// one disc radius of the chart at x1, the 0.029 that k = 7 gave when the top was 1 — so half the knob is spent on
/// offsets that stay on the chart and the other half reaches everything else.. NOT SET BY EAR YET; it is one number.
inline constexpr double kRadiusCurve = 8.4;

/// **WHERE A RADIUS SITS ON THE FACE, 0..1** — the knob's pointer, and the coordinate a route adds in. Floored at
/// the centre (and a NaN reads as the centre), capped at the face's top.
inline double radiusFace(double radius) noexcept {
	if (!(radius > 0.0)) {
		return 0.0;
	}
	const double unit = std::min(radius / kTravelRadiusMax, 1.0);
	return std::log1p(unit * std::expm1(kRadiusCurve)) / kRadiusCurve;
}

/// **THE RADIUS AT A FACE POSITION** — `radiusFace`'s inverse: exactly 0 at 0 and exactly `kTravelRadiusMax` at 1.
inline double radiusAtFace(double face) noexcept {
	if (!(face > 0.0)) {
		return 0.0;
	}
	const double p = std::min(face, 1.0);
	return kTravelRadiusMax * std::expm1(kRadiusCurve * p) / std::expm1(kRadiusCurve);
}

/// **THE FACE POSITION A SOUND IS PLAYED AT: its orbit's face position plus what modulation adds, IN FACE UNITS,
/// kept on the face** (radius-mod-jump). A route's amount is a fraction of the FACE —
/// 1% depth moves the knob's mark 1% of its throw, wherever the knob sits — rather than a fraction of the radius,
/// which on an exponential face put a 1% route at 45% of the throw from zero. A bipolar source moves the face both
/// ways, symmetrically. The floor parks on the sound's own place; the top HOLDS (a clamp, as option A was
/// stated), since the top is already the whole plane.
///
/// THE ONE MAPPING: the engine (`playedRadius`), the knob's displacement mark and its modulation ring
/// (`TravelControl`), and the chart's star all read it; a relative macro turning the knob steps along the same face.
inline double playedRadiusFace(double orbitRadius, double facePull) noexcept {
	const double face = radiusFace(orbitRadius) + facePull;
	if (!(face > 0.0)) {
		return 0.0;
	}
	return std::min(face, 1.0);
}

/// **THE RADIUS A SOUND IS PLAYED AT** — `playedRadiusFace` put back into travel units. With NOTHING routed (a pull
/// of exactly zero) the orbit's own radius is returned BIT FOR BIT rather than round-tripped through the curve, so
/// an unmodulated patch renders exactly what it did.
inline double playedRadius(double orbitRadius, double facePull) noexcept {
	if (facePull == 0.0) {
		return orbitRadius > 0.0 ? std::min(orbitRadius, kTravelRadiusMax) : 0.0;
	}
	return radiusAtFace(playedRadiusFace(orbitRadius, facePull));
}

/// **WHERE A SOUND IS PLAYED: where it was put, plus its polar offset, plus what modulation is adding
/// to that offset. ONE FUNCTION, TWO CALLERS** — the engine (`Synth::updateCharacter`) and the Explore
/// chart, which draws the star where the engine put the sound.
/// Shared rather than restated, because a chart that recomputed this by its own formula would be a
/// second opinion about where the sound is.
///
/// A TOTAL RADIUS OF ZERO RETURNS THE AUTHORED PAIR BIT FOR BIT. The zero is load-bearing — a patch
/// that has never used these renders what it always rendered — and at zero radius the angle has
/// nothing to turn, so it is not consulted at all.
///
/// THE ANGLE IS CONSUMED ONLY BY cos/sin, AND THAT IS THE WHOLE OF THE WRAP HANDLING: they are
/// periodic, so a ramp that resets is positionally continuous and there is nothing to wrap.
/// THE RADIUS HAS A FLOOR AND A CEILING — see `playedRadius`.
inline AnchorPoint pulledPlace(AnchorPoint authored, PolarOffset orbit, double radiusPull,
                               double thetaPull) {
	const double radius = playedRadius(orbit.radius, radiusPull);
	if (!(radius > 0.0)) {
		return authored;
	}
	const double theta = orbit.theta + thetaPull;
	return { authored.x + radius * std::cos(theta), authored.y + radius * std::sin(theta) };
}

/// **EVERY SOUND'S PLAYED PLACE AT ONCE, as the `SoundTravel` `characterFromParams` takes** — offsets
/// resolved through `pulledPlace`, orbits spent (zeroed), so a consumer that knows only `offset` reads
/// the right thing. The engine and the SCOPE both build their character from this: the scope is on the
/// played road (wave_display.cpp: *"every channel the engine reads, and the list is not optional"*),
/// and one drawing the authored place beside an engine playing the offset one is backlog G1 again.
inline SoundTravel playedTravel(const SoundTravel& authored, const TravelPulls& pulls) {
	SoundTravel played;
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		played.offset[slot] = pulledPlace(authored.offset[slot], authored.orbit[slot],
		                                  pulls.radius[slot], pulls.theta[slot]);
	}
	return played;
}

} // namespace feathers
