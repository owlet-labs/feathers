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

// The decimation half of the oversampled voice path: 4x host rate down to host rate, through two
// cascaded polyphase-allpass halfband stages. OUR OWN CODE AND OUR OWN COEFFICIENTS (licence
// migration, 2026-09-24): this replaced a vendored GPL halfband, designed clean-room from the
// published method below and matched to the old one only by black-box measurement.
// Since 2026-09-25 it also holds the matching INTERPOLATOR (`HalfbandInterpolator2x`, the same structure
// transposed, on the same coefficients), which the dynamics devices' 4x oversampler
// (`fx/dyn/oversampler.h`) runs up with; the voice path still only decimates.
//
// THE STRUCTURE — the polyphase IIR halfband of
//   R. A. Valenzuela and A. G. Constantinides, "Digital signal processing schemes for efficient
//   interpolation and decimation", IEE Proceedings G, vol. 130, no. 6, pp. 225-235, Dec. 1983,
// as popularised for audio by Laurent de Soras' HIIR (public domain / WTFPL). The prototype is
//   H(z) = 1/2 · ( A0(z^2) + z^-1 · A1(z^2) ),
// where A0 and A1 are cascades of first-order allpasses in z^2, (a + z^-2) / (1 + a·z^-2). At the
// decimated rate each section is a first-order allpass in z^-1, so a stage costs one multiply per
// coefficient per OUTPUT sample and nothing is ever computed that the decimation throws away. The
// coefficients are sorted ascending and dealt alternately: 0, 2, 4, ... to A0 (which takes the later
// of each input pair) and 1, 3, 5, ... to A1 (the earlier).
//
// THE COEFFICIENTS — the elliptic-halfband design from the same paper, in the closed form HIIR's
// PolyphaseIir2Designer computes: for N coefficients and a transition half-width tbw (in units of
// the stage's input rate, centred on a quarter of it),
//   k  = tan((1 - 2·tbw)·π/4)^2,           q from k by the nome series e(1 + 2e^4 + 15e^8 + 150e^12),
//   ωi = q^(1/4) Σ (-1)^m q^(m(m+1)) sin((2m+1)·i·π/(2N+1)) / (1/2 + Σ (-1)^m q^(m^2) cos(2m·i·π/(2N+1))),
//   x  = sqrt((1 - ωi^2·k)(1 - ωi^2/k)) / (1 + ωi^2),   ai = (1 - x)/(1 + x),   i = 1..N.
// The literals below are that formula evaluated in double; tests/test_halfband.cpp re-derives them
// from it, so the provenance of every digit is checkable rather than asserted.
//
// THE TWO STAGES ARE DELIBERATELY DIFFERENT, which is where this is cheaper than a design that
// repeats one stage: the first stage (192 -> 96 kHz at a 48 kHz host) only has to keep content that
// would land below ~28 kHz at 96 kHz from folding, so its transition band is enormous and six
// coefficients buy ~112 dB; the second stage (96 -> 48 kHz) is the steep one and carries fourteen.
// MEASURED against the implementation it replaced, black box, at a 48 kHz host (tests pin the same):
//
//                                         old (vendored)      this
//   passband 0-20 kHz ripple              < 0.001 dB          < 0.001 dB (design: 1e-10 dB)
//   gain at 23.9 / 24.1 / 24.3 kHz        -0.14/-14.9/-47.5   -0.10/-16.3/-51.4 dB
//   worst image, input 24.5-96 kHz        -103.9 dB           -107.9 dB
//   impulse tail, 512 samples on          ~1e-5               ~1e-5 (same decay rate)
//   group delay at DC / 20 kHz (host)     2.23 / 7.77 smp     2.44 / 8.81 smp
//   cost per host-rate sample, -O3        ~81 ns              ~40 ns
//
// (Cost: alternating A/B, best of 15 rounds, on a heavily loaded shared host — the ratio is the
// number, not the nanoseconds. The old one was a stereo SIMD filter run with a silent right channel.)
// test_aliasing's broadband alias floors came out identical to the hundredth of a dB across the swap,
// as they must: those aliases fold against the 192 kHz internal rate before any decimator runs.
//
// A halfband filter's transition band is symmetric about a quarter of its input rate, which for the
// second stage means it is centred on 24 kHz and not on 20 kHz. Content between 24 and ~24.5 kHz
// therefore folds back into the top of the band only partly attenuated. That is inherent to the
// halfband structure rather than a shortfall of these particular coefficients, and it is why the
// alias test measures the audible band and the top of the band separately.

