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

// THE ADDITIVE ENGINE'S ROTATOR BANK: see partial_bank.h for what it promises. The per-sample kernel is the one
// measured in the additive engine's voice benchmark (`render`), with the single amplitude
// split into per-ear ramps and the ramp's final sample snapped onto its target. setTargets is vectorised too, four
// partials per pass, so the control block costs a fraction of the samples it governs (tests/bench_partial_bank.cpp).

#include "dsp/partial_bank.h"

#include <cassert>
#include <cmath>
#include <limits>
#include <numbers>

// FEATHERS_FORCE_SIMDE takes the arm64 road on x86-64 too, so scripts/clang-preflight.sh compiles what macOS does.
#if !defined(FEATHERS_FORCE_SIMDE)                                                                        \
    && (defined(__SSE2__) || defined(_M_AMD64) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2))
#include <emmintrin.h>
#else
#define SIMDE_ENABLE_NATIVE_ALIASES
#include "simde/x86/sse2.h"
#endif

namespace feathers::partials {

namespace {

constexpr float kPiF = std::numbers::pi_v<float>;
constexpr float kFadeA = kFadeStart * kPiF;
constexpr float kFadeB = kFadeEnd * kPiF;
constexpr float kFadeInvSpan = 1.0f / (kFadeB - kFadeA);

/// sin and cos of w in [0, π], four at a time. Reduced to |x| <= π/4 about the nearest of 0, π/2, π, then Taylor
/// polynomials whose truncation is below 2e-9 there. π/2 is split kHi + kLo: q·kHi is exact for q in {0, 1, 2}, and
/// w − q·kHi is exact by Sterbenz's lemma (for q >= 1, w lies within a factor of two of q·kHi), so the only rounding
/// in the reduction is the tiny q·kLo term. Reducing about 0 for low partials keeps sin w RELATIVELY accurate when w
/// is small, which is what holds a low partial's frequency: the bench's single expansion about π/2 cancels there.
inline void sincosPs(__m128 w, __m128& s, __m128& c) {
	// q = round(w·2/π) by explicit rounding (w >= 0), not the MXCSR mode: 0, 1 or 2.
	const __m128i q = _mm_cvttps_epi32(_mm_add_ps(_mm_mul_ps(w, _mm_set1_ps(2.0f / kPiF)), _mm_set1_ps(0.5f)));
	const __m128 qf = _mm_cvtepi32_ps(q);
	const __m128 x = _mm_sub_ps(_mm_sub_ps(w, _mm_mul_ps(qf, _mm_set1_ps(1.57079601287841796875f))),
	                            _mm_mul_ps(qf, _mm_set1_ps(3.13916473e-07f)));
	const __m128 x2 = _mm_mul_ps(x, x);
	// sin x = x − x³/3! + x⁵/5! − x⁷/7! + x⁹/9!
	__m128 ps = _mm_set1_ps(1.0f / 362880.0f);
	ps = _mm_add_ps(_mm_mul_ps(ps, x2), _mm_set1_ps(-1.0f / 5040.0f));
	ps = _mm_add_ps(_mm_mul_ps(ps, x2), _mm_set1_ps(1.0f / 120.0f));
	ps = _mm_add_ps(_mm_mul_ps(ps, x2), _mm_set1_ps(-1.0f / 6.0f));
	const __m128 s0 = _mm_add_ps(x, _mm_mul_ps(_mm_mul_ps(ps, x2), x));
	// cos x = 1 − x²/2! + x⁴/4! − x⁶/6! + x⁸/8! − x¹⁰/10!
	__m128 pc = _mm_set1_ps(-1.0f / 3628800.0f);
	pc = _mm_add_ps(_mm_mul_ps(pc, x2), _mm_set1_ps(1.0f / 40320.0f));
	pc = _mm_add_ps(_mm_mul_ps(pc, x2), _mm_set1_ps(-1.0f / 720.0f));
	pc = _mm_add_ps(_mm_mul_ps(pc, x2), _mm_set1_ps(1.0f / 24.0f));
	pc = _mm_add_ps(_mm_mul_ps(pc, x2), _mm_set1_ps(-0.5f));
	const __m128 c0 = _mm_add_ps(_mm_set1_ps(1.0f), _mm_mul_ps(pc, x2));
	// Quadrant: q = 0 -> (s0, c0); q = 1 -> (c0, −s0); q = 2 -> (−s0, −c0).
	const __m128 q1 = _mm_castsi128_ps(_mm_cmpeq_epi32(q, _mm_set1_epi32(1)));
	const __m128 q2 = _mm_castsi128_ps(_mm_cmpeq_epi32(q, _mm_set1_epi32(2)));
	const __m128 sign = _mm_set1_ps(-0.0f);
	const __m128 negS0 = _mm_xor_ps(s0, sign), negC0 = _mm_xor_ps(c0, sign);
	s = _mm_or_ps(_mm_andnot_ps(q1, _mm_or_ps(_mm_and_ps(q2, negS0), _mm_andnot_ps(q2, s0))), _mm_and_ps(q1, c0));
	c = _mm_or_ps(_mm_andnot_ps(q1, _mm_or_ps(_mm_and_ps(q2, negC0), _mm_andnot_ps(q2, c0))), _mm_and_ps(q1, negS0));
}

inline __m128 select(__m128 mask, __m128 a, __m128 b) {
	return _mm_or_ps(_mm_and_ps(mask, a), _mm_andnot_ps(mask, b));
}

/// nyquistFade, four lanes, the same arithmetic: (b − w)·(1/(b − a)) clamped to [0, 1], exactly 1 at or below a,
/// and 0 for w <= 0 or NaN. The max takes zero as its SECOND operand, which SSE returns when either is NaN.
inline __m128 fadePs(__m128 w) {
	const __m128 zero = _mm_setzero_ps(), one = _mm_set1_ps(1.0f);
	const __m128 line = _mm_mul_ps(_mm_sub_ps(_mm_set1_ps(kFadeB), w), _mm_set1_ps(kFadeInvSpan));
	const __m128 f = _mm_min_ps(_mm_max_ps(line, zero), one);
	return _mm_and_ps(select(_mm_cmple_ps(w, _mm_set1_ps(kFadeA)), one, f), _mm_cmpgt_ps(w, zero));
}

/// Exactly zero below the silence floor and for anything not finite (NaN, ±inf); the value otherwise.
inline __m128 flooredPs(__m128 v) {
	const __m128 mag = _mm_andnot_ps(_mm_set1_ps(-0.0f), v);
	const __m128 keep = _mm_and_ps(_mm_cmpge_ps(mag, _mm_set1_ps(kSilenceFloor)),
	                               _mm_cmplt_ps(mag, _mm_set1_ps(std::numeric_limits<float>::infinity())));
	return _mm_and_ps(v, keep);
}

/// Lanes k..k+3 of p, those at or past `count` read as zero (and never read from p).
inline __m128 load4(const float* p, int k, int count) {
	if (k + 4 <= count)
		return _mm_loadu_ps(p + k);
	alignas(16) float t[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	for (int j = 0; k + j < count && j < 4; ++j)
		t[j] = p[k + j];
	return _mm_load_ps(t);
}

/// All-ones in lanes k..k+3 that are below `count`.
inline __m128 namedLanes(int k, int count) {
	const __m128i lane = _mm_add_epi32(_mm_set_epi32(3, 2, 1, 0), _mm_set1_epi32(k));
	return _mm_castsi128_ps(_mm_cmplt_epi32(lane, _mm_set1_epi32(count)));
}

/// e^{i·2π·a}, four lanes, normalised: `a` in turns, clamped at ±2^22 (where a float has no fraction left) and reduced
/// to the nearest whole turn first, by explicit floor(a + 0.5) rather than the MXCSR mode — truncate, then step down
/// where truncation went up (a negative argument). The fraction left is in [−0.5, 0.5]. A zero `a` is exactly (0, 1).
inline void turnPs(__m128 a, __m128& s, __m128& c) {
	const __m128 limit = _mm_set1_ps(4194304.0f); // 2^22 turns
	const __m128 one = _mm_set1_ps(1.0f), sign = _mm_set1_ps(-0.0f), twoPi = _mm_set1_ps(2.0f * kPiF);
	a = _mm_min_ps(_mm_max_ps(a, _mm_sub_ps(_mm_setzero_ps(), limit)), limit);
	const __m128 r = _mm_add_ps(a, _mm_set1_ps(0.5f));
	__m128 t = _mm_cvtepi32_ps(_mm_cvttps_epi32(r));
	t = _mm_sub_ps(t, _mm_and_ps(_mm_cmpgt_ps(t, r), one));
	const __m128 frac = _mm_sub_ps(a, t);
	const __m128 neg = _mm_and_ps(frac, sign);
	sincosPs(_mm_mul_ps(_mm_andnot_ps(sign, frac), twoPi), s, c); // the angle's magnitude, in [0, π]
	s = _mm_xor_ps(s, neg);
	const __m128 inv = _mm_div_ps(one, _mm_sqrt_ps(_mm_add_ps(_mm_mul_ps(c, c), _mm_mul_ps(s, s))));
	s = _mm_mul_ps(s, inv);
	c = _mm_mul_ps(c, inv);
}

} // namespace

float nyquistFade(float increment) {
	if (!(increment > 0.0f)) // zero, negative or NaN: no partial
		return 0.0f;
	if (increment <= kFadeA)
		return 1.0f;
	if (increment >= kFadeB)
		return 0.0f;
	return (kFadeB - increment) * kFadeInvSpan;
}

void PartialBank::clearState() {
	for (int k = 0; k < kMaxPartials; ++k) {
		cr_[k] = 1.0f;
		ci_[k] = 0.0f;
		ampL_[k] = dampL_[k] = tgtL_[k] = 0.0f;
		ampR_[k] = dampR_[k] = tgtR_[k] = 0.0f;
	}
	live_ = 0;
	first_ = 0;
	rampLeft_ = 0;
	stereo_ = false;
	snap_ = false;
}

void PartialBank::reset(int partialCount) {
	clearState();
	const long long n = partialCount > 1 ? partialCount : 1;
	for (int k = 0; k < kMaxPartials; ++k) {
		// φ_k = π·k²/N, reduced mod 2π exactly in integers first: k² mod 2N.
		const long long m = (static_cast<long long>(k) * k) % (2LL * n);
		const double phi = std::numbers::pi * static_cast<double>(m) / static_cast<double>(n);
		re_[k] = static_cast<float>(std::cos(phi));
		im_[k] = static_cast<float>(std::sin(phi));
	}
}

void PartialBank::reset(const float* power, int count) {
	if (count > kMaxPartials)
		count = kMaxPartials;
	double total = 0.0;
	for (int k = 0; k < count; ++k)
		if (power[k] > 0.0f && std::isfinite(power[k]))
			total += static_cast<double>(power[k]);
	if (!(total > 0.0) || !std::isfinite(total)) {
		reset(count); // no power to weight by: the flat law
		return;
	}
	clearState();
	// φ_k = −2π·Σ_{l<k} (k − l)·p_l = −2π·(k·S0 − S1), S0 = Σ_{l<k} p_l and S1 = Σ_{l<k} l·p_l, in TURNS reduced before the
	// sincos (k·S0 reaches a few hundred turns; double keeps the fraction to ~1e-13). Only the groups `count` names: a
	// partial past them is silent, and keeps whatever phase it had until a later block names it (header).
	const int named = (count + 3) & ~3;
	double s0 = 0.0, s1 = 0.0;
	for (int k = 0; k < named; ++k) {
		double turns = static_cast<double>(k) * s0 - s1;
		turns -= std::floor(turns);
		const double phi = -2.0 * std::numbers::pi * turns;
		re_[k] = static_cast<float>(std::cos(phi));
		im_[k] = static_cast<float>(std::sin(phi));
		if (k < count && power[k] > 0.0f && std::isfinite(power[k])) {
			const double p = static_cast<double>(power[k]) / total;
			s0 += p;
			s1 += static_cast<double>(k) * p;
		}
	}
}

void PartialBank::resetPhases(const float* phase, int count) {
	if (count > kMaxPartials)
		count = kMaxPartials;
	if (count < 0)
		count = 0;
	clearState();
	const int named = (count + 3) & ~3;
	for (int k = 0; k < named; ++k) {
		const double phi = k < count && std::isfinite(phase[k]) ? static_cast<double>(phase[k]) : 0.0;
		re_[k] = static_cast<float>(std::cos(phi));
		im_[k] = static_cast<float>(std::sin(phi));
	}
}

void PartialBank::setTargets(const float* increment, const float* targetL, const float* targetR, int count,
                             int frames, bool stereo, const float* bendRatio, float bend) {
	assert(!stereo || targetR != nullptr);
	if (count < 0)
		count = 0;
	if (count > kMaxPartials)
		count = kMaxPartials;
	// In mono the R ear IS the L ear: bring R to where L is before anything ramps from it.
	if (!stereo_)
		for (int k = 0; k < live_; k += 4)
			_mm_store_ps(ampR_ + k, _mm_load_ps(ampL_ + k));
	stereo_ = stereo;
	ear_ = false; // a block's own ear phase, asked after this (setEarPhase), or none


	// One pass, four partials at a time, over every group the caller named or that is still sounding:
	//   - targets: the caller's through the Nyquist fade and the silence floor, zero past `count` (a ramp-out);
	//   - live_: the highest group sounding or about to, in either ear;
	//   - coefficients: the rotation by each named partial's increment, normalised to |c| = 1 so the drift does not
	//     depend on how often this is called; a partial past `count` keeps the one it had. The increment is clamped
	//     to [0, π] for the sincos (outside it the fade has already silenced the partial).
	const int oldLive = live_;
	const int named = (count + 3) & ~3;
	const int span = named > oldLive ? named : oldLive;
	const __m128 zero = _mm_setzero_ps(), pi = _mm_set1_ps(kPiF), bendV = _mm_set1_ps(bend);
	const __m128 twoPiV = _mm_set1_ps(2.0f * kPiF);
	int live = 0;
	int first = -1; // the lowest group sounding or about to: the groups below it render nothing
	for (int k = 0; k < span; k += 4) {
		const __m128 w = load4(increment, k, count);
		const __m128 fade = fadePs(w);
		const __m128 l = flooredPs(_mm_mul_ps(load4(targetL, k, count), fade));
		const __m128 r = stereo ? flooredPs(_mm_mul_ps(load4(targetR, k, count), fade)) : l;
		_mm_store_ps(tgtL_ + k, l);
		_mm_store_ps(tgtR_ + k, r);
		const __m128 nz = _mm_or_ps(_mm_or_ps(_mm_cmpneq_ps(l, zero), _mm_cmpneq_ps(r, zero)),
		                            _mm_or_ps(_mm_cmpneq_ps(_mm_load_ps(ampL_ + k), zero),
		                                      _mm_cmpneq_ps(_mm_load_ps(ampR_ + k), zero)));
		if (_mm_movemask_ps(nz) != 0) {
			live = k + 4;
			first = first < 0 ? k : first;
		}
		if (k < count) {
			// The rotator's angle: the increment, plus the Phase control's ramp when there is one (the fade above stayed
			// the un-bent increment's).
			__m128 s, c;
			if (bendRatio == nullptr) {
				sincosPs(_mm_min_ps(_mm_max_ps(w, zero), pi), s, c);
			}
			else {
				// A bent angle past π (a partial up in the Nyquist fade, bent further up) is the same rotation as that
				// angle less 2π: wrapped, then the sincos of its magnitude with the sign put back on the sine. So the
				// phase at the block's end is exact for every partial; only a faded one's slope inside the block aliases.
				__m128 turn = _mm_add_ps(w, _mm_mul_ps(load4(bendRatio, k, count), bendV));
				// AND BELOW −π: a deep downward Shift gives its silent low partials ratios more negative than the top partial's is
				// positive, so their bent angle can pass −π the same way (spectral-ops Shift review, 2026-10-12).
				turn = select(_mm_cmpgt_ps(turn, pi), _mm_sub_ps(turn, twoPiV), turn);
				turn = select(_mm_cmplt_ps(turn, _mm_sub_ps(zero, pi)), _mm_add_ps(turn, twoPiV), turn);
				const __m128 sign = _mm_and_ps(turn, _mm_set1_ps(-0.0f));
				sincosPs(_mm_min_ps(_mm_andnot_ps(_mm_set1_ps(-0.0f), turn), pi), s, c);
				s = _mm_xor_ps(s, sign);
			}
			const __m128 inv = _mm_div_ps(_mm_set1_ps(1.0f), _mm_sqrt_ps(_mm_add_ps(_mm_mul_ps(c, c), _mm_mul_ps(s, s))));
			s = _mm_mul_ps(s, inv);
			c = _mm_mul_ps(c, inv);
			if (k + 4 > count) { // the ragged last group: only the named lanes move
				const __m128 m = namedLanes(k, count);
				s = select(m, s, _mm_load_ps(ci_ + k));
				c = select(m, c, _mm_load_ps(cr_ + k));
			}
			_mm_store_ps(ci_ + k, s);
			_mm_store_ps(cr_ + k, c);
		}
	}
	live_ = live;
	first_ = first < 0 ? 0 : first;

	// Ramps, or the snap. And renormalise the rotators: z ·= 1.5 − 0.5·|z|² (bench_additive_voice.cpp's step).
	const bool snap = snap_ || frames <= 0;
	snap_ = false;
	rampLeft_ = snap ? 0 : frames;
	const __m128 inv = _mm_set1_ps(snap ? 0.0f : 1.0f / static_cast<float>(frames));
	const __m128 half = _mm_set1_ps(0.5f), threeHalves = _mm_set1_ps(1.5f);
	for (int k = 0; k < live_; k += 4) {
		const __m128 tl = _mm_load_ps(tgtL_ + k), tr = _mm_load_ps(tgtR_ + k);
		if (snap) {
			_mm_store_ps(ampL_ + k, tl);
			_mm_store_ps(ampR_ + k, tr);
			_mm_store_ps(dampL_ + k, zero);
			_mm_store_ps(dampR_ + k, zero);
		}
		else {
			_mm_store_ps(dampL_ + k, _mm_mul_ps(_mm_sub_ps(tl, _mm_load_ps(ampL_ + k)), inv));
			_mm_store_ps(dampR_ + k, _mm_mul_ps(_mm_sub_ps(tr, _mm_load_ps(ampR_ + k)), inv));
		}
		const __m128 r = _mm_load_ps(re_ + k), i = _mm_load_ps(im_ + k);
		const __m128 g = _mm_sub_ps(threeHalves, _mm_mul_ps(half, _mm_add_ps(_mm_mul_ps(r, r), _mm_mul_ps(i, i))));
		_mm_store_ps(re_ + k, _mm_mul_ps(r, g));
		_mm_store_ps(im_ + k, _mm_mul_ps(i, g));
	}
	if (snap)
		trimLive();
	// THE GROUPS BELOW first_ ARE NOT RENDERED, BUT THEIR PHASE STILL MOVES: render skips them (they are silent in both
	// ears), so the block's whole turn is applied to their rotators here, once — each partial's angle (its coefficient,
	// the Phase control's bend included) times the block's length, reduced to (−π, π] — and the phase at the block's end is
	// exact for EVERY partial, as the wrap above promises (E/F/G re-review N-1). One complex multiply per partial.
	if (first_ > 0 && frames > 0) {
		constexpr double kTwoPi = 2.0 * 3.14159265358979323846;
		for (int k = 0; k < first_; ++k) {
			const double turn = std::atan2(static_cast<double>(ci_[k]), static_cast<double>(cr_[k]));
			const double angle = turn * static_cast<double>(frames);
			const double a = angle - kTwoPi * std::nearbyint(angle / kTwoPi);
			const float cs = static_cast<float>(std::cos(a));
			const float sn = static_cast<float>(std::sin(a));
			const float r = re_[k];
			re_[k] = r * cs - im_[k] * sn;
			im_[k] = r * sn + im_[k] * cs;
		}
	}
}

void PartialBank::trimLive() {
	// Amplitudes are on their targets: live_ shrinks to the highest group with a nonzero target in either ear.
	while (live_ > 0) {
		const int k = live_ - 4;
		const __m128 z = _mm_setzero_ps();
		const __m128 nz = _mm_or_ps(_mm_cmpneq_ps(_mm_load_ps(tgtL_ + k), z), _mm_cmpneq_ps(_mm_load_ps(tgtR_ + k), z));
		if (_mm_movemask_ps(nz) != 0)
			break;
		live_ = k;
	}
	// AND FROM BELOW: a downward Shift's silent low partials leave the groups they fill (render skips them).
	while (first_ < live_) {
		const __m128 z = _mm_setzero_ps();
		const __m128 nz =
		    _mm_or_ps(_mm_cmpneq_ps(_mm_load_ps(tgtL_ + first_), z), _mm_cmpneq_ps(_mm_load_ps(tgtR_ + first_), z));
		if (_mm_movemask_ps(nz) != 0)
			break;
		first_ += 4;
	}
	if (first_ >= live_)
		first_ = 0;
}

template <bool kStereo, bool kRamp, bool kEar>
void PartialBank::renderSpan(float* outL, float* outR, int frames, bool snapLast) {
	static_assert(kStereo || !kEar, "an ear phase is a stereo block's");
	const int n4 = live_ / 4;
	// FROM THE LOWEST SOUNDING GROUP: under a downward Shift the partials at or below 0 Hz fill the groups below it, and
	// their amplitudes are 0 in both ears, so they are not turned (they resume where they stopped, as those above live_ do).
	const int v0 = first_ / 4;
	[[maybe_unused]] EarRotators* ear = kEar ? earRot_.get() : nullptr;
	for (int s = 0; s < frames; ++s) {
		const bool last = kRamp && snapLast && s == frames - 1;
		__m128 accL = _mm_setzero_ps(), accR = _mm_setzero_ps();
		for (int v = v0; v < n4; ++v) {
			float* pr = re_ + 4 * v;
			float* pi = im_ + 4 * v;
			const __m128 r = _mm_load_ps(pr), i = _mm_load_ps(pi);
			const __m128 c = _mm_load_ps(cr_ + 4 * v), d = _mm_load_ps(ci_ + 4 * v);
			const __m128 ni = _mm_add_ps(_mm_mul_ps(r, d), _mm_mul_ps(i, c));
			_mm_store_ps(pr, _mm_sub_ps(_mm_mul_ps(r, c), _mm_mul_ps(i, d)));
			_mm_store_ps(pi, ni);
			// The amplitude steps BEFORE it is applied, so the ramp's last sample carries the target itself.
			__m128 aL = _mm_load_ps(ampL_ + 4 * v);
			if constexpr (kRamp) {
				aL = last ? _mm_load_ps(tgtL_ + 4 * v) : _mm_add_ps(aL, _mm_load_ps(dampL_ + 4 * v));
				_mm_store_ps(ampL_ + 4 * v, aL);
			}
			accL = _mm_add_ps(accL, _mm_mul_ps(aL, ni));
			if constexpr (kStereo) {
				__m128 aR = _mm_load_ps(ampR_ + 4 * v);
				if constexpr (kRamp) {
					aR = last ? _mm_load_ps(tgtR_ + 4 * v) : _mm_add_ps(aR, _mm_load_ps(dampR_ + 4 * v));
					_mm_store_ps(ampR_ + 4 * v, aR);
				}
				if constexpr (kEar) {
					// THE RIGHT EAR'S OWN ROTATOR (setEarPhase): the same complex multiply, by its own coefficient.
					float* qr = ear->re + 4 * v;
					float* qi = ear->im + 4 * v;
					const __m128 rr = _mm_load_ps(qr), ir = _mm_load_ps(qi);
					const __m128 cR = _mm_load_ps(ear->cr + 4 * v), dR = _mm_load_ps(ear->ci + 4 * v);
					const __m128 niR = _mm_add_ps(_mm_mul_ps(rr, dR), _mm_mul_ps(ir, cR));
					_mm_store_ps(qr, _mm_sub_ps(_mm_mul_ps(rr, cR), _mm_mul_ps(ir, dR)));
					_mm_store_ps(qi, niR);
					accR = _mm_add_ps(accR, _mm_mul_ps(aR, niR));
				}
				else {
					accR = _mm_add_ps(accR, _mm_mul_ps(aR, ni));
				}
			}
		}
		alignas(16) float t4[4];
		_mm_store_ps(t4, accL);
		outL[s] += t4[0] + t4[1] + t4[2] + t4[3];
		if constexpr (kStereo) {
			_mm_store_ps(t4, accR);
			outR[s] += t4[0] + t4[1] + t4[2] + t4[3];
		}
	}
}

void PartialBank::render(float* outL, float* outR, int frames) {
	if (frames <= 0)
		return;
	if (live_ == 0) {
		rampLeft_ = 0;
		return;
	}
	// The ramp's remaining samples (its last one snapped onto the target), then steady amplitudes.
	const int ramped = frames < rampLeft_ ? frames : rampLeft_;
	const bool ends = ramped > 0 && ramped == rampLeft_;
	if (ramped > 0) {
		renderSpans(outL, outR, ramped, true, ends);
		rampLeft_ -= ramped;
	}
	if (ends) {
		for (int k = 0; k < live_; k += 4) {
			_mm_store_ps(dampL_ + k, _mm_setzero_ps());
			_mm_store_ps(dampR_ + k, _mm_setzero_ps());
		}
		// In mono only L ramped, and R's target IS L's: land R on it too, so R never holds what L does not. Without
		// this a partial left sounding in R by a stereo -> mono switch keeps its R amplitude past trimLive, revives
		// live_ at zero amplitude, and ramps from that stale value when stereo returns.
		if (!stereo_)
			for (int k = 0; k < live_; k += 4)
				_mm_store_ps(ampR_ + k, _mm_load_ps(tgtR_ + k));
		trimLive();
	}
	const int steady = frames - ramped;
	if (steady > 0 && live_ > 0) {
		renderSpans(outL + ramped, outR + ramped, steady, false, false);
	}
}

void PartialBank::renderSpans(float* outL, float* outR, int frames, bool ramp, bool snapLast) {
	if (!stereo_) {
		ramp ? renderSpan<false, true, false>(outL, outR, frames, snapLast)
		     : renderSpan<false, false, false>(outL, outR, frames, snapLast);
	}
	else if (!ear_) {
		ramp ? renderSpan<true, true, false>(outL, outR, frames, snapLast)
		     : renderSpan<true, false, false>(outL, outR, frames, snapLast);
	}
	else {
		ramp ? renderSpan<true, true, true>(outL, outR, frames, snapLast)
		     : renderSpan<true, false, true>(outL, outR, frames, snapLast);
	}
}

void PartialBank::rotate(const float* ratio, float cycles, int count) {
	if (count > live_) // live_ is a multiple of 4: the groups that render, and so the only ones that turn in time
		count = live_;
	if (count <= 0 || !(std::abs(cycles) > 0.0f)) // no move, or a NaN one: nothing turns
		return;
	const __m128 c4 = _mm_set1_ps(cycles);
	for (int k = 0; k < count; k += 4) {
		// Lanes past count read a zero ratio, which is a zero angle: sincos gives exactly (0, 1), the identity.
		__m128 s, c;
		turnPs(_mm_mul_ps(load4(ratio, k, count), c4), s, c);
		const __m128 re = _mm_load_ps(re_ + k), im = _mm_load_ps(im_ + k);
		_mm_store_ps(re_ + k, _mm_sub_ps(_mm_mul_ps(re, c), _mm_mul_ps(im, s)));
		_mm_store_ps(im_ + k, _mm_add_ps(_mm_mul_ps(re, s), _mm_mul_ps(im, c)));
	}
}

void PartialBank::setEarPhase(const float* ratio, float from, float to, int frames, int count) {
	assert(stereo_);
	if (count > live_)
		count = live_;
	if (count < 0)
		count = 0;
	ear_ = stereo_ && live_ > 0;
	if (!ear_)
		return;
	EarRotators* ear = earRot_.get();
	// THE PER-SAMPLE TURN of the ears' difference, (to − from)/frames of a cycle of the fundamental, in turns per ratio;
	// a block of no length (never asked) holds it.
	const float perSample = frames > 0 ? (to - from) / static_cast<float>(frames) : 0.0f;
	const __m128 from4 = _mm_set1_ps(from), step4 = _mm_set1_ps(perSample);
	for (int k = 0; k < live_; k += 4) {
		// Lanes past count read a zero ratio: no turn, so the right ear reads the shared rotator there.
		const __m128 q = load4(ratio, k, count);
		__m128 s, c;
		turnPs(_mm_mul_ps(q, from4), s, c);
		const __m128 re = _mm_load_ps(re_ + k), im = _mm_load_ps(im_ + k);
		_mm_store_ps(ear->re + k, _mm_sub_ps(_mm_mul_ps(re, c), _mm_mul_ps(im, s)));
		_mm_store_ps(ear->im + k, _mm_add_ps(_mm_mul_ps(re, s), _mm_mul_ps(im, c)));
		// The right ear's coefficient: the shared one (the increment and the Phase control's bend) turned by the ramp.
		turnPs(_mm_mul_ps(q, step4), s, c);
		const __m128 cr = _mm_load_ps(cr_ + k), ci = _mm_load_ps(ci_ + k);
		_mm_store_ps(ear->cr + k, _mm_sub_ps(_mm_mul_ps(cr, c), _mm_mul_ps(ci, s)));
		_mm_store_ps(ear->ci + k, _mm_add_ps(_mm_mul_ps(cr, s), _mm_mul_ps(ci, c)));
	}
}

float PartialBank::earPhaseForTest(int i) const {
	return ear_ ? std::atan2(earRot_->im[i], earRot_->re[i]) : phaseForTest(i);
}

float PartialBank::phaseForTest(int i) const {
	return std::atan2(im_[i], re_[i]);
}

float PartialBank::amplitudeForTest(int i, int ear) const {
	return (ear == 0 || !stereo_) ? ampL_[i] : ampR_[i];
}

float PartialBank::incrementForTest(int i) const {
	return std::atan2(ci_[i], cr_[i]);
}

} // namespace feathers::partials
