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

#include "device/registry.h"
#include "fx/fx_tenant.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

//--------------------------------------------------------------------------------------------------//
// THE DELAY, AND THE GRANULATOR THAT LIVES INSIDE ITS LOOP.                                          //
//--------------------------------------------------------------------------------------------------//
//
// **THE TOPOLOGY IS PORTAL'S, TURNED THE RIGHT WAY ROUND FOR A DEVICE THAT IS A DELAY FIRST.**
// Output's grain delay is a delay wrapped AROUND a granulator — its manual: TIME *"add delay to the
// audio buffer which is then fed back into the input of the granulator"*, with FEEDBACK setting how
// much returns and HP/LP filtering what returns. So every trip round the loop is granularised again
// and the tail disintegrates.
//
// Portal is a granulator, so it grains the input too. This is a DELAY, so it does not: the dry path
// is clean, the granulator sits on the WET path, and the feedback is taken AFTER it. That shape is a
// deliberate decision (2026-09-20). What falls out of it is the thing worth having and it costs no code:
//
//     tap 1 is granulated once, tap 2 twice, tap 3 three times — the tail decays into dust by
//     construction, with no per-tap special case anywhere.
//
//         dry ──────────────────────────────────────> out
//                                                      ^
//         in ──> [ring] ──> [GRAIN] ──┬────────────────┘
//                   ^                 │
//                   └──── x FB <── [TONE]
//
// **WHAT IS DELIBERATELY NOT HERE (the Portal-parity backlog).** COUNT, OFFSET and its ratio-lock to
// DENSITY, per-grain PAN, SCALE-quantised PITCH, REVERSE, the STRETCH/SPEED/RETRIGGER section, HP and
// LP as a separate pair rather than one tilt, and a per-knob jitter amount (Portal's "DIAMOND").
//
// **AND THE REASON THE DIAMONDS ARE NOT URGENT, measured rather than assumed.** A device's controls
// are refreshed once per `device::kTickWindow` (64 samples) — `RackEngine::processStereo` cuts every
// block on it. A grain at 20/s is 2400 samples apart and at 200/s still 240, so ANY density below
// roughly 750 grains a second already reads a different value per grain from an ordinary routed
// modulator. Pointing a Random LFO at Grain Size IS Portal's diamond today. A built-in jitter saves a
// modulator slot; it does not unlock a behaviour., asked the better question behind
// it — whether per-grain MOD OUTPUTS should exist, so a grain can drive things elsewhere — and that
// one is genuinely new and filed rather than guessed at here.

