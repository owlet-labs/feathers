/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "fx/drawn/drawn_types.h"

#include <array>

// THE DRAWN-CURVE FILTER'S SAMPLE LOOP — six biquads in series, per channel.
//
// WHY THIS IS NOT `MultimodeFilter`. Every other filter device in this repo hands its state to that
// class, and this one cannot: `MultimodeFilter` is driven by (type, cutoff, resonance) and realises
// its own coefficients from them. THIS device's state IS coefficients — arbitrary pole and zero
// positions, six sections of them — so there is nothing to hand over. What it borrows instead is the
// SHAPE: a transposed-direct-form-II section with double state, which is the form the vendored
// filters and `harm`'s notch use, so the numerical behaviour is the one this repo already reasons
// about.

namespace feathers::fx::drawn {

/// HOW MANY SAMPLES BETWEEN COEFFICIENT UPDATES. The puck is modulatable, so the coefficients move;
/// re-realising per sample would be six quadratic solves per sample and re-realising per BLOCK would
/// quantise a fast morph to the host's block size. Thirty-two samples is 0.67 ms at 48 kHz — below
/// anything audible as a step, and 1/32 of the cost.
inline constexpr int kControlBlock = 32;

/// HOW LONG THE **POLES** TAKE TO FOLLOW THE PUCK, in seconds. Fixed, and `Wake` does not reach it —
/// that is what keeps the stability argument true by construction rather than by measurement.
inline constexpr double kSmoothingSeconds = 0.005;

/// HOW LONG THE **ZEROS** TAKE AT FULL `Wake`. The two constants are the ends of a GEOMETRIC sweep:
/// `tau = kSmoothingSeconds * (kWakeMaxSeconds / kSmoothingSeconds)^wake`, so the knob is even in
/// octaves of time the way every other time control in this repo is, and `wake == 0` gives back
/// `kSmoothingSeconds` EXACTLY — `pow(x, 0)` is exactly 1 in IEEE, which is what makes "bit-identical
/// at zero" a fact rather than a tolerance.
///
/// **HALF A SECOND IS A DEFENDED STARTING POINT, NOT A MEASURED ONE.** The effect has to outlast the
/// gesture that causes it or there is nothing to hear: a puck flick by hand is 150-400 ms, and three
/// time constants at 0.5 s is about 1.5 s of trail — clearly a wake rather than a smoothing artifact.
/// The ratio is 100:1 across the knob's travel, which is a comfortable geometric span. Expect it to
/// tune it by ear; it is one number and nothing depends on its value.
inline constexpr double kWakeMaxSeconds = 0.5;


class DrawnFilter {
public:
	void prepare(double sampleRate);

	/// THE ONLY INPUT ROAD. Blends the four frames at the puck, applies the globals, and realises —
	/// see `realisedAt`. The result becomes the TARGET the smoother walks toward.
	void setTarget(const Settings& settings);

	[[nodiscard]] const Settings& target() const { return target_; }
	/// The coefficients the sample loop is running RIGHT NOW, which lag `target()` by the smoother.
	[[nodiscard]] const Realised& realised() const { return realised_; }
	/// The smoothed pole and zero positions behind them.
	[[nodiscard]] const Frame& smoothedFrame() const { return smoothed_; }
	/// THE PATH BEFORE THE IDLE SKIP (backlog PB 3), for its equivalence case: realise every control block, moved or not.
	void setReferenceForTest(bool on) { referenceForTest_ = on; }
	/// Control blocks on which the frame was realised (the skip's count: a case can see it skip and see it wake).
	[[nodiscard]] uint64_t realisationsForTest() const { return realisations_; }

	/// Forget the delay lines. The coefficients stay where they are.
	void reset();
	/// AS IF JUST SWITCHED ON: forget the delay lines AND the fact that the device was running, so
	/// the next block SNAPS to its coefficients instead of ramping in from wherever the puck last
	/// left them. `AudioOps::reset` means this one — the filter device learned the distinction the
	/// hard way and the waypoint device restates it.
	void restart();

