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

#include <cstddef>
#include <span>

// The constellation: where the sounds are, where the puck is, and how much of each you hear.
//
// PURE GEOMETRY, deliberately. Nothing here knows what a sound sounds like, what a parameter is, or
// which slot an anchor came from — it takes points and returns weights. That is what lets the whole
// of it be tested by arithmetic, and it is why the caller passes only the ENABLED anchors: "is this
// sound switched on" is a question about the patch, and answering it here would drag the parameter
// table into a file that should never have heard of it.

namespace feathers {

/// How many sounds the constellation holds. A design decision, and it is the number the mock's palette,
/// the voice manager's limit and the CPU budget were all already pointing at.
///
/// It lives HERE, with the geometry, rather than with the parameter table that spends ids on it: the
/// character layer sizes its sound array from it and must not have to know what a parameter is, and
/// two constants named for the same eight are two constants that can be changed one at a time.
inline constexpr size_t kMaxSounds = 8;

struct AnchorPoint {
	double x = 0.0;
	double y = 0.0;

	friend bool operator==(const AnchorPoint&, const AnchorPoint&) = default;
};

/// How local the blend is: weight falls as distance to the power of this.
///
/// THE DESIGN DOC SAYS INVERSE-SQUARE AND THIS IS ONE, and the difference is not a detail — it is the
/// only value that leaves an existing two-sound patch sounding exactly as it does today.
///
/// With two anchors and the puck on the segment between them, normalised inverse-distance weights
/// come out as `1 - t` and `t`: the LINEAR crossfade that Path has always been. That is a property of
/// this exponent alone. At inverse-square the same quarter-way point weights the near sound 0.9
/// instead of 0.75, so every patch with Path anywhere but its ends would change character.
///
/// Squared is the more usual choice and gives tighter local support with more sounds; this is a
/// v1-compatibility choice made while the question is open, not a claim that it is the better feel.
/// Marked here rather than buried so that changing it is a decision somebody takes on purpose.
inline constexpr double kBlendFalloffPower = 1.0;

/// Below this distance the puck is treated as being ON an anchor.
///
/// Not an optimisation: at zero distance the weight is a division by zero, and the limit it is
/// approaching is "all of this sound and none of any other". Coincident anchors share it evenly,
/// because there is no reason to prefer one and a tie broken by index would flip as a patch loaded.
inline constexpr double kAnchorSnapDistance = 1e-9;

/// Where the tour is at `path`.
///
/// The tour is the polyline through the anchors in order, walked at constant PARAMETER rather than
/// constant speed: each leg takes an equal share of `path`, so with two anchors `path` is exactly the
/// crossfade it has always been, and adding a third does not squeeze the first two into part of the
/// knob. Path is clamped, and an empty constellation returns the origin.
AnchorPoint tourPoint(std::span<const AnchorPoint> anchors, double path);

/// The share of each anchor at `puck`, written into `out` and summing to 1.
///
/// `out` must be at least as long as `anchors`. An empty constellation writes nothing — the caller
/// has no sounds to play and should be making silence rather than asking this.
///
/// `power` is the falloff, defaulting to the one value that leaves a two-sound patch alone. It is an
/// ARGUMENT rather than the constant read directly so that §7.1d's A/B can be heard without a
/// rebuild: the exponent is a question for an ear, and an ear cannot compare two builds. Nothing in
/// the shipping path passes anything but the default.
void blendWeights(std::span<const AnchorPoint> anchors, AnchorPoint puck, std::span<double> out,
                  double power = kBlendFalloffPower);

} // namespace feathers
