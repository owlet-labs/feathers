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

// THE ADDITIVE ENGINE'S SPECTRUM FOR ONE SOUND: what the
// terrain authors, before the drawer (dsp/spectral_shaper.h) and before the voice. Pitch-independent, so the
// bank builds it once per character and every voice shares it.

#include <array>
#include <cstdint>

namespace feathers::partials {

inline constexpr int kMaxPartials = 320;
/// The envelope table: linear amplitude at kEnvelopePoints evenly spaced values of log2 k over
/// [kEnvelopeLog2Min, kEnvelopeLog2Max], so Formant Shift can read it anywhere from k = 0.5 to k = 1024.
inline constexpr int kEnvelopePoints = 256;
inline constexpr double kEnvelopeLog2Min = -1.0;
inline constexpr double kEnvelopeLog2Max = 10.0;
/// Off-grid (design §2.3): the start sweeps 320 -> 6 on a log scale; past it a partial drifts by up to
/// kOffGridDepth harmonics, a fixed φ-irrational pattern, ramped in over kOffGridRampShare of the start.
inline constexpr double kOffGridLowestStart = 6.0;
inline constexpr double kOffGridDepth = 0.5;
inline constexpr double kOffGridRampShare = 0.5;
inline constexpr double kOffGridMinRamp = 4.0;
/// Each sound is normalised so the root of the summed squared partial amplitudes over all kMaxPartials
/// (envelope at ratio × fine) is this, so Rolloff changes colour, not loudness. NOTE it is a root-SUM-square,
/// not a mean: a sum of sines at these amplitudes has an RMS of kTargetRss / √2.
/// A taste constant [to measure: peak level against the fixed phases, Task 9].
inline constexpr double kTargetRss = 0.35;

/// The thirteen terrain fields at one coordinate, each in [0, 1]. Zero is "nothing" for every field but rolloff,
/// which always has a value (0 is p = 0.5, the brightest). Detail at 0 is "no detail", which is its rest.
/// Only buildSound clamps: the closed forms below take the fields as given.
struct PartialFields {
	float rolloff{0.25f};
	float formant1Centre{0.0f}, formant1Width{0.0f}, formant1Gain{0.0f};
	float formant2Centre{0.0f}, formant2Width{0.0f}, formant2Gain{0.0f};
	/// ONE-SIDED (design §2.1): 0 = neutral, 1 = evens removed (square-like). The even-heavy direction is
	/// the drawer's Odd/Even operator, because a duty-gated field rests at 0 and 0 must mean "nothing".
	float oddEven{0.0f};
	float combSpacing{0.0f}, combDepth{0.0f};
	float detail{0.0f};
	float offGrid{0.0f};
	/// SMOOTHNESS (design §4.1, keep smoothness): "simplify the spectrum". See smoothed().
	float smoothness{0.0f};
	/// **PAL STAGE 1 (design §3.7): THE ENVELOPE'S WIDER VOCABULARY AND SPARSE COUNTRY.** Each rests at 0 =
	/// "nothing" (the position fields below at 0 read only while their partner is live). Read through EXISTING basis rows at
	/// their own φ configs (slice_build.cpp's kPartialExtraFields), as the detail harmonics read their group rows, so no
	/// residue row, stance member or patch value is added.
	float knee{0.0f};      ///< ENVELOPE: flat below harmonic 2^(4·knee), the power law above (0: no knee)
	float shelf{0.0f};     ///< ENVELOPE: +12·shelf dB above the shelf point, over an octave's step (0: none)
	float shelfAt{0.0f};   ///< the shelf point: harmonic 2^(1.5 + 3·shelfAt), ~2.8 .. 23
	float body{0.0f};      ///< ENVELOPE: a broad bump (σ 0.75 octave) up to +12 dB (0: none) — wider than a formant
	float bodyAt{0.0f};    ///< its centre: harmonic 2^(1 + 3.6·bodyAt), 2 .. 24
	float sparsity{0.0f};  ///< FINE: harmonics 2..7 gated to a chosen few and the rest silenced (0: none; 1: fully sparse)
	float sparseSet{0.0f}; ///< which few: a φ-pattern over harmonics 2..7 that glides as the field moves (sparseKeep)
	friend bool operator==(const PartialFields&, const PartialFields&) = default;
};

/// **ROLLOFF'S EXPONENT FOR A FIELD VALUE** (PAL stage 1, the re-spread; was p = 0.5 + 2·field, uniform over 0.5 .. 2.5, which put
/// the median position steeper than a triangle — MEASUREMENT PAL). Now p = 1 + c·sign(u)·|u|^1.6 with u = 2·field − 1 (c 1.2 below
/// the middle, 1.5 above): field 0 is p = −0.2 (a rising tilt), 0.5 a saw (p 1), 1 still p 2.5 (Smoothness's smooth spectrum), and
/// the field spends more of itself near the saw. Monotonic and continuous.
double rolloffExponent(float field);
/// **WHETHER SPARSE COUNTRY KEEPS HARMONIC k (2..7)** for a sparseSet value, 0..1, smoothly: a triangle-wave φ-pattern
/// tri(k·φ + 1.5·set) (continuous, unlike a frac, so nothing jumps as the set glides) through a soft threshold, so as the set moves the
/// kept partials fade in and out. Harmonic 1 is always kept; harmonics past 7 never are (sparse country is the fundamental and a
/// few low partials: 1+2+3, 1+3+5, 1+2+4+6).
double sparseKeep(int k, float sparseSet);

struct PartialSpectrum {
	std::array<float, kEnvelopePoints> envelope{}; ///< linear amplitude on the log2 k grid, normalised
	std::array<float, kMaxPartials> fine{};        ///< per partial: odd/even × comb × detail
	std::array<float, kMaxPartials> ratio{};       ///< per partial: frequency / f0 (k + off-grid drift)
	std::array<float, kMaxPartials> envAtRatio{};  ///< envelope read at ratio[i]: Formant Shift off's fast path
	friend bool operator==(const PartialSpectrum&, const PartialSpectrum&) = default;
};

/// Closed forms, public so cases can restate against them and the build can share them. They do NOT clamp:
/// a field outside [0, 1] gives the formula's value there, which can be NaN (a formant width below −1/6) or a
/// negative gain (Odd/Even or Comb depth above 1). buildSound is the clamping entry point.
double envelopeAt(const PartialFields& f, double k);               ///< UN-normalised Rolloff × formants
double fineAt(const PartialFields& f, int k, float detailTriangle); ///< Odd/Even × Comb × detail at harmonic k
double offGridStart(float offGrid);
double offGridRatio(int k, double start);

/// Smoothness applied to CLAMPED fields (design §4.1): with σ = smoothness, Detail, Comb depth, Odd/Even, both
/// formant gains and Off-grid scale by (1 − σ), and Rolloff moves toward 1 (p = 2.5, its steepest) by σ. At
/// σ = 1 the spectrum is the smooth, harmonic one whatever the other fields say. buildSound calls this after
/// clamping, before anything reads the fields.
PartialFields smoothed(const PartialFields& clamped);

/// One sound's spectrum. `detail[i]` is harmonic i+1's detail triangle, in [-1, 1]. THE CLAMPING ENTRY POINT:
/// every field is clamped to [0, 1] and every detail triangle to [-1, 1] on entry, NaN to 0, so whatever the
/// terrain hands in, the spectrum is finite and equal to the clamped input's.
void buildSound(const PartialFields& f, const std::array<float, kMaxPartials>& detail, PartialSpectrum& out);

/// The blend: `into += weight · sound`, member by member. Start `into` value-initialised. The weights over
/// one blend must sum to 1: the ratios are blended too, so weights summing to w scale every frequency by w.
void accumulate(PartialSpectrum& into, const PartialSpectrum& sound, double weight);

/// The envelope table read at harmonic position k (linear in log2 k, clamped at the table's ends). Any k is
/// safe: 0, a negative k and NaN read the bottom end; +inf and anything past k = 1024 read the top end.
float envelopeRead(const PartialSpectrum& s, double k);

/// Rescale `s` so the root-sum-square of envAtRatio[i]·fine[i] over every partial is kTargetRss: ONE scale on
/// `envelope` and `envAtRatio` alike, so a single sound's fast path stays the table read at each ratio TO ROUNDING,
/// not to the bit: scaling a read and reading a scaled table round differently in the last place. (buildSound's own
/// spectrum IS the table read to the bit, because it re-reads after scaling; anything this touches is not.) So the
/// voice (plan Task 9) must not compare its two roads bitwise. A spectrum with no power is left as it is (silence
/// stays silence, never NaN).
void normaliseRss(PartialSpectrum& s);

/// THE SHAPE CHOICE (design §4.1, one knob + a dropdown). Its Choice param and menu land with
/// the Explore controls (plan Phase B), in this order.
enum class ShapeKind : uint8_t { Sine, Triangle, Saw, Square, kCount };

/// The exact band-limited target of `kind`, normalised by normaliseRss. Ratios are exactly k. The rolloff lives
/// in the ENVELOPE (and envAtRatio): 1/k for Sine, Saw and Square, 1/k² for Triangle. The harmonic MASK lives in
/// `fine`: every harmonic for Saw, the odd ones for Square and Triangle, harmonic 1 alone for Sine. So Formant
/// Shift slides a shape's envelope like any other, and its mask stays with the partials.
///
/// TRIANGLE'S ALTERNATING SIGN IS NOT REPRESENTED: its series is Σ (−1)^n sin((2n+1)x)/(2n+1)², and the sign is a
/// phase, which the engine fixes rather than authors (design §2.4). Magnitudes only.
///
/// `envAtRatio` is the closed form at k (k^−1 or k^−2, exactly), NOT the table read at k: the table is linear in
/// log2 k, which reads an exponential high between its points, and "exact" is this function's promise. For a
/// shape the two differ by a few parts in 10^4 (derived: (p·h·ln 2)²/8 at the grid step h, p = 1 or 2).
void buildShapeSpectrum(ShapeKind kind, PartialSpectrum& out);

/// THE SHAPE BLEND, one sound's: `sound = (1 − a)·sound + a·shape` member by member (a = `amount` clamped to
/// [0, 1], NaN to 0), then normaliseRss. AT a = 0 IT RETURNS WITHOUT TOUCHING `sound`, so a zero amount is the
/// terrain to the bit; at a = 1 it is the shape (ratios onto the harmonics too), to the renormalisation's
/// rounding.
void blendTowardShape(PartialSpectrum& sound, const PartialSpectrum& shape, float amount);

/// **ONE CYCLE OF A SPECTRUM, AS A WAVEFORM** — for the pictures and the stack's likeness reading, which have no note-on of
/// their own (plan Task 9b). Sample j of `points` sits at phase j / `period` (period = points for a cycle that wraps,
/// points − 1 for a picture whose last sample is the cycle's end) and is
///     Σ_{k < count} A_k · sin(2π · ratio_k · phase + π·k² / N),     A_k = envAtRatio_k · fine_k · (gain_k or 1),
/// the partials at NEWMAN'S FLAT PHASES for N = `phaseN` partials (PartialBank::reset(N), design §2.4; N ≤ 0 is taken as
/// `count`): a note-on weights them by its own spectrum (Schroeder), which a picture with no note cannot know, and flat
/// Newman is that law's equal-power case. N IS SEPARATE FROM `count` so a caller comparing cycles can fix it: two
/// readings that summed different counts at count-dependent phases would differ by their PHASES, not their spectra. `gain` (may be null) is a per-partial factor, the Nyquist fade at a voice's pitch for a voice.
/// A partial whose ratio is not whole does not close on the cycle: drawn as it plays over that one period. Each
/// partial is turned by a phasor recurrence in double (no sin per sample); `count` is clamped to [0, kMaxPartials].
void cycleOf(const PartialSpectrum& s, int count, int phaseN, const float* gain, int points, double period, float* out);

} // namespace feathers::partials