namespace feathers {

/// The two stages' coefficients, ascending. See the header comment for where every digit comes from.
///
/// Stage 1: N = 6, tbw = 0.12  (stopband from 0.37·fs_in: 71 kHz at 192 kHz; design -112.6 dB).
/// Stage 2: N = 14, tbw = 0.01075 (nominal edges 0.239 / 0.261·fs_in, 23.0 / 25.0 kHz at 96 kHz;
///          the realised response is flat to 23.8 kHz and -107.9 dB from 24.5 kHz on, because at
///          this order the design comfortably overshoots its nominal attenuation).
///
/// WHY NOT STEEPER, which the formula would happily give: the top coefficient sets the slowest
/// pole, and so how long the decimator rings after a change. tbw = 0.005 at the same order was
/// steeper still (-107 dB by 24.3 kHz) but its top coefficient was 0.994 against 0.989 here, and its
/// tail after a character crossfade measured ~7x the replaced filter's — test_osc_voice's "block
/// after the crossfade" case caught it at 1.3e-5 against a 1e-5 bound. This width was picked as the
/// widest that still holds -105 dB (in double) from 24.5 kHz, which is the fastest decay available at
/// that rejection; its ringing matches the old filter's.
inline constexpr int kHalfbandStage1Coefs = 6;
inline constexpr int kHalfbandStage2Coefs = 14;
inline constexpr double kHalfbandStage1Tbw = 0.12;
inline constexpr double kHalfbandStage2Tbw = 0.01075;
inline constexpr double kHalfbandStage1[kHalfbandStage1Coefs] = {
	0.034527243895164733, 0.13163507855039655, 0.27561532380260084,
	0.45007523683810918,  0.64646161470281949, 0.87042547098933665,
};
inline constexpr double kHalfbandStage2[kHalfbandStage2Coefs] = {
	0.026637291603891968, 0.10110477487731713, 0.20930793375227011, 0.33375065194319081,
	0.45874167964422874,  0.57336932305234878, 0.67196886413932466, 0.75304243095503443,
	0.81772790509480864,  0.86848388789866926, 0.9082118539884384,  0.93978358852323274,
	0.96585956250124672,  0.98889085400686361,
};

/// THE TWO ALLPASS BRANCHES BOTH DIRECTIONS SHARE: N first-order sections, coefficients dealt 0, 2, 4,
/// ... to branch A0 and 1, 3, 5, ... to A1, and one step that runs a sample through each branch. The
/// decimator and the interpolator differ only in what they feed the branches and what they make of
/// what comes out, so the section arithmetic lives here once (backlog JL) — a fix to it cannot reach
/// one direction and miss the other. Trivially copyable, like both of its users.
template <int N>
class HalfbandAllpassBranches {
public:
	static_assert(N >= 2 && N % 2 == 0, "coefficients are dealt in pairs to the two branches");

	explicit HalfbandAllpassBranches(const double (&coefs)[N]) {
		for (int i = 0; i < N; i++) {
			a_[i] = static_cast<float>(coefs[i]);
		}
		reset();
	}

	void reset() noexcept {
		for (int i = 0; i < N; i++) {
			x1_[i] = 0.0f;
			y1_[i] = 0.0f;
		}
	}

	/// Run `b0` through A0 and `b1` through A1, in place.
	void step(float& b0, float& b1) noexcept {
		// Each section is y = a·(x - y[-1]) + x[-1], the one-multiply form of (a + z^-1)/(1 + a·z^-1).
		for (int i = 0; i < N; i += 2) {
			const float y0 = (b0 - y1_[i]) * a_[i] + x1_[i];
			x1_[i] = b0;
			y1_[i] = y0;
			b0 = y0;
			const float y1 = (b1 - y1_[i + 1]) * a_[i + 1] + x1_[i + 1];
			x1_[i + 1] = b1;
			y1_[i + 1] = y1;
			b1 = y1;
		}
	}

private:
	float a_[N];
	float x1_[N];
	float y1_[N];
};

/// One 2:1 polyphase-allpass halfband decimator with N coefficients. Mono, float, no scratch, and
/// trivially copyable — the stereo voice adopts one by memcpy (OscVoice::beginStereoStream).
template <int N>
class HalfbandDecimator2x {
public:
	explicit HalfbandDecimator2x(const double (&coefs)[N]) : branches_(coefs) {}

