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

#include "fx/filter/filter_types.h"

// OUR OWN PRIMITIVES, NOT A VENDOR'S (licence migration, 2026-09-24). The SVF is Andy Simper's, copied
// under its MIT carve-out; the one-pole, the sinc delay line and the fast tan/tanh they run on are
// Feathers' own, written from the published mathematics each file names.
#include "dsp/elliptic_highpass.h"
#include "dsp/sinc_delay_line.h"
#include "dsp/tpt_one_pole.h"
#include "fx/filter/cytomic_svf.h"

#include <cstddef>

// THE MULTIMODE FILTER'S SAMPLE LOOP. What it is — the types, the descriptors, the macro mapping,
// the curve — is next door in filter_types.h; this is the object that renders.
//
// ONE FILTER, EIGHT TYPES, ONE COEFFICIENT SET. Andy Simper's trapezoidal SVF (cytomic_svf.h, MIT by
// its author's carve-out) computes g and k once and then only varies the three-term output mix, so
// every type in the dropdown costs exactly the same and switching between them is a change of three
// multipliers rather than a change of filter. Two consequences run through this whole module:
//
//   - THE STATE UPDATE DOES NOT DEPEND ON THE OUTPUT MIX. ic1eq/ic2eq evolve from the input and from
//     a1/a2/a3 alone. So four SIMD lanes fed the same input with the same g and k hold identical
//     state forever, and only their m-vectors differ — which is what makes Morph free: lanes 0/1 run
//     the declared type on L/R, lanes 2/3 run the morph partner on the same L/R, and the blend of
//     their outputs IS the blend of the mix vectors, exactly, with no second filter and no second
//     state to keep in step.
//   - A blend of two of these filters' outputs is a blend of two linear systems, so the response
//     curve drawn on the card can be computed in closed form even with the macros engaged. See
//     gainAt() in filter_types.h.
//
// SMOOTHING IS ON A FIXED SAMPLE GRID, and that is a correctness constraint rather than a nicety.
// tests/test_synth.cpp pins that a performance renders identically in blocks of 1 and 37; a
// coefficient smoother stepped once per block, with a rate taken from the block length, is invariant
// only in algebra and not in floating point. This module therefore steps every kTickSamples samples
// counted from the start of the stream, exactly as PuckMotion does and for the same reason — the
// tick boundaries sit at absolute sample positions, so how the host chopped its buffer cannot reach
// the audio. CytomicSVF's own setCoeffForBlock<N> smooths across a compile-time block length and is
// the thing NOT to use here.

namespace feathers::fx::filter {

/// THE SINC TABLE THE COMB'S DELAY LINE INTERPOLATES WITH — one, shared by every filter that ever
/// exists, because it is a function of nothing but itself.
///
/// A NAMESPACE-SCOPE OBJECT RATHER THAN A FUNCTION-LOCAL STATIC, and that is a realtime decision.
/// SincDelayLine takes the table's address at construction, and a MultimodeFilter used to be
/// CONSTRUCTED ON THE AUDIO THREAD — FxRunner::sync placement-newed a device whenever the chain's tag
/// in a slot changed. A function-local static would therefore have built its 25 KB of
/// blackman-windowed sinc, with its transcendentals and its thread-safe-init guard, inside a render
/// call the first time a filter was seated.
///
/// CONSTRUCTION MOVED TO THE MESSAGE THREAD ON 2026-08-26 (DeviceInstancePool::build), WHICH DOES NOT
/// GIVE THE DECISION BACK. The table would then be built on whichever thread first seated a filter,
/// which is a 25 KB transcendental sweep inside a GUI frame instead of inside a render call — better,
/// and still not something to leave to whoever gets there first. At namespace scope it is built during
/// the library's own dynamic initialisation, before either thread exists. It allocates nothing either
/// way: every member is a fixed array.
inline const feathers::dsp::SincTable kSincTable {};

/// WHAT THE OPS TABLE HANDED THE DSP, for tests only.
///
/// The five-line seam CLAUDE.md prescribes rather than the ingenuity it warns against: the claim is
/// "keyOffsetSemitones is 12 at this instant", and no amount of comparing renders recovers one
/// quantity from a signal carrying every quantity at once. `storage` must be a block the filter's own
/// AudioOps::construct built.
const Settings& opsTargetForTest(const void* storage);
/// The f0 an ops-driven filter's brickwall is running at, 0 when it is another type — see `brickwallCornerForTest`.
double opsBrickwallCornerForTest(const void* storage);

class MultimodeFilter {
public:
	/// The smoothing grid, in samples. Sixteen is 0.33 ms at 48 kHz — fine enough that a cutoff
	/// sweep does not read as stairs against a time constant a hundred times longer, and coarse
	/// enough that the coefficient solve happens a sixteenth as often as the sample loop.
	///
	/// Deliberately NOT the renderer's chunk size. A grid that happened to equal the chunk cap would
	/// be indistinguishable from per-block smoothing on most calls and would hide the bug the
	/// block-size case exists to catch.
	static constexpr int kTickSamples = 16;

