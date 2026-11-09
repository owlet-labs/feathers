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

#include "device/transport_lock.h"
#include "fx/phaser/phaser_types.h"

#include <array>
#include <complex>

namespace feathers::fx::phaser {

/// **THE PHASER/FLANGER'S RUNNING STATE AND ITS SAMPLE LOOP.**
///
/// THE PHASER: up to twelve first-order TPT allpasses (`stageResponseOfG` has the algebra) in cascade, the
/// dry summed back by Mix, and a one-sample feedback loop around the cascade:
///
///     u[n]   = x[n] + fb · sat(damp(wet[n − 1]))
///     wet[n] = cascade(u[n])
///     out[n] = x[n] + mix · (wet[n] − x[n])
///
/// so with Damping open and Drive at 0, wet = M·x / (1 − fb·z⁻¹·M) for the cascade's response M.
///
/// THE FLANGER: a sinc-interpolated line read at `flangerDelaySamples`, the same loop around it
/// (`line.write(x + fb · sat(damp(wet)))`), and the same Mix. **THROUGH-ZERO** delays the dry by a fixed
/// `kTzDryMs` and sweeps the wet around it (`tzWetSamples`), INVERTED — tape TZF wires the second deck out of
/// phase — so at the crossing the two cancel completely:
///
///     out = dry_d + mix · (−wet − dry_d)
///
/// **THE WET IS READ THROUGH A B = 1 KERNEL** (`kFlangerBandwidth`), which is an exact delta at every
/// whole-sample delay, and **THE DRY IS A PLAIN RING BUFFER**: at the crossing the wet reads at the dry's
/// whole-sample delay, so the two are the same samples and cancel exactly, and the dry — on, and switched off —
/// is the input itself, late, with nothing done to its tone. (The comb's B = 0.455 kernel is a lowpass; read
/// through it the dry would have lost its top octave.)
///
/// A MODE SWITCH CLEARS the engine being entered, so nothing it held when it was left replays.
///
/// **THE FLANGER IS LATE BY THROUGH-ZERO'S DRY DELAY, THROUGH-ZERO ON OR OFF** (`latencySamplesFor`): the plain
/// flanger's whole output goes through `FlangerChannel::late`, so flipping Through Zero never moves the latency and
/// never costs a host restart. **SWITCHED OFF IN THE FLANGER IT STILL DELAYS THE DRY**: the reported latency is the
/// stored row's (Mode, never Enabled — the EQ's arrangement), so the off road must be exactly that late.
///
/// **A NON-FINITE INPUT IS READ AS ZERO** (eq.cpp's guard), and a loop found non-finite at the end of a block
/// is reset — the LOOP only (`resetLoop`): the sources, the LFO's phase, the follower and the readings are not
/// where a non-finite sample lives, and survive it.
///
/// **THE SWEEP, PER CHANNEL** (`sweepOf`): the glided Position, plus LfoDepth · lfoShape(φ_ch), plus
/// EnvAmount · env — the two source terms added AFTER Position's glide, never through it, each with its own
/// short glide on its amount. The left reads φ, the right φ + Stereo/360, so each channel has its own corners
/// and its own delay.
///
/// THE SOURCES RUN AT CONTROL RATE: the LFO's phase and the follower step once a tick. The phaser's corners are
/// ramped across the tick like every other coefficient. **THE FLANGER'S DELAY IS NOT**: it is evaluated PER
/// SAMPLE from a rotating phasor (one complex multiply a sample a channel) through the same delay map, because
/// a straight line through the ticks' delays MEASURED a zipper at ±3 kHz (the tick rate) of −25.6 dB re a 1 kHz
/// carrier and −24.1 dB re 10 kHz (20 Hz LFO, Depth 1, Delay 10 ms — `test_fx_phaser`'s zipper case, against a
/// per-sample reference through the same line). The delay's curvature over 16 samples is up to ~0.4 samples
/// there, which a comb hears; a corner's is not (the allpass coefficient is smooth in it).
///
///   - **THE LFO.** Free, it advances at Rate. Synced, a cycle is the division's beats at the host's tempo —
///     and with NO tempo, at one second a beat (60 BPM), the delay's fallback (`delay.cpp`'s `tapSeconds`): a
///     number a hand recognises as "no tempo", rather than a stopped sweep. **ON A PLAYING TRANSPORT IT ALWAYS
///     FOLLOWS THE SONG** — the decision for automod, 2026-09-09, that Free always locks, and automod's
///     two roads exactly (`automod.cpp`'s `songPeriod`): a synced division with a tempo on the BEATS timeline,
///     everything else (Free, or a division with no tempo, at the rate it is actually running) on the SECONDS
///     timeline with a window of one cycle. FOLLOWED, not divided (`device::TransportLock`), so a modulated Rate
///     frequency-modulates the phase instead of re-deriving it from the song every tick.
///   - **AT A JUMP (play, seek, loop) THE PHASE STEPS to the song's** — automod's precedent: `TransportLock` has
///     no slew, and automod hands its re-anchored turns straight to `lockPhaseTo`. Here the step lands on the
///     next tick's corners (ramped over 16 samples) and on the flanger's per-sample delay at once.
///   - **THE FOLLOWER** reads the INPUT, never the output — a follower inside the loop it steers would be a
///     second feedback path. Per sample it only keeps the peak of max(|L|, |R|); at each tick's END that peak,
///     times Sens, steps a one-pole whose time constant is Attack when it rises and Release when it falls. Its
///     reading is that level in dB, mapped [−48, 0] dBFS → 0..1 (`envelopeOf`). It runs with Env off too, so
///     switching it on finds it current and the card can meter it.
///
/// THE CONTROL GRID: every `kControlTick` samples the smoothed controls step once (one-pole, `kSmoothSeconds`)
/// and each stage's TPT gain G = g/(1+g), the feedback, the drive, the mix — and the flanger's sweep base, LFO
/// amount and log delay, which its per-sample delay reads — are ramped LINEARLY across the next tick from where
/// the last one ended. The grid is carried across calls, so the output
/// does not depend on how the host cuts its blocks.
///
/// **THE TICK IS BUILT AROUND VOICES × CHANNELS × STAGES** (`VoiceTick`, `cascades_`): each voice, each channel,
/// has its own sweep, its own window and its own stage corners (`stageCornerHz`, where the explore layer's
/// per-stage spread enters). Sweep = Normal is voice 0 alone, with the window (1, 0, 0): the other
/// voices are not computed and not run, so Normal is the engine as it was before the barberpole, bit for bit.
///
/// **THE EXPLORE LAYER** (`phaser_phase_space.h` has the index table, mode by mode): thirty-four offsets derived from the
/// pad's stored travel once a tick (`phase_space::exploreOf`) and glided like the knobs, each EXACTLY ZERO AT HOME —
/// so with no travel every road below adds +0.0 and the device is the plain phaser/flanger, bit for bit. They reach:
/// each stage's corner (spread) and sweep (LFO phase offset — on φ in Normal, on the sweep VALUE under the barberpole,
/// never on ψ: `stageSweepsOf`), the LFO's shape (`lfoShape(phase, bend)`, evaluated per sample by the flanger when
/// bent), the damping lowpass's corner (an open loop stays open), the saturator's bias (`saturate(v, drive, bias)`,
/// non-expansive, its own DC removed in the algebra so tails still reach exact zero, and the DC it rectifies out of a
/// loud loop taken out by a 10 Hz blocker that runs only while the bias is nonzero), and the right channel's centre. The
/// flanger reads the per-stage groups per TAP: Normal's L and R take entries 0/12 and 1/13, the barberpole's tap k
/// takes k and 12 + k. **ROTATE NEVER REACHES ANY OF IT**: it turns the plane the pad's next gesture moves in.
/// ROUND 2 added four (thirty-four in all): R's LFO rate against L's (its own clock and song lock, `Clock`), a feedback
/// tilt (a feedback ramp a loop, `loopFb_`), the phaser's notch pairing (`stageSpreadOf`) and the sweep curve
/// (`sweepOf`'s exponent, per sample in the flanger too). And each group has a PILL in front of its fields (`pillsOf`,
/// the house `fieldWithOverride`): at 0.5 (or off) zero travel stays exactly zero; 0 and 1 force the field anywhere.
///
/// **THE BARBERPOLE** (Sweep = Up | Down; `kBarberVoices`, `barberSweepOf`, `barberGain` in the types header). ψ is
/// its own phase ρ (`barberPhaseFor`), turning at the LFO's rate scaled by Depth (`barberCyclesPerSecond`), so Rate
/// sets the climb in octaves a second and Depth only the span :
///   - THE PHASER runs three whole cascades a channel, each with its own feedback loop (a `Channel` is one
///     cascade's whole state), and sums their wets by their windows, ramped per sample across the tick like every
///     other coefficient.
///   - THE FLANGER reads its ONE line at three taps a channel, sums them by their windows BEFORE the loop, and
///     feeds the sum back — so `loopOf` and the guard see one wet. Each tap's delay is evaluated PER SAMPLE from the
///     per-sample ramp phase (linear in ψ, so no phasor is needed for the sweep), and the windows from one rotating
///     phasor a channel (`barberGainOfCos`). Under through-zero each tap's RELATIVE delay ramps (2ψ_k − 1)·Delay,
///     through zero, endlessly (decided 2026-10-03).
///   - ENTERING it, voices 1 and 2 JOIN EMPTY — out of the signal since they last left, as a stage joining the
///     cascade is — and their corners are placed, not ramped. LEAVING it, the three windows ramp to (1, 0, 0)
///     across one tick and then voice 0 runs alone. Up ↔ Down re-aims every voice (ψ jumps from ρ to 1 − ρ) and
///     clears nothing: all three were in the signal. The flanger has one line and nothing stale to clear, but its
///     read MOVES at a Sweep switch, and a stepped read recirculates through the loop — so the outgoing read is
///     frozen and crossfaded into the incoming one over four ticks (`kSweepFadeSamples`), both on the same line.
///     The phaser's switch fades too (the windows' ramps), so the two engines answer one action alike.
///   - WITH NO LFO AMOUNT (off, or Depth 0) every voice sits on Position and the windows sum to one: the plain
///     engine at three times its cost — UNTIL THE EXPLORE LAYER MAKES THE VOICES DIFFER (the Fb tilt's per-voice loops, the
///     flanger's per-tap spread), when the turning windows are heard: so Sweep has no gate. ψ keeps turning, so the LFO
///     coming back on finds the pole moving.
///
/// **KEY TRACKING** (Key Track on, and a held note since the reset): the note sets the centre the sweep moves around,
/// and Center or Delay becomes the OFFSET from it — its distance from its default, in semitones (`trackedHzOf`), so
/// the default lands on the note (retiring Key Offset) — the phaser's placed so its FIRST NOTCH is exactly on
/// the note in the digital domain (`keyCornerLog2Hz` says why not the analog prototype), the flanger's one period
/// long so its comb is tuned to the note (`keyDelayLog2s`; under through-zero that period is the span the relative
/// delay reaches at the sweep's extremes). The note is LATCHED while a key is held, so the centre holds the last note
/// after the release; it goes through the centre's own glide (5 ms for a note, `kKeySmoothSeconds`), so a new note, or
/// a glide the host folds into the pitch, slides it. With no note yet the knobs rule, as if Key Track were off.
/// **FROZEN AT THE RELEASE, UNLIKE THE DISPERSER** (which follows the pitch after the release once it has heard a
/// note): by spec, so a per-voice pitch bend during a release tail is ignored — a decision, recorded at
/// `setNoteContext`.
class Phaser {
public:
	/// Activate-time only. Nothing allocates: every state is a fixed member.
	void prepare(double sampleRate) noexcept;
	/// AS IF JUST SWITCHED ON: every state zero, the smoothers to snap onto the next target.
	void reset() noexcept;

