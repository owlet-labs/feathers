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

// THE ADDITIVE ENGINE'S ROTATOR BANK: one
// recursive oscillator per partial, a complex multiply per partial per sample, four partials per __m128, at host
// rate. The kernel is the measured one from the additive bench (0.31 ns per partial per sample), made into a
// tested unit.
//
// WHAT IT PROMISES:
//   - Each partial's output is amp · Im(z), z rotating by its increment every sample. Sample n of a block (0-based)
//     is sin(φ + (n+1)·w) for a partial at phase φ when the block starts.
//   - Fixed phases (C8): reset(partialCount) puts partial k (0-based) at the Newman phase
//     φ_k = π·k²/N with N = max(1, partialCount), THE NOTE'S partial count, not the ceiling, amplitude zero. With N
//     fixed at kMaxPartials a high note's few partials would take only the first M/N of the chirp and their peaks
//     would bunch, the worst crest factor exactly where headroom is scarcest (decided 2026-10-08). That is the FLAT
//     case. The voice uses reset(power, count): Schroeder's law weighted by the note's own spectrum (decided
//     2026-10-10), because Newman's pattern is low-crest only for equal amplitudes and a bright terrain spectrum
//     peaked above full scale in one voice under it. Flat power reduces it to Newman's quadratic.
//   - Amplitudes ramp LINEARLY per ear from the current value to the target over setTargets' `frames` samples, and
//     the ramp's final sample is EXACTLY the target (snapped, not accumulated), so a ramp to zero ends on 0.0f.
//   - THE NYQUIST FADE (§5.3) is LINEAR IN THE INCREMENT: gain 1 at or below kFadeStart·π, 0 at or above
//     kFadeEnd·π, a straight line between. A partial at or above kFadeEnd·π is silent and is not rendered, and so is one
//     whose increment is zero, negative or NaN (it would otherwise render DC, amp·sin φ).
//   - A target whose magnitude is below kSilenceFloor (−160 dB) is exactly zero, so no denormal can be ramped to and
//     sit there keeping the plugin awake (CLAUDE.md, SLEEP is decided by the output). So is a non-finite target.
//   - A partial at zero in both ears (target AND current) is not counted live. live_ is a multiple of 4 covering the
//     highest partial still sounding in either ear, and "NOT RENDERED" IS PER GROUP OF FOUR: groups past live_ are
//     skipped, but a silent lane inside a live group still rotates (at zero amplitude, contributing nothing). A
//     partial that is not rendered does not rotate either, so it resumes from the phase it stopped at.
//   - The two ears share one rotator per partial: stereo is amplitude only, never phase, so a mono sum cannot
//     cancel (§1.4) — UNLESS THE BLOCK ASKS FOR AN EAR PHASE (setEarPhase, backlog OZ): then, for that block only, the
//     right ear reads its own rotator, the shared one turned by ratio·2π·(the ears' offset difference). A block that
//     does not ask renders the shared rotator in both ears through the arithmetic it always had.
//   - Partials at or past `count` RAMP OUT: their target is zero, so a shrinking count fades them over the block
//     rather than cutting them (a cut clicks). While ramping out a partial keeps the last increment it was given
//     (it is not in the caller's arrays any more); once at zero in both ears it is not rendered.
//   - In mono the R ear IS the L ear: R never holds an amplitude L does not, so returning to stereo ramps R from
//     where L is.
//   - The rotators are renormalised once per setTargets (z ·= 1.5 − 0.5·|z|²), which holds |z| at 1 against
//     rounding drift, and each new coefficient is normalised (c /= |c|), so the drift within a block does not
//     depend on how often setTargets is called.
//
// No allocation after construction: every array is fixed at kMaxPartials (the right ear's, setEarPhase, allocated with
// the bank). render() ADDS into its outputs (the caller sums voices);
// mono writes only outL. render() honours ANY frame count: the ramp lasts setTargets' `frames` samples, carried
// across as many renders as it takes, and the amplitudes then hold. The bank uses aligned loads, so it must sit on a
// 16-byte boundary (its alignas members make that the type's alignment, which new and the stack honour).

#include "dsp/partial_spectrum.h"

#include <memory>

namespace feathers::partials {

inline constexpr int kControlBlock = 32;
/// The Nyquist fade (design §5.3): a partial's gain falls from 1 to 0 as its angular increment rises from
/// kFadeStart·π to kFadeEnd·π, LINEARLY in the increment, so vibrato never pops a partial in or out.
inline constexpr float kFadeStart = 0.90f;
inline constexpr float kFadeEnd = 0.98f;
/// Targets below this magnitude are exactly zero (−160 dB): the floor that keeps a ramp off the denormals.
inline constexpr float kSilenceFloor = 1e-8f;

static_assert(kMaxPartials % 4 == 0, "the bank runs four partials per vector");

class PartialBank {
public:
	/// Phases to Newman's low-crest pattern for EQUAL amplitudes (φ_k = π·k²/N, N = max(1, partialCount)), amplitudes
	/// to zero. The flat case; the voice uses the weighted reset below.
	void reset(int partialCount);

