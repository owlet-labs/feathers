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
#include "character/phi_stair.hpp"

#include <span>

// The plugin's own renderer for the PhiStair character layer.
//
// The firmware builds the staircase (buildPhiStairParams) and then, inside a fixed-point renderer
// (since removed), resampled it onto a 128-slot q31 table which the scan loop interpolated. We keep
// the staircase — that is the character, and it is pinned by the golden test — and replace the
// resample and the scan with float evaluation at arbitrary phase resolution. Evaluation is a pure
// function of params and phase, and the rebuild crossfade belongs to the voice path.

namespace feathers {

/// A PhiStairParams with the per-sample work hoisted out of it: widths renormalised to cumulative
/// cycle boundaries, the wrap's riser origin resolved, and the loudness rule already measured.
///
/// Preparing this costs 128 evaluations, so it belongs on the zone/crossfade change and not in the
/// sample loop. It is plugin-owned; the firmware has no equivalent because it bakes the same
/// quantities straight into its q31 slot table.
struct StairTable {
	/// Cumulative normalised width, so step i spans [boundaries[i], boundaries[i + 1]).
	/// boundaries[0] is 0 and boundaries[kPhiStairSteps] is 1. A step the character layer has faded
	/// out is a zero-width interval, which drops out of the search for free.
	double boundaries[owlet::dsp::kPhiStairSteps + 1];
	float heights[owlet::dsp::kPhiStairSteps];
	float slope;

	/// Last step with any width at all. Step 0's riser rises from THIS height, which is what joins the
	/// end of the cycle to its start.
	int lastActive;

	/// The DC the firmware's table build subtracts, and the gain it applies afterwards, folded to the
	/// ±1.0 full scale the plugin works in. See the .cpp for why these are measured on 128 samples.
	float mean;
	float scale;

	/// The staircase rendered onto the firmware's own 128-slot grid: slot j holds the waveform at phase
	/// j/128, normalised exactly as evalStairTable normalises it. This is what the original fixed-point Stair table build
	/// wrote into its slot table, in float rather than q31.
	///
	/// The last entry repeats the first. The firmware's scan reads slot idx and idx+1 and declares
	/// its slot table one entry longer for precisely that reason; carrying the same guard means the
	/// interpolation needs no wrap test in the sample loop.
	float slots[owlet::dsp::kPhiStairSlots + 1];