	void setTarget(const Settings& settings) noexcept { target_ = settings; }
	/// THE NOTE, for key tracking — latched while a key is held (see the class comment).
	void setNoteContext(const device::NoteContext& note) noexcept;
	[[nodiscard]] const Settings& target() const noexcept { return target_; }

	void processStereo(float* left, float* right, int frames) noexcept;
	/// DECLARED: one channel, the left channel's road.
	void processMono(float* mono, int frames) noexcept;

	/// The host's clock for the NEXT process call — HELD, read at each tick (delay's and automod's pattern).
	/// Its song position is the block's first sample's.
	void setTransportClock(const device::TransportClock& clock) noexcept { clock_ = clock; }
	/// Where the readings go, once per process call; a null `publish` publishes nothing.
	void setReadingsSink(device::ReadingsSink sink) noexcept { sink_ = sink; }

	/// How fast the continuous controls glide to a new value (a one-pole time constant).
	static constexpr double kSmoothSeconds = 0.02;
	/// **POSITION GLIDES FASTER**: it is THE external-modulation target, and a rack modulator arrives already
	/// smoothed upstream, so a long glide here would only lag it. (Task 4's LFO and envelope terms are added to
	/// the GLIDED Position, after this, not through it — an LFO run through a 5 ms one-pole would lose its top.)
	static constexpr double kPositionSmoothSeconds = 0.005;
	/// THE SOURCES' AMOUNTS GLIDE FASTER STILL: LFO Depth and Env Amount (each 0 while its source is off, so a
	/// power switch fades its term in and out rather than stepping the sweep). 3 ms so that from any amount (|a| ≤ 1)
	/// the glide LANDS on zero within 50 ms: e^(−50/3) = 5.8e-8, under `kGlideSnap`.
	static constexpr double kSourceSmoothSeconds = 0.003;
	/// **A TRACKED NOTE GLIDES THE CENTRE IN 5 ms, NOT THE KNOBS' 20.** A note is an event the player meant to land
	/// on: at 20 ms a trill on a resonant flanger never reaches either note and smears into a pitched swoop. A knob
	/// move keeps `kSmoothSeconds` — the centre's glide reads this constant only while Key Track has a note.
	static constexpr double kKeySmoothSeconds = 0.005;
	/// WHICH TIMELINE A SONG LOCK FOLLOWS — `TransportLock::follow`'s `unit`, automod's two roads by name.
	static constexpr int kBeatsRoad = 1;
	static constexpr int kSecondsRoad = 2;
	/// A glide this close to its target lands on it: a one-pole only approaches, and "open" damping or a
	/// settled coefficient must be reachable exactly.
	static constexpr double kGlideSnap = 1.0e-7;

