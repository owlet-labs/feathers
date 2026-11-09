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

#include "plugin/params.h"

#include <array>
#include <cstdint>

// **THE STACK INDEX — one value per part of a stacked voice, −1..+1** (backlog KE step 3; the voice-stacking doc
// §3.2). The firmware's `Sound::setupUnisonIndexSpread` (sound.cpp:3315-3607, doc §1.3) in three stages: a SHAPE
// gives each part a value in part order, a CURVE warps every value by a signed power, and a MAPPING decides which
// part gets which. Pure arithmetic over the settings, so the synth, the Engine rail's plot and a parity case all
// call the same code.
//
// **THE FIRMWARE'S QUIRKS ARE FIXED**:
// - **No sort.** The firmware sorted the shaped values before permuting them, so only their SET survived:
//   Triangle's peak and Sine's cycles changed which values existed, never which part got which. The mapping here
//   permutes the values IN PART ORDER, and "As shaped" (the identity) is the first mapping. On Linear, Power,
//   S-Curve and Step, whose values already ascend with the part, the sort was the identity, so they come out as
//   the firmware's did — which is what the parity case pins.
// - **Drift walks around a base and keeps it.** Its base is the Linear fan through the Curve and the Mapping,
//   and the walk (`stackDriftStep`) is an offset AROUND it, so its mean is the base rather than 0.
// - **Random is one fixed draw**; the amount is its depth, so moving the amount no longer redraws. **Random
//   Fresh** draws anew at each note (the seed the caller hands in).
// - **Pairs uses the whole fan:** a group takes the mean of its members, where the firmware's `(u / g) · g` never
//   read the top value unless g divided N − 1.

namespace feathers {

using StackValues = std::array<float, kMaxStack>;

/// **THE SEED OF THE FIXED RANDOM DRAW** — the same in every patch (voice-stacking doc §7, assumption 17).
inline constexpr uint32_t kStackRandomSeed = 0;

struct StackIndexSettings {
	int count = 1;
	StackShape shape = StackShape::Linear;
	float shapeAmount = 0.0f; ///< the shape's meta, 0..1
	double freePhase = 0.0;   ///< Free's position along the field
	StackMap map = StackMap::AsShaped;
	float mapAmount = 0.0f;   ///< the mapping's meta, 0..1
	float curve = 0.0f;       ///< −1..+1, the firmware's −50..+50
	uint32_t freshSeed = 0;   ///< Random Fresh's draw, per note
};

/// Stage one, in part order. Drift answers its base, the plain Linear fan.
StackValues stackShapeValues(StackShape shape, float amount, double freePhase, uint32_t freshSeed, int count);
/// Stage two, in place: the firmware's signed power, exponent 1 + 3c for c > 0 and 1 / (1 + 3|c|) below.
void stackCurveWarp(float curve, int count, StackValues& values);
/// Stage three: which part gets which of `shaped`.
StackValues stackMapValues(StackMap map, float amount, const StackValues& shaped, int count);
/// All three. Parts at and past `count` are 0, and a stack of one is all 0: one part is the centre.
StackValues stackIndexSpread(const StackIndexSettings& settings);

/// **DRIFT'S WALK, ONE STEP OF `samples`** — the firmware's mean-reverting walk (`tickUnisonIndexDrift`), rate
/// `a² · 2·10⁷ + 5·10⁴` and decay rate / 8 in q31 per 128-sample block, restated in float and scaled to the step:
/// the noise by √(samples / 128), the decay by samples / 128. `offsets` is what the synth adds to the base.
void stackDriftStep(float amount, int count, int samples, uint32_t& rng, StackValues& offsets);

/// The firmware's hash of a part and a seed onto −1..+1 (its Random zone), for Random and Random Fresh.
float stackHash(int part, uint32_t seed);

} // namespace feathers
