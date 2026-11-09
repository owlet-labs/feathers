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

#include "fx/automod/automod_activity.h"
#include "fx/automod/automod_derived.h"

#include <algorithm>
#include <cmath>

namespace feathers::ui::automod_terrain {

/// **HOW MUCH OF AUTOMOD'S MACHINERY IS ENGAGED AT A POSITION — THE FIELD, DECODED.**
///
/// BY DESIGN the terrain need not be rendered previews; it can be decoded from whatever is to hand.
/// So this decodes the DERIVE rather than rendering audio: `deriveAutomod` at the
/// position, and a sum of the things that make the device audible there.
///
/// WHAT IS SUMMED, and why these: the comb's mix and feedback, the SVF's feedback, the filter's mod
/// depth, and the magnitude of the three band responses. Between them the two axes are both
/// represented — Type drives the comb and the filter mix through a 3-bank, Flavour drives the band
/// responses and phase offsets through a 9-bank — so a map made from these varies along BOTH axes,
/// which a map of Type's terms alone would not.
///
/// **CLAMPED AT ZERO BECAUSE THE RAW SUM GOES NEGATIVE.** MEASURED over an 8x8-zone sweep: the raw
/// sum spans -0.206 to 5.366. Some of the terms are signed — a band response is a response, not an
/// amount — and a brightness map has no way to draw less than nothing.
///
/// **DEFINED IN `fx/automod/automod_activity.h` SINCE 2026-11-07** and re-exported here, so the pad and
/// the Twin Field modulator read the terrain through ONE function, not two copies.
using fx::automod::rawActivityOf;

/// THE SAME READING AT A POINT OF THE COORDINATE SQUARE — Type across, Flavour up, Mod at zero. This
/// is the rank-1 home and nothing else: no standing travel, no gamma, no plane. It is what the
/// structure case samples to assert the field's distribution, and what the pad's terrain USED to
/// sample, which is why the pad could not follow the puck — see `AutomodPad::rebuildMap`.
[[nodiscard]] inline float rawActivityAt(float type, float flavour) {
	fx::automod::AutomodCoords coords {};
	coords.type = type;
	coords.flavor = flavour;
	coords.mod = 0.0f;
	// A NOMINAL RATE. The derive needs one — the spring frequencies scale with it — and the terms
	// summed above do not depend on it, so the map does not have to be rebuilt when Rate moves.
	coords.lfoHz = 1.0f;
	return rawActivityOf(fx::automod::deriveAutomod(coords));
}

/// **THE NORMALISER, MEASURED RATHER THAN CHOSEN.** Twice the field's own mean over an 8x8-zone
/// sweep. The candidates and what each does to the picture, measured on a 160x160 grid:
///
///     normaliser        dark (<0.2)    bright (>0.8)
///     5.366 (the max)      20.1%           0.18%      <- nearly nothing reaches full
///     4.025 (0.75 max)     11.5%           4.0%
///     3.556 (2x mean)       9.2%           9.9%       <- this
///
/// **A MAP NORMALISED BY ITS MAXIMUM IS ALMOST ALL FLOOR**, because one rare corner of the field sets
/// the scale for the whole picture. Two-times-mean puts roughly a tenth of the plane at each end,
/// which is what lets a reader see quiet country AND busy country instead of a dim wash with one
/// bright speck. Same purpose the shaper's map has: *"dark where the half-rectified triangles have
/// all gone quiet, bright where several bases are blending."*
using fx::automod::kActivityFull; // 3.556f, defined beside `rawActivityOf`

/// 0 (nothing happening here) to 1 (everything happening here), for a reading the caller already
/// derived — the pad's terrain derives at a point of the PLANE and hands the result here.
using fx::automod::activityOf;

/// 0 (nothing happening here) to 1 (everything happening here), at a point of the coordinate square.
[[nodiscard]] inline float activityAt(float type, float flavour) {
	return std::clamp(rawActivityAt(type, flavour) / kActivityFull, 0.0f, 1.0f);
}

/// **HOW MANY NAMED ZONES SPAN A COORDINATE'S RANGE — A GRID COUNT, NOT A DOMAIN MULTIPLIER.**
///
/// **THE FIRST VERSION OF THIS FILE GOT THAT WRONG AND THE PAD INHERITED IT.** `automod_types.h:43`
/// says the coordinates *"name regions of an infinite line"*, which was misread as the COORDINATE being
/// unbounded. It is not: `Type` is declared **0..1** with eight zones, and the unbounded quantity is
/// `Type Phase` beside it. So a map sampled at `activityAt(u * 8, ...)` covered eight LAPS of the
/// field where the control can only reach one — a picture of country no drag can visit.
///
/// Caught by the pad's own drag case reading `0.7` where it wanted `1.0`: the coordinate saturated at
/// its declared maximum, which is what a range of 0..1 does when you carry it by 1.0.
///
/// **THE NORMALISER SURVIVED THE CORRECTION**, which is worth recording rather than quietly
/// re-measuring: over the true 0..1 range the field reads min 0.000, max 4.419, mean 1.781, and
/// 2x mean = 3.561 against the 3.556 measured over eight laps — 9.8% dark and 10.5% bright, against
/// 9.2% and 9.9%. The field's statistics over one lap match its statistics over eight, so the
/// constant was right for a reason that had nothing to do with the mistake.
inline constexpr int kZonesShown = 8;

} // namespace feathers::ui::automod_terrain