	//----------------------------------------------------------------------------------------------//
	// TEST SEAMS — a claim about a value cannot be inferred from a rendered buffer.                  //
	//----------------------------------------------------------------------------------------------//

	[[nodiscard]] double sampleRateForTest() const noexcept { return sampleRate_; }
	/// The TPT gain G = g/(1+g) stage `stage` of `channel` ran its last sample with.
	[[nodiscard]] float stageCoefficientForTest(int channel, int stage) const noexcept {
		return voiceStageCoefficientForTest(0, channel, stage);
	}
	/// The same, of voice `voice`'s cascade.
	[[nodiscard]] float voiceStageCoefficientForTest(int voice, int channel, int stage) const noexcept {
		return cascades_[voiceIndex(voice)][static_cast<size_t>(channel & 1)].G[static_cast<size_t>(stage) % kMaxStages].value;
	}
	/// The same stage's prewarped corner g = tan(π·fc/sr), recovered from G as G/(1 − G).
	[[nodiscard]] float stageGForTest(int channel, int stage) const noexcept {
		const float G = stageCoefficientForTest(channel, stage);
		return G / (1.0f - G);
	}
	/// The feedback the loop ran its last sample with — after the ceiling.
	[[nodiscard]] float feedbackForTest() const noexcept { return loopFb_[0][0].value; }
	/// THE FEEDBACK voice `voice`'s loop on `channel` ran its last sample with — after the tilt (index 31) and the
	/// ceiling. The flanger's one loop a channel is voice 0's.
	[[nodiscard]] float loopFeedbackForTest(int voice, int channel) const noexcept {
		return loopFb_[voiceIndex(voice)][static_cast<size_t>(channel & 1)].value;
	}
	/// THE LFO's rate on `channel` at the last tick, cycles a second — R's is L's · 2^(rate skew).
	[[nodiscard]] double lfoRateHzForTest(int channel) const noexcept {
		return lfoClock_[static_cast<size_t>(channel & 1)].inc * sampleRate_;
	}
	/// Whether R's LFO clock (`which` 0) or barberpole clock (1) is L's again, copied, with no offset.
	[[nodiscard]] bool rConvergedForTest(int which) const noexcept { return convergedR_[static_cast<size_t>(which & 1)]; }
	/// ψ's rate on `channel` at the last tick, cycles a second.
	[[nodiscard]] double barberRateHzForTest(int channel) const noexcept {
		return barberClock_[static_cast<size_t>(channel & 1)].inc * sampleRate_;
	}
	/// Whether the damping lowpass is out of the loop this tick.
	[[nodiscard]] bool dampingOpenForTest() const noexcept { return dampOpen_; }
	/// The damping lowpass's corner this tick, in Hz (meaningless while open).
	[[nodiscard]] double dampingHzForTest() const noexcept { return dampHz_; }
	/// The glided damping control, log2(Hz).
	[[nodiscard]] double dampingLog2ForTest() const noexcept { return dampLog2_; }
	/// The glided Position.
	[[nodiscard]] double positionForTest() const noexcept { return position_; }
	/// The sweep point `channel` computed at the last tick — what its corner and delay are ramping toward. Under the
	/// barberpole, the voices' window-weighted centre Σ w_k·s_k (each voice's own is `voiceSweepForTest`).
	[[nodiscard]] double sweepForTest(int channel) const noexcept { return sweep_[static_cast<size_t>(channel & 1)]; }
	/// The LFO phase (0..1) the LEFT channel used at the last tick; the right used it + Stereo/360.
	[[nodiscard]] double lfoPhaseForTest() const noexcept { return lfoPhaseUsed_[0]; }
	/// The phase `channel` used at the last tick.
	[[nodiscard]] double lfoPhaseForTest(int channel) const noexcept {
		return lfoPhaseUsed_[static_cast<size_t>(channel & 1)];
	}
	/// The follower's level, a linear peak, after the last tick that ended.
	[[nodiscard]] double envLevelForTest() const noexcept { return envLevel_; }
	/// The follower's 0..1 reading of that level (`envelopeOf`).
	[[nodiscard]] double envForTest() const noexcept { return envelopeOf(envLevel_); }
	/// The 0..1 follower reading the LAST TICK'S SWEEP used — one tick behind `envForTest` whenever a tick has
	/// ended since (the follower steps at a tick's end, the sweep reads it at the next tick's start).
	[[nodiscard]] double envUsedForTest() const noexcept { return envUsed_; }
	/// The phaser's corner `channel` computed at the last tick, Hz. Under the barberpole, the voices' corners averaged
	/// in log by their windows, 2^(Σ w_k·log2 fc_k).
	[[nodiscard]] double cornerHzForTest(int channel) const noexcept {
		return cornerHz_[static_cast<size_t>(channel & 1)];
	}
	/// The flanger's wet read at the last tick's phase, samples — the per-sample delay passes through it on the tick's
	/// last sample. Under the barberpole, the taps' reads weighted by their windows.
	[[nodiscard]] double delayTargetSamplesForTest(int channel) const noexcept {
		return delayTarget_[static_cast<size_t>(channel & 1)];
	}
	/// The cascade's live stage count.
	[[nodiscard]] int stagesForTest() const noexcept { return activeStages_; }
	/// **WHETHER THE FLANGER'S LOOP STILL HOLDS SOUND** — `AudioOps::holdsTail` (backlog MC). Its wet is a sinc read up
	/// to 20 ms behind the write, so between passes of a click round the loop the output is exactly zero for longer
	/// than a small block. Only the wet line: the dry and late rings are the latency the host was told. Off, the
	/// loop is frozen and cleared on the way back on; in the Phaser the line is not read, and entering the Flanger
	/// clears it.
	[[nodiscard]] bool holdsTail() const noexcept {
		return target_.enabled && wasEnabled_ && mode_ == Mode::Flanger
		       && (flangers_[0].wet.holdsSound() || flangers_[1].wet.holdsSound());
	}
	/// The flanger's wet read delay, in samples, that `channel` ran its last sample with (under the barberpole, its
	/// taps' window-weighted delay — `tapDelaySamplesForTest` has each).
	[[nodiscard]] float delaySamplesForTest(int channel) const noexcept {
		return flangers_[static_cast<size_t>(channel & 1)].delay;
	}
	/// The flanger's dry delay in samples: through-zero's fixed one, or 0 without it.
	[[nodiscard]] int dryDelaySamplesForTest() const noexcept { return throughZero_ ? tzDry_ : 0; }
	/// THE PLAIN FLANGER AS IT WAS BEFORE IT REPORTED A LATENCY — its output straight out, not through
	/// `FlangerChannel::late`. Only so a case can prove the latency is a pure delay of exactly that output.
	void setPlainUndelayedForTest(bool on) noexcept { plainUndelayedForTest_ = on; }
	/// Whether a held note has arrived since the reset.
	[[nodiscard]] bool heardNoteForTest() const noexcept { return heardNote_; }
	/// The note it latched, in semitones (A4 = 69) — what KeyTrack centres on. TEST ONLY.
	[[nodiscard]] double noteSemitonesForTest() const noexcept { return noteSemitones_; }
	/// The frequency key tracking tuned the last tick to, Hz (the `NoteHz` reading): 0 while not tracking.
	[[nodiscard]] double noteHzForTest() const noexcept { return noteHz_; }
	/// The glided centre the last tick swept around: log2(Hz) for the phaser, log2(s) for the flanger.
	[[nodiscard]] double centerLog2ForTest() const noexcept { return centerLog2_; }
	[[nodiscard]] double delayLog2ForTest() const noexcept { return delayLog2_; }
	/// Times the end-of-block guard found the loop non-finite and reset it.
	[[nodiscard]] int loopResetsForTest() const noexcept { return loopResets_; }