	/// Time constant of the control smoother, seconds. Long enough to swallow a host's parameter
	/// steps, short enough that a hand-drawn sweep arrives when the hand does.
	static constexpr float kSmoothSeconds = 0.01f;

	/// **BELOW THIS THE TAIL IS OVER AND THE STATE IS CLEARED**, so a decay reaches EXACT zero instead
	/// of crawling into the denormals and parking there.
	///
	/// `puck_motion.h` paid for this lesson already and states it best: *"Multiplying by 0.982 each
	/// tick walks the value down through the denormals and then STICKS ... the step is smaller than
	/// half the gap between adjacent denormals, and the velocity sits there for the rest of the
	/// session."* It fixed it with `kRestThreshold`. A biquad's decay is the same arithmetic, and
	/// MEASURED: after a 0.8 burst, `Waypoint` produced a non-zero sample in **400 blocks of 400** at
	/// 1.4013e-45 — *identical to every digit across 2,800 blocks*, which is not a decay.
	///
	/// **AND EVERY DEVICE PAYS FOR THE DESCENT, NOT ONLY THE ONES THAT PARK.** Two thousand silent
	/// blocks with and without the host setting flush-to-zero: Filter 1.55x, OWLTT 1.45x, Drawn 1.40x,
	/// Harm 1.24x, Waypoint 1.15x. Feathers sets FTZ nowhere.
	///
	/// **ONE ABSOLUTE LEVEL, AND THE REASON IS THAT THE QUANTITY IS THE ARITHMETIC'S RATHER THAN THE
	/// SIGNAL'S.** The backlog asks for a level scaled to something each device knows about itself,
	/// on the grounds that the devices' burst peaks span 2.27e-05 (Drawn) to 4.22 (OWLTT). That span
	/// is five orders; the gap from the quietest of them down to `FLT_MIN` is **thirty-three**. This
	/// level sits eight orders above `FLT_MIN` and twenty-five below the quietest device output
	/// measured — so it cannot truncate an audible tail on any of them, and a device-relative level
	/// would buy margin where there are already twenty-five orders of it. Said out loud rather than
	/// quietly departed from.
	static constexpr float kRestLevel = 1.0e-30f;

