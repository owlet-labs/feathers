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

// The two per-sample modifiers the PhiMorph character layer carries: phase jitter and
// amplitude-dependent noise. Both are zone-derived — most zone positions have them at zero — and both
// live in the render path rather than the table, which is why the character golden captures the
// fields but nothing before this file ever read them.
//
// The original fixed-point Morph renderer (since removed) applied them in Q31. The arithmetic there is compact enough
// to be opaque, so the scale factors below are the reduced form of it, derived once:
//
//   jitterRange = phaseJitter * 0.02 * 2^32, and the jitter added to the lookup phase is
//   (noise * jitterRange >> 32) << 1 — with `noise` the whole state word read as int32, i.e. u in
//   [-1,1). That is u * phaseJitter * 0.02 of a full 2^32 cycle, so ±2% of a cycle at the field's
//   maximum and 4% peak to peak. (The header's "~5%" is loose; the arithmetic says 4%.)
//
//   noiseContrib = ((noise * ampNoiseQ31 >> 32) * |waveform| >> 32) << 2, which reduces to exactly
//   u * ampNoise * |waveform| — the two shifts undo the two headroom halvings the >>32 pair
//   introduces. Scaling by |waveform| is what keeps zero crossings clean: the grit vanishes with the
//   signal instead of sitting under it as a constant hiss.
//
// Neither is applied here to the phase ACCUMULATOR. The firmware jitters `evalPhase`, a copy, so the
// oscillator's pitch is untouched and only the point the waveform is sampled at moves.

namespace feathers {

/// The firmware's noise source, algorithm for algorithm: the Numerical Recipes LCG that
/// The original fixed-point Morph renderer stepped inline (`noiseState * 1664525 + 1013904223`), read as a signed 32-bit word.
///
/// Copied rather than improved on, because a different generator would be a different sound: this
/// noise is shaped by |waveform| and added straight to the signal, so the LCG's own spectral
/// character — the lattice structure a linear congruential sequence has in its high bits — is part of
/// what the hardware sounds like.
///
/// Not, as an earlier revision of this comment claimed, because the LCG's poor low bits are audible.
/// They cannot be, for most of the range: a float carries a 24-bit significand against the state
/// word's 31 bits of magnitude, so the conversion rounds away everything below one unit in the last
/// place, which is 2^(e-23) for a state in [2^e, 2^(e+1)). Below 2^24 that is nothing at all and
/// every state converts exactly; from 2^24 consecutive states start collapsing onto the same float,
/// in pairs to begin with and in fours by 2^25; only in the top octave are seven or eight low bits
/// gone. The firmware's own `>>32` after its multiply keeps a comparable top slice. Both paths
/// therefore draw on the same portion of the word, which is why copying the generator gets the
/// character right, and neither resolves the bottom of it.
///
/// One instance per voice, seeded explicitly. The firmware reseeds from the phase accumulator at the
/// top of every render call, which we cannot copy: it would make the noise a function of where the
/// host chose to split its blocks, and block size is not allowed to change a sample here. A voice's
/// stream therefore runs free from its seed instead, which is also what makes two voices given the
/// same seed render bit-identically.
class CharacterNoise {
public:
	/// The firmware's reseed constant, reused as our starting state so an untouched voice begins
	/// somewhere the hardware also visits. Its low 32 bits: the original fixed-point Morph renderer wrote
	/// `phase ^ 0x5DEECE66D` into a uint32, so the top three bits of that 35-bit literal never reach
	/// the generator there either.
	static constexpr uint32_t kDefaultSeed = 0xDEECE66Du;

	explicit CharacterNoise(uint32_t seed = kDefaultSeed) : state_{seed} {}

	void seed(uint32_t s) { state_ = s; }

	/// Uniform on [-1, 1] — closed at both ends, see kToUnit. Uniform, not Gaussian: the firmware takes
	/// the raw state word, and the difference between a flat and a bell-shaped noise is audible at
	/// these depths.
	float next() {
		state_ = state_ * 1664525u + 1013904223u;
		return static_cast<float>(static_cast<int32_t>(state_)) * kToUnit;
	}

private:
	// 2^31, not 2^31-1: int32 spans [-2^31, 2^31-1], so this keeps the spacing even.
	//
	// The range it produces is [-1, 1] closed, not the [-1, 1) the arithmetic suggests. Float spacing
	// at 2^31 is 256, so static_cast<float>(INT32_MAX) rounds UP to 2^31 and the product is exactly
	// 1.0f — 64 of the 2^32 states do this, about one draw in 67 million. (65 land on -1.0f, which is
	// the legitimate endpoint anyway.) Harmless where it lands: applyAmpNoise clamps, and a jitter
	// offset at its exact peak is a phase like any other. Stated because "[-1, 1)" written as a
	// half-open range is the kind of claim a later reader would build a bound on.
	static constexpr float kToUnit = 1.0f / 2147483648.0f;

	uint32_t state_;
};

/// Peak phase excursion at phaseJitter = 1, in cycles. See the derivation above.
inline constexpr double kPhaseJitterCycles = 0.02;

/// How far to move the lookup phase this sample, in cycles. `u` is one draw from CharacterNoise.
inline double jitterOffsetCycles(float amount, float u) {
	return kPhaseJitterCycles * static_cast<double>(amount) * static_cast<double>(u);
}

/// Grit proportional to the instantaneous level, clipped at the rail the hardware's add_saturate
/// clips at. `u` is one draw.
///
/// `amount` is the table's ampNoise, which reaches 0.25 and not the 0.5 phi_morph.hpp:202 advertises:
/// the builder multiplies its unipolar triangle by 0.25 at phi_morph.cpp:180, and sweeping all 720
/// zones across 200 phase offsets tops out at 0.249997. The firmware's comment is the stale one, the
/// same way its "~5% of cycle" describes jitter arithmetic that actually gives 2%.
inline float applyAmpNoise(float sample, float amount, float u) {
	const float grit = u * amount * std::abs(sample);
	return std::clamp(sample + grit, -1.0f, 1.0f);
}

} // namespace feathers
