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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>

// WARP — the read phase, bent before the cycle is looked up.
//
// Serum's warp family, ported, with a dropdown near the knob. There is no phi field behind it: it is
// a plain modifier knob, as in Serum.
//
// THE ONE IDEA: a warp does not touch the stored cycle. It changes WHERE in the cycle a given output
// phase reads from. The clearest outside statement of it, from a KVR DSP thread, is the one this
// file implements: *"if the oscillator phase is at 71%, with bend at 0% the program will read the
// sample at 71%, but if bend is at 30%, it may read something like the sample at 84%."*
//
// WHY THIS IS CHEAPER AND CLEANER HERE THAN IN SERUM. Serum warps at READ time on a mipped
// wavetable, and a warped read rate makes the chosen mip the wrong one — so it drops the mip, and
// (in the words of the same thread) *"aliases like hell"*. This engine warps inside `flagshipChain`,
// and the flagship BAKES the operated cycle to 256 slots and filters two binomial mips AFTERWARDS.
// **Warp then band-limit is the alias-free order**, and it is the order this engine already had. The
// thread calls doing it properly "pretty over the top"; the flagship pays for it as a matter of
// course. What a warp costs on top is, MEASURED, at or below the noise floor of one chain evaluation.
//
// WHICH MODES ARE HERE, AND WHICH DELIBERATELY ARE NOT.
//
//   - **Remap is not here.** Remap was not requested. It is also the one mode that could not have
//     been ported cheaply: an arbitrary drawn curve is persisted state, and `flagship.h` says the
//     operator tier may not grow any.
//   - **Sync and Quantize are not here, and that is not an omission.** They are Serum warp modes and
//     they are ALREADY flagship operators with their own per-sound knobs (`+7` and `+6`). Adding
//     them to this menu would make two controls write one quantity, which is this repo's
//     most-repeated bug shape, and resolving it once for a single field-modifier knob already took
//     real work. One writer each.
//
// THE SHAPES ARE MATCHED BY DESCRIPTION, NOT REVERSE-ENGINEERED. Serum was not instrumented; each
// mode below implements what its manual and the community describe, and each says in its own comment
// what it actually computes. Where a shape here differs from Serum's it will differ in degree rather
// than in kind, and nobody should read these as bit-parity with anything.
//
// NO TRANSCENDENTALS, AND THAT IS A MEASURED CHOICE. The obvious spelling of Bend and Asym is
// `pow(p, 1+k)`. MEASURED against one chain evaluation on this machine: a `pow` costs **+36% to
// +44%**, while the same curve written as a rational costs **+0.9% to +4.6%**, which is at the
// baseline's own noise floor. Every curve below is a rational or a piecewise line for that reason.

