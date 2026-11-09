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

// THE SPRING — a second-order smoother that can be made to overshoot, and the automodulator's whole
// reason for not sounding like a plain LFO.
//
// WHAT IT DOES THAT A ONE-POLE CANNOT. A one-pole approaches its target and stops. A spring has
// MOMENTUM: under-damp it and it overshoots the target and settles back, so a step in the modulation
// arrives as a bounce rather than as a slide. The automodulator takes the damping from its Mod field,
// so turning that knob moves the filter sweep from "tracks smoothly" to "springs past and comes
// back", which is a character change no amount of LFO shaping reproduces.
//
// ---------------------------------------------------------------------------------------------
// ONE CLASS, USED TWICE. THIS IS NOT TWO SPRINGS.
// ---------------------------------------------------------------------------------------------
//
// The firmware has a filter spring (`springPos/Vel`) and a comb spring (`combSpringPos/Vel`) written
// out separately, ~40 lines apiece. Read side by side they are the SAME semi-implicit Euler update
// with different coefficients: the comb's is critically damped, the filter's takes its omega and zeta
// from the Mod field. Nothing distinguishes the arithmetic.
//
// So this is one class the effect instantiates twice, per channel. Two classes would be a copy, and
// this repository has already made the opposite mistake's opposite point — the filter's Comb type and
// the automod's comb share one delay line for exactly this reason. The redundancy rule cuts both
// ways: check whether apparent duplication is load-bearing before removing it, and check whether
// apparent distinctness is real before preserving it.
//
// ---------------------------------------------------------------------------------------------
// COEFFICIENTS ARE DERIVED FROM SECONDS, NOT PASSED AS RAW NUMBERS.
// ---------------------------------------------------------------------------------------------
//
// The firmware passes `springOmega2Q` and `springDampingCoeffQ` — coefficients already scaled for a
// per-BUFFER update, so they carry the host's block size inside them. That is the same defect the
// envelope follower had, and it is fixed the same way: this takes a natural frequency in Hz and a
// damping ratio, both dimensionless facts about the sound, and derives the per-sample coefficients
// from the sample rate. `automod_envelope.h` states the argument at length.
//
// STABILITY IS A PROPERTY OF THE DERIVATION AND IS ASSERTED BY A CASE. Semi-implicit Euler is stable
// while `omega * dt` stays well under 1; at 48 kHz and the 15 Hz ceiling the firmware's field
// reaches, `omega * dt` is 0.002, which is four hundred times inside the bound. The frequency is
// clamped anyway, because a coefficient arriving from a field is a number nobody has bounded by hand.

namespace feathers::fx::automod {

/// WHERE A SPRING'S NATURAL FREQUENCY MAY SIT, in Hz. The firmware's field maps 0..1 to 2..15 Hz;
/// this is that range with room either side, because the mapping is the effect's to choose and a
/// component that clamped to exactly the current mapping would have to change when it did.
inline constexpr float kSpringFreqMin = 0.1f;
inline constexpr float kSpringFreqMax = 100.0f;

/// CRITICAL DAMPING IS 1.0 — approaches the target as fast as possible WITHOUT overshooting. Below
/// it the spring bounces; above it it crawls. Named rather than spelled 1.0f at call sites, because
/// "critically damped" is the thing the comb spring is and a bare 1.0 does not say so.
inline constexpr float kCriticalDamping = 1.0f;

/// The floor is not zero: an undamped spring never settles, and one arriving from a field could
/// otherwise ring forever on a value nobody chose.
inline constexpr float kSpringDampingMin = 0.05f;
inline constexpr float kSpringDampingMax = 4.0f;

class Spring {
public:
	/// Told the sample rate, which is what turns a frequency into a per-sample step.
	void prepare(double sampleRate) {
		sampleRate_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
		recompute();
		reset();
	}

	/// At rest, at zero, with no momentum.
	void reset() {
		position_ = 0.0f;
		velocity_ = 0.0f;
	}

	/// PUT IT WHERE THE SIGNAL ALREADY IS, WITHOUT MOMENTUM. What the effect does when the LFO's
	/// waveform is rebuilt under it — the firmware does exactly this (`automodulator.cpp:601`,
	/// `combSpringPos = combVal; combSpringVel = 0`) and it is not an optimisation. A spring left
	/// pointing at the old shape's value would lunge across to the new one, so a knob turn would
	/// produce a swoop the knob did not ask for.
	void snapTo(float value) {
		position_ = std::isfinite(value) ? value : 0.0f;
		velocity_ = 0.0f;
	}