	// THE EXPLORE LAYER'S SEAMS (`phaser_phase_space.h`). Every value is the GLIDED one the last tick used.

	/// Everything the layer derived, glided: per-stage spread and LFO offsets, the bend, colour, bias, skew.
	[[nodiscard]] const phase_space::Explore& exploreForTest() const noexcept { return explore_; }
	/// The saturator's bias the loop ran its last sample with (index 28; 0 at home, or with Drive at 0 ignored).
	[[nodiscard]] float driveBiasForTest() const noexcept { return driveBias_; }
	/// Stage `stage`'s own sweep on `channel`, voice 0 (Normal) — what its corner was computed from.
	[[nodiscard]] double stageSweepForTest(int channel, int stage) const noexcept {
		return voiceStageSweepForTest(0, channel, stage);
	}
	[[nodiscard]] double voiceStageSweepForTest(int voice, int channel, int stage) const noexcept {
		return tickOf(voice, channel).stageS[static_cast<size_t>(stage) % kMaxStages];
	}
	/// The flanger's per-tap explore offsets on `channel` as the last sample ran them: the delay's log2 offset (the
	/// tap's spread, plus the stereo skew on R) and the LFO phase offset, cycles. Tap 0 is the Normal read.
	[[nodiscard]] float tapLog2OffsetForTest(int voice, int channel) const noexcept {
		return tapLog2Ramp_[static_cast<size_t>(channel & 1)][voiceIndex(voice)].value;
	}
	[[nodiscard]] float tapPhaseOffsetForTest(int voice, int channel) const noexcept {
		return tapPhaseRamp_[static_cast<size_t>(channel & 1)][voiceIndex(voice)].value;
	}

	// THE BARBERPOLE'S SEAMS. "At the last tick" values are what the tick computed (the ramps' targets).

