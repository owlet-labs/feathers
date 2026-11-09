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

#include "dsp/blue_shift.h"
#include "dsp/constellation.h"
#include "character/phi_morph.hpp"

#include <span>

// The plugin's own renderer for the PhiMorph character layer.
//
// The firmware builds the wavetable (buildPhiMorphWavetable) and then rendered it in Q31 through a
// fixed-point renderer (since removed) and evaluateWaveformDirect. We keep the build — that is the
// character, and it is pinned by the golden test — and replace the render with float evaluation at
// arbitrary phase resolution.

namespace feathers {

/// Evaluate one wavetable at `phase01`, normalised so full scale is ±1.0.
///
/// Phase wraps, so any real value is legal. The character layer builds waypoints against
/// kPhiMorphRefAmplitude — roughly half of Q31 full scale — so a nominal waypoint sits near ±0.5 and
/// only a gain-driven zone reaches the ±1.0 rail, where it flat-tops exactly as the hardware clipper
/// does. The clip is part of evaluation, not something a caller adds afterwards.
///
/// The flat top holds its sign on every host only because buildSegmentsFromWaypoints was fixed to
/// clamp in double: its positive rail used to be a float literal one past INT32_MAX, so on x86 a
/// clipped waypoint wrapped and the zone dived instead of flat-topping. The suite pins the sign now.
float evalMorphTable(const owlet::dsp::PhiMorphWavetable& table, double phase01);

/// How hard the morph is being driven: 0 parked at either bank, 1 at the midpoint.
///
/// The engine's own quantity — it scales amplitude overshoot, curvature boost and phase distortion in
/// buildMorphXfade — and the editor draws it, so it lives here rather than being written out twice.
constexpr float morphExcite(float xf01) {
	const float cf = xf01 < 0.0f ? 0.0f : (xf01 > 1.0f ? 1.0f : xf01);
	return cf * (1.0f - cf) * 4.0f;
}

/// The same quantity for a constellation: HOW SPREAD THE WEIGHTS ARE, `2·(1 − Σw²)`.
///
/// 0 when one sound has all of the blend — standing on an anchor, where the character must be exactly
/// that sound's — and greatest when the sounds share evenly. At two sounds it is algebraically
/// `4t(1−t)`, which is morphExcite, so the shape of the morph is the function it has always been now
/// asking "how mixed is this" instead of "how far along a fade".
///
/// Here rather than inside the blend for the reason morphExcite is here: the editor's scope PRINTS
/// this number, and a scope with its own copy of the formula is a scope that can report an excitation
/// the engine is not applying. `weights` are the normalised shares.
inline float blendExcite(std::span<const double> weights) {
	double spread = 0.0;
	for (const double w : weights) {
		spread += w * w;
	}
	const double excite = 2.0 * (1.0 - spread);
	return static_cast<float>(excite < 0.0 ? 0.0 : (excite > 1.0 ? 1.0 : excite));
}

/// Build the effective table for a crossfade between two zones, `xf01` 0 = all `a`, 1 = all `b`.
///
/// This is a crossfade in waypoint space, not a mix of two rendered waveforms: the waypoints, gain
/// and endpoint amplitude interpolate, and around the midpoint bank B's morph-excite factors push
/// amplitude past both banks, amplify curvature and warp phase — shapes that exist only while
/// morphing. At either endpoint the excitation vanishes and the result renders identically to that
/// bank.
///
/// Per-sample modifiers (phaseJitter, ampNoise) are interpolated into the result but not applied
/// here — they are per-sample, and this is per-block. OscVoice::nextSample applies them, off
/// modifiers.h, and reads the depths back off this table.
owlet::dsp::PhiMorphWavetable buildMorphXfade(const owlet::dsp::PhiMorphWavetable& a,
                                               const owlet::dsp::PhiMorphWavetable& b, float xf01);

/// The same blend over any number of sounds: RECIPES averaged by weight, not audio summed.
///
/// The design decision is recipe blending (multi-oscillator blending is deliberately left for later), and
/// it is the generalisation of buildMorphXfade rather than a replacement for it. One oscillator still
/// plays the result, so eight sounds cost what two cost, and what you hear between them is a shape
/// that is neither rather than both at once.
///
/// EXCITE GENERALISES AS THE SPREAD OF THE WEIGHTS: `2·(1 − Σw²)`, which is 0 when one sound has all
/// of it and greatest when they share evenly. At two sounds that is algebraically `4t(1−t)` — exactly
/// morphExcite — so the shape of the morph is the same function it has always been, now asking "how
/// mixed is this" instead of "how far along a fade".
///
/// `weights` must be the same length as `tables` and sum to 1; the caller has already normalised
/// them, because the geometry that produced them is the only place that knows what to do when the
/// puck sits on an anchor.
///
/// EVERY SOUND COUNT COMES THROUGH HERE NOW. Two sounds used to keep buildMorphXfade, because
/// bit-identity with v1 was the shipping promise; §2c made the rule uniform once blue shift landed, and
/// buildMorphXfade is reached only through parity mode. The suite still pins it exactly there, so the
/// reference cannot bit-rot into something that merely resembles what v1 did.
owlet::dsp::PhiMorphWavetable buildMorphBlend(std::span<const owlet::dsp::PhiMorphWavetable> tables,
                                               std::span<const double> weights,
                                               const BlueShift& blue = {});

} // namespace feathers