	/// **DRIVE, PER SAMPLE, FOR THE NEXT CALL** — its tick window, or null while Drive is not moving (backlog FK).
	void setDriveRun(const float* run) noexcept { pendingDriveRun_ = run; }
	void processStereo(float* left, float* right, int frames);
	void processMono(float* mono, int frames);

private:
	/// ONE SECTION'S STATE. Transposed direct form II: two words, double, and the coefficients are
	/// arguments rather than members because all six sections share one loop.
	struct SectionState {
		double z1 = 0.0;
		double z2 = 0.0;
		void reset() { z1 = z2 = 0.0; }
		[[nodiscard]] float process(float x, const Section& s) {
			const double in = static_cast<double>(x);
			const double out = static_cast<double>(s.gain) * in + z1;
			z1 = static_cast<double>(s.gain) * static_cast<double>(s.c.b1) * in
			     - static_cast<double>(s.c.a1) * out + z2;
			z2 = static_cast<double>(s.gain) * static_cast<double>(s.c.b2) * in
			     - static_cast<double>(s.c.a2) * out;
			return static_cast<float>(out);
		}
	};

	/// ADVANCE THE SMOOTHER AND RE-REALISE. Called once per control block.
	void advanceControl();
	/// RECOMPUTE BOTH ONE-POLE COEFFICIENTS. Called wherever the sample rate or `Wake` can change.
	void refreshAlphas();
	[[nodiscard]] static float driveShape(float x, float gain, float comp);
	/// `drive` is the Drive window at this block's first sample, or null to use the setting.
	void runBlock(float* left, float* right, int frames, const float* drive);

	Settings target_ {};
	/// THE BLENDED, GLOBALS-APPLIED FRAME the smoother is walking toward.
	Frame targetFrame_ {};
	/// WHERE IT HAS GOT TO. **The smoothing happens in the POLE AND ZERO domain, not in the
	/// coefficients**, and that is the same rule the blend follows for the same reason: a convex
	/// combination of two stable poles is stable, so every intermediate frame the smoother passes
	/// through is a legal filter by construction.
	///
	/// AND THE POLES AND ZEROS WALK AT DIFFERENT SPEEDS — that gap IS `Wake` (design §8). The poles'
	/// speed is fixed, so the stability argument above is untouched by anything the knob does.
	Frame smoothed_ {};
	Realised realised_ {};

	double sampleRate_ = 48000.0;
	/// THE POLES' ONE-POLE COEFFICIENT, from `kSmoothingSeconds` and nothing else.
	float poleAlpha_ = 1.0f;
	/// THE ZEROS', from `Wake`. Equal to `poleAlpha_` BIT FOR BIT at `wake == 0`, because both come
	/// out of the same expression on the same number.
	float zeroAlpha_ = 1.0f;
	/// HAS THE SMOOTHER BEEN PRIMED? False means the next update snaps rather than ramps.
	bool active_ = false;
	bool referenceForTest_ = false;
	uint64_t realisations_ = 0;

	/// HOW FAR INTO THE CURRENT CONTROL BLOCK THE DEVICE IS, carried across `processStereo` calls.
	///
	/// WITHOUT IT THE TIME CONSTANTS ARE NOT TIME CONSTANTS. `refreshAlphas` expresses
	/// `kSmoothingSeconds` and `kWakeMaxSeconds` in CONTROL BLOCKS — one step per `kControlBlock`
	/// samples — so a step taken at any other rate is a different smoother. Restarting the chunking at
	/// each call takes one step per CALL instead, which at eight-sample blocks ran the 5 ms poles at
	/// 1.25 ms; `HARM` carries the same counter for the same reason, and `MultimodeFilter` carries
	/// `untilTick_`.
	int gridPhase_ = 0;

	float driveAmount_ = 0.0f;
	float driveGain_ = 1.0f;
	float driveComp_ = 1.0f;
	/// See `setDriveRun`: set for the next call, taken by it.
	const float* pendingDriveRun_ = nullptr;

	std::array<SectionState, kBands> left_ {};
	std::array<SectionState, kBands> right_ {};
};

} // namespace feathers::fx::drawn
