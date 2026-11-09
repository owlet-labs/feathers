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

#include <algorithm>
#include <cmath>

// THE AUTOMODULATOR'S ENVELOPE FOLLOWER — how loud the input is, and whether it is getting louder.
//
// WHAT IT IS FOR. The automodulator's Mod field derives five influence amounts from this: the
// envelope's LEVEL pushes the LFO's depth, its phase, and its value; the envelope's DERIVATIVE
// pushes depth and phase again. So a hard-picked note opens the filter further and shifts where in
// the sweep it lands, which is what makes the effect play like an auto-wah rather than a tremolo.
// (The two PHASE pushes are compiled out in the firmware and unwired here — `envelopeSteerFor` in
// automod.h is the port of the three that ship; until 2026-09-16 only the level→depth road was.)
//
// ---------------------------------------------------------------------------------------------
// PER SAMPLE, WHERE THE FIRMWARE IS PER BUFFER — AND HERE THAT REALLY IS A CORRECTNESS FIX.
// ---------------------------------------------------------------------------------------------
//
// The same claim was made about the LFO and was WRONG there: the firmware's phase advance is
// `phaseInc * bufferSize` in uint32, which is exact for any decomposition. This is a different
// case and the difference is worth stating, because the two look alike.
//
// `automodulator.cpp:613` runs `state += (peak - state) * attack` ONCE PER BUFFER, with `attack` a
// bare coefficient. That is not a per-sample rate accumulated over the buffer — it is one step of a
// one-pole whose TIME CONSTANT IS THEREFORE MEASURED IN BUFFERS. Halve the host's block size and the
// envelope moves twice as fast, for the same audio. Nothing recovers that; it is not a rounding
// difference and no integer trick fixes it.
//
// `tests/test_synth.cpp` pins that a performance renders identically in blocks of 1 and 37, so a
// buffer-rate coefficient cannot come across. This follower runs one identical update per SAMPLE,
// which is exactly block-invariant: a one-pole's decay composes, `exp(-a)*exp(-b) == exp(-(a+b))`,
// so any chunking of the same audio gives the same trajectory.
//
// AND THE TIMES ARE SECONDS, NOT COEFFICIENTS, for the same reason. The firmware's `envAttack` is
// `flavorScalar^2` used directly as a per-buffer coefficient — a number whose meaning depends on the
// host. Seconds mean the same thing everywhere. Nothing user-facing is lost: that scalar comes from a
// phi triangle rather than from a knob, so there is no control whose feel is being changed; what the
// field's 0..1 should MEAN in seconds is a mapping the effect chooses, and that choice belongs with
// the effect rather than here.
//
// THE THIRD DIFFERENCE IS NOT A CHARACTER CHOICE — IT IS THE SAME DEFECT AGAIN, AND HERE IS WHY.
//
// This file first recorded "the firmware follows the PEAK OF EACH BUFFER; this follows |input| every
// sample" as a sound decision to be escalated, in the same class as the comb's linear interpolation.
// It is not, and the source says so in two ways.
//
// FIRST, IT IS NOT A PEAK. `automodulator.cpp:570`:
//
//     q31_t peakL = std::max(std::abs(buffer.front().l), std::abs(buffer.back().l));
//
// That is the larger of the FIRST AND LAST SAMPLE of the block. Everything between them is ignored,
// so a transient in the middle of a buffer is invisible to it — two samples out of a hundred and
// twenty-eight. Three lines below, the code calls itself `// Envelope follower at buffer rate -
// one-pole filter on INPUT peaks`. The comment claims peaks; the code samples two endpoints.
//
// SECOND, AND THIS IS THE PART THAT SETTLES IT, THE AUTHOR SAID SO. Directly above that line:
//
//     // Sample at buffer boundaries instead of scanning entire buffer (~400 cycles saved)
//     // TODO: verify this doesn't regress envelope tracking feel compared to full peak scan
//
// A stated CPU saving and an admitted unverified risk to the feel. There is no reading under which
// this is an instrument somebody tuned.
//
// AND THE TWO HALVES ARE NOT SEPARABLE ANYWAY, which is the practical point: fixing the block
// dependence forces per-sample processing, and a per-sample follower tracks |x| because "peak of the
// buffer" only exists as a concept while the processing is per-buffer. The character cannot be kept
// without keeping the bug.
//
// This project has also already fixed the block-rate half once, in the OWLTT meter's fall time — 314
// ms at block 32 against 1610 ms at block 512. Fixing it in a meter and porting it into a filter is
// not a position anyone would defend.
//
// ---------------------------------------------------------------------------------------------
// AND DO NOT GO LOOKING FOR THE CASE THAT DEFENDS THE THIRD HALF — THE SIGNATURE IS THE DEFENCE.
// ---------------------------------------------------------------------------------------------
//
// Every design decision on this device that changed behaviour gets a case that fails if it is quietly
// reverted, because a decision no test protects can be undone by anyone tidying up and the diff reads
// as a simplification. Two of the three here have one: a per-BUFFER coefficient reds three cases,
// and tracking the raw input instead of its magnitude reds one.
//
// THE THIRD HAS NONE, AND THAT IS A WALL RATHER THAN A HOLE. Reverting to peak-of-buffer would need
// a block-rate entry point, and `process()` takes ONE SAMPLE — so there is no mutation to write,
// because the revert cannot be expressed without rewriting every caller. **A decision defended by API
// SHAPE is defended better than one defended by a case: a case can be deleted by someone tidying up;
// a signature cannot be reverted quietly.**
//
// Written down because the next person auditing this file's coverage will look for the missing case
// otherwise, and conclude a gap where there is none.
//
// NO ALLOCATION AND NO STATE BEYOND FOUR FLOATS PER CHANNEL.