	void reset() noexcept { branches_.reset(); }

	/// Consume one input pair (`earlier`, then `later`) and return one output sample.
	float process(float earlier, float later) {
		float b0 = later;
		float b1 = earlier;
		branches_.step(b0, b1);
		return 0.5f * (b0 + b1);
	}

private:
	HalfbandAllpassBranches<N> branches_;
};

/// One 1:2 polyphase-allpass halfband INTERPOLATOR with N coefficients: the decimator above, transposed.
/// Mono, float, no scratch, trivially copyable, and one input sample at a time, so it takes any block
/// length and carries nothing between calls but its allpass state.
///
/// THE SAME STRUCTURE READ THE OTHER WAY (Valenzuela & Constantinides 1983, as above; HIIR's Upsampler2x
/// is the same thing). Zero-stuffing x to the doubled rate and filtering it by
///   2 · H(z) = A0(z^2) + z^-1 · A1(z^2)
/// needs no multiply by zero: A0 and A1 run at the INPUT rate on the same sample, and because the z^-1
/// sits on A1, A0's output is the EARLIER of the two output samples and A1's the later. The dealing is
/// the decimator's (coefficients 0, 2, 4, ... to A0; 1, 3, 5, ... to A1), so one coefficient table
/// designs both directions. THE GAIN IS ONE, not the decimator's half: zero-stuffing halves the passband
/// level, and 2·H restores it. `fx-dyn` measures both facts rather than trusting this paragraph: unity at
/// 1 kHz, images rejected at fs - f, and each of a swapped order, a dropped branch and a halved gain red.
template <int N>
class HalfbandInterpolator2x {
public:
	explicit HalfbandInterpolator2x(const double (&coefs)[N]) : branches_(coefs) {}

	void reset() noexcept { branches_.reset(); }

	/// Consume one input sample and produce two output samples, `earlier` then `later`.
	void process(float in, float& earlier, float& later) {
		float b0 = in;
		float b1 = in;
		branches_.step(b0, b1);
		earlier = b0;
		later = b1;
	}

private:
	HalfbandAllpassBranches<N> branches_;
};

/// Two cascaded halfband decimators: 4x -> 2x -> 1x.
class Halfband4x {
public:
	/// Largest host-rate block one decimate() call accepts, and the reason callers have to chunk.
	/// The filter itself has no scratch and would take any length; this stays 32 because the voice's
	/// oversampled buffer, kRenderChunk and the vox bake interval are all sized from it.
	static constexpr int kMaxBlock = 32;

	/// Host-rate samples decimate() insists on working in whole multiples of.
	///
	/// NOT a limit of this filter, which produces one output from every four inputs and could take
	/// any n. It is a CONTRACT the voice is built on and was kept unchanged when the filter was: a
	/// caller asked for a block that is not a multiple of this has to generate the round-up and hold
	/// the surplus, so the surplus a voice can be carrying — how far its rendering runs ahead of its
	/// caller's, and the instant at which a character change lands (generated position is always
	/// ceil4(consumed)) — is fixed by this number. Anything reasoning about that lead time should
	/// derive it from here rather than restate the four. Changing it is a separate decision that moves
	/// those schedules, not a free simplification.
	static constexpr int kGroup = 4;
	static_assert(kMaxBlock % kGroup == 0,
	              "rounding a block up to a whole group must never overshoot the chunk limit");

	/// Most host-rate samples a caller can be holding after a decimate() that rounded up.
	static constexpr int kMaxCarry = kGroup - 1;

	Halfband4x() = default;

	/// Clear both stages' state.
	void reset() {
		stage1_.reset();
		stage2_.reset();
	}

	/// Decimate `4 * n` oversampled samples down to `n`.
	///
	/// `n` must be a multiple of kGroup and at most kMaxBlock (see kGroup for why the first bound is
	/// kept). `in` and `out` may be unaligned and may not overlap.
	void decimate(const float* in, float* out, int n);

private:
	HalfbandDecimator2x<kHalfbandStage1Coefs> stage1_ {kHalfbandStage1};
	HalfbandDecimator2x<kHalfbandStage2Coefs> stage2_ {kHalfbandStage2};
};

} // namespace feathers