	/// THE NOTE-ON'S PHASES, WEIGHTED BY THE NOTE'S SPECTRUM (decided 2026-10-10):
	/// Schroeder's low-crest law for a NON-flat spectrum, φ_k = −2π·Σ_{l<k} (k − l)·p_l with p_l = power[l] / Σ power
	/// over the first `count` partials (power[l] = the amplitude squared the first block will render), computed with two
	/// running sums (Σ p and Σ l·p), over the groups `count` names only (rounded up to 4). A partial past them is silent
	/// and KEEPS WHATEVER PHASE IT HAD (the last note's), until a later block names it, a glide down for instance. Inside
	/// the last group, lanes at or past `count` continue the same law with no further
	/// power. Newman's pattern is the flat case's: with every p_l = 1/N the law is φ_k = −π·k(k+1)/N, which is
	/// −(π·k²/N) − π·k/N — Newman's quadratic MIRRORED (a time reversal) plus a phase linear in k (a time shift of 1/(2N)
	/// of a period), so the same waveform shape and the same crest factor. A count of zero, or no finite positive power,
	/// is the flat reset(count). Amplitudes to zero, as reset(int).
	void reset(const float* power, int count);

	/// **A-PH: THE NOTE-ON'S PHASES FROM AN OPTIMISED PROFILE** (dsp/phase_profile.h) — partial k at `phase[k]` radians (its next
	/// sample amp·sin(phase + increment)), for the groups `count` names (lanes past `count` in the last group at 0). Otherwise
	/// as reset(power, count): amplitudes to zero, a partial past the named groups keeps whatever phase it had.
	void resetPhases(const float* phase, int count);

	/// Once per control block: each partial's angular increment (radians per sample, k·2π·f0/sr or the off-grid
	/// ratio's) and target amplitude per ear. Ramps from the current amplitudes over `frames` samples. Partials past
	/// `count` ramp out to zero, keeping their last increment while they do; a partial at zero in both ears (target
	/// and current) is not rendered.
	/// Renormalises the rotators. targetR is read only when `stereo` (it may be null otherwise; asserted non-null when
	/// stereo).
	/// THE PHASE CONTROL'S RAMP (`bendRatio`, `bend`): each named partial's rotator turns by increment + bendRatio·bend
	/// per sample instead, so a read offset moving by Δ cycles over the block is folded in as bend = 2π·Δ/frames — the
	/// phase moves linearly per sample rather than stepping. THE NYQUIST FADE IS STILL THE UN-BENT INCREMENT'S (the
	/// partial's frequency, not the momentary phase slope). A bent angle past π (a partial up in the fade, bent further
	/// up) is wrapped to the same rotation less 2π, so every partial's phase at the block's end is exact; the caller keeps
	/// the FULL-GAIN partials' bent angles inside (0, π], so only a faded partial's slope inside the block can alias.
	void setTargets(const float* increment, const float* targetL, const float* targetR, int count, int frames,
	                bool stereo, const float* bendRatio = nullptr, float bend = 0.0f);

	/// Note-on: amplitudes JUMP to the targets of the next setTargets instead of ramping (the amp envelope owns
	/// the attack, design §5.2).
	void snapNextTargets() { snap_ = true; }

	/// ADDS `frames` samples into outL (and outR when stereo). Any frame count: a ramp carries across renders.
	void render(float* outL, float* outR, int frames);

	/// THE PHASE CONTROL'S STEP (plan Task 9: the note-on's offset, and a jump too big to fold into setTargets' bend):
	/// turn partial i (i < count) by ratio[i]·2π·cycles, at once, for a read
	/// offset that moved by `cycles` of the fundamental. Exact for a harmonic partial (a time shift of the whole
	/// waveform by `cycles` periods); an off-grid one turns by its own ratio, so the shift is the same TIME for every
	/// partial. Each angle is reduced to the nearest whole turn before its sincos, so any ratio and any move are safe
	/// (a |ratio·cycles| past 2^22 turns, where a float has no fraction left, is clamped there). The rotation's
	/// coefficient is normalised, so |z| is untouched to rounding; the next setTargets renormalises as always.
	/// Partials past `count`, past the live partials (those are not rendered and do not turn in time either: they resume
	/// where they stopped), and any whose ratio is zero are left where they are. Amplitudes are not touched.
	void rotate(const float* ratio, float cycles, int count);