namespace feathers::fx::delay {

//--------------------------------------------------------------------------------------------------//
// THE SYNC LIST.                                                                                     //
//--------------------------------------------------------------------------------------------------//

/// **THE FREE TIMES THE DROPDOWN CARRIES, AND THEY ARE IN THE LIST RATHER THAN IN A SECOND MODE OF
/// THE TIME KNOB.** in bitwig, you get non-synced time constants in the sync dropdown, eg 1
/// hz, which are naturally scaled — said of `automod`, and chosen again for this device on
/// 2026-09-20 over the LFO's older shape.
///
/// **THE DIFFERENCE FROM `automod` IS THE DIMENSION, NOT THE DESIGN.** There the free entries are
/// RATES in hertz, because the thing being selected is how fast a shape runs. Here they are
/// DURATIONS, because the thing being selected is how far back the tap reads. Same construction,
/// same one-meaning-everywhere rule for the knob beside it.
///
/// **THERE IS NO `Off`, AND ITS ABSENCE IS FORCED RATHER THAN CHOSEN** — the same argument
/// `automod_types.h` makes. `Off` would have meant "read the Time knob as a duration", which is the
/// second meaning this scheme exists to remove. With Time always a multiplier there is no base for
/// `Off` to select.
///
/// **THREE, AND THE BOTTOM ONE IS 10 ms BECAUSE THIS DEVICE DOES NOT GO INTO COMB RANGE.** Decided
/// 2026-09-20: sub-10 ms is flanger/comb territory, `automod` already owns it (`automod_comb.h`), and
/// a delay that reaches down there is a delay competing with the device next to it. `10 ms x0.25` is
/// 2.5 ms and reachable if a hand insists; nothing here is built for it.
inline constexpr double kFreeTimeSeconds[] = { 0.010, 0.100, 1.000 };
inline constexpr const char* kFreeTimeNames[] = { "10 ms", "100 ms", "1 s" };
inline constexpr size_t kFreeTimeCount = std::size(kFreeTimeSeconds);
static_assert(std::size(kFreeTimeNames) == kFreeTimeCount);

/// THE FREE TIMES FIRST, THEN EVERY MUSICAL DIVISION, THEN `Note` (delay-love S6b, from Dedalus's Pitch mode: the tap is
/// the period of the held note, so with Feedback high the line is a tuned comb that plays the keyboard).
inline constexpr size_t kSyncNoteChoice = kFreeTimeCount + device::kDivisionCount;
inline constexpr size_t kSyncChoiceCount = kSyncNoteChoice + 1;

/// A `constexpr` FUNCTION RATHER THAN A LITERAL LIST, for `automod`'s reason: thirty hand-written
/// strings is a list nobody can check against the table it shadows.
constexpr std::array<const char*, kSyncChoiceCount> makeSyncNames() {
	std::array<const char*, kSyncChoiceCount> out {};
	for (size_t i = 0; i < kFreeTimeCount; ++i) {
		out[i] = kFreeTimeNames[i];
	}
	for (size_t i = 0; i < device::kDivisionCount; ++i) {
		out[kFreeTimeCount + i] = device::kDivisionNames[i];
	}
	out[kSyncNoteChoice] = "Note";
	return out;
}
inline constexpr auto kSyncNames = makeSyncNames();

/// WHICH KIND OF ENTRY A CHOICE IS. One place converts, so a reader never does the offset arithmetic
/// by hand — that offset is exactly the trap this hides, and `automod` hides the same one.
constexpr bool isFreeTime(int choice) {
	return choice >= 0 && static_cast<size_t>(choice) < kFreeTimeCount;
}
constexpr bool isSyncedTime(int choice) {
	return static_cast<size_t>(choice) >= kFreeTimeCount
	       && static_cast<size_t>(choice) < kSyncNoteChoice;
}
/// THE HELD NOTE'S PERIOD, times Time — appended, so every older choice keeps its index and its label.
constexpr bool isNoteTime(int choice) {
	return choice >= 0 && static_cast<size_t>(choice) == kSyncNoteChoice;
}
constexpr double freeTimeSecondsFor(int choice) {
	return isFreeTime(choice) ? kFreeTimeSeconds[choice] : kFreeTimeSeconds[0];
}
constexpr double syncDivisionBeats(int choice) {
	return isSyncedTime(choice)
	           ? device::kDivisionBeats[static_cast<size_t>(choice) - kFreeTimeCount]
	           : 0.0;
}

/// **THE DEFAULT IS AN EIGHTH, PINNED BY ITS MEANING RATHER THAN BY ITS INDEX.** `device::
/// kDefaultDivision` is one bar, which is the length a hand assumes when it DRAWS A SHAPE and says
/// nothing — it is not what a hand assumes when it drops a delay. The assert below is what makes this
/// safe to state as a number: reorder `kDivisionNames` and this fails rather than silently defaulting
/// the device to a dotted 1/16, which is the failure `kDefaultDivision`'s own assert exists to
/// prevent and which this table inherits by being built from it.
/// **THE NOTE A Note-synced Delay PLAYS BEFORE ANY IS HELD**: middle C, 261.6 Hz, a 3.8 ms tap. Once a note has been held,
/// its pitch is kept while none is (to confirm), so letting go does not jump the tap back here.
inline constexpr float kDefaultNotePitch = 60.0f;
[[nodiscard]] inline double notePeriodSeconds(float pitchSemitones) {
	return 1.0 / (440.0 * std::exp2((static_cast<double>(pitchSemitones) - 69.0) / 12.0));
}

inline constexpr size_t kDefaultSync = kFreeTimeCount + 9;
static_assert(kDefaultSync < kSyncChoiceCount);
static_assert(std::string_view(kSyncNames[kDefaultSync]) == "1/8");

//--------------------------------------------------------------------------------------------------//
// THE RANGES.                                                                                        //
//--------------------------------------------------------------------------------------------------//

/// **TIME IS A MULTIPLIER AND THE UNIT FIXES ITS CURVE.** `Unit::LogMultiple` is `exp2(value)`
/// exactly — device.h is explicit that a control with any other multiplier function may not use it —
/// so the range is stated in octaves and the taper is not a free choice.
inline constexpr double kTimeLog2Min = -2.0; // x0.25
inline constexpr double kTimeLog2Max = 2.0;  // x4
static_assert(kTimeLog2Min < kTimeLog2Max);

/// **HOW LONG THE LINE IS, AND THE ONE CLAMP THAT COVERS EVERY WAY OF EXCEEDING IT.**
///
/// Sixteen seconds, which is `fx::sklitter::kLooperSeconds` — not copied for symmetry but because it
/// is the same trade already made once in this repo, and matching it means the largest buffer here is
/// not a new high-water mark. Stereo at 48 kHz that is ~6.1 MB.
///
/// **IT IS REACHABLE, so the clamp is a real behaviour rather than a guard against the impossible.**
/// `4 bar.` is 24 beats; at 60 BPM that is 24 seconds before the multiplier, and `x4` on almost
/// anything overruns. At 120 BPM — where a hand actually is — `4 bar` is 8 s and `4 bar.` is 12 s, so
/// nothing in ordinary use touches this. A request past the end is CLAMPED and the delay is shorter
/// than the dropdown says; the alternative (refusing, or wrapping) is worse, and the alternative of
/// sizing for the worst case costs 9 MB for a combination almost nobody asks for.
inline constexpr double kMaxDelaySeconds = 16.0;

/// **THE SHORTEST TAP, AND IT IS NOT ZERO FOR A REASON THE GRANULATOR NEEDS.** A read head at the
/// write head reads the sample being written this instant, which with feedback is a zero-delay loop.
/// One millisecond is comfortably outside that and still shorter than anything this device is for.
inline constexpr double kMinDelaySeconds = 0.001;

/// **FEEDBACK GOES PAST UNITY ON PURPOSE.** Below 1 the tail decays and reaches exact silence (see
/// `kSilenceFloor`); at and above 1 it sustains, which is a delay's oldest instrument and not a bug
/// to be clamped away. The soft clip in the loop is what keeps that bounded rather than infinite.
///
/// **TO 200% SINCE delay-love S6b** (from Dedalus, decided together with the loop limiter): above `kFeedbackLimitAbove` — 110%, the
/// old ceiling — the loop limiter holds the returns at full scale, so a runaway is a sustained swell, bounded, rather
/// than `softClip`'s grind. At or below it nothing changed, to the bit. Same meaning per number, so the same key.
inline constexpr double kFeedbackMax = 2.0;
inline constexpr double kFeedbackLimitAbove = 1.1;

/// **WHERE THE TAIL IS DECLARED OVER.** `test_fx_devices.cpp` requires that every audio device's tail
/// reaches EXACT silence or is named as one that does not, and a geometric decay reaches
/// denormal-small but never zero. Rather than take the exemption, the line is flushed once it has been
/// below this for a whole buffer length — at which point every sample in it is below this, so nothing
/// audible is discarded. -140 dBFS.
inline constexpr float kSilenceFloor = 1.0e-7f;

//--------------------------------------------------------------------------------------------------//
// THE DUCK, THE FREEZE, THE CLEAR (delay-love stage S4).                                             //
//--------------------------------------------------------------------------------------------------//
//
// **DUCK IS ONE BIPOLAR CONTROL** (by design, from Cluster Delay's ducker and Dedalus's Duck/Gate). An envelope follower
// on the DRY INPUT sets the HEARD wet's gain: positive ducks (the repeats get out of the way while you play and bloom
// in the gaps), negative gates (the repeats sound only while you play). It acts on what is HEARD, never on the line,
// so the repeats are still there when it lets go. At 0 the follower runs but nothing reads it: the device is what it
// was, to the bit.
//
// **FREEZE IS A TRUE HOLD** (by design). On, the input stops entering (over `kFreezeRampSeconds`) and the line recirculates
// its last loop at unity — a raw read at the tap the head stood at when it froze, latched to the frame — bypassing
// Tone, the lane, the limiter and the clip, so it holds for ever, exactly. The heard heads keep reading as they do, so
// Granular sprays over a held loop and Multi-Tap taps one. **WHAT PERSISTS IS THE SWITCH, NOT THE AUDIO** (to
// confirm): CLAUDE.md lists a delay freeze among the captured states that persist, and also says audio buffers do not
// unless it is added later — so a frozen patch reloads frozen, over an EMPTY loop.
//
// **CLEAR IS A RISING EDGE**: the heard wet fades out over `kClearFadeSeconds`, then the line, the grains and the
// device's own loop state are emptied at the next block's start. A lane device's own tail (a reverb in FEEDBACK FX)
// is the lane's and still rings back in through Feedback — stated, not hidden. Its field persists as whatever it was;
// a saved high reloads high, which is no edge, so a load never clears.

/// THE DUCK'S FOLLOWER: its fixed attack, and where it engages on a dB scale — none at -48 dBFS, fully at -12.
inline constexpr double kDuckAttackSeconds = 0.005;
inline constexpr float kDuckFloorDb = -48.0f;
inline constexpr float kDuckFullDb = -12.0f;
inline constexpr double kDuckReleaseLog2Min = -5.64385619; // 20 ms
inline constexpr double kDuckReleaseLog2Max = 1.0;         // 2 s
inline constexpr double kDuckReleaseLog2Default = -2.0;    // 250 ms
/// HOW LONG THE FREEZE TAKES TO CLOSE THE INPUT AND HAND THE LOOP TO THE HOLD, and the clear's fade.
inline constexpr double kFreezeRampSeconds = 0.010;
inline constexpr double kClearFadeSeconds = 0.005;

//--------------------------------------------------------------------------------------------------//
// THE BUCKET BRIGADE (BBD mode) AND THE SWARM (on Granular) — delay-love, 2026-11-05.                  //
//--------------------------------------------------------------------------------------------------//
//
// **A BBD IS A FIXED NUMBER OF STAGES CLOCKED AT WHATEVER RATE THE DELAY NEEDS**: `D = N / (2·f_clk)`. A longer Time
// is a SLOWER clock, so the chip samples coarser, and the filters around it — the anti-alias in front, the
// reconstruction behind — have to follow it down, or the clock's images come through. That is the whole character and
// it is what separates this from Tape: Tape wobbles and saturates at any length; a BBD loses its top as it gets longer.
//
//   f_clk = N / (2·D)              the clock, Hz (D in seconds)
//   f_c   = min(kBbdFilterFraction · f_clk / 2, kBbdFilterCeiling · sr)    both filters' corner
//
// The signal path, one pass: input + feedback → saturation (Drive) → compressor (Compander) → anti-alias low-pass →
// the line, sampled and held at f_clk → reconstruction low-pass → expander → heard; and the loop is fed from what is
// heard, so every pass goes through the chip again and darkens again. A Time that moves re-clocks the chip — the
// head GLIDES, as Clean's does — so the pitch bends, which is the sound of a BBD's time knob.
//
// **THE NOISE RIDES ON THE HEARD SIGNAL, NOT IN THE LINE** (to confirm): hiss and the clock's whine are scaled by
// the expander's own envelope, so they breathe with the signal and stop with it. In the line they would recirculate
// for ever and no tail could reach exact zero. The whine is a sine at the clock itself, so it is under the
// reconstruction filter's corner only when the clock is slow — audible on a long Time, as on a real chip.
//
// **SWARM** (swarm as a setting on grain): at 0 the granulator is exactly what it was. Up, each grain
// shrinks, more of them spawn, and each is thrown in time, panned and detuned by a draw that is a HASH of the seed and
// a grain index, with nothing to store (the persistence rule's "derive it" branch). **ON A PLAYING TRANSPORT THE INDEX
// IS THE SONG'S** (2026-11-05): grain k is born where floor(song frame / spawn period) steps to k, so the same position is
// the same cloud to the bit, whatever the block size or when playback started (`Delay::processStereo`) — Granular's own spray too, at Swarm 0. Stopped,
// the index is the serial since reset, as before.
inline constexpr const char* kBbdStageNames[] = { "1024", "2048", "4096", "8192" };
inline constexpr int kBbdStageCounts[] = { 1024, 2048, 4096, 8192 };
inline constexpr size_t kBbdDefaultStages = 2; // 4096 — the MN3005, the classic long chip
inline constexpr double kBbdFilterFraction = 0.8; // of the chip's Nyquist, f_clk / 2
inline constexpr double kBbdFilterCeiling = 0.45;  // of the host's rate: a short Time is never darker than this
inline constexpr double kBbdMaxDrive = 4.0;
inline constexpr double kBbdCompanderAttackSeconds = 0.003;
inline constexpr double kBbdCompanderReleaseSeconds = 0.060;
inline constexpr float kBbdCompanderRef = 0.25f; // -12 dBFS: where the compander leaves the level alone
inline constexpr float kBbdHissLevel = 0.02f;
inline constexpr float kBbdWhineLevel = 0.01f;
/// THE CORNER BOTH BBD FILTERS SIT AT FOR A DELAY OF `delaySeconds` THROUGH `stages`, Hz — the formula above.
[[nodiscard]] inline double bbdCutoffHz(int stages, double delaySeconds, double sampleRate) {
	const double clock = static_cast<double>(stages) / (2.0 * std::max(1.0e-6, delaySeconds));
	return std::min(kBbdFilterFraction * clock * 0.5, kBbdFilterCeiling * sampleRate);
}
inline constexpr double kSwarmSpreadSeconds = 0.040;    // a grain thrown up to ±40 ms about its place, at full Swarm
inline constexpr double kSwarmPitchSemitones = 0.35;    // and detuned up to ±35 cents
inline constexpr double kSwarmShrink = 0.75;            // grains shrink to a quarter of Grain Size at full Swarm
inline constexpr double kSwarmDensityGain = 3.0;        // and spawn up to four times as often
inline constexpr uint64_t kSwarmSeed = 0x5741524d00000001ull; // "SWARM"

/// **GRAIN SIZE, AND THE BOTTOM OF IT GENUINELY MEANS OFF.** `Unit::LogSeconds` prints "off" at
/// exactly `desc.min` (`value_box.cpp`, and device.h states the convention), so a range whose bottom
/// was merely "very short grains" would be a card promising a stage the device does not skip — the
/// same class of lie the convention exists to prevent. So the device honours it: at the minimum the
/// granulator passes the tap through untouched. The LFO's Smoothing is the same shape.
inline constexpr double kGrainSizeLog2Min = -8.0;     // the OFF position
inline constexpr double kGrainSizeLog2Max = -1.0;     // 500 ms
inline constexpr double kGrainSizeLog2Default = -4.32192809; // 50 ms

/// GRAINS PER SECOND, as `Unit::LogHertz` — it is a rate, and a rate in this project is logarithmic
/// and printed in hertz.
inline constexpr double kDensityLog2Min = -1.0;      // 0.5/s
inline constexpr double kDensityLog2Max = 7.64385619; // 200/s
inline constexpr double kDensityLog2Default = 4.32192809; // 20/s

/// HOW FAR A GRAIN'S READ HEAD MAY BE THROWN from the tap, at Spray 100%, as a fraction of the tap.
/// Half, so a fully sprayed grain still lands inside the material the tap is reading rather than in
/// whatever the ring held a lap ago.
inline constexpr double kSprayFraction = 0.5;

/// PITCH, IN SEMITONES, two octaves either way — the range a granular tail is played over.
inline constexpr double kPitchSemitonesMin = -24.0;
inline constexpr double kPitchSemitonesMax = 24.0;

/// **HOW LONG THE READ HEAD TAKES TO REACH A NEW TAP, in the repitching modes.**
///
/// This is the whole character of a moving delay time and it is a FEEL number, to be set by ear.
/// A one-pole approach rather than a constant rate, because the bend should be strongest at the
/// moment the hand moves and ease out — which is what a tape transport does and what a linear ramp
/// does not.
///
/// **MEASURED BEFORE IT EXISTED: without any of this the tap JUMPED.** A steady tone with Time
/// moved one notch (x1 -> x1.5) stepped by 0.173 against a typical sample-to-sample slope of
/// 0.031 — **5.5x the signal's own slope**, which is a click rather than a transition. The question that
/// found it: when the delay time changes, does the tap repitch or crossfade? The answer
/// was neither.
inline constexpr double kGlideSeconds = 0.08;

/// **HOW FAR THE HEAD MAY MOVE IN ONE SAMPLE**, which bounds the pitch bend rather than the time.
/// At 0.5 the read rate stays inside [0.5x, 1.5x] — about a fifth either way — so a violent jump
/// bends hard and briefly instead of tearing. A tape delay would go further; this is the bound that
/// keeps a modulated Time musical rather than a special effect, and it is the other number to set by
/// ear.
inline constexpr double kGlideMaxFramesPerSample = 0.5;

/// **HOW LONG THE CROSSFADE IS in `Xfade` mode.** Long enough that neither end clicks, short enough
/// that the doubling reads as a transition rather than as two delays. Equal-power, because the two
/// taps are reading different material and a linear fade would dip in the middle.
inline constexpr double kXfadeSeconds = 0.015;

//--------------------------------------------------------------------------------------------------//
// THE TAPE MACHINE.                                                                                  //
//--------------------------------------------------------------------------------------------------//
//
// **EVERY NUMBER BELOW IS A STARTING POINT TO BE SET BY EAR, and they are written as the physical
// quantity rather than as a coefficient so that the ear and the constant are talking about the same
// thing.** Real transports run 0.1–0.3% pitch deviation when healthy and 1–3% when worn; those are
// the figures these ranges bracket, not numbers this code measured.

/// **WOW IS A POSITION MODULATION, BUT THE CONTROL IS A PITCH DEVIATION — and the conversion is the
/// only interesting arithmetic here.** Modulating the read head by `A·sin(2πft)` gives an
/// instantaneous pitch deviation of `A·2πf/sr`, so a FIXED offset depth would wobble the pitch
/// harder the faster it ran. Declaring the deviation and solving for `A` is what makes Wow and
/// Flutter comparable: both say "this much pitch", and the rate is a separate fact about each.
inline constexpr double kWowMaxDeviation = 0.015;     // 1.5% at full — a tired machine
inline constexpr double kFlutterMaxDeviation = 0.005; // 0.5%: flutter is faster and shallower

/// **TWO INCOMMENSURATE RATES EACH, so neither reads as an LFO.** A single sine is a vibrato and a
/// hand hears it as one; a pair whose ratio is irrational never repeats its own shape. The same
/// argument the phi fields make elsewhere in this project, at a much smaller scale.
inline constexpr double kWowHzA = 0.83;
inline constexpr double kWowHzB = 1.37;
inline constexpr double kFlutterHzA = 7.3;
inline constexpr double kFlutterHzB = 11.9;

/// **WHERE THE HF LOSS PUTS ITS CORNER AT FULL.** Two kilohertz is a dark repeat rather than a muffled
/// one; the loss is applied PER PASS, so the tail darkens cumulatively and the fifth repeat is much
/// further down than one application of this suggests.
inline constexpr double kHfLossMinHz = 2000.0;
inline constexpr double kHfLossMaxHz = 18000.0;

/// **HOW HARD THE TAPE SATURATES AT FULL.** The drive of a `tanh` whose small-signal gain is held at
/// unity, so turning it up compresses and adds odd harmonics rather than simply getting louder —
/// and it bounds the loop on its own, which matters because this REPLACES the transparent safety
/// clip in Tape mode rather than sitting beside it.
inline constexpr double kTapeMaxDrive = 8.0;

//--------------------------------------------------------------------------------------------------//
// THE MULTI-TAP.                                                                                     //
//--------------------------------------------------------------------------------------------------//
//
// **FOUR CONTROLS DESCRIBE N TAPS, RATHER THAN N TAPS HAVING THREE CONTROLS EACH**, and the flex row
// is what decides it: the mode's controls live in one row of four, and a per-tap time, level and pan
// for even four taps is twelve. So the taps are PARAMETRIC — how many, how they are spaced, how
// their levels run, how wide they sit — which is also the more playable instrument: one knob
// reshapes the whole rhythm instead of one tap of it.
//
// **THE LAST TAP IS THE BASE TAP, AND IT ALONE FEEDS THE LOOP.** Two decisions in one sentence:
//
//   - Tap k of N sits at a FRACTION of the base time and tap N sits AT it, so `Sync` and `Time`
//     still mean exactly one thing — the length of the whole pattern — in this mode as in every
//     other.
//   - The loop is fed from the raw read at the base tap and NOT from the summed taps. Fed from the
//     sum, every tap re-enters the line and re-taps, so the second cycle is N*N echoes and the third
//     is mush; fed from the base tap, the pattern REPEATS — cycle two is cycle one again, quieter —
//     which is what a multi-tap delay is for. It also leaves `Feedback` owning the loop gain
//     alone: `Slope` can take the last tap's audible level to zero without starving the repeats.

/// As many as eight. Two reads per tap per sample, so the ceiling is a cost that was looked at
/// rather than a number that sounded generous.
inline constexpr int kMinTaps = 2;
inline constexpr int kMaxTaps = 8;

/// **HOW FAR `Shape` BENDS THE SPACING.** Tap k sits at `(k/N)^p` of the base with `p = 2^shape`, so
/// the knob's centre is EVEN spacing (p = 1) and an octave either way is the range: at +1 the gaps
/// widen toward the end like an echo slowing down, at -1 they narrow like a ball bouncing to rest.
inline constexpr double kShapeOctaves = 1.0;

//--------------------------------------------------------------------------------------------------//
// PATTERN AND LEVELS AS ZONE CONTROLS — backlog MH: more ways to shape the taps, at least in multi-  //
// tap mode, by making Shape and Slope zone controls.                                                 //
//--------------------------------------------------------------------------------------------------//
//
// **EACH IS ONE UNBOUNDED COORDINATE, the house zone cell** (Resonate's Cycle is the model): the fraction of a lap,
// times the zone count, is the ZONE — a family of patterns — and where the coordinate sits inside the zone is the
// POSITION, 0..1, which morphs within the family. Turning on past the last zone comes round to the first, and a route
// moves one zone per full depth (`modSpan`). A family change re-places the taps, which rides MD's per-tap crossfade
// like any Shape move.
//
// **ZONE 0 OF EACH IS TODAY'S CONTROL**, and the default is ITS CENTRE: Pattern's zone 0 is the old Shape's power
// law, Levels' is the old Slope's linear run, and both default to position 0.5 — where the old defaults (Shape 0,
// Slope −0.35) sit EXACTLY, so a device that never touches them renders the bits it rendered before (asserted).
// Four zones each, so the default coordinate 0.5/4 = 0.125 is exact in a float.

inline constexpr uint32_t kTapZones = 4;
inline constexpr double kTapOneZone = 1.0 / static_cast<double>(kTapZones);
/// The centre of zone 0 — both controls' default.
inline constexpr float kTapCoordDefault = 0.5f / static_cast<float>(kTapZones);
inline constexpr double kTapCoordMax = 8388608.0;

/// WHERE THE TAPS FALL, by zone. Every family keeps the LAST tap at the full time (Sync and Time still mean the
/// pattern's length) and the taps in order.
enum class TapPattern : int { Curve, Golden, Swing, Scatter, kCount };
inline constexpr device::ZoneName kTapPatternZoneNames[] = {
	{ "Curve", "The taps bunched toward the end like a ball bouncing to rest, even at the centre, or spread out like "
	           "an echo slowing down" },
	{ "Golden", "Each gap the golden ratio of the one before: shrinking one way, growing the other, even at the "
	            "centre" },
	{ "Swing", "Even taps with every other one pushed late, from straight to a heavy shuffle" },
	{ "Scatter", "Even taps each nudged by its own fixed random amount, from even to loose — the same every time" },
};
static_assert(std::size(kTapPatternZoneNames) == static_cast<size_t>(TapPattern::kCount));
static_assert(std::size(kTapPatternZoneNames) == kTapZones);

/// HOW LOUD EACH TAP IS, by zone.
enum class TapLevels : int { Fade, Arch, Alternate, Scatter, kCount };
inline constexpr device::ZoneName kTapLevelsZoneNames[] = {
	{ "Fade", "The levels running across the pattern: fading out, level at the centre-right, swelling in" },
	{ "Arch", "Loudest in the middle of the pattern, or quietest there" },
	{ "Alternate", "Every other tap quieter, from level to only the accents" },
	{ "Scatter", "Each tap quieter by its own fixed random amount, from level to uneven — the same every time" },
};
static_assert(std::size(kTapLevelsZoneNames) == static_cast<size_t>(TapLevels::kCount));
static_assert(std::size(kTapLevelsZoneNames) == kTapZones);

/// A COORDINATE DECODED: its zone and the position inside it, 0..1.
struct TapZone {
	int zone = 0;
	float position = 0.5f;
};
[[nodiscard]] inline TapZone decodeTapCoord(float coord) {
	if (!std::isfinite(coord)) {
		return {};
	}
	const double c = static_cast<double>(coord);
	const double lap = c - std::floor(c); // 0..1 of a lap
	const double scaled = lap * static_cast<double>(kTapZones);
	const int zone = std::clamp(static_cast<int>(std::floor(scaled)), 0, static_cast<int>(kTapZones) - 1);
	return { zone, static_cast<float>(scaled - static_cast<double>(zone)) };
}

/// **A POSITION AS A SIGNED AMOUNT, −1..1, 0 AT THE CENTRE**, saturating over the zone's outer 1% so each family's
/// extremes are reachable inside it (the next zone starts where the coordinate reaches the boundary). At 0.5 it is
/// `0 / 0.49`, exactly 0.
[[nodiscard]] inline float tapAmountOf(float position) {
	return std::clamp((position - 0.5f) / 0.49f, -1.0f, 1.0f);
}
/// THE OLD SHAPE, from Pattern's zone 0.
[[nodiscard]] inline float curveShapeOf(float position) { return tapAmountOf(position); }
/// THE OLD SLOPE, from Levels' zone 0: the centre is the old default, −0.35, EXACTLY (`−0.35f + 0·k`), with −1 and +1
/// at the zone's ends — so the halves have different gradients, which is what puts the old default at the centre.
inline constexpr float kFadeCentreSlope = -0.35f;
[[nodiscard]] inline float fadeSlopeOf(float position) {
	const float u = tapAmountOf(position);
	return u <= 0.0f ? kFadeCentreSlope + u * (1.0f + kFadeCentreSlope) : kFadeCentreSlope + u * (1.0f - kFadeCentreSlope);
}

/// THE COORDINATES THAT MEAN AN OLD SHAPE OR SLOPE — the inverses, for whoever has a number in the old units (a test, a
/// preset author). Inside zone 0.
[[nodiscard]] inline float patternCoordForShape(float shape) {
	const float position = 0.5f + std::clamp(shape, -1.0f, 1.0f) * 0.49f;
	return position / static_cast<float>(kTapZones);
}
[[nodiscard]] inline float levelsCoordForSlope(float slope) {
	const float s = std::clamp(slope, -1.0f, 1.0f);
	const float u = s <= kFadeCentreSlope ? (s - kFadeCentreSlope) / (1.0f + kFadeCentreSlope)
	                                      : (s - kFadeCentreSlope) / (1.0f - kFadeCentreSlope);
	return (0.5f + u * 0.49f) / static_cast<float>(kTapZones);
}
/// A COORDINATE IN A NAMED ZONE at a position — the forward map, for the same callers.
[[nodiscard]] inline float tapCoordIn(int zone, float position) {
	return (static_cast<float>(zone) + std::clamp(position, 0.0f, 0.999f)) / static_cast<float>(kTapZones);
}

/// **A FIXED NUMBER PER TAP, −1..1** — Scatter's "random but seeded". DERIVED from the tap's index alone (a hash, no
/// state), so it is the same on every run and every reload by construction: nothing to persist (CLAUDE.md: derive
/// what can be derived).
[[nodiscard]] inline float tapScatterOf(int k) {
	uint32_t x = static_cast<uint32_t>(k + 1) * 0x9E3779B1u;
	x ^= x >> 15;
	x *= 0x2C1B3C6Du;
	x ^= x >> 12;
	return static_cast<float>(x & 0xFFFFu) / 32767.5f - 1.0f;
}

/// **WHERE TAP k OF N FALLS, as a fraction of the base** (k = 0..N−1, the last at exactly 1).
[[nodiscard]] inline double tapFractionOf(TapZone z, int k, int n) {
	const double along = static_cast<double>(k + 1) / static_cast<double>(n);
	if (k + 1 >= n) {
		return 1.0; // the base tap, in every family
	}
	switch (static_cast<TapPattern>(z.zone)) {
	case TapPattern::Curve:
	case TapPattern::kCount:
		// TODAY'S SHAPE, the same expression: `(k/N)^p`, `p = 2^shape`.
		return std::pow(along, std::exp2(static_cast<double>(curveShapeOf(z.position)) * kShapeOctaves));
	case TapPattern::Golden: {
		// GAPS IN A GOLDEN PROGRESSION: gap i ∝ φ^(d·i), d the signed amount, normalised to the base.
		const double d = static_cast<double>(tapAmountOf(z.position));
		constexpr double kPhi = 1.6180339887498949;
		double before = 0.0;
		double total = 0.0;
		for (int i = 0; i < n; ++i) {
			const double gap = std::pow(kPhi, d * static_cast<double>(i));
			total += gap;
			before += i <= k ? gap : 0.0;
		}
		return before / total;
	}
	case TapPattern::Swing: {
		// EVERY OTHER TAP LATE (the 1st, 3rd, ...), by up to half a step — a straight grid at the zone's start.
		const double late = (k % 2) == 0 ? 0.5 * static_cast<double>(z.position) : 0.0;
		return (static_cast<double>(k + 1) + late) / static_cast<double>(n);
	}
	case TapPattern::Scatter: {
		// EACH TAP NUDGED BY ITS OWN FIXED AMOUNT, up to 0.45 of a step either way, so the order holds.
		const double nudge = 0.45 * static_cast<double>(z.position) * static_cast<double>(tapScatterOf(k));
		return (static_cast<double>(k + 1) + nudge) / static_cast<double>(n);
	}
	}
	return along;
}

/// **HOW LOUD TAP k OF N IS, 0..1**, before Width and the power normalisation.
[[nodiscard]] inline float tapLevelOf(TapZone z, int k, int n) {
	const float t = n > 1 ? static_cast<float>(k) / static_cast<float>(n - 1) : 0.0f;
	switch (static_cast<TapLevels>(z.zone)) {
	case TapLevels::Fade:
	case TapLevels::kCount: {
		// TODAY'S SLOPE, the same expression.
		const float slope = fadeSlopeOf(z.position);
		return slope < 0.0f ? 1.0f + slope * t : 1.0f - slope * (1.0f - t);
	}
	case TapLevels::Arch: {
		const float u = tapAmountOf(z.position);
		const float middle = 1.0f - std::fabs(2.0f * t - 1.0f); // 1 at the middle, 0 at the ends
		return u >= 0.0f ? 1.0f - u * (1.0f - middle) : 1.0f + u * middle;
	}
	case TapLevels::Alternate:
		return (k % 2) == 0 ? 1.0f : 1.0f - z.position;
	case TapLevels::Scatter:
		return 1.0f - z.position * 0.5f * (1.0f + tapScatterOf(k + 101));
	}
	return 1.0f;
}

/// **HOW LONG A TAP TAKES TO GET WHERE `Shape` OR `Taps` HAS MOVED IT** (backlog MD). The tap times used to be
/// re-derived per block and read straight away, so a Shape sweep jumped every tap's read by `head · Δfrac` each
/// block: 6.7x the signal's own step by hand and 24x under an LFO, 290-570 clicks a second. Now a tap that moves
/// CROSSFADES from where it was to where it is asked to be (`Xfade` mode's answer applied per tap, but LINEARLY:
/// `delay.cpp` has the measurement that chose that over equal power), and a tap that appears or goes (Taps) fades
/// its gain over the same window; every tap gain ramps rather than steps. Ten
/// milliseconds: inside the 5-20 ms where neither end clicks, and short enough that a modulated Shape is re-read
/// a hundred times a second. A feel number, to be set by ear.
inline constexpr double kTapFadeSeconds = 0.010;

/// HOW FAR SPREAD PUSHES THE RIGHT CHANNEL, as a fraction of the tap, at 100%.
///
/// **A QUARTER, AND IT MOVES ONE CHANNEL RATHER THAN BOTH.** Symmetric spread (L earlier, R later)
/// reads better on paper and is worse in practice: it moves the delay a hand just dialled in, so the
/// tap is no longer the time the dropdown says for EITHER channel. Offsetting only the right leaves
/// the left channel meaning exactly what the controls say and makes Spread a widening rather than a
/// retune.
inline constexpr double kSpreadFraction = 0.25;

//--------------------------------------------------------------------------------------------------//
// THE REVERSE (backlog MB).                                                                          //
//--------------------------------------------------------------------------------------------------//
//
// **EACH REPEAT PLAYS THE MOST RECENT TIME-LONG CHUNK OF THE LINE BACKWARDS** — the classic reverse delay.
// A chunk is defined by its MIRROR, an instant `M` in the line: for one chunk's length `L` after `M` the head
// reads `2M - now`, so it walks back through the `L` before `M` at the speed the line is written, and the
// material comes out reversed at its own pitch. An impulse that went in `d` before a mirror comes out `d`
// after it.
//
// **TWO HEADS, HALF A CHUNK APART, EQUAL-GAIN.** A head that jumps from the oldest end of one chunk to the
// newest end of the next is a click, so each head is windowed by `sin^2` of its own phase, zero at both ends
// of its chunk, and the second head runs half a chunk behind with the complementary window `cos^2`. The two
// weights sum to exactly one, so a held tone comes through at its own level, and at the instant either head
// jumps its weight is zero. Equal GAIN rather than equal power because the two heads read the same line a
// half-chunk apart; decided 2026-10-11, and it is what makes an impulse's two returns sum
// to the impulse.
//
// **FEEDBACK IS TAKEN FROM A FORWARD READ AT THE CHUNK LENGTH, NOT FROM WHAT IS HEARD.** Fed back as heard,
// the reversed repeat would be reversed AGAIN on its next pass and come out forwards, so the repeats would
// alternate direction. Fed from the forward read, the line holds an ordinary echo train `in + fb·in(t-L) +
// ...`, and because the chunk grid repeats every `L` exactly, reversing a copy delayed by `L` is the reversed
// copy delayed by `L` — each further repeat is the reversed chunk, delayed again, quieter. The Multi-Tap's
// precedent: the loop is fed from the base tap's raw read rather than from the sum that is heard.

/// **A CHUNK READS BACK TO TWICE ITS LENGTH**, so Reverse's tap stops at half the line. At 120 BPM that is
/// `4 bar` and nothing in ordinary use touches it; past it the chunk is clamped, the same trade
/// `kMaxDelaySeconds` states for the other modes.
inline constexpr double kReverseMaxFraction = 0.5;

//--------------------------------------------------------------------------------------------------//
// THE CONTROLS.                                                                                      //
//--------------------------------------------------------------------------------------------------//

/// ORDER IS THE STREAM ORDER AND THE DESCRIPTOR ORDER AND THE ROW ORDER, all three.
enum class Param : int {
	Enabled,   ///< bypass switch
	Sync,      ///< which base the tap is: a free time or a musical division
	Time,      ///< a multiplier on whatever Sync selected — always, in both cases
	Feedback,  ///< how much of the wet returns to the line
	Tone,      ///< tilt in the feedback path: dark one way, thin the other
	PingPong,  ///< Off, Stereo (the feedback crosses) or Classic (and the input enters on the left only)
	Spread,    ///< pushes the right channel later, as a fraction of the tap
	Mode,      ///< Clean, or the granulator in the loop
	GrainSize, ///< grain length; at the bottom of its travel it is OFF
	Density,   ///< grains per second
	Spray,     ///< how far a grain's read head is thrown from the tap
	Pitch,     ///< per-grain transposition in semitones
	Mix,       ///< dry against wet — or how much reaches the line at all, in Dub
	Dub,       ///< the dry stops being a crossfade and becomes a full-level path beside the send
	Wow,       ///< the transport's slow wander — Tape only
	Flutter,   ///< the fast irregularity above it — Tape only
	Saturation,///< tape saturation, which REPLACES the transparent safety clip — Tape only
	HfLoss,    ///< how much top each pass loses, on top of Tone — Tape only
	Taps,      ///< how many taps the base time is divided into — Multi-Tap only
	TapPattern, ///< WHERE the taps fall: a zoned coordinate, its zone the spacing family (backlog MH) — Multi-Tap only
	TapLevels,  ///< how LOUD each tap is: a zoned coordinate, its zone the level family (backlog MH) — Multi-Tap only
	TapWidth,  ///< how far successive taps alternate across the stereo field — Multi-Tap only
	// THE DUCK, THE FREEZE AND THE CLEAR (delay-love S4). Appended: the ledger stores by key, so position is free, and
	// the end keeps every older index where it was.
	Duck,        ///< bipolar: positive ducks the heard repeats under the input, negative gates them to it
	DuckRelease, ///< how long the duck takes to let go, log2 seconds
	Freeze,      ///< hold the loop exactly: no input in, no loss, no lane
	Clear,       ///< a rising edge empties the line (a momentary in the window, a trigger to a route)
	// BBD AND SWARM (delay-love, 2026-11-05: Swarm is a setting on Grain). Appended.
	Swarm,        ///< Granular only: each repeat a cloud of tiny grains — spread in time, pan and pitch
	BbdStages,    ///< BBD only: the chip's length in stages — the clock that a Time needs, so the bandwidth it leaves
	BbdCompander, ///< BBD only: how hard the compander squeezes and restores — the pump
	BbdNoise,     ///< BBD only: hiss and the clock's whine, riding the signal
	BbdDrive,     ///< BBD only: how hard the chip's input saturates

