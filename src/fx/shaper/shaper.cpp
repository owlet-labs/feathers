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

#include "fx/shaper/shaper.h"

// FOR `fieldWithOverride` AND `kFieldNeutral` — the φ layer's field-modifier curve, used rather than
// restated. Header-only and inline, so this adds no link dependency on feathers-dsp; and CLAUDE.md's
// rule is the reason it is not copied: two functions that must agree at a point are two functions
// that stop agreeing.
#include "dsp/slice_build.h"

#include <string_view>


#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>
#include <numbers>

namespace feathers::fx::shaper {

namespace {

/// A ONE-POLE'S COEFFICIENT FOR A TIME CONSTANT, at the rate we actually have.
float onePole(double seconds, double sampleRate) {
	return static_cast<float>(1.0 - std::exp(-1.0 / (seconds * sampleRate)));
}

/// ONE STEP OF A GLIDE THAT ARRIVES, WHEREVER IT IS GOING. An exponential approach never reaches its target, and
/// the memory stages are exact only AT zero — so within a hair of the target it lands on it. The hair alone is not
/// enough away from zero: there a step of `(target - value) * coeff` rounds to nothing before the gap closes inside
/// 1e-6 — MEASURED, a move to 0.3 parked at 0.299997 for good — so it also lands when a step leaves the value where
/// it was. Every glide in the device uses this one; Bloom, Sag, Loop and Cheby had only the hair until backlog
/// FA's follow-up. A glide that lands through the hair is unchanged by the second test, bit for bit.
void glide(float& value, float target, float coeff) {
	const float before = value;
	value += (target - value) * coeff;
	if (value == before || std::fabs(target - value) < 1.0e-6f) {
		value = target;
	}
}

/// HOW FAR BELOW ZERO THE THRESHOLD SITS AT FULL MIX.
///
/// The wet blend is `(|dry| - threshold) * slope`, clamped to [0, 1]. At mix 1 the intent is "shape
/// everything", and a threshold of exactly zero would leave a sample AT zero with a blend of zero —
/// so the quietest part of every waveform would stay dry and the curve would have a dry notch
/// through its middle. Pushing the threshold just under zero closes it.
///
/// THE FIRMWARE ARRIVES AT THE SAME PLACE THROUGH FOUR INTEGER CONSTANTS. `kRequiredDiffQ16` is the
/// difference needed for the blend to saturate at the steepest slope the mix can produce, and
/// `kThresholdForFullWet` is that difference negated — which in the normalised domain is this
/// number. Written as one float because the q31 chain it comes out of describes a fixed-point
/// range and not an idea, and reproducing the chain would carry over an arithmetic this port does
/// not have.
// MOVED to shaper_curve.h, where the plot can reach it too — see kFullWetUndershoot there.

/// THE SUB-HARMONIC'S DEPTH AT FULL INTENSITY — a gain of 0.7 or 1.3 as the sign flips. The
/// firmware's `kSubBoostMax_Q16` (19660 in Q16), which is this.
constexpr float kSubBoostMax = 0.3f;

/// HOW FAR HYSTERESIS DISPLACES THE TABLE READ, at full intensity. The firmware's
/// `hysteresis * 320000000` in a q31 domain, which is this fraction of full scale.
constexpr float kHysteresisMax = 0.149012f;

/// THE LOWPASS'S CORNER, AS AN INTERVAL ABOVE THE NOTE. Intensity 0 puts it two octaves up and
/// intensity 1 puts it one — so turning the field's slew intensity UP brings the corner DOWN, which
/// is what makes the extra progressively soften rather than progressively brighten.
constexpr float kLpfOctaveMin = 1.0f;
constexpr float kLpfOctaveMax = 2.0f;

/// **THE TABLE IS READ AT THE EXCESS, NOT AT THE SIGNAL, AND WHAT IT RETURNS IS ADDED TO THE
/// THRESHOLD.** For `|x|` above the mix threshold `t`:
///
///     out = sign(x) · t  +  a · T( r · e )           a = h / R,   r = R / (h · m),   e = |x| − t
///
/// with `h = 1 − t` the headroom the shaped part may fill, `m` the table's steepest secant from the
/// origin on the half being read (`boundTable`), and `R >= 1` the sweep Drive chooses. Below the
/// threshold the sample is returned as it came. (A Knee pre-squash of `e` stood here from 2026-09-03 to
/// 2026-09-22 and is retired — see `shaper_curve.h` above `kneeFadeFor`.) Everything asked for is then true BY
/// CONSTRUCTION rather than by tuning, and each line is one of the constraints:
///
///   * NEVER ABOVE 0 dBFS, FOR ANY INPUT — **ON THE SHAPED PATH.** Since backlog FA the device's output is that path
///     crossfaded with the dry by `Mix`, and by design (2026-10-11) there is no clamp on the mix: below Mix 100% a hot
///     dry share leaves as it came, above 0 dBFS. What follows is about the wet. `|T| <= 1` after `normaliseTable`, so `|a·T| <= h` and
///     `|out| <= t + h = 1` — whatever position the table is read at, which is what makes it hold
///     with the extras on and with a rotated sample reading anywhere. And `t <= 1` at every mix
///     (1.0 at mix 0), so a sample above 0 dBFS is ALWAYS shaped and never reaches the output dry.
///     Before this the compare sat in a 0.25-padded domain, where mix 0.5 at unity drive was crossed
///     only by an input of 1.97: a +6 dB signal passed through untouched, at 2.0 (MEASURED).
///   * NEVER ABOVE THE DRY IT REPLACED. `|a·T(r·e)| <= a·m·r·e = e`, so `|out| <= t + e = |x|`.
///     the reason dry can exceed wet is because it can go >0dbfs while the table must
///     clip — the wet below the dry on a hot signal is the ceiling working.
///   * NO STEP AT THE CROSSING, AND NO KINK EITHER ON A TABLE THAT BENDS DOWN. `T(0) = 0` after
///     anchoring, and the slope at the crossing is `a·r·T'(0) = T'(0)/m`, exactly 1 wherever the
///     table sits below its own tangent — which it does across the brief's whole knee × colour grid —
///     and below 1 only where the field draws a deadzone, which is the deadzone showing.
///
/// DRIVE IS THE SWEEP, `R`, AND NOTHING ELSE. `R = 1` — drive 0, the default — reads the table so
/// slowly that a full-scale signal sees only its first rise, and the top reaches toward the
/// ceiling; a larger `R` sweeps the excess further into the table — into the folds that are this
/// device's character — and, because the unit-slope bound ties `a` to `r`, scales what it finds by
/// `1/R`. **So more drive is more curvature and more harmonics and never more level.** This is not
/// a tuning choice, it is what the two bounds above leave: a table whose first rise has slope `m`
/// can only be explored past `u = 1/m` by a signal below 0 dBFS if the ceiling it reaches is
/// lowered in the same ratio — `|out| <= |in|` says so directly. At mix 1.0 the threshold is at
/// zero and the "top" is the whole signal, so there full drive is a 12 dB drop on a 0 dBFS input;
/// at mix 0.5 the top can fall no further than the threshold it sits on, 5.6 dB. One constant sets
/// how far the knob goes; 4x reaches the folds of every folding table in the brief's grid
/// (`u = 4/m` is 0.27..0.67 there) and is where the level cost was judged to stop being worth more
/// table. The previous read — the signal times `32·p⁵` times a 0.25 pad, looked up directly — had
/// the shaped top ABOVE the dry by +0.5 dB at drive 0.6 and by +22 dB at drive 1.0 (mix 0.5),
/// because a table with an origin slope of 10 read at the signal multiplies it by 10 until it clips.
///
/// THE OLD `kInputReference` (0.25) IS GONE WITH THE OLD READ. It was the headroom that put a 0 dBFS
/// signal a quarter of the way up the table; the excess read has no such tier, because `r` is set
/// by the table's own slope. Its history — the port had not dropped a firmware tier, the reference's
/// material is 21.6 dB quieter than a bus, 0.25 gave 12 dB of that back — is in 9906e2f's ancestors.
///
/// **THE TABLE'S OUTPUT IS STILL THE DEVICE'S OUTPUT: NO POST GAIN.** By design the table
/// clips at 0 dBFS with no post gain. `a` is not a post gain — it is the ceiling the shaped part may
/// reach, and it is at most the headroom.
// MOVED to shaper_curve.h — see kSweepOctaves and sweepFor there.


/// HEADROOM BELOW WHICH THE READ IS NOT COMPUTED. At mix 0 the threshold is 1.0 and `h` is 0, so the
/// shaped part is exactly nothing: `a = 0` and the output is the threshold — a clip at 0 dBFS, which
/// is what "shape nothing below 0 dBFS" has to mean above it. Guarding here keeps `r = R/(h·m)`
/// finite; the read it would have produced is multiplied by zero anyway.
// MOVED to shaper_curve.h — see kMinHeadroom there.

/// THE DRIVE GAIN'S TIME CONSTANT. The firmware smooths its Q26 gain with a per-sample IIR whose
/// effective alpha is 0.001 at 44.1 kHz; that is this many seconds, expressed as a TIME so the same
/// smoothing is heard at every sample rate rather than the same coefficient being used at all of them.
constexpr float kDriveSmoothingSeconds = 0.0227f;

/// WHERE THE DC BLOCKER'S CORNER SITS. Low enough to be inaudible on anything a player would call
/// bass — a 20 Hz tone is down 0.3 dB — and high enough that the offset is gone within about 30 ms of
/// the curve changing.
constexpr float kDcBlockerHz = 10.0f;

/// ROTATE THE SAMPLE WORD RIGHT — the aliasing extra, and the one place this port has to leave float
/// to be faithful.
///
/// IT IS A BIT OPERATION AND THERE IS NO FLOAT EQUIVALENT. The firmware rotates a q31 sample, which
/// takes the low-order bits of the mantissa round to the top and produces a signal correlated with
/// the input in a way nothing continuous does — that correlation IS the effect. So the sample is put
/// into the same 32-bit signed representation the firmware uses, rotated, and brought back. Anything
/// smoother would be a different effect wearing this one's name.
///
/// SATURATING RATHER THAN WRAPPING ON THE WAY IN, because a float path can hand this a sample past
/// full scale and the firmware's q31 could not. Wrapping there would put a full-scale sign flip in
/// front of an effect that is already the rudest thing in the device, on inputs a player has every
/// right to send. THE CONSEQUENCE, said plainly (Shaper parity sweep, 2026-09-16): a sample above
/// 0 dBFS rotates as exactly full scale, so the effect stops varying with level up there — and the
/// firmware rotates the value already scaled into its table domain, so it never saw such a sample.
[[nodiscard]] float rotateRight(float x, int32_t bits) {
	if (bits <= 0 || bits > 31) {
		return x;
	}
	const auto word = static_cast<uint32_t>(
	    static_cast<int32_t>(std::clamp(x, -1.0f, 1.0f) * 2147483647.0f));
	const auto rotated = static_cast<uint32_t>((word >> bits) | (word << (32 - bits)));
	return static_cast<float>(static_cast<int32_t>(rotated)) / 2147483648.0f;
}

/// A NOTE'S FREQUENCY. A4 = 69 = 440 Hz, the convention `device::NoteContext` states.
[[nodiscard]] float hzOf(float semitones) {
	return 440.0f * std::exp2((semitones - 69.0f) / 12.0f);
}

} // namespace

Shaper::Shaper() {
	// A STRAIGHT LINE IN BOTH TABLES AT CONSTRUCTION, not zeros.
	//
	// ZEROS WOULD BE SILENCE, AND SILENCE IS WHAT A NOT-YET-BUILT TABLE WOULD PLAY. The device is
	// switched off by default and takes nine blocks to build its first table, so there is a window in
	// which a chain could be enabled and the table asked for an answer it does not have yet. The
	// identity is the only honest thing to have there: the worst case becomes "the effect has not
	// arrived yet", which is what it is, rather than "the effect muted the track".
	for (auto& table : bank_->table) {
		for (size_t i = 0; i < kTableEntries; ++i) {
			table[i] = static_cast<float>(i) * (2.0f / static_cast<float>(kTableEntries - 1)) - 1.0f;
		}
	}
	prepare(48000.0);
}

void Shaper::prepare(double sampleRate) {
	sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
	sweepAlpha_ = 1.0f
	              - std::exp(-1.0f / (kDriveSmoothingSeconds * static_cast<float>(sampleRate_)));
	fadeStep_ = 1.0f / static_cast<float>(kFadeSamples);
	// **THE METER'S FALL, CONVERTED FROM dB PER SECOND AT THE RATE WE ACTUALLY HAVE.** A per-sample
	// constant would change meaning with the sample rate — the display falling twice as fast at 96 kHz
	// as at 48 — which is what the number it replaces did. See `kDriveFallDbPerSecond`.
	driveDecay_ = std::pow(10.0f, -kDriveFallDbPerSecond / (20.0f * static_cast<float>(sampleRate_)));
	envAttack_ = onePole(kMemoryAttackSeconds, sampleRate_);
	envRelease_ = onePole(kMemoryReleaseSeconds, sampleRate_);
	loopAlpha_ = static_cast<float>(
	    1.0 - std::exp(-2.0 * std::numbers::pi * kLoopCornerHz / sampleRate_));
	dcPole_ = std::clamp(
	    1.0f - 2.0f * 3.14159265f * kDcBlockerHz / static_cast<float>(sampleRate_), 0.0f, 0.99999f);
	reset();
}

void Shaper::reset() {
	channel_ = {};
	mixPrimed_ = false; // the next block snaps the mix staging, as a just-switched-on device does
	// SNAPPED, NOT SMOOTHED. A reset means the device was just switched on, and a gain that ramped up
	// from the last note's would be exactly the carried-over state this entry point exists to
	// discard — `AudioOps::reset`'s own comment, about a filter remembering it was running.
	sweep_ = sweepTarget_;
	voiceFade_ = 1.0f;
	snapNext_ = true;
	ceilingExcess_ = 0.0f;
	// A VOICE READING A SHARED BANK FORGETS ITS SIGNAL AND NOTHING ELSE (backlog OM): the bank is its bus instance's,
	// and a note starting on one voice must not put every voice's table back to the identity.
	if (bank_ == nullptr || bank_ != own_.get()) {
		return;
	}
	bank_->fade = 1.0f;

	// AND THE TABLE GOES BACK TO THE IDENTITY, which this function used to keep.
	//
	// IT KEPT IT ON AN ARGUMENT THAT SOUNDED RIGHT AND WAS ANSWERING THE WRONG QUESTION: the table is
	// a pure function of the settings, a reset does not change the settings, so rebuilding it spends
	// 127 us arriving at the table already in hand. True, and irrelevant — `AudioOps::reset` is not
	// about efficiency, it is about DETERMINISM. Its contract is "as if just switched on", and a
	// just-switched-on Shaper has identity tables and nine blocks of building ahead of it. Keeping
	// the old curve made a reset device and a fresh device on the same settings two different
	// instruments for the first 10 ms.
	//
	// CAUGHT BY `test_fx_runner.cpp`'s "EVERY audio device: reset means AS IF JUST SWITCHED ON" —
	// a sweep driven from the registry, so registering the device was enough to be asked. It reported
	// 256 of 256 samples differing, worst 1.052. Nothing in this file's own suite could have seen it:
	// every case here resets a device onto the settings it already had, which is precisely the
	// knife that cannot cut, and that test's own helper comment says so.
	for (auto& table : bank_->table) {
		for (size_t i = 0; i < kTableEntries; ++i) {
			table[i] = static_cast<float>(i) * (2.0f / static_cast<float>(kTableEntries - 1)) - 1.0f;
		}
	}
	bank_->bound = {};
	ceilingExcess_ = 0.0f;
	bank_->front = 0;
	bank_->fadeFrom = 0;
	bank_->built = Settings {};
	bank_->latched = Settings {};
	bank_->building = false;
	bank_->everBuilt = false;
	snapNext_ = true;
	bank_->cursor = 0;
	bank_->budget = 0.0f;
	bank_->extent = {};
}

void Shaper::becomeVoice() noexcept {
	// THE MESSAGE THREAD, ONCE, right after construction (`AudioOps::setVoice`): a voice builds no table of its own.
	own_.reset();
	bank_ = nullptr;
}

void Shaper::shareFrom(const Shaper& bus) noexcept {
	if (&bus == this || bus.own_ == nullptr) {
		return;
	}
	// A CONTEMPORANEOUS HANDLE, re-read every block: the bus instance lives as long as its slot's device, and the
	// host hands it again before every block it runs these voices in (CLAUDE.md's address rule).
	shared_ = &bus;
	bank_ = bus.own_.get();
}

void Shaper::buildNow() {
	const size_t back = 1 - bank_->front;
	bank_->latched = target_;
	bank_->extent = {};
	fillTable(curveOf(bank_->latched), bank_->table[back], 0, kTableEntries, bank_->extent);
	normaliseTable(bank_->table[back], bank_->extent);
	bank_->bound[back] = boundTable(bank_->table[back]);
	bank_->fadeFrom = back;
	bank_->front = back;
	bank_->fade = 1.0f;
	bank_->cursor = 0;
	bank_->budget = 0.0f;
	bank_->built = bank_->latched;
	bank_->everBuilt = true;
	bank_->building = false;
	++bank_->rebuilds;
}

void Shaper::beginSharedBlock(int frames) {
	if (own_ == nullptr) {
		return;
	}
	linear_ = target_.linear();
	// A BANK ITS VOICES WOULD HEAR AS THE IDENTITY — fresh, or reset when Poly came on — IS BUILT AT ONCE, so a voice
	// seat plays the device's table from its first block (by design: no "default curve for three blocks").
	if (!linear_ && !bank_->everBuilt && !bank_->building) {
		buildNow();
		return;
	}
	// OTHERWISE THE BUS ROAD'S OWN AMORTISED BUILD AND CROSSFADE, `frames` samples of them, as if it had run the audio.
	maybeStartBuild();
	for (int i = 0; i < frames; ++i) {
		advanceBuild();
		if (bank_->fade < 1.0f) {
			bank_->fade = std::min(bank_->fade + fadeStep_, 1.0f);
		}
	}
}

void Shaper::setTarget(const Settings& settings) {
	target_ = settings;
}

void Shaper::setNoteContext(const device::NoteContext& note) {
	// GATE IGNORED, PITCH KEPT. The corner should stay where the last note put it through the release
	// rather than snapping to the reference the instant a key lifts — a filter that moved on note-off
	// would be audible on every note, and the note context's own contract is that the two facts arrive
	// together and mean different things.
	noteHz_ = hzOf(note.pitchSemitones);
}

void Shaper::beginBlock(int frames) {
	// **A VOICE ON THE SHARED TABLE (backlog OM)** plays its bus instance's table, so the settings the table is BAKED from
	// are the bus's — the device's own row, resolved once per block — whatever a per-voice route said; everything read
	// per sample (Drive, Threshold, the extras' amounts, Mix, ...) stays this voice's. The list is `sameCurveAs`'s.
	if (shared_ != nullptr) {
		const Settings& bus = shared_->target_;
		target_.knee = bus.knee;
		target_.colour = bus.colour;
		target_.gamma = bus.gamma;
		target_.travel = bus.travel;
		target_.deadzone = bus.deadzone;
		target_.deadzoneOn = bus.deadzoneOn;
		target_.colourMode = bus.colourMode;
		target_.inflateAmount = bus.inflateAmount;
		voiceFade_ = bank_->fade;
	}
	// THE FIELD, EVERY BLOCK, BECAUSE IT IS CHEAP AND THE TABLE IS NOT. `deriveCurve` is triangles and
	// multiplies; the table is 1025 evaluations of six basis functions. Separating them is what lets
	// Harmonics and the five switches take effect on the next sample while Knee, Colour and Gamma go
	// through the amortised build — and it is why `sameCurveAs` names exactly the four inputs the
	// TABLE depends on rather than every setting that reaches this function.
	// **ONE PHASE INPUT AGAIN.** This summed `gamma + colourPhase`, the firmware's
	// `shapeYPhaseOffset + gammaPhase`. `Colour Phase` is gone (none of these should be
	// bounded): it was measurably a second gamma knob, and Colour carries its own laps now.
	curve_ = curveOf(target_);
	// THE READ SHAPES (backlog IM) — Stair and Cheby at the read, every block from the curve. The Cheby
	// crossfade glides per frame, on the mix staging's half-life, so a modulated or dragged amount does not
	// step at the block rate; Stair is a grid and steps anyway.
	shapesTarget_ = readShapesOf(curve_);
	{
		const float glidingCheby = shapes_.chebyMix;
		shapes_ = shapesTarget_;
		shapes_.chebyMix = glidingCheby;
	}
	linear_ = target_.linear();
	// **AND HOW MUCH OF THE TABLE IS IN THE SIGNAL AT ALL** — the abrupt jump off minimum knee.
	// At the block rate because it is a function of one number and needs
	// no build, so it follows the hand at once. One law, `kneeFadeFor`, which the plot asks too.
	// THE TRIM AS A LINEAR GAIN, at the block rate for the same reason the fade is:
	// it is a function of one number and needs no build, so it follows the hand at once.
	trimGain_ = std::pow(10.0f, target_.trim / 20.0f);
	kneeFade_ = kneeFadeFor(target_.knee);
	// EMPHASIS AT THE BLOCK RATE — a first-order section whose coefficients follow the knob; no table
	// term, so no rebuild. Not smoothed: a first-order tilt stepping once a block is a gentle move.
	emphasisIn_ = tiltSection(static_cast<double>(target_.emphasisDb), kEmphasisPivotHz, sampleRate_, false);
	emphasisOut_ = tiltSection(static_cast<double>(target_.emphasisDb), kEmphasisPivotHz, sampleRate_, true);
	// THE INPUT FILTERS (backlog FA), at the block rate like Emphasis: a one-pole's coefficient stepping once a
	// block is a gentle move. Each is OFF at its open end — a corner of 0 Hz here — and an off filter is not
	// run at all, which is what keeps a fresh device the device it was to the bit.
	inputHpfHz_ = inputHighpassOn(target_.inputHpf) ? std::exp2(target_.inputHpf) : 0.0f;
	inputLpfHz_ = inputLowpassOn(target_.inputLpf) ? std::exp2(target_.inputLpf) : 0.0f;
	inputHpfAlpha_ = inputHpfHz_ > 0.0f ? inputFilterAlpha(target_.inputHpf, sampleRate_) : 0.0f;
	inputLpfAlpha_ = inputLpfHz_ > 0.0f ? inputFilterAlpha(target_.inputLpf, sampleRate_) : 1.0f;

	if (shared_ == nullptr) {
		maybeStartBuild(); // a voice builds nothing: its bus instance keeps the bank (`beginSharedBlock`)
	}

	// THE SWEEP — 4^drive, so drive 0 reads the table at the unit-slope minimum and drive 1 reads it
	// four times faster, each quarter of the travel 41 % more sweep and 3 dB less ceiling. Exponential
	// so the knob is even in the only units the ear has for it; see the note on `kSweepOctaves`.
	sweepTarget_ = sweepFor(target_.drive);
	// SNAPPED ON THE FIRST BLOCK AFTER A RESET, not in `reset()` itself. A reset arrives BEFORE the
	// settings it is being reset onto — the host's order is reset, then fields — so a snap taken in
	// `reset()` would park the sweep on the OLD target and ramp away from it. This is the same
	// "as if just switched on" the tables get, applied to the one quantity that smooths.
	if (snapNext_) {
		sweep_ = sweepTarget_;
		snapNext_ = false;
	}

	// THRESHOLD (named Mix until backlog FA) IS AN AMPLITUDE THRESHOLD, NOT A CROSSFADE, and this is the whole of it. At mix 0 the
	// threshold is at full scale and nothing reaches the curve; as mix rises the threshold descends
	// and the LOUD parts of the waveform cross it first. So a low mix is not "a little of the effect
	// everywhere", it is "the effect on the peaks" — which is what the firmware's testing guide means
	// by "Mix 50% → effect only applies to the peaks of the waveform", and it is a materially
	// different thing to have on a knob.
	//
	// IN INPUT UNITS, COMPARED AGAINST `|x|` ITSELF. It used to be compared against the signal after
	// the drive gain and a 0.25 pad, which put the crossing at 3.59 / 1.97 / 0.34 of full scale for
	// mix 0.1 / 0.5 / 0.9 at unity drive (MEASURED) — above 0 dBFS for every mix below about 0.75,
	// so a hot input reached the output through the dry path and clipped the format. Now the
	// crossing is 0.90 / 0.49 / 0.09 at those mixes and 1.0 at mix 0, at every drive: nothing above
	// 0 dBFS is ever dry.
	// THE STAGING, FROM THE SHARED DERIVATION — see `stagingFor`. The plot needs the same three
	// numbers to draw the same curve, and two spellings of them is how it drifted.
	const Staging staging = stagingFor(target_.threshold, target_.aggression);
	headroom_ = staging.headroom;
	// **THE THRESHOLD AND THE BLEND SLOPE ARE SMOOTHED, AS THE FIRMWARE SMOOTHS THEM** (Shaper parity
	// sweep, 2026-09-16): it closes half the distance to each target every buffer and snaps on first
	// use, so a Mix or Aggression move — both modulatable here — reaches the sample as a short glide
	// rather than a step in the threshold every shaped sample is offset by. Its buffer is 128 frames
	// at 44.1 kHz; the half-life is kept in TIME and stepped PER SAMPLE (`advanceFrame`), because a
	// per-block step of the same total is what the firmware does and is block-size dependent by
	// construction — the registry's partition case caught the first draft at 0.09. The picture
	// (`stagingFor`) draws the target, which is what a hand set.
	thresholdTarget_ = staging.threshold;
	blendSlopeTarget_ = staging.blendSlope;
	// Half-life of one 44.1 kHz 128-frame buffer, as a per-sample one-pole coefficient at our rate.
	mixCoeff_ = 1.0f - static_cast<float>(std::pow(0.5, 44100.0 / (128.0 * sampleRate_)));
	if (!mixPrimed_) {
		threshold_ = thresholdTarget_;
		blendSlope_ = blendSlopeTarget_;
		// THE MEMORY AMOUNTS PRIME HERE TOO, and not in `reset`: a fresh device reaches this branch on
		// its first block and a reset one on its next, so both start from the knobs rather than one of
		// them gliding in from zero — MEASURED, snapping in `reset` alone made a reset device differ from
		// a just-switched-on one in 256 samples of the reset sweep.
		bloom_ = target_.bloom;
		sag_ = target_.sag;
		loop_ = target_.loop;
		shapes_.chebyMix = shapesTarget_.chebyMix;
		dryWet_ = target_.dryWet;
		mixPrimed_ = true;
	}
	// LOOP'S REFERENCE SLOPE, from the curve as it stands this block — see `processSample`.
	// **A CENTRAL DIFFERENCE WITH ANY STEP AT ZERO TAKEN OUT** — there was one at Mix 1 until backlog IF (see
	// `processSample`):
	// the one-sided secant `f(p) / p` read the step as a slope — MEASURED ~770 — and the loop's
	// residue was never zero at silence.
	silence_ = staticStage(0.0f);
	loopSlope_ = std::fmax(
	    (staticStage(kLoopProbe) - staticStage(-kLoopProbe) - 2.0f * silence_) / (2.0f * kLoopProbe), 1.0f);
	(void)frames;
	// AND THE CROSSFADE STEEPENS WITH MIX ALONE. The mix term decides how abruptly the shaped path
	// takes over once the threshold is crossed. It is divided by the headroom because the ceiling
	// requires it: the crossfade's dry side is `x` itself, which a hot input carries above 1.0, so
	// the blend has to be complete by the time `|x|` reaches 1.0 — the excess reaches `h` — or a
	// sample just above full scale would come out part-dry and above the ceiling (worked example:
	// mix 0.1, x 1.2, a slope of 1.64 in input units gives out 1.096).
	//
	// **THE KNEE TERM IS GONE.** The firmware's `blendAggression` — `0.1 + knee² · 1.9` — scaled
	// this slope by 20x across the Knee sweep, which put Knee inside the mix crossfade: the width
	// from the crossing to 95 % wet ran 2.23 of full scale at knee 0.01 to 0.11 at knee 1.0
	// (MEASURED, mix 0.5, unity drive). It was there so a hard knee would SNAP into a curve that
	// left the dry level in a step; with the shaped side now starting AT the dry level, with unit
	// slope, there is no step for Knee to hide, and Knee's only job is to bend the curve. Mix is
	// where the bend starts and how abruptly; Knee is how hard it bends. (The slope itself is
	// assigned above, smoothed beside the threshold.)

	//-- the extras, valued by the field and SCALED by their knobs (backlog II) -----------------------//
	// Each is live iff its scaled amount is above zero — `curveOf` applied the scalers, so a knob at 0
	// is an amount of 0 is off, and there is no second switch that could disagree with it.

	subActive_ = curve_.subIntensity > 0.0f;
	feedbackActive_ = curve_.feedback > 0.0f;
	// BOTH MAY RUN, WHICH IS A DELIBERATE DIVERGENCE FROM THE FIRMWARE.
	//
	// There, `shaper_buffer.h` gives the integrator precedence and the code's own parenthetical says
	// why: they share one intensity and one piece of state, and are "mutually exclusive in practice".
	// **The precedence was a guard on a shared variable, not a designed sound** — and on a surface
	// where both are labelled switches a player can see, it becomes a switch that silently disables
	// another, which is exactly what the honesty rule forbids. Two state floats cost nothing and the
	// chain is well defined: the lowpass is a PRE-transform reading the dry signal, and the
	// integrator accumulates whatever reaches it.
	//
	// AND THE REGISTRY'S OWN SWEEP IS WHAT FORCED THE QUESTION. "EVERY device with a NOTE road:
	// fields and notes may arrive in either order" drives every stepped control to its ceiling — so
	// all five extras ON — and under the firmware's precedence the integrator won, the lowpass never
	// ran, and the lowpass is the ONLY thing the note reaches. The case failed on its non-vacuity
	// guard: "the note reaches nothing on Shaper". A declared note road that is dead in the maximal
	// configuration is the filter's own shipped bug wearing a different hat.
	integratorActive_ = curve_.integratorAmount > 0.0f;
	lowpassActive_ = curve_.lowpassAmount > 0.0f;

	// NOT PORTED: THE FIRMWARE'S FORCED LOWPASS. `shaper_buffer.h:376` switches the lowpass ON,
	// regardless of the mask, whenever gammaPhase is 0 and the slew intensity is above zero — a
	// convenience for square-wave oscillators on the hardware, where the mask lives behind a secret
	// menu most players never open. Here the switch is on the panel with a label on it, and a switch
	// that reads Off while the filter runs is exactly what the honesty rule forbids. The behaviour is
	// still reachable: turn Lowpass up.

	subBoost_ = subActive_ ? curve_.subIntensity * kSubBoostMax : 0.0f;
	subRatio_ = curve_.subRatio;
	stride_ = std::clamp(curve_.stride, 1, 128);
	feedbackAmount_ = feedbackActive_ ? curve_.feedback : 0.0f;
	rotation_ = curve_.rotation;
	integratorBlend_ = integratorActive_ ? curve_.integratorAmount : 0.0f;

	if (lowpassActive_) {
		// THE CORNER TRACKS THE NOTE, which is what makes this "shape only the low harmonics" rather
		// than "shape below 300 Hz". An absolute corner means a different interval on every pitch, so
		// the same setting would soften a bass note and do nothing to a lead.
		const float octaves = kLpfOctaveMax - (kLpfOctaveMax - kLpfOctaveMin) * curve_.lowpassAmount;
		lowpassCutoff_ = noteHz_ * std::exp2(octaves);
		// THE EXACT ONE-POLE COEFFICIENT, not the firmware's linear `2π·fc/fs`.
		//
		// MEASURED WHY IT MATTERS: that approximation is fine while fc is small against the rate and
		// diverges where this device puts it. At A5 the corner lands at 3520 Hz, where the linear form
		// gives alpha 0.46 against the exact 0.37 — and it passes 1.0 at 7.6 kHz, which is a one-pole
		// that has stopped filtering and started ringing. A note two octaves above A5 reaches that.
		lowpassAlpha_ = std::clamp(
		    1.0f - std::exp(-2.0f * 3.14159265f * lowpassCutoff_ / static_cast<float>(sampleRate_)),
		    0.0f, 1.0f);
	}
	else {
		lowpassCutoff_ = 0.0f;
		lowpassAlpha_ = 0.0f;
	}

	// IN EXCESS UNITS, AS A FRACTION OF THE HEADROOM — which keeps the relationship the offset has
	// always had to the SIGNAL rather than to the table.
	//
	// The firmware's offset is absolute in the table's domain, and this port scaled it by the input
	// pad for a reason its comment recorded: a fixed table offset is loud in the middle of the range
	// where a bus signal lives (75 % of the signal at colour 0.3, and it inverted the knee's THD
	// ladder), so the offset was made a fixed fraction of a full-scale signal's SPAN on the table —
	// 18.9 % at colour 0.3, either way. The excess read has no pad and no fixed span: a full-scale
	// signal's excess is `h`, and its span on the table is `r·h`. Expressing the offset as a fraction
	// of `h` and letting the read rate carry it onto the table is that same rule, restated for a read
	// whose span the table's own slope sets.
	//
	// THE FIELD, THEN THE KNOB IN FRONT OF IT. `fieldWithOverride` is the φ layer's own curve — 0 is
	// none of the field, 0.5 is the field exactly as this position authored it, 1 is all of it — and
	// six of the flagship's fields already sit behind it.
	//
	// BIASED ON THE MAGNITUDE AND THE SIGN REAPPLIED, because the curve's domain is [0,1] and this
	// field is bipolar. Biasing the raw triangle instead would make knob 0 mean "hysteresis at its
	// full NEGATIVE extreme" rather than "no hysteresis", which is the opposite of what the label
	// says — the same trap `pointFieldWithOverride` records for the point count.
	const float hystField = curve_.hysteresis;
	const float hystMagnitude = fieldWithOverride(std::fabs(hystField), target_.hysteresis);
	hystOffset_ = std::copysign(hystMagnitude, hystField) * kHysteresisMax * headroom_;
}

/// THE DC BLOCKER IS NOT OPTIONAL AND IT IS NOT TIDINESS — it is the fix for a measured defect.
///
/// **A NORMALISED TABLE DOES NOT MAP ZERO TO ZERO.** `normaliseTable` centres on the MIDPOINT OF THE
/// EXTREMES, which is what makes the curve fill [-1, +1] and use the whole table; but for any curve
/// that is not an odd function that midpoint is not f(0). Four of the six bases are not odd, and the
/// field sets `asymmetry` away from 0.5 nearly everywhere, so nearly every position in the field has
/// f(0) != 0 — and the amplitude blend does not save it, because at full mix the threshold sits BELOW
/// zero precisely so that the quietest sample is still shaped.
///
/// MEASURED, and it is not subtle: a channel fed exact silence came out at **-0.112, which is
/// -19 dBFS of pure DC.** On the reference this is survivable because the shaper sits after a per-voice
/// envelope, so "silence in" means the voice is not running; on a bus it is a constant offset in the
/// mix and a click every time the curve changes under a hand.
///
/// FOUND BY A CASE, WHICH IS THE PART WORTH RECORDING. Nothing about a spectrum plot or an A/B render
/// shows it — DC is the one bin nobody looks at — and the case that caught it was asking a completely
/// different question (whether the two channels share state). It is exactly CLAUDE.md's shape: a
/// PARTIAL effect surviving where a total one would have been caught.
///
/// BLOCKING RATHER THAN ANCHORING THE TABLE AT f(0) = 0, and the choice matters. Anchoring removes
/// the offset at silence only; asymmetric shaping produces an amplitude-DEPENDENT offset too, which
/// is inherent to even-harmonic generation and is the whole point of the asymmetry control. A one-pole
/// blocker removes both, and costs three operations.
float Shaper::blockDc(float x, ChannelState& state) {
	state.dcOut = x - state.dcIn + dcPole_ * state.dcOut;
	state.dcIn = x;
	// FLUSHED WHEN IT STOPS MATTERING, because an IIR decays asymptotically and float arithmetic
	// parks the result on a denormal rather than reaching zero. MEASURED: the tail settled at
	// 5.3e-43 and stayed there for the whole 4.3-second window `test_fx_runner.cpp`'s silence sweep
	// watches. -600 dBFS is inaudible and is still not silent, and the sweep is right to insist on
	// the difference: a device that never reaches zero keeps a denormal-slow path warm forever.
	//
	// ONLY THE OUTPUT IS FLUSHED. Zeroing `dcIn` as well — which the first version did — makes the
	// blocker see the very next sample as a STEP UP FROM ZERO, so a constant input re-triggers the
	// whole decay and the tail restarts forever. MEASURED: it turned a device that settled into one
	// that was still sounding at block 399 of 400, which is the failure this flush was added to fix,
	// reappearing inside the fix.
	if (std::fabs(state.dcOut) < 1.0e-30f) {
		state.dcOut = 0.0f;
	}
	return state.dcOut;
}

/// THE CEILING, AFTER THE BLOCKER — and it is a TRIM, not the mechanism.
///
/// Everything the read produces is inside ±1.0 by construction (see `kSweepOctaves`). The one stage
/// after the read that is not is the DC blocker, because a highpass cannot be bounded by its input's
/// bound: an asymmetric shaped wave carries an offset, removing it shifts the wave, and the peak on
/// the far side moves OUT by the offset. MEASURED: a +6 dB sine at colour 0.38, whose shaped peaks
/// sit at the ceiling, left the blocker at 1.009. The constraint is the format's, so the last
/// line clamps — and `ceilingExcess_` records the most it has ever had to, so the ceiling case can
/// assert that the number is the blocker's DC and not the read's.
float Shaper::toOutput(float y, ChannelState& state) {
	// **EMPHASIS OUT** — the exact inverse of the section in `processSample`, before the blocker.
	const float blocked = blockDc(emphasisOut_.step(y, state.emphasisOut), state);
	const float over = std::fabs(blocked) - 1.0f;
	if (over > 0.0f) {
		ceilingExcess_ = std::fmax(ceilingExcess_, over);
		return std::copysign(1.0f, blocked);
	}
	return blocked;
}

void Shaper::maybeStartBuild() {
	if (!bank_->building) {
		// A BUILD IS STARTED ONLY FROM A SETTLED STATE — nothing in flight, and no fade still running.
		//
		// THE FADE GUARD IS WHAT KEEPS TWO TABLES ENOUGH. Swapping while a previous swap is still
		// crossing would need the outgoing, the incoming and the one being written: three. Waiting is
		// free, because a fade is 512 samples and a build is nine blocks.
		if (linear_ || (bank_->everBuilt && target_.sameCurveAs(bank_->built)) || fadeRef() < 1.0f) {
			return;
		}
		// LATCHED HERE AND NOT RE-READ UNTIL THE BUILD LANDS. This is the line that stops a drag from
		// restarting the build every block and freezing the curve at whatever it was when the hand
		// started moving.
		bank_->latched = target_;
		bank_->extent = {};
		bank_->cursor = 0;
		bank_->budget = 0.0f;
		bank_->building = true;
	}
}

/// SPENT INSIDE THE SAMPLE LOOP, which is what makes the schedule deterministic — see
/// `kBuildEntriesPerFrame`. After N samples exactly 2N entries exist, however the host chopped the
/// buffer up, so the table lands at the same point in the audio every time.
void Shaper::advanceBuild() {
	if (!bank_->building) {
		return;
	}
	bank_->budget += static_cast<float>(kBuildEntriesPerFrame);
	const auto owed = static_cast<size_t>(bank_->budget);
	if (owed == 0) {
		return;
	}
	bank_->budget -= static_cast<float>(owed);

	const size_t back = 1 - bank_->front;
	// **THROUGH THE TRAVEL, WHICH THIS CALL USED TO DROP — AND IT IS THE TABLE, SO IT IS THE SOUND.**
	//
	// `curveFor` takes `travel` with a DEFAULT OF NULLPTR. `beginBlock` passes it; this did not. So
	// the pad's displacement reached `curve_` — where it feeds the per-sample modifier terms — and
	// never reached the SIX BASIS WEIGHTS, which live only in the table. Moving the puck changed the
	// curve object and left the shape the signal is actually read through exactly as it was.
	//
	// **THE CASE THAT SHOULD HAVE CAUGHT THIS STOPPED ONE STEP SHORT.** "travel in the blob reaches
	// the curve" asserts on `deriveCurve`'s output — deliberately, because an earlier version had
	// asserted on a render and been fooled by a fold. It was right to leave the render and wrong to
	// stop at the params: the params are not what the signal is read through. `sameCurveAs` names
	// `travel` and so the rebuild was correctly REQUESTED every time; it was the rebuild's own read of
	// the settings that was short.
	//
	// MEASURED before the fix, at knee 0.03 colour 0.38 with an eight-phase travel: the built table
	// was IDENTICAL — 0 of 1025 entries differing — while `curve_.chebyWeight` moved 0.745 -> 0.379.
	const CurveParams latchedCurve = curveOf(bank_->latched);
	const size_t next = std::min(bank_->cursor + owed, kTableEntries);
	fillTable(latchedCurve, bank_->table[back], bank_->cursor, next, bank_->extent);
	bank_->cursor = next;

	if (bank_->cursor < kTableEntries) {
		return;
	}

	// THE LAST SLICE LANDED: centre it, normalise it, bound it, and start it crossing in.
	//
	// THE BOUND IS A SECOND PASS OVER THE TABLE, in the same sample as the normalising pass and of
	// the same size — one divide per entry against one multiply. Both are paid on the frame the
	// last slice lands and on no other, which the per-sample budget already accepts for the first.
	normaliseTable(bank_->table[back], bank_->extent);
	bank_->bound[back] = boundTable(bank_->table[back]);
	bank_->fadeFrom = bank_->front;
	bank_->front = back;
	fadeRef() = 0.0f;
	bank_->built = bank_->latched;
	bank_->everBuilt = true;
	bank_->building = false;
	++bank_->rebuilds;
}

float Shaper::readTable(size_t which, float excess) const {
	// **ONE DEFINITION, IN `shaper_curve.h`, BECAUSE THE PLOT READS IT TOO.** This body was here and
	// the plot restated a lookalike; the two drifted twice in a day. Lifting it means a change to the
	// read reaches the drawn curve by construction rather than by somebody remembering.
	return readTableAt(bank_->table[which], bank_->bound[which], headroom_, sweep_, excess, shapes_);
}

float Shaper::shape(float excess) const {
	// ONE READ PER TABLE, and a fade between two tables is a fade between two reads.
	const float value = readTable(bank_->front, excess);
	if (fadeRef() >= 1.0f) {
		return value;
	}
	const float previous = readTable(bank_->fadeFrom, excess);
	return previous + (value - previous) * fadeRef();
}

void Shaper::advanceFrame() {
	if (shared_ == nullptr) {
		advanceBuild();
	}
	sweep_ += (sweepTarget_ - sweep_) * sweepAlpha_;
	// THE MIX STAGING'S GLIDE, one step per frame — see beginBlock for the half-life and why it is here.
	threshold_ += (thresholdTarget_ - threshold_) * mixCoeff_;
	blendSlope_ += (blendSlopeTarget_ - blendSlope_) * mixCoeff_;
	// THE MEMORY AMOUNTS GLIDE ON THE MIX STAGING'S HALF-LIFE, and land EXACTLY: a knob returned to zero
	// must leave the stages exact again (`+ 0`, `/ 1`), not a denormal away from it forever, and a knob
	// moved anywhere else must be heard AT that value, not a stalled float beside it.
	glide(bloom_, target_.bloom, mixCoeff_);
	glide(sag_, target_.sag, mixCoeff_);
	glide(loop_, target_.loop, mixCoeff_);
	glide(shapes_.chebyMix, shapesTarget_.chebyMix, mixCoeff_);
	// THE CROSSFADE GLIDES ON THE SAME HALF-LIFE AND LANDS EXACTLY, so a Mix returned to 100% is the wet to the
	// bit — `dryWetMix` sees 1, not 0.999997.
	glide(dryWet_, target_.dryWet, mixCoeff_);
	if (fadeRef() < 1.0f) {
		fadeRef() = std::min(fadeRef() + fadeStep_, 1.0f);
	}
}

float Shaper::processSample(float x, int channel) {
	// A SAMPLE THAT IS NOT A NUMBER IS SILENCE HERE (backlog EH, shape 3), before it can reach the
	// table read, the threshold memory or the blocker's history — one NaN from the device before
	// would otherwise poison every stage's state for good. The table read guards itself as well.
	if (!std::isfinite(x)) {
		x = 0.0f;
	}
	ChannelState& state = channel_[static_cast<size_t>(channel)];

	// **THE TRIM, FIRST AND ON EVERYTHING.** It is an INPUT gain, so it moves the sample the threshold
	// is compared against and the dry a sub-threshold sample is returned as — which is exactly what
	// separates it from Drive. Drive scales the read POSITION and leaves the dry where it was, so it
	// only ever changes the shaped path; this changes what the whole device is looking at.
	//
	// ABOVE THE BYPASS TOO. At Knee 0 the device is a clamp and nothing else, and a trim that stopped
	// working there would be a control that silently depends on another — the honesty rule applied to
	// an interaction rather than to a knob. The clamp still holds afterwards, so a trimmed-up bypass
	// cannot leave the format.
	x *= trimGain_;
	// **THE INPUT FILTERS, THEN THE DRY TAP (backlog FA).** After the trim, so they filter what the device is
	// given at the level it is given it; before everything else, the bypass included, because the input filters are by design
	// tone control on the WHOLE device — so the crossfade's dry is taken here, after them.
	x = inputFilters(x, state);
	const float dry = x;
	// **EMPHASIS IN**, after the trim and before everything the curve sees — the threshold compare,
	// the extras, the read. Its inverse is the first thing `toOutput` does, so whatever passes through
	// linearly comes back as it went in, and only what the curve bent comes back tilted.
	x = emphasisIn_.step(x, state.emphasisIn);

	// **METERED HERE, AFTER THE TRIM AND IN ONE PLACE.** when i adjust the trim knob, the
	// changes arent shown in the level meter. They were not: `processStereo` and `processMono` both
	// metered `left[i]` straight out of the buffer, with a comment saying "before anything touches
	// it — the plot's x axis is the input axis". **That was true until the trim existed.** The meter
	// answers "how hard is the table being driven" — the words of the request — and the trim
	// sits between the buffer and the table, so the two stopped being the same number.
	//
	// It lives INSIDE `processSample` rather than at the two loops so there is no way for the metered
	// value and the driven value to drift: the sample that is metered is the sample that goes on to be
	// read. A `* trimGain_` restated at each loop would have been a third copy of the law.
	//
	// **CHANNEL 0 ONLY, which keeps it once per FRAME** — the two loops metered once per frame before
	// this and a per-channel meter would count L and R of the same frame twice. Before the bypass for
	// the same reason it was before it in the loops: a meter that goes dark at Knee 0 would say the
	// signal had stopped rather than that the curve had.
	if (channel == 0) {
		meterInput(x);
	}

	if (linear_) {
		// A TRUE BYPASS AT KNEE 0 — no table consulted — UNDER THE ONE RULE THAT OUTRANKS IT: the
		// output never exceeds 0 dBFS, for any input. A bypass that let a hot input through would be
		// the only setting of the device that could clip the format, so the bypass is clamped at
		// full scale and nothing else is done to it. Below 0 dBFS it is the input to the bit.
		//
		// THIS USED TO RETURN `driven`, on the firmware's argument that Drive was a control of its own.
		// Drive is a table sweep now; with no table there is nothing for it to sweep, and the panel
		// should say so — the honesty rule.
		// THROUGH THE DE-EMPHASIS, which undoes the pre-emphasis exactly here where nothing between them
		// is non-linear — so the bypass is still the input, at any Emphasis.
		preBlock_ = std::clamp(emphasisOut_.step(x, state.emphasisOut), -1.0f, 1.0f);
		// **THE BLOCKER'S HISTORY ADVANCES THROUGH THE BYPASS** (backlog EH, shape 6) — for the reason
		// the sub-threshold branch below gives in its own words: returning raw leaves `dcIn`/`dcOut`
		// standing where the last shaped sample left them, so the first sample after Knee crosses back
		// above zero sees a stale difference and gets a step of its own. Advanced for its history and
		// NOT applied, because at Knee 0 the promise is the input to the bit.
		(void)blockDc(preBlock_, state);
		return preBlock_;
	}

	// **THE MEMORY STAGES (backlog IE) — Bloom, Sag and Loop, all around the curve and none of them in
	// it**, which is how they move at audio rate without a table rebuild. With all three at zero every
	// operation below is exact — `+ 0`, `/ 1`, `* 1` — so the stage sees the input to the bit and the
	// firmware parity the Colour axis carries is untouched. ONE thing here is new at zero and is not a
	// memory stage: the stage's output is clamped to ±1 before the blocker rather than after it, see
	// below — the value the drawn transfer already claimed.
	//
	// ONE ENVELOPE FOR BOTH OF THE PLAYING-DEPENDENT ONES, because "how hard and how long" is one
	// quantity. It tracks whether or not anything reads it, so a knob turned up mid-note starts from
	// the note's real level rather than from silence.
	const float level = std::fabs(x);
	state.envelope += (level - state.envelope) * ((level > state.envelope) ? envAttack_ : envRelease_);
	const float envelope = std::fmin(state.envelope, 1.0f);

	// BLOOM: an offset into the curve that grows with the envelope, so one side meets the curve first.
	const float bias = bloom_ * kBloomDepth * envelope;
	// SAG: the rails come in. `rail * stage(u / rail)` is the same curve on smaller rails — the linear
	// region is unchanged and the ceiling is `rail`.
	const float rail = std::fmax(1.0f - sag_ * kSagDepth * envelope, kSagFloor);

	// LOOP: what the curve FAILED TO PASS, in its own input's units and darkened, goes back into its
	// input — so a compressed peak is pushed harder next time round. The residue is
	// `u - (f(u) - anchor) / g0`:
	// the input the curve's own small-signal slope `g0` would have needed to produce what came out,
	// subtracted from the input it actually got.
	//
	// **TWO DRAFTS FAILED, AND EACH FAILURE IS WHY THIS FORM IS THIS FORM.** The first fed back
	// `y - input`: at Mix 1 there is no linear region (the threshold sits below zero) and the origin
	// gain is well above one, so a quiet signal was all "residue" — MEASURED, ±0.8 out of a silent
	// input. The second fed back `f(u) - g0 u`, which is zero near zero but runs to `-g0 u` in
	// saturation, a positive loop gain of `0.9 g0` — MEASURED non-finite at Knee 1, Drive 1. This one
	// is zero near zero (silence stays silence), and since |f| <= 1 and `g0` is floored at one, the
	// residue is within 1 of `u`, so with a loop gain under one `u` is bounded for EVERY curve,
	// folders included: |u| <= (|a| + L) / (1 - L).
	//
	// **AND ITS GAIN IS THE ENVELOPE'S, like Bloom's and Sag's — THE THIRD FAILURE.** Bounded is not
	// contracting: on a folder `f'` is steep and changes sign, `1 - f'/g0` leaves (-1/L, 1/L), and the loop
	// found a limit cycle of its own — MEASURED 0.35 still coming out half a second after the input
	// stopped. No residue built from the curve can promise contraction on every curve, so the loop is
	// given the one thing that does promise it: it feeds on itself only while you play, and the
	// envelope — of the INPUT, which a self-sustaining loop cannot feed — takes it to zero.
	const float curveIn = (x + bias) / rail + loop_ * kLoopMaxGain * envelope * state.loopResidue;
	// **CLAMPED AT THE CURVE'S OWN CEILING, which is what the drawn transfer (`staticTransferAt`) already
	// says it is.** Below Knee's full fade part of the dry input rides through the blend, so an input
	// past full scale comes out past it — and the loop's bound above rests on |f| <= 1.
	const float curveOut = std::clamp(stage(curveIn, state), -1.0f, 1.0f);
	float y = curveOut;
	// **EVERYTHING BELOW IS MEASURED FROM THE CURVE'S OWN VALUE AT ZERO, `±silence_`, NOT FROM ZERO.**
	// Written when the Mix-1 curve STEPPED to ±f(0) at every zero crossing (0.046 at the defaults, 0.77
	// at Knee 1 / Drive 1): done about zero, `rail * stage(u)` shrank that step with the envelope, so
	// on silence it DRIFTED as the rails recovered and the blocker passed the drift — MEASURED, 0.8 s
	// above -60 dB after the input stopped. Backlog IF took the step out (the read now starts at the
	// threshold floored at zero), so `silence_` reads 0 and these anchors are inert — KEPT as the guard
	// that makes the stages right for any curve that does not pass through zero, at two reads a block.
	const float anchor = (curveIn < 0.0f) ? -silence_ : silence_;
	if (bloom_ != 0.0f || sag_ != 0.0f) {
		// BLOOM'S OFFSET COMES OUT THROUGH THE CURVE: the extra it bought at silence, `f(bias) - silence_`,
		// is subtracted rather than `bias` itself, so silence reads `silence_` at every envelope — and
		// the expression is continuous in the envelope, so nothing steps when it underflows to zero.
		// The division keeps the far rail in range: the offset moves the near rail in by as much as it
		// would push the other out. SAG'S `rail` then scales what lies beyond the side's anchor — the
		// curve on smaller rails, with the step left where it was.
		const float lift = staticStage(bias / rail) - silence_;
		y = anchor + rail * (curveOut - anchor - lift) / (1.0f + std::fabs(lift));
	}
	state.loopResidue += ((curveIn - (curveOut - anchor) / loopSlope_) - state.loopResidue) * loopAlpha_;
	preBlock_ = y;
	// **THE CROSSFADE, LAST (backlog FA)** — the filtered input against everything the device did to it. At 100%
	// it returns the wet untouched; below it the dry share is NOT clamped — see `dryWetMix`.
	return dryWetMix(dry, toOutput(y, state), dryWet_);
}

float Shaper::inputFilters(float x, ChannelState& state) const {
	// THE HIGHPASS AS THE INPUT MINUS ITS OWN LOW PART. Off, the low part is held at ZERO, so switching it in
	// (a modulated corner leaving 20 Hz) starts from "nothing subtracted" — the output does not step.
	if (inputHpfHz_ > 0.0f) {
		state.inputHpfLow += (x - state.inputHpfLow) * inputHpfAlpha_;
		// FLUSHED AT -600 dBFS, `blockDc`'s reason: an IIR on silence otherwise parks on a denormal for ever.
		if (std::fabs(state.inputHpfLow) < 1.0e-30f) {
			state.inputHpfLow = 0.0f;
		}
		x -= state.inputHpfLow;
	}
	else {
		state.inputHpfLow = 0.0f;
	}
	// THE LOWPASS. Off, its state FOLLOWS the input, so switching it in starts from where the signal is — the
	// same no-step reason, from the other side.
	if (inputLpfHz_ > 0.0f) {
		state.inputLpf += (x - state.inputLpf) * inputLpfAlpha_;
		if (std::fabs(state.inputLpf) < 1.0e-30f) {
			state.inputLpf = 0.0f;
		}
		x = state.inputLpf;
	}
	else {
		state.inputLpf = x;
	}
	return x;
}

float Shaper::staticStage(float x) const {
	// THE STAGE WITH NO MEMORY: the threshold split, the knee fade and the read, and none of the
	// extras — they are state, and this is asked what the curve does to one value on its own.
	const float absX = std::fabs(x);
	const float diff = absX - threshold_;
	if (diff <= 0.0f) {
		return x;
	}
	const float sign = (x < 0.0f) ? -1.0f : 1.0f;
	const float blend = kneeFade_ * std::min(diff * blendSlope_, 1.0f);
	// THE READ STARTS AT `readFrom` AND CARRIES THE SIGN — the first draft passed `shape(diff)`, which
	// is the + side's answer for BOTH sides, so `staticStage(-p)` (Loop's slope probe) read the wrong half.
	const float readFrom = std::fmax(threshold_, 0.0f);
	const float shaped = sign * readFrom + shape(sign * (absX - readFrom));
	return x * (1.0f - blend) + shaped * blend;
}

float Shaper::stage(float x, ChannelState& state) {
	// **THE THRESHOLD IS COMPARED AGAINST THE INPUT ITSELF**, in input units, and so is everything
	// from here to the read. The old code scaled the signal by the drive gain and a 0.25 pad first
	// and compared THAT, which is how a threshold above 0 dBFS came to exist; see the note above
	// `kSweepOctaves` for what the read is now and why.
	const float absX = std::fabs(x);
	const float sign = (x < 0.0f) ? -1.0f : 1.0f;

	float wet = x;

	// THE STRIDE STEP — one shared counter serving the comb, the zero-crossing detector and the
	// integrator's reset, because all three are the firmware's "every N samples" and a second counter
	// would let them drift apart.
	float feedbackSample = 0.0f;
	if (subActive_ || feedbackActive_ || integratorActive_) {
		if (++state.strideCounter >= stride_) {
			state.strideCounter = 0;
			const float previous = state.prevInput;
			if (feedbackActive_) {
				// AN IMPULSE EVERY `stride` SAMPLES, NOT A HELD ECHO. The firmware clears this local
				// each sample and only sets it inside the stride branch, so what the comb injects is a
				// train at fs/stride rather than a continuous delayed copy — that train is the buzz the
				// effect is for. Held instead, it would be a plain and much duller delay.
				feedbackSample = previous * feedbackAmount_;
			}
			state.prevInput = x;
			if ((x < 0.0f) != (previous < 0.0f)) {
				if (subActive_) {
					if (++state.zcCount >= subRatio_) {
						state.subSign = -state.subSign;
						state.zcCount = 0;
					}
				}
				if (integratorActive_) {
					// THE RESET IS THE EFFECT. An integrator that never reset would run off to a DC
					// rail; resetting at zero crossings is what turns it into a per-cycle ramp, which
					// is why a sine comes out triangle-ish.
					state.integrator = 0.0f;
				}
			}
		}
	}

	wet += feedbackSample;

	if (rotation_ != 0) {
		// AFTER THE COMB, so the rotation mangles the comb-filtered signal rather than being combed
		// itself. The firmware's ordering, and it is audible: the other way round the aliasing would be
		// echoed at the stride rate instead of applied to the echo.
		wet = rotateRight(wet, rotation_);
	}

	// THE LOWPASS FIRST, FED FROM THE DRY SIGNAL. It is a PRE-transform whose job is to hand the
	// curve a signal with its top end removed; feeding it the comb's or the rotator's output would
	// make it a filter on those effects instead. The firmware reads `scaledDry` here for the same
	// reason.
	if (lowpassActive_) {
		state.lowpass += (x - state.lowpass) * lowpassAlpha_;
		wet = state.lowpass;
	}
	// THEN THE INTEGRATOR, on whatever reached it. Its own accumulator, so the two stages cannot
	// tread on each other — see beginBlock for why that is a divergence worth making.
	if (integratorActive_) {
		// BOUNDED AT FULL SCALE, the firmware's `add_saturate` on a q31 accumulator (Shaper parity sweep,
		// 2026-09-16): a signal that never crosses zero — a DC offset, a one-sided waveform — never
		// resets this, and an unbounded float grew without limit, finite at the output only because the
		// pre-squash happened to follow. The reference's ceiling is the contract.
		state.integrator = std::clamp(state.integrator + wet * (1.0f / 64.0f), -1.0f, 1.0f);
		wet += (state.integrator - wet) * integratorBlend_;
	}

	if (subBoost_ != 0.0f) {
		wet -= state.subSign * wet * subBoost_;
	}

	// THE AMPLITUDE BLEND — measured against the DRY signal, always.
	//
	// AGAINST DRY AND NOT WET, WHICH IS LOAD-BEARING. The wet path has been through the comb, the
	// rotator and the sub, any of which can change its level; deciding how much of the effect to hear
	// from the effect's own output would make the threshold depend on the extras and would put the
	// rotator, which can swing a quiet sample to full scale, in charge of its own mix.
	const float diff = absX - threshold_;
	if (diff <= 0.0f) {
		state.prevAbs = absX;
		// THROUGH THE BLOCKER EVEN WHEN THE SAMPLE IS BELOW THE THRESHOLD. Returning the dry sample
		// raw would leave the blocker's history un-advanced, so the first sample to cross back would
		// see a stale difference and get a step of its own — a fix that introduced the click it was
		// added to remove, on exactly the signals that sit near the threshold.
		//
		// `x`, THE INPUT. This is the line that once made Drive a volume knob below the threshold,
		// and the two parity cases at the end of `test_fx_shaper.cpp` are what keep it `x`.
		// `preBlock_` IS SET ONCE, IN `processSample`, after the memory stages have had their say — so
		// it cannot report a road out that one of them later changed.
		return x;
	}
	// **SCALED BY THE KNEE FADE**, so knee -> 0 approaches the linear pass instead of stepping to it.
	// See `kneeFadeFor` for the 0.91 dB this closes and for why knee itself could not close it.
	const float blend = kneeFade_ * std::min(diff * blendSlope_, 1.0f);

	// THE WET'S EXCESS, IN THE INPUT'S DIRECTION. With the extras off this is `diff` with `x`'s
	// sign, and the table's matching half is read — the asymmetry between the halves is the even
	// harmonics, and reading one half for both signs would normalise it out. With an extra on, the
	// wet has been moved and its excess is wherever the extra put it: possibly negative, which reads
	// the other half, possibly far past the table's end, which `lookup` pins. Every one of those
	// reads is inside the ceiling — `|a·T| <= h` says nothing about WHERE the table was read — and
	// the `|out| <= |in|` bound is a claim about the dry read only, which is the one the parity and
	// ceiling cases make.
	// FROM `readFrom`, NOT FROM `threshold_` — the blend above keeps the undershoot, the read does not
	// (backlog IF; see `Staging::readFrom`).
	const float readFrom = std::fmax(threshold_, 0.0f);
	float excess = sign * wet - readFrom;
	if (hystOffset_ != 0.0f) {
		// DIRECTION-DEPENDENT, AND ONE-SIDED. The table is read where the dry excess says on the edge
		// the signal is getting louder, and a little BACK on the edge it is getting quieter — so the
		// curve a sample sees depends on which way the waveform is going, which is what a hysteresis
		// loop IS. One-sided rather than the firmware's ±offset because an edge that read AHEAD of
		// the dry excess would read the table past the unit-slope bound and put the shaped top above
		// the dry by `a·m·δ`; referencing the loop to the bound keeps its width and loses nothing
		// else. A negative field reverses which edge lags, as the ± did.
		const bool quieter = absX < state.prevAbs;
		if (quieter == (hystOffset_ > 0.0f)) {
			excess = std::fmax(0.0f, excess - std::fabs(hystOffset_));
		}
	}
	state.prevAbs = absX;

	// **THE TABLE'S OUTPUT IS THE DEVICE'S OUTPUT. NO POST GAIN.** By design — see the note at
	// `kSweepOctaves` for the read, and for why the ceiling and the dry bound hold by construction.
	//
	// **AND THE DRY SIDE OF THE CROSSFADE IS THE SAME `x` THE BRANCH ABOVE RETURNS, WHICH IS NOT A
	// DETAIL.** Whatever the sub-threshold branch returns, this line must return at `blend == 0` or
	// a step opens exactly at the threshold, on every sample that passes through it. "THE THRESHOLD
	// IS CONTINUOUS" in `test_fx_shaper.cpp` pins it — and the shaped side now starts at exactly the
	// same value, `sign·t + 0`, so the crossfade is between two things that agree at the crossing.
	const float shaped = sign * readFrom + shape(sign * excess);
	// **THE MIXED SAMPLE BEFORE THE DC BLOCKER, KEPT FOR THE CERTIFICATE.** `staticTransferAt` is a
	// STATIC transfer and `blockDc` is a signal-dependent filter on top of it, so the two can only be
	// compared here. Academic while the read reached a near-symmetric sliver of the table; decisive now
	// that the folds are reachable, because a folded transfer is ASYMMETRIC and generates real DC —
	// comparing after the blocker would report the blocker rather than the transfer.
	const float mixed = x * (1.0f - blend) + shaped * blend;
	return mixed;
}


void Shaper::meterInput(float x) {
	// **HOW HARD THE TABLE IS BEING DRIVEN, IN THE TRANSFER'S OWN COORDINATE.**
	//
	// lets get a level meter in the background of the transfer curve display which is
	// post-mix transform so we can see how hard we drive the actual table. The plot's x axis IS the
	// input axis, so the honest meter is the peak INPUT — it says which part of the drawn curve the
	// signal is actually exercising, and because the curve already carries the threshold, the span
	// from the threshold to this peak is the excess reaching the table. That is the post-mix reading
	// asked for, drawn where it can be compared with what the table does to it.
	//
	// **THE PEAK, NOT THE RMS**, because the question is how far ALONG the curve the signal reaches,
	// and the far end of the curve is where the folding is. An RMS meter would answer a loudness
	// question and hide exactly the excursions the shaper acts on.
	//
	// A PLAIN FLOAT, LIKE OWLTT'S METERS. The reader is a repaint on the UI thread and the writer is
	// the audio thread; a torn float here is one frame of a meter, which is a cosmetic risk of the
	// same size as the frame it would be drawn in. `device_host.h` records the same trade for
	// playheads.
	const float level = std::fabs(x);
	if (level > drivePeak_) {
		drivePeak_ = level;
		return;
	}
	// **AND IT FALLS, or the first transient paints a permanent mark.** A meter that only ever rises
	// is a high-water line, which is a different instrument and a worse one for "am I driving it hard
	// RIGHT NOW" — the question being asked. ~200 ms to a tenth at 48 kHz, slow enough to read and
	// fast enough to follow a phrase.
	drivePeak_ *= driveDecay_;
}
void Shaper::processStereo(float* left, float* right, int frames) {
	const float* trimRun = pendingTrimRun_; // this call's window, taken before any return
	pendingTrimRun_ = nullptr;
	// A VOICE NOT YET HANDED ITS BUS'S TABLE (backlog OM) passes its input: the host hands it before every block it runs.
	if (left == nullptr || right == nullptr || frames <= 0 || bank_ == nullptr) {
		return;
	}
	if (!target_.enabled) {
		// AND THE CHANNEL STATE IS CLEARED ON THE WAY OUT (backlog EH, shape 6) — the blocker's history
		// and the threshold memory would otherwise resume from wherever the last enabled sample left
		// them, a bypass later. The curve stays built; only the running state goes.
		if (running_) {
			running_ = false;
			reset();
		}
		return;
	}
	running_ = true;
	beginBlock(frames);
	for (int i = 0; i < frames; ++i) {
		// ONCE PER FRAME, BEFORE EITHER CHANNEL — so L and R of the same frame are driven by the same
		// gain and read the same table, which they were not while this lived in `processSample`.
		advanceFrame();
		if (trimRun != nullptr) {
			trimGain_ = std::pow(10.0f, trimRun[i] / 20.0f); // `beginBlock`'s law, per frame while Trim moves
		}
		// THE METER IS INSIDE `processSample`, after the trim — see the note there.
		left[i] = processSample(left[i], 0);
		right[i] = processSample(right[i], 1);
	}
}

void Shaper::processMono(float* mono, int frames) {
	const float* trimRun = pendingTrimRun_;
	pendingTrimRun_ = nullptr;
	if (mono == nullptr || frames <= 0 || bank_ == nullptr) {
		return;
	}
	if (!target_.enabled) {
		// AND THE CHANNEL STATE IS CLEARED ON THE WAY OUT (backlog EH, shape 6) — the blocker's history
		// and the threshold memory would otherwise resume from wherever the last enabled sample left
		// them, a bypass later. The curve stays built; only the running state goes.
		if (running_) {
			running_ = false;
			reset();
		}
		return;
	}
	running_ = true;
	beginBlock(frames);
	for (int i = 0; i < frames; ++i) {
		advanceFrame();
		if (trimRun != nullptr) {
			trimGain_ = std::pow(10.0f, trimRun[i] / 20.0f);
		}
		mono[i] = processSample(mono[i], 0);
	}
}

/// THE CONTROLS READ PER SAMPLE WHEN MODULATED — see `AudioOps::windowedControls`.
constexpr std::array<uint8_t, 1> kWindowedControls { static_cast<uint8_t>(Param::Trim) };

/// HOW THE HOST RUNS THIS DEVICE'S AUDIO.
///
/// `setSlot` IS NULL AND THAT IS THE DECLARATION. The shaper publishes no meters and keeps no
/// per-slot state, so there is nothing for a slot number to key — and `AudioOps`' own rule is that
/// null IS the capability. OWLTT takes one and its own header records that nothing consumes the
/// number; this device does not take one at all.
const device::AudioOps kAudioOps {
	.stateBytes = sizeof(Shaper),
	.stateAlign = alignof(Shaper),
	.construct = [](void* storage) { new (storage) Shaper(); },
	.destruct = [](void* storage) { static_cast<Shaper*>(storage)->~Shaper(); },
	.prepare = [](void* storage, double sampleRate, int) {
		static_cast<Shaper*>(storage)->prepare(sampleRate);
	},
	.reset = [](void* storage) { static_cast<Shaper*>(storage)->reset(); },
	.setSlot = nullptr,
	.setFields =
	    [](void* storage, const float* fields, size_t fieldsPresent) {
		    Row row {};
		    const size_t take = std::min(fieldsPresent, row.size());
		    for (size_t i = 0; i < take; ++i) {
			    row[i] = fields[i];
		    }
		    // THE COUNT GOES THROUGH, not just the values. `stateOf` puts absent fields at their
		    // DECLARED defaults, and a shaper decoded with absent fields at zero would come back with
		    // Mix at zero — nothing shaped — and Knee at zero — a bypass — rather than at 1.0 and 0.5.
		    // **AND THE FOLDED TRAVEL, WHICH LIVES PAST THE PARAMS AND SO PAST `Row`.** `stateOf` takes
		    // the params-only row by design; the blob is the wide field array's business and this is the
		    // only place that sees both. A row from before the pad existed simply has none, and zero
		    // travel is the honest reading of that: the field where colour and gamma left it.
		    Settings s = stateOf(row, take);
		    for (size_t i = 0; i < phase_space::kPhaseCount; ++i) {
			    const size_t at = kTravelFirst + i;
			    s.travel[i] = at < fieldsPresent ? fields[at] : 0.0f;
		    }
		    static_cast<Shaper*>(storage)->setTarget(s);
	    },
	.setNoteContext =
	    [](void* storage, const device::NoteContext& note) {
		    static_cast<Shaper*>(storage)->setNoteContext(note);
	    },
	// THE DRIVE METER — it was asked to see how hard the table is actually driven. `Shaper::meterInput`
	// is the writer; `drawShaperCurve` colours the reached part of the curve from it.
	.drivePeak = [](const void* storage) { return static_cast<const Shaper*>(storage)->drivePeak(); },
	.processStereo = [](void* storage, float* left, float* right, int frames) {
		static_cast<Shaper*>(storage)->processStereo(left, right, frames);
	},
	.processMono = [](void* storage, float* mono, int frames) {
		static_cast<Shaper*>(storage)->processMono(mono, frames);
	},
	// TRIM IS READ PER FRAME WHILE IT MOVES (backlog FK); the table's controls are a build, and stay on the tick.
	.windowedControls = kWindowedControls,
	.setFieldWindows =
	    [](void* storage, const float* const* runs, size_t count, int) {
		    const auto trim = static_cast<size_t>(Param::Trim);
		    static_cast<Shaper*>(storage)->setTrimRun(trim < count ? runs[trim] : nullptr);
	    },
	// **ONE TABLE FOR EVERY VOICE (backlog OM)**: a voice frees its own bank, reads its bus instance's, and the bus
	// keeps that bank built while its voices run.
	.pullHandBlocks = [](void* storage, const void* source) {
		    static_cast<Shaper*>(storage)->shareFrom(*static_cast<const Shaper*>(source));
	    },
	.setVoice = [](void* storage, size_t) { static_cast<Shaper*>(storage)->becomeVoice(); },
	.beginSharedBlock =
	    [](void* storage, const float* fields, size_t fieldsPresent, int frames) {
		    kAudioOps.setFields(storage, fields, fieldsPresent);
		    static_cast<Shaper*>(storage)->beginSharedBlock(frames);
	    },
};

} // namespace feathers::fx::shaper