	/// **HOW LONG THE INPUT MUST HAVE BEEN EXACTLY SILENT BEFORE THE STATE MAY BE CLEARED**, in
	/// samples at the prepared rate.
	///
	/// **A QUIET OUTPUT IS NOT A QUIET STATE, and that is what this constant is for.** The first
	/// version flushed on the output alone and broke two impulse-response cases by **0.472 dB and
	/// 1.24 dB** — because a delay line holds a full-scale sample while the output between taps is
	/// nothing, and a highpass answers a DC input with silence while its state is large. The comb is
	/// the extreme: `kCombLineSamples` of stored signal behind an output that can be zero for a whole
	/// block. **So the condition is about the INPUT**, which is the only thing that can promise there
	/// is nothing left to come out.
	///
	/// **THE TWO CONDITIONS COVER DIFFERENT HAZARDS AND NEITHER IS REDUNDANT.** The INPUT condition
	/// covers signal that is STORED and has not come out yet — `kCombLineSamples` is 4096, 85 ms at
	/// 48 kHz, and one second is 11.7 times that. The OUTPUT condition covers signal that is still
	/// COMING out: a comb with feedback rings for seconds, and while it does its output is nowhere
	/// near `kRestLevel`, so the flush cannot fire however long the input has been quiet. Drop either
	/// and the pair stops being safe.
	///
	/// MEASURED AFTER THE FIX, 2000 silent blocks, without host FTZ, twice with the same answer:
	/// Filter 81-86 ms -> 44.3, Harm 96-98 -> 46.1, OWLTT 78-81 -> 24.5, Waypoint 25.5 -> 13.1, Drawn
	/// 118-120 -> 67.7. **And the two devices that PARKED stopped caring about FTZ at all** — OWLTT
	/// 1.45x -> 1.01x, Waypoint 1.15x -> 1.02x. What is left for the other three (1.41x to 1.55x) is
	/// the DESCENT through the denormals during this second, which no threshold can shorten and which
	/// is what an FTZ decision is now about.
	static constexpr double kRestSilentSeconds = 1.0;

	void prepare(double sampleRate);
	/// Zero the filter state. Coefficients are left alone; they are a function of the settings.
	void reset();

	/// Where the smoother is walking to. Cheap: the coefficient solve happens on the tick.
	void setTarget(const Settings& s) { target_ = s; }
	const Settings& target() const { return target_; }

	/// Put the smoother where the target already is — a voice starting, a patch loading, a device
	/// being switched on. Nobody should hear the settings ramp in from wherever they were left.
	void snap();

	/// Forget the state AND the fact that the device was running, so the next block starts as if it
	/// had just been switched on: coefficients snapped to whatever the settings are by then, rather
	/// than ramping in from the last note's. What a voice does when it takes a new note — a filter
	/// that carried the previous note's cutoff into the first ten milliseconds of this one would be
	/// audible on every staccato line.
	void restart() {
		reset();
		active_ = false;
	}

	/// In place, any n from 1 upward, and BIT-EXACTLY THE IDENTITY when the device is off: the
	/// buffers are not read and not written.
	void processStereo(float* left, float* right, int n);
	void processMono(float* mono, int n);

private:
	/// The type switch and its eight loops. `processStereo` is the wrapper that adds the rest check;
	/// this is what it wrapped, unchanged.
	void processStereoCore(float* left, float* right, int n);
	void processMonoCore(float* mono, int n);

	/// **CLEAR THE STATE ONCE THE OUTPUT HAS DECAYED PAST `kRestLevel`.** One pass over the block that
	/// was just written, so it costs a `fabs` and a `max` per sample against a filter step, and
	/// nothing at all while the device is loud enough to matter.
	///
	/// READS THE BUFFER, so it runs only when the device is enabled and has written it — the bypass
	/// contract is that *"not one sample is read"*, and a rest check on a buffer this device did not
	/// produce would be measuring the caller's audio.
	void flushIfAtRest(float* left, float* right, int n, bool silentInput);

	/// Consecutive samples of exactly silent INPUT. See `kRestSilentSeconds`.
	int restSilence_ = 0;
	/// `kRestSilentSeconds` in samples at the prepared rate.
	int restSilenceNeeded_ = 48000;

public:

	bool active() const { return active_; }
	/// **WHETHER THE COMB'S LINE STILL HOLDS SOUND** — `AudioOps::holdsTail` (backlog MC). The one stage whose output
	/// can be exactly zero for a whole block while it holds signal (`restSilenceNeeded_` says why): up to 4080 samples
	/// between echoes with the damper at 0. Off, or in any other type, the line is not read, and is cleared before a
	/// comb reads it again.
	[[nodiscard]] bool holdsTail() const noexcept {
		return target_.enabled && active_ && combRunning_ && (combLine_[0].holdsSound() || combLine_[1].holdsSound());
	}
	/// The first cascade section, for the per-ear lane case (backlog KU) — its g and k per lane. Test-only reader.
	[[nodiscard]] const CytomicSVF& stage1ForTest() const noexcept { return stage1_; }
	/// THE f0 THE BRICKWALL IS AT (backlog NL), left channel, or 0 while another type runs. Test-only reader.
	[[nodiscard]] double brickwallCornerForTest() const noexcept { return brickwallRunning_ ? brickwall_[0].cornerHz() : 0.0; }

private:
	/// EVERY CONTINUOUS CONTROL THIS DEVICE HAS, and the list is the contract: a control that is here
	/// glides and a control that is not steps, once per coefficient update. The discrete ones —
	/// the type, the slope, the keytrack architecture — are deliberately absent, because a choice is
	/// not a position and crossfading between two of them means nothing.
	///
	/// `spread` was missing, and missing for a traceable reason: it used to arrive through macro1 and
	/// macro2, which are on this list, and promoting it to a parameter of its own took it off. It cost
	/// a measured 22-29x tone-slew discontinuity on every jump. If a control is added to Settings and
	/// not to this struct, that is the bug it will be.
	struct Coeffs {
		float cutoffLog2Hz;
		float resonance;
		float drive;
		float macro1;
		float macro2;
		float spread;
	};

	void tick();
	void solve();
	/// **`solve` WITH A RIGHT EAR** (backlog KU): each ear resolved from its own smoothers, lanes {L, R, L, R}.
	/// Runs only while `target_.rightLive` is non-zero, so the mono road is `solve`'s own body.
	void solveEars();
	/// The mono road's tail: every right-ear twin takes its left's value, so equal numbers give equal bits.
	void mirrorRightEar();
	float driveShape(float x) const;
	float driveShapeR(float x) const;

	Settings target_{};
	Coeffs smoothed_{};
	/// THE RIGHT EAR'S SMOOTHERS (backlog KU). Stepped only while a control is stereo-live; a control that is not
	/// live is its left. `liveSmoothed_` is which controls were live at the last tick, so one that has just become
	/// live starts from its left rather than from wherever it was last left.
	Coeffs smoothedR_{};
	uint8_t liveSmoothed_{0};
	double sampleRate_{48000.0};
	float srInv_{1.0f / 48000.0f};
	float smoothAlpha_{1.0f};

	/// THE CASCADE. stage1_ is the section wearing the resonance the knob asks for, and it is the only
	/// one a 12 dB slope runs — which is what makes every patch that predates this control render the
	/// samples it always did. stageButter_ joins it at 24 dB, at the same corner and at Butterworth Q,
	/// so the pair peaks once. stage2_ is the SPREAD stage and is untouched: a blended section at an
	/// offset corner, after the cascade, exactly where it always sat.
	CytomicSVF stage1_{};
	CytomicSVF stageButter_{};
	CytomicSVF stage2_{};

	/// THE ONE-POLE, which is the only way this family reaches an odd order at all: CytomicSVF is
	/// structurally two-pole. dsp/tpt_one_pole.h is the same trapezoidal integrator the SVF is built
	/// on, taken down to one pole (Zavalishin's TPT one-pole), one float of state each, and its g is
	/// fasttan(pi·fc/sr) exactly as setCoeff's is — so a one-pole and an SVF section at one corner
	/// agree about where that corner is.
	///
	/// One per channel and no morph lanes: it runs on the already-BLENDED signal, so there is no
	/// partner mix for it to carry. A lowpass pair and a highpass pair rather than one with a flag,
	/// because a branch per sample is not worth saving 16 bytes.
	feathers::dsp::OnePoleLP onePoleLpL_{};
	feathers::dsp::OnePoleLP onePoleLpR_{};
	feathers::dsp::OnePoleHP onePoleHpL_{};
	feathers::dsp::OnePoleHP onePoleHpR_{};
	bool onePoleHighpass_{false};