	/// The anti-alias mips: `mip1` is `slots` put through one binomial pass across the grid, `mip2` is
	/// `mip1` through another. Same 128 slots, no decimation — the firmware's two mip tables.
	///
	/// Which one plays is a function of pitch alone and nothing to do with the character. A high note
	/// on hardware is not playing the bright table at all, so rendering the bright table at every pitch
	/// would be wrong in brightness as well as noisier. See stairMipForFrequency.
	float mip1[owlet::dsp::kPhiStairSlots + 1];
	float mip2[owlet::dsp::kPhiStairSlots + 1];
};

/// Prepare `p` for evaluation. Also the only place the loudness rule runs.
StairTable buildStairTable(const owlet::dsp::PhiStairParams& p);

/// Evaluate the staircase at `phase01`, normalised so full scale is ±1.0.
///
/// Phase wraps, so any real value is legal. A nominal zone reaches ±0.475 rather than the rail: the
/// firmware's table build scales into half of q31 and its render doubles the amplitude back, the same
/// convention the morph evaluator normalises against. The clip at ±1.0 is part of evaluation and
/// exists for one real case — a step narrower than the 128 samples the loudness rule is measured on,
/// which is therefore scaled by a gain that never saw it.
float evalStairTable(const StairTable& table, double phase01);

/// Convenience overload for tests and one-shots: prepares a table and throws it away. Identical
/// output to the pair above, and far too expensive for a sample loop.
float evalStairTable(const owlet::dsp::PhiStairParams& p, double phase01);

/// Evaluate through the 128-slot grid instead — two slot loads and a lerp, which is the firmware's
/// scan in float. THIS is what the voice path renders.
///
/// The difference from evalStairTable is not an approximation of it. The original fixed-point Stair renderer never evaluates
/// the staircase continuously; it resamples onto this grid and interpolates, and the band-limiting
/// that imposes is part of how the hardware sounds rather than an artefact of how it is implemented.
/// A step narrower than 1/128 of a cycle does not survive the grid on hardware and must not survive
/// it here either. evalStairTable remains the grid's own reference — it is what the slots are built
/// from, and the tests use it to pin that they agree at the slot phases.
float evalStairSlots(const StairTable& table, double phase01);

/// Which mip a note of `frequencyHz` plays: 0 for the bright grid, 1 or 2 for the filtered ones.
///
/// The original fixed-point Stair renderer selects on its phase increment, comparing against 68174083 and 175304787. A phase
/// increment is frequency over sample rate scaled by 2^32, so at the hardware's 44100 Hz those come
/// out at 699.999989 Hz and 1799.999994 Hz. They are 700 and 1800 to within a hundredth of a
/// millihertz — the "~700/1800 Hz" of its comment, and thresholds it shares with WEAVE.
///
/// Expressed in Hz rather than as a phase increment deliberately. Keeping the firmware's phase-
/// increment form would make the switch points move with the host sample rate, so the same note would
/// change brightness at 96 kHz. The threshold is a statement about the note, so it is written as one.
///
/// The switch is hard, with no crossfade between mips. That is the firmware's behaviour, not a
/// simplification of it.
int stairMipForFrequency(double frequencyHz);

/// The 129-entry grid for a mip level; anything outside 0..2 clamps into range.
const float* stairMipSlots(const StairTable& table, int mipLevel);

/// Evaluate one 129-entry grid — the shared body of evalStairSlots, exposed so the voice path can
/// hoist the grid selection out of its sample loop.
float evalStairGrid(const float* slots, double phase01);

/// Build the effective staircase for a crossfade between two banks, `xf01` 0 = all `a`, 1 = all `b`.
///
/// A plain lerp of heights, widths and slope — which is exactly what the original fixed-point Stair table build does, in
/// float, before it quantises anything. The halved-difference lerps that file is full of are all in
/// the q31 scan, where the raw difference between two full-scale slots would wrap; there is no such
/// hazard in the params.
owlet::dsp::PhiStairParams buildStairXfade(const owlet::dsp::PhiStairParams& a,
                                            const owlet::dsp::PhiStairParams& b, float xf01);

/// The same blend over any number of staircases: heights, widths and slope averaged by weight.
///
/// The exact generalisation of buildStairXfade, and it reduces to it by construction — a lerp is the
/// weighted average at `(1−t, t)`. Stair has no excite shaping to argue about, so unlike the morph
/// side nothing here fails to reduce; the two-sound path still goes through buildStairXfade because
/// bit-identity is a promise and the same number by different arithmetic is not the same bits.
///
/// `weights` must be as long as `params` and sum to 1 — the caller normalised them, because only the
/// geometry knows what to do when the puck sits on an anchor. An empty span gives a zeroed staircase,
/// which prepares into silence rather than into a division by zero.
/// BLUE SHIFT REACHES STAIR TOO, and the design names no engine — see blue_shift.h. What differs is
/// only which of the recipe's own quantities carries each half:
///
///   - the weight tilt is IDENTICAL, because it is a statement about the constellation rather than
///     about a waveform;
///   - brightness leans on SLOPE, and it leans the other way. `slope` is the fraction of a step spent
///     ramping to its height, so a SMALLER slope is a faster edge and a brighter sound. Approaching
///     therefore shortens it. Written out because the inversion looks like a sign error to anyone who
///     has just read the morph side, where more curvature means more edge;
///   - the overshoot pushes HEIGHTS along the velocity, which is the step pattern's own amplitude.
///     Differentially, so it changes the SHAPE — a uniform push would be undone by the loudness rule
///     that follows, which normalises DC and RMS.
owlet::dsp::PhiStairParams buildStairBlend(std::span<const owlet::dsp::PhiStairParams> params,
                                            std::span<const double> weights,
                                            const BlueShift& blue = {});

/// The crossfade prepared for evaluation, which is what a caller wanting to hear one always needs.
///
/// Stair takes a step morph does not: the loudness rule has to be re-measured, because a crossfade of
/// two staircases is a third staircase with its own DC and its own peak. This exists so that costs a
/// caller one call rather than two, matching the build-then-evaluate shape of the morph side.
StairTable buildStairXfadeTable(const owlet::dsp::PhiStairParams& a, const owlet::dsp::PhiStairParams& b,
                                float xf01);

} // namespace feathers