	kCount,
};

inline constexpr const char* kOffOnNames[] = { "Off", "On" };

/// **PING-PONG IS THREE WAYS, BECAUSE A CENTRED SOUND DOES NOT BOUNCE IN THE FIRST ONE** (the
/// strip showed ticks on both sides at once, so the bouncing behaviour became another mode, Classic).
///
/// - `Stereo` is what `On` always was: each input feeds its own line and only the FEEDBACK crosses. A hard-left
///   sound bounces L, R, L; a centred one has the same thing in both lines, and swapping two equal signals
///   changes nothing, so it repeats on both sides at once. Kept, because it is the one that keeps a stereo
///   source's image.
/// - `Classic` is the one a listener expects: while it is on, the input is summed to mono and fed into the LEFT
///   line only, so EVERY sound bounces L, R, L.
///
/// **THE KEY IS KEPT AND `On` IS ALIASED TO `Stereo`, RATHER THAN A NEW KEY.** A Choice is stored by its label,
/// so the stored value of an old patch is the word `On` — and that word means exactly what `Stereo` means now,
/// to the bit (`test_fx_delay` pins it). A new key would have retired the old one and dropped every saved `On`
/// to `Off`; the alias keeps it. Nothing an old patch stored changes meaning, which is the ledger's test.
enum class PingPong : int { Off, Stereo, Classic, kCount };
inline constexpr const char* kPingPongNames[] = { "Off", "Stereo", "Classic" };
static_assert(std::size(kPingPongNames) == static_cast<size_t>(PingPong::kCount));
inline constexpr device::ChoiceAlias kPingPongAliases[] = { { "On", "Stereo" } };

/// **CLEAN AND GRANULAR, AND THE CHOICE IS WHAT GATES THE FOUR GRAIN CONTROLS.** Two values, so the
/// mask below is a pair of bits rather than anything that needs explaining — and `SwitchChip`'s own
/// rule applies (a two-state control's *next* and *which* are the same thing), which is why this is
/// not the multivalue-toggle antipattern.
/// **HOW THE WET PATH BEHAVES — including what it does when the tap MOVES.**
///
/// **`Xfade` IS HERE RATHER THAN AS A SWITCH BESIDE `Time`, by design** (2026-09-21: Xfade is a
/// mode alongside Granular and Clean, while repitching stays the default behaviour). It reads as a
/// third character rather than a second axis: each entry is a different answer to "what is the wet
/// path doing", and moving the tap is part of that answer.
///
/// **`Reverse` IS APPENDED, AND APPENDING IS THE SAFE PLACE** (backlog MB 2026-10-11: a reverse mode
/// for the delay). A Choice is stored by its LABEL (the schema ledger), so a sixth label moves no
/// saved patch; and it is last so every older value keeps its index too.
enum class Mode : int { Clean, Granular, Xfade, Tape, MultiTap, Reverse, Bbd, kCount };
inline constexpr const char* kModeNames[] = { "Clean", "Granular", "Xfade", "Tape", "Multi-Tap", "Reverse", "Buckets" };
/// "BBD" WAS RENAMED "Buckets". Stored by label, so a patch saved with BBD finds it here. Internal
/// identifiers (`Mode::Bbd`, the `delay_bbd_*` keys) keep the old name: a key names a meaning, and that did not move.
inline constexpr device::ChoiceAlias kModeAliases[] = { { "BBD", "Buckets" } };
static_assert(std::size(kModeNames) == static_cast<size_t>(Mode::kCount));

/// WHICH MODES A GATED CONTROL BELONGS TO, as a mask over `Mode` — `ParamDesc::gateMask`'s shape,
/// named once rather than written as a bit at each use.
inline constexpr uint32_t kModeBit(Mode m) {
	return 1u << static_cast<uint32_t>(m);
}
/// The four the granulator owns. **THE HONESTY RULE IN THE DIRECTION THAT HIDES A DEAD CONTROL**:
/// in Clean mode there is no granulator for these to reach, and `GenericCard` asks `device::
/// relevantFor` per control, so declaring the gate is the whole of the work.
inline constexpr uint32_t kGranularOnly = kModeBit(Mode::Granular);

/// The four the tape machine owns. **THEY OCCUPY THE SAME ROOM AS THE GRAIN FOUR AND THAT IS BY
/// DESIGN**: tape-specific params fit in the same space and follow the same expand pattern as
/// grain. The two groups are mutually exclusive because `Mode` is, so
/// exactly one section is ever live and the card is the same height in either — the expand is the
/// same gesture wearing a different heading.
inline constexpr uint32_t kTapeOnly = kModeBit(Mode::Tape);

/// The four the multi-tap owns — the third tenant of the flex row (the same idea:
/// additional controls go in the flex row). Same gate, same room, same expand.
inline constexpr uint32_t kMultiTapOnly = kModeBit(Mode::MultiTap);

/// THE FOUR THE BUCKET BRIGADE OWNS (delay-love, 2026-11-05) — the flex row's fourth tenant, never live beside the others.
inline constexpr uint32_t kBbdOnly = kModeBit(Mode::Bbd);

/// WHAT THIS DEVICE IS, for a browser to group it by.
inline constexpr const char* kTags[] = { "delay", "fx" };

/// **THE ORDER THE FACE READS IN, which is not the stream order.** `Param` is the row order a patch
/// stores and must not move; this is what a hand scans. Mix sits with the Tone it balances rather
/// than at the bottom under four grain knobs, and Sync leads because it is the control the others
/// are measured against.
///
/// `Enabled` is deliberately absent: the shared card head draws it as its square, and a device
/// listing its enable on the face too would have one field with two controls.
inline constexpr uint8_t kFaceOrder[] = {
	// **THE TOP ROW IS THE FOUR YOU REACH FOR** (Sync, Time, Mode, Mix) — what the tap is measured in, how long it is, which flavour of delay, and how much of
	// it you hear. Everything below is a detail of one of those four.
	static_cast<uint8_t>(Param::Sync),      static_cast<uint8_t>(Param::Time),
	// `Dub` follows `Mode` because it STACKS onto it — the flag is on the follower and means "the
	// cell before me in this order", so the two must be adjacent here or the pairing moves with it.
	static_cast<uint8_t>(Param::Mode),      static_cast<uint8_t>(Param::Dub),
	static_cast<uint8_t>(Param::Mix),
	// Then the loop, four across.
	static_cast<uint8_t>(Param::Feedback),  static_cast<uint8_t>(Param::Tone),
	static_cast<uint8_t>(Param::PingPong),  static_cast<uint8_t>(Param::Spread),
	// **DUCK, ITS RELEASE, FREEZE AND SPRAY ARE NOT ON THE CARD**, because the card had grown too
	// large; they are removed from the card but not from the window. The window keeps all four (its
	// HOLD band and its Granular band, `delay_expanded.cpp`), so the card is still a subset of it; CLEAR was never here
	// (a grid cell would latch a momentary). Their DUCK / FREEZE heading went with them.
	// Then the grain controls, which the Mode above summons — three, and SWARM (2026-11-05) beside them, gated with them:
	// one row of four, the same height as the Tape, Multi-Tap and BBD rows below.
	static_cast<uint8_t>(Param::GrainSize), static_cast<uint8_t>(Param::Density),
	static_cast<uint8_t>(Param::Pitch),     static_cast<uint8_t>(Param::Swarm),
	// **AND THE TAPE FOUR, IN THE SAME ROOM.** Listed after the grain four, but the two groups are
	// never live together — `Mode` is exclusive and each group is gated on its own value — so this
	// is one row of four on the card whichever is showing, and the card is the same height in
	// Granular as in Tape.
	static_cast<uint8_t>(Param::Wow),        static_cast<uint8_t>(Param::Flutter),
	static_cast<uint8_t>(Param::Saturation), static_cast<uint8_t>(Param::HfLoss),
	// AND THE MULTI-TAP FOUR — the flex row's third tenant, never live beside the other two.
	static_cast<uint8_t>(Param::Taps),       static_cast<uint8_t>(Param::TapPattern),
	static_cast<uint8_t>(Param::TapLevels),   static_cast<uint8_t>(Param::TapWidth),
	// AND THE BBD FOUR (2026-11-05) — the flex row's fourth tenant.
	static_cast<uint8_t>(Param::BbdStages),  static_cast<uint8_t>(Param::BbdCompander),
	static_cast<uint8_t>(Param::BbdNoise),   static_cast<uint8_t>(Param::BbdDrive),
};

/// **WHERE THE HEADINGS FALL.** Keyed by the control each section OPENS AT — an index into `params`,
/// not a position in `kFaceOrder`, so reordering the face above moves the headings with it.
///
/// **`GRAIN` OPENS AT `Mode` RATHER THAN AT `Grain Size`, and that is the whole design.** Putting it
/// at the first grain knob would leave the switch that summons them sitting in the section above,
/// which is an arrangement where the connection is not obvious at all. Opening the section AT the switch
/// makes the picker the section's first row: what it governs is everything under its own heading.
inline constexpr device::FaceSection kFaceSections[] = {
	// **THE TOP ROW CARRIES NO HEADING**, because a heading over the four principal controls would
	// be naming the device again two lines under its own title. The sections below it are the
	// groups that need saying.
	{ "FEEDBACK", static_cast<uint8_t>(Param::Feedback) },
	// **GRAIN NO LONGER OPENS AT `Mode`, AND THAT IS A DELIBERATE TRADE RATHER THAN A SIMPLIFICATION.**
	// It used to, so that the switch summoning the group sat as the group's own first row — the most
	// direct answer to Mode enabling the grain controls with nothing on the face to say so. Moving
	// `Mode` up to the top row costs that adjacency and buys something else: in Clean mode EVERY
	// control under this heading is gated away, so the section does not draw at all, and switching
	// Mode makes a whole titled GRAIN block appear where there was none. The connection is now
	// "I moved that and a section arrived" rather than "the picker is sitting in the group it
	// fills". Both say it; the second said it while standing still, the first says it on the move.
	{ "GRAIN", static_cast<uint8_t>(Param::GrainSize) },
	// **THE SAME EXPAND, A DIFFERENT HEADING.** Every control under it is gated on `Mode == Tape`,
	// so it is absent in the other three modes and appears when the mode moves — the pattern GRAIN
	// established, reused rather than reinvented.
	{ "TAPE", static_cast<uint8_t>(Param::Wow) },
	{ "TAPS", static_cast<uint8_t>(Param::Taps) },
	{ "BUCKETS", static_cast<uint8_t>(Param::BbdStages) },
};

/// THE OLD SHAPE AND SLOPE, RETIRED (backlog MH): each a −1..1 amount, now a zoned coordinate whose zone 0 is the old
/// control. Same place in the face, different number — so new keys.
inline constexpr device::RetiredKey kRetiredKeys[] = {
	{ "delay_tap_shape", "2026-10-30", "became the zoned coordinate delay_tap_pattern, whose Curve zone is the old Shape" },
	{ "delay_tap_slope", "2026-10-30", "became the zoned coordinate delay_tap_levels, whose Fade zone is the old Slope" },
};

inline constexpr fx::TenantTraits kTraits {
	// FIVE CHARACTERS, WHICH FITS — `device::kTagBytes` is 8. It was briefly going to be `DLAY`
	// against a four-byte field that no longer exists; OWLTT's own comment records that widening.
	.tag = "DELAY",
	.name = "Delay",
	.hasCard = true,
	// **A WINDOW (delay-love S2)**: the time plane large, every control around it, the FEEDBACK FX lane on the right —
	// `plugin/ui/delay_expanded.h`. Strictly a superset of the card (the tooltips suite's guard walks every Mode).
	.hasExpanded = true,
	// **ONE LINE ON THE BUS, NOT ONE PER VOICE.** A delay is a bus effect and a per-voice road would
	// mean a 6 MB ring per voice. `-1` is the declaration that there is no per-voice road, and
	// device.h is explicit that a device without one must not claim it.
	.perVoiceControl = -1,
	.extraState = 0,
	// WHICH CONTROL SWITCHES THE DEVICE IN AND OUT, so the shared head draws its enable square from
	// the declaration rather than from a lane branch.
	.enableControl = static_cast<int>(Param::Enabled),
	// MODULATOR-ONLY (backlog DH); an fx device leaves it alone.
	.amountControl = -1,
	.faceControls = 0,
	// **THE FACE IS A GRID, NOT A COLUMN.** Thirteen controls one-per-row is a card that scrolls in
	// Clean mode and loses its Mix off the bottom in Granular — a knob list is not
	// ideal. Two across, so a section is a pair of rows rather than four.
	.faceOrder = kFaceOrder,
	.faceColumns = 4,
	.tags = kTags,
	// **THE HEADINGS, AND THE GRAIN ONE IS THE POINT OF THEM.** Four controls appear when Mode moves
	// to Granular and nothing on a flat face says which control summoned them, so the link between Mode
	// and the grain controls was not obvious at all. A heading immediately under Mode makes
	// the group visible as a group, and because a section is drawn only when something beneath it is
	// live, it is ABSENT in Clean mode rather than sitting over four hidden rows.
	.faceSections = kFaceSections,
	.retiredKeys = kRetiredKeys,
	// ITS MIX IS ITS OWN (backlog NR, D2): named for the frame, made by the device, so the slot does not blend and a
	// POST chain is a series insert after it.
	.mixControl = static_cast<int>(Param::Mix),
	.mixInDevice = true,
};

/// **EVERY ROW BY FIELD NAME, NOT POSITIONALLY.** device.h records the hazard twice over: every
/// declaration in this repo initialises `ParamDesc` positionally, so a field inserted mid-struct
/// silently re-points every value after it — `modSpan` once landed in `zoneNames`, and a tooltip once
/// slid into a bool. Half these rows set a gate and a bool, which is exactly the case the file says to
/// write out.
inline constexpr fx::ParamDesc kParamDescs[] = {
    {.key = "delay_enabled", .name = "Delay Enabled",
     .min = 0.0,
     .max = 1.0,
     .def = 1.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kOffOnNames,
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Bypass switch — off removes the delay from the chain"},
    {.key = "delay_sync", .name = "Delay Sync",
     .min = 0.0,
     .max = static_cast<double>(kSyncChoiceCount) - 1.0,
     .def = static_cast<double>(kDefaultSync),
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kSyncNames,
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "What the tap is measured in — a free time, or a musical division locked to the host "
                "tempo. Delay Time multiplies whichever you pick"},
    {.key = "delay_time", .name = "Delay Time",
     .min = kTimeLog2Min,
     .max = kTimeLog2Max,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::LogMultiple,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "How many of the selected base one tap is — always a multiplier, whether the base is "
                "a free time or a division"},
    {.key = "delay_feedback", .name = "Delay Feedback",
     .min = 0.0,
     .max = kFeedbackMax,
     .def = 0.35,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "How much of the wet signal returns to the line. Past 100% it sustains instead of "
                "decaying, and past 110% the loop limiter holds the swell at full scale; below that "
                "the soft clip in the loop is what keeps it bounded",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = false,
     // UNDER A HEADING THAT SAYS FEEDBACK, the word is already on the card.
     .shortName = "Amount"},
    {.key = "delay_tone", .name = "Delay Tone",
     .min = -1.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Tilt in the feedback path — left darkens each repeat, right thins it. Flat in the "
                "middle",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = true},
    {.key = "delay_ping_pong", .name = "Delay Ping-Pong",
     .min = 0.0,
     .max = static_cast<double>(PingPong::kCount) - 1.0,
     .def = 0.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kPingPongNames,
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "The feedback crosses channels. Stereo keeps each input on its own side, so a centred "
                "sound repeats on both sides at once and only an off-centre one bounces. Classic sums "
                "the input to mono into the left side, so every sound bounces left, right, left",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = false,
     // UNDER A HEADING THAT SAYS FEEDBACK — "Ping-Pong" is what it is called everywhere, so it stays.
     .shortName = nullptr,
     .stacked = false,
     .choiceAliases = kPingPongAliases},
    {.key = "delay_spread", .name = "Delay Spread",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     // THE END OF THE KNOB IS THE END OF THE MECHANISM — the filter's Spread says no for the same
     // reason, and the ring's length is what bounds it besides.
     .offsetsMayLeaveRange = false,
     .tooltip = "Pushes the right channel's tap later than the left, widening the repeats. The left "
                "channel always means exactly what Sync and Time say"},
    {.key = "delay_mode", .name = "Delay Mode",
     .min = 0.0,
     .max = static_cast<double>(Mode::kCount) - 1.0,
     .def = 0.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kModeNames,
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "Clean is one tap. Granular puts a granulator inside the feedback loop, so each "
                "repeat is granularised again and the tail disintegrates. Both REPITCH when the "
                "delay time moves — the head glides and the material in flight bends. Xfade is one "
                "tap that fades between the old time and the new instead, so it changes time "
                "without bending. Reverse plays each Time-long chunk backwards, and every repeat is "
                "that reversed chunk again" ". Buckets is a bucket-brigade chip: it darkens as the time grows, "
                "with a compander, hiss and drive of its own",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = false,
     .shortName = nullptr,
     .stacked = false,
     .choiceAliases = kModeAliases},
    {.key = "delay_grain_size", .name = "Delay Grain Size",
     .min = kGrainSizeLog2Min,
     .max = kGrainSizeLog2Max,
     .def = kGrainSizeLog2Default,
     .stepped = false,
     .unit = fx::ParamUnit::LogSeconds,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "How long one grain is. At the bottom of its travel it is off and the tap passes "
                "through untouched",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kGranularOnly,
     .centred = false,
     // UNDER A HEADING THAT SAYS GRAIN — `GRAIN SIZE` clipped to `GRAIN SIZ` at four across.
     .shortName = "Size"},
    {.key = "delay_density", .name = "Delay Density",
     .min = kDensityLog2Min,
     .max = kDensityLog2Max,
     .def = kDensityLog2Default,
     .stepped = false,
     .unit = fx::ParamUnit::LogHertz,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "How many grains a second are started. Sparse and long overlaps into a smear; dense "
                "and short is a texture",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kGranularOnly},
    {.key = "delay_spray", .name = "Delay Spray",
     .min = 0.0,
     .max = 1.0,
     .def = 0.25,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "How far each grain's read head is thrown from the tap. Low values smear the repeat "
                "across time; high values break its rhythm apart",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kGranularOnly},
    {.key = "delay_pitch", .name = "Delay Pitch",
     .min = kPitchSemitonesMin,
     .max = kPitchSemitonesMax,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Number,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Transposes each grain, in semitones. Inside the feedback loop it compounds — every "
                "repeat is shifted again, so the tail climbs or falls away",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kGranularOnly,
     .centred = true},
    {.key = "delay_mix", .name = "Delay Mix",
     .min = 0.0,
     .max = 1.0,
     .def = 0.35,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Dry against wet — or, with Dub on, how much of the input reaches the line at all, "
                "with the dry passing at full level beside it"},
    // **DUB: THE SEND, NOT THE BALANCE** (a dub mode switches Mix to control the input sent into the
    // wet chain, with the dry summed at 100%).
    //
    // **IT IS ITS OWN CONTROL RATHER THAN A THIRD `Mode`, and the axis is the reason.** `Mode` is
    // what the WET PATH DOES — Clean, Granular, and the Analog and Tape named for later.
    // Dub is how the wet is ROUTED against the dry. Folding it into Mode would make them mutually
    // exclusive and cost granular dub, which is the combination most worth having: feed grains in,
    // cut the send, let the cloud wash out.
    //
    // **AND THE SIGNATURE MOVE FALLS OUT OF THE ROUTING RATHER THAN BEING BUILT.** Because Mix
    // gates what ENTERS the line instead of what leaves it, taking it to zero in Dub leaves
    // everything already in the line decaying — the phrase washes out instead of being cut off. A
    // crossfade cannot do that at any setting.
    {.key = "delay_dub", .name = "Delay Dub",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kOffOnNames,
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "Mix becomes a SEND into the delay and the dry passes at full level beside it. "
                "Pull the send down and the repeats already in the line keep washing out",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = false,
     .shortName = nullptr,
     // **SHARES `Mode`'S CELL, SITTING UNDER IT** (perhaps we can double stack mode and the
     // dub button into one knob height). A picker and a switch are each far shorter than the knob
     // the cell is sized for, so the pair costs the top row nothing and Dub needs no row of its own.
     .stacked = true},
    //-- THE TAPE MACHINE'S FOUR. Gated on `Mode`, exactly as the grain four are, and occupying the
    //-- same room because the two are mutually exclusive.
    {.key = "delay_wow", .name = "Delay Wow",
     .min = 0.0,
     .max = 1.0,
     .def = 0.3,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "The slow wander of a tape transport — a drifting pitch rather than a vibrato. "
                "Compounds through the feedback, so each repeat is less steady than the one before",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kTapeOnly},
    {.key = "delay_flutter", .name = "Delay Flutter",
     .min = 0.0,
     .max = 1.0,
     .def = 0.25,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "The fast irregularity above the wow — shallower and quicker, the texture rather "
                "than the drift",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kTapeOnly},
    {.key = "delay_saturation", .name = "Delay Saturation",
     .min = 0.0,
     .max = 1.0,
     .def = 0.35,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "How hard the tape saturates. Unlike the safety clip the other modes use, this is "
                "audible well below full scale — it compresses and thickens as the feedback builds",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kTapeOnly,
     .centred = false,
     .shortName = "Sat"},
    {.key = "delay_hf_loss", .name = "Delay HF Loss",
     .min = 0.0,
     .max = 1.0,
     .def = 0.4,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "How much top each pass loses. Applied per repeat on top of Tone, so the tail "
                "darkens cumulatively and dissolves rather than fading as a bright copy",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     // **THIS ROW SHIPPED FOR TEN MINUTES WITHOUT ITS GATE**, ended at the tooltip, so `HF Loss`
     // would have drawn in all four modes and done nothing in three of them — the dead control
     // the honesty rule forbids. The section-bounded case in `test_fx_delay.cpp` caught it as a
     // `gateControl` of -1 where it expected `Mode`; nothing else would have, since an ungated
     // control renders perfectly and simply does not work.
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kTapeOnly},
    //-- THE MULTI-TAP'S FOUR, the flex row's third tenant.
    {.key = "delay_taps", .name = "Delay Taps",
     .min = static_cast<double>(kMinTaps),
     .max = static_cast<double>(kMaxTaps),
     .def = 4.0,
     .stepped = true,
     .unit = fx::ParamUnit::Number,
     .names = {},
     // A DESTINATION SINCE BACKLOG MD: still a whole count, so a route STEPS it (`withRouteOffset` rounds), and each
     // step fades the taps that come and go and crossfades the ones that move — never a pop.
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "How many taps the delay time is divided into. The last tap always sits at the "
                "full time, so Sync and Time still set the length of the whole pattern",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kMultiTapOnly},
    // **PATTERN AND LEVELS, THE ZONED PAIR (backlog MH, 2026-10-30).** New keys because the number means something new
    // (a coordinate, not a −1..1 amount); `delay_tap_shape`/`_slope` are retired below. Each defaults to the centre of
    // its zone 0, which is the old control at its old default, exactly.
    {.key = "delay_tap_pattern", .name = "Delay Tap Pattern",
     .min = -kTapCoordMax,
     .max = kTapCoordMax,
     .def = static_cast<double>(kTapCoordDefault),
     .stepped = false,
     .unit = fx::ParamUnit::Number,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Where the taps fall. Its zone is the family — Curve, Golden, Swing, Scatter — and turning within it "
                "shapes that family; Curve's centre is even spacing. It keeps turning. The last tap always sits at "
                "the full time",
     .zones = kTapZones,
     .modSpan = kTapOneZone,
     .zoneNames = kTapPatternZoneNames,
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kMultiTapOnly,
     .centred = false,
     .shortName = "Pattern"},
    {.key = "delay_tap_levels", .name = "Delay Tap Levels",
     .min = -kTapCoordMax,
     .max = kTapCoordMax,
     .def = static_cast<double>(kTapCoordDefault),
     .stepped = false,
     .unit = fx::ParamUnit::Number,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "How loud each tap is. Its zone is the family — Fade, Arch, Alternate, Scatter — and turning within "
                "it shapes that family. It shapes what is HEARD: the repeats are fed from the last tap at full level "
                "whatever this says, so Feedback alone owns the loop",
     .zones = kTapZones,
     .modSpan = kTapOneZone,
     .zoneNames = kTapLevelsZoneNames,
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kMultiTapOnly,
     .centred = false,
     .shortName = "Levels"},
    {.key = "delay_tap_width", .name = "Delay Tap Width",
     .min = 0.0,
     .max = 1.0,
     .def = 0.5,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "How far successive taps alternate left and right. At zero every tap is centred; "
                "at full they land hard left, hard right, in turn",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kMultiTapOnly,
     .centred = false,
     .shortName = "Width"},
    // delay-love S4 — NEW KEYS, each at a default where the device is what it was.
    {.key = "delay_duck", .name = "Delay Duck",
     .min = -1.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Right ducks the repeats while the input plays, so they bloom in the gaps; left gates them, so they "
                "sound only while it plays. Centre is off",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = true,
     .shortName = "Duck"},
    {.key = "delay_duck_release", .name = "Delay Duck Release",
     .min = kDuckReleaseLog2Min,
     .max = kDuckReleaseLog2Max,
     .def = kDuckReleaseLog2Default,
     .stepped = false,
     .unit = fx::ParamUnit::LogSeconds,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "How long the duck takes to let go once the input stops",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = false,
     .shortName = "Release"},
    {.key = "delay_freeze", .name = "Delay Freeze",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kOffOnNames,
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Hold the loop as it is, for ever: nothing new goes in and nothing fades. The switch is saved with "
                "the patch; the held audio is not",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = false,
     .shortName = "Freeze"},
    {.key = "delay_clear", .name = "Delay Clear",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kOffOnNames,
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Empty the line: the repeats fade out at once. A route clears on each rising edge",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = false,
     .shortName = "Clear"},
    // delay-love 2026-11-05 — SWARM ON GRANULAR AND THE BBD FOUR, new keys, each gated to its mode.
    {.key = "delay_swarm", .name = "Delay Swarm",
     .min = 0.0, .max = 1.0, .def = 0.0, .stepped = false, .unit = fx::ParamUnit::Percent, .names = {},
     .modulatable = true, .offsetsMayLeaveRange = false,
     .tooltip = "Turns each repeat into a cloud of tiny grains, thrown about its place in time, panned and slightly "
                "detuned. At 0 the granulator is as it was",
     .zones = 0, .modSpan = 0.0, .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode), .gateMask = kGranularOnly, .centred = false,
     .shortName = "Swarm"},
    {.key = "delay_bbd_stages", .name = "Delay Buckets Stages",
     .min = 0.0, .max = static_cast<double>(std::size(kBbdStageNames)) - 1.0, .def = static_cast<double>(kBbdDefaultStages),
     .stepped = true, .unit = fx::ParamUnit::Choice, .names = kBbdStageNames,
     .modulatable = false, .offsetsMayLeaveRange = false,
     .tooltip = "How long the chip is. A bucket brigade clocks slower for a longer delay, so it loses its top as the "
                "time grows — more stages keep it brighter at the same time",
     .zones = 0, .modSpan = 0.0, .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode), .gateMask = kBbdOnly, .centred = false,
     .shortName = "Stages"},
    {.key = "delay_bbd_compander", .name = "Delay Buckets Compander",
     .min = 0.0, .max = 1.0, .def = 0.5, .stepped = false, .unit = fx::ParamUnit::Percent, .names = {},
     .modulatable = true, .offsetsMayLeaveRange = false,
     .tooltip = "The compressor in front of the chip and the expander after it. Up, quiet repeats lift and the tail "
                "pumps and breathes",
     .zones = 0, .modSpan = 0.0, .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode), .gateMask = kBbdOnly, .centred = false,
     .shortName = "Compander"},
    {.key = "delay_bbd_noise", .name = "Delay Buckets Noise",
     .min = 0.0, .max = 1.0, .def = 0.2, .stepped = false, .unit = fx::ParamUnit::Percent, .names = {},
     .modulatable = true, .offsetsMayLeaveRange = false,
     .tooltip = "Hiss and the clock's whine, riding on the repeats. The whine is heard on long times, where the clock "
                "falls into the audible band",
     .zones = 0, .modSpan = 0.0, .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode), .gateMask = kBbdOnly, .centred = false,
     .shortName = "Noise"},
    {.key = "delay_bbd_drive", .name = "Delay Buckets Drive",
     .min = 0.0, .max = 1.0, .def = 0.3, .stepped = false, .unit = fx::ParamUnit::Percent, .names = {},
     .modulatable = true, .offsetsMayLeaveRange = false,
     .tooltip = "How hard the chip's input saturates",
     .zones = 0, .modSpan = 0.0, .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode), .gateMask = kBbdOnly, .centred = false,
     .shortName = "Drive"},
};
static_assert(std::size(kParamDescs) == static_cast<size_t>(Param::kCount));