	/// THE COMB'S DELAY LINE, ONE PER CHANNEL — and it is the whole of this object's size. Two of
	/// these are 32 KB against the 1.4 KB everything else adds up to, and they are held whether or not
	/// a comb is selected.
	///
	/// UNCONDITIONAL BECAUSE `AudioOps::stateBytes` IS ONE NUMBER PER DEVICE, not one per type. A
	/// device still cannot ask for a different size depending on what it is set to, so a filter that
	/// is not a comb carries the line anyway.
	///
	/// WHAT CHANGED ON 2026-08-26 IS WHO PAYS FOR THE SLOTS THAT HOLD NOTHING. The host used to pool a
	/// UNIFORM STRIDE of the widest device's bytes for every (voice, slot) pair, because the only
	/// other moment it could size anything was FxRunner::sync — on the audio thread, where allocation
	/// is forbidden — and that took the fx pool from 236 KB to 5.5 MB per plugin instance whether or
	/// not any comb was selected. Building moved to the message thread (DeviceInstancePool::build) and
	/// the stride went with it: a slot now costs its OWN device's stateBytes and an empty slot costs a
	/// pointer. The 32 KB is still paid by every filter that exists; it is no longer paid by the
	/// seven eighths of the lane that is empty.
	feathers::dsp::SincDelayLine<kCombLineSamples> combLine_[2] {
	    feathers::dsp::SincDelayLine<kCombLineSamples>(kSincTable),
	    feathers::dsp::SincDelayLine<kCombLineSamples>(kSincTable)};

	/// The damper's one-pole memory, one per channel. INSIDE the loop, so it is filter state and not a
	/// coefficient.
	float combDampZ_[2] {0.0f, 0.0f};

	/// WHERE THE READ HEAD IS AND WHERE IT IS GOING. The delay is not a coefficient that may step on
	/// the tick like the SVF's g and k: a jump in read position is a jump in the signal, so the tick
	/// sets a per-sample increment and the sample loop walks it. The increment is a function of the
	/// tick length and of nothing else, which is what keeps the block size out of the audio.
	float combDelay_ {0.0f};
	float combDelayTarget_ {0.0f};
	float combDelayInc_ {0.0f};
	float combFeedback_ {0.0f};
	float combDamp_ {0.0f};
	float combDampGain_ {1.0f};
	/// THE RIGHT EAR'S COMB (backlog KU) — equal to the left's while nothing is stereo-live.
	float combDelayR_ {0.0f};
	float combDelayTargetR_ {0.0f};
	float combDelayIncR_ {0.0f};
	float combFeedbackR_ {0.0f};
	float combDampR_ {0.0f};
	float combDampGainR_ {1.0f};
	/// Same rule as spreadRunning_ and butterRunning_: a line that comes back after the type moved
	/// away and back would ring a tail from two settings ago, so it is cleared as it starts running
	/// rather than on every reset — 32 KB of memset per note-on is not free either.
	bool combRunning_ {false};

	/// THE BRICKWALL HP (backlog NL), one per channel — the right ear's corner when a stereo route splits it. Cleared
	/// as it starts running, the comb's rule.
	feathers::dsp::EllipticHighpass brickwall_[2] {};
	bool brickwallRunning_ {false};

	/// Blend weights, both zero when their mechanism is neutral, and both used as `a + w * (b - a)`
	/// so that a zero weight is the identity in floating point and not merely in the limit.
	float morphWeight_{0.0f};
	float spreadWeight_{0.0f};
	float driveAmount_{0.0f};
	float driveGain_{1.0f};
	float driveComp_{1.0f};
	/// THE RIGHT CHANNEL'S TWINS of the five above (backlog KU). `mirrorRightEar` makes them equal to the left's
	/// on the mono road, which is what keeps every right-channel sample the bits it was.
	float morphWeightR_{0.0f};
	float spreadWeightR_{0.0f};
	float driveAmountR_{0.0f};
	float driveGainR_{1.0f};
	float driveCompR_{1.0f};

	int untilTick_{0};
	bool active_{false};
	bool spreadRunning_{false};
	/// Which of the cascade's stages are in the signal this block. Held rather than recomputed in the
	/// sample loop, and each carries the same "start it clean when it begins running" rule the spread
	/// stage has always had: a section that comes back after a slope change with the state it held
	/// two settings ago would ring a stale tail into the signal.
	bool mainRunning_{true};
	bool butterRunning_{false};
	bool onePoleRunning_{false};
};


} // namespace feathers::fx::filter