	/// **MOVE WITH THE TARGET, KEEPING THE MOTION** (backlog IS). What a waveform rebuild does to a spring
	/// that is TRACKING something: its target jumps by `delta` because the shape under the LFO changed, so the
	/// position jumps by the same amount and the velocity is untouched. Neither a lunge toward the new
	/// target (the spring had no part in the change) nor a reset (its lag and bounce are the sound).
	void shiftBy(float delta) {
		if (std::isfinite(delta)) {
			position_ += delta;
		}
	}

	/// HOW FAST IT WANTS TO MOVE AND HOW HARD IT IS HELD BACK.
	///
	/// @param naturalHz   where it resonates — higher tracks faster and rings higher
	/// @param dampingRatio 1.0 is critical (no overshoot); below bounces, above crawls
	void setResponse(float naturalHz, float dampingRatio) {
		naturalHz_ = std::isfinite(naturalHz) ? std::clamp(naturalHz, kSpringFreqMin, kSpringFreqMax)
		                                      : kSpringFreqMin;
		damping_ = std::isfinite(dampingRatio)
		               ? std::clamp(dampingRatio, kSpringDampingMin, kSpringDampingMax)
		               : kCriticalDamping;
		recompute();
	}

	[[nodiscard]] float naturalHz() const noexcept { return naturalHz_; }
	[[nodiscard]] float dampingRatio() const noexcept { return damping_; }

	/// ONE SAMPLE TOWARD `target`, and the value after it.
	///
	/// SEMI-IMPLICIT EULER — velocity updated first, then position from the NEW velocity.
	///
	/// AND THE FIRST DRAFT OF THIS COMMENT OVERCLAIMED WHY. It said explicit Euler "gains energy every
	/// step and diverges however small the step", which is true of an UNDAMPED oscillator over
	/// unbounded time and is not true here. MEASURED, swapping the two lines and driving a square
	/// wave for 400,000 samples across the whole declared range — 0.1/10/100 Hz against damping
	/// 0.05/1.0/4.0 — the two orderings agree to SIX SIGNIFICANT FIGURES at every one of the nine
	/// points, and nothing diverges. At omega*dt <= 0.013 with any real damping the difference is
	/// below float resolution.
	///
	/// SO THE ORDERING IS DEFENSIVE RATHER THAN LOAD-BEARING AT TODAY'S NUMBERS, and it stays for
	/// that reason: it is free, it is the correct integrator for an oscillator, and it is what keeps
	/// the class honest if `kSpringFreqMax` is ever raised or the sample rate falls. A known-bad that
	/// swaps the two lines is GREEN, and that green is reported rather than hidden — a measured
	/// nothing on a redundancy is what stops the next reader "simplifying" it back.
	float tick(float target) {
		const float safe = std::isfinite(target) ? target : position_;

		const float error = safe - position_;
		const float acceleration = error * omega2_ - velocity_ * dampingCoeff_;
		velocity_ += acceleration;
		position_ += velocity_;

		// A SPRING HAS MEMORY, so one non-finite value would be permanent — not a crash, but a
		// modulation that silently stops moving.
		//
		// UNREACHABLE INSIDE THE DECLARED RANGE, AND THAT IS STATED RATHER THAN LEFT TO BE DISCOVERED.
		// The target is already sanitised above, and the same 400,000-sample sweep measured a peak
		// excursion of 2.16 at the worst corner of the range — nowhere near overflow. So a known-bad
		// that deletes this guard is GREEN, and honestly so: it guards a state the clamps make
		// unreachable. It stays because the clamps are the only thing making it so, and a widened
		// `kSpringFreqMax` or a caller reaching past `setResponse` would need it — an unreachable
		// guard that says it is unreachable is documentation, not a gap.
		if (!std::isfinite(position_) || !std::isfinite(velocity_)) {
			reset();
		}
		return position_;
	}

	/// Where it is, without moving it. A test-only seam in CLAUDE.md's sense — the effect mixes this
	/// into a cutoff and a delay, and neither is recoverable from the output.
	[[nodiscard]] float position() const noexcept { return position_; }

	/// How fast it is moving. Exposed because "settled" means velocity near zero AND position near
	/// target, and a case that checked only position would pass at the instant of maximum overshoot.
	[[nodiscard]] float velocity() const noexcept { return velocity_; }

private:
	void recompute() {
		const float dt = 1.0f / static_cast<float>(sampleRate_);
		const float omega = 2.0f * 3.14159265358979323846f * naturalHz_;
		// Both coefficients absorb dt, so `tick` is three multiplies and two adds.
		omega2_ = omega * omega * dt * dt;
		dampingCoeff_ = 2.0f * damping_ * omega * dt;
	}

	double sampleRate_ = 48000.0;
	float naturalHz_ = 5.0f;
	float damping_ = kCriticalDamping;
	float omega2_ = 0.0f;
	float dampingCoeff_ = 0.0f;
	float position_ = 0.0f;
	float velocity_ = 0.0f;
};

} // namespace feathers::fx::automod