//--------------------------------------------------------------------------------------------------//
// WHAT A PATCH STORES.                                                                               //
//--------------------------------------------------------------------------------------------------//

/// The DSP's input. Every field is in the units the descriptor declares, EXCEPT the three logarithmic
/// ones, which are resolved to seconds and hertz here so the sample loop never calls `exp2`.
struct Settings {
	bool enabled = true;
	size_t sync = kDefaultSync;
	double timeMultiple = 1.0;  ///< already `exp2(Param::Time)`
	float feedback = 0.35f;
	float tone = 0.0f;
	PingPong pingPong = PingPong::Off;
	float spread = 0.0f;
	Mode mode = Mode::Clean;
	double grainSeconds = 0.05; ///< already `exp2(Param::GrainSize)`; 0 means the OFF position
	double densityHz = 20.0;    ///< already `exp2(Param::Density)`
	float spray = 0.25f;
	float pitchSemitones = 0.0f;
	float mix = 0.35f;
	bool dub = false;
	float wow = 0.3f;
	float flutter = 0.25f;
	float saturation = 0.35f;
	float hfLoss = 0.4f;
	int taps = 4;
	/// THE TWO ZONED COORDINATES (backlog MH), raw: `decodeTapCoord` turns each into a family and a position.
	float tapPattern = kTapCoordDefault;
	float tapLevels = kTapCoordDefault;
	float tapWidth = 0.5f;
	/// delay-love S4. Duck 0, Freeze off and Clear low are the device as it was, to the bit.
	float duck = 0.0f;
	double duckReleaseSeconds = 0.25; ///< already `exp2(Param::DuckRelease)`
	bool freeze = false;
	bool clear = false;
	/// BBD and Swarm (2026-11-05). Swarm 0 is today's Granular, to the bit; the BBD four act only in BBD.
	float swarm = 0.0f;
	int bbdStages = 4096;
	float bbdCompander = 0.5f;
	float bbdNoise = 0.2f;
	float bbdDrive = 0.3f;

