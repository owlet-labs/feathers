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

// HOW FAST YOU ARE MOVING, AND IN WHICH DIRECTION — the one quantity in this engine that is not a
// function of position.
//
// The blue shift needs it: a constellation has no target to aim at, so where you are GOING is what
// replaces the direction a two-point fade got for free. Smoothed rather than raw because a puck's
// per-block difference is a step function full of holes — a host that sends one parameter event per
// block and a host that sends one per buffer would otherwise disagree about how fast the same gesture
// was — and because ~50–100 ms is the scale at which a gesture reads as a gesture rather than as a
// jitter.
//
// IT UPDATES ON A FIXED SAMPLE GRID, AND THAT IS THE WHOLE INVARIANCE ARGUMENT. Time enters this
// engine nowhere else, so this is the one place where a host's choice of buffer size could reach the
// audio, and the constraint is bit-exactness rather than closeness (see the block-size case in
// tests/test_synth.cpp). A smoother stepped once per block with a dt taken from the block length
// would be invariant only in the limit: exp(-n₁/τ)·exp(-n₂/τ) and exp(-(n₁+n₂)/τ) are the same number
// in algebra and different numbers in floating point, and the difference lands in a wavetable.
//
// So the step is always exactly one tick of kTickSamples, the coefficient is a constant computed once
// per sample rate, and a report of n samples applies however many whole ticks n crossed. The tick
// boundaries are at absolute sample positions, and the parameters at a given absolute sample do not
// depend on how the host chopped its buffer — so neither does the sequence of steps, nor their order,
// nor their number. The same reasoning, and the same integer counting, as the character bank's
// deferred-build guard.

namespace feathers {

class PuckMotion {
public:
	/// How often the smoother steps. Short enough that a fast gesture is not quantised into stairs —
	/// 64 samples is 1.3 ms at 48 kHz, against a time constant a hundred times that — and long enough
	/// that the arithmetic is nothing. A power of two only because it reads as a grid; nothing here
	/// requires it.
	static constexpr int kTickSamples = 64;

	/// The gesture scale, from §2b: *"the smoothed derivative of the puck, time constant ~50–100 ms as
	/// a feel dial"*. The middle of the range named, and the dial is the ear's.
	static constexpr double kTimeConstantSeconds = 0.075;

	/// Below this, in units per second, the gesture is OVER and the velocity becomes exactly zero.
	///
	/// A GEOMETRIC DECAY DOES NOT REACH ZERO, and on this arithmetic it does not even get close enough
	/// to stop mattering. Multiplying by 0.982 each tick walks the value down through the denormals and
	/// then STICKS: at 1.38e-322 the product rounds back to itself, because the step is smaller than
	/// half the gap between adjacent denormals, and the velocity sits there for the rest of the
	/// session. Three things follow, and all three are wrong:
	///
	///   - static purity is broken after the first gesture. §2b.1's "at rest a patch is a pure function
	///     of position" holds by every term being multiplied by a velocity that is exactly zero, and a
	///     velocity that is merely almost zero keeps the whole motion path live forever;
	///   - the character is rebuilt on every tick for the ~55 seconds the value spends crawling down,
	///     because each tick's velocity differs from the last — measured at roughly twice the CPU;
	///   - the bits depend on the HOST. A host that runs its audio thread with flush-to-zero set turns
	///     those denormals into real zeros and gets a different sound from one that does not.
	///
	/// A tenth of a nanometre per second, against gestures that live between 0.1 and 10 units per
	/// second. Ten orders of magnitude below anything a hand can do, and reached about 1.9 seconds
	/// after a gesture ends, so nothing audible is being truncated — what is cut off is a number that
	/// was only ever going to be multiplied by a field and rounded away.
	static constexpr double kRestThreshold = 1.0e-9;

	/// Also resets the state: a rate change is not a gesture, and smoothing across one would invent a
	/// velocity out of a sample-rate switch.
	void setSampleRate(double hostSampleRate);

	/// Where the puck is now. Held until the next tick — parameters are constant between events, so a
	/// tick reads the same position wherever inside a span it falls.
	void setPosition(AnchorPoint puck) { position_ = puck; }

	/// Report `hostSamples` of rendering, exactly as the character bank is told. Steps the smoother
	/// once per whole tick crossed.
	void advance(int hostSamples);

	/// How many host samples may be rendered before the velocity is allowed to change.
	///
	/// The character bank's samplesUntilPendingBuild has exactly this shape and exists for exactly this
	/// reason: a renderer asks, renders no further, and the new value therefore lands at the same
	/// ABSOLUTE sample whatever the host's buffer size. Without it the velocity would still be
	/// invariant — it is stepped on this grid — but the moment it reached the character would not be,
	/// because a character is rebuilt when the renderer happens to look. Never zero.
	int samplesUntilTick() const { return kTickSamples - samplesIntoTick_; }

	/// The smoothed velocity, in constellation units per second. EXACTLY zero until the puck has
	/// actually moved, which is what makes every blue-shift effect exactly zero at rest rather than
	/// nearly zero: the difference of a position with itself is ±0.0, and a pole fed zeros stays at
	/// the zero it started from.
	AnchorPoint velocity() const { return velocity_; }

	/// Forget the gesture without forgetting where the puck is. Used where a synth resets its voices:
	/// a transport jump is not a swipe across the pad.
	void reset();

private:
	void step();

	AnchorPoint position_ {};
	AnchorPoint lastTickPosition_ {};
	AnchorPoint velocity_ {};
	bool havePosition_ = false;

	int samplesIntoTick_ = 0;
	double tickSeconds_ = static_cast<double>(kTickSamples) / 48000.0;
	/// exp(-tick/τ): what a step keeps of the old velocity. Constant, because the step is.
	double retain_ = 0.0;
};

} // namespace feathers