	/// How many voices the last tick ran: 1 under Sweep = Normal, 3 under the barberpole and on the tick leaving it.
	[[nodiscard]] int voicesRunningForTest() const noexcept { return voicesRunning_; }
	/// ψ for `channel` at the last tick (the right's is Stereo/360 on); 0 with Sweep = Normal.
	[[nodiscard]] double barberPhaseForTest(int channel) const noexcept { return psi_[static_cast<size_t>(channel & 1)]; }
	/// ψ's rate at the last tick, cycles a second (`barberCyclesPerSecond`) — computed in Normal too, where ρ turns
	/// unheard.
	[[nodiscard]] double barberRateHzForTest() const noexcept { return barberRateHzForTest(0); }
	/// THE EFFECTIVE CENTRE at the last tick, Σ w_k·s_k — the sweep the readings report. Stationary under the
	/// barberpole: the voices move, it does not (`test_fx_phaser`'s monotonic case says why).
	[[nodiscard]] double barberSweepForTest(int channel) const noexcept { return sweepForTest(channel); }
	/// Voice `voice`'s ramp phase ψ_k, sweep s_k, window w_k, corner and flanger delay, at the last tick.
	[[nodiscard]] double voicePhaseForTest(int voice, int channel) const noexcept { return tickOf(voice, channel).psi; }
	[[nodiscard]] double voiceSweepForTest(int voice, int channel) const noexcept { return tickOf(voice, channel).s; }
	[[nodiscard]] double voiceGainForTest(int voice, int channel) const noexcept { return tickOf(voice, channel).w; }
	[[nodiscard]] double voiceCornerHzForTest(int voice, int channel) const noexcept {
		return tickOf(voice, channel).cornerHz;
	}
	[[nodiscard]] double voiceDelayTargetSamplesForTest(int voice, int channel) const noexcept {
		return tickOf(voice, channel).delay;
	}
	/// The PHASER's window ramp for `voice`, as its cascade's wet was weighted on the last sample.
	[[nodiscard]] float voiceGainRampForTest(int voice, int channel) const noexcept {
		return gain_[voiceIndex(voice)][static_cast<size_t>(channel & 1)].value;
	}
	/// Voice `voice`'s stage `stage`, its prewarped corner g = G/(1 − G).
	[[nodiscard]] float voiceStageGForTest(int voice, int channel, int stage) const noexcept {
		const float G = voiceStageCoefficientForTest(voice, channel, stage);
		return G / (1.0f - G);
	}
	/// Σ|state| over voice `voice`'s cascade on `channel` — its integrators, its loop sample, its lowpass.
	[[nodiscard]] double voiceStateMagnitudeForTest(int voice, int channel) const noexcept;
	/// THE FLANGER'S PER-SAMPLE RAMP PHASE for `channel`, as the last sample ran it.
	[[nodiscard]] double barberRunForTest(int channel) const noexcept {
		return barberRun_[static_cast<size_t>(channel & 1)];
	}
	/// The flanger's tap `voice`: its read delay in samples, and its window, as the last sample ran them.
	[[nodiscard]] float tapDelaySamplesForTest(int voice, int channel) const noexcept {
		return flangers_[static_cast<size_t>(channel & 1)].tapDelay[voiceIndex(voice)];
	}
	/// Samples of the flanger's sweep-switch crossfade still to run (0 when none is in flight).
	[[nodiscard]] int sweepFadeLeftForTest() const noexcept { return fadeLeft_; }
	[[nodiscard]] float tapGainForTest(int voice, int channel) const noexcept {
		return flangers_[static_cast<size_t>(channel & 1)].tapGain[voiceIndex(voice)];
	}

private:
	[[nodiscard]] static size_t voiceIndex(int voice) noexcept {
		return static_cast<size_t>(voice) % static_cast<size_t>(kBarberVoices);
	}

	/// **ONE VOICE OF ONE CHANNEL, AS THE TICK COMPUTED IT** — the sweep array the tick is built around, computed by
	/// the pure `channelSweepOf` (`phaser_types.h`) that the response model runs too.
	using VoiceTick = VoiceSweep;
	[[nodiscard]] const VoiceTick& tickOf(int voice, int channel) const noexcept {
		return voiceTick_[voiceIndex(voice)][static_cast<size_t>(channel & 1)];
	}

	/// A VALUE RAMPED ACROSS ONE CONTROL TICK. `aim` starts the tick EXACTLY where the last one was aimed,
	/// so rounding in the per-sample steps never accumulates from tick to tick, and a value that has
	/// arrived stays bit-exact.
	struct Ramp {
		float value = 0.0f;
		float step = 0.0f;
		float end = 0.0f;
		void aim(float target, bool snap) noexcept {
			value = snap ? target : end;
			end = target;
			step = snap ? 0.0f : (end - value) / static_cast<float>(kControlTick);
		}
		void advance() noexcept { value += step; }
	};

	/// **ONE CHANNEL'S PHASE ACCUMULATOR AND ITS SONG LOCK** (the LFO's φ, and the barberpole's ρ). Each channel has
	/// its own — round 2's rate skew (index 30) runs R's LFO at L's rate · 2^skew, so R needs a phase of its own and a
	/// lock at ITS period (the Free road's arrangement for a modulated Rate). At skew 0 R's clock is handed exactly L's
	/// arguments every tick, so it holds exactly L's numbers (the same arithmetic on the same inputs), and R's phase is
	/// L's + Stereo/360 as it always was.
	struct Clock {
		double phase = 0.0;            ///< the free accumulator: the phase the NEXT tick uses unless locked
		double inc = 0.0;              ///< cycles a sample this tick
		device::TransportLock lock {};
	};

	struct Channel {
		std::array<float, kMaxStages> s {};  ///< each stage's integrator state
		std::array<Ramp, kMaxStages> G {};   ///< each stage's TPT gain g/(1+g) — ramped, so no divide per sample
		float prevWet = 0.0f;                ///< the loop's one sample of delay
		float damp = 0.0f;                   ///< the damping lowpass's state
		float dc = 0.0f;                     ///< the loop DC blocker's lowpass state (run only with a bias)
	};