	/// **THE RIGHT EAR'S OWN PHASE FOR THIS BLOCK** (backlog OZ, 2026-10-29): Phase as a stereo destination, which the
	/// other engines honour by reading the right ear at its own offset. Called after setTargets (and any rotate) of a
	/// STEREO block whose ears' offsets differ: the right ear then reads rotator z_i·e^{i·ratio_i·2π·from} from the
	/// block's first sample, turning by the shared rotator's angle plus ratio_i·2π·(to − from)/frames per sample, so it
	/// lands on z_i·e^{i·ratio_i·2π·to} (to rounding) after `frames`: the ears' difference ramps linearly, as the left
	/// ear's offset does. The right rotator is DERIVED from the shared one every block, so it carries no drift of its
	/// own. Lanes past `count` (and a zero ratio) read the shared rotator unturned. Each angle reduced to the nearest
	/// whole turn before its sincos, as rotate() does. The next setTargets clears it: a block that does not ask renders
	/// both ears off the shared rotator. Cost while asked: two sincos per live partial per block, and per sample one
	/// complex multiply per partial for the right ear.
	void setEarPhase(const float* ratio, float from, float to, int frames, int count);

	/// THE TARGET partial i is ramping to (or holds) in ear 0 (L) or 1 (R): the caller's amplitude through the Nyquist fade
	/// and the silence floor — what the bank renders once the block's ramp lands. In mono R is L. The spectrum drawer's
	/// plot reads it (OscVoice::plotPartials): a block's value, not a sample's, so a held note publishes one frame.
	float target(int i, int ear) const { return ear == 0 ? tgtL_[i] : tgtR_[i]; }

	// TEST SEAMS: the values, not a rendered buffer.
	/// Current amplitude of partial i in ear 0 (L) or 1 (R). In mono R is L.
	float amplitudeForTest(int i, int ear) const;
	/// The angle the rotator turns partial i by each sample, read back from its coefficient (atan2(ci, cr)).
	float incrementForTest(int i) const;
	/// Partial i's phase now, atan2(im, re), in (−π, π]. Its next sample is amp·sin(phase + increment).
	float phaseForTest(int i) const;
	/// The right ear's phase for partial i while an ear phase is live (setEarPhase), else the shared one's.
	float earPhaseForTest(int i) const;
	/// Whether this block's right ear reads its own rotator (setEarPhase).
	bool earForTest() const { return ear_; }
	int liveForTest() const { return live_; }
	/// The partials rendering now (a multiple of four): how far the phase control's rotate() reads its ratios.
	int live() const { return live_; }
	/// TEST ONLY. The lowest group the bank renders: the groups below it are silent in both ears (a downward Shift's).
	int firstForTest() const { return first_; }

private:
	template <bool kStereo, bool kRamp, bool kEar>
	void renderSpan(float* outL, float* outR, int frames, bool snapLast);
	void renderSpans(float* outL, float* outR, int frames, bool ramp, bool snapLast);
	void trimLive();
	/// Amplitudes, ramps, targets and coefficients to their rest (live_ 0, no snap, mono); the PHASES are left alone, for
	/// the reset that writes them.
	void clearState();

	// Structure of arrays, 16-byte aligned, kMaxPartials each (a multiple of 4).
	alignas(16) float re_[kMaxPartials] {}, im_[kMaxPartials] {}, cr_[kMaxPartials] {}, ci_[kMaxPartials] {};
	alignas(16) float ampL_[kMaxPartials] {}, dampL_[kMaxPartials] {}, tgtL_[kMaxPartials] {};
	alignas(16) float ampR_[kMaxPartials] {}, dampR_[kMaxPartials] {}, tgtR_[kMaxPartials] {};
	// THE RIGHT EAR'S ROTATOR AND ITS COEFFICIENT (setEarPhase), read only while ear_: derived from re_/im_ and cr_/ci_. ON
	// THE HEAP, allocated with the bank (on the message thread): 5 KB per oscillator inline would push a Synth past a test
	// rig's stack, as OscVoice's VocodeGains says.
	struct EarRotators {
		alignas(16) float re[kMaxPartials] {}, im[kMaxPartials] {}, cr[kMaxPartials] {}, ci[kMaxPartials] {};
	};
	std::unique_ptr<EarRotators> earRot_ = std::make_unique<EarRotators>();
	bool ear_{false};
	int live_{0};
	int first_{0}; ///< the lowest group sounding or ramping (a multiple of 4): render starts there
	int rampLeft_{0}; ///< samples of the current ramp still to render; its last one lands on tgt
	bool stereo_{false};
	bool snap_{false};
};

static_assert(alignof(PartialBank) >= 16, "render and setTargets use aligned loads on the members");

/// The Nyquist fade alone, so a case can restate it (0 for an increment that is zero, negative or NaN).
float nyquistFade(float increment);

} // namespace feathers::partials