namespace feathers {

/// The warp shapes, in menu order. `Off` is first and is the default, so a patch that never mentions
/// warp is the engine exactly as it was.
enum class WarpMode : uint8_t {
	Off = 0,
	BendPlus,
	BendMinus,
	BendBipolar,
	Pwm,
	AsymPlus,
	AsymMinus,
	AsymBipolar,
	Flip,
	Mirror,
	kCount
};

inline constexpr const char* const kWarpModeNames[] = {
    "Off", "Bend +", "Bend -", "Bend +/-", "PWM", "Asym +", "Asym -", "Asym +/-", "Flip", "Mirror",
};
static_assert(std::size(kWarpModeNames) == static_cast<size_t>(WarpMode::kCount));

/// IS THIS MODE'S KNOB BIPOLAR — neutral in the middle rather than at the bottom?
///
/// Serum's `+/-` modes sweep through both directions on one knob, so their resting place is 0.5 and
/// not 0. The unipolar modes rest at 0. **This is why the knob's own default value does not matter:**
/// the default MODE is `Off`, where the knob is inert whatever it says, so nothing is wrong at the
/// origin under either convention. The editor reads this to decide whether to draw a centre detent.
[[nodiscard]] constexpr bool warpIsBipolar(WarpMode mode) {
	return mode == WarpMode::BendBipolar || mode == WarpMode::AsymBipolar;
}

/// THE KNOB, IN THE UNITS THE SHAPE WANTS: 0..1 for a unipolar mode, -1..+1 for a bipolar one.
[[nodiscard]] constexpr float warpAmount(WarpMode mode, float knob) {
	const float k = std::clamp(knob, 0.0f, 1.0f);
	return warpIsBipolar(mode) ? (k * 2.0f - 1.0f) : k;
}

/// WHAT A WARP HANDS BACK: where to read, and which way up.
///
/// A SIGN AS WELL AS A PHASE, because one mode is not a phase warp at all. Flip is a polarity
/// inversion — Serum's own description is *"an instantaneous polarity flip"* — and forcing it into a
/// phase warp would be inventing a different effect and giving it Flip's name. One float carries it,
/// every other mode leaves it at 1, and the caller multiplies. That is cheaper than a second entry
/// point and it keeps all ten modes behind one call.
struct WarpRead {
	double phase = 0.0;
	float sign = 1.0f;
};

namespace detail {

/// THE ONE CURVE EVERY BEND AND ASYM IS BUILT FROM — a Mobius bend of the unit interval.
///
/// `x / (x + (1-x)·g)` is monotonic on [0,1] for any g > 0, fixed at both endpoints, and equal to the
/// identity at g = 1. One divide. It replaces `pow(x, e)`, which measured 10x more expensive for a
/// curve of the same family and no more useful shape.
[[nodiscard]] inline double mobius(double x, double g) {
	const double d = x + (1.0 - x) * g;
	return d > 1.0e-9 ? x / d : x;
}

/// HOW HARD A UNIT OF KNOB PULLS. 4.0 at full deflection is a strong but still musical bend — the
/// curve's tangent at the origin goes as 1/g, so this is a 4x speed-up at one end of the cycle
/// against a 4x slow-down at the other. Chosen to be tuned by ear at audition, like every mixing
/// coefficient in the flagship.
inline constexpr double kWarpGain = 3.0;

/// The gain for a signed amount: positive pulls one way, negative the other, 1.0 at zero.
[[nodiscard]] inline double gainFor(double amount) {
	return amount >= 0.0 ? 1.0 / (1.0 + kWarpGain * amount) : 1.0 - kWarpGain * amount;
}

} // namespace detail

/// WARP A READ PHASE.
///
/// @param mode    which shape
/// @param phase01 the phase to read at, already wrapped into [0,1)
/// @param knob    the warp knob, 0..1 as the parameter stores it
///
/// AT `Off`, OR AT A KNOB THAT MEANS NOTHING, THIS IS THE IDENTITY TO THE BIT — `{phase01, 1.0f}` —
/// which is what lets `FlagshipRecipe::operated` stay exactly false and the identity corner keep
/// coinciding with Morph. A warp that was merely *nearly* the identity at rest would have cost that.
[[nodiscard]] inline WarpRead warpRead(WarpMode mode, double phase01, float knob) {
	const double p = std::clamp(phase01, 0.0, 1.0);
	const double a = static_cast<double>(warpAmount(mode, knob));
	if (mode == WarpMode::Off || a == 0.0) {
		return {p, 1.0f};
	}

	switch (mode) {
	case WarpMode::BendPlus:
	case WarpMode::BendMinus:
	case WarpMode::BendBipolar: {
		// BEND — symmetric about the centre, so the cycle's midpoint and both edges stay put and the
		// two halves are bent as mirror images. That is what makes it read as a "pinch" rather than
		// as a shift: no part of the waveform moves past the middle.
		//
		// Bend + pulls the read toward the centre (the middle of the cycle is stretched over more
		// output phase); Bend - pushes it toward the edges.
		const double signedAmount = (mode == WarpMode::BendMinus) ? -a : a;
		const double u = 2.0 * p - 1.0;
		const double mag = detail::mobius(std::fabs(u), detail::gainFor(-signedAmount));
		return {0.5 + (u < 0.0 ? -mag : mag) * 0.5, 1.0f};
	}

	case WarpMode::AsymPlus:
	case WarpMode::AsymMinus:
	case WarpMode::AsymBipolar: {
		// ASYM — the whole cycle leans one way. Both edges stay put and the MIDDLE moves, which is
		// the difference from Bend: the waveform's features slide toward one end of the cycle
		// instead of gathering about its centre.
		const double signedAmount = (mode == WarpMode::AsymMinus) ? -a : a;
		return {detail::mobius(p, detail::gainFor(signedAmount)), 1.0f};
	}

	case WarpMode::Pwm: {
		// PWM — the duty cycle, as a phase warp, which is what pulse width IS on a wavetable: the
		// first half of the STORED cycle is read over the first `t` of the output cycle and the
		// second half over the rest. On a square that is literally pulse width; on anything else it
		// is the same gesture generalised, which is what Serum's PWM mode does to a non-square table.
		//
		// PIECEWISE LINEAR AND CONTINUOUS AT THE JOIN, by construction: both pieces meet at 0.5.
		// Monotonic for any t in (0,1), so it cannot fold.
		const double t = std::clamp(0.5 - 0.45 * a, 0.02, 0.98);
		return {p < t ? 0.5 * p / t : 0.5 + 0.5 * (p - t) / (1.0 - t), 1.0f};
	}

	case WarpMode::Flip: {
		// FLIP — a polarity inversion of the tail of the cycle, not a phase warp at all. The phase is
		// handed back untouched and the SIGN carries the effect.
		//
		// THE DISCONTINUITY IS THE POINT and it is also the cost: the waveform steps at `t`, which is
		// a wide harmonic burst. On this engine that burst is band-limited by the bake rather than
		// aliased, so it will sound DULLER here than in Serum — where the same edge is partly alias.
		// That difference is expected and should not be debugged.
		const double t = 1.0 - 0.5 * std::fabs(a);
		return {p, p >= t ? -1.0f : 1.0f};
	}

	case WarpMode::Mirror: {
		// MIRROR — read the cycle forward and then backward within one output cycle, so the result is
		// symmetric about its own midpoint. Deliberately NOT monotonic: a fold is the effect.
		//
		// Blended by the knob rather than switched, so the mode arrives continuously from the
		// identity instead of stepping the moment the knob leaves zero.
		const double folded = p < 0.5 ? 2.0 * p : 2.0 * (1.0 - p);
		return {p + std::fabs(a) * (folded - p), 1.0f};
	}

	case WarpMode::Off:
	case WarpMode::kCount:
	default:
		return {p, 1.0f};
	}
}

/// THE STEEPEST THE WARP EVER READS: max over the cycle of |d(read phase)/d(output phase)|.
///
/// A warp is a time-varying read RATE, so a partial k of the stored cycle sounds at up to k·f0 times this. A reader of a
/// band-limited table (Basic Shapes' mip ladders, backlog KV) picks its mip from the note's top frequency times this, so
/// the compressed half of the cycle cannot push a partial past the band limit. Derived per mode from warpRead above:
///
///   - Bend and Asym are the Mobius curve x/(x + (1−x)g), whose slope runs from 1/g at one end to g at the other, so the
///     steepest is max(g, 1/g) = 1 + kWarpGain·|amount| for either sign of gainFor.
///   - PWM reads half the cycle over `t` of the output and half over 1 − t: slopes 0.5/t and 0.5/(1 − t).
///   - Mirror's two halves are p + a(2p − p) and p + a(2 − 2p − p): slopes 1 + a and 1 − 3a.
///   - Flip moves no phase (its effect is a SIGN step, which no mip choice can band-limit).
///
/// EXACTLY 1.0 at Off and at a knob that means nothing, so a reader that multiplies by it is unchanged to the bit there.
///
/// ONE BOUND FOR THE WHOLE CYCLE, BY DESIGN: where a mode also reads SLOWLY (Asym, PWM), the duller mip
/// drops partials that would have fitted there. MEASURED: 4–6 dB less 3–12 kHz harmonic energy on Asym/PWM at depth 1 on
/// low and mid notes, accepted. Bend is unaffected (its jump sits in its fast region). A per-sample local-slope read was
/// measured as the alternative; see backlog KV.
[[nodiscard]] inline double warpMaxSlope(WarpMode mode, float knob) {
	const double a = std::fabs(static_cast<double>(warpAmount(mode, knob)));
	if (mode == WarpMode::Off || a == 0.0) {
		return 1.0;
	}
	switch (mode) {
	case WarpMode::BendPlus:
	case WarpMode::BendMinus:
	case WarpMode::BendBipolar:
	case WarpMode::AsymPlus:
	case WarpMode::AsymMinus:
	case WarpMode::AsymBipolar: return 1.0 + detail::kWarpGain * a;
	case WarpMode::Pwm: {
		const double t = std::clamp(0.5 - 0.45 * a, 0.02, 0.98);
		return 0.5 / std::min(t, 1.0 - t);
	}
	case WarpMode::Mirror: return std::max(1.0 + a, std::fabs(1.0 - 3.0 * a));
	case WarpMode::Flip:
	case WarpMode::Off:
	case WarpMode::kCount:
	default: return 1.0;
	}
}

} // namespace feathers