	/// THE FLANGER'S CHANNEL: its wet line (fed back), the through-zero dry ring, its own loop lowpass.
	struct FlangerChannel {
		FlangerChannel();
		dsp::SincDelayLine<kLineSize> wet;
		std::array<float, kDryLineSize> dry {};
		int dryPos = 0;     ///< where the next dry sample is written
		float delay = 0.0f; ///< the wet read delay the last sample ran, samples
		float damp = 0.0f;  ///< the damping lowpass's state
		float dc = 0.0f;    ///< the loop DC blocker's lowpass state (run only with a bias)
		float lastWet = 0.0f; ///< the line's last read (the barberpole's: its windowed sum) — the guard's view of the loop
		std::array<float, kBarberVoices> tapDelay {}; ///< the barberpole's three reads, samples, as the last sample ran
		std::array<float, kBarberVoices> tapGain {};  ///< and their windows
		/// THE OUTGOING READ a Sweep switch fades from, frozen at the switch: the plain read's delay, or the taps.
		float fadeDelay = 0.0f;
		std::array<float, kBarberVoices> fadeTapDelay {};
		std::array<float, kBarberVoices> fadeTapGain {};
		/// The dry `d` writes ago (most recent = 1), read before the write as the wet is. 1 ≤ d ≤ kDryLineSize.
		[[nodiscard]] float dryAt(int d) const noexcept {
			return dry[static_cast<size_t>((dryPos - d) & (kDryLineSize - 1))];
		}
		void writeDry(float x) noexcept {
			dry[static_cast<size_t>(dryPos)] = x;
			dryPos = (dryPos + 1) & (kDryLineSize - 1);
		}
		/// **THE PLAIN FLANGER'S OUTPUT, HELD BACK BY THE LATENCY** (`latencySamplesFor`): the Flanger reports
		/// through-zero's dry delay with Through Zero off too, so its plain output is that late. Written on every
		/// road (the plain flanger writes its output, every other road its input), so a switch onto the plain
		/// flanger reads something current. Read before write, `d` writes ago, as `dryAt`.
		std::array<float, kDryLineSize> late {};
		int latePos = 0;
		[[nodiscard]] float lateAt(int d) const noexcept {
			return late[static_cast<size_t>((latePos - d) & (kDryLineSize - 1))];
		}
		void writeLate(float x) noexcept {
			late[static_cast<size_t>(latePos)] = x;
			latePos = (latePos + 1) & (kDryLineSize - 1);
		}
	};
	static_assert((kDryLineSize & (kDryLineSize - 1)) == 0, "the dry ring wraps by mask");

	/// THE CONTROL PATH, once per tick. `offset` is how many samples of this process call came before it.
	void tick(int offset) noexcept;
	/// THE LFO's phase for this tick on the channel whose clock `c` is, running at the LFO's rate times `rateScale`
	/// (1 for L; 2^skew for R): from the song when locked, else the running accumulator.
	[[nodiscard]] double lfoPhaseFor(int offset, Clock& c, double rateScale) noexcept;
	/// THE BARBERPOLE'S PHASE ρ for this tick on clock `c`, by `lfoPhaseFor`'s roads at ψ's own rate
	/// (`barberCyclesPerSecond` of the channel's LFO rate — times `rateScale` — and the glided Depth `depth`): the song
	/// when locked, else the running accumulator.
	[[nodiscard]] double barberPhaseFor(int offset, double depth, Clock& c, double rateScale) noexcept;
	/// The LFO's rate as it is actually running, cycles a second (a division with no tempo at 60 BPM).
	[[nodiscard]] double lfoCyclesPerSecond() const noexcept;
	/// THE FOLLOWER, at a tick's end: the tick's input peak steps the one-pole.
	void followerStep() noexcept;
	void publishReadings() noexcept;
	void render(float* left, float* right, int frames) noexcept;
	/// One sample of the loop's damping, saturation and gain, on `state`.
	/// `damp` is the damping lowpass's state, `dc` the DC blocker's (`kLoopDcCornerHz`, run only with a bias).
	/// `fb` is the loop's own feedback this sample (`loopFb_`: the tilt makes it per loop).
	[[nodiscard]] float loopOf(float wet, float& damp, float& dc, float fb) const noexcept;
	[[nodiscard]] float phaserSample(Channel& c, float x, float fb) noexcept;
	/// One flanger sample, the mix applied: the loop written, the output mixed. `wet` is the line's read and
	/// `dryLate` the through-zero dry, both read by the caller BEFORE this writes.
	[[nodiscard]] float flangerSample(FlangerChannel& f, float x, float dryLate, float mix, float wet, float fb) noexcept;
	/// THE BARBERPOLE'S WET: three taps read at `delays` and summed by `gains` — the live taps, or a fade's frozen ones.
	[[nodiscard]] static float tapsWet(const FlangerChannel& f, const std::array<float, kBarberVoices>& delays,
	                                   const std::array<float, kBarberVoices>& gains) noexcept;
	/// THE STAGES' CORNERS for one voice of one channel: each stage's G aimed from its OWN sweep (`stageS`, the
	/// explore layer's per-stage LFO offsets already in it) and its own spread, around the channel's centre
	/// (`stageCornerHz`).
	void aimStages(Channel& c, const std::array<double, kMaxStages>& stageS, double centreLog2, bool snap) noexcept;
	/// THE FLANGER'S SWEEP-SWITCH CROSSFADE, at a tick: freeze the outgoing read and start the fade.
	void beginSweepCrossfade() noexcept;
	/// THE FLANGER'S BARBERPOLE TAPS for `ch` this sample: the ramp phase stepped, each tap's delay and window.
	void barberTaps(size_t ch, double delayLog2) noexcept;
	/// Voice `voice`'s cascades (both channels) to zero.
	void clearVoice(size_t voice) noexcept;
	/// THE END-OF-BLOCK GUARD: a non-finite loop is reset by `resetLoop`.
	void guardLoop() noexcept;
	/// **THE LOOP ALONE TO ZERO** — the cascades' and lines' states and the damping lowpasses. Not the dry ring
	/// (it only ever holds sanitised input), not the coefficients, the glides, the LFO, the follower or the
	/// readings: none of them is computed from the audio a fault lives in, and resetting them would make a loop
	/// fault also jump the sweep.
	void resetLoop() noexcept;
	/// One channel's flanger sweep this sample, from its rotating phasor and the tick's ramps.
	[[nodiscard]] double flangerSweepSample(size_t ch) noexcept;
	void clearPhaser() noexcept;
	void clearFlanger() noexcept;
	/// `reset`, optionally keeping the through-zero dry ring — the off road keeps it current.
	void resetState(bool keepDry) noexcept;