	friend bool operator==(const Settings&, const Settings&) = default;
};

/// **THERE IS NOTHING IN `State` BUT `Settings`, AND SAYING SO IS THE POINT.** The filter's `State`
/// exists because Keytrack and Poly are declared controls that its DSP does not take — architecture
/// rather than numbers. This device declares nothing of that kind: every control it publishes feeds
/// the sample loop. The wrapper stays because the codec vocabulary (`rowOf`/`stateOf`) is stated in
/// terms of it and a device that dropped it would be the one device shaped differently.
struct State {
	Settings settings {};

	friend bool operator==(const State&, const State&) = default;
};

/// The controls as an indexed row, aligned with `Param` and therefore with `kParamDescs`.
using Row = std::array<float, static_cast<size_t>(Param::kCount)>;

/// THE FIELD ORDER LIVES HERE AND NOWHERE ELSE.
Row rowOf(const State& state);

/// `fieldsPresent` is how many of the row's entries the caller actually has — a payload written by an
/// older build is shorter, and the fields it omits keep their declared defaults. Every value is
/// clamped and rounded by its own descriptor on the way in, because a file is editable by hand and
/// outlives the build that wrote it.
State stateOf(const Row& row, size_t fieldsPresent);

/// One control's value, in the units its descriptor declares.
float valueOf(const State& state, Param param);

