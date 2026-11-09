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
 */

#pragma once

#include "dsp/fast_math_approx.h"

#include <numbers>

// THE TPT ONE-POLE — a single trapezoidal integrator in a feedback loop, the "zero-delay feedback"
// one-pole of Vadim Zavalishin, "The Art of VA Filter Design" (rev. 2.1.2, 2020), §3.9-3.10.
//
// The analog prototype is H(s) = ωc / (s + ωc). Discretising the integrator with the trapezoidal rule
// and prewarping the cutoff gives the instantaneous gain g = tan(π·fc/sr), and solving the one-sample
// loop for its output gives, with G = g / (1 + g) and s the integrator's state,
//
//     v  = (x − s) · G
//     lp = v + s
//     s  = lp + v              (the trapezoidal state update: s ← s + 2v)
//     hp = x − lp
//
// whose transfer function is H_lp(z) = g(z + 1) / ((1 + g)z + (g − 1)) — unity at DC, a zero at
// Nyquist, and the corner exactly where the prewarp put it. The highpass is the input less the
// lowpass, which is H_hp = 1 − H_lp exactly (Zavalishin §3.10), so it is one state and not a second
// filter. `multimode_filter.cpp`'s onePoleResponse is this same H(z), and a case measures the two
// against each other.
//
// THE PREWARP IS `fasttan`, THE SAME FUNCTION THE SVF'S SOLVE USES, so the one-pole and the two-pole
// sections of a cascade sit on the same corner to the bit. NO NYQUIST CLAMP IN HERE: tan of an angle
// past π/2 goes negative and the filter stops being one, so the CALLER clamps fc/sr below 0.5 (the
// multimode filter holds it to 0.499, as its SVF does).

namespace feathers::dsp {

class TptOnePole {
public:
	/// Set the corner. `cutoffHz / sampleRate` must be below 0.5 — see the file header.
	void setCutoff(float cutoffHz, float sampleRate) {
		const float g = fasttan(std::numbers::pi_v<float> * cutoffHz / sampleRate);
		G_ = g / (1.0f + g);
	}

	void reset() { s_ = 0.0f; }

	/// One sample; the lowpass output.
	float lowpass(float x) {
		const float v = (x - s_) * G_;
		const float lp = v + s_;
		s_ = lp + v;
		return lp;
	}

	/// One sample; the highpass output, input minus lowpass.
	float highpass(float x) { return x - lowpass(x); }

	/// The coefficient and the state, for the cases that read them rather than infer them.
	[[nodiscard]] float gainForTest() const { return G_; }
	[[nodiscard]] float stateForTest() const { return s_; }

private:
	float G_ = 0.0f;
	float s_ = 0.0f;
};

/// The lowpass and highpass as the multimode filter holds them: one object per response, with `step`.
struct OnePoleLP {
	TptOnePole pole;
	void setCutoff(float cutoffHz, float sampleRate) { pole.setCutoff(cutoffHz, sampleRate); }
	void reset() { pole.reset(); }
	float step(float x) { return pole.lowpass(x); }
};

struct OnePoleHP {
	TptOnePole pole;
	void setCutoff(float cutoffHz, float sampleRate) { pole.setCutoff(cutoffHz, sampleRate); }
	void reset() { pole.reset(); }
	float step(float x) { return pole.highpass(x); }
};

} // namespace feathers::dsp