	Settings target_ {};
	double sampleRate_ = 48000.0;
	double smoothAlpha_ = 1.0;
	double positionAlpha_ = 1.0;
	double keyAlpha_ = 1.0; ///< the tracked centre's glide (`kKeySmoothSeconds`)

	// THE GRID.
	int untilTick_ = 0;
	bool placed_ = false;
	bool wasEnabled_ = true;
	Mode mode_ = Mode::Phaser;
	int activeStages_ = 0;

	// THE SMOOTHED CONTROLS, stepped once a tick.
	double centerLog2_ = 0.0;
	double delayLog2_ = 0.0;
	double position_ = 0.0;
	double feedbackRaw_ = 0.0;
	double driveRaw_ = 0.0;
	double mixRaw_ = 0.0;
	double dampLog2_ = 0.0;
	double rateLog2_ = 0.0;
	double stereo_ = 0.0;      ///< degrees
	double lfoAmount_ = 0.0;   ///< LFO Depth, or 0 with the LFO off
	double envAmount_ = 0.0;   ///< Env Amount, or 0 with the follower off
	double envSensDb_ = 0.0;
	double envAttackLog2_ = 0.0;
	double envReleaseLog2_ = 0.0;
	double sourceAlpha_ = 1.0;

	// KEY TRACKING. The note survives `resetState` (the enable road) and is forgotten by `reset`.
	bool heardNote_ = false;     ///< a held note has arrived since the reset
	double noteSemitones_ = 60.0; ///< the last held note's pitch, A4 = 69
	double noteHz_ = 0.0;        ///< what the last tick tracked, Hz (0 when it did not)

	// THE LFO.
	std::array<Clock, 2> lfoClock_ {};
	std::array<double, 2> lfoPhaseUsed_ {}; ///< what each channel used at the last tick
	/// THE FLANGER'S PER-SAMPLE LFO: each channel's e^(j2πφ), rotated by its `lfoStep_` before each sample and
	/// re-seeded from the exact phase at every tick, so it cannot drift. (The barberpole's is `barberRot_`.)
	std::array<std::complex<double>, 2> lfoRot_ {};
	std::array<std::complex<double>, 2> lfoStep_ {};
	device::TransportClock clock_ {};
	/// **THE BARBERPOLE'S OWN PHASE ρ, A CHANNEL** (ψ_L is ρ_L, or 1 − ρ_L for Down; ψ_R is ρ_R's + Stereo/360): turned
	/// at `barberCyclesPerSecond` — the channel's LFO rate scaled by Depth so the climb's speed is Depth's to leave
	/// alone — and song-locked by its own lock at that scaled period. Advanced every tick, Normal included, so a switch
	/// into the barberpole finds it moving; it reaches nothing in Normal.
	std::array<Clock, 2> barberClock_ {};
	/// R's RATE AGAINST L's this tick, 2^(the glided rate skew) — exactly 1 while the derived rate skew is 0 (zero
	/// travel with the Rate skew pill at 50% or off). A pill FORCED off 50% keeps R skewed at home, by design (the
	/// 2026-10-04 decision: the pills follow `fieldWithOverride`, so 0 and 1 pin the field wherever the puck is).
	double rateScaleR_ = 1.0;
	/// **R RE-CONVERGES ON L WHENEVER THE DERIVED RATE SKEW IS 0** (the Task 11 decision: the textbook sound
	/// survives a played excursion). A skewed R has drifted from L by the time the skew returns to 0, and a clock at L's
	/// rate would keep that drift for ever (Stereo 0 still moving). So R's phase carries an OFFSET, frozen while the skew
	/// is live (R stays continuous), and glided while it is 0 toward the value that puts R on L (`kReconvergeSeconds`); once
	/// within `kGlideSnap` of it, L's clock — phase, rate and song lock — is copied into R's and the offset is 0, so the
	/// two are bit-identical again. One per clock pair: the LFO's φ and the barberpole's ρ.
	static constexpr double kReconvergeSeconds = 0.2;
	double reconvergeAlpha_ = 1.0;
	std::array<double, 2> offsetR_ {};          ///< [0] the LFO's, [1] the barberpole's, cycles
	std::array<bool, 2> convergedR_ { true, true }; ///< R's clock IS L's (copied), and its offset 0
	/// THE PHASE R USES this tick from its clock's `rawR`, L's `rawL`, and the pair `which` — see `offsetR_`.
	[[nodiscard]] double phaseRFor(size_t which, double rawR, double rawL) noexcept;

	// THE FOLLOWER.
	float envPeak_ = 0.0f;   ///< the running tick's input peak, max(|L|, |R|)
	double envLevel_ = 0.0;  ///< the one-pole's level, linear
	double envGain_ = 1.0;   ///< Sens, as a gain, this tick
	double envAttackA_ = 1.0;
	double envReleaseA_ = 1.0;
	double envUsed_ = 0.0;   ///< the 0..1 reading the last tick's sweep used

	// WHAT EACH CHANNEL'S SWEEP MADE, this tick.
	std::array<double, 2> sweep_ {};
	std::array<double, 2> cornerHz_ {};
	std::array<double, 2> delayTarget_ {};

	device::ReadingsSink sink_ {};