/// Set one control, clamped and rounded by its own declaration.
void setValueOf(State* state, Param param, float value);

/// **HOW LONG ONE TAP IS, IN SECONDS — the one place the sync list, the multiplier and the host's
/// tempo meet.** Shared by the sample loop and by anything that wants to report the number, so a
/// reader cannot be told a different figure from the one it hears.
///
/// **A SYNCED DIVISION IS N BEATS AND IS DERIVED FROM `secondsPerBeat`, NOT FROM A POSITION.**
/// device.h states the rule and `automod` follows it: a beat-measured period stays on the grid and
/// survives tempo automation. A free time is a number of seconds and has nothing to do with tempo, so
/// it ignores the clock entirely — not a compromise between the two, each reads the timeline its own
/// period is measured in.
///
/// **WITH NO TEMPO A SYNCED CHOICE FALLS BACK TO ONE SECOND PER BEAT (60 BPM).** A host that reports
/// no tempo is not a host that wants silence, and `haveTempo()` is independent of `playing` — a
/// stopped host still reports its tempo, so this fallback is rarer than it looks.
[[nodiscard]] double tapSeconds(const Settings& s, const device::TransportClock& clock,
                                float notePitch = kDefaultNotePitch);

/// **WHAT `Sync` SELECTS, BEFORE `Time` MULTIPLIES IT** — the free time, or the division at the clock's
/// tempo, unclamped. `tapSeconds` is this times the multiplier, clamped; the taps strip reads it alone so
/// its axis can follow the hand's Time rather than the modulated one (`delay_taps.h`).
[[nodiscard]] double syncBaseSeconds(const Settings& s, const device::TransportClock& clock,
                                     float notePitch = kDefaultNotePitch);

extern const device::AudioOps kAudioOps;
/// **AND A CONTAINER: THE FEEDBACK FX LANE** — one `rack::InnerChain` inside the
/// loop, built LAZILY (by design). Like SKLITTER and Resonate, the holder lives above the rack (`delay_ops.cpp`).
extern const device::ContainerOps kContainerOps;

/// The Descriptor: everything the host needs to know about the delay.
inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	.outputs = {},
	.audio = &kAudioOps,
	.container = &kContainerOps,
	.category = device::Category::TimeSpace,
};

} // namespace feathers::fx::delay