namespace feathers::fx::automod {

/// WHICH EAR. Named rather than a bare index, because `level(0)` at a call site says nothing and this
/// object's whole output is two numbers that must not be swapped.
enum class Channel : int { Left = 0, Right = 1 };

/// THE FASTEST TIME THE FOLLOWER WILL TAKE SERIOUSLY, in seconds. Below this the coefficient rounds
/// to 1 and the follower simply tracks its input, which is the correct behaviour for "no smoothing at
/// all" — so this is where the range collapses gracefully rather than a clamp preventing something.
inline constexpr float kEnvMinSeconds = 1.0e-5f;

class EnvelopeFollower {
public:
	/// Told the sample rate, which is what turns a time into a per-sample coefficient. RESETS, for
	/// the reason every other device here does: carrying an envelope across a rate change would be
	/// inventing a level out of a configuration event.
	void prepare(double sampleRate) {
		sampleRate_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
		recomputeCoefficients();
		reset();
	}

	/// Silence, and no motion. What activate() and an all-notes-off mean.
	void reset() {
		for (int i = 0; i < 2; ++i) {
			level_[i] = 0.0f;
			derivative_[i] = 0.0f;
		}
	}

	/// HOW FAST IT RISES AND HOW SLOWLY IT FALLS, in seconds. Both are time constants: after
	/// `attackSeconds` of a step, the level has covered 1 - 1/e of the distance.
	///
	/// ASYMMETRIC ON PURPOSE, which is what makes it an envelope follower rather than a lowpass. A
	/// fast attack and a slow release track a note's onset and then hold through its body; equal
	/// times would smear the onset that the whole effect is triggered by.
	void setTimes(float attackSeconds, float releaseSeconds) {
		attackSeconds_ = sanitiseTime(attackSeconds);
		releaseSeconds_ = sanitiseTime(releaseSeconds);
		recomputeCoefficients();
	}

	/// ONE SAMPLE, BOTH CHANNELS. See the header for why this is per sample and not per buffer.
	void process(float left, float right) {
		step(0, left);
		step(1, right);
	}

	/// How loud that ear is, 0 upward. Never negative — it follows |input|.
	[[nodiscard]] float level(Channel ch) const noexcept { return level_[static_cast<int>(ch)]; }

	/// HOW FAST THE LEVEL IS CHANGING, in level per SECOND, smoothed by the attack time.
	///
	/// PER SECOND RATHER THAN PER SAMPLE, and that is what makes the number mean the same thing at
	/// every sample rate — a per-sample delta is a thousand times smaller at 48 kHz than the same
	/// musical gesture is at 48 Hz. Positive while the input is swelling, negative while it decays.
	///
	/// The effect scales this by an influence amount from the phi field, and choosing that scaling is
	/// the effect's decision rather than this object's — which is why nothing is normalised here.
	[[nodiscard]] float derivative(Channel ch) const noexcept { return derivative_[static_cast<int>(ch)]; }

private:
	static float sanitiseTime(float seconds) {
		if (!std::isfinite(seconds) || seconds < 0.0f) {
			return 0.0f;
		}
		return seconds;
	}

	/// A one-pole's per-sample coefficient for a given time constant. A zero or sub-quantum time
	/// gives 1, which is "follow the input exactly" rather than a special case anybody has to write.
	float coefficientFor(float seconds) const {
		if (seconds < kEnvMinSeconds) {
			return 1.0f;
		}
		return 1.0f - std::exp(-1.0f / (seconds * static_cast<float>(sampleRate_)));
	}

	void recomputeCoefficients() {
		attackCoeff_ = coefficientFor(attackSeconds_);
		releaseCoeff_ = coefficientFor(releaseSeconds_);
	}

	void step(int i, float input) {
		// A NON-FINITE SAMPLE MUST NOT BECOME A PERMANENT LEVEL. The follower has memory, so one NaN
		// in the audio would otherwise poison every block after it — which is worse than a click, and
		// invisible until somebody wonders why the filter stopped moving.
		const float magnitude = std::isfinite(input) ? std::fabs(input) : 0.0f;

		const float before = level_[i];
		// RISING AND FALLING TAKE DIFFERENT COEFFICIENTS. This is the asymmetry, and it is the whole
		// difference between a follower and a lowpass.
		const float coeff = (magnitude > before) ? attackCoeff_ : releaseCoeff_;
		level_[i] = before + (magnitude - before) * coeff;

		// THE DERIVATIVE IS SMOOTHED WITH THE ATTACK COEFFICIENT, which is the firmware's choice and
		// is the right one: an unsmoothed per-sample difference is mostly quantisation noise, and
		// smoothing it with the RELEASE time would make a fast transient's derivative arrive late.
		const float perSecond = (level_[i] - before) * static_cast<float>(sampleRate_);
		derivative_[i] += (perSecond - derivative_[i]) * attackCoeff_;
	}

	double sampleRate_ = 48000.0;
	float attackSeconds_ = 0.001f;
	float releaseSeconds_ = 0.100f;
	float attackCoeff_ = 1.0f;
	float releaseCoeff_ = 1.0f;
	float level_[2] {};
	float derivative_[2] {};
};

} // namespace feathers::fx::automod