	// THE PER-SAMPLE RAMPS shared by both channels.
	/// **THE FEEDBACK, ONE RAMP A LOOP** — [voice][channel], because round 2's feedback tilt (index 31) gives each loop
	/// its own: the channels split it in the phaser's Normal and in the flanger (whose loop is voice 0's slot), the
	/// phaser's barberpole voices step it (`feedbackTiltOf`). At home every ramp is aimed at the one value the shared
	/// ramp used to be, so each holds exactly its numbers.
	std::array<std::array<Ramp, 2>, kBarberVoices> loopFb_ {};
	Ramp drive_ {};
	Ramp mix_ {};
	// THE FLANGER'S PER-SAMPLE SWEEP reads these: each channel's Position + env term, the LFO's amount, log2 Delay.
	std::array<Ramp, 2> sweepBase_ {};
	Ramp lfoAmountRamp_ {};
	Ramp delayLog2Ramp_ {};

	// THE DAMPING LOWPASS, per tick: its TPT gain G = g/(1+g), and whether it is in the loop at all.
	bool dampOpen_ = true;
	float dampG_ = 1.0f;
	double dampHz_ = 0.0;

	// THE EXPLORE LAYER, glided once a tick toward `phase_space::exploreOf(target_.travel, pillsOf(target_))` — every
	// member exactly 0 at home with the pills at 0.5, so the engine below is the plain one bit for bit.
	phase_space::Explore explore_ {};
	phase_space::Explore exploreWant_ {}; ///< `fieldsSeen_` through `pillsSeen_` — what the glides aim at
	phase_space::Fields fieldsSeen_ = phase_space::kHomeField; ///< the fields of `travelSeen_`, before the pills
	phase_space::Travel travelSeen_ {};   ///< the travel the fields were last read from
	/// THE PILLS the want was last derived with (`pillsOf`): a pill's move — or its modulation — re-derives from the
	/// fields it already has; only a travel change reads the triangles again.
	phase_space::Pills pillsSeen_ {};
	/// THE SWEEP CURVE'S EXPONENT, 2^c of the glided c (index 33), for the flanger's per-sample sweeps — 1 at home.
	double curveExp_ = 1.0;
	bool exploreSettled_ = false;         ///< the glides have landed on `exploreWant_`
	bool exploreAtHome_ = false;          ///< ... and every value is exactly 0: the tick skips the layer's work
	float driveBias_ = 0.0f; ///< `explore_.driveBias` as the loop reads it
	/// tanh of the saturator's effective bias, once a tick from the drive ramp's END (`biasTanhOf`): 0 exactly with no
	/// bias, so the loop is `saturate(v, drive)` then.
	float biasTanh_ = 0.0f;
	/// THE LOOP DC BLOCKER: engaged while the bias is nonzero (this tick), and its TPT lowpass gain at 10 Hz.
	bool dcOn_ = false;
	float dcG_ = 0.0f;
	double squareNorm_ = 1.0; ///< `squareNormOf(explore_.bend)`, for the flanger's per-sample shape
	/// THE FLANGER'S PER-TAP OFFSETS, ramped per sample like its other per-sample reads: [channel][tap] the delay's
	/// log2 offset and the LFO phase offset (Normal reads tap 0 on each channel — see the index table).
	std::array<std::array<Ramp, kBarberVoices>, 2> tapLog2Ramp_ {};
	std::array<std::array<Ramp, kBarberVoices>, 2> tapPhaseRamp_ {};
	/// THE FLANGER'S PER-SAMPLE LFO PHASE, re-seeded from the tick's φ — read only when the layer bends the shape or
	/// offsets a phase (`flangerRun_`); at home the phasor `lfoRot_` is the road, as it always was.
	std::array<double, 2> lfoRun_ {};
	bool flangerOffsets_ = false; ///< some tap offset (delay or phase) is nonzero this tick
	bool flangerRun_ = false;     ///< the per-sample LFO phase is needed this tick

	// THE VOICES × CHANNELS: each its own cascade (loop included), its own window, its own tick of sweep.
	std::array<std::array<Channel, 2>, kBarberVoices> cascades_ {};
	std::array<std::array<Ramp, 2>, kBarberVoices> gain_ {};
	std::array<std::array<VoiceTick, 2>, kBarberVoices> voiceTick_ {};
	int voicesRunning_ = 1;     ///< the voices this tick runs: 1, or 3 under the barberpole and on the tick leaving it
	bool barber_ = false;       ///< the barberpole is on this tick
	bool barberPrev_ = false;   ///< it was on the tick before
	std::array<double, 2> psi_ {};       ///< each channel's ramp phase this tick (0 with Sweep = Normal)
	std::array<double, 2> barberRun_ {}; ///< THE FLANGER'S PER-SAMPLE ramp phase, re-seeded from ψ each tick
	std::array<double, 2> barberStep_ {}; ///< each channel's signed step a sample: ±its ρ's cycles a sample
	/// THE FLANGER'S WINDOW PHASOR: each channel's e^(j2πψ), turned by its `barberRotStep_` before each sample and
	/// re-seeded from ψ every tick; its real part against the cube roots of unity is the taps' windows.
	std::array<std::complex<double>, 2> barberRot_ {};
	std::array<std::complex<double>, 2> barberRotStep_ {};
	double barberDir_ = 1.0;             ///< the direction the last tick ran: +1 Up (and Normal), −1 Down
	/// **THE FLANGER'S SWEEP-SWITCH CROSSFADE.** One line, so a switch between Normal and the barberpole, or Up and
	/// Down, moves the read — and a stepped read recirculates through the loop. So the outgoing read is frozen and
	/// faded out linearly against the incoming one over `kSweepFadeSamples`, both reads on the same line.
	static constexpr int kSweepFadeSamples = 4 * kControlTick;
	int fadeLeft_ = 0;                   ///< samples of the fade still to run
	bool fadeFromBarber_ = false;        ///< the outgoing read is the taps (else the plain read)

	// THE FLANGER.
	bool throughZero_ = false;
	int tzDry_ = 0; ///< through-zero's dry delay at this rate, whole samples
	bool plainUndelayedForTest_ = false; ///< `setPlainUndelayedForTest`
	std::array<FlangerChannel, 2> flangers_ {};
	int loopResets_ = 0;
};

} // namespace feathers::fx::phaser
